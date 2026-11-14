#pragma once

#include <cstdint>
#include <cstddef>

namespace recam {

// One libcameraservice import. The original is published before the slot changes.
struct MetadataImport {
    uintptr_t* slot = nullptr;
    uintptr_t original = 0;
    uintptr_t replacement = 0;
    int protection = 0;
    bool installed = false;
};

bool metadata_import_install(uintptr_t cameraFunction, uintptr_t replacement,
                             MetadataImport& hook, const char* name = "find_camera_metadata_ro_entry");
bool metadata_import_remove(MetadataImport& hook);
bool metadata_import_build_id(uintptr_t cameraFunction, const uint8_t* expected, size_t size);

} // namespace recam
