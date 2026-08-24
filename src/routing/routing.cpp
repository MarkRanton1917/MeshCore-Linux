// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#include "routing.h"

#include <algorithm>
#include <cstring>

using namespace routing;

namespace core = crypto::core;
namespace protocol = crypto::protocol;

// Publishing is a no-op when no bus is attached.
static void publish(telemetry::Bus* bus, telemetry::EventType type, Millis at, uint32_t detail = 0)
{
  if (bus != nullptr) bus->publish(type, (uint32_t)(at / 1000), detail);
}

static bool samePublicKey(const PublicKey& a, const PublicKey& b)
{
  return std::memcmp(a.data.data(), b.data.data(), PACKET_PUBLIC_KEY_SIZE) == 0;
}

Router::Router(identity::Store& store, Radio& radio, Delegate& delegate, Config config)
  : store_(store),
    radio_(radio),
    delegate_(delegate),
    config_(config)
{
  // One slot minimum: a cache of nothing would forward every echo forever.
  seen_.resize(config_.seenSlots == 0 ? 1 : config_.seenSlots);
}

// ------------------------------------------------------------------ dedup

// Four bytes of the packet hash are plenty; the full thirty-two would only cost
// memory.
static uint32_t shortHashOf(ByteView frame)
{
  const core::Hash hash = protocol::packetHash(frame);
  return (uint32_t)hash.data[0] | (uint32_t)hash.data[1] << 8 | (uint32_t)hash.data[2] << 16
    | (uint32_t)hash.data[3] << 24;
}

// Stamps are stored one past the clock so that an entry written at time zero is
// still distinguishable from a slot that was never used. The offset cancels out
// wherever an age is computed, and both places are here.
bool Router::alreadySeen(ByteView frame)
{
  const uint32_t key = shortHashOf(frame);
  for (const Seen& entry : seen_) {
    if (entry.at == 0 || entry.shortHash != key) continue;

    // An entry older than the flood it was meant to suppress is no longer
    // evidence of anything: the packet in hand is a new one that happens to
    // hash the same, or the same packet sent again minutes later, and both
    // deserve to travel.
    if (config_.seenTtl != 0 && (now_ + 1) - entry.at >= config_.seenTtl) return false;
    return true;
  }
  return false;
}

void Router::rememberSeen(ByteView frame)
{
  seen_[seenNext_] = Seen { shortHashOf(frame), now_ + 1 };
  seenNext_ = (seenNext_ + 1) % seen_.size();
}

// ------------------------------------------------------------------ queueing

Millis Router::jitter()
{
  if (config_.forwardJitter == 0) return 0;

  uint32_t raw = 0;
  core::randomBytes(ByteSpan { (uint8_t*)&raw, sizeof raw });
  return raw % config_.forwardJitter;
}

void Router::queueFrame(std::vector<uint8_t> frame, Millis notBefore, Priority priority)
{
  queue_.push_back(Delayed { notBefore, priority, std::move(frame) });
}

void Router::handToRadio(ByteView frame, Priority priority)
{
  publish(config_.bus, telemetry::EventType::FrameTx, now_, (uint32_t)frame.size());
  radio_.enqueue(frame, priority);
}

void Router::transmitNow(std::vector<uint8_t> frame, Priority priority)
{
  // Our own packets go into the dedup cache before they leave, or the first
  // neighbour will hand our flood straight back and we will forward it.
  rememberSeen(ByteView { frame.data(), frame.size() });
  handToRadio(ByteView { frame.data(), frame.size() }, priority);
}

// ------------------------------------------------------------------ routes

Router::Route* Router::findRoute(const PublicKey& to)
{
  for (Route& route : routes_) {
    if (samePublicKey(route.to, to)) return &route;
  }
  return nullptr;
}

const Router::Route* Router::findRoute(const PublicKey& to) const
{
  return const_cast<Router*>(this)->findRoute(to);
}

bool Router::hasRoute(const PublicKey& to) const
{
  return findRoute(to) != nullptr;
}

void Router::forgetRoute(const PublicKey& to)
{
  for (size_t i = 0; i < routes_.size(); i++) {
    if (samePublicKey(routes_[i].to, to)) {
      routes_.erase(routes_.begin() + (long)i);
      return;
    }
  }
}

size_t Router::reversePath(ByteView forward, ByteSpan out)
{
  if (forward.size() > out.size()) return 0;

  for (size_t i = 0; i < forward.size(); i++) {
    out[i] = forward[forward.size() - 1 - i];
  }
  return forward.size();
}

