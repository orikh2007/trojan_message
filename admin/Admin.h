#pragma once
#include "../network/headers/Node.h"
#include "../network/headers/networkSettings.h"
#include <thread>
#include <condition_variable>
#include <filesystem>

namespace fs = std::filesystem;


class Admin {
public:
    explicit Admin(int port);
    ~Admin();
    void run();

private:
    void connect_to(const NodeId& dst);
    void run_shell_session();

    std::shared_ptr<Node> node_;
    std::thread net_thread_;
    NodeId active_session_dst_;
    std::string active_session_cwd_;

    std::mutex cv_mtx_;
    std::mutex cv_mtx_scrsht_;
    std::condition_variable cv_;
    std::condition_variable cv_scrsht_;
    std::vector<ShellOut> pending_outs_;
    std::vector<std::vector<uint8_t>> pending_scrsht_outs_;

    fs::path image_path_;
    uint32_t scrsht_count_ = 0;

};
