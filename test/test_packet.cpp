// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

// Tests for src/packet.
//
// There are no published vectors for the frame layout, so these check
// properties instead: every decoder must reject a truncated or oversized input,
// and every encode/decode pair must round-trip byte for byte.

#include "check.h"
#include "packet.h"

#include <cstring>
#include <vector>

using check::section;
using check::that;

static ByteView view(const std::vector<uint8_t>& v)
{
  return ByteView { v.data(), v.size() };
}

static std::optional<packet::Packet> parse(std::vector<uint8_t> frame)
{
  return packet::parse(view(frame));
}

static void testParseRejects()
{
  section("parse: rejects");

  that("empty frame", !parse({}));
  that("header only", !parse({ 0x01 }));
  that("transport route with no path_length byte", !parse({ 0x00, 1, 2, 3, 4 }));
  that("path declared longer than the frame", !parse({ 0x01, 0x05, 0xAA }));
  that("hash size code 0b11 is undefined", !parse({ 0x01, 0xC1, 7, 7, 7 }));

  std::vector<uint8_t> oversized(MAX_PACKET_FRAME + 1, 0);
  oversized[0] = 0x01;
  that("frame longer than 255", !parse(oversized));

  // 63 hops of 3 bytes is 189, past the 64-byte path ceiling.
  std::vector<uint8_t> longPath { 0x01, (uint8_t)((0x02 << 6) | 63) };
  longPath.resize(2 + 189, 0xAA);
  that("path over 64 bytes", !parse(longPath));

  std::vector<uint8_t> bigPayload { 0x01, 0x00 };
  bigPayload.resize(2 + MAX_PACKET_PAYLOAD + 1, 7);
  that("payload over 184 bytes", !parse(bigPayload));
}

static void testParseAccepts()
{
  section("parse: accepts");

  {
    auto p = parse({ 0x01, 0x00, 1, 2, 3 });
    that("flood, no hops", p && p->hopCount == 0 && !p->hasTransportCodes());
    that("payload is the tail", p && p->payloadSize == 3 && p->payload[2] == 3);
    that("route type decoded", p && p->routeType() == packet::RouteType::FLOOD);
  }
  {
    auto p = parse({ 0x00, 0x34, 0x12, 0x78, 0x56, 0x02, 9, 9, 7 });
    that("transport codes are little-endian", p && p->transportCode1 == 0x1234 && p->transportCode2 == 0x5678);
    that("path follows the codes", p && p->hopCount == 2 && p->pathSize() == 2);
    that("payload follows the path", p && p->payloadSize == 1 && p->payload[0] == 7);
  }
  {
    auto p = parse({ 0x01, (uint8_t)((0x01 << 6) | 2), 1, 2, 3, 4, 0xFF });
    that("two-byte hashes widen the path", p && p->hashSize == 2 && p->pathSize() == 4);
  }
  {
    auto p = parse({ 0x11, 0x00 });
    that("empty payload is legal", p && p->payloadSize == 0);
    that("payload type decoded", p && p->payloadType() == packet::PayloadType::ADVERT);
  }
}

static void testSerialize()
{
  section("serialize: round-trip");

  const std::vector<std::vector<uint8_t>> frames = {
    { 0x01, 0x00, 1, 2, 3 }, // flood, no hops
    { 0x01, 0x00 }, // empty payload
    { 0x00, 0x34, 0x12, 0x78, 0x56, 0x02, 9, 9, 7 }, // transport codes
    { 0x03, 1, 2, 3, 4, (uint8_t)((0x01 << 6) | 2), 1, 2, 3, 4, 0xAA }, // two-byte hashes
    { 0x11, 0x05, 1, 2, 3, 4, 5, 0xDE }, // advert with a path
  };

  for (const auto& frame : frames) {
    auto p = packet::parse(view(frame));
    std::vector<uint8_t> out(MAX_PACKET_FRAME);
    auto written = p ? packet::serialize(*p, ByteSpan { out.data(), out.size() }) : std::nullopt;

    char name[64];
    std::snprintf(name, sizeof name, "%zu-byte frame survives a round-trip", frame.size());
    that(name, p && written && *written == frame.size() && std::memcmp(out.data(), frame.data(), frame.size()) == 0);
  }

  auto p = parse({ 0x01, 0x00, 1, 2, 3 });
  std::vector<uint8_t> tiny(3);
  that("out too small", p && !packet::serialize(*p, ByteSpan { tiny.data(), tiny.size() }));
}

