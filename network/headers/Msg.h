//
// Created by Ori Kedar Haspel on 20/12/2025.
//
#ifndef TROJAN_MESSAGE_MSG_H
#define TROJAN_MESSAGE_MSG_H

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Clock = std::chrono::steady_clock;

struct peerInfo {
    udp::endpoint ep;
    Clock::time_point last_seen;
    std::string peerId;
};

namespace proto {
using json = nlohmann::json;

// ---------------------------
// Constants / Types
// ---------------------------
inline constexpr int PROTO_VERSION = 0xb00b;

enum class MsgType {
    REGISTER,
    REGISTER_ACK,
    KEEPALIVE,
    PEER_LIST,
    CONNECT_REQUEST,
    INTRODUCE,
    PUNCH,
    PUNCH_ACK,
    DATA,
    ERROR_
};

inline std::string to_string(const MsgType t) {
    switch (t) {
        case MsgType::REGISTER:         return "REGISTER";
        case MsgType::REGISTER_ACK:     return "REGISTER_ACK";
        case MsgType::KEEPALIVE:        return "KEEPALIVE";
        case MsgType::PEER_LIST:        return "PEER_LIST";
        case MsgType::CONNECT_REQUEST:  return "CONNECT_REQUEST";
        case MsgType::INTRODUCE:        return "INTRODUCE";
        case MsgType::PUNCH:            return "PUNCH";
        case MsgType::PUNCH_ACK:        return "PUNCH_ACK";
        case MsgType::DATA:             return "DATA";
        case MsgType::ERROR_:           return "ERROR";
    }
    return "ERROR";
}

inline std::optional<MsgType> parse_type(std::string_view s) {
    if (s == "REGISTER") return MsgType::REGISTER;
    if (s == "REGISTER_ACK") return MsgType::REGISTER_ACK;
    if (s == "KEEPALIVE") return MsgType::KEEPALIVE;
    if (s == "PEER_LIST") return MsgType::PEER_LIST;
    if (s == "CONNECT_REQUEST") return MsgType::CONNECT_REQUEST;
    if (s == "INTRODUCE") return MsgType::INTRODUCE;
    if (s == "PUNCH") return MsgType::PUNCH;
    if (s == "PUNCH_ACK") return MsgType::PUNCH_ACK;
    if (s == "DATA") return MsgType::DATA;
    if (s == "ERROR") return MsgType::ERROR_;
    return std::nullopt;
}

// ---------------------------
// Small utilities
// ---------------------------

inline bool is_hex(std::string_view s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

// Node id: 64-bit hex string (16 chars)
inline std::string random_node_id_hex() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t x = gen();
    static const char* hex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[i] = hex[x & 0xF];
        x >>= 4;
    }
    return out;
}

// Token: 64-bit hex string (16 chars)
inline std::string random_token_hex() { return random_node_id_hex(); }

// Transaction id: keep it simple for iteration 1.
// If you want real UUIDs, swap this with a UUID generator later.
inline std::string random_tx_id() {
    // 32 hex chars
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 32; ++i) out.push_back(hex[dist(gen)]);
    return out;
}

// ---------------------------
// Base64 (minimal, sufficient)
// ---------------------------

inline std::string b64_encode(const std::vector<uint8_t>& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= in.size()) {
        uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | uint32_t(in[i + 2]);
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);
        out.push_back(tbl[n & 63]);
        i += 3;
    }

    const size_t rem = in.size() - i;
    if (rem == 1) {
        uint32_t n = (uint32_t(in[i]) << 16);
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);
        out.push_back('=');
    }

    return out;
}

