#!/bin/bash
# package-win.sh - Assemble a standalone Windows distribution of Ladybird (UltraCanvas UI),
# built with clang-cl + vcpkg, into a .zip. Modeled on the UltraCanvas demo's package-win.sh but
# adapted for the Ladybird clang-cl/vcpkg build (DLLs come from vcpkg, not /mingw64).
#
# Run from an MSYS2 or Git-Bash shell, from the repo root, AFTER building:
#   py Meta\ladybird.py build --gui UltraCanvas ladybird
#
# Usage:
#   ./UI/UltraCanvas/package-win.sh [package-name.zip]
#   BUILD_DIR=Build/release ./UI/UltraCanvas/package-win.sh
#
# Produced layout (matches how Ladybird locates things on Windows, see
# Libraries/LibWebView/Utilities.cpp: helper exes are found next to Ladybird.exe, and resources
# at <exe>/../share/Lagom):
#   Ladybird/
#     bin/            Ladybird.exe + WebContent/RequestServer/ImageDecoder/WebWorker/Compositor .exe + all runtime .dll
#     share/Lagom/    resources (icons, fonts, themes, ladybird/, about-pages, ...)
#     etc/fonts/fonts.conf   fontconfig config (maps sans-serif/serif/monospace to real Windows fonts)
#     Ladybird.bat    launcher (points FONTCONFIG_FILE at the bundled fonts.conf)
set -e

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/Build/release}"
VCPKG_BIN="${VCPKG_BIN:-$BUILD_DIR/vcpkg_installed/x64-windows/bin}"
DIST_ROOT="${DIST_ROOT:-$REPO_ROOT/dist}"
DIST="$DIST_ROOT/Ladybird"
PACKAGE_ZIP="${1:-$REPO_ROOT/Ladybird-Windows-x64-`date +%y%m%d`.zip}"

if [ ! -d "$BUILD_DIR/bin" ]; then
    echo "Error: '$BUILD_DIR/bin' not found. Build first (py Meta\\ladybird.py build --gui UltraCanvas ladybird)," >&2
    echo "       or set BUILD_DIR to your build directory." >&2
    exit 1
fi

# Resolve the PE import table statically with objdump (never loads the module, unlike ldd).
if command -v llvm-objdump >/dev/null 2>&1; then OBJDUMP=llvm-objdump
elif command -v objdump >/dev/null 2>&1; then OBJDUMP=objdump
else echo "Error: need llvm-objdump (ships with LLVM) or objdump on PATH." >&2; exit 1; fi

echo "Build dir : $BUILD_DIR"
echo "vcpkg bin : $VCPKG_BIN"
echo "Output    : $PACKAGE_ZIP"

rm -rf "$DIST"
mkdir -p "$DIST/bin" "$DIST/etc/fonts"

