# Build image for cross-compiling Qt 5.15 and apps for PocketBook (SDK 6.5, B288 toolchain).
# The SDK and the sources are mounted at /work, not copied into the image.
# Also carries Debian's ARM softfloat GCC 12 (arm-linux-gnueabi) to pair a modern compiler with
# the SDK's sysroot, the way kobuild does for Kobo.
FROM debian:bookworm-slim
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential perl python3 cmake ninja-build git xz-utils file pkg-config ca-certificates \
        patch autoconf automake libtool \
        libmpfr6 libmpc3 libgmp10 \
        gcc-arm-linux-gnueabi g++-arm-linux-gnueabi binutils-arm-linux-gnueabi \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s libmpfr.so.6 /usr/lib/x86_64-linux-gnu/libmpfr.so.4
WORKDIR /work
