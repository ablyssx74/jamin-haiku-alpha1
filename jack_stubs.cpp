#include <app/MessageFilter.h>
#include <app/Messenger.h>
#include <app/Application.h>
#include <MediaKit.h>
#include <MediaNode.h>
#include <BufferConsumer.h>
#include <BufferProducer.h>
#include <MediaEventLooper.h>
#include <media/MediaDefs.h>
#include <media/MediaNode.h>
#include <TimeSource.h>
#include <String.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <OS.h>
#include <cmath> 
#include <Locker.h>
#include <Autolock.h>
#include <algorithm>
#include <map>

#define DEBUG_MEDIA_NODE 0  


class JaminNode;     
class RosterHandler;       
extern JaminNode* gHaikuNode; 
status_t poll_thread_func(void* data); 



/* --- 1. INTERNAL JACK TYPES --- */

typedef void* jack_client_t;
typedef void* jack_port_t;
typedef uint32_t jack_nframes_t;

struct HijackSession {
    media_source source;
    media_destination original_dest;
    media_format format;
};

typedef struct { 
    uint32_t frame; 
    uint32_t frame_rate; 
} jack_position_t;

typedef struct {
    char *buf;
    size_t len;
} jack_ringbuffer_data_t;

/* 
 * CRITICAL: JACK1 MEMORY LAYOUT
 * Even though we don't touch these directly anymore, JAMin uses 
 * this implementation internally to talk between its IO and DSP threads.
 * It MUST match what JAMin expects.
 */
struct PrivateRingBuffer {
    char *buf;
    volatile size_t write_ptr;
    volatile size_t read_ptr;
    size_t size;
    size_t size_mask;
    int mlocked;
    int _pad; 
};

#define PRB(rb) ((struct PrivateRingBuffer*)(rb))



// Port Registry IDs
#define PORT_IN_L  1
#define PORT_IN_R  2
#define PORT_OUT_L 3
#define PORT_OUT_R 4



extern "C" int io_xrun(void *arg);

extern "C" {
    extern float gCurrentSampleRate;
    extern int32_t gCurrentBufferSize;
    extern void (*UpdateAudioParams_Ptr)(float rate, int32_t bufferSize);
    void RealUpdateAudioParams(float rate, int32_t bufferSize);
}


extern "C" void io_update_engine_rate(float rate);



