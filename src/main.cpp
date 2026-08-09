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
#include "room.h"
#include "routing.h"
#include "telemetry.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
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
    device_.send(frame, priority == routing::Priority::HIGH ? radio::Priority::HIGH : radio::Priority::NORMAL);
  }

private:
  radio::IRadio& device_;
};

// The other direction, plus the one job routing deliberately does not do.
// An advert is unaddressed and unencrypted, so verifying and remembering it is
// policy, and policy belongs in the composition root.
class Receiver : public radio::RxSink {
public:
  Receiver(identity::Store& store, telemetry::Bus& bus)
    : store_(store),
      bus_(bus)
  {
  }

  void attach(routing::Router& router)
  {
    router_ = &router;
  }

  void onFrame(ByteView frame, const radio::RxMeta& meta) override
  {
    rememberIfAdvert(frame, meta.at);

    routing::RxMeta rx;
    rx.airtime = meta.airtimeUs / 1000;
    rx.snr = meta.snr;
    if (router_ != nullptr) router_->onFrame(frame, rx);
  }

private:
  void rememberIfAdvert(ByteView frame, platform::Millis at)
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
    // secret shared with ourselves.
    if (std::memcmp(advert->publicKey.data(), store_.selfPk().data.data(), PACKET_PUBLIC_KEY_SIZE) == 0) {
      return;
    }

    const identity::Update update = store_.remember(*advert);
    if (update == identity::Update::ADDED) {
      bus_.publish(telemetry::EventType::ContactAdded, (uint32_t)(at / 1000));
      platform::Log::write(platform::LogLevel::INFO, "new contact");
    }
  }

  identity::Store& store_;
  telemetry::Bus& bus_;
  routing::Router* router_ = nullptr;
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

private:
  routing::Router* router_ = nullptr;
};

// Our own advert: build it with an empty signature, sign the frame that
// results, then put the signature back where it belongs.
std::vector<uint8_t> buildAdvertPayload(const identity::Store& store, uint32_t timestamp, const std::string& name)
{
  std::vector<uint8_t> appdata;
  appdata.push_back((uint8_t)((uint8_t)identity::NodeType::ROOM_SERVER | (name.empty() ? 0 : ADVERT_HAS_NAME)));
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
  if (auto* udp = dynamic_cast<const radio::UdpRadio*>(&device)) return (uint32_t)udp->queueDepth();
  if (auto* fake = dynamic_cast<const radio::VirtualRadio*>(&device)) {
    return (uint32_t)fake->queueDepth();
  }
  return 0;
}

uint32_t usedPermilleOf(const radio::IRadio& device)
{
  if (auto* udp = dynamic_cast<const radio::UdpRadio*>(&device)) return udp->usedPermille();
  if (auto* fake = dynamic_cast<const radio::VirtualRadio*>(&device)) return fake->usedPermille();
  return 0;
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
  platform::Log::setLevel(levelFromName(config.get("log.level", "info")));

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

  const std::string dataDir = config.get("node.dir", "./data");

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

  // Everything above talks to IRadio and nothing else, so the driver is a
  // choice made here and nowhere else. The SX1262 one still needs RadioLib on
  // real hardware.
  const std::string driver = config.get("radio.driver", "virtual");

  radio::Medium medium;
  std::unique_ptr<radio::IRadio> device;

  if (driver == "udp") {
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
  else if (driver == "virtual") {
    device = std::make_unique<radio::VirtualRadio>(medium, params);
  }
  else {
    platform::Log::write(platform::LogLevel::ERROR, "unknown radio.driver '%s'", driver.c_str());
    return 1;
  }

  Receiver receiver(store, bus);
  device->setSink(&receiver);

  RadioLink link(*device);
  RouterSender sender;

  routing::Config routingConfig;
  routingConfig.bus = telemetryOn ? &bus : nullptr;

  room::Config roomConfig;
  roomConfig.adminPassword = config.get("room.admin_password");
  roomConfig.guestPassword = config.get("room.guest_password");
  roomConfig.allowAnonymousRead = config.getBool("room.anonymous_read", false);
  roomConfig.bus = telemetryOn ? &bus : nullptr;

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

  const std::string name = config.get("node.name", "room");
  platform::Log::write(platform::LogLevel::INFO, "started");

  while (stopping == 0) {
    const platform::Millis now = clock.mono();

    device->tick(now);
    router.tick(now);
    room.setServerTime(clock.wall());
    room.tick(now);

    if (now >= nextAdvert) {
      const std::vector<uint8_t> payload = buildAdvertPayload(store, clock.wall(), name);
      if (!payload.empty()) {
        router.sendFlood(packet::PayloadType::ADVERT, ByteView { payload.data(), payload.size() });
      }
      nextAdvert = now + advertEvery;
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
