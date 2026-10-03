#!/usr/bin/env bash
#
# simpledir installer. checks for python, installs it if needed (yay, pacman
# or mise, in that order), then drops the tool in ~/.local/bin and wires your
# shell rc.
#
#   curl -fsSL https://raw.githubusercontent.com/noxthedevwindev-greatest/simpledir/main/install.sh | bash
#
# environment overrides:
#   SIMPLEDIR_BIN_DIR   where to put the binary          (default ~/.local/bin)
#   SIMPLEDIR_RC        which rc file to patch           (default ~/.zshrc or ~/.bashrc)
#   SIMPLEDIR_PM        force a package manager: yay | pacman | mise
#   SIMPLEDIR_NO_RC=1   install the binary, don't touch the rc
#   SIMPLEDIR_RELEASE_URL  where to fetch the tool from  (default: the latest release asset)
#   SIMPLEDIR_RAW_URL      fallback source              (default: the file on main)
#
set -euo pipefail

OWNER="noxthedevwindev-greatest"
REPO="simpledir"
BIN_DIR="${SIMPLEDIR_BIN_DIR:-$HOME/.local/bin}"
BIN="$BIN_DIR/simpledir"
MARK_BEGIN="# >>> simpledir >>>"
MARK_END="# <<< simpledir <<<"

info() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
}

validate_pm() {
  # fail fast on a typo, even when python is already present
  case "${SIMPLEDIR_PM:-}" in
    ""|yay|pacman|mise) ;;
    *) die "SIMPLEDIR_PM must be yay, pacman or mise (got '$SIMPLEDIR_PM')" ;;
  esac
}

# ---------------------------------------------------------------- dependencies

python_ok() {
  command -v python3 >/dev/null 2>&1 || return 1
  python3 -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 8) else 1)' 2>/dev/null
}

python_ver() {
  python3 -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])' 2>/dev/null || echo "?"
}

as_root() { # run a command as root, using sudo only if we aren't already
  if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi
}

install_python() {
  # yay first: on Arch it's the expected front end and it handles AUR too
  local pm="${SIMPLEDIR_PM:-}"
  if [ -z "$pm" ]; then
    for candidate in yay pacman mise; do
      if command -v "$candidate" >/dev/null 2>&1; then pm=$candidate; break; fi
    done
  fi

  case "$pm" in
    yay)
      info "installing python with yay"
      yay -S --noconfirm --needed python
      ;;
    pacman)
      info "installing python with pacman"
      as_root pacman -S --noconfirm --needed python
      ;;
    mise)
      info "installing python with mise"
      mise use -g python@3.12 >/dev/null
      ;;
    "")
      die "python 3.8+ not found, and none of yay, pacman or mise are installed. install python and re-run this script."
      ;;
  esac

  # a package manager can replace the python3 that bash had already hashed, and
  # mise's shims only land on PATH in a fresh shell. drop the cache and retry.
  hash -r 2>/dev/null || true

  python_ok || die "python 3.8+ still isn't on PATH. open a new shell and re-run this script."
}

# ------------------------------------------------------------------ the binary

fetch() { # fetch > path ; tries the latest release asset, falls back to main
  local url out
  for url in \
    "${SIMPLEDIR_RELEASE_URL:-https://github.com/$OWNER/$REPO/releases/latest/download/simpledir}" \
    "${SIMPLEDIR_RAW_URL:-https://raw.githubusercontent.com/$OWNER/$REPO/main/simpledir}"
  do
    out=$(mktemp)
    chmod 755 "$out"
    if curl -fsSL --retry 2 --connect-timeout 10 "$url" -o "$out" 2>/dev/null; then
      # refuse anything that isn't our tool: a 404 page or an html error would
      # otherwise get chmod +x'd into your PATH
      if "$out" --version >/dev/null 2>&1 && "$out" --version 2>/dev/null | grep -q '^simpledir '; then
        printf '%s' "$out"
        return 0
      fi
      warn "downloaded something that isn't simpledir from $url, skipping"
    fi
    rm -f "$out"
  done
  return 1
}

