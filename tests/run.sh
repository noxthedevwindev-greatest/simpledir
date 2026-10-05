#!/usr/bin/env bash
# simpledir test suite. isolated config + a real bash that sources the wrapper.
#
# two commands, one program: `sd` reads and moves, `sdcfg` writes.
# $SD is the move half, $CFG the config half, both copies of the same file.
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export SIMPLEDIR_CONFIG_DIR=$(mktemp -d)
trap 'rm -rf "$SIMPLEDIR_CONFIG_DIR"' EXIT

# two names, one file. the mode is decided by argv[0], exactly like the install.
SD="$ROOT/sd"
CFG="$ROOT/sdcfg"
ln -sf sd "$CFG"

pass=0 fail=0

check() { # check <label> <expected-exit> <command...>
  local label=$1 want=$2; shift 2
  local out; out=$("$@" 2>&1); local got=$?
  if [[ $got -eq $want ]]; then
    pass=$((pass + 1)); printf 'ok   %s\n' "$label"
  else
    fail=$((fail + 1)); printf 'FAIL %s (exit %s, wanted %s)\n%s\n' "$label" "$got" "$want" "$out"
  fi
}

contains() { # contains <label> <needle> <command...>
  local label=$1 needle=$2; shift 2
  local out; out=$("$@" 2>&1)
  if [[ $out == *"$needle"* ]]; then
    pass=$((pass + 1)); printf 'ok   %s\n' "$label"
  else
    fail=$((fail + 1)); printf 'FAIL %s (missing %q)\n%s\n' "$label" "$needle" "$out"
  fi
}

lacks() { # lacks <label> <needle> <command...>
  local label=$1 needle=$2; shift 2
  local out; out=$("$@" 2>&1)
  if [[ $out != *"$needle"* ]]; then
    pass=$((pass + 1)); printf 'ok   %s\n' "$label"
  else
    fail=$((fail + 1)); printf 'FAIL %s (should not contain %q)\n%s\n' "$label" "$needle" "$out"
  fi
}

has() { # has <label> <needle> <text> - for output you already captured
  local label=$1 needle=$2 text=$3
  if [[ $text == *"$needle"* ]]; then
    pass=$((pass + 1)); printf 'ok   %s\n' "$label"
  else
    fail=$((fail + 1)); printf 'FAIL %s (missing %q)\n%s\n' "$label" "$needle" "$text"
  fi
}

# the update nudge only fires on a terminal, so those checks get a pty
on_tty() { script -qec "$*" /dev/null; }

# --- the split itself --------------------------------------------------------
check "both names run"            0 "$SD" --version
check "both names run"            0 "$CFG" --version
contains "sd calls itself sd"    "sd " "$SD" --version
contains "sdcfg calls itself sdcfg" "sdcfg " "$CFG" --version

# sd must not answer config verbs, and should point at sdcfg when it doesn't
check "sd refuses to add"         2 "$SD" add somewhere /tmp
contains "sd points at sdcfg"    'sdcfg add' "$SD" add somewhere /tmp
check "sd refuses rm"            2 "$SD" rm whatever
check "sd refuses import"         2 "$SD" import /tmp
contains "sd says it only reads" "only reads your config" "$SD" add somewhere /tmp

# sdcfg must not answer move verbs, and should point at sd when it doesn't
check "sdcfg refuses a bare alias" 2 "$CFG" whatever
check "sdcfg refuses a move verb"  2 "$CFG" ls
contains "sdcfg points at sd"    'sd ls' "$CFG" ls
contains "sdcfg says it only writes" "only changes things" "$CFG" ls
check "sdcfg refuses ls"          2 "$CFG" ls
check "sdcfg refuses i"           2 "$CFG" i

# --- sdcfg add ---------------------------------------------------------------
check "add binds cwd"          0 "$CFG" add here
check "add with explicit path" 0 "$CFG" add home "$HOME"
check "add tilde path"         0 "$CFG" add dotdir "$HOME/.config"
check "add rejects bad path"   1 "$CFG" add nope /does/not/exist
check "add no clobber"         1 "$CFG" add here
check "add --force"            0 "$CFG" add --force here "$HOME"

# --- sd ls -------------------------------------------------------------------
contains "ls lists alias"      "here"  "$SD" ls
contains "ls --long absolute"  "$HOME" "$SD" ls --long
while read -r name; do "$CFG" rm "$name" >/dev/null; done < <("$SD" ls --names)
check "ls empty exits 1"       1 "$SD" ls

# --- sd print / jump ---------------------------------------------------------
"$CFG" add home "$HOME" >/dev/null
"$CFG" add dotdir "$HOME/.config" >/dev/null
check "print resolves"         0 "$SD" print home
contains "print echoes path"   "$HOME" "$SD" print home
check "bare name jumps"        0 "$SD" home
contains "bare form echoes path" "$HOME" "$SD" home
check "unknown exits 1"        1 "$SD" print zzzz
contains "suggests near miss"  "did you mean" "$SD" print hoem
contains "suggests a typo"     "did you mean" "$SD" print dotdirx
check "no args = help"         0 "$SD"

# --- prefix matching ---------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$CFG" add hypr "$HOME" >/dev/null
"$CFG" add hyprland "$HOME" >/dev/null
"$CFG" add kbd "$HOME" >/dev/null
check "unique prefix jumps"        0 "$SD" print kbd
contains "unique prefix resolves"  "$HOME" "$SD" print kbd
"$CFG" rename hyprland hl >/dev/null
check "prefix after a rename"      0 "$SD" print hy
check "exact name beats prefix"    0 "$SD" print hypr
check "ambiguous prefix fails"     1 "$SD" print h
contains "ambiguous prefix lists them" "hl hypr" "$SD" print h

# --- subdirectory suffixes ---------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tree/src/deep"
"$CFG" add tree "$SIMPLEDIR_CONFIG_DIR/tree" >/dev/null
contains "suffix resolves"     "/tree/src"          "$SD" print tree/src
contains "deep suffix"         "/tree/src/deep"     "$SD" print tree/src/deep
contains "suffix normalizes .." "/tree/src"         "$SD" print tree/src/../src
contains "trailing slash ok"   "/tree"              "$SD" print tree/
check "missing suffix fails"   1 "$SD" print tree/nope
contains "missing suffix names it" "tree/nope"       "$SD" print tree/nope
check "absolute path not an alias" 1 "$SD" print /tmp
contains "absolute path says so" "isn't an alias"    "$SD" print /tmp

# --- sdcfg rm / rename -------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$CFG" add dots "$HOME" >/dev/null
check "rename works"           0 "$CFG" rename dots dotfiles
contains "rename keeps path"   "$HOME" "$SD" print dotfiles
check "old name is gone"       1 "$SD" print dots
"$CFG" add other /tmp >/dev/null
check "rename no clobber"      1 "$CFG" rename dotfiles other
check "rename --force"         0 "$CFG" rename --force dotfiles other
check "rename unknown fails"   1 "$CFG" rename nope whatever
"$CFG" add gone /tmp >/dev/null
check "rm removes"             0 "$CFG" rm gone
contains "rm gone says gone"   "gone" "$CFG" rm gone
check "rm unknown fails"       1 "$CFG" rm nope

# --- missing target ----------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tmpdir"
"$CFG" add vanish "$SIMPLEDIR_CONFIG_DIR/tmpdir" >/dev/null
rmdir "$SIMPLEDIR_CONFIG_DIR/tmpdir"
check "print refuses missing"  1 "$SD" print vanish
contains "ls tags missing"     "[missing]" "$SD" ls

# --- config integrity --------------------------------------------------------
contains "config is json" '"aliases"' cat "$SIMPLEDIR_CONFIG_DIR/config.json"
check "no temp files left"     0 bash -c "! compgen -G '$SIMPLEDIR_CONFIG_DIR/.config.*'"
printf 'not json{' > "$SIMPLEDIR_CONFIG_DIR/config.json"
contains "corrupt config warns" "not valid JSON" "$SD" ls

