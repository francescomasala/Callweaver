/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.callweaver.org for more information about
 * the CallWeaver project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Translate via the use of pseudo channels
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <spandsp.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/corelib/translate.c $", "$Revision: 4723 $")

#include "callweaver/lock.h"
#include "callweaver/channel.h"
#include "callweaver/logger.h"
#include "callweaver/translate.h"
#include "callweaver/options.h"
#include "callweaver/frame.h"
#include "callweaver/sched.h"
#include "callweaver/cli.h"
#include "callweaver/term.h"

#define MAX_RECALC 200 /* max sample recalc */

/* This could all be done more efficiently *IF* we chained packets together
   by default, but it would also complicate virtually every application. */
   
CW_MUTEX_DEFINE_STATIC(list_lock);
static struct cw_translator *list = NULL;

struct cw_translator_dir
{
    struct cw_translator *step;    /* Next step translator */
    int cost;            /* Complete cost to destination */
};

struct cw_frame_delivery
{
    struct cw_frame *f;
    struct cw_channel *chan;
    int fd;
    struct translator_pvt *owner;
    struct cw_frame_delivery *prev;
    struct cw_frame_delivery *next;
};

static struct cw_translator_dir tr_matrix[MAX_FORMAT][MAX_FORMAT];

struct cw_trans_pvt
{
    struct cw_translator *step;
    struct cw_translator_pvt *state;
    struct cw_trans_pvt *next;
    struct timeval nextin;
    struct timeval nextout;
};

void cw_translator_free_path(struct cw_trans_pvt *p)
{
    struct cw_trans_pvt *pl;
    struct cw_trans_pvt *pn;

    pn = p;
    while (pn)
    {
        pl = pn;
        pn = pn->next;
        if (pl->state  &&  pl->step->destroy)
            pl->step->destroy(pl->state);
        free(pl);
    }
}

/* Build a set of translators based upon the given source and destination formats */
struct cw_trans_pvt *cw_translator_build_path(int dest, int dest_rate, int source, int source_rate)
{
    struct cw_trans_pvt *tmpr = NULL;
    struct cw_trans_pvt *tmp = NULL;
    
    source = bottom_bit(source);
    dest = bottom_bit(dest);
    
    while (source != dest)
    {
        if (!tr_matrix[source][dest].step)
        {
            /* We shouldn't have allocated any memory */
            cw_log(LOG_WARNING,
                     "No translator path from %s to %s\n", 
                     cw_getformatname(1 << source),
                     cw_getformatname(1 << dest));
            return NULL;
        }

        if (tmp)
        {
            tmp->next = malloc(sizeof(*tmp));
            tmp = tmp->next;
        }
        else
        {
            tmp = malloc(sizeof(*tmp));
        }
        if (tmp == NULL)
        {
            cw_log(LOG_WARNING, "Out of memory\n");
            if (tmpr)
                cw_translator_free_path(tmpr);    
            return NULL;
        }

        /* Set the root, if it doesn't exist yet... */
        if (tmpr == NULL)
            tmpr = tmp;

        tmp->next = NULL;
        tmp->nextin =
        tmp->nextout = cw_tv(0, 0);
        tmp->step = tr_matrix[source][dest].step;
        tmp->state = tmp->step->newpvt();
        
        if (!tmp->state)
        {
            cw_log(LOG_WARNING, "Failed to build translator step from %d to %d\n", source, dest);
            cw_translator_free_path(tmpr);    
            return NULL;
        }
        
        /* Keep going if this isn't the final destination */
        source = tmp->step->dst_format;
    }
    return tmpr;
}

struct cw_frame *cw_translate(struct cw_trans_pvt *path, struct cw_frame *f, int consume)
{
    struct cw_trans_pvt *p;
    struct cw_frame *out;
    struct timeval delivery;
    int has_timing_info;
    long ts;
    long len;
    int seq_no;
    
    has_timing_info = f->has_timing_info;
    ts = f->ts;
    len = f->len;
    seq_no = f->seq_no;

