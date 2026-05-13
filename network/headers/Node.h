//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_NODE_H
#define TROJAN_MESSAGE_NODE_H
#include "Msg.h"
#include "networkSettings.h"
#include "apiComm.h"
#include "log.h"
#include <queue>
#include <deque>
#include <mutex>
#include <fstream>
constexpr uint32_t attempt_budget = 200;
constexpr auto expiration_time_sec = std::chrono::seconds(45);
constexpr auto prune_sec = std::chrono::seconds(10);
constexpr auto circuit_init_timeout_sec = std::chrono::seconds(10);
constexpr auto circuit_dead_sec         = std::chrono::seconds(30);
constexpr uint32_t MAX_NACK_RETRIES     = 5;
constexpr auto outgoing_transfer_timeout_sec = std::chrono::seconds(60);

constexpr int MAX_CIRCUIT_BUILD_ATTEMPTS = 5;
constexpr int MAX_CONNS = 4;
constexpr int MIN_CONNS = 1;
constexpr int ROOT_PORT = 12345;
constexpr int MIN_RELAYS = 1;
constexpr int MAX_RELAYS = 4;
constexpr int MIN_GRAPH_SIZE = 2; // minimum nodes in local graph to attempt circuit building
constexpr size_t CHUNK_SIZE = 900;

using udp = asio::ip::udp;
using MsgType = proto::MsgType;



struct Connection {
    int level;
    udp::endpoint ep;
    std::string punch_token;
    int tries = 0;
    bool connected = false;
    int ka_ms = 15000;
    Clock::time_point last_seen = Clock::now();
    asio::steady_timer timer;
    asio::steady_timer ka_timer;

    Connection(asio::io_context& io, udp::endpoint endP, std::string tkn)
        : level(-1), ep(std::move(endP)), punch_token(std::move(tkn)), timer(io), ka_timer(io) {}
};

struct TokenEntry {
    std::string peer_id;
    std::chrono::steady_clock::time_point expires;
};

enum class CircuitState { READY, FAILED, INITIATING };

struct Circuit {
    std::string circuit_id;
    std::vector<NodeId> path;       // [relay1, relay2, ..., dst]  (excludes src)
    // path_cids[i] = the CID used on the link *into* path[i].
    // path_cids[0] == circuit_id (src→relay1), path_cids[1] = relay1→relay2, etc.
    std::vector<std::string> path_cids;
    CircuitState state = CircuitState::READY;
    Clock::time_point last_used{};
    Clock::time_point last_ack{};
};

// Relay-side state for one direction of a circuit link.
// Keyed by in_cid (the CID we received from the upstream hop).
struct RelayHop {
    NodeId prev_hop;      // node that sent us packets on in_cid (for sending replies back)
    std::string out_cid;  // CID we use when forwarding to the next hop
};

struct ReceivedMsg {
    uint32_t chunk_num; //total number of chunks
    ContentType content_type;
    NodeId src;
    uint32_t nack_retries = 0;
    std::unordered_map<uint32_t, std::vector<uint8_t>> chunks;
};

struct OutgoingTransfer {
    NodeId dst;
    ContentType content_type;
    uint32_t total_chunks;
    std::vector<std::vector<uint8_t>> chunks; // chunks[i] = raw bytes of chunk i
};

bool operator==(proto::MsgType msg, char* str);

inline int random_0_to_n(int n) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, n - 1);
    return dis(gen);
}

struct CircuitInfo
{
    std::string circuit_id;
    NodeId dst;
    CircuitState state;
};

struct NodeSnapshot
{
    NodeId node_id;
    int level{-1};
    bool is_root{false};
    NodeId daddy;
    std::vector<NodeId> linked_peers;
    std::vector<NodeId> known_peers;
    std::vector<CircuitInfo> circuits;
};

struct ChatMessage
{
    std::string                           from;
    std::string                           to;
    std::string                           text;
    std::chrono::system_clock::time_point when;
    bool                                  conn_req{false};
};

class Node : public std::enable_shared_from_this<Node> {
public:

    explicit Node(uint16_t listen_port);

    void start();

    asio::io_context& io();

    void start_pruner();

    void become_root();

    void rebind();

    const std::string& id();

