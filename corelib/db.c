/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * SQLite DB Functionality
 * 
 * Copyright (C) 2004, Anthony Minessale II
 *
 * Anthony Minessale II <anthmct@cylynx.com>
 *
 * Original Function Prototypes
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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

/*! \file
 *
 * \brief SQLite Management
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/channel.h"
#include "openpbx/file.h"
#include "openpbx/app.h"
#include "openpbx/dsp.h"
#include "openpbx/logger.h"
#include "openpbx/options.h"
#include "openpbx/opbxdb.h"
#include "openpbx/cli.h"
#include "openpbx/utils.h"
#include "openpbx/lock.h"
#include "openpbx/manager.h"
#include "sqlite3.h"

OPBX_MUTEX_DEFINE_STATIC(dblock);

static char *create_odb_sql = 
"create table odb (\n"
"						 family varchar(255),\n"
"						 keys varchar(255) not null,\n"
"						 value varchar(255) not null\n"
"						 );\n\n"
"CREATE INDEX odb_index_1 ON odb(family);\n"
"CREATE INDEX odb_index_2 ON odb(keys);\n"
"CREATE INDEX odb_index_3 ON odb(value);\n";

static int loaded = 0;

static struct {
	char *dbdir;
	char *dbfile;
	char *dbname;
	char *tablename;
} globals;

struct opbx_db_data {
	char *data;
	int datalen;
	int rownum;
};

static int dbinit(void);
static void sqlite_pick_path(char *dbname, char *buf, size_t size);
static sqlite3 *sqlite_open_db(char *filename);
static void sqlite_check_table_exists(char *dbfile, char *test_sql, char *create_sql);
static int get_callback(void *pArg, int argc, char **argv, char **columnNames);
static int tree_callback(void *pArg, int argc, char **argv, char **columnNames);
static int show_callback(void *pArg, int argc, char **argv, char **columnNames);
static int database_show(int fd, int argc, char *argv[]);
static int database_put(int fd, int argc, char *argv[]);
static int database_get(int fd, int argc, char *argv[]);
static int database_del(int fd, int argc, char *argv[]);
static int database_deltree(int fd, int argc, char *argv[]);


static int sanity_check(void)
{
	if (!loaded) {
		opbx_log(LOG_ERROR, "NICE RACE CONDITION WE HAVE HERE! PUTTING THE CART BEFORE THE HORSE EH? >=0\n");
		dbinit();
	}
	return 0;
}

static int dbinit(void)
{
	char *sql;

	opbx_mutex_lock(&dblock);
 	globals.dbdir = opbx_config_OPBX_DB_DIR;
	globals.dbfile = opbx_config_OPBX_DB;
	globals.tablename = "odb";
	
	
	if ((sql = sqlite3_mprintf("select count(*) from %q limit 1", globals.tablename))) {
		sqlite_check_table_exists(globals.dbfile, sql, create_odb_sql);
		sqlite3_free(sql);
		sql = NULL;
		loaded = 1;
	}

	opbx_mutex_unlock(&dblock);
	return loaded ? 0 : -1;
}


static void sqlite_pick_path(char *dbname, char *buf, size_t size) 
{

	memset(buf, 0, size);
	if (strchr(dbname, '/')) {
		strncpy(buf, dbname, size);
	} else {
		snprintf(buf, size, "%s/%s", globals.dbdir, dbname);
	}
}

static sqlite3 *sqlite_open_db(char *filename) 
{
	sqlite3 *db;
	char path[1024];
	
	sqlite_pick_path(filename, path, sizeof(path));
	if (sqlite3_open(path, &db)) {
		opbx_log(LOG_WARNING, "SQL ERR [%s]\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		db=NULL;
	}
	return db;
}


static void sqlite_check_table_exists(char *dbfile, char *test_sql, char *create_sql) 
{
	sqlite3 *db;
	char *errmsg;


	if ((db = sqlite_open_db(dbfile))) {
		if (test_sql) {
			sqlite3_exec(
						 db,
						 test_sql,
						 NULL,
						 NULL,
						 &errmsg
						 );

			if (errmsg) {
				opbx_log(LOG_WARNING,"SQL ERR [%s]\n[%s]\nAuto Repairing!\n",errmsg,test_sql);
				sqlite3_free(errmsg);
				errmsg = NULL;
				sqlite3_exec(
							 db,
							 create_sql,
							 NULL,
							 NULL,
							 &errmsg
							 );
				if (errmsg) {
					opbx_log(LOG_WARNING,"SQL ERR [%s]\n[%s]\n",errmsg,create_sql);
					sqlite3_free(errmsg);
					errmsg = NULL;
				}
			}
			sqlite3_close(db);
		}
	}

}

int opbx_db_put(const char *family, const char *keys, char *value)
{
	char *sql;
	char *zErr = 0;
	int res = 0;
	sqlite3 *db;

	sanity_check();
	if (!(db = sqlite_open_db(globals.dbfile))) {
		return -1;
	}


	if (!family || opbx_strlen_zero(family)) {
		family = "_undef_";
	}

	opbx_db_del(family, keys);
	if ((sql = sqlite3_mprintf("insert into %q values('%q','%q','%q')", globals.tablename, family, keys, value))) {
		opbx_log(LOG_DEBUG, "SQL [%s]\n", sql);

		res = sqlite3_exec(db,
						   sql,
						   NULL,
						   NULL,
						   &zErr
						   );

		if (zErr) {
			opbx_log(LOG_ERROR, "SQL ERR [%s] [%s]\n", sql, zErr);
			res = -1;
		} else {
			res = 0;
		}
	} else {
		opbx_log(LOG_ERROR, "Memory Error!\n");
	}

	if (sql) {
		sqlite3_free(sql);
		sql = NULL;
	}
	sqlite3_close(db);
	return res;
}

static int get_callback(void *pArg, int argc, char **argv, char **columnNames) 
{
	struct opbx_db_data *result = pArg;

	opbx_copy_string(result->data, argv[0], result->datalen);
	result->rownum++;
	return 0;
}

int opbx_db_get(const char *family, const char *keys, char *value, int valuelen)
{

	char *sql;
	char *zErr = 0;
	int res = 0;
	struct opbx_db_data result;
	sqlite3 *db;

	sanity_check();
	if (!(db = sqlite_open_db(globals.dbfile))) {
		return -1;
	}

	if (!family || opbx_strlen_zero(family)) {
		family = "_undef_";
	}

	result.data = value;
	result.datalen = valuelen;
	result.rownum = 0;

	if ((sql = sqlite3_mprintf("select value from %q where family='%q' and keys='%q'", globals.tablename, family, keys))) {
		opbx_log(LOG_DEBUG, "SQL [%s]\n", sql);
		res = sqlite3_exec(db,
						   sql,
						   get_callback,
						   &result,
						   &zErr
						   );
		
		if (zErr) {
			opbx_log(LOG_ERROR, "SQL ERR [%s] [%s]\n", sql, zErr);
			res = -1;
		} else {
			if (result.rownum)
				res = 0;
			else
				res = -1;
		}
	} else {
		opbx_log(LOG_ERROR, "Memory Error!\n");
	}

	if (sql) {
		sqlite3_free(sql);
		sql = NULL;
	}

	sqlite3_close(db);
	return res;
}

static int opbx_db_del_main(const char *family, const char *keys, int like)
{
	char *sql;
	char *zErr = 0;
	int res = 0;
	sqlite3 *db;
	char *op = "=";
	char *pct = "";


	sanity_check();
	if (!(db = sqlite_open_db(globals.dbfile))) {
		return -1;
	}

	if (!family || opbx_strlen_zero(family)) {
		family = "_undef_";
	}

	if (like) {
		op = "like";
		pct = "%";
	}

	if (family && keys) {
		sql = sqlite3_mprintf("delete from %q where family %s '%q%s' and keys %s '%q%s'", globals.tablename, op, family, pct, op, keys, pct);
	} else if (family) {
		sql = sqlite3_mprintf("delete from %q where family %s '%q%s'", globals.tablename, op, family, pct);
	} else {
		sql = sqlite3_mprintf("delete from %q", globals.tablename);
	}


	if (sql) {
		opbx_log(LOG_DEBUG, "SQL [%s]\n", sql);
		res = sqlite3_exec(db,
						   sql,
						   NULL,
						   NULL,
						   &zErr
						   );

		if (zErr) {
			opbx_log(LOG_ERROR, "SQL ERR [%s] [%s]\n", sql, zErr);
			res = -1;
		} else {
			if (!sqlite3_changes(db))
				res = -1;
			else
				res = 0;
		}
	} else {
		opbx_log(LOG_ERROR, "Memory Error!\n");
	}

	if (sql) {
		sqlite3_free(sql);
		sql = NULL;
	}

	sqlite3_close(db);
	return res;
}

int opbx_db_del(const char *family, const char *keys)
{
	return opbx_db_del_main(family, keys, 0);
}

int opbx_db_deltree(const char *family, const char *keytree)
{
	return opbx_db_del_main(family, keytree, 1);
}

static int tree_callback(void *pArg, int argc, char **argv, char **columnNames) 
{
	int x = 0;
	char *keys, *values;
	struct opbx_db_entry **treeptr = pArg,
		*cur = NULL,
		*ret = NULL;

	for(x=0; x < argc; x++) {
		keys = argv[0];
		values = argv[1];

		cur = malloc(sizeof(struct opbx_db_entry) + strlen(keys) + strlen(values) + 2);
		if (cur) {
			cur->next = NULL;
			cur->key = cur->data + strlen(values) + 1;
			strcpy(cur->data, values);
			strcpy(cur->key, keys);
			if (*treeptr) {
				cur->next = *treeptr;
			}
			ret = cur;
		}
	}

	*treeptr = ret;
	return 0;
}

struct opbx_db_entry *opbx_db_gettree(const char *family, const char *keytree)
{
	char *sql;
	char *zErr = 0;
	int res = 0;
	struct opbx_db_entry *tree = NULL;
	sqlite3 *db;

	sanity_check();
	if (!(db = sqlite_open_db(globals.dbfile))) {
		return NULL;
	}

	if (!family || opbx_strlen_zero(family)) {
		family = "_undef_";
	}

	if (family && keytree && !opbx_strlen_zero(keytree)) {
		sql = sqlite3_mprintf("select keys,value from %q where family='%q' and keys like '%q%%'", globals.tablename, family, keytree);
	} else if(family) {
		sql = sqlite3_mprintf("select keys,value from %q where family='%q'", globals.tablename, family);
	} else {
		opbx_log(LOG_ERROR, "No parameters supplied.\n");
		return NULL;
	}

	if (sql) {
		opbx_log(LOG_DEBUG, "SQL [%s]\n", sql);
		res = sqlite3_exec(db,
						   sql,
						   tree_callback,
						   &tree,
						   &zErr
						   );
		
		if (zErr) {
			opbx_log(LOG_ERROR, "SQL ERR [%s] [%s]\n", sql, zErr);
			res = -1;
		} else {
			res = 0;
		}
	} else {
		opbx_log(LOG_ERROR, "Memory Error!\n");
	}

	if (sql) {
		sqlite3_free(sql);
		sql = NULL;
	}

	sqlite3_close(db);
	return tree;

}

void opbx_db_freetree(struct opbx_db_entry *dbe)
{
	struct opbx_db_entry *last;

	while (dbe) {
		last = dbe;
		dbe = dbe->next;
		free(last);
	}
}


static int show_callback(void *pArg, int argc, char **argv, char **columnNames) 
{
	int *fdp = pArg;
	int fd = *fdp;

	opbx_cli(fd, "/%s/%-50s: %-25s\n", argv[0], argv[1], argv[2]);

	return 0;
}

static int database_show(int fd, int argc, char *argv[])
{
	char *prefix, *family;
	char *sql;
	char *zErr = 0;
	int res = 0;
	sqlite3 *db;

	sanity_check();
	if (!(db = sqlite_open_db(globals.dbfile))) {
		return -1;
	}

	if (argc == 4) {
		/* Family and key tree */
		prefix = argv[3];
		family = argv[2];
	} else if (argc == 3) {
		/* Family only */
		family = argv[2];
		prefix = NULL;
	} else if (argc == 2) {
		/* Neither */
		prefix = family = NULL;
	} else {
		return RESULT_SHOWUSAGE;
	}

	if (family && prefix) {
		sql = sqlite3_mprintf("select * from %q where family='%q' and keys='%q'", globals.tablename, family, prefix);
	} else if (family) {
		sql = sqlite3_mprintf("select * from %q where family='%q'", globals.tablename, family);
	} else {
		sql = sqlite3_mprintf("select * from %q", globals.tablename);
	}

	if (sql) {
		opbx_log(LOG_DEBUG, "SQL [%s]\n", sql);
		res = sqlite3_exec(db,
						   sql,
						   show_callback,
						   &fd,
						   &zErr
						   );
		
		if (zErr) {
			opbx_log(LOG_ERROR, "SQL ERR [%s] [%s]\n", sql, zErr);
			res = -1;
		} else {
			res = 0;
		}
	} else {
		opbx_log(LOG_ERROR, "Memory Error!\n");
	}

	if (sql) {
		sqlite3_free(sql);
		sql = NULL;
	}

	sqlite3_close(db);
	return RESULT_SUCCESS;	
}



