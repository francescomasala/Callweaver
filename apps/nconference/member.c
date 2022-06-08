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
#include "dtmf.h"
#include "vad.h"

OPENPBX_FILE_VERSION(__FILE__, "$Revision: 2325 $");

/* *****************************************************************************
	MANAGER UTILS
   ****************************************************************************/
void send_state_change_notifications( struct opbx_conf_member* member )
{

#if  ( APP_NCONFERENCE_DEBUG == 1 )
    opbx_log(OPBX_CONF_DEBUG,
	    "Member on channel %s. State changed to %s.\n",
	    member->chan->name,
	    ( ( member->is_speaking == 1 ) ? "speaking" : "silent" )
	);
#endif

    manager_event(
	EVENT_FLAG_CALL, 
	APP_CONFERENCE_MANID"State", 
	"Channel: %s\r\n"
	"State: %s\r\n",
	member->chan->name, 
	( ( member->is_speaking == 1 ) ? "speaking" : "silent" )
    ) ;

    if ( member->is_speaking == 0 ) 
	queue_incoming_silent_frame(member,2);

}

/* *****************************************************************************
	HANDLING OUTGOING PACKETS
   ****************************************************************************/

// process outgoing frames for the channel, playing either normal conference audio,
// or requested sounds
static int process_outgoing( struct opbx_conf_member *member, int samples )
{

    int res;
    struct opbx_frame *cf = NULL;

    opbx_mutex_lock(&member->lock);

    cf=get_outgoing_frame( member->conf, member, samples ) ;

    opbx_mutex_unlock(&member->lock);

/*
    opbx_log(LOG_WARNING,
	    "OURGen: samples %d - conf %s - speak: %d - format: %d\n", 
	    samples, member->chan->name, member->is_speaking , cf->frametype
    );
*/

    // if there's no frames exit the loop.
    if( cf == NULL ) {
        opbx_log( LOG_ERROR, "Nothing to write to the conference, channel => %s\n", member->channel_name ) ;
	return 0;
    }

    // send the voice frame
    res = opbx_write( member->chan, cf );

    if ( ( res != 0) )
    {
        // log 'dropped' outgoing frame
        opbx_log( LOG_ERROR, "unable to write voice frame to channel, channel => %s, samples %d \n", member->channel_name, samples ) ;
    }

    // clean up frame
    opbx_frfree(cf);

    return 0;
}

/* *****************************************************************************
	HANDLING INCOMING PACKETS
   ****************************************************************************/

