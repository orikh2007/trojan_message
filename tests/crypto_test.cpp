//
// Created by Ori Kedar Haspel on 03/05/2026.
//

#include "crypto_test.h"

#include <iostream>
#include <ostream>

#include "../network/headers/Msg.h"

crypto_test::crypto_test() {
    key_ = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    nonce_ = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
        0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b
    };
    keypair_ = gen_x25519_keypair();
}

void crypto_check(const int code, const char* where) {
    if (code != 1) {
        ERR_print_errors_fp(stderr);
        assert(false && where);
    }
}

std::vector<uint8_t> crypto_test::sym_encrypt_data(const std::vector<uint8_t> &data) const {
    std::vector<uint8_t> ciphertext_buf;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    crypto_check(EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "EncryptInit #1");
    crypto_check(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr), "IV len");
    crypto_check(EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce_.data()), "EncryptInit #2");
    int out_len = 0;
    ciphertext_buf.resize(data.size());
    crypto_check(EVP_EncryptUpdate(ctx, ciphertext_buf.data(), &out_len, data.data(), static_cast<int>(data.size())), "UPDATE #1");
    int final_len = 0;
    crypto_check(EVP_EncryptFinal_ex(ctx, ciphertext_buf.data() + out_len, &final_len), "FINAL");
    uint8_t tag[16];
    crypto_check(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag), "TAG");
    EVP_CIPHER_CTX_free(ctx);
    ciphertext_buf.resize(out_len + final_len);
    ciphertext_buf.insert(ciphertext_buf.end(), tag, tag + 16);
    return ciphertext_buf;
}

std::optional<std::vector<uint8_t>> crypto_test::sym_decrypt_data(const std::vector<uint8_t> &data) const {
    std::vector<uint8_t> text_buf;
    size_t ct_len = data.size() - 16;
    const uint8_t* tag_ptr = data.data() + ct_len;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    crypto_check(EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "EncryptInit #1");
    crypto_check(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr), "IV len");
    crypto_check(EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce_.data()), "EncryptInit #2");
    int out_len = 0;
    text_buf.resize(data.size());
    crypto_check(EVP_DecryptUpdate(ctx, text_buf.data(), &out_len, data.data(), static_cast<int>(ct_len)), "UPDATE #1");
    int final_len = 0;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag_ptr);
    int ok = EVP_DecryptFinal_ex(ctx, text_buf.data() + out_len, &final_len);
    EVP_CIPHER_CTX_free(ctx);

    text_buf.resize(out_len + final_len);

    if (ok != 1) return std::nullopt;

    return text_buf;
}

EVP_PKEY *crypto_test::gen_x25519_keypair() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    crypto_check(EVP_PKEY_keygen_init(ctx), "INIT");
    EVP_PKEY* keypair = nullptr;
    crypto_check(EVP_PKEY_keygen(ctx, &keypair), "keygen");
    EVP_PKEY_CTX_free(ctx);
    return keypair;
}

std::array<uint8_t, 32> crypto_test::get_pubkey(EVP_PKEY* keypair) {
    std::array<uint8_t, 32> pubkey = {};
    size_t len = 32;
    EVP_PKEY_get_raw_public_key(keypair, pubkey.data(), &len);
    return pubkey;
}

std::array<uint8_t, 32> derive_secret(EVP_PKEY* my_keypair, const std::array<uint8_t, 32>& peer_raw_pubkey) {
    EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_raw_pubkey.data(), 32);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(my_keypair, nullptr);
    crypto_check(EVP_PKEY_derive_init(ctx), "DERIVE INIT");
    crypto_check(EVP_PKEY_derive_set_peer(ctx, peer), "SET PEER");
    std::array<uint8_t, 32> secret = {};
    size_t sec_len = 32;
    crypto_check(EVP_PKEY_derive(ctx, secret.data(), &sec_len), "DERIVE");
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer);
    return secret;
}

