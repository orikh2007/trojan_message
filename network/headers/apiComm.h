//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_APICOMM_H
#define TROJAN_MESSAGE_APICOMM_H
#include <curl/curl.h>
#include "networkSettings.h"

const std::string DDNS_URL = "trojantext.ddnsgeek.com";
const std::string DDNS_ID = "13292760";
const std::string DDNS_API_KEY = "6WU4aYgcb6TYXc6ebYgVb54Y3cT45Xca";
const std::string DDNS_API = "https://api.dynu.com/v2/";

enum IPS {
    v4 = 0,
    v6 = 1
};

std::string getDDNS();
void setRoot(const std::string& ip);
std::string getIP(int n);

#endif //TROJAN_MESSAGE_APICOMM_H