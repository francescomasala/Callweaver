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

// mutex for synchronizing access to conflist
OPBX_MUTEX_DEFINE_EXPORTED(conflist_lock);

// mutex for synchronizing calls to start_conference() and remove_conf()
OPBX_MUTEX_DEFINE_STATIC(start_stop_conf_lock);


// single-linked list of current conferences
struct opbx_conference *conflist = NULL ;
int conference_count = 0 ;


// called by app_conference.c:load_module()
void init_conference( void ) 
{
    opbx_mutex_init( &conflist_lock ) ;
    opbx_mutex_init( &start_stop_conf_lock ) ;
}

/* ********************************************************************************************** 
     command queue related functions
   *********************************************************************************************/

int add_command_to_queue ( 
	struct opbx_conference *conf, struct opbx_conf_member *member ,
	int command, int param_number, char *param_text 
	) 
{

    struct opbx_conf_command_queue *cq, *cq_last;

    cq = calloc(1, sizeof( struct opbx_conf_command_queue ) ) ;
	
    if ( cq == NULL ) 
    {
    	opbx_log( LOG_ERROR, "unable to malloc opbx_conf_command_queue\n" ) ;
    	return 1 ;
    }

    cq->next = NULL;
    cq->command = command;
    cq->issuedby = member;
    cq->param_number = param_number;
    strncpy(cq->param_text, param_text, sizeof(cq->param_text) );

    opbx_mutex_lock( &conf->lock ) ;

    if ( conf->command_queue == NULL ) {
	conf->command_queue = cq;
    } else {
	cq_last = conf->command_queue;
	while ( cq_last->next != NULL ) 
	    cq_last = cq_last->next;
	cq_last->next = cq;
    }

    opbx_log( OPBX_CONF_DEBUG, "Conference, name => %s - Added command %d params: '%d/%s'\n", 
	      conf->name, cq->command, cq->param_number, cq->param_text );

    opbx_mutex_unlock( &conf->lock ) ;

    return 1;
}

static struct opbx_conf_command_queue *get_command_from_queue ( struct opbx_conference *conf ) 
{
    struct opbx_conf_command_queue *cq;

    opbx_mutex_lock( &conf->lock ) ;

    cq = conf->command_queue;

    if (cq != NULL)
	conf->command_queue = cq->next;

    opbx_mutex_unlock( &conf->lock ) ;

    return cq;    
}

