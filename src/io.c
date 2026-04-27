/*
 *  io.c -- JAMin I/O driver.
 *
 *  Copyright (C) 2003, 2004 Jack O'Quin.
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*  DSP Engine
 *
 *  The DSP engine is managed as if it were firmware running on a
 *  separate signal processing board.  It uses two realtime threads:
 *  the JACK thread and the DSP thread.  The JACK thread runs the JACK
 *  process() callback.  In some cases, signal processing is invoked
 *  directly from the JACK thread.  But, when the JACK period is too
 *  short for efficiently computing the FFT, signal processing should
 *  be done in the DSP thread, instead.
 *
 *  The DSP thread is created if the -t option was not specified and
 *  the process is capable of creating a realtime thread.  Otherwise,
 *  all signal processing will be done in the JACK thread, regardless
 *  of buffer size.
 *
 *  The JACK buffer size could change dynamically due to the
 *  jack_set_buffer_size_callback() function.  So, we do not assume
 *  that this buffer size is fixed.  Since current versions of JACK
 *  (May 2003) do not support that feature, there is no way to test
 *  that it is handled correctly.
 */

/*  Changes to this file should be tested for these conditions...
 *
 *  without -t option
 *	+ JACK running realtime (as root)
 *	   + JACK buffer size < DSP block size
 *	   + JACK buffer size >= DSP block size
 *	+ JACK running realtime (using capabilities)
 *	   + JACK buffer size < DSP block size
 *	   + JACK buffer size >= DSP block size
 *	+ JACK not running realtime
 *
 *  with -t option
 *	+ JACK running realtime
 *	   + JACK buffer size < DSP block size
 *	   + JACK buffer size >= DSP block size
 *	+ JACK not running realtime
 */
#include <OS.h> 


#include "config.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <getopt.h>
#include <errno.h>
#include <assert.h>
#include <jack/jack.h>
#ifdef HAVE_JACK_CREATE_THREAD
#include <jack/thread.h>
#endif

//#include "ringbuffer.h"		/* uses <jack/ringbuffer.h>, if available */
#include "process.h"
#include "resource.h"
#include "plugin.h"
#include "io.h"
#include "transport.h"
#include "jackstatus.h"
#include "state.h"
#include "spectrum.h"
#include "preferences.h"
#include "debug.h"
#include "help.h"
#include "support.h"
#include <OS.h>

/* Valid JAMin options - kept standard for compatibility */
char *jamin_options = "dFf:j:n:hprTtvVl:s:c:igD";   
char *pname;				      

/* Logic Switches */
int dummy_mode = 0;			      
int all_errors_fatal = 0;		      
int show_help = 0;			      

/* 
 * In Haiku, we default connect_ports to 0. 
 * The Media Server/Cortex handles routing, so JAMin shouldn't 
 * try to force physical connections on startup. 
 */
int connect_ports = 0;			      

int trace_option = 0;			      

/* 
 * We keep thread_option enabled so the background DSP thread 
 * (which we've pinned to Haiku priority 90) still runs.
 */
int thread_option = 1;			      

int debug_level = DBG_OFF;		      
char session_file[PATH_MAX];		      

/* Interface Modes */
int gui_mode = 0;			      /* 0: Classic, 1: Presets, 2: Daemon */

/* Limiter Selection */
int limiter_plugin_type = 0;           

/* Error string buffer */
static char *errstr = NULL;

/*  Synchronization within the DSP engine state machine. */
#define DSP_INIT	001
#define DSP_ACTIVATING	002
#define DSP_STARTING	004
#define DSP_RUNNING	010
#define DSP_STOPPING	020
#define DSP_STOPPED	040

#define DSP_STATE_IS(x)		((dsp_state)&(x))
#define DSP_STATE_NOT(x)	((dsp_state)&(~(x)))

/* Marked as extern so jack_stubs.cpp can see the engine status */
volatile int dsp_state = DSP_INIT;

static int have_dsp_thread = 0;		
static size_t dsp_block_bytes;		

/* 
 * Standard POSIX threading for the DSP loop. 
 * In Haiku, we will manually set this thread's priority to 90.
 */
pthread_t dsp_thread;	
pthread_cond_t run_dsp = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lock_dsp = PTHREAD_MUTEX_INITIALIZER;

