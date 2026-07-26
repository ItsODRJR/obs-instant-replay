/*
OBS Instant Replay - async replay source

An asynchronous obs_source that plays back a ReplayClip. A worker thread decodes
the clip out of RAM and pushes decoded frames into OBS via
obs_source_output_video() with stretched timestamps for slow motion. OBS paces
display from those timestamps.
*/

#pragma once

#include <obs.h>

#include <functional>
#include <memory>

#include "ring-buffer.hpp"

constexpr const char *kReplaySourceId = "instant_replay_source";

// Registers the async source type. Call once at load.
void register_replay_source();

// Thin handle the controller uses to drive a specific source instance without
// poking at OBS source internals directly.
class ReplaySourceControl {
public:
	explicit ReplaySourceControl(obs_source_t *source) : source_(source) {}

	// Begin playing a clip at the given speed (1.0 = realtime, 0.5 = half).
	// on_finished fires on the worker thread when playback completes or is
	// cancelled - keep it lightweight / thread-safe.
	void play(ReplayClip &&clip, double speed, std::function<void()> on_finished);

	// Stop immediately (emergency return-to-live).
	void stop();

	obs_source_t *source() const { return source_; }

private:
	obs_source_t *source_;
};

// Look up the singleton replay source instance (created lazily by the
// controller), or nullptr if not yet created.
obs_source_t *replay_source_get_singleton();
