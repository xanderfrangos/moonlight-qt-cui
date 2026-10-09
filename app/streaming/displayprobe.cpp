#include "displayprobe.h"

#include "path.h"
#include "streaming/streamutils.h"
#include "streaming/video/ffmpeg-renderers/pacer/gamescopedisplaystate.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>

#include <SDL.h>

#ifdef Q_OS_LINUX

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#if __has_include(<libdrm/drm_mode.h>)
#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>
#define DISPLAYPROBE_HAS_DRM 1
#elif __has_include(<drm/drm_mode.h>)
#include <drm/drm.h>
#include <drm/drm_mode.h>
#define DISPLAYPROBE_HAS_DRM 1
#endif

#if defined(HAVE_LIBPLACEBO_VULKAN) && __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#include <SDL_vulkan.h>
#define DISPLAYPROBE_HAS_VULKAN 1
#endif

// Xlib's macros (None, Status, Bool...) go last so they cannot collide
#ifdef HAS_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#if __has_include(<X11/extensions/Xrandr.h>)
#include <X11/extensions/Xrandr.h>
#define DISPLAYPROBE_HAS_XRANDR 1
#endif
#endif

namespace {

// Wlroots gives each Xwayland the first free display number up to this
constexpr int MaxXwaylandDisplay = 32;

std::string vformat(const char* fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    const int length = vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (length <= 0) {
        return std::string();
    }
    std::string text(size_t(length) + 1, '\0');
    vsnprintf(&text[0], text.size(), fmt, ap);
    text.resize(size_t(length));
    return text;
}

std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::string text = vformat(fmt, ap);
    va_end(ap);
    return text;
}

// Body lines, plus the few that are worth repeating at the top and in the log
class Report
{
public:
    void section(const std::string& title)
    {
        m_Body += "\n== " + title + " ==\n";
    }

    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        va_list ap;
        va_start(ap, fmt);
        m_Body += vformat(fmt, ap) + "\n";
        va_end(ap);
    }

    void summary(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        va_list ap;
        va_start(ap, fmt);
        std::string line = vformat(fmt, ap);
        va_end(ap);
        m_Body += line + "\n";
        m_Summary.push_back(line);
    }

    const std::vector<std::string>& summaries() const { return m_Summary; }

    std::string text(const char* reason) const
    {
        std::string out = format("Moonlight %s display probe (%s), %s\n",
                                 qPrintable(QCoreApplication::applicationVersion()), reason,
                                 qPrintable(QDateTime::currentDateTime().toString(Qt::ISODate)));
        out += "\n== Summary ==\n";
        for (const std::string& line : m_Summary) {
            out += line + "\n";
        }
        return out + m_Body;
    }

private:
    std::string m_Body;
    std::vector<std::string> m_Summary;
};

uint64_t nowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

bool readFile(const std::string& path, std::string& out, size_t limit = 1 << 20)
{
    out.clear();
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char buffer[4096];
    ssize_t count;
    while (out.size() < limit && (count = read(fd, buffer, sizeof(buffer))) > 0) {
        out.append(buffer, size_t(count));
    }
    close(fd);
    return true;
}

std::string trim(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

std::string fileOrError(const std::string& path)
{
    std::string text;
    if (!readFile(path, text, 4096)) {
        return format("(%s)", strerror(errno));
    }
    text = trim(text);
    std::replace(text.begin(), text.end(), '\n', ' ');
    return text;
}

std::vector<std::string> listDirectory(const std::string& path)
{
    std::vector<std::string> names;
    if (DIR* dir = opendir(path.c_str())) {
        while (dirent* entry = readdir(dir)) {
            if (entry->d_name[0] != '.' || std::strncmp(entry->d_name, ".X", 2) == 0) {
                names.push_back(entry->d_name);
            }
        }
        closedir(dir);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void hexDump(Report& r, const std::vector<uint8_t>& data, const char* indent)
{
    for (size_t offset = 0; offset < data.size(); offset += 16) {
        std::string line;
        for (size_t i = offset; i < std::min(data.size(), offset + 16); i++) {
            line += format("%02x", data[i]);
        }
        r.add("%s%s", indent, line.c_str());
    }
}

// ---------------------------------------------------------------------------
// EDID

double timingRefresh(uint64_t clockHz, uint32_t htotal, uint32_t vtotal)
{
    return htotal != 0 && vtotal != 0 ? double(clockHz) / (double(htotal) * double(vtotal)) : 0.0;
}

std::string descriptorText(const uint8_t* d)
{
    std::string text;
    for (int i = 5; i < 18 && d[i] != 0x0a; i++) {
        text += (d[i] >= 0x20 && d[i] < 0x7f) ? char(d[i]) : '?';
    }
    return trim(text);
}

std::string detailedTiming(const uint8_t* d)
{
    const uint32_t clockKhz = uint32_t(d[0] | (d[1] << 8)) * 10;
    const uint32_t hactive = d[2] | ((d[4] & 0xf0) << 4);
    const uint32_t hblank = d[3] | ((d[4] & 0x0f) << 8);
    const uint32_t vactive = d[5] | ((d[7] & 0xf0) << 4);
    const uint32_t vblank = d[6] | ((d[7] & 0x0f) << 8);
    const bool interlaced = (d[17] & 0x80) != 0;
    double refresh = timingRefresh(uint64_t(clockKhz) * 1000, hactive + hblank, vactive + vblank);
    if (interlaced) {
        refresh *= 2;
    }
    return format("%ux%u%s @ %.3f Hz (pixel clock %u kHz)", hactive, vactive * (interlaced ? 2 : 1),
                  interlaced ? "i" : "", refresh, clockKhz);
}

void decodeEdid(Report& r, const std::vector<uint8_t>& e, const std::string& label)
{
    static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    if (e.size() < 128 || std::memcmp(e.data(), header, sizeof(header)) != 0) {
        r.add("    EDID: %zu bytes, not a valid EDID header", e.size());
        hexDump(r, e, "      ");
        return;
    }
    const uint16_t vendor = uint16_t((e[8] << 8) | e[9]);
    const char mfg[4] = { char('A' - 1 + ((vendor >> 10) & 0x1f)), char('A' - 1 + ((vendor >> 5) & 0x1f)),
                          char('A' - 1 + (vendor & 0x1f)), 0 };
    r.add("    EDID: %zu bytes, manufacturer %s product 0x%04x, %d, version %u.%u, %u extension block(s)",
          e.size(), mfg, e[10] | (e[11] << 8), 1990 + e[17], e[18], e[19], e[126]);

    std::string name, range;
    for (int offset = 54; offset <= 108; offset += 18) {
        const uint8_t* d = &e[size_t(offset)];
        if (d[0] != 0 || d[1] != 0) {
            r.add("    detailed timing: %s", detailedTiming(d).c_str());
        }
        else if (d[3] == 0xfc) {
            name = descriptorText(d);
            r.add("    name: %s", name.c_str());
        }
        else if (d[3] == 0xfd) {
            // EDID 1.4 offsets: bit 1 adds 255 to the max V rate, bits 1:0 = 11
            // add it to the min as well; bits 3:2 do the same for H
            const int minV = d[5] + ((d[4] & 0x03) == 0x03 ? 255 : 0);
            const int maxV = d[6] + ((d[4] & 0x02) ? 255 : 0);
            const int minH = d[7] + ((d[4] & 0x0c) == 0x0c ? 255 : 0);
            const int maxH = d[8] + ((d[4] & 0x08) ? 255 : 0);
            range = format("%d-%d Hz vertical", minV, maxV);
            r.add("    range limits: V %d-%d Hz, H %d-%d kHz, max pixel clock %d MHz, timing support 0x%02x",
                  minV, maxV, minH, maxH, d[9] * 10, d[10]);
        }
        else if (d[3] == 0xff || d[3] == 0xfe) {
            r.add("    text descriptor 0x%02x: %s", d[3], descriptorText(d).c_str());
        }
    }
    r.summary("%s EDID: %s %s, range limits %s", label.c_str(), mfg, name.empty() ? "(unnamed)" : name.c_str(),
              range.empty() ? "absent" : range.c_str());

    for (size_t block = 128; block + 128 <= e.size(); block += 128) {
        const uint8_t* b = &e[block];
        if (b[0] == 0x02) {
            const uint8_t dtdOffset = b[2];
            r.add("    block %zu: CTA-861 revision %u, flags 0x%02x", block / 128, b[1], b[3]);
            for (size_t i = 4; i < dtdOffset && i < 127;) {
                const int tag = b[i] >> 5;
                const int length = b[i] & 0x1f;
                static const char* names[] = { "reserved", "audio", "video", "vendor-specific", "speaker",
                                               "VESA DTC", "reserved", "extended" };
                std::string what = names[tag];
                if (tag == 3 && length >= 3) {
                    const uint32_t oui = b[i + 1] | (b[i + 2] << 8) | (b[i + 3] << 16);
                    what += format(" OUI %06x", oui);
                    if (oui == 0x000c03) what += " (HDMI 1.4)";
                    else if (oui == 0xc45dd8) what += " (HDMI Forum, holds VRRmin/VRRmax)";
                    else if (oui == 0x00001a) what += " (AMD FreeSync)";
                }
                else if (tag == 7 && length >= 1) {
                    what += format(" tag %u", b[i + 1]);
                }
                std::string bytes;
                for (int k = 0; k <= length && i + size_t(k) < 128; k++) {
                    bytes += format("%02x", b[i + size_t(k)]);
                }
                r.add("      data block %s: %s", what.c_str(), bytes.c_str());
                i += size_t(length) + 1;
            }
            for (size_t i = dtdOffset; dtdOffset >= 4 && i + 18 <= 127; i += 18) {
                if (b[i] != 0 || b[i + 1] != 0) {
                    r.add("      detailed timing: %s", detailedTiming(&b[i]).c_str());
                }
            }
        }
        else {
            r.add("    block %zu: extension tag 0x%02x (see hex)", block / 128, b[0]);
        }
    }
    r.add("    EDID hex (for edid-decode):");
    hexDump(r, e, "      ");
}

// ---------------------------------------------------------------------------
// X11: every Xwayland, its root properties and RandR

#ifdef HAS_X11

struct XServer {
    std::string name;
    Display* display = nullptr;
    bool own = false;
    bool gamescope = false;
    uint32_t pid = 0;
    uint32_t serverId = 0;
};

bool rootCardinal(Display* display, Atom atom, uint32_t& value)
{
    Atom type = None;
    int bits = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    bool found = false;
    if (atom != None &&
            XGetWindowProperty(display, DefaultRootWindow(display), atom, 0, 1, False, XA_CARDINAL,
                               &type, &bits, &count, &remaining, &data) == Success &&
            type == XA_CARDINAL && bits == 32 && count == 1 && data != nullptr) {
        // Xlib returns 32-bit properties as longs
        value = uint32_t(*reinterpret_cast<unsigned long*>(data));
        found = true;
    }
    if (data != nullptr) {
        XFree(data);
    }
    return found;
}

bool rootCardinal(Display* display, const char* name, uint32_t& value)
{
    return rootCardinal(display, XInternAtom(display, name, True), value);
}

bool rootString(Display* display, const char* name, std::string& value)
{
    const Atom atom = XInternAtom(display, name, True);
    Atom type = None;
    int bits = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    bool found = false;
    if (atom != None &&
            XGetWindowProperty(display, DefaultRootWindow(display), atom, 0, 1024, False, AnyPropertyType,
                               &type, &bits, &count, &remaining, &data) == Success &&
            bits == 8 && data != nullptr) {
        value.assign(reinterpret_cast<const char*>(data), strnlen(reinterpret_cast<const char*>(data), count));
        found = true;
    }
    if (data != nullptr) {
        XFree(data);
    }
    return found;
}

std::string atomName(Display* display, Atom atom)
{
    if (atom == None) {
        return "None";
    }
    char* name = XGetAtomName(display, atom);
    std::string text = name != nullptr ? name : format("atom%lu", atom);
    if (name != nullptr) {
        XFree(name);
    }
    return text;
}

std::string formatProperty(Display* display, Atom type, int bits, unsigned long count, unsigned long remaining,
                           const unsigned char* data)
{
    const std::string typeName = atomName(display, type);
    std::string out = typeName + "/" + std::to_string(bits) + ":";
    if (data == nullptr) {
        return out + " (empty)";
    }
    unsigned long shown = count;
    if (bits == 8) {
        const bool text = type == XA_STRING || typeName == "UTF8_STRING";
        shown = std::min(count, text ? 1024ul : 64ul);
        out += " ";
        for (unsigned long i = 0; i < shown; i++) {
            if (text) {
                const char c = char(data[i]);
                out += c == 0 ? '|' : ((c >= 0x20 && c < 0x7f) ? c : '?');
            }
            else {
                out += format("%02x", data[i]);
            }
        }
    }
    else if (bits == 16) {
        shown = std::min(count, 32ul);
        const short* values = reinterpret_cast<const short*>(data);
        for (unsigned long i = 0; i < shown; i++) {
            out += format(" %d", values[i]);
        }
    }
    else if (bits == 32) {
        shown = std::min(count, 64ul);
        // Xlib returns 32-bit properties as longs
        const long* values = reinterpret_cast<const long*>(data);
        for (unsigned long i = 0; i < shown; i++) {
            if (type == XA_ATOM) {
                out += " " + atomName(display, Atom(values[i]));
            }
            else if (type == XA_WINDOW) {
                out += format(" 0x%lx", (unsigned long)values[i]);
            }
            else if (type == XA_INTEGER) {
                out += format(" %d", int32_t(values[i]));
            }
            else {
                out += format(" %u", uint32_t(values[i]));
            }
        }
    }
    if (shown < count || remaining != 0) {
        out += format(" ... (%lu items, %lu bytes more)", count, remaining);
    }
    return out;
}

std::string rootProperty(Display* display, Atom atom)
{
    Atom type = None;
    int bits = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, DefaultRootWindow(display), atom, 0, 1024, False, AnyPropertyType,
                           &type, &bits, &count, &remaining, &data) != Success) {
        return "(unreadable)";
    }
    std::string text = formatProperty(display, type, bits, count, remaining, data);
    if (data != nullptr) {
        XFree(data);
    }
    return text;
}

// ":1.0" and "unix:1" name the same server as ":1"
std::string normalizeDisplayName(const char* name)
{
    if (name == nullptr) {
        return std::string();
    }
    std::string text = name;
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos) {
        return text;
    }
    text = text.substr(colon);
    const size_t dot = text.find('.');
    return dot == std::string::npos ? text : text.substr(0, dot);
}