/* 
 * Ringbuffers are declared but remain NULL in the Haiku port.
 * We use the Scratch Pads in jack_stubs.cpp for zero-copy transport.
 */
static jack_ringbuffer_t *in_rb[NCHANNELS] = {NULL};
static jack_ringbuffer_t *out_rb[BCHANNELS] = {NULL};

/* JACK/Haiku status data */
io_jack_status_t jst = {0};		
jack_client_t *client = NULL;			
char *client_name = NULL;		
char *server_name = NULL;		
int nchannels = NCHANNELS;		
int bchannels = BCHANNELS;  

/* Port handles returned by jack_port_register in stubs */
jack_port_t *input_ports[NCHANNELS+1] = {NULL};
jack_port_t *output_ports[BCHANNELS+1] = {NULL};

/* Port names as they will appear in Cortex / Media Preferences */
static const char *in_names[NCHANNELS] = {"in_L", "in_R"};
static const char *out_names[BCHANNELS] = {
    "a.master.out_L", "a.master.out_R",
    "b.low.out_L", "b.low.out_R", 
    "c.mid.out_L", "c.mid.out_R", 
    "d.high.out_L", "d.high.out_R" 
};

/* User-specified manual connections (usually NULL in Haiku) */
static const char *iports[NCHANNELS] = {NULL, NULL};
static const char *oports[BCHANNELS] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

/* io_trace -- trace I/O activity for the Haiku Node.
 *
 *  Real-time safe: uses trylock to avoid blocking the DSP thread 
 *  if the UI thread is currently reading the logs.
 */
pthread_mutex_t io_trace_lock = PTHREAD_MUTEX_INITIALIZER;
#define TR_BUFSIZE	256		/* must be power of 2 */
#define TR_MSGSIZE	64      /* slightly increased for longer Haiku names */

struct io_trace_t {
    jack_nframes_t timestamp;		/* Current Media Kit Frame/Time */
    char message[TR_MSGSIZE];		/* trace message */
};

size_t tr_next = 0;
struct io_trace_t tr_buf[TR_BUFSIZE] = {{0}};

void io_set_sample_rate(float rate) {
    jst.sample_rate = (int)rate;
}

void io_trace(const char *fmt, ...)
{
    va_list ap;

    /* If lock is already held, we skip this entry.
     * This is CRITICAL for real-time safety in the Media Kit. */
    if (pthread_mutex_trylock(&io_trace_lock) == 0) {

        /* Get the current 'time' from our JACK stub.
         * In Haiku, this should ideally return fCurrentPerformanceTime
         * to help debug sync issues between nodes. */
        if (client)
            tr_buf[tr_next].timestamp = jack_frame_time(client);
        else
            tr_buf[tr_next].timestamp = 0;

        /* Format trace message */
        va_start(ap, fmt);
        vsnprintf(tr_buf[tr_next].message, TR_MSGSIZE, fmt, ap);
        va_end(ap);

        /* Ensure the message is null-terminated */
        tr_buf[tr_next].message[TR_MSGSIZE - 1] = '\0';

        /* Advance the ring buffer index */
        tr_next = (tr_next + 1) & (TR_BUFSIZE - 1);

        pthread_mutex_unlock(&io_trace_lock);
    }
}


/* io_list_trace -- list trace buffer contents
 *
 *  In the Haiku port, this helps track when the Media Node 
 *  receives buffers vs when the DSP actually processes them.
 */
void io_list_trace()
{
    size_t t;

    // Use a try-lock here to avoid hanging the UI if the trace is huge
    if (pthread_mutex_lock(&io_trace_lock) != 0) return;

    t = tr_next;
    fprintf(stderr, "--- [ %s Trace Log Begin ] ---\n", PACKAGE);
    do {
        if (tr_buf[t].message[0] != '\0') {
            /* We use PRIu32 for the JACK-style timestamp (frames)
               but in Haiku these are often performance time microseconds. */
            fprintf(stderr, "  [%010" PRIu32 "] : %s\n", 
                    tr_buf[t].timestamp, tr_buf[t].message);
        }
        t = (t+1) & (TR_BUFSIZE-1);
    } while (t != tr_next);
    fprintf(stderr, "--- [ %s Trace Log End ] ---\n", PACKAGE);

    pthread_mutex_unlock(&io_trace_lock);
}