// ------------------------------------------------------------------ framing

std::vector<uint8_t> Router::buildFrame(packet::PayloadType type, ByteView payload, bool direct, ByteView path) const
{
  packet::Packet p;
  p.header = (uint8_t)(((uint8_t)type << 2) | (uint8_t)(direct ? packet::RouteType::DIRECT : packet::RouteType::FLOOD));
  p.hashSize = NODE_HASH_SIZE;
  p.hopCount = (uint8_t)path.size();
  if (!path.empty()) std::memcpy(p.path.data(), path.data(), path.size());

  p.payloadSize = (uint8_t)std::min<size_t>(payload.size(), MAX_PACKET_PAYLOAD);
  if (p.payloadSize > 0) std::memcpy(p.payload.data(), payload.data(), p.payloadSize);

  std::vector<uint8_t> frame(MAX_PACKET_FRAME);
  auto written = packet::serialize(p, ByteSpan { frame.data(), frame.size() });
  frame.resize(written ? *written : 0);
  return frame;
}

// ------------------------------------------------------------------ receive

void Router::onFrame(ByteView frame, RxMeta meta)
{
  // 1. Garbage off the air is routine. Drop it without a word.
  auto parsed = packet::parse(frame);
  if (!parsed.has_value()) return;

  publish(config_.bus, telemetry::EventType::FrameRx, now_, (uint32_t)frame.size());

  // 2. Deduplication before anything else.
  if (alreadySeen(frame)) {
    publish(config_.bus, telemetry::EventType::FrameDuplicate, now_);
    return;
  }
  rememberSeen(frame);

  // 3. Our own flood coming back as an echo: it already carries our hash.
  // Direct packets are excluded — our hash being in the path is exactly how
  // they are routed through us.
  if (parsed->routeType() == packet::RouteType::FLOOD || parsed->routeType() == packet::RouteType::TRANSPORT_FLOOD) {
    for (size_t i = 0; i < parsed->pathSize(); i++) {
      if (parsed->path[i] == store_.selfHash()) return;
    }
  }

  const ByteView payload = parsed->payloadView();

  // 4. Addressed to us. Dispatch on the payload type, not on what happens to
  // parse: an ANON_REQ is long enough to look like an envelope, so trying the
  // envelope first would swallow it.
  switch (parsed->payloadType()) {
  case packet::PayloadType::ACK:
    handleAck(payload);
    break;
  case packet::PayloadType::ANON_REQ:
    deliverAnon(*parsed, payload);
    break;
  case packet::PayloadType::ADVERT:
    // Unaddressed and unencrypted. Verifying and remembering it belongs to the
    // layer above; here it only gets forwarded.
    break;
  case packet::PayloadType::TRACE:
    // Unencrypted and addressed to nobody. Handed up as it stands; our own
    // reading is written in on the way out, where the packet is copied.
    if (auto trace = packet::decodeTrace(payload)) {
      delegate_.onTrace(*trace, ByteView { parsed->path.data(), parsed->pathSize() });
    }
    break;
  case packet::PayloadType::GRP_TXT:
  case packet::PayloadType::GRP_DATA:
    // Addressed to a key rather than to a node, so there is no destination hash
    // to match and nothing here can open it.
    delegate_.onGroup(parsed->payloadType(), payload);
    break;
  default:
    deliverToSelf(*parsed, payload);
    break;
  }

  // 5. A separate if, never an else. A room server is both an endpoint and a
  // repeater: a packet addressed to it still has to travel on, because the
  // neighbours behind it may have missed the original.
  considerForwarding(*parsed, meta);
}

