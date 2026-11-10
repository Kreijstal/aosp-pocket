// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdio.h>
inline void pocketServiceOutput32() {
#ifndef POCKET_CORE_COMPONENT
    setbuf(stdout,nullptr);
#endif
    // The shared core initializes stdout once, before starting component threads.
}
