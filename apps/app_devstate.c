/*
 * Devstate application
 * 
 * Since we like the snom leds so much, a little app to
 * light the lights on the snom on demand ....
 *
 * Copyright (C) 2005, Druid Software
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <openpbx/lock.h>
#include <openpbx/file.h>
#include <openpbx/logger.h>
#include <openpbx/channel.h>
#include <openpbx/pbx.h>
#include <openpbx/module.h>
#include <openpbx/opbxdb.h>
#include <openpbx/utils.h>
#include <openpbx/cli.h>
#include <openpbx/manager.h>
#include <openpbx/devicestate.h>


static char type[] = "DS";
static char tdesc[] = "Application for sending device state messages";

static char app[] = "DevState";

static char synopsis[] = "Generate a device state change event given the input parameters";

static char descrip[] = " DevState(device|state):  Generate a device state change event given the input parameters. Returns 0. State values match the openpbx device states. They are 0 = unknown, 1 = not inuse, 2 = inuse, 3 = busy, 4 = invalid, 5 = unavailable, 6 = ringing\n";

static char devstate_cli_usage[] = 
"Usage: DevState device state\n" 
"       Generate a device state change event given the input parameters.\n Mainly used for lighting the LEDs on the snoms.\n";

static int devstate_cli(int fd, int argc, char *argv[]);
static struct opbx_cli_entry  cli_dev_state =
        { { "devstate", NULL }, devstate_cli, "Set the device state on one of the \"pseudo devices\".", devstate_cli_usage };

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;


static int devstate_cli(int fd, int argc, char *argv[])
{
    if ((argc != 3) && (argc != 4) && (argc != 5))
        return RESULT_SHOWUSAGE;

    if (opbx_db_put("DEVSTATES", argv[1], argv[2]))
    {
        opbx_log(LOG_DEBUG, "opbx_db_put failed\n");
    }
	opbx_device_state_changed("DS/%s", argv[1]);
    
    return RESULT_SUCCESS;
}

static int devstate_exec(struct opbx_channel *chan, void *data)
{
    struct localuser *u;
    char *device, *state, *info;
    if (!(info = opbx_strdupa(data))) {
            opbx_log(LOG_WARNING, "Unable to dupe data :(\n");
            return -1;
    }
    LOCAL_USER_ADD(u);
    
    device = info;
    state = strchr(info, '|');
    if (state) {
        *state = '\0';
        state++;
    }
    else
    {
        opbx_log(LOG_DEBUG, "No state argument supplied\n");
        return -1;
    }

    if (opbx_db_put("DEVSTATES", device, state))
    {
        opbx_log(LOG_DEBUG, "opbx_db_put failed\n");
    }

    opbx_device_state_changed("DS/%s", device);

    LOCAL_USER_REMOVE(u);
    return 0;
}


static int ds_devicestate(void *data)
{
    char *dest = data;
    char stateStr[16];
    if (opbx_db_get("DEVSTATES", dest, stateStr, sizeof(stateStr)))
    {
        opbx_log(LOG_DEBUG, "ds_devicestate couldnt get state in opbxdb\n");
        return 0;
    }
    else
    {
        opbx_log(LOG_DEBUG, "ds_devicestate dev=%s returning state %d\n",
               dest, atoi(stateStr));
        return (atoi(stateStr));
    }
}

static struct opbx_channel_tech devstate_tech = {
	.type = type,
	.description = tdesc,
	.capabilities = ((OPBX_FORMAT_MAX_AUDIO << 1) - 1),
	.devicestate = ds_devicestate,
	.requester = NULL,
	.send_digit = NULL,
	.send_text = NULL,
	.call = NULL,
	.hangup = NULL,
	.answer = NULL,
	.read = NULL,
	.write = NULL,
	.bridge = NULL,
	.exception = NULL,
	.indicate = NULL,
	.fixup = NULL,
	.setoption = NULL,
};

static char mandescr_devstate[] = 
"Description: Put a value into opbxdb\n"
"Variables: \n"
"	Family: ...\n"
"	Key: ...\n"
"	Value: ...\n";

static int action_devstate(struct mansession *s, struct message *m)
{
        char *devstate = astman_get_header(m, "Devstate");
        char *value = astman_get_header(m, "Value");
	char *id = astman_get_header(m,"ActionID");

	if (!strlen(devstate)) {
		astman_send_error(s, m, "No Devstate specified");
		return 0;
	}
	if (!strlen(value)) {
		astman_send_error(s, m, "No Value specified");
		return 0;
	}

        if (!opbx_db_put("DEVSTATES", devstate, value)) {
	    opbx_device_state_changed("DS/%s", devstate);
	    opbx_cli(s->fd, "Response: Success\r\n");
	} else {
	    opbx_log(LOG_DEBUG, "opbx_db_put failed\n");
	    opbx_cli(s->fd, "Response: Failed\r\n");
	}
	if (id && !opbx_strlen_zero(id))
		opbx_cli(s->fd, "ActionID: %s\r\n",id);
	opbx_cli(s->fd, "\r\n");
	return 0;
}

int load_module(void)
{
    if (opbx_channel_register(&devstate_tech)) {
        opbx_log(LOG_DEBUG, "Unable to register channel class %s\n", type);
        return -1;
    }
    opbx_cli_register(&cli_dev_state);  
    opbx_manager_register2( "Devstate", EVENT_FLAG_CALL, action_devstate, "Change a device state", mandescr_devstate );
    return opbx_register_application(app, devstate_exec, synopsis, descrip);
}

int unload_module(void)
{
    int res = 0;
    STANDARD_HANGUP_LOCALUSERS;
    opbx_manager_unregister( "Devstate");
    opbx_cli_unregister(&cli_dev_state);
    res = opbx_unregister_application(app);
    opbx_channel_unregister(&devstate_tech);    
    return res;
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
