#!/bin/bash
# package-linux.sh - Assemble a standalone, relocatable Linux distribution of Ladybird
# (UltraCanvas UI) into a .zip that runs on most distros without installing anything.
# Modeled on UI/UltraCanvas/package-win.sh, and reuses the transitive-ldd closure pattern
# from UI/UltraCanvas/UltraCanvasSources/package-linux.sh.
#
# Run from the repo root AFTER building the Linux release, e.g.:
#   ninja -C Build/release ladybird
#
# Usage:
#   ./UI/UltraCanvas/package-linux.sh [package-name.zip]
#   BUILD_DIR=Build/release ./UI/UltraCanvas/package-linux.sh
#
# Produced layout (matches how Ladybird locates things on Linux, see
# Libraries/LibWebView/Utilities.cpp: resources at <exe>/../share/Lagom, helper services
# at <exe>/../libexec/; and UltraCanvasConfig.cpp: UI fonts at <exe>/../share/media):
#   Ladybird/
#     Ladybird            launcher (sets LD_LIBRARY_PATH -> lib/, then exec bin/Ladybird)
#     bin/Ladybird        main executable
#     libexec/            WebContent RequestServer ImageDecoder WebWorker Compositor
#     lib/                liblagom-*.so* + every bundled dependency .so (except glibc + GPU/display)
#     share/Lagom/        Ladybird resources (icons, fonts, themes, ladybird/, about-pages)
#     share/media/        UltraCanvas UI fonts/icons
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/Build/release}"
DIST_ROOT="${DIST_ROOT:-$REPO_ROOT/dist}"
DIST="$DIST_ROOT/Ladybird"
PACKAGE_ZIP="${1:-$REPO_ROOT/Ladybird-Linux-x64-$(date +%y%m%d).zip}"

# Shared libraries that must come from the host, NOT be bundled: the glibc/loader core,
# and the GPU/GL/driver + display stack that has to match the running system (bundling a
# copy typically yields black windows or crashes on other machines). libstdc++ / libgcc_s
# ARE bundled on purpose so the package runs on older distros.
EXCLUDE_PATTERNS=(
    'ld-linux' 'libc.so' 'libm.so' 'libdl.so' 'libpthread' 'librt.so'
    'libresolv.so' 'libutil.so' 'libnsl.so' 'libnss_' 'libmvec.so'
    'libanl.so' 'libBrokenLocale.so' 'libcrypt.so' 'libthread_db'
    'libGL.so' 'libGLX' 'libGLdispatch' 'libEGL' 'libGLESv1' 'libGLESv2'
    'libOpenGL' 'libglapi' 'libgbm' 'libdrm'
    'libX11' 'libxcb' 'libX11-xcb' 'libwayland'
)

is_excluded() {
    local name="$1" pat
    for pat in "${EXCLUDE_PATTERNS[@]}"; do
        [[ "$name" == "$pat"* ]] && return 0
    done
    return 1
}

if [ ! -x "$BUILD_DIR/bin/Ladybird" ]; then
    echo "Error: '$BUILD_DIR/bin/Ladybird' not found. Build first (ninja -C Build/release ladybird)," >&2
    echo "       or set BUILD_DIR to your build directory." >&2
    exit 1
fi

echo "Build dir : $BUILD_DIR"
echo "Output    : $PACKAGE_ZIP"

rm -rf "$DIST"
mkdir -p "$DIST"/{bin,libexec,lib,share}

# 1) Main executable. bin/ also holds build-only tools (cranelift-compiler, flapc,
#    generate_*) that end users don't need, so copy just Ladybird.
echo "Copying executable..."
cp "$BUILD_DIR/bin/Ladybird" "$DIST/bin/"

