<img src="images/logo.svg" alt="logo" width="200"/>

# RedisTimeSeries
[![Forum](https://img.shields.io/badge/Forum-RedisTimeSeries-blue)](https://forum.redislabs.com/c/modules/redistimeseries)
[![Discord](https://img.shields.io/discord/697882427875393627?style=flat-square)](https://discord.gg/KExRgMb)

RedisTimeSeries is a Redis Module adding a Time Series data structure to Redis.

## Features
Read more about the v1.0 GA features [here](https://redislabs.com/blog/redistimeseries-ga-making-4th-dimension-truly-immersive/).

- High volume inserts, low latency reads
- Query by start time and end-time
- Aggregated queries (Min, Max, Avg, Sum, Range, Count, First, Last, STD.P, STD.S, Var.P, Var.S, Deriv) for any time bucket
- Configurable maximum retention period
- Downsampling/Compaction - automatically updated aggregated timeseries
- Secondary index - each time series has labels (field value pairs) which will allows to query by labels


## Client Libraries

Official and community client libraries in Python, Java, JavaScript, Ruby, Go, C#, Rust, and PHP. 

See the [Clients page](clients.md) for the full list.

## Using with other tools metrics tools
In the [RedisTimeSeries](https://github.com/RedisTimeSeries) organization you can
find projects that help you integrate RedisTimeSeries with other tools, including:

1. [Prometheus](https://github.com/RedisTimeSeries/prometheus-redistimeseries-adapter) - read/write adapter to use RedisTimeSeries as backend db.
2. [Grafana 7.1+](https://github.com/RedisTimeSeries/grafana-redis-datasource) - using the [Redis Data Source](https://redislabs.com/blog/introducing-the-redis-data-source-plug-in-for-grafana/).
3. [Telegraph](https://github.com/RedisTimeSeries/telegraf)
4. StatsD, Graphite exports using graphite protocol.

## Memory model

A time series is a linked list of memory chunks.
Each chunk has a predefined size of samples.
Each sample is a tuple of the time and the value of 128 bits,
64 bits for the timestamp and 64 bits for the value.

## Setup

You can either get RedisTimeSeries setup in the cloud, in a Docker container or on your own machine.

### Redis Cloud

RedisTimeSeries is available on all Redis Cloud managed services. Redis Cloud Essentials offers a completely free managed database up to 30MB.

[Get started here](https://redislabs.com/try-free/)


### Docker
To quickly try out RedisTimeSeries, launch an instance using docker:
```sh
docker run -p 6379:6379 -it --rm redislabs/redistimeseries
```

### Download and running binaries

First download the pre-compiled version from [RedisLabs download center](https://redislabs.com/download-center/modules/).

Next, run Redis with RedisTimeSeries: 

```
$ redis-server --loadmodule /path/to/module/redistimeseries.so
```

### Build and Run it yourself

You can also build and run RedisTimeSeries on your own machine.

Major Linux distributions as well as macOS are supported.

#### Requirements

First, clone the RedisTimeSeries repository from git:

```
git clone --recursive https://github.com/RedisTimeSeries/RedisTimeSeries.git
```

Then, to install required build artifacts, invoke the following:

```
cd RedisTimeSeries
make setup
```
Or you can install required dependencies manually listed in [system-setup.py](https://github.com/RedisTimeSeries/RedisTimeSeries/blob/master/system-setup.py).

If ```make``` is not yet available, the following commands are equivalent:

```
./deps/readies/bin/getpy2
./system-setup.py
```

Note that ```system-setup.py``` **will install various packages on your system** using the native package manager and pip. This requires root permissions (i.e. sudo) on Linux.

If you prefer to avoid that, you can:

* Review system-setup.py and install packages manually,
* Utilize a Python virtual environment,
* Use Docker with the ```--volume``` option to create an isolated build environment.

#### Build

```bash
make build
```

Binary artifacts are placed under the ```bin``` directory.

#### Run

In your redis-server run: `loadmodule bin/redistimeseries.so`

For more information about modules, go to the [redis official documentation](https://redis.io/topics/modules-intro).

## Give it a try

After you setup RedisTimeSeries, you can interact with it using redis-cli.

Here we'll create a time series representing sensor temperature measurements. 
After you create the time series, you can send temperature measurements.
Then you can query the data for a time range on some aggregation rule.

### With `redis-cli`

```sh
$ redis-cli
127.0.0.1:6379> TS.CREATE temperature:3:11 RETENTION 6000 LABELS sensor_id 2 area_id 32
OK
127.0.0.1:6379> TS.ADD temperature:3:11 1548149181 30
OK
127.0.0.1:6379> TS.ADD temperature:3:11 1548149191 42
OK
127.0.0.1:6379>  TS.RANGE temperature:3:11 1548149180 1548149210 AGGREGATION avg 5
1) 1) (integer) 1548149180
   2) "30"
2) 1) (integer) 1548149190
   2) "42"
```


## Mailing List / Forum

Got questions? Feel free to ask at the [RedisTimeSeries mailing list](https://forum.redislabs.com/c/modules/redistimeseries).

## License

Redis Source Available License Agreement - see [LICENSE](https://raw.githubusercontent.com/RedisTimeSeries/RedisTimeSeries/master/LICENSE)
