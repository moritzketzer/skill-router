// skill_library.hpp - the engine behind skillrouter.
//
// Design goal: a 10,000-skill library can live entirely on disk (one
// directory per skill, each with a SKILL.md) with only a lightweight index
// in SQLite (skill_id, description, keywords, path, telemetry). The engine
// NEVER loads a skill's full body except in fetch_body(), which is the one
// operation a caller invokes only once it has decided to actually use that
// skill. search() and stats() never touch skill bodies at all - their cost
// is bounded by the index, not by library size.
//
// State machine per skill (explicit, guarded transitions - no implicit ones):
//   REGISTERED -> INDEXED -> ACTIVE -> STALE -> INDEXED (re-index)
//                                    -> DEPRECATED -> ARCHIVED
// REGISTERED: row exists, frontmatter not yet successfully parsed.
// INDEXED:    frontmatter parsed OK, searchable, never fetched.
// ACTIVE:     has been fetched at least once.
// STALE:      on-disk content_hash no longer matches the indexed hash.
// DEPRECATED: marked by an operator; still fetchable but ranked down.
// ARCHIVED:   excluded from search by default.
//
// Telemetry: every search() call logs one SUGGESTED row per returned
// candidate (query, skill_id, rank, score); every fetch_body() call logs one
// FETCHED row and bumps fetch_count. This is the raw signal a ranking boost
// and a "graveyard candidate" report (searched often, never fetched) are
// built from - deterministic, no ML, matches the stdlib-first doctrine.
//
// Dependencies: SQLite amalgamation only (third_party/sqlite3.{h,c}), C++20
// stdlib. No other third-party code.
#pragma once

#include "third_party/sqlite3.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace skilllib {

inline constexpr const char* kEngineVersion = "1.0";
inline constexpr std::size_t kMaxDescBytes = 4096;
inline constexpr std::size_t kMaxQueryBytes = 8192;

// ---------- small utilities (same proven approach as context_manager.hpp) ---

inline std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline std::string fnv1a64(const std::string& s) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ULL; }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

inline const std::set<std::string>& stopwords() {
  static const std::set<std::string> kStop = {
      "a","an","the","and","or","of","to","in","on","for","with","is","are",
      "be","it","this","that","use","uses","using","when","whenever","also",
      "trigger","triggers","user","users","should","would","can","will",
      "any","all","not","from","into","by","at","as"};
  return kStop;
}

inline std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  std::string cur;
  auto flush = [&] {
    if (cur.size() > 1 && !stopwords().count(cur) && seen.insert(cur).second)
      out.push_back(cur);
    cur.clear();
  };
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '_') cur += static_cast<char>(std::tolower(c));
    else flush();
  }
  flush();
  return out;
}

// Bounded Levenshtein edit distance between two tokens, used for fuzzy
// (typo-tolerant) search. Returns early with maxd+1 the moment the best
// achievable distance provably exceeds maxd, so it stays cheap: the length
// pre-check rejects obvious mismatches in O(1), and the per-row minimum prunes
// the DP as soon as a row can no longer beat the threshold.
inline int bounded_edit_distance(const std::string& a, const std::string& b, int maxd) {
  const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
  if (std::abs(n - m) > maxd) return maxd + 1;
  std::vector<int> prev(m + 1), cur(m + 1);
  for (int j = 0; j <= m; ++j) prev[j] = j;
  for (int i = 1; i <= n; ++i) {
    cur[0] = i;
    int row_min = cur[0];
    for (int j = 1; j <= m; ++j) {
      int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
      row_min = std::min(row_min, cur[j]);
    }
    if (row_min > maxd) return maxd + 1;
    std::swap(prev, cur);
  }
  return prev[m];
}

inline std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, 8, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
  }
  return o;
}

// ---------- minimal frontmatter parser (mirrors adapters/skill_frontmatter_import.py) --
// Handles: flat `key: value` pairs and YAML `>` folded block scalars, which
// is the entirety of what canonical SKILL.md frontmatter actually uses.

struct Frontmatter {
  std::string name;
  std::string description;
  bool valid = false;
};

