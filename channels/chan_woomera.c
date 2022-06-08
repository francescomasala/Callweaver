/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Woomera Channel Driver
 * 
 * Copyright (C) 2005 Anthony Minessale II
 *
 * Anthony Minessale II <anthmct@yahoo.com>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <netinet/tcp.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/sched.h"
#include "openpbx/astobj.h"
#include "openpbx/lock.h"
#include "openpbx/manager.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/cli.h"
#include "openpbx/logger.h"
#include "openpbx/frame.h"
#include "openpbx/config.h"
#include "openpbx/module.h"
#include "openpbx/lock.h"
#include "openpbx/translate.h"

#define MEDIA_ANSWER "ANSWER"
#define USE_ANSWER 0



static const char desc[] = "Woomera Channel Driver";
static const char type[] = "WOOMERA";
static const char tdesc[] = "Woomera Channel Driver";
static char configfile[] = "woomera.conf";

#define WOOMERA_STRLEN 256
#define WOOMERA_ARRAY_LEN 50
#define WOOMERA_MIN_PORT 9900
#define WOOMERA_MAX_PORT 9999
#define WOOMERA_BODYLEN 2048
#define WOOMERA_LINE_SEPERATOR "\r\n"
#define WOOMERA_RECORD_SEPERATOR "\r\n\r\n"
#define WOOMERA_DEBUG_PREFIX "**[WOOMERA]** "
#define WOOMERA_DEBUG_LINE "--------------------------------------------------------------------------------" 
#define WOOMERA_HARD_TIMEOUT -10000
#define WOOMERA_QLEN 10

#define FRAME_LEN 480
static int WFORMAT = OPBX_FORMAT_SLINEAR;

typedef enum {
	WFLAG_EXISTS = (1 << 0),
	WFLAG_EVENT = (1 << 1),
	WFLAG_CONTENT = (1 << 2),
} WFLAGS;


typedef enum {
	WCFLAG_NOWAIT = (1 << 0)
} WCFLAGS;


typedef enum {
	PFLAG_INBOUND = (1 << 0),
	PFLAG_OUTBOUND = (1 << 1),
	PFLAG_DYNAMIC = (1 << 2),
	PFLAG_DISABLED = (1 << 3)
} PFLAGS;

typedef enum {
	TFLAG_MEDIA = (1 << 0),
	TFLAG_INBOUND = (1 << 1),
	TFLAG_OUTBOUND = (1 << 2),
	TFLAG_INCOMING = (1 << 3),
	TFLAG_PARSE_INCOMING = (1 << 4),
	TFLAG_ACTIVATE = (1 << 5),
	TFLAG_DTMF = (1 << 6),
	TFLAG_DESTROY = (1 << 7),
	TFLAG_ABORT = (1 << 8),
	TFLAG_PBX = (1 << 9),
	TFLAG_ANSWER = (1 << 10),
} TFLAGS;

static int usecnt = 0;

struct woomera_message {
	char callid[WOOMERA_STRLEN];
	int mval;
	char command[WOOMERA_STRLEN];
	char command_args[WOOMERA_STRLEN];
	char names[WOOMERA_STRLEN][WOOMERA_ARRAY_LEN];
	char values[WOOMERA_STRLEN][WOOMERA_ARRAY_LEN];
	char body[WOOMERA_BODYLEN];
	unsigned int flags;
	int last;
	struct woomera_message *next;
};


static struct {
	int next_woomera_port;	
	int debug;
	int panic;
	int more_threads;
} globals;

struct woomera_event_queue {
	struct woomera_message *head;
};

struct woomera_profile {
	ASTOBJ_COMPONENTS(struct woomera_profile);
	opbx_mutex_t iolock;
	char woomera_host[WOOMERA_STRLEN];
	int woomera_port;
	char audio_ip[WOOMERA_STRLEN];
	char context[WOOMERA_STRLEN];
	pthread_t thread;
	unsigned int flags;
	int thread_running;
	struct woomera_event_queue event_queue;
};


struct private_object {
	ASTOBJ_COMPONENTS(struct private_object);
	opbx_mutex_t iolock;
	struct opbx_channel *owner;
	struct sockaddr_in udpread;
	struct sockaddr_in udpwrite;
	int command_channel;
	int udp_socket;
	unsigned int flags;
	struct opbx_frame frame;
	short fdata[FRAME_LEN + OPBX_FRIENDLY_OFFSET];
	struct woomera_message call_info;
	struct woomera_profile *profile;
	char dest[WOOMERA_STRLEN];
	int port;
	struct timeval started;
	int timeout;
	char dtmfbuf[WOOMERA_STRLEN];
	char cid_name[WOOMERA_STRLEN];
	char cid_num[WOOMERA_STRLEN];
	pthread_t thread;
	struct woomera_event_queue event_queue;
};

typedef struct private_object private_object;
typedef struct woomera_message woomera_message;
typedef struct woomera_profile woomera_profile;
typedef struct woomera_event_queue woomera_event_queue;

//static struct sched_context *sched;

static struct private_object_container {
    ASTOBJ_CONTAINER_COMPONENTS(private_object);
} private_object_list;

static struct woomera_profile_container {
    ASTOBJ_CONTAINER_COMPONENTS(woomera_profile);
} woomera_profile_list;

static woomera_profile default_profile;

/* some locks you will use for use count and for exclusive access to the main linked-list of private objects */
OPBX_MUTEX_DEFINE_STATIC(usecnt_lock);
OPBX_MUTEX_DEFINE_STATIC(lock);

/* local prototypes */
static void woomera_close_socket(int *socket);
static void global_set_flag(int flags);
static void woomera_printf(woomera_profile *profile, int fd, char *fmt, ...);
static char *woomera_message_header(woomera_message *wmsg, char *key);
static int woomera_enqueue_event(woomera_event_queue *event_queue, woomera_message *wmsg);
static int woomera_dequeue_event(woomera_event_queue *event_queue, woomera_message *wmsg);
static int woomera_message_parse(int fd, woomera_message *wmsg, int timeout, woomera_profile *profile, woomera_event_queue *event_queue);
static int waitfor_socket(int fd, int timeout);
static int woomera_profile_thread_running(woomera_profile *profile, int set, int new);
static int woomera_locate_socket(woomera_profile *profile, int *woomera_socket);
static void *woomera_thread_run(void *obj); 
static void launch_woomera_thread(woomera_profile *profile);
static void destroy_woomera_profile(woomera_profile *profile); 
static woomera_profile *clone_woomera_profile(woomera_profile *new_profile, woomera_profile *default_profile);
static woomera_profile *create_woomera_profile(woomera_profile *default_profile);
static int config_woomera(void);
static int create_udp_socket(char *ip, int port, struct sockaddr_in *sockaddr, int client);
static int connect_woomera(int *new_socket, woomera_profile *profile, int flags);
static int init_woomera(void);
static struct opbx_channel *woomera_new(const char *type, int format, void *data, int *cause);
static int woomera_cli(int fd, int argc, char *argv[]);
static void tech_destroy(private_object *tech_pvt);
static struct opbx_channel *woomera_new(const char *type, int format, void *data, int *cause);
static void launch_tech_thread(private_object *tech_pvt);
static int tech_create_read_socket(private_object *tech_pvt);
static int tech_activate(private_object *tech_pvt);
static void tech_init(private_object *tech_pvt, woomera_profile *profile, int flags);
static void tech_destroy(private_object *tech_pvt);
static void *tech_monitor_thread(void *obj);
static void tech_monitor_in_one_thread(void);
static void launch_tech_thread(private_object *tech_pvt);


/********************CHANNEL METHOD PROTOTYPES*******************
 * You may or may not need all of these methods, remove any unnecessary functions/protos/mappings as needed.
 *
 */
static struct opbx_channel *tech_requester(const char *type, int format, void *data, int *cause);
static int tech_send_digit(struct opbx_channel *self, char digit);
static int tech_call(struct opbx_channel *self, char *dest, int timeout);
static int tech_hangup(struct opbx_channel *self);
static int tech_answer(struct opbx_channel *self);
static struct opbx_frame *tech_read(struct opbx_channel *self);
static struct opbx_frame *tech_exception(struct opbx_channel *self);
static int tech_write(struct opbx_channel *self, struct opbx_frame *frame);
static int tech_indicate(struct opbx_channel *self, int condition);
static int tech_fixup(struct opbx_channel *oldchan, struct opbx_channel *newchan);
static int tech_send_html(struct opbx_channel *self, int subclass, const char *data, int datalen);
static int tech_send_text(struct opbx_channel *self, const char *text);
static int tech_send_image(struct opbx_channel *self, struct opbx_frame *frame);
static int tech_setoption(struct opbx_channel *self, int option, void *data, int datalen);
static int tech_queryoption(struct opbx_channel *self, int option, void *data, int *datalen);
static struct opbx_channel *tech_bridged_channel(struct opbx_channel *self, struct opbx_channel *bridge);
static int tech_transfer(struct opbx_channel *self, const char *newdest);
static enum opbx_bridge_result tech_bridge(struct opbx_channel *chan_a, 
					   struct opbx_channel *chan_b, 
					   int flags, 
					   struct opbx_frame **outframe, 
					   struct opbx_channel **recent_chan,
					   int timeoutms);
