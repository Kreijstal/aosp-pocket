# SPDX-License-Identifier: GPL-3.0-only
"""Pocket edits to 12L's camera HandleImporter: graphics mapper 2.0/3.0 only.

The 12L guest provides graphics mapper 2.0, so the mapper 4.0 path (and the gralloc4 AIDL plane-layout
types it alone needs) is compiled out; the importer falls back to 3.0 and then 2.0 as stock does.
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from port_util import edit, region

path = Path(sys.argv[1]) / 'camera/common/1.0/default/HandleImporter.cpp'
text = edit(path.read_text(), path.name, [
('#include <gralloctypes/Gralloc4.h>\n', ''),
('''using aidl::android::hardware::graphics::common::PlaneLayout;
using aidl::android::hardware::graphics::common::PlaneLayoutComponent;
using aidl::android::hardware::graphics::common::PlaneLayoutComponentType;
''', ''),
('''    mMapperV4 = IMapperV4::getService();
    if (mMapperV4 != nullptr) {
        mInitialized = true;
        return;
    }

''', '''    // Pocket: no mapper 4.0 in this guest; mMapperV4 stays null.
'''),
('''    if (mMapperV4 != nullptr) {
        return lockYCbCrInternal<IMapperV4, MapperErrorV4>(mMapperV4, buf, cpuUsage, accessRegion);
    }

''', ''),
('''    if (mMapperV4 != nullptr) {
        std::vector<PlaneLayout> planeLayouts = getPlaneLayouts(mMapperV4, buf);
        if (planeLayouts.size() != 1) {
            ALOGE("%s: Unexpected number of planes %zu!",  __FUNCTION__, planeLayouts.size());
            return BAD_VALUE;
        }

        *stride = planeLayouts[0].strideInBytes;
    } else {
        ALOGE("%s: mMapperV4 is null! Query not supported!", __FUNCTION__);
        return NO_INIT;
    }
''', '''    ALOGE("%s: mMapperV4 is null! Query not supported!", __FUNCTION__);
    return NO_INIT;
'''),
])
a, c = region(text, 'std::vector<PlaneLayout> getPlaneLayouts(', 'YCbCrLayout HandleImporter::lockYCbCrInternal<IMapperV4, MapperErrorV4>(')
text = text[:a] + '#if 0  // Pocket: mapper 4.0 plane layouts\n' + text[a:c] + '#endif\n' + text[c:]
path.write_text(text)
