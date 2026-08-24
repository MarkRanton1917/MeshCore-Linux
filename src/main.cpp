// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

// Composition root. The only file where the modules learn about each other:
// every one of them is written so that it can be built and tested without
// knowing who sits on the other side, and the wiring is paid for here, once.

#include "identity.h"
#include "packet.h"
#include "platform.h"
#include "protocol.h"
#include "radio.h"
#include "repeater.h"
#include "room.h"
#include "routing.h"
#include "telemetry.h"

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace core = crypto::core;
namespace protocol = crypto::protocol;

namespace {

volatile sig_atomic_t stopping = 0;

void onSignal(int)
{
  stopping = 1;
}

// routing hands frames to something with enqueue(); radio offers send(). The
// mismatch is deliberate — neither module should name the other — so the
// adapter lives here.
class RadioLink : public routing::Radio {
public:
  RadioLink(radio::IRadio& device)
    : device_(device)
  {
  }

  void enqueue(ByteView frame, routing::Priority priority) override
  {
    device_.send(frame, translate(priority));
  }

private:
  // Two enums with the same three levels, and neither module may name the
  // other. The mapping is one switch rather than a cast, so adding a level to
  // one of them fails to compile here instead of silently becoming HIGH.
  static radio::Priority translate(routing::Priority priority)
  {
    switch (priority) {
    case routing::Priority::HIGH:
      return radio::Priority::HIGH;
    case routing::Priority::LOW:
      return radio::Priority::LOW;
    case routing::Priority::NORMAL:
      break;
    }
    return radio::Priority::NORMAL;
  }

  radio::IRadio& device_;
};

// The other direction, plus the one job routing deliberately does not do.
// An advert is unaddressed and unencrypted, so verifying and remembering it is
// policy, and policy belongs in the composition root.
class Receiver : public radio::RxSink {
public:
  Receiver(identity::Store& store, telemetry::Bus& bus, platform::SystemClock& clock)
    : store_(store),
      bus_(bus),
      clock_(clock)
  {
  }

  void attach(routing::Router& router)
  {
    router_ = &router;
  }

  void onFrame(ByteView frame, const radio::RxMeta& meta) override
  {
    rememberIfAdvert(frame, meta);

    routing::RxMeta rx;
    rx.airtime = meta.airtimeUs / 1000;
    rx.snr = meta.snr;
    if (router_ != nullptr) router_->onFrame(frame, rx);
  }

private:
  void rememberIfAdvert(ByteView frame, const radio::RxMeta& meta)
  {
    auto parsed = packet::parse(frame);
    if (!parsed.has_value()) return;
    if (parsed->payloadType() != packet::PayloadType::ADVERT) return;

    // Signature first: without it anyone could claim any key.
    if (!protocol::packetVerify(frame)) return;

    auto advert = packet::decodeAdvert(parsed->payloadView());
    if (!advert.has_value()) return;

    // Our own advert, handed back by a neighbour that forwarded it. Recording
    // it would waste a slot and put us in our own hash bucket, where every
    // packet from a node sharing our first byte would try to decrypt against a
    // secret shared with ourselves. It is still worth counting on the way out:
    // it is the one thing this node hears that proves its own transmissions
    // arrive somewhere. Everything else in the log reads the same on a node
    // whose antenna has fallen off.
    if (std::memcmp(advert->publicKey.data(), store_.selfPk().data.data(), PACKET_PUBLIC_KEY_SIZE) == 0) {
      bus_.publish(telemetry::EventType::AdvertEchoed, (uint32_t)(meta.at / 1000));
      if (!echoed_) {
        echoed_ = true;

        // The first hash in the path is whoever heard us off our own
        // transmitter and passed it on, which is the neighbour worth naming.
        // An empty path cannot happen on a half-duplex radio, so if it ever
        // does, say so rather than name node 00.
        if (parsed->hopCount > 0) {
          platform::Log::write(platform::LogLevel::INFO,
            "our advert came back from %02x: we are on the air and being heard", parsed->path[0]);
        }
        else {
          platform::Log::write(platform::LogLevel::WARN, "our own advert came back with an empty path");
        }
      }
      return;
    }

    const identity::Update update = store_.remember(*advert);
    if (update == identity::Update::ADDED) {
      bus_.publish(telemetry::EventType::ContactAdded, (uint32_t)(meta.at / 1000));
      platform::Log::write(platform::LogLevel::INFO, "new contact");
    }

    // Even a stale advert says the node is alive and how well we hear it, so
    // this is recorded whatever remember() made of the claim inside. An advert
    // that arrived with an empty path came off that node's own transmitter.
    core::PublicKey heard;
    std::memcpy(heard.data.data(), advert->publicKey.data(), PACKET_PUBLIC_KEY_SIZE);
    store_.noteHeard(heard, clock_.wall(), meta.snr, parsed->hopCount);
  }

