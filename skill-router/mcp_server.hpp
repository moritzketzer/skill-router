// mcp_server.hpp - Model Context Protocol (MCP) interface for skillrouter.
//
// This is the piece that lets ANY MCP-capable model host (Claude Desktop,
// Claude Code, Cursor, or any custom agent loop that speaks MCP) use the
// full registered skill library without ever pasting every skill's
// name+description into a context window. The router's existing invariant is
// preserved verbatim across the protocol boundary:
//
//   * the library index (skill_id + description, index-only, ~constant size)
//     is exposed both as MCP *tools* (skill_search / skill_fetch / ...) and
//     as MCP *resources* (one skill:// resource per registered skill);
//   * a skill's full body is loaded in exactly ONE place - skill_fetch /
//     resources/read - which maps onto SkillLibrary::fetch_body(), the same
//     single body-loading operation the CLI and HTTP API already funnel
//     through. Telemetry (SUGGESTED / FETCHED) accrues through MCP calls just
//     as it does through the CLI.
//
// Transport is deliberately stdio + newline-delimited JSON-RPC 2.0 (the MCP
// stdio transport): one JSON message per line, requests on stdin, responses
// on stdout, logs on stderr. That is the lowest-common-denominator MCP
// transport every client supports and needs zero network setup, matching
// this project's "drops onto a machine with zero dependencies" posture. The
// dispatch logic here is transport-agnostic and pure (a request string in, a
// response string out) so it is unit-testable without any sockets or pipes;
// main.cpp wraps it in the trivial stdio read/write loop.
//
// Dependencies: skill_library.hpp (which is C++20 stdlib + vendored SQLite)
// and nothing else. The JSON parser below is a small, self-contained
// recursive-descent reader - the project already hand-rolls JSON *output*
// (json_escape); MCP additionally needs JSON *input*, and pulling in a
// third-party JSON library would violate the stdlib-first, single-dependency
// doctrine the rest of the codebase holds to.
#pragma once

#include "skill_library.hpp"

#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace skilllib {
namespace mcp {

// The MCP protocol revision this server implements. 2024-11-05 is the stable,
// universally-supported revision; if a client requests a different one we
// still answer with ours (clients negotiate down gracefully).
inline constexpr const char* kProtocolVersion = "2024-11-05";
inline constexpr const char* kServerName = "skillrouter";

// ---------------------------------------------------------------------------
// Minimal JSON parser (values in; json_escape from skill_library.hpp is used
// for strings on the way out). Supports the full JSON grammar we can receive
// from an MCP client: objects, arrays, strings (with \uXXXX + surrogate
// pairs), numbers, true/false/null. Throws std::runtime_error on malformed
// input, which the dispatcher turns into a JSON-RPC parse error.
// ---------------------------------------------------------------------------

struct JsonValue {
  enum class Type { Null, Bool, Num, Str, Arr, Obj };
  Type type = Type::Null;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<JsonValue> arr;
  std::map<std::string, JsonValue> obj;

  bool is_null() const { return type == Type::Null; }
  bool is_obj() const { return type == Type::Obj; }
  bool is_str() const { return type == Type::Str; }
  bool is_num() const { return type == Type::Num; }
  bool is_bool() const { return type == Type::Bool; }

  // Look up a key in an object; returns nullptr if absent or not an object.
  const JsonValue* find(const std::string& key) const {
    if (type != Type::Obj) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
  }
  std::string as_str(const std::string& def = "") const { return type == Type::Str ? str : def; }
  double as_num(double def = 0.0) const { return type == Type::Num ? num : def; }
  bool as_bool(bool def = false) const { return type == Type::Bool ? b : def; }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& s) : s_(s) {}

  JsonValue parse() {
    skip_ws();
    JsonValue v = parse_value();
    skip_ws();
    if (i_ != s_.size()) throw std::runtime_error("trailing characters after JSON value");
    return v;
  }

 private:
  const std::string& s_;
  size_t i_ = 0;

  [[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string("JSON parse error: ") + what); }
  void skip_ws() {
    while (i_ < s_.size()) {
      char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
      else break;
    }
  }
  char peek() { if (i_ >= s_.size()) fail("unexpected end of input"); return s_[i_]; }

