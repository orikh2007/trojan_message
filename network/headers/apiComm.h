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
void setRoot(const string& ip);
string getIP(int n);

enum IPS {
    v4 = 0,
    v6 = 1
};

#endif //TROJAN_MESSAGE_APICOMM_H