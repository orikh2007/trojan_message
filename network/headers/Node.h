//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_NODE_H
#define TROJAN_MESSAGE_NODE_H
#include "msg.h"
#include "networkSettings.h"
#include "apiComm.h"


constexpr int MAX_CONNS = 4;

constexpr int ROOT_PORT = 12345;

using udp = asio::ip::udp;
using MsgType = proto::MsgType;

struct Connection {
    udp::endpoint ep;
    std::string punch_token;
    int tries = 0;
    bool connected = false;
    asio::steady_timer timer;
    asio::steady_timer ka_timer;

    Connection(asio::io_context& io, udp::endpoint endp, std::string tkn)
        : ep(std::move(endp)), punch_token(std::move(tkn)), timer(io), ka_timer(io) {}
};

struct TokenEntry {
    std::string peer_id;
    std::chrono::steady_clock::time_point expires;
};


bool operator==(proto::MsgType msg, char* str);

class Node : public std::enable_shared_from_this<Node> {
public:

    explicit Node(const uint16_t listen_port);

    void start();

    asio::io_context& io();

    void become_root();

    const std::string& id();

    void handle_command(const std::string& line);


private:
    void send_text(const udp::endpoint &target, std::string text);

    void start_receive();

    void on_receive(const std::error_code& ec, std::size_t n);

    void process_receive(udp::endpoint& from, const std::string &msg);

    void process_msg(udp::endpoint& from, const proto::Envelope& env);

    void on_register(const udp::endpoint& from, const proto::Envelope& env);

    void on_register_ack(const udp::endpoint& from, const proto::Envelope& env);

    void remember_token(const std::string &peer_id, const std::string &token, int ttl_ms);

    void prune_tokens();

    void on_introduce(const udp::endpoint &from, const proto::Envelope &env);

    void mark_connected(std::string tkn, const std::string &node_id, udp::endpoint ep);

    bool token_is_known(std::string tkn, udp::endpoint from, std::string n_id);

    void on_punch(const udp::endpoint& from, const proto::Envelope& env);

    void on_punch_ack(const udp::endpoint &from, const proto::Envelope &env);

    void handle_send(std::istringstream& iss);

    void handle_register();

    void handle_register_ack(const std::string& tx,
        const peerInfo& curP, const int want);

    void start_punch(peerInfo p, int timeout, int punch_ms, std::string tkn);

    void punch(const std::string& peerId, int timeout, int punch_ms);



    std::string ip_;
    uint16_t port_;
    udp::endpoint ep_;

    bool is_root_ = false;
    std::string root_ip_;
    std::string node_id_;
    asio::io_context io_;
    udp::socket socket_;
    udp::endpoint remote_;
    std::array<char, 2048> recv_buf_{};

    using anyV = std::variant<udp::endpoint, std::string, std::chrono::steady_clock::time_point>;
    std::map<std::string, peerInfo> clients_map_;
    std::vector<std::string> clients_;
    std::unordered_map<std::string, std::unique_ptr<Connection>> connections_;

    std::unordered_map<std::string, TokenEntry> token_cache_; // key=token


};

#endif //TROJAN_MESSAGE_NODE_H