#include "jev_functions.hpp"
#include "jev_client.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "yyjson.hpp"

#include <cstdlib>

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

//! Answers are requested under this name and read back from `answers.<name>`.
static constexpr const char *QUESTION_NAME = "q";

//===--------------------------------------------------------------------===//
// JSON helpers
//===--------------------------------------------------------------------===//

struct JsonDoc {
	explicit JsonDoc(yyjson_doc *doc_p) : doc(doc_p) {
	}
	~JsonDoc() {
		if (doc) {
			yyjson_doc_free(doc);
		}
	}
	JsonDoc(const JsonDoc &) = delete;
	JsonDoc &operator=(const JsonDoc &) = delete;

	static JsonDoc Parse(const string &text, const char *what) {
		auto doc = yyjson_read(text.c_str(), text.size(), 0);
		if (!doc) {
			throw InvalidInputException("%s is not valid JSON: %s", what,
			                            text.size() > 200 ? text.substr(0, 200) + "..." : text);
		}
		return JsonDoc(doc);
	}
	JsonDoc(JsonDoc &&other) noexcept : doc(other.doc) {
		other.doc = nullptr;
	}

	yyjson_val *Root() const {
		return yyjson_doc_get_root(doc);
	}

	yyjson_doc *doc;
};

struct JsonMutDoc {
	JsonMutDoc() : doc(yyjson_mut_doc_new(nullptr)) {
	}
	~JsonMutDoc() {
		yyjson_mut_doc_free(doc);
	}
	JsonMutDoc(const JsonMutDoc &) = delete;
	JsonMutDoc &operator=(const JsonMutDoc &) = delete;

	yyjson_mut_val *Str(const string &value) {
		return yyjson_mut_strncpy(doc, value.c_str(), value.size());
	}
	//! Text, or parsed JSON when the SQL argument has the JSON type.
	yyjson_mut_val *Entry(const Value &value, bool is_json, const char *what) {
		if (value.IsNull()) {
			return yyjson_mut_null(doc);
		}
		auto text = StringValue::Get(value);
		if (!is_json) {
			return Str(text);
		}
		auto parsed = JsonDoc::Parse(text, what);
		return yyjson_val_mut_copy(doc, parsed.Root());
	}
	void Add(yyjson_mut_val *obj, const string &key, yyjson_mut_val *val) {
		yyjson_mut_obj_add(obj, Str(key), val);
	}
	string Write() {
		size_t len;
		auto data = yyjson_mut_write(doc, 0, &len);
		if (!data) {
			throw InternalException("jev: failed to serialize request JSON");
		}
		string result(data, len);
		free(data);
		return result;
	}

	yyjson_mut_doc *doc;
};

static double GetNumber(yyjson_val *obj, const char *key, const char *kind) {
	auto val = yyjson_obj_get(obj, key);
	if (!yyjson_is_num(val)) {
		throw IOException("Unexpected TypeSafe response: %s answer has no numeric '%s' field", kind, key);
	}
	return yyjson_get_num(val);
}

//===--------------------------------------------------------------------===//
// Bind data
//===--------------------------------------------------------------------===//

enum class JevQuestionKind : uint8_t { NOUL, CHOICE, SCORE, RAW };
enum class JevOutput : uint8_t { NOUL, CHOICE, CHOICE_DETAIL, SCORE, SCORE_DETAIL, RAW };

struct JevBindData : public FunctionData {
	JevConfig config;
	JevQuestionKind kind;
	JevOutput output;
	//! Per-argument: whether the SQL argument had the JSON logical type
	vector<bool> is_json;
	//! Choice criteria are given as MAP(label -> description) rather than a list of labels
	bool criteria_is_map = false;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<JevBindData>();
		result->config = config;
		result->kind = kind;
		result->output = output;
		result->is_json = is_json;
		result->criteria_is_map = criteria_is_map;
		return std::move(result);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<JevBindData>();
		return config == other.config && kind == other.kind && output == other.output && is_json == other.is_json &&
		       criteria_is_map == other.criteria_is_map;
	}
};

