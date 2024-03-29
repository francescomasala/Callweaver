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

/*! \file
 *
 * \brief Meet me conference bridge
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include ZAPTEL_H

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/apps/app_meetme.c $", "$Revision: 4723 $")

#include "callweaver/lock.h"
#include "callweaver/file.h"
#include "callweaver/logger.h"
#include "callweaver/channel.h"
#include "callweaver/pbx.h"
#include "callweaver/module.h"
#include "callweaver/config.h"
#include "callweaver/app.h"
#include "callweaver/dsp.h"
#include "callweaver/musiconhold.h"
#include "callweaver/manager.h"
#include "callweaver/options.h"
#include "callweaver/cli.h"
#include "callweaver/say.h"
#include "callweaver/utils.h"

static char *tdesc = "MeetMe conference bridge";

static void *app;
static void *app2;
static void *app3;

static char *name = "MeetMe";
static char *name2 = "MeetMeCount";
static char *name3 = "MeetMeAdmin";

static char *synopsis = "MeetMe conference bridge";
static char *synopsis2 = "MeetMe participant count";
static char *synopsis3 = "MeetMe conference Administration";

static char *syntax = "MeetMe([confno[, options[, pin]]])";
static char *syntax2 = "MeetMeCount(confno[, var])";
static char *syntax3 = "MeetMeAdmin(confno,command[, user])";

static char *descrip =
    "Enters the user into a specified MeetMe conference.\n"
    "If the conference number is omitted, the user will be prompted to enter\n"
    "one. \n"
    "MeetMe returns 0 if user pressed # to exit (see option 'p'), otherwise -1.\n"
    "Please note: A ZAPTEL INTERFACE MUST BE INSTALLED FOR CONFERENCING TO WORK!\n\n"

    "The option string may contain zero or more of the following characters:\n"
    "      'm' -- set monitor only mode (Listen only, no talking)\n"
    "      't' -- set talk only mode. (Talk only, no listening)\n"
    "      'T' -- set talker detection (sent to manager interface and meetme list)\n"
    "      'i' -- announce user join/leave\n"
    "      'p' -- allow user to exit the conference by pressing '#'\n"
    "      'X' -- allow user to exit the conference by entering a valid single\n"
    "             digit extension ${MEETME_EXIT_CONTEXT} or the current context\n"
    "             if that variable is not defined.\n"
    "      'd' -- dynamically add conference\n"
    "      'D' -- dynamically add conference, prompting for a PIN\n"
    "      'e' -- select an empty conference\n"
    "      'E' -- select an empty pinless conference\n"
    "      'v' -- video mode\n"
    "      'r' -- Record conference (records as ${MEETME_RECORDINGFILE}\n"
    "             using format ${MEETME_RECORDINGFORMAT}). Default filename is\n"
    "             meetme-conf-rec-${CONFNO}-${UNIQUEID} and the default format is wav.\n"
    "      'q' -- quiet mode (don't play enter/leave sounds)\n"
    "      'c' -- announce user(s) count on joining a conference\n"
    "      'M' -- enable music on hold when the conference has a single caller\n"
    "      'x' -- close the conference when last marked user exits\n"
    "      'w' -- wait until the marked user enters the conference\n"
    "      'b' -- run OGI script specified in ${MEETME_OGI_BACKGROUND}\n"
    "         Default: conf-background.ogi\n"
    "        (Note: This does not work with non-Zap channels in the same conference)\n"
    "      's' -- Present menu (user or admin) when '*' is received ('send' to menu)\n"
    "      'a' -- set admin mode\n"
    "      'A' -- set marked mode\n"
    "      'P' -- always prompt for the pin even if it is specified\n";

static char *descrip2 =
    "Plays back the number of users in the specifiedi\n"
    "MeetMe conference. If var is specified, playback will be skipped and the value\n"
    "will be returned in the variable. Returns 0 on success or -1 on a hangup.\n"
    "A ZAPTEL INTERFACE MUST BE INSTALLED FOR CONFERENCING FUNCTIONALITY.\n";

static char *descrip3 =
    "Run admin command for conference\n"
    "      'K' -- Kick all users out of conference\n"
    "      'k' -- Kick one user out of conference\n"
    "      'e' -- Eject last user that joined\n"
    "      'L' -- Lock conference\n"
    "      'l' -- Unlock conference\n"
    "      'M' -- Mute conference\n"
    "      'm' -- Unmute conference\n"
    "      'N' -- Mute entire conference (except admin)\n"
    "      'n' -- Unmute entire conference (except admin)\n"
    "";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static struct cw_conference
{
    char confno[CW_MAX_EXTENSION];        /* Conference */
    struct cw_channel *chan;  /* Announcements channel */
    int fd;                     /* Announcements fd */
    int zapconf;                /* Zaptel Conf # */
    int users;                  /* Number of active users */
    int markedusers;            /* Number of marked users */
    struct cw_conf_user *firstuser;  /* Pointer to the first user struct */
    struct cw_conf_user *lastuser;   /* Pointer to the last user struct */
    time_t start;               /* Start time (s) */
    int recording;              /* recording status */
    int isdynamic;              /* Created on the fly? */
    int locked;                 /* Is the conference locked? */
    pthread_t recordthread;     /* thread for recording */
    pthread_attr_t attr;        /* thread attribute */
    char *recordingfilename;    /* Filename to record the Conference into */
    char *recordingformat;      /* Format to record the Conference in */
    char pin[CW_MAX_EXTENSION];            /* If protected by a PIN */
    char pinadmin[CW_MAX_EXTENSION];    /* If protected by a admin PIN */
    struct cw_conference *next;
} *confs;

struct volume
{
    int desired;                /* Desired volume adjustment */
    int actual;                 /* Actual volume adjustment (for channels that can't adjust) */
};

struct cw_conf_user
{
    int user_no;                /* User Number */
    struct cw_conf_user *prevuser;        /* Pointer to the previous user */
    struct cw_conf_user *nextuser;        /* Pointer to the next user */
    int userflags;              /* Flags as set in the conference */
    int adminflags;             /* Flags set by the Admin */
    struct cw_channel *chan;  /* Connected channel */
    int talking;                /* Is user talking */
    int zapchannel;             /* Is a Zaptel channel */
    char usrvalue[50];          /* Custom User Value */
    char namerecloc[CW_MAX_EXTENSION];    /* Name Recorded file Location */
    time_t jointime;            /* Time the user joined the conference */
    struct volume talk;
    struct volume listen;
};

#define ADMINFLAG_MUTED (1 << 1)    /* User is muted */
#define ADMINFLAG_KICKME (1 << 2)    /* User is kicked */
#define MEETME_DELAYDETECTTALK         300
#define MEETME_DELAYDETECTENDTALK     1000

enum volume_action
{
    VOL_UP,
    VOL_DOWN,
};

CW_MUTEX_DEFINE_STATIC(conflock);

static int admin_exec(struct cw_channel *chan, int argc, char **argv);

static void *recordthread(void *args);

#include "enter.h"
#include "leave.h"

#define ENTER    0
#define LEAVE    1

#define MEETME_RECORD_OFF           0
#define MEETME_RECORD_ACTIVE        1
#define MEETME_RECORD_TERMINATE     2

#define CONF_SIZE 320

#define CONFFLAG_ADMIN    (1 << 1)    /* If set the user has admin access on the conference */
#define CONFFLAG_MONITOR (1 << 2)    /* If set the user can only receive audio from the conference */
#define CONFFLAG_POUNDEXIT (1 << 3)    /* If set callweaver will exit conference when '#' is pressed */
#define CONFFLAG_STARMENU (1 << 4)    /* If set callweaver will provide a menu to the user what '*' is pressed */
#define CONFFLAG_TALKER (1 << 5)    /* If set the use can only send audio to the conference */
#define CONFFLAG_QUIET (1 << 6)        /* If set there will be no enter or leave sounds */
#define CONFFLAG_VIDEO (1 << 7)        /* Set to enable video mode */
#define CONFFLAG_OGI (1 << 8)        /* Set to run OGI Script in Background */
#define CONFFLAG_MOH (1 << 9)        /* Set to have music on hold when user is alone in conference */
#define CONFFLAG_MARKEDEXIT (1 << 10)    /* If set the MeetMe will return if all marked with this flag left */
#define CONFFLAG_WAITMARKED (1 << 11)    /* If set, the MeetMe will wait until a marked user enters */
#define CONFFLAG_EXIT_CONTEXT (1 << 12)    /* If set, the MeetMe will exit to the specified context */
#define CONFFLAG_MARKEDUSER (1 << 13)    /* If set, the user will be marked */
#define CONFFLAG_INTROUSER (1 << 14)    /* If set, user will be ask record name on entry of conference */
#define CONFFLAG_RECORDCONF (1<< 15)    /* If set, the MeetMe will be recorded */
#define CONFFLAG_MONITORTALKER (1 << 16) /* If set, the user will be monitored if the user is talking or not */
#define CONFFLAG_DYNAMIC (1 << 17)
#define CONFFLAG_DYNAMICPIN (1 << 18)
#define CONFFLAG_EMPTY (1 << 19)
#define CONFFLAG_EMPTYNOPIN (1 << 20)
#define CONFFLAG_ALWAYSPROMPT (1 << 21)
#define CONFFLAG_ANNOUNCEUSERCOUNT (1 << 22) /* If set, when user joins the conference, they will be told the number of users that are already in */


