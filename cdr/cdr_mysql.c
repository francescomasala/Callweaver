/*
 * OpenPBX -- A telephony toolkit for Linux.
 *
 * MySQL CDR logger 
 * 
 * James Sharp <jsharp@psychoses.org>
 *
 * Modified August 2003
 * Tilghman Lesher <asterisk__cdr__cdr_mysql__200308@the-tilghman.com>
 *
 * Modified August 6, 2005
 * Joseph Benden <joe@thrallingpenguin.com>
 * Added mysql connection timeout parameter
 * Added an automatic reconnect as to not lose a cdr record
 * Cleaned up the original code to match the coding guidelines
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License.
 *
 */

#include <openpbx.h>

#include <sys/types.h>
#include <openpbx/config.h>
#include <openpbx/options.h>
#include <openpbx/channel.h>
#include <openpbx/cdr.h>
#include <openpbx/module.h>
#include <openpbx/logger.h>
#include <openpbx/cli.h>

#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include <mysql/mysql.h>
#include <mysql/errmsg.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

#define OPBX_MODULE "cdr_mysql"

#define DATE_FORMAT "%Y-%m-%d %T"

static char *desc = "MySQL CDR Backend";
static char *name = "mysql";
static char *config = "cdr_mysql.conf";
static char *hostname = NULL, *dbname = NULL, *dbuser = NULL, *password = NULL, *dbsock = NULL, *dbtable = NULL;
static int hostname_alloc = 0, dbname_alloc = 0, dbuser_alloc = 0, password_alloc = 0, dbsock_alloc = 0, dbtable_alloc = 0;
static int dbport = 0;
static int connected = 0;
static time_t connect_time = 0;
static int records = 0;
static int totalrecords = 0;
static int userfield = 0;
static unsigned int timeout = 0;

OPBX_MUTEX_DEFINE_STATIC(mysql_lock);

static MYSQL mysql;

static char cdr_mysql_status_help[] =
"Usage: cdr mysql status\n"
"       Shows current connection status for cdr_mysql\n";

static int handle_cdr_mysql_status(int fd, int argc, char *argv[])
{
	if (connected) {
		char status[256], status2[100] = "";
		int ctime = time(NULL) - connect_time;
		if (dbport)
			snprintf(status, 255, "Connected to %s@%s, port %d", dbname, hostname, dbport);
		else if (dbsock)
			snprintf(status, 255, "Connected to %s on socket file %s", dbname, dbsock);
		else
			snprintf(status, 255, "Connected to %s@%s", dbname, hostname);

		if (dbuser && *dbuser)
			snprintf(status2, 99, " with username %s", dbuser);
		if (dbtable && *dbtable)
			snprintf(status2, 99, " using table %s", dbtable);
		if (ctime > 31536000) {
			opbx_cli(fd, "%s%s for %d years, %d days, %d hours, %d minutes, %d seconds.\n", status, status2, ctime / 31536000, (ctime % 31536000) / 86400, (ctime % 86400) / 3600, (ctime % 3600) / 60, ctime % 60);
		} else if (ctime > 86400) {
			opbx_cli(fd, "%s%s for %d days, %d hours, %d minutes, %d seconds.\n", status, status2, ctime / 86400, (ctime % 86400) / 3600, (ctime % 3600) / 60, ctime % 60);
		} else if (ctime > 3600) {
			opbx_cli(fd, "%s%s for %d hours, %d minutes, %d seconds.\n", status, status2, ctime / 3600, (ctime % 3600) / 60, ctime % 60);
		} else if (ctime > 60) {
			opbx_cli(fd, "%s%s for %d minutes, %d seconds.\n", status, status2, ctime / 60, ctime % 60);
		} else {
			opbx_cli(fd, "%s%s for %d seconds.\n", status, status2, ctime);
		}
		if (records == totalrecords)
			opbx_cli(fd, "  Wrote %d records since last restart.\n", totalrecords);
		else
			opbx_cli(fd, "  Wrote %d records since last restart and %d records since last reconnect.\n", totalrecords, records);
		return RESULT_SUCCESS;
	} else {
		opbx_cli(fd, "Not currently connected to a MySQL server.\n");
		return RESULT_FAILURE;
	}
}

