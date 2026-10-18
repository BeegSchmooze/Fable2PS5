# Fable 2 for PS5 - build environment.
#
#   docker build -t fable2-ps5 .                      (once; about an hour, mostly Mesa/RADV)
#   docker run --rm -v "$PWD/game:/game:ro" -v "$PWD/out:/out" fable2-ps5
#
# /game must hold your own default.xex (and optionally cover.png). The finished title folder
# is written to out/PPSA99124/. See README.md.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

# Packages come from Ubuntu's snapshot archive as it was when the reference build was made, so
# the compilers are the exact versions the build recipe was verified with (the recompiled
# objects are reproduced byte for byte).
ARG UBUNTU_SNAPSHOT=20261015T140000Z
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
 && apt-get update --snapshot "$UBUNTU_SNAPSHOT" \
 && apt-get install -y --no-install-recommends --snapshot "$UBUNTU_SNAPSHOT" \
      build-essential git wget curl unzip zip zstd rsync make cmake ninja-build pkg-config \
      clang clang-18 clang-19 llvm llvm-18 llvm-18-dev llvm-19 llvm-19-dev lld lld-18 \
      libclang-rt-18-dev libclang-rt-19-dev \
      libc++-18-dev libc++abi-18-dev libclang-18-dev libclang-cpp18-dev libclc-18 libclc-18-dev \
      libllvmspirvlib-18-dev llvm-spirv-18 spirv-tools glslang-tools \
      libdrm-dev libudev-dev libwayland-dev libx11-xcb-dev \
      python3 python3-mako python3-numpy python3-pil python3-pip python3-venv python3-yaml \
 && rm -rf /var/lib/apt/lists/*

# Mesa's build tools, at the versions RADV was built with.
RUN python3 -m venv --system-site-packages /opt/ps5-venv \
 && /opt/ps5-venv/bin/pip install --no-cache-dir meson==1.12.1 ninja==1.13.2

# Toolchain and dependencies from pinned sources (RADV, payload SDK, FFmpeg, rexglue host tool).
COPY docker/ /work/fable2-ps5/docker/
COPY patches/ /work/fable2-ps5/patches/
COPY overlay/ /work/fable2-ps5/overlay/
RUN bash /work/fable2-ps5/docker/setup-toolchain.sh

# The port: recipe, transforms, port sources, packaging.
COPY . /work/fable2-ps5/
RUN cp -a /work/fable2-ps5/overlay/. /work/ && test -f /work/fable2-ps5/recomp/transform.py

WORKDIR /work
ENTRYPOINT ["python3", "/work/fable2-ps5/tools/build_port.py"]
