# OBS Instant Replay

A custom in-memory replay engine for OBS Studio, built for **live sports**. It
keeps the last N seconds of the program feed as *encoded* video in a RAM ring
buffer and plays it back through a custom async source — skipping the
mux-to-disk step that makes OBS's built-in Replay Buffer too slow for
"press the button, see it now" sports replays.

> **Status: V1 working.** Builds against libobs and runs in **OBS Studio 32.1.2+
> (Windows x64)**: the RAM ring buffer, FFmpeg decode, timestamp-stretched slow
> motion, the opt-in buffer toggle, and the Qt operator dock are all functional.
> Because the plugin links libobs directly, the release build targets a specific
> OBS major version — see the Release notes for the supported OBS version.
> See [What's real vs TODO](#whats-real-vs-todo).

## Architecture

```
OBS program mix
      │  (dedicated low-latency H.264 encoder: short GOP, no B-frames, CBR)
      ▼
instant_replay_output           ← custom obs_output (src/replay-output.cpp)
      │  encoded_packet callback
      ▼
RingBuffer                      ← encoded RAM ring, keyframe-indexed
      │                           (src/ring-buffer.cpp)
      │  "Take replay" pressed
      ▼
ReplayController                ← post-roll wait, clip extract, scene switch
      │                           (src/replay-controller.cpp)
      ▼
Decoder (FFmpeg)                ← decodes clip from RAM, seeded with extradata
      │                           (src/decoder.cpp)
      ▼
instant_replay_source           ← async obs_source, stretched timestamps = slo-mo
      │                           (src/replay-source.cpp)
      ▼
"Replay" scene → auto-return to live
```

### Files

| File | Responsibility |
|------|----------------|
| `src/plugin-main.cpp` | Module entry; registers types + hotkeys; starts the controller on `FINISHED_LOADING`. |
| `src/ring-buffer.{hpp,cpp}` | Thread-safe encoded packet ring; keyframe indexing; clip extraction from the prior IDR. |
| `src/replay-output.{hpp,cpp}` | Custom `obs_output` that captures encoded packets + the encoder **extradata (SPS/PPS)**. |
| `src/decoder.{hpp,cpp}` | FFmpeg H.264/H.265 decode wrapper, seeded with extradata. |
| `src/replay-source.{hpp,cpp}` | Async source; worker thread decodes and pushes frames with stretched timestamps. |
| `src/replay-controller.{hpp,cpp}` | Dedicated replay encoder, take-replay flow, scene switch/return, hotkeys. |

## Using it

1. Add a scene named **`Replay`** containing an **`Instant Replay`** source.
2. Bind the hotkeys in **Settings → Hotkeys**:
   - *Instant Replay: Toggle Buffer (on/off)* — starts/stops buffering (opt-in, so
     it never touches the GPU encode pipeline until you turn it on).
   - *Instant Replay: Angle 1–4 (8s @ 50%)* — take a replay of that angle.
   - *Instant Replay: return to live* — cut back early.
3. Use the **OBS Instant Replay** dock (or the toggle button added to the
   **Controls** area) to enable buffering and pick which scenes to buffer as angles.
4. Press an angle hotkey — it cuts to the Replay scene, plays the slo-mo clip,
   and returns to the live scene automatically.

## What's real vs TODO

**Implemented & working in OBS 32.1.2 (Windows x64):**
- Encoded RAM ring buffer with keyframe-safe trimming and prior-IDR clip extraction.
- Extradata capture from the encoder (the #1 decode-failure cause — handled).
- FFmpeg decoder wrapper (single-threaded, no-drop feed for a clean reference chain).
- Timestamp-stretch slow motion (0.5×) with first-frame preload.
- Dedicated low-latency replay encoder (native canvas res, so the stream's SPS
  matches the coded frames) + custom output, opt-in so it coexists with the
  operator's real stream/recording encoder.
- Qt operator dock + Controls-area toggle button + status-bar indicator.
- Multi-angle buffering (pick scenes in the dock), post-roll wait, scene
  switch, auto-return, and per-angle hotkeys.

**TODO (flagged inline with `TODO`):**
- B-frame-safe PTS handling (V1 assumes `bframes=0`, so decode order = display order).
- Colorspace/range detection (V1 assumes BT.709 limited).
- Auto-create/manage the `Replay` scene + source (V1 requires manual setup).
- Async-buffer / preload-lead tuning to hit the latency targets; **measure real
  trigger-to-picture before trusting the 100–300 ms figure**.
- Slowed/pitch-preserved/crowd audio (V1 mutes replay audio; output is video-only).
- Detached take-replay thread vs. `shutdown()` lifetime hardening.

## Building

Standard OBS plugin template flow (deps are fetched by `buildspec.json`; FFmpeg
comes from the obs-deps prebuilt package):

```powershell
# Windows (from repo root)
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

CMake options (set in `CMakeLists.txt`): `ENABLE_FRONTEND_API=ON` (required for
scene switching + hotkeys) and `ENABLE_QT=ON` (required for the operator dock).

Prebuilt installers for tagged releases are produced by GitHub Actions — see the
[Releases](../../releases) page.

## Roadmap

- **V1 (this scaffold):** program-feed replay, 50/75/25% speed, hotkeys, auto scene switch.
- **V2:** operator dock, mark-in/out, post-roll control, replay queue, Stream Deck / WebSocket, save-to-disk, PiP, audio options.
- **V3:** 120 FPS capture, frame blending, optical-flow interpolation, multi-camera buffers, preview/program.

---

Built from the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate).
The GitHub Actions CI, packaging, and `cmake/` helpers from the template are retained.
