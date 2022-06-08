/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Trivial application to send a TIFF file as a FAX
 * 
 * Copyright (C) 2003, 2005, Steve Underwood
 *
 * Steve Underwood <steveu@coppice.org>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>

#include <spandsp.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/apps/app_txfax.c $", "$Revision: 4723 $")

#include "callweaver/lock.h"
#include "callweaver/file.h"
#include "callweaver/logger.h"
#include "callweaver/channel.h"
#include "callweaver/pbx.h"
#include "callweaver/module.h"
#include "callweaver/translate.h"
#include "callweaver/manager.h"

static char *tdesc = "Trivial FAX Transmit Application";

static void *txfax_app;
static const char *txfax_name = "TxFAX";
static const char *txfax_synopsis = "Send a FAX file";
static const char *txfax_syntax = "TxFAX(filename[, caller][, debug][, ecm])";
static const char *txfax_descrip = 
"Send a given TIFF file to the channel as a FAX.\n"
"The \"caller\" option makes the application behave as a calling machine,\n"
"rather than the answering machine. The default behaviour is to behave as\n"
"an answering machine.\n"
"The \"ecm\" option enables ECM.\n"
"Uses LOCALSTATIONID to identify itself to the remote end.\n"
"     LOCALHEADERINFO to generate a header line on each page.\n"
"Sets REMOTESTATIONID to the receiver CSID.\n"
"     FAXPAGES to the number of pages received.\n"
"     FAXBITRATE to the transmition rate.\n"
"     FAXRESOLUTION to the resolution.\n"
"     PHASEESTATUS to the phase E result status.\n"
"     PHASEESTRING to the phase E result string.\n"
"Returns -1 when the user hangs up, or if the file does not exist.\n"
"Returns 0 otherwise.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

#define MAX_BLOCK_SIZE 240
#define ready_to_talk(chan) ( (!chan ||  cw_check_hangup(chan) )  ?  0  :  1)

static void span_message(int level, const char *msg)
{
    int cw_level;
    
    if (level == SPAN_LOG_ERROR)
        cw_level = __LOG_ERROR;
    else if (level == SPAN_LOG_WARNING)
        cw_level = __LOG_WARNING;
    else
        cw_level = __LOG_DEBUG;
    //cw_level = __LOG_WARNING;
    cw_log(cw_level, __FILE__, __LINE__, __PRETTY_FUNCTION__, msg);
}
/*- End of function --------------------------------------------------------*/

/* Return a monotonically increasing time, in microseconds */
static uint64_t nowis(void)
{
    int64_t now;
#ifndef HAVE_POSIX_TIMERS
    struct timeval tv;

    gettimeofday(&tv, NULL);
    now = tv.tv_sec*1000000LL + tv.tv_usec;
#else
    struct timespec ts;
    
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        cw_log(LOG_WARNING, "clock_gettime returned %s\n", strerror(errno));
    now = ts.tv_sec*1000000LL + ts.tv_nsec/1000;
#endif
    return now;
}

/*- End of function --------------------------------------------------------*/

/* *****************************************************************************
	MEMBER GENERATOR
   ****************************************************************************/

static void *faxgen_alloc(struct cw_channel *chan, void *params)
{
    cw_log(LOG_DEBUG,"Allocating fax generator\n");
    return params;
}

/*- End of function --------------------------------------------------------*/

static void faxgen_release(struct cw_channel *chan, void *data)
{
    cw_log(LOG_DEBUG,"Releasing fax generator\n");
    return;
}

/*- End of function --------------------------------------------------------*/

static int faxgen_generate(struct cw_channel *chan, void *data, int samples)
{
    int len;
    fax_state_t *fax;
    struct cw_frame outf;

    uint8_t __buf[sizeof(uint16_t)*MAX_BLOCK_SIZE + 2*CW_FRIENDLY_OFFSET];
    uint8_t *buf = __buf + CW_FRIENDLY_OFFSET;
    
    fax = (fax_state_t*) data;

    samples = ( samples <= MAX_BLOCK_SIZE )  ?  samples  :  MAX_BLOCK_SIZE;
    len = fax_tx(fax, (int16_t *) &buf[CW_FRIENDLY_OFFSET], samples);

    if (len) {
        cw_fr_init_ex(&outf, CW_FRAME_VOICE, CW_FORMAT_SLINEAR, "TxFAX");
        outf.datalen = len*sizeof(int16_t);
        outf.samples = len;
        outf.data = &buf[CW_FRIENDLY_OFFSET];
        outf.offset = CW_FRIENDLY_OFFSET;

        if (cw_write(chan, &outf) < 0) {
            cw_log(LOG_WARNING, "Unable to write frame to channel; %s\n", strerror(errno));
        }
    }

    return 0;
}

