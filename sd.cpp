// sd - the next zoxide. named directory shortcuts for your shell.
//
// Two commands, one binary, split by which name you invoke it as:
//
//   sd      move around. jumping, and anything that only reads the config
//           except the visit log, which it appends to on purpose.
//   sdcfg   change things. add, remove, rename, import, maintenance.
//           never changes directory.
//
// State lives in ~/.simpledir:
//   config.json   hand-editable alias -> path map, version 2
//   history.json  machine-written frecency counts, not yours to edit
//
// The `print` verb writes a directory to stdout; the shell wrapper that
// `sdcfg init` emits performs the cd, because a subprocess cannot change
// the calling shell's cwd.
//
// Single file, no dependencies beyond libstdc++. Linux. Build with:
//   g++ -std=c++17 -O2 -static-libstdc++ -static-libgcc -o sd sd.cpp
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/utsname.h>

namespace fs = std::filesystem;

#ifndef VERSION
#define VERSION "6.3.0"
#endif
#define CONFIG_VERSION 2

namespace {

// frecency, in one place. a directory you visited today counts as much as one
// you visited twice last month, and something you visited a year ago fades to
// nothing. half-life of three days, computed lazily at read time, so nothing
// has to run in the background to keep the scores honest.
constexpr double HALF_LIFE = 3 * 24 * 3600;
// a visit within this many seconds of the last one doesn't count again, so
// holding down a key in a shell doesn't rewrite the file fifty times
constexpr int VISIT_THROTTLE = 60;
// stop tracking a directory once it fades below this, and keep at most this many
constexpr double FLOOR = 0.02;
constexpr size_t MAX_HISTORY = 500;

const std::string MOVE = "sd";
const std::string MOVE_ID = "sd";
const std::string legacy_ID = "simpledir";
const std::string CONFIG = "sdcfg";

// which half this invocation is: decided by the name it was called as
std::string g_prog = MOVE;
std::string g_mode = MOVE;
std::string g_owner = "noxthedevwindev-greatest";
std::string g_repo = g_owner + "/simpledir";
// the releases *list*, not the repository object. this url had no /releases on
// the end of it, so every check fetched the repo json, found no tag_name in it,
// and reported "couldn't reach GitHub" — which is what it said, while being
// perfectly able to reach it.
std::string g_api =
    "https://api.github.com/repos/noxthedevwindev-greatest/simpledir/releases";
std::string g_releases = "https://github.com/noxthedevwindev-greatest/simpledir/releases";

// the asset name carries the architecture, and it has to match what `make assets`
// and install.sh publish: sd-linux-x86_64 / sd-linux-arm64. a plain `sd` 404s on
// every release, which is why `update` never worked against the real thing.
std::string arch_tag() {
  struct utsname uts;
  if (uname(&uts) != 0) return "x86_64";
  std::string machine = uts.machine;
  if (machine == "aarch64" || machine == "arm64") return "arm64";
  if (machine == "x86_64" || machine == "amd64") return "x86_64";
  return machine;
}

std::string asset_name() { return "sd-linux-" + arch_tag(); }
std::string g_config_dir;
std::string g_config_file;
std::string g_history_file;

struct UserError : std::runtime_error {
  explicit UserError(const std::string& what) : std::runtime_error(what) {}
};

// a mistake in how it was called, not a problem with the request. exit 2, which
// is what argparse did and what every other cli does for usage errors.
struct UsageError : UserError {
  explicit UsageError(const std::string& what) : UserError(what) {}
};

[[noreturn]] void die(const std::string& msg) {
  std::cerr << g_prog << ": " << msg << "\n";
  std::exit(1);
}

double now_seconds() {
  using namespace std::chrono;
  // whole seconds: frecency doesn't need sub-second precision, and it keeps
  // history.json readable instead of full of 1.79109e+09
  return std::floor(duration<double>(system_clock::now().time_since_epoch()).count());
}

std::string stamp() {
  std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d%H%M%S", std::localtime(&t));
  return buf;
}

// ------------------------------------------------------------------ strings

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

bool contains_ci(const std::string& hay, const std::string& needle) {
  return contains(lower(hay), lower(needle));
}

bool starts_with(const std::string& s, const std::string& p) {
  return s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream in(s);
  while (std::getline(in, cur, sep)) out.push_back(cur);
  return out;
}

std::vector<std::string> shell_split(const std::string& s) {
  // good enough for paths and history lines; quotes and backslashes handled,
  // not a full shell parser and not pretending to be one
  std::vector<std::string> out;
  std::string cur;
  bool have = false, squote = false, dquote = false, esc = false;
  for (char c : s) {
    if (esc) { cur += c; have = true; esc = false; continue; }
    if (c == '\\' && !squote) { esc = true; continue; }
    if (c == '\'' && !dquote) { squote = !squote; have = true; continue; }
    if (c == '"' && !squote) { dquote = !dquote; have = true; continue; }
    if (std::isspace(static_cast<unsigned char>(c)) && !squote && !dquote) {
      if (have) { out.push_back(cur); cur.clear(); have = false; }
      continue;
    }
    cur += c;
    have = true;
  }
  if (have) out.push_back(cur);
  return out;
}

std::string shell_quote(const std::string& v) {
  bool needs = v.empty();
  for (char c : v) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || std::strchr("@%+=:,./-_", c))) {
      needs = true;
      break;
    }
  }
  if (!needs) return v;
  std::string out = "'";
  for (char c : v) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  return out + "'";
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) out += sep;
    out += parts[i];
  }
  return out;
}

// ----------------------------------------------------------------- filesystem

std::string home_dir() {
  if (const char* h = std::getenv("HOME")) return h;
  if (const passwd* pw = getpwuid(getuid())) return pw->pw_dir;
  return ".";
}

std::string env_or(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

// `~` and $VARS expanded, no symlink resolution
std::string expand(const std::string& raw) {
  std::string out;
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i] == '$' && i + 1 < raw.size()) {
      size_t j = i + 1;
      bool braced = raw[j] == '{';
      if (braced) j++;
      size_t start = j;
      while (j < raw.size() && (std::isalnum(static_cast<unsigned char>(raw[j])) || raw[j] == '_')) j++;
      if (j > start) {
        std::string name = raw.substr(start, j - start);
        if (const char* v = std::getenv(name.c_str())) out += v;
        i = j + (braced && j < raw.size() && raw[j] == '}' ? 1 : 0);
        continue;
      }
    }
    out += raw[i];
  }
  if (out == "~") return home_dir();
  if (starts_with(out, "~/")) return home_dir() + out.substr(1);
  return out;
}

// absolute and normalised, symlinks left alone
std::string abspath(const std::string& raw) {
  std::string path = expand(raw);
  std::error_code ec;
  fs::path p = fs::path(path).lexically_normal();
  if (p.is_absolute()) return p.string();
  fs::path cwd = fs::current_path(ec);
  if (ec) return p.string();
  return (cwd / p).string();
}

// absolute with symlinks resolved: what `add` stores by default
std::string realpath(const std::string& raw) {
  std::string path = expand(raw);
  std::error_code ec;
  fs::path resolved = fs::weakly_canonical(fs::path(path), ec);
  if (ec) return abspath(path);
  return resolved.string();
}

// What an alias means, as stored: absolute, symlinks left alone. `add` resolves
// symlinks unless asked not to, but reading an alias must never rewrite it --
// --keep-symlinks would be a lie otherwise.
std::string as_stored(const std::string& raw) { return abspath(raw); }

bool is_dir(const std::string& path) {
  std::error_code ec;
  return fs::is_directory(path, ec);
}

bool path_exists(const std::string& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw UserError("can't read " + path + ": " + std::strerror(errno));
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string basename_of(const std::string& path) {
  return fs::path(path).filename().string();
}

// never a partial file on disk, and never a mode that loses the exec bit
void write_atomic(const std::string& path, const std::string& data, bool executable = false) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::string tmp = path + ".new." + std::to_string(static_cast<long>(getpid()));
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw UserError("can't write " + tmp + ": " + std::strerror(errno));
    out << data;
    out.flush();
    if (!out) throw UserError("can't write " + tmp);
  }
  // owner_all includes the execute bit, which is how every file this tool writes
  // ended up 0744 and config.json came out executable. be explicit about which
  // bits are wanted instead of reaching for a convenient bundle.
  fs::perms mode = fs::perms::owner_read | fs::perms::owner_write;
  if (executable)
    mode |= fs::perms::owner_exec | fs::perms::group_read | fs::perms::group_exec |
           fs::perms::others_read | fs::perms::others_exec;
  else
    mode |= fs::perms::group_read | fs::perms::others_read;
  fs::permissions(tmp, mode, fs::perm_options::replace, ec);
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp);
    throw UserError("can't replace " + path + ": " + ec.message());
  }
}

// --------------------------------------------------------------------- json

struct Json;
using JsonPtr = std::shared_ptr<Json>;

struct Json {
  enum class Kind { Null, Bool, Num, Str, Arr, Obj } kind = Kind::Null;
  bool boolean = false;
  double num = 0;
  std::string str;
  std::vector<JsonPtr> arr;
  std::map<std::string, JsonPtr> obj;

  static JsonPtr make_obj() { auto j = std::make_shared<Json>(); j->kind = Kind::Obj; return j; }
  static JsonPtr make_arr() { auto j = std::make_shared<Json>(); j->kind = Kind::Arr; return j; }
  static JsonPtr make_str(const std::string& s) { auto j = std::make_shared<Json>(); j->kind = Kind::Str; j->str = s; return j; }
  static JsonPtr make_num(double n) { auto j = std::make_shared<Json>(); j->kind = Kind::Num; j->num = n; return j; }
  static JsonPtr make_bool(bool b) { auto j = std::make_shared<Json>(); j->kind = Kind::Bool; j->boolean = b; return j; }

  bool is_obj() const { return kind == Kind::Obj; }
  bool is_arr() const { return kind == Kind::Arr; }
  bool is_num() const { return kind == Kind::Num; }
  bool is_str() const { return kind == Kind::Str; }

  JsonPtr get(const std::string& key) const {
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : it->second;
  }
  void set(const std::string& key, JsonPtr value) { obj[key] = std::move(value); }
  std::string as_str(const std::string& fallback = "") const { return kind == Kind::Str ? str : fallback; }
  double as_num(double fallback = 0) const { return kind == Kind::Num ? num : fallback; }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : s_(text) {}

  JsonPtr parse() {
    skip();
    JsonPtr v = value();
    return v;
  }

 private:
  const std::string& s_;
  size_t i_ = 0;

  [[noreturn]] void fail(const std::string& why) const {
    throw UserError(std::to_string(i_) + ": " + why);
  }
  void skip() {
    while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) i_++;
  }
  char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

  JsonPtr value() {
    skip();
    switch (peek()) {
      case '{': return object();
      case '[': return array();
      case '"': return Json::make_str(string());
      case 't': expect("true"); return Json::make_bool(true);
      case 'f': expect("false"); return Json::make_bool(false);
      case 'n': expect("null"); return std::make_shared<Json>();
      default: return number();
    }
  }

  void expect(const char* word) {
    size_t n = std::strlen(word);
    if (s_.compare(i_, n, word) != 0) fail(std::string("expected ") + word);
    i_ += n;
  }

  JsonPtr object() {
    auto node = Json::make_obj();
    i_++;  // {
    skip();
    if (peek() == '}') { i_++; return node; }
    while (true) {
      skip();
      if (peek() != '"') fail("expected a key");
      std::string key = string();
      skip();
      if (peek() != ':') fail("expected :");
      i_++;
      node->obj[key] = value();
      skip();
      if (peek() == ',') { i_++; continue; }
      if (peek() == '}') { i_++; return node; }
      fail("expected , or }");
    }
  }

  JsonPtr array() {
    auto node = Json::make_arr();
    i_++;  // [
    skip();
    if (peek() == ']') { i_++; return node; }
    while (true) {
      node->arr.push_back(value());
      skip();
      if (peek() == ',') { i_++; continue; }
      if (peek() == ']') { i_++; return node; }
      fail("expected , or ]");
    }
  }

  std::string string() {
    std::string out;
    i_++;  // opening quote
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return out;
      if (c != '\\') { out += c; continue; }
      if (i_ >= s_.size()) break;
      char esc = s_[i_++];
      switch (esc) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          if (i_ + 4 > s_.size()) fail("bad \\u escape");
          unsigned code = std::stoul(s_.substr(i_, 4), nullptr, 16);
          i_ += 4;
          // surrogate pair, the boring half of it
          if (code >= 0xD800 && code <= 0xDBFF && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
            unsigned low = std::stoul(s_.substr(i_ + 2, 4), nullptr, 16);
            if (low >= 0xDC00 && low <= 0xDFFF) {
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
              i_ += 6;
            }
          }
          if (code < 0x80) out += static_cast<char>(code);
          else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
          } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
          } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
          }
          break;
        }
        default: out += esc;
      }
    }
    fail("unterminated string");
  }

  JsonPtr number() {
    size_t start = i_;
    if (peek() == '-' || peek() == '+') i_++;
    while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
                              s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+')) {
      i_++;
    }
    if (start == i_) fail("expected a value");
    try {
      return Json::make_num(std::stod(s_.substr(start, i_ - start)));
    } catch (const std::exception&) {
      fail("bad number");
    }
  }
};