/* io_errlog -- log I/O error.
 *
 *  Modified to ensure Media Kit errors are visible in the 
 *  Haiku Terminal.
 */
void io_errlog(int err, char *fmt, ...)
{
    va_list ap;
    char buffer[300];

    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    // Record the error in the trace buffer for later listing
    IF_DEBUG(DBG_TERSE,
	     io_trace("!!! ERROR %d: %s", err, buffer));

    // In Haiku, g_print usually goes to the terminal
    g_print(_("%s [Haiku-Node] Error %d: %s\n"), PACKAGE, err, buffer);

    if (all_errors_fatal) {
        fprintf(stderr, "[JAMin-Haiku] FATAL ERROR. Terminating as requested.\n");
        // Instead of a hard abort(), we try a clean shutdown of the node
        io_cleanup();
        exit(err);
    }
}



/* io_new_state -- DSP engine state transition.
 *
 *  In Haiku, this remains the core state tracker. 
 *  State 010 (DSP_RUNNING) is what enables the GUI meters.
 */
void io_new_state(int next)
{
    /* Validation logic remains mostly the same to ensure JAMin's
       internal logic doesn't get confused by out-of-order calls. */
    switch (next) {
    case DSP_INIT:
        goto invalid;
    case DSP_ACTIVATING:
        if (DSP_STATE_NOT(DSP_INIT))
            goto invalid;
        break;
    case DSP_STARTING:
        if (DSP_STATE_NOT(DSP_ACTIVATING))
            goto invalid;
        break;
    case DSP_RUNNING:
        /* Allow transition from ACTIVATING or STARTING */
        if (DSP_STATE_NOT(DSP_ACTIVATING|DSP_STARTING))
            goto invalid;
        break;
    case DSP_STOPPING:
        if (DSP_STATE_NOT(DSP_ACTIVATING|DSP_RUNNING|DSP_STARTING))
            goto invalid;
        break;
    case DSP_STOPPED:
        if (DSP_STATE_NOT(DSP_INIT|DSP_STOPPING))
            goto invalid;
        break;
    default:
    invalid:
        /* Log error but don't crash; Media Kit might have slightly different timing */
        fprintf(stderr, "[JAMin-Haiku] State Transition Error: 0%o -> 0%o\n", dsp_state, next);
        return;
    }

    dsp_state = next;
    
    // Log the state change to terminal for debugging node activation
    IF_DEBUG(DBG_TERSE, io_trace("new DSP state: 0%o.", next));
    if (next == DSP_RUNNING) {
        fprintf(stderr, "[JAMin-Haiku] Engine State: RUNNING\n");
    }
}


/* io_get_status -- collect current status for the UI. */
void io_get_status(io_jack_status_t *jp)
{
    if (client) {
        /* 
         * HAIKU CPU LOAD CALCULATION
         * Haiku tracks CPU time per-thread. We must sum the user/kernel 
         * time of all threads in the current team to get the total load.
         */
        static bigtime_t last_time = 0;
        static bigtime_t last_active = 0;
        
        bigtime_t now = system_time();
        
        // Update roughly every 100ms
        if (now - last_time > 100000) {
            thread_info t_info;
            int32 cookie = 0;
            bigtime_t active = 0;
            
            // Iterate over all threads in this team (B_CURRENT_TEAM)
            while (get_next_thread_info(B_CURRENT_TEAM, &cookie, &t_info) == B_OK) {
                active += t_info.user_time + t_info.kernel_time;
            }
            
            if (last_time > 0) {
                double total_delta  = (double)(now - last_time);
                double active_delta = (double)(active - last_active);
                
                // Calculate Percentage
                // Note: On multi-core systems, this can exceed 100% (e.g. 200% = 2 cores full)
                float instantaneous_load = (float)((active_delta / total_delta) * 100.0);
                
                // Simple Low-Pass Filter (Smoothing)
                jst.cpu_load = (jst.cpu_load * 0.8f) + (instantaneous_load * 0.2f);
            }
            
            last_time = now;
            last_active = active;
        }
    }
    
    *jp = jst;
}



