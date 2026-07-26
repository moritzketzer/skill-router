// test_library.cpp - unit tests for skill_library.hpp (no framework; asserts).
#include "skill_library.hpp"
#include "mcp_server.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <filesystem>

using namespace skilllib;
namespace fs = std::filesystem;

static int g_pass = 0;
#define TEST(name) void name(); struct name##_reg { name##_reg() { tests().push_back({#name, name}); } } name##_inst; void name()
static std::vector<std::pair<const char*, void (*)()>>& tests() {
  static std::vector<std::pair<const char*, void (*)()>> t;
  return t;
}

static std::string write_skill(const std::string& dir, const std::string& name,
                               const std::string& desc, const std::string& body = "body text") {
  std::string path = dir + "/" + name + "_SKILL.md";
  std::ofstream f(path);
  f << "---\nname: " << name << "\ndescription: \"" << desc << "\"\n---\n\n# " << name << "\n\n" << body << "\n";
  return path;
}

static fs::path test_root() {
  static fs::path root = [] {
    fs::path p = fs::temp_directory_path() / "skillrouter_tests";
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
  }();
  return root;
}

static std::string test_dir(const std::string& name) {
  fs::path p = test_root() / name;
  std::error_code ec;
  fs::create_directories(p, ec);
  return p.string();
}

static std::string path_join(const std::string& dir, const std::string& file) {
  return (fs::path(dir) / file).string();
}

TEST(test_frontmatter_quoted_string) {
  Frontmatter fm = parse_frontmatter("---\nname: foo\ndescription: \"Use when doing foo things\"\n---\nbody");
  assert(fm.valid);
  assert(fm.name == "foo");
  assert(fm.description == "Use when doing foo things");
}

TEST(test_frontmatter_folded_block_scalar) {
  Frontmatter fm = parse_frontmatter(
      "---\nname: bar\ndescription: >\n  Use when doing bar\n  things across lines\n---\nbody");
  assert(fm.valid);
  assert(fm.description == "Use when doing bar things across lines");
}

TEST(test_frontmatter_missing_fields_invalid) {
  assert(!parse_frontmatter("---\nname: onlyname\n---\nbody").valid);
  assert(!parse_frontmatter("no frontmatter here at all").valid);
}

TEST(test_xml_tag_detector_catches_placeholder_syntax) {
  assert(looks_like_xml_tag("Call search with \"<query>\" to get results"));
  assert(looks_like_xml_tag("fetch <skill_id> to load it"));
  assert(looks_like_xml_tag("</closing_tag> style too"));
}

TEST(test_xml_tag_detector_does_not_flag_comparisons_or_prose) {
  // Must not false-positive on ordinary text containing '<'/'>' as
  // comparison operators or unrelated punctuation - only genuinely
  // tag-shaped substrings should trip this.
  assert(!looks_like_xml_tag("use when score < 3 or count > 10"));
  assert(!looks_like_xml_tag("a < b and c > d in the formula"));
  assert(!looks_like_xml_tag("plain description with no angle brackets at all"));
  assert(!looks_like_xml_tag("weird but harmless: 3 < 4"));
}

TEST(test_register_rejects_description_with_xml_tag_shaped_text) {
  auto dir = test_dir("skl_test15");
  auto path = path_join(dir, "bad2_SKILL.md");
  std::ofstream f(path);
  f << "---\nname: bad2\ndescription: \"Call search with <query> to get results\"\n---\nbody\n";
  f.close();
  SkillLibrary lib(":memory:");
  auto r = lib.register_skill(path);
  assert(!r.ok);
  assert(r.error.find("XML/HTML-tag-shaped") != std::string::npos);
}

TEST(test_register_creates_indexed_row) {
  auto dir = test_dir("skl_test1");
  auto path = write_skill(dir, "alpha", "Use when doing alpha things with keywords foo bar");
  SkillLibrary lib(":memory:");
  auto r = lib.register_skill(path);
  assert(r.ok && r.created && r.skill_id == "alpha");
  SkillRow row;
  assert(lib.get_row("alpha", row));
  assert(row.state == State::Indexed);
  assert(row.search_count == 0 && row.fetch_count == 0);
}

