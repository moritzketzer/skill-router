// main.cpp - skillrouter: the consolidated interface/router binary.
//
// This is the ONE tool a harness or model needs on the PATH to work with a
// skill library of arbitrary size (10, 10,000, whatever) without ever
// loading skill bodies into context except on explicit fetch.
//
//   skillrouter register <SKILL.md path> [--db PATH]
//   skillrouter index    <root dir>      [--db PATH]     bulk (re)index a tree
//   skillrouter search   "<query>"       [--db PATH] [--top N] [--json]
//   skillrouter fetch    <skill_id>      [--db PATH] [--context "<query>"]
//   skillrouter stats    [--db PATH]                       telemetry summary
//   skillrouter graveyard [--db PATH] [--min-searches N]    low-conversion report
//   skillrouter deprecate <skill_id> [--db PATH]
//   skillrouter archive   <skill_id> [--db PATH]
//   skillrouter shell     [--db PATH]                      interactive REPL
//   skillrouter serve     [--db PATH] [--port 8090]         HTTP API
//   skillrouter mcp       [--db PATH]      Model Context Protocol server (stdio)
//
// Design mirrors ctxmgr's hardened patterns (loopback-only serve, whole-
// connection exception guard, sigaction-based shutdown, request-size caps)
// but this binary is fully self-contained: it links only the SQLite
// amalgamation, nothing else, so it drops onto a machine with zero
// dependencies.
#include "skill_library.hpp"
#include "mcp_server.hpp"

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <fcntl.h>
  #include <io.h>
  #pragma comment(lib, "ws2_32.lib")
  using socket_t = SOCKET;
  #define CLOSESOCK closesocket
  static void platform_net_init() { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
  static void platform_net_cleanup() { WSACleanup(); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_t = int;
  #define CLOSESOCK close
  static void platform_net_init() {}
  static void platform_net_cleanup() {}
#endif

#include <algorithm>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>

namespace fs = std::filesystem;
using namespace skilllib;

namespace {

constexpr size_t kMaxRequestBytes = 2u * 1024u * 1024u;

struct Args {
  std::string db = "skill_index.db";
  std::string target, query, context_query, format = "text", mode = "hybrid";
  int top = 8, port = 8090;
  long long min_searches = 5;
  bool include_archived = false;
};

Args parse(int argc, char** argv, int start) {
  Args a;
  for (int i = start; i < argc; ++i) {
    std::string s = argv[i];
    auto next = [&](const char* opt) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + opt);
      return argv[++i];
    };
    if (s == "--db") a.db = next("--db");
    else if (s == "--top") a.top = std::stoi(next("--top"));
    else if (s == "--json") a.format = "json";
    else if (s == "--port") a.port = std::stoi(next("--port"));
    else if (s == "--context") a.context_query = next("--context");
    else if (s == "--mode") a.mode = next("--mode");
    else if (s == "--min-searches") a.min_searches = std::stoll(next("--min-searches"));
    else if (s == "--include-archived") a.include_archived = true;
    else if (a.target.empty()) a.target = s;
    else { if (!a.query.empty()) a.query += " "; a.query += s; }
  }
  if (a.query.empty() && !a.target.empty()) { a.query = a.target; }
  return a;
}

std::string state_json(const SkillRow& r) {
  std::ostringstream o;
  o << "{\"skill_id\":\"" << json_escape(r.skill_id) << "\",\"description\":\""
    << json_escape(r.description) << "\",\"state\":\"" << to_string(r.state)
    << "\",\"search_count\":" << r.search_count << ",\"fetch_count\":" << r.fetch_count
    << ",\"path\":\"" << json_escape(r.path) << "\"}";
  return o.str();
}

// -------- recursive directory walk for `index` (portable: std::filesystem) ---
void walk_for_skill_md(const std::string& root, std::vector<std::string>& out) {
  std::error_code ec;
  if (!fs::exists(root, ec) || ec) return;
  for (const auto& entry : fs::recursive_directory_iterator(
           root, fs::directory_options::skip_permission_denied, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec) || ec) continue;
    std::string name = entry.path().filename().string();
    if (name == "SKILL.md" || (name.size() > 9 &&
        name.compare(name.size() - 9, 9, "_SKILL.md") == 0))
      out.push_back(entry.path().string());
  }
}