    void handle_command(const std::string& line);

    void handle_link(std::istringstream &iss);

    //GUI

    NodeSnapshot             get_snapshot() const;

    std::vector<ChatMessage> take_new_messages();

private:
    void send_text(const udp::endpoint &target, std::string text);

    void start_receive();

    void on_receive(const std::error_code& ec, std::size_t n);

    void process_receive(udp::endpoint& from, const std::string &msg);

    void dispatch(udp::endpoint& from, const proto::Envelope& env);

    void on_register(const udp::endpoint& from, const proto::Envelope& env);

    void on_req_conns(const udp::endpoint &from, const proto::Envelope &env);

    void on_register_ack(const udp::endpoint& from, const proto::Envelope& env);

    void on_req_conns_ack(const udp::endpoint &from, const proto::Envelope &env);

    void remember_token(const std::string &peer_id, const std::string &token, int ttl_ms);

    void prune_tokens();

    void on_introduce(const udp::endpoint &from, const proto::Envelope &env);

    void mark_connected(std::string tkn, const std::string &node_id, udp::endpoint ep, int level);

    bool token_is_known(std::string tkn, udp::endpoint from, const std::string &n_id);

    void on_punch(const udp::endpoint& from, const proto::Envelope& env);

    void on_punch_ack(const udp::endpoint &from, const proto::Envelope &env);

    void choose_parent();

    void on_data(const udp::endpoint &from, const proto::Envelope &env);

    void on_disconnect(const udp::endpoint &from, const proto::Envelope &env);

    void handle_send(std::istringstream& iss);

    void handle_register();

    void handle_dis();

    void handle_register_ack(const std::string& tx,
                             const PeerInfo& curP, const int want);

    void send_up(proto::Envelope env);

    void request_conns();

    void broadcast(const std::string &msg);

    void start_punch(const PeerInfo &p, int timeout, int punch_ms, const std::string &tkn);

    void punch(const std::string& peerId, int timeout, int punch_ms);

    void send_chunked(const NodeId &dst, std::vector<uint8_t> data, ContentType content_type);

    void on_linkup(const udp::endpoint &from, const proto::Envelope &env);

    void on_linkdown(const udp::endpoint &from, const proto::Envelope &env);

    void on_keepalive(const udp::endpoint &from, const proto::Envelope &env);

    void on_chunk(const udp::endpoint &from, const proto::Envelope &env);

    void on_chunk_ack(const udp::endpoint &from, const proto::Envelope &env);

    void on_chunk_nack(const udp::endpoint &from, const proto::Envelope &env);

    void link_up(const NodeId &peer);

    void link_down(const NodeId &peer);

    void keep_alive(const std::string &peerId);

    void prune_tick();

    void prune_connections();

    void prune_dead();

    void print_graph();

    void rand_disconnect();

    void dynamic_disconnect(const NodeId& id);

    void remove_connection(const std::string &peerId);

// ------------------------ onion routing ------------------------

    void on_hop(const udp::endpoint &from, const proto::Envelope &env);
    void on_hop_reply(const udp::endpoint &from, const proto::Envelope &env);
    void on_circuit_broken(const udp::endpoint &from, const proto::Envelope &env);

    void on_introduce_req(const udp::endpoint &from, const proto::Envelope &env);  // root
    void on_graph_update(const udp::endpoint &from, const proto::Envelope &env);
    void broadcast_graph_update(const NodeId &node);  // root only
    void send_graph_snapshot(const udp::endpoint &to); // root only

    void request_introduce(const NodeId &target_id);  // send INTRODUCE_REQ to root (or handle locally if we are root)

    void begin_circuit_build(const NodeId &dst);

    void invalidate_circuits_through(const NodeId& peer_id);

    void send_via_circuit(const std::string &circuit_id, const std::string &data);

    void send_reply_via_circuit(const std::string &circuit_id, const std::string &data);

    void cancel_circuit_timer(const std::string &circuit_id);

    void cleanup_relay_circuit(const std::string &circuit_id);

    std::vector<EVP_PKEY*> gen_path_keys(size_t hop_num);

    std::vector<std::array<uint8_t, 32>> get_path_pubkeys(const std::vector<EVP_PKEY*>& kps);

// ------------------------ routing logic ------------------------

