# syntax=docker/dockerfile:1
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config ca-certificates \
      libmysqlclient-dev libpq-dev unixodbc-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . /src
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)"

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl gnupg unzip \
      libmysqlclient21 libpq5 libodbc2 libodbcinst2 unixodbc-common libssl3t64 libaio1t64 \
    && rm -rf /var/lib/apt/lists/*

# Microsoft ODBC Driver 18 + bcp
RUN curl -fsSL https://packages.microsoft.com/keys/microsoft.asc \
      | gpg --dearmor -o /usr/share/keyrings/microsoft-prod.gpg \
 && echo "deb [arch=amd64 signed-by=/usr/share/keyrings/microsoft-prod.gpg] https://packages.microsoft.com/ubuntu/24.04/prod noble main" \
      > /etc/apt/sources.list.d/mssql-release.list \
 && apt-get update \
 && ACCEPT_EULA=Y apt-get install -y --no-install-recommends msodbcsql18 mssql-tools18 \
 && rm -rf /var/lib/apt/lists/*

# Oracle Instant Client (basic/sdk/odbc/tools)
RUN mkdir -p /opt/oracle && cd /opt/oracle \
 && for f in basic sdk odbc tools; do \
      curl -sSLO "https://download.oracle.com/otn_software/linux/instantclient/instantclient-$f-linuxx64.zip"; \
    done \
 && for z in *.zip; do unzip -qo "$z"; done && rm -f *.zip \
 && IC=$(ls -d /opt/oracle/instantclient_* | head -1) \
 && ln -sfn "$IC" /opt/oracle/instantclient \
 && printf '[Oracle 23 ODBC Driver]\nDriver=%s/libsqora.so.23.1\nSetup=%s/libsqoras.so.23.1\n' "$IC" "$IC" >> /etc/odbcinst.ini

RUN ln -sf /usr/lib/x86_64-linux-gnu/libaio.so.1t64 /usr/lib/x86_64-linux-gnu/libaio.so.1
ENV LD_LIBRARY_PATH=/opt/oracle/instantclient
ENV ODBCSYSINI=/etc
ENV MIG_BCP=/opt/mssql-tools18/bin/bcp
ENV MIG_SQLLDR=/opt/oracle/instantclient/sqlldr
ENV MIG_PROVIDERS=/etc/mini-migrator/providers.ini

COPY --from=build /src/build/migrator /usr/local/bin/mini-migrator
COPY config/providers.ini /etc/mini-migrator/providers.ini

ENTRYPOINT ["mini-migrator"]
CMD ["doctor"]
