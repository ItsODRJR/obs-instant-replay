/*
OBS Instant Replay - controller implementation (multi-camera)
*/

#include "replay-controller.hpp"
#include "replay-output.hpp"
#include "replay-source.hpp"
#include "ring-buffer.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// Playback scene: the operator puts the "Instant Replay" source in a scene with
// this name. V1 relies on it existing; V2 will manage it automatically.
constexpr const char *kReplaySceneName = "Replay";

// Safety cap on simultaneous encoders (each angle is a full NVENC session).
constexpr size_t kMaxChannels = 6;

// How many per-angle hotkeys to register (angles beyond this can't be bound).
constexpr size_t kAngleHotkeys = 4;

// Replay encoder is kept deliberately LIGHT so it can coexist with the
// operator's real stream/recording encoder without starving the shared GPU
// encode pipeline: fastest single-pass NVENC preset, low-latency tuning. We
// encode at the native canvas size (no scaled_size) so the stream's SPS always
// matches the coded frames - see the note in build_channels().
constexpr int kReplayBitrateKbps = 10000; // plenty for 1080p at p1/ull

// Preferred encoders, best-effort first. All are H.264 for V1.
const char *kEncoderPreference[] = {
	"jim_nvenc",        // NVIDIA NVENC
	"h264_texture_amf", // AMD AMF
	"obs_qsv11",        // Intel Quick Sync
	"obs_x264",         // software fallback
};

// Create a dedicated low-latency replay encoder: short GOP (fast seek to a
// keyframe), no B-frames (decode order == display order), and the lightest
// NVENC settings so it doesn't contend with the operator's real encoder:
// fastest preset (p1), single-pass (multipass disabled), ultra-low-latency
// tuning, no lookahead/AQ. Falls back gracefully for x264. Returns nullptr if
// no encoder could be created.
obs_encoder_t *create_replay_encoder(const std::string &name)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "bitrate", kReplayBitrateKbps);
	obs_data_set_string(s, "rate_control", "CBR");
	obs_data_set_int(s, "keyint_sec", 1); // 0.5-1s keyframe interval
	obs_data_set_int(s, "bf", 0);         // no B-frames
	// NVENC (obs-nvenc) keys - lightest possible:
	obs_data_set_string(s, "preset", "p1");        // fastest
	obs_data_set_string(s, "tuning", "ull");       // ultra low latency
	obs_data_set_string(s, "multipass", "disabled"); // single pass
	obs_data_set_bool(s, "lookahead", false);
	obs_data_set_bool(s, "psycho_aq", false);
	// x264 fallback tune (ignored by NVENC). NOTE: we intentionally do NOT
	// override "preset" for x264 - keeping "p1" here means the rare software
	// fallback just uses x264's default preset, while NVENC gets its fastest.
	obs_data_set_string(s, "tune", "zerolatency");

	obs_encoder_t *enc = nullptr;
	for (const char *id : kEncoderPreference) {
		enc = obs_video_encoder_create(id, name.c_str(), s, nullptr);
		if (enc)
			break;
	}
	obs_data_release(s);
	return enc;
}

} // namespace

// One independent capture pipeline (one camera angle).
struct ReplayController::Channel {
	std::string label;               // scene name, or "(program)"
	obs_source_t *scene = nullptr;   // strong ref (null for program feed)
	obs_view_t *view = nullptr;      // null when buffering the program feed
	video_t *video = nullptr;        // encoder input
	obs_encoder_t *venc = nullptr;
	obs_output_t *output = nullptr;
	std::unique_ptr<RingBuffer> ring;
};

struct ReplayController::Impl {
	std::vector<std::unique_ptr<Channel>> channels;
	std::vector<std::string> selected;       // scene names to buffer (dock)
	obs_weak_source_t *prev_scene = nullptr; // scene to return to
	std::atomic<bool> playing{false};
};

ReplayController &ReplayController::instance()
{
	static ReplayController inst;
	return inst;
}

size_t ReplayController::channel_count() const
{
	return impl_ ? impl_->channels.size() : 0;
}

void ReplayController::set_selected_scenes(const std::vector<std::string> &scene_names)
{
	if (!impl_)
		impl_ = std::make_unique<Impl>();
	impl_->selected = scene_names;
}

std::vector<std::string> ReplayController::selected_scenes() const
{
	return impl_ ? impl_->selected : std::vector<std::string>{};
}

