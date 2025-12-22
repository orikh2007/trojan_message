//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_NODE_H
#define TROJAN_MESSAGE_NODE_H
#include "msg.h"
#include "networkSettings.h"
#include "apiComm.h"

constexpr int ROOT_PORT = 12345;

using udp = asio::ip::udp;
using MsgType = proto::MsgType;


bool operator==(proto::MsgType msg, char* str);

class Node : public std::enable_shared_from_this<Node> {
public:

    explicit Node(const uint16_t listen_port);

    void start();

    asio::io_context& io();

    void become_root();

    const std::string& id();

    void handle_command(const std::string& line);

    void handle_send(std::istringstream& iss);

    void handle_register();

    void handle_register_ack(const std::string& tx,
        const peerInfo& curP, const int want);


private:
    void send_text(const udp::endpoint& target, const std::string& text);

    void start_receive();

    void on_receive(const std::error_code& ec, std::size_t n);

    void process_receive(udp::endpoint& from, const std::string &msg);

    void process_msg(udp::endpoint& from, const proto::Envelope& env);

    void process_register(const udp::endpoint& from, const proto::Envelope& env);

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

};

#endif //TROJAN_MESSAGE_NODE_H