/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Based on Asterisk written by Mark Spencer <markster@digium.com>
 *  Copyright (C) 1999 - 2005, Digium, Inc.
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


/* Doxygenified Copyright Header */
/*!
 * \mainpage CallWeaver -- An Open Source Telephony Toolkit
 *
 * \arg \ref DevDoc
 * \arg \ref ConfigFiles
 *
 * \section copyright Copyright and author
 *
 * Based on Asterisk written by Mark Spencer <markster@digium.com>
 *  Copyright (C) 1999 - 2005, Digium, Inc.
 * Asterisk is a trade mark registered by Digium, Inc.
 *
 * Also see \ref cwCREDITS
 *
 * \section license License
 * See http://www.callweaver.org for more information about
 * the CallWeaver project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 *
 * \verbinclude LICENSE
 *
 */

/*! \file
  \brief Top level source file for CallWeaver  - the Open Source PBX. Implementation
  of PBX core functions and CLI interface.

 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/resource.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#ifdef __linux__
# include <sys/prctl.h>
#endif
#include <regex.h>
#include <spandsp.h>

#if  defined(__FreeBSD__) || defined( __NetBSD__ ) || defined(SOLARIS)
#include <netdb.h>
#endif

#undef _POSIX_SOURCE
#ifdef __linux__
# include <sys/capability.h>
#endif

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/corelib/callweaver.c $", "$Revision: 4746 $")

#include "callweaver/logger.h"
#include "callweaver/options.h"
#include "callweaver/cli.h"
#include "callweaver/channel.h"
#include "callweaver/ulaw.h"
#include "callweaver/alaw.h"
#include "callweaver/phone_no_utils.h"
#include "callweaver/module.h"
#include "callweaver/image.h"
#include "callweaver/term.h"
#include "callweaver/manager.h"
#include "callweaver/cdr.h"
#include "callweaver/pbx.h"
#include "callweaver/enum.h"
#include "callweaver/udp.h"
#include "callweaver/rtp.h"
#include "callweaver/udptl.h"
#include "callweaver/stun.h"
#include "callweaver/app.h"
#include "callweaver/lock.h"
#include "callweaver/utils.h"
#include "callweaver/file.h"
#include "callweaver/io.h"
#include "callweaver/lock.h"
#include "callweaver/config.h"
#include "callweaver/linkedlists.h"
#include "callweaver/devicestate.h"

#include <readline/readline.h>
#include <readline/history.h>

/* defines various compile-time defaults */
#include "defaults.h"

#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#define PF_LOCAL PF_UNIX
#endif

#define CW_MAX_CONNECTS 128
#define NUM_MSGS 64

#define WELCOME_MESSAGE cw_verbose(PACKAGE_STRING SVN_VERSION " http://www.callweaver.org - The True Open Source PBX\n"); \
		cw_verbose( "=========================================================================\n")


int option_verbose=0;
int option_debug=0;
int option_exec_includes=0;
int option_nofork=0;
int option_quiet=0;
int option_console=0;
int option_highpriority=0;
int option_remote=0;
int option_exec=0;
int option_initcrypto=0;
int option_nocolor = 0;
int option_dumpcore = 0;
int option_cache_record_files = 0;
int option_timestamp = 0;
int option_overrideconfig = 0;
int option_reconnect = 0;
int option_transcode_slin = 1;
int option_maxcalls = 0;
double option_maxload = 0.0;
int option_dontwarn = 0;
int option_priority_jumping = 1;
int fully_booted = 0;
char record_cache_dir[CW_CACHE_DIR_LEN] = cwtmpdir_default;
char debug_filename[CW_FILENAME_MAX] = "";

static int cw_socket = -1;		/*!< UNIX Socket for allowing remote control */
static int cw_consock = -1;		/*!< UNIX Socket for controlling another callweaver */
int cw_mainpid;
struct console
{
    int fd;				/*!< File descriptor */
    int p[2];			/*!< Pipe */
    pthread_t t;			/*!< Thread of handler */
};

static struct cw_atexit
{
    void (*func)(void);
    struct cw_atexit *next;
} *atexits = NULL;

CW_MUTEX_DEFINE_STATIC(atexitslock);

time_t cw_startuptime;
time_t cw_lastreloadtime;

static char *remotehostname;

struct console consoles[CW_MAX_CONNECTS];

char defaultlanguage[MAX_LANGUAGE] = DEFAULT_LANGUAGE;

static int rl_init = 0;
static int cw_rl_add_history(char *);
static int cw_rl_read_history(char *);
static int cw_rl_write_history(char *);

char cw_config_CW_CONFIG_DIR[CW_CONFIG_MAX_PATH];
char cw_config_CW_CONFIG_FILE[CW_CONFIG_MAX_PATH];
char cw_config_CW_MODULE_DIR[CW_CONFIG_MAX_PATH];
char cw_config_CW_SPOOL_DIR[CW_CONFIG_MAX_PATH];
char cw_config_CW_MONITOR_DIR[CW_CONFIG_MAX_PATH];
char cw_config_CW_VAR_DIR[CW_CONFIG_MAX_PATH];
char cw_config_CW_LOG_DIR[CW_CONFIG_MAX_PATH];
char cw_config_CW_OGI_DIR[CW_CONFIG_MAX_PATH];
char cw_config_CW_DB[CW_CONFIG_MAX_PATH];
char cw_config_CW_DB_DIR[CW_CONFIG_MAX_PATH];
char cw_config_CW_KEY_DIR[CW_CONFIG_MAX_PATH];
char cw_config_CW_PID[CW_CONFIG_MAX_PATH];
char cw_config_CW_SOCKET[CW_CONFIG_MAX_PATH];
char cw_config_CW_RUN_DIR[CW_CONFIG_MAX_PATH];
char cw_config_CW_RUN_USER[CW_CONFIG_MAX_PATH];
char cw_config_CW_RUN_GROUP[CW_CONFIG_MAX_PATH];
char cw_config_CW_CTL_PERMISSIONS[CW_CONFIG_MAX_PATH];
char cw_config_CW_CTL_OWNER[CW_CONFIG_MAX_PATH] = "\0";
char cw_config_CW_CTL_GROUP[CW_CONFIG_MAX_PATH] = "\0";
char cw_config_CW_CTL[CW_CONFIG_MAX_PATH] = "callweaver.ctl";
char cw_config_CW_SYSTEM_NAME[20] = "";
char cw_config_CW_SOUNDS_DIR[CW_CONFIG_MAX_PATH];
char cw_config_CW_ALLOW_SPAGHETTI_CODE[20] = "";

static char *_argv[256];
static int shuttingdown = 0;
static int restartnow = 0;
static pthread_t consolethread = CW_PTHREADT_NULL;

#if !defined(LOW_MEMORY)
struct file_version
{
    CW_LIST_ENTRY(file_version) list;
    const char *file;
    const char *version;
    size_t file_len;
    size_t version_len;
};

static CW_LIST_HEAD_STATIC(file_versions, file_version);

void cw_register_file_version(const char *file, const char *version)
{
    struct file_version *new;

    new = malloc(sizeof(*new));
    if (new)
    {
        while (*file && isspace(*file)) file++;
        if (!strncmp(file, "$HeadURL: ", 10))
        {
            new->file = file + 10;
            new->file_len = strlen(file + 10) - 2;
        }
        else
        {
            new->file = file;
            new->file_len = strlen(file);
        }

        while (*version && isspace(*version)) version++;
        if (!strncmp(version, "$Revision: ", 11))
        {
            new->version = version + 11;
            new->version_len = strlen(version + 11) - 2;
        }
        else
        {
            new->version = version;
            new->version_len = strlen(version);
        }

        CW_LIST_LOCK(&file_versions);
        CW_LIST_INSERT_HEAD(&file_versions, new, list);
        CW_LIST_UNLOCK(&file_versions);
    }
}

void cw_unregister_file_version(const char *file)
{
    struct file_version *find;

    while (*file && isspace(*file)) file++;
    if (!strncmp(file, "$HeadURL: ", 10))
        file += 10;

    CW_LIST_LOCK(&file_versions);
    CW_LIST_TRAVERSE_SAFE_BEGIN(&file_versions, find, list)
    {
        if (!strcasecmp(find->file, file))
        {
            CW_LIST_REMOVE_CURRENT(&file_versions, list);
            break;
        }
    }
    CW_LIST_TRAVERSE_SAFE_END;
    CW_LIST_UNLOCK(&file_versions);
    if (find)
        free(find);
}

static char show_version_files_help[] =
    "Usage: show version files [like <pattern>]\n"
    "       Shows the revision numbers of the files used to build this copy of CallWeaver.\n"
    "       Optional regular expression pattern is used to filter the file list.\n";

/*! CLI command to list module versions */
static int handle_show_version_files(int fd, int argc, char *argv[])
{
#define FORMAT "%-8.*s %.*s\n"
    struct file_version *iterator;
    regex_t regexbuf;
    int havepattern = 0;
    int havename = 0;
    int count_files = 0;

    switch (argc)
    {
    case 5:
        if (!strcasecmp(argv[3], "like"))
        {
            if (regcomp(&regexbuf, argv[4], REG_EXTENDED | REG_NOSUB))
                return RESULT_SHOWUSAGE;
            havepattern = 1;
        }
        else
            return RESULT_SHOWUSAGE;
        break;
    case 4:
        havename = 1;
        break;
    case 3:
        break;
    default:
        return RESULT_SHOWUSAGE;
    }

    cw_cli(fd, FORMAT, 8, "Revision", 8, "SVN Path");
    cw_cli(fd, FORMAT, 8, "--------", 8, "--------");
    CW_LIST_LOCK(&file_versions);
    CW_LIST_TRAVERSE(&file_versions, iterator, list)
    {
        if (havename && strcasecmp(iterator->file, argv[3]))
            continue;

        if (havepattern && regexec(&regexbuf, iterator->file, 0, NULL, 0))
            continue;

        cw_cli(fd, FORMAT, (int)iterator->version_len, iterator->version, (int)iterator->file_len, iterator->file);
        count_files++;
        if (havename)
            break;
    }
    CW_LIST_UNLOCK(&file_versions);
    if (!havename)
    {
        cw_cli(fd, "%d files listed.\n", count_files);
    }

    if (havepattern)
        regfree(&regexbuf);

    return RESULT_SUCCESS;
#undef FORMAT
}