void crypto_test::run_crypto_tests_sym() {
    std::vector<uint8_t> plaintext = {'h', 'e', 'l', 'l', 'o'};

    // Test 1: round-trip
    {
        auto cipher = sym_encrypt_data(plaintext);
        auto result = sym_decrypt_data(cipher);
        assert(result.has_value());
        assert(result.value() == plaintext);
        std::cout << "Test 1 passed (round-trip)\n";
    }

    // Test 2: ciphertext differs from plaintext
    {
        auto cipher = sym_encrypt_data(plaintext);
        assert(cipher != plaintext);
        std::cout << "Test 2 passed (ciphertext != plaintext)\n";
    }

    // Test 3: ciphertext length == plaintext length + 16
    {
        auto cipher = sym_encrypt_data(plaintext);
        assert(cipher.size() == plaintext.size() + 16);
        std::cout << "Test 3 passed (ciphertext length)\n";
    }

    // Test 4: tag corruption causes decryption failure
    {
        auto cipher = sym_encrypt_data(plaintext);
        cipher.back() ^= 0xFF; // flip a byte in the tag (last 16 bytes)
        auto result = sym_decrypt_data(cipher);
        assert(!result.has_value());
        std::cout << "Test 4 passed (tag corruption)\n";
    }

    // Test 5: ciphertext body corruption causes decryption failure
    {
        auto cipher = sym_encrypt_data(plaintext);
        cipher[0] ^= 0xFF; // flip a byte in the ciphertext body
        auto result = sym_decrypt_data(cipher);
        assert(!result.has_value());
        std::cout << "Test 5 passed (ciphertext corruption)\n";
    }

    // Test 6: wrong nonce causes decryption failure
    {
        auto cipher = sym_encrypt_data(plaintext);
        std::array<uint8_t, 12> bad_nonce = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        std::array<uint8_t, 12> saved_nonce = nonce_;
        nonce_ = bad_nonce;
        auto result = sym_decrypt_data(cipher);
        nonce_ = saved_nonce;
        assert(!result.has_value());
        std::cout << "Test 6 passed (wrong nonce)\n";
    }

    // Test 7: wrong key causes decryption failure
    {
        auto cipher = sym_encrypt_data(plaintext);
        std::array<uint8_t, 32> bad_key;
        bad_key.fill(0xFF);
        std::array<uint8_t, 32> saved_key = key_;
        key_ = bad_key;
        auto result = sym_decrypt_data(cipher);
        key_ = saved_key;
        assert(!result.has_value());
        std::cout << "Test 7 passed (wrong key)\n";
    }

    // Test 8: empty plaintext
    {
        std::vector<uint8_t> empty = {};
        auto cipher = sym_encrypt_data(empty);
        assert(cipher.size() == 16); // just the tag
        auto result = sym_decrypt_data(cipher);
        assert(result.has_value());
        assert(result.value().empty());
        std::cout << "Test 8 passed (empty plaintext)\n";
    }

    std::cout << "All tests passed.\n";
}

void crypto_test::run_crypto_tests_asym() {
    // Test 1: Alice and Bob derive the same shared secret
    {
        EVP_PKEY* alice = gen_x25519_keypair();
        EVP_PKEY* bob   = gen_x25519_keypair();

        auto alice_pub = get_pubkey(alice);
        auto bob_pub   = get_pubkey(bob);

        auto secret_alice = derive_secret(alice, bob_pub);
        auto secret_bob   = derive_secret(bob, alice_pub);

        assert(secret_alice == secret_bob);
        std::cout << "Asym Test 1 passed (shared secret matches)\n";

        EVP_PKEY_free(alice);
        EVP_PKEY_free(bob);
    }

    // Test 2: different peers produce different secrets
    {
        EVP_PKEY* alice = gen_x25519_keypair();
        EVP_PKEY* bob   = gen_x25519_keypair();
        EVP_PKEY* carol = gen_x25519_keypair();

        auto bob_pub   = get_pubkey(bob);
        auto carol_pub = get_pubkey(carol);

        auto secret_with_bob   = derive_secret(alice, bob_pub);
        auto secret_with_carol = derive_secret(alice, carol_pub);

        assert(secret_with_bob != secret_with_carol);
        std::cout << "Asym Test 2 passed (different peers -> different secrets)\n";

        EVP_PKEY_free(alice);
        EVP_PKEY_free(bob);
        EVP_PKEY_free(carol);
    }

    // Test 3: pubkey round-trip — import raw bytes, derive same secret
    {
        EVP_PKEY* alice = gen_x25519_keypair();
        EVP_PKEY* bob   = gen_x25519_keypair();

        auto bob_pub_bytes = get_pubkey(bob);

        auto secret_direct = derive_secret(alice, bob_pub_bytes);

        EVP_PKEY* bob_imported    = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                                 bob_pub_bytes.data(), 32);
        auto bob_imported_pub     = get_pubkey(bob_imported);
        auto secret_imported      = derive_secret(alice, bob_imported_pub);

        assert(secret_direct == secret_imported);
        std::cout << "Asym Test 3 passed (pubkey round-trip)\n";

        EVP_PKEY_free(alice);
        EVP_PKEY_free(bob);
        EVP_PKEY_free(bob_imported);
    }

    std::cout << "All asym tests passed.\n";
}