static unique_ptr<FunctionData> JevBind(ClientContext &context, JevQuestionKind kind, JevOutput output,
                                        vector<unique_ptr<Expression>> &arguments) {
	auto result = make_uniq<JevBindData>();
	result->config = JevConfig::Resolve(context);
	result->kind = kind;
	result->output = output;
	for (auto &arg : arguments) {
		result->is_json.push_back(arg->return_type.IsJSONType());
	}
	if (kind == JevQuestionKind::CHOICE && arguments.size() > 2) {
		result->criteria_is_map = arguments[2]->return_type.id() == LogicalTypeId::MAP;
	}
	return std::move(result);
}

template <JevQuestionKind KIND, JevOutput OUTPUT>
static unique_ptr<FunctionData> JevBindTemplate(ClientContext &context, ScalarFunction &bound_function,
                                                vector<unique_ptr<Expression>> &arguments) {
	return JevBind(context, KIND, OUTPUT, arguments);
}

//===--------------------------------------------------------------------===//
// Request construction
//===--------------------------------------------------------------------===//

static yyjson_mut_val *BuildQuestion(JsonMutDoc &doc, const JevBindData &bind, const vector<Value> &row) {
	auto question = yyjson_mut_obj(doc.doc);
	switch (bind.kind) {
	case JevQuestionKind::NOUL: {
		doc.Add(question, "type", doc.Str("noul"));
		doc.Add(question, "instructions", doc.Entry(row[1], bind.is_json[1], "instructions"));
		if (row.size() >= 4) {
			auto criteria = yyjson_mut_obj(doc.doc);
			doc.Add(criteria, "true", doc.Entry(row[2], bind.is_json[2], "true criterion"));
			doc.Add(criteria, "false", doc.Entry(row[3], bind.is_json[3], "false criterion"));
			doc.Add(question, "criteria", criteria);
		}
		break;
	}
	case JevQuestionKind::CHOICE: {
		doc.Add(question, "type", doc.Str("choice"));
		doc.Add(question, "instructions", doc.Entry(row[1], bind.is_json[1], "instructions"));
		auto criteria = yyjson_mut_obj(doc.doc);
		idx_t label_count = 0;
		if (bind.criteria_is_map) {
			for (auto &entry : MapValue::GetChildren(row[2])) {
				auto &kv = StructValue::GetChildren(entry);
				if (kv[0].IsNull() || StringValue::Get(kv[0]).empty()) {
					throw InvalidInputException("jev_choice: choice labels must be non-empty strings");
				}
				doc.Add(criteria, StringValue::Get(kv[0]), doc.Entry(kv[1], false, "choice description"));
				label_count++;
			}
		} else {
			for (auto &label : ListValue::GetChildren(row[2])) {
				if (label.IsNull() || StringValue::Get(label).empty()) {
					throw InvalidInputException("jev_choice: choice labels must be non-empty strings");
				}
				doc.Add(criteria, StringValue::Get(label), yyjson_mut_null(doc.doc));
				label_count++;
			}
		}
		if (label_count == 0) {
			throw InvalidInputException("jev_choice: at least one choice label is required");
		}
		doc.Add(question, "criteria", criteria);
		break;
	}
	case JevQuestionKind::SCORE: {
		doc.Add(question, "type", doc.Str("score"));
		doc.Add(question, "instructions", doc.Entry(row[1], bind.is_json[1], "instructions"));
		auto &rubric = ListValue::GetChildren(row[2]);
		if (rubric.size() < 2) {
			throw InvalidInputException("jev_score: the rubric needs at least two levels (indexed by score from "
			                            "zero), got %llu",
			                            rubric.size());
		}
		auto criteria = yyjson_mut_arr(doc.doc);
		for (auto &level : rubric) {
			yyjson_mut_arr_append(criteria, doc.Entry(level, false, "score description"));
		}
		doc.Add(question, "criteria", criteria);
		break;
	}
	default:
		throw InternalException("jev: unexpected question kind");
	}
	return question;
}