static int tech_write_video(struct opbx_channel *self, struct opbx_frame *frame);


/********************************************************************************
 * Constant structure for mapping local methods to the core interface.
 * This structure only needs to contain the methods the channel requires to operate
 * Not every channel needs all of them defined.
 */

static const struct opbx_channel_tech technology = {
	.type = type,
	.description = tdesc,
	.capabilities = -1,
	.requester = tech_requester,
	.send_digit = tech_send_digit,
	.call = tech_call,
	.bridge = tech_bridge,
	.hangup = tech_hangup,
	.answer = tech_answer,
	.transfer = tech_transfer,
	.write_video = tech_write_video,
	.read = tech_read,
	.write = tech_write,
	.exception = tech_exception,
	.indicate = tech_indicate,
	.fixup = tech_fixup,
	.send_html = tech_send_html,
	.send_text = tech_send_text,
	.send_image = tech_send_image,
	.setoption = tech_setoption,
	.queryoption = tech_queryoption,
	.bridged_channel = tech_bridged_channel,
	.transfer = tech_transfer,
};


static void woomera_close_socket(int *socket) 
{

	if (*socket > 0) {
		close(*socket);
	}
	*socket = 0;
}

static void global_set_flag(int flags)
{
	private_object *tech_pvt;

	ASTOBJ_CONTAINER_TRAVERSE(&private_object_list, 1, do {
		ASTOBJ_RDLOCK(iterator);
        tech_pvt = iterator;
		opbx_set_flag(tech_pvt, flags);
		ASTOBJ_UNLOCK(iterator);
    } while(0));
} 



static void woomera_printf(woomera_profile *profile, int fd, char *fmt, ...)
{
    char *stuff;
    int res = 0;

	if (fd <= 0) {
		opbx_log(LOG_ERROR, "Not gonna write to fd %d\n", fd);
		return;
	}
	
    va_list ap;
    va_start(ap, fmt);
#ifdef SOLARIS
	stuff = (char *)malloc(10240);
	vsnprintf(stuff, 10240, fmt, ap);
#else
    res = vasprintf(&stuff, fmt, ap);
#endif
    va_end(ap);
    if (res == -1) {
        opbx_log(LOG_ERROR, "Out of memory\n");
    } else {
		if (profile && globals.debug) {
			opbx_verbose(WOOMERA_DEBUG_PREFIX "Send Message: {%s} [%s/%d]\n%s\n%s", profile->name, profile->woomera_host, profile->woomera_port, WOOMERA_DEBUG_LINE, stuff);
		}
        opbx_carefulwrite(fd, stuff, strlen(stuff), 100);
        free(stuff);
    }

}

static char *woomera_message_header(woomera_message *wmsg, char *key) 
{
	int x = 0;
	char *value = NULL;

	for (x = 0 ; x < wmsg->last ; x++) {
		if (!strcasecmp(wmsg->names[x], key)) {
			value = wmsg->values[x];
			break;
		}
	}

	return value;
}

static int woomera_enqueue_event(woomera_event_queue *event_queue, woomera_message *wmsg)
{
	woomera_message *new, *mptr;

	if ((new = malloc(sizeof(woomera_message)))) {
		memcpy(new, wmsg, sizeof(woomera_message));
		new->next = NULL;

		if (!event_queue->head) {
			event_queue->head = new;
		} else {
			for (mptr = event_queue->head; mptr && mptr->next ; mptr = mptr->next);
			mptr->next = new;
		}
		return 1;
	} else {
		opbx_log(LOG_ERROR, "Memory Allocation Error!\n");
	}

	return 0;
}

static int woomera_dequeue_event(woomera_event_queue *event_queue, woomera_message *wmsg)
{
	woomera_message *mptr = NULL;
	
	if (event_queue->head) {
		mptr = event_queue->head;
		event_queue->head = mptr->next;
	}

	if(mptr) {
		memcpy(wmsg, mptr, sizeof(woomera_message));
		free(mptr);
		return 1;
	} else {
		memset(wmsg, 0, sizeof(woomera_message));
	}
	
	return 0;
}

static int woomera_message_parse(int fd, woomera_message *wmsg, int timeout, woomera_profile *profile, woomera_event_queue *event_queue) 
{
	char *cur, *cr, *next = NULL, *eor = NULL;
	char buf[2048];
	int res = 0, bytes = 0, sanity = 0;
	struct timeval started, ended;
	int elapsed, loops = 0;
	int failto = 0;

	memset(wmsg, 0, sizeof(woomera_message));

	if (fd <= 0 ) {
		return -1;
	}

	gettimeofday(&started, NULL);
	memset(buf, 0, sizeof(buf));

	if (timeout < 0) {
		timeout = abs(timeout);
		failto = 1;
	} else if(timeout == 0) {
		timeout = -1;
	}

	while (!(eor = strstr(buf, WOOMERA_RECORD_SEPERATOR))) {

		if (!profile->thread_running) {
			return -1;
		}
		/* Keep things moving.
		   Stupid Sockets -Homer Simpson */
		woomera_printf(NULL, fd, "%s", WOOMERA_RECORD_SEPERATOR);

		if((res = waitfor_socket(fd, (timeout > 0 ? timeout : 100)) > 0)) {
			res = recv(fd, buf, sizeof(buf), MSG_PEEK);
			if (res == 0) {
				sanity++;
			} else if (res < 0) {
				opbx_verbose(WOOMERA_DEBUG_PREFIX "{%s} error during packet retry #%d\n", profile->name, loops);
				return res;
			} else if (loops && globals.debug) {
				opbx_verbose(WOOMERA_DEBUG_PREFIX "{%s} Didnt get complete packet retry #%d\n", profile->name, loops);
				woomera_printf(NULL, fd, "%s", WOOMERA_RECORD_SEPERATOR);
				usleep(100);
			}

		}

		gettimeofday(&ended, NULL);
		elapsed = (((ended.tv_sec * 1000) + ended.tv_usec / 1000) - ((started.tv_sec * 1000) + started.tv_usec / 1000));

		if (res < 0) {
			return res;
		}

		if (sanity > 1000) {
			opbx_log(LOG_ERROR, "{%s} Failed Sanity Check! [errors]\n", profile->name);
			globals.panic = 1;
			return -1;
		}

		if (timeout > 0 && (elapsed > timeout)) {
			return failto ? -1 : 0;
		}
		
		loops++;
	}
	*eor = '\0';
	bytes = strlen(buf) + 4;
	
	memset(buf, 0, sizeof(buf));
	res = read(fd, buf, bytes);
	next = buf;

	if (globals.debug) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "Receive Message: {%s} [%s/%d]\n%s\n%s", profile->name, profile->woomera_host, profile->woomera_port, WOOMERA_DEBUG_LINE, buf);
	}

	while((cur = next)) {
		if ((cr = strstr(cur, WOOMERA_LINE_SEPERATOR))) {
			*cr = '\0';
			next = cr + (sizeof(WOOMERA_LINE_SEPERATOR) - 1);
			if (!strcmp(next, WOOMERA_RECORD_SEPERATOR)) {
				break;
			}
		} 

		if (opbx_strlen_zero(cur)) {
			break;
		}

		if (!wmsg->last) {
			opbx_set_flag(wmsg, WFLAG_EXISTS);
			if (!strncasecmp(cur, "EVENT", 5)) {
				cur += 6;
				opbx_set_flag(wmsg, WFLAG_EVENT);

				if (cur && (cr = strchr(cur, ' '))) {
					char *id;

					*cr = '\0';
					cr++;
					id = cr;
					if (cr && (cr = strchr(cr, ' '))) {
						*cr = '\0';
						cr++;
						strncpy(wmsg->command_args, cr, WOOMERA_STRLEN);
					}
					if(id) {
						opbx_copy_string(wmsg->callid, id, sizeof(wmsg->callid));
					}
				}
			} else {
				if (cur && (cur = strchr(cur, ' '))) {
					*cur = '\0';
					cur++;
					wmsg->mval = atoi(buf);
				} else {
					opbx_log(LOG_WARNING, "Malformed Message!\n");
					break;
				}
			}
			if (cur) {
				strncpy(wmsg->command, cur, WOOMERA_STRLEN);
			} else {
				opbx_log(LOG_WARNING, "Malformed Message!\n");
				break;
			}
		} else {
			char *name, *val;
			name = cur;
			if ((val = strchr(name, ':'))) {
				*val = '\0';
				val++;
				while (*val == ' ') {
					*val = '\0';
					val++;
				}
				strncpy(wmsg->values[wmsg->last-1], val, WOOMERA_STRLEN);
			}
			strncpy(wmsg->names[wmsg->last-1], name, WOOMERA_STRLEN);
			if (name && val && !strcasecmp(name, "content-type")) {
				opbx_set_flag(wmsg, WFLAG_CONTENT);
				bytes = atoi(val);
			}

		}
		wmsg->last++;
	}

	wmsg->last--;

	if (bytes && opbx_test_flag(wmsg, WFLAG_CONTENT)) {
		read(fd, wmsg->body, (bytes > sizeof(wmsg->body)) ? sizeof(wmsg->body) : bytes);
		if (globals.debug) {
			opbx_verbose("%s\n", wmsg->body);
		}
	}

	if (event_queue && opbx_test_flag(wmsg, WFLAG_EVENT)) {
		if (globals.debug) {
			opbx_verbose(WOOMERA_DEBUG_PREFIX "Queue Event: {%s} [%s]\n", profile->name, wmsg->command);
		}
		/* we don't want events we want a reply so we will stash them for later */
		woomera_enqueue_event(event_queue, wmsg);

		/* call ourself recursively to find the reply. we'll keep doing this as long we get events.
		 * wmsg will be overwritten but it's ok we just queued it.
		 */
		return woomera_message_parse(fd, wmsg, timeout, profile, event_queue);
		
	} else if (wmsg->mval > 99 && wmsg->mval < 200) {
		/* reply in the 100's are nice but we need to wait for another reply 
		   call ourself recursively to find the reply > 199 and forget this reply.
		*/
		return woomera_message_parse(fd, wmsg, timeout, profile, event_queue);
	} else {
		return opbx_test_flag(wmsg, WFLAG_EXISTS);
	}
}


