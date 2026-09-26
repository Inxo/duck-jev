#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Connection settings for the TypeSafe API, resolved once per query at bind time.
struct JevConfig {
	string api_key;
	string base_url;
	string model;
	string http_proxy;
	string http_proxy_username;
	string http_proxy_password;
	string ca_cert_file;
	bool verify_ssl = true;
	int64_t timeout_ms = 30000;
	int64_t max_retries = 2;
	int64_t max_concurrency = 8;

	//! Resolve settings in priority order: SET option > TYPE typesafe secret > environment > default.
	static JevConfig Resolve(ClientContext &context);

	void RequireApiKey() const;

	bool operator==(const JevConfig &other) const;
};

//! Minimal HTTP client for the TypeSafe API (https://docs.typesafe.ai/api).
//! Mirrors the retry behaviour of the official SDKs: 408/429/5xx and connection errors are retried with
//! capped exponential backoff and jitter, honouring `retry-after-ms` / `Retry-After`.
class JevClient {
public:
	explicit JevClient(const JevConfig &config);

	//! POST /v1/systemone with a JSON body; returns the raw JSON response body or throws.
	string SystemOne(const string &request_body) const;
	//! GET /v1/models; returns the raw JSON response body or throws.
	string ListModels() const;

	//! Run many /v1/systemone requests concurrently (bounded by max_concurrency). Output order matches input.
	vector<string> SystemOneMany(const vector<string> &request_bodies) const;

	const JevConfig &Config() const {
		return config;
	}
	const string &SchemeHostPort() const {
		return scheme_host_port;
	}
	const string &PathPrefix() const {
		return path_prefix;
	}

private:
	JevConfig config;
	string scheme_host_port;
	string path_prefix;
};

} // namespace duckdb
