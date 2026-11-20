#!/bin/bash
# android/deps/build_gen_deps.sh: what the app needs to make the game code on the phone itself (docs/las/ANDROID-PORT.md,
# Phase 6) - the generator (tools/gen.py) runs on Python for Android, the compiler is zig - from pinned sources into
# android/deps/out/gen for arm64-v8a:
#   python/        the official CPython for Android (python.org's aarch64-linux-android package): libpython and its
#                  standard library, plus pymain (a launcher: an executable the app runs, as libpymain.so)
#   lib/           the generator's native libraries, built with the NDK: capstone (ARM64 only), unicorn (aarch64
#                  only) and lz4 - their Python bindings load them with ctypes
#   site/          those bindings: capstone's and unicorn's own (pure Python), lz4.block from android/gen/lz4
#   zig/           zig for aarch64 Linux (a static binary: it runs on Android) with only what compiling the game's C
#                  for aarch64 needs (its clang headers, musl's libc headers, its std for compiler_rt)
# Runs in Git Bash on Windows (or any bash) with the Android SDK's NDK and CMake; each part is skipped when its output
# exists (delete out/gen to redo).
set -euo pipefail
cd "$(dirname "$0")"
D=$PWD
S=$D/src
P=$D/out/gen
W=$D/work
mkdir -p "$S" "$P/lib" "$W"

SDK=${ANDROID_HOME:-${LOCALAPPDATA:-$HOME}/Android/Sdk}
SDK=${SDK//\\//}
NDK=${ANDROID_NDK:-$SDK/ndk/30.0.16248370}
CMAKE_DIR=$SDK/cmake/3.30.4/bin
API=30
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=.exe; HOST=windows-x86_64 ;; Darwin) EXE=; HOST=darwin-x86_64 ;; *) EXE=; HOST=linux-x86_64 ;; esac
CMAKE=$CMAKE_DIR/cmake$EXE
ANDROID_CMAKE=(-G Ninja -DCMAKE_MAKE_PROGRAM="$CMAKE_DIR/ninja$EXE" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
               -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release
               -DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384)
CC=$NDK/toolchains/llvm/prebuilt/$HOST/bin/clang$EXE
STRIP=$NDK/toolchains/llvm/prebuilt/$HOST/bin/llvm-strip$EXE

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || { curl -fsSL -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"; }
    (cd "$S" && echo "$2  $3" | sha256sum -c --quiet)
}
unpack() { rm -rf "$W/$2" && mkdir -p "$W/$2" && (cd "$S" && tar --force-local -xf "$1" -C "$W/$2" --strip-components 1 2>/dev/null || true); }

fetch https://www.python.org/ftp/python/3.14.8/python-3.14.8-aarch64-linux-android.tar.gz \
      11324313957da3736f44e90257304d288864321a4dd376ed0cc35a54eacf9a33 python-3.14.8-aarch64-linux-android.tar.gz
fetch https://github.com/capstone-engine/capstone/archive/refs/tags/5.0.9.tar.gz \
      0619da31af08152600af95c481527ef6d756c0a8404fca7544a4fdf6dfc2c0f9 capstone-5.0.9.tar.gz
fetch https://github.com/unicorn-engine/unicorn/archive/refs/tags/2.1.4.tar.gz \
      ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4 unicorn-2.1.4.tar.gz
fetch https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz \
      537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b lz4-1.10.0.tar.gz
fetch https://ziglang.org/download/0.17.0/zig-aarch64-linux-0.17.0.tar.xz \
      9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8 zig-aarch64-linux-0.17.0.tar.xz

# Python: libpython + the standard library (no tests, no idle / tkinter / ensurepip), and the launcher
if [ ! -f "$P/python/libpymain.so" ]; then
    rm -rf "$W/py" && mkdir -p "$W/py" && (cd "$S" && tar --force-local -xzf python-3.14.8-aarch64-linux-android.tar.gz -C "$W/py" 2>/dev/null || true)
    rm -rf "$P/python" && mkdir -p "$P/python/lib"
    cp "$W/py/prefix/lib/libpython3.14.so" "$P/python/"
    cp -r "$W/py/prefix/lib/python3.14" "$P/python/lib/"
    rm -rf "$P/python/lib/python3.14/"{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3,__pycache__} "$P/python/lib/python3.14/site-packages"
    find "$P/python/lib" -name __pycache__ -type d -prune -exec rm -rf {} +
    printf '#include <Python.h>\nint main(int argc, char **argv) { return Py_BytesMain(argc, argv); }\n' > "$W/pymain.c"
    "$CC" --target=aarch64-linux-android$API -O2 "$W/pymain.c" -I"$W/py/prefix/include/python3.14" -L"$P/python" -lpython3.14 \
        -Wl,-rpath,'$ORIGIN' -Wl,-z,max-page-size=16384 -o "$P/python/libpymain.so"
    cp "$W/py/prefix/../README.md" "$P/python/README-python-android.md" 2>/dev/null || true