CW_DECLARE_OPTIONS(meetme_opts,{
    ['a'] = { CONFFLAG_ADMIN },
    ['c'] = { CONFFLAG_ANNOUNCEUSERCOUNT },
    ['T'] = { CONFFLAG_MONITORTALKER },
    ['i'] = { CONFFLAG_INTROUSER },
    ['m'] = { CONFFLAG_MONITOR },
    ['p'] = { CONFFLAG_POUNDEXIT },
    ['s'] = { CONFFLAG_STARMENU },
    ['t'] = { CONFFLAG_TALKER },
    ['q'] = { CONFFLAG_QUIET },
    ['M'] = { CONFFLAG_MOH },
    ['x'] = { CONFFLAG_MARKEDEXIT },
    ['X'] = { CONFFLAG_EXIT_CONTEXT },
    ['A'] = { CONFFLAG_MARKEDUSER },
    ['b'] = { CONFFLAG_OGI },
    ['w'] = { CONFFLAG_WAITMARKED },
    ['r'] = { CONFFLAG_RECORDCONF },
    ['d'] = { CONFFLAG_DYNAMIC },
    ['D'] = { CONFFLAG_DYNAMICPIN },
    ['e'] = { CONFFLAG_EMPTY },
    ['E'] = { CONFFLAG_EMPTYNOPIN },
    ['P'] = { CONFFLAG_ALWAYSPROMPT },
});

static char *istalking(int x)
{
    if (x > 0)
        return "(talking)";
    else if (x < 0)
        return "(unmonitored)";
    else
        return "(not talking)";
}

static int careful_write(int fd, unsigned char *data, int len)
{
    int res;
    int x;
    while (len)
    {
        x = ZT_IOMUX_WRITE | ZT_IOMUX_SIGEVENT;
        res = ioctl(fd, ZT_IOMUX, &x);
        if (res >= 0)
            res = write(fd, data, len);
        if (res < 1)
        {
            if (errno != EAGAIN)
            {
                cw_log(LOG_WARNING, "Failed to write audio data to conference: %s\n", strerror(errno));
                return -1;
            }
            return 0;
        }
        len -= res;
        data += res;
    }
    return 0;
}

/* Map 'volume' levels from -5 through +5 into
   decibel (dB) settings for channel drivers
   Note: these are not a straight linear-to-dB
   conversion... the numbers have been modified
   to give the user a better level of adjustability
*/
static signed char gain_map[] =
{
    -15,
    -13,
    -10,
    -6,
    0,
    0,
    0,
    6,
    10,
    13,
    15
};

static int set_talk_volume(struct cw_conf_user *user, int volume)
{
    signed char gain_adjust;

    /* attempt to make the adjustment in the channel driver;
       if successful, don't adjust in the frame reading routine
    */
    gain_adjust = gain_map[volume + 5];
    return cw_channel_setoption(user->chan, CW_OPTION_RXGAIN, &gain_adjust, sizeof(gain_adjust), 0);
}

static int set_listen_volume(struct cw_conf_user *user, int volume)
{
    signed char gain_adjust;

    /* attempt to make the adjustment in the channel driver;
       if successful, don't adjust in the frame reading routine
    */
    gain_adjust = gain_map[volume + 5];
    return cw_channel_setoption(user->chan, CW_OPTION_TXGAIN, &gain_adjust, sizeof(gain_adjust), 0);
}

static void tweak_volume(struct volume *vol, enum volume_action action)
{
    switch (action)
    {
    case VOL_UP:
        switch (vol->desired)
        {
        case 5:
            break;
        case 0:
            vol->desired = 2;
            break;
        case -2:
            vol->desired = 0;
            break;
        default:
            vol->desired++;
            break;
        }
        break;
    case VOL_DOWN:
        switch (vol->desired)
        {
        case -5:
            break;
        case 2:
            vol->desired = 0;
            break;
        case 0:
            vol->desired = -2;
            break;
        default:
            vol->desired--;
            break;
        }
    }
}

static void tweak_talk_volume(struct cw_conf_user *user, enum volume_action action)
{
    tweak_volume(&user->talk, action);
    /* attempt to make the adjustment in the channel driver;
       if successful, don't adjust in the frame reading routine
    */
    if (!set_talk_volume(user, user->talk.desired))
        user->talk.actual = 0;
    else
        user->talk.actual = user->talk.desired;
}

static void tweak_listen_volume(struct cw_conf_user *user, enum volume_action action)
{
    tweak_volume(&user->listen, action);
    /* attempt to make the adjustment in the channel driver;
       if successful, don't adjust in the frame reading routine
    */
    if (!set_listen_volume(user, user->listen.desired))
        user->listen.actual = 0;
    else
        user->listen.actual = user->listen.desired;
}

static void reset_volumes(struct cw_conf_user *user)
{
    signed char zero_volume = 0;

    cw_channel_setoption(user->chan, CW_OPTION_TXGAIN, &zero_volume, sizeof(zero_volume), 0);
    cw_channel_setoption(user->chan, CW_OPTION_RXGAIN, &zero_volume, sizeof(zero_volume), 0);
}

static void conf_play(struct cw_channel *chan, struct cw_conference *conf, int sound)
{
    unsigned char *data;
    int len;
    int res=-1;

    if (!chan->_softhangup)
        res = cw_autoservice_start(chan);
    cw_mutex_lock(&conflock);
    switch (sound)
    {
    case ENTER:
        data = enter;
        len = sizeof(enter);
        break;
    case LEAVE:
        data = leave;
        len = sizeof(leave);
        break;
    default:
        data = NULL;
        len = 0;
    }
    if (data)
        careful_write(conf->fd, data, len);
    cw_mutex_unlock(&conflock);
    if (!res)
        cw_autoservice_stop(chan);
}

static struct cw_conference *build_conf(char *confno, char *pin, char *pinadmin, int make, int dynamic)
{
    struct cw_conference *cnf;
    struct zt_confinfo ztc;
    cw_mutex_lock(&conflock);
    cnf = confs;
    while (cnf)
    {
        if (!strcmp(confno, cnf->confno))
            break;
        cnf = cnf->next;
    }
    if (!cnf && (make || dynamic))
    {
        cnf = malloc(sizeof(struct cw_conference));
        if (cnf)
        {
            /* Make a new one */
            memset(cnf, 0, sizeof(struct cw_conference));
            cw_copy_string(cnf->confno, confno, sizeof(cnf->confno));
            cw_copy_string(cnf->pin, pin, sizeof(cnf->pin));
            cw_copy_string(cnf->pinadmin, pinadmin, sizeof(cnf->pinadmin));
            cnf->markedusers = 0;
            cnf->chan = cw_request("zap", CW_FORMAT_ULAW, "pseudo", NULL);
            if (cnf->chan)
            {
                cnf->fd = cnf->chan->fds[0];    /* for use by conf_play() */
            }
            else
            {
                cw_log(LOG_WARNING, "Unable to open pseudo channel - trying device\n");
                cnf->fd = open("/dev/zap/pseudo", O_RDWR);
                if (cnf->fd < 0)
                {
                    cw_log(LOG_WARNING, "Unable to open pseudo device\n");
                    free(cnf);
                    cnf = NULL;
                    goto cnfout;
                }
            }
            memset(&ztc, 0, sizeof(ztc));
            /* Setup a new zap conference */
            ztc.chan = 0;
            ztc.confno = -1;
            ztc.confmode = ZT_CONF_CONFANN | ZT_CONF_CONFANNMON;
            if (ioctl(cnf->fd, ZT_SETCONF, &ztc))
            {
                cw_log(LOG_WARNING, "Error setting conference\n");
                if (cnf->chan)
                    cw_hangup(cnf->chan);
                else
                    close(cnf->fd);
                free(cnf);
                cnf = NULL;
                goto cnfout;
            }
            /* Fill the conference struct */
            cnf->start = time(NULL);
            cnf->zapconf = ztc.confno;
            cnf->isdynamic = dynamic;
            cnf->firstuser = NULL;
            cnf->lastuser = NULL;
            cnf->locked = 0;
            if (option_verbose > 2)
                cw_verbose(VERBOSE_PREFIX_3 "Created MeetMe conference %d for conference '%s'\n", cnf->zapconf, cnf->confno);
            cnf->next = confs;
            confs = cnf;
        }
        else
            cw_log(LOG_WARNING, "Out of memory\n");
    }
cnfout:
    cw_mutex_unlock(&conflock);
    return cnf;
}

static int confs_show(int fd, int argc, char **argv)
{
    cw_cli(fd, "Deprecated! Please use 'meetme' instead.\n");
    return RESULT_SUCCESS;
}

static char show_confs_usage[] =
    "Deprecated! Please use 'meetme' instead.\n";

static struct cw_cli_entry cli_show_confs =
{
    {"show", "conferences", NULL},
    confs_show,
    "Show status of conferences",
    show_confs_usage,
    NULL
};