# --- sdcfg add with fewer arguments ------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/dotfiles"
contains "add derives name"    "dotfiles ->" bash -c "cd '$SIMPLEDIR_CONFIG_DIR/dotfiles' && '$CFG' add --force"
check "add on / is nameable"   0 "$CFG" add rootdir /
contains "add name only"       "mydir ->" bash -c "cd '$SIMPLEDIR_CONFIG_DIR' && '$CFG' add mydir"

# --- sd ls flags and filter --------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$CFG" add alpha "$HOME" >/dev/null
"$CFG" add beta /tmp >/dev/null
contains "ls filters by name"  "alpha"  "$SD" ls alp
contains "ls filters by path"  "beta"   "$SD" ls tmp
check "ls filter miss exits 1" 1 "$SD" ls zzz
contains "ls filter says so"   "nothing matches" "$SD" ls zzz
contains "ls --names"          "alpha"  "$SD" ls --names
check "ls --names is one per line" 0 bash -c "'$SD' ls --names | grep -qx alpha"
contains "ls --json has aliases" '"aliases"' "$SD" ls --json
contains "ls --json has version" '"version"' "$SD" ls --json
check "ls --json is valid json" 0 bash -c "'$SD' ls --json | python3 -c 'import json,sys; json.load(sys.stdin)'"

# --- add --keep-symlinks -----------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/real"
ln -sfn "$SIMPLEDIR_CONFIG_DIR/real" "$SIMPLEDIR_CONFIG_DIR/link"
"$CFG" add resolved "$SIMPLEDIR_CONFIG_DIR/link" >/dev/null
"$CFG" add literal --keep-symlinks "$SIMPLEDIR_CONFIG_DIR/link" >/dev/null
contains "default resolves the symlink" "/real" "$SD" print resolved
contains "--keep-symlinks keeps it"      "/link" "$SD" print literal
check "both still point at a real dir"   0 "$SD" print literal
lacks "no hint when resolved"            "kept the symlink" "$CFG" add resolved2 "$SIMPLEDIR_CONFIG_DIR/link"

# --- sdcfg import ------------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tree"/{alpha,beta,gamma/nested/leaf} "$SIMPLEDIR_CONFIG_DIR/tree/.hidden"
contains "import binds children"  "alpha beta gamma" "$CFG" import "$SIMPLEDIR_CONFIG_DIR/tree"
check "import is idempotent"     0 "$CFG" import "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import skips bound"    "skipped 3"  "$CFG" import "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import with prefix"    "p-alpha"    "$CFG" import --prefix p- "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import depth 2"        "g-nested"   "$CFG" import --depth 2 --prefix g- "$SIMPLEDIR_CONFIG_DIR/tree"
lacks   "import skips dotfiles" ".hidden"    "$CFG" import --depth 1 --prefix z- "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import --hidden takes them" ".hidden" "$CFG" import --hidden --depth 1 --prefix h- "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import --dry-run says would" "would bind" "$CFG" import --dry-run --prefix dry- --depth 1 "$SIMPLEDIR_CONFIG_DIR/tree"
check "import --dry-run writes nothing" 1 "$SD" ls dry-alpha
contains "import rejects a file"  "not a directory" "$CFG" import /etc/hostname

