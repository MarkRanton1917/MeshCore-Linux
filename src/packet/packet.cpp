// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#include "packet.h"
#include "defines.h"

#include <cstring>

static constexpr size_t kFixedPrefixSize = PACKET_HEADER_SIZE + PACKET_PATH_LENGTH_SIZE;

size_t packet::Advert::signedMessage(ByteSpan out) const
{
  if (publicKey.size() != PACKET_PUBLIC_KEY_SIZE) return 0;
  if (out.size() < PACKET_PUBLIC_KEY_SIZE + PACKET_TIMESTAMP_SIZE + appdata.size()) return 0;

  memcpy(out.data(), publicKey.data(), publicKey.size());

  // Little-endian by hand: memcpy of the field would depend on host layout.
  size_t offset = PACKET_PUBLIC_KEY_SIZE;
  out[offset++] = (uint8_t)(timestamp);
  out[offset++] = (uint8_t)(timestamp >> 8);
  out[offset++] = (uint8_t)(timestamp >> 16);
  out[offset++] = (uint8_t)(timestamp >> 24);

  if (!appdata.empty()) {
    memcpy(out.data() + offset, appdata.data(), appdata.size());
    offset += appdata.size();
  }
  return offset;
}

std::optional<packet::Packet> packet::parse(ByteView frame)
{
  if (frame.size() < kFixedPrefixSize || frame.size() > MAX_PACKET_FRAME) return std::nullopt;

  Packet p;
  p.header = frame[0];
  size_t offset = PACKET_HEADER_SIZE;

  if (p.hasTransportCodes()) {
    if (frame.size() < offset + PACKET_TRANSPORT_CODES_SIZE) return std::nullopt;
    p.transportCode1 = (uint16_t)(frame[offset] | (frame[offset + 1] << 8));
    p.transportCode2 = (uint16_t)(frame[offset + 2] | (frame[offset + 3] << 8));
    offset += PACKET_TRANSPORT_CODES_SIZE;
  }

  if (frame.size() < offset + PACKET_PATH_LENGTH_SIZE) return std::nullopt;
  const uint8_t pathLength = frame[offset++];
  p.hopCount = pathLength & 0x3F;

  const uint8_t hashCode = (pathLength & 0xC0) >> 6;
  if (hashCode + 1 > MAX_HASH_SIZE) return std::nullopt; // size code not defined by the spec
  p.hashSize = hashCode + 1;

  // The declared path must fit both its own ceiling and what actually arrived.
  const size_t pathSize = p.pathSize();
  if (pathSize > MAX_PATH_SIZE) return std::nullopt;
  if (frame.size() - offset < pathSize) return std::nullopt;
  if (pathSize > 0) memcpy(p.path.data(), frame.data() + offset, pathSize);
  offset += pathSize;

  const size_t payloadSize = frame.size() - offset;
  if (payloadSize > MAX_PACKET_PAYLOAD) return std::nullopt;
  if (payloadSize > 0) memcpy(p.payload.data(), frame.data() + offset, payloadSize);
  p.payloadSize = (uint8_t)payloadSize;

  return p;
}

std::optional<size_t> packet::serialize(const Packet& p, ByteSpan out)
{
  if (p.hopCount > MAX_HOP_COUNT) return std::nullopt;
  if (p.hashSize < 1 || p.hashSize > MAX_HASH_SIZE) return std::nullopt;
  if (p.payloadSize > MAX_PACKET_PAYLOAD) return std::nullopt;

  const size_t pathSize = p.pathSize();
  if (pathSize > MAX_PATH_SIZE) return std::nullopt;

  size_t total = kFixedPrefixSize + pathSize + p.payloadSize;
  if (p.hasTransportCodes()) total += PACKET_TRANSPORT_CODES_SIZE;
  if (total > MAX_PACKET_FRAME || out.size() < total) return std::nullopt;

  size_t offset = 0;
  out[offset++] = p.header;

  if (p.hasTransportCodes()) {
    out[offset++] = (uint8_t)(p.transportCode1);
    out[offset++] = (uint8_t)(p.transportCode1 >> 8);
    out[offset++] = (uint8_t)(p.transportCode2);
    out[offset++] = (uint8_t)(p.transportCode2 >> 8);
  }

  out[offset++] = (uint8_t)(((p.hashSize - 1) << 6) | p.hopCount);

  if (pathSize > 0) memcpy(out.data() + offset, p.path.data(), pathSize);
  offset += pathSize;

  if (p.payloadSize > 0) memcpy(out.data() + offset, p.payload.data(), p.payloadSize);
  offset += p.payloadSize;

  return offset;
}