static int conf_cmd(int fd, int argc, char **argv)
{
    /* Process the command */
    char buf[1024] = "";
    char *header_format = "%-14s %-14s %-10s %-8s  %-8s\n";
    char *data_format = "%-12.12s   %4.4d          %4.4s       %02d:%02d:%02d  %-8s\n";
    struct cw_conference *cnf;
    struct cw_conf_user *user;
    int hr, min, sec;
    int i = 0, total = 0;
    time_t now;

    if (argc > 8)
        cw_cli(fd, "Invalid Arguments.\n");
    /* Check for length so no buffer will overflow... */
    for (i = 0; i < argc; i++)
    {
        if (strlen(argv[i]) > 100)
            cw_cli(fd, "Invalid Arguments.\n");
    }
    if (argc == 1)
    {
        /* 'MeetMe': List all the conferences */
        now = time(NULL);
        cnf = confs;
        if (!cnf)
        {
            cw_cli(fd, "No active MeetMe conferences.\n");
            return RESULT_SUCCESS;
        }
        cw_cli(fd, header_format, "Conf Num", "Parties", "Marked", "Activity", "Creation");
        while (cnf)
        {
            if (cnf->markedusers == 0)
                strcpy(buf, "N/A ");
            else
                snprintf(buf, sizeof(buf), "%4.4d", cnf->markedusers);
            hr = (now - cnf->start) / 3600;
            min = ((now - cnf->start) % 3600) / 60;
            sec = (now - cnf->start) % 60;

            cw_cli(fd, data_format, cnf->confno, cnf->users, buf, hr, min, sec, cnf->isdynamic ? "Dynamic" : "Static");

            total += cnf->users;
            cnf = cnf->next;
        }
        cw_cli(fd, "* Total number of MeetMe users: %d\n", total);
        return RESULT_SUCCESS;
    }
    if (argc < 3)
        return RESULT_SHOWUSAGE;

    if (strstr(argv[1], "lock"))
    {
        argv[3] = (strcmp(argv[1], "lock") == 0 ? argv[3] = "L" : "l");
        argc = 2;
    }
    else if (strstr(argv[1], "mute"))
    {
        if (argc < 4)
            return RESULT_SHOWUSAGE;
        if (strcmp(argv[1], "mute") == 0)
        {
            /* Mute */
            if (strcmp(argv[3], "all") == 0)
            {
                argv[3] = "N";
                argc = 2;
            }
            else
            {
                argv[4] = argv[3];
                argv[3] = "M";
                argc = 3;
            }
        }
        else
        {
            /* Unmute */
            if (strcmp(argv[3], "all") == 0)
            {
                argv[3] = "n";
                argc = 2;
            }
            else
            {
                argv[4] = argv[3];
                argv[3] = "m";
                argc = 3;
            }
        }
    }
    else if (strcmp(argv[1], "kick") == 0)
    {
        if (argc < 4)
            return RESULT_SHOWUSAGE;
        if (strcmp(argv[3], "all") == 0)
        {
            /* Kick all */
            argv[3] = "K";
            argc = 2;
        }
        else
        {
            /* Kick a single user */
            argv[4] = argv[3];
            argv[3] = "k";
            argc = 3;
        }
    }
    else if(strcmp(argv[1], "list") == 0)
    {
        /* List all the users in a conference */
        if (!confs)
        {
            cw_cli(fd, "No active conferences.\n");
            return RESULT_SUCCESS;
        }
        cnf = confs;
        /* Find the right conference */
        while (cnf)
        {
            if (strcmp(cnf->confno, argv[2]) == 0)
                break;
            if (cnf->next)
            {
                cnf = cnf->next;
            }
            else
            {
                cw_cli(fd, "No such conference: %s.\n",argv[2]);
                return RESULT_SUCCESS;
            }
        }
        /* Show all the users */
        user = cnf->firstuser;
        while (user)
        {
            cw_cli(fd, "User #: %-2.2d %12.12s %-20.20s Channel: %s %s %s %s %s\n", user->user_no, user->chan->cid.cid_num ? user->chan->cid.cid_num : "<unknown>", user->chan->cid.cid_name ? user->chan->cid.cid_name : "<no name>", user->chan->name, (user->userflags & CONFFLAG_ADMIN) ? "(Admin)" : "", (user->userflags & CONFFLAG_MONITOR) ? "(Listen only)" : "", (user->adminflags & ADMINFLAG_MUTED) ? "(Admn Muted)" : "", istalking(user->talking));
            user = user->nextuser;
        }
        cw_cli(fd,"%d users in that conference.\n",cnf->users);
        return RESULT_SUCCESS;
    }
    else
        return RESULT_SHOWUSAGE;

    admin_exec(NULL, argc, argv + 2);
    return 0;
}

static char *complete_confcmd(char *line, char *word, int pos, int state)
{
#define CONF_COMMANDS 6
    int which = 0, x = 0;
    struct cw_conference *cnf = NULL;
    struct cw_conf_user *usr = NULL;
    char *confno = NULL;
    char usrno[50] = "";
    char cmds[CONF_COMMANDS][20] = {"lock", "unlock", "mute", "unmute", "kick", "list"};
    char *myline;

    if (pos == 1)
    {
        /* Command */
        for (x = 0;x < CONF_COMMANDS; x++)
        {
            if (!strncasecmp(cmds[x], word, strlen(word)))
            {
                if (++which > state)
                {
                    return strdup(cmds[x]);
                }
            }
        }
    }
    else if (pos == 2)
    {
        /* Conference Number */
        cw_mutex_lock(&conflock);
        cnf = confs;
        while (cnf)
        {
            if (!strncasecmp(word, cnf->confno, strlen(word)))
            {
                if (++which > state)
                    break;
            }
            cnf = cnf->next;
        }
        cw_mutex_unlock(&conflock);
        return cnf ? strdup(cnf->confno) : NULL;
    }
    else if (pos == 3)
    {
        /* User Number || Conf Command option*/
        if (strstr(line, "mute") || strstr(line, "kick"))
        {
            if ((state == 0) && (strstr(line, "kick") || strstr(line,"mute")) && !(strncasecmp(word, "all", strlen(word))))
            {
                return strdup("all");
            }
            which++;
            cw_mutex_lock(&conflock);
            cnf = confs;

            /* TODO: Find the conf number from the cmdline (ignore spaces) <- test this and make it fail-safe! */
            myline = cw_strdupa(line);
            if (strsep(&myline, " ") && strsep(&myline, " ") && !confno)
            {
                while ((confno = strsep(&myline, " ")) && (strcmp(confno, " ") == 0))
                    ;
            }

            while (cnf)
            {
                if (strcmp(confno, cnf->confno) == 0)
                    break;
                cnf = cnf->next;
            }
            if (cnf)
            {
                /* Search for the user */
                usr = cnf->firstuser;
                while (usr)
                {
                    snprintf(usrno, sizeof(usrno), "%d", usr->user_no);
                    if (!strncasecmp(word, usrno, strlen(word)))
                    {
                        if (++which > state)
                            break;
                    }
                    usr = usr->nextuser;
                }
            }
            cw_mutex_unlock(&conflock);
            return usr ? strdup(usrno) : NULL;
        }
    }
    return NULL;
}

static char conf_usage[] =
    "Usage: meetme  (un)lock|(un)mute|kick|list <confno> <usernumber>\n"
    "       Executes a command for the conference or on a conferee\n";

static struct cw_cli_entry cli_conf =
    {
        { "meetme", NULL, NULL
        }
        , conf_cmd,
        "Execute a command on a conference or conferee", conf_usage, complete_confcmd
    };

static void conf_flush(int fd)
{
    int x;
    x = ZT_FLUSH_ALL;
    if (ioctl(fd, ZT_FLUSH, &x))
        cw_log(LOG_WARNING, "Error flushing channel\n");
}

/* Remove the conference from the list and free it.
   We assume that this was called while holding conflock. */
static int conf_free(struct cw_conference *conf)
{
    struct cw_conference *prev = NULL, *cur = confs;

    while (cur)
    {
        if (cur == conf)
        {
            if (prev)
                prev->next = conf->next;
            else
                confs = conf->next;
            break;
        }
        prev = cur;
        cur = cur->next;
    }

    if (!cur)
        cw_log(LOG_WARNING, "Conference not found\n");

    if (conf->recording == MEETME_RECORD_ACTIVE)
    {
        conf->recording = MEETME_RECORD_TERMINATE;
        cw_mutex_unlock(&conflock);
        while (1)
        {
            cw_mutex_lock(&conflock);
            if (conf->recording == MEETME_RECORD_OFF)
                break;
            cw_mutex_unlock(&conflock);
        }
    }

    if (conf->chan)
        cw_hangup(conf->chan);
    else
        close(conf->fd);

    free(conf);

    return 0;
}