std::string ReplayController::angle_label(size_t index) const
{
	if (impl_ && index < impl_->channels.size())
		return impl_->channels[index]->label;
	return {};
}

bool ReplayController::start()
{
	if (!impl_)
		impl_ = std::make_unique<Impl>();
	// Opt-in: do NOT start buffering here. Buffering only runs when the
	// operator presses the "Toggle Replay Buffer" hotkey, so the plugin never
	// touches the GPU encode pipeline (and thus streaming/recording) until then.
	obs_log(LOG_INFO, "replay: ready - buffering is OFF; bind & press 'Toggle Replay Buffer' to start");
	return true;
}

void ReplayController::toggle_buffering()
{
	// Build/teardown touch obs_frontend_* and encoders - do it on the UI thread.
	obs_queue_task(OBS_TASK_UI, &ReplayController::ui_toggle_buffering, this, false);
}

void ReplayController::ui_toggle_buffering(void *param)
{
	auto *self = static_cast<ReplayController *>(param);
	if (!self->impl_)
		return;

	if (self->impl_->channels.empty()) {
		if (self->build_channels())
			obs_log(LOG_INFO, "replay: buffer ENABLED (%zu angle[s]) - now retaining 30s",
				self->impl_->channels.size());
		else
			obs_log(LOG_ERROR, "replay: failed to enable buffer");
	} else {
		self->teardown_channels();
		obs_log(LOG_INFO, "replay: buffer DISABLED");
	}
}

void ReplayController::shutdown()
{
	teardown_channels();
	impl_.reset();
}

bool ReplayController::build_channels()
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		obs_log(LOG_ERROR, "replay: no video info; is OBS video active?");
		return false;
	}

	// Build one Channel for `scene` (or the program feed if scene is null) and
	// start it buffering. Defined here as a lambda so it can name the private
	// nested Channel type. Returns nullptr on failure.
	auto make_channel = [&](const char *label, obs_source_t *scene) -> std::unique_ptr<Channel> {
		auto ch = std::make_unique<Channel>();
		ch->label = label;
		ch->ring = std::make_unique<RingBuffer>();
		ch->ring->set_max_duration(30.0); // TODO(V2): configurable

		if (scene) {
			// Render this scene into its own off-air video mix so it is
			// buffered independently of what's on program. Rendering it
			// here also keeps the camera inside it active while off-air.
			ch->scene = obs_source_get_ref(scene);
			ch->view = obs_view_create();
			obs_view_set_source(ch->view, 0, ch->scene);
			ch->video = obs_view_add2(ch->view, &ovi);
			if (!ch->video) {
				obs_log(LOG_ERROR, "replay: obs_view_add2 failed for '%s'", label);
				return nullptr;
			}
		} else {
			ch->video = obs_get_video(); // program mix
		}

		ch->venc = create_replay_encoder(std::string("replay_venc_") + label);
		if (!ch->venc) {
			obs_log(LOG_ERROR, "replay: could not create encoder for '%s'", label);
			return nullptr;
		}
		// IMPORTANT: do NOT call obs_encoder_set_scaled_size here. On this build
		// it forces obs-nvenc onto its CPU-scaling fallback and emits a stream
		// whose SPS resolution (1080p) does not match the coded slice size
		// (720p). The decoder then allocates a 1080p frame from 720p slice data,
		// so macroblocks land at the wrong stride -> diagonal shear + a garbage
		// bottom third. Encode at the native canvas size so the SPS always
		// matches the frames; NVENC p1/ull at 1080p is cheap (~1 ms/frame) and
		// still coexists with the operator's real stream/recording encoder.
		obs_encoder_set_video(ch->venc, ch->video);

		obs_data_t *osettings = replay_output_make_settings(ch->ring.get());
		ch->output = obs_output_create("instant_replay_output",
					       (std::string("replay_out_") + label).c_str(), osettings, nullptr);
		obs_data_release(osettings);
		if (!ch->output) {
			obs_log(LOG_ERROR, "replay: failed to create output for '%s'", label);
			return nullptr;
		}
		obs_output_set_video_encoder(ch->output, ch->venc);

		if (!obs_output_start(ch->output)) {
			obs_log(LOG_ERROR, "replay: failed to start output for '%s': %s", label,
				obs_output_get_last_error(ch->output));
			return nullptr;
		}

		obs_log(LOG_INFO, "replay: buffering angle '%s' (30s)", label);
		return ch;
	};

	if (impl_->selected.empty()) {
		// No scenes chosen in the dock: buffer the program feed (1 angle).
		if (auto ch = make_channel("(program)", nullptr))
			impl_->channels.push_back(std::move(ch));
		obs_log(LOG_INFO, "replay: no scenes selected - buffering program feed");
	} else {
		for (const std::string &name : impl_->selected) {
			if (impl_->channels.size() >= kMaxChannels) {
				obs_log(LOG_WARNING, "replay: more than %zu scenes selected; ignoring the rest",
					kMaxChannels);
				break;
			}
			obs_source_t *scene = obs_get_source_by_name(name.c_str());
			if (!scene) {
				obs_log(LOG_WARNING, "replay: selected scene '%s' not found", name.c_str());
				continue;
			}
			auto ch = make_channel(name.c_str(), scene);
			obs_source_release(scene); // make_channel keeps its own ref
			if (ch)
				impl_->channels.push_back(std::move(ch));
		}
	}

	obs_log(LOG_INFO, "replay: %zu angle(s) buffering", impl_->channels.size());
	return !impl_->channels.empty();
}

