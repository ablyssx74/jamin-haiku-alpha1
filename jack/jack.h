/*
 * Copyright 2026, ablyss jb@epluribusunix.net
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#ifndef JACK_DUMMY_H
#define JACK_DUMMY_H
#define JackPortIsPhysical 0x4

#include <stdint.h>
#include <stddef.h>

// Core Types
typedef uint32_t jack_nframes_t;
typedef uint64_t jack_time_t;
typedef float jack_default_audio_sample_t;
typedef unsigned char jack_midi_data_t;

#define JACK_DEFAULT_AUDIO_TYPE "32 bit float mono audio"
#define JACK_DEFAULT_MIDI_TYPE  "8 bit raw midi"
#define JackPortIsInput  0x1
#define JackPortIsOutput 0x2

extern float gCurrentSampleRate; 
extern int32_t gCurrentBufferSize; 

// Pointer Handles
typedef void* jack_client_t;
typedef void* jack_port_t;

// Enums and Status
//typedef int jack_options_t;
//typedef int jack_status_t;
//typedef int jack_transport_state_t;
typedef enum {
    JackTransportStopped = 0,
    JackTransportRolling = 1,
    JackTransportStarting = 2,
    JackTransportNetStarting = 3
} jack_transport_state_t;


// Options for jack_client_open
typedef enum {
    JackNullOption = 0x00,
    JackNoStartServer = 0x01,
    JackUseExactName = 0x02,
    JackServerName = 0x04,
    JackLoadName = 0x08,
    JackLoadInit = 0x10,
    JackSessionID = 0x20
} jack_options_t;

// Status bits returned by jack_client_open
typedef enum {
    JackFailure = 0x01,
    JackInvalidOption = 0x02,
    JackNameNotUnique = 0x04,
    JackServerStarted = 0x08,
    JackServerFailed = 0x10,
    JackServerError = 0x20,
    JackNoSuchClient = 0x40,
    JackLoadFailure = 0x80,
    JackInitFailure = 0x100,
    JackShmFailure = 0x200,
    JackVersionError = 0x400,
    JackBackendError = 0x800,
    JackClientZombie = 0x1000
} jack_status_t;

// Missing function declarations noticed in your compiler warnings
unsigned int jack_frame_time(void* client);
void jack_port_set_latency(void* port, unsigned int latency);


// Structs
typedef struct {
    uint32_t frame;
    double beats_per_minute;
    uint32_t frame_rate;
} jack_position_t;

// This replaces the old 'typedef void' version
typedef struct {
    jack_nframes_t time;
    size_t size;
    jack_midi_data_t *buffer;
} jack_midi_event_t;


// Function Prototypes for Linker satisfaction
#ifdef __cplusplus
extern "C" {
#endif


#ifndef JAMIN_RINGBUFFER_H
typedef struct {
    char    *buf;
    size_t   len;
    size_t   size;
    size_t   size_mask;
    size_t   write_ptr;
    size_t   read_ptr;
    int      mlocked;
} jack_ringbuffer_t;
#endif



const char** jack_get_ports(jack_client_t, const char*, const char*, unsigned long);
void jack_free(void* ptr); // You'll likely need this too to free the pports list
void jack_transport_start(jack_client_t);
void jack_transport_stop(jack_client_t);
int jack_transport_locate(jack_client_t, jack_nframes_t);


    void jack_set_process_callback(jack_client_t, int (*)(jack_nframes_t, void*), void*);
    void* jack_port_get_buffer(jack_port_t, jack_nframes_t);
    int jack_activate(jack_client_t);
    void jack_on_shutdown(jack_client_t, void (*)(void*), void*);
    int jack_connect(jack_client_t, const char*, const char*);
    const char* jack_port_name(const jack_port_t);
    float jack_cpu_load(jack_client_t);
    int jack_port_connected(const jack_port_t);
    void jack_client_close(jack_client_t);
    int jack_transport_query(jack_client_t, jack_position_t*);

    jack_client_t *jack_client_open(const char *client_name, jack_options_t options, jack_status_t *status, ...);
    

    const char* jack_get_client_name(jack_client_t);
    //uint32_t jack_get_sample_rate(jack_client_t);
    //jack_nframes_t jack_get_buffer_size(jack_client_t);
    //static uint32_t jack_get_sample_rate(jack_client_t) { return 192000; }
    //static jack_nframes_t jack_get_buffer_size(jack_client_t) { return 1024; }
	static uint32_t jack_get_sample_rate(jack_client_t client) { 
    	return (uint32_t)gCurrentSampleRate; 
	}

	static jack_nframes_t jack_get_buffer_size(jack_client_t client) { 
    	return (jack_nframes_t)gCurrentBufferSize; 
	}
    
    // MIDI
    int jack_midi_get_event_count(void*);
    void jack_midi_clear_buffer(void*);
    int jack_midi_event_get(jack_midi_event_t*, void*, uint32_t);
    void jack_midi_event_write(void*, jack_nframes_t, const jack_midi_data_t*, size_t);
#ifdef __cplusplus
}

#endif
#endif
