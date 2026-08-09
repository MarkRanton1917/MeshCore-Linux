// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#pragma once

#include "defines.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace packet {

enum class RouteType {
  TRANSPORT_FLOOD = 0x00,
  FLOOD = 0x01,
  DIRECT = 0x02,
  TRANSPORT_DIRECT = 0x03
};

enum class PayloadType {
  REQ = 0x00,
  RESPONSE = 0x01,
  TXT_MSG = 0x02,
  ACK = 0x03,
  ADVERT = 0x04,
  GRP_TXT = 0x05,
  GRP_DATA = 0x06,
  ANON_REQ = 0x07,
  PATH = 0x08,
  TRACE = 0x09,
  MULTIPART = 0x0A,
  CONTROL = 0x0B,
  RAW_CUSTOM = 0x0F
};

// A parsed frame. Fixed-size arrays and no heap: every bound is known at compile
// time, so a packet can live on the stack of whoever received it.
struct Packet {
  uint8_t header = 0;

  // Only present for the two transport route types. Little-endian on air.
  uint16_t transportCode1 = 0;
  uint16_t transportCode2 = 0;

  uint8_t hashSize = NODE_HASH_SIZE; // bytes per hop, 1..3
  uint8_t hopCount = 0;
  std::array<uint8_t, MAX_PATH_SIZE> path {};

  std::array<uint8_t, MAX_PACKET_PAYLOAD> payload {};
  uint8_t payloadSize = 0;

  RouteType routeType() const
  {
    return (RouteType)(header & 0x03);
  }
  PayloadType payloadType() const
  {
    return (PayloadType)((header & 0x3C) >> 2);
  }

  bool hasTransportCodes() const
  {
    return routeType() == RouteType::TRANSPORT_FLOOD || routeType() == RouteType::TRANSPORT_DIRECT;
  }

  size_t pathSize() const
  {
    return (size_t)hopCount * hashSize;
  }
  ByteView payloadView() const
  {
    return { payload.data(), payloadSize };
  }
};

// All payload structs below hold views into the buffer they were decoded from.
// That buffer must outlive them.

// ADVERT: publicKey(32) timestamp(4) signature(64) appdata(rest).
// The signature covers publicKey, timestamp and appdata, which are not
// contiguous on air: the signature itself sits between them.
struct Advert {
  ByteView publicKey;
  uint32_t timestamp;
  ByteView signature;
  ByteView appdata;

  // Builds the message the signature covers, which is NOT the wire payload:
  // publicKey || timestamp || appdata, with the signature left out.
  // Returns bytes written, 0 on failure.
  size_t signedMessage(ByteSpan out) const;
};

// TXT_MSG: timestamp(4) txtType+attempt(1) message(rest). Already decrypted.
struct TextMsg {
  uint32_t timestamp = 0;
  uint8_t txtType = 0; // upper six bits of the flags byte
  uint8_t attempt = 0; // lower two bits
  ByteView message;
};

// ANON_REQ: destinationHash(1) publicKey(32) cipherMac(2) ciphertext(rest).
struct AnonReq {
  uint8_t destinationHash = 0;
  ByteView publicKey;
  ByteView cipherMac;
  ByteView ciphertext;
};

// RESPONSE body. The wire format is application-defined, so only the leading
// four random bytes are fixed here: without them two identical replies would
// produce the same packet and be dropped as duplicates.
struct LoginResponse {
  std::array<uint8_t, LOGIN_RESPONSE_NONCE_SIZE> nonce {};
  ByteView body;
};

// Envelope shared by REQ, RESPONSE, TXT_MSG and PATH on air. The plaintext
// inside is whichever payload the type says; decodeText and decodePath run on
// it only after open() has decrypted it.
struct Envelope {
  uint8_t destinationHash = 0;
  uint8_t sourceHash = 0;
  ByteView cipherMac;
  ByteView ciphertext;
};

// PATH: pathLength(1) path(n, one byte per hash) extraType(1) extra(rest).
struct PathReturn {
  ByteView path;
  uint8_t extraType = 0;
  ByteView extra;
};

// Splits a frame into header, optional transport codes, path and payload.
// nullopt on anything malformed: garbage off the air is routine, and a declared
// path length that overruns the buffer is exactly what this has to catch.
std::optional<Packet> parse(ByteView frame);

// Writes the packet back out as a frame. Returns bytes written.
std::optional<size_t> serialize(const Packet& p, ByteSpan out);

// Flood: append our own hash to the path. False when the path is at its limit,
// which means the packet travels no further.
bool appendSelf(Packet& p, uint8_t selfHash);

// Direct: if our hash is first in the path, drop it. False means this packet is
// not routed through us and should be discarded without a word.
bool stripSelf(Packet& p, uint8_t selfHash);

// Payload codecs. The caller picks which one to use from Packet::payloadType();
// none of them re-check it. Decoders return nullopt on a malformed payload,
// encoders return bytes written or nullopt when out is too small.

// Advert — needed by everyone, repeaters included.
std::optional<Advert> decodeAdvert(ByteView payload);
std::optional<size_t> encodeAdvert(const Advert& a, ByteSpan out);

// Text, already decrypted — room server.
std::optional<TextMsg> decodeText(ByteView plain);
std::optional<size_t> encodeText(const TextMsg& m, ByteSpan out);

// Login and its reply — room server.
std::optional<AnonReq> decodeAnonReq(ByteView payload);
std::optional<size_t> encodeLoginResponse(const LoginResponse& r, ByteSpan out);

// Addressed envelope — every direct payload type wears it.
std::optional<Envelope> decodeEnvelope(ByteView payload);
std::optional<size_t> encodeEnvelope(const Envelope& e, ByteSpan out);

// Returned path — room server as the end node.
std::optional<PathReturn> decodePath(ByteView payload);
std::optional<size_t> encodePath(const PathReturn& p, ByteSpan out);

} // namespace packet
