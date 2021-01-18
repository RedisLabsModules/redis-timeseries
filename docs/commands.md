# RedisTimeSeries Commands

## Create

### TS.CREATE

Create a new time-series.

```sql
TS.CREATE key [RETENTION retentionTime] [UNCOMPRESSED] [CHUNK_SIZE size] [DUPLICATE_POLICY policy] [LABELS label value..]
```

* key - Key name for timeseries

Optional args:

 * RETENTION - Maximum age for samples compared to last event time (in milliseconds)
    * Default: The global retention secs configuration of the database (by default, `0`)
    * When set to 0, the series is not trimmed at all
 * UNCOMPRESSED - since version 1.2, both timestamps and values are compressed by default.
   Adding this flag will keep data in an uncompressed form. Compression not only saves
   memory but usually improve performance due to lower number of memory accesses. 
 * CHUNK_SIZE - amount of memory, in bytes, allocated for data. Default: 4000.
 * DUPLICATE_POLICY - configure what to do on duplicate sample.
   When this is not set, the server-wide default will be used. 
   For further details: [Duplicate sample policy](configuration.md#DUPLICATE_POLICY).
 * labels - Set of label-value pairs that represent metadata labels of the key

#### Complexity

TS.CREATE complexity is O(1).

#### Create Example

```sql
TS.CREATE temperature:2:32 RETENTION 60000 DUPLICATE_POLICY MAX LABELS sensor_id 2 area_id 32
```

## Delete

### DEL

A series can be deleted using redis `DEL` command. Timeout can be set for a series using
redis `EXPIRE` command.

## Update

### TS.ALTER

Update the retention, labels of an existing key. The parameters are the same as TS.CREATE.

```sql
TS.ALTER key [RETENTION retentionTime] [LABELS label value..]
```

#### Alter Example

```sql
TS.ALTER temperature:2:32 LABELS sensor_id 2 area_id 32 sub_area_id 15
```

#### Notes
* The command only alters the labels that are given,
  e.g. if labels are given but retention isn't, then only the labels are altered.
* If the labels are altered, the given label-list is applied,
  i.e. labels that are not present in the given list are removed implicitly.
* Supplying the `LABELS` keyword without any labels will remove all existing labels.  

### TS.ADD

Append (or create and append) a new sample to the series.

```sql
TS.ADD key timestamp value [RETENTION retentionTime] [UNCOMPRESSED] [CHUNK_SIZE size] [ON_DUPLICATE policy] [LABELS label value..]
```

* timestamp - UNIX timestamp of the sample. `*` can be used for automatic timestamp (using the system clock)
* value - numeric data value of the sample (double)

These arguments are optional because they can be set by TS.CREATE:

 * RETENTION - Maximum age for samples compared to last event time (in milliseconds)
    * Default: The global retention secs configuration of the database (by default, `0`)
    * When set to 0, the series is not trimmed at all
 * UNCOMPRESSED - Changes data storage from compressed (by default) to uncompressed
 * CHUNK_SIZE - amount of memory, in bytes, allocated for data. Default: 4000.
 * ON_DUPLICATE - overwrite key and database configuration for `DUPLICATE_POLICY`. [See Duplicate sample policy](configuration.md#DUPLICATE_POLICY)
 * labels - Set of label-value pairs that represent metadata labels of the key

If this command is used to add data to an existing timeseries, `retentionTime` and `labels` are ignored.

#### Examples
```sql
127.0.0.1:6379>TS.ADD temperature:2:32 1548149180000 26 LABELS sensor_id 2 area_id 32
(integer) 1548149180000
127.0.0.1:6379>TS.ADD temperature:3:11 1548149183000 27 RETENTION 3600
(integer) 1548149183000
127.0.0.1:6379>TS.ADD temperature:3:11 * 30
(integer) 1559718352000
```

#### Complexity

If a compaction rule exits on a timeseries, `TS.ADD` performance might be reduced.
The complexity of `TS.ADD` is always O(M) when M is the amount of compaction rules or O(1) with no compaction.

#### Notes

- You can use this command to add data to an non existing timeseries in a single command.
  This is the reason why `labels` and `retentionTime` are optional arguments.
- When specified and the key doesn't exist, RedisTimeSeries will create the key with the specified `labels` and or `retentionTime`.
  Setting the `labels` and `retentionTime` introduces additional time complexity.
- Updating a sample in a trimmed window will update down-sampling aggregation based on the existing data.

### TS.MADD

Append new samples to a list of series.

```sql
TS.MADD key timestamp value [key timestamp value ...]
```

* timestamp - UNIX timestamp of the sample. `*` can be used for automatic timestamp (using the system clock)
* value - numeric data value of the sample (double)

#### Examples
```sql
127.0.0.1:6379>TS.MADD temperature:2:32 1548149180000 26 cpu:2:32 1548149183000 54
1) (integer) 1548149180000
2) (integer) 1548149183000
127.0.0.1:6379>TS.MADD temperature:2:32 1548149181000 45 cpu:2:32 1548149180000 30
1) (integer) 1548149181000
2) (integer) 1548149180000
```

#### Complexity

If a compaction rule exits on a timeseries, `TS.MADD` performance might be reduced.
The complexity of `TS.MADD` is always O(N*M) when N is the amount of series updated and M is the amount of compaction rules or O(N) with no compaction.

### TS.INCRBY/TS.DECRBY

Creates a new sample that increments/decrements the latest sample's value.
> Note: TS.INCRBY/TS.DECRBY support updates for the latest sample.

```sql
TS.INCRBY key value [TIMESTAMP timestamp] [RETENTION retentionTime] [UNCOMPRESSED] [CHUNK_SIZE size] [LABELS label value..]
```

or

```sql
TS.DECRBY key value [TIMESTAMP timestamp] [RETENTION retentionTime] [UNCOMPRESSED] [CHUNK_SIZE size] [LABELS label value..]
```

This command can be used as a counter or gauge that automatically gets history as a time series.

* key - Key name for timeseries
* value - numeric data value of the sample (double)

Optional args:

 * TIMESTAMP - UNIX timestamp of the sample. `*` can be used for automatic timestamp (using the system clock)
 * RETENTION - Maximum age for samples compared to last event time (in milliseconds)
    * Default: The global retention secs configuration of the database (by default, `0`)
    * When set to 0, the series is not trimmed at all
 * UNCOMPRESSED - Changes data storage from compressed (by default) to uncompressed
 * CHUNK_SIZE - amount of memory, in bytes, allocated for data. Default: 4000.
 * labels - Set of label-value pairs that represent metadata labels of the key

If this command is used to add data to an existing timeseries, `retentionTime` and `labels` are ignored.

#### Notes

- You can use this command to add data to an non existing timeseries in a single command.
  This is the reason why `labels` and `retentionTime` are optional arguments.
- When specified and the key doesn't exist, RedisTimeSeries will create the key with the specified `labels` and or `retentionTime`.
  Setting the `labels` and `retentionTime` introduces additional time complexity.

## Aggregation, Compaction, Downsampling

### TS.CREATERULE

Create a compaction rule.

```sql
TS.CREATERULE sourceKey destKey AGGREGATION aggregationType timeBucket
```

* sourceKey - Key name for source time series
* destKey - Key name for destination time series
* aggregationType - Aggregation type: avg, sum, min, max, range, count, first, last, std.p, std.s, var.p, var.s
* timeBucket - Time bucket for aggregation in milliseconds

DEST_KEY should be of a `timeseries` type, and should be created before TS.CREATERULE is called.

!!! info "Note on existing samples in the source time series"
        
        Currently, only new samples that are added into the source series after creation of the rule will be aggregated.


### TS.DELETERULE

Delete a compaction rule.

```sql
TS.DELETERULE sourceKey destKey
```

- sourceKey - Key name for source time series
- destKey - Key name for destination time series

## Query

### Filtering
For certain read commands a list of filters needs to be applied.  This is the list of possible filters:

* `l=v` label equals value
* `l!=v` label doesn't equal value
* `l=` key does not have the label `l`
* `l!=` key has label `l`
* `l=(v1,v2,...)` key with label `l` that equals one of the values in the list
* `l!=(v1,v2,...)` key with label `l` that doesn't equal any of the values in the list

Note: Whenever filters need to be provided, a minimum of one `l=v` filter must be applied.

### TS.RANGE/TS.REVRANGE

Query a range in forward or reverse directions.

```sql
TS.RANGE key fromTimestamp toTimestamp [COUNT count] [OFFSET offset] [AGGREGATION aggregationType timeBucket]
TS.REVRANGE key fromTimestamp toTimestamp [COUNT count] [OFFSET offset] [AGGREGATION aggregationType timeBucket]
```

- key - Key name for timeseries
- fromTimestamp - Start timestamp for the range query. `-` can be used to express the minimum possible timestamp (0).
- toTimestamp - End timestamp for range query, `+` can be used to express the maximum possible timestamp.

Optional args:
* count - Maximum number of returned results per time-series.
* offset - Offset the timestamps being returned; allows for the starting value to be set.
* aggregationType - Aggregation type: avg, sum, min, max, range, count, first, last, std.p, std.s, var.p, var.s
* timeBucket - Time bucket for aggregation in milliseconds

#### Complexity

TS.RANGE complexity is O(n/m+k).

n = Number of data points
m = Chunk size (data points per chunk)
k = Number of data points that are in the requested range

This can be improved in the future by using binary search to find the start of the range, which makes this O(Log(n/m)+k*m).
But because m is pretty small, we can neglect it and look at the operation as O(Log(n) + k).

#### Aggregated Query Example

```sql
127.0.0.1:6379> TS.RANGE temperature:3:32 1548149180000 1548149210000 AGGREGATION avg 5000
1) 1) (integer) 1548149180000
   2) "26.199999999999999"
2) 1) (integer) 1548149185000
   2) "27.399999999999999"
3) 1) (integer) 1548149190000
   2) "24.800000000000001"
4) 1) (integer) 1548149195000
   2) "23.199999999999999"
5) 1) (integer) 1548149200000
   2) "25.199999999999999"
6) 1) (integer) 1548149205000
   2) "28"
7) 1) (integer) 1548149210000
   2) "20"
```

### TS.MRANGE/TS.MREVRANGE

Query a range across multiple time-series by filters in forward or reverse directions.

```sql
TS.MRANGE fromTimestamp toTimestamp [COUNT count] [OFFSET offset] [AGGREGATION aggregationType timeBucket] [WITHLABELS] FILTER filter..
TS.MREVRANGE fromTimestamp toTimestamp [COUNT count] [OFFSET offset] [AGGREGATION aggregationType timeBucket] [WITHLABELS] FILTER filter..
```

* fromTimestamp - Start timestamp for the range query. `-` can be used to express the minimum possible timestamp (0).
* toTimestamp - End timestamp for range query, `+` can be used to express the maximum possible timestamp.
* filter - [See Filtering](#filtering)

Optional args:

* count - Maximum number of returned results per time-series.
* offset - Offset the timestamps being returned; allows for the starting value to be set.
* aggregationType - Aggregation type: avg, sum, min, max, range, count, first, last, std.p, std.s, var.p, var.s
* timeBucket - Time bucket for aggregation in milliseconds.
* WITHLABELS - Include in the reply the label-value pairs that represent metadata labels of the time-series. If this argument is not set, by default, an empty Array will be replied on the labels array position.

#### Return Value

Array-reply, specifically:

The command returns the entries with labels matching the specified filter.
The returned entries are complete, that means that the name, labels and all the samples that match the range are returned.

The returned array will contain key1,labels1,values1,...,keyN,labelsN,valuesN, with labels and values being also of array data types. By default, the labels array will be an empty Array for each of the returned time-series. If the `WITHLABELS` option is specified the labels Array will be filled with label-value pairs that represent metadata labels of the time-series.


#### Examples

##### Query by Filters Example
```sql
127.0.0.1:6379> TS.MRANGE 1548149180000 1548149210000 AGGREGATION avg 5000 FILTER area_id=32 sensor_id!=1
1) 1) "temperature:2:32"
   2) (empty list or set)
   3) 1) 1) (integer) 1548149180000
         2) "27.600000000000001"
      2) 1) (integer) 1548149185000
         2) "23.800000000000001"
      3) 1) (integer) 1548149190000
         2) "24.399999999999999"
      4) 1) (integer) 1548149195000
         2) "24"
      5) 1) (integer) 1548149200000
         2) "25.600000000000001"
      6) 1) (integer) 1548149205000
         2) "25.800000000000001"
      7) 1) (integer) 1548149210000
         2) "21"
2) 1) "temperature:3:32"
   2) (empty list or set)
   3) 1) 1) (integer) 1548149180000
         2) "26.199999999999999"
      2) 1) (integer) 1548149185000
         2) "27.399999999999999"
      3) 1) (integer) 1548149190000
         2) "24.800000000000001"
      4) 1) (integer) 1548149195000
         2) "23.199999999999999"
      5) 1) (integer) 1548149200000
         2) "25.199999999999999"
      6) 1) (integer) 1548149205000
         2) "28"
      7) 1) (integer) 1548149210000
         2) "20"
```

##### Query by Filters Example with WITHLABELS option

```sql
127.0.0.1:6379> TS.MRANGE 1548149180000 1548149210000 AGGREGATION avg 5000 WITHLABELS FILTER area_id=32 sensor_id!=1
1) 1) "temperature:2:32"
   2) 1) 1) "sensor_id"
         2) "2"
      2) 1) "area_id"
         2) "32"
   3) 1) 1) (integer) 1548149180000
         2) "27.600000000000001"
      2) 1) (integer) 1548149185000
         2) "23.800000000000001"
      3) 1) (integer) 1548149190000
         2) "24.399999999999999"
      4) 1) (integer) 1548149195000
         2) "24"
      5) 1) (integer) 1548149200000
         2) "25.600000000000001"
      6) 1) (integer) 1548149205000
         2) "25.800000000000001"
      7) 1) (integer) 1548149210000
         2) "21"
2) 1) "temperature:3:32"
   2) 1) 1) "sensor_id"
         2) "3"
      2) 1) "area_id"
         2) "32"
   3) 1) 1) (integer) 1548149180000
         2) "26.199999999999999"
      2) 1) (integer) 1548149185000
         2) "27.399999999999999"
      3) 1) (integer) 1548149190000
         2) "24.800000000000001"
      4) 1) (integer) 1548149195000
         2) "23.199999999999999"
      5) 1) (integer) 1548149200000
         2) "25.199999999999999"
      6) 1) (integer) 1548149205000
         2) "28"
      7) 1) (integer) 1548149210000
         2) "20"
```

### TS.GET

Get the last sample.

```sql
TS.GET key
```

* key - Key name for timeseries


#### Return Value

Array-reply, specifically:

The returned array will contain:
- The last sample timestamp followed by the last sample value, when the time-series contains data. 
- An empty array, when the time-series is empty.


#### Complexity

TS.GET complexity is O(1).

#### Examples

##### Get Example on time-series containing data

```sql
127.0.0.1:6379> TS.GET temperature:2:32
1) (integer) 1548149279
2) "23"
```

##### Get Example on empty time-series 

```sql
127.0.0.1:6379> redis-cli TS.GET empty_ts
(empty array)
```

### TS.MGET
Get the last samples matching the specific filter.

```sql
TS.MGET [WITHLABELS] FILTER filter...
```
* filter - [See Filtering](#filtering)

Optional args:

* WITHLABELS - Include in the reply the label-value pairs that represent metadata labels of the time-series. If this argument is not set, by default, an empty Array will be replied on the labels array position.

#### Return Value

Array-reply, specifically:

The command returns the entries with labels matching the specified filter.
The returned entries are complete, that means that the name, labels and all the last sample of the time-serie.

The returned array will contain key1,labels1,lastsample1,...,keyN,labelsN,lastsampleN, with labels and lastsample being also of array data types. By default, the labels array will be an empty Array for each of the returned time-series. If the `WITHLABELS` option is specified the labels Array will be filled with label-value pairs that represent metadata labels of the time-series.


#### Complexity

TS.MGET complexity is O(n).

n = Number of time-series that match the filters

#### Examples

##### MGET Example with default behaviour
```sql
127.0.0.1:6379> TS.MGET FILTER area_id=32
1) 1) "temperature:2:32"
   2) (empty list or set)
   3) 1) (integer) 1548149181000
      2) "30"
2) 1) "temperature:3:32"
   2) (empty list or set)
   3) 1) (integer) 1548149181000
      2) "29"
```

##### MGET Example with WITHLABELS option
```sql
127.0.0.1:6379> TS.MGET WITHLABELS FILTER area_id=32
1) 1) "temperature:2:32"
   2) 1) 1) "sensor_id"
         2) "2"
      2) 1) "area_id"
         2) "32"
   3) 1) (integer) 1548149181000
      2) "30"
2) 1) "temperature:3:32"
   2) 1) 1) "sensor_id"
         2) "2"
      2) 1) "area_id"
         2) "32"
   3) 1) (integer) 1548149181000
      2) "29"
```

## General

### TS.INFO

#### Format
```sql
TS.INFO key [DEBUG]
```

#### Description

Returns information and statistics on the time-series.

#### Parameters

* key - Key name of the time-series.
* DEBUG - An optional flag to get a more detailed information about the chunks.

#### Complexity

O(1)

#### Return Value

Array-reply, specifically:

* totalSamples - Total number of samples in the time series.
* memoryUsage - Total number of bytes allocated for the time series.
* firstTimestamp - First timestamp present in the time series.
* lastTimestamp - Last timestamp present in the time series.
* retentionTime - Retention time, in milliseconds, for the time series.
* chunkCount - Number of Memory Chunks used for the time series.
* chunkSize - Amount of memory, in bytes, allocated for data.
* chunkType - The chunk type, `compressed` or `uncompressed`.
* duplicatePolicy - [Duplicate sample policy](configuration.md#DUPLICATE_POLICY).
* labels - A nested array of label-value pairs that represent the metadata labels of the time series.
* sourceKey - Key name for source time series in case the current series is a target of a [rule](#tscreaterule).
* rules - A nested array of compaction [rules](#tscreaterule) of the time series.

When `DEBUG` is passed, the response will contain an additional array field called `Chunks`.
Each item (per chunk) will contain:
* startTimestamp - First timestamp present in the chunk.
* endTimestamp - Last timestamp present in the chunk.
* samples - Total number of samples in the chunk.
* size - The chunk *data* size in bytes (this is the exact size that used for data only inside the chunk, 
  doesn't include other overheads)
* bytesPerSample - Ratio of `size` and `samples`

#### `TS.INFO` Example

```sql
TS.INFO temperature:2:32
 1) totalSamples
 2) (integer) 100
 3) memoryUsage
 4) (integer) 4184
 5) firstTimestamp
 6) (integer) 1548149180
 7) lastTimestamp
 8) (integer) 1548149279
 9) retentionTime
10) (integer) 0
11) chunkCount
12) (integer) 1
13) chunkSize
14) (integer) 256
15) chunkType
16) compressed
17) duplicatePolicy
18) (nil)
19) labels
20) 1) 1) "sensor_id"
       2) "2"
    2) 1) "area_id"
       2) "32"
21) sourceKey
22) (nil)
23) rules
24) (empty list or set)
```

With `DEBUG`:
```
...
23) rules
24) (empty list or set)
25) Chunks
26) 1)  1) startTimestamp
        2) (integer) 1548149180
        3) endTimestamp
        4) (integer) 1548149279
        5) samples
        6) (integer) 100
        7) size
        8) (integer) 256
        9) bytesPerSample
       10) "1.2799999713897705"
```

### TS.QUERYINDEX

Get all the keys matching the filter list.

```sql
TS.QUERYINDEX filter...
```

* filter - [See Filtering](#filtering)

### Query index example
```sql
127.0.0.1:6379> TS.QUERYINDEX sensor_id=2
1) "temperature:2:32"
2) "temperature:2:33"
```
