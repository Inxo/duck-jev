# jev — DuckDB extension for TypeSafe Jev

This extension lets you ask typed questions to the TypeSafe AI
[Jev](https://docs.typesafe.ai/) model (System One) directly from SQL, through the HTTP API
`POST /v1/systemone`, and get the answers back as regular DuckDB values:
a probability (`noul`), a selected label (`choice`) or a position on a rubric (`score`).

```sql
LOAD jev;
CREATE SECRET (TYPE typesafe, API_KEY 'ts-...');

SELECT id,
       jev_choice(body, 'What is this ticket about?', ['billing', 'technical', 'other']) AS topic,
       jev_noul(body, 'Does the customer explicitly ask for a refund?')                 AS p_refund,
       jev_score(body, 'How badly is the customer blocked?',
                 ['Cosmetic', 'Workaround available', 'Blocks the task'])             AS severity
FROM tickets;
```

## Functions

| Function | Returns | API question |
|---|---|---|
| `jev_noul(state, instructions)` | `DOUBLE` — probability of a "yes" answer (0…1) | `noul` |
| `jev_noul(state, instructions, true_desc, false_desc)` | `DOUBLE` | `noul` with outcome descriptions |
| `jev_choice(state, instructions, labels VARCHAR[])` | `VARCHAR` — the selected label | `choice` |
| `jev_choice(state, instructions, MAP(label → description))` | `VARCHAR` | `choice` with label descriptions |
| `jev_choice_detail(...)` (same arguments) | `STRUCT(choice VARCHAR, confidence DOUBLE, probabilities MAP(VARCHAR, DOUBLE))` | `choice` |
| `jev_score(state, instructions, rubric VARCHAR[])` | `DOUBLE` — expected score from 0 to `len(rubric)-1` | `score` |
| `jev_score_detail(state, instructions, rubric)` | `STRUCT(score DOUBLE, confidence DOUBLE, probabilities DOUBLE[])` (index = score) | `score` |
| `jev_system_one(state, questions [, model])` | `JSON` — the full API response (`model`, `answers`, `usage`) | any |
| `jev_models()` | table `name, description, release_date` | `GET /v1/models` |

* `state` and `instructions` are text. If an argument has the `JSON` type, it is sent as a JSON
  object/array rather than a string: `jev_noul(to_json(t), 'Is this order suspicious?')`.
* A `score` rubric is a list of at least two descriptions, indexed from zero.
* `jev_system_one` takes a question object in the API format, for example
  `'{"refund": {"type": "noul", "instructions": "Refund?"}, "topic": {"type": "choice", "criteria": {"billing": null, "other": null}}}'`.
* `NULL` in any argument → `NULL`, without calling the API.

### How requests are executed

For each vector (up to 2048 rows) the extension builds one request body per row, **removes duplicates**
(identical `state` + question pairs are sent once) and sends the requests **in parallel**
(`jev_max_concurrency`, 8 per vector by default; DuckDB also processes vectors on multiple threads).
408/429/5xx responses and network failures are retried with exponential backoff and jitter,
honouring the `Retry-After` / `retry-after-ms` headers — the same way the official SDKs do.

## Configuration

Precedence: `SET jev_*` → `TYPE typesafe` secret → environment variables → default value.

```sql
-- a secret (recommended; can be made PERSISTENT)
CREATE SECRET typesafe (TYPE typesafe, API_KEY 'ts-...', MODEL 'jev-latest');

-- or session settings
SET jev_api_key = 'ts-...';
SET jev_model = 'jev-1.13';
```

| Setting | Secret key | Env | Default |
|---|---|---|---|
| `jev_api_key` | `api_key` | `TYPESAFE_API_KEY` | — (required) |
| `jev_base_url` | `base_url` | `TYPESAFE_BASE_URL` | `https://api.typesafe.ai` |
| `jev_model` | `model` | `TYPESAFE_DEFAULT_MODEL` | `jev-latest` |
| `jev_http_proxy` (`host:port`) | `http_proxy` | `HTTPS_PROXY` / `HTTP_PROXY` (+ `NO_PROXY`) | — |
| `jev_http_proxy_username` / `_password` | `http_proxy_username` / `_password` | — | — |
| `jev_ca_cert_file` | `ca_cert_file` | — | system certificates |
| `jev_verify_ssl` | — | — | `true` |
| `jev_timeout_ms` | — | — | `30000` (per attempt) |
| `jev_max_retries` | — | — | `2` |
| `jev_max_concurrency` | — | — | `8` |

## Building

You need CMake, a C++17 compiler, Ninja (optional) and OpenSSL (a dev package or vcpkg).

```sh
git clone --recurse-submodules https://github.com/inxo/duck-jav.git
cd duck-jav
GEN=ninja make            # build/release/duckdb and build/release/extension/jev/jev.duckdb_extension
```

Loading the built extension into another DuckDB of the same version (v1.5.5):

```sh
duckdb -unsigned -c "LOAD 'build/release/extension/jev/jev.duckdb_extension'; SELECT * FROM jev_models();"
```

## Tests

```sh
make test                      # offline tests (test/sql/jev.test)
scripts/run_mock_tests.sh      # + end-to-end tests against a local mock API server (test/sql/jev_mock.test)
```

The mock server (`test/mock/mock_typesafe_server.py`) implements `/v1/systemone` and `/v1/models`
with deterministic answers, 401/422 errors, and a 503 with `Retry-After`.

## Layout

* `src/jev_extension.cpp` — registers the functions, settings and the `typesafe` secret type.
* `src/jev_functions.cpp` — SQL functions: building JSON requests, deduplication, parsing responses.
* `src/jev_client.cpp` — HTTP client (cpp-httplib + OpenSSL from the DuckDB tree), retries, proxy, errors.
