#!/usr/bin/env bash
#
# simpledir installer. finds a prebuilt binary for this box, or compiles one,
# then installs sd + sdcfg and wires your shell rc.
#
#   curl -fsSL https://raw.githubusercontent.com/noxthedevwindev-greatest/simpledir/v9.0.0/install.sh | bash
#
# options:
#   --source        compile from source even if a binary exists
#   --repair        refresh an existing install without asking
#   --uninstall     remove the binaries and the rc block
#   --help
#
# environment:
#   SIMPLEDIR_BIN_DIR   where to install            (default ~/.local/bin)
#   SIMPLEDIR_RC        which rc file to patch      (default ~/.zshrc or ~/.bashrc)
#   SIMPLEDIR_PM        force a package manager: yay | pacman | mise
#   SIMPLEDIR_NO_RC=1   install the binaries, don't touch the rc
#   SIMPLEDIR_SOURCE_URL  base URL to fetch sd.cpp from when compiling
#   SIMPLEDIR_BASE_URL    where release assets are fetched from
#
set -euo pipefail

OWNER="noxthedevwindev-greatest"
REPO="simpledir"
BASE="${SIMPLEDIR_BASE_URL:-https://github.com/$OWNER/$REPO}"
# a base directory for raw files, not a full path to one
# The version this installer belongs to. Fetch from the *tag*, not from main:
# raw.githubusercontent caches by path, and it will happily serve you an
# install.sh from before the fix you are reading about — which is exactly what
# happened to v7.0.2, and why the question printed nothing. A tagged path never
# changes, so the cache is always right for the version it names.
#
# Keep this in step with VERSION in sd.cpp; a test checks that it is.
SD_VERSION="9.0.0"
RAW="${SIMPLEDIR_SOURCE_URL:-https://raw.githubusercontent.com/$OWNER/$REPO/v$SD_VERSION}"
case "$RAW" in */) ;; *) RAW="$RAW/" ;; esac
BIN_DIR="${SIMPLEDIR_BIN_DIR:-$HOME/.local/bin}"
SD="$BIN_DIR/sd"
SDCFG="$BIN_DIR/sdcfg"
MARK_BEGIN="# >>> simpledir >>>"
MARK_END="# <<< simpledir <<<"

info() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

MODE="install"
FORCE_SOURCE=0
case "${1-}" in
  -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  --source)   FORCE_SOURCE=1; shift ;;
  --repair)   MODE=repair; shift ;;
  -u|--uninstall) MODE=uninstall; shift ;;
  "") ;;
  *) die "unknown option '$1'. try --help" ;;
esac

# ------------------------------------------------------------------- platform

uname_s=$(uname -s)
uname_m=$(uname -m)

# only linux, as advertised. say so plainly rather than installing something
# that was never built or tested here.
if [ "$uname_s" != "Linux" ]; then
  die "this build of simpledir is for Linux only, and this is $uname_s.
  building from source here would need a port; the source is one file:
    git clone $BASE && cd simpledir && make"
fi

case "$uname_m" in
  x86_64|amd64)      ARCH=x86_64 ;;
  aarch64|arm64)     ARCH=arm64 ;;
  *) die "no prebuilt binary for $uname_m. build from source: bash $0 --source" ;;
esac

ASSET="sd-linux-$ARCH"

have() { command -v "$1" >/dev/null 2>&1; }

# the version currently installed, or empty if there isn't one
installed_version() {
  [ -x "$SD" ] || return 0
  "$SD" --version 2>/dev/null | sed -n 's/^sd \([0-9][0-9.]*\).*/\1/p'
}

# is $1 older than $2? a plain numeric field-by-field compare; these are the two
# or three numbers a release tag ever has.
older_than() {
  [ "$1" != "$2" ] || return 1
  [ "$(printf '%s\n%s\n' "$2" "$1" | sort -t. -k1,1n -k2,2n -k3,3n | head -1)" = "$1" ]
}

ALLOW_DOWNGRADE="${SIMPLEDIR_ALLOW_DOWNGRADE:-0}"