// Detects XML/HTML-tag-shaped substrings (e.g. "<query>", "<skill_id>",
// "</foo>") in a frontmatter field. Some skill-loading systems parse or
// render frontmatter in a context where such substrings are misread as
// literal markup rather than prose - this was found the hard way (a
// shipped `description` used "<query>"/"<skill_id>" as placeholder prose
// and was rejected downstream). Caught here at registration time so the
// error class doesn't require manual re-discovery.
inline bool looks_like_xml_tag(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    size_t lt = s.find('<', i);
    if (lt == std::string::npos) return false;
    size_t gt = s.find('>', lt);
    if (gt == std::string::npos) return false;
    std::string inner = s.substr(lt + 1, gt - lt - 1);
    if (!inner.empty() && inner[0] == '/') inner = inner.substr(1);
    // A real tag name starts with a letter/underscore and contains only
    // tag-safe characters (letters, digits, underscore, space, slash,
    // hyphen) - this deliberately does NOT flag things like "a < b > c"
    // (comparison operators) or "score < 3", only genuinely tag-shaped text.
    bool tag_shaped = !inner.empty() &&
        (std::isalpha(static_cast<unsigned char>(inner[0])) || inner[0] == '_');
    if (tag_shaped) {
      for (unsigned char c : inner) {
        if (!(std::isalnum(c) || c == '_' || c == ' ' || c == '/' || c == '-')) {
          tag_shaped = false;
          break;
        }
      }
    }
    if (tag_shaped) return true;
    i = gt + 1;
  }
  return false;
}

inline Frontmatter parse_frontmatter(const std::string& text) {
  Frontmatter fm;
  std::string normalized = text;
  normalized.erase(std::remove(normalized.begin(), normalized.end(), '\r'), normalized.end());
  if (normalized.compare(0, 4, "---\n") != 0) return fm;
  auto end = normalized.find("\n---\n", 4);
  if (end == std::string::npos) return fm;
  std::string block = normalized.substr(4, end - 4);
  std::vector<std::string> lines;
  { std::istringstream iss(block); std::string l; while (std::getline(iss, l)) lines.push_back(l); }

  std::map<std::string, std::string> kv;
  size_t i = 0;
  while (i < lines.size()) {
    const std::string& line = lines[i];
    auto colon = line.find(':');
    if (colon == std::string::npos || colon == 0 || std::isspace(static_cast<unsigned char>(line[0]))) {
      ++i; continue;
    }
    std::string key = line.substr(0, colon);
    std::string rest = line.substr(colon + 1);
    // trim leading space
    size_t s = rest.find_first_not_of(' ');
    rest = (s == std::string::npos) ? "" : rest.substr(s);
    if (rest == ">" || rest == "|") {
      std::string block_val;
      ++i;
      while (i < lines.size() && (lines[i].empty() ||
             (lines[i].size() > 0 && std::isspace(static_cast<unsigned char>(lines[i][0]))))) {
        std::string t = lines[i];
        size_t ts = t.find_first_not_of(' ');
        if (ts != std::string::npos) {
          if (!block_val.empty()) block_val += " ";
          block_val += t.substr(ts);
        }
        ++i;
      }
      kv[key] = block_val;
      continue;
    }
    // strip one layer of matching quotes
    if (rest.size() >= 2 && (rest.front() == '"' || rest.front() == '\'') && rest.back() == rest.front())
      rest = rest.substr(1, rest.size() - 2);
    kv[key] = rest;
    ++i;
  }
  fm.name = kv.count("name") ? kv["name"] : "";
  fm.description = kv.count("description") ? kv["description"] : "";
  fm.valid = !fm.name.empty() && !fm.description.empty();
  return fm;
}

// ---------- RAII sqlite (identical pattern to context_manager.hpp, proven) ---

class DbError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Stmt {
 public:
  Stmt(sqlite3* db, const std::string& sql) {
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s_, nullptr) != SQLITE_OK)
      throw DbError(std::string("prepare: ") + sqlite3_errmsg(db) + " in " + sql);
  }
  ~Stmt() { sqlite3_finalize(s_); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  Stmt& bind(int i, const std::string& v) { sqlite3_bind_text(s_, i, v.c_str(), -1, SQLITE_TRANSIENT); return *this; }
  Stmt& bind(int i, double v) { sqlite3_bind_double(s_, i, v); return *this; }
  Stmt& bind(int i, long long v) { sqlite3_bind_int64(s_, i, v); return *this; }
  bool step() {
    int rc = sqlite3_step(s_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw DbError(std::string("step rc=") + std::to_string(rc));
  }
  long long col_i(int i) { return sqlite3_column_int64(s_, i); }
  double col_d(int i) { return sqlite3_column_double(s_, i); }
  std::string col_s(int i) {
    const unsigned char* t = sqlite3_column_text(s_, i);
    return t ? reinterpret_cast<const char*>(t) : "";
  }
 private:
  sqlite3_stmt* s_ = nullptr;
};

class Db {
 public:
  explicit Db(const std::string& path) {
    if (sqlite3_open(path.c_str(), &h_) != SQLITE_OK) throw DbError("open failed: " + path);
    sqlite3_busy_timeout(h_, 5000);
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
  }
  ~Db() { sqlite3_close(h_); }
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;
  void exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(h_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
      std::string m = err ? err : "exec failed"; sqlite3_free(err);
      throw DbError(m + " in " + sql);
    }
  }
  sqlite3* raw() { return h_; }
 private:
  sqlite3* h_ = nullptr;
};