int process_incoming(struct opbx_conf_member *member, struct opbx_frame *f) 
{
    int res;

    // Play our sound queue, if not empty. 
    if (member->soundq) {
	// Free the frame.
	if ( f != NULL ) {
	    opbx_frfree( f ) ;
	    f = NULL ;
	}
	res = conf_play_soundqueue( member ); 
	if (res != 0) {
	    queue_incoming_silent_frame(member,2);
	    // send the DTMF event to the MGR interface..
	    manager_event(
		EVENT_FLAG_CALL,
		APP_CONFERENCE_MANID"DTMF",
		"Channel: %s\r\n"
		"Key: %c\r\n",
		member->channel_name,
		res
	    ) ;
	    parse_dtmf_option( member, res);
	}
	return res;
    }

    //
    // Moderator forces MOH management
    //

    if ( member->force_on_hold == 1 ) {
	opbx_moh_start(member->chan,"");
	member->force_on_hold = 0 ;
    } 
    else if ( member->force_on_hold == -1 ) {
	opbx_moh_stop(member->chan);
	opbx_generator_activate(member->chan,&membergen,member);
	member->force_on_hold = 0 ;
    } 

    //
    // MOH When the user is alone in the conference
    //
    if ( member->conf->membercount == 1 && 
	 member->is_on_hold == 0 && 
	 member->skip_moh_when_alone == 0 
       ) {
	opbx_moh_start(member->chan,"");
	member->is_on_hold = 1 ;
	return 0;
    }

    if ( member->conf->membercount > 1 && 
	 member->is_on_hold == 1 && 
	 member->skip_moh_when_alone == 0 
       ) {
	opbx_moh_stop(member->chan);
	opbx_generator_activate(member->chan,&membergen,member);
	member->is_on_hold = 0 ;
	return 0;
    }

    if ( member->force_remove_flag == 1 ) {
        return 0;
    }

    // If we don't have any frame to parse, then return
    if ( f == NULL ) {
	return 0;
    }

    // Actions based on the content of the frame
    if ( f->frametype == OPBX_FRAME_DTMF && member->manage_dtmf )
    {	
	queue_incoming_silent_frame(member,2);

	// send the DTMF event to the MGR interface..
	manager_event(
		EVENT_FLAG_CALL,
		APP_CONFERENCE_MANID"DTMF",
		"Channel: %s\r\n"
		"Key: %c\r\n",
		member->channel_name,
		f->subclass
	) ;

	parse_dtmf_option( member, f->subclass);

	opbx_frfree(f);
	f = NULL;
    }
    else if (  (member->type == MEMBERTYPE_LISTENER) || (member->talk_mute) )
    {
	// this is a listen-only user, or it's muted. 	
	// Ignore the frame
	opbx_frfree( f ) ;
	f = NULL ;
    }
    else if ( f->frametype == OPBX_FRAME_VOICE ) 
    {			
	// ********************************************************************************** VOICE
	int old_speaking_state = member->is_speaking;

#if ENABLE_VAD
	if ( member->talk_mute == 1 ) member->is_speaking = 0;

	if ( member->enable_vad 
	     && f->subclass == OPBX_FORMAT_SLINEAR && f->samples > 0 
	   )
	{
	    // and if it's time to check what the member is doing
	    if ( member->skip_voice_detection <= 0 || member->is_speaking ) 
	    {
		int rees;
		rees = vad_is_talk( f->data, f->datalen, &member->silence_nr, 20);
		// send the frame to the preprocessor
		if ( rees != 0 )
		{
		    // voice detected, reset skip count
		    if ( member->framelen != 0 )
			member->skip_voice_detection = (OPBX_CONF_SKIP_MS_AFTER_VOICE_DETECTION / member->framelen);
		    else 
			// Let's suppose that 20ms as a framelen is not too different from the real situation
			member->skip_voice_detection = 20;
		    member->is_speaking=1;
		}
		else {
		    // member is silent
		    member->is_speaking=0;
		    if ( member->framelen != 0 )
			member->skip_voice_detection = ( OPBX_CONF_SKIP_MS_WHEN_SILENT / member->framelen );
		    else 
			member->skip_voice_detection = 5;
		}

	    }
	    --member->skip_voice_detection ;
	}

	if (old_speaking_state != member ->is_speaking)
	    send_state_change_notifications(member);
#endif

	// volume of the frame is modified after the VAD has been done
	if (member->talk_volume != 0) 
	    set_talk_volume(member, f, 1);


	if (  member->is_speaking && queue_incoming_frame( member, f ) != 0 )
	    opbx_log( OPBX_CONF_DEBUG, "dropped incoming frame, channel => %s\n", member->channel_name ) ;

	// free the original frame
	opbx_frfree( f ) ;
	f = NULL ;
    }
    else if ( f->frametype == OPBX_FRAME_CONTROL && f->subclass == OPBX_CONTROL_HANGUP ) 
    {
	// hangup received, queue silence && free the frame 
	queue_incoming_silent_frame(member,2);
	opbx_frfree( f ) ;
	f = NULL ;
    }
    else
    {
	// Unmanaged frame
#if  ( APP_NCONFERENCE_DEBUG == 1 )
	opbx_log(LOG_WARNING,"Freeing unknown frame: type %d  member %s \n", f->frametype, member->chan->name );
#endif
	opbx_frfree( f ) ;
	f = NULL ;
    }

    return 0;
}

/* *****************************************************************************
	MEMBER GENERATOR
   ****************************************************************************/

static void *membergen_alloc(struct opbx_channel *chan, void *params)
{
#if  ( APP_NCONFERENCE_DEBUG == 1 )
    opbx_log(OPBX_CONF_DEBUG,"Allocating generator\n");
#endif
    return params;
}

