// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/ce_storage.h"
#include <assert.h>
#include <stdio.h>
int main() {
    CeSecret32 secret;
    assert(secret.decode("0011aAFF") && secret.bytes==std::vector<uint8_t>({0,17,170,255}));
    assert(!secret.decode("0011zz") && secret.bytes.empty());
    assert(secret.decode("!") && secret.bytes.empty());
    assert(!secret.decode("0") && secret.bytes.empty());
    assert(!secret.decode(std::string("00\0f",4)) && secret.bytes.empty());
    assert(!secret.decode(std::string(4098,'a')) && secret.bytes.empty());
    assert(secret.decode(std::string(4096,'a')) && secret.bytes.size()==2048);
    assert(secret.decode("") && secret.bytes.empty());
    puts("API32_CE_SECRET_PASS valid_hex=binary invalid_odd_embedded_nul_oversize=rejected failed_decode=empty");
}