std::vector<XServer> openXServers()
{
    const std::string own = normalizeDisplayName(getenv("DISPLAY"));
    std::vector<std::string> names;
    for (int number = 0; number <= MaxXwaylandDisplay; number++) {
        names.push_back(format(":%d", number));
    }
    if (!own.empty() && std::find(names.begin(), names.end(), own) == names.end()) {
        names.push_back(own);
    }

    std::vector<XServer> servers;
    for (const std::string& name : names) {
        Display* display = XOpenDisplay(name.c_str());
        if (display == nullptr) {
            continue;
        }
        XServer server;
        server.name = name;
        server.display = display;
        server.own = name == own;
        server.gamescope = rootCardinal(display, "GAMESCOPE_PID", server.pid) &&
                           rootCardinal(display, "GAMESCOPE_XWAYLAND_SERVER_ID", server.serverId);
        servers.push_back(server);
    }
    return servers;
}

void closeXServers(std::vector<XServer>& servers)
{
    for (XServer& server : servers) {
        if (server.display != nullptr) {
            XCloseDisplay(server.display);
        }
    }
    servers.clear();
}

// Gamescope's first Xwayland, where Steam's settings and Gamescope's display
// feedback live: server 0 of the Gamescope that owns DISPLAY
const XServer* gamescopeRootServer(const std::vector<XServer>& servers)
{
    const XServer* own = nullptr;
    for (const XServer& server : servers) {
        if (server.own) {
            own = &server;
        }
    }
    for (const XServer& server : servers) {
        if (server.gamescope && server.serverId == 0 &&
                (own == nullptr || !own->gamescope || own->pid == server.pid)) {
            return &server;
        }
    }
    return nullptr;
}

std::string gamescopeStateLine(Display* display)
{
    static const struct {
        const char* atom;
        const char* label;
    } items[] = {
        { "GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK", "refresh" },
        { "GAMESCOPE_VRR_CAPABLE", "vrr_capable" },
        { "GAMESCOPE_VRR_ENABLED", "vrr_enabled" },
        { "GAMESCOPE_VRR_FEEDBACK", "vrr_in_use" },
        { "GAMESCOPE_FPS_LIMIT", "fps_limit" },
        { "GAMESCOPE_DYNAMIC_REFRESH", "dynamic_refresh" },
        { "GAMESCOPE_DYNAMIC_REFRESH_EXTERNAL", "dynamic_refresh_ext" },
        { "GAMESCOPE_ALLOW_TEARING", "allow_tearing" },
        { "GAMESCOPE_LIMITER_FEEDBACK", "limiter" },
        { "GAMESCOPE_DISPLAY_IS_EXTERNAL", "external" },
        { "GAMESCOPE_FOCUSED_APP", "focused_app" },
    };
    std::string line;
    for (const auto& item : items) {
        uint32_t value = 0;
        line += line.empty() ? "" : " ";
        line += item.label;
        line += rootCardinal(display, item.atom, value) ? "=" + std::to_string(value) : "=-";
    }
    return line;
}

#ifdef DISPLAYPROBE_HAS_XRANDR

struct RandrApi {
    bool loaded = false;
    Bool (*queryExtension)(Display*, int*, int*) = nullptr;
    Status (*queryVersion)(Display*, int*, int*) = nullptr;
    XRRScreenResources* (*getResources)(Display*, Window) = nullptr;
    void (*freeResources)(XRRScreenResources*) = nullptr;
    XRROutputInfo* (*getOutput)(Display*, XRRScreenResources*, RROutput) = nullptr;
    void (*freeOutput)(XRROutputInfo*) = nullptr;
    XRRCrtcInfo* (*getCrtc)(Display*, XRRScreenResources*, RRCrtc) = nullptr;
    void (*freeCrtc)(XRRCrtcInfo*) = nullptr;
    Atom* (*listOutputProperties)(Display*, RROutput, int*) = nullptr;
    int (*getOutputProperty)(Display*, RROutput, Atom, long, long, Bool, Bool, Atom, Atom*, int*,
                             unsigned long*, unsigned long*, unsigned char**) = nullptr;

    static const RandrApi& get()
    {
        static RandrApi api = [] {
            RandrApi a;
            void* library = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
            if (library == nullptr) {
                return a;
            }
#define LOAD(field, symbol) a.field = reinterpret_cast<decltype(a.field)>(dlsym(library, symbol))
            LOAD(queryExtension, "XRRQueryExtension");
            LOAD(queryVersion, "XRRQueryVersion");
            LOAD(getResources, "XRRGetScreenResourcesCurrent");
            LOAD(freeResources, "XRRFreeScreenResources");
            LOAD(getOutput, "XRRGetOutputInfo");
            LOAD(freeOutput, "XRRFreeOutputInfo");
            LOAD(getCrtc, "XRRGetCrtcInfo");
            LOAD(freeCrtc, "XRRFreeCrtcInfo");
            LOAD(listOutputProperties, "XRRListOutputProperties");
            LOAD(getOutputProperty, "XRRGetOutputProperty");
#undef LOAD
            a.loaded = a.queryExtension && a.queryVersion && a.getResources && a.freeResources && a.getOutput &&
                       a.freeOutput && a.getCrtc && a.freeCrtc && a.listOutputProperties && a.getOutputProperty;
            return a;
        }();
        return api;
    }
};

double randrRefresh(const XRRModeInfo& mode)
{
    double vtotal = mode.vTotal;
    if (mode.modeFlags & RR_DoubleScan) vtotal *= 2;
    if (mode.modeFlags & RR_Interlace) vtotal /= 2;
    return mode.hTotal != 0 && vtotal != 0 ? double(mode.dotClock) / (mode.hTotal * vtotal) : 0.0;
}

void probeRandr(Report& r, const XServer& server)
{
    const RandrApi& rr = RandrApi::get();
    if (!rr.loaded) {
        r.add("  RandR: libXrandr.so.2 unavailable");
        return;
    }
    Display* display = server.display;
    int eventBase = 0, errorBase = 0, major = 0, minor = 0;
    if (!rr.queryExtension(display, &eventBase, &errorBase) || !rr.queryVersion(display, &major, &minor)) {
        r.add("  RandR: extension not present");
        return;
    }
    XRRScreenResources* res = rr.getResources(display, DefaultRootWindow(display));
    if (res == nullptr) {
        r.add("  RandR %d.%d: no screen resources", major, minor);
        return;
    }
    r.add("  RandR %d.%d: %d output(s), %d crtc(s), %d mode(s)", major, minor, res->noutput, res->ncrtc, res->nmode);
    std::map<RRMode, const XRRModeInfo*> modes;
    for (int i = 0; i < res->nmode; i++) {
        const XRRModeInfo& mode = res->modes[i];
        modes[mode.id] = &mode;
        r.add("    mode 0x%lx %s: %ux%u @ %.3f Hz (clock %lu, htotal %u, vtotal %u, flags 0x%lx)", mode.id,
              mode.name ? mode.name : "", mode.width, mode.height, randrRefresh(mode), mode.dotClock, mode.hTotal,
              mode.vTotal, mode.modeFlags);
    }
    for (int i = 0; i < res->ncrtc; i++) {
        XRRCrtcInfo* crtc = rr.getCrtc(display, res, res->crtcs[i]);
        if (crtc == nullptr) {
            continue;
        }
        const auto mode = modes.find(crtc->mode);
        const double refresh = mode != modes.end() ? randrRefresh(*mode->second) : 0.0;
        r.add("    crtc 0x%lx: %ux%u+%d+%d mode 0x%lx (%.3f Hz), %d output(s)", res->crtcs[i], crtc->width,
              crtc->height, crtc->x, crtc->y, crtc->mode, refresh, crtc->noutput);
        if (crtc->mode != None) {
            r.summary("Xwayland %s%s RandR current mode: %ux%u @ %.3f Hz", server.name.c_str(),
                      server.own ? " (DISPLAY)" : "", crtc->width, crtc->height, refresh);
        }
        rr.freeCrtc(crtc);
    }
    for (int i = 0; i < res->noutput; i++) {
        XRROutputInfo* output = rr.getOutput(display, res, res->outputs[i]);
        if (output == nullptr) {
            continue;
        }
        std::string modeList;
        for (int m = 0; m < output->nmode; m++) {
            const auto mode = modes.find(output->modes[m]);
            if (mode != modes.end()) {
                modeList += format("%s%ux%u@%.2f%s", modeList.empty() ? "" : " ", mode->second->width,
                                   mode->second->height, randrRefresh(*mode->second), m < output->npreferred ? "*" : "");
            }
        }
        r.add("    output 0x%lx %s: connection %d, crtc 0x%lx, %lux%lu mm, modes: %s", res->outputs[i],
              output->name ? output->name : "", output->connection, output->crtc, output->mm_width,
              output->mm_height, modeList.c_str());
        int propertyCount = 0;
        Atom* properties = rr.listOutputProperties(display, res->outputs[i], &propertyCount);
        for (int p = 0; p < propertyCount; p++) {
            Atom type = None;
            int bits = 0;
            unsigned long count = 0, remaining = 0;
            unsigned char* data = nullptr;
            if (rr.getOutputProperty(display, res->outputs[i], properties[p], 0, 64, False, False, AnyPropertyType,
                                     &type, &bits, &count, &remaining, &data) == Success) {
                r.add("      %s = %s", atomName(display, properties[p]).c_str(),
                      formatProperty(display, type, bits, count, remaining, data).c_str());
            }
            if (data != nullptr) {
                XFree(data);
            }
        }
        if (properties != nullptr) {
            XFree(properties);
        }
        rr.freeOutput(output);
    }
    rr.freeResources(res);
}

#else

void probeRandr(Report& r, const XServer&)
{
    r.add("  RandR: built without Xrandr headers");
}

#endif

