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
Type "cd ~ && sdcfg init" Enter
Sleep 500ms
Type "cd ~" Enter
Sleep 300ms

# a directory called dotfiles keeps the "did you mean" honest: `dotfile` is a
# near miss for `dotfiles`, not a unique prefix of anything
Type "mkdir -p ~/.config/dotfiles/hypr" Enter
Sleep 900ms

Type "sdcfg add home" Enter
Sleep 600ms
Type "sdcfg add dotfiles ~/.config/dotfiles" Enter
Sleep 700ms
Type "sdcfg add downloads ~/Downloads" Enter
Sleep 600ms

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

Type "cd ~" Enter
Sleep 1200ms