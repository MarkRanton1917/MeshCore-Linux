// AES-128 block cipher core (FIPS-197).
//
// libsodium does not expose AES-128 in any form -- only AES-256-GCM, and only
// on CPUs with AES-NI. The protocol needs AES-128, so the raw block cipher
// lives here instead of in crypto.cpp. This is the textbook FIPS-197
// construction, nothing invented; it is checked against the FIPS-197 Appendix B
// / C.1 vectors in the test suite.
//
// This is the block cipher only. Modes of operation and padding are in
// crypto.cpp -- do not call these directly from protocol code.

#ifndef MESH_AES128_H
#define MESH_AES128_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AES128_KEY_SIZE   16
#define AES128_BLOCK_SIZE 16
#define AES128_ROUNDS     10

  typedef struct {
    uint8_t round_keys[(AES128_ROUNDS + 1) * AES128_BLOCK_SIZE];
  } aes128_ctx;

  // Expands a 16-byte key into the 11 round keys.
  void aes128_init(aes128_ctx* ctx, const uint8_t key[AES128_KEY_SIZE]);

  // Single-block ECB. in and out may alias.
  void aes128_encrypt_block(const aes128_ctx* ctx, const uint8_t in[AES128_BLOCK_SIZE], uint8_t out[AES128_BLOCK_SIZE]);
  void aes128_decrypt_block(const aes128_ctx* ctx, const uint8_t in[AES128_BLOCK_SIZE], uint8_t out[AES128_BLOCK_SIZE]);

  // Wipes the expanded key schedule.
  void aes128_clear(aes128_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif // MESH_AES128_H
