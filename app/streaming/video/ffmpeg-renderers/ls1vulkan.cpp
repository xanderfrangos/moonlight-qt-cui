#include "ls1vulkan.h"

#ifdef Q_OS_LINUX

#include "ls1shaders.h"

// The four LS1 stage order and bindings follow MAKO's GPL-3.0-or-later
// implementation (eugeniosegala/MAKO, commit 0534110a381672dc33a285d45a61662ff06f44ae).

#include <SDL.h>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

[[noreturn]] void vkFail(const char* operation, VkResult result) {
    throw std::runtime_error(QString("%1 failed (%2)").arg(operation).arg(result).toStdString());
}

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) vkFail(operation, result);
}

struct Functions {
    PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties getFormatProperties = nullptr;
    PFN_vkGetDeviceQueue getQueue = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkCreateImageView createImageView = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferRequirements = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkCreateSampler createSampler = nullptr;
    PFN_vkDestroySampler destroySampler = nullptr;
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines createComputePipelines = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool createDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool destroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets allocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets updateDescriptorSets = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
    PFN_vkCmdDispatch cmdDispatch = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkResetFences resetFences = nullptr;
};

Functions loadFunctions(pl_vulkan vk) {
    Functions f;
    const auto deviceProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        vk->get_proc_addr(vk->instance, "vkGetDeviceProcAddr"));
    if (!deviceProc) throw std::runtime_error("Vulkan device function lookup is unavailable");
    f.getMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        vk->get_proc_addr(vk->instance, "vkGetPhysicalDeviceMemoryProperties"));
    f.getFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        vk->get_proc_addr(vk->instance, "vkGetPhysicalDeviceFormatProperties"));
