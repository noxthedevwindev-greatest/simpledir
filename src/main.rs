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
//   config.json   hand-editable alias -> path map, version 3
//   history.json  machine-written frecency counts, not yours to edit
//
// The `print` verb writes a directory to stdout; the shell wrapper that
// `sdcfg init` emits performs the cd, because a subprocess cannot change
// the calling shell's cwd.
//
// Single file, no crates. Linux. Build with:
//   rustc --edition 2021 -O -C strip=symbols -C target-feature=+crt-static -o sd src/main.rs

use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

// Kept in two pieces so `make audit-tags` and the test suite can read the
// declared release without parsing Rust. Tests set SIMPLDIR_VERSION at compile
// time to build stub binaries that claim an older or newer version.
const VERSION_BASE: &str = "13.0.0";
const VERSION: &str = match option_env!("SIMPLDIR_VERSION") {
    Some(v) => v,
    None => VERSION_BASE,
};
const TAGLINE: &str = "see it all, tidy it up";
const CONFIG_VERSION: i64 = 3;

const HALF_LIFE: f64 = 3.0 * 24.0 * 3600.0;
const VISIT_THROTTLE: f64 = 60.0;
const FLOOR: f64 = 0.02;
const MAX_HISTORY: usize = 500;
const PROJECT_FILE: &str = ".simpledir.json";
const MOVE: &str = "sd";
const LEGACY_ID: &str = "simpledir";
const CONFIG: &str = "sdcfg";
const TAG_PREFIX: &str = "@";
const OWNER: &str = "noxthedevwindev-greatest";
const REPO: &str = "simpledir";
const API: &str = "https://api.github.com/repos/noxthedevwindev-greatest/simpledir/releases";
const RELEASES: &str = "https://github.com/noxthedevwindev-greatest/simpledir/releases";
const RAW_BASE: &str = "https://raw.githubusercontent.com/";
const MARK_BEGIN: &str = "# >>> simpledir >>>";
const MARK_END: &str = "# <<< simpledir <<<";

unsafe extern "C" {
    fn isatty(fd: i32) -> i32;
    fn write(fd: i32, buf: *const u8, count: usize) -> isize;
    fn getpid() -> i32;
    fn getuid() -> u32;
}

// ------------------------------------------------------------------- output

struct Out {
    fd: i32,
    tty: bool,
    buf: Vec<u8>,
}

impl Out {
    fn new(fd: i32) -> Self {
        Self {
            fd,
            tty: unsafe { isatty(fd) == 1 },
            buf: Vec::new(),
        }
    }

    fn put(&mut self, bytes: &[u8]) {
        self.buf.extend_from_slice(bytes);
        if !self.tty || self.buf.len() >= 8192 || (bytes.len() == 1 && bytes[0] == b'\n') {
            self.flush();
        }
    }

    fn flush(&mut self) {
        let mut done = 0usize;
        while done < self.buf.len() {
            let n = unsafe { write(self.fd, self.buf[done..].as_ptr(), self.buf.len() - done) };
            if n > 0 {
                done += n as usize;
            } else if n < 0 {
                // EINTR is the only reason to try the same bytes again. EPIPE and
                // friends mean nobody is listening; that is not worth an error.
                continue;
            } else {
                break;
            }
        }
        self.buf.clear();
    }
}

static OUT: Mutex<Option<Out>> = Mutex::new(None);
static ERR: Mutex<Option<Out>> = Mutex::new(None);

fn out(s: &str) {
    if let Ok(mut slot) = OUT.lock() {
        if let Some(buffer) = slot.as_mut() {
            buffer.put(s.as_bytes());
        }
    }
}

fn err(s: &str) {
    if let Ok(mut slot) = ERR.lock() {
        if let Some(buffer) = slot.as_mut() {
            buffer.put(s.as_bytes());
            // Errors have to land in order, between lines of something else.
            buffer.flush();
        }
    }
}

fn flush_out() {
    if let Ok(mut slot) = OUT.lock() {
        if let Some(buffer) = slot.as_mut() {
            buffer.flush();
        }
    }
}

fn flush_err() {
    if let Ok(mut slot) = ERR.lock() {
        if let Some(buffer) = slot.as_mut() {
            buffer.flush();
        }
    }
}

// ------------------------------------------------------------------- errors

#[derive(Debug, Clone)]
enum AppError {
    User(String),
    Usage(String),
}

type Res<T> = Result<T, AppError>;

fn user(msg: impl Into<String>) -> AppError {
    AppError::User(msg.into())
}

fn usage(msg: impl Into<String>) -> AppError {
    AppError::Usage(msg.into())
}

fn die(msg: impl Into<String>) -> AppError {
    user(msg)
}

// Only "no alias named ..." may fall through to frecency. An ambiguous prefix
// or an alias whose directory is gone must say so, not silently jump to an
// unrelated directory that happens to match the same word in the log.
fn is_unknown_alias(err: &AppError) -> bool {
    let msg = match err {
        AppError::User(m) | AppError::Usage(m) => m,
    };
    msg.starts_with("no alias named '")
}

// --------------------------------------------------------------- program mode

fn prog() -> &'static str {
    // main sets this once before anything else can fail.
    PROG.with(|p| p.get())
}

std::thread_local! {
    static PROG: std::cell::Cell<&'static str> = const { std::cell::Cell::new(MOVE) };
}

fn set_prog(s: &'static str) {
    PROG.with(|p| p.set(s));
}

fn move_verbs() -> BTreeSet<&'static str> {
    [
        "ls", "i", "print", "top", "recent", "stats", "suggest", "adapt", "sha256",
    ]
    .into_iter()
    .collect()
}

fn config_verbs() -> BTreeSet<&'static str> {
    [
        "add", "rm", "rename", "import", "bind", "tag", "project", "adapt", "forget", "tidy",
        "migrate", "bench", "zoxide", "prompt", "export", "adopt", "edit", "init", "completions",
        "update", "revert", "releases", "uninstall", "doctor",
    ]
    .into_iter()
    .collect()
}

const MOVE_HELP: &str = r#"the next zoxide. move between the directories you named.

usage: sd <alias>              jump to a directory you bound
       sd -                    previous directory
       sd                      $HOME
       sd /some/path           a raw path, cd'd as-is

  sd ls [<query>] [--long]     list what you can jump to
  sd ls --names                one alias per line, for scripts
  sd ls --json                 the same shape as the config file
  sd print <alias[/sub]>       print the directory, don't cd
  sd top [<query>] [--json]    the frecency log, best first
  sd recent [<query>] [--top N]  most recent visits, newest first
  sd recent --json             the same list as JSON
  sd stats [--json]            aliases, tags and history at a glance
  sd suggest [<query>]         directories from your history worth naming
  sd suggest --json            as JSON
  sd i [<query>]               interactive picker (fzf if installed)
  sd adapt [--top N]           directories you keep visiting that have no name
  sd sha256 <file>             hash a file, to check a download against its release

  sd --help                    this text
  sd --version                 print the version and exit

  sd only reads your config. to change things, use sdcfg:
    sdcfg add, rm, rename, import, bind, edit, doctor, ...

  a unique name prefix is enough: `sd hy` finds `hypr`.
  flags are long-form only.
"#;

const CONFIG_HELP: &str = r#"change your directory shortcuts. this command never moves you.

usage: sdcfg <command> [arguments]

  sdcfg add [<name>] [<path>]  bind a name; defaults to the dir's own name and $PWD
  sdcfg rm <name>              unbind
  sdcfg rename <old> <new>     rename, keeping the path; --tags carries tags
  sdcfg import <dir>           bind every subdirectory of a tree at once
  sdcfg bind                   name everything `sd suggest` found
  sdcfg tag [@name] <alias>... group aliases; @old --rename @new renames a tag
  sdcfg project [add|rm]       an alias that follows you into a project
  sdcfg adapt [--accept <n>] remember what you said about a suggestion
  sdcfg export [<file>]     write a portable config; $HOME becomes ~
  sdcfg adopt <file>        merge one back in
  sdcfg forget                 drop directories from the visit log
  sdcfg tidy [--dry-run]       prune dead aliases, tags and faded history
  sdcfg migrate                migrate the config file to the current version
  sdcfg zoxide                 import the directories zoxide knows about
  sdcfg edit [<editor>]        open the config in $EDITOR
  sdcfg init                   print the shell wrapper
  sdcfg prompt bash|zsh        print the prompt segment
  sdcfg completions bash|zsh   print a completion script
  sdcfg update                 check for a newer release and install it
  sdcfg revert                 go back to the previously installed version
  sdcfg releases               list the published releases
  sdcfg uninstall              remove the binaries and the wrapper
  sdcfg doctor                 check the install
  sdcfg bench [--runs N]    how long a jump takes on this machine

  sdcfg --help                 this text
  sdcfg --version              print the version and exit

  to move, use sd: sd <alias>, sd ls, sd i, sd sha256, ...
  flags are long-form only: --force --dry-run --keep-symlinks --purge --yes

  `migrate` moves your config file from v1/v2 to the current version (v3).
  `update` replaces the program itself. different things: neither calls the other.
"#;

// ------------------------------------------------------------------ strings

fn now_seconds() -> f64 {
    match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(d) => d.as_secs() as f64,
        Err(e) => -(e.duration().as_secs() as f64),
    }
}

fn stamp() -> String {
    let output = Command::new("date").arg("+%Y%m%d%H%M%S").output();
    match output {
        Ok(o) => String::from_utf8_lossy(&o.stdout).trim().to_string(),
        Err(_) => format!("{}", now_seconds() as i64),
    }
}

fn trim(s: &str) -> String {
    s.trim_matches(|c: char| c == ' ' || c == '\t' || c == '\r' || c == '\n')
        .to_string()
}

fn lower(s: &str) -> String {
    s.to_lowercase()
}

fn contains(hay: &str, needle: &str) -> bool {
    hay.contains(needle)
}

fn contains_ci(hay: &str, needle: &str) -> bool {
    contains(&lower(hay), &lower(needle))
}

fn starts_with(s: &str, prefix: &str) -> bool {
    s.starts_with(prefix)
}

fn ends_with(s: &str, suffix: &str) -> bool {
    s.ends_with(suffix)
}

fn split(s: &str, sep: char) -> Vec<String> {
    let mut parts: Vec<String> = s.split(sep).map(|p| p.to_string()).collect();
    if s.is_empty() || s.ends_with(sep) {
        if !parts.is_empty() {
            parts.pop();
        }
    }
    parts
}

fn shell_split(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut have = false;
    let mut squote = false;
    let mut dquote = false;
    let mut esc = false;
    for c in s.chars() {
        if esc {
            cur.push(c);
            have = true;
            esc = false;
            continue;
        }
        if c == '\\' && !squote {
            esc = true;
            continue;
        }
        if c == '\'' && !dquote {
            squote = !squote;
            have = true;
            continue;
        }
        if c == '"' && !squote {
            dquote = !dquote;
            have = true;
            continue;
        }
        if c.is_whitespace() && !squote && !dquote {
            if have {
                out.push(std::mem::take(&mut cur));
                have = false;
            }
            continue;
        }
        cur.push(c);
        have = true;
    }
    if have {
        out.push(cur);
    }
    out
}

fn shell_quote(v: &str) -> String {
    let safe = |c: char| c.is_ascii_alphanumeric() || "@%+=:,./-_".contains(c);
    if !v.is_empty() && v.chars().all(safe) {
        return v.to_string();
    }
    let mut out = String::from("'");
    for c in v.chars() {
        if c == '\'' {
            out.push_str("'\\''");
        } else {
            out.push(c);
        }
    }
    out.push('\'');
    out
}

fn join(parts: &[String], sep: &str) -> String {
    parts.join(sep)
}

fn num_cmp(a: f64, b: f64) -> std::cmp::Ordering {
    a.partial_cmp(&b).unwrap_or(std::cmp::Ordering::Equal)
}

fn relative_age(now: f64, at: f64) -> String {
    let seconds = (now - at).max(0.0);
    if seconds < 60.0 {
        "just now".to_string()
    } else if seconds < 3600.0 {
        format!("{}m ago", (seconds / 60.0) as i64)
    } else if seconds < 86400.0 {
        format!("{}h ago", (seconds / 3600.0) as i64)
    } else if seconds < 7.0 * 86400.0 {
        format!("{}d ago", (seconds / 86400.0) as i64)
    } else {
        format!("{}w ago", (seconds / (7.0 * 86400.0)) as i64)
    }
}

fn read_line() -> Option<String> {
    let mut line = String::new();
    match io::stdin().read_line(&mut line) {
        Ok(0) => None,
        Ok(_) => {
            while line.ends_with('\n') || line.ends_with('\r') {
                line.pop();
            }
            Some(line)
        }
        Err(_) => None,
    }
}

// ----------------------------------------------------------------- filesystem

fn env_or(name: &str, fallback: &str) -> String {
    match env::var(name) {
        Ok(v) if !v.is_empty() => v,
        _ => fallback.to_string(),
    }
}

fn home_dir() -> String {
    if let Ok(h) = env::var("HOME") {
        if !h.is_empty() {
            return h;
        }
    }
    let uid = unsafe { getuid() }.to_string();
    if let Ok(passwd) = fs::read_to_string("/etc/passwd") {
        for line in passwd.lines() {
            let fields = split(line, ':');
            if fields.len() >= 6 && fields[2] == uid {
                return fields[5].clone();
            }
        }
    }
    ".".to_string()
}

fn config_dir() -> String {
    env_or("SIMPLEDIR_CONFIG_DIR", &(home_dir() + "/.simpledir"))
}

fn config_file() -> String {
    config_dir() + "/config.json"
}

fn history_file() -> String {
    config_dir() + "/history.json"
}

fn expand(raw: &str) -> String {
    let bytes = raw.as_bytes();
    let mut out = String::new();
    let mut i = 0usize;
    while i < bytes.len() {
        if bytes[i] == b'$' && i + 1 < bytes.len() {
            let mut j = i + 1;
            let braced = bytes[j] == b'{';
            if braced {
                j += 1;
            }
            let start = j;
            while j < bytes.len() && (bytes[j].is_ascii_alphanumeric() || bytes[j] == b'_') {
                j += 1;
            }
            if j > start {
                let name = &raw[start..j];
                if let Ok(v) = env::var(name) {
                    out.push_str(&v);
                }
                i = j + if braced && j < bytes.len() && bytes[j] == b'}' { 1 } else { 0 };
                continue;
            }
        }
        // Copy one UTF-8 character at a time so paths with non-ASCII bytes survive.
        let ch = raw[i..].chars().next().unwrap();
        out.push(ch);
        i += ch.len_utf8();
    }
    if out == "~" {
        return home_dir();
    }
    if let Some(rest) = out.strip_prefix("~/") {
        return home_dir() + "/" + rest;
    }
    out
}

fn lexically_normal(path: &str) -> String {
    if path.is_empty() {
        return ".".to_string();
    }
    let absolute = path.starts_with('/');
    let mut stack: Vec<&str> = Vec::new();
    for part in path.split('/') {
        match part {
            "" | "." => {}
            ".." => {
                if let Some(last) = stack.last() {
                    if *last != ".." {
                        stack.pop();
                    } else if !absolute {
                        stack.push("..");
                    }
                } else if !absolute {
                    stack.push("..");
                }
            }
            other => stack.push(other),
        }
    }
    let joined = stack.join("/");
    if absolute {
        if joined.is_empty() { "/".to_string() } else { format!("/{joined}") }
    } else if joined.is_empty() {
        ".".to_string()
    } else {
        joined
    }
}

fn abspath(raw: &str) -> String {
    let path = expand(raw);
    let normalized = lexically_normal(&path);
    if normalized.starts_with('/') {
        return normalized;
    }
    let cwd = env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    lexically_normal(&(cwd.to_string_lossy().to_string() + "/" + &normalized))
}

fn realpath(raw: &str) -> String {
    let path = expand(raw);
    // weakly_canonical: resolve the longest existing prefix, leave the rest alone.
    let normalized = lexically_normal(&path);
    let absolute = if normalized.starts_with('/') {
        normalized
    } else {
        let cwd = env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
        lexically_normal(&(cwd.to_string_lossy().to_string() + "/" + &normalized))
    };
    let parts: Vec<&str> = absolute.split('/').filter(|p| !p.is_empty()).collect();
    let mut prefix = PathBuf::from("/");
    let mut best: Option<(String, usize)> = None;
    for (i, part) in parts.iter().enumerate() {
        prefix.push(part);
        if let Ok(canon) = fs::canonicalize(&prefix) {
            best = Some((canon.to_string_lossy().to_string(), i + 1));
        }
    }
    match best {
        Some((canon, consumed)) => {
            let rest = parts[consumed..].join("/");
            if rest.is_empty() {
                canon
            } else {
                lexically_normal(&(canon + "/" + &rest))
            }
        }
        None => absolute,
    }
}

fn as_stored(raw: &str) -> String {
    // A config value has already had $VARS expanded at add/import time. Only a
    // leading ~ is still symbolic, because exported configs use it to travel.
    // Going through abspath() here re-expanded a literal $HOME in a directory
    // name and reported a perfectly good path as missing.
    let path = if raw == "~" {
        home_dir()
    } else if let Some(rest) = raw.strip_prefix("~/") {
        home_dir() + "/" + rest
    } else {
        raw.to_string()
    };
    if path.starts_with('/') {
        return lexically_normal(&path);
    }
    let cwd = env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    lexically_normal(&(cwd.to_string_lossy().to_string() + "/" + &path))
}

fn is_dir(path: &str) -> bool {
    fs::metadata(path).map(|m| m.is_dir()).unwrap_or(false)
}

fn path_exists(path: &str) -> bool {
    Path::new(path).exists()
}

fn read_file(path: &str) -> Res<String> {
    fs::read_to_string(path).map_err(|e| user(format!("can't read {path}: {e}")))
}

fn basename_of(path: &str) -> String {
    Path::new(path)
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default()
}

fn write_atomic(path: &str, data: &str, executable: bool) -> Res<()> {
    if let Some(parent) = Path::new(path).parent() {
        let _ = fs::create_dir_all(parent);
    }
    let tmp = format!("{path}.new.{}", unsafe { getpid() });
    {
        let mut file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(&tmp)
            .map_err(|e| user(format!("can't write {tmp}: {e}")))?;
        file.write_all(data.as_bytes())
            .map_err(|_| user(format!("can't write {tmp}")))?;
        file.flush().map_err(|_| user(format!("can't write {tmp}")))?;
    }
    let mode = if executable { 0o755 } else { 0o644 };
    let _ = fs::set_permissions(&tmp, fs::Permissions::from_mode(mode));
    if let Err(e) = fs::rename(&tmp, path) {
        let _ = fs::remove_file(&tmp);
        return Err(user(format!("can't replace {path}: {e}")));
    }
    Ok(())
}

// --------------------------------------------------------------------- json

#[derive(Clone, Debug)]
enum Json {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<Json>),
    Obj(BTreeMap<String, Json>),
}

impl Json {
    fn obj() -> Self {
        Json::Obj(BTreeMap::new())
    }
    fn arr() -> Self {
        Json::Arr(Vec::new())
    }
    fn str(s: impl Into<String>) -> Self {
        Json::Str(s.into())
    }
    fn num(n: f64) -> Self {
        Json::Num(n)
    }
    fn boolean(b: bool) -> Self {
        Json::Bool(b)
    }
    fn is_obj(&self) -> bool {
        matches!(self, Json::Obj(_))
    }
    fn is_arr(&self) -> bool {
        matches!(self, Json::Arr(_))
    }
    fn is_num(&self) -> bool {
        matches!(self, Json::Num(_))
    }
    fn is_str(&self) -> bool {
        matches!(self, Json::Str(_))
    }
    fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(map) => map.get(key),
            _ => None,
        }
    }
    fn set(&mut self, key: impl Into<String>, value: Json) {
        if let Json::Obj(map) = self {
            map.insert(key.into(), value);
        }
    }
    fn as_str(&self) -> String {
        match self {
            Json::Str(s) => s.clone(),
            _ => String::new(),
        }
    }
    fn as_num(&self) -> f64 {
        match self {
            Json::Num(n) => *n,
            _ => 0.0,
        }
    }
    fn as_bool(&self) -> bool {
        match self {
            Json::Bool(b) => *b,
            _ => false,
        }
    }
}

struct JsonParser<'a> {
    s: &'a [u8],
    i: usize,
}

impl<'a> JsonParser<'a> {
    fn new(text: &'a str) -> Self {
        Self { s: text.as_bytes(), i: 0 }
    }

    fn fail<T>(&self, why: &str) -> Res<T> {
        Err(user(format!("{}: {why}", self.i)))
    }

    fn skip(&mut self) {
        while self.i < self.s.len() && self.s[self.i].is_ascii_whitespace() {
            self.i += 1;
        }
    }

    fn peek(&self) -> u8 {
        if self.i < self.s.len() { self.s[self.i] } else { 0 }
    }

    fn parse(mut self) -> Res<Json> {
        self.skip();
        let value = self.value()?;
        self.skip();
        if self.i != self.s.len() {
            return self.fail("extra text after value");
        }
        Ok(value)
    }

    fn value(&mut self) -> Res<Json> {
        self.skip();
        match self.peek() {
            b'{' => self.object(),
            b'[' => self.array(),
            b'"' => Ok(Json::Str(self.string()?)),
            b't' => {
                self.expect("true")?;
                Ok(Json::Bool(true))
            }
            b'f' => {
                self.expect("false")?;
                Ok(Json::Bool(false))
            }
            b'n' => {
                self.expect("null")?;
                Ok(Json::Null)
            }
            _ => self.number(),
        }
    }

    fn expect(&mut self, word: &str) -> Res<()> {
        if self.s.get(self.i..self.i + word.len()) != Some(word.as_bytes()) {
            return self.fail(&format!("expected {word}"));
        }
        self.i += word.len();
        Ok(())
    }

    fn object(&mut self) -> Res<Json> {
        let mut node = Json::obj();
        self.i += 1;
        self.skip();
        if self.peek() == b'}' {
            self.i += 1;
            return Ok(node);
        }
        loop {
            self.skip();
            if self.peek() != b'"' {
                return self.fail("expected a key");
            }
            let key = self.string()?;
            self.skip();
            if self.peek() != b':' {
                return self.fail("expected :");
            }
            self.i += 1;
            let value = self.value()?;
            if let Json::Obj(map) = &mut node {
                map.insert(key, value);
            }
            self.skip();
            if self.peek() == b',' {
                self.i += 1;
                continue;
            }
            if self.peek() == b'}' {
                self.i += 1;
                return Ok(node);
            }
            return self.fail("expected , or }");
        }
    }

    fn array(&mut self) -> Res<Json> {
        let mut node = Json::arr();
        self.i += 1;
        self.skip();
        if self.peek() == b']' {
            self.i += 1;
            return Ok(node);
        }
        loop {
            let value = self.value()?;
            if let Json::Arr(items) = &mut node {
                items.push(value);
            }
            self.skip();
            if self.peek() == b',' {
                self.i += 1;
                continue;
            }
            if self.peek() == b']' {
                self.i += 1;
                return Ok(node);
            }
            return self.fail("expected , or ]");
        }
    }

