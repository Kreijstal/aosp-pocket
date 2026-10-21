// SPDX-License-Identifier: GPL-3.0-only
// Uses Android 9's real Parcel implementation and the real upstream TA.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <openssl/bytestring.h>
#include <openssl/x509.h>
#include "keystore_router.cpp"

extern "C" int teesim_android_api() { return 28; }
extern "C" int __android_log_write(int, const char* tag, const char* message) {
    fprintf(stderr, "%s: %s\n", tag, message);
    return 0;
}

static void check(bool ok, const char* what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}
static void status(Parcel& p) {
    p.setDataPosition(0);
    check(p.readInt32() == 0, "no Java exception");
    check(p.readInt32() == KS_NO_ERROR, "Keystore success code");
}
static void characteristics(Parcel& p) {
    check(p.readInt32() == 1, "characteristics present");
    std::vector<std::vector<uint8_t>> storage;
    auto sw = ReadKeymasterArguments(p, storage);
    auto hw = ReadKeymasterArguments(p, storage);
    check(ParamInt(sw, TAG_ALGORITHM, -1) == ALGORITHM_EC, "software EC characteristic");
    check(ParamInt(sw, 0x100002be, -1) == 2, "imported origin");
    check(hw.empty(), "no hardware-enforced claims");
    check(p.dataAvail() == 0, "no trailing callback/status data");
}