/* --- RINGBUFFER IMPLEMENTATION (JACK1) --- */
extern "C" {
	
	// Haiku doesn't export this, so we provide a safe dummy
    int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy) {
        return 0; // Return success so JAMin continues
    }	
	
	/* --- GLOBALS --- */
	// Scratch Pads for Audio Processing (Float 32-bit)
	float* _scratch_in_L = NULL;
	float* _scratch_in_R = NULL;
	float* _scratch_out_L = NULL;
	float* _scratch_out_R = NULL;
	float* _scratch_dummy = NULL; // For unused ports
	
	//float gCurrentSampleRate = 192000.0f; 
	bigtime_t fDownstreamLatency;
	bigtime_t fCurrentPerformanceTime;	
	typedef struct jack_ringbuffer_t jack_ringbuffer_t; 
    extern jack_ringbuffer_t *in_rb[]; 
    extern jack_ringbuffer_t *out_rb[];
    
    // The Callback Hook
    static int (*global_process_callback)(jack_nframes_t, void*) = NULL;
    static void* global_process_arg = NULL;	
	void io_resync_engine(float new_rate); 	
	jack_ringbuffer_t* jack_ringbuffer_create(size_t sz) {
    	size_t power_of_two;
    	for (power_of_two = 1; 1u << power_of_two < sz; power_of_two++);
    	struct PrivateRingBuffer *rb = (struct PrivateRingBuffer *)calloc(1, sizeof(struct PrivateRingBuffer));
    	if (!rb) return NULL;
    	rb->size = 1 << power_of_two;
    	rb->size_mask = rb->size - 1;
    	rb->write_ptr = 0;
    	rb->read_ptr = 0;
    	rb->buf = (char*)malloc(rb->size);
    	rb->mlocked = 0;
    	return (jack_ringbuffer_t*)rb;
	}

	void jack_ringbuffer_free(jack_ringbuffer_t *rb) {
    	struct PrivateRingBuffer *prb = PRB(rb);
    	if (prb) { free(prb->buf); free(prb); }
	}

	void jack_ringbuffer_reset(jack_ringbuffer_t *rb) {
    	PRB(rb)->read_ptr = 0;
    	PRB(rb)->write_ptr = 0;
	}

	size_t jack_ringbuffer_read_space(const jack_ringbuffer_t *rb) {
    	struct PrivateRingBuffer *prb = PRB(rb);
    	size_t w = prb->write_ptr;
    	size_t r = prb->read_ptr;
    	if (w > r) return w - r;
    	return (w - r + prb->size) & prb->size_mask;
	}

	size_t jack_ringbuffer_write_space(const jack_ringbuffer_t *rb) {
    	struct PrivateRingBuffer *prb = PRB(rb);
    	size_t w = prb->write_ptr;
    	size_t r = prb->read_ptr;
    	if (w > r) return ((r - w + prb->size) & prb->size_mask) - 1;
    	else if (w < r) return (r - w) - 1;
    	return prb->size - 1;
	}

	size_t jack_ringbuffer_read(jack_ringbuffer_t *rb, char *dest, size_t cnt) {
    	struct PrivateRingBuffer *prb = PRB(rb);
    	size_t free_cnt = jack_ringbuffer_read_space(rb);
    	if (free_cnt == 0) return 0;
    	size_t to_read = cnt > free_cnt ? free_cnt : cnt;
    	size_t cnt2 = prb->read_ptr + to_read;
    	size_t n1, n2;
    	if (cnt2 > prb->size) {
        	n1 = prb->size - prb->read_ptr;
        	n2 = cnt2 & prb->size_mask;
    	} else {
        	n1 = to_read;
        	n2 = 0;
    	}
    	memcpy(dest, prb->buf + prb->read_ptr, n1);
    	prb->read_ptr = (prb->read_ptr + n1) & prb->size_mask;
    	if (n2) {
        	memcpy(dest + n1, prb->buf + prb->read_ptr, n2);
        	prb->read_ptr = (prb->read_ptr + n2) & prb->size_mask;
    	}
    	return to_read;
	}

	size_t jack_ringbuffer_write(jack_ringbuffer_t *rb, const char *src, size_t cnt) {
    	struct PrivateRingBuffer *prb = PRB(rb);
    	size_t free_cnt = jack_ringbuffer_write_space(rb);
    	if (free_cnt == 0) return 0;
    	size_t to_write = cnt > free_cnt ? free_cnt : cnt;
    	size_t cnt2 = prb->write_ptr + to_write;
    	size_t n1, n2;
    	if (cnt2 > prb->size) {
        	n1 = prb->size - prb->write_ptr;
        	n2 = cnt2 & prb->size_mask;
    	} else {
        	n1 = to_write;
        	n2 = 0;
    	}
    	memcpy(prb->buf + prb->write_ptr, src, n1);
    	prb->write_ptr = (prb->write_ptr + n1) & prb->size_mask;
    	if (n2) {
        	memcpy(prb->buf + prb->write_ptr, src + n1, n2);
       	 	prb->write_ptr = (prb->write_ptr + n2) & prb->size_mask;
    	}
    	return to_write;
	}

	void jack_ringbuffer_read_advance(jack_ringbuffer_t *rb, size_t cnt) {
    	struct PrivateRingBuffer *prb = PRB(rb);
    	prb->read_ptr = (prb->read_ptr + cnt) & prb->size_mask;
	}

	void jack_ringbuffer_write_advance(jack_ringbuffer_t *rb, size_t cnt) {
    	struct PrivateRingBuffer *prb = PRB(rb);
    	prb->write_ptr = (prb->write_ptr + cnt) & prb->size_mask;
	}

	void jack_ringbuffer_get_read_vector(const jack_ringbuffer_t *rb, jack_ringbuffer_data_t *vec) {
    	struct PrivateRingBuffer *prb = PRB(rb);
    	size_t w = prb->write_ptr;
    	size_t r = prb->read_ptr;
    	size_t free_cnt = (w > r) ? w - r : ((w - r + prb->size) & prb->size_mask);
    	size_t cnt2 = r + free_cnt;
    	if (cnt2 > prb->size) {
        	vec[0].buf = prb->buf + r;
        	vec[0].len = prb->size - r;
        	vec[1].buf = prb->buf;
        	vec[1].len = cnt2 & prb->size_mask;
    	} else {
        	vec[0].buf = prb->buf + r;
        	vec[0].len = free_cnt;
        	vec[1].len = 0;
    	}
	}

	void jack_ringbuffer_get_write_vector(const jack_ringbuffer_t *rb, jack_ringbuffer_data_t *vec) {
    	struct PrivateRingBuffer *prb = PRB(rb);
    	size_t w = prb->write_ptr;
    	size_t r = prb->read_ptr;
    	size_t free_cnt = (w > r) ? ((r - w + prb->size) & prb->size_mask) - 1 : ((w < r) ? (r - w) - 1 : prb->size - 1);
    	size_t cnt2 = w + free_cnt;
    	if (cnt2 > prb->size) {
        	vec[0].buf = prb->buf + w;
        	vec[0].len = prb->size - w;
        	vec[1].buf = prb->buf;
        	vec[1].len = cnt2 & prb->size_mask;
    	} else {
        	vec[0].buf = prb->buf + w;
        	vec[0].len = free_cnt;
        	vec[1].len = 0;
    	}
	}

    jack_port_t jack_port_register(jack_client_t c, const char* n, const char* t, unsigned long f, unsigned long b) {
        static int port_count = 0; 
        port_count++;
        fprintf(stderr, "[JAMin-Server] Registered Port %d: %s\n", port_count, n);
        return (jack_port_t)(size_t)port_count;
    }

    void jack_set_process_callback(jack_client_t c, int (*cb)(jack_nframes_t, void*), void* arg) {
        fprintf(stderr, "[JAMin-Server] Process Callback Hooked!\n");
        global_process_callback = cb;
        global_process_arg = arg;
    } 

    // Boilerplate stubs
    void *l_notebook1 = NULL;
    int jack_disconnect(jack_client_t c, const char *a, const char *b) { return 0; }
    int jack_connect(jack_client_t c, const char *a, const char *b) { return 0; }
    const char* jack_port_name(const jack_port_t p) { return "stub_port"; }
    const char* jack_port_short_name(const jack_port_t p) { return "stub"; }
    int jack_port_flags(const jack_port_t p) { return 0; }
    int jack_port_connected_to(const jack_port_t p, const char *pn) { return 0; }
    int jack_port_connected(const jack_port_t p) { return 0; }
    int jack_client_close(jack_client_t c) { return 0; }
    int jack_activate(jack_client_t c) { return 0; } 
    const char* jack_get_client_name(jack_client_t c) { return "JAMin-Haiku"; }
    uint32_t jack_get_sample_rate(jack_client_t c) {  return (uint32_t)gCurrentSampleRate;   }
    jack_nframes_t jack_get_buffer_size(jack_client_t c) {  return (jack_nframes_t)gCurrentBufferSize; } 
    void jack_on_shutdown(jack_client_t c, void (*cb)(void*), void* arg) {}
    int jack_set_xrun_callback(jack_client_t c, int (*cb)(void*), void* arg) { return 0; }
    int jack_set_sample_rate_callback(jack_client_t c, int (*cb)(jack_nframes_t, void*), void* arg) { return 0; }
    int jack_set_buffer_size_callback(jack_client_t c, int (*cb)(jack_nframes_t, void*), void* arg) { return 0; }
    int jack_transport_locate(jack_client_t c, jack_nframes_t f) { return 0; }
    jack_nframes_t jack_frame_time(jack_client_t c) { return system_time(); }
    int jack_transport_query(jack_client_t c, jack_position_t *pos) { return 0; }
    void jack_transport_start(jack_client_t c) {}
    void jack_transport_stop(jack_client_t c) {}
    float jack_cpu_load(jack_client_t c) { return 0.0f; }
    void jack_port_set_latency(jack_port_t p, jack_nframes_t n) {}
    const char **jack_get_ports(jack_client_t c, const char *p, const char *t, unsigned long f) { return NULL; }
   
   pthread_t jack_client_thread_id(void* client) {
        return pthread_self(); 
    }

    int pthread_getschedparam(pthread_t thread, int *policy, struct sched_param *param) {
        if (policy) *policy = 0;
        if (param) param->sched_priority = 0;
        return 0; 
    }

    int jack_is_realtime(void* client) {
        return 0; 
    }
    
     void io_resync_engine(float new_rate) { }   
    
} // End extern "C"









/* --- 2. THE CONSOLIDATED NODE --- */
class JaminNode : public BBufferConsumer, public BBufferProducer, public BMediaEventLooper {
public:
    media_output fOutput;
    media_input  fInput;
    BBufferGroup* fBufferGroup;
	RosterHandler* fHandler;
	BLocker fLock;
	void RestoreAll();
	