# Ask a question and put the answer in REPLY. Reads the terminal, not stdin: when
# this script is piped into bash, stdin is the script and /dev/tty is the user's
# keyboard. Returns non-zero when there is no terminal at all, so callers can
# decide what to do rather than silently guessing.
REPLY=""
ask() { # ask <default>
  if [ -t 0 ]; then
    printf '  what should i do? [r/u/c] ' >&2
    read -r REPLY || REPLY=""
  elif exec 3<>/dev/tty 2>/dev/null; then
    # Opening it is the only reliable test. `[ -r /dev/tty ]` is true whenever the
    # node exists and the mode allows it, even with no controlling terminal, and
    # the read then dies with ENXIO — which read as "asked, got nothing" and so
    # silently took the default.
    #
    # `<>` and not `<`: read-only, and the prompt written to fd 3 fails, so the
    # installer asked a question it never displayed and only appeared to work
    # because the default happened to be the right answer.
    printf '  what should i do? [r/u/c] ' >&3
    read -r REPLY <&3 || REPLY=""
    exec 3<&-
  else
    return 1
  fi
  [ -z "$REPLY" ] && REPLY=$1
  printf '\n' >&2
  return 0
}

compiler() { # prints the name of a C++ compiler, or nothing
  if have g++; then echo g++
  elif have clang++; then echo clang++
  fi
}

# --------------------------------------------------------------- the toolchain

# only needed when there's no binary for this box
install_toolchain() {
  if compiler >/dev/null; then
    return 0
  fi
  local pm="${SIMPLEDIR_PM:-}"
  if [ -z "$pm" ]; then
    for candidate in yay pacman mise; do
      if have "$candidate"; then pm=$candidate; break; fi
    done
  fi
  case "${SIMPLEDIR_PM:-}" in
    yay|pacman|mise) ;;
    "") warn "no C++ compiler found, and no yay, pacman or mise to install one with" ;;
    *) die "SIMPLEDIR_PM must be yay, pacman or mise (got '$SIMPLEDIR_PM')" ;;
  esac

  info "installing a C++ toolchain with $pm"
  case "$pm" in
    yay)    yay -S --noconfirm --needed base-devel ;;
    pacman)
      if [ "$(id -u)" -eq 0 ]; then pacman -S --noconfirm --needed base-devel
      else sudo pacman -S --noconfirm --needed base-devel; fi ;;
    mise)   mise use -g gcc@latest >/dev/null; mise use -g llvm@latest >/dev/null ;;
    "")     die "install gcc (Arch: sudo pacman -S base-devel), then re-run with --source" ;;
  esac
  compiler >/dev/null || die "still no C++ compiler. install one and re-run with --source"
}

# ----------------------------------------------------------------- fetching it

fetch() { # fetch <url> <dest>; returns non-zero if it isn't there
  curl -fsSL --retry 2 --connect-timeout 15 "$1" -o "$2" 2>/dev/null
}