static struct opbx_cli_entry cdr_mysql_status_cli =
	{ { "cdr", "mysql", "status", NULL },
	handle_cdr_mysql_status, "Show connection status of cdr_mysql",
	cdr_mysql_status_help, NULL };

static int mysql_log(struct opbx_cdr *cdr)
{
	struct tm tm;
	struct timeval tv;
	struct opbx_module_user *u;
	char *userfielddata = NULL;
	char sqlcmd[2048], timestr[128];
	char *clid=NULL, *dcontext=NULL, *channel=NULL, *dstchannel=NULL, *lastapp=NULL, *lastdata=NULL;
	int retries = 5;
#ifdef MYSQL_LOGUNIQUEID
	char *uniqueid = NULL;
#endif

	opbx_mutex_lock(&mysql_lock);

	memset(sqlcmd, 0, 2048);

	localtime_r(&cdr->start.tv_sec, &tm);
	strftime(timestr, 128, DATE_FORMAT, &tm);

db_reconnect:
	if ((!connected) && (hostname || dbsock) && dbuser && password && dbname && dbtable ) {
		/* Attempt to connect */
		mysql_init(&mysql);
		/* Add option to quickly timeout the connection */
		if (timeout && mysql_options(&mysql, MYSQL_OPT_CONNECT_TIMEOUT, (char *)&timeout)!=0) {
			opbx_log(LOG_ERROR, "cdr_mysql: mysql_options returned (%d) %s\n", mysql_errno(&mysql), mysql_error(&mysql));
		}
		if (mysql_real_connect(&mysql, hostname, dbuser, password, dbname, dbport, dbsock, 0)) {
			connected = 1;
			connect_time = time(NULL);
			records = 0;
		} else {
			opbx_log(LOG_ERROR, "cdr_mysql: cannot connect to database server %s.\n", hostname);
			connected = 0;
		}
	} else {
		/* Long connection - ping the server */
		int error;
		if ((error = mysql_ping(&mysql))) {
			connected = 0;
			records = 0;
			switch (error) {
				case CR_SERVER_GONE_ERROR:
				case CR_SERVER_LOST:
					opbx_log(LOG_ERROR, "cdr_mysql: Server has gone away. Attempting to reconnect.\n");
					break;
				default:
					opbx_log(LOG_ERROR, "cdr_mysql: Unknown connection error: (%d) %s\n", mysql_errno(&mysql), mysql_error(&mysql));
			}
			retries--;
			if (retries)
				goto db_reconnect;
			else
				opbx_log(LOG_ERROR, "cdr_mysql: Retried to connect fives times, giving up.\n");
		}
	}

	/* Maximum space needed would be if all characters needed to be escaped, plus a trailing NULL */
	/* WARNING: This code previously used mysql_real_escape_string, but the use of said function
	   requires an active connection to a database.  If we are not connected, then this function
	    cannot be used.  This is a problem since we need to store off the SQL statement into our
	   spool file for later restoration.
	   So the question is, what's the best way to handle this?  This works for now.
	*/
	if ((clid = alloca(strlen(cdr->clid) * 2 + 1)) != NULL)
		mysql_escape_string(clid, cdr->clid, strlen(cdr->clid));
	if ((dcontext = alloca(strlen(cdr->dcontext) * 2 + 1)) != NULL)
		mysql_escape_string(dcontext, cdr->dcontext, strlen(cdr->dcontext));
	if ((channel = alloca(strlen(cdr->channel) * 2 + 1)) != NULL)
		mysql_escape_string(channel, cdr->channel, strlen(cdr->channel));
	if ((dstchannel = alloca(strlen(cdr->dstchannel) * 2 + 1)) != NULL)
		mysql_escape_string(dstchannel, cdr->dstchannel, strlen(cdr->dstchannel));
	if ((lastapp = alloca(strlen(cdr->lastapp) * 2 + 1)) != NULL)
		mysql_escape_string(lastapp, cdr->lastapp, strlen(cdr->lastapp));
	if ((lastdata = alloca(strlen(cdr->lastdata) * 2 + 1)) != NULL)
		mysql_escape_string(lastdata, cdr->lastdata, strlen(cdr->lastdata));
#ifdef MYSQL_LOGUNIQUEID
	if ((uniqueid = alloca(strlen(cdr->uniqueid) * 2 + 1)) != NULL)
		mysql_escape_string(uniqueid, cdr->uniqueid, strlen(cdr->uniqueid));
#endif
	if (userfield && ((userfielddata = alloca(strlen(cdr->userfield) * 2 + 1)) != NULL))
		mysql_escape_string(userfielddata, cdr->userfield, strlen(cdr->userfield));

	/* Check for all alloca failures above at once */
#ifdef MYSQL_LOGUNIQUEID
	if ((!clid) || (!dcontext) || (!channel) || (!dstchannel) || (!lastapp) || (!lastdata) || (!uniqueid)) {
#else
	if ((!clid) || (!dcontext) || (!channel) || (!dstchannel) || (!lastapp) || (!lastdata)) {
#endif
		opbx_log(LOG_ERROR, "cdr_mysql:  Out of memory error (insert fails)\n");
		opbx_mutex_unlock(&mysql_lock);
		return -1;
	}

	opbx_log(LOG_DEBUG, "cdr_mysql: inserting a CDR record.\n");

	if (userfield && userfielddata) {
#ifdef MYSQL_LOGUNIQUEID
		sprintf(sqlcmd, "INSERT INTO %s (calldate,clid,src,dst,dcontext,channel,dstchannel,lastapp,lastdata,duration,billsec,disposition,amaflags,accountcode,uniqueid,userfield) VALUES ('%s','%s','%s','%s','%s', '%s','%s','%s','%s',%i,%i,'%s',%i,'%s','%s','%s')", dbtable, timestr, clid, cdr->src, cdr->dst, dcontext, channel, dstchannel, lastapp, lastdata, cdr->duration, cdr->billsec, opbx_cdr_disp2str(cdr->disposition), cdr->amaflags, cdr->accountcode, uniqueid, userfielddata);
#else
		sprintf(sqlcmd, "INSERT INTO %s (calldate,clid,src,dst,dcontext,channel,dstchannel,lastapp,lastdata,duration,billsec,disposition,amaflags,accountcode,userfield) VALUES ('%s','%s','%s','%s','%s', '%s','%s','%s','%s',%i,%i,'%s',%i,'%s','%s')", dbtable, timestr, clid, cdr->src, cdr->dst, dcontext,channel, dstchannel, lastapp, lastdata, cdr->duration, cdr->billsec, opbx_cdr_disp2str(cdr->disposition), cdr->amaflags, cdr->accountcode, userfielddata);
#endif
	} else {
#ifdef MYSQL_LOGUNIQUEID
		sprintf(sqlcmd, "INSERT INTO %s (calldate,clid,src,dst,dcontext,channel,dstchannel,lastapp,lastdata,duration,billsec,disposition,amaflags,accountcode,uniqueid) VALUES ('%s','%s','%s','%s','%s', '%s','%s','%s','%s',%i,%i,'%s',%i,'%s','%s')", dbtable, timestr, clid, cdr->src, cdr->dst, dcontext,channel, dstchannel, lastapp, lastdata, cdr->duration, cdr->billsec, opbx_cdr_disp2str(cdr->disposition), cdr->amaflags, cdr->accountcode, uniqueid);
#else
		sprintf(sqlcmd, "INSERT INTO %s (calldate,clid,src,dst,dcontext,channel,dstchannel,lastapp,lastdata,duration,billsec,disposition,amaflags,accountcode) VALUES ('%s','%s','%s','%s','%s', '%s','%s','%s','%s',%i,%i,'%s',%i,'%s')", dbtable, timestr, clid, cdr->src, cdr->dst, dcontext, channel, dstchannel, lastapp, lastdata, cdr->duration, cdr->billsec, opbx_cdr_disp2str(cdr->disposition), cdr->amaflags, cdr->accountcode);
#endif
	}
	
	opbx_log(LOG_DEBUG, "cdr_mysql: SQL command as follows: %s\n", sqlcmd);
	
	if (connected) {
		if (mysql_real_query(&mysql, sqlcmd, strlen(sqlcmd))) {
			opbx_log(LOG_ERROR, "mysql_cdr: Failed to insert into database: (%d) %s", mysql_errno(&mysql), mysql_error(&mysql));
			mysql_close(&mysql);
			connected = 0;
		} else {
			records++;
			totalrecords++;
		}
	}
	opbx_mutex_unlock(&mysql_lock);
	return 0;
}

static int my_unload_module(void)
{ 
	opbx_cli_unregister(&cdr_mysql_status_cli);
	if (connected) {
		mysql_close(&mysql);
		connected = 0;
		records = 0;
	}
	if (hostname && hostname_alloc) {
		free(hostname);
		hostname = NULL;
		hostname_alloc = 0;
	}
	if (dbname && dbname_alloc) {
		free(dbname);
		dbname = NULL;
		dbname_alloc = 0;
	}
	if (dbuser && dbuser_alloc) {
		free(dbuser);
		dbuser = NULL;
		dbuser_alloc = 0;
	}
	if (dbsock && dbsock_alloc) {
		free(dbsock);
		dbsock = NULL;
		dbsock_alloc = 0;
	}
	if (dbtable && dbtable_alloc) {
		free(dbtable);
		dbtable = NULL;
		dbtable_alloc = 0;
	}
	if (password && password_alloc) {
		free(password);
		password = NULL;
		password_alloc = 0;
	}
	dbport = 0;
	opbx_cdr_unregister(name);
	return 0;
}

static int my_load_module(void)
{
	int res;
	struct opbx_config *cfg;
	struct opbx_variable *var;
	const char *tmp;

	cfg = opbx_config_load(config);
	if (!cfg) {
		opbx_log(LOG_WARNING, "Unable to load config for mysql CDR's: %s\n", config);
		return 0;
	}
	
	var = opbx_variable_browse(cfg, "global");
	if (!var) {
		/* nothing configured */
		return 0;
	}

	tmp = opbx_variable_retrieve(cfg, "global", "hostname");
	if (tmp) {
		hostname = malloc(strlen(tmp) + 1);
		if (hostname != NULL) {
			hostname_alloc = 1;
			strcpy(hostname, tmp);
		} else {
			opbx_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		opbx_log(LOG_WARNING, "MySQL server hostname not specified.  Assuming localhost\n");
		hostname = "localhost";
	}

	tmp = opbx_variable_retrieve(cfg, "global", "dbname");
	if (tmp) {
		dbname = malloc(strlen(tmp) + 1);
		if (dbname != NULL) {
			dbname_alloc = 1;
			strcpy(dbname, tmp);
		} else {
			opbx_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		opbx_log(LOG_WARNING, "MySQL database not specified.  Assuming openpbxcdrdb\n");
		dbname = "openpbxcdrdb";
	}

	tmp = opbx_variable_retrieve(cfg, "global", "user");
	if (tmp) {
		dbuser = malloc(strlen(tmp) + 1);
		if (dbuser != NULL) {
			dbuser_alloc = 1;
			strcpy(dbuser, tmp);
		} else {
			opbx_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		opbx_log(LOG_WARNING, "MySQL database user not specified.  Assuming root\n");
		dbuser = "root";
	}

	tmp = opbx_variable_retrieve(cfg, "global", "sock");
	if (tmp) {
		dbsock = malloc(strlen(tmp) + 1);
		if (dbsock != NULL) {
			dbsock_alloc = 1;
			strcpy(dbsock, tmp);
		} else {
			opbx_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		opbx_log(LOG_WARNING, "MySQL database sock file not specified.  Using default\n");
		dbsock = NULL;
	}

	tmp = opbx_variable_retrieve(cfg, "global", "table");
	if (tmp) {
		dbtable = malloc(strlen(tmp) + 1);
		if (dbtable != NULL) {
			dbtable_alloc = 1;
			strcpy(dbtable, tmp);
		} else {
			opbx_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		opbx_log(LOG_NOTICE, "MySQL database table not specified.  Assuming \"cdr\"\n");
		dbtable = "cdr";
	}

	tmp = opbx_variable_retrieve(cfg, "global", "password");
	if (tmp) {
		password = malloc(strlen(tmp) + 1);
		if (password != NULL) {
			password_alloc = 1;
			strcpy(password, tmp);
		} else {
			opbx_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		opbx_log(LOG_WARNING, "MySQL database password not specified.  Assuming blank\n");
		password = "";
	}

	tmp = opbx_variable_retrieve(cfg, "global", "port");
	if (tmp) {
		if (sscanf(tmp, "%d", &dbport) < 1) {
			opbx_log(LOG_WARNING, "Invalid MySQL port number.  Using default\n");
			dbport = 0;
		}
	}

	tmp = opbx_variable_retrieve(cfg, "global", "timeout");
	if (tmp) {
		if (sscanf(tmp,"%d", &timeout) < 1) {
			opbx_log(LOG_WARNING, "Invalid MySQL timeout number.  Using default\n");
			timeout = 0;
		}
	}
	
	tmp = opbx_variable_retrieve(cfg, "global", "userfield");
	if (tmp) {
		if (sscanf(tmp, "%d", &userfield) < 1) {
			opbx_log(LOG_WARNING, "Invalid MySQL configurtation file\n");
			userfield = 0;
		}
	}
	
	opbx_config_destroy(cfg);

	opbx_log(LOG_DEBUG, "cdr_mysql: got hostname of %s\n", hostname);
	opbx_log(LOG_DEBUG, "cdr_mysql: got port of %d\n", dbport);
	opbx_log(LOG_DEBUG, "cdr_mysql: got a timeout of %d\n", timeout);
	if (dbsock)
		opbx_log(LOG_DEBUG, "cdr_mysql: got sock file of %s\n", dbsock);
	opbx_log(LOG_DEBUG, "cdr_mysql: got user of %s\n", dbuser);
	opbx_log(LOG_DEBUG, "cdr_mysql: got dbname of %s\n", dbname);
	opbx_log(LOG_DEBUG, "cdr_mysql: got password of %s\n", password);

	mysql_init(&mysql);

	if (timeout && mysql_options(&mysql, MYSQL_OPT_CONNECT_TIMEOUT, (char *)&timeout)!=0) {
		opbx_log(LOG_ERROR, "cdr_mysql: mysql_options returned (%d) %s\n", mysql_errno(&mysql), mysql_error(&mysql));
	}

	if (!mysql_real_connect(&mysql, hostname, dbuser, password, dbname, dbport, dbsock, 0)) {
		opbx_log(LOG_ERROR, "Failed to connect to mysql database %s on %s.\n", dbname, hostname);
		connected = 0;
		records = 0;
	} else {
		opbx_log(LOG_DEBUG, "Successfully connected to MySQL database.\n");
		connected = 1;
		records = 0;
		connect_time = time(NULL);
	}

	res = opbx_cdr_register(name, desc, mysql_log);
	if (res) {
		opbx_log(LOG_ERROR, "Unable to register MySQL CDR handling\n");
	} else {
		res = opbx_cli_register(&cdr_mysql_status_cli);
	}

	return res;
}

int load_module(void)
{
	return my_load_module();
}

int unload_module(void)
{
	return my_unload_module();
}

int reload(void)
{
	int ret;

	opbx_mutex_lock(&mysql_lock);    
	my_unload_module();
	ret = my_load_module();
	opbx_mutex_unlock(&mysql_lock);

	return ret;
}

char *description(void)
{
	return "MySQL CDR Backend";
}

int usecount(void)
{
        int res;
        STANDARD_USECOUNT(res);
        return res;
}