    JaminNode() : BMediaNode("JAMin-Haiku"), BBufferConsumer(B_MEDIA_RAW_AUDIO), 
                  BBufferProducer(B_MEDIA_RAW_AUDIO), BMediaEventLooper() {
        fBufferGroup = NULL;
        fHandler = NULL;
		AddNodeKind(B_BUFFER_CONSUMER | B_BUFFER_PRODUCER | B_CONTROLLABLE | B_PHYSICAL_OUTPUT | B_PHYSICAL_INPUT | B_SYSTEM_MIXER);

	
	    if (_scratch_in_L == NULL) {
        _scratch_in_L = (float*)malloc(8192 * sizeof(float));
        _scratch_in_R = (float*)malloc(8192 * sizeof(float));
        _scratch_out_L = (float*)malloc(8192 * sizeof(float));
        _scratch_out_R = (float*)malloc(8192 * sizeof(float));
        _scratch_dummy = (float*)malloc(8192 * sizeof(float));
        
        // Zero them out to prevent loud noise on startup
        memset(_scratch_in_L, 0, 8192 * sizeof(float));
        memset(_scratch_in_R, 0, 8192 * sizeof(float));
        memset(_scratch_out_L, 0, 8192 * sizeof(float));
        memset(_scratch_out_R, 0, 8192 * sizeof(float));
        memset(_scratch_dummy, 0, 8192 * sizeof(float));
    }
	fBufferGroup = NULL;
    fHandler = NULL;
    gHaikuNode = this;	
	float currentRate = gCurrentSampleRate; 

    // 3. Define the Audio Format (Crucial to prevent "Unknown" status)
    media_format format;
    format.type = B_MEDIA_RAW_AUDIO;
    format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format.u.raw_audio.frame_rate = gCurrentSampleRate;
    format.u.raw_audio.channel_count = 2;
    format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
    //format.u.raw_audio.buffer_size = 8192; // 1024 frames * 2 ch * 4 bytes
    format.u.raw_audio.buffer_size = gCurrentBufferSize * 2 * sizeof(float);


    // 4. Assign the format to our Input and Output ports
    fInput.format = format;
    fOutput.format = format;

    // 5. Port Initialization
    fOutput.node = fInput.node = Node();
    fOutput.source.port = fInput.destination.port = ControlPort();
    fOutput.source.id = 0;
    fInput.destination.id = 0;
    fOutput.destination = media_destination::null;
    fInput.source = media_source::null;
    
    sprintf(fOutput.name, "JAMin Out");
    sprintf(fInput.name, "JAMin In");

    // 6. Fix the "(unknown run mode)" crash
    this->SetRunMode(BMediaNode::B_RECORDING);
    this->SetTimeSource(NULL);    
    
    // --- THE FIX: ALLOCATE THE GROUP IMMEDIATELY ---
    // Instead of just setting it to NULL, create the object here.
    // This ensures that even if connection fails, the pointer is valid.
    fBufferGroup = new BBufferGroup(65536, 16); 
    

    
    if (fBufferGroup->InitCheck() != B_OK) {  
    	fprintf(stderr, "[JAMin-Error] Failed to pre-allocate BufferGroup!\n");
        delete fBufferGroup;
        fBufferGroup = NULL;
    	} else { fprintf(stderr, "[JAMin-Debug] BufferGroup pre-allocated safely.\n"); }         
    }

	virtual ~JaminNode(); 


    /* --- MANDATORY PRODUCER METHODS --- */
    virtual status_t SetBufferGroup(const media_source& forSource, BBufferGroup* group) override {
    	// This allows downstream nodes (like the Mixer) to provide their own 
    	// memory buffers to us for zero-copy processing.
    	fBufferGroup = group;
    	return B_OK;
	}

virtual status_t HandleMessage(int32 code, const void* data, size_t size) override {
    // Check if the looper can handle it first
    if (BMediaEventLooper::HandleMessage(code, data, size) == B_OK) return B_OK;
    // Then check the consumer/producer parts
    if (BBufferConsumer::HandleMessage(code, data, size) == B_OK) return B_OK;
    if (BBufferProducer::HandleMessage(code, data, size) == B_OK) return B_OK;
    
    return B_ERROR;
}
    
    
	void PublishLatency(bigtime_t total_us) {
    	fCurrentLatency = total_us;
    	// Tell the system our latency changed
    	SendLatencyChange(fOutput.source, fOutput.destination, fCurrentLatency + 10000);
	}

	virtual status_t GetNextInput(int32* cookie, media_input* out_input) override {
    // Only one input (cookie 0)
    if (*cookie != 0) return B_BAD_INDEX;

    // Ensure the input structure is fully described for the Roster
    out_input->node = Node();
    out_input->source = fInput.source;
    out_input->destination = fInput.destination;
    out_input->format = fInput.format;
    strcpy(out_input->name, "JAMin In");

    *cookie = 1;
    return B_OK;
	}

	virtual status_t GetNextOutput(int32* cookie, media_output* out_output) override {
    // Only one output (cookie 0)
    if (*cookie != 0) return B_BAD_INDEX;

    // Ensure the output structure is fully described
    out_output->node = Node();
    out_output->source = fOutput.source;
    out_output->destination = fOutput.destination;
    out_output->format = fOutput.format;
    strcpy(out_output->name, "JAMin Out");

    *cookie = 1;
    return B_OK;
	}


    virtual status_t FormatProposal(const media_source& output, media_format* format) override {
        *format = fInput.format; return B_OK; 
    }
    virtual status_t FormatChangeRequested(const media_source&, const media_destination&, media_format*, int32*) override { return B_ERROR; }
    virtual status_t DisposeOutputCookie(int32 cookie) override { return B_OK; }
    virtual status_t PrepareToConnect(const media_source& what, const media_destination& where, media_format* format, media_source* out_source, char* name) override {
        *out_source = fOutput.source;
        strcpy(name, fOutput.name);
        format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
        return B_OK;
    }
virtual void Connect(status_t error, const media_source& s, const media_destination& d, const media_format& f, char* name) override {
    if (!error) { 
        fOutput.destination = d; 
        fOutput.format = f; 
        
        // Create a much larger buffer group to prevent "RequestBuffer FAILED"
        if (fBufferGroup == NULL) {
            // INCREASED: 64KB per buffer (to handle high rates/bit depth)
            // INCREASED: 32 buffers total (to provide a stable processing pool)
            fBufferGroup = new BBufferGroup(65536, 64); 
            
            if (fBufferGroup->InitCheck() != B_OK) {
                fprintf(stderr, "[JAMin-Error] Failed to initialize BBufferGroup!\n");
                delete fBufferGroup;
                fBufferGroup = NULL;
            } else {
                fprintf(stderr, "[JAMin-Debug] Buffer Group initialized: 32 buffers @ 64KB.\n");
            }
        }
    }
}


    virtual void Disconnect(const media_source& what, const media_destination& where) override {
        if (what == fOutput.source) fOutput.destination = media_destination::null;
    }
    virtual void LateNoticeReceived(const media_source&, bigtime_t, bigtime_t) override {}
    virtual void EnableOutput(const media_source&, bool, int32*) override {}