JsonPtr json_parse(const std::string& text) {
  return JsonParser(text).parse();
}

std::string json_escape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

// keys come out sorted because std::map is: config.json stays diff-friendly
std::string json_dump(const JsonPtr& node, int indent = 2, int depth = 0) {
  const std::string pad(static_cast<size_t>(indent * depth), ' ');
  const std::string pad_in(static_cast<size_t>(indent * (depth + 1)), ' ');
  const char* nl = indent > 0 ? "\n" : "";
  switch (node->kind) {
    case Json::Kind::Null: return "null";
    case Json::Kind::Bool: return node->boolean ? "true" : "false";
    case Json::Kind::Num: {
      double v = node->num;
      char buf[40];
      // whole numbers print as integers, so `"version": 2` stays `2` and the
      // config file doesn't churn every time python handed us a float
      if (v == std::floor(v) && std::fabs(v) < 1e15) {
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
      } else {
        std::snprintf(buf, sizeof buf, "%g", v);
      }
      return buf;
    }
    case Json::Kind::Str: return "\"" + json_escape(node->str) + "\"";
    case Json::Kind::Arr: {
      if (node->arr.empty()) return "[]";
      std::string out = "[" + std::string(nl);
      for (size_t i = 0; i < node->arr.size(); i++) {
        out += pad_in + json_dump(node->arr[i], indent, depth + 1);
        if (i + 1 < node->arr.size()) out += ",";
        out += nl;
      }
      return out + pad + "]";
    }
    case Json::Kind::Obj: {
      if (node->obj.empty()) return "{}";
      std::string out = "{" + std::string(nl);
      size_t i = 0;
      for (const auto& [key, value] : node->obj) {
        out += pad_in + "\"" + json_escape(key) + "\": ";
        if (indent > 0) out += "";
        out += json_dump(value, indent, depth + 1);
        if (++i < node->obj.size()) out += ",";
        out += nl;
      }
      return out + pad + "}";
    }
  }
  return "null";
}

}  // namespace

// ------------------------------------------------------------------------ config

namespace {

struct Config {
  int version = 1;
  bool history = true;
  std::map<std::string, std::string> aliases;
};

JsonPtr config_json(const Config& cfg) {
  auto node = Json::make_obj();
  node->set("version", Json::make_num(cfg.version));
  node->set("history", Json::make_bool(cfg.history));
  auto aliases = Json::make_obj();
  for (const auto& [name, path] : cfg.aliases) aliases->set(name, Json::make_str(path));
  node->set("aliases", aliases);
  return node;
}

Config load_config() {
  Config cfg;
  if (!path_exists(g_config_file)) return cfg;  // absent is empty, not an error

  std::string raw = read_file(g_config_file);
  JsonPtr root;
  try {
    root = json_parse(raw);
  } catch (const UserError& err) {
    throw UserError(g_config_file + " is not valid JSON (" + err.what() + ").\n"
                                  "  fix it by hand, or nuke it: rm " + g_config_dir);
  }
  if (!root || !root->is_obj())
    throw UserError(g_config_file + " is missing an \"aliases\" object.\n"
                                  "  expected: {\"version\": 1, \"aliases\": {\"name\": \"/path\"}}");

  if (JsonPtr version = root->get("version")) {
    if (!version->is_num())
      throw UserError(g_config_file + " has a non-numeric version: " + version->as_str());
    cfg.version = static_cast<int>(version->as_num());
  }
  if (JsonPtr history = root->get("history")) {
    if (history->kind == Json::Kind::Bool) cfg.history = history->boolean;
    else if (history->is_num()) cfg.history = history->as_num() != 0;
  }

  JsonPtr aliases = root->get("aliases");
  if (!aliases || !aliases->is_obj())
    throw UserError(g_config_file + " is missing an \"aliases\" object.\n"
                                  "  expected: {\"version\": 1, \"aliases\": {\"name\": \"/path\"}}");
  for (const auto& [name, value] : aliases->obj) {
    if (value->is_str()) cfg.aliases[name] = value->str;
  }
  return cfg;
}

void save_config(const Config& cfg) {
  write_atomic(g_config_file, json_dump(config_json(cfg)) + "\n");
}

// ------------------------------------------------------------------- frecency

struct Visit {
  double n = 0;
  double t = 0;
};

std::map<std::string, Visit> read_history() {
  std::map<std::string, Visit> out;
  if (!path_exists(g_history_file)) return out;
  JsonPtr root;
  try {
    root = json_parse(read_file(g_history_file));
  } catch (const UserError&) {
    return out;  // a corrupt log is not worth a cd
  }
  if (!root || !root->is_obj()) return out;
  JsonPtr dirs = root->get("dirs");
  if (!dirs || !dirs->is_obj()) return out;
  for (const auto& [path, entry] : dirs->obj) {
    if (!entry->is_obj()) continue;
    Visit v;
    if (JsonPtr n = entry->get("n")) v.n = n->as_num();
    if (JsonPtr t = entry->get("t")) v.t = t->as_num();
    out[path] = v;
  }
  return out;
}

double decayed(const Visit& v, double now) {
  double age = std::max(0.0, now - v.t);
  return v.n * std::pow(0.5, age / HALF_LIFE);
}

bool history_enabled(const Config& cfg) {
  if (std::getenv("SIMPLEDIR_NO_HISTORY")) return false;
  return cfg.history;
}

// Note that we went there. Throttled, prunable, and never fatal: remembering
// is a convenience, not the function you called.
void record_visit(const std::string& path, double now) {
  if (std::getenv("SIMPLEDIR_NO_HISTORY")) return;
  try {
    std::map<std::string, Visit> dirs = read_history();
    auto it = dirs.find(path);
    if (it != dirs.end() && now - it->second.t < VISIT_THROTTLE) return;  // just recorded

    Visit v;
    v.n = (it == dirs.end() ? 0.0 : it->second.n) + 1.0;
    v.t = now;
    dirs[path] = v;

    // keep the file small: forget what has faded, then cap it
    std::map<std::string, Visit> alive;
    for (const auto& [p, entry] : dirs) {
      if (decayed(entry, now) >= FLOOR) alive[p] = entry;
    }
    if (alive.size() > MAX_HISTORY) {
      std::vector<std::pair<std::string, Visit>> ranked(alive.begin(), alive.end());
      std::stable_sort(ranked.begin(), ranked.end(),
                       [now](const auto& a, const auto& b) {
                         return decayed(a.second, now) > decayed(b.second, now);
                       });
      alive.clear();
      for (size_t i = 0; i < MAX_HISTORY && i < ranked.size(); i++) alive[ranked[i].first] = ranked[i].second;
    }

    auto root = Json::make_obj();
    root->set("version", Json::make_num(1));
    auto dirs_node = Json::make_obj();
    for (const auto& [p, entry] : alive) {
      auto v = Json::make_obj();
      v->set("n", Json::make_num(entry.n));
      v->set("t", Json::make_num(entry.t));
      dirs_node->set(p, v);
    }
    root->set("dirs", dirs_node);
    write_atomic(g_history_file, json_dump(root) + "\n");
  } catch (const std::exception&) {
    // a read-only home, a full disk, whatever: the cd still happens
  }
}

struct Hit {
  double score;
  std::string path;
};

// Directories you've been that match `word`, best first. Named directories are
// excluded: if you gave it a name, `sd name` is the way to reach it, and a
// count would only muddy that.
std::vector<Hit> frecency_ranked(const std::string& word, const Config& cfg, double now) {
  std::set<std::string> bound;
  for (const auto& [name, path] : cfg.aliases) bound.insert(as_stored(path));
  std::string needle = lower(word);

  std::vector<Hit> out;
  for (const auto& [path, entry] : read_history()) {
    if (bound.count(path)) continue;
    if (!needle.empty() && !contains_ci(path, needle)) continue;
    double score = decayed(entry, now);
    if (score >= FLOOR) out.push_back({score, path});
  }
  std::sort(out.begin(), out.end(), [](const Hit& a, const Hit& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.path < b.path;
  });
  return out;
}

// --------------------------------------------------------------- suggestions

// ratio*100, difflib's SequenceMatcher on the cheap approximation of it. good
// enough to say "did you mean", which is all we use it for.
int similarity(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return 0;
  size_t common = 0;
  for (char c : a) {
    if (b.find(c) != std::string::npos) common++;
  }
  return static_cast<int>(200.0 * common / (a.size() + b.size()));
}

std::vector<std::string> suggestions_for(const std::string& alias, const Config& cfg) {
  std::vector<std::string> hits;
  for (const auto& [name, path] : cfg.aliases) {
    (void)path;
    if (contains_ci(name, alias)) {
      if (std::find(hits.begin(), hits.end(), name) == hits.end()) hits.push_back(name);
    }
  }
  std::vector<std::string> close;
  for (const auto& [name, path] : cfg.aliases) {
    (void)path;
    if (similarity(lower(alias), lower(name)) >= 60 &&
        std::find(hits.begin(), hits.end(), name) == hits.end()) {
      close.push_back(name);
    }
  }
  std::sort(close.begin(), close.end());
  for (const auto& name : close) {
    if (hits.size() >= 3) break;
    hits.push_back(name);
  }
  if (hits.size() > 3) hits.resize(3);
  return hits;
}

std::string unknown_alias_error(const std::string& alias, const Config& cfg) {
  std::string msg = "no alias named '" + alias + "'";
  std::vector<std::string> hits = suggestions_for(alias, cfg);
  if (!hits.empty()) msg += "\n  did you mean: " + join(hits, ", ");
  if (cfg.aliases.empty()) {
    msg += "\n  your config is empty. add one: " + CONFIG + " add <name> [path]";
  } else {
    msg += "\n  see them all: " + MOVE + " ls";
  }
  return msg;
}

// ------------------------------------------------------------------ resolving

// Turn `name` or `name/sub/dir` into an existing absolute directory. Suffixes
// cost nothing to support and turn a flat map into something you can navigate
// deeply: `sd dots/src` is `<dots>/src`.
std::string resolve(const std::string& spec, const Config& cfg) {
  std::string name = spec;
  std::string suffix;
  size_t slash = spec.find('/');
  if (slash != std::string::npos) {
    name = spec.substr(0, slash);
    suffix = spec.substr(slash + 1);
  }
  // an absolute path is never an alias. say so plainly rather than letting the
  // frecency log substring-match it and jump somewhere surprising.
  if (name.empty() || starts_with(spec, "/"))
    throw UserError("'" + spec + "' isn't an alias, it's a path\n"
                    "  bind it first: " + CONFIG + " add <name> " + spec);

  std::string key = name;
  if (!cfg.aliases.count(name)) {
    // zoxide-style: a unique prefix is good enough. `sd hy` finds `hypr`, but
    // `sd h` with two candidates still fails and says so.
    std::vector<std::string> matches;
    for (const auto& [candidate, path] : cfg.aliases) {
      (void)path;
      if (starts_with(candidate, name)) matches.push_back(candidate);
    }
    std::sort(matches.begin(), matches.end());
    if (matches.size() == 1) {
      key = matches[0];
    } else if (matches.size() > 1) {
      throw UserError("'" + name + "' matches several aliases: " + join(matches, " ") +
                      "\n  use the whole name, or " + MOVE + " ls");
    } else {
      throw UserError(unknown_alias_error(name, cfg));
    }
  }

  std::string target = as_stored(cfg.aliases.at(key));
  if (!suffix.empty()) {
    std::error_code ec;
    fs::path joined = fs::path(target) / suffix;
    target = joined.lexically_normal().string();
  }

  if (!is_dir(target)) {
    std::string where = suffix.empty() ? "'" + key + "'" : "'" + spec + "'";
    throw UserError("alias " + where + " resolves to " + target +
                    "\n  that directory is gone. fix it: " + CONFIG + " add --force " +
                    key + " <new-path>");
  }
  return target;
}

// Where does `sd <word>` go, having noted that we went there? Named alias
// first, then a directory you have actually been to. This is the one function
// on the hot path, so frecency is consulted and recorded here and nowhere else.
std::string jump(const std::string& word, Config& cfg, double now) {
  if (starts_with(word, "/") || starts_with(word, "~"))
    throw UserError("'" + word + "' isn't an alias, it's a path\n"
                    "  bind it first: " + CONFIG + " add <name> " + word);
  std::string target;
  try {
    target = resolve(word, cfg);
  } catch (const UserError&) {
    for (const Hit& hit : frecency_ranked(word, cfg, now)) {
      if (is_dir(hit.path)) {
        target = hit.path;
        break;
      }
    }
    if (target.empty()) throw;  // rethrow the real error
  }
  if (history_enabled(cfg)) record_visit(target, now);
  return target;
}

// (name, absolute target, missing) sorted by name, optionally filtered
std::vector<std::tuple<std::string, std::string, bool>> rows(const Config& cfg,
                                                             const std::string& query = "") {
  std::vector<std::tuple<std::string, std::string, bool>> out;
  for (const auto& [name, path] : cfg.aliases) {
    std::string target = as_stored(path);
    if (!query.empty() && !contains_ci(name, query) && !contains_ci(target, query)) continue;
    out.emplace_back(name, target, !is_dir(target));
  }
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
  return out;
}


// --------------------------------------------------------------- arguments

// long flags only: no short aliases, so nothing to misremember
struct Args {
  std::vector<std::string> words;
  // value-form flags hold their argument; plain ones hold "true"
  std::map<std::string, std::string> flags;