void Router::deliverToSelf(const packet::Packet& p, ByteView payload)
{
  auto envelope = packet::decodeEnvelope(payload);
  if (!envelope.has_value()) return;
  if (envelope->destinationHash != store_.selfHash()) return;

  // One byte of source hash is ambiguous, so every candidate is tried; the MAC
  // that checks out is the real identification.
  //
  // Candidates come from two places, because two different packets can name a
  // sender. The store knows everybody who has advertised. The delegate knows
  // everybody who logged in — a login carries a whole key — and those two sets
  // do not have to overlap: a client can talk to a room for hours without ever
  // sending an advert the room happened to hear.
  const auto contacts = store_.findByHash(envelope->sourceHash);

  std::array<PublicKey, MAX_LOGGED_IN_CANDIDATES> loggedIn;
  const size_t loggedInCount = delegate_.keysMatching(envelope->sourceHash, std::span<PublicKey> { loggedIn });

  const size_t candidates = contacts.size() + loggedInCount;

  for (size_t i = 0; i < candidates; i++) {
    // A contact where we have one, the bare key where we do not. What the
    // delegate reads off it is the key; the rest is what an advert would have
    // filled in and nothing here can invent.
    identity::Contact bare;
    const bool advertised = i < contacts.size();
    if (advertised) {
      bare = *contacts[i];
    }
    else {
      bare.pk = loggedIn[i - contacts.size()];
    }
    const identity::Contact& candidate = advertised ? *contacts[i] : bare;

    const auto* secret = store_.secretFor(candidate.pk);
    if (secret == nullptr) continue;

    protocol::Mac mac;
    std::memcpy(mac.data.data(), envelope->cipherMac.data(), PACKET_MAC_SIZE);

    std::vector<uint8_t> plain(envelope->ciphertext.size());
    auto length = protocol::open(*secret, mac, envelope->ciphertext, ByteSpan { plain.data(), plain.size() });
    if (!length.has_value()) {
      publish(config_.bus, telemetry::EventType::DecryptFailed, now_);
      continue;
    }

    // A flood that reached us carries the path it took; reversed, that is the
    // way back.
    if (p.routeType() == packet::RouteType::FLOOD) learnRouteFrom(p, candidate.pk);

    // A returned path is routing's own business, not the layer above.
    if (p.payloadType() == packet::PayloadType::PATH) {
      if (auto returned = packet::decodePath(ByteView { plain.data(), *length })) {
        installRoute(candidate.pk, returned->path);
      }
      return;
    }

    const ByteView plainView { plain.data(), *length };
    delegate_.onPayload(candidate, p.payloadType(), plainView);

    if (!delegate_.shouldAck(p.payloadType(), plainView)) return;

    // Acknowledge what we could read, so the sender can stop retrying. Over the
    // plaintext and the sender's key: they hashed the same two things before
    // sending, and this has to come out at the same number or every message
    // they send reads as undelivered and is sent again.
    sendAck(candidate.pk, protocol::expectedAck(plainView, candidate.pk), ByteView {});
    return;
  }
}

// A login request comes from somebody who is not a contact yet, so the key it
// is encrypted to travels inside the packet. There is no candidate list and no
// contact to hand up — only the raw key.
void Router::deliverAnon(const packet::Packet& p, ByteView payload)
{
  auto request = packet::decodeAnonReq(payload);
  if (!request.has_value()) return;
  if (request->destinationHash != store_.selfHash()) return;

  PublicKey sender;
  std::memcpy(sender.data.data(), request->publicKey.data(), PACKET_PUBLIC_KEY_SIZE);

  const auto* secret = store_.secretFor(sender);
  if (secret == nullptr) return; // degenerate key in the packet

  protocol::Mac mac;
  std::memcpy(mac.data.data(), request->cipherMac.data(), PACKET_MAC_SIZE);

  std::vector<uint8_t> plain(request->ciphertext.size());
  auto length = protocol::open(*secret, mac, request->ciphertext, ByteSpan { plain.data(), plain.size() });
  if (!length.has_value()) return; // not for us, or damaged

  // Keep the way back so the login response can go direct, but send no PATH
  // packet: the peer is not a contact yet and the response carries the reply.
  if (p.routeType() == packet::RouteType::FLOOD) {
    std::vector<uint8_t> reversed(MAX_PATH_SIZE);
    const size_t reversedSize =
      reversePath(ByteView { p.path.data(), p.pathSize() }, ByteSpan { reversed.data(), reversed.size() });
    installRoute(sender, ByteView { reversed.data(), reversedSize });
  }

  delegate_.onAnon(sender, ByteView { plain.data(), *length });
}

void Router::handleAck(ByteView payload)
{
  if (payload.size() < 4) return;

  const uint32_t value =
    (uint32_t)payload[0] | (uint32_t)payload[1] << 8 | (uint32_t)payload[2] << 16 | (uint32_t)payload[3] << 24;

  for (size_t i = 0; i < pending_.size(); i++) {
    if (pending_[i].expectedAck != value) continue;

    const SendId id = pending_[i].id;
    pending_.erase(pending_.begin() + (long)i);

    delegate_.onAck(id);
    return;
  }
}