    /* --- MANDATORY CONSUMER METHODS --- */
virtual status_t AcceptFormat(const media_destination& dest, media_format* format) override {
    if (format->type != B_MEDIA_NO_TYPE && format->type != B_MEDIA_RAW_AUDIO) 
        return B_MEDIA_BAD_FORMAT;

    format->type = B_MEDIA_RAW_AUDIO;

    // 1. Handle Rate (Suggest 44.1k if unknown)
    if (format->u.raw_audio.frame_rate == media_raw_audio_format::wildcard.frame_rate) {
        format->u.raw_audio.frame_rate = 44100.0f; 
    }

    // 2. Handle Format (THE FIX)
    // Instead of rejecting Int16, we ACCEPT it. We will handle conversion internally.
    if (format->u.raw_audio.format == media_raw_audio_format::wildcard.format) {
        format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
    }
    // Note: We now implicitly return B_OK for B_AUDIO_SHORT (Int16) too.

    // 3. Handle Channels
    if (format->u.raw_audio.channel_count == media_raw_audio_format::wildcard.channel_count) {
        format->u.raw_audio.channel_count = 2;
    }

    return B_OK;
}



    virtual void DisposeInputCookie(int32) override {}
    virtual void ProducerDataStatus(const media_destination&, int32, bigtime_t) override {}
    virtual status_t GetLatencyFor(const media_destination&, bigtime_t* l, media_node_id* ts) override {
        //*l = fDownstreamLatency + 10000;
        *l = fCurrentLatency + 10000; 
        if (TimeSource()) *ts = TimeSource()->ID();
        return B_OK;
    }
    
virtual status_t Connected(const media_source& prod, const media_destination& where, 
                           const media_format& format, media_input* out_input) override {
    
    fInput.source = prod;
    fInput.format = format; 
    *out_input = fInput;

    const char* formatName = "UNKNOWN";
    if (format.u.raw_audio.format == media_raw_audio_format::B_AUDIO_FLOAT) formatName = "FLOAT";
    else if (format.u.raw_audio.format == media_raw_audio_format::B_AUDIO_SHORT) formatName = "INT16";
    else if (format.u.raw_audio.format == media_raw_audio_format::B_AUDIO_INT) formatName = "INT32";

    fprintf(stderr, "[JAMin-Connect] Connection established! Rate: %.0f Hz | Format: %s\n", 
            fInput.format.u.raw_audio.frame_rate, formatName);

    size_t maxBufferSize = 8192 * 2 * sizeof(float);

    if (fBufferGroup) delete fBufferGroup;
    fBufferGroup = new BBufferGroup(maxBufferSize, 8);
    
    if (fBufferGroup->InitCheck() < B_OK) {
        fprintf(stderr, "[JAMin-Error] BufferGroup Init Failed!\n");
        return B_ERROR;
    }

    // --- THE CRASH FIX ---
    // Use the source (prod) that just connected to us.
    this->SetBufferGroup(prod, fBufferGroup);


    return B_OK;
}




virtual void Disconnected(const media_source& prod, const media_destination& where) override {
    BMediaRoster* roster = BMediaRoster::Roster();
    live_node_info info;

    // 1. Identify who is leaving using the port ID
    media_node sourceNode;
    if (roster && roster->GetNodeFor(prod.port, &sourceNode) == B_OK) {
        if (roster->GetLiveNodeInfo(sourceNode, &info) == B_OK) {
            fprintf(stderr, "[JAMin-Info] Audio source DISCONNECTED: %s\n", info.name);
        } else {
            fprintf(stderr, "[JAMin-Info] Audio source (Node %" B_PRId32 ") has left.\n", sourceNode.node);
        }
    } else {
        fprintf(stderr, "[JAMin-Info] Audio source (Port %" B_PRId32 ") has left.\n", prod.port);
    }

    // 2. Reset the input source
    fInput.source = media_source::null;

    // 3. SAFE CLEANUP
    BAutolock locker(fLock);
    if (fBufferGroup) {
        delete fBufferGroup;
        fBufferGroup = NULL;
    }
}




    virtual status_t FormatChanged(const media_source& producer, const media_destination& consumer, 
                               int32 change_tag, const media_format& format) override {
    // If the Mixer (consumer) changes format, update our output record
    if (consumer == fOutput.destination) {
        fOutput.format = format;
        fprintf(stderr, "[JAMin-System] Format Changed to %.0fHz\n", 
                fOutput.format.u.raw_audio.frame_rate);
    }
    return B_OK;
}


    /* --- CORE LOOPER METHODS --- */
virtual void HandleEvent(const media_timed_event* event, bigtime_t lateness, bool realTime) override {
    switch (event->type) {
        case BTimedEventQueue::B_HANDLE_BUFFER:
            // Process audio buffers only
            BufferReceived((BBuffer*)event->pointer);
            break;
        default:
            break;
    }
}


void ScanAndHijack() {	
    BMediaRoster* roster = BMediaRoster::Roster();
    live_node_info nodes[64];
    int32 count = 64;

    if (roster->GetLiveNodes(nodes, &count) == B_OK) {
        for (int i = 0; i < count; i++) {
            // 1. Skip ourselves and the system nodes
            if (nodes[i].node.node == Node().node) continue;
            if (strstr(nodes[i].name, "Mixer") || strstr(nodes[i].name, "HD Audio")) continue;

            // 2. We only care about Producers (things making sound)
            if (nodes[i].node.kind & B_BUFFER_PRODUCER) {
                media_output out[8]; // Array for multiple outputs
                int32 outCount = 0;
                
                // Get all outputs for the app (using the correct function name)
                if (roster->GetAllOutputsFor(nodes[i].node, out, 8, &outCount) == B_OK) {
                    for (int j = 0; j < outCount; j++) {
                        
                        // 3. Is it connected to something that ISN'T our input?
                        if (out[j].destination != media_destination::null && 
                            out[j].destination.id != fInput.destination.id) {
                            
                            fprintf(stderr, "[JAMin-Aggressive] Hijacking %s (%s)...\n", nodes[i].name, out[j].name);
                            
                            
                                                      // --- SAVE SESSION HERE ---
                            HijackSession session;
                            session.source = out[j].source;
                            session.original_dest = out[j].destination;
                            session.format = out[j].format;
                            fHijackedNodes[nodes[i].node.node] = session; // Use node ID as key
                            
                            fprintf(stderr, "[JAMin-Debug] Session saved for %s during Scan. Map size: %zu\n", 
                                    nodes[i].name, fHijackedNodes.size());
                            // -------------------------  
                            
                            
                            
                            
                            // A. Snip the existing cable
                            roster->Disconnect(out[j].node.node, out[j].source, 
                                             out[j].destination.port, out[j].destination);
                            
                            // B. Plug it into JAMin
                            status_t err = roster->Connect(out[j].source, fInput.destination, 
                                                         &out[j].format, &out[j], &fInput);
                            
                            if (err == B_OK) {
                                fprintf(stderr, "[JAMin-Aggressive] SUCCESS: %s is now routed to JAMin.\n", nodes[i].name);
                            }
                        }
                    }
                }
            }
        }
    }
}



    
void HijackNode(media_node_id id) {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster) return;

    // 1. SETTLING DELAY: Let the newly created node finish its own 
    // internal connection to the Mixer before we interfere.
    snooze(50000); 