static char *complete_show_version_files(char *line, char *word, int pos, int state)
{
    struct file_version *find;
    int which = 0;
    char *ret = NULL;
    int matchlen = strlen(word);

    if (pos != 3)
        return NULL;

    CW_LIST_LOCK(&file_versions);
    CW_LIST_TRAVERSE(&file_versions, find, list)
    {
        if (!strncasecmp(word, find->file, matchlen))
        {
            if (++which > state)
            {
                ret = strdup(find->file);
                break;
            }
        }
    }
    CW_LIST_UNLOCK(&file_versions);

    return ret;
}
#endif /* ! LOW_MEMORY */

int cw_register_atexit(void (*func)(void))
{
    int res = -1;
    struct cw_atexit *ae;
    cw_unregister_atexit(func);
    ae = malloc(sizeof(struct cw_atexit));
    cw_mutex_lock(&atexitslock);
    if (ae)
    {
        memset(ae, 0, sizeof(struct cw_atexit));
        ae->next = atexits;
        ae->func = func;
        atexits = ae;
        res = 0;
    }
    cw_mutex_unlock(&atexitslock);
    return res;
}

void cw_unregister_atexit(void (*func)(void))
{
    struct cw_atexit *ae, *prev = NULL;
    cw_mutex_lock(&atexitslock);
    ae = atexits;
    while (ae)
    {
        if (ae->func == func)
        {
            if (prev)
                prev->next = ae->next;
            else
                atexits = ae->next;
            break;
        }
        prev = ae;
        ae = ae->next;
    }
    cw_mutex_unlock(&atexitslock);
}

static int fdprint(int fd, const char *s)
{
    return write(fd, s, strlen(s) + 1);
}

/*! NULL handler so we can collect the child exit status */
static void null_sig_handler(int signal)
{

}

CW_MUTEX_DEFINE_STATIC(safe_system_lock);
static unsigned int safe_system_level = 0;
static void *safe_system_prev_handler;

int cw_safe_system(const char *s)
{
    pid_t pid;
    int x;
    int res;
    struct rusage rusage;
    int status;
    unsigned int level;

    /* keep track of how many cw_safe_system() functions
       are running at this moment
    */
    cw_mutex_lock(&safe_system_lock);
    level = safe_system_level++;

    /* only replace the handler if it has not already been done */
    if (level == 0)
        safe_system_prev_handler = signal(SIGCHLD, null_sig_handler);

    cw_mutex_unlock(&safe_system_lock);

    pid = fork();

    if (pid == 0)
    {
        /* Close file descriptors and launch system command */
        for (x = STDERR_FILENO + 1; x < 4096; x++)
            close(x);
        execl("/bin/sh", "/bin/sh", "-c", s, NULL);
        exit(1);
    }
    else if (pid > 0)
    {
        for (;;)
        {
            res = wait4(pid, &status, 0, &rusage);
            if (res > -1)
            {
                res = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                break;
            }
            else if (errno != EINTR)
                break;
        }
    }
    else
    {
        cw_log(LOG_WARNING, "Fork failed: %s\n", strerror(errno));
        res = -1;
    }

    cw_mutex_lock(&safe_system_lock);
    level = --safe_system_level;

    /* only restore the handler if we are the last one */
    if (level == 0)
        signal(SIGCHLD, safe_system_prev_handler);

    cw_mutex_unlock(&safe_system_lock);

    return res;
}

/*!
 * write the string to all attached console clients
 */
static void cw_network_puts(const char *string)
{
    int x;
    for (x=0;x<CW_MAX_CONNECTS; x++)
    {
        if (consoles[x].fd > -1)
            fdprint(consoles[x].p[1], string);
    }
}

/*!
 * write the string to the console, and all attached
 * console clients
 */
void cw_console_puts(const char *string)
{
    fputs(string, stdout);
    fflush(stdout);
    cw_network_puts(string);
}

static void network_verboser(const char *s, int pos, int replace, int complete)
/* ARGUSED */
{
    if (replace)
    {
        char *t = alloca(strlen(s) + 2);
        sprintf(t, "\r%s", s);
        if (complete)
            cw_network_puts(t);
    }
    else
    {
        if (complete)
            cw_network_puts(s);
    }
}

static pthread_t lthread;

static void *netconsole(void *vconsole)
{
    struct console *con = vconsole;
    char hostname[MAXHOSTNAMELEN]="";
    char tmp[512];
    int res;
    struct pollfd fds[2];

    if (gethostname(hostname, sizeof(hostname)-1))
        cw_copy_string(hostname, "<Unknown>", sizeof(hostname));

    snprintf(tmp, sizeof(tmp), "%s/%d/%s\n", hostname, cw_mainpid,  PACKAGE_STRING SVN_VERSION );

    fdprint(con->fd, tmp);
    for (;;)
    {
        fds[0].fd = con->fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = con->p[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        res = poll(fds, 2, -1);
        if (res < 0)
        {
            if (errno != EINTR)
                cw_log(LOG_WARNING, "poll returned < 0: %s\n", strerror(errno));
            continue;
        }
        if (fds[0].revents)
        {
            res = read(con->fd, tmp, sizeof(tmp));
            if (res < 1)
            {
                break;
            }
            tmp[res] = 0;
            cw_cli_command(con->fd, tmp);
        }
        if (fds[1].revents)
        {
            res = read(con->p[0], tmp, sizeof(tmp));
            if (res < 1)
            {
                cw_log(LOG_ERROR, "read returned %d\n", res);
                break;
            }
            res = write(con->fd, tmp, res);
            if (res < 1)
                break;
        }
    }
    if (option_verbose > 2)
        cw_verbose(VERBOSE_PREFIX_3 "Remote UNIX connection disconnected\n");
    close(con->fd);
    close(con->p[0]);
    close(con->p[1]);
    con->fd = -1;
    return NULL;
}

static void *listener(void *unused)
{
    struct sockaddr_un sunaddr;
    int s;
    socklen_t len;
    int x;
    int flags;
    struct pollfd fds[1];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    for (;;)
    {
        if (cw_socket < 0)
            return NULL;
        fds[0].fd = cw_socket;
        fds[0].events= POLLIN;
        pthread_testcancel();
        s = poll(fds, 1, -1);
        if (s < 0)
        {
            if (errno != EINTR)
                cw_log(LOG_WARNING, "poll returned error: %s\n", strerror(errno));
            continue;
        }
        len = sizeof(sunaddr);
        s = accept(cw_socket, (struct sockaddr *)&sunaddr, &len);
        if (s < 0)
        {
            if (errno != EINTR)
                cw_log(LOG_WARNING, "Accept returned %d: %s\n", s, strerror(errno));
        }
        else
        {
            for (x=0;x<CW_MAX_CONNECTS;x++)
            {
                if (consoles[x].fd < 0)
                {
                    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, consoles[x].p))
                    {
                        cw_log(LOG_ERROR, "Unable to create pipe: %s\n", strerror(errno));
                        consoles[x].fd = -1;
                        fdprint(s, "Server failed to create pipe\n");
                        close(s);
                        break;
                    }
                    flags = fcntl(consoles[x].p[1], F_GETFL);
                    fcntl(consoles[x].p[1], F_SETFL, flags | O_NONBLOCK);
                    consoles[x].fd = s;
                    if (cw_pthread_create(&consoles[x].t, &attr, netconsole, &consoles[x]))
                    {
                        cw_log(LOG_ERROR, "Unable to spawn thread to handle connection: %s\n", strerror(errno));
                        consoles[x].fd = -1;
                        fdprint(s, "Server failed to spawn thread\n");
                        close(s);
                    }
                    break;
                }
            }
            if (x >= CW_MAX_CONNECTS)
            {
                fdprint(s, "No more connections allowed\n");
                cw_log(LOG_WARNING, "No more connections allowed\n");
                close(s);
            }
            else if (consoles[x].fd > -1)
            {
                if (option_verbose > 2)
                    cw_verbose(VERBOSE_PREFIX_3 "Remote UNIX connection\n");
            }
        }
    }
    return NULL;
}

static int cw_makesocket(void)
{
    struct sockaddr_un sunaddr;
    int res;
    int x;
    uid_t uid = -1;
    gid_t gid = -1;

    for (x = 0; x < CW_MAX_CONNECTS; x++)
        consoles[x].fd = -1;
    unlink(cw_config_CW_SOCKET);
    cw_socket = socket(PF_LOCAL, SOCK_STREAM, 0);
    if (cw_socket < 0)
    {
        cw_log(LOG_WARNING, "Unable to create control socket: %s\n", strerror(errno));
        return -1;
    }
    memset(&sunaddr, 0, sizeof(sunaddr));
    sunaddr.sun_family = AF_LOCAL;
    cw_copy_string(sunaddr.sun_path, cw_config_CW_SOCKET, sizeof(sunaddr.sun_path));
    res = bind(cw_socket, (struct sockaddr *)&sunaddr, sizeof(sunaddr));
    if (res)
    {
        cw_log(LOG_WARNING, "Unable to bind socket to %s: %s\n", cw_config_CW_SOCKET, strerror(errno));
        close(cw_socket);
        cw_socket = -1;
        return -1;
    }
    res = listen(cw_socket, 2);
    if (res < 0)
    {
        cw_log(LOG_WARNING, "Unable to listen on socket %s: %s\n", cw_config_CW_SOCKET, strerror(errno));
        close(cw_socket);
        cw_socket = -1;
        return -1;
    }
    cw_register_verbose(network_verboser);
    cw_pthread_create(&lthread, NULL, listener, NULL);

    if (!cw_strlen_zero(cw_config_CW_CTL_OWNER))
    {
        struct passwd *pw;
        if ((pw = getpwnam(cw_config_CW_CTL_OWNER)) == NULL)
        {
            cw_log(LOG_WARNING, "Unable to find uid of user %s\n", cw_config_CW_CTL_OWNER);
        }
        else
        {
            uid = pw->pw_uid;
        }
    }

    if (!cw_strlen_zero(cw_config_CW_CTL_GROUP))
    {
        struct group *grp;
        if ((grp = getgrnam(cw_config_CW_CTL_GROUP)) == NULL)
        {
            cw_log(LOG_WARNING, "Unable to find gid of group %s\n", cw_config_CW_CTL_GROUP);
        }
        else
        {
            gid = grp->gr_gid;
        }
    }

    if (chown(cw_config_CW_SOCKET, uid, gid) < 0)
        cw_log(LOG_WARNING, "Unable to change ownership of %s: %s\n", cw_config_CW_SOCKET, strerror(errno));

    if (!cw_strlen_zero(cw_config_CW_CTL_PERMISSIONS))
    {
        mode_t p;
        sscanf(cw_config_CW_CTL_PERMISSIONS, "%o", (int *) &p);
        if ((chmod(cw_config_CW_SOCKET, p)) < 0)
            cw_log(LOG_WARNING, "Unable to change file permissions of %s: %s\n", cw_config_CW_SOCKET, strerror(errno));
    }

    return 0;
}