  identity::Store& store_;
  telemetry::Bus& bus_;
  platform::SystemClock& clock_;
  routing::Router* router_ = nullptr;

  // Logged once, counted always: the proof is worth a line the first time and
  // noise every five minutes after that.
  bool echoed_ = false;
};

// room only ever needs one thing from routing.
class RouterSender : public room::Sender {
public:
  void attach(routing::Router& router)
  {
    router_ = &router;
  }

  routing::SendId sendDirect(const core::PublicKey& to,
    packet::PayloadType type,
    ByteView payload,
    bool wantAck) override
  {
    if (router_ == nullptr) return 0;
    return router_->sendDirect(to, type, payload, wantAck);
  }

  void sendFlood(packet::PayloadType type, ByteView payload) override
  {
    if (router_ != nullptr) router_->sendFlood(type, payload);
  }

private:
  routing::Router* router_ = nullptr;
};

// Everything an admin command reaches for that the room does not own. Nothing
// here acts at once: a command runs inside the receive path, and rebooting or
// transmitting from there would cut off the reply that is still to be sent.
// Each one sets an intent the main loop picks up on its next turn.
class HostAdmin : public room::Admin {
public:
  HostAdmin(std::string name, platform::Overlay& overlay, platform::Millis startedAt)
    : name_(std::move(name)),
      overlay_(overlay),
      startedAt_(startedAt)
  {
  }

  void sendAdvert() override
  {
    advertWanted_ = true;
  }

  bool setClock(uint32_t unixSeconds) override
  {
    return platform::SystemClock::setWall(unixSeconds);
  }

  std::string nodeName() const override
  {
    return name_;
  }

  // Stored before it takes effect: a name that reverts at the next restart is
  // worse than one that could not be changed at all.
  bool setNodeName(std::string_view name) override
  {
    if (!saveSetting("node.name", name)) return false;

    name_.assign(name);
    return true;
  }

  bool saveSetting(std::string_view key, std::string_view value) override
  {
    if (!overlay_.set(key, value)) {
      platform::Log::write(platform::LogLevel::ERROR, "cannot write %s", overlay_.path().c_str());
      return false;
    }
    platform::Log::write(platform::LogLevel::INFO, "setting %.*s changed over the air", (int)key.size(), key.data());
    return true;
  }

  void requestReboot() override
  {
    rebootAt_ = now_ + kRebootDelay;
  }

  uint32_t uptime() const override
  {
    return (uint32_t)((now_ - startedAt_) / 1000);
  }

  // Called once per turn of the loop, before the intents are read.
  void tick(platform::Millis now)
  {
    now_ = now;
  }

  bool takeAdvertRequest()
  {
    const bool wanted = advertWanted_;
    advertWanted_ = false;
    return wanted;
  }

  bool rebootDue() const
  {
    return rebootAt_ != 0 && now_ >= rebootAt_;
  }

private:
  // Long enough for the reply to clear the transmit queue. Exiting the instant
  // the command lands loses it, and the admin sees a node that went quiet.
  static constexpr platform::Millis kRebootDelay = 3000;

