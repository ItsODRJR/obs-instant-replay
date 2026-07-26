/*
OBS Instant Replay - encoded output implementation
*/

#include "replay-output.hpp"
#include "ring-buffer.hpp"

#include <obs-module.h>
#include <plugin-support.h>

namespace {

constexpr const char *kOutputId = "instant_replay_output";

struct ReplayOutput {
	obs_output_t *output = nullptr;
	RingBuffer *ring = nullptr;
	bool extradata_captured = false;
};

const char *output_get_name(void *)
{
	return "Instant Replay Buffer";
}

void *output_create(obs_data_t *settings, obs_output_t *output)
{
	auto *self = new ReplayOutput();
	self->output = output;
	// The controller hands us the shared ring buffer via settings. Stored as an
	// integer-encoded pointer (internal plumbing, never serialized to disk).
	self->ring = reinterpret_cast<RingBuffer *>(
		static_cast<intptr_t>(obs_data_get_int(settings, "ring_ptr")));
	obs_log(LOG_INFO, "replay output created (ring=%p)", (void *)self->ring);
	return self;
}

void output_destroy(void *data)
{
	delete static_cast<ReplayOutput *>(data);
}

bool output_start(void *data)
{
	auto *self = static_cast<ReplayOutput *>(data);

	if (!self->ring) {
		obs_log(LOG_ERROR, "replay output started without a ring buffer");
		return false;
	}
	// Standard encoded-output startup handshake.
	if (!obs_output_can_begin_data_capture(self->output, 0))
		return false;
	if (!obs_output_initialize_encoders(self->output, 0))
		return false;

	self->extradata_captured = false;
	bool ok = obs_output_begin_data_capture(self->output, 0);
	obs_log(LOG_INFO, "replay output start: %s", ok ? "ok" : "failed");
	return ok;
}

void output_stop(void *data, uint64_t /*ts*/)
{
	auto *self = static_cast<ReplayOutput *>(data);
	obs_output_end_data_capture(self->output);
	obs_log(LOG_INFO, "replay output stopped");
}

void capture_extradata(ReplayOutput *self)
{
	obs_encoder_t *venc = obs_output_get_video_encoder(self->output);
	if (!venc)
		return;

	uint8_t *extra = nullptr;
	size_t extra_size = 0;
	if (obs_encoder_get_extra_data(venc, &extra, &extra_size) && extra_size) {
		self->ring->set_video_extradata(extra, extra_size);
		self->extradata_captured = true;
		obs_log(LOG_INFO, "replay: captured %zu bytes of codec extradata",
			extra_size);
	}
}

void output_encoded_packet(void *data, encoder_packet *packet)
{
	auto *self = static_cast<ReplayOutput *>(data);
	if (!self->ring || !packet)
		return;

	// Grab SPS/PPS once, as soon as the encoder has produced it. Without this
	// the decoder in replay-source cannot be initialized.
	if (!self->extradata_captured && packet->type == OBS_ENCODER_VIDEO)
		capture_extradata(self);

	self->ring->push(packet);
}

obs_output_info make_output_info()
{
	obs_output_info info = {};
	info.id = kOutputId;
	// V1 buffers video only (replay audio is muted). Add OBS_OUTPUT_AUDIO and an
	// AAC encoder in V2 when slowed/crowd audio lands.
	info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
	info.get_name = output_get_name;
	info.create = output_create;
	info.destroy = output_destroy;
	info.start = output_start;
	info.stop = output_stop;
	info.encoded_packet = output_encoded_packet;
	// Codecs the output accepts; the controller picks the actual encoder.
	info.encoded_video_codecs = "h264;hevc";
	return info;
}

} // namespace

void register_replay_output()
{
	static obs_output_info info = make_output_info();
	obs_register_output(&info);
}

obs_data_t *replay_output_make_settings(RingBuffer *ring)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "ring_ptr",
			 static_cast<long long>(reinterpret_cast<intptr_t>(ring)));
	return settings;
}
