#pragma once
#include <memory>

class Node;

class UserGUI {
public:
    explicit UserGUI(std::shared_ptr<Node> node);
    ~UserGUI();   // defined in .cpp — Impl must be complete at destruction site
    void run();   // blocks until the window is closed
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