static void opbx_conf_command_execute( struct opbx_conference *conf ) {
    struct opbx_conf_command_queue 	*cq;
    struct opbx_conf_member *member 	= NULL ;

    cq = get_command_from_queue(conf);

    if ( cq == NULL ) return;

    opbx_log(OPBX_CONF_DEBUG,"Parsing Command Queue for conference %s\n",conf->name);

    switch (cq->command) {
	case CONF_ACTION_MUTE_ALL: 
	    // get list of conference members
	    member = conf->memberlist ;
	    // loop over member list to retrieve queued frames
	    while ( member != NULL )
	    {
		if (member != cq->issuedby) {
		    opbx_mutex_lock( &member->lock ) ;
		    queue_incoming_silent_frame(member,2);
		    member->talk_mute = cq->param_number;
		    opbx_log(OPBX_CONF_DEBUG,"(CQ) Member Talk MUTE set to %d\n",member->talk_mute);
		    if (cq->param_number)
			conference_queue_sound( member, "conf-muted" );
		    else
			conference_queue_sound( member, "conf-unmuted" );
		    opbx_mutex_unlock( &member->lock ) ;
		}
		// adjust our pointer to the next inline
		member = member->next ;
	    } 
	    break;
	case CONF_ACTION_ENABLE_SOUNDS: 
	    // get list of conference members
	    member = conf->memberlist ;
	    // loop over member list to retrieve queued frames
	    while ( member != NULL )
	    {
		opbx_mutex_lock( &member->lock ) ;
		queue_incoming_silent_frame(member,2);
		member->dont_play_any_sound =  cq->param_number;
		opbx_log(OPBX_CONF_DEBUG,"(CQ) Member Talk Disable sounds set to %d\n",member->dont_play_any_sound);
		opbx_mutex_unlock( &member->lock ) ;
		// adjust our pointer to the next inline
		member = member->next ;
	    } 
	    break;
	case CONF_ACTION_QUEUE_SOUND: 
	    // get list of conference members
	    member = conf->memberlist ;
	    // loop over member list to retrieve queued frames
	    while ( member != NULL )
	    {
		if (member != cq->issuedby) {
		    opbx_mutex_lock( &member->lock ) ;
		    queue_incoming_silent_frame(member,2);
		    if ( !(member->quiet_mode == 1 && cq->param_number) )
			conference_queue_sound( member, cq->param_text );
		    opbx_mutex_unlock( &member->lock ) ;
		    // adjust our pointer to the next inline
		}
		    member = member->next ;
	    } 
	    break;
	case CONF_ACTION_QUEUE_NUMBER: 
	    // get list of conference members
	    member = conf->memberlist ;
	    // loop over member list to retrieve queued frames
	    while ( member != NULL )
	    {
		if (member != cq->issuedby) {
		    opbx_mutex_lock( &member->lock ) ;
		    queue_incoming_silent_frame(member,2);
		    if ( !(member->quiet_mode == 1 && cq->param_number) )
			conference_queue_number( member, cq->param_text );
		    opbx_mutex_unlock( &member->lock ) ;
		    // adjust our pointer to the next inline
		}
		member = member->next ;
	    } 
	    break;
	case CONF_ACTION_PLAYMOH: 
	    // get list of conference members
	    member = conf->memberlist ;
	    // loop over member list to retrieve queued frames
	    while ( member != NULL )
	    {
		if (member != cq->issuedby) {;
		    opbx_mutex_lock( &member->lock ) ;
		    if ( cq->param_number == 1) {
			member->force_on_hold =  1;
		    } 
		    else {
			member->force_on_hold = -1;
		    }
		    opbx_mutex_unlock( &member->lock ) ;
		    opbx_log(OPBX_CONF_DEBUG,"(CQ) Member: playing moh set to %d\n",cq->param_number);
		    // adjust our pointer to the next inline
		}
		member = member->next ;
	    } 
	    break;
	case CONF_ACTION_HANGUP: 
	    member = conf->memberlist ;
	    // Scan all the members
	    while ( member != NULL )
	    {
		// If it's not me and we don't have to kick all members
		if (member != cq->issuedby) {
		    opbx_mutex_lock( &member->lock ) ;
		    queue_incoming_silent_frame(member,2);
		    if (cq->param_number == 0) 
			conference_queue_sound( member, "goodbye" );
		    else
			conference_queue_sound( member, "conf-kicked" );
		    member->force_remove_flag = 1;
		    opbx_log(OPBX_CONF_DEBUG,"(CQ) Conf %s Member Kicked: %s\n",conf->name, member->chan->name);
		    opbx_mutex_unlock( &member->lock ) ;
		    if (cq->param_number == 1) 
			break;
		}
		// adjust our pointer to the next inline
		member 	= member->next ;
	    } 
	    break;
	default:
	    opbx_log( LOG_WARNING, "Conference %s : don't know how to execute command %d\n", conf->name, cq->command) ;	
	    break;
    }

    // Free the struct we got
    free(cq);
}

/* ********************************************************************************************** 
     member-related functions
   *********************************************************************************************/

void add_member( struct opbx_conference *conf, struct opbx_conf_member *member ) 
{
    char cnum[80];

    if ( conf == NULL ) 
    {
    	opbx_log( LOG_ERROR, "unable to add member to NULL conference\n" ) ;
    	return ;
    }

    // acquire the conference lock
    opbx_mutex_lock( &conf->lock ) ;

    member->next = conf->memberlist ; // next is now list
    conf->memberlist = member ; // member is now at head of list
    conf->membercount ++;


    // release the conference lock
    opbx_mutex_unlock( &conf->lock ) ;	

    if ( (member->chan != NULL) && (member->chan->cid.cid_num != NULL) )
	strncpy( cnum, member->chan->cid.cid_num, sizeof(cnum) );
    else
	strncpy( cnum, "", sizeof(cnum) );
    queue_incoming_silent_frame(member,2);
    add_command_to_queue( conf, member, CONF_ACTION_QUEUE_NUMBER , 1, cnum );
    add_command_to_queue( conf, member, CONF_ACTION_QUEUE_SOUND  , 1, "conf-hasjoin" );

    opbx_log( OPBX_CONF_DEBUG, "member added to conference, name => %s\n", conf->name ) ;	

    manager_event(
	EVENT_FLAG_CALL, 
	APP_CONFERENCE_MANID"Join", 
	"Channel: %s\r\n",
	member->chan->name
    ) ;

    return ;
}


