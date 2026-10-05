# Record with:  vhs docs/demo.tape   (needs vhs + ttyd: https://github.com/charmbracelet/vhs)
# Run from the repo root.
#
# No setup commands in here: simpledir is already installed, so the recording
# can start on the tool. `Hide` isn't used either — it hides the keystrokes but
# the shell still echoes them, so the plumbing ends up on screen anyway.
#
# Sleeps after Enter are load-bearing: ttyd needs a beat to swallow the
# newline, otherwise the next Type gets glued onto the same command line.
Output docs/demo.gif

Set Shell "bash"
Set FontSize 18
Set Width 980
Set Height 460
Set Theme "Catppuccin Mocha"
Set TypingSpeed 35ms
Set Padding 16

# a throwaway config, so re-recording never inherits the last take's aliases and
# a real ~/.simpledir is never touched. wipe it before recording, not in it:
# `rm -rf /tmp/simpledir-demo && vhs docs/demo.tape`
Env SIMPLEDIR_CONFIG_DIR "/tmp/simpledir-demo"
Env SIMPLEDIR_NO_UPDATE_CHECK "1"

Type "cd ~ && sdcfg --version" Enter
Sleep 1000ms

Type "sdcfg add home" Enter
Sleep 600ms
Type "sdcfg add dotfiles ~/.config/hypr" Enter
Sleep 700ms
Type "sdcfg add downloads ~/Downloads" Enter
Sleep 700ms

Type "clear" Enter
Sleep 600ms
Type "sd ls" Enter
Sleep 1400ms

Type "clear" Enter
Sleep 600ms
Type "cd /tmp" Enter
Sleep 300ms
Type "sd dotfiles" Enter
Sleep 400ms
Type "pwd" Enter
Sleep 1400ms

Type "clear" Enter
Sleep 600ms
Type "cd /tmp && sd dotfiles/hypr" Enter
Sleep 1500ms

Type "clear" Enter
Sleep 600ms
Type "sdcfg rename dotfiles dots" Enter
Sleep 1300ms

Type "clear" Enter
Sleep 600ms
Type "cd /tmp && sd downloads && pwd" Enter
Sleep 1500ms

Type "clear" Enter
Sleep 700ms
Type "sd dotfles" Enter
Sleep 2600ms

# the part zoxide got right first: a directory you go to stays remembered even
# after you delete its name. `wz` is bound, jumped to, then unbound — and `sim`,
# which was never an alias at all, still finds it.
Type "clear" Enter
Sleep 600ms
Type "sdcfg add wz ~/Projects/simpledir && sd wz && sdcfg rm wz" Enter
Sleep 2200ms

Type "clear" Enter
Sleep 600ms
Type "sd sim" Enter
Sleep 400ms
Type "pwd" Enter
Sleep 2000ms

Type "clear" Enter
Sleep 600ms
Type "sd top" Enter
Sleep 1800ms

Type "cd ~" Enter
Sleep 1200ms
