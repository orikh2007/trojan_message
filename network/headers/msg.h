//
// Created by orikh on 12/12/2025.
//
#pragma once

#include <iostream>
#include <nlohmann/json.hpp>

class msg {
public:
    uint32_t size = 0;
    asio::ip::address_v4 target_ip;
    int target_port;
    std::string data;
    char type; //type of msg: e - error, m - message,


};