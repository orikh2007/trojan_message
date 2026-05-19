//
// Created by orikh on 22/12/2025.
//

#include "../headers/Node.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../extern_libs/stb_image_write.h"


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
      register_timer_(io_),
      clients_map_(), is_admin_(false)
{
    log_debug("Node init started");
    std::error_code ec;

    Logger::get().set_level(LogLevel::DEBUG);

    node_id_ = proto::random_node_id_hex();

    log_info("Node ID: {}", node_id_);

    socket_.open(udp::v4(), ec);
    if (ec)
    {
        io_.stop();
        throw std::runtime_error("socket_.open failed: " + ec.message());
    }

    socket_.bind(udp::endpoint(udp::v4(), listen_port), ec);
    if (ec)
    {
        io_.stop();
        throw std::runtime_error("socket_.bind failed (port in use?): " + ec.message());
    }
    log_info("Listening on UDP port {}", listen_port);
    shell_.start_shell();
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
    register_timer_.cancel();
    register_retries_ = 0;
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
    designate_heir();
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
                if (!circuit_by_dst_.contains(dst)) log_warn("No ready circuit to {}. Run 'circuit {}' first.", dst, dst);
                else {
                    send_chunked(dst, proto::to_bytes(msg), TXT);
                    {
                        ChatMessage cm;
                        cm.from = node_id_;
                        cm.to   = dst;
                        cm.text = msg;
                        cm.when = std::chrono::system_clock::now();
                        std::lock_guard lock(chat_mutex_);
                        chat_log_.push_back(std::move(cm));
                    }
                }
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

NodeSnapshot Node::get_snapshot() const {
    std::lock_guard lock(gui_mutex_);
    return snapshot_;
}

std::vector<ChatMessage> Node::take_new_messages() {
    std::lock_guard lock(chat_mutex_);
    std::vector<ChatMessage> out(chat_log_.begin(), chat_log_.end());
    chat_log_.clear();
    return out;
}

std::vector<ShellOut> Node::take_shell_outs() {
    std::lock_guard lock(shell_mutex_);
    std::vector<ShellOut> outs = shell_outs_;
    shell_outs_.clear();
    return outs;
}

void Node::send_shell_cmd(std::string cmd, NodeId to) {
    asio::post(io_, [self = shared_from_this(), cmd = std::move(cmd), to = std::move(to)]() {
        if (!self->circuit_by_dst_.contains(to)) {
            log_warn("[ADMIN] No circuit to {}", to);
            return;
        }
        self->send_chunked(to, proto::to_bytes(cmd), SHELL_CMD);
    });
}
void Node::send_scrsht_cmd(NodeId to) {
    std::string cmd = "null";
    asio::post(io_, [self = shared_from_this(), cmd = std::move(cmd), to = std::move(to)]() {
        if (!self->circuit_by_dst_.contains(to)) {
            log_warn("[ADMIN] No circuit to {}", to);
            return;
        }
        self->send_chunked(to, proto::to_bytes(cmd), SCRSHT);
    });
}

void Node::set_shell_out_callback(std::function<void(ShellOut)> cb) {
    std::lock_guard lock(shell_mutex_);
    shell_out_cb_ = std::move(cb);
}

void Node::set_scrsht_out_callback(std::function<void(std::vector<uint8_t>)> cb) {
    std::lock_guard lock(shell_mutex_);
    scrsht_out_cb_ = std::move(cb);
}

std::vector<NodeId> Node::get_known_clients() const {
    std::lock_guard lock(gui_mutex_);
    return snapshot_.known_peers;
}

bool Node::has_ready_circuit(const NodeId& dst) const {
    std::lock_guard lock(gui_mutex_);
    for (const auto& c : snapshot_.circuits)
        if (c.dst == dst && c.state == CircuitState::READY)
            return true;
    return false;
}

void Node::set_admin()
{
    is_admin_ = true;
}

void Node::reply_chunked(const NodeId& src, std::vector<uint8_t> data, ContentType content_type) {
    auto circuit_it = received_circuit_by_src_.find(src);
    if (circuit_it == received_circuit_by_src_.end()) {
        log_warn("[CHUNK] No received circuit from {} to reply on", src);
        return;
    }
    const auto& circuit_id = circuit_it->second;

    auto transfer_id = proto::random_tx_id();
    uint32_t total_chunks = std::max(1u, static_cast<uint32_t>((data.size() + CHUNK_SIZE - 1) / CHUNK_SIZE));

    OutgoingTransfer transfer;
    transfer.dst          = src;
    transfer.content_type = content_type;
    transfer.total_chunks = total_chunks;
    transfer.circuit_id   = circuit_id;

    for (uint32_t i = 0; i < total_chunks; i++) {
        size_t start = i * CHUNK_SIZE;
        size_t end   = std::min(start + CHUNK_SIZE, data.size());
        transfer.chunks.emplace_back(data.begin() + start, data.begin() + end);
    }

    outgoing_transfers_[transfer_id] = std::move(transfer);
    auto xfer_timer = std::make_unique<asio::steady_timer>(io_);
    xfer_timer->expires_after(outgoing_transfer_timeout_sec);
    xfer_timer->async_wait([self = shared_from_this(), transfer_id](const std::error_code& ec) {
        if (ec) return;
        if (self->outgoing_transfers_.erase(transfer_id))
            log_warn("[CHUNK] Reply transfer {} sender-timeout — cache freed", transfer_id);
        self->outgoing_transfer_timers_.erase(transfer_id);
    });
    outgoing_transfer_timers_[transfer_id] = std::move(xfer_timer);

    log_info("[CHUNK] Replied {} chunks (transfer {}) to {}", total_chunks, transfer_id, src);
    schedule_chunk_sender(transfer_id, 0);
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
    case MsgType::YOU_ARE_HEIR:
        on_you_are_heir(from, env);
        break;
    case MsgType::NEW_ROOT:
        on_new_root(from, env);
        break;
    case MsgType::GRAPH_RESET: {
        std::vector<NodeId> stale;
        for (const auto& [id, _] : clients_map_)
            if (id != node_id_) stale.push_back(id);
        for (const auto& id : stale) clients_map_.erase(id);
        graph_seq_.clear();
        update_snapshot();
        break;
    }
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
        update_snapshot();

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
    register_timer_.cancel();
    register_retries_ = 0;
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

void Node::on_disconnect(const udp::endpoint& from, const proto::Envelope& env) {
    remove_connection(env.src);
    link_down(env.src);
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
    constexpr int MAX_RETRIES = 3;
    constexpr auto RETRY_TIMEOUT = std::chrono::seconds(3);

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

    if (register_retries_ >= MAX_RETRIES) {
        log_warn("HANDLE_REGISTER: no response after {} attempts, giving up", MAX_RETRIES);
        register_retries_ = 0;
        return;
    }

    register_retries_++;
    const auto j = proto::msg_register(node_id_, socket_.local_endpoint().port(), 3);
    const auto data = proto::dump_compact(j);
    send_text(root_ep_, data);
    log_info("Sent REGISTER to {}:{}", root_ip_, ROOT_PORT);


    register_timer_.expires_after(RETRY_TIMEOUT);
    register_timer_.async_wait([self = shared_from_this()](const std::error_code& ec) {
        if (ec == asio::error::operation_aborted) return;
        if (ec) { log_error("register_timer error: {}", ec.message()); return; }
        self->handle_register();
    });
} //register with the root - send it node id and open port

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
        const auto& entry = clients_map_.at(cli);
        if (entry.ep.address().is_unspecified()) continue; // no valid endpoint (learned via GRAPH_UPDATE only)
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

void Node::request_conns() {
    constexpr int MAX_RETRIES = 3;
    constexpr auto RETRY_TIMEOUT = std::chrono::seconds(3);

    if (register_retries_ >= MAX_RETRIES) {
        log_warn("REQ_CONNS: no response after {} attempts, giving up", MAX_RETRIES);
        register_retries_ = 0;
        return;
    }

    register_retries_++;
    const auto j    = proto::msg_req_conns(node_id_, port_);
    const auto data = proto::dump_compact(j);
    send_text(root_ep_, data);
    log_debug("Sent REQ_CONNS to root (attempt {}/{})", register_retries_, MAX_RETRIES);

    register_timer_.expires_after(RETRY_TIMEOUT);
    register_timer_.async_wait([self = shared_from_this()](const std::error_code& ec) {
        if (ec == asio::error::operation_aborted) return;
        if (ec) { log_error("req_conns_timer error: {}", ec.message()); return; }
        self->request_conns();
    });
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

void Node::schedule_chunk_sender(std::string transfer_id, uint32_t index) {
    auto it = outgoing_transfers_.find(transfer_id);
    if (it == outgoing_transfers_.end()) return;
    auto& transfer = it->second;
    if (index >= transfer.total_chunks) return;

    // Send this chunk
    auto& raw = transfer.chunks[index];
    proto::ChunkMeta meta{transfer_id, index, transfer.total_chunks, transfer.content_type, proto::b64_encode(raw)};
    auto msg = proto::dump_compact(proto::msg_chunk(node_id_, meta));

    if (transfer.circuit_id.empty()) {
        auto cit = circuit_by_dst_.find(transfer.dst);
        if (cit == circuit_by_dst_.end()) {
            log_warn("[CHUNK] Circuit to {} gone during send of transfer {}", transfer.dst, transfer_id);
            return;
        }
        send_via_circuit(cit->second, msg);
    } else {
        send_reply_via_circuit(transfer.circuit_id, msg);
    }

    if (index + 1 >= transfer.total_chunks) return;

    // Schedule next chunk after a short delay to pace the stream
    auto timer = std::make_shared<asio::steady_timer>(io_);
    timer->expires_after(chunk_send_interval_ms);
    timer->async_wait([self = shared_from_this(), transfer_id, index, timer](const std::error_code& ec) mutable {
        if (ec) return;
        self->schedule_chunk_sender(std::move(transfer_id), index + 1);
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
        transfer.chunks.emplace_back(data.begin() + start, data.begin() + end);
    }

    outgoing_transfers_[transfer_id] = std::move(transfer);

    auto xfer_timer = std::make_unique<asio::steady_timer>(io_);
    xfer_timer->expires_after(outgoing_transfer_timeout_sec);
    xfer_timer->async_wait([self = shared_from_this(), transfer_id](const std::error_code& ec) {
        if (ec) return;
        if (self->outgoing_transfers_.erase(transfer_id))
            log_warn("[CHUNK] Transfer {} sender-timeout — cache freed", transfer_id);
        self->outgoing_transfer_timers_.erase(transfer_id);
    });
    outgoing_transfer_timers_[transfer_id] = std::move(xfer_timer);

    log_info("[CHUNK] Sending {} chunks for transfer {}", total_chunks, transfer_id);
    schedule_chunk_sender(transfer_id, 0);
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
    update_snapshot();
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
    update_snapshot();
}

void Node::on_keepalive(const udp::endpoint& from, const proto::Envelope& env) {
    auto it = connections_.find(env.src);
    if (it != connections_.end() && it->second) {
        it->second->last_seen = Clock::now();
    } else {
        log_warn("KEEPALIVE from unknown/untracked: {}", env.src);
    }
}

void Node::on_chunk(const udp::endpoint& from, const proto::Envelope& env)
{
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
        std::vector<uint32_t> missing;
        for (uint32_t i = 0; i < msg.chunk_num; i++)
            if (!msg.chunks.count(i)) missing.push_back(i);
        if (missing.empty()) return;
        log_info("[CHUNK] Sending NACK for {} missing chunks of transfer {}", missing.size(), transfer_id);
        auto nack_msg = proto::dump_compact(proto::msg_chunk_nack(node_id_, transfer_id, missing));
        auto reply_it = received_circuit_by_src_.find(msg.src);
        auto dst_it   = circuit_by_dst_.find(msg.src);
        if (reply_it != received_circuit_by_src_.end())
            send_reply_via_circuit(reply_it->second, nack_msg);
        else if (dst_it != circuit_by_dst_.end())
            send_via_circuit(dst_it->second, nack_msg);
        else
            log_warn("[CHUNK] Can't send NACK — no circuit to {}", msg.src);
    };

    // On first chunk arrival, arm the recurring NACK retry timer
    if (!chunk_timers_.contains(transfer_id)) {
        chunk_timers_[transfer_id] = std::make_unique<asio::steady_timer>(io_);

        auto retry_fn = std::make_shared<std::function<void(const std::error_code&)>>();
        *retry_fn = [self = shared_from_this(), transfer_id, retry_fn](const std::error_code& ec) mutable {
            if (ec) return; // cancelled — completed or timer destroyed

            auto msg_it = self->incoming_msgs_.find(transfer_id);
            if (msg_it == self->incoming_msgs_.end()) return; // already completed
            auto& rmsg = msg_it->second;

            if (rmsg.nack_retries >= MAX_NACK_RETRIES) {
                log_warn("[CHUNK] Transfer {} gave up after {} retries — dropping", transfer_id, MAX_NACK_RETRIES);
                self->incoming_msgs_.erase(transfer_id);
                self->chunk_timers_.erase(transfer_id);
                return;
            }

            std::vector<uint32_t> missing;
            for (uint32_t i = 0; i < rmsg.chunk_num; i++)
                if (!rmsg.chunks.count(i)) missing.push_back(i);

            if (!missing.empty()) {
                auto reply_it = self->received_circuit_by_src_.find(rmsg.src);
                auto dst_it   = self->circuit_by_dst_.find(rmsg.src);
                if (reply_it == self->received_circuit_by_src_.end() &&
                    dst_it   == self->circuit_by_dst_.end()) {
                    log_warn("[CHUNK] Transfer {} — no circuit to send NACK, dropping", transfer_id);
                    self->incoming_msgs_.erase(transfer_id);
                    self->chunk_timers_.erase(transfer_id);
                    return;
                }
                rmsg.nack_retries++;
                log_info("[CHUNK] NACK #{} — {}/{} missing for transfer {}",
                         rmsg.nack_retries, missing.size(), rmsg.chunk_num, transfer_id);
                auto nack_msg = proto::dump_compact(proto::msg_chunk_nack(self->node_id_, transfer_id, missing));
                if (reply_it != self->received_circuit_by_src_.end())
                    self->send_reply_via_circuit(reply_it->second, nack_msg);
                else
                    self->send_via_circuit(dst_it->second, nack_msg);
            }

            // Reschedule for next retry
            auto t_it = self->chunk_timers_.find(transfer_id);
            if (t_it != self->chunk_timers_.end()) {
                t_it->second->expires_after(std::chrono::seconds(10));
                t_it->second->async_wait(*retry_fn);
            }
        };

        chunk_timers_[transfer_id]->expires_after(std::chrono::seconds(10));
        chunk_timers_[transfer_id]->async_wait(*retry_fn);
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
        {
            ChatMessage cm;
            cm.from = msg.src;
            cm.to   = node_id_;
            cm.text = text;
            cm.when = std::chrono::system_clock::now();
            std::lock_guard lock(chat_mutex_);
            chat_log_.push_back(std::move(cm));
        }
    } else if (ct == SHELL_CMD) {
        if (is_admin_) return;
        std::string cmd(full_data.begin(), full_data.end());
        if (!cmd.empty())
        {
            std::string out = shell_.run(cmd);
            if (out.empty())
                return;
            std::string cwd = shell_.run("cd");
            auto se = proto::msg_shell_out(node_id_, out, cwd);
            reply_chunked(msg.src, proto::to_bytes(proto::dump_compact(se)), SHELL_OUT);
        }
    } else if (ct == SHELL_OUT)
    {
        std::string exec_str(full_data.begin(), full_data.end());
        if (exec_str.empty()) {log_error("Got empty out from shell command"); return;}
        auto exec_j = json::parse(exec_str);
        ShellOut exec {
            exec_j["from"].get<std::string>(),
            exec_j["out"].get<std::string>(),
            exec_j["cwd"].get<std::string>()
        };
        std::function<void(ShellOut)> cb;
        {
            std::lock_guard lock(shell_mutex_);
            shell_outs_.push_back(exec);
            cb = shell_out_cb_;
        }
        if (cb) cb(std::move(exec));
    }
    else if (ct == SCRSHT){
        int w, h;
        auto screen_pixels = take_screenshot(w, h);
        auto png = to_png(screen_pixels, w, h);
        auto png_msg = proto::msg_scrsht(node_id_, png, w, h);
        reply_chunked(msg.src, proto::to_bytes(proto::dump_compact(png_msg)), SCRSHT_OUT);
    }
    else if (ct == SCRSHT_OUT) {
        std::string exec_str(full_data.begin(), full_data.end());
        if (exec_str.empty()) {log_error("Got empty out from shell command"); return;}
        auto exec_j = json::parse(exec_str);
        int h = exec_j["h"], w = exec_j["w"];
        auto img_decoded = proto::b64_decode(exec_j["out"].get<std::string>());
        if (!img_decoded) { log_error("Failed to decode screenshot PNG data"); return; }
        std::vector<uint8_t> image_vec = std::move(*img_decoded);
        std::function<void(std::vector<uint8_t>)> cb;
        {
            std::lock_guard lock(scrsht_mutex_);
            scrsht_outs_.push_back(image_vec);
            cb = scrsht_out_cb_;
        }
        if (cb) cb(std::move(image_vec));

    }
    else {
        log_info("[FILE from {}]: {} bytes received", msg.src, full_data.size());
    }

    const NodeId src = msg.src;  // save before erase invalidates the reference
    incoming_msgs_.erase(transfer_id);
    auto chunk_ack = proto::msg_chunk_ack(node_id_, transfer_id);
    auto circuit_it = received_circuit_by_src_.find(src);
    if (circuit_it == received_circuit_by_src_.end()) return;
    send_reply_via_circuit(circuit_it->second, proto::dump_compact(chunk_ack));
}

void Node::on_chunk_ack(const udp::endpoint& from, const proto::Envelope& env) {
    const std::string transfer_id = env.body.at("transfer_id");
    outgoing_transfers_.erase(transfer_id);
    if (auto it = outgoing_transfer_timers_.find(transfer_id); it != outgoing_transfer_timers_.end()) {
        it->second->cancel();
        outgoing_transfer_timers_.erase(it);
    }
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

    const OutgoingTransfer& transfer = transfer_it->second;

    log_info("[CHUNK_NACK] Retransmitting {}/{} missing chunks for transfer {}",
             missing.size(), transfer.total_chunks, transfer_id);

    for (uint32_t i : missing) {
        if (i >= transfer.total_chunks) {
            log_warn("[CHUNK_NACK] Invalid chunk index {} in retransmit request", i);
            continue;
        }
        proto::ChunkMeta chunk{transfer_id, i, transfer.total_chunks,
            transfer.content_type, proto::b64_encode(transfer.chunks[i])};
        auto msg = proto::dump_compact(proto::msg_chunk(node_id_, chunk));

        if (!transfer.circuit_id.empty()) {
            // Reply transfer — use the stored reply circuit
            send_reply_via_circuit(transfer.circuit_id, msg);
        } else {
            // Initiated transfer — route forward through circuit_by_dst_
            auto circuit_it = circuit_by_dst_.find(transfer.dst);
            if (circuit_it == circuit_by_dst_.end()) {
                log_warn("[CHUNK_NACK] No circuit to {} for retransmit", transfer.dst);
                return;
            }
            send_via_circuit(circuit_it->second, msg);
        }
    }
}

void Node::link_up(const NodeId& peer) {
    if (linked_up_.contains(peer)) return;
    linked_up_.insert(peer);
    update_snapshot();
    if (is_root_ && !has_heir_) designate_heir();
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
        send_text(root_ep_, proto::dump_compact(data));
        return;
    }
    // Root handles directly — mirrors on_linkdown logic (src=node_id_, neigh=peer)
    auto n_it = clients_map_.find(peer);
    auto s_it = clients_map_.find(node_id_);
    if (n_it != clients_map_.end()) n_it->second.neighbors.erase(node_id_);
    if (s_it != clients_map_.end()) s_it->second.neighbors.erase(peer);
    if (n_it != clients_map_.end()) {
        broadcast_graph_update(peer);
        if (n_it->second.neighbors.empty())
            clients_map_.erase(n_it);
    }
    if (s_it != clients_map_.end())
        broadcast_graph_update(node_id_);
    update_snapshot();
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
        self->prune_tokens();
        self->prune_dead();

        // Ghost circuit detection: READY circuits where a reply is overdue
        const auto now = Clock::now();
        std::vector<std::pair<std::string, NodeId>> ghost_circuits;
        for (const auto& [cid, circ] : self->circuits_) {
            if (circ.state != CircuitState::READY) continue;
            if (circ.last_used == Clock::time_point{}) continue;  // never used
            if (circ.last_ack >= circ.last_used) continue;        // got a reply after last send — healthy
            if (now - circ.last_used > circuit_dead_sec)          // been waiting 30s with no reply — dead
                ghost_circuits.emplace_back(cid, circ.path.back());
        }
        for (auto& [cid, dst] : ghost_circuits) {
            log_warn("[CIRCUIT {}] No reply in {}s - presumed dead, rebuilding to {}",
                     cid, circuit_dead_sec.count(), dst);
            self->cancel_circuit_timer(cid);
            if (auto kp_it = self->circuit_pending_kp_.find(cid); kp_it != self->circuit_pending_kp_.end()) {
                for (auto* kp : kp_it->second) crypto::free_keypair(kp);
                self->circuit_pending_kp_.erase(kp_it);
            }
            self->circuit_keys_.erase(cid);
            self->circuits_.erase(cid);
            self->begin_circuit_build(dst);
        }

        // reschedule
        self->prune_tick();
    });
}