// ---------- domain types --------------------------------------------------

enum class State { Registered, Indexed, Active, Stale, Deprecated, Archived };

inline std::string to_string(State s) {
  switch (s) {
    case State::Registered: return "REGISTERED";
    case State::Indexed:    return "INDEXED";
    case State::Active:     return "ACTIVE";
    case State::Stale:      return "STALE";
    case State::Deprecated: return "DEPRECATED";
    case State::Archived:   return "ARCHIVED";
  }
  return "REGISTERED";
}
inline State state_from_string(const std::string& s) {
  if (s == "INDEXED") return State::Indexed;
  if (s == "ACTIVE") return State::Active;
  if (s == "STALE") return State::Stale;
  if (s == "DEPRECATED") return State::Deprecated;
  if (s == "ARCHIVED") return State::Archived;
  return State::Registered;
}

// Which matching engines contribute to a search. `Hybrid` (the default) blends
// all three: exact token match (dominant), FTS5 stemmed full-text (recall for
// train/training, optimizer/optimizers, prefixes), and fuzzy edit-distance
// (typo tolerance). The single-engine modes exist for inspection and testing -
// `Exact` reproduces the original pre-FTS behavior verbatim.
enum class SearchMode { Hybrid, Exact, Fts, Fuzzy };

inline SearchMode search_mode_from_string(const std::string& s) {
  if (s == "exact") return SearchMode::Exact;
  if (s == "fts") return SearchMode::Fts;
  if (s == "fuzzy") return SearchMode::Fuzzy;
  return SearchMode::Hybrid;  // "hybrid" or anything unrecognized
}

inline std::string to_string(SearchMode m) {
  switch (m) {
    case SearchMode::Exact: return "exact";
    case SearchMode::Fts:   return "fts";
    case SearchMode::Fuzzy: return "fuzzy";
    case SearchMode::Hybrid: return "hybrid";
  }
  return "hybrid";
}

struct SkillRow {
  long long id = 0;
  std::string skill_id, description, keywords, path, content_hash, version;
  State state = State::Registered;
  long long size_bytes = 0, search_count = 0, fetch_count = 0;
  std::string registered_at, updated_at, last_searched_at, last_fetched_at;
};

struct SearchHit {
  std::string skill_id, description, path;
  double score = 0.0;
  State state = State::Registered;
};

struct RegisterResult {
  bool ok = false;
  bool created = false;      // true if this was a new row
  bool updated = false;      // true if content_hash changed (re-indexed)
  std::string skill_id;
  std::string error;
};

struct GraveyardCandidate {
  std::string skill_id;
  long long search_count = 0;
  long long fetch_count = 0;
};

// One row of the append-only search_log: the raw event stream behind the
// ranking boost and the graveyard report (SUGGESTED per returned candidate,
// FETCHED per body load, USED per ON_SKILL_MENTION signal). Surfaced so an
// operator view can show live activity.
struct LogEvent {
  std::string ts, query, skill_id, event;
  long long rank = 0;
  double score = 0.0;
};

// ---------- engine --------------------------------------------------------

class SkillLibrary {
 public:
  explicit SkillLibrary(const std::string& db_path) : db_(db_path) { init_schema(); }

  void init_schema() {
    db_.exec(
        "CREATE TABLE IF NOT EXISTS skills ("
        " id INTEGER PRIMARY KEY,"
        " skill_id TEXT NOT NULL UNIQUE,"
        " description TEXT NOT NULL,"
        " keywords TEXT NOT NULL DEFAULT '',"
        " path TEXT NOT NULL,"
        " content_hash TEXT NOT NULL DEFAULT '',"
        " size_bytes INTEGER NOT NULL DEFAULT 0,"
        " version TEXT NOT NULL DEFAULT '',"
        " state TEXT NOT NULL DEFAULT 'REGISTERED',"
        " search_count INTEGER NOT NULL DEFAULT 0,"
        " fetch_count INTEGER NOT NULL DEFAULT 0,"
        " registered_at TEXT NOT NULL DEFAULT (datetime('now')),"
        " updated_at TEXT NOT NULL DEFAULT (datetime('now')),"
        " last_searched_at TEXT,"
        " last_fetched_at TEXT"
        ") STRICT;");
    db_.exec(
        "CREATE TABLE IF NOT EXISTS search_log ("
        " id INTEGER PRIMARY KEY,"
        " ts TEXT NOT NULL DEFAULT (datetime('now')),"
        " query TEXT NOT NULL,"
        " skill_id TEXT NOT NULL,"
        " rank INTEGER NOT NULL,"
        " score REAL NOT NULL,"
        " event TEXT NOT NULL"
        ") STRICT;");
    init_fts();
  }