    p = path;
    /* Feed the first frame into the first translator */
    p->step->framein(p->state, f);
    if (!cw_tvzero(f->delivery))
    {
        if (!cw_tvzero(path->nextin))
        {
            /* Make sure this is in line with what we were expecting */
            if (!cw_tveq(path->nextin, f->delivery))
            {
                /* The time has changed between what we expected and this
                   most recent time on the new packet.  If we have a
                   valid prediction adjust our output time appropriately */
                if (!cw_tvzero(path->nextout))
                {
                    path->nextout = cw_tvadd(path->nextout,
                                               cw_tvsub(f->delivery, path->nextin));
                }
                path->nextin = f->delivery;
            }
        }
        else
        {
            /* This is our first pass.  Make sure the timing looks good */
            path->nextin = f->delivery;
            path->nextout = f->delivery;
        }
        /* Predict next incoming sample */
        path->nextin = cw_tvadd(path->nextin, cw_samp2tv(f->samples, 8000));
    }
    delivery = f->delivery;
    if (consume)
        cw_fr_free(f);
    while (p)
    {
        /* If we get nothing out, return NULL */
        if ((out = p->step->frameout(p->state)) == NULL)
            return NULL;
        /* If there is a next state, feed it in there.  If not,
           return this frame  */
        if (p->next)
        { 
            p->next->step->framein(p->next->state, out);
        }
        else
        {
            if (!cw_tvzero(delivery))
            {
                /* Regenerate prediction after a discontinuity */
                if (cw_tvzero(path->nextout))
                    path->nextout = cw_tvnow();

                /* Use next predicted outgoing timestamp */
                out->delivery = path->nextout;
                
                /* Predict next outgoing timestamp from samples in this
                   frame. */
                path->nextout = cw_tvadd(path->nextout, cw_samp2tv( out->samples, 8000));
            }
            else
            {
                out->delivery = cw_tv(0, 0);
            }
            /* Invalidate prediction if we're entering a silence period */
            if (out->frametype == CW_FRAME_CNG)
                path->nextout = cw_tv(0, 0);

            out->has_timing_info = has_timing_info;
            if (has_timing_info)
            {
                out->ts = ts;
                out->len = len;
                //out->len = cw_codec_get_samples(out)/8;
                out->seq_no = seq_no;
            }

            return out;
        }
        p = p->next;
    }
    cw_log(LOG_WARNING, "I should never get here...\n");
    return NULL;
}

static void calc_cost(struct cw_translator *t, int secs)
{
    int sofar;
    struct cw_translator_pvt *pvt;
    struct cw_frame *f;
    struct cw_frame *out;
    struct timeval start;
    int cost;

    if (secs < 1)
        secs = 1;
    
    /* If they don't make samples, give them a terrible score */
    if (t->sample == NULL)
    {
        cw_log(LOG_WARNING, "Translator '%s' does not produce sample frames.\n", t->name);
        t->cost = 99999;
        return;
    }
    if ((pvt = t->newpvt()) == NULL)
    {
        cw_log(LOG_WARNING, "Translator '%s' appears to be broken and will probably fail.\n", t->name);
        t->cost = 99999;
        return;
    }
    start = cw_tvnow();
    /* Call the encoder until we've processed "secs" seconds of data */
    for (sofar = 0;  sofar < secs*t->dst_rate;  )
    {
        if ((f = t->sample()) == NULL)
        {
            cw_log(LOG_WARNING, "Translator '%s' failed to produce a sample frame.\n", t->name);
            t->destroy(pvt);
            t->cost = 99999;
            return;
        }
        t->framein(pvt, f);
        cw_fr_free(f);
        while ((out = t->frameout(pvt)))
        {
            sofar += out->samples;
            cw_fr_free(out);
        }
    }
    cost = cw_tvdiff_ms(cw_tvnow(), start);
    t->destroy(pvt);
    t->cost = cost/secs;
    if (t->cost <= 0)
        t->cost = 1;
}

