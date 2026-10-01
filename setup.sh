#!/bin/sh
# wine-dxgi-framelimiter — one-click installer.
#
#   ./setup.sh                          install into $WINEPREFIX (default ~/.wine)
#   ./setup.sh --prefix ~/Games/pfx     install into another prefix
#   ./setup.sh status | uninstall       (same --prefix option)
#
# install:  system32\dxgi.dll (DXMT)  -> system32\dxgi_orig.dll
#           proxy dxgi.dll            -> system32\dxgi.dll
# The proxy is a pure passthrough unless FL_FPS is set, so nothing changes until a
# program is launched with FL_FPS (see ./flwine). Re-run after updating Wine/DXMT.
set -e
cd "$(dirname "$0")"

REPO="dngur521/wine-dxgi-framelimiter"
MARK="FL_FPS"            # string only the proxy contains
CMD=install
PREFIX="${WINEPREFIX:-$HOME/.wine}"
FORCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        install|uninstall|status) CMD="$1" ;;
        --prefix) PREFIX="$2"; shift ;;
        --force)  FORCE=1 ;;
        -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see --help)"; exit 1 ;;
    esac
    shift
done

SYS="$PREFIX/drive_c/windows/system32"
say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*"; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*"; exit 1; }
is_proxy() { strings -a "$1" 2>/dev/null | grep -q "$MARK"; }
is_dxmt()  { strings -a "$1" 2>/dev/null | grep -qi 'dxmt'; }

[ -d "$SYS" ] || die "no Wine prefix at $PREFIX (set --prefix or WINEPREFIX)"

# Older installs of this project kept the real DLL as dxgi_dxmt.dll.
if [ -f "$SYS/dxgi_dxmt.dll" ] && [ ! -f "$SYS/dxgi_orig.dll" ]; then
    mv "$SYS/dxgi_dxmt.dll" "$SYS/dxgi_orig.dll"
fi

case "$CMD" in
status)
    if is_proxy "$SYS/dxgi.dll"; then say "proxy installed in $PREFIX"
    else say "proxy not installed in $PREFIX"; fi
    ls -la "$SYS/dxgi.dll" "$SYS/dxgi_orig.dll" 2>/dev/null || true
    exit 0 ;;
uninstall)
    is_proxy "$SYS/dxgi.dll" || { say "proxy not installed"; exit 0; }
    [ -f "$SYS/dxgi_orig.dll" ] || die "dxgi_orig.dll missing; reinstall DXMT to restore dxgi.dll"
    mv -f "$SYS/dxgi_orig.dll" "$SYS/dxgi.dll"
    say "restored the original dxgi.dll"
    exit 0 ;;
esac

# ---- 1. get the proxy DLL: build it, or download the prebuilt release ----
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    say "building with mingw-w64"
    ./build.sh >/dev/null
    DLL=build/dxgi.dll
else
    say "mingw-w64 not found; downloading prebuilt dxgi.dll"
    mkdir -p build
    curl -fsSL -o build/dxgi.dll "https://github.com/$REPO/releases/latest/download/dxgi.dll" \
        || die "download failed (or build it yourself: brew install mingw-w64 && ./build.sh)"
    DLL=build/dxgi.dll
fi
is_proxy "$DLL" || die "$DLL is not the proxy"

# ---- 2. check what is in the prefix ----
if is_proxy "$SYS/dxgi.dll"; then
    [ -f "$SYS/dxgi_orig.dll" ] || die "proxy is installed but dxgi_orig.dll is missing; reinstall DXMT"
    say "proxy already installed; updating it"
else
    [ -f "$SYS/dxgi.dll" ] || die "no system32\\dxgi.dll in the prefix; install DXMT first"
    if ! is_dxmt "$SYS/dxgi.dll"; then
        [ "$FORCE" = 1 ] || die "system32\\dxgi.dll is not DXMT. Only DXMT is tested (Wine's builtin
       wined3d dxgi will NOT work). Use --force to install anyway."
        warn "installing over a non-DXMT dxgi.dll (--force)"
    fi
    mkdir -p backup
    cp -p "$SYS/dxgi.dll" "backup/dxgi.dll.$(date +%Y%m%d%H%M%S)"
    cp -p "$SYS/dxgi.dll" "$SYS/dxgi_orig.dll"
fi

if ! grep -qE '^"dxgi"="native' "$PREFIX/user.reg" 2>/dev/null; then
    warn "no \"dxgi\"=\"native,builtin\" DLL override found in $PREFIX/user.reg — DXMT normally sets it"
fi

# ---- 3. install (copy + rename: running processes keep the old file) ----
cp "$DLL" "$SYS/dxgi.dll.fl-new"
mv -f "$SYS/dxgi.dll.fl-new" "$SYS/dxgi.dll"
say "installed into $SYS"

cat <<EOF

Done. Now launch with the limiter enabled, e.g.:

  ./flwine --fps 60 --exe Game.exe "C:\\Program Files (x86)\\Steam\\Steam.exe"

or put these lines in your own launch script before the wine command:

  export FL_FPS=60
  export FL_EXE=Game.exe

Quit Steam (and every Wine program in this prefix) first — the variables only
reach the game when Wine starts fresh. See README.md for details.
EOF