# --- sd suggest + sdcfg bind -------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
HIST=$(mktemp -d)
# a fake HOME, or the developer's real ~/.bash_history leaks into the counts
mkdir -p "$HIST/home" "$HIST/alpha" "$HIST/beta" "$HIST/gamma"
cat > "$HIST/home/.bash_history" <<EOF
cd $HIST/alpha
cd $HIST/alpha
cd $HIST/alpha/sub
cd $HIST/beta
cd '$HIST/gamma'
pushd $HIST/gamma
cd ~/definitely/not/here
cd
cd -
cd /tmp
ls -l $HIST/beta
EOF
sexport() { env HOME="$HIST/home" SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" "$@"; }

contains "suggest counts visits"   "2x"  sexport "$SD" suggest
contains "suggest lists a dir"     "$HIST/alpha" sexport "$SD" suggest
lacks   "suggest skips non-dirs"  "not/here"     sexport "$SD" suggest
lacks   "suggest ignores ls -l"    "ls -l"        sexport "$SD" suggest
contains "suggest emits an sdcfg add line" "sdcfg add alpha $HIST/alpha" sexport "$SD" suggest
check "suggest --json is valid json" 0 bash -c "env HOME='$HIST/home' SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' suggest --json | python3 -c 'import json,sys; json.load(sys.stdin)'"
contains "suggest --top limits"    "top 1" sexport "$SD" suggest --top 1
lacks   "suggest --top respects it" "$HIST/beta" sexport "$SD" suggest --top 1
# sd must never write: bind lives on the other half
check "sd has no bind verb"       1 "$SD" bind

contains "sdcfg bind creates" "bound 4" sexport "$CFG" bind
check "sdcfg bind wrote aliases" 0 bash -c "'$SD' ls --names | grep -qx alpha"
contains "suggest knows they're bound" "already bound" sexport "$SD" suggest
check "suggest exits 0 when all bound" 0 bash -c \
  "env HOME='$HIST/home' SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' suggest"
contains "bind --dry-run says would" "would bind" bash -c \
  "rm -rf '$SIMPLEDIR_CONFIG_DIR'; env HOME='$HIST/home' SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$CFG' bind --dry-run"

: > "$HIST/home/.bash_history"
contains "empty history explains itself" 'no `cd` targets' sexport "$SD" suggest
check "empty history exits 1"       1 sexport "$SD" suggest
mv "$HIST/home/.bash_history" "$HIST/home/.bash_history.off"
check "missing history explains itself" 1 sexport "$SD" suggest
contains "missing history lists where" ".bash_history" sexport "$SD" suggest
rm -rf "$HIST"

# relative paths are the common case, and `cd` must not match mid-argument
REL=$(mktemp -d)
mkdir -p "$REL/home/Projects/one" "$REL/home/Projects/two"
cat > "$REL/home/.bash_history" <<EOF
cd Projects
cd Projects/one
cd Projects/two
cd ./Projects/one
echo cd Projects
ls Projects
sudo cd Projects
cd Projects/one && cd Projects/two
cd nosuchdir
EOF
rel() { env HOME="$REL/home" SIMPLEDIR_CONFIG_DIR="$REL/cfg" "$@"; }
out=$(rel "$SD" suggest)
has "relative cd resolves against \$HOME" "$REL/home/Projects/one" "$out"
has "relative cd counts every visit"    "2x" "$out"
has "sudo cd counts"                    "$REL/home/Projects" "$out"
lacks "echo cd is not a cd"             "echo cd" "$out"
lacks "a nonexistent relative path is skipped" "nosuchdir" "$out"
rm -rf "$REL"

# --- sd i (picker) -----------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$CFG" add alpha "$HOME" >/dev/null
"$CFG" add beta /tmp >/dev/null
STUB=$(mktemp -d)
# Real fzf takes its list on stdin and rejects positional arguments outright
# (`fzf one two` -> "unknown option: one"), so the stub does the same. It also
# records what it was handed, so a test can assert the shape of the list rather
# than just that a name came back.
cat > "$STUB/fzf" <<'STUBEOF'
#!/usr/bin/env python3
import os, sys
items = sys.stdin.read().splitlines()
log_path = os.environ.get("FZF_CALL_LOG")
if log_path:
    with open(log_path, "a") as log:
        log.write("\n".join(items) + "\n--\n")
if not items:
    sys.exit(1)
print(items[0])
STUBEOF
chmod +x "$STUB/fzf"

# this machine has a real fzf, so force each branch explicitly
contains "picker uses fzf when present"  "alpha" env PATH="$STUB:/usr/bin:/bin" SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" "$SD" i
contains "picker filters"                "alpha" env PATH="$STUB:/usr/bin:/bin" SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" "$SD" i alp

# the list must arrive on stdin as name<TAB>path, one per line
FZF_CALL_LOG=$(mktemp)
env PATH="$STUB:/usr/bin:/bin" FZF_CALL_LOG="$FZF_CALL_LOG" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" "$SD" i >/dev/null
has "fzf got the names"        "alpha" "$(cat "$FZF_CALL_LOG")"
has "fzf got the paths too"    "/tmp"   "$(cat "$FZF_CALL_LOG")"
check "fzf got a tab separator" 0 grep -qP '^alpha\t' "$FZF_CALL_LOG"
check "fzf got one line each"   0 bash -c "[[ \$(grep -cP '\t' '$FZF_CALL_LOG') -eq 2 ]]"
rm -f "$FZF_CALL_LOG"

# a stub that picks something we never offered must not be believed
printf '#!/usr/bin/env bash\necho notanalias\n' > "$STUB/fzf"
check "picker rejects a bogus fzf answer"  1 env PATH="$STUB:/usr/bin:/bin" SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" "$SD" i
contains "picker explains a bogus answer" "isn't one of your aliases" env PATH="$STUB:/usr/bin:/bin" SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" "$SD" i

printf '#!/usr/bin/env bash\nexit 130\n' > "$STUB/fzf"
check "escape from fzf exits 1"         1 env PATH="$STUB:/usr/bin:/bin" SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" "$SD" i

contains "picker falls back to a list"  "pick a number" bash -c \
  "printf '2\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
contains "picker answers with the name" "beta" bash -c \
  "printf '2\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
contains "picker takes a typed name"    "gamma" bash -c \
  "printf 'gamma\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
check "picker rejects out-of-range"     1 bash -c \
  "printf '99\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
contains "picker explains out-of-range" "no entry 99" bash -c \
  "printf '99\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
check "picker gives up on empty input"  1 bash -c \
  "printf '\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
contains "picker says why it's a list" "SIMPLEDIR_NO_FZF" bash -c \
  "printf '\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
mkdir -p "$STUB/bare"
ln -sf "$(command -v python3)" "$STUB/bare/python3"
contains "picker reports fzf missing" "no fzf installed" bash -c \
  "printf '\n' | env PATH='$STUB/bare' SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
check "picker with nothing to pick"    1 env SIMPLEDIR_CONFIG_DIR="$STUB/empty" "$SD" i
rm -rf "$STUB"

# --- sdcfg edit --------------------------------------------------------------
EDITOR=true check "edit with a no-op editor" 0 "$CFG" edit
EDITOR=/nonexistent-binary-xyz check "edit reports bad editor" 1 "$CFG" edit
contains "edit created a config" '"aliases"' cat "$SIMPLEDIR_CONFIG_DIR/config.json"

# a data file must not be executable. write_atomic used fs::perms::owner_all for
# the non-executable case, which includes the x bit, so config.json came out 0744
check "config.json is not executable" 0 bash -c \
  "[[ ! -x '$SIMPLEDIR_CONFIG_DIR/config.json' ]]"
check "config.json is readable by others" 0 bash -c \
  "[[ \$(stat -c '%a' '$SIMPLEDIR_CONFIG_DIR/config.json') == 644 ]]"
"$CFG" add permcheck /tmp >/dev/null
check "the mode survives a rewrite" 0 bash -c \
  "[[ \$(stat -c '%a' '$SIMPLEDIR_CONFIG_DIR/config.json') == 644 ]]"
"$CFG" rm permcheck >/dev/null

# --- config v1 -> v2, `sdcfg migrate` ----------------------------------------
# `migrate` moves the config file. `update` replaces the program. neither calls
# the other, and the names are one character apart, so both get their own tests.
V1=$(mktemp -d)
printf '{"version": 1, "aliases": {"dots": "%s"}}' "$HOME" > "$V1/config.json"
v1() { env SIMPLEDIR_CONFIG_DIR="$V1" "$CFG" "$@"; }

out=$(v1 migrate --dry-run)
has "migrate --dry-run says would"        "would migrate"     "$out"
has "migrate --dry-run shows the version" "version 1 -> 2"    "$out"
check "migrate --dry-run wrote nothing"   0 grep -q '"version": 1' "$V1/config.json"
check "migrate --dry-run made no history" 0 bash -c "! test -e '$V1/history.json'"

out=$(v1 migrate)
has "migrate reports the versions"        "version 1 -> 2"    "$out"
has "migrate says the aliases survived"   "aliases kept"      "$out"
has "migrate names the backup"            "backup:"           "$out"
check "migrate wrote version 2"           0 grep -q '"version": 2' "$V1/config.json"
check "migrate created the history file"  0 test -f "$V1/history.json"
check "migrate kept a backup"             0 bash -c "compgen -G '$V1/config.json.bak.*' >/dev/null"
check "migrate kept every alias"          0 grep -q "\"dots\": \"$HOME\"" "$V1/config.json"
check "the alias still jumps"             0 env SIMPLEDIR_CONFIG_DIR="$V1" "$SD" print dots
contains "migrate says nothing to do next time" "already version 2" "$(v1 migrate)"
check "migrate is idempotent"             0 v1 migrate
check "a second migrate changes nothing"  0 grep -q '"version": 2' "$V1/config.json"

V9=$(mktemp -d)
printf '{"version": 9, "aliases": {}}' > "$V9/config.json"
out=$(env SIMPLEDIR_CONFIG_DIR="$V9" "$CFG" migrate 2>&1)
has "a newer config is refused"      "only knows version" "$out"
has "a newer config names the fix"   "sdcfg update"      "$out"
check "a newer config is not rewritten" 0 grep -q '"version": 9' "$V9/config.json"

# --- frecency: directories you never named ------------------------------------
# the whole point of v6. seed a visit log by jumping, then reach it by a word
# that was never an alias.
FR=$(mktemp -d)
mkdir -p "$FR/wezterm-config" "$FR/dots"
fr() { env SIMPLEDIR_CONFIG_DIR="$FR" "$@" ; }
fr "$CFG" add wz "$FR/wezterm-config" >/dev/null
fr "$SD" print wz >/dev/null            # the jump records it
check "a jump writes the history file"  0 test -f "$FR/history.json"
contains "history records the path" 'wezterm-config' cat "$FR/history.json"
fr "$CFG" rm wz >/dev/null              # and the name goes away
lacks   "the name is out of the listing" "wz" "$(fr "$SD" ls --names)"
out=$(fr "$SD" print wez)
has "frecency finds the unnamed dir"   "$FR/wezterm-config" "$out"
check "the alias is not in the config"  1 grep -q '"wz"' "$FR/config.json"
check "the path is still remembered"    0 grep -q 'wezterm-config' "$FR/history.json"

# a named directory is reachable by name, so frecency needn't offer it
fr "$CFG" add dots "$FR/dots" >/dev/null
fr "$SD" print dots >/dev/null          # a named directory is recorded too
contains "top lists what was visited"   "wezterm-config" "$(fr "$SD" top)"
contains "top marks named paths"        "[named]"         "$(fr "$SD" top)"
lacks   "top leaves unnamed paths bare" "$(printf '%s   [named]' "$FR/wezterm-config")" \
  "$(fr "$SD" top)"
contains "top --json is an array"       '"score"'         "$(fr "$SD" top --json)"
lacks   "top --json is not an object"   '"aliases"'       "$(fr "$SD" top --json)"

# recording can be turned off, two ways
FR2=$(mktemp -d); mkdir -p "$FR2/quiet"
fr2() { env SIMPLEDIR_CONFIG_DIR="$FR2" "$@"; }
fr2 "$CFG" add q "$FR2/quiet" >/dev/null
SIMPLEDIR_NO_HISTORY=1 fr2 "$SD" print q >/dev/null
check "SIMPLEDIR_NO_HISTORY stops the log" 0 bash -c "! test -e '$FR2/history.json'"
rm -f "$FR2/history.json"
python3 - "$FR2/config.json" <<'EOF'
import json, sys
p = sys.argv[1]
c = json.load(open(p))
c["history"] = False
json.dump(c, open(p, "w"))
EOF
fr2 "$SD" print q >/dev/null
check '"history": false stops the log'   0 bash -c "! test -e '$FR2/history.json'"

# --- sdcfg forget -------------------------------------------------------------
contains "forget on an empty log is fine" "already empty" \
  "$(env SIMPLEDIR_CONFIG_DIR=$(mktemp -d) "$CFG" forget)"
contains "forget --all wipes it"          "forgot all"   "$(fr "$CFG" forget --all --yes)"
check   "forget --all emptied the file"   0 bash -c "[[ \$(grep -c wezterm '$FR/history.json' 2>/dev/null || echo 0) -eq 0 ]]"

# a directory that has been deleted is the one worth forgetting
FR3=$(mktemp -d); mkdir -p "$FR3/gone"
fr3() { env SIMPLEDIR_CONFIG_DIR="$FR3" "$@"; }
fr3 "$CFG" add g "$FR3/gone" >/dev/null
fr3 "$SD" print g >/dev/null
rmdir "$FR3/gone"
contains "top marks a deleted dir"  "gone"     "$(fr3 "$SD" top)"
out=$(fr3 "$CFG" forget)
has "forget says how many it dropped" "forgot 1"   "$out"
has "forget names the dead path"      "$FR3/gone"  "$out"
check   "forget dropped it"            1 grep -q "gone" "$FR3/history.json"
# --path drops one entry and leaves the rest, so the log needs two in it
mkdir -p "$FR3/live" "$FR3/other"
fr3 "$CFG" add l "$FR3/live" >/dev/null
fr3 "$SD" print l >/dev/null
fr3 "$CFG" add o "$FR3/other" >/dev/null
fr3 "$SD" print o >/dev/null
contains "forget --path drops one"      "forgot $FR3/live" "$(fr3 "$CFG" forget --path "$FR3/live")"
contains "forget --path keeps the rest" "$FR3/other"      "$(fr3 "$SD" top)"
check   "forget --path kept the entry"  0 grep -q "other" "$FR3/history.json"
contains "forget --path on a stranger is an error" "nothing remembered" \
  "$(fr3 "$CFG" forget --path /nowhere/at/all 2>&1)"
rm -rf "$V1" "$V9" "$FR" "$FR2" "$FR3"

# --- sdcfg update (offline: file:// stubs) -----------------------------------
STUBS=$(mktemp -d)
me=$("$CFG" --version | awk '{print $2}')
# the shape the endpoint actually returns: an array of release objects. these were
# bare objects, which the old hand-rolled scanner happened to accept and a real
# parser rightly refuses.
printf '[{"tag_name":"v99.0.0","published_at":"2026-01-01T00:00:00Z","name":"new"}]\n' \
  > "$STUBS/api_new.json"
printf '[{"tag_name":"v%s","published_at":"2026-01-01T00:00:00Z","name":"same"}]\n' "$me" \
  > "$STUBS/api_same.json"
printf '[{"tag_name":"v0.0.1","published_at":"2026-01-01T00:00:00Z","name":"old"}]\n' \
  > "$STUBS/api_old.json"

export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_new.json"
contains "update sees a newer release" "v99.0.0" "$CFG" update --check
check "update --check exits 1 when newer" 1 "$CFG" update --check

export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_same.json"
contains "update says up to date"  "you're up to date" "$CFG" update --check
check "update --check exits 0 when current" 0 "$CFG" update --check

export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_old.json"
contains "old release isn't an update" "up to date" "$CFG" update --check

export SIMPLEDIR_UPDATE_URL="file://$STUBS/missing.json"
contains "unreachable api explains itself" "couldn't reach GitHub" "$CFG" update --check
check "unreachable api exits 1" 1 "$CFG" update --check

# self-install: the stub asset is the same source compiled with a bumped
# VERSION, since sd is a compiled binary now rather than a script to sed
if command -v g++ >/dev/null 2>&1; then
  g++ -std=c++17 -O1 -o "$STUBS/candidate" "$ROOT/sd.cpp" -DVERSION='"99.0.0"' 2>/dev/null
fi
export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_new.json"
export SIMPLEDIR_UPDATE_ASSET_URL="file://$STUBS/candidate"
export SIMPLEDIR_BIN="$STUBS/installed"
contains "update installs with --yes" "updated to 99.0.0" "$CFG" update --yes
check "installed binary works" 0 "$STUBS/installed" --version
contains "installed binary is the new version" "99.0.0" "$STUBS/installed" --version
contains "installed binary still knows its name" "sd 99.0.0" "$STUBS/installed" --version
check "no staging files left behind" 0 bash -c "! compgen -G '$STUBS/installed.*'"

printf 'not a program\n' > "$STUBS/garbage"
export SIMPLEDIR_UPDATE_ASSET_URL="file://$STUBS/garbage"
export SIMPLEDIR_BIN="$STUBS/untouched"
contains "update rejects junk" "isn't runnable" "$CFG" update --yes
check "junk never got written" 0 bash -c "! test -e '$STUBS/untouched'"

export SIMPLEDIR_UPDATE_ASSET_URL="file://$SD"
export SIMPLEDIR_BIN="$STUBS/samever"
contains "update rejects same-version asset" "same as what you have" "$CFG" update --yes
check "same-version asset wrote nothing" 0 bash -c "! test -e '$STUBS/samever'"

unset SIMPLEDIR_BIN SIMPLEDIR_UPDATE_ASSET_URL SIMPLEDIR_UPDATE_URL
rm -rf "$STUBS"

# --- the real release URL, built the way GitHub serves one --------------------
# Everything above overrides the finished URL, which means none of it saw how
# that URL is constructed — and it was constructed wrong: it asked for an asset
# called `sd`, while every release publishes `sd-linux-x86_64`. So `update` had
# never once succeeded against github. Lay the tree out the way github does and
# point only the *base* at it, which is the same thing SIMPLEDIR_RELEASE_URL does
# for install.sh.
REL=$(mktemp -d)
arch=$(uname -m)
case "$arch" in x86_64|amd64) asset="sd-linux-x86_64" ;; aarch64|arm64) asset="sd-linux-arm64" ;;
  *) asset="sd-linux-$arch" ;; esac
