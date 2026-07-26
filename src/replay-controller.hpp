/*
OBS Instant Replay - controller (multi-camera)

Owns one independent capture pipeline per camera and ties everything together:
  * a dedicated low-latency replay encoder per camera, each fed by its own
    off-air OBS view (so you can buffer Camera 2 while Camera 1 is on program)
  * the custom encoded output feeding that camera's RAM ring buffer
  * the async replay source (shared - one replay plays at a time)
  * the "take replay" flow (post-roll wait, clip extract, scene switch, return)

Cameras are discovered by scene name: any scene named "Replay Cam 1",
"Replay Cam 2", ... is buffered as a separate angle. If none exist, the program
feed is buffered instead (single angle).

One process-wide instance, created at module load, torn down at unload.
*/

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class ReplayController {
public:
	static ReplayController &instance();

	// Called on OBS FINISHED_LOADING. Prepares the controller but does NOT
	// start buffering - buffering is opt-in (see toggle_buffering) so it never
	// interferes with streaming/recording unless the operator turns it on.
	bool start();
	// Called from obs_module_unload.
	void shutdown();

	// Turn background buffering on/off. Safe from any thread (marshals the
	// actual build/teardown to the OBS UI thread).
	void toggle_buffering();

	// Trigger a replay of a specific camera angle (0-based). Plays the last
	// `length_seconds` before now, plus `postroll` seconds captured after the
	// press, at `speed` (1.0 = realtime). Out-of-range angles are logged/ignored.
	void take_replay(size_t angle, double length_seconds, double postroll, double speed);

	// Emergency: stop playback and return to the live scene right now.
	void return_to_live();

	// Number of buffered angles (cameras, or 1 for the program-feed fallback).
	size_t channel_count() const;

	// True while background buffering is active.
	bool is_buffering() const { return channel_count() > 0; }

	// Scenes to buffer as camera angles, chosen in the dock. Empty => buffer the
	// program feed. Takes effect the next time buffering is enabled.
	void set_selected_scenes(const std::vector<std::string> &scene_names);
	std::vector<std::string> selected_scenes() const;

	// Label of angle `index` (for UI: scene name, or "(program)").
	std::string angle_label(size_t index) const;

private:
	ReplayController() = default;
	~ReplayController() = default;
	ReplayController(const ReplayController &) = delete;
	ReplayController &operator=(const ReplayController &) = delete;

	bool build_channels();
	void teardown_channels();

	// Runs on the OBS UI thread (via obs_queue_task): remembers the current
	// scene, then cuts to the replay scene. `param` is the ReplayController*.
	static void ui_enter_replay_scene(void *param);

	// Runs on the OBS UI thread: builds channels if off, tears them down if on.
	static void ui_toggle_buffering(void *param);

	struct Channel; // one per-camera capture pipeline
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// Registers per-angle hotkeys (Angle 1..N) plus return-to-live. Call once at load.
void register_replay_hotkeys();
void unregister_replay_hotkeys();
