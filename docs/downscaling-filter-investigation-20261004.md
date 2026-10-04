# Downscaling filter investigation

Implementation follow-up: this worktree now adds Bilinear, Bicubic, Mitchell,
and Lanczos choices for libplacebo and D3D11, with Bilinear preserving the old
default. See the 2026-10-04 entry in [architecture.md](../architecture.md) for
the implemented pipeline and validation. The investigation below records the
pre-change source and options considered.

Investigated 2026-10-04 against source commit `d8fa2d3d`. This is a source and
API review, not a measurement of the installed Steam Deck build. No application
behavior, settings, or deployment changed.

## Findings

There is no video downscaling filter preference in this fork. Ordinary Vulkan,
EGL, native Metal, and D3D11 rendering use basic texture sampling. Other paths
delegate scaling to SDL, a driver, or the OS; their output is not necessarily
bilinear. A single libplacebo change can support selectable filters on Linux,
Windows Vulkan, and macOS Vulkan/MoltenVK. Native renderers require separate work.

For the Deck, compare Mitchell and Lanczos against the existing bilinear path.
Mitchell is a reasonable candidate for a balanced preset; Lanczos is a candidate
for sharper detail. Neither is established as the best choice without testing
moving game content and GPU cost on the Deck.

## The example resolution matters

`2560x1600 -> 1280x800` is exactly half size on each axis. Correctly aligned
bilinear sampling can already average each 2x2 source block in this special case.
It is a useful baseline, though it does not provide a strong general-purpose
filter for fine moving patterns.

The stated `2650x1600` has a different aspect ratio. With aspect preservation,
its image occupies about `1280x773` pixels, with narrow bars above and below;
the shared rectangle helper rounds height up. It is approximately a 2.07:1
reduction, so the exact-half-size shortcut no longer applies. Compare both
resolutions rather than silently treating 2650 as 2560.