  // FTS5 full-text index over the searchable columns (skill_id, description,
  // keywords - never the body), using the Porter stemmer so morphological
  // variants match ("train"~"training", "optimizer"~"optimizers") plus prefix
  // queries. It is an *external-content* table backed by `skills`, kept in sync
  // by triggers, so there is no second copy of the data to drift - the FTS
  // index stores only tokens, and the columns are read back from `skills`.
  //
  // The whole thing is wrapped so the engine degrades gracefully to exact +
  // fuzzy search if this SQLite build lacks FTS5 (i.e. was compiled without
  // -DSQLITE_ENABLE_FTS5): fts_enabled_ stays false and search() skips the FTS
  // signal rather than failing.
  void init_fts() {
    try {
      db_.exec(
          "CREATE VIRTUAL TABLE IF NOT EXISTS skills_fts USING fts5("
          " skill_id, description, keywords,"
          " content='skills', content_rowid='id',"
          " tokenize='porter unicode61');");
      // Sync triggers. The UPDATE trigger is scoped to the indexed columns, so
      // telemetry-only updates (search_count/fetch_count/state) do NOT churn
      // the FTS index - only real content changes do.
      db_.exec(
          "CREATE TRIGGER IF NOT EXISTS skills_fts_ai AFTER INSERT ON skills BEGIN"
          " INSERT INTO skills_fts(rowid, skill_id, description, keywords)"
          " VALUES (new.id, new.skill_id, new.description, new.keywords);"
          " END;");
      db_.exec(
          "CREATE TRIGGER IF NOT EXISTS skills_fts_ad AFTER DELETE ON skills BEGIN"
          " INSERT INTO skills_fts(skills_fts, rowid, skill_id, description, keywords)"
          " VALUES('delete', old.id, old.skill_id, old.description, old.keywords);"
          " END;");
      db_.exec(
          "CREATE TRIGGER IF NOT EXISTS skills_fts_au"
          " AFTER UPDATE OF skill_id, description, keywords ON skills BEGIN"
          " INSERT INTO skills_fts(skills_fts, rowid, skill_id, description, keywords)"
          " VALUES('delete', old.id, old.skill_id, old.description, old.keywords);"
          " INSERT INTO skills_fts(rowid, skill_id, description, keywords)"
          " VALUES (new.id, new.skill_id, new.description, new.keywords);"
          " END;");
      fts_enabled_ = true;
      // Self-heal: if the FTS index is out of sync with skills (e.g. a database
      // created before FTS existed, or rows inserted while it was disabled),
      // rebuild it once from the content table.
      Stmt sc(db_.raw(), "SELECT (SELECT COUNT(*) FROM skills),(SELECT COUNT(*) FROM skills_fts)");
      if (sc.step() && sc.col_i(0) != sc.col_i(1))
        db_.exec("INSERT INTO skills_fts(skills_fts) VALUES('rebuild');");
    } catch (const DbError&) {
      fts_enabled_ = false;
    }
  }

  bool fts_enabled() const { return fts_enabled_; }

