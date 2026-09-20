#pragma once

#include "ivrrframepresenter.h"
#include "renderer.h"

#ifdef Q_OS_WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include <atomic>

#ifdef Q_OS_LINUX
#include "vulkantiming.h"
#endif

#ifdef HAS_WAYLAND
#include "waylandfeedback/wayland.h"
#ifdef Q_OS_LINUX
#include "gamescoperepaint.h"
#endif
#endif

#ifdef Q_OS_DARWIN
class MetalVulkanTextureFactory {
public:
    MetalVulkanTextureFactory(pl_vulkan vulkan);
    ~MetalVulkanTextureFactory();

    bool mapVideoToolboxToPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    void unmapVideoToolboxFromPlacebo(pl_frame* mappedFrame);

private:
    pl_vulkan m_Vulkan;
    /* CVMetalTextureCacheRef */ void* m_TextureCache = nullptr;
};

// Work around direct-to-display mode sometimes (but not always!) blocking us
// from getting a new drawable while the current one is getting scanned out.
#define PLVK_USE_DYNAMIC_SWAPCHAIN_DEPTH 1

// MoltenVK will block for the next drawable when we render the next frame
// rather than inside pl_swapchain_start_frame(), so we will force it to wait
// by rendering some no-op work right after we get the new swapchain frame.
#define PLVK_USE_EARLY_RENDER_TO_WAIT 1

#endif

class PlVkRenderer : public IFFmpegRenderer, public IVrrFramePresenter {
public:
    QString getCalibrationIdentity() override;
    PlVkRenderer(AVHWDeviceType hwDeviceType = AV_HWDEVICE_TYPE_NONE, IFFmpegRenderer *backendRenderer = nullptr);
    virtual ~PlVkRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual IVrrFramePresenter* getVrrFramePresenter() override;
    virtual VrrFallbackReason checkSupport() const override;
    virtual bool canLatchAdaptivePresent() const override;
    virtual uint64_t waitForDecode(AVFrame* frame) override;
    virtual VrrPrepareResult prepareFrame(AVFrame* frame,
                                          uint64_t decodeBoundary) override;
    virtual VrrPrepareResult prepareFrame(AVFrame* frame,
                                          uint64_t decodeBoundary,
                                          const VrrPresentRequest& request) override;
    virtual VrrPresentFeedback presentAdaptive(
        const VrrPresentRequest& request) override;
    virtual VrrPresentFeedback cancelFrame() override;
    virtual void setSuspended(bool suspended) override;
    virtual bool restoreFixedPresentation(VrrFallbackReason reason) override;
    virtual bool testRenderFrame(AVFrame* frame) override;
    virtual void waitToRender() override;
    virtual void cleanupRenderContext() override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO) override;
    virtual int getRendererAttributes() override;
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override;
    virtual int getDecoderCapabilities() override;
    virtual bool isPixelFormatSupported(int videoFormat, enum AVPixelFormat pixelFormat) override;
    virtual AVPixelFormat getPreferredPixelFormat(int videoFormat) override;

private:
    static void lockQueue(AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index);
    static void unlockQueue(AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index);
    void uploadPendingOverlays();

    void beginRenderTiming();
    void endRenderTiming();
    void selectPresentationMode(PDECODER_PARAMETERS params);
    void selectLegacyPresentMode(PDECODER_PARAMETERS params);
    bool acquirePendingSwapchainFrame(const char* earlyRenderFailureMessage);
    bool acquireVrrSwapchainFrame();
    bool submitPendingSwapchainFrame();
    void finishVrrRenderTiming();
    bool cancelVrrFrame();
    bool waitForVrrGpuReady(VrrPresentFeedback& feedback);
    void queueRenderDeviceReset();

    bool createSwapchain(int depth);
    // Both must run on the render thread. pl_gpu is not thread-safe.
    bool createOverlay(pl_overlay* overlay, SDL_Surface* surface);
    bool mapAvFrameToPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    void unmapAvFrameFromPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    bool populateQueues(int videoFormat);
    bool chooseVulkanDevice(PDECODER_PARAMETERS params, bool hdrOutputRequired);
    bool tryInitializeDevice(VkPhysicalDevice device, VkPhysicalDeviceProperties* deviceProps,
                             PDECODER_PARAMETERS decoderParams, bool hdrOutputRequired);
    bool isExtensionSupportedByPhysicalDevice(VkPhysicalDevice device, const char* extensionName);
    bool isPresentModeSupportedByPhysicalDevice(VkPhysicalDevice device, VkPresentModeKHR presentMode);
    bool isColorSpaceSupportedByPhysicalDevice(VkPhysicalDevice device, VkColorSpaceKHR colorSpace);
    bool isSurfacePresentationSupportedByPhysicalDevice(VkPhysicalDevice device);

    // The backend renderer if we're frontend-only
    IFFmpegRenderer* m_Backend;
    AVHWDeviceType m_HwDeviceType;

