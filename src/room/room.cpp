// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#include "room.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace room;

namespace core = crypto::core;

// Login response body, ours to define — the protocol calls a RESPONSE payload
// application data:
//   serverTime(4) code(1) reserved(1) isAdmin(1) permissions(1)
//   random(4) firmwareVersion(2)
static constexpr size_t kResponseSize = 4 + 1 + 1 + 1 + 1 + 4 + 2;
static constexpr uint8_t kResponseOk = 0;

// Status reply to a REQ, laid out where sendStatusResponse builds it.
static constexpr size_t kStatusSize = 4 + 1 + 1 + 1 + 1 + 1 + 1 + 4 + 2;

static uint32_t readUint32(ByteView view, size_t offset)
{
  return (uint32_t)view[offset] | (uint32_t)view[offset + 1] << 8 | (uint32_t)view[offset + 2] << 16
    | (uint32_t)view[offset + 3] << 24;
}

static void writeUint32(std::vector<uint8_t>& out, uint32_t value)
{
  for (int i = 0; i < 4; i++)
    out.push_back((uint8_t)(value >> (8 * i)));
}

static bool samePublicKey(const PublicKey& a, const PublicKey& b)
{
  return std::memcmp(a.data.data(), b.data.data(), PACKET_PUBLIC_KEY_SIZE) == 0;
}

// Constant time: a password check that returns early leaks its prefix.
static bool passwordMatches(std::string_view candidate, const std::string& expected)
{
  if (expected.empty()) return false;
  return core::constantTimeEqual(ByteView { (const uint8_t*)candidate.data(), candidate.size() },
    ByteView { (const uint8_t*)expected.data(), expected.size() });
}

Room::Room(identity::Store& store, Sender& sender, Config config)
  : store_(store),
    sender_(sender),
    config_(config)
{
  posts_.reserve(MAX_POSTS);
  clients_.reserve(MAX_ROOM_CLIENTS);
}

void Room::setServerTime(uint32_t unixSeconds)
{
  serverTime_ = unixSeconds;
}

void Room::tick(Millis now)
{
  now_ = now;

  if (Client* client = nextClientToPush()) pushNextPost(*client);
}

// ------------------------------------------------------------------ rights

bool Room::can(const Client& client, Action action)
{
  switch (action) {
  case Action::READ:
    return client.access != Access::NONE;
  case Action::POST:
    return client.access == Access::GUEST || client.access == Access::ADMIN;
  case Action::COMMAND:
    return client.access == Access::ADMIN;
  }
  return false;
}

Client* Room::findClient(const PublicKey& pk)
{
  for (Client& client : clients_) {
    if (samePublicKey(client.pk, pk)) return &client;
  }
  return nullptr;
}

// ------------------------------------------------------------------ posts

uint32_t Room::sanitiseTimestamp(uint32_t claimed) const
{
  if (serverTime_ == 0) return claimed;

  const uint32_t low = serverTime_ > config_.clockWindow ? serverTime_ - config_.clockWindow : 0;
  const uint32_t high = serverTime_ + config_.clockWindow;
  return (claimed >= low && claimed <= high) ? claimed : serverTime_;
}

bool Room::addPost(const PublicKey& author, uint32_t timestamp, std::string_view text)
{
  Post post;
  post.seq = nextSeq_++;
  post.timestamp = sanitiseTimestamp(timestamp);
  std::memcpy(post.author.data(), author.data.data(), POST_AUTHOR_PREFIX);

  // Truncate quietly rather than reject: the client would never learn why.
  post.text.assign(text.substr(0, MAX_POST_TEXT));

  if (posts_.size() >= MAX_POSTS) posts_.erase(posts_.begin());
  posts_.push_back(std::move(post));

  if (config_.bus != nullptr) {
    config_.bus->publish(telemetry::EventType::PostAdded, serverTime_);
  }
  return true;
}

// ------------------------------------------------------------------ login