int remove_member(struct opbx_conference* conf, struct opbx_conf_member* member ) 
{
    // check for member
    if ( member == NULL )
    {
	opbx_log( LOG_WARNING, "unable to remove NULL member\n" ) ;
	return -1 ;
    }

    // check for conference
    if ( conf == NULL )
    {
    	opbx_log( LOG_WARNING, "unable to remove member from NULL conference\n" ) ;
    	return -1 ;
    }

    //
    // loop through the member list looking for the requested member
    //

    struct opbx_conf_member *member_list = conf->memberlist ;
    struct opbx_conf_member *member_temp = NULL ;

    while ( member_list != NULL ) 
    {
    	if ( member_list == member ) 
    	{
	    //
	    // if this is the first member in the linked-list,
	    // skip over the first member in the list, else
	    //
	    // point the previous 'next' to the current 'next',
	    // thus skipping the current member in the list	
	    //	
	    if ( member_temp == NULL )
		conf->memberlist = member->next ;
	    else 
		member_temp->next = member->next ;

	    manager_event(
		EVENT_FLAG_CALL, 
		APP_CONFERENCE_MANID"Leave", 
		"Channel: %s\r\n",
		member->chan->name
	    ) ;

	    // delete the member
	    delete_member( member ) ;
			
	    conf->membercount --;
	    opbx_log( OPBX_CONF_DEBUG, "removed member from conference, name => %s\n", conf->name) ;
			
	    break ;
	}
		
	// save a pointer to the current member,
	// and then point to the next member in the list
	member_temp = member_list ;
	member_list = member_list->next ;
    }
	
    // return -1 on error, or the number of members 
    // remaining if the requested member was deleted
    return conf->membercount ;
}

/* ********************************************************************************************** 
     conference-related functions
   *********************************************************************************************/

void conference_exec( struct opbx_conference *conf ) 
{

    struct opbx_conf_member *member, *temp_member ;
    struct timeval empty_start = {0,0}, tv = {0,0} ;
	
    opbx_log( OPBX_CONF_DEBUG, "Entered conference_exec, name => %s\n", conf->name ) ;
	
    //
    // main conference thread loop
    //

    while ( 1 )
    {

	//Acquire the mutex
	opbx_mutex_lock( &conf->lock );
	
	// get list of conference members
	member = conf->memberlist ;

	// loop over member list to retrieve queued frames
	while ( member != NULL )
	{
	    opbx_mutex_lock( &member->lock ) ;
	    // check for dead members
	    if ( member->remove_flag == 1 ) 
	    {
		char cnum[80];
	    	opbx_log( OPBX_CONF_DEBUG, "found member slated for removal, channel => %s\n", member->channel_name ) ;
	    	temp_member = member->next ;
		if ( (member->chan != NULL) && (member->chan->cid.cid_num != NULL) )
		    strncpy( cnum,member->chan->cid.cid_num,sizeof(cnum) );
		else
		    strncpy( cnum, "", sizeof(cnum) );
		queue_incoming_silent_frame(member,2);
	    	remove_member( conf, member ) ;
	    	member = temp_member ;
	        add_command_to_queue( conf, NULL, CONF_ACTION_QUEUE_NUMBER , 1, cnum );
	        add_command_to_queue( conf, NULL, CONF_ACTION_QUEUE_SOUND , 1, "conf-hasleft" );
	    	continue ;
	    }
	    opbx_mutex_unlock( &member->lock ) ;
	    // adjust our pointer to the next inline
	    member = member->next ;
	} 
	//
	// break, if we have no more members
	//

	if ( conf->memberlist == NULL ) {
	    if ( empty_start.tv_sec == 0 ) 
 		gettimeofday( &empty_start, NULL );

	    if (conf->auto_destroy) {
		opbx_log( OPBX_CONF_DEBUG, "removing conference, count => %d, name => %s\n", conf->membercount, conf->name ) ;
		remove_conf( conf ) ; // stop the conference
		break ; // break from main processing loop
	    } else {
		//opbx_log( OPBX_CONF_DEBUG, "Delaying conference removal. Auto destroy is not set.\n" ) ;
 		gettimeofday( &tv, NULL ) ;
		if ( tv.tv_sec - empty_start.tv_sec > OPBX_CONF_DESTROY_TIME ) 
		    conf->auto_destroy=1;
	    }
	} else {
	    empty_start.tv_sec = 0;
	}


	//
	// Check for the commands to be executed by the conference
	//
	if (conf->command_queue) 
	    opbx_conf_command_execute( conf );

	//---------//
	// CLEANUP //
	//---------//

	// release conference mutex
	opbx_mutex_unlock( &conf->lock ) ;

	//Sleep for 1ms (relax)
	usleep(1000);
    } // end while ( 1 )

    //
    // exit the conference thread
    // 
	
    opbx_log( OPBX_CONF_DEBUG, "exit conference_exec\n" ) ;

    // exit the thread
    pthread_exit( NULL ) ;
    return ;
}