struct cw_generator faxgen = 
{
	alloc: 		faxgen_alloc,
	release: 	faxgen_release,
	generate: 	faxgen_generate,
};

/*- End of function --------------------------------------------------------*/

static void phase_e_handler(t30_state_t *s, void *user_data, int result)
{
    struct cw_channel *chan;
    char buf[128];
    t30_stats_t t;
    const char *tx_ident;
    const char *rx_ident;

    chan = (struct cw_channel *) user_data;
    t30_get_transfer_statistics(s, &t);
    
    tx_ident = t30_get_tx_ident(s);
    if (tx_ident == NULL)
        tx_ident = "";
    rx_ident = t30_get_rx_ident(s);
    if (rx_ident == NULL)
        rx_ident = "";
    pbx_builtin_setvar_helper(chan, "REMOTESTATIONID", rx_ident);
    snprintf(buf, sizeof(buf), "%d", t.pages_transferred);
    pbx_builtin_setvar_helper(chan, "FAXPAGES", buf);
    snprintf(buf, sizeof(buf), "%d", t.y_resolution);
    pbx_builtin_setvar_helper(chan, "FAXRESOLUTION", buf);
    snprintf(buf, sizeof(buf), "%d", t.bit_rate);
    pbx_builtin_setvar_helper(chan, "FAXBITRATE", buf);
    snprintf(buf, sizeof(buf), "%d", result);
    pbx_builtin_setvar_helper(chan, "PHASEESTATUS", buf);
    snprintf(buf, sizeof(buf), "%s", t30_completion_code_to_str(result));
    pbx_builtin_setvar_helper(chan, "PHASEESTRING", buf);


    cw_log(LOG_DEBUG, "==============================================================================\n");
    if (result == T30_ERR_OK) {
        cw_log(LOG_DEBUG, "Fax successfully sent.\n");
        cw_log(LOG_DEBUG, "Remote station id: %s\n", rx_ident);
        cw_log(LOG_DEBUG, "Local station id:  %s\n", tx_ident);
        cw_log(LOG_DEBUG, "Pages transferred: %i\n", t.pages_transferred);
        cw_log(LOG_DEBUG, "Image resolution:  %i x %i\n", t.x_resolution, t.y_resolution);
        cw_log(LOG_DEBUG, "Transfer Rate:     %i\n", t.bit_rate);
        manager_event(EVENT_FLAG_CALL,
                      "FaxSent", "Channel: %s\nExten: %s\nCallerID: %s\nRemoteStationID: %s\nLocalStationID: %s\nPagesTransferred: %i\nResolution: %i\nTransferRate: %i\nFileName: %s\n",
                      chan->name,
                      chan->exten,
                      (chan->cid.cid_num)  ?  chan->cid.cid_num  :  "",
                      rx_ident,
                      tx_ident,
                      t.pages_transferred,
                      t.y_resolution,
                      t.bit_rate,
                      s->rx_file);
    }
    else
        cw_log(LOG_DEBUG, "Fax send not successful - result (%d) %s.\n", result, t30_completion_code_to_str(result));
    cw_log(LOG_DEBUG, "==============================================================================\n");
}
/*- End of function --------------------------------------------------------*/

