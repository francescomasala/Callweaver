/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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
 * \brief Device state management
 */

#ifndef _OPENPBX_DEVICESTATE_H
#define _OPENPBX_DEVICESTATE_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*! Device is valid but channel didn't know state */
#define OPBX_DEVICE_UNKNOWN	0
/*! Device is not used */
#define OPBX_DEVICE_NOT_INUSE	1
/*! Device is in use */
#define OPBX_DEVICE_INUSE	2
/*! Device is busy */
#define OPBX_DEVICE_BUSY	3
/*! Device is invalid */
#define OPBX_DEVICE_INVALID	4
/*! Device is unavailable */
#define OPBX_DEVICE_UNAVAILABLE	5
/*! Device is ringing */
#define OPBX_DEVICE_RINGING	6

typedef int (*opbx_devstate_cb_type)(const char *dev, int state, void *data);

/*! \brief Convert device state to text string for output 
 * \param devstate Current device state 
 */
const char *devstate2str(int devstate);

/*! \brief Search the Channels by Name
 * \param device like a dialstring
 * Search the Device in active channels by compare the channelname against 
 * the devicename. Compared are only the first chars to the first '-' char.
 * Returns an OPBX_DEVICE_UNKNOWN if no channel found or
 * OPBX_DEVICE_INUSE if a channel is found
 */
int opbx_parse_device_state(const char *device);

/*! \brief Asks a channel for device state
 * \param device like a dialstring
 * Asks a channel for device state, data is  normaly a number from dialstring
 * used by the low level module
 * Trys the channel devicestate callback if not supported search in the
 * active channels list for the device.
 * Returns an OPBX_DEVICE_??? state -1 on failure
 */
int opbx_device_state(const char *device);

/*! \brief Tells OpenPBX the State for Device is changed
 * \param fmt devicename like a dialstring with format parameters
 * OpenPBX polls the new extensionstates and calls the registered
 * callbacks for the changed extensions
 * Returns 0 on success, -1 on failure
 */
int opbx_device_state_changed(const char *fmt, ...)
	__attribute__ ((format (printf, 1, 2)));


/*! \brief Tells OpenPBX the State for Device is changed 
 * \param device devicename like a dialstrin
 * OpenPBX polls the new extensionstates and calls the registered
 * callbacks for the changed extensions
 * Returns 0 on success, -1 on failure
 */
int opbx_device_state_changed_literal(const char *device);

/*! \brief Registers a device state change callback 
 * \param data to pass to callback
 * The callback is called if the state for extension is changed
 * Return -1 on failure, ID on success
 */ 
int opbx_devstate_add(opbx_devstate_cb_type callback, void *data);
void opbx_devstate_del(opbx_devstate_cb_type callback, void *data);

int opbx_device_state_engine_init(void);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _OPENPBX_DEVICESTATE_H */
