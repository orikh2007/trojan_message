//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_NODE_H
#define TROJAN_MESSAGE_NODE_H

#endif //TROJAN_MESSAGE_NODE_H
#include "Asio.h"
#include "apiComm.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <map>

constexpr int ROOT_PORT = 12345;

using udp = asio::ip::udp;
using Clock = std::chrono::steady_clock;
using MsgType = proto::MsgType;


bool operator==(proto::MsgType msg, char* str);

struct peerInfo {
    udp::endpoint ep;
    Clock::time_point last_seen;
};

class Node : public enable_shared_from_this<Node> {
public:
    const string ip = getIP(v4);
    // int level;
    // int64_t nodeId;
    // int64_t parentId;
    // asio::ip::udp::endpoint parentEp;
    // vector<asio::ip::udp::endpoint> children;
    // map<asio::ip::udp::endpoint, int64_t> conns;

    explicit Node(const uint16_t listen_port, const uint16_t send_port)
        :   io_(),
            recv_socket_(io_),
            send_socket_(io_),
            node_id_(),
            root_ip_(),
            is_root_(),
            clients_()
    {
        std::error_code ec;

        node_id_ = proto::random_node_id_hex();

        root_ip_ = getDDNS();

        std::cout << "Node ID: " << node_id_ << "\n";

        recv_socket_.open(udp::v4(), ec);
        if (ec) {
            io_.stop();
            throw std::runtime_error("socket_.open failed: " + ec.message());
        }

        recv_socket_.bind(udp::endpoint(udp::v4(), listen_port), ec);
        if (ec) {
            io_.stop();
            throw std::runtime_error("socket_.bind failed (port in use?): " + ec.message());
        }
        send_socket_.open(udp::v4(), ec);
        if (ec) {
            io_.stop();
            throw std::runtime_error("socket_.open failed: " + ec.message());
        }

        send_socket_.bind(udp::endpoint(udp::v4(), send_port), ec);
        if (ec) {
            io_.stop();
            throw std::runtime_error("socket_.bind failed (port in use?): " + ec.message());
        }

        std::cout << "Listening on UDP IPv4 port " << listen_port << "\n";
    }

    void start() { start_receive(); }

    asio::io_context& io() { return io_; }

    void become_root(){ is_root_ = true; }

    const std::string& id() {return node_id_;}

    void handle_command(const std::string& line) {
        // Example commands:
        // send 1.2.3.4 7777 hello
        // quit
        try {
            if (line == "quit") {
                std::cout << "Stopping...\n";
                io_.stop();
                return;
            }

            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;

            if (cmd == "send") {
                handle_send(iss);
            } else if (line == "register") {
                handle_register();
            } else if (line == "root") {
                become_root();
            } else {
                std::cout   << "Commands:\n"
                            << "  send <ip> <port> <message>\n"
                            << "  register\n"
                            << "  root\n"
                            << "  quit\n";
            }
        } catch (const std::exception& e) {
            io_.stop();
            // Never let exceptions escape into asio handlers
            std::cerr << "handle_command exception: " << e.what() << "\n";
        }

    }
    void handle_send(istringstream& iss) {
        std::string tgt_ip;
        int port_int = 0;
        iss >> tgt_ip >> port_int;

        if (tgt_ip.empty() || port_int <= 0 || port_int > 65535) {
            std::cout << "Usage: send <ip> <port> <message>\n";
            return;
        }

        std::string msg;
        std::getline(iss, msg);
        if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);

        std::error_code ec;
        auto addr = asio::ip::make_address(tgt_ip, ec);
        if (ec) {
            io_.stop();
            std::cout << "Bad IP address: " << tgt_ip << " (" << ec.message() << ")\n";
            return;
        }