static void membergen_release(struct opbx_channel *chan, void *data)
{
#if  ( APP_NCONFERENCE_DEBUG == 1 )
    opbx_log(OPBX_CONF_DEBUG,"Releasing generator\n");
#endif
    return;
}


static int membergen_generate(struct opbx_channel *chan, void *data, int samples)
{
    struct opbx_conf_member *member = data;

    // If this is a talker, don't send any packets
    if (member->type==MEMBERTYPE_TALKER)
	return 0;

    if (member->framelen!=0)
        process_outgoing( member, samples );
    return 0;
}

struct opbx_generator membergen = 
{
	alloc: 		membergen_alloc,
	release: 	membergen_release,
	generate: 	membergen_generate,
} ;



/* *****************************************************************************
	HANDLING MEMBERS    
   ****************************************************************************/

int member_exec( struct opbx_channel* chan, void* data ) {
    int left = 0 ;
    int res;

    struct opbx_conference  *conf   	= NULL;
    struct opbx_conf_member *member	= NULL;
    struct opbx_frame *f		= NULL;

    opbx_log( OPBX_CONF_DEBUG, "Launching NConference %s\n", "$Revision: 2325 $" ) ;

    if (chan->_state != OPBX_STATE_UP)
	if ( (res = opbx_answer( chan )) )
	{
    	    opbx_log( LOG_ERROR, "unable to answer call\n" ) ;
    	    return -1 ;
	}

    member = create_member( chan, (const char*)( data ) ) ;

    // unable to create member, return an error
    if ( member == NULL ) 
    {
	opbx_log( LOG_ERROR, "unable to create member\n" ) ;
	return -1 ;
    }

    //
    // setup Openpbx read/write formats
    //
	
    opbx_log( OPBX_CONF_DEBUG, 
		"CHANNEL INFO, CHANNEL => %s, DNID => %s, CALLER_ID => %s, ANI => %s\n", 
		chan->name, chan->cid.cid_dnid, chan->cid.cid_num, chan->cid.cid_ani ) ;

    opbx_log( OPBX_CONF_DEBUG, 
		"CHANNEL CODECS, CHANNEL => %s, NATIVE => %d, READ => %d, WRITE => %d\n", 
		chan->name, chan->nativeformats, member->read_format, member->write_format ) ;


    if ( opbx_set_read_format( chan, member->read_format ) < 0 )
    {
    	opbx_log( LOG_ERROR, "unable to set read format.\n" ) ;
    	delete_member( member ) ;
    	return -1 ;
    } 

    // for right now, we'll send everything as slinear
    if ( opbx_set_write_format( chan, member->write_format ) < 0 )
    {
    	opbx_log( LOG_ERROR, "unable to set write format.\n" ) ;
    	delete_member( member ) ;
    	return -1 ;
    }

    //
    // setup a conference for the new member
    //

    conf = start_conference( member ) ;
	
    if ( conf == NULL )
    {
	opbx_log( LOG_ERROR, "unable to setup member conference\n" ) ;
	delete_member( member) ;
	return -1 ;
    } else {
	if (conf->is_locked && (member->type != MEMBERTYPE_MASTER) ) {
	    if ( strcmp(conf->pin,member->pin) ) {
		conference_queue_sound(member,"conf-locked");
		conf_play_soundqueue( member ); 
		member->force_remove_flag = 1 ;
	    }
	}  else {
	    member->conf = conf;
	    if ( member->type == MEMBERTYPE_MASTER )
		conf->auto_destroy = member->auto_destroy;
	}
    }

    if ( member->type == MEMBERTYPE_MASTER ) {
	conf->auto_destroy = member->auto_destroy;
	if ( strlen( member->pin ) > 0 ) {
	    strncpy(conf->pin,member->pin,sizeof(conf->pin));
	    opbx_log( OPBX_CONF_DEBUG, "Conference pin set to => %s\n", conf->pin ) ;
	}
    }

    //
    // process loop for new member ( this runs in it's own thread
    //
	
    opbx_log( OPBX_CONF_DEBUG, "begin member event loop, channel => %s\n", chan->name ) ;

    // Activate the generator for the channel.
    res = opbx_conf_member_genactivate( member );
    if ( !res ) {
	member->force_remove_flag = 1;
	opbx_log( OPBX_CONF_DEBUG, "member marked for removal => %s\n", chan->name ) ;
    }

    //Play the join info messages
    if (!member->force_remove_flag && !member->quiet_mode) {
	conference_queue_sound( member, "conf-youareinconfnum" );
	conference_queue_number( member, member->id );
    }

    // The member at the very beginningis speaking
    member->is_speaking = 1 ;
    // tell conference_exec we're ready for frames
    member->active_flag = 1 ;

    //Main loop.
    while ( !member->force_remove_flag || member->soundq != NULL )
    {
	usleep(1000);

	// make sure we have a channel to process
	if ( chan == NULL )
	{
	    opbx_log( LOG_NOTICE, "member channel has closed\n" ) ;
	    break ;
	}

	//-----------------//
	// INCOMING FRAMES //
	//-----------------//

	if ( member->force_remove_flag == 1 ) {
	    // break to the loop
	    break;
	}

	// wait for an event on this channel
	int waittime = ( member->framelen == 0 ) ? OPBX_CONF_WAITFOR_TIME : member->framelen;

	left = opbx_waitfor( chan, waittime ) ;

	if ( left < 0 )
	{
	    // an error occured	
	    opbx_log( 
		LOG_NOTICE, 
		"an error occured waiting for a frame, channel => %s, error => %d\n", 
		chan->name, left
		) ;
	}
	else if ( left == 0 )
	{
	    // No frame avalaible
	    member->lostframecount ++;

	    // We have lost a frame.
	    // In this case, we queue some silence
	    // Sothat if we keep loosing frames,
	    // there will be no glitching in the conference.
	    // Set the speaking state to 0.
	    if ( member->lostframecount == 1 ) {
		queue_incoming_silent_frame(member,1);
	    }
	    member->is_speaking = 0;
	}
	else if ( left > 0 ) 
	{
	    // a frame has come in before the latency timeout 
	    // was reached, so we process the frame

	    // let's reset the lost frame count
	    if ( member->lostframecount ) {
		member->lostframecount = 0;
		// If vad is not enabled, then set the speaking state back to 1
		if ( !member->enable_vad )
		    member->is_speaking = 1;
	    }
	    
	    f = opbx_read( chan ) ;
			
	    if ( f == NULL ) 
	    {
		opbx_log( OPBX_CONF_DEBUG, "unable to read from channel, channel => %s. Got Hangup.\n", chan->name ) ;
		queue_incoming_silent_frame(member,5);
		member->is_speaking = 0;
		break ;
	    } 
	    else {
/*
		opbx_log( OPBX_CONF_DEBUG, 
			"Read (PRE dsp), channel => %s, datalen: %d samplefreq: %ld len: %ld samples %d class: %d\n", 
			chan->name, f->datalen, member->samplefreq, f->len, f->samples, f->subclass) ;
*/
		if ( member->samplefreq == 0 && f->samples != 0 )
		{
		    if ( ( f->len == 0 ) && ( f->datalen == 320 ) && ( f->samples == 160 ) )
			member->framelen = 20;				// This is probably chan_zap not setting the correct len.
		    else
			member->framelen   = f->len;			// frame length in milliseconds
		    member->datalen    = f->datalen;			// frame length in milliseconds
		    member->samples    = f->samples;			// number of samples in framelen
		    member->samplefreq = (int)(member->samples/member->framelen)*1000;	// calculated sample frequency
		    opbx_log( OPBX_CONF_DEBUG, "MEMBER FRAME DATA: datalen %d  samples %d  len(ms) %ld, offset: %d \n", f->datalen, f->samples, f->len, f->offset );

/*
		    // Try to initialize the smoother, only once
		    queue_incoming_silent_frame(member);
		    if ( member->smooth_size_in < 0 ) {
			member->smooth_size_in = f->samples ;
			opbx_log( OPBX_CONF_DEBUG, "Initializing Smooother.\n");
			member->inSmoother = opbx_smoother_new(member->smooth_size_in); 
			if ( member->inSmoother == NULL )
			    opbx_log( OPBX_CONF_DEBUG, "Smoother initialization failed\n");
		    }
*/

		} 

		if ( 
			    ( (member->framelen != f->len      ) && ( f->len !=0     ) ) 
				|| 
			    ( (member->samples  != f->samples  ) && ( f->samples !=0 )  && ( f->len !=0     ) )
			) 
		{
		    opbx_log( OPBX_CONF_DEBUG, "FRAME CHANGE  : samples %d  len(ms) %ld\n", f->samples, f->len );
		    opbx_log( OPBX_CONF_DEBUG, "FRAME SHOULDBE: samples %d  len(ms) %ld\n", member->samples, member->framelen );
		    if (member->samples == 0 ) {
			member->framelen   = f->len;				// frame length in milliseconds
			member->datalen    = f->datalen;			// frame length in milliseconds
			member->samples    = f->samples;			// number of samples in framelen
			member->samplefreq = (int) ( f->samples/f->len)*1000;	// calculated sample frequency
		    }
		}
		
		// This fix is for chan_zap
		// Chan_zap NEVER sets framelen value.
		// Probably when adding support to 16Khz we should add a check for this.
		if ( ( member->framelen == 0 ) && ( member->datalen == 320 ) && ( member->samples == 160 ) )
		    member->framelen = 20;
		
	    }
	}

	// actually process the frame.
	res = process_incoming(member, f);

	if (member->force_remove_flag)
	    member->remove_flag = 1 ;

    }

    //
    // clean up
    //

    if ( member != NULL ) 
	member->remove_flag = 1 ;

    opbx_log( OPBX_CONF_DEBUG, "end member event loop, time_entered => %ld -  removal: %d\n", member->time_entered.tv_sec, member->remove_flag ) ;

    //opbx_log( OPBX_CONF_DEBUG, "Deactivating generator - Channel => %s\n", member->chan->name ) ;
    opbx_generator_deactivate(chan);

    return -1 ;
		
}









