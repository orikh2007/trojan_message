//
// Created by orikh on 22/12/2025.
//

#include "../headers/Node.h"


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
      clients_map_() {
    log_debug("Node init started");
    std::error_code ec;

    Logger::get().set_level(LogLevel::DEBUG);

    node_id_ = proto::random_node_id_hex();

    log_info("Node ID: {}", node_id_);

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
    log_info("Listening on UDP port {}", listen_port);
} //constructor

void Node::start() {
    recv_ = true;
    start_receive();
    log_debug("Starting pruner");
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
        if (ec) { log_error("Socket reopen failed: {}", ec.message()); return; }

        self->socket_.bind(udp::endpoint(udp::v4(), self->port_), ec);
        if (ec) { log_error("Socket rebind failed: {}", ec.message()); return; }

        log_info("Rebound to UDP port {}", self->port_);

        self->prune_timer_.cancel(ec);
        // 4) re-arm receive
        self->start_receive();
        self->start_pruner();
    });
}



const std::string& Node::id() {return node_id_;} //get node id

void Node::handle_command(const std::string& line) {
    // Example commands:
    // send 1.2.3.4 7777 hello
    // quit
    try {
        if (line == "quit") {
            log_info("Stopping...");
            io_.stop();
            return;
        }

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "send" || cmd == "s") {
            handle_send(iss);
        } else if (cmd == "register" || cmd == "r") {
            handle_register();
        } else if (cmd == "root" ) {
            become_root();
        } else if (cmd == "graph" || cmd == "g"){
            print_graph();
        } else if (cmd == "dis" || cmd == "d") {
            handle_dis();
        } else if (cmd == "circuit") {
            NodeId dst;
            iss >> dst;
            if (dst.empty()) { log_info("Usage: circuit <dst_id>"); }
            else {
                if (static_cast<int>(clients_map_.size()) < MIN_GRAPH_SIZE) {
                    log_warn("Graph not populated yet - wait for GRAPH_UPDATE broadcasts");
                } else {
                    begin_circuit_build(dst);
                }
            }
        } else if (cmd == "sanon") {
            NodeId dst;
            std::string msg;
            iss >> dst;
            std::getline(iss, msg);
            if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
            if (dst.empty() || msg.empty()) { log_info("Usage: sanon <dst_id> <message>"); }
            else {
                auto it = circuit_by_dst_.find(dst);
                if (it == circuit_by_dst_.end()) log_warn("No ready circuit to {}. Run 'circuit {}' first.", dst, dst);
                else send_via_circuit(it->second, msg);
            }
        } else if (cmd == "sendfile") {
            NodeId dst;
            std::string filepath;
            iss >> dst >> filepath;
            if (dst.empty() || filepath.empty()) {
                log_info("Usage: sendfile <dst_id> <filepath>");
            } else {
                auto circuit_it = circuit_by_dst_.find(dst);
                if (circuit_it == circuit_by_dst_.end()) {
                    log_warn("No ready circuit to {}. Run 'circuit {}' first.", dst, dst);
                } else {
                    std::ifstream file(filepath, std::ios::binary);
                    if (!file) {
                        log_warn("Cannot open file: {}", filepath);
                    } else {
                        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                                   std::istreambuf_iterator<char>());
                        // Detect content type from extension
                        ContentType ct = TXT;
                        if (filepath.ends_with(".jpg") || filepath.ends_with(".jpeg") ||
                            filepath.ends_with(".png") || filepath.ends_with(".gif"))
                            ct = IMG;
                        else if (filepath.ends_with(".mp4") || filepath.ends_with(".mkv") ||
                                 filepath.ends_with(".avi") || filepath.ends_with(".mov"))
                            ct = VID;
                        log_info("Sending {} bytes to {} as {}", data.size(), dst, to_string(ct));
                        send_chunked(dst, std::move(data), ct);
                    }
                }
            }
        } else if (cmd == "reply") {
            NodeId src_id;
            std::string msg;
            iss >> src_id;
            std::getline(iss, msg);
            if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
            if (src_id.empty() || msg.empty()) { log_info("Usage: reply <src_id> <message>"); }
            else {
                auto it = received_circuit_by_src_.find(src_id);
                if (it == received_circuit_by_src_.end())
                    log_warn("No circuit from {}. Has their probe arrived yet?", src_id);
                else send_reply_via_circuit(it->second, msg);
            }
        } else {
            std::cout << "Commands:\n"
                      << "  send <peer_id> <message>\n"
                      << "  register\n"
                      << "  root\n"
                      << "  circuit <dst_id>          - build onion circuit to dst\n"
                      << "  sanon <dst_id> <msg>      - send anonymously via circuit\n"
                      << "  sendfile <dst_id> <path>  - send a file anonymously via circuit\n"
                      << "  reply <src_id> <msg>      - reply back through a received circuit\n"
                      << "  quit\n";
        }
    } catch (const std::exception& e) {
        io_.stop();
        // Never let exceptions escape into asio handlers
        log_error("handle_command exception: {}", e.what());
    }

} //handle the cli command - temporary

void Node::handle_link(std::istringstream& iss) {
    NodeId to_id;
    iss >> to_id;
    if (to_id.empty()) {
        log_info("Usage: link <peer_id>");
        return;
    }
    try {
        auto& p = *connections_.at(to_id);
        link_up(to_id);
    } catch (std::exception& e) {
        log_error("Link up failed: {}", e.what());
    }
}

void Node::handle_send(std::istringstream& iss) {
    NodeId to_id;
    int port_int = 0;
    iss >> to_id;

    if (to_id.empty()) {
        log_info("Usage: send <to_id> <message>");
        return;
    }
    try {
        auto& p = *connections_.at(to_id);
    udp::endpoint tgt_ep = p.ep;

    std::string msg;
    std::getline(iss, msg);
    if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);

    log_debug("Sending to {} at {}:{}", to_id, p.ep.address().to_string(), p.ep.port());

    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t x = gen();

    auto seq = x%500;

    json j = proto::msg_data_b64(node_id_, to_id, seq, proto::to_bytes(msg));

    auto data = proto::dump_compact(j);

    send_text(tgt_ep, data);
    log_info("Sent DATA to {}:{}", tgt_ep.address().to_string(), tgt_ep.port());
    } catch (std::exception e) {
        log_error("Exception sending message: {}", e.what());
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
        log_error("Bad root IP address: {} ({})", root_ip_, ec.message());
        return;
    }
    const udp::endpoint ep(addr, ROOT_PORT);

    root_ep_ = ep;

    send_text(root_ep_, data);
    log_info("Sent REGISTER to {}:{}", root_ip_, ROOT_PORT);
} //register with the root - send it node id and open port

