// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#pragma once

#include "core.h"

// MeshCore-specific constructions: what is fed into the primitives and in which
// order. Everything that a protocol revision could change lives here and only
// here. Depends on core; core never depends on this.
namespace crypto::protocol {

using core::CipherKey;
using core::Hash;
using core::PrivateKey;
using core::PublicKey;
using core::SharedSecret;
using core::Signature;

using Mac = core::Bytes<PACKET_MAC_SIZE>; // HMAC truncated for the packet

struct Sealed {
  Mac mac;
  std::size_t ciphertextLength = 0;
};

// The AES key is the head of the shared secret, taken raw with no KDF.
CipherKey cipherKeyFrom(const SharedSecret& secret);

// Deduplication key: SHA-256 over the header and the payload. The path is left
// out on purpose, it grows on every hop.
Hash packetHash(ByteView frame);

// Signs the advert fields: public key, timestamp and appdata.
Signature packetSign(const PrivateKey& sk, ByteView frame);

// Checks an advert against the key it carries. Proves authorship, not freshness.
bool packetVerify(ByteView frame);

// Encrypts and authenticates: AES-128-ECB, PKCS#7, HMAC-SHA256 cut to 2 bytes.
// No IV and no nonce, so repeats are visible on air and replay defence belongs
// to the layer above. out must hold the plaintext rounded up to a whole block.
std::optional<Sealed> seal(const SharedSecret& secret, ByteView plaintext, ByteSpan out);

// Checks the MAC in constant time, then decrypts. Returns the plaintext length.
// nullopt means "not for us" or "corrupt", indistinguishable on purpose: about
// one foreign packet in 256 lands here, so log at debug and count it, never warn.
std::optional<size_t> open(const SharedSecret& secret, const Mac& mac, ByteView ciphertext, ByteSpan out);

// Delivery receipt: SHA-256 over the payload and the recipient key, first 4 bytes.
// The payload must be the ciphertext that went on air, never the plaintext.
// A correlation id, not authentication: anyone who saw the packet can forge it.
std::uint32_t expectedAck(ByteView payload, const PublicKey& recipient);

} // namespace crypto::protocol