static int database_put(int fd, int argc, char *argv[])
{
	int res;
	if (argc != 5)
		return RESULT_SHOWUSAGE;
	res = opbx_db_put(argv[2], argv[3], argv[4]);
	if (res)  {
		opbx_cli(fd, "Failed to update entry\n");
	} else {
		opbx_cli(fd, "Updated database successfully\n");
	}
	return RESULT_SUCCESS;
}

static int database_get(int fd, int argc, char *argv[])
{
	int res;
	char tmp[256];
	if (argc != 4)
		return RESULT_SHOWUSAGE;
	res = opbx_db_get(argv[2], argv[3], tmp, sizeof(tmp));
	if (res) {
		opbx_cli(fd, "Database entry not found.\n");
	} else {
		opbx_cli(fd, "Value: %s\n", tmp);
	}
	return RESULT_SUCCESS;
}

static int database_del(int fd, int argc, char *argv[])
{
	int res;
	if (argc != 4)
		return RESULT_SHOWUSAGE;
	res = opbx_db_del(argv[2], argv[3]);
	if (res) {
		opbx_cli(fd, "Database entry does not exist.\n");
	} else {
		opbx_cli(fd, "Database entry removed.\n");
	}
	return RESULT_SUCCESS;
}

static int database_deltree(int fd, int argc, char *argv[])
{
	int res;
	if ((argc < 3) || (argc > 4))
		return RESULT_SHOWUSAGE;
	if (argc == 4) {
		res = opbx_db_deltree(argv[2], argv[3]);
	} else {
		res = opbx_db_deltree(argv[2], NULL);
	}
	if (res) {
		opbx_cli(fd, "Database entries do not exist.\n");
	} else {
		opbx_cli(fd, "Database entries removed.\n");
	}
	return RESULT_SUCCESS;
}