void Node::request_conns() {
    auto j = proto::msg_req_conns(node_id_, port_);
    const auto data = proto::dump_compact(j);
    std::error_code ec;
    send_text(root_ep_, data);
    log_debug("Sent REQ_CONNS to root");
}

void Node::handle_dis() {
    auto msg = proto::msg_disconnect(node_id_);
    auto data = proto::dump_compact(msg);
    broadcast(data);
    for (auto& it : connections_) {
        link_down(it.first);
    }
}

void Node::handle_register_ack(const std::string& tx, const PeerInfo& curP, const int want) { //root function
    int n = std::min(static_cast<int>(clients_map_.size()) - 1, 4);
    n = std::min(n, want);
    std::vector<PeerInfo> peers;

    std::vector<std::string> keys;
    for (const auto &id: clients_map_ | std::views::keys) keys.push_back(id);
    std::random_device rd;
    std::mt19937 gen{rd()};
    std::ranges::shuffle(keys, gen);

    for (const auto& cli : keys) {
        if (static_cast<int>(peers.size()) >= n) break;
        if (cli == curP.peerId) continue;
        clients_map_.at(cli).tkn = proto::random_token_hex();
        peers.push_back(clients_map_.at(cli));
    }

    const std::string token = proto::random_token_hex();

    const json jNew = proto::msg_register_ack(tx, curP.ep, peers, token); //reg_ack JSON for newcomer
    const udp::endpoint registeringCli = curP.ep;
    const auto dataNew = proto::dump_compact(jNew); //reg_ack data for newcomer
    send_text(registeringCli, dataNew);
    log_info("Sent REGISTER_ACK to {}:{}", registeringCli.address().to_string(), registeringCli.port());

    for (const auto& membr : peers){
        const json jMembr = proto::msg_introduce(curP, membr.tkn); //introduce JSON for existing
        auto epMembr = membr.ep;
        const auto dataMembr = proto::dump_compact(jMembr);
        send_text(epMembr, dataMembr);
        log_debug("Sent INTRODUCE to {}:{}", epMembr.address().to_string(), epMembr.port());
    }

} /* for root - handles register_ack -
                                                                                                  * sends registering client his peers and token,
                                                                                                  * and tells the peers to connect to the registering client
                                                                                                  */

void Node::send_up(const proto::Envelope env) {
    auto& conn = connections_.at(daddy_);
    auto ep = conn->ep;
    auto j = proto::make_envelope(env.type, env.src, env.tx, env.body);
    send_text(ep, proto::dump_compact(j));
}

void Node::broadcast(const std::string& msg) {
    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
        auto curEp = it->second->ep;
        send_text(curEp, msg);
    }
}

void Node::start_punch(const PeerInfo& p, const int timeout, const int punch_ms, const std::string& tkn) {
    log_debug("[PUNCH] Starting punch to {} at {}:{}", p.peerId, p.ep.address().to_string(), p.ep.port());
    auto& slot = connections_[p.peerId];
    slot = std::make_unique<Connection>(io_, p.ep, tkn);
    slot->tries = 0;
    slot->connected = false;

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
        log_warn("[PUNCH] Failed to reach {}:{} after {} attempts", a.ep.address().to_string(), a.ep.port(), a.tries);
        connections_.erase(it);
        return;
    }
    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t x = gen();
    auto jitt = x%10;
    log_debug("[PUNCH] Retry {} to {}:{} (jitter {}ms)", a.tries, a.ep.address().to_string(), a.ep.port(), jitt);
    a.timer.expires_after(std::chrono::milliseconds(punch_ms) + std::chrono::milliseconds(10 * a.tries) + std::chrono::milliseconds(jitt));
    a.timer.async_wait([self = shared_from_this(), peerId, timeout, punch_ms](const std::error_code& ec){
        if (ec) return;
        self->punch(peerId, timeout, punch_ms);
    });
}

void Node::send_chunked(const NodeId& dst, std::vector<uint8_t> data, ContentType content_type) {
    auto transfer_id = proto::random_tx_id();
    uint32_t total_chunks = (data.size() + CHUNK_SIZE - 1) / CHUNK_SIZE;
    auto it = circuit_by_dst_.find(dst);
    if (it == circuit_by_dst_.end()) {
        log_warn("[CHUNK] No circuit to {}", dst);
        return;
    }
    auto circuit_id = it->second;

    OutgoingTransfer transfer;
    transfer.dst          = dst;
    transfer.content_type = content_type;
    transfer.total_chunks = total_chunks;

    for (uint32_t i = 0; i < total_chunks; i++) {
        size_t start = i * CHUNK_SIZE;
        size_t end = std::min(start + CHUNK_SIZE, data.size());
        std::vector<uint8_t> raw_chunk(data.begin() + start, data.begin() + end);
        transfer.chunks.push_back(raw_chunk);

        proto::ChunkMeta chunk{transfer_id, i, total_chunks, content_type, proto::b64_encode(raw_chunk)};
        auto msg = proto::msg_chunk(node_id_, chunk);
        send_via_circuit(circuit_id, proto::dump_compact(msg));
    }

    outgoing_transfers_[transfer_id] = std::move(transfer);
    log_info("[CHUNK] Sent {} chunks for transfer {}", total_chunks, transfer_id);
}