static void testAppendSelf()
{
  section("appendSelf");

  {
    auto p = *parse({ 0x01, 0x00, 0xEE });
    that("first hop appended", packet::appendSelf(p, 0x42) && p.hopCount == 1 && p.path[0] == 0x42);
    that("second hop appended", packet::appendSelf(p, 0x43) && p.hopCount == 2 && p.path[1] == 0x43);

    std::vector<uint8_t> out(MAX_PACKET_FRAME);
    auto written = packet::serialize(p, ByteSpan { out.data(), out.size() });
    that("path_length byte updated", written && *written == 5 && out[1] == 0x02);

    auto again = packet::parse(ByteView { out.data(), *written });
    that("re-parses with both hops", again && again->hopCount == 2 && again->payloadSize == 1);
  }
  {
    auto p = *parse({ 0x01, 0x00 });
    for (int i = 0; i < MAX_HOP_COUNT; i++)
      packet::appendSelf(p, (uint8_t)i);
    that("63 hops fit", p.hopCount == MAX_HOP_COUNT);
    that("the 64th is refused", !packet::appendSelf(p, 0xFF));
    that("a refusal leaves the packet alone", p.hopCount == MAX_HOP_COUNT);
  }
  {
    // A one-byte hash cannot fill a wider slot.
    auto p = *parse({ 0x01, (uint8_t)((0x01 << 6) | 1), 9, 9 });
    that("refused on a multi-byte path", !packet::appendSelf(p, 0x42));
  }
}

static void testStripSelf()
{
  section("stripSelf");

  {
    auto p = *parse({ 0x02, 0x03, 0x42, 0x43, 0x44, 0xEE });
    that("own hash first is stripped", packet::stripSelf(p, 0x42));
    that("hop count drops", p.hopCount == 2);
    that("path shifts down", p.path[0] == 0x43 && p.path[1] == 0x44);
    that("vacated byte cleared", p.path[2] == 0);
    that("payload untouched", p.payloadSize == 1 && p.payload[0] == 0xEE);

    std::vector<uint8_t> out(MAX_PACKET_FRAME);
    auto written = packet::serialize(p, ByteSpan { out.data(), out.size() });
    that("serialises one hop shorter", written && *written == 5 && out[1] == 0x02);
  }
  {
    auto p = *parse({ 0x02, 0x03, 0x42, 0x43, 0x44, 0xEE });
    that("foreign hash first is refused", !packet::stripSelf(p, 0x43));
    that("a refusal leaves the packet alone", p.hopCount == 3 && p.path[0] == 0x42);
  }
  {
    auto p = *parse({ 0x02, 0x00, 0xEE });
    that("empty path is refused", !packet::stripSelf(p, 0x42));
  }
}