    fn string(&mut self) -> Res<String> {
        let mut out = String::new();
        self.i += 1;
        while self.i < self.s.len() {
            let b = self.s[self.i];
            if b == b'"' {
                self.i += 1;
                return Ok(out);
            }
            if b == b'\\' {
                self.i += 1;
                if self.i >= self.s.len() {
                    break;
                }
                let esc = self.s[self.i];
                self.i += 1;
                match esc {
                    b'n' => out.push('\n'),
                    b't' => out.push('\t'),
                    b'r' => out.push('\r'),
                    b'b' => out.push('\u{8}'),
                    b'f' => out.push('\u{c}'),
                    b'u' => {
                        if self.i + 4 > self.s.len()
                            || !self.s[self.i..self.i + 4].iter().all(|c| c.is_ascii_hexdigit())
                        {
                            return self.fail("bad \\u escape");
                        }
                        let hex = std::str::from_utf8(&self.s[self.i..self.i + 4]).unwrap_or("");
                        let mut code = u32::from_str_radix(hex, 16).unwrap_or(0);
                        self.i += 4;
                        if (0xD800..=0xDBFF).contains(&code)
                            && self.i + 6 <= self.s.len()
                            && self.s[self.i] == b'\\'
                            && self.s[self.i + 1] == b'u'
                            && self.s[self.i + 2..self.i + 6]
                                .iter()
                                .all(|c| c.is_ascii_hexdigit())
                        {
                            let low_hex =
                                std::str::from_utf8(&self.s[self.i + 2..self.i + 6]).unwrap_or("");
                            let low = u32::from_str_radix(low_hex, 16).unwrap_or(0);
                            if (0xDC00..=0xDFFF).contains(&low) {
                                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                                self.i += 6;
                            }
                        }
                        if let Some(ch) = char::from_u32(code) {
                            out.push(ch);
                        } else {
                            // The C++ parser wrote the three UTF-8 bytes of a lone
                            // surrogate directly. Rust strings cannot hold one, so
                            // its replacement character is the closest faithful form.
                            out.push('\u{FFFD}');
                        }
                    }
                    other => out.push(other as char),
                }
                continue;
            }
            if b < 0x80 {
                out.push(b as char);
                self.i += 1;
                continue;
            }
            // The input is a &str, so this is valid UTF-8. Copy one whole
            // character rather than pushing its bytes as separate chars, which
            // used to turn "café" into "cafÃ©" on every write.
            let ch = std::str::from_utf8(&self.s[self.i..])
                .ok()
                .and_then(|s| s.chars().next())
                .unwrap_or('\u{FFFD}');
            out.push(ch);
            self.i += ch.len_utf8();
        }
        self.fail("unterminated string")
    }

    fn number(&mut self) -> Res<Json> {
        let start = self.i;
        if self.peek() == b'-' || self.peek() == b'+' {
            self.i += 1;
        }
        while self.i < self.s.len()
            && (self.s[self.i].is_ascii_digit()
                || matches!(self.s[self.i], b'.' | b'e' | b'E' | b'-' | b'+'))
        {
            self.i += 1;
        }
        if start == self.i {
            return self.fail("expected a value");
        }
        let raw = std::str::from_utf8(&self.s[start..self.i]).unwrap_or("");
        // std::stod takes the longest numeric prefix; try shorter prefixes when
        // the permissive scanner swallowed something it cannot parse.
        for end in (1..=raw.len()).rev() {
            if let Ok(n) = raw[..end].parse::<f64>() {
                return Ok(Json::Num(n));
            }
        }
        self.fail("bad number")
    }
}

fn json_parse(text: &str) -> Res<Json> {
    JsonParser::new(text).parse()
}

fn json_escape(s: &str) -> String {
    let mut out = String::new();
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\t' => out.push_str("\\t"),
            '\r' => out.push_str("\\r"),
            c if (c as u32) <= 0x1f => out.push_str(&format!("\\u{:04x}", c as u32)),
            // Raw UTF-8 is legal inside a JSON string. Iterating bytes here
            // used to encode every non-ASCII path twice on every save.
            _ => out.push(c),
        }
    }
    out
}

fn json_dump_num(v: f64) -> String {
    if v == v.floor() && v.abs() < 1e15 {
        return format!("{}", v as i64);
    }
    if v == 0.0 {
        return "0".to_string();
    }
    let a = v.abs();
    if a >= 1e6 || a < 1e-4 {
        let s = format!("{v:e}");
        let (mantissa, exponent) = s.split_once('e').unwrap_or((&s, "0"));
        let mantissa = mantissa.trim_end_matches('0').trim_end_matches('.');
        let exp: i32 = exponent.parse().unwrap_or(0);
        return format!("{mantissa}e{:+03}", exp);
    }
    format!("{v}")
}

fn json_dump(node: &Json, indent: usize, depth: usize) -> String {
    let pad = " ".repeat(indent * depth);
    let pad_in = " ".repeat(indent * (depth + 1));
    let nl = if indent > 0 { "\n" } else { "" };
    match node {
        Json::Null => "null".to_string(),
        Json::Bool(b) => b.to_string(),
        Json::Num(v) => json_dump_num(*v),
        Json::Str(s) => format!("\"{}\"", json_escape(s)),
        Json::Arr(items) => {
            if items.is_empty() {
                return "[]".to_string();
            }
            let mut out = String::from("[") + nl;
            for (i, item) in items.iter().enumerate() {
                out.push_str(&pad_in);
                out.push_str(&json_dump(item, indent, depth + 1));
                if i + 1 < items.len() {
                    out.push(',');
                }
                out.push_str(nl);
            }
            out + &pad + "]"
        }
        Json::Obj(map) => {
            if map.is_empty() {
                return "{}".to_string();
            }
            let mut out = String::from("{") + nl;
            for (i, (key, value)) in map.iter().enumerate() {
                out.push_str(&pad_in);
                out.push('"');
                out.push_str(&json_escape(key));
                out.push_str("\": ");
                out.push_str(&json_dump(value, indent, depth + 1));
                if i + 1 < map.len() {
                    out.push(',');
                }
                out.push_str(nl);
            }
            out + &pad + "}"
        }
    }
}

// ---------------------------------------------------------------- config state

#[derive(Clone, Default)]
struct Config {
    version: i64,
    history: bool,
    aliases: BTreeMap<String, String>,
    tags: Vec<(String, Vec<String>)>,
}

fn tag_key(name: &str) -> String {
    if starts_with(name, TAG_PREFIX) { name.to_string() } else { TAG_PREFIX.to_string() + name }
}

fn tag_label(key: &str) -> String {
    if let Some(rest) = key.strip_prefix(TAG_PREFIX) { rest.to_string() } else { key.to_string() }
}

fn find_tag<'a>(cfg: &'a Config, name: &str) -> Option<&'a Vec<String>> {
    let key = tag_key(name);
    cfg.tags.iter().find(|(k, _)| *k == key).map(|(_, members)| members)
}

fn config_json(cfg: &Config) -> Json {
    let mut node = Json::obj();
    node.set("version", Json::num(cfg.version as f64));
    node.set("history", Json::boolean(cfg.history));
    let mut aliases = Json::obj();
    for (name, path) in &cfg.aliases {
        aliases.set(name.clone(), Json::str(path.clone()));
    }
    node.set("aliases", aliases);
    if !cfg.tags.is_empty() {
        let mut tags = Json::obj();
        for (key, members) in &cfg.tags {
            let mut list = Json::arr();
            if let Json::Arr(items) = &mut list {
                for member in members {
                    items.push(Json::str(member.clone()));
                }
            }
            tags.set(key.clone(), list);
        }
        node.set("tags", tags);
    }
    node
}

fn load_config() -> Res<Config> {
    let mut cfg = Config {
        version: 1,
        history: true,
        aliases: BTreeMap::new(),
        tags: Vec::new(),
    };
    let file = config_file();
    if !path_exists(&file) {
        return Ok(cfg);
    }
    let raw = read_file(&file)?;
    let root = json_parse(&raw).map_err(|e| {
        user(format!(
            "{file} is not valid JSON ({}).\n  fix it by hand, or nuke it: rm {}",
            match e {
                AppError::User(m) => m,
                AppError::Usage(m) => m,
            },
            config_dir()
        ))
    })?;
    if !root.is_obj() {
        return Err(user(format!(
            "{file} is missing an \"aliases\" object.\n  expected: {{\"version\": 1, \"aliases\": {{\"name\": \"/path\"}}}}"
        )));
    }
    if let Some(version) = root.get("version") {
        if !version.is_num() {
            return Err(user(format!("{file} has a non-numeric version: {}", version.as_str())));
        }
        cfg.version = version.as_num() as i64;
    }
    if let Some(history) = root.get("history") {
        match history {
            Json::Bool(b) => cfg.history = *b,
            Json::Num(n) => cfg.history = *n != 0.0,
            _ => {}
        }
    }
    match root.get("aliases") {
        Some(Json::Obj(aliases)) => {
            for (name, value) in aliases {
                if let Json::Str(path) = value {
                    cfg.aliases.insert(name.clone(), path.clone());
                }
            }
        }
        _ => {
            return Err(user(format!(
                "{file} is missing an \"aliases\" object.\n  expected: {{\"version\": 1, \"aliases\": {{\"name\": \"/path\"}}}}"
            )));
        }
    }
    if let Some(Json::Obj(tags)) = root.get("tags") {
        for (key, value) in tags {
            if let Json::Arr(items) = value {
                let members: Vec<String> = items
                    .iter()
                    .filter_map(|item| if let Json::Str(s) = item { Some(s.clone()) } else { None })
                    .collect();
                if !members.is_empty() {
                    cfg.tags.push((key.clone(), members));
                }
            }
        }
    }
    Ok(cfg)
}

fn save_config(cfg: &Config) -> Res<()> {
    write_atomic(&config_file(), &(json_dump(&config_json(cfg), 2, 0) + "\n"), false)
}

// ------------------------------------------------------------------- frecency

#[derive(Clone, Copy, Default)]
struct Visit {
    n: f64,
    t: f64,
}

fn parse_history(raw: &str) -> BTreeMap<String, Visit> {
    let mut out = BTreeMap::new();
    let root = match json_parse(raw) {
        Ok(v) => v,
        Err(_) => return out,
    };
    let dirs = match root.get("dirs") {
        Some(Json::Obj(d)) => d,
        _ => return out,
    };
    for (path, entry) in dirs {
        if !entry.is_obj() {
            continue;
        }
        let mut v = Visit::default();
        if let Some(n) = entry.get("n") {
            v.n = n.as_num();
        }
        if let Some(t) = entry.get("t") {
            v.t = t.as_num();
        }
        out.insert(path.clone(), v);
    }
    out
}

fn read_history() -> BTreeMap<String, Visit> {
    let file = history_file();
    if !path_exists(&file) {
        return BTreeMap::new();
    }
    parse_history(&read_file(&file).unwrap_or_default())
}

fn decayed(v: &Visit, now: f64) -> f64 {
    let age = (now - v.t).max(0.0);
    v.n * 0.5_f64.powf(age / HALF_LIFE)
}

fn history_enabled(cfg: &Config) -> bool {
    if env::var_os("SIMPLEDIR_NO_HISTORY").is_some() {
        return false;
    }
    cfg.history
}

fn scanned_visit_time(raw: &str, path: &str) -> Option<f64> {
    let key = format!("\"{}\":", json_escape(path));
    let at = raw.find(&key)?;
    let entry_end = raw[at..].find('}').map(|p| at + p);
    let t_at = raw[at..].find("\"t\":").map(|p| at + p)?;
    if let Some(end) = entry_end {
        if t_at > end {
            return None;
        }
    }
    let digits = &raw[t_at + 4..];
    let mut end = 0usize;
    for (i, c) in digits.char_indices() {
        if matches!(c, '0'..='9' | '.' | '-' | '+' | 'e' | 'E') {
            end = i + c.len_utf8();
        } else {
            break;
        }
    }
    digits[..end].parse::<f64>().ok()
}

fn record_visit(path: &str, now: f64) {
    if env::var_os("SIMPLEDIR_NO_HISTORY").is_some() {
        return;
    }
    let raw = if path_exists(&history_file()) {
        read_file(&history_file()).unwrap_or_default()
    } else {
        String::new()
    };
    let throttle = || -> bool {
        if raw.is_empty() {
            return false;
        }
        if let Some(last) = scanned_visit_time(&raw, path) {
            return now - last < VISIT_THROTTLE;
        }
        if let Some(found) = parse_history(&raw).get(path) {
            return now - found.t < VISIT_THROTTLE;
        }
        false
    };
    if throttle() {
        return;
    }
    let mut dirs = parse_history(&raw);
    let n = dirs.get(path).map(|v| v.n).unwrap_or(0.0) + 1.0;
    dirs.insert(path.to_string(), Visit { n, t: now });

    let mut alive: BTreeMap<String, Visit> = dirs
        .into_iter()
        .filter(|(_, entry)| decayed(entry, now) >= FLOOR)
        .collect();
    if alive.len() > MAX_HISTORY {
        let mut ranked: Vec<(String, Visit)> = alive.into_iter().collect();
        ranked.sort_by(|a, b| {
            decayed(&b.1, now)
                .partial_cmp(&decayed(&a.1, now))
                .unwrap_or(std::cmp::Ordering::Equal)
        });
        ranked.truncate(MAX_HISTORY);
        alive = ranked.into_iter().collect();
    }

    let mut root = Json::obj();
    root.set("version", Json::num(1.0));
    let mut dirs_node = Json::obj();
    for (p, entry) in &alive {
        let mut v = Json::obj();
        v.set("n", Json::num(entry.n));
        v.set("t", Json::num(entry.t));
        dirs_node.set(p.clone(), v);
    }
    root.set("dirs", dirs_node);
    // A convenience, not the function you called: never break the cd.
    let _ = write_atomic(&history_file(), &(json_dump(&root, 2, 0) + "\n"), false);
}

#[derive(Clone)]
struct Hit {
    score: f64,
    path: String,
}

fn frecency_ranked(word: &str, cfg: &Config, now: f64) -> Vec<Hit> {
    let bound: BTreeSet<String> = cfg.aliases.values().map(|p| as_stored(p)).collect();
    let needle = lower(word);
    let mut found: Vec<Hit> = read_history()
        .into_iter()
        .filter(|(path, _)| !bound.contains(path))
        .filter(|(path, _)| needle.is_empty() || contains_ci(path, &needle))
        .map(|(path, entry)| Hit { score: decayed(&entry, now), path })
        .filter(|hit| hit.score >= FLOOR)
        .collect();
    found.sort_by(|a, b| {
        b.score
            .partial_cmp(&a.score)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| a.path.cmp(&b.path))
    });
    found
}

// --------------------------------------------------------------- suggestions

fn similarity(a: &str, b: &str) -> i32 {
    if a.is_empty() || b.is_empty() {
        return 0;
    }
    let common = a.chars().filter(|c| b.contains(*c)).count();
    (200.0 * common as f64 / (a.chars().count() + b.chars().count()) as f64) as i32
}

fn suggestions_for(alias: &str, cfg: &Config) -> Vec<String> {
    let mut hits = Vec::new();
    for name in cfg.aliases.keys() {
        if contains_ci(name, alias) && !hits.contains(name) {
            hits.push(name.clone());
        }
    }
    let mut close: Vec<String> = cfg
        .aliases
        .keys()
        .filter(|name| {
            similarity(&lower(alias), &lower(name)) >= 60 && !hits.contains(name)
        })
        .cloned()
        .collect();
    close.sort();
    for name in close {
        if hits.len() >= 3 {
            break;
        }
        hits.push(name);
    }
    hits.truncate(3);
    hits
}

fn unknown_alias_error(alias: &str, cfg: &Config) -> String {
    let mut msg = format!("no alias named '{alias}'");
    let hits = suggestions_for(alias, cfg);
    if !hits.is_empty() {
        msg.push_str("\n  did you mean: ");
        msg.push_str(&join(&hits, ", "));
    }
    if cfg.aliases.is_empty() {
        msg.push_str("\n  your config is empty. add one: ");
        msg.push_str(CONFIG);
        msg.push_str(" add <name> [path]");
    } else {
        msg.push_str("\n  see them all: ");
        msg.push_str(MOVE);
        msg.push_str(" ls");
    }
    msg
}

fn name_for(path: &str, taken: &BTreeSet<String>) -> String {
    let mut base = basename_of(path);
    if base.is_empty() {
        base = path.to_string();
    }
    let mut clean: String = base
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '.' || c == '_' || c == '-' {
                c
            } else {
                '-'
            }
        })
        .collect();
    if clean.is_empty() {
        clean = "dir".to_string();
    }
    if !taken.contains(&clean) {
        return clean;
    }
    for n in 2.. {
        let candidate = format!("{clean}{n}");
        if !taken.contains(&candidate) {
            return candidate;
        }
    }
    clean
}

fn require_dir(path: &str) -> Res<()> {
    if !is_dir(path) {
        Err(die(format!("not a directory: {path}")))
    } else {
        Ok(())
    }
}

// ----------------------------------------------------------- project aliases

#[derive(Default)]
struct Project {
    dir: String,
    aliases: BTreeMap<String, String>,
}

fn find_project_dir() -> String {
    let mut dir = match env::current_dir() {
        Ok(d) => d.to_string_lossy().to_string(),
        Err(_) => return String::new(),
    };
    let home = home_dir();
    for _ in 0..64 {
        let candidate = format!("{dir}/{PROJECT_FILE}");
        if path_exists(&candidate) {
            return dir;
        }
        let parent = Path::new(&dir).parent().map(|p| p.to_string_lossy().to_string());
        let parent = match parent {
            Some(p) => p,
            None => break,
        };
        if parent == dir || dir == home {
            break;
        }
        dir = parent;
    }
    String::new()
}

fn load_project(dir: &str) -> Res<Project> {
    let mut project = Project { dir: dir.to_string(), aliases: BTreeMap::new() };
    let file = format!("{dir}/{PROJECT_FILE}");
    if !path_exists(&file) {
        return Ok(project);
    }
    let raw = read_file(&file)?;
    let root = json_parse(&raw).map_err(|_| {
        user(format!(
            "{file} is not valid JSON.\n  fix it by hand, or move it aside: mv {file} {file}.bak"
        ))
    })?;
    if !root.is_obj() {
        return Err(user(format!("{file} needs an \"aliases\" object")));
    }
    match root.get("aliases") {
        Some(Json::Obj(aliases)) => {
            for (name, value) in aliases {
                if let Json::Str(stored) = value {
                    project.aliases.insert(name.clone(), stored.clone());
                }
            }
        }
        _ => {
            return Err(user(format!(
                "{file} needs an \"aliases\" object, even if it is empty:\n  {{\"aliases\": {{}}}}"
            )));
        }
    }
    Ok(project)
}

fn save_project(project: &Project) -> Res<()> {
    let mut root = Json::obj();
    root.set("version", Json::num(1.0));
    let mut aliases = Json::obj();
    for (name, stored) in &project.aliases {
        aliases.set(name.clone(), Json::str(stored.clone()));
    }
    root.set("aliases", aliases);
    write_atomic(
        &(project.dir.clone() + "/" + PROJECT_FILE),
        &(json_dump(&root, 2, 0) + "\n"),
        false,
    )
}

fn lexically_relative(path: &str, base: &str) -> String {
    let p: Vec<&str> = path.split('/').filter(|s| !s.is_empty()).collect();
    let b: Vec<&str> = base.split('/').filter(|s| !s.is_empty()).collect();
    let mut i = 0usize;
    while i < p.len() && i < b.len() && p[i] == b[i] {
        i += 1;
    }
    let mut parts: Vec<&str> = vec![".."; b.len() - i];
    parts.extend_from_slice(&p[i..]);
    parts.join("/")
}

fn store_for_project(absolute: &str, project_dir: &str) -> String {
    if project_dir.is_empty() {
        return absolute.to_string();
    }
    let rel = lexically_relative(absolute, project_dir);
    if !rel.is_empty() && !starts_with(&rel, "..") {
        rel
    } else {
        absolute.to_string()
    }
}

fn resolve_project_path(stored: &str, project_dir: &str) -> String {
    if starts_with(stored, "~") {
        return abspath(stored);
    }
    if stored.starts_with('/') {
        return stored.to_string();
    }
    abspath(&(project_dir.to_string() + "/" + stored))
}

// ------------------------------------------------------------------ adapting

const ADAPT_BASE_DAYS: f64 = 7.0;
const ADAPT_MAX_DAYS: f64 = 365.0;
const ADAPT_MAX_VERDICTS: usize = 500;

#[derive(Clone, Default)]
struct Verdict {
    verdict: String,
    at: f64,
    strikes: i32,
}

fn adapt_file() -> String {
    config_dir() + "/adapt.json"
}

fn read_verdicts() -> BTreeMap<String, Verdict> {
    let mut out = BTreeMap::new();
    let path = adapt_file();
    if !path_exists(&path) {
        return out;
    }
    let raw = match read_file(&path) {
        Ok(r) => r,
        Err(_) => return out,
    };
    let root = match json_parse(&raw) {
        Ok(v) => v,
        Err(_) => return out,
    };
    let verdicts = match root.get("verdicts") {
        Some(Json::Obj(v)) => v,
        _ => return out,
    };
    for (key, node) in verdicts {
        if !node.is_obj() {
            continue;
        }
        let mut v = Verdict::default();
        if let Some(s) = node.get("v") {
            v.verdict = s.as_str();
        }
        if let Some(t) = node.get("t") {
            v.at = t.as_num();
        }
        if let Some(n) = node.get("s") {
            v.strikes = n.as_num() as i32;
        }
        if !v.verdict.is_empty() {
            out.insert(key.clone(), v);
        }
    }
    out
}

fn write_verdicts(verdicts: &BTreeMap<String, Verdict>) {
    let now = now_seconds();
    let mut by_age: Vec<(f64, String)> = Vec::new();
    for (path, v) in verdicts {
        if now - v.at > 2.0 * ADAPT_MAX_DAYS * 24.0 * 3600.0 {
            continue;
        }
        by_age.push((v.at, path.clone()));
    }
    by_age.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal));
    if by_age.len() > ADAPT_MAX_VERDICTS {
        let excess = by_age.len() - ADAPT_MAX_VERDICTS;
        by_age.drain(0..excess);
    }
    let keep: BTreeSet<String> = by_age.iter().map(|(_, p)| p.clone()).collect();
    let mut root = Json::obj();
    root.set("version", Json::num(1.0));
    let mut node = Json::obj();
    for (path, v) in verdicts {
        if !keep.contains(path) {
            continue;
        }
        let mut entry = Json::obj();
        entry.set("v", Json::str(v.verdict.clone()));
        entry.set("t", Json::num(v.at));
        entry.set("s", Json::num(v.strikes as f64));
        node.set(path.clone(), entry);
    }
    root.set("verdicts", node);
    let _ = write_atomic(&adapt_file(), &(json_dump(&root, 2, 0) + "\n"), false);
}

fn quiet_for(v: &Verdict) -> f64 {
    let strikes = v.strikes.max(1);
    let days = ADAPT_BASE_DAYS * 2_f64.powi(strikes - 1);
    days.min(ADAPT_MAX_DAYS) * 24.0 * 3600.0
}

#[derive(Clone)]
struct Proposal {
    path: String,
    name: String,
    score: f64,
    refused: bool,
    retry: bool,
    strikes: i32,
    quiet_left: f64,
}

