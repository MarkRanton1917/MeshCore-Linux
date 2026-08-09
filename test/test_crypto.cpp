// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

// Tests for src/crypto.
//
// Primitives are pinned to published vectors rather than to themselves: a
// wrapper that concatenates its inputs in the wrong order would otherwise pass
// on internal consistency alone.
//
//   Ed25519      RFC 8032 section 7.1
//   SHA-256      FIPS 180-4
//   HMAC-SHA256  RFC 4231
//   AES-128 ECB  FIPS-197 Appendix C.1
//
// The protocol layer has no published vectors and cannot have any, so it is
// checked by properties (round-trip, refusal on any corruption) and by pinning
// the assumptions themselves: what the signature covers, the padding scheme,
// the encrypt-then-MAC order.

#include "check.h"
#include "core.h"
#include "protocol.h"
#include "packet.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

using check::equalsHex;
using check::fromHex;
using check::section;
using check::that;

namespace core = crypto::core;
namespace protocol = crypto::protocol;

// ------------------------------------------------------------------ utilities

static ByteView view(const std::vector<uint8_t>& v)
{
  return ByteView { v.data(), v.size() };
}

// A random source that replays a fixed buffer, so key generation becomes
// reproducible and can be checked against published vectors.
static std::vector<uint8_t> scriptedBytes;
static void scriptedRandom(ByteSpan out)
{
  for (size_t i = 0; i < out.size(); i++)
    out[i] = scriptedBytes[i % scriptedBytes.size()];
}

static core::KeyPair keypairFromSeed(const char* seedHex)
{
  scriptedBytes = fromHex(seedHex);
  core::init(scriptedRandom);
  core::KeyPair keypair = core::generateKeypair();
  core::init(nullptr);
  return keypair;
}

// Builds an advert frame with the signature slot zeroed, plus its offset.
static std::vector<uint8_t> advertFrame(const core::PublicKey& pk,
  uint32_t timestamp,
  const std::vector<uint8_t>& appdata,
  size_t& signatureAt,
  uint8_t route = 0x01)
{
  std::vector<uint8_t> frame;
  frame.push_back((uint8_t)(((uint8_t)packet::PayloadType::ADVERT << 2) | route));
  if (route == 0x00 || route == 0x03) {
    for (int i = 0; i < PACKET_TRANSPORT_CODES_SIZE; i++)
      frame.push_back((uint8_t)(i + 1));
  }
  frame.push_back(0x00); // path_length: no hops

  frame.insert(frame.end(), pk.data.begin(), pk.data.end());
  for (int i = 0; i < PACKET_TIMESTAMP_SIZE; i++) {
    frame.push_back((uint8_t)(timestamp >> (8 * i)));
  }

  signatureAt = frame.size();
  frame.resize(frame.size() + PACKET_SIGNATURE_SIZE, 0);
  frame.insert(frame.end(), appdata.begin(), appdata.end());
  return frame;
}

// ------------------------------------------------------------------ core