# 1) Executables. On Windows every helper service lives next to Ladybird.exe, so they are all in
#    the build's bin/. Skip unit-test/dev binaries end users don't need.
echo "Copying executables..."
for exe in "$BUILD_DIR"/bin/*.exe; do
    [ -e "$exe" ] || continue
    case "$(basename "$exe")" in
        *Test.exe|*Tests.exe|TestWeb*.exe) continue ;;
    esac
    cp "$exe" "$DIST/bin/"
    echo "  $(basename "$exe")"
done

# 2) DLLs already staged next to the exe (vcpkg z-applocal + Ladybird's own lagom-*.dll).
cp "$BUILD_DIR"/bin/*.dll "$DIST/bin/" 2>/dev/null || true

# 2b) Runtime-loaded DLLs the import-table walk cannot see. ANGLE's GLES/EGL and the D3D shader
#     compiler are LoadLibrary'd at runtime by Skia/WebContent (so they are NOT in any import table),
#     and the MSVC C++ runtime is a redistributable that is present on a dev box but not necessarily
#     on a clean target. Search vcpkg's bin, System32 and the VS redist for each; warn if not found.
#     Done before the closure below so those DLLs' own dependencies get pulled in too.
echo "Bundling runtime-loaded DLLs..."
win_system32="$(cygpath -u "${SYSTEMROOT:-C:\\Windows}" 2>/dev/null)/System32"
vs_crt_dir="$(ls -d "/c/Program Files"*"/Microsoft Visual Studio/"*"/"*"/VC/Redist/MSVC/"*"/x64/Microsoft.VC"*".CRT" 2>/dev/null | sort | tail -1)"
extra_search_dirs=( "$VCPKG_BIN" "$win_system32" "/c/Windows/System32" "$vs_crt_dir" )
extra_dlls=(
    libEGL.dll libGLESv2.dll d3dcompiler_47.dll
    vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_1.dll msvcp140_2.dll concrt140.dll
)
for name in "${extra_dlls[@]}"; do
    [ -f "$DIST/bin/$name" ] && continue
    found=""
    for d in "${extra_search_dirs[@]}"; do
        if [ -n "$d" ] && [ -f "$d/$name" ]; then
            cp "$d/$name" "$DIST/bin/"
            echo "  runtime: $name"
            found=1
            break
        fi
    done
    [ -z "$found" ] && echo "  (not found: $name — if the app needs it, install the VC++ Redistributable on the target or copy it manually)"
done

# 3) Transitive PE dependency closure, pulling any still-missing DLLs from vcpkg's bin. Repeats
#    until a full pass adds nothing (objdump keeps every pass fast and hang-free).
echo "Resolving DLL dependencies..."
pe_deps() { "$OBJDUMP" -p "$1" 2>/dev/null | sed -n 's/.*DLL Name:[[:space:]]*//p'; }
while : ; do
    before=$(find "$DIST/bin" -maxdepth 1 -name '*.dll' | wc -l)
    while IFS= read -r bin; do
        pe_deps "$bin" | while IFS= read -r name; do
            [ -z "$name" ] && continue
            [ -f "$DIST/bin/$name" ] && continue
            if [ -f "$VCPKG_BIN/$name" ]; then
                cp "$VCPKG_BIN/$name" "$DIST/bin/"
                echo "  + $name"
            fi
        done
    done < <(find "$DIST/bin" -maxdepth 1 \( -name '*.exe' -o -name '*.dll' \))
    after=$(find "$DIST/bin" -maxdepth 1 -name '*.dll' | wc -l)
    [ "$before" = "$after" ] && break
done
echo "  $(find "$DIST/bin" -maxdepth 1 -name '*.dll' | wc -l) DLLs bundled"

# 3b) Authenticode code signing (via sign-win.ps1). Unsigned, zero-reputation helper exes
#     (notably ImageDecoder.exe, which does IPC handle duplication into a peer process) get
#     flagged by heuristic AV as false positives (e.g. AVG "IDP.Generic"). By default this
#     signs with a SELF-SIGNED "Cloverleaf UG" placeholder cert so the pipeline is complete;
#     that does NOT clear AV/SmartScreen on other machines — swap in a real CA cert for the
#     actual fix by setting SIGN_THUMBPRINT or SIGN_PFX (they take precedence automatically).
#       SIGN_THUMBPRINT      SHA1 thumbprint of a real cert in the Windows store (EV tokens)
#       SIGN_PFX / SIGN_PFX_PASSWORD   path + password of a real .pfx/.p12 file cert (OV)
#       SIGN_TIMESTAMP_URL   RFC3161 timestamp server (default DigiCert)
#       SIGN_DLLS=1          also sign our own lagom-*.dll (third-party vcpkg DLLs are left as-is)
#       SIGN=none            skip signing entirely (produce an unsigned zip)
sign_binaries() {
    if [ "$SIGN" = "none" ]; then
        echo "Skipping code signing (SIGN=none) — binaries will be unsigned."
        return 0
    fi

    if ! command -v powershell.exe >/dev/null 2>&1; then
        echo "Warning: powershell.exe not found — skipping code signing (binaries will be unsigned)." >&2
        return 0
    fi

    local ps1 win_bindir
    ps1="$(cygpath -w "$REPO_ROOT/UI/UltraCanvas/sign-win.ps1")"
    win_bindir="$(cygpath -w "$DIST/bin")"

    local -a ps_args=( -BinDir "$win_bindir" )
    [ -n "$SIGN_THUMBPRINT" ]    && ps_args+=( -Thumbprint "$SIGN_THUMBPRINT" )
    [ -n "$SIGN_PFX" ]          && ps_args+=( -PfxPath "$(cygpath -w "$SIGN_PFX")" )
    [ -n "$SIGN_PFX_PASSWORD" ] && ps_args+=( -PfxPassword "$SIGN_PFX_PASSWORD" )
    [ -n "$SIGN_TIMESTAMP_URL" ] && ps_args+=( -TimestampUrl "$SIGN_TIMESTAMP_URL" )
    [ "$SIGN_DLLS" = "1" ]      && ps_args+=( -SignDlls )

    echo "Code signing binaries via sign-win.ps1..."
    # set -e aborts the whole package run if signing fails with a real cert, so a misconfigured
    # cert never silently ships unsigned.
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$ps1" "${ps_args[@]}"
}
sign_binaries