struct opbx_conf_member *find_member( struct opbx_conference *conf, const char* name ) 
{
    // check for member
    if ( name == NULL )
	return NULL ;

    // check for conference
    if ( conf == NULL )
    	return NULL ;
    //
    // loop through the member list looking for the requested member
    //
    struct opbx_conf_member *member_list = conf->memberlist ;

    while ( member_list != NULL ) 
    {
    	if ( !strcmp(member_list->chan->name,name) ) 
	    return member_list;
	member_list = member_list->next ;
    }
	
    // return -1 on error, or the number of members 
    // remaining if the requested member was deleted
    return NULL ;
}

struct opbx_conference *find_conf( const char* name ) 
{	
    struct opbx_conference *conf = conflist ;
	
    // no conferences exist
    if ( conflist == NULL ) 
    {
    	opbx_log( OPBX_CONF_DEBUG, "conflist has not yet been initialized, name => %s\n", name ) ;
    	return NULL ;
    }
	
    // acquire mutex
    opbx_mutex_lock( &conflist_lock ) ;

    // loop through conf list
    while ( conf != NULL ) 
    {
	if ( strncasecmp( (char*)&(conf->name), name, sizeof(conf->name) ) == 0 )
	{
	    // found conf name match 
	    opbx_log( OPBX_CONF_DEBUG, "found conference in conflist, name => %s\n", name ) ;
	    break ;
	}
	conf = conf->next ;
    }

    // release mutex
    opbx_mutex_unlock( &conflist_lock ) ;

    if ( conf == NULL )
    {
	opbx_log( OPBX_CONF_DEBUG, "unable to find conference in conflist, name => %s\n", name ) ;
    }

    return conf ;

}

struct opbx_conference* create_conf( char* name, struct opbx_conf_member* member )
{
    opbx_log( OPBX_CONF_DEBUG, "entered create_conf, name => %s\n", name ) ;	


    // allocate memory for conference
    struct opbx_conference *conf = calloc(1, sizeof( struct opbx_conference ) ) ;
	
    if ( conf == NULL ) 
    {
    	opbx_log( LOG_ERROR, "unable to malloc opbx_conference\n" ) ;
    	return NULL ;
    }

    //
    // initialize conference
    //
	
    conf->next = NULL ;
    conf->memberlist = NULL ;
    conf->membercount = 0 ;
    conf->conference_thread = OPBX_PTHREADT_NULL ;
    conf->is_locked = 0;
    conf->command_queue = NULL ;

    // copy name to conference
    strncpy( (char*)&(conf->name), name, sizeof(conf->name) - 1 ) ;
    // initialize mutexes
    opbx_mutex_init( &conf->lock ) ;
	
    // add the initial member
    add_member( conf, member) ;
	
    //
    // prepend new conference to conflist
    //

    // acquire mutex
    opbx_mutex_lock( &conflist_lock ) ;

    conf->next = conflist ;
    conflist = conf ;
    opbx_log( OPBX_CONF_DEBUG, "added new conference to conflist, name => %s\n", name ) ;

    //
    // spawn thread for new conference, using conference_exec( conf )
    //
    // acquire conference mutexes
    opbx_mutex_lock( &conf->lock ) ;
	
