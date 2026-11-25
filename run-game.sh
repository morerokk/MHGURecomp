#!/bin/sh
# Starts MHGU Recomp from the build folder (macOS / Linux: the game files in build/data/exefs and build/data/romfs;
# saves go to ~/Library/Application Support/MHGURecomp/save, on Linux ~/.local/share/MHGURecomp/save). Settings,
# caches, mods/ and the log are kept in run/ (MHGU_HOME), so build/ stays exactly what a game folder needs, as
# run-game.bat does on Windows.
here=$(cd "$(dirname "$0")" && pwd)
export MHGU_HOME="$here/run"
mkdir -p "$MHGU_HOME"
"$here/build/mhgurecomp" 2> "$MHGU_HOME/log.txt"
status=$?
[ $status -ne 0 ] && echo "The game exited with an error ($status). See run/log.txt"
exit $status