void Router::learnRouteFrom(const packet::Packet& p, const PublicKey& sender)
{
  std::vector<uint8_t> reversed(MAX_PATH_SIZE);
  const size_t length =
    reversePath(ByteView { p.path.data(), p.pathSize() }, ByteSpan { reversed.data(), reversed.size() });
  reversed.resize(length);

  if (Route* route = findRoute(sender)) {
    route->path = reversed;
  }
  else {
    if (routes_.size() >= config_.maxRoutes) routes_.erase(routes_.begin());
    routes_.push_back(Route { sender, reversed });
  }

  // We keep the reversed path; the peer gets the one it recorded, which is
  // already the direction it needs. Sending the reversed copy instead is the
  // classic mistake: flood keeps working and direct packets go nowhere.
  sendPathReturn(sender, ByteView { p.path.data(), p.pathSize() });
}

// Hands the peer the route back to us so it can stop flooding.
void Router::installRoute(const PublicKey& to, ByteView path)
{
  if (Route* route = findRoute(to)) {
    route->path.assign(path.begin(), path.end());
    return;
  }
  if (routes_.size() >= config_.maxRoutes) routes_.erase(routes_.begin());
  routes_.push_back(Route { to, std::vector<uint8_t>(path.begin(), path.end()) });
}

void Router::sendPathReturn(const PublicKey& to, ByteView forward)
{
  const auto* secret = store_.secretFor(to);
  if (secret == nullptr) return;

  packet::PathReturn returned;
  returned.path = forward;
  returned.extraType = 0;
  returned.extra = ByteView {};

  std::vector<uint8_t> plain(MAX_PACKET_PAYLOAD);
  auto plainSize = packet::encodePath(returned, ByteSpan { plain.data(), plain.size() });
  if (!plainSize.has_value()) return;

  std::vector<uint8_t> cipher(MAX_PACKET_PAYLOAD);
  auto sealed =
    protocol::seal(*secret, ByteView { plain.data(), *plainSize }, ByteSpan { cipher.data(), cipher.size() });
  if (!sealed.has_value()) return;

  packet::Envelope envelope;
  envelope.destinationHash = to.data[0];
  envelope.sourceHash = store_.selfHash();
  envelope.cipherMac = sealed->mac.view();
  envelope.ciphertext = ByteView { cipher.data(), sealed->ciphertextLength };

  std::vector<uint8_t> payload(MAX_PACKET_PAYLOAD);
  auto payloadSize = packet::encodeEnvelope(envelope, ByteSpan { payload.data(), payload.size() });
  if (!payloadSize.has_value()) return;

  // Sent along the route we just learned, at high priority: until it lands the
  // peer keeps flooding.
  const Route* route = findRoute(to);
  transmitNow(buildFrame(packet::PayloadType::PATH, ByteView { payload.data(), *payloadSize }, route != nullptr,
                route ? ByteView { route->path.data(), route->path.size() } : ByteView {}),
    Priority::HIGH);
}

void Router::considerForwarding(const packet::Packet& p, RxMeta meta)
{
  if (!delegate_.shouldForward(p)) return;

  const bool flood = p.routeType() == packet::RouteType::FLOOD || p.routeType() == packet::RouteType::TRANSPORT_FLOOD;

  packet::Packet copy = p;
  if (flood) {
    // Record ourselves so the receiver can reverse the path, and so the packet
    // stops if it comes back.
    if (!packet::appendSelf(copy, store_.selfHash())) return; // hop limit reached
  }
  else {
    // Direct: forward only if we are the next hop, and drop ourselves from the
    // remaining route. Anything else is not travelling through us.
    if (!packet::stripSelf(copy, store_.selfHash())) return;
  }

  // A trace collects one reading per node that carried it, in the order they
  // did. Flood has just written our hash into the path and the reading lines up
  // with it; a direct trace travels a route its sender wrote out beforehand,
  // and the readings line up with that instead. Either way the count only stays
  // right if every hop appends exactly one, which is why failing to append
  // stops the packet.
  if (copy.payloadType() == packet::PayloadType::TRACE) {
    if (!packet::appendTraceHop(copy, (int16_t)(meta.snr * 4))) return;
  }

  std::vector<uint8_t> frame(MAX_PACKET_FRAME);
  auto written = packet::serialize(copy, ByteSpan { frame.data(), frame.size() });
  if (!written.has_value()) return;
  frame.resize(*written);

  publish(config_.bus, telemetry::EventType::Forwarded, now_);
  const Millis delay = meta.airtime * config_.forwardAirtimeFactor + jitter();

  // Somebody else's packet, so it waits behind everything of ours.
  queueFrame(std::move(frame), now_ + delay, Priority::LOW);
}

