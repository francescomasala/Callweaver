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


struct opbx_conf_soundq 
{
	char name[256];
//	struct opbx_filestream *stream; // the stream
	struct opbx_conf_soundq *next;
};


int conf_play_soundqueue( struct opbx_conf_member *member );
int conference_queue_sound( struct opbx_conf_member *member, char *soundfile );
int conference_queue_number( struct opbx_conf_member *member, char *str );
int conference_stop_sounds( struct opbx_conf_member *member );