Client* Room::authenticate(const PublicKey& pk, std::string_view password, uint32_t timestamp)
{
  Client* client = findClient(pk);

  // First check of all: strictly newer than what we stored, or this is a
  // replayed login and the only guard against it is right here.
  if (client != nullptr && timestamp <= client->lastLogin) return nullptr;

  Access access = Access::NONE;
  if (passwordMatches(password, config_.adminPassword)) {
    access = Access::ADMIN;
  }
  else if (passwordMatches(password, config_.guestPassword)) {
    access = Access::GUEST;
  }
  else if (config_.allowAnonymousRead) {
    access = Access::READ_ONLY;
  }
  else {
    return nullptr;
  }

  if (client == nullptr) {
    if (clients_.size() >= MAX_ROOM_CLIENTS) clients_.erase(clients_.begin());
    clients_.push_back(Client {});
    client = &clients_.back();
    client->pk = pk;
  }
  client->access = access;
  client->lastLogin = timestamp;
  return client;
}

void Room::onAnon(const PublicKey& from, ByteView plain)
{
  if (plain.size() < LOGIN_REQUEST_PREFIX_SIZE) return;

  const uint32_t timestamp = readUint32(plain, 0);
  const uint32_t syncSince = readUint32(plain, 4);
  const std::string_view password((const char*)plain.data() + LOGIN_REQUEST_PREFIX_SIZE,
    std::min<size_t>(plain.size() - LOGIN_REQUEST_PREFIX_SIZE, MAX_PASSWORD));

  Client* client = authenticate(from, password, timestamp);
  if (client == nullptr) return; // replayed or refused, silently

  // The client's own bookmark, and we have to trust it: after reinstalling the
  // app it may legitimately ask for everything from zero.
  client->syncSeq = seqFromTimestamp(syncSince);

  if (config_.bus != nullptr) {
    config_.bus->publish(telemetry::EventType::ClientLogin, serverTime_);
  }
  sendLoginResponse(from, *client);
}

void Room::sendLoginResponse(const PublicKey& to, const Client& client)
{
  std::vector<uint8_t> body;
  body.reserve(kResponseSize);

  writeUint32(body, serverTime_);
  body.push_back(kResponseOk);
  body.push_back(0); // reserved
  body.push_back(client.access == Access::ADMIN ? 1 : 0);
  body.push_back((uint8_t)client.access);

  // Not for secrecy: without it two identical responses are the same packet,
  // deduplication somewhere in the network swallows the second, and the client
  // gets nothing at all.
  std::array<uint8_t, LOGIN_RESPONSE_NONCE_SIZE> nonce {};
  core::randomBytes(ByteSpan { nonce.data(), nonce.size() });
  body.insert(body.end(), nonce.begin(), nonce.end());

  body.push_back((uint8_t)(config_.firmwareVersion));
  body.push_back((uint8_t)(config_.firmwareVersion >> 8));

  sender_.sendDirect(to, packet::PayloadType::RESPONSE, ByteView { body.data(), body.size() }, false);
}

// ------------------------------------------------------------------ receive

void Room::onPayload(const identity::Contact& from, packet::PayloadType type, ByteView plain)
{
  switch (type) {
  case packet::PayloadType::TXT_MSG:
    handleText(from, plain);
    return;
  case packet::PayloadType::REQ: {
    Client* client = findClient(from.pk);
    if (client == nullptr) return; // never logged in, answered with silence
    handleRequest(*client, plain);
    return;
  }
  default:
    return;
  }
}

// REQ plaintext: timestamp(4) type(1) argument(rest).
//
// Answered with a RESPONSE and never acked — the response is the receipt, and
// on a duty-cycled radio two packets where one will do is a quarter of a second
// of airtime spent on nothing.
bool Room::handleRequest(Client& client, ByteView plain)
{
  if (plain.size() < REQUEST_PREFIX_SIZE) return false;

  const uint32_t timestamp = readUint32(plain, 0);
  const RequestType type = (RequestType)plain[4];

  // Same rule as commands, and a counter of its own: a keep-alive every minute
  // must not start refusing the commands typed between them.
  if (timestamp < client.lastRequest) return false;
  client.lastRequest = timestamp;

  if (!can(client, Action::READ)) return false;

  switch (type) {
  case RequestType::KEEP_ALIVE:
    // The client is awake now, so drop the pause a failed delivery imposed
    // rather than making it wait out a timer set for a node that was not there.
    client.retryAfter = 0;
    sendStatusResponse(client, type);
    return true;
  case RequestType::STATUS:
    sendStatusResponse(client, type);
    return true;
  default:
    return false;
  }
}

