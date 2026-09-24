#include "ls1shaders.h"

#ifdef Q_OS_LINUX

#include <QDir>
#include <QFile>
#include <QLibrary>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>

// The resource IDs, descriptor bindings, and vkd3d-shader ABI here follow
// MAKO's GPL-3.0-or-later LS1 implementation (eugeniosegala/MAKO, commit
// 0534110a381672dc33a285d45a61662ff06f44ae). This is a small C++17
// adaptation for Moonlight's existing Vulkan renderer.
namespace {

[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toStdString());
}

uint16_t u16(const QByteArray& bytes, qsizetype offset) {
    if (offset < 0 || offset > bytes.size() - 2) fail("Truncated Lossless.dll");
    const auto* p = reinterpret_cast<const uint8_t*>(bytes.constData() + offset);
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}

uint32_t u32(const QByteArray& bytes, qsizetype offset) {
    if (offset < 0 || offset > bytes.size() - 4) fail("Truncated Lossless.dll");
    const auto* p = reinterpret_cast<const uint8_t*>(bytes.constData() + offset);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void put32(QByteArray& bytes, qsizetype offset, uint32_t value) {
    if (offset < 0 || offset > bytes.size() - 4) fail("Invalid translated LS1 shader");
    for (int i = 0; i < 4; ++i) bytes[offset + i] = char(value >> (8 * i));
}

QByteArray resource(const QByteArray& dll, uint32_t id) {
    if (u16(dll, 0) != 0x5a4d) fail("Lossless.dll is not a PE file");
    const qsizetype pe = u32(dll, 60);
    if (u32(dll, pe) != 0x00004550) fail("Lossless.dll has no PE header");
    const uint16_t sections = u16(dll, pe + 6);
    const uint16_t optionalSize = u16(dll, pe + 20);
    const qsizetype optional = pe + 24;
    const uint16_t magic = u16(dll, optional);
    const qsizetype resourceDirectory = magic == 0x20b ? 128 :
                                         magic == 0x10b ? 112 : -1;
    if (resourceDirectory < 0 || optionalSize < resourceDirectory + 8)
        fail("Lossless.dll has an unsupported PE header");
    const uint32_t resourceRva = u32(dll, optional + resourceDirectory);
    const uint32_t resourceSize = u32(dll, optional + resourceDirectory + 4);
    if (!resourceRva || resourceSize < 24 || resourceSize > 512 * 1024 * 1024)
        fail("Lossless.dll has no valid resource directory");
    const qsizetype table = optional + optionalSize;
    qsizetype base = -1;
    for (uint16_t i = 0; i < sections; ++i) {
        const qsizetype section = table + qsizetype(i) * 40;
        const uint32_t virtualSize = u32(dll, section + 8);
        const uint32_t virtualAddress = u32(dll, section + 12);
        const uint32_t rawSize = u32(dll, section + 16);
        const uint32_t rawOffset = u32(dll, section + 20);
        if (resourceRva >= virtualAddress &&
                uint64_t(resourceRva) + resourceSize <=
                    uint64_t(virtualAddress) + std::max(virtualSize, rawSize) &&
                uint64_t(resourceRva - virtualAddress) + resourceSize <= rawSize) {
            base = qsizetype(rawOffset) + resourceRva - virtualAddress;
            if (base < 0 || base > dll.size() - resourceSize)
                fail("Lossless.dll resource directory is outside the file");
            break;
        }
    }
    if (base < 0) fail("Lossless.dll resource section was not found");

    auto entry = [&](uint32_t directory, uint32_t wanted) -> uint32_t {
        if (directory > resourceSize - 16) fail("Invalid Lossless.dll resource tree");
        const qsizetype header = base + directory;
        const uint32_t count = uint32_t(u16(dll, header + 12)) +
                               uint32_t(u16(dll, header + 14));
        if (count > 4096 || uint64_t(directory) + 16 + uint64_t(count) * 8 > resourceSize)
            fail("Invalid Lossless.dll resource directory");
        for (uint32_t i = 0; i < count; ++i) {
            const qsizetype item = header + 16 + qsizetype(i) * 8;
            if (u32(dll, item) == wanted) return u32(dll, item + 4);
        }
        fail(QString("Lossless.dll has no LS1 resource %1").arg(wanted));
    };
    const uint32_t type = entry(0, 10); // RT_RCDATA
    if (!(type & 0x80000000)) fail("Invalid Lossless.dll resource type");
    const uint32_t name = entry(type & 0x7fffffff, id);
    if (!(name & 0x80000000)) fail("Invalid Lossless.dll resource name");
    const uint32_t languageDirectory = name & 0x7fffffff;
    if (languageDirectory > resourceSize - 24)
        fail("Invalid Lossless.dll resource language");
    const uint32_t count = uint32_t(u16(dll, base + languageDirectory + 12)) +
                           uint32_t(u16(dll, base + languageDirectory + 14));
    if (!count) fail("Lossless.dll LS1 resource has no language");
    const uint32_t leaf = u32(dll, base + languageDirectory + 20);
    if (leaf & 0x80000000 || leaf > resourceSize - 16)
        fail("Invalid Lossless.dll resource leaf");
    const uint32_t dataRva = u32(dll, base + leaf);
    const uint32_t size = u32(dll, base + leaf + 4);
    if (dataRva < resourceRva ||
            uint64_t(dataRva - resourceRva) + size > resourceSize ||
            size > 64 * 1024 * 1024)
        fail("Lossless.dll LS1 resource is outside the resource section");
    return dll.mid(base + dataRva - resourceRva, size);
}

enum class StructureType : int32_t { CompileInfo = 0, InterfaceInfo = 1, SpirvTargetInfo = 4 };
enum class SourceType : int32_t { DxbcTpf = 1 };
enum class TargetType : int32_t { SpirvBinary = 1 };
enum class LogLevel : int32_t { Warning = 2 };
enum class DescriptorType : int32_t { Srv = 0, Uav = 1, Cbv = 2, Sampler = 3 };
enum class Visibility : int32_t { Compute = 1000000000 };
enum class SpirvEnvironment : int32_t { Vulkan10 = 2 };
struct ShaderCode { const void* code; size_t size; };
struct DescriptorBinding { uint32_t set, binding, count; };
struct ResourceBinding {
    DescriptorType type;
    uint32_t registerSpace, registerIndex;
    Visibility visibility;
    uint32_t flags;
    DescriptorBinding binding;
};
struct InterfaceInfo {
    StructureType type;
    const void* next;
    const ResourceBinding* bindings;
    uint32_t bindingCount;
    const void* pushConstantBuffers;
    uint32_t pushConstantBufferCount;
    const void* combinedSamplers;
    uint32_t combinedSamplerCount;
    const void* uavCounters;
    uint32_t uavCounterCount;
};
struct SpirvTargetInfo {
    StructureType type;
    const void* next;
    const char* entryPoint;
    SpirvEnvironment environment;
    const void* extensions;
    uint32_t extensionCount;
    const void* parameters;
    uint32_t parameterCount;
    bool dualSourceBlending;
    const uint32_t* outputSwizzles;
    uint32_t outputSwizzleCount;
};
struct CompileInfo {
    StructureType type;
    const void* next;
    ShaderCode source;
    SourceType sourceType;
    TargetType targetType;
    const void* options;
    uint32_t optionCount;
    LogLevel logLevel;
    const char* sourceName;
};
using Compile = int (*)(const CompileInfo*, ShaderCode*, char**);
using FreeCode = void (*)(ShaderCode*);
using FreeMessages = void (*)(char*);

struct Spec { uint32_t id, sampled, samplers, cbuffers, storageFormat; };

QByteArray translate(const QByteArray& dxbc, const Spec& spec,
                     Compile compile, FreeCode freeCode, FreeMessages freeMessages) {
    if (dxbc.size() < 32 || u32(dxbc, 0) != 0x43425844 ||
            u32(dxbc, 24) > uint32_t(dxbc.size()))
        fail(QString("LS1 resource %1 is not valid DXBC").arg(spec.id));
    std::array<ResourceBinding, 5> bindings{};
    uint32_t count = 0;
    auto add = [&](DescriptorType type, uint32_t reg, uint32_t binding, uint32_t flags) {
        bindings[count++] = {type, 0, reg, Visibility::Compute, flags, {0, binding, 1}};
    };
    if (spec.cbuffers) add(DescriptorType::Cbv, 0, 0, 1);
    if (spec.samplers) add(DescriptorType::Sampler, 0, 16, 0);
    for (uint32_t i = 0; i < spec.sampled; ++i)
        add(DescriptorType::Srv, i, 32 + i, 2);
    add(DescriptorType::Uav, 0, 48, 2);
    const SpirvTargetInfo target{StructureType::SpirvTargetInfo, nullptr, nullptr,
                                 SpirvEnvironment::Vulkan10, nullptr, 0, nullptr, 0,
                                 false, nullptr, 0};
    const InterfaceInfo interfaceInfo{StructureType::InterfaceInfo, &target,
                                      bindings.data(), count, nullptr, 0,
                                      nullptr, 0, nullptr, 0};
    const QByteArray name = QByteArray::number(spec.id);
    const CompileInfo info{StructureType::CompileInfo, &interfaceInfo,
                           {dxbc.constData(), size_t(dxbc.size())},
                           SourceType::DxbcTpf, TargetType::SpirvBinary,
                           nullptr, 0, LogLevel::Warning, name.constData()};
    ShaderCode output{};
    char* messages = nullptr;
    const int result = compile(&info, &output, &messages);
    const QString diagnostic = messages ? QString::fromUtf8(messages) : QString();
    if (messages) freeMessages(messages);
    if (result || !output.code || !output.size || output.size > 64 * 1024 * 1024) {
        if (output.code) freeCode(&output);
        fail(QString("Could not translate LS1 resource %1: %2")
             .arg(spec.id).arg(diagnostic));
    }
    QByteArray spirv(static_cast<const char*>(output.code), qsizetype(output.size));
    freeCode(&output);

    if (spirv.size() < 20 || spirv.size() % 4 || u32(spirv, 0) != 0x07230203)
        fail("Translated LS1 shader is not valid SPIR-V");
    bool patched = false;
    bool extendedFormat = spec.storageFormat != 20;
    for (qsizetype at = 20; at < spirv.size();) {
        const uint32_t op = u32(spirv, at);
        const uint32_t words = op >> 16;
        const uint32_t code = op & 0xffff;
        if (!words || qsizetype(words) * 4 > spirv.size() - at)
            fail("Translated LS1 shader has a malformed instruction");
        if (code == 17 && words >= 2) {
            const uint32_t capability = u32(spirv, at + 4);
            if (capability == 49) extendedFormat = true;
            if (capability == 56) {
                put32(spirv, at + 4, spec.storageFormat == 20 ? 49 : 1);
                if (spec.storageFormat == 20) extendedFormat = true;
            }
        }
        if (code == 25 && words >= 9 && u32(spirv, at + 28) == 2) {
            put32(spirv, at + 32, spec.storageFormat);
            patched = true;
        }
        at += qsizetype(words) * 4;
    }
    if (!patched || !extendedFormat)
        fail("Translated LS1 shader has an incompatible storage image");
    return spirv;
}

QStringList translatorCandidates(const QString& dllPath) {
    QStringList result;
    const QString configured = qEnvironmentVariable("MOONLIGHT_VKD3D_SHADER_PATH");
    if (!configured.isEmpty()) result << configured;
    const QDir common = QFileInfo(dllPath).dir();
    QDir steamCommon = common;
    steamCommon.cdUp();
    const QStringList runtimes = {"SteamLinuxRuntime_4", "SteamLinuxRuntime_sniper",
                                  "SteamLinuxRuntime_soldier", "SteamLinuxRuntime"};
    for (const QString& runtime : runtimes) {
        QDir root(steamCommon.filePath(runtime));
        for (const QString& version : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString libDir = root.filePath(version + "/files/lib/x86_64-linux-gnu");
            QDir libraries(libDir);
            for (const QString& file : libraries.entryList({"libvkd3d-shader.so.*"}, QDir::Files))
                result << libraries.filePath(file);
        }
    }
    result << "libvkd3d-shader.so.1";
    return result;
}

} // namespace