void Node::prune_connections() {
    if (cur_connections_ >= MAX_CONNS) {
        log_info("Connection limit reached ({}) - dropping one peer", cur_connections_);
        rand_disconnect();
    }
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
    update_snapshot();
    linked_up_.erase(peerId);

    // Heir takeover must happen BEFORE handle_register/choose_parent so we
    // don't send REGISTER to an address we're about to rebind to ourselves.
    if (is_heir_ && peerId == heired_from_) {
        if (cur_connections_ > MIN_CONNS) cur_connections_--;
        take_over_as_root();
        link_down(peerId);
        invalidate_circuits_through(peerId);
        log_info("Removed peer {} (heir takeover)", peerId);
        return;
    }

    if (cur_connections_ > MIN_CONNS) cur_connections_--;
    else handle_register();

    auto par = connections_.find(daddy_);
    if (par == connections_.end())
        choose_parent();

    if (is_root_ && peerId == heir_id_) {
        heir_id_.clear();
        has_heir_ = false;
        designate_heir();
    }

    link_down(peerId);
    invalidate_circuits_through(peerId);
    log_info("Removed peer {} (disconnected or replaced)", peerId);
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

    if (layer.next_id.empty()) {
        // Terminal (exit node) — record who sent this so replies can travel back
        if (!circuit_id.empty()) {
            auto& entry = circuit_hop_table_[circuit_id];
            if (entry.prev_hop.empty()) {
                entry.prev_hop = env.src;
                entry.out_cid  = "";   // exit doesn't forward; no out_cid
            }
        }
        if (layer.circuit_init && !circuit_id.empty()) {
            auto kp = crypto::gen_keypair();
            auto secret = crypto::hkdf_sha256(crypto::derive_secret(kp, layer.src_pub), circuit_id);
            circuit_keys_[circuit_id] = std::vector{secret};
            circuit_pending_kp_[circuit_id] = std::vector{kp};
            const auto my_pub = crypto::get_pubkey(kp);
            const std::vector<uint8_t> my_pub_vec(my_pub.begin(), my_pub.end());
            auto ack_payload_json = json{
                {"type","probe_ack"},
                {"keys",json::array({proto::b64_encode(my_pub_vec)})}}.dump(-1);
            auto ack_b64 = proto::b64_encode(proto::to_bytes(ack_payload_json));
            const NodeId& prev = circuit_hop_table_[circuit_id].prev_hop;
            auto ack = proto::msg_hop_reply(circuit_id, ack_b64, node_id_);
            auto ret_it = connections_.find(prev);
            if (ret_it == connections_.end()) {
                log_error("[DST_HOP] Can't return probe — no connection to {}", prev);
                return;
            }
            send_text(ret_it->second->ep, proto::dump_compact(ack));
        }
        std::string deliver_payload = layer.payload;
        if (!layer.circuit_init && !circuit_id.empty()) {
            if (auto ck_find = circuit_keys_.find(circuit_id); ck_find != circuit_keys_.end()) {
                auto enc = proto::b64_decode(layer.payload);
                if (!enc) {
                    log_error("[HOP] failed to decode payload");
                    return;
                }
                auto dec = crypto::decrypt(ck_find->second[0], *enc);
                if (!dec) {
                    log_error("[HOP] failed to decrypt payload");
                    return;
                }
                deliver_payload = proto::b64_encode(*dec);
            }
        }
        auto inner = proto::b64_decode(deliver_payload);
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
                    // Clean up the old circuit's state before registering the replacement
                    if (auto old_it = received_circuit_by_src_.find(sender);
                        old_it != received_circuit_by_src_.end() && old_it->second != circuit_id)
                        cleanup_relay_circuit(old_it->second);
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
        // Relay — populate the CID-switching table on first sight of this circuit
        std::string out_cid;
        if (!circuit_id.empty()) {
            auto& entry = circuit_hop_table_[circuit_id];
            if (entry.prev_hop.empty()) {
                entry.prev_hop = env.src;
                // Use the initiator-assigned next_cid if present; otherwise generate one
                entry.out_cid = !layer.next_cid.empty()
                                    ? layer.next_cid
                                    : proto::random_tx_id();
                circuit_reply_table_[entry.out_cid] = circuit_id;
            }
            out_cid = entry.out_cid;
        }

        auto it = connections_.find(layer.next_id);
        if (it == connections_.end() || !it->second || !it->second->connected) {
            log_warn("[HOP] No connection to next hop: {} - sending CIRCUIT_BROKEN", layer.next_id);
            if (!circuit_id.empty()) {
                auto back_it = circuit_hop_table_.find(circuit_id);
                if (back_it != circuit_hop_table_.end()) {
                    auto conn_it = connections_.find(back_it->second.prev_hop);
                    if (conn_it != connections_.end() && conn_it->second && conn_it->second->connected) {
                        const json broken = proto::msg_circuit_broken(circuit_id, node_id_);
                        send_text(conn_it->second->ep, proto::dump_compact(broken));
                        log_debug("[HOP] CIRCUIT_BROKEN for {} sent back to {}", circuit_id, back_it->second.prev_hop);
                    }
                }
            }
            return;
        }
        if (layer.circuit_init) {
            EVP_PKEY* kp = crypto::gen_keypair();
            // Key is bound to in_cid (circuit_id here) — same value A used for this hop
            std::array<uint8_t, 32> sym_key = crypto::hkdf_sha256(crypto::derive_secret(kp, layer.src_pub), circuit_id);
            circuit_keys_[circuit_id] = std::vector{sym_key};
            circuit_pending_kp_[circuit_id] = std::vector{kp};
        }
        std::string payload = layer.payload;
        if (auto ck_it = circuit_keys_.find(circuit_id); ck_it != circuit_keys_.end() && !layer.circuit_init) {
            auto enc_payload = proto::b64_decode(layer.payload);
            if (!enc_payload) { log_error("[HOP] Circuit payload decrypt failed"); return; }
            auto dec_payload = crypto::decrypt(ck_it->second[0], *enc_payload);
            if (!dec_payload) { log_error("[HOP] Circuit payload decrypt failed"); return; }
            payload = proto::b64_encode(*dec_payload);
        }
        // Forward with out_cid — the CID on the next link is different from the one we received
        auto fwd = proto::msg_hop(payload, node_id_, out_cid);
        send_text(it->second->ep, proto::dump_compact(fwd));
        log_debug("[HOP] Relay: {} -> {}", circuit_id, out_cid);
    }
}