void Node::send_text(const udp::endpoint& target, std::string text) {
    auto data = make_shared<std::string>(std::move(text));
    socket_.async_send_to(
        asio::buffer(*data),
        target,
        [self = shared_from_this(), data, target](const std::error_code& ec, std::size_t bytes) {
            if (ec){
                log_error("Send error to {}:{}: {}", target.address().to_string(), target.port(), ec.message());
            }
            else {
                log_debug("SEND -> {}:{}", target.address().to_string(), target.port());
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
        log_error("Receive error: {} ({} bytes)", ec.message(), n);
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
        log_debug("RECV from {}:{} | type={}", from.address().to_string(), from.port(), proto::to_string(env.type));
        dispatch(from, env);
    } catch (const std::exception& e) {
        log_warn("Bad message from {}:{}: {}", from.address().to_string(), from.port(), e.what());
    }
} //processes receive - unwraps the envelope and sends it to process_msg

void Node::dispatch(udp::endpoint& from, const proto::Envelope& env) {
    log_debug("Dispatch: {}", proto::to_string(env.type));
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
        case MsgType::REQ_CONNS:
            on_req_conns(from, env);
            break;
        case MsgType::REQ_CONNS_ACK:
            on_req_conns_ack(from, env);
            break;
        case MsgType::HOP:
            on_hop(from, env);
            break;
        case MsgType::HOP_REPLY:
            on_hop_reply(from, env);
            break;
        case MsgType::CIRCUIT_BROKEN:
            on_circuit_broken(from, env);
            break;
        case MsgType::GRAPH_UPDATE:
            on_graph_update(from, env);
            break;
        case MsgType::INTRODUCE_REQ:
            on_introduce_req(from, env);
            break;
        case MsgType::CHUNK:
            on_chunk(from, env);
            break;
        case MsgType::CHUNK_ACK:
            on_chunk_ack(from, env);
            break;
        case MsgType::CHUNK_NACK:
            on_chunk_nack(from, env);
            break;
        default:
            log_warn("Unhandled message type: {}", static_cast<int>(env.type));
    }
} //dispatches funcs according to message type

void Node::on_register(const udp::endpoint& from, const proto::Envelope& env) { //root function
    if (!is_root_) {
        log_error("Received REGISTER but not root - ignoring (src: {})", env.src);
        return;
    }
    try {
        PeerInfo p;
        p.ep = from;
        p.last_seen = Clock::now();
        p.peerId = env.src;
        clients_map_[env.src] = p;

        int want = env.body.at("want_peers").get<int>();

        std::string tx = env.tx;

        log_info("Peer {} registered from {}:{}", env.src, from.address().to_string(), from.port());

        handle_register_ack(tx, p, want);
        send_graph_snapshot(from);
    } catch (const std::exception& e) {
        log_error("Bad REGISTER from {}: {}", from.address().to_string(), e.what());
    }
} //for root - processes registration requests.

void Node::on_req_conns(const udp::endpoint& from, const proto::Envelope& env) {
    if (!is_root_) return;
    int want = env.body.at("want_peers").get<int>();
    auto tx = env.tx;

    auto cur = clients_map_.find(env.src);
    if (cur == clients_map_.end()) {
        on_register(from, env);
        return;
    }
    const PeerInfo& curP = cur->second;

    int n = std::min(static_cast<int>(clients_map_.size()) - 1, MAX_CONNS);
    n = std::min(n, want);
    std::vector<PeerInfo> peers;

    std::vector<std::string> keys;
    for (const auto& [id, _] : clients_map_) keys.push_back(id);
    std::random_device rd;
    std::mt19937 gen{rd()};
    std::ranges::shuffle(keys, gen);

    for (const auto& cli : keys) {
        if (static_cast<int>(peers.size()) >= n) break;
        if (cli == curP.peerId) continue;
        if (clients_map_.at(curP.peerId).neighbors.contains(cli)) continue;
        clients_map_.at(cli).tkn = proto::random_token_hex();
        peers.push_back(clients_map_.at(cli));
    }

    const std::string token = proto::random_token_hex();

    const json jNew = proto::msg_req_conns_ack(tx, curP.ep, peers, token); //reg_ack JSON for newcomer
    const udp::endpoint registeringCli = curP.ep;
    const auto dataNew = proto::dump_compact(jNew); //reg_ack data for newcomer
    send_text(registeringCli, dataNew);
    log_info("Sent REQ_CONNS_ACK to {}:{}", registeringCli.address().to_string(), registeringCli.port());

    for (auto membr : peers){
        const json jMembr = proto::msg_introduce(curP, membr.tkn); //introduce JSON for existing
        auto epMembr = membr.ep;
        const auto dataMembr = proto::dump_compact(jMembr);
        send_text(epMembr, dataMembr);
        log_debug("Sent INTRODUCE to {}:{}", epMembr.address().to_string(), epMembr.port());
    }
}

void Node::on_register_ack(const udp::endpoint& from, const proto::Envelope& env) {
    try {
        if (env.src != "ROOT" || from.address().to_string() != root_ip_)
            log_warn("REGISTER_ACK from unexpected source: {} (body: {})", env.src, env.body.dump());

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
                log_warn("Invalid peer address {}: {}", jp.at("ip").dump(), ec.message());
            }
        }

        int punch_ms = env.body["punch_ms"];
        int timeout = env.body["timeout_ms"];
        int ka = env.body["ka_ms"];
        std::string tkn = env.body["token"];



        for (const auto& p : peers) {
            remember_token(p.peerId, p.tkn, 6000);
            log_debug("Punching {}:{}", p.ep.address().to_string(), p.ep.port());
            start_punch(p, timeout, punch_ms, p.tkn);
        }
    } catch (const std::exception& e) {
        log_error("REGISTER_ACK handler exception: {}", e.what());
    }
}