  // true when the flag is present and not literally "false"
  bool has(const std::string& name) const {
    auto it = flags.find(name);
    if (it == flags.end()) return false;
    return it->second != "false" && it->second != "0" && it->second != "";
  }
  bool saw(const std::string& name) const { return flags.count(name) > 0; }
  std::string value(const std::string& name, const std::string& fallback = "") const {
    auto it = flags.find(name);
    return it == flags.end() ? fallback : it->second;
  }
  std::string word(size_t i, const std::string& fallback = "") const {
    return i < words.size() ? words[i] : fallback;
  }
};

// flags that consume the next token as their value
const std::set<std::string>& value_flags() {
  static const std::set<std::string> flags = {"depth", "prefix", "top", "to", "path"};
  return flags;
}

Args parse_args(const std::vector<std::string>& argv) {
  Args out;
  bool no_more_flags = false;
  for (size_t i = 0; i < argv.size(); i++) {
    const std::string& arg = argv[i];
    if (!no_more_flags && arg == "--") {
      no_more_flags = true;
      continue;
    }
    if (!no_more_flags && starts_with(arg, "--")) {
      std::string name = arg.substr(2);
      if (value_flags().count(name)) {
        size_t eq = name.find('=');
        if (eq != std::string::npos) {
          out.flags[name.substr(0, eq)] = argv[i].substr(eq + 1);
          continue;
        }
        out.flags[name] = (i + 1 < argv.size()) ? argv[++i] : "";
        continue;
      }
      bool value = true;
      size_t eq = name.find('=');
      if (eq != std::string::npos) {
        value = name.substr(eq + 1) != "false" && name.substr(eq + 1) != "0";
        name = name.substr(0, eq);
      }
      out.flags[name] = value;
      continue;
    }
    if (!no_more_flags && arg.size() > 1 && arg[0] == '-') {
      throw UsageError("unknown flag '" + arg + "'. all flags here are long form, e.g. --force");
    }
    out.words.push_back(arg);
  }
  return out;
}

// nudge() is defined with the other maintenance commands further down; it and
// releases() both need pieces that aren't declared yet at this point in the file
struct Release;
std::vector<Release> releases(int limit = 10);
void nudge();

// ------------------------------------------------------------------ move half

int cmd_ls(const Args& args) {
  Config cfg = load_config();
  std::string query = args.word(0);
  auto found = rows(cfg, query);

  if (args.has("names")) {
    for (const auto& [name, target, missing] : found) {
      (void)target;
      (void)missing;
      std::cout << name << "\n";
    }
    return found.empty() ? 1 : 0;
  }

  if (args.has("json")) {
    auto node = Json::make_obj();
    node->set("version", Json::make_num(1));
    auto aliases = Json::make_obj();
    for (const auto& [name, target, missing] : found) {
      (void)missing;
      aliases->set(name, Json::make_str(target));
    }
    node->set("aliases", aliases);
    std::cout << json_dump(node) << "\n";
    return found.empty() ? 1 : 0;
  }

  if (found.empty()) {
    if (!cfg.aliases.empty() && !query.empty()) {
      std::cerr << MOVE << ": nothing matches '" << query << "'. see them all: " << MOVE << " ls\n";
    } else {
      std::cerr << MOVE << ": no aliases yet. add one: " << CONFIG << " add\n";
    }
    return 1;
  }

  size_t width = 0;
  for (const auto& [name, target, missing] : found) {
    (void)target;
    (void)missing;
    width = std::max(width, name.size());
  }
  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  for (const auto& [name, target, missing] : found) {
    std::string shown = target;
    if (!args.has("long")) {
      std::string relative = fs::path(target).lexically_relative(cwd).string();
      // a path full of ../ is noise, so keep the absolute one
      if (!relative.empty() && !starts_with(relative, "..")) shown = relative;
    }
    std::cout << name << std::string(width - name.size() + 2, ' ') << shown
              << (missing ? "   [missing]" : "") << "\n";
  }
  nudge();
  return 0;
}

int cmd_top(const Args& args) {
  Config cfg = load_config();
  double now = now_seconds();
  std::string query = args.word(0);
  std::set<std::string> bound;
  for (const auto& [name, path] : cfg.aliases) {
    (void)name;
    bound.insert(as_stored(path));
  }

  struct Row {
    double score;
    std::string path;
    bool named;
    bool exists;
  };
  std::vector<Row> ranked;
  for (const auto& [path, entry] : read_history()) {
    if (!query.empty() && !contains_ci(path, query)) continue;
    double score = decayed(entry, now);
    if (score < FLOOR) continue;
    ranked.push_back({score, path, bound.count(path) > 0, is_dir(path)});
  }
  std::sort(ranked.begin(), ranked.end(), [](const Row& a, const Row& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.path < b.path;
  });

  if (ranked.empty()) {
    std::cout << "nothing in your history yet. every directory you jump to gets remembered.\n"
              << "  turn it off with SIMPLEDIR_NO_HISTORY=1, or " << CONFIG << " forget\n";
    return 0;
  }

  if (args.has("json")) {
    auto list = Json::make_arr();
    for (const Row& row : ranked) {
      auto node = Json::make_obj();
      node->set("path", Json::make_str(row.path));
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.3f", row.score);
      node->set("score", Json::make_num(std::stod(buf)));
      node->set("named", Json::make_bool(row.named));
      node->set("exists", Json::make_bool(row.exists));
      list->arr.push_back(node);
    }
    std::cout << json_dump(list) << "\n";
    return 0;
  }

  size_t width = 0;
  for (const Row& row : ranked) width = std::max(width, row.path.size());
  std::cout << ranked.size() << " directories, most recent visits first:\n\n";
  bool any_gone = false;
  for (const Row& row : ranked) {
    std::vector<std::string> tags;
    if (!row.exists) { tags.push_back("gone"); any_gone = true; }
    else if (row.named) tags.push_back("named");
    char score[32];
    std::snprintf(score, sizeof score, "%6.2f", row.score);
    std::cout << "  " << score << "  " << row.path
              << std::string(width - row.path.size(), ' ');
    if (!tags.empty()) std::cout << "   [" << join(tags, ", ") << "]";
    std::cout << "\n";
  }
  if (any_gone) std::cout << "\n  clean those up: " << CONFIG << " forget --missing\n";
  return 0;
}

// Interactive picker. fzf when it's installed, a numbered list when not.
int cmd_pick(const Args& args) {
  Config cfg = load_config();
  auto found = rows(cfg, args.word(0));
  if (found.empty()) die("nothing to pick from. add one: " + CONFIG + " add");

  std::vector<std::string> names;
  for (const auto& [name, target, missing] : found) {
    (void)missing;
    names.push_back(name);
  }

  bool have_fzf = false;
  for (const std::string& dir : split(env_or("PATH", "/usr/bin:/bin"), ':')) {
    if (dir.empty()) continue;
    std::string candidate = dir + "/fzf";
    if (access(candidate.c_str(), X_OK) == 0) {
      have_fzf = true;
      break;
    }
  }

  if (have_fzf && !std::getenv("SIMPLEDIR_NO_FZF")) {
    // the list goes in on stdin, as a pipe, which is the only form fzf accepts:
    // `fzf one two` is "unknown option: one". fzf reads its list from stdin and
    // opens /dev/tty for the keyboard, so the TUI still works while we read the
    // answer off its stdout. `printf %s` rather than a heredoc or a temp file:
    // no escape processing, no litter, nothing to clean up if the user escapes.
    std::string feed;
    for (const auto& row : found) {
      feed += std::get<0>(row) + "\t" + std::get<1>(row) + "\n";
    }
    std::string cmd = "printf '%s' " + shell_quote(feed) +
                      " | fzf --delimiter='\\t' --with-nth=1,2 --prompt='sd> ' --height=40%";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) die("couldn't run fzf");
    char buf[4096];
    std::string chosen;
    while (fgets(buf, sizeof buf, pipe)) chosen += buf;
    int status = pclose(pipe);
    if (status != 0 || trim(chosen).empty()) return 1;  // escape, or nothing chosen
    std::string line = trim(chosen);
    size_t tab = line.find('\t');
    std::string name = tab == std::string::npos ? line : line.substr(0, tab);
    // fzf hands back whatever line it was given; only believe an alias we offered
    if (std::find(names.begin(), names.end(), name) == names.end())
      throw UserError("fzf returned '" + name + "', which isn't one of your aliases");
    std::cout << name << "\n";
    return 0;
  }

  std::string reason = std::getenv("SIMPLEDIR_NO_FZF") ? "fzf disabled (SIMPLEDIR_NO_FZF)"
                                                        : "no fzf installed";
  std::cout << reason << ", so: pick a number or type a name\n\n";
  size_t width = 0;
  for (const std::string& name : names) width = std::max(width, name.size());
  for (size_t i = 0; i < found.size(); i++) {
    std::string shown = std::get<1>(found[i]) + (std::get<2>(found[i]) ? "   [missing]" : "");
    char number[16];
    std::snprintf(number, sizeof number, "%3zu", i + 1);
    std::cout << "  " << number << "  " << names[i] << std::string(width - names[i].size(), ' ')
              << "  " << shown << "\n";
  }
  std::cout << "\n> " << std::flush;
  std::string answer;
  if (!std::getline(std::cin, answer)) return 1;
  answer = trim(answer);
  if (answer.empty()) return 1;