  std::string name_;
  platform::Overlay& overlay_;
  platform::Millis startedAt_ = 0;
  platform::Millis now_ = 0;
  platform::Millis rebootAt_ = 0;
  bool advertWanted_ = false;
};

// Where the node stands, in the units the advert uses: millionths of a degree,
// signed, little-endian. Optional because a node that does not know cannot
// guess, and a wrong position on somebody's map is worse than none.
struct Location {
  bool known = false;
  int32_t latitude = 0;
  int32_t longitude = 0;
};

// Degrees as they are written on a map. Out-of-range numbers are refused rather
// than clamped: 55.75 typed as 5575 is a mistake, and a node that quietly
// advertises the equator is a mistake nobody finds.
Location locationFrom(const platform::Config& config)
{
  Location out;
  if (!config.has("node.lat") && !config.has("node.lon")) return out;
  if (!config.has("node.lat") || !config.has("node.lon")) {
    platform::Log::write(
      platform::LogLevel::ERROR, "node.lat and node.lon go together; advertising without a position");
    return out;
  }

  const double latitude = std::strtod(config.get("node.lat").c_str(), nullptr);
  const double longitude = std::strtod(config.get("node.lon").c_str(), nullptr);
  if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) {
    platform::Log::write(
      platform::LogLevel::ERROR, "node.lat/node.lon are outside the globe; advertising without a position");
    return out;
  }

  out.known = true;
  out.latitude = (int32_t)std::llround(latitude * 1e6);
  out.longitude = (int32_t)std::llround(longitude * 1e6);
  return out;
}

void appendInt32(std::vector<uint8_t>& out, int32_t value)
{
  const uint32_t bits = (uint32_t)value;
  out.push_back((uint8_t)(bits));
  out.push_back((uint8_t)(bits >> 8));
  out.push_back((uint8_t)(bits >> 16));
  out.push_back((uint8_t)(bits >> 24));
}

// Our own advert: build it with an empty signature, sign the frame that
// results, then put the signature back where it belongs.
//
// The appdata order is the flags byte, then the position, then the two feature
// blocks, then the name — the same walk the receiving side does in identity's
// applyAppdata, and the same one every node on the air already speaks.
std::vector<uint8_t> buildAdvertPayload(const identity::Store& store,
  uint32_t timestamp,
  const std::string& name,
  identity::NodeType type,
  const Location& location)
{
  std::vector<uint8_t> appdata;
  uint8_t flags = (uint8_t)type;
  if (location.known) flags |= ADVERT_HAS_LOCATION;
  if (!name.empty()) flags |= ADVERT_HAS_NAME;
  appdata.push_back(flags);

  if (location.known) {
    appendInt32(appdata, location.latitude);
    appendInt32(appdata, location.longitude);
  }
  appdata.insert(appdata.end(), name.begin(), name.end());

  core::Signature blank {};
  packet::Advert advert;
  advert.publicKey = store.selfPk().view();
  advert.timestamp = timestamp;
  advert.signature = blank.view();
  advert.appdata = ByteView { appdata.data(), appdata.size() };

  std::vector<uint8_t> payload(MAX_PACKET_PAYLOAD);
  auto size = packet::encodeAdvert(advert, ByteSpan { payload.data(), payload.size() });
  if (!size.has_value()) return {};
  payload.resize(*size);

  std::vector<uint8_t> frame;
  frame.push_back((uint8_t)(((uint8_t)packet::PayloadType::ADVERT << 2) | (uint8_t)packet::RouteType::FLOOD));
  frame.push_back(0x00); // no hops yet
  frame.insert(frame.end(), payload.begin(), payload.end());

  const core::Signature signature = protocol::packetSign(store.selfSk(), ByteView { frame.data(), frame.size() });
  std::memcpy(
    payload.data() + PACKET_PUBLIC_KEY_SIZE + PACKET_TIMESTAMP_SIZE, signature.data.data(), PACKET_SIGNATURE_SIZE);

  return payload;
}

