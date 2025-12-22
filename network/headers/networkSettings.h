//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_NETWORKSETTINGS_H
#define TROJAN_MESSAGE_NETWORKSETTINGS_H

#endif //TROJAN_MESSAGE_NETWORKSETTINGS_H

#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <cstring>
#include <iostream>
#include <vector>
#include <chrono>
#include <map>
#include <memory>


#ifdef _WIN32
  #include <winsock2.h>
#else
  #include <arpa/inet.h>
#endif

using udp = asio::ip::udp;
using json = nlohmann::json;
