# Development Environment Setup

## System packages

These are needed to build libxayagame, xayax, and the roguelike GSP.
Run each block one at a time.

### Already installed
- build-essential, cmake, git, pkg-config, libssl-dev, zlib1g-dev

### Core libraries (apt)

```bash
sudo apt-get install -y \
  libjsoncpp-dev \
  libgoogle-glog-dev \
  libgflags-dev \
  libprotobuf-dev protobuf-compiler \
  libsqlite3-dev \
  liblmdb-dev \
  libzmq3-dev \
  libcurl4-openssl-dev \
  libmicrohttpd-dev \
  libsecp256k1-dev \
  autoconf automake libtool autoconf-archive
```

### Additional libraries for Xaya X (apt)

```bash
sudo apt-get install -y \
  libboost-all-dev \
  libunivalue-dev \
  libwebsocketpp-dev \
  libmariadb-dev
```

### Google Test (from source — Debian package is too old for libxayagame)

```bash
cd /tmp
git clone https://github.com/google/googletest.git --branch v1.14.0 --depth 1
cd googletest
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j$(nproc)
sudo cmake --install build
```

### cppzmq (C++ ZMQ bindings — Debian package is too old)

```bash
cd /tmp
git clone https://github.com/zeromq/cppzmq.git --branch v4.10.0 --depth 1
cd cppzmq
cmake -B build -DCPPZMQ_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
sudo cmake --install build
```

### libjson-rpc-cpp (Debian package is too old)

Needs `libargtable2-dev` for the stub generator:

```bash
sudo apt-get install -y libargtable2-dev
```