bool packet::appendSelf(Packet& p, uint8_t selfHash)
{
  if (p.hashSize != NODE_HASH_SIZE) return false; // a one-byte hash cannot fill a wider slot
  if (p.hopCount >= MAX_HOP_COUNT) return false;

  const size_t pathSize = p.pathSize();
  if (pathSize + p.hashSize > MAX_PATH_SIZE) return false;

  p.path[pathSize] = selfHash;
  p.hopCount++;
  return true;
}

bool packet::stripSelf(Packet& p, uint8_t selfHash)
{
  if (p.hashSize != NODE_HASH_SIZE) return false;
  if (p.hopCount == 0) return false;
  if (p.path[0] != selfHash) return false; // not routed through us

  const size_t pathSize = p.pathSize();
  memmove(p.path.data(), p.path.data() + p.hashSize, pathSize - p.hashSize);
  p.path[pathSize - p.hashSize] = 0;
  p.hopCount--;
  return true;
}

// Little-endian, like everything else on air.
static uint32_t readUint32(ByteView view, size_t offset)
{
  return (uint32_t)view[offset] | (uint32_t)view[offset + 1] << 8 | (uint32_t)view[offset + 2] << 16
    | (uint32_t)view[offset + 3] << 24;
}

static void writeUint32(ByteSpan out, size_t offset, uint32_t value)
{
  out[offset] = (uint8_t)(value);
  out[offset + 1] = (uint8_t)(value >> 8);
  out[offset + 2] = (uint8_t)(value >> 16);
  out[offset + 3] = (uint8_t)(value >> 24);
}

std::optional<packet::Advert> packet::decodeAdvert(ByteView payload)
{
  if (payload.size() < ADVERT_PAYLOAD_SIZE) return std::nullopt;

  Advert advert;
  advert.publicKey = payload.subspan(0, PACKET_PUBLIC_KEY_SIZE);
  advert.timestamp = readUint32(payload, PACKET_PUBLIC_KEY_SIZE);
  advert.signature = payload.subspan(PACKET_PUBLIC_KEY_SIZE + PACKET_TIMESTAMP_SIZE, PACKET_SIGNATURE_SIZE);
  advert.appdata = payload.subspan(ADVERT_PAYLOAD_SIZE); // optional, may be empty
  return advert;
}

std::optional<size_t> packet::encodeAdvert(const Advert& a, ByteSpan out)
{
  if (a.publicKey.size() != PACKET_PUBLIC_KEY_SIZE) return std::nullopt;
  if (a.signature.size() != PACKET_SIGNATURE_SIZE) return std::nullopt;

  const size_t total = ADVERT_PAYLOAD_SIZE + a.appdata.size();
  if (total > MAX_PACKET_PAYLOAD || out.size() < total) return std::nullopt;

  memcpy(out.data(), a.publicKey.data(), PACKET_PUBLIC_KEY_SIZE);
  size_t offset = PACKET_PUBLIC_KEY_SIZE;

  writeUint32(out, offset, a.timestamp);
  offset += PACKET_TIMESTAMP_SIZE;

  memcpy(out.data() + offset, a.signature.data(), PACKET_SIGNATURE_SIZE);
  offset += PACKET_SIGNATURE_SIZE;

  if (!a.appdata.empty()) {
    memcpy(out.data() + offset, a.appdata.data(), a.appdata.size());
    offset += a.appdata.size();
  }
  return offset;
}

std::optional<packet::TextMsg> packet::decodeText(ByteView plain)
{
  if (plain.size() < TEXT_MSG_PREFIX_SIZE) return std::nullopt;

  TextMsg message;
  message.timestamp = readUint32(plain, 0);

  const uint8_t flags = plain[PACKET_TIMESTAMP_SIZE];
  message.txtType = (flags & 0xFC) >> 2;
  message.attempt = flags & 0x03;
  message.message = plain.subspan(TEXT_MSG_PREFIX_SIZE);
  return message;
}

std::optional<size_t> packet::encodeText(const TextMsg& m, ByteSpan out)
{
  if (m.txtType > 0x3F || m.attempt > 0x03) return std::nullopt;

  const size_t total = TEXT_MSG_PREFIX_SIZE + m.message.size();
  if (total > MAX_PACKET_PAYLOAD || out.size() < total) return std::nullopt;

  writeUint32(out, 0, m.timestamp);
  out[PACKET_TIMESTAMP_SIZE] = (uint8_t)((m.txtType << 2) | m.attempt);

  if (!m.message.empty()) {
    memcpy(out.data() + TEXT_MSG_PREFIX_SIZE, m.message.data(), m.message.size());
  }
  return total;
}

