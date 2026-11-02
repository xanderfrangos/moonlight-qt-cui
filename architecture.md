# Streaming and VRR architecture

Source baseline: master plus the decode-buffer accounting correction on 2026-10-30. The production worker
resolves one adaptive input policy, bounded catch-up and corrected input
accounting. The calibration key includes effective policy and presenter
capabilities; existing keys are not reused. Schema-6 capture and schema-2
replay configs support this current policy. Old compressed captures can still
be decoded, but older controller revisions require a fresh capture for replay.
The three former experimental environment switches are ignored with a warning.
Smoothing uses named off, worker phase-locked, native tolerance-bounded and
Absorb modes. Retired interval-scoring/release selectors and metronome state
are absent from the current capture and replay parameter contract. The follow-up
audit also removes metronome-specific frame dropping, its constant trace column,
the old absolute render ceiling, base-guard latch exit, and pre-schema queue
budget/disabled-budget alternatives. Current pacing budgets require a nonzero
divisor. The unused extra-frame session flag is also removed from traces,
replay and calibration identity; calibration keys use version 3.
The startup preparation predictor retains the later master safeguard: it uses
the bounded lead until eight samples exist and caps the learned prediction at
one source/display period. At the display ceiling, the current controller may
request replacement to shed standing phase; the worker reserves a proven-ready
successor first and preserves its observed decode boundary and wait. Replay
joins a recorded reservation to that successor's lifecycle. These are software
contracts, not proof of optical display timing.
See [historical streaming notes](docs/streaming-history.md)
and [master VRR history](docs/master-vrr-history-20261029.md) for past revisions;
they are not evidence of current behavior.

This fork's baseline: its own rendering, pacing, overlay, controller UI and
timestamp pacing changes (section 16) merged with upstream `master` at
`76ee5025` (2026-11-01). The fork's VRR and timestamp pacing traces share
`TraceFile` (pacer/tracefile.h); upstream's split-out `vrrpacingtrace.cpp`
writes through it instead of its own inline file writer.

## 1. Fundamental model

Moonlight is the client. The host captures and encodes video, sends compressed
frames, and receives input. The client receives and repairs packets, assembles
compressed frames, decodes them, renders the resulting image, and submits it for
display. Audio and input have their own queues and timing paths.

VRR changes the scheduling and presentation of decoded video. It cannot create
a missing host frame, reverse network loss, or remove the time already spent
capturing, encoding, transporting, and decoding. It can absorb some variability
by delaying frames to a more regular schedule. That costs latency, so the
implementation constrains both the frame queue and the learned delay.

```text
Host capture / encode                           [outside this client]
    |
    | UDP video: RTP timestamp + NV frame/packet identity + FEC
    v
VideoReceiveThreadProc -> RtpvAddPacket -> processRtpPayload
    | packet ordering, repair, access-unit assembly, recovery
    v
Bounded compressed-frame queue
    |
    v
FFDecoder thread: pull decode unit -> avcodec_send_packet
    |                            -> avcodec_receive_frame
    | AVFrame + retained identity/timing metadata + decode GPU boundary
    v
Pacer selection
    +-- legacy queues / V-sync source / renderer
    |
    +-- VrrPacingWorker: bounded decoded-frame queue
          -> establish the backend decode dependency; record any CPU wait
          -> controller computes source slot, target, render-start deadline
          -> discard stale work when a newer frame is available
          -> wait for render start
          -> prepare GPU rendering and establish completion/ownership state
          -> release only a source the presenter marks reusable
          -> wait for target and applicable submission floor
          -> recheck lifecycle -> verify deferred readiness -> presentAdaptive
          -> submission/native feedback -> future controller decisions
          -> retire deferred source ownership only at a safe backend boundary
          -> asynchronous trace writer

Audio UDP -> audio RTP queue -> renderer packet queue -> Opus on device pull
SDL input events -> input queue / sender -> host
```

Keep these separate: intended source slot, scheduled CPU submission, GPU
readiness, actual native call, OS presentation feedback, and physical scanout.
They are related observations, not interchangeable timestamps.

## 2. Source map and reading order

Paths are relative to the repository. The repeated `moonlight-common-c` directory
is intentional: the outer directory contains the qmake wrapper and the inner
directory contains the common library.

| Area | Source and entry points |
| --- | --- |
| User preferences | [streamingpreferences.cpp](app/settings/streamingpreferences.cpp): `reload()`, `save()`; [SettingsView.qml](app/gui/SettingsView.qml) |
| Session orchestration | [session.cpp](app/streaming/session.cpp): `snapshotPresentationSettings()`, `initialize()`, `drSubmitDecodeUnit()`, stream event loop |
| FPS recommendations | [vrrratepolicy.cpp](app/streaming/vrrratepolicy.cpp) |
| Protocol configuration | [Limelight.h](moonlight-common-c/moonlight-common-c/src/Limelight.h), [Connection.c](moonlight-common-c/moonlight-common-c/src/Connection.c), [SdpGenerator.c](moonlight-common-c/moonlight-common-c/src/SdpGenerator.c), [RtspConnection.c](moonlight-common-c/moonlight-common-c/src/RtspConnection.c) |
| Packet ingress | [VideoStream.c](moonlight-common-c/moonlight-common-c/src/VideoStream.c): `VideoReceiveThreadProc()`; [Video.h](moonlight-common-c/moonlight-common-c/src/Video.h) |
| Packet repair and assembly | [RtpVideoQueue.c](moonlight-common-c/moonlight-common-c/src/RtpVideoQueue.c): `RtpvAddPacket()`; [VideoDepacketizer.c](moonlight-common-c/moonlight-common-c/src/VideoDepacketizer.c): `processRtpPayload()`, `requestDecoderRefresh()` |
| Decoder | [ffmpeg.cpp](app/streaming/video/ffmpeg.cpp): `ffGetFormat()`, `submitDecodeUnit()`, decoder thread; [ffmpeg.h](app/streaming/video/ffmpeg.h) |
| Renderer abstraction | [renderer.h](app/streaming/video/ffmpeg-renderers/renderer.h): `IFFmpegRenderer` |
| Pacer mode selection | [pacer.cpp](app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp) |
| Frame and presenter contracts | [vrrtypes.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtypes.h), [ivrrframepresenter.h](app/streaming/video/ffmpeg-renderers/ivrrframepresenter.h) |
| VRR execution and tracing | [vrrpacingworker.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.cpp), [vrrpacingtrace.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrrpacingtrace.cpp) |
| Deadline waiting | [vrrtargetwaiter.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.cpp) |
| Timing policy | [vrrtimingcontroller.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.cpp), [vrrtimingcontroller.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h) |
| Active learning models | [prediction.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/prediction.h), [reserve.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/reserve.h), [smoothnessfeedback.h](app/streaming/video/ffmpeg-renderers/pacer/vrr/smoothnessfeedback.h) |
| Calibration persistence | [profile.cpp](app/streaming/video/ffmpeg-renderers/pacer/vrr/profile.cpp) |
| Windows native presentation | [d3d11va.cpp](app/streaming/video/ffmpeg-renderers/d3d11va.cpp), [d3d11composition.cpp](app/streaming/video/ffmpeg-renderers/d3d11composition.cpp) |
| macOS native presentation and display range | [vt_metal.mm](app/streaming/video/ffmpeg-renderers/vt_metal.mm), [macdisplaytiming.mm](app/streaming/video/ffmpeg-renderers/macdisplaytiming.mm) |
| PyroWave decode and framing | [pyrowavedecoder.cpp](app/streaming/video/pyrowave/pyrowavedecoder.cpp), [pyrowaveframing.cpp](app/streaming/video/pyrowave/pyrowaveframing.cpp) |
| macOS PyroWave shared output | [pyrowavemetal.mm](app/streaming/video/pyrowave/pyrowavemetal.mm) |
| macOS PyroWave calibration draw | [pyrowavemetalcalibrator.mm](app/streaming/video/pyrowave/pyrowavemetalcalibrator.mm) |
| Replay and its contract | [vrrreplay.cpp](tests/vrr/vrrreplay.cpp), [VRR test README](tests/vrr/README.md) |
| Statistics | [decoder.h](app/streaming/video/decoder.h): `VIDEO_STATS`; overlay formatting in `ffmpeg.cpp` |

For a timing change, start with the resolved session parameters, follow
`VrrPacingWorker` into `schedule()`, then follow the actual presenter call and
feedback back into the controller. Read replay only after understanding what
the live path records and which parts replay holds fixed.

## 3. Settings, negotiation, and mode selection

### 3.1 Preferences are not proof of an active mode

`StreamingPreferences` persists ordinary settings through `QSettings`.
At the inspected revision, V-sync defaults on and VRR defaults off. The VRR
timing selector defaults to Balanced Target for new users, with the saved-checkbox
migration described above. Reduce judder defaults on, preserves the saved
`smoothvrrframetiming` choice, and enables the moderate cadence smoother below.
Legacy frame pacing defaults off. The default requested stream is 720p60.
These are defaults, not evidence of the user's current saved settings.

The FPS picker is advisory and independent of the VRR setting. It offers fixed
30 and 60 FPS, usable native display rates, and a plain below-refresh choice at
`floor(refresh - refresh^2 / 3600)` (116 FPS at 120 Hz). A saved custom FPS
remains selectable. Toggling VRR does not rewrite saved FPS;
`m_StreamConfig.fps` receives the requested preference.

`snapshotPresentationSettings()` resolves that request for the session:

0. Start from the saved window mode, but use borderless desktop fullscreen
   whenever the controller UI (`SystemProperties::isTvMode()`) is active for
   this launch. The saved preference is not rewritten.
1. Query the actual window display refresh. An unavailable refresh may fall
   back to 60 Hz for legacy behavior, but that fallback cannot qualify VRR.
2. Resolve effective V-sync. Under Gamescope it is always on and never
   disabled, and fixed frame pacing is off (see "Gamescope owns V-Sync" in
   section 16). Elsewhere, a requested FPS over refresh plus 5 disables it.
3. Require readable refresh, effective V-sync, and stream FPS no greater than
   display refresh for VRR.
4. Force effective desktop fullscreen when VRR is accepted, keeping the saved
   window preference intact. macOS uses native Cocoa fullscreen/Spaces for it;
   Windows/Linux retain the borderless desktop path.
5. If VRR was requested but rejected and effective V-sync remains enabled,
   enable fixed pacing even if the separate legacy pacing checkbox is off
   (not under Gamescope).

On macOS, the strict refresh query first uses `NSScreen.maximumFramesPerSecond`.
The FPS picker uses the same native maximum; a ProMotion mode whose SDL refresh
is zero therefore contributes its actual 120 Hz choices. Session VRR additionally
requires finite, positive native intervals and
`maximumRefreshInterval - minimumRefreshInterval > 1 us`. A fixed-refresh
screen cannot qualify merely by advertising a high maximum FPS. The window's
actual screen is queried on the main thread. Display-list queries match SDL
desktop bounds to CoreGraphics display IDs instead of assuming enumeration order.
Moving an active VRR window recreates the Metal renderer; an unavailable or
fixed range, or changed maximum refresh, disables VRR for that connection and
uses fixed pacing. Reconnect to qualify a later display again.
Before initializing SDL video, a requested Mac VRR+V-sync session forces
`SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=1`. This overrides saved borderless/notch
presentation choices for that connection because SDL caches the hint at Cocoa
driver startup. The Metal presenter additionally requires that the Cocoa window
actually entered native fullscreen; the hint alone does not qualify playback.

The renderer must subsequently support the mode. The Pacer constructs
`VrrSessionConfig`, fixes `allowAdditionalQueuedFrame=false`, passes smoothing
settings, checks presenter support, and starts the worker.
Unsupported presentation or failed worker initialization falls back to the
legacy path. A UI checkbox alone cannot establish DXGI capability, active
adaptive presentation, or that the physical panel is varying refresh.

`tracevrrframes` defaults to false. The Settings checkbox enables diagnostic
frame tracing only, for VRR Pacing Mode or (since 2026-10-08) timestamp pacing,
with export of the latest completed recording and a button
to open `Desktop/vrr-diagnostics`. The platform's Desktop location is resolved
through Qt, rather than hardcoding an English profile path. The session owns its
environment wrapper after acquiring the active-session semaphore, restoring it
only after decoder shutdown
and logger drain. Existing external trace launchers take precedence. Recording is
fixed for the stream, including decoder recreation; full UI/file/lifecycle
contracts are in [diagnostics](docs/vrr-diagnostics.md).

### 3.2 Host frame-limiter discovery

Vibeshine advertises optional `/serverinfo` fields `FrameLimiterSupported`,
`FrameLimiterEnabled`, `VirtualDisplayFrameLimiterEnabled` (integer booleans),
and
`FrameLimiterFpsLimitMilliHz` (zero follows stream FPS). The protocol is
brand-independent and can also be implemented by Vibepollo. Missing flags
mean no advertised integration; capabilities are ephemeral and refreshed on
host polling, including clearing them after downgrading a host.

`FrameLimiterEnabled` reports limiting for the configured host display path:
either the manual limiter is enabled with an applicable provider, or virtual
display mode is selected and automatic virtual-display limiting is enabled.
Windows does not require the manual checkbox for the automatic path. Linux
also requires a selected Linux limiter provider. The separate virtual-display
flag reports automatic policy availability, not activation on physical outputs.
These are configuration reports, not per-game proof of provider availability,
successful application, or app/client display overrides.

The game list no longer displays a frame-limiter warning. Capability discovery
remains available internally. Settings describe full-refresh streaming without requiring a host limiter.

### 3.3 What the host is told

Session setup fills `STREAM_CONFIGURATION` with FPS, dimensions, bitrate,
colorspace/range, encryption, codec capabilities, and other connection choices.
`clientRefreshRateX100` is a refresh hint in Hz times 100; SDP emits it as
`x-nv-video[0].clientRefreshRateX100`. RTSP negotiation determines the actual
codec/profile using host SDP and server codec flags, with AV1/HEVC/H.264 paths.
Renderer setup receives that negotiated format before video streaming starts.

Packet size is aligned down to a 16-byte multiple for FEC. Connection setup
also applies route-dependent packet-size limits. These are transport decisions,
not VRR scheduling parameters.

When VRR presentation qualifies for the session
(`m_PresentationSettings.enableVrr`, decided before launch), `NvHTTP::startApp`
adds `clientVrrRequested=1` to the launch/resume query. Hosts that do not know
it ignore it. Vibeshine uses it for a low-latency WGC buffer and, in its
Automatic virtual-display capture mode (`vibe-test` 09b09ae8), holds the
virtual display at 1000 Hz. WGC stamps frames at DWM composition, which lands
on the virtual display's refresh grid; a 4x display at 116 FPS put every RTP
timestamp on a 2.156 ms grid (capture 20260922-195738 deep trace), while 1000 Hz
bounds that to 1 ms. Vibeshine also refines stamps from the game's DXGI Present
events at send time. This is host behavior the client only requests.

The client source proves which values it sends and how it interprets the
response. It does not prove how a particular Sunshine/GFE version captures,
timestamps, paces, or encodes in response. A refresh hint is not host/client
clock synchronization.

## 4. Packet ingress, frame assembly, and recovery

The video receive thread reads UDP into staging buffers, optionally decrypts
AES-GCM payloads, converts network-order RTP fields, and transfers packets to
`RtpvAddPacket()`. Video packets contain RTP sequence/timestamp identity plus
an NV header with stream packet index, frame index, SOF/EOF/picture-data flags,
and FEC metadata. RTP timestamps are in a 90 kHz domain.

The socket receive buffer accommodates roughly 2048 packets to absorb bursts.
Connection watchdogs detect prolonged absence of traffic or successful frames.
The receive path is not a frame scheduler: it should advance valid compressed
data without intentional presentation waiting.

`RtpVideoQueue` groups by frame and FEC block, tracks ordering and missing data,
and uses Reed-Solomon parity to recover losses when possible. Completed data
packets are delivered in order to the depacketizer. Unrecoverable gaps advance
recovery and notify the host. FEC status and frame-loss control messages should
not be described as individual RTP packet retransmission.

One frame's first receive time is reused for its packets. That gives a stable
assembly-duration boundary without taking a clock sample for every packet.
`processRtpPayload()` validates stream continuity, strips headers, identifies
frame boundaries/types, and assembles codec access units. H.264/HEVC Annex-B
NAL units become a linked buffer chain; IDR setup includes codec parameter sets.
AV1 follows its appropriate picture-data assembly path.

The resulting `DECODE_UNIT` owns the compressed buffer chain and carries
frame identity and timing metadata. Non-direct operation uses a bounded queue
of 15 decode units. Queue overflow flushes queued compressed frames and requests
IDR recovery, rather than allowing latency to grow without bound. Direct-submit
renderers can instead submit on the receive/depacketizer path, but the production
FFmpeg decoder advertises pull-renderer capability and owns its decoder thread.

Loss recovery respects codec dependencies. Dropping an arbitrary compressed
reference frame is different from discarding an already-decoded presentation
frame. Invalid packet continuity can drop the current frame and trigger IDR or
reference-frame invalidation recovery depending on capabilities. Repeated drops
eventually force IDR recovery; the inspected code has a 120-consecutive-drop
threshold. `requestDecoderRefresh()` flushes pending units and defers assembly
state reset to a suitable boundary. Completion releases the compressed buffers;
successful IDR completion establishes valid reference state.

## 5. Clock domains and latency boundaries

| Value | Domain / units | Meaning and limitation |
| --- | --- | --- |
| RTP timestamp | Host-origin 90 kHz counter | Source timing identity; wraps and must be unwrapped. Not a client wall-clock timestamp. |
| `presentationTimeUs` | Relative source presentation time, microseconds | Normally derived from RTP. Missing Sunshine PTS can use elapsed local receive time as fallback. |
| `receiveTimeUs` | Client monotonic microseconds | First packet arrival for the frame. Not host capture time. |
| `enqueueTimeUs` / reassembled time | Client monotonic microseconds | Complete compressed frame assembled/queued. |
| `decodeSubmitUs` | Client monotonic microseconds | Sampled immediately before FFmpeg packet submission. |
| `decoderOutputUs` | Client monotonic microseconds | Immutable timestamp captured immediately when FFmpeg returns the decoded frame; client-processing reporting origin. |
| `decodeCompleteUs` | Client monotonic microseconds | Post-output readiness observation; anchors production RTP-to-client mapping so hardware decode duration is absorbed in the sender offset. Linux samples it after `vaSyncSurface()`, Windows after the monitored decode-to-render fence wait. Every proven D3D11 completion, including a short wait or an already-ready fence, retains a post-observation upper bound. Historical captures and backends without completion evidence retain their output-time mapping fallback. |
| `decodeSyncWaitUs` | Elapsed client microseconds | Explicit CPU time spent by the worker on a decoder/backend completion primitive. It is serial service and latency accounting, not a source-clock timestamp. A zero value does not exclude a GPU-queued dependency. |
| Worker queue, decision, preparation, wait, submission times | Client monotonic microseconds | Distinct CPU-side lifecycle boundaries. |
| Shared fence values | GPU ordering identities | Establish dependencies/completion; not elapsed time by themselves. |
| Native DXGI QPC fields | QPC ticks plus frequency/correlation | OS timing evidence requiring identity and clock mapping. |
| Metal `MTLDrawable.presentedTime` | Core Animation seconds | OS-reported drawable presentation event, freshly correlated with `CACurrentMediaTime()` inside a bracket on the client clock; not the native call/return time or physical panel measurement. |
| Host processing latency | 1/10 millisecond units | Host-reported aggregate when present; zero means unavailable/inapplicable. |
| Audio samples | Audio stream/device cadence | Independent of video target scheduling. |
| `Reserve` internal time | Nanoseconds | Convert explicitly at the controller/model boundary. |

`LiGetMicroseconds()` calls the common platform clock. On Windows that is elapsed
QPC time from an opaque local epoch. Decoder and pacer timestamps therefore share
a monotonic domain. They are not automatically synchronized to host, GPU, audio
hardware, or panel clocks.

Useful differences are:

```text
assembly       = enqueueTimeUs       - receiveTimeUs
pre-submit     = decodeSubmitUs      - enqueueTimeUs
decode         = decoderOutputUs     - decodeSubmitUs
post-decode    = submissionTimeUs    - decoderOutputUs
client ingress = submissionTimeUs    - receiveTimeUs
client processing = presentationCallEndUs - decoderOutputUs
rendering         = preparationDurationUs + presentationCallDurationUs
queue/pacing      = client processing - rendering - explicitDecodeSyncWait
```

Check validity and ordering before subtracting. Post-decode includes queueing,
GPU dependencies, rendering/preparation, scheduler delays, deliberate pacing,
and native submission behavior. It is not simply the configured playout delay.
The performance overlay retains frame queue delay and rendering
time, without a separate client-processing row. These use the current queue/pacing
and rendering quantities for successfully presented frames, with one shared frame count.
Queue/pacing excludes the explicit worker GPU decode wait. In advanced tracing,
the two components plus the separate GPU decode synchronization line partition client processing.
Queue/pacing includes queue residence, target waits and other time outside
preparation, presentation and explicit decode synchronization. “Client processing
delay” ends when the presentation call returns. It does not include unmeasured
time from that return until the image becomes visible on the display.
On Windows with monitored fences, the worker waits for the decode fence before
scheduling, and that wait is the GPU decode synchronization line, as on Linux. The
GPU-side decode-to-render wait remains queued. The verified present-ready fence
wait occurs inside preparation, before the cadence hold, and is not a
decode-wait row. Its duration remains part of rendering time.
Linux VAAPI/Vulkan Mailbox output is asynchronous and
does not report a CPU output-completion sample; other Linux imports still poll
before the target hold. These accounting identities therefore partition the
observed CPU path; they do not expose every GPU stage.
None of these differences alone measures click-to-photon or glass-to-glass
latency. RTT is a round trip, not measured one-way video delay.

