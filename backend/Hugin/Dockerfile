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
COPY Hugin/vcpkg.json /tmp/Hugin-manifest/vcpkg.json
RUN --mount=type=cache,id=vcpkg-downloads,target=/opt/vcpkg/downloads,sharing=locked \
    --mount=type=cache,id=vcpkg-packages,target=/opt/vcpkg/packages,sharing=locked \
    --mount=type=cache,id=hugin-vcpkg-buildtrees,target=/opt/vcpkg/buildtrees,sharing=locked \
    --mount=type=cache,id=vcpkg-bincache,target=/opt/vcpkg/bincache,sharing=locked \
    /opt/vcpkg/vcpkg install --triplet "${VCPKG_DEFAULT_TRIPLET}" --x-manifest-root=/tmp/Hugin-manifest

COPY Hugin /app/Hugin
COPY Muninn /app/Muninn
COPY shared /app/shared

RUN --mount=type=cache,id=vcpkg-downloads,target=/opt/vcpkg/downloads,sharing=locked \
    --mount=type=cache,id=vcpkg-packages,target=/opt/vcpkg/packages,sharing=locked \
    --mount=type=cache,id=hugin-vcpkg-buildtrees,target=/opt/vcpkg/buildtrees,sharing=locked \
    --mount=type=cache,id=vcpkg-bincache,target=/opt/vcpkg/bincache,sharing=locked \
    cmake --preset dev-release -S Hugin && \
    cmake --build /app/Hugin/build/dev-release --target hugin hugin_tests -j && \
    ctest --test-dir /app/Hugin/build/dev-release --output-on-failure

EXPOSE 9440

CMD ["/app/Hugin/build/dev-release/hugin", "--host-id", "docker-host", "--muninn-host", "muninn", "--muninn-port", "9460", "--muninn-path", "/muninn", "--api-address", "0.0.0.0", "--api-port", "9440"]