size_t Room::unreadFor(const Client& client) const
{
  size_t count = 0;
  for (const Post& post : posts_) {
    if (post.seq <= client.syncSeq) continue;
    if (std::memcmp(post.author.data(), client.pk.data.data(), POST_AUTHOR_PREFIX) == 0) continue;
    count++;
  }
  return count;
}

// Status body, ours to define:
//   serverTime(4) code(1) reqType(1) access(1) posts(1) clients(1) unread(1)
//   random(4) firmwareVersion(2)
//
// reqType sits where the login reply keeps a zero, so a client can tell the two
// apart from the bytes alone rather than from what it last sent.
void Room::sendStatusResponse(const Client& client, RequestType type)
{
  std::vector<uint8_t> body;
  body.reserve(kStatusSize);

  writeUint32(body, serverTime_);
  body.push_back(kResponseOk);
  body.push_back((uint8_t)type);
  body.push_back((uint8_t)client.access);
  body.push_back((uint8_t)std::min<size_t>(posts_.size(), 255));
  body.push_back((uint8_t)std::min<size_t>(clients_.size(), 255));
  body.push_back((uint8_t)std::min<size_t>(unreadFor(client), 255));

  // Same reason as the login reply: two identical answers are one packet on
  // air, and deduplication somewhere in the network swallows the second.
  std::array<uint8_t, LOGIN_RESPONSE_NONCE_SIZE> nonce {};
  core::randomBytes(ByteSpan { nonce.data(), nonce.size() });
  body.insert(body.end(), nonce.begin(), nonce.end());

  body.push_back((uint8_t)(config_.firmwareVersion));
  body.push_back((uint8_t)(config_.firmwareVersion >> 8));

  sender_.sendDirect(client.pk, packet::PayloadType::RESPONSE, ByteView { body.data(), body.size() }, false);
}

bool Room::handleText(const identity::Contact& from, ByteView plain)
{
  auto message = packet::decodeText(plain);
  if (!message.has_value()) return false;

  Client* client = findClient(from.pk);
  if (client == nullptr) return false; // never logged in

  switch ((TextType)message->txtType) {
  case TextType::PLAIN: {
    if (!can(*client, Action::POST)) return false;

    const std::string_view text((const char*)message->message.data(), message->message.size());
    addPost(from.pk, message->timestamp, text);
    return true;
  }
  case TextType::CLI: {
    const std::string_view line((const char*)message->message.data(), message->message.size());
    return handleCommand(*client, message->timestamp, line);
  }
  case TextType::SIGNED:
  default:
    // A room is not expected to receive these.
    return false;
  }
}

// A CLI command must not be acknowledged: its reply is the acknowledgement, and
// two of them would leave the client showing the command as answered twice. A
// post always is, whether we took it or refused it on rights — otherwise the
// client retries until it gives up and reports the message as undelivered.
bool Room::shouldAck(packet::PayloadType type, ByteView plain)
{
  // A REQ is answered with a RESPONSE, which is receipt enough.
  if (type == packet::PayloadType::REQ) return false;
  if (type != packet::PayloadType::TXT_MSG) return true;

  auto message = packet::decodeText(plain);
  if (!message.has_value()) return true;
  return (TextType)message->txtType != TextType::CLI;
}

bool Room::shouldForward(const packet::Packet& p)
{
  (void)p;
  return true;
}

// ------------------------------------------------------------------ commands

// Takes the leading word and advances the line past it. Whitespace-only leaves
// an empty word and an empty line, which every caller reads as "no argument".
static std::string_view takeWord(std::string_view& line)
{
  const size_t begin = line.find_first_not_of(" \t");
  if (begin == std::string_view::npos) {
    line = {};
    return {};
  }

  const size_t end = line.find_first_of(" \t", begin);
  if (end == std::string_view::npos) {
    const std::string_view word = line.substr(begin);
    line = {};
    return word;
  }

  const std::string_view word = line.substr(begin, end - begin);
  line = line.substr(end + 1);
  return word;
}