For arbitrary reductions, filters must expand their source sampling footprint
with the reduction ratio. A fixed-size bicubic or Lanczos shader can still
produce jagged edges, moire, and shimmer. libplacebo supplies this expansion;
keep `skip_anti_aliasing=false`. Its documentation explicitly calls out the
exact-half bilinear exception. [libplacebo quality options](https://libplacebo.org/options/#skip_anti_aliasingyesno)

## Filter choices

These are expected tradeoffs, not measured performance rankings on the Deck.

| Choice | Expected behavior | Suggested role |
|---|---|---|
| Basic hardware bilinear | Very cheap; limited source footprint; exact-half aligned reduction is a special favorable case | Preserve current baseline |
| Filtered bilinear / triangle | Wider, ratio-aware averaging; softer than sharper reconstruction filters | Distinguish from basic bilinear if exposed |
| Box / area averaging | Averages source coverage without edge halos; less selective removal of fine patterns | Simple reduction reference; useful at integer ratios |
| Hermite | Small filter footprint and no negative weights; fast quality-oriented averaging | Low-cost quality candidate; libplacebo's normal downscaler |
| Bicubic B-spline | Smooth, soft cubic filter; not necessarily sharper than bilinear | Explicitly label the cubic variant |
| Bicubic Mitchell | Balances softness, detail, and edge artifacts | First balanced candidate |
| Bicubic Catmull-Rom | Sharper cubic filter; more risk of edge halos | Optional sharp cubic candidate |
| Lanczos 2 / 3 | Stronger detail retention and frequency filtering; possible halos; larger footprints cost more work | Sharp quality candidates |
| Spline36 | Another reconstruction filter worth comparing after the main candidates | Advanced option |
| Gaussian | Smooth, without negative-lobe ringing; trades away fine detail | Soft/stable option rather than primary default |
| EWA Lanczos / Jinc | Two-dimensional filter, heavier work; commonly emphasized for enlargement | Secondary experiment, not the initial Deck default |
| Nearest | Discards source information; jagged edges and unstable fine detail | Pixel-art/debug option, not general game downscaling |

The bundled header exposes these libplacebo filters. Current upstream defines
`bicubic` as B-spline (`B=1, C=0`), Mitchell as `B=C=1/3`, and Catmull-Rom as
`B=0, C=1/2`. Generic "Bicubic" is therefore an ambiguous UI label.
[Filter definitions](https://github.com/haasn/libplacebo/blob/master/src/filters.c),
[mpv's filter descriptions](https://mpv.io/manual/master/#options-scale)

Do not promise a universal "Lanczos + anti-ringing" fix. Current libplacebo
disables neighborhood anti-ringing for separable downscaling because clamping
can reintroduce aliasing. Choose a milder filter if halos are objectionable;
polar filters have different behavior. [Sampling implementation](https://github.com/haasn/libplacebo/blob/master/src/shaders/sampling.c)

## Renderer and operating-system coverage

| Renderer / systems | Current implementation | Practical route to control |
|---|---|---|
| libplacebo Vulkan: Linux, Windows; macOS through MoltenVK | `plvk.cpp:673` starts with `pl_render_fast_params`; no downscaler is assigned. The bundled API documents NULL as basic bilinear/nearest sampling without ratio-aware anti-aliasing | Assign `pl_render_params.downscaler`; existing library supports the main filters. Smallest shared implementation |
| Native D3D11: Windows x64/ARM64 | `d3d11va.cpp:4477` uses a linear sampler; decoded textures have no prefiltered mip chain | Add ratio-aware HLSL filtering. A separable horizontal/vertical pass is a practical quality implementation; integrate with color conversion, dithering, and HDR |
| DXVA2 / D3D9: Windows compatibility fallback | `dxva2.cpp:989` uses linear `StretchRect`; preferred `VideoProcessBlt` uses device-specific scaling | Query processor capabilities if offering a hardware quality preset. Named Mitchell/Lanczos requires custom rendering or selecting a compatible newer renderer |
| EGL / OpenGL ES: Linux and eligible Unix/embedded builds | `eglvid.cpp:628` selects `GL_LINEAR`; EGL fragment shaders sample external textures | Add GLSL filtering with source dimensions and actual destination dimensions. External/opaque texture and multi-plane paths need separate handling; filter quality cannot be obtained by changing one sampler enum |
| Native Metal: macOS | `vt_renderer.metal:18` declares a linear sampler for YUV and RGB shaders | Custom Metal filters for consistent cross-platform kernels, or evaluate Apple's `MPSImageLanczosScale` for a macOS-specific Lanczos path |
| AVSampleBufferDisplayLayer: macOS | `vt_avsamplelayer.mm:358` sets aspect-preserving video gravity; scaling is delegated to Apple's display layer | No named video-filter selector is used here. Route explicit quality requests through Metal or libplacebo |
| SDL renderer: desktop software/generic/CUDA fallback | `sdlvid.cpp:409,575` creates a video texture and calls `SDL_RenderCopy`; no video scale mode is set | Nearest/linear can be selected explicitly. Native SDL2 defaults to nearest unless a hint overrides it; SDL2 compatibility runtimes can differ. "Best" is currently the same as linear in SDL2. Higher filters need another rendering path or explicit CPU scaling |
| DRM/KMS direct display: Linux/embedded | `drm.cpp:1935` supplies source/destination rectangles to a display plane; no scaling-filter property is set | Device-dependent plane scaling. Optional `SCALING_FILTER` exposes driver default/nearest, not portable Mitchell/Lanczos. For uniform quality filters, render through Vulkan/EGL first |
| VAAPI direct X11 display: Linux/eligible Unix builds | `vaapi.cpp:879` calls `vaPutSurface` with color-space flags and default scaling | libva has fast/HQ and interpolation flags, subject to driver support. These are not portable named kernels. When VAAPI exports frames to Vulkan/EGL, that frontend controls sampling instead |
| VDPAU direct X11 display: Linux/eligible Unix builds | `vdpau.cpp:574` creates its mixer with zero optional features; mixer performs scaling | Query and enable optional HQ scaling levels 1-9. Levels are driver-specific, not guaranteed bicubic/Lanczos equivalents |
| MMAL: legacy Raspberry Pi Linux | `mmal.cpp:118` supplies a display-region destination rectangle | Firmware/hardware scaler; no named filter choice here. A compatible EGL frontend is the route to custom filtering |
| SLVideo: Steam Link embedded build | `slvid.cpp:221` sets overlay display area | Platform display scaler; no named filter control in this implementation. Requires platform API investigation or a different rendering path |

macOS is particularly relevant: **this fork prefers libplacebo/MoltenVK for
VideoToolbox when renderer selection is Auto**, when compiled with libplacebo
(`ffmpeg.cpp:1829`). It also links libplacebo in the standard macOS prebuilt
configuration (`app.pro:164`). Native Metal is an alternative, not the only
macOS route. Builds without the dependency retain native paths.

CUDA and `GenericHwAccelRenderer` do not present directly. Their decoder names
do not establish scaling behavior: follow the selected presentation frontend.
The Windows D3D11 and Linux Vulkan PyroWave paths use these same presentation
renderers and should inherit their filter implementation. This review concerns
Moonlight Qt, not the separate Android or iOS Moonlight applications.

Platform API evidence:

- [SDL2 scale quality](https://wiki.libsdl.org/SDL2/SDL_HINT_RENDER_SCALE_QUALITY) and [SDL3 migration/default changes](https://wiki.libsdl.org/SDL3/README-migration).
- [Apple Lanczos scale](https://developer.apple.com/documentation/metalperformanceshaders/mpsimagelanczosscale).
- [Linux DRM/KMS scaling-filter property](https://docs.kernel.org/gpu/drm-kms.html).
- [libva scaling/interpolation flags](https://github.com/intel/libva/blob/master/va/va.h).
- [VDPAU mixer quality levels](https://download.nvidia.com/XFree86/vdpau/doxygen/html/group___vdp_video_mixer.html).

## Recommended implementation

1. Add a saved **Downscaling filter** preference with Current/Auto, Bilinear,
   Hermite, Bicubic (Mitchell), Bicubic (B-spline), and Lanczos initially.
   Preserve current behavior by default; decide a quality default from measurements.
2. Carry it through the existing presentation-settings snapshot and decoder
   parameters. Apply it in the shared libplacebo setup first, covering all three
   desktop operating systems where that renderer is available. Both direct and
   offscreen preparation already use `renderParamsForFrame()`.
3. Add equivalent native D3D11, EGL, and Metal implementations if native-renderer
   parity is required. Define kernels precisely so labels mean the same thing.
   Hardware-only paths should expose an honest device quality option or choose
   a compatible shader renderer, with clear fallback reporting.
4. Log the requested and active filter plus renderer, source size, and actual
   video destination size. Initially apply changes on reconnect, consistent with
   existing image-processing controls.

Keep chroma resampling separate from the main image reduction: 4:2:0 color planes
start at half the luma resolution, while 4:4:4 planes do not. libplacebo has
`plane_upscaler` / `plane_downscaler` overrides. Preserve existing chroma behavior
for a first main-filter change rather than silently altering both. Retain color
conversion, adequate intermediate precision, HDR handling, and final dithering.

FSR1 and LS1 in this fork activate for enlargement; they do not solve this
downscaling case. SteamOS/Gamescope filter settings control the compositor's own
resize. If Moonlight already submits a native-sized 1280x800 image, changing the
compositor filter cannot restore source detail lost inside Moonlight. Avoid two
consecutive resizes when assessing quality. [Gamescope's rendering role and scaling controls](https://github.com/ValveSoftware/gamescope/blob/master/README.md)

Host-side reduction is another possible pipeline: render a large game image but
capture/encode at the Deck's native size. It can reduce transmitted resolution
and client decode work, but changes which component owns filtering and needs
separate host support and measurement.

## Validation before selecting defaults

Compare identical decoded input at exact-half and noninteger reductions, at
native size, and during window resize. Include small text, diagonal edges,
fences/foliage, and slow camera movement; still screenshots alone miss shimmer.
Cover 4:2:0/4:4:4, 8/10-bit SDR, HDR, letterboxing, and renderer failures.

Measure GPU preparation cost and its tail, missed presentation deadlines,
decode/render throughput, and Deck power use at the user's stream frame rate.
These filters are spatial and need no future frames, but extra GPU work can
still increase latency or cause dropped frames. No Deck timing, battery, visual
quality, or installed-renderer result has been established by this investigation.
