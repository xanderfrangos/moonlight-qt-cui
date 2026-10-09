# Gamescope and Timestamp Pacing: testing notes

Findings from testing Moonlight's Timestamp Pacing under Gamescope (SteamOS
Game Mode), 2026-10-08 to 2026-10-09. These notes keep the evidence behind
each change in one place. [architecture.md](../architecture.md) describes the
resulting design ("Gamescope display probe" and the sections after it).

Unless a section says otherwise, a figure is from one capture, and a fix is
not proven on hardware until a later capture shows it.

## Setup

| Device | GPU | Display | Notes |
|---|---|---|---|
| Steam Machine | RADV NAVI33 | LG TV over HDMI, 3840x2160, 120 Hz, VRR 24–120 Hz | Gamescope VRR in use |
| Steam Deck (internal) | RADV VANGOGH | 1280x800 panel (800x1280 native, rotated), 45–90 Hz | No VRR; Steam switches the panel mode |
| Steam Deck (docked) | RADV VANGOGH | LG HDR 4K over DP, 60 Hz, no VRR | One probe run only |

- SteamOS 20260925.101, kernel 7.2.7-valve1, Gamescope 3.16.30, Mesa 26.1/26.2.
- Gamescope command line (both devices): `--generate-drm-mode fixed
  --xwayland-count 2 ... -O *,eDP-1`. Two Xwaylands: Steam's on `:0`, games
  (Moonlight) on `:1`.
- Moonlight: AppImage built in the WSL Ubuntu 22.04 chroot (with
  `disable-wayland` and `disable-libdrm`), Vulkan renderer (libplacebo) with
  VAAPI decode. Streams were 2560x1600 HEVC Main 10 at 90 FPS on the Deck,
  and 2560x1440 at 116 or 120 FPS on the Steam Machine.
- Captures: Settings' diagnostic capture (`MOONLIGHT_VRR_TRACE` with deep
  trace). Each produced `Moonlight.log`, `Moonlight.tstrace` (expanded with
  `scripts/decode-vrr-trace.py`), a GPU diagnostics CSV,
  `capture-info.json`, and, from the probe builds on,
  `Moonlight.display-stream.txt`.

## How to read the evidence

- **Trace rows** (`*.tstrace` → CSV): `scheduled` (decoder hands a frame to
  the pacer), `released` (pacer hands it to the renderer), `presented`
  (present returned), `display_event` (Gamescope's display time),
  `superseded`, `evicted` and `display_mode`.
- **Useful derived measures:**
  - shown frames per second: the intervals between successive
    `display_event` display times;
  - repeats: intervals of 2 or more refreshes;
  - decoder output → present: `present_us` − `decoder_output_us`;
  - release → render start, and render start → present: whether a wait was
    before or inside the render call;
  - "planned V-blank had a display report": whether a released frame was
    actually shown.
- **Gamescope reports only frames it showed.** A frame missing from the
  display reports was replaced, not merely unreported (see below). The log's
  "Gamescope timing summary" line counts this: submissions against returned.
- **Gamescope's display times are its scheduled V-blank** (`ulTargetVBlank`),
  not a measured flip. They can't show tearing.
- **The GPU diagnostics CSV** had only decode-side events (`decode_sync`,
  `decoder_*`) and no render GPU timing.
- **Trace times and sampler times differ slightly.** The sampler's `[+Ns]`
  counts from the stream window opening, and the trace from the pacer
  starting. They are within about a second of each other.

## Gamescope facts established

These come from reading Gamescope's source (ValveSoftware/gamescope
`36848c2f`, 2026-10-05) and from the captures.

### Where the display state lives

- **Root window properties.** Steam's settings and Gamescope's feedback are
  root window properties of Gamescope's **first** Xwayland (server 0, `:0`
  here). The game's server (`:1`, Moonlight's `DISPLAY`) carries none of
  them; Timestamp Pacing reads `:0` (found by `GAMESCOPE_PID` and
  `GAMESCOPE_XWAYLAND_SERVER_ID`).
- **Xwayland's mode can be stale.** Each Xwayland's `wl_output` mode, which is
  what SDL and Qt read through RandR, gets its refresh rate when it is
  created or when Steam resizes it, and only server 0 follows output mode
  changes. A Deck log showed `:1` at 89.9 Hz while the panel ran at 60 Hz.
  Most other captures had both at the real rate.