QString findLosslessScalingDll(const QString& configuredPath) {
    if (!configuredPath.isEmpty()) return configuredPath;
    const QString environment = qEnvironmentVariable("MOONLIGHT_LOSSLESS_SCALING_DLL");
    if (!environment.isEmpty()) return environment;
    const QString home = QDir::homePath();
    const QStringList roots = {home + "/.steam/steam/steamapps/common",
                               home + "/.local/share/Steam/steamapps/common",
                               home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common"};
    for (const QString& root : roots) {
        const QString path = QDir(root).filePath("Lossless Scaling/Lossless.dll");
        if (QFile::exists(path)) return path;
    }
    return {};
}

bool loadLs1Shaders(const QString& dllPath, int variant, Ls1Shaders* shaders,
                    QString* error) {
    try {
        if (!shaders || variant < 0 || variant > 4) fail("Invalid LS1 model variant");
        QFile dllFile(dllPath);
        if (!dllFile.open(QIODevice::ReadOnly))
            fail(QString("Cannot open the user's Lossless.dll: %1").arg(dllPath));
        if (dllFile.size() <= 0 || dllFile.size() > 1024ll * 1024 * 1024)
            fail("Lossless.dll has an unsupported size");
        const QByteArray dll = dllFile.readAll();
        if (dll.size() != dllFile.size()) fail("Could not read all of Lossless.dll");

        std::unique_ptr<QLibrary> library;
        Compile compile = nullptr;
        FreeCode freeCode = nullptr;
        FreeMessages freeMessages = nullptr;
        for (const QString& candidate : translatorCandidates(dllPath)) {
            auto loaded = std::make_unique<QLibrary>(candidate);
            if (!loaded->load()) continue;
            compile = reinterpret_cast<Compile>(loaded->resolve("vkd3d_shader_compile"));
            freeCode = reinterpret_cast<FreeCode>(loaded->resolve("vkd3d_shader_free_shader_code"));
            freeMessages = reinterpret_cast<FreeMessages>(loaded->resolve("vkd3d_shader_free_messages"));
            if (compile && freeCode && freeMessages) {
                library = std::move(loaded);
                break;
            }
        }
        if (!library) fail("A compatible libvkd3d-shader.so.1 was not found on this PC");

        const uint32_t first = 147 + uint32_t(variant) * 3;
        const std::array<Spec, 4> specs{{
            {first, 1, 0, 1, 4},
            {first + 1, 1, 0, 0, 4},
            {first + 2, 1, 0, 1, 20},
            {146, 2, 1, 1, 4},
        }};
        Ls1Shaders loaded;
        loaded.stage1 = translate(resource(dll, specs[0].id), specs[0],
                                  compile, freeCode, freeMessages);
        loaded.stage2 = translate(resource(dll, specs[1].id), specs[1],
                                  compile, freeCode, freeMessages);
        loaded.stage3 = translate(resource(dll, specs[2].id), specs[2],
                                  compile, freeCode, freeMessages);
        loaded.reconstruct = translate(resource(dll, specs[3].id), specs[3],
                                       compile, freeCode, freeMessages);
        loaded.translator = library->fileName();
        *shaders = std::move(loaded);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
}

bool patchLs1OutputFormat(QByteArray* shader, uint32_t spirvFormat) {
    if (!shader || shader->size() < 20 || shader->size() % 4 ||
            u32(*shader, 0) != 0x07230203 ||
            (spirvFormat != 1 && spirvFormat != 2 && spirvFormat != 4))
        return false;
    bool patched = false;
    for (qsizetype at = 20; at < shader->size();) {
        const uint32_t op = u32(*shader, at);
        const uint32_t words = op >> 16;
        if (!words || qsizetype(words) * 4 > shader->size() - at) return false;
        if ((op & 0xffff) == 25 && words >= 9 && u32(*shader, at + 28) == 2) {
            put32(*shader, at + 32, spirvFormat);
            patched = true;
        }
        at += qsizetype(words) * 4;
    }
    return patched;
}

#endif
