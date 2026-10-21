// SPDX-License-Identifier: GPL-3.0-only
int pocket_loader_answer(void) { return 42; }
int JNI_OnLoad(void* vm,void* reserved) {
    (void)vm;(void)reserved;return 0x00010006;
}
