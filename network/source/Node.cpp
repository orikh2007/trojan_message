//
// Created by orikh on 22/12/2025.
//

#include "../headers/Node.h"

void erase_client_id(std::vector<NodeId>& v, const NodeId& id) {
    v.erase(std::remove(v.begin(), v.end(), id), v.end());
}

Node::Node(const uint16_t listen_port)
    : ip_(getIP(v4)),
      port_(listen_port),
      ep_(udp::endpoint(asio::ip::make_address(ip_), port_)),
      is_root_(),
      root_ip_(),
      root_ep_(),
      node_id_(),
      io_(),
      socket_(io_),
      recv_(true), daddy_(),
      level_(-1),
      cur_connections_(0),
      prune_timer_(io_),
      clients_map_(),
      clients_() {
    std::cout << "starting node init" << std::endl;
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

void Node::start() {
    recv_ = true;
    start_receive();
    std::cout << "starting pruner" << std::endl;
    start_pruner();
} //start the receiving io "loop"

asio::io_context& Node::io() { return io_; } //get io context

void Node::start_pruner() {
    prune_tick();
}

void Node::become_root() {
    is_root_ = true;
    setRoot(ip_);
    level_ = 0;

    port_ = ROOT_PORT;
    const auto addr = asio::ip::make_address(ip_);
    ep_ = udp::endpoint(addr, port_);

    PeerInfo me;
    me.peerId = node_id_;
    me.ep = ep_;
    me.last_seen = Clock::now();
    clients_.push_back(me.peerId);
    clients_map_.insert_or_assign(me.peerId, me);
    rebind();
} //become the root - tell dynu and everyone you're the root

void Node::rebind() {
    asio::post(io_, [self = shared_from_this()] {
        std::error_code ec;

        // 1) cancel pending ops (will trigger operation_aborted in handler)
        self->socket_.cancel(ec);

        // 2) close old socket
        self->socket_.close(ec);

        // 3) open + bind new port
        self->socket_.open(udp::v4(), ec);
        if (ec) { std::cerr << "open failed: " << ec.message() << "\n"; return; }

        self->socket_.bind(udp::endpoint(udp::v4(), self->port_), ec);
        if (ec) { std::cerr << "bind failed: " << ec.message() << "\n"; return; }

        std::cout << "Rebound to UDP port " << self->port_ << "\n";

        self->prune_timer_.cancel(ec);
        // 4) re-arm receive
        self->start_receive();
    });
}



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
        } else if (cmd == "graph"){
            print_graph();
        } else if (cmd == "dis") {
            handle_dis();
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

void Node::handle_link(std::istringstream& iss) {
    NodeId to_id;
    try {
        auto& p = *connections_.at(to_id);

    } catch (std::exception& e) {
        std::cerr << "Linking up failed: " << e.what() << std::endl;
    }
}

void Node::handle_send(std::istringstream& iss) {
    NodeId to_id;
    int port_int = 0;
    iss >> to_id;

    if (to_id.empty()) {
        std::cout << "Usage: send <to_id> <message>\n";
        return;
    }
    try {
        auto& p = *connections_.at(to_id);
    udp::endpoint tgt_ep = p.ep;

    std::string msg;
    std::getline(iss, msg);
    if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);

    std::cout << to_id << ", " << p.ep.address().to_string() << ", " << p.ep.port() << ", " << std::endl;

    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t x = gen();

    auto seq = x%500;

    json j = proto::msg_data_b64(node_id_, to_id, seq, proto::to_bytes(msg));

    auto data = proto::dump_compact(j);

    send_text(tgt_ep, data);
    std::cout << "Sent (async) to " << tgt_ep.address().to_string() << ":" << tgt_ep.port() << "\n";
    } catch (std::exception e) {
        std::cerr << "Exception trying to send msg: " << e.what() <<std::endl;
    }
} //send someone a message - temporary

void Node::handle_register() {
    const auto j = proto::msg_register(node_id_, socket_.local_endpoint().port(), 3);
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

    root_ep_ = ep;

    send_text(root_ep_, data);
    std::cout << "Sent (async) to " << root_ip_ << ":" << ROOT_PORT << "\n";
} //register with the root - send it node id and open port

void Node::handle_dis() {
    auto msg = proto::msg_disconnect(node_id_);
    auto data = proto::dump_compact(msg);
    broadcast(data);
    for (auto& it : connections_) {
        link_down(it.first);
    }
}