static int t38_tx_packet_handler(t38_core_state_t *s, void *user_data, const uint8_t *buf, int len, int count)
{
    struct cw_frame outf;
    struct cw_channel *chan;

    chan = (struct cw_channel *) user_data;

    cw_fr_init_ex(&outf, CW_FRAME_MODEM, CW_MODEM_T38, "TxFAX");
    outf.datalen = len;
    outf.data = (char *) buf;
    outf.tx_copies = count;
    if (cw_write(chan, &outf) < 0)
        cw_log(LOG_WARNING, "Unable to write frame to channel; %s\n", strerror(errno));
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int txfax_t38(struct cw_channel *chan, t38_terminal_state_t *t38, char *source_file, int calling_party,int verbose, int ecm) {
    char 		*x;
    struct cw_frame 	*inf = NULL;
    int 		ready = 1,
			res = 0;
    uint64_t 		now;
    uint64_t 		passage;

    memset(t38, 0, sizeof(*t38));

    if (t38_terminal_init(t38, calling_party, t38_tx_packet_handler, chan) == NULL)
    {
        cw_log(LOG_WARNING, "Unable to start T.38 termination.\n");
        return -1;
    }

    span_log_set_message_handler(&t38->logging, span_message);
    span_log_set_message_handler(&t38->t30_state.logging, span_message);
    span_log_set_message_handler(&t38->t38.logging, span_message);

    if (verbose)
    {
        span_log_set_level(&t38->logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_FLOW);
        span_log_set_level(&t38->t30_state.logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_FLOW);
        span_log_set_level(&t38->t38.logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_FLOW);
    }

    x = pbx_builtin_getvar_helper(chan, "LOCALSTATIONID");
    if (x  &&  x[0])
        t30_set_tx_ident(&t38->t30_state, x);
    x = pbx_builtin_getvar_helper(chan, "LOCALSUBADDRESS");
    if (x  &&  x[0])
        t30_set_tx_sub_address(&t38->t30_state, x);
    x = pbx_builtin_getvar_helper(chan, "LOCALHEADERINFO");
    if (x  &&  x[0])
        t30_set_tx_page_header_info(&t38->t30_state, x);
    t30_set_tx_file(&t38->t30_state, source_file, -1, -1);

    //t30_set_phase_b_handler(&t38.t30_state, phase_b_handler, chan);
    //t30_set_phase_d_handler(&t38.t30_state, phase_d_handler, chan);
    t30_set_phase_e_handler(&t38->t30_state, phase_e_handler, chan);

    x = pbx_builtin_getvar_helper(chan, "FAX_DISABLE_V17");
    if (x  &&  x[0])
        t30_set_supported_modems(&t38->t30_state, T30_SUPPORT_V29 | T30_SUPPORT_V27TER);
    else
        t30_set_supported_modems(&t38->t30_state, T30_SUPPORT_V17 | T30_SUPPORT_V29 | T30_SUPPORT_V27TER);

    t30_set_supported_image_sizes(&t38->t30_state, T30_SUPPORT_US_LETTER_LENGTH | T30_SUPPORT_US_LEGAL_LENGTH | T30_SUPPORT_UNLIMITED_LENGTH
                                	        | T30_SUPPORT_215MM_WIDTH | T30_SUPPORT_255MM_WIDTH | T30_SUPPORT_303MM_WIDTH);
    t30_set_supported_resolutions(&t38->t30_state, T30_SUPPORT_STANDARD_RESOLUTION | T30_SUPPORT_FINE_RESOLUTION | T30_SUPPORT_SUPERFINE_RESOLUTION
                                                | T30_SUPPORT_R8_RESOLUTION | T30_SUPPORT_R16_RESOLUTION);

    if (ecm) {
        t30_set_ecm_capability(&t38->t30_state, TRUE);
        t30_set_supported_compressions(&t38->t30_state, T30_SUPPORT_T4_1D_COMPRESSION | T30_SUPPORT_T4_2D_COMPRESSION | T30_SUPPORT_T6_COMPRESSION);
        cw_log(LOG_DEBUG, "Enabling ECM mode for app_txfax\n"  );
    } 
    else 
    {
        t30_set_supported_compressions(&t38->t30_state, T30_SUPPORT_T4_1D_COMPRESSION | T30_SUPPORT_T4_2D_COMPRESSION );
    }

    passage = nowis();

    t38_terminal_set_tep_mode(t38, TRUE);
    while (ready  &&  ready_to_talk(chan))
    {
    
	if ( chan->t38_status != T38_NEGOTIATED )
	    break;

        if ((res = cw_waitfor(chan, 20)) < 0) {
	    ready = 0;
            break;
	}

        now = nowis();
        t38_terminal_send_timeout(t38, (now - passage)/125);
        passage = now;
        /* End application when T38/T30 has finished */
        if ((t38->current_rx_type == T30_MODEM_DONE)  ||  (t38->current_tx_type == T30_MODEM_DONE)) 
            break;

        inf = cw_read(chan);
        if (inf == NULL) {
	    ready = 0;
            break;
        }

        if (inf->frametype == CW_FRAME_MODEM  &&  inf->subclass == CW_MODEM_T38)
    	    t38_core_rx_ifp_packet(&t38->t38, inf->data, inf->datalen, inf->seq_no);

        cw_fr_free(inf);
    }

    return ready;

}
/*- End of function --------------------------------------------------------*/

static int txfax_audio(struct cw_channel *chan, fax_state_t *fax, char *source_file, int calling_party,int verbose, int ecm) {
    char 		*x;
    struct cw_frame 	*inf = NULL;
    struct cw_frame 	outf;
    int 		ready = 1,
			samples = 0,
			res = 0,
			len = 0,
			generator_mode = 0;
    uint64_t		begin = 0,
			received_frames = 0;

    uint8_t __buf[sizeof(uint16_t)*MAX_BLOCK_SIZE + 2*CW_FRIENDLY_OFFSET];
    uint8_t *buf = __buf + CW_FRIENDLY_OFFSET;

    memset(fax, 0, sizeof(*fax));

    if (fax_init(fax, calling_party) == NULL)
    {
        cw_log(LOG_WARNING, "Unable to start FAX\n");
        return -1;
    }
    fax_set_transmit_on_idle(fax, TRUE);
    span_log_set_message_handler(&fax->logging, span_message);
    span_log_set_message_handler(&fax->t30_state.logging, span_message);
    if (verbose)
    {
        span_log_set_level(&fax->logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_FLOW);
        span_log_set_level(&fax->t30_state.logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_FLOW);
    }
    x = pbx_builtin_getvar_helper(chan, "LOCALSTATIONID");
    if (x  &&  x[0])
        t30_set_tx_ident(&fax->t30_state, x);
    x = pbx_builtin_getvar_helper(chan, "LOCALSUBADDRESS");
    if (x  &&  x[0])
        t30_set_tx_sub_address(&fax->t30_state, x);
    x = pbx_builtin_getvar_helper(chan, "LOCALHEADERINFO");
    if (x  &&  x[0])
        t30_set_tx_page_header_info(&fax->t30_state, x);
    t30_set_tx_file(&fax->t30_state, source_file, -1, -1);
    //t30_set_phase_b_handler(&fax.t30_state, phase_b_handler, chan);
    //t30_set_phase_d_handler(&fax.t30_state, phase_d_handler, chan);
    t30_set_phase_e_handler(&fax->t30_state, phase_e_handler, chan);

    x = pbx_builtin_getvar_helper(chan, "FAX_DISABLE_V17");
    if (x  &&  x[0])
        t30_set_supported_modems(&fax->t30_state, T30_SUPPORT_V29 | T30_SUPPORT_V27TER);

    /* Support for different image sizes && resolutions*/
    t30_set_supported_image_sizes(&fax->t30_state, T30_SUPPORT_US_LETTER_LENGTH | T30_SUPPORT_US_LEGAL_LENGTH | T30_SUPPORT_UNLIMITED_LENGTH
                                                | T30_SUPPORT_215MM_WIDTH | T30_SUPPORT_255MM_WIDTH | T30_SUPPORT_303MM_WIDTH);
    t30_set_supported_resolutions(&fax->t30_state, T30_SUPPORT_STANDARD_RESOLUTION | T30_SUPPORT_FINE_RESOLUTION | T30_SUPPORT_SUPERFINE_RESOLUTION
                                                | T30_SUPPORT_R8_RESOLUTION | T30_SUPPORT_R16_RESOLUTION);
    if (ecm) {
        t30_set_ecm_capability(&fax->t30_state, TRUE);
        t30_set_supported_compressions(&fax->t30_state, T30_SUPPORT_T4_1D_COMPRESSION | T30_SUPPORT_T4_2D_COMPRESSION | T30_SUPPORT_T6_COMPRESSION);
        cw_log(LOG_DEBUG, "Enabling ECM mode for app_txfax\n"  );
    }

    /* This is the main loop */

    begin = nowis();

    while ( ready && ready_to_talk(chan) )
    {
    
	if ( chan->t38_status == T38_NEGOTIATED )
	    break;

        if ((res = cw_waitfor(chan, 20)) < 0) {
	    ready = 0;
            break;
	}

        if ((fax->current_rx_type == T30_MODEM_DONE)  ||  (fax->current_tx_type == T30_MODEM_DONE))
            break;

        inf = cw_read(chan);
        if (inf == NULL) {
	    ready = 0;
            break;
        }

	/* We got a frame */
        if (inf->frametype == CW_FRAME_VOICE) {

	    received_frames ++;

            if (fax_rx(fax, inf->data, inf->samples))
                    break;

            samples = (inf->samples <= MAX_BLOCK_SIZE)  ?  inf->samples  :  MAX_BLOCK_SIZE;
            if ((len = fax_tx(fax, (int16_t *) &buf[CW_FRIENDLY_OFFSET], samples)) > 0) {
                cw_fr_init_ex(&outf, CW_FRAME_VOICE, CW_FORMAT_SLINEAR, "TxFAX");
                outf.datalen = len*sizeof(int16_t);
                outf.samples = len;
                outf.data = &buf[CW_FRIENDLY_OFFSET];
                outf.offset = CW_FRIENDLY_OFFSET;

                if (cw_write(chan, &outf) < 0) {
                    cw_log(LOG_WARNING, "Unable to write frame to channel; %s\n", strerror(errno));
                    break;
                }
            }
	    else
	    {
	    	len = samples;
    		cw_fr_init_ex(&outf, CW_FRAME_VOICE, CW_FORMAT_SLINEAR, "TxFAX");
    		outf.datalen = len*sizeof(int16_t);
    		outf.samples = len;
    		outf.data = &buf[CW_FRIENDLY_OFFSET];
    		outf.offset = CW_FRIENDLY_OFFSET;
    		memset(&buf[CW_FRIENDLY_OFFSET], 0, outf.datalen);
    		if (cw_write(chan, &outf) < 0)
    		{
        	    cw_log(LOG_WARNING, "Unable to write frame to channel; %s\n", strerror(errno));
		    break;
    		}
	    }
        }
	else {
	    if ( (nowis() - begin) > 1000000 ) {
		if (received_frames < 20 ) { // just to be sure we have had no frames ...
		    cw_log(LOG_WARNING,"Switching to generator mode\n");
		    generator_mode = 1;
		    break;
		}
	    }
	}
        cw_fr_free(inf);
        inf = NULL;
    }

    if (inf) {
        cw_fr_free(inf);
        inf = NULL;
    }

    if (generator_mode) {
	// This is activated when we don't receive any frame for
	// X seconds (see above)... we are probably on ZAP or talking without UDPTL to
	// another callweaver box
	cw_generator_activate(chan, &faxgen, fax);

	while ( ready && ready_to_talk(chan) ) {

	    if ( chan->t38_status == T38_NEGOTIATED )
		break;

	    if ((res = cw_waitfor(chan, 20)) < 0) {
	        ready = 0;
        	break;
	    }

    	    if ((fax->current_rx_type == T30_MODEM_DONE)  ||  (fax->current_tx_type == T30_MODEM_DONE))
        	break;

    	    inf = cw_read(chan);
    	    if (inf == NULL) {
		ready = 0;
        	break;
    	    }

	    /* We got a frame */
    	    if (inf->frametype == CW_FRAME_VOICE) {
        	if (fax_rx(fax, inf->data, inf->samples)) {
		    ready = 0;
                    break;
		}
	    }

    	    cw_fr_free(inf);
	    inf = NULL;
	}

	if (inf) {
    	    cw_fr_free(inf);
	    inf = NULL;
	}
	cw_generator_deactivate(chan);

    }

    return ready;
}
/*- End of function --------------------------------------------------------*/

static int txfax_exec(struct cw_channel *chan, int argc, char **argv)
{
    fax_state_t 	fax;
    t38_terminal_state_t t38;

    char *source_file;
    int res = 0;
    int ready;

    int calling_party;
    int verbose;
    int ecm = FALSE;
    
    struct localuser *u;

    int original_read_fmt;
    int original_write_fmt;

    /* Basic initial checkings */

    if (chan == NULL)
    {
        cw_log(LOG_WARNING, "Fax transmit channel is NULL. Giving up.\n");
        return -1;
    }

    /* Make sure they are initialized to zero */
    memset(&fax, 0, sizeof(fax));
    memset(&t38, 0, sizeof(t38));

    if (argc < 1)
    {
        cw_log(LOG_ERROR, "Syntax: %s\n", txfax_syntax);
        return -1;
    }

    /* Resetting channel variables related to T38 */
    
    pbx_builtin_setvar_helper(chan, "REMOTESTATIONID", "");
    pbx_builtin_setvar_helper(chan, "FAXPAGES", "");
    pbx_builtin_setvar_helper(chan, "FAXRESOLUTION", "");
    pbx_builtin_setvar_helper(chan, "FAXBITRATE", "");
    pbx_builtin_setvar_helper(chan, "PHASEESTATUS", "");
    pbx_builtin_setvar_helper(chan, "PHASEESTRING", "");

    /* Parsing parameters */
    
    calling_party = FALSE;
    verbose = FALSE;

    source_file = argv[0];

    while (argv++, --argc) {
        if (strcmp("caller", argv[0]) == 0)
        {
            calling_party = TRUE;
        }
        else if (strcmp("debug", argv[0]) == 0)
        {
            verbose = TRUE;
        }
        else if (strcmp("ecm", argv[0]) == 0)
        {
            ecm = TRUE;
        }
        else if (strcmp("start", argv[0]) == 0)
        {
            /* TODO: handle this */
        }
        else if (strcmp("end", argv[0]) == 0)
        {
            /* TODO: handle this */
        }
    }
    /* Done parsing */

    LOCAL_USER_ADD(u);

    if (chan->_state != CW_STATE_UP)
    {
        /* Shouldn't need this, but checking to see if channel is already answered
         * Theoretically the PBX should already have answered before running the app */
        res = cw_answer(chan);
	if (!res)
	{
    	    cw_log(LOG_DEBUG, "Could not answer channel '%s'\n", chan->name);
	    //LOCAL_USER_REMOVE(u);
	    //return res;
	}
    }

    /* Setting read and write formats */
    
    original_read_fmt = chan->readformat;
    if (original_read_fmt != CW_FORMAT_SLINEAR)
    {
        res = cw_set_read_format(chan, CW_FORMAT_SLINEAR);
        if (res < 0)
        {
            cw_log(LOG_WARNING, "Unable to set to linear read mode, giving up\n");
            LOCAL_USER_REMOVE(u);
            return -1;
        }
    }

    original_write_fmt = chan->writeformat;
    if (original_write_fmt != CW_FORMAT_SLINEAR)
    {
        res = cw_set_write_format(chan, CW_FORMAT_SLINEAR);
        if (res < 0)
        {
            cw_log(LOG_WARNING, "Unable to set to linear write mode, giving up\n");
            res = cw_set_read_format(chan, original_read_fmt);
            if (res)
                cw_log(LOG_WARNING, "Unable to restore read format on '%s'\n", chan->name);
            LOCAL_USER_REMOVE(u);
            return -1;
        }
    }


    /* This is the main loop */

    ready = TRUE;        

    while ( ready && ready_to_talk(chan) )
    {


        if ( ready && chan->t38_status != T38_NEGOTIATED ) {
	    ready = txfax_audio( chan, &fax, source_file, calling_party, verbose, ecm);
	}

        if ( ready && chan->t38_status == T38_NEGOTIATED ) {
	    ready = txfax_t38  ( chan, &t38, source_file, calling_party, verbose, ecm);
	}

	if ( chan->t38_status != T38_NEGOTIATING )
	    ready = 0; // 1 loop is enough. This could be useful if we want to turn from udptl to RTP later.

    }

    if (chan->t38_status == T38_NEGOTIATED)
	t30_terminate(&t38.t30_state);
    else
	t30_terminate(&fax.t30_state);

    fax_release(&fax);
    t38_terminal_release(&t38);

    /* Restoring initial channel formats. */

    if (original_read_fmt != CW_FORMAT_SLINEAR)
    {
        if ((res = cw_set_read_format(chan, original_read_fmt)))
            cw_log(LOG_WARNING, "Unable to restore read format on '%s'\n", chan->name);
    }
    if (original_write_fmt != CW_FORMAT_SLINEAR)
    {
        if ((res = cw_set_write_format(chan, original_write_fmt)))
            cw_log(LOG_WARNING, "Unable to restore write format on '%s'\n", chan->name);
    }

    return ready;

}
/*- End of function --------------------------------------------------------*/

int unload_module(void)
{
    int res = 0;
    STANDARD_HANGUP_LOCALUSERS;
    res |= cw_unregister_application(txfax_app);
    return res;
}
/*- End of function --------------------------------------------------------*/

int load_module(void)
{
    txfax_app = cw_register_application(txfax_name, txfax_exec, txfax_synopsis, txfax_syntax, txfax_descrip);
    return 0;
}
/*- End of function --------------------------------------------------------*/

char *description(void)
{
    return tdesc;
}
/*- End of function --------------------------------------------------------*/

int usecount(void)
{
    int res;

    STANDARD_USECOUNT(res);
    return res;
}
/*- End of function --------------------------------------------------------*/
/*- End of file ------------------------------------------------------------*/