void ReplayController::teardown_channels()
{
	if (!impl_)
		return;

	for (auto &ch : impl_->channels) {
		if (ch->output) {
			obs_output_stop(ch->output);
			obs_output_release(ch->output);
		}
		if (ch->venc)
			obs_encoder_release(ch->venc);
		if (ch->view) {
			obs_view_set_source(ch->view, 0, nullptr);
			obs_view_remove(ch->view);
			obs_view_destroy(ch->view);
		}
		if (ch->scene)
			obs_source_release(ch->scene);
	}
	impl_->channels.clear();

	if (impl_->prev_scene) {
		obs_weak_source_release(impl_->prev_scene);
		impl_->prev_scene = nullptr;
	}
}

// Runs on the OBS UI thread. Takes ownership of a strong scene ref and releases
// it after switching. Used for the return-to-live switch.
static void ui_switch_scene(void *param)
{
	obs_source_t *scene = static_cast<obs_source_t *>(param);
	if (scene) {
		obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	}
}

void ReplayController::return_to_live()
{
	// Signal any active playback to stop (safe from any thread; never joins).
	if (obs_source_t *src = replay_source_get_singleton())
		ReplaySourceControl(src).stop();

	// Restore the pre-replay scene exactly once per replay. Both the worker's
	// on_finished callback and the return hotkey call this; the exchange gates it.
	if (!impl_ || !impl_->playing.exchange(false))
		return;

	if (impl_->prev_scene) {
		// Hand a strong ref to the UI thread; ui_switch_scene releases it.
		if (obs_source_t *scene = obs_weak_source_get_source(impl_->prev_scene))
			obs_queue_task(OBS_TASK_UI, ui_switch_scene, scene, false);
	}
}

void ReplayController::ui_enter_replay_scene(void *param)
{
	auto *self = static_cast<ReplayController *>(param);
	if (!self->impl_)
		return;

	// Remember where we came from so we can return to it.
	if (obs_source_t *cur = obs_frontend_get_current_scene()) {
		if (self->impl_->prev_scene)
			obs_weak_source_release(self->impl_->prev_scene);
		self->impl_->prev_scene = obs_source_get_weak_source(cur);
		obs_source_release(cur);
	}

	// Cut to the replay scene.
	if (obs_source_t *rs = obs_get_source_by_name(kReplaySceneName)) {
		obs_frontend_set_current_scene(rs);
		obs_source_release(rs);
	} else {
		obs_log(LOG_WARNING,
			"replay: no scene named '%s' - create one containing the "
			"'Instant Replay' source",
			kReplaySceneName);
	}
}