#define LS1_LOAD(member, type, symbol) \
    f.member = reinterpret_cast<type>(deviceProc(vk->device, symbol)); \
    if (!f.member) throw std::runtime_error("Missing Vulkan function " symbol)
    LS1_LOAD(getQueue, PFN_vkGetDeviceQueue, "vkGetDeviceQueue");
    LS1_LOAD(createImage, PFN_vkCreateImage, "vkCreateImage");
    LS1_LOAD(destroyImage, PFN_vkDestroyImage, "vkDestroyImage");
    LS1_LOAD(getImageRequirements, PFN_vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements");
    LS1_LOAD(allocateMemory, PFN_vkAllocateMemory, "vkAllocateMemory");
    LS1_LOAD(freeMemory, PFN_vkFreeMemory, "vkFreeMemory");
    LS1_LOAD(bindImageMemory, PFN_vkBindImageMemory, "vkBindImageMemory");
    LS1_LOAD(createImageView, PFN_vkCreateImageView, "vkCreateImageView");
    LS1_LOAD(destroyImageView, PFN_vkDestroyImageView, "vkDestroyImageView");
    LS1_LOAD(createBuffer, PFN_vkCreateBuffer, "vkCreateBuffer");
    LS1_LOAD(destroyBuffer, PFN_vkDestroyBuffer, "vkDestroyBuffer");
    LS1_LOAD(getBufferRequirements, PFN_vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    LS1_LOAD(bindBufferMemory, PFN_vkBindBufferMemory, "vkBindBufferMemory");
    LS1_LOAD(mapMemory, PFN_vkMapMemory, "vkMapMemory");
    LS1_LOAD(unmapMemory, PFN_vkUnmapMemory, "vkUnmapMemory");
    LS1_LOAD(createSampler, PFN_vkCreateSampler, "vkCreateSampler");
    LS1_LOAD(destroySampler, PFN_vkDestroySampler, "vkDestroySampler");
    LS1_LOAD(createShaderModule, PFN_vkCreateShaderModule, "vkCreateShaderModule");
    LS1_LOAD(destroyShaderModule, PFN_vkDestroyShaderModule, "vkDestroyShaderModule");
    LS1_LOAD(createDescriptorSetLayout, PFN_vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
    LS1_LOAD(destroyDescriptorSetLayout, PFN_vkDestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
    LS1_LOAD(createPipelineLayout, PFN_vkCreatePipelineLayout, "vkCreatePipelineLayout");
    LS1_LOAD(destroyPipelineLayout, PFN_vkDestroyPipelineLayout, "vkDestroyPipelineLayout");
    LS1_LOAD(createComputePipelines, PFN_vkCreateComputePipelines, "vkCreateComputePipelines");
    LS1_LOAD(destroyPipeline, PFN_vkDestroyPipeline, "vkDestroyPipeline");
    LS1_LOAD(createDescriptorPool, PFN_vkCreateDescriptorPool, "vkCreateDescriptorPool");
    LS1_LOAD(destroyDescriptorPool, PFN_vkDestroyDescriptorPool, "vkDestroyDescriptorPool");
    LS1_LOAD(allocateDescriptorSets, PFN_vkAllocateDescriptorSets, "vkAllocateDescriptorSets");
    LS1_LOAD(updateDescriptorSets, PFN_vkUpdateDescriptorSets, "vkUpdateDescriptorSets");
    LS1_LOAD(createCommandPool, PFN_vkCreateCommandPool, "vkCreateCommandPool");
    LS1_LOAD(destroyCommandPool, PFN_vkDestroyCommandPool, "vkDestroyCommandPool");
    LS1_LOAD(allocateCommandBuffers, PFN_vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    LS1_LOAD(resetCommandBuffer, PFN_vkResetCommandBuffer, "vkResetCommandBuffer");
    LS1_LOAD(beginCommandBuffer, PFN_vkBeginCommandBuffer, "vkBeginCommandBuffer");
    LS1_LOAD(endCommandBuffer, PFN_vkEndCommandBuffer, "vkEndCommandBuffer");
    LS1_LOAD(cmdPipelineBarrier, PFN_vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
    LS1_LOAD(cmdBindPipeline, PFN_vkCmdBindPipeline, "vkCmdBindPipeline");
    LS1_LOAD(cmdBindDescriptorSets, PFN_vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    LS1_LOAD(cmdDispatch, PFN_vkCmdDispatch, "vkCmdDispatch");
    LS1_LOAD(queueSubmit, PFN_vkQueueSubmit, "vkQueueSubmit");
    LS1_LOAD(createFence, PFN_vkCreateFence, "vkCreateFence");
    LS1_LOAD(destroyFence, PFN_vkDestroyFence, "vkDestroyFence");
    LS1_LOAD(waitForFences, PFN_vkWaitForFences, "vkWaitForFences");
    LS1_LOAD(resetFences, PFN_vkResetFences, "vkResetFences");
#undef LS1_LOAD
    if (!f.getMemoryProperties || !f.getFormatProperties)
        throw std::runtime_error("Missing Vulkan physical device functions");
    return f;
}

struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct Pipeline {
    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline handle = VK_NULL_HANDLE;
};

struct alignas(16) Parameters {
    uint32_t sourceWidth, sourceHeight, capturedWidth, capturedHeight;
    uint32_t sourceOffsetX, sourceOffsetY, gammaPreprocess, reserved;
    uint32_t outputWidth, outputHeight, outputOffsetX, outputOffsetY;
};
static_assert(sizeof(Parameters) == 48, "LS1 parameter layout changed");

struct FrameSlot {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    std::array<VkDescriptorSet, 4> sets{};
    VkImageView sourceView = VK_NULL_HANDLE;
    VkImageView outputView = VK_NULL_HANDLE;
};

VkImageMemoryBarrier barrier(VkImage image, VkImageLayout oldLayout,
                             VkImageLayout newLayout, VkAccessFlags source,
                             VkAccessFlags destination) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = source;
    b.dstAccessMask = destination;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    return b;
}

} // namespace

struct Ls1VulkanHook::Impl {
    explicit Impl(pl_vulkan v, Ls1Shaders s) : vk(v), shaders(std::move(s)), f(loadFunctions(v)) {
        if (!v->features || !v->features->features.shaderStorageImageExtendedFormats)
            throw std::runtime_error("Vulkan lacks R8_SNORM storage image support for LS1");
        VkFormatProperties format{};
        f.getFormatProperties(v->phys_device, VK_FORMAT_R8_SNORM, &format);
        if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
                !(format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            throw std::runtime_error("Vulkan device cannot use LS1's R8_SNORM feature image");
        if (!v->queue_graphics.count)
            throw std::runtime_error("Vulkan graphics queue is unavailable for LS1");
        f.getQueue(v->device, v->queue_graphics.index, 0, &queue);
        if (!queue) throw std::runtime_error("Could not get Vulkan queue for LS1");
        // libplacebo creates these using the already enabled timeline feature.
        pl_vulkan_sem_params semParams{};
        semParams.type = VK_SEMAPHORE_TYPE_TIMELINE;
        inputReady = pl_vulkan_sem_create(vk->gpu, &semParams);
        outputReady = pl_vulkan_sem_create(vk->gpu, &semParams);
        computeDone = pl_vulkan_sem_create(vk->gpu, &semParams);
        if (!inputReady || !outputReady || !computeDone) {
            if (inputReady) pl_vulkan_sem_destroy(vk->gpu, &inputReady);
            if (outputReady) pl_vulkan_sem_destroy(vk->gpu, &outputReady);
            if (computeDone) pl_vulkan_sem_destroy(vk->gpu, &computeDone);
            throw std::runtime_error("Could not create LS1 Vulkan timeline semaphores");
        }
    }
    ~Impl() { destroy(); }

    pl_vulkan vk;
    Ls1Shaders shaders;
    Functions f;
    VkQueue queue = VK_NULL_HANDLE;
    VkSemaphore inputReady = VK_NULL_HANDLE;
    VkSemaphore outputReady = VK_NULL_HANDLE;
    VkSemaphore computeDone = VK_NULL_HANDLE;
    uint64_t semaphoreValue = 0;
    uint32_t sourceWidth = 0, sourceHeight = 0;
    uint32_t outputWidth = 0, outputHeight = 0;
    VkFormat outputFormat = VK_FORMAT_UNDEFINED;
    Image feature, intermediateA, intermediateB;
    VkBuffer parameters = VK_NULL_HANDLE;
    VkDeviceMemory parameterMemory = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::array<Pipeline, 4> pipelines{};
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::array<FrameSlot, 3> frameSlots{};
    size_t nextSlot = 0;

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties props{};
        f.getMemoryProperties(vk->phys_device, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((bits & (1u << i)) &&
                    (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("No suitable Vulkan memory type for LS1");
    }

    VkImageView imageView(VkImage image, VkFormat format) const {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        check(f.createImageView(vk->device, &info, nullptr, &view), "vkCreateImageView");
        return view;
    }

    Image createImage(uint32_t w, uint32_t h, VkFormat format,
                      VkImageUsageFlags usage) const {
        Image result;
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {w, h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        try {
            check(f.createImage(vk->device, &info, nullptr, &result.handle), "vkCreateImage");
            VkMemoryRequirements requirements{};
            f.getImageRequirements(vk->device, result.handle, &requirements);
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(f.allocateMemory(vk->device, &allocation, nullptr, &result.memory), "vkAllocateMemory");
            check(f.bindImageMemory(vk->device, result.handle, result.memory, 0), "vkBindImageMemory");
            result.view = imageView(result.handle, format);
        } catch (...) {
            if (result.view) f.destroyImageView(vk->device, result.view, nullptr);
            if (result.handle) f.destroyImage(vk->device, result.handle, nullptr);
            if (result.memory) f.freeMemory(vk->device, result.memory, nullptr);
            throw;
        }
        return result;
    }

    void destroyImage(Image& image) {
        if (image.view) f.destroyImageView(vk->device, image.view, nullptr);
        if (image.handle) f.destroyImage(vk->device, image.handle, nullptr);
        if (image.memory) f.freeMemory(vk->device, image.memory, nullptr);
        image = {};
    }

    void destroyPipeline() {
        if (!f.destroyFence) return;
        // All renderer and compute uses must complete before their Vulkan
        // resources and libplacebo's semaphore references can be released.
        pl_gpu_finish(vk->gpu);
        for (auto& slot : frameSlots) {
            if (slot.submitted) f.waitForFences(vk->device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
            if (slot.sourceView) f.destroyImageView(vk->device, slot.sourceView, nullptr);
            if (slot.outputView) f.destroyImageView(vk->device, slot.outputView, nullptr);
            if (slot.fence) f.destroyFence(vk->device, slot.fence, nullptr);
            slot = {};
        }
        if (commandPool) f.destroyCommandPool(vk->device, commandPool, nullptr);
        commandPool = VK_NULL_HANDLE;
        if (descriptorPool) f.destroyDescriptorPool(vk->device, descriptorPool, nullptr);
        descriptorPool = VK_NULL_HANDLE;
        for (auto& pipeline : pipelines) {
            if (pipeline.handle) f.destroyPipeline(vk->device, pipeline.handle, nullptr);
            if (pipeline.layout) f.destroyPipelineLayout(vk->device, pipeline.layout, nullptr);
            if (pipeline.descriptors)
                f.destroyDescriptorSetLayout(vk->device, pipeline.descriptors, nullptr);
            pipeline = {};
        }
        if (sampler) f.destroySampler(vk->device, sampler, nullptr);
        sampler = VK_NULL_HANDLE;
        if (parameters) f.destroyBuffer(vk->device, parameters, nullptr);
        if (parameterMemory) f.freeMemory(vk->device, parameterMemory, nullptr);
        parameters = VK_NULL_HANDLE;
        parameterMemory = VK_NULL_HANDLE;
        destroyImage(feature);
        destroyImage(intermediateA);
        destroyImage(intermediateB);
        sourceWidth = sourceHeight = outputWidth = outputHeight = 0;
        outputFormat = VK_FORMAT_UNDEFINED;
        nextSlot = 0;
    }

    void destroy() {
        if (f.destroyFence) destroyPipeline();
        if (inputReady) pl_vulkan_sem_destroy(vk->gpu, &inputReady);
        if (outputReady) pl_vulkan_sem_destroy(vk->gpu, &outputReady);
        if (computeDone) pl_vulkan_sem_destroy(vk->gpu, &computeDone);
    }

    Pipeline createPipeline(const QByteArray& spirv, uint32_t sampled,
                            bool uniform, bool useSampler) const;
    void createResources(uint32_t w, uint32_t h, uint32_t outW, uint32_t outH,
                         VkFormat format);
    void updateDescriptors(FrameSlot& slot, VkImageView source, VkImageView output);
    void record(FrameSlot& slot);
    pl_hook_res run(const pl_hook_params* params);
};

Pipeline Ls1VulkanHook::Impl::createPipeline(const QByteArray& spirv,
                                              uint32_t sampled, bool uniform,
                                              bool useSampler) const {
    Pipeline pipeline;
    VkShaderModule module = VK_NULL_HANDLE;
    try {
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        uint32_t count = 0;
        auto add = [&](uint32_t binding, VkDescriptorType type) {
            auto& item = bindings[count++];
            item.binding = binding;
            item.descriptorType = type;
            item.descriptorCount = 1;
            item.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        };
        if (uniform) add(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        if (useSampler) add(16, VK_DESCRIPTOR_TYPE_SAMPLER);
        for (uint32_t i = 0; i < sampled; ++i)
            add(32 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        add(48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        VkDescriptorSetLayoutCreateInfo descriptors{};
        descriptors.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptors.bindingCount = count;
        descriptors.pBindings = bindings.data();
        check(f.createDescriptorSetLayout(vk->device, &descriptors, nullptr,
                                          &pipeline.descriptors), "vkCreateDescriptorSetLayout");
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &pipeline.descriptors;
        check(f.createPipelineLayout(vk->device, &layout, nullptr,
                                     &pipeline.layout), "vkCreatePipelineLayout");
        VkShaderModuleCreateInfo shader{};
        shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader.codeSize = size_t(spirv.size());
        shader.pCode = reinterpret_cast<const uint32_t*>(spirv.constData());
        check(f.createShaderModule(vk->device, &shader, nullptr, &module),
              "vkCreateShaderModule");
        VkComputePipelineCreateInfo compute{};
        compute.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = module;
        compute.stage.pName = "main";
        compute.layout = pipeline.layout;
        const VkResult result = f.createComputePipelines(vk->device, VK_NULL_HANDLE,
                                                          1, &compute, nullptr,
                                                          &pipeline.handle);
        f.destroyShaderModule(vk->device, module, nullptr);
        module = VK_NULL_HANDLE;
        check(result, "vkCreateComputePipelines");
    } catch (...) {
        if (module) f.destroyShaderModule(vk->device, module, nullptr);
        if (pipeline.handle) f.destroyPipeline(vk->device, pipeline.handle, nullptr);
        if (pipeline.layout) f.destroyPipelineLayout(vk->device, pipeline.layout, nullptr);
        if (pipeline.descriptors)
            f.destroyDescriptorSetLayout(vk->device, pipeline.descriptors, nullptr);
        throw;
    }
    return pipeline;
}

void Ls1VulkanHook::Impl::createResources(uint32_t w, uint32_t h,
                                           uint32_t outW, uint32_t outH,
                                           VkFormat format) {
    destroyPipeline();
    if (!w || !h || !outW || !outH || w > 8192 || h > 8192 ||
            outW > 16384 || outH > 16384)
        throw std::runtime_error("LS1 image dimensions exceed its Vulkan limits");
    VkFormatProperties featureFormat{};
    f.getFormatProperties(vk->phys_device, format, &featureFormat);
    if (!(featureFormat.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        throw std::runtime_error("Vulkan output format does not support LS1 storage writes");

    feature = createImage(w * 2, h * 2, VK_FORMAT_R8_SNORM,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    intermediateA = createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    intermediateB = createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = sizeof(Parameters);
    buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(f.createBuffer(vk->device, &buffer, nullptr, &parameters), "vkCreateBuffer");
    VkMemoryRequirements memory{};
    f.getBufferRequirements(vk->device, parameters, &memory);
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = memory.size;
    allocation.memoryTypeIndex = memoryType(memory.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(f.allocateMemory(vk->device, &allocation, nullptr, &parameterMemory),
          "vkAllocateMemory");
    check(f.bindBufferMemory(vk->device, parameters, parameterMemory, 0),
          "vkBindBufferMemory");
    void* mapped = nullptr;
    check(f.mapMemory(vk->device, parameterMemory, 0, sizeof(Parameters), 0, &mapped),
          "vkMapMemory");
    const Parameters values{w, h, w, h, 0, 0, 0, 0, outW, outH, 0, 0};
    std::memcpy(mapped, &values, sizeof(values));
    f.unmapMemory(vk->device, parameterMemory);

    VkSamplerCreateInfo sample{};
    sample.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sample.magFilter = VK_FILTER_LINEAR;
    sample.minFilter = VK_FILTER_LINEAR;
    sample.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sample.addressModeU = sample.addressModeV = sample.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sample.maxLod = VK_LOD_CLAMP_NONE;
    check(f.createSampler(vk->device, &sample, nullptr, &sampler), "vkCreateSampler");

    QByteArray reconstruction = shaders.reconstruct;
    const uint32_t spirvFormat = format == VK_FORMAT_R16G16B16A16_SFLOAT ? 2 : 1;
    if (!patchLs1OutputFormat(&reconstruction, spirvFormat))
        throw std::runtime_error("Could not adapt LS1 output shader to libplacebo's image format");
    pipelines[0] = createPipeline(shaders.stage1, 1, true, false);
    pipelines[1] = createPipeline(shaders.stage2, 1, false, false);
    pipelines[2] = createPipeline(shaders.stage3, 1, true, false);
    pipelines[3] = createPipeline(reconstruction, 2, true, true);

    const std::array<VkDescriptorPoolSize, 4> sizes{{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 9},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 3},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 15},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12},
    }};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 12;
    pool.poolSizeCount = uint32_t(sizes.size());
    pool.pPoolSizes = sizes.data();
    check(f.createDescriptorPool(vk->device, &pool, nullptr, &descriptorPool),
          "vkCreateDescriptorPool");
    VkCommandPoolCreateInfo commands{};
    commands.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commands.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commands.queueFamilyIndex = vk->queue_graphics.index;
    check(f.createCommandPool(vk->device, &commands, nullptr, &commandPool),
          "vkCreateCommandPool");
    const std::array<VkDescriptorSetLayout, 4> layouts{{
        pipelines[0].descriptors, pipelines[1].descriptors,
        pipelines[2].descriptors, pipelines[3].descriptors,
    }};
    for (auto& slot : frameSlots) {
        VkDescriptorSetAllocateInfo sets{};
        sets.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        sets.descriptorPool = descriptorPool;
        sets.descriptorSetCount = uint32_t(layouts.size());
        sets.pSetLayouts = layouts.data();
        check(f.allocateDescriptorSets(vk->device, &sets, slot.sets.data()),
              "vkAllocateDescriptorSets");
        VkCommandBufferAllocateInfo command{};
        command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command.commandPool = commandPool;
        command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command.commandBufferCount = 1;
        check(f.allocateCommandBuffers(vk->device, &command, &slot.commands),
              "vkAllocateCommandBuffers");
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(f.createFence(vk->device, &fence, nullptr, &slot.fence), "vkCreateFence");
    }
    sourceWidth = w;
    sourceHeight = h;
    outputWidth = outW;
    outputHeight = outH;
    outputFormat = format;
}

void Ls1VulkanHook::Impl::updateDescriptors(FrameSlot& slot,
                                              VkImageView source,
                                              VkImageView output) {
    std::array<VkDescriptorImageInfo, 10> images{};
    std::array<VkWriteDescriptorSet, 16> writes{};
    uint32_t imageCount = 0, writeCount = 0;
    const VkDescriptorBufferInfo uniform{parameters, 0, sizeof(Parameters)};
    auto buffer = [&](int stage) {
        auto& write = writes[writeCount++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = slot.sets[stage];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &uniform;
    };
    auto image = [&](int stage, uint32_t binding, VkDescriptorType type,
                     VkImageView view) {
        auto& info = images[imageCount++];
        info.imageView = view;
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        auto& write = writes[writeCount++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = slot.sets[stage];
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pImageInfo = &info;
    };
    buffer(0);
    image(0, 32, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, source);
    image(0, 48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, intermediateA.view);
    image(1, 32, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, intermediateA.view);
    image(1, 48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, intermediateB.view);
    buffer(2);
    image(2, 32, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, intermediateB.view);
    image(2, 48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, feature.view);
    buffer(3);
    auto& samplerInfo = images[imageCount++];
    samplerInfo.sampler = sampler;
    auto& samplerWrite = writes[writeCount++];
    samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    samplerWrite.dstSet = slot.sets[3];
    samplerWrite.dstBinding = 16;
    samplerWrite.descriptorCount = 1;
    samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerWrite.pImageInfo = &samplerInfo;
    image(3, 32, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, feature.view);
    image(3, 33, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, source);
    image(3, 48, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, output);
    f.updateDescriptorSets(vk->device, writeCount, writes.data(), 0, nullptr);
}

void Ls1VulkanHook::Impl::record(FrameSlot& slot) {
    check(f.resetCommandBuffer(slot.commands, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(f.beginCommandBuffer(slot.commands, &begin), "vkBeginCommandBuffer");
    const std::array<VkImageMemoryBarrier, 3> init{{
        barrier(intermediateA.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT),
        barrier(intermediateB.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT),
        barrier(feature.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT),
    }};
    f.cmdPipelineBarrier(slot.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, uint32_t(init.size()), init.data());
    auto dispatch = [&](int stage, uint32_t w, uint32_t h) {
        f.cmdBindPipeline(slot.commands, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelines[stage].handle);
        f.cmdBindDescriptorSets(slot.commands, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipelines[stage].layout, 0, 1, &slot.sets[stage],
                                0, nullptr);
        f.cmdDispatch(slot.commands, (w + 15) / 16, (h + 15) / 16, 1);
    };
    auto between = [&](VkImage image) {
        const auto b = barrier(image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_GENERAL,
                               VK_ACCESS_SHADER_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT);
        f.cmdPipelineBarrier(slot.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
    };
    dispatch(0, sourceWidth, sourceHeight);
    between(intermediateA.handle);
    dispatch(1, sourceWidth, sourceHeight);
    between(intermediateB.handle);
    dispatch(2, sourceWidth, sourceHeight);
    between(feature.handle);
    dispatch(3, outputWidth, outputHeight);
    check(f.endCommandBuffer(slot.commands), "vkEndCommandBuffer");
}

pl_hook_res Ls1VulkanHook::Impl::run(const pl_hook_params* params) {
    pl_hook_res result{};
    if (!params || !params->tex || !params->get_tex ||
            (params->orig_color &&
             (params->orig_color->transfer == PL_COLOR_TRC_PQ ||
              params->orig_color->transfer == PL_COLOR_TRC_HLG)))
        return result;
    const pl_tex input = params->tex;
    const uint32_t w = uint32_t(input->params.w);
    const uint32_t h = uint32_t(input->params.h);
    const uint32_t outW = uint32_t(std::abs(params->dst_rect.x1 - params->dst_rect.x0));
    const uint32_t outH = uint32_t(std::abs(params->dst_rect.y1 - params->dst_rect.y0));
    if (!w || !h || !outW || !outH ||
            uint64_t(outW) * outH <= uint64_t(w) * h)
        return result;
    // The current LS1 parameter layout supports a whole RGB input image.
    // Leave cropped or flipped images to libplacebo's regular scaler.
    if (params->rect.x0 != 0 || params->rect.y0 != 0 ||
            params->rect.x1 != int(w) || params->rect.y1 != int(h))
        return result;
    pl_tex output = params->get_tex(params->priv, int(outW), int(outH));
    if (!output) throw std::runtime_error("Could not allocate LS1 output image");
    VkFormat sourceFormat = VK_FORMAT_UNDEFINED, targetFormat = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags sourceFlags = 0, outputFlags = 0;
    const VkImage sourceImage = pl_vulkan_unwrap(vk->gpu, input, &sourceFormat,
                                                 &sourceFlags);
    const VkImage outputImage = pl_vulkan_unwrap(vk->gpu, output, &targetFormat,
                                                 &outputFlags);
    if (!sourceImage || !outputImage ||
            !(sourceFlags & VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !(outputFlags & VK_IMAGE_USAGE_STORAGE_BIT) ||
            (targetFormat != VK_FORMAT_R16G16B16A16_SFLOAT &&
             targetFormat != VK_FORMAT_R32G32B32A32_SFLOAT))
        throw std::runtime_error("Libplacebo image formats cannot run LS1");
    if (w != sourceWidth || h != sourceHeight || outW != outputWidth ||
            outH != outputHeight || targetFormat != outputFormat)
        createResources(w, h, outW, outH, targetFormat);

    FrameSlot& slot = frameSlots[nextSlot++ % frameSlots.size()];
    if (slot.submitted) {
        check(f.waitForFences(vk->device, 1, &slot.fence, VK_TRUE,
                              1000000000ULL), "LS1 compute fence wait");
        slot.submitted = false;
        if (slot.sourceView) f.destroyImageView(vk->device, slot.sourceView, nullptr);
        if (slot.outputView) f.destroyImageView(vk->device, slot.outputView, nullptr);
        slot.sourceView = slot.outputView = VK_NULL_HANDLE;
    }
    const uint64_t value = ++semaphoreValue;
    pl_vulkan_hold_params inputHold{};
    inputHold.tex = input;
    inputHold.layout = VK_IMAGE_LAYOUT_GENERAL;
    inputHold.qf = vk->queue_graphics.index;
    inputHold.semaphore = {inputReady, value};
    pl_vulkan_hold_params outputHold{};
    outputHold.tex = output;
    outputHold.layout = VK_IMAGE_LAYOUT_GENERAL;
    outputHold.qf = vk->queue_graphics.index;
    outputHold.semaphore = {outputReady, value};
    if (!pl_vulkan_hold_ex(vk->gpu, &inputHold))
        throw std::runtime_error("Could not hold LS1 source image");
    if (!pl_vulkan_hold_ex(vk->gpu, &outputHold)) {
        pl_vulkan_release_params release{};
        release.tex = input;
        release.layout = VK_IMAGE_LAYOUT_GENERAL;
        release.qf = vk->queue_graphics.index;
        release.semaphore = inputHold.semaphore;
        pl_vulkan_release_ex(vk->gpu, &release);
        throw std::runtime_error("Could not hold LS1 output image");
    }
    bool submitted = false;
    try {
        slot.sourceView = imageView(sourceImage, sourceFormat);
        slot.outputView = imageView(outputImage, targetFormat);
        updateDescriptors(slot, slot.sourceView, slot.outputView);
        record(slot);
        pl_gpu_flush(vk->gpu);
        const std::array<VkSemaphore, 2> waits{{inputReady, outputReady}};
        const std::array<VkPipelineStageFlags, 2> stages{{
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        }};
        const std::array<uint64_t, 2> waitValues{{value, value}};
        const VkTimelineSemaphoreSubmitInfo timeline{
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr,
            uint32_t(waitValues.size()), waitValues.data(), 1, &value,
        };
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = &timeline;
        submit.waitSemaphoreCount = uint32_t(waits.size());
        submit.pWaitSemaphores = waits.data();
        submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slot.commands;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &computeDone;
        check(f.resetFences(vk->device, 1, &slot.fence), "vkResetFences");
        vk->lock_queue(vk, vk->queue_graphics.index, 0);
        const VkResult queued = f.queueSubmit(queue, 1, &submit, slot.fence);
        vk->unlock_queue(vk, vk->queue_graphics.index, 0);
        check(queued, "vkQueueSubmit LS1");
        slot.submitted = true;
        submitted = true;
    } catch (...) {
        if (!submitted) {
            for (auto held : {inputHold, outputHold}) {
                pl_vulkan_release_params release{};
                release.tex = held.tex;
                release.layout = VK_IMAGE_LAYOUT_GENERAL;
                release.qf = vk->queue_graphics.index;
                release.semaphore = held.semaphore;
                pl_vulkan_release_ex(vk->gpu, &release);
            }
        }
        throw;
    }
    for (pl_tex texture : {input, output}) {
        pl_vulkan_release_params release{};
        release.tex = texture;
        release.layout = VK_IMAGE_LAYOUT_GENERAL;
        release.qf = vk->queue_graphics.index;
        release.semaphore = {computeDone, value};
        pl_vulkan_release_ex(vk->gpu, &release);
    }
    result.output = PL_HOOK_SIG_TEX;
    result.tex = output;
    result.repr = params->repr;
    result.color = params->color;
    result.components = params->components;
    result.rect = {0, 0, float(outW), float(outH)};
    return result;
}

Ls1VulkanHook::Ls1VulkanHook(pl_vulkan vulkan, const QString& dllPath,
                             int variant) {
    m_Hook.stages = PL_HOOK_RGB;
    m_Hook.input = PL_HOOK_SIG_TEX;
    m_Hook.priv = this;
    m_Hook.hook = &Ls1VulkanHook::execute;
    m_Hook.signature = 0x4d4f4f4e4c5331ULL;
    Ls1Shaders shaders;
    QString error;
    if (!loadLs1Shaders(dllPath, variant, &shaders, &error)) {
        m_Error = error;
        return;
    }
    try {
        m_Impl = std::make_unique<Impl>(vulkan, std::move(shaders));
    } catch (const std::exception& e) {
        m_Error = QString::fromUtf8(e.what());
    }
}

Ls1VulkanHook::~Ls1VulkanHook() = default;

bool Ls1VulkanHook::ready() const { return m_Impl != nullptr; }
QString Ls1VulkanHook::error() const { return m_Error; }

pl_hook_res Ls1VulkanHook::execute(void* context,
                                   const pl_hook_params* params) {
    auto* self = static_cast<Ls1VulkanHook*>(context);
    try {
        return self->m_Impl->run(params);
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LS1 Vulkan hook failed: %s", e.what());
        pl_hook_res result{};
        result.failed = true;
        return result;
    }
}

#endif