/* *****************************************************************************
	CREATE/ DESTROY MEMBERS    
   ****************************************************************************/

struct opbx_conf_member *create_member( struct opbx_channel *chan, const char* data ) {

    if ( chan == NULL )
    {
    	opbx_log( LOG_ERROR, "unable to create member with null channel\n" ) ;
    	return NULL ;
    }
	
    if ( chan->name == NULL )
    {
    	opbx_log( LOG_ERROR, "unable to create member with null channel name\n" ) ;
    	return NULL ;
    }
	
    struct opbx_conf_member *member = calloc( 1, sizeof( struct opbx_conf_member ) ) ;
	
    if ( member == NULL ) 
    {
    	opbx_log( LOG_ERROR, "unable to malloc opbx_conf_member\n" ) ;
    	return NULL ;
    }

    //
    // initialize member with passed data values
    //

    // initialize mutex
    opbx_mutex_init( &member->lock ) ;
	
    char argstr[80];
    char *stringp, *token ;

    // copy the passed data
    strncpy( argstr, data, sizeof(argstr) - 1 ) ;

    // point to the copied data
    stringp = argstr ;
	
    opbx_log( OPBX_CONF_DEBUG, "attempting to parse passed params, stringp => %s\n", stringp ) ;
	
    // parse the id
    if ( ( token = strsep( &stringp, "/" ) ) != NULL )
    {
    	member->id = malloc( strlen( token ) + 1 ) ;
    	strcpy( member->id, token ) ;
    }
    else
    {
    	opbx_log( LOG_ERROR, "unable to parse member id\n" ) ;
    	free( member ) ;
    	return NULL ;
    }