static int conf_run(struct cw_channel *chan, struct cw_conference *conf, int confflags)
{
    struct cw_conf_user *user = malloc(sizeof(struct cw_conf_user));
    struct cw_conf_user *usr = NULL;
    int fd;
    struct zt_confinfo ztc, ztc_empty;
    struct cw_frame *f;
    struct cw_channel *c;
    struct cw_frame fr;
    int outfd;
    int ms;
    int nfds;
    int res;
    int flags;
    int retryzap;
    int origfd;
    int musiconhold = 0;
    int firstpass = 0;
    int origquiet;
    int lastmarked = 0;
    int currentmarked = 0;
    int ret = -1;
    int x;
    int menu_active = 0;
    int using_pseudo = 0;
    int duration=20;
    struct cw_dsp *dsp=NULL;

    struct cw_app *app;
    char *ogifile;
    char *ogifiledefault = "conf-background.ogi";
    char meetmesecs[30] = "";
    char exitcontext[CW_MAX_CONTEXT] = "";
    char recordingtmp[CW_MAX_EXTENSION] = "";
    int dtmf;

    ZT_BUFFERINFO bi;
    char __buf[CONF_SIZE + CW_FRIENDLY_OFFSET];
    char *buf = __buf + CW_FRIENDLY_OFFSET;

    if (!user)
    {
        cw_log(LOG_ERROR, "Out of memory\n");
        return(ret);
    }
    memset(user, 0, sizeof(struct cw_conf_user));

    if (confflags & CONFFLAG_RECORDCONF && conf->recording !=MEETME_RECORD_ACTIVE)
    {
        conf->recordingfilename = pbx_builtin_getvar_helper(chan,"MEETME_RECORDINGFILE");
        if (!conf->recordingfilename)
        {
            snprintf(recordingtmp,sizeof(recordingtmp),"meetme-conf-rec-%s-%s",conf->confno,chan->uniqueid);
            conf->recordingfilename = cw_strdupa(recordingtmp);
        }
        conf->recordingformat = pbx_builtin_getvar_helper(chan, "MEETME_RECORDINGFORMAT");
        if (!conf->recordingformat)
        {
            snprintf(recordingtmp,sizeof(recordingtmp), "wav");
            conf->recordingformat = cw_strdupa(recordingtmp);
        }
        pthread_attr_init(&conf->attr);
        pthread_attr_setdetachstate(&conf->attr, PTHREAD_CREATE_DETACHED);
        cw_verbose(VERBOSE_PREFIX_4 "Starting recording of MeetMe Conference %s into file %s.%s.\n", conf->confno, conf->recordingfilename, conf->recordingformat);
        cw_pthread_create(&conf->recordthread, &conf->attr, recordthread, conf);
    }

    user->user_no = 0; /* User number 0 means starting up user! (dead - not in the list!) */

    time(&user->jointime);

    if (conf->locked)
    {
        /* Sorry, but this confernce is locked! */
        if (!cw_streamfile(chan, "conf-locked", chan->language))
            cw_waitstream(chan, "");
        goto outrun;
    }

    if (confflags & CONFFLAG_MARKEDUSER)
        conf->markedusers++;

    cw_mutex_lock(&conflock);
    if (conf->firstuser == NULL)
    {
        /* Fill the first new User struct */
        user->user_no = 1;
        user->nextuser = NULL;
        user->prevuser = NULL;
        conf->firstuser = user;
        conf->lastuser = user;
    }
    else
    {
        /* Fill the new user struct */
        user->user_no = conf->lastuser->user_no + 1;
        user->prevuser = conf->lastuser;
        user->nextuser = NULL;
        if (conf->lastuser->nextuser != NULL)
        {
            cw_log(LOG_WARNING, "Error in User Management!\n");
            cw_mutex_unlock(&conflock);
            goto outrun;
        }
        else
        {
            conf->lastuser->nextuser = user;
            conf->lastuser = user;
        }
    }
    user->chan = chan;
    user->userflags = confflags;
    user->adminflags = 0;
    user->talking = -1;
    cw_mutex_unlock(&conflock);
    origquiet = confflags & CONFFLAG_QUIET;
    if (confflags & CONFFLAG_EXIT_CONTEXT)
    {
        if ((ogifile = pbx_builtin_getvar_helper(chan, "MEETME_EXIT_CONTEXT")))
            cw_copy_string(exitcontext, ogifile, sizeof(exitcontext));
        else if (!cw_strlen_zero(chan->proc_context))
            cw_copy_string(exitcontext, chan->proc_context, sizeof(exitcontext));
        else
            cw_copy_string(exitcontext, chan->context, sizeof(exitcontext));
    }

    if (!(confflags & CONFFLAG_QUIET) && (confflags & CONFFLAG_INTROUSER))
    {
        snprintf(user->namerecloc,sizeof(user->namerecloc),"%s/meetme/meetme-username-%s-%d",cw_config_CW_SPOOL_DIR,conf->confno,user->user_no);
        cw_record_review(chan,"vm-rec-name",user->namerecloc, 10,"sln", &duration, NULL);
    }

    conf->users++;

    if (!(confflags & CONFFLAG_QUIET))
    {
        if (conf->users == 1 && !(confflags & CONFFLAG_WAITMARKED))
            if (!cw_streamfile(chan, "conf-onlyperson", chan->language))
                cw_waitstream(chan, "");
        if ((confflags & CONFFLAG_WAITMARKED) && conf->markedusers == 0)
            if (!cw_streamfile(chan, "conf-waitforleader", chan->language))
                cw_waitstream(chan, "");
    }

    if (!(confflags & CONFFLAG_QUIET) && (confflags & CONFFLAG_ANNOUNCEUSERCOUNT) && conf->users > 1)
    {
        int keepplaying=1;

        if (conf->users == 2)
        {
            if (!cw_streamfile(chan,"conf-onlyone",chan->language))
            {
                res = cw_waitstream(chan, CW_DIGIT_ANY);
                if (res > 0)
                    keepplaying=0;
                else if (res == -1)
                    goto outrun;
            }
        }
        else
        {
            if (!cw_streamfile(chan, "conf-thereare", chan->language))
            {
                res = cw_waitstream(chan, CW_DIGIT_ANY);
                if (res > 0)
                    keepplaying=0;
                else if (res == -1)
                    goto outrun;
            }
            if (keepplaying)
            {
                res = cw_say_number(chan, conf->users - 1, CW_DIGIT_ANY, chan->language, (char *) NULL);
                if (res > 0)
                    keepplaying=0;
                else if (res == -1)
                    goto outrun;
            }
            if (keepplaying && !cw_streamfile(chan, "conf-otherinparty", chan->language))
            {
                res = cw_waitstream(chan, CW_DIGIT_ANY);
                if (res > 0)
                    keepplaying=0;
                else if (res == -1)
                    goto outrun;
            }
        }
    }

    /* Set it into linear mode (write) */
    if (cw_set_write_format(chan, CW_FORMAT_SLINEAR) < 0)
    {
        cw_log(LOG_WARNING, "Unable to set '%s' to write linear mode\n", chan->name);
        goto outrun;
    }

    /* Set it into linear mode (read) */
    if (cw_set_read_format(chan, CW_FORMAT_SLINEAR) < 0)
    {
        cw_log(LOG_WARNING, "Unable to set '%s' to read linear mode\n", chan->name);
        goto outrun;
    }
    cw_indicate(chan, -1);
    retryzap = strcasecmp(chan->type, "Zap");
    user->zapchannel = !retryzap;
zapretry:
    origfd = chan->fds[0];
    if (retryzap)
    {
        fd = open("/dev/zap/pseudo", O_RDWR);
        if (fd < 0)
        {
            cw_log(LOG_WARNING, "Unable to open pseudo channel: %s\n", strerror(errno));
            goto outrun;
        }
        using_pseudo = 1;
        /* Make non-blocking */
        flags = fcntl(fd, F_GETFL);
        if (flags < 0)
        {
            cw_log(LOG_WARNING, "Unable to get flags: %s\n", strerror(errno));
            close(fd);
            goto outrun;
        }
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK))
        {
            cw_log(LOG_WARNING, "Unable to set flags: %s\n", strerror(errno));
            close(fd);
            goto outrun;
        }
        /* Setup buffering information */
        memset(&bi, 0, sizeof(bi));
        bi.bufsize = CONF_SIZE/2;
        bi.txbufpolicy = ZT_POLICY_IMMEDIATE;
        bi.rxbufpolicy = ZT_POLICY_IMMEDIATE;
        bi.numbufs = 4;
        if (ioctl(fd, ZT_SET_BUFINFO, &bi))
        {
            cw_log(LOG_WARNING, "Unable to set buffering information: %s\n", strerror(errno));
            close(fd);
            goto outrun;
        }
        x = 1;
        if (ioctl(fd, ZT_SETLINEAR, &x))
        {
            cw_log(LOG_WARNING, "Unable to set linear mode: %s\n", strerror(errno));
            close(fd);
            goto outrun;
        }
        nfds = 1;
    }
    else
    {
        /* XXX Make sure we're not running on a pseudo channel XXX */
        fd = chan->fds[0];
        nfds = 0;
    }
    memset(&ztc, 0, sizeof(ztc));
    memset(&ztc_empty, 0, sizeof(ztc_empty));
    /* Check to see if we're in a conference... */
    ztc.chan = 0;
    if (ioctl(fd, ZT_GETCONF, &ztc))
    {
        cw_log(LOG_WARNING, "Error getting conference\n");
        close(fd);
        goto outrun;
    }
    if (ztc.confmode)
    {
        /* Whoa, already in a conference...  Retry... */
        if (!retryzap)
        {
            cw_log(LOG_DEBUG, "Zap channel is in a conference already, retrying with pseudo\n");
            retryzap = 1;
            goto zapretry;
        }
    }
    memset(&ztc, 0, sizeof(ztc));
    /* Add us to the conference */
    ztc.chan = 0;
    ztc.confno = conf->zapconf;
    cw_mutex_lock(&conflock);
    if (!(confflags & CONFFLAG_QUIET) && (confflags & CONFFLAG_INTROUSER) && conf->users > 1)
    {
        if (conf->chan && cw_fileexists(user->namerecloc, NULL, NULL))
        {
            if (!cw_streamfile(conf->chan, user->namerecloc, chan->language))
                cw_waitstream(conf->chan, "");
            if (!cw_streamfile(conf->chan, "conf-hasjoin", chan->language))
                cw_waitstream(conf->chan, "");
        }
    }

    if (confflags & CONFFLAG_MONITOR)
        ztc.confmode = ZT_CONF_CONFMON | ZT_CONF_LISTENER;
    else if (confflags & CONFFLAG_TALKER)
        ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER;
    else
        ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER | ZT_CONF_LISTENER;

    if (ioctl(fd, ZT_SETCONF, &ztc))
    {
        cw_log(LOG_WARNING, "Error setting conference\n");
        close(fd);
        cw_mutex_unlock(&conflock);
        goto outrun;
    }
    cw_log(LOG_DEBUG, "Placed channel %s in ZAP conf %d\n", chan->name, conf->zapconf);

    manager_event(EVENT_FLAG_CALL, "MeetmeJoin",
                  "Channel: %s\r\n"
                  "Uniqueid: %s\r\n"
                  "Meetme: %s\r\n"
                  "Usernum: %d\r\n",
                  chan->name, chan->uniqueid, conf->confno, user->user_no);

    if (!firstpass && !(confflags & CONFFLAG_MONITOR) && !(confflags & CONFFLAG_ADMIN))
    {
        firstpass = 1;
        if (!(confflags & CONFFLAG_QUIET))
            if (!(confflags & CONFFLAG_WAITMARKED) || (conf->markedusers >= 1))
                conf_play(chan, conf, ENTER);
    }
    conf_flush(fd);
    cw_mutex_unlock(&conflock);
    if (confflags & CONFFLAG_OGI)
    {

        if (user->zapchannel)
        {
            /*  Set CONFMUTE mode on Zap channel to mute DTMF tones */
            x = 1;
            cw_channel_setoption(chan,CW_OPTION_TONE_VERIFY,&x,sizeof(char),0);
        }
        /* Find a pointer to the ogi app and execute the script */
        app = pbx_findapp("OGI");
        if (app)
        {
            /* Get name of OGI file to run from $(MEETME_OGI_BACKGROUND)
              or use default filename of conf-background.ogi */
            ogifile = pbx_builtin_getvar_helper(chan,"MEETME_OGI_BACKGROUND");
            ogifile = strdup(ogifile ? ogifile : ogifiledefault);
            ret = pbx_exec(chan, app, ogifile);
            free(ogifile);
        }
        else
        {
            cw_log(LOG_WARNING, "Could not find application (ogi)\n");
            ret = -2;
        }
        if (user->zapchannel)
        {
            /*  Remove CONFMUTE mode on Zap channel */
            x = 0;
            cw_channel_setoption(chan,CW_OPTION_TONE_VERIFY,&x,sizeof(char),0);
        }
    }
    else
    {
        if (user->zapchannel && (confflags & CONFFLAG_STARMENU))
        {
            /*  Set CONFMUTE mode on Zap channel to mute DTMF tones when the menu is enabled */
            x = 1;
            cw_channel_setoption(chan,CW_OPTION_TONE_VERIFY,&x,sizeof(char),0);
        }
        if (confflags &  CONFFLAG_MONITORTALKER && !(dsp = cw_dsp_new()))
        {
            cw_log(LOG_WARNING, "Unable to allocate DSP!\n");
            res = -1;
        }
        for(;;)
        {
            int menu_was_active = 0;

            outfd = -1;
            ms = -1;

            /* if we have just exited from the menu, and the user had a channel-driver
               volume adjustment, restore it
            */
            if (!menu_active  &&  menu_was_active  &&  user->listen.desired  &&  !user->listen.actual)
                set_talk_volume(user, user->listen.desired);

            menu_was_active = menu_active;

            currentmarked = conf->markedusers;
            if (!(confflags & CONFFLAG_QUIET)  &&  (confflags & CONFFLAG_MARKEDUSER) && (confflags & CONFFLAG_WAITMARKED) && lastmarked == 0)
            {
                if (currentmarked == 1 && conf->users > 1)
                {
                    cw_say_number(chan, conf->users - 1, CW_DIGIT_ANY, chan->language, (char *) NULL);
                    if (conf->users - 1 == 1)
                    {
                        if (!cw_streamfile(chan, "conf-userwilljoin", chan->language))
                            cw_waitstream(chan, "");
                    }
                    else
                    {
                        if (!cw_streamfile(chan, "conf-userswilljoin", chan->language))
                            cw_waitstream(chan, "");
                    }
                }
                if (conf->users == 1 && ! (confflags & CONFFLAG_MARKEDUSER))
                    if (!cw_streamfile(chan, "conf-onlyperson", chan->language))
                        cw_waitstream(chan, "");
            }

            c = cw_waitfor_nandfds(&chan, 1, &fd, nfds, NULL, &outfd, &ms);

            /* Update the struct with the actual confflags */
            user->userflags = confflags;

            if (confflags & CONFFLAG_WAITMARKED)
            {
                if(currentmarked == 0)
                {
                    if (lastmarked != 0)
                    {
                        if (!(confflags & CONFFLAG_QUIET))
                            if (!cw_streamfile(chan, "conf-leaderhasleft", chan->language))
                                cw_waitstream(chan, "");
                        if(confflags & CONFFLAG_MARKEDEXIT)
                            break;
                        else
                        {
                            ztc.confmode = ZT_CONF_CONF;
                            if (ioctl(fd, ZT_SETCONF, &ztc))
                            {
                                cw_log(LOG_WARNING, "Error setting conference\n");
                                close(fd);
                                goto outrun;
                            }
                        }
                    }
                    if (musiconhold == 0 && (confflags & CONFFLAG_MOH))
                    {
                        cw_moh_start(chan, NULL);
                        musiconhold = 1;
                    }
                    else
                    {
                        ztc.confmode = ZT_CONF_CONF;
                        if (ioctl(fd, ZT_SETCONF, &ztc))
                        {
                            cw_log(LOG_WARNING, "Error setting conference\n");
                            close(fd);
                            goto outrun;
                        }
                    }
                }
                else if(currentmarked >= 1 && lastmarked == 0)
                {
                    if (confflags & CONFFLAG_MONITOR)
                        ztc.confmode = ZT_CONF_CONFMON | ZT_CONF_LISTENER;
                    else if (confflags & CONFFLAG_TALKER)
                        ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER;
                    else
                        ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER | ZT_CONF_LISTENER;
                    if (ioctl(fd, ZT_SETCONF, &ztc))
                    {
                        cw_log(LOG_WARNING, "Error setting conference\n");
                        close(fd);
                        goto outrun;
                    }
                    if (musiconhold && (confflags & CONFFLAG_MOH))
                    {
                        cw_moh_stop(chan);
                        musiconhold = 0;
                    }
                    if ( !(confflags & CONFFLAG_QUIET) && !(confflags & CONFFLAG_MARKEDUSER))
                    {
                        if (!cw_streamfile(chan, "conf-placeintoconf", chan->language))
                            cw_waitstream(chan, "");
                        conf_play(chan, conf, ENTER);
                    }
                }
            }

            /* trying to add moh for single person conf */
            if ((confflags & CONFFLAG_MOH) && !(confflags & CONFFLAG_WAITMARKED))
            {
                if (conf->users == 1)
                {
                    if (musiconhold == 0)
                    {
                        cw_moh_start(chan, NULL);
                        musiconhold = 1;
                    }
                }
                else
                {
                    if (musiconhold)
                    {
                        cw_moh_stop(chan);
                        musiconhold = 0;
                    }
                }
            }

            /* Leave if the last marked user left */
            if (currentmarked == 0 && lastmarked != 0 && (confflags & CONFFLAG_MARKEDEXIT))
            {
                ret = -1;
                break;
            }

            /* Check if the admin changed my modes */
            if (user->adminflags)
            {
                /* Set the new modes */
                if ((user->adminflags & ADMINFLAG_MUTED) && (ztc.confmode & ZT_CONF_TALKER))
                {
                    ztc.confmode ^= ZT_CONF_TALKER;
                    if (ioctl(fd, ZT_SETCONF, &ztc))
                    {
                        cw_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
                        ret = -1;
                        break;
                    }
                }
                if (!(user->adminflags & ADMINFLAG_MUTED) && !(confflags & CONFFLAG_MONITOR) && !(ztc.confmode & ZT_CONF_TALKER))
                {
                    ztc.confmode |= ZT_CONF_TALKER;
                    if (ioctl(fd, ZT_SETCONF, &ztc))
                    {
                        cw_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
                        ret = -1;
                        break;
                    }
                }
                if (user->adminflags & ADMINFLAG_KICKME)
                {
                    /* You have been kicked. */
                    if (!cw_streamfile(chan, "conf-kicked", chan->language))
                        cw_waitstream(chan, "");
                    ret = 0;
                    break;
                }
            }
            else if (!(confflags & CONFFLAG_MONITOR) && !(ztc.confmode & ZT_CONF_TALKER))
            {
                ztc.confmode |= ZT_CONF_TALKER;
                if (ioctl(fd, ZT_SETCONF, &ztc))
                {
                    cw_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
                    ret = -1;
                    break;
                }
            }

            if (c)
            {
                if (c->fds[0] != origfd)
                {
                    if (using_pseudo)
                    {
                        /* Kill old pseudo */
                        close(fd);
                        using_pseudo = 0;
                    }
                    cw_log(LOG_DEBUG, "Ooh, something swapped out under us, starting over\n");
                    retryzap = strcasecmp(c->type, "Zap");
                    user->zapchannel = !retryzap;
                    goto zapretry;
                }
                f = cw_read(c);
                if (!f)
                    break;
                if ((f->frametype == CW_FRAME_VOICE) && (f->subclass == CW_FORMAT_SLINEAR))
                {
                    if (user->talk.actual)
                        cw_frame_adjust_volume(f, user->talk.actual);

                    if (confflags &  CONFFLAG_MONITORTALKER)
                    {
                        int totalsilence;
                        if (user->talking == -1)
                            user->talking = 0;

                        res = cw_dsp_silence(dsp, f, &totalsilence);
                        if (!user->talking && totalsilence < MEETME_DELAYDETECTTALK)
                        {
                            user->talking = 1;
                            manager_event(EVENT_FLAG_CALL, "MeetmeTalking",
                                          "Channel: %s\r\n"
                                          "Uniqueid: %s\r\n"
                                          "Meetme: %s\r\n"
                                          "Usernum: %d\r\n",
                                          chan->name, chan->uniqueid, conf->confno, user->user_no);
                        }
                        if (user->talking && totalsilence > MEETME_DELAYDETECTENDTALK)
                        {
                            user->talking = 0;
                            manager_event(EVENT_FLAG_CALL, "MeetmeStopTalking",
                                          "Channel: %s\r\n"
                                          "Uniqueid: %s\r\n"
                                          "Meetme: %s\r\n"
                                          "Usernum: %d\r\n",
                                          chan->name, chan->uniqueid, conf->confno, user->user_no);
                        }
                    }
                    if (using_pseudo)
                    {
                        /* Carefully write */
                        careful_write(fd, f->data, f->datalen);
                    }
                }
                else if ((f->frametype == CW_FRAME_DTMF) && (confflags & CONFFLAG_EXIT_CONTEXT))
                {
                    char tmp[2];

                    tmp[0] = f->subclass;
                    tmp[1] = '\0';
                    if (cw_goto_if_exists(chan, exitcontext, tmp, 1))
                    {
                        ret = 0;
                        break;
                    }
                    else if (option_debug > 1)
                        cw_log(LOG_DEBUG, "Exit by single digit did not work in meetme. Extension %s does not exist in context %s\n", tmp, exitcontext);
                }
                else if ((f->frametype == CW_FRAME_DTMF) && (f->subclass == '#') && (confflags & CONFFLAG_POUNDEXIT))
                {
                    ret = 0;
                    break;
                }
                else if (((f->frametype == CW_FRAME_DTMF) && (f->subclass == '*') && (confflags & CONFFLAG_STARMENU)) || ((f->frametype == CW_FRAME_DTMF) && menu_active))
                {
                    if (ioctl(fd, ZT_SETCONF, &ztc_empty))
                    {
                        cw_log(LOG_WARNING, "Error setting conference\n");
                        close(fd);
                        cw_mutex_unlock(&conflock);
                        goto outrun;
                    }

                    /* if we are entering the menu, and the user has a channel-driver
                       volume adjustment, clear it
                    */
                    if (!menu_active && user->talk.desired && !user->talk.actual)
                        set_talk_volume(user, 0);

                    if (musiconhold)
                        cw_moh_stop(chan);
                    if ((confflags & CONFFLAG_ADMIN))
                    {
                        /* Admin menu */
                        if (!menu_active)
                        {
                            menu_active = 1;
                            /* Record this sound! */
                            if (!cw_streamfile(chan, "conf-adminmenu", chan->language))
                                dtmf = cw_waitstream(chan, CW_DIGIT_ANY);
                            else
                                dtmf = 0;
                        }
                        else
                        {
                            dtmf = f->subclass;
                        }
                        if (dtmf)
                        {
                            switch (dtmf)
                            {
                            case '1': /* Un/Mute */
                                menu_active = 0;
                                if (ztc.confmode & ZT_CONF_TALKER)
                                {
                                    ztc.confmode = ZT_CONF_CONF | ZT_CONF_LISTENER;
                                    confflags |= CONFFLAG_MONITOR ^ CONFFLAG_TALKER;
                                }
                                else
                                {
                                    ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER | ZT_CONF_LISTENER;
                                    confflags ^= CONFFLAG_MONITOR | CONFFLAG_TALKER;
                                }
                                if (ioctl(fd, ZT_SETCONF, &ztc))
                                {
                                    cw_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
                                    ret = -1;
                                    break;
                                }
                                if (ztc.confmode & ZT_CONF_TALKER)
                                {
                                    if (!cw_streamfile(chan, "conf-unmuted", chan->language))
                                        cw_waitstream(chan, "");
                                }
                                else
                                {
                                    if (!cw_streamfile(chan, "conf-muted", chan->language))
                                        cw_waitstream(chan, "");
                                }
                                break;
                            case '2': /* Un/Lock the Conference */
                                menu_active = 0;
                                if (conf->locked)
                                {
                                    conf->locked = 0;
                                    if (!cw_streamfile(chan, "conf-unlockednow", chan->language))
                                        cw_waitstream(chan, "");
                                }
                                else
                                {
                                    conf->locked = 1;
                                    if (!cw_streamfile(chan, "conf-lockednow", chan->language))
                                        cw_waitstream(chan, "");
                                }
                                break;
                            case '3': /* Eject last user */
                                menu_active = 0;
                                usr = conf->lastuser;
                                if ((usr->chan->name == chan->name)||(usr->userflags & CONFFLAG_ADMIN))
                                {
                                    if(!cw_streamfile(chan, "conf-errormenu", chan->language))
                                        cw_waitstream(chan, "");
                                }
                                else
                                    usr->adminflags |= ADMINFLAG_KICKME;
                                cw_stopstream(chan);
                                break;
                            case '4':
                                tweak_listen_volume(user, VOL_DOWN);
                                break;
                            case '5': /* en/disable flag marked to self as admin */
                                if (! (confflags & CONFFLAG_MARKEDUSER)) {
                                    conf->markedusers++;
                                    confflags ^= CONFFLAG_MARKEDUSER;
                                } else {
                                    conf->markedusers--;
                                    confflags |= CONFFLAG_MARKEDUSER;
				}
				break;
                            case '6':
                                tweak_listen_volume(user, VOL_UP);
                                break;
                            case '7':
                                tweak_talk_volume(user, VOL_DOWN);
                                break;
                            case '9':
                                tweak_talk_volume(user, VOL_UP);
                                break;
                            default:
                                menu_active = 0;
                                /* Play an error message! */
                                if (!cw_streamfile(chan, "conf-errormenu", chan->language))
                                    cw_waitstream(chan, "");
                                break;
                            }
                        }
                    }
                    else
                    {
                        /* User menu */
                        if (!menu_active)
                        {
                            menu_active = 1;
                            /* Record this sound! */
                            if (!cw_streamfile(chan, "conf-usermenu", chan->language))
                                dtmf = cw_waitstream(chan, CW_DIGIT_ANY);
                            else
                                dtmf = 0;
                        }
                        else
                        {
                            dtmf = f->subclass;
                        }
                        if (dtmf)
                        {
                            switch (dtmf)
                            {
                            case '1': /* Un/Mute */
                                menu_active = 0;
                                if (ztc.confmode & ZT_CONF_TALKER)
                                {
                                    ztc.confmode = ZT_CONF_CONF | ZT_CONF_LISTENER;
                                    confflags |= CONFFLAG_MONITOR ^ CONFFLAG_TALKER;
                                }
                                else if (!(user->adminflags & ADMINFLAG_MUTED))
                                {
                                    ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER | ZT_CONF_LISTENER;
                                    confflags ^= CONFFLAG_MONITOR | CONFFLAG_TALKER;
                                }
                                if (ioctl(fd, ZT_SETCONF, &ztc))
                                {
                                    cw_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
                                    ret = -1;
                                    break;
                                }
                                if (ztc.confmode & ZT_CONF_TALKER)
                                {
                                    if (!cw_streamfile(chan, "conf-unmuted", chan->language))
                                        cw_waitstream(chan, "");
                                }
                                else
                                {
                                    if (!cw_streamfile(chan, "conf-muted", chan->language))
                                        cw_waitstream(chan, "");
                                }
                                break;
                            case '4':
                                tweak_listen_volume(user, VOL_DOWN);
                                break;
                            case '6':
                                tweak_listen_volume(user, VOL_UP);
                                break;
                            case '7':
                                tweak_talk_volume(user, VOL_DOWN);
                                break;
                            case '8':
                                menu_active = 0;
                                break;
                            case '9':
                                tweak_talk_volume(user, VOL_UP);
                                break;
                            default:
                                menu_active = 0;
                                /* Play an error message! */
                                if (!cw_streamfile(chan, "conf-errormenu", chan->language))
                                    cw_waitstream(chan, "");
                                break;
                            }
                        }
                    }
                    if (musiconhold)
                        cw_moh_start(chan, NULL);

                    if (ioctl(fd, ZT_SETCONF, &ztc))
                    {
                        cw_log(LOG_WARNING, "Error setting conference\n");
                        close(fd);
                        cw_mutex_unlock(&conflock);
                        goto outrun;
                    }
                    conf_flush(fd);
                }
                else if (option_debug)
                {
                    cw_log(LOG_DEBUG, "Got unrecognized frame on channel %s, f->frametype=%d,f->subclass=%d\n",chan->name,f->frametype,f->subclass);
                }
                cw_fr_free(f);
            }
            else if (outfd > -1)
            {
                res = read(outfd, buf, CONF_SIZE);
                if (res > 0)
                {
                    cw_fr_init_ex(&fr, CW_FRAME_VOICE, CW_FORMAT_SLINEAR, NULL);
                    fr.datalen = res;
                    fr.samples = res/2;
                    fr.data = buf;
                    fr.offset = CW_FRIENDLY_OFFSET;
                    if (user->listen.actual)
                        cw_frame_adjust_volume(&fr, user->listen.actual);
                    if (cw_write(chan, &fr) < 0)
                    {
                        cw_log(LOG_WARNING, "Unable to write frame to channel: %s\n", strerror(errno));
                        /* break; */
                    }
                }
                else
                {
                    cw_log(LOG_WARNING, "Failed to read frame: %s\n", strerror(errno));
                }
            }
            lastmarked = currentmarked;
        }
    }
    if (using_pseudo)
        close(fd);
    else
    {
        /* Take out of conference */
        ztc.chan = 0;
        ztc.confno = 0;
        ztc.confmode = 0;
        if (ioctl(fd, ZT_SETCONF, &ztc))
        {
            cw_log(LOG_WARNING, "Error setting conference\n");
        }
    }

    reset_volumes(user);

    cw_mutex_lock(&conflock);
    if (!(confflags & CONFFLAG_QUIET) && !(confflags & CONFFLAG_MONITOR) && !(confflags & CONFFLAG_ADMIN))
        conf_play(chan, conf, LEAVE);

    if (!(confflags & CONFFLAG_QUIET) && (confflags & CONFFLAG_INTROUSER))
    {
        if (cw_fileexists(user->namerecloc, NULL, NULL))
        {
            if ((conf->chan) && (conf->users > 1))
            {
                if (!cw_streamfile(conf->chan, user->namerecloc, chan->language))
                    cw_waitstream(conf->chan, "");
                if (!cw_streamfile(conf->chan, "conf-hasleft", chan->language))
                    cw_waitstream(conf->chan, "");
            }
            cw_filedelete(user->namerecloc, NULL);
        }
    }
    cw_mutex_unlock(&conflock);