// ---------------------------- subcommands -------------------------------------

int cmd_register(const Args& a) {
  if (a.query.empty()) throw std::runtime_error("register requires a SKILL.md path");
  SkillLibrary lib(a.db);
  auto r = lib.register_skill(a.query);
  if (!r.ok) {
    std::cout << "{\"ok\":false,\"error\":\"" << json_escape(r.error) << "\"}\n";
    return 1;
  }
  std::cout << "{\"ok\":true,\"skill_id\":\"" << json_escape(r.skill_id)
            << "\",\"created\":" << (r.created ? "true" : "false")
            << ",\"updated\":" << (r.updated ? "true" : "false") << "}\n";
  return 0;
}

int cmd_index(const Args& a) {
  if (a.query.empty()) throw std::runtime_error("index requires a root directory");
  SkillLibrary lib(a.db);
  std::vector<std::string> files;
  walk_for_skill_md(a.query, files);
  int created = 0, updated = 0, unchanged = 0, errors = 0;
  for (const auto& f : files) {
    auto r = lib.register_skill(f);
    if (!r.ok) { ++errors; std::cerr << "  ! " << f << ": " << r.error << "\n"; }
    else if (r.created) ++created;
    else if (r.updated) ++updated;
    else ++unchanged;
  }
  std::cout << "{\"ok\":true,\"scanned\":" << files.size() << ",\"created\":" << created
            << ",\"updated\":" << updated << ",\"unchanged\":" << unchanged
            << ",\"errors\":" << errors << "}\n";
  return 0;
}

int cmd_search(const Args& a) {
  if (a.query.empty()) throw std::runtime_error("search requires a query string");
  SkillLibrary lib(a.db);
  auto hits = lib.search(a.query, a.top, a.include_archived, search_mode_from_string(a.mode));
  if (a.format == "json") {
    std::cout << "[";
    for (size_t i = 0; i < hits.size(); ++i) {
      const auto& h = hits[i];
      std::cout << (i ? "," : "") << "{\"skill_id\":\"" << json_escape(h.skill_id)
                << "\",\"description\":\"" << json_escape(h.description)
                << "\",\"score\":" << h.score << ",\"state\":\"" << to_string(h.state) << "\"}";
    }
    std::cout << "]\n";
  } else {
    if (hits.empty()) { std::cout << "(no matching skills)\n"; return 0; }
    for (const auto& h : hits)
      std::cout << h.skill_id << "  (score " << h.score << ", " << to_string(h.state)
                << ")\n    " << h.description << "\n";
  }
  return 0;
}

int cmd_fetch(const Args& a) {
  if (a.query.empty()) throw std::runtime_error("fetch requires a skill_id");
  SkillLibrary lib(a.db);
  std::cout << lib.fetch_body(a.query, a.context_query);
  return 0;
}

int cmd_use(const Args& a) {
  // Wires the harness's ON_SKILL_MENTION hook: a soft signal, distinct from
  // FETCHED, that the model actually incorporated a previously-fetched
  // skill's guidance into its response (as opposed to fetching it and then
  // not using it). Logged, not currently folded into the ranking boost -
  // documented extension point in INTEGRATION_MANUAL.md.
  if (a.query.empty()) throw std::runtime_error("use requires a skill_id");
  SkillLibrary lib(a.db);
  lib.log_used(a.query, a.context_query);
  std::cout << "{\"ok\":true,\"skill_id\":\"" << json_escape(a.query) << "\",\"event\":\"USED\"}\n";
  return 0;
}

