#include "capture_layout.hpp"
#include "metadata_import.hpp"
#include "qcom_layout.hpp"
#include "../common/ring_access.hpp"

#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <cstring>
#include <cstddef>

namespace recam {
namespace {

// Public camera_metadata_ro_entry_t LP64 ABI from system/media.
struct Entry {
    size_t index;
    uint32_t tag;
    uint8_t type;
    size_t count;
    const void* data;
};
static_assert(sizeof(Entry) == 32 && offsetof(Entry, data) == 24);
constexpr uint32_t kSensorTimestamp = 0xe0010;
using Find = int (*)(const void*, uint32_t, void*);
using Count = size_t (*)(const void*);
using Get = int (*)(const void*, size_t, Entry*);
using Name = const char* (*)(uint32_t, const void*);
MetadataImport g_import;
Count g_count = nullptr;
Get g_get = nullptr;
Name g_section = nullptr, g_name = nullptr;

struct Geometry {
    int32_t width, height, stride, rows;
    bool valid() const {
        return width >= 2 && height >= 2 && stride >= width && rows >= height &&
               stride <= 8192 && rows <= 8192 && !((width | height | stride | rows) & 1);
    }
};
struct Record {
    int64_t timestamp = 0;
    Geometry input{}, output{};
    bool conflict = false;
};
Record g_records[128]{};
uint32_t g_access = 0, g_next = 0;

void observe_metadata(const void* metadata, int64_t timestamp) {
    if (timestamp <= 0) return;
    const size_t count = g_count(metadata);
    if (!count || count > 4096) return;
    Record record{};
    record.timestamp = timestamp;
    bool input = false, output = false;
    for (size_t i = 0; i < count; ++i) {
        Entry entry{};
        if (g_get(metadata, i, &entry) || !(entry.tag & 0x80000000u) ||
            entry.type != 1 || entry.count != 4 || !entry.data) continue;
        const char* section = g_section(entry.tag, metadata);
        if (!section || strcmp(section, "com.oplus")) continue;
        const char* name = g_name(entry.tag, metadata);
        if (!name) continue;
        if (!strcmp(name, "upscale.input.size")) {
            if (input) return;
            memcpy(&record.input, entry.data, sizeof(Geometry));
            input = true;
        } else if (!strcmp(name, "upscale.output.size")) {
            if (output) return;
            memcpy(&record.output, entry.data, sizeof(Geometry));
            output = true;
        }
    }
    if (!input || !output || !record.input.valid() || !record.output.valid()) return;
    RingWrite guard(&g_access);
    if (!guard) return;
    for (auto& saved : g_records) {
        if (saved.timestamp != timestamp) continue;
        if (memcmp(&saved.input, &record.input, sizeof(Geometry)) ||
            memcmp(&saved.output, &record.output, sizeof(Geometry))) saved.conflict = true;
        return;
    }
    g_records[g_next++ % 128] = record;
}

} // namespace

bool capture_layout_install(uintptr_t cameraFunction, uintptr_t replacement) {
    char manufacturer[PROP_VALUE_MAX]{};
    __system_property_get("ro.product.manufacturer", manufacturer);
    if (strcmp(manufacturer, "OnePlus")) return false;
    if (!g_count) g_count = reinterpret_cast<Count>(dlsym(RTLD_DEFAULT, "get_camera_metadata_entry_count"));
    if (!g_get) g_get = reinterpret_cast<Get>(dlsym(RTLD_DEFAULT, "get_camera_metadata_ro_entry"));
    if (!g_section) g_section = reinterpret_cast<Name>(dlsym(RTLD_DEFAULT, "get_local_camera_metadata_section_name"));
    if (!g_name) g_name = reinterpret_cast<Name>(dlsym(RTLD_DEFAULT, "get_local_camera_metadata_tag_name"));
    if (!g_count || !g_get || !g_section || !g_name) {
        __android_log_print(ANDROID_LOG_WARN, "ReCam", "capture layout: metadata API unavailable");
        return false;
    }
    {
        RingWrite guard(&g_access);
        if (!guard) return false;
        for (auto& r : g_records) r = {};
        g_next = 0;
    }
    const bool ok = metadata_import_install(cameraFunction, replacement, g_import);
    __android_log_print(ok ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "ReCam",
        "capture layout: per-frame Oplus metadata observer %s", ok ? "installed" : "unavailable");
    return ok;
}

bool capture_layout_remove() { return metadata_import_remove(g_import); }
bool capture_layout_installed() { return g_import.installed; }

int capture_layout_query(const void* metadata, uint32_t tag, void* entry, bool observe) {
    auto original = reinterpret_cast<Find>(__atomic_load_n(&g_import.original, __ATOMIC_ACQUIRE));
    if (!original) return -1;
    const int result = original(metadata, tag, entry);
    if (!result && observe && metadata && entry && tag == kSensorTimestamp) {
        Entry view{};
        memcpy(&view, entry, sizeof(view));
        if (view.type == 3 && view.count == 1 && view.data) {
            int64_t timestamp;
            memcpy(&timestamp, view.data, sizeof(timestamp));
            observe_metadata(metadata, timestamp);
        }
    }
    return result;
}

bool capture_layout_apply(int64_t timestamp, const void* native, FrameTarget& t) {
    if (timestamp <= 0) return false;
    qcom::Handle handle;
    if (!qcom::read_handle(native, handle) || handle.format != 0x113 ||
        (handle.flags & 0x08000000u)) return false;
    Record record{};
    {
        RingWrite guard(&g_access);
        if (!guard) return false;
        for (const auto& saved : g_records) {
            if (saved.timestamp == timestamp && !saved.conflict) { record = saved; break; }
        }
    }
    if (!record.timestamp) return false;
    const auto& in = record.input;
    const auto& out = record.output;
    if (!in.valid() || !out.valid() || out.width != t.width || out.height != t.height ||
        handle.requestedWidth != out.width || handle.requestedHeight != out.height ||
        handle.width != out.stride || handle.height != out.rows ||
        in.width > out.width || in.height > out.height ||
        in.stride > out.stride || in.rows > out.rows) return false;
    if (t.y.pixelStride != 1 || t.cb.pixelStride != 2 || t.cr.pixelStride != 2 ||
        t.y.rowStride != out.stride || t.cb.rowStride != out.stride || t.cr.rowStride != out.stride)
        return false;
    const auto base = reinterpret_cast<uintptr_t>(t.y.data);
    const uint64_t outputY = uint64_t(out.stride) * out.rows;
    const uint64_t inputY = uint64_t(in.stride) * in.rows;
    if (!base || qcom::field<uint64_t>(native, 84) != base ||
        outputY + outputY / 2 > handle.size || inputY + inputY / 2 > handle.size ||
        base > UINTPTR_MAX - handle.size ||
        reinterpret_cast<uintptr_t>(t.cr.data) != base + outputY ||
        reinterpret_cast<uintptr_t>(t.cb.data) != base + outputY + 1) return false;
    if (!memcmp(&in, &out, sizeof(Geometry))) return false;
    t.width = in.width;
    t.height = in.height;
    t.y.rowStride = in.stride;
    t.cr = {reinterpret_cast<uint8_t*>(base + inputY), in.stride, 2};
    t.cb = {reinterpret_cast<uint8_t*>(base + inputY + 1), in.stride, 2};
    return true;
}

} // namespace recam
