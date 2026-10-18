#!/usr/bin/env bash
# Cross-compile ReXGlue's FFmpeg submodule for the PS5: static libavcodec + libavutil with only
# the WMA Pro decoder and ReXGlue's XMA-frames decoder (the game's audio). Same configuration as
# the reference build (provider-batch9); configure runs in a shadow tree because FFmpeg refuses
# an out-of-tree build when the source tree already holds a config.h.
set -euo pipefail
W=/work
SDK=$W/PS5_Fable2/.deps/native/ps5-payload-sdk
FFMPEG=$W/ReXGlueSDK/thirdparty/FFmpeg
OUT=$W/PS5_Fable2/build/ps5/fable2-link-sweep/provider-batch9
SHADOW=$OUT/ffmpeg-source-shadow
BUILD=$OUT/ffmpeg-build-shadow
export PS5_PAYLOAD_SDK=$SDK

mkdir -p "$SHADOW" "$BUILD"
for entry in "$FFMPEG"/* "$FFMPEG"/.[!.]*; do
    name=$(basename "$entry")
    case $name in config.h|config.asm|config_components.h|config.mak) continue ;; esac
    [[ -e $SHADOW/$name ]] || ln -s "$entry" "$SHADOW/$name"
done
grep -q ff_xmaframes_decoder "$FFMPEG/libavcodec/wmaprodec.c"

cd "$BUILD"
sh "$SHADOW/configure" --target-os=freebsd --arch=x86_64 --enable-cross-compile \
    --cc="$SDK/bin/prospero-clang" --ld="$SDK/bin/prospero-clang" --ar="$SDK/bin/prospero-ar" \
    --ranlib="$SDK/bin/prospero-ranlib" --nm="$SDK/bin/prospero-nm" \
    --enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --disable-everything --enable-decoder=wmapro --disable-avdevice \
    --disable-avfilter --disable-avformat --disable-swscale --disable-swresample --disable-network \
    --disable-asm --disable-pthreads --disable-symver --extra-cflags='-O2 -fPIC' > configure.log

# ReXGlue's codec registration (adds the XMA-frames decoder that shares wmaprodec.c).
mkdir -p libavcodec
for f in codec_list.c parser_list.c bsf_list.c; do
    cp "$W/ReXGlueSDK/thirdparty/ffmpeg-overlay/$f" libavcodec/$f
done
target=config_components.h
[[ -f $target ]] || target=config.h
if grep -Eq '^[[:space:]]*#define[[:space:]]+CONFIG_XMAFRAMES_DECODER[[:space:]]+[01]' "$target"; then
    sed -Ei 's/^[[:space:]]*#define[[:space:]]+CONFIG_XMAFRAMES_DECODER[[:space:]]+[01]\b/#define CONFIG_XMAFRAMES_DECODER 1/' "$target"
else
    printf '\n#define CONFIG_XMAFRAMES_DECODER 1\n' >> "$target"
fi
grep -Eq '#define[[:space:]]+CONFIG_WMAPRO_DECODER[[:space:]]+1' config_components.h config.h 2>/dev/null

make -j"$(nproc)" > build.log
test -f libavcodec/libavcodec.a && test -f libavutil/libavutil.a
echo "ffmpeg: $(stat -c %s libavcodec/libavcodec.a) + $(stat -c %s libavutil/libavutil.a) bytes"