//! Serialize the /v1/systemone request body for one row.
static string BuildRequestBody(const JevBindData &bind, const vector<Value> &row) {
	JsonMutDoc doc;
	auto root = yyjson_mut_obj(doc.doc);
	yyjson_mut_doc_set_root(doc.doc, root);

	string model = bind.config.model;
	yyjson_mut_val *questions;
	if (bind.kind == JevQuestionKind::RAW) {
		auto parsed = JsonDoc::Parse(StringValue::Get(row[1]), "questions");
		if (!yyjson_is_obj(parsed.Root()) || yyjson_obj_size(parsed.Root()) == 0) {
			throw InvalidInputException("jev_system_one: questions must be a non-empty JSON object keyed by name");
		}
		questions = yyjson_val_mut_copy(doc.doc, parsed.Root());
		if (row.size() >= 3) {
			model = StringValue::Get(row[2]);
		}
	} else {
		questions = yyjson_mut_obj(doc.doc);
		doc.Add(questions, QUESTION_NAME, BuildQuestion(doc, bind, row));
	}
	doc.Add(root, "model", doc.Str(model));
	doc.Add(root, "state", doc.Entry(row[0], bind.is_json[0], "state"));
	doc.Add(root, "questions", questions);
	return doc.Write();
}

//===--------------------------------------------------------------------===//
// Response decoding
//===--------------------------------------------------------------------===//

static Value DecodeResponse(const JevBindData &bind, const string &body) {
	if (bind.output == JevOutput::RAW) {
		return Value(body);
	}
	auto parsed = JsonDoc::Parse(body, "TypeSafe response");
	auto answers = yyjson_obj_get(parsed.Root(), "answers");
	auto answer = yyjson_obj_get(answers, QUESTION_NAME);
	if (!yyjson_is_obj(answer)) {
		throw IOException("Unexpected TypeSafe response (no answers.%s): %s", QUESTION_NAME,
		                  body.size() > 200 ? body.substr(0, 200) + "..." : body);
	}

	switch (bind.output) {
	case JevOutput::NOUL:
		return Value::DOUBLE(GetNumber(answer, "noul", "noul"));
	case JevOutput::CHOICE:
	case JevOutput::CHOICE_DETAIL: {
		auto choice = yyjson_obj_get(answer, "choice");
		if (!yyjson_is_str(choice)) {
			throw IOException("Unexpected TypeSafe response: choice answer has no 'choice' field");
		}
		Value label(string(yyjson_get_str(choice), yyjson_get_len(choice)));
		if (bind.output == JevOutput::CHOICE) {
			return label;
		}
		vector<Value> keys;
		vector<Value> values;
		auto probabilities = yyjson_obj_get(answer, "probabilities");
		size_t idx, max;
		yyjson_val *key, *val;
		yyjson_obj_foreach(probabilities, idx, max, key, val) {
			keys.emplace_back(string(yyjson_get_str(key), yyjson_get_len(key)));
			values.push_back(yyjson_is_num(val) ? Value::DOUBLE(yyjson_get_num(val)) : Value(LogicalType::DOUBLE));
		}
		child_list_t<Value> children;
		children.emplace_back("choice", label);
		children.emplace_back("confidence", Value::DOUBLE(GetNumber(answer, "confidence", "choice")));
		children.emplace_back(
		    "probabilities", Value::MAP(LogicalType::VARCHAR, LogicalType::DOUBLE, std::move(keys), std::move(values)));
		return Value::STRUCT(std::move(children));
	}
	case JevOutput::SCORE:
		return Value::DOUBLE(GetNumber(answer, "score", "score"));
	case JevOutput::SCORE_DETAIL: {
		// Probabilities are keyed by score ("0", "1", ...); return them as a list indexed by score.
		vector<Value> probabilities;
		auto probs = yyjson_obj_get(answer, "probabilities");
		size_t idx, max;
		yyjson_val *key, *val;
		yyjson_obj_foreach(probs, idx, max, key, val) {
			auto level = std::strtoull(yyjson_get_str(key), nullptr, 10);
			if (level > 10000) {
				continue;
			}
			if (probabilities.size() <= level) {
				probabilities.resize(level + 1, Value(LogicalType::DOUBLE));
			}
			probabilities[level] = yyjson_is_num(val) ? Value::DOUBLE(yyjson_get_num(val)) : Value(LogicalType::DOUBLE);
		}
		child_list_t<Value> children;
		children.emplace_back("score", Value::DOUBLE(GetNumber(answer, "score", "score")));
		children.emplace_back("confidence", Value::DOUBLE(GetNumber(answer, "confidence", "score")));
		children.emplace_back("probabilities", Value::LIST(LogicalType::DOUBLE, std::move(probabilities)));
		return Value::STRUCT(std::move(children));
	}
	default:
		throw InternalException("jev: unexpected output kind");
	}
}