fn adapt_proposals(limit: i32, include_rejected: bool) -> Res<Vec<Proposal>> {
    let cfg = load_config()?;
    let now = now_seconds();
    let verdicts = read_verdicts();
    let bound: BTreeSet<String> = cfg.aliases.values().map(|p| as_stored(p)).collect();
    let taken: BTreeSet<String> = cfg.aliases.keys().cloned().collect();
    let mut found: Vec<Proposal> = Vec::new();
    for (path, entry) in read_history() {
        if bound.contains(&path) || !is_dir(&path) {
            continue;
        }
        let score = decayed(&entry, now);
        if score < FLOOR {
            continue;
        }
        let mut refused = false;
        let mut retry = false;
        let mut strikes = 0;
        let mut quiet_left = 0.0;
        if let Some(v) = verdicts.get(&path) {
            strikes = v.strikes;
            if v.verdict == "yes" {
                continue;
            }
            if v.verdict == "no" {
                refused = true;
                quiet_left = v.at + quiet_for(v) - now;
                if quiet_left > 0.0 {
                    if !include_rejected {
                        continue;
                    }
                } else {
                    retry = true;
                    quiet_left = 0.0;
                }
            }
        }
        found.push(Proposal {
            name: name_for(&path, &taken),
            path,
            score,
            refused,
            retry,
            strikes,
            quiet_left,
        });
    }
    found.sort_by(|a, b| {
        b.score
            .partial_cmp(&a.score)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| a.path.cmp(&b.path))
    });
    if limit > 0 && (limit as usize) < found.len() {
        found.truncate(limit as usize);
    }
    Ok(found)
}

// ---------------------------------------------------------------- arguments

#[derive(Default, Clone)]
struct Args {
    words: Vec<String>,
    flags: BTreeMap<String, String>,
}

impl Args {
    fn has(&self, name: &str) -> bool {
        match self.flags.get(name) {
            Some(v) => v != "false" && v != "0" && !v.is_empty(),
            None => false,
        }
    }
    fn saw(&self, name: &str) -> bool {
        self.flags.contains_key(name)
    }
    fn value(&self, name: &str) -> String {
        self.flags.get(name).cloned().unwrap_or_default()
    }
    fn word(&self, i: usize) -> String {
        self.words.get(i).cloned().unwrap_or_default()
    }
}

fn parse_args(argv: &[String]) -> Res<Args> {
    let mut args = Args::default();
    let value_flags: BTreeSet<&str> = ["depth", "prefix", "top", "to", "path", "runs"].into_iter().collect();
    let mut no_more_flags = false;
    let mut i = 0usize;
    while i < argv.len() {
        let arg = &argv[i];
        if !no_more_flags && arg == "--" {
            no_more_flags = true;
            i += 1;
            continue;
        }
        if !no_more_flags && starts_with(arg, "--") {
            let name = &arg[2..];
            if value_flags.contains(name) {
                i += 1;
                let value = argv.get(i).cloned().unwrap_or_default();
                args.flags.insert(name.to_string(), value);
                i += 1;
                continue;
            }
            let (name, value) = match name.split_once('=') {
                Some((n, v)) => (n, v != "false" && v != "0"),
                None => (name, true),
            };
            args.flags.insert(name.to_string(), if value { "true".to_string() } else { "false".to_string() });
            i += 1;
            continue;
        }
        if !no_more_flags && arg.len() > 1 && arg.starts_with('-') {
            return Err(usage(format!(
                "unknown flag '{arg}'. all flags here are long form, e.g. --force"
            )));
        }
        args.words.push(arg.clone());
        i += 1;
    }
    Ok(args)
}

fn parse_limit(args: &Args, default: i32) -> Res<i32> {
    if !args.saw("top") {
        return Ok(default);
    }
    match args.value("top").parse::<i32>() {
        Ok(n) if n > 0 => Ok(n),
        _ => Err(usage("--top wants a positive number")),
    }
}

// ---------------------------------------------------------------- dispatch

fn run_move(argv: &[String]) -> Res<i32> {
    // `sd @work` and `sd @work dots` address a tag. Handled before the fast path,
    // because a tag is never an alias and must never be resolved as one.
    if !argv.is_empty() && starts_with(&argv[0], TAG_PREFIX) && argv[0] != "--help" && argv[0] != "--version" {
        let cfg = load_config()?;
        let probe = Args { words: argv.to_vec(), flags: BTreeMap::new() };
        let (tag_name, members, rest) = split_tag(&probe, &cfg)?;
        if rest.is_empty() {
            return cmd_tag_list(&cfg, &tag_name, &members);
        }
        if !members.contains(&rest) {
            let mut msg = format!("{rest} isn't in {tag_name}.");
            msg.push_str(" it has:");
            for member in &members {
                msg.push(' ');
                msg.push_str(member);
            }
            msg.push_str("\n  add it: ");
            msg.push_str(CONFIG);
            msg.push_str(" tag ");
            msg.push_str(&tag_name);
            msg.push(' ');
            msg.push_str(&rest);
            return Err(user(msg));
        }
        out(&(jump(&rest, &cfg, now_seconds())? + "\n"));
        return Ok(0);
    }

    // Fast path: `sd <word>` and `sd --version` run on every prompt.
    if argv.len() == 1 {
        if argv[0] == "--version" {
            out(&format!("{MOVE} {VERSION} - {TAGLINE}\n"));
            return Ok(0);
        }
        if argv[0] == "--help" {
            out(MOVE_HELP);
            return Ok(0);
        }
        if !argv[0].starts_with('-') && !move_verbs().contains(argv[0].as_str()) {
            let cfg = load_config()?;
            out(&(jump(&argv[0], &cfg, now_seconds())? + "\n"));
            return Ok(0);
        }
    }
    if argv.is_empty() {
        out(MOVE_HELP);
        return Ok(0);
    }

    let verb = argv[0].as_str();
    let rest = &argv[1..];
    match verb {
        "ls" => {
            let args = parse_args(rest)?;
            let allowed: BTreeSet<&str> = ["names", "json", "long"].into_iter().collect();
            for flag in args.flags.keys() {
                if !allowed.contains(flag.as_str()) {
                    return Err(usage(format!(
                        "unknown flag '--{flag}' for `{MOVE} ls`. try --long, --names, --json"
                    )));
                }
            }
            cmd_ls(&args)
        }
        "top" => {
            let args = parse_args(rest)?;
            let allowed: BTreeSet<&str> = ["json"].into_iter().collect();
            for flag in args.flags.keys() {
                if !allowed.contains(flag.as_str()) {
                    return Err(usage(format!(
                        "unknown flag '--{flag}' for `{MOVE} top`. try --json"
                    )));
                }
            }
            cmd_top(&args)
        }
        "recent" => {
            let args = parse_args(rest)?;
            let allowed: BTreeSet<&str> = ["json", "top"].into_iter().collect();
            for flag in args.flags.keys() {
                if !allowed.contains(flag.as_str()) {
                    return Err(usage(format!(
                        "unknown flag '--{flag}' for `{MOVE} recent`. try --top, --json"
                    )));
                }
            }
            cmd_recent(&args)
        }
        "stats" => {
            let args = parse_args(rest)?;
            for flag in args.flags.keys() {
                if flag != "json" {
                    return Err(usage(format!(
                        "unknown flag '--{flag}' for `{MOVE} stats`. try --json"
                    )));
                }
            }
            cmd_stats(&args)
        }
        "adapt" => {
            let args = parse_args(rest)?;
            let allowed: BTreeSet<&str> = ["json", "top", "all"].into_iter().collect();
            for flag in args.flags.keys() {
                if !allowed.contains(flag.as_str()) {
                    return Err(usage(format!(
                        "unknown flag '--{flag}' for `sd adapt`. try --top, --all, --json"
                    )));
                }
            }
            cmd_adapt(&args)
        }
        "i" => cmd_pick(&parse_args(rest)?),
        "suggest" => {
            let args = parse_args(rest)?;
            let allowed: BTreeSet<&str> = ["json", "top"].into_iter().collect();
            for flag in args.flags.keys() {
                if !allowed.contains(flag.as_str()) {
                    return Err(usage(format!(
                        "unknown flag '--{flag}' for `{MOVE} suggest`. try --top, --json"
                    )));
                }
            }
            cmd_suggest(&args)
        }
        "print" => {
            let args = parse_args(rest)?;
            if args.words.is_empty() {
                return Err(user("which alias? usage: sd print <alias>"));
            }
            let cfg = load_config()?;
            out(&(jump(&args.word(0), &cfg, now_seconds())? + "\n"));
            Ok(0)
        }
        "sha256" => cmd_sha256(&parse_args(rest)?),
        other if config_verbs().contains(other) => Err(usage(format!(
            "'{other}' is not an {MOVE} command.\n  did you mean `{CONFIG} {other}`? {MOVE} only reads your config."
        ))),
        other => Err(usage(format!(
            "'{other}' is not an {MOVE} command. try `{MOVE} --help`"
        ))),
    }
}

fn run_config(argv: &[String]) -> Res<i32> {
    if argv.is_empty() {
        out(CONFIG_HELP);
        return Ok(0);
    }
    let verb = argv[0].as_str();
    let rest = &argv[1..];
    let args = parse_args(rest)?;
    let known = config_verbs();

    if verb == "--version" {
        out(&format!("{CONFIG} {VERSION} - {TAGLINE}\n"));
        return Ok(0);
    }
    if verb == "--help" {
        out(CONFIG_HELP);
        return Ok(0);
    }
    if !known.contains(verb) {
        let move_hints: BTreeSet<&str> =
            ["ls", "print", "suggest", "i", "top", "recent", "stats", "adapt", "sha256"]
                .into_iter()
                .collect();
        if move_hints.contains(verb) {
            err(&format!("did you mean `{MOVE} {verb}`? {CONFIG} only changes things.\n"));
        }
        return Err(usage(format!("'{verb}' is not an {CONFIG} command. try `{CONFIG} --help`")));
    }

    match verb {
        "add" => cmd_add(&args),
        "rm" => cmd_rm(&args),
        "rename" => cmd_rename(&args),
        "import" => cmd_import(&args),
        "bind" => cmd_bind(&args),
        "tag" => cmd_tag(&args),
        "project" => cmd_project(&args),
        "export" => cmd_export(&args),
        "adopt" => cmd_adopt(&args),
        "adapt" => cmd_adapt_apply(&args),
        "forget" => cmd_forget(&args),
        "tidy" => cmd_tidy(&args),
        "migrate" => cmd_migrate(&args),
        "zoxide" => cmd_zoxide(&args),
        "update" => cmd_update(&args),
        "revert" => cmd_revert(&args),
        "releases" => cmd_releases(&args),
        "uninstall" => cmd_uninstall(&args),
        "doctor" => cmd_doctor(),
        "bench" => cmd_bench(&args),
        "init" => cmd_init(),
        "prompt" => cmd_prompt(&args.word(0)),
        "edit" => cmd_edit(&args),
        "completions" => {
            let shell = args.word(0);
            if shell != "bash" && shell != "zsh" {
                return Err(usage("completions takes bash or zsh"));
            }
            cmd_completions(&shell)
        }
        // Unreachable in a correct build: `known` is the list of verbs and every
        // one of them has a line above. It used to be a bare `return 0`, ...
        _ => Err(die(format!(
            "'{verb}' is listed as a command but nothing handles it.\n  that is a bug in {MOVE}, not something you did wrong: {CONFIG} {verb} should have done something."
        ))),
    }
}

fn main() {
    *OUT.lock().unwrap() = Some(Out::new(1));
    *ERR.lock().unwrap() = Some(Out::new(2));

    let argv: Vec<String> = env::args_os().skip(1).map(|a| a.to_string_lossy().to_string()).collect();
    let invoked = env::args_os()
        .next()
        .map(|a| {
            Path::new(&a)
                .file_name()
                .map(|s| s.to_string_lossy().to_string())
                .unwrap_or_else(|| MOVE.to_string())
        })
        .unwrap_or_else(|| MOVE.to_string());
    if invoked == CONFIG {
        set_prog(CONFIG);
    }

    let status = if prog() == CONFIG { run_config(&argv) } else { run_move(&argv) };
    match status {
        Ok(code) => {
            flush_out();
            std::process::exit(code);
        }
        Err(AppError::Usage(msg)) => {
            flush_out();
            err(&format!("{}: {msg}\n", prog()));
            flush_err();
            std::process::exit(2);
        }
        Err(AppError::User(msg)) => {
            flush_out();
            err(&format!("{}: {msg}\n", prog()));
            flush_err();
            std::process::exit(1);
        }
    }
}

// The move half of simpledir, ported from sd.cpp (v11).
//
// Concatenated with the core in src/main.rs; no crate attributes, no main and no
// dependencies. Private helpers are prefixed `mv_` so they cannot collide with
// the other parts of the recode.

// Which of the core helpers' error type a missing tag produces. Identical in
// `split_tag` and in `sd ls @tag`, including the ordering of the tag list.
fn mv_no_tag_error(tag: &str, cfg: &Config) -> AppError {
    let mut msg = format!("no tag {tag}.");
    if cfg.tags.is_empty() {
        msg.push_str(" you have no tags: ");
        msg.push_str(CONFIG);
        msg.push_str(" tag @work dots hypr");
    } else {
        msg.push_str(" you have:");
        for (key, _) in &cfg.tags {
            msg.push(' ');
            msg.push_str(key);
        }
    }
    user(msg)
}

// Round a score the way the C++ `snprintf("%.3f")` + `stod` did before putting
// it in JSON: three decimals, then a number again.
fn mv_json_score(score: f64) -> Json {
    Json::num(format!("{score:.3}").parse::<f64>().unwrap_or(0.0))
}

fn mv_fzf_available() -> bool {
    for dir in split(&env_or("PATH", "/usr/bin:/bin"), ':') {
        if dir.is_empty() {
            continue;
        }
        let candidate = format!("{dir}/fzf");
        if let Ok(meta) = fs::metadata(&candidate) {
            if meta.permissions().mode() & 0o111 != 0 {
                return true;
            }
        }
    }
    false
}

// ------------------------------------------------------------------ resolving

fn resolve(spec: &str, cfg: &Config) -> Res<String> {
    let (name, suffix) = match spec.find('/') {
        Some(slash) => (spec[..slash].to_string(), spec[slash + 1..].to_string()),
        None => (spec.to_string(), String::new()),
    };
    // An absolute path is never an alias. Say so plainly rather than letting
    // the frecency log substring-match it and jump somewhere surprising.
    if name.is_empty() || starts_with(spec, "/") {
        return Err(user(format!(
            "'{}' isn't an alias, it's a path\n  bind it first: {} add <name> {}",
            spec, CONFIG, spec
        )));
    }

    // A project's own name wins while you are inside its tree; a broken project
    // file must not break every global alias.
    let project_dir = find_project_dir();
    let mut project_aliases: BTreeMap<String, String> = BTreeMap::new();
    if !project_dir.is_empty() {
        if let Ok(project) = load_project(&project_dir) {
            project_aliases = project.aliases;
        }
    }

    let mut key = name.clone();
    let mut target = String::new();
    let mut found = false;

    if let Some(stored) = project_aliases.get(&name) {
        target = resolve_project_path(stored, &project_dir);
        found = true;
    }
    if !found {
        if let Some(stored) = cfg.aliases.get(&name) {
            target = as_stored(stored);
            found = true;
        }
    }

    if !found {
        // A unique prefix is enough. Each pool is tried on its own, so a
        // project alias is never made ambiguous by a global one.
        let mut matches: Vec<String> = project_aliases
            .keys()
            .filter(|candidate| starts_with(candidate, &name))
            .cloned()
            .collect();
        matches.sort();
        if matches.is_empty() {
            matches = cfg
                .aliases
                .keys()
                .filter(|candidate| starts_with(candidate, &name))
                .cloned()
                .collect();
            matches.sort();
        }
        if matches.len() == 1 {
            key = matches[0].clone();
            target = match project_aliases.get(&key) {
                Some(stored) => resolve_project_path(stored, &project_dir),
                None => as_stored(&cfg.aliases[&key]),
            };
        } else if matches.len() > 1 {
            return Err(user(format!(
                "'{}' matches several aliases: {}\n  use the whole name, or {} ls",
                name,
                join(&matches, " "),
                MOVE
            )));
        } else {
            return Err(user(unknown_alias_error(&name, cfg)));
        }
    }

    if !suffix.is_empty() {
        target = lexically_normal(&Path::new(&target).join(&suffix).to_string_lossy());
    }

    if !is_dir(&target) {
        let where_ = if suffix.is_empty() {
            format!("'{key}'")
        } else {
            format!("'{spec}'")
        };
        return Err(user(format!(
            "alias {} resolves to {}\n  that directory is gone. fix it: {} add --force {} <new-path>",
            where_, target, CONFIG, key
        )));
    }
    Ok(target)
}

fn jump(word: &str, cfg: &Config, now: f64) -> Res<String> {
    if starts_with(word, "/") || starts_with(word, "~") {
        return Err(user(format!(
            "'{}' isn't an alias, it's a path\n  bind it first: {} add <name> {}",
            word, CONFIG, word
        )));
    }
    let mut target = String::new();
    match resolve(word, cfg) {
        Ok(found) => target = found,
        // A directory you have actually been to is good enough when the name is
        // unknown. Every other resolution error is real and must reach the user.
        Err(err) if is_unknown_alias(&err) => {
            for hit in frecency_ranked(word, cfg, now) {
                if is_dir(&hit.path) {
                    target = hit.path;
                    break;
                }
            }
            if target.is_empty() {
                return Err(err); // rethrow the real error
            }
        }
        Err(err) => return Err(err),
    }
    if history_enabled(cfg) {
        record_visit(&target, now);
    }
    Ok(target)
}

// (name, absolute target, missing) sorted by name, optionally filtered.
fn rows(cfg: &Config, query: &str) -> Vec<(String, String, bool)> {
    let mut found = Vec::new();
    for (name, path) in &cfg.aliases {
        let target = as_stored(path);
        if !query.is_empty() && !contains_ci(name, query) && !contains_ci(&target, query) {
            continue;
        }
        let missing = !is_dir(&target);
        found.push((name.clone(), target, missing));
    }
    found.sort_by(|a, b| a.0.cmp(&b.0));
    found
}

// Export helper: $HOME becomes ~, everything else stays absolute.
fn portable(absolute: &str) -> String {
    let home = home_dir();
    if home.is_empty() || home == "/" {
        return absolute.to_string();
    }
    if absolute == home {
        return "~".to_string();
    }
    if let Some(rest) = absolute.strip_prefix(&(home.clone() + "/")) {
        return format!("~/{rest}");
    }
    absolute.to_string()
}

// ----------------------------------------------------------------------- tags

// A tag can be jumped through or listed, so the move half has to understand
// one before treating the word as an alias. Returns empty strings when the
// first word is not a tag (the C++ function returned false in that case).
fn split_tag(args: &Args, cfg: &Config) -> Res<(String, Vec<String>, String)> {
    if args.words.is_empty() || !starts_with(&args.words[0], TAG_PREFIX) {
        return Ok((String::new(), Vec::new(), String::new()));
    }
    let tag_name = args.words[0].clone();
    let rest = args.words.get(1).cloned().unwrap_or_default();
    match cfg.tags.iter().find(|(key, _)| *key == tag_name) {
        Some((_, members)) => Ok((tag_name, members.clone(), rest)),
        None => Err(mv_no_tag_error(&tag_name, cfg)),
    }
}

fn cmd_tag_list(cfg: &Config, tag_name: &str, members: &[String]) -> Res<i32> {
    // Members that exist, in tag order, with their stored paths.
    let listed: Vec<(&String, &String)> = members
        .iter()
        .filter_map(|name| cfg.aliases.get_key_value(name))
        .collect();
    if listed.is_empty() {
        out(&format!("{tag_name} has nothing left in it. its aliases were renamed or removed.\n"));
        out(&format!("  drop it: {} tag --drop {tag_name}\n", CONFIG));
        return Ok(1);
    }
    let mut missing = String::new();
    for name in members {
        if !cfg.aliases.contains_key(name) {
            if !missing.is_empty() {
                missing.push(' ');
            }
            missing.push_str(name);
        }
    }

    let width = listed.iter().map(|(name, _)| name.len()).max().unwrap_or(0);
    let cwd = env::current_dir().ok().map(|p| p.to_string_lossy().to_string());
    out(&format!(
        "{} {} in {tag_name}:\n\n",
        listed.len(),
        if listed.len() == 1 { "alias" } else { "aliases" }
    ));
    for (name, path) in &listed {
        let mut shown = (*path).clone();
        if let Some(cwd) = &cwd {
            let relative = lexically_relative(path, cwd);
            if !relative.is_empty() && !starts_with(&relative, "..") {
                shown = relative;
            }
        }
        let gone = !is_dir(&as_stored(path));
        out(&format!(
            "  {}{}{}{}\n",
            name,
            " ".repeat(width - name.len() + 2),
            shown,
            if gone { "   [missing]" } else { "" }
        ));
    }
    if !missing.is_empty() {
        out(&format!(
            "\n  {missing} no longer exist as aliases. drop them with:\n    {} tag {tag_name} --remove {missing}\n",
            CONFIG
        ));
    }
    out(&format!("\n  jump to one: {} {tag_name} <name>\n", MOVE));
    Ok(0)
}

// --------------------------------------------------------------- move commands

fn cmd_ls(args: &Args) -> Res<i32> {
    let cfg = load_config()?;
    let mut query = args.word(0);

    // `sd ls @work` is the tag, filtered the same way a name filter is.
    let mut tag_filter = String::new();
    if starts_with(&query, TAG_PREFIX) {
        tag_filter = query.clone();
        query.clear();
        if !cfg.tags.iter().any(|(key, _)| *key == tag_filter) {
            return Err(mv_no_tag_error(&tag_filter, &cfg));
        }
    }

    let mut found = rows(&cfg, &query);
    if !tag_filter.is_empty() {
        let members = cfg
            .tags
            .iter()
            .find(|(key, _)| *key == tag_filter)
            .map(|(_, members)| members)
            .unwrap();
        let keep: BTreeSet<&str> = members.iter().map(|name| name.as_str()).collect();
        found.retain(|(name, _, _)| keep.contains(name.as_str()));
    }

    if args.has("names") {
        for (name, _, _) in &found {
            out(&format!("{name}\n"));
        }
        return Ok(if found.is_empty() { 1 } else { 0 });
    }

    if args.has("json") {
        let mut node = Json::obj();
        node.set("version", Json::num(1.0));
        let mut aliases = Json::obj();
        for (name, target, _) in &found {
            aliases.set(name.clone(), Json::str(target.clone()));
        }
        node.set("aliases", aliases);
        out(&(json_dump(&node, 2, 0) + "\n"));
        return Ok(if found.is_empty() { 1 } else { 0 });
    }

    if found.is_empty() {
        if !cfg.aliases.is_empty() && !query.is_empty() {
            err(&format!(
                "{}: nothing matches '{}'. see them all: {} ls\n",
                MOVE, query, MOVE
            ));
        } else {
            err(&format!("{}: no aliases yet. add one: {} add\n", MOVE, CONFIG));
        }
        return Ok(1);
    }

    let width = found.iter().map(|(name, _, _)| name.len()).max().unwrap_or(0);
    let cwd = env::current_dir().ok().map(|p| p.to_string_lossy().to_string());
    for (name, target, missing) in &found {
        let mut shown = target.clone();
        if !args.has("long") {
            if let Some(cwd) = &cwd {
                let relative = lexically_relative(target, cwd);
                // A path full of ../ is noise, so keep the absolute one.
                if !relative.is_empty() && !starts_with(&relative, "..") {
                    shown = relative;
                }
            }
        }
        let mut badges = Vec::new();
        if *missing {
            badges.push("missing".to_string());
        }
        // No badge when already filtered by that tag: it would just repeat.
        if tag_filter.is_empty() {
            for (key, members) in &cfg.tags {
                if members.contains(name) {
                    badges.push(tag_label(key));
                }
            }
        }
        let mut line = format!("{}{}{}", name, " ".repeat(width - name.len() + 2), shown);
        if !badges.is_empty() {
            line.push_str("   [");
            line.push_str(&join(&badges, " "));
            line.push(']');
        }
        line.push('\n');
        out(&line);
    }
    // The nudge owns SIMPLEDIR_NO_UPDATE_CHECK and the tty check itself.
    nudge();
    Ok(0)
}