TEST(test_register_invalid_frontmatter_reports_error) {
  auto dir = test_dir("skl_test2");
  auto path = path_join(dir, "bad_SKILL.md");
  std::ofstream f(path);
  f << "not a valid skill file\n";
  f.close();
  SkillLibrary lib(":memory:");
  auto r = lib.register_skill(path);
  assert(!r.ok && !r.error.empty());
}

TEST(test_reregister_unchanged_is_noop) {
  auto dir = test_dir("skl_test3");
  auto path = write_skill(dir, "gamma", "gamma description");
  SkillLibrary lib(":memory:");
  auto r1 = lib.register_skill(path);
  auto r2 = lib.register_skill(path);
  assert(r1.created && !r2.created && !r2.updated);  // hash unchanged -> no-op
}

TEST(test_drift_detection_and_reindex_transition) {
  auto dir = test_dir("skl_test4");
  auto path = write_skill(dir, "delta", "delta v1 description");
  SkillLibrary lib(":memory:");
  lib.register_skill(path);
  SkillRow row; lib.get_row("delta", row);
  assert(row.state == State::Indexed);

  // mutate the file on disk without going through register_skill
  { std::ofstream f(path); f << "---\nname: delta\ndescription: \"delta v2 description\"\n---\nchanged\n"; }
  bool went_stale = lib.mark_stale_if_drifted("delta");
  assert(went_stale);
  lib.get_row("delta", row);
  assert(row.state == State::Stale);

  auto r = lib.register_skill(path);   // re-index resolves drift
  assert(r.ok && r.updated);
  lib.get_row("delta", row);
  assert(row.state == State::Indexed);
  assert(row.description == "delta v2 description");
}

TEST(test_search_ranks_by_keyword_and_never_touches_body) {
  auto dir = test_dir("skl_test5");
  auto p1 = write_skill(dir, "pptx", "Use when creating slide decks and presentations",
                        "THIS BODY MUST NEVER APPEAR IN SEARCH RESULTS " + std::string(500, 'x'));
  auto p2 = write_skill(dir, "docx", "Use when creating Word documents");
  SkillLibrary lib(":memory:");
  lib.register_skill(p1);
  lib.register_skill(p2);
  auto hits = lib.search("make a slide presentation");
  assert(!hits.empty());
  assert(hits[0].skill_id == "pptx");
  for (auto& h : hits) assert(h.description.find("MUST NEVER APPEAR") == std::string::npos);
}

TEST(test_search_logs_suggested_and_bumps_search_count) {
  auto dir = test_dir("skl_test6");
  auto p = write_skill(dir, "eps", "Use when handling epsilon tasks");
  SkillLibrary lib(":memory:");
  lib.register_skill(p);
  lib.search("epsilon tasks");
  lib.search("epsilon tasks");
  SkillRow row; lib.get_row("eps", row);
  assert(row.search_count == 2);
  assert(lib.telemetry().total_searches == 2);
}

TEST(test_fetch_returns_body_bumps_fetch_count_and_activates) {
  auto dir = test_dir("skl_test7");
  auto p = write_skill(dir, "zeta", "Use for zeta processing", "UNIQUE_ZETA_BODY_MARKER");
  SkillLibrary lib(":memory:");
  lib.register_skill(p);
  SkillRow before; lib.get_row("zeta", before);
  assert(before.state == State::Indexed);

  std::string body = lib.fetch_body("zeta", "zeta processing");
  assert(body.find("UNIQUE_ZETA_BODY_MARKER") != std::string::npos);

  SkillRow after; lib.get_row("zeta", after);
  assert(after.state == State::Active);
  assert(after.fetch_count == 1);
  assert(lib.telemetry().total_fetches == 1);
}