// Neither gauge belongs on IRadio: they are diagnostics, not something the
// stack above may steer by.
uint32_t queueDepthOf(const radio::IRadio& device)
{
#if defined(SX1262_RADIO)
  if (auto* chip = dynamic_cast<const radio::Sx1262Radio*>(&device)) return (uint32_t)chip->queueDepth();
#elif defined(UDP_RADIO)
  if (auto* udp = dynamic_cast<const radio::UdpRadio*>(&device)) return (uint32_t)udp->queueDepth();
#endif
  return 0;
}

uint32_t usedPermilleOf(const radio::IRadio& device)
{
#if defined(SX1262_RADIO)
  if (auto* chip = dynamic_cast<const radio::Sx1262Radio*>(&device)) return chip->usedPermille();
#elif defined(UDP_RADIO)
  if (auto* udp = dynamic_cast<const radio::UdpRadio*>(&device)) return udp->usedPermille();
#endif
  return 0;
}

// Config entries read "name:hex", the key being 32 bytes as 64 hex characters.
// One string per channel rather than an object, because the config parser
// flattens objects to text and refuses an array of them.
std::vector<room::Channel> channelsFrom(const std::vector<std::string>& entries)
{
  std::vector<room::Channel> channels;

  for (const std::string& entry : entries) {
    const size_t colon = entry.find(':');
    if (colon == std::string::npos) {
      platform::Log::write(platform::LogLevel::ERROR, "channel '%s' is not name:key", entry.c_str());
      continue;
    }

    const std::string name = entry.substr(0, colon);
    const std::string hex = entry.substr(colon + 1);
    if (name.empty() || name.size() > MAX_CHANNEL_NAME || hex.size() != PACKET_SHARED_SECRET_SIZE * 2) {
      platform::Log::write(platform::LogLevel::ERROR, "channel '%s' needs a name and %d hex characters", name.c_str(),
        PACKET_SHARED_SECRET_SIZE * 2);
      continue;
    }

    room::Channel channel;
    channel.name = name;
    bool ok = true;
    for (size_t i = 0; i < PACKET_SHARED_SECRET_SIZE && ok; i++) {
      unsigned byte = 0;
      if (std::sscanf(hex.c_str() + i * 2, "%2x", &byte) != 1) ok = false;
      channel.secret.span()[i] = (uint8_t)byte;
    }
    if (!ok) {
      platform::Log::write(platform::LogLevel::ERROR, "channel '%s' has a bad key", name.c_str());
      continue;
    }

    channel.hash = room::channelHashOf(channel.secret);
    if (channels.size() >= MAX_CHANNELS) {
      platform::Log::write(platform::LogLevel::WARN, "more than %d channels, '%s' ignored", MAX_CHANNELS, name.c_str());
      break;
    }
    channels.push_back(std::move(channel));
  }
  return channels;
}

// The config spelling of a node type. Everything else about the three follows
// from the value itself: room asks it whether there is a board, and the wiring
// below asks it whether to build a transit policy at all.
room::NodeType nodeTypeFromName(const std::string& name)
{
  if (name == "room") return room::NodeType::ROOM;
  if (name == "repeater") return room::NodeType::REPEATER;
  if (name == "room-repeater") return room::NodeType::ROOM_REPEATER;

  // A typo must not quietly turn a repeater into something that carries
  // nothing, so the fallback is named in the log rather than left to be
  // discovered.
  platform::Log::write(
    platform::LogLevel::WARN, "unknown node.type '%s', running as a room and carrying nothing", name.c_str());
  return room::NodeType::ROOM;
}

