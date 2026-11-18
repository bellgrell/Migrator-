# syntax=docker/dockerfile:1
FROM debian:bookworm AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ca-certificates \
      libmysqlclient-dev libpq-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_QT_GUI=OFF \
    && cmake --build build -j"$(nproc)"

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      libmysqlclient21 libpq5 libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -u 10001 -m -d /var/lib/migrator migrator

WORKDIR /opt/migrator
COPY --from=build /src/build/migrator /usr/local/bin/migrator
COPY config /opt/migrator/config
COPY web /opt/migrator/web

USER migrator
EXPOSE 8123
VOLUME ["/var/lib/migrator"]

ENTRYPOINT ["/usr/local/bin/migrator"]
CMD ["--mode", "sim", "--rows", "200000", "--tables", "4", \
     "--http-bind", "0.0.0.0", \
     "--http-token-file", "/run/secrets/dashboard_token", \
     "--tls-cert", "/run/tls/tls.crt", "--tls-key", "/run/tls/tls.key", \
     "--http-audit", "/var/lib/migrator/audit.log", \
     "--instance-lock", "/var/lib/migrator/migrator.lock"]