# 4) Resources. Ladybird resolves these as <exe>/../share/Lagom on Windows, so mirror share/Lagom
#    next to bin/. This carries the toolbar icons, fonts, themes, about-pages and ladybird/ assets.
echo "Copying resources (share/Lagom)..."
LAGOM_SRC="$(find "$BUILD_DIR" -type d -name Lagom -path '*/share/*' 2>/dev/null | head -1)"
if [ -z "$LAGOM_SRC" ]; then
    echo "Error: could not find share/Lagom under $BUILD_DIR. Was the build completed?" >&2
    exit 1
fi
mkdir -p "$DIST/share"
cp -r "$LAGOM_SRC" "$DIST/share/Lagom"

# 4b) UltraCanvas's own default UI fonts (Ubuntu), staged by the build at bin/Resources/media/fonts/
#     — UltraCanvas registers them from <exe_dir>/Resources/media/fonts/ at startup.
if [ -d "$BUILD_DIR/bin/Resources" ]; then
    cp -r "$BUILD_DIR/bin/Resources" "$DIST/bin/Resources"
    echo "  bundled UltraCanvas fonts (Resources/media/fonts)"
fi

# 5) fontconfig config. The chrome renders via Pango+fontconfig; without a config that maps the
#    generic families to real fonts, fontconfig falls back to a monospace default (the "Courier"
#    look). WINDOWSFONTDIR/WINDOWSUSERFONTDIR are fontconfig keywords for the Windows font folders.
cat > "$DIST/etc/fonts/fonts.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>WINDOWSFONTDIR</dir>
  <dir>WINDOWSUSERFONTDIR</dir>
  <cachedir>LOCAL_APPDATA_FONTCONFIG_CACHE</cachedir>

  <alias>
    <family>sans-serif</family>
    <prefer><family>Segoe UI</family><family>Arial</family><family>Tahoma</family></prefer>
  </alias>
  <alias>
    <family>serif</family>
    <prefer><family>Times New Roman</family><family>Georgia</family></prefer>
  </alias>
  <alias>
    <family>monospace</family>
    <prefer><family>Consolas</family><family>Courier New</family></prefer>
  </alias>
  <!-- Note: the UltraCanvas chrome's own Ubuntu/UbuntuMono fonts are registered at runtime from
       bin/Resources/media/fonts/ (FcConfigAppFontAddFile), so they resolve without a substitution. -->
</fontconfig>
EOF

# 6) Launcher. Points fontconfig at the bundled config; resources are found automatically
#    (bin\..\share\Lagom). Runs the console-subsystem exe directly so logs stay visible.
cat > "$DIST/Ladybird.bat" <<'EOF'
@echo off
set "HERE=%~dp0"
set "FONTCONFIG_FILE=%HERE%etc\fonts\fonts.conf"
rem Windowed app (no console). Logs go to bin\debug.log by default; override with --debug-log <path>.
rem "start" launches detached so this launcher window closes immediately.
start "" "%HERE%bin\Ladybird.exe" %*
EOF

# 7) Zip it (store the top-level Ladybird/ folder).
echo "Creating $PACKAGE_ZIP..."
rm -f "$PACKAGE_ZIP"
( cd "$DIST_ROOT" && zip -q -r "$PACKAGE_ZIP" Ladybird )

echo ""
echo "=== Package complete ==="
echo "  $(find "$DIST/bin" -maxdepth 1 -name '*.exe' | wc -l) executables, $(find "$DIST/bin" -maxdepth 1 -name '*.dll' | wc -l) DLLs"
echo "  Staging : $DIST"
echo "  Archive : $PACKAGE_ZIP"
echo ""
echo "Run with: extract the zip, then double-click Ladybird\\Ladybird.bat"