static int cw_tryconnect(void)
{
    struct sockaddr_un sunaddr;
    int res;
    cw_consock = socket(PF_LOCAL, SOCK_STREAM, 0);
    if (cw_consock < 0)
    {
        cw_log(LOG_WARNING, "Unable to create socket: %s\n", strerror(errno));
        return 0;
    }
    memset(&sunaddr, 0, sizeof(sunaddr));
    sunaddr.sun_family = AF_LOCAL;
    cw_copy_string(sunaddr.sun_path, (char *)cw_config_CW_SOCKET, sizeof(sunaddr.sun_path));
    res = connect(cw_consock, (struct sockaddr *)&sunaddr, sizeof(sunaddr));
    if (res)
    {
        close(cw_consock);
        cw_consock = -1;
        return 0;
    }
    else
    {
        return 1;
    }
}

/*! Urgent handler
 Called by soft_hangup to interrupt the poll, read, or other
 system call.  We don't actually need to do anything though.
 Remember: Cannot EVER cw_log from within a signal handler
 */
static void urg_handler(int num)
{
    signal(num, urg_handler);
    return;
}

static void hup_handler(int num)
{
    if (option_verbose > 1)
        printf("Received HUP signal -- Reloading configs\n");
    if (restartnow)
        execvp(_argv[0], _argv);
    /* XXX This could deadlock XXX */
    cw_module_reload(NULL);
    signal(num, hup_handler);
}

static void child_handler(int sig)
{
    /* Must not ever cw_log or cw_verbose within signal handler */
    int n, status;

    /*
     * Reap all dead children -- not just one
     */
    for (n = 0; wait3(&status, WNOHANG, NULL) > 0; n++)
        ;
    if (n == 0 && option_debug)
        printf("Huh?  Child handler, but nobody there?\n");
    signal(sig, child_handler);
}

/*! Set an X-term or screen title */
static void set_title(char *text)
{
    if (getenv("TERM") && strstr(getenv("TERM"), "xterm"))
        fprintf(stdout, "\033]2;%s\007", text);
}

static void set_icon(char *text)
{
    if (getenv("TERM") && strstr(getenv("TERM"), "xterm"))
        fprintf(stdout, "\033]1;%s\007", text);
}

/*! We set ourselves to a high priority, that we might pre-empt everything
   else.  If your PBX has heavy activity on it, this is a good thing.  */
int cw_set_priority(int pri)
{
    struct sched_param sched;
    memset(&sched, 0, sizeof(sched));
#ifdef __linux__
    if (pri)
    {
        sched.sched_priority = 10;
        if (sched_setscheduler(0, SCHED_RR, &sched))
        {
            cw_log(LOG_WARNING, "Unable to set high priority\n");
            return -1;
        }
        else
            if (option_verbose)
                cw_verbose("Set to realtime thread\n");
    }
    else
    {
        sched.sched_priority = 0;
        if (sched_setscheduler(0, SCHED_OTHER, &sched))
        {
            cw_log(LOG_WARNING, "Unable to set normal priority\n");
            return -1;
        }
    }
#else
    if (pri)
    {
        if (setpriority(PRIO_PROCESS, 0, -10) == -1)
        {
            cw_log(LOG_WARNING, "Unable to set high priority\n");
            return -1;
        }
        else
            if (option_verbose)
                cw_verbose("Set to high priority\n");
    }
    else
    {
        if (setpriority(PRIO_PROCESS, 0, 0) == -1)
        {
            cw_log(LOG_WARNING, "Unable to set normal priority\n");
            return -1;
        }
    }
#endif
    return 0;
}

static void cw_run_atexits(void)
{
    struct cw_atexit *ae;
    cw_mutex_lock(&atexitslock);
    ae = atexits;
    while (ae)
    {
        if (ae->func)
            ae->func();
        ae = ae->next;
    }
    cw_mutex_unlock(&atexitslock);
}

static void quit_handler(int num, int nice, int safeshutdown, int restart)
{
    char filename[80] = "";
    time_t s,e;
    int x;
    /* Try to get as many CDRs as possible submitted to the backend engines (if in batch mode) */
    cw_cdr_engine_term();
    if (safeshutdown)
    {
        shuttingdown = 1;
        if (!nice)
        {
            /* Begin shutdown routine, hanging up active channels */
            cw_begin_shutdown(1);
            if (option_verbose && option_console)
                cw_verbose("Beginning callweaver %s....\n", restart ? "restart" : "shutdown");
            time(&s);
            for (;;)
            {
                time(&e);
                /* Wait up to 15 seconds for all channels to go away */
                if ((e - s) > 15)
                    break;
                if (!cw_active_channels())
                    break;
                if (!shuttingdown)
                    break;
                /* Sleep 1/10 of a second */
                usleep(100000);
            }
        }
        else
        {
            if (nice < 2)
                cw_begin_shutdown(0);
            if (option_verbose && option_console)
                cw_verbose("Waiting for inactivity to perform %s...\n", restart ? "restart" : "halt");
            for (;;)
            {
                if (!cw_active_channels())
                    break;
                if (!shuttingdown)
                    break;
                sleep(1);
            }
        }

        if (!shuttingdown)
        {
            if (option_verbose && option_console)
            {
                cw_verbose("CallWeaver %s cancelled.\n", restart ? "restart" : "shutdown");
                printf(cw_term_quit());
                if (rl_init)
                    rl_deprep_terminal();
            }
            return;
        }
    }
    if (option_console || option_remote)
    {
        if (getenv("HOME"))
            snprintf(filename, sizeof(filename), "%s/.callweaver_history", getenv("HOME"));
        if (!cw_strlen_zero(filename))
            cw_rl_write_history(filename);
    }
    if (option_verbose)
        cw_verbose("Executing last minute cleanups\n");
    cw_run_atexits();

    cwdb_shutdown();

    /* Called on exit */
    if (option_verbose && option_console)
        cw_verbose("CallWeaver %s ending (%d).\n", cw_active_channels() ? "uncleanly" : "cleanly", num);
    if (option_debug)
        cw_log(LOG_DEBUG, "CallWeaver ending (%d).\n", num);

    manager_event(EVENT_FLAG_SYSTEM, "Shutdown", "Shutdown: %s\r\nRestart: %s\r\n", cw_active_channels() ? "Uncleanly" : "Cleanly", restart ? "True" : "False");
    if (cw_socket > -1)
    {
        pthread_cancel(lthread);
        close(cw_socket);
        cw_socket = -1;
        unlink(cw_config_CW_SOCKET);
    }
    if (cw_consock > -1)
        close(cw_consock);
    //if (cw_socket > -1)
    //	unlink((char *)cw_config_CW_SOCKET);
    if (!option_remote) unlink((char *)cw_config_CW_PID);
    if (restart)
    {
        if (option_verbose || option_console)
            cw_verbose("Preparing for CallWeaver restart...\n");
        /* Mark all FD's for closing on exec */
        for (x=3;x<32768;x++)
        {
            fcntl(x, F_SETFD, FD_CLOEXEC);
        }
        if (option_verbose || option_console)
            cw_verbose("Restarting CallWeaver NOW...\n");
        restartnow = 1;

        /* close logger */
        close_logger();

        /* If there is a consolethread running send it a SIGHUP
           so it can execvp, otherwise we can do it ourselves */
        if (consolethread != CW_PTHREADT_NULL && consolethread != pthread_self())
        {
            pthread_kill(consolethread, SIGHUP);
            /* Give the signal handler some time to complete */
            sleep(2);
        }
        else
            execvp(_argv[0], _argv);

    }
    else
    {
        /* close logger */
        close_logger();
    }
    printf(cw_term_quit());
    if (rl_init)
        rl_deprep_terminal();
    exit(0);
}

static void __quit_handler(int num)
{
    quit_handler(num, 0, 1, 0);
}

static const char *fix_header(char *outbuf, int maxout, const char *s, char *cmp)
{
    const char *c;
    if (!strncmp(s, cmp, strlen(cmp)))
    {
        c = s + strlen(cmp);
        cw_term_color(outbuf, cmp, COLOR_GRAY, 0, maxout);
        return c;
    }
    return NULL;
}

static void console_verboser(const char *s, int pos, int replace, int complete)
{
    char tmp[80];
    const char *c=NULL;
    /* Return to the beginning of the line */
    if (!pos)
    {
        fprintf(stdout, "\r");
        if ((c = fix_header(tmp, sizeof(tmp), s, VERBOSE_PREFIX_4)) ||
                (c = fix_header(tmp, sizeof(tmp), s, VERBOSE_PREFIX_3)) ||
                (c = fix_header(tmp, sizeof(tmp), s, VERBOSE_PREFIX_2)) ||
                (c = fix_header(tmp, sizeof(tmp), s, VERBOSE_PREFIX_1)))
            fputs(tmp, stdout);
    }
    if (c)
        fputs(c + pos,stdout);
    else
        fputs(s + pos,stdout);
    fflush(stdout);
    if (complete)
    {
        /* Wake up a poll()ing console */
        if (option_console && consolethread != CW_PTHREADT_NULL)
            pthread_kill(consolethread, SIGURG);
    }
}

static int cw_all_zeros(char *s)
{
    while (*s)
    {
        if (*s > 32)
            return 0;
        s++;
    }
    return 1;
}