The controller keeps source periods in Q16 fixed point where needed. Do not
collapse that to rounded milliseconds when reasoning about long-run drift.
RTP-to-microsecond conversion and epoch/wrap handling must be followed at the
specific use site; a converted RTP number still needs a client-clock mapping.

## 6. Decoder ownership and renderer handoff

The production FFmpeg decoder starts a dedicated `FFDecoder` thread after
renderer setup. It waits through `LiWaitForNextVideoFrame()` when no packets are
outstanding and interleaves pulling compressed input with draining FFmpeg output
when work is in flight. “Pull decoder” describes ownership of the common-library
queue; FFmpeg still uses `avcodec_send_packet()` and `avcodec_receive_frame()`.
Individual FFmpeg codecs may execute more work at either call.

`submitDecodeUnit()` requires a suitable initial IDR, tracks frame-number gaps,
copies the `LENTRY` chain into the reusable packet buffer, and sends the packet.
After successful submission it retains a metadata copy and submission timestamp
in matching queues. The original compressed payload pointers become invalid
after completion and must not be retained as frame storage.

For each output `AVFrame`, the decoder associates queued metadata with that
output, stamps decode completion, and copies frame number, RTP identity,
receive/reassembly time, and decode-submit time into the VRR frame record.
Legacy rendering uses `frame->pts` for source timing and a local `pkt_dts` handoff
timestamp for queue measurements. Those fields should not be substituted for
the explicit VRR timing fields.

`ffGetFormat()` selects the pixel format expected by the chosen renderer and
refuses an incompatible ordinary FFmpeg fallback. Windows has DXVA2 and D3D11VA
paths; software output and other platforms use their corresponding renderers.
Hardware decode can keep image data on the GPU. An `AVFrame` being available
does not by itself mean every GPU read/write dependency has completed.

`Session::drSubmitDecodeUnit()` also protects decoder lifetime with a try-lock:
decoder destruction has main-thread/API constraints, and units can be ignored
while that lock is held, with refresh recovery after recreation. The FFmpeg
pull path is the important steady-state path for this fork.

Legacy Pacer queues drop old frames at their bounds and move frames according
to the V-sync/render path. They defer freeing a rendered frame to protect GPU
use. VRR replaces that pacing mechanism with its worker and explicit presenter
contract; it does not replace network assembly or codec reference handling.

## 7. VRR worker: queue, execution, and lifecycle

### 7.1 Queue ownership and backpressure

The VRR queue admits four waiting frames plus one active frame in every
profile (`playout_queue_frames`; Smooth alone until 2026-09-26, and 0 = the
historical three in older captures). Separately, profile playout-delay
allowances default to half, one, or four fitted source frames for Low Latency,
Balanced Target, or Smooth and can be customized. The queue limit in
`playoutQueueLimitUs()` remains a safety
bound on those allowances:
waiting frames x period, minus render lead and the full Reduce judder retiming
budget. With three frames at 116 FPS that budget was ~16.9 ms once the retiming
cap rose to 6 ms, so Smooth's 24 ms ceiling was unreachable and live overlays
showed "limit 16.86 ms" (capture 20260922-221707). The decoder pool reserves
the extra surface (`extra_hw_frames` = classic pacer outstanding frames +
`VrrLargestQueuedFrames` - `VrrMaximumQueuedFrames`). This is a
decoded-frame queue, separate from the 15-unit compressed queue and native
swapchain buffers. Do not add these counts and treat the result as a fixed
latency: the queues have different owners, lifetimes, and service rates.
Backend-held source references can outlive the active worker step without
increasing decoded-frame admission. Their limits protect decoder-surface lifetime,
not a target amount of playout buffering.

At `submit()`, the presenter captures the decode boundary before subsequent
decoder GPU work can be queued. Under the queue mutex, stopped/suspended workers
reject frames; a full queue evicts the oldest waiting frame, marks a discontinuity,
and admits the new frame. Trace/counter work occurs outside the queue lock.

The worker also sheds stale work when a fresher queued successor exists and
age/capacity criteria apply (historical snapshots also retain floor-debt
and missed-tick rejection). All non-metronome profiles use a
two-source-period age allowance. Balanced Target and Low Latency measure it from
pacer admission, while Smooth uses the scheduled target for its second
check. A lone late frame may still be shown.
This differs from throwing away compressed reference frames and does not require
resetting the codec merely because an image was not presented.

### 7.2 One normal frame

1. Wake and consume pending window notifications. Before dequeuing for a blocking
   decode wait, reject expired queue fronts only while a newer adjacent source
   frame exists. Use two periods of the larger of fitted cadence and successor
   RTP spacing (four for historical metronome); skip this check on pending rebase
   or discontinuous stamps. Record `queue_stale` without a controller decision.
   The sole image remains eligible. Then dequeue the retained frame.
2. Check stop/suspend state and establish this frame's decode dependency. Record
   any explicit CPU wait as serial service. Production at `18602b1c` maps from
   decode completion; immutable `decoderOutputUs` remains the full-latency
   origin. Historical captured parameters can instead select decoder output.
3. Ask `VrrTimingController::schedule()` for the target, render-start deadline,
   latch request, and diagnostics using the current monotonic time.
4. Apply stale replacement policy when newer work is available.
5. Wait until render start, then recheck window/display epoch and lifecycle.
6. Call the presenter's `prepareFrame()` with the captured decode dependency
   and the selected `VrrPresentRequest`. Mode changes, rendering, and image
   acquisition belong inside this measured preparation interval; intentional
   target waiting does not. D3D11 keeps its mode selection at Present; Linux
   Vulkan keeps the swapchain's startup-selected mode.
7. If preparation supplies a completed renderer wait, feed that measured interval
   into the controller's bounded readiness history. A presenter may instead
   report completion after the target hold, or leave it unavailable. Future
   frames may start rendering earlier by an available learned lead; the source
   target and native latch decision are unchanged. Failed waits and incomplete
   timing are not training samples. The worker removes an explicit preparation-
   time GPU wait from the generic learned preparation cost so one stall cannot
   inflate both budgets.
8. Handle preparation failure/cancellation. If the presenter reports
   `sourceFrameReusable`, release the decoder surface before the target wait.
9. Wait for the target, then enforce the controller's currently applicable
   earliest-submission floor with another clock read and wait if necessary.
10. Consume final lifecycle notifications immediately before the native operation.
11. Call `presentAdaptive()`, capture result and timing, and consume any deferred
   completion result before recording submission. A deferred completion can
   train future readiness lead and bound current serial service; it cannot alter
   the target already issued for this frame.
12. Trace the outcome and retain/defer frame ownership as required by the presenter.
   Backend source retirement may continue after this worker step.

With `MOONLIGHT_VRR_OFFSCREEN_PREPARATION=1` on Linux VAAPI or PyroWave/Mailbox,
after the first ordinary frame establishes the real
swapchain format, admitted frames also receive cancellable preparation tickets.
A separate thread performs decode synchronization, source import, rendering
to an offscreen texture, and output-completion polling. It owns its own
libplacebo renderer and mapping textures; it never acquires a swapchain image.
The pacing thread waits for the ticket before scheduling, then acquires the
swapchain, copies the completed output, verifies that short copy has completed,
and applies the ordinary target wait. Only after the copy does preparation of
the next image proceed, preventing its GPU work from delaying that copy.

The 2026-09-21 freeze correction is based on `abd6b82d`. A completed
preparation ticket bypasses the two pre-render stale-replacement checks:
rendering has already finished, and a newer queued source is not evidence of
a ready replacement. Queue capacity and expiry still shed waiting work, and
shutdown, suspension and output-epoch checks still cancel active work. This
prevents a render/drop loop under sustained GPU load without altering source
timestamps or hiding preparation latency. The completed live capture
`20260921-002333-70409` reproduced exactly and contained only two presentations
in 45.8 seconds, with 2,528 completed staged images rejected as stale. This was
presentation starvation rather than a mutex deadlock. A worker regression
exercises repeated 40 ms preparation with newer queued images in all three
latency modes. It fails before the correction and passes afterward; all eleven
VRR/backend/profile/GPU trace suites, replay help, native startup and the latest
capture's exact replay check pass. Evidence is in `build/freeze-fix-validation/`.
Actual GPU throughput and visible recovery still need a live retest.

The subsequent `20260921-004719-85692` session log reported 54.17 and 31.90
rendered FPS for its two 3840x2160 streams, despite 105.36 and 107.74 incoming
FPS. Offscreen preparation introduces an extra target copy and CPU-observed
completion waits before allowing the next preparation job. It is now opt-in;
the default once again renders directly into the swapchain and retains the
mapped source until GPU reads retire, using the presentation semaphore for
completion. This removes the new staging cost; recovered 4K throughput still
requires live verification. The completed-ticket starvation correction remains
in place for experimental use.

Tickets refer to the existing three waiting admissions plus the active image;
they do not add another playout queue. Eviction and lifecycle discard cancel
the corresponding ticket. The preparation queue is independently bounded to
three pending jobs and one running job. Completed textures are reused. A
cancelled job that already submitted GPU work retains its source mapping until
the output completes; a timeout/error drains outstanding GPU work before
unmapping. Shutdown interrupts ticket waits and joins preparation before
renderer destruction. An output-size, representation, or colorspace mismatch
rerenders the same source into the current swapchain instead of displaying an
image encoded for the old output. Unsupported formats retain the existing path.

The main trace records `prepared_ahead` and `stage_*` fields in schema 6. Stage
decode, render-command, and output-ready boundaries precede the scheduling
decision and may precede pacing dequeue. The recorded pacing-thread decode wait
remains zero for these frames. Aggregate statistics include actual stage decode
wait and rendering service rather than counting them as queue residence.
The GPU sidecar adds `stage_render` and `stage_output_ready` spans. The latter
is a CPU completion observation, not a GPU execution timestamp or scanout proof.
Replay validates stage ordering independently of direct-worker readiness.
For direct-worker captures, serial-service revision 2 identifies post-wait
clock semantics independently of the source-mapping selection, fixing the
false exact-baseline rejection after Gemini restored decode-anchored mapping.
Counterfactual replay retains recorded stage readiness; it cannot predict
how changing GPU scheduling changes stage throughput or compositor service.

Preparation-stage validation (2026-09-21): the native incremental release and
offscreen startup help pass, as do eleven deterministic VRR/backend/profile/GPU
trace suites. Nine exact replay checks pass, including prepared-frame shutdown
while awaiting completion and the latest pre-change live capture
`20260920-231030-28049`. The signed playout-offset parser and direct readiness
clock audit were corrected without bypassing exactness checks. Evidence is in
`build/prepared-stage-validation/`. These tests do not exercise the complete
new GPU path in gameplay or establish lower physical judder; a matched live
capture is still required. No Windows build or ChaseShare deployment was made.

For performance reporting, the worker keeps the decoder-output timestamp
unchanged through this sequence. On a successful presentation it records the
full interval through presentation return and the sum of measured preparation
and presentation-call durations. Telemetry subtracts the explicit decode wait
before classifying the remaining time as queue/pacing. Failed and cancelled presentations retain outcome diagnostics but
do not enter any of these paired duration totals or their frame denominator.

The spacing floor is policy-dependent. In production, a frame classified as
latched can have the software floor disabled. Therefore “every submission is
at least one display period plus guard apart” is not a universal invariant.
The worker enforces the floor the controller returns. See the native latch
contract and historical-capture caveat in section 10 before inferring hardware
protection from this choice.

### 7.3 Waiting and scheduler accounting

`VrrTargetWaiter` uses the same monotonic clock as the controller. It sleeps
coarsely until a bounded active region, then polls using
`SDL_CPUPauseInstruction()` near the deadline. Unlike the previous
`std::this_thread::yield()`, this CPU hint does not voluntarily surrender the
OS timeslice just before submission. The base active region is 500 microseconds;
learned target wake lead adds at most another 500 microseconds, for a maximum
1 ms region per wait. Clock-stall escape and absolute-deadline checks remain.
Historical trace `active_yield_count` fields now count active polling steps;
the hook name remains `yield` for deterministic tests. Windows prefers a high-resolution waitable timer
with a sleep fallback. Render and target wake-delay observations feed later
decisions, with separate limits.

`VideoThreadPriority` runs once at entry on the dedicated decoder, renderer,
V-sync, VRR pacer and optional Vulkan preparation threads. Windows first
registers each with MMCSS `Playback`, at relative HIGH for deadline threads
and NORMAL for decode/render/preparation. Registration is released on the same
thread at exit; the system AVRT DLL is loaded dynamically. If unavailable,
SDL provides the platform fallback. On other platforms SDL is the primary
API: deadline threads retain the TIME_CRITICAL request but now retry HIGH on
failure, while other dedicated video threads request HIGH. Each accepted or
rejected request is logged once. These requests need no GPU-specific CPU API;
OS permissions and scheduler policy still control effectiveness. Main-thread
renderers, codec-internal workers, network, audio and the compositor retain
their existing scheduling. No process priority, affinity or system settings
are changed. Priority and polling improvements require a fresh live comparison
of submit jerk and matched display feedback; replay cannot predict scheduling
or compositor changes. The bounded active wait consumes more CPU than yielding
when another runnable thread could otherwise use that time.

A timer returning is not permission to submit early. The worker rereads time
and loops until the applicable floor has actually been reached. Conversely,
an OS deschedule can make it late despite a correct target. Trace planned
deadlines, actual wakeups, and native call boundaries separately.

### 7.4 Suspend, restore, cancellation, and shutdown

Minimize/suspend immediately clears queued work and wakes the worker. An
in-flight prepared image can be cancelled; the worker then blocks until restore
or stop. Display/window epoch changes invalidate current assumptions, reconcile
presenter state, and cause controller rebase. Session-level display changes can
recreate the renderer or disable VRR when refresh becomes different/unreadable.

Cancellation is backend-specific: some presenters may need a native submission
to release an acquired image. The worker accounts for that feedback and any
required spacing instead of assuming cancellation has no timing effect.
D3D11 cancellation unbinds its render target and does not use Present to cancel.

Shutdown sets stop state, wakes and joins the worker, discards remaining queued
frames, closes the trace, and conditionally saves calibration. The final
presenter cancellation releases retained native state. Avoid destroying a
decoder surface or native image while a GPU operation can still reference it.

## 8. Controller: source timeline and target construction

`vrrProductionSessionConfig()` resolves session settings before both the pacer
and worker use them. `vrrTimingParametersForSession()` turns the effective
settings and presenter capabilities into the controller policy. Follow the
actual session mode, smoothing preference, native scheduling and tearing
permission; default-constructed test parameters are not the production policy.

The source timeline tracks RTP frame identity and cadence, including rate
changes and discontinuities. It maps source time into the client monotonic
clock and builds the intended target, preparation deadline and presentation
floor. Requested FPS and display maximum are not evidence of a constant source
rate. The normalized producer mapping uses prompt GPU completion observations;
a delayed collection can include queue residence and is not treated as GPU
service. Real readiness still constrains the submitted frame.

Worker-paced Reduce judder uses bounded phase-locked smoothing. Native
future-target Reduce judder uses the tolerance-bounded source-gap path.
Absorb uses its fixed 2T pad and its own bounded smoothing; worker smoothing
owns its movement while native scheduled presentation retains its gate.
Smoothing is independently controlled by the session preference. The
controller also uses bounded phase-trajectory catch-up, readiness and display
floors, and presenter-specific latch protection. Applied input buffering,
planned extra delay, queue capacity and retained phase are distinct quantities.

## 9. Adaptive input buffer and calibration

`InputGoalBuffer` observes eligible input readiness against the unbuffered
source slot. The goal solver selects the smallest buffer meeting the selected
severity-weighted client timing goal over its configured window, aiming at a
fraction of the allowed loss. If the goal is unreachable within the cap, it
selects the best attainable score. Rise and release are rate limited; the
controller maintains rate-specific evidence layers and credits available VRR
headroom according to the profile. Eligible outcomes, actual input coverage,
interval quality, network and decoder stage attribution are separate metrics.
Packet loss, discontinuities and invalid observations have explicit eligibility
rules; downstream preparation or submission delay is not input lateness.

The existing `normalized_source_intrinsic_decode_gpu=1` path admits a GPU
service sample only when a blocking completion check began within 1 ms of
decoder output. A late check can include our own queue residence even when
the final wait blocks. Source mapping and input accounting share one median
of those prompt samples. Late observations use output plus that median,
bounded by observed completion; without a qualified sample they use output.
This is an accounting estimate: the worker still waits for actual readiness
before presentation. Promptly observed GPU stalls retain their full duration
for input feedback instead of being clipped to the median. No second policy
or additional user setting is introduced.

`profile.cpp` stores calibration under the cache path. The worker constructs
the key from device/session context, effective controller policy and presenter
capabilities before loading. The new key deliberately cold-starts rather than
borrowing an old policy's cached evidence. Cached history is not proof of live
coverage or physical presentation, and phase rebases and complete resets have
different lifecycle effects.
The `prompt-decode-accounting` cache marker additionally isolates both saved
startup delay and reserve history from the former queue-contaminated estimator.
Existing cache files are preserved, but their prior keys are not loaded.

## 10. Windows D3D11 mechanics and native synchronization

### 10.1 Eligibility and GPU synchronization

The renderer prefers the adapter owning the display, creates a flip-discard
HWND swapchain with five buffers, and chooses appropriate RGBA/10-bit format.
It checks tearing support and uses `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` when
supported. Adaptive eligibility checks effective V-sync, borderless fullscreen,
flip-model state, refresh, renderer threading, readiness fencing, tearing
capability/flags, and render/output adapter compatibility. Actual swapchain and
fullscreen descriptors are rechecked through display transitions.

The source deliberately avoids assuming `SetMaximumFrameLatency(1)` is a free
latency improvement: with interval-zero presentation it can make Present block
in a V-sync-like way. Swapchain buffer count is not the worker queue capacity.

With separate decode/render devices, `captureDecodeBoundary()` signals a shared
decode-to-render fence when a decoder output is handed off. Rendering waits for
that exact value, so it need not wait for newer decode work. Render-to-decode
ordering protects texture reuse. The purpose is both correctness and avoiding
accidental waits for work belonging to subsequent frames.
`renderVideo()` queues `ID3D11DeviceContext4::Wait` for the captured boundary
before any copy or shader read; that GPU dependency is the correctness mechanism.
With monitored fences, `observeDecodeReadiness()` independently reports CPU
wait duration and completion evidence, taking no context lock and using its own
event with the 50 ms / fence-value-verified wait described below. The worker
records its post-observation upper bound as `decodeCompleteUs` even for short
waits and already-ready fences; the former 200 us threshold cannot determine
timestamp correctness. Unknown asynchronous readiness stays unknown in the
input graph. Production source mapping uses the observed completion boundary. Between
`78b99f1c` and 2026-09-22 this check was nonblocking, which left Windows mapped
from decoder output while Linux mapped from completion. Non-monitored fences and
shared devices remain nonblocking; there, a zero CPU decode-wait measurement
does not mean the decode dependency was already complete.

Two mutexes serialize this renderer. FFmpeg's D3D11VA lock (`m_ContextLock`)
guards the decode device's immediate context; FFmpeg holds it for each frame's
entire decode submission. A separate presentation lock guards the render
context, swapchain and prepared VRR frame against window-change callbacks. The
presentation lock includes FFmpeg's lock only when decode and render share one
immediate context. On separate devices, preparation and `Present` never hold
FFmpeg's lock; `renderVideo()` and `captureDecodeBoundary()` take it only
around decode-context `Signal`/`Wait`/`Flush`. Lock order is presentation, then
context.

Preparation binds and clears the backbuffer, renders video and overlays, and sets
colorspace/HDR state. Direct decoder texture binding follows stock Moonlight on
Intel and on separate decode/render devices. AMD/NVIDIA single-device sessions
keep the compatibility copy below 4K; 4K streams bind when the GPU has Feature
Level 11.1+ or D3D11 fences. It holds the presentation lock while manipulating
render state. Immediately after recording the frame, preparation signals and
flushes a present-ready fence, arms its event, and performs one nonblocking value
poll. It publishes the in-flight marker under the presentation lock, then
verifies completion of that exact value during preparation. The lock is released
while waiting and reacquired before checking the prepared frame, display state
and device. Successful preparation caches that exact completion proof, reports
`sourceFrameReusable=true`, and releases the presentation lock before the worker's
cadence hold. The worker may then release the source `AVFrame`: the completed
rendering marker proves that every GPU read from it has retired. The separate-
device render-to-decode fence remains an additional ordering dependency.

At the target boundary `presentAdaptive()` reacquires the presentation lock and
checks the cached proof for the exact prepared marker before calling `Present`.
It does not poll or wait for GPU completion at that boundary. Cancellation or
replacement clears the proof, and completion returning from an interrupted wait
cannot revive a discarded frame. Final presentation still revalidates the display
and device; mutex acquisition and the native Present call may themselves block.
The preparation fence wait has a 50 ms
bound, blocks on the event in 1 ms slices, and checks the fence value between
waits; the value, rather than notification delivery alone, proves readiness. A
stale notification cannot release incomplete work, and a delayed notification
cannot hold already-completed work for 50 ms. Display/resize mutation drains an
outstanding prepared fence before invalidating its backbuffer. The device-removed
sentinel is rejected. A true fence timeout or wait failure disables the adaptive
path and requests recovery; its log includes the target, final completed value,
last event result and device status.
Decode-to-render and render-to-decode Signal/Wait failures likewise abort frame
preparation and request device recovery; an unsynchronized frame is never
submitted as a fallback.
`gpu_ready_wait_result` records the aggregate fence-wait status in Win32 wait
codes; individual slice timeouts are not whole-fence failures. The trace's poll
fields describe the first poll during the completion verification in preparation,
replacing the earlier nonblocking poll after Signal. Poll and final readiness
timestamps retain conservative completion bounds. Historical captures retain
their original preparation or Present-boundary observations unchanged.

