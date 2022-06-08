/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Digium, Inc.
 * Portions Copyright (C) 2005, Tilghman Lesher.  All rights reserved.
 * Portions Copyright (C) 2005, Anthony Minessale II
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
 * \brief String manipulation dialplan functions
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/module.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/logger.h"
#include "openpbx/utils.h"
#include "openpbx/app.h"
#include "openpbx/localtime.h"

/* Maximum length of any variable */
#define MAXRESULT	1024

struct sortable_keys {
	char *key;
	float value;
};

static char *function_fieldqty(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	char *varname, *varval, workspace[256];
	char *delim = opbx_strdupa(data);
	int fieldcount = 0;

	if (delim) {
		varname = strsep(&delim, "|");
		pbx_retrieve_variable(chan, varname, &varval, workspace, sizeof(workspace), NULL);
		while (strsep(&varval, delim))
			fieldcount++;
		snprintf(buf, len, "%d", fieldcount);
	} else {
		opbx_log(LOG_ERROR, "Out of memory\n");
		strncpy(buf, "1", len);
	}
	return buf;
}

static struct opbx_custom_function fieldqty_function = {
	.name = "FIELDQTY",
	.synopsis = "Count the fields, with an arbitrary delimiter",
	.syntax = "FIELDQTY(<varname>,<delim>)",
	.read = function_fieldqty,
};

static char *builtin_function_regex(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	char *arg, *earg = NULL, *tmp, errstr[256] = "";
	int errcode;
	regex_t regexbuf;

	opbx_copy_string(buf, "0", len);
	
	tmp = opbx_strdupa(data);
	if (!tmp) {
		opbx_log(LOG_ERROR, "Out of memory in %s(%s)\n", cmd, data);
		return buf;
	}

	/* Regex in quotes */
	arg = strchr(tmp, '"');
	if (arg) {
		arg++;
		earg = strrchr(arg, '"');
		if (earg) {
			*earg++ = '\0';
			/* Skip over any spaces before the data we are checking */
			while (*earg == ' ')
				earg++;
		}
	} else {
		arg = tmp;
	}

	if ((errcode = regcomp(&regexbuf, arg, REG_EXTENDED | REG_NOSUB))) {
		regerror(errcode, &regexbuf, errstr, sizeof(errstr));
		opbx_log(LOG_WARNING, "Malformed input %s(%s): %s\n", cmd, data, errstr);
	} else {
		if (!regexec(&regexbuf, earg ? earg : "", 0, NULL, 0))
			opbx_copy_string(buf, "1", len); 
	}
	regfree(&regexbuf);

	return buf;
}

static struct opbx_custom_function regex_function = {
	.name = "REGEX",
	.synopsis = "Regular Expression: Returns 1 if data matches regular expression.",
	.syntax = "REGEX(\"<regular expression>\" <data>)",
	.read = builtin_function_regex,
};

static char *builtin_function_len(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	int length = 0;
	if (data) {
		length = strlen(data);
	}
	snprintf(buf, len, "%d", length);
	return buf;
}

static struct opbx_custom_function len_function = {
	.name = "LEN",
	.synopsis = "Returns the length of the argument given",
	.syntax = "LEN(<string>)",
	.read = builtin_function_len,
};

static char *acf_strftime(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	char *format, *epoch, *timezone = NULL;
	long epochi;
	struct tm time;

	buf[0] = '\0';

	if (!data) {
		opbx_log(LOG_ERROR, "OpenPBX function STRFTIME() requires an argument.\n");
		return buf;
	}
	
	format = opbx_strdupa(data);
	if (!format) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		return buf;
	}
	
	epoch = strsep(&format, "|");
	timezone = strsep(&format, "|");

	if (!epoch || opbx_strlen_zero(epoch) || !sscanf(epoch, "%ld", &epochi)) {
		struct timeval tv = opbx_tvnow();
		epochi = tv.tv_sec;
	}

	opbx_localtime(&epochi, &time, timezone);

	if (!format) {
		format = "%c";
	}

	if (!strftime(buf, len, format, &time)) {
		opbx_log(LOG_WARNING, "C function strftime() output nothing?!!\n");
	}
	buf[len - 1] = '\0';

	return buf;
}

static struct opbx_custom_function strftime_function = {
	.name = "STRFTIME",
	.synopsis = "Returns the current date/time in a specified format.",
	.syntax = "STRFTIME([<epoch>][,[timezone][,format]])",
	.read = acf_strftime,
};

