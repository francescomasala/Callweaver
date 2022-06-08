/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2005 OpenPBX.org.
 *
 * Marc Olivier Chouinard <mochouinard@openpbx.org>
 *
 * res_config_pgsql.c <PostgreSQL plugin for portable configuration engine >
 * Copyright (C) 2005 Business Technology Group (http://www.btg.co.nz)
 *   Danel Swarbrick <daniel@btg.co.nz>
 *
 * See http://www.openpbx.org for more information about
 * the OpenPBX project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/config.h"
#include "openpbx/module.h"
#include "openpbx/lock.h"
#include "openpbx/options.h"
#include "openpbx/utils.h"

#include <libpq-fe.h>

static char *tdesc = "PostgreSQL Configuration";

OPBX_MUTEX_DEFINE_STATIC(pgsql_lock);
#define RES_CONFIG_PGSQL_CONF "res_pgsql.conf"
static char conninfo[512];
static PGconn *conn = NULL;

static int parse_config(void);
static int pgsql_reconnect(const char *database);

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int parse_config(void)
{
	struct opbx_config *config;
	char *s;

	config = opbx_config_load(RES_CONFIG_PGSQL_CONF);

	if (config) {

		/* get the database host */
		s = opbx_variable_retrieve(config, "general", "dsn");
		if (s == NULL) {
			opbx_log(LOG_WARNING, "PgSQL RealTime: No DSN found, using 'dbname=openpbx user=openpbx'.\n");
			strncpy(conninfo, "dbname=openpbx user=openpbx", sizeof(conninfo));
		} else {
			strncpy(conninfo, s, sizeof(conninfo));
		}

	} else {
		opbx_log(LOG_WARNING, "PgSQL RealTime config file (%s) not found.\n", RES_CONFIG_PGSQL_CONF);
	}
	opbx_config_destroy(config);

	return 1;
}

static int pgsql_reconnect(const char *database)
{
	if (conn != NULL) {
		/* we may already be connected */
		if (PQstatus(conn) == CONNECTION_OK) {
			return 1;
		} else {
			opbx_log(LOG_NOTICE, "PgSQL RealTime: Existing database connection broken. Trying to reset.\n");

			/* try to reset the connection */
			PQreset(conn);

			/* check the connection status again */
			if (PQstatus(conn) == CONNECTION_OK) {
				opbx_log(LOG_NOTICE, "PgSQL RealTime: Existing database connection reset ok.\n");
				return 1;
			} else {
				/* still no luck, tear down the connection and we'll make a new connection */
				opbx_log(LOG_NOTICE, "PgSQL RealTime: Unable to reset existing database connection.\n");
				PQfinish(conn);
			}
		}
	}

	conn = PQconnectdb(conninfo);

	if (PQstatus(conn) == CONNECTION_OK) {
		opbx_log(LOG_NOTICE, "PgSQL RealTime: Successfully connected to PostgreSQL database.\n");
		return 1;
	} else {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Couldn't establish DB connection. Check debug.\n");
		opbx_log(LOG_ERROR, "PgSQL RealTime: reason %s\n", PQerrorMessage(conn));
	}		

	return -1;
}

static struct opbx_variable *realtime_pgsql(const char *database, const char *table, va_list ap)
{
	PGresult *res;
	long int row, rowcount = 0;
	int col, colcount = 0;
	char sql[1024];
	char *stringp;
	char *chunk;
	char *op;	
	const char *newparam, *newval;
	struct opbx_variable *var=NULL, *prev=NULL;

	if (!table) {
		opbx_log(LOG_WARNING, "PgSQL RealTime: No table specified.\n");
		return NULL;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval)  {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
		return NULL;
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	if (!strchr(newparam, ' ')) op = " ="; else op = "";

	snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s%s '%s'", table, newparam, op, newval);
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		if (!strchr(newparam, ' ')) op = " ="; else op = "";
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " AND %s%s '%s'", newparam, op, newval);
	}
	va_end(ap);

	opbx_log(LOG_DEBUG, "PgSQL RealTime: Retrieve SQL: %s\n", sql);

	/* SQL statement is ready, check database connection is still good */
	if (!pgsql_reconnect(database)) {
		return NULL;
	}

	opbx_mutex_lock(&pgsql_lock);
	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Failed to query database. Check debug for more info.\n");
		opbx_log(LOG_DEBUG, "PgSQL RealTime: Query: %s\n", sql);
		opbx_log(LOG_DEBUG, "PgSQL RealTime: Query failed because: %s\n", PQresultErrorMessage(res));
		PQclear(res);
		opbx_mutex_unlock(&pgsql_lock);
		return NULL;
	}

	rowcount = PQntuples(res);
	if (rowcount > 0) {
		colcount = PQnfields(res);

		for (row = 0; row < rowcount; row++) {
			for (col = 0; col < colcount; col++) {
				stringp = PQgetvalue(res, row, col);
				
				while (stringp) {
					chunk = strsep(&stringp, ";");
					if (chunk && !opbx_strlen_zero(opbx_strip(chunk))) {
						if (prev) {
							prev->next = opbx_variable_new(PQfname(res, col), chunk);
							if (prev->next) {
								prev = prev->next;
							}
						} else {
							prev = var = opbx_variable_new(PQfname(res, col), chunk);
						}
					}
				}
			}
		}
	} else {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Could not find any rows in table %s.\n", table);
	}

	PQclear(res);
	opbx_mutex_unlock(&pgsql_lock);

	return var;
}

