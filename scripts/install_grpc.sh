#!/bin/bash

git clone -b v1.44.0 https://github.com/grpc/grpc

pushd grpc
git submodule init && git submodule update --recursive 
mkdir -p cmake/build && pushd cmake/build

cmake ../..   -DgRPC_INSTALL=ON              \
              -DCMAKE_BUILD_TYPE=Release       \
              -DgRPC_ABSL_PROVIDER=module     \
              -DgRPC_CARES_PROVIDER=module    \
              -DgRPC_PROTOBUF_PROVIDER=module \
              -DgRPC_RE2_PROVIDER=module      \
              -DgRPC_SSL_PROVIDER=package      \
              -DgRPC_ZLIB_PROVIDER=package

make -j$(nproc) && sudo make install && sudo ldconfig
popd

pushd third_party/protobuf
git submodule update --init --recursive
./autogen.sh && ./configure --prefix=/usr
make -j$(nproc) && sudo make install && sudo ldconfig
popd

rm -rf grpc