static char *function_eval(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	memset(buf, 0, len);

	if (!data || opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "EVAL requires an argument: EVAL(<string>)\n");
		return buf;
	}

	pbx_substitute_variables_helper(chan, data, buf, len - 1);

	return buf;
}

static struct opbx_custom_function eval_function = {
	.name = "EVAL",
	.synopsis = "Evaluate stored variables.",
	.syntax = "EVAL(<variable>)",
	.desc = "Using EVAL basically causes a string to be evaluated twice.\n"
		"When a variable or expression is in the dialplan, it will be\n"
		"evaluated at runtime. However, if the result of the evaluation\n"
		"is in fact a variable or expression, using EVAL will have it\n"
		"evaluated a second time. For example, if the variable ${MYVAR}\n"
		"contains \"${OTHERVAR}\", then the result of putting ${EVAL(${MYVAR})}\n"
		"in the dialplan will be the contents of the variable, OTHERVAR.\n"
		"Normally, by just putting ${MYVAR} in the dialplan, you would be\n"
		"left with \"${OTHERVAR}\".\n", 
	.read = function_eval,
};

static char *function_cut(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	char *s, *varname=NULL, *delimiter=NULL, *field=NULL;
	int args_okay = 0;

	memset(buf, 0, len);

	/* Check and parse arguments */
	if (data) {
		s = opbx_strdupa((char *)data);
		if (s) {
			varname = strsep(&s, "|");
			if (varname && (varname[0] != '\0')) {
				delimiter = strsep(&s, "|");
				if (delimiter) {
					field = strsep(&s, "|");
					if (field) {
						args_okay = 1;
					}
				}
			}
		} else {
			opbx_log(LOG_ERROR, "Out of memory\n");
			return buf;
		}
	}

	if (args_okay) {
		char d, ds[2];
		char *tmp = alloca(strlen(varname) + 4);
		char varvalue[MAXRESULT], *tmp2=varvalue;

		if (tmp) {
			snprintf(tmp, strlen(varname) + 4, "${%s}", varname);
			memset(varvalue, 0, sizeof(varvalue));
		} else {
			opbx_log(LOG_ERROR, "Out of memory\n");
			return buf;
		}

		if (delimiter[0])
			d = delimiter[0];
		else
			d = '-';

		/* String form of the delimiter, for use with strsep(3) */
		snprintf(ds, sizeof(ds), "%c", d);

		pbx_substitute_variables_helper(chan, tmp, tmp2, MAXRESULT - 1);

		if (tmp2) {
			int curfieldnum = 1;
			while ((tmp2 != NULL) && (field != NULL)) {
				char *nextgroup = strsep(&field, "&");
				int num1 = 0, num2 = MAXRESULT;
				char trashchar;

				if (sscanf(nextgroup, "%d-%d", &num1, &num2) == 2) {
					/* range with both start and end */
				} else if (sscanf(nextgroup, "-%d", &num2) == 1) {
					/* range with end */
					num1 = 0;
				} else if ((sscanf(nextgroup, "%d%c", &num1, &trashchar) == 2) && (trashchar == '-')) {
					/* range with start */
					num2 = MAXRESULT;
				} else if (sscanf(nextgroup, "%d", &num1) == 1) {
					/* single number */
					num2 = num1;
				} else {
					opbx_log(LOG_ERROR, "Usage: CUT(<varname>,<char-delim>,<range-spec>)\n");
					return buf;
				}

				/* Get to start, if any */
				if (num1 > 0) {
					while ((tmp2 != (char *)NULL + 1) && (curfieldnum < num1)) {
						tmp2 = index(tmp2, d) + 1;
						curfieldnum++;
					}
				}

				/* Most frequent problem is the expectation of reordering fields */
				if ((num1 > 0) && (curfieldnum > num1)) {
					opbx_log(LOG_WARNING, "We're already past the field you wanted?\n");
				}

				/* Re-null tmp2 if we added 1 to NULL */
				if (tmp2 == (char *)NULL + 1)
					tmp2 = NULL;

				/* Output fields until we either run out of fields or num2 is reached */
				while ((tmp2 != NULL) && (curfieldnum <= num2)) {
					char *tmp3 = strsep(&tmp2, ds);
					int curlen = strlen(buf);

					if (curlen) {
						snprintf(buf + curlen, len - curlen, "%c%s", d, tmp3);
					} else {
						snprintf(buf, len, "%s", tmp3);
					}

					curfieldnum++;
				}
			}
		}
	}
	return buf;
}

