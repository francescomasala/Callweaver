/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2002, Pauline Middelink
 *
 * Pauline Middelink <middelink@polyware.nl>
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

/*! \file res_indications.c 
 *
 * \brief Load the indications
 * 
 * \author Pauline Middelink <middelink at polyware.nl>
 *
 * Load the country specific dialtones into the openpbx PBX.
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h> 
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/cli.h"
#include "openpbx/logger.h"
#include "openpbx/config.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/translate.h"
#include "openpbx/indications.h"


/* Globals */
static const char dtext[] = "Indications Configuration";
static const char config[] = "indications.conf";

/*
 * Help for commands provided by this module ...
 */
static char help_add_indication[] =
"Usage: indication add <country> <indication> \"<tonelist>\"\n"
"       Add the given indication to the country.\n";

static char help_remove_indication[] =
"Usage: indication remove <country> <indication>\n"
"       Remove the given indication from the country.\n";

static char help_show_indications[] =
"Usage: show indications [<country> ...]\n"
"       Show either a condensed for of all country/indications, or the\n"
"       indications for the specified countries.\n";

char *playtones_desc=
"PlayTones(arg): Plays a tone list. Execution will continue with the next step immediately,\n"
"while the tones continue to play.\n"
"Arg is either the tone name defined in the indications.conf configuration file, or a directly\n"
"specified list of frequencies and durations.\n"
"See the sample indications.conf for a description of the specification of a tonelist.\n\n"
"Use the StopPlayTones application to stop the tones playing. \n";

/*
 * Implementation of functions provided by this module
 */

/*
 * ADD INDICATION command stuff
 */
static int handle_add_indication(int fd, int argc, char *argv[])
{
    struct tone_zone *tz;
    int created_country = 0;
    if (argc != 5) return RESULT_SHOWUSAGE;

    tz = opbx_get_indication_zone(argv[2]);
    if (!tz) {
        /* country does not exist, create it */
        opbx_log(LOG_NOTICE, "Country '%s' does not exist, creating it.\n",argv[2]);

        if ((tz = malloc(sizeof(struct tone_zone))) == NULL)
        {
            opbx_log(LOG_WARNING, "Out of memory\n");
            return -1;
        }
        memset(tz,0,sizeof(struct tone_zone));
        opbx_copy_string(tz->country,argv[2],sizeof(tz->country));
        if (opbx_register_indication_country(tz))
        {
            opbx_log(LOG_WARNING, "Unable to register new country\n");
            free(tz);
            return -1;
        }
        created_country = 1;
    }
    if (opbx_register_indication(tz,argv[3],argv[4]))
    {
        opbx_log(LOG_WARNING, "Unable to register indication %s/%s\n",argv[2],argv[3]);
        if (created_country)
            opbx_unregister_indication_country(argv[2]);
        return -1;
    }
    return 0;
}

/*
 * REMOVE INDICATION command stuff
 */
static int handle_remove_indication(int fd, int argc, char *argv[])
{
    struct tone_zone *tz;
    
    if (argc != 3  &&  argc != 4)
        return RESULT_SHOWUSAGE;

    if (argc == 3)
    {
        /* remove entiry country */
        if (opbx_unregister_indication_country(argv[2]))
        {
            opbx_log(LOG_WARNING, "Unable to unregister indication country %s\n",argv[2]);
            return -1;
        }
        return 0;
    }

    tz = opbx_get_indication_zone(argv[2]);
    if (!tz) {
        opbx_log(LOG_WARNING, "Unable to unregister indication %s/%s, country does not exists\n",argv[2],argv[3]);
        return -1;
    }
    if (opbx_unregister_indication(tz,argv[3])) {
        opbx_log(LOG_WARNING, "Unable to unregister indication %s/%s\n",argv[2],argv[3]);
        return -1;
    }
    return 0;
}

/*
 * SHOW INDICATIONS command stuff
 */