static void testAdvertCodec()
{
  section("codec: advert");

  std::vector<uint8_t> out(MAX_PACKET_PAYLOAD);
  ByteSpan sink { out.data(), out.size() };

  std::vector<uint8_t> pk(PACKET_PUBLIC_KEY_SIZE, 0xA1);
  std::vector<uint8_t> sig(PACKET_SIGNATURE_SIZE, 0xB2);
  std::vector<uint8_t> appdata { 0x01, 'n', 'o', 'd', 'e' };

  packet::Advert advert;
  advert.publicKey = view(pk);
  advert.timestamp = 0xAABBCCDD;
  advert.signature = view(sig);
  advert.appdata = view(appdata);

  auto written = packet::encodeAdvert(advert, sink);
  that("encodes to 100 + appdata", written && *written == ADVERT_PAYLOAD_SIZE + appdata.size());

  auto decoded = packet::decodeAdvert(ByteView { out.data(), *written });
  that("round-trip",
    decoded && decoded->timestamp == advert.timestamp
      && std::memcmp(decoded->publicKey.data(), pk.data(), pk.size()) == 0
      && std::memcmp(decoded->signature.data(), sig.data(), sig.size()) == 0
      && decoded->appdata.size() == appdata.size());

  std::vector<uint8_t> none;
  advert.appdata = view(none);
  auto bare = packet::encodeAdvert(advert, sink);
  auto bareDecoded = packet::decodeAdvert(ByteView { out.data(), *bare });
  that("appdata is optional", bare && *bare == ADVERT_PAYLOAD_SIZE && bareDecoded && bareDecoded->appdata.empty());

  that("99 bytes is too short", !packet::decodeAdvert(ByteView { out.data(), 99 }));

  std::vector<uint8_t> tiny(50);
  that("out too small", !packet::encodeAdvert(advert, ByteSpan { tiny.data(), tiny.size() }));

  std::vector<uint8_t> shortKey(PACKET_PUBLIC_KEY_SIZE - 1, 0);
  advert.publicKey = view(shortKey);
  that("wrong key length refused", !packet::encodeAdvert(advert, sink));

  // The signed message is the wire payload minus the signature. Confusing the
  // two is silent: signatures would verify nowhere but here.
  advert.publicKey = view(pk);
  advert.appdata = view(appdata);
  std::vector<uint8_t> signable(MAX_PACKET_PAYLOAD);
  const size_t signableSize = advert.signedMessage(ByteSpan { signable.data(), signable.size() });
  that("signedMessage omits exactly the signature", signableSize == *written - PACKET_SIGNATURE_SIZE);
}

static void testTextCodec()
{
  section("codec: text");

  std::vector<uint8_t> out(MAX_PACKET_PAYLOAD);
  ByteSpan sink { out.data(), out.size() };
  std::vector<uint8_t> body { 'h', 'i' };

  packet::TextMsg message;
  message.timestamp = 0x11223344;
  message.txtType = 0x2A;
  message.attempt = 2;
  message.message = view(body);

  auto written = packet::encodeText(message, sink);
  that("encodes to 5 + text", written && *written == TEXT_MSG_PREFIX_SIZE + body.size());
  that("type and attempt share a byte", out[PACKET_TIMESTAMP_SIZE] == (uint8_t)((0x2A << 2) | 2));

  auto decoded = packet::decodeText(ByteView { out.data(), *written });
  that("round-trip",
    decoded && decoded->timestamp == message.timestamp && decoded->txtType == 0x2A && decoded->attempt == 2
      && decoded->message.size() == body.size());

  that("4 bytes is too short", !packet::decodeText(ByteView { out.data(), 4 }));

  auto empty = packet::decodeText(ByteView { out.data(), TEXT_MSG_PREFIX_SIZE });
  that("empty text is legal", empty && empty->message.empty());

  message.txtType = 0x40;
  that("txtType past 6 bits refused", !packet::encodeText(message, sink));
  message.txtType = 0x01;
  message.attempt = 4;
  that("attempt past 2 bits refused", !packet::encodeText(message, sink));
}

static void testAnonReqCodec()
{
  section("codec: anonReq");

  std::vector<uint8_t> payload(ANON_REQ_PREFIX_SIZE + 6);
  payload[0] = 0x42;
  for (int i = 0; i < PACKET_PUBLIC_KEY_SIZE; i++)
    payload[1 + i] = (uint8_t)(i + 1);
  payload[33] = 0xAA;
  payload[34] = 0xBB;
  for (int i = 0; i < 6; i++)
    payload[ANON_REQ_PREFIX_SIZE + i] = (uint8_t)(0xF0 + i);

  auto decoded = packet::decodeAnonReq(view(payload));
  that("fields split at the right offsets",
    decoded && decoded->destinationHash == 0x42 && decoded->publicKey.size() == PACKET_PUBLIC_KEY_SIZE
      && decoded->publicKey[0] == 1 && decoded->cipherMac.size() == PACKET_MAC_SIZE && decoded->cipherMac[0] == 0xAA
      && decoded->ciphertext.size() == 6 && decoded->ciphertext[0] == 0xF0);

  that("one byte short is refused", !packet::decodeAnonReq(ByteView { payload.data(), ANON_REQ_PREFIX_SIZE - 1 }));

  auto bare = packet::decodeAnonReq(ByteView { payload.data(), ANON_REQ_PREFIX_SIZE });
  that("empty ciphertext is legal", bare && bare->ciphertext.empty());
}