static struct opbx_config *realtime_multi_pgsql(const char *database, const char *table, va_list ap)
{
	PGresult *res;
	long int row, rowcount = 0;
	int col, colcount = 0;
	char sql[1024];
	const char *initfield = NULL;
	char *stringp;
	char *chunk;
	char *op;
	const char *newparam, *newval;
	struct opbx_realloca ra;
	struct opbx_variable *var=NULL;
	struct opbx_config *cfg = NULL;
	struct opbx_category *cat = NULL;

	if(!table) {
		opbx_log(LOG_WARNING, "PgSQL RealTime: No table specified.\n");
		return NULL;
	}
	
	memset(&ra, 0, sizeof(ra));

	cfg = opbx_config_new();
	if (!cfg) {
		/* If I can't alloc memory at this point, why bother doing anything else? */
		opbx_log(LOG_WARNING, "Out of memory!\n");
		return NULL;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval)  {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
		return NULL;
	}

	initfield = opbx_strdupa(newparam);
	if (initfield && (op = strchr(initfield, ' '))) {
		*op = '\0';
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	if(!strchr(newparam, ' ')) op = " ="; else op = "";

	snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s%s '%s'", table, newparam, op, newval);
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		if (!strchr(newparam, ' ')) op = " ="; else op = "";
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " AND %s%s '%s'", newparam, op, newval);
	}

	if (initfield) {
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " ORDER BY %s", initfield);
	}

	va_end(ap);

	opbx_log(LOG_DEBUG, "PgSQL RealTime: Retrieve SQL: %s\n", sql);

	/* SQL statement is ready, check database connection is still good */
	if (!pgsql_reconnect(database)) {
		return NULL;
	}

	opbx_mutex_lock(&pgsql_lock);
	res = PQexec(conn, sql);
	
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Failed to query database. Check debug for more info.\n");
		opbx_log(LOG_DEBUG, "PgSQL RealTime: Query: %s\n", sql);
		opbx_log(LOG_DEBUG, "PgSQL RealTime: Query failed because: %s\n", PQresultErrorMessage(res));
		PQclear(res);
		opbx_mutex_unlock(&pgsql_lock);
		return NULL;
	}

	rowcount = PQntuples(res);
	if (rowcount > 0) {
		colcount = PQnfields(res);

		for (row = 0; row < rowcount; row++) {

			var = NULL;
			cat = opbx_category_new("");
			if (!cat) {
				opbx_log(LOG_WARNING, "Out of memory!\n");
				continue;
			}

			for (col = 0; col < colcount; col++) {
				stringp = PQgetvalue(res, row, col);
				while (stringp) {
					chunk = strsep(&stringp, ";");
					if (chunk && !opbx_strlen_zero(opbx_strip(chunk))) {
						if (initfield && !strcmp(initfield, PQfname(res, col))) {
							opbx_category_rename(cat, chunk);
						}
						var = opbx_variable_new(PQfname(res, col), chunk);
						opbx_variable_append(cat, var);
					}
				}
			}
			opbx_category_append(cfg, cat);
		}
	} else {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Could not find any rows in table %s.\n", table);
	}

	PQclear(res);
	opbx_mutex_unlock(&pgsql_lock);

	return cfg;
}

static int update_pgsql(const char *database, const char *table, const char *keyfield, const char *lookup, va_list ap)
{
	PGresult *res;
	long int rowcount = 0;
	char sql[1024];
	const char *newparam, *newval;

	if (!table) {
		opbx_log(LOG_WARNING, "PgSQL RealTime: No table specified.\n");
		return -1;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval)  {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
		return -1;
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	snprintf(sql, sizeof(sql), "UPDATE %s SET %s = '%s'", table, newparam, newval);
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), ", %s = '%s'", newparam, newval);
	}
	va_end(ap);
	snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " WHERE %s = '%s'", keyfield, lookup);

	opbx_log(LOG_DEBUG, "PgSQL RealTime: Update SQL: %s\n", sql);

	/* SQL statement is ready, check database connection is still good */
	if (!pgsql_reconnect(database)) {
		return -1;
	}

	opbx_mutex_lock(&pgsql_lock);
	res = PQexec(conn, sql);
	
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Failed to query database. Check debug for more info.\n");
		opbx_log(LOG_DEBUG, "PgSQL RealTime: Query: %s\n", sql);
		opbx_log(LOG_DEBUG, "PgSQL RealTime: Query failed because: %s\n", PQresultErrorMessage(res));
		PQclear(res);
		opbx_mutex_unlock(&pgsql_lock);
		return -1;
	}

	rowcount = atol(PQcmdTuples(res));

	PQclear(res);
	opbx_mutex_unlock(&pgsql_lock);

	opbx_log(LOG_DEBUG, "PgSQL RealTime: Updated %lu rows on table: %s\n", rowcount, table);

	if (rowcount >= 0)
		return (int)rowcount;
	else
		return -1;
}

