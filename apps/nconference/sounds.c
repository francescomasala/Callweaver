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
#include "member.h"
#include "frame.h"
#include "sounds.h"

OPENPBX_FILE_VERSION(__FILE__, "$Revision: 2308 $");

static int conf_play_soundfile( struct opbx_conf_member *member, char * file ) 
{
    int res = 0;

    if ( member->dont_play_any_sound ) 
	return 0;

    if ( !member->chan ) 
	return 0;

    opbx_stopstream(member->chan);

    queue_incoming_silent_frame(member,3);

    if (
	    ( strrchr(file,'/')!=NULL ) || (opbx_fileexists(file, NULL, member->chan->language) > 0) 
       )
    {
	res = opbx_streamfile(member->chan, file, member->chan->language);
	if (!res) { 
	    res = opbx_waitstream(member->chan, OPBX_DIGIT_ANY);	
	    opbx_stopstream(member->chan);
	}
	//opbx_log(LOG_DEBUG, "Soundfile found %s - %d\n", file, opbx_fileexists(file, NULL,  member->chan->language) );
    } else 
	opbx_log(LOG_DEBUG, "Soundfile not found %s - lang: %s\n", file, member->chan->language );


    opbx_set_write_format( member->chan, OPBX_FORMAT_SLINEAR );
    opbx_generator_activate(member->chan,&membergen,member);

    return res;
}



int conf_play_soundqueue( struct opbx_conf_member *member ) 
{
    int res = 0;

    opbx_stopstream(member->chan);
    queue_incoming_silent_frame(member,3);

    struct opbx_conf_soundq *toplay, *delitem;

    opbx_mutex_lock(&member->lock);

    toplay = member->soundq;
    while (  ( toplay != NULL) && ( res == 0 )  ) {

	manager_event(
		EVENT_FLAG_CALL, 
		APP_CONFERENCE_MANID"Sound",
		"Channel: %s\r\n"
		"Sound: %s\r\n",
		member->channel_name, 
		toplay->name
	);

	res = conf_play_soundfile( member, toplay->name );
	if (res) break;

	delitem = toplay;
	toplay = toplay->next;
	member->soundq = toplay;
	free(delitem);
    }
    opbx_mutex_unlock(&member->lock);

    if (res != 0)
        conference_stop_sounds( member );

    return res;
}





int conference_queue_sound( struct opbx_conf_member *member, char *soundfile )
{
	struct opbx_conf_soundq *newsound;
	struct opbx_conf_soundq **q;

	if( member == NULL ) {
	    opbx_log(LOG_WARNING, "Member is null. Cannot play\n");
	    return 0;
	}

	if( soundfile == NULL ) {
	    opbx_log(LOG_WARNING, "Soundfile is null. Cannot play\n");
	    return 0;
	}

	if  (
		( member->force_remove_flag == 1 ) ||
		( member->remove_flag == 1 ) 
	    )
	{
	    return 0;
	}

	newsound = calloc(1,sizeof(struct opbx_conf_soundq));

	opbx_copy_string(newsound->name, soundfile, sizeof(newsound->name));

	// append sound to the end of the list.

	opbx_mutex_lock(&member->lock);

	for( q = &member->soundq; *q; q = &((*q)->next) ) ;;
	*q = newsound;

	opbx_mutex_unlock(&member->lock);

	return 0 ;
}


int conference_queue_number( struct opbx_conf_member *member, char *str )
{
	struct opbx_conf_soundq *newsound;
	struct opbx_conf_soundq **q;

	if( member == NULL ) {
	    opbx_log(LOG_WARNING, "Member is null. Cannot play\n");
	    return 0;
	}

	if( str == NULL ) {
	    opbx_log(LOG_WARNING, "STRING is null. Cannot play\n");
	    return 0;
	}

	if  (
		( member->force_remove_flag == 1 ) ||
		( member->remove_flag == 1 ) 
	    )
	{
	    return 0;
	}

	const char *fn = NULL;
	char soundfile[255] = "";
	int num = 0;
	int res = 0;

	while (str[num] && !res) {
		fn = NULL;
		switch (str[num]) {
		case ('*'):
			fn = "digits/star";
			break;
		case ('#'):
			fn = "digits/pound";
			break;
		case ('-'):
			fn = "digits/minus";
			break;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			strcpy(soundfile, "digits/X");
			soundfile[7] = str[num];
			fn = soundfile;
			break;
		}
		num++;

	    if (fn) {
		newsound = calloc(1,sizeof(struct opbx_conf_soundq));
		opbx_copy_string(newsound->name, fn, sizeof(newsound->name));

		// append sound to the end of the list.
		opbx_mutex_lock(&member->lock);

		for( q = &member->soundq; *q; q = &((*q)->next) ) ;;
		*q = newsound;

		opbx_mutex_unlock(&member->lock);

	    }
	}

	return 0 ;
}


int conference_stop_sounds( struct opbx_conf_member *member )
{
	struct opbx_conf_soundq *sound;
	struct opbx_conf_soundq *next;


	if( member == NULL ) {
	    opbx_log(LOG_WARNING, "Member is null. Cannot play\n");
	    return 0;
	}

	// clear all sounds

	opbx_mutex_lock(&member->lock);

	sound = member->soundq;
	member->soundq = NULL;

	while(sound) {
	    next = sound->next;
	    free(sound);
	    sound = next;
	}

	opbx_mutex_unlock(&member->lock);

	opbx_log(OPBX_CONF_DEBUG,"Stopped sounds to member %s\n", member->chan->name);	
	
	return 0 ;
}