void Node::on_req_conns_ack(const udp::endpoint& from, const proto::Envelope& env) {
    try {
        if (env.src != "ROOT" || from.address().to_string() != root_ip_)
            log_warn("REQ_CONNS_ACK from unexpected source: {} (body: {})", env.src, env.body.dump());

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
                log_warn("Invalid peer address {}: {}", jp.at("ip").dump(), ec.message());
            }
        }

        int punch_ms = env.body["punch_ms"];
        int timeout = env.body["timeout_ms"];
        int ka = env.body["ka_ms"];
        std::string tkn = env.body["token"];



        for (const auto& p : peers) {
            remember_token(p.peerId, p.tkn, 6000);
            log_debug("Punching {}:{}", p.ep.address().to_string(), p.ep.port());
            start_punch(p, timeout, punch_ms, p.tkn);
        }
    } catch (const std::exception& e) {
        log_error("REQ_CONNS_ACK handler exception: {}", e.what());
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
        log_warn("INTRODUCE from unexpected source: {} (body: {})", env.src, env.body.dump());

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
        log_error("INTRODUCE has invalid peer IP {} (from {}): {}", pIp, from.address().to_string(), ec.message());
        return;
    }

    remember_token(peer_id, tkn);

    PeerInfo p(udp::endpoint(pAddr, pPort), peer_id, tkn);

    log_debug("Punching {}:{} (INTRODUCE)", p.ep.address().to_string(), p.ep.port());

    start_punch(p, timeout, punch_ms, tkn);
}

void Node::mark_connected(std::string tkn, const std::string &node_id, udp::endpoint ep, int level) {
    auto& cur = connections_[node_id];
    if (!cur) cur = std::make_unique<Connection>(io_, ep, tkn);

    const bool was_connected = cur->connected;

    cur->level = level;
    cur->ep = ep;
    cur->punch_token = tkn;
    cur->connected = true;
    cur->timer.cancel();
    cur->last_seen = Clock::now();

    if (!was_connected)
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
        log_warn("Ignoring PUNCH with unknown token from {}", senderId);
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

    const int level = body.at("level").get<int>();
    mark_connected(tkn, senderId, from, level);

    link_up(env.src);

    log_info("Connected to {} at {}:{} (total: {})", senderId, from.address().to_string(), from.port(), cur_connections_);
    choose_parent();

}

void Node::choose_parent() {
    if (is_root_) return;
    if (connections_.empty()) return;
    auto con = connections_.begin();
    while (con != connections_.end() && !con->second->connected) {
        ++con;
    }
    if (con == connections_.end()) return; // no connected peer yet
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
    log_debug("DADDY: {} (level {})", daddy_, minLevel);
}

void Node::on_data(const udp::endpoint& from, const proto::Envelope& env) {
    auto& b = env.body;
    auto data = proto::data_payload_bytes(env);
    if (!data) return;
    std::string msg(data->begin(), data->end());
    if (connections_.contains(env.src))
        connections_[env.src]->last_seen = Clock::now();
    else {
        log_warn("DATA from unknown sender: {}", env.src);
    }
    log_info("DATA from {}: {}", env.src, msg);
}

void Node::on_hop(const udp::endpoint& from, const proto::Envelope& env) {
    const std::string& raw_payload = env.body.at("payload").get<std::string>();
    const std::string circuit_id = env.body.value("circuit_id", "");

    // HOP.body["payload"] is always b64(OnionLayer_json)
    auto decoded = proto::b64_decode(raw_payload);
    if (!decoded) { log_error("[HOP] Outer b64_decode failed"); return; }

    std::string layer_json(decoded->begin(), decoded->end());
    proto::OnionLayer layer;
    try {
        layer = proto::onion_layer_from_json(json::parse(layer_json));
    } catch (const std::exception& e) {
        log_error("[HOP] Bad OnionLayer: {}", e.what());
        return;
    }

    // Record who sent this to us so HOP_REPLY can travel back
    if (!circuit_id.empty()) circuit_hop_table_[circuit_id] = env.src;

    if (layer.next_id.empty()) {
        // Terminal - deliver here
        auto inner = proto::b64_decode(layer.payload);
        if (!inner) { log_error("[HOP] Inner b64_decode failed"); return; }
        std::string inner_str(inner->begin(), inner->end());

        // Try to re-dispatch as a control message
        try {
            proto::Envelope inner_env = proto::parse_envelope(inner_str);
            dispatch(const_cast<udp::endpoint&>(from), inner_env);
        } catch (...) {
            // Try to parse as a circuit JSON control message
            try {
                const json j = json::parse(inner_str);
                const std::string type = j.value("type", "");
                if (type == "probe" && !circuit_id.empty()) {
                    const NodeId sender = j.at("src").get<std::string>();
                    received_circuit_by_src_[sender] = circuit_id;
                    log_info("[HOP] Circuit established from {}. Reply with: reply {} <message>",
                             sender, sender);
                } else {
                    log_info("[HOP] Delivered: {}", inner_str);
                }
            } catch (...) {
                log_info("[HOP] Delivered: {}", inner_str);
            }
        }
    } else {
        // Relay - forward payload to next hop (layer.payload is already b64 of next layer)
        auto it = connections_.find(layer.next_id);
        if (it == connections_.end() || !it->second || !it->second->connected) {
            log_warn("[HOP] No connection to next hop: {} - sending CIRCUIT_BROKEN", layer.next_id);
            if (!circuit_id.empty()) {
                auto back_it = circuit_hop_table_.find(circuit_id);
                if (back_it != circuit_hop_table_.end()) {
                    auto conn_it = connections_.find(back_it->second);
                    if (conn_it != connections_.end() && conn_it->second && conn_it->second->connected) {
                        const json broken = proto::msg_circuit_broken(circuit_id, node_id_);
                        send_text(conn_it->second->ep, proto::dump_compact(broken));
                        log_debug("[HOP] CIRCUIT_BROKEN for {} sent back to {}", circuit_id, back_it->second);
                    }
                }
            }
            return;
        }
        auto fwd = proto::msg_hop(layer.payload, node_id_, circuit_id);
        send_text(it->second->ep, proto::dump_compact(fwd));
    }
}