  // -- registration: reads ONE skill's SKILL.md, parses frontmatter, upserts --
  // the index row. Never stores the body - only the path to re-read it from
  // at fetch time. Returns a result describing what happened (created vs
  // re-indexed vs unchanged vs error) so index() can report aggregate stats.
  RegisterResult register_skill(const std::string& skill_md_path, const std::string& explicit_keywords = "") {
    RegisterResult r;
    std::ifstream f(skill_md_path, std::ios::binary);
    if (!f) { r.error = "cannot open " + skill_md_path; return r; }
    std::ostringstream ss; ss << f.rdbuf();
    std::string text = ss.str();
    if (text.size() > 8u * 1024 * 1024) { r.error = "file too large (>8MiB)"; return r; }

    Frontmatter fm = parse_frontmatter(text);
    if (!fm.valid) { r.error = "invalid or missing frontmatter (need name: and description:)"; return r; }
    if (looks_like_xml_tag(fm.description)) {
      r.error = "description contains XML/HTML-tag-shaped text (e.g. \"<query>\") - "
                "use word-based placeholders instead (e.g. \"your query text\")";
      return r;
    }
    if (fm.description.size() > kMaxDescBytes) fm.description.resize(kMaxDescBytes);

    std::string hash = fnv1a64(text);
    std::string keywords = explicit_keywords.empty()
        ? derive_keywords(fm.name + " " + fm.description) : explicit_keywords;

    Stmt sel(db_.raw(), "SELECT content_hash, state FROM skills WHERE skill_id=?");
    sel.bind(1, fm.name);
    bool exists = sel.step();
    std::string old_hash = exists ? sel.col_s(0) : "";
    State old_state = exists ? state_from_string(sel.col_s(1)) : State::Registered;

    if (!exists) {
      Stmt ins(db_.raw(),
          "INSERT INTO skills (skill_id,description,keywords,path,content_hash,"
          "size_bytes,version,state) VALUES (?,?,?,?,?,?,?,?)");
      ins.bind(1, fm.name).bind(2, fm.description).bind(3, keywords).bind(4, skill_md_path)
         .bind(5, hash).bind(6, static_cast<long long>(text.size())).bind(7, std::string("1.0"))
         .bind(8, to_string(State::Indexed));
      ins.step();
      r.created = true;
    } else if (old_hash != hash) {
      // content changed on disk since last index: re-index, preserve telemetry,
      // and route through STALE->INDEXED so the transition is explicit and
      // auditable rather than a silent overwrite.
      State next = (old_state == State::Active) ? State::Active : State::Indexed;
      Stmt upd(db_.raw(),
          "UPDATE skills SET description=?, keywords=?, path=?, content_hash=?,"
          " size_bytes=?, state=?, updated_at=datetime('now') WHERE skill_id=?");
      upd.bind(1, fm.description).bind(2, keywords).bind(3, skill_md_path).bind(4, hash)
         .bind(5, static_cast<long long>(text.size())).bind(6, to_string(next)).bind(7, fm.name);
      upd.step();
      r.updated = true;
    }
    r.ok = true;
    r.skill_id = fm.name;
    return r;
  }

  static std::string derive_keywords(const std::string& text) {
    auto toks = tokenize(text);
    std::string out;
    for (size_t i = 0; i < toks.size() && i < 20; ++i) { if (i) out += ","; out += toks[i]; }
    return out;
  }

  // Detects on-disk drift for one skill without re-registering it: compares
  // the currently-stored content_hash against a fresh hash of the file on
  // disk, and if they differ, marks the row STALE. This is the explicit
  // Indexed/Active -> Stale transition; register_skill() performs the
  // reverse (Stale -> Indexed/Active) transition once the drift is resolved.
  bool mark_stale_if_drifted(const std::string& skill_id) {
    Stmt sel(db_.raw(), "SELECT path, content_hash, state FROM skills WHERE skill_id=?");
    sel.bind(1, skill_id);
    if (!sel.step()) return false;
    std::string path = sel.col_s(0), stored_hash = sel.col_s(1);
    State st = state_from_string(sel.col_s(2));
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    std::string cur_hash = fnv1a64(ss.str());
    if (cur_hash != stored_hash && st != State::Stale) {
      set_state(skill_id, State::Stale);
      return true;
    }
    return false;
  }

  void set_state(const std::string& skill_id, State s) {
    Stmt upd(db_.raw(), "UPDATE skills SET state=?, updated_at=datetime('now') WHERE skill_id=?");
    upd.bind(1, to_string(s)).bind(2, skill_id);
    upd.step();
  }

  // Weights for the non-exact signals in hybrid ranking. Both are < 1.0 and
  // their maximum combined contribution (kFtsWeight + kFuzzyWeight = 0.95) is
  // kept strictly below one exact-match tier (1.0), so FTS/fuzzy can surface
  // skills that have NO exact token hit and break ties among equal exact
  // scores, yet can never overturn a clear exact-token winner. Exact match
  // stays the spine of the ranking; FTS and fuzzy only add recall.
  static constexpr double kFtsWeight = 0.6;
  static constexpr double kFuzzyWeight = 0.35;