TEST(test_fetch_unknown_skill_throws) {
  SkillLibrary lib(":memory:");
  bool threw = false;
  try { lib.fetch_body("does_not_exist"); } catch (const DbError&) { threw = true; }
  assert(threw);
}

TEST(test_deprecated_skills_rank_lower_but_still_fetchable) {
  auto dir = test_dir("skl_test8");
  auto p1 = write_skill(dir, "old_tool", "Use for widget processing tasks");
  auto p2 = write_skill(dir, "new_tool", "Use for widget processing tasks too");
  SkillLibrary lib(":memory:");
  lib.register_skill(p1);
  lib.register_skill(p2);
  lib.set_state("old_tool", State::Deprecated);
  auto hits = lib.search("widget processing");
  assert(hits[0].skill_id == "new_tool");
  std::string body = lib.fetch_body("old_tool");
  assert(!body.empty());
}

TEST(test_archived_excluded_from_search_by_default) {
  auto dir = test_dir("skl_test9");
  auto p = write_skill(dir, "retired", "Use for retired legacy tasks");
  SkillLibrary lib(":memory:");
  lib.register_skill(p);
  lib.set_state("retired", State::Archived);
  auto hits = lib.search("retired legacy tasks");
  assert(hits.empty());
  auto hits2 = lib.search("retired legacy tasks", 8, true);
  assert(!hits2.empty());
}

TEST(test_conversion_boost_reorders_over_time) {
  auto dir = test_dir("skl_test10");
  auto p1 = write_skill(dir, "tied_a", "Use for shared keyword matching task");
  auto p2 = write_skill(dir, "tied_b", "Use for shared keyword matching task");
  SkillLibrary lib(":memory:");
  lib.register_skill(p1);
  lib.register_skill(p2);
  auto tie = lib.search("shared keyword matching");
  assert(tie.size() == 2 && tie[0].score == tie[1].score);

  for (int i = 0; i < 5; ++i) { lib.search("shared keyword matching"); lib.fetch_body("tied_b"); }
  auto hits = lib.search("shared keyword matching");
  assert(hits[0].skill_id == "tied_b");
}

TEST(test_graveyard_candidates_identifies_low_conversion_high_search) {
  // NOTE: vocabularies are deliberately disjoint. An earlier version shared
  // "matching task" between both fixtures, which caused the second skill to
  // legitimately (if weakly) match the first query too -- correct engine
  // behavior, but it broke this test's isolation assumption. Root-caused via
  // debug_graveyard.cpp before touching anything; the engine was right,
  // the fixture was wrong.
  auto dir = test_dir("skl_test11");
  auto p1 = write_skill(dir, "popular_useless", "Use for common overused generic broad operation");
  auto p2 = write_skill(dir, "rarely_searched", "Use for unique zzqq quantum flux processing");
  SkillLibrary lib(":memory:");
  lib.register_skill(p1);
  lib.register_skill(p2);
  for (int i = 0; i < 10; ++i) lib.search("common generic broad operation");
  lib.search("zzqq quantum flux");
  auto candidates = lib.graveyard_candidates(5);
  assert(candidates.size() == 1);
  assert(candidates[0].skill_id == "popular_useless");
}

TEST(test_list_all_and_state_counts) {
  auto dir = test_dir("skl_test12");
  auto p1 = write_skill(dir, "s1", "desc one");
  auto p2 = write_skill(dir, "s2", "desc two");
  SkillLibrary lib(":memory:");
  lib.register_skill(p1);
  lib.register_skill(p2);
  lib.fetch_body("s1");
  auto all = lib.list_all();
  assert(all.size() == 2);
  auto counts = lib.state_counts();
  assert(counts["ACTIVE"] == 1 && counts["INDEXED"] == 1);
}