// ------------------------------------------------------------------ send

SendId Router::sendDirect(const PublicKey& to, packet::PayloadType type, ByteView payload, bool wantAck)
{
  const SendId id = nextId_++;

  const auto* secret = store_.secretFor(to);
  if (secret == nullptr) return id;

  std::vector<uint8_t> cipher(MAX_PACKET_PAYLOAD);
  auto sealed = protocol::seal(*secret, payload, ByteSpan { cipher.data(), cipher.size() });
  if (!sealed.has_value()) return id;

  packet::Envelope envelope;
  envelope.destinationHash = to.data[0];
  envelope.sourceHash = store_.selfHash();
  envelope.cipherMac = sealed->mac.view();
  envelope.ciphertext = ByteView { cipher.data(), sealed->ciphertextLength };

  std::vector<uint8_t> wire(MAX_PACKET_PAYLOAD);
  auto wireSize = packet::encodeEnvelope(envelope, ByteSpan { wire.data(), wire.size() });
  if (!wireSize.has_value()) return id;

  const Route* route = findRoute(to);
  std::vector<uint8_t> frame = buildFrame(type, ByteView { wire.data(), *wireSize }, route != nullptr,
    route ? ByteView { route->path.data(), route->path.size() } : ByteView {});

  if (wantAck && pending_.size() < config_.maxPending) {
    Pending entry;
    entry.id = id;
    // Our own plaintext and our own key: we are the sender, and the peer will
    // hash exactly this pair when it answers.
    entry.expectedAck = protocol::expectedAck(payload, store_.selfPk());
    entry.viaFlood = route == nullptr;
    entry.deadline = now_
      + (entry.viaFlood ? config_.floodAckTimeout :
                          config_.directAckBase + config_.directAckPerHop * (route ? route->path.size() : 0));
    entry.attempts = 1;
    entry.to = to;
    entry.frame = frame;
    pending_.push_back(std::move(entry));
  }

  transmitNow(std::move(frame), Priority::NORMAL);
  return id;
}

void Router::sendFlood(packet::PayloadType type, ByteView payload)
{
  transmitNow(buildFrame(type, payload, false, ByteView {}), Priority::NORMAL);
}

void Router::sendAck(const PublicKey& to, uint32_t value, ByteView extra)
{
  std::vector<uint8_t> payload;
  payload.reserve(4 + extra.size());
  for (int i = 0; i < 4; i++)
    payload.push_back((uint8_t)(value >> (8 * i)));
  payload.insert(payload.end(), extra.begin(), extra.end());

  const Route* route = findRoute(to);
  transmitNow(buildFrame(packet::PayloadType::ACK, ByteView { payload.data(), payload.size() }, route != nullptr,
                route ? ByteView { route->path.data(), route->path.size() } : ByteView {}),
    Priority::HIGH);
}

// ------------------------------------------------------------------ tick

void Router::tick(Millis now)
{
  now_ = now;

  // Highest priority first, then whatever has waited longest.
  std::stable_sort(queue_.begin(), queue_.end(), [](const Delayed& a, const Delayed& b) {
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.notBefore < b.notBefore;
  });

  for (size_t i = 0; i < queue_.size();) {
    if (queue_[i].notBefore > now_) {
      i++;
      continue;
    }
    std::vector<uint8_t> frame = std::move(queue_[i].frame);
    const Priority priority = queue_[i].priority;
    queue_.erase(queue_.begin() + (long)i);
    handToRadio(ByteView { frame.data(), frame.size() }, priority);
  }

  for (size_t i = 0; i < pending_.size();) {
    Pending& entry = pending_[i];
    if (entry.deadline > now_) {
      i++;
      continue;
    }

    if (entry.attempts >= config_.maxAttempts) {
      const SendId id = entry.id;
      const PublicKey to = entry.to;
      pending_.erase(pending_.begin() + (long)i);
      publish(config_.bus, telemetry::EventType::AckTimeout, now_, id);
      publish(config_.bus, telemetry::EventType::RouteReset, now_);

      // The only way back from a repeater switched off mid-chain: forget the
      // route, and the next message floods and finds a new one.
      forgetRoute(to);
      delegate_.onDeliveryFailed(id);
      continue;
    }

    entry.attempts++;
    entry.deadline = now_ + (entry.viaFlood ? config_.floodAckTimeout : config_.directAckBase);
    handToRadio(ByteView { entry.frame.data(), entry.frame.size() }, Priority::NORMAL);
    i++;
  }
}
