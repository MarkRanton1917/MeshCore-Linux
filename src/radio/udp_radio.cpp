#include "radio.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace radio;

// The sender id in front of every datagram. Eight bytes is enough to tell our
// own transmissions apart from everybody else's, which is all it is for.
static constexpr size_t kSenderIdSize = 8;
static constexpr size_t kDatagramMax = kSenderIdSize + MAX_PACKET_FRAME;

static struct sockaddr_in toSockaddr(uint32_t address, uint16_t port)
{
  struct sockaddr_in out;
  std::memset(&out, 0, sizeof out);
  out.sin_family = AF_INET;
  out.sin_port = htons(port);
  out.sin_addr.s_addr = address;
  return out;
}

UdpRadio::UdpRadio(UdpOptions options, Params params)
  : options_(std::move(options)),
    params_(params),
    duty_(params.dutyCyclePercent, params.dutyWindowMs),
    queue_(params.queueDepth)
{
  crypto::core::randomBytes(ByteSpan { (uint8_t*)&selfId_, sizeof selfId_ });
}

UdpRadio::~UdpRadio()
{
  if (socket_ >= 0) ::close(socket_);
}

bool UdpRadio::open()
{
  socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_ < 0) {
    platform::Log::write(platform::LogLevel::ERROR, "udp: socket failed: %s", strerror(errno));
    return false;
  }

  int one = 1;
  setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  struct sockaddr_in local;
  std::memset(&local, 0, sizeof local);
  local.sin_family = AF_INET;
  local.sin_port = htons(options_.listenPort);

  // A multicast member has to bind the wildcard address to receive the group.
  const bool useMulticast = !options_.multicastGroup.empty();
  if (useMulticast) {
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(options_.multicastPort);
  }
  else if (inet_pton(AF_INET, options_.bindAddress.c_str(), &local.sin_addr) != 1) {
    platform::Log::write(platform::LogLevel::ERROR, "udp: bad bind address %s", options_.bindAddress.c_str());
    return false;
  }

  if (bind(socket_, (struct sockaddr*)&local, sizeof local) < 0) {
    platform::Log::write(platform::LogLevel::ERROR, "udp: bind failed: %s", strerror(errno));
    return false;
  }

  struct sockaddr_in actual;
  socklen_t length = sizeof actual;
  if (getsockname(socket_, (struct sockaddr*)&actual, &length) == 0) {
    boundPort_ = ntohs(actual.sin_port);
  }

  if (useMulticast) {
    struct ip_mreq request;
    std::memset(&request, 0, sizeof request);
    if (inet_pton(AF_INET, options_.multicastGroup.c_str(), &request.imr_multiaddr) != 1) {
      platform::Log::write(platform::LogLevel::ERROR, "udp: bad multicast group %s", options_.multicastGroup.c_str());
      return false;
    }
    request.imr_interface.s_addr = INADDR_ANY;

    if (setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof request) < 0) {
      platform::Log::write(platform::LogLevel::ERROR, "udp: cannot join %s: %s (this host may have no multicast route)",
        options_.multicastGroup.c_str(), strerror(errno));
      return false;
    }

    unsigned char loop = 1; // several nodes on one machine must hear each other
    setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    unsigned char ttl = 1; // stay on the local segment
    setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);

    destinations_.push_back(Endpoint { request.imr_multiaddr.s_addr, options_.multicastPort });
  }

  for (const std::string& peer : options_.peers) {
    const size_t colon = peer.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= peer.size()) {
      platform::Log::write(platform::LogLevel::ERROR, "udp: bad peer %s", peer.c_str());
      return false;
    }

    struct in_addr parsed;
    if (inet_pton(AF_INET, peer.substr(0, colon).c_str(), &parsed) != 1) {
      platform::Log::write(platform::LogLevel::ERROR, "udp: bad peer address %s", peer.c_str());
      return false;
    }
    destinations_.push_back(Endpoint { parsed.s_addr, (uint16_t)std::strtoul(peer.c_str() + colon + 1, nullptr, 10) });
  }

  // Receiving must never block the main loop.
  const int flags = fcntl(socket_, F_GETFL, 0);
  fcntl(socket_, F_SETFL, flags | O_NONBLOCK);

  platform::Log::write(
    platform::LogLevel::INFO, "udp: listening on port %u, %zu peers", boundPort_, destinations_.size());
  return true;
}

bool UdpRadio::send(ByteView frame, Priority priority)
{
  return queue_.push(frame, priority);
}

uint32_t UdpRadio::airtimeUs(size_t length) const
{
  return airtimeUsFor(params_, length);
}

bool UdpRadio::canTransmitNow() const
{
  return socket_ >= 0 && duty_.allows(now_, airtimeUs(MAX_PACKET_FRAME));
}

void UdpRadio::transmit(ByteView frame)
{
  uint8_t datagram[kDatagramMax];
  std::memcpy(datagram, &selfId_, kSenderIdSize);
  std::memcpy(datagram + kSenderIdSize, frame.data(), frame.size());

  for (const Endpoint& endpoint : destinations_) {
    const struct sockaddr_in destination = toSockaddr(endpoint.address, endpoint.port);
    sendto(
      socket_, datagram, kSenderIdSize + frame.size(), 0, (const struct sockaddr*)&destination, sizeof destination);
  }
}

void UdpRadio::drainSocket()
{
  uint8_t datagram[kDatagramMax];
  for (;;) {
    const ssize_t got = recv(socket_, datagram, sizeof datagram, 0);
    if (got < 0) {
      if (errno == EINTR) continue;
      return; // EAGAIN: nothing left
    }
    if ((size_t)got <= kSenderIdSize) continue;

    uint64_t sender = 0;
    std::memcpy(&sender, datagram, kSenderIdSize);
    if (sender == selfId_) continue; // our own transmission looped back

    if (sink_ == nullptr) continue;

    const size_t length = (size_t)got - kSenderIdSize;
    RxMeta meta;
    meta.at = now_;
    meta.airtimeUs = airtimeUs(length);
    meta.rssi = -60;
    meta.snr = 8;
    sink_->onFrame(ByteView { datagram + kSenderIdSize, length }, meta);
  }
}

void UdpRadio::tick(Millis now)
{
  now_ = now;
  if (socket_ < 0) return;

  drainSocket();

  std::vector<uint8_t> frame;
  while (queue_.pop(frame)) {
    if (frame.size() > MAX_PACKET_FRAME) continue; // cannot be a real frame

    const uint32_t airtime = airtimeUs(frame.size());
    if (!duty_.allows(now_, airtime)) {
      // Budget spent: hold it rather than break the rules.
      queue_.push(ByteView { frame.data(), frame.size() }, Priority::HIGH);
      return;
    }
    duty_.record(now_, airtime);
    transmit(ByteView { frame.data(), frame.size() });
  }
}