void Node::on_hop_reply(const udp::endpoint& from, const proto::Envelope& env) {
    const std::string circuit_id = env.body.at("circuit_id").get<std::string>();
    const std::string payload_b64 = env.body.at("payload").get<std::string>();

    // If we originated this circuit, deliver the reply
    if (circuits_.count(circuit_id)) {
        auto decoded = proto::b64_decode(payload_b64);
        if (!decoded) { log_error("[HOP_REPLY] b64_decode failed"); return; }
        std::string msg(decoded->begin(), decoded->end());
        log_info("[HOP_REPLY] [circuit={}] Reply received: {}", circuit_id, msg);
        return;
    }

    // We're a relay - forward backwards through the circuit
    auto it = circuit_hop_table_.find(circuit_id);
    if (it == circuit_hop_table_.end()) {
        log_warn("[HOP_REPLY] Unknown circuit_id: {}", circuit_id);
        return;
    }
    const NodeId& prev_hop = it->second;
    auto conn_it = connections_.find(prev_hop);
    if (conn_it == connections_.end() || !conn_it->second || !conn_it->second->connected) {
        log_warn("[HOP_REPLY] No connection to prev hop: {}", prev_hop);
        return;
    }
    const json fwd = proto::msg_hop_reply(circuit_id, payload_b64, node_id_);
    send_text(conn_it->second->ep, proto::dump_compact(fwd));
    log_debug("[HOP_REPLY] Forwarded circuit {} back to {}", circuit_id, prev_hop);
}

void Node::on_circuit_broken(const udp::endpoint& from, const proto::Envelope& env) {
    const std::string circuit_id = env.body.at("circuit_id").get<std::string>();

    // If we own this circuit, mark it failed and rebuild
    auto it = circuits_.find(circuit_id);
    if (it != circuits_.end()) {
        if (it->second.state == CircuitState::INITIATING) {
            log_debug("[CIRCUIT_BROKEN] Circuit {} already rebuilding", circuit_id);
            return;
        }
        const NodeId dst = it->second.path.back();
        it->second.state = CircuitState::INITIATING; // block duplicate CIRCUIT_BROKEN messages
        log_warn("[CIRCUIT {}] Broken in transit - rebuilding to {}", circuit_id, dst);
        circuits_.erase(it);
        begin_circuit_build(dst);
        return;
    }

    // We're a relay - propagate backward through the circuit
    auto back_it = circuit_hop_table_.find(circuit_id);
    if (back_it == circuit_hop_table_.end()) {
        log_warn("[CIRCUIT_BROKEN] Unknown circuit {}", circuit_id);
        return;
    }
    auto conn_it = connections_.find(back_it->second);
    if (conn_it == connections_.end() || !conn_it->second || !conn_it->second->connected) {
        log_warn("[CIRCUIT_BROKEN] Can't propagate - no connection to {}", back_it->second);
        return;
    }
    const json fwd = proto::msg_circuit_broken(circuit_id, node_id_);
    send_text(conn_it->second->ep, proto::dump_compact(fwd));
    log_debug("[CIRCUIT_BROKEN] Forwarded {} back to {}", circuit_id, back_it->second);
}

void Node::send_reply_via_circuit(const std::string& circuit_id, const std::string& data) {
    auto it = circuit_hop_table_.find(circuit_id);
    if (it == circuit_hop_table_.end()) {
        log_warn("[REPLY] No return path for circuit {}. Was a message received on this circuit?", circuit_id);
        return;
    }
    const NodeId& prev_hop = it->second;
    auto conn_it = connections_.find(prev_hop);
    if (conn_it == connections_.end() || !conn_it->second || !conn_it->second->connected) {
        log_warn("[REPLY] No connection to relay {}", prev_hop);
        return;
    }
    const std::string payload = proto::b64_encode(proto::to_bytes(data));
    const json msg = proto::msg_hop_reply(circuit_id, payload, node_id_);
    send_text(conn_it->second->ep, proto::dump_compact(msg));
    log_info("[REPLY] Sent reply on circuit {}", circuit_id);
}

void Node::on_disconnect(const udp::endpoint& from, const proto::Envelope& env) {
    remove_connection(env.src);
    link_down(env.src);
}

void Node::broadcast_graph_update(const NodeId& node) { // root only
    auto it = clients_map_.find(node);
    if (it == clients_map_.end()) return;
    const uint64_t seq = ++graph_seq_out_[node];
    const json msg = proto::msg_graph_update(node_id_, node, it->second.neighbors, seq);
    const std::string data = proto::dump_compact(msg);
    for (const auto& [id, conn] : connections_) {
        if (conn && conn->connected) send_text(conn->ep, data);
    }
    log_debug("[ROOT] Broadcast GRAPH_UPDATE for {} seq={}", node, seq);
}

void Node::send_graph_snapshot(const udp::endpoint& to) { // root only
    for (const auto& [id, entry] : clients_map_) {
        const uint64_t seq = ++graph_seq_out_[id];
        const json msg = proto::msg_graph_update(node_id_, id, entry.neighbors, seq, true);
        send_text(to, proto::dump_compact(msg));
    }
    log_info("[ROOT] Sent graph snapshot ({} nodes) to {}:{}", clients_map_.size(), to.address().to_string(), to.port());
}

void Node::on_graph_update(const udp::endpoint& from, const proto::Envelope& env) {
    const NodeId node     = env.body.at("node").get<std::string>();
    const uint64_t seq    = env.body.at("seq").get<uint64_t>();
    const json& neigh_arr = env.body.at("neighbors");
    const bool initial    = env.body.value("initial", false);

    // Dedup: drop if we've already seen this seq for this node
    if (auto it = graph_seq_.find(node); it != graph_seq_.end() && seq <= it->second) return;
    graph_seq_[node] = seq;

    // Upsert into local graph
    auto& entry = clients_map_[node];
    entry.peerId = node;
    entry.neighbors.clear();
    for (const auto& n : neigh_arr) entry.neighbors.insert(n.get<std::string>());

    log_info("[GRAPH] Updated {} neighbors={} seq={}", node, entry.neighbors.size(), seq);

    // Initial snapshot messages are point-to-point from root - don't re-flood
    if (initial) return;

    // Re-flood to all connections except the sender
    const std::string fwd = proto::dump_compact(
        proto::msg_graph_update(node_id_, node, entry.neighbors, seq));
    for (const auto& [id, conn] : connections_) {
        if (id != env.src && conn && conn->connected)
            send_text(conn->ep, fwd);
    }

    // Node has no more connections — remove it from local graph
    if (entry.neighbors.empty() && node != node_id_) {
        clients_map_.erase(node);
        log_debug("[GRAPH] Removed {} (no neighbors)", node);
    }
}