static int tech_create_read_socket(private_object *tech_pvt)
{
	if ((tech_pvt->port = globals.next_woomera_port++) >= WOOMERA_MAX_PORT) {
		tech_pvt->port = globals.next_woomera_port = WOOMERA_MIN_PORT;
	}

	if ((tech_pvt->udp_socket = create_udp_socket(tech_pvt->profile->audio_ip, tech_pvt->port, &tech_pvt->udpread, 0))) {
		tech_pvt->owner->fds[0] = tech_pvt->udp_socket;
	}
	return tech_pvt->udp_socket;
}


static int tech_activate(private_object *tech_pvt) 
{
	woomera_message wmsg;

	if (tech_pvt) {
		if((connect_woomera(&tech_pvt->command_channel, tech_pvt->profile, 0))) {
			opbx_log(LOG_NOTICE, "connected to woomera!\n");
		} else {
			opbx_log(LOG_ERROR, "Can't connect to woomera!\n");
			return -1;
		}

		if (opbx_test_flag(tech_pvt, TFLAG_OUTBOUND)) {

			woomera_printf(tech_pvt->profile,
						   tech_pvt->command_channel, 
						   "CALL %s%sRaw-Audio: %s/%d%sLocal-Name: %s!%s%s", 
						   tech_pvt->dest, 
						   WOOMERA_LINE_SEPERATOR,
						   tech_pvt->profile->audio_ip,
						   tech_pvt->port,
						   WOOMERA_LINE_SEPERATOR,
						   tech_pvt->cid_name,
						   tech_pvt->cid_num,
						   WOOMERA_RECORD_SEPERATOR
						   );

			woomera_message_parse(tech_pvt->command_channel,
								  &wmsg,
								  WOOMERA_HARD_TIMEOUT,
								  tech_pvt->profile,
								  &tech_pvt->event_queue
								  );
		} else {
			opbx_set_flag(tech_pvt, TFLAG_PARSE_INCOMING);
			woomera_printf(tech_pvt->profile, tech_pvt->command_channel, "LISTEN%s", WOOMERA_RECORD_SEPERATOR);
			if (woomera_message_parse(tech_pvt->command_channel,
									  &wmsg,
									  WOOMERA_HARD_TIMEOUT,
									  tech_pvt->profile,
									  &tech_pvt->event_queue
									  ) < 0) {
				opbx_log(LOG_ERROR, "{%s} HELP! Woomera is broken!\n", tech_pvt->profile->name);
				opbx_set_flag(tech_pvt, TFLAG_ABORT);
				globals.panic = 1;
			}
		}
	} else {
		opbx_log(LOG_ERROR, "Where's my tech_pvt?\n");
	}

	return 0;
}

static void tech_init(private_object *tech_pvt, woomera_profile *profile, int flags) 
{

	gettimeofday(&tech_pvt->started, NULL);

	if (profile) {
		tech_pvt->profile = profile;
	}
	if (!tech_pvt->udp_socket) {
		tech_create_read_socket(tech_pvt);
	}

	opbx_set_flag(tech_pvt, flags);


	/* Openpbx being openpbx and all allows approx 1 nanosecond 
	 * to try and establish a connetion here before it starts crying.
	 * Now openpbx, being unsure of it's self will not enforce a lock while we work
	 * and after even a 1 second delay it will give up on the lock and mess everything up
	 * This stems from the fact that openpbx will scan it's list of channels constantly for 
	 * silly reasons like tab completion and cli output.
	 *
	 * Anyway, since we've already spent that nanosecond with the previous line of code
	 * tech_create_read_socket(tech_pvt); to setup a read socket
	 * which, by the way, openpbx insists we have before going any furthur.  
	 * So, in short, we are between a rock and a hard place and openpbx wants us to open a socket here
	 * but it too impaitent to wait for us to make sure it's ready so in the case of outgoing calls
	 * we will defer the rest of the socket establishment process to the monitor thread.  This is, of course, common 
	 * knowledge since openpbx abounds in documentation right?, sorry to bother you with all this!
	 */
	if (globals.more_threads) {
		opbx_set_flag(tech_pvt, TFLAG_ACTIVATE);
		/* we're gonna try "wasting" a thread to do a better realtime monitoring */
		launch_tech_thread(tech_pvt);
	} else {
		if (opbx_test_flag(tech_pvt, TFLAG_OUTBOUND)) {
			opbx_set_flag(tech_pvt, TFLAG_ACTIVATE);
		} else {
			tech_activate(tech_pvt);
		}
	}
}



static void tech_destroy(private_object *tech_pvt) 
{
	woomera_message wmsg;
	
	ASTOBJ_CONTAINER_UNLINK(&private_object_list, tech_pvt);
	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++DESTROY\n");
	}
	opbx_mutex_destroy(&tech_pvt->iolock);
	if (tech_pvt->command_channel) {
		woomera_printf(tech_pvt->profile, tech_pvt->command_channel, "hangup %s%s", tech_pvt->call_info.callid, WOOMERA_RECORD_SEPERATOR);
		if(woomera_message_parse(tech_pvt->command_channel,
								 &wmsg,
								 WOOMERA_HARD_TIMEOUT,
								 tech_pvt->profile,
								 &tech_pvt->event_queue
								 ) < 0) {
			opbx_log(LOG_ERROR, "{%s} HELP! Woomera is broken!\n", tech_pvt->profile->name);
			opbx_set_flag(tech_pvt, TFLAG_ABORT);
			globals.panic = 1;
		}
		woomera_printf(tech_pvt->profile, tech_pvt->command_channel, "bye%s", WOOMERA_RECORD_SEPERATOR);
		if(woomera_message_parse(tech_pvt->command_channel,
								 &wmsg,
								 WOOMERA_HARD_TIMEOUT,
								 tech_pvt->profile,
								 &tech_pvt->event_queue
								 ) < 0) {
			opbx_log(LOG_ERROR, "{%s} HELP! Woomera is broken!\n", tech_pvt->profile->name);
			opbx_set_flag(tech_pvt, TFLAG_ABORT);
			globals.panic = 1;
		}
		woomera_close_socket(&tech_pvt->command_channel);
	}
	if (tech_pvt->udp_socket) {
		woomera_close_socket(&tech_pvt->udp_socket);
	}
	if (tech_pvt->owner) {
		struct opbx_channel *chan;
		
		if ((chan = tech_pvt->owner)) {
			chan->tech_pvt = NULL;
			if (! opbx_test_flag(tech_pvt, TFLAG_PBX)) {
				opbx_hangup(chan);
			} else {
				opbx_softhangup(chan, OPBX_SOFTHANGUP_EXPLICIT);
			}
		}

	}
	
	free(tech_pvt);	
	opbx_mutex_lock(&usecnt_lock);
	usecnt--;
	if (usecnt < 0) {
		usecnt = 0;
	}
	opbx_mutex_unlock(&usecnt_lock);
}