    media_node newNode;
    if (roster->GetNodeFor(id, &newNode) == B_OK) {
        if (newNode.node == Node().node || (newNode.kind & B_SYSTEM_MIXER)) return;

        live_node_info info;
        if (roster->GetLiveNodeInfo(newNode, &info) == B_OK) {
            fprintf(stderr, "[JAMin-Hijack] Inspecting: '%s'\n", info.name);

            if (newNode.kind & B_BUFFER_PRODUCER) {
                media_output out[8]; 
                int32 count = 0;
                
                if (roster->GetAllOutputsFor(newNode, out, 8, &count) == B_OK && count > 0) {
                    for (int j = 0; j < count; j++) {
                        if (out[j].destination.id != fInput.destination.id) {
                            
                            fprintf(stderr, "[JAMin-Hijack] Found app: %s. Attempting hijack...\n", info.name);
                       
	HijackSession session;
session.source = out[j].source;
session.original_dest = out[j].destination; 
session.format = out[j].format;

// Use the actual node ID from the newNode structure
fHijackedNodes[newNode.node] = session; 

fprintf(stderr, "[JAMin-Debug] SESSION SAVED for node %d. Map size: %zu\n", 
        (int)newNode.node, fHijackedNodes.size());
                            
                            // 2. STOP: Force the node to stop asking for buffers.
                            // The 'true' makes it synchronous, preventing the null-pointer crash.
                            roster->StopNode(newNode, 0, true);
                            snooze(20000); 

                            // 3. SNIP: Disconnect existing cable
                            if (out[j].destination != media_destination::null) {
                                roster->Disconnect(out[j].node.node, out[j].source, 
                                                 out[j].destination.port, out[j].destination);
                                
                                // Give the Media Server time to release port ownership
                                snooze(50000); 
                            }

                            // 4. PLUG: Force connection to JAMin
                            media_format tryFormat = fInput.format;
                            status_t err = roster->Connect(out[j].source, fInput.destination, 
                                                         &tryFormat, &out[j], &fInput);                        
                            
                            
                            if (err == B_OK) {
                                // 5. START: Resume audio flow through the new pipe
                                roster->StartNode(newNode, 0); 
                                fprintf(stderr, "[JAMin-Hijack] SUCCESS: %s is now ours.\n", info.name);
                                break; 
                            } else {
                                // Fallback: restart it anyway so the app doesn't stay frozen
                                roster->StartNode(newNode, 0);
                                fprintf(stderr, "[JAMin-Hijack] FAILED: %s (Err: %s)\n", info.name, strerror(err));
                            }
                        }
                    }
                }
            }
        }
    }
}


   
    virtual BMediaAddOn* AddOn(int32* id) const override { return NULL; }
    
    