outrun:
    cw_mutex_lock(&conflock);
    if (confflags & CONFFLAG_MONITORTALKER && dsp)
        cw_dsp_free(dsp);

    if (user->user_no)
    { /* Only cleanup users who really joined! */
        manager_event(EVENT_FLAG_CALL, "MeetmeLeave",
                      "Channel: %s\r\n"
                      "Uniqueid: %s\r\n"
                      "Meetme: %s\r\n"
                      "Usernum: %d\r\n",
                      chan->name, chan->uniqueid, conf->confno, user->user_no);
        conf->users--;
        if (confflags & CONFFLAG_MARKEDUSER)
            conf->markedusers--;
        if (!conf->users)
        {
            /* No more users -- close this one out */
            conf_free(conf);
        }
        else
        {
            /* Remove the user struct */
            if (user == conf->firstuser)
            {
                if (user->nextuser)
                {
                    /* There is another entry */
                    user->nextuser->prevuser = NULL;
                }
                else
                {
                    /* We are the only entry */
                    conf->lastuser = NULL;
                }
                /* In either case */
                conf->firstuser = user->nextuser;
            }
            else if (user == conf->lastuser)
            {
                if (user->prevuser)
                    user->prevuser->nextuser = NULL;
                else
                    cw_log(LOG_ERROR, "Bad bad bad!  We're the last, not the first, but nobody before us??\n");
                conf->lastuser = user->prevuser;
            }
            else
            {
                if (user->nextuser)
                    user->nextuser->prevuser = user->prevuser;
                else
                    cw_log(LOG_ERROR, "Bad! Bad! Bad! user->nextuser is NULL but we're not the end!\n");
                if (user->prevuser)
                    user->prevuser->nextuser = user->nextuser;
                else
                    cw_log(LOG_ERROR, "Bad! Bad! Bad! user->prevuser is NULL but we're not the beginning!\n");
            }
        }
        /* Return the number of seconds the user was in the conf */
        snprintf(meetmesecs, sizeof(meetmesecs), "%d", (int) (time(NULL) - user->jointime));
        pbx_builtin_setvar_helper(chan, "MEETMESECS", meetmesecs);
    }
    free(user);
    cw_mutex_unlock(&conflock);
    return ret;
}