install_binary() {
  local fetched
  info "downloading simpledir"
  fetched=$(fetch) || die "could not download simpledir. check your network and try again."

  mkdir -p "$BIN_DIR"
  # write into the target directory, then rename: never a half-written binary
  local staged
  staged=$(mktemp "$BIN_DIR/.simpledir.XXXXXX")
  cat "$fetched" > "$staged"
  chmod 755 "$staged"
  mv -f "$staged" "$BIN"
  rm -f "$fetched"

  info "installed $BIN ($("$BIN" --version))"
}

# ----------------------------------------------------------------- the rc file

pick_rc() {
  if [ -n "${SIMPLEDIR_RC-}" ]; then
    printf '%s' "$SIMPLEDIR_RC"
    return
  fi
  case "${SHELL:-}" in
    */zsh) printf '%s' "${ZDOTDIR:-$HOME}/.zshrc" ;;
    *)     printf '%s' "$HOME/.bashrc" ;;
  esac
}

wire_rc() {
  if [ -n "${SIMPLEDIR_NO_RC-}" ]; then
    info "skipping rc setup (SIMPLEDIR_NO_RC)"
    return
  fi

  local rc
  rc=$(pick_rc)
  touch "$rc"

  if grep -qF "$MARK_BEGIN" "$rc"; then
    # already wired: refresh it so a new subcommand list reaches the wrapper
    local backup="$rc.bak.$(date +%Y%m%d%H%M%S)"
    cp -p "$rc" "$backup"
    local kept
    kept=$(mktemp)
    sed "/$(printf '%s' "$MARK_BEGIN" | sed 's/[][\.*^$/]/\\&/g')/,/$(printf '%s' "$MARK_END" | sed 's/[][\.*^$/]/\\&/g')/d" "$rc" > "$kept"
    { cat "$kept"; echo; echo "$MARK_BEGIN"; "$BIN" init; echo "$MARK_END"; } > "$rc"
    rm -f "$kept"
    info "refreshed the wrapper block in $rc (old one in $backup)"
  else
    { echo; echo "$MARK_BEGIN"; "$BIN" init; echo "$MARK_END"; } >> "$rc"
    info "added the wrapper to $rc"
  fi

  case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) warn "$BIN_DIR is not in your PATH. add this to your shell rc:"
       warn "    export PATH=\"$BIN_DIR:\$PATH\"" ;;
  esac
}

uninstall() {
  # prefer the tool's own uninstaller: one implementation of "what to remove".
  # SIMPLEDIR_RC keeps it to this installerrc file and nothing else.
  local rc; rc=$(pick_rc)
  if [ -x "$BIN" ]; then
    if SIMPLEDIR_RC="$rc" "$BIN" uninstall --yes; then
      return 0
    fi
    warn "simpledir uninstall exited non-zero, falling back to doing it by hand"
  fi

  # fall back to doing it by hand, for when the binary is already gone or broken
  command -v simpledir >/dev/null 2>&1 && info "removing $BIN"
  rm -f "$BIN"
  if [ -f "$rc" ] && grep -qF "$MARK_BEGIN" "$rc"; then
    cp -p "$rc" "$rc.bak.$(date +%Y%m%d%H%M%S)"
    sed "/$(printf '%s' "$MARK_BEGIN" | sed 's/[][\.*^$/]/\\&/g')/,/$(printf '%s' "$MARK_END" | sed 's/[][\.*^$/]/\\&/g')/d" "$rc" > "$rc.tmp"
    mv -f "$rc.tmp" "$rc"
    info "removed the wrapper block from $rc"
  fi
  info "done. your aliases are still in ~/.simpledir/config.json"
}

# ----------------------------------------------------------------------- main

case "${1-}" in
  -h|--help) usage; exit 0 ;;
  -u|--uninstall) uninstall; exit $? ;;
  "") ;;
  *) die "unknown option '$1'. try --help" ;;
esac

info "simpledir installer"
validate_pm

if python_ok; then
  info "python $(python_ver) found"
else
  warn "python 3.8+ not found"
  install_python
  info "python $(python_ver) ready"
fi

install_binary
wire_rc

cat <<EOF

  simpledir is installed.

    simpledir add <name>     bind the current directory
    sd <name>                jump to it

  open a new shell (or run: source $(pick_rc)) and try:

    cd ~ && simpledir add home && simpledir ls

  docs: https://github.com/$OWNER/$REPO
EOF