int cmd_stats(const Args& a) {
  SkillLibrary lib(a.db);
  auto t = lib.telemetry();
  auto counts = lib.state_counts();
  std::ostringstream o;
  o << "{\"total_searches\":" << t.total_searches << ",\"total_fetches\":" << t.total_fetches
    << ",\"overall_conversion\":" << t.overall_conversion << ",\"by_state\":{";
  bool first = true;
  for (const auto& [k, v] : counts) { o << (first ? "" : ",") << "\"" << k << "\":" << v; first = false; }
  o << "}}";
  std::cout << o.str() << "\n";
  return 0;
}

int cmd_graveyard(const Args& a) {
  SkillLibrary lib(a.db);
  auto cands = lib.graveyard_candidates(a.min_searches);
  std::cout << "[";
  for (size_t i = 0; i < cands.size(); ++i) {
    const auto& c = cands[i];
    double conv = c.search_count > 0 ? static_cast<double>(c.fetch_count) / c.search_count : 0.0;
    std::cout << (i ? "," : "") << "{\"skill_id\":\"" << json_escape(c.skill_id)
              << "\",\"search_count\":" << c.search_count << ",\"fetch_count\":" << c.fetch_count
              << ",\"conversion\":" << conv << "}";
  }
  std::cout << "]\n";
  return 0;
}

int cmd_deprecate(const Args& a) {
  if (a.query.empty()) throw std::runtime_error("deprecate requires a skill_id");
  SkillLibrary lib(a.db);
  lib.set_state(a.query, State::Deprecated);
  std::cout << "{\"ok\":true,\"skill_id\":\"" << json_escape(a.query) << "\",\"state\":\"DEPRECATED\"}\n";
  return 0;
}

int cmd_archive(const Args& a) {
  if (a.query.empty()) throw std::runtime_error("archive requires a skill_id");
  SkillLibrary lib(a.db);
  lib.set_state(a.query, State::Archived);
  std::cout << "{\"ok\":true,\"skill_id\":\"" << json_escape(a.query) << "\",\"state\":\"ARCHIVED\"}\n";
  return 0;
}

// Renders the live operator dashboard for the interactive shell: the library
// at a glance (skill count by state), telemetry (searches/fetches/conversion),
// and the tail of the search_log event stream. All index-only - it never
// reads a skill body, so it stays cheap no matter how large the library is.
// Re-rendered each prompt cycle so counts and events update as you work.
void render_dashboard(SkillLibrary& lib, const std::string& db, int event_tail = 8) {
  auto counts = lib.state_counts();
  auto t = lib.telemetry();
  long long total = 0;
  for (const auto& [k, v] : counts) total += v;

  std::cout << "\n=== skillrouter live status =============================================\n";
  std::cout << " db " << db << "   engine v" << kEngineVersion << "   events " << lib.total_events() << "\n";
  std::cout << " skills " << total;
  if (!counts.empty()) {
    std::cout << "  [";
    bool first = true;
    for (const auto& [k, v] : counts) { std::cout << (first ? "" : ", ") << k << " " << v; first = false; }
    std::cout << "]";
  }
  std::cout << "\n searches " << t.total_searches << "   fetches " << t.total_fetches
            << "   conversion " << t.overall_conversion << "\n";

  auto ev = lib.recent_events(event_tail);
  std::cout << " recent events" << (ev.empty() ? " (none yet)" : ":") << "\n";
  for (const auto& e : ev) {
    // left-pad the event name to a fixed width so the columns line up
    std::string name = e.event;
    if (name.size() < 9) name += std::string(9 - name.size(), ' ');
    std::cout << "   " << (e.ts.empty() ? "--" : e.ts) << "  " << name << "  " << e.skill_id;
    if (!e.query.empty()) std::cout << "   q=\"" << e.query << "\"";
    std::cout << "\n";
  }
  std::cout << "========================================================================\n";
}