// Arguments that are the rest of the line — a name, a password — keep their
// inner spaces and lose the surrounding ones.
static std::string_view trimmed(std::string_view line)
{
  // Spelled out rather than written as a literal: the trailing NUL matters.
  // Clients that pad the message to a fixed length send it, and a password with
  // an invisible NUL on the end never matches again.
  static constexpr char kBlank[] = { ' ', '\t', '\r', '\n', '\0' };
  const std::string_view blank(kBlank, sizeof kBlank);

  const size_t begin = line.find_first_not_of(blank);
  if (begin == std::string_view::npos) return {};

  const size_t end = line.find_last_not_of(blank);
  return line.substr(begin, end - begin + 1);
}

// Hand-rolled: the input is a view off the air, not a C string, and a stray
// letter must fail rather than parse as far as it goes.
static bool parseUint32(std::string_view text, uint32_t& out)
{
  if (text.empty() || text.size() > 10) return false;

  uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') return false;
    value = value * 10 + (uint64_t)(digit - '0');
  }
  if (value > 0xFFFFFFFFull) return false;

  out = (uint32_t)value;
  return true;
}

// Days to a civil date, Howard Hinnant's algorithm. Here rather than in gmtime
// because that would be a call into the C library's own notion of the timezone,
// and this has to be UTC on every host.
static void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day)
{
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned dayOfEra = (unsigned)(days - era * 146097);
  const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const unsigned monthPrime = (5 * dayOfYear + 2) / 153;

  day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
  month = monthPrime + (monthPrime < 10 ? 3 : -9);
  year = (int)((int64_t)yearOfEra + era * 400 + (month <= 2 ? 1 : 0));
}

static void formatUtc(uint32_t epoch, char* out, size_t size)
{
  const int64_t days = (int64_t)(epoch / 86400);
  const uint32_t seconds = epoch % 86400;

  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civilFromDays(days, year, month, day);

  std::snprintf(
    out, size, "%02u:%02u:%02u %u/%u/%d UTC", seconds / 3600, (seconds / 60) % 60, seconds % 60, day, month, year);
}

void Room::sendCliReply(const PublicKey& to, std::string_view text)
{
  // Truncate rather than drop: a reply that does not fit is still worth more
  // than the silence the client would otherwise sit in.
  text = text.substr(0, MAX_CLI_REPLY);

  packet::TextMsg message;
  message.timestamp = serverTime_;
  message.txtType = (uint8_t)TextType::CLI;
  message.attempt = 0;
  message.message = ByteView { (const uint8_t*)text.data(), text.size() };

  std::vector<uint8_t> payload(MAX_PACKET_PAYLOAD);
  auto size = packet::encodeText(message, ByteSpan { payload.data(), payload.size() });
  if (!size.has_value()) return;

  // No ack asked for: this reply is the acknowledgement.
  sender_.sendDirect(to, packet::PayloadType::TXT_MSG, ByteView { payload.data(), *size }, false);
}

void Room::replyf(const PublicKey& to, const char* format, ...)
{
  char buffer[MAX_CLI_REPLY + 1];

  va_list args;
  va_start(args, format);
  const int written = std::vsnprintf(buffer, sizeof buffer, format, args);
  va_end(args);
  if (written < 0) return;

  sendCliReply(to, std::string_view(buffer, std::min((size_t)written, sizeof buffer - 1)));
}