static struct opbx_config *config_pgsql(const char *database, const char *table, const char *file, struct opbx_config *cfg)
{
	PGresult *res;
	long int row, rowcount = 0;
	int colcount = 0;
	char sql[1024];
	char last[128] = "";
	struct opbx_category *cur_cat = NULL;
	struct opbx_variable *new_v;
	int last_cat_metric = 0;

	if (!file || !strcmp(file, RES_CONFIG_PGSQL_CONF)) {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Cannot configure myself.\n");
		return NULL;		
	}

	snprintf(sql, sizeof(sql), "SELECT category, var_name, var_val, cat_metric FROM %s WHERE filename='%s' and commented=0 ORDER BY filename, cat_metric DESC, var_metric ASC, category, var_name, var_val, id", table, file);

	opbx_log(LOG_NOTICE, "PgSQL RealTime: Static SQL: %s\n", sql);

	/* SQL statement is ready, check database connection is still good */
	if (!pgsql_reconnect(database)) {
		return NULL;
	}

	opbx_mutex_lock(&pgsql_lock);
	res = PQexec(conn, sql);
	
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Failed to query database. Check debug for more info.\n");
		opbx_log(LOG_DEBUG, "PgSQL RealTime: Query: %s\n", sql);
		opbx_log(LOG_DEBUG, "PgSQL RealTime: Query failed because: %s\n", PQresultErrorMessage(res));
		PQclear(res);
		opbx_mutex_unlock(&pgsql_lock);
		return NULL;
	}

	rowcount = PQntuples(res);
	if (rowcount > 0) {

		opbx_log(LOG_NOTICE, "PgSQL RealTime: Found %lu rows.\n", rowcount);
		colcount = PQnfields(res);
		for (row = 0; row < rowcount; row++) {
			if (!strcmp(PQgetvalue(res, row, 1), "#include")) {
				if (!opbx_config_internal_load(PQgetvalue(res, row, 2), cfg)) {
					PQclear(res);
					opbx_mutex_unlock(&pgsql_lock);
					return NULL;
				}
				continue;
			}

			if (strcmp(last, PQgetvalue(res, row, 0)) || last_cat_metric != atoi(PQgetvalue(res, row, 3))) {
				cur_cat = opbx_category_new(PQgetvalue(res, row, 0));
				if (!cur_cat) {
					opbx_log(LOG_WARNING, "Out of memory!\n");
					break;
				}
				strcpy(last, PQgetvalue(res, row, 0));
				last_cat_metric = atoi(PQgetvalue(res, row, 3));
				opbx_category_append(cfg, cur_cat);
			}
			new_v = opbx_variable_new(PQgetvalue(res, row, 1), PQgetvalue(res, row, 2));
			opbx_variable_append(cur_cat, new_v);

		}

	} else {
		opbx_log(LOG_WARNING, "PgSQL RealTime: Could not find config '%s' in database.\n", file);
	}

	PQclear(res);
	opbx_mutex_unlock(&pgsql_lock);

	return cfg;
}

static struct opbx_config_engine pgsql_engine = {
	.name = "pgsql",
	.load_func = config_pgsql,
	.realtime_func = realtime_pgsql,
	.realtime_multi_func = realtime_multi_pgsql,
	.update_func = update_pgsql
};

int unload_module(void)
{
	/* acquire control before doing anything to the module itself */
	opbx_mutex_lock(&pgsql_lock);

	PQfinish(conn);
	opbx_config_engine_deregister(&pgsql_engine);
	if (option_verbose) {
		opbx_verbose("PgSQL RealTime unloaded.\n");
	}

	STANDARD_HANGUP_LOCALUSERS;

	/* Unlock so something else can destroy the lock */
	opbx_mutex_unlock(&pgsql_lock);

	return 0;
}

int load_module(void)
{
	parse_config();

	pgsql_reconnect(NULL);

	opbx_config_engine_register(&pgsql_engine);
	if (option_verbose) {
		opbx_verbose("PgSQL RealTime driver loaded.\n");
	}

	return 0;
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	/* never unload a config module */
	return 1;
}