void shell_help() {
  std::cout <<
      "commands:\n"
      "  search <query>     search the library (logs SUGGESTED events)\n"
      "  fetch  <skill_id>  load a skill's full body (logs a FETCHED event)\n"
      "  events [N]         show the last N search_log events (default 20)\n"
      "  status             redraw the live status dashboard\n"
      "  stats              telemetry summary\n"
      "  graveyard          skills suggested often but rarely fetched\n"
      "  deprecate <id>     rank a skill down (still fetchable)\n"
      "  archive   <id>     exclude a skill from search by default\n"
      "  help               show this list\n"
      "  quit | exit        leave the shell\n";
}

int cmd_shell(const Args& a) {
  SkillLibrary lib(a.db);
  std::cout << "skillrouter interactive shell (skill_library engine v" << kEngineVersion << ")\n"
            << "type 'help' for commands, 'quit' to exit. Status refreshes after each command.\n";
  shell_help();
  std::string line;
  while (true) {
    render_dashboard(lib, a.db);        // live refresh at the top of every cycle
    std::cout << "skillrouter> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (line.empty()) continue;         // bare Enter just redraws the dashboard
    std::istringstream iss(line);
    std::string cmd; iss >> cmd;
    std::string rest; std::getline(iss, rest);
    if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
    try {
      if (cmd == "quit" || cmd == "exit") break;
      else if (cmd == "help") { shell_help(); }
      else if (cmd == "status") { /* dashboard redraws at next loop top */ }
      else if (cmd == "search") {
        auto hits = lib.search(rest, 8);
        if (hits.empty()) std::cout << "(no matches)\n";
        for (auto& h : hits)
          std::cout << "  " << h.skill_id << " (" << h.score << ", " << to_string(h.state)
                    << "): " << h.description << "\n";
      } else if (cmd == "fetch") {
        std::cout << lib.fetch_body(rest) << "\n";
      } else if (cmd == "events") {
        int n = 20;
        if (!rest.empty()) { try { n = std::max(1, std::stoi(rest)); } catch (...) {} }
        auto ev = lib.recent_events(n);
        if (ev.empty()) std::cout << "(no events yet)\n";
        for (const auto& e : ev) {
          std::string name = e.event;
          if (name.size() < 9) name += std::string(9 - name.size(), ' ');
          std::cout << "  " << (e.ts.empty() ? "--" : e.ts) << "  " << name << "  " << e.skill_id;
          if (!e.query.empty()) std::cout << "   q=\"" << e.query << "\"";
          std::cout << "\n";
        }
      } else if (cmd == "stats") {
        auto t = lib.telemetry();
        std::cout << "searches=" << t.total_searches << " fetches=" << t.total_fetches
                  << " conversion=" << t.overall_conversion << "\n";
      } else if (cmd == "graveyard") {
        auto cands = lib.graveyard_candidates();
        if (cands.empty()) std::cout << "(no graveyard candidates)\n";
        for (auto& c : cands)
          std::cout << "  " << c.skill_id << " searched=" << c.search_count
                    << " fetched=" << c.fetch_count << "\n";
      } else if (cmd == "deprecate") { lib.set_state(rest, State::Deprecated); std::cout << "ok\n"; }
      else if (cmd == "archive") { lib.set_state(rest, State::Archived); std::cout << "ok\n"; }
      else std::cout << "unknown command: " << cmd << "  (type 'help')\n";
    } catch (const std::exception& e) {
      std::cout << "error: " << e.what() << "\n";
    }
  }
  return 0;
}

// ---------------------------- MCP stdio server --------------------------------