static void consolehandler(char *s)
{
    printf(cw_term_end());
    fflush(stdout);
    /* Called when readline data is available */
    if (s && !cw_all_zeros(s))
        cw_rl_add_history(s);
    /* Give the console access to the shell */
    if (s)
    {
        /* The real handler for bang */
        if (s[0] == '!')
        {
            if (s[1])
                cw_safe_system(s+1);
            else
                cw_safe_system(getenv("SHELL") ? getenv("SHELL") : "/bin/sh");
        }
        else
            cw_cli_command(STDOUT_FILENO, s);
    }
    else
        fprintf(stdout, "\nUse \"quit\" to exit\n");
}

static int remoteconsolehandler(char *s)
{
    int ret = 0;
    /* Called when readline data is available */
    if (s && !cw_all_zeros(s))
        cw_rl_add_history(s);
    /* Give the console access to the shell */
    if (s)
    {
        /* The real handler for bang */
        if (s[0] == '!')
        {
            if (s[1])
                cw_safe_system(s+1);
            else
                cw_safe_system(getenv("SHELL") ? getenv("SHELL") : "/bin/sh");
            ret = 1;
        }
        if ((strncasecmp(s, "quit", 4) == 0 || strncasecmp(s, "exit", 4) == 0) &&
                (s[4] == '\0' || isspace(s[4])))
        {
            quit_handler(0, 0, 0, 0);
            ret = 1;
        }
    }
    else
        fprintf(stdout, "\nUse \"quit\" to exit\n");

    return ret;
}

static char abort_halt_help[] =
    "Usage: abort shutdown\n"
    "       Causes CallWeaver to abort an executing shutdown or restart, and resume normal\n"
    "       call operations.\n";

static char shutdown_now_help[] =
    "Usage: stop now\n"
    "       Shuts down a running CallWeaver immediately, hanging up all active calls .\n";

static char shutdown_gracefully_help[] =
    "Usage: stop gracefully\n"
    "       Causes CallWeaver to not accept new calls, and exit when all\n"
    "       active calls have terminated normally.\n";

static char shutdown_when_convenient_help[] =
    "Usage: stop when convenient\n"
    "       Causes CallWeaver to perform a shutdown when all active calls have ended.\n";

static char restart_now_help[] =
    "Usage: restart now\n"
    "       Causes CallWeaver to hangup all calls and exec() itself performing a cold\n"
    "       restart.\n";

static char restart_gracefully_help[] =
    "Usage: restart gracefully\n"
    "       Causes CallWeaver to stop accepting new calls and exec() itself performing a cold\n"
    "       restart when all active calls have ended.\n";

static char restart_when_convenient_help[] =
    "Usage: restart when convenient\n"
    "       Causes CallWeaver to perform a cold restart when all active calls have ended.\n";

static char bang_help[] =
    "Usage: !<command>\n"
    "       Executes a given shell command\n";

#if 0
static int handle_quit(int fd, int argc, char *argv[])
{
    if (argc != 1)
        return RESULT_SHOWUSAGE;
    quit_handler(0, 0, 1, 0);
    return RESULT_SUCCESS;
}
#endif

static int handle_shutdown_now(int fd, int argc, char *argv[])
{
    if (argc != 2)
        return RESULT_SHOWUSAGE;
    quit_handler(0, 0 /* Not nice */, 1 /* safely */, 0 /* not restart */);
    return RESULT_SUCCESS;
}

static int handle_shutdown_gracefully(int fd, int argc, char *argv[])
{
    if (argc != 2)
        return RESULT_SHOWUSAGE;
    quit_handler(0, 1 /* nicely */, 1 /* safely */, 0 /* no restart */);
    return RESULT_SUCCESS;
}

static int handle_shutdown_when_convenient(int fd, int argc, char *argv[])
{
    if (argc != 3)
        return RESULT_SHOWUSAGE;
    quit_handler(0, 2 /* really nicely */, 1 /* safely */, 0 /* don't restart */);
    return RESULT_SUCCESS;
}

static int handle_restart_now(int fd, int argc, char *argv[])
{
    if (argc != 2)
        return RESULT_SHOWUSAGE;
    quit_handler(0, 0 /* not nicely */, 1 /* safely */, 1 /* restart */);
    return RESULT_SUCCESS;
}

static int handle_restart_gracefully(int fd, int argc, char *argv[])
{
    if (argc != 2)
        return RESULT_SHOWUSAGE;
    quit_handler(0, 1 /* nicely */, 1 /* safely */, 1 /* restart */);
    return RESULT_SUCCESS;
}

static int handle_restart_when_convenient(int fd, int argc, char *argv[])
{
    if (argc != 3)
        return RESULT_SHOWUSAGE;
    quit_handler(0, 2 /* really nicely */, 1 /* safely */, 1 /* restart */);
    return RESULT_SUCCESS;
}

static int handle_abort_halt(int fd, int argc, char *argv[])
{
    if (argc != 2)
        return RESULT_SHOWUSAGE;
    cw_cancel_shutdown();
    shuttingdown = 0;
    return RESULT_SUCCESS;
}

static int handle_bang(int fd, int argc, char *argv[])
{
    return RESULT_SUCCESS;
}

#define CALLWEAVER_PROMPT "*CLI> "

#define CALLWEAVER_PROMPT2 "%s*CLI> "

static struct cw_cli_entry core_cli[] =
{
    {
        { "abort", "halt", NULL
        }
        , handle_abort_halt,
        "Cancel a running halt", abort_halt_help
    },
    { { "stop", "now", NULL }, handle_shutdown_now,
        "Shut down CallWeaver immediately", shutdown_now_help
    },
    { { "stop", "gracefully", NULL }, handle_shutdown_gracefully,
        "Gracefully shut down CallWeaver", shutdown_gracefully_help
    },
    { { "stop", "when","convenient", NULL }, handle_shutdown_when_convenient,
        "Shut down CallWeaver at empty call volume", shutdown_when_convenient_help
    },
    { { "restart", "now", NULL }, handle_restart_now,
        "Restart CallWeaver immediately", restart_now_help
    },
    { { "restart", "gracefully", NULL }, handle_restart_gracefully,
        "Restart CallWeaver gracefully", restart_gracefully_help
    },
    { { "restart", "when", "convenient", NULL }, handle_restart_when_convenient,
        "Restart CallWeaver at empty call volume", restart_when_convenient_help
    },
    { { "!", NULL }, handle_bang,
        "Execute a shell command", bang_help
    },
#if !defined(LOW_MEMORY)
    {
        { "show", "version", "files", NULL
        }
        , handle_show_version_files,
        "Show versions of files used to build CallWeaver", show_version_files_help, complete_show_version_files
    },
#endif /* ! LOW_MEMORY */
};

static int cw_rl_read_char(FILE *cp)
{
    int num_read=0;
    int lastpos=0;
    struct pollfd fds[2];
    int res;
    int max;
    char buf[512];

    for (;;)
    {
        max = 1;
        fds[0].fd = cw_consock;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        if (!option_exec)
        {
            fds[1].fd = STDIN_FILENO;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            max++;
        }
        res = poll(fds, max, -1);
        if (res < 0)
        {
            if (errno == EINTR)
                continue;
            cw_log(LOG_ERROR, "poll failed: %s\n", strerror(errno));
            break;
        }

        if (!option_exec && fds[1].revents)
        {
            num_read = rl_getc(cp);
            if (num_read < 1)
                break;
            else
                return (num_read);
        }
        if (fds[0].revents)
        {
            res = read(cw_consock, buf, sizeof(buf) - 1);
            /* if the remote side disappears exit */
            if (res < 1)
            {
                fprintf(stderr, "\nDisconnected from CallWeaver server\n");
                if (!option_reconnect)
                {
                    quit_handler(0, 0, 0, 0);
                }
                else
                {
                    int tries;
                    int reconnects_per_second = 20;
                    fprintf(stderr, "Attempting to reconnect for 30 seconds\n");
                    for (tries = 0; tries < 30 * reconnects_per_second;tries++)
                    {
                        if (cw_tryconnect())
                        {
                            fprintf(stderr, "Reconnect succeeded after %.3f seconds\n", 1.0 / reconnects_per_second * tries);
                            printf(cw_term_quit());
                            WELCOME_MESSAGE;
                            break;
                        }
                        else
                        {
                            usleep(1000000 / reconnects_per_second);
                        }
                    }
                    if (tries >= 30 * reconnects_per_second)
                    {
                        fprintf(stderr, "Failed to reconnect for 30 seconds.  Quitting.\n");
                        quit_handler(0, 0, 0, 0);
                    }
                }
            }

            buf[res] = '\0';

            if (!option_exec && !lastpos)
                write(STDOUT_FILENO, "\r", 1);
            write(STDOUT_FILENO, buf, res);
            if ((buf[res-1] == '\n') || (buf[res-2] == '\n'))
            {
                rl_forced_update_display();
                return (0);
            }
            else
            {
                lastpos = 1;
            }
        }
    }
    rl_forced_update_display();
    return (0);
}

#ifdef __Darwin__
static int cw_rl_out_event(void)
{
    int lastpos=0;
    struct pollfd fds[2];
    int res;
    char buf[512];

    fds[0].fd = cw_consock;
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    res = poll(fds, 1, 25);


    if (fds[0].revents)
        while ( (res = read(cw_consock, buf, sizeof(buf) - 1) ) )
        {
            if ( res > 0 )
            {
                buf[res] = '\0';

                if (!option_exec && !lastpos)
                    write(STDOUT_FILENO, "\r", 1);
                write(STDOUT_FILENO, buf, res);
                if ((buf[res-1] == '\n') || (buf[res-2] == '\n'))
                {
                    rl_forced_update_display();
                    return (0);
                }
                else
                {
                    lastpos = 1;
                }
            }
            rl_forced_update_display();
        }
    return (0);
}
#endif