void Node::on_hop_reply(const udp::endpoint& from, const proto::Envelope& env) {
    const std::string circuit_id = env.body.at("circuit_id").get<std::string>();
    std::string payload_b64 = env.body.at("payload").get<std::string>();

    // If we originated this circuit, deliver the reply
    if (circuits_.contains(circuit_id)) {
        auto decoded = proto::b64_decode(payload_b64);
        if (!decoded) { log_error("[HOP_REPLY] b64_decode failed"); return; }
        std::string msg(decoded->begin(), decoded->end());
        if (!circuit_keys_.contains(circuit_id)) {
            auto msg_j = json::parse(msg);
            if (msg_j.value("type", "") != "probe_ack") {
                log_error("[HOP_REPLY] Got an unknown packet");
                return;
            }
            std::vector<std::array<uint8_t, 32>> shared_keys;
            auto& keys_json = msg_j.at("keys");
            shared_keys.resize(keys_json.size());
            const auto& path_cids = circuits_[circuit_id].path_cids;
            for (size_t i = 0; i < keys_json.size(); i++) {
                auto key_b64 = keys_json[i].get<std::string>();
                auto key = proto::b64_decode(key_b64);
                if (!key) {
                    log_error("[HOP_REPLY] key b64_decode failed");
                    return;
                }
                std::array<uint8_t, 32> peer_pub{};
                std::ranges::copy(*key, peer_pub.begin());
                const size_t kp_idx = keys_json.size() - 1 - i;
                // Each hop derived its key using its own in_cid (path_cids[kp_idx]), not circuit_id
                const std::string& hop_cid = (kp_idx < path_cids.size()) ? path_cids[kp_idx] : circuit_id;
                auto shared_key = crypto::hkdf_sha256(crypto::derive_secret(circuit_pending_kp_[circuit_id][kp_idx], peer_pub), hop_cid);
                shared_keys.at(shared_keys.size() - i - 1) = shared_key;
                crypto::free_keypair(circuit_pending_kp_[circuit_id][kp_idx]);
            }
            circuit_pending_kp_.erase(circuit_id);
            circuit_keys_[circuit_id] = std::move(shared_keys);
            cancel_circuit_timer(circuit_id);
            log_debug("[HOP_REPLY] Circuit_id: {} has been established", circuit_id);
            circuits_[circuit_id].state   = CircuitState::READY;
            update_snapshot();
            circuits_[circuit_id].last_ack = Clock::now();
            circuit_build_attempts_.erase(circuits_[circuit_id].path.back());
        }
        else {
            for (std::array<uint8_t, 32>& key : circuit_keys_[circuit_id]) {
                decoded = crypto::decrypt(key, *decoded);
                if (!decoded) { log_error("[HOP_REPLY] reply decrypt failed"); return; }
            }
            if (auto c_it = circuits_.find(circuit_id); c_it != circuits_.end())
                c_it->second.last_ack = Clock::now();
            msg = std::string(decoded->begin(), decoded->end());
            // Re-dispatch as a protocol message (CHUNK_ACK, CHUNK_NACK, etc.)
            try {
                proto::Envelope inner_env = proto::parse_envelope(msg);
                dispatch(const_cast<udp::endpoint&>(from), inner_env);
            } catch (...) {
                log_info("[HOP_REPLY] [circuit={}] Reply: {}", circuit_id, msg);
            }
        }
        return;
    }

    // We're a relay — the circuit_id we received is our out_cid; translate back to in_cid
    auto reply_it = circuit_reply_table_.find(circuit_id);
    if (reply_it == circuit_reply_table_.end()) {
        log_warn("[HOP_REPLY] Unknown out_cid: {}", circuit_id);
        return;
    }
    const std::string in_cid = reply_it->second;

    auto it = circuit_hop_table_.find(in_cid);
    if (it == circuit_hop_table_.end()) {
        log_warn("[HOP_REPLY] No hop entry for in_cid: {}", in_cid);
        return;
    }
    const NodeId& prev_hop = it->second.prev_hop;
    auto conn_it = connections_.find(prev_hop);
    if (conn_it == connections_.end() || !conn_it->second || !conn_it->second->connected) {
        log_warn("[HOP_REPLY] No connection to prev hop: {}", prev_hop);
        return;
    }
    // Keys are stored under in_cid (the CID we received from upstream)
    if (auto cpkp_it = circuit_pending_kp_.find(in_cid); cpkp_it != circuit_pending_kp_.end()) {
        auto payload = proto::b64_decode(payload_b64);
        if (!payload) { log_error("[HOP_REPLY] payload decode failed"); return; }
        std::string pl_json_str(payload->begin(), payload->end());
        json pl_json = json::parse(pl_json_str);
        auto keys = pl_json["keys"].get<json::array_t>();
        auto my_pub = crypto::get_pubkey(cpkp_it->second[0]);
        auto my_pub_vec = std::vector<uint8_t>(my_pub.begin(), my_pub.end());
        keys.push_back(proto::b64_encode(my_pub_vec));
        pl_json["keys"] = keys;
        auto pl_str = proto::dump_compact(pl_json);
        payload_b64 = proto::b64_encode(proto::to_bytes(pl_str));
        crypto::free_keypair(cpkp_it->second[0]);
        circuit_pending_kp_.erase(in_cid);
    } else if (auto ck_it = circuit_keys_.find(in_cid); ck_it != circuit_keys_.end()) {
        auto raw = proto::b64_decode(payload_b64);
        if (!raw) { log_error("[HOP_REPLY] relay encrypt: b64_decode failed"); return; }
        payload_b64 = proto::b64_encode(crypto::encrypt(ck_it->second[0], *raw));
    }
    // Forward backwards with in_cid — CID is switched back to what the upstream expects
    const json fwd = proto::msg_hop_reply(in_cid, payload_b64, node_id_);
    send_text(conn_it->second->ep, proto::dump_compact(fwd));
    log_debug("[HOP_REPLY] Relay: {} -> {}", circuit_id, in_cid);
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
        cancel_circuit_timer(circuit_id);
        circuits_.erase(it);
        if (auto kp_it = circuit_pending_kp_.find(circuit_id); kp_it != circuit_pending_kp_.end()) {
            for (auto* kp : kp_it->second) crypto::free_keypair(kp);
            circuit_pending_kp_.erase(kp_it);
        }
        circuit_keys_.erase(circuit_id);
        begin_circuit_build(dst);
        return;
    }

    // We're a relay — received circuit_id is our out_cid; translate to in_cid first
    auto reply_it = circuit_reply_table_.find(circuit_id);
    if (reply_it == circuit_reply_table_.end()) {
        log_warn("[CIRCUIT_BROKEN] Unknown out_cid {}", circuit_id);
        return;
    }
    const std::string in_cid = reply_it->second;
    auto back_it = circuit_hop_table_.find(in_cid);
    if (back_it == circuit_hop_table_.end()) {
        log_warn("[CIRCUIT_BROKEN] No hop entry for in_cid {}", in_cid);
        return;
    }
    const NodeId& prev_hop = back_it->second.prev_hop;
    auto conn_it = connections_.find(prev_hop);
    if (conn_it == connections_.end() || !conn_it->second || !conn_it->second->connected) {
        log_warn("[CIRCUIT_BROKEN] Can't propagate - no connection to {}", prev_hop);
        cleanup_relay_circuit(in_cid);
        return;
    }
    // Propagate backwards with in_cid so the upstream hop recognises it
    const json fwd = proto::msg_circuit_broken(in_cid, node_id_);
    send_text(conn_it->second->ep, proto::dump_compact(fwd));
    cleanup_relay_circuit(in_cid);
    log_debug("[CIRCUIT_BROKEN] Forwarded {} -> {} back to {}", circuit_id, in_cid, prev_hop);
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
    if (initial) { update_snapshot(); return; }

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
    update_snapshot();
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
    // Tell the receiver to wipe stale graph entries before applying fresh state
    send_text(to, proto::dump_compact(proto::msg_graph_reset()));
    for (const auto& [id, entry] : clients_map_) {
        const uint64_t seq = ++graph_seq_out_[id];
        const json msg = proto::msg_graph_update(node_id_, id, entry.neighbors, seq, true);
        send_text(to, proto::dump_compact(msg));
    }
    log_info("[ROOT] Sent graph snapshot ({} nodes) to {}:{}", clients_map_.size(), to.address().to_string(), to.port());
}