# Put a working `sd` at the path given. Tries the prebuilt binary for this
# box first, and compiles from source if there isn't one. No traps: explicit
# cleanup is easier to reason about than a RETURN trap that fires when some
# nested function happens to return.
obtain() {
  local dest=$1
  local work; work=$(mktemp -d)
  local ok=0

  if [ "$FORCE_SOURCE" = "0" ]; then
    info "fetching the $ASSET binary"
    if fetch "$BASE/releases/latest/download/$ASSET" "$work/sd"; then
      chmod 755 "$work/sd"
      # refuse anything that isn't our tool: a 404 page would otherwise get
      # chmod +x'd into your PATH
      if "$work/sd" --version 2>/dev/null | grep -q '^sd '; then
        got=$("$work/sd" --version | sed -n 's/^sd \([0-9][0-9.]*\).*/\1/p')
        # $(...), not a bare word: `have=installed_version` assigns the literal
        # string, and the comparison below then quietly compares against it
        have=$(installed_version)
        # `releases/latest/download/` is a redirect, and redirects get cached. A
        # stale one hands you the previous release, and an installer that only
        # checks "is this our tool?" will happily downgrade you. It did, silently,
        # to 6.5.0 while 7.0.0 was published.
        # Two floors, not one. `got` must not be older than this installer, or
        # the cache is serving a release from before the installer you are running;
        # and it must not be older than what is already installed, or the cached
        # `latest` redirect is undoing an upgrade. A *newer* asset is fine and
        # expected — this installer installs whatever latest is.
        stale_for_this=0
        if [ -n "$got" ] && older_than "$got" "$SD_VERSION"; then stale_for_this=1; fi
        if [ -n "$have" ] && [ -n "$got" ] && older_than "$got" "$have"; then
          warn "the download is v$got but v$have is already installed."
          warn "that's a stale mirror or cache of releases/latest/download, not a real downgrade."
        fi
        if [ "$stale_for_this" = "1" ]; then
          warn "the download is v$got but this installer is v$SD_VERSION."
          warn "raw.githubusercontent caches by path, so it served an older release."
        fi
        if [ "$stale_for_this" = "1" ] || { [ -n "$have" ] && [ -n "$got" ] && older_than "$got" "$have"; }; then
          if [ "$ALLOW_DOWNGRADE" = "1" ]; then
            warn "installing it anyway, because SIMPLEDIR_ALLOW_DOWNGRADE=1."
          else
            warn "not installing it. to override: SIMPLEDIR_ALLOW_DOWNGRADE=1 bash $0"
            warn "or build from the source you already have: bash $0 --source"
            rm -rf "$work"
            exit 1
          fi
        elif [ -n "$got" ] && [ "$got" != "$SD_VERSION" ]; then
          info "(that is newer than this installer, v$SD_VERSION. fine.)"
        fi
        info "got $("$work/sd" --version)"
        cp "$work/sd" "$dest"
        ok=1
      else
        warn "that download isn't simpledir. building from source instead."
      fi
    else
      warn "no prebuilt binary at $BASE/releases/latest/download/$ASSET"
    fi
  fi

  if [ "$ok" = "0" ]; then
    install_toolchain
    info "compiling from source (one file, this takes a few seconds)"
    fetch "$RAW/sd.cpp" "$work/sd.cpp" || { rm -rf "$work"; die "couldn't download sd.cpp from $RAW"; }
    if ! "$(compiler)" -std=c++17 -O2 -static-libstdc++ -static-libgcc -o "$work/sd" "$work/sd.cpp"; then
      rm -rf "$work"
      die "compilation failed. sd.cpp is one file; the full error is above."
    fi
    "$work/sd" --version >/dev/null || { rm -rf "$work"; die "the build didn't produce a working binary"; }
    info "built $("$work/sd" --version)"
    cp "$work/sd" "$dest"
  fi

  chmod 755 "$dest"
  rm -rf "$work"
}

# ------------------------------------------------------------------ the rc file

pick_rc() {
  if [ -n "${SIMPLEDIR_RC-}" ]; then printf '%s' "$SIMPLEDIR_RC"; return; fi
  case "${SHELL:-}" in
    */zsh) printf '%s' "${ZDOTDIR:-$HOME}/.zshrc" ;;
    *)     printf '%s' "$HOME/.bashrc" ;;
  esac
}

marker_re() { printf '%s' "$1" | sed 's/[][\.*^$/]/\\&/g'; }

wire_rc() {
  if [ -n "${SIMPLEDIR_NO_RC-}" ]; then
    info "skipping rc setup (SIMPLEDIR_NO_RC)"
    return
  fi
  local rc; rc=$(pick_rc)
  touch "$rc"

  if grep -qF "$MARK_BEGIN" "$rc"; then
    local backup="$rc.bak.$(date +%Y%m%d%H%M%S)"
    cp -p "$rc" "$backup"
    local kept; kept=$(mktemp)
    sed "/$(marker_re "$MARK_BEGIN")/,/$(marker_re "$MARK_END")/d" "$rc" > "$kept"
    { cat "$kept"; echo; echo "$MARK_BEGIN"; "$SDCFG" init; echo "$MARK_END"; } > "$rc"
    rm -f "$kept"
    info "refreshed the wrapper block in $rc (the old one is in $backup)"
  else
    { echo; echo "$MARK_BEGIN"; "$SDCFG" init; echo "$MARK_END"; } >> "$rc"
    info "added the wrapper to $rc"
  fi

  case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) warn "$BIN_DIR is not in your PATH. add this to your shell rc:"
       warn "    export PATH=\"$BIN_DIR:\$PATH\"" ;;
  esac
}