static void rebuild_matrix(int samples)
{
    struct cw_translator *t;
    int changed;
    int x;
    int y;
    int z;

    if (option_debug)
        cw_log(LOG_DEBUG, "Reseting translation matrix\n");
    /* Use the list of translators to build a translation matrix */
    bzero(tr_matrix, sizeof(tr_matrix));
    t = list;
    while (t)
    {
        if (samples)
            calc_cost(t,samples);
      
        if (!tr_matrix[t->src_format][t->dst_format].step
            ||
            tr_matrix[t->src_format][t->dst_format].cost > t->cost)
        {
            tr_matrix[t->src_format][t->dst_format].step = t;
            tr_matrix[t->src_format][t->dst_format].cost = t->cost;
        }
        t = t->next;
    }
    do
    {
        changed = 0;
        /* Don't you just love O(N^3) operations? */
        for (x = 0;  x < MAX_FORMAT;  x++)
        {
            for (y = 0;  y < MAX_FORMAT;  y++)
            {
                if (x != y)
                {
                    for (z = 0;  z < MAX_FORMAT;  z++)
                    {
                        if ((x != z)  &&  (y != z))
                        {
                            if (tr_matrix[x][y].step
                                &&
                                tr_matrix[y][z].step
                                &&
                                    (!tr_matrix[x][z].step
                                    ||
                                    (tr_matrix[x][y].cost + tr_matrix[y][z].cost < tr_matrix[x][z].cost)))
                            {
                                /* We can get from x to z via y with a cost that
                                   is the sum of the transition from x to y and
                                   from y to z */
                                tr_matrix[x][z].step = tr_matrix[x][y].step;
                                tr_matrix[x][z].cost = tr_matrix[x][y].cost + tr_matrix[y][z].cost;
                                if (option_debug)
                                    cw_log(LOG_DEBUG, "Discovered %d cost path from %s to %s, via %d\n", tr_matrix[x][z].cost, cw_getformatname(x), cw_getformatname(z), y);
                                changed++;
                            }
                        }
                    }
                }
            }
        }
    }
    while (changed);
}

static int show_translation(int fd, int argc, char *argv[])
{
#define SHOW_TRANS 11
    int x;
    int y;
    int z;
    char line[120]; /* Assume 120 character wide screen */

    if (argc > 4) 
        return RESULT_SHOWUSAGE;

    if (argv[2]  &&  !strcasecmp(argv[2], "recalc"))
    {
        z = argv[3]  ?  atoi(argv[3])  :  1;

        if (z <= 0)
        {
            cw_cli(fd,"         C'mon let's be serious here... defaulting to 1.\n");
            z = 1;
        }

        if (z > MAX_RECALC)
        {
            cw_cli(fd,"         Maximum limit of recalc exceeded by %d, truncating value to %d\n", z - MAX_RECALC,MAX_RECALC);
            z = MAX_RECALC;
        }
        cw_cli(fd,"         Recalculating Codec Translation (number of sample seconds: %d)\n\n", z);
        rebuild_matrix(z);
    }

    cw_cli(fd, "         Translation times between formats (in milliseconds)\n");
    cw_cli(fd, "          Source Format (Rows) Destination Format(Columns)\n\n");
    cw_mutex_lock(&list_lock);
    for (x = -1;  x < SHOW_TRANS;  x++)
    {
        line[0] = ' ';
        line[1] = '\0';
        for (y = -1;  y < SHOW_TRANS;  y++)
        {
            if (x >= 0  &&  y >= 0  &&  tr_matrix[x][y].step)
                snprintf(line + strlen(line), sizeof(line) - strlen(line), " %5d", (tr_matrix[x][y].cost >= 99999)  ?  tr_matrix[x][y].cost - 99999  :  tr_matrix[x][y].cost);
            else if (((x == -1  &&  y >= 0)  ||  (y == -1  &&  x >= 0)))
                snprintf(line + strlen(line), sizeof(line) - strlen(line), " %5s", cw_getformatname(1 << (x + y + 1)));
            else if (x != -1  &&  y != -1)
                snprintf(line + strlen(line), sizeof(line) - strlen(line), "     -");
            else
                snprintf(line + strlen(line), sizeof(line) - strlen(line), "      ");
        }
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "\n");
        cw_cli(fd, line);            
    }
    cw_mutex_unlock(&list_lock);
    return RESULT_SUCCESS;
}