mkdir -p "$REL/latest/download" "$REL/download/v99.0.0"
if command -v g++ >/dev/null 2>&1; then
  g++ -std=c++17 -O1 -o "$REL/latest/download/$asset" "$ROOT/sd.cpp" -DVERSION='"99.0.0"' 2>/dev/null
  g++ -std=c++17 -O1 -o "$REL/download/v99.0.0/$asset" "$ROOT/sd.cpp" -DVERSION='"99.0.0"' 2>/dev/null
fi
contains "the stand-in release runs" "sd 99.0.0" "$REL/latest/download/$asset" --version
printf '[{"tag_name":"v99.0.0","published_at":"2026-01-01T00:00:00Z","name":"stub"}]\n' > "$REL/api_new.json"

rel() { # rel <target-name> <args...>
  local name=$1; shift
  env SIMPLEDIR_RELEASE_URL="file://$REL" SIMPLEDIR_UPDATE_URL="file://$REL/api_new.json" \
      SIMPLEDIR_BIN="$REL/installed-$name" SIMPLEDIR_NO_UPDATE_CHECK=1 "$CFG" "$@"
}

contains "update to latest finds the arch asset" "updated to 99.0.0" rel latest update --yes
contains "update --to finds the tag's asset"     "done"           rel tag update --to v99.0.0
contains "--to accepts a version without the v"   "done"           rel tag2 update --to 99.0.0
check   "--to installed the right version"        0 bash -c "'$REL/installed-tag' --version | grep -q 99.0.0"

# a tag that isn't shaped like a release is a typo, and should say so rather than
# 404: `4.0.04` and `latest-ish` both look plausible to a user
check   "--to rejects a malformed version"  2 rel bad update --to 4.0.04
contains "--to says what a version looks like" "v6.0.0" rel bad update --to nonsense
contains "--to points at releases"             "releases"  rel bad update --to nonsense
check   "a malformed --to wrote nothing"    0 bash -c "! test -e '$REL/installed-bad'"

# a well-shaped tag with no asset for this architecture
check   "--to on a missing tag fails"       1 rel gone update --to v1.2.3
contains "a missing asset names the arch"   "$(basename "$asset")" rel gone update --to v1.2.3
contains "a missing asset suggests releases" "releases"  rel gone update --to v1.2.3
check   "a missing tag wrote nothing"       0 bash -c "! test -e '$REL/installed-gone'"
check   "no staging files left behind"      0 bash -c "! compgen -G '$REL/installed-gone.*'"