static int waitfor_socket(int fd, int timeout) 
{
	struct pollfd pfds[1];
	int res;

	memset(&pfds[0], 0, sizeof(pfds[0]));
	pfds[0].fd = fd;
	pfds[0].events = POLLIN | POLLERR;
	res = poll(pfds, 1, timeout);

	if ((pfds[0].revents & POLLERR)) {
		res = -1;
	} else if((pfds[0].revents & POLLIN)) {
		res = 1;
	}

	return res;
}



static void *tech_monitor_thread(void *obj) 
{
	private_object *tech_pvt;
	woomera_message wmsg;
	int res = 0;

	tech_pvt = obj;

	for(;;) {
		if (globals.panic) {
			opbx_set_flag(tech_pvt, TFLAG_ABORT);
		}
		/* finish the deferred crap openpbx won't allow us to do live */

		if (opbx_test_flag(tech_pvt, TFLAG_ABORT)) {
			if (tech_pvt->command_channel) {
				woomera_close_socket(&tech_pvt->command_channel);
			}
			if (tech_pvt->udp_socket) {
				woomera_close_socket(&tech_pvt->udp_socket);
			}
			if (tech_pvt->owner && !opbx_check_hangup(tech_pvt->owner)) {
				opbx_softhangup(tech_pvt->owner, OPBX_SOFTHANGUP_EXPLICIT);
			}

			opbx_set_flag(tech_pvt, TFLAG_DESTROY);
		}

		if (opbx_test_flag(tech_pvt, TFLAG_DESTROY)) {
			tech_destroy(tech_pvt);
			tech_pvt = NULL;
			break;
		}

		if (opbx_test_flag(tech_pvt, TFLAG_ACTIVATE)) {
			opbx_clear_flag(tech_pvt, TFLAG_ACTIVATE);
			tech_activate(tech_pvt);
		}

		if (opbx_test_flag(tech_pvt, TFLAG_ANSWER)) {
			opbx_clear_flag(tech_pvt, TFLAG_ANSWER);
#ifndef USE_ANSWER
			woomera_printf(tech_pvt->profile, tech_pvt->command_channel, "ANSWER %s%s",tech_pvt->call_info.callid, WOOMERA_RECORD_SEPERATOR);
#endif
		}
		
		if (opbx_test_flag(tech_pvt, TFLAG_DTMF)) {
			opbx_mutex_lock(&tech_pvt->iolock);
			woomera_printf(tech_pvt->profile, tech_pvt->command_channel, "DTMF %s %s%s",tech_pvt->call_info.callid, tech_pvt->dtmfbuf, WOOMERA_RECORD_SEPERATOR);
			if(woomera_message_parse(tech_pvt->command_channel,
									 &wmsg,
									 WOOMERA_HARD_TIMEOUT,
									 tech_pvt->profile,
									 &tech_pvt->event_queue
									 ) < 0) {
				opbx_log(LOG_ERROR, "{%s} HELP! Woomera is broken!\n", tech_pvt->profile->name);
				opbx_set_flag(tech_pvt, TFLAG_ABORT);
				globals.panic = 1;
				continue;
			}
			opbx_clear_flag(tech_pvt, TFLAG_DTMF);
			memset(tech_pvt->dtmfbuf, 0, sizeof(tech_pvt->dtmfbuf));
			opbx_mutex_unlock(&tech_pvt->iolock);
		}

		if(tech_pvt->timeout) {
			struct timeval now;
			int elapsed;
			gettimeofday(&now, NULL);
			elapsed = (((now.tv_sec * 1000) + now.tv_usec / 1000) - ((tech_pvt->started.tv_sec * 1000) + tech_pvt->started.tv_usec / 1000));
			if (elapsed > tech_pvt->timeout) {
				/* call timed out! */
				opbx_set_flag(tech_pvt, TFLAG_ABORT);
			}
		}
		if (!tech_pvt->command_channel) {
			if (!globals.more_threads) {
				continue;
			} else {
				break;
			}
		}
		/* Check for events */
		if((res = woomera_dequeue_event(&tech_pvt->event_queue, &wmsg)) ||
		   (res = woomera_message_parse(tech_pvt->command_channel,
										&wmsg,
										100,
										tech_pvt->profile,
										NULL
										))) {

			if (res < 0 || !strcasecmp(wmsg.command, "HANGUP")) {
				if (res < 0) {
					opbx_log(LOG_ERROR, "{%s} HELP! I lost my connection to woomera!\n", tech_pvt->profile->name);
					opbx_set_flag(tech_pvt, TFLAG_ABORT);
					globals.panic = 1;
					continue;
				}
				if (! tech_pvt->owner) {
					break;
				}
				opbx_set_flag(tech_pvt, TFLAG_ABORT);
				continue;
			} else if (!strcasecmp(wmsg.command, "DTMF")) {
				struct opbx_frame dtmf_frame = {OPBX_FRAME_DTMF};
				int x = 0;
				for (x = 0; x < strlen(wmsg.command_args); x++) {
					dtmf_frame.subclass = wmsg.command_args[x];
					opbx_queue_frame(tech_pvt->owner, opbx_frdup(&dtmf_frame));
					if (globals.debug > 1) {
						opbx_verbose(WOOMERA_DEBUG_PREFIX "SEND DTMF [%c] to %s\n", dtmf_frame.subclass, tech_pvt->owner->name);
					}
				}
			} else if (!strcasecmp(wmsg.command, "PROCEED")) {
				/* This packet has lots of info so well keep it */
				tech_pvt->call_info = wmsg;
			} else if (opbx_test_flag(tech_pvt, TFLAG_PARSE_INCOMING) && !strcasecmp(wmsg.command, "INCOMING")) {
				char *exten;
				char *cid_name;
				char *cid_num;
				opbx_clear_flag(tech_pvt, TFLAG_PARSE_INCOMING);
				opbx_set_flag(tech_pvt, TFLAG_INCOMING);
				tech_pvt->call_info = wmsg;

				if (opbx_strlen_zero(tech_pvt->profile->context)) {
					opbx_log(LOG_WARNING, "No context configured for inbound calls aborting call!\n");
					opbx_set_flag(tech_pvt, TFLAG_ABORT);
					continue;
				}
				
				strncpy(tech_pvt->owner->context, tech_pvt->profile->context, sizeof(tech_pvt->owner->context) - 1);

				exten = woomera_message_header(&wmsg, "Local-Number");
				if (! exten || opbx_strlen_zero(exten)) {
					exten = "s";
				}

				strncpy(tech_pvt->owner->exten, exten, sizeof(tech_pvt->owner->exten) - 1);
				
				cid_name = opbx_strdupa(woomera_message_header(&wmsg, "Remote-Name"));

				if ((cid_num = strchr(cid_name, '!'))) {
					*cid_num = '\0';
					cid_num++;
				} else {
					cid_num = woomera_message_header(&wmsg, "Remote-Number");
				}
				opbx_set_callerid(tech_pvt->owner, cid_num, cid_name, cid_num);


				if (!opbx_exists_extension(tech_pvt->owner,
										  tech_pvt->owner->context,
										  tech_pvt->owner->exten,
										  1,
										  tech_pvt->owner->cid.cid_num)) {
					opbx_log(LOG_DEBUG, "Invalid exten %s@%s called!\n", tech_pvt->owner->exten, tech_pvt->owner->context);
					woomera_printf(tech_pvt->profile, tech_pvt->command_channel, "hangup %s%s", wmsg.callid, WOOMERA_RECORD_SEPERATOR);
					if(woomera_message_parse(tech_pvt->command_channel, 
											 &wmsg,
											 WOOMERA_HARD_TIMEOUT,
											 tech_pvt->profile,
											 &tech_pvt->event_queue
											 ) < 0) {
						opbx_log(LOG_ERROR, "{%s} HELP! Woomera is broken!\n", tech_pvt->profile->name);
						opbx_set_flag(tech_pvt, TFLAG_ABORT);
						globals.panic = 1;
					continue;
					}
					if (!(wmsg.mval >= 200 && wmsg.mval <= 299)) {
						opbx_set_flag(tech_pvt, TFLAG_ABORT);
					}
					continue;
				}


				woomera_printf(tech_pvt->profile, tech_pvt->command_channel, 
							   "%s %s%s"
							   "Raw-Audio: %s/%d%s",
							   MEDIA_ANSWER,
							   wmsg.callid,
							   WOOMERA_LINE_SEPERATOR,
							   tech_pvt->profile->audio_ip,
							   tech_pvt->port,
							   WOOMERA_RECORD_SEPERATOR);

				if(woomera_message_parse(tech_pvt->command_channel, 
										 &wmsg,
										 WOOMERA_HARD_TIMEOUT,
										 tech_pvt->profile,
										 &tech_pvt->event_queue
										 ) < 0) {
					opbx_log(LOG_ERROR, "{%s} HELP! Woomera is broken!\n", tech_pvt->profile->name);
					opbx_set_flag(tech_pvt, TFLAG_ABORT);
					globals.panic = 1;
					continue;
				} 
			} else if (!strcasecmp(wmsg.command, "CONNECT")) {
				struct opbx_frame answer_frame = {OPBX_FRAME_CONTROL, OPBX_CONTROL_ANSWER};
				opbx_setstate(tech_pvt->owner, OPBX_STATE_RING);
				opbx_queue_frame(tech_pvt->owner, &answer_frame);
			} else if (!strcasecmp(wmsg.command, "MEDIA")) {
				char *raw_audio_header;

				if ((raw_audio_header = woomera_message_header(&wmsg, "Raw-Audio"))) {
						char ip[25];
						char *ptr;
						int port = 0;
						struct hostent *hp;
						struct opbx_hostent ahp;   

						strncpy(ip, raw_audio_header, sizeof(ip) - 1);
						if ((ptr=strchr(ip, '/'))) {
							*ptr = '\0';
							ptr++;
							port = atoi(ptr);
						}
						
						if (!opbx_strlen_zero(ip) && (hp = opbx_gethostbyname(ip, &ahp))) {
							tech_pvt->udpwrite.sin_family = hp->h_addrtype;
							memcpy((char *) &tech_pvt->udpwrite.sin_addr.s_addr, hp->h_addr_list[0], hp->h_length);
							tech_pvt->udpwrite.sin_port = htons(port);
							opbx_set_flag(tech_pvt, TFLAG_MEDIA);
							tech_pvt->timeout = 0;
							opbx_setstate(tech_pvt->owner, OPBX_STATE_RINGING);
							if (opbx_test_flag(tech_pvt, TFLAG_INBOUND)) {
								if (opbx_pbx_start(tech_pvt->owner)) {
									opbx_log(LOG_WARNING, "Unable to start PBX on %s\n", tech_pvt->owner->name);
									opbx_hangup(tech_pvt->owner);
								} else {
									opbx_set_flag(tech_pvt, TFLAG_PBX);
								}
							}
						} else {
							if (globals.debug) {
								opbx_verbose(WOOMERA_DEBUG_PREFIX "{%s} Cannot resolve %s\n", tech_pvt->profile->name, ip);
							}
						}
				}
			}
		}
		if (globals.debug > 2) {
			opbx_verbose(WOOMERA_DEBUG_PREFIX "CHECK {%s} %s (%d)\n", tech_pvt->profile->name, tech_pvt->owner->name, res);
		}
		if (!globals.more_threads) {
			break;
		}
	}

	return NULL;
}