int cw_translator_best_choice(int *dst, int *srcs)
{
    /* Calculate our best source format, given costs, and a desired destination */
    int x;
    int y;
    int best = -1;
    int bestdst = 0;
    int cur = 1;
    int besttime = INT_MAX;
    int common;

    if ((common = (*dst) & (*srcs)))
    {
        /* We have a format in common */
        for (y = 0;  y < MAX_FORMAT;  y++)
        {
            if (cur & common)
            {
                /* This is a common format to both.  Pick it if we don't have one already */
                besttime = 0;
                bestdst = cur;
                best = cur;
            }
            cur <<= 1;
        }
    }
    else
    {
        /* We will need to translate */
        cw_mutex_lock(&list_lock);
        for (y = 0;  y < MAX_FORMAT;  y++)
        {
            if (cur & *dst)
            {
                for (x = 0;  x < MAX_FORMAT;  x++)
                {
                    if ((*srcs & (1 << x))          /* x is a valid source format */
                        &&
                        tr_matrix[x][y].step        /* There's a step */
                        &&
                        (tr_matrix[x][y].cost < besttime))
                    {
                        /* It's better than what we have so far */
                        best = 1 << x;
                        bestdst = cur;
                        besttime = tr_matrix[x][y].cost;
                    }
                }
            }
            cur <<= 1;
        }
        cw_mutex_unlock(&list_lock);
    }
    if (best > -1)
    {
        *srcs = best;
        *dst = bestdst;
        best = 0;
    }
    return best;
}







/* ************************************************************************** */

static int added_cli = 0;

static char show_trans_usage[] =
"Usage: show translation [recalc] [<recalc seconds>]\n"
"       Displays known codec translators and the cost associated\n"
"with each conversion.  if the argument 'recalc' is supplied along\n"
"with optional number of seconds to test a new test will be performed\n"
"as the chart is being displayed.\n";

static struct cw_cli_entry show_trans =
{
    { "show", "translation", NULL },
    show_translation,
    "Display translation matrix",
    show_trans_usage
};

int cw_register_translator(struct cw_translator *t)
{
    char tmp[120]; /* Assume 120 character wide screen */

    t->src_format = bottom_bit(t->src_format);
    t->dst_format = bottom_bit(t->dst_format);
    if (t->src_format >= MAX_FORMAT)
    {
        cw_log(LOG_WARNING, "Source format %s is larger than MAX_FORMAT\n", cw_getformatname(1 << t->src_format));
        return -1;
    }
    if (t->dst_format >= MAX_FORMAT)
    {
        cw_log(LOG_WARNING, "Destination format %s is larger than MAX_FORMAT\n", cw_getformatname(1 << t->dst_format));
        return -1;
    }
    calc_cost(t, 1);
    if (option_verbose > 1)
        cw_verbose(VERBOSE_PREFIX_2 "Registered translator '%s' from format %s to %s, cost %d\n", cw_term_color(tmp, t->name, COLOR_MAGENTA, COLOR_BLACK, sizeof(tmp)), cw_getformatname(1 << t->src_format), cw_getformatname(1 << t->dst_format), t->cost);
    cw_mutex_lock(&list_lock);
    if (!added_cli)
    {
        cw_cli_register(&show_trans);
        added_cli++;
    }
    t->next = list;
    list = t;
    rebuild_matrix(0);
    cw_mutex_unlock(&list_lock);
    return 0;
}

int cw_unregister_translator(struct cw_translator *t)
{
    char tmp[120]; /* Assume 120 character wide screen */
    struct cw_translator *u;
    struct cw_translator *ul = NULL;
    
    cw_mutex_lock(&list_lock);
    u = list;
    while (u)
    {
        if (u == t)
        {
            if (ul)
                ul->next = u->next;
            else
                list = u->next;
            if (option_verbose > 1)
                cw_verbose(VERBOSE_PREFIX_2 "Unregistered translator '%s' from format %s to %s\n", cw_term_color(tmp, t->name, COLOR_MAGENTA, COLOR_BLACK, sizeof(tmp)), cw_getformatname(1 << t->src_format), cw_getformatname(1 << t->dst_format));
            break;
        }
        ul = u;
        u = u->next;
    }
    rebuild_matrix(0);
    cw_mutex_unlock(&list_lock);
    return (u  ?  0  :  -1);
}