  JsonValue parse_value() {
    skip_ws();
    char c = peek();
    switch (c) {
      case '{': return parse_object();
      case '[': return parse_array();
      case '"': { JsonValue v; v.type = JsonValue::Type::Str; v.str = parse_string(); return v; }
      case 't': case 'f': return parse_bool();
      case 'n': return parse_null();
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        fail("unexpected character");
    }
  }

  JsonValue parse_object() {
    JsonValue v; v.type = JsonValue::Type::Obj;
    ++i_;  // consume '{'
    skip_ws();
    if (i_ < s_.size() && s_[i_] == '}') { ++i_; return v; }
    while (true) {
      skip_ws();
      if (peek() != '"') fail("expected string key in object");
      std::string key = parse_string();
      skip_ws();
      if (peek() != ':') fail("expected ':' after object key");
      ++i_;
      v.obj[key] = parse_value();
      skip_ws();
      char c = peek();
      if (c == ',') { ++i_; continue; }
      if (c == '}') { ++i_; break; }
      fail("expected ',' or '}' in object");
    }
    return v;
  }

  JsonValue parse_array() {
    JsonValue v; v.type = JsonValue::Type::Arr;
    ++i_;  // consume '['
    skip_ws();
    if (i_ < s_.size() && s_[i_] == ']') { ++i_; return v; }
    while (true) {
      v.arr.push_back(parse_value());
      skip_ws();
      char c = peek();
      if (c == ',') { ++i_; continue; }
      if (c == ']') { ++i_; break; }
      fail("expected ',' or ']' in array");
    }
    return v;
  }

  static void append_utf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
      out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  unsigned int parse_hex4() {
    if (i_ + 4 > s_.size()) fail("truncated \\u escape");
    unsigned int cp = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s_[i_++];
      cp <<= 4;
      if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
      else fail("invalid hex digit in \\u escape");
    }
    return cp;
  }

  std::string parse_string() {
    ++i_;  // consume opening quote
    std::string out;
    while (true) {
      if (i_ >= s_.size()) fail("unterminated string");
      char c = s_[i_++];
      if (c == '"') break;
      if (c == '\\') {
        if (i_ >= s_.size()) fail("unterminated escape");
        char e = s_[i_++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            unsigned int cp = parse_hex4();
            if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate
              if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                i_ += 2;
                unsigned int lo = parse_hex4();
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else { append_utf8(out, cp); cp = lo; }
              }
            }
            append_utf8(out, cp);
            break;
          }
          default: fail("invalid escape character");
        }
      } else {
        out += c;
      }
    }
    return out;
  }

  JsonValue parse_number() {
    size_t start = i_;
    if (s_[i_] == '-') ++i_;
    while (i_ < s_.size()) {
      char c = s_[i_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') ++i_;
      else break;
    }
    JsonValue v; v.type = JsonValue::Type::Num;
    try { v.num = std::stod(s_.substr(start, i_ - start)); }
    catch (...) { fail("invalid number"); }
    return v;
  }

  JsonValue parse_bool() {
    JsonValue v; v.type = JsonValue::Type::Bool;
    if (s_.compare(i_, 4, "true") == 0) { v.b = true; i_ += 4; }
    else if (s_.compare(i_, 5, "false") == 0) { v.b = false; i_ += 5; }
    else fail("invalid literal");
    return v;
  }

  JsonValue parse_null() {
    if (s_.compare(i_, 4, "null") != 0) fail("invalid literal");
    i_ += 4;
    return JsonValue{};  // Type::Null
  }
};

inline JsonValue json_parse(const std::string& s) { return JsonParser(s).parse(); }

// Serialize a JSON-RPC id back into the response exactly as we must echo it:
// numbers as integers when integral, strings quoted+escaped, null otherwise.
inline std::string serialize_id(const JsonValue& id) {
  switch (id.type) {
    case JsonValue::Type::Str:
      return std::string("\"") + json_escape(id.str) + "\"";
    case JsonValue::Type::Num: {
      double d = id.num;
      long long as_int = static_cast<long long>(d);
      if (static_cast<double>(as_int) == d) return std::to_string(as_int);
      std::ostringstream o; o << d; return o.str();
    }
    default:
      return "null";
  }
}

// ---------------------------------------------------------------------------
// JSON-RPC envelope builders.
// ---------------------------------------------------------------------------