static struct cw_conference *find_conf(struct cw_channel *chan, char *confno, int make, int dynamic, char *dynamic_pin)
{
    struct cw_config *cfg;
    struct cw_variable *var;
    struct cw_conference *cnf;

    /* Check first in the conference list */
    cw_mutex_lock(&conflock);
    cnf = confs;
    while (cnf)
    {
        if (!strcmp(confno, cnf->confno))
            break;
        cnf = cnf->next;
    }
    cw_mutex_unlock(&conflock);

    if (!cnf)
    {
        if (dynamic)
        {
            /* No need to parse meetme.conf */
            cw_log(LOG_DEBUG, "Building dynamic conference '%s'\n", confno);
            if (dynamic_pin)
            {
                if (dynamic_pin[0] == 'q')
                {
                    /* Query the user to enter a PIN */
                    cw_app_getdata(chan, "conf-getpin", dynamic_pin, CW_MAX_EXTENSION - 1, 0);
                }
                cnf = build_conf(confno, dynamic_pin, "", make, dynamic);
            }
            else
            {
                cnf = build_conf(confno, "", "", make, dynamic);
            }
        }
        else
        {
            /* Check the config */
            cfg = cw_config_load("meetme.conf");
            if (!cfg)
            {
                cw_log(LOG_WARNING, "No meetme.conf file :(\n");
                return NULL;
            }
            var = cw_variable_browse(cfg, "rooms");
            while (var)
            {
                if (!strcasecmp(var->name, "conf"))
                {
                    /* Separate the PIN */
                    char *pin, *pinadmin, *conf;

                    pinadmin = cw_strdupa(var->value);
                    conf = strsep(&pinadmin, "|,");
                    pin = strsep(&pinadmin, "|,");
                    if (!strcasecmp(conf, confno))
                    {
                        /* Bingo it's a valid conference */
                        if (pin)
                            if (pinadmin)
                                cnf = build_conf(confno, pin, pinadmin, make, dynamic);
                            else
                                cnf = build_conf(confno, pin, "", make, dynamic);
                        else
                            if (pinadmin)
                                cnf = build_conf(confno, "", pinadmin, make, dynamic);
                            else
                                cnf = build_conf(confno, "", "", make, dynamic);
                        break;
                    }
                }
                var = var->next;
            }
            if (!var)
            {
                cw_log(LOG_DEBUG, "%s isn't a valid conference\n", confno);
            }
            cw_config_destroy(cfg);
        }
    }
    else if (dynamic_pin)
    {
        /* Correct for the user selecting 'D' instead of 'd' to have
           someone join into a conference that has already been created
           with a pin. */
        if (dynamic_pin[0] == 'q')
            dynamic_pin[0] = '\0';
    }
    return cnf;
}