virtual void BufferReceived(BBuffer* b) override {
    // --- THE SAFETY LOCK ---
    BAutolock locker(fLock);
    if (!b) return;

    media_header* inHdr = b->Header();
    bigtime_t systemNow = system_time(); 

    // --- XRUN TIMING DETECTION ---
    if (TimeSource()) {
        bigtime_t performanceNow = TimeSource()->Now();
        // Check if the buffer's scheduled time is already in the past.
        // We use a 20ms (20,000us) grace period to ignore minor system jitter.
        if (performanceNow > (inHdr->start_time + 20000)) {
             io_xrun(NULL); 
        }
    }


    if (fBufferGroup == NULL || fInput.source == media_source::null) {
        b->Recycle();
        return;
    }

    static int bCount = 0;
    static bigtime_t nextSchedTime = 0;
    static double phaseAcc = 0.0; 
    static size_t lastInFrames = 0;
    static bigtime_t lastInHdrTime = 0;
    static float sensedInRate = 44100.0f; 
    static bigtime_t lastDebugTime = 0; 
    
    bCount++;
    bool shouldDebug = (DEBUG_MEDIA_NODE && (systemNow - lastDebugTime >= 1000000));

    // 1. DATA VALIDATION
    void* rawData = b->Data();
    size_t sizeUsed = b->SizeUsed();

    if (!rawData || sizeUsed == 0) {
        b->Recycle();
        return;
    }

    // 2. FORMAT DETECTION & CONVERSION
    uint32 inChannels = fInput.format.u.raw_audio.channel_count;
    if (inChannels < 1 || inChannels > 16) inChannels = 2;
    
    static float _conv_buffer[8192 * 16]; 
    
    uint32 format = fInput.format.u.raw_audio.format;
    size_t inFrames = 0;
    float* inData = NULL;

    if (format == media_raw_audio_format::B_AUDIO_SHORT) {
        int16* shortData = (int16*)rawData;
        inFrames = sizeUsed / (inChannels * sizeof(int16));
        if (inFrames > 8192) inFrames = 8192;
        
        for (size_t i = 0; i < inFrames * inChannels; i++) {
            _conv_buffer[i] = (float)shortData[i] * (1.0f / 32768.0f);
        }
        inData = _conv_buffer;
    } else if (format == media_raw_audio_format::B_AUDIO_INT) {
        int32* intData = (int32*)rawData;
        inFrames = sizeUsed / (inChannels * sizeof(int32));
        if (inFrames > 8192) inFrames = 8192;
        
        for (size_t i = 0; i < inFrames * inChannels; i++) {
            _conv_buffer[i] = (float)intData[i] * (1.0f / 2147483648.0f);
        }
        inData = _conv_buffer;
    } else {
        // Assume Float
        inData = (float*)rawData;
        inFrames = sizeUsed / (inChannels * sizeof(float));
    }

    if (inFrames == 0 || inFrames > 8192) {
        b->Recycle();
        return;
    }

    // --- HAZMAT: INPUT SANITIZER & NORMALIZER ---
    size_t totalSamples = inFrames * inChannels;
    float maxSample = 0.0f;
    
    for (size_t k = 0; k < totalSamples; k++) {
        if (!std::isfinite(inData[k])) {
            inData[k] = 0.0f;
        } else {
            float absVal = fabsf(inData[k]);
            if (absVal > maxSample) maxSample = absVal;
        }
    }

    if (maxSample > 1000000.0f) {
        for (size_t k = 0; k < totalSamples; k++) inData[k] = 0.0f;
        if (shouldDebug) fprintf(stderr, "[BufferReceived] ERROR: Garbage detected (%e). Muting.\n", maxSample);
    } else if (maxSample > 2.0f) {
        float scaler = 1.0f / maxSample;
        for (size_t k = 0; k < totalSamples; k++) inData[k] *= scaler;
        if (shouldDebug) fprintf(stderr, "[BufferReceived] WARNING: Input loud (%.1f). Scaling.\n", maxSample);
    }

    // 3. RATE DISCOVERY
    float inRate = fInput.format.u.raw_audio.frame_rate;
    if (inRate < 100.0f) inRate = 44100.0f;
       
    float outRate = fOutput.format.u.raw_audio.frame_rate;
    if (outRate < 100.0f) outRate = 192000.0f;

    if (lastInHdrTime > 0 && inHdr->start_time > lastInHdrTime) {
        bigtime_t delta = inHdr->start_time - lastInHdrTime;
        if (delta > 1000) { 
            float currentRate = (float)((double)inFrames * 1000000.0 / (double)delta);
            if (currentRate > 40000 && currentRate < 205000) {
                sensedInRate = (sensedInRate * 0.95f) + (currentRate * 0.05f);
            }
        }
    }
    lastInHdrTime = inHdr->start_time;

    float activeInRate;
    if (sensedInRate > 144000.0f)      activeInRate = 192000.0f;
    else if (sensedInRate > 72000.0f) activeInRate = 96000.0f;
    else if (sensedInRate > 46500.0f) activeInRate = 48000.0f;
    else                              activeInRate = 44100.0f;

    static float lastActiveRate = 0.0f;
    if (activeInRate != lastActiveRate) {
         io_update_engine_rate(activeInRate);
         lastActiveRate = activeInRate;
         if (shouldDebug) fprintf(stderr, "[BufferReceived] ENGINE UPDATED TO: %.0f Hz\n", activeInRate);
    }

    if (shouldDebug) {
        fprintf(stderr, "[BufferReceived] Sensed: %.2f Hz -> Selected: %.0f Hz\n", 
                sensedInRate, activeInRate);
    }

    // 4. CADENCE RESET
    if (inFrames != lastInFrames) {
        nextSchedTime = 0; 
        phaseAcc = 0.0; 
        lastInFrames = inFrames;
        memset(_scratch_in_L, 0, 8192 * sizeof(float));
        memset(_scratch_in_R, 0, 8192 * sizeof(float));
    }

    // 5. TARGET FRAMES & BUFFER CLAMP
    size_t outFrames = (size_t)((double)inFrames * ((double)outRate / (double)activeInRate));
    if (outFrames > 8192) outFrames = 8192;
    if (outFrames == 0) outFrames = 1;

    // 6. HERMITE RESAMPLER
    float step = (float)inFrames / (float)outFrames;

    if (!std::isfinite(step) || step <= 0.0f) {
        memset(_scratch_in_L, 0, 8192 * sizeof(float));
        memset(_scratch_in_R, 0, 8192 * sizeof(float));
        phaseAcc = 0.0;
    } else {
        if (!std::isfinite(phaseAcc) || phaseAcc < 0) phaseAcc = 0;

        for (size_t i = 0; i < outFrames; i++) {
            int i1 = (int)phaseAcc;
            float f = (float)(phaseAcc - i1);
            int im1 = (i1 > 0) ? i1 - 1 : 0;
            int i2 = (i1 < (int)inFrames - 1) ? i1 + 1 : i1;
            int i3 = (i1 < (int)inFrames - 2) ? i1 + 2 : i2;

            for (uint32 ch = 0; ch < 2; ch++) {
                float* dest = (ch == 0) ? _scratch_in_L : _scratch_in_R;
                uint32 off = (inChannels == 2) ? ch : 0;
                float y0 = inData[im1 * inChannels + off];
                float y1 = inData[i1 * inChannels + off];
                float y2 = inData[i2 * inChannels + off];
                float y3 = inData[i3 * inChannels + off];

                float c = (y2 - y0) * 0.5f;
                float v = y1 - y2;
                float w = c + v;
                float a = w + v + (y3 - y2) * 0.5f;
                float b = w + a;
                dest[i] = ((((a * f) - b) * f) + c) * f + y1;
            }
            phaseAcc += step;
        }
        phaseAcc -= (float)inFrames; 
    }

    for (size_t i = 0; i < outFrames; i++) {
        if (!std::isfinite(_scratch_in_L[i])) _scratch_in_L[i] = 0.0f;
        if (!std::isfinite(_scratch_in_R[i])) _scratch_in_R[i] = 0.0f;
    }

    // 7. ENGINE CALL
    if (global_process_callback) {
        global_process_callback(outFrames, global_process_arg);
    } else {
        memcpy(_scratch_out_L, _scratch_in_L, outFrames * sizeof(float));
        memcpy(_scratch_out_R, _scratch_in_R, outFrames * sizeof(float));
    }

    // 8. DISPATCH
    if (fOutput.destination != media_destination::null && fBufferGroup != NULL) {
        size_t outSize = outFrames * 2 * sizeof(float);
        
        if (shouldDebug) fprintf(stderr, "[BufferReceived] Req: %zu | ", outSize);

        BBuffer* outBuffer = fBufferGroup->RequestBuffer(outSize, 10000);
        if (outBuffer != NULL) {
            float* outData = (float*)outBuffer->Data();
            if (outData) {
                for (size_t i = 0; i < outFrames; i++) {
                    float left = _scratch_out_L[i];
                    float right = _scratch_out_R[i];
                    outData[i * 2]     = std::isfinite(left) ? left : 0.0f;
                    outData[i * 2 + 1] = std::isfinite(right) ? right : 0.0f;
                }

                media_header* outHdr = outBuffer->Header();
                outHdr->type = B_MEDIA_RAW_AUDIO;
                outHdr->size_used = outSize;

                bigtime_t actualDuration = (bigtime_t)((double)outFrames * 1000000.0 / (double)outRate);
                if (nextSchedTime == 0 || (nextSchedTime < (inHdr->start_time - 100000))) {
                    nextSchedTime = inHdr->start_time + 100000; 
                }

                outHdr->start_time = nextSchedTime;
                nextSchedTime += actualDuration; 

                if (shouldDebug) {
                    fprintf(stderr, "Sel: %p (%zu) | Final: %zu\n", outBuffer, outBuffer->SizeAvailable(), outHdr->size_used);
                    lastDebugTime = systemNow; 
                }

                SendBuffer(outBuffer, fOutput.source, fOutput.destination);
            }
        }
    }
    b->Recycle();
}





    
    virtual status_t FormatSuggestionRequested(media_type type, int32 quality, media_format* format) override {
    	if (type != B_MEDIA_RAW_AUDIO) return B_MEDIA_BAD_FORMAT;
    
    	format->type = B_MEDIA_RAW_AUDIO;
    	format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
    	format->u.raw_audio.channel_count = 2;
    	format->u.raw_audio.frame_rate = gCurrentSampleRate;
    	format->u.raw_audio.buffer_size = 8192; // 1024 frames * 2 ch * 4 bytes
    	return B_OK;
	}

    
virtual void NodeRegistered() override {
    this->SetRunMode(BMediaNode::B_RECORDING); 
    Run(); 

    // Start a native Haiku thread for the hijacker
    thread_id poll_thread = spawn_thread(poll_thread_func, "JaminHijackPoll", B_LOW_PRIORITY, this);
    if (poll_thread >= 0) {
        resume_thread(poll_thread);
        fprintf(stderr, "[JAMin-Debug] Native Polling Thread started.\n");
    }
}

