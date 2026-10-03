#!/usr/bin/env bash
# simpledir test suite. isolated config + a real bash that sources the wrapper.
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SD="$ROOT/simpledir"
export SIMPLEDIR_CONFIG_DIR=$(mktemp -d)
trap 'rm -rf "$SIMPLEDIR_CONFIG_DIR"' EXIT

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

# --- add --------------------------------------------------------------------
check "add binds cwd"          0 "$SD" add here
check "add with explicit path" 0 "$SD" add home "$HOME"
check "add tilde path"         0 "$SD" add dotdir "$HOME/.config"
check "add rejects bad path"   1 "$SD" add nope /does/not/exist
check "add no clobber"         1 "$SD" add here
check "add --force"            0 "$SD" add --force here "$HOME"

# --- ls ---------------------------------------------------------------------
contains "ls lists alias"      "here"  "$SD" ls
contains "ls -l absolute"      "$HOME" "$SD" ls -l
while read -r name; do "$SD" rm "$name" >/dev/null; done < <("$SD" ls | awk '{print $1}')
check "ls empty exits 1"       1 "$SD" ls
# back to a populated config for the jump tests
"$SD" add home "$HOME" >/dev/null
"$SD" add dotdir "$HOME/.config" >/dev/null

# --- jump -------------------------------------------------------------------
check "jump resolves"          0 "$SD" jump home
check "bare form jumps"        0 "$SD" dotdir
check "jump unknown exits 1"   1 "$SD" jump zzzz
contains "jump echoes path"    "$HOME" "$SD" jump home
contains "suggests near miss"  "did you mean" "$SD" jump hoem
contains "suggests substring"  "did you mean" "$SD" jump dot
contains "no args = help"      "usage:" "$SD"

# --- rm ---------------------------------------------------------------------
"$SD" add gone >/dev/null
check "rm removes"             0 "$SD" rm gone
contains "rm gone says gone"   "gone" "$SD" rm gone

# --- missing target ---------------------------------------------------------
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tmpdir"
"$SD" add vanish "$SIMPLEDIR_CONFIG_DIR/tmpdir" >/dev/null
rmdir "$SIMPLEDIR_CONFIG_DIR/tmpdir"
check "jump refuses missing"   1 "$SD" jump vanish
contains "ls tags missing"      "[missing]" "$SD" ls

# --- config integrity -------------------------------------------------------
contains "config is json" '"aliases"' cat "$SIMPLEDIR_CONFIG_DIR/config.json"
check "no temp files left"     0 bash -c "! compgen -G '$SIMPLEDIR_CONFIG_DIR/.config.*'"
printf 'not json{' > "$SIMPLEDIR_CONFIG_DIR/config.json"
contains "corrupt config warns" "not valid JSON" "$SD" ls

# --- v2: path suffixes --------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tree/src/deep"
"$SD" add tree "$SIMPLEDIR_CONFIG_DIR/tree" >/dev/null
contains "suffix resolves"     "/tree/src"          "$SD" jump tree/src
contains "deep suffix"         "/tree/src/deep"     "$SD" jump tree/src/deep
contains "suffix normalizes .." "/tree/src"         "$SD" jump tree/src/../src
contains "trailing slash ok"   "/tree"              "$SD" jump tree/
check "suffix in bare form"    0 "$SD" tree/src
check "missing suffix fails"   1 "$SD" jump tree/nope
contains "missing suffix names it" "tree/nope"       "$SD" jump tree/nope
check "absolute path not an alias" 1 "$SD" jump /tmp
contains "absolute path says so" "isn't an alias"    "$SD" jump /tmp

# --- v2: rename ---------------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$SD" add dots "$HOME" >/dev/null
check "rename works"           0 "$SD" rename dots dotfiles
contains "rename keeps path"   "$HOME" "$SD" jump dotfiles
check "old name is gone"       1 "$SD" jump dots
"$SD" add other /tmp >/dev/null
check "rename no clobber"      1 "$SD" rename dotfiles other
check "rename --force"         0 "$SD" rename --force dotfiles other
check "rename unknown fails"   1 "$SD" rename nope whatever

# --- v2: add with fewer arguments --------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/dotfiles"
contains "add derives name"    "dotfiles ->" bash -c "cd '$SIMPLEDIR_CONFIG_DIR/dotfiles' && '$SD' add --force"
check "add no args works"      0 bash -c "cd '$SIMPLEDIR_CONFIG_DIR/dotfiles' && '$SD' add --force"
check "add on / is nameable"   0 "$SD" add rootdir /
contains "add name only"       "mydir ->" bash -c "cd '$SIMPLEDIR_CONFIG_DIR' && '$SD' add mydir"