  if (answer.find_first_not_of("0123456789") == std::string::npos) {
    size_t index = std::stoul(answer);
    if (index < 1 || index > names.size())
      throw UserError("no entry " + answer + ". there are " + std::to_string(names.size()) + ".");
    std::cout << names[index - 1] << "\n";
    return 0;
  }
  if (std::find(names.begin(), names.end(), answer) == names.end())
    throw UserError(unknown_alias_error(answer, cfg));
  std::cout << answer << "\n";
  return 0;
}


// ---------------------------------------------------------------- config half

// A decent alias name for a directory: its own name, de-duplicated.
std::string name_for(const std::string& path, const std::set<std::string>& taken) {
  std::string base = basename_of(path);
  if (base.empty()) base = path;
  std::string clean;
  for (char c : base) {
    clean += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-') ? c : '-';
  }
  if (clean.empty()) clean = "dir";
  if (!taken.count(clean)) return clean;
  for (int n = 2;; n++) {
    std::string candidate = clean + std::to_string(n);
    if (!taken.count(candidate)) return candidate;
  }
}

void require_dir(const std::string& path) {
  if (!is_dir(path)) die("not a directory: " + path);
}

int cmd_add(const Args& args) {
  std::string raw = args.word(1);
  if (raw.empty()) raw = fs::current_path().string();
  bool keep = args.has("keep-symlinks");
  std::string target = keep ? abspath(raw) : realpath(raw);
  require_dir(target);

  Config cfg = load_config();
  std::string name = args.word(0);
  std::string derived = basename_of(target);
  if (name.empty()) {
    name = derived;
    if (name.empty())
      die("can't derive a name from " + target + ". usage: " + CONFIG + " add <name> [path]");
  }
  if (cfg.aliases.count(name) && !args.has("force")) {
    die("alias '" + name + "' already exists -> " + cfg.aliases.at(name) +
        "\n  overwrite: " + CONFIG + " add --force " + name + " " + target);
  }

  cfg.aliases[name] = target;
  save_config(cfg);
  std::cout << name << " -> " << target << "\n";
  nudge();
  if (!derived.empty() && name != derived)
    std::cout << "  rename it: " << CONFIG << " rename " << name << " <other-name>\n";
  if (keep && fs::is_symlink(target)) std::cout << "  kept the symlink: " << target << "\n";
  return 0;
}

int cmd_rm(const Args& args) {
  std::string name = args.word(0);
  if (name.empty()) throw UserError("which alias? usage: " + CONFIG + " rm <name>");
  Config cfg = load_config();
  auto it = cfg.aliases.find(name);
  if (it == cfg.aliases.end()) throw UserError(unknown_alias_error(name, cfg));
  std::cout << "removed " << name << " -> " << it->second << "\n";
  cfg.aliases.erase(it);
  save_config(cfg);
  return 0;
}

int cmd_rename(const Args& args) {
  std::string from = args.word(0), to = args.word(1);
  if (from.empty() || to.empty())
    throw UserError("rename needs two names: " + CONFIG + " rename <old> <new>");
  Config cfg = load_config();
  auto it = cfg.aliases.find(from);
  if (it == cfg.aliases.end()) throw UserError(unknown_alias_error(from, cfg));
  if (cfg.aliases.count(to) && !args.has("force")) {
    die("alias '" + to + "' already exists -> " + cfg.aliases.at(to) + "\n  overwrite: " + CONFIG +
        " rename --force " + from + " " + to);
  }
  std::string target = it->second;
  cfg.aliases.erase(it);
  cfg.aliases[to] = target;
  save_config(cfg);
  std::cout << from << " -> " << to << " (" << target << ")\n";
  return 0;
}

// Bind every subdirectory of a tree in one shot: "I have twelve projects and
// don't want to type this twelve times". Depth-limited, skips what is bound.
int cmd_import(const Args& args) {
  std::string root = realpath(args.word(0));
  if (root.empty()) throw UserError("which directory? usage: " + CONFIG + " import <dir>");
  require_dir(root);

  int depth = 1;
  if (args.saw("depth")) {
    try {
      depth = std::stoi(args.value("depth", "1"));
    } catch (const std::exception&) {
      throw UsageError("--depth wants a number");
    }
    if (depth < 1) throw UsageError("--depth is at least 1");
  }
  std::string prefix = args.value("prefix", "");
  bool hidden = args.has("hidden");
  bool force = args.has("force");
  bool dry = args.has("dry-run");
  bool keep = args.has("keep-symlinks");

  std::vector<std::pair<std::string, std::string>> found;  // name -> path
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       it != end && !ec; it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    fs::path rel = fs::relative(it->path(), root, ec);
    if (ec) break;
    int levels = static_cast<int>(std::distance(rel.begin(), rel.end()));
    if (levels > depth) continue;
    std::string base = it->path().filename().string();
    if (!hidden && !base.empty() && base[0] == '.') continue;
    found.emplace_back(prefix + base, it->path().string());
  }

  if (found.empty()) throw UserError("nothing to import under " + root);

  Config cfg = load_config();
  std::vector<std::string> bound, skipped;
  std::sort(found.begin(), found.end());
  for (const auto& [name, target] : found) {
    if (cfg.aliases.count(name) && !force) {
      skipped.push_back(name);
      continue;
    }
    cfg.aliases[name] = keep ? abspath(target) : realpath(target);
    bound.push_back(name);
  }
  if (!bound.empty() && !dry) save_config(cfg);

  std::cout << (dry ? "would bind " : "bound ") << bound.size() << ": " << join(bound, " ") << "\n";
  if (!skipped.empty()) {
    std::cout << "skipped " << skipped.size() << " already bound: " << join(skipped, " ") << "\n";
    if (!force) std::cout << "  overwrite them: " << CONFIG << " import --force " << root << "\n";
  }
  return 0;
}

// ------------------------------------------------------------ history mining

struct Candidate {
  std::string path;
  int hits;
};

// Mine your shell history for directories worth naming. Three formats, one
// shape out, and nothing that isn't an existing directory gets through.
std::vector<Candidate> history_candidates(int limit) {
  std::vector<std::string> files;
  if (const char* extra = std::getenv("SIMPLEDIR_HISTORY")) files.push_back(extra);
  for (const std::string& name : {".bash_history", ".zsh_history",
                                  ".local/share/fish/fish_history", ".config/fish/fish_history"}) {
    files.push_back(home_dir() + "/" + name);
  }

  const std::set<std::string> cd_words = {"cd", "pushd", "z"};
  const std::set<std::string> separators = {";",  "&&", "||", "|", "(", "{", "!", "then",
                                             "else", "do", "sudo", "command", "time",
                                             "nohup", "builtin"};
  std::map<std::string, int> counts;
  bool any_file = false;

  for (const std::string& file : files) {
    if (!path_exists(file)) continue;
    any_file = true;
    bool fish = contains(file, "fish_history");
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty()) continue;
      if (fish) {
        size_t at = line.find("cmd: ");
        line = at == std::string::npos ? "" : line.substr(at + 5);
      } else if (starts_with(line, ": ")) {
        size_t at = line.find(';');
        if (at == std::string::npos) continue;
        line = line.substr(at + 1);
      } else if (line.find(';') != std::string::npos && std::isdigit(static_cast<unsigned char>(line[0]))) {
        line = line.substr(0, line.find(';'));
      }
      if (line.empty()) continue;

      std::vector<std::string> words = shell_split(line);
      for (size_t i = 0; i < words.size(); i++) {
        if (!cd_words.count(words[i])) continue;
        if (i > 0 && !separators.count(words[i - 1])) continue;
        std::vector<std::string> rest;
        for (size_t j = i + 1; j < words.size(); j++) {
          if (!starts_with(words[j], "-")) rest.push_back(words[j]);
        }
        if (rest.empty() || rest[0] == "-") continue;
        std::string candidate = rest.size() > 1 && fs::path(rest.back()).is_absolute() ? rest.back()
                                                                                        : rest[0];
        // relative targets are resolved against $HOME: history doesn't record
        // which directory you were standing in when you typed them
        std::string resolved = fs::path(candidate).is_absolute() ? abspath(candidate)
                                                                 : abspath(home_dir() + "/" + candidate);
        if (!is_dir(resolved)) {
          std::string expanded = expand(candidate);
          if (starts_with(expanded, "~/")) resolved = abspath(expanded);
        }
        if (is_dir(resolved)) counts[resolved]++;
        break;
      }
    }
  }

  if (!any_file) {
    std::string looked;
    for (const std::string& name : {".bash_history", ".zsh_history", ".local/share/fish/fish_history",
                                    ".config/fish/fish_history"}) {
      looked += "\n  " + home_dir() + "/" + name;
    }
    die("no shell history found. looked for:" + looked +
        "\ntell me where yours is: SIMPLEDIR_HISTORY=/path/to/history " + MOVE + " suggest");
  }
  if (counts.empty())
    die("no `cd` targets in your history files. is your history enabled? (HISTFILE)");

  Config cfg = load_config();
  std::set<std::string> bound;
  for (const auto& [name, path] : cfg.aliases) {
    (void)name;
    bound.insert(as_stored(path));
  }

  std::vector<Candidate> out;
  for (const auto& [path, hits] : counts) {
    if (bound.count(path)) continue;
    out.push_back({path, hits});
  }
  std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
    if (a.hits != b.hits) return a.hits > b.hits;
    return a.path < b.path;
  });
  if (limit > 0 && static_cast<size_t>(limit) < out.size()) out.resize(limit);
  return out;
}

int cmd_suggest(const Args& args) {
  Config cfg = load_config();
  int limit = 10;
  if (args.saw("top")) limit = std::atoi(args.value("top", "10").c_str());
  auto candidates = history_candidates(limit);

  if (candidates.empty()) {
    std::cout << "every directory in your history is already bound (" << cfg.aliases.size()
              << " aliases)\n";
    return 0;
  }

  if (args.has("json")) {
    auto node = Json::make_obj();
    for (const Candidate& c : candidates) node->set(c.path, Json::make_num(c.hits));
    std::cout << json_dump(node) << "\n";
    return 0;
  }

  size_t width = 0;
  for (const Candidate& c : candidates) width = std::max(width, c.path.size());
  std::cout << candidates.size() << " new directories from your history, showing the top "
            << candidates.size() << ":\n\n";
  for (const Candidate& c : candidates) {
    char hits[16];
    std::snprintf(hits, sizeof hits, "%5d", c.hits);
    std::cout << "  " << hits << "x  " << c.path << std::string(width - c.path.size(), ' ') << "\n";
  }
  std::cout << "\nbind them all:\n";
  std::set<std::string> taken;
  for (const auto& [name, path] : cfg.aliases) taken.insert(name);
  for (const Candidate& c : candidates) {
    std::cout << "  " << CONFIG << " add " << name_for(c.path, taken) << " " << shell_quote(c.path)
              << "\n";
    taken.insert(name_for(c.path, taken));
  }
  return 0;
}

// `sd suggest`, but it actually binds.
int cmd_bind(const Args& args) {
  auto candidates = history_candidates(10);
  if (candidates.empty()) {
    std::cout << "nothing new in your history to bind\n";
    return 0;
  }
  if (args.has("dry-run")) {
    Config cfg = load_config();
    std::set<std::string> taken;
    for (const auto& [name, path] : cfg.aliases) taken.insert(name);
    std::vector<std::string> names;
    for (const Candidate& c : candidates) {
      names.push_back(name_for(c.path, taken));
      taken.insert(names.back());
    }
    std::cout << "would bind " << names.size() << ": " << join(names, " ") << "\n";
    return 0;
  }

  Config cfg = load_config();
  std::vector<std::pair<std::string, std::string>> made;
  for (const Candidate& c : candidates) {
    std::set<std::string> taken;
    for (const auto& [name, path] : cfg.aliases) {
      (void)path;
      taken.insert(name);
    }
    std::string name = name_for(c.path, taken);
    cfg.aliases[name] = c.path;
    made.emplace_back(name, c.path);
  }
  save_config(cfg);
  std::cout << "bound " << made.size() << ":\n";
  for (const auto& [name, path] : made) std::cout << "  " << name << " -> " << path << "\n";
  std::cout << "\nrename any of them: " << CONFIG << " rename <old> <new>\n";
  return 0;
}