# reverting onto the identical version is a no-op and has to say so, rather than
# claiming success while writing the same bytes back over the same file
install -m 755 "$SD" "$REL/installed-same"
cp "$REL/installed-same" "$REL/installed-same.previous"
contains "revert onto the same version is a no-op" "nothing to do" env \
  SIMPLEDIR_BIN="$REL/installed-same" SIMPLEDIR_NO_UPDATE_CHECK=1 "$CFG" revert
contains "revert no-op points at --to" "--to" env \
  SIMPLEDIR_BIN="$REL/installed-same" SIMPLEDIR_NO_UPDATE_CHECK=1 "$CFG" revert
check   "revert no-op left a working binary" 0 "$REL/installed-same" --version

# The asset name has changed twice across releases: sd-linux-<arch> (v6+), sd
# (v5.0.0), simpledir (v3-v4). `--to` has to try all of them, and has to accept a
# legacy build that calls itself `simpledir` rather than `sd` -- it is still
# simpledir, and refusing it looked like a corrupt download.
if command -v g++ >/dev/null 2>&1; then
  mkdir -p "$REL/download/v5.0.0" "$REL/download/v4.0.0"
  cp "$REL/latest/download/$asset" "$REL/download/v5.0.0/sd"
  # a v4-shaped asset: same program, older name, older version
  cat > "$REL/legacy.cpp" <<'LEOF'
// a stand-in for a pre-v5.0.0 asset: same idea, older command name
#include <cstdio>
#include <cstring>
int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "--version") == 0) { printf("simpledir 4.0.0 - the next zoxide\n"); return 0; }
  return 0;
}
LEOF
  g++ -std=c++17 -O1 -o "$REL/download/v4.0.0/simpledir" "$REL/legacy.cpp" 2>/dev/null
fi
contains "--to finds the v5 'sd' asset"   "done" rel v5 update --to v5.0.0
contains "--to finds the v4 'simpledir' asset" "done" rel v4 update --to v4.0.0
contains "--to accepts the legacy name"  "calls itself" rel v4b update --to v4.0.0
contains "--to warns the legacy build can't return" "cannot bring you back" \
  rel v4c update --to v4.0.0
# The one-way door hands you a way back that needs nothing: the binary it replaced,
# sitting right there. `.previous` only exists when there *was* something to
# replace, which is why the message must not promise it unconditionally.
install -m 755 "$SD" "$REL/installed-back"
out=$(env SIMPLEDIR_RELEASE_URL="file://$REL" SIMPLEDIR_BIN="$REL/installed-back" \
  SIMPLEDIR_NO_UPDATE_CHECK=1 "$CFG" update --to v4.0.0 2>&1)
has "the one-way door gives a local way back" "cp "        "$out"
has "the way back names the file it uses"    ".previous"  "$out"
lacks "the way back doesn't tell you to curl" "install.sh" "$out"
# the state the message describes: the target is now the old build, .previous is
# the good one, and the pasted command brings it back
check "the downgrade did take effect"   0 bash -c \
  "'$REL/installed-back' --version | grep -q '^simpledir '"
check "the downgrade left the good copy behind" 0 bash -c \
  "'$REL/installed-back.previous' --version | grep -q '^sd '"
check "the promised cp really restores" 0 bash -c "
  cp '$REL/installed-back.previous' '$REL/installed-back' &&
  '$REL/installed-back' --version | grep -q '^sd '"

# with no prior install there is nothing to go back to, so it must not promise a file
out=$(env SIMPLEDIR_RELEASE_URL="file://$REL" SIMPLEDIR_BIN="$REL/installed-fresh" \
  SIMPLEDIR_NO_UPDATE_CHECK=1 "$CFG" update --to v4.0.0 2>&1)
has "a first install has no previous to offer" "re-run the installer" "$out"
lacks "a first install promises no file"       "cp "                "$out"
check "--to installed the legacy build" 0 bash -c \
  "'$REL/installed-v4' --version | grep -q 'simpledir 4.0.0'"
rm -f "$REL/legacy.cpp"

# v1.0.0 and v2.0.0 were never given a release asset at all, but the program is in
# the tag as one executable file. Fall back to the source of the tag so `--to`
# keeps its promise for every published version, and say where it came from.
mkdir -p "$REL/raw/noxthedevwindev-greatest/simpledir/v2.0.0"
if command -v g++ >/dev/null 2>&1; then
  cat > "$REL/legacy2.cpp" <<'LEOF'
#include <cstdio>
#include <cstring>
int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "--version") == 0) { printf("simpledir 2.0.0 - the next zoxide\n"); return 0; }
  return 0;
}
LEOF
  g++ -std=c++17 -O1 -o "$REL/raw/noxthedevwindev-greatest/simpledir/v2.0.0/simpledir" \
      "$REL/legacy2.cpp" 2>/dev/null
fi
contains "--to falls back to the source of the tag" "source file from" \
  env SIMPLEDIR_RELEASE_URL="file://$REL" SIMPLEDIR_SOURCE_URL="file://$REL/raw" \
      SIMPLEDIR_BIN="$REL/installed-src" SIMPLEDIR_NO_UPDATE_CHECK=1 \
      "$CFG" update --to v2.0.0
contains "--to installed the source fallback" "done" \
  env SIMPLEDIR_RELEASE_URL="file://$REL" SIMPLEDIR_SOURCE_URL="file://$REL/raw" \
      SIMPLEDIR_BIN="$REL/installed-src" SIMPLEDIR_NO_UPDATE_CHECK=1 \
      "$CFG" update --to v2.0.0

# a tag whose file disagrees with the tag name is reported, not papered over. the
# real v1.0.0 tag contains a program whose --version says 2.0.0, and silently
# believing either number is how you end up not trusting the tool.
mkdir -p "$REL/raw/noxthedevwindev-greatest/simpledir/v1.5.0"
if command -v g++ >/dev/null 2>&1; then
  sed 's/2\.0\.0/1.4.0/' "$REL/legacy2.cpp" > "$REL/legacy3.cpp"
  g++ -std=c++17 -O1 -o "$REL/raw/noxthedevwindev-greatest/simpledir/v1.5.0/simpledir" \
      "$REL/legacy3.cpp" 2>/dev/null
fi
out=$(env SIMPLEDIR_RELEASE_URL="file://$REL" SIMPLEDIR_SOURCE_URL="file://$REL/raw" \
      SIMPLEDIR_BIN="$REL/installed-mismatch" SIMPLEDIR_NO_UPDATE_CHECK=1 \
      "$CFG" update --to v1.5.0 2>&1)
has "a version mismatch is reported"     "you asked for v1.5.0"  "$out"
has "a version mismatch names both"      "says 1.4.0"           "$out"
has "a version mismatch explains why"    "version string"       "$out"
check "the mismatched build was installed anyway" 0 bash -c \
  "'$REL/installed-mismatch' --version | grep -q 1.4.0"
rm -f "$REL/legacy2.cpp" "$REL/legacy3.cpp"

# The url the tool actually asks for. Two bugs hid here and neither was visible
# to a test: the release list url had no `/releases` on the end of it, so every
# check fetched the *repository* object and found no tag_name in it; and the
# hand-rolled json scan it used to read the answer cut every object short at the
# `{?name,label}` that sits inside the upload_url string. Both reported the same
# thing — "couldn't reach GitHub" — while reaching github perfectly well.
# A spy on curl is the only thing that sees either.
SPY=$(mktemp -d)
cat > "$SPY/curl" <<'SPYEOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$SPY_LOG"
cat "$SPY_BODY"
SPYEOF
chmod +x "$SPY/curl"
SPY_LOG=$(mktemp)
SPY_BODY=$(mktemp)
# the second entry carries `{?name,label}` inside a string value, exactly as
# github's upload_url does. that is what truncated the old scanner.
cat > "$SPY_BODY" <<'BODYEOF'
[
  { "tag_name": "v9.9.9", "published_at": "2026-02-02T00:00:00Z", "name": "spy" },
  {
    "upload_url": "https://uploads.github.com/repos/o/r/releases/1/assets{?name,label}",
    "tag_name": "v9.9.8",
    "published_at": "2026-01-01T00:00:00Z",
    "name": "braces inside a string"
  }
]
BODYEOF