void ReplayController::take_replay(size_t angle, double length_seconds, double postroll, double speed)
{
	if (!impl_ || impl_->channels.empty()) {
		obs_log(LOG_WARNING, "replay: buffer is OFF - press 'Toggle Replay Buffer' first");
		return;
	}
	if (angle >= impl_->channels.size()) {
		obs_log(LOG_INFO, "replay: angle %zu not configured (only %zu buffering)", angle + 1,
			impl_->channels.size());
		return;
	}
	if (impl_->playing.exchange(true)) {
		obs_log(LOG_INFO, "replay: already playing, ignoring trigger");
		return;
	}

	RingBuffer *ring = impl_->channels[angle]->ring.get();
	const int64_t total_ns = (int64_t)((length_seconds + postroll) * 1'000'000'000.0);
	const int64_t postroll_ns = (int64_t)(postroll * 1'000'000'000.0);
	const char *label = impl_->channels[angle]->label.c_str();
	obs_log(LOG_INFO, "replay: take angle %zu '%s'", angle + 1, label);

	// Do the wait + extraction off the caller (hotkey) thread.
	std::thread([this, ring, total_ns, postroll_ns, speed]() {
		// 1. Wait for the requested post-roll footage to be encoded.
		if (postroll_ns > 0)
			os_sleep_ms((uint32_t)(postroll_ns / 1'000'000));

		// 2. Pull the clip out of that angle's RAM ring.
		ReplayClip clip;
		if (!ring->extract_recent(total_ns, clip)) {
			obs_log(LOG_WARNING, "replay: not enough buffered footage yet");
			impl_->playing.store(false);
			return;
		}
		clip.video_codec = ReplayVideoCodec::H264; // matches replay encoder

		// 3. Remember the current scene and cut to the replay scene - on the UI
		//    thread. obs_frontend_* must not be driven from this worker thread.
		obs_queue_task(OBS_TASK_UI, &ReplayController::ui_enter_replay_scene, this, false);

		// 4. Play, and return to live when it finishes.
		obs_source_t *src = replay_source_get_singleton();
		if (!src) {
			obs_log(LOG_ERROR, "replay: no replay source in any scene");
			impl_->playing.store(false);
			return;
		}
		ReplaySourceControl(src).play(std::move(clip), speed, [this]() { return_to_live(); });
	}).detach();
}

// ---------------------------------------------------------------------------
// Hotkeys
// ---------------------------------------------------------------------------

namespace {

std::vector<obs_hotkey_id> g_hotkeys;

enum class HotkeyAction { Toggle, Take, Return };

struct HotkeyDef {
	const char *name;
	const char *description;
	HotkeyAction action;
	size_t angle;
	double length;
	double postroll;
	double speed;
};

// Toggle buffer on/off, one "take" hotkey per angle (8s @ 50%), plus
// return-to-live. Operators bind actual keys in Settings -> Hotkeys.
HotkeyDef g_hotkey_defs[] = {
	{"instant_replay.toggle", "Instant Replay: Toggle Buffer (on/off)", HotkeyAction::Toggle, 0, 0, 0, 0},
	{"instant_replay.angle1", "Instant Replay: Angle 1 (8s @ 50%)", HotkeyAction::Take, 0, 8.0, 0.5, 0.50},
	{"instant_replay.angle2", "Instant Replay: Angle 2 (8s @ 50%)", HotkeyAction::Take, 1, 8.0, 0.5, 0.50},
	{"instant_replay.angle3", "Instant Replay: Angle 3 (8s @ 50%)", HotkeyAction::Take, 2, 8.0, 0.5, 0.50},
	{"instant_replay.angle4", "Instant Replay: Angle 4 (8s @ 50%)", HotkeyAction::Take, 3, 8.0, 0.5, 0.50},
	{"instant_replay.return", "Instant Replay: return to live", HotkeyAction::Return, 0, 0, 0, 0},
};

void hotkey_cb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	const auto *def = static_cast<const HotkeyDef *>(data);
	switch (def->action) {
	case HotkeyAction::Toggle:
		ReplayController::instance().toggle_buffering();
		break;
	case HotkeyAction::Return:
		ReplayController::instance().return_to_live();
		break;
	case HotkeyAction::Take:
		ReplayController::instance().take_replay(def->angle, def->length, def->postroll, def->speed);
		break;
	}
}

} // namespace

void register_replay_hotkeys()
{
	static_assert(sizeof(g_hotkey_defs) / sizeof(g_hotkey_defs[0]) == kAngleHotkeys + 2,
		      "hotkey table must cover every angle plus toggle and return");
	for (HotkeyDef &def : g_hotkey_defs) {
		obs_hotkey_id id = obs_hotkey_register_frontend(def.name, def.description, hotkey_cb, &def);
		g_hotkeys.push_back(id);
	}
	obs_log(LOG_INFO, "replay: registered %zu hotkeys", g_hotkeys.size());
}

void unregister_replay_hotkeys()
{
	for (obs_hotkey_id id : g_hotkeys)
		obs_hotkey_unregister(id);
	g_hotkeys.clear();
}