bool Room::handleCommand(Client& client, uint32_t timestamp, std::string_view line)
{
  // A refusal answers too. A guest who typed a command and got nothing back
  // cannot tell that from a node which has stopped listening.
  if (!can(client, Action::COMMAND)) {
    sendCliReply(client.pk, "ERR - admin rights required");
    return false;
  }

  // Backwards in time means replayed, and a captured `reboot` is worth
  // replaying. Equal is allowed through: a client that saw no reply resends the
  // identical frame, and answering that again is the whole point.
  if (timestamp < client.lastCommand) return false;
  client.lastCommand = timestamp;

  // Trimmed once here so no verb or key downstream carries the padding some
  // clients put on the end of a message.
  std::string_view rest = trimmed(line);
  const std::string_view verb = takeWord(rest);

  if (verb == "help") {
    sendCliReply(client.pk, "clock, time <epoch>, ver, advert, set, get, clear posts, reboot");
    return true;
  }
  if (verb == "ver") {
    replyf(client.pk, "MeshCore room v%u", (unsigned)config_.firmwareVersion);
    return true;
  }
  if (verb == "clock" || verb == "time") {
    commandClock(client.pk, rest);
    return true;
  }
  if (verb == "advert") {
    if (config_.admin == nullptr) {
      sendCliReply(client.pk, "ERR - no radio attached");
      return false;
    }
    config_.admin->sendAdvert();
    sendCliReply(client.pk, "OK - advert sent");
    return true;
  }
  if (verb == "set") {
    commandSet(client.pk, rest);
    return true;
  }
  if (verb == "get") {
    commandGet(client.pk, rest);
    return true;
  }
  if (verb == "clear") {
    commandClear(client.pk, rest);
    return true;
  }
  if (verb == "reboot") {
    if (config_.admin == nullptr) {
      sendCliReply(client.pk, "ERR - not supported here");
      return false;
    }
    // Reply first. The host holds the stop back long enough for the queue to
    // drain, or the admin would never learn the command was taken.
    sendCliReply(client.pk, "OK - rebooting");
    config_.admin->requestReboot();
    return true;
  }

  replyf(client.pk, "ERR - unknown command '%.*s'", (int)std::min<size_t>(verb.size(), 24), verb.data());
  return false;
}

// No argument reports the clock, an argument sets it. Both spellings land here
// because half the clients send `clock` and half `time`.
void Room::commandClock(const PublicKey& to, std::string_view rest)
{
  const std::string_view argument = takeWord(rest);

  if (argument.empty()) {
    if (serverTime_ == 0) {
      sendCliReply(to, "ERR - clock not set");
      return;
    }
    char stamp[48];
    formatUtc(serverTime_, stamp, sizeof stamp);
    replyf(to, "%s (%u)", stamp, (unsigned)serverTime_);
    return;
  }

  uint32_t epoch = 0;
  if (!parseUint32(argument, epoch)) {
    sendCliReply(to, "ERR - expected a unix timestamp");
    return;
  }
  // The same floor the host applies at startup: a node dated 1970 corrupts its
  // neighbours' records, and taking that over the air would be a way to do it
  // on purpose.
  if (epoch <= 1577836800u) {
    sendCliReply(to, "ERR - timestamp before 2020, refused");
    return;
  }
  if (config_.admin == nullptr || !config_.admin->setClock(epoch)) {
    sendCliReply(to, "ERR - host refused the clock change");
    return;
  }

  setServerTime(epoch);
  char stamp[48];
  formatUtc(epoch, stamp, sizeof stamp);
  replyf(to, "OK - clock set to %s", stamp);
}