When preparation verifies completion successfully, the VRR worker records its
verified wait as `gpu_readiness_applied_us` and the next controller
decision exposes the bounded `gpu_readiness_lead_us`. The estimator uses only completed waits in
the current ten-second window, takes the configured percentile (99% in live
sessions), adds a 500 us margin, and slews toward that demand. It is a render
start opportunity, not a promise of completion: a late fence still follows the
bounded failure/recovery path, and the current source target
is never moved to hide the stall.

Fence completion proves the prepared backbuffer work has finished before Present.
If the recorded poll found the value complete, its end is the completion upper
bound; otherwise the successful preparation wait is the upper bound. A historical
final-boundary poll may overstate service because the fence could have completed
earlier during the cadence hold. Replay accepts either a preparation poll or a final poll and
anchors counterfactual upper-bound mapping according to that poll's placement.
The controller uses the resulting uncertainty conservatively for the serial-
service gate. CPU poll/event brackets are not an exact hardware timestamp or a
measurement of total GPU execution time.

### 10.2 Native Present parameters and telemetry

On the DXGI path, `D3D11VARenderer::presentAdaptive()` creates one
`DxgiPresentParameters` value from the controller's latch request and native
predecessor protection. The same value supplies native
telemetry and `presentPreparedFrame()`, which forwards it to DXGI:

- Latched: `Present(1, 0)`.
- Adaptive: `Present(0, DXGI_PRESENT_ALLOW_TEARING)`.
- Legacy: interval zero with the existing `legacyPresentFlags()` value.

The controller can omit its software spacing floor for a latched decision;
passing interval one to DXGI is therefore part of the renderer contract.
`tst_dxgipresent` exercises the actual shared call boundary using a fake
swapchain, including transitions, parameter reporting, and native result
propagation. Actual display behavior still requires Windows validation.

For a controller-planned adaptive slot, native flip protection performs at most
one `GetFrameStatistics()` query, immediately before the native call. An accepted
predecessor ID must be known and `PresentCount` must equal it; missing identity,
failed/disjoint statistics, pending work and unexpected counters synchronize.
Only a fresh successful blank observation on the same uniquely identified physical
output can admit the adaptive candidate. Active, unavailable, stale, reversed or
uncertain raster evidence also synchronizes. Protection starts with the first
adaptive request, independently of diagnostic/alignment flags. The final query
follows metadata population and optional diagnostic probes, and its full age is
limited to min(250 us, display period / 16). There is no scanline spin or
refresh-age extrapolation, and repeated observation failures never disable safety.
The native override is reported as `flip_protection_latched`; its reference stays
zero because the raster-query time is not a measured predecessor flip timestamp.
This closes the pending-only guard's unknown/active-scanout admission holes; a
fresh blank cannot reserve the phase of a later asynchronous flip.
`MOONLIGHT_VRR_SYNC_FLIPS=1` retains the explicit all-synchronized diagnostic mode.

Historical builds computed and recorded interval one but hardcoded zero in
the native helper. Their `latched_present` and native interval fields describe
intent, not proof that DXGI received interval one. Replay cannot repair that
old instrumentation or turn historical `confirmed_safe_latched` classifications
into independent scanout evidence. Check the executable used for each capture.

`restoreFixedPresentation()` disables VRR and retains the swapchain. The legacy
software-paced caller still explicitly uses interval zero; this correction does
not change its pacing mechanism.

### 10.3 Native evidence limits

Only `S_OK` is treated as a presented result. Failed calls request recovery;
non-display success statuses such as occlusion are cancellation outcomes rather
than proof of monitor delivery.

`GetLastPresentCount()` and `GetFrameStatistics()` can report earlier operations.
`PresentRefreshCount` and `SyncRefreshCount` are different identities;
`SyncQPCTime` timestamps the sync observation and is not automatically the
presentation timestamp of the accompanying present ID.

Production cadence diagnostics require `playout_require_display_events=1`. Presentation feedback
explicitly distinguishes unavailable timestamps, refresh references, and display
events. DXGI `GetFrameStatistics()` is marked as a refresh reference and cannot
teach compositor latency, authorize buffer growth/release, or create a measured
cadence sample, even when its refresh IDs match and its timestamp follows
submission. The Windows-host vrr14/vrr15 captures and the Ally GTA capture
demonstrate why: different present IDs can share an identical raw `SyncQPCTime`,
including frames submitted after that reference. A reference is not the frame's
image-change instant. Current schema-6 captures record
`latch_time_kind` (0 unavailable, 1 refresh reference, 2 display event).

The fallback DXGI statistics provider supplies no verified display events.
The graph separately joins its image IDs and measured refresh-count/QPC anchors
to show `Display refresh (DXGI)`. That observation does not enter verified
display-event cadence, latency learning, or adaptation.
Production uses submission estimates for its client cadence report. Matched
DXGI refresh observations populate the separately labeled graph lane;
independent-flip events from composition populate display-event timing
and present-timing diagnostics.
Readiness prediction independently adapts padding in both directions. Composition-frame statistics also lack a verified frame
display instant, so the same estimator covers periods without independent-flip
events. This lower-confidence timing remains internal telemetry; it does not
claim native display coverage or learn display-service latency from it.
Linux Wayland presentation feedback and Gamescope actual-present timestamps
are explicitly marked as display events and remain eligible for measurement.

`MOONLIGHT_VRR_ALIGN=1` adds observation-only raster probes around Present.
The independent protection guard can record a before-only sample with alignment
off. Such samples require matching protection-query and native Present brackets;
replay accepts that provenance without relaxing the paired alignment contract.
DisplayConfig signal geometry and QPC correlation support phase modeling.
`D3DKMTGetScanLine()` reports raster position around a CPU observation; it does
not establish when a queued flip became visible. Cloned/ambiguous display paths
must not be silently treated as an exact calibration match.

Software timing, tearing permission, and modeled active-scanout exposure do not
confirm an optical tear or its absence. External display measurement is needed
for that claim.

### 10.4 Composition presentation and display timing

Windows VRR sessions use waitable DXGI by default. The composition presenter
is requested only with `MOONLIGHT_VRR_COMPOSITION=1` on supported devices.
The value is captured during renderer initialization,
so a stream reconnect is required. Startup logs identify the actual presenter,
including setup fallback. Hardware support permits independent flip but does
not prove that every live present uses it.

The renderer checks the actual OS version using `RtlGetVersion` (including
revision 194 on build 22000), loads `CreatePresentationFactory` dynamically,
and requires `IsPresentationSupportedWithIndependentFlip()`. OS version alone
is insufficient. The render device uses BGRA support and disables internal
threading optimizations as required by this API; an unsupported device retries
with the original DXGI device flags. Setup failure retains the DXGI path.
System-relative presentation time is QPC scaled to 100 ns with the actual QPC
frequency. It no longer depends on resolving an interrupt-clock export or assumes
that the interrupt-clock epoch equals the presentation clock's epoch.

`D3D11CompositionPresenter` owns five displayable textures, a presentation
manager/surface, and a DirectComposition visual bound to the streaming window.
Initial allocation and each resize explicitly set the surface source rectangle
to the full buffer; omitting this leaves successful submissions with no image.
The same shaders, overlays, colorspace, GPU-ready fence, and pacing deadline
are used. Buffer acquisition checks availability without waiting and returns
`S_FALSE` when all five buffers remain unavailable. Scheduled submission preserves
accepted predecessors and translates the worker's future display deadline into
QPC-derived 100 ns presentation time using a fresh clock bracket. Elapsed
deadlines request immediate presentation, without adding a period to hide
lateness. The legacy immediate overload remains available for probes.
Clock correlation is retried a bounded number of times when preemption makes
the bracket unreliable; persistent uncertainty drops that frame with `S_FALSE`
instead of treating a scheduling interruption as device loss.
`ForceVSyncInterrupt`
requests prompt statistics even with hardware flip queues. Buffer storage
is not a queue-depth target; OS/driver scheduling still needs measurement.
`CancelPresentsFrom(1)` is restricted to resize/reset, window/display epoch,
suspension, and worker shutdown rather than discarding accepted predecessors
before each new present. Retiring-fence, availability, and skipped/canceled
status evidence release the ownership accounting. Queued status alone does
not release ownership or establish display. Trace rows expose cumulative skip,
cancel, and status-overflow counts plus the latest drained status ID and value.

Only independent-flip statistics with the matching surface tag, output adapter,
source ID, and increasing present ID become display events. Their 100 ns system-relative
timestamps are correlated with scaled QPC through a fresh bracket on the worker
clock, with bounded age and uncertainty. Composition statistics do not become
display events. The backend is trace value 3; DXGI flags, query results, and raw
QPC fields remain unset. The explicit `native_synchronized_presentation` flag
keeps every controller decision latched without claiming DXGI flag switching.
The independent `native_scheduled_presentation` flag enforces a display-period
deadline floor and permits early CPU enqueue. `submission_us` records actual
native enqueue; `native_target_us` records an accepted deadline, and
`scheduling_boundary_us` is their maximum for accepted work. CPU holding occurs
after enqueue, outside the renderer lock. Failed enqueue does not reserve a
future anchor. Raw target/clock-bracket fields audit native deadline translation;
skipped/canceled counts distinguish acceptance from display. The raw
`native_displayed_duration_100ns` field is Windows' `GetPresentDuration()` value:
the driver-approved custom refresh duration, not measured image residency or
proof that the image remained visible for that long. A native display event
earlier than the requested target is diagnostic rather than capture corruption;
Windows documents the target as an attempt to display as close as possible.
Session-policy replay resolves
the current composition capabilities, but cannot predict native service timing
under a changed queue model. D3D11 calibration identity includes
`native-scheduled=1` so old scheduling history is not silently reused.
Resize replaces
buffers; display changes recreate the renderer and its output identity.

`compositionprobe --run` is an optional fullscreen Windows hardware diagnostic
for independent-flip coverage and submission-to-display latency. It does not
prove optical VRR, tear freedom, or end-to-end latency. The 2026-10-04 native
ALLYTWO probe confirmed independent-flip coverage with no rejected timestamps
or unavailable buffers. At 116 FPS/120 Hz, steady-state coverage was 464/465,
submission-to-display p50 was 7.057 ms and p99 was 15.968 ms. Its strict
below-one-refresh p99 gate failed (exit 2); native capability does not guarantee
sub-refresh latency or live VRR cadence. The 60 FPS probe also failed that gate.
These are synthetic presenter observations, not gameplay or optical validation.

Windows integration validation for this update: the incremental application
build and diagnostics build pass. All six required VRR suites, DXGI call-boundary
and presentation-clock suites pass. The overlay check passes on retry after a
stats-only asynchronous assertion in the unchanged fixture. The new composition
worker fixture populates 22 matched display intervals for 24 submissions and
passed exact replay under the then-current schema-5 tool; fresh schema-6 captures are required now. The newest
completed capture in the canonical trace roots (`20260926-131302-979`) remains
exploratory: old and new replay both exit 3 with identical fidelity results
(6,622 exact targets and tear classifications, 6,621 exact submissions). This
change has not been validated in a new live gameplay stream.

## 11. Other presentation paths

The shared presenter interface separates support checks, decode-boundary
capture/readiness, preparation, adaptive presentation, cancellation, and feedback.
Its implementations can have different acquisition and cancellation semantics.
Completion feedback may be available during preparation, deferred until the
native-present boundary, or unavailable when the backend instead retains source
ownership asynchronously. The worker treats those cases explicitly.
Do not transfer D3D11 fence or Present assumptions directly to Vulkan.

### macOS Metal presentation

An Auto VideoToolbox renderer request uses `VTMetalRenderer` when either VRR
playback or the matching startup renderer probe is requested. The probe does
not start adaptive presentation; it keeps color-range/HDR negotiation aligned
with the playback renderer. The native display helper qualifies both continuous
Adaptive-Sync ranges and discrete ProMotion ranges on macOS 12 or newer. A
requested checkbox, the 120 Hz maximum, or the existence of a Metal device
alone cannot establish a variable-refresh range. Playback also requires native
Cocoa fullscreen; a windowed or older borderless path falls back to fixed pacing.

Active Metal VRR uses a persistent `CAMetalLayer` with
`displaySyncEnabled=YES`, three drawable slots, and drawable acquisition timeout
enabled. `CAMetalDisplayLink` is bypassed only while the shared VRR worker owns
cadence. A synchronized layer can retain both its displayed drawable and a
submitted successor; the third resource lets early preparation proceed without
waiting for the compositor to retire either. The worker still prepares one
image at a time and submits at its existing target, with no additional playout
queue. Startup logging records drawable dimensions, bounds, count, and
transaction mode alongside the native display range.
Preparation acquires a drawable, encodes the existing video/color
conversion and overlays, commits the render command buffer, then observes its
completion with a 50 ms CPU wait bound. Acquisition is a separately measured
native backpressure cost; that 50 ms bound applies to command completion, not
the entire preparation call. Failed rendering or a timed-out wait requests
renderer recovery rather than submitting an incomplete image.

Each render command retains its source `AVFrame` and any CoreVideo texture-cache
wrappers until its GPU completion callback. Those references remain valid even
if the bounded wait fails and the worker/renderer is torn down. Successful
completion permits the worker to release its source before the target hold;
the prepared drawable remains owned until presentation or cancellation.
Cancellation releases an already-rendered drawable without a native submit.

At the worker's target, `presentAdaptive()` calls
`presentAfterMinimumDuration:` with the screen's native minimum refresh
interval. This constrains the previous drawable's visible duration; it is not
an added sleep or source frame period. Every VRR drawable is synchronized, so
Metal advertises native protected-slot support without switching modes for the
controller's per-frame latch request. On fixed fallback the layer returns to the
session's V-sync setting and three drawable slots, and the existing fixed
presentation/display-link path remains available. An active display change
requires renderer recreation even if both screens have the same maximum Hz,
because the native interval range and drawable epoch can differ.
Readiness calibration identity snapshots the Metal device registry ID, stable
CoreGraphics display UUID, and native minimum/maximum/granularity during main-
thread initialization. The worker therefore does not touch AppKit while using
or saving calibration history.

Metal is native backend value 4. Submission IDs are local increasing serials.
The native method returns void, so result 0 denotes accepted submission, not
proof the compositor showed the drawable. DXGI query results, tearing flags,
adapter snapshots, fence values and raw QPC fields remain unavailable. GPU-ready
fields describe the CPU-observed command-completion bracket, not an exact GPU
execution timestamp.

`addPresentedHandler:` obtains the drawable's OS-reported `presentedTime`.
A nonpositive time supplies no event. A fresh `CACurrentMediaTime()` reference
is sampled inside a `LiGetMicroseconds()` bracket; the shared clock translator
rejects future events, events older than 100 ms, and brackets wider than 500 us.
Callbacks retain only shared observation state, never a renderer pointer or
decoder surface. A later submit can report a previous serial's `DisplayEvent`
with correlation uncertainty. Shared diagnostics match that identity and keep
missing events as gaps; requested latch transitions do not reset Metal's fixed
native presentation mode. This is compositor evidence, not optical confirmation
of panel cadence, tearing, or end-to-end latency.

First external-display investigation (2026-10-04): clean-close capture
`20261004-222950-425-c89ef903-1755-412c-bf82-cf0abaaea792/Moonlight.vrrtrace`
(SHA-256 `415d1cb8f8914cafa4c3b05d4ebc7d784fd0289dc4babe1d3d76312716baee19`)
requested 4K90 PyroWave 4:4:4 10-bit on the LG TV's advertised 40-120 Hz range.
The log reported 90.19 incoming FPS, 71.02 rendered FPS, and 21.05% client pacing
drops. Recorded drawable acquisition averaged 9.01 ms, versus 1.45 ms rendering;
native display intervals were predominantly 16.67 ms. Trace sequence/footer
integrity passes and all controller targets and submissions replay exactly,
but five queue-depth semantic errors reject the strict baseline gate. This is
recorded execution evidence, not strict counterfactual A/B proof.

A host-free 600-frame native smoke at 90 FPS on the same display reproduced
401 submissions and 199 drops with two resources. Three resources produced
600 submissions and zero drops and passed exact replay. The observed display
intervals still alternated around 8.33/16.67 ms; successful submission throughput
must not be described as adaptive display-cadence success. The constant-rate
`check_metal_cadence.py` hardware gate independently checks matched OS display
events and rejects that result. The user confirmed that macOS and the TV are
already in VRR mode; asking to enable VRR again does not resolve this failure.

Follow-up native experiments explicitly requested 90 FPS through a
`CADisplayLink`, and separately passed the controller's fitted 11.111 ms source
period to `presentAfterMinimumDuration:`. Neither produced uniform 90 Hz OS
presentation events. The fitted-period run submitted all 600 frames, matched
99.65% of steady display events, but only 0.35% of consecutive intervals were
within 500 us of the requested period; acquisition rose to 4.63 ms on average.
Both experiments were reverted. A standalone Cocoa/Metal control using
`CAMetalDisplayLink`, a 90 FPS preferred range, and its supplied drawables also
received predominantly 120 Hz callbacks. A display-link preference is a
best-effort request, not proof of adaptive scanout, and its drawables reject
`presentAfterMinimumDuration:`; mixing the two APIs throws a native exception.
These experiments do not establish the root cause or justify a new production
timing policy. The remaining investigation is native presentation eligibility,
including direct versus composited presentation; optical panel validation also
remains outstanding. The resource correction must not be presented as a VRR fix.

Continuation on 2026-10-04 separated environmental throttling from native cadence.
The user observed the TV's numerical refresh rate vary toward 90 Hz in the native
control. The Metal HUD then exposed Low Power Mode, composited presentation, and
about 60 presented FPS during a 90 FPS request; the control's display events were
predominantly 16.67 ms. The user changed AC power mode to Automatic. A fullscreen
settle and a focus check were also necessary: an earlier unsettled smoke dropped
245 of 900 frames and cannot establish steady fullscreen performance.

With Automatic power, fullscreen settled, and no focus loss, the production
presenter submitted all 900 frames with zero drops and passed exact replay.
Recorded display intervals were distributed around 11.11 ms rather than a fixed
120 Hz grid. The strict constant-cadence gate still failed: 44.71% of consecutive
events were within 500 us, with 2.111 ms p95 interval error. Passing the fitted
source period to Metal also submitted all 900 frames but yielded similar cadence
(47.97%, 2.249 ms p95); that experiment was reverted. Neither gate failure alone
proves VRR is inactive, nor does zero-drop throughput prove uniform scanout.
Optical validation and an end-to-end stream remain separate from this smoke.

Initialization now logs Low Power Mode and warns when it is enabled. The advertised
`NSScreen` range remains a capability snapshot, not proof that the current power
policy permits the requested presentation rate. The native smoke logs fullscreen,
activation, focus, and power state, waits for fullscreen to settle before the
worker run, and rejects focus loss during frame delivery.

### Linux Vulkan presentation

Linux package capabilities must be qualified independently of launch/version.
The daily.183 AppImage disabled Moonlight Wayland, lacked matching Qt Wayland
plugins, and built SDL without its Wayland backend; its X11 launch test did not
qualify native feedback or HDR. The corrected packaging candidate enables all
three layers, keeps DRM/EGLFS renderer policy separate, and uses host libwayland
with host Mesa/EGL to avoid an older bundled client library hiding host symbols.
AMDGPU stable-pstate capability detection accepts distribution header layouts
while retaining required ioctl operations and the existing stream-scoped hold.
Package gates inspect actual SDL drivers, Qt plugins, feedback/HDR code and GPU
capability; isolated feedback tests and private headless Wayland startup complement
the existing X11 smoke test. They do not establish hardware HDR surface negotiation,
physical VRR, GPU state changes or calibration quality. Matched Deck runtime
validation remains required; backend/present-mode calibration caches are preserved
and must not be cross-seeded from Immediate into Mailbox.

On Linux the VRR request prefers the Vulkan frontend. The adaptive mode is
selected for the surface at startup: Mailbox on ordinary Wayland, Immediate on
X11/KMSDRM, and Immediate on Gamescope. Gamescope additionally tries Mailbox
when the dormant SteamOS experiment is enabled, according to exposed surface
capabilities.

The startup decoder probe that asks the host for a color range must use that
same Linux Vulkan preference without activating VRR presentation.
`chooseDecoder()` still forbids `enableVrr` in test-only mode; the probe passes
`preferVrrRenderer` from the session VRR snapshot instead. Otherwise an EGL
probe can request limited-range video while VRR playback interprets it as full
range, which washes out SDR. Vulkan's AMF AV1 full-range mapping override
applies only when the negotiated stream range is full. HDR remains gated on the
client's Enable HDR preference; VRR does not advertise 10-bit formats by itself.

For VAAPI hardware frames, the worker performs the explicit `vaSyncSurface()`
readiness check once and records its CPU wait. Preparation no longer repeats that
explicit synchronization; libplacebo's `AV_HWFRAME_MAP_READ` import still
validates/maps the dependency. A failed explicit VA sync disables the adaptive
path and requests renderer recovery before preparation can import or read the
surface. This removes redundant CPU serialization without treating decoder
output as proof of GPU completion.

Hardware Vulkan preparation retains the mapped `pl_frame`, including libplacebo's
AVFrame reference and imported source textures, until their GPU reads finish.
Retained hardware output is submitted asynchronously in every presentation mode:
libplacebo transitions the output image, signals a render-complete semaphore, and supplies it to
`vkQueuePresentKHR`. CPU completion is not required for this handoff. Preparation,
presentation and cancellation retire idle source mappings and preparation reports
`sourceFrameReusable=true` only when no retained mapping remains. This releases
the worker's surface before the target hold without recycling external decoder
memory while Vulkan reads it. Cancellation and failure can leave mappings pending;
they remain owned until idle or healthy-GPU teardown finishes them.

