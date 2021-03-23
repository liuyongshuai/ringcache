# ringcache

一个环形缓存，所有数据紧凑存储、高效的内存利用率。

通过调整 `RING_BUFFER_NUM` 的大小控制最大并发数。

把所有的内存分配指定数量个环形缓冲区，每个缓冲区的数据淘汰并非LRU，是后来的把前面的挤掉。

已经过压测，功能测试、性能测试均无问题。

# 几个重要的宏

`MAX_KEY_SIZE`：最大的key的长度

`MAX_VALUE_SIZE`：最大的value的长度

`RING_BUFFER_NUM`：将所给的内存划分的缓冲区个数，个数越大应对并发的能力越强

`RING_BUFFER_MIN_SIZE`：每个缓冲区的最小大小，可以自己调。