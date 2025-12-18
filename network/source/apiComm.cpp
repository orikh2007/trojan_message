//
// Created by orikh on 02/12/2025.
//

#include "../headers/apiComm.h"
const std::string DDNS_API = "https://api.dynu.com/v2/";
using json = nlohmann::json;
using namespace std;
static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
	((string*)userp)->append((char*)contents, size * nmemb);
	return size * nmemb;
};

static size_t writeData(void* buffer, size_t size, size_t nmemb, void* userp) {return size * nmemb;}

static bool fetchUrlToString(const char* url, std::string& out, long ipResolve /* CURL_IPRESOLVE_* */) {
	CURL* curl = curl_easy_init();
	if (!curl) return false;

	out.clear();
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
	curl_easy_setopt(curl, CURLOPT_IPRESOLVE, ipResolve);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		std::cerr << "curl failed (" << url << "): " << curl_easy_strerror(res) << "\n";
		curl_easy_cleanup(curl);
		return false;
	}

	curl_easy_cleanup(curl);
	return true;
}

vector<string> getIP_() {
	string ipv4, ipv6;

	fetchUrlToString("https://api64.ipify.org", ipv4, CURL_IPRESOLVE_V4);
	fetchUrlToString("https://api64.ipify.org", ipv6, CURL_IPRESOLVE_V6);

	return {ipv4, ipv6};
}

vector<string> getIP() {
	curl_global_init(CURL_GLOBAL_DEFAULT);
	auto ips = getIP_();
	curl_global_cleanup();
	return ips;
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

void setRoot(string ipv4, string ipv6) {
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


		string json_req = "{\"name\":\"" + DDNS_URL + "\",\"ipv4Address\":\"" + ipv4 + "\",\"ipv6Address\":\"" + ipv6 + "\"}";

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