void probeX11(Report& r, uint32_t& gamescopePid, std::string& edidPath)
{
    r.section("X11 servers");
    r.add("DISPLAY=%s; scanning :0 to :%d", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)", MaxXwaylandDisplay);
    std::vector<XServer> servers = openXServers();
    if (servers.empty()) {
        r.summary("X11: no X server reachable");
        return;
    }
    const XServer* root = gamescopeRootServer(servers);
    for (const XServer& server : servers) {
        Display* display = server.display;
        r.section(format("X11 %s%s%s", server.name.c_str(), server.own ? " (DISPLAY)" : "",
                         &server == root ? " (Gamescope root server)" : ""));
        r.add("  vendor %s %d, screen %dx%d px, %dx%d mm", ServerVendor(display), VendorRelease(display),
              DisplayWidth(display, DefaultScreen(display)), DisplayHeight(display, DefaultScreen(display)),
              DisplayWidthMM(display, DefaultScreen(display)), DisplayHeightMM(display, DefaultScreen(display)));
        if (server.gamescope) {
            r.add("  Gamescope PID %u, Xwayland server %u", server.pid, server.serverId);
            r.summary("Xwayland %s%s: Gamescope PID %u server %u: %s", server.name.c_str(),
                      server.own ? " (DISPLAY)" : "", server.pid, server.serverId,
                      gamescopeStateLine(display).c_str());
            std::string modeList;
            if (rootString(display, "GAMESCOPE_DISPLAY_MODE_LIST_EXTERNAL", modeList)) {
                r.summary("Xwayland %s: GAMESCOPE_DISPLAY_MODE_LIST_EXTERNAL = %s", server.name.c_str(),
                          modeList.c_str());
            }
        }
        else {
            r.summary("X11 %s%s: not a Gamescope Xwayland", server.name.c_str(), server.own ? " (DISPLAY)" : "");
        }

        int count = 0;
        Atom* atoms = XListProperties(display, DefaultRootWindow(display), &count);
        std::vector<std::pair<std::string, Atom>> named;
        for (int i = 0; i < count; i++) {
            named.emplace_back(atomName(display, atoms[i]), atoms[i]);
        }
        if (atoms != nullptr) {
            XFree(atoms);
        }
        std::sort(named.begin(), named.end());
        r.add("  root window properties (%d):", count);
        for (const auto& property : named) {
            r.add("    %s = %s", property.first.c_str(), rootProperty(display, property.second).c_str());
        }
        probeRandr(r, server);
    }
    if (root != nullptr) {
        gamescopePid = root->pid;
        rootString(root->display, "GAMESCOPE_DISPLAY_EDID_PATH", edidPath);
    }
    else {
        r.summary("X11: no Gamescope server 0 found for DISPLAY's Gamescope");
    }
    closeXServers(servers);
}

#else

void probeX11(Report& r, uint32_t&, std::string&)
{
    r.section("X11 servers");
    r.summary("X11: built without X11 support");
}

#endif

// ---------------------------------------------------------------------------
// Gamescope's Wayland socket, through libwayland-client loaded at runtime so
// builds without Wayland development files still carry it

namespace Wl {

// ABI-stable layouts from wayland-util.h
struct Interface;
struct Message {
    const char* name;
    const char* signature;
    const Interface** types;
};
struct Interface {
    const char* name;
    int version;
    int methodCount;
    const Message* methods;
    int eventCount;
    const Message* events;
};
struct Array {
    size_t size;
    size_t alloc;
    void* data;
};

constexpr uint32_t MarshalFlagDestroy = 1;

const Interface* noTypes[8] = {};

// gamescope-control.xml, version 7. Only feature_support and
// active_display_info (bound at version 2) are ever received.
const Message controlRequests[] = {
    { "destroy", "", noTypes },
    { "set_app_target_refresh_cycle", "2uu", noTypes },
    { "take_screenshot", "3suu", noTypes },
    { "display_sleep", "4uu", noTypes },
    { "set_look", "5hhu", noTypes },
    { "unset_look", "5", noTypes },
    { "request_app_performance_stats", "6u", noTypes },
    { "set_keyboard_layout", "7ss", noTypes },
};
const Message controlEvents[] = {
    { "feature_support", "uuu", noTypes },
    { "active_display_info", "2sssua", noTypes },
    { "screenshot_taken", "3s", noTypes },
    { "app_performance_stats", "6uuu", noTypes },
};
const Interface controlInterface = { "gamescope_control", 7, 8, controlRequests, 4, controlEvents };

struct Api {
    bool loaded = false;
    void* (*connect)(const char*) = nullptr;
    void (*disconnect)(void*) = nullptr;
    int (*roundtrip)(void*) = nullptr;
    void* (*marshalFlags)(void*, uint32_t, const Interface*, uint32_t, uint32_t, ...) = nullptr;
    int (*addListener)(void*, void (**)(void), void*) = nullptr;
    void (*destroy)(void*) = nullptr;
    uint32_t (*version)(void*) = nullptr;
    const Interface* registry = nullptr;
    const Interface* output = nullptr;

    static const Api& get()
    {
        static Api api = [] {
            Api a;
            void* library = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
            if (library == nullptr) {
                return a;
            }
#define LOAD(field, symbol) a.field = reinterpret_cast<decltype(a.field)>(dlsym(library, symbol))
            LOAD(connect, "wl_display_connect");
            LOAD(disconnect, "wl_display_disconnect");
            LOAD(roundtrip, "wl_display_roundtrip");
            LOAD(marshalFlags, "wl_proxy_marshal_flags");
            LOAD(addListener, "wl_proxy_add_listener");
            LOAD(destroy, "wl_proxy_destroy");
            LOAD(version, "wl_proxy_get_version");
            LOAD(registry, "wl_registry_interface");
            LOAD(output, "wl_output_interface");
#undef LOAD
            a.loaded = a.connect && a.disconnect && a.roundtrip && a.marshalFlags && a.addListener && a.destroy &&
                       a.version && a.registry && a.output;
            return a;
        }();
        return api;
    }
};

struct Global {
    uint32_t name;
    std::string interface;
    uint32_t version;
};

struct Probe {
    Report* report;
    std::string socket;
    std::vector<Global> globals;
};

using Callback = void (*)(void);

void onGlobal(void* data, void*, uint32_t name, const char* interface, uint32_t version)
{
    static_cast<Probe*>(data)->globals.push_back({ name, interface != nullptr ? interface : "", version });
}
void onGlobalRemove(void*, void*, uint32_t) {}
Callback registryListener[] = { reinterpret_cast<Callback>(onGlobal), reinterpret_cast<Callback>(onGlobalRemove) };

void onGeometry(void* data, void*, int32_t x, int32_t y, int32_t widthMm, int32_t heightMm, int32_t subpixel,
                const char* make, const char* model, int32_t transform)
{
    static_cast<Probe*>(data)->report->add("    wl_output geometry: %d,%d %dx%d mm, subpixel %d, make '%s' model '%s', transform %d",
                                           x, y, widthMm, heightMm, subpixel, make ? make : "", model ? model : "", transform);
}
void onMode(void* data, void*, uint32_t flags, int32_t width, int32_t height, int32_t refreshMhz)
{
    Probe* probe = static_cast<Probe*>(data);
    probe->report->summary("Wayland %s wl_output mode: %dx%d @ %.3f Hz%s%s", probe->socket.c_str(), width,
                           height, refreshMhz / 1000.0, (flags & 1) ? " current" : "", (flags & 2) ? " preferred" : "");
}
void onDone(void*, void*) {}
void onScale(void* data, void*, int32_t scale)
{
    static_cast<Probe*>(data)->report->add("    wl_output scale: %d", scale);
}
void onName(void* data, void*, const char* name)
{
    static_cast<Probe*>(data)->report->add("    wl_output name: %s", name ? name : "");
}
void onDescription(void* data, void*, const char* description)
{
    static_cast<Probe*>(data)->report->add("    wl_output description: %s", description ? description : "");
}
Callback outputListener[] = {
    reinterpret_cast<Callback>(onGeometry), reinterpret_cast<Callback>(onMode),
    reinterpret_cast<Callback>(onDone), reinterpret_cast<Callback>(onScale),
    reinterpret_cast<Callback>(onName), reinterpret_cast<Callback>(onDescription),
};

void onFeature(void* data, void*, uint32_t feature, uint32_t version, uint32_t flags)
{
    static_cast<Probe*>(data)->report->add("    gamescope_control feature %u version %u flags 0x%x", feature, version, flags);
}
void onDisplayInfo(void* data, void*, const char* connector, const char* make, const char* model, uint32_t flags,
                   Array* rates)
{
    Probe* probe = static_cast<Probe*>(data);
    std::string rateList;
    if (rates != nullptr && rates->data != nullptr) {
        const uint32_t* values = static_cast<const uint32_t*>(rates->data);
        for (size_t i = 0; i < rates->size / sizeof(uint32_t); i++) {
            rateList += format("%s%u", rateList.empty() ? "" : " ", values[i]);
        }
    }
    probe->report->summary("Gamescope active_display_info: connector %s, '%s %s', flags 0x%x (%s%s%s), valid refresh rates [%s]",
                           connector ? connector : "", make ? make : "", model ? model : "", flags,
                           (flags & 1) ? "internal " : "external ", (flags & 2) ? "HDR " : "",
                           (flags & 4) ? "VRR" : "no VRR", rateList.empty() ? "empty: fixed at its mode" : rateList.c_str());
}
void onScreenshot(void*, void*, const char*) {}
void onPerformance(void*, void*, uint32_t, uint32_t, uint32_t) {}
Callback controlListener[] = {
    reinterpret_cast<Callback>(onFeature), reinterpret_cast<Callback>(onDisplayInfo),
    reinterpret_cast<Callback>(onScreenshot), reinterpret_cast<Callback>(onPerformance),
};

void probeSocket(Report& r, const Api& api, const std::string& socket)
{
    r.add("  socket %s:", socket.c_str());
    void* display = api.connect(socket.c_str());
    if (display == nullptr) {
        r.summary("Wayland %s: connect failed (%s)", socket.c_str(), strerror(errno));
        return;
    }
    Probe probe{ &r, socket, {} };
    // wl_display.get_registry
    void* registry = api.marshalFlags(display, 1, api.registry, api.version(display), 0, nullptr);
    api.addListener(registry, registryListener, &probe);
    api.roundtrip(display);

    std::vector<std::pair<void*, bool>> bound;
    std::string interfaces;
    for (const Global& global : probe.globals) {
        r.add("    global %u: %s v%u", global.name, global.interface.c_str(), global.version);
        interfaces += (interfaces.empty() ? "" : " ") + global.interface;
        const Interface* interface = nullptr;
        uint32_t version = 0;
        Callback* listener = nullptr;
        if (global.interface == "wl_output") {
            interface = api.output;
            version = std::min<uint32_t>({ global.version, uint32_t(api.output->version), 4 });
            listener = outputListener;
        }
        else if (global.interface == "gamescope_control") {
            interface = &controlInterface;
            version = std::min<uint32_t>(global.version, 2);
            listener = controlListener;
        }
        if (interface != nullptr) {
            // wl_registry.bind
            void* proxy = api.marshalFlags(registry, 0, interface, version, 0, global.name, interface->name,
                                           version, nullptr);
            if (proxy != nullptr) {
                api.addListener(proxy, listener, &probe);
                bound.emplace_back(proxy, interface == &controlInterface);
            }
        }
    }
    api.roundtrip(display);
    api.roundtrip(display);
    for (const auto& proxy : bound) {
        if (proxy.second) {
            // gamescope_control.destroy
            api.marshalFlags(proxy.first, 0, nullptr, api.version(proxy.first), MarshalFlagDestroy);
        }
        else {
            api.destroy(proxy.first);
        }
    }
    api.destroy(registry);
    api.disconnect(display);
}

}

void probeWayland(Report& r)
{
    r.section("Wayland");
    const Wl::Api& api = Wl::Api::get();
    if (!api.loaded) {
        r.summary("Wayland: libwayland-client.so.0 unavailable");
        return;
    }
    std::vector<std::string> sockets;
    for (const char* variable : { "GAMESCOPE_WAYLAND_DISPLAY", "WAYLAND_DISPLAY" }) {
        const char* value = getenv(variable);
        if (value != nullptr && *value != 0 && std::find(sockets.begin(), sockets.end(), value) == sockets.end()) {
            sockets.push_back(value);
        }
    }
    if (sockets.empty()) {
        r.summary("Wayland: neither GAMESCOPE_WAYLAND_DISPLAY nor WAYLAND_DISPLAY is set");
    }
    for (const std::string& socket : sockets) {
        Wl::probeSocket(r, api, socket);
    }
}

// ---------------------------------------------------------------------------
// KMS through raw DRM ioctls, so builds without libdrm still carry it. A card
// node opened by a non-master can read every object; it never probes
// connectors (count_modes is nonzero on the first query, like
// drmModeGetConnectorCurrent()).