/* io_set_latency -- set DSP engine latencies for Haiku */
void io_set_latency(int source, jack_nframes_t delay)
{
    static jack_nframes_t latency_delay[LAT_NSOURCES] = {0};
    
    if (source < 0 || source >= LAT_NSOURCES) return;

    /* Update internal status record */
    jst.latency += delay - latency_delay[source];
    latency_delay[source] = delay;

    /* 
     * In Haiku, we don't 'set' port latency. Instead, we tell the Node
     * to update its internal latency so the Media Server can adjust 
     * the system-wide synchronization.
     */
    extern void haiku_update_node_latency(bigtime_t total_us);
    
    // Get actual sample rate from jst (calculated in io_init/io_bufsize)
    float rate = (jst.sample_rate > 0) ? (float)jst.sample_rate : 44100.0f;

    // Convert frames to microseconds (bigtime_t)
    // Formula: (frames * 1,000,000) / sample_rate
    bigtime_t total_us = (bigtime_t)(((double)jst.latency * 1000000.0) / rate);

    /* Push the update to the JaminNode */
    haiku_update_node_latency(total_us);
    
    // DEBUG: Verify the conversion matches the detected rate
    IF_DEBUG(DBG_TERSE, 
             io_trace("[Haiku Latency] Frames: %u | Rate: %.1f Hz -> Time: %lld us", 
                      jst.latency, rate, total_us));
    
    IF_DEBUG(DBG_TERSE, 
             io_trace("Haiku Latency Update: %lld us (%u frames at %.1f Hz)", 
                      total_us, jst.latency, rate));
}



/* io_get_dsp_buffers -- get buffer addresses for DSP thread.
 *
 * In the Haiku port, we bypass JACK ringbuffers and point 
 * directly to the scratch pads managed in jack_stubs.cpp.
 */
int io_get_dsp_buffers(int nchannels, int bchannels,
		       jack_default_audio_sample_t *in[NCHANNELS],
		       jack_default_audio_sample_t *out[BCHANNELS])
{
    /* 
     * Link to the scratch pads defined in jack_stubs.cpp.
     * We use 'extern' to find them at link-time.
     */
    extern float* _scratch_in_L;
    extern float* _scratch_in_R;
    extern float* _scratch_out_L;
    extern float* _scratch_out_R;
    extern float* _scratch_dummy;

    /* 
     * Safety check: If the scratch pads aren't allocated yet,
     * tell the DSP engine there is no data to process.
     */
    if (!_scratch_in_L || !_scratch_out_L) {
        return 0; 
    }

    /* Assign Input scratch pads */
    in[0] = (jack_default_audio_sample_t *)_scratch_in_L;
    in[1] = (jack_default_audio_sample_t *)_scratch_in_R;

    /* Assign Output scratch pads */
    out[0] = (jack_default_audio_sample_t *)_scratch_out_L;
    out[1] = (jack_default_audio_sample_t *)_scratch_out_R;
    
    /* 
     * Fill remaining output ports (low/mid/high/etc) 
     * with the dummy buffer to prevent processing null pointers.
     */
    for (int i = 2; i < bchannels; i++) {
        out[i] = (jack_default_audio_sample_t *)_scratch_dummy;
    }

    /* 
     * Returns 1 (success) because in the synchronous Media Kit 
     * architecture, we always have a buffer ready to process 
     * once io_schedule() wakes the thread.
     */
    return 1;
}



