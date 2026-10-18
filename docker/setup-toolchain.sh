#!/usr/bin/env bash
# Build everything the port compiles and links against, from pinned public sources, into /work
# (the directory layout the recorded build recipe expects). Runs once, at `docker build`.
#
#   ReXGlue SDK      recompiler + emulator runtime, with this port's patch (patches/rexglue-sdk.patch)
#   Fable-2-Recomp   the recompilation project (manifest, app, game-specific code)
#   PS5_PayloadSDK   payload SDK fork (two pins: PS5_Vulkan's and the port's)
#   PS5_Mesa         RADV for the PS5, built by PS5_Vulkan's tools/build-radv.sh release
#   PS5_Vulkan       link recipe, libc.prx, the ELF -> signed eboot tool
#   FFmpeg           ReXGlue's submodule, WMA Pro/XMA decoders only, cross-compiled for the PS5
set -euo pipefail
W=/work
REPO=$W/fable2-ps5
# shellcheck source=pins.sh
source "$REPO/docker/pins.sh"

clone() {  # dir url commit
    git clone --quiet "$2" "$W/$1"
    git -C "$W/$1" checkout --quiet "$3"
}

echo "==> sources"
clone ReXGlueSDK "$REXGLUE_URL" "$REXGLUE_COMMIT"
git -C "$W/ReXGlueSDK" submodule update --init --recursive --quiet
echo "==> rexglue host tool (codegen), clang 19, from the unpatched SDK"
# Built before the PS5 patch, as in the reference environment: the patch makes parts of the
# runtime PS5-only. Codegen output does not depend on it (one header line is fixed by the transform).
cmake -S "$W/ReXGlueSDK" -B "$W/ReXGlueSDK/build-host-codegen19" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/usr/bin/clang-19 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-19 -DCMAKE_CXX_FLAGS=-mssse3 \
    -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_ENABLE_FIDELITYFX=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF \
    -DREXGLUE_ENABLE_SANITIZERS=OFF -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_USE_VULKAN=ON > /tmp/rexglue-configure.log 2>&1 ||
    { tail -60 /tmp/rexglue-configure.log; exit 1; }
cmake --build "$W/ReXGlueSDK/build-host-codegen19" --target rexglue > /tmp/rexglue-build.log 2>&1 ||
    { grep -n -B2 -A8 'error:' /tmp/rexglue-build.log | head -80; tail -20 /tmp/rexglue-build.log; exit 1; }
# The port compiles against the reference build's version header (it carries a timestamp).
cp "$REPO/overlay/ReXGlueSDK/build-host-codegen19/include/rex/version.h" \
    "$W/ReXGlueSDK/build-host-codegen19/include/rex/version.h"
test -x "$W/ReXGlueSDK/out/linux-amd64/rexglue"
git -C "$W/ReXGlueSDK" apply --whitespace=nowarn "$REPO/patches/rexglue-sdk.patch"
clone Fable2Recomp "$FABLE2RECOMP_URL" "$FABLE2RECOMP_COMMIT"
clone PS5_PayloadSDK "$PAYLOADSDK_URL" "$PAYLOADSDK_COMMIT"
clone PS5_Mesa "$MESA_URL" "$MESA_COMMIT"
clone PS5_Vulkan "$PS5VULKAN_URL" "$PS5VULKAN_COMMIT"

# The port's own files at their recorded paths (sources, linker inputs, SDK setup script).
cp -a "$REPO/overlay/." "$W/"

# Everything below was built with clang 18 in the reference environment.
export LLVM_CONFIG=/usr/bin/llvm-config-18
export PATH=/opt/ps5-venv/bin:$PATH

echo "==> PS5_Vulkan: payload SDK ($PS5VULKAN_SDK_REVISION), zlib, RADV release"
(cd "$W/PS5_Vulkan" && bash tools/setup-native-dependencies.sh && bash tools/build-radv.sh release)

echo "==> PS5_Vulkan: clean-room libc.prx (rebuilt twice, checked against its published digest)"
(cd "$W/PS5_Vulkan" && bash tools/rebuild-libc.sh)

echo "==> PS5_Vulkan: ps5-native-tool"
native=$W/PS5_Vulkan/tooling/native
zlib_root=$W/PS5_Vulkan/.deps/native/zlib/root
mkdir -p "$W/PS5_Vulkan/build/host"
clang++-18 -std=c++20 -O2 -Wall -Wextra -Werror -I "$zlib_root/usr/include" \
    "$native/native_app_builder.cpp" "$native/self_container.cpp" "$native/elf_object.cpp" \
    "$native/sce_module_writer.cpp" "$(find "$zlib_root" -type f -name libz.a -print -quit)" \
    -o "$W/PS5_Vulkan/build/host/ps5-native-tool"

echo "==> port payload SDK ($PORT_SDK_REVISION)"
bash "$W/PS5_Fable2/ps5/tools/setup-sdk.sh"

echo "==> FFmpeg (WMA Pro + XMA frames, static, PS5)"
bash "$REPO/docker/build-ffmpeg.sh"


echo "==> toolchain ready"