// `skillrouter mcp` speaks the Model Context Protocol over its stdio transport:
// newline-delimited JSON-RPC 2.0, one message per line, requests on stdin,
// responses on stdout, diagnostics on stderr. This is the transport every MCP
// client (Claude Desktop, Claude Code, Cursor, custom agent loops) supports
// out of the box, so pointing such a client at this binary is all it takes to
// hand any model the full registered skill library - searchable and fetchable
// as MCP tools, and enumerable as MCP resources - without loading any skill
// body into context except on an explicit fetch.
//
// All protocol logic lives in mcp_server.hpp (pure, transport-agnostic,
// unit-tested); this function is only the read/parse-line/write loop. Skill
// telemetry (SUGGESTED/FETCHED) accrues through these calls exactly as it does
// via the CLI, since both go through the same SkillLibrary engine.
int cmd_mcp(const Args& a) {
#ifdef _WIN32
  // Prevent the CRT from translating '\n' to '\r\n' on stdout (which would
  // corrupt the newline framing) and from eating '\r' on stdin. MCP frames
  // messages on bare '\n'; we normalize any stray '\r' ourselves below.
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  SkillLibrary lib(a.db);
  std::cerr << "skillrouter MCP server on stdio (JSON-RPC 2.0), db=" << a.db << "\n";
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF input
    if (line.empty()) continue;  // blank keep-alive line: ignore
    std::optional<std::string> resp = skilllib::mcp::handle_request(lib, line);
    if (resp) { std::cout << *resp << "\n" << std::flush; }  // notifications get no reply
  }
  return 0;
}

// ---------------------------- HTTP server -------------------------------------

volatile std::sig_atomic_t g_stop = 0;
void on_sig(int) { g_stop = 1; }

static void install_shutdown_handler() {
#ifdef _WIN32
  // Windows lacks sigaction/SA_RESTART entirely, so the POSIX "no
  // SA_RESTART" fix ctxmgr needed doesn't apply here - plain signal() is
  // the correct, sufficient mechanism on this platform.
  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);
#else
  // Deliberately sigaction, NOT signal(): signal() sets SA_RESTART on
  // Linux, which silently resumes accept() across SIGTERM and prevents
  // clean shutdown - the exact bug found and fixed in ctxmgr's server (see
  // that project's integration manual). Applying the lesson here up front.
  struct sigaction sa{};
  sa.sa_handler = on_sig;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
#endif
}

std::string http_response(int code, const std::string& ctype, const std::string& body) {
  const char* status = code == 200 ? "200 OK"
                     : code == 404 ? "404 Not Found"
                     : code == 413 ? "413 Payload Too Large"
                                   : "400 Bad Request";
  std::ostringstream o;
  o << "HTTP/1.1 " << status << "\r\nContent-Type: " << ctype
    << "\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\n\r\n" << body;
  return o.str();
}

std::string url_decode(const std::string& s) {
  std::string out; out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() &&
        std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      auto hex = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                                     : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10; };
      out += static_cast<char>((hex(s[i + 1]) << 4) | hex(s[i + 2])); i += 2;
    } else if (s[i] == '+') out += ' ';
    else out += s[i];
  }
  return out;
}