void *io_dsp_thread(void *arg)
{
    jack_default_audio_sample_t *in[NCHANNELS], *out[BCHANNELS];

    int rc;

    fprintf(stderr, "[JAMin-Haiku] DSP thread started and pinned to Media Kit timing.\n");

    pthread_mutex_lock(&lock_dsp);

    if (DSP_STATE_IS(DSP_ACTIVATING))
        io_new_state(DSP_STARTING);

    while (DSP_STATE_NOT(DSP_STOPPING)) {

        /* 
         * In Haiku, we don't want to spin or wait on a condition variable 
         * if we can help it. We check if the Media Kit has filled our 
         * scratch pads/buffers.
         */
        if (io_get_dsp_buffers(nchannels, bchannels, in, out)) {

            // Call the actual JAMin DSP engine
            rc = process_signal(dsp_block_size, nchannels, bchannels, in, out);
            
            if (rc != 0)
                io_errlog(EAGAIN, "signal processing error: %d.", rc);

            /* 
             * CRITICAL: We don't 'advance' ringbuffers anymore because 
             * jack_stubs handles the buffer pointers directly. 
             * We just mark the state as running.
             */
            if (DSP_STATE_IS(DSP_STARTING))
                io_new_state(DSP_RUNNING);
        }

        /* 
         * We still wait here, but the trigger now comes from 
         * JaminNode::BufferReceived calling io_schedule().
         */
        rc = pthread_cond_wait(&run_dsp, &lock_dsp);
        
        if (rc != 0) {
            io_errlog(EINVAL, "pthread_cond_wait() error: %d.", rc);
            break; 
        }
    }

    pthread_mutex_unlock(&lock_dsp);
    fprintf(stderr, "[JAMin-Haiku] DSP thread shutting down.\n");
    return NULL;
}




void io_schedule()
{
    /* 
     * In Haiku, the Media Node calls this from BufferReceived.
     * We signal the DSP thread to process the scratch pads.
     */
    pthread_mutex_lock(&lock_dsp);
    pthread_cond_signal(&run_dsp);
    pthread_mutex_unlock(&lock_dsp);
}




int io_queue(jack_nframes_t nframes, int nchannels, int bchannels,
	     jack_default_audio_sample_t *in[NCHANNELS],
	     jack_default_audio_sample_t *out[BCHANNELS])
{
    /* 
     * In the Haiku port, 'in' and 'out' are pointers to our scratch pads.
     * The data is already there! We just need to trigger the DSP.
     */

    if (DSP_STATE_IS(DSP_ACTIVATING)) {
        /* If we are still activating, zero out the output to avoid loud noise */
        for (int chan = 0; chan < bchannels; chan++) {
            memset(out[chan], 0, nframes * sizeof(float));
        }
        return EBUSY;
    }

    // Trigger the DSP thread (io_dsp_thread) to run process_signal()
    io_schedule();

    /*
     * NOTE: In a perfectly synchronous Haiku node, we might call 
     * process_signal() directly here instead of scheduling a thread.
     * But since JAMin's UI and FFT rely on the background thread state,
     * scheduling is safer for now.
     */

    return 0;
}



int io_process(jack_nframes_t nframes, void *arg)
{
    jack_default_audio_sample_t *in[NCHANNELS], *out[BCHANNELS];
    int chan;
	int return_code = 0;

    /* 1. Get scratch pad addresses from your jack_stubs.cpp */
    for (chan = 0; chan < bchannels; chan++) {
        if (chan < nchannels) {
            in[chan] = (jack_default_audio_sample_t *)jack_port_get_buffer(input_ports[chan], nframes);
        } 
        out[chan] = (jack_default_audio_sample_t *)jack_port_get_buffer(output_ports[chan], nframes);
    }

    /* 2. Check Engine State */
    if (DSP_STATE_IS(DSP_ACTIVATING | DSP_INIT)) {
        // Zero out the output if the engine isn't ready to prevent noise
        for (chan = 0; chan < bchannels; chan++) {
            memset(out[chan], 0, nframes * sizeof(jack_default_audio_sample_t));
        }
        return 0;
    }

    /* 3. Direct Processing 
     * In Haiku, we bypass the separate DSP thread and io_queue logic.
     * This ensures the Mixer gets processed audio back instantly.
     */
    if (nframes <= dsp_block_size) {
        // Standard block size processing
        return_code = process_signal(nframes, nchannels, bchannels, in, out);
    } else {
        // Handle cases where Haiku gives us a buffer larger than JAMin's internal FFT size
        jack_nframes_t frames_left = nframes;
        while (frames_left >= dsp_block_size) {
            process_signal(dsp_block_size, nchannels, bchannels, in, out);
            
            for (chan = 0; chan < bchannels; chan++) {
                if (chan < nchannels) in[chan] += dsp_block_size;
                out[chan] += dsp_block_size;
            }
            frames_left -= dsp_block_size;
        }
        
        // Process remaining tail if any
        if (frames_left > 0) {
            process_signal(frames_left, nchannels, bchannels, in, out);
        }
    }

    /* 4. Update UI Meters 
     * Even though we skip the DSP thread, we still trigger io_schedule 
     * so JAMin's spectrum analyzer and meters update on the screen.
     */
    io_schedule();

    return 0;
}