void Room::commandSet(const PublicKey& to, std::string_view rest)
{
  const std::string_view key = takeWord(rest);
  const std::string_view value = trimmed(rest);

  if (key == "name") {
    if (value.empty() || value.size() > MAX_NODE_NAME) {
      replyf(to, "ERR - name must be 1..%u characters", (unsigned)MAX_NODE_NAME);
      return;
    }
    if (config_.admin == nullptr || !config_.admin->setNodeName(value)) {
      sendCliReply(to, "ERR - host refused the name change");
      return;
    }
    // The network still holds the old name until something says otherwise.
    config_.admin->sendAdvert();
    replyf(to, "OK - name is now %.*s", (int)value.size(), value.data());
    return;
  }

  if (key == "password" || key == "guest.password") {
    if (value.size() > MAX_PASSWORD) {
      replyf(to, "ERR - password longer than %u characters", (unsigned)MAX_PASSWORD);
      return;
    }
    // Stored first, applied second. The other order would leave an admin told
    // the password had changed while the next restart brings the old one back.
    const bool forAdmin = key == "password";
    if (config_.admin == nullptr
      || !config_.admin->saveSetting(forAdmin ? "room.admin_password" : "room.guest_password", value)) {
      sendCliReply(to, "ERR - could not store the new password");
      return;
    }

    // Empty is a legitimate value: it closes that role off entirely.
    // Sessions already open keep the rights they were granted — their access is
    // stored per client and outlives the password that produced it.
    if (forAdmin) {
      config_.adminPassword.assign(value);
    }
    else {
      config_.guestPassword.assign(value);
    }
    // Never echoed back, not even a length.
    replyf(to, "OK - %s password %s", key == "password" ? "admin" : "guest", value.empty() ? "cleared" : "set");
    return;
  }

  if (key == "anonymous.read") {
    bool allow = false;
    if (value == "on" || value == "1" || value == "true") {
      allow = true;
    }
    else if (value == "off" || value == "0" || value == "false") {
      allow = false;
    }
    else {
      sendCliReply(to, "ERR - expected on or off");
      return;
    }

    // Stored in the config's own spelling, not the one the admin typed, so the
    // overlay shadows exactly the key it is named after.
    if (config_.admin == nullptr || !config_.admin->saveSetting("room.anonymous_read", allow ? "true" : "false")) {
      sendCliReply(to, "ERR - could not store the setting");
      return;
    }

    config_.allowAnonymousRead = allow;
    replyf(to, "OK - anonymous read %s", allow ? "on" : "off");
    return;
  }

  sendCliReply(to, "ERR - known keys: name, password, guest.password, anonymous.read");
}

void Room::commandGet(const PublicKey& to, std::string_view rest)
{
  const std::string_view key = takeWord(rest);

  if (key == "stats") {
    const uint32_t uptime = config_.admin != nullptr ? config_.admin->uptime() : 0;
    replyf(to, "posts %zu/%u, clients %zu/%u, up %uh%02um", posts_.size(), (unsigned)MAX_POSTS, clients_.size(),
      (unsigned)MAX_ROOM_CLIENTS, uptime / 3600, (uptime / 60) % 60);
    return;
  }
  if (key == "name") {
    const std::string name = config_.admin != nullptr ? config_.admin->nodeName() : std::string();
    replyf(to, "name %s", name.empty() ? "(unknown)" : name.c_str());
    return;
  }
  if (key == "time") {
    commandClock(to, {});
    return;
  }

  sendCliReply(to, "ERR - known keys: stats, name, time");
}

void Room::commandClear(const PublicKey& to, std::string_view rest)
{
  const std::string_view key = takeWord(rest);

  if (key == "posts") {
    const size_t removed = posts_.size();
    posts_.clear();
    // The clients keep their bookmarks. Rewinding them would replay whatever
    // arrives next from the beginning for everybody.
    replyf(to, "OK - %zu posts cleared", removed);
    return;
  }

  sendCliReply(to, "ERR - known keys: posts");
}

// ------------------------------------------------------------------ push

// A post counts as unsynced when its seq is past the client's bookmark and the
// client did not write it. Forgetting the second half looks like an echo to
// every user.
//
// By seq, never by timestamp: posts arrive in the order they are stored, so the
// scan is in order too, and no two of them can share a number.
const Post* Room::nextPostFor(const Client& client) const
{
  for (const Post& post : posts_) {
    if (post.seq <= client.syncSeq) continue;
    if (std::memcmp(post.author.data(), client.pk.data.data(), POST_AUTHOR_PREFIX) == 0) continue;
    return &post;
  }
  return nullptr;
}

uint32_t Room::seqFromTimestamp(uint32_t bookmark) const
{
  uint32_t seq = 0;
  for (const Post& post : posts_) {
    // Stop at the first one the client has not seen rather than carrying on:
    // client clocks are not monotonic between each other, so an older post can
    // sit behind a newer one, and skipping past it would lose it.
    if (post.timestamp > bookmark) break;
    seq = post.seq;
  }
  return seq;
}

Client* Room::nextClientToPush()
{
  if (clients_.empty()) return nullptr;

  for (size_t step = 0; step < clients_.size(); step++) {
    const size_t index = (nextClientIdx_ + step) % clients_.size();
    Client& client = clients_[index];

    if (client.pending) continue;
    if (client.retryAfter > now_) continue;
    if (!can(client, Action::READ)) continue;
    if (nextPostFor(client) == nullptr) continue;

    nextClientIdx_ = (index + 1) % clients_.size();
    return &client;
  }
  return nullptr;
}