Then build with Redis disabled (we don't use it):

```bash
cd /tmp
git clone https://github.com/cinemast/libjson-rpc-cpp.git --depth 50
cd libjson-rpc-cpp
cmake -B build \
  -DCOMPILE_TESTS=OFF \
  -DCOMPILE_STUBGEN=ON \
  -DCOMPILE_EXAMPLES=OFF \
  -DREDIS_SERVER=NO \
  -DREDIS_CLIENT=NO \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j$(nproc)
sudo cmake --install build
sudo ldconfig
```

### eth-utils (Xaya-specific dependency — uses autotools, not CMake)

```bash
cd /tmp
git clone https://github.com/xaya/eth-utils.git --depth 1
cd eth-utils
./autogen.sh
./configure --prefix=/usr/local
make -j$(nproc)
sudo make install
sudo ldconfig
```

---

## Build libxayagame

```bash
cd ~/Explore/libxayagame
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j$(nproc)
sudo cmake --install build
sudo ldconfig
```

### Verify libxayagame is available

```bash
pkg-config --modversion libxayagame
# Should print 1.0.3 or similar
```

---

## WASM build environment (optional, for future browser channel client)

Not needed for the current GSP work — only relevant once we start
prototyping a browser-side channel client for off-chain dungeon visits.
Noted here so we don't forget it exists.

Upstream PR xaya/libxayagame#143 (merged 2026-04-10) adds a Dockerfile at
`wasm/docker/Dockerfile` that produces a self-contained image with
Emscripten plus all WASM-cross-compiled dependencies (OpenSSL, Protobuf,
jsoncpp, secp256k1, eth-utils) pre-installed into the Emscripten sysroot.
Replaces the previous manual cross-compilation dance for each dep.

Build the image:

```bash
cd ~/Explore/libxayagame
docker build -f wasm/docker/Dockerfile -t libxayagame-wasm \
  --build-arg N=$(nproc) .
```

Use it against a game project (mounts the project as `/game`):

```bash
docker run --rm -v $(pwd):/game libxayagame-wasm \
  bash -c "
    emcmake cmake -B /game/build /game \
      -DCMAKE_PREFIX_PATH=\${WASM_PREFIX} && \
    cmake --build /game/build
  "
```

`WASM_PREFIX` is set inside the image to the Emscripten sysroot, so
project CMake files don't need to hardcode paths.

The same PR also fixed a one-line bug in `wasm/XayaGameWasmConfig.cmake.in`
where the eth-utils library filename was wrong (`libeth-utils.a` →
`libethutils.a`), so consuming the WASM build via the provided CMake
config now actually links.

**When this matters for the roguelike:** the `channelcore` library is
designed to run in WASM frontends (see `gamechannel/README.md`), so when
we build the in-browser client for solo/co-op dungeon visits this image
is the recommended starting point.

---

## Build the roguelike GSP

Once libxayagame is installed, the full daemon will build:

```bash
cd ~/Projects/xayaroguelike
rm -rf build
cmake -B build
cmake --build build -j$(nproc)
```

### Run unit tests (no libxayagame needed)

```bash
cd ~/Projects/xayaroguelike
cmake -B build
cmake --build build -j$(nproc)
LD_LIBRARY_PATH=build/_deps/glog-build:build/_deps/jsoncpp-build/src/lib_json \
  ./build/roguelike_tests
```

---

## Xaya X (EVM bridge)

Xaya X lets the GSP run against an EVM chain (Polygon, local devnet, etc.)
instead of Xaya Core. It exposes a Xaya-Core-compatible JSON-RPC + ZMQ
interface so the GSP works unchanged.

### Install Foundry (provides anvil + forge)

Foundry provides `anvil` (local EVM node) and `forge` (Solidity compiler).

```bash
curl -L https://raw.githubusercontent.com/foundry-rs/foundry/master/foundryup/foundryup -o /tmp/foundryup
chmod +x /tmp/foundryup
/tmp/foundryup
```

If the install script fails to create `~/.foundry/bin/`, fix it manually:

```bash
mkdir -p ~/.foundry/bin
ln -s ~/.foundry/versions/stable/* ~/.foundry/bin/
```

Add to your PATH (add this to `~/.bashrc`):

```bash
export PATH="$HOME/.foundry/bin:$PATH"
```

Verify:

```bash
forge --version
anvil --version
```

### Build mypp (Xaya C++ template library)

```bash
cd /tmp
git clone https://github.com/xaya/mypp.git --depth 1
cd mypp
./autogen.sh
./configure --prefix=/usr/local
make -j$(nproc)
sudo make install
sudo ldconfig
```

### Build Xaya X

Clone (if not already done):

```bash
cd ~/Explore
git clone https://github.com/xaya/xayax.git --depth 50
cd xayax
git submodule update --init --recursive
```

Create a Python venv for the build tools and runtime:

```bash
cd ~/Explore/xayax
python3 -m venv .venv
source .venv/bin/activate
pip install web3==7
```

Configure and build:

```bash
./autogen.sh
./configure
```

Compile Solidity contracts:

```bash
make -C eth/solidity
```

Generate contract constants (requires web3 in the venv):

```bash
cd eth
python3 gen-contract-constants.py > contract-constants.cpp
cd ..
```

Build and install:

```bash
make -j$(nproc)
sudo env PATH="$PATH" make install
sudo ldconfig
```

**Important**: `forge` must be on your PATH when running both `make` and
`sudo make install`. The `sudo env PATH="$PATH"` trick passes your PATH
(including `~/.foundry/bin`) through to the sudo'd process. If you get
`forge: command not found`, run `export PATH="$HOME/.foundry/bin:$PATH"` first.

### Install the xayax Python package

With the venv still active:

```bash
cd ~/Explore/xayax
pip install -e .
```

This provides the `xayax.eth` module used for contract deployment and
test orchestration.

### Verify Xaya X

```bash
which xayax-eth
# Should print /usr/local/bin/xayax-eth
```

---

## Local devnet (end-to-end testing)

The smoke test runs the full stack locally: Anvil (local EVM chain) →
Xaya X bridge → rogueliked GSP. Everything is in-memory and self-contained.

### Run the smoke test

```bash
source ~/Explore/xayax/.venv/bin/activate
cd ~/Projects/xayaroguelike
python3 devnet/smoke_test.py
```

This will:
1. Start an Anvil node (local EVM, no-mining mode)
2. Deploy Xaya name contracts (WCHI, XayaAccounts, XayaPolicy)
3. Start xayax-eth bridge pointed at Anvil
4. Start rogueliked pointed at Xaya X
5. Register a player, discover a segment, verify state via RPC
6. Tear everything down

The test should complete in ~3 seconds and print `ALL SMOKE TESTS PASSED!`.