void Node::send_reply_via_circuit(const std::string& circuit_id, const std::string& data) {
    auto it = circuit_hop_table_.find(circuit_id);
    if (it == circuit_hop_table_.end()) {
        log_warn("[REPLY] No return path for circuit {}. Was a message received on this circuit?", circuit_id);
        return;
    }
    const NodeId& prev_hop = it->second.prev_hop;
    auto conn_it = connections_.find(prev_hop);
    if (conn_it == connections_.end() || !conn_it->second || !conn_it->second->connected) {
        log_warn("[REPLY] No connection to relay {}", prev_hop);
        return;
    }
    std::vector<uint8_t> data_bytes = proto::to_bytes(data);
    if (const auto ck_it = circuit_keys_.find(circuit_id); ck_it != circuit_keys_.end())
        data_bytes = crypto::encrypt(ck_it->second[0], data_bytes);
    const std::string payload = proto::b64_encode(data_bytes);
    const json msg = proto::msg_hop_reply(circuit_id, payload, node_id_);
    send_text(conn_it->second->ep, proto::dump_compact(msg));
    log_info("[REPLY] Sent reply on circuit {}", circuit_id);
}

void Node::update_snapshot() {
    NodeSnapshot s;
    s.node_id = node_id_;
    s.level   = level_;
    s.is_root = is_root_;
    s.daddy   = daddy_;
    s.linked_peers.assign(linked_up_.begin(), linked_up_.end());
    for (const auto& [dst, cid] : circuit_by_dst_) {
        auto it = circuits_.find(cid);
        if (it == circuits_.end()) continue;
        s.circuits.push_back({cid, dst, it->second.state});
    }
    for (const auto& [pid, info] : clients_map_) {
        s.known_peers.push_back(pid);
        s.peer_ips[pid] = info.ep.address().to_string() + ":" + std::to_string(info.ep.port());
    }
    std::lock_guard lock(gui_mutex_);
    snapshot_ = std::move(s);
}

