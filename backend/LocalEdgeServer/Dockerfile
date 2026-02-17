FROM ubuntu:24.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libboost-system-dev \
        libssl-dev \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

RUN cmake -S LocalEdgeServer -B /tmp/build-localedge && \
    cmake --build /tmp/build-localedge --target localedge_server -j

EXPOSE 9440

CMD ["/tmp/build-localedge/localedge_server", "--host-id", "docker-host", "--hostindexer-host", "hostindexer", "--hostindexer-port", "9460", "--hostindexer-path", "/hostindexer", "--api-address", "0.0.0.0", "--api-port", "9440"]