The first asynchronous output-wait bypass was withdrawn after the 2026-09-19 23:54:19
clean capture: 117.67 incoming FPS, 85.29 rendered FPS, 27.52% client drops and
zero network drops. Concurrent Vibeshine capture was reported to cause a hard
stall. Restoring the barrier did not establish the cause. The subsequent trace
proved decode-wait discard starvation, corrected in `78b99f1c`. The 00:30:01
launch then produced 76.11% client timing / 100.23 rendered FPS at 301.5 Mbps,
versus 99.67% / 117.37 FPS at 57 Mbps, both Smooth without concurrent capture.
After 20 seconds from the first arrival, high-bitrate presented frames averaged 9.559 ms of serial
decode-wait + preparation + submission service against an 8.333 ms period.

The updated path pairs asynchronous retained-hardware output and the corrected stale
policy with preparation on arrival. It retains up to four source mappings to
prevent capacity stalls, applies bounded retirement backpressure before acquiring another swapchain image, and
never calls an unavailable output-completion sample a zero-duration completion.
Immediate presentation omits that CPU completion check
when the source mapping is retained. GPU-delayed flips may still bunch despite
correctly spaced CPU submissions. Software and unretained imports keep the 50 ms / 100,000-observation output-poll bound.
D3D11 fence/event fields remain unset on Vulkan rows. Windows explicitly flushes the decoder context
after signaling its cross-device boundary, so dispatch does not depend on a later input frame.

The selected adaptive mode remains immutable for the lifetime of one persistent
swapchain. Per-frame controller requests never destroy or recreate that chain.
Persistent Mailbox provides synchronized, stale-image-replacing presentation,
so it advertises protected latch support without a native mode change or the
controller's redundant software spacing floor. Immediate retains the
display-period-plus-guard floor because it may tear. Explicit historical
revision 2 adds a 225 us entry margin to that floor; current revision 1 does not.
That historical extra spacing can reduce sustainable throughput near native refresh;
worker stale-frame replacement remains responsible for bounded backlog.
It does not turn Immediate into a tear-free native presentation mode.
A FIFO-only compatibility path likewise does not advertise
adaptive latch support because it may accumulate queued frames. Actual resize,
reset, or fallback can recreate the swapchain and restores the cached
colorspace/HDR hint before the next acquisition. Deterministic tests do not
establish compositor or physical scanout behavior.

Gamescope WSI's FIFO compatibility exception is used when Immediate is unavailable
and the Mailbox experiment is disabled or Mailbox is unavailable. Although the WSI layer sends Mailbox to the underlying
driver, it forwards the application's original present mode to Gamescope, which
implements FIFO commit scheduling itself. Selecting Mailbox explicitly avoids
that FIFO policy. Steam's frame
limiter can still override a request to FIFO. Native presentation here remains
compositor-owned, and submission success is not physical scanout feedback.
See [SteamOS VRR investigation](docs/steamos-vrr.md) for source evidence and the
reversible composition test for performance-overlay-dependent stutter.
Unsupported renderer combinations
fall back to fixed pacing. Windows Vulkan remains rejected for VRR, and macOS
VRR uses the native Metal path above rather than Vulkan swapchain mode switching.
DRM VRR properties, compositor policy,
and physical scanout evidence remain separate from the client's mode request.

Gamescope timing rejection counters are cumulative across swapchain resets and
logged at renderer teardown. They distinguish empty queries, returned and emitted
records, unmatched IDs, timestamps before submission or in the future, stale or
invalid timestamps, clock-correlation failures, and skipped warmup. They do not
relax validation or change pacing. Sparse usable feedback alone does not establish
that the compositor dropped the other frames. The installed Gamescope 3.16.23.5
source supplies its scheduled vblank target as `actualPresentTime`; these records
must not be treated as an independent physical scanout measurement.

The worker presents only newly received frames and waits for queue activity
when empty. It no longer retains and re-presents the last image to fill gaps.
Display-side low-frame-rate compensation remains the display's responsibility.
Historical `gap_fills_before` and `gap_fill_last_us` CSV columns remain reserved
and zero-valued to preserve trace compatibility.

#### KWin scanout guard (2026-10-11)

On Plasma Wayland, a fullscreen stream's handoff-to-screen latency
(`latch_time_us - presenter_submission_time_us`) has a floor of about 1.4 ms
with direct scanout and about 6 ms when KWin composites. KWin 6.4.3 never sets
the presentation ZERO_COPY flag, so that floor is the evidence. A TV hotplug
left two KWin states that composite every frame until something resets them:

- A short animation effect (`fadingpopups`) stayed active forever. Its
  timeline only advances when its window is painted, and any active effect that
  does not opt out blocks direct scanout on every output.
- The atomic test that moves the HDR gamma 2.2 to PQ conversion into the CRTC
  `GAMMA_LUT` failed, so KWin fell back to a shadow buffer: an extra 4K GPU pass
  per frame that delayed PyroWave 4K 4:4:4 decode (decode wait p50 3.7 ms
  -> 8 ms, about 20% client pacing drops).

`streaming/kwinscanoutguard.cpp` runs on its own thread while a stream is
active. Every 2 s while fullscreen it reads KWin's `activeEffects` over D-Bus
and reloads a short animation effect active for 6 s, at most three times per
effect. KMS checks identify active HDR connectors with no CRTC `GAMMA_LUT`;
that can indicate hardware-color fallback. Recovery requires two consistent
fullscreen observations at least 2 s apart and consumes a single attempt per
stream, including failures or uncertain replies. Intentional color-accuracy
outputs are left alone. Unknown KMS state and output hotplug reset only the
settling observation, never a consumed retry budget.

Output discovery supports legacy `kde_output_device_v2` globals through
version 16 and `kde_output_device_registry_v2` at version 21. Initial state
must include `done`; removed outputs/modes, duplicate descriptions, stale
state and configuration failures are handled without applying stale values.
Registry-based KWin receives an output-configuration apply with no setting
setters. Legacy KWin receives only the current SDR reference value, after a
freshness check; no brightness nudge or mode/HDR/VRR/calibration setter is sent.
Source-path fixtures for KWin 6.7.3 and 6.4.3 verify that an unchanged reference
creates the change set and reaches `tryKmsColorOffloading()`. The 6.7.3 apply
path adds current DPMS state even to an empty configuration. These fixtures
verify the retry entry, not GPU acceptance. A live KWin 6.7.3 test restored a
nonzero CRTC gamma LUT at the unchanged 300-nit reference; the user confirmed
correct color and no flicker. The remaining present-timing issue was not
resolved. Hardware gamma offload alone does not establish direct scanout or
correct display cadence. An accepted configuration is logged separately from
the later KMS result.

The guard links neither libwayland-client nor libdrm (the AppImage disables
both). It loads libwayland-client at runtime. A single 2 s deadline covers the
whole short-lived output-management operation, including enumeration,
freshness and reply waits. The Flatpak needs `--talk-name=org.kde.KWin` for the
effect check. The KMS check and HDR retry use the Wayland socket and `/dev/dri`.
`MOONLIGHT_KWIN_SCANOUT_GUARD=0` disables the guard. The isolated
`tests/vrr/tst_kwinscanoutguard` fixture checks registry/legacy requests,
unchanged settings, hotplug, timeouts and the one-attempt policy under sanitizers.

## 12. Audio, input, and end-to-end latency

Audio has its own UDP/RTP queue, Opus decoding callback, and renderer/device
queue. See [AudioStream.c](moonlight-common-c/moonlight-common-c/src/AudioStream.c),
[audio.cpp](app/streaming/audio/audio.cpp), and
[sdlaud.cpp](app/streaming/audio/renderers/sdlaud.cpp).
Audio packet duration controls its sample cadence, independent of video FPS.

The SDL renderer (updated 2026-09-26) is callback driven. `audio.cpp` hands it
compressed Opus packets, including zero-length placeholders for packets lost on
the network, and the SDL device callback decodes them through
`Session::arDecodeForRenderer` as it consumes audio, so the decoder is used on
one thread only. The device period is at least 480 samples (10 ms). The packet
queue is a jitter buffer whose target comes from the `audiobuffer` preference
(0 = automatic, starting at 20 ms, growing 5 ms per concealed late packet and
10 ms per underrun up to 80 ms, and shrinking 5 ms after 60 s without trouble;
fixed values are 10/20/30/50/80 ms in the UI). Playback waits until the queue
holds the target plus one period. When the queue is empty the callback asks
Opus for up to 30 ms of packet loss concealment. Beyond that it fades out over
2 ms and waits for the target again before fading back in. When more than twice
the target plus one packet is buffered, it decodes and drops packets back down
to the target, cross-fading each drop over 2 ms. Gaps over 500 ms (muting,
reconnects) are not counted as underruns. Counts are logged about once a second
when they change, and as totals when the renderer closes. There is no clock
drift resampling; drift appears as occasional concealment or cross-faded drops.
The `audiodriver` preference picks SDL's audio backend. `createAudioRenderer`
applies it as the `SDL_AUDIODRIVER` hint, so a user's environment variable still
wins, and falls back to SDL's default backend if the chosen one fails to open.
The settings list comes from `SystemProperties::getAudioDrivers()`, which opens
each backend compiled into SDL once. It skips `disk` and `dummy`, lists the
default first, and returns nothing when the environment already sets a backend.
The renderer also records into the session-owned `AudioStats`
(`audio/audiostats.h`) on each device request: the time since the last request,
the amount requested, what stayed buffered, underruns, concealment episodes and
the target. The stats-graph sampler takes it every 100 ms for the opt-in Audio
graphs (buffer against target, underruns and filled gaps, device request
interval against request size).
Video timestamps are not used. Audio startup intentionally discards an initial backlog of about
500 ms; renderer reinitialization similarly prevents downtime becoming permanent
queued audio latency. Muting can suppress audio processing without retiming VRR.

Input goes from SDL handlers to common-library input APIs and a separate sender.

Windows/Linux DualSense waveform feedback (updated 2026-10-05) runs separately
from video and ordinary stream audio. A Bluetooth Sony DualSense/Edge with an
exact SDL HID device path can advertise controller capability
`LI_CCAP_HAPTICS_PCM` (`0x8000`) after its output backend opens successfully.
The client also advertises `ML_FF_HAPTICS_PCM` (`0x04`) in SDP. Vibeshine captures
48 kHz S16LE actuator channels 3/4 from its virtual USB audio interface and sends
5 ms stereo blocks using encrypted control type `0x5601`, unreliable ENet
channel `0x08`. Both flags and the versioned payload are a coordinated extension
in these forks; they are not an upstream Moonlight protocol guarantee.

The session registers the waveform receive callback on both Windows and Linux
with SDL 2.24 or newer. The common library advertises the SDP flag only when
that callback is present; enabling the controller backend alone cannot enable
waveform delivery. A Linux-only session callback guard previously left Windows
controllers advertising `LI_CCAP_HAPTICS_PCM` while the connection omitted
`ML_FF_HAPTICS_PCM`, so the host discarded their PCM feedback.

The receive callback validates exact length, version, format, controller range,
reserved fields and sample count before copying into a bounded per-controller
queue. It never accesses Session/InputHandler objects that may be tearing down.
The shared worker resamples to 3 kHz signed 8-bit stereo with SDL's audio stream,
then sends SAxense-derived Bluetooth reports. Linux uses the controller's hidraw
node and verifies its kernel bus identity. Windows opens SDL's exact device
path with shared access, verifies Sony VID/PID and the Bluetooth gamepad HID
collection's report lengths, and writes through overlapped Windows HID I/O.
Output is padded to the descriptor's maximum report length while preserving the
142-byte waveform report's CRC offset. A pending write has a 40 ms completion
wait; timeout cancels and drains the operation before its buffer can be reused
or freed. This is not a hard bound on a faulty driver's cancellation completion.
Input stays with SDL; no kernel module or Bluetooth reconfiguration is added on
the client. Windows USB additionally matches the exact SDL Sony USB controller's
HID device container to its active four-channel WASAPI render endpoint. The
worker submits 48 kHz float samples to actuator channels 3/4 while keeping
channels 1/2 silent; endpoints whose mix format is not four channels are rejected.
Shared-mode conversion uses the endpoint's channel mask. Startup buffers 10 ms
(or waits at most 10 ms); the software FIFO is capped at 40 ms, separately from
the requested 20 ms engine buffer. The actual engine buffer size is queried.
Idle output drains to silence, and disconnect joins the worker before SDL closes
the controller. Linux USB and other platforms keep ordinary rumble. Native game
haptics must originate from the host's controller audio endpoint. Linux uses
its composite DS5; Windows uses the optional local usbip-win2 composite DS5,
selected per application with `dualsense-haptics` or globally with `usbip_ds5`.
The wired host identity is independent of the physical client's USB/Bluetooth
connection. USB/IP stays on the Windows host; no USB/IP transport is required
on the Moonlight client. Game soundtrack audio is not a substitute.

Bluetooth playback drops old/duplicate packets, resets conversion history on
packet loss,
bounds its input and converted queues, sends silence on underflow/idle/removal,
and joins its worker before SDL closes the controller. It cancels SDL emulated
rumble when waveform playback starts and suppresses legacy rumble while active;
LED, motion and adaptive-trigger callbacks retain their own paths. Shared
receive admission rejects late/duplicate sequences, including wrap, before
changing rumble priority; the sequence remains tracked across idle silence.
A silent PCM stream cannot start playback. Both workers idle 60 ms after the
last nonzero actuator sample, send/drain final silence, and stay inactive while
an open host audio endpoint continues to submit zeros. This prevents silent
audio from indefinitely suppressing rumble or overwriting it with silent
Bluetooth reports. Windows HID transport includes the SDK before SDL so
`CancelIoEx` remains declared when compiled separately. Write failure
stops that waveform worker and logs the need to reconnect; ordinary controller
input continues. Hardware coexistence with other applications writing the same
controller still requires physical testing. The USB backend has not been validated
on Windows hardware; Linux deterministic tests cover the shared input queue and
existing Bluetooth worker, not WASAPI endpoint discovery or physical output.

Adaptive triggers already use `SDL_GameControllerSendEffect` on Windows and
Linux, independently of PCM support. SDL owns Bluetooth framing/CRC. The shared
47-byte effect builder sets only trigger validity bits, preserving waveform,
rumble, LED and audio state. Callback admission and event consumption reject
controller indices outside 0–15; allocation and event-push failure release the
report safely. Failed SDL effect submission is logged. Trigger dispatch runs on
the input thread so controller removal cannot race its SDL handle.

The pinned SAxense source, license, research credit and adaptation notes are in
[`third-party/saxense`](third-party/saxense/PROVENANCE.md). Every binary embeds the
original/adapted covered source and both license texts, printable headlessly with
`--haptics-license`. Hardware-free validation is `tests/haptics/haptics.pro`;
passing it does not establish actual Bluetooth/game behavior. The suite runs on
Windows, Linux and macOS using a recording output and SDL's real resampler; a
virtual controller checks trigger payload dispatch. Windows CI builds the native
HID transport and runs the suite. Local 2026-09-19 validation passed on macOS
using isolated headers from the pinned common-library revision; Windows native
build, simultaneous physical waveform/trigger playback and deployment remain
unverified. The pre-existing local common-library checkout was left unchanged.

See [InputStream.c](moonlight-common-c/moonlight-common-c/src/InputStream.c) and
[input handlers](app/streaming/input). Mouse movement is coalesced/batched with
a 1 ms interval; the stream event loop normally sleeps 1 ms when idle, with
platform differences. Input does not wait for the next video target to be sent.

The inspected paths show no client mechanism that makes audio playout follow
VRR targets or makes VRR follow the audio device clock. Added video protection
therefore must not be assumed to produce a corresponding audio delay.

An end-to-end interaction contains client input collection/sending, host
simulation, host capture/encode, network transit, client receive/decode,
presentation, and physical scanout. Client statistics expose only portions.
Host-processing reports, local decoder/pacer timings, and RTT cannot be summed
into a precise physical latency measurement without defining non-overlapping
boundaries and obtaining the missing evidence.

## 13. Trace architecture and replay fidelity

`MOONLIGHT_VRR_TRACE` enables a local CSV or compressed `.vrrtrace` capture;
`MOONLIGHT_VRR_DEEP_TRACE` adds diagnostic detail. The bounded producer queue
and background writer keep formatting, compression and I/O off frame delivery.
When the queue fills, diagnostic rows are dropped instead of blocking the pacer.
Capture retains at least an hour before applying the 512 MiB cap. UNC paths
are rejected so network writes do not enter the frame path. The file handling
lives in `TraceFile` (pacer/tracefile.h) and is shared with timestamp pacing,
whose own `.tstrace` rows are described under "Timestamp pacing" in section 16. Settings recording
and the launchers manage capture lifecycle and export; see
[VRR diagnostics](docs/vrr-diagnostics.md).

The worker emits schema 6 and freezes its resolved controller parameters and
startup seed for each trace. Rows include frame identity, source timestamps,
queue lifecycle, controller decisions, preparation and submission times,
presenter feedback, and optional GPU/raster evidence. The writer footer records
sequence, integrity, write-failure and cap state. Older compressed traces remain
decodable by `scripts/decode-vrr-trace.py`, but replay rejects older schemas and
retired controller policies. Schema-2 scenario configs derive the current
session policy rather than selecting historical revision families.

An exact baseline requires a successful replay process, complete recorded
sequence, `capture.recorded_sequence_integrity_valid`, and
`fidelity.baseline_exact`. A JSON file alone is insufficient. Exact replay
checks deterministic reproduction in the recorded model; it is not an optical
measurement and cannot establish the real effects of a changed policy on the
host, network, GPU, compositor or panel. See [VRR tests](tests/vrr/README.md)
and `AGENTS.md` for capture selection, baseline and scenario workflows.

## 14. Metrics and investigation

Follow matching frame identities and source/RTP timestamps across boundaries.
Separate host source cadence, packet ingress, decode output and GPU completion,
worker queue residence, preparation, CPU submission, OS-reported display
feedback and physical scanout. A long interval alone is not a stall: an idle or
lower-rate source can produce it legitimately. CPU enqueue spacing may vary
while a native presentation queue maintains display cadence. OS reports require
matching identities and do not establish optical output.

Compare presented-interval jerk and sender-spacing fidelity together with
latency, drift, interval violations and modeled raster bounds. Input coverage is
a binary measurement and differs from the severity-weighted client timing
score; neither by itself proves visible smoothness. Fixed-admission replay can
become saturated where the live worker would shed frames, invalidating its
latency result. Use matched live A/B evidence for visible behavior.

## 15. Tests, deployment boundaries, and maintenance

For opt-in Linux bandwidth testing, [moonlight-link-test.py](scripts/moonlight-link-test.py)
redirects incoming IPv4/IPv6 traffic on a selected interface through an IFB with a bounded TBF
queue (default 1000 Mbps, 2 ms of queue service). It exercises packet delivery
and reassembly before decode; existing frame-level VRR replay cannot model this
bottleneck. It is an approximate software link, not physical gigabit timing,
and does not alter production policy. An optional host filter limits only that
host's incoming UDP. See [network-link-testing.md](docs/network-link-testing.md)
for activation, counters, limitations and removal. Isolated Linux namespace
checks cover IPv4/IPv6 setup/removal, real IPv4 UDP redirection and overflow,
host isolation, existing-rule refusal and failed-setup rollback; live gameplay
and sustained gigabit throughput remain separate validation.

The deterministic suites are
[tst_vrrtimingcontroller.cpp](tests/vrr/tst_vrrtimingcontroller.cpp),
[tst_vrrratepolicy.cpp](tests/vrr/tst_vrrratepolicy.cpp),
[tst_vrrpacingworker.cpp](tests/vrr/tst_vrrpacingworker.cpp),
[tst_vrrreplayconfig.cpp](tests/vrr/tst_vrrreplayconfig.cpp),
[tst_vrrrenderpolicy.cpp](tests/vrr/tst_vrrrenderpolicy.cpp), and
[tst_d3d11bindpolicy.cpp](tests/vrr/tst_d3d11bindpolicy.cpp).
The last covers the 4K decoder-bind vs compatibility-copy rule.
They cover timing arithmetic, timestamp wrap/rebase, cadence changes, delay and
history behavior, queue/drop/cancellation/suspension, ownership, wait floors,
trace integrity, native diagnostic modeling, and replay configuration/contracts.
Consult the test names and assertions for the specific behavior being changed;
the existence of a broad suite is not proof that a native API argument is tested.
`tst_vrrdiagnostics` additionally exercises capture scope, environment restoration,
export locking, file preservation and ZIP bounds; the independent Python ZIP
check verifies contents and CRCs. Windows CI includes that test and exact replay
of cold and warm-history worker fixtures under the unchanged production policy.

The ordinary application build does not build the opt-in replay/tests.
Follow AGENTS.md to build diagnostics separately and run all four suites plus
`vrrreplay --help` when required. Missing runtime DLLs are environment failures,
not pacing failures. The macOS native presenter smoke test exercises the actual
Metal/VideoToolbox renderer separately from the hardware-free worker fixture.
The display-range test checks range validation independently of a particular
attached display. See [Mac build and use](docs/macos-vrr-pyrowave.md) for build
commands and validation outcomes. No deterministic test here establishes optical
tearing, full host behavior, or physical A/V synchronization.

Mac validation (2026-10-04, Apple M5 Pro): 17 deterministic VRR suites and
seven CPU PyroWave/haptics suites pass. Native shared-plane regression passes
12 cases through 4K and 3024x1964 with exact Metal/Vulkan byte agreement,
exhaustion, poison/reuse and retained-lifetime checks. The native calibration
smoke passes 288 draws across eight format/resolution combinations through
1080p into a 1440p target, including grayscale/color/alpha and matrix checks.
An actual encode/framing/asynchronous shared-decode-to-Metal smoke presents
128x96 4:2:0 8-bit and 4:4:4 10-bit patterns adaptively, including cancellation,
source recycling and decoder teardown. A 4:4:4 10-bit shared frame also passes
the fixed/display-link path.
Its native worker fixture submits 100 synthetic frames at 116 FPS with no worker
drops and 97 matched native display intervals. The then-current schema-5 trace
passed exact replay, as did success/failed-preparation Metal feedback fixtures;
11 corrupted-evidence checks remain rejected. These validate native ownership,
rendering and diagnostic contracts without a live host, network or game.
External-display behavior, sustained live throughput and physical display/A/V
results remain unmeasured. Runtime evidence here is arm64; the PyroWave static
library also cross-compiles for x86_64, without an Intel runtime test.

