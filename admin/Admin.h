#pragma once
#include "../network/headers/Node.h"
#include "../network/headers/networkSettings.h"
#include <thread>
#include <condition_variable>

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
    std::condition_variable cv_;
    std::vector<ShellOut> pending_outs_;
};