static int woomera_profile_thread_running(woomera_profile *profile, int set, int new) 
{
	int running = 0;

	opbx_mutex_lock(&profile->iolock);
	if (set) {
		profile->thread_running = new;
	}
	running = profile->thread_running;
	opbx_mutex_unlock(&profile->iolock);
	return running;
	
}

static int woomera_locate_socket(woomera_profile *profile, int *woomera_socket) 
{
	woomera_message wmsg;
	
	for (;;) {

		while (!connect_woomera(woomera_socket, profile, 0)) {
			if(!woomera_profile_thread_running(profile, 0, 0)) {
				break;
			}
			opbx_log(LOG_WARNING, "{%s} Cannot Reconnect to Woomera! retry in 5 seconds\n", profile->name);
			sleep(5);
		}

		if (*woomera_socket) {
			if (opbx_test_flag(profile, PFLAG_INBOUND)) {
				woomera_printf(profile, *woomera_socket, "LISTEN%s", WOOMERA_RECORD_SEPERATOR);
				if (woomera_message_parse(*woomera_socket,
										  &wmsg,
										  WOOMERA_HARD_TIMEOUT,
										  profile,
										  &profile->event_queue
										  ) < 0) {
					opbx_log(LOG_ERROR, "{%s} HELP! Woomera is broken!\n", profile->name);
					globals.panic = 1;
					if (*woomera_socket) {
						woomera_close_socket(woomera_socket);
					}
					continue;
				}
			}

		}
		usleep(100);
		break;
	}	
	return *woomera_socket;
}

static void tech_monitor_in_one_thread(void) 
{
	private_object *tech_pvt;

	ASTOBJ_CONTAINER_TRAVERSE(&private_object_list, 1, do {
		ASTOBJ_RDLOCK(iterator);
        tech_pvt = iterator;
		tech_monitor_thread(tech_pvt);
		ASTOBJ_UNLOCK(iterator);
    } while(0));
}

static void *woomera_thread_run(void *obj) 
{

	int woomera_socket = 0, res = 0;
	woomera_message wmsg;
	woomera_profile *profile;

	profile = obj;
	opbx_log(LOG_NOTICE, "Started Woomera Thread {%s}.\n", profile->name);
 
	profile->thread_running = 1;



	while(woomera_profile_thread_running(profile, 0, 0)) {
		/* listen on socket and handle events */
		if (globals.panic == 2) {
			opbx_log(LOG_NOTICE, "Woomera is disabled!\n");
			sleep(5);
			continue;
		}

		if (! woomera_socket) {
			if (woomera_locate_socket(profile, &woomera_socket)) {
				globals.panic = 0;
			}
			if (!woomera_profile_thread_running(profile, 0, 0)) {
				break;
			}
			opbx_log(LOG_NOTICE, "Woomera Thread Up {%s} %s/%d\n", profile->name, profile->woomera_host, profile->woomera_port);

		}

		if (globals.panic) {
			if (globals.panic != 2) {
				opbx_log(LOG_ERROR, "Help I'm in a state of panic!\n");
			}
			if (woomera_socket) {
				woomera_close_socket(&woomera_socket);
			}
			continue;
		}
		if (!globals.more_threads) {
			if (woomera_socket) {
				tech_monitor_in_one_thread();
			}
		}

		if ((res = woomera_dequeue_event(&profile->event_queue, &wmsg) ||
			 (res = woomera_message_parse(woomera_socket,
										  &wmsg,
										  /* if we are not stingy with threads we can block forever */
										  globals.more_threads ? 0 : 100,
										  profile,
										  NULL
										  )))) {
			if (res < 0) {
				opbx_log(LOG_ERROR, "{%s} HELP! I lost my connection to woomera!\n", profile->name);
				if (woomera_socket) {
					woomera_close_socket(&woomera_socket);
				}
				global_set_flag(TFLAG_ABORT);
				globals.panic = 1;
				continue;

				if (woomera_socket) {
					if (opbx_test_flag(profile, PFLAG_INBOUND)) {
						woomera_printf(profile, woomera_socket, "LISTEN%s", WOOMERA_RECORD_SEPERATOR);
						if(woomera_message_parse(woomera_socket,
												 &wmsg,
												 WOOMERA_HARD_TIMEOUT,
												 profile,
												 &profile->event_queue
												 ) < 0) {
							opbx_log(LOG_ERROR, "{%s} HELP! Woomera is broken!\n", profile->name);
							globals.panic = 1;
							woomera_close_socket(&woomera_socket);
						} 
					}
					if (woomera_socket) {
						opbx_log(LOG_NOTICE, "Woomera Thread Up {%s} %s/%d\n", profile->name, profile->woomera_host, profile->woomera_port);
					}
				}
				continue;
			}

			if (!strcasecmp(wmsg.command, "INCOMING")) {
				int cause = 0;
				struct opbx_channel *inchan;
				char *name;

				if (!(name = woomera_message_header(&wmsg, "Remote-Address"))) {
					name = woomera_message_header(&wmsg, "Channel-Name");
				}

				if ((inchan = woomera_new(type, OPBX_FORMAT_SLINEAR, name, &cause))) {
					private_object *tech_pvt;
					tech_pvt = inchan->tech_pvt;
					tech_init(tech_pvt, profile, TFLAG_INBOUND);
				} else {
					opbx_log(LOG_ERROR, "Cannot Create new Inbound Channel!\n");
				}
			}
		}
		if(globals.debug > 2) {
			opbx_verbose(WOOMERA_DEBUG_PREFIX "Main Thread {%s} Select Return %d\n", profile->name, res);
		}
		usleep(100);
	}
	

	if (woomera_socket) {
		woomera_printf(profile, woomera_socket, "BYE%s", WOOMERA_RECORD_SEPERATOR);
		if(woomera_message_parse(woomera_socket,
								 &wmsg,
								 WOOMERA_HARD_TIMEOUT,
								 profile,
								 &profile->event_queue
								 ) < 0) {
			opbx_log(LOG_ERROR, "{%s} HELP! Woomera is broken!\n", profile->name);
			globals.panic = 1;
		}
		woomera_close_socket(&woomera_socket);
	}

	opbx_log(LOG_NOTICE, "Ended Woomera Thread {%s}.\n", profile->name);
	woomera_profile_thread_running(profile, 1, -1);
	return NULL;
}