int main(int argc, char** argv) {
    check(argc == 2, "test-keybox.xml argument");
    std::ifstream stream(argv[1], std::ios::binary);
    std::string xml((std::istreambuf_iterator<char>(stream)), {});
    check(!xml.empty(), "read local test identity");
    TsBootInfo boot{};
    boot.device_locked = false; boot.verified_boot_state = 2;
    boot.attest_version_tee = 3; boot.attest_version_strongbox = 3;
    teesim_cfg_begin(&boot);
    const int uid = 10018;
    const char* package = "dev.kreijstal.pocketattestationtest";
    TsProfile profile{};
    profile.id = "local-test"; profile.security_level = 0; profile.os_version = 90000;
    profile.keybox = reinterpret_cast<const uint8_t*>(xml.data()); profile.keybox_len = xml.size();
    profile.uids = &uid; profile.n_uids = 1; profile.uid_packages = &package;
    check(teesim_cfg_add_profile(&profile), "initialize real TA with local test CA");
    char error[128]{};
    check(teesim_cfg_commit(1, error, sizeof(error)) == 1, "commit profile");
    check(Codes().generateKey == 23 && Codes().attestKey == 35, "Android 9 transaction ordinals");
    const String16 alias("USRPKEY_pocket-wire-test");
    const std::vector<KmParam> params = {
        {TAG_ALGORITHM, ALGORITHM_EC, nullptr, 0}, {TAG_KEY_SIZE, 256, nullptr, 0},
        {TAG_EC_CURVE, 1, nullptr, 0}, {TAG_PURPOSE, 2, nullptr, 0},
        {TAG_PURPOSE, 3, nullptr, 0}, {TAG_DIGEST, 4, nullptr, 0},
        {0x700001f7, 1, nullptr, 0}};  // NO_AUTH_REQUIRED
    auto generate = [&](Parcel& in, int requested_uid, int flags) {
        in.writeString16(alias); in.writeInt32(1); WriteKeymasterArguments(in, params);
        in.writeByteArray(0, nullptr); in.writeInt32(requested_uid); in.writeInt32(flags);
        in.setDataPosition(0);
    };
    {
        Parcel& in = *new Parcel();
        Parcel& reply = *new Parcel(); generate(in, uid+1, 0);
        check(!HandleGenerateKey(uid, in, &reply), "cross-UID generation falls through");
        check(g_keys.empty(), "cross-UID request creates no key");
    }
    {
        Parcel& in = *new Parcel();
        Parcel& reply = *new Parcel(); generate(in, -1, 16);
        check(HandleGenerateKey(uid, in, &reply), "StrongBox refusal handled");
        reply.setDataPosition(0);
        check(reply.readInt32() == 0 && reply.readInt32() == -68, "StrongBox unavailable preserved");
        check(g_keys.empty(), "StrongBox refusal creates no key");
    }
    {
        Parcel& in = *new Parcel();
        Parcel& reply = *new Parcel(); generate(in, -1, 0);
        check(HandleGenerateKey(uid, in, &reply), "synchronous generateKey");
        status(reply); characteristics(reply);
    }
    {
        Parcel& in = *new Parcel();
        Parcel& reply = *new Parcel();
        in.writeString16(alias); in.writeInt32(0); in.writeInt32(0); in.writeInt32(-1);
        in.setDataPosition(0);
        check(HandleGetKeyCharacteristics(uid, in, &reply), "synchronous getKeyCharacteristics");
        status(reply); characteristics(reply);
    }
    std::vector<uint8_t> spki;
    {
        Parcel& in = *new Parcel();
        Parcel& reply = *new Parcel();
        in.writeString16(alias); in.writeInt32(0); in.writeInt32(0); in.writeInt32(0); in.writeInt32(-1);
        in.setDataPosition(0);
        check(HandleExportKey(uid, in, &reply), "synchronous exportKey/real key generation");
        reply.setDataPosition(0);
        check(reply.readInt32() == 0 && reply.readInt32() == 1 && reply.readInt32() == 1,
              "ExportResult synchronous envelope");
        spki = ReadByteArray(reply);
        check(!spki.empty() && reply.dataAvail() == 0, "exported SubjectPublicKeyInfo");
    }
    std::vector<std::vector<uint8_t>> chain;
    const uint8_t challenge[] = "Pocket Android 9 unique wire-test challenge";
    {
        Parcel& in = *new Parcel();
        Parcel& reply = *new Parcel();
        in.writeString16(alias); in.writeInt32(1);
        WriteKeymasterArguments(in, {{TAG_ATTESTATION_CHALLENGE, 0, challenge, sizeof(challenge)}});
        in.setDataPosition(0);
        check(HandleAttestKey(uid, in, &reply), "synchronous attestKey");
        status(reply);
        check(reply.readInt32() == 1, "certificate chain present");
        const int n = reply.readInt32(); check(n == 3, "leaf and two local CA certificates");
        for (int i=0; i<n; ++i) chain.push_back(ReadByteArray(reply));
        check(reply.dataAvail() == 0, "attestation reply has no callback envelope");
    }
    std::vector<bssl::UniquePtr<X509>> certs;
    for (const auto& der : chain) {
        const uint8_t* cursor = der.data();
        certs.emplace_back(d2i_X509(nullptr, &cursor, der.size()));
        check(certs.back() != nullptr && cursor == der.data()+der.size(), "parse complete X.509 certificate");
    }
    for (size_t i=0; i+1<certs.size(); ++i) {
        bssl::UniquePtr<EVP_PKEY> issuer(X509_get_pubkey(certs[i+1].get()));
        check(X509_verify(certs[i].get(), issuer.get()) == 1, "certificate chain signature");
    }
    bssl::UniquePtr<EVP_PKEY> public_key(X509_get_pubkey(certs[0].get()));
    uint8_t* public_der = nullptr; const int public_len = i2d_PUBKEY(public_key.get(), &public_der);
    check(public_len == static_cast<int>(spki.size()) && !memcmp(public_der, spki.data(), spki.size()),
          "attestation cert binds the exported key");
    OPENSSL_free(public_der);
    bssl::UniquePtr<ASN1_OBJECT> oid(OBJ_txt2obj("1.3.6.1.4.1.11129.2.1.17", 1));
    int ext_index = X509_get_ext_by_OBJ(certs[0].get(), oid.get(), -1);
    check(ext_index >= 0, "Android attestation extension");
    ASN1_OCTET_STRING* value = X509_EXTENSION_get_data(X509_get_ext(certs[0].get(), ext_index));
    CBS encoded, record, item;
    CBS_init(&encoded, ASN1_STRING_get0_data(value), ASN1_STRING_length(value));
    check(CBS_get_asn1(&encoded, &record, CBS_ASN1_SEQUENCE), "attestation record sequence");
    check(CBS_get_asn1(&record, &item, CBS_ASN1_INTEGER), "attestation version");
    check(CBS_get_asn1(&record, &item, CBS_ASN1_ENUMERATED) && CBS_len(&item)==1 && CBS_data(&item)[0]==0,
          "attestation security level is SOFTWARE");
    check(CBS_get_asn1(&record, &item, CBS_ASN1_INTEGER), "keymaster version");
    check(CBS_get_asn1(&record, &item, CBS_ASN1_ENUMERATED) && CBS_len(&item)==1 && CBS_data(&item)[0]==0,
          "keymaster security level is SOFTWARE");
    check(CBS_get_asn1(&record, &item, CBS_ASN1_OCTETSTRING) && CBS_len(&item)==sizeof(challenge) &&
          !memcmp(CBS_data(&item), challenge, sizeof(challenge)), "exact caller challenge in certificate");
    // Exercise the actual TA operation lifecycle independently of Binder IPC.
    auto ta = ProfileForUid(uid);
    const auto& blob = g_keys.at(KeyId(uid, alias)).ta_blob;
    const KmParam digest{TAG_DIGEST, 4, nullptr, 0};
    TsBeginResult* begun = nullptr;
    check(teesim_km_begin(ta.get(), 2, blob.data(), blob.size(), &digest, 1, nullptr, &begun) == 0 && begun,
          "TA begin signing");
    const int64_t handle = teesim_km_begin_op_handle(begun);
    teesim_km_free_begin(begun);
    uint8_t* output = nullptr; size_t output_len = 0;
    check(teesim_km_update(ta.get(), handle, challenge, sizeof(challenge), nullptr, nullptr,
                          &output, &output_len) == 0, "TA update signing");
    if (output) teesim_km_free_buf(output, output_len);
    output = nullptr; output_len = 0;
    check(teesim_km_finish(ta.get(), handle, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, 0,
                          &output, &output_len) == 0 && output && output_len, "TA finish signing");
    bssl::UniquePtr<EVP_MD_CTX> verify(EVP_MD_CTX_new());
    check(EVP_DigestVerifyInit(verify.get(), nullptr, EVP_sha256(), nullptr, public_key.get()) == 1 &&
          EVP_DigestVerifyUpdate(verify.get(), challenge, sizeof(challenge)) == 1 &&
          EVP_DigestVerifyFinal(verify.get(), output, output_len) == 1,
          "TA signature verifies against attested public key");
    teesim_km_free_buf(output, output_len);
    puts("PASS: Android 9 synchronous wire formats, UID isolation, StrongBox refusal, real EC key generation,");
    puts("      signed local certificate chain, exported-key binding, SOFTWARE levels, caller challenge and TA sign/verify");
    fflush(stdout);
    // Android 9 Parcel destruction eagerly opens /dev/binder even for data-only
    // buffers. These small process-lifetime buffers intentionally remain alive
    // for qemu-user validation without a kernel Binder driver. Device RPC is a
    // separate test; no Binder transport success is claimed here.
    _exit(0);
}