static char database_show_usage[] =
"Usage: database show [family [keytree]]\n"
"       Shows OpenPBX database contents, optionally restricted\n"
"to a given family, or family and keytree.\n";

static char database_put_usage[] =
"Usage: database put <family> <key> <value>\n"
"       Adds or updates an entry in the OpenPBX database for\n"
"a given family, key, and value.\n";

static char database_get_usage[] =
"Usage: database get <family> <key>\n"
"       Retrieves an entry in the OpenPBX database for a given\n"
"family and key.\n";

static char database_del_usage[] =
"Usage: database del <family> <key>\n"
"       Deletes an entry in the OpenPBX database for a given\n"
"family and key.\n";

static char database_deltree_usage[] =
"Usage: database deltree <family> [keytree]\n"
"       Deletes a family or specific keytree within a family\n"
"in the OpenPBX database.\n";

struct opbx_cli_entry cli_database_show =
{ { "database", "show", NULL }, database_show, "Shows database contents", database_show_usage };

struct opbx_cli_entry cli_database_get =
{ { "database", "get", NULL }, database_get, "Gets database value", database_get_usage };

struct opbx_cli_entry cli_database_put =
{ { "database", "put", NULL }, database_put, "Adds/updates database value", database_put_usage };

struct opbx_cli_entry cli_database_del =
{ { "database", "del", NULL }, database_del, "Removes database key/value", database_del_usage };

struct opbx_cli_entry cli_database_deltree =
{ { "database", "deltree", NULL }, database_deltree, "Removes database keytree/values", database_deltree_usage };

int opbxdb_init(void)
{
	int res = dbinit();
	if (res == 0) {
		opbx_cli_register(&cli_database_show);
		opbx_cli_register(&cli_database_get);
		opbx_cli_register(&cli_database_put);
		opbx_cli_register(&cli_database_del);
		opbx_cli_register(&cli_database_deltree);
	}
	return res;
}