void Node::designate_heir() {
    if (!is_root_ || linked_up_.empty()) return;
    std::vector<NodeId> candidates(linked_up_.begin(), linked_up_.end());
    heir_id_ = candidates[random_0_to_n(static_cast<int>(candidates.size()))];
    has_heir_ = true;
    send_text(connections_.at(heir_id_)->ep,
              proto::dump_compact(proto::msg_you_are_heir(node_id_)));
    log_info("[HEIR] Designated {} as successor", heir_id_);
}


void Node::take_over_as_root() {
    log_info("[ELECTION] Taking over as root");
    is_heir_ = false;
    const NodeId dead_root = std::move(heired_from_);
    heired_from_.clear();
    become_root();
    for (auto& [id, info] : clients_map_)
        info.neighbors.erase(dead_root);
    clients_map_.erase(dead_root);
    const auto msg = proto::dump_compact(proto::msg_new_root(node_id_, ip_, dead_root));
    for (const auto& [id, conn] : connections_)
        if (conn && conn->connected) send_text(conn->ep, msg);
    designate_heir();
    update_snapshot();
}

void Node::on_you_are_heir(const udp::endpoint& from, const proto::Envelope& env) {
    heired_from_ = env.body.at("root_id").get<NodeId>();
    is_heir_     = true;
    log_info("[HEIR] I am the designated successor to {}", heired_from_);
}