const char* nameOf(room::NodeType type)
{
  switch (type) {
  case room::NodeType::REPEATER:
    return "repeater";
  case room::NodeType::ROOM_REPEATER:
    return "room-repeater";
  case room::NodeType::ROOM:
    break;
  }
  return "room";
}

const char* nameOf(identity::NodeType type)
{
  switch (type) {
  case identity::NodeType::CHAT:
    return "chat node";
  case identity::NodeType::REPEATER:
    return "repeater";
  case identity::NodeType::ROOM_SERVER:
    return "room server";
  case identity::NodeType::SENSOR:
    return "sensor";
  case identity::NodeType::UNKNOWN:
    break;
  }
  return "node of no stated type";
}

// What the network is told this node is. Only one value fits the four bits an
// advert has for it, so a node that keeps a board is a room server first:
// clients look for that, and nothing in an advert says whether a node repeats.
// A node with no board has nothing else to claim, and says repeater.
//
// Derived, never configured: the node already knows which of the three it is,
// and a second setting saying otherwise would be a second answer to a question
// that has one.
identity::NodeType advertisedAs(room::NodeType type)
{
  return room::keepsBoard(type) ? identity::NodeType::ROOM_SERVER : identity::NodeType::REPEATER;
}

// "1f" or "0x1f", one node hash each. A hash and not a key: one byte is what a
// packet carries, and blocking one blocks everybody who shares it.
std::vector<uint8_t> blockedFrom(const std::vector<std::string>& entries)
{
  std::vector<uint8_t> blocked;
  for (const std::string& entry : entries) {
    unsigned value = 0;
    if (std::sscanf(entry.c_str(), "%x", &value) != 1 || value > 0xFF) {
      platform::Log::write(platform::LogLevel::ERROR, "blocked '%s' is not a node hash", entry.c_str());
      continue;
    }
    blocked.push_back((uint8_t)value);
    platform::Log::write(platform::LogLevel::WARN, "not carrying traffic for %02x", value);
  }
  return blocked;
}

platform::LogLevel levelFromName(const std::string& name)
{
  if (name == "error") return platform::LogLevel::ERROR;
  if (name == "warn") return platform::LogLevel::WARN;
  if (name == "debug") return platform::LogLevel::DEBUG;
  return platform::LogLevel::INFO;
}

} // namespace