inline std::string rpc_result(const std::string& id_json, const std::string& result_json) {
  return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id_json + ",\"result\":" + result_json + "}";
}

inline std::string rpc_error(const std::string& id_json, int code, const std::string& message) {
  std::ostringstream o;
  o << "{\"jsonrpc\":\"2.0\",\"id\":" << id_json << ",\"error\":{\"code\":" << code
    << ",\"message\":\"" << json_escape(message) << "\"}}";
  return o.str();
}

// A tools/call result: MCP wraps tool output in a content array. `is_error`
// distinguishes a tool-level failure (e.g. unknown skill_id) - which by MCP
// convention is reported here, not as a JSON-RPC protocol error - from success.
inline std::string tool_text_result(const std::string& text, bool is_error = false) {
  std::ostringstream o;
  o << "{\"content\":[{\"type\":\"text\",\"text\":\"" << json_escape(text) << "\"}]";
  if (is_error) o << ",\"isError\":true";
  o << "}";
  return o.str();
}

// The static tool catalogue advertised by tools/list. These four cover the
// router's full surface: search the index, fetch a body, read telemetry, and
// see the graveyard report - the same operations the CLI and HTTP API expose.
inline std::string tools_list_json() {
  return R"json({"tools":[)json"
      R"json({"name":"skill_search","title":"Search the skill library",)json"
      R"json("description":"Search the registered skill library by a loose natural-language query and return the top-ranked skills as [{skill_id, description, score, state}]. Hybrid ranking by default: exact token match, FTS5 stemmed full-text (so train~training, optimizer~optimizers, and prefixes match), and fuzzy edit-distance (typo tolerance). Returns only skill_id + description (never a skill body), so its cost is independent of library size. Call skill_fetch with a returned skill_id to load that skill's full instructions.",)json"
      R"json("inputSchema":{"type":"object","properties":{)json"
      R"json("query":{"type":"string","description":"Natural-language description of the task you need a skill for."},)json"
      R"json("top":{"type":"integer","description":"Max number of skills to return (default 8).","minimum":1},)json"
      R"json("mode":{"type":"string","enum":["hybrid","exact","fts","fuzzy"],"description":"Matching engine (default hybrid): exact=token match only; fts=FTS5 stemmed full-text; fuzzy=edit-distance typo tolerance; hybrid=all three combined."},)json"
      R"json("include_archived":{"type":"boolean","description":"Include ARCHIVED skills, which are excluded by default (default false)."}},)json"
      R"json("required":["query"]}},)json"
      R"json({"name":"skill_fetch","title":"Fetch a skill's full body",)json"
      R"json("description":"Load the full SKILL.md body for a given skill_id. This is the ONE operation that pulls a complete skill into context - call it only once you have decided to actually use the skill (typically a skill_id returned by skill_search).",)json"
      R"json("inputSchema":{"type":"object","properties":{)json"
      R"json("skill_id":{"type":"string","description":"The exact skill_id to fetch (as returned by skill_search or resources/list)."},)json"
      R"json("context":{"type":"string","description":"Optional query context for telemetry - the task this fetch is in service of."}},)json"
      R"json("required":["skill_id"]}},)json"
      R"json({"name":"skill_stats","title":"Skill library telemetry",)json"
      R"json("description":"Return aggregate telemetry for the library: total searches, total fetches, overall suggestion->fetch conversion, and a count of skills by lifecycle state.",)json"
      R"json("inputSchema":{"type":"object","properties":{}}},)json"
      R"json({"name":"skill_graveyard","title":"Low-value skill report",)json"
      R"json("description":"Return skills that are frequently suggested by search but rarely or never fetched - candidates for review, deprecation, or removal.",)json"
      R"json("inputSchema":{"type":"object","properties":{)json"
      R"json("min_searches":{"type":"integer","description":"Only consider skills searched at least this many times (default 5).","minimum":0}}}})json"
      R"json(]})json";
}

// ---------------------------------------------------------------------------
// Serializers reused by the tool handlers (same shapes the CLI --json / HTTP
// API already emit, so a caller sees identical payloads across every transport).
// ---------------------------------------------------------------------------