void SetSampleRate(float rate) {
    BAutolock locker(fLock);
    fInput.format.u.raw_audio.frame_rate = rate;
    fOutput.format.u.raw_audio.frame_rate = rate;
}







    
private:
bigtime_t fCurrentLatency; 
bigtime_t fNextPollTime;
std::map<media_node_id, HijackSession> fHijackedNodes;   

};

status_t poll_thread_func(void* data) {
    JaminNode* node = (JaminNode*)data;
    while (true) {
        // Sleep for 2 seconds
        snooze(2000000); 
        
        // Check if the node is still alive and then scan
        if (gHaikuNode) {
            gHaikuNode->ScanAndHijack();
        } else {
            break; // Exit thread if node is gone
        }
    }
    return B_OK;
}


/* --- UPDATED JACK API STUB --- */
extern "C" void *jack_port_get_buffer(jack_port_t port, jack_nframes_t nframes) {
    // Make sure this is the ONLY definition of this function in the file
    size_t id = (size_t)port;
    // Logic to return _scratch pads...
    switch(id) {
        case PORT_IN_L: return (void*)_scratch_in_L;
        case PORT_IN_R: return (void*)_scratch_in_R;
        case PORT_OUT_L: return (void*)_scratch_out_L;
        case PORT_OUT_R: return (void*)_scratch_out_R;
        default: return (void*)_scratch_dummy;
    }
}
extern "C" {
 	
    void haiku_update_node_latency(bigtime_t total_us) {
        if (gHaikuNode) {
            gHaikuNode->PublishLatency(total_us);
        }
    } 
} 

extern "C" void haiku_shutdown_node() {
    if (gHaikuNode == NULL) return;

    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster) return;

    media_node myNode = gHaikuNode->Node();
    

    gHaikuNode->RestoreAll();
    

    snooze(250000); 

    // 1. BREAK THE CONNECTIONS (The Fix for the Ghost Entry)
    media_input connectedInput;
    int32 inputCount = 0;
    if (roster->GetConnectedInputsFor(myNode, &connectedInput, 1, &inputCount) == B_OK && inputCount > 0) {
        roster->Disconnect(connectedInput.node.node, connectedInput.source, 
                          myNode.node, connectedInput.destination);
    }

    media_output connectedOutput;
    int32 outputCount = 0;
    if (roster->GetConnectedOutputsFor(myNode, &connectedOutput, 1, &outputCount) == B_OK && outputCount > 0) {
        roster->Disconnect(myNode.node, connectedOutput.source, 
                          connectedOutput.node.node, connectedOutput.destination);
    }

    // 2. STOP AND RELEASE
    roster->StopNode(myNode, 0, true);
    
    // This is vital: Release() triggers the internal cleanup and eventual delete
    gHaikuNode->Release(); 
    gHaikuNode = NULL;
}



JaminNode* gHaikuNode = NULL; 

class JaminRosterListener : public BHandler {
public:
    JaminRosterListener() : BHandler("JaminRosterListener") {}
    virtual void MessageReceived(BMessage* msg) override {
        // Log every single message code that hits this specific handler
        fprintf(stderr, "[JAMin-Roster] RECEIVED: 0x%08lx\n", msg->what);

        // 0x40000000 = Node Created, 0x40000002 = Node Started
        if (msg->what == 0x40000000 || msg->what == 0x40000002) {
            media_node_id nid;
            if (msg->FindInt32("node", &nid) == B_OK) {
                fprintf(stderr, "[JAMin-Roster] >>> HIJACK TRIGGER: Node %d\n", (int)nid);
                if (gHaikuNode) gHaikuNode->HijackNode(nid);
            }
        }
    }
};


class RosterHandler : public BHandler {
public:
    RosterHandler(JaminNode* node) : BHandler("JaminRosterHandler"), fNode(node) {}
    
virtual void MessageReceived(BMessage* msg) override {
    // Log EVERY message that hits the handler to confirm it's working
    fprintf(stderr, "[JAMin-Roster] Handler received code: 0x%lx\n", msg->what);

    // 0x40000000 is B_MEDIA_NODE_CREATED
    // 0x40000002 is B_MEDIA_NODE_DELETED (or often sent during resets)
    if (msg->what == 0x40000000 || msg->what == 0x40000002) {
        media_node_id nid;
        if (msg->FindInt32("node", &nid) == B_OK) {
            fprintf(stderr, "[JAMin-Roster] HIJACK TRIGGER: Node %d\n", (int)nid);
            
            // This is the line that might trigger the "incomplete type" error
            // If it does, just move this whole RosterHandler block below JaminNode
            fNode->HijackNode(nid); 
        }
    }
}

private:
    JaminNode* fNode;
};


/* --- 1. Helper function for the message bridge --- */
void forward_media_message_to_node(BMessage* msg) {
    if (!gHaikuNode) {
        fprintf(stderr, "[JAMin-Bridge] ERROR: gHaikuNode is NULL!\n");
        return;
    }
    
    BMessenger nodeMessenger(NULL, gHaikuNode->ControlPort());
    if (nodeMessenger.IsValid()) {
        fprintf(stderr, "[JAMin-Bridge] Sending 0x%lx to Node port %d\n", msg->what, (int)gHaikuNode->ControlPort());
        nodeMessenger.SendMessage(msg);
    } else {
        fprintf(stderr, "[JAMin-Bridge] ERROR: Node messenger is INVALID (Port: %d)\n", (int)gHaikuNode->ControlPort());
    }
}


/* --- 2. Filter function to sniff for Media Server events --- */
filter_result jamin_media_filter(BMessage* message, BHandler** target, BMessageFilter* filter) {
    // 1. LOUD DEBUG: Print every message that hits the app
    // This will create a LOT of text, but it proves the filter is alive.
    fprintf(stderr, "[JAMin-Filter] Sniffed: 0x%08lx\n", message->what);

    // 0x40000000 range is Media Kit notifications
    if ((message->what & 0xF0000000) == 0x40000000 || message->what == 1073741824) {
        media_node_id nid;
        if (message->FindInt32("node", &nid) == B_OK) {
            fprintf(stderr, "[JAMin-Filter] >>> MEDIA NODE EVENT: ID %d. Code: 0x%lx\n", (int)nid, message->what);
            
            // Execute hijack immediately from the filter
            if (gHaikuNode) {
                gHaikuNode->HijackNode(nid);
            }
        }
    }
    return B_DISPATCH_MESSAGE;
}





