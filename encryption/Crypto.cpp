//
// Created by orikh on 26/04/2026.
//
#include "Crypto.h"
#include <cassert>

void static crypto_check(const int code, const char* where) {
    if (code != 1) {
        ERR_print_errors_fp(stderr);
        assert(false && where);
    }
}

EVP_PKEY* crypto::gen_keypair() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    crypto_check(EVP_PKEY_keygen_init(ctx), "[CRYPTO] get_keypair keygen_init fail");
    EVP_PKEY* keypair = nullptr;
    crypto_check(EVP_PKEY_keygen(ctx, &keypair), "[CRYPTO] get_keypair keygen fail");
    EVP_PKEY_CTX_free(ctx);
    return keypair;
}

void crypto::free_keypair(EVP_PKEY *kp) {
    EVP_PKEY_free(kp);
}

std::array<uint8_t, 32> crypto::get_pubkey(EVP_PKEY* kp) {
    std::array<uint8_t, 32> pubkey = {};
    size_t len = 32;
    EVP_PKEY_get_raw_public_key(kp, pubkey.data(), &len);
    return pubkey;
}

std::array<uint8_t, 32> crypto::derive_secret(EVP_PKEY *my_kp, const std::array<uint8_t, 32> &peer_pub) {
    EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_pub.data(), 32);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(my_kp, nullptr);
    crypto_check(EVP_PKEY_derive_init(ctx), "DERIVE INIT");
    crypto_check(EVP_PKEY_derive_set_peer(ctx, peer), "SET PEER");
    std::array<uint8_t, 32> secret = {};
    size_t sec_len = 32;
    crypto_check(EVP_PKEY_derive(ctx, secret.data(), &sec_len), "DERIVE");
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer);
    return secret;
}

std::vector<uint8_t> crypto::encrypt(const std::array<uint8_t,32>& key, const std::vector<uint8_t>& plaintext) {
    std::array<uint8_t, 12> nonce{};
    RAND_bytes(nonce.data(), 12);
    std::vector<uint8_t> out;
    out.resize(12 + plaintext.size() + 16); //12 - nonce, 16 - tag
    std::ranges::copy(nonce, out.begin());
    constexpr int nonce_len = 12, tag_len = 16;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    crypto_check(EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "[CRYPTO] encrypt EncryptInit_ex #1 fail");
    crypto_check(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce_len, nullptr), "[CRYPTO] encrypt set IV len fail");
    crypto_check(EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()), "[CRYPTO] encrypt EncryptInit_ex #2 fail");
    int out_len = 0;
    crypto_check(EVP_EncryptUpdate(ctx, out.data() + nonce_len, &out_len, plaintext.data(), static_cast<int>(plaintext.size())), "[CRYPTO] encrypt EncryptUpdate fail");
    int final_len = 0;
    crypto_check(EVP_EncryptFinal_ex(ctx, out.data() + out_len + nonce_len, &final_len), "[CRYPTO] encrypt EncryptFinal fail");
    crypto_check(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag_len, out.data() + out_len + nonce_len), "[CRYPTO] encrypt get tag fail");
    EVP_CIPHER_CTX_free(ctx);
    out.resize(out_len + final_len + nonce_len + tag_len);
    return out;
}

std::optional<std::vector<uint8_t>> crypto::decrypt(const std::array<uint8_t,32>& key, const std::vector<uint8_t>& buf) {
    if (buf.size() < 28) return std::nullopt; //min = 12 (nonce) + 0 (plaintext) + 16 (tag) = 28
    std::vector<uint8_t> plaintext;
    const size_t ct_len = buf.size() - 28; //ct - ciphertext
    const auto nonce_ptr = buf.data();
    auto ct_ptr = buf.data() + 12;
    const auto tag_ptr = buf.data() + 12 + ct_len;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    crypto_check(EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "[CRYPTO] decrypt DecryptInit #1");
    crypto_check(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr), "[CRYPTO] decrypt set IV len");
    crypto_check(EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce_ptr), "[CRYPTO] decrypt DecryptInit #2");
    int out_len = 0;
    plaintext.resize(buf.size());
    crypto_check(EVP_DecryptUpdate(ctx, plaintext.data(), &out_len, ct_ptr, static_cast<int>(ct_len)), "[CRYPTO] decrypt DecryptUpdate");
    int final_len = 0;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag_ptr);
    int ok = EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len, &final_len);
    EVP_CIPHER_CTX_free(ctx);
    if (ok != 1) return std::nullopt;

    plaintext.resize(out_len + final_len);
    return plaintext;
}

std::array<uint8_t, 32> crypto::hkdf_sha256(const std::array<uint8_t, 32> &ikm, const std::string &salt, std::string_view info) {
    uint8_t prk[32];
    unsigned int prk_len = 32;
    HMAC(EVP_sha256(),
        salt.data(), static_cast<int>(salt.size()),
        ikm.data(), 32,
        prk, &prk_len);
    std::vector<uint8_t> expand_input(info.begin(), info.end());
    expand_input.push_back(0x01);

    std::array<uint8_t, 32> out{};
    unsigned int out_len = 32;
    HMAC(EVP_sha256(), prk, 32, expand_input.data(), static_cast<int>(expand_input.size()), out.data(), &out_len);
    return out;
}
