#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include "jev_client.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "yyjson.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <random>
#include <thread>

namespace duckdb {

namespace httplib = duckdb_httplib_openssl;
using namespace duckdb_yyjson; // NOLINT

static constexpr const char *DEFAULT_BASE_URL = "https://api.typesafe.ai";
static constexpr const char *DEFAULT_MODEL = "jev-latest";
static constexpr const char *USER_AGENT = "duckdb-jev/0.1.0";

static constexpr int64_t BACKOFF_INITIAL_MS = 500;
static constexpr int64_t BACKOFF_MAX_MS = 5000;
static constexpr double BACKOFF_JITTER = 0.25;
static constexpr int64_t MAX_RETRY_AFTER_MS = 60000;

//===--------------------------------------------------------------------===//
// Configuration
//===--------------------------------------------------------------------===//

static string GetEnv(const char *name) {
	auto value = std::getenv(name);
	if (!value) {
		return string();
	}
	auto result = string(value);
	StringUtil::Trim(result);
	return result;
}

static bool TryGetStringSetting(ClientContext &context, const string &name, string &out) {
	Value value;
	if (!context.TryGetCurrentSetting(name, value) || value.IsNull()) {
		return false;
	}
	auto str = value.ToString();
	if (str.empty()) {
		return false;
	}
	out = str;
	return true;
}

static bool TryGetIntSetting(ClientContext &context, const string &name, int64_t &out) {
	Value value;
	if (!context.TryGetCurrentSetting(name, value) || value.IsNull()) {
		return false;
	}
	out = value.GetValue<int64_t>();
	return true;
}

static bool TryGetSecretString(const KeyValueSecret *secret, const string &key, string &out) {
	if (!secret) {
		return false;
	}
	Value value;
	if (!secret->TryGetValue(key, value) || value.IsNull()) {
		return false;
	}
	auto str = value.ToString();
	if (str.empty()) {
		return false;
	}
	out = str;
	return true;
}

static string StripTrailingSlashes(string url) {
	while (!url.empty() && url.back() == '/') {
		url.pop_back();
	}
	return url;
}

JevConfig JevConfig::Resolve(ClientContext &context) {
	JevConfig config;

	// Base URL is needed first, since it is the path a scoped secret is matched against.
	string setting_base_url;
	bool has_base_url_setting = TryGetStringSetting(context, "jev_base_url", setting_base_url);
	auto env_base_url = GetEnv("TYPESAFE_BASE_URL");
	string lookup_url =
	    has_base_url_setting ? setting_base_url : (env_base_url.empty() ? string(DEFAULT_BASE_URL) : env_base_url);

	unique_ptr<SecretEntry> secret_entry;
	const KeyValueSecret *secret = nullptr;
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto match = secret_manager.LookupSecret(transaction, lookup_url, "typesafe");
	if (match.HasMatch()) {
		secret_entry = std::move(match.secret_entry);
		secret = dynamic_cast<const KeyValueSecret *>(secret_entry->secret.get());
	}

	auto resolve = [&](const string &setting, const string &secret_key, const char *env, const string &fallback) {
		string out;
		if (TryGetStringSetting(context, setting, out)) {
			return out;
		}
		if (TryGetSecretString(secret, secret_key, out)) {
			return out;
		}
		if (env) {
			out = GetEnv(env);
			if (!out.empty()) {
				return out;
			}
		}
		return fallback;
	};

	config.api_key = resolve("jev_api_key", "api_key", "TYPESAFE_API_KEY", "");
	config.base_url = StripTrailingSlashes(resolve("jev_base_url", "base_url", "TYPESAFE_BASE_URL", DEFAULT_BASE_URL));
	config.model = resolve("jev_model", "model", "TYPESAFE_DEFAULT_MODEL", DEFAULT_MODEL);
	config.http_proxy = resolve("jev_http_proxy", "http_proxy", nullptr, "");
	config.http_proxy_username = resolve("jev_http_proxy_username", "http_proxy_username", nullptr, "");
	config.http_proxy_password = resolve("jev_http_proxy_password", "http_proxy_password", nullptr, "");
	config.ca_cert_file = resolve("jev_ca_cert_file", "ca_cert_file", nullptr, "");

	Value verify;
	if (context.TryGetCurrentSetting("jev_verify_ssl", verify) && !verify.IsNull()) {
		config.verify_ssl = verify.GetValue<bool>();
	}
	TryGetIntSetting(context, "jev_timeout_ms", config.timeout_ms);
	TryGetIntSetting(context, "jev_max_retries", config.max_retries);
	TryGetIntSetting(context, "jev_max_concurrency", config.max_concurrency);

	if (config.timeout_ms <= 0) {
		throw InvalidInputException("jev_timeout_ms must be positive, got %lld", config.timeout_ms);
	}
	if (config.max_retries < 0) {
		throw InvalidInputException("jev_max_retries must be non-negative, got %lld", config.max_retries);
	}
	if (config.max_concurrency <= 0) {
		throw InvalidInputException("jev_max_concurrency must be positive, got %lld", config.max_concurrency);
	}
	return config;
}

void JevConfig::RequireApiKey() const {
	if (api_key.empty()) {
		throw InvalidInputException(
		    "No TypeSafe API key was provided. Use SET jev_api_key = '...', "
		    "CREATE SECRET (TYPE typesafe, API_KEY '...'), or set the TYPESAFE_API_KEY environment variable.");
	}
}

bool JevConfig::operator==(const JevConfig &other) const {
	return api_key == other.api_key && base_url == other.base_url && model == other.model &&
	       http_proxy == other.http_proxy && http_proxy_username == other.http_proxy_username &&
	       http_proxy_password == other.http_proxy_password && ca_cert_file == other.ca_cert_file &&
	       verify_ssl == other.verify_ssl && timeout_ms == other.timeout_ms && max_retries == other.max_retries &&
	       max_concurrency == other.max_concurrency;
}

//===--------------------------------------------------------------------===//
// URL / proxy helpers
//===--------------------------------------------------------------------===//

static void SplitBaseUrl(const string &url, string &scheme_host_port, string &path_prefix) {
	auto scheme_end = url.find("://");
	if (scheme_end == string::npos) {
		throw InvalidInputException("jev_base_url must start with http:// or https://, got '%s'", url);
	}
	auto scheme = StringUtil::Lower(url.substr(0, scheme_end));
	if (scheme != "http" && scheme != "https") {
		throw InvalidInputException("jev_base_url must start with http:// or https://, got '%s'", url);
	}
	auto path_start = url.find('/', scheme_end + 3);
	if (path_start == string::npos) {
		scheme_host_port = url;
		path_prefix = "";
	} else {
		scheme_host_port = url.substr(0, path_start);
		path_prefix = url.substr(path_start);
	}
}

static string HostOf(const string &scheme_host_port) {
	auto host = scheme_host_port.substr(scheme_host_port.find("://") + 3);
	if (!host.empty() && host[0] == '[') {
		auto end = host.find(']');
		return host.substr(1, end == string::npos ? string::npos : end - 1);
	}
	auto colon = host.find(':');
	return StringUtil::Lower(colon == string::npos ? host : host.substr(0, colon));
}

//! Whether `host` is excluded from proxying by a NO_PROXY-style list (exact names and domain suffixes).
static bool BypassProxy(const string &host) {
	auto no_proxy = GetEnv("NO_PROXY");
	if (no_proxy.empty()) {
		no_proxy = GetEnv("no_proxy");
	}
	for (auto entry : StringUtil::Split(no_proxy, ',')) {
		StringUtil::Trim(entry);
		entry = StringUtil::Lower(entry);
		if (entry.empty()) {
			continue;
		}
		if (entry == "*") {
			return true;
		}
		if (StringUtil::StartsWith(entry, "*.")) {
			entry = entry.substr(1);
		}
		if (entry[0] == '.') {
			if (StringUtil::EndsWith(host, entry) || host == entry.substr(1)) {
				return true;
			}
		} else if (host == entry || StringUtil::EndsWith(host, "." + entry)) {
			return true;
		}
	}
	return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

struct ProxyInfo {
	string host;
	int port = 0;
	string username;
	string password;
};

static bool ResolveProxy(const JevConfig &config, const string &scheme_host_port, ProxyInfo &out) {
	string proxy = config.http_proxy;
	auto https = StringUtil::StartsWith(StringUtil::Lower(scheme_host_port), "https://");
	if (proxy.empty()) {
		if (BypassProxy(HostOf(scheme_host_port))) {
			return false;
		}
		proxy = https ? GetEnv("HTTPS_PROXY") : GetEnv("HTTP_PROXY");
		if (proxy.empty()) {
			proxy = https ? GetEnv("https_proxy") : GetEnv("http_proxy");
		}
	}
	if (proxy.empty()) {
		return false;
	}
	auto scheme_end = proxy.find("://");
	if (scheme_end != string::npos) {
		proxy = proxy.substr(scheme_end + 3);
	}
	while (!proxy.empty() && proxy.back() == '/') {
		proxy.pop_back();
	}
	auto at = proxy.rfind('@');
	if (at != string::npos) {
		auto credentials = proxy.substr(0, at);
		proxy = proxy.substr(at + 1);
		auto colon = credentials.find(':');
		out.username = credentials.substr(0, colon);
		if (colon != string::npos) {
			out.password = credentials.substr(colon + 1);
		}
	}
	auto colon = proxy.rfind(':');
	if (colon != string::npos && proxy.find(']', colon) == string::npos) {
		out.host = proxy.substr(0, colon);
		out.port = std::atoi(proxy.substr(colon + 1).c_str());
	} else {
		out.host = proxy;
		out.port = 80;
	}
	if (!config.http_proxy_username.empty()) {
		out.username = config.http_proxy_username;
		out.password = config.http_proxy_password;
	}
	return !out.host.empty();
}

//===--------------------------------------------------------------------===//
// Error handling
//===--------------------------------------------------------------------===//

//! Extract a readable message from an error body: {"error": "..."}, {"error": {"message": ...}},
//! {"message": ...}, {"detail": ...} or FastAPI-style validation errors.
static string ExtractErrorMessage(const string &body) {
	if (body.empty()) {
		return "(no body)";
	}
	auto doc = yyjson_read(body.c_str(), body.size(), 0);
	if (!doc) {
		return body.size() > 200 ? body.substr(0, 200) + "..." : body;
	}
	string result;
	auto root = yyjson_doc_get_root(doc);
	if (yyjson_is_obj(root)) {
		auto error = yyjson_obj_get(root, "error");
		auto message = yyjson_obj_get(root, "message");
		auto detail = yyjson_obj_get(root, "detail");
		if (yyjson_is_str(error)) {
			result = yyjson_get_str(error);
		} else if (yyjson_is_obj(error) && yyjson_is_str(yyjson_obj_get(error, "message"))) {
			result = yyjson_get_str(yyjson_obj_get(error, "message"));
		} else if (yyjson_is_str(message)) {
			result = yyjson_get_str(message);
		} else if (yyjson_is_str(detail)) {
			result = yyjson_get_str(detail);
		} else if (yyjson_is_obj(detail) && yyjson_is_str(yyjson_obj_get(detail, "message"))) {
			result = yyjson_get_str(yyjson_obj_get(detail, "message"));
		} else if (yyjson_is_arr(detail)) {
			size_t idx, max;
			yyjson_val *item;
			yyjson_arr_foreach(detail, idx, max, item) {
				auto msg = yyjson_obj_get(item, "msg");
				if (!yyjson_is_str(msg)) {
					continue;
				}
				string loc;
				auto loc_arr = yyjson_obj_get(item, "loc");
				if (yyjson_is_arr(loc_arr)) {
					size_t lidx, lmax;
					yyjson_val *part;
					yyjson_arr_foreach(loc_arr, lidx, lmax, part) {
						string piece = yyjson_is_str(part)
						                   ? string(yyjson_get_str(part))
						                   : (yyjson_is_int(part) ? to_string(yyjson_get_int(part)) : "");
						if (piece.empty() || piece == "body") {
							continue;
						}
						loc += loc.empty() ? piece : "." + piece;
					}
				}
				if (!result.empty()) {
					result += "; ";
				}
				result += loc.empty() ? string(yyjson_get_str(msg)) : loc + ": " + yyjson_get_str(msg);
			}
		}
	}
	yyjson_doc_free(doc);
	if (result.empty()) {
		result = body.size() > 200 ? body.substr(0, 200) + "..." : body;
	}
	return result;
}

static string DescribeStatus(int status) {
	switch (status) {
	case 400:
		return "Bad Request";
	case 401:
		return "Authentication failed";
	case 403:
		return "Permission denied";
	case 404:
		return "Not found";
	case 422:
		return "Unprocessable entity";
	case 429:
		return "Rate limit exceeded";
	default:
		return status >= 500 ? "Server error" : "HTTP error";
	}
}

static bool IsRetryableStatus(int status) {
	return status == 408 || status == 429 || (status >= 500 && status <= 599);
}

//! Server-requested delay from `retry-after-ms` or `Retry-After` (seconds), or -1 when absent/invalid.
static int64_t ParseRetryAfterMs(const httplib::Result &res) {
	if (!res) {
		return -1;
	}
	if (res->has_header("retry-after-ms")) {
		char *end = nullptr;
		auto raw = res->get_header_value("retry-after-ms");
		auto ms = std::strtod(raw.c_str(), &end);
		if (end != raw.c_str() && ms >= 0) {
			return static_cast<int64_t>(ms);
		}
	}
	if (res->has_header("retry-after")) {
		char *end = nullptr;
		auto raw = res->get_header_value("retry-after");
		auto seconds = std::strtod(raw.c_str(), &end);
		if (end != raw.c_str() && seconds >= 0) {
			return static_cast<int64_t>(seconds * 1000);
		}
	}
	return -1;
}

static int64_t BackoffMs(int64_t attempt, int64_t retry_after_ms) {
	if (retry_after_ms >= 0 && retry_after_ms <= MAX_RETRY_AFTER_MS) {
		return retry_after_ms;
	}
	thread_local std::mt19937_64 rng(std::random_device {}());
	std::uniform_real_distribution<double> dist(0.0, 1.0);
	auto exponential = BACKOFF_INITIAL_MS;
	for (int64_t i = 0; i < attempt && exponential < BACKOFF_MAX_MS; i++) {
		exponential *= 2;
	}
	exponential = MinValue<int64_t>(exponential, BACKOFF_MAX_MS);
	return static_cast<int64_t>(static_cast<double>(exponential) * (1.0 - dist(rng) * BACKOFF_JITTER));
}

//===--------------------------------------------------------------------===//
// Transport
//===--------------------------------------------------------------------===//

static unique_ptr<httplib::Client> MakeHttpClient(const JevClient &client) {
	auto &config = client.Config();
	auto http = make_uniq<httplib::Client>(client.SchemeHostPort());
	auto timeout = std::chrono::milliseconds(config.timeout_ms);
	http->set_connection_timeout(timeout);
	http->set_read_timeout(timeout);
	http->set_write_timeout(timeout);
	http->set_keep_alive(true);
	http->set_follow_location(false);
	http->enable_server_certificate_verification(config.verify_ssl);
	if (!config.ca_cert_file.empty()) {
		http->set_ca_cert_path(config.ca_cert_file);
	}
	ProxyInfo proxy;
	if (ResolveProxy(config, client.SchemeHostPort(), proxy)) {
		http->set_proxy(proxy.host, proxy.port);
		if (!proxy.username.empty()) {
			http->set_proxy_basic_auth(proxy.username, proxy.password);
		}
	}
	return http;
}

static string Send(const JevClient &client, httplib::Client &http, const string &method, const string &path,
                   const string *body) {
	auto &config = client.Config();
	auto full_path = client.PathPrefix() + path;
	httplib::Headers base_headers = {{"Authorization", "Bearer " + config.api_key},
	                                 {"Accept", "application/json"},
	                                 {"User-Agent", USER_AGENT},
	                                 {"X-TypeSafe-SDK", USER_AGENT}};

	for (int64_t attempt = 0;; attempt++) {
		auto retries_left = config.max_retries - attempt;
		auto headers = base_headers;
		if (attempt > 0) {
			headers.emplace("X-TypeSafe-Retry-Count", to_string(attempt));
		}
		auto res =
		    method == "POST" ? http.Post(full_path, headers, *body, "application/json") : http.Get(full_path, headers);
		if (!res) {
			auto error = httplib::to_string(res.error());
			if (retries_left <= 0) {
				throw IOException("TypeSafe API request %s %s failed: %s", method, config.base_url + path, error);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(BackoffMs(attempt, -1)));
			continue;
		}
		if (res->status >= 200 && res->status < 300) {
			return res->body;
		}
		if (retries_left > 0 && IsRetryableStatus(res->status)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(BackoffMs(attempt, ParseRetryAfterMs(res))));
			continue;
		}
		auto request_id = res->get_header_value("x-typesafe-request-id");
		auto message = StringUtil::Format("TypeSafe API error %d (%s): %s", res->status, DescribeStatus(res->status),
		                                  ExtractErrorMessage(res->body));
		if (!request_id.empty()) {
			message += " [request " + request_id + "]";
		}
		if (res->status == 400 || res->status == 404 || res->status == 422) {
			throw InvalidInputException(message);
		}
		if (res->status == 401 || res->status == 403) {
			throw PermissionException(message);
		}
		throw IOException(message);
	}
}

JevClient::JevClient(const JevConfig &config_p) : config(config_p) {
	SplitBaseUrl(config.base_url, scheme_host_port, path_prefix);
}

string JevClient::SystemOne(const string &request_body) const {
	config.RequireApiKey();
	auto http = MakeHttpClient(*this);
	return Send(*this, *http, "POST", "/v1/systemone", &request_body);
}

string JevClient::ListModels() const {
	config.RequireApiKey();
	auto http = MakeHttpClient(*this);
	return Send(*this, *http, "GET", "/v1/models", nullptr);
}

vector<string> JevClient::SystemOneMany(const vector<string> &request_bodies) const {
	vector<string> results(request_bodies.size());
	if (request_bodies.empty()) {
		return results;
	}
	config.RequireApiKey();
	auto worker_count = MinValue<idx_t>(request_bodies.size(), NumericCast<idx_t>(config.max_concurrency));

	std::atomic<idx_t> next {0};
	std::atomic<bool> failed {false};
	vector<string> errors(worker_count);
	vector<ExceptionType> error_types(worker_count, ExceptionType::IO);

	auto work = [&](idx_t worker_idx) {
		try {
			auto http = MakeHttpClient(*this);
			while (!failed.load()) {
				auto idx = next.fetch_add(1);
				if (idx >= request_bodies.size()) {
					break;
				}
				results[idx] = Send(*this, *http, "POST", "/v1/systemone", &request_bodies[idx]);
			}
		} catch (std::exception &ex) {
			ErrorData error(ex);
			errors[worker_idx] = error.RawMessage();
			error_types[worker_idx] = error.Type();
			failed = true;
		}
	};

	if (worker_count == 1) {
		work(0);
	} else {
		vector<std::thread> threads;
		threads.reserve(worker_count);
		for (idx_t i = 0; i < worker_count; i++) {
			threads.emplace_back(work, i);
		}
		for (auto &thread : threads) {
			thread.join();
		}
	}
	for (idx_t i = 0; i < worker_count; i++) {
		if (!errors[i].empty()) {
			switch (error_types[i]) {
			case ExceptionType::INVALID_INPUT:
				throw InvalidInputException(errors[i]);
			case ExceptionType::PERMISSION:
				throw PermissionException(errors[i]);
			default:
				throw IOException(errors[i]);
			}
		}
	}
	return results;
}

} // namespace duckdb