extern "C" {
    void* jack_client_open(const char* name, int options, int* status, ...) {
    	
    	/* Inside your jack_client_open stub or JaminNode constructor */
		UpdateAudioParams_Ptr = RealUpdateAudioParams;
		
        if (status) *status = 0;
        
        if (gHaikuNode == NULL) {
            fprintf(stderr, "[JAMin-Debug] Creating JaminNode instance...\n");
            gHaikuNode = new JaminNode();             
            
            BMediaRoster* roster = BMediaRoster::Roster();
            if (!roster) {
                fprintf(stderr, "[JAMin-Debug] FATAL: Could not get BMediaRoster!\n");
                return (void*)0xDEADBEEF;
            }

            // 1. Register Node
            status_t err = roster->RegisterNode(gHaikuNode);
            if (err != B_OK) {
                fprintf(stderr, "[JAMin-Debug] RegisterNode FAILED: %s\n", strerror(err));
                return (void*)0xDEADBEEF;
            }

            // 2. Start Roster Watching immediately via the Control Port
            snooze(50000); 
            BMessenger nodeMessenger(NULL, gHaikuNode->ControlPort());
            if (nodeMessenger.IsValid()) {
                if (roster->StartWatching(nodeMessenger, B_MEDIA_WILDCARD) == B_OK) {
                    fprintf(stderr, "[JAMin-Debug] Roster watching active on port %d\n", (int)gHaikuNode->ControlPort());
                }
            }

            // 3. Locate the Audio Mixer
            media_node mixerNode;
            err = roster->GetAudioMixer(&mixerNode); 
            if (err != B_OK) {
                fprintf(stderr, "[JAMin-Debug] GetAudioMixer failed, searching by name...\n");
                
                // Declare as an array (buffer) of 64 nodes
// Corrected Search Block
live_node_info info[64]; // Explicitly declare an array of 64
int32 count = 64;

if (roster->GetLiveNodes(info, &count) == B_OK) {
    for (int i = 0; i < count; i++) {
        if (strstr(info[i].name, "Audio Mixer")) {
            mixerNode = info[i].node;
            err = B_OK;
            break;
        }
    }
}

            }


            if (err == B_OK) {
                media_input mixerInput;
                int32 inputCount = 0;
                
                // 4. Find Mixer Input
                err = roster->GetFreeInputsFor(mixerNode, &mixerInput, 1, &inputCount, B_MEDIA_RAW_AUDIO);
                if (err != B_OK || inputCount == 0) {
                    err = roster->GetAllInputsFor(mixerNode, &mixerInput, 1, &inputCount);
                }

                if (err == B_OK && inputCount > 0) {
                    media_format connectFormat = mixerInput.format;
                    connectFormat.type = B_MEDIA_RAW_AUDIO;
                    connectFormat.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
                    connectFormat.u.raw_audio.channel_count = 2;

                    // 5. Connect JAMin -> Mixer
                    err = roster->Connect(gHaikuNode->fOutput.source, mixerInput.destination, 
                                          &connectFormat, &gHaikuNode->fOutput, &mixerInput);
                    
                    if (err == B_OK) {
                        fprintf(stderr, "[JAMin-Debug] SUCCESS: Auto-connected to Mixer!\n");
                        gCurrentSampleRate = connectFormat.u.raw_audio.frame_rate;
                        roster->SetAudioOutput(gHaikuNode->Node());
                    } else {
                        fprintf(stderr, "[JAMin-Debug] Connect FAILED: %s\n", strerror(err));
                    }
                }
            } else {
                fprintf(stderr, "[JAMin-Debug] Could not find the Audio Mixer node.\n");
            }
        }
        return (void*)0xDEADBEEF;
    }
}
   
    
extern "C" {
    void* jack_client_new(const char* name) {
        return jack_client_open(name, 0, NULL);
    }
}

extern "C" void RealUpdateAudioParams(float rate, int32_t bufferSize) {
    if (gHaikuNode) {
        BAutolock locker(gHaikuNode->fLock);
        
        // 1. Update the internal Media Kit formats
        gHaikuNode->fInput.format.u.raw_audio.frame_rate = rate;
        gHaikuNode->fOutput.format.u.raw_audio.frame_rate = rate;
        
        // 2. Update the buffer size (Frames -> Bytes)
        size_t byteSize = bufferSize * 2 * sizeof(float);
        gHaikuNode->fInput.format.u.raw_audio.buffer_size = byteSize;
        gHaikuNode->fOutput.format.u.raw_audio.buffer_size = byteSize;

        // 3. Clear resampler state to prevent audio artifacts
        // (Assuming you made phaseAcc a member)
        // gHaikuNode->phaseAcc = 0.0;

        fprintf(stderr, "[JAMin-Haiku] Params updated: %.0fHz / %d frames\n", rate, bufferSize);
    }
}

void JaminNode::RestoreAll() {
    fprintf(stderr, "[JAMin-Debug] RestoreAll called. Map size: %zu\n", fHijackedNodes.size());
    if (fHijackedNodes.empty()) return;

    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster) return;

    for (auto const& [id, session] : fHijackedNodes) {
        media_node node;
        // 1. Get the actual producer node (the app being hijacked)
        if (roster->GetNodeFor(id, &node) == B_OK) {
            fprintf(stderr, "[JAMin] Restoring node %d...\n", (int)id);

            // STOP the source app to prevent buffer requests during the swap
            roster->StopNode(node, 0, true);
            
            // 2. SNIP: Find where the source is currently connected
            media_input inputs[1]; // We only need the first connection
            int32 inputCount = 0;
            
            // Get current connection info for the producer node
            if (roster->GetConnectedInputsFor(node, inputs, 1, &inputCount) == B_OK && inputCount > 0) {
                // Use the live destination info provided by the Roster to disconnect
                roster->Disconnect(node.node, session.source, 
                                 inputs[0].node.node, inputs[0].destination);
            }
            
            snooze(150000); 

            // 3. PLUG: Reconnect to original Mixer/Hardware
            media_format fmt;
            fmt.type = B_MEDIA_RAW_AUDIO;
            fmt.u.raw_audio = media_raw_audio_format::wildcard;
            
            status_t err = roster->Connect(session.source, session.original_dest, 
                                         &fmt, NULL, NULL);
            
            if (err == B_OK) {
                roster->StartNode(node, 0);
                fprintf(stderr, "[JAMin] SUCCESS: Node %d restored.\n", (int)id);
            } else {
                fprintf(stderr, "[JAMin] FAILED Restore: %s (0x%lx)\n", strerror(err), err);
            }
        }
    }
    fHijackedNodes.clear();
}







// Put this after class RosterHandler is fully defined
JaminNode::~JaminNode() {
    fprintf(stderr, "[JAMin-Debug] Cleaning up JaminNode memory...\n");

    if (_scratch_in_L) {
        free(_scratch_in_L);
        free(_scratch_in_R);
        free(_scratch_out_L);
        free(_scratch_out_R);
        if (_scratch_dummy) free(_scratch_dummy);
        _scratch_in_L = _scratch_in_R = _scratch_out_L = _scratch_out_R = _scratch_dummy = NULL;
    }

    if (fHandler) {
        delete fHandler; // Compiler now knows the full type, so no warning!
        fHandler = NULL;
    }
    
    fprintf(stderr, "[JAMin-Debug] JaminNode memory cleaned up.\n");
}




