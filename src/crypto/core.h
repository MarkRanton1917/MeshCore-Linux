// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#pragma once

#include "defines.h"

#include <array>
#include <span>
#include <cstdint>
#include <optional>
#include <string>

// Cryptographic primitives and key material. Nothing here knows what a MeshCore
// packet looks like: every protocol-specific decision lives in protocol.h.
namespace crypto::core {

// Wipe the optimiser is not allowed to drop. Declared first: Secret's inline
// destructor calls it, and the name must be visible by then.
void secureZero(ByteSpan buf);

// Fixed-length public data.
template<std::size_t N>
struct Bytes {
  static constexpr std::size_t size = N;
  std::array<std::uint8_t, N> data {};

  ByteView view() const noexcept
  {
    return { data.data(), N };
  }
  ByteSpan span() noexcept
  {
    return { data.data(), N };
  }
  std::uint8_t* begin() noexcept
  {
    return data.data();
  }
  std::uint8_t* end() noexcept
  {
    return data.data() + N;
  }
};

// Same, but wiped on destruction. For everything secret.
template<std::size_t N>
class Secret {
public:
  static constexpr std::size_t size = N;

  Secret() = default;
  ~Secret()
  {
    secureZero(ByteSpan { data_.data(), N });
  }
  Secret(const Secret&) = default;
  Secret& operator=(const Secret&) = default;

  ByteView view() const noexcept
  {
    return { data_.data(), N };
  }
  ByteSpan span() noexcept
  {
    return { data_.data(), N };
  }

private:
  std::array<std::uint8_t, N> data_ {};
};

using PublicKey = Bytes<PACKET_PUBLIC_KEY_SIZE>; // Ed25519, also the node id
using Signature = Bytes<PACKET_SIGNATURE_SIZE>;
using Hash = Bytes<PACKET_HASH_SIZE>; // SHA-256 / HMAC-SHA256
using MontPublic = Bytes<PACKET_MONT_KEY_SIZE>; // X25519 after conversion

using PrivateKey = Secret<PACKET_PRIVATE_KEY_SIZE>; // Ed25519, backend format (seed + pk)
using MontPrivate = Secret<PACKET_MONT_KEY_SIZE>;
using SharedSecret = Secret<PACKET_SHARED_SECRET_SIZE>; // ECDH result
using CipherKey = Secret<PACKET_CIPHER_KEY_SIZE>; // AES-128
using Seed = Secret<PACKET_SEED_SIZE>; // Ed25519 unfolds from this

using RandomFn = void (*)(ByteSpan);

struct KeyPair {
  PublicKey pk;
  PrivateKey sk;
};

// Constant-time comparison. Mandatory for MAC checks; length is not a secret.
bool constantTimeEqual(ByteView a, ByteView b);

// Starts the backend. fn replaces the random source, nullptr restores the system one.
bool init(RandomFn fn);

// Fills the buffer with cryptographically strong random bytes.
void randomBytes(ByteSpan out);

// Creates a node identity from a fresh 32-byte seed.
KeyPair generateKeypair();

// Reads a keypair. Rejects a file of the wrong size, corrupt, or readable by others.
bool loadKeypair(KeyPair& keypair, const std::string& path);

// Writes a keypair with mode 0600. Never overwrites an existing file.
bool saveKeypair(const KeyPair& keypair, const std::string& path);

// X25519 shared secret. Symmetric; nullopt on a degenerate peer key.
// Expensive — cache it per contact instead of deriving it per packet.
std::optional<SharedSecret> deriveShared(const PrivateKey& sk, const PublicKey& pk);

// SHA-256 over the chunks in order, without joining the buffers first.
Hash sha256(std::span<const ByteView> chunks);
Hash sha256(ByteView message);

// HMAC-SHA256 per RFC 2104. Accepts a key of any length.
Hash hmacSha256(ByteView key, ByteView message);

// Ed25519 detached signature. Deterministic: the same input always signs the same.
Signature sign(const PrivateKey& sk, ByteView message);
bool verify(const PublicKey& pk, ByteView message, const Signature& signature);

// AES-128 in ECB over whole blocks; in and out may be the same buffer.
// False if the length is not a whole number of blocks or out is too small.
bool aesEncrypt(const CipherKey& key, ByteView in, ByteSpan out);
bool aesDecrypt(const CipherKey& key, ByteView in, ByteSpan out);

} // namespace crypto::core