// Drop things from the visit log. The log is ours, so wiping it is fine.
int cmd_forget(const Args& args) {
  auto dirs = read_history();
  if (dirs.empty()) {
    std::cout << "your visit log is already empty\n";
    return 0;
  }

  if (args.has("all")) {
    if (!args.has("yes") && isatty(STDIN_FILENO)) {
      std::cout << "  forget all " << dirs.size() << " remembered directories? [y/N] " << std::flush;
      std::string answer;
      if (!std::getline(std::cin, answer) || lower(trim(answer)) != "y") {
        std::cout << "  ok, left alone\n";
        return 0;
      }
    }
    std::error_code ec;
    fs::remove(g_history_file, ec);
    std::cout << "forgot all " << dirs.size() << " remembered directories\n";
    return 0;
  }

  if (args.saw("path")) {
    std::string target = args.value("path");
    if (!dirs.count(target)) die("nothing remembered about " + target + ". see them: " + MOVE + " top");
    auto root = Json::make_obj();
    root->set("version", Json::make_num(1));
    auto node = Json::make_obj();
    for (const auto& [path, visit] : dirs) {
      if (path == target) continue;
      auto v = Json::make_obj();
      v->set("n", Json::make_num(visit.n));
      v->set("t", Json::make_num(visit.t));
      node->set(path, v);
    }
    root->set("dirs", node);
    write_atomic(g_history_file, json_dump(root) + "\n");
    std::cout << "forgot " << target << "\n";
    return 0;
  }

  std::vector<std::string> gone;
  for (const auto& [path, visit] : dirs) {
    (void)visit;
    if (!is_dir(path)) gone.push_back(path);
  }
  if (gone.empty()) {
    std::cout << "nothing forgotten: every remembered directory still exists\n";
    return 0;
  }
  auto root = Json::make_obj();
  root->set("version", Json::make_num(1));
  auto node = Json::make_obj();
  for (const auto& [path, visit] : dirs) {
    if (std::find(gone.begin(), gone.end(), path) != gone.end()) continue;
    auto v = Json::make_obj();
    v->set("n", Json::make_num(visit.n));
    v->set("t", Json::make_num(visit.t));
    node->set(path, v);
  }
  root->set("dirs", node);
  write_atomic(g_history_file, json_dump(root) + "\n");
  std::cout << "forgot " << gone.size() << " directories that no longer exist:\n";
  for (const std::string& path : gone) std::cout << "  " << path << "\n";
  return 0;
}

// Migrate the config to the current version. No network, no data loss.
int cmd_migrate(const Args& args) {
  Config cfg = load_config();
  int found = cfg.version;
  if (found > CONFIG_VERSION) {
    die("your config is version " + std::to_string(found) + ", but this simpledir only knows version " +
        std::to_string(CONFIG_VERSION) + ".\n  update the program first: " + CONFIG + " update");
  }
  if (found == CONFIG_VERSION) {
    std::cout << "config is already version " << CONFIG_VERSION << ". nothing to do.\n";
    return 0;
  }

  if (args.has("dry-run")) {
    std::cout << "would migrate " << g_config_file << ": version " << found << " -> " << CONFIG_VERSION
              << "\n  " << cfg.aliases.size() << " aliases kept exactly as they are"
              << "\n  would add \"history\": true, and create " << g_history_file << "\n";
    return 0;
  }

  // back up first: a migration that loses somebody's aliases is unforgivable
  std::string backup = g_config_file + ".bak." + stamp();
  write_atomic(backup, read_file(g_config_file));

  cfg.version = CONFIG_VERSION;
  save_config(cfg);
  if (!path_exists(g_history_file)) {
    auto root = Json::make_obj();
    root->set("version", Json::make_num(1));
    root->set("dirs", Json::make_obj());
    write_atomic(g_history_file, json_dump(root) + "\n");
  }

  std::cout << "migrated " << g_config_file << ": version " << found << " -> " << CONFIG_VERSION << "\n"
            << "  " << cfg.aliases.size() << " aliases kept as they were\n"
            << "  backup: " << backup << "\n"
            << "  created " << g_history_file << " (empty)\n"
            << "  now `" << MOVE << " top` will remember where you go\n";
  return 0;
}



// ------------------------------------------------------------- from zoxide

// Read zoxide's own record of where you go, and turn it into aliases. Zoxide
// keeps a sqlite database of every directory it has seen; we read it with the
// sqlite3 CLI rather than linking a library we would otherwise not need.
std::string popen_capture(const std::string& cmd) {
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return "";
  std::string out;
  char buf[4096];
  while (fgets(buf, sizeof buf, pipe)) out += buf;
  pclose(pipe);
  return out;
}

int cmd_zoxide(const Args& args) {
  Config cfg = load_config();
  std::string db = env_or("ZO_DATA_DIR", home_dir() + "/.local/share/zoxide") + "/db.zdb";
  std::string legacy = home_dir() + "/.zoxide.db";

  if (!path_exists(db)) {
    if (path_exists(legacy)) {
      db = legacy;
    } else {
      die("couldn't find zoxide's database.\n  looked for:\n    " + db + "\n    " + legacy +
          "\n  it lives in $ZO_DATA_DIR if you moved it. point me at it: ZO_DATA_DIR=/path " +
          CONFIG + " zoxide");
    }
  }
  if (popen_capture("which sqlite3").empty()) {
    die("reading zoxide's database needs the sqlite3 command, which isn't installed.\n"
        "  on Arch: sudo pacman -S sqlite");
  }

  std::string sql = "SELECT path, rank FROM paths WHERE rank > 0 ORDER BY rank DESC LIMIT 200;";
  std::string out = popen_capture("sqlite3 " + shell_quote(db) + " " + shell_quote(sql) + " 2>/dev/null");
  if (trim(out).empty()) {
    die("zoxide's database had nothing in it, or it isn't a sqlite file.\n"
        "  if zoxide is very old it used a different format; try `zoxide import` in zoxide itself first");
  }

  struct Row {
    std::string path;
    double rank;
  };
  std::vector<Row> rows_in;
  for (const std::string& line : split(out, '\n')) {
    std::string trimmed = trim(line);
    if (trimmed.empty()) continue;
    size_t bar = trimmed.find('|');
    if (bar == std::string::npos) continue;
    std::string path = trimmed.substr(0, bar);
    double rank = std::atof(trimmed.substr(bar + 1).c_str());
    if (path.empty() || !is_dir(path)) continue;
    rows_in.push_back({path, rank});
  }
  if (rows_in.empty()) die("zoxide's database has no directories that still exist");

  int limit = 20;
  if (args.saw("top")) limit = std::atoi(args.value("top", "20").c_str());
  if (static_cast<size_t>(limit) < rows_in.size()) rows_in.resize(limit);

  std::set<std::string> bound;
  for (const auto& [name, path] : cfg.aliases) {
    (void)name;
    bound.insert(as_stored(path));
  }

  std::cout << "top " << rows_in.size() << " directories from zoxide's database:\n\n";
  size_t width = 0;
  for (const Row& row : rows_in) width = std::max(width, row.path.size());

  std::vector<std::pair<std::string, std::string>> made;
  std::set<std::string> taken;
  for (const auto& [name, path] : cfg.aliases) taken.insert(name);
  bool dry = args.has("dry-run");

  for (const Row& row : rows_in) {
    char rank[16];
    std::snprintf(rank, sizeof rank, "%6.1f", row.rank);
    std::string suggested = name_for(row.path, taken);
    bool already = bound.count(row.path) > 0;
    std::cout << "  " << rank << "  " << row.path << std::string(width - row.path.size(), ' ');
    if (already) std::cout << "   [already bound]";
    std::cout << "\n";
    if (!already && args.has("bind")) {
      cfg.aliases[suggested] = row.path;
      taken.insert(suggested);
      made.emplace_back(suggested, row.path);
    }
  }

  if (args.has("bind")) {
    if (dry) {
      std::cout << "\nwould bind " << made.size() << "\n";
    } else {
      save_config(cfg);
      std::cout << "\nimported " << made.size() << " from zoxide:\n";
      for (const auto& [name, path] : made) std::cout << "  " << name << " -> " << path << "\n";
      std::cout << "\nrename any you got wrong: " << CONFIG << " rename <old> <new>\n";
    }
  } else {
    std::cout << "\nbind them all:\n";
    for (const Row& row : rows_in) {
      if (bound.count(row.path)) continue;
      std::cout << "  " << CONFIG << " add " << name_for(row.path, taken) << " "
                << shell_quote(row.path) << "\n";
      taken.insert(name_for(row.path, taken));
    }
  }
  return 0;
}

// ------------------------------------------------------------- installation

std::string install_target() {
  if (const char* override = std::getenv("SIMPLEDIR_BIN")) return override;
  std::string standard = home_dir() + "/.local/bin/sd";
  if (path_exists(standard)) return standard;
  std::error_code ec;
  fs::path self = fs::read_symlink("/proc/self/exe", ec);
  if (!ec && path_exists(self.string())) return self.string();
  return standard;
}

std::string run_capture(const std::string& cmd, int* status) {
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    if (status) *status = -1;
    return "";
  }
  std::string out;
  char buf[4096];
  while (fgets(buf, sizeof buf, pipe)) out += buf;
  int code = pclose(pipe);
  if (status) *status = code;
  return out;
}

// TLS isn't in the C++ standard library, so the download goes through curl,
// which the installer already requires.
bool download(const std::string& url, const std::string& dest) {
  std::string cmd = "curl -fsSL --retry 2 --connect-timeout 15 " + shell_quote(url) + " -o " +
                    shell_quote(dest) + " 2>/dev/null";
  int status = 0;
  run_capture(cmd, &status);
  return status == 0 && path_exists(dest);
}

struct Release {
  std::string tag;
  std::string published;
  std::string name;
};

std::vector<Release> releases(int limit) {
  std::string url = env_or("SIMPLEDIR_UPDATE_URL", g_api);
  std::string body =
      run_capture("curl -fsSL --connect-timeout 15 " + shell_quote(url) + " 2>/dev/null", nullptr);
  if (trim(body).empty()) return {};

  // Parse it with the parser this file already has. The hand-rolled scan this
  // replaces looked for the next `}` after each `{` — and github's release
  // objects contain a literal `{?name,label}` inside the `upload_url` *string*,
  // so every object was cut short at 334 characters, long before `tag_name`.
  // The result was that releases(), and therefore update, update --check,
  // releases and the daily nudge, had never once parsed a real response.
  JsonPtr root;
  try {
    root = json_parse(body);
  } catch (const UserError&) {
    return {};
  }
  if (!root || !root->is_arr()) return {};

  std::vector<Release> out;
  for (const JsonPtr& entry : root->arr) {
    if (!entry || !entry->is_obj()) continue;
    JsonPtr tag = entry->get("tag_name");
    if (!tag) continue;
    Release r;
    r.tag = tag->as_str();
    if (r.tag.empty()) continue;
    if (JsonPtr published = entry->get("published_at"))
      r.published = published->as_str().substr(0, 10);
    if (JsonPtr name = entry->get("name")) r.name = name->as_str();
    out.push_back(r);
    if (static_cast<int>(out.size()) >= limit) break;
  }
  return out;
}