void Node::on_linkup(const udp::endpoint& from, const proto::Envelope& env) { //root function
    if (!is_root_) return;
    const NodeId src   = env.src;
    const NodeId neigh = env.body.at("peer").get<NodeId>();
    clients_map_[src].neighbors.insert(neigh);
    clients_map_[neigh].neighbors.insert(src);
    log_info("[ROOT] Link up: {} <-> {} (deg={}, {})", src, neigh,
             clients_map_[src].neighbors.size(), clients_map_[neigh].neighbors.size());
    broadcast_graph_update(src);
    broadcast_graph_update(neigh);
    if (neigh == node_id_) broadcast_graph_update(node_id_);
}

void Node::on_linkdown(const udp::endpoint& from, const proto::Envelope& env) { //root function
    if (!is_root_) return;
    const NodeId src   = env.src;
    const NodeId neigh = env.body.at("peer").get<NodeId>();
    auto src_it   = clients_map_.find(src);
    auto neigh_it = clients_map_.find(neigh);

    if (neigh_it != clients_map_.end()) clients_map_[neigh].neighbors.erase(src);
    if (src_it   != clients_map_.end()) clients_map_[src].neighbors.erase(neigh);

    // Broadcast updated state first, then erase locally if node has no more connections.
    // Broadcasting before erasing ensures other nodes learn the empty-neighbor state
    // and can prune the node from their own local graphs.
    if (auto it = clients_map_.find(neigh); it != clients_map_.end()) {
        broadcast_graph_update(neigh);
        if (it->second.neighbors.empty() && neigh != node_id_)
            clients_map_.erase(it);
    }
    if (auto it = clients_map_.find(src); it != clients_map_.end()) {
        broadcast_graph_update(src);
        if (it->second.neighbors.empty() && src != node_id_)
            clients_map_.erase(it);
    }
}

void Node::on_keepalive(const udp::endpoint& from, const proto::Envelope& env) {
    auto it = connections_.find(env.src);
    if (it != connections_.end() && it->second) {
        it->second->last_seen = Clock::now();
    } else {
        log_warn("KEEPALIVE from unknown/untracked: {}", env.src);
    }
}

void Node::on_chunk(const udp::endpoint& from, const proto::Envelope& env) {
    auto chunk_json = env.body;
    const std::string transfer_id = chunk_json.at("transfer_id");
    uint32_t index      = chunk_json.at("index");
    uint32_t chunk_num  = chunk_json.at("chunk_num");
    ContentType ct      = chunk_json.at("content_type");

    auto decoded = proto::b64_decode(chunk_json.at("payload").get<std::string>());
    if (!decoded) {
        log_warn("[CHUNK] b64_decode failed for transfer {} chunk {}", transfer_id, index);
        return;
    }

    auto& msg = incoming_msgs_[transfer_id];
    msg.chunk_num    = chunk_num;
    msg.content_type = ct;
    msg.src          = env.src;
    msg.chunks[index] = std::move(*decoded);

    log_debug("[CHUNK] {}/{} received for transfer {}", msg.chunks.size(), chunk_num, transfer_id);

    // Helper: collect missing indices and send NACK via circuit
    auto send_nack = [this, transfer_id, &msg]() {
        auto circuit_it = received_circuit_by_src_.find(msg.src);
        if (circuit_it == received_circuit_by_src_.end()) {
            log_warn("[CHUNK] Can't send NACK — no circuit from {}", msg.src);
            return;
        }
        std::vector<uint32_t> missing;
        for (uint32_t i = 0; i < msg.chunk_num; i++)
            if (!msg.chunks.count(i)) missing.push_back(i);
        if (missing.empty()) return;
        log_info("[CHUNK] Sending NACK for {} missing chunks of transfer {}", missing.size(), transfer_id);
        auto nack = proto::msg_chunk_nack(node_id_, transfer_id, missing);
        send_reply_via_circuit(circuit_it->second, proto::dump_compact(nack));
    };

    // On first chunk arrival, start a 10-second timeout timer
    if (!chunk_timers_.count(transfer_id)) {
        auto timer = std::make_unique<asio::steady_timer>(io_);
        timer->expires_after(std::chrono::seconds(10));
        timer->async_wait(
            [self = shared_from_this(), transfer_id](const std::error_code& ec) {
                if (ec) return; // cancelled
                auto it = self->incoming_msgs_.find(transfer_id);
                if (it == self->incoming_msgs_.end()) return; // already complete
                log_warn("[CHUNK] Transfer {} timed out — sending NACK", transfer_id);
                auto& msg = it->second;
                auto circuit_it = self->received_circuit_by_src_.find(msg.src);
                if (circuit_it == self->received_circuit_by_src_.end()) return;
                std::vector<uint32_t> missing;
                for (uint32_t i = 0; i < msg.chunk_num; i++)
                    if (!msg.chunks.count(i)) missing.push_back(i);
                if (missing.empty()) return;
                auto nack = proto::msg_chunk_nack(self->node_id_, transfer_id, missing);
                self->send_reply_via_circuit(circuit_it->second, proto::dump_compact(nack));
            });
        chunk_timers_[transfer_id] = std::move(timer);
    }

    // Check if all chunks have arrived
    if (msg.chunks.size() < chunk_num) return;

    // Cancel timeout — all chunks arrived
    if (auto it = chunk_timers_.find(transfer_id); it != chunk_timers_.end()) {
        it->second->cancel();
        chunk_timers_.erase(it);
    }

    // Reassemble in order
    std::vector<uint8_t> full_data;
    for (uint32_t i = 0; i < chunk_num; i++) {
        auto it = msg.chunks.find(i);
        if (it == msg.chunks.end()) {
            log_warn("[CHUNK] Transfer {} missing chunk {} despite full count — sending NACK", transfer_id, i);
            send_nack();
            return;
        }
        full_data.insert(full_data.end(), it->second.begin(), it->second.end());
    }

    log_info("[CHUNK] Transfer {} complete: {} bytes, type={}", transfer_id, full_data.size(), to_string(ct));

    // Deliver based on content type
    if (ct == TXT) {
        std::string text(full_data.begin(), full_data.end());
        log_info("[MSG from {}]: {}", msg.src, text);
    } else {
        // TODO: save to file for IMG/VID
        log_info("[FILE from {}]: {} bytes received", msg.src, full_data.size());
    }

    incoming_msgs_.erase(transfer_id);
    auto chunk_ack = proto::msg_chunk_ack(node_id_, transfer_id);
    auto circuit_it = received_circuit_by_src_.find(msg.src);
    if (circuit_it == received_circuit_by_src_.end()) return;
    send_reply_via_circuit(circuit_it->second, proto::dump_compact(chunk_ack));
}