static void testVectors()
{
  section("core: published vectors");

  // FIPS 180-4.
  std::vector<uint8_t> abc { 'a', 'b', 'c' };
  that("SHA-256(\"abc\")",
    equalsHex(core::sha256(view(abc)).view(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

  // Chunked and single-shot must agree, or packetHash would drift.
  std::vector<uint8_t> a { 'a' }, bc { 'b', 'c' };
  const ByteView chunks[] = { view(a), view(bc) };
  that("chunked SHA-256 matches single-shot",
    std::memcmp(core::sha256(chunks).data.data(), core::sha256(view(abc)).data.data(), PACKET_HASH_SIZE) == 0);

  // RFC 4231 test case 1.
  std::vector<uint8_t> hmacKey(20, 0x0b);
  std::vector<uint8_t> hmacData { 'H', 'i', ' ', 'T', 'h', 'e', 'r', 'e' };
  that("HMAC-SHA256 RFC 4231 case 1",
    equalsHex(core::hmacSha256(view(hmacKey), view(hmacData)).view(),
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

  // FIPS-197 Appendix C.1.
  core::CipherKey aesKey;
  std::vector<uint8_t> keyBytes = fromHex("000102030405060708090a0b0c0d0e0f");
  std::memcpy(aesKey.span().data(), keyBytes.data(), keyBytes.size());

  std::vector<uint8_t> plain = fromHex("00112233445566778899aabbccddeeff");
  std::vector<uint8_t> encrypted(PACKET_CIPHER_BLOCK_SIZE);
  that("AES-128 encrypt FIPS-197 C.1",
    core::aesEncrypt(aesKey, view(plain), ByteSpan { encrypted.data(), encrypted.size() })
      && equalsHex(view(encrypted), "69c4e0d86a7b0430d8cdb78070b4c55a"));

  std::vector<uint8_t> decrypted(PACKET_CIPHER_BLOCK_SIZE);
  that("AES-128 decrypt FIPS-197 C.1",
    core::aesDecrypt(aesKey, view(encrypted), ByteSpan { decrypted.data(), decrypted.size() })
      && std::memcmp(decrypted.data(), plain.data(), plain.size()) == 0);

  that("AES refuses a partial block",
    !core::aesEncrypt(aesKey, ByteView { plain.data(), 15 }, ByteSpan { encrypted.data(), encrypted.size() }));

  // RFC 8032 section 7.1 test 1: seed, public key and signature over an empty
  // message. This pins the whole seed -> keypair -> signature path at once.
  core::KeyPair rfc = keypairFromSeed("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  that("Ed25519 public key from seed",
    equalsHex(rfc.pk.view(), "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
  that("Ed25519 signature over an empty message",
    equalsHex(core::sign(rfc.sk, ByteView {}).view(),
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
      "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"));
  that("verify accepts it", core::verify(rfc.pk, ByteView {}, core::sign(rfc.sk, ByteView {})));

  core::Signature broken = core::sign(rfc.sk, ByteView {});
  broken.data[0] ^= 1;
  that("verify rejects a tampered signature", !core::verify(rfc.pk, ByteView {}, broken));
}

static void testRandom()
{
  section("core: randomBytes");

  core::init(nullptr);

  // Exact-size heap buffers: an overrun would trip the sanitiser.
  for (size_t n : { size_t(1), size_t(31), size_t(32), size_t(MAX_PACKET_PAYLOAD) }) {
    std::vector<uint8_t> buffer(n);
    core::randomBytes(ByteSpan { buffer.data(), n });
  }
  that("writes within bounds", true);

  std::vector<uint8_t> marked(64, 0xAA);
  core::randomBytes(ByteSpan { marked.data(), marked.size() });
  size_t untouched = 0;
  for (uint8_t b : marked)
    if (b == 0xAA) untouched++;
  that("fills the whole buffer", untouched < 8);

  std::vector<uint8_t> first(32), second(32);
  core::randomBytes(ByteSpan { first.data(), first.size() });
  core::randomBytes(ByteSpan { second.data(), second.size() });
  that("two calls differ", first != second);

  core::randomBytes(ByteSpan {});
  that("empty span is a no-op", true);

  // The swappable source is what makes key generation reproducible.
  scriptedBytes = fromHex("0102030405060708");
  core::init(scriptedRandom);
  std::vector<uint8_t> scripted(4);
  core::randomBytes(ByteSpan { scripted.data(), scripted.size() });
  that("source can be replaced", equalsHex(view(scripted), "01020304"));

  core::init(nullptr);
  std::vector<uint8_t> back(32), backAgain(32);
  core::randomBytes(ByteSpan { back.data(), back.size() });
  core::randomBytes(ByteSpan { backAgain.data(), backAgain.size() });
  that("init(nullptr) restores the system source", back != backAgain);
}

static void testConstantTimeEqual()
{
  section("core: constantTimeEqual");

  std::vector<uint8_t> a { 1, 2, 3 }, same { 1, 2, 3 }, other { 1, 2, 4 }, shorter { 1, 2 };
  that("equal buffers", core::constantTimeEqual(view(a), view(same)));
  that("differing byte", !core::constantTimeEqual(view(a), view(other)));
  that("differing length", !core::constantTimeEqual(view(a), view(shorter)));
  that("both empty", core::constantTimeEqual(ByteView {}, ByteView {}));
}

static void testKeypairStorage(const std::string& dir)
{
  section("core: keypair storage");

  core::init(nullptr);
  core::KeyPair keypair = core::generateKeypair();
  core::KeyPair other = core::generateKeypair();

  that("two keypairs differ", std::memcmp(keypair.pk.data.data(), other.pk.data.data(), PACKET_PUBLIC_KEY_SIZE) != 0);
  that("the private key ends with the public one",
    std::memcmp(keypair.sk.view().data() + PACKET_PUBLIC_KEY_SIZE, keypair.pk.data.data(), PACKET_PUBLIC_KEY_SIZE)
      == 0);

  const std::string path = dir + "/node/identity.key";
  that("saved", core::saveKeypair(keypair, path));

  struct stat info {};
  stat(path.c_str(), &info);
  that("file mode is 0600", (info.st_mode & 07777) == 0600);
  that("holds the 64-byte private key", info.st_size == PACKET_PRIVATE_KEY_SIZE);

  struct stat dirInfo {};
  stat((dir + "/node").c_str(), &dirInfo);
  that("directory mode is 0700", (dirInfo.st_mode & 07777) == 0700);

  that("an existing key is never overwritten", !core::saveKeypair(other, path));

  core::KeyPair loaded {};
  that("loaded", core::loadKeypair(loaded, path));
  that("round-trip",
    std::memcmp(loaded.pk.data.data(), keypair.pk.data.data(), PACKET_PUBLIC_KEY_SIZE) == 0
      && std::memcmp(loaded.sk.view().data(), keypair.sk.view().data(), PACKET_PRIVATE_KEY_SIZE) == 0);

  const std::string open644 = dir + "/open.key";
  int fd = ::open(open644.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ssize_t ignored = write(fd, keypair.sk.view().data(), PACKET_PRIVATE_KEY_SIZE);
  (void)ignored;
  close(fd);
  core::KeyPair rejected {};
  that("a group-readable key is refused", !core::loadKeypair(rejected, open644));

  const std::string shortFile = dir + "/short.key";
  fd = ::open(shortFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  ignored = write(fd, keypair.sk.view().data(), PACKET_PRIVATE_KEY_SIZE - 1);
  (void)ignored;
  close(fd);
  that("a short file is refused", !core::loadKeypair(rejected, shortFile));

  // Corruption in either half must be caught by re-deriving from the seed.
  for (size_t offset : { size_t(5), size_t(40) }) {
    const std::string corrupt = dir + "/corrupt.key";
    std::vector<uint8_t> bytes(keypair.sk.view().begin(), keypair.sk.view().end());
    bytes[offset] ^= 0xFF;
    fd = ::open(corrupt.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ignored = write(fd, bytes.data(), bytes.size());
    (void)ignored;
    close(fd);

    char name[64];
    std::snprintf(name, sizeof name, "corrupt byte %zu is refused", offset);
    that(name, !core::loadKeypair(rejected, corrupt));
    unlink(corrupt.c_str());
  }

  that("a missing file is refused", !core::loadKeypair(rejected, dir + "/nothing.key"));
  that("a directory is refused", !core::loadKeypair(rejected, dir));
}

static void testDeriveShared()
{
  section("core: deriveShared");

  core::init(nullptr);
  core::KeyPair alice = core::generateKeypair();
  core::KeyPair bob = core::generateKeypair();
  core::KeyPair eve = core::generateKeypair();

  auto ab = core::deriveShared(alice.sk, bob.pk);
  auto ba = core::deriveShared(bob.sk, alice.pk);
  that("both sides get a secret", ab && ba);
  that("symmetric", ab && ba && std::memcmp(ab->view().data(), ba->view().data(), PACKET_SHARED_SECRET_SIZE) == 0);

  auto again = core::deriveShared(alice.sk, bob.pk);
  that("deterministic", again && std::memcmp(again->view().data(), ab->view().data(), PACKET_SHARED_SECRET_SIZE) == 0);

  auto ae = core::deriveShared(alice.sk, eve.pk);
  that("a different peer gives a different secret",
    ae && std::memcmp(ae->view().data(), ab->view().data(), PACKET_SHARED_SECRET_SIZE) != 0);

  core::PublicKey zero {};
  that("an all-zero key is refused", !core::deriveShared(alice.sk, zero));

  core::PublicKey ones;
  ones.data.fill(0xFF);
  that("an all-ones key is refused", !core::deriveShared(alice.sk, ones));

  // An Ed25519 key is a curve point; a corrupted one almost never is.
  int refused = 0;
  for (size_t i = 0; i < PACKET_PUBLIC_KEY_SIZE; i++) {
    core::PublicKey damaged = bob.pk;
    damaged.data[i] ^= 0xFF;
    auto secret = core::deriveShared(alice.sk, damaged);
    if (!secret || std::memcmp(secret->view().data(), ab->view().data(), PACKET_SHARED_SECRET_SIZE) != 0) {
      refused++;
    }
  }
  that("no corrupted key reproduces the secret", refused == PACKET_PUBLIC_KEY_SIZE);
}

// ------------------------------------------------------------------ protocol

static void testAdvertSigning()
{
  section("protocol: advert signing");

  core::init(nullptr);
  core::KeyPair node = core::generateKeypair();
  core::KeyPair stranger = core::generateKeypair();

  size_t signatureAt = 0;
  std::vector<uint8_t> appdata { 0x01, 'n', 'o', 'd', 'e' };
  std::vector<uint8_t> frame = advertFrame(node.pk, 0xAABBCCDD, appdata, signatureAt);

  core::Signature signature = protocol::packetSign(node.sk, view(frame));
  std::memcpy(frame.data() + signatureAt, signature.data.data(), PACKET_SIGNATURE_SIZE);
  that("own advert verifies", protocol::packetVerify(view(frame)));

  struct Tamper {
    const char* name;
    size_t offset;
  };
  const Tamper tampers[] = {
    { "tampered public key", 2 },
    { "tampered timestamp", 2 + PACKET_PUBLIC_KEY_SIZE },
    { "tampered signature", signatureAt },
    { "tampered appdata", signatureAt + PACKET_SIGNATURE_SIZE },
  };
  for (const Tamper& t : tampers) {
    std::vector<uint8_t> damaged = frame;
    damaged[t.offset] ^= 0x01;
    that(t.name, !protocol::packetVerify(view(damaged)));
  }

  std::vector<uint8_t> truncated(frame.begin(), frame.end() - 1);
  that("truncated appdata", !protocol::packetVerify(view(truncated)));

  // Same advert, signed by somebody else.
  std::vector<uint8_t> forged = frame;
  core::Signature wrong = protocol::packetSign(stranger.sk, view(frame));
  std::memcpy(forged.data() + signatureAt, wrong.data.data(), PACKET_SIGNATURE_SIZE);
  that("a foreign signature is rejected", !protocol::packetVerify(view(forged)));

  // Appdata is optional, and transport codes shift every offset.
  std::vector<uint8_t> none;
  size_t bareAt = 0;
  std::vector<uint8_t> bare = advertFrame(node.pk, 7, none, bareAt);
  core::Signature bareSig = protocol::packetSign(node.sk, view(bare));
  std::memcpy(bare.data() + bareAt, bareSig.data.data(), PACKET_SIGNATURE_SIZE);
  that("advert with no appdata", protocol::packetVerify(view(bare)));

  size_t transportAt = 0;
  std::vector<uint8_t> transport = advertFrame(node.pk, 9, appdata, transportAt, 0x00);
  core::Signature transportSig = protocol::packetSign(node.sk, view(transport));
  std::memcpy(transport.data() + transportAt, transportSig.data.data(), PACKET_SIGNATURE_SIZE);
  that("advert behind transport codes", protocol::packetVerify(view(transport)));

  std::vector<uint8_t> notAdvert { 0x01, 0x00, 1, 2, 3 };
  that("a non-advert frame is rejected", !protocol::packetVerify(view(notAdvert)));
  that("garbage is rejected", !protocol::packetVerify(ByteView {}));
}

static void testPacketHash()
{
  section("protocol: packetHash");

  std::vector<uint8_t> frame { 0x11, 0x00, 1, 2, 3 };
  core::Hash hash = protocol::packetHash(view(frame));

  std::vector<uint8_t> same = frame;
  that("stable", std::memcmp(protocol::packetHash(view(same)).data.data(), hash.data.data(), PACKET_HASH_SIZE) == 0);

  std::vector<uint8_t> otherHeader { 0x15, 0x00, 1, 2, 3 };
  that("depends on the header",
    std::memcmp(protocol::packetHash(view(otherHeader)).data.data(), hash.data.data(), PACKET_HASH_SIZE) != 0);

  std::vector<uint8_t> otherPayload { 0x11, 0x00, 1, 2, 4 };
  that("depends on the payload",
    std::memcmp(protocol::packetHash(view(otherPayload)).data.data(), hash.data.data(), PACKET_HASH_SIZE) != 0);

  // Deduplication only works if the growing path stays out of the hash.
  std::vector<uint8_t> withHop { 0x11, 0x01, 0x42, 1, 2, 3 };
  that("ignores the path",
    std::memcmp(protocol::packetHash(view(withHop)).data.data(), hash.data.data(), PACKET_HASH_SIZE) == 0);
}

static void testSealOpen()
{
  section("protocol: seal / open");

  core::init(nullptr);
  core::KeyPair alice = core::generateKeypair();
  core::KeyPair bob = core::generateKeypair();
  const core::SharedSecret secret = *core::deriveShared(alice.sk, bob.pk);
  const core::SharedSecret elsewhere = *core::deriveShared(alice.sk, alice.pk);

  for (size_t length : { size_t(0), size_t(1), size_t(15), size_t(16), size_t(17), size_t(100) }) {
    std::vector<uint8_t> plain(length);
    for (size_t i = 0; i < length; i++)
      plain[i] = (uint8_t)(i * 7 + 1);

    std::vector<uint8_t> cipher(length + PACKET_CIPHER_BLOCK_SIZE * 2);
    std::vector<uint8_t> out(cipher.size());

    auto sealed = protocol::seal(secret, view(plain), ByteSpan { cipher.data(), cipher.size() });
    char name[80];
    std::snprintf(name, sizeof name, "length %zu: padded up to a whole block", length);
    that(name, sealed && sealed->ciphertextLength % PACKET_CIPHER_BLOCK_SIZE == 0 && sealed->ciphertextLength > length);

    auto opened = protocol::open(
      secret, sealed->mac, ByteView { cipher.data(), sealed->ciphertextLength }, ByteSpan { out.data(), out.size() });
    std::snprintf(name, sizeof name, "length %zu: opens back to the plaintext", length);
    that(name, opened && *opened == length && (length == 0 || std::memcmp(out.data(), plain.data(), length) == 0));
  }

  std::vector<uint8_t> plain { 1, 2, 3 };
  std::vector<uint8_t> cipher(64), out(64);
  auto sealed = *protocol::seal(secret, view(plain), ByteSpan { cipher.data(), cipher.size() });
  const size_t length = sealed.ciphertextLength;
  ByteSpan sink { out.data(), out.size() };

  protocol::Mac damagedMac = sealed.mac;
  damagedMac.data[0] ^= 1;
  that("tampered MAC", !protocol::open(secret, damagedMac, ByteView { cipher.data(), length }, sink));

  std::vector<uint8_t> damagedCipher = cipher;
  damagedCipher[0] ^= 1;
  that("tampered ciphertext", !protocol::open(secret, sealed.mac, ByteView { damagedCipher.data(), length }, sink));

  that("wrong secret", !protocol::open(elsewhere, sealed.mac, ByteView { cipher.data(), length }, sink));
  that("length not a whole block", !protocol::open(secret, sealed.mac, ByteView { cipher.data(), length - 1 }, sink));
  that("empty ciphertext", !protocol::open(secret, sealed.mac, ByteView {}, sink));

  std::vector<uint8_t> tiny(4);
  that("out too small for open",
    !protocol::open(secret, sealed.mac, ByteView { cipher.data(), length }, ByteSpan { tiny.data(), tiny.size() }));
  std::vector<uint8_t> tooSmall(2);
  that("out too small for seal", !protocol::seal(secret, view(plain), ByteSpan { tooSmall.data(), tooSmall.size() }));

  // ECB has no IV, so identical blocks encrypt identically. This is a
  // protocol property, not a bug: pinned so a change is deliberate.
  std::vector<uint8_t> repeated(PACKET_CIPHER_BLOCK_SIZE * 2, 0xAB);
  std::vector<uint8_t> repeatedCipher(64);
  protocol::seal(secret, view(repeated), ByteSpan { repeatedCipher.data(), repeatedCipher.size() });
  that("identical blocks encrypt identically",
    std::memcmp(repeatedCipher.data(), repeatedCipher.data() + PACKET_CIPHER_BLOCK_SIZE, PACKET_CIPHER_BLOCK_SIZE)
      == 0);

  // cipherKeyFrom is the head of the secret, no KDF.
  core::CipherKey key = protocol::cipherKeyFrom(secret);
  that("cipher key is the head of the secret",
    std::memcmp(key.view().data(), secret.view().data(), PACKET_CIPHER_KEY_SIZE) == 0);
}

static void testExpectedAck()
{
  section("protocol: expectedAck");

  core::init(nullptr);
  core::KeyPair alice = core::generateKeypair();
  core::KeyPair bob = core::generateKeypair();
  const core::SharedSecret secret = *core::deriveShared(alice.sk, bob.pk);

  std::vector<uint8_t> payload { 9, 8, 7, 6 };
  const uint32_t ack = protocol::expectedAck(view(payload), bob.pk);
  that("deterministic", protocol::expectedAck(view(payload), bob.pk) == ack);
  that("depends on the recipient", protocol::expectedAck(view(payload), alice.pk) != ack);

  std::vector<uint8_t> otherPayload { 9, 8, 7, 5 };
  that("depends on the payload", protocol::expectedAck(view(otherPayload), bob.pk) != ack);
  that("empty payload does not crash", protocol::expectedAck(ByteView {}, bob.pk) != 0);

  // The trap: the ACK must be computed over what went on air, the ciphertext.
  // Feeding it the plaintext looks fine until every message reads as undelivered.
  std::vector<uint8_t> plain { 'h', 'e', 'l', 'l', 'o' };
  std::vector<uint8_t> cipher(64), out(64);
  auto sealed = *protocol::seal(secret, view(plain), ByteSpan { cipher.data(), cipher.size() });
  const size_t length = sealed.ciphertextLength;

  const uint32_t sent = protocol::expectedAck(ByteView { cipher.data(), length }, bob.pk);
  auto opened =
    protocol::open(secret, sealed.mac, ByteView { cipher.data(), length }, ByteSpan { out.data(), out.size() });
  const uint32_t replied = protocol::expectedAck(ByteView { cipher.data(), length }, bob.pk);
  that("both sides agree over the ciphertext", opened && sent == replied);
  that(
    "the plaintext gives a different value", protocol::expectedAck(ByteView { out.data(), *opened }, bob.pk) != sent);
}

// ------------------------------------------------------------------ main

int main()
{
  if (!core::init(nullptr)) {
    std::printf("backend init failed\n");
    return 1;
  }

  char templ[] = "meshcore_test_XXXXXX";
  const char* dir = mkdtemp(templ);
  if (dir == nullptr) {
    std::printf("cannot create a temporary directory\n");
    return 1;
  }
  const std::string root = dir;

  testVectors();
  testRandom();
  testConstantTimeEqual();
  testKeypairStorage(root);
  testDeriveShared();
  testAdvertSigning();
  testPacketHash();
  testSealOpen();
  testExpectedAck();

  for (const char* name : { "node/identity.key", "open.key", "short.key" }) {
    unlink((root + "/" + name).c_str());
  }
  rmdir((root + "/node").c_str());
  rmdir(root.c_str());

  return check::report();
}