#ifdef DISPLAYPROBE_HAS_DRM

namespace Drm {

int xioctl(int fd, unsigned long request, void* argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && (errno == EINTR || errno == EAGAIN));
    return result;
}

template <typename T>
uint64_t pointer(T* value)
{
    return uint64_t(uintptr_t(value));
}

struct Property {
    uint32_t id = 0;
    std::string name;
    uint32_t flags = 0;
    uint64_t value = 0;
    std::vector<std::pair<uint64_t, std::string>> enums;
};

std::vector<Property> properties(int fd, uint32_t object, uint32_t type)
{
    std::vector<Property> result;
    drm_mode_obj_get_properties query{};
    query.obj_id = object;
    query.obj_type = type;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &query) != 0 || query.count_props == 0) {
        return result;
    }
    std::vector<uint32_t> ids(query.count_props);
    std::vector<uint64_t> values(query.count_props);
    query.props_ptr = pointer(ids.data());
    query.prop_values_ptr = pointer(values.data());
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &query) != 0) {
        return result;
    }
    const size_t count = std::min<size_t>(query.count_props, ids.size());
    for (size_t i = 0; i < count; i++) {
        Property property;
        property.id = ids[i];
        property.value = values[i];
        drm_mode_get_property info{};
        info.prop_id = ids[i];
        if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &info) != 0) {
            continue;
        }
        property.name.assign(info.name, strnlen(info.name, sizeof(info.name)));
        property.flags = info.flags;
        if ((info.flags & (DRM_MODE_PROP_ENUM | DRM_MODE_PROP_BITMASK)) && info.count_enum_blobs > 0) {
            std::vector<drm_mode_property_enum> enums(info.count_enum_blobs);
            std::vector<uint64_t> enumValues(std::max<uint32_t>(info.count_values, 1));
            info.enum_blob_ptr = pointer(enums.data());
            info.values_ptr = pointer(enumValues.data());
            info.count_values = uint32_t(enumValues.size());
            if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &info) == 0) {
                for (size_t e = 0; e < std::min<size_t>(info.count_enum_blobs, enums.size()); e++) {
                    property.enums.emplace_back(enums[e].value,
                                                std::string(enums[e].name, strnlen(enums[e].name, sizeof(enums[e].name))));
                }
            }
        }
        result.push_back(property);
    }
    return result;
}

const Property* findProperty(const std::vector<Property>& properties, const char* name)
{
    for (const Property& property : properties) {
        if (property.name == name) {
            return &property;
        }
    }
    return nullptr;
}

std::vector<uint8_t> blob(int fd, uint32_t id)
{
    drm_mode_get_blob query{};
    query.blob_id = id;
    if (id == 0 || xioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &query) != 0 || query.length == 0) {
        return {};
    }
    std::vector<uint8_t> data(query.length);
    query.data = pointer(data.data());
    if (xioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &query) != 0) {
        return {};
    }
    data.resize(std::min<size_t>(data.size(), query.length));
    return data;
}

std::string valueText(const Property& property)
{
    const uint32_t extended = property.flags & DRM_MODE_PROP_EXTENDED_TYPE;
    std::string text;
    if (property.flags & DRM_MODE_PROP_BLOB) {
        text = format("blob %llu", (unsigned long long)property.value);
    }
    else if (property.flags & DRM_MODE_PROP_ENUM) {
        text = format("%llu", (unsigned long long)property.value);
        for (const auto& entry : property.enums) {
            if (entry.first == property.value) {
                text += " (" + entry.second + ")";
            }
        }
    }
    else if (property.flags & DRM_MODE_PROP_BITMASK) {
        text = format("0x%llx", (unsigned long long)property.value);
        for (const auto& entry : property.enums) {
            if (entry.first < 64 && (property.value & (1ull << entry.first))) {
                text += " " + entry.second;
            }
        }
    }
    else if (extended == DRM_MODE_PROP_OBJECT) {
        text = format("object %llu", (unsigned long long)property.value);
    }
    else if (extended == DRM_MODE_PROP_SIGNED_RANGE) {
        text = format("%lld", (long long)property.value);
    }
    else {
        text = format("%llu", (unsigned long long)property.value);
    }
    if (property.flags & DRM_MODE_PROP_IMMUTABLE) {
        text += " [immutable]";
    }
    if (property.flags & DRM_MODE_PROP_ATOMIC) {
        text += " [atomic]";
    }
    return text;
}

double modeRefresh(const drm_mode_modeinfo& mode)
{
    double vtotal = mode.vtotal;
    if (mode.flags & DRM_MODE_FLAG_INTERLACE) vtotal /= 2;
    if (mode.flags & DRM_MODE_FLAG_DBLSCAN) vtotal *= 2;
    if (mode.vscan > 1) vtotal *= mode.vscan;
    return mode.htotal != 0 && vtotal != 0 ? double(mode.clock) * 1000.0 / (mode.htotal * vtotal) : 0.0;
}

std::string modeText(const drm_mode_modeinfo& mode)
{
    return format("%s %ux%u @ %.3f Hz (vrefresh %u, clock %u kHz, htotal %u, vtotal %u, flags 0x%x, type 0x%x%s)",
                  std::string(mode.name, strnlen(mode.name, sizeof(mode.name))).c_str(), mode.hdisplay,
                  mode.vdisplay, modeRefresh(mode), mode.vrefresh, mode.clock, mode.htotal, mode.vtotal, mode.flags,
                  mode.type, (mode.type & DRM_MODE_TYPE_PREFERRED) ? " preferred" : "");
}

bool sameTiming(const drm_mode_modeinfo& a, const drm_mode_modeinfo& b)
{
    return a.clock == b.clock && a.hdisplay == b.hdisplay && a.vdisplay == b.vdisplay && a.htotal == b.htotal &&
           a.vtotal == b.vtotal && a.flags == b.flags;
}

std::string connectorName(uint32_t type, uint32_t typeId)
{
    static const char* names[] = { "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS",
                                   "Component", "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI",
                                   "DPI", "Writeback", "SPI", "USB" };
    return format("%s-%u", type < sizeof(names) / sizeof(names[0]) ? names[type] : "Type", typeId);
}

struct Resources {
    std::vector<uint32_t> crtcs, connectors, encoders;
};

bool resources(int fd, Resources& out)
{
    drm_mode_card_res res{};
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        return false;
    }
    std::vector<uint32_t> fbs(std::max<uint32_t>(res.count_fbs, 1));
    out.crtcs.resize(res.count_crtcs);
    out.connectors.resize(res.count_connectors);
    out.encoders.resize(res.count_encoders);
    res.fb_id_ptr = pointer(fbs.data());
    res.count_fbs = uint32_t(fbs.size());
    res.crtc_id_ptr = pointer(out.crtcs.data());
    res.connector_id_ptr = pointer(out.connectors.data());
    res.encoder_id_ptr = pointer(out.encoders.data());
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        return false;
    }
    out.crtcs.resize(std::min<size_t>(out.crtcs.size(), res.count_crtcs));
    out.connectors.resize(std::min<size_t>(out.connectors.size(), res.count_connectors));
    out.encoders.resize(std::min<size_t>(out.encoders.size(), res.count_encoders));
    return true;
}

struct Connector {
    drm_mode_get_connector info{};
    std::vector<drm_mode_modeinfo> modes;
};

bool connector(int fd, uint32_t id, Connector& out)
{
    // A nonzero count_modes on the first query keeps the kernel from probing
    drm_mode_modeinfo scratch{};
    drm_mode_get_connector query{};
    query.connector_id = id;
    query.count_modes = 1;
    query.modes_ptr = pointer(&scratch);
    if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &query) != 0) {
        return false;
    }
    std::vector<drm_mode_modeinfo> modes(std::max<uint32_t>(query.count_modes, 1));
    std::vector<uint32_t> encoders(std::max<uint32_t>(query.count_encoders, 1));
    drm_mode_get_connector full{};
    full.connector_id = id;
    full.count_modes = uint32_t(modes.size());
    full.modes_ptr = pointer(modes.data());
    full.count_encoders = uint32_t(encoders.size());
    full.encoders_ptr = pointer(encoders.data());
    if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &full) != 0) {
        return false;
    }
    modes.resize(full.count_modes <= modes.size() ? full.count_modes : 0);
    out.info = full;
    out.modes = modes;
    return true;
}

bool crtc(int fd, uint32_t id, drm_mode_crtc& out)
{
    out = drm_mode_crtc{};
    out.crtc_id = id;
    return xioctl(fd, DRM_IOCTL_MODE_GETCRTC, &out) == 0;
}

int openCard(const std::string& path)
{
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (fd >= 0) {
        drm_set_client_cap cap{};
        cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
        cap.value = 1;
        xioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
        // Exposes the atomic properties (CRTC_ID, ACTIVE, VRR_ENABLED...)
        cap.capability = DRM_CLIENT_CAP_ATOMIC;
        xioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    }
    return fd;
}

std::vector<std::string> cardPaths()
{
    std::vector<std::string> paths;
    for (const std::string& name : listDirectory("/dev/dri")) {
        if (name.compare(0, 4, "card") == 0) {
            paths.push_back("/dev/dri/" + name);
        }
    }
    return paths;
}

std::string driverName(int fd)
{
    char name[64] = {};
    drm_version version{};
    version.name = name;
    version.name_len = sizeof(name) - 1;
    if (xioctl(fd, DRM_IOCTL_VERSION, &version) != 0) {
        return "?";
    }
    return name;
}

// The active CRTCs' modes and VRR_ENABLED, for repeated sampling
std::string activeState(int fd, const std::string& card)
{
    Resources res;
    if (!resources(fd, res)) {
        return card + ": unreadable";
    }
    std::string line;
    for (uint32_t id : res.crtcs) {
        drm_mode_crtc info;
        if (!crtc(fd, id, info) || !info.mode_valid) {
            continue;
        }
        const std::vector<Property> props = properties(fd, id, DRM_MODE_OBJECT_CRTC);
        const Property* vrr = findProperty(props, "VRR_ENABLED");
        line += format("%s%s crtc %u %ux%u@%.3f vrr_enabled=%s", line.empty() ? "" : "; ", card.c_str(), id,
                       info.mode.hdisplay, info.mode.vdisplay, modeRefresh(info.mode),
                       vrr != nullptr ? std::to_string(vrr->value).c_str() : "-");
    }
    return line.empty() ? card + ": no active crtc" : line;
}

