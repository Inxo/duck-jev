#define DUCKDB_EXTENSION_MAIN

#include "jev_extension.hpp"
#include "jev_functions.hpp"

#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// CREATE SECRET (TYPE typesafe, API_KEY '...', BASE_URL '...', MODEL '...')
//===--------------------------------------------------------------------===//

static const char *const SECRET_KEYS[] = {
    "api_key", "base_url", "model", "http_proxy", "http_proxy_username", "http_proxy_password", "ca_cert_file"};

static unique_ptr<BaseSecret> CreateTypeSafeSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
	auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	for (auto key : SECRET_KEYS) {
		secret->TrySetValue(key, input);
	}
	secret->redact_keys = {"api_key", "http_proxy_password"};
	return std::move(secret);
}

static void RegisterSecret(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = "typesafe";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	secret_type.extension = "jev";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction config_function;
	config_function.secret_type = "typesafe";
	config_function.provider = "config";
	config_function.function = CreateTypeSafeSecretFromConfig;
	for (auto key : SECRET_KEYS) {
		config_function.named_parameters[key] = LogicalType::VARCHAR;
	}
	loader.RegisterFunction(config_function);
}

//===--------------------------------------------------------------------===//
// Settings
//===--------------------------------------------------------------------===//

static void RegisterSettings(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("jev_api_key",
	                          "TypeSafe API key (falls back to a typesafe secret, then TYPESAFE_API_KEY)",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("jev_base_url",
	                          "TypeSafe API root (falls back to a typesafe secret, TYPESAFE_BASE_URL, then "
	                          "https://api.typesafe.ai)",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("jev_model",
	                          "Model used for requests (falls back to a typesafe secret, TYPESAFE_DEFAULT_MODEL, then "
	                          "jev-latest)",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("jev_timeout_ms", "Timeout per HTTP attempt in milliseconds", LogicalType::BIGINT,
	                          Value::BIGINT(30000));
	config.AddExtensionOption("jev_max_retries", "Retries after the first attempt for 408/429/5xx and network errors",
	                          LogicalType::BIGINT, Value::BIGINT(2));
	config.AddExtensionOption("jev_max_concurrency", "Maximum parallel API requests per evaluated vector",
	                          LogicalType::BIGINT, Value::BIGINT(8));
	config.AddExtensionOption("jev_http_proxy",
	                          "HTTP proxy as host:port (defaults to HTTPS_PROXY / HTTP_PROXY, honouring NO_PROXY)",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("jev_http_proxy_username", "Username for the HTTP proxy", LogicalType::VARCHAR,
	                          Value(""));
	config.AddExtensionOption("jev_http_proxy_password", "Password for the HTTP proxy", LogicalType::VARCHAR,
	                          Value(""));
	config.AddExtensionOption("jev_ca_cert_file", "Path to a CA bundle used to verify the API's TLS certificate",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("jev_verify_ssl", "Verify the API's TLS certificate", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(true));
}

static void LoadInternal(ExtensionLoader &loader) {
	RegisterSettings(loader);
	RegisterSecret(loader);
	JevFunctions::RegisterScalarFunctions(loader);
	JevFunctions::RegisterTableFunctions(loader);
}

void JevExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string JevExtension::Name() {
	return "jev";
}

std::string JevExtension::Version() const {
#ifdef EXT_VERSION_JEV
	return EXT_VERSION_JEV;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(jev, loader) {
	duckdb::LoadInternal(loader);
}
}