//===--------------------------------------------------------------------===//
// Execution
//===--------------------------------------------------------------------===//

//! Evaluate a chunk: build one request per non-NULL row, deduplicate identical requests, send them concurrently
//! and decode each answer. A NULL in any argument yields NULL.
static void JevExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind = func_expr.bind_info->Cast<JevBindData>();

	auto all_constant = args.AllConstant();
	auto count = all_constant ? idx_t(1) : args.size();
	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}

	vector<string> unique_bodies;
	unordered_map<string, idx_t> body_index;
	vector<optional_idx> row_request(count);
	vector<Value> row(args.ColumnCount());
	for (idx_t i = 0; i < count; i++) {
		bool has_null = false;
		for (idx_t col = 0; col < args.ColumnCount(); col++) {
			row[col] = args.data[col].GetValue(i);
			has_null = has_null || row[col].IsNull();
		}
		if (has_null) {
			continue;
		}
		auto body = BuildRequestBody(bind, row);
		auto entry = body_index.find(body);
		if (entry == body_index.end()) {
			entry = body_index.emplace(body, unique_bodies.size()).first;
			unique_bodies.push_back(std::move(body));
		}
		row_request[i] = entry->second;
	}

	JevClient client(bind.config);
	auto responses = client.SystemOneMany(unique_bodies);

	vector<Value> decoded;
	decoded.reserve(responses.size());
	for (auto &response : responses) {
		decoded.push_back(DecodeResponse(bind, response).DefaultCastAs(result.GetType()));
	}
	for (idx_t i = 0; i < count; i++) {
		if (!row_request[i].IsValid()) {
			result.SetValue(i, Value(result.GetType()));
		} else {
			result.SetValue(i, decoded[row_request[i].GetIndex()]);
		}
	}
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

static LogicalType ChoiceDetailType() {
	child_list_t<LogicalType> children;
	children.emplace_back("choice", LogicalType::VARCHAR);
	children.emplace_back("confidence", LogicalType::DOUBLE);
	children.emplace_back("probabilities", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::DOUBLE));
	return LogicalType::STRUCT(std::move(children));
}

static LogicalType ScoreDetailType() {
	child_list_t<LogicalType> children;
	children.emplace_back("score", LogicalType::DOUBLE);
	children.emplace_back("confidence", LogicalType::DOUBLE);
	children.emplace_back("probabilities", LogicalType::LIST(LogicalType::DOUBLE));
	return LogicalType::STRUCT(std::move(children));
}