inline std::optional<std::vector<uint8_t>> b64_decode(std::string_view s) {
    auto val = [](char c) -> int {
        if ('A' <= c && c <= 'Z') return c - 'A';
        if ('a' <= c && c <= 'z') return c - 'a' + 26;
        if ('0' <= c && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        if (c == '=') return -2; // padding
        return -1;
    };

    if (s.size() % 4 != 0) return std::nullopt;

    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);

    for (size_t i = 0; i < s.size(); i += 4) {
        int a = val(s[i]);
        int b = val(s[i + 1]);
        int c = val(s[i + 2]);
        int d = val(s[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1) return std::nullopt;

        uint32_t n = (uint32_t(a) << 18) | (uint32_t(b) << 12);
        if (c >= 0) n |= (uint32_t(c) << 6);
        if (d >= 0) n |= uint32_t(d);

        out.push_back(uint8_t((n >> 16) & 0xFF));
        if (c != -2) out.push_back(uint8_t((n >> 8) & 0xFF));
        if (d != -2) out.push_back(uint8_t(n & 0xFF));
    }

    return out;
}

// ---------------------------
// Validation helpers
// ---------------------------

struct Envelope {
    int v = PROTO_VERSION;
    MsgType type{};
    std::string tx;
    std::string src;
    int64_t ts = 0;
    json body;
};

inline void require(bool cond, std::string_view msg) {
    if (!cond) throw std::runtime_error(std::string("protocol error: ") + std::string(msg));
}

inline Envelope parse_envelope(const std::string& text) {
    json j = json::parse(text);

    require(j.is_object(), "root must be object");
    require(j.contains("v") && j["v"].is_number_integer(), "missing/int v");
    require(j.contains("type") && j["type"].is_string(), "missing/string type");
    require(j.contains("tx") && j["tx"].is_string(), "missing/string tx");
    require(j.contains("src") && j["src"].is_string(), "missing/string src");
    require(j.contains("body") && j["body"].is_object(), "missing/object body");

    Envelope e;
    e.v = j["v"].get<int>();
    require(e.v == PROTO_VERSION, "unsupported version");

    const auto tstr = j["type"].get<std::string>();
    auto mt = parse_type(tstr);
    require(mt.has_value(), "unknown type");
    e.type = *mt;

    e.tx = j["tx"].get<std::string>();
    e.src = j["src"].get<std::string>();
    e.ts = j.value("ts", int64_t{0});
    e.body = j["body"];

    // Basic sanity for node IDs (skip for ROOT)
    if (e.src != "ROOT") {
        require(e.src.size() == 16 && is_hex(e.src), "src must be 16 hex chars");
    }

    return e;
}

inline json make_envelope(MsgType type, std::string src, std::string tx, json body, int64_t ts = 0) {
    json j;
    j["v"] = PROTO_VERSION;
    j["type"] = to_string(type);
    j["tx"] = std::move(tx);
    j["src"] = std::move(src);
    if (ts != 0) j["ts"] = ts;
    j["body"] = std::move(body);
    return j;
}

inline std::string dump_compact(const json& j) {
    return j.dump(-1); // compact JSON
}

// ---------------------------
// Message builders
// ---------------------------

inline json msg_register(const std::string& node_id, uint16_t listen_port, int want_peers = 4) {
    json body;
    body["listen_port"] = listen_port;
    body["want_peers"] = want_peers;
    return make_envelope(MsgType::REGISTER, node_id, random_tx_id(), body);
}

inline json msg_register_ack(const std::string& tx,
                             const udp::endpoint you,
                             const std::vector<peerInfo>& peers,
                             const std::string& token_hex,
                             int ka_sec = 15, int punch_ms = 250, int timeout_ms = 4000) {
    require(token_hex.size() == 16 && is_hex(token_hex), "token must be 16 hex chars");
    json body;
    std::string you_ip = you.address().to_string();
    uint16_t you_port = you.port();
    vector<json> peersJ;
    for (auto peer : peers) {
        json peerJ;
        peerJ["id"] = peer.peerId;
        peerJ["ip"] = peer.ep.address().to_string();
        peerJ["port"] = peer.ep.port();
        peersJ.push_back(peerJ);
    }

    body["you"] = {{"ip", you_ip}, {"port", you_port}};
    body["peers"] = peersJ; // each peer: {"id","ip","port"}
    body["ka_sec"] = ka_sec;
    body["token"] = token_hex;
    body["punch_ms"] = punch_ms;
    body["timeout_ms"] = timeout_ms;

    return make_envelope(MsgType::REGISTER_ACK, "ROOT", tx, body);
}

inline json msg_keepalive(const std::string& node_id) {
    json body = json::object();
    return make_envelope(MsgType::KEEPALIVE, node_id, random_tx_id(), body);
}

// inline json msg_connect_request(const std::string& node_id, const std::string& target_id) {
    // require(target_id.size() == 16 && is_hex(target_id), "target_id must be 16 hex chars");
    // json body;
    // body["target_id"] = target_id;
    // return make_envelope(MsgType::CONNECT_REQUEST, node_id, random_tx_id(), body);
// } //connect request - used reg_ack in instead (easier to understand)

inline json msg_introduce(const std::string& tx,
                          const peerInfo& peer,
                          const std::string& token_hex,
                          int ka_sec = 15, int punch_ms = 250, int timeout_ms = 4000) {
    require(peer.peerId.size() == 16 && is_hex(peer.peerId), "peer.id must be 16 hex chars");
    require(token_hex.size() == 16 && is_hex(token_hex), "token must be 16 hex chars");

    json body;
    body["peer"] = {{"id", peer.peerId}, {"ip", peer.ep.address().to_string()}, {"port", peer.ep.port()}};
    body["ka_sec"] = ka_sec;
    body["token"] = token_hex;
    body["punch_ms"] = punch_ms;
    body["timeout_ms"] = timeout_ms;

    return make_envelope(MsgType::INTRODUCE, "ROOT", random_tx_id(), body);
}

inline json msg_punch(const std::string& node_id, const std::string& token_hex) {
    require(token_hex.size() == 16 && is_hex(token_hex), "token must be 16 hex chars");
    json body;
    body["token"] = token_hex;
    return make_envelope(MsgType::PUNCH, node_id, random_tx_id(), body);
}

inline json msg_punch_ack(const std::string& node_id, const std::string& token_hex) {
    require(token_hex.size() == 16 && is_hex(token_hex), "token must be 16 hex chars");
    json body;
    body["token"] = token_hex;
    return make_envelope(MsgType::PUNCH_ACK, node_id, random_tx_id(), body);
}

inline json msg_data_b64(const std::string& node_id, const std::string& to_id,
                         uint32_t seq, const std::vector<uint8_t>& payload_bytes) {
    require(to_id.size() == 16 && is_hex(to_id), "to must be 16 hex chars");

    json body;
    body["to"] = to_id;
    body["seq"] = seq;
    body["payload"] = b64_encode(payload_bytes);
    body["enc"] = "b64";

    return make_envelope(MsgType::DATA, node_id, random_tx_id(), body);
}

inline json msg_error(const std::string& tx, std::string code, std::string detail) {
    json body;
    body["code"] = std::move(code);
    body["detail"] = std::move(detail);
    return make_envelope(MsgType::ERROR_, "ROOT", tx, body);
}

// ---------------------------
// Typed extractors (safe reads)
// ---------------------------

inline uint16_t get_u16(const json& j, const char* key) {
    require(j.contains(key) && j[key].is_number_integer(), std::string("missing/int ") + key);
    int x = j[key].get<int>();
    require(0 <= x && x <= 65535, std::string("out of range ") + key);
    return static_cast<uint16_t>(x);
}

inline std::string get_str(const json& j, const char* key) {
    require(j.contains(key) && j[key].is_string(), std::string("missing/string ") + key);
    return j[key].get<std::string>();
}

// For DATA body
inline std::optional<std::vector<uint8_t>> data_payload_bytes(const Envelope& e) {
    if (e.type != MsgType::DATA) return std::nullopt;
    const auto enc = e.body.value("enc", "");
    if (enc != "b64") return std::nullopt;
    auto p = e.body.value("payload", "");
    if (p.empty()) return std::nullopt;
    return b64_decode(p);
}

} // namespace proto


#endif //TROJAN_MESSAGE_MSG_H