static char *cli_prompt(void)
{
    static char prompt[200];
    char *pfmt;
    int color_used=0;
    char term_code[20];

    if ((pfmt = getenv("CALLWEAVER_PROMPT")))
    {
        char *t = pfmt, *p = prompt;
        memset(prompt, 0, sizeof(prompt));
        while (*t != '\0' && *p < sizeof(prompt))
        {
            if (*t == '%')
            {
                char hostname[MAXHOSTNAMELEN]="";
                int i;
                struct timeval tv;
                struct tm tm;
#ifdef linux
                FILE *LOADAVG;
#endif
                int fgcolor = COLOR_WHITE, bgcolor = COLOR_BLACK;

                t++;
                switch (*t)
                {
                case 'C': /* color */
                    t++;
                    if (sscanf(t, "%d;%d%n", &fgcolor, &bgcolor, &i) == 2)
                    {
                        strncat(p, cw_term_color_code(term_code, fgcolor, bgcolor, sizeof(term_code)),sizeof(prompt) - strlen(prompt) - 1);
                        t += i - 1;
                    }
                    else if (sscanf(t, "%d%n", &fgcolor, &i) == 1)
                    {
                        strncat(p, cw_term_color_code(term_code, fgcolor, 0, sizeof(term_code)),sizeof(prompt) - strlen(prompt) - 1);
                        t += i - 1;
                    }

                    /* If the color has been reset correctly, then there's no need to reset it later */
                    if ((fgcolor == COLOR_WHITE) && (bgcolor == COLOR_BLACK))
                    {
                        color_used = 0;
                    }
                    else
                    {
                        color_used = 1;
                    }
                    break;
                case 'd': /* date */
                    memset(&tm, 0, sizeof(struct tm));
                    tv = cw_tvnow();
                    if (localtime_r(&(tv.tv_sec), &tm))
                    {
                        strftime(p, sizeof(prompt) - strlen(prompt), "%Y-%m-%d", &tm);
                    }
                    break;
                case 'h': /* hostname */
                    if (!gethostname(hostname, sizeof(hostname) - 1))
                    {
                        strncat(p, hostname, sizeof(prompt) - strlen(prompt) - 1);
                    }
                    else
                    {
                        strncat(p, "localhost", sizeof(prompt) - strlen(prompt) - 1);
                    }
                    break;
                case 'H': /* short hostname */
                    if (!gethostname(hostname, sizeof(hostname) - 1))
                    {
                        for (i=0;i<sizeof(hostname);i++)
                        {
                            if (hostname[i] == '.')
                            {
                                hostname[i] = '\0';
                                break;
                            }
                        }
                        strncat(p, hostname, sizeof(prompt) - strlen(prompt) - 1);
                    }
                    else
                    {
                        strncat(p, "localhost", sizeof(prompt) - strlen(prompt) - 1);
                    }
                    break;
#ifdef linux
                case 'l': /* load avg */
                    t++;
                    if ((LOADAVG = fopen("/proc/loadavg", "r")))
                    {
                        float avg1, avg2, avg3;
                        int actproc, totproc, npid, which;
                        fscanf(LOADAVG, "%f %f %f %d/%d %d",
                               &avg1, &avg2, &avg3, &actproc, &totproc, &npid);
                        if (sscanf(t, "%d", &which) == 1)
                        {
                            switch (which)
                            {
                            case 1:
                                snprintf(p, sizeof(prompt) - strlen(prompt), "%.2f", avg1);
                                break;
                            case 2:
                                snprintf(p, sizeof(prompt) - strlen(prompt), "%.2f", avg2);
                                break;
                            case 3:
                                snprintf(p, sizeof(prompt) - strlen(prompt), "%.2f", avg3);
                                break;
                            case 4:
                                snprintf(p, sizeof(prompt) - strlen(prompt), "%d/%d", actproc, totproc);
                                break;
                            case 5:
                                snprintf(p, sizeof(prompt) - strlen(prompt), "%d", npid);
                                break;
                            }
                        }
                    }
                    break;
#endif
                case 't': /* time */
                    memset(&tm, 0, sizeof(struct tm));
                    tv = cw_tvnow();
                    if (localtime_r(&(tv.tv_sec), &tm))
                    {
                        strftime(p, sizeof(prompt) - strlen(prompt), "%H:%M:%S", &tm);
                    }
                    break;
                case '#': /* process console or remote? */
                    if (! option_remote)
                    {
                        strncat(p, "#", sizeof(prompt) - strlen(prompt) - 1);
                    }
                    else
                    {
                        strncat(p, ">", sizeof(prompt) - strlen(prompt) - 1);
                    }
                    break;
                case '%': /* literal % */
                    strncat(p, "%", sizeof(prompt) - strlen(prompt) - 1);
                    break;
                case '\0': /* % is last character - prevent bug */
                    t--;
                    break;
                }
                while (*p != '\0')
                {
                    p++;
                }
                t++;
            }
            else
            {
                *p = *t;
                p++;
                t++;
            }
        }
        if (color_used)
        {
            /* Force colors back to normal at end */
            cw_term_color_code(term_code, COLOR_WHITE, COLOR_BLACK, sizeof(term_code));
            if (strlen(term_code) > sizeof(prompt) - strlen(prompt))
            {
                strncat(prompt + sizeof(prompt) - strlen(term_code) - 1, term_code, strlen(term_code));
            }
            else
            {
                strncat(p, term_code, sizeof(term_code));
            }
        }
    }
    else if (remotehostname)
        snprintf(prompt, sizeof(prompt), CALLWEAVER_PROMPT2, remotehostname);
    else
        snprintf(prompt, sizeof(prompt), CALLWEAVER_PROMPT);

    return (prompt);
}

static char **cw_rl_strtoarr(char *buf)
{
    char **match_list = NULL, *retstr;
    size_t match_list_len;
    int matches = 0;

    match_list_len = 1;
    while ( (retstr = strsep(&buf, " ")) != NULL)
    {

        if (!strcmp(retstr, CW_CLI_COMPLETE_EOF))
            break;
        if (matches + 1 >= match_list_len)
        {
            match_list_len <<= 1;
            match_list = realloc(match_list, match_list_len * sizeof(char *));
        }

        match_list[matches++] = strdup(retstr);
    }

    if (!match_list)
        return (char **) NULL;

    if (matches>= match_list_len)
        match_list = realloc(match_list, (match_list_len + 1) * sizeof(char *));

    match_list[matches] = (char *) NULL;

    return match_list;
}

static char *dummy_completer(char *text, int state)
{
    return (char*)NULL;
}

static char **cli_completion(const char *text, int start, int end)
{
    int nummatches = 0;
    char buf[2048];
    char **matches;
    int res;

    matches = (char**)NULL;
    if (option_remote)
    {
        snprintf(buf, sizeof(buf),"_COMMAND NUMMATCHES \"%s\" \"%s\"", (char *)rl_line_buffer, (char *)text);
        fdprint(cw_consock, buf);
        res = read(cw_consock, buf, sizeof(buf));
        buf[res] = '\0';
        nummatches = atoi(buf);

        if (nummatches > 0)
        {
            char *mbuf;
            int mlen = 0, maxmbuf = 2048;
            // Start with a 2048 byte buffer
            mbuf = malloc(maxmbuf);
            if (!mbuf)
                return (matches);

            snprintf(buf, sizeof(buf),"_COMMAND MATCHESARRAY \"%s\" \"%s\"", (char *)rl_line_buffer, (char *)text);
            fdprint(cw_consock, buf);
            res = 0;
            mbuf[0] = '\0';

            while (!strstr(mbuf, CW_CLI_COMPLETE_EOF) && res != -1)
            {
                if (mlen + 1024 > maxmbuf)
                {
                    // Every step increment buffer 1024 bytes
                    maxmbuf += 1024;
                    mbuf = realloc(mbuf, maxmbuf);
                    if (!mbuf)
                        return (matches);
                }
                // Only read 1024 bytes at a time
                res = read(cw_consock, mbuf + mlen, 1024);
                if (res > 0)
                    mlen += res;
            }
            mbuf[mlen] = '\0';

            matches = cw_rl_strtoarr(mbuf);
            free(mbuf);
        }

    }
    else
    {
        nummatches = cw_cli_generatornummatches((char *)rl_line_buffer, (char*)text);

        if (nummatches > 0 )
            matches = cw_cli_completion_matches((char*)rl_line_buffer, (char*)text);
    }
    return (matches);
}

static int cw_rl_initialize(void)
{
    /*
    char *editor = getenv("CW_EDITOR");
    */
    rl_initialize ();
    rl_editing_mode = 1;
    /* start history*/
    using_history();
#ifdef __Darwin__
    rl_completion_entry_function = (Function *)dummy_completer;
    rl_attempted_completion_function = (CPPFunction *)cli_completion;
#else
    rl_completion_entry_function = (rl_compentry_func_t *) dummy_completer;
    rl_attempted_completion_function = (CPPFunction *) cli_completion;
#endif
    rl_prep_terminal (0);

    /* setup history with 100 entries */
    stifle_history(100);

    rl_init = 1;
    return 0;
}

static int cw_rl_add_history(char *buf)
{
    HIST_ENTRY *last;

    if (strlen(buf) > 256)
        return 0;

    if (!rl_init)
        cw_rl_initialize();

    last = previous_history();
    if (!last || strcmp (last->line, buf) != 0)
    {
        add_history (buf);
        return 1;
    }
    return 0;
}

static int cw_rl_write_history(char *filename)
{
    if (!rl_init)
        cw_rl_initialize();

    return write_history(filename);
}

static int cw_rl_read_history(char *filename)
{
    if (!rl_init)
        cw_rl_initialize();

    return read_history(filename);
}