// Download a release, check it really is us and really is the version asked
// for, then swap it in. The binary it replaces is kept as `sd.previous`, which
// is what `revert` puts back.
// a tag has to look like a tag. `4.0.04` is a typo and would 404 with a message
// that doesn't say so, so say it here instead.
std::string normalize_tag(const std::string& raw) {
  std::string want = trim(raw);
  if (want.empty()) throw UsageError("which version? try " + CONFIG + " update --to v6.0.0");
  if (want[0] != 'v') want = "v" + want;
  std::string rest = want.substr(1);
  // vX.Y.Z, optionally with a -rc.N suffix. nothing else is a release we made.
  bool shaped = rest.size() >= 5 && std::isdigit(static_cast<unsigned char>(rest[0]));
  int dots = 0;
  for (size_t i = 0; i < rest.size() && shaped; i++) {
    char c = rest[i];
    if (c == '.') dots++;
    else if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != 'a' && c != 'b' &&
             c != 'c' && c != 'd' && c != 'e' && c != 'f' && c != 'g' && c != 'h' && c != 'i' &&
             c != 'j' && c != 'k' && c != 'l' && c != 'm' && c != 'n' && c != 'o' && c != 'p' &&
             c != 'q' && c != 'r' && c != 's' && c != 't' && c != 'u' && c != 'v' && c != 'w' &&
             c != 'x' && c != 'y' && c != 'z')
      shaped = false;
  }
  if (shaped && dots == 2) {
    // v4.0.04 is a typo, not a release. a leading zero in a component is never
    // something we publish, so say so here instead of 404ing on it later.
    size_t start = 0;
    for (int part = 0; part < 3; part++) {
      size_t stop = rest.find('.', start);
      std::string piece =
          rest.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
      size_t dash = piece.find('-');
      if (dash != std::string::npos) piece = piece.substr(0, dash);
      if (piece.size() > 1 && piece[0] == '0') shaped = false;
      if (stop == std::string::npos) break;
      start = stop + 1;
    }
  }
  if (!shaped || dots != 2)
    throw UsageError("'" + raw + "' isn't a version. releases look like v6.0.0 or v6.1.0-rc.1\n"
                     "  see what exists: " + CONFIG + " releases");
  return want;
}

// Download a release, check it really is us and really is the version asked for,
// then swap it in. The binary it replaces is kept as `sd.previous`, which is what
// `revert` puts back.
//
// One download, straight to a staging file beside the target: the old version
// fetched to /tmp to test it and then fetched the same bytes again, which is two
// chances to fail and twice the bandwidth for no reason.
int install_release(const std::string& tag, bool allow_older) {
  // SIMPLEDIR_RELEASE_URL is a *base*, exactly as install.sh treats it, so the
  // path below is the real one and a test can exercise it against a file:// tree
  // laid out like a release. overriding the finished URL instead meant no test
  // ever saw how it was built, which is how it went on pointing at an asset
  // name nobody publishes.
  std::string base = env_or("SIMPLEDIR_RELEASE_URL", g_releases);
  // The asset name has changed twice, so try every spelling we have ever used,
  // newest first: sd-linux-<arch> from v6.0.0, sd from v5.0.0, and simpledir from
  // v3.0.0. Without this `--to v5.0.0` 404s while the version sits right there.
  std::vector<std::string> urls;
  if (const char* override = std::getenv("SIMPLEDIR_UPDATE_ASSET_URL")) {
    urls.push_back(override);
  } else {
    std::string stem = tag == "latest" ? base + "/latest/download/"
                                       : base + "/download/" + tag + "/";
    urls.push_back(stem + asset_name());
    urls.push_back(stem + "sd");
    urls.push_back(stem + "simpledir");
  }

  std::string target = install_target();
  std::error_code ec;
  fs::create_directories(fs::path(target).parent_path(), ec);
  std::string staged = target + ".new." + std::to_string(static_cast<long>(getpid()));

  std::string url = urls.front();
  bool fetched = false;
  for (const std::string& candidate : urls) {
    if (download(candidate, staged)) {
      url = candidate;
      fetched = true;
      break;
    }
  }
  if (!fetched) {
    fs::remove(staged, ec);
    die("download failed for " + tag + ":\n  tried " + join(urls, "\n         ") +
        "\n  if that tag exists, it predates v3.0.0 and shipped no binary at all;\n"
        "  otherwise " + CONFIG + " releases lists what does");
  }

  // verify before we replace a working install
  fs::permissions(staged, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                fs::perms::others_read | fs::perms::others_exec,
                 fs::perm_options::replace);
  int status = 0;
  std::string reported = trim(run_capture(shell_quote(staged) + " --version 2>/dev/null", &status));
  if (status != 0) {
    fs::remove(staged, ec);
    die("the download isn't runnable: " + url + "\n"
        "  if that 404s, the release has no asset for " + arch_tag() + " yet");
  }
  // It has to identify as us before it replaces a working install — that's what
  // stops a 404 page or a stray file getting chmod +x'd into your PATH. Releases
  // before v5.0.0 called the command `simpledir`, so that name counts too:
  // rejecting it meant `--to v4.0.0` refused a download that was perfectly
  // correct, with a message that looked like corruption.
  const std::string legacy = "simpledir";
  bool is_sd = starts_with(reported, MOVE + " ");
  bool is_legacy = starts_with(reported, legacy + " ");
  if (!is_sd && !is_legacy) {
    fs::remove(staged, ec);
    die("the download isn't " + MOVE + ": " + reported + "\n  nothing was changed");
  }
  const std::string& name = is_sd ? MOVE_ID : legacy_ID;
  double got = std::atof(reported.c_str() + name.size() + 1);
  std::string got_version = trim(reported.substr(name.size() + 1));
  size_t sp = got_version.find(' ');
  if (sp != std::string::npos) got_version = got_version.substr(0, sp);
  if (!allow_older && got <= std::atof(VERSION)) {
    fs::remove(staged, ec);
    std::cout << "  that asset is v" << got_version << ", same as what you have."
              << " not changing anything\n";
    return 0;
  }

  if (path_exists(target)) {
    std::error_code copy_ec;
    fs::copy_file(target, target + ".previous", fs::copy_options::overwrite_existing, copy_ec);
  }
  fs::rename(staged, target, ec);
  if (ec) {
    fs::remove(staged);
    die("couldn't replace " + target + ": " + ec.message() + "\n  nothing was changed");
  }

  // Going back past v6.0.0 is a one-way door: the older program is the python
  // one, which has neither `revert` nor `update --to`, so it cannot undo this.
  // Say so while the user still has this version's installer to hand.
  if (is_legacy) {
    std::cout << "  note: v" << got_version << " calls itself `simpledir`, not `" << MOVE
              << "`. it predates the two-command split,\n"
              << "  so there is no `" << CONFIG << "` half in this version.\n";
  }
  if (got < 6.0) {
    std::cout << "  heads up: v" << got_version
              << " is the old python build. it has no `" << CONFIG
              << " revert` and no `update --to`,\n"
              << "  so it cannot bring you back here. to return to v6 or later:\n"
              << "    curl -fsSL https://raw.githubusercontent.com/" << g_repo
              << "/main/install.sh | bash\n";
  }
  return static_cast<int>(got * 1000);  // the new version, times 1000
}

int cmd_update(const Args& args) {
  // `--to` names the version, so it never needs the release list. asking for it
  // first meant an explicit `update --to v5.0.0` failed with "couldn't reach
  // GitHub" whenever the API was rate-limited or offline, even though it had
  // everything it needed to act.
  if (args.saw("to")) {
    std::string want = normalize_tag(args.value("to"));
    std::cout << CONFIG << " " << VERSION << " installed, installing " << want << " over "
              << install_target() << "\n";
    int installed = install_release(want, true);
    if (installed)
      std::cout << "  done. the previous one is at " << install_target() << ".previous\n";
    std::cout << "  new shell needed if " << MOVE << " gained subcommands\n";
    return 0;
  }

  auto found = releases(1);
  if (found.empty())
    die("couldn't reach GitHub to check for updates.\n"
        "  the unauthenticated api allows 60 requests an hour per address; wait, or\n"
        "  install a version directly: " + CONFIG + " update --to v6.0.0\n"
        "  releases are also listed at " + g_repo + "/releases");
  std::string latest = found.front().tag;
  std::cout << CONFIG << " " << VERSION << " installed, newest release is " << latest << "\n";

  double have = std::atof(VERSION);
  double newest = std::atof(latest.c_str() + latest.find_first_not_of("v"));
  if (newest <= have) {
    std::cout << "you're up to date\n";
    return 0;
  }
  if (args.has("check")) {
    std::cout << "update available: " << latest << " (exit 1 means 'there is one')\n";
    return 1;
  }

  std::string target = install_target();
  if (!args.has("yes")) {
    if (!isatty(STDIN_FILENO)) {
      std::cout << "  not a terminal, so not asking. install it with: " << CONFIG << " update --yes\n";
      return 1;
    }
    std::cout << "  install " << latest << " over " << target << "? [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer) || lower(trim(answer)) != "y") {
      std::cout << "  ok, leaving it alone\n";
      return 0;
    }
  }
  int installed = install_release(latest, false);
  if (installed) {
    std::cout << "updated to " << latest.substr(1) << " at " << target << "\n";
    std::cout << "  the old one is kept at " << target << ".previous. go back: " << CONFIG
              << " revert\n";
  }
  std::cout << "  new shell needed if " << MOVE << " gained subcommands\n";
  return 0;
}

// Put back whatever was installed before the last update.
int cmd_revert(const Args& args) {
  std::string target = install_target();
  std::string previous = target + ".previous";

  if (args.saw("to")) {
    std::string want = normalize_tag(args.value("to"));
    install_release(want, true);
    std::cout << "now running " << want << " at " << target << "\n";
    return 0;
  }

  if (!path_exists(previous)) {
    die("nothing to go back to. no " + previous + ".\n"
        "  install a specific version instead: " + CONFIG + " update --to v5.0.0");
  }
  int status = 0;
  std::string reported = trim(run_capture(shell_quote(previous) + " --version 2>/dev/null", &status));
  if (status != 0 || !starts_with(reported, MOVE + " ")) {
    die(previous + " isn't runnable. delete it, or use " + CONFIG + " update --to <version>");
  }

  // An sd.previous holding the version already installed means a previous revert
  // ran, or an install overwrote things. Saying "back to X" while replacing the
  // binary with the identical file reads like it worked and did nothing.
  std::string have = trim(run_capture(shell_quote(target) + " --version 2>/dev/null", nullptr));
  if (have == reported) {
    std::cout << "nothing to do: " << previous << " is the same version you're already on ("
              << reported << ")\n"
              << "  install a specific one instead: " << CONFIG << " update --to v6.0.0\n";
    return 0;
  }

  std::error_code ec;
  std::string swapped = target + ".swapping." + std::to_string(static_cast<long>(getpid()));
  if (path_exists(target)) fs::rename(target, swapped, ec);
  fs::copy_file(previous, target, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    fs::rename(swapped, target, ec);
    die("couldn't put the old version back: " + ec.message());
  }
  fs::permissions(target, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read |
                                    fs::perms::owner_exec,
                  fs::perm_options::replace);
  fs::remove(swapped, ec);
  std::cout << "back to " << reported << ", installed at " << target << "\n";
  std::cout << "  (it was " << VERSION << ". new shell needed if " << MOVE << " gained subcommands)\n";
  return 0;
}

int cmd_releases(const Args& args) {
  auto found = releases(15);
  if (found.empty()) die("couldn't reach GitHub to list releases. try again, or see " + g_repo);
  std::cout << "published releases:\n\n";
  for (const Release& r : found) {
    std::string mark = r.tag == "v" + std::string(VERSION) ? "  <- you are here" : "";
    std::cout << "  " << r.tag << "  " << r.published << std::string(9, ' ') << r.name << mark
              << "\n";
  }
  return 0;
}

// ------------------------------------------------------------------- updates

std::string g_update_cache;