inline std::string search_hits_json(const std::vector<SearchHit>& hits) {
  std::ostringstream o; o << "[";
  for (size_t i = 0; i < hits.size(); ++i) {
    o << (i ? "," : "") << "{\"skill_id\":\"" << json_escape(hits[i].skill_id)
      << "\",\"description\":\"" << json_escape(hits[i].description)
      << "\",\"score\":" << hits[i].score << ",\"state\":\"" << to_string(hits[i].state) << "\"}";
  }
  o << "]";
  return o.str();
}

inline std::string stats_json(SkillLibrary& lib) {
  auto t = lib.telemetry();
  auto counts = lib.state_counts();
  std::ostringstream o;
  o << "{\"total_searches\":" << t.total_searches << ",\"total_fetches\":" << t.total_fetches
    << ",\"overall_conversion\":" << t.overall_conversion << ",\"by_state\":{";
  bool first = true;
  for (const auto& [k, v] : counts) { o << (first ? "" : ",") << "\"" << k << "\":" << v; first = false; }
  o << "}}";
  return o.str();
}

inline std::string graveyard_json(SkillLibrary& lib, long long min_searches) {
  auto cands = lib.graveyard_candidates(min_searches);
  std::ostringstream o; o << "[";
  for (size_t i = 0; i < cands.size(); ++i) {
    double conv = cands[i].search_count > 0
        ? static_cast<double>(cands[i].fetch_count) / cands[i].search_count : 0.0;
    o << (i ? "," : "") << "{\"skill_id\":\"" << json_escape(cands[i].skill_id)
      << "\",\"search_count\":" << cands[i].search_count
      << ",\"fetch_count\":" << cands[i].fetch_count << ",\"conversion\":" << conv << "}";
  }
  o << "]";
  return o.str();
}

// resources/list exposes the FULL registered library, one resource per skill,
// carrying only skill_id + description (index-only, never a body) - so listing
// a 10,000-skill library is still cheap and constant-per-skill. Archived
// skills are included here (the list is the full library inventory); search is
// where the archived-by-default filtering lives.
inline std::string resources_list_json(SkillLibrary& lib) {
  auto rows = lib.list_all();
  std::ostringstream o; o << "{\"resources\":[";
  for (size_t i = 0; i < rows.size(); ++i) {
    o << (i ? "," : "")
      << "{\"uri\":\"skill://" << json_escape(rows[i].skill_id) << "\",\"name\":\""
      << json_escape(rows[i].skill_id) << "\",\"description\":\"" << json_escape(rows[i].description)
      << "\",\"mimeType\":\"text/markdown\"}";
  }
  o << "]}";
  return o.str();
}

// ---------------------------------------------------------------------------
// Tool dispatch.
// ---------------------------------------------------------------------------

inline std::string call_tool(SkillLibrary& lib, const std::string& name, const JsonValue& args) {
  try {
    if (name == "skill_search") {
      const JsonValue* q = args.find("query");
      if (!q || !q->is_str() || q->str.empty())
        return tool_text_result("skill_search requires a non-empty string 'query'", true);
      int top = 8;
      if (const JsonValue* t = args.find("top"); t && t->is_num()) {
        int v = static_cast<int>(t->num);
        if (v >= 1) top = v;
      }
      bool include_archived = false;
      if (const JsonValue* ia = args.find("include_archived"); ia && ia->is_bool())
        include_archived = ia->b;
      SearchMode smode = SearchMode::Hybrid;
      if (const JsonValue* md = args.find("mode"); md && md->is_str())
        smode = search_mode_from_string(md->str);
      auto hits = lib.search(q->str, top, include_archived, smode);
      return tool_text_result(search_hits_json(hits));
    }
    if (name == "skill_fetch") {
      const JsonValue* id = args.find("skill_id");
      if (!id || !id->is_str() || id->str.empty())
        return tool_text_result("skill_fetch requires a non-empty string 'skill_id'", true);
      std::string context = args.find("context") ? args.find("context")->as_str() : "";
      try {
        return tool_text_result(lib.fetch_body(id->str, context));
      } catch (const DbError& e) {
        return tool_text_result(std::string("cannot fetch skill: ") + e.what(), true);
      }
    }
    if (name == "skill_stats") {
      return tool_text_result(stats_json(lib));
    }
    if (name == "skill_graveyard") {
      long long min_searches = 5;
      if (const JsonValue* m = args.find("min_searches"); m && m->is_num()) {
        long long v = static_cast<long long>(m->num);
        if (v >= 0) min_searches = v;
      }
      return tool_text_result(graveyard_json(lib, min_searches));
    }
    return tool_text_result(std::string("unknown tool: ") + name, true);
  } catch (const std::exception& e) {
    return tool_text_result(std::string("tool error: ") + e.what(), true);
  }
}