/*--- count_exec: The MeetmeCount application */
static int count_exec(struct cw_channel *chan, int argc, char **argv)
{
    char val[80] = "0";
    struct localuser *u;
    int res = 0;
    struct cw_conference *conf;
    int count;

    if (argc < 1 || argc > 2)
    {
        cw_log(LOG_ERROR, "Syntax: %s\n", syntax2);
        return -1;
    }

    LOCAL_USER_ADD(u);

    conf = find_conf(chan, argv[0], 0, 0, NULL);
    if (conf)
        count = conf->users;
    else
        count = 0;

    if (argc > 1)
    {
        /* have var so load it and exit */
        snprintf(val, sizeof(val), "%d",count);
        pbx_builtin_setvar_helper(chan, argv[1], val);
    }
    else
    {
        if (chan->_state != CW_STATE_UP)
            cw_answer(chan);
        res = cw_say_number(chan, count, "", chan->language, (char *) NULL); /* Needs gender */
    }

    LOCAL_USER_REMOVE(u);
    return res;
}

/*--- conf_exec: The meetme() application */
static int conf_exec(struct cw_channel *chan, int argc, char **argv)
{
    char confno[CW_MAX_EXTENSION] = "";
    char the_pin[CW_MAX_EXTENSION] = "";
    struct cw_flags confflags =
        {
            0
        };
    struct cw_conference *cnf;
    struct localuser *u;
    int res=-1;
    int allowretry = 0;
    int retrycnt = 0;
    int dynamic = 0;
    int empty = 0, empty_no_pin = 0;
    int always_prompt = 0;

    if (argc > 3)
    {
        cw_log(LOG_ERROR, "Syntax: %s\n", syntax);
        return -1;
    }

    LOCAL_USER_ADD(u);

    if (argc == 0 || !argv[0][0])
        allowretry = 1;
    cw_copy_string(confno, argv[0], sizeof(confno));

    if (argc > 2)
        cw_copy_string(the_pin, argv[2], sizeof(the_pin));

    if (argc > 1 && argv[1][0])
    {
        cw_parseoptions(meetme_opts, &confflags, NULL, argv[1]);
        dynamic = cw_test_flag(&confflags, CONFFLAG_DYNAMIC | CONFFLAG_DYNAMICPIN);
        if (cw_test_flag(&confflags, CONFFLAG_DYNAMICPIN) && (argc < 3 || !argv[2][0]))
            strcpy(the_pin, "q");

        empty = cw_test_flag(&confflags, CONFFLAG_EMPTY | CONFFLAG_EMPTYNOPIN);
        empty_no_pin = cw_test_flag(&confflags, CONFFLAG_EMPTYNOPIN);
        always_prompt = cw_test_flag(&confflags, CONFFLAG_ALWAYSPROMPT);
    }

    if (chan->_state != CW_STATE_UP)
        cw_answer(chan);

    do
    {
        if (retrycnt > 3)
            allowretry = 0;
        if (empty)
        {
            int i, map[1024];
            struct cw_config *cfg;
            struct cw_variable *var;
            int confno_int;

            memset(map, 0, sizeof(map));

            cw_mutex_lock(&conflock);
            cnf = confs;
            while (cnf)
            {
                if (sscanf(cnf->confno, "%d", &confno_int) == 1)
                {
                    /* Disqualify in use conference */
                    if (confno_int >= 0 && confno_int < 1024)
                        map[confno_int]++;
                }
                cnf = cnf->next;
            }
            cw_mutex_unlock(&conflock);

            /* We only need to load the config file for static and empty_no_pin (otherwise we don't care) */
            if ((empty_no_pin) || (!dynamic))
            {
                cfg = cw_config_load("meetme.conf");
                if (cfg)
                {
                    var = cw_variable_browse(cfg, "rooms");
                    while (var)
                    {
                        if (!strcasecmp(var->name, "conf"))
                        {
                            char *stringp = cw_strdupa(var->value);
                            char *confno_tmp = strsep(&stringp, "|,");
                            int found = 0;
                            if (sscanf(confno_tmp, "%d", &confno_int) == 1)
                            {
                                if ((confno_int >= 0) && (confno_int < 1024))
                                {
                                    if (stringp && empty_no_pin)
                                    {
                                        map[confno_int]++;
                                    }
                                }
                            }
                            if (! dynamic)
                            {
                                /* For static:  run through the list and see if this conference is empty */
                                cw_mutex_lock(&conflock);
                                cnf = confs;
                                while (cnf)
                                {
                                    if (!strcmp(confno_tmp, cnf->confno))
                                    {
                                        /* The conference exists, therefore it's not empty */
                                        found = 1;
                                        break;
                                    }
                                    cnf = cnf->next;
                                }
                                cw_mutex_unlock(&conflock);
                                if (!found)
                                {
                                    /* At this point, we have a confno_tmp (static conference) that is empty */
                                    if ((empty_no_pin && ((!stringp) || (stringp && (stringp[0] == '\0')))) || (!empty_no_pin))
                                    {
                                        /* Case 1:  empty_no_pin and pin is nonexistent (NULL)
                                         * Case 2:  empty_no_pin and pin is blank (but not NULL)
                                         * Case 3:  not empty_no_pin
                                         */
                                        cw_copy_string(confno, confno_tmp, sizeof(confno));
                                        break;
                                        /* XXX the map is not complete (but we do have a confno) */
                                    }
                                }
                            }
                        }
                        var = var->next;
                    }
                    cw_config_destroy(cfg);
                }
            }
            /* Select first conference number not in use */
            if (cw_strlen_zero(confno) && dynamic)
            {
                for (i=0;i<1024;i++)
                {
                    if (!map[i])
                    {
                        snprintf(confno, sizeof(confno), "%d", i);
                        break;
                    }
                }
            }

            /* Not found? */
            if (cw_strlen_zero(confno))
            {
                res = cw_streamfile(chan, "conf-noempty", chan->language);
                if (!res)
                    cw_waitstream(chan, "");
            }
            else
            {
                if (sscanf(confno, "%d", &confno_int) == 1)
                {
                    res = cw_streamfile(chan, "conf-enteringno", chan->language);
                    if (!res)
                    {
                        cw_waitstream(chan, "");
                        res = cw_say_digits(chan, confno_int, "", chan->language);
                    }
                }
                else
                {
                    cw_log(LOG_ERROR, "Could not scan confno '%s'\n", confno);
                }
            }
        }
        while (allowretry && (cw_strlen_zero(confno)) && (++retrycnt < 4))
        {
            /* Prompt user for conference number */
            res = cw_app_getdata(chan, "conf-getconfno", confno, sizeof(confno) - 1, 0);
            if (res < 0)
            {
                /* Don't try to validate when we catch an error */
                confno[0] = '\0';
                allowretry = 0;
                break;
            }
        }
        if (confno[0])
        {
            /* Check the validity of the conference */
            cnf = find_conf(chan, confno, 1, dynamic, the_pin);
            if (!cnf)
            {
                res = cw_streamfile(chan, "conf-invalid", chan->language);
                if (!res)
                    cw_waitstream(chan, "");
                res = -1;
                if (allowretry)
                    confno[0] = '\0';
            }
            else
            {
                if ((!cw_strlen_zero(cnf->pin) &&  !cw_test_flag(&confflags, CONFFLAG_ADMIN)) || (!cw_strlen_zero(cnf->pinadmin) && cw_test_flag(&confflags, CONFFLAG_ADMIN)))
                {
                    char pin[CW_MAX_EXTENSION]="";
                    int j;

                    /* Allow the pin to be retried up to 3 times */
                    for (j=0; j<3; j++)
                    {
                        if (*the_pin && (always_prompt==0))
                        {
                            cw_copy_string(pin, the_pin, sizeof(pin));
                            res = 0;
                        }
                        else
                        {
                            /* Prompt user for pin if pin is required */
                            res = cw_app_getdata(chan, "conf-getpin", pin + strlen(pin), sizeof(pin) - 1 - strlen(pin), 0);
                        }
                        if (res >= 0)
                        {
                            if (!strcasecmp(pin, cnf->pin)  || (!cw_strlen_zero(cnf->pinadmin) && !strcasecmp(pin, cnf->pinadmin)))
                            {

                                /* Pin correct */
                                allowretry = 0;
                                if (!cw_strlen_zero(cnf->pinadmin) && !strcasecmp(pin, cnf->pinadmin))
                                    cw_set_flag(&confflags, CONFFLAG_ADMIN);
                                /* Run the conference */
                                res = conf_run(chan, cnf, confflags.flags);
                                break;
                            }
                            else
                            {
                                /* Pin invalid */
                                res = cw_streamfile(chan, "conf-invalidpin", chan->language);
                                if (!res)
                                    cw_waitstream(chan, CW_DIGIT_ANY);
                                if (res < 0)
                                    break;
                                pin[0] = res;
                                pin[1] = '\0';
                                res = -1;
                                if (allowretry)
                                    confno[0] = '\0';
                            }
                        }
                        else
                        {
                            /* failed when getting the pin */
                            res = -1;
                            allowretry = 0;
                            /* see if we need to get rid of the conference */
                            cw_mutex_lock(&conflock);
                            if (!cnf->users)
                            {
                                conf_free(cnf);
                            }
                            cw_mutex_unlock(&conflock);
                            break;
                        }

                        /* Don't retry pin with a static pin */
                        if (*the_pin && (always_prompt==0))
                        {
                            break;
                        }
                    }
                }
                else
                {
                    /* No pin required */
                    allowretry = 0;

                    /* Run the conference */
                    res = conf_run(chan, cnf, confflags.flags);
                }
            }
        }
    }
    while (allowretry);

    LOCAL_USER_REMOVE(u);

    return res;
}

