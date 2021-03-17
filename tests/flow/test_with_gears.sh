#!/usr/bin/env bash
#apt-get install git python build-essential
#python3 -m pip install -r ./requirements.txt

mkdir -p /tmp/gears

if [ -f "/tmp/gears/RedisGears/bin/linux-x64-release/redisgears.so" ]; then
    echo "Gears already exists"
    exit
fi

pushd /tmp/gears

git clone https://github.com/RedisGears/RedisGears.git
cd RedisGears
make get_deps
git submodule init
git submodule update

make setup
make fetch
make all

popd

#python3 -m RLTest --clear-logs --env oss-cluster --shards-count 4 \
#  --module ../../bin/redistimeseries.so \
#  --module /tmp/gears/RedisGears/bin/linux-x64-release/redisgears.so