# --- v2: ls flags and filter --------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$SD" add alpha "$HOME" >/dev/null
"$SD" add beta /tmp >/dev/null
contains "ls filters by name"  "alpha"  "$SD" ls alp
contains "ls filters by path"  "beta"   "$SD" ls tmp
check "ls filter miss exits 1" 1 "$SD" ls zzz
contains "ls filter says so"   "nothing matches" "$SD" ls zzz
contains "ls --names"          "alpha"  "$SD" ls --names
check "ls --names is one per line" 0 bash -c "'$SD' ls --names | grep -qx alpha"
contains "ls --json has aliases" '"aliases"' "$SD" ls --json
contains "ls --json has version" '"version"' "$SD" ls --json
check "ls --json is valid json" 0 bash -c "'$SD' ls --json | python3 -c 'import json,sys; json.load(sys.stdin)'"

# --- v2: completions ----------------------------------------------------------
contains "bash completions"    "complete" "$SD" completions bash
contains "bash comp has sd"    " sd"      "$SD" completions bash
contains "zsh completions"     "#compdef" "$SD" completions zsh
check "completions reject junk" 2 "$SD" completions fish
check "generated bash comp is valid bash" 0 bash -n <("$SD" completions bash)

# --- v2: edit -----------------------------------------------------------------
EDITOR=true check "edit with a no-op editor" 0 "$SD" edit
EDITOR=/nonexistent-binary-xyz check "edit reports bad editor" 1 "$SD" edit
contains "edit created a config" '"aliases"' cat "$SIMPLEDIR_CONFIG_DIR/config.json"

# --- shell wrapper ----------------------------------------------------------
# written to a file so we don't fight three layers of quoting
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tree/src"
"$SD" add home "$HOME" >/dev/null
"$SD" add tree "$SIMPLEDIR_CONFIG_DIR/tree" >/dev/null

wrapper=$(mktemp)
cat > "$wrapper" <<EOF
export SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR'
export PATH="$ROOT:\$PATH"
eval "\$($SD init)"
EOF

check "sd jumps in real bash"  0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd home; [[ \$PWD == '$HOME' ]]"
check "sd - returns to OLDPWD" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd home; cd /tmp; sd -; [[ \$PWD == '$HOME' ]]"
check "sd no args = HOME"      0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd; [[ \$PWD == '$HOME' ]]"
check "sd miss stays put"      1 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd bogus"

# the wrapper must cd into a suffix too
check "sd jumps into a suffix" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd tree/src; [[ \$PWD == '$SIMPLEDIR_CONFIG_DIR/tree/src' ]]"
check "sd takes a raw path"    0 bash --noprofile --norc -c "
  source '$wrapper'; cd /; sd '$SIMPLEDIR_CONFIG_DIR/tree'; [[ \$PWD == '$SIMPLEDIR_CONFIG_DIR/tree' ]]"

# the wrapper also owns the `simpledir` name, so a bare alias must cd
check "simpledir <alias> cds"  0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; simpledir home; [[ \$PWD == '$HOME' ]]"
check "simpledir - = OLDPWD"   0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; simpledir home; cd /tmp; simpledir -; [[ \$PWD == '$HOME' ]]"
check "simpledir no args=HOME" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; simpledir; [[ \$PWD == '$HOME' ]]"
check "simpledir miss stays"   1 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; simpledir bogus"

# subcommands must still reach the real binary, not the wrapper
check "simpledir add works"    0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir add brandnew '$SIMPLEDIR_CONFIG_DIR'"
check "simpledir ls works"     0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir ls | grep -q brandnew"
check "simpledir rm works"     0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir rm brandnew"
check "simpledir --help works" 0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir --help | grep -q usage"
check "simpledir --bogusflag"  2 bash --noprofile --norc -c "
  source '$wrapper'; simpledir --bogusflag"
check "wrapper is a function"  0 bash --noprofile --norc -c "
  source '$wrapper'; [[ \$(type -t simpledir) == function && \$(type -t sd) == function ]]"

# v2 subcommands must also reach the binary. these run last: rename mutates
# the config the earlier tests rely on.
check "wrapper forwards rename" 0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir rename tree shrub; simpledir jump shrub >/dev/null"
check "wrapper forwards edit"   0 bash --noprofile --norc -c "
  source '$wrapper'; EDITOR=true simpledir edit"
check "wrapper forwards ls --names" 0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir ls --names | grep -qx shrub"
check "wrapper forwards completions" 0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir completions bash | grep -q complete"
check "wrapper forwards bare add" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd '$SIMPLEDIR_CONFIG_DIR/tree' && simpledir add"
rm -f "$wrapper"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[[ $fail -eq 0 ]]
