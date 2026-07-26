/*
OBS Instant Replay - encoded output

A custom obs_output that OBS feeds encoded video/audio packets to. Instead of
muxing to a file, it hands every packet to a RingBuffer. This is registered as
an output type and started by the controller against a dedicated low-latency
replay encoder.
*/

#pragma once

#include <obs.h>

class RingBuffer;

// Registers the "instant_replay_output" output type with OBS. Call once at load.
void register_replay_output();

// The controller passes the shared ring buffer to the output instance through
// the output's settings ("ring_ptr"). These helpers keep that contract in one
// place.
obs_data_t *replay_output_make_settings(RingBuffer *ring);
