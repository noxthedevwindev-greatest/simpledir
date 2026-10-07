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
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <map>
#include <set>
#include <tuple>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/utsname.h>

namespace fs = std::filesystem;

#ifndef VERSION
#define VERSION "11.0.0"
#endif

// The one line this release is about, shown by `--version`. A number on its own
// doesn't say what you installed. Its own guard, not inside the one above: the
// test suite builds stub binaries with -DVERSION, and a TAGLINE that only exists
// when VERSION does not would leave those stubs uncompilable.
#ifndef TAGLINE
#define TAGLINE "every download is checked against its published checksum"
#endif

#define CONFIG_VERSION 3

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

// A project carries its own aliases in this file, beside the project, so it can
// be committed and shared.
const std::string PROJECT_FILE = ".simpledir.json";

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

// ------------------------------------------------------------------- output
//
// iostreams cost more than they look. Including <iostream> drags in
// std::ios_base::Init, whose static constructor builds the locale machinery
// before main() even runs: measured at ~90us per invocation of a statically
// linked build, which is 12% of the whole cost of `sd name`. This program's job
// is to print a path and exit, and it runs on every single `cd`, so that is the
// last place to spend 90 microseconds.
//
// So: two buffers, write(2), done. The buffering matches what std::cout did --
// line buffered on a terminal so a long listing appears as it is produced, fully
// buffered into a pipe so `sd ls | head` is still one write -- and stderr stays
// unbuffered because that is how error messages have to behave when they land
// between two lines of something else.

class Out {
 public:
  explicit Out(int fd) : fd_(fd), tty_(isatty(fd) == 1) {}
  ~Out() { flush(); }
  Out(const Out&) = delete;
  Out& operator=(const Out&) = delete;

  Out& operator<<(const char* s) {
    if (s) put(s, std::strlen(s));
    return *this;
  }
  Out& operator<<(const std::string& s) {
    put(s.data(), s.size());
    return *this;
  }
  Out& operator<<(char c) {
    put(&c, 1);
    return *this;
  }
  // a bare bool would print as 1, and iostream printed true/false. Nothing here
  // streams one today, but a silent behaviour change is not worth the risk.
  Out& operator<<(bool b) {
    put(b ? "true" : "false", b ? 4 : 5);
    return *this;
  }
  template <class T>
  std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>,
                   Out>&
  operator<<(T value) {
    char buf[24];
    int n = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(value));
    if (n > 0) put(buf, static_cast<size_t>(n));
    return *this;
  }

  Out& flush() {
    const char* p = buf_.data();
    size_t left = buf_.size();
    while (left > 0) {
      ssize_t n = ::write(fd_, p, left);
      if (n <= 0) {
        if (n < 0 && errno == EINTR) continue;
        break;  // a closed pipe is not this program's problem to solve
      }
      p += n;
      left -= static_cast<size_t>(n);
    }
    buf_.clear();
    return *this;
  }

 private:
  int fd_;
  bool tty_;
  std::string buf_;

  void put(const char* p, size_t n) {
    buf_.append(p, n);
    if (!tty_ || buf_.size() >= 8192 ||
        (n == 1 && p[0] == '\n'))
      flush();
  }
};

Out out(1);
Out err(2);

// Every subcommand the move half has.
//
// One list, because the fast path has to recognise a verb before it decides a
// bare word is an alias name, and the dispatch further down has to agree with it.
// Those were two separate literals and that is exactly how `sd sha256` came to
// resolve an alias called "sha256" instead of complaining about its arguments --
// the same class of bug as v8.0.1, where `adapt` reached the dispatch but the
// shell wrapper never forwarded it. If you add a move verb, add it here.
const std::set<std::string>& move_verbs() {
  static const std::set<std::string> verbs = {"ls", "i", "print", "top", "suggest", "adapt", "sha256"};
  return verbs;
}

// What std::getline(std::cin, x) used to do, for the four places that ask a
// question. Nothing else in the program reads a whole line off stdin.
bool read_line(std::string& into) {
  into.clear();
  int c;
  bool any = false;
  while ((c = std::fgetc(stdin)) != EOF) {
    any = true;
    if (c == '\n') return true;
    into.push_back(static_cast<char>(c));
  }
  return any;
}
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
  err << g_prog << ": " << msg << "\n";
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

// $HOME first, always. The fallback exists for the handful of setups that unset
// it, and it matters more than it looks: the password-database lookup this used to
// use is the only thing in the program that reaches into NSS, and a statically
// linked binary cannot dlopen the NSS modules it needs -- so a static build that
// called it would break in exactly the environment it was meant to help. Reading
// /etc/passwd ourselves is twenty lines, touches no shared library, and is
// correct for every local user — which is everyone running this.
std::string home_dir() {
  if (const char* h = std::getenv("HOME")) {
    if (*h) return h;
  }
  std::ifstream passwd_file("/etc/passwd");
  std::string uid_text = std::to_string(getuid());
  std::string line;
  while (std::getline(passwd_file, line)) {
    std::vector<std::string> fields = split(line, ':');
    if (fields.size() < 6) continue;
    if (fields[2] != uid_text) continue;
    return fields[5];
  }
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
  // A tag is a named group of alias names, so `sd @work` means "the directories I
  // use for work" and `sd @work dots` jumps to one of them. Ordered, because the
  // order you added them in is the order you want to see them in.
  std::vector<std::pair<std::string, std::vector<std::string>>> tags;
};

const std::string TAG_PREFIX = "@";

// tags are stored as @name so a tag can never collide with an alias that happens
// to start with an @, and so `sd @work` reads the same in the config as on screen
std::string tag_key(const std::string& name) {
  return starts_with(name, TAG_PREFIX) ? name : TAG_PREFIX + name;
}

std::string tag_label(const std::string& key) {
  return starts_with(key, TAG_PREFIX) ? key.substr(1) : key;
}

const std::vector<std::string>* find_tag(const Config& cfg, const std::string& name) {
  std::string key = tag_key(name);
  for (const auto& [k, members] : cfg.tags)
    if (k == key) return &members;
  return nullptr;
}