fi

# capstone: ARM64 only
if [ ! -f "$P/lib/libcapstone.so" ]; then
    unpack capstone-5.0.9.tar.gz capstone
    "$CMAKE" -S "$W/capstone" -B "$W/capstone/build" "${ANDROID_CMAKE[@]}" -DBUILD_SHARED_LIBS=ON \
        -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_ARM64_SUPPORT=ON
    "$CMAKE" --build "$W/capstone/build"
    cp "$W/capstone/build/libcapstone.so" "$P/lib/"
fi

# unicorn: aarch64 only - its build runs QEMU's configure (a Unix shell script): android/deps/build_unicorn.sh, in WSL
# on Windows
if [ ! -f "$P/lib/libunicorn.so" ]; then
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) echo "libunicorn.so: run  wsl -d Ubuntu-26.04 -- bash android/deps/build_unicorn.sh  first"; exit 1 ;;
        *) bash "$D/build_unicorn.sh" ;;
    esac
fi
[ -d "$W/unicorn/bindings" ] || unpack unicorn-2.1.4.tar.gz unicorn  # (its Python bindings, below)

# lz4
if [ ! -f "$P/lib/liblz4.so" ]; then
    unpack lz4-1.10.0.tar.gz lz4
    "$CMAKE" -S "$W/lz4/build/cmake" -B "$W/lz4/b" "${ANDROID_CMAKE[@]}" -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DLZ4_BUILD_CLI=OFF
    "$CMAKE" --build "$W/lz4/b"
    cp "$(ls "$W"/lz4/b/liblz4.so* | head -1)" "$P/lib/liblz4.so"
fi
for f in "$P"/lib/*.so; do "$STRIP" --strip-unneeded "$f"; done

# the bindings: capstone's and unicorn's (pure Python, ctypes), lz4.block (android/gen/lz4: the decompress gen.py uses)
if [ ! -d "$P/site/unicorn" ]; then
    rm -rf "$P/site" && mkdir -p "$P/site"
    cp -r "$W/capstone/bindings/python/capstone" "$P/site/"
    cp -r "$W/unicorn/bindings/python/unicorn" "$P/site/"
    cp -r "$D/../gen/lz4" "$P/site/"
    find "$P/site" \( -name '*.so' -o -name '*.dll' -o -name '*.dylib' -o -name __pycache__ \) -prune -exec rm -rf {} +
fi

# zig: the binary, its clang headers, musl's headers for aarch64 and its std (compiler_rt is built from it)
if [ ! -f "$P/zig/libzig.so" ]; then
    rm -rf "$W/zig" && mkdir -p "$W/zig" && (cd "$S" && tar --force-local -xJf zig-aarch64-linux-0.17.0.tar.xz -C "$W/zig" --strip-components 1)
    rm -rf "$P/zig" && mkdir -p "$P/zig/lib/libc/include"
    cp "$W/zig/zig" "$P/zig/libzig.so"
    cp -r "$W/zig/lib/include" "$W/zig/lib/std" "$W/zig/lib/compiler_rt" "$P/zig/lib/"
    cp "$W/zig/lib/compiler_rt.zig" "$W/zig/lib/c.zig" "$P/zig/lib/"
    for d in aarch64-linux-musl aarch64-linux-any any-linux-any generic-musl; do cp -r "$W/zig/lib/libc/include/$d" "$P/zig/lib/libc/include/"; done
    cp "$W/zig/LICENSE" "$P/zig/LICENSE-zig.txt"
fi
echo "android/deps/out/gen: $(du -sh "$P" | cut -f1)"
ls -la "$P/lib" "$P/python" | head -20
