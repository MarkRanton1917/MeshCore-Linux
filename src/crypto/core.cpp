#include "core.h"
#include "aes128.h"

#include <cerrno>
#include <cstring>
#include <sodium.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace crypto::core;

static_assert(PACKET_SEED_SIZE == crypto_sign_SEEDBYTES);
static_assert(PACKET_MONT_KEY_SIZE == crypto_scalarmult_curve25519_BYTES);
static_assert(PACKET_CIPHER_KEY_SIZE == AES128_KEY_SIZE);
static_assert(PACKET_CIPHER_BLOCK_SIZE == AES128_BLOCK_SIZE);

static RandomFn randomSource = nullptr;

static bool makeParentDir(const std::string& path);
static bool writeAll(int fd, const std::uint8_t* data, size_t size);
static bool readAll(int fd, std::uint8_t* data, size_t size);
static const std::uint8_t* pointerOrEmpty(ByteView view);

void crypto::core::secureZero(ByteSpan buf)
{
  if (!buf.empty()) sodium_memzero(buf.data(), buf.size());
}

bool crypto::core::constantTimeEqual(ByteView a, ByteView b)
{
  if (a.size() != b.size()) return false; // the length is not a secret
  if (a.empty()) return true;
  return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool crypto::core::init(RandomFn fn)
{
  randomSource = fn;
  return sodium_init() >= 0;
}

void crypto::core::randomBytes(ByteSpan out)
{
  if (out.empty()) return; // data() of an empty span is nullptr

  if (randomSource != nullptr) {
    randomSource(out);
    return;
  }
  randombytes_buf(out.data(), out.size());
}

KeyPair crypto::core::generateKeypair()
{
  // Ed25519 unfolds deterministically from a 32-byte seed. Seed wipes itself.
  Seed seed;
  randomBytes(seed.span());

  KeyPair keypair {};
  crypto_sign_seed_keypair(keypair.pk.span().data(), keypair.sk.span().data(), seed.view().data());
  return keypair;
}

bool crypto::core::saveKeypair(const KeyPair& keypair, const std::string& path)
{
  if (!makeParentDir(path)) return false;

  // O_EXCL: an existing key is never overwritten, losing it means a new identity.
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return false;

  // umask may have stripped bits off the mode passed to open().
  if (fchmod(fd, 0600) != 0 || !writeAll(fd, keypair.sk.view().data(), keypair.sk.size) || fsync(fd) != 0) {
    close(fd);
    unlink(path.c_str()); // a half-written key is worse than none
    return false;
  }
  return close(fd) == 0;
}

bool crypto::core::loadKeypair(KeyPair& keypair, const std::string& path)
{
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;

  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)
    || (info.st_mode & (S_IRWXG | S_IRWXO)) != 0 // reachable by someone else
    || static_cast<size_t>(info.st_size) != PACKET_PRIVATE_KEY_SIZE) {
    close(fd);
    return false;
  }

  bool complete = readAll(fd, keypair.sk.span().data(), keypair.sk.size);
  close(fd);
  if (!complete) return false;

  // Rebuild the pair from its own seed half to catch a corrupted file.
  Seed seed;
  if (crypto_sign_ed25519_sk_to_seed(seed.span().data(), keypair.sk.view().data()) != 0) {
    return false;
  }

  KeyPair derived {};
  crypto_sign_seed_keypair(derived.pk.span().data(), derived.sk.span().data(), seed.view().data());

  if (sodium_memcmp(derived.sk.view().data(), keypair.sk.view().data(), keypair.sk.size) != 0) {
    return false;
  }
  keypair.pk = derived.pk;
  return true;
}

