# Record with:  vhs docs/demo.tape   (needs vhs + ttyd: https://github.com/charmbracelet/vhs)
# Run from the repo root. The demo only touches $HOME.
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

# a throwaway config so the recording never touches ~/.simpledir
Env SIMPLEDIR_CONFIG_DIR "/tmp/simpledir-demo"

Type "export PATH=$PWD:$PATH" Enter
Sleep 400ms
Type "source <(simpledir init)" Enter
Sleep 500ms
Type "cd ~" Enter
Sleep 300ms

Type "simpledir ls" Enter
Sleep 1100ms

Type "simpledir add home" Enter
Sleep 600ms
Type "simpledir add dotfiles ~/.config" Enter
Sleep 600ms
Type "simpledir add downloads ~/Downloads" Enter
Sleep 600ms

Type "clear" Enter
Sleep 600ms
Type "simpledir ls" Enter
Sleep 1400ms

Type "clear" Enter
Sleep 600ms
Type "cd /tmp" Enter
Sleep 300ms
Type "sd dotfiles" Enter
Sleep 500ms
Type "pwd" Enter
Sleep 1400ms

Type "clear" Enter
Sleep 600ms
Type "cd /tmp && simpledir downloads && pwd" Enter
Sleep 1400ms

Type "clear" Enter
Sleep 600ms
Type "sd dotfile" Enter
Sleep 2000ms

Type "cd ~" Enter
Sleep 600ms
