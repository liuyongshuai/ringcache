# ringcache

一个环形缓存，所有数据紧凑存储、高效的内存利用率。

因其淘汰算法并非LRU，非通用缓存服务，仅适用于特定的业务场景。

在此场景下所能拿到的内存有限、又要求较高的性能，所以要最大限度的利用内存，才弄这么一个东西来。

通过调整 `RING_BUFFER_NUM` 的大小控制最大并发数。

把所有的内存分配指定数量个环形缓冲区，每个缓冲区的数据淘汰并非LRU，是后来的把前面的挤掉。

已经过压测，功能测试、性能测试均无问题。直接引入头文件即可使用。

采用数组 + 链表实现的map，一般情况下map的hash_power是随便数据的写入而扩容增大的。

如hash_power固定为28时，map的容量为 268435456 个，2.6亿左右，可容纳绝大多数业务场景，此时map的数组部分占了2G的额外空间。

今次类推，hash_power=25时，可容纳三千多万的数据量，map的数组部分只占了256M，可根据需要自行调整，也可让其自行扩容。

# 几个重要的宏

`MAX_KEY_SIZE`：最大的key的长度。

`MAX_VALUE_SIZE`：最大的value的长度

`RING_BUFFER_NUM`：将所给的内存划分的缓冲区个数，个数越大应对并发的能力越强，默认256个足够了。对于读多写少的场景，直接64或128都足够了。

`RING_BUFFER_MIN_SIZE`：每个缓冲区的最小大小，可以自己调。


# 示例测试

cmake . && make && ./test