    // parse the flags
    if ( ( token = strsep( &stringp, "/" ) ) != NULL )
    {
	member->flags = malloc( strlen( token ) + 1 ) ;
	strcpy( member->flags, token ) ;
    }
    else
    {
	// make member->flags something 
	member->flags = malloc( sizeof( char ) ) ;
	memset( member->flags, 0x0, sizeof( char ) ) ;
    }

    // parse the pin
    if ( ( token = strsep( &stringp, "/" ) ) != NULL )
    {
	member->pin = malloc( strlen( token ) + 1 ) ;
	strcpy( member->pin, token ) ;
    }
    else
    {
	// make member->pin something 
	member->pin = malloc( sizeof( char ) ) ;
	memset( member->pin, 0x0, sizeof( char ) ) ;
    }
	
    // debugging
    opbx_log( 
    	OPBX_CONF_DEBUG, 
	"parsed data params, id => %s, flags => %s, pin %s\n",
	member->id, member->flags, member->pin
    ) ;

    //
    // initialize member with default values
    //

    // keep pointer to member's channel
    member->chan = chan ;
    member->auto_destroy = 1 ;

    // copy the channel name
    member->channel_name = malloc( strlen( chan->name ) + 1 ) ;
    strcpy( member->channel_name, chan->name ) ;
			