void Node::on_chunk_ack(const udp::endpoint& from, const proto::Envelope& env) {
    const std::string transfer_id = env.body.at("transfer_id");
    outgoing_transfers_.erase(transfer_id);
    log_info("[CHUNK] Transfer {} acked, cache freed", transfer_id);
}

void Node::on_chunk_nack(const udp::endpoint& from, const proto::Envelope& env) {
    const std::string transfer_id = env.body.at("transfer_id");
    std::vector<uint32_t> missing = env.body.at("missing").get<std::vector<uint32_t>>();

    auto transfer_it = outgoing_transfers_.find(transfer_id);
    if (transfer_it == outgoing_transfers_.end()) {
        log_warn("[CHUNK_NACK] Unknown transfer {}", transfer_id);
        return;
    }

    auto circuit_it = circuit_by_dst_.find(transfer_it->second.dst);
    if (circuit_it == circuit_by_dst_.end()) {
        log_warn("[CHUNK_NACK] No circuit to {} for retransmit", transfer_it->second.dst);
        return;
    }

    log_info("[CHUNK_NACK] Retransmitting {}/{} missing chunks for transfer {}",
             missing.size(), transfer_it->second.total_chunks, transfer_id);

    const OutgoingTransfer& transfer = transfer_it->second;
    for (uint32_t i : missing) {
        if (i >= transfer.total_chunks) {
            log_warn("[CHUNK_NACK] Invalid chunk index {} in retransmit request", i);
            continue;
        }
        proto::ChunkMeta chunk{transfer_id, i, transfer.total_chunks,
                               transfer.content_type, proto::b64_encode(transfer.chunks[i])};
        auto msg = proto::msg_chunk(node_id_, chunk);
        send_via_circuit(circuit_it->second, proto::dump_compact(msg));
    }
}


