#pragma once

#include <libplacebo/vulkan.h>
#include <chrono>

// An image-local upload fence. Only the overlay worker waits on it; video
// continues using the previous completed texture. No presentation prediction.
//
// Recording the fence records libplacebo commands, so signal() must run under
// the renderer's command lock. wait() is a plain Vulkan wait and must not, or
// it would hold swapchain submits hostage to the overlay upload.
class OverlayCompletion {
public:
    explicit OverlayCompletion(pl_vulkan vk) : m_Vulkan(vk) {
        const auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            vk->get_proc_addr(vk->instance, "vkGetDeviceProcAddr"));
        if (!get) return;
        const auto create = reinterpret_cast<PFN_vkCreateSemaphore>(get(vk->device, "vkCreateSemaphore"));
        m_Destroy = reinterpret_cast<PFN_vkDestroySemaphore>(get(vk->device, "vkDestroySemaphore"));
        m_Wait = reinterpret_cast<PFN_vkWaitSemaphores>(get(vk->device, "vkWaitSemaphores"));
        if (!m_Wait) m_Wait = reinterpret_cast<PFN_vkWaitSemaphores>(get(vk->device, "vkWaitSemaphoresKHR"));
        if (!create || !m_Destroy || !m_Wait) return;
        VkSemaphoreTypeCreateInfo timeline{};
        timeline.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = &timeline;
        create(vk->device, &info, nullptr, &m_Semaphore);
    }
    ~OverlayCompletion() {
        if (m_Semaphore) {
            // Renderer detachment has joined all callbacks before destruction.
            pl_gpu_finish(m_Vulkan->gpu);
            m_Destroy(m_Vulkan->device, m_Semaphore, nullptr);
        }
    }
    // Records and flushes a fence behind all pending work on texture. Returns
    // the value to pass to wait(), or zero if no fence could be recorded.
    // The caller must hold the renderer's command lock.
    uint64_t signal(pl_tex texture) {
        if (!m_Semaphore) return 0;
        const uint64_t value = ++m_Value;
        VkImageLayout layout;
        pl_vulkan_hold_params hold{};
        hold.tex = texture;
        hold.out_layout = &layout;
        hold.qf = VK_QUEUE_FAMILY_IGNORED;
        hold.semaphore = {m_Semaphore, value};
        if (!pl_vulkan_hold_ex(m_Vulkan->gpu, &hold)) return 0;
        pl_vulkan_release_params release{};
        release.tex = texture;
        release.layout = layout;
        release.qf = VK_QUEUE_FAMILY_IGNORED;
        release.semaphore = hold.semaphore;
        pl_vulkan_release_ex(m_Vulkan->gpu, &release);
        pl_gpu_flush(m_Vulkan->gpu);
        return value;
    }
    // Waits up to 100 ms for a value from signal(). Must not hold the command lock.
    bool wait(uint64_t value) {
        if (!m_Semaphore || value == 0) return false;
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &m_Semaphore;
        wait.pValues = &value;
        return m_Wait(m_Vulkan->device, &wait, 100000000) == VK_SUCCESS;
    }
private:
    pl_vulkan m_Vulkan;
    VkSemaphore m_Semaphore = VK_NULL_HANDLE;
    uint64_t m_Value = 0;
    PFN_vkDestroySemaphore m_Destroy = nullptr;
    PFN_vkWaitSemaphores m_Wait = nullptr;
};