    if ( opbx_pthread_create( &conf->conference_thread, NULL, (void*)conference_exec, conf ) == 0 ) 
    {
	// detach the thread so it doesn't leak
	pthread_detach( conf->conference_thread ) ;
	// release conference mutexes
	opbx_mutex_unlock( &conf->lock ) ;
	opbx_log( OPBX_CONF_DEBUG, "started conference thread for conference, name => %s\n", conf->name ) ;
	manager_event(
	    EVENT_FLAG_CALL, 
	    APP_CONFERENCE_MANID"ConfCreate", 
	    "Channel: %s\r\n"
	    "ConfNo: %s\r\n",
	    member->chan->name,
	    name
	) ;
    }
    else
    {
    	opbx_log( LOG_ERROR, "unable to start conference thread for conference %s\n", conf->name ) ;
	conf->conference_thread = OPBX_PTHREADT_NULL ;

	// release conference mutexes
	opbx_mutex_unlock( &conf->lock ) ;

	// clean up conference
	free( conf ) ;
	conf = NULL ;
    }

    // count new conference 
    if ( conf != NULL )
    	++conference_count ;

    conf->auto_destroy = 1;

    // release mutex
    opbx_mutex_unlock( &conflist_lock ) ;

    return conf ;
}


void remove_conf( struct opbx_conference *conf )
{
    struct opbx_conference *conf_current = conflist ;
    struct opbx_conference *conf_temp = NULL ;

    opbx_log( OPBX_CONF_DEBUG, "attempting to remove conference, name => %s\n", conf->name ) ;

    // acquire mutex
    opbx_mutex_lock( &start_stop_conf_lock ) ;

    // acquire mutex
    opbx_mutex_lock( &conflist_lock ) ;

    // loop through list of conferences
    while ( conf_current != NULL ) 
    {
	// if conf_current point to the passed conf,
	if ( conf_current == conf ) 
	{
	    if ( conf_temp == NULL ) 
	    {
		// this is the first conf in the list, so we just point 
		// conflist past the current conf to the next
		conflist = conf_current->next ;
	    }
	    else 
	    {
		// this is not the first conf in the list, so we need to
		// point the preceeding conf to the next conf in the list
		conf_temp->next = conf_current->next ;
	    }

	    // calculate time in conference
	    opbx_log( OPBX_CONF_DEBUG, "removed conference, name => %s\n", conf_current->name ) ;
	    manager_event(
		EVENT_FLAG_CALL, 
		APP_CONFERENCE_MANID"ConfRemove",
		"ConfNo: %s\r\n",
		conf_current->name
	    ) ;
	    opbx_mutex_unlock( &conf_current->lock ) ;

	    //Free all the command queue
	    struct opbx_conf_command_queue *cqd, *cq = conf_current->command_queue;
	    while ( cq != NULL) {
		cqd=cq;
		cq=cq->next;
		free(cqd);
	    }


	    free( conf_current ) ;
	    conf_current = NULL ;
			
	    break ;
	}

	// save a refence to the soon to be previous conf
	conf_temp = conf_current ;
		
	// move conf_current to the next in the list
	conf_current = conf_current->next ;
    }
	
    // count new conference 
    --conference_count ;

    // release mutex
    opbx_mutex_unlock( &conflist_lock ) ;
	
    // release mutex
    opbx_mutex_unlock( &start_stop_conf_lock ) ;

    return ;
}


struct opbx_conference* start_conference( struct opbx_conf_member* member ) 
{
    struct opbx_conference* conf = NULL ;

    // check input
    if ( member == NULL )
    {
    	opbx_log( LOG_WARNING, "unable to handle null member\n" ) ;
    	return NULL ;
    }

    // look for an existing conference
    opbx_log( OPBX_CONF_DEBUG, "attempting to find requested conference\n" ) ;

    // acquire mutex
    opbx_mutex_lock( &start_stop_conf_lock ) ;

    conf = find_conf( member->id ) ;
	
    // unable to find an existing conference, try to create one
    if ( conf == NULL )
    {
	// create a new conference
	opbx_log( OPBX_CONF_DEBUG, "attempting to create requested conference\n" ) ;
	
	// create the new conference with one member
	conf = create_conf( member->id, member ) ;

	// return an error if create_conf() failed
	if ( conf == NULL ) 
	{
	    opbx_log( LOG_ERROR, "unable to find or create requested conference\n" ) ;
	    opbx_mutex_unlock( &start_stop_conf_lock ) ;
	    return NULL ;
	}
    }
    else
    {
	// existing conference found, add new member to the conference
	// once we call add_member(), this thread
	// is responsible for calling delete_member()
	//
	add_member( conf, member) ;
    }	