static void cw_remotecontrol(char * data)
{
    char buf[80];
    int res;
    char filename[80] = "";
    char *hostname;
    char *cpid;
    char *version;
    int pid;
    char tmp[80];
    char *stringp = NULL;

    char *ebuf = NULL;

    read(cw_consock, buf, sizeof(buf));
    if (data)
        write(cw_consock, data, strlen(data) + 1);
    stringp=buf;
    hostname = strsep(&stringp, "/");
    cpid = strsep(&stringp, "/");
    version = strsep(&stringp, "\n");
    if (!version)
        version = "<Version Unknown>";
    stringp=hostname;
    strsep(&stringp, ".");
    if (cpid)
        pid = atoi(cpid);
    else
        pid = -1;
    snprintf(tmp, sizeof(tmp), "set verbose atleast %d", option_verbose);
    fdprint(cw_consock, tmp);
    snprintf(tmp, sizeof(tmp), "set debug atleast %d", option_debug);
    fdprint(cw_consock, tmp);
    cw_verbose("Connected to CallWeaver %s currently running on %s (pid = %d)\n", version, hostname, pid);
    remotehostname = hostname;
    if (getenv("HOME"))
        snprintf(filename, sizeof(filename), "%s/.callweaver_history", getenv("HOME"));

    if (!rl_init)
        cw_rl_initialize();

    if (!cw_strlen_zero(filename))
        cw_rl_read_history(filename);

    FILE tempchar;
    struct pollfd fds[0];
    fds[0].fd = cw_consock;
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    if (option_exec && data)    /* hack to print output then exit if callweaver -rx is used */
    {
        while (poll(fds, 1, 100) > 0)
        {
            cw_rl_read_char(&tempchar);
        }
        return;
    }

#ifdef __Darwin__
    rl_event_hook = cw_rl_out_event;
#else
    rl_getc_function = cw_rl_read_char;
#endif
    for (;;)
    {
        if (ebuf)
        {
            free (ebuf);
            ebuf = (char *)NULL;
        }


        ebuf = readline(cli_prompt());

        if (!cw_strlen_zero(ebuf))
        {
            if (ebuf[strlen(ebuf)-1] == '\n')
                ebuf[strlen(ebuf)-1] = '\0';
            if (!remoteconsolehandler(ebuf))
            {
                res = write(cw_consock, ebuf, strlen(ebuf) + 1);
                if (res < 1)
                {
                    cw_log(LOG_WARNING, "Unable to write: %s\n", strerror(errno));
                    break;
                }
            }
        }

    }

    if (ebuf)
    {
        free(ebuf);
        ebuf = (char *)NULL;
    }
    printf("\nDisconnected from CallWeaver server\n");
}

static int show_version(void)
{
    printf( PACKAGE_STRING SVN_VERSION "\n");
    return 0;
}

static int show_cli_help(void)
{
    printf( PACKAGE_STRING SVN_VERSION  "\n");
    printf("Usage: callweaver [OPTIONS]\n");
    printf("Valid Options:\n");
    printf("   -V              Display version number and exit\n");
    printf("   -C <configfile> Use an alternate configuration file\n");
    printf("   -G <group>      Run as a group other than the caller\n");
    printf("   -U <user>       Run as a user other than the caller\n");
    printf("   -c              Provide console CLI\n");
    printf("   -d              Enable extra debugging\n");
    printf("   -f              Do not fork\n");
    printf("   -g              Dump core in case of a crash\n");
    printf("   -h              This help screen\n");
    printf("   -i              Initialize crypto keys at startup\n");
    printf("   -n              Disable console colorization at startup or console (not remote)\n");
    printf("   -p              Run as pseudo-realtime thread\n");
    printf("   -q              Quiet mode (suppress output)\n");
    printf("   -r              Connect to CallWeaver on this machine\n");
    printf("   -R              Connect to CallWeaver, and attempt to reconnect if disconnected\n");
    printf("   -t              Record soundfiles in /var/tmp and move them where they belong after they are done.\n");
    printf("   -T              Display the time in [Mmm dd hh:mm:ss] format for each line of output to the CLI.\n");
    printf("   -v              Increase verbosity (multiple v's = more verbose)\n");
    printf("   -x <cmd>        Execute command <cmd> (only valid with -r)\n");
    printf("\n");
    return 0;
}

static void cw_readconfig(void)
{
    struct cw_config *cfg;
    struct cw_variable *v;
    char *config = cw_config_CW_CONFIG_FILE;

    if (option_overrideconfig == 1)
    {
        cfg = cw_config_load(cw_config_CW_CONFIG_FILE);
        if (!cfg)
            cw_log(LOG_WARNING, "Unable to open specified master config file '%s', using built-in defaults\n", cw_config_CW_CONFIG_FILE);
    }
    else
    {
        cfg = cw_config_load(config);
    }

    /* init with buildtime config */

    cw_copy_string(cw_config_CW_RUN_USER, cwrunuser_default, sizeof(cw_config_CW_RUN_USER));
    cw_copy_string(cw_config_CW_RUN_GROUP, cwrungroup_default, sizeof(cw_config_CW_RUN_GROUP));
    cw_copy_string(cw_config_CW_CONFIG_DIR, cwconfdir_default, sizeof(cw_config_CW_CONFIG_DIR));
    cw_copy_string(cw_config_CW_SPOOL_DIR, cwspooldir_default, sizeof(cw_config_CW_SPOOL_DIR));
    cw_copy_string(cw_config_CW_MODULE_DIR, cwmoddir_default, sizeof(cw_config_CW_MODULE_DIR));
    snprintf(cw_config_CW_MONITOR_DIR, sizeof(cw_config_CW_MONITOR_DIR) - 1, "%s/monitor", cw_config_CW_SPOOL_DIR);
    cw_copy_string(cw_config_CW_VAR_DIR, cwvardir_default, sizeof(cw_config_CW_VAR_DIR));
    cw_copy_string(cw_config_CW_LOG_DIR, cwlogdir_default, sizeof(cw_config_CW_LOG_DIR));
    cw_copy_string(cw_config_CW_OGI_DIR, cwogidir_default, sizeof(cw_config_CW_OGI_DIR));
    cw_copy_string(cw_config_CW_DB, cwdbfile_default, sizeof(cw_config_CW_DB));
    cw_copy_string(cw_config_CW_DB_DIR, cwdbdir_default, sizeof(cw_config_CW_DB_DIR));
    cw_copy_string(cw_config_CW_KEY_DIR, cwkeydir_default, sizeof(cw_config_CW_KEY_DIR));
    cw_copy_string(cw_config_CW_PID, cwpidfile_default, sizeof(cw_config_CW_PID));
    cw_copy_string(cw_config_CW_SOCKET, cwsocketfile_default, sizeof(cw_config_CW_SOCKET));
    cw_copy_string(cw_config_CW_RUN_DIR, cwrundir_default, sizeof(cw_config_CW_RUN_DIR));
    cw_copy_string(cw_config_CW_SOUNDS_DIR, cwsoundsdir_default, sizeof(cw_config_CW_SOUNDS_DIR));

    /* no callweaver.conf? no problem, use buildtime config! */
    if (!cfg)
    {
        return;
    }
    v = cw_variable_browse(cfg, "general");
    while (v)
    {
        if (!strcasecmp(v->name, "cwrunuser"))
        {
            cw_copy_string(cw_config_CW_RUN_USER, v->value, sizeof(cw_config_CW_RUN_USER));
        }
        else if (!strcasecmp(v->name, "cwrungroup"))
        {
            cw_copy_string(cw_config_CW_RUN_GROUP, v->value, sizeof(cw_config_CW_RUN_GROUP));
        }
        v = v->next;
    }
    v = cw_variable_browse(cfg, "files");
    while (v)
    {
        if (!strcasecmp(v->name, "cwctlpermissions"))
        {
            cw_copy_string(cw_config_CW_CTL_PERMISSIONS, v->value, sizeof(cw_config_CW_CTL_PERMISSIONS));
        }
        else if (!strcasecmp(v->name, "cwctlowner"))
        {
            cw_copy_string(cw_config_CW_CTL_OWNER, v->value, sizeof(cw_config_CW_CTL_OWNER));
        }
        else if (!strcasecmp(v->name, "cwctlgroup"))
        {
            cw_copy_string(cw_config_CW_CTL_GROUP, v->value, sizeof(cw_config_CW_CTL_GROUP));
        }
        else if (!strcasecmp(v->name, "cwctl"))
        {
            cw_copy_string(cw_config_CW_CTL, v->value, sizeof(cw_config_CW_CTL));
        }
        else if (!strcasecmp(v->name, "cwdb"))
        {
            cw_copy_string(cw_config_CW_DB, v->value, sizeof(cw_config_CW_DB));
        }
        v = v->next;
    }
    v = cw_variable_browse(cfg, "directories");
    while (v)
    {
        if (!strcasecmp(v->name, "cwetcdir"))
        {
            cw_copy_string(cw_config_CW_CONFIG_DIR, v->value, sizeof(cw_config_CW_CONFIG_DIR));
        }
        else if (!strcasecmp(v->name, "cwspooldir"))
        {
            cw_copy_string(cw_config_CW_SPOOL_DIR, v->value, sizeof(cw_config_CW_SPOOL_DIR));
            snprintf(cw_config_CW_MONITOR_DIR, sizeof(cw_config_CW_MONITOR_DIR) - 1, "%s/monitor", v->value);
        }
        else if (!strcasecmp(v->name, "cwvarlibdir"))
        {
            cw_copy_string(cw_config_CW_VAR_DIR, v->value, sizeof(cw_config_CW_VAR_DIR));
        }
        else if (!strcasecmp(v->name, "cwdbdir"))
        {
            cw_copy_string(cw_config_CW_DB_DIR, v->value, sizeof(cw_config_CW_DB_DIR));
        }
        else if (!strcasecmp(v->name, "cwlogdir"))
        {
            cw_copy_string(cw_config_CW_LOG_DIR, v->value, sizeof(cw_config_CW_LOG_DIR));
        }
        else if (!strcasecmp(v->name, "cwogidir"))
        {
            cw_copy_string(cw_config_CW_OGI_DIR, v->value, sizeof(cw_config_CW_OGI_DIR));
        }
        else if (!strcasecmp(v->name, "cwsoundsdir"))
        {
            cw_copy_string(cw_config_CW_SOUNDS_DIR, v->value, sizeof(cw_config_CW_SOUNDS_DIR));
        }
        else if (!strcasecmp(v->name, "cwrundir"))
        {
            snprintf(cw_config_CW_PID, sizeof(cw_config_CW_PID), "%s/%s", v->value, "callweaver.pid");
            snprintf(cw_config_CW_SOCKET, sizeof(cw_config_CW_SOCKET), "%s/%s", v->value, cw_config_CW_CTL);
            cw_copy_string(cw_config_CW_RUN_DIR, v->value, sizeof(cw_config_CW_RUN_DIR));
        }
        else if (!strcasecmp(v->name, "cwmoddir"))
        {
            cw_copy_string(cw_config_CW_MODULE_DIR, v->value, sizeof(cw_config_CW_MODULE_DIR));
        }
        else if (!strcasecmp(v->name, "cwkeydir"))
        {
            cw_copy_string(cw_config_CW_KEY_DIR, v->value, sizeof(cw_config_CW_MODULE_DIR));
        }
        v = v->next;
    }
    v = cw_variable_browse(cfg, "options");
    while (v)
    {
        /* verbose level (-v at startup) */
        if (!strcasecmp(v->name, "verbose"))
        {
            option_verbose = atoi(v->value);
            /* whether or not to force timestamping. (-T at startup) */
        }
        else if (!strcasecmp(v->name, "timestamp"))
        {
            option_timestamp = cw_true(v->value);
            /* whether or not to support #exec in config files */
        }
        else if (!strcasecmp(v->name, "execincludes"))
        {
            option_exec_includes = cw_true(v->value);
            /* debug level (-d at startup) */
        }
        else if (!strcasecmp(v->name, "debug"))
        {
            option_debug = 0;
            if (sscanf(v->value, "%d", &option_debug) != 1)
            {
                option_debug = cw_true(v->value);
            }
            /* Disable forking (-f at startup) */
        }
        else if (!strcasecmp(v->name, "nofork"))
        {
            option_nofork = cw_true(v->value);
            /* Run quietly (-q at startup ) */
        }
        else if (!strcasecmp(v->name, "quiet"))
        {
            option_quiet = cw_true(v->value);
            /* Run as console (-c at startup, implies nofork) */
        }
        else if (!strcasecmp(v->name, "console"))
        {
            option_console = cw_true(v->value);
            /* Run with highg priority if the O/S permits (-p at startup) */
        }
        else if (!strcasecmp(v->name, "highpriority"))
        {
            option_highpriority = cw_true(v->value);
            /* Initialize RSA auth keys (IAX2) (-i at startup) */
        }
        else if (!strcasecmp(v->name, "initcrypto"))
        {
            option_initcrypto = cw_true(v->value);
            /* Disable ANSI colors for console (-c at startup) */
        }
        else if (!strcasecmp(v->name, "nocolor"))
        {
            option_nocolor = cw_true(v->value);
            /* Disable some usage warnings for picky people :p */
        }
        else if (!strcasecmp(v->name, "dontwarn"))
        {
            option_dontwarn = cw_true(v->value);
            /* Dump core in case of crash (-g) */
        }
        else if (!strcasecmp(v->name, "dumpcore"))
        {
            option_dumpcore = cw_true(v->value);
            /* Cache recorded sound files to another directory during recording */
        }
        else if (!strcasecmp(v->name, "cache_record_files"))
        {
            option_cache_record_files = cw_true(v->value);
            /* Specify cache directory */
        }
        else if (!strcasecmp(v->name, "record_cache_dir"))
        {
            cw_copy_string(record_cache_dir, v->value, CW_CACHE_DIR_LEN);
            /* Build transcode paths via SLINEAR, instead of directly */
        }
        else if (!strcasecmp(v->name, "transcode_via_sln"))
        {
            option_transcode_slin = cw_true(v->value);
        }
        else if (!strcasecmp(v->name, "maxcalls"))
        {
            if ((sscanf(v->value, "%d", &option_maxcalls) != 1) || (option_maxcalls < 0))
            {
                option_maxcalls = 0;
            }
        }
        else if (!strcasecmp(v->name, "maxload"))
        {
            double test[1];

            if (getloadavg(test, 1) == -1)
            {
                cw_log(LOG_ERROR, "Cannot obtain load average on this system. 'maxload' option disabled.\n");
                option_maxload = 0.0;
            }
            else if ((sscanf(v->value, "%lf", &option_maxload) != 1) || (option_maxload < 0.0))
            {
                option_maxload = 0.0;
            }
        }
        else if (!strcasecmp(v->name, "systemname"))
        {
            cw_copy_string(cw_config_CW_SYSTEM_NAME, v->value, sizeof(cw_config_CW_SYSTEM_NAME));
        }
		else if (!strcasecmp(v->name, "enablespaghetticode"))
		{
            cw_copy_string(cw_config_CW_ALLOW_SPAGHETTI_CODE, v->value, sizeof(cw_config_CW_ALLOW_SPAGHETTI_CODE));
		}
        v = v->next;
    }
    cw_config_destroy(cfg);
}