    // ( default can be overridden by passed flags )
    member->type = MEMBERTYPE_LISTENER ;

    // linked-list pointer
    member->next = NULL ;
	
    // flags
    member->remove_flag = 0 ;
    member->force_remove_flag = 0 ;

    // record start time
    gettimeofday( &member->time_entered, NULL ) ;

    // Initialize member RTP data
    member->framelen   = 0;		// frame length in milliseconds
    member->samples    = 0;		// number of samples in framelen
    member->samplefreq = 0;		// calculated sample frequency
    member->enable_vad = 0;
    member->enable_vad_allowed = 0;
    member->silence_nr = 1;

    if  (!strncmp(chan->name,"Local",sizeof("Local")) )
	member->enable_vad_allowed = 0;

    // smoother defaults.
    member->smooth_size_in = -1;
    member->smooth_size_out = -1;
    member->inSmoother= NULL;

    // Audio data
    member->talk_volume = 0;		
    member->talk_volume_adjust = 0;		
    member->talk_mute   = 0;		
    member->skip_voice_detection = 10;

    member->quiet_mode = 0;
    member->is_on_hold = 0;
    member->skip_moh_when_alone = 0;

    member->lostframecount = 0;

    //DTMF Data
    member->manage_dtmf = 1;
    member->dtmf_admin_mode=0;
    member->dtmf_long_insert=0;

    //Play conference sounds by default
    member->dont_play_any_sound=0;

    // Zeroing output frame buffer
    memset(member->framedata,0,sizeof(member->framedata));

    //
    // parse passed flags
    //
	
    // temp pointer to flags string
    char* flags = member->flags ;

    int i;
    for ( i = 0 ; i < strlen( flags ) ; ++i )
    {
	// allowed flags are M, L, T, C, V, d
	switch ( flags[i] )
	{
	    // member types ( last flag wins )
		case 'M':
		    member->type = MEMBERTYPE_MASTER ;
		    break ;
		case 'S':
		    member->type = MEMBERTYPE_SPEAKER ;
		    break ;
		case 'L':
		    member->type = MEMBERTYPE_LISTENER ;
		    break ;
		case 'T':
		    member->type = MEMBERTYPE_TALKER ;
		    break ;
		case 'C':
		    member->type = MEMBERTYPE_CONSULTANT ;
		    break ;
		// speex preprocessing options
		case 'V':
#if ENABLE_VAD
		    if  ( strncmp(chan->name,"Local",sizeof("Local")-1) ) {
		        if (member->type != MEMBERTYPE_LISTENER) {
			    member->enable_vad_allowed = 1 ;
			    member->enable_vad = 1 ;
			}
		    } else { 
			member->enable_vad_allowed = 0;
			member->enable_vad = 0 ;
			opbx_log( LOG_WARNING, "VAD Not supported on outgoing channels.\n"); 
		    }
#else
		    opbx_log( LOG_WARNING, "VAD Support is not compiled in. Disabling.\n"); 
#endif	
		    break ;

		// additional features
		case 'd': // Send DTMF manager events..
		    member->manage_dtmf = 0;
		    break;
		case 'm': // don't play MOH when alone
		    member->skip_moh_when_alone = 1;
		    break;
		case 'x': // Don't destroy when empty
		    if ( member->type == MEMBERTYPE_MASTER )
			member->auto_destroy = 0;
		    break;
		case 'q': // Quiet mode
		    member->quiet_mode = 1;
		    break;

		default:
		    opbx_log( LOG_WARNING, "received invalid flag, chan => %s, flag => %c\n", 
			    chan->name, flags[i] ) ;			
		    break ;
	}
    }