void Node::handle_register_ack(const std::string& tx, const PeerInfo& curP, const int want) {
    const size_t size = clients_.size();
    int n = std::min(static_cast<int>(size)-1, 4);
    n = std::min(n, want);
    std::vector<PeerInfo> peers;

    std::random_device rd;
    std::mt19937 gen {rd()};
    std::ranges::shuffle(clients_, gen);

    for (size_t i = 0; i < clients_.size() && peers.size() < static_cast<size_t>(n); ++i) {
        const std::string& cli = clients_[i];
        if (cli == curP.peerId) continue;
        clients_map_.at(cli).tkn = proto::random_token_hex();
        peers.push_back(clients_map_.at(cli));
    }

    const std::string token = proto::random_token_hex();

    const json jNew = proto::msg_register_ack(tx, curP.ep, peers, token); //reg_ack JSON for newcomer
    const udp::endpoint registeringCli = curP.ep;
    const auto dataNew = proto::dump_compact(jNew); //reg_ack data for newcomer
    send_text(registeringCli, dataNew);
    std::cout << "Sent reg_ack to " << registeringCli.address().to_string() << ":" << registeringCli.port() << std::endl;

    for (auto membr : peers){
        const json jMembr = proto::msg_introduce(curP, membr.tkn); //introduce JSON for existing
        auto epMembr = membr.ep;
        const auto dataMembr = proto::dump_compact(jMembr);
        send_text(epMembr, dataMembr);
        std::cout << "Sent introduce msg to " << epMembr.address().to_string() << ":" << epMembr.port() << std::endl;
    }

} /* for root - handles register_ack -
                                                                                                  * sends registering client his peers and token,
                                                                                                  * and tells the peers to connect to the registering client
                                                                                                  */

void Node::broadcast(const std::string& msg) {
    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
        auto curEp = it->second->ep;
        send_text(curEp, msg);
    }
}

void Node::start_punch(const PeerInfo& p, const int timeout, const int punch_ms, const std::string& tkn) {
    std::cout << "Trying to punch: " << p.ep.address().to_string() << std::endl;
    auto& slot = connections_[p.peerId];
    slot = std::make_unique<Connection>(io_, p.ep, tkn);
    slot->tries = 0;
    slot->connected = false;

    std::cout << "[START_PUNCH] peerId=" << p.peerId
              << " ep=" << p.ep.address().to_string() << ":" << p.ep.port()
              << " token=" << tkn << "\n";

    punch(p.peerId, timeout, punch_ms);
}

void Node::punch(const std::string& peerId, const int timeout, const int punch_ms) {
    const auto it = connections_.find(peerId);
    if (it == connections_.end()) return;
    auto& a = *it->second;
    if (a.connected) return;

    const auto j = proto::msg_punch(node_id_, a.punch_token, level_);
    send_text(a.ep, proto::dump_compact(j));

    a.tries++;



    if (a.tries >= 8) {
        std::cerr << "failed punching " << a.ep.address().to_string() << ":" << a.ep.port() << ":(" << std::endl;
        connections_.erase(it);
        return;
    }
    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t x = gen();
    auto jitt = x%10;
    std::cout << "JITTER " << jitt << std::endl;
    a.timer.expires_after(std::chrono::milliseconds(punch_ms) + std::chrono::milliseconds(10 * a.tries) + std::chrono::milliseconds(jitt));
    a.timer.async_wait([self = shared_from_this(), peerId, timeout, punch_ms](const std::error_code& ec){
        if (ec) return;
        self->punch(peerId, timeout, punch_ms);
    });
}

