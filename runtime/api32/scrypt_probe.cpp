// SPDX-License-Identifier: GPL-3.0-only
// The three vectors in the pinned AOSP scrypt tests (RFC 7914 parameters).
extern "C" {
#include <crypto_scrypt.h>
}
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
struct Vector {const char* password;const char* salt;uint64_t n;uint32_t r,p;const char* expected;};
static const Vector vectors[]={
    {"","",16,1,1,
     "77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442"
     "fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906"},
    {"password","NaCl",1024,8,16,
     "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b373162"
     "2eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640"},
    {"pleaseletmein","SodiumChloride",16384,8,1,
     "7023bdcb3afd7348461c06cd81fd38ebfda8fbba904f8e3ea9b543f6545da1f2"
     "d5432955613f0fcf62d49705242a9af9e61e85dc0d651e40dfcf017b45575887"},
};
int main() {
    char sdk[PROP_VALUE_MAX];__system_property_get("ro.build.version.sdk",sdk);
    if(getuid()!=0 || strcmp(sdk,"32") || syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL)return 64;
    for(const auto& vector:vectors) {
        unsigned char output[64];char hex[129];
        if(crypto_scrypt(reinterpret_cast<const uint8_t*>(vector.password),strlen(vector.password),
                reinterpret_cast<const uint8_t*>(vector.salt),strlen(vector.salt),
                vector.n,vector.r,vector.p,output,sizeof(output)))return 1;
        for(size_t i=0;i<sizeof(output);i++)snprintf(hex+i*2,3,"%02x",output[i]);
        if(strcmp(hex,vector.expected))return 2;
    }
    puts("API32_SCRYPT_KAT_PASS vectors=3 backend=pinned_aosp_arm64");return 0;
}