static void launch_woomera_thread(woomera_profile *profile) 
{
	pthread_attr_t attr;
	int result = 0;

	result = pthread_attr_init(&attr);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	result = opbx_pthread_create(&profile->thread, &attr, woomera_thread_run, profile);
	result = pthread_attr_destroy(&attr);
}


static void launch_tech_thread(private_object *tech_pvt) 
{
	pthread_attr_t attr;
	int result = 0;

	result = pthread_attr_init(&attr);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	result = opbx_pthread_create(&tech_pvt->thread, &attr, tech_monitor_thread, tech_pvt);
	result = pthread_attr_destroy(&attr);
}


static void destroy_woomera_profile(woomera_profile *profile) 
{
	if (profile && opbx_test_flag(profile, PFLAG_DYNAMIC)) {
		opbx_mutex_destroy(&profile->iolock);
		free(profile);
	}
}

static woomera_profile *clone_woomera_profile(woomera_profile *new_profile, woomera_profile *default_profile) 
{
	return memcpy(new_profile, default_profile, sizeof(woomera_profile));
}

static woomera_profile *create_woomera_profile(woomera_profile *default_profile) 
{
	woomera_profile *profile;

	if((profile = malloc(sizeof(woomera_profile)))) {
		clone_woomera_profile(profile, default_profile);
		opbx_mutex_init(&profile->iolock);
		opbx_set_flag(profile, PFLAG_DYNAMIC);
	}
	return profile;
}

static int config_woomera(void) 
{
	struct opbx_config *cfg;
	char *entry;
	struct opbx_variable *v;
	woomera_profile *profile;
	int count = 0;

	memset(&default_profile, 0, sizeof(default_profile));
	
	if ((cfg = opbx_config_load(configfile))) {
		for (entry = opbx_category_browse(cfg, NULL); entry != NULL; entry = opbx_category_browse(cfg, entry)) {
			if (!strcmp(entry, "settings")) {
				for (v = opbx_variable_browse(cfg, entry); v ; v = v->next) {
					if (!strcmp(v->name, "debug")) {
						globals.debug = atoi(v->value);
					} else if (!strcmp(v->name, "more_threads")) {
						globals.more_threads = opbx_true(v->value);
					}
					
				}
			} else {
				int new = 0;
				count++;
				if (!strcmp(entry, "default")) {
					profile = &default_profile;
				} else {
					if((profile = ASTOBJ_CONTAINER_FIND(&woomera_profile_list, entry))) {
						clone_woomera_profile(profile, &default_profile);
					} else {
						if((profile = create_woomera_profile(&default_profile))) {
							new = 1;
						} else {
							opbx_log(LOG_ERROR, "Memory Error!\n");
						}
					}
				}
				strncpy(profile->name, entry, sizeof(profile->name) - 1);
				/*default is inbound and outbound enabled */
				opbx_set_flag(profile, PFLAG_INBOUND | PFLAG_OUTBOUND);
				for (v = opbx_variable_browse(cfg, entry); v ; v = v->next) {
					if (!strcmp(v->name, "audio_ip")) {
						strncpy(profile->audio_ip, v->value, sizeof(profile->audio_ip) - 1);
					} else if (!strcmp(v->name, "host")) {
						strncpy(profile->woomera_host, v->value, sizeof(profile->woomera_host) - 1);
					} else if (!strcmp(v->name, "port")) {
						profile->woomera_port = atoi(v->value);
					} else if (!strcmp(v->name, "disabled")) {
						opbx_set2_flag(profile, opbx_true(v->value), PFLAG_DISABLED);
					} else if (!strcmp(v->name, "inbound")) {
						if (opbx_false(v->value)) {
							opbx_clear_flag(profile, PFLAG_INBOUND);
						}
					} else if (!strcmp(v->name, "outbound")) {
						if (opbx_false(v->value)) {
							opbx_clear_flag(profile, PFLAG_OUTBOUND);
						}
					} else if (!strcmp(v->name, "context")) {
						strncpy(profile->context, v->value, sizeof(profile->context) - 1);
					}
				}

				ASTOBJ_CONTAINER_LINK(&woomera_profile_list, profile);
			}
		}
		opbx_config_destroy(cfg);
	} else {
		return 0;
	}

	return count;

}

static int create_udp_socket(char *ip, int port, struct sockaddr_in *sockaddr, int client)
{
	int rc, sd = 0;
	struct hostent *hp;
    struct opbx_hostent ahp;
	struct sockaddr_in servAddr, *addr, cliAddr;
	
	if(sockaddr) {
		addr = sockaddr;
	} else {
		addr = &servAddr;
	}
	
	if ((sd = socket(AF_INET, SOCK_DGRAM, 0))) {
		if ((hp = opbx_gethostbyname(ip, &ahp))) {
			addr->sin_family = hp->h_addrtype;
			memcpy((char *) &addr->sin_addr.s_addr, hp->h_addr_list[0], hp->h_length);
			addr->sin_port = htons(port);
			if (client) {
				cliAddr.sin_family = AF_INET;
				cliAddr.sin_addr.s_addr = htonl(INADDR_ANY);
				cliAddr.sin_port = htons(0);
  				rc = bind(sd, (struct sockaddr *) &cliAddr, sizeof(cliAddr));
			} else {
				rc = bind(sd, (struct sockaddr *) addr, sizeof(cliAddr));
			}
			if(rc < 0) {
				opbx_log(LOG_ERROR,"Error opening udp socket\n");
				woomera_close_socket(&sd);
			} else if(globals.debug) {
				opbx_verbose(WOOMERA_DEBUG_PREFIX "Socket Binded %s to %s/%d\n", client ? "client" : "server", ip, port);
			}
		}
	}

	return sd;
}


static int connect_woomera(int *new_socket, woomera_profile *profile, int flags) 
{
	struct sockaddr_in localAddr, remoteAddr;
	struct hostent *hp;
	struct opbx_hostent ahp;
	int res = 0;

	if ((hp = opbx_gethostbyname(profile->woomera_host, &ahp))) {
		remoteAddr.sin_family = hp->h_addrtype;
		memcpy((char *) &remoteAddr.sin_addr.s_addr, hp->h_addr_list[0], hp->h_length);
		remoteAddr.sin_port = htons(profile->woomera_port);
		do {
			/* create socket */
			*new_socket = socket(AF_INET, SOCK_STREAM, 0);
			if (*new_socket < 0) {
				opbx_log(LOG_ERROR, "cannot open socket to %s/%d\n", profile->woomera_host, profile->woomera_port);
				res = 0;
				break;
			}
			
			/* bind any port number */
			localAddr.sin_family = AF_INET;
			localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
			localAddr.sin_port = htons(0);
  
			res = bind(*new_socket, (struct sockaddr *) &localAddr, sizeof(localAddr));
			if (res < 0) {
				opbx_log(LOG_ERROR, "cannot bind to %s/%d\n", profile->woomera_host, profile->woomera_port);
				woomera_close_socket(new_socket);
				break;
			}
		
			/* connect to server */
			res = connect(*new_socket, (struct sockaddr *) &remoteAddr, sizeof(remoteAddr));
			if (res < 0) {
				opbx_log(LOG_ERROR, "cannot connect to {%s} %s/%d\n", profile->name, profile->woomera_host, profile->woomera_port);
				res = 0;
				woomera_close_socket(new_socket);
				break;
			}
			res = 1;
		} while(0);
		
	} else {
		res = 0;
	}
	if (res) {
		int flag = 1;
		int res;
		woomera_message wmsg;

		/* disable nagle's algorythm */
		res = setsockopt(*new_socket, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));

		
		if (!(flags & WCFLAG_NOWAIT)) {
			/* kickstart the session waiting for a HELLO */
			woomera_printf(NULL, *new_socket, "%s", WOOMERA_RECORD_SEPERATOR);
			
			if ((res = woomera_message_parse(*new_socket,
											 &wmsg,
											 WOOMERA_HARD_TIMEOUT,
											 profile,
											 NULL
											 )) < 0) {
				opbx_log(LOG_ERROR, "{%s} Timed out waiting for a hello from woomera!\n", profile->name);
				woomera_close_socket(new_socket);
			}
			if (res > 0 && strcasecmp(wmsg.command, "HELLO")) {
				opbx_log(LOG_ERROR, "{%s} unexpected reply [%s] while waiting for a hello from woomera!\n", profile->name, wmsg.command);
				woomera_close_socket(new_socket);
			}
		}

	} else {
		woomera_close_socket(new_socket);
	}

	return *new_socket;
}

