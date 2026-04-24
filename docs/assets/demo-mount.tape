# vhs tape for docs/assets/demo-mount.gif
#
# Regenerate with: vhs docs/assets/demo-mount.tape
# Or run:          make demo
#
# Requires: vhs (charmbracelet), ttyd, tmux, ffmpeg.

Output docs/assets/demo-mount.gif

Set Shell bash
Set FontFamily "DejaVu Sans Mono"
Set FontSize  14
Set Width    1400
Set Height   360
Set Padding  20
Set Theme    "Catppuccin Mocha"
Set TypingSpeed 0ms
Set PlaybackSpeed 1.0

Hide
Type "bash scripts/demo-mount-driver.sh"
Enter
Show

Sleep 16s