int io_xrun(void *arg)
{
    /* Increment the counter so the JAMin status bar shows the error */
    ++jst.xruns; 
    
    /* Log to the terminal so we can see it in Haiku's Terminal */
    fprintf(stderr, "[JAMin-Haiku] Performance Warning: Buffer Late!\n");
    
    IF_DEBUG(DBG_TERSE, io_trace("I/O xrun"));
    return 0;
}

/* io_bufsize -- Buffer size change.
 *
 * Called when the Haiku Media Kit renegotiates the buffer size 
 * (e.g., when you connect "SuperMusicThingy" to the node).
 */
int io_bufsize(jack_nframes_t nframes, void *arg)
{
    jst.buf_size = nframes;
    
    IF_DEBUG(DBG_TERSE, io_trace("buffer size is %" PRIu32, nframes));

    /* 
     * In Haiku, we don't have a separate DSP thread latency anymore 
     * because we process in-place. We set the latency to 0 here 
     * and let io_set_latency handle the plugin-specific delays.
     */
    io_set_latency(LAT_BUFFERS, 0);

    return 0;
}

/* io_free_heap -- Standard cleanup helper. */
static inline void io_free_heap(char **p)
{
    if (p && *p) {
        free(*p);
        *p = NULL;
    }
}


/* io_cleanup -- clean up all DSP I/O resources and Haiku Media Node */
void io_cleanup()
{
    int chan;

    IF_DEBUG(DBG_TERSE, io_trace("shutting down I/O and DSP"));

    /* Handle state transitions */
    switch (dsp_state) {
        case DSP_INIT:
            io_new_state(DSP_STOPPED);
            break;

        case DSP_ACTIVATING:
        case DSP_STARTING:
        case DSP_RUNNING:
            /* Stop the DSP thread if it exists */
            if (have_dsp_thread) {
                pthread_mutex_lock(&lock_dsp);
                io_new_state(DSP_STOPPING);
                pthread_cond_signal(&run_dsp);
                pthread_mutex_unlock(&lock_dsp);
                pthread_join(dsp_thread, NULL);
            } else {
                io_new_state(DSP_STOPPING);
            }
            break;
        
        case DSP_STOPPED:
            return; /* Already done */
    };	

    if (DSP_STATE_IS(DSP_STOPPING)) {
        /* 1. Stop processing immediately */
        jack_client_t *client_save = client;
        client = NULL;
        jst.active = 0;

        /* 2. Unregister the Media Node from Haiku */
        // We call a helper in jack_stubs to handle BMediaRoster::UnregisterNode
        extern void haiku_shutdown_node();
        haiku_shutdown_node();

        /* 3. Close the JACK stub client */
        if (client_save) {
            jack_client_close(client_save);
        }

        io_new_state(DSP_STOPPED);
        
        io_free_heap(&client_name);
        io_free_heap(&server_name);

        /* 4. Free ring buffers ONLY if they were allocated 
           (In the new scratch-pad version, these are usually NULL) */
        for (chan = 0; chan < bchannels; chan++) {
            if (chan < nchannels && in_rb[chan]) {
                jack_ringbuffer_free(in_rb[chan]);
                in_rb[chan] = NULL;
            }
            if (out_rb[chan]) {
                jack_ringbuffer_free(out_rb[chan]);
                out_rb[chan] = NULL;
            }
        }
        
        fprintf(stderr, "[JAMin-Haiku] I/O Cleanup complete.\n");
    }

    if (trace_option)
        io_list_trace();
}

void io_shutdown(void *arg)
{
    // If we are already inactive, don't try to shut down again
    if (jst.active == 0) return;

    fprintf(stderr, "[JAMin-Haiku] I/O Shutdown requested.\n");
    jst.active = 0;
    
    // This calls your updated io_cleanup() which now handles 
    // the Haiku Media Node unregistration.
    io_cleanup();
}