fn cmd_top(args: &Args) -> Res<i32> {
    let cfg = load_config()?;
    let now = now_seconds();
    let query = args.word(0);
    let bound: BTreeSet<String> = cfg.aliases.values().map(|path| as_stored(path)).collect();

    struct MvRow {
        score: f64,
        path: String,
        named: bool,
        exists: bool,
    }
    let mut ranked: Vec<MvRow> = Vec::new();
    for (path, entry) in read_history() {
        if !query.is_empty() && !contains_ci(&path, &query) {
            continue;
        }
        let score = decayed(&entry, now);
        if score < FLOOR {
            continue;
        }
        ranked.push(MvRow {
            score,
            named: bound.contains(&path),
            exists: is_dir(&path),
            path,
        });
    }
    ranked.sort_by(|a, b| {
        b.score
            .partial_cmp(&a.score)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| a.path.cmp(&b.path))
    });

    if ranked.is_empty() {
        out("nothing in your history yet. every directory you jump to gets remembered.\n");
        out(&format!(
            "  turn it off with SIMPLEDIR_NO_HISTORY=1, or {} forget\n",
            CONFIG
        ));
        return Ok(0);
    }

    if args.has("json") {
        let mut list = Json::arr();
        if let Json::Arr(items) = &mut list {
            for row in &ranked {
                let mut node = Json::obj();
                node.set("path", Json::str(row.path.clone()));
                node.set("score", mv_json_score(row.score));
                node.set("named", Json::boolean(row.named));
                node.set("exists", Json::boolean(row.exists));
                items.push(node);
            }
        }
        out(&(json_dump(&list, 2, 0) + "\n"));
        return Ok(0);
    }

    let width = ranked.iter().map(|row| row.path.len()).max().unwrap_or(0);
    out(&format!(
        "{} directories, most recent visits first:\n\n",
        ranked.len()
    ));
    let mut any_gone = false;
    for row in &ranked {
        let mut tags = Vec::new();
        if !row.exists {
            tags.push("gone".to_string());
            any_gone = true;
        } else if row.named {
            tags.push("named".to_string());
        }
        let mut line = format!(
            "  {:6.2}  {}{}",
            row.score,
            row.path,
            " ".repeat(width - row.path.len())
        );
        if !tags.is_empty() {
            line.push_str("   [");
            line.push_str(&join(&tags, ", "));
            line.push(']');
        }
        line.push('\n');
        out(&line);
    }
    if any_gone {
        out(&format!("\n  clean those up: {} forget --missing\n", CONFIG));
    }
    Ok(0)
}

// `top` ranks by decayed frecency. `recent` answers the other question: where
// did I just come from? Both read the same log; neither writes to it.
fn cmd_recent(args: &Args) -> Res<i32> {
    let cfg = load_config()?;
    let limit = parse_limit(args, 10)?;
    let now = now_seconds();
    let query = args.word(0);
    let bound: BTreeSet<String> = cfg.aliases.values().map(|p| as_stored(p)).collect();

    struct RecentRow {
        path: String,
        at: f64,
        score: f64,
        named: bool,
        exists: bool,
    }
    let mut rows: Vec<RecentRow> = Vec::new();
    for (path, entry) in read_history() {
        if !query.is_empty() && !contains_ci(&path, &query) {
            continue;
        }
        let score = decayed(&entry, now);
        if score < FLOOR {
            continue;
        }
        let named = bound.contains(&path);
        let exists = is_dir(&path);
        rows.push(RecentRow {
            path,
            at: entry.t,
            score,
            named,
            exists,
        });
    }
    rows.sort_by(|a, b| num_cmp(b.at, a.at).then_with(|| a.path.cmp(&b.path)));
    if (limit as usize) < rows.len() {
        rows.truncate(limit as usize);
    }

    if rows.is_empty() {
        if !cfg.aliases.is_empty() && !query.is_empty() {
            err(&format!(
                "{MOVE}: nothing recent matches '{query}'. see the whole log: {MOVE} top\n"
            ));
        } else {
            out("nothing in your visit log yet. every jump gets remembered.\n");
            out(&format!(
                "  turn it off with SIMPLEDIR_NO_HISTORY=1, or {CONFIG} forget\n"
            ));
        }
        return Ok(0);
    }

    if args.has("json") {
        let mut list = Json::arr();
        if let Json::Arr(items) = &mut list {
            for row in &rows {
                let mut node = Json::obj();
                node.set("path", Json::str(row.path.clone()));
                node.set("last", Json::num(row.at));
                node.set("age", Json::str(relative_age(now, row.at)));
                node.set("score", mv_json_score(row.score));
                node.set("named", Json::boolean(row.named));
                node.set("exists", Json::boolean(row.exists));
                items.push(node);
            }
        }
        out(&(json_dump(&list, 2, 0) + "\n"));
        return Ok(0);
    }

    let width = rows
        .iter()
        .map(|row| relative_age(now, row.at).len())
        .max()
        .unwrap_or(0);
    out(&format!("{} directories, newest visit first:\n\n", rows.len()));
    for row in &rows {
        let mut badges = Vec::new();
        if !row.exists {
            badges.push("gone".to_string());
        } else if row.named {
            badges.push("named".to_string());
        }
        let age = relative_age(now, row.at);
        let mut line = format!("  {:>age_width$}  {}", age, row.path, age_width = width);
        if !badges.is_empty() {
            line.push_str("   [");
            line.push_str(&join(&badges, ", "));
            line.push(']');
        }
        line.push('\n');
        out(&line);
    }
    out(&format!("\n  ranked by visits: {MOVE} top\n"));
    Ok(0)
}

fn cmd_stats(args: &Args) -> Res<i32> {
    let cfg = load_config()?;
    let now = now_seconds();
    let dirs = read_history();

    let missing: Vec<String> = cfg
        .aliases
        .iter()
        .filter(|(_, path)| !is_dir(&as_stored(path)))
        .map(|(name, _)| name.clone())
        .collect();
    let stale: Vec<String> = cfg
        .tags
        .iter()
        .flat_map(|(_, members)| members.iter())
        .filter(|member| !cfg.aliases.contains_key(*member))
        .cloned()
        .collect();
    let dead_history: Vec<String> = dirs
        .iter()
        .filter(|(path, visit)| !is_dir(path) || decayed(visit, now) < FLOOR)
        .map(|(path, _)| path.clone())
        .collect();
    let alive = dirs.len() - dead_history.len();
    let top = dirs
        .iter()
        .max_by(|a, b| num_cmp(decayed(a.1, now), decayed(b.1, now)));
    let newest = dirs.iter().max_by(|a, b| num_cmp(a.1.t, b.1.t));
    let oldest = dirs.iter().min_by(|a, b| num_cmp(a.1.t, b.1.t));

    if args.has("json") {
        let mut node = Json::obj();
        node.set("aliases", Json::num(cfg.aliases.len() as f64));
        node.set("missing_aliases", Json::num(missing.len() as f64));
        node.set("tags", Json::num(cfg.tags.len() as f64));
        node.set("stale_tag_members", Json::num(stale.len() as f64));
        node.set("history", Json::num(dirs.len() as f64));
        node.set("alive", Json::num(alive as f64));
        node.set("dead_history", Json::num(dead_history.len() as f64));
        node.set("recording", Json::boolean(history_enabled(&cfg)));
        node.set(
            "top",
            match top {
                Some((path, visit)) => {
                    let mut entry = Json::obj();
                    entry.set("path", Json::str(path.clone()));
                    entry.set("score", mv_json_score(decayed(visit, now)));
                    entry
                }
                None => Json::Null,
            },
        );
        let when = |found: Option<(&String, &Visit)>| match found {
            Some((path, visit)) => {
                let mut entry = Json::obj();
                entry.set("path", Json::str(path.clone()));
                entry.set("last", Json::num(visit.t));
                entry.set("age", Json::str(relative_age(now, visit.t)));
                entry
            }
            None => Json::Null,
        };
        node.set("newest", when(newest));
        node.set("oldest", when(oldest));
        out(&(json_dump(&node, 2, 0) + "\n"));
        return Ok(0);
    }

    out("simpledir at a glance:\n\n");
    out(&format!(
        "  aliases: {} ({} missing)\n",
        cfg.aliases.len(),
        missing.len()
    ));
    out(&format!(
        "  tags: {} ({} stale members)\n",
        cfg.tags.len(),
        stale.len()
    ));
    out(&format!(
        "  history: {} ({} alive, {} dead)\n",
        dirs.len(),
        alive,
        dead_history.len()
    ));
    out(&format!(
        "  recording: {}\n",
        if history_enabled(&cfg) { "on" } else { "off" }
    ));
    if let Some((path, visit)) = top {
        out(&format!(
            "  most visited: {} ({:.2})\n",
            path,
            decayed(visit, now)
        ));
    }
    if let Some((path, visit)) = newest {
        out(&format!(
            "  newest visit: {} ({})\n",
            path,
            relative_age(now, visit.t)
        ));
    }
    if let Some((path, visit)) = oldest {
        out(&format!(
            "  oldest visit: {} ({})\n",
            path,
            relative_age(now, visit.t)
        ));
    }
    if missing.is_empty() && stale.is_empty() && dead_history.is_empty() {
        out("\n  nothing to clean up.\n");
    } else {
        out(&format!("\n  clean up: {CONFIG} tidy --dry-run\n"));
    }
    Ok(0)
}

fn cmd_pick(args: &Args) -> Res<i32> {
    use std::process::Stdio;

    let cfg = load_config()?;
    let found = rows(&cfg, &args.word(0));
    if found.is_empty() {
        return Err(die(format!(
            "nothing to pick from. add one: {} add",
            CONFIG
        )));
    }
    let names: Vec<String> = found.iter().map(|(name, _, _)| name.clone()).collect();

    if mv_fzf_available() && env::var_os("SIMPLEDIR_NO_FZF").is_none() {
        // The list goes in on stdin as a pipe, which is the only form fzf
        // accepts. It opens /dev/tty for the keyboard itself.
        let mut feed = String::new();
        for (name, target, _) in &found {
            feed.push_str(name);
            feed.push('\t');
            feed.push_str(target);
            feed.push('\n');
        }
        let cmd = format!(
            "printf '%s' {} | fzf --delimiter='\\t' --with-nth=1,2 --prompt='sd> ' --height=40%",
            shell_quote(&feed)
        );
        let result = Command::new("sh")
            .arg("-c")
            .arg(&cmd)
            .stdin(Stdio::inherit())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .output();
        let output = match result {
            Ok(output) => output,
            Err(_) => return Err(die("couldn't run fzf")),
        };
        if !output.status.success() {
            return Ok(1); // escape, or nothing chosen
        }
        let chosen = trim(&String::from_utf8_lossy(&output.stdout));
        if chosen.is_empty() {
            return Ok(1);
        }
        let name = match chosen.find('\t') {
            Some(tab) => chosen[..tab].to_string(),
            None => chosen.clone(),
        };
        // fzf hands back whatever line it was given; only believe an alias
        // we actually offered.
        if !names.contains(&name) {
            return Err(user(format!(
                "fzf returned '{}', which isn't one of your aliases",
                name
            )));
        }
        out(&format!("{name}\n"));
        return Ok(0);
    }

    let reason = if env::var_os("SIMPLEDIR_NO_FZF").is_some() {
        "fzf disabled (SIMPLEDIR_NO_FZF)"
    } else {
        "no fzf installed"
    };
    out(&format!("{reason}, so: pick a number or type a name\n\n"));
    let width = names.iter().map(|name| name.len()).max().unwrap_or(0);
    for (i, (name, target, missing)) in found.iter().enumerate() {
        let shown = format!("{}{}", target, if *missing { "   [missing]" } else { "" });
        out(&format!(
            "  {:3}  {}{}  {}\n",
            i + 1,
            name,
            " ".repeat(width - name.len()),
            shown
        ));
    }
    out("\n> ");
    flush_out();
    let answer = match read_line() {
        Some(answer) => trim(&answer),
        None => return Ok(1),
    };
    if answer.is_empty() {
        return Ok(1);
    }
    if answer.chars().all(|c| c.is_ascii_digit()) {
        let index = answer.parse::<usize>().unwrap_or(0);
        if index < 1 || index > names.len() {
            return Err(user(format!(
                "no entry {answer}. there are {}.",
                names.len()
            )));
        }
        out(&format!("{}\n", names[index - 1]));
        return Ok(0);
    }
    if !names.contains(&answer) {
        return Err(user(unknown_alias_error(&answer, &cfg)));
    }
    out(&format!("{answer}\n"));
    Ok(0)
}

// ------------------------------------------------------------ history mining

#[derive(Clone)]
struct Candidate {
    path: String,
    hits: i32,
}

// Mine the shell history for directories worth naming. Three formats, one
// shape out, and nothing that isn't an existing directory gets through.
fn history_candidates(limit: i32) -> Res<Vec<Candidate>> {
    let mut files = Vec::new();
    if let Ok(extra) = env::var("SIMPLEDIR_HISTORY") {
        files.push(extra);
    }
    let names = [
        ".bash_history",
        ".zsh_history",
        ".local/share/fish/fish_history",
        ".config/fish/fish_history",
    ];
    for name in names {
        files.push(format!("{}/{name}", home_dir()));
    }

    let cd_words = ["cd", "pushd", "z"];
    let separators = [
        ";", "&&", "||", "|", "(", "{", "!", "then", "else", "do", "sudo", "command", "time",
        "nohup", "builtin",
    ];
    let mut counts: BTreeMap<String, i32> = BTreeMap::new();
    let mut any_file = false;

    for file in &files {
        if !path_exists(file) {
            continue;
        }
        any_file = true;
        let fish = contains(file, "fish_history");
        let raw = match fs::read_to_string(file) {
            Ok(raw) => raw,
            Err(_) => continue, // history is a log: junk and unreadable files don't throw
        };
        for raw_line in raw.lines() {
            let mut line = trim(raw_line);
            if line.is_empty() {
                continue;
            }
            if fish {
                line = match line.find("cmd: ") {
                    Some(at) => line[at + 5..].to_string(),
                    None => String::new(),
                };
            } else if starts_with(&line, ": ") {
                line = match line.find(';') {
                    Some(at) => line[at + 1..].to_string(),
                    None => continue,
                };
            } else if let Some(at) = line.find(';') {
                if line.as_bytes().first().map_or(false, |c| c.is_ascii_digit()) {
                    line.truncate(at);
                }
            }
            if line.is_empty() {
                continue;
            }

            let words = shell_split(&line);
            for (i, word) in words.iter().enumerate() {
                if !cd_words.contains(&word.as_str()) {
                    continue;
                }
                if i > 0 && !separators.contains(&words[i - 1].as_str()) {
                    continue;
                }
                let rest: Vec<String> = words[i + 1..]
                    .iter()
                    .filter(|w| !starts_with(w, "-"))
                    .cloned()
                    .collect();
                if rest.is_empty() || rest[0] == "-" {
                    continue;
                }
                let candidate = if rest.len() > 1 && starts_with(rest.last().unwrap(), "/") {
                    rest.last().unwrap().clone()
                } else {
                    rest[0].clone()
                };
                // Relative targets are resolved against $HOME: history doesn't
                // record which directory you were standing in when you typed them.
                let mut resolved = if starts_with(&candidate, "/") {
                    abspath(&candidate)
                } else {
                    abspath(&format!("{}/{candidate}", home_dir()))
                };
                if !is_dir(&resolved) {
                    let expanded = expand(&candidate);
                    if starts_with(&expanded, "~/") {
                        resolved = abspath(&expanded);
                    }
                }
                if is_dir(&resolved) {
                    *counts.entry(resolved).or_insert(0) += 1;
                }
                break;
            }
        }
    }

    if !any_file {
        let mut looked = String::from("no shell history found. looked for:");
        for name in names {
            looked.push_str("\n  ");
            looked.push_str(&format!("{}/{name}", home_dir()));
        }
        looked.push_str(&format!(
            "\ntell me where yours is: SIMPLEDIR_HISTORY=/path/to/history {} suggest",
            MOVE
        ));
        return Err(die(looked));
    }
    if counts.is_empty() {
        return Err(die(
            "no `cd` targets in your history files. is your history enabled? (HISTFILE)",
        ));
    }

    let cfg = load_config()?;
    let bound: BTreeSet<String> = cfg.aliases.values().map(|path| as_stored(path)).collect();
    let mut out_candidates: Vec<Candidate> = counts
        .into_iter()
        .filter(|(path, _)| !bound.contains(path))
        .map(|(path, hits)| Candidate { path, hits })
        .collect();
    out_candidates.sort_by(|a, b| b.hits.cmp(&a.hits).then_with(|| a.path.cmp(&b.path)));
    if limit > 0 && (limit as usize) < out_candidates.len() {
        out_candidates.truncate(limit as usize);
    }
    Ok(out_candidates)
}

fn cmd_suggest(args: &Args) -> Res<i32> {
    let cfg = load_config()?;
    let limit = parse_limit(args, 10)?;
    let candidates = history_candidates(limit)?;

    if candidates.is_empty() {
        out(&format!(
            "every directory in your history is already bound ({} aliases)\n",
            cfg.aliases.len()
        ));
        return Ok(0);
    }

    if args.has("json") {
        let mut node = Json::obj();
        for c in &candidates {
            node.set(c.path.clone(), Json::num(c.hits as f64));
        }
        out(&(json_dump(&node, 2, 0) + "\n"));
        return Ok(0);
    }

    let width = candidates.iter().map(|c| c.path.len()).max().unwrap_or(0);
    out(&format!(
        "{} new directories from your history, showing the top {}:\n\n",
        candidates.len(),
        candidates.len()
    ));
    for c in &candidates {
        out(&format!(
            "  {:5}x  {}{}\n",
            c.hits,
            c.path,
            " ".repeat(width - c.path.len())
        ));
    }
    out("\nbind them all:\n");
    let mut taken: BTreeSet<String> = cfg.aliases.keys().cloned().collect();
    for c in &candidates {
        let name = name_for(&c.path, &taken);
        out(&format!("  {} add {} {}\n", CONFIG, name, shell_quote(&c.path)));
        taken.insert(name);
    }
    Ok(0)
}

// --------------------------------------------------------------------- adapt

// `sd adapt` is the read-only half: it says what it wants to learn and never
// writes a verdict. `sdcfg adapt` owns the writing.
fn cmd_adapt(args: &Args) -> Res<i32> {
    let limit = parse_limit(args, 10)?;
    let found = adapt_proposals(limit, args.has("all"))?;

    if found.is_empty() {
        let refused = read_verdicts()
            .values()
            .filter(|verdict| verdict.verdict == "no")
            .count();
        out("nothing left to learn. every directory you keep visiting has a name already.\n");
        if refused > 0 {
            out(&format!(
                "  {refused} refused, waiting out their quiet period\n"
            ));
        }
        out(&format!("  {} top shows the whole log, including named ones\n", MOVE));
        return Ok(0);
    }

    if args.has("json") {
        let mut list = Json::arr();
        if let Json::Arr(items) = &mut list {
            for proposal in &found {
                let mut node = Json::obj();
                node.set("path", Json::str(proposal.path.clone()));
                node.set("name", Json::str(proposal.name.clone()));
                node.set("score", mv_json_score(proposal.score));
                node.set("retry", Json::boolean(proposal.retry));
                node.set("refusals", Json::num(proposal.strikes as f64));
                items.push(node);
            }
        }
        out(&(json_dump(&list, 2, 0) + "\n"));
        return Ok(0);
    }

    out(&format!(
        "{} director{} you keep visiting that {} no name, most recent first:\n\n",
        found.len(),
        if found.len() == 1 { "y" } else { "ies" },
        if found.len() == 1 { "has" } else { "have" }
    ));
    for proposal in &found {
        let mut line = format!(
            "  {:6.2}  {}{}  {}",
            proposal.score,
            proposal.name,
            " ".repeat(12 - std::cmp::min(12, proposal.name.len())),
            proposal.path
        );
        if proposal.retry {
            line.push_str(&format!(
                "   [you said no {}x, asking once more]",
                proposal.strikes
            ));
        } else if proposal.refused {
            let hours = (proposal.quiet_left / 3600.0) as i64;
            let quiet = if hours < 48 {
                format!("{hours}h")
            } else {
                format!("{}d", hours / 24)
            };
            line.push_str(&format!("   [refused, quiet for {quiet}]"));
        }
        line.push('\n');
        out(&line);
    }
    out("\nkeep one:\n");
    out(&format!(
        "  {} adapt --accept <name>          bind it as well as remember it\n",
        CONFIG
    ));
    out(&format!(
        "  {} adapt --reject <name>          stop asking\n",
        CONFIG
    ));
    out(&format!(
        "  {} adapt --accept <name> --no-bind just remember the verdict\n",
        CONFIG
    ));
    out("\nforget every verdict:\n");
    out(&format!("  {} adapt --clear\n", CONFIG));
    Ok(0)
}

// -------------------------------------------------------------------- sha-256

const MV_SHA256_K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

fn mv_rotr(x: u32, n: u32) -> u32 {
    (x >> n) | (x << (32 - n))
}

