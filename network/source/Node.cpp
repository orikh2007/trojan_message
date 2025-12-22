//
// Created by orikh on 22/12/2025.
//
#include "../headers/Node.h"

Node::Node(const uint16_t listen_port)
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
} //constructor

void Node::start() { start_receive(); } //start the receiving io "loop"

asio::io_context& Node::io() { return io_; } //get io context

void Node::become_root() {
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
    clients_map_.insert_or_assign(me.peerId, me);
} //become the root - tell dynu and everyone you're the root

const std::string& Node::id() {return node_id_;} //get node id

void Node::handle_command(const std::string& line) {
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

} //handle the cli command - temporary

void Node::handle_send(std::istringstream& iss) {
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
} //send someone a message - temporary

void Node::handle_register() {
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
} //register with the root - send it node id and open port

void Node::handle_register_ack(const std::string& tx, const peerInfo& curP, const int want) {
    const size_t size = clients_.size();
    int n = std::min(static_cast<int>(size), 4);
    n = std::min(n, want);
    std::vector<peerInfo> peers;

    std::random_device rd;
    std::mt19937 gen {rd()};
    std::ranges::shuffle(clients_, gen);

    for (int i = 0; i < n; ++i) {
        std::string cli = clients_.at(i);
        if (cli == curP.peerId)
            continue;
        peers.push_back(clients_map_.at(cli));
    }

    const std::string token = proto::random_token_hex();

    const json jNew = proto::msg_register_ack(tx, curP.ep, peers, token); //reg_ack json for newcomer
    const udp::endpoint registeringCli = curP.ep;
    const auto dataNew = proto::dump_compact(jNew); //reg_ack data for newcomer
    send_text(registeringCli, dataNew);
    std::cout << "Sent reg_ack to " << registeringCli.address().to_string() << ":" << registeringCli.port() << std::endl;

    for (auto membr : peers){
        const json jMembr = proto::msg_introduce(tx, curP, token); //introduce json for existing
        auto epMembr = membr.ep;
        const auto dataMembr = proto::dump_compact(jMembr);
        send_text(epMembr, dataMembr);
        std::cout << "Sent introduce msg to " << epMembr.address().to_string() << ":" << epMembr.port() << std::endl;
    }

} /* for root - handles register_ack -
                                                                                                  * sends registering client his peers and token,
                                                                                                  * and tells the peers to connect to the registering client
                                                                                                  */

void Node::send_text(const udp::endpoint& target, const std::string& text) {
    auto data = make_shared<std::string>(std::move(text));
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
} //sends string text to target.

void Node::start_receive() {
    socket_.async_receive_from(
        asio::buffer(recv_buf_),
        remote_,
        [self = shared_from_this()](std::error_code ec, std::size_t n) {
            self->on_receive(ec, n);
        }
    );
} //async receiver

void Node::on_receive(const std::error_code& ec, std::size_t n) {
    if (ec) {
        std::cerr << "recv error: " << ec.message() << "\n";
        if (!io_.stopped()) start_receive();
        return;
    }
    std::cout << "received message from " << remote_ << std::endl;
    // Copy data NOW (recv_buf_ will be reused next receive)
    std::string msg(recv_buf_.data(), recv_buf_.data() + n);
    auto from = remote_; // copy endpoint

    // Re-arm receive ASAP
    start_receive();

    // Dispatch processing on the same io thread
    asio::post(io_, [self = shared_from_this(), from, msg = std::move(msg)]() mutable {
        self->process_receive(from, msg);
    });
} //handles newly received messages - rearms start_receive sends it to process_receive

void Node::process_receive(udp::endpoint& from, const std::string &msg) {
    try {
        const proto::Envelope env = proto::parse_envelope(msg);

        process_msg(from, env);
    } catch (const std::exception& e) {
        std::cerr << "Bad message from "
                  << from.address().to_string() << ":" << from.port()
                  << " : " << e.what() << "\n";
    }
} //processes receive - unwraps the envelope and sends it to process_msg

void Node::process_msg(udp::endpoint& from, const proto::Envelope& env) {
    std::cout << "processing message" << std::endl;
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
            std::cerr << "Message type cannot be processed: " << static_cast<int>(env.type) << std::endl;
    }
} //dispatches funcs according to message type

void Node::process_register(const udp::endpoint& from, const proto::Envelope& env) {
    if (!is_root_) {
        std::cerr << "I'M GROOT (NOT ROOT, DONT REGISTER HERE!)";
        return;
    }
    try {
        peerInfo p;
        if (env.body["listen_port"] != from.port())
            std::cerr << "register listen port is different from sending port" << std::endl;
        p.ep = from;
        p.last_seen = Clock::now();
        p.peerId = env.src;
        clients_map_[env.src] = p;
        clients_.push_back(p.peerId);

        int want = env.body["want_peers"];

        std::string tx = env.tx;

        std::cout   << "peer " << env.src << " registered\n";
        std::cout   << "here's his info: " << clients_map_[p.peerId].ep.address() << ":" << clients_map_[env.src].ep.port() << std::endl;

        handle_register_ack(tx, p, want);
    } catch (const std::exception& e) {
        std::cerr << "Bad register message from " << from.address().to_string() << ": " << e.what();
    }
} //for root - processes registration requests.

