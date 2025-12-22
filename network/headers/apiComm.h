//
// Created by orikh on 02/12/2025.
//

#ifndef TROJAN_MESSAGE_APICOMM_H
#define TROJAN_MESSAGE_APICOMM_H
#include <curl/curl.h>
#include <iostream>
#include <string>
#include "networkSettings.h"
using namespace std;
string getDDNS();
string getIP();
void setRoot(string ip);

class apiComm {
};

#endif //TROJAN_MESSAGE_APICOMM_H