spy() { env PATH="$SPY:$PATH" SPY_LOG="$SPY_LOG" SPY_BODY="$SPY_BODY" "$CFG" "$@"; }
contains "releases parses a real api body" "v9.9.9" spy releases
contains "releases survives braces inside strings" "v9.9.8" spy releases
contains "releases shows dates"             "2026-02-02" spy releases
: > "$SPY_LOG"
spy update --check >/dev/null 2>&1
check "the release list url ends in /releases" 0 grep -q '/releases' "$SPY_LOG"
lacks   "the release list url is not the repo object" 'simpledir$' "$SPY_LOG"
check   "the update check asked exactly once"    0 bash -c "[[ \$(grep -c 'releases' '$SPY_LOG') -eq 1 ]]"

# a body that is valid json but not a list of releases must not be reported as
# "couldn't reach github"
printf '{"id":1,"full_name":"noxthedevwindev-greatest/simpledir"}\n' > "$SPY_BODY"
lacks   "a repo object is not mistaken for releases" "published releases" spy releases
contains "a repo object is reported as unusable" "couldn't reach GitHub" spy releases
printf 'not json at all\n' > "$SPY_BODY"
check   "garbage from the api is handled" 1 spy releases
rm -rf "$SPY" "$SPY_LOG" "$SPY_BODY"

# --to must not need the release list, so an unreachable api can't block it
printf '[]\n' > "$REL/api.json"
contains "--to works with no api reachable" "done" env \
  SIMPLEDIR_RELEASE_URL="file://$REL" SIMPLEDIR_UPDATE_URL="file://$REL/api.json" \
  SIMPLEDIR_BIN="$REL/installed-noapi" SIMPLEDIR_NO_UPDATE_CHECK=1 \
  "$CFG" update --to v99.0.0
check   "the api being empty is still an error for a plain update" 1 env \
  SIMPLEDIR_UPDATE_URL="file://$REL/api.json" SIMPLEDIR_BIN="$REL/installed-x" \
  SIMPLEDIR_NO_UPDATE_CHECK=1 "$CFG" update
contains "an unreachable api suggests --to" "--to" env \
  SIMPLEDIR_UPDATE_URL="file:///nonexistent/api.json" SIMPLEDIR_BIN="$REL/installed-x" \
  SIMPLEDIR_NO_UPDATE_CHECK=1 "$CFG" update 2>&1
contains "an unreachable api explains the limit" "60 requests" env \
  SIMPLEDIR_UPDATE_URL="file:///nonexistent/api.json" SIMPLEDIR_BIN="$REL/installed-x" \
  SIMPLEDIR_NO_UPDATE_CHECK=1 "$CFG" update 2>&1
rm -rf "$REL"

# the nudge: silent off-terminal, silent on the jump path, one line on a tty
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$CFG" add home "$HOME" >/dev/null
mkdir -p "$SIMPLEDIR_CONFIG_DIR"
printf '{"tag_name": "v99.0.0"}' > "$SIMPLEDIR_CONFIG_DIR/api_new.json"
printf '{"checked": 9999999999, "latest": "v99.0.0"}' > "$SIMPLEDIR_CONFIG_DIR/update-check.json"
check "print path stays silent"  0 env SIMPLEDIR_UPDATE_URL="file://$SIMPLEDIR_CONFIG_DIR/api_new.json" "$SD" print home
lacks   "ls stays silent off a terminal" "is out" "$SD" ls
check "jump never nudges even on a tty" 0 on_tty "SIMPLEDIR_UPDATE_URL=file://$SIMPLEDIR_CONFIG_DIR/api_new.json '$SD' print home"
contains "ls nudges on a tty" "v99.0.0 is out" on_tty "SIMPLEDIR_UPDATE_URL=file://$SIMPLEDIR_CONFIG_DIR/api_new.json '$SD' ls"
lacks   "opt-out silences the nudge" "is out" on_tty "SIMPLEDIR_NO_UPDATE_CHECK=1 '$SD' ls"

# --- sdcfg uninstall ---------------------------------------------------------
UNROOT=$(mktemp -d)
fresh() { # fresh <name> -> prints the scenario dir
  local dir="$UNROOT/$1"
  mkdir -p "$dir/bin" "$dir/cfg"
  install -m 755 "$SD" "$dir/bin/sd"
  ln -sf sd "$dir/bin/sdcfg"
  env SIMPLEDIR_CONFIG_DIR="$dir/cfg" "$dir/bin/sdcfg" add mine "$HOME" >/dev/null
  printf '# my rc\n\n# >>> simpledir >>>\nwrapper junk\n# <<< simpledir <<<\n\ntail\n' > "$dir/rc"
  printf '%s' "$dir"
}
un() { # un <dir> [args...]
  local dir=$1; shift
  env SIMPLEDIR_RC="$dir/rc" SIMPLEDIR_CONFIG_DIR="$dir/cfg" \
      SIMPLEDIR_BIN="$dir/bin/sd" SIMPLEDIR_NO_UPDATE_CHECK=1 \
      "$dir/bin/sdcfg" "$@"
}

d=$(fresh keep)
out=$(un "$d" uninstall --yes)
has "uninstall says aliases survive" "these stay"   "$out"
has "uninstall names the purge flag"  "--purge"     "$out"
has "uninstall reports the binary"    "removed"     "$out"
has "uninstall reports the rc"        "cleaned"     "$out"
has "uninstall reminds about reload"  "exec bash"   "$out"
check "uninstall removed the binary"  0 bash -c "! test -e '$d/bin/sd'"
check "uninstall cleaned the block"   0 bash -c "! grep -q '>>> simpledir >>>' '$d/rc'"
contains "rc keeps what came before"   "my rc" cat "$d/rc"
contains "rc keeps what came after"    "tail"  cat "$d/rc"
lacks   "rc block is gone"             "wrapper junk" cat "$d/rc"
check "uninstall kept a backup"        0 bash -c "compgen -G '$d/rc.bak.*' >/dev/null"
check "uninstall kept the config"      0 bash -c "test -f '$d/cfg/config.json'"
check "no staging files left behind"   0 bash -c "! compgen -G '$d/rc.new.*'"

d=$(fresh purge)
contains "purge says it deletes the config" "deleted" un "$d" uninstall --yes --purge
check "purge removed the config dir"   0 bash -c "! test -e '$d/cfg'"
check "purge removed the binary"       0 bash -c "! test -e '$d/bin/sd'"
check "purge cleaned the rc"           0 bash -c "! grep -q '>>> simpledir >>>' '$d/rc'"

d=$(fresh refuse)
check "uninstall refuses without a tty" 1 un "$d" uninstall
check "refusal changed nothing"         0 bash -c "test -x '$d/bin/sd'"
check "refusal left the rc"             0 bash -c "grep -q '>>> simpledir >>>' '$d/rc'"

d=$(fresh declined)
# the answer has to come through the pty, or stdin isn't a terminal
out=$(printf 'n\n' | on_tty "env SIMPLEDIR_RC=$d/rc SIMPLEDIR_CONFIG_DIR=$d/cfg SIMPLEDIR_BIN=$d/bin/sd SIMPLEDIR_NO_UPDATE_CHECK=1 $d/bin/sdcfg uninstall")
has "answering no changes nothing" "nothing changed" "$out"
check "binary survived the no"         0 bash -c "test -x '$d/bin/sd'"
check "rc survived the no"             0 bash -c "grep -q '>>> simpledir >>>' '$d/rc'"

d=$(fresh norc)
rm -f "$d/rc"
contains "uninstall tolerates no rc"   "no wrapper block" un "$d" uninstall --yes
check "binary still removed without an rc" 0 bash -c "! test -e '$d/bin/sd'"