For the current Windows setup, ALLYTWO is this client and also the SMB server;
the Sunshine host is another LAN machine. The private live portable installation
is `\\allytwo\ChaseShare\MoonlightPortable-x64-6.1.0-vrr-lite`.
Keep its stable name. `AllyShare` is an open host-log drop and must not receive
release builds or profile/settings data.

An incremental local link is not publication. A requested ChaseShare update
also needs staging, changed diagnostics, refreshed ZIP, process-safety checks,
complete copy, source/live hashes, and the UNC replay smoke test. Preserve
`portable.dat.inactive`, dependencies, and tools; do not overwrite a running
gaming installation. Follow the complete commands in AGENTS.md rather than
reconstructing them from this architectural summary.

When maintaining this document:

- Update the baseline and affected explanations when active behavior changes.
- Follow resolver values through effective formulas, not only declarations.
- Follow intended native parameters through the actual API call and telemetry.
- Keep live policy, fallback behavior, historical notes, and optional
  experiments distinct.
- Preserve clock units, frame identity, queue ownership, and lifecycle order in
  every new diagnostic or model.
- Revisit exact replay whenever schema, feedback, policy state, or execution
  boundaries change; update both share launchers when their contract changes.
- Keep native Present arguments and telemetry aligned; validate the native call
  boundary and real Windows behavior before interpreting optical results.

The durable debugging approach is to trace an observed frame through the full
chain, identify the first boundary that differs from its intended behavior,
and check how that difference propagates into subsequent frames and feedback.
That keeps host stalls, local overload, scheduling policy, native behavior,
and measurement limitations from being conflated.

## 16. Fork additions

Features this fork adds on top of upstream, kept together so upstream's rewrites of
the sections above do not have to be reconciled with them.

### Rendering, scaling and dithering

Downscaling filters (2026-10-04, source `d8fa2d3d` plus this change): the saved
`downscalingfilter` preference offers Bilinear (default), Bicubic (B-spline),
Mitchell, and Lanczos (three lobes). The presentation snapshot carries it into
both probing and playback/reset. Bilinear retains the previous direct sampling.
On eligible Unix frontends, another choice prefers libplacebo Vulkan, including
the probe so negotiated color range matches playback. macOS Auto already prefers
libplacebo/MoltenVK when available. Unsupported fallback renderers log that they
are using their own default scaling; the setting is disabled for explicit native
Metal or AVSampleBufferDisplayLayer selection.

`PlVkRenderer` assigns only the main downscaler, keeps bilinear plane sampling,
and retains filter widening and full-precision intermediates. Direct rendering
and optional offscreen preparation use the same parameters. D3D11's
`D3D11Downscaler` shares source/quad resources with the optional upscalers but
activates only for reduction. Color conversion/debanding first writes the cropped
stream into RGBA16F without output dithering. CPU-generated normalized weight
textures carry exact source indices and combine clamped border samples. Horizontal
filtering linearizes SDR/PQ and writes RGBA32F, retaining dark HDR and signed
filter lobes; vertical filtering restores the transfer function and performs final
dithering/grain inside the letterbox rectangle. An unchanged axis is copied.
The full viewport is restored before overlays. Filter/source resources are
reconfigured on frame-format changes or window resize; allocation failure falls
back to the existing bilinear draw. FSR1/LS1 remain enlargement-only and can be
enabled alongside the downscaling preference.

Validation: shaders compile with FXC; the incremental Windows release builds.
`tests/rendering/tst_d3d11downscaling.cpp` runs the production scaler through WARP:
132 cases cover CPU-reference comparisons, libplacebo Vulkan comparisons, linear/
gamma/sRGB/PQ input, output dithering, borders, one-axis resizing, extreme reduction,
native/upscaled bypass, and constant dark PQ at 2650x1600 -> 1280x773. The largest
D3D11/libplacebo difference in the tested SDR patterns is 0.001125 in normalized
RGB. The local D3D11 debug layer is unavailable. This is offscreen software/GPU
validation, not live-stream quality, timing, power, or Deck/macOS validation.
The linked executable was staged into `build/deploy-x64-release-local` and its
SHA-256 matches the build output. Application `--help` startup checks stalled
before producing output, so application startup is not verified here. The
Chase-specific build helper/toolchain and ChaseShare are unavailable in this
environment; no ChaseShare publication is established.

Original Moonlight `master` merge (2026-09-25, `032529d7`): D3D11VA now
logs the adapter driver version and uses upstream's vendor and driver checks
when choosing separate decode and render devices. This fork's texture bind
policy remains active after that choice. The common library includes upstream's
frame-loss fixes alongside the fork's PyroWave frame handling; speculative
loss reports require reference-frame invalidation and exclude PyroWave frames.
These source changes still need a build and live validation on supported GPUs.
The merge also made full range the default requested color range (upstream
c1623ff4). FFmpeg reports full-range 8-bit HEVC as `yuvj420p`. The FFmpeg 8
build in the older `setup-deps` packages leaves that unmapped in its D3D11VA
frame parameters (`sw_format` NONE), so every 8-bit D3D11VA stream fails with
"Failed initialize hwframes context: -22". Upstream fixed this with a
dependency update (2e13ed99). After merging, rerun `setup-deps.ps1` (currently
tag v17, FFmpeg 9.0.2) instead of reusing an older `libs\windows`.

Windows renderer, dithering, and debanding (2026-09-28, not yet validated on a
live stream):
- **Forced Vulkan:** the "Force Vulkan renderer" checkbox stores `RS_VULKAN` in
  the existing `rendererSelection` preference on Windows and Linux. On Windows,
  pass 0 of `createHwAccelRenderer()` skips D3D11VA so the Vulkan hwaccel
  config reaches `PlVkRenderer` first; D3D11VA/DXVA2 still follow in pass 1.
  The forced selection also overrides `PlVkRenderer`'s Intel Vulkan Video
  rejection. On Linux it joins the existing Vulkan-frontend preference.
- **Dithering:** applies to 8-bit, 10-bit SDR and PQ streams in both
  renderers. D3D11 quantizes SDR to min(swapchain, display) bits and PQ to the
  10-bit swapchain. Its shaders read thresholds from a t3 texture: libplacebo's
  64x64 blue noise (`pl_generate_blue_noise`) for every non-ordered mode, or
  an 8x8 Bayer matrix for Fast. Error diffusion is libplacebo-only, so the UI
  hides it unless Vulkan is forced. With dithering on, upscaler intermediates
  stay 10-bit even for 8-bit streams, and FSR1 has a PQ dither RCAS variant.
- **Blue noise grain (2026-09-29):** the opt-in `ditherGrainMode` (Off/Light/
  Medium/Strong, `--dither-grain`) adapts Lilium's ReShade
  `lilium__blue_noise_dithering.fx` (GPL-3.0) as extra blue noise added
  *ahead of* the dithering quantizer, never in place of it, so it only applies
  while dithering is on. Presets and the display model are in `dithergrain.h`.
  Per channel, the noise is clamped so it never leaves [0, 1] (no clipping
  bias; exact black and white stay put), and channels with zero amplitude pass
  through unchanged. SDR amplitude is in steps of the quantizer's own depth. PQ
  amplitude is sized in 10-bit steps of the display's gamma 2.2 response scaled
  to its peak (D3D11: `DXGI_OUTPUT_DESC1.MaxLuminance`; libplacebo: the
  target's `hdr.max_luma`; 1000 nits if unknown), then added symmetrically in
  PQ. D3D11 does this in `addDitherGrain()` in `d3d11_dither.hlsli`, reading
  b1 (now 32 bytes, rewritten only on change) and blue noise at t4. Vulkan
  uses `DitherGrainHook`, a native `PL_HOOK_OUTPUT` hook with the
  `PL_HOOK_SIG_COLOR` signature, so its code is appended to libplacebo's final
  pass before `pl_shader_dither()`. An mpv-style user shader was tried and
  rejected: it forces an fp16 intermediate that biased near-white 8-bit
  output by about -0.13 steps and 10-bit PQ by about -0.5 before dithering. A
  GPU harness (2026-09-29, one Windows GPU, not a live stream) found D3D11
  grain-off output bit-identical to the previous shader, and grain-on per-level
  bias of at most 0.012 steps (SDR) and 0.11 (PQ strong, within sampling noise)
  in both renderers, including libplacebo upscaling into a letterboxed crop.
- **Debanding:** `d3d11_deband.hlsli` ports libplacebo's `pl_shader_deband()`
  into `*_deband_pixel` and `*_deband_dither_pixel` variants of the
  4:2:0/AYUV/Y410 shaders (constants at b3, per-frame seed). It debands each
  plane at its own texel size before color conversion, inside the
  color-conversion draw rather than as libplacebo's separate plane pass.
  `updateDebandConstants()` mirrors `plane_deband()` for P010 scaling, grain
  neutral points and HDR grain scaling. Both renderers take presets from
  `debandpresets.h`. PyroWave's planar shader has no deband variant.

Linux Vulkan LS1 integration (2026-09-24, based on MAKO's GPL-3.0-or-later
implementation): the opt-in `ls1upscaling` setting selects the Vulkan frontend
and adds a four-stage LS1 Quality compute hook at libplacebo's resizable RGB
stage. Libplacebo debands the decoded image before this hook and applies final
output dithering afterward. The hook is limited to SDR, whole-image RGB input,
and output sizes larger than the source. It uses the user's Steam-installed
`Lossless.dll` (or an explicit path) and loads `libvkd3d-shader.so.1` at runtime;
neither is linked, bundled, or persisted by the app. The DLL's model resources
are parsed and translated in memory when the stream renderer starts. The
reconstruction writes to libplacebo's floating-point output texture, and
timeline semaphores synchronize the app's four Vulkan compute passes with
libplacebo. A missing DLL, incompatible translator, or unsupported Vulkan
feature leaves standard scaling active and logs the reason. FSR1 and LS1 are
mutually exclusive in settings. LS1 adds GPU work; live frame rate and VRR
readiness still need measurement on a supported Linux GPU. The user's Windows
Steam DLL passed resource extraction and all four stages translated with
open-source vkd3d 1.17 in a local test. A headless Lavapipe test with Vulkan
validation submitted all five sharpness variants and read back nonzero output
pixels without validation errors. That does not establish visual quality or
live-stream performance.

Linux Vulkan FSR1 integration (2026-09-24, based on upstream PR #1557): the
opt-in `fsr1upscaling` preference selects the libplacebo Vulkan frontend when
available and attaches the PR's SDR/PQ FSR1 luma hooks to this fork's existing
render parameters. Libplacebo debands the source planes before the luma hook
and dithers the final output afterward. Both direct and optional offscreen VRR
preparation use the same hook selection, preserving the existing worker and
presenter timing policy. The hook only runs when output area exceeds source
size; a failed shader parse falls back to ordinary Vulkan scaling. The option
is hidden outside Linux builds with Vulkan support and defaults off. Added
GPU work may alter frame readiness and must be measured live; this source
change alone does not establish throughput or smoothness.
The RCAS sharpness slider saves a 0-100 setting and substitutes the matching
0-2 RCAS stop value into both shaders when the stream starts. Its 62.5 default
retains the PR's 0.75-stop setting; the minimum is still a mild RCAS pass.
Changes take effect after reconnecting.

Windows D3D11 FSR1 integration (2026-09-25): the same `fsr1upscaling` and
RCAS sharpness settings now also drive `D3D11Fsr1Upscaler` in
`d3d11fsr1.cpp`, so FSR1 no longer needs the Vulkan frontend on Windows. It
uses AMD's unmodified FidelityFX FSR 1.0.2 headers, compiled with FXC into five
committed pixel shaders (`d3d11_fsr1_*.fxc`, see `build_hlsl.bat`). When the
aspect-fitted output area exceeds the stream area, `drawVideoPlanes()` runs the
color-conversion shader into a stream-sized RGB intermediate, then EASU into a
destination-sized texture, then RCAS into the back buffer inside the letterbox
rectangle. It then restores the full viewport, so overlays draw as before. Unlike the Linux
luma hook, both passes filter RGB. PQ frames use variants that convert to
approximate gamma 2.0 around each pass, as `FSR1_HDR.glsl` does. Dithering
(SDR and, since 2026-09-28, PQ) moves from the color-conversion shader to the RCAS pass, so the
upscaler never smears the pattern. The decision is recomputed whenever the
video vertex buffer is rebuilt, which happens on a frame-format change or a
resize. A shader or resource failure logs and keeps bilinear scaling. The work
runs inside `prepareFrameForPresent()`, so it adds to both the legacy and the
VRR preparation time. This has only been compiled, not measured on a live
stream.

Windows D3D11 LS1 integration (2026-09-25): the `ls1upscaling` and LS1
sharpness settings now also drive `D3D11Ls1Upscaler` in `d3d11ls1.cpp`. There
is no DLL path setting: `Lossless.dll` is found in the Steam libraries (or
`MOONLIGHT_LOSSLESS_SCALING_DLL`), and `SystemProperties` hides the LS1 option
when it isn't found.
Both it and `D3D11Fsr1Upscaler` derive from `D3D11Upscaler`
(`d3d11upscaler.cpp`), which owns the enlargement rule, the stream-sized RGB
intermediate, and the letterbox viewport. The renderer holds at most one
upscaler; LS1 wins if both settings are on, as on Linux.

Lossless Scaling is itself a D3D11 application, so its compute shaders run
without translation:
- `Lossless.dll` is mapped as a data file with `LoadLibraryEx`, and resources
  147+3×variant through 149+3×variant plus 146 go directly to
  `CreateComputeShader`.
- The resource IDs, registers (t0/t1, u0, b0, s0), the 48-byte parameter block
  and the 16×16 dispatch sizes match `ls1shaders.cpp`/`ls1vulkan.cpp`.
- D3D11 orders the dependent dispatches itself, so no explicit barriers are
  needed.

The pipeline:
1. Stages 1 and 2 write RGBA8 intermediates at stream size.
2. Stage 3 writes an R8_SNORM feature texture at twice the stream size.
3. The reconstruction writes an RGBA16F texture at destination size.
4. `d3d11_upscale_copy_pixel.hlsl` copies that into the letterbox rectangle,
   with a dither variant.

LS1 is SDR only. `drawVideoPlanes()` draws PQ frames directly and hides the
overlay chip for them. The DLL is found from an explicit path,
`MOONLIGHT_LOSSLESS_SCALING_DLL`, or the Steam registry keys plus
`libraryfolders.vdf`. Nothing from it is stored.

A headless WARP test ran variants 0, 2 and 4 against the user's installed DLL:
the output was complete, left the letterbox untouched, stayed close to the
bilinear reference with steeper edges, and reconfigured cleanly. That doesn't
establish visual quality, and GPU cost hasn't been measured on a live stream.

On successful `LiStartConnection()`, the session records the connection start
time. `Session::exec()` owns the SDL event loop while streaming, so it raises
the stream window once two seconds have elapsed there; a QML timer would not
run during that loop.

### Intra Refresh negotiation (2026-09-23)

Intra Refresh negotiation (2026-09-23, based on `eefde52f`) is owned by the
parent repository's `moonlight-common-c/SdpGeneratorExtensions.c`. The qmake
target compiles this wrapper instead of the submodule's `SdpGenerator.c`;
the wrapper includes that unchanged source with only its entry point renamed.
The Advanced Settings `Use Intra Refresh` preference defaults off because it
trades image quality and loss-recovery time for smoother frame sizes. When
enabled, the wrapper adds
`x-ss-video[0].intraRefresh:1` to the generated attribute block for Sunshine
protocol versions at least 7.1.350 when the decoder advertises reference-frame
recovery for the actual negotiated codec. The setting is disabled in the UI
when the selected decoder/codec combination does not advertise that capability.
Other cases return the original payload unchanged. The existing RTSP path
handles the updated length and encryption. There are no submodule edits or
public callback changes. This is a capability request, not confirmation that
the host encoder enabled Intra Refresh. The wrapper relies on common-c's
internal generator contract; its SDP regression test must pass when updating
the submodule.

### Changing settings during a stream (2026-10-24)

The gamepad menu's Advanced > Change Settings pages change V-Sync, VRR Pacing
Mode, timestamp pacing, bitrate, the downscaling filter, FSR1/LS1 and their
sharpening, dithering, blue noise grain and debanding. They are the one place
the presentation snapshot changes after launch. Each change is written to the
session's preferences object and saved by
`StreamingPreferences::saveStreamSetting()`: to the PC's override when it
overrides that group (`bitrate`, `vsync`), otherwise to the global settings,
writing only that setting's keys so command-line options for one stream are not
saved with it. A bitrate override now also turns `autoAdjustBitrate` off in the
PC's copy, so a native stream mode no longer replaces the PC's bitrate.

`Session::applyStreamSetting()` then takes one of two paths:

- The host reads it at launch: the bitrate, and any pacing change after which
  `resolvePacingSettings()` (the same resolution `snapshotPresentationSettings()`
  uses, run against the live window) disagrees with the `clientVrrRequested`
  value sent at launch (`m_HostVrrRequested`). The stream ends without quitting
  the host app, and `StreamSegue.qml` starts a session from
  `createRestartSession()`, which always resumes, once the old session's
  cleanup releases the active-session semaphore.
- Otherwise the changed fields are copied into `m_PresentationSettings` and an
  `SDL_RENDER_DEVICE_RESET` rebuilds the decoder and renderer as a display change
  does: the connection stays up and an IDR is requested. A pacing change on this
  path never starts or stops VRR Pacing Mode, so the window mode is untouched.

Rows are shown only where they take effect: renderers report the decoder
parameters they apply through `IFFmpegRenderer::getVideoOptions()` (D3D11VA and
libplacebo today), the downscaling filter needs a stream drawn smaller than it
decodes, FSR1/LS1 one drawn larger (LS1 not under host HDR), and VRR Pacing Mode
needs `canUseVrr()` on the current display. An upscaler that is on stays listed
so it can be turned off. These pages anchor the card at `OverlayAnchorLeft` and
leave `OverlayMenuBackground` off, so the stream is undimmed; the first pages
keep the centered, dimmed card. The menu is the existing painted overlay
texture, republished only on input, so it adds no per-frame work. Diagnostic
trace metadata still describes the settings at stream start.

### Stats history graphs (2026-09-20)

The performance settings select one of three saved arrangements: text only,
text plus graphs (the default), or graphs only. The stats hotkey (keyboard,
gamepad combo, and the gamepad menu item) toggles the selected arrangement as a
unit through `Session::toggleStatsOverlay()`.
The graphs are a second overlay type, `OverlayDebugGraphs`, anchored top right
opposite the existing text, and are painted with QPainter by
`Overlay::Painter::paintStatsGraphs()` at a fixed pixel size, matching the fixed
font size of the text overlay rather than scaling with the viewport.
The stream-info chips distinguish a 10-bit source from an 8-bit renderer output
as `10-bit -> 8-bit` when the active renderer reports that output depth; the
D3D11, EGL and libplacebo Vulkan renderers provide it.

`Overlay::StatsGraphs` owns a sampling thread that reads cumulative counters
every 100 ms and keeps the last 10 seconds, plotting the difference between
consecutive reads. It plots, in order, incoming frame rate, rendering frame
rate, average host processing latency, frames dropped by the network
connection, frames dropped by network jitter, average network latency, and
video bandwidth. Sampling runs whether or not the graphs are visible, so a full
window is already present when they are brought up; painting and surface
publication happen only while they are on screen.

The sample interval is independent of the roughly one-second decoder stat
windows behind the text overlay, which are unchanged. Rendered and pacer-dropped
frames come from `Pacer::telemetrySnapshot()` directly, because the decoder
windows only merge that cumulative snapshot once a second. Incoming frames,
network drops and host processing latency are decoder-thread state, so
`submitDecodeUnit()` republishes those totals under a lock for the sampling
thread to read; the sum of the global and active windows is continuous across a
window rollover. RTT comes from `LiGetEstimatedRttInfo()` and is a gauge, not a
counter. Bandwidth differences a running total of the same `du->fullLength`
bytes that feed `BandwidthTracker`, because that tracker deliberately smooths
over 2.5 seconds of 250 ms buckets and would flatten a 100 ms graph; like the
`DISPLAY_BITRATE` text line, it is video payload without FEC overhead.

An interval with no decoded frames in it carries no new latency measurement, so
both latency graphs hold their previous value rather than plotting a zero that
would read as an improvement. Rates and drop counts do fall to zero, which is
how a stall appears. A counter that decreases, which happens when the decoder is
reinitialized mid-session, is treated as a new baseline rather than a delta.
Each graph auto-scales to the window's maximum, rounded up to the next
1/2/5 x 10^n and floored at a per-metric minimum so an idle graph does not
amplify noise. These are the same measurements the text overlay reports as
running averages; the graphs add time resolution, not new instrumentation.

The opt-in "Lateness vs host timestamps" graph (2026-10-07,
`PG_PRESENTATION_LATENESS`) is the exception: it is new instrumentation, and
only fixed pacing produces it. The settings page hides it while V-sync and VRR
are both requested, and the session drops it while VRR Pacing Mode is active.
`Pacer::renderFrame()` passes each presented frame's RTP timestamp, which the
decoder stores in `frame->pts`, and the presentation-call return time to
`PresentationLateness` ([presentationlateness.h](app/streaming/video/presentationlateness.h)).
Lateness is that frame's `presentUs - rtpUs` minus the smallest such offset
in the last 3 seconds (twelve 250 ms buckets), so the fastest recent frame
reads zero and host/client clock drift is followed. RTP wraps are unwrapped. A
repeated or backwards timestamp, or a step of more than 1 s in the offset
between consecutive frames, starts a new baseline. The graph plots the
per-interval mean, with min and max as the spread. The spread's top edge
approximates the playout delay a timestamp-following pacer would have needed
in that interval (see [the proposal](docs/timestamp-pacing-proposal.md)). It
is a CPU presentation-call boundary, not scanout, and frames the pacer dropped
are absent. `tst_presentationlateness` covers cadence, delay, baseline expiry,
wrap, discontinuities and drift.

The opt-in performance graphs `Incoming smoothness (host)` and `VRR cadence
smoothness` plot the same two scores ten times a second on a fixed 0-100%
axis, holding the last valid value while a score is unavailable. The VRR graph
follows whichever score the text overlay shows beside `VRR pacing` for the
active policy, read from the pacer telemetry's readiness window.

### Timestamp pacing (2026-10-07)

An opt-in fourth presentation mode, built from
[the proposal](docs/timestamp-pacing-proposal.md). Offline evaluation, known
gaps and next steps are in
[timestamp-pacing-findings.md](docs/timestamp-pacing-findings.md). Under
Gamescope it was tested on a Steam Deck and a Steam Machine over nineteen
captures (2026-10-08 to 2026-10-09); each capture, its finding and the change
it led to are in
[gamescope-timestamp-pacing-testing.md](docs/gamescope-timestamp-pacing-testing.md).
Windows has only the earlier pacing logs.

**What the Gamescope testing established (outline).**
- **Where Gamescope's state is.** Root properties of its first Xwayland
  (`:0`), not the game's `DISPLAY` (`:1`). Reliable: refresh-rate feedback,
  VRR capable, VRR feedback (KMS `VRR_ENABLED`), Allow Tearing, limiter
  feedback (on/off only). Stale or misleading: `GAMESCOPE_FPS_LIMIT`,
  `GAMESCOPE_DYNAMIC_REFRESH`, and `GAMESCOPE_VRR_ENABLED`, which is Steam's
  toggle and reads 1 on the Deck's non-VRR panel. The WSI layer's refresh
  cycle restarts at 16.67 ms for every swapchain, so it can't set the grid.
- **What display reports mean.** Gamescope reports only frames it showed,
  at the V-blank it scheduled (which can still be ahead when read), so a
  missing report is a replaced frame. Reports pause while Quick Access is
  open.
- **How frames reach the screen.** Mailbox replaces a frame that misses the
  latch (about 4 ms before the V-blank on the Deck). The frame limit forces
  FIFO and overrides Allow Tearing. While VRR is in use, Allow Tearing
  changes nothing.
- **VRR's ceiling.** A stream at a VRR display's maximum refresh leaves no
  headroom: the display refreshes at a fixed rate, the host's clock drifts
  against it (135–650 ppm, one refresh every 13–62 s), and frames are
  replaced in bursts at each wrap. The limit's FIFO avoids the bursts for
  about a refresh more latency. Pacing such streams on the grid was tried
  and reverted (fewer replaced frames, 6 ms more latency). A rate a few FPS
  below, R − R²/3600, was smooth in every Gamescope setting; the frame rate
  menu now offers it (see "Recommended frame rate" below).
- **Decode.** VAAPI returns a frame before its GPU decode finishes: about
  4.5–6 ms later on the Deck (2560x1600 HEVC 10-bit), 1.8 ms on the Steam
  Machine. A finished decode's sync can still wait 0.25–1.3 ms (driver
  contention). The stats line's decode time is the CPU side only.
- **Learned values must not learn from their own effects.** Five bugs had
  that shape: the decode delay (holding frames for it kept renders late
  enough to raise it), the render lead (FIFO back-pressure counted as
  rendering), the margin under FIFO (gaps in reports read as replaced
  frames), the margin at VRR's ceiling (a release lead near a whole
  refresh made frames collide, and each collision widened it) and the
  frame limit's period (a grid spaced to it shows frames no closer, so a
  slow desktop held a 90 FPS game at 18 FPS). Each value now learns only
  from samples it can't cause itself, is bounded, and has a way back down
  that doesn't depend on the condition it creates (for the decode delay,
  the median-wait decay; for the limit, measuring it again at one
  refresh).