std::optional<SharedSecret> crypto::core::deriveShared(const PrivateKey& sk, const PublicKey& pk)
{
  // Signing lives on the Edwards form of the curve, key exchange on Montgomery.
  // The peer key is checked first: it is the only one that comes from outside.
  MontPublic montgomeryPk;
  if (crypto_sign_ed25519_pk_to_curve25519(montgomeryPk.span().data(), pk.view().data()) != 0) {
    return std::nullopt;
  }

  // MontPrivate wipes itself, including on the early return below.
  MontPrivate montgomerySk;
  if (crypto_sign_ed25519_sk_to_curve25519(montgomerySk.span().data(), sk.view().data()) != 0) {
    return std::nullopt;
  }

  // The backend clamps the scalar and rejects a zero result, which is what
  // catches small-order points.
  SharedSecret secret {};
  if (crypto_scalarmult(secret.span().data(), montgomerySk.view().data(), montgomeryPk.view().data()) != 0) {
    return std::nullopt;
  }
  return secret;
}

Hash crypto::core::sha256(std::span<const ByteView> chunks)
{
  crypto_hash_sha256_state state;
  crypto_hash_sha256_init(&state);
  for (const ByteView& chunk : chunks) {
    if (!chunk.empty()) crypto_hash_sha256_update(&state, chunk.data(), chunk.size());
  }

  Hash out;
  crypto_hash_sha256_final(&state, out.data.data());
  return out;
}

Hash crypto::core::sha256(ByteView message)
{
  const ByteView chunks[] = { message };
  return sha256(std::span<const ByteView> { chunks });
}

Hash crypto::core::hmacSha256(ByteView key, ByteView message)
{
  // The streaming form accepts a key of any length and applies RFC 2104 itself.
  crypto_auth_hmacsha256_state state;
  crypto_auth_hmacsha256_init(&state, pointerOrEmpty(key), key.size());
  crypto_auth_hmacsha256_update(&state, pointerOrEmpty(message), message.size());

  Hash out;
  crypto_auth_hmacsha256_final(&state, out.data.data());
  sodium_memzero(&state, sizeof(state));
  return out;
}

Signature crypto::core::sign(const PrivateKey& sk, ByteView message)
{
  Signature signature {};
  crypto_sign_detached(signature.data.data(), nullptr, pointerOrEmpty(message), message.size(), sk.view().data());
  return signature;
}

bool crypto::core::verify(const PublicKey& pk, ByteView message, const Signature& signature)
{
  return crypto_sign_verify_detached(signature.data.data(), pointerOrEmpty(message), message.size(), pk.view().data())
    == 0;
}

bool crypto::core::aesEncrypt(const CipherKey& key, ByteView in, ByteSpan out)
{
  if (in.size() % PACKET_CIPHER_BLOCK_SIZE != 0 || out.size() < in.size()) return false;

  aes128_ctx ctx;
  aes128_init(&ctx, key.view().data());
  for (size_t offset = 0; offset < in.size(); offset += PACKET_CIPHER_BLOCK_SIZE) {
    aes128_encrypt_block(&ctx, in.data() + offset, out.data() + offset);
  }
  aes128_clear(&ctx);
  return true;
}

bool crypto::core::aesDecrypt(const CipherKey& key, ByteView in, ByteSpan out)
{
  if (in.size() % PACKET_CIPHER_BLOCK_SIZE != 0 || out.size() < in.size()) return false;

  aes128_ctx ctx;
  aes128_init(&ctx, key.view().data());
  for (size_t offset = 0; offset < in.size(); offset += PACKET_CIPHER_BLOCK_SIZE) {
    aes128_decrypt_block(&ctx, in.data() + offset, out.data() + offset);
  }
  aes128_clear(&ctx);
  return true;
}

// An empty span has data() == nullptr, which parts of the backend dislike.
static const std::uint8_t* pointerOrEmpty(ByteView view)
{
  static const std::uint8_t empty = 0;
  return view.empty() ? &empty : view.data();
}

// Creates the immediate parent only, does not expand nested paths.
static bool makeParentDir(const std::string& path)
{
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return true; // current directory or root

  const std::string dir = path.substr(0, slash);
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
  return true;
}

static bool writeAll(int fd, const std::uint8_t* data, size_t size)
{
  while (size > 0) {
    const ssize_t written = write(fd, data, size);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

static bool readAll(int fd, std::uint8_t* data, size_t size)
{
  while (size > 0) {
    const ssize_t got = read(fd, data, size);
    if (got < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (got == 0) return false; // file ended early
    data += got;
    size -= static_cast<size_t>(got);
  }
  return true;
}
