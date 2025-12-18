//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_NODE_H
#define TROJAN_MESSAGE_NODE_H

#endif //TROJAN_MESSAGE_NODE_H

#include <string>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

using namespace std;

class Node {
public:
    const string ip = getIP();
};