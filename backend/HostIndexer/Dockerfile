FROM ubuntu:24.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libboost-all-dev \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY HostIndexer /app/HostIndexer
COPY shared /app/shared

WORKDIR /app/HostIndexer
RUN cmake -S . -B /tmp/build-hostindexer && \
    cmake --build /tmp/build-hostindexer --target host_indexer_cli -j

EXPOSE 9460

CMD ["/tmp/build-hostindexer/host_indexer_cli", "--serve", "--db", "/data/hostindex", "--listen-address", "0.0.0.0", "--listen-port", "9460", "--ws-path", "/hostindexer", "--default-root", "/app/HostIndexer"]