    // Circular buffer
    member->cbuf = calloc( 1, sizeof( struct member_cbuffer ) ) ;
	
    if ( member->cbuf == NULL ) 
    {
    	opbx_log( LOG_ERROR, "unable to malloc member_cbuffer\n" ) ;
    	return NULL ;
    } else {
	// initialize it
	memset(member->cbuf, 0, sizeof(struct member_cbuffer) );
    }

    //
    // read, write, and translation options
    //

    opbx_log( OPBX_CONF_DEBUG, "created member on channel %s, type => %d, readformat => %d, writeformat => %d\n", 	
		member->chan->name, member->type, chan->readformat, chan->writeformat ) ;

    // set member's audio formats, taking dsp preprocessing into account
    // ( chan->nativeformats, OPBX_FORMAT_SLINEAR, OPBX_FORMAT_ULAW, OPBX_FORMAT_GSM )
    member->read_format = OPBX_FORMAT_SLINEAR ;
    member->write_format = OPBX_FORMAT_SLINEAR ;

    //
    // finish up
    //
		
    opbx_log( OPBX_CONF_DEBUG, "created member on channel %s, type => %d, readformat => %d, writeformat => %d\n", 	
		member->chan->name, member->type, chan->readformat, chan->writeformat ) ;

    if ( !opbx_generator_is_active(chan) )
	opbx_generator_activate(chan,&membergen,member);


    return member ;
}



int opbx_conf_member_genactivate( struct opbx_conf_member *member ) {
    int res = 1;

    if ( !opbx_generator_is_active(member->chan) )
	res = opbx_generator_activate(member->chan,&membergen,member);

    if (res < 0)
    {
    	opbx_log(LOG_WARNING,"Failed to activate generator on conference '%s'\n",member->chan->name);
    	res = 0;
    }
    else
        res = 1;

    return res ;
}



struct opbx_conf_member* delete_member( struct opbx_conf_member* member ) 
{

    if ( member == NULL )
    {
	opbx_log( LOG_WARNING, "unable to the delete null member\n" ) ;
	return NULL ;
    }

    //
    // clean up member flags
    //
    if ( member->id != NULL )
    {
	opbx_log( OPBX_CONF_DEBUG, "freeing member id, name => %s\n", member->channel_name ) ;
	free( member->id ) ;
    }

    if ( member->flags != NULL )
    {
	opbx_log( OPBX_CONF_DEBUG, "freeing member flags, name => %s\n", member->channel_name ) ;
	free( member->flags ) ;
    }

    if ( member->pin != NULL )
    {
	opbx_log( OPBX_CONF_DEBUG, "freeing member pin, name => %s\n", member->channel_name ) ;
	free( member->pin ) ;
    }

    if ( member->cbuf != NULL )
    {
	opbx_log( OPBX_CONF_DEBUG, "freeing member circular buffer, name => %s\n", member->channel_name ) ;
	free( member->cbuf ) ;
    }
	
    //
    // delete the members frames
    //

    // free the member's copy for the channel name
    free( member->channel_name ) ;

    // free the smoother
    if (member->inSmoother != NULL)
    	opbx_smoother_free(member->inSmoother);
	
    // get a pointer to the next 
    // member so we can return it
    struct opbx_conf_member* nm = member->next ;
	
    opbx_mutex_destroy( &member->lock ) ;

    opbx_log( OPBX_CONF_DEBUG, "freeing member\n" ) ;
    // free the member's memory

    free( member ) ;
    member = NULL ;
	
    return nm ;
}


char *membertypetostring  (int member_type ) {

    switch (member_type) {
	case MEMBERTYPE_MASTER:
	    return "Moderator";
	    break;
	case MEMBERTYPE_SPEAKER:
	    return "Speaker";
	    break;
	case MEMBERTYPE_LISTENER:
	    return "Listener";
	    break;
	case MEMBERTYPE_TALKER:
	    return "Talker";
	    break;
	case MEMBERTYPE_CONSULTANT:
	    return "Consultant";
	    break;
    };
    return "Unknown";
}