TEST(test_persistence_across_reopen) {
  auto db_path = (test_root() / "skl_persist.db").string();
  std::remove(db_path.c_str());
  std::remove((db_path + "-wal").c_str());
  std::remove((db_path + "-shm").c_str());
  auto dir = test_dir("skl_test13");
  auto p = write_skill(dir, "persistent", "Use for persistence testing tasks");
  {
    SkillLibrary lib(db_path);
    lib.register_skill(p);
    lib.search("persistence testing");
  }
  SkillLibrary lib2(db_path);
  SkillRow row;
  assert(lib2.get_row("persistent", row));
  assert(row.search_count == 1);
}

TEST(test_oversized_query_rejected) {
  SkillLibrary lib(":memory:");
  std::string huge(kMaxQueryBytes + 1, 'x');
  bool threw = false;
  try { lib.search(huge); } catch (const DbError&) { threw = true; }
  assert(threw);
}

TEST(test_log_used_appears_in_search_log) {
  auto dir = test_dir("skl_test14");
  auto p = write_skill(dir, "eta", "Use for eta processing tasks");
  SkillLibrary lib(":memory:");
  lib.register_skill(p);
  lib.search("eta processing");
  lib.fetch_body("eta");
  lib.log_used("eta", "eta processing");
  assert(lib.count_events("eta", "USED") == 1);
  assert(lib.count_events("eta", "FETCHED") == 1);
  assert(lib.count_events("eta", "SUGGESTED") == 1);
}

TEST(test_recent_events_returns_chronological_stream) {
  auto dir = test_dir("skl_test_events");
  auto p = write_skill(dir, "evt", "Use for event stream testing tasks");
  SkillLibrary lib(":memory:");
  lib.register_skill(p);
  assert(lib.total_events() == 0);
  lib.search("event stream testing");   // logs 1 SUGGESTED
  lib.fetch_body("evt");                // logs 1 FETCHED
  assert(lib.total_events() == 2);
  auto ev = lib.recent_events(10);
  assert(ev.size() == 2);
  assert(ev[0].event == "SUGGESTED" && ev[0].skill_id == "evt");  // oldest first
  assert(ev[1].event == "FETCHED");
  assert(ev[0].query == "event stream testing");
}

TEST(test_recent_events_honors_limit) {
  auto dir = test_dir("skl_test_events2");
  auto p = write_skill(dir, "evt2", "Use for limit testing tasks");
  SkillLibrary lib(":memory:");
  lib.register_skill(p);
  for (int i = 0; i < 5; ++i) lib.search("limit testing");
  assert(lib.total_events() == 5);
  assert(lib.recent_events(2).size() == 2);  // capped to the last 2
}

// --------------------------------------------------------------------------
// FTS5 stemmed full-text + fuzzy search + hybrid ranking (skill_library.hpp).
// --------------------------------------------------------------------------

TEST(test_fts5_is_enabled_in_this_build) {
  // This build compiles SQLite with -DSQLITE_ENABLE_FTS5, so the FTS index
  // must come up. (If it didn't, the engine still works via exact + fuzzy -
  // but then the stemmed-match test below would be the canary.)
  SkillLibrary lib(":memory:");
  assert(lib.fts_enabled());
}

TEST(test_fts_stemmed_match_finds_morphological_variants) {
  auto dir = test_dir("fts_stem");
  auto p = write_skill(dir, "ml_stem", "Use for training neural networks and tuning optimizers");
  SkillLibrary lib(":memory:");
  lib.register_skill(p);
  // Singular/stemmed query: "train"/"optimizer" are NOT exact tokens of the
  // description ("training"/"optimizers"), so exact mode finds nothing...
  assert(lib.search("train optimizer", 8, false, SearchMode::Exact).empty());
  // ...but the Porter stemmer in FTS5 matches them under hybrid (and fts) mode.
  auto hybrid = lib.search("train optimizer", 8, false, SearchMode::Hybrid);
  assert(!hybrid.empty() && hybrid[0].skill_id == "ml_stem");
  assert(!lib.search("train optimizer", 8, false, SearchMode::Fts).empty());
}

