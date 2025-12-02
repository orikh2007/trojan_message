//
// Created by orikh on 02/12/2025.
//

#include "../headerFiles/networking/apiComm.h"
const std::string DDNS_API = "https://api.dynu.com/v2/";
using json = nlohmann::json;
using namespace std;
static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
	((string*)userp)->append((char*)contents, size * nmemb);
	return size * nmemb;
};

static size_t writeData(void* buffer, size_t size, size_t nmemb, void* userp) {return size * nmemb;}

string getIP() {
	CURL *curl;
	string response;
	CURLcode res;

	curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, "https://api.ipify.org");
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);

		res = curl_easy_perform(curl);
		if (res != CURLE_OK) cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << endl; // debug

		curl_easy_cleanup(curl);
	}

	curl_global_cleanup();

	return response;
}

string getDDNS() {
	CURL* curl;
	CURLcode res;
	string response;

	curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, "https://api.dynu.com/v2/dns/");

		struct curl_slist* headers = NULL;
		headers = curl_slist_append(headers, "Host: api.dynu.com");
		headers = curl_slist_append(headers, "accept: application/json");
		headers = curl_slist_append(headers, ("API-Key: " + DDNS_API_KEY).c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

		res = curl_easy_perform(curl);
		if (res != CURLE_OK) cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << endl; // debug

		curl_easy_cleanup(curl);
	}

	curl_global_cleanup();
	string resp = json::parse(response)["domains"][0]["ipv4Address"];
	cout << json::parse(response).dump(4) << endl;
	cout << resp << endl;
	return resp;
}

void setRoot(string ip) {
	CURL *curl;
	CURLcode res;
	string response;

	curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, ("https://api.dynu.com/v2/dns/"+DDNS_ID).c_str());

		struct curl_slist* headers = NULL;
		headers = curl_slist_append(headers, "Host: api.dynu.com");
		headers = curl_slist_append(headers, "accept: application/json");
		headers = curl_slist_append(headers, ("API-Key: " + DDNS_API_KEY).c_str());
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);


		string json_req = "{\"name\":\"" + DDNS_URL + "\",\"ipv4Address\":\"" + ip + "\"}";

		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_req.c_str());

		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeData);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);


		res = curl_easy_perform(curl);
		if (res != CURLE_OK) cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << endl; // debug

		curl_easy_cleanup(curl);
	}
	cout << response << endl;

	curl_global_cleanup();
}