void Node::on_new_root(const udp::endpoint& from, const proto::Envelope& env) {
    if (is_root_) return;  // we are the new root — already handled in take_over_as_root
    const std::string new_ip   = env.body.at("ip").get<std::string>();
    const NodeId      new_root = env.body.at("root_id").get<NodeId>();
    const NodeId      old_root = env.body.value("old_root_id", std::string{});
    std::error_code ec;
    const auto addr = asio::ip::make_address(new_ip, ec);
    if (ec) { log_warn("[NEW_ROOT] Bad IP: {}", new_ip); return; }
    root_ip_ = new_ip;
    root_ep_ = udp::endpoint(addr, ROOT_PORT);
    log_info("[NEW_ROOT] Root is now {} at {}", new_root, new_ip);
    if (!old_root.empty()) {
        for (auto& [id, info] : clients_map_)
            info.neighbors.erase(old_root);
        clients_map_.erase(old_root);
        remove_connection(old_root);
        update_snapshot();
    }
    choose_parent();
    const auto fwd = proto::dump_compact(proto::msg_new_root(new_root, new_ip, old_root));
    for (const auto& [id, conn] : connections_)
        if (conn && conn->connected && id != env.src)
            send_text(conn->ep, fwd);
}

// -------------------------- Admin capabilities --------------------------

