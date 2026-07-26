/*
OBS Instant Replay - encoded RAM ring buffer

Holds the last N seconds of compressed video (and, later, audio) packets in
memory so a replay can start decoding immediately without ever touching disk.
This is the piece that replaces OBS's mux-to-file step in the live replay path.
*/

#pragma once

#include <obs.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// One compressed packet retained in the ring.
//
// IMPORTANT: we deep-copy the bitstream into our own `data` buffer rather than
// calling obs_encoder_packet_ref(). A video-only output receives the encoder's
// raw, transient packet (data points straight into the encoder's reused
// buffer, with no refcount header), so obs_encoder_packet_ref() would corrupt
// the heap. Owning a copy is the only safe option on this path.
struct StoredPacket {
	encoder_packet packet{};   // metadata; .data/.size mirror `data` below
	std::vector<uint8_t> data; // owned copy of the compressed bitstream

	StoredPacket() = default;
	explicit StoredPacket(const encoder_packet *src);
	~StoredPacket() = default;

	StoredPacket(const StoredPacket &other);
	StoredPacket &operator=(const StoredPacket &other);
	StoredPacket(StoredPacket &&other) noexcept;
	StoredPacket &operator=(StoredPacket &&other) noexcept;

	bool is_video() const { return packet.type == OBS_ENCODER_VIDEO; }
	bool is_keyframe() const { return packet.keyframe; }
	// Decode timestamp in microseconds on OBS's monotonic clock. Used for
	// ordering and for selecting "the last N seconds".
	int64_t dts_usec() const { return packet.dts_usec; }
};

// Which codec the video packets are in, so the decoder picks the right path.
enum class ReplayVideoCodec { Unknown, H264, HEVC };

// A contiguous slice of packets ready to be handed to the decoder. Packets are
// owned copies (each carries its own ref) so the ring buffer can keep trimming
// itself while a replay plays back.
struct ReplayClip {
	std::vector<StoredPacket> packets;
	std::vector<uint8_t> video_extradata; // SPS/PPS - decoder needs this
	ReplayVideoCodec video_codec = ReplayVideoCodec::H264; // set by controller

	// Decoding must begin at the keyframe at/before the requested start.
	int64_t decode_start_usec = 0; // pts/dts of the lead-in keyframe
	int64_t visible_start_usec = 0; // first frame the operator should see
	int64_t end_usec = 0;

	bool empty() const { return packets.empty(); }
};

class RingBuffer {
public:
	// Maximum history to retain. Older packets are dropped as new ones arrive.
	void set_max_duration(double seconds);

	// SPS/PPS / codec extradata, captured once from the encoder. FFmpeg cannot
	// decode the ring's packets without this - it is the single most common
	// reason replay decoding fails. See replay-output.cpp.
	void set_video_extradata(const uint8_t *data, size_t size);

	// Take a reference to a freshly encoded packet and trim anything now older
	// than the configured window. Safe to call from the encoder callback.
	void push(const encoder_packet *packet);

	// Drop everything (e.g. on encoder restart / settings change).
	void clear();

	// Newest decode timestamp currently held, or 0 if empty.
	int64_t latest_usec() const;

	// Extract the most recent `total_ns` of footage ending at the newest packet
	// held, starting decode from the keyframe at/before that point. Returns
	// false if there is not enough buffered video yet. The returned clip owns
	// its own packet refs.
	bool extract_recent(int64_t total_ns, ReplayClip &out) const;

	// Extract an explicit [start_usec, end_usec] window (mark-in/mark-out, V2).
	bool extract_range(int64_t start_usec, int64_t end_usec, ReplayClip &out) const;

private:
	// Assumes mutex_ held.
	bool extract_range_locked(int64_t start_usec, int64_t end_usec, ReplayClip &out) const;
	void trim_locked();

	mutable std::mutex mutex_;
	std::deque<StoredPacket> packets_;
	std::vector<uint8_t> video_extradata_;
	int64_t max_duration_usec_ = 30LL * 1'000'000; // 30 s default
};
