# syntax=docker/dockerfile:1.7

FROM ubuntu:24.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        curl \
        git \
        ninja-build \
        pkg-config \
        tar \
        unzip \
        zip \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_BINARY_SOURCES=clear;files,/opt/vcpkg/bincache,readwrite \
    VCPKG_DEFAULT_TRIPLET=x64-linux
RUN git clone --depth 1 https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}" && \
    "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /app
COPY Muninn/vcpkg.json /tmp/Muninn-manifest/vcpkg.json
RUN --mount=type=cache,id=vcpkg-downloads,target=/opt/vcpkg/downloads,sharing=locked \
    --mount=type=cache,id=vcpkg-packages,target=/opt/vcpkg/packages,sharing=locked \
    --mount=type=cache,id=muninn-vcpkg-buildtrees,target=/opt/vcpkg/buildtrees,sharing=locked \
    --mount=type=cache,id=vcpkg-bincache,target=/opt/vcpkg/bincache,sharing=locked \
    /opt/vcpkg/vcpkg install --triplet "${VCPKG_DEFAULT_TRIPLET}" --x-manifest-root=/tmp/Muninn-manifest

COPY Muninn /app/Muninn
COPY shared /app/shared

WORKDIR /app/Muninn
RUN --mount=type=cache,id=vcpkg-downloads,target=/opt/vcpkg/downloads,sharing=locked \
    --mount=type=cache,id=vcpkg-packages,target=/opt/vcpkg/packages,sharing=locked \
    --mount=type=cache,id=muninn-vcpkg-buildtrees,target=/opt/vcpkg/buildtrees,sharing=locked \
    --mount=type=cache,id=vcpkg-bincache,target=/opt/vcpkg/bincache,sharing=locked \
    cmake --preset dev-release -S . && \
    cmake --build /app/Muninn/build/dev-release --target muninn_cli muninn_core_tests -j && \
    ctest --test-dir /app/Muninn/build/dev-release --output-on-failure

VOLUME ["/data/watched", "/data/hostindex"]

EXPOSE 9460

CMD ["/app/Muninn/build/dev-release/muninn_cli", "--serve", "--db", "/data/hostindex", "--listen-address", "0.0.0.0", "--listen-port", "9460", "--ws-path", "/muninn", "--default-root", "/data/watched", "--watch-interval-seconds", "2"]
