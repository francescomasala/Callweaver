/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Martin Pycko <martinp@digium.com>
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
 * \brief Internal OpenPBX hangup causes
 */

#ifndef _OPENPBX_CAUSES_H
#define _OPENPBX_CAUSES_H

/* Causes for disconnection (from Q.931) */
#define OPBX_CAUSE_UNALLOCATED				1
#define OPBX_CAUSE_NO_ROUTE_TRANSIT_NET			2
#define OPBX_CAUSE_NO_ROUTE_DESTINATION			3
#define OPBX_CAUSE_CHANNEL_UNACCEPTABLE			6
#define OPBX_CAUSE_CALL_AWARDED_DELIVERED		7
#define OPBX_CAUSE_NORMAL_CLEARING			16
#define OPBX_CAUSE_USER_BUSY				17
#define OPBX_CAUSE_NO_USER_RESPONSE			18
#define OPBX_CAUSE_NO_ANSWER				19
#define OPBX_CAUSE_CALL_REJECTED				21
#define OPBX_CAUSE_NUMBER_CHANGED			22
#define OPBX_CAUSE_NONSELECTED_USER_CLEARING		26
#define OPBX_CAUSE_DESTINATION_OUT_OF_ORDER		27
#define OPBX_CAUSE_INVALID_NUMBER_FORMAT			28
#define OPBX_CAUSE_FACILITY_REJECTED			29
#define OPBX_CAUSE_RESPONSE_TO_STATUS_ENQUIRY		30
#define OPBX_CAUSE_NORMAL_UNSPECIFIED			31
#define OPBX_CAUSE_NORMAL_CIRCUIT_CONGESTION		34
#define OPBX_CAUSE_NETWORK_OUT_OF_ORDER			38
#define OPBX_CAUSE_NORMAL_TEMPORARY_FAILURE		41
#define OPBX_CAUSE_SWITCH_CONGESTION			42
#define OPBX_CAUSE_ACCESS_INFO_DISCARDED			43
#define OPBX_CAUSE_REQUESTED_CHAN_UNAVAIL		44
#define OPBX_CAUSE_PRE_EMPTED				45
#define OPBX_CAUSE_FACILITY_NOT_SUBSCRIBED  		50
#define OPBX_CAUSE_OUTGOING_CALL_BARRED     		52
#define OPBX_CAUSE_INCOMING_CALL_BARRED     		54
#define OPBX_CAUSE_BEARERCAPABILITY_NOTAUTH		57
#define OPBX_CAUSE_BEARERCAPABILITY_NOTAVAIL     	58
#define OPBX_CAUSE_BEARERCAPABILITY_NOTIMPL		65
#define OPBX_CAUSE_CHAN_NOT_IMPLEMENTED     		66
#define OPBX_CAUSE_FACILITY_NOT_IMPLEMENTED      	69
#define OPBX_CAUSE_INVALID_CALL_REFERENCE		81
#define OPBX_CAUSE_INCOMPATIBLE_DESTINATION		88
#define OPBX_CAUSE_INVALID_MSG_UNSPECIFIED  		95
#define OPBX_CAUSE_MANDATORY_IE_MISSING			96
#define OPBX_CAUSE_MESSAGE_TYPE_NONEXIST			97
#define OPBX_CAUSE_WRONG_MESSAGE				98
#define OPBX_CAUSE_IE_NONEXIST				99
#define OPBX_CAUSE_INVALID_IE_CONTENTS			100
#define OPBX_CAUSE_WRONG_CALL_STATE			101
#define OPBX_CAUSE_RECOVERY_ON_TIMER_EXPIRE		102
#define OPBX_CAUSE_MANDATORY_IE_LENGTH_ERROR		103
#define OPBX_CAUSE_PROTOCOL_ERROR			111
#define OPBX_CAUSE_INTERWORKING				127

/* Special OpenPBX aliases */
#define OPBX_CAUSE_BUSY 					OPBX_CAUSE_USER_BUSY
#define OPBX_CAUSE_FAILURE 				OPBX_CAUSE_NETWORK_OUT_OF_ORDER
#define OPBX_CAUSE_NORMAL 				OPBX_CAUSE_NORMAL_CLEARING
#define OPBX_CAUSE_NOANSWER	 			OPBX_CAUSE_NO_ANSWER
#define OPBX_CAUSE_CONGESTION	 			OPBX_CAUSE_NORMAL_CIRCUIT_CONGESTION
#define OPBX_CAUSE_UNREGISTERED				OPBX_CAUSE_NO_ROUTE_DESTINATION
#define OPBX_CAUSE_NOTDEFINED 				0
#define OPBX_CAUSE_NOSUCHDRIVER				OPBX_CAUSE_CHAN_NOT_IMPLEMENTED

#endif /* _OPENPBX_CAUSES_H */