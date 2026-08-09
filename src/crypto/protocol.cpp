#include "protocol.h"
#include "packet.h"

#include <cstring>

// Decisions the header leaves open. Each is an assumption checked only against
// real traffic, so they are kept in one place:
//
//  1. Encrypt-then-MAC. The MAC covers the ciphertext and open() checks it
//     before decrypting, which rules out a padding oracle.
//  2. The HMAC key is the whole shared secret; the AES key is its first
//     16 bytes.
//  3. PKCS#7 padding, and a whole block is added even when the length already
//     divides evenly. Only because it is self-describing can open() report an
//     exact plaintext length.
//  4. cipherKeyFrom() truncates the ECDH result with no KDF. That is what the
//     protocol does; hashing it would be stronger but would not interoperate.

using namespace crypto;
using namespace crypto::protocol;

static size_t paddedLength(size_t length);
static Mac truncateMac(const Hash& full);

CipherKey crypto::protocol::cipherKeyFrom(const SharedSecret& secret)
{
  CipherKey key;
  std::memcpy(key.span().data(), secret.view().data(), CipherKey::size);
  return key;
}

Hash crypto::protocol::packetHash(ByteView frame)
{
  const auto parsed = packet::parse(frame);
  if (!parsed.has_value()) return {};

  // The path is deliberately left out: it grows on every hop.
  const ByteView chunks[] = { ByteView { &parsed->header, PACKET_HEADER_SIZE }, parsed->payloadView() };
  return core::sha256(chunks);
}

Signature crypto::protocol::packetSign(const PrivateKey& sk, ByteView frame)
{
  const auto parsed = packet::parse(frame);
  if (!parsed.has_value()) return {};

  if (parsed->payloadType() != packet::PayloadType::ADVERT) return {};

  const auto advert = packet::decodeAdvert(parsed->payloadView());
  if (!advert.has_value()) return {};

  std::array<std::uint8_t, MAX_PACKET_PAYLOAD> buffer;
  const size_t size = advert->signedMessage(buffer);
  if (size == 0) return {};

  return core::sign(sk, ByteView { buffer.data(), size });
}

bool crypto::protocol::packetVerify(ByteView frame)
{
  const auto parsed = packet::parse(frame);
  if (!parsed.has_value()) return false;

  if (parsed->payloadType() != packet::PayloadType::ADVERT) return false;

  const auto advert = packet::decodeAdvert(parsed->payloadView());
  if (!advert.has_value()) return false;

  std::array<std::uint8_t, MAX_PACKET_PAYLOAD> buffer;
  const size_t size = advert->signedMessage(buffer);
  if (size == 0) return false;

  // decodeAdvert slices both fields to a fixed length, so this only guards
  // against a future change there.
  if (advert->publicKey.size() != PublicKey::size) return false;
  if (advert->signature.size() != Signature::size) return false;

  PublicKey pk;
  std::memcpy(pk.data.data(), advert->publicKey.data(), PublicKey::size);
  Signature signature;
  std::memcpy(signature.data.data(), advert->signature.data(), Signature::size);

  return core::verify(pk, ByteView { buffer.data(), size }, signature);
}

std::optional<Sealed> crypto::protocol::seal(const SharedSecret& secret, ByteView plaintext, ByteSpan out)
{
  const size_t total = paddedLength(plaintext.size());
  if (out.size() < total) return std::nullopt;

  if (!plaintext.empty()) std::memcpy(out.data(), plaintext.data(), plaintext.size());

  const std::uint8_t pad = static_cast<std::uint8_t>(total - plaintext.size());
  std::memset(out.data() + plaintext.size(), pad, pad);

  const ByteSpan block = out.subspan(0, total);
  if (!core::aesEncrypt(cipherKeyFrom(secret), ByteView { block.data(), block.size() }, block)) {
    return std::nullopt;
  }

  Sealed sealed;
  sealed.mac = truncateMac(core::hmacSha256(secret.view(), ByteView { block.data(), block.size() }));
  sealed.ciphertextLength = total;
  return sealed;
}

std::optional<size_t> crypto::protocol::open(const SharedSecret& secret,
  const Mac& mac,
  ByteView ciphertext,
  ByteSpan out)
{
  if (ciphertext.empty() || ciphertext.size() % PACKET_CIPHER_BLOCK_SIZE != 0) {
    return std::nullopt;
  }
  if (out.size() < ciphertext.size()) return std::nullopt;

  // MAC first, decryption second, or bad padding becomes observable from outside.
  const Mac expected = truncateMac(core::hmacSha256(secret.view(), ciphertext));
  if (!core::constantTimeEqual(expected.view(), mac.view())) return std::nullopt;

  if (!core::aesDecrypt(cipherKeyFrom(secret), ciphertext, out)) return std::nullopt;

  // Padding is checked after authentication, so leaving early here tells an
  // attacker nothing: only someone holding the key gets this far.
  const size_t length = ciphertext.size();
  const std::uint8_t pad = out[length - 1];
  if (pad == 0 || pad > PACKET_CIPHER_BLOCK_SIZE || pad > length) return std::nullopt;
  for (size_t i = length - pad; i < length; i++) {
    if (out[i] != pad) return std::nullopt;
  }
  return length - pad;
}

std::uint32_t crypto::protocol::expectedAck(ByteView payload, const PublicKey& recipient)
{
  const ByteView chunks[] = { payload, recipient.view() };
  const Hash digest = core::sha256(chunks);

  return static_cast<std::uint32_t>(digest.data[0]) | (static_cast<std::uint32_t>(digest.data[1]) << 8)
    | (static_cast<std::uint32_t>(digest.data[2]) << 16) | (static_cast<std::uint32_t>(digest.data[3]) << 24);
}

// PKCS#7 always adds between 1 and one whole block.
static size_t paddedLength(size_t length)
{
  return length + (PACKET_CIPHER_BLOCK_SIZE - (length % PACKET_CIPHER_BLOCK_SIZE));
}

static Mac truncateMac(const Hash& full)
{
  Mac mac;
  std::memcpy(mac.data.data(), full.data.data(), Mac::size);
  return mac;
}