void Room::pushNextPost(Client& client)
{
  const Post* post = nextPostFor(client);
  if (post == nullptr) return;

  // One post at a time per client. A 255-byte frame holds one anyway, and
  // parallel sends to one address jam the air and scramble the acks.
  packet::TextMsg message;
  message.timestamp = post->timestamp;
  message.txtType = (uint8_t)TextType::SIGNED;
  message.attempt = 0;

  std::vector<uint8_t> body;
  body.insert(body.end(), post->author.begin(), post->author.end());
  body.insert(body.end(), post->text.begin(), post->text.end());
  message.message = ByteView { body.data(), body.size() };

  std::vector<uint8_t> payload(MAX_PACKET_PAYLOAD);
  auto size = packet::encodeText(message, ByteSpan { payload.data(), payload.size() });
  if (!size.has_value()) return;

  client.pendingId =
    sender_.sendDirect(client.pk, packet::PayloadType::TXT_MSG, ByteView { payload.data(), *size }, true);
  client.pending = true;
  client.pendingSeq = post->seq;
}

// Only on acknowledgement. Moving the bookmark at send time means a lost packet
// loses that post for the client forever.
void Room::onAck(SendId id)
{
  for (Client& client : clients_) {
    if (!client.pending || client.pendingId != id) continue;

    client.syncSeq = client.pendingSeq;
    client.pending = false;
    client.retryAfter = 0;
    return;
  }
}

void Room::onDeliveryFailed(SendId id)
{
  for (Client& client : clients_) {
    if (!client.pending || client.pendingId != id) continue;

    // The bookmark stays put; the client gets this post on the next round,
    // after a pause so we stop hammering dead air.
    client.pending = false;
    client.retryAfter = now_ + config_.retryDelay;
    return;
  }
}

// ------------------------------------------------------------------ storage

//   byte 0        format version
//   version 2+:   nextSeq(4)
//   byte          post count
//   posts:        version 2+ seq(4), then timestamp(4) author(4) length(1) text
//   byte          client count
//   clients:      pk(32) access(1) bookmark(4) lastLogin(4)
//
// The bookmark is a seq from version 2 and was a timestamp in version 1; the
// reader converts. nextSeq is saved rather than recomputed because the posts it
// counted may all have been evicted from the ring, and starting over at 1 would
// leave every new post looking already delivered to a client holding 40.
//
// Settings an admin changed over the air are not here: they live in the config
// overlay, where a person can read them and delete them.

bool Room::writeState(const std::string& path) const
{
  std::vector<uint8_t> blob;
  blob.push_back(ROOM_FORMAT_VERSION);
  writeUint32(blob, nextSeq_);

  blob.push_back((uint8_t)posts_.size());
  for (const Post& post : posts_) {
    writeUint32(blob, post.seq);
    writeUint32(blob, post.timestamp);
    blob.insert(blob.end(), post.author.begin(), post.author.end());
    blob.push_back((uint8_t)post.text.size());
    blob.insert(blob.end(), post.text.begin(), post.text.end());
  }

  blob.push_back((uint8_t)clients_.size());
  for (const Client& client : clients_) {
    blob.insert(blob.end(), client.pk.data.begin(), client.pk.data.end());
    blob.push_back((uint8_t)client.access);
    writeUint32(blob, client.syncSeq);
    writeUint32(blob, client.lastLogin);
  }

  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return false;

  size_t written = 0;
  while (written < blob.size()) {
    const ssize_t chunk = write(fd, blob.data() + written, blob.size() - written);
    if (chunk < 0 && errno == EINTR) continue;
    if (chunk < 0) {
      close(fd);
      return false;
    }
    written += (size_t)chunk;
  }
  if (fsync(fd) != 0) {
    close(fd);
    return false;
  }
  return close(fd) == 0;
}