static void testLoginResponseCodec()
{
  section("codec: login response");

  std::vector<uint8_t> out(MAX_PACKET_PAYLOAD);
  ByteSpan sink { out.data(), out.size() };
  std::vector<uint8_t> body { 1, 2, 3 };

  packet::LoginResponse response;
  response.nonce = { 0xDE, 0xAD, 0xBE, 0xEF };
  response.body = view(body);

  auto written = packet::encodeLoginResponse(response, sink);
  that("encodes to 4 + body", written && *written == LOGIN_RESPONSE_NONCE_SIZE + body.size());
  that("nonce leads", out[0] == 0xDE && out[3] == 0xEF && out[4] == 1);

  std::vector<uint8_t> none;
  response.body = view(none);
  auto bare = packet::encodeLoginResponse(response, sink);
  that("empty body is just the nonce", bare && *bare == LOGIN_RESPONSE_NONCE_SIZE);

  std::vector<uint8_t> tiny(LOGIN_RESPONSE_NONCE_SIZE - 1);
  that("out too small", !packet::encodeLoginResponse(response, ByteSpan { tiny.data(), tiny.size() }));

  // The nonce exists so two identical replies are not deduplicated away.
  packet::LoginResponse a = response, b = response;
  a.nonce = { 1, 1, 1, 1 };
  b.nonce = { 2, 2, 2, 2 };
  std::vector<uint8_t> first(16), second(16);
  packet::encodeLoginResponse(a, ByteSpan { first.data(), first.size() });
  packet::encodeLoginResponse(b, ByteSpan { second.data(), second.size() });
  that("a different nonce changes the bytes", std::memcmp(first.data(), second.data(), LOGIN_RESPONSE_NONCE_SIZE) != 0);
}

static void testPathCodec()
{
  section("codec: path");

  std::vector<uint8_t> out(MAX_PACKET_PAYLOAD);
  ByteSpan sink { out.data(), out.size() };
  std::vector<uint8_t> hops { 0x11, 0x22, 0x33 };
  std::vector<uint8_t> extra { 0xEE, 0xFF };

  packet::PathReturn returned;
  returned.path = view(hops);
  returned.extraType = 0x02;
  returned.extra = view(extra);

  auto written = packet::encodePath(returned, sink);
  that("encodes to 1 + path + 1 + extra", written && *written == 7);
  that("path length leads", out[0] == 3 && out[4] == 0x02);

  auto decoded = packet::decodePath(ByteView { out.data(), *written });
  that("round-trip",
    decoded && decoded->path.size() == 3 && decoded->path[0] == 0x11 && decoded->extraType == 0x02
      && decoded->extra.size() == 2);

  std::vector<uint8_t> none;
  returned.path = view(none);
  returned.extra = view(none);
  auto bare = packet::encodePath(returned, sink);
  auto bareDecoded = packet::decodePath(ByteView { out.data(), *bare });
  that("empty path and extra",
    bare && *bare == 2 && bareDecoded && bareDecoded->path.empty() && bareDecoded->extra.empty());

  that("empty payload refused", !packet::decodePath(ByteView {}));

  std::vector<uint8_t> truncated { 3, 1, 2 };
  that("path shorter than declared", !packet::decodePath(view(truncated)));

  std::vector<uint8_t> noExtraType { 2, 1, 2 };
  that("missing extraType byte", !packet::decodePath(view(noExtraType)));

  std::vector<uint8_t> tooLong { MAX_PATH_SIZE + 1 };
  tooLong.resize(MAX_PATH_SIZE + 3, 0);
  that("path over 64 refused", !packet::decodePath(view(tooLong)));

  std::vector<uint8_t> full(MAX_PATH_SIZE, 0x77);
  returned.path = view(full);
  returned.extra = view(extra);
  auto atLimit = packet::encodePath(returned, sink);
  that("path of exactly 64 fits", atLimit && *atLimit == 1 + MAX_PATH_SIZE + 1 + extra.size());
}

int main()
{
  testParseRejects();
  testParseAccepts();
  testSerialize();
  testAppendSelf();
  testStripSelf();
  testAdvertCodec();
  testTextCodec();
  testAnonReqCodec();
  testLoginResponseCodec();
  testPathCodec();
  return check::report();
}
