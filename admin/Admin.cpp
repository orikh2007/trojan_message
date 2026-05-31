#include "Admin.h"
#include <iostream>
#include <sstream>
#include <chrono>

Admin::Admin(int port) {
    node_ = std::make_shared<Node>(static_cast<uint16_t>(port));
    Logger::get().set_level(LogLevel::ERR);

    node_->set_shell_out_callback([this](ShellOut out) {
        {
            std::lock_guard lock(cv_mtx_);
            pending_outs_.push_back(std::move(out));
        }
        cv_.notify_one();
    });

    node_->set_admin();

    node_->start();
    net_thread_ = std::thread([this] {
        try {
            node_->io().run();
        } catch (const std::exception& e) {
            std::cerr << "io_context exception: " << e.what() << "\n";
        }
    });
}

Admin::~Admin() {
    node_->io().stop();
    if (net_thread_.joinable()) net_thread_.join();
}

void Admin::connect_to(const NodeId& dst) {
    node_->handle_command("circuit " + dst);
    std::cout << "[building circuit to " << dst << "...]\n";

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (node_->has_ready_circuit(dst)) {
            active_session_dst_ = dst;
            std::cout << "[connected]\n";
            node_->send_shell_cmd("cd", active_session_dst_);
            std::unique_lock lock(cv_mtx_);
            bool got = cv_.wait_for(lock, std::chrono::seconds(10),
                                    [this] { return !pending_outs_.empty(); });
            if (got) {
                active_session_cwd_ = pending_outs_[0].cwd;
            } else {
                std::cout << "[no response in 10s]\n";
            }
            return;
        }
    }
    std::cout << "[timeout: could not build circuit to " << dst << "]\n";
}

void Admin::run_shell_session() {
    std::string line;
    active_session_cwd_.clear();
    { std::lock_guard lock(cv_mtx_); pending_outs_.clear(); }
    node_->send_shell_cmd("echo nthn > null", active_session_dst_);
    {
        std::unique_lock lock(cv_mtx_);
        bool got = cv_.wait_for(lock, std::chrono::seconds(30),
                                [this] { return !pending_outs_.empty(); });
        if (got) {
            for (const auto& o : pending_outs_) {
                std::cout << o.out;
                active_session_cwd_ = o.cwd;
            }
            pending_outs_.clear();
        } else {
            std::cout << "[no response in 30s]\n";
        }
    }
    while (true) {
        std::cout << active_session_dst_ << "$" << (active_session_cwd_.empty() ? "(run command to see cwd)" : active_session_cwd_) << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line == "dscnct" || line == "exit") {
            active_session_dst_.clear();
            break;
        }
        if (line.empty()) continue;

        // discard any stale responses from previous timed-out commands
        { std::lock_guard lock(cv_mtx_); pending_outs_.clear(); }

        node_->send_shell_cmd(line, active_session_dst_);

        std::unique_lock lock(cv_mtx_);
        bool got = cv_.wait_for(lock, std::chrono::seconds(30),
                                [this] { return !pending_outs_.empty(); });
        if (got) {
            for (const auto& o : pending_outs_) {
                std::cout << o.out;
                active_session_cwd_ = o.cwd;
            }
            pending_outs_.clear();
        } else {
            std::cout << "[no response in 30s]\n";
        }
    }
}

void Admin::run() {
    asio::post(node_->io(), [n = node_]{ n->handle_command("r"); });

    std::cout << "commands:\n"
                 "lst - list net members\n"
                 "cnct - connect to id\n"
                 "rt - become root\n"
                 "rg - register\n";

    std::string line;
    while (std::cout << "admin> " && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "quit") {
            break;
        } else if (cmd == "lst") {
            auto snap = node_->get_snapshot();
            auto clients = snap.known_peers;
            if (clients.empty()) { std::cout << "(no known nodes)\n"; continue; }
            for (const auto& id : clients) {
                if (id == snap.node_id) continue;
                auto it = snap.peer_ips.find(id);
                std::string ip = (it != snap.peer_ips.end()) ? it->second : "unknown";
                std::cout << "  " << id << "\n";
            }
        } else if (cmd == "cnct") {
            NodeId dst;
            iss >> dst;
            if (dst.empty()) { std::cout << "Usage: connect <node_id>\n"; continue; }
            connect_to(dst);
            if (!active_session_dst_.empty())
                run_shell_session();
        } else if (cmd == "rt") {
            asio::post(node_->io(), [n = node_]{ n->handle_command("root"); });
        } else if (cmd == "rg") {
            asio::post(node_->io(), [n = node_]{ n->handle_command("r"); });
        }
        else {
            std::cout << "Unknown command. Try: list, connect <id>, quit\n";
        }
    }
}
