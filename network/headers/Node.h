//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_NODE_H
#define TROJAN_MESSAGE_NODE_H
#include "msg.h"
#include "networkSettings.h"
#include "apiComm.h"
#include <queue>
constexpr uint32_t attempt_budget = 200;
constexpr auto expiration_time_sec = std::chrono::seconds(45);
constexpr auto prune_sec = std::chrono::seconds(10);

constexpr int MAX_CONNS = 3;
constexpr int MIN_CONNS = 1;
constexpr int ROOT_PORT = 12345;

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

enum class CircuitState { PUNCHING_ENTRY, EXTENDING, READY, FAILED };

struct Circuit {
    std::string circuit_id;
    std::vector<NodeId> path;   // [relay1, relay2, ..., dst]  (excludes src)
    int hops_confirmed = 0;     // how many CIRCUIT_EXTENDED confirmations received
    CircuitState state = CircuitState::PUNCHING_ENTRY;
};


bool operator==(proto::MsgType msg, char* str);

inline int random_0_to_n(int n) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, n - 1);
    return dis(gen);
}

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

    void on_linkup(const udp::endpoint &from, const proto::Envelope &env);

    void on_linkdown(const udp::endpoint &from, const proto::Envelope &env);

    void on_keepalive(const udp::endpoint &from, const proto::Envelope &env);

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

    void on_node_list_req(const udp::endpoint &from, const proto::Envelope &env);  // root
    void on_node_list_resp(const udp::endpoint &from, const proto::Envelope &env); // src
    void on_introduce_req(const udp::endpoint &from, const proto::Envelope &env);  // root

    void on_circuit_extend(const udp::endpoint &from, const proto::Envelope &env);   // relay
    void on_circuit_extended(const udp::endpoint &from, const proto::Envelope &env); // src + relay

    void request_introduce(const NodeId &target_id);  // send INTRODUCE_REQ to root (or handle locally if we are root)

    void begin_circuit_build(const NodeId &dst, int num_relays = 2);
    void send_next_extend(Circuit &c);
    void send_via_circuit(const std::string &circuit_id, const std::string &data);

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



// -------------------------- Attributes --------------------------
    std::string ip_;
    uint16_t port_;
    udp::endpoint ep_;

    bool is_root_ = false;
    std::string root_ip_;
    udp::endpoint root_ep_;
    NodeId node_id_;
    asio::io_context io_;
    udp::socket socket_;
    udp::endpoint remote_;
    bool recv_;
    std::array<char, 2048> recv_buf_{};

    NodeId daddy_; // parent node

    int level_;

    int cur_connections_;

    asio::steady_timer prune_timer_;

    std::unordered_set<NodeId> linked_up_;

    std::map<std::string, PeerInfo> clients_map_;
    std::vector<std::string> clients_;
    std::unordered_map<std::string, std::unique_ptr<Connection>> connections_;

    std::unordered_map<std::string, TokenEntry> token_cache_; // key=token

    // onion routing — source side
    std::unordered_map<std::string, Circuit> circuits_;           // circuit_id → Circuit
    std::unordered_map<NodeId, std::string>  circuit_by_dst_;    // dst_id → circuit_id
    std::vector<PeerInfo> node_list_candidates_;                  // from NODE_LIST_RESP
    NodeId pending_circuit_dst_;                                  // dst waiting for NODE_LIST_RESP

    // onion routing — relay side
    std::unordered_map<std::string, NodeId> circuit_hop_table_;        // circuit_id → prev_hop_id
    std::unordered_map<NodeId, std::string> pending_extend_circuits_;  // next_peer_id → circuit_id


};

#endif //TROJAN_MESSAGE_NODE_H