    // release mutex
    opbx_mutex_unlock( &start_stop_conf_lock ) ;
    
    return conf ;
}

/* ********************************************************************************************** 
     admin mode-related functions
   *********************************************************************************************/

struct fopbx_originate_helper {
	char tech[256];
	char data[256];
	int timeout;
	char app[256];
	char appdata[256];
	char cid_name[256];
	char cid_num[256];
	char context[256];
	char exten[256];
	char idtext[256];
	int priority;
	struct opbx_variable *vars;
	struct opbx_conf_member *frommember;
};

static void *fopbx_originate(void *data)
{
    int res = 0,
	reason;
    struct fopbx_originate_helper *in = data;
    struct opbx_channel *chan = NULL;

    opbx_indicate(in->frommember->chan, OPBX_CONTROL_RINGING);


    if (1) {
		res = opbx_pbx_outgoing_app(
			in->tech, OPBX_FORMAT_SLINEAR, 
			in->data, in->timeout, 
			in->app, in->appdata, 
			&reason, 1, 
			!opbx_strlen_zero(in->cid_num) ? in->cid_num : NULL, 
			!opbx_strlen_zero(in->cid_name) ? in->cid_name : NULL,
			in->vars, &chan );
    } else {
		res = opbx_pbx_outgoing_exten(
			in->tech, OPBX_FORMAT_SLINEAR, 
			in->data, in->timeout, 
			in->context, in->exten, in->priority, 
			&reason, 1, 
			!opbx_strlen_zero(in->cid_num) ? in->cid_num : NULL, 
			!opbx_strlen_zero(in->cid_name) ? in->cid_name : NULL,
			in->vars, NULL );
    }   

    opbx_log(OPBX_CONF_DEBUG,"Originate returned %d \n",reason);
    opbx_indicate(in->frommember->chan, -1);

    if ( reason == OPBX_CONTROL_ANSWER ) {
	conference_queue_sound( in->frommember, "beep" );
    } 
    else {
	conference_queue_sound( in->frommember, "beeperr" );
    }

    if (chan)
	opbx_mutex_unlock(&chan->lock);
    free(in);
    return NULL;
}


int conf_do_originate(struct opbx_conf_member *member, char *ext) {
    int res;

    pthread_t th;
    pthread_attr_t attr;
    struct fopbx_originate_helper *fast = malloc(sizeof(struct fopbx_originate_helper));

    char dst[80]="";
    char appdata[80]="";
    char *var;
    
    if (!fast) {
	res = -1;
    } else {
	memset(fast, 0, sizeof(struct fopbx_originate_helper));

	if (  (var = pbx_builtin_getvar_helper(member->chan, "NCONF_OUTBOUND_TIMEOUT")) ) {
	    fast->timeout = atoi(var) * 1000;
	} else
	    fast->timeout = 30000;

	strcat(dst,ext);
	strcat(dst,"@");
	if ( (var = pbx_builtin_getvar_helper(member->chan, "NCONF_OUTBOUND_CONTEXT")) )
	    strcat(dst,var);
	else
	    strcat(dst,member->chan->context);


	strcat(appdata,member->id);
	strcat(appdata,"/");
	if ( (var = pbx_builtin_getvar_helper(member->chan, "NCONF_OUTBOUND_PARAMS")) )
	    strcat(appdata,var);
	else {
	    strcat(appdata,"Sdq");
#if ENABLE_VAD
	    strcat(appdata,"V");
#endif
	}

	opbx_copy_string( fast->tech, 	 "Local", sizeof(fast->tech) );
	opbx_copy_string( fast->data, 	 dst, sizeof(fast->data) );
	opbx_copy_string( fast->app, 	 APP_CONFERENCE_NAME, sizeof(fast->app) );
	opbx_copy_string( fast->appdata, appdata, sizeof(fast->appdata) );

	if ( (var = pbx_builtin_getvar_helper(member->chan, "NCONF_OUTBOUND_CID_NAME")) )
	    opbx_copy_string( fast->cid_name, var, sizeof(fast->cid_name) );
	else
	    opbx_copy_string( fast->cid_name,"NavyConference",sizeof(fast->cid_name) );

	if ( (var = pbx_builtin_getvar_helper(member->chan, "NCONF_OUTBOUND_CID_NUM")) )
	    opbx_copy_string( fast->cid_num, var,sizeof(fast->cid_num) );
	else
	    opbx_copy_string( fast->cid_num, member->id,sizeof(fast->cid_num) );

	opbx_copy_string( fast->context, "internal", sizeof(fast->context) );
	opbx_copy_string( fast->exten, ext, sizeof(fast->exten) );
	fast->priority = 1;
	fast->vars=NULL;
/**/

	fast->frommember=member;

        pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (opbx_pthread_create(&th, &attr, fopbx_originate, fast)) {
    	    free(fast);
	    res = -1;
	} else {
	    res = 0;
	}

    }

    opbx_mutex_unlock(&member->chan->lock);
    opbx_conf_member_genactivate( member ) ;

    return res;
}