static int init_woomera(void) 
{
	opbx_mutex_lock(&lock);
	woomera_profile *profile;

	if (!config_woomera()) {
		return 0;
	}
	
	ASTOBJ_CONTAINER_TRAVERSE(&woomera_profile_list, 1, do {
		ASTOBJ_RDLOCK(iterator);
		profile = iterator;
		if (!opbx_test_flag(profile, PFLAG_DISABLED)) {
			launch_woomera_thread(profile);
		}
		ASTOBJ_UNLOCK(iterator);
	} while(0));

	opbx_mutex_unlock(&lock);
	return 1;
}

static struct opbx_channel *woomera_new(const char *type, int format, void *data, int *cause)
{
	private_object *tech_pvt;
	struct opbx_channel *chan = NULL;
	
	if ((chan = opbx_channel_alloc(1))) {
		chan->nativeformats = WFORMAT;
		chan->type = type;
		snprintf(chan->name, sizeof(chan->name), "%s/%s-%04x", chan->type, (char *)data, opbx_random() & 0xffff);
		chan->writeformat = chan->rawwriteformat = chan->readformat = WFORMAT;
		chan->_state = OPBX_STATE_DOWN;
		chan->_softhangup = 0;
		tech_pvt = malloc(sizeof(private_object));
		memset(tech_pvt, 0, sizeof(private_object));
		opbx_mutex_init(&tech_pvt->iolock);
		chan->tech_pvt = tech_pvt;
		chan->tech = &technology;
		opbx_clear_flag(chan, OPBX_FLAGS_ALL);
		memset(&tech_pvt->frame, 0, sizeof(tech_pvt->frame));
		tech_pvt->frame.frametype = OPBX_FRAME_VOICE;
		tech_pvt->frame.subclass = WFORMAT;
		tech_pvt->frame.offset = OPBX_FRIENDLY_OFFSET;
		tech_pvt->owner = chan;

		ASTOBJ_CONTAINER_LINK(&private_object_list, tech_pvt);

	} else {
		opbx_log(LOG_ERROR, "Can't allocate a channel\n");
	}
	opbx_mutex_lock(&usecnt_lock);
	usecnt++;
	opbx_mutex_unlock(&usecnt_lock);

	return chan;
}




/********************CHANNEL METHOD LIBRARY********************
 * This is the actual functions described by the prototypes above.
 *
 */


/*--- tech_requester: parse 'data' a url-like destination string, allocate a channel and a private structure
 * and return the newly-setup channel.
 */
static struct opbx_channel *tech_requester(const char *type, int format, void *data, int *cause)
{
	struct opbx_channel *chan = NULL;

	if (globals.panic) {
		return NULL;
	}
	if ((chan = woomera_new(type, format, data, cause))) {
		private_object *tech_pvt;
		
		tech_pvt = chan->tech_pvt;
		opbx_set_flag(tech_pvt, TFLAG_PBX); /* so we know we dont have to free the channel ourselves */
	} else {
		opbx_log(LOG_ERROR, "Can't allocate a channel\n");
	}
	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++REQ %s\n", chan->name);
	}



	return chan;
}

/*--- tech_senddigit: Send a DTMF character */
static int tech_send_digit(struct opbx_channel *self, char digit)
{
	private_object *tech_pvt = self->tech_pvt;
	int res = 0;

	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++DIGIT %s '%c'\n",self->name, digit);
	}

	/* we don't have time to make sure the dtmf command is successful cos openpbx again 
	   is much too impaitent... so we will cache the digits so the monitor thread can send
	   it for us when it has time to actually wait.
	*/
	opbx_mutex_lock(&tech_pvt->iolock);
	snprintf(tech_pvt->dtmfbuf + strlen(tech_pvt->dtmfbuf), sizeof(tech_pvt->dtmfbuf), "%c", digit);
	opbx_set_flag(tech_pvt, TFLAG_DTMF);
	opbx_mutex_unlock(&tech_pvt->iolock);

	return res;
}

/*--- tech_call: Initiate a call on my channel 
 * 'dest' has been passed telling you where to call
 * but you may already have that information from the requester method
 * not sure why it sends it twice, maybe it changed since then *shrug*
 * You also have timeout (in ms) so you can tell how long the caller
 * is willing to wait for the call to be complete.
 */

static int tech_call(struct opbx_channel *self, char *dest, int timeout)
{
	private_object *tech_pvt = self->tech_pvt;
	char *workspace;

	if (globals.panic) {
		return -1;
	}
	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++CALL %s (%s <%s>)\n",self->name, self->cid.cid_name, self->cid.cid_num);
	}
	if (self->cid.cid_name) {
		strncpy(tech_pvt->cid_name, self->cid.cid_name, sizeof(tech_pvt->cid_name)-1);
	}
	if (self->cid.cid_num) {
		strncpy(tech_pvt->cid_num, self->cid.cid_num, sizeof(tech_pvt->cid_num)-1);
	}
	if ((workspace = opbx_strdupa(dest))) {
		char *addr, *profile_name, *proto;
		woomera_profile *profile;
		int isprofile = 0;

		if ((addr = strchr(workspace, ':'))) {
			proto = workspace;
			*addr = '\0';
			addr++;
		} else {
			proto = "H323";
			addr = workspace;
		}
		
		if ((profile_name = strchr(addr, '*'))) {
			*profile_name = '\0';
			profile_name++;
			isprofile = 1;
		} else {
			profile_name = "default";
		}
		if (! (profile = ASTOBJ_CONTAINER_FIND(&woomera_profile_list, profile_name))) {
			profile = ASTOBJ_CONTAINER_FIND(&woomera_profile_list, "default");
		}
		
		if (!profile) {
			opbx_log(LOG_ERROR, "Unable to find profile! Call Aborted!\n");
			return -1;
		}

		if (!opbx_test_flag(profile, PFLAG_OUTBOUND)) {
			opbx_log(LOG_ERROR, "This profile is not allowed to make outbound calls! Call Aborted!\n");
			return -1;
		}

		snprintf(tech_pvt->dest, sizeof(tech_pvt->dest), "%s", addr ? addr : "");

		tech_pvt->timeout = timeout;
		tech_init(tech_pvt, profile, TFLAG_OUTBOUND);
	}
	
	return 0;
}


/*--- tech_hangup: end a call on my channel 
 * Now is your chance to tear down and free the private object
 * from the channel it's about to be freed so you must do so now
 * or the object is lost.  Well I guess you could tag it for reuse
 * or for destruction and let a monitor thread deal with it too.
 * during the load_module routine you have every right to start up
 * your own fancy schmancy bunch of threads or whatever else 
 * you want to do.
 */
static int tech_hangup(struct opbx_channel *self)
{
	private_object *tech_pvt = self->tech_pvt;
	int res = 0;

	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++HANGUP %s\n",self->name);
	}
	
	if (tech_pvt) {
		tech_pvt->owner = NULL;
		opbx_set_flag(tech_pvt, TFLAG_DESTROY);
	}
	self->tech_pvt = NULL;
	
	return res;
}

/*--- tech_answer: answer a call on my channel
 * if being 'answered' means anything special to your channel
 * now is your chance to do it!
 */
static int tech_answer(struct opbx_channel *self)
{
	private_object *tech_pvt;
	int res = 0;

	tech_pvt = self->tech_pvt;
	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++ANSWER %s\n",self->name);
	}

	opbx_set_flag(tech_pvt, TFLAG_ANSWER);
	opbx_setstate(self, OPBX_STATE_UP);
	return res;
}

/*--- tech_read: Read an audio frame from my channel.
 * You need to read data from your channel and convert/transfer the
 * data into a newly allocated struct opbx_frame object
 */
static struct opbx_frame  *tech_read(struct opbx_channel *self)
{
	private_object *tech_pvt = self->tech_pvt;
	int res = 0;

	if (globals.panic) {
		return NULL;
	}

	res = waitfor_socket(tech_pvt->udp_socket, 100);

	if (res < 1) {
		return NULL;
	}

	res = read(tech_pvt->udp_socket, tech_pvt->fdata + OPBX_FRIENDLY_OFFSET, FRAME_LEN);

	if (res < 1) {
		return NULL;
	}

	tech_pvt->frame.datalen = res;
	tech_pvt->frame.samples = res / 2;
	tech_pvt->frame.data = tech_pvt->fdata + OPBX_FRIENDLY_OFFSET;