fn sha256_hex_bytes(data: &[u8]) -> String {
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
        0x5be0cd19,
    ];
    let bits = (data.len() as u64).wrapping_mul(8);
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    for i in (0..8).rev() {
        msg.push(((bits >> (i * 8)) & 0xff) as u8);
    }

    for chunk in msg.chunks_exact(64) {
        let mut w = [0u32; 64];
        for (i, word) in w[..16].iter_mut().enumerate() {
            let p = &chunk[i * 4..i * 4 + 4];
            *word = ((p[0] as u32) << 24)
                | ((p[1] as u32) << 16)
                | ((p[2] as u32) << 8)
                | (p[3] as u32);
        }
        for i in 16..64 {
            let s0 = mv_rotr(w[i - 15], 7) ^ mv_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            let s1 = mv_rotr(w[i - 2], 17) ^ mv_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let (mut a, mut b, mut c, mut d) = (h[0], h[1], h[2], h[3]);
        let (mut e, mut f, mut g, mut hh) = (h[4], h[5], h[6], h[7]);
        for i in 0..64 {
            let s1 = mv_rotr(e, 6) ^ mv_rotr(e, 11) ^ mv_rotr(e, 25);
            let ch = (e & f) ^ ((!e) & g);
            let t1 = hh
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(MV_SHA256_K[i])
                .wrapping_add(w[i]);
            let s0 = mv_rotr(a, 2) ^ mv_rotr(a, 13) ^ mv_rotr(a, 22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
        h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g);
        h[7] = h[7].wrapping_add(hh);
    }

    let mut digest = String::with_capacity(64);
    for word in h {
        digest.push_str(&format!("{word:08x}"));
    }
    digest
}

fn cmd_sha256(args: &Args) -> Res<i32> {
    let what = args.word(0);
    if what.is_empty() {
        return Err(usage(format!(
            "which file? {} sha256 <file>\n  to check a download against its release: {} sha256 sd-linux-x86_64",
            MOVE, MOVE
        )));
    }
    if what == "-" {
        let mut all = Vec::new();
        let _ = io::stdin().read_to_end(&mut all);
        out(&format!("{}  -\n", sha256_hex_bytes(&all)));
        return Ok(0);
    }
    let path = abspath(&what);
    if !path_exists(&path) {
        return Err(die(format!("no such file: {path}")));
    }
    if is_dir(&path) {
        return Err(die(format!(
            "that's a directory: {path}\n  hash a file, not a directory"
        )));
    }
    // Hash raw bytes: release assets are binaries, not UTF-8 text.
    let data = fs::read(&path).map_err(|e| user(format!("can't read {path}: {e}")))?;
    out(&format!("{}  {}\n", sha256_hex_bytes(&data), what));
    Ok(0)
}
// ------------------------------------------------------------- installation
//
// Ported from sd.cpp's installation / release / update block. The split is the
// whole point: everything here belongs to `sdcfg`, never to `sd`. Only this
// section (and the nudge at the bottom) is allowed to touch the network.

// The asset name carries the architecture, and it has to match what `make assets`
// and install.sh publish: sd-linux-x86_64 / sd-linux-arm64. `uname -m` in C++
// becomes std::env::consts::ARCH here; the aliases are the same shape.
fn arch_tag() -> String {
    let machine = std::env::consts::ARCH;
    if machine == "aarch64" || machine == "arm64" {
        return "arm64".to_string();
    }
    if machine == "x86_64" || machine == "amd64" {
        return "x86_64".to_string();
    }
    machine.to_string()
}

fn asset_name() -> String {
    format!("sd-linux-{}", arch_tag())
}

// SIMPLEDIR_BIN is a raw override (C++ used getenv, not env_or, so even an
// empty value is honoured). Otherwise prefer the conventional location, then
// wherever this running executable actually lives.
fn install_target() -> String {
    if let Ok(v) = env::var("SIMPLEDIR_BIN") {
        return v;
    }
    let standard = home_dir() + "/.local/bin/sd";
    if path_exists(&standard) {
        return standard;
    }
    if let Ok(p) = fs::read_link("/proc/self/exe") {
        let self_path = p.to_string_lossy().to_string();
        if path_exists(&self_path) {
            return self_path;
        }
    }
    standard
}

// popen(..., "r") is just `/bin/sh -c` with stdout piped. `output()` captures
// stdout; every caller here already redirects stderr to /dev/null in the
// command string, so nothing leaks that C++ would have let through.
fn run_capture(cmd: &str) -> (String, i32) {
    match Command::new("sh").arg("-c").arg(cmd).output() {
        Ok(o) => {
            let text = String::from_utf8_lossy(&o.stdout).to_string();
            let code = o.status.code().unwrap_or(-1);
            (text, code)
        }
        Err(_) => (String::new(), -1),
    }
}

// TLS isn't in the standard library, so downloads go through curl, which the
// installer already requires.
fn download(url: &str, dest: &str) -> bool {
    let cmd = format!(
        "curl -fsSL --retry 2 --connect-timeout 15 {} -o {} 2>/dev/null",
        shell_quote(url),
        shell_quote(dest)
    );
    let (_, status) = run_capture(&cmd);
    status == 0 && path_exists(dest)
}

// The releases *list*, not the repository object. Parse with the file's own
// Json parser: GitHub's upload_url contains a literal `{?name,label}` inside a
// string, which defeated the old brace-scanning version of this function.
fn releases(limit: i32) -> Res<Vec<(String, String, String)>> {
    let url = env_or("SIMPLEDIR_UPDATE_URL", API);
    let cmd = format!(
        "curl -fsSL --connect-timeout 15 {} 2>/dev/null",
        shell_quote(&url)
    );
    let (body, _) = run_capture(&cmd);
    if trim(&body).is_empty() {
        return Ok(Vec::new());
    }
    let root = match json_parse(&body) {
        Ok(v) => v,
        Err(_) => return Ok(Vec::new()),
    };
    if !root.is_arr() {
        return Ok(Vec::new());
    }
    let mut found: Vec<(String, String, String)> = Vec::new();
    if let Json::Arr(items) = &root {
        for entry in items {
            if !entry.is_obj() {
                continue;
            }
            let tag = match entry.get("tag_name") {
                Some(t) => t.as_str(),
                None => continue,
            };
            if tag.is_empty() {
                continue;
            }
            let published = match entry.get("published_at") {
                Some(p) => p.as_str().chars().take(10).collect(),
                None => String::new(),
            };
            let name = match entry.get("name") {
                Some(n) => n.as_str(),
                None => String::new(),
            };
            found.push((tag, published, name));
            if found.len() as i32 >= limit {
                break;
            }
        }
    }
    Ok(found)
}

// `update` only ever moves between stable releases. GitHub's first release can
// be a prerelease, so the list has to be filtered rather than trusted in order;
// `sdcfg releases` deliberately shows everything.
fn stable_release_tag(tag: &str) -> bool {
    let rest = match tag.strip_prefix('v') {
        Some(r) => r,
        None => return false,
    };
    let mut parts = rest.split('.');
    match (parts.next(), parts.next(), parts.next(), parts.next()) {
        (Some(a), Some(b), Some(c), None) => {
            [a, b, c].iter().all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()))
        }
        _ => false,
    }
}

// A tag has to look like a tag. vX.Y.Z with an optional prerelease suffix,
// lowercase letters only, no leading zeros in a numeric component. Saying it
// here beats a 404 that doesn't explain what went wrong.
fn normalize_tag(raw: &str) -> Res<String> {
    let mut want = trim(raw);
    if want.is_empty() {
        return Err(usage(format!("which version? try {CONFIG} update --to v6.0.0")));
    }
    if !want.starts_with('v') {
        want = format!("v{want}");
    }
    let rest = &want[1..];
    let mut shaped = rest.len() >= 5 && rest.as_bytes()[0].is_ascii_digit();
    let mut dots = 0i32;
    for c in rest.chars() {
        if c == '.' {
            dots += 1;
        } else if !(c.is_ascii_digit() || c == '-' || ('a'..='z').contains(&c)) {
            shaped = false;
        }
        if !shaped {
            break;
        }
    }
    if shaped && dots == 2 {
        let mut start = 0usize;
        for _ in 0..3 {
            let stop = rest[start..].find('.').map(|p| start + p);
            let piece_full = match stop {
                Some(s) => &rest[start..s],
                None => &rest[start..],
            };
            let piece = match piece_full.find('-') {
                Some(d) => &piece_full[..d],
                None => piece_full,
            };
            if piece.len() > 1 && piece.starts_with('0') {
                shaped = false;
            }
            match stop {
                Some(s) => start = s + 1,
                None => break,
            }
        }
    }
    if !shaped || dots != 2 {
        return Err(usage(format!(
            "'{raw}' isn't a version. releases look like v6.0.0 or v6.1.0-rc.1\n  see what exists: {CONFIG} releases"
        )));
    }
    Ok(want)
}

// ------------------------------------------------------------------- sha-256
//
// Forty lines of arithmetic beats a crypto dependency. This verifies that two
// files arrived together; it does not authenticate who published them.

const NW_SHA_K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

fn nw_rotr(x: u32, n: u32) -> u32 {
    (x >> n) | (x << (32 - n))
}

fn nw_sha256_hex(data: &[u8]) -> String {
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
        0x5be0cd19,
    ];
    let bits = (data.len() as u64).wrapping_mul(8);
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    for i in (0..8).rev() {
        msg.push(((bits >> (i * 8)) & 0xff) as u8);
    }
    for chunk in msg.chunks_exact(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            let p = &chunk[i * 4..i * 4 + 4];
            w[i] = ((p[0] as u32) << 24)
                | ((p[1] as u32) << 16)
                | ((p[2] as u32) << 8)
                | (p[3] as u32);
        }
        for i in 16..64 {
            let s0 = nw_rotr(w[i - 15], 7) ^ nw_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            let s1 = nw_rotr(w[i - 2], 17) ^ nw_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let (mut a, mut b, mut c, mut d) = (h[0], h[1], h[2], h[3]);
        let (mut e, mut f, mut g, mut hh) = (h[4], h[5], h[6], h[7]);
        for i in 0..64 {
            let s1 = nw_rotr(e, 6) ^ nw_rotr(e, 11) ^ nw_rotr(e, 25);
            let ch = (e & f) ^ ((!e) & g);
            let t1 = hh
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(NW_SHA_K[i])
                .wrapping_add(w[i]);
            let s0 = nw_rotr(a, 2) ^ nw_rotr(a, 13) ^ nw_rotr(a, 22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
        h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g);
        h[7] = h[7].wrapping_add(hh);
    }
    let mut result = String::with_capacity(64);
    for word in h {
        for i in (0..4).rev() {
            let byte = (word >> (i * 8)) & 0xff;
            result.push(char::from_digit(byte >> 4, 16).unwrap());
            result.push(char::from_digit(byte & 0xf, 16).unwrap());
        }
    }
    result
}

// The sha256 a sums file publishes for `name`. Format is sha256sum output:
// "<64 hex>  <name>", two spaces, names bare. "Missing" and "malformed" must
// stay different: an absent sum warns, a listed-but-bogus one is tampering.
enum SumLookup {
    Missing,
    Listed(String),
    Malformed,
}

fn published_sum(sums: &str, name: &str) -> SumLookup {
    for line in sums.lines() {
        let mut line = line.to_string();
        while line.ends_with('\r') || line.ends_with(' ') {
            line.pop();
        }
        let gap = match line.find(|c: char| c == ' ' || c == '\t') {
            Some(g) => g,
            None => continue,
        };
        let digest = &line[..gap];
        let mut listed = &line[gap..];
        while listed.starts_with(' ') || listed.starts_with('\t') {
            listed = &listed[1..];
        }
        if listed.starts_with('*') {
            listed = &listed[1..];
        }
        if listed != name {
            continue;
        }
        if digest.len() != 64 || !digest.bytes().all(|c| c.is_ascii_hexdigit()) {
            return SumLookup::Malformed;
        }
        return SumLookup::Listed(digest.to_lowercase());
    }
    SumLookup::Missing
}

// Where the SHA256SUMS for a release lives. `latest` is a redirect like the
// asset itself, so it must be spelled the same way or the two can come from
// different releases -- which would report every download as tampered.
fn nw_sums_url_for(base: &str, tag: &str) -> String {
    let stem = if tag == "latest" {
        format!("{base}/latest/download/")
    } else {
        format!("{base}/download/{tag}/")
    };
    stem + "SHA256SUMS"
}

// std::atof: parse the longest numeric prefix, returning 0.0 when there isn't
// one. Rust's str::parse insists on the whole string, so version comparisons
// ("12.0.0 - rewritten in Rust") need this.
fn nw_atof(s: &str) -> f64 {
    let s = s.trim_start();
    let b = s.as_bytes();
    let mut end = 0usize;
    if end < b.len() && (b[end] == b'+' || b[end] == b'-') {
        end += 1;
    }
    let mut digits = 0usize;
    while end < b.len() && b[end].is_ascii_digit() {
        end += 1;
        digits += 1;
    }
    if end < b.len() && b[end] == b'.' {
        end += 1;
        while end < b.len() && b[end].is_ascii_digit() {
            end += 1;
            digits += 1;
        }
    }
    if digits == 0 {
        return 0.0;
    }
    if end < b.len() && (b[end] == b'e' || b[end] == b'E') {
        let mut e = end + 1;
        if e < b.len() && (b[e] == b'+' || b[e] == b'-') {
            e += 1;
        }
        let exp_start = e;
        while e < b.len() && b[e].is_ascii_digit() {
            e += 1;
        }
        if e > exp_start {
            end = e;
        }
    }
    s[..end].parse::<f64>().unwrap_or(0.0)
}

// Download a release, check it really is us and really is the version asked
// for, then swap it in. The old binary is kept as `sd.previous`, which is what
// `revert` puts back. Returns the *new version times 1000*, not an exit code.
fn install_release(tag: &str, allow_older: bool) -> Res<i32> {
    // SIMPLEDIR_RELEASE_URL is a *base*, exactly as install.sh treats it, so
    // tests can point it at a file:// tree laid out like a real release.
    let base = env_or("SIMPLEDIR_RELEASE_URL", RELEASES);

    // The asset name has changed twice: sd-linux-<arch> (v6+), sd (v5), and
    // simpledir (v3). Each candidate remembers where it came from so the
    // message afterwards doesn't have to guess from the hostname.
    let mut urls: Vec<(String, bool)> = Vec::new(); // (url, from_source)
    if let Ok(override_url) = env::var("SIMPLEDIR_UPDATE_ASSET_URL") {
        urls.push((override_url, false));
    } else {
        let stem = if tag == "latest" {
            format!("{base}/latest/download/")
        } else {
            format!("{base}/download/{tag}/")
        };
        urls.push((stem.clone() + &asset_name(), false));
        urls.push((stem.clone() + "sd", false));
        urls.push((stem + "simpledir", false));
        // v1 and v2 never had a release asset, but the program is right there
        // in the tag. Going back to source is the last resort.
        let mut raw = env_or("SIMPLEDIR_SOURCE_URL", RAW_BASE);
        if raw.ends_with('/') {
            raw.pop();
        }
        urls.push((format!("{raw}/{OWNER}/{REPO}/{tag}/sd"), true));
        urls.push((format!("{raw}/{OWNER}/{REPO}/{tag}/simpledir"), true));
    }

    let target = install_target();
    if let Some(parent) = Path::new(&target).parent() {
        let _ = fs::create_dir_all(parent);
    }
    let staged = format!("{target}.new.{}", unsafe { getpid() });

    let mut url = urls[0].0.clone();
    let mut fetched = false;
    let mut from_source = false;
    for (candidate, source) in &urls {
        if download(candidate, &staged) {
            url = candidate.clone();
            from_source = *source;
            fetched = true;
            break;
        }
    }
    if !fetched {
        let tried: Vec<String> = urls.iter().map(|(u, _)| u.clone()).collect();
        let _ = fs::remove_file(&staged);
        return Err(die(format!(
            "download failed for {tag}:\n  tried {}\n  check the tag with {CONFIG} releases",
            join(&tried, "\n         ")
        )));
    }

    // A missing checksum warns and continues (old mirrors, v1/v2 have none).
    // A *wrong* checksum is fatal before anything is run or replaced.
    if !from_source {
        let sums_staged = format!("{staged}.sums");
        let have_sums = download(&nw_sums_url_for(&base, tag), &sums_staged);
        let lookup = if have_sums {
            published_sum(&read_file(&sums_staged)?, &asset_name())
        } else {
            SumLookup::Missing
        };
        let _ = fs::remove_file(&sums_staged);
        let listed = match lookup {
            SumLookup::Listed(sum) => Some(sum),
            SumLookup::Missing => None,
            SumLookup::Malformed => {
                let _ = fs::remove_file(&staged);
                return Err(die(format!(
                    "the published SHA256SUMS has a malformed checksum for {}.\n  nothing was run or replaced. a listed-but-broken digest is not the same as\n  an old release with no checksum at all. try another mirror, or build the source:\n    {CONFIG} update --to {tag}",
                    asset_name()
                )));
            }
        };
        if let Some(listed) = listed {
            // The staged file is a binary: fs::read, not read_to_string.
            let bytes = fs::read(&staged)
                .map_err(|e| user(format!("can't read {staged}: {e}")))?;
            let actual = nw_sha256_hex(&bytes);
            if actual != listed {
                let _ = fs::remove_file(&staged);
                return Err(die(format!(
                    "the downloaded {} does not match the checksum this release published.\n  published: {listed}\n  downloaded: {actual}\n  nothing was changed. that is a corrupted download, a mirror serving something\n  else, or a release nobody signed off on -- try again, or build the source:\n    {CONFIG} update --to {tag}  (with SIMPLEDIR_RELEASE_URL pointed at a mirror you trust)",
                    asset_name()
                )));
            }
            out("  verified against the published SHA256SUMS\n");
        } else {
            out(&format!(
                "  no published checksum for {}, so it could not be verified\n",
                asset_name()
            ));
        }
    }

    if from_source {
        out("  that release has no binary attached, so this is the source file from the tag\n");
    }

    // Verify before replacing a working install. 0755, like the C++ perm set.
    let _ = fs::set_permissions(&staged, fs::Permissions::from_mode(0o755));
    let (reported_raw, status) =
        run_capture(&format!("{} --version 2>/dev/null", shell_quote(&staged)));
    let reported = trim(&reported_raw);
    if status != 0 {
        let _ = fs::remove_file(&staged);
        return Err(die(format!(
            "the download isn't runnable: {url}\n  if that 404s, the release has no asset for {} yet",
            arch_tag()
        )));
    }
    // It has to identify as us. Releases before v5 called the command
    // `simpledir`, so that name counts too.
    let is_sd = starts_with(&reported, &format!("{MOVE} "));
    let is_legacy = starts_with(&reported, &format!("{LEGACY_ID} "));
    if !is_sd && !is_legacy {
        let _ = fs::remove_file(&staged);
        return Err(die(format!(
            "the download isn't {MOVE}: {reported}\n  nothing was changed"
        )));
    }
    let name = if is_sd { MOVE } else { LEGACY_ID };
    let tail = reported.get(name.len() + 1..).unwrap_or("");
    let got = nw_atof(tail);
    let mut got_version = trim(tail);
    if let Some(sp) = got_version.find(' ') {
        got_version = got_version[..sp].to_string();
    }

    if !allow_older && got <= nw_atof(VERSION) {
        let _ = fs::remove_file(&staged);
        out(&format!(
            "  that asset is v{got_version}, same as what you have. not changing anything\n"
        ));
        return Ok(0);
    }

    // The tag once pointed at a commit whose VERSION had already moved on.
    // Install what the tag contains, but say so.
    let mut wanted = if tag == "latest" { String::new() } else { trim(tag) };
    if wanted.starts_with('v') {
        wanted = wanted[1..].to_string();
    }
    if !wanted.is_empty() && got_version != wanted {
        out(&format!(
            "  note: you asked for {tag}, and that release's own --version says {got_version}.\n  the tag was cut from a commit whose version string had already moved on. installing what\n  {tag} actually contains.\n"
        ));
    }

    if path_exists(&target) {
        // Keep the way back atomic too. A crash mid-copy used to leave a
        // half-written .previous, which is the one file a downgrade needs.
        let previous = target.clone() + ".previous";
        let backup_stage = format!("{previous}.new.{}", unsafe { getpid() });
        if fs::copy(&target, &backup_stage).is_ok() {
            let _ = fs::set_permissions(&backup_stage, fs::Permissions::from_mode(0o755));
            let _ = fs::rename(&backup_stage, &previous);
        } else {
            let _ = fs::remove_file(&backup_stage);
        }
    }
    if let Err(e) = fs::rename(&staged, &target) {
        let _ = fs::remove_file(&staged);
        return Err(die(format!(
            "couldn't replace {target}: {e}\n  nothing was changed"
        )));
    }

    if is_legacy {
        out(&format!(
            "  note: v{got_version} calls itself `{LEGACY_ID}`, not `{MOVE}`. it predates the two-command split,\n  so there is no `{CONFIG}` half in this version.\n"
        ));
    }
    if got < 6.0 {
        out(&format!(
            "  heads up: v{got_version} is the old python build. it has no `{CONFIG} revert` and no `update --to`,\n  so it cannot bring you back here.\n"
        ));
        if path_exists(&(target.clone() + ".previous")) {
            out(&format!(
                "  to undo this, in a new shell:\n\n    cp {target}.previous {target}\n"
            ));
        } else {
            out(&format!(
                "  to undo this, re-run the installer:\n    curl -fsSL https://raw.githubusercontent.com/{REPO}/main/install.sh | bash\n"
            ));
        }
    }
    Ok((got * 1000.0) as i32)
}

fn cmd_update(args: &Args) -> Res<i32> {
    // `--to` names the version, so it never needs the release list. Asking for
    // it first made an explicit install fail whenever the API was down.
    if args.saw("to") {
        let want = normalize_tag(&args.value("to"))?;
        out(&format!(
            "{CONFIG} {VERSION} installed, installing {want} over {}\n",
            install_target()
        ));
        let installed = install_release(&want, true)?;
        if installed != 0 {
            out(&format!("  done. the previous one is at {}.previous\n", install_target()));
        }
        out(&format!("  new shell needed if {MOVE} gained subcommands\n"));
        return Ok(0);
    }

    let found = releases(30)?;
    if found.is_empty() {
        return Err(die(format!(
            "couldn't reach GitHub to check for updates.\n  the unauthenticated api allows 60 requests an hour per address; wait, or\n  install a version directly: {CONFIG} update --to v6.0.0\n  releases are also listed at {REPO}/releases"
        )));
    }
    let latest = match found.iter().find(|r| stable_release_tag(&r.0)) {
        Some(r) => r.0.clone(),
        None => {
            return Err(die(format!(
                "there are published releases, but none is a stable vX.Y.Z.\n  update never installs a prerelease. list them with: {CONFIG} releases"
            )));
        }
    };
    out(&format!(
        "{CONFIG} {VERSION} installed, newest release is {latest}\n"
    ));

    let have = nw_atof(VERSION);
    let bare_latest: String = latest.chars().skip_while(|c| *c == 'v').collect();
    let newest = nw_atof(&bare_latest);
    if newest <= have {
        out("you're up to date\n");
        return Ok(0);
    }
    if args.has("check") {
        out(&format!(
            "update available: {latest} (exit 1 means 'there is one')\n"
        ));
        return Ok(1);
    }

    let target = install_target();
    if !args.has("yes") {
        if unsafe { isatty(0) } != 1 {
            out(&format!(
                "  not a terminal, so not asking. install it with: {CONFIG} update --yes\n"
            ));
            return Ok(1);
        }
        out(&format!("  install {latest} over {target}? [y/N] "));
        flush_out();
        match read_line() {
            Some(a) if lower(&trim(&a)) == "y" => {}
            _ => {
                out("  ok, leaving it alone\n");
                return Ok(0);
            }
        }
    }
    let installed = install_release(&latest, false)?;
    if installed != 0 {
        let short = latest.get(1..).unwrap_or("");
        out(&format!("updated to {short} at {target}\n"));
        out(&format!(
            "  the old one is kept at {target}.previous. go back: {CONFIG} revert\n"
        ));
    }
    out(&format!("  new shell needed if {MOVE} gained subcommands\n"));
    Ok(0)
}

// Put back whatever was installed before the last update.
fn cmd_revert(args: &Args) -> Res<i32> {
    let target = install_target();
    let previous = target.clone() + ".previous";

    if args.saw("to") {
        let want = normalize_tag(&args.value("to"))?;
        install_release(&want, true)?;
        out(&format!("now running {want} at {target}\n"));
        return Ok(0);
    }

    if !path_exists(&previous) {
        return Err(die(format!(
            "nothing to go back to. no {previous}.\n  install a specific version instead: {CONFIG} update --to v5.0.0"
        )));
    }
    let (reported_raw, status) =
        run_capture(&format!("{} --version 2>/dev/null", shell_quote(&previous)));
    let reported = trim(&reported_raw);
    if status != 0 || !starts_with(&reported, &format!("{MOVE} ")) {
        return Err(die(format!(
            "{previous} isn't runnable. delete it, or use {CONFIG} update --to <version>"
        )));
    }

    // A .previous holding the version already installed means a previous revert
    // ran. Saying "back to X" while replacing it with the same file is a lie.
    let (have_raw, _) = run_capture(&format!("{} --version 2>/dev/null", shell_quote(&target)));
    let have = trim(&have_raw);
    if have == reported {
        out(&format!(
            "nothing to do: {previous} is the same version you're already on ({reported})\n  install a specific one instead: {CONFIG} update --to v6.0.0\n"
        ));
        return Ok(0);
    }

    // Stage the copied bytes beside the target, make them executable, then
    // rename. Copying straight over the running binary was neither atomic nor
    // mode-preserving: a revert left 0744 and a crash left a truncated sd.
    let restore = format!("{target}.revert.{}", unsafe { getpid() });
    if let Err(e) = fs::copy(&previous, &restore) {
        return Err(die(format!("couldn't put the old version back: {e}")));
    }
    let _ = fs::set_permissions(&restore, fs::Permissions::from_mode(0o755));
    if let Err(e) = fs::rename(&restore, &target) {
        let _ = fs::remove_file(&restore);
        return Err(die(format!("couldn't put the old version back: {e}")));
    }
    out(&format!("back to {reported}, installed at {target}\n"));
    out(&format!(
        "  (it was {VERSION}. new shell needed if {MOVE} gained subcommands)\n"
    ));
    Ok(0)
}