d=$(fresh elsewhere)
contains "uninstall flags a different target" "not the copy you're running" env \
  SIMPLEDIR_RC="$d/rc" SIMPLEDIR_CONFIG_DIR="$d/cfg" \
  SIMPLEDIR_BIN="$UNROOT/elsewhere/sd" SIMPLEDIR_NO_UPDATE_CHECK=1 \
  "$CFG" uninstall --yes
check "the running copy was left alone" 0 bash -c "test -x '$SD'"

# SIMPLEDIR_BIN is exclusive too: a scoped uninstall must not reach for
# $HOME/.local/bin/sdcfg. it used to, because that path was hardcoded instead of
# derived from the target being removed, and a test run quietly deleted the
# developer's real symlink — twice, before anyone worked out why it kept
# disappearing between runs.
#
# HOME is faked rather than using the real ~/.local/bin: the bug is a hardcoded
# $HOME path, so pointing HOME at a throwaway home reproduces it exactly without
# putting the developer's own install at risk. An earlier version of this check
# used the real directory behind an `if [ -e ]` guard, and the guard meant the
# assertion never ran — the first uninstall had already deleted the file it was
# supposed to be protecting.
CANARY=$(mktemp -d)
mkdir -p "$CANARY/.local/bin" "$CANARY/cfg" "$CANARY/target"
printf 'not yours\n' > "$CANARY/.local/bin/sdcfg"
printf '# rc\n\n# >>> simpledir >>>\nx\n# <<< simpledir <<<\n' > "$CANARY/rc"
install -m 755 "$SD" "$CANARY/target/sd"
ln -sf sd "$CANARY/target/sdcfg"
check "the canary exists to begin with"   0 test -f "$CANARY/.local/bin/sdcfg"
env HOME="$CANARY" SIMPLEDIR_RC="$CANARY/rc" SIMPLEDIR_CONFIG_DIR="$CANARY/cfg" \
  SIMPLEDIR_BIN="$CANARY/target/sd" SIMPLEDIR_NO_UPDATE_CHECK=1 \
  "$CANARY/target/sdcfg" uninstall --yes >/dev/null 2>&1
check "uninstall removed the named target" 0 bash -c "! test -e '$CANARY/target/sd'"
check "uninstall left \$HOME/.local/bin/sdcfg alone" 0 test -f "$CANARY/.local/bin/sdcfg"
rm -rf "$CANARY"

# SIMPLEDIR_RC is exclusive: uninstalling against it must leave the real
# ~/.bashrc byte for byte alone. a test run once removed the wrapper from a
# developer's real rc because of exactly this.
rc_before=$(md5sum "$HOME/.bashrc" 2>/dev/null || echo missing)
d=$(fresh exclusiverc)
env SIMPLEDIR_RC="$d/rc" SIMPLEDIR_CONFIG_DIR="$d/cfg" SIMPLEDIR_BIN="$d/bin/sd" \
    SIMPLEDIR_NO_UPDATE_CHECK=1 "$d/bin/sdcfg" uninstall --yes >/dev/null 2>&1
check "SIMPLEDIR_RC is exclusive"      0 bash -c \
  "[[ \"\$(md5sum '$HOME/.bashrc' 2>/dev/null || echo missing)\" == '$rc_before' ]]"
lacks   "the named rc did get cleaned" ">>> simpledir >>>" cat "$d/rc"
rm -rf "$UNROOT"

# --- sdcfg doctor ------------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
check "doctor fails with no config"  1 "$CFG" doctor
contains "doctor says so"         "no config yet" "$CFG" doctor
"$CFG" add alive /tmp >/dev/null
mkdir -p "$SIMPLEDIR_CONFIG_DIR/dead"
"$CFG" add deadalias "$SIMPLEDIR_CONFIG_DIR/dead" >/dev/null
rmdir "$SIMPLEDIR_CONFIG_DIR/dead"
contains "doctor finds dead aliases" "is gone" "$CFG" doctor
contains "doctor suggests a fix"     "sdcfg add --force deadalias" "$CFG" doctor
"$CFG" add 'weird name' /tmp >/dev/null
contains "doctor flags odd names"    "whitespace" "$CFG" doctor

# --- completions -------------------------------------------------------------
contains "bash completions"     "complete" "$CFG" completions bash
contains "bash comp has sd"     " sd"      "$CFG" completions bash
contains "bash comp has sdcfg"  " sdcfg"   "$CFG" completions bash
contains "bash comp verbs"      "uninstall" "$CFG" completions bash
contains "zsh completions"      "#compdef" "$CFG" completions zsh
check "completions reject junk" 2 "$CFG" completions fish
check "generated bash comp is valid bash" 0 bash -n <("$CFG" completions bash)

# --- install.sh --------------------------------------------------------------
# offline: the release fetcher is pointed at a local stand-in for a published
# binary, and the source fetcher at the repo, so neither path touches the network
check "install.sh parses"        0 bash -n "$ROOT/install.sh"
contains "install.sh --help"     "curl -fsSL" bash "$ROOT/install.sh" --help
check "install.sh rejects junk"  1 bash "$ROOT/install.sh" --nope
contains "install.sh validates pm" "must be yay" env SIMPLEDIR_PM=brew bash "$ROOT/install.sh"
check "install.sh bad pm exits 1" 1 env SIMPLEDIR_PM=brew bash "$ROOT/install.sh"

FAKEHOME=$(mktemp -d)

# a stand-in for a published release, so the happy path (download a prebuilt
# binary) is exercised without the network
PUBLISH_DIR=$(mktemp -d)
arch=$(uname -m)
case "$arch" in x86_64|amd64) arch=x86_64 ;; aarch64|arm64) arch=arm64 ;; esac
mkdir -p "$PUBLISH_DIR/releases/latest/download"
install -m 755 "$SD" "$PUBLISH_DIR/releases/latest/download/sd-linux-$arch"
contains "the stand-in release asset works" "sd " \
  "$PUBLISH_DIR/releases/latest/download/sd-linux-$arch" --version

helper="$FAKEHOME/install-here"
cat > "$helper" <<EOF
#!/usr/bin/env bash
# install into a throwaway HOME, with the network stubbed out.
# \$1 is the fake home, the rest are install.sh's own arguments.
home=\$1; shift
mkdir -p "\$home"
env HOME="\$home" SHELL=/bin/bash PATH="/usr/bin:/bin" \\
    SIMPLEDIR_BASE_URL="file://$PUBLISH_DIR" \\
    SIMPLEDIR_SOURCE_URL="file://$ROOT" \\
    bash "$ROOT/install.sh" "\$@"
EOF
chmod +x "$helper"

# the installer only claims Linux, and says so rather than pretending
stub_uname="uname() { case \"\$1\" in -s) echo Darwin;; *) command uname \"\$1\";; esac; }; export -f uname;"
check "install.sh refuses non-linux" 1 env SIMPLEDIR_NO_RC=1 bash -c \
  "$stub_uname '$helper' '$FAKEHOME/mac'"
contains "non-linux message names the os" "Darwin" env SIMPLEDIR_NO_RC=1 bash -c \
  "$stub_uname '$helper' '$FAKEHOME/mac2'"

check "install.sh installs sd"       0 env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/plain"
check "sd is executable"              0 "$FAKEHOME/plain/.local/bin/sd" --version
check "sdcfg exists and runs"         0 "$FAKEHOME/plain/.local/bin/sdcfg" --version
check "sdcfg is a symlink to sd"      0 bash -c "[ \"\$(readlink '$FAKEHOME/plain/.local/bin/sdcfg')\" = sd ]"
contains "installer reports the version" "simpledir" env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/again"
check "installed copy matches the repo" 0 bash -c "diff -q '$ROOT/sd' '$FAKEHOME/plain/.local/bin/sd'"
check "no staging files left behind"    0 bash -c "! compgen -G '$FAKEHOME/plain/.local/bin/.sd.*'"

check "opt-out leaves the rc alone"     0 env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/norc"
check "no rc file was created"          0 bash -c "! test -e '$FAKEHOME/norc/.bashrc'"

check "install.sh wires the rc"         0 "$helper" "$FAKEHOME/withrc"
contains "rc has the begin marker" "# >>> simpledir >>>" cat "$FAKEHOME/withrc/.bashrc"
check "rc marker appears once" 0 bash -c \
  "[ \$(grep -c '>>> simpledir >>>' '$FAKEHOME/withrc/.bashrc') -eq 1 ]"
