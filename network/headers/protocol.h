//
// Created by orikh on 12/12/2025.
//

#ifndef TROJAN_MESSAGE_PROTOCOL_H
#define TROJAN_MESSAGE_PROTOCOL_H
#include "networkSettings.h"

#pragma pack(push, 1)
struct Header {
    uint32_t magic = 0x626f6f62; //check the hex meaning
    uint16_t version = 1;
    uint8_t  type;
    uint8_t  flags;
    uint32_t session_id;
    uint32_t seq;
    uint32_t ack;
    uint32_t payload_len;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 24, "header must be 24 bytes");

enum MsgType : uint8_t {
    //ipv6 entry channel
    BOOT_HELLO = 1,
    BOOT_PEER_QUERY = 2,
    BOOT_PEER_INFO = 3,

    //ipv4 comm channel
    PUNCH = 10,
    PUNCH_ACK = 11,
    DATA = 12,
    PING = 13,
    PONG = 14
};

//Inline funcs that allow consistent networking between different machines.
static inline uint16_t to_be16(uint16_t x) { return htons(x); } //converts 16 byte data to big endian
static inline uint32_t to_be32(uint32_t x) { return htonl(x); } //converts 32 byte data to big endian
static inline uint16_t from_be16(uint16_t x) { return ntohs(x); } //converts 16 byte data back from big endian to host calibration (host dependent)
static inline uint32_t from_be32(uint32_t x) { return ntohl(x); } //converts 32 byte data back from big endian to host calibration (host dependent)

//Inline funcs that allow writing and reading correctly into/from buffer.
static inline void write_u32(std::vector<uint8_t>& out, uint32_t v_be) {
    uint8_t b[4];
    std::memcpy(b, &v_be, 4);
    out.insert(out.end(), b, b + 4);
}
static inline void write_u16(std::vector<uint8_t>& out, uint16_t v_be) {
    uint8_t b[2];
    std::memcpy(b, &v_be, 2);
    out.insert(out.end(), b, b + 2);
}
static inline bool read_u32(const uint8_t*& p, const uint8_t* end, uint32_t& v_be) {
    if (end - p < 4) return false;
    std::memcpy(&v_be, p, 4);
    p += 4;
    return true;
}
static inline bool read_u16(const uint8_t*& p, const uint8_t* end, uint16_t& v_be) {
    if (end - p < 2) return false;
    std::memcpy(&v_be, p, 2);
    p += 2;
    return true;
}

struct EndpointHash {
    std::size_t operator()(udp::endpoint const& e) const noexcept {
        auto a = e.address().to_string();
        std::size_t h1 = std::hash<std::string>{}(a);
        std::size_t h2 = std::hash<unsigned short>{}(e.port());
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1<<6) + (h1>>2));
    }
};

struct EndpointEq {
    bool operator()(udp::endpoint const& a, udp::endpoint const& b) const noexcept {
        return a.address() == b.address() && a.port() == b.port();
    }
};
#endif //TROJAN_MESSAGE_PROTOCOL_H