fn cmd_releases(args: &Args) -> Res<i32> {
    let _ = args;
    let found = releases(15)?;
    if found.is_empty() {
        return Err(die(format!(
            "couldn't reach GitHub to list releases. try again, or see {REPO}"
        )));
    }
    out("published releases:\n\n");
    for (tag, published, name) in &found {
        let mark = if *tag == format!("v{VERSION}") {
            "  <- you are here"
        } else {
            ""
        };
        out(&format!(
            "  {tag}  {published}{}{name}{mark}\n",
            " ".repeat(9)
        ));
    }
    Ok(0)
}

// ------------------------------------------------------------------- updates
//
// One stderr line if a newer release exists. At most one network call a day,
// only on a terminal, and silent if anything at all goes wrong. Never on the
// jump path.

fn nw_update_cache() -> String {
    config_dir() + "/update-check.json"
}

fn nw_read_update_cache() -> String {
    let path = nw_update_cache();
    if !path_exists(&path) {
        return String::new();
    }
    let raw = match read_file(&path) {
        Ok(r) => r,
        Err(_) => return String::new(),
    };
    let root = match json_parse(&raw) {
        Ok(v) => v,
        Err(_) => return String::new(),
    };
    if !root.is_obj() {
        return String::new();
    }
    match root.get("latest") {
        Some(v) => v.as_str(),
        None => String::new(),
    }
}

fn nw_write_update_cache(latest: &str) {
    let mut root = Json::obj();
    root.set("checked", Json::num(now_seconds()));
    root.set("latest", Json::str(latest.to_string()));
    // A read-only home shouldn't break `ls`.
    let _ = write_atomic(&nw_update_cache(), &(json_dump(&root, 2, 0) + "\n"), false);
}

fn nudge() {
    if env::var_os("SIMPLEDIR_NO_UPDATE_CHECK").is_some() {
        return;
    }
    if unsafe { isatty(2) } != 1 {
        return;
    }
    let cache = nw_update_cache();
    let mut latest = nw_read_update_cache();
    let mut checked = 0.0f64;
    if path_exists(&cache) {
        if let Ok(raw) = read_file(&cache) {
            if let Ok(root) = json_parse(&raw) {
                if root.is_obj() {
                    if let Some(c) = root.get("checked") {
                        checked = c.as_num();
                    }
                }
            }
        }
    }
    if latest.is_empty() || now_seconds() - checked > 86400.0 {
        let found = match releases(30) {
            Ok(f) => f,
            Err(_) => return,
        };
        if found.is_empty() {
            return; // offline, rate limited, api changed: say nothing
        }
        latest = match found.iter().find(|r| stable_release_tag(&r.0)) {
            Some(r) => r.0.clone(),
            None => return, // only prereleases: don't nudge toward half-finished work
        };
        nw_write_update_cache(&latest);
    }
    if latest.is_empty() {
        return;
    }
    let bare: String = latest.chars().skip_while(|c| *c == 'v').collect();
    if nw_atof(&bare) > nw_atof(VERSION) {
        err(&format!(
            "{MOVE}: v{bare} is out (you're on v{VERSION}). `{CONFIG} update` installs it.\n"
        ));
    }
}
// config_b — the second half of the config-side command family.
//
// Ported from sd.cpp: adapt --accept/--reject, edit, init, prompt,
// completions, uninstall, doctor and bench. No crates, no main: this file is
// concatenated into the crate that defines AppError/Res/out/err/Config/Args
// and the shared json/path/config/history/adapt helpers.
//
// Every private item here is prefixed `cb_` so it cannot collide with a
// sibling section.

// ---------------------------------------------------------------- adapt apply

fn cmd_adapt_apply(args: &Args) -> Res<i32> {
    if args.has("clear") {
        if !path_exists(&adapt_file()) {
            out("no verdicts recorded yet\n");
            return Ok(0);
        }
        let _ = fs::remove_file(&adapt_file());
        out(&format!(
            "forgot every verdict. {MOVE} adapt will ask about anything you refused again.\n"
        ));
        out(&format!(
            "  names you already bound are names, not verdicts — {CONFIG} rm to unbind one\n"
        ));
        return Ok(0);
    }

    let verdict = if args.has("accept") {
        "yes"
    } else if args.has("reject") {
        "no"
    } else {
        ""
    };
    if verdict.is_empty() {
        return Err(usage(format!(
            "say yes or no: {CONFIG} adapt --accept <name>, or --reject <name>"
        )));
    }

    // --accept/--reject are switches: the name is the first *word*, not the
    // second argument.
    let wanted = args.word(0);
    if wanted.is_empty() {
        return Err(usage(format!("which one? {CONFIG} adapt --accept <name>")));
    }

    // Resolve through the proposals, not by trusting a bare name to be unique.
    let found = adapt_proposals(0, true)?;
    let mut target = String::new();
    for p in &found {
        if p.name == wanted || p.path == wanted {
            target = p.path.clone();
            break;
        }
    }
    if target.is_empty() {
        let mut msg = format!("'{wanted}' isn't one of the proposals");
        if !found.is_empty() {
            msg.push_str(&format!(". run `{MOVE} adapt` to see them"));
        }
        return Err(user(msg));
    }

    let mut verdicts = read_verdicts();
    let now = now_seconds();
    let v = verdicts.entry(target.clone()).or_default();
    v.verdict = verdict.to_string();
    v.at = now;
    v.strikes = if verdict == "no" { v.strikes + 1 } else { 0 };
    if verdict == "yes" {
        v.strikes = 0;
    }
    write_verdicts(&verdicts);

    let mut cfg = load_config()?;
    let mut bound_name = String::new();
    if verdict == "yes" && !args.has("no-bind") {
        let taken: BTreeSet<String> = cfg.aliases.keys().cloned().collect();
        let name = name_for(&target, &taken);
        if let Some(existing) = cfg.aliases.get(&name) {
            out(&format!(
                "not binding: the name '{name}' is already taken by {existing}\n"
            ));
        } else {
            cfg.aliases.insert(name.clone(), target.clone());
            save_config(&cfg)?;
            bound_name = name;
        }
    }

    if verdict == "no" {
        let v = &verdicts[&target];
        let days = quiet_for(v) / (24.0 * 3600.0);
        out(&format!("noted. {target}\n"));
        if days < 1.0 {
            // std::to_string in the original prints six decimals here.
            out(&format!("  quiet for {:.6} hours", days * 24.0));
        } else {
            out(&format!("  quiet for {} days", days as i64));
        }
        if v.strikes > 1 {
            out(&format!(", doubled because that's refusal {}", v.strikes));
        }
        out("\n  it'll ask again once that's up, then stop asking entirely after a year\n");
    } else if !bound_name.is_empty() {
        out(&format!("bound {bound_name} -> {target}\n"));
        out(&format!("  {MOVE} {bound_name} now\n"));
    } else {
        out(&format!("noted, not bound: {target}\n"));
    }
    Ok(0)
}

// ----------------------------------------------------------------------- edit

fn cmd_edit(args: &Args) -> Res<i32> {
    if !path_exists(&config_file()) {
        // The C++ default Config is version 1 with history on; derive(Default)
        // in the Rust core is not, so spell the fields out.
        let empty = Config {
            version: 1,
            history: true,
            aliases: BTreeMap::new(),
            tags: Vec::new(),
        };
        save_config(&empty)?;
    }
    let word = args.word(0);
    let editor = if word.is_empty() {
        env_or("EDITOR", &env_or("VISUAL", "vi"))
    } else {
        word
    };
    let cmd = format!("{} {}", editor, shell_quote(&config_file()));
    match Command::new("sh").arg("-c").arg(&cmd).status() {
        Ok(status) if status.success() => Ok(0),
        _ => Err(die("editor exited with an error")),
    }
}

// ----------------------------------------------------------------------- init

fn cmd_init() -> Res<i32> {
    let bindir = match fs::read_link("/proc/self/exe") {
        Ok(p) => match p.parent() {
            Some(parent) => parent.to_string_lossy().to_string(),
            None => env::current_dir().unwrap_or_default().to_string_lossy().to_string(),
        },
        Err(_) => env::current_dir().unwrap_or_default().to_string_lossy().to_string(),
    };

    // Every statement ends in `;`, there is no glob and no `#` before the last
    // construct, and the prefix tests use `${1:0:1}` rather than a pattern.
    // That is what makes `eval $(sdcfg init)` behave like `eval "$(sdcfg init)"`.
    out(&format!(
        "if ! command -v {MOVE} >/dev/null 2>&1; then export PATH=\"{bindir}:$PATH\"; fi;\n"
    ));
    out("export SD_ALIAS=;\n");
    out("export SD_JUMPED_TO=;\n");
    out("_sd_jump() {\n");
    out("  if [ -z \"${1-}\" ]; then SD_ALIAS=; SD_JUMPED_TO=; export SD_ALIAS SD_JUMPED_TO; builtin cd -- \"$HOME\" && return $?; fi;\n");
    out("  if [ \"${1-}\" = \"-\" ]; then SD_ALIAS=; SD_JUMPED_TO=; export SD_ALIAS SD_JUMPED_TO; builtin cd -- \"$OLDPWD\" && return $?; fi;\n");
    out("  if [ \"${1:0:1}\" = \"/\" ]; then SD_ALIAS=; SD_JUMPED_TO=; export SD_ALIAS SD_JUMPED_TO; builtin cd -- \"$1\" && return $?; fi;\n");
    out("  if [ \"${1:0:1}\" = \"~\" ]; then SD_ALIAS=; SD_JUMPED_TO=; export SD_ALIAS SD_JUMPED_TO; builtin cd -- \"${1/#\\~/$HOME}\" && return $?; fi;\n");
    out("  local _sd_dir;\n");
    out(&format!("  _sd_dir=$(command {MOVE} print \"$@\") || return $?;\n"));
    out("  builtin cd -- \"$_sd_dir\" || return $?;\n");
    out("  SD_ALIAS=$1; SD_JUMPED_TO=$_sd_dir; export SD_ALIAS SD_JUMPED_TO;\n");
    out("  return 0;\n");
    out("};\n");
    let mut forward: Vec<String> = move_verbs()
        .iter()
        .map(|verb| format!("[ \"${{1-}}\" = \"{verb}\" ]"))
        .collect();
    forward.push("([ \"${1:0:1}\" = \"@\" ])".to_string());
    forward.push("([ \"${1:0:1}\" = \"-\" ] && [ \"${1-}\" != \"-\" ])".to_string());
    out(&format!("{MOVE}() {{\n"));
    out(&format!("  if {}; then\n", forward.join(" || ")));
    out(&format!("    command {MOVE} \"$@\";\n"));
    out("    return $?;\n");
    out("  fi;\n");
    out("  _sd_jump \"$@\";\n");
    out("};\n");
    out("# simpledir - the next zoxide. a subprocess cannot change this shell's\n");
    out(&format!("# directory, so the `cd` lives in the function and `{MOVE} print` only\n"));
    out(&format!("# resolves the name. `{CONFIG}` needs no wrapper: it changes things and\n"));
    out("# never moves you. installed by `bash install.sh`, or by hand with\n");
    out(&format!("# eval \"$({CONFIG} init)\".\n"));
    Ok(0)
}

// --------------------------------------------------------------------- prompt

fn cmd_prompt(shell: &str) -> Res<i32> {
    let colour = env_or("SD_PROMPT_COLOR", "38;5;110");
    if !colour.contains(';') && !colour.starts_with("38;5;") {
        return Err(usage("SD_PROMPT_COLOR wants an SGR sequence like 38;5;110"));
    }

    if shell == "bash" {
        out("# simpledir prompt segment for bash. add it to ~/.bashrc:\n");
        out(&format!("#   eval \"$({CONFIG} prompt bash)\"\n"));
        out(&format!("# shows the alias you jumped by, in {colour}, while you're still in the directory it took you to.\n"));
        out("# change that colour with SD_PROMPT_COLOR, e.g. 38;5;213\n");
        out("_SD_BASE_PS1=\"\";\n");
        out("_SD_ESC=$'\\033';\n");
        out("_sd_prompt() {\n");
        out("  if [ -z \"$_SD_BASE_PS1\" ]; then _SD_BASE_PS1=\"$PS1\"; fi;\n");
        out("  SD_SEG=\"\";\n");
        out("  if [ -n \"${SD_ALIAS-}\" ] && [ \"${SD_JUMPED_TO-}\" = \"$PWD\" ]; then\n");
        out(&format!("    SD_SEG=\"$_SD_ESC[{colour}m$SD_ALIAS$_SD_ESC[0m \";\n"));
        out("  fi;\n");
        out("  PS1=\"$SD_SEG$_SD_BASE_PS1\";\n");
        out("};\n");
        out("if [ -z \"${_SD_PROMPT_ON-}\" ]; then\n");
        out("  _SD_PROMPT_ON=1;\n");
        out("  PROMPT_COMMAND=\"_sd_prompt${PROMPT_COMMAND:+; $PROMPT_COMMAND}\";\n");
        out("fi;\n");
    } else if shell == "zsh" {
        out("# simpledir prompt segment for zsh. add it to ~/.zshrc:\n");
        out(&format!("#   eval \"$({CONFIG} prompt zsh)\"\n"));
        out(&format!("# shows the alias you jumped by, in {colour}, while you're still in the directory it took you to.\n"));
        out("# change that colour with SD_PROMPT_COLOR, e.g. 38;5;213\n");
        out("typeset -g _SD_ESC=$'\\033';\n");
        out("_sd_prompt() {\n");
        out("  local seg=\"\";\n");
        out("  if [[ -n \"${SD_ALIAS-}\" && \"${SD_JUMPED_TO-}\" == \"$PWD\" ]]; then\n");
        out(&format!("    seg=\"$_SD_ESC[{colour}m${{SD_ALIAS}}$_SD_ESC[0m \";\n"));
        out("  fi;\n");
        out("  PS1=\"${seg}${PS1}\";\n");
        out("};\n");
        out("typeset -gaU precmd_functions;\n");
        out("if (( ! ${precmd_functions[(I)_sd_prompt]} )); then\n");
        out("  precmd_functions+=(sd_prompt);\n");
        out("fi;\n");
    } else {
        return Err(usage("prompt takes bash or zsh"));
    }
    Ok(0)
}

// ---------------------------------------------------------------- completions

fn cmd_completions(shell: &str) -> Res<i32> {
    if shell == "bash" {
        out("# simpledir bash completion. install it with:\n");
        out(&format!("#   {CONFIG} completions bash > /usr/share/bash-completion/completions/{MOVE}\n"));
        out(&format!("_{MOVE}_complete() {{\n"));
        out("  local cur names\n");
        out("  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n");
        out(&format!("  names=$(command {MOVE} ls --names 2>/dev/null)\n"));
        out("  COMPREPLY=( $(compgen -W \"$names\" -- \"$cur\") )\n");
        out("  if [[ ${#COMPREPLY[@]} -eq 0 && -d \"$cur\" ]]; then\n");
        out("    # let the shell complete directories too: `sd dots/<tab>`\n");
        out("    COMPREPLY=( $(compgen -d -- \"$cur\") )\n");
        out("  fi\n");
        out("}\n");
        out(&format!("complete -o filenames -F _{MOVE}_complete {MOVE}\n"));
        out("\n");
        let verbs = config_verbs()
            .iter()
            .map(|verb| verb.to_string())
            .collect::<Vec<String>>()
            .join(" ");
        out(&format!("_{CONFIG}_complete() {{\n"));
        out(&format!("  local cur verbs=\"{verbs}\"\n"));
        out("  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n");
        out("  COMPREPLY=( $(compgen -W \"$verbs\" -- \"$cur\") )\n");
        out("}\n");
        out(&format!("complete -F _{CONFIG}_complete {CONFIG}\n"));
    } else {
        out(&format!("#compdef {MOVE} {CONFIG}\n"));
        out("# simpledir zsh completion. requires compinit; put this in ~/.zshrc:\n");
        out(&format!("#   {CONFIG} completions zsh > \"${{fpath[1]}}/_{MOVE}\"\n"));
        out("local -a _sd_aliases\n");
        out(&format!("_sd_aliases=(${{(f)\"$(command {MOVE} ls --names 2>/dev/null)\"}})\n"));
        out("_arguments '1: :->alias' '*: :_files -/'\n");
        out("if [[ $state == alias ]]; then\n");
        out("  _describe -t aliases 'directory alias' _sd_aliases\n");
        out("fi\n");
        out("\n");
        let verbs = config_verbs()
            .iter()
            .map(|verb| verb.to_string())
            .collect::<Vec<String>>()
            .join(" ");
        out(&format!("_{CONFIG}() {{\n"));
        out(&format!("  local -a verbs=({verbs})\n"));
        out("  _describe -t commands 'sdcfg command' verbs\n");
        out("}\n");
        out(&format!("compdef _{CONFIG} {CONFIG}\n"));
    }
    Ok(0)
}

// ------------------------------------------------------------------ uninstall

fn rc_candidates() -> Vec<String> {
    // SIMPLEDIR_RC is exclusive: naming a file means naming that file.
    if let Ok(only) = env::var("SIMPLEDIR_RC") {
        return vec![only];
    }
    let mut candidates = Vec::new();
    let shell = env_or("SHELL", "");
    if contains(&shell, "zsh") {
        candidates.push(env_or("ZDOTDIR", &home_dir()) + "/.zshrc");
    }
    for name in ["/.bashrc", "/.zshrc", "/.config/fish/config.fish"] {
        candidates.push(home_dir() + name);
    }
    let mut unique: Vec<String> = Vec::new();
    for p in candidates {
        if !unique.contains(&p) {
            unique.push(p);
        }
    }
    unique
}

fn cb_weak_canonical(p: &str) -> String {
    match fs::canonicalize(p) {
        Ok(c) => c.to_string_lossy().to_string(),
        Err(_) => abspath(p),
    }
}

fn cmd_uninstall(args: &Args) -> Res<i32> {
    let target = install_target();
    let cfg = load_config()?;

    // Be explicit: we may be removing a different copy than the one running.
    let running = match fs::read_link("/proc/self/exe") {
        Ok(p) => p.to_string_lossy().to_string(),
        Err(_) => prog().to_string(),
    };
    if cb_weak_canonical(&target) != cb_weak_canonical(&running) {
        out(&format!(
            "note: removing {target}, not the copy you're running ({running})\n"
        ));
    }

    if !cfg.aliases.is_empty() && !args.has("purge") {
        out(&format!(
            "you have {} alias{} in {}\n",
            cfg.aliases.len(),
            if cfg.aliases.len() == 1 { "" } else { "es" },
            config_file()
        ));
        out(&format!(
            "  these stay. to remove them too: {CONFIG} uninstall --purge\n"
        ));
    }

    if !args.has("yes") {
        if unsafe { isatty(0) } != 1 {
            return Err(die("not a terminal, so not asking. re-run with --yes"));
        }
        out(&format!(
            "  remove {target} and the wrapper from your shell rc? [y/N] "
        ));
        flush_out();
        let answer = read_line().unwrap_or_default();
        if lower(&trim(&answer)) != "y" {
            out("  ok, nothing changed\n");
            return Ok(0);
        }
    }

    let mut touched: Vec<String> = Vec::new();
    for rc in rc_candidates() {
        if !path_exists(&rc) {
            continue;
        }
        let original = read_file(&rc)?;
        if !original.contains(MARK_BEGIN) {
            continue;
        }
        // Drop the marked span, keep the rest byte for byte.
        let mut cleaned = String::new();
        let mut i = 0usize;
        while i < original.len() {
            match original[i..].find(MARK_BEGIN) {
                None => {
                    cleaned.push_str(&original[i..]);
                    break;
                }
                Some(rel_begin) => {
                    let begin = i + rel_begin;
                    cleaned.push_str(&original[i..begin]);
                    match original[begin..].find(MARK_END) {
                        None => {
                            i = original.len();
                            break;
                        }
                        Some(rel_end) => {
                            let end = begin + rel_end;
                            match original[end..].find('\n') {
                                None => i = original.len(),
                                Some(rel_stop) => i = end + rel_stop + 1,
                            }
                        }
                    }
                }
            }
        }
        let backup = format!("{rc}.bak.{}", stamp());
        write_atomic(&backup, &original, false)?;
        write_atomic(&rc, &cleaned, false)?;
        touched.push(format!("{rc} (backup: {backup})"));
    }
    if touched.is_empty() {
        out("no wrapper block found in any shell rc\n");
    }

    if path_exists(&target) {
        match fs::remove_file(&target) {
            Ok(()) => out(&format!("removed {target}\n")),
            Err(e) => out(&format!(
                "couldn't remove {target}: {e}\n  remove it by hand: rm {target}\n"
            )),
        }
    } else {
        out(&format!("nothing to remove at {target}\n"));
    }
    // sdcfg lives next to the sd we just removed, and nowhere else.
    if let Some(parent) = Path::new(&target).parent() {
        let _ = fs::remove_file(parent.join("sdcfg"));
    }

    for rc in &touched {
        out(&format!("cleaned the wrapper from {rc}\n"));
    }

    if args.has("purge") {
        let dir = config_dir();
        let _ = fs::remove_dir_all(&dir);
        out(&format!("deleted {dir} and every alias in it\n"));
    } else {
        out(&format!("aliases kept in {}\n", config_file()));
    }
    out("open a new shell, or `exec bash`, to drop the old functions\n");
    Ok(0)
}

// --------------------------------------------------------------------- doctor

fn cb_ok(msg: &str) {
    out(&format!("  \x1b[32mok\x1b[0m    {msg}\n"));
}

fn cb_bad(msg: &str) {
    out(&format!("  \x1b[31mproblem\x1b[0m {msg}\n"));
}

fn cb_note(msg: &str) {
    out(&format!("  \x1b[2mnote\x1b[0m    {msg}\n"));
}

fn cb_built_with() -> String {
    // __VERSION__ in the C++ is the compiler's own version macro. Allow the
    // build to name the Rust toolchain; otherwise just say what built it.
    option_env!("SIMPLDIR_RUSTC_VERSION")
        .unwrap_or("rustc")
        .to_string()
}

