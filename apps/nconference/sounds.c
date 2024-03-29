/*
 * app_nconference
 *
 * NConference
 * A channel independent conference application for CallWeaver
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

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/apps/nconference/sounds.c $", "$Revision: 4723 $");

static int conf_play_soundfile( struct cw_conf_member *member, char * file ) 
{
    int res = 0;

    if ( member->dont_play_any_sound ) 
	return 0;

    if ( !member->chan ) 
	return 0;

    cw_stopstream(member->chan);

    queue_incoming_silent_frame(member,3);

    if (
	    ( strrchr(file,'/')!=NULL ) || (cw_fileexists(file, NULL, member->chan->language) > 0) 
       )
    {
	res = cw_streamfile(member->chan, file, member->chan->language);
	if (!res) { 
	    res = cw_waitstream(member->chan, CW_DIGIT_ANY);	
	    cw_stopstream(member->chan);
	}
	//cw_log(LOG_DEBUG, "Soundfile found %s - %d\n", file, cw_fileexists(file, NULL,  member->chan->language) );
    } else 
	cw_log(LOG_DEBUG, "Soundfile not found %s - lang: %s\n", file, member->chan->language );


    cw_set_write_format( member->chan, CW_FORMAT_SLINEAR );
    cw_generator_activate(member->chan,&membergen,member);

    return res;
}



int conf_play_soundqueue( struct cw_conf_member *member ) 
{
    int res = 0;

    cw_stopstream(member->chan);
    queue_incoming_silent_frame(member,3);

    struct cw_conf_soundq *toplay, *delitem;

    cw_mutex_lock(&member->lock);

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
    cw_mutex_unlock(&member->lock);

    if (res != 0)
        conference_stop_sounds( member );

    return res;
}





int conference_queue_sound( struct cw_conf_member *member, char *soundfile )
{
	struct cw_conf_soundq *newsound;
	struct cw_conf_soundq **q;

	if( member == NULL ) {
	    cw_log(LOG_WARNING, "Member is null. Cannot play\n");
	    return 0;
	}

	if( soundfile == NULL ) {
	    cw_log(LOG_WARNING, "Soundfile is null. Cannot play\n");
	    return 0;
	}

	if  (
		( member->force_remove_flag == 1 ) ||
		( member->remove_flag == 1 ) 
	    )
	{
	    return 0;
	}

	newsound = calloc(1,sizeof(struct cw_conf_soundq));

	cw_copy_string(newsound->name, soundfile, sizeof(newsound->name));

	// append sound to the end of the list.

	cw_mutex_lock(&member->lock);

	for( q = &member->soundq; *q; q = &((*q)->next) ) ;;
	*q = newsound;

	cw_mutex_unlock(&member->lock);

	return 0 ;
}


int conference_queue_number( struct cw_conf_member *member, char *str )
{
	struct cw_conf_soundq *newsound;
	struct cw_conf_soundq **q;

	if( member == NULL ) {
	    cw_log(LOG_WARNING, "Member is null. Cannot play\n");
	    return 0;
	}

	if( str == NULL ) {
	    cw_log(LOG_WARNING, "STRING is null. Cannot play\n");
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
		newsound = calloc(1,sizeof(struct cw_conf_soundq));
		cw_copy_string(newsound->name, fn, sizeof(newsound->name));

		// append sound to the end of the list.
		cw_mutex_lock(&member->lock);

		for( q = &member->soundq; *q; q = &((*q)->next) ) ;;
		*q = newsound;

		cw_mutex_unlock(&member->lock);

	    }
	}

	return 0 ;
}


int conference_stop_sounds( struct cw_conf_member *member )
{
	struct cw_conf_soundq *sound;
	struct cw_conf_soundq *next;


	if( member == NULL ) {
	    cw_log(LOG_WARNING, "Member is null. Cannot play\n");
	    return 0;
	}

	// clear all sounds

	cw_mutex_lock(&member->lock);

	sound = member->soundq;
	member->soundq = NULL;

	while(sound) {
	    next = sound->next;
	    free(sound);
	    sound = next;
	}

	cw_mutex_unlock(&member->lock);

	cw_log(CW_CONF_DEBUG,"Stopped sounds to member %s\n", member->chan->name);	
	
	return 0 ;
}