/*  Silly little function to check file names for a valid, readable file
    prior to trying to use them.  */
gboolean check_file (char *optarg)
{
    FILE *fp;

    if (optarg == NULL) return FALSE;

    if ((fp = fopen (optarg, "r")) == NULL)
    {
        errstr = g_strdup_printf(_("File %s : %s\nUsing default."), optarg, 
                                  strerror (errno));
        g_print("%s\n", errstr);
        message (GTK_MESSAGE_ERROR, errstr);
        free (errstr);
        return (FALSE);
    }

    fclose (fp);
    return (TRUE);
}



jack_client_t *io_jack_open()
{
    /* 
     * In the Haiku port, jack_client_open is provided by our jack_stubs.cpp.
     * It initializes the Media Kit Node and registers it with the Roster.
     */
    
    jack_status_t status;

    fprintf(stderr, "[JAMin-Haiku] Initializing Media Kit Bridge for client: %s\n", client_name);

    // Call our stubbed jack_client_open
    client = jack_client_open(client_name, JackNullOption, &status);

    if (client == NULL) {
        g_print(_("%s: Failed to initialize Haiku Media Kit bridge (status = 0x%2.0x)\n"),
                PACKAGE, status);
        return NULL;
    }

    /* 
     * If the stub assigned a unique name (e.g., if JAMin is already running),
     * update the global client_name so the UI displays correctly.
     */
    if (status & JackNameNotUnique) {
        if (client_name) free(client_name);
        client_name = strdup(jack_get_client_name(client));
        g_print(_("%s: Unique node name `%s' assigned by Media Server\n"), PACKAGE, client_name);
    }

    fprintf(stderr, "[JAMin-Haiku] Media Node created successfully.\n");

    return client;
}


void io_init(int argc, char *argv[])
{

    int opt, spectrum_freq;
    float crossfade_time;

    spectrum_freq = 10;
    crossfade_time = 1.0;
    gui_mode = 0;

    /* basename $0 */
    pname = strrchr(argv[0], '/');
    if (pname == 0) pname = argv[0];
    else pname++;

    /* Parse standard options */
    while ((opt = getopt(argc, argv, jamin_options)) != -1) {
        switch (opt) {
            case 'd': dummy_mode = 1; break;
            case 'F': all_errors_fatal = 1; break;
            case 'f':
                if (check_file(optarg)) {
                    strncpy(session_file, optarg, sizeof(session_file));
                    s_set_session_filename(session_file);
                }
                break;
            case 'n': client_name = strdup(optarg); break;
            case 's': sscanf(optarg, "%d", &spectrum_freq); break;
            case 'c': sscanf(optarg, "%f", &crossfade_time); break;
            case 'h': show_help = 1; break;
            case 'p': connect_ports = 0; break;
            case 'i': process_set_crossover_type(IIR); break;
            case 'g': gui_mode = 1; break;
            case 'D': gui_mode = 2; break;
            case 'l': 
                sscanf(optarg, "%d", &limiter_plugin_type);
                process_set_limiter_plugin(limiter_plugin_type);
                break;
            case 'v': debug_level += 1; break;
            case 'V': exit(9);
            default: show_help = 1; break;
        }
    }

    set_spectrum_freq(spectrum_freq);
    s_set_crossfade_time(crossfade_time);

    if (show_help) {
        fprintf(stderr, "JAMin Haiku Port\nUsage: %s [-f session] [-n name] [-g] [-D]\n", pname);
        exit(1);
    }

    /* Initialize the Bridge */
    if (!client_name) client_name = strdup(PACKAGE);

    fprintf(stderr, "[JAMin-Haiku] Initializing Media Kit Node...\n");
    client = io_jack_open();
    if (client == NULL) {
        fprintf(stderr, "[JAMin-Haiku] FATAL: Could not initialize Media Kit bridge.\n");
        exit(2);
    }

    /* Set "JACK" callback functions (linked to our stubs) */
    jack_set_process_callback(client, io_process, NULL);
    jack_on_shutdown(client, io_shutdown, NULL);
    jack_set_xrun_callback(client, io_xrun, NULL);
    jack_set_buffer_size_callback(client, io_bufsize, NULL);

    /* 
     * SYNC INTERNAL STATE:
     * In Haiku, we force the block size to 1024 frames (8192 bytes for float)
     * to ensure the FFT and Media Kit buffers align perfectly.
     */
    dsp_block_bytes = dsp_block_size * sizeof(jack_default_audio_sample_t);
    
    // Get initial settings from the Media Kit via the stubs
    jst.sample_rate = jack_get_sample_rate(client);
    io_bufsize(jack_get_buffer_size(client), NULL);

    /* initialize process_signal() with the Media Kit's sample rate */
    process_init((float)jst.sample_rate);
    
    fprintf(stderr, "[JAMin-Haiku] Node initialized at %u Hz.\n", (unsigned int)jst.sample_rate);

}





