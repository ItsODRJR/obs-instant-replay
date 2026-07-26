/*
OBS Instant Replay - encoded RAM ring buffer implementation
*/

#include "ring-buffer.hpp"

#include <plugin-support.h>

#include <algorithm>
#include <utility>

// ---------------------------------------------------------------------------
// StoredPacket
// ---------------------------------------------------------------------------

StoredPacket::StoredPacket(const encoder_packet *src)
{
	// Copy metadata, then deep-copy the bitstream. We must NOT retain the
	// encoder's pointer or ref it (see the note in ring-buffer.hpp).
	packet = *src;
	packet.encoder = nullptr; // don't hold a dangling encoder pointer
	if (src->data && src->size)
		data.assign(src->data, src->data + src->size);
	packet.data = data.data();
	packet.size = data.size();
}

StoredPacket::StoredPacket(const StoredPacket &other) : packet(other.packet), data(other.data)
{
	packet.data = data.data(); // re-point at our own copy
	packet.size = data.size();
}

StoredPacket &StoredPacket::operator=(const StoredPacket &other)
{
	if (this != &other) {
		packet = other.packet;
		data = other.data;
		packet.data = data.data();
		packet.size = data.size();
	}
	return *this;
}

StoredPacket::StoredPacket(StoredPacket &&other) noexcept
	: packet(other.packet), data(std::move(other.data))
{
	packet.data = data.data(); // buffer moved with us; re-point to be safe
	packet.size = data.size();
	other.packet = {};
}

StoredPacket &StoredPacket::operator=(StoredPacket &&other) noexcept
{
	if (this != &other) {
		packet = other.packet;
		data = std::move(other.data);
		packet.data = data.data();
		packet.size = data.size();
		other.packet = {};
	}
	return *this;
}

// ---------------------------------------------------------------------------
// RingBuffer
// ---------------------------------------------------------------------------

void RingBuffer::set_max_duration(double seconds)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (seconds < 1.0)
		seconds = 1.0;
	max_duration_usec_ = static_cast<int64_t>(seconds * 1'000'000.0);
	trim_locked();
}

void RingBuffer::set_video_extradata(const uint8_t *data, size_t size)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (data && size)
		video_extradata_.assign(data, data + size);
	else
		video_extradata_.clear();
}

void RingBuffer::push(const encoder_packet *packet)
{
	if (!packet || !packet->data)
		return;

	std::lock_guard<std::mutex> lock(mutex_);
	packets_.emplace_back(packet);
	trim_locked();
}

void RingBuffer::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	packets_.clear();
}

int64_t RingBuffer::latest_usec() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return packets_.empty() ? 0 : packets_.back().dts_usec();
}

void RingBuffer::trim_locked()
{
	if (packets_.empty())
		return;

	const int64_t newest = packets_.back().dts_usec();
	const int64_t cutoff = newest - max_duration_usec_;

	// Never trim past the keyframe that a still-in-window packet would need to
	// decode. We keep dropping from the front only while the *next* packet is
	// itself a keyframe older than the cutoff (so decoding always has an IDR).
	while (packets_.size() > 1 && packets_.front().dts_usec() < cutoff) {
		// Peek: only drop the front if the following packet is not relying on
		// it as its decode anchor, i.e. keep at least one keyframe <= cutoff.
		if (packets_.size() >= 2 && packets_[1].dts_usec() < cutoff) {
			packets_.pop_front();
		} else {
			break;
		}
	}
}

bool RingBuffer::extract_recent(int64_t total_ns, ReplayClip &out) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (packets_.empty())
		return false;

	const int64_t end_usec = packets_.back().dts_usec();
	const int64_t start_usec = end_usec - (total_ns / 1000);
	return extract_range_locked(start_usec, end_usec, out);
}

bool RingBuffer::extract_range(int64_t start_usec, int64_t end_usec, ReplayClip &out) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return extract_range_locked(start_usec, end_usec, out);
}

bool RingBuffer::extract_range_locked(int64_t start_usec, int64_t end_usec, ReplayClip &out) const
{
	if (packets_.empty())
		return false;

	// Find the video keyframe at or before start_usec: playback must begin from
	// an IDR because H.264/H.265 cannot start decoding mid-GOP. We scan for the
	// last keyframe whose timestamp is <= start_usec.
	int decode_anchor = -1;
	for (size_t i = 0; i < packets_.size(); ++i) {
		const StoredPacket &p = packets_[i];
		if (p.dts_usec() > start_usec)
			break;
		if (p.is_video() && p.is_keyframe())
			decode_anchor = static_cast<int>(i);
	}

	if (decode_anchor < 0) {
		obs_log(LOG_WARNING,
			"replay: no keyframe found before requested start "
			"(buffer too short or GOP too long)");
		return false;
	}

	out.packets.clear();
	out.video_extradata = video_extradata_;
	out.decode_start_usec = packets_[decode_anchor].dts_usec();
	out.visible_start_usec = start_usec;
	out.end_usec = end_usec;

	// Copy every packet from the anchor through end_usec (video + audio). Each
	// StoredPacket owns its own byte buffer, so the live ring can keep trimming.
	for (size_t i = static_cast<size_t>(decode_anchor); i < packets_.size(); ++i) {
		const StoredPacket &p = packets_[i];
		if (p.dts_usec() > end_usec)
			break;
		out.packets.push_back(p);
	}

	return !out.packets.empty();
}
