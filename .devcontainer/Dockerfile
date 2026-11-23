# Development container for cometvisu-knx-fcgi.
#
# Provides everything needed for the full test suite and for KNXnet/IP work:
#   - C++20 toolchain: g++, clang, cmake, ninja, gdb
#   - FastCGI development files and the spawn-fcgi/cgi-fcgi tools
#   - knxd + knxd-dev for the knxd transport (eibtypes.h)
#   - networking tools to observe/debug KNXnet/IP traffic (UDP 3671,
#     multicast 224.0.23.12): iproute2, tcpdump, socat, netcat
#
# The base image already ships build-essential, cmake, gdb, git and the
# "vscode" user (uid 1000), which matches a typical Linux host user.
FROM mcr.microsoft.com/devcontainers/cpp:1-ubuntu-24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libfcgi-dev \
        libfcgi-bin \
        spawn-fcgi \
        ninja-build \
        clang \
        clang-format \
        clang-tidy \
        valgrind \
        knxd \
        knxd-dev \
        knxd-tools \
        iproute2 \
        tcpdump \
        socat \
        netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

# Sanity check at build time instead of in a broken first build.
RUN cmake --version && g++ --version | head -1 && clang++ --version | head -1 \
    && test -f /usr/include/eibtypes.h \
    && command -v cgi-fcgi && command -v knxd
