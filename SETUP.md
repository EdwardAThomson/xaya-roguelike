# Development Environment Setup

## System packages

These are needed to build libxayagame and the roguelike GSP.
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

## Build libxayagame

```bash
cd ~/Explore/libxayagame
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j$(nproc)
sudo cmake --install build
sudo ldconfig
```

## Verify libxayagame is available

```bash
pkg-config --modversion libxayagame
# Should print 1.0.3 or similar
```

## Build the roguelike GSP (full build)

Once libxayagame is installed, the full daemon will build:

```bash
cd ~/Projects/xayaroguelike
rm -rf build
cmake -B build
cmake --build build -j$(nproc)
```

## Run tests (works now, no libxayagame needed)

```bash
cd ~/Projects/xayaroguelike
cmake -B build
cmake --build build -j$(nproc)
LD_LIBRARY_PATH=build/_deps/glog-build:build/_deps/jsoncpp-build/src/lib_json \
  ./build/roguelike_tests
```