	if (globals.debug > 2) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++READ %s %d\n",self->name, res);
	}



	return &tech_pvt->frame;
}

/*--- tech_write: Write an audio frame to my channel
 * Yep, this is the opposite of tech_read, you need to examine
 * a frame and transfer the data to your technology's audio stream.
 * You do not have any responsibility to destroy this frame and you should
 * consider it to be read-only.
 */
static int tech_write(struct opbx_channel *self, struct opbx_frame *frame)
{
	private_object *tech_pvt = self->tech_pvt;
	int res = 0, i = 0;

	if (globals.panic) {
		return -1;
	}
	if(opbx_test_flag(tech_pvt, TFLAG_MEDIA) && frame->datalen) {
		if (frame->frametype == OPBX_FRAME_VOICE) {
			i = sendto(tech_pvt->udp_socket, frame->data, frame->datalen, 0, (struct sockaddr *) &tech_pvt->udpwrite, sizeof(tech_pvt->udpwrite));
			if (i < 0) {
				return -1;
			}
			if (globals.debug > 2) {
				opbx_verbose(WOOMERA_DEBUG_PREFIX "+++WRITE %s %d\n",self->name, i);
			}
		} else {
			opbx_log(LOG_WARNING, "Invalid frame type %d sent\n", frame->frametype);
		}
	}
	
	return res;
}

/*--- tech_write_video: Write a video frame to my channel ---*/
static int tech_write_video(struct opbx_channel *self, struct opbx_frame *frame)
{
	private_object *tech_pvt;
	int res = 0;

	tech_pvt = self->tech_pvt;
	return res;
}

/*--- tech_exception: Read an exception audio frame from my channel ---*/
static struct opbx_frame *tech_exception(struct opbx_channel *self)
{
	private_object *tech_pvt;
	struct opbx_frame *new_frame = NULL;

	tech_pvt = self->tech_pvt;	
	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++EXCEPT %s\n",self->name);
	}
	return new_frame;
}

/*--- tech_indicate: Indicaate a condition to my channel ---*/
static int tech_indicate(struct opbx_channel *self, int condition)
{
	private_object *tech_pvt;
	int res = 0;

	tech_pvt = self->tech_pvt;
	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++INDICATE %s %d\n",self->name, condition);
	}
	return res;
}

/*--- tech_fixup: add any finishing touches to my channel if it is masqueraded---*/
static int tech_fixup(struct opbx_channel *oldchan, struct opbx_channel *newchan)
{
	int res = 0;
	private_object *tech_pvt;

	if ((tech_pvt = oldchan->tech_pvt)) {
		opbx_mutex_lock(&tech_pvt->iolock);
		tech_pvt->owner = newchan;
		opbx_mutex_unlock(&tech_pvt->iolock);
	}

	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++FIXUP %s\n",oldchan->name);
	}
	return res;
}

/*--- tech_send_html: Send html data on my channel ---*/
static int tech_send_html(struct opbx_channel *self, int subclass, const char *data, int datalen)
{
	private_object *tech_pvt;
	int res = 0;

	tech_pvt = self->tech_pvt;


	return res;
}

/*--- tech_send_text: Send plain text data on my channel ---*/
static int tech_send_text(struct opbx_channel *self, const char *text)
{
	int res = 0;

	return res;
}

/*--- tech_send_image: Send image data on my channel ---*/
static int tech_send_image(struct opbx_channel *self, struct opbx_frame *frame) 
{
	int res = 0;

	return res;
}


/*--- tech_setoption: set options on my channel ---*/
static int tech_setoption(struct opbx_channel *self, int option, void *data, int datalen)
{
	int res = 0;

	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++SETOPT %s\n",self->name);
	}
	return res;

}

/*--- tech_queryoption: get options from my channel ---*/
static int tech_queryoption(struct opbx_channel *self, int option, void *data, int *datalen)
{
	int res = 0;

	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++GETOPT %s\n",self->name);
	}
	return res;
}

/*--- tech_bridged_channel: return a pointer to a channel that may be bridged to our channel. ---*/
static struct opbx_channel *tech_bridged_channel(struct opbx_channel *self, struct opbx_channel *bridge)
{
	struct opbx_channel *chan = NULL;

	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++BRIDGED %s\n",self->name);
	}
	return chan;
}


/*--- tech_transfer: Technology-specific code executed to peform a transfer. ---*/
static int tech_transfer(struct opbx_channel *self, const char *newdest)
{
	int res = -1;

	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++TRANSFER %s\n",self->name);
	}
	return res;
}

/*--- tech_bridge:  Technology-specific code executed to natively bridge 2 of our channels ---*/
static enum opbx_bridge_result tech_bridge(struct opbx_channel *chan_a, struct opbx_channel *chan_b, int flags, struct opbx_frame **outframe, struct opbx_channel **recent_chan, int timeoutms)
{
	int res = -1;

	if (globals.debug > 1) {
		opbx_verbose(WOOMERA_DEBUG_PREFIX "+++BRIDGE %s\n",chan_a->name);
	}
	return res;
}


static int woomera_cli(int fd, int argc, char *argv[]) 
{
	if (argc > 1) {
		if (!strcmp(argv[1], "debug")) {
			if (argc > 2) {
				globals.debug = atoi(argv[2]);
			}
			opbx_cli(fd, "OK debug=%d\n", globals.debug);
		} else if (!strcmp(argv[1], "panic")) {
			if (argc > 2) {
				globals.panic = atoi(argv[2]);
			}
			opbx_cli(fd, "OK panic=%d\n", globals.panic);
		} else if (!strcmp(argv[1], "threads")) {
			opbx_cli(fd, "chan_woomera is using %s threads!\n", globals.more_threads ? "more" : "less");
		
		} else if (!strcmp(argv[1], "abort")) {
			global_set_flag(TFLAG_ABORT);
		}

	} else {
		opbx_cli(fd, "Usage: woomera <debug> <level>\n");
	}
	return 0;
}

static struct opbx_cli_entry  cli_woomera = { { "woomera", NULL }, woomera_cli, "Woomera", "Woomera" };

/******************************* CORE INTERFACE ********************************************
 * These are module-specific interface functions that are common to every module
 * To be used to initilize/de-initilize, reload and track the use count of a loadable module. 
 */




int load_module()
{
	if (opbx_channel_register(&technology)) {
		opbx_log(LOG_ERROR, "Unable to register channel class %s\n", type);
		return -1;
	}
	memset(&globals, 0, sizeof(globals));
	globals.next_woomera_port = WOOMERA_MIN_PORT;
	/* Use more threads for better timing this adds a dedicated monitor thread to
	   every channel you can disable it with more_threads => no in [settings] */
	globals.more_threads = 1;

	opbx_mutex_init(&default_profile.iolock);
	if (!init_woomera()) {
		return -1;
	}

	ASTOBJ_CONTAINER_INIT(&private_object_list);
	/*
	sched = sched_context_create();
    if (!sched) {
        opbx_log(LOG_WARNING, "Unable to create schedule context\n");
    }
	*/
	
	opbx_cli_register(&cli_woomera);
	return 0;
}

int reload()
{
	return 0;
}

int unload_module()
{
	time_t then, now;
	woomera_profile *profile = NULL;

	ASTOBJ_CONTAINER_TRAVERSE(&woomera_profile_list, 1, do {
		ASTOBJ_RDLOCK(iterator);
		profile = iterator;

		time(&then);
		if (!opbx_test_flag(profile, PFLAG_DISABLED)) {
			opbx_log(LOG_NOTICE, "Shutting Down Thread. {%s}\n", profile->name);
			woomera_profile_thread_running(profile, 1, 0);
			
			while (!woomera_profile_thread_running(profile, 0, 0)) {
				time(&now);
				if (now - then > 30) {
					opbx_log(LOG_WARNING, "Timed out waiting for thread to exit\n");
					break;
				}
				usleep(100);
			}
		}
		ASTOBJ_UNLOCK(iterator);
	} while(0));

	opbx_mutex_destroy(&default_profile.iolock);
	opbx_cli_unregister(&cli_woomera);
	ASTOBJ_CONTAINER_DESTROY(&private_object_list);
	ASTOBJ_CONTAINER_DESTROYALL(&woomera_profile_list, destroy_woomera_profile);
	//sched_context_destroy(sched);

	opbx_channel_unregister(&technology);
	return 0;
}

int usecount()
{
	int res;
	opbx_mutex_lock(&usecnt_lock);
	res = usecnt;
	opbx_mutex_unlock(&usecnt_lock);
	return res;
}

/* returns a descriptive string to the system console */
char *description()
{
	return (char *) desc;
}

