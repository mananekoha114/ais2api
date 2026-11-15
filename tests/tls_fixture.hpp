// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A self-signed certificate for loopback TLS servers in tests.
#pragma once
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace ais2api::test {
struct SelfSigned {
    // PEM text; the certificate is also the CA a client trusts.
    std::string certificate, key;
};

// An RSA certificate for localhost and 127.0.0.1, valid for an hour.
inline SelfSigned self_signed_localhost() {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keygen(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!keygen || EVP_PKEY_keygen_init(keygen.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(keygen.get(), 2048) <= 0)
        throw std::runtime_error("fixture key generation failed");
    EVP_PKEY *raw = nullptr;
    if (EVP_PKEY_keygen(keygen.get(), &raw) <= 0) throw std::runtime_error("fixture key generation failed");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
    X509_set_pubkey(cert.get(), key.get());
    auto *name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0);
    X509_set_issuer_name(cert.get(), name);
    const auto extension = [&](int nid, const char *value) {
        auto *ext = X509V3_EXT_conf_nid(nullptr, nullptr, nid, const_cast<char *>(value));
        if (!ext) throw std::runtime_error("fixture extension failed");
        X509_add_ext(cert.get(), ext, -1);
        X509_EXTENSION_free(ext);
    };
    extension(NID_basic_constraints, "critical,CA:TRUE");
    extension(NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
    if (!X509_sign(cert.get(), key.get(), EVP_sha256())) throw std::runtime_error("fixture signing failed");
    const auto pem = [](BIO *bio) {
        char *text = nullptr;
        const auto size = BIO_get_mem_data(bio, &text);
        return std::string(text, static_cast<std::size_t>(size));
    };
    std::unique_ptr<BIO, decltype(&BIO_free)> certbio(BIO_new(BIO_s_mem()), BIO_free), keybio(BIO_new(BIO_s_mem()), BIO_free);
    PEM_write_bio_X509(certbio.get(), cert.get());
    PEM_write_bio_PrivateKey(keybio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    return {pem(certbio.get()), pem(keybio.get())};
}
} // namespace ais2api::test
