#!/bin/sh
set -eu
cd "$(dirname "$0")"
pkg=restate-server-x86_64-unknown-linux-musl
if [ ! -x bin/restate-server ]; then
  mkdir -p bin
  curl -sSLf -o bin/$pkg.tar.xz https://github.com/restatedev/restate/releases/download/v1.8.1/$pkg.tar.xz
  echo "56f12ccc54ee227657d4d8b070622849977444425184ee242a33e3ccc28bf745  bin/$pkg.tar.xz" | sha256sum -c -
  tar -xJf bin/$pkg.tar.xz -C bin --strip-components=1 $pkg/restate-server
  rm bin/$pkg.tar.xz
fi
PATH=$HOME/.cargo/bin:$PATH exec cargo build --release --locked -q