// **********************************************************************************

int conference_set_pin(struct opbx_conf_member *member, char *pin) {

    opbx_copy_string(member->conf->pin, pin, sizeof(member->conf->pin));
    opbx_log( OPBX_CONF_DEBUG, "Conference %s: PIN Set to %s\n",member->conf->name, member->conf->pin ) ;
    conference_queue_number( member, member->conf->pin );

    manager_event(
	EVENT_FLAG_CALL, 
	APP_CONFERENCE_MANID"SetPIN",
	"Channel: %s\r\n"
	"PIN: %s\r\n",
	member->chan->name, 
	member->conf->pin
    ) ;

    return 1;
}


// **********************************************************************************


int conference_parse_admin_command(struct opbx_conf_member *member) {
    char action;
    char *parameters;
    int res = 0;

    action=member->dtmf_buffer[0];
    parameters=member->dtmf_buffer+1;

    switch (action) {
	case '1':
	    queue_incoming_silent_frame( member, 2 );
	    res = conf_do_originate(member,parameters);
	    break;
	case '4':
	    if      ( parameters[0] == '0' )
	        add_command_to_queue( member->conf, member, CONF_ACTION_ENABLE_SOUNDS , 0, "" );
	    else if ( parameters[0] == '1' )
	        add_command_to_queue( member->conf, member, CONF_ACTION_ENABLE_SOUNDS , 1, "" );
	    else
		conference_queue_sound( member, "beeperr" );
	    break;
	case '5':
	    if      ( parameters[0] == '0' )
	        add_command_to_queue( member->conf, member, CONF_ACTION_MUTE_ALL , 0, "" );
	    else if ( parameters[0] == '1' )
	        add_command_to_queue( member->conf, member, CONF_ACTION_MUTE_ALL , 1, "" );
	    else
		conference_queue_sound( member, "beeperr" );
	    break;
	case '6':
	    if      ( parameters[0] == '0' )
	        add_command_to_queue( member->conf, member, CONF_ACTION_PLAYMOH , 0, "" );
	    else if ( parameters[0] == '1' )
	        add_command_to_queue( member->conf, member, CONF_ACTION_PLAYMOH , 1, "" );
	    else
		conference_queue_sound( member, "beeperr" );
	    break;
	case '7':
	    if      ( parameters[0] == '0' ) {
		member->conf->is_locked = 0;
		conference_queue_sound( member, "conf-unlockednow" );
	    }
	    else if ( parameters[0] == '1' ) {
		member->conf->is_locked = 1;
		conference_queue_sound( member, "conf-lockednow" );
	    }
	    else {
		conference_queue_sound( member, "beep" );
	    }
	    break;
	case '9':
	    res = conference_set_pin(member,parameters);
	    conference_queue_sound( member, "beep" );
	    break;
	case '0':
	    if      ( parameters[0] == '0' )
	        add_command_to_queue( member->conf, member, CONF_ACTION_HANGUP , 0, "" );
	    else if ( parameters[0] == '1' )
	        add_command_to_queue( member->conf, member, CONF_ACTION_HANGUP , 1, "" );
	    else if ( parameters[0] == '2' )
	        add_command_to_queue( member->conf, member, CONF_ACTION_HANGUP , 2, "" );
	    else
		conference_queue_sound( member, "beeperr" );
	    break;
	default:
	    opbx_log(OPBX_CONF_DEBUG,"Admin mode: Action: %c parameters: %s. Invalid or unknown\n",
		action, parameters);
	    break;
    }

    return 1;
}
