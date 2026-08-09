#include "room.h"

#include <cerrno>
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
  client->syncSince = syncSince;

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
  if (type != packet::PayloadType::TXT_MSG) return;
  handleText(from, plain);
}

bool Room::handleText(const identity::Contact& from, ByteView plain)
{
  auto message = packet::decodeText(plain);
  if (!message.has_value()) return false;

  Client* client = findClient(from.pk);
  if (client == nullptr) return false; // never logged in

  switch (message->txtType) {
  case TEXT_TYPE_PLAIN: {
    if (!can(*client, Action::POST)) return false;

    const std::string_view text((const char*)message->message.data(), message->message.size());
    addPost(from.pk, message->timestamp, text);
    return true;
  }
  case TEXT_TYPE_CLI:
    // Admin only, and handled by a separate parser.
    return can(*client, Action::COMMAND);
  case TEXT_TYPE_SIGNED:
  default:
    // A room is not expected to receive these.
    return false;
  }
}

// A CLI command must not be acknowledged; a post always is, whether we took it
// or refused it on rights — otherwise the client retries until it gives up and
// then reports the message as undelivered.
bool Room::shouldAck(packet::PayloadType type, ByteView plain)
{
  if (type != packet::PayloadType::TXT_MSG) return true;

  auto message = packet::decodeText(plain);
  if (!message.has_value()) return true;
  return message->txtType != TEXT_TYPE_CLI;
}

bool Room::shouldForward(const packet::Packet& p)
{
  (void)p;
  return true;
}

// ------------------------------------------------------------------ push

// A post counts as unsynced when it is newer than the client's bookmark and the
// client did not write it. Forgetting the second half looks like an echo to
// every user.
const Post* Room::nextPostFor(const Client& client) const
{
  for (const Post& post : posts_) {
    if (post.timestamp <= client.syncSince) continue;
    if (std::memcmp(post.author.data(), client.pk.data.data(), POST_AUTHOR_PREFIX) == 0) continue;
    return &post;
  }
  return nullptr;
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
  message.txtType = TEXT_TYPE_SIGNED;
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
  client.pendingPost = post->timestamp;
}

// Only on acknowledgement. Moving the bookmark at send time means a lost packet
// loses that post for the client forever.
void Room::onAck(SendId id)
{
  for (Client& client : clients_) {
    if (!client.pending || client.pendingId != id) continue;

    client.syncSince = client.pendingPost;
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
//   byte 1        post count
//   posts:        timestamp(4) author(4) length(1) text
//   byte          client count
//   clients:      pk(32) access(1) syncSince(4) lastLogin(4)

bool Room::writeState(const std::string& path) const
{
  std::vector<uint8_t> blob;
  blob.push_back(ROOM_FORMAT_VERSION);

  blob.push_back((uint8_t)posts_.size());
  for (const Post& post : posts_) {
    writeUint32(blob, post.timestamp);
    blob.insert(blob.end(), post.author.begin(), post.author.end());
    blob.push_back((uint8_t)post.text.size());
    blob.insert(blob.end(), post.text.begin(), post.text.end());
  }

  blob.push_back((uint8_t)clients_.size());
  for (const Client& client : clients_) {
    blob.insert(blob.end(), client.pk.data.begin(), client.pk.data.end());
    blob.push_back((uint8_t)client.access);
    writeUint32(blob, client.syncSince);
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

  if (blob.size() < 2 || blob[0] != ROOM_FORMAT_VERSION) return false;

  const ByteView view { blob.data(), blob.size() };
  size_t offset = 1;

  std::vector<Post> posts;
  const uint8_t postCount = view[offset++];
  if (postCount > MAX_POSTS) return false;
  for (uint8_t i = 0; i < postCount; i++) {
    if (view.size() - offset < 4 + POST_AUTHOR_PREFIX + 1) return false;

    Post post;
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
    client.syncSince = readUint32(view, offset);
    offset += 4;
    client.lastLogin = readUint32(view, offset);
    offset += 4;
    clients.push_back(std::move(client));
  }

  posts_ = std::move(posts);
  clients_ = std::move(clients);
  posts_.reserve(MAX_POSTS);
  clients_.reserve(MAX_ROOM_CLIENTS);
  nextClientIdx_ = 0;
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