std::optional<packet::AnonReq> packet::decodeAnonReq(ByteView payload)
{
  if (payload.size() < ANON_REQ_PREFIX_SIZE) return std::nullopt;

  AnonReq request;
  request.destinationHash = payload[0];
  request.publicKey = payload.subspan(NODE_HASH_SIZE, PACKET_PUBLIC_KEY_SIZE);
  request.cipherMac = payload.subspan(NODE_HASH_SIZE + PACKET_PUBLIC_KEY_SIZE, PACKET_MAC_SIZE);
  request.ciphertext = payload.subspan(ANON_REQ_PREFIX_SIZE);
  return request;
}

std::optional<size_t> packet::encodeLoginResponse(const LoginResponse& r, ByteSpan out)
{
  const size_t total = LOGIN_RESPONSE_NONCE_SIZE + r.body.size();
  if (total > MAX_PACKET_PAYLOAD || out.size() < total) return std::nullopt;

  memcpy(out.data(), r.nonce.data(), LOGIN_RESPONSE_NONCE_SIZE);
  if (!r.body.empty()) {
    memcpy(out.data() + LOGIN_RESPONSE_NONCE_SIZE, r.body.data(), r.body.size());
  }
  return total;
}

std::optional<packet::Envelope> packet::decodeEnvelope(ByteView payload)
{
  if (payload.size() < ENVELOPE_PREFIX_SIZE) return std::nullopt;

  Envelope envelope;
  envelope.destinationHash = payload[0];
  envelope.sourceHash = payload[1];
  envelope.cipherMac = payload.subspan(NODE_HASH_SIZE * 2, PACKET_MAC_SIZE);
  envelope.ciphertext = payload.subspan(ENVELOPE_PREFIX_SIZE);
  return envelope;
}

std::optional<size_t> packet::encodeEnvelope(const Envelope& e, ByteSpan out)
{
  if (e.cipherMac.size() != PACKET_MAC_SIZE) return std::nullopt;

  const size_t total = ENVELOPE_PREFIX_SIZE + e.ciphertext.size();
  if (total > MAX_PACKET_PAYLOAD || out.size() < total) return std::nullopt;

  out[0] = e.destinationHash;
  out[1] = e.sourceHash;
  memcpy(out.data() + NODE_HASH_SIZE * 2, e.cipherMac.data(), PACKET_MAC_SIZE);
  if (!e.ciphertext.empty()) {
    memcpy(out.data() + ENVELOPE_PREFIX_SIZE, e.ciphertext.data(), e.ciphertext.size());
  }
  return total;
}

std::optional<packet::PathReturn> packet::decodePath(ByteView payload)
{
  if (payload.empty()) return std::nullopt;

  const uint8_t pathSize = payload[0];
  if (pathSize > MAX_PATH_SIZE) return std::nullopt;

  // The path length byte, the path itself, and the bundled payload type.
  const size_t prefix = PACKET_PATH_LENGTH_SIZE + pathSize + PATH_EXTRA_TYPE_SIZE;
  if (payload.size() < prefix) return std::nullopt;

  PathReturn returned;
  returned.path = payload.subspan(PACKET_PATH_LENGTH_SIZE, pathSize);
  returned.extraType = payload[PACKET_PATH_LENGTH_SIZE + pathSize];
  returned.extra = payload.subspan(prefix);
  return returned;
}

std::optional<size_t> packet::encodePath(const PathReturn& p, ByteSpan out)
{
  if (p.path.size() > MAX_PATH_SIZE) return std::nullopt;

  const size_t total = PACKET_PATH_LENGTH_SIZE + p.path.size() + PATH_EXTRA_TYPE_SIZE + p.extra.size();
  if (total > MAX_PACKET_PAYLOAD || out.size() < total) return std::nullopt;

  out[0] = (uint8_t)p.path.size();
  size_t offset = PACKET_PATH_LENGTH_SIZE;

  if (!p.path.empty()) {
    memcpy(out.data() + offset, p.path.data(), p.path.size());
    offset += p.path.size();
  }

  out[offset++] = p.extraType;

  if (!p.extra.empty()) {
    memcpy(out.data() + offset, p.extra.data(), p.extra.size());
    offset += p.extra.size();
  }
  return offset;
}
