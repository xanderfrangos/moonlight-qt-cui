#pragma once

#include <QtGlobal>

#ifdef Q_OS_LINUX

#include <QString>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/vulkan.h>
#include <memory>

// A libplacebo RGB hook backed by the four LS1 Vulkan compute stages.
class Ls1VulkanHook {
public:
    Ls1VulkanHook(pl_vulkan vulkan, const QString& dllPath, int variant);
    ~Ls1VulkanHook();
    Ls1VulkanHook(const Ls1VulkanHook&) = delete;
    Ls1VulkanHook& operator=(const Ls1VulkanHook&) = delete;
    const pl_hook* hook() const { return &m_Hook; }
    bool ready() const;
    QString error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
    QString m_Error;
    pl_hook m_Hook{};
    static pl_hook_res execute(void* context, const pl_hook_params* params);
};

#endif
