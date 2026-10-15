#pragma once
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <cassert>
#include <cstring>
extern "C" void mesh_aes_block_encrypt(const uint8_t *key, int bits, const uint8_t in[16], uint8_t out[16]) {
    auto *ctx = EVP_CIPHER_CTX_new(); int length = 0;
    assert(EVP_EncryptInit_ex(ctx, bits == 128 ? EVP_aes_128_ecb() : EVP_aes_256_ecb(), nullptr, key, nullptr) == 1);
    EVP_CIPHER_CTX_set_padding(ctx, 0); assert(EVP_EncryptUpdate(ctx, out, &length, in, 16) == 1 && length == 16); EVP_CIPHER_CTX_free(ctx);
}
extern "C" bool mesh_x25519_dh(const uint8_t priv[32], const uint8_t pub[32], uint8_t shared[32]) {
    auto *local = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv, 32);
    auto *remote = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, pub, 32);
    auto *ctx = EVP_PKEY_CTX_new(local, nullptr); size_t length = 32;
    bool ok = ctx && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, remote) == 1 && EVP_PKEY_derive(ctx, shared, &length) == 1;
    EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(local); EVP_PKEY_free(remote); return ok;
}
extern "C" void mesh_sha256(const uint8_t *data, size_t length, uint8_t out[32]) { SHA256(data, length, out); }
extern "C" bool mesh_generate_keypair(uint8_t pub[32], uint8_t priv[32]) {
    if (RAND_bytes(priv, 32) != 1) return false;
    priv[0] &= 248; priv[31] &= 127; priv[31] |= 64;
    auto *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv, 32); size_t length = 32;
    const bool ok = key && EVP_PKEY_get_raw_public_key(key, pub, &length) == 1; EVP_PKEY_free(key); return ok;
}
extern "C" bool mesh_ccm_encrypt(const uint8_t key[32], const uint8_t nonce[13], const uint8_t *plain, size_t length, uint8_t *cipher, uint8_t *tag, size_t tagLength) {
    auto *ctx = EVP_CIPHER_CTX_new(); int n = 0;
    const bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_ccm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_IVLEN, 13, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_TAG, tagLength, nullptr) == 1 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1 &&
        EVP_EncryptUpdate(ctx, nullptr, &n, nullptr, length) == 1 &&
        EVP_EncryptUpdate(ctx, cipher, &n, plain, length) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_GET_TAG, tagLength, tag) == 1;
    EVP_CIPHER_CTX_free(ctx); return ok;
}
extern "C" bool mesh_ccm_decrypt(const uint8_t key[32], const uint8_t nonce[13], const uint8_t *cipher, size_t length, const uint8_t *tag, size_t tagLength, uint8_t *plain) {
    auto *ctx = EVP_CIPHER_CTX_new(); int n = 0;
    const bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_ccm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_IVLEN, 13, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_TAG, tagLength, const_cast<uint8_t *>(tag)) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1 &&
        EVP_DecryptUpdate(ctx, nullptr, &n, nullptr, length) == 1 && EVP_DecryptUpdate(ctx, plain, &n, cipher, length) == 1;
    EVP_CIPHER_CTX_free(ctx); return ok;
}