fn cmd_doctor() -> Res<i32> {
    let mut problems: i32 = 0;

    out(&format!("{CONFIG} doctor - version {VERSION}\n"));
    out("runtime\n");
    cb_ok(&format!("built with {}", cb_built_with()));

    out("config\n");
    let cf = config_file();
    if path_exists(&cf) {
        cb_ok(&cf);
        match load_config() {
            Ok(cfg) => {
                cb_ok(&format!(
                    "{} alias{}",
                    cfg.aliases.len(),
                    if cfg.aliases.len() == 1 { "" } else { "es" }
                ));
                if cfg.version < CONFIG_VERSION {
                    problems += 1;
                    cb_bad(&format!(
                        "config is version {}, this is version {}. migrate it: {CONFIG} migrate",
                        cfg.version, CONFIG_VERSION
                    ));
                }
                for (name, path) in &cfg.aliases {
                    let target = as_stored(path);
                    if !is_dir(&target) {
                        problems += 1;
                        cb_bad(&format!(
                            "'{name}' -> {target} is gone. rebind: {CONFIG} add --force {name} <path>"
                        ));
                    }
                    let trimmed = trim(name);
                    if trimmed != *name || starts_with(name, "-") || name.contains(' ') {
                        problems += 1;
                        cb_bad(&format!(
                            "alias name '{name}' has whitespace or a leading dash"
                        ));
                    }
                }
            }
            Err(e) => {
                let msg = match e {
                    AppError::User(m) | AppError::Usage(m) => m,
                };
                let first = msg.split('\n').next().unwrap_or("");
                problems += 1;
                cb_bad(first);
            }
        }
    } else {
        problems += 1;
        cb_bad(&format!(
            "no config yet at {cf}. create one: {CONFIG} add"
        ));
    }

    out("shell\n");
    let mut wired = false;
    for rc in rc_candidates() {
        if !path_exists(&rc) {
            continue;
        }
        if let Ok(content) = read_file(&rc) {
            if content.contains(MARK_BEGIN) {
                cb_ok(&format!("wrapper wired into {rc}"));
                wired = true;
            }
        }
    }
    if !wired {
        let shell = env_or("SHELL", "");
        problems += 1;
        cb_bad(&format!(
            "no shell wrapper found. add it: {CONFIG} init >> {}",
            if contains(&shell, "zsh") { "~/.zshrc" } else { "~/.bashrc" }
        ));
    }

    let dir = Path::new(&install_target())
        .parent()
        .map(|p| p.to_string_lossy().to_string())
        .unwrap_or_default();
    if contains(
        &format!(":{}:", env_or("PATH", "")),
        &format!(":{dir}:"),
    ) {
        cb_ok(&format!("{dir} is in PATH"));
    } else {
        problems += 1;
        cb_bad(&format!(
            "{dir} is not in PATH. add: export PATH=\"{dir}:$PATH\""
        ));
    }

    out("history\n");
    let history = read_history();
    if history.is_empty() {
        cb_note(&format!(
            "empty. {MOVE} remembers where you go; {CONFIG} forget clears it"
        ));
    } else {
        cb_ok(&format!("{} directories remembered", history.len()));
        cb_ok("recording is on");
    }
    if env::var_os("SIMPLEDIR_NO_HISTORY").is_some() {
        cb_note("recording is off (SIMPLEDIR_NO_HISTORY)");
    }

    out("\n");
    if problems > 0 {
        out(&format!(
            "{problems} problem{} found\n",
            if problems == 1 { "" } else { "s" }
        ));
        return Ok(1);
    }
    out("everything looks fine\n");
    Ok(0)
}

// ---------------------------------------------------------------------- bench

#[derive(Clone, Copy, Default)]
struct CbTiming {
    median: f64,
    best: f64,
}

struct CbCase {
    label: String,
    words: Vec<String>,
    note: String,
}

fn cb_now_micros() -> f64 {
    match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(d) => d.as_secs_f64() * 1_000_000.0,
        Err(_) => 0.0,
    }
}

fn cb_time_one(cmd: &mut Command) -> f64 {
    let start = cb_now_micros();
    if let Ok(mut child) = cmd.spawn() {
        let _ = child.wait();
    }
    cb_now_micros() - start
}

fn cb_measure<F>(mut make: F, runs: usize) -> CbTiming
where
    F: FnMut() -> Command,
{
    let mut samples: Vec<f64> = Vec::with_capacity(runs);
    // Warm up hard: the first execs after a build pay for a cold page cache.
    for _ in 0..6 {
        let mut c = make();
        let _ = cb_time_one(&mut c);
    }
    for _ in 0..runs {
        let mut c = make();
        samples.push(cb_time_one(&mut c));
    }
    samples.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    CbTiming {
        median: samples[samples.len() / 2],
        best: samples[0],
    }
}

fn cb_floor_command(self_path: &str) -> Command {
    // The floor is this binary doing no work, not some other process with a
    // different linker and startup path. It is the only honest baseline for
    // the "over startup" column. Both streams go to /dev/null so an undrained
    // pipe is not what we end up timing.
    let words = vec!["--version".to_string()];
    cb_self_command(self_path, &words)
}

fn cb_self_command(self_path: &str, words: &[String]) -> Command {
    let mut c = Command::new(self_path);
    c.args(words).stdout(Stdio::null()).stderr(Stdio::null());
    c
}

fn cmd_bench(args: &Args) -> Res<i32> {
    // Deliberately tolerant: this measures the machine, and a config it cannot
    // parse is exactly when somebody reaches for a benchmark.
    let (cfg, alias_count) = match load_config() {
        Ok(c) => {
            let n = c.aliases.len();
            (Some(c), n)
        }
        Err(_) => (None, 0),
    };

    let mut runs: i32 = 40;
    if args.has("runs") {
        let raw = args.value("runs");
        runs = raw.parse::<i32>().unwrap_or(0);
        if !(5..=2000).contains(&runs) {
            return Err(usage(format!("--runs takes 5 to 2000, got {raw}")));
        }
    }

    let self_path = match fs::read_link("/proc/self/exe") {
        Ok(p) => p.to_string_lossy().to_string(),
        Err(_) => MOVE.to_string(),
    };

    let mut entries: usize = 0;
    let hf = history_file();
    if path_exists(&hf) {
        if let Ok(raw) = read_file(&hf) {
            if let Ok(root) = json_parse(&raw) {
                if let Some(Json::Obj(map)) = root.get("dirs") {
                    entries = map.len();
                }
            }
        }
    }

    out(&format!("simpledir benchmark ({MOVE} {VERSION})\n\n"));
    out(&format!(
        "  every timing below is a real fork+exec of this binary, median of {runs} runs,\n  measured on this machine just now. yours will differ.\n\n"
    ));

    // Measured twice, before and after the cases; keep the faster median.
    // Measuring only at the start used to absorb the cold page cache and made
    // a jump look cheaper than this binary doing no work at all.
    let mut floor = cb_measure(|| cb_floor_command(&self_path), runs as usize);

    let mut cases: Vec<CbCase> = Vec::new();
    if let Some(ref c) = cfg {
        if let Some((name, _)) = c.aliases.iter().next() {
            cases.push(CbCase {
                label: format!("{MOVE} <alias>"),
                words: vec![name.clone()],
                note: "the common case".to_string(),
            });
        }
    }
    cases.push(CbCase {
        label: format!("{MOVE} ls"),
        words: vec!["ls".to_string()],
        note: format!("{alias_count} aliases"),
    });
    if entries > 0 {
        cases.push(CbCase {
            label: format!("{MOVE} top"),
            words: vec!["top".to_string()],
            note: format!("{entries} dirs remembered"),
        });
        cases.push(CbCase {
            label: format!("{MOVE} print <miss>"),
            words: vec!["print".to_string(), "nosuchthingxyz".to_string()],
            note: "frecency scan, worst case".to_string(),
        });
    }
    cases.push(CbCase {
        label: format!("{MOVE} doctor"),
        words: vec!["doctor".to_string()],
        note: "every check it can do".to_string(),
    });

    // Measure every case before printing anything, then measure the floor a
    // second time. Both the table and the paragraph below must use the same
    // faster floor: printing the table first used to leave it on the cold
    // first measurement while the paragraph used the faster second one.
    let mut measured: Vec<(CbCase, CbTiming)> = Vec::with_capacity(cases.len());
    for case in cases {
        let t = cb_measure(|| cb_self_command(&self_path, &case.words), runs as usize);
        measured.push((case, t));
    }
    let floor_again = cb_measure(|| cb_floor_command(&self_path), runs as usize);
    if floor_again.median < floor.median {
        floor = floor_again;
    }

    out(&format!(
        "  {:<18} {:>10} {:>13} {:>11}   {}\n",
        "case", "median", "over startup", "best", ""
    ));
    out(&format!("  {}\n", "-".repeat(74)));

    // --version does no work at all, so it is both the floor and a row of its own.
    out(&format!(
        "  {:<18} {:>7.3} ms {:>13} {:>7.3} ms   this binary, no work\n",
        format!("{MOVE} --version"),
        floor.median / 1000.0,
        "--",
        floor.best / 1000.0
    ));

    let mut jump_over_floor = 0.0;
    for (i, (case, t)) in measured.iter().enumerate() {
        if i == 0 {
            jump_over_floor = (t.median - floor.median) / 1000.0;
        }
        out(&format!(
            "  {:<18} {:>7.3} ms {:>+9.3} ms {:>7.3} ms   {}\n",
            case.label,
            t.median / 1000.0,
            (t.median - floor.median) / 1000.0,
            t.best / 1000.0,
            case.note
        ));
    }

    // Say the thing the table is actually saying, so a table of milliseconds
    // does not invite the reading that this program is slow.
    out(&format!(
        "\n  starting this binary with --version costs {:.3} ms, before it does any work\n  at all. a jump costs {:.3} ms more than that: read the config, find the\n  name, check the directory still exists, print the path. The frecency log is\n  not searched on a jump -- recording a visit only needs one timestamp out\n  of it, which is a substring scan rather than a parse.\n",
        floor.median / 1000.0,
        jump_over_floor
    ));
    out(&format!("  more runs: {CONFIG} bench --runs 200\n"));
    Ok(0)
}
// config_a.rs — the config-writing half of sdcfg, ported from sd.cpp.
// This file is concatenated into src/main.rs; it defines no main and uses no
// crates. Private helpers are prefixed ca_ so they cannot collide with the
// other parts.

fn ca_num_prefix(s: &str, allow_fraction: bool) -> (usize, f64) {
    let t = s.trim_start();
    let b = t.as_bytes();
    let mut end = 0usize;
    if end < b.len() && (b[end] == b'+' || b[end] == b'-') {
        end += 1;
    }
    let mut any = false;
    while end < b.len() && b[end].is_ascii_digit() {
        end += 1;
        any = true;
    }
    if allow_fraction && end < b.len() && b[end] == b'.' {
        end += 1;
        while end < b.len() && b[end].is_ascii_digit() {
            end += 1;
            any = true;
        }
    }
    if !any {
        return (0, 0.0);
    }
    if allow_fraction && end < b.len() && (b[end] == b'e' || b[end] == b'E') {
        let save = end;
        end += 1;
        if end < b.len() && (b[end] == b'+' || b[end] == b'-') {
            end += 1;
        }
        let exp_start = end;
        while end < b.len() && b[end].is_ascii_digit() {
            end += 1;
        }
        if end == exp_start {
            end = save;
        }
    }
    let value = t[..end].parse::<f64>().unwrap_or(0.0);
    (end, value)
}

// std::atof / std::atoi take the longest numeric prefix, or zero.
fn ca_atof(s: &str) -> f64 {
    ca_num_prefix(s, true).1
}

fn ca_atoi(s: &str) -> i32 {
    ca_num_prefix(s, false).1 as i32
}

// popen(cmd, "r"), read stdout to EOF, pclose. stderr is handled by the caller's
// command line (zoxide adds 2>/dev/null itself).
fn ca_capture(cmd: &str) -> String {
    match std::process::Command::new("sh").arg("-c").arg(cmd).output() {
        Ok(o) => String::from_utf8_lossy(&o.stdout).to_string(),
        Err(_) => String::new(),
    }
}

fn ca_is_symlink(path: &str) -> bool {
    std::fs::symlink_metadata(path)
        .map(|m| m.file_type().is_symlink())
        .unwrap_or(false)
}

// Write a history log with every path in `skip` left out. The shape is the one
// record_visit() writes: version 1, then dirs -> { path -> { n, t } }.
fn ca_history_without(dirs: &std::collections::BTreeMap<String, Visit>, skip: &[String]) -> Json {
    let mut root = Json::obj();
    root.set("version", Json::num(1.0));
    let mut node = Json::obj();
    for (path, visit) in dirs {
        if skip.iter().any(|s| s == path) {
            continue;
        }
        let mut v = Json::obj();
        v.set("n", Json::num(visit.n));
        v.set("t", Json::num(visit.t));
        node.set(path.clone(), v);
    }
    root.set("dirs", node);
    root
}

// recursive_directory_iterator(root, skip_permission_denied) with the levels
// check from cmd_import. Directory symlinks are listed but never descended
// into, exactly as the C++ iterator does without follow_directory_symlink.
fn ca_import_walk(
    prefix: &str,
    dir: &str,
    level: i32,
    depth: i32,
    hidden: bool,
    found: &mut Vec<(String, String)>,
) {
    let entries = match std::fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => return,
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let text = path.to_string_lossy().to_string();
        if !std::fs::metadata(&path).map(|m| m.is_dir()).unwrap_or(false) {
            continue;
        }
        let base = entry.file_name().to_string_lossy().to_string();
        let next = level + 1;
        // A hidden directory is not bound, but the recursive_directory_iterator
        // still walks into it: its non-hidden children can be imported.
        if hidden || !base.starts_with('.') {
            if next <= depth {
                found.push((format!("{prefix}{base}"), text.clone()));
            }
        }
        if next < depth && entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
            ca_import_walk(prefix, &text, next, depth, hidden, found);
        }
    }
}

// Tag membership is a list of alias names, not a foreign key. These two helpers
// keep it in step with a rename or an unbind, and drop a tag that ends up empty.
fn prune_tag_members(cfg: &mut Config, name: &str) -> usize {
    let mut changed = 0usize;
    for (_, members) in &mut cfg.tags {
        let before = members.len();
        members.retain(|member| member != name);
        changed += before - members.len();
    }
    cfg.tags.retain(|(_, members)| !members.is_empty());
    changed
}

fn rename_tag_members(cfg: &mut Config, from: &str, to: &str) -> usize {
    let mut changed = 0usize;
    for (_, members) in &mut cfg.tags {
        for member in members.iter_mut() {
            if member == from {
                *member = to.to_string();
                changed += 1;
            }
        }
        let mut seen = BTreeSet::new();
        members.retain(|member| seen.insert(member.clone()));
    }
    changed
}

// ------------------------------------------------------------------ add/rm

fn cmd_add(args: &Args) -> Res<i32> {
    let mut raw = args.word(1);
    if raw.is_empty() {
        raw = std::env::current_dir()
            .unwrap_or_else(|_| std::path::PathBuf::from("."))
            .to_string_lossy()
            .to_string();
    }
    let keep = args.has("keep-symlinks");
    let target = if keep { abspath(&raw) } else { realpath(&raw) };
    require_dir(&target)?;

    let mut cfg = load_config()?;
    let mut name = args.word(0);
    let derived = basename_of(&target);
    if name.is_empty() {
        name = derived.clone();
        if name.is_empty() {
            return Err(die(format!(
                "can't derive a name from {target}. usage: {CONFIG} add <name> [path]"
            )));
        }
    }
    if let Some(existing) = cfg.aliases.get(&name).cloned() {
        if !args.has("force") {
            return Err(die(format!(
                "alias '{name}' already exists -> {existing}\n  overwrite: {CONFIG} add --force {name} {target}"
            )));
        }
    }

    cfg.aliases.insert(name.clone(), target.clone());
    save_config(&cfg)?;
    out(&format!("{name} -> {target}\n"));
    nudge();
    if !derived.is_empty() && name != derived {
        out(&format!("  rename it: {CONFIG} rename {name} <other-name>\n"));
    }
    if keep && ca_is_symlink(&target) {
        out(&format!("  kept the symlink: {target}\n"));
    }
    Ok(0)
}

fn cmd_rm(args: &Args) -> Res<i32> {
    let name = args.word(0);
    if name.is_empty() {
        return Err(user(format!("which alias? usage: {CONFIG} rm <name>")));
    }
    let mut cfg = load_config()?;
    let target = match cfg.aliases.get(&name).cloned() {
        Some(p) => p,
        None => return Err(user(unknown_alias_error(&name, &cfg))),
    };
    out(&format!("removed {name} -> {target}\n"));
    cfg.aliases.remove(&name);
    let pruned = if args.has("prune-tags") {
        prune_tag_members(&mut cfg, &name)
    } else {
        0
    };
    save_config(&cfg)?;
    if pruned > 0 {
        out(&format!(
            "  dropped {name} from {pruned} tag{}\n",
            if pruned == 1 { "" } else { "s" }
        ));
    }
    Ok(0)
}

fn cmd_rename(args: &Args) -> Res<i32> {
    let from = args.word(0);
    let to = args.word(1);
    if from.is_empty() || to.is_empty() {
        return Err(user(format!(
            "rename needs two names: {CONFIG} rename <old> <new>"
        )));
    }
    let mut cfg = load_config()?;
    let target = match cfg.aliases.get(&from).cloned() {
        Some(p) => p,
        None => return Err(user(unknown_alias_error(&from, &cfg))),
    };
    if let Some(existing) = cfg.aliases.get(&to).cloned() {
        if !args.has("force") {
            return Err(die(format!(
                "alias '{to}' already exists -> {existing}\n  overwrite: {CONFIG} rename --force {from} {to}"
            )));
        }
    }
    cfg.aliases.remove(&from);
    cfg.aliases.insert(to.clone(), target.clone());
    let carried = if args.has("tags") {
        rename_tag_members(&mut cfg, &from, &to)
    } else {
        0
    };
    save_config(&cfg)?;
    out(&format!("{from} -> {to} ({target})\n"));
    if carried > 0 {
        out(&format!(
            "  carried the name into {carried} tag{}\n",
            if carried == 1 { "" } else { "s" }
        ));
    }
    Ok(0)
}

// ------------------------------------------------------------------ import

fn cmd_import(args: &Args) -> Res<i32> {
    let raw = args.word(0);
    if raw.is_empty() {
        return Err(user("which directory? usage: sdcfg import <dir>"));
    }
    let root = realpath(&raw);
    require_dir(&root)?;

    let mut depth = 1i32;
    if args.saw("depth") {
        match args.value("depth").parse::<i32>() {
            Ok(d) => depth = d,
            Err(_) => return Err(usage("--depth wants a number")),
        }
        if depth < 1 {
            return Err(usage("--depth is at least 1"));
        }
    }
    let prefix = args.value("prefix");
    let hidden = args.has("hidden");
    let force = args.has("force");
    let dry = args.has("dry-run");
    let keep = args.has("keep-symlinks");

    let mut found: Vec<(String, String)> = Vec::new();
    ca_import_walk(&prefix, &root, 0, depth, hidden, &mut found);
    if found.is_empty() {
        return Err(user(format!("nothing to import under {root}")));
    }

    let mut cfg = load_config()?;
    let mut bound: Vec<String> = Vec::new();
    let mut skipped: Vec<String> = Vec::new();
    found.sort();
    for (name, target) in found {
        if cfg.aliases.contains_key(&name) && !force {
            skipped.push(name);
            continue;
        }
        let stored = if keep { abspath(&target) } else { realpath(&target) };
        cfg.aliases.insert(name.clone(), stored);
        bound.push(name);
    }
    if !bound.is_empty() && !dry {
        save_config(&cfg)?;
    }
    out(&format!(
        "{} {}: {}\n",
        if dry { "would bind" } else { "bound" },
        bound.len(),
        join(&bound, " ")
    ));
    if !skipped.is_empty() {
        out(&format!(
            "skipped {} already bound: {}\n",
            skipped.len(),
            join(&skipped, " ")
        ));
        if !force {
            out(&format!("  overwrite them: {CONFIG} import --force {root}\n"));
        }
    }
    Ok(0)
}

// --------------------------------------------------------------------- bind

fn cmd_bind(args: &Args) -> Res<i32> {
    let candidates = history_candidates(10)?;
    if candidates.is_empty() {
        out("nothing new in your history to bind\n");
        return Ok(0);
    }
    if args.has("dry-run") {
        let cfg = load_config()?;
        let mut taken: std::collections::BTreeSet<String> = cfg.aliases.keys().cloned().collect();
        let mut names: Vec<String> = Vec::new();
        for c in &candidates {
            let name = name_for(&c.path, &taken);
            taken.insert(name.clone());
            names.push(name);
        }
        out(&format!("would bind {}: {}\n", names.len(), join(&names, " ")));
        return Ok(0);
    }

    let mut cfg = load_config()?;
    let mut made: Vec<(String, String)> = Vec::new();
    for c in &candidates {
        // Deliberately rebuilt each time: that is what the C++ did, so a name
        // picked for one candidate is not reserved for the next one.
        let taken: std::collections::BTreeSet<String> = cfg.aliases.keys().cloned().collect();
        let name = name_for(&c.path, &taken);
        cfg.aliases.insert(name.clone(), c.path.clone());
        made.push((name, c.path.clone()));
    }
    save_config(&cfg)?;
    out(&format!("bound {}:\n", made.len()));
    for (name, path) in &made {
        out(&format!("  {name} -> {path}\n"));
    }
    out(&format!("\nrename any of them: {CONFIG} rename <old> <new>\n"));
    Ok(0)
}

// ---------------------------------------------------------------------- tag

