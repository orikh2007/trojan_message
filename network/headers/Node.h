//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_NODE_H
#define TROJAN_MESSAGE_NODE_H
#include "msg.h"
#include "Asio.h"
#include "apiComm.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <map>

constexpr int ROOT_PORT = 12345;

using udp = asio::ip::udp;
using MsgType = proto::MsgType;


bool operator==(proto::MsgType msg, char* str);

class Node : public enable_shared_from_this<Node> {
public:
    // int level;
    // int64_t nodeId;
    // int64_t parentId;
    // asio::ip::udp::endpoint parentEp;
    // vector<asio::ip::udp::endpoint> children;
    // map<asio::ip::udp::endpoint, int64_t> conns;

    explicit Node(const uint16_t listen_port)
        :   ip_(getIP(v4)),
            port_(listen_port),
            ep_(udp::endpoint(asio::ip::make_address(ip_), port_)),
            is_root_(),
            root_ip_(),
            node_id_(),
            io_(),
            socket_(io_),
            clients_map_(),
            clients_()
    {
        std::error_code ec;

        node_id_ = proto::random_node_id_hex();

        std::cout << "Node ID: " << node_id_ << "\n";

        socket_.open(udp::v4(), ec);
        if (ec) {
            io_.stop();
            throw std::runtime_error("socket_.open failed: " + ec.message());
        }

        socket_.bind(udp::endpoint(udp::v4(), listen_port), ec);
        if (ec) {
            io_.stop();
            throw std::runtime_error("socket_.bind failed (port in use?): " + ec.message());
        }
        std::cout << "Listening on UDP IPv4 port " << listen_port << "\n";
    }

    void start() { start_receive(); }

    asio::io_context& io() { return io_; }

    void become_root() {
        is_root_ = true;
        setRoot(ip_);

        port_ = ROOT_PORT;
        const auto addr = asio::ip::make_address(ip_);
        ep_ = udp::endpoint(addr, port_);

        peerInfo me;
        me.peerId = node_id_;
        me.ep = ep_;
        me.last_seen = Clock::now();
        clients_.push_back(me.peerId);
        clients_map_[me.peerId] = me;
    }

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
            } else if (cmd == "register") {
                handle_register();
            } else if (cmd == "root") {
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
            std::cout << "Bad IP address: " << tgt_ip << " (" << ec.message() << ")\n";
            return;
        }

        udp::endpoint ep(addr, static_cast<uint16_t>(port_int));
        send_text(ep, msg);
        std::cout << "Sent (async) to " << tgt_ip << ":" << port_int << "\n";
    }

    void handle_register() {
        const auto j = proto::msg_register(node_id_, socket_.local_endpoint().port(), 1);
        const auto data = proto::dump_compact(j);
        std::error_code ec;
        root_ip_ = getDDNS();
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

    void handle_register_ack(const std::string& tx, const peerInfo& curP, const int want) {
        const size_t size = clients_.size();
        int n = min(static_cast<int>(size), 4);
        n = min(n, want);
        vector<peerInfo> peers;

        std::random_device rd;
        std::mt19937 gen {rd()};
        ranges::shuffle(clients_, gen);

        for (int i = 0; i < n; ++i) {
            std::string cli = clients_.at(i);
            if (cli == curP.peerId)
                continue;
            peers.push_back(clients_map_[cli]);
        }

        const string token = proto::random_token_hex();

        const json jNew = proto::msg_register_ack(tx, curP.ep, peers, token); //reg_ack json for newcomer
        const udp::endpoint registeringCli = curP.ep;
        const auto dataNew = proto::dump_compact(jNew); //reg_ack data for newcomer
        send_text(registeringCli, dataNew);
        std::cout << "Sent reg_ack to " << registeringCli.address().to_string() << ":" << registeringCli.port() << endl;

        for (auto membr : peers){
            const json jMembr = proto::msg_introduce(tx, curP, token); //introduce json for existing
            auto epMembr = membr.ep;
            const auto dataMembr = proto::dump_compact(jMembr);
            send_text(epMembr, dataMembr);
            std::cout << "Sent introduce msg to " << epMembr.address().to_string() << ":" << epMembr.port() << endl;
        }

    }


private:
    void send_text(const udp::endpoint& target, const std::string& text) {
        auto data = make_shared<std::string>(text);
        socket_.async_send_to(
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
        socket_.async_receive_from(
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
        std::cout << "received message from " << remote_ << endl;
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
            const proto::Envelope env = proto::parse_envelope(msg);

            process_msg(from, env);
        } catch (const std::exception& e) {
            std::cerr << "Bad message from "
                      << from.address().to_string() << ":" << from.port()
                      << " : " << e.what() << "\n";
        }
    }

    void process_msg(udp::endpoint& from, const proto::Envelope& env) {
        std::cout << "processing message" << endl;
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
            default:
                std::cerr << "Message type cannot be processed: " << static_cast<int>(env.type) << endl;
        }
    }

    void process_register(const udp::endpoint& from, const proto::Envelope& env) {
        if (!is_root_) {
            std::cerr << "I'M GROOT (NOT ROOT, DONT REGISTER HERE!)";
            return;
        }
        try {
            peerInfo p;
            if (env.body["listen_port"] != from.port())
                std::cerr << "register listen port is different from sending port" << endl;
            p.ep = from;
            p.last_seen = Clock::now();
            p.peerId = env.src;
            clients_map_[env.src] = p;
            clients_.push_back(p.peerId);

            int want = env.body["want_peers"];

            std::string tx = env.tx;

            std::cout   << "peer " << env.src << " registered\n";
            std::cout   << "here's his info: " << clients_map_[p.peerId].ep.address() << ":" << clients_map_[env.src].ep.port() << endl;

            handle_register_ack(tx, p, want);
        } catch (const std::exception& e) {
            std::cerr << "Bad register message from " << from.address().to_string() << ": " << e.what();
        }
    };

    string ip_;
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
    map<std::string, peerInfo> clients_map_;
    vector<std::string> clients_;

};

#endif //TROJAN_MESSAGE_NODE_H