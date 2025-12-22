//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_NODE_H
#define TROJAN_MESSAGE_NODE_H
#endif //TROJAN_MESSAGE_NODE_H

#ifdef _WIN32
#define _WIN32_WINNT 0x0A00
#endif


#include <curl/curl.h>
#include "apiComm.h"
#include <asio.hpp>
#include "protocol.h"
using namespace std;

class Node {
public:
    struct PeerInfo {
        udp::endpoint public_v4;
        uint32_t token = 0;
        uint32_t session_id = 0;
    };

    // App hooks
    std::function<void(const PeerInfo&)> on_peer_info;
    std::function<void(const udp::endpoint&)> on_p2p_established;
    std::function<void(const udp::endpoint&, std::vector<uint8_t>)> on_data;

    Node(asio::io_context& io, uint16_t v4_listen_port)
    : io_(io),
      strand_(asio::make_strand(io)),
      sock_v4_(io),
      sock_v6_(io),
      punch_timer_(io),
      keepalive_timer_(io),
      v4_port_(v4_listen_port)
    {}
    void KeepAlive(auto target) {

    }
    void sendMsg(asio::ip::address_v4 target, string msg);
    void receiveMsg(vector<char> buffer);
};