template <JevQuestionKind KIND, JevOutput OUTPUT>
static ScalarFunction MakeJevFunction(const vector<LogicalType> &arguments, const LogicalType &return_type) {
	ScalarFunction function("", arguments, return_type, JevExecute, JevBindTemplate<KIND, OUTPUT>);
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

static void RegisterSet(ExtensionLoader &loader, ScalarFunctionSet &set) {
	for (auto &function : set.functions) {
		function.name = set.name;
	}
	loader.RegisterFunction(set);
}

void JevFunctions::RegisterScalarFunctions(ExtensionLoader &loader) {
	auto varchar = LogicalType::VARCHAR;
	auto labels = LogicalType::LIST(LogicalType::VARCHAR);
	auto described_labels = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	auto rubric = LogicalType::LIST(LogicalType::VARCHAR);

	// jev_noul(state, instructions [, true_criterion, false_criterion]) -> probability of "yes"
	ScalarFunctionSet noul("jev_noul");
	noul.AddFunction(MakeJevFunction<JevQuestionKind::NOUL, JevOutput::NOUL>({varchar, varchar}, LogicalType::DOUBLE));
	noul.AddFunction(MakeJevFunction<JevQuestionKind::NOUL, JevOutput::NOUL>({varchar, varchar, varchar, varchar},
	                                                                         LogicalType::DOUBLE));
	RegisterSet(loader, noul);

	// jev_choice(state, instructions, labels | map(label -> description)) -> selected label
	ScalarFunctionSet choice("jev_choice");
	choice.AddFunction(
	    MakeJevFunction<JevQuestionKind::CHOICE, JevOutput::CHOICE>({varchar, varchar, labels}, varchar));
	choice.AddFunction(
	    MakeJevFunction<JevQuestionKind::CHOICE, JevOutput::CHOICE>({varchar, varchar, described_labels}, varchar));
	RegisterSet(loader, choice);

	ScalarFunctionSet choice_detail("jev_choice_detail");
	choice_detail.AddFunction(MakeJevFunction<JevQuestionKind::CHOICE, JevOutput::CHOICE_DETAIL>(
	    {varchar, varchar, labels}, ChoiceDetailType()));
	choice_detail.AddFunction(MakeJevFunction<JevQuestionKind::CHOICE, JevOutput::CHOICE_DETAIL>(
	    {varchar, varchar, described_labels}, ChoiceDetailType()));
	RegisterSet(loader, choice_detail);

	// jev_score(state, instructions, rubric) -> expected score (0 .. len(rubric) - 1)
	ScalarFunctionSet score("jev_score");
	score.AddFunction(
	    MakeJevFunction<JevQuestionKind::SCORE, JevOutput::SCORE>({varchar, varchar, rubric}, LogicalType::DOUBLE));
	RegisterSet(loader, score);

	ScalarFunctionSet score_detail("jev_score_detail");
	score_detail.AddFunction(MakeJevFunction<JevQuestionKind::SCORE, JevOutput::SCORE_DETAIL>(
	    {varchar, varchar, rubric}, ScoreDetailType()));
	RegisterSet(loader, score_detail);

	// jev_system_one(state, questions_json [, model]) -> raw JSON response
	ScalarFunctionSet system_one("jev_system_one");
	system_one.AddFunction(
	    MakeJevFunction<JevQuestionKind::RAW, JevOutput::RAW>({varchar, varchar}, LogicalType::JSON()));
	system_one.AddFunction(
	    MakeJevFunction<JevQuestionKind::RAW, JevOutput::RAW>({varchar, varchar, varchar}, LogicalType::JSON()));
	RegisterSet(loader, system_one);
}

//===--------------------------------------------------------------------===//
// jev_models()
//===--------------------------------------------------------------------===//

struct JevModelsBindData : public TableFunctionData {
	vector<vector<Value>> rows;
};

struct JevModelsState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static unique_ptr<FunctionData> JevModelsBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	names = {"name", "description", "release_date"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};

	auto result = make_uniq<JevModelsBindData>();
	JevClient client(JevConfig::Resolve(context));
	auto body = client.ListModels();
	auto parsed = JsonDoc::Parse(body, "TypeSafe response");
	auto models = yyjson_obj_get(parsed.Root(), "models");
	if (!yyjson_is_arr(models)) {
		throw IOException("Unexpected response shape from GET /v1/models; expected { models: [...] }");
	}
	size_t idx, max;
	yyjson_val *model;
	yyjson_arr_foreach(models, idx, max, model) {
		vector<Value> row;
		for (auto &field : names) {
			auto val = yyjson_obj_get(model, field.c_str());
			row.push_back(yyjson_is_str(val) ? Value(string(yyjson_get_str(val), yyjson_get_len(val)))
			                                 : Value(LogicalType::VARCHAR));
		}
		result->rows.push_back(std::move(row));
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> JevModelsInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<JevModelsState>();
}

static void JevModelsFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<JevModelsBindData>();
	auto &state = data.global_state->Cast<JevModelsState>();
	idx_t count = 0;
	while (state.offset < bind.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = bind.rows[state.offset++];
		for (idx_t col = 0; col < row.size(); col++) {
			output.SetValue(col, count, row[col]);
		}
		count++;
	}
	output.SetCardinality(count);
}

void JevFunctions::RegisterTableFunctions(ExtensionLoader &loader) {
	TableFunction models("jev_models", {}, JevModelsFunction, JevModelsBind, JevModelsInit);
	loader.RegisterFunction(models);
}

} // namespace duckdb
