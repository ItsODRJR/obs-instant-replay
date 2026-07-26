/*
OBS Instant Replay - FFmpeg video decoder wrapper

Decodes H.264/H.265 packets straight out of the RAM ring buffer. The critical
detail is that it must be initialized with the encoder's extradata (SPS/PPS);
otherwise decoding produces nothing or garbage.
*/

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
}

class Decoder {
public:
	Decoder() = default;
	~Decoder();

	Decoder(const Decoder &) = delete;
	Decoder &operator=(const Decoder &) = delete;

	// codec_id is AV_CODEC_ID_H264 or AV_CODEC_ID_HEVC. extradata is the
	// SPS/PPS captured from the OBS encoder. Returns false if no decoder is
	// available or init fails.
	bool open(AVCodecID codec_id, const uint8_t *extradata, size_t extradata_size);
	void close();
	bool is_open() const { return ctx_ != nullptr; }

	// Feed one compressed packet. Returns true if it was accepted (or skipped on
	// a hard error), false if the decoder is full (EAGAIN) - in which case the
	// caller must drain with receive_frame() and then resend THE SAME packet, so
	// no packet is ever lost. pts is carried through to the decoded frame.
	bool send_packet(const uint8_t *data, size_t size, int64_t pts, bool keyframe);

	// Pull the next decoded frame, if one is ready. The returned AVFrame is
	// owned by the decoder and valid until the next receive_frame/close call.
	// Returns nullptr when more input is needed.
	AVFrame *receive_frame();

	// Flush at end of clip so buffered frames drain out.
	void send_flush();

private:
	AVCodecContext *ctx_ = nullptr;
	AVPacket *pkt_ = nullptr;
	AVFrame *frame_ = nullptr;
};