static int handle_show_indications(int fd, int argc, char *argv[])
{
    struct tone_zone *tz;
    char buf[256];
    int found_country = 0;

    if (opbx_mutex_lock(&tzlock)) {
        opbx_log(LOG_WARNING, "Unable to lock tone_zones list\n");
        return 0;
    }
    if (argc == 2) {
        /* no arguments, show a list of countries */
        opbx_cli(fd,"Country Alias   Description\n"
               "===========================\n");
        for (tz=tone_zones; tz; tz=tz->next) {
            opbx_cli(fd,"%-7.7s %-7.7s %s\n", tz->country, tz->alias, tz->description);
        }
        opbx_mutex_unlock(&tzlock);
        return 0;
    }
    /* there was a request for specific country(ies), lets humor them */
    for (tz=tone_zones; tz; tz=tz->next) {
        int i,j;
        for (i = 2;  i < argc;  i++)
        {
            if (strcasecmp(tz->country,argv[i])==0 &&
                !tz->alias[0])
            {
                struct tone_zone_sound* ts;
                if (!found_country)
                {
                    found_country = 1;
                    opbx_cli(fd,"Country Indication      PlayList\n"
                           "=====================================\n");
                }
                j = snprintf(buf,sizeof(buf),"%-7.7s %-15.15s ",tz->country,"<ringcadence>");
                for (i = 0;  i < tz->nrringcadence;  i++)
                {
                    j += snprintf(buf + j, sizeof(buf) - j,"%d,",tz->ringcadence[i]);
                }
                if (tz->nrringcadence)
                    j--;
                opbx_copy_string(buf+j,"\n",sizeof(buf)-j);
                opbx_cli(fd,buf);
                for (ts = tz->tones;  ts;  ts = ts->next)
                    opbx_cli(fd,"%-7.7s %-15.15s %s\n", tz->country, ts->name, ts->data);
                break;
            }
        }
    }
    if (!found_country)
        opbx_cli(fd,"No countries matched your criteria.\n");
    opbx_mutex_unlock(&tzlock);
    return -1;
}

/*
 * Playtones command stuff
 */
static int handle_playtones(struct opbx_channel *chan, void *data)
{
    struct tone_zone_sound *ts;
    int res;

    if (data == NULL  ||  ((char *) data)[0] == 0)
    {
        opbx_log(LOG_NOTICE,"Nothing to play\n");
        return -1;
    }
    ts = opbx_get_indication_tone(chan->zone, (const char*)data);
    if (ts  &&  ts->data[0])
        res = opbx_playtones_start(chan, 0, ts->data, 0);
    else
        res = opbx_playtones_start(chan, 0, (const char*)data, 0);
    if (res)
        opbx_log(LOG_NOTICE,"Unable to start playtones\n");
    return res;
}

/*
 * StopPlaylist command stuff
 */
static int handle_stopplaytones(struct opbx_channel *chan, void *data)
{
    opbx_playtones_stop(chan);
    return 0;
}

/*
 * Load module stuff
 */