static struct cw_conf_user* find_user(struct cw_conference *conf, char *callerident)
{
    struct cw_conf_user *user = NULL;
    char usrno[1024] = "";

    if (conf && callerident)
    {
        user = conf->firstuser;
        while (user)
        {
            snprintf(usrno, sizeof(usrno), "%d", user->user_no);
            if (strcmp(usrno, callerident) == 0)
                return user;
            user = user->nextuser;
        }
    }
    return NULL;
}

/*--- admin_exec: The MeetMeadmin application */
/* MeetMeAdmin(confno, command, caller) */
static int admin_exec(struct cw_channel *chan, int argc, char **argv)
{
    struct cw_conference *cnf;
    struct cw_conf_user *user;
    struct localuser *u;

    if (argc < 2  ||  argc > 3)
    {
        cw_log(LOG_ERROR, "Syntax: %s\n", syntax3);
        return -1;
    }

    LOCAL_USER_ADD(u);
    cw_mutex_lock(&conflock);

    cnf = confs;
    while (cnf)
    {
        if (strcmp(cnf->confno, argv[0]) == 0)
            break;
        cnf = cnf->next;
    }

    user = (argc > 2)  ?  find_user(cnf, argv[2])  :  NULL;

    if (cnf)
    {
        switch ((int) (*argv[1]))
        {
        case 'L':
            /* L: Lock */
            cnf->locked = 1;
            break;
        case 'l':
            /* l: Unlock */
            cnf->locked = 0;
            break;
        case 'K':
            /* K: kick all users*/
            user = cnf->firstuser;
            while (user)
            {
                user->adminflags |= ADMINFLAG_KICKME;
                if (user->nextuser)
                    user = user->nextuser;
                else
                    break;
            }
            break;
        case 'e':
            /* e: Eject last user*/
            user = cnf->lastuser;
            if (!(user->userflags & CONFFLAG_ADMIN))
            {
                user->adminflags |= ADMINFLAG_KICKME;
                break;
            }
            cw_log(LOG_NOTICE, "Not kicking last user, is an Admin!\n");
            break;
        case 'M':
            /* M: Mute */
            if (user)
                user->adminflags |= ADMINFLAG_MUTED;
            else
                cw_log(LOG_NOTICE, "Specified User not found!\n");
            break;
        case 'N':
            /* N: Mute all users */
            user = cnf->firstuser;
            while (user)
            {
                if (user && !(user->userflags & CONFFLAG_ADMIN))
                    user->adminflags |= ADMINFLAG_MUTED;
                if (user->nextuser)
                    user = user->nextuser;
                else
                    break;
            }
            break;
        case 'm':
            /* m: Unmute */
            if (user && (user->adminflags & ADMINFLAG_MUTED))
                user->adminflags ^= ADMINFLAG_MUTED;
            else
                cw_log(LOG_NOTICE, "Specified User not found or he muted himself!");
            break;
        case 'n':
            /* n: Unmute all users */
            user = cnf->firstuser;
            while (user)
            {
                if (user && (user-> adminflags & ADMINFLAG_MUTED))
                    user->adminflags ^= ADMINFLAG_MUTED;
                if (user->nextuser)
                    user = user->nextuser;
                else
                    break;
            }
            break;
        case 'k':
            /* k: Kick user */
            if (user)
                user->adminflags |= ADMINFLAG_KICKME;
            else
                cw_log(LOG_NOTICE, "Specified User not found!");
            break;
        }
    }
    else
    {
        cw_log(LOG_NOTICE, "Conference Number not found\n");
    }

    cw_mutex_unlock(&conflock);
    LOCAL_USER_REMOVE(u);
    return 0;
}

static void *recordthread(void *args)
{
    struct cw_conference *cnf;
    struct cw_frame *f=NULL;
    int flags;
    struct cw_filestream *s;
    int res=0;

    cnf = (struct cw_conference *)args;
    if( !cnf || !cnf->chan )
    {
        pthread_exit(0);
    }
    cw_stopstream(cnf->chan);
    flags = O_CREAT|O_TRUNC|O_WRONLY;
    s = cw_writefile(cnf->recordingfilename, cnf->recordingformat, NULL, flags, 0, 0644);

    if (s)
    {
        cnf->recording = MEETME_RECORD_ACTIVE;
        while (cw_waitfor(cnf->chan, -1) > -1)
        {
            f = cw_read(cnf->chan);
            if (!f)
            {
                res = -1;
                break;
            }
            if (f->frametype == CW_FRAME_VOICE)
            {
                res = cw_writestream(s, f);
                if (res)
                    break;
            }
            cw_fr_free(f);
            if (cnf->recording == MEETME_RECORD_TERMINATE)
            {
                cw_mutex_lock(&conflock);
                cw_mutex_unlock(&conflock);
                break;
            }
        }
        cnf->recording = MEETME_RECORD_OFF;
        cw_closestream(s);
    }
    pthread_exit(0);
}

int unload_module(void)
{
    int res = 0;

    STANDARD_HANGUP_LOCALUSERS;
    cw_cli_unregister(&cli_show_confs);
    cw_cli_unregister(&cli_conf);
    res |= cw_unregister_application(app3);
    res |= cw_unregister_application(app2);
    res |= cw_unregister_application(app);
    return res;
}

int load_module(void)
{
    cw_cli_register(&cli_show_confs);
    cw_cli_register(&cli_conf);
    app3 = cw_register_application(name3, admin_exec, synopsis3, syntax3, descrip3);
    app2 = cw_register_application(name2, count_exec, synopsis2, syntax2, descrip2);
    app = cw_register_application(name, conf_exec, synopsis, syntax, descrip);
    return 0;
}

char *description(void)
{
    return tdesc;
}

int usecount(void)
{
    int res;
    STANDARD_USECOUNT(res);
    return res;
}