static struct opbx_custom_function cut_function = {
	.name = "CUT",
	.synopsis = "Slices and dices strings, based upon a named delimiter.",
	.syntax = "CUT(<varname>,<char-delim>,<range-spec>)",
	.desc = "  varname    - variable you want cut\n"
		"  char-delim - defaults to '-'\n"
		"  range-spec - number of the field you want (1-based offset)\n"
		"             may also be specified as a range (with -)\n"
		"             or group of ranges and fields (with &)\n",
	.read = function_cut,
};

static int sort_subroutine(const void *arg1, const void *arg2)
{
	const struct sortable_keys *one=arg1, *two=arg2;
	if (one->value < two->value) {
		return -1;
	} else if (one->value == two->value) {
		return 0;
	} else {
		return 1;
	}
}

static char *function_sort(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	char *strings, *ptrkey, *ptrvalue;
	int count=1, count2, element_count=0;
	struct sortable_keys *sortable_keys;

	memset(buf, 0, len);

	if (!data) {
		opbx_log(LOG_ERROR, "SORT() requires an argument\n");
		return buf;
	}

	strings = opbx_strdupa((char *)data);
	if (!strings) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		return buf;
	}

	for (ptrkey = strings; *ptrkey; ptrkey++) {
		if (*ptrkey == '|') {
			count++;
		}
	}

	sortable_keys = alloca(count * sizeof(struct sortable_keys));
	if (!sortable_keys) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		return buf;
	}

	memset(sortable_keys, 0, count * sizeof(struct sortable_keys));

	/* Parse each into a struct */
	count2 = 0;
	while ((ptrkey = strsep(&strings, "|"))) {
		ptrvalue = index(ptrkey, ':');
		if (!ptrvalue) {
			count--;
			continue;
		}
		*ptrvalue = '\0';
		ptrvalue++;
		sortable_keys[count2].key = ptrkey;
		sscanf(ptrvalue, "%f", &sortable_keys[count2].value);
		count2++;
	}

	/* Sort the structs */
	qsort(sortable_keys, count, sizeof(struct sortable_keys), sort_subroutine);

	for (count2 = 0; count2 < count; count2++) {
		int blen = strlen(buf);
		if (element_count++) {
			strncat(buf + blen, ",", len - blen - 1);
		}
		strncat(buf + blen + 1, sortable_keys[count2].key, len - blen - 2);
	}

	return buf;
}

static struct opbx_custom_function sort_function = {
	.name= "SORT",
	.synopsis = "Sorts a list of key/vals into a list of keys, based upon the vals",
	.syntax = "SORT(key1:val1[...][,keyN:valN])",
	.desc = "Takes a comma-separated list of keys and values, each separated by a colon, and returns a\n"
		"comma-separated list of the keys, sorted by their values.  Values will be evaluated as\n"
		"floating-point numbers.\n",
	.read = function_sort,
};

static char *tdesc = "string functions";

int unload_module(void)
{
        int res = 0;

        if (opbx_custom_function_unregister(&fieldqty_function) < 0)
                res = -1;

        if (opbx_custom_function_unregister(&regex_function) < 0)
                res = -1;

        if (opbx_custom_function_unregister(&len_function) < 0)
                res = -1;

        if (opbx_custom_function_unregister(&strftime_function) < 0)
                res = -1;

        if (opbx_custom_function_unregister(&eval_function) < 0)
                res = -1;

        if (opbx_custom_function_unregister(&cut_function) < 0)
                res = -1;

        if (opbx_custom_function_unregister(&sort_function) < 0)
                res = -1;

        return res;
}

int load_module(void)
{
        int res = 0;

        if (opbx_custom_function_register(&fieldqty_function) < 0)
                res = -1;

        if (opbx_custom_function_register(&regex_function) < 0)
                res = -1;

        if (opbx_custom_function_register(&len_function) < 0)
                res = -1;

        if (opbx_custom_function_register(&strftime_function) < 0)
                res = -1;

        if (opbx_custom_function_register(&eval_function) < 0)
                res = -1;

        if (opbx_custom_function_register(&cut_function) < 0)
                res = -1;

        if (opbx_custom_function_register(&sort_function) < 0)
                res = -1;

        return res;
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	return 0;
}

/*
Local Variables:
mode: C
c-file-style: "linux"
indent-tabs-mode: nil
End:
*/
