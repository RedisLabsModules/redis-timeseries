#!/usr/bin/env bash
set -e
mkdir -p /tmp/gears

if [ -f "/tmp/gears/RedisGears/bin/linux-x64-release/redisgears.so" ]; then
    echo "Gears already exists"
    exit 0
fi

cd /tmp/gears

git clone https://github.com/RedisGears/RedisGears.git
cd RedisGears
make get_deps
git submodule init
git submodule update

make setup
make fetch
make all