std::string read_update_cache() {
  if (g_update_cache.empty()) {
    g_update_cache = g_config_dir + "/update-check.json";
  }
  if (!path_exists(g_update_cache)) return "";
  JsonPtr root;
  try {
    root = json_parse(read_file(g_update_cache));
  } catch (const UserError&) {
    return "";
  }
  if (!root || !root->is_obj()) return "";
  if (JsonPtr latest = root->get("latest")) return latest->as_str();
  return "";
}

void write_update_cache(const std::string& latest) {
  auto root = Json::make_obj();
  root->set("checked", Json::make_num(now_seconds()));
  root->set("latest", Json::make_str(latest));
  try {
    write_atomic(g_config_dir + "/update-check.json", json_dump(root) + "\n");
  } catch (const std::exception&) {
    // a read-only home shouldn't break `ls`
  }
}

// One stderr line if a newer release exists. At most one network call a day,
// only on a terminal, and silent if anything at all goes wrong. Never on the
// jump path.
void nudge() {
  if (std::getenv("SIMPLEDIR_NO_UPDATE_CHECK")) return;
  if (!isatty(STDERR_FILENO)) return;

  std::string latest = read_update_cache();
  double checked = 0;
  if (path_exists(g_update_cache)) {
    try {
      JsonPtr root = json_parse(read_file(g_update_cache));
      if (root && root->is_obj() && root->get("checked")) checked = root->get("checked")->as_num();
    } catch (const UserError&) {
    }
  }
  if (latest.empty() || now_seconds() - checked > 86400) {
    auto found = releases(1);
    if (found.empty()) return;  // offline, rate limited, api changed: say nothing
    latest = found.front().tag;
    write_update_cache(latest);
  }
  if (latest.empty()) return;
  std::string bare = latest.substr(latest.find_first_not_of("v"));
  if (std::atof(bare.c_str()) > std::atof(VERSION)) {
    std::cerr << MOVE << ": v" << bare << " is out (you're on v" << VERSION << "). `" << CONFIG
              << " update` installs it.\n";
  }
}



// ------------------------------------------------------------------ wrapper

const char* MARK_BEGIN = "# >>> simpledir >>>";
const char* MARK_END = "# <<< simpledir <<<";

std::vector<std::string> rc_candidates() {
  std::vector<std::string> out;
  if (const char* only = std::getenv("SIMPLEDIR_RC")) {
    out.push_back(only);
    return out;  // exclusive: naming a file means naming that file
  }
  std::string shell = env_or("SHELL", "");
  if (contains(shell, "zsh")) out.push_back(env_or("ZDOTDIR", home_dir()) + "/.zshrc");
  for (const std::string& name : {"/.bashrc", "/.zshrc", "/.config/fish/config.fish"}) {
    out.push_back(home_dir() + name);
  }
  std::vector<std::string> unique;
  for (const std::string& p : out) {
    if (std::find(unique.begin(), unique.end(), p) == unique.end()) unique.push_back(p);
  }
  return unique;
}

int cmd_init() {
  std::error_code ec;
  fs::path self = fs::read_symlink("/proc/self/exe", ec);
  std::string bindir = ec ? fs::current_path().string() : self.parent_path().string();

  // This block has to survive two hostile readings, because they are what people
  // actually do:
  //
  //   eval "$(sdcfg init)"   fine for a multi-line block
  //   eval $(sdcfg init)     word splitting: the newlines vanish and every
  //                          unquoted `*` becomes a glob against $PWD
  //
  // So: every statement ends in `;` rather than only in a newline, and there is
  // not a single glob or `#` before the last construct. `[ "$x:0:1" = "/" ]`
  // instead of `case $x in /*)`, because a pattern is a glob. Readable in a
  // file, correct when pasted. Learned by watching `/*` expand to
  // "/bin /boot /dev /etc ..." and the shell die with a syntax error.
  std::cout << "if ! command -v " << MOVE << " >/dev/null 2>&1; then export PATH=\"" << bindir
            << ":$PATH\"; fi;\n"
            << "_sd_jump() {\n"
            << "  if [ -z \"${1-}\" ]; then builtin cd -- \"$HOME\" && return $?; fi;\n"
            << "  if [ \"${1-}\" = \"-\" ]; then builtin cd -- \"$OLDPWD\" && return $?; fi;\n"
            << "  if [ \"${1:0:1}\" = \"/\" ]; then builtin cd -- \"$1\" && return $?; fi;\n"
            << "  if [ \"${1:0:1}\" = \"~\" ]; then builtin cd -- \"${1/#\\~/$HOME}\" && return $?;"
               " fi;\n"
            << "  local _sd_dir;\n"
            << "  _sd_dir=$(command " << MOVE << " print \"$@\") || return $?;\n"
            << "  builtin cd -- \"$_sd_dir\";\n"
            << "};\n"
            << MOVE << "() {\n"
            << "  if [ \"${1-}\" = \"ls\" ] || [ \"${1-}\" = \"i\" ] || [ \"${1-}\" = \"print\" ]"
               " || [ \"${1-}\" = \"top\" ] || [ \"${1-}\" = \"suggest\" ] || "
               "([ \"${1:0:1}\" = \"-\" ] && [ \"${1-}\" != \"-\" ]); then\n"
            << "    command " << MOVE << " \"$@\";\n"
            << "    return $?;\n"
            << "  fi;\n"
            << "  _sd_jump \"$@\";\n"
            << "};\n"
            << "# simpledir - the next zoxide. a subprocess cannot change this shell's\n"
            << "# directory, so the `cd` lives in the function and `" << MOVE << " print` only\n"
            << "# resolves the name. `" << CONFIG << "` needs no wrapper: it changes things and\n"
            << "# never moves you. installed by `bash install.sh`, or by hand with\n"
            << "# eval \"$(" << CONFIG << " init)\".\n";
  return 0;
}

int cmd_completions(const std::string& shell) {
  if (shell == "bash") {
    std::cout << "# simpledir bash completion. install it with:\n"
              << "#   " << CONFIG << " completions bash > "
              << "/usr/share/bash-completion/completions/" << MOVE << "\n"
              << "_" << MOVE << "_complete() {\n"
              << "  local cur names\n"
              << "  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
              << "  names=$(command " << MOVE << " ls --names 2>/dev/null)\n"
              << "  COMPREPLY=( $(compgen -W \"$names\" -- \"$cur\") )\n"
              << "  if [[ ${#COMPREPLY[@]} -eq 0 && -d \"$cur\" ]]; then\n"
              << "    # let the shell complete directories too: `sd dots/<tab>`\n"
              << "    COMPREPLY=( $(compgen -d -- \"$cur\") )\n"
              << "  fi\n"
              << "}\n"
              << "complete -o filenames -F _" << MOVE << "_complete " << MOVE << "\n"
              << "\n"
              << "_" << CONFIG << "_complete() {\n"
              << "  local cur verbs=\"add rm rename import bind forget migrate zoxide edit init "
                 "completions update revert releases uninstall doctor\"\n"
              << "  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
              << "  COMPREPLY=( $(compgen -W \"$verbs\" -- \"$cur\") )\n"
              << "}\n"
              << "complete -F _" << CONFIG << "_complete " << CONFIG << "\n";
  } else {
    std::cout << "#compdef " << MOVE << " " << CONFIG << "\n"
              << "# simpledir zsh completion. requires compinit; put this in ~/.zshrc:\n"
              << "#   " << CONFIG << " completions zsh > \"${fpath[1]}/_" << MOVE << "\"\n"
              << "local -a _sd_aliases\n"
              << "_sd_aliases=(${(f)\"$(command " << MOVE << " ls --names 2>/dev/null)\"})\n"
              << "_arguments '1: :->alias' '*: :_files -/'\n"
              << "if [[ $state == alias ]]; then\n"
              << "  _describe -t aliases 'directory alias' _sd_aliases\n"
              << "fi\n"
              << "\n"
              << "_" << CONFIG << "() {\n"
              << "  local -a verbs=(add rm rename import bind forget migrate zoxide edit init "
                 "completions update revert releases uninstall doctor)\n"
              << "  _describe -t commands 'sdcfg command' verbs\n"
              << "}\n"
              << "compdef _" << CONFIG << " " << CONFIG << "\n";
  }
  return 0;
}

// Remove the install: the binary, and the wrapper block from shell rc. Your
// aliases are data, not installation, so they stay unless --purge.
int cmd_uninstall(const Args& args) {
  std::string target = install_target();
  Config cfg = load_config();

  // be explicit: we may be removing a different copy than the one running
  std::error_code self_ec;
  std::string running = fs::read_symlink("/proc/self/exe", self_ec);
  if (self_ec) running = g_prog;
  if (fs::weakly_canonical(target, self_ec) != fs::weakly_canonical(running, self_ec)) {
    std::cout << "note: removing " << target << ", not the copy you're running (" << running << ")\n";
  }

  if (!cfg.aliases.empty() && !args.has("purge")) {
    std::cout << "you have " << cfg.aliases.size() << " alias"
              << (cfg.aliases.size() == 1 ? "" : "es") << " in " << g_config_file << "\n"
              << "  these stay. to remove them too: " << CONFIG << " uninstall --purge\n";
  }

  if (!args.has("yes")) {
    if (!isatty(STDIN_FILENO)) die("not a terminal, so not asking. re-run with --yes");
    std::cout << "  remove " << target << " and the wrapper from your shell rc? [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer) || lower(trim(answer)) != "y") {
      std::cout << "  ok, nothing changed\n";
      return 0;
    }
  }

  std::vector<std::string> touched;
  for (const std::string& rc : rc_candidates()) {
    if (!path_exists(rc)) continue;
    std::string original = read_file(rc);
    if (original.find(MARK_BEGIN) == std::string::npos) continue;
    // drop the marked span, keep the rest byte for byte
    std::string cleaned;
    size_t i = 0;
    while (i < original.size()) {
      size_t begin = original.find(MARK_BEGIN, i);
      if (begin == std::string::npos) {
        cleaned += original.substr(i);
        break;
      }
      cleaned += original.substr(i, begin - i);
      size_t end = original.find(MARK_END, begin);
      if (end == std::string::npos) {
        i = original.size();
        break;
      }
      size_t stop = original.find('\n', end);
      i = stop == std::string::npos ? original.size() : stop + 1;
    }
    std::string backup = rc + ".bak." + stamp();
    write_atomic(backup, original);
    write_atomic(rc, cleaned);
    touched.push_back(rc + " (backup: " + backup + ")");
  }
  if (touched.empty()) std::cout << "no wrapper block found in any shell rc\n";

  std::error_code ec;
  if (path_exists(target)) {
    if (fs::remove(target, ec)) {
      std::cout << "removed " << target << "\n";
    } else {
      std::cout << "couldn't remove " << target << ": " << ec.message() << "\n"
                << "  remove it by hand: rm " << target << "\n";
    }
  } else {
    std::cout << "nothing to remove at " << target << "\n";
  }
  // sdcfg lives *next to* the sd we just removed, and nowhere else. a
  // hardcoded ~/.local/bin/sdcfg here ignores SIMPLEDIR_BIN entirely, so a test
  // run with a scoped SIMPLEDIR_BIN deleted the developer's real symlink — and
  // did so quietly, twice, before anybody worked out why it kept vanishing.
  fs::remove(fs::path(target).parent_path() / "sdcfg", ec);

  for (const std::string& rc : touched) std::cout << "cleaned the wrapper from " << rc << "\n";

  if (args.has("purge")) {
    fs::remove_all(g_config_dir, ec);
    std::cout << "deleted " << g_config_dir << " and every alias in it\n";
  } else {
    std::cout << "aliases kept in " << g_config_file << "\n";
  }
  std::cout << "open a new shell, or `exec bash`, to drop the old functions\n";
  return 0;
}