void probeCard(Report& r, const std::string& path)
{
    const int fd = openCard(path);
    if (fd < 0) {
        r.summary("KMS %s: open failed (%s)", path.c_str(), strerror(errno));
        return;
    }
    const std::string card = path.substr(path.rfind('/') + 1);
    r.section(format("KMS %s (%s)", path.c_str(), driverName(fd).c_str()));
    Resources res;
    if (!resources(fd, res)) {
        r.summary("KMS %s: GETRESOURCES failed (%s)", card.c_str(), strerror(errno));
        close(fd);
        return;
    }

    std::map<uint32_t, drm_mode_crtc> crtcs;
    for (uint32_t id : res.crtcs) {
        drm_mode_crtc info;
        if (crtc(fd, id, info)) {
            crtcs[id] = info;
        }
        const std::vector<Property> props = properties(fd, id, DRM_MODE_OBJECT_CRTC);
        std::string propText;
        for (const Property& property : props) {
            propText += format("%s%s=%s", propText.empty() ? "" : ", ", property.name.c_str(), valueText(property).c_str());
        }
        r.add("  crtc %u: %s", id, crtcs.count(id) && crtcs[id].mode_valid ? modeText(crtcs[id].mode).c_str() : "no mode");
        r.add("    properties: %s", propText.c_str());
    }

    for (uint32_t id : res.connectors) {
        Connector conn;
        if (!connector(fd, id, conn)) {
            continue;
        }
        const std::string name = connectorName(conn.info.connector_type, conn.info.connector_type_id);
        const char* status = conn.info.connection == 1 ? "connected" : conn.info.connection == 2 ? "disconnected" : "unknown";
        const std::vector<Property> props = properties(fd, id, DRM_MODE_OBJECT_CONNECTOR);

        // Atomic clients see the CRTC directly; otherwise go through the encoder
        uint32_t crtcId = 0;
        if (const Property* crtcProperty = findProperty(props, "CRTC_ID")) {
            crtcId = uint32_t(crtcProperty->value);
        }
        else if (conn.info.encoder_id != 0) {
            drm_mode_get_encoder encoder{};
            encoder.encoder_id = conn.info.encoder_id;
            if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) == 0) {
                crtcId = encoder.crtc_id;
            }
        }
        const drm_mode_crtc* active = crtcs.count(crtcId) && crtcs[crtcId].mode_valid ? &crtcs[crtcId] : nullptr;

        r.add("  connector %u %s: %s, %ux%u mm, crtc %u, %zu mode(s)", id, name.c_str(), status, conn.info.mm_width,
              conn.info.mm_height, crtcId, conn.modes.size());
        std::string edidLabel;
        for (const Property& property : props) {
            r.add("    %s = %s", property.name.c_str(), valueText(property).c_str());
        }
        double maxRefresh = 0;
        for (const drm_mode_modeinfo& mode : conn.modes) {
            maxRefresh = std::max(maxRefresh, modeRefresh(mode));
            r.add("    mode %s%s", modeText(mode).c_str(), active != nullptr && sameTiming(mode, active->mode) ? " [current]" : "");
        }
        if (conn.info.connection == 1) {
            const Property* vrrCapable = findProperty(props, "vrr_capable");
            const std::vector<Property> crtcProps = crtcId ? properties(fd, crtcId, DRM_MODE_OBJECT_CRTC) : std::vector<Property>();
            const Property* vrrEnabled = findProperty(crtcProps, "VRR_ENABLED");
            r.summary("KMS %s %s: current %s, vrr_capable=%s, crtc %u VRR_ENABLED=%s, highest listed mode %.3f Hz",
                      card.c_str(), name.c_str(),
                      active != nullptr ? format("%ux%u @ %.3f Hz", active->mode.hdisplay, active->mode.vdisplay,
                                                 modeRefresh(active->mode)).c_str() : "no active mode",
                      vrrCapable != nullptr ? std::to_string(vrrCapable->value).c_str() : "-", crtcId,
                      vrrEnabled != nullptr ? std::to_string(vrrEnabled->value).c_str() : "-", maxRefresh);
            if (const Property* edid = findProperty(props, "EDID")) {
                const std::vector<uint8_t> data = blob(fd, uint32_t(edid->value));
                if (!data.empty()) {
                    decodeEdid(r, data, "KMS " + card + " " + name);
                }
            }
        }
    }
    close(fd);
}

}

void probeDrm(Report& r)
{
    r.section("KMS");
    const std::vector<std::string> cards = Drm::cardPaths();
    if (cards.empty()) {
        r.summary("KMS: no /dev/dri/card* node visible");
    }
    for (const std::string& path : cards) {
        Drm::probeCard(r, path);
    }
}

#else

void probeDrm(Report& r)
{
    r.section("KMS");
    r.summary("KMS: built without DRM headers");
}

#endif

// ---------------------------------------------------------------------------
// sysfs, debugfs and Gamescope's own files and process

void probeSysfs(Report& r)
{
    r.section("sysfs");
    const std::string root = "/sys/class/drm/";
    for (const std::string& name : listDirectory(root)) {
        const size_t dash = name.find('-');
        if (name.compare(0, 4, "card") != 0) {
            continue;
        }
        const std::string base = root + name + "/";
        if (dash == std::string::npos) {
            char link[256] = {};
            const ssize_t length = readlink((base + "device/driver").c_str(), link, sizeof(link) - 1);
            std::string driver = length > 0 ? std::string(link, size_t(length)) : std::string("?");
            driver = driver.substr(driver.rfind('/') + 1);
            r.add("  %s: driver %s, vendor %s device %s", name.c_str(), driver.c_str(),
                  fileOrError(base + "device/vendor").c_str(), fileOrError(base + "device/device").c_str());
            continue;
        }
        std::string modes;
        readFile(base + "modes", modes, 4096);
        modes = trim(modes);
        std::replace(modes.begin(), modes.end(), '\n', ' ');
        struct stat edid;
        const bool hasEdid = stat((base + "edid").c_str(), &edid) == 0;
        r.add("  %s: status %s, enabled %s, dpms %s, edid %s, modes: %s", name.c_str(),
              fileOrError(base + "status").c_str(), fileOrError(base + "enabled").c_str(),
              fileOrError(base + "dpms").c_str(), hasEdid ? "present" : "absent", modes.c_str());

        // amdgpu's FreeSync range, root-only on most systems
        const std::string card = name.substr(4, dash - 4);
        const std::string connector = name.substr(dash + 1);
        const std::string vrrRange = "/sys/kernel/debug/dri/" + card + "/" + connector + "/vrr_range";
        std::string range;
        if (readFile(vrrRange, range, 256)) {
            r.summary("debugfs %s vrr_range: %s", name.c_str(), trim(range).c_str());
        }
        else {
            r.add("    %s: %s", vrrRange.c_str(), strerror(errno));
        }
    }
    r.add("  amdgpu freesync_video: %s", fileOrError("/sys/module/amdgpu/parameters/freesync_video").c_str());
}

void probeGamescopeEdid(Report& r, const std::string& path)
{
    r.section("Gamescope EDID");
    if (path.empty()) {
        r.add("  GAMESCOPE_DISPLAY_EDID_PATH not set");
        return;
    }
    std::string data;
    if (!readFile(path, data, 65536)) {
        r.add("  %s: %s", path.c_str(), strerror(errno));
        return;
    }
    r.add("  %s:", path.c_str());
    decodeEdid(r, std::vector<uint8_t>(data.begin(), data.end()), "Gamescope patched");
}

