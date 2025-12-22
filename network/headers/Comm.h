//
// Created by Ori Kedar Haspel on 18/12/2025.
//

#ifndef TROJAN_MESSAGE_COMMPROTOCOL_H
#define TROJAN_MESSAGE_COMMPROTOCOL_H
#include <asio.hpp>
#include <array>
#include <iostream>
using udp = asio::ip::udp;

class Comm : std::enable_shared_from_this<Comm>{
public:
    using OnMessage = std::function<void(const asio::ip::udp::endpoint&, const std::string&)>;

    Comm(asio::io_context& io, uint16_t port)
    : io_(io),
    socket_(io),
    resolver_(io)
    {
        socket_.open(udp::v4());
        socket_.set_option(asio::socket_base::reuse_address(true));
        socket_.set_option(asio::ip::v6_only(true));
        socket_.bind(udp::endpoint(udp::v4(), port));
    }

    void set_on_message(OnMessage cb) { on_message_ = std::move(cb); }

    void start() { start_receive(); }

    void ipv6_msg_listener();
        /*
         * ipv6 init msg receiver
         */

    void msg_receiver();

    void msg_sender();

private:
    void start_receive() {
        socket_.async_receive_from(asio::buffer(rx_buf_), remote_, [self = shared_from_this()](std::error_code ec, std::size_t n) {
            if (!ec) {
                const std::string msg(self->rx_buf_.data(), self->rx_buf_.data() + n);
                if (self->on_message_) {
                    self->on_message_(self->remote_, msg);
                } else {
                    std::cout << "From [" << self->remote_.address() << "] " << self->remote_.port() << ": " << msg <<  std::endl;
                }
            } else {
                std::cerr << "recv error: " << ec.message() << std::endl;
                self->io_.stop();
            }
            self-> start_receive();
        });
    }

    udp::endpoint remote_;
    asio::io_context& io_;
    udp::socket socket_;
    std::array<char, 2048> rx_buf_{};
    udp::resolver resolver_;

    OnMessage on_message_;
};


#endif //TROJAN_MESSAGE_COMMPROTOCOL_H