fn cmd_tag(args: &Args) -> Res<i32> {
    let mut cfg = load_config()?;

    if args.words.is_empty() || args.has("list") {
        if cfg.tags.is_empty() {
            out(&format!(
                "no tags yet. make one:\n  {CONFIG} tag @work dots hypr projects\nthen `{MOVE} @work` lists that group, and `{MOVE} @work dots` jumps to one\n"
            ));
            return Ok(0);
        }
        for (key, members) in &cfg.tags {
            out(&format!(
                "  {key}  {} {}\n",
                members.len(),
                if members.len() == 1 { "alias" } else { "aliases" }
            ));
            let width = members.iter().map(|m| m.len()).max().unwrap_or(0);
            let mut line = String::from("    ");
            for m in members {
                line.push_str(m);
                line.push_str(&" ".repeat(width - m.len() + 2));
            }
            line.push('\n');
            out(&line);
        }
        return Ok(0);
    }

    let key = tag_key(&args.word(0));
    let wanted: Vec<String> = args.words.iter().skip(1).cloned().collect();
    let slot = cfg.tags.iter().position(|(k, _)| k == &key);

    if args.has("rename") {
        if wanted.len() != 1 {
            return Err(usage(format!(
                "--rename takes one new name: {CONFIG} tag {key} --rename @new"
            )));
        }
        let idx = match slot {
            Some(i) => i,
            None => {
                return Err(die(format!(
                    "no tag {key}. {CONFIG} tag lists the ones you have"
                )))
            }
        };
        let new_key = tag_key(&wanted[0]);
        if new_key == key {
            out(&format!("{key} already has that name\n"));
            return Ok(0);
        }
        if cfg.tags.iter().any(|(k, _)| *k == new_key) {
            return Err(die(format!("tag {new_key} already exists")));
        }
        let count = cfg.tags[idx].1.len();
        cfg.tags[idx].0 = new_key.clone();
        save_config(&cfg)?;
        out(&format!(
            "{key} -> {new_key} ({} {}, nothing was unbound)\n",
            count,
            if count == 1 { "alias" } else { "aliases" }
        ));
        out(&format!("  list them: {MOVE} {new_key}\n"));
        return Ok(0);
    }

    if args.has("remove") || args.has("drop") {
        let idx = match slot {
            Some(i) => i,
            None => {
                return Err(die(format!(
                    "no tag {key}. {CONFIG} tag lists the ones you have"
                )));
            }
        };
        if args.has("drop") && !wanted.is_empty() {
            return Err(die(format!(
                "--drop takes no names. {CONFIG} tag --drop {key} removes the whole tag"
            )));
        }
        if args.has("drop") {
            out(&format!(
                "  dropped {key} ({} {}, nothing was unbound)\n",
                cfg.tags[idx].1.len(),
                if cfg.tags[idx].1.len() == 1 { "alias" } else { "aliases" }
            ));
            cfg.tags[idx].1.clear();
        }
        for name in &wanted {
            if let Some(p) = cfg.tags[idx].1.iter().position(|m| m == name) {
                cfg.tags[idx].1.remove(p);
                out(&format!("  removed {name} from {key}\n"));
            } else {
                out(&format!("  {name} isn't in {key}\n"));
            }
        }
        let empty = cfg.tags[idx].1.is_empty();
        if empty {
            cfg.tags.remove(idx);
        }
        save_config(&cfg)?;
        if empty {
            out(&format!("{key} is empty now, so it's gone\n"));
        }
        return Ok(0);
    }

    if wanted.is_empty() {
        return Err(usage(format!("which aliases? {CONFIG} tag {key} <alias>...")));
    }

    let unknown: Vec<String> = wanted
        .iter()
        .filter(|n| !cfg.aliases.contains_key(*n))
        .cloned()
        .collect();
    if !unknown.is_empty() {
        let mut msg = format!(
            "{key} holds alias names, and these don't exist: {}",
            join(&unknown, " ")
        );
        msg.push_str(&format!("\n  bind them first: {CONFIG} add <name> <path>"));
        msg.push_str(&format!("\n  or see what you have: {MOVE} ls"));
        return Err(user(msg));
    }

    let idx = match slot {
        Some(i) => i,
        None => {
            cfg.tags.push((key.clone(), Vec::new()));
            cfg.tags.len() - 1
        }
    };
    let mut added: Vec<String> = Vec::new();
    for name in &wanted {
        if cfg.tags[idx].1.contains(name) {
            continue;
        }
        cfg.tags[idx].1.push(name.clone());
        added.push(name.clone());
    }
    save_config(&cfg)?;

    if added.is_empty() {
        out(&format!("{key} already had all of those\n"));
        return Ok(0);
    }
    let count = cfg.tags[idx].1.len();
    out(&format!(
        "{key} ({} {}): {}\n",
        count,
        if count == 1 { "alias" } else { "aliases" },
        join(&added, " ")
    ));
    out(&format!("  {MOVE} {key}          list them\n"));
    out(&format!("  {MOVE} {key} <alias>  jump to one\n"));
    Ok(0)
}

// ------------------------------------------------------------------- project

fn cmd_project(args: &Args) -> Res<i32> {
    let here = find_project_dir();

    if args.has("list") || args.words.is_empty() {
        if here.is_empty() {
            out(&format!(
                "no {PROJECT_FILE} here or in any parent directory up to your home.\n  make one: {CONFIG} project add <name> <path>\n"
            ));
            return Ok(1);
        }
        let project = load_project(&here)?;
        out(&format!(
            "{} {} in {here}/{PROJECT_FILE}:\n\n",
            project.aliases.len(),
            if project.aliases.len() == 1 { "alias" } else { "aliases" }
        ));
        for (name, stored) in &project.aliases {
            let target = resolve_project_path(stored, &here);
            let pad = " ".repeat(12 - std::cmp::min(12, name.len()));
            out(&format!(
                "  {name}{pad}{target}{}\n",
                if is_dir(&target) { "" } else { "   [missing]" }
            ));
        }
        return Ok(0);
    }

    let verb = args.word(0);
    if verb == "rm" || verb == "remove" {
        if here.is_empty() {
            return Err(die(format!(
                "no {PROJECT_FILE} here or in any parent up to your home"
            )));
        }
        let mut project = load_project(&here)?;
        let name = args.word(1);
        if name.is_empty() {
            return Err(usage(format!("which one? {CONFIG} project rm <name>")));
        }
        let stored = match project.aliases.get(&name).cloned() {
            Some(s) => s,
            None => {
                return Err(user(format!("'{name}' isn't in {here}/{PROJECT_FILE}")));
            }
        };
        out(&format!(
            "removed {name} -> {}\n",
            resolve_project_path(&stored, &here)
        ));
        project.aliases.remove(&name);
        save_project(&project)?;
        return Ok(0);
    }

    if verb != "add" {
        return Err(usage("project takes add, rm or --list"));
    }

    let name = args.word(1);
    let mut raw = args.word(2);
    if name.is_empty() {
        return Err(usage(format!("usage: {CONFIG} project add <name> [<path>]")));
    }
    if raw.is_empty() {
        raw = std::env::current_dir()
            .unwrap_or_else(|_| std::path::PathBuf::from("."))
            .to_string_lossy()
            .to_string();
    }
    let target = abspath(&raw);
    require_dir(&target)?;

    let home = home_dir();
    let cwd = std::env::current_dir()
        .unwrap_or_else(|_| std::path::PathBuf::from("."))
        .to_string_lossy()
        .to_string();
    if cwd == home {
        return Err(die(format!(
            "not writing {PROJECT_FILE} in your home directory.\n  every alias would then depend on where you stood. cd into the project first."
        )));
    }

    let here = if here.is_empty() { cwd } else { here };
    let mut project = load_project(&here)?;
    let stored = store_for_project(&target, &here);
    if let Some(old) = project.aliases.get(&name).cloned() {
        if !args.has("force") {
            let old_target = resolve_project_path(&old, &here);
            return Err(die(format!(
                "'{name}' is already in {here}/{PROJECT_FILE} -> {old_target}\n  overwrite: {CONFIG} project add --force {name} {}",
                shell_quote(&raw)
            )));
        }
    }
    project.aliases.insert(name.clone(), stored.clone());
    save_project(&project)?;
    out(&format!("{name} -> {target}\n"));
    out(&format!("  stored in {here}/{PROJECT_FILE}"));
    if stored != target {
        out(&format!(
            " as the relative path \"{stored}\", so it travels"
        ));
    }
    out(&format!("\n  {MOVE} {name} works anywhere under {here}\n"));
    Ok(0)
}

// -------------------------------------------------------------------- export

fn cmd_export(args: &Args) -> Res<i32> {
    let cfg = load_config()?;
    let mut node = config_json(&cfg);
    // Tags hold alias names, not paths, so only the alias values are rewritten.
    let mut aliases = Json::obj();
    for (name, path) in &cfg.aliases {
        aliases.set(name.clone(), Json::str(portable(path)));
    }
    node.set("aliases", aliases);
    node.set("version", Json::num(CONFIG_VERSION as f64));
    node.set("exported_by", Json::str(format!("{MOVE} {VERSION}")));
    let text = json_dump(&node, 2, 0) + "\n";

    let asked_for = args.word(0);
    if asked_for.is_empty() || asked_for == "-" {
        out(&text);
        return Ok(0);
    }
    let file = abspath(&asked_for);
    write_atomic(&file, &text, false)?;

    let mut portable_count = 0usize;
    for path in cfg.aliases.values() {
        let stored = as_stored(path);
        if portable(&stored) != stored {
            portable_count += 1;
        }
    }
    out(&format!("wrote {} aliases to {file}\n", cfg.aliases.len()));
    if portable_count != 0 {
        out(&format!(
            "  {portable_count} under your home were written as ~/ so they travel\n"
        ));
    }
    out(&format!(
        "  adopt it elsewhere with: {CONFIG} adopt {}\n",
        shell_quote(&file)
    ));
    Ok(0)
}

// --------------------------------------------------------------------- adopt

fn cmd_adopt(args: &Args) -> Res<i32> {
    let from = args.word(0);
    if from.is_empty() {
        return Err(usage(format!(
            "which file? {CONFIG} adopt <file>\n  make one to import: {CONFIG} export <file>"
        )));
    }
    let path = abspath(&from);
    if !path_exists(&path) {
        return Err(die(format!("no such file: {path}")));
    }
    let raw = read_file(&path)?;
    let root = match json_parse(&raw) {
        Ok(v) => v,
        Err(e) => {
            let why = match e {
                AppError::User(m) => m,
                AppError::Usage(m) => m,
            };
            return Err(die(format!("{path} is not valid JSON ({why})")));
        }
    };
    if !root.is_obj() {
        return Err(die(format!("{path} needs to be a config object")));
    }
    let aliases_obj = match root.get("aliases") {
        Some(Json::Obj(o)) => o,
        _ => return Err(die(format!("{path} has no \"aliases\" object"))),
    };

    let mut cfg = load_config()?;
    let mut added: Vec<String> = Vec::new();
    let mut replaced: Vec<String> = Vec::new();
    let mut skipped: Vec<String> = Vec::new();
    let mut missing: Vec<String> = Vec::new();
    for (name, value) in aliases_obj {
        if !value.is_str() {
            skipped.push(name.clone());
            continue;
        }
        let target = as_stored(&value.as_str());
        if !is_dir(&target) {
            missing.push(format!("{name} -> {target}"));
        }
        if let Some(existing) = cfg.aliases.get(name).cloned() {
            if as_stored(&existing) == target {
                skipped.push(name.clone());
                continue;
            }
            if !args.has("force") {
                replaced.push(name.clone());
                continue;
            }
            cfg.aliases.insert(name.clone(), target);
            continue;
        }
        cfg.aliases.insert(name.clone(), target);
        added.push(name.clone());
    }

    let mut tags_added: Vec<String> = Vec::new();
    if let Some(Json::Obj(tags)) = root.get("tags") {
        for (key, value) in tags {
            let items = match value {
                Json::Arr(a) => a,
                _ => continue,
            };
            let mut members: Vec<String> = Vec::new();
            for item in items {
                if let Json::Str(s) = item {
                    if cfg.aliases.contains_key(s) {
                        members.push(s.clone());
                    }
                }
            }
            if members.is_empty() {
                continue;
            }
            if let Some(idx) = cfg.tags.iter().position(|(k, _)| k == key) {
                for m in &members {
                    if !cfg.tags[idx].1.contains(m) {
                        cfg.tags[idx].1.push(m.clone());
                    }
                }
            } else {
                cfg.tags.push((key.clone(), members));
                tags_added.push(key.clone());
            }
        }
    }

    if !added.is_empty() || !args.has("dry-run") {
        if args.has("dry-run") {
            out(&format!(
                "would add {}, replace {}\n",
                added.len(),
                replaced.len()
            ));
        } else {
            save_config(&cfg)?;
            out(&format!("imported from {path}\n"));
        }
    }
    if !tags_added.is_empty() {
        out(&format!("  tags {}\n", join(&tags_added, " ")));
    }
    if !added.is_empty() {
        out(&format!("  added {}: {}\n", added.len(), join(&added, " ")));
    }
    if !replaced.is_empty() {
        out(&format!(
            "  {} already existed with a different path: {}\n  overwrite: {CONFIG} import --force {}\n",
            replaced.len(),
            join(&replaced, " "),
            shell_quote(&path)
        ));
    }
    if !skipped.is_empty() {
        out(&format!("  {} unchanged\n", skipped.len()));
    }
    if !missing.is_empty() {
        out(&format!(
            "  {} point at directories that don't exist here:\n",
            missing.len()
        ));
        for line in &missing {
            out(&format!("    {line}\n"));
        }
        out(&format!(
            "  they were still imported. bind them properly with: {CONFIG} add --force <name> <path>\n"
        ));
    }
    if added.is_empty() && replaced.is_empty() {
        out("  nothing to do, your config already matches\n");
    }
    Ok(0)
}

// --------------------------------------------------------------------- tidy

fn cmd_tidy(args: &Args) -> Res<i32> {
    let cfg = load_config()?;
    let dirs = read_history();
    let now = now_seconds();

    let dead_aliases: Vec<String> = cfg
        .aliases
        .iter()
        .filter(|(_, path)| !is_dir(&as_stored(path)))
        .map(|(name, _)| name.clone())
        .collect();
    let dead_history: Vec<String> = dirs
        .iter()
        .filter(|(path, visit)| !is_dir(path) || decayed(visit, now) < FLOOR)
        .map(|(path, _)| path.clone())
        .collect();
    let mut stale = 0usize;
    for (_, members) in &cfg.tags {
        for member in members {
            if !cfg.aliases.contains_key(member) || dead_aliases.contains(member) {
                stale += 1;
            }
        }
    }

    if dead_aliases.is_empty() && stale == 0 && dead_history.is_empty() {
        out("nothing to tidy: your aliases, tags and history are in order\n");
        return Ok(0);
    }

    if args.has("dry-run") {
        out(&format!(
            "would unbind {} {}, drop {} stale tag member{} and forget {} dead history entr{}:\n",
            dead_aliases.len(),
            if dead_aliases.len() == 1 { "alias" } else { "aliases" },
            stale,
            if stale == 1 { "" } else { "s" },
            dead_history.len(),
            if dead_history.len() == 1 { "y" } else { "ies" },
        ));
        for name in &dead_aliases {
            out(&format!("  unbind {name} -> {}\n", cfg.aliases[name]));
        }
        for path in &dead_history {
            out(&format!("  forget {path}\n"));
        }
        out(&format!("\n  do it: {} tidy --yes\n", CONFIG));
        return Ok(0);
    }

    if !args.has("yes") && unsafe { isatty(0) } == 1 {
        out(&format!(
            "  unbind {} {}, drop {} stale tag member{} and forget {} dead history entr{}? [y/N] ",
            dead_aliases.len(),
            if dead_aliases.len() == 1 { "alias" } else { "aliases" },
            stale,
            if stale == 1 { "" } else { "s" },
            dead_history.len(),
            if dead_history.len() == 1 { "y" } else { "ies" },
        ));
        flush_out();
        let answer = read_line().unwrap_or_default();
        if lower(&trim(&answer)) != "y" {
            out("  ok, left alone\n");
            return Ok(0);
        }
    }

    let mut next = cfg.clone();
    for name in &dead_aliases {
        next.aliases.remove(name);
    }
    for (_, members) in &mut next.tags {
        members.retain(|member| {
            cfg.aliases.contains_key(member) && !dead_aliases.contains(member)
        });
    }
    next.tags.retain(|(_, members)| !members.is_empty());
    if !dead_aliases.is_empty() || stale > 0 {
        save_config(&next)?;
    }
    if !dead_history.is_empty() {
        let root = ca_history_without(&dirs, &dead_history);
        write_atomic(&history_file(), &(json_dump(&root, 2, 0) + "\n"), false)?;
    }

    out(&format!(
        "tidied: unbound {} {}, dropped {} stale tag member{}, forgot {} dead history entr{}\n",
        dead_aliases.len(),
        if dead_aliases.len() == 1 { "alias" } else { "aliases" },
        stale,
        if stale == 1 { "" } else { "s" },
        dead_history.len(),
        if dead_history.len() == 1 { "y" } else { "ies" },
    ));
    for name in &dead_aliases {
        out(&format!("  unbound {name}\n"));
    }
    for path in &dead_history {
        out(&format!("  forgot {path}\n"));
    }
    Ok(0)
}

// -------------------------------------------------------------------- forget

fn cmd_forget(args: &Args) -> Res<i32> {
    let file = history_file();
    let dirs = read_history();
    if dirs.is_empty() {
        out("your visit log is already empty\n");
        return Ok(0);
    }

    if args.has("all") {
        if !args.has("yes") && unsafe { isatty(0) } == 1 {
            out(&format!(
                "  forget all {} remembered directories? [y/N] ",
                dirs.len()
            ));
            flush_out();
            let answer = read_line().unwrap_or_default();
            if lower(&trim(&answer)) != "y" {
                out("  ok, left alone\n");
                return Ok(0);
            }
        }
        let _ = std::fs::remove_file(&file);
        out(&format!("forgot all {} remembered directories\n", dirs.len()));
        return Ok(0);
    }

    if args.saw("path") {
        let target = args.value("path");
        if !dirs.contains_key(&target) {
            return Err(die(format!(
                "nothing remembered about {target}. see them: {MOVE} top"
            )));
        }
        let skip = vec![target.clone()];
        let root = ca_history_without(&dirs, &skip);
        write_atomic(&file, &(json_dump(&root, 2, 0) + "\n"), false)?;
        out(&format!("forgot {target}\n"));
        return Ok(0);
    }

    let gone: Vec<String> = dirs.keys().filter(|p| !is_dir(p)).cloned().collect();
    if gone.is_empty() {
        out("nothing forgotten: every remembered directory still exists\n");
        return Ok(0);
    }
    let root = ca_history_without(&dirs, &gone);
    write_atomic(&file, &(json_dump(&root, 2, 0) + "\n"), false)?;
    out(&format!(
        "forgot {} directories that no longer exist:\n",
        gone.len()
    ));
    for path in &gone {
        out(&format!("  {path}\n"));
    }
    Ok(0)
}

// ------------------------------------------------------------------- migrate

fn cmd_migrate(args: &Args) -> Res<i32> {
    let cfg = load_config()?;
    let found = cfg.version;
    if found > CONFIG_VERSION {
        return Err(die(format!(
            "your config is version {found}, but this simpledir only knows version {CONFIG_VERSION}.\n  update the program first: {CONFIG} update"
        )));
    }
    if found == CONFIG_VERSION {
        out(&format!(
            "config is already version {CONFIG_VERSION}. nothing to do.\n"
        ));
        return Ok(0);
    }

    let cf = config_file();
    let hf = history_file();
    if args.has("dry-run") {
        let mut msg = format!(
            "would migrate {cf}: version {found} -> {CONFIG_VERSION}\n  {} aliases kept exactly as they are",
            cfg.aliases.len()
        );
        if found < 2 {
            msg.push_str(&format!(
                "\n  would add \"history\": true, and create {hf}"
            ));
        }
        if found < 3 {
            msg.push_str(&format!(
                "\n  would add an empty \"tags\" object, for {CONFIG} tag"
            ));
        }
        msg.push('\n');
        out(&msg);
        return Ok(0);
    }

    let backup = format!("{cf}.bak.{}", stamp());
    let raw = read_file(&cf)?;
    write_atomic(&backup, &raw, false)?;

    let mut cfg = cfg;
    cfg.version = CONFIG_VERSION;
    save_config(&cfg)?;

    let mut did: Vec<String> = Vec::new();
    if found < 2 {
        if !path_exists(&hf) {
            let mut root = Json::obj();
            root.set("version", Json::num(1.0));
            root.set("dirs", Json::obj());
            write_atomic(&hf, &(json_dump(&root, 2, 0) + "\n"), false)?;
        }
        did.push(format!(
            "created {hf} (empty), so {MOVE} top has something to read"
        ));
    }
    if found < 3 {
        did.push(format!(
            "added an empty \"tags\" object, for {CONFIG} tag"
        ));
    }

    out(&format!(
        "migrated {cf}: version {found} -> {CONFIG_VERSION}\n  {} aliases kept as they were\n  backup: {backup}\n",
        cfg.aliases.len()
    ));
    for line in &did {
        out(&format!("  {line}\n"));
    }
    if found < 2 {
        out(&format!("  now `{MOVE} top` will remember where you go\n"));
    }
    if found < 3 {
        out(&format!(
            "  tags: `{CONFIG} tag <name> <alias>...`, then `{MOVE} @<name> <alias>`\n"
        ));
    }
    Ok(0)
}

// -------------------------------------------------------------------- zoxide

fn cmd_zoxide(args: &Args) -> Res<i32> {
    let mut cfg = load_config()?;
    let home = home_dir();
    let mut db = format!(
        "{}/db.zdb",
        env_or("ZO_DATA_DIR", &(home.clone() + "/.local/share/zoxide"))
    );
    let legacy = format!("{home}/.zoxide.db");
    if !path_exists(&db) {
        if path_exists(&legacy) {
            db = legacy.clone();
        } else {
            return Err(die(format!(
                "couldn't find zoxide's database.\n  looked for:\n    {db}\n    {legacy}\n  it lives in $ZO_DATA_DIR if you moved it. point me at it: ZO_DATA_DIR=/path {CONFIG} zoxide"
            )));
        }
    }
    if ca_capture("which sqlite3").is_empty() {
        return Err(die(
            "reading zoxide's database needs the sqlite3 command, which isn't installed.\n  on Arch: sudo pacman -S sqlite",
        ));
    }

    let sql = "SELECT path, rank FROM paths WHERE rank > 0 ORDER BY rank DESC LIMIT 200;";
    let dump = ca_capture(&format!(
        "sqlite3 {} {} 2>/dev/null",
        shell_quote(&db),
        shell_quote(sql)
    ));
    if trim(&dump).is_empty() {
        return Err(die(
            "zoxide's database had nothing in it, or it isn't a sqlite file.\n  if zoxide is very old it used a different format; try `zoxide import` in zoxide itself first",
        ));
    }

    let mut rows: Vec<(String, f64)> = Vec::new();
    for line in split(&dump, '\n') {
        let t = trim(&line);
        if t.is_empty() {
            continue;
        }
        let bar = match t.find('|') {
            Some(b) => b,
            None => continue,
        };
        let path = t[..bar].to_string();
        let rank = ca_atof(&t[bar + 1..]);
        if path.is_empty() || !is_dir(&path) {
            continue;
        }
        rows.push((path, rank));
    }
    if rows.is_empty() {
        return Err(die("zoxide's database has no directories that still exist"));
    }

    let mut limit = 20i32;
    if args.saw("top") {
        limit = ca_atoi(&args.value("top"));
    }
    // A negative limit becomes a huge size_t in the C++ and truncates nothing.
    let keep = if limit >= 0 {
        (limit as usize).min(rows.len())
    } else {
        rows.len()
    };
    rows.truncate(keep);

    let bound: std::collections::BTreeSet<String> =
        cfg.aliases.values().map(|p| as_stored(p)).collect();
    out(&format!(
        "top {} directories from zoxide's database:\n\n",
        rows.len()
    ));
    let width = rows.iter().map(|(p, _)| p.len()).max().unwrap_or(0);
    let mut made: Vec<(String, String)> = Vec::new();
    let mut taken: std::collections::BTreeSet<String> = cfg.aliases.keys().cloned().collect();
    let dry = args.has("dry-run");

    for (path, rank) in &rows {
        let suggested = name_for(path, &taken);
        let already = bound.contains(path);
        out(&format!(
            "  {:6.1}  {}{}{}\n",
            rank,
            path,
            " ".repeat(width - path.len()),
            if already { "   [already bound]" } else { "" }
        ));
        if !already && args.has("bind") {
            cfg.aliases.insert(suggested.clone(), path.clone());
            taken.insert(suggested.clone());
            made.push((suggested, path.clone()));
        }
    }

    if args.has("bind") {
        if dry {
            out(&format!("\nwould bind {}\n", made.len()));
        } else {
            save_config(&cfg)?;
            out(&format!("\nimported {} from zoxide:\n", made.len()));
            for (name, path) in &made {
                out(&format!("  {name} -> {path}\n"));
            }
            out(&format!(
                "\nrename any you got wrong: {CONFIG} rename <old> <new>\n"
            ));
        }
    } else {
        out("\nbind them all:\n");
        for (path, _) in &rows {
            if bound.contains(path) {
                continue;
            }
            let suggested = name_for(path, &taken);
            out(&format!(
                "  {CONFIG} add {} {}\n",
                suggested,
                shell_quote(path)
            ));
            taken.insert(suggested);
        }
    }
    Ok(0)
}