void probeProcesses(Report& r, uint32_t rootPid)
{
    r.section("Gamescope processes");
    for (const std::string& entry : listDirectory("/proc")) {
        if (entry.empty() || entry.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        std::string comm;
        if (!readFile("/proc/" + entry + "/comm", comm, 64) || trim(comm).compare(0, 9, "gamescope") != 0) {
            continue;
        }
        std::string commandLine;
        readFile("/proc/" + entry + "/cmdline", commandLine, 8192);
        std::replace(commandLine.begin(), commandLine.end(), '\0', ' ');
        const bool root = std::to_string(rootPid) == entry;
        r.add("  pid %s%s (%s): %s", entry.c_str(), root ? " [owns DISPLAY]" : "", trim(comm).c_str(), trim(commandLine).c_str());
        if (root) {
            r.summary("Gamescope command line: %s", trim(commandLine).c_str());
        }
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start("gamescope", QStringList() << "--version");
    if (process.waitForStarted(1000) && process.waitForFinished(2000)) {
        const std::string version = trim(process.readAll().toStdString());
        r.summary("gamescope --version: %s", version.substr(0, version.find('\n')).c_str());
    }
    else {
        process.kill();
        process.waitForFinished(500);
        r.add("  gamescope --version: unavailable (%s)", qPrintable(process.errorString()));
    }
}

void probeEnvironment(Report& r)
{
    r.section("Environment");
    static const char* variables[] = {
        "DISPLAY", "WAYLAND_DISPLAY", "GAMESCOPE_WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE",
        "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "QT_QPA_PLATFORM", "SDL_VIDEODRIVER", "SDL_VIDEO_DRIVER",
        "ENABLE_GAMESCOPE_WSI", "DISABLE_GAMESCOPE_WSI", "GAMESCOPE_WSI_FRAME_LIMITER_AWARE", "SteamAppId",
        "SteamGameId", "SteamDeck", "SteamOS", "STEAM_GAMESCOPE_VRR_SUPPORTED", "STEAM_DISPLAY_REFRESH_LIMITS",
        "STEAM_GAMESCOPE_HAS_TEARING_SUPPORT", "STEAM_GAMESCOPE_DYNAMIC_REFRESH_IN_STEAM_SUPPORTED",
        "STEAM_MULTIPLE_XWAYLANDS", "VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_INSTANCE_LAYERS",
        "VK_LOADER_LAYERS_ENABLE", "VK_LOADER_LAYERS_DISABLE", "MANGOHUD", "FLATPAK_ID", "APPIMAGE",
        "MOONLIGHT_DISPLAY_PROBE", "MOONLIGHT_DISPLAY_PROBE_SECONDS",
    };
    for (const char* variable : variables) {
        const char* value = getenv(variable);
        r.add("  %s=%s", variable, value != nullptr ? value : "(unset)");
    }
    // Steam's own capability hints for Gamescope sessions
    for (char** env = environ; env != nullptr && *env != nullptr; env++) {
        if (std::strncmp(*env, "STEAM_GAMESCOPE", 15) == 0 || std::strncmp(*env, "GAMESCOPE_", 10) == 0) {
            r.add("  [all] %s", *env);
        }
    }

    utsname name;
    if (uname(&name) == 0) {
        r.add("  kernel %s %s", name.release, name.version);
    }
    std::string osRelease;
    if (readFile("/run/host/os-release", osRelease) || readFile("/etc/os-release", osRelease)) {
        std::string pretty, build;
        size_t start = 0;
        while (start < osRelease.size()) {
            const size_t end = osRelease.find('\n', start);
            const std::string line = osRelease.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (line.compare(0, 12, "PRETTY_NAME=") == 0) pretty = line.substr(12);
            if (line.compare(0, 9, "BUILD_ID=") == 0) build = line.substr(9);
            if (line.compare(0, 11, "VERSION_ID=") == 0 && build.empty()) build = line.substr(11);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        r.summary("OS %s %s, kernel %s%s", pretty.c_str(), build.c_str(), uname(&name) == 0 ? name.release : "?",
                  access("/.flatpak-info", F_OK) == 0 ? ", Flatpak" : (getenv("APPIMAGE") ? ", AppImage" : ""));
    }

    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr) {
        std::string sockets;
        for (const std::string& entry : listDirectory(runtime)) {
            struct stat info;
            if (stat((std::string(runtime) + "/" + entry).c_str(), &info) == 0 && S_ISSOCK(info.st_mode)) {
                sockets += " " + entry;
            }
        }
        r.add("  sockets in XDG_RUNTIME_DIR:%s", sockets.c_str());
    }
    std::string x11Sockets;
    for (const std::string& entry : listDirectory("/tmp/.X11-unix")) {
        x11Sockets += " " + entry;
    }
    r.add("  /tmp/.X11-unix:%s", x11Sockets.c_str());
}

// ---------------------------------------------------------------------------
// What Moonlight itself sees

void probeSdl(Report& r, SDL_Window* window)
{
    r.section("SDL");
    const bool wasInitialized = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    if (!wasInitialized && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        r.summary("SDL: video init failed: %s", SDL_GetError());
        return;
    }
    r.add("  video driver %s", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");
    for (int display = 0; display < SDL_GetNumVideoDisplays(); display++) {
        SDL_DisplayMode current{}, desktop{};
        const int currentResult = SDL_GetCurrentDisplayMode(display, &current);
        const int desktopResult = SDL_GetDesktopDisplayMode(display, &desktop);
        int highest = 0;
        std::string modes;
        for (int i = 0; i < SDL_GetNumDisplayModes(display); i++) {
            SDL_DisplayMode mode{};
            if (SDL_GetDisplayMode(display, i, &mode) == 0) {
                highest = std::max(highest, mode.refresh_rate);
                modes += format(" %dx%d@%d", mode.w, mode.h, mode.refresh_rate);
            }
        }
        r.summary("SDL (%s) display %d '%s': current %dx%d@%d%s, desktop %dx%d@%d%s, highest listed %d Hz",
                  SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?", display,
                  SDL_GetDisplayName(display) ? SDL_GetDisplayName(display) : "", current.w, current.h,
                  current.refresh_rate, currentResult == 0 ? "" : " (failed)", desktop.w, desktop.h,
                  desktop.refresh_rate, desktopResult == 0 ? "" : " (failed)", highest);
        r.add("    modes:%s", modes.c_str());
    }
    if (window != nullptr) {
        SDL_DisplayMode windowMode{};
        const int windowResult = SDL_GetWindowDisplayMode(window, &windowMode);
        int refresh = 0;
        const bool known = StreamUtils::tryGetDisplayRefreshRate(window, refresh);
        r.summary("SDL stream window: display %d, flags 0x%x, window mode %dx%d@%d%s, refresh Moonlight uses %s",
                  SDL_GetWindowDisplayIndex(window), SDL_GetWindowFlags(window), windowMode.w, windowMode.h,
                  windowMode.refresh_rate, windowResult == 0 ? "" : " (failed)",
                  known ? std::to_string(refresh).c_str() : "unknown");
    }
    if (!wasInitialized) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

void probeQt(Report& r)
{
    r.section("Qt");
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        r.add("  no QGuiApplication");
        return;
    }
    r.add("  platform %s", qPrintable(QGuiApplication::platformName()));
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect geometry = screen->geometry();
        r.summary("Qt (%s) screen '%s'%s: %dx%d @ %.3f Hz, make '%s' model '%s', %.0fx%.0f mm, dpr %.2f",
                  qPrintable(QGuiApplication::platformName()), qPrintable(screen->name()),
                  screen == QGuiApplication::primaryScreen() ? " (primary)" : "", geometry.width(),
                  geometry.height(), screen->refreshRate(), qPrintable(screen->manufacturer()),
                  qPrintable(screen->model()), screen->physicalSize().width(), screen->physicalSize().height(),
                  screen->devicePixelRatio());
    }
}

void collectPassive(Report& r, SDL_Window* window)
{
    uint32_t gamescopePid = 0;
    std::string edidPath;
    probeEnvironment(r);
    probeSdl(r, window);
    probeQt(r);
    probeX11(r, gamescopePid, edidPath);
    probeProcesses(r, gamescopePid);
    probeWayland(r);
    probeDrm(r);
    probeSysfs(r);
    probeGamescopeEdid(r, edidPath);
}

// Gamescope's root properties and the KMS state, cheap enough to poll
class StateReader
{
public:
    StateReader()
    {
#ifdef HAS_X11
        std::vector<XServer> servers = openXServers();
        if (const XServer* root = gamescopeRootServer(servers)) {
            m_XName = root->name;
            m_Display = root->display;
            for (XServer& server : servers) {
                if (server.display == m_Display) {
                    server.display = nullptr;
                }
            }
        }
        closeXServers(servers);
#endif
#ifdef DISPLAYPROBE_HAS_DRM
        for (const std::string& path : Drm::cardPaths()) {
            const int fd = Drm::openCard(path);
            if (fd >= 0) {
                m_Cards.emplace_back(path.substr(path.rfind('/') + 1), fd);
            }
        }
#endif
    }

    ~StateReader()
    {
#ifdef HAS_X11
        if (m_Display != nullptr) {
            XCloseDisplay(m_Display);
        }
#endif
        for (const auto& card : m_Cards) {
            close(card.second);
        }
    }

    StateReader(const StateReader&) = delete;
    StateReader& operator=(const StateReader&) = delete;

    std::string line()
    {
        std::string text;
#ifdef HAS_X11
        text = m_Display != nullptr ? "gamescope " + m_XName + ": " + gamescopeStateLine(m_Display)
                                    : std::string("gamescope: no root server");
#endif
#ifdef DISPLAYPROBE_HAS_DRM
        for (const auto& card : m_Cards) {
            text += " | kms " + Drm::activeState(card.second, card.first);
        }
#endif
        return text;
    }

private:
#ifdef HAS_X11
    Display* m_Display = nullptr;
#endif
    std::string m_XName;
    std::vector<std::pair<std::string, int>> m_Cards;
};

QString reportDirectory()
{
    QStringList candidates;
    const QString override = qEnvironmentVariable("MOONLIGHT_DISPLAY_PROBE_DIR");
    if (!override.isEmpty()) {
        candidates << override;
    }
    candidates << QDir::homePath() + "/moonlight-display-probe";
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!appData.isEmpty()) {
        candidates << appData + "/display-probe";
    }
    candidates << Path::getLogDir();
    for (const QString& candidate : candidates) {
        if (QDir().mkpath(candidate) && QFileInfo(candidate).isWritable()) {
            return candidate;
        }
    }
    return QString();
}

// Beside the VRR trace, named like it: Moonlight.vrrtrace in a diagnostic
// capture gives Moonlight.display-stream.txt. Empty when tracing is off.
// Settings' capture sets the variable at session start, after startup.
QString tracePathFor(const char* reason)
{
    QString base = qEnvironmentVariable("MOONLIGHT_VRR_TRACE");
    if (base.isEmpty()) {
        return QString();
    }
    if (base.endsWith(QStringLiteral(".vrrtrace"))) {
        base.chop(int(sizeof(".vrrtrace") - 1));
    }
    QString path = QString("%1.display-%2.txt").arg(base, reason);
    if (QFileInfo::exists(path)) {
        // A later stream in the same process keeps the earlier report
        path = QString("%1.display-%2-%3.txt").arg(base, reason).arg(QDateTime::currentSecsSinceEpoch());
    }
    return path;
}

QString writeReport(const Report& r, const char* reason, QString path)
{
    if (!path.isEmpty()) {
        FILE* file = fopen(QFile::encodeName(path).constData(), "w");
        if (file != nullptr) {
            const std::string text = r.text(reason);
            fwrite(text.data(), 1, text.size(), file);
            fclose(file);
        }
        else {
            path.clear();
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Display probe (%s): report %s", reason,
                path.isEmpty() ? "could not be written" : qPrintable(path));
    for (const std::string& line : r.summaries()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Display probe: %s", line.c_str());
    }
    return path;
}

// ---------------------------------------------------------------------------
// Active Vulkan test: what present modes exist on Gamescope's surface, and at
// what intervals Gamescope actually displays frames in each

#ifdef DISPLAYPROBE_HAS_VULKAN

namespace Vk {

struct Functions {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance CreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
    PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceDisplayPropertiesKHR GetPhysicalDeviceDisplayPropertiesKHR = nullptr;
    PFN_vkGetDisplayModePropertiesKHR GetDisplayModePropertiesKHR = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;

    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkGetRefreshCycleDurationGOOGLE GetRefreshCycleDurationGOOGLE = nullptr;
    PFN_vkGetPastPresentationTimingGOOGLE GetPastPresentationTimingGOOGLE = nullptr;
};

const char* presentModeName(VkPresentModeKHR mode)
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "OTHER";
    }
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

struct IntervalStats {
    size_t count = 0;
    double p50 = 0;
    std::string fits;
    double pacingFit = -1;
};

// Prints a distribution of millisecond intervals. "fit" is the share that
// lands on a multiple of each fixed refresh period, so a fixed 120 Hz display
// shows 120:1.00 (and 60:~0.5); an adaptive one tracks the pacing instead.
IntervalStats intervals(Report& r, const char* label, std::vector<double> values, double pacingMs)
{
    IntervalStats stats;
    stats.count = values.size();
    if (values.empty()) {
        r.add("      %s: none", label);
        stats.fits = " n/a";
        return stats;
    }
    std::sort(values.begin(), values.end());
    auto percentile = [&](double q) {
        return values[std::min(values.size() - 1, size_t(q * double(values.size() - 1) + 0.5))];
    };
    double sum = 0;
    for (double value : values) {
        sum += value;
    }
    stats.p50 = percentile(0.5);
    r.add("      %s: n=%zu mean=%.3f min=%.3f p5=%.3f p50=%.3f p95=%.3f max=%.3f ms", label, values.size(),
          sum / double(values.size()), values.front(), percentile(0.05), stats.p50, percentile(0.95), values.back());

    std::map<long, int> buckets;
    for (double value : values) {
        buckets[lround(value * 4)]++;
    }
    std::vector<std::pair<int, long>> ranked;
    for (const auto& bucket : buckets) {
        ranked.emplace_back(bucket.second, bucket.first);
    }
    std::sort(ranked.rbegin(), ranked.rend());
    std::string histogram;
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 10); i++) {
        histogram += format(" %.2fms x%d", ranked[i].second / 4.0, ranked[i].first);
    }
    r.add("        most common (0.25 ms buckets):%s", histogram.c_str());

    static const int rates[] = { 60, 90, 100, 120, 144, 165, 240 };
    for (int rate : rates) {
        const double period = 1000.0 / rate;
        const double tolerance = std::min(0.5, period * 0.06);
        size_t hits = 0;
        for (double value : values) {
            const long multiple = lround(value / period);
            if (multiple >= 1 && std::fabs(value - double(multiple) * period) <= tolerance) {
                hits++;
            }
        }
        stats.fits += format(" %d:%.2f", rate, double(hits) / double(values.size()));
    }
    std::string pacing;
    if (pacingMs > 0) {
        size_t hits = 0;
        for (double value : values) {
            hits += std::fabs(value - pacingMs) <= 1.0 ? 1 : 0;
        }
        stats.pacingFit = double(hits) / double(values.size());
        pacing = format(", within 1 ms of the %.2f ms pacing: %.2f", pacingMs, stats.pacingFit);
    }
    r.add("        fixed-refresh grid fit:%s%s", stats.fits.c_str(), pacing.c_str());
    return stats;
}

class Test
{
public:
    Test(Report& report, SDL_Window* window) : r(report), m_Window(window) {}

    ~Test()
    {
        if (m_Device != VK_NULL_HANDLE) {
            f.DeviceWaitIdle(m_Device);
            destroySwapchain();
            for (int i = 0; i < FramesInFlight; i++) {
                if (m_Fences[i]) f.DestroyFence(m_Device, m_Fences[i], nullptr);
                if (m_AcquireSemaphores[i]) f.DestroySemaphore(m_Device, m_AcquireSemaphores[i], nullptr);
            }
            if (m_Pool) f.DestroyCommandPool(m_Device, m_Pool, nullptr);
            f.DestroyDevice(m_Device, nullptr);
        }
        if (m_Surface != VK_NULL_HANDLE) {
            f.DestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        }
        if (m_Instance != VK_NULL_HANDLE) {
            f.DestroyInstance(m_Instance, nullptr);
        }
    }

    void run(int seconds)
    {
        if (!createInstance() || !choosePhysicalDevice() || !createDevice()) {
            return;
        }
        StateReader state;
        r.add("  state before presenting: %s", state.line().c_str());
        static const VkPresentModeKHR order[] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                                                  VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR };
        // Neither rate sits on a multiple of a common fixed refresh period,
        // and both stay above the usual 48 Hz VRR floor
        static const int pacings[] = { 0, 53, 77 };
        for (VkPresentModeKHR mode : order) {
            if (std::find(m_PresentModes.begin(), m_PresentModes.end(), mode) == m_PresentModes.end()) {
                continue;
            }
            if (!createSwapchain(mode)) {
                continue;
            }
            for (int pacing : pacings) {
                // An out-of-date swapchain that could not be recreated ends the mode
                if (m_Swapchain == VK_NULL_HANDLE) {
                    break;
                }
                runPhase(mode, pacing, seconds, state);
            }
            f.DeviceWaitIdle(m_Device);
            destroySwapchain();
        }
        r.add("  state after presenting: %s", state.line().c_str());
    }

private:
    static constexpr int FramesInFlight = 2;

    bool check(VkResult result, const char* what)
    {
        if (result != VK_SUCCESS) {
            r.summary("Vulkan: %s failed (%d)", what, result);
            return false;
        }
        return true;
    }

    bool createInstance()
    {
        f.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (f.GetInstanceProcAddr == nullptr) {
            r.summary("Vulkan: no loader (%s)", SDL_GetError());
            return false;
        }
#define GLOBAL(name) f.name = reinterpret_cast<PFN_vk##name>(f.GetInstanceProcAddr(VK_NULL_HANDLE, "vk" #name))
        GLOBAL(CreateInstance);
        GLOBAL(EnumerateInstanceExtensionProperties);
        GLOBAL(EnumerateInstanceLayerProperties);
#undef GLOBAL
        uint32_t count = 0;
        f.EnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        f.EnumerateInstanceLayerProperties(&count, layers.data());
        for (const VkLayerProperties& layer : layers) {
            r.add("  instance layer %s: %s", layer.layerName, layer.description);
        }
        count = 0;
        f.EnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        f.EnumerateInstanceExtensionProperties(nullptr, &count, available.data());

        unsigned int sdlCount = 0;
        SDL_Vulkan_GetInstanceExtensions(m_Window, &sdlCount, nullptr);
        std::vector<const char*> extensions(sdlCount);
        SDL_Vulkan_GetInstanceExtensions(m_Window, &sdlCount, extensions.data());
        m_HasDisplay = hasExtension(available, VK_KHR_DISPLAY_EXTENSION_NAME);
        if (m_HasDisplay) {
            extensions.push_back(VK_KHR_DISPLAY_EXTENSION_NAME);
        }
        std::string names;
        for (const char* name : extensions) {
            names += std::string(" ") + name;
        }
        r.add("  instance extensions enabled:%s", names.c_str());

        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Moonlight display probe";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        if (!check(f.CreateInstance(&info, nullptr, &m_Instance), "vkCreateInstance")) {
            return false;
        }
