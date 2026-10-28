# 64-bit build environment for PSP.pak (minarch64 + ppsspp)
# matches the stock rg35xxplus aarch64 userland: Ubuntu 22.04, glibc 2.35
FROM --platform=linux/arm64 ubuntu:22.04@sha256:e0f80c829509ef4df9e76bd8944eb7b8ec25cbb379b112a7f9448b2c77a77c9b

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
		build-essential git cmake pkg-config python3 zip file xz-utils ca-certificates curl \
		autoconf automake libtool \
		libsdl2-image-dev libsdl2-ttf-dev libgles-dev libegl-dev libasound2-dev zlib1g-dev \
		libegl-mesa0 libgl1-mesa-dri \
	&& rm -rf /var/lib/apt/lists/*

ENV CROSS_COMPILE=/usr/bin/
ENV PREFIX=/opt/psp64
ENV UNION_PLATFORM=rg35xxplus
RUN mkdir -p $PREFIX/include $PREFIX/lib

WORKDIR /root/workspace