int io_create_dsp_thread()
{
    int rc;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);

    /* 1. Simplify Realtime Status 
     * In Haiku, we treat the Media Kit connection as inherently realtime.
     */
    jst.realtime = 1; 

    fprintf(stderr, "[JAMin-Haiku] Creating DSP thread...\n");

    /* 2. Create a standard thread 
     * We don't use jack_client_create_thread because our stubs 
     * are simpler. Standard pthreads are best here.
     */
    rc = pthread_create(&dsp_thread, &attributes, io_dsp_thread, NULL);
    
    if (rc == 0) {
        /* 3. Set Haiku-specific Realtime Priority
         * B_URGENT_DISPLAY_PRIORITY (66) or B_REAL_TIME_PRIORITY (100)
         * We use 90 to be just below the Media Server's main pulse.
         */
        thread_id haiku_tid = (thread_id)dsp_thread; // pthread_t is a pointer/long in Haiku
        set_thread_priority(haiku_tid, 90); 
        
        fprintf(stderr, "[JAMin-Haiku] DSP thread created successfully. State: %d\n", dsp_state);
        IF_DEBUG(DBG_TERSE, io_trace("Haiku DSP thread created (Priority 90)"));
    } else {
        fprintf(stderr, "[JAMin-Haiku] ERROR: Could not create DSP thread (rc=%d)\n", rc);
        io_errlog(rc, "error creating DSP thread");
    }

    pthread_attr_destroy(&attributes);
    return rc;
}



void io_activate()
{
    int chan;
    // We don't need bufsize or ringbuffer allocation anymore as 
    // jack_stubs uses the scratch pads directly.

    if (DSP_STATE_IS(DSP_STOPPED))
        return;

    io_new_state(DSP_ACTIVATING);

    // 1. Register Ports (These will be caught by your jack_stubs.cpp)
    for (chan = 0; chan < nchannels; chan++) {
        input_ports[chan] = jack_port_register(client, in_names[chan],
                       JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (input_ports[chan] == NULL) {
            g_print(_("%s: Cannot register ports."), PACKAGE);
            exit(2);
        }
    }
 
    for (chan = 0; chan < bchannels; chan++) {
        output_ports[chan] = jack_port_register(client, out_names[chan],
                       JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (output_ports[chan] == NULL) {
            g_print(_("%s: Cannot register ports."), PACKAGE);
            exit(2);
        }
    }   

    // 2. Activate the "Client" 
    if (jack_activate(client)) {
        g_print(_("%s: Cannot activate bridge."), PACKAGE);
        exit(2);
    }

    jst.active = 1;

    // 3. SKIP Physical Connections
    /* 
     * We REMOVE the jack_connect logic here. 
     * JAMin is now a Media Node; physical routing is handled 
     * by the Haiku Media Server/Cortex, not the app itself.
     */

    // 4. Threading and Latency
    /*
     * We force 'have_dsp_thread' to 0 because the Haiku Media Kit
     * provides the processing thread for us via BufferReceived.
     */
    pthread_mutex_lock(&lock_dsp);
    have_dsp_thread = 0; 
    
    io_new_state(DSP_RUNNING);

    /* Set internal latency to 0 for the buffer part, 
       since we are processing in-place in the Haiku thread. */
    io_set_latency(LAT_BUFFERS, 0);
    
    pthread_mutex_unlock(&lock_dsp);

    fprintf(stderr, "[JAMin-Haiku] Bridge Activated. Engine Running.\n");
}
