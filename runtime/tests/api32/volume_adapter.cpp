// SPDX-License-Identifier: GPL-3.0-only
#include "volume_adapter_errors.h"
int main() {
    char directory[]="/tmp/pocket-adapter-errors-XXXXXX";assert(mkdtemp(directory));
    volumeAdapterErrors(directory);assert(!rmdir(directory));
}
