//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_APICOMM_H
#define TROJAN_MESSAGE_APICOMM_H
#include <curl/curl.h>
#include <iostream>
#include <string>
#include "../headers/networkSettings.h"
using namespace std;
string getDDNS();
vector<string> getIP_();
void setRoot(string ipv4, string ipv6);

class apiComm {
};


#endif //TROJAN_MESSAGE_APICOMM_H