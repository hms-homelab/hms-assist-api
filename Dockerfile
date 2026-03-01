# Stage 1: build
FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y \
    cmake g++ git libdrogon-dev libjsoncpp-dev \
    libcurl4-openssl-dev libyaml-cpp-dev \
    libpqxx-dev libpq-dev libgtest-dev libgmock-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)

# Stage 2: runtime
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y \
    libjsoncpp25 libcurl4 libyaml-cpp0.8 libpqxx-7.9 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/build/hms_assist /usr/local/bin/hms_assist
EXPOSE 8894
ENV HMS_CONFIG_PATH=/etc/hms-assist/config.yaml
ENTRYPOINT ["/usr/local/bin/hms_assist"]