    std::optional<std::vector<NodeId>> bfs_shortest_path(
    const NodeId& src,
    const NodeId& dst,
    const std::unordered_set<NodeId>& forbidden) const;

    bool try_inject_detour(std::vector<NodeId>& path, size_t edge_i) const;

    std::optional<std::vector<NodeId>> compute_route(
        const NodeId& src,
        const NodeId& dst,
        int min_hops) const;

// -------------------------- GUI --------------------------

    void update_snapshot();

// -------------------------- heir election --------------------------

    void designate_heir();
    void take_over_as_root();
    void on_you_are_heir(const udp::endpoint& from, const proto::Envelope& env);
    void on_new_root(const udp::endpoint& from, const proto::Envelope& env);

// -------------------------- Attributes --------------------------
    std::string ip_;
    uint16_t port_;
    udp::endpoint ep_;

    bool   is_root_     = false;
    bool   is_heir_     = false;
    NodeId heired_from_;
    NodeId heir_id_;
    bool has_heir_{false};
    std::string root_ip_;
    udp::endpoint root_ep_;
    NodeId node_id_;
    asio::io_context io_;
    udp::socket socket_;
    udp::endpoint remote_;
    bool recv_;
    std::array<char, 16384> recv_buf_{};

    NodeId daddy_; // parent node

    int level_;

    int cur_connections_;

    asio::steady_timer prune_timer_;
    asio::steady_timer register_timer_;
    int register_retries_ = 0;

    std::unordered_set<NodeId> linked_up_;

    std::map<std::string, PeerInfo> clients_map_;
    std::unordered_map<std::string, std::unique_ptr<Connection>> connections_;

    std::unordered_map<std::string, TokenEntry> token_cache_; // key=token

    std::unordered_map<NodeId, uint64_t> graph_seq_;  // dedup: node_id → last seq seen
    std::unordered_map<NodeId, uint64_t> graph_seq_out_; // root only: per-node broadcast counter

    // onion routing — source side
    std::unordered_map<std::string, Circuit> circuits_;           // circuit_id → Circuit
    std::unordered_map<NodeId, std::string>  circuit_by_dst_;    // dst_id → circuit_id

    // onion routing — relay side
    // in_cid → {prev_hop, out_cid}: the bidirectional CID-switching table.
    // Each relay stores two entries per circuit: in_cid→out_cid (forward) and out_cid→in_cid (reverse).
    std::unordered_map<std::string, RelayHop> circuit_hop_table_;   // in_cid  → RelayHop
    std::unordered_map<std::string, std::string> circuit_reply_table_; // out_cid → in_cid

    // onion routing — reply side (populated when a circuit probe is received)
    std::unordered_map<NodeId, std::string> received_circuit_by_src_;  // src_node_id → circuit_id

    std::unordered_map<std::string, ReceivedMsg> incoming_msgs_;
    std::unordered_map<std::string, OutgoingTransfer> outgoing_transfers_; // transfer_id → transfer
    std::unordered_map<std::string, std::unique_ptr<asio::steady_timer>> chunk_timers_;            // transfer_id → nack retry timer
    std::unordered_map<std::string, std::unique_ptr<asio::steady_timer>> outgoing_transfer_timers_; // transfer_id → sender timeout
    std::unordered_map<std::string, std::unique_ptr<asio::steady_timer>> circuit_timers_;           // circuit_id  → init timeout
    std::unordered_map<NodeId, int> circuit_build_attempts_;  // dst → consecutive failed build count

    //CRYPTO - key maps
    std::unordered_map<std::string, std::vector<std::array<uint8_t, 32>>> circuit_keys_; // src: [k_relay1, k_relay2, k_dst] — relay: [k_i] (just one)
    std::unordered_map<std::string, std::vector<EVP_PKEY*>> circuit_pending_kp_; // src: [e1_kp, e2_kp, e3_kp] — relay: [r_i_kp] (just one, until probe_ack passes through)

    //GUI
    mutable std::mutex      gui_mutex_;
    NodeSnapshot            snapshot_;
    std::mutex              chat_mutex_;
    std::deque<ChatMessage> chat_log_;
};

#endif //TROJAN_MESSAGE_NODE_H