// ---------------------------------------------------------------------------
// Top-level request dispatch. Pure: a single JSON-RPC request string in, the
// JSON-RPC response string out. Returns std::nullopt for a notification (a
// request with no id), to which JSON-RPC forbids a reply. This is the whole
// protocol brain; main.cpp only supplies the stdio read/write loop.
// ---------------------------------------------------------------------------

inline std::optional<std::string> handle_request(SkillLibrary& lib, const std::string& line) {
  JsonValue req;
  try {
    req = json_parse(line);
  } catch (const std::exception& e) {
    // Parse error: no reliable id, so respond with null id per JSON-RPC.
    return rpc_error("null", -32700, std::string("parse error: ") + e.what());
  }

  const JsonValue* id = req.find("id");
  bool is_notification = (id == nullptr || id->is_null());
  std::string id_json = id ? serialize_id(*id) : "null";

  const JsonValue* method_v = req.find("method");
  if (!method_v || !method_v->is_str()) {
    if (is_notification) return std::nullopt;
    return rpc_error(id_json, -32600, "invalid request: missing 'method'");
  }
  const std::string method = method_v->str;
  const JsonValue* params = req.find("params");
  JsonValue empty_obj; empty_obj.type = JsonValue::Type::Obj;
  const JsonValue& p = params ? *params : empty_obj;

  // Notifications: no response is ever sent (initialized, cancelled, etc.).
  if (is_notification) return std::nullopt;

  if (method == "initialize") {
    std::ostringstream o;
    o << "{\"protocolVersion\":\"" << kProtocolVersion
      << "\",\"capabilities\":{\"tools\":{},\"resources\":{}},"
      << "\"serverInfo\":{\"name\":\"" << kServerName << "\",\"version\":\"" << kEngineVersion << "\"}}";
    return rpc_result(id_json, o.str());
  }
  if (method == "ping") {
    return rpc_result(id_json, "{}");
  }
  if (method == "tools/list") {
    return rpc_result(id_json, tools_list_json());
  }
  if (method == "tools/call") {
    const JsonValue* name = p.find("name");
    if (!name || !name->is_str())
      return rpc_error(id_json, -32602, "invalid params: 'name' is required");
    const JsonValue* args = p.find("arguments");
    JsonValue empty_args; empty_args.type = JsonValue::Type::Obj;
    return rpc_result(id_json, call_tool(lib, name->str, args ? *args : empty_args));
  }
  if (method == "resources/list") {
    return rpc_result(id_json, resources_list_json(lib));
  }
  if (method == "resources/templates/list") {
    return rpc_result(id_json, "{\"resourceTemplates\":[]}");
  }
  if (method == "resources/read") {
    const JsonValue* uri_v = p.find("uri");
    if (!uri_v || !uri_v->is_str())
      return rpc_error(id_json, -32602, "invalid params: 'uri' is required");
    const std::string uri = uri_v->str;
    const std::string scheme = "skill://";
    if (uri.compare(0, scheme.size(), scheme) != 0)
      return rpc_error(id_json, -32602, "unsupported resource uri (expected skill://<skill_id>): " + uri);
    std::string skill_id = uri.substr(scheme.size());
    try {
      std::string body = lib.fetch_body(skill_id);
      std::ostringstream o;
      o << "{\"contents\":[{\"uri\":\"" << json_escape(uri)
        << "\",\"mimeType\":\"text/markdown\",\"text\":\"" << json_escape(body) << "\"}]}";
      return rpc_result(id_json, o.str());
    } catch (const DbError& e) {
      // Resource not found is a protocol-level error per the MCP spec.
      return rpc_error(id_json, -32002, std::string("resource not found: ") + e.what());
    }
  }

  return rpc_error(id_json, -32601, std::string("method not found: ") + method);
}

}  // namespace mcp
}  // namespace skilllib