- **Per-frame fixes collide.** Moving individual late frames to the next
  V-blank made them collide (up to 394 frames dropped in a stretch); one
  shift for all frames with hysteresis replaced it.
- **The frame limit was not smoother in itself.** Its early edge (99.9% vs
  97.4% single refreshes on the Deck) came from the decode delay's extra
  holding. With that bounded, the limit and fixed refresh are close.

**How it was tested.**
- **Captures:** Settings' diagnostic capture (`MOONLIGHT_VRR_TRACE`) writes
  the session log, the timestamp trace (`*.tstrace`, expanded by
  `scripts/decode-vrr-trace.py`) and, only while tracing, the display probe
  report with a sampler that logs Gamescope's state on each change.
- **Analysis:** split each trace at the sampler's setting changes (trace and
  sampler times differ by up to a second), then per stretch: single-refresh
  display intervals and repeats, replaced frames (presents with no report),
  pacer drops, decoder output → display, present → display, the release
  lead, extra margin, late shift, decode waits and delay, and host vs
  display clock rates from RTP and display times. Check the host's frame
  rate first: drops there look like pacing problems.
- **Discipline that came out of it:** several builds mostly fixed the
  previous build's regressions, and an early latency comparison rested on
  9 s of data. Change one thing at a time, compare the same Gamescope
  setting across builds, and replay existing traces offline before choosing
  a threshold (the decay's 0.15 ms came from replaying the decode waits of
  eight captures). The comparison against the build before decode readiness
  was never run.
- **Still open:** VRR with a limit below the stream's rate, Windows VRR
  detection (NVAPI, the EDID or a short test window; none built), the late
  shift, and the docked Deck's 4K Mailbox stall.

**Selection.** The `timestamppacing` preference (Settings checkbox
"Timestamp pacing", `--timestamp-pacing`) is snapshotted per session in
`PresentationSettings::timestampPacing`.
- The settings page offers only the smoothing level (`timestampsmoothinglevel`:
  Off, Light (default) or Standard). Since 2026-10-08 the on-time target,
  minimum and maximum buffer and V-blank margin have no controls. They are
  read from never-written test keys (`timestamptesttargetpermille`,
  `timestamptestminbufferms`, `timestamptestmaxbufferms`,
  `timestamptestvsyncmarginus`), and the keys the settings page used to save
  are removed on save. Their replayed effect is in the findings doc,
  section 4.5.
- VRR Pacing Mode and timestamp pacing are mutually exclusive. Enabling
  timestamp pacing clears `enableVrr`, and the VRR checkbox is disabled while
  it is on. The session also treats VRR as not requested when both are set
  (an explicit `--vrr` on the command line clears timestamp pacing instead).
- It replaces fixed frame pacing (`enableFramePacing` is false) and works with
  V-Sync on or off.
- `DECODER_PARAMETERS::timestampPacing` reaches `Pacer::initialize()`, which
  creates a `TimestampPacer` instead of using `handleVsync()`.
- The decoder then builds `PacedFrame`s as it does for VRR, adding the host's
  per-frame processing latency (`setHostLatencyUs`).

**Policy** ([timestamppacingpolicy.h](app/streaming/video/ffmpeg-renderers/pacer/timestamppacingpolicy.h)).
Pure and header-only; tested by `tst_timestamppacing`.
- `RepeatDetector`: a frame with zero host latency, from a host that has
  reported nonzero latency 16 times this session, is a host repeat. Repeats are
  backdated by about one timeout, so they are shown on arrival and never
  learned from.
  - Since 2026-10-08 the detection never expires. It used to lapse 30 s after
    the last nonzero report, which a static screen (nothing but repeats)
    always reached. The repeats were then followed, and the baseline crept up
    to their ~57 ms backdating at 0.1 ms per repeat. When the picture moved,
    every frame was held that much longer, past what the three-frame queue
    covers, so frames were evicted unseen and the cursor stalled for tens of
    seconds.
  - Zero-latency frames before the detector has 16 reports are still
    followed. When a run of them lasting at least the buffer's 3 s window
    ends with a nonzero report, the policy restarts the timeline at the
    period in force before the run (`Timeline::restart`), which also clears
    the buffer's history and baseline. A shorter run can't replace the
    window minimum, so it doesn't restart.
- `Timeline`: a two-gain phase-locked loop on the RTP timeline, counting
  frames by host frame number. Gains are phase/frequency 0.25/0.01 (light,
  the default) and 0.1/0.002 (standard); "off" follows raw timestamps.
  A "strong" level (0.05/0.001) was removed on 2026-10-08 after it skipped
  3.8% of frames on a Steam Deck capture whose rate kept changing. The
  preference key became `timestampsmoothinglevel`, so values saved under the
  old `timestampsmoothing` key no longer apply.
  - An error over max(8 ms, half a period) re-anchors to the raw timestamp.
  - The period is replaced outright only when the last nine single-frame gaps
    all agree to within 10% on a rate more than 15% away.
  - Otherwise, after a re-anchor whose gap matches the long-run period (0.2%
    per frame EMA) to within 10%, the loop snaps back to that period. This
    stops brief game slowdowns from causing a run of re-anchors afterwards.