JsonPtr config_json(const Config& cfg) {
  auto node = Json::make_obj();
  node->set("version", Json::make_num(cfg.version));
  node->set("history", Json::make_bool(cfg.history));
  auto aliases = Json::make_obj();
  for (const auto& [name, path] : cfg.aliases) aliases->set(name, Json::make_str(path));
  node->set("aliases", aliases);
  if (!cfg.tags.empty()) {
    auto tags = Json::make_obj();
    for (const auto& [key, members] : cfg.tags) {
      auto list = Json::make_arr();
      for (const std::string& member : members) list->arr.push_back(Json::make_str(member));
      tags->set(key, list);
    }
    node->set("tags", tags);
  }
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
  // "tags" is optional: a v1 or v2 config has none, and that is not an error
  if (JsonPtr tags = root->get("tags")) {
    if (tags->is_obj()) {
      for (const auto& [key, value] : tags->obj) {
        if (!value->is_arr()) continue;
        std::vector<std::string> members;
        for (const JsonPtr& item : value->arr)
          if (item && item->is_str()) members.push_back(item->str);
        if (!members.empty()) cfg.tags.emplace_back(key, members);
      }
    } else if (!tags->is_obj()) {
      // nothing to do: a malformed tag is worse than no tag, but refusing to load
      // the aliases would be worse still
    }
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

std::map<std::string, Visit> parse_history(const std::string& raw);

std::map<std::string, Visit> read_history() {
  if (!path_exists(g_history_file)) return {};
  return parse_history(read_file(g_history_file));
}

std::map<std::string, Visit> parse_history(const std::string& raw) {
  std::map<std::string, Visit> out;
  JsonPtr root;
  try {
    root = json_parse(raw);
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
// Find the timestamp recorded for one path without parsing the whole log.
//
// This runs on every single jump -- recording a visit is part of jumping -- and
// the log holds up to 500 entries. Building a json tree for all of them to look
// up one key was measurably the most expensive thing this program did: more than
// the fork, the exec and the config parse put together. The file is machine
// written in a known shape, so one substring search for the exact key finds it.
//
// Returns false when the scan comes up empty, which means either the directory
// genuinely isn't in the log or the file isn't shaped the way we write it. The
// caller then parses it properly rather than guessing: a hand-edited log has to
// keep working, and being wrong here costs a redundant write, never a wrong jump.
bool scanned_visit_time(const std::string& raw, const std::string& path, double& out_time) {
  std::string key = "\"" + json_escape(path) + "\":";
  size_t at = raw.find(key);
  if (at == std::string::npos) return false;
  // the timestamp is the "t" member of that entry; bound the search to this
  // entry so a hand-written file can't hand us the next directory's value
  size_t entry_end = raw.find("}", at);
  size_t t_at = raw.find("\"t\":", at);
  if (t_at == std::string::npos) return false;
  if (entry_end != std::string::npos && t_at > entry_end) return false;
  const char* digits = raw.c_str() + t_at + 4;
  char* stop = nullptr;
  double value = std::strtod(digits, &stop);
  if (stop == digits) return false;
  out_time = value;
  return true;
}

void record_visit(const std::string& path, double now) {
  if (std::getenv("SIMPLEDIR_NO_HISTORY")) return;
  try {
    // The throttle is checked first, on the raw text, because the answer is
    // almost always "just recorded" and a full parse to establish that would be
    // the largest cost in the program.
    std::string raw = path_exists(g_history_file) ? read_file(g_history_file) : "";
    if (!raw.empty()) {
      double last = 0;
      if (scanned_visit_time(raw, path, last)) {
        if (now - last < VISIT_THROTTLE) return;
      } else {
        std::map<std::string, Visit> scanned = parse_history(raw);
        auto found = scanned.find(path);
        if (found != scanned.end() && now - found->second.t < VISIT_THROTTLE) return;
      }
    }
    std::map<std::string, Visit> dirs = parse_history(raw);
    auto it = dirs.find(path);

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
struct Project {
  std::string dir;                            // absolute, where the file lives
  std::map<std::string, std::string> aliases; // name -> stored value
};

Project load_project(const std::string& dir) {
  Project out;
  out.dir = dir;
  std::string file = dir + "/" + PROJECT_FILE;
  // Absent is empty, not broken: the first `sdcfg project add` in a directory has
  // nothing to read. Reporting "not valid JSON" for a file that doesn't exist was
  // the first thing this command ever did.
  if (!path_exists(file)) return out;
  JsonPtr root;
  try {
    root = json_parse(read_file(file));
  } catch (const UserError&) {
    throw UserError(file + " is not valid JSON.\n  fix it by hand, or move it aside: mv " + file +
                    " " + file + ".bak");
  }
  if (!root || !root->is_obj()) throw UserError(file + " needs an \"aliases\" object");
  JsonPtr aliases = root->get("aliases");
  if (!aliases || !aliases->is_obj())
    throw UserError(file + " needs an \"aliases\" object, even if it is empty:\n"
                    "  {\"aliases\": {}}");
  for (const auto& [name, value] : aliases->obj) {
    if (value->is_str()) out.aliases[name] = value->str;
  }
  return out;
}

void save_project(const Project& project) {
  auto root = Json::make_obj();
  root->set("version", Json::make_num(1));
  auto aliases = Json::make_obj();
  for (const auto& [name, stored] : project.aliases) aliases->set(name, Json::make_str(stored));
  root->set("aliases", aliases);
  write_atomic(project.dir + "/" + PROJECT_FILE, json_dump(root) + "\n");
}

// project aliases are defined further down, with the commands that edit them
std::string find_project_dir();
struct Project;
Project load_project(const std::string& dir);
std::string resolve_project_path(const std::string& stored, const std::string& project_dir);

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

  // A project can hold its own aliases in a .simpledir.json beside it, and those
  // only exist while you are somewhere inside that tree. The project's own name
  // wins while you are inside it, the way a local .env or a direnv does, and the
  // global one applies everywhere else. Same name, two meanings, no configuration
  // to say so -- which is the entire reason this file exists.
  std::string project_dir = find_project_dir();
  std::map<std::string, std::string> project_aliases;
  if (!project_dir.empty()) {
    try {
      project_aliases = load_project(project_dir).aliases;
    } catch (const UserError&) {
      project_aliases.clear();  // a broken project file must not break every alias
    }
  }

  std::string key = name;
  std::string target;
  bool found = false;

  // 1. the project's own name, while you are inside it
  auto project_it = project_aliases.find(name);
  if (project_it != project_aliases.end()) {
    target = resolve_project_path(project_it->second, project_dir);
    found = true;
  }
  // 2. the global name
  if (!found && cfg.aliases.count(name)) {
    target = as_stored(cfg.aliases.at(name));
    found = true;
  }

  if (!found) {
    // zoxide-style: a unique prefix is good enough. `sd hy` finds `hypr`, but
    // `sd h` with two candidates still fails and says so. Each pool is tried on
    // its own, so a project alias is never made ambiguous by a global one.
    auto prefix_in = [&](const std::map<std::string, std::string>& pool,
                         std::vector<std::string>& hits) {
      for (const auto& [candidate, value] : pool) {
        (void)value;
        if (starts_with(candidate, name)) hits.push_back(candidate);
      }
      std::sort(hits.begin(), hits.end());
    };
    std::vector<std::string> matches;
    prefix_in(project_aliases, matches);
    if (matches.empty()) prefix_in(cfg.aliases, matches);
    if (matches.size() == 1) {
      key = matches[0];
      target = project_aliases.count(key) ? resolve_project_path(project_aliases.at(key), project_dir)
                                         : as_stored(cfg.aliases.at(key));
    } else if (matches.size() > 1) {
      throw UserError("'" + name + "' matches several aliases: " + join(matches, " ") +
                      "\n  use the whole name, or " + MOVE + " ls");
    } else {
      throw UserError(unknown_alias_error(name, cfg));
    }
  }

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
  static const std::set<std::string> flags = {"depth", "prefix", "top", "to", "path", "runs"};
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

// defined with the import commands further down; the proposals need it too
std::string name_for(const std::string& path, const std::set<std::string>& taken);
// defined with the config commands further down
void require_dir(const std::string& path);

// -------------------------------------------------------- export and import
//
// A config full of absolute paths is no use on another machine, and a config full
// of `~` is no use to anything that isn't your shell. So an exported config writes
// $HOME as `~`, which is the one convention every shell, every dotfile manager and
// every other tool already understands. Import expands it again.
//
// Nothing else is rewritten. A path outside $HOME stays absolute because there is
// nothing portable to say about it, and silently mangling it would be worse.

std::string portable(const std::string& absolute) {
  std::string home = home_dir();
  if (home.empty() || home == "/") return absolute;
  if (absolute == home) return "~";
  if (starts_with(absolute, home + "/")) return "~" + absolute.substr(home.size());
  return absolute;
}

int cmd_export(const Args& args) {
  Config cfg = load_config();
  auto node = config_json(cfg);
  // rewrite both the aliases and anything inside them
  auto aliases = node->get("aliases");
  if (aliases && aliases->is_obj()) {
    for (const auto& [name, value] : aliases->obj) {
      if (value->is_str()) value->str = portable(value->str);
    }
  }
  node->set("version", Json::make_num(CONFIG_VERSION));
  node->set("exported_by", Json::make_str(MOVE + " " + VERSION));
  // tags hold names, not paths, so they travel as they are
  std::string text = json_dump(node) + "\n";

  std::string asked_for = args.word(0);
  if (asked_for.empty() || asked_for == "-") {
    out << text;
    return 0;
  }
  std::string file = abspath(asked_for);
  write_atomic(file, text);
  size_t portable_count = 0;
  for (const auto& [name, path] : cfg.aliases)
    if (portable(as_stored(path)) != as_stored(path)) portable_count++;
  out << "wrote " << cfg.aliases.size() << " aliases to " << file << "\n";
  if (portable_count)
    out << "  " << portable_count << " under your home were written as ~/ so they travel\n";
  out << "  adopt it elsewhere with: " << CONFIG << " adopt " << shell_quote(file) << "\n";
  return 0;
}

int cmd_adopt(const Args& args) {
  std::string from = args.word(0);
  if (from.empty())
    throw UsageError("which file? " + CONFIG + " adopt <file>\n"
                     "  make one to import: " + CONFIG + " export <file>");
  std::string path = abspath(from);
  if (!path_exists(path)) die("no such file: " + path);

  JsonPtr root;
  try {
    root = json_parse(read_file(path));
  } catch (const UserError& err) {
    die(path + " is not valid JSON (" + err.what() + ")");
  }
  if (!root || !root->is_obj()) die(path + " needs to be a config object");
  JsonPtr aliases = root->get("aliases");
  if (!aliases || !aliases->is_obj()) die(path + " has no \"aliases\" object");

  Config cfg = load_config();
  std::vector<std::string> added, replaced, skipped, missing;
  for (const auto& [name, value] : aliases->obj) {
    if (!value->is_str()) {
      skipped.push_back(name);
      continue;
    }
    std::string target = as_stored(value->str);  // expands ~ and makes it absolute
    bool gone = !is_dir(target);
    if (gone) missing.push_back(name + " -> " + target);
    if (cfg.aliases.count(name)) {
      if (as_stored(cfg.aliases.at(name)) == target) {
        skipped.push_back(name);
        continue;
      }
      if (!args.has("force")) {
        replaced.push_back(name);
        continue;
      }
      cfg.aliases[name] = target;
      continue;
    }
    cfg.aliases[name] = target;
    added.push_back(name);
  }
  // tags travel with the aliases: a group of names is meaningless without them
  std::vector<std::string> tags_added;
  if (JsonPtr tags = root->get("tags")) {
    if (tags->is_obj()) {
      for (const auto& [key, value] : tags->obj) {
        if (!value->is_arr()) continue;
        std::vector<std::string> members;
        for (const JsonPtr& item : value->arr) {
          if (!item || !item->is_str()) continue;
          // only keep names that actually arrived, so a tag can't end up holding
          // aliases that aren't there
          if (cfg.aliases.count(item->str)) members.push_back(item->str);
        }
        if (members.empty()) continue;
        auto slot = std::find_if(cfg.tags.begin(), cfg.tags.end(),
                                 [&](const std::pair<std::string, std::vector<std::string>>& t) {
                                   return t.first == key;
                                 });
        if (slot == cfg.tags.end()) {
          cfg.tags.emplace_back(key, members);
          tags_added.push_back(key);
        } else {
          for (const std::string& m : members) {
            if (std::find(slot->second.begin(), slot->second.end(), m) == slot->second.end())
              slot->second.push_back(m);
          }
        }
      }
    }
  }

  if (!added.empty() || !args.has("dry-run")) {
    if (args.has("dry-run")) {
      out << "would add " << added.size() << ", replace " << replaced.size() << "\n";
    } else {
      save_config(cfg);
      out << "imported from " << path << "\n";
    }
  }
  if (!tags_added.empty()) out << "  tags " << join(tags_added, " ") << "\n";
  if (!added.empty()) out << "  added " << added.size() << ": " << join(added, " ") << "\n";
  if (!replaced.empty())
    out << "  " << replaced.size() << " already existed with a different path: "
              << join(replaced, " ")
              << "\n  overwrite: " << CONFIG << " import --force " << shell_quote(path) << "\n";
  if (!skipped.empty()) out << "  " << skipped.size() << " unchanged\n";
  if (!missing.empty()) {
    out << "  " << missing.size() << " point at directories that don't exist here:\n";
    for (const std::string& line : missing) out << "    " << line << "\n";
    out << "  they were still imported. bind them properly with: " << CONFIG
              << " add --force <name> <path>\n";
  }
  if (added.empty() && replaced.empty())
    out << "  nothing to do, your config already matches\n";
  return 0;
}

// ------------------------------------------------------------ project aliases
//
// A global alias is right for `dots` and wrong for `src`: inside a project, `src`
// means that project's src, and somewhere else it means something else or nothing.
// So a project can carry its own aliases in a `.simpledir.json` next to it, and
// `sd src` finds them anywhere inside that tree.
//
// The file is plain json and it goes in the repository, which is the point: it can
// be committed, shared, and reviewed. Absolute paths would make that useless, so
// entries are stored relative to the file when they can be, and always resolve
// against it.

// Walk up from the current directory looking for one. Stops at $HOME, so a stray
// file in your home directory can't turn every alias everywhere into a project one.
std::string find_project_dir() {
  std::error_code ec;
  fs::path dir = fs::current_path(ec);
  if (ec) return "";
  std::string home = home_dir();
  for (int depth = 0; depth < 64; depth++) {
    std::error_code sub_ec;
    fs::path candidate = dir / PROJECT_FILE;
    if (path_exists(candidate.string())) return dir.string();
    fs::path parent = dir.parent_path();
    if (parent == dir || dir.string() == home) break;
    if (home.size() && dir.string() == home) break;
    dir = parent;
    (void)sub_ec;
  }
  return "";
}

// Store a path relative to the project file when it is inside it, so the file can
// be committed and used on another machine. Anything else stays absolute, because
// a relative path to somewhere outside the project would be worse than useless.
std::string store_for_project(const std::string& absolute, const std::string& project_dir) {
  if (project_dir.empty()) return absolute;
  std::error_code ec;
  fs::path rel = fs::path(absolute).lexically_relative(project_dir);
  std::string text = rel.string();
  if (!text.empty() && !starts_with(text, "..")) return text;
  return absolute;
}

std::string resolve_project_path(const std::string& stored, const std::string& project_dir) {
  if (starts_with(stored, "~")) return abspath(stored);
  if (!stored.empty() && stored[0] == '/') return stored;
  return abspath(project_dir + "/" + stored);
}

// `sdcfg project` — writes. Everything here edits the nearest file.
int cmd_project(const Args& args) {
  std::error_code ec;
  std::string here = find_project_dir();

  if (args.has("list") || args.words.empty()) {
    if (here.empty()) {
      out << "no " << PROJECT_FILE << " here or in any parent directory up to your home.\n"
                << "  make one: " << CONFIG << " project add <name> <path>\n";
      return 1;
    }
    Project project = load_project(here);
    out << project.aliases.size()
              << (project.aliases.size() == 1 ? " alias" : " aliases") << " in "
              << here << "/" << PROJECT_FILE << ":\n\n";
    for (const auto& [name, stored] : project.aliases) {
      std::string target = resolve_project_path(stored, here);
      out << "  " << name << std::string(12 - std::min<size_t>(12, name.size()), ' ') << target
                << (is_dir(target) ? "" : "   [missing]") << "\n";
    }
    return 0;
  }

  std::string verb = args.word(0);

  if (verb == "rm" || verb == "remove") {
    if (here.empty()) die("no " + PROJECT_FILE + " here or in any parent up to your home");
    Project project = load_project(here);
    std::string name = args.word(1);
    if (name.empty()) throw UsageError("which one? " + CONFIG + " project rm <name>");
    auto it = project.aliases.find(name);
    if (it == project.aliases.end())
      throw UserError("'" + name + "' isn't in " + here + "/" + PROJECT_FILE);
    out << "removed " << name << " -> " << resolve_project_path(it->second, here) << "\n";
    project.aliases.erase(it);
    save_project(project);
    return 0;
  }

  if (verb != "add") throw UsageError("project takes add, rm or --list");

  std::string name = args.word(1);
  std::string raw = args.word(2);
  if (name.empty()) throw UsageError("usage: " + CONFIG + " project add <name> [<path>]");
  if (raw.empty()) raw = fs::current_path().string();
  std::string target = abspath(raw);
  require_dir(target);

  // Creating a file in $HOME would make every alias everywhere a project one, so
  // refuse it and say where to go instead.
  std::string home = home_dir();
  std::error_code dir_ec;
  std::string cwd = fs::current_path(dir_ec).string();
  if (cwd == home)
    die("not writing " + PROJECT_FILE + " in your home directory.\n"
        "  every alias would then depend on where you stood. cd into the project first.");

  if (here.empty()) here = cwd;
  Project project = load_project(here);
  std::string stored = store_for_project(target, here);
  if (project.aliases.count(name) && !args.has("force")) {
    std::string old = resolve_project_path(project.aliases.at(name), here);
    die("'" + name + "' is already in " + here + "/" + PROJECT_FILE + " -> " + old +
        "\n  overwrite: " + CONFIG + " project add --force " + name + " " + shell_quote(raw));
  }
  project.aliases[name] = stored;
  save_project(project);
  out << name << " -> " << target << "\n";
  out << "  stored in " << here << "/" << PROJECT_FILE;
  if (stored != target) out << " as the relative path \"" << stored << "\", so it travels";
  out << "\n  " << MOVE << " " << name << " works anywhere under " << here << "\n";
  return 0;
}

// ----------------------------------------------------------------------- tags
//
// A tag is a named group of alias names: `sd @work` is "the directories I use for
// work", `sd @work dots` jumps to one of them. It costs one object in the config
// and it makes a long alias list usable, because you almost never want all of it on
// screen at once.
//
// It is a group of *names*, not paths. The path stays in one place, so renaming or
// rebinding an alias updates every tag that mentions it, and `sd @work dots` can
// never disagree with `sd dots` about where dots is.

int cmd_tag(const Args& args) {
  Config cfg = load_config();

  // no name: show them all
  if (args.words.empty() || args.has("list")) {
    if (cfg.tags.empty()) {
      out << "no tags yet. make one:\n"
                << "  " << CONFIG << " tag @work dots hypr projects\n"
                << "then `" << MOVE << " @work` lists that group, and `" << MOVE
                << " @work dots` jumps to one\n";
      return 0;
    }
    for (const auto& [key, members] : cfg.tags) {
      out << "  " << key << "  " << members.size() << (members.size() == 1 ? " alias" : " aliases")
                << "\n";
      size_t width = 0;
      for (const std::string& m : members) width = std::max(width, m.size());
      out << "    ";
      for (const std::string& m : members) out << m << std::string(width - m.size() + 2, ' ');
      out << "\n";
    }
    return 0;
  }

  std::string key = tag_key(args.word(0));
  std::vector<std::string> wanted(args.words.begin() + 1, args.words.end());
  auto slot = std::find_if(cfg.tags.begin(), cfg.tags.end(),
                           [&](const std::pair<std::string, std::vector<std::string>>& t) {
                             return t.first == key;
                           });
  std::vector<std::string>* members = slot == cfg.tags.end() ? nullptr : &slot->second;

  if (args.has("remove") || args.has("drop")) {
    if (slot == cfg.tags.end())
      die("no tag " + key + ". " + CONFIG + " tag lists the ones you have");
    if (args.has("drop") && !wanted.empty())
      die("--drop takes no names. " + CONFIG + " tag --drop " + key + " removes the whole tag");
    if (args.has("drop")) {
      out << "  dropped " << key << " (" << slot->second.size()
                << (slot->second.size() == 1 ? " alias" : " aliases")
                << ", nothing was unbound)\n";
      slot->second.clear();
    }
    for (const std::string& name : wanted) {
      auto it = std::find(slot->second.begin(), slot->second.end(), name);
      if (it == slot->second.end()) {
        out << "  " << name << " isn't in " << key << "\n";
        continue;
      }
      slot->second.erase(it);
      out << "  removed " << name << " from " << key << "\n";
    }
    // Mutate in place and only erase the element once, by index. Erasing the
    // whole tag and re-adding `*members` used a pointer into the vector's own
    // storage, which the erase had already moved: removing `play` from @life
    // replaced it with @work's contents.
    bool empty = slot->second.empty();
    if (empty) cfg.tags.erase(cfg.tags.begin() + (slot - cfg.tags.begin()));
    save_config(cfg);
    if (empty) out << key << " is empty now, so it's gone\n";
    return 0;
  }

  if (wanted.empty())
    throw UsageError("which aliases? " + CONFIG + " tag " + key + " <alias>...");

  // a tag can only hold names that exist. Catching it here beats `sd @work dots`
  // failing later with a confusing message.
  std::vector<std::string> unknown;
  for (const std::string& name : wanted)
    if (!cfg.aliases.count(name)) unknown.push_back(name);
  if (!unknown.empty()) {
    std::string msg = key + " holds alias names, and these don't exist: " + join(unknown, " ");
    msg += "\n  bind them first: " + CONFIG + " add <name> <path>";
    msg += "\n  or see what you have: " + MOVE + " ls";
    throw UserError(msg);
  }

  if (!members) {
    cfg.tags.emplace_back(key, std::vector<std::string>());
    members = &cfg.tags.back().second;
  }
  std::vector<std::string> added;
  for (const std::string& name : wanted) {
    if (std::find(members->begin(), members->end(), name) != members->end()) continue;
    members->push_back(name);
    added.push_back(name);
  }
  save_config(cfg);

  if (added.empty()) {
    out << key << " already had all of those\n";
    return 0;
  }
  out << key << " (" << members->size() << (members->size() == 1 ? " alias" : " aliases") << "): "
            << join(added, " ") << "\n";
  out << "  " << MOVE << " " << key << "          list them\n"
            << "  " << MOVE << " " << key << " <alias>  jump to one\n";
  return 0;
}

// A tag can be jumped through or listed, so the move half has to understand one
// before it treats the word as an alias. Split off the tag if there is one, and
// hand back the member list for the caller to work with.
bool split_tag(const Args& args, Config& cfg, std::string& tag_name,
               std::vector<std::string>& members, std::string& rest) {
  if (args.words.empty()) return false;
  if (!starts_with(args.words[0], TAG_PREFIX)) return false;
  tag_name = args.words[0];
  rest = args.words.size() > 1 ? args.words[1] : "";
  const std::vector<std::string>* found = nullptr;
  for (const auto& [key, list] : cfg.tags)
    if (key == tag_name) found = &list;
  if (!found) {
    std::string msg = "no tag " + tag_name + ".";
    if (cfg.tags.empty())
      msg += " you have no tags: " + CONFIG + " tag @work dots hypr";
    else {
      msg += " you have:";
      for (const auto& [key, list] : cfg.tags) {
        (void)list;
        msg += " " + key;
      }
    }
    throw UserError(msg);
  }
  members = *found;
  return true;
}

// The members of a tag that exist, in tag order, with their paths.
std::vector<std::pair<std::string, std::string>> tag_rows(const Config& cfg,
                                                         const std::vector<std::string>& members) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const std::string& name : members) {
    auto it = cfg.aliases.find(name);
    if (it == cfg.aliases.end()) continue;  // renamed or removed since it was tagged
    out.emplace_back(name, it->second);
  }
  return out;
}

int cmd_tag_list(const Config& cfg, const std::string& tag_name,
                 const std::vector<std::string>& members) {
  auto rows = tag_rows(cfg, members);
  if (rows.empty()) {
    out << tag_name << " has nothing left in it. its aliases were renamed or removed.\n";
    out << "  drop it: " << CONFIG << " tag --drop " << tag_name << "\n";
    return 1;
  }
  std::string missing;
  for (const std::string& name : members)
    if (!cfg.aliases.count(name)) missing += (missing.empty() ? "" : " ") + name;

  size_t width = 0;
  for (const auto& [name, path] : rows) width = std::max(width, name.size());
  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  out << rows.size() << (rows.size() == 1 ? " alias" : " aliases") << " in " << tag_name << ":\n\n";
  for (const auto& [name, path] : rows) {
    std::string shown = path;
    if (!ec) {
      std::string relative = fs::path(path).lexically_relative(cwd).string();
      if (!relative.empty() && !starts_with(relative, "..")) shown = relative;
    }
    bool gone = !is_dir(as_stored(path));
    out << "  " << name << std::string(width - name.size() + 2, ' ') << shown
              << (gone ? "   [missing]" : "") << "\n";
  }
  if (!missing.empty())
    out << "\n  " << missing << " no longer exist as aliases. drop them with:\n"
              << "    " << CONFIG << " tag " << tag_name << " --remove " << missing << "\n";
  out << "\n  jump to one: " << MOVE << " " << tag_name << " <name>\n";
  return 0;
}

// ------------------------------------------------------------------- adapting
//
// The frecency log already knows every directory you go to, including the ones you
// never named. Those are the ones worth a name: a directory you visit forty times a
// month and have to tab-complete every time.
//
// What makes this adaptive rather than a `sd suggest` for frecency is the verdicts.
// Say no to a proposal and it goes quiet, and saying no again pushes it quieter —
// the backoff doubles per rejection. Say yes and it's bound and never proposed
// again. All of it is arithmetic over two counters; nothing here guesses anything.

constexpr double ADAPT_BASE_DAYS = 7;      // quiet after the first "no"
constexpr double ADAPT_MAX_DAYS = 365;     // ...but never for more than a year
constexpr size_t ADAPT_MAX_VERDICTS = 500;

struct Verdict {
  std::string verdict;  // "yes" or "no"
  double at = 0;
  int strikes = 0;
};

std::string adapt_file() { return g_config_dir + "/adapt.json"; }

std::map<std::string, Verdict> read_verdicts() {
  std::map<std::string, Verdict> out;
  std::string path = adapt_file();
  if (!path_exists(path)) return out;
  JsonPtr root;
  try {
    root = json_parse(read_file(path));
  } catch (const UserError&) {
    return out;  // our own file; a bad one must not stop a `cd`
  }
  if (!root || !root->is_obj()) return out;
  JsonPtr verdicts = root->get("verdicts");
  if (!verdicts || !verdicts->is_obj()) return out;
  for (const auto& [path_key, node] : verdicts->obj) {
    if (!node || !node->is_obj()) continue;
    Verdict v;
    if (JsonPtr s = node->get("v")) v.verdict = s->as_str();
    if (JsonPtr t = node->get("t")) v.at = t->as_num();
    if (JsonPtr n = node->get("s")) v.strikes = static_cast<int>(n->as_num());
    if (!v.verdict.empty()) out[path_key] = v;
  }
  return out;
}

void write_verdicts(const std::map<std::string, Verdict>& verdicts) {
  // keep the file from growing forever: drop the oldest first, and anything that
  // faded years ago along with it
  double now = now_seconds();
  std::vector<std::pair<double, std::string>> by_age;
  for (const auto& [path, v] : verdicts)
    if (now - v.at > 2 * ADAPT_MAX_DAYS * 24 * 3600) continue;
    else by_age.emplace_back(v.at, path);
  if (by_age.size() > ADAPT_MAX_VERDICTS) {
    std::sort(by_age.begin(), by_age.end());
    by_age.erase(by_age.begin(), by_age.begin() + (by_age.size() - ADAPT_MAX_VERDICTS));
  }

  auto root = Json::make_obj();
  root->set("version", Json::make_num(1));
  auto node = Json::make_obj();
  std::set<std::string> keep;
  for (const auto& [age, path] : by_age) keep.insert(path);
  for (const auto& [path, v] : verdicts) {
    if (!keep.count(path)) continue;
    auto entry = Json::make_obj();
    entry->set("v", Json::make_str(v.verdict));
    entry->set("t", Json::make_num(v.at));
    entry->set("s", Json::make_num(v.strikes));
    node->set(path, entry);
  }
  root->set("verdicts", node);
  try {
    write_atomic(adapt_file(), json_dump(root) + "\n");
  } catch (const std::exception&) {
    // a read-only home shouldn't break a command
  }
}

// How long a "no" lasts. Doubles per rejection, so a directory you keep refusing
// stops taking up a slot in the list, and a year is the end of it — people move on
// and come back.
double quiet_for(const Verdict& v) {
  int strikes = v.strikes < 1 ? 1 : v.strikes;
  double days = ADAPT_BASE_DAYS * std::pow(2.0, strikes - 1);
  return std::min(days, ADAPT_MAX_DAYS) * 24 * 3600;
}

struct Proposal {
  std::string path;
  std::string name;
  double score = 0;
  bool refused = false;  // you said no at some point
  bool retry = false;    // ...and the backoff has since run out, so it's asking again
  int strikes = 0;
  double quiet_left = 0;  // seconds until it asks again, when refused and not retrying
};

std::vector<Proposal> adapt_proposals(int limit, bool include_rejected) {
  Config cfg = load_config();
  double now = now_seconds();
  auto verdicts = read_verdicts();

  std::set<std::string> bound;
  std::set<std::string> taken;
  for (const auto& [name, path] : cfg.aliases) {
    bound.insert(as_stored(path));
    taken.insert(name);
  }

  std::vector<Proposal> out;
  for (const auto& [path, entry] : read_history()) {
    if (bound.count(path)) continue;       // it has a name; `sd <name>` is the way in
    if (!is_dir(path)) continue;           // it isn't there any more
    double score = decayed(entry, now);
    if (score < FLOOR) continue;           // you haven't been in ages

    auto it = verdicts.find(path);
    bool refused = false, retry = false;
    int strikes = 0;
    double quiet_left = 0;
    if (it != verdicts.end()) {
      const Verdict& v = it->second;
      strikes = v.strikes;
      if (v.verdict == "yes") continue;    // handled; it has a name or you said it was fine
      if (v.verdict == "no") {
        refused = true;
        quiet_left = (v.at + quiet_for(v)) - now;
        if (quiet_left > 0) {
          if (!include_rejected) continue;
        } else {
          retry = true;                    // the backoff ran out; worth asking once more
          quiet_left = 0;
        }
      }
    }
    out.push_back({path, name_for(path, taken), score, refused, retry, strikes, quiet_left});
  }
  std::sort(out.begin(), out.end(), [](const Proposal& a, const Proposal& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.path < b.path;
  });
  if (limit > 0 && static_cast<size_t>(limit) < out.size()) out.resize(limit);
  return out;
}

// `sd adapt` — read-only. what it wants to learn, and nothing else.
int cmd_adapt(const Args& args) {
  int limit = 10;
  if (args.saw("top")) limit = std::atoi(args.value("top", "10").c_str());
  auto found = adapt_proposals(limit, args.has("all"));

  if (found.empty()) {
    auto verdicts = read_verdicts();
    size_t refused = 0;
    for (const auto& [path, v] : verdicts)
      if (v.verdict == "no") refused++;
    out << "nothing left to learn. every directory you keep visiting has a name"
              << " already.\n";
    if (refused) out << "  " << refused << " refused, waiting out their quiet period\n";
    out << "  " << MOVE << " top shows the whole log, including named ones\n";
    return 0;
  }

  if (args.has("json")) {
    auto list = Json::make_arr();
    for (const Proposal& p : found) {
      auto node = Json::make_obj();
      node->set("path", Json::make_str(p.path));
      node->set("name", Json::make_str(p.name));
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.3f", p.score);
      node->set("score", Json::make_num(std::stod(buf)));
      node->set("retry", Json::make_bool(p.retry));
      node->set("refusals", Json::make_num(p.strikes));
      list->arr.push_back(node);
    }
    out << json_dump(list) << "\n";
    return 0;
  }

  size_t width = 0;
  for (const Proposal& p : found) width = std::max(width, p.path.size());
  out << found.size() << " director"
            << (found.size() == 1 ? "y" : "ies")
            << " you keep visiting that " << (found.size() == 1 ? "has" : "have")
            << " no name, most recent first:\n\n";
  for (const Proposal& p : found) {
    char score[32];
    std::snprintf(score, sizeof score, "%6.2f", p.score);
    out << "  " << score << "  " << p.name << std::string(12 - std::min<size_t>(12, p.name.size()), ' ')
              << "  " << p.path;
    if (p.retry) {
      out << "   [you said no " << p.strikes << "x, asking once more]";
    } else if (p.refused) {
      long hours = static_cast<long>(p.quiet_left / 3600);
      out << "   [refused, quiet for "
                << (hours < 48 ? std::to_string(hours) + "h"
                               : std::to_string(hours / 24) + "d")
                << "]";
    }
    out << "\n";
  }
  out << "\nkeep one:\n"
            << "  " << CONFIG << " adapt --accept <name>          bind it as well as remember it\n"
            << "  " << CONFIG << " adapt --reject <name>          stop asking\n"
            << "  " << CONFIG << " adapt --accept <name> --no-bind just remember the verdict\n"
            << "\nforget every verdict:\n"
            << "  " << CONFIG << " adapt --clear\n";
  return 0;
}

// `sdcfg adapt` — writes. records what you said, and optionally acts on it.
int cmd_adapt_apply(const Args& args) {
  if (args.has("clear")) {
    if (!path_exists(adapt_file())) {
      out << "no verdicts recorded yet\n";
      return 0;
    }
    std::error_code ec;
    fs::remove(adapt_file(), ec);
    out << "forgot every verdict. " << MOVE << " adapt will ask about anything you refused again.\n"
              << "  names you already bound are names, not verdicts — " << CONFIG << " rm to unbind one\n";
    return 0;
  }

  std::string verdict = args.has("accept") ? "yes" : args.has("reject") ? "no" : "";
  if (verdict.empty())
    throw UsageError("say yes or no: " + CONFIG + " adapt --accept <name>, or --reject <name>");

  // The argument is the name as `sd adapt` printed it. Resolve it back to a path
  // through the proposals rather than trusting the name to be unique on its own.
  // --accept and --reject are switches, so the name is the first *word*, not the
  // second argument
  std::string wanted = args.word(0);
  if (wanted.empty())
    throw UsageError("which one? " + CONFIG + " adapt --accept <name>");

  auto found = adapt_proposals(0, true);
  std::string target;
  for (const Proposal& p : found) {
    if (p.name == wanted) { target = p.path; break; }
    // also accept the full path, because that's what's on screen
    if (p.path == wanted) { target = p.path; break; }
  }
  if (target.empty()) {
    std::string msg = "'" + wanted + "' isn't one of the proposals";
    if (!found.empty()) msg += ". run `" + MOVE + " adapt` to see them";
    throw UserError(msg);
  }

  auto verdicts = read_verdicts();
  Verdict& v = verdicts[target];
  double now = now_seconds();
  v.verdict = verdict;
  v.at = now;
  // Saying yes twice, or no once then yes, clears the strike count: the answer
  // changed, so the old refusals no longer describe you.
  v.strikes = verdict == "no" ? v.strikes + 1 : 0;
  if (verdict == "yes") v.strikes = 0;
  write_verdicts(verdicts);

  Config cfg = load_config();
  std::string bound_name;
  if (verdict == "yes" && !args.has("no-bind")) {
    std::set<std::string> taken;
    for (const auto& [name, path] : cfg.aliases) {
      (void)path;
      taken.insert(name);
    }
    std::string name = name_for(target, taken);
    if (cfg.aliases.count(name)) {
      out << "not binding: the name '" << name << "' is already taken by "
                << cfg.aliases.at(name) << "\n";
    } else {
      cfg.aliases[name] = target;
      save_config(cfg);
      bound_name = name;
    }
  }

  if (verdict == "no") {
    double days = quiet_for(v) / (24 * 3600);
    out << "noted. " << target << "\n";
    out << "  quiet for " << (days < 1 ? std::to_string(days * 24) + " hours"
                                            : std::to_string(static_cast<long>(days)) + " days");
    if (v.strikes > 1) out << ", doubled because that's refusal " << v.strikes;
    out << "\n  it'll ask again once that's up, then stop asking entirely after a year\n";
  } else if (!bound_name.empty()) {
    out << "bound " << bound_name << " -> " << target << "\n";
    out << "  " << MOVE << " " << bound_name << " now\n";
  } else {
    out << "noted, not bound: " << target << "\n";
  }
  return 0;
}

// ------------------------------------------------------------------ move half

int cmd_ls(const Args& args) {
  Config cfg = load_config();
  std::string query = args.word(0);

  // `sd ls @work` is the tag, filtered the same way a name filter is
  std::string tag_filter;
  if (starts_with(query, TAG_PREFIX)) {
    tag_filter = query;
    query.clear();
    const std::vector<std::string>* members = nullptr;
    for (const auto& [key, list] : cfg.tags)
      if (key == tag_filter) members = &list;
    if (!members) {
      std::string msg = "no tag " + tag_filter + ".";
      if (cfg.tags.empty())
        msg += " you have no tags: " + CONFIG + " tag @work dots hypr";
      else {
        msg += " you have:";
        for (const auto& [key, list] : cfg.tags) {
          (void)list;
          msg += " " + key;
        }
      }
      throw UserError(msg);
    }
  }

  auto found = rows(cfg, query);
  if (!tag_filter.empty()) {
    const std::vector<std::string>* members = nullptr;
    for (const auto& [key, list] : cfg.tags)
      if (key == tag_filter) members = &list;
    std::set<std::string> keep(members->begin(), members->end());
    found.erase(std::remove_if(found.begin(), found.end(),
                               [&](const std::tuple<std::string, std::string, bool>& row) {
                                 return !keep.count(std::get<0>(row));
                               }),
                found.end());
  }

  if (args.has("names")) {
    for (const auto& [name, target, missing] : found) {
      (void)target;
      (void)missing;
      out << name << "\n";
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
    out << json_dump(node) << "\n";
    return found.empty() ? 1 : 0;
  }

  if (found.empty()) {
    if (!cfg.aliases.empty() && !query.empty()) {
      err << MOVE << ": nothing matches '" << query << "'. see them all: " << MOVE << " ls\n";
    } else {
      err << MOVE << ": no aliases yet. add one: " << CONFIG << " add\n";
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
    out << name << std::string(width - name.size() + 2, ' ') << shown;
    std::vector<std::string> badges;
    if (missing) badges.push_back("missing");
    // no badge when you already filtered by that tag: it would just repeat
    if (tag_filter.empty()) {
      for (const auto& [key, members] : cfg.tags) {
        if (std::find(members.begin(), members.end(), name) != members.end())
          badges.push_back(tag_label(key));
      }
    }
    if (!badges.empty()) out << "   [" << join(badges, " ") << "]";
    out << "\n";
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
    out << "nothing in your history yet. every directory you jump to gets remembered.\n"
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
    out << json_dump(list) << "\n";
    return 0;
  }

  size_t width = 0;
  for (const Row& row : ranked) width = std::max(width, row.path.size());
  out << ranked.size() << " directories, most recent visits first:\n\n";
  bool any_gone = false;
  for (const Row& row : ranked) {
    std::vector<std::string> tags;
    if (!row.exists) { tags.push_back("gone"); any_gone = true; }
    else if (row.named) tags.push_back("named");
    char score[32];
    std::snprintf(score, sizeof score, "%6.2f", row.score);
    out << "  " << score << "  " << row.path
              << std::string(width - row.path.size(), ' ');
    if (!tags.empty()) out << "   [" << join(tags, ", ") << "]";
    out << "\n";
  }
  if (any_gone) out << "\n  clean those up: " << CONFIG << " forget --missing\n";
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
    out << name << "\n";
    return 0;
  }

  std::string reason = std::getenv("SIMPLEDIR_NO_FZF") ? "fzf disabled (SIMPLEDIR_NO_FZF)"
                                                        : "no fzf installed";
  out << reason << ", so: pick a number or type a name\n\n";
  size_t width = 0;
  for (const std::string& name : names) width = std::max(width, name.size());
  for (size_t i = 0; i < found.size(); i++) {
    std::string shown = std::get<1>(found[i]) + (std::get<2>(found[i]) ? "   [missing]" : "");
    char number[16];
    std::snprintf(number, sizeof number, "%3zu", i + 1);
    out << "  " << number << "  " << names[i] << std::string(width - names[i].size(), ' ')
              << "  " << shown << "\n";
  }
  out << "\n> "; out.flush();
  std::string answer;
  if (!read_line(answer)) return 1;
  answer = trim(answer);
  if (answer.empty()) return 1;

  if (answer.find_first_not_of("0123456789") == std::string::npos) {
    size_t index = std::stoul(answer);
    if (index < 1 || index > names.size())
      throw UserError("no entry " + answer + ". there are " + std::to_string(names.size()) + ".");
    out << names[index - 1] << "\n";
    return 0;
  }
  if (std::find(names.begin(), names.end(), answer) == names.end())
    throw UserError(unknown_alias_error(answer, cfg));
  out << answer << "\n";
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
  out << name << " -> " << target << "\n";
  nudge();
  if (!derived.empty() && name != derived)
    out << "  rename it: " << CONFIG << " rename " << name << " <other-name>\n";
  if (keep && fs::is_symlink(target)) out << "  kept the symlink: " << target << "\n";
  return 0;
}

int cmd_rm(const Args& args) {
  std::string name = args.word(0);
  if (name.empty()) throw UserError("which alias? usage: " + CONFIG + " rm <name>");
  Config cfg = load_config();
  auto it = cfg.aliases.find(name);
  if (it == cfg.aliases.end()) throw UserError(unknown_alias_error(name, cfg));
  out << "removed " << name << " -> " << it->second << "\n";
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
  out << from << " -> " << to << " (" << target << ")\n";
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

  out << (dry ? "would bind " : "bound ") << bound.size() << ": " << join(bound, " ") << "\n";
  if (!skipped.empty()) {
    out << "skipped " << skipped.size() << " already bound: " << join(skipped, " ") << "\n";
    if (!force) out << "  overwrite them: " << CONFIG << " import --force " << root << "\n";
  }
  return 0;
}
// ------------------------------------------------------------------- bench
//
// v10 is the release about being fast, so it had better be able to prove it. This
// times the binary as the shell invokes it -- fork, exec, loader, parse, print --
// because that is the only number anyone actually pays. Nothing here is modelled
// or estimated; it runs the real command and counts wall-clock time.
//
// It measures the whole process from the outside, so it has to fork itself. That
// is the honest cost: a jump is `sd name`, and a wrapper that avoided the fork
// would be measuring something nobody runs.

double now_micros() {
  return std::chrono::duration<double, std::micro>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

// fork/exec ourselves and wait, so the measurement includes everything a shell's
// `sd name` does and nothing a library call would have hidden.
double time_one(const std::string& self, const std::vector<std::string>& words,
                const std::string& cwd) {
  double start = now_micros();
  pid_t pid = fork();
  if (pid == 0) {
    if (!cwd.empty()) {
      if (chdir(cwd.c_str()) != 0) std::_Exit(127);
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(self.c_str()));
    for (const std::string& w : words) argv.push_back(const_cast<char*>(w.c_str()));
    argv.push_back(nullptr);
    // Both streams, to /dev/null: we are timing rather than reading, and a pipe
    // the parent never drains would measure the pipe instead of the program.
    // stderr too, because `sd print <miss>` fails on purpose and an error
    // message printed 40 times would be this command's loudest output.
    int null_fd = ::open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      ::dup2(null_fd, 1);
      ::dup2(null_fd, 2);
      ::close(null_fd);
    }
    execv(self.c_str(), argv.data());
    std::_Exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return now_micros() - start;
}

struct Timing {
  double median = 0;
  double best = 0;
  bool ok = true;
};

Timing measure(const std::string& self, const std::vector<std::string>& words,
               const std::string& cwd, int runs) {
  std::vector<double> samples;
  samples.reserve(runs);
  // warm up hard: the first few execs after a build pay for a cold page cache,
  // and a benchmark that measures that is measuring the build
  for (int i = 0; i < 6; i++) time_one(self, words, cwd);
  for (int i = 0; i < runs; i++) samples.push_back(time_one(self, words, cwd));
  std::sort(samples.begin(), samples.end());
  Timing t;
  t.median = samples[samples.size() / 2];
  t.best = samples.front();
  return t;
}

int cmd_bench(const Args& args) {
  // Deliberately tolerant: this measures the machine, and a config it cannot
  // parse is exactly when somebody reaches for a benchmark. Failing here would
  // be the one moment the tool is useless.
  Config cfg;
  size_t alias_count = 0;
  try {
    cfg = load_config();
    alias_count = cfg.aliases.size();
  } catch (const UserError&) {
    alias_count = 0;
  }
  int runs = 40;
  if (args.has("runs")) {
    runs = std::atoi(args.value("runs", "40").c_str());
    if (runs < 5 || runs > 2000)
      throw UsageError("--runs takes 5 to 2000, got " + args.value("runs", "40"));
  }

  std::string self;
  {
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    self = ec ? std::string(MOVE) : exe.string();
  }

  size_t entries = 0;
  if (path_exists(g_history_file)) {
    try {
      JsonPtr root = json_parse(read_file(g_history_file));
      if (JsonPtr dirs = root->get("dirs")) entries = dirs->obj.size();
    } catch (const UserError&) {
      entries = 0;  // an unreadable log is not this command's problem to report
    }
  }

  out << "simpledir benchmark (" << MOVE << " " << VERSION << ")\n\n"
      << "  every timing below is a real fork+exec of this binary, median of " << runs
      << " runs,\n  measured on this machine just now. yours will differ.\n\n";

  // The floor, because without it the rest is unreadable: this is what it costs to
  // start any process at all on this box, before a single line of this program
  // runs. An empty C++ program costs exactly the same.
  //
  // Measured twice, before and after the cases, and the *faster* of the two is
  // used. Measuring it only at the start was a bug with visible consequences: the
  // first measurement absorbs the cold page cache for the binary, and every case
  // measured afterwards then looks faster than doing nothing at all -- which is
  // what `sdcfg bench --runs 12` printed, at -0.065 ms for a jump. A benchmark
  // that reports a jump as cheaper than an empty program is worse than none.
  Timing floor = measure(self, {"--version"}, "", runs);

  struct Case {
    std::string label;
    std::vector<std::string> words;
    std::string note;
  };
  std::vector<Case> cases;
  if (!cfg.aliases.empty())
    cases.push_back({"sd <alias>", {cfg.aliases.begin()->first}, "the common case"});
  cases.push_back({"sd ls", {"ls"}, std::to_string(alias_count) + " aliases"});
  if (entries)
    cases.push_back({"sd top", {"top"}, std::to_string(entries) + " dirs remembered"});
  if (entries)
    cases.push_back({"sd print <miss>", {"print", "nosuchthingxyz"}, "frecency scan, worst case"});
  cases.push_back({"sd doctor", {"doctor"}, "every check it can do"});

  char row[768];  // a table row, or the paragraph underneath it
  std::snprintf(row, sizeof row, "  %-18s %10s %13s %11s   %s\n", "case", "median", "over startup",
                "best", "");
  out << row;
  std::snprintf(row, sizeof row, "  %s\n", std::string(74, '-').c_str());
  out << row;

  // `--version` does no work at all, so it is both the floor and a row of its own.
  std::snprintf(row, sizeof row,
                "  %-18s %7.3f ms %13s %7.3f ms   any process at all\n",
                "process startup", floor.median / 1000.0, "--", floor.best / 1000.0);
  out << row;

  double jump_over_floor = 0;
  for (size_t i = 0; i < cases.size(); i++) {
    Timing t = measure(self, cases[i].words, "", runs);
    if (i == 0) jump_over_floor = (t.median - floor.median) / 1000.0;
    std::snprintf(row, sizeof row, "  %-18s %7.3f ms %+9.3f ms %7.3f ms   %s\n",
                  cases[i].label.c_str(), t.median / 1000.0, (t.median - floor.median) / 1000.0,
                  t.best / 1000.0, cases[i].note.c_str());
    out << row;
  }

  // now that the cache is warm, ask again, and believe whichever is faster
  Timing floor_again = measure(self, {"--version"}, "", runs);
  if (floor_again.median < floor.median) floor = floor_again;

  // say the thing the table is actually saying, because a table of milliseconds
  // on its own invites the reading that this program is slow
  std::snprintf(row, sizeof row,
                "\n  starting any process here costs %.3f ms, before this program does\n"
                "  anything at all. a jump costs %.3f ms more than that: read the config, find\n"
                "  the name, check the directory still exists, print the path. The frecency\n"
                "  log is not searched on a jump -- recording a visit only needs one\n"
                "  timestamp out of it, which is a substring scan rather than a parse.\n",
                floor.median / 1000.0, jump_over_floor);
  out << row;
  out << "  more runs: " << CONFIG << " bench --runs 200\n";
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
  for (const std::string name : {".bash_history", ".zsh_history",
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
    for (const std::string name : {".bash_history", ".zsh_history", ".local/share/fish/fish_history",
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
    out << "every directory in your history is already bound (" << cfg.aliases.size()
              << " aliases)\n";
    return 0;
  }

  if (args.has("json")) {
    auto node = Json::make_obj();
    for (const Candidate& c : candidates) node->set(c.path, Json::make_num(c.hits));
    out << json_dump(node) << "\n";
    return 0;
  }

  size_t width = 0;
  for (const Candidate& c : candidates) width = std::max(width, c.path.size());
  out << candidates.size() << " new directories from your history, showing the top "
            << candidates.size() << ":\n\n";
  for (const Candidate& c : candidates) {
    char hits[16];
    std::snprintf(hits, sizeof hits, "%5d", c.hits);
    out << "  " << hits << "x  " << c.path << std::string(width - c.path.size(), ' ') << "\n";
  }
  out << "\nbind them all:\n";
  std::set<std::string> taken;
  for (const auto& [name, path] : cfg.aliases) taken.insert(name);
  for (const Candidate& c : candidates) {
    out << "  " << CONFIG << " add " << name_for(c.path, taken) << " " << shell_quote(c.path)
              << "\n";
    taken.insert(name_for(c.path, taken));
  }
  return 0;
}

// `sd suggest`, but it actually binds.
int cmd_bind(const Args& args) {
  auto candidates = history_candidates(10);
  if (candidates.empty()) {
    out << "nothing new in your history to bind\n";
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
    out << "would bind " << names.size() << ": " << join(names, " ") << "\n";
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
  out << "bound " << made.size() << ":\n";
  for (const auto& [name, path] : made) out << "  " << name << " -> " << path << "\n";
  out << "\nrename any of them: " << CONFIG << " rename <old> <new>\n";
  return 0;
}

// Drop things from the visit log. The log is ours, so wiping it is fine.
int cmd_forget(const Args& args) {
  auto dirs = read_history();
  if (dirs.empty()) {
    out << "your visit log is already empty\n";
    return 0;
  }

  if (args.has("all")) {
    if (!args.has("yes") && isatty(STDIN_FILENO)) {
      out << "  forget all " << dirs.size() << " remembered directories? [y/N] "; out.flush();
      std::string answer;
      if (!read_line(answer) || lower(trim(answer)) != "y") {
        out << "  ok, left alone\n";
        return 0;
      }
    }
    std::error_code ec;
    fs::remove(g_history_file, ec);
    out << "forgot all " << dirs.size() << " remembered directories\n";
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
    out << "forgot " << target << "\n";
    return 0;
  }

  std::vector<std::string> gone;
  for (const auto& [path, visit] : dirs) {
    (void)visit;
    if (!is_dir(path)) gone.push_back(path);
  }
  if (gone.empty()) {
    out << "nothing forgotten: every remembered directory still exists\n";
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
  out << "forgot " << gone.size() << " directories that no longer exist:\n";
  for (const std::string& path : gone) out << "  " << path << "\n";
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
    out << "config is already version " << CONFIG_VERSION << ". nothing to do.\n";
    return 0;
  }

  if (args.has("dry-run")) {
    out << "would migrate " << g_config_file << ": version " << found << " -> " << CONFIG_VERSION
              << "\n  " << cfg.aliases.size() << " aliases kept exactly as they are";
    if (found < 2) out << "\n  would add \"history\": true, and create " << g_history_file;
    if (found < 3) out << "\n  would add an empty \"tags\" object, for " << CONFIG << " tag";
    out << "\n";
    return 0;
  }

  // back up first: a migration that loses somebody's aliases is unforgivable
  std::string backup = g_config_file + ".bak." + stamp();
  write_atomic(backup, read_file(g_config_file));

  cfg.version = CONFIG_VERSION;
  save_config(cfg);

  std::vector<std::string> did;
  if (found < 2) {
    if (!path_exists(g_history_file)) {
      auto root = Json::make_obj();
      root->set("version", Json::make_num(1));
      root->set("dirs", Json::make_obj());
      write_atomic(g_history_file, json_dump(root) + "\n");
    }
    did.push_back("created " + g_history_file + " (empty), so " + MOVE + " top has something to read");
  }
  if (found < 3) did.push_back("added an empty \"tags\" object, for " + CONFIG + " tag");

  out << "migrated " << g_config_file << ": version " << found << " -> " << CONFIG_VERSION << "\n"
            << "  " << cfg.aliases.size() << " aliases kept as they were\n"
            << "  backup: " << backup << "\n";
  for (const std::string& line : did) out << "  " << line << "\n";
  if (found < 2) out << "  now `" << MOVE << " top` will remember where you go\n";
  if (found < 3)
    out << "  tags: `" << CONFIG << " tag <name> <alias>...`, then `" << MOVE
              << " @<name> <alias>`\n";
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
  std::string dump = popen_capture("sqlite3 " + shell_quote(db) + " " + shell_quote(sql) + " 2>/dev/null");
  if (trim(dump).empty()) {
    die("zoxide's database had nothing in it, or it isn't a sqlite file.\n"
        "  if zoxide is very old it used a different format; try `zoxide import` in zoxide itself first");
  }

  struct Row {
    std::string path;
    double rank;
  };
  std::vector<Row> rows_in;
  for (const std::string& line : split(dump, '\n')) {
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

  out << "top " << rows_in.size() << " directories from zoxide's database:\n\n";
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
    out << "  " << rank << "  " << row.path << std::string(width - row.path.size(), ' ');
    if (already) out << "   [already bound]";
    out << "\n";
    if (!already && args.has("bind")) {
      cfg.aliases[suggested] = row.path;
      taken.insert(suggested);
      made.emplace_back(suggested, row.path);
    }
  }

  if (args.has("bind")) {
    if (dry) {
      out << "\nwould bind " << made.size() << "\n";
    } else {
      save_config(cfg);
      out << "\nimported " << made.size() << " from zoxide:\n";
      for (const auto& [name, path] : made) out << "  " << name << " -> " << path << "\n";
      out << "\nrename any you got wrong: " << CONFIG << " rename <old> <new>\n";
    }
  } else {
    out << "\nbind them all:\n";
    for (const Row& row : rows_in) {
      if (bound.count(row.path)) continue;
      out << "  " << CONFIG << " add " << name_for(row.path, taken) << " "
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
// ------------------------------------------------------------------- sha-256
//
// `sdcfg update` downloads a binary over the network and swaps it in, so asking
// "did these bytes arrive intact?" is not optional. There is no TLS in the C++
// standard library and no crypto library in the dependency list, and adding one
// for a hash function that is forty lines of arithmetic would be silly: SHA-256
// is exactly the kind of thing this project does with nothing but the stdlib.
//
// It is here to verify, never to authenticate. A checksum published next to the
// file it describes proves the two arrived together; it cannot prove who wrote
// them. Anyone who can change the download can change the sum. What it does catch
// is the ordinary failure — a truncated transfer, a corrupted mirror, a proxy
// that mangled a byte, an asset quietly swapped between two requests — and it
// catches it before a wrong binary lands in your PATH.

constexpr uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// returns the lowercase hex digest of `data`
std::string sha256_hex(const std::string& data) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

  std::string msg = data;
  const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back('\0');
  for (int i = 7; i >= 0; i--) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));

  for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(msg.data()) + chunk + i * 4;
      w[i] = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
             (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    }
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = hh + S1 + ch + SHA256_K[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      hh = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }

  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (uint32_t word : h)
    for (int i = 3; i >= 0; i--) {
      unsigned byte = (word >> (i * 8)) & 0xff;
      out.push_back(digits[byte >> 4]);
      out.push_back(digits[byte & 0xf]);
    }
  return out;
}

// The sha256 a sums file publishes for `name`, or "" when it doesn't list it.
// The file is `sha256sum` output: "<64 hex>  <name>", two spaces, names bare.
std::string published_sum(const std::string& sums, const std::string& name) {
  std::istringstream in(sums);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    size_t gap = line.find_first_of(" \t");
    if (gap == std::string::npos) continue;
    std::string digest = line.substr(0, gap);
    std::string listed = line.substr(gap);
    while (!listed.empty() && (listed.front() == ' ' || listed.front() == '\t')) listed.erase(0, 1);
    if (!listed.empty() && listed[0] == '*') listed.erase(0, 1);
    if (listed != name) continue;
    for (char c : digest) {
      bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if (!hex) return "";
    }
    if (digest.size() != 64) return "";
    for (char& c : digest) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return digest;
  }
  return "";
}

// Where the SHA256SUMS for a release lives. `latest` is a redirect like the asset
// itself, so it has to be spelled the same way or the two can come from different
// releases -- which would report every download as tampered.
std::string sums_url_for(const std::string& base, const std::string& tag) {
  std::string stem = tag == "latest" ? base + "/latest/download/" : base + "/download/" + tag + "/";
  return stem + "SHA256SUMS";
}

// `sd sha256 <file>`, so verifying a release does not require trusting the thing
// doing the verifying. Every release publishes a SHA256SUMS, and the installer
// and `update` both check it; this is for the person who wants to look for
// themselves, or who downloaded an asset by hand and has nothing to point it at.
int cmd_sha256(const Args& args) {
  std::string what = args.word(0);
  if (what.empty())
    throw UsageError("which file? " + MOVE + " sha256 <file>\n"
                     "  to check a download against its release: " + MOVE +
                     " sha256 sd-linux-x86_64");
  if (what == "-") {
    // piped in, because that is how you hash a string: printf abc | sd sha256 -
    std::string all;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, stdin)) > 0) all.append(buf, n);
    out << sha256_hex(all) << "  -\n";
    return 0;
  }
  std::string path = abspath(what);
  if (!path_exists(path)) die("no such file: " + path);
  std::error_code ec;
  if (fs::is_directory(path, ec)) die("that's a directory: " + path + "\n  hash a file, not a directory");
  out << sha256_hex(read_file(path)) << "  " << what << "\n";
  return 0;
}

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
  // Each candidate remembers where it came from, so the message afterwards doesn't
  // have to guess from the hostname — which also meant it stayed silent whenever
  // SIMPLEDIR_SOURCE_URL pointed anywhere but raw.githubusercontent.com.
  std::vector<std::pair<std::string, bool>> urls;  // url, from_source
  if (const char* override = std::getenv("SIMPLEDIR_UPDATE_ASSET_URL")) {
    urls.emplace_back(override, false);
  } else {
    std::string stem = tag == "latest" ? base + "/latest/download/"
                                       : base + "/download/" + tag + "/";
    urls.emplace_back(stem + asset_name(), false);
    urls.emplace_back(stem + "sd", false);
    urls.emplace_back(stem + "simpledir", false);
    // v1.0.0 and v2.0.0 were never given a release asset at all, but the program
    // is right there in the tag: one executable file, mode 755, called `simpledir`
    // (and `sd` from v5.0.0). Going back to the source of the tag is the last
    // resort, so `--to v2.0.0` does what it says instead of explaining that the
    // version exists and then refusing to install it.
    std::string raw = env_or("SIMPLEDIR_SOURCE_URL", "https://raw.githubusercontent.com/");
    if (raw.size() && raw.back() == '/') raw.pop_back();
    urls.emplace_back(raw + "/" + g_repo + "/" + tag + "/sd", true);
    urls.emplace_back(raw + "/" + g_repo + "/" + tag + "/simpledir", true);
  }

  std::string target = install_target();
  std::error_code ec;
  fs::create_directories(fs::path(target).parent_path(), ec);
  std::string staged = target + ".new." + std::to_string(static_cast<long>(getpid()));

  std::string url = urls.front().first;
  bool fetched = false;
  bool from_source = false;
  for (const auto& candidate : urls) {
    if (download(candidate.first, staged)) {
      url = candidate.first;
      from_source = candidate.second;
      fetched = true;
      break;
    }
  }
  if (!fetched) {
    std::vector<std::string> tried;
    for (const auto& candidate : urls) tried.push_back(candidate.first);
    fs::remove(staged, ec);
    die("download failed for " + tag + ":\n  tried " + join(tried, "\n         ") +
        "\n  check the tag with " + CONFIG + " releases");
  }
  // Check the bytes against the checksum this release published, before anything
  // is run or replaced. Fetching the sums file is best effort: v1.0.0 and v2.0.0
  // have no release assets at all and so have no sums, and neither does a mirror
  // that only carries binaries. A *wrong* sum is never best effort -- that is the
  // one case where continuing would put someone else's bytes in your PATH.
  if (!from_source) {
    std::string sums_staged = staged + ".sums";
    bool have_sums = download(sums_url_for(base, tag), sums_staged);
    std::string listed;
    if (have_sums) listed = published_sum(read_file(sums_staged), asset_name());
    fs::remove(sums_staged, ec);
    if (!listed.empty()) {
      std::string actual = sha256_hex(read_file(staged));
      if (actual != listed) {
        fs::remove(staged, ec);
        die("the downloaded " + asset_name() + " does not match the checksum this release published.\n"
            "  published: " + listed + "\n"
            "  downloaded: " + actual + "\n"
            "  nothing was changed. that is a corrupted download, a mirror serving something\n"
            "  else, or a release nobody signed off on -- try again, or build the source:\n"
            "    " + CONFIG + " update --to " + tag + "  (with SIMPLEDIR_RELEASE_URL pointed at a mirror you trust)");
      }
      out << "  verified against the published SHA256SUMS\n";
    } else {
      out << "  no published checksum for " << asset_name() << ", so it could not be verified\n";
    }
  }

  // Say where it actually came from. Only worth a line when that isn't obvious:
  // a source fallback is normal for the very old releases and alarming otherwise.
  if (from_source) {
    out << "  that release has no binary attached, so this is the source file from"
              << " the tag\n";
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
    out << "  that asset is v" << got_version << ", same as what you have."
              << " not changing anything\n";
    return 0;
  }

  // Running the download with --version is how we know it's the right program, so
  // use it for this too: the v1.0.0 tag was cut from a commit whose VERSION
  // already said 2.0.0, so `--to v1.0.0` installs a file that calls itself 2.0.0.
  // Installing what the tag actually contains is right; saying nothing about it
  // is not.
  std::string wanted = tag == "latest" ? "" : trim(tag);
  if (!wanted.empty() && starts_with(wanted, "v")) wanted = wanted.substr(1);
  if (!wanted.empty() && got_version != wanted) {
    out << "  note: you asked for " << tag << ", and that release's own --version says "
              << got_version << ".\n"
              << "  the tag was cut from a commit whose version string had already moved on."
              << " installing what\n"
              << "  " << tag << " actually contains.\n";
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
    out << "  note: v" << got_version << " calls itself `simpledir`, not `" << MOVE
              << "`. it predates the two-command split,\n"
              << "  so there is no `" << CONFIG << "` half in this version.\n";
  }
  // Going back past v6.0.0 leaves a shell that cannot undo it. Lead with the one
  // command that needs nothing: the binary we just replaced is sitting right
  // there. Telling someone to curl a script off the internet to undo a local
  // change is three steps and a network where zero would do.
  if (got < 6.0) {
    out << "  heads up: v" << got_version << " is the old python build. it has no `"
              << CONFIG << " revert` and no `update --to`,\n"
              << "  so it cannot bring you back here.\n";
    // Never promise a file that isn't there. install_release writes .previous on
    // the way in, so it normally is — but "normally" isn't good enough for the
    // one instruction someone is about to paste into a shell.
    if (path_exists(target + ".previous")) {
      out << "  to undo this, in a new shell:\n\n"
                << "    cp " << target << ".previous " << target << "\n";
    } else {
      out << "  to undo this, re-run the installer:\n"
                << "    curl -fsSL https://raw.githubusercontent.com/" << g_repo
                << "/main/install.sh | bash\n";
    }
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
    out << CONFIG << " " << VERSION << " installed, installing " << want << " over "
              << install_target() << "\n";
    int installed = install_release(want, true);
    if (installed)
      out << "  done. the previous one is at " << install_target() << ".previous\n";
    out << "  new shell needed if " << MOVE << " gained subcommands\n";
    return 0;
  }

  auto found = releases(1);
  if (found.empty())
    die("couldn't reach GitHub to check for updates.\n"
        "  the unauthenticated api allows 60 requests an hour per address; wait, or\n"
        "  install a version directly: " + CONFIG + " update --to v6.0.0\n"
        "  releases are also listed at " + g_repo + "/releases");
  std::string latest = found.front().tag;
  out << CONFIG << " " << VERSION << " installed, newest release is " << latest << "\n";

  double have = std::atof(VERSION);
  double newest = std::atof(latest.c_str() + latest.find_first_not_of("v"));
  if (newest <= have) {
    out << "you're up to date\n";
    return 0;
  }
  if (args.has("check")) {
    out << "update available: " << latest << " (exit 1 means 'there is one')\n";
    return 1;
  }

  std::string target = install_target();
  if (!args.has("yes")) {
    if (!isatty(STDIN_FILENO)) {
      out << "  not a terminal, so not asking. install it with: " << CONFIG << " update --yes\n";
      return 1;
    }
    out << "  install " << latest << " over " << target << "? [y/N] "; out.flush();
    std::string answer;
    if (!read_line(answer) || lower(trim(answer)) != "y") {
      out << "  ok, leaving it alone\n";
      return 0;
    }
  }
  int installed = install_release(latest, false);
  if (installed) {
    out << "updated to " << latest.substr(1) << " at " << target << "\n";
    out << "  the old one is kept at " << target << ".previous. go back: " << CONFIG
              << " revert\n";
  }
  out << "  new shell needed if " << MOVE << " gained subcommands\n";
  return 0;
}

// Put back whatever was installed before the last update.
int cmd_revert(const Args& args) {
  std::string target = install_target();
  std::string previous = target + ".previous";

  if (args.saw("to")) {
    std::string want = normalize_tag(args.value("to"));
    install_release(want, true);
    out << "now running " << want << " at " << target << "\n";
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
    out << "nothing to do: " << previous << " is the same version you're already on ("
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
  out << "back to " << reported << ", installed at " << target << "\n";
  out << "  (it was " << VERSION << ". new shell needed if " << MOVE << " gained subcommands)\n";
  return 0;
}

int cmd_releases(const Args& args) {
  (void)args;
  auto found = releases(15);
  if (found.empty()) die("couldn't reach GitHub to list releases. try again, or see " + g_repo);
  out << "published releases:\n\n";
  for (const Release& r : found) {
    std::string mark = r.tag == "v" + std::string(VERSION) ? "  <- you are here" : "";
    out << "  " << r.tag << "  " << r.published << std::string(9, ' ') << r.name << mark
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
    err << MOVE << ": v" << bare << " is out (you're on v" << VERSION << "). `" << CONFIG
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
  for (const std::string name : {"/.bashrc", "/.zshrc", "/.config/fish/config.fish"}) {
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
  out << "if ! command -v " << MOVE << " >/dev/null 2>&1; then export PATH=\"" << bindir
            << ":$PATH\"; fi;\n"
            // How you arrived, for `sdcfg prompt` to render. The shell owns these
            // because a subprocess cannot set them, same reason it owns the cd.
            // SD_JUMPED_TO is what makes a plain `cd` clear the marker on its own:
            // the prompt only shows the segment while $PWD still matches, so
            // nothing has to hook or override cd.
            << "export SD_ALIAS=;\n"
            << "export SD_JUMPED_TO=;\n"
            << "_sd_jump() {\n"
            << "  if [ -z \"${1-}\" ]; then SD_ALIAS=; SD_JUMPED_TO=; export SD_ALIAS SD_JUMPED_TO;"
               " builtin cd -- \"$HOME\" && return $?; fi;\n"
            << "  if [ \"${1-}\" = \"-\" ]; then SD_ALIAS=; SD_JUMPED_TO=; export SD_ALIAS"
               " SD_JUMPED_TO; builtin cd -- \"$OLDPWD\" && return $?; fi;\n"
            << "  if [ \"${1:0:1}\" = \"/\" ]; then SD_ALIAS=; SD_JUMPED_TO=; export SD_ALIAS"
               " SD_JUMPED_TO; builtin cd -- \"$1\" && return $?; fi;\n"
            << "  if [ \"${1:0:1}\" = \"~\" ]; then SD_ALIAS=; SD_JUMPED_TO=; export SD_ALIAS"
               " SD_JUMPED_TO; builtin cd -- \"${1/#\\~/$HOME}\" && return $?; fi;\n"
            << "  local _sd_dir;\n"
            << "  _sd_dir=$(command " << MOVE << " print \"$@\") || return $?;\n"
            << "  builtin cd -- \"$_sd_dir\" || return $?;\n"
            << "  SD_ALIAS=$1; SD_JUMPED_TO=$_sd_dir; export SD_ALIAS SD_JUMPED_TO;\n"
            << "  return 0;\n"
            << "};\n"
            << MOVE << "() {\n"
            // Every read-only verb has to be listed here. It is a literal in the
            // generated block, so adding a verb and forgetting this is the easy
            // mistake, and the symptom is `sd adapt` reporting "no alias named
            // 'adapt'". A test derives the list from the help text.
            << "  if [ \"${1-}\" = \"ls\" ] || [ \"${1-}\" = \"i\" ] || [ \"${1-}\" = \"print\" ]"
               " || [ \"${1-}\" = \"top\" ] || [ \"${1-}\" = \"suggest\" ]"
               " || [ \"${1-}\" = \"adapt\" ]"
               " || [ \"${1-}\" = \"sha256\" ] || "
               "([ \"${1:0:1}\" = \"@\" ]) || "
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

// The prompt segment. `sd` records how you arrived (SD_ALIAS, SD_JUMPED_TO) and
// this renders it, but only while you are still in the directory it took you to.
// Comparing $PWD against SD_JUMPED_TO is what lets a plain `cd` clear the marker
// with no hook and no overridden cd: move elsewhere and the segment stops
// matching on its own.
int cmd_prompt(const std::string& shell) {
  std::string colour = env_or("SD_PROMPT_COLOR", "38;5;110");
  if (colour.find(';') == std::string::npos && colour.find("38;5;") != 0)
    throw UsageError("SD_PROMPT_COLOR wants an SGR sequence like 38;5;110");

  if (shell == "bash") {
    out << "# simpledir prompt segment for bash. add it to ~/.bashrc:\n"
              << "#   eval \"$(" << CONFIG << " prompt bash)\"\n"
              << "# shows the alias you jumped by, in " << colour
              << ", while you're still in the directory it took you to.\n"
              << "# change that colour with SD_PROMPT_COLOR, e.g. 38;5;213\n"
              << "_SD_BASE_PS1=\"\";\n"
              << "_SD_ESC=$'\\033';\n"
              << "_sd_prompt() {\n"
              << "  if [ -z \"$_SD_BASE_PS1\" ]; then _SD_BASE_PS1=\"$PS1\"; fi;\n"
              << "  SD_SEG=\"\";\n"
              << "  if [ -n \"${SD_ALIAS-}\" ] && [ \"${SD_JUMPED_TO-}\" = \"$PWD\" ]; then\n"
              << "    SD_SEG=\"$_SD_ESC[" << colour << "m$SD_ALIAS$_SD_ESC[0m \";\n"
              << "  fi;\n"
              << "  PS1=\"$SD_SEG$_SD_BASE_PS1\";\n"
              << "};\n"
              << "if [ -z \"${_SD_PROMPT_ON-}\" ]; then\n"
              << "  _SD_PROMPT_ON=1;\n"
              << "  PROMPT_COMMAND=\"_sd_prompt${PROMPT_COMMAND:+; $PROMPT_COMMAND}\";\n"
              << "fi;\n";
  } else if (shell == "zsh") {
    out << "# simpledir prompt segment for zsh. add it to ~/.zshrc:\n"
              << "#   eval \"$(" << CONFIG << " prompt zsh)\"\n"
              << "# shows the alias you jumped by, in " << colour
              << ", while you're still in the directory it took you to.\n"
              << "# change that colour with SD_PROMPT_COLOR, e.g. 38;5;213\n"
              << "typeset -g _SD_ESC=$'\\033';\n"
              << "_sd_prompt() {\n"
              << "  local seg=\"\";\n"
              << "  if [[ -n \"${SD_ALIAS-}\" && \"${SD_JUMPED_TO-}\" == \"$PWD\" ]]; then\n"
              << "    seg=\"$_SD_ESC[" << colour << "m${SD_ALIAS}$_SD_ESC[0m \";\n"
              << "  fi;\n"
              << "  PS1=\"${seg}${PS1}\";\n"
              << "};\n"
              << "typeset -gaU precmd_functions;\n"
              << "if (( ! ${precmd_functions[(I)_sd_prompt]} )); then\n"
              << "  precmd_functions+=(sd_prompt);\n"
              << "fi;\n";
  } else {
    throw UsageError("prompt takes bash or zsh");
  }
  return 0;
}

int cmd_completions(const std::string& shell) {
  if (shell == "bash") {
    out << "# simpledir bash completion. install it with:\n"
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
              << "  local cur verbs=\"add rm rename import bind tag forget adapt migrate bench zoxide prompt edit init "
                 "completions update revert releases uninstall doctor\"\n"
              << "  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
              << "  COMPREPLY=( $(compgen -W \"$verbs\" -- \"$cur\") )\n"
              << "}\n"
              << "complete -F _" << CONFIG << "_complete " << CONFIG << "\n";
  } else {
    out << "#compdef " << MOVE << " " << CONFIG << "\n"
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
              << "  local -a verbs=(add rm rename import bind forget migrate zoxide prompt edit init "
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
    out << "note: removing " << target << ", not the copy you're running (" << running << ")\n";
  }

  if (!cfg.aliases.empty() && !args.has("purge")) {
    out << "you have " << cfg.aliases.size() << " alias"
              << (cfg.aliases.size() == 1 ? "" : "es") << " in " << g_config_file << "\n"
              << "  these stay. to remove them too: " << CONFIG << " uninstall --purge\n";
  }

  if (!args.has("yes")) {
    if (!isatty(STDIN_FILENO)) die("not a terminal, so not asking. re-run with --yes");
    out << "  remove " << target << " and the wrapper from your shell rc? [y/N] "; out.flush();
    std::string answer;
    if (!read_line(answer) || lower(trim(answer)) != "y") {
      out << "  ok, nothing changed\n";
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
  if (touched.empty()) out << "no wrapper block found in any shell rc\n";

  std::error_code ec;
  if (path_exists(target)) {
    if (fs::remove(target, ec)) {
      out << "removed " << target << "\n";
    } else {
      out << "couldn't remove " << target << ": " << ec.message() << "\n"
                << "  remove it by hand: rm " << target << "\n";
    }
  } else {
    out << "nothing to remove at " << target << "\n";
  }
  // sdcfg lives *next to* the sd we just removed, and nowhere else. a
  // hardcoded ~/.local/bin/sdcfg here ignores SIMPLEDIR_BIN entirely, so a test
  // run with a scoped SIMPLEDIR_BIN deleted the developer's real symlink — and
  // did so quietly, twice, before anybody worked out why it kept vanishing.
  fs::remove(fs::path(target).parent_path() / "sdcfg", ec);

  for (const std::string& rc : touched) out << "cleaned the wrapper from " << rc << "\n";

  if (args.has("purge")) {
    fs::remove_all(g_config_dir, ec);
    out << "deleted " << g_config_dir << " and every alias in it\n";
  } else {
    out << "aliases kept in " << g_config_file << "\n";
  }
  out << "open a new shell, or `exec bash`, to drop the old functions\n";
  return 0;
}

int cmd_doctor() {
  int problems = 0;
  auto ok = [](const std::string& m) { out << "  \033[32mok\033[0m    " << m << "\n"; };
  auto bad = [&](const std::string& m) { problems++; out << "  \033[31mproblem\033[0m " << m << "\n"; };
  auto note = [](const std::string& m) { out << "  \033[2mnote\033[0m    " << m << "\n"; };

  out << CONFIG << " doctor - version " << VERSION << "\n";
  out << "runtime\n";
  ok(std::string("built with ") + __VERSION__);

  out << "config\n";
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

  out << "shell\n";
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

  out << "history\n";
  auto history = read_history();
  if (history.empty()) {
    note("empty. " + MOVE + " remembers where you go; " + CONFIG + " forget clears it");
  } else {
    ok(std::to_string(history.size()) + " directories remembered");
    ok("recording is on");
  }
  if (std::getenv("SIMPLEDIR_NO_HISTORY")) note("recording is off (SIMPLEDIR_NO_HISTORY)");

  out << "\n";
  if (problems) {
    out << problems << " problem" << (problems == 1 ? "" : "s") << " found\n";
    return 1;
  }
  out << "everything looks fine\n";
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
    "  sd adapt [--top N]           directories you keep visiting that have no name\n"
    "  sd sha256 <file>             hash a file, to check a download against its release\n"
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
    "  sdcfg tag @name <alias>...  group aliases; `sd @name` lists them\n"
    "  sdcfg project add <n> [p]  an alias that follows you into a project\n"
    "  sdcfg adapt [--accept <n>] remember what you said about a suggestion\n"
    "  sdcfg export [<file>]     write a portable config; $HOME becomes ~\n"
    "  sdcfg adopt <file>        merge one back in\n"
    "  sdcfg forget                 drop directories from the visit log\n"
    "  sdcfg migrate                migrate the config file to the current version\n"
    "  sdcfg zoxide                 import the directories zoxide knows about\n"
    "  sdcfg edit [<editor>]        open the config in $EDITOR\n"
    "  sdcfg init                   print the shell wrapper\n"
    "  sdcfg prompt bash|zsh        print the prompt segment\n"
    "  sdcfg completions bash|zsh   print a completion script\n"
    "  sdcfg update                 check for a newer release and install it\n"
    "  sdcfg revert                 go back to the previously installed version\n"
    "  sdcfg releases               list the published releases\n"
    "  sdcfg uninstall              remove the binaries and the wrapper\n"
    "  sdcfg doctor                 check the install\n"
    "  sdcfg bench [--runs N]    how long a jump takes on this machine\n"
    "\n"
    "  sdcfg --help                 this text\n"
    "  sdcfg --version              print the version and exit\n"
    "\n"
    "  to move, use sd: sd <alias>, sd ls, sd i, sd sha256, ...\n"
    "  flags are long-form only: --force --dry-run --keep-symlinks --purge --yes\n"
    "\n"
    "  `migrate` moves your config file from version 1 to version 2. `update`\n"
    "  replaces the program itself. different things: neither calls the other.\n";

int run_move(const std::vector<std::string>& argv) {
  // `sd @work` and `sd @work dots` address a tag. Handled before the fast path,
  // because a tag is never an alias and must never be resolved as one.
  if (!argv.empty() && starts_with(argv[0], TAG_PREFIX) &&
      !std::set<std::string>({"--help", "--version"}).count(argv[0])) {
    Config cfg = load_config();
    std::string tag_name, rest;
    std::vector<std::string> members;
    Args probe;
    probe.words = argv;
    if (!split_tag(probe, cfg, tag_name, members, rest))
      throw UserError("'" + argv[0] + "' isn't a tag. " + CONFIG + " tag lists the ones you have");
    if (rest.empty()) return cmd_tag_list(cfg, tag_name, members);
    // `sd @work dots` resolves exactly like `sd dots`, but only inside the tag
    if (std::find(members.begin(), members.end(), rest) == members.end()) {
      std::string msg = rest + " isn't in " + tag_name + ".";
      msg += " it has:";
      for (const std::string& m : members) msg += " " + m;
      msg += "\n  add it: " + CONFIG + " tag " + tag_name + " " + rest;
      throw UserError(msg);
    }
    out << jump(rest, cfg, now_seconds()) << "\n";
    return 0;
  }
  // fast path: `sd <word>` and `sd --version` are what run on every prompt
  if (argv.size() == 1) {
    if (argv[0] == "--version") {
      out << MOVE << " " << VERSION << " - " << TAGLINE << "\n";
      return 0;
    }
    if (argv[0] == "--help") {
      out << MOVE_HELP;
      return 0;
    }
    if (!starts_with(argv[0], "-") && !move_verbs().count(argv[0])) {
      Config cfg = load_config();
      out << jump(argv[0], cfg, now_seconds()) << "\n";
      return 0;
    }
  }
  if (argv.empty()) {
    out << MOVE_HELP;
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
  if (verb == "adapt") {
    Args args = parse_args(rest);
    for (const auto& flag : args.flags) {
      if (!std::set<std::string>({"json", "top", "all"}).count(flag.first))
        throw UsageError("unknown flag '--" + flag.first + "' for `sd adapt`. try --top, --all, --json");
    }
    return cmd_adapt(args);
  }
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
    out << jump(args.words[0], cfg, now_seconds()) << "\n";
    return 0;
  }
  if (verb == "sha256") return cmd_sha256(parse_args(rest));
  static const std::set<std::string> config_verbs = {
      "add", "rm", "rename", "import", "bind", "tag", "project", "adapt", "forget", "migrate", "bench",
      "zoxide", "prompt", "export", "adopt",
      "edit", "init", "completions", "update", "revert", "releases", "uninstall", "doctor"};
  if (config_verbs.count(verb)) {
    throw UsageError("'" + verb + "' is not an " + MOVE + " command.\n  did you mean `" + CONFIG +
                      " " + verb + "`? " + MOVE + " only reads your config.");
  }
  throw UsageError("'" + verb + "' is not an " + MOVE + " command. try `" + MOVE + " --help`");
}

int run_config(const std::vector<std::string>& argv) {
  if (argv.empty()) {
    out << CONFIG_HELP;
    return 0;
  }
  const std::string verb = argv[0];
  std::vector<std::string> rest(argv.begin() + 1, argv.end());
  Args args = parse_args(rest);

  static const std::set<std::string> known = {
      "add", "rm", "rename", "import", "bind", "tag", "project", "adapt", "forget", "migrate", "bench",
      "zoxide", "prompt", "export", "adopt",
      "edit", "init", "completions", "update", "revert", "releases", "uninstall", "doctor"};

  if (verb == "--version") {
    out << CONFIG << " " << VERSION << " - " << TAGLINE << "\n";
    return 0;
  }
  if (verb == "--help") {
    out << CONFIG_HELP;
    return 0;
  }
  if (!known.count(verb)) {
    // reaching for the wrong half is the commonest mistake; help if we can
    static const std::set<std::string> move_verbs = {"ls", "print", "suggest", "i", "top", "adapt"};
    if (move_verbs.count(verb)) {
      err << "did you mean `" << MOVE << " " << verb << "`? " << CONFIG
                << " only changes things.\n";
    }
    throw UsageError("'" + verb + "' is not an " + CONFIG + " command. try `" + CONFIG + " --help`");
  }

  if (verb == "add") return cmd_add(args);
  if (verb == "rm") return cmd_rm(args);
  if (verb == "rename") return cmd_rename(args);
  if (verb == "import") return cmd_import(args);
  if (verb == "bind") return cmd_bind(args);
  if (verb == "tag") return cmd_tag(args);
  if (verb == "project") return cmd_project(args);
  if (verb == "export") return cmd_export(args);
  if (verb == "adopt") return cmd_adopt(args);
  if (verb == "adapt") return cmd_adapt_apply(args);
  if (verb == "forget") return cmd_forget(args);
  if (verb == "migrate") return cmd_migrate(args);
  if (verb == "zoxide") return cmd_zoxide(args);
  if (verb == "update") return cmd_update(args);
  if (verb == "revert") return cmd_revert(args);
  if (verb == "releases") return cmd_releases(args);
  if (verb == "uninstall") return cmd_uninstall(args);
  if (verb == "doctor") return cmd_doctor();
  if (verb == "bench") return cmd_bench(args);

  if (verb == "init") return cmd_init();
  if (verb == "prompt") return cmd_prompt(args.word(0));
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
  // Unreachable in a correct build: `known` above is the list of verbs and every
  // one of them has a line above. It used to be a bare `return 0`, which means
  // adding a verb to `known` and forgetting the dispatch made it exit 0 having
  // done nothing -- silently, which is the one thing this program never does.
  // `sdcfg bench` did exactly that for a while and nothing caught it.
  die("'" + verb + "' is listed as a command but nothing handles it.\n"
      "  that is a bug in " + MOVE + ", not something you did wrong: " + CONFIG + " " + verb +
      " should have done something.");
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
    out.flush();
    return status;
  } catch (const UsageError& error) {
    out.flush();
    err << g_prog << ": " << error.what() << "\n";
    return 2;
  } catch (const UserError& error) {
    out.flush();
    err << g_prog << ": " << error.what() << "\n";
    return 1;
  } catch (const std::exception& error) {
    out.flush();
    err << g_prog << ": " << error.what() << "\n";
    return 1;
  }
}
