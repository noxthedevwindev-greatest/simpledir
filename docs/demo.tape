# Record with:  vhs docs/demo.tape   (needs vhs + ttyd: https://github.com/charmbracelet/vhs)
# Run from the repo root.
#
# No setup commands in here: simpledir is already installed, so the recording
# can start on the tool. `Hide` isn't used either — it hides the keystrokes but
# the shell still echoes them, so the plumbing ends up on screen anyway.
#
# Wipe the throwaway config before recording, not during it:
#   rm -rf /tmp/simpledir-demo && vhs docs/demo.tape
#
# Sleeps after Enter are load-bearing: ttyd needs a beat to swallow the
# newline, otherwise the next Type gets glued onto the same command line.
Output docs/demo.gif

Set Shell "bash"
Set FontSize 18
Set Width 1200
Set Height 620
Set Theme "Catppuccin Mocha"
Set TypingSpeed 35ms
Set Padding 16

# a throwaway config, so re-recording never inherits the last take's aliases and
# a real ~/.simpledir is never touched.
Env SIMPLEDIR_CONFIG_DIR "/tmp/simpledir-demo"
Env SIMPLEDIR_NO_UPDATE_CHECK "1"

# the tagline carries the release: every download is checked against a published
# checksum, so that is what the recording opens on.
Type "cd ~ && sdcfg --version" Enter
Sleep 1200ms

Type "sdcfg add home" Enter
Sleep 600ms
Type "sdcfg add dots ~/.config" Enter
Sleep 700ms
Type "sdcfg add shots ~/Pictures" Enter
Sleep 700ms
Type "sdcfg add downloads ~/Downloads" Enter
Sleep 800ms

Type "clear" Enter
Sleep 600ms
Type "sd ls" Enter
Sleep 1500ms

Type "clear" Enter
Sleep 600ms
Type "cd /tmp" Enter
Sleep 400ms
Type "sd dots" Enter
Sleep 400ms
Type "pwd" Enter
Sleep 1500ms

# one bookmark for a whole tree: dots/hypr lands inside it
Type "clear" Enter
Sleep 600ms
Type "sd dots/hypr && pwd" Enter
Sleep 1600ms

# tags: a named group, so `sd @desk` is "the directories I use at a desk"
Type "clear" Enter
Sleep 600ms
Type "sdcfg tag @desk dots downloads shots" Enter
Sleep 1300ms
Type "sd @desk" Enter
Sleep 1700ms

# bench: the release before this one was about being fast, and it ships the
# measuring stick rather than a number you have to take on trust
Type "clear" Enter
Sleep 600ms
Type "cd ~/Projects/simpledir && sdcfg bench --runs 40" Enter
Sleep 6000ms

# and this one ships the reason to trust it: hash anything yourself
Type "clear" Enter
Sleep 600ms
Type "sd sha256 sd.cpp" Enter
Sleep 1600ms
Type "sd sha256 ~/.bashrc" Enter
Sleep 1500ms

# the part zoxide got right first: a directory you keep going to stays
# remembered after you delete its name
Type "clear" Enter
Sleep 600ms
Type "cd ~ && sdcfg add wz ~/Projects/simpledir && sd wz && sdcfg rm wz" Enter
Sleep 2400ms

Type "clear" Enter
Sleep 600ms
Type "sd sim" Enter
Sleep 500ms
Type "pwd" Enter
Sleep 2000ms

# a unique prefix is enough...
Type "clear" Enter
Sleep 600ms
Type "sd shot && pwd" Enter
Sleep 1600ms

# ...and a word that is neither explains itself rather than failing flat
Type "clear" Enter
Sleep 600ms
Type "sd sots" Enter
Sleep 1700ms

Type "cd ~" Enter
Sleep 1400ms