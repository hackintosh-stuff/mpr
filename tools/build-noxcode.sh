#!/bin/sh
# build-noxcode.sh - build MPR.kext with only the Command Line Tools
#
# The target machine has /Library/Developer/CommandLineTools but no Xcode.app:
# xcodebuild is a stub. This script drives clang, MacKernelSDK headers, libkmod
# and `ld -kext` directly, following the build-noxcode.sh convention used by
# AppleIGB / NvmeHook in this workspace.
#
# Usage (on macOS):
#   sh tools/build-noxcode.sh [source-root]
#   MACKERNELSDK=/path/to/MacKernelSDK sh tools/build-noxcode.sh
#
# Output: <root>/build/MPR.kext (fixed path, so deploy tooling can find it).
# This script only builds. It does not deploy, does not write to any system
# volume and does not rebuild a kernel collection.
set -eu

[ "$(uname -s)" = "Darwin" ] || { echo "This script can only run on macOS" >&2; exit 1; }
die() { echo "[error] $*" >&2; exit 1; }

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${1:-$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)}
ROOT=$(CDPATH= cd -- "$ROOT" && pwd)
SDK=${MACKERNELSDK:-$ROOT/MacKernelSDK}

NAME=MPR
VERSION=0.20.6
BUNDLE_ID=com.ling.MPR

echo "== Environment =="
echo "root   : $ROOT"
xcrun clang++ --version | head -1
echo "sdk    : $SDK"
echo "version: $NAME $VERSION"

[ -f "$ROOT/MPR/MPRController.cpp" ] || die "missing MPR/MPRController.cpp"
[ -f "$ROOT/MPR/MPRController.h" ] || die "missing MPR/MPRController.h"
[ -f "$ROOT/MPR/MPRBlockDev.cpp" ] || die "missing MPR/MPRBlockDev.cpp"
[ -f "$ROOT/MPR/MPRBlockDev.h" ] || die "missing MPR/MPRBlockDev.h"
[ -f "$ROOT/MPR/Info.plist" ] || die "missing MPR/Info.plist"
[ -d "$SDK/Headers/IOKit" ] || die "MacKernelSDK headers missing: $SDK/Headers/IOKit"
[ -f "$SDK/Library/universal/libkmodc++.a" ] || die "missing libkmodc++.a"
[ -f "$SDK/Library/x86_64/libkmod.a" ] || die "missing libkmod.a (x86_64)"

W="$ROOT/build"
OBJ="$W/objects"
KEXT="$W/$NAME.kext"
BIN="$KEXT/Contents/MacOS/$NAME"
rm -rf "$OBJ" "$KEXT"
mkdir -p "$OBJ" "$KEXT/Contents/MacOS"

# xcodebuild normally emits this translation unit via CreateKModInfo.perl.
# Without Xcode the kext has no _kmod_info and is rejected at load time.
# _realmain stays null: this is a plain IOKit kext, not a Lilu plugin.
cat > "$OBJ/kmod-info.cpp" <<'KMOD_EOF'
#include <mach/kmod.h>
#include <libkern/OSKextLib.h>

extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);

KMOD_EXPLICIT_DECL(com.ling.MPR, "0.20.6", _start, _stop);

__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = nullptr;
__attribute__((visibility("hidden"))) kmod_stop_func_t  *_antimain = nullptr;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
KMOD_EOF

CFLAGS="-arch x86_64 -mkernel -fapple-kext -mno-red-zone -nostdinc++ -O0 \
 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -D__APPLE_KEXT__ \
 -fno-builtin -fno-common -fno-exceptions -fno-rtti \
 -fstack-protector -fno-threadsafe-statics \
 -Wno-unused-parameter -Wno-unused-function -std=gnu++17"
INCLUDES="-I $SDK/Headers -I $ROOT/MPR"

echo "== Compilation =="
for source in "$ROOT"/MPR/*.cpp; do
    object=$(basename "$source" .cpp)
    # shellcheck disable=SC2086
    xcrun clang++ $CFLAGS $INCLUDES -c "$source" -o "$OBJ/$object.o"
done
# shellcheck disable=SC2086
xcrun clang++ $CFLAGS $INCLUDES -c "$OBJ/kmod-info.cpp" -o "$OBJ/kmod-info.o"

echo "== Linking =="
# -kext leaves kernel symbols undefined on purpose; they resolve at load.
# shellcheck disable=SC2086
xcrun ld -kext -arch x86_64 "$OBJ"/*.o \
    "$SDK/Library/universal/libkmodc++.a" \
    "$SDK/Library/x86_64/libkmod.a" \
    -o "$BIN"

echo "== Bundle validation =="
nm "$BIN" > "$W/symbols.txt"
for sym in ' _kmod_info$' ' __start$' ' __stop$'; do
    grep -q "$sym" "$W/symbols.txt" || die "missing symbol: $sym"
done
FT=$(otool -h "$BIN" | awk 'NR>1{print $5}' | tr -d ' ' | tail -1)
[ "$FT" = "11" ] || die "filetype=$FT; expected 11 (MH_KEXT_BUNDLE)"
echo "  _kmod_info / _start / _stop present, MH_KEXT_BUNDLE OK"

if nm -u "$BIN" | c++filt | grep -E 'MPRController::|MPRBlockDev::' > "$W/unresolved.txt"; then
    echo "[error] unresolved symbols from the driver's own sources:" >&2
    cat "$W/unresolved.txt" >&2
    exit 1
fi
echo "  all driver-internal symbols defined: OK"

echo "== Info.plist =="
cp "$ROOT/MPR/Info.plist" "$KEXT/Contents/Info.plist"
plutil -lint "$KEXT/Contents/Info.plist" || die "Info.plist is not a valid plist"
if grep -E '\$\{|\$\(' "$KEXT/Contents/Info.plist" >/dev/null; then
    die "Info.plist contains unexpanded Xcode variables"
fi

echo "== Signing (ad hoc) =="
codesign --force --sign - --timestamp=none "$KEXT" >/dev/null || die "ad-hoc signing failed"
codesign --verify --strict "$KEXT" || die "signature verification failed"

printf '%s\n' "$KEXT" > "$ROOT/latest-noxcode-build.txt"
echo
echo "BUILD OK: $KEXT ($NAME $VERSION)"
echo "Nothing was deployed: /Library/Extensions, the ESP and kernel collections are untouched."