        udp::endpoint ep(addr, static_cast<uint16_t>(port_int));
        send_text(ep, msg);
        std::cout << "Sent (async) to " << tgt_ip << ":" << port_int << "\n";
    }

    void handle_register() {
        const auto j = proto::msg_register(node_id_, recv_socket_.local_endpoint().port(), 1);
        const auto data = proto::dump_compact(j);
        std::error_code ec;
        const auto addr = asio::ip::make_address(root_ip_, ec);
        if (ec) {
            io_.stop();
            std::cout << "Bad IP address: " << root_ip_ << " (" << ec.message() << ")\n";
            return;
        }
        const udp::endpoint ep(addr, ROOT_PORT);
        send_text(ep, data);
        std::cout << "Sent (async) to " << root_ip_ << ":" << ROOT_PORT << "\n";
    }


private:
    void send_text(const udp::endpoint& target, const std::string& text) {
        auto data = make_shared<std::string>(text);
        send_socket_.async_send_to(
            asio::buffer(*data),
            target,
            [self = shared_from_this(), data](const std::error_code& ec, std::size_t bytes) {
                if (ec){
                    self->io_.stop();
                    std::cerr << "send error: " << ec.message() << "\n";
                }
                else {
                    std::cout << "sent " << bytes << "\n";
                }
        });
    }

    void start_receive() {
        recv_socket_.async_receive_from(
            asio::buffer(recv_buf_),
            remote_,
            [self = shared_from_this()](std::error_code ec, std::size_t n) {
                self->on_receive(ec, n);
            }
        );
    }

    void on_receive(const std::error_code& ec, std::size_t n) {
        if (ec) {
            std::cerr << "recv error: " << ec.message() << "\n";
            if (!io_.stopped()) start_receive();
            return;
        }

        // Copy data NOW (recv_buf_ will be reused next receive)
        std::string msg(recv_buf_.data(), recv_buf_.data() + n);
        auto from = remote_; // copy endpoint

        // Re-arm receive ASAP
        start_receive();

        // Dispatch processing on the same io thread
        asio::post(io_, [self = shared_from_this(), from, msg = std::move(msg)]() mutable {
            self->process_receive(from, msg);
        });
    }

    void process_receive(udp::endpoint& from, const std::string &msg) {
        try {
            proto::Envelope env = proto::parse_envelope(msg);
            process_msg(from, env);
        } catch (const std::exception& e) {
            std::cerr << "Bad message from "
                      << from.address().to_string() << ":" << from.port()
                      << " : " << e.what() << "\n";
        }
    }

    void process_msg(udp::endpoint& from, proto::Envelope env) {
        switch (env.type) {
            case MsgType::REGISTER:
                process_register(from, env);
                break;
            case MsgType::REGISTER_ACK:
                break;
            case MsgType::KEEPALIVE:
                break;
            case MsgType::PEER_LIST:
                break;
            case MsgType::CONNECT_REQUEST:
                break;
            case MsgType::INTRODUCE:
                break;
            case MsgType::PUNCH:
                break;
            case MsgType::PUNCH_ACK:
                break;
            case MsgType::DATA:
                break;
            case MsgType::ERROR_:
                break;
        }
    }

    void process_register(const udp::endpoint& from, proto::Envelope env) {
        if (!is_root_) {
            std::cerr << "I'M GROOT (NOT ROOT, DONT REGISTER HERE!)";
            return;
        }
        try {
            map<std::string, anyV> client;
            peerInfo p;
            p.ep = from;
            p.last_seen = Clock::now();
            clients_[env.src] = p;

            std::cout << "peer " << env.src << " registered\n";
            std::cout << "here's his info: " << clients_[env.src].ep.address() << ":" << clients_[env.src].ep.port() << endl;
        } catch (const std::exception& e) {
            std::cerr << "Bad register message from " << from.address().to_string();
        }
    };


    bool is_root_;
    std::string root_ip_;
    std::string node_id_;
    asio::io_context io_;
    udp::socket recv_socket_;
    udp::socket send_socket_;
    udp::endpoint remote_;
    std::array<char, 2048> recv_buf_{};

    using anyV = std::variant<udp::endpoint, std::string, std::chrono::steady_clock::time_point>;
    map<std::string, peerInfo> clients_;

};