check "rc block defines sd" 0 bash -c "grep -q '^sd()' '$FAKEHOME/withrc/.bashrc'"
check "rc block mentions sdcfg" 0 bash -c "grep -q 'never moves you' '$FAKEHOME/withrc/.bashrc'"
"$CFG" add home "$HOME" >/dev/null
check "wired shell actually jumps" 0 env -i HOME="$FAKEHOME/withrc" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" PATH="/usr/bin:/bin" TERM=dumb \
  bash --noprofile --norc -c "
  source '$FAKEHOME/withrc/.bashrc'
  cd /tmp; sd home; [[ \$PWD == '$HOME' ]]"
# the fresh install must be the one that answers, not some other sd on the
# runner's PATH. `command -v` would answer "sd" because the rc defines a
# function of that name; `type -P` asks for the path instead.
check "wired shell uses the new install" 0 env -i HOME="$FAKEHOME/withrc" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" PATH="/usr/bin:/bin" TERM=dumb \
  bash --noprofile --norc -c "
  source '$FAKEHOME/withrc/.bashrc'
  [[ \$(type -P sd) == '$FAKEHOME/withrc/.local/bin/sd' ]]"
check "the installed copy reports its version" 0 env -i HOME="$FAKEHOME/withrc" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" PATH="/usr/bin:/bin" TERM=dumb \
  bash --noprofile --norc -c "
  source '$FAKEHOME/withrc/.bashrc'
  sd --version | grep -q '^sd '"

check "re-running refreshes"            0 "$helper" "$FAKEHOME/withrc"
check "still one marker after re-run" 0 bash -c \
  "[ \$(grep -c '>>> simpledir >>>' '$FAKEHOME/withrc/.bashrc') -eq 1 ]"
check "refresh kept a backup" 0 bash -c "compgen -G '$FAKEHOME/withrc/.bashrc.bak.*' >/dev/null"

check "uninstall works"        0 env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/uninst"
check "uninstall removes both names" 0 "$helper" "$FAKEHOME/uninst" --uninstall
check "uninstall removed sd"     0 bash -c "! test -e '$FAKEHOME/uninst/.local/bin/sd'"
check "uninstall removed sdcfg"  0 bash -c "! test -e '$FAKEHOME/uninst/.local/bin/sdcfg'"
check "uninstall removes the rc block" 0 "$helper" "$FAKEHOME/withrc" --uninstall
check "rc block is gone" 0 bash -c "! grep -q '>>> simpledir >>>' '$FAKEHOME/withrc/.bashrc'"
check "uninstall kept a backup" 0 bash -c "compgen -G '$FAKEHOME/withrc/.bashrc.bak.*' >/dev/null"

# a bad download must not be chmod +x'd into place, and when there is no usable
# asset and no source either, it must say so rather than install nothing quietly
JUNKY=$(mktemp -d)
mkdir -p "$JUNKY/releases/latest/download"
printf '<html>404</html>\n' > "$JUNKY/releases/latest/download/sd-linux-$arch"
mkdir -p "$FAKEHOME/bad"
check "installer refuses a bad download" 1 env HOME="$FAKEHOME/bad" SHELL=/bin/bash \
  PATH="/usr/bin:/bin" SIMPLEDIR_NO_RC=1 \
  SIMPLEDIR_BASE_URL="file://$JUNKY" SIMPLEDIR_SOURCE_URL="file:///nonexistent" \
  bash "$ROOT/install.sh"
check "nothing installed from a bad download" 0 bash -c "! test -e '$FAKEHOME/bad/.local/bin/sd'"
rm -rf "$JUNKY"
rm -rf "$FAKEHOME"

# --- the shell wrapper -------------------------------------------------------
# written to a file so we don't fight three layers of quoting
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tree/src"
"$CFG" add home "$HOME" >/dev/null
"$CFG" add tree "$SIMPLEDIR_CONFIG_DIR/tree" >/dev/null

wrapper=$(mktemp)
cat > "$wrapper" <<EOF
export SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR'
export PATH="$ROOT:\$PATH"
eval "\$($CFG init)"
EOF

check "sd jumps in real bash"  0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd home; [[ \$PWD == '$HOME' ]]"
check "sd - returns to OLDPWD" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd home; cd /tmp; sd -; [[ \$PWD == '$HOME' ]]"
check "sd no args = HOME"      0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd; [[ \$PWD == '$HOME' ]]"
check "sd miss stays put"      1 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd bogus"
check "sd jumps into a suffix" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd tree/src; [[ \$PWD == '$SIMPLEDIR_CONFIG_DIR/tree/src' ]]"
check "sd takes a raw path"    0 bash --noprofile --norc -c "
  source '$wrapper'; cd /; sd '$SIMPLEDIR_CONFIG_DIR/tree'; [[ \$PWD == '$SIMPLEDIR_CONFIG_DIR/tree' ]]"

# the read-only verbs must reach the binary and NOT change directory
check "sd ls does not cd"      0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd ls >/dev/null; [[ \$PWD == /tmp ]]"
check "sd print does not cd"   0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd print home >/dev/null; [[ \$PWD == /tmp ]]"
check "sd i does not cd"       0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; printf '1\n' | SIMPLEDIR_NO_FZF=1 sd i >/dev/null; [[ \$PWD == /tmp ]]"
check "sd suggest does not cd" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd suggest >/dev/null 2>&1; [[ \$PWD == /tmp ]]"

# the other way people install this: `eval $(sdcfg init)`, pasted by hand. word
# splitting folds the newlines into one line, so the wrapper must not depend on
# them: every statement ends in `;` and nothing before the last construct is a
# comment. both it and the quoted form have to define a working `sd`.
check "quoted eval of init defines sd" 0 bash --noprofile --norc -c "
  eval \"\$($CFG init)\"; [[ \$(type -t sd) == function ]]"
check "quoted eval of init jumps" 0 bash --noprofile --norc -c "
  eval \"\$($CFG init)\"; cd /tmp; sd home; [[ \$PWD == '$HOME' ]]"
check "unquoted eval of init defines sd" 0 bash --noprofile --norc -c "
  eval \$($CFG init); [[ \$(type -t sd) == function ]]"
check "unquoted eval of init jumps" 0 bash --noprofile --norc -c "
  eval \$($CFG init); cd /tmp; sd home; [[ \$PWD == '$HOME' ]]"
check "eval of init leaves sdcfg alone" 0 bash --noprofile --norc -c "
  eval \"\$($CFG init)\"; [[ \$(type -t sdcfg) != function ]]"
# a sourced-from-file install must still work too
check "sourced init defines sd" 0 bash --noprofile --norc -c "
  source '$wrapper'; [[ \$(type -t sd) == function ]]"

# sdcfg needs no wrapper and must never move you
check "sdcfg runs bare"        0 bash --noprofile --norc -c "
  source '$wrapper'; type -t sdcfg >/dev/null || command sdcfg --version"
check "sdcfg does not cd"      0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; command sdcfg add later /tmp >/dev/null; [[ \$PWD == /tmp ]]"
check "sdcfg rm does not cd"   0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; command sdcfg rm later >/dev/null; [[ \$PWD == /tmp ]]"

check "wrapper is a function"  0 bash --noprofile --norc -c "
  source '$wrapper'; [[ \$(type -t sd) == function ]]"
check "sdcfg is not shadowed"  0 bash --noprofile --norc -c "
  source '$wrapper'; [[ \$(type -t sdcfg) != function ]]"

# the fresh install must be the one that answers, not some other sd on PATH.
# `command -v` would answer "sd" because the rc defines a function of that name;
# `type -P` asks for the path instead.
check "wired shell uses the new install" 0 env -i HOME="$FAKEHOME/withrc" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" PATH="/usr/bin:/bin" TERM=dumb \
  bash --noprofile --norc -c "
  source '$wrapper'; [[ \$(type -P sd) == '$ROOT/sd' ]]"
rm -f "$wrapper"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[[ $fail -eq 0 ]]