void Node::link_up(const NodeId& peer) {
    if (linked_up_.contains(peer)) return;
    linked_up_.insert(peer);
    cur_connections_++;
    prune_connections();
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
        log_error("keep_alive called for unknown peer: {}", peerId);
        return;
    }
    Connection& conn = *it->second;

    udp::endpoint ep = conn.ep;
    json j = proto::msg_keepalive(node_id_);
    auto data = proto::dump_compact(j);

    log_debug("Sent KEEPALIVE to {}", ep.address().to_string());

    send_text(ep, data);

    conn.ka_timer.expires_after(std::chrono::milliseconds(conn.ka_ms));
    conn.ka_timer.async_wait([self = shared_from_this(), peerId](const std::error_code ec) {
        if (ec == asio::error::operation_aborted) return;
        if (ec) {
            log_error("KA timer error for {}: {}", peerId, ec.message());
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
            log_error("Prune timer error: {}", ec.message());
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
        log_warn("Peer {} expired - disconnecting", id);
        dynamic_disconnect(id);
    }
}

void Node::print_graph() { //root function
    std::cout << "\n==========  GRAPH  ==========\n";
    std::cout << "\nLevel: " << level_ << "\n";
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

    for (const auto& [u, pu] : connections_) {
        std::cout << u << " --> " << pu->ep.address().to_string() << ":" << pu->ep.port() << "\n";
    }
    std::cout << "================================\n\n";
}

void Node::prune_connections() {
    if (cur_connections_ >= MAX_CONNS) {
        log_info("Connection limit reached ({}) - dropping one peer", cur_connections_);
        rand_disconnect();
    }
}


void Node::rand_disconnect() {
    // disconnecting from a random connection because the connection limit has been exceeded.
    // daddy_ is excluded - dropping the parent would orphan us.
    std::vector<NodeId> candidates;
    for (auto& [id, _] : connections_) {
        if (id != daddy_) candidates.push_back(id);
    }
    if (candidates.empty()) return;
    dynamic_disconnect(candidates[random_0_to_n(static_cast<int>(candidates.size()))]);
}

void Node::dynamic_disconnect(const NodeId& id) {
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
    if (cur_connections_ > MIN_CONNS) cur_connections_--;
    else request_conns();

    auto par = connections_.find(daddy_);
    if (par == connections_.end())
        choose_parent();

    link_down(peerId);

    invalidate_circuits_through(peerId);

    log_info("Removed peer {} (disconnected or replaced)", peerId);
}


// ------------------------ onion routing ------------------------

void Node::request_introduce(const NodeId& target_id) {
    const json req = proto::msg_introduce_req(node_id_, target_id);
    if (is_root_) {
        // We are root - dispatch directly without a UDP round-trip
        const proto::Envelope env = proto::parse_envelope(proto::dump_compact(req));
        on_introduce_req(ep_, env);
    } else {
        send_text(root_ep_, proto::dump_compact(req));
    }
}

void Node::on_introduce_req(const udp::endpoint& from, const proto::Envelope& env) {
    if (!is_root_) return;
    const NodeId requester_id = env.src;
    const NodeId target_id    = env.body.at("target_id").get<std::string>();

    auto req_it = clients_map_.find(requester_id);
    auto tgt_it = clients_map_.find(target_id);
    if (req_it == clients_map_.end() || tgt_it == clients_map_.end()) {
        log_warn("[ROOT] INTRODUCE_REQ for unknown peer: {}", target_id);
        return;
    }

    const std::string token = proto::random_token_hex();
    PeerInfo& req_peer = req_it->second;
    PeerInfo& tgt_peer = tgt_it->second;

    // Tell requester about target (requester will punch target)
    req_peer.tkn = token;
    tgt_peer.tkn = token;
    const json intro_to_req = proto::msg_introduce(tgt_peer, token);
    send_text(req_peer.ep, proto::dump_compact(intro_to_req));

    // Tell target about requester (target will punch requester) - simultaneous open
    const json intro_to_tgt = proto::msg_introduce(req_peer, token);
    send_text(tgt_peer.ep, proto::dump_compact(intro_to_tgt));

    log_info("[ROOT] Introduced {} <-> {}", requester_id, target_id);
}

void Node::begin_circuit_build(const NodeId& dst) {
    // Scale relays with graph size: each relay needs a distinct node (src + relays + dst)
    const int num_relays = std::clamp(static_cast<int>(clients_map_.size()) - 2, MIN_RELAYS, MAX_RELAYS);
    log_info("[CIRCUIT] Building circuit to {} (graph_size={}, relays={})", dst, clients_map_.size(), num_relays);
    auto path_opt = compute_route(node_id_, dst, num_relays + 1);
    if (!path_opt) {
        log_warn("[CIRCUIT] compute_route failed - graph too small or {} unreachable", dst);
        return;
    }

    // path = [src, relay1, ..., dst]; circuit path excludes src
    std::vector<NodeId> circuit_path(path_opt->begin() + 1, path_opt->end());

    if (circuit_path.empty()) {
        log_warn("[CIRCUIT] Computed path is empty");
        return;
    }

    Circuit c;
    c.circuit_id = proto::random_tx_id();
    c.path       = std::move(circuit_path);
    c.state      = CircuitState::READY;
    circuits_[c.circuit_id] = c;
    circuit_by_dst_[dst]    = c.circuit_id;

    std::string path_str = node_id_;
    for (const auto& hop : c.path) path_str += " -> " + hop;
    log_info("[CIRCUIT {}] READY - path: {}", c.circuit_id, path_str);
    log_info("[CIRCUIT {}] Use: sanon {} <message>", c.circuit_id, c.path.back());

    // Send a probe through the circuit so relays populate their circuit_hop_table_
    // and dst learns who to reply to.
    const std::string probe_payload = json{{"type", "probe"}, {"src", node_id_}}.dump(-1);
    const auto probe_onion = proto::build_onion(c.path, probe_payload);
    const std::string probe_json = proto::dump_compact(probe_onion);
    const std::string hop_payload = proto::b64_encode(proto::to_bytes(probe_json));
    const json hop = proto::msg_hop(hop_payload, node_id_, c.circuit_id);
    send_text(connections_.at(c.path[0])->ep, proto::dump_compact(hop));
    log_debug("[CIRCUIT {}] Probe sent to populate relay tables", c.circuit_id);
}

void Node::invalidate_circuits_through(const NodeId& peer_id) {
    std::vector<std::pair<std::string, NodeId>> to_rebuild; // {circuit_id, dst}
    for (auto& [cid, circ] : circuits_) {
        if (circ.state != CircuitState::READY) continue;
        if (std::ranges::find(circ.path, peer_id) != circ.path.end()) {
            circ.state = CircuitState::FAILED;
            to_rebuild.emplace_back(cid, circ.path.back());
        }
    }
    for (auto& [old_cid, dst] : to_rebuild) {
        log_warn("[CIRCUIT {}] Relay {} lost - rebuilding to {}", old_cid, peer_id, dst);
        circuits_.erase(old_cid);
        begin_circuit_build(dst);
    }
}

void Node::send_via_circuit(const std::string& circuit_id, const std::string& data) {
    auto it = circuits_.find(circuit_id);
    if (it == circuits_.end() || it->second.state != CircuitState::READY) {
        log_warn("[CIRCUIT] {} not found or not READY", circuit_id);
        return;
    }

    const NodeId dst = it->second.path.back();
    const auto& path = it->second.path;

    auto first_hop = connections_.find(path[0]);
    if (first_hop == connections_.end() || !first_hop->second) {
        log_warn("[CIRCUIT {}] First hop {} gone - rebuilding", circuit_id, path[0]);
        circuits_.erase(it);
        begin_circuit_build(dst);
        return;
    }

    const json onion = proto::build_onion(path, data);
    const std::string onion_json = proto::dump_compact(onion);
    const std::string hop_payload = proto::b64_encode(proto::to_bytes(onion_json));
    const json hop = proto::msg_hop(hop_payload, node_id_, circuit_id);
    send_text(first_hop->second->ep, proto::dump_compact(hop));
    log_debug("[CIRCUIT {}] Sent data via circuit", circuit_id);
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
    std::ranges::reverse(path.begin(), path.end());
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

    // Force at least one intermediate node by starting BFS from u's neighbors (skipping v).
    // Add u itself to forbidden so the sub-BFS can't loop back through it.
    forbidden.insert(u);
    auto u_it = clients_map_.find(u);
    if (u_it == clients_map_.end()) return false;

    std::vector<NodeId> best;
    for (const auto& w : u_it->second.neighbors) {
        if (w == v || forbidden.contains(w)) continue;
        auto sub = bfs_shortest_path(w, v, forbidden);
        if (!sub) continue;
        // sub = [w, ..., v]; full detour = [u, w, ..., v]
        if (best.empty() || sub->size() < best.size()) best = *sub;
    }
    if (best.empty()) return false;

    // Splice: replace [u, v] with [u, w, ..., v]
    std::vector<NodeId> out;
    out.reserve(path.size() + best.size());

    // prefix includes u
    out.insert(out.end(), path.begin(), path.begin() + static_cast<long long>(edge_i) + 1);

    // middle: w..(v excluded — v is the start of the suffix)
    out.insert(out.end(), best.begin(), best.end() - 1);

    // suffix starts at v (path[edge_i+1])
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

    if (hops() < min_hops)
        log_warn("[CIRCUIT] Graph topology only allows {} hops (wanted {}) - using best available path", hops(), min_hops);

    return path;
}