int main(int argc, char** argv)
{
  const std::string configPath = argc > 1 ? argv[1] : "meshcore.json";

  platform::Config config;
  if (!config.loadFile(configPath)) {
    std::fprintf(stderr, "cannot read %s\n", configPath.c_str());
    return 1;
  }

  // Where the node keeps what it learned, and the one setting an overlay may
  // not shadow: the overlay lives inside it.
  const std::string dataDir = config.get("node.dir", "./data");

  // What an admin changed over the air, laid over the operator's file, which is
  // never written to. A damaged overlay is fatal on purpose — coming up with
  // half of those settings, a password among them, is worse than not coming up.
  platform::Overlay overlay(dataDir + "/overrides.json");
  if (!overlay.load()) {
    std::fprintf(stderr, "cannot read %s\n", overlay.path().c_str());
    return 1;
  }
  config.applyOverlay(overlay);

  platform::Log::setLevel(levelFromName(config.get("log.level", "info")));
  if (!overlay.values().empty()) {
    // Otherwise editing the config and seeing nothing change is a mystery.
    platform::Log::write(platform::LogLevel::WARN, "%zu setting(s) come from %s, not the config",
      overlay.values().size(), overlay.path().c_str());
  }

  if (!core::init(nullptr)) {
    platform::Log::write(platform::LogLevel::ERROR, "crypto backend failed to start");
    return 1;
  }

  platform::SystemClock clock;

  // A node that advertises itself dated 1970 corrupts its neighbours' records,
  // so refuse to start before the clock is set.
  if (!platform::SystemClock::wallLooksSynced(clock.wall())) {
    platform::Log::write(platform::LogLevel::ERROR, "wall clock not synchronised yet");
    return 1;
  }

  identity::Store store;
  if (!store.loadOrCreate(dataDir)) {
    // Never silently mint a new identity: the network would see a stranger and
    // nobody would notice.
    platform::Log::write(platform::LogLevel::ERROR, "identity unusable, refusing to start");
    return 1;
  }
  platform::Log::write(
    platform::LogLevel::INFO, "node hash %02x, %zu contacts", store.selfHash(), store.contactCount());

  telemetry::Bus bus(config.getBool("telemetry.enabled", true) ? (size_t)config.getInt("telemetry.queue", 256) : 1);
  telemetry::Collector collector;
  const bool telemetryOn = config.getBool("telemetry.enabled", true);

  radio::Params params;
  params.frequencyHz = (uint32_t)config.getInt("radio.frequency", params.frequencyHz);
  params.spreadingFactor = (uint8_t)config.getInt("radio.spreading_factor", params.spreadingFactor);
  params.bandwidthHz = (uint32_t)config.getInt("radio.bandwidth", params.bandwidthHz);
  params.codingRate = (uint8_t)config.getInt("radio.coding_rate", params.codingRate);
  params.dutyCyclePercent = (uint8_t)config.getInt("radio.duty_cycle", params.dutyCyclePercent);

  // Everything above talks to IRadio and nothing else, so which radio this is
  // gets settled here and nowhere else -- at compile time, by -DRADIO=. A node
  // cannot ask for hardware the binary was not built with, because there is no
  // question to answer wrongly.
  std::unique_ptr<radio::IRadio> device;

#if defined(SX1262_RADIO)
  {
    radio::Sx1262Options chip;
    chip.spiBus = (uint8_t)config.getInt("radio.spi_bus", chip.spiBus);
    chip.spiChipSelect = (uint8_t)config.getInt("radio.spi_cs", chip.spiChipSelect);
    chip.spiSpeedHz = (uint32_t)config.getInt("radio.spi_speed", chip.spiSpeedHz);
    chip.gpioChip = (uint8_t)config.getInt("radio.gpio_chip", chip.gpioChip);
    chip.pinNss = (int32_t)config.getInt("radio.pin_nss", chip.pinNss);
    chip.pinBusy = (int32_t)config.getInt("radio.pin_busy", chip.pinBusy);
    chip.pinReset = (int32_t)config.getInt("radio.pin_reset", chip.pinReset);
    chip.pinDio1 = (int32_t)config.getInt("radio.pin_dio1", chip.pinDio1);
    chip.pinRxEnable = (int32_t)config.getInt("radio.pin_rx_enable", chip.pinRxEnable);
    chip.pinTxEnable = (int32_t)config.getInt("radio.pin_tx_enable", chip.pinTxEnable);
    chip.dio2AsRfSwitch = config.getBool("radio.dio2_rf_switch", chip.dio2AsRfSwitch);
    chip.useRegulatorLdo = config.getBool("radio.regulator_ldo", chip.useRegulatorLdo);
    chip.txPowerDbm = (int8_t)config.getInt("radio.tx_power", chip.txPowerDbm);
    chip.currentLimitMa = (float)config.getInt("radio.current_limit_ma", (long)chip.currentLimitMa);

    // Millivolts, because the config reads integers and 1.6 would arrive as 1.
    // Zero is a board with a plain crystal, which is a real answer and not a
    // missing one.
    chip.tcxoVoltage = (float)config.getInt("radio.tcxo_millivolts", 0) / 1000.0f;

    auto chipDevice = std::make_unique<radio::Sx1262Radio>(chip, params);
    if (!chipDevice->open()) {
      platform::Log::write(platform::LogLevel::ERROR, "radio unavailable, refusing to start");
      return 1;
    }
    device = std::move(chipDevice);
  }
#elif defined(UDP_RADIO)
  {
    radio::UdpOptions udp;
    udp.bindAddress = config.get("radio.udp_bind", "127.0.0.1");
    udp.listenPort = (uint16_t)config.getInt("radio.udp_port", 0);
    udp.peers = config.getList("radio.udp_peers");
    udp.multicastGroup = config.get("radio.udp_group");
    udp.multicastPort = (uint16_t)config.getInt("radio.udp_group_port", 4242);

    auto udpDevice = std::make_unique<radio::UdpRadio>(udp, params);
    if (!udpDevice->open()) {
      platform::Log::write(platform::LogLevel::ERROR, "radio unavailable, refusing to start");
      return 1;
    }
    device = std::move(udpDevice);
  }
#else
#error "no radio: configure the build with -DRADIO=UDP or -DRADIO=SX1262"
#endif

  Receiver receiver(store, bus, clock);
  device->setSink(&receiver);

  RadioLink link(*device);
  RouterSender sender;

  routing::Config routingConfig;
  routingConfig.bus = telemetryOn ? &bus : nullptr;
  routingConfig.forwardAirtimeFactor =
    (uint32_t)config.getInt("routing.forward_airtime_factor", routingConfig.forwardAirtimeFactor);
  routingConfig.forwardJitter =
    (routing::Millis)config.getInt("routing.forward_jitter", (long)routingConfig.forwardJitter);
  routingConfig.maxRoutes = (size_t)config.getInt("routing.max_routes", (long)routingConfig.maxRoutes);
  routingConfig.seenSlots = (size_t)config.getInt("routing.seen_slots", (long)routingConfig.seenSlots);
  routingConfig.seenTtl = (routing::Millis)config.getInt("routing.seen_ttl_ms", (long)routingConfig.seenTtl);

  const room::NodeType nodeType = nodeTypeFromName(config.get("node.type", "room"));

  // On what terms other people's packets travel on. Only the types that repeat
  // get a policy at all, so the `repeater` settings are read for those and
  // ignored for the rest rather than half-applied to a node that was never
  // going to use them.
  std::optional<repeater::Policy> forwarder;
  if (room::repeats(nodeType)) {
    repeater::Config transitConfig;
    transitConfig.enabled = config.getBool("repeater.enabled", transitConfig.enabled);
    transitConfig.maxHops = (uint8_t)config.getInt("repeater.max_hops", transitConfig.maxHops);
    transitConfig.floodPerMinute = (uint32_t)config.getInt("repeater.flood_per_minute", transitConfig.floodPerMinute);
    transitConfig.dutyCeilingPermille =
      (uint32_t)config.getInt("repeater.duty_ceiling", transitConfig.dutyCeilingPermille);
    transitConfig.blocked = blockedFrom(config.getList("repeater.blocked"));
    forwarder.emplace(transitConfig);

    if (!forwarder->enabled()) {
      // Worth saying out loud: a node that hears everything and passes nothing
      // on looks exactly like a network with a hole in it.
      platform::Log::write(platform::LogLevel::WARN, "transit is off, this node carries nothing for anybody");
    }
  }

  // Already the effective value: the overlay was laid over the config above.
  HostAdmin admin(config.get("node.name", "room"), overlay, clock.mono());

  room::Config roomConfig;
  roomConfig.type = nodeType;
  roomConfig.adminPassword = config.get("room.admin_password");
  roomConfig.guestPassword = config.get("room.guest_password");
  roomConfig.allowAnonymousRead = config.getBool("room.anonymous_read", false);
  roomConfig.bus = telemetryOn ? &bus : nullptr;
  roomConfig.admin = &admin;
  roomConfig.forwarder = forwarder.has_value() ? &*forwarder : nullptr;

  // A repeater has no board for a channel message to land on, so its channels
  // are not opened at all. Said out loud, because a config full of channels on
  // a node that ignores every one of them is not obvious from the outside.
  if (room::keepsBoard(nodeType)) {
    roomConfig.channels = channelsFrom(config.getList("room.channels"));
    for (const room::Channel& channel : roomConfig.channels) {
      platform::Log::write(platform::LogLevel::INFO, "channel '%s', hash %02x", channel.name.c_str(), channel.hash);
    }
  }
  else if (!config.getList("room.channels").empty() || !config.get("room.guest_password").empty()) {
    platform::Log::write(platform::LogLevel::WARN, "node.type is repeater: no board, so the room settings do nothing");
  }

  room::Room room(store, sender, roomConfig);
  if (!room.load(dataDir)) {
    platform::Log::write(platform::LogLevel::ERROR, "room state unreadable, refusing to start");
    return 1;
  }

  // routing needs the delegate, room needs the router: the cycle is broken by
  // attaching after both exist.
  routing::Router router(store, link, room, routingConfig);
  receiver.attach(router);
  sender.attach(router);

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  const platform::Millis flushEvery = (platform::Millis)config.getInt("node.flush_ms", 60000);
  const platform::Millis advertEvery = (platform::Millis)config.getInt("node.advert_ms", 300000);
  const platform::Millis reportEvery = (platform::Millis)config.getInt("telemetry.report_ms", 60000);

  platform::Millis nextFlush = 0;
  platform::Millis nextAdvert = 0;
  platform::Millis nextReport = 0;

  // Decided once: neither changes while the node runs, and an advert built from
  // them goes out every few minutes.
  const identity::NodeType claimedType = advertisedAs(nodeType);
  const Location location = locationFrom(config);

  platform::Log::write(platform::LogLevel::INFO, "started as '%s', a %s, advertising as a %s", admin.nodeName().c_str(),
    nameOf(nodeType), nameOf(claimedType));
  if (location.known) {
    platform::Log::write(platform::LogLevel::INFO, "advertising a position of %.6f, %.6f",
      (double)location.latitude / 1e6, (double)location.longitude / 1e6);
  }

  while (stopping == 0) {
    const platform::Millis now = clock.mono();

    admin.tick(now);
    device->tick(now);
    router.tick(now);
    room.setServerTime(clock.wall());

    // How much air has gone, so the transit policy can stand aside before the
    // duty cycle does it for us — by then our own replies are queued behind
    // other people's packets.
    room.setRadioLoad(usedPermilleOf(*device));
    room.tick(now);

    // On the timer, or because a command asked for one — after a rename the
    // network holds the old name until the next advert goes out.
    if (now >= nextAdvert || admin.takeAdvertRequest()) {
      const std::vector<uint8_t> payload =
        buildAdvertPayload(store, clock.wall(), admin.nodeName(), claimedType, location);
      if (!payload.empty()) {
        router.sendFlood(packet::PayloadType::ADVERT, ByteView { payload.data(), payload.size() });
      }
      nextAdvert = now + advertEvery;
    }

    // Exiting is the whole of it: the supervisor restarts us. Late enough that
    // the reply to the command has left the queue.
    if (admin.rebootDue()) {
      platform::Log::write(platform::LogLevel::INFO, "reboot requested over the air");
      break;
    }

    if (now >= nextFlush) {
      // Lazily, on a timer: adverts arrive constantly and a write per advert
      // would stall the node and wear out the card.
      store.flush();
      room.flush();
      nextFlush = now + flushEvery;
    }

    if (telemetryOn && now >= nextReport) {
      collector.drain(bus);
      // Both gauges come from whichever driver is in use.
      collector.observe(queueDepthOf(*device), usedPermilleOf(*device));
      telemetry::logCounters(collector.counters());
      nextReport = now + reportEvery;
    }

    // Nothing here is blocking, so yield instead of spinning a core.
    struct timespec idle {
      0, 5 * 1000 * 1000
    };
    nanosleep(&idle, nullptr);
  }

  platform::Log::write(platform::LogLevel::INFO, "stopping, saving state");
  const bool saved = store.flush() && room.flush();
  return saved ? 0 : 1;
}
