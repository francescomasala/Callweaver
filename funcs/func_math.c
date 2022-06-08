/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2004 - 2005, Andy Powell 
 *
 * Updated by Mark Spencer <markster@digium.com>
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
 * \brief Maths relatad dialplan functions
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/module.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/logger.h"
#include "openpbx/utils.h"
#include "openpbx/app.h"
#include "openpbx/config.h"

enum TypeOfFunctions
{
    ADDFUNCTION,
    DIVIDEFUNCTION,
    MULTIPLYFUNCTION,
    SUBTRACTFUNCTION,
    MODULUSFUNCTION,

    GTFUNCTION,
    LTFUNCTION,
    GTEFUNCTION,
    LTEFUNCTION,
    EQFUNCTION
};

enum TypeOfResult
{
    FLOAT_RESULT,
    INT_RESULT,
    HEX_RESULT,
    CHAR_RESULT
};


static char *builtin_function_math(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	int argc;
	char *argv[2];
	char *args;
	float fnum1;
	float fnum2;
	float ftmp = 0;
	char *op;
	int iaction=-1;
	int type_of_result=FLOAT_RESULT;

	/* dunno, big calulations :D */
	char user_result[30];

	char *mvalue1, *mvalue2=NULL, *mtype_of_result;
		
	if (!data || opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Syntax: Math(<number1><op><number 2>[,<type_of_result>]) - missing argument!\n");
		return NULL;
	}

	args = opbx_strdupa(data);	
	argc = opbx_separate_app_args(args, '|', argv, sizeof(argv) / sizeof(argv[0]));

	if (argc < 1) {
		opbx_log(LOG_WARNING, "Syntax: Math(<number1><op><number 2>[,<type_of_result>]) - missing argument!\n");
		return NULL;
	}

	mvalue1 = argv[0];
	
	if ((op = strchr(mvalue1, '+'))) {
		iaction = ADDFUNCTION;
		*op = '\0';
	} else if ((op = strchr(mvalue1, '-'))) {
		iaction = SUBTRACTFUNCTION;
		*op = '\0';
	} else if ((op = strchr(mvalue1, '*'))) {
		iaction = MULTIPLYFUNCTION;
		*op = '\0';
	} else if ((op = strchr(mvalue1, '/'))) {
		iaction = DIVIDEFUNCTION;
		*op = '\0';
	} else if ((op = strchr(mvalue1, '%'))) {
		iaction = MODULUSFUNCTION;
		*op = '\0';
	} else if ((op = strchr(mvalue1, '>'))) {
		iaction = GTFUNCTION;
		*op = '\0';
		if (*(op+1) == '=') {
			*++op = '\0';
			iaction = GTEFUNCTION;
		}
	} else if ((op = strchr(mvalue1, '<'))) {
		iaction = LTFUNCTION;
		*op = '\0';
		if (*(op+1) == '=') {
			*++op = '\0';
			iaction = LTEFUNCTION;
		}
	} else if ((op = strchr(mvalue1, '='))) {
		iaction = GTFUNCTION;
		*op = '\0';
		if (*(op+1) == '=') {
			*++op = '\0';
			iaction = EQFUNCTION;
		} else
			op = NULL;
	} 
	
	if (op) 
		mvalue2 = op + 1;

	/* detect wanted type of result */
	mtype_of_result = argv[1];
	if (mtype_of_result)
	{
		if (!strcasecmp(mtype_of_result,"float") || !strcasecmp(mtype_of_result,"f"))
			type_of_result=FLOAT_RESULT;
		else if (!strcasecmp(mtype_of_result,"int") || !strcasecmp(mtype_of_result,"i"))
			type_of_result=INT_RESULT;
		else if (!strcasecmp(mtype_of_result,"hex") || !strcasecmp(mtype_of_result,"h"))
			type_of_result=HEX_RESULT;
		else if (!strcasecmp(mtype_of_result,"char") || !strcasecmp(mtype_of_result,"c"))
			type_of_result=CHAR_RESULT;
		else
		{
			opbx_log(LOG_WARNING, "Unknown type of result requested '%s'.\n", mtype_of_result);
			return NULL;
		}
	}
	
	if (!mvalue1 || !mvalue2) {
		opbx_log(LOG_WARNING, "Supply all the parameters - just this once, please\n");
		return NULL;
	}

	if (sscanf(mvalue1, "%f", &fnum1) != 1) {
		opbx_log(LOG_WARNING, "'%s' is not a valid number\n", mvalue1);
		return NULL;
	}

	if (sscanf(mvalue2, "%f", &fnum2) != 1) {
		opbx_log(LOG_WARNING, "'%s' is not a valid number\n", mvalue2);
		return NULL;
	}

	switch (iaction) {
	case ADDFUNCTION :
		ftmp = fnum1 + fnum2;
		break;
	case DIVIDEFUNCTION :
		if (fnum2 <= 0)
			ftmp = 0; /* can't do a divide by 0 */
		else
			ftmp = (fnum1 / fnum2);
		break;
	case MULTIPLYFUNCTION :
		ftmp = (fnum1 * fnum2);
		break;
	case SUBTRACTFUNCTION :
		ftmp = (fnum1 - fnum2);
		break;
	case MODULUSFUNCTION :
	{
		int inum1 = fnum1;
		int inum2 = fnum2;
			
		ftmp = (inum1 % inum2);
		
		break;
	}
	case GTFUNCTION :
		opbx_copy_string (user_result, (fnum1 > fnum2)?"TRUE":"FALSE", sizeof (user_result));
		break;
	case LTFUNCTION :
		opbx_copy_string (user_result, (fnum1 < fnum2)?"TRUE":"FALSE", sizeof (user_result));
		break;
	case GTEFUNCTION :
		opbx_copy_string (user_result, (fnum1 >= fnum2)?"TRUE":"FALSE", sizeof (user_result));
		break;
	case LTEFUNCTION :
		opbx_copy_string (user_result, (fnum1 <= fnum2)?"TRUE":"FALSE", sizeof (user_result));
		break;					
	case EQFUNCTION :
		opbx_copy_string (user_result, (fnum1 == fnum2)?"TRUE":"FALSE", sizeof (user_result));
		break;
	default :
		opbx_log(LOG_WARNING, "Something happened that neither of us should be proud of %d\n", iaction);
		return NULL;
	}

	if (iaction < GTFUNCTION || iaction > EQFUNCTION) {
	    if (type_of_result == FLOAT_RESULT)
		    snprintf(user_result, sizeof(user_result), "%f", ftmp);
	    else if (type_of_result == INT_RESULT)
		    snprintf(user_result, sizeof(user_result), "%i", (int) ftmp);
	    else if (type_of_result == HEX_RESULT)
		snprintf(user_result, sizeof(user_result), "%x", (unsigned int) ftmp);
	    else if (type_of_result == CHAR_RESULT)
		snprintf(user_result, sizeof(user_result), "%c", (unsigned char) ftmp);
	}
		
	opbx_copy_string(buf, user_result, len);
	
	return buf;
}

static struct opbx_custom_function math_function = {
	.name = "MATH",
	.synopsis = "Performs Mathematical Functions",
	.syntax = "MATH(<number1><op><number 2>[,<type_of_result>])",
	.desc = "Perform calculation on number 1 to number 2. Valid ops are: \n"
    	    "    +,-,/,*,%,<,>,>=,<=,==\n"
		"and behave as their C equivalents.\n"
		"<type_of_result> - wanted type of result:\n"
		"	f, float - float(default)\n"
		"	i, int - integer,\n"
		"	h, hex - hex,\n"
		"	c, char - char\n"
		"Example: Set(i=${MATH(123%16,int)}) - sets var i=11",
	.read = builtin_function_math
};

static char *tdesc = "math functions";

int unload_module(void)
{
        return opbx_custom_function_unregister(&math_function);
}

int load_module(void)
{
        return opbx_custom_function_register(&math_function);
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