| Property | Meaning | Reliable? |
|---|---|---|
| `GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK` | Current output refresh (Hz) | Yes. It followed every Deck mode switch (90/80/60/53/77 Hz), at most one 1 s sample behind KMS. Whole hertz only (119.976 Hz shows as 120). |
| `GAMESCOPE_VRR_CAPABLE` | Connector supports VRR | Yes |
| `GAMESCOPE_VRR_ENABLED` | Steam's VRR toggle (a preference) | It reads 1 on the Deck's non-VRR panel. Not an "in use" signal. |
| `GAMESCOPE_VRR_FEEDBACK` | KMS `VRR_ENABLED` actually committed | Yes. It changed with KMS `VRR_ENABLED` on every toggle, within the 1 s sampler. It already implies `VRR_ENABLED` and capability. |
| `GAMESCOPE_ALLOW_TEARING` | Steam's Allow Tearing | Yes |
| `GAMESCOPE_LIMITER_FEEDBACK` | Gamescope's frame limit is engaged (target FPS nonzero) | Yes. On a Deck it read 1 exactly while presents blocked at the refresh rate. It does not give the limit's value. |
| `GAMESCOPE_FPS_LIMIT` | The last value Steam wrote | **No.** It stayed 60 on an unlimited 120 Hz stream and 90 on the Deck, whatever the limit. Steam now sets the limit through `gamescope_control`, which resets Gamescope's internal limit without touching this property. |
| `GAMESCOPE_DYNAMIC_REFRESH` | | Stayed 90 through every Deck refresh change. Not useful. |
| `GAMESCOPE_DISPLAY_MODE_LIST_EXTERNAL`, `GAMESCOPE_DISPLAY_EDID_PATH` | External mode list, patched EDID | Informational |

- **`gamescope_control` (v2+)** sends `active_display_info`: connector,
  make, model, flags (internal/HDR/VRR) and the valid refresh rates. For
  example: Steam Machine `HDMI-A-1`, VRR, [120]; Deck `eDP-1`, no VRR, [45 …
  90].

### Presentation behaviour

- **Present modes.** The Gamescope WSI layer always gives the driver Mailbox,
  and passes the app's requested mode to Gamescope. The Vulkan surface offers
  only MAILBOX and FIFO. Gamescope treats Mailbox and Immediate as async.