static int ind_load_module(void)
{
    struct opbx_config *cfg;
    struct opbx_variable *v;
    char *cxt;
    char *c;
    struct tone_zone *tones;
    const char *country = NULL;

    /* that the following cast is needed, is yuk! */
    /* yup, checked it out. It is NOT written to. */
    cfg = opbx_config_load((char *)config);
    if (!cfg)
        return 0;

    /* Use existing config to populate the Indication table */
    cxt = opbx_category_browse(cfg, NULL);
    while (cxt)
    {
        /* All categories but "general" are considered countries */
        if (!strcasecmp(cxt, "general"))
        {
            cxt = opbx_category_browse(cfg, cxt);
            continue;
        }
        if ((tones = malloc(sizeof(struct tone_zone))) == NULL)
        {
            opbx_log(LOG_WARNING,"Out of memory\n");
            opbx_config_destroy(cfg);
            return -1;
        }
        memset(tones, 0, sizeof(struct tone_zone));
        opbx_copy_string(tones->country, cxt, sizeof(tones->country));

        v = opbx_variable_browse(cfg, cxt);
        while (v)
        {
            if (!strcasecmp(v->name, "description"))
            {
                opbx_copy_string(tones->description, v->value, sizeof(tones->description));
            }
            else if (!strcasecmp(v->name,"ringcadence") || !strcasecmp(v->name,"ringcadance"))
            {
                char *ring,*rings = opbx_strdupa(v->value);
                c = rings;
                ring = strsep(&c,",");
                while (ring)
                {
                    int *tmp, val;
                    if (!isdigit(ring[0]) || (val=atoi(ring))==-1)
                    {
                        opbx_log(LOG_WARNING,"Invalid ringcadence given '%s' at line %d.\n",ring,v->lineno);
                        ring = strsep(&c,",");
                        continue;
                    }
                    if ((tmp = realloc(tones->ringcadence,(tones->nrringcadence+1)*sizeof(int))) == NULL)
                    {
                        opbx_log(LOG_WARNING, "Out of memory\n");
                        opbx_config_destroy(cfg);
                        return -1;
                    }
                    tones->ringcadence = tmp;
                    tmp[tones->nrringcadence] = val;
                    tones->nrringcadence++;
                    /* next item */
                    ring = strsep(&c,",");
                }
            }
            else if (!strcasecmp(v->name,"alias"))
            {
                char *countries = opbx_strdupa(v->value);
                c = countries;
                country = strsep(&c,",");
                while (country)
                {
                    struct tone_zone* azone = malloc(sizeof(struct tone_zone));
                    if (!azone)
                    {
                        opbx_log(LOG_WARNING,"Out of memory\n");
                        opbx_config_destroy(cfg);
                        return -1;
                    }
                    memset(azone,0,sizeof(struct tone_zone));
                    opbx_copy_string(azone->country, country, sizeof(azone->country));
                    opbx_copy_string(azone->alias, cxt, sizeof(azone->alias));
                    if (opbx_register_indication_country(azone))
                    {
                        opbx_log(LOG_WARNING, "Unable to register indication alias at line %d.\n",v->lineno);
                        free(tones);
                    }
                    /* next item */
                    country = strsep(&c,",");
                }
            }
            else
            {
                /* add tone to country */
                struct tone_zone_sound *ps,*ts;
                for (ps = NULL, ts = tones->tones;  ts;  ps = ts, ts = ts->next)
                {
                    if (strcasecmp(v->name,ts->name) == 0)
                    {
                        /* already there */
                        opbx_log(LOG_NOTICE,"Duplicate entry '%s', skipped.\n",v->name);
                        goto out;
                    }
                }
                /* not there, add it to the back */
                if ((ts = malloc(sizeof(struct tone_zone_sound))) == NULL)
                {
                    opbx_log(LOG_WARNING, "Out of memory\n");
                    opbx_config_destroy(cfg);
                    return -1;
                }
                ts->next = NULL;
                ts->name = strdup(v->name);
                ts->data = strdup(v->value);
                if (ps)
                    ps->next = ts;
                else
                    tones->tones = ts;
            }
out:            v = v->next;
        }
        if (tones->description[0] || tones->alias[0] || tones->tones) {
            if (opbx_register_indication_country(tones)) {
                opbx_log(LOG_WARNING, "Unable to register indication at line %d.\n",v->lineno);
                free(tones);
            }
        } else free(tones);

        cxt = opbx_category_browse(cfg, cxt);
    }

    /* determine which country is the default */
    country = opbx_variable_retrieve(cfg,"general","country");
    if (!country || !*country || opbx_set_indication_country(country))
        opbx_log(LOG_WARNING,"Unable to set the default country (for indication tones)\n");

    opbx_config_destroy(cfg);
    return 0;
}

/*
 * CLI entries for commands provided by this module
 */
static struct opbx_cli_entry add_indication_cli =
    { { "indication", "add", NULL }, handle_add_indication,
        "Add the given indication to the country", help_add_indication,
        NULL };

static struct opbx_cli_entry remove_indication_cli =
    { { "indication", "remove", NULL }, handle_remove_indication,
        "Remove the given indication from the country", help_remove_indication,
        NULL };

static struct opbx_cli_entry show_indications_cli =
    { { "show", "indications", NULL }, handle_show_indications,
        "Show a list of all country/indications", help_show_indications,
        NULL };

/*
 * Standard module functions ...
 */
int unload_module(void)
{
    /* remove the registed indications... */
    opbx_unregister_indication_country(NULL);

    /* and the functions */
    opbx_cli_unregister(&add_indication_cli);
    opbx_cli_unregister(&remove_indication_cli);
    opbx_cli_unregister(&show_indications_cli);
    opbx_unregister_application("PlayTones");
    opbx_unregister_application("StopPlayTones");
    return 0;
}

int load_module(void)
{
    if (ind_load_module()) return -1;
 
    opbx_cli_register(&add_indication_cli);
    opbx_cli_register(&remove_indication_cli);
    opbx_cli_register(&show_indications_cli);
    opbx_register_application("PlayTones", handle_playtones, "Play a tone list", playtones_desc);
    opbx_register_application("StopPlayTones", handle_stopplaytones, "Stop playing a tone list","Stop playing a tone list");

    return 0;
}

int reload(void)
{
    /* remove the registed indications... */
    opbx_unregister_indication_country(NULL);

    return ind_load_module();
}

char *description(void)
{
    /* that the following cast is needed, is yuk! */
    return (char*)dtext;
}

int usecount(void)
{
    return 0;
}