remove_rc_block() {
  local rc; rc=$(pick_rc)
  if [ -f "$rc" ] && grep -qF "$MARK_BEGIN" "$rc"; then
    cp -p "$rc" "$rc.bak.$(date +%Y%m%d%H%M%S)"
    sed "/$(marker_re "$MARK_BEGIN")/,/$(marker_re "$MARK_END")/d" "$rc" > "$rc.tmp"
    mv -f "$rc.tmp" "$rc"
    info "removed the wrapper block from $rc"
  fi
}

# --------------------------------------------------------------------- install

install_tool() {
  local work; work=$(mktemp -d)
  obtain "$work/sd" || { rm -rf "$work"; die "could not obtain a working sd"; }

  mkdir -p "$BIN_DIR"
  # write beside the target, then rename: never a half-written binary in PATH
  local staged; staged=$(mktemp "$BIN_DIR/.sd.XXXXXX")
  cat "$work/sd" > "$staged"
  chmod 755 "$staged"
  mv -f "$staged" "$SD"
  ln -sfn sd "$SDCFG"
  rm -rf "$work"
  info "installed $SD and $SDCFG"
}

# An existing install is a normal state, not an error. ask what to do with it
# rather than silently overwriting or silently refusing.
handle_existing() {
  local rc; rc=$(pick_rc)
  local have_binary=0 have_rc=0
  [ -x "$SD" ] && have_binary=1
  [ -f "$rc" ] && grep -qF "$MARK_BEGIN" "$rc" && have_rc=1
  [ "$have_binary$have_rc" = "00" ] && return 0

  info "found an existing simpledir install:"
  [ "$have_binary" = "1" ] && info "  $SD ($("$SD" --version 2>/dev/null || echo 'unknown version'))"
  [ "$have_rc" = "1" ] && info "  wrapper block in $rc"
  info "  your aliases are in ~/.simpledir and are never touched by this script"

  if [ "$MODE" = "repair" ]; then
    info "repairing: replacing the binaries and refreshing the wrapper"
    return 0
  fi

  printf '\n'
  printf '  \033[1mr\033[0mepair    replace the binaries and refresh the wrapper (default)\n'
  printf '  \033[1mu\033[0mninstall  remove the binaries and the wrapper, keep your aliases\n'
  printf '  \033[1mc\033[0mancel    leave everything as it is and do nothing\n\n'

  # `curl ... | bash` is the documented install path, and there stdin is a pipe,
  # so `[ -t 0 ]` is false and the question could never be asked — the exact case
  # the question exists for. /dev/tty is still the terminal in that situation.
  if ! ask r; then
    warn "no terminal to ask on. repairing the install (pass --uninstall to remove it)."
    return 0
  fi

  case "$REPLY" in
    u|U) MODE=uninstall ;;
    c|C) info "cancelled. nothing was changed."; exit 0 ;;
    *)   info "repairing" ;;
  esac
}

uninstall() {
  if [ -x "$SDCFG" ]; then
    SIMPLEDIR_RC="$(pick_rc)" "$SDCFG" uninstall --yes || warn "sdcfg uninstall reported a problem, finishing by hand"
    return 0
  fi
  rm -f "$SD" "$SDCFG"
  remove_rc_block
  info "done. your aliases are still in ~/.simpledir/config.json"
}

# ----------------------------------------------------------------------- main

# a typo in SIMPLEDIR_PM should be loud now, not 20 lines later when it matters
case "${SIMPLEDIR_PM:-}" in
  ""|yay|pacman|mise) ;;
  *) die "SIMPLEDIR_PM must be yay, pacman or mise (got '$SIMPLEDIR_PM')" ;;
esac

info "simpledir installer ($uname_s/$ARCH)"
handle_existing

if [ "$MODE" = "uninstall" ]; then
  uninstall
  exit 0
fi

install_tool
wire_rc

cat <<EOF

  simpledir is installed. two commands:

    sd <alias>           jump there
    sd ls                list what you can jump to
    sd top               the frecency log
    sdcfg add <name>     bind the current directory
    sdcfg import ~/Projects

  open a new shell (or run: source $(pick_rc)) and try:

    cd ~ && sdcfg add home && sd ls

  docs: $BASE
EOF