static void cw_exit(int val)
{
    printf(cw_term_quit());
    if (rl_init)
        rl_deprep_terminal();

    exit(val);
}

int callweaver_main(int argc, char *argv[])
{
    int c;
    char filename[80] = "";
    char hostname[MAXHOSTNAMELEN]="";
    char tmp[80];
    char * xarg = NULL;
    int x;
    FILE *f;
    sigset_t sigs;
    int is_child_of_nonroot=0;
    char *buf = NULL;
    static char *runuser = NULL, *rungroup = NULL;


    /* init with default */
    cw_copy_string(cw_config_CW_CONFIG_FILE, cwconffile_default, sizeof(cw_config_CW_CONFIG_FILE));

    /* Remember original args for restart */
    if (argc > sizeof(_argv) / sizeof(_argv[0]) - 1)
    {
        fprintf(stderr, "Truncating argument size to %d\n", (int)(sizeof(_argv) / sizeof(_argv[0])) - 1);
        argc = sizeof(_argv) / sizeof(_argv[0]) - 1;
    }
    for (x=0;x<argc;x++)
        _argv[x] = argv[x];
    _argv[x] = NULL;

    /* if the progname is rcallweaver consider it a remote console */
    if (argv[0] && (strstr(argv[0], "rcallweaver")) != NULL)
    {
        option_remote++;
        option_nofork++;
    }
    if (gethostname(hostname, sizeof(hostname) - 1))
        cw_copy_string(hostname, "<Unknown>", sizeof(hostname));
    cw_mainpid = getpid();
    cw_ulaw_init();
    cw_alaw_init();
    cw_utils_init();
    /* When CallWeaver restarts after it has dropped the root privileges,
     * it can't issue setuid(), setgid(), setgroups() or set_priority()
     * */
    if (getenv("CALLWEAVER_ALREADY_NONROOT"))
        is_child_of_nonroot = 1;
    if (getenv("HOME"))
        snprintf(filename, sizeof(filename), "%s/.callweaver_history", getenv("HOME"));
    /* Check for options */
    while ((c=getopt(argc, argv, "tThfdvVqprRgcinx:U:G:C:L:M:")) != -1)
    {
        switch (c)
        {
        case 'd':
            option_debug++;
            break;
        case 'c':
            option_console++;
            option_nofork++;
            break;
        case 'f':
            option_nofork++;
            break;
        case 'n':
            option_nocolor++;
            break;
        case 'r':
            option_remote++;
            option_nofork++;
            break;
        case 'R':
            option_remote++;
            option_nofork++;
            option_reconnect++;
            break;
        case 'p':
            option_highpriority++;
            break;
        case 'v':
            option_verbose++;
            break;
        case 'M':
            if ((sscanf(optarg, "%d", &option_maxcalls) != 1) || (option_maxcalls < 0))
                option_maxcalls = 0;
            break;
        case 'L':
            if ((sscanf(optarg, "%lf", &option_maxload) != 1) || (option_maxload < 0.0))
                option_maxload = 0.0;
            break;
        case 'q':
            option_quiet++;
            break;
        case 't':
            option_cache_record_files++;
            break;
        case 'T':
            option_timestamp++;
            break;
        case 'x':
            option_exec++;
            xarg = optarg;
            break;
        case 'C':
            cw_copy_string((char *)cw_config_CW_CONFIG_FILE,optarg,sizeof(cw_config_CW_CONFIG_FILE));
            option_overrideconfig++;
            break;
        case 'i':
            option_initcrypto++;
            break;
        case 'g':
            option_dumpcore++;
            break;
        case 'h':
            show_cli_help();
            exit(0);
        case 'V':
            show_version();
            exit(0);
        case 'U':
            runuser = optarg;
            break;
        case 'G':
            rungroup = optarg;
            break;
        case '?':
            exit(1);
        }
    }

    /* For remote connections, change the name of the remote connection.
     * We do this for the benefit of init scripts (which need to know if/when
     * the main callweaver process has died yet). */
    if (option_remote)
    {
        strcpy(argv[0], "rcallweaver");
        for (x = 1; x < argc; x++)
        {
            argv[x] = argv[0] + 10;
        }
    }

    if (option_console && !option_verbose)
        cw_verbose("[ Reading Master Configuration ]");
    cw_readconfig();

    if (option_dumpcore)
    {
        struct rlimit l;
        memset(&l, 0, sizeof(l));
        l.rlim_cur = RLIM_INFINITY;
        l.rlim_max = RLIM_INFINITY;
        if (setrlimit(RLIMIT_CORE, &l))
        {
            cw_log(LOG_WARNING, "Unable to disable core size resource limit: %s\n", strerror(errno));
        }
    }


    if (!is_child_of_nonroot && cw_set_priority(option_highpriority))
    {
        exit(1);
    }
    if (!runuser)
        runuser = cw_config_CW_RUN_USER;
    if (!rungroup)
        rungroup = cw_config_CW_RUN_GROUP;
    if (!is_child_of_nonroot)
    {
        struct group *gr;
        struct passwd *pw;
#if defined(__linux__)
        cap_user_header_t cap_header;
        cap_user_data_t cap_data;

        /* inherit our capabilities */
        if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) == -1)
        {
            cw_log(LOG_WARNING, "Unable to keep capabilities: %s\n", strerror(errno));
        }
#endif

        gr = getgrnam(rungroup);
        if (!gr)
        {
            cw_log(LOG_ERROR, "No such group '%s'!\n", rungroup);
            exit(1);
        }
        pw = getpwnam(runuser);
        if (!pw)
        {
            cw_log(LOG_ERROR, "No such user '%s'!\n", runuser);
            exit(1);
        }

        if (gr->gr_gid != getegid() )
            if (initgroups(pw->pw_name, gr->gr_gid) == -1)
            {
                cw_log(LOG_ERROR, "Unable to initgroups '%s' (%d)\n", pw->pw_name, gr->gr_gid);
                exit(1);
            }

        if (setregid(gr->gr_gid, gr->gr_gid))
        {
            cw_log(LOG_ERROR, "Unable to setgid to '%s' (%d)\n", gr->gr_name, gr->gr_gid);
            exit(1);
        }
        if (option_verbose)
        {
            int ngroups;
            gid_t gid_list[NGROUPS_MAX];
            int i;
            struct group *gr2;

            gr2 = getgrgid(getegid());
            if (gr2)
            {
                cw_verbose("Now running as group '%s' (%d)\n", gr2->gr_name, gr2->gr_gid);
            }
            else
            {
                cw_verbose("Now running as group '' (%d)\n", getegid());
            }

            cw_verbose("Supplementary groups:\n");
            ngroups = getgroups(NGROUPS_MAX, gid_list);
            for (i = 0; i < ngroups; i++)
            {
                gr2 = getgrgid(gid_list[i]);
                if (gr2)
                {
                    cw_verbose("   '%s' (%d)\n", gr2->gr_name, gr2->gr_gid);
                }
                else
                {
                    cw_verbose("   '' (%d)\n", gid_list[i]);
                }
            }
        }