- `PlayoutBuffer`: a frame's offset is `(ready + render lead - smoothed
  source time)`. Lateness is that offset minus its 3 s rolling minimum
  (twelve 250 ms buckets), which also follows clock drift.
  - **Render lead (2026-10-18):** `TimestampPacer::submit()` passes the
    learned render lead, read once per frame. That same value sets the late
    flag and goes into the trace. Before this, the buffer covered readiness
    only. On the 2026-10-08 Deck captures, 23–91% of frames then had their
    target within the lead of readiness, the pacer's own late flag.
  - **Baseline:** follows that minimum at 2.4 ms/s, at most 0.1 ms per frame,
    as VRR Pacing Mode's playout offset does. For the first 64 frames of a
    timeline, an earlier minimum is adopted at once.
  - **Target (2026-10-18):** held in the offset coordinate, as VRR Pacing
    Mode holds its input requirements (6920a2e6). It is the configured
    percentile of 3 s of admitted offsets plus 0.5 ms. It grows at once and
    releases at 0.5 ms/s.
  - **Bounds:** the target is held between the baseline plus the configured
    minimum and maximum buffer. With no admitted frame left in the window, it
    releases toward the minimum. The reported buffer is the target's distance
    past the baseline.
  - **Why the coordinate changed:** the target used to be a delay above the
    minimum. Each step of the minimum, when an unusually early arrival
    entered or left the window, then moved the delay. The delay rose at once
    and released slowly, so the target jumped later, then overshot. On a Deck
    capture, frames sat 8–10 ms after readiness for about 10 s; it is now
    3–4 ms.
  - **Cost:** a lasting shift to earlier arrivals is followed only once the
    later ones leave the window, then at the release rate: about 9 s for
    3 ms. The old delay followed the baseline down within 1.5 s.
  - **Replay (`tsreplay`), nine 2026-10-08 Gamescope captures,
    frame-weighted, against the policy before both changes:**
    - Presented jerk over 2 ms: 32.0 → 9.9 per mille.
    - Missed planned V-blanks: 118 → 1.5 per mille.
    - Skipped frames: 14.2 → 2.3 per mille.
    - Mean delay: 11.3 → 14.1 ms (p95 18.0 → 21.6 ms).
    - Each change alone: the render lead alone gave 10.0 per mille jerk at
      14.3 ms. The coordinate change alone was slightly worse (33.3 per mille
      at 11.3 ms): removing the overshoot exposed the uncovered lead.
    - This is model output, not display timing, and its misses are judged by
      the same recorded lead.
  - A frame's target uses the target in force before it arrived.
- Admission (2026-10-07): a paced frame's lateness sizes the delay only when
  its timeline didn't restart or re-anchor, it is no more than three times the
  maximum buffer late (a stall), and over the last second frames waited on
  average no longer than a source period to enter the decoder
  (`PacedFrame::decoderQueueUs()`; a sustained backlog only grows lateness).
  - The overlay reports the share left out.
  - VRR Pacing Mode's other exclusions were tried against the 2026-10-07
    pacing log and rejected: delivery stalls over max(25 ms, 1.5 periods) with
    their trailing backlog, and host intervals under 0.75 period. Both
    shrank the buffer by 0.6–0.8 ms but raised jerk p99 from 1.08 to
    1.2–1.5 ms and late frames from 0.65% to 0.77%. Frames late beyond the
    maximum still count against the chosen share on time, so lower stall
    multiples than three did the same.
- `VblankGrid`: estimates V-blank phase and period from V-sync source wakeups.
  The earliest wakeup sets the phase; the period may move ±3% from nominal.
  The grid is stale 250 ms after the last real V-blank.
- `PhaseLock`: active when the source period is within 2% of a whole multiple
  of the refresh period. It trims targets (±half a refresh) to sit mid-interval,
  so drift costs one deliberate repeat or skip per boundary crossing.
  - **Wrap hysteresis (2026-10-18):** once per drift cycle, the trim's
    resting point fell on its wrap. Frame noise then carried it back and
    forth: a repeat, a skip, a repeat, each moving every frame a refresh.
    A Deck capture (2026-10-18, 90 FPS at 90 Hz, 43 ppm, so one wrap due
    every 256 s) wrapped 31 times in 8 minutes; 16 were undone within 2 s.
  - **Rule:** a wrap back the way the last one came now needs the trim
    0.5 ms past the boundary (at most an eighth of a refresh). A wrap onward
    is unchanged. While a wrap back is held off, a frame can land up to
    0.5 ms before its target, or wait that much longer than a refresh.
  - **Replay** (ten Gamescope captures, frame-weighted):
    - Wraps per grid phase: 36.5 → 17.3. On the Deck capture: 51.9 → 19.5,
      with wraps undone within 2 s at phase 0 going from 81 to 13.
    - Presented jerk over 2 ms: 8.09 → 7.65 per mille. Skipped: 2.12 → 1.90.
      Missed: 1.46 → 1.57. Mean delay: +0.09 ms.
  - **Not adopted:**
    - The remaining wraps mostly follow the buffer moving 1–2 ms.
    - A longer hold on the waiting side reduces them: 2.5 ms gave jerk 7.01
      and missed 1.19 per mille, for +1.0 ms delay; 4 ms gave 6.75 for
      +1.7 ms. That is a latency choice, so it isn't the default.
    - A longer hold on the early side showed frames too early: 2 ms raised
      missed to 2.56 per mille.
  - **Platforms:** the lock serves every V-blank grid, D3D11's included, so
    this applies on Windows too. Windows traces weren't available to
    replay.

**Execution** ([timestamppacer.cpp](app/streaming/video/ffmpeg-renderers/pacer/timestamppacer.cpp)).
The decoder thread schedules each frame into a queue of at most three; a full
queue evicts the oldest. The pacing thread waits on a condition variable, then
uses `VrrTargetWaiter` for the last 2 ms, and hands frames to the existing
render queue, so every renderer works unchanged. As in VRR Pacing Mode, the
waiter wakes early by the 95th percentile of its last 19 sleep overruns
(`schedulerDelayUs`, at most 500 us) and spins the rest.
- **Queue-full logging (2026-10-08, `c5d8884b` plus logger fix):** the
  one-time warning is emitted after releasing the pacing mutex, waking the
  worker and returning the evicted decoder surface. A Windows capture showed
  a logger worker-start warning recursively entering the locked Qt thread
  pool from an MMCSS decoder thread, freezing frame delivery. The logger now
  starts workers at normal priority. `LogDispatch` writes nested startup
  warnings directly; warnings from the writer itself go to stderr without
  taking the logger locks. Both queued writes and final flush use that guard.
  `tst_logdispatch` covers startup and writer re-entry, including a real Qt
  worker. This fixes the captured lock cycle; live streaming validation is
  still required.
- **V-Sync with a V-sync source:** the frame is released at the first V-blank
  at or after (target + trim), minus the learned release-to-present time, a
  2 ms margin (4.5 ms under Gamescope; see below) and any margin added by
  missed V-blank detection.
  - Frames assigned to the same V-blank are reduced to the newest (mailbox).
  - D3D11's non-VRR `Present(0)` already replaces a pending frame at the next
    V-blank.
  - Renderers with `RENDERER_ATTRIBUTE_FORCE_PACING` (exclusive fullscreen)
    flip at once, so for them the frame is released at the V-blank itself.
- **V-Sync off, or no V-sync source:** frames are released at target minus the
  learned release-to-present time.
- The V-sync thread passes only real V-blanks to the grid.
  `IVsyncSource::waitForVsync()` now reports success, and an async source's
  100 ms timeout is ignored.
- Release-to-present time is measured from release to `renderFrame()`
  returning. It rises quickly and falls slowly, within 0.2–8 ms.

**Gamescope (Linux, 2026-10-07).** In Game Mode, Qt and SDL normally run on
Gamescope's Xwayland, where there is no V-sync source. Under Gamescope
(`GAMESCOPE_WAYLAND_DISPLAY` set), timestamp pacing adapts as follows:
- **Renderer:** it prefers the Vulkan renderer, through the same
  `preferVrrRenderer` path as VRR, in both the startup probe and playback.
  This keeps the negotiated color range consistent.
- **Present mode:** with V-Sync, `PlVkRenderer::vsyncPresentMode()` picks
  Mailbox on native Wayland instead of FIFO, so a newer frame replaces one
  still waiting. Under Gamescope it picks Mailbox (or Immediate) whether or
  not timestamp pacing is on; see "Gamescope owns V-Sync" below. Gamescope treats both Immediate and
  Mailbox as "async" (`wlserver_surface_is_async`). Its own settings then
  decide presentation:
  - **Allow Tearing:** async flips that tear.
  - **VRR in use:** a flip on arrival (`adaptive_sync_uncapped`).
  - **Otherwise:** the newest commit at each refresh.
  - **Steam's frame limiter:** forces FIFO through the WSI layer regardless,
    `GAMESCOPE_WSI_FRAME_LIMITER_AWARE` or not.
- **Present times:** `VulkanTiming` is enabled for timestamp pacing under
  any Gamescope session whose device has `VK_GOOGLE_display_timing`
  (not for test-only probes). Since 2026-10-08 that no longer requires the
  Gamescope WSI layer (`ENABLE_GAMESCOPE_WSI=1`), which VRR's diagnostics
  still require. A Steam Deck Game Mode capture on the `mailbox-test` branch
  got display times for 99.9% of presents without it. The fixed
  path's `renderFrame()` arms it around each submit. Gamescope's actual
  present times (`VK_GOOGLE_display_timing`) reach the pacer through
  `IFFmpegRenderer::setDisplayEventSink()`, together with the compositor's
  refresh period (`vkGetRefreshCycleDurationGOOGLE`, queried at most once a
  second). These times build the V-blank grid when V-Sync is on and no
  V-sync source exists. The grid ignores repeated or slightly late reports.
  Since 2026-10-08 the grid's period comes from the display-mode probe's
  `GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK` whenever Gamescope reports it,
  and the renderer's refresh period is then ignored: the WSI layer starts
  each swapchain at 16.67 ms and Gamescope resends the real cycle only when
  it changes, so a recreated swapchain read 60 Hz on a 120 Hz display (see
  "Gamescope display probe" below). A change in the reported rate
  reconfigures the grid, resets the phase lock, releases and miss detector,
  logs, and writes a `display_mode` trace row with the new grid period.
- **Submit margin:** starts at 4.5 ms instead of 2 ms
  (`TimestampPacingOptions::GamescopeVsyncMarginUs`, 2026-10-08), unless the
  `timestamptestvsyncmarginus` test key sets one. On a `mailbox-test` Deck
  capture, frames handed over less than about 6 ms before a refresh mostly
  missed it, and that branch's learned lead settled near 4.5 ms. Missed
  V-blank detection still widens it from there.
- **Display-mode probe:**
  [gamescopedisplaystate.cpp](app/streaming/video/ffmpeg-renderers/pacer/gamescopedisplaystate.cpp)
  opens a private Xlib connection and reads root-window properties. Steam
  sets them, and Gamescope reports VRR, only on Gamescope's first Xwayland
  (`GAMESCOPE_XWAYLAND_SERVER_ID` 0). Game Mode runs two, and games get the
  second as `DISPLAY`, where every property reads zero ("fixed refresh" on a
  VRR display, fixed 2026-10-08). When `DISPLAY` is not server 0, the probe
  tries `:0` to `:32` for the server 0 with the same `GAMESCOPE_PID`
  (reachable from Flatpak through abstract sockets); none found leaves the
  mode unknown. The pacing thread polls it every 250 ms with its lock
  released.
  Precedence:
  1. `GAMESCOPE_VRR_FEEDBACK`: VRR. If `GAMESCOPE_LIMITER_FEEDBACK` is also
     set, the probe warns once: FIFO queues frames when the limit is below
     the stream's rate, and Gamescope does not report the limited cadence a
     grid would need.
  2. `GAMESCOPE_LIMITER_FEEDBACK`: frame limited (FIFO). It comes before
     tearing because the limit's FIFO overrides Allow Tearing.
  3. `GAMESCOPE_ALLOW_TEARING`: tearing.
  4. Otherwise: fixed refresh.
  `GAMESCOPE_FPS_LIMIT` is not read. Until 2026-10-08 a nonzero value came
  first and meant frame limited. Captures showed it stuck at 60 on a Steam
  Machine and 90 on a Deck whether or not a limit applied, which locked whole
  streams out of the grid and VRR. Gamescope rewrites the limiter feedback
  whenever its effective target FPS becomes zero or nonzero
  (`update_runtime_info()`), through either Steam's property or
  `gamescope_control`. On a Deck capture, it read 1 exactly while the
  renderer's presents blocked at the refresh rate (60, 53, 77 and 90 Hz).
  It does not give the limit's value.
- **Pacing per mode:** fixed refresh, frame limited and no probe use the
  grid (`TimestampPacer::modeUsesGrid()`); VRR and tearing present at target
  minus render lead. Under a frame limit, FIFO shows each queued frame at
  its own refresh, so the grid's release of at most one frame (the newest)
  per V-blank drops surplus frames instead of queuing them. That reduction
  only works among frames waiting together, and nearly every frame under
  Gamescope is ready after its target and released at once, so under a
  frame limit `vblankForLocked()` also moves a frame whose V-blank an
  earlier release already took to the next V-blank. Until 2026-10-22 a
  newer frame due by then replaced it. Since then
  `TimestampPacer::holdsFrontLocked()` keeps it, and the frames behind it
  follow a refresh late, until the plan leaves a refresh empty, a frame
  would be two refreshes late, or the hold reaches
  `TimestampPacingOptions::fifoHoldLimitUs()` (Light 1 s, Standard 2 s,
  Off 0: always replace); the frame is then replaced as before, which gives
  the refresh back. Released rows record `postponed_refreshes` and
  `hold_us`. tsreplay models it (`hold_ms`); on four Deck captures it cut
  jerks over 2 ms by 11–42% for 0.35–4.3 ms more mean delay (see
  [the testing notes](docs/gamescope-timestamp-pacing-testing.md), "Frame-limit
  hold"). The phase lock still learns from the V-blank the target
  falls on. Gamescope doesn't publish the limit itself, so a limit below
  the refresh rate is measured from display intervals and the grid spaced
  to it (see "Frame limit period" below); until then it queues. Entering
  frame limited logs a warning.
  A mode change resets the grid, phase lock and release records, and logs.
- **Missed V-blank detection:** each grid release records its planned
  V-blank. Each reported display time is matched to the release planned for
  the nearest V-blank at or before it (within −1.5 to +0.5 refresh).
  - `TimestampPacing::MissDetector` rounds display minus planned V-blank to
    whole refreshes. A frame has missed when that lag is above the least lag
    among the last 128 frames, so a constant compositor delay is not counted.
  - A miss adds 0.5 ms to the submit margin, up to 6 ms, unless the frame's
    render waited at least that long for its decode (`Release::decodeBound`,
    set from `notePresented()`'s decode wait): it then presented when the
    decode finished, and handing it over a step earlier couldn't have
    helped. The same goes for replaced frames. Such a miss still counts in
    the overlay's miss rate. Renderers that don't report a decode wait
    (D3D11) are unaffected.
  - After 5 s without a miss that widens it, the margin releases 0.25 ms.
  - A new compositor refresh rate sets the margin back to zero: Gamescope
    switches up to a poll (250 ms) before the pacer sees it, and frames
    planned on the old grid until then look missed.
  - Before 2026-10-09 every miss widened it. At 60 FPS on the Deck's 90 Hz
    panel (captures 17 and 18 in the testing notes) frames planned for a
    V-blank their decode finished only 2.6-3.6 ms before missed in stretches
    of 20-25 s; 611 of 624 misses were decode-bound. They held the margin at
    6 ms, and at 60 Hz afterwards that cost about 2.7 ms of latency for the
    two minutes it took to decay. `tst_timestamptrace`'s
    `marginIgnoresDecodeBoundMisses` runs the real pacer against a modelled
    Gamescope (FIFO at 90 Hz, 4 ms latch, 4.8 ms decodes) with a 55-65 FPS
    stream: about 18% of frames miss, the margin stays at 0-0.75 ms, and
    the old rule takes it to 6 ms. On the Deck (capture 19) 60 FPS at 90 Hz
    still missed 7-31% of frames in each 10 s with the margin at 0-0.5 ms,
    and 60 FPS at 60 Hz then ran at 10.1 ms decoder output to display.
  - The text overlay shows the mode, the miss rate and the added margin.
  - Windows (2026-10-08) uses the same detection; see "Windows missed V-blank
    detection" below.
- **Not covered:** the Moonlight-side VRR request (`clientVrrRequested`) is
  unchanged. EGL renderers, and Gamescope sessions whose device lacks
  `VK_GOOGLE_display_timing`, get no grid and fall back to presenting at
  target.
- **Validation:** tested on SteamOS from 2026-10-08 (see the outline at the
  start of this section and the testing notes).

**Gamescope owns V-Sync (2026-10-08).** Gamescope composites every frame
and decides how it reaches the display: async flips that tear (Allow Tearing),
a flip on arrival (VRR), the newest commit at each refresh, or a queue
(Steam's frame limit). Application V-Sync only queued frames in front of that,
so under Gamescope (`GamescopeDisplayState::runningUnderGamescope()`, which is
now header-only and checks `GAMESCOPE_WAYLAND_DISPLAY`) Moonlight no longer
syncs itself:
- `StreamingPreferences::effectiveVsync()` (QML `effectiveVsync`) is always
  true; the saved `vsync` preference is kept and logged but ignored. VRR
  Pacing Mode and the timestamp pacing V-blank grid therefore remain eligible.
- The session no longer disables V-Sync for a stream more than 5 FPS over
  refresh: nothing blocks, so surplus frames are only dropped.
- Fixed frame pacing is off, including the VRR Pacing Mode rejection
  fallback in the session and in `Pacer::initialize()`. Qt and SDL run on
  Gamescope's Xwayland, which has no V-sync source, so it had rendered frames
  on arrival anyway.
- Vulkan: `PlVkRenderer::vsyncPresentMode()` returns Mailbox, else Immediate,
  instead of FIFO, for ordinary V-Sync and for every fixed fallback of VRR
  Pacing Mode (invalid refresh, main-thread renderer, no adaptive mode, worker
  start failure). Gamescope treats both as async, so neither tears unless
  Allow Tearing is on. FIFO remains only when neither is supported.
- EGL uses swap interval 0 and the SDL renderer omits
  `SDL_RENDERER_PRESENTVSYNC`, as on native Wayland.
- Settings hide the V-Sync and Frame pacing checkboxes under Gamescope
  (`SystemProperties.isGamescope`) and say tearing is set in Steam's Quick
  Access menu.
- Not validated on SteamOS. In particular, that Mailbox and Immediate behave
  alike for Xwayland clients with Allow Tearing off comes from the timestamp
  pacing notes above, not a fresh capture.

**Gamescope display probe (2026-10-08, diagnostic).** On a 120 Hz VRR
display Moonlight was seeing 60 Hz fixed refresh in Game Mode.
[displayprobe.cpp](app/streaming/displayprobe.cpp) records what every source
reports so a Steam Machine capture can show which one is wrong. From Gamescope
source (ValveSoftware/gamescope `36848c2f`, 2026-10-05):
- Each Xwayland's `wl_output` (and so its RandR mode, which SDL and Qt read)
  gets its refresh when the Xwayland is created: the nested refresh (`-r`)
  if given, else `g_nOutputRefresh` at that moment, which `drm_unset_mode()`
  sets to 60 Hz when nothing better is known. With more than one Xwayland,
  an output mode change updates only server 0 (Steam's). A game's server,
  which is Moonlight's `DISPLAY`, changes only when Steam resizes it through
  `GAMESCOPE_XWAYLAND_MODE_CONTROL`, and then takes the refresh of that moment
  (`steamcompmgr.cpp`, `wlserver_set_xwayland_server_mode()`).
- Gamescope publishes the live values on server 0's root:
  `GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK` (current output refresh, Hz),
  `GAMESCOPE_VRR_CAPABLE` (connector supports VRR), `GAMESCOPE_VRR_ENABLED`
  (Steam's VRR setting), `GAMESCOPE_VRR_FEEDBACK` (KMS VRR active right now,
  `IsVRRActive()`), `GAMESCOPE_DISPLAY_MODE_LIST_EXTERNAL` and
  `GAMESCOPE_DISPLAY_EDID_PATH` (`update_vrr_atoms()`, `update_mode_atoms()`).
  The Timestamp Pacing probe reads only `VRR_FEEDBACK`, so a capable,
  enabled display that is not refreshing adaptively at that instant reads as
  fixed refresh.
- `gamescope_control` (v2+) sends `active_display_info`: connector, make,
  model, internal/HDR/VRR flags and the valid refresh rates.

Each report reads, independently: environment and OS; SDL's display modes
(and the stream window's, plus `StreamUtils::tryGetDisplayRefreshRate()`); Qt's
screens; every X server `:0` to `:32` (all root properties, RandR outputs,
CRTCs and modes); Gamescope process command lines and `gamescope --version`;
the Gamescope and session Wayland sockets (globals, `wl_output`,
`gamescope_control`); KMS through raw DRM ioctls on every card node (all
connector and CRTC properties, `vrr_capable`, `VRR_ENABLED`, the active and
listed modes with exact refresh, the EDID); sysfs connector state and
amdgpu's debugfs `vrr_range` where readable; and Gamescope's patched EDID.
Xrandr and libwayland-client are loaded with `dlopen()` and DRM uses kernel
uapi headers, because the AppImage is built with `disable-wayland` and
`disable-libdrm`. Connectors are read like `drmModeGetConnectorCurrent()`, so
the probe never forces a connector probe.
- Only while VRR tracing is on (`MOONLIGHT_VRR_TRACE`, set by the tracing
  launchers at startup or by Settings' diagnostic capture at session start)
  and under Gamescope: startup writes `<trace base>.display-startup.txt`
  and each stream `<trace base>.display-stream.txt` beside the trace (in a
  capture folder, `Moonlight.display-stream.txt`; a later stream in the same
  process adds an epoch suffix). The stream's `DisplayProbe::StreamSampler`
  thread then appends Gamescope's root state and each active CRTC's mode and
  `VRR_ENABLED` once a second when they change (otherwise every 30 s).
  Summary lines also go to the log. Without tracing nothing is read or
  written (2026-10-08; before that every Gamescope launch and stream wrote
  to `~/moonlight-display-probe`).
- `moonlight probe-display` (or `MOONLIGHT_DISPLAY_PROBE=active` for
  launchers that cannot pass arguments) writes the passive report, then
  opens a fullscreen Vulkan window and, for each present mode the surface
  offers (FIFO, Mailbox, Immediate, FIFO relaxed), presents unpaced, at 53 FPS
  and at 77 FPS (rates on no common fixed grid) for `MOONLIGHT_DISPLAY_PROBE_SECONDS` (default 4) each. It
  also lists `VK_KHR_display` displays and modes. It records
  `VK_GOOGLE_display_timing` display times and the refresh cycle, and
  summarizes display intervals as a histogram, with each interval's fit to
  60–240 Hz grids and to the pacing. Gamescope's state is sampled
  mid-phase. Fixed 120 Hz shows 53 FPS as 16.7/25 ms intervals, fixed 60 Hz
  as 16.7/33.3 ms, and VRR as 18.9 ms.
- The explicit `probe-display` report goes to `$MOONLIGHT_DISPLAY_PROBE_DIR`,
  else `~/moonlight-display-probe`, else the app data directory (Flatpak has
  no home access), else the log directory, whether or not tracing is on.
- Built into the WSL AppImage and run under WSLg (no Gamescope, no KMS,
  llvmpipe without display timing): every section reports and all four
  present modes run.
- First SteamOS captures (2026-10-08, SteamOS 20260925.101, Gamescope
  3.16.30, two active probes; no stream reports yet):
  - Steam Machine (NAVI33) on an LG TV over HDMI, 120 Hz, VRR 24–120 Hz.
    SDL, Qt, both Xwayland RandR modes, `wl_output`, KMS and
    `GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK` all said 120 Hz. KMS
    `VRR_ENABLED` and `GAMESCOPE_VRR_FEEDBACK` were 1 throughout, and paced
    frames were displayed at the pacing interval, on no fixed grid.
  - `vkGetRefreshCycleDurationGOOGLE` said 120 Hz for the probe's first
    swapchain (FIFO) and 60 Hz for its second (Mailbox) on the same window.
    The Gamescope WSI layer starts each swapchain at 16.67 ms, and Gamescope
    sends `refresh_cycle` only when the per-surface value changes
    (`handle_presented_for_window()`), so a recreated swapchain keeps 60 Hz.
    The Timestamp Pacing grid takes its period from this query.
  - `GAMESCOPE_FPS_LIMIT` read 60 while the probe's FIFO frames were
    displayed at 120 Hz and the swapchain's refresh cycle was unlimited. On
    a docked Deck it read 90 (the internal panel's) on a 60 Hz external
    display. The atom is set from Steam's property but reset internally by
    `_update_app_target_refresh_cycle()`, so it is not the active limit.
  - Under VRR, about half the paced frames were displayed within 1 ms of the
    pacing interval; present-to-display time fell in two clusters one
    120 Hz period apart, as if commits still latch on a 120 Hz schedule.
    The display times are Gamescope's scheduled V-blank
    (`ulTargetVBlank`), not a measured flip.
  - Steam Machine stream (116 FPS, 2560x1440, VRR, Allow Tearing, Steam's
    VRR, tearing and frame limit toggled during it): Timestamp Pacing
    entered "frame limited (FIFO)" at once from `GAMESCOPE_FPS_LIMIT=60` and
    never left it, so it never used a grid or followed VRR. The atom stayed
    60 throughout, including while a limit Gamescope applied held frames to
    25 ms. `GAMESCOPE_VRR_ENABLED`, `GAMESCOPE_VRR_FEEDBACK` and KMS
    `VRR_ENABLED` changed together within the sampler's 1 s resolution on
    every toggle. Display reports followed the 8.6 ms stream cadence with VRR
    and 8.33/16.67/25 ms without it. The renderer's refresh cycle read
    16.67 ms for the first 48 s (the renderer had been recreated after the
    window was shown) and changed only when the frame limit changed
    Gamescope's value (25 ms, then 8.33 ms).
  - Deck internal panel stream (old build, 90 FPS; Allow Tearing, refresh
    rate and frame limit toggled): the panel switched KMS modes (90.004,
    80.003, 60.003, 53.000, 77.029 Hz) and
    `GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK` followed each, at most one 1 s
    sample behind KMS. The renderer's refresh cycle read 16.67 ms for the
    first 135 s at 90 Hz, then was right after every change. With
    `GAMESCOPE_LIMITER_FEEDBACK` 0 at 80 Hz, 90 presents a second gave about
    70 displayed (Mailbox replacing); with it 1, presents blocked at the
    refresh rate. `GAMESCOPE_FPS_LIMIT` and `GAMESCOPE_DYNAMIC_REFRESH` stayed
    90 throughout. `GAMESCOPE_VRR_ENABLED` was 1 on the non-VRR panel
    (`GAMESCOPE_VRR_CAPABLE` and `_FEEDBACK` 0). In three stretches, display
    reports fell to 5–12 a second with gaps up to 0.9 s while presents
    stayed at 90 and focus did not change; possibly the Quick Access menu.
  - Deck internal panel stream with the new probe (90 FPS, Steam's frame
    limit, refresh rate and Allow Tearing toggled): every mode and refresh
    change was logged and the grid period followed. Fixed refresh at 90 Hz:
    a frame every refresh, release to present 1.9 ms. Frame limited at
    60 Hz before the one-per-V-blank rule: all 90 frames a second were
    released on the grid but only 60 presented, 15 replaced in the whole
    stream, release to present 9.7–11.6 ms median (14 ms p95), the render
    lead pinned at its 8 ms cap and the average frame queue delay was 15 ms.
    Tearing at 90 Hz: renders took 6.3 ms (1.9 ms with tearing off), which
    raised the learned render lead above the 2–9 ms buffer, so frames were
    released as soon as they were ready, and Gamescope reported display
    times for only 40–49 of the 90 frames a second. The longer render is not
    a tearing or present cost: in every mode, a render started within 2 ms
    of the decoder returning the frame took 5.8–6.2 ms, one started 4 ms or
    more after took 1.2–1.6 ms. `renderFrame()` waits for VAAPI's GPU decode
    (about 6 ms for 2560x1600 10-bit HEVC on the Deck), which tearing exposes
    because it releases frames on arrival while the grid holds them about
    5.5 ms. `waitToRender()` (swap_buffers and acquire) runs before a frame
    is taken, so it is not in this time. Timestamp pacing treats decoder
    output as ready and learns that wait into the render lead; VRR Pacing
    Mode tracks GPU readiness separately (`PacedFrame::noteGpuReadyUs()`).
  - Docked Deck (VANGOGH) on a 60 Hz non-VRR LG monitor over DP at 4K: all
    sources agreed on 60 Hz fixed. FIFO displayed every refresh, but
    Mailbox displayed only 22–24 frames a second (33–67 ms intervals) with
    presents blocking 25–42 ms, with Allow Tearing on for the shortcut.

**Decode readiness, frame limit period, session refresh and overlay chips
(2026-10-08).** Follow-ups to the Deck and Steam Machine captures above:
- **Decode readiness.** `PlVkRenderer::renderFrame()` now calls
  `waitForDecode()` (an explicit, timed `vaSyncSurface()`, or PyroWave's
  completion wait) before mapping the frame, outside the VRR path, which
  already waited; libplacebo's own sync then returns at once. The wait is
  reported through `IFFmpegRenderer::lastDecodeWaitUs()` (zero for other
  renderers) and `Pacer::renderFrame()` passes it, with the frame's decoder
  output time, to `TimestampPacer::notePresented()`.
  - The render lead learns from release-to-present time minus that wait.
  - `m_DecodeDelayUs` learns when decodes finish relative to decoder
    output: a render that waited at least 1 ms, and began no more than 3 ms
    after the current estimate, gives (render start − output + wait),
    rising by a quarter of the gap; one that waited less bounds it at
    (render start − output + wait), falling by 1/32. Capped at 1.5 stream
    frame periods and 20 ms.
  - Frames are held until the estimate, so renders rarely begin before it,
    and an estimate that ends up too high could stay there. So once a
    second, the delay also falls 2% while the median wait of renders begun
    no more than 1 ms after it is under 0.15 ms (at least a quarter of a
    second's frames, and 8). Replayed over six Deck and two Steam Machine
    captures (2026-10-09), renders begun after their decode had finished
    had 1 s median waits of 0.004–0.047 ms (contention waits of 0.3 ms or
    more were 0–2.5% of them), while renders begun x ms before it had
    medians of about x (0.09–0.2 ms at 0–0.3 ms, 0.32–0.48 at 0.3–0.6). The
    decay therefore stops once a typical render begins about 0.15 ms before
    its decode finishes, and the rules above leave it there, so it settles
    rather than cycling (`tst_timestamptrace` `decodeDelaySettles`).
    Renders delayed for other reasons, such as FIFO back-pressure, begin
    later and don't count. Not yet captured on hardware.
  - The first version (250 us threshold, no start window, 20 ms cap) ran
    away on a Deck capture (2026-10-09, 90 FPS, frame limit at 90 and
    60 Hz). Renders under the limit began 15–21 ms after decoder output,
    when the decode had long finished, but about 3% still waited
    0.25–1.3 ms for the sync (driver contention). Each read as a decode
    finishing that late, so the delay reached 20 ms; frames were then held
    for it, which kept every render that late and nothing lowered it. The
    pacer queue sat 2–3 deep (one "queue full" eviction burst), and decoder
    output to present was 17–32 ms median against 7–9 ms with the limit
    off. Real decode waits there were about 3 ms (2.2 ms median for renders
    started within 3 ms of output). Replaying the trace's presents through
    the current rules keeps the delay at 4.6–6.0 ms.
  - The same capture's frame-limited stretches showed 99.9–100% of frames
    on a single refresh, against 97.4–99.4% with fixed refresh, but that
    came from the extra ~15 ms of holding, not from FIFO itself.
  - `submit()` treats a frame as ready at max(decode complete, decoder
    output + `m_DecodeDelayUs`), which the policy, lateness and the trace
    use. Renderers that report no wait (D3D11, EGL, SDL) leave the delay at
    zero, so their pacing is unchanged.
  - Trace columns appended (schema unchanged): `decode_wait_us` and
    `decode_delay_us` on presented rows, `decode_delay_us` on scheduled rows.
  - Not yet captured on hardware.
- **Deck capture with the bounded estimator** (2026-10-09, 90 FPS at 90 Hz,
  frame limit toggled seven times): decode delay 4.8–6.4 ms throughout,
  pacer queue depth 0–1 (2 five times), no queue-full eviction, decoder
  output to present 8–12 ms median in both modes, frames dropped by
  pacing 0.05%, ready late 25%. Frame limited showed 99.4–100% of frames
  on a single refresh, fixed refresh 99.2–99.6%, except one fixed-refresh
  stretch below.
- **Replaced frames count as misses.** In that 30 s fixed-refresh stretch
  the host sent a steady 90 FPS and every frame was released on the grid,
  with presents returning 5 ms before their planned V-blank, yet only 68–76
  frames a second were reported shown, worsening and then recovering with
  no change in render or decode time. Gamescope returned no timing for
  about 580 frames: they missed its latch, the next frame replaced them
  (Mailbox), and the V-blank repeated a frame. The miss detector saw
  nothing because the frames it was shown were on time. `onDisplayEvent()`
  now treats each grid release planned for an earlier V-blank (within 3.5
  refreshes) that was never reported as replaced: it counts as a missed
  V-blank in the overlay, is recorded as `superseded` and `missed` on the
  display report's trace row, and three within a second widen the margin
  by 0.5 ms (up to 6 ms, narrowing as before). Gamescope leaves out a few
  reports of frames it did show; replaying the capture, that stretch would
  widen the margin 20–60 times per 10 s and the rest 0–5 times.
- **The margin alone did not stop it.** The next Deck capture (2026-10-09,
  same settings) had a 12 s stretch right after the limit was turned off:
  25 frames a second were replaced, the margin reached 6 ms within 2 s, and
  replacement continued at that rate for 10 s before fading. Frames there
  became ready 5.5 ms before their planned V-blank (10.7 ms in good
  stretches; the phase lock trim was −3.5 ms), so they couldn't be handed
  over earlier. Shown rate by how long before the V-blank the present
  returned: 31% at 0–2 ms, 60% at 2–4 ms, 91% at 4–6 ms, about 100% beyond,
  consistent with Gamescope latching about 4 ms ahead.
- **Planning a refresh later while frames are late.** A first version
  planned each frame whose expected ready time (`Entry::readyUs`, decoder
  output plus the decode delay) plus the grid release lead came after its
  target's V-blank for the first V-blank it could make. On the next Deck
  capture (2026-10-09, 90 FPS at 90 Hz, limit toggled seven times, then a
  45 FPS and a 60 Hz limit) Gamescope replaced almost nothing (at most 18
  frames a stretch), but in four of seven stretches the pacer itself
  dropped 180–394 frames (79–85 shown a second): readiness jitters around
  the limit, so a moved frame collided with the unmoved one after it, and
  frame limited stretches lost frames FIFO would have shown a refresh late.
  It was replaced by a uniform shift (`noteLateLocked()`): each grid
  release records whether it was ready too late for its target's own
  V-blank (ready + lead, the lead including the widened margin); once half
  of the last 64 were, every frame is planned a refresh later until at most
  2 of 64 would be late without it. Not used under the frame limit (FIFO
  already shows a late frame a refresh later) or without a compositor
  probe, and reset with the grid. It logs both switches and writes a
  `late_shift` trace column. Replayed over the two captures before it, it
  switches on within a second of the bad stretch (64 of 64 late) and never
  in the normal stretches (at most 18 of 64). While on, frames show about
  11 ms later; with a widened margin it stays on until the margin narrows
  (0.25 ms per clean 5 s). The capture before those (256–286 s, frames
  ready in time but still missed) would switch it on only through the
  widened margin. Not yet captured.
- **Late shift needs confirmed misses (2026-10-18).** The rule is now
  `TimestampPacing::LateShift`.
  - **What went wrong:** on a Deck capture (2026-10-18, 90 FPS at 90 Hz,
    Steam's limit off) the shift engaged with Gamescope reporting every
    frame on its V-blank. The release lead's 4.5 ms Gamescope margin
    predicted lateness that wasn't there: 262 of 265 frames predicted late
    were shown on time. The shift held every frame 11 ms later for 30 s.
    Leaving it then collided frames (12 of 64 missed), and it re-entered
    twice.
  - **New rule:** where Gamescope's display reports are arriving, the shift
    also needs 8 confirmed misses in the last 64 reported outcomes (frames
    replaced, or shown after their V-blank). That capture had at most 3,
    and 1 when it fired. The stretch the shift was made for lost about a
    quarter of its frames to replacement, about 16 in 64. That capture
    isn't on this machine, so its replay against the new rule hasn't been
    run.
  - **Unchanged:** without a report in the last second, prediction alone
    decides, as before. Leaving the shift is also unchanged.
  - **Windows and other platforms:** only Gamescope's probe enables the
    shift, so they never reach it. D3D11's `onFrameDisplayed` feeds only
    the margin. `tst_timestamptrace` pins both: D3D11 misses still widen
    the margin, and the shift never engages.
  - **Replay:** `tsreplay` replays the shift from the trace's releases,
    display reports and mode changes. Prediction alone reproduces that
    capture's recorded shift state on all 65,099 releases. The new rule
    skips the 225 s entry. Its two later entries are the recorded
    re-entries after the recorded exit, whose misses only that exit
    caused; a replay can't remove them.
- **Replaced frames and FIFO; future-dated reports.** A 210 s Deck capture
  (2026-10-09) with the 90 FPS limit on throughout had its margin climb to
  6 ms (1.90% missed V-blanks). FIFO never replaces a frame, but the
  replaced-frame check counted unreported releases anyway, and the
  widened margin then handed frames over up to 17 ms before their V-blank,
  where they waited in Gamescope's queue. Gamescope had returned all but 3
  of 18428 frames; 486 were dropped by `VulkanTiming` as future-dated (the
  scheduled V-blank of a latched frame can still be ahead when read), in
  bursts that looked like replacement. The replaced-frame check is now off
  under frame limited, and with the timestamp pacer's display sink set,
  `VulkanTiming::setFutureToleranceUs(40000)` keeps times up to 40 ms ahead
  (`future_accepted` in the summary log). VRR Pacing Mode keeps rejecting
  them. The same capture's latency swings (decoder output to present 8–21
  ms, queue depth 0–1) followed the playout buffer (4–15 ms) as it
  answered Wi-Fi arrival jitter (±4–7 ms p1/p99, spikes to 16 ms), not
  pacing: almost no frame was moved past its own V-blank.
- **Render lead under FIFO.** The next Deck capture (2026-10-09, 90 FPS
  limit on throughout, 135 s) confirmed the FIFO and future-report fixes:
  margin +0.00 ms, missed V-blanks 0.01%, no future-dated report rejected
  (22 accepted), and 99.9% of frames on single refreshes. But from 100 s
  the render lead jumped from 2.1 to 5.6 ms while rendering took 1.5 ms:
  occasional 11 ms release-to-present samples, from the render thread still
  waiting for a swapchain image behind FIFO, held it up (it rises by a
  quarter, falls by 1%), which released frames earlier (10 ms before the
  V-blank instead of 6.6 ms), kept FIFO fuller and produced more such
  waits. Frames still showed at their planned V-blank, 4–5 ms later after
  decoder output than before. With a compositor probe, the lead is now
  timed from max(release, render start); Windows keeps timing from release,
  where the wait before rendering is DXGI's frame latency. Replayed over
  that capture the lead stays 1.5–2.1 ms. The capture's other latency (up
  to 24 ms in its first 40 s) was the playout buffer (13–16 ms) after
  Wi-Fi arrival spikes of up to 59 ms. The decode delay there was 7.5–8.3
  ms, against 5–6 ms in earlier captures.
- **Sub-refresh limit detected.** The same capture's 45 FPS limit at 90 Hz
  logged "frames are shown every 2 refreshes" within 2 s of the limit;
  45.0 frames a second were shown, all on single 22.2 ms intervals, with
  the other 45 dropped in the pacer (none queued) and decoder output to
  present 16.6 ms median. A 60 Hz limit showed 59.8 a second.
- **Frame limit period** (`TimestampPacing::LimitMeter`, fed by
  `noteLimitedDisplayLocked()` and a count of submitted frames). Under
  frame limited, each display report's interval is recorded; every 32,
  the third shortest rounded to whole refreshes (1 to 8) becomes the grid
  period, unless the stream is at least 90% that slow by its frame clock
  or by how often frames actually arrived in the window (then the stream,
  not the limit, spaces frames). A grid spaced to a limit shows frames no
  closer, so it can't see the limit go: 5 s after a limit is found, the
  grid goes back to one refresh for 8 reports and the limit is measured
  again (second shortest of the 8), then at doubling intervals up to 30 s
  while it holds, and at the next report after a gap of over 200 ms in
  reports (Quick Access, where the limit is changed, pauses them). Only a
  changed result logs. Every grid change resets the phase lock, releases
  and miss detector, and writes a `display_mode` trace row with
  `limit_refreshes`. Leaving frame limited or a new compositor refresh
  rate puts the grid back at one refresh (`resetGridLocked()`). The FIFO
  one-per-V-blank rule uses the spaced grid. Before 2026-10-09 the
  measurement used the 25th percentile and the frame clock only, with no
  re-measurement: a Deck session that began on the host's desktop
  (capture 16 in the testing notes) measured 3-5 refreshes from the
  desktop's sparse frames and held the game at 18 FPS for 30 s.
  `tst_timestamppacing` runs the meter in a loop with a modelled FIFO and
  grid; the old rules fail 8 of its checks.
- **Session refresh.** `StreamUtils::tryGetDisplayRefreshRate()` returns
  `GamescopeDisplayState::readRefreshHz()`
  (`GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK`, read over a fresh connection)
  when it is available, logging when it differs from SDL's. This feeds the
  session's refresh, the pacer's initial grid, and VRR Pacing Mode's
  mid-stream refresh check. The settings page's refresh list still comes
  from SDL's modes.
- **Overlay chips.** The graph overlay's sync chip now reads "VRR Pacing"
  for VRR Pacing Mode. Otherwise, when the timestamp pacer knows the
  display mode (Gamescope's probe or Windows' measurement), it shows that
  mode live: "V-Sync" (fixed refresh), "VRR", "Tearing" or "Frame limit".
  `PacerTelemetryCounters` carries the mode to
  `StatsGraphStreamInfo::displayMode`. With no mode known it falls back to
  "V-Sync" when V-Sync is on.

**Recommended frame rate (2026-10-09).** A stream at a VRR display's maximum
refresh leaves VRR no headroom: the display refreshes at that rate as a fixed
one, and the host's clock drifts against it. On a 120 Hz VRR TV under
Gamescope, 120 FPS (host 119.983 FPS against a 119.999 Hz ceiling) lost bursts
of 50–77 frames in 10 s about once a minute, while 116 FPS was smooth in every
Gamescope setting. Pacing those streams on the V-blank grid was tried and
reverted: replaced frames fell only from about 1.8 to 1 a second, still in
bursts, and decoder output to display rose to 13.8 ms median, because the
Gamescope margin plus the margin replaced frames widened made the release lead
about a whole refresh. So the settings page offers a dynamic "Recommended"
frame rate after Native (`StreamingPreferences::recommendedFps`, resolved in
`Session::applyNativeStreamMode()` at launch):
- `StreamingPreferences::getRecommendedFps()`: on a VRR display,
  `VrrRatePolicy::vrrRateForRefresh()`, floor(R − R²/3600) (116 at 120 Hz, 138
  at 144, 59 at 60); otherwise the refresh rate. VRR Pacing Mode counts as a
  VRR display.
- `StreamUtils::displayUsesVrr()`: under Gamescope, `GAMESCOPE_VRR_CAPABLE` and
  either Steam's VRR toggle (`GAMESCOPE_VRR_ENABLED`, which alone also reads 1
  on the Deck's panel) or `GAMESCOPE_VRR_FEEDBACK`; on macOS, an NSScreen
  refresh range (`MacDisplayTiming::supportsVariableRefresh()`). Elsewhere,
  including Windows, it can't tell (`StreamUtils::canDetectVrr()` is false).
- Where it can't tell, the same choice is named "VRR (N FPS)" and always uses
  the VRR rate, so the user decides. Guessing would be worse either way: the
  VRR rate on a display that actually refreshes at a fixed rate (116 FPS at
  120 Hz) repeats a frame every quarter second. Windows' only signals are
  per-vendor (NVAPI's VRR state), the EDID (capability, not whether it is
  on), or a measurement (a short flip-model window presenting below the
  refresh rate), none built. When VRR Pacing Mode also lists that rate, its
  duplicate is hidden.
- `SystemProperties::getStreamDisplayMode()` adds `vrr`. A note under the
  frame rate menu appears while the selected rate is above Recommended's on
  such a display.
- `StreamUtils::getDisplayOutputMode()` now takes Gamescope's output refresh
  rate over SDL's, which reads the game Xwayland's possibly stale mode, as
  `tryGetDisplayRefreshRate()` already did. This also affects Native.

**Windows VRR detection (2026-10-08).** On a display refreshing as frames
arrive, snapping targets to a V-blank grid only adds jitter and delay. Windows
has no reliable query for whether VRR is engaged on a window: Moonlight's VRR
qualification checks capability, and only NVAPI or ADL could tell, per vendor.
So it is measured instead.
- `TimestampPacing::RefreshClassifier` counts V-sync wakeups over whole
  1-second windows against the display mode's nominal rate.
  - A fixed-rate display has a V-blank every refresh whether or not anything
    was presented. An adaptive one has them only for frames, including LFC
    repeats.
  - Below 85% of the nominal rate is adaptive; above 95% is fixed; in between,
    the previous result holds.
  - A wakeup gap over 100 ms restarts the window.
  - Unit-tested: fixed with lost wakeups, 90 FPS on 240 Hz, LFC, recovery, and
    the dead band.
- `Pacer::initialize()` enables it with `TimestampPacer::measureRefreshMode()`
  on Windows (V-Sync on, `DxVsyncSource`).
  - It is not used for renderers with `RENDERER_ATTRIBUTE_FORCE_PACING`
    (exclusive fullscreen): their presents flip at once, so a wrong verdict
    would tear.
  - It is not used when a compositor probe (Gamescope) exists.
- The pacing thread maps the result onto the same `DisplayMode` the Gamescope
  probe uses, every 250 ms:
  - Fixed: V-blank grid.
  - Adaptive: frames presented at target minus render lead.
  - Unknown: grid, as before.
- Switching between Unknown and fixed keeps the grid; any other switch resets
  it. Each switch is logged with the measured share.
- The overlay shows "Display: VRR (measured)" or "Display: fixed refresh
  (measured)", where Gamescope shows "Gamescope: …".
  (`VIDEO_STATS::timestampDisplayModeMeasured`.)
- It depends on V-blank virtualization being disabled, which `main.cpp` does
  on Windows 11 22H2 and later. Without that, DWM's virtual V-blanks look
  fixed, and the grid stays as before.
- Wayland is not measured: `WaylandVsyncSource` commits the surface on every
  frame callback, so its wakeups follow compositor repaints, which those
  commits may themselves keep at the maximum rate.
- When the stream runs at the display's maximum rate, both kinds of display
  refresh at that rate, so the result stays fixed; the grid is then harmless.
- This may also be what the `mailbox-test` branch saw as "refreshing on
  demand" with VRR off. If enough refreshes go missing, the display is now
  treated as adaptive.

**Windows missed V-blank detection (2026-10-08).** On Windows the grid comes
from `DxVsyncSource`, so display reports are used only to detect misses.
- With V-Sync, a V-sync source and a renderer whose
  `supportsFrameDisplayedEvents()` is true, `Pacer::initialize()` sets an
  `IFFmpegRenderer::FrameDisplayedSink`. That is D3D11 with a swapchain and
  no composition presenter.
- `D3D11VARenderer::renderFrame()` stamps each fixed-path present's start
  time and records its `GetLastPresentCount()`. Right after the present, it
  reads `GetFrameStatistics()`.
- A newly displayed `PresentCount` that is one of the last eight presents is
  reported as (its present start, display time, refresh period).
  - The display time is `SyncQPCTime` (translated with the VRR path's QPC
    correlation) minus `SyncRefreshCount - PresentRefreshCount` refreshes.
  - The refresh period is averaged from successive statistics samples.
- `TimestampPacer::onFrameDisplayed()` matches the report to the last release
  before that present began. It then applies the same miss rule and margin
  as Gamescope.
- Teardown logs how many frames were reported and how often statistics were
  unavailable. Untested on hardware: whether DWM's composed (non-independent)
  presentation reports usable statistics for windowed flip-model swapchains
  is not yet confirmed.

**Telemetry.** The text overlay shows a "Timestamp pacing" line: buffer, share
of paced frames left out of sizing it, share of paced frames ready after their
target, superseded frames, frames shown on
arrival, and the V-blank grid's trim. The graphs card shows a "Timestamp
pacing" chip. The pacing log (below) gains `scheduled` rows with target,
buffer, lateness and repeat flag, plus two new drop reasons.

Six opt-in stats graphs (bits 19–24) need timestamp pacing to be active. The
settings page hides them while it is off, and the session drops them
otherwise. "V-blank wait" is also dropped when there is no V-sync source.
- **Pacing buffer vs lateness:** per-frame lateness against the buffer's
  baseline (band), with the current buffer as a line. Lateness above the line
  is a late frame.
- **Presented smoothness:** change in presented interval from the previous
  one (band; chain broken by gaps over 50 ms). A second line shows the same
  measure on the host's raw timestamps for consecutive frames.
- **Pacing schedule error:** present return minus the planned release plus
  the learned release-to-present time. Signed; positive is late.
- **Smoothing shift:** smoothed minus raw source time (signed), with
  re-anchors totalled over the window in the label.
- **V-blank wait:** assigned V-blank minus target (signed), with the phase
  trim as a line.
- **Release to present:** release to `renderFrame()` return, with the learned
  lead as a line.

The pacer telemetry accumulates each as a signed `PacerAccumulator` per
100 ms sample. Buffer, trim and lead are state, carried across samples.
Signed graphs use a symmetric axis centred on the midpoint gridline
(`GraphSpec::symmetric`). A label-only window total uses
`GraphSpec::secondaryWindowSum`. Presented smoothness is computed for every
pacing mode but shown only with timestamp pacing.

**Evidence and limits.**
- `tst_timestamppacing` covers stamp-noise smoothing, buffer growth, release
  and bounds, repeats, rate changes, recovery from a slowdown, RTP wrap and
  restart, the V-blank grid and phase-lock behaviour.
- Replaying the 2026-10-07 pacing log (110 FPS, frame pacing off) through the
  C++ policy at Standard/99%:
  - 0.69% of consecutive intervals change by over 2 ms, against 30.4% without
    smoothing; jump p99 is 1.2 ms;
  - 8.1 ms mean delay over readiness, and 0.64% of frames ready after target.
- These are present-call times, not scanout. The V-blank path has no capture
  yet and has only been checked against synthetic grids.

**Tracing (2026-10-08, source `fc95fe55` plus this change).** Timestamp
pacing records a frame trace whenever VRR Pacing Mode would: when
`MOONLIGHT_VRR_TRACE` is set, by a launcher or by the Settings checkbox (now
"Trace paced frames for debugging" under "Pacing diagnostics").
- **File:** beside the VRR path, with `.tstrace` in place of its suffix
  (`Moonlight.vrrtrace` becomes `Moonlight.tstrace`; a `.csv` path becomes
  `<base>.tstrace.csv`, plain CSV). It is opened in `TimestampPacer::start()`
  and closed when the pacer is destroyed, after the V-sync and render threads
  have stopped, so their last rows still land.
- **Shared mechanics:** [tracefile.h](app/streaming/video/ffmpeg-renderers/pacer/tracefile.h)
  (`TraceFile`) now owns the trace file for both modes: local-path check,
  `-connection-N` archiving of an earlier connection, the `MLVRR1` chunked
  container, the one-hour minimum before the 512 MiB cap, and the footer's
  size cap, write failure and decoded SHA-256 fields. `VrrPacingWorker` was
  moved onto it with its output format unchanged: the footer is identical,
  and freshly exported cold and warm worker fixtures pass exact replay. Each mode keeps its own rows, bounded MPSC
  queue (`Vrr13::TraceQueue`) and writer thread.
- **Rows** ([timestamptrace.h](app/streaming/video/ffmpeg-renderers/pacer/timestamptrace.h),
  header first column `timestamp_trace_schema`, version 1): one flat CSV
  with an `event` column. `session` (options, rates, grid availability),
  `scheduled` (delivery timeline, host latency, decoder queue time, the
  policy's decision, the held target, render lead, queue depth), `evicted`,
  `superseded`, `released` (planned release, grid use, assigned V-blank,
  trim, release lead and extra margin, the waiter's last oversleep),
  `presented` (render start and present return, by RTP timestamp),
  `vsync`, `display_event` and `frame_displayed` (matched V-blank and miss
  verdict), and `display_mode` changes (with the measured refresh share).
  Each row has a `sequence` allocated when recorded, since rows from
  different threads can reach the file out of order.
- **Footer:** `#timestamp_trace_footer,format_version=1,clean_shutdown=1,`
  `rows_allocated,rows_enqueued,rows_dropped` plus the shared fields. A
  contended producer gives up on a slot after 16 attempts, as VRR's do, so
  `rows_dropped` can be nonzero under contention; every row is either written
  or counted.
