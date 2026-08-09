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
#define CONTACTS_FORMAT_VERSION 1

// Room. A noticeboard, not an archive: post 33 overwrites post 1.
#define MAX_POSTS 32
#define MAX_POST_TEXT 151
#define POST_AUTHOR_PREFIX 4
#define MAX_ROOM_CLIENTS 64
#define MAX_PASSWORD 32
#define ROOM_FORMAT_VERSION 1

// Login request plaintext: timestamp(4) syncSince(4) password(rest).
#define LOGIN_REQUEST_PREFIX_SIZE 8

// txt_type values carried in the upper six bits of the text flags byte.
#define TEXT_TYPE_PLAIN 0
#define TEXT_TYPE_CLI 1
#define TEXT_TYPE_SIGNED 2

using ByteView = std::span<const std::uint8_t>;
using ByteSpan = std::span<std::uint8_t>;
