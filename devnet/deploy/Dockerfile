# Runs the full Xaya Roguelike devnet sandbox (anvil + xayax + GSP + move
# proxy) inside Ubuntu 24.04, matching the toolchain the Xaya stack is built
# and tested against (GCC 13 / CMake 3.28). This isolates the build from newer
# host toolchains (e.g. GCC 15 / CMake 4) under which the pinned Xaya sources
# do not compile, so the same image runs identically on a dev box and on a
# server regardless of the host distro.
#
# Every upstream component is pinned to a known-good commit / release.
#
# The build context is this directory only (nothing is COPYed in; all sources
# are cloned), so build with a tiny context:
#
#   docker build -t rog-sandbox devnet/deploy
#   docker run --rm rog-sandbox /opt/xayax/.venv/bin/python3 devnet/smoke_test.py   # validate
#   docker run -d --name rog -p 127.0.0.1:18380:18380 rog-sandbox                   # serve
#
# Caddy + cloudflared on the host stay unchanged; they point at the
# container's 127.0.0.1:18380.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# --- Core + Xaya X apt dependencies (see docs/SETUP.md) ---
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates curl \
      autoconf automake libtool autoconf-archive pkg-config \
      libjsoncpp-dev libgoogle-glog-dev libgflags-dev \
      libprotobuf-dev protobuf-compiler python3-protobuf \
      libsqlite3-dev liblmdb-dev libzmq3-dev libcurl4-openssl-dev \
      libmicrohttpd-dev libsecp256k1-dev libssl-dev zlib1g-dev \
      libboost-all-dev libunivalue-dev libwebsocketpp-dev libmariadb-dev \
      libargtable2-dev \
      python3 python3-venv python3-dev python3-pip \
    && rm -rf /var/lib/apt/lists/*

# --- googletest (source; Debian package too old for libxayagame) ---
RUN git clone https://github.com/google/googletest.git --branch v1.14.0 --depth 1 /tmp/gtest \
    && cmake -S /tmp/gtest -B /tmp/gtest/build -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /tmp/gtest/build -j"$(nproc)" && cmake --install /tmp/gtest/build \
    && rm -rf /tmp/gtest && ldconfig

# --- cppzmq (source; Debian package too old) ---
RUN git clone https://github.com/zeromq/cppzmq.git --branch v4.10.0 --depth 1 /tmp/cppzmq \
    && cmake -S /tmp/cppzmq -B /tmp/cppzmq/build -DCPPZMQ_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /tmp/cppzmq/build -j"$(nproc)" && cmake --install /tmp/cppzmq/build \
    && rm -rf /tmp/cppzmq && ldconfig

# --- libjson-rpc-cpp (source, Redis off; matches libxayagame's pinned tag) ---
RUN git clone https://github.com/cinemast/libjson-rpc-cpp.git --branch v1.4.1 --depth 1 /tmp/jsonrpc \
    && cmake -S /tmp/jsonrpc -B /tmp/jsonrpc/build \
        -DCOMPILE_TESTS=OFF -DCOMPILE_STUBGEN=ON -DCOMPILE_EXAMPLES=OFF \
        -DREDIS_SERVER=NO -DREDIS_CLIENT=NO -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /tmp/jsonrpc/build -j"$(nproc)" && cmake --install /tmp/jsonrpc/build \
    && rm -rf /tmp/jsonrpc && ldconfig

# --- eth-utils (pinned) ---
ARG ETHUTILS_COMMIT=ece89a90a62f406deeb8dc25534b94fc091438f4
RUN git clone https://github.com/xaya/eth-utils.git /tmp/ethutils \
    && cd /tmp/ethutils && git checkout ${ETHUTILS_COMMIT} \
    && ./autogen.sh && ./configure --prefix=/usr/local && make -j"$(nproc)" && make install \
    && cd / && rm -rf /tmp/ethutils && ldconfig

# --- mypp (pinned) ---
ARG MYPP_COMMIT=c3895a6b157d1cb68e9d9c6835ec11b01278312c
RUN git clone https://github.com/xaya/mypp.git /tmp/mypp \
    && cd /tmp/mypp && git checkout ${MYPP_COMMIT} \
    && ./autogen.sh && ./configure --prefix=/usr/local && make -j"$(nproc)" && make install \
    && cd / && rm -rf /tmp/mypp && ldconfig

# --- libxayagame (pinned to the dev box's commit) ---
ARG LIBXAYAGAME_COMMIT=81249f1f93342f4005993ac3b64341a3cca8d7ae
RUN git clone https://github.com/xaya/libxayagame.git /opt/libxayagame \
    && cd /opt/libxayagame && git checkout ${LIBXAYAGAME_COMMIT} \
    && cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build build -j"$(nproc)" && cmake --install build \
    && ldconfig

# --- Foundry (anvil + forge), pinned release ---
ARG FOUNDRY_RELEASE=nightly-1186156961f648273299ac067d098ee0e33a0d7e
RUN curl -L "https://github.com/foundry-rs/foundry/releases/download/${FOUNDRY_RELEASE}/foundry_nightly_linux_amd64.tar.gz" \
      | tar zxv -C /usr/local/bin

# --- xayax (pinned): build the xayax-eth binary + its Python venv ---
ARG XAYAX_COMMIT=0e07223556664f66bbf401e646a99020b6821d3e
RUN git clone https://github.com/xaya/xayax.git /opt/xayax \
    && cd /opt/xayax && git checkout ${XAYAX_COMMIT} \
    && git submodule update --init --recursive \
    && python3 -m venv .venv \
    && .venv/bin/pip install --upgrade pip \
    && .venv/bin/pip install web3==7 \
    && ./autogen.sh && ./configure \
    && make -C eth/solidity \
    && ( cd eth && ../.venv/bin/python3 gen-contract-constants.py > contract-constants.cpp ) \
    && make -j"$(nproc)" && make install && ldconfig \
    && .venv/bin/pip install -e .

# --- Roguelike GSP: with libxayagame present, the daemon (rogueliked) builds ---
RUN git clone https://github.com/EdwardAThomson/xaya-roguelike.git /opt/xayaroguelike \
    && cd /opt/xayaroguelike && cmake -B build && cmake --build build -j"$(nproc)" \
    && test -x build/rogueliked

ENV ROG_PROJECT=/opt/xayaroguelike \
    ROG_VENV=/opt/xayax/.venv \
    ROG_REQUIRE_CLAIM_TOKEN=1
EXPOSE 18380
WORKDIR /opt/xayaroguelike
CMD ["/opt/xayaroguelike/devnet/run_sandbox.sh"]