TEST(test_fuzzy_match_tolerates_typos) {
  auto dir = test_dir("fuzzy_typo");
  auto p = write_skill(dir, "kube_skill", "Use for kubernetes orchestration");
  SkillLibrary lib(":memory:");
  lib.register_skill(p);
  // "kubernets" is a one-edit typo of "kubernetes" and matches nothing exactly
  // or via stemming, so only fuzzy recovers it.
  assert(lib.search("kubernets", 8, false, SearchMode::Exact).empty());
  auto fuzzy = lib.search("kubernets", 8, false, SearchMode::Fuzzy);
  assert(!fuzzy.empty() && fuzzy[0].skill_id == "kube_skill");
  assert(!lib.search("kubernets", 8, false, SearchMode::Hybrid).empty());
}

TEST(test_exact_mode_still_dominates_hybrid_ranking) {
  // A skill with an exact keyword hit must outrank one that only matches via
  // stemming/fuzzy - exact is the spine of the ranking.
  auto dir = test_dir("hybrid_rank");
  auto p1 = write_skill(dir, "exact_hit", "Use for kubernetes deployment");
  auto p2 = write_skill(dir, "stem_only", "Use for deploying kubernetes clusters");
  SkillLibrary lib(":memory:");
  lib.register_skill(p1);
  lib.register_skill(p2);
  // "deployment" is exact in exact_hit; "deploying" only stems to match in stem_only.
  auto hits = lib.search("kubernetes deployment", 8, false, SearchMode::Hybrid);
  assert(hits.size() == 2);
  assert(hits[0].skill_id == "exact_hit");   // exact keeps the top spot
}

TEST(test_fts_index_resyncs_on_reindex) {
  auto dir = test_dir("fts_resync");
  auto path = write_skill(dir, "drifter", "Use for alpha widget processing");
  SkillLibrary lib(":memory:");
  lib.register_skill(path);
  assert(!lib.search("alpha", 8, false, SearchMode::Fts).empty());
  // Rewrite the description on disk and re-index; the FTS sync trigger must
  // drop the old term and index the new one.
  { std::ofstream f(path); f << "---\nname: drifter\ndescription: \"Use for beta gadget assembly\"\n---\nx\n"; }
  auto r = lib.register_skill(path);
  assert(r.updated);
  assert(lib.search("alpha", 8, false, SearchMode::Fts).empty());        // old term gone
  assert(!lib.search("beta", 8, false, SearchMode::Fts).empty());        // new term present
}

// --------------------------------------------------------------------------
// MCP interface tests (mcp_server.hpp): the JSON parser and the pure
// request->response dispatcher. No sockets or pipes - handle_request is pure,
// so the whole protocol surface is exercised in-process.
// --------------------------------------------------------------------------

TEST(test_mcp_json_parses_nested_objects_and_arrays) {
  auto v = mcp::json_parse(R"({"a":1,"b":[true,false,null,"x"],"c":{"d":"e"}})");
  assert(v.is_obj());
  assert(v.find("a") && v.find("a")->is_num() && v.find("a")->num == 1);
  const mcp::JsonValue* b = v.find("b");
  assert(b && b->type == mcp::JsonValue::Type::Arr && b->arr.size() == 4);
  assert(b->arr[0].is_bool() && b->arr[0].b == true);
  assert(b->arr[2].is_null());
  assert(b->arr[3].as_str() == "x");
  assert(v.find("c") && v.find("c")->find("d")->as_str() == "e");
}

TEST(test_mcp_json_decodes_escapes_and_unicode) {
  auto v = mcp::json_parse(R"({"s":"line1\nline2\t\"q\" é 😀"})");
  std::string s = v.find("s")->str;
  assert(s.find("line1\nline2") != std::string::npos);
  assert(s.find('"') != std::string::npos);
  assert(s.find("\xc3\xa9") != std::string::npos);        // U+00E9 e-acute (2-byte UTF-8)
  assert(s.find("\xf0\x9f\x98\x80") != std::string::npos);  // U+1F600 grinning face (surrogate pair)
}

