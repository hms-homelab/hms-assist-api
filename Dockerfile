# Stage 1: build
FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make g++ git ca-certificates \
    libjsoncpp-dev libssl-dev uuid-dev zlib1g-dev \
    libcurl4-openssl-dev libyaml-cpp-dev \
    libpqxx-dev libpq-dev libgtest-dev libgmock-dev \
    && rm -rf /var/lib/apt/lists/*

# Build Drogon from source (not packaged in Debian repos)
RUN git clone --depth 1 --recurse-submodules \
    https://github.com/drogonframework/drogon.git /drogon \
    && cmake -B /drogon/build /drogon \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_CTL=OFF \
        -DBUILD_TESTING=OFF \
    && cmake --build /drogon/build -j$(nproc) \
    && cmake --install /drogon/build \
    && rm -rf /drogon

WORKDIR /build
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)

# Stage 2: runtime
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libjsoncpp25 libcurl4 libssl3 libuuid1 zlib1g libpq5 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /usr/lib/x86_64-linux-gnu/libyaml-cpp* /usr/lib/x86_64-linux-gnu/
COPY --from=builder /usr/lib/x86_64-linux-gnu/libpqxx* /usr/lib/x86_64-linux-gnu/
COPY --from=builder /usr/local/lib/libdrogon* /usr/local/lib/
COPY --from=builder /usr/local/lib/libtrantor* /usr/local/lib/
RUN ldconfig
COPY --from=builder /build/build/hms_assist /usr/local/bin/hms_assist
EXPOSE 8894
ENV HMS_CONFIG_PATH=/etc/hms-assist/config.yaml
ENTRYPOINT ["/usr/local/bin/hms_assist"]