std::vector<uint8_t> Node::take_screenshot(int& out_w, int& out_h) {
    HDC screen = GetDC(nullptr);
    out_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    out_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);

    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, out_w, out_h);
    HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp));
    BitBlt(mem, 0, 0, out_w, out_h, screen, x, y, SRCCOPY);
    SelectObject(mem, old); // deselect bmp — required before GetDIBits

    BITMAPINFOHEADER bi{sizeof(bi), out_w, -out_h, 1, 32};
    std::vector<uint8_t> buf(out_w*out_h*4);
    GetDIBits(screen, bmp, 0, out_h, buf.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    // GetDIBits returns BGRA; swap to RGBA for stb_image_write
    for (size_t i = 0; i < buf.size(); i += 4)
        std::swap(buf[i], buf[i + 2]);

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return buf;
}

std::vector<uint8_t> Node::to_png(const std::vector<uint8_t> &bit_data, int w, int h) {
    std::vector<uint8_t> out;
    stbi_write_png_to_func([](void* ctx, void*data, int size) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        v->insert(v->end(), (uint8_t*)data, (uint8_t*)data+size);
    }, &out, w, h, 4, bit_data.data(), w*4);
    return out;
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
    if (++circuit_build_attempts_[dst] > MAX_CIRCUIT_BUILD_ATTEMPTS) {
        log_warn("[CIRCUIT] Giving up on {} after {} consecutive failed build attempts", dst, MAX_CIRCUIT_BUILD_ATTEMPTS);
        circuit_build_attempts_.erase(dst);
        circuit_by_dst_.erase(dst);
        return;
    }
    // Scale relays with graph size: each relay needs a distinct node (src + relays + dst)
    const int num_relays = std::clamp(static_cast<int>(clients_map_.size()) - 2, MIN_RELAYS, MAX_RELAYS);
    log_info("[CIRCUIT] Building circuit to {} (graph_size={}, relays={})", dst, clients_map_.size(), num_relays);
    auto path_opt = compute_route(node_id_, dst, num_relays + 1);
    if (!path_opt) {
        log_warn("[CIRCUIT] compute_route failed - graph too small or {} unreachable", dst);
        return;
    }

    // path = [src, relay1, ..., dst]; circuit path excludes src
    std::vector circuit_path(path_opt->begin() + 1, path_opt->end());

    if (circuit_path.empty()) {
        log_warn("[CIRCUIT] Computed path is empty");
        return;
    }

    // If there is already a stuck INITIATING circuit for this dst, clean it up first.
    if (auto old_by_dst = circuit_by_dst_.find(dst); old_by_dst != circuit_by_dst_.end()) {
        const std::string& old_cid = old_by_dst->second;
        if (auto old_circ = circuits_.find(old_cid);
            old_circ != circuits_.end() && old_circ->second.state == CircuitState::INITIATING) {
            cancel_circuit_timer(old_cid);
            if (auto kp_it = circuit_pending_kp_.find(old_cid); kp_it != circuit_pending_kp_.end()) {
                for (auto* kp : kp_it->second) crypto::free_keypair(kp);
                circuit_pending_kp_.erase(kp_it);
            }
            circuit_keys_.erase(old_cid);
            circuits_.erase(old_circ);
        }
    }

    Circuit c;
    c.circuit_id = proto::random_tx_id();
    c.path       = std::move(circuit_path);
    c.state      = CircuitState::INITIATING;

    // path_cids[i] = CID used on the link INTO path[i].
    // path_cids[0] == circuit_id (src→relay1); the rest are fresh random IDs.
    c.path_cids.push_back(c.circuit_id);
    for (size_t i = 1; i < c.path.size(); ++i)
        c.path_cids.push_back(proto::random_tx_id());

    circuits_[c.circuit_id] = c;
    circuit_by_dst_[dst]    = c.circuit_id;
    update_snapshot();

    std::string path_str = node_id_;
    for (const auto& hop : c.path) path_str += " -> " + hop;
    log_info("[CIRCUIT {}] READY - path: {}", c.circuit_id, path_str);
    log_info("[CIRCUIT {}] Use: sanon {} <message>", c.circuit_id, c.path.back());

    // Send a probe so relays populate their CID tables and derive per-hop keys.
    // path_cids is embedded in each onion layer so every relay knows which out_cid to assign.
    std::vector<EVP_PKEY*> path_keypairs = gen_path_keys(c.path.size());
    circuit_pending_kp_[c.circuit_id] = path_keypairs;
    auto path_pubkeys = get_path_pubkeys(path_keypairs);
    const std::string probe_payload = json{
        {"type", "probe"},
        {"src", node_id_}}.dump(-1);
    const auto probe_onion = proto::build_onion(c.path, probe_payload, path_pubkeys, {}, c.path_cids);
    const std::string probe_json = proto::dump_compact(probe_onion);
    const std::string hop_payload = proto::b64_encode(proto::to_bytes(probe_json));
    const json hop = proto::msg_hop(hop_payload, node_id_, c.circuit_id);
    send_text(connections_.at(c.path[0])->ep, proto::dump_compact(hop));
    log_debug("[CIRCUIT {}] Probe sent to populate relay tables", c.circuit_id);

    // Start a timeout: if the handshake never completes, clean up and retry.
    const std::string cid_copy = c.circuit_id;
    auto timer = std::make_unique<asio::steady_timer>(io_);
    timer->expires_after(circuit_init_timeout_sec);
    timer->async_wait([self = shared_from_this(), cid_copy, dst](const std::error_code& ec) {
        if (ec) return; // cancelled — circuit became READY or was torn down
        auto it = self->circuits_.find(cid_copy);
        if (it == self->circuits_.end() || it->second.state != CircuitState::INITIATING) return;
        log_warn("[CIRCUIT {}] Handshake timed out — rebuilding to {}", cid_copy, dst);
        if (auto kp_it = self->circuit_pending_kp_.find(cid_copy); kp_it != self->circuit_pending_kp_.end()) {
            for (auto* kp : kp_it->second) crypto::free_keypair(kp);
            self->circuit_pending_kp_.erase(kp_it);
        }
        self->circuit_keys_.erase(cid_copy);
        self->circuits_.erase(cid_copy);
        self->circuit_timers_.erase(cid_copy);
        self->begin_circuit_build(dst);
    });
    circuit_timers_[c.circuit_id] = std::move(timer);
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
        cancel_circuit_timer(old_cid);
        circuits_.erase(old_cid);
        if (auto kp_it = circuit_pending_kp_.find(old_cid); kp_it != circuit_pending_kp_.end()) {
            for (auto* kp : kp_it->second) crypto::free_keypair(kp);
            circuit_pending_kp_.erase(kp_it);
        }
        circuit_keys_.erase(old_cid);
        begin_circuit_build(dst);
    }
    update_snapshot();
}