TEST(test_mcp_json_rejects_malformed_input) {
  bool threw = false;
  try { mcp::json_parse("{not json"); } catch (const std::exception&) { threw = true; }
  assert(threw);
}

TEST(test_mcp_serialize_id_number_string_null) {
  assert(mcp::serialize_id(mcp::json_parse("7")) == "7");
  assert(mcp::serialize_id(mcp::json_parse("\"abc\"")) == "\"abc\"");
  assert(mcp::serialize_id(mcp::json_parse("null")) == "null");
}

TEST(test_mcp_initialize_advertises_capabilities) {
  SkillLibrary lib(":memory:");
  auto resp = mcp::handle_request(
      lib, R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})");
  assert(resp.has_value());
  assert(resp->find("\"protocolVersion\":\"2024-11-05\"") != std::string::npos);
  assert(resp->find("\"serverInfo\"") != std::string::npos);
  assert(resp->find("skillrouter") != std::string::npos);
  assert(resp->find("\"tools\"") != std::string::npos);
  assert(resp->find("\"resources\"") != std::string::npos);
}

TEST(test_mcp_notification_gets_no_reply) {
  SkillLibrary lib(":memory:");
  auto resp = mcp::handle_request(lib, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  assert(!resp.has_value());  // notifications (no id) must never be answered
}

TEST(test_mcp_tools_list_includes_search_and_fetch) {
  SkillLibrary lib(":memory:");
  auto resp = mcp::handle_request(lib, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
  assert(resp.has_value());
  assert(resp->find("\"skill_search\"") != std::string::npos);
  assert(resp->find("\"skill_fetch\"") != std::string::npos);
  assert(resp->find("\"inputSchema\"") != std::string::npos);
}

TEST(test_mcp_tools_call_search_returns_hits_and_logs_telemetry) {
  auto dir = test_dir("mcp_test1");
  auto p = write_skill(dir, "mcp_widget", "Use for widget processing and gadget assembly tasks");
  SkillLibrary lib(":memory:");
  lib.register_skill(p);
  auto resp = mcp::handle_request(
      lib, R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"skill_search","arguments":{"query":"widget processing"}}})");
  assert(resp.has_value());
  assert(resp->find("mcp_widget") != std::string::npos);
  assert(resp->find("\"content\"") != std::string::npos);
  // telemetry accrues through MCP exactly as through the CLI
  SkillRow row; lib.get_row("mcp_widget", row);
  assert(row.search_count == 1);
}

TEST(test_mcp_tools_call_fetch_returns_body_and_activates) {
  auto dir = test_dir("mcp_test2");
  auto p = write_skill(dir, "mcp_zeta", "Use for zeta things", "UNIQUE_MCP_BODY_MARKER");
  SkillLibrary lib(":memory:");
  lib.register_skill(p);
  auto resp = mcp::handle_request(
      lib, R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"skill_fetch","arguments":{"skill_id":"mcp_zeta"}}})");
  assert(resp.has_value());
  assert(resp->find("UNIQUE_MCP_BODY_MARKER") != std::string::npos);
  SkillRow row; lib.get_row("mcp_zeta", row);
  assert(row.state == State::Active && row.fetch_count == 1);
}

TEST(test_mcp_tools_call_fetch_unknown_is_tool_error_not_protocol_error) {
  SkillLibrary lib(":memory:");
  auto resp = mcp::handle_request(
      lib, R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"skill_fetch","arguments":{"skill_id":"nope"}}})");
  assert(resp.has_value());
  assert(resp->find("\"isError\":true") != std::string::npos);
  assert(resp->find("\"error\"") == std::string::npos);  // NOT a JSON-RPC protocol error
}

