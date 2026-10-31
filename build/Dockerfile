# The whole toolchain. Native arm64 or amd64, cross-compiling to armhf: the
# SC7731G is a 32-bit Cortex-A7. debugfs (e2fsprogs) reads the Android
# system image for the firmware step without mounting it.
FROM debian:trixie-20250929

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential gcc-arm-linux-gnueabihf bc bison flex cpio kmod \
      libssl-dev libgnutls28-dev uuid-dev python3 python3-dev python3-setuptools \
      python3-pyelftools swig device-tree-compiler u-boot-tools \
      libusb-1.0-0-dev zlib1g-dev libfdt-dev pkg-config \
      ca-certificates wget xz-utils bzip2 git \
      dosfstools mtools e2fsprogs fdisk zstd \
    && rm -rf /var/lib/apt/lists/*

ENV ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
WORKDIR /work
