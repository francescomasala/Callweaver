/*
 * app_nconference
 *
 * NConference
 * A channel independent conference application for Openpbx
 *
 * Copyright (C) 2002, 2003 Navynet SRL
 * http://www.navynet.it
 *
 * Massimo "CtRiX" Cetra - ctrix (at) navynet.it
 *
 * This program may be modified and distributed under the 
 * terms of the GNU Public License V2.
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif
#include <stdio.h>
#include "common.h"
#include "conference.h"
#include "sounds.h"
#include "member.h"
#include "frame.h"
#include "dtmf.h"

OPENPBX_FILE_VERSION(__FILE__, "$Revision: 2308 $");

int parse_dtmf_option( struct opbx_conf_member *member, int subclass ) {


    if ( !member->dtmf_admin_mode && !member->dtmf_long_insert )

	    // *************************************************************** DTMF NORMAL MODE

	switch (subclass) {
	    case '*':
		if ( member->type != MEMBERTYPE_MASTER )
		    break;
		member->dtmf_admin_mode=1;
		member->dtmf_buffer[0]='\0';
		opbx_log(OPBX_CONF_DEBUG,"Dialplan admin mode activated\n" );
		conference_queue_sound( member, "conf-sysop" );
		break;
	    case '#': 
		opbx_log(OPBX_CONF_DEBUG,"Disconnecting member from conference %s after request\n",member->chan->name);
		member->force_remove_flag = 1 ;
		opbx_softhangup(member->chan,OPBX_SOFTHANGUP_SHUTDOWN);
		break;
	    case '1': 
		conference_queue_sound( member, "beep" );
		member->talk_volume = (member->talk_volume > -5) ? member->talk_volume-1 : member->talk_volume;
		opbx_log(OPBX_CONF_DEBUG,"TALK Volume DOWN to %d\n",member->talk_volume);
		if (member->talk_volume) set_talk_volume(member, NULL, 1);
		break;
	    case '2': 
		member->talk_mute = (member->talk_mute == 0 ) ? 1 : 0;
		queue_incoming_silent_frame(member,3);
		if ( member->talk_mute == 1) {
    		    opbx_moh_start(member->chan,"");
		    if ( member->is_speaking == 1 ) { 
#if ENABLE_VAD
			member->is_speaking = 0;
			send_state_change_notifications(member);
#endif
		    }
		} 
		else {
    		    opbx_moh_stop(member->chan);
		    opbx_generator_activate(member->chan,&membergen,member);
		}
		opbx_log(OPBX_CONF_DEBUG,"Volume MUTE (muted: %d)\n",member->talk_mute);
		break;
	    case '3': 
		conference_queue_sound( member, "beep" );
		member->talk_volume = (member->talk_volume < 5) ? member->talk_volume+1 : member->talk_volume;
		opbx_log(OPBX_CONF_DEBUG,"TALK Volume UP %d\n",member->talk_volume);
		if (member->talk_volume) set_talk_volume(member, NULL, 1);
		break;
	    case '4': 
#if ENABLE_VAD
		if (member->enable_vad_allowed) {
		    member->enable_vad = ( member->enable_vad ==0 ) ? 1 : 0;
		    // if we disable VAD, Then the user is always speaking 
		    if (!member->enable_vad) {
			member->is_speaking = 1;
			conference_queue_sound( member, "disabled" );
		    } else 
			conference_queue_sound( member, "enabled" );
		    opbx_log(OPBX_CONF_DEBUG,"Member VAD set to %d\n",member->enable_vad);
		}
		else
#endif
		    opbx_log(OPBX_CONF_DEBUG,"Member not enabled for VAD\n");
		break;
	    case '5':
		queue_incoming_silent_frame(member,3);
		member->talk_mute =  !(member->talk_mute);
		if (member->talk_mute)
		    conference_queue_sound( member, "conf-muted" );
		else
		    conference_queue_sound( member, "conf-unmuted" );
		opbx_log(OPBX_CONF_DEBUG,"Member Talk MUTE set to %d\n",member->dont_play_any_sound);
		break;
	    case '6':
		member->dont_play_any_sound =  !(member->dont_play_any_sound);
		opbx_log(OPBX_CONF_DEBUG,"Member dont_play_any_sound set to %d\n",member->dont_play_any_sound);
		if (!member->dont_play_any_sound)
		    conference_queue_sound(member,"beep");
		break;
	    case '9':
		    conference_queue_sound(member,"conf-getpin");
		    member->dtmf_buffer[0]='\0';
		    member->dtmf_long_insert=1;
		break;
	    case '0': {
		    char buf[10];
		    snprintf(buf, sizeof(buf), "%d", member->conf->membercount);
		    conference_queue_sound(member,"conf-thereare");
		    conference_queue_number(member, buf );
		    conference_queue_sound(member,"conf-peopleinconf");
		    }
		break;

	    default:
		//opbx_log(OPBX_CONF_DEBUG, "DTMF received - Key %c\n",f->subclass);
		opbx_log(OPBX_CONF_DEBUG,"Don't know how to manage %c DTMF\n",subclass);
		break;
	}
    else if ( !member->dtmf_admin_mode && member->dtmf_long_insert ) {
	    switch (subclass) {
		case '*':
		    member->dtmf_long_insert=0;
		    break;
		case '#':
		    member->dtmf_long_insert=0;
		    opbx_log(OPBX_CONF_DEBUG,"Pin entered %s does match ?\n",member->dtmf_buffer);
		    if ( strcmp( member->dtmf_buffer, member->conf->pin ) )
			conference_queue_sound(member,"conf-invalidpin");
		    else {
			conference_queue_sound(member,"beep");
			member->type = MEMBERTYPE_MASTER;
		    }
		    member->dtmf_buffer[0]='\0';
		    break;
		default: {
		    char t[2];
		    t[0] = subclass;
		    t[1] = '\0';
		    if ( strlen(member->dtmf_buffer)+1 < sizeof(member->dtmf_buffer)  ) {
			strcat(member->dtmf_buffer,t);
		    }
		    opbx_log(OPBX_CONF_DEBUG,"DTMF Buffer: %s \n",member->dtmf_buffer);
		    }
		    break;
	    }
    }

    else if (member->dtmf_admin_mode) {
	
	    // *************************************************************** DTMF ADMIN MODE

	    if ( subclass == '*' ) { 
		member->dtmf_admin_mode=0;
		opbx_log(OPBX_CONF_DEBUG,"Dialplan admin mode deactivated\n" );
	    } 
	    else if ( subclass == '#' ) { 
		member->dtmf_admin_mode=0;
		if ( strlen(member->dtmf_buffer) >= 1 ) {
		    opbx_log(OPBX_CONF_DEBUG,"Admin mode. Trying to parse command %s\n",member->dtmf_buffer );
		    conference_parse_admin_command(member);
		}
		else
		    opbx_log(OPBX_CONF_DEBUG,"Admin mode. Invalid empty command (%s)\n",member->dtmf_buffer );
	    } 
	    else {
		char t[2];
		t[0] = subclass;
		t[1] = '\0';
		if ( strlen(member->dtmf_buffer)+1 < sizeof(member->dtmf_buffer)  ) {
		    strcat(member->dtmf_buffer,t);
		}
		opbx_log(OPBX_CONF_DEBUG,"DTMF Buffer: %s \n",member->dtmf_buffer);

	    }
    }

    return 0;
}
