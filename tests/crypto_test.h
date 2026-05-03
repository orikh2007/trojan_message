//
// Created by Ori Kedar Haspel on 03/05/2026.
//

#ifndef TROJAN_MESSAGE_CRYPTO_TEST_H
#define TROJAN_MESSAGE_CRYPTO_TEST_H
#include <openssl/evp.h>   // EVP_CIPHER_CTX, EVP_aes_256_gcm, all the encrypt/decrypt calls
#include <vector>           // std::vector<uint8_t>
#include <array>           // std::vector<uint8_t>
#include <cassert>          // assert()
#include <cstring>          // memcmp()
#include <openssl/err.h>
#include <optional>

class crypto_test {
public:
    crypto_test();

    std::vector<uint8_t> sym_encrypt_data(const std::vector<uint8_t> &data) const;

    std::optional<std::vector<uint8_t>> sym_decrypt_data(const std::vector<uint8_t> &data) const;

    EVP_PKEY *gen_x25519_keypair();

    std::array<uint8_t, 32> get_pubkey(EVP_PKEY *keypair);

    std::array<uint8_t, 32> derive_secret(EVP_PKEY* my_keypair, const std::array<uint8_t, 32>& peer_raw_pubkey);

    void run_crypto_tests_sym();

    void run_crypto_tests_asym();

private:
    std::array<uint8_t, 32> key_ = {};
    std::array<uint8_t, 12> nonce_ = {};
    EVP_PKEY* keypair_ = nullptr;
};


#endif //TROJAN_MESSAGE_CRYPTO_TEST_H