#define INSTANCE(name) f.name = reinterpret_cast<PFN_vk##name>(f.GetInstanceProcAddr(m_Instance, "vk" #name))
        INSTANCE(DestroyInstance);
        INSTANCE(EnumeratePhysicalDevices);
        INSTANCE(GetPhysicalDeviceProperties);
        INSTANCE(GetPhysicalDeviceProperties2);
        INSTANCE(EnumerateDeviceExtensionProperties);
        INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
        INSTANCE(GetPhysicalDeviceSurfaceSupportKHR);
        INSTANCE(GetPhysicalDeviceSurfaceCapabilitiesKHR);
        INSTANCE(GetPhysicalDeviceSurfaceFormatsKHR);
        INSTANCE(GetPhysicalDeviceSurfacePresentModesKHR);
        INSTANCE(DestroySurfaceKHR);
        INSTANCE(GetPhysicalDeviceDisplayPropertiesKHR);
        INSTANCE(GetDisplayModePropertiesKHR);
        INSTANCE(CreateDevice);
        INSTANCE(GetDeviceProcAddr);
#undef INSTANCE
        if (!SDL_Vulkan_CreateSurface(m_Window, m_Instance, &m_Surface)) {
            r.summary("Vulkan: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            return false;
        }
        return true;
    }

    void probeDisplays(VkPhysicalDevice device)
    {
        if (!m_HasDisplay || f.GetPhysicalDeviceDisplayPropertiesKHR == nullptr) {
            r.add("    VK_KHR_display: unavailable");
            return;
        }
        uint32_t count = 0;
        const VkResult result = f.GetPhysicalDeviceDisplayPropertiesKHR(device, &count, nullptr);
        std::vector<VkDisplayPropertiesKHR> displays(count);
        if (count > 0) {
            f.GetPhysicalDeviceDisplayPropertiesKHR(device, &count, displays.data());
        }
        if (count == 0) {
            r.summary("Vulkan VK_KHR_display: no displays (result %d)", result);
        }
        for (const VkDisplayPropertiesKHR& display : displays) {
            uint32_t modeCount = 0;
            f.GetDisplayModePropertiesKHR(device, display.display, &modeCount, nullptr);
            std::vector<VkDisplayModePropertiesKHR> modes(modeCount);
            f.GetDisplayModePropertiesKHR(device, display.display, &modeCount, modes.data());
            uint32_t highest = 0;
            std::string list;
            for (const VkDisplayModePropertiesKHR& mode : modes) {
                highest = std::max(highest, mode.parameters.refreshRate);
                list += format(" %ux%u@%.3f", mode.parameters.visibleRegion.width, mode.parameters.visibleRegion.height,
                               mode.parameters.refreshRate / 1000.0);
            }
            r.summary("Vulkan VK_KHR_display '%s': %ux%u native, %u mode(s), highest %.3f Hz",
                      display.displayName ? display.displayName : "", display.physicalResolution.width,
                      display.physicalResolution.height, modeCount, highest / 1000.0);
            r.add("      modes:%s", list.c_str());
        }
    }

    bool choosePhysicalDevice()
    {
        uint32_t count = 0;
        f.EnumeratePhysicalDevices(m_Instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        f.EnumeratePhysicalDevices(m_Instance, &count, devices.data());
        static const char* interesting[] = {
            VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, "VK_EXT_present_timing", "VK_KHR_present_id",
            "VK_KHR_present_wait", "VK_KHR_present_id2", "VK_KHR_present_wait2", "VK_EXT_swapchain_maintenance1",
            "VK_KHR_swapchain_maintenance1", "VK_EXT_display_control", "VK_KHR_display_swapchain",
            "VK_EXT_hdr_metadata", "VK_AMD_display_native_hdr", "VK_AMD_anti_lag",
        };
        for (VkPhysicalDevice device : devices) {
            VkPhysicalDeviceProperties properties{};
            f.GetPhysicalDeviceProperties(device, &properties);
            std::string driver;
            if (f.GetPhysicalDeviceProperties2 != nullptr && properties.apiVersion >= VK_API_VERSION_1_2) {
                VkPhysicalDeviceDriverProperties driverProperties{};
                driverProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
                VkPhysicalDeviceProperties2 properties2{};
                properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                properties2.pNext = &driverProperties;
                f.GetPhysicalDeviceProperties2(device, &properties2);
                driver = format("%s %s", driverProperties.driverName, driverProperties.driverInfo);
            }
            uint32_t extensionCount = 0;
            f.EnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
            std::vector<VkExtensionProperties> extensions(extensionCount);
            f.EnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
            std::string present;
            for (const char* name : interesting) {
                present += format(" %s%s", hasExtension(extensions, name) ? "+" : "-", name);
            }
            r.summary("Vulkan device '%s' (%04x:%04x, API %u.%u.%u, %s)", properties.deviceName, properties.vendorID,
                      properties.deviceID, VK_VERSION_MAJOR(properties.apiVersion),
                      VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion), driver.c_str());
            r.add("    extensions:%s", present.c_str());
            probeDisplays(device);

            uint32_t familyCount = 0;
            f.GetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            f.GetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
            for (uint32_t family = 0; family < familyCount; family++) {
                VkBool32 supported = VK_FALSE;
                f.GetPhysicalDeviceSurfaceSupportKHR(device, family, m_Surface, &supported);
                if (m_PhysicalDevice == VK_NULL_HANDLE && supported && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    m_PhysicalDevice = device;
                    m_Family = family;
                    m_HasTiming = hasExtension(extensions, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
                }
            }
        }
        if (m_PhysicalDevice == VK_NULL_HANDLE) {
            r.summary("Vulkan: no device can present to the probe window");
            return false;
        }

        uint32_t modeCount = 0;
        f.GetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &modeCount, nullptr);
        m_PresentModes.resize(modeCount);
        f.GetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &modeCount, m_PresentModes.data());
        std::string modes;
        for (VkPresentModeKHR mode : m_PresentModes) {
            modes += std::string(" ") + presentModeName(mode);
        }
        uint32_t formatCount = 0;
        f.GetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        f.GetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, formats.data());
        if (formats.empty()) {
            r.summary("Vulkan: surface reports no formats");
            return false;
        }
        m_Format = formats[0];
        VkSurfaceCapabilitiesKHR caps{};
        f.GetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &caps);
        r.summary("Vulkan surface: present modes%s; images %u-%u, extent %ux%u, %u format(s), display timing %s",
                  modes.c_str(), caps.minImageCount, caps.maxImageCount, caps.currentExtent.width,
                  caps.currentExtent.height, formatCount, m_HasTiming ? "yes" : "no");
        return true;
    }

    bool createDevice()
    {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = m_Family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        if (m_HasTiming) {
            extensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
        }
        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        info.enabledExtensionCount = uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        if (!check(f.CreateDevice(m_PhysicalDevice, &info, nullptr, &m_Device), "vkCreateDevice")) {
            return false;
        }
#define DEVICE(name) f.name = reinterpret_cast<PFN_vk##name>(f.GetDeviceProcAddr(m_Device, "vk" #name))
        DEVICE(DestroyDevice);
        DEVICE(GetDeviceQueue);
        DEVICE(CreateSwapchainKHR);
        DEVICE(DestroySwapchainKHR);
        DEVICE(GetSwapchainImagesKHR);
        DEVICE(AcquireNextImageKHR);
        DEVICE(QueuePresentKHR);
        DEVICE(CreateCommandPool);
        DEVICE(DestroyCommandPool);
        DEVICE(AllocateCommandBuffers);
        DEVICE(BeginCommandBuffer);
        DEVICE(EndCommandBuffer);
        DEVICE(ResetCommandBuffer);
        DEVICE(CmdPipelineBarrier);
        DEVICE(CmdClearColorImage);
        DEVICE(QueueSubmit);
        DEVICE(CreateFence);
        DEVICE(DestroyFence);
        DEVICE(WaitForFences);
        DEVICE(ResetFences);
        DEVICE(CreateSemaphore);
        DEVICE(DestroySemaphore);
        DEVICE(DeviceWaitIdle);
        if (m_HasTiming) {
            DEVICE(GetRefreshCycleDurationGOOGLE);
            DEVICE(GetPastPresentationTimingGOOGLE);
        }
#undef DEVICE
        f.GetDeviceQueue(m_Device, m_Family, 0, &m_Queue);

        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = m_Family;
        if (!check(f.CreateCommandPool(m_Device, &pool, nullptr, &m_Pool), "vkCreateCommandPool")) {
            return false;
        }
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = m_Pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = FramesInFlight;
        if (!check(f.AllocateCommandBuffers(m_Device, &allocate, m_Commands), "vkAllocateCommandBuffers")) {
            return false;
        }
        for (int i = 0; i < FramesInFlight; i++) {
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo semaphore{};
            semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (!check(f.CreateFence(m_Device, &fence, nullptr, &m_Fences[i]), "vkCreateFence") ||
                    !check(f.CreateSemaphore(m_Device, &semaphore, nullptr, &m_AcquireSemaphores[i]), "vkCreateSemaphore")) {
                return false;
            }
        }
        return true;
    }

    bool createSwapchain(VkPresentModeKHR mode)
    {
        VkSurfaceCapabilitiesKHR caps{};
        f.GetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &caps);
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            int width = 0, height = 0;
            SDL_Vulkan_GetDrawableSize(m_Window, &width, &height);
            extent.width = uint32_t(std::max(width, 1));
            extent.height = uint32_t(std::max(height, 1));
        }
        uint32_t images = std::max<uint32_t>(caps.minImageCount, 3);
        if (caps.maxImageCount != 0) {
            images = std::min(images, caps.maxImageCount);
        }
        m_CanClear = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = m_Surface;
        info.minImageCount = images;
        info.imageFormat = m_Format.format;
        info.imageColorSpace = m_Format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (m_CanClear ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0);
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                  ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                  : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
        info.presentMode = mode;
        info.clipped = VK_TRUE;
        if (!check(f.CreateSwapchainKHR(m_Device, &info, nullptr, &m_Swapchain), "vkCreateSwapchainKHR")) {
            m_Swapchain = VK_NULL_HANDLE;
            return false;
        }
        uint32_t count = 0;
        f.GetSwapchainImagesKHR(m_Device, m_Swapchain, &count, nullptr);
        m_Images.resize(count);
        f.GetSwapchainImagesKHR(m_Device, m_Swapchain, &count, m_Images.data());
        m_RenderSemaphores.resize(count, VK_NULL_HANDLE);
        for (VkSemaphore& semaphore : m_RenderSemaphores) {
            VkSemaphoreCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            f.CreateSemaphore(m_Device, &create, nullptr, &semaphore);
        }
        m_Extent = extent;
        return true;
    }

    void destroySwapchain()
    {
        for (VkSemaphore semaphore : m_RenderSemaphores) {
            if (semaphore) f.DestroySemaphore(m_Device, semaphore, nullptr);
        }
        m_RenderSemaphores.clear();
        m_Images.clear();
        if (m_Swapchain != VK_NULL_HANDLE) {
            f.DestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
            m_Swapchain = VK_NULL_HANDLE;
        }
    }

    // Keeps a fence that was reset but not submitted from blocking forever
    void signalFence(int slot)
    {
        f.QueueSubmit(m_Queue, 0, nullptr, m_Fences[slot]);
    }

    void record(VkCommandBuffer command, VkImage image, uint32_t frame)
    {
        f.ResetCommandBuffer(command, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        f.BeginCommandBuffer(command, &begin);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (m_CanClear) {
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            f.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
            // A slow ramp, so every present carries new content
            VkClearColorValue color{};
            const float level = 0.04f + 0.04f * float(frame % 240) / 240.0f;
            color.float32[0] = color.float32[1] = color.float32[2] = level;
            color.float32[3] = 1.0f;
            f.CmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                                 &barrier.subresourceRange);
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.dstAccessMask = 0;
        f.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        f.EndCommandBuffer(command);
    }

    void collectTimings()
    {
        if (f.GetPastPresentationTimingGOOGLE == nullptr) {
            return;
        }
        uint32_t count = 0;
        if (f.GetPastPresentationTimingGOOGLE(m_Device, m_Swapchain, &count, nullptr) != VK_SUCCESS || count == 0) {
            return;
        }
        std::vector<VkPastPresentationTimingGOOGLE> timings(count);
        if (f.GetPastPresentationTimingGOOGLE(m_Device, m_Swapchain, &count, timings.data()) >= 0) {
            for (uint32_t i = 0; i < count && i < timings.size(); i++) {
                m_Timings[timings[i].presentID] = timings[i];
            }
        }
    }

    void runPhase(VkPresentModeKHR mode, int pacingHz, int seconds, StateReader& state)
    {
        m_Timings.clear();
        std::unordered_map<uint32_t, uint64_t> presentedAt;
        std::vector<uint64_t> cpuPresents;
        const uint64_t period = pacingHz > 0 ? 1000000000ull / uint64_t(pacingHz) : 0;
        const uint64_t start = nowNs();
        const uint64_t end = start + uint64_t(seconds) * 1000000000ull;
        uint64_t next = start;
        uint32_t frame = 0, timeouts = 0, outOfDate = 0, failures = 0;
        std::string midState;

        while (nowNs() < end && failures == 0) {
            if (period != 0) {
                next += period;
                timespec target = { time_t(next / 1000000000ull), long(next % 1000000000ull) };
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr);
                if (nowNs() > next + period) {
                    next = nowNs();
                }
            }
            SDL_PumpEvents();
            const int slot = int(frame % FramesInFlight);
            f.WaitForFences(m_Device, 1, &m_Fences[slot], VK_TRUE, 1000000000ull);
            f.ResetFences(m_Device, 1, &m_Fences[slot]);
            uint32_t index = 0;
            const VkResult acquired = f.AcquireNextImageKHR(m_Device, m_Swapchain, 1000000000ull,
                                                            m_AcquireSemaphores[slot], VK_NULL_HANDLE, &index);
            if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) {
                timeouts++;
                signalFence(slot);
                continue;
            }
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
                outOfDate++;
                signalFence(slot);
                f.DeviceWaitIdle(m_Device);
                destroySwapchain();
                if (!createSwapchain(mode)) {
                    failures++;
                }
                continue;
            }
            if (acquired < 0) {
                failures++;
                signalFence(slot);
                break;
            }
            record(m_Commands[slot], m_Images[index], frame);
            const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &m_AcquireSemaphores[slot];
            submit.pWaitDstStageMask = &waitStage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &m_Commands[slot];
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &m_RenderSemaphores[index];
            if (f.QueueSubmit(m_Queue, 1, &submit, m_Fences[slot]) != VK_SUCCESS) {
                failures++;
                break;
            }

            const uint32_t id = m_NextPresentId++;
            VkPresentTimeGOOGLE presentTime{ id, 0 };
            VkPresentTimesInfoGOOGLE times{};
            times.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
            times.swapchainCount = 1;
            times.pTimes = &presentTime;
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.pNext = m_HasTiming ? &times : nullptr;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &m_RenderSemaphores[index];
            present.swapchainCount = 1;
            present.pSwapchains = &m_Swapchain;
            present.pImageIndices = &index;
            const VkResult presented = f.QueuePresentKHR(m_Queue, &present);
            const uint64_t after = nowNs();
            presentedAt[id] = after;
            cpuPresents.push_back(after);
            frame++;
            if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
                outOfDate++;
                f.DeviceWaitIdle(m_Device);
                destroySwapchain();
                if (!createSwapchain(mode)) {
                    failures++;
                }
                continue;
            }
            if (presented < 0) {
                failures++;
                break;
            }
            collectTimings();
            if (midState.empty() && after >= start + (end - start) / 2) {
                midState = state.line();
            }
        }
        f.DeviceWaitIdle(m_Device);
        SDL_Delay(200);
        if (m_Swapchain != VK_NULL_HANDLE) {
            collectTimings();
        }

        uint64_t refreshNs = 0;
        if (f.GetRefreshCycleDurationGOOGLE != nullptr && m_Swapchain != VK_NULL_HANDLE) {
            VkRefreshCycleDurationGOOGLE refresh{};
            if (f.GetRefreshCycleDurationGOOGLE(m_Device, m_Swapchain, &refresh) == VK_SUCCESS) {
                refreshNs = refresh.refreshDuration;
            }
        }
        const std::string pacing = pacingHz > 0 ? format("paced %d fps", pacingHz) : std::string("unpaced");
        r.add("    [%s, %s] %ux%u, %u presents, %zu with display times, refresh cycle %s, timeouts %u, out of date %u, failures %u",
              presentModeName(mode), pacing.c_str(), m_Extent.width, m_Extent.height, frame, m_Timings.size(),
              refreshNs ? format("%.3f ms (%.2f Hz)", refreshNs / 1e6, 1e9 / double(refreshNs)).c_str() : "unknown",
              timeouts, outOfDate, failures);
        r.add("      Gamescope and KMS mid-phase: %s", midState.c_str());

        std::vector<double> cpu;
        for (size_t i = 1; i < cpuPresents.size(); i++) {
            cpu.push_back((cpuPresents[i] - cpuPresents[i - 1]) / 1e6);
        }
        intervals(r, "CPU present interval", cpu, period / 1e6);

        std::vector<uint64_t> displayed;
        std::vector<double> latency;
        for (const auto& timing : m_Timings) {
            if (timing.second.actualPresentTime != 0) {
                displayed.push_back(timing.second.actualPresentTime);
                const auto submitted = presentedAt.find(timing.first);
                if (submitted != presentedAt.end()) {
                    latency.push_back((double(timing.second.actualPresentTime) - double(submitted->second)) / 1e6);
                }
            }
        }
        std::sort(displayed.begin(), displayed.end());
        displayed.erase(std::unique(displayed.begin(), displayed.end()), displayed.end());
        std::vector<double> shown;
        for (size_t i = 1; i < displayed.size(); i++) {
            shown.push_back((displayed[i] - displayed[i - 1]) / 1e6);
        }
        const IntervalStats stats = intervals(r, "display interval", shown, period / 1e6);
        intervals(r, "present return to display", latency, 0);
        r.summary("Vulkan %s %s: %u presents, %zu displayed, display interval p50 %.3f ms, grid fit%s%s, refresh cycle %s",
                  presentModeName(mode), pacing.c_str(), frame, displayed.size(), stats.p50, stats.fits.c_str(),
                  stats.pacingFit >= 0 ? format(", pacing fit %.2f", stats.pacingFit).c_str() : "",
                  refreshNs ? format("%.2f Hz", 1e9 / double(refreshNs)).c_str() : "unknown");
        r.summary("  mid-phase state: %s", midState.c_str());
    }

    Report& r;
    SDL_Window* m_Window;
    Functions f;
    VkInstance m_Instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    uint32_t m_Family = 0;
    bool m_HasDisplay = false;
    bool m_HasTiming = false;
    bool m_CanClear = false;
    std::vector<VkPresentModeKHR> m_PresentModes;
    VkSurfaceFormatKHR m_Format{};
    VkDevice m_Device = VK_NULL_HANDLE;
    VkQueue m_Queue = VK_NULL_HANDLE;
    VkCommandPool m_Pool = VK_NULL_HANDLE;
    VkCommandBuffer m_Commands[FramesInFlight] = {};
    VkFence m_Fences[FramesInFlight] = {};
    VkSemaphore m_AcquireSemaphores[FramesInFlight] = {};
    VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
    VkExtent2D m_Extent{};
    std::vector<VkImage> m_Images;
    std::vector<VkSemaphore> m_RenderSemaphores;
    std::map<uint32_t, VkPastPresentationTimingGOOGLE> m_Timings;
    uint32_t m_NextPresentId = 1;
};

}