void Node::send_via_circuit(const std::string& circuit_id, const std::string& data) {
    auto it = circuits_.find(circuit_id);
    if (it == circuits_.end() || it->second.state != CircuitState::READY) {
        log_warn("[CIRCUIT] {} not found or not READY", circuit_id);
        return;
    }

    it->second.last_used = Clock::now();

    const NodeId dst = it->second.path.back();
    const auto& path = it->second.path;

    auto first_hop = connections_.find(path[0]);
    if (first_hop == connections_.end() || !first_hop->second) {
        log_warn("[CIRCUIT {}] First hop {} gone - rebuilding", circuit_id, path[0]);
        cancel_circuit_timer(circuit_id);
        circuits_.erase(it);
        if (auto kp_it = circuit_pending_kp_.find(circuit_id); kp_it != circuit_pending_kp_.end()) {
            for (auto* kp : kp_it->second) crypto::free_keypair(kp);
            circuit_pending_kp_.erase(kp_it);
        }
        circuit_keys_.erase(circuit_id);
        begin_circuit_build(dst);
        return;
    }

    std::vector<std::array<uint8_t,32>> hop_keys{};
    if (const auto ck_it = circuit_keys_.find(circuit_id); ck_it != circuit_keys_.end())
        hop_keys = ck_it->second;
    const json onion = proto::build_onion(path, data, {}, hop_keys);
    const std::string onion_json = proto::dump_compact(onion);
    const std::string hop_payload = proto::b64_encode(proto::to_bytes(onion_json));
    const json hop = proto::msg_hop(hop_payload, node_id_, circuit_id);
    send_text(first_hop->second->ep, proto::dump_compact(hop));
    log_debug("[CIRCUIT {}] Sent data via circuit", circuit_id);
}


// ------------------------ encryption logic ------------------------

void Node::cancel_circuit_timer(const std::string& circuit_id) {
    if (auto it = circuit_timers_.find(circuit_id); it != circuit_timers_.end()) {
        it->second->cancel();
        circuit_timers_.erase(it);
    }
}

// Cleans up relay-side state for a circuit.  Pass the in_cid (the CID received from upstream).
void Node::cleanup_relay_circuit(const std::string& in_cid) {
    if (auto hop_it = circuit_hop_table_.find(in_cid); hop_it != circuit_hop_table_.end()) {
        circuit_reply_table_.erase(hop_it->second.out_cid);
        circuit_hop_table_.erase(hop_it);
    }
    circuit_keys_.erase(in_cid);
    if (auto kp_it = circuit_pending_kp_.find(in_cid); kp_it != circuit_pending_kp_.end()) {
        for (auto* kp : kp_it->second) crypto::free_keypair(kp);
        circuit_pending_kp_.erase(kp_it);
    }
}

std::vector<EVP_PKEY*> Node::gen_path_keys(const size_t hop_num) {
    std::vector<EVP_PKEY*> path{};
    for (size_t i = 0; i < hop_num; i++) {
        EVP_PKEY* cur = crypto::gen_keypair();
        path.push_back(cur);
    }
    return path;
}

std::vector<std::array<uint8_t, 32>> Node::get_path_pubkeys(const std::vector<EVP_PKEY*>& kps) {
    std::vector<std::array<uint8_t, 32>> path{};
    for (auto& kp : kps) {
        auto cur = crypto::get_pubkey(kp);
        path.push_back(cur);
    }
    return path;
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