  // -- search: index-only, never reads a skill body. Hybrid by default -
  // exact token match (dominant) + FTS5 stemmed full-text (train~training,
  // optimizer~optimizers, prefixes) + fuzzy edit-distance (typos). SearchMode
  // can isolate any single engine (Exact reproduces the original behavior).
  // Logs one SUGGESTED row per returned hit (telemetry for graveyard/boost).
  std::vector<SearchHit> search(const std::string& query, int top_n = 8,
                                bool include_archived = false,
                                SearchMode mode = SearchMode::Hybrid) {
    if (query.size() > kMaxQueryBytes) throw DbError("query exceeds max size");
    auto tokens = tokenize(query);
    const bool use_exact = (mode == SearchMode::Hybrid || mode == SearchMode::Exact);
    const bool use_fts   = (mode == SearchMode::Hybrid || mode == SearchMode::Fts);
    const bool use_fuzzy = (mode == SearchMode::Hybrid || mode == SearchMode::Fuzzy);

    // FTS pass first: skill_id -> raw relevance. Normalized per query into
    // [0.5, 1.0] so any stemmed/prefix match contributes, best matches most.
    std::map<std::string, double> fts_raw = use_fts ? fts_scores(query)
                                                    : std::map<std::string, double>{};
    double fmin = 0.0, fmax = 0.0; bool ffirst = true;
    for (const auto& [k, v] : fts_raw) {
      if (ffirst) { fmin = fmax = v; ffirst = false; }
      else { fmin = std::min(fmin, v); fmax = std::max(fmax, v); }
    }
    auto fts_norm = [&](const std::string& id) -> double {
      auto it = fts_raw.find(id);
      if (it == fts_raw.end()) return 0.0;
      double n = (fmax > fmin) ? (it->second - fmin) / (fmax - fmin) : 1.0;
      return 0.5 + 0.5 * n;
    };

    std::vector<SearchHit> hits;
    Stmt sel(db_.raw(), "SELECT skill_id,description,keywords,path,state,fetch_count,search_count FROM skills");
    while (sel.step()) {
      State st = state_from_string(sel.col_s(4));
      if (st == State::Archived && !include_archived) continue;
      std::string skill_id = sel.col_s(0), desc = sel.col_s(1), kw = lower(sel.col_s(2));
      long long fetch_count = sel.col_i(5), search_count = sel.col_i(6);

      auto kw_set = split_csv(kw);
      auto name_toks = tokenize(skill_id);
      auto desc_toks = tokenize(desc);
      std::set<std::string> name_set(name_toks.begin(), name_toks.end());
      std::set<std::string> desc_set(desc_toks.begin(), desc_toks.end());

      double exact = 0.0;
      if (use_exact) {
        for (const auto& t : tokens) {
          if (kw_set.count(t)) exact += 3.0;
          else if (name_set.count(t)) exact += 2.0;
          else if (desc_set.count(t)) exact += 1.0;
        }
      }

      double fuzzy = 0.0;
      if (use_fuzzy && !tokens.empty()) {
        std::set<std::string> pool = kw_set;               // full token pool for this skill
        pool.insert(name_set.begin(), name_set.end());
        pool.insert(desc_set.begin(), desc_set.end());
        double acc = 0.0;
        for (const auto& t : tokens) {
          // Only fuzzy-match the query tokens that exact matching missed -
          // fuzzy is for typos/near-misses, not for re-scoring exact hits.
          if (kw_set.count(t) || name_set.count(t) || desc_set.count(t)) continue;
          int maxd = (t.size() <= 4) ? 1 : 2;              // stricter on short tokens
          int best = maxd + 1;
          for (const auto& pt : pool) {
            int d = bounded_edit_distance(t, pt, maxd);
            if (d < best) best = d;
            if (best <= 1) break;                          // can't do better that matters
          }
          if (best <= maxd) acc += 1.0 - static_cast<double>(best) / static_cast<double>(t.size());
        }
        fuzzy = acc / static_cast<double>(tokens.size());  // in [0, 1]
      }

      double base = exact + kFtsWeight * fts_norm(skill_id) + kFuzzyWeight * fuzzy;
      if (base <= 0.0) continue;

      // Deterministic, bounded telemetry boost (unchanged): skills that convert
      // suggestions into fetches rank incrementally higher over time, capped so
      // a handful of historical fetches can't drown out a strong fresh match.
      double conversion = search_count > 0 ? static_cast<double>(fetch_count) / search_count : 0.0;
      double boost = 1.0 + std::min(0.5, conversion * 0.5 + std::log1p(static_cast<double>(fetch_count)) * 0.05);
      double score = base * boost;
      if (st == State::Deprecated) score *= 0.3;

      hits.push_back({skill_id, desc, sel.col_s(3), score, st});
    }
    std::sort(hits.begin(), hits.end(), [](const SearchHit& a, const SearchHit& b) {
      if (a.score != b.score) return a.score > b.score;
      return a.skill_id < b.skill_id;   // deterministic tie-break
    });
    if (static_cast<int>(hits.size()) > top_n) hits.resize(top_n);

    for (size_t i = 0; i < hits.size(); ++i) {
      log_event(query, hits[i].skill_id, static_cast<int>(i), hits[i].score, "SUGGESTED");
      bump_search_count(hits[i].skill_id);
    }
    return hits;
  }