bool Room::readState(const std::string& path)
{
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;

  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    close(fd);
    return false;
  }

  std::vector<uint8_t> blob((size_t)info.st_size);
  size_t filled = 0;
  while (filled < blob.size()) {
    const ssize_t got = read(fd, blob.data() + filled, blob.size() - filled);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) {
      close(fd);
      return false;
    }
    filled += (size_t)got;
  }
  close(fd);

  if (blob.size() < 2) return false;

  const uint8_t version = blob[0];
  if (version < ROOM_FORMAT_MIN_VERSION || version > ROOM_FORMAT_VERSION) return false;

  const ByteView view { blob.data(), blob.size() };
  size_t offset = 1;

  // Version 1 knew no sequence numbers. Its posts get one each, in the order
  // they were stored, which is the order they arrived in.
  uint32_t nextSeq = 1;
  if (version >= 2) {
    if (view.size() - offset < 4) return false;
    nextSeq = readUint32(view, offset);
    offset += 4;
  }

  std::vector<Post> posts;
  if (view.size() - offset < 1) return false;
  const uint8_t postCount = view[offset++];
  if (postCount > MAX_POSTS) return false;
  for (uint8_t i = 0; i < postCount; i++) {
    if (view.size() - offset < (version >= 2 ? 4u : 0u) + 4 + POST_AUTHOR_PREFIX + 1) return false;

    Post post;
    if (version >= 2) {
      post.seq = readUint32(view, offset);
      offset += 4;
    }
    else {
      post.seq = nextSeq++;
    }
    post.timestamp = readUint32(view, offset);
    offset += 4;
    std::memcpy(post.author.data(), view.data() + offset, POST_AUTHOR_PREFIX);
    offset += POST_AUTHOR_PREFIX;

    const uint8_t length = view[offset++];
    if (length > MAX_POST_TEXT || view.size() - offset < length) return false;
    post.text.assign((const char*)view.data() + offset, length);
    offset += length;
    posts.push_back(std::move(post));
  }

  if (view.size() - offset < 1) return false;
  std::vector<Client> clients;
  const uint8_t clientCount = view[offset++];
  if (clientCount > MAX_ROOM_CLIENTS) return false;
  for (uint8_t i = 0; i < clientCount; i++) {
    if (view.size() - offset < PACKET_PUBLIC_KEY_SIZE + 1 + 4 + 4) return false;

    Client client;
    std::memcpy(client.pk.data.data(), view.data() + offset, PACKET_PUBLIC_KEY_SIZE);
    offset += PACKET_PUBLIC_KEY_SIZE;
    client.access = (Access)view[offset++];

    // A seq in version 2, a timestamp in version 1. Converted below, once the
    // posts it has to be measured against are in place.
    client.syncSeq = readUint32(view, offset);
    offset += 4;
    client.lastLogin = readUint32(view, offset);
    offset += 4;
    clients.push_back(std::move(client));
  }

  // Never behind the posts on disk, whatever the header claimed: a nextSeq that
  // repeats a number already in the ring would hand out the same bookmark twice.
  for (const Post& post : posts) {
    if (post.seq >= nextSeq) nextSeq = post.seq + 1;
  }

  posts_ = std::move(posts);
  clients_ = std::move(clients);
  nextSeq_ = nextSeq;
  posts_.reserve(MAX_POSTS);
  clients_.reserve(MAX_ROOM_CLIENTS);
  nextClientIdx_ = 0;

  if (version < 2) {
    for (Client& client : clients_) {
      client.syncSeq = seqFromTimestamp(client.syncSeq);
    }
  }
  return true;
}

bool Room::load(const std::string& dir)
{
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
  dir_ = dir;

  const std::string path = dir + "/room.dat";
  if (access(path.c_str(), F_OK) != 0) return true; // nothing saved yet
  return readState(path);
}

bool Room::flush()
{
  if (dir_.empty()) return false;

  const std::string target = dir_ + "/room.dat";
  const std::string temporary = target + ".tmp";

  if (!writeState(temporary)) {
    unlink(temporary.c_str());
    return false;
  }
  if (rename(temporary.c_str(), target.c_str()) != 0) {
    unlink(temporary.c_str());
    return false;
  }
  return true;
}
