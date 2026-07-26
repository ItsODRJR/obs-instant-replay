/*
OBS Instant Replay - async replay source implementation
*/

#include "replay-source.hpp"
#include "decoder.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <atomic>
#include <mutex>
#include <thread>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace {

// Internal state carried by the single replay source instance.
struct ReplaySource {
	obs_source_t *source = nullptr;

	std::mutex worker_mutex;
	std::thread worker;
	std::atomic<bool> cancel{false};

	// Preload lead: start decoding this far ahead of display so the first frame
	// is ready and OBS never shows a black frame at the cut. Also the async
	// buffer we keep ahead of the playhead.
	int64_t preload_lead_ns = 120'000'000; // 120 ms
};

// V1 has exactly one replay source; keep a process-wide handle so the
// controller can drive it without an OBS API to fetch a source's private data.
ReplaySource *g_instance = nullptr;
std::mutex g_instance_mutex;

// --- OBS pixel format mapping --------------------------------------------

bool map_pixfmt(int av_fmt, video_format &out)
{
	switch (av_fmt) {
	case AV_PIX_FMT_YUV420P:
		out = VIDEO_FORMAT_I420;
		return true;
	case AV_PIX_FMT_NV12:
		out = VIDEO_FORMAT_NV12;
		return true;
	case AV_PIX_FMT_YUV444P:
		out = VIDEO_FORMAT_I444;
		return true;
	case AV_PIX_FMT_YUYV422:
		out = VIDEO_FORMAT_YUY2;
		return true;
	default:
		return false;
	}
}

void fill_frame(obs_source_frame &frame, AVFrame *av, video_format fmt, uint64_t timestamp_ns)
{
	frame = {};
	frame.format = fmt;
	frame.width = (uint32_t)av->width;
	frame.height = (uint32_t)av->height;
	frame.timestamp = timestamp_ns;

	for (int i = 0; i < MAX_AV_PLANES && av->data[i]; ++i) {
		frame.data[i] = av->data[i];
		frame.linesize[i] = (uint32_t)av->linesize[i];
	}

	// Color: derive matrix from the source colorspace/range. NOTE: for V1 we
	// assume BT.709 limited range (typical 1080p60). TODO: read
	// av->colorspace / av->color_range and pick precisely for HDR/BT.601.
	frame.full_range = (av->color_range == AVCOL_RANGE_JPEG);
	video_format_get_parameters(VIDEO_CS_709,
				    frame.full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL,
				    frame.color_matrix, frame.color_range_min,
				    frame.color_range_max);
}

// --- worker ---------------------------------------------------------------

void playback_worker(ReplaySource *self, ReplayClip clip, double speed,
		     std::function<void()> on_finished)
{
	if (speed <= 0.0)
		speed = 1.0;

	Decoder decoder;
	AVCodecID codec_id = (clip.video_codec == ReplayVideoCodec::HEVC)
				     ? AV_CODEC_ID_HEVC
				     : AV_CODEC_ID_H264;

	if (!decoder.open(codec_id, clip.video_extradata.data(),
			  clip.video_extradata.size())) {
		obs_log(LOG_ERROR, "replay: decoder failed to open; aborting playback");
		if (on_finished)
			on_finished();
		return;
	}

	const uint64_t base_wall = os_gettime_ns() + self->preload_lead_ns;
	bool first_shown = false;
	bool logged_fmt = false;
	int emitted_count = 0; // frames actually handed to OBS

	auto emit = [&](AVFrame *av) {
		if (!logged_fmt) {
			obs_log(LOG_INFO,
				"replay: decoded frame fmt=%d %dx%d linesize=[%d,%d,%d] range=%d",
				av->format, av->width, av->height, av->linesize[0],
				av->linesize[1], av->linesize[2], av->color_range);
			logged_fmt = true;
		}

		// Frame pts is the microsecond clock we stamped on the packet.
		const int64_t frame_usec = av->pts;
		if (frame_usec < clip.visible_start_usec)
			return; // lead-in frame decoded only to prime the GOP

		const int64_t clip_offset_ns = (frame_usec - clip.visible_start_usec) * 1000;
		const int64_t stretched_ns = (int64_t)(clip_offset_ns / speed);
		const uint64_t target_wall = base_wall + stretched_ns;

		// Pace: don't run the async buffer too far ahead of real time.
		const uint64_t now = os_gettime_ns();
		if (target_wall > now + self->preload_lead_ns)
			os_sleepto_ns(target_wall - self->preload_lead_ns);

		video_format fmt;
		if (!map_pixfmt(av->format, fmt)) {
			obs_log(LOG_WARNING, "replay: unsupported pixel format %d",
				av->format);
			return;
		}

		obs_source_frame frame;
		fill_frame(frame, av, fmt, target_wall);

		if (!first_shown) {
			// Prime the first frame so the cut to the replay shows a
			// picture immediately instead of black.
			obs_source_preload_video(self->source, &frame);
			first_shown = true;
		}
		obs_source_output_video(self->source, &frame);
		++emitted_count;
	};

	for (const StoredPacket &sp : clip.packets) {
		if (self->cancel.load())
			break;
		if (sp.packet.type != OBS_ENCODER_VIDEO)
			continue; // MVP: video only, audio muted

		// Send the packet, resending on EAGAIN after draining, so no packet is
		// ever dropped (a dropped packet corrupts all following P-frames).
		while (!decoder.send_packet(sp.data.data(), sp.data.size(), sp.packet.dts_usec,
					    sp.packet.keyframe)) {
			while (AVFrame *f = decoder.receive_frame()) {
				if (self->cancel.load())
					break;
				emit(f);
			}
			if (self->cancel.load())
				break;
		}
		while (AVFrame *f = decoder.receive_frame()) {
			if (self->cancel.load())
				break;
			emit(f);
		}
	}

	// Drain frames still buffered in the decoder.
	if (!self->cancel.load()) {
		decoder.send_flush();
		while (AVFrame *f = decoder.receive_frame()) {
			if (self->cancel.load())
				break;
			emit(f);
		}
	}

	obs_log(LOG_INFO, "replay: playback finished (cancelled=%d, %d frames shown)",
		(int)self->cancel.load(), emitted_count);
	if (on_finished)
		on_finished();
}

