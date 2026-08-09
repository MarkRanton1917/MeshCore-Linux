// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#pragma once

#include <cstdint>
#include <span>

#define MAX_PACKET_FRAME 255
#define MAX_PATH_SIZE 64
#define MAX_PACKET_PAYLOAD 184

// Hop count is 6 bits of the path_length byte; the hash size code is the other 2.
#define MAX_HOP_COUNT 63
#define MAX_HASH_SIZE 3
#define NODE_HASH_SIZE 1

// Packet layout.
#define PACKET_HEADER_SIZE 1
#define PACKET_PATH_LENGTH_SIZE 1
#define PACKET_TRANSPORT_CODES_SIZE 4
#define PACKET_TIMESTAMP_SIZE 4

// Key material and digests.
#define PACKET_HASH_SIZE 32
#define PACKET_PUBLIC_KEY_SIZE 32
#define PACKET_PRIVATE_KEY_SIZE 64
#define PACKET_MONT_KEY_SIZE 32
#define PACKET_SEED_SIZE 32
#define PACKET_SIGNATURE_SIZE 64
#define PACKET_SHARED_SECRET_SIZE 32

// Fixed by the protocol: changing either breaks interoperability.
#define PACKET_MAC_SIZE 2
#define PACKET_CIPHER_KEY_SIZE 16
#define PACKET_CIPHER_BLOCK_SIZE 16

#define ADVERT_PAYLOAD_SIZE (PACKET_PUBLIC_KEY_SIZE + PACKET_TIMESTAMP_SIZE + PACKET_SIGNATURE_SIZE)

// Payload prefixes ahead of the variable-length tail.
#define TEXT_FLAGS_SIZE 1
#define TEXT_MSG_PREFIX_SIZE (PACKET_TIMESTAMP_SIZE + TEXT_FLAGS_SIZE)
#define ANON_REQ_PREFIX_SIZE (NODE_HASH_SIZE + PACKET_PUBLIC_KEY_SIZE + PACKET_MAC_SIZE)
#define PATH_EXTRA_TYPE_SIZE 1
#define ENVELOPE_PREFIX_SIZE (NODE_HASH_SIZE * 2 + PACKET_MAC_SIZE)
#define TRACE_PREFIX_SIZE (4 + 4 + 1) // tag, auth code, flags
#define GROUP_PREFIX_SIZE (NODE_HASH_SIZE + PACKET_MAC_SIZE)
#define LOGIN_RESPONSE_NONCE_SIZE 4

// Advert appdata: low four bits of the flags byte are the node type, the high
// four say which optional fields follow.
#define ADVERT_TYPE_MASK 0x0F
#define ADVERT_HAS_LOCATION 0x10
#define ADVERT_HAS_FEATURE1 0x20
#define ADVERT_HAS_FEATURE2 0x40
#define ADVERT_HAS_NAME 0x80
#define ADVERT_LOCATION_SIZE 8
#define ADVERT_FEATURE_SIZE 2

// Identity store. Contacts are bounded so their storage never reallocates and
// the hash index can hold plain pointers.
#define MAX_CONTACTS 512
#define MAX_CONTACT_NAME 64
#define MAX_SECRET_CACHE 256

// Version 2 keeps what the radio heard — when, how well, how far — beside what
// the advert claimed. Version 1 files are read and upgraded; the fields they
// have no room for start empty and fill in with the next packet.
#define CONTACTS_FORMAT_VERSION 2
#define CONTACTS_FORMAT_MIN_VERSION 1

// Room. A noticeboard, not an archive: post 33 overwrites post 1.
#define MAX_POSTS 32
#define MAX_POST_TEXT 151
#define POST_AUTHOR_PREFIX 4
#define MAX_ROOM_CLIENTS 64
#define MAX_PASSWORD 32

// A pushed post reads "name: text". The name is capped so that the longest
// possible one plus the longest possible text still fits a single frame — the
// text is what somebody wrote, and truncating that to fit a long nickname would
// be the wrong way round.
#define MAX_POST_AUTHOR_NAME 16
#define MAX_PUSH_BODY 170

// Keys currently being made to wait after wrong passwords. Bounded like
// everything else here, and small: the table is a speed bump, not a ledger.
#define MAX_LOGIN_FAILURES 32
#define MAX_NODE_NAME 32

// Channels. A handful: each one costs a flood per post, and the airtime budget
// runs out long before the memory does.
#define MAX_CHANNELS 4
#define MAX_CHANNEL_NAME 32

// Version 2 gave posts a sequence number of their own; version 1 files are read
// and upgraded, their timestamp bookmarks converted on the way in.
#define ROOM_FORMAT_VERSION 2
#define ROOM_FORMAT_MIN_VERSION 1

// A reply has to fit one frame: 184 payload bytes, less the envelope, less what
// AES padding rounds up, less the text prefix. 160 leaves room to spare.
#define MAX_CLI_REPLY 160

// Repeater. Transit is rate limited per previous hop, and there are only so
// many of those: 256 node hashes, plus one bucket for what we heard straight
// from its author, whose hash the frame does not carry.
#define REPEATER_SOURCE_BUCKETS 257
#define REPEATER_DIRECT_BUCKET 256
#define REPEATER_RATE_WINDOW_MS 60000

// Login request plaintext: timestamp(4) syncSince(4) password(rest).
#define LOGIN_REQUEST_PREFIX_SIZE 8

// REQ plaintext: timestamp(4) type(1) argument(rest).
#define REQUEST_PREFIX_SIZE 5

using ByteView = std::span<const std::uint8_t>;
using ByteSpan = std::span<std::uint8_t>;
