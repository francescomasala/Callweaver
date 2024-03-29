/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.callweaver.org for more information about
 * the CallWeaver project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*
 *
 * Phone number checking and manipulation support 
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <spandsp.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/corelib/phone_no_utils.c $", "$Revision: 4723 $")

#include "callweaver/ulaw.h"
#include "callweaver/alaw.h"
#include "callweaver/frame.h"
#include "callweaver/channel.h"
#include "callweaver/phone_no_utils.h"
#include "callweaver/logger.h"
#include "callweaver/utils.h"

static struct {
	int val;
	char *name;
	char *description;
} pres_types[] = {
	{  CW_PRES_ALLOWED_USER_NUMBER_NOT_SCREENED, "allowed_not_screened", "Presentation Allowed, Not Screened"},
	{  CW_PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN, "allowed_passed_screen", "Presentation Allowed, Passed Screen"},
	{  CW_PRES_ALLOWED_USER_NUMBER_FAILED_SCREEN, "allowed_failed_screen", "Presentation Allowed, Failed Screen"},
	{  CW_PRES_ALLOWED_NETWORK_NUMBER, "allowed", "Presentation Allowed, Network Number"},
	{  CW_PRES_PROHIB_USER_NUMBER_NOT_SCREENED, "prohib_not_screened", "Presentation Prohibited, Not Screened"},
	{  CW_PRES_PROHIB_USER_NUMBER_PASSED_SCREEN, "prohib_passed_screen", "Presentation Prohibited, Passed Screen"},
	{  CW_PRES_PROHIB_USER_NUMBER_FAILED_SCREEN, "prohib_failed_screen", "Presentation Prohibited, Failed Screen"},
	{  CW_PRES_PROHIB_NETWORK_NUMBER, "prohib", "Presentation Prohibited, Network Number"},
	{  CW_PRES_NUMBER_NOT_AVAILABLE, "unavailable", "Number Unavailable"},
};

int cw_parse_caller_presentation(const char *data)
{
	int i;

	for (i = 0; i < ((sizeof(pres_types) / sizeof(pres_types[0]))); i++) {
		if (!strcasecmp(pres_types[i].name, data))
			return pres_types[i].val;
	}

	return -1;
}

const char *cw_describe_caller_presentation(int data)
{
	int i;

	for (i = 0; i < ((sizeof(pres_types) / sizeof(pres_types[0]))); i++) {
		if (pres_types[i].val == data)
			return pres_types[i].description;
	}

	return "unknown";
}

void cw_shrink_phone_number(char *n)
{
	int x,y=0;
	int bracketed=0;
	for (x=0;n[x];x++) {
		switch(n[x]) {
		case '[':
			bracketed++;
			n[y++] = n[x];
			break;
		case ']':
			bracketed--;
			n[y++] = n[x];
			break;
		case '-':
			if (bracketed)
				n[y++] = n[x];
			break;
		case '.':
			if (!n[x+1])
				n[y++] = n[x];
			break;
		default:
			if (!strchr("( )", n[x]))
				n[y++] = n[x];
		}
	}
	n[y] = '\0';
}

int cw_isphonenumber(const char *n)
{
	int x;
	if (!n || cw_strlen_zero(n))
		return 0;
	for (x=0;n[x];x++)
		if (!strchr("0123456789*#+", n[x]))
			return 0;
	return 1;
}

char *cw_callerid_merge(char *buf, int bufsiz, const char *name, const char *num, const char *unknown)
{
	if (!unknown)
		unknown = "<unknown>";
	if (name && num)
		snprintf(buf, bufsiz, "\"%s\" <%s>", name, num);
	else if (name) 
		cw_copy_string(buf, name, bufsiz);
	else if (num)
		cw_copy_string(buf, num, bufsiz);
	else
		cw_copy_string(buf, unknown, bufsiz);
	return buf;
}

int cw_callerid_split(const char *buf, char *name, int namelen, char *num, int numlen)
{
	char *tmp;
	char *l = NULL;
    char *n = NULL;
    
	tmp = cw_strdupa(buf);
	cw_callerid_parse(tmp, &n, &l);
	if (n)
		cw_copy_string(name, n, namelen);
	else
		name[0] = '\0';
	if (l) {
		cw_shrink_phone_number(l);
		cw_copy_string(num, l, numlen);
	} else
		num[0] = '\0';
	return 0;
}

int cw_callerid_parse(char *instr, char **name, char **location)
{
	char *ns, *ne;
	char *ls, *le;
	char tmp[256];
	/* Try for "name" <location> format or 
	   name <location> format */
	if ((ls = strchr(instr, '<')) && (le = strchr(ls, '>'))) {
		/* Found the location */
		*le = '\0';
		*ls = '\0';
		*location = ls + 1;
		if ((ns = strchr(instr, '\"')) && (ne = strchr(ns + 1, '\"'))) {
			/* Get name out of quotes */
			*ns = '\0';
			*ne = '\0';
			*name = ns + 1;
			return 0;
		} else {
			/* Just trim off any trailing spaces */
			*name = instr;
			while(!cw_strlen_zero(instr) && (instr[strlen(instr) - 1] < 33))
				instr[strlen(instr) - 1] = '\0';
			/* And leading spaces */
			while(**name && (**name < 33))
				(*name)++;
			return 0;
		}
	} else {
		cw_copy_string(tmp, instr, sizeof(tmp));
		cw_shrink_phone_number(tmp);
		if (cw_isphonenumber(tmp)) {
			/* Assume it's just a location */
			*name = NULL;
			*location = instr;
		} else {
			/* Assume it's just a name.  Make sure it's not quoted though */
			*name = instr;
			while(*(*name) && ((*(*name) < 33) || (*(*name) == '\"'))) (*name)++;
			ne = *name + strlen(*name) - 1;
			while((ne > *name) && ((*ne < 33) || (*ne == '\"'))) { *ne = '\0'; ne--; }
			*location = NULL;
		}
		return 0;
	}
	return -1;
}
