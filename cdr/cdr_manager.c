/*
 * CallWeaver.org -- An open source telephony toolkit.
 *
 * Copyright (C) 2004 - 2005
 *
 * See http://www.callweaver.org for more information about
 * the CallWeaver.org project. Please do not directly contact
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
 * \brief CallWeaver.org Call Manager CDR records.
 * 
 * See also
 * \arg \ref OpbxCDR
 * \arg \ref OpbxOMI
 * \arg \ref Config_omi
 * \ingroup cdr_drivers
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <sys/types.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/cdr/cdr_manager.c $", "$Revision: 4723 $")

#include "callweaver/channel.h"
#include "callweaver/cdr.h"
#include "callweaver/module.h"
#include "callweaver/logger.h"
#include "callweaver/utils.h"
#include "callweaver/manager.h"
#include "callweaver/config.h"

#define DATE_FORMAT 	"%Y-%m-%d %T"
#define CONF_FILE	"cdr_manager.conf"

static char *desc = "CallWeaver.org Call Manager CDR Backend";
static char *name = "cdr_manager";

static int enablecdr = 0;

static void loadconfigurationfile(void)
{
	char *cat;
	struct cw_config *cfg;
	struct cw_variable *v;
	
	cfg = cw_config_load(CONF_FILE);
	if (!cfg) {
		/* Standard configuration */
		enablecdr = 0;
		return;
	}
	
	cat = cw_category_browse(cfg, NULL);
	while (cat) {
		if (!strcasecmp(cat, "general")) {
			v = cw_variable_browse(cfg, cat);
			while (v) {
				if (!strcasecmp(v->name, "enabled")) {
					enablecdr = cw_true(v->value);
				}
				
				v = v->next;
			}
		}
	
		/* Next category */
		cat = cw_category_browse(cfg, cat);
	}
	
	cw_config_destroy(cfg);
}

static int manager_log(struct cw_cdr *cdr)
{
	time_t t;
	struct tm timeresult;
	char strStartTime[80] = "";
	char strAnswerTime[80] = "";
	char strEndTime[80] = "";
	
	if (!enablecdr)
		return 0;

	t = cdr->start.tv_sec;
	localtime_r(&t, &timeresult);
	strftime(strStartTime, sizeof(strStartTime), DATE_FORMAT, &timeresult);
	
	if (cdr->answer.tv_sec)	{
    		t = cdr->answer.tv_sec;
    		localtime_r(&t, &timeresult);
		strftime(strAnswerTime, sizeof(strAnswerTime), DATE_FORMAT, &timeresult);
	}

	t = cdr->end.tv_sec;
	localtime_r(&t, &timeresult);
	strftime(strEndTime, sizeof(strEndTime), DATE_FORMAT, &timeresult);

	manager_event(EVENT_FLAG_CALL, "Cdr",
	    "AccountCode: %s\r\n"
	    "Source: %s\r\n"
	    "Destination: %s\r\n"
	    "DestinationContext: %s\r\n"
	    "CallerID: %s\r\n"
	    "Channel: %s\r\n"
	    "DestinationChannel: %s\r\n"
	    "LastApplication: %s\r\n"
	    "LastData: %s\r\n"
	    "StartTime: %s\r\n"
	    "AnswerTime: %s\r\n"
	    "EndTime: %s\r\n"
	    "Duration: %d\r\n"
	    "BillableSeconds: %d\r\n"
	    "Disposition: %s\r\n"
	    "AMAFlags: %s\r\n"
	    "UniqueID: %s\r\n"
	    "UserField: %s\r\n",
	    cdr->accountcode, cdr->src, cdr->dst, cdr->dcontext, cdr->clid, cdr->channel,
	    cdr->dstchannel, cdr->lastapp, cdr->lastdata, strStartTime, strAnswerTime, strEndTime,
	    cdr->duration, cdr->billsec, cw_cdr_disp2str(cdr->disposition), 
	    cw_cdr_flags2str(cdr->amaflags), cdr->uniqueid, cdr->userfield);
	    	
	return 0;
}

char *description(void)
{
	return desc;
}

int unload_module(void)
{
	cw_cdr_unregister(name);
	return 0;
}

int load_module(void)
{
	int res;

	/* Configuration file */
	loadconfigurationfile();
	
	res = cw_cdr_register(name, desc, manager_log);
	if (res) {
		cw_log(LOG_ERROR, "Unable to register CallWeaver.org Call Manager CDR handling\n");
	}
	
	return res;
}

int reload(void)
{
	loadconfigurationfile();
	return 0;
}

int usecount(void)
{
	return 0;
}