#ifdef __Darwin__
        if (seteuid(pw->pw_uid))
        {
#else
        if (setreuid(pw->pw_uid, pw->pw_uid))
        {
#endif
            cw_log(LOG_ERROR, "Unable to setuid to '%s' (%d)\n", pw->pw_name, pw->pw_uid);
            exit(1);
        }
        setenv("CALLWEAVER_ALREADY_NONROOT","yes",1);
        if (option_verbose)
        {
            struct passwd *pw2;
            pw2 = getpwuid(geteuid());
            if (pw2)
            {
                cw_verbose("Now running as user '%s' (%d)\n", pw2->pw_name, pw2->pw_uid);
            }
            else
            {
                cw_verbose("Now running as user '' (%d)\n", getegid());
            }
        }

#if defined(__linux__)
        cap_header = alloca(sizeof(*cap_header));
        cap_data = alloca(sizeof(*cap_data));
        cap_header->version = _LINUX_CAPABILITY_VERSION;
        cap_header->pid = 0;
        cap_data->effective = 1 << CAP_NET_ADMIN;
#ifdef HAVE_LINUX_VISDN_ROUTER_H
        cap_data->effective |= 1 << CAP_NET_BIND_SERVICE;
#endif
        cap_data->permitted = cap_data->effective;
        cap_data->inheritable = 0;
        /* set capabilities including NET_ADMIN */
        /* this allows us to e.g. set all TOS bits */

        if (gr->gr_gid != getegid() )
            if (capset(cap_header, cap_data) == -1)
            {
                cw_log(LOG_ERROR, "Unable to set new capabilities (CAP_NET_ADMIN"
#ifdef HAVE_LINUX_VISDN_ROUTER_H
                         "|CAP_NET_BIND_SERVICE"
#endif
                         ")\n");
                exit(1);
            }
#endif
    }

    /* Check if we're root */
    if (!geteuid())
    {
#ifdef VERY_SECURE
        cw_log(LOG_ERROR, "Running as root has been disabled\n");
        exit(1);
#else
        cw_log(LOG_ERROR, "Running as root has been enabled\n");
#endif /* VERY_SECURE */
    }



#if defined(__linux__)
    /* after set*id() the dumpable flag is deleted,
       so we set it again to get core dumps */
    if (option_dumpcore)
    {
        if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1)
        {
            cw_log(LOG_ERROR, "Unable to set dumpable flag: %s\n", strerror(errno));
        }
    }
#endif

    cw_term_init();
    printf(cw_term_end());
    fflush(stdout);

    if (option_console && !option_verbose)
        cw_verbose("[ Initializing Custom Configuration Options ]");
    /* custom config setup */
    register_config_cli();
    read_config_maps();


    if (option_console)
    {

        if (!rl_init)
            cw_rl_initialize();

        if (!cw_strlen_zero(filename))
            cw_rl_read_history(filename);
    }

    if (cw_tryconnect())
    {
        /* One is already running */
        if (option_remote)
        {
            if (option_exec)
            {
                cw_remotecontrol(xarg);
                quit_handler(0, 0, 0, 0);
                exit(0);
            }
            printf(cw_term_quit());
            cw_register_verbose(console_verboser);
            WELCOME_MESSAGE;
            cw_remotecontrol(NULL);
            quit_handler(0, 0, 0, 0);
            exit(0);
        }
        else
        {
            cw_log(LOG_ERROR, "CallWeaver already running on %s.  Use 'callweaver -r' to connect.\n", (char *)cw_config_CW_SOCKET);
            printf(cw_term_quit());
            if (rl_init)
                rl_deprep_terminal();
            exit(1);
        }
    }
    else if (option_remote || option_exec)
    {
        cw_log(LOG_ERROR, "Unable to connect to remote callweaver (does %s exist?)\n",cw_config_CW_SOCKET);
        printf(cw_term_quit());
        if (rl_init)
            rl_deprep_terminal();
        exit(1);
    }

    /* Blindly write pid file since we couldn't connect */
    unlink((char *)cw_config_CW_PID);
    f = fopen((char *)cw_config_CW_PID, "w");
    if (f)
    {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
    else
        cw_log(LOG_WARNING, "Unable to open pid file '%s': %s\n", (char *)cw_config_CW_PID, strerror(errno));

    if (!option_nofork)
    {
        daemon(1,0);
        /* Blindly re-write pid file since we are forking */
        unlink((char *)cw_config_CW_PID);
        f = fopen((char *)cw_config_CW_PID, "w");
        if (f)
        {
            fprintf(f, "%d\n", getpid());
            fclose(f);
        }
        else
            cw_log(LOG_WARNING, "Unable to open pid file '%s': %s\n", (char *)cw_config_CW_PID, strerror(errno));
    }

    /* Test recursive mutex locking. */
    if (test_for_thread_safety())
        cw_verbose("Warning! CallWeaver is not thread safe.\n");

    cw_makesocket();
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGHUP);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGPIPE);
    sigaddset(&sigs, SIGWINCH);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    if (option_console || option_verbose || option_remote)
        cw_register_verbose(console_verboser);
    /* Print a welcome message if desired */
    if (option_verbose || option_console)
    {
        WELCOME_MESSAGE;
    }
    if (option_console && !option_verbose)
        cw_verbose("[ Booting...");

    signal(SIGURG, urg_handler);
    signal(SIGINT, __quit_handler);
    signal(SIGTERM, __quit_handler);
    signal(SIGHUP, hup_handler);
    signal(SIGCHLD, child_handler);
    signal(SIGPIPE, SIG_IGN);

    /* ensure that the random number generators are seeded with a different value every time
       CallWeaver is started
    */
    srand((unsigned int) getpid() + (unsigned int) time(NULL));
    srandom((unsigned int) getpid() + (unsigned int) time(NULL));

    if (init_logger())
    {
        cw_exit(1);
    }
    if (dnsmgr_init())
    {
        cw_exit(1);
    }
    /* initialize module loader */
    if (cw_loader_init() < 0)
    {
        cw_exit(1);
    }
    /* load 'preload' modules, required for access to Realtime-mapped configuration files */
    if (load_modules(1))
    {
        cw_exit(1);
    }
    cw_channels_init();
    if (cw_cdr_engine_init())
    {
        cw_exit(1);
    }
    if (init_manager())
    {
        cw_exit(1);
    }
    if (cw_device_state_engine_init())
    {
        cw_exit(1);
    }
    cw_rtp_init();
    cw_udptl_init();
    cw_stun_init();

    if (cw_image_init())
    {
        cw_exit(1);
    }
    if (cw_file_init())
    {
        cw_exit(1);
    }
    if (load_pbx())
    {
        cw_exit(1);
    }
    if (cwdb_init())
    {
        cw_exit(1);
    }
    if (init_framer())
    {
        cw_exit(1);
    }
    if (load_modules(0))
    {
        cw_exit(1);
    }
    if (cw_enum_init())
    {
        cw_exit(1);
    }
#if 0
    /* This should no longer be necessary */
    /* sync cust config and reload some internals in case a custom config handler binded to them */
    read_cw_cust_config();
    reload_logger(0);
    cw_enum_reload();
    cw_rtp_reload();
#endif

    /* We might have the option of showing a console, but for now just
       do nothing... */
    if (option_console && !option_verbose)
        cw_verbose(" ]\n");
    if (option_verbose || option_console)
        cw_verbose(cw_term_color(tmp, "CallWeaver Ready.\n", COLOR_BRWHITE, COLOR_BLACK, sizeof(tmp)));
    if (option_nofork)
        consolethread = pthread_self();
    fully_booted = 1;
    pthread_sigmask(SIG_UNBLOCK, &sigs, NULL);
#ifdef __CW_DEBUG_MALLOC
    __cw_mm_init();
#endif
    time(&cw_startuptime);
    cw_cli_register_multiple(core_cli, sizeof(core_cli) / sizeof(core_cli[0]));
    if (option_console)
    {
        /* Console stuff now... */
        /* Register our quit function */
        char title[256];
        set_icon("CallWeaver");
        snprintf(title, sizeof(title), "CallWeaver Console on '%s' (pid %d)", hostname, cw_mainpid);
        set_title(title);

        for (;;)
        {
            if (buf)
            {
                free (buf);
                buf = (char *)NULL;
            }
            buf = readline(cli_prompt());

            if (buf)
            {
                if ( (strlen(buf)>0) && buf[strlen(buf)-1] == '\n')
                    buf[strlen(buf)-1] = '\0';

                consolehandler(buf);
            }
            else
            {
                if (write(STDOUT_FILENO, "\nUse EXIT or QUIT to exit the callweaver console\n",
                          strlen("\nUse EXIT or QUIT to exit the callweaver console\n")) < 0)
                {
                    /* Whoa, stdout disappeared from under us... Make /dev/null's */
                    int fd;
                    fd = open("/dev/null", O_RDWR);
                    if (fd > -1)
                    {
                        dup2(fd, STDOUT_FILENO);
                        dup2(fd, STDIN_FILENO);
                    }
                    else
                        cw_log(LOG_WARNING, "Failed to open /dev/null to recover from dead console.  Bad things will happen!\n");

                    printf(cw_term_quit());
                    break;
                }
            }
        }
        if (buf)
        {
            free(buf);
            buf = (char *)NULL;
        }
    }
    /* Do nothing */
    for (;;)   	/* apparently needed for the MACos */
    {
        struct pollfd p =
        {
            -1 /* no descriptor */, 0, 0
        };
        poll(&p, 0, -1);
    }

    if (rl_init)
        rl_deprep_terminal();

    return 0;
}