void Node::send_text(const udp::endpoint& target, std::string text) {
    auto data = make_shared<std::string>(std::move(text));
    socket_.async_send_to(
        asio::buffer(*data),
        target,
        [self = shared_from_this(), data, target](const std::error_code& ec, std::size_t bytes) {
            if (ec){
                std::cerr << "send error: " << ec.message() << " " << target.address().to_string() << " " << target.port() << ": " << proto::to_string(proto::parse_envelope(*data).type) << "\n";
            }
            else {
                std::cout << "SEND to " << target.address().to_string() << ":" << target.port() << ": " << data.get() << std::endl;
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
        if (ec == asio::error::operation_aborted) {
            // This happens when we close/cancel the socket on purpose.
            return;
        }
        std::cerr << "recv error: " << ec.message() << " w/ " << n << " bytes\n";
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
} //handles newly received messages - rearms start_receive sends it to process_receive

void Node::process_receive(udp::endpoint& from, const std::string &msg) {
    try {
        const proto::Envelope env = proto::parse_envelope(msg);
        std::cout << "RECV from " << from.address().to_string() << ":" << from.port() << " | Type: " << proto::to_string(env.type) << std::endl;
        dispatch(from, env);
    } catch (const std::exception& e) {
        std::cerr << "Bad message from "
                  << from.address().to_string() << ":" << from.port()
                  << " : " << e.what() << "\n";
    }
} //processes receive - unwraps the envelope and sends it to process_msg

void Node::dispatch(udp::endpoint& from, const proto::Envelope& env) {
    std::cout << "processing message" << std::endl;
    switch (env.type) {
        case MsgType::REGISTER:
            on_register(from, env);
            break;
        case MsgType::REGISTER_ACK:
            on_register_ack(from, env);
            break;
        case MsgType::KEEPALIVE:
            on_keepalive(from, env);
            break;
        case MsgType::PEER_LIST:
            break;
        case MsgType::INTRODUCE:
            on_introduce(from, env);
            break;
        case MsgType::PUNCH:
            on_punch(from, env);
            break;
        case MsgType::PUNCH_ACK:
            on_punch_ack(from, env);
            break;
        case MsgType::DATA:
            on_data(from, env);
            break;
        case MsgType::ERROR_:
            break;
        case MsgType::DISCONNECT:
            on_disconnect(from, env);
            break;
        case MsgType::LINK_UP:
            on_linkup(from, env);
            break;
        case MsgType::LINK_DOWN:
            on_linkdown(from, env);
            break;
        default:
            std::cerr << "Message type cannot be processed: " << static_cast<int>(env.type) << std::endl;
    }
} //dispatches funcs according to message type

void Node::on_register(const udp::endpoint& from, const proto::Envelope& env) {
    if (!is_root_) {
        std::cerr << "I'M GROOT (NOT ROOT, DONT REGISTER HERE!)";
        return;
    }
    try {
        PeerInfo p;
        p.ep = from;
        p.last_seen = Clock::now();
        p.peerId = env.src;
        std::cout << "K: " << env.src << " T: " << p.peerId << std::endl;
        clients_map_[env.src] = p;
        clients_.push_back(p.peerId);

        int want = env.body.at("want_peers").get<int>();

        std::string tx = env.tx;

        std::cout   << "peer " << env.src << " registered\n";
        std::cout   << "here's his info: " << clients_map_[p.peerId].ep.address() << ":" << clients_map_[env.src].ep.port() << std::endl;

        handle_register_ack(tx, p, want);
    } catch (const std::exception& e) {
        std::cerr << "Bad register message from " << from.address().to_string() << ": " << e.what();
    }
} //for root - processes registration requests.

void Node::on_register_ack(const udp::endpoint& from, const proto::Envelope& env) {
    try {
        if (env.src != "ROOT" || from.address().to_string() != root_ip_)
            std::cerr << "Received reg_ack from not-root: " << env.src << " data: " << env.body << std::endl;

        json body = env.body;

        std::vector<json> jPeers = body["peers"];
        std::vector<PeerInfo> peers;
        for (auto& jp : jPeers) {
            std::error_code ec;
            auto peerAddr = asio::ip::make_address((jp.at("ip")).get<std::string>(), ec);
            if (!ec){
                PeerInfo p;
                auto peerPort = jp.at("port").get<uint16_t>();
                auto peerEp   = udp::endpoint(peerAddr, peerPort);
                p.ep = peerEp;
                p.last_seen = Clock::now();
                p.peerId = jp.at("id").get<std::string>();
                p.tkn = jp.at("token").get<std::string>();
                peers.push_back(p);
            } else {
                std::cerr << "Peer's address: " << jp.at("ip") << ", couldn't be used" << ec.message() << std::endl;
            }
        }

        int punch_ms = env.body["punch_ms"];
        int timeout = env.body["timeout_ms"];
        int ka = env.body["ka_ms"];
        std::string tkn = env.body["token"];



        for (const auto& p : peers) {
            remember_token(p.peerId, p.tkn, 6000);
            std::cout << "Trying to punch: " << p.ep.address().to_string() << ":" << p.ep.port() << std::endl;
            start_punch(p, timeout, punch_ms, p.tkn);
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception while registering register_ack: " << e.what() << std::endl;
    }
}

void Node::remember_token(const std::string& peer_id, const std::string& token, int ttl_ms = 6000) {
    token_cache_[token] = TokenEntry{
        peer_id,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl_ms)
    };
}

void Node::prune_tokens() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = token_cache_.begin(); it != token_cache_.end(); ) {
        if (it->second.expires <= now) it = token_cache_.erase(it);
        else ++it;
    }
}

void Node::on_introduce(const udp::endpoint& from, const proto::Envelope& env) {
    if (env.src != "ROOT")
        std::cerr << "Received introduce from not-root: " << env.src << " data: " << env.body << std::endl;

    const json& body = env.body;


    const std::string peer_id = body.at("peer").at("id").get<std::string>();
    const std::string pIp     = body.at("peer").at("ip").get<std::string>();
    const uint16_t    pPort   = body.at("peer").at("port").get<uint16_t>();

    const int timeout  = body.value("timeout_ms", 4000);
    const int punch_ms = body.value("punch_ms", 250);
    const std::string tkn = body.at("token").get<std::string>();


    std::error_code ec;
    auto pAddr = asio::ip::make_address(pIp, ec);
    if (ec) {
        std::cerr << "Bad address from " << from.address().to_string() << std::endl;
        return;
    }

    remember_token(peer_id, tkn);

    PeerInfo p(udp::endpoint(pAddr, pPort), peer_id, tkn);

    std::cout << "Trying to punch: " << p.ep.address().to_string() << ":" << p.ep.port() << std::endl;

    start_punch(p, timeout, punch_ms, tkn);
}

void Node::mark_connected(std::string tkn, const std::string &node_id, udp::endpoint ep, int level) {
    auto& cur = connections_[node_id];
    if (!cur) cur = std::make_unique<Connection>(io_, ep, tkn);

    cur->level = level;
    cur->ep = ep;
    cur->punch_token = tkn;
    cur->connected = true;
    cur->timer.cancel();
    cur->last_seen = Clock::now();
    keep_alive(node_id);
}

bool Node::token_is_known(std::string tkn, udp::endpoint from, const std::string &n_id) {
    prune_tokens();

    // Case 1: we already have an attempt/connection entry for that peer id
    if (auto it = connections_.find(n_id); it != connections_.end() && it->second) {
        if (it->second->punch_token == tkn) return true;
    }

    // Case 2: accept based on token cache (handles reorder: punch arrives before introduce)
    if (auto it = token_cache_.find(tkn); it != token_cache_.end()) {
        return true;
    }

    return false;
}

void Node::on_punch(const udp::endpoint& from, const proto::Envelope& env) {
    auto senderId = env.src;
    json body = env.body;
    auto tkn = body["token"].get<std::string>();

    prune_tokens();

    // If we don't yet have a connection entry for senderId, but token is cached, allow and create.
    if (!connections_.contains(senderId)) {
        auto itTok = token_cache_.find(tkn);
        if (itTok != token_cache_.end()) {
            // create attempt entry so future checks are exact
            auto& cur = connections_[senderId];
            if (!cur) cur = std::make_unique<Connection>(io_, from, tkn);
        }
    }

    if (!token_is_known(tkn, from, senderId)) {
        std::cerr << "Ignoring punch with unknown token\n";
        return;
    }

    auto ack = proto::msg_punch_ack(node_id_, tkn, level_);
    send_text(from, proto::dump_compact(ack));


    link_up(env.src);

    mark_connected(tkn, senderId, from, env.body.at("level").get<uint16_t>());
    choose_parent();
}

void Node::on_punch_ack(const udp::endpoint& from, const proto::Envelope& env) {
    auto senderId = env.src;
    json body = env.body;

    auto tkn = body["token"].get<std::string>();

    auto it = connections_.find(senderId);
    if (it == connections_.end()) return;
    auto& a = *it->second;

    if (a.punch_token != tkn) return; // mismatch/old attempt - ignore!

    a.connected = true;
    a.timer.cancel();

    a.level = body.at("level");


    link_up(env.src);

    std::cout << "Success! connected to: " << senderId << " on " << from.address().to_string() << ":" << from.port() << ". Now connected to: " << cur_connections_ << std::endl;
    choose_parent();
}

void Node::choose_parent() {
    if (connections_.empty()) return;
    auto con = connections_.begin();
    int minLevel = con->second->level;
    NodeId minLevelId = con->first;
    ++con;
    for (; con != connections_.end(); ++con){
        if (con->second->level < minLevel && con->second->level != -1 && con->second->connected) {
            minLevel = con->second->level;
            minLevelId = con->first;
        }
    }
    daddy_ = minLevelId;
    level_ = minLevel + 1;
    std::cout << "DADDY: " << daddy_ << " LEVEL: " << level_ << std::endl;
}

void Node::on_data(const udp::endpoint& from, const proto::Envelope& env) {
    auto& b = env.body;
    auto data = proto::data_payload_bytes(env);
    if (!data) return;
    std::string msg(data->begin(), data->end());
    if (connections_.contains(env.src))
        connections_[env.src]->last_seen = Clock::now();
    else {
        std::cerr << "Got data from unknown sender" << std::endl;
    }
    std::cout << "Got DATA from: " << env.src << " saying: " << msg << std::endl;
}

void Node:: on_disconnect(const udp::endpoint& from, const proto::Envelope& env) {
    remove_connection(env.src);
    link_down(env.src);
}

void Node::on_linkup(const udp::endpoint& from, const proto::Envelope& env) {
    if (!is_root_) return;
    NodeId src = env.src;
    NodeId neigh = env.body.at("peer").get<NodeId>();
    clients_map_[src].neighbors.insert(neigh);
    clients_map_[neigh].neighbors.insert(src);
    std::cout << "[ROOT GRAPH] " << src << " <-> " << neigh
          << " | deg(src)=" << clients_map_[src].neighbors.size()
          << " deg(neigh)=" << clients_map_[neigh].neighbors.size()
          << "\n";
}

void Node::on_linkdown(const udp::endpoint& from, const proto::Envelope& env) {
    if (!is_root_) return;
    NodeId src = env.src;
    NodeId neigh = env.body.at("peer").get<NodeId>();
    auto src_it = clients_map_.find(src);
    auto neigh_it = clients_map_.find(neigh);
    if (neigh_it != clients_map_.end()) clients_map_[neigh].neighbors.erase(src);
    if (src_it != clients_map_.end()) clients_map_[src].neighbors.erase(neigh);
    if (clients_map_[neigh].neighbors.size() == 0 && neigh != node_id_) {
        clients_map_.erase(neigh);
        erase_client_id(clients_, neigh);
    }if (clients_map_[src].neighbors.size() == 0 && src != node_id_) {
        clients_map_.erase(src);
        erase_client_id(clients_, src);
    }
}

void Node::on_keepalive(const udp::endpoint& from, const proto::Envelope& env) {
    auto it = connections_.find(env.src);
    if (it != connections_.end() && it->second) {
        it->second->last_seen = Clock::now();
        std::cout << level_ << std::endl;
    } else {
        std::cerr << "KEEPALIVE from unknown/untracked " << env.src << "\n";
    }
}

void Node::link_up(const NodeId& peer) {
    if (linked_up_.contains(peer)) return;
    prune_connections();
    linked_up_.insert(peer);
    cur_connections_++;
    if (!is_root_) {
        proto::json data = proto::msg_linkup(peer, node_id_);
        auto msg = proto::dump_compact(data);
        send_text(root_ep_, msg);
    }
}

void Node::link_down(const NodeId& peer) {
    if (!is_root_) {
        proto::json data = proto::msg_linkdown(peer, node_id_);
        auto msg = proto::dump_compact(data);
        send_text(root_ep_, msg);
    }
}

void Node::keep_alive(const std::string& peerId) {
    prune_dead();
    auto it = connections_.find(peerId);
    if (it == connections_.end() || !it->second) {
        std::cerr << "tried to keep_alive with a non-existing connection" << std::endl;
        return;
    }
    Connection& conn = *it->second;

    udp::endpoint ep = conn.ep;
    json j = proto::msg_keepalive(node_id_);
    auto data = proto::dump_compact(j);

    std::cout << "sent keep alive to: " << ep.address().to_string() << std::endl;

    send_text(ep, data);

    conn.ka_timer.expires_after(std::chrono::milliseconds(conn.ka_ms));
    conn.ka_timer.async_wait([self = shared_from_this(), peerId](const std::error_code ec) {
        if (ec == asio::error::operation_aborted) return;
        if (ec) {
            std::cerr << "ka_timer async wait failed: " << ec.message() << std::endl;
            return;
        }
        self->keep_alive(peerId);
    });
}

void Node::prune_tick() {
    prune_timer_.expires_after(prune_sec);
    prune_timer_.async_wait([self = shared_from_this()](const std::error_code& ec) {
        if (ec == asio::error::operation_aborted) return; // canceled on shutdown/rebind
        if (ec) {
            std::cerr << "prune_timer error: " << ec.message() << "\n";
            return;
        }

        // Do the pruning work
        self->prune_tokens();            // you already have this
        self->prune_dead();  // implement as we discussed

        // reschedule
        self->prune_tick();
    });
}

void Node::prune_dead() {
    std::vector<NodeId> dead;
    for (const auto& [id, conn] : connections_) {
        if (Clock::now() - conn->last_seen > expiration_time_sec) {
            dead.push_back(id);
        }
    }
    for (const auto& id : dead) {
        std::cout << id << " is dead :(\n";
        dynamic_disconnect(id);
    }
}

void Node::print_graph() {
    if (!is_root_) {
        std::cout << "[ROOT GRAPH] not root\n";
        return;
    }

    std::cout << "\n========== ROOT GRAPH ==========\n";
    std::cout << "nodes: " << clients_map_.size() << "\n";

    // Per-node summary
    for (const auto& [id, p] : clients_map_) {
        std::cout << " - " << id
                  << " deg=" << p.neighbors.size()
                  << " ep=" << p.ep.address().to_string() << ":" << p.ep.port()
                  << "\n";
    }

    // Print edges once (u < v)
    std::cout << "\nedges:\n";
    size_t edges = 0;
    for (const auto& [u, pu] : clients_map_) {
        for (const auto& v : pu.neighbors) {
            if (u < v) { // print each undirected edge only once
                std::cout << "  " << u << " <-> " << v << "\n";
                ++edges;
            }
        }
    }
    std::cout << "edge_count=" << edges << "\n";
    std::cout << "================================\n\n";
}

void Node::prune_connections() {
    if (cur_connections_ > MAX_CONNS) rand_disconnect();
}


void Node::rand_disconnect() {
    // disconnecting from a random connection because the connection limit has been exceeded.
    if (connections_.empty()) return;
    auto conn = connections_.begin();
    std::advance(conn, random_0_to_n(static_cast<int>(connections_.size())));
    NodeId curId = conn->first;

    dynamic_disconnect(curId);
}

void Node::dynamic_disconnect(NodeId id) {
    auto it = connections_.find(id);
    if (it == connections_.end()) return;

    udp::endpoint cur = it->second->ep;
    json msg = proto::msg_disconnect(node_id_);
    send_text(cur, proto::dump_compact(msg));
    remove_connection(id);
}

void Node::remove_connection(const NodeId& peerId) {
    auto it = connections_.find(peerId);
    if (it == connections_.end() || !it->second) return;

    // cancel timers first (handlers will get operation_aborted)
    std::error_code ec;
    it->second->timer.cancel(ec);
    it->second->ka_timer.cancel(ec);

    connections_.erase(it);

    linked_up_.erase(peerId);
    if (cur_connections_ > 0) cur_connections_--;

    link_down(peerId);

    std::cout << "Removed " << peerId << " As I got a new connections or because I've been disconnected from." << std::endl;
}


// ------------------------ routing logic ------------------------

// ---------- helpers ----------

static std::vector<NodeId> reconstruct_path(
    const NodeId& src,
    const NodeId& dst,
    const std::unordered_map<NodeId, NodeId>& parent)
{
    std::vector<NodeId> path;
    NodeId cur = dst;
    path.push_back(cur);

    while (cur != src) {
        auto it = parent.find(cur);
        if (it == parent.end()) return {}; // no path
        cur = it->second;
        path.push_back(cur);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

// BFS shortest path on the root's adjacency graph.
// If forbidden contains a node, BFS will never step into it.
// (Typically forbidden is "current route nodes", excluding endpoints.)
std::optional<std::vector<NodeId>> Node::bfs_shortest_path(
    const NodeId& src,
    const NodeId& dst,
    const std::unordered_set<NodeId>& forbidden) const
{
    if (!clients_map_.contains(src) || !clients_map_.contains(dst)) return std::nullopt;
    if (src == dst) return std::vector<NodeId>{src};

    std::queue<NodeId> q;
    std::unordered_set<NodeId> vis;
    std::unordered_map<NodeId, NodeId> parent;

    auto allowed = [&](const NodeId& x) -> bool {
        if (forbidden.contains(x)) return false;
        return clients_map_.contains(x);
    };

    if (!allowed(src) || !allowed(dst)) return std::nullopt;

    vis.insert(src);
    q.push(src);

    bool found = false;

    while (!q.empty() && !found) {
        NodeId u = q.front();
        q.pop();

        const auto& neighs = clients_map_.at(u).neighbors;
        for (const auto& v : neighs) {
            if (!allowed(v)) continue;
            if (vis.contains(v)) continue;

            vis.insert(v);
            parent[v] = u;

            if (v == dst) { found = true; break; }
            q.push(v);
        }
    }

    if (!found) return std::nullopt;

    auto path = reconstruct_path(src, dst, parent);
    if (path.empty()) return std::nullopt;
    return path;
}

// Attempts to replace edge (u->v) in path with a longer detour u..v,
// avoiding nodes already in `path` (except u and v).
// Returns true if detour injected (path modified).
bool Node::try_inject_detour(std::vector<NodeId>& path, size_t edge_i) const
{
    // edge_i refers to (path[edge_i], path[edge_i+1])
    if (edge_i + 1 >= path.size()) return false;

    const NodeId u = path[edge_i];
    const NodeId v = path[edge_i + 1];

    // Forbidden = all nodes in current path except u and v
    std::unordered_set<NodeId> forbidden;
    forbidden.reserve(path.size());
    for (const auto& n : path) forbidden.insert(n);
    forbidden.erase(u);
    forbidden.erase(v);

    auto detour_opt = bfs_shortest_path(u, v, forbidden);
    if (!detour_opt) return false;

    const auto& detour = *detour_opt;

    // detour must be longer than direct edge (length >= 3 nodes => >=2 edges)
    if (detour.size() < 3) return false;

    // Splice: replace [u, v] with [u, x, ..., v]
    // path = prefix(0..edge_i) + detour(1..end-2) + suffix(edge_i+1..end)
    std::vector<NodeId> out;
    out.reserve(path.size() + detour.size());

    // prefix includes u
    out.insert(out.end(), path.begin(), path.begin() + static_cast<long long>(edge_i) + 1);

    // middle: detour without endpoints
    out.insert(out.end(), detour.begin() + 1, detour.end() - 1);

    // suffix starts at v
    out.insert(out.end(), path.begin() + static_cast<long long>(edge_i) + 1, path.end());

    path.swap(out);
    return true;
}

// ---------- main API ----------

// Root-side route computation:
// 1) shortest path BFS
// 2) if hops < min_hops, inject detours to stretch while keeping it simple
std::optional<std::vector<NodeId>> Node::compute_route(
    const NodeId& src,
    const NodeId& dst,
    int min_hops) const
{
    if (!is_root_) return std::nullopt;
    if (min_hops < 0) min_hops = 0;

    // Step A: shortest path with no restrictions
    std::unordered_set<NodeId> none;
    auto base_opt = bfs_shortest_path(src, dst, none);
    if (!base_opt) return std::nullopt;

    std::vector<NodeId> path = *base_opt;

    auto hops = [&]() -> int {
        if (path.size() < 2) return 0;
        return static_cast<int>(path.size() - 1);
    };

    if (hops() >= min_hops) return path;

    // Step B: stretch by detour injection
    // We need randomness to avoid always trying the same edge.
    std::random_device rd;
    std::mt19937 rng(rd());

    // Safety: you can't have a simple path longer than N-1 edges.
    // This isn't a "max hops" feature—it's a graph reality constraint.
    const int N = static_cast<int>(clients_map_.size());
    const int max_possible_simple_hops = std::max(0, N - 1);
    if (min_hops > max_possible_simple_hops) {
        // impossible without repeats
        return std::nullopt;
    }

    // Keep trying to inject detours until we reach min_hops
    // Bound the work so we don't loop forever on hard graphs.
    int attempts = 0;

    while (hops() < min_hops && attempts < attempt_budget) {
        attempts++;

        if (path.size() < 2) break;

        // pick a random edge index in [0, path.size()-2]
        std::uniform_int_distribution<int> dist(0, static_cast<int>(path.size() - 2));
        size_t edge_i = static_cast<size_t>(dist(rng));

        // try to inject; if fails, loop and try another edge
        (void)try_inject_detour(path, edge_i);
    }

    if (hops() < min_hops) {
        // Graph too small / constrained to reach min hops without repeats
        return std::nullopt;
    }

    return path;
}