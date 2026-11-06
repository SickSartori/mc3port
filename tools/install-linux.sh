#!/bin/sh
# Installs mc3port for the current user on Linux, through Wine: copies this folder to ~/.local/share/mc3port, makes
# a Wine prefix of its own there, and adds the "mc3port" command (~/.local/bin) and an entry in the applications
# menu. Needs a recent Wine with 32-bit support: README.md, "Linux". Run it again to update: settings and saves stay.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dest=${XDG_DATA_HOME:-$HOME/.local/share}/mc3port
bin=$HOME/.local/bin
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
export WINEDEBUG=${WINEDEBUG:--all}

command -v wine >/dev/null 2>&1 || { echo "wine not found: install Wine first (README.md, section Linux)." >&2; exit 1; }

mkdir -p "$dest" "$bin" "$apps"
cp -f "$here/mc3.exe" "$here/mc3runtime.dll" "$here/mc3port.png" "$here/README.txt" "$dest/"
# Up to 0.3.0 the documents were next to mc3.exe; they are in docs now.
rm -rf "$dest/licenses" "$dest/docs" "$dest"/*.md "$dest/LICENSE" && cp -r "$here/docs" "$dest/"
# Settings, saves, log and shader cache next to mc3.exe (README.md, "Portable"): everything in one folder.
: > "$dest/portable.txt"

export WINEPREFIX=$dest/prefix
if [ ! -f "$WINEPREFIX/system.reg" ]; then
    echo "Making the Wine prefix in $WINEPREFIX (about a minute)..."
    # No Mono and no Gecko: mc3port does not use them, and Wine would offer to download them.
    WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i
    wineserver -w
fi

cat > "$bin/mc3port" <<SCRIPT
#!/bin/sh
# Starts mc3port (installed by install.sh) in its own Wine prefix.
export WINEPREFIX="$dest/prefix"
export WINEDEBUG=\${WINEDEBUG:--all}
cd "$dest" && exec wine mc3.exe "\$@"
SCRIPT
chmod +x "$bin/mc3port"

cat > "$apps/mc3port.desktop" <<ENTRY
[Desktop Entry]
Type=Application
Name=Midnight Club 3 DUB Edition Remix (mc3port)
Comment=Unofficial native Windows port of Midnight Club 3: DUB Edition Remix, through Wine
Exec="$bin/mc3port"
Icon=$dest/mc3port.png
Terminal=false
Categories=Game;
ENTRY
update-desktop-database "$apps" >/dev/null 2>&1 || true

echo "mc3port is installed in $dest."
echo "Start it from the applications menu, or with: $bin/mc3port"
echo "(if the command is not found, add $bin to your PATH, or run it with its full path)"
echo "Your settings and saves are in $dest. To remove everything:"
echo "  rm -rf \"$dest\" \"$bin/mc3port\" \"$apps/mc3port.desktop\""