void probeVulkanActive(Report& r, int seconds)
{
    const bool wasInitialized = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    if (!wasInitialized && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        r.section("Vulkan presentation test");
        r.summary("Vulkan test: SDL video init failed: %s", SDL_GetError());
        return;
    }
    SDL_Window* window = SDL_CreateWindow("Moonlight display probe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN_DESKTOP |
                                          SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == nullptr) {
        r.section("Vulkan presentation test");
        r.summary("Vulkan test: SDL_CreateWindow failed: %s", SDL_GetError());
    }
    else {
        // Let Gamescope map and focus the window before measuring
        const Uint32 shown = SDL_GetTicks();
        while (SDL_GetTicks() - shown < 1500) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {}
            SDL_Delay(10);
        }
        probeSdl(r, window);
        r.section("Vulkan presentation test");
        {
            Vk::Test test(r, window);
            test.run(seconds);
        }
        SDL_DestroyWindow(window);
    }
    if (!wasInitialized) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

#else

void probeVulkanActive(Report& r, int)
{
    r.section("Vulkan presentation test");
    r.summary("Vulkan test: built without Vulkan support");
}

#endif

}

QString DisplayProbe::writePassiveReport(const char* reason, SDL_Window* window)
{
    // Only while VRR tracing is on, so an ordinary session pays nothing
    const QString path = tracePathFor(reason);
    if (path.isEmpty()) {
        return QString();
    }
    Report r;
    collectPassive(r, window);
    return writeReport(r, reason, path);
}

int DisplayProbe::runActiveProbe()
{
    int seconds = qEnvironmentVariableIntValue("MOONLIGHT_DISPLAY_PROBE_SECONDS");
    seconds = seconds > 0 ? std::min(seconds, 30) : 4;
    Report r;
    collectPassive(r, nullptr);
    probeVulkanActive(r, seconds);
    // Asked for explicitly, so written whether or not tracing is on
    const QString directory = reportDirectory();
    const QString path = writeReport(r, "probe", directory.isEmpty() ? QString() :
        QDir(directory).filePath(QString("probe-%1.txt").arg(QDateTime::currentSecsSinceEpoch())));
    fprintf(stderr, "Display probe report: %s\n", path.isEmpty() ? "(not written)" : qPrintable(path));
    return path.isEmpty() ? 1 : 0;
}

struct DisplayProbe::StreamSampler::Impl {
    std::thread thread;
    std::mutex lock;
    std::condition_variable wake;
    bool stopping = false;
    QString path;

    void append(const std::string& line)
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Display probe sample: %s", line.c_str());
        if (path.isEmpty()) {
            return;
        }
        if (FILE* file = fopen(QFile::encodeName(path).constData(), "a")) {
            fprintf(file, "%s\n", line.c_str());
            fclose(file);
        }
    }

    void run()
    {
        StateReader state;
        const uint64_t start = nowNs();
        uint64_t written = 0;
        std::string last;
        append("\n== Samples during the stream (on change, else every 30 s) ==");
        std::unique_lock<std::mutex> guard(lock);
        while (!stopping) {
            guard.unlock();
            const std::string line = state.line();
            const uint64_t now = nowNs();
            if (line != last || now - written >= 30000000000ull) {
                append(format("[+%.1fs] %s", (now - start) / 1e9, line.c_str()));
                last = line;
                written = now;
            }
            guard.lock();
            wake.wait_for(guard, std::chrono::seconds(1), [this] { return stopping; });
        }
    }
};

DisplayProbe::StreamSampler::StreamSampler() = default;

DisplayProbe::StreamSampler::~StreamSampler()
{
    stop();
}

void DisplayProbe::StreamSampler::start(SDL_Window* window)
{
    if (m_Impl != nullptr || !GamescopeDisplayState::runningUnderGamescope()) {
        return;
    }
    // Only while VRR tracing is on: the samples go beside the trace
    const QString path = writePassiveReport("stream", window);
    if (path.isEmpty()) {
        return;
    }
    m_Impl = new Impl();
    m_Impl->path = path;
    m_Impl->thread = std::thread(&Impl::run, m_Impl);
}

void DisplayProbe::StreamSampler::stop()
{
    if (m_Impl == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(m_Impl->lock);
        m_Impl->stopping = true;
    }
    m_Impl->wake.notify_all();
    m_Impl->thread.join();
    delete m_Impl;
    m_Impl = nullptr;
}

#else

QString DisplayProbe::writePassiveReport(const char*, SDL_Window*)
{
    return QString();
}

int DisplayProbe::runActiveProbe()
{
    fprintf(stderr, "The display probe is only available on Linux\n");
    return 1;
}

struct DisplayProbe::StreamSampler::Impl {};

DisplayProbe::StreamSampler::StreamSampler() = default;

DisplayProbe::StreamSampler::~StreamSampler()
{
    delete m_Impl;
}

void DisplayProbe::StreamSampler::start(SDL_Window*) {}

void DisplayProbe::StreamSampler::stop() {}

#endif
