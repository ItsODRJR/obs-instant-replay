/*
OBS Instant Replay - FFmpeg video decoder wrapper implementation
*/

#include "decoder.hpp"

// util/base.h (LOG_* levels + blogva) must precede plugin-support.h, which
// re-declares blogva - including it first triggers a linkage-mismatch error.
#include <util/base.h>
#include <plugin-support.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/log.h>
}

namespace {
// Forward FFmpeg's warnings/errors into the OBS log so decode failures are
// visible (missing reference, error concealment, corrupt slices, etc.).
void ffmpeg_log_cb(void *, int level, const char *fmt, va_list vl)
{
	if (level > AV_LOG_WARNING)
		return;
	char buf[512];
	vsnprintf(buf, sizeof(buf), fmt, vl);
	size_t n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	if (n)
		obs_log(LOG_WARNING, "ffmpeg: %s", buf);
}
} // namespace

Decoder::~Decoder()
{
	close();
}

bool Decoder::open(AVCodecID codec_id, const uint8_t *extradata, size_t extradata_size)
{
	close();

	static bool s_log_set = false;
	if (!s_log_set) {
		av_log_set_callback(ffmpeg_log_cb);
		s_log_set = true;
	}

	const AVCodec *codec = avcodec_find_decoder(codec_id);
	if (!codec) {
		obs_log(LOG_ERROR, "replay decoder: no decoder for codec id %d",
			(int)codec_id);
		return false;
	}

	ctx_ = avcodec_alloc_context3(codec);
	if (!ctx_) {
		obs_log(LOG_ERROR, "replay decoder: failed to alloc context");
		return false;
	}

	// Feed the encoder's SPS/PPS to the decoder. This is mandatory: the ring
	// buffer's packets do not repeat parameter sets on every keyframe, so
	// without extradata the very first decode fails.
	if (extradata && extradata_size) {
		ctx_->extradata =
			(uint8_t *)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!ctx_->extradata) {
			close();
			return false;
		}
		memcpy(ctx_->extradata, extradata, extradata_size);
		ctx_->extradata_size = (int)extradata_size;
	} else {
		obs_log(LOG_WARNING,
			"replay decoder: opening without extradata - decoding will "
			"likely fail until a keyframe with in-band parameter sets");
	}

	// Deterministic single-threaded decode: with one thread, each packet we
	// send yields its frame right away, so we never have to drop a packet to
	// EAGAIN (dropping one breaks the H.264 reference chain and corrupts every
	// following P-frame). A short 720p replay clip decodes plenty fast on one
	// thread.
	ctx_->thread_count = 1;

	if (avcodec_open2(ctx_, codec, nullptr) < 0) {
		obs_log(LOG_ERROR, "replay decoder: avcodec_open2 failed");
		close();
		return false;
	}

	pkt_ = av_packet_alloc();
	frame_ = av_frame_alloc();
	if (!pkt_ || !frame_) {
		close();
		return false;
	}

	obs_log(LOG_INFO, "replay decoder opened (%s)", codec->name);
	return true;
}

void Decoder::close()
{
	if (frame_)
		av_frame_free(&frame_);
	if (pkt_)
		av_packet_free(&pkt_);
	if (ctx_)
		avcodec_free_context(&ctx_);
}

bool Decoder::send_packet(const uint8_t *data, size_t size, int64_t pts, bool keyframe)
{
	if (!ctx_)
		return false;

	av_packet_unref(pkt_);
	pkt_->data = const_cast<uint8_t *>(data);
	pkt_->size = (int)size;
	pkt_->pts = pts;
	pkt_->dts = pts;
	pkt_->flags = keyframe ? AV_PKT_FLAG_KEY : 0;

	int ret = avcodec_send_packet(ctx_, pkt_);
	pkt_->data = nullptr;
	pkt_->size = 0;
	if (ret == 0)
		return true; // packet accepted
	if (ret == AVERROR(EAGAIN))
		return false; // decoder full: caller must drain, then resend THIS packet
	// Genuine error: log and report "accepted" so the caller doesn't spin
	// forever resending the same bad packet.
	obs_log(LOG_WARNING, "replay decoder: send_packet failed (%d)", ret);
	return true;
}

AVFrame *Decoder::receive_frame()
{
	if (!ctx_)
		return nullptr;

	int ret = avcodec_receive_frame(ctx_, frame_);
	if (ret == 0)
		return frame_;
	return nullptr; // AVERROR(EAGAIN) / AVERROR_EOF -> need more input
}

void Decoder::send_flush()
{
	if (ctx_)
		avcodec_send_packet(ctx_, nullptr); // enter draining mode
}