# 2) Helper services (Ladybird spawns these from <prefix>/libexec/).
echo "Copying helper services..."
for exe in "$BUILD_DIR"/libexec/*; do
    [ -f "$exe" ] || continue
    case "$(basename "$exe")" in
        *Test|*Tests) continue ;;
    esac
    cp "$exe" "$DIST/libexec/"
    echo "  $(basename "$exe")"
done

# 3) Ladybird's own shared libs (liblagom-*.so*). cp -a preserves the versioned symlink
#    chains (.so -> .so.0 -> .so.0.1.0); the *.a static libs are excluded by the glob.
cp -a "$BUILD_DIR"/lib/*.so* "$DIST/lib/"

# 4) Transitive dependency closure. Walk ldd over every exe/.so in the staging tree and
#    pull in each resolved, non-excluded .so; repeat until a full pass adds nothing.
echo "Resolving shared-library dependencies..."
declare -A SKIPPED
copy_deps_once() {
    local added=0 target base _arrow resolved _addr
    while IFS= read -r -d '' target; do
        # ldd lines:  libfoo.so.1 => /path/libfoo.so.1 (0x..)
        while read -r base _arrow resolved _addr; do
            [ "$_arrow" = "=>" ] || continue
            [ -f "$resolved" ] || continue
            if is_excluded "$base"; then
                SKIPPED["$base"]=1
                continue
            fi
            [ -e "$DIST/lib/$base" ] && continue
            cp -L "$resolved" "$DIST/lib/$base"
            echo "  + $base"
            added=1
        done < <(ldd "$target" 2>/dev/null)
    done < <(find "$DIST/bin" "$DIST/libexec" "$DIST/lib" -type f \( -name '*.so*' -o -perm -u+x \) -print0)
    [ "$added" -eq 1 ]
}
while copy_deps_once; do :; done
if [ "${#SKIPPED[@]}" -gt 0 ]; then
    echo "  (left to host: ${!SKIPPED[*]})"
fi
echo "  $(find "$DIST/lib" -maxdepth 1 -name '*.so*' | wc -l) libraries bundled"

# 5) Resources. Ladybird resolves these as <exe>/../share/Lagom on Linux.
echo "Copying resources (share/Lagom)..."
LAGOM_SRC="$(find "$BUILD_DIR" -type d -name Lagom -path '*/share/*' 2>/dev/null | head -1)"
if [ -z "$LAGOM_SRC" ]; then
    echo "Error: could not find share/Lagom under $BUILD_DIR. Was the build completed?" >&2
    exit 1
fi
cp -r "$LAGOM_SRC" "$DIST/share/Lagom"

# 6) UltraCanvas UI fonts/icons. On Linux UltraCanvasConfig.cpp probes for a media/ dir at
#    <exe>/../share/, so stage the toolkit's media tree there (fonts + default icons).
echo "Copying UltraCanvas media (fonts/icons)..."
cp -r "$REPO_ROOT/UI/UltraCanvas/UltraCanvasSources/media" "$DIST/share/media"

# 7) Launcher. Points the dynamic loader at bundled lib/ (child helper processes inherit
#    LD_LIBRARY_PATH, so they resolve it too), then exec the real exe. Resources are found
#    automatically (bin/../share/Lagom, bin/../libexec).
cat > "$DIST/Ladybird" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/bin/Ladybird" "$@"
EOF
chmod +x "$DIST/Ladybird"

# 8) Zip it (store the top-level Ladybird/ folder). -y preserves the lib/ symlinks so the
#    archive stays compact instead of tripling every versioned library.
echo "Creating $PACKAGE_ZIP..."
rm -f "$PACKAGE_ZIP"
( cd "$DIST_ROOT" && zip -q -r -y "$PACKAGE_ZIP" Ladybird )

echo ""
echo "=== Package complete ==="
echo "  1 executable, $(find "$DIST/libexec" -maxdepth 1 -type f | wc -l) helpers, $(find "$DIST/lib" -maxdepth 1 -name '*.so*' | wc -l) libraries"
echo "  Staging : $DIST"
echo "  Archive : $PACKAGE_ZIP"
echo ""
echo "Run with: unzip the archive, then ./Ladybird/Ladybird"
