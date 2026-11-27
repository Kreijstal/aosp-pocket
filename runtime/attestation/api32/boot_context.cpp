// SPDX-License-Identifier: GPL-3.0-only
//
// Pocket API 32 attestation identity: the API 32 counterpart of the Android 9
// adapter in runtime/attestation/preload.cpp.
//
// The Android 12L guest has no Android 10/11 `keystore` daemon and no KeyMint
// AIDL device: `pocket-keystore` is keystore2 talking to the stock software
// Keymaster HAL served by `pocket-crypto-api32`, and that HAL answers
// KeymasterContext::GetVerifiedBootParams() from AOSP's pure-soft constants:
//
//     verified_boot_state = 2 (UNVERIFIED), device_locked = false,
//     verified_boot_key = empty, verified_boot_hash = empty
//
// (Verified by disassembling libpuresoftkeymasterdevice.so @0xb244 in the
// shipped image.) Those constants are the whole RootOfTrust the guest can
// produce, and they are what sets DexProtector's anomaly flag in Revolut.
//
// This library is preloaded into pocket-crypto-api32. It defines the same
// mangled symbols as keymaster::PureSoftKeymasterContext, so the ELF data
// relocations of that class's vtable resolve to these functions. No ptrace, no
// Binder hook, no image binary patching.
//
// Owner configuration reuses the Android 9 file verbatim:
//     /data/misc/keystore/pocket-teesim.json
//     {"bootInfo":{"deviceLocked":true,"verifiedBootState":0,...}, "profiles":[...]}
// `bootInfo` is read here; `profiles`/`keyboxB64` is the part that only a full
// TEESimulator KeyMint TA can serve (see REPORT.md stage 2).

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr const char* kConfig = "/data/misc/keystore/pocket-teesim.json";
constexpr size_t kBlobLength = 32;

// Fallback when the owner has no configuration: a locked, "verified" boot.
// 32 bytes each so DER encodings stay canonical; the classifier checks the
// state and presence, not the AVB key's identity.
unsigned char gBootKey[kBlobLength] = {
    0x50, 0x4f, 0x43, 0x4b, 0x45, 0x54, 0x2d, 0x41, 0x56, 0x42, 0x2d, 0x56,
    0x42, 0x4d, 0x45, 0x54, 0x41, 0x2d, 0x4b, 0x45, 0x59, 0x2d, 0x30, 0x30,
    0x30, 0x31, 0x2d, 0x50, 0x4f, 0x43, 0x4b, 0x45,
};
unsigned char gBootHash[kBlobLength] = {
    0x50, 0x4f, 0x43, 0x4b, 0x45, 0x54, 0x2d, 0x56, 0x42, 0x4d, 0x45, 0x54,
    0x41, 0x2d, 0x44, 0x49, 0x47, 0x45, 0x53, 0x54, 0x2d, 0x30, 0x30, 0x30,
    0x31, 0x2d, 0x50, 0x4f, 0x43, 0x4b, 0x45, 0x54,
};
int32_t gBootState = 0;   // KM_VERIFIED_BOOT_VERIFIED
bool gDeviceLocked = true;
pthread_once_t gLoadOnce = PTHREAD_ONCE_INIT;
// Static, not on the stack: HAL Binder threads have small stacks.
char gText[64 * 1024];

// Minimal scan for the three bootInfo members; the file also carries profiles
// and a base64 keybox, so a full JSON parser buys nothing here.
bool readInteger(const char* text, const char* key, long* out) {
    const char* at = strstr(text, key);
    if (!at) return false;
    at = strchr(at + strlen(key), ':');
    if (!at) return false;
    char* end = nullptr;
    long value = strtol(at + 1, &end, 10);
    if (end == at + 1) return false;
    *out = value;
    return true;
}

bool readBoolean(const char* text, const char* key, bool* out) {
    const char* at = strstr(text, key);
    if (!at) return false;
    at = strchr(at + strlen(key), ':');
    if (!at) return false;
    ++at;
    while (*at == ' ' || *at == '\t') ++at;
    if (!strncmp(at, "true", 4)) { *out = true; return true; }
    if (!strncmp(at, "false", 5)) { *out = false; return true; }
    return false;
}

void load() {
    FILE* file = fopen(kConfig, "rb");
    if (!file) return;
    char* text = gText;
    const size_t length = fread(text, 1, sizeof(gText) - 1, file);
    fclose(file);
    if (length == 0 || length >= sizeof(gText) - 1) return;
    text[length] = '\0';
    long state = 0;
    if (readInteger(text, "\"verifiedBootState\"", &state) && state >= 0 && state <= 3)
        gBootState = static_cast<int32_t>(state);
    bool locked = true;
    if (readBoolean(text, "\"deviceLocked\"", &locked)) gDeviceLocked = locked;
}

}  // namespace

// keymaster::KeymasterContext::GetVerifiedBootParams() returns
// `const VerifiedBootParams*`: a pointer, in x0, to a struct the context owns
// (the stock implementation fills a function-local static and returns its
// address; libpuresoftkeymasterdevice.so @0xb244). Callers keep and
// dereference that pointer, so it must stay valid for the process lifetime.
// Layout: key{data,size}, hash{data,size}, int32 state, bool device_locked.
struct kblob { const void* data; size_t size; };
struct vboot_params {
    kblob verified_boot_key;
    kblob verified_boot_hash;
    int32_t verified_boot_state;
    bool device_locked;
};
static_assert(sizeof(vboot_params) == 40, "VerifiedBootParams layout");

namespace {
vboot_params gParams;
pthread_once_t gParamsOnce = PTHREAD_ONCE_INIT;

void fillParams() {
    pthread_once(&gLoadOnce, load);
    gParams.verified_boot_key = {gBootKey, kBlobLength};
    gParams.verified_boot_hash = {gBootHash, kBlobLength};
    gParams.verified_boot_state = gBootState;
    gParams.device_locked = gDeviceLocked;
}
}  // namespace

extern "C" __attribute__((visibility("default")))
const vboot_params* pocket_verified_boot_params(const void* self, int32_t* error) {
    (void)self;
    pthread_once(&gParamsOnce, fillParams);
    if (error) *error = 0;  // KM_ERROR_OK
    return &gParams;
}

#define POCKET_BOOT_PARAMS(symbol)                                                        \
    __asm__(".globl " symbol "\n.set " symbol ", pocket_verified_boot_params\n")

POCKET_BOOT_PARAMS("_ZNK9keymaster24PureSoftKeymasterContext21GetVerifiedBootParamsEP17keymaster_error_t");
POCKET_BOOT_PARAMS("_ZNK9keymaster22SoftAttestationContext21GetVerifiedBootParamsEP17keymaster_error_t");
POCKET_BOOT_PARAMS("_ZThn16_NK9keymaster24PureSoftKeymasterContext21GetVerifiedBootParamsEP17keymaster_error_t");
POCKET_BOOT_PARAMS("_ZThn8_NK9keymaster22SoftAttestationContext21GetVerifiedBootParamsEP17keymaster_error_t");