TEST(test_mcp_tools_call_missing_required_arg_is_tool_error) {
  SkillLibrary lib(":memory:");
  auto resp = mcp::handle_request(
      lib, R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"skill_search","arguments":{}}})");
  assert(resp.has_value());
  assert(resp->find("\"isError\":true") != std::string::npos);
}

TEST(test_mcp_resources_list_serves_full_library) {
  auto dir = test_dir("mcp_test3");
  auto p1 = write_skill(dir, "mcp_res_a", "first resource description");
  auto p2 = write_skill(dir, "mcp_res_b", "second resource description");
  SkillLibrary lib(":memory:");
  lib.register_skill(p1);
  lib.register_skill(p2);
  auto resp = mcp::handle_request(lib, R"({"jsonrpc":"2.0","id":7,"method":"resources/list"})");
  assert(resp.has_value());
  assert(resp->find("skill://mcp_res_a") != std::string::npos);
  assert(resp->find("skill://mcp_res_b") != std::string::npos);
  assert(resp->find("\"mimeType\":\"text/markdown\"") != std::string::npos);
}

TEST(test_mcp_resources_read_returns_body_for_valid_uri) {
  auto dir = test_dir("mcp_test4");
  auto p = write_skill(dir, "mcp_res_body", "resource body desc", "READABLE_RESOURCE_BODY");
  SkillLibrary lib(":memory:");
  lib.register_skill(p);
  auto resp = mcp::handle_request(
      lib, R"({"jsonrpc":"2.0","id":8,"method":"resources/read","params":{"uri":"skill://mcp_res_body"}})");
  assert(resp.has_value());
  assert(resp->find("READABLE_RESOURCE_BODY") != std::string::npos);
  assert(resp->find("\"contents\"") != std::string::npos);
}

TEST(test_mcp_resources_read_unknown_is_protocol_error) {
  SkillLibrary lib(":memory:");
  auto resp = mcp::handle_request(
      lib, R"({"jsonrpc":"2.0","id":9,"method":"resources/read","params":{"uri":"skill://ghost"}})");
  assert(resp.has_value());
  assert(resp->find("\"code\":-32002") != std::string::npos);
}

TEST(test_mcp_resources_read_bad_scheme_is_invalid_params) {
  SkillLibrary lib(":memory:");
  auto resp = mcp::handle_request(
      lib, R"({"jsonrpc":"2.0","id":10,"method":"resources/read","params":{"uri":"http://evil"}})");
  assert(resp.has_value());
  assert(resp->find("\"code\":-32602") != std::string::npos);
}

TEST(test_mcp_unknown_method_is_method_not_found) {
  SkillLibrary lib(":memory:");
  auto resp = mcp::handle_request(lib, R"({"jsonrpc":"2.0","id":11,"method":"no/such/method"})");
  assert(resp.has_value());
  assert(resp->find("\"code\":-32601") != std::string::npos);
}

TEST(test_mcp_malformed_json_is_parse_error_with_null_id) {
  SkillLibrary lib(":memory:");
  auto resp = mcp::handle_request(lib, "this is not json");
  assert(resp.has_value());
  assert(resp->find("\"code\":-32700") != std::string::npos);
  assert(resp->find("\"id\":null") != std::string::npos);
}

TEST(test_mcp_string_id_is_echoed_verbatim) {
  SkillLibrary lib(":memory:");
  auto resp = mcp::handle_request(lib, R"({"jsonrpc":"2.0","id":"abc-123","method":"ping"})");
  assert(resp.has_value());
  assert(resp->find("\"id\":\"abc-123\"") != std::string::npos);
  assert(resp->find("\"result\":{}") != std::string::npos);
}

int main() {
  for (auto& [name, fn] : tests()) {
    fn();
    std::printf("PASS  %s\n", name);
    ++g_pass;
  }
  std::printf("\n%d/%zu passed\n", g_pass, tests().size());
  return g_pass == static_cast<int>(tests().size()) ? 0 : 1;
}