#ifdef Q_OS_DARWIN
    std::unique_ptr<MetalVulkanTextureFactory> m_MetalTextureFactory;
#endif

#ifdef PLVK_USE_DYNAMIC_SWAPCHAIN_DEPTH
    int m_DelayedPresents = 0;
    Uint32 m_RenderStartTime = 0;
#endif

    // SDL state
    SDL_Window* m_Window = nullptr;

    // Stream state
    int m_MaxVideoFps = 0;

    // The libplacebo rendering state
    pl_log m_Log = nullptr;
    pl_vk_inst m_PlVkInstance = nullptr;
    VkSurfaceKHR m_VkSurface = VK_NULL_HANDLE;
    int m_SwapchainDepth = 0;
    VkPresentModeKHR m_VkPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR m_VrrAdaptivePresentMode = VK_PRESENT_MODE_FIFO_KHR;
    pl_vulkan m_Vulkan = nullptr;
    pl_swapchain m_Swapchain = nullptr;
    pl_renderer m_Renderer = nullptr;
    pl_tex m_Textures[PL_MAX_PLANES] = {};
    pl_color_space m_LastColorspace = {};

    // Render parameters for this session. This is pl_render_fast_params unless
    // output dithering is enabled, in which case m_DitherParams is attached.
    // libplacebo dithers to the target's own bit depth as the final step of its
    // pipeline, so unlike the D3D11 shaders this needs no display query and is
    // correct for HDR targets too.
    pl_render_params m_RenderParams = pl_render_fast_params;
    pl_dither_params m_DitherParams = {};
    pl_deband_params m_DebandParams = {};

#ifdef PLVK_USE_EARLY_RENDER_TO_WAIT
    pl_overlay m_EmptyOverlay = {};
    pl_overlay_part m_EmptyOverlayPart = {};
#endif

    // Pending swapchain state shared between the legacy wait/render path and
    // the VRR preparation/presentation path. A successfully started frame
    // must always be submitted before it is resized, destroyed, or replaced.
    pl_swapchain_frame m_SwapchainFrame = {};
    bool m_HasPendingSwapchainFrame = false;

    // VRR presentation state. The pacing worker is the only thread that
    // touches the non-atomic fields after initialization. Window callbacks on
    // the main thread only mark the atomic resize flag; the worker then safely
    // abandons a prepared image before resizing the swapchain.
    bool m_VrrRequested = false;
    bool m_VrrSuspended = false;
    VrrFallbackReason m_VrrFallbackReason = VrrFallbackReason::InitializationFailed;
    std::atomic<bool> m_VrrWindowChangePending { false };
    bool m_VrrPreparingFrame = false;
    bool m_VrrFramePrepared = false;
    bool m_VrrRenderSucceeded = false;
    bool m_VrrRenderTimingActive = false;
    // Readiness evidence from the current prepared frame is copied into the
    // eventual present or cancellation result. Vulkan may have to submit an
    // acquired image to abandon it, and the worker must not lose the GPU wait
    // that happened before that neutral submission.
    VrrPresentFeedback m_VrrGpuReadyFeedback;
    uint64_t m_PresentationId = 0;
    bool m_LoggedPresentationFeedback = false;
#ifdef Q_OS_LINUX
    std::unique_ptr<VulkanTiming> m_GamescopeTiming;
#endif
#ifdef HAS_WAYLAND
    std::unique_ptr<Vrr13::WaylandFeedback> m_PresentationFeedback;
#ifdef Q_OS_LINUX
    std::unique_ptr<GamescopeRepaint> m_GamescopeRepaint;
#endif
#endif

    // Overlay state
    //
    // libplacebo's pl_gpu is explicitly not thread-safe, and notifyOverlayUpdated()
    // runs on the overlay worker thread while the render thread is submitting
    // frames. Uploading overlay textures there raced the renderer's command pool
    // and tripped libplacebo's vk_cmd_submit() timeline assertion. So the update
    // thread now only hands over pixels, exactly as EGLRenderer does, and every
    // pl_gpu call for overlays happens on the render thread in
    // uploadPendingOverlays().
    SDL_SpinLock m_OverlayLock = 0;
    struct {
        // Owned exclusively by the render thread. No lock required.
        bool hasOverlay;
        pl_overlay overlay;

        // Handoff from the overlay update thread, guarded by m_OverlayLock.
        // A pending update with a null surface means "drop this overlay".
        bool hasPendingUpdate;
        SDL_Surface* pendingSurface;
    } m_Overlays[Overlay::OverlayMax] = {};

    // Device context used for hwaccel decoders
    AVBufferRef* m_HwDeviceCtx = nullptr;

    // Vulkan functions we call directly
    PFN_vkDestroySurfaceKHR fn_vkDestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 fn_vkGetPhysicalDeviceQueueFamilyProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fn_vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fn_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkEnumeratePhysicalDevices fn_vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties fn_vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR fn_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties fn_vkEnumerateDeviceExtensionProperties = nullptr;
};