int cmd_doctor() {
  int problems = 0;
  auto ok = [](const std::string& m) { std::cout << "  \033[32mok\033[0m    " << m << "\n"; };
  auto bad = [&](const std::string& m) { problems++; std::cout << "  \033[31mproblem\033[0m " << m << "\n"; };
  auto note = [](const std::string& m) { std::cout << "  \033[2mnote\033[0m    " << m << "\n"; };

  std::cout << CONFIG << " doctor - version " << VERSION << "\n";
  std::cout << "runtime\n";
  ok(std::string("built with ") + __VERSION__);

  std::cout << "config\n";
  if (path_exists(g_config_file)) {
    ok(g_config_file);
    try {
      Config cfg = load_config();
      ok(std::to_string(cfg.aliases.size()) + " alias" + (cfg.aliases.size() == 1 ? "" : "es"));
      if (cfg.version < CONFIG_VERSION) {
        bad("config is version " + std::to_string(cfg.version) + ", this is version " +
            std::to_string(CONFIG_VERSION) + ". migrate it: " + CONFIG + " migrate");
      }
      for (const auto& [name, path] : cfg.aliases) {
        std::string target = as_stored(path);
        if (!is_dir(target))
          bad("'" + name + "' -> " + target + " is gone. rebind: " + CONFIG + " add --force " + name +
              " <path>");
        std::string trimmed = trim(name);
        if (trimmed != name || starts_with(name, "-") || name.find(' ') != std::string::npos)
          bad("alias name '" + name + "' has whitespace or a leading dash");
      }
    } catch (const UserError& err) {
      bad(std::string(err.what()).substr(0, std::string(err.what()).find('\n')));
    }
  } else {
    bad("no config yet at " + g_config_file + ". create one: " + CONFIG + " add");
  }

  std::cout << "shell\n";
  bool wired = false;
  for (const std::string& rc : rc_candidates()) {
    if (!path_exists(rc)) continue;
    if (read_file(rc).find(MARK_BEGIN) != std::string::npos) {
      ok("wrapper wired into " + rc);
      wired = true;
    }
  }
  if (!wired) {
    std::string shell = env_or("SHELL", "");
    bad("no shell wrapper found. add it: " + CONFIG + " init >> " +
        (contains(shell, "zsh") ? "~/.zshrc" : "~/.bashrc"));
  }

  std::string dir = fs::path(install_target()).parent_path().string();
  if (contains(":" + env_or("PATH", "") + ":", ":" + dir + ":")) {
    ok(dir + " is in PATH");
  } else {
    bad(dir + " is not in PATH. add: export PATH=\"" + dir + ":$PATH\"");
  }

  std::cout << "history\n";
  auto history = read_history();
  if (history.empty()) {
    note("empty. " + MOVE + " remembers where you go; " + CONFIG + " forget clears it");
  } else {
    ok(std::to_string(history.size()) + " directories remembered");
    ok("recording is on");
  }
  if (std::getenv("SIMPLEDIR_NO_HISTORY")) note("recording is off (SIMPLEDIR_NO_HISTORY)");

  std::cout << "\n";
  if (problems) {
    std::cout << problems << " problem" << (problems == 1 ? "" : "s") << " found\n";
    return 1;
  }
  std::cout << "everything looks fine\n";
  return 0;
}

// ------------------------------------------------------------------------ main

const char* MOVE_HELP =
    "the next zoxide. move between the directories you named.\n"
    "\n"
    "usage: sd <alias>              jump to a directory you bound\n"
    "       sd -                    previous directory\n"
    "       sd                      $HOME\n"
    "       sd /some/path           a raw path, cd'd as-is\n"
    "\n"
    "  sd ls [<query>] [--long]     list what you can jump to\n"
    "  sd ls --names                one alias per line, for scripts\n"
    "  sd ls --json                 the same shape as the config file\n"
    "  sd print <alias[/sub]>       print the directory, don't cd\n"
    "  sd top [<query>]             the frecency log, best first\n"
    "  sd suggest [<query>]         directories from your history worth naming\n"
    "  sd suggest --json            as JSON\n"
    "  sd i [<query>]               interactive picker (fzf if installed)\n"
    "\n"
    "  sd --help                    this text\n"
    "  sd --version                 print the version and exit\n"
    "\n"
    "  sd only reads your config. to change things, use sdcfg:\n"
    "    sdcfg add, rm, rename, import, bind, edit, doctor, ...\n"
    "\n"
    "  a unique name prefix is enough: `sd hy` finds `hypr`.\n"
    "  flags are long-form only.\n";

const char* CONFIG_HELP =
    "change your directory shortcuts. this command never moves you.\n"
    "\n"
    "usage: sdcfg <command> [arguments]\n"
    "\n"
    "  sdcfg add [<name>] [<path>]  bind a name; defaults to the dir's own name and $PWD\n"
    "  sdcfg rm <name>              unbind\n"
    "  sdcfg rename <old> <new>     rename, keeping the path\n"
    "  sdcfg import <dir>           bind every subdirectory of a tree at once\n"
    "  sdcfg bind                   name everything `sd suggest` found\n"
    "  sdcfg forget                 drop directories from the visit log\n"
    "  sdcfg migrate                migrate the config file to the current version\n"
    "  sdcfg zoxide                 import the directories zoxide knows about\n"
    "  sdcfg edit [<editor>]        open the config in $EDITOR\n"
    "  sdcfg init                   print the shell wrapper\n"
    "  sdcfg completions bash|zsh   print a completion script\n"
    "  sdcfg update                 check for a newer release and install it\n"
    "  sdcfg revert                 go back to the previously installed version\n"
    "  sdcfg releases               list the published releases\n"
    "  sdcfg uninstall              remove the binaries and the wrapper\n"
    "  sdcfg doctor                 check the install\n"
    "\n"
    "  sdcfg --help                 this text\n"
    "  sdcfg --version              print the version and exit\n"
    "\n"
    "  to move, use sd: sd <alias>, sd ls, sd i, ...\n"
    "  flags are long-form only: --force --dry-run --keep-symlinks --purge --yes\n"
    "\n"
    "  `migrate` moves your config file from version 1 to version 2. `update`\n"
    "  replaces the program itself. different things: neither calls the other.\n";

int run_move(const std::vector<std::string>& argv) {
  // fast path: `sd <word>` and `sd --version` are what run on every prompt
  if (argv.size() == 1) {
    if (argv[0] == "--version") {
      std::cout << MOVE << " " << VERSION << " - the next zoxide\n";
      return 0;
    }
    if (argv[0] == "--help") {
      std::cout << MOVE_HELP;
      return 0;
    }
    const std::set<std::string> verbs = {"ls", "print", "suggest", "i", "top"};
    if (!starts_with(argv[0], "-") && !verbs.count(argv[0])) {
      Config cfg = load_config();
      std::cout << jump(argv[0], cfg, now_seconds()) << "\n";
      return 0;
    }
  }
  if (argv.empty()) {
    std::cout << MOVE_HELP;
    return 0;
  }

  const std::string verb = argv[0];
  std::vector<std::string> rest(argv.begin() + 1, argv.end());

  if (verb == "ls") {
    Args args = parse_args(rest);
    for (const auto& flag : args.flags) {
      if (!std::set<std::string>({"names", "json", "long"}).count(flag.first))
        throw UserError("unknown flag '--" + flag.first + "' for `sd ls`. try --long, --names, --json");
    }
    return cmd_ls(args);
  }
  if (verb == "top") return cmd_top(parse_args(rest));
  if (verb == "i") return cmd_pick(parse_args(rest));
  if (verb == "suggest") {
    Args args = parse_args(rest);
    for (const auto& flag : args.flags) {
      if (!std::set<std::string>({"json", "top"}).count(flag.first))
        throw UserError("unknown flag '--" + flag.first + "' for `sd suggest`. try --top, --json");
    }
    return cmd_suggest(args);
  }
  if (verb == "print") {
    Args args = parse_args(rest);
    if (args.words.empty()) throw UserError("which alias? usage: sd print <alias>");
    Config cfg = load_config();
    std::cout << jump(args.words[0], cfg, now_seconds()) << "\n";
    return 0;
  }
  static const std::set<std::string> config_verbs = {
      "add", "rm", "rename", "import", "bind", "forget", "migrate", "zoxide",
      "edit", "init", "completions", "update", "revert", "releases", "uninstall", "doctor"};
  if (config_verbs.count(verb)) {
    throw UsageError("'" + verb + "' is not an " + MOVE + " command.\n  did you mean `" + CONFIG +
                      " " + verb + "`? " + MOVE + " only reads your config.");
  }
  throw UsageError("'" + verb + "' is not an " + MOVE + " command. try `" + MOVE + " --help`");
}

int run_config(const std::vector<std::string>& argv) {
  if (argv.empty()) {
    std::cout << CONFIG_HELP;
    return 0;
  }
  const std::string verb = argv[0];
  std::vector<std::string> rest(argv.begin() + 1, argv.end());
  Args args = parse_args(rest);

  static const std::set<std::string> known = {
      "add", "rm", "rename", "import", "bind", "forget", "migrate", "zoxide",
      "edit", "init", "completions", "update", "revert", "releases", "uninstall", "doctor"};

  if (verb == "--version") {
    std::cout << CONFIG << " " << VERSION << " - the next zoxide\n";
    return 0;
  }
  if (verb == "--help") {
    std::cout << CONFIG_HELP;
    return 0;
  }
  if (!known.count(verb)) {
    // reaching for the wrong half is the commonest mistake; help if we can
    static const std::set<std::string> move_verbs = {"ls", "print", "suggest", "i", "top"};
    if (move_verbs.count(verb)) {
      std::cerr << "did you mean `" << MOVE << " " << verb << "`? " << CONFIG
                << " only changes things.\n";
    }
    throw UsageError("'" + verb + "' is not an " + CONFIG + " command. try `" + CONFIG + " --help`");
  }

  if (verb == "add") return cmd_add(args);
  if (verb == "rm") return cmd_rm(args);
  if (verb == "rename") return cmd_rename(args);
  if (verb == "import") return cmd_import(args);
  if (verb == "bind") return cmd_bind(args);
  if (verb == "forget") return cmd_forget(args);
  if (verb == "migrate") return cmd_migrate(args);
  if (verb == "zoxide") return cmd_zoxide(args);
  if (verb == "update") return cmd_update(args);
  if (verb == "revert") return cmd_revert(args);
  if (verb == "releases") return cmd_releases(args);
  if (verb == "uninstall") return cmd_uninstall(args);
  if (verb == "doctor") return cmd_doctor();
  if (verb == "init") return cmd_init();
  if (verb == "edit") {
    if (!path_exists(g_config_file)) {
      Config empty;
      save_config(empty);
    }
    std::string editor = args.word(0, env_or("EDITOR", env_or("VISUAL", "vi")));
    std::string cmd = editor + " " + shell_quote(g_config_file);
    if (system(cmd.c_str()) != 0) die("editor exited with an error");
    return 0;
  }
  if (verb == "completions") {
    std::string shell = args.word(0);
    if (shell != "bash" && shell != "zsh") throw UsageError("completions takes bash or zsh");
    return cmd_completions(shell);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);

  // the mode is decided by the name it was invoked as
  std::string invoked = fs::path(argc > 0 ? argv[0] : MOVE).filename().string();
  if (invoked == CONFIG) {
    g_mode = CONFIG;
    g_prog = CONFIG;
  }

  g_config_dir = env_or("SIMPLEDIR_CONFIG_DIR", home_dir() + "/.simpledir");
  g_config_file = g_config_dir + "/config.json";
  g_history_file = g_config_dir + "/history.json";

  try {
    int status = g_mode == MOVE ? run_move(args) : run_config(args);
    std::cout.flush();
    return status;
  } catch (const UsageError& err) {
    std::cout.flush();
    std::cerr << g_prog << ": " << err.what() << "\n";
    return 2;
  } catch (const UserError& err) {
    std::cout.flush();
    std::cerr << g_prog << ": " << err.what() << "\n";
    return 1;
  } catch (const std::exception& err) {
    std::cout.flush();
    std::cerr << g_prog << ": " << err.what() << "\n";
    return 1;
  }
}