- **Tools:** `scripts/decode-vrr-trace.py` expands it. `vrrreplay` does not
  read it; `tsreplay` (2026-10-18, `tests/vrr/tsreplay.h`) does. It replays
  the `scheduled` rows through `TimestampPacing::Policy`, which must
  reproduce every recorded decision and late flag exactly. Then it scores
  scenarios on a small fixed-grid or VRR presentation model. All nine
  2026-10-08 Gamescope captures replay exactly under the current policy
  (unchanged since `6700d95f`). Usage and model limits are in
  [tests/vrr/README.md](tests/vrr/README.md), "Timestamp pacing replay".
  Settings capture export includes `Moonlight*.tstrace`, and
  `capture-info.json` gains `timestamp_trace_files` and the session's
  timestamp pacing options. The diagnostic launchers skip exact replay and
  still upload the `.tstrace` when only it exists.
- **Tests:** `tst_timestamptrace` covers paths, concurrent producers and the
  footer accounting, connection archiving, CSV output, an unwritable
  destination, and a real `TimestampPacer` at 60 FPS on a synthetic 60 Hz
  V-blank grid (every frame scheduled and then released, superseded or
  evicted; one presented row per release; V-sync rows after `stop()`).
- **Not done:** no live stream has been traced. The temporary pacing log
  below remains, independently of this trace.

Temporary per-frame logging for the same proposal (2026-10-07,
[pacinglog.h](app/streaming/video/pacinglog.h)): with
`MOONLIGHT_PACING_LOG=1`, each decoder instance writes
`%TEMP%\moonlight-pacing-<time>.csv`. It has `decoded` rows (frame number,
RTP, frame type, size, host latency, receive/reassembly/decode timestamps) in
every pacing mode. The fixed path adds `presented`, `dropped` (with the drop
site) and `vsync` rows. Producers only append a record under a mutex; a writer
thread formats and writes every 250 ms. It is meant to be removed once the
timestamp-pacing model exists.

Overlay delivery keeps GPU work and waiting off the frame preparation and
presentation threads (2026-09-26), because the graphs republish an overlay ten
times a second. In the libplacebo renderer, the overlay worker creates or
reuses a staging texture, records the upload and a timeline-semaphore fence
under `m_CommandLock`, waits for the fence outside that lock, and then
publishes the texture. `renderMappedImage()` only swaps a published texture
in; it takes the overlay spinlock with a try-lock and keeps the previous
overlay when the worker holds it. An earlier fork change had moved uploads
onto the rendering threads to avoid libplacebo's `vk_cmd_submit()` timeline
assertion. `m_CommandLock` (upstream 68bdaea1) now fixes that race directly.
The D3D11 renderer already created overlay resources on the worker. Its
`renderOverlay()` now also uses a try-lock and draws the resources it drew on
the previous frame when the lock is busy. In both renderers an overlay can
appear a frame late, but a frame is never delayed by an overlay. Neither
change has been measured on a live stream.
