//
// Created by orikh on 26/04/2026.
//

#ifndef TROJAN_MESSAGE_CRYPTO_H
#define TROJAN_MESSAGE_CRYPTO_H
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <vector>
#include <array>
#include <cstdint>
#include <optional>

namespace crypto {
    EVP_PKEY* gen_keypair();
    void free_keypair(EVP_PKEY* kp);
    std::array<uint8_t, 32> get_pubkey(EVP_PKEY* kp);
    std::array<uint8_t, 32> derive_secret(EVP_PKEY* my_kp, const std::array<uint8_t, 32>& peer_pub);
    std::vector<uint8_t> encrypt(const std::array<uint8_t,32>& key, const std::vector<uint8_t>& plaintext);
    std::optional<std::vector<uint8_t>> decrypt(const std::array<uint8_t, 32> &key, const std::vector<uint8_t> &buf);
}
#endif //TROJAN_MESSAGE_CRYPTO_H