- **Gamescope's own settings decide how frames reach the display:**
  - Allow Tearing: async flips;
  - VRR in use: a flip when the frame arrives;
  - otherwise: the newest commit at each refresh;
  - with the frame limiter engaged: the layer forces FIFO (unless the app is
    frame-limiter-aware, which Moonlight isn't). FIFO overrides Allow
    Tearing: with the limit and tearing both on, presents still blocked at
    60 a second and every display interval was 16.67 ms.
- **The Deck's limit moves the panel's refresh rate.** Turning the limit on
  moved the panel to 60 Hz and off back to 90. Separately, a 45 FPS limit
  at 90 Hz showed frames every 2 refreshes.
- **The latch is about 4 ms before the V-blank on the Deck panel.** Shown
  rate by how long before the V-blank the present returned: 31% at 0–2 ms,
  60% at 2–4 ms, 91% at 4–6 ms, about 100% beyond.
- **A frame that misses the latch in Mailbox is replaced** by the next one.
  That V-blank repeats a frame, and the late frame is never shown or
  reported.

### WSI layer refresh cycle (`vkGetRefreshCycleDurationGOOGLE`)

- **It goes stale.** The layer starts every swapchain at 16.67 ms (60 Hz),
  and Gamescope sends `refresh_cycle` only when its per-surface value
  changes. A recreated swapchain on the same window keeps 60 Hz. Moonlight
  recreates its renderer after the window is shown.
- **Seen on both devices:**
  - Steam Machine probe: FIFO swapchain 120 Hz, then Mailbox on the same
    window 60 Hz. Its stream read 16.67 ms for 48 s at 120 Hz.
  - Deck: 16.67 ms for 135 s at 90 Hz, correct after the first refresh
    change.
- **When fresh, it gives the limited cycle** (25 ms at a 40 FPS limit on
  120 Hz). It can't be trusted otherwise, so Timestamp Pacing ignores it
  when the refresh-rate property is available.

### Display timing (`VK_GOOGLE_display_timing`)

- **Available on both devices without `ENABLE_GAMESCOPE_WSI`.**
- **Under VRR (Steam Machine), paced frames were shown at the pacing
  interval,** on no fixed grid: 53 FPS → 18.9 ms, 77 FPS → 13.0 ms. Only
  29–67% landed within 1 ms of the interval. Present-to-display time fell in
  two clusters 8.3 ms apart, as if commits still latch on a 120 Hz schedule.
- **With Allow Tearing, Gamescope reported display times for only 40–49 of
  90 frames a second.** While VRR was in use (captures 12 and 13), Allow
  Tearing changed nothing: reports stayed complete and present-to-display
  times matched plain VRR.
- **A 116 FPS stream on the 120 Hz VRR TV was shown at its own rate**
  (8.61–8.62 ms intervals), about 1.5 ms after present returned.
- **At 120 FPS, VRR sits at the panel's ceiling and behaves as a fixed
  120 Hz display.** The host sent 119.983 FPS against a 119.999 Hz ceiling
  (135 ppm), so the frames' phase against the refresh drifted one refresh
  every 62 s. Without the limit, present-to-display time ramped 7.6 → 1 ms
  and wrapped, with bursts of 50–77 replaced frames in 10 s at each wrap.
  With the limit (FIFO), nothing was replaced, but one frame always waited a
  refresh in the queue and the render thread waited up to 6 ms for a
  swapchain image.
- **Report gaps:** while Quick Access is open or during overlays, reports can
  stop or thin out (gaps up to 0.9 s).
- **Future-dated reports:** Gamescope reports a latched frame's scheduled
  V-blank, which may still be ahead when read. `VulkanTiming` used to
  reject these as "future" (486 in one 210 s capture, in bursts),
  creating report gaps that looked like replaced frames. "Returned"
  against "submissions" in the timing summary separates real replacement
  (583 never returned in capture 7) from rejection (3 never returned in
  capture 10).

### VAAPI decode completion

- **The decoder returns a frame before its GPU decode finishes.** About 6 ms
  for 2560x1600 HEVC 10-bit on the Deck; the render then waits for it.
- **Render time by how soon after decoder output it started** (Deck):

| Render started after decoder output | Render took |
|---|---|
| 0–2 ms | 5.8–6.2 ms |
| 2–4 ms | 3.1–3.9 ms |
| 4 ms or more | 1.2–1.6 ms |

- **This is not a present or tearing cost.** It was what made tearing look
  slower (1.9 → 6.3 ms): tearing releases frames on arrival, while the grid
  holds them about 5.5 ms.
- **Driver contention adds short sync waits.** `vaSyncSurface` also waits
  0.25–1.3 ms when the decode finished long ago. Real decode waits there
  were about 3 ms.
- **The stats line is misleading.** "Average decoding time" (about 0.7 ms)
  is only the CPU side.

### Other device observations

- **Docked Deck (VANGOGH), 4K60 DP monitor, probe:** FIFO showed a frame
  every refresh. Mailbox showed only 22–24 frames a second, with presents
  blocking 25–42 ms, with Allow Tearing on for the shortcut. That probe drew
  no video, so it isn't the decode wait. Unresolved; it needs a re-run with
  Allow Tearing off.
- **Steam Machine:** `VK_KHR_display` listed no displays while Gamescope
  held the device.

## Captures and what each changed

| # | Capture | Build | Key finding | Change made |
|---|---|---|---|---|
| 1 | Steam Machine and Deck probes | Probe | Every source said 120 Hz/VRR on the Steam Machine except the Vulkan refresh cycle of a second swapchain (60 Hz). `fps_limit=60`. | Hypotheses only |
| 2 | Steam Machine stream, VRR and tearing toggled | Probe | Pacing locked in "frame limited (FIFO)" for the whole stream from `GAMESCOPE_FPS_LIMIT=60`; never used the grid or followed VRR. Refresh cycle 16.67 ms for 48 s. | Stopped reading `GAMESCOPE_FPS_LIMIT`; grid period from `GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK` |
| 3 | Deck stream: tearing, refresh rates, frame limits | Old build | `limiter=1` matched presents blocking at the refresh rate; refresh-rate property tracked KMS mode switches; refresh cycle stale for 135 s. | Frame limited detected from `GAMESCOPE_LIMITER_FEEDBACK`, using the grid; precedence VRR > limit > tearing > fixed |
| 4 | Deck stream with limiter detection | Limit detection | Under the 60 Hz limit, all 90 frames a second were released but only 60 presented, with release → present 9.7–11.6 ms and a 15 ms queue delay: late frames never waited together, so the newest-per-V-blank rule never replaced. Tearing renders 6.3 ms. | One frame per V-blank under the limit (a taken V-blank pushes to the next) |
| 5 | Capture 4 again: tearing investigation | (analysis only) | Tearing's slower render was the VAAPI decode wait, in every mode. | Decode readiness: explicit timed `waitForDecode()` in plvk, learned decode delay, render lead without it |
| 6 | Deck, limit on and off | Decode readiness v1 | The decode delay ran away to its 20 ms cap under the limit (contention waits on late renders, then holding frames for it kept renders late); queue 2–3 deep with a "queue full" burst; decoder output → present 17–32 ms against 7–9 ms. The limit looked smoothest (99.9–100% single refreshes) only because of the extra holding. | Decode waits count only if ≥1 ms and the render began within 3 ms of the estimate; cap 1.5 frames |
| 7 | Deck, limit toggled seven times | Bounded estimator | Decode delay 4.8–6.4 ms; queue 0–1; decoder output → present 8–12 ms in both modes; pacing drops 0.05%. One 30 s fixed-refresh stretch lost about a fifth of its frames, replaced by Gamescope (about 580 never reported), with presents 5 ms before the V-blank and no miss detected. | Unreported earlier releases count as replaced (missed); 3 in a second widen the margin |
| 8 | Deck, limit toggled | Replaced detection | A 12 s stretch right after the limit went off: 25 replaced a second; margin hit 6 ms in 2 s but replacement continued 10 s. Frames ready only 5.5 ms before their V-blank (10.7 normally; trim −3.5 ms). | Per-frame "first reachable V-blank" push |
| 9 | Deck, limit toggled, 45 FPS and 60 Hz limits | Per-frame push | Gamescope replacement gone (≤18 a stretch), but the pacer dropped 180–394 frames in 4 of 7 stretches (79–85 shown a second): readiness jitter around the cut-off made moved frames collide. 45 FPS limit detected correctly. | Uniform "late shift" with hysteresis instead of per-frame pushes |
| 10 | Deck, 90 FPS limit on throughout (210 s) | Late shift | 99%+ single refreshes, pacing drops 0.15%, but the margin reached 6 ms: under FIFO, unreported frames were counted as replaced, mostly 486 reports `VulkanTiming` rejected as future-dated (Gamescope returned all but 3 of 18428). The wide margin handed frames over up to 17 ms early. Latency swings (8–21 ms) followed the playout buffer answering Wi-Fi jitter. | No replaced-frame check under FIFO; accept display times up to 40 ms in the future for Timestamp Pacing |
| 11 | Deck, 90 FPS limit on throughout (135 s) | FIFO/future fixes | Fixes confirmed: margin 0, missed 0.01%, no future reports rejected, 99.9% single refreshes. But the render lead jumped 2.1 → 5.6 ms at 100 s (render 1.5 ms): occasional 11 ms samples from waiting for a swapchain image behind FIFO, self-reinforcing through earlier release; frames waited in FIFO, about 4–5 ms more latency. | Render lead timed from max(release, render start) under Gamescope |
| 12 | Steam Machine, 116 FPS, VRR in use throughout: limit, VRR, tearing, VRR (193 s) | Render lead from render start | Smooth in every setting: 99.5–100% of frames shown, display interval change p95 0.35–0.37 ms, decoder output → display 4.5–4.9 ms median. Decode delay 1.8 ms. Spikes only at stream start and when Quick Access opened. The VRR-with-limit warning was logged 4 times a second. | Warning logged once per change |
| 13 | Steam Machine, 120 FPS, VRR in use throughout: tearing + limit, tearing, limit, VRR (249 s) | Render lead from render start | VRR at the panel's ceiling: a 62 s beat between the host's and the display's clocks. Without the limit, latency drifted 4 → 11 ms with bursts of replaced frames at each wrap (1.3–2.7% overall); with it, none replaced but 10–16 ms latency. | None (see open questions) |
| 14 | Steam Machine, 120 FPS: limit off (77 s), then on (73 s) | VRR at the ceiling paced on the grid; decode delay decay | On the grid, replaced frames fell only to 0.97/s, still in bursts (11–33 per 5 s); decoder output → display 13.8 ms median. The release lead (4.5 ms Gamescope margin, up to 3.5 ms more from replaced frames, 1.2 ms render) was about a whole refresh, so presents collided with the previous frame's latch, each replacement widened the margin, and the late shift was on for 69% of frames. With the limit, the sawtooth of capture 13's no-limit stretches, unlike its limit stretch. Decode delay settled at 1.5–1.9 ms. | Grid pacing at the ceiling reverted; Recommended frame rate (116 FPS at 120 Hz) offered instead |
| 15 | Deck, 90 FPS at 90 Hz: limit, no limit, limit (76 s) | Same | Limit 99.95%/99.91% single refreshes, no limit 99.63%; decoder output → display 17.5 / 14.1 ms median (host sent only about 51 FPS in the last stretch). No pacer drops, no extra margin. Decode delay decayed 5.56 → 4.72 ms against a real 4.5 ms decode, median waits 0.02 ms. | None |
| 16 | Deck, 90 FPS at 90 Hz, 90 FPS limit on throughout: a launch that began on the host's desktop (120 s), then a resume straight into the game (73 s) | Same | The launch was jittery in game until near the end. The desktop sent a frame every 50–60 ms (about 20 FPS, in bursts) and the loading screen 16 FPS, which the limit measurement read as frames every 3–5 refreshes; the host's frame clock still said 90 FPS, so the slow-stream check passed. With the grid at 5 refreshes the pacer released one frame in 5, so frames were shown 55.6 ms apart and each measurement confirmed it: the game at a steady 90 FPS was shown at 18 FPS for 30 s (about 72 frames a second dropped; 39% overall). It escaped only when the grid's fitted period drifted 3% short, a few frames landed 4 refreshes apart, and the reconfigured grid let frames through at full rate for a moment. The resume never saw a slow stream: one refresh throughout, 2 replaced, 0.11% dropped. | Slow-stream check also by arrival rate; limit measured again at one refresh after 5 s (doubling to 30 s) and after report gaps; third shortest of 32 intervals instead of the 25th percentile |
| 17 | Deck, 90 Hz, 90 FPS limit on throughout, a launch from the host's desktop; the game itself then ran at 90, 45, 30 and 60 FPS (7 min) | Capture 16's fix | Fix confirmed: the grid stayed at one refresh throughout and the stream spaced itself (45 FPS on 2-refresh intervals, 30 on 3, 60 alternating 1 and 2); pacer drops 0.18%. But at 60 FPS, from 315 s to 340 s, 16-22% of frames were shown a refresh late: planned for a V-blank their decode finished only 2.6 ms before (decode about 4.8 ms after output), they waited 4.8 ms for it and presented 1.4 ms before the V-blank. 280 of 282 misses in the capture were such frames. They took the margin to 6 ms, where it stayed; no visible repeats (intervals stayed 1/2), latency 14-17 ms. | None yet (see capture 18) |
| 18 | Deck, 90 FPS limit, 90 FPS at 90 Hz, then the game at 60 FPS, then Steam's 60 FPS limit (the panel moved to 60 Hz), a brief 59 Hz (6 min) | Same | 60 FPS at 60 Hz: every interval one refresh, no misses, no drops. 60 FPS at 90 Hz again missed in stretches (177 of 600 frames in 10 s), 331 of 342 misses decode-bound, margin to 6 ms; it then cost about 2.7 ms at 60 Hz (latency 14.8 → 12.1 ms as it decayed over two minutes). 7 more misses came in the 0.1 s between the panel switching to 60 Hz and the pacer's poll seeing it. At 59.013 Hz with 60 FPS, about one frame a second dropped and latency rose to about 23 ms, as a rate mismatch must. | A miss whose render waited at least a margin step (0.5 ms) for its decode no longer widens the margin; a new refresh rate resets it |

### Smoothness and latency by mode (Deck, 90 FPS)

| Mode | Single-refresh intervals | Decoder output → present (median) | Capture |
|---|---|---|---|
| Fixed refresh, 90 Hz | 97.4–99.6% (normal stretches) | 7–10 ms | 6, 7 |
| Frame limit, 90 Hz (estimator running away) | 99.9–100% | 17–32 ms | 6 |
| Frame limit, 90 Hz (bounded estimator) | 99.4–100% | 8–12 ms | 7 |
| Frame limit, 60 Hz | 99.9% | 14.5 ms | 9 |
| 45 FPS limit at 90 Hz | 100% (on 22.2 ms) | 16.6 ms | 9 |

The frame limit's early smoothness edge was extra buffering, not FIFO itself.
With the bounded estimator, both modes are about equally smooth at similar
latency. For a 90 FPS stream on the Deck's 90 Hz panel, fixed refresh avoids
FIFO's queuing at no cost. The limit is useful to hold a steady lower rate.

### Steam Machine, 120 Hz VRR TV (VRR in use throughout)

| Stream | Gamescope setting | Frames shown | Decoder output → display | Capture |
|---|---|---|---|---|
| 116 FPS | Any (limit, tearing, neither) | 99.5–100% | 4.5–4.9 ms median, p95 4.9–9.9 ms | 12 |
| 120 FPS | Tearing and/or VRR, no limit | 97.3–98.7% | drifts 4 → 11 ms over 62 s | 13 |
| 120 FPS | Limit | 100% | drifts 16 → 10 ms | 13 |
| 120 FPS | No limit, paced on the grid (reverted) | 0.97 replaced/s, in bursts | 13.8 ms median | 14 |

Streaming a few FPS below the VRR ceiling keeps VRR following the stream;
the frame rate menu's Recommended option picks it (R − R²/3600).
At the ceiling, the limit trades the stutter bursts for about a refresh of
extra latency.

## Bugs found in Moonlight along the way

1. **Wrong Xwayland.** The pacer's probe read `DISPLAY` (`:1`), where every
   property is zero, so it always saw fixed refresh. Fixed before these notes
   started: it reads server 0.
2. **`GAMESCOPE_FPS_LIMIT` precedence.** It locked streams into frame-limited
   mode (capture 2).
3. **Stale WSI refresh cycle used as the grid period** (captures 1–3).
4. **Session refresh from SDL.** It could be the game Xwayland's stale mode.
   `tryGetDisplayRefreshRate()` now prefers Gamescope's refresh-rate
   property.
5. **Frame-limited releases queued in FIFO** (capture 4).
6. **Decode wait learned into the render lead.** It made 88% of frames
   "late", so tearing and VRR did no smoothing (capture 5).
7. **Decode-delay feedback loop** (capture 6).
8. **Replaced frames invisible to miss detection** (capture 7).
9. **Per-frame V-blank push collisions** (capture 9; introduced in response
   to capture 8).
10. **Replaced-frame detection under FIFO and rejected future-dated reports**
    (capture 10). Together they widened the margin to 6 ms for no reason.
11. **Render lead learned from FIFO back-pressure** (capture 11). Waiting for
    a swapchain image before rendering counted as rendering, which fed
    itself through earlier releases.
12. **VRR-with-limit warning logged 4 times a second** (captures 12 and 13).
    The pacer copies the probe for each poll, which reset the probe's own
    "warned" flag. Its state is now shared between copies.
13. **The frame limit's period confirmed itself** (capture 16). A grid
    spaced to a measured limit shows frames no closer, so the measurement
    can't see that the limit was wrong or has gone. A slow desktop got it
    there: the slow-stream check used the host's frame clock, which stays
    at the stream rate while the host skips frames. Raising the limit in
    Quick Access had the same blind spot.
14. **Decode-bound misses widened the margin** (captures 17 and 18). A
    frame planned for a V-blank its decode couldn't make missed however
    early it was handed over, yet each miss widened the margin, which only
    added latency. A refresh-rate switch, seen up to a poll late, also
    widened it.

## Current behaviour under Gamescope (as built after capture 18)

- **Mode,** polled every 250 ms from server 0:
  1. VRR in use: no grid; frames go out at their target minus the render
     lead.
  2. Limiter engaged: grid, one frame per V-blank, sub-refresh limits
     measured from display intervals.
  3. Allow Tearing: like VRR.
  4. Otherwise: fixed refresh, on the grid.
- **Grid period:** Gamescope's output refresh rate. While frame-limited, a
  multiple of it once display intervals show frames every N refreshes and
  the stream (by its frame clock and by how often frames arrive) is faster
  than that. A limit above one refresh is measured again at one refresh 5 s
  later, then every 10, 20 and 30 s while it holds, and after any gap in
  reports over 200 ms.
- **Readiness:** decoder output plus the learned decode delay. The render
  lead excludes the decode wait, and under Gamescope it is timed from when
  rendering begins. The delay also decays 2% a second while renders it
  holds mostly don't wait (1 s median under 0.15 ms), so an estimate that
  ends up too high comes back down.
- **Missed V-blanks:** lag on reported frames, plus replaced frames
  (unreported earlier releases, not under the frame limit); a lagging frame
  or 3 replaced in a second widen the margin by 0.5 ms, up to 6 ms, except
  frames whose render waited at least 0.5 ms for its decode (handing those
  over earlier wouldn't have helped). A new refresh rate resets the margin.
  Display times up to 40 ms in the future are accepted.
- **Late shift:** when half of the last 64 grid frames couldn't be ready
  their lead before their V-blank, every frame is planned a refresh later
  until at most 2 of 64 would be late. Not used under the frame limit.
- **Diagnostics:** the display probe report and stream sampler are written
  beside the trace, only while VRR tracing is on. `moonlight probe-display`
  runs the full probe, including a Vulkan presentation test, on request.
- **Overlay chips:** the live Gamescope mode appears in the graph overlay's
  sync chip ("V-Sync", "VRR", "Tearing", "Frame limit"), and VRR Pacing
  Mode is "VRR Pacing".

## Open questions and unverified changes

- **The late shift** (after capture 9) is not yet captured. The bad stretch
  in capture 7 had frames ready in time that Gamescope still missed; the
  shift would switch on there only through the widened margin. After a bad
  spell it can stay on (one refresh more latency) until the margin narrows,
  up to about two minutes.
- **Why the bad phase occurs.** Both observed bad stretches began within a
  second of turning the frame limit off, but not every switch did it. One
  faded by itself after about 15 s; the other was still worsening after 30 s.
  Possibly the alignment between frame arrival and the refresh: a 90 FPS
  stream on a 90.004 Hz panel drifts about one refresh every four minutes.
  This is an inference.
- **A limit below the stream's rate with VRR** queues frames; Gamescope
  doesn't report the limited cadence. A warning is logged once each time
  the limit and VRR come on together.
- **Decode delay decay** (after capture 13). Chosen by replaying the decode
  waits of captures 6–13: with the decode finished, 1 s median waits never
  exceeded 0.047 ms; renders begun x ms early had medians of about x. In
  captures 14 and 15 it settled without overshooting (see the table).
- **Streams at the VRR ceiling** (captures 13 and 14). Pacing them on the
  grid was tried and reverted: it cut replaced frames only by about 40%
  and added about 6 ms of latency. Streaming a few FPS below the refresh
  rate is the fix; the frame rate menu now recommends it on VRR displays.
- **Tearing:** Gamescope's display reports can't show tearing, and are
  missing for about half of the frames.
- **Docked Deck Mailbox at 4K** (22–24 FPS) is unexplained.
- **Under VRR, only 29–67% of paced frames landed within 1 ms of their
  interval** (Steam Machine probe, at 53 and 77 FPS). At 116 FPS (capture 12)
  display interval changes stayed under 0.4 ms at p95, so this may be
  specific to low rates or that build. Not re-tested.
- **Windows:** the decode readiness, replaced-frame and late-shift changes
  don't apply there (no decode wait reported, no compositor probe). Windows
  captures would still be needed to confirm nothing changed.
- **Limiter detection timing:** `GAMESCOPE_LIMITER_FEEDBACK` was trusted
  from one Deck capture and the stream logs after it.
- **Unsteady frame rates without VRR** (after capture 18). At 55-65 FPS on
  90 Hz the frames' phase against the refresh keeps moving, so some are
  always planned for a V-blank their decode can't make; in the modelled
  test about 18% miss. Under the frame limit each is shown a refresh late
  (no drop); without it, a late frame is usually replaced by the next, and
  the late shift (half of 64 late) wouldn't engage at that rate. Not
  captured.
- **Re-measuring a frame limit** (after capture 16) is tested only against a
  model of FIFO. Under a real limit, each re-measurement sends a frame every
  refresh for 8 reports (about 0.27 s at 30 FPS), which may queue a frame
  or two in FIFO, as at the start of a limited session. A capture with a
  30 or 45 FPS limit should check latency around the re-measurements, and
  one that raises the limit in Quick Access should show it followed within
  a second.