  // FTS5 stemmed match: skill_id -> raw relevance (higher = better). Builds an
  // OR of quoted, prefixed query tokens ("tok"*) so the Porter stemmer + prefix
  // matching both apply, and reads bm25 with the same column priority as exact
  // scoring (keywords highest, then skill_id, then description). Returns empty
  // if FTS is disabled, there are no tokens, or the MATCH expression is
  // rejected - callers treat an absent entry as "no FTS signal", never an error.
  std::map<std::string, double> fts_scores(const std::string& query) {
    std::map<std::string, double> out;
    if (!fts_enabled_) return out;
    auto toks = tokenize(query);
    if (toks.empty()) return out;
    std::string match;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (i) match += " OR ";
      match += "\"" + toks[i] + "\"*";
    }
    try {
      Stmt sel(db_.raw(),
          "SELECT skill_id, bm25(skills_fts, 2.0, 1.0, 3.0) FROM skills_fts WHERE skills_fts MATCH ?");
      sel.bind(1, match);
      while (sel.step()) out[sel.col_s(0)] = -sel.col_d(1);  // bm25: lower is better -> negate
    } catch (const DbError&) {
      out.clear();  // malformed MATCH -> degrade to no FTS signal
    }
    return out;
  }

  // -- fetch: the ONE place a skill's full body is read and returned. Bumps
  // fetch_count, logs a FETCHED event, and performs the Indexed->Active
  // transition (guarded: Archived/Deprecated skills are still fetchable on
  // explicit request, but do not transition state).
  std::string fetch_body(const std::string& skill_id, const std::string& query_context = "") {
    Stmt sel(db_.raw(), "SELECT path, state FROM skills WHERE skill_id=?");
    sel.bind(1, skill_id);
    if (!sel.step()) throw DbError("unknown skill_id: " + skill_id);
    std::string path = sel.col_s(0);
    State st = state_from_string(sel.col_s(1));

    std::ifstream f(path, std::ios::binary);
    if (!f) throw DbError("skill file missing on disk: " + path);
    std::ostringstream ss; ss << f.rdbuf();
    std::string body = ss.str();

    Stmt upd(db_.raw(),
        "UPDATE skills SET fetch_count=fetch_count+1, last_fetched_at=datetime('now')"
        " WHERE skill_id=?");
    upd.bind(1, skill_id);
    upd.step();
    if (st == State::Registered || st == State::Indexed) set_state(skill_id, State::Active);
    log_event(query_context, skill_id, 0, 0.0, "FETCHED");
    return body;
  }

  void log_used(const std::string& skill_id, const std::string& query_context = "") {
    log_event(query_context, skill_id, 0, 0.0, "USED");
  }

  // Most-recent search_log events, returned oldest-first (chronological, so a
  // live view can append newest at the bottom). Index-only: never reads a
  // skill body. Cheap regardless of library size.
  std::vector<LogEvent> recent_events(int limit = 10) {
    std::vector<LogEvent> out;
    Stmt sel(db_.raw(),
        "SELECT ts, query, skill_id, rank, score, event FROM search_log ORDER BY id DESC LIMIT ?");
    sel.bind(1, static_cast<long long>(limit));
    while (sel.step())
      out.push_back({sel.col_s(0), sel.col_s(1), sel.col_s(2), sel.col_s(5),
                     sel.col_i(3), sel.col_d(4)});
    std::reverse(out.begin(), out.end());
    return out;
  }

  // Total rows in search_log - the full event count, for the status header.
  long long total_events() {
    Stmt sel(db_.raw(), "SELECT COUNT(*) FROM search_log");
    sel.step();
    return sel.col_i(0);
  }

  // Public accessor for search_log, used by stats/tests; avoids exposing db_.
  long long count_events(const std::string& skill_id, const std::string& event) {
    Stmt sel(db_.raw(), "SELECT COUNT(*) FROM search_log WHERE skill_id=? AND event=?");
    sel.bind(1, skill_id).bind(2, event);
    sel.step();
    return sel.col_i(0);
  }

  bool get_row(const std::string& skill_id, SkillRow& out) {
    Stmt sel(db_.raw(),
        "SELECT id,skill_id,description,keywords,path,content_hash,size_bytes,version,"
        "state,search_count,fetch_count,registered_at,updated_at,last_searched_at,last_fetched_at"
        " FROM skills WHERE skill_id=?");
    sel.bind(1, skill_id);
    if (!sel.step()) return false;
    out = {sel.col_i(0), sel.col_s(1), sel.col_s(2), sel.col_s(3), sel.col_s(4), sel.col_s(5),
           sel.col_s(7), state_from_string(sel.col_s(8)), sel.col_i(6), sel.col_i(9), sel.col_i(10),
           sel.col_s(11), sel.col_s(12), sel.col_s(13), sel.col_s(14)};
    return true;
  }

  std::vector<SkillRow> list_all(int limit = 100000) {
    std::vector<SkillRow> out;
    Stmt sel(db_.raw(),
        "SELECT id,skill_id,description,keywords,path,content_hash,size_bytes,version,"
        "state,search_count,fetch_count,registered_at,updated_at,last_searched_at,last_fetched_at"
        " FROM skills ORDER BY skill_id LIMIT ?");
    sel.bind(1, static_cast<long long>(limit));
    while (sel.step())
      out.push_back({sel.col_i(0), sel.col_s(1), sel.col_s(2), sel.col_s(3), sel.col_s(4), sel.col_s(5),
                     sel.col_s(7), state_from_string(sel.col_s(8)), sel.col_i(6), sel.col_i(9),
                     sel.col_i(10), sel.col_s(11), sel.col_s(12), sel.col_s(13), sel.col_s(14)});
    return out;
  }

  std::map<std::string, long long> state_counts() {
    std::map<std::string, long long> out;
    Stmt sel(db_.raw(), "SELECT state, COUNT(*) FROM skills GROUP BY state");
    while (sel.step()) out[sel.col_s(0)] = sel.col_i(1);
    return out;
  }

  // Graveyard candidates: suggested often, never (or rarely) fetched -
  // exactly the doctrine's "extract value from failure / do not let every
  // branch remain alive" signal, computed from real telemetry.
  std::vector<GraveyardCandidate> graveyard_candidates(long long min_searches = 5, double max_conversion = 0.05) {
    std::vector<GraveyardCandidate> out;
    Stmt sel(db_.raw(),
        "SELECT skill_id, search_count, fetch_count FROM skills"
        " WHERE search_count >= ? AND state != 'ARCHIVED'"
        " ORDER BY search_count DESC");
    sel.bind(1, min_searches);
    while (sel.step()) {
      long long sc = sel.col_i(1), fc = sel.col_i(2);
      double conv = sc > 0 ? static_cast<double>(fc) / sc : 0.0;
      if (conv <= max_conversion) out.push_back({sel.col_s(0), sc, fc});
    }
    return out;
  }

  struct Telemetry {
    long long total_searches = 0, total_fetches = 0;
    double overall_conversion = 0.0;
  };
  Telemetry telemetry() {
    Telemetry t;
    Stmt s1(db_.raw(), "SELECT COUNT(*) FROM search_log WHERE event='SUGGESTED'");
    s1.step(); t.total_searches = s1.col_i(0);
    Stmt s2(db_.raw(), "SELECT COUNT(*) FROM search_log WHERE event='FETCHED'");
    s2.step(); t.total_fetches = s2.col_i(0);
    t.overall_conversion = t.total_searches > 0
        ? static_cast<double>(t.total_fetches) / t.total_searches : 0.0;
    return t;
  }

 private:
  void bump_search_count(const std::string& skill_id) {
    Stmt upd(db_.raw(),
        "UPDATE skills SET search_count=search_count+1, last_searched_at=datetime('now')"
        " WHERE skill_id=?");
    upd.bind(1, skill_id);
    upd.step();
  }
  void log_event(const std::string& query, const std::string& skill_id, int rank, double score,
                const std::string& event) {
    Stmt ins(db_.raw(),
        "INSERT INTO search_log (query,skill_id,rank,score,event) VALUES (?,?,?,?,?)");
    ins.bind(1, query).bind(2, skill_id).bind(3, static_cast<long long>(rank)).bind(4, score).bind(5, event);
    ins.step();
  }
  static std::set<std::string> split_csv(const std::string& s) {
    std::set<std::string> out; std::string cur;
    for (char c : s) {
      if (c == ',') { if (!cur.empty()) out.insert(cur); cur.clear(); }
      else if (!std::isspace(static_cast<unsigned char>(c))) cur += c;
    }
    if (!cur.empty()) out.insert(cur);
    return out;
  }

  Db db_;
  bool fts_enabled_ = false;  // set by init_fts(); false if this SQLite lacks FTS5
};

}  // namespace skilllib
