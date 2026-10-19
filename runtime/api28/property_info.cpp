// SPDX-License-Identifier: GPL-3.0-only
// Use AOSP Android 9's actual serialized property-info format.
#include <property_info_serializer/property_info_serializer.h>
#include <stdio.h>

bool pocketCreatePropertyInfo() {
    std::string output, error;
    if (!android::properties::BuildTrie({}, "u:object_r:default_prop:s0", "string", &output, &error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    FILE* file = fopen("/dev/__properties__/property_info", "we");
    if (!file) return false;
    bool written = fwrite(output.data(), 1, output.size(), file) == output.size();
    return fclose(file) == 0 && written;
}