// --- OBS source callbacks -------------------------------------------------

const char *source_get_name(void *)
{
	return "Instant Replay";
}

void *source_create(obs_data_t *, obs_source_t *source)
{
	auto *self = new ReplaySource();
	self->source = source;
	{
		std::lock_guard<std::mutex> lock(g_instance_mutex);
		g_instance = self;
	}
	obs_log(LOG_INFO, "replay source created");
	return self;
}

void source_destroy(void *data)
{
	auto *self = static_cast<ReplaySource *>(data);
	self->cancel.store(true);
	{
		std::lock_guard<std::mutex> lock(self->worker_mutex);
		if (self->worker.joinable())
			self->worker.join();
	}
	{
		std::lock_guard<std::mutex> lock(g_instance_mutex);
		if (g_instance == self)
			g_instance = nullptr;
	}
	delete self;
}

obs_source_info make_source_info()
{
	obs_source_info info = {};
	info.id = kReplaySourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	info.get_name = source_get_name;
	info.create = source_create;
	info.destroy = source_destroy;
	// Async video sources report size from their frames; no get_width/height
	// or video_render needed. OBS syncs A/V from frame timestamps for us.
	return info;
}

} // namespace

void register_replay_source()
{
	static obs_source_info info = make_source_info();
	obs_register_source(&info);
}

obs_source_t *replay_source_get_singleton()
{
	std::lock_guard<std::mutex> lock(g_instance_mutex);
	return g_instance ? g_instance->source : nullptr;
}

// --- ReplaySourceControl --------------------------------------------------

void ReplaySourceControl::play(ReplayClip &&clip, double speed,
			       std::function<void()> on_finished)
{
	ReplaySource *self;
	{
		std::lock_guard<std::mutex> lock(g_instance_mutex);
		self = g_instance;
	}
	if (!self) {
		obs_log(LOG_ERROR, "replay: play() with no source instance");
		return;
	}

	std::lock_guard<std::mutex> lock(self->worker_mutex);
	// Cancel any in-flight playback and start fresh.
	self->cancel.store(true);
	if (self->worker.joinable())
		self->worker.join();
	self->cancel.store(false);

	self->worker = std::thread(playback_worker, self, std::move(clip), speed,
				   std::move(on_finished));
}

void ReplaySourceControl::stop()
{
	ReplaySource *self;
	{
		std::lock_guard<std::mutex> lock(g_instance_mutex);
		self = g_instance;
	}
	if (!self)
		return;

	// Signal only - NEVER join here. stop() is reachable from the worker
	// thread itself (playback's on_finished -> return_to_live -> stop), and a
	// thread joining itself deadlocks forever. The worker is joined instead by
	// play() (before starting the next one) and by source_destroy(), both of
	// which run on other threads.
	self->cancel.store(true);
}