struct ParsedPath { std::string path; std::map<std::string, std::string> query; };
ParsedPath parse_path(const std::string& raw) {
  ParsedPath p; auto q = raw.find('?');
  p.path = q == std::string::npos ? raw : raw.substr(0, q);
  if (q == std::string::npos) return p;
  std::string qs = raw.substr(q + 1); size_t i = 0;
  while (i < qs.size()) {
    auto amp = qs.find('&', i);
    std::string pair = qs.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    auto eq = pair.find('=');
    if (eq != std::string::npos) p.query[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return p;
}

std::string recv_request(socket_t c, bool& too_big) {
  std::string req; char buf[8192];
  size_t hdr_end = std::string::npos, content_len = 0;
  too_big = false;
#ifdef _WIN32
  int r;
#else
  ssize_t r;
#endif
  while ((r = ::recv(c, buf, sizeof(buf), 0)) > 0) {
    req.append(buf, static_cast<size_t>(r));
    if (req.size() > kMaxRequestBytes) { too_big = true; return ""; }
    if (hdr_end == std::string::npos) {
      hdr_end = req.find("\r\n\r\n");
      if (hdr_end != std::string::npos) {
        auto p = req.find("Content-Length:");
        if (p != std::string::npos) {
          try { content_len = std::stoul(req.substr(p + 15)); } catch (...) { content_len = 0; }
        }
      }
    }
    if (hdr_end != std::string::npos && req.size() >= hdr_end + 4 + content_len) break;
  }
  return req;
}

int cmd_serve(const Args& a) {
  platform_net_init();
  SkillLibrary lib(a.db);
  socket_t srv = ::socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
  if (srv == INVALID_SOCKET) throw std::runtime_error("socket() failed");
#else
  if (srv < 0) throw std::runtime_error("socket() failed");
#endif
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
  sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(a.port));
  if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    throw std::runtime_error("bind() failed on port " + std::to_string(a.port));
  if (::listen(srv, 16) != 0) throw std::runtime_error("listen() failed");
  install_shutdown_handler();
  std::cerr << "skillrouter API on http://127.0.0.1:" << a.port
            << "  (GET /health /stats /graveyard /search?q=.., GET /fetch?id=..)\n";

  while (!g_stop) {
    socket_t c = ::accept(srv, nullptr, nullptr);
#ifdef _WIN32
    if (c == INVALID_SOCKET) { if (g_stop) break; continue; }
#else
    if (c < 0) { if (g_stop) break; continue; }
#endif
    std::string resp;
    try {
      bool too_big = false;
      std::string req = recv_request(c, too_big);
      if (too_big) {
        resp = http_response(413, "application/json", "{\"ok\":false,\"error\":\"request too large\"}");
      } else {
        std::istringstream head(req.substr(0, req.find("\r\n")));
        std::string method, rawpath; head >> method >> rawpath;
        ParsedPath pp = parse_path(rawpath);
        if (method == "GET" && pp.path == "/health") {
          resp = http_response(200, "application/json",
                              std::string("{\"ok\":true,\"version\":\"") + kEngineVersion + "\"}");
        } else if (method == "GET" && pp.path == "/stats") {
          auto t = lib.telemetry(); auto counts = lib.state_counts();
          std::ostringstream o;
          o << "{\"total_searches\":" << t.total_searches << ",\"total_fetches\":" << t.total_fetches
            << ",\"overall_conversion\":" << t.overall_conversion << ",\"by_state\":{";
          bool first = true;
          for (const auto& [k, v] : counts) { o << (first ? "" : ",") << "\"" << k << "\":" << v; first = false; }
          o << "}}";
          resp = http_response(200, "application/json", o.str());
        } else if (method == "GET" && pp.path == "/graveyard") {
          auto cands = lib.graveyard_candidates();
          std::ostringstream o; o << "[";
          for (size_t i = 0; i < cands.size(); ++i) {
            o << (i ? "," : "") << "{\"skill_id\":\"" << json_escape(cands[i].skill_id)
              << "\",\"search_count\":" << cands[i].search_count
              << ",\"fetch_count\":" << cands[i].fetch_count << "}";
          }
          o << "]";
          resp = http_response(200, "application/json", o.str());
        } else if (method == "GET" && pp.path == "/search") {
          auto qit = pp.query.find("q");
          if (qit == pp.query.end()) {
            resp = http_response(400, "application/json", "{\"ok\":false,\"error\":\"missing q\"}");
          } else {
            auto mit = pp.query.find("mode");
            SearchMode smode = mit == pp.query.end() ? SearchMode::Hybrid
                                                     : search_mode_from_string(mit->second);
            auto hits = lib.search(qit->second, 8, false, smode);
            std::ostringstream o; o << "[";
            for (size_t i = 0; i < hits.size(); ++i) {
              o << (i ? "," : "") << state_json(SkillRow{0, hits[i].skill_id, hits[i].description, "",
                                                        hits[i].path, "", "", hits[i].state, 0, 0, 0,
                                                        "", "", "", ""});
            }
            o << "]";
            resp = http_response(200, "application/json", o.str());
          }
        } else if (method == "GET" && pp.path == "/fetch") {
          auto qit = pp.query.find("id");
          if (qit == pp.query.end()) {
            resp = http_response(404, "application/json", "{\"ok\":false,\"error\":\"missing id\"}");
          } else {
            try {
              std::string body = lib.fetch_body(qit->second);
              resp = http_response(200, "text/plain; charset=utf-8", body);
            } catch (const DbError&) {
              resp = http_response(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
            }
          }
        } else {
          resp = http_response(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
        }
      }
    } catch (const std::exception& e) {
      resp = http_response(400, "application/json",
                           std::string("{\"ok\":false,\"error\":\"") + json_escape(e.what()) + "\"}");
    }
    size_t sent = 0;
    while (sent < resp.size()) {
      int chunk = static_cast<int>(
          std::min(resp.size() - sent, static_cast<size_t>((std::numeric_limits<int>::max)())));
      auto n = ::send(c, resp.data() + sent, chunk, 0);
      if (n <= 0) break;
      sent += static_cast<size_t>(n);
    }
    CLOSESOCK(c);
  }
  CLOSESOCK(srv);
  platform_net_cleanup();
  std::cerr << "skillrouter API stopped\n";
  return 0;
}

void usage() {
  std::cerr <<
      "skillrouter - interface + router for a skill library of any size\n"
      "  skillrouter            (no args)         live interactive shell + event dashboard\n"
      "  skillrouter register <SKILL.md path>   [--db PATH]\n"
      "  skillrouter index    <root dir>         [--db PATH]\n"
      "  skillrouter search   \"<query>\"          [--db PATH] [--top N] [--json] [--mode hybrid|exact|fts|fuzzy]\n"
      "  skillrouter fetch    <skill_id>         [--db PATH] [--context \"<q>\"]\n"
      "  skillrouter use      <skill_id>         [--db PATH] [--context \"<q>\"]\n"
      "  skillrouter stats    [--db PATH]\n"
      "  skillrouter graveyard [--db PATH] [--min-searches N]\n"
      "  skillrouter deprecate <skill_id> [--db PATH]\n"
      "  skillrouter archive   <skill_id> [--db PATH]\n"
      "  skillrouter shell    [--db PATH]\n"
      "  skillrouter serve    [--db PATH] [--port 8090]\n"
      "  skillrouter mcp      [--db PATH]                   MCP server (JSON-RPC over stdio)\n";
}

}  // namespace

int main(int argc, char** argv) {
  // No subcommand: drop straight into the live interactive shell (dashboard +
  // event stream) against the default DB. `skillrouter --help`/`-h` still
  // prints the subcommand usage for scripting.
  if (argc < 2) {
    try {
      Args a;
      return cmd_shell(a);
    } catch (const std::exception& e) {
      std::cerr << "{\"ok\":false,\"error\":\"" << json_escape(e.what()) << "\"}\n";
      return 1;
    }
  }
  std::string cmd = argv[1];
  if (cmd == "--help" || cmd == "-h" || cmd == "help") { usage(); return 0; }
  try {
    Args a = parse(argc, argv, 2);
    if (cmd == "register") return cmd_register(a);
    if (cmd == "index") return cmd_index(a);
    if (cmd == "search") return cmd_search(a);
    if (cmd == "fetch") return cmd_fetch(a);
    if (cmd == "use") return cmd_use(a);
    if (cmd == "stats") return cmd_stats(a);
    if (cmd == "graveyard") return cmd_graveyard(a);
    if (cmd == "deprecate") return cmd_deprecate(a);
    if (cmd == "archive") return cmd_archive(a);
    if (cmd == "shell") return cmd_shell(a);
    if (cmd == "serve") return cmd_serve(a);
    if (cmd == "mcp") return cmd_mcp(a);
    usage();
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "{\"ok\":false,\"error\":\"" << json_escape(e.what()) << "\"}\n";
    return 1;
  }
}
