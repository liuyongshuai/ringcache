/*************************************************************************
 * File:	ringbcache.h
 * Author:	liuyongshuai<liuyongshuai@hotmail.com>
 * Time:	2021-03-11 11:39
 ************************************************************************/
#ifndef _RINGCACHE_RINGBUFFER_H_202103111139_
#define _RINGCACHE_RINGBUFFER_H_202103111139_

#include "entry.h"
#include <iostream>
#include <math.h>
#include <thread>
#include <assert.h>

namespace ringcache{

    class ringcache{
    public:
        /**
         * 注意，这里的mem_size单位为MB
         */
        explicit ringcache(uint64_t megabyte_size){
            /**
             * 将单位换算成MB
             */
            uint64_t mem_byte_size = megabyte_size * MB;
            std::cout << "mem_byte_size=" << mem_byte_size << "\tmegabyte_size=" << megabyte_size << std::endl;

            //缓冲区的数量
            std::cout << "buffer_num=" << RING_BUFFER_NUM << std::endl;

            /**
             * 初始化统计信息
             */
            this->stats = new stats_t();
            this->stats->buffer_num = RING_BUFFER_NUM;


            /**
             * 平均每个缓冲区的大小
             */
            this->buffer_size = mem_byte_size / RING_BUFFER_NUM;
            this->buffer_size = this->buffer_size > RING_BUFFER_MIN_SIZE ? this->buffer_size : RING_BUFFER_MIN_SIZE;
            std::cout << "avg_size=" << this->buffer_size << std::endl;
            this->alloc_buffer_memory();

            /**
             * hash表的锁
             */
            for (int i = 0; i < HASH_SIZE(HASHTABLE_LOCK_POWER); i++){
                this->hashtable_locks.push_back(new std::mutex());
            }

            /**
             * hash表初始化
             */
            //预估初始容量大小，一般按512字节
            uint8_t init_hash_power = HASH_POWER_INIT;
            //预估容量
            uint32_t entrySize = mem_byte_size / AVG_DATA_SIZE;
            size_t entryPower = ceil(log((double) entrySize) / log(2.0));
            if (entryPower > init_hash_power){
                init_hash_power = entryPower + 1;
            }
            if (init_hash_power >= HASH_POWER_MAX){
                init_hash_power = HASH_POWER_MAX;
            }
            this->hash_power = init_hash_power;
            this->primary_hashtable = (entry_t **) calloc(HASH_SIZE(this->hash_power), sizeof(entry_t *));
            for (uint32_t i = 0; i < HASH_SIZE(this->hash_power); i++){
                this->primary_hashtable[i] = nullptr;
            }

            /**
             * 其他参数初始化
             */
            this->secondary_hashtable = nullptr;
            this->is_hashtable_expanding = false;
            this->is_hashtable_full = false;
            this->is_thread_stop = false;
            this->hashtable_expanding_index = -1;

            /**
             * 一个扩容hash表线程、一个申请内存的线程
             */
            this->expand_hashtable_thread = new std::thread(&ringcache::expand_hashtable_func, this);
            this->expand_buffer_thread = new std::thread(&ringcache::expand_buffer_func, this);
        }

        /**
         * 写入数据
         */
        uint32_t set(const std::string &key, const std::string &value, uint32_t expire_time){
            return this->set(key, value.c_str(), value.length(), expire_time);
        }

        /**
         * 写入数据
         */
        uint32_t set(const std::string &key, const char *val, uint32_t val_len, uint32_t expire_time){
            /**
             * key & value 长度校验
             */
            if (key.length() >= MAX_KEY_SIZE){
                return RINGCACHE_ERRNO_KEY_TOO_LONG;
            }
            if (val_len >= MAX_VALUE_SIZE){
                return RINGCACHE_ERRNO_VALUE_TOO_LONG;
            }

            /**
             * 提取一个要存数据的buffer
             */
            uint32_t hash_val = hash(key);
            ring_buffer_t *buffer = this->get_buffer_with_lock(hash_val);
            if (buffer == nullptr){
                return RINGCACHE_ERRNO_ALLOC_MEMORY_FAILED;
            }
            if (!buffer->mem_begin){
                buffer->mtx->unlock();
                return RINGCACHE_ERRNO_ALLOC_MEMORY_FAILED;
            }

            /**
             * 从buffer找一块合适的空间
             */
            entry_t *entry = this->get_mem_without_lock(key.length() + val_len, hash_val, buffer);
            if (entry == nullptr){
                buffer->mtx->unlock();
                return RINGCACHE_ERRNO_ALLOC_MEMORY_FAILED;
            }


            //锁定hash相关的项
            std::mutex *hash_mtx = this->get_hashtable_lock(hash_val);
            std::lock_guard< std::mutex > hash_lock(*hash_mtx);

            /**
             * 拷贝数据到缓存空间里
             */
            entry->hash_next = nullptr;
            entry->key_len = key.length();
            entry->value_len = val_len;
            entry->expire_time = expire_time;
            memcpy(entry->data, key.c_str(), key.length());
            memcpy(entry->data + key.length(), val, val_len);

            entry_t **hash_entry = this->get_hashtable_bucket(hash_val);

            /**
             * 如果之前已经有相同的key了直接清理了
             */
            entry_t *pre = nullptr;
            entry_t *cur = *hash_entry;
            std::string tmpKey;
            while (cur != nullptr){
                cur->key(tmpKey);
                //有可能同一个bucket会有多个相同的key
                if (cur->key_len == key.length() && tmpKey == key){
                    if (pre == nullptr){
                        *hash_entry = cur->hash_next;
                    }
                    else{
                        pre->hash_next = cur->hash_next;
                    }
                    cur->hash_next = nullptr;
                    cur->expire_time = 1;
                    cur->key_len = 0;
                    cur = cur->hash_next;
                    continue;
                }
                pre = cur;
                cur = cur->hash_next;
            }

            entry->hash_next = *hash_entry;
            *hash_entry = entry;
            buffer->mtx->unlock();
            return RINGCACHE_ERRNO_OK;
        }

        /**
         * 提取数据
         */
        uint32_t del(const std::string &key){
            if (key.length() >= MAX_KEY_SIZE){
                return RINGCACHE_ERRNO_KEY_TOO_LONG;
            }

            uint32_t hash_val = hash(key);
            std::mutex *hash_mtx = this->get_hashtable_lock(hash_val);
            std::lock_guard< std::mutex > lock(*hash_mtx);
            entry_t **hash_entry = this->get_hashtable_bucket(hash_val);

            entry_t *pre = nullptr;
            entry_t *next = nullptr;
            entry_t *cur = *hash_entry;
            std::string tmpKey;
            while (cur){
                next = cur->hash_next;
                cur->key(tmpKey);
                //有可能同一个bucket会有多个相同的key
                if (tmpKey == key){
                    if (pre == nullptr){
                        *hash_entry = cur->hash_next;
                    }
                    else{
                        pre->hash_next = cur->hash_next;
                    }
                    cur->hash_next = nullptr;
                    cur->expire_time = 1;
                    cur->key_len = 0;
                    cur = next;
                    continue;
                }
                pre = cur;
                cur = cur->hash_next;
            }
            return RINGCACHE_ERRNO_OK;
        }

        /**
         * 检查数据是否存在
         */
        bool check(const std::string &key){
            std::string v;
            return this->get(key, v, true) == RINGCACHE_ERRNO_OK;
        }

        /**
         * 提取数据，不会锁hash表
         */
        uint32_t get(const std::string &key, std::string &value){
            return this->get(key, value, false);
        }

        /**
         * 提取数据，不会锁hash表
         */
        uint32_t get(const std::string &key, std::string &value, bool only_check){
            if (key.length() >= MAX_KEY_SIZE){
                return RINGCACHE_ERRNO_KEY_TOO_LONG;
            }

            uint32_t hash_val = hash(key);
            entry_t **hash_entry = this->get_hashtable_bucket(hash_val);

            entry_t *entry = *hash_entry;
            uint32_t klen = key.length();
            std::string tmpKey;
            int64_t ct = time(nullptr);
            while (entry != nullptr){
                if (entry->key_len != klen){
                    entry = entry->hash_next;
                    continue;
                }
                entry->key(tmpKey);
                if (tmpKey != key){
                    entry = entry->hash_next;
                    continue;
                }
                //过期了
                if (entry->expire_time > 0 && entry->expire_time <= ct){
                    return RINGCACHE_ERRNO_KEY_EXPIRED;
                }
                //是不是只检查数据存在，并不获取数据
                if (!only_check){
                    entry->value(value);
                }
                break;
            }
            //没找着
            if (entry == nullptr){
                return RINGCACHE_ERRNO_NOT_FOUND;
            }
            return RINGCACHE_ERRNO_OK;
        }

        /**
         * 当前统计信息
         */
        const stats_t *get_stats(){
            return this->stats;
        }

        /**
         * 释放空间
         */
        ~ringcache(){
            this->is_thread_stop = true;
            this->expand_buffer_thread->join();
            this->expand_hashtable_thread->join();
            free(this->primary_hashtable);
            for (auto it:this->hashtable_locks){
                delete (it);
            }
            for (auto it:this->buffers){
                free(it->mem_begin);
                delete it->stats;
                delete it->mtx;
                delete it;
            }
        }

    private:
        /**
         * 线程
         */
        std::thread *expand_hashtable_thread;
        std::thread *expand_buffer_thread;

        /**
         * 挑一个buffer，目前是随机选择一个buffer开始，目前看是还比较均匀
         */
        ring_buffer_t *get_buffer_with_lock(uint32_t hash_val){
            ring_buffer_t *buffer;
            uint32_t retry_times = 0;
            srand(time(nullptr) & HASH_MASK(32));
            uint32_t startIdx = rand();
            uint32_t endIdx = this->buffers.size() + startIdx;
            do{
                for (uint32_t i = startIdx; i < endIdx; i++){
                    buffer = this->buffers.at(i % this->buffers.size());
                    if (buffer->mtx->try_lock()){
                        return buffer;
                    }
                }
            }while (retry_times++ < 5);
            buffer = this->buffers[hash_val % this->buffers.size()];
            buffer->mtx->lock();
            return buffer;
        }

        /**
         * 提取多少字节：从指定buffer里获取可以存下数据的块
         */
        entry_t *get_mem_without_lock(uint32_t msize, uint32_t hash_val, ring_buffer_t *buffer){
            //开始寻找空间
            uint64_t need_size = msize + sizeof(entry_t);
            uint64_t buffer_remain_size = buffer->mem_end - buffer->mem_cur_ptr + 1;

            /**
             * 如果当前指针后面剩余的空间不够存储当前数据，从头开始
             */
            if (buffer_remain_size < need_size){
                buffer->mem_cur_ptr = buffer->mem_begin;
                buffer->stats->reset_header_times++;
            }

            buffer->stats->item_num++;
            buffer->stats->set_num++;

            //还差多少空间
            uint64_t reduce_size = need_size;

            //寻找需要释放后面的几个空间
            entry_t *tmpEntry = (entry_t *) buffer->mem_cur_ptr;

            /**
             * 寻找当前数据要剔除的其他数据。还要确定最后一个entry腾出来的空间。
             * 如果够一个sizeof(entry_t)的话就留着给下一次写入用，如果不够直接全让给本数据
             */
            std::string tmpKey;
            while (true){
                assert(tmpEntry->key_len < MAX_KEY_SIZE);
                assert(tmpEntry->entry_len <= buffer->mem_size);
                //剔除当前的数据
                if (tmpEntry->key_len > 0){
                    tmpEntry->key(tmpKey);
                    this->del(tmpKey);
                    buffer->stats->del_num++;
                    buffer->stats->item_num--;
                }
                if (tmpEntry->entry_len >= reduce_size){
                    break;
                }
                assert(tmpEntry->entry_len > 0);
                reduce_size -= tmpEntry->entry_len;
                tmpEntry = (entry_t *) ((char *) tmpEntry + tmpEntry->entry_len);
            }

            //最后一个剩余的空间
            assert(tmpEntry->entry_len >= reduce_size);
            uint64_t last_entry_remain_size = tmpEntry->entry_len - reduce_size;

            /**
             * 要返回的内存地址
             */
            entry_t *ret = (entry_t *) buffer->mem_cur_ptr;

            /**
             * 最后一个entry，如果剩余的空间少于sizeof(entry_t)则不再保留
             */
            if (last_entry_remain_size > sizeof(entry_t)){
                buffer->mem_cur_ptr = (char *) tmpEntry + reduce_size;
                entry_t *tmp = (entry_t *) buffer->mem_cur_ptr;
                tmp->value_len = 0;
                tmp->entry_len = last_entry_remain_size;
                tmp->key_len = 0;
                tmp->expire_time = 1;
                tmp->hash_next = nullptr;
                ret->entry_len = need_size;
            }
            else{
                buffer->mem_cur_ptr = (char *) tmpEntry + tmpEntry->entry_len;
                //剩下的空间不足一个entry_t结构体，直接带走对齐得了
                ret->entry_len = need_size + last_entry_remain_size;
            }
            ret->hash_next = nullptr;
            ret->key_len = 0;
            ret->value_len = 0;
            ret->expire_time = 0;


            //如果正好到末尾，修改一下当前指针的指向
            if (buffer->mem_cur_ptr >= buffer->mem_end){
                buffer->mem_cur_ptr = buffer->mem_begin;
                buffer->stats->reset_header_times++;
            }
            return ret;
        }

        /**
         * 扩容用的，当使用的总的小格子量超过总量60%时开始扩容
         * hash_power=25时容量差不多是33554432，此时差不多可以开始以秒为单位sleep
         * =28时差不多就是2.6亿，够可以的了，可以用10秒了
         */
        void expand_hashtable_func(){
            std::cout << "[thread_func]start expand_hashtable_func" << std::endl;
            uint64_t sleepInterval = 10000;
            do{
                //检查总空间的使用量
                if ((HASH_SIZE(this->hash_power) / 4) * 3 < this->stats->item_num()){
                    std::cout << "[expand_hashtable_func] total_thres=" << (HASH_SIZE(this->hash_power) / 10) * 6 << "\tcur_item_num=" << this->stats->item_num() << std::endl;
                    this->expand_hash_table();
                }
                if (this->is_hashtable_full.load()){
                    break;
                }
                usleep(sleepInterval);

                //转换一下sleep的时间
                if (this->hash_power >= 28){//大概2.6亿个，应该到极限了
                    sleepInterval = 10000000;
                }
                else if (this->hash_power >= 27){//大概1.3亿个
                    sleepInterval = 5000000;
                }
                else if (this->hash_power > 25){//大概3.3千万个
                    sleepInterval = 1000000;
                }
            }while (!this->is_thread_stop);
            std::cout << "[thread_func]end expand_hashtable_func" << std::endl;
        }

        /**
         * 在后台线程里申请内存
         */
        void expand_buffer_func(){
            std::cout << "[thread_func]start expand_buffer_func" << std::endl;
            while (this->buffers.size() < RING_BUFFER_NUM){
                if (this->is_thread_stop){
                    return;
                }
                this->alloc_buffer_memory();
            }
            std::cout << "[thread_func]finish expand_buffer_func" << std::endl;
        }

        /**
         * 给缓冲区分配内存
         */
        void alloc_buffer_memory(){
            ring_buffer_t *buffer = new ring_buffer_t();
            buffer->mem_begin = (char *) calloc(this->buffer_size, sizeof(char));
            if (buffer->mem_begin == nullptr){
                delete buffer;
                return;
            }
            std::cout << "[alloc_buffer_memory]alloc buffer success, size=" << (this->buffer_size / MB) << "MB" << std::endl;

            buffer->mem_end = buffer->mem_begin + this->buffer_size - 1;
            buffer->mem_cur_ptr = buffer->mem_begin;
            buffer->mtx = new std::mutex();
            buffer->mem_size = this->buffer_size;

            //初始化统计信息
            buffer->stats = new buffer_stats_t();
            buffer->stats->index = this->buffers.size();
            buffer->stats->item_num = 0;
            buffer->stats->set_num = 0;
            buffer->stats->del_num = 0;
            buffer->stats->reset_header_times = 0;
            buffer->stats->cache_byte_size = this->buffer_size;
            this->stats->buffer_stats.push_back(buffer->stats);

            //初始化内存块header信息
            entry_t *tmpEntry = (entry_t *) buffer->mem_begin;
            tmpEntry->expire_time = 1;
            tmpEntry->key_len = 0;
            tmpEntry->entry_len = this->buffer_size;
            tmpEntry->value_len = 0;
            tmpEntry->hash_next = nullptr;

            this->buffers.push_back(buffer);
        }

        /**
         * 扩容hash表
         */
        void expand_hash_table(){
            if (this->hash_power >= HASH_POWER_MAX){
                this->is_hashtable_full = true;
            }
            //已经满了
            if (this->is_hashtable_full.load()){
                return;
            }
            std::cout << "[expand_hash_table]start expand_hash_table......" << std::endl;

            //扩容标志检查
            this->secondary_hashtable = this->primary_hashtable;
            bool expanding = false;
            this->is_hashtable_expanding.compare_exchange_strong(expanding, true);
            if (expanding){
                std::cout << "[expand_hash_table] is_hashtable_expanding=true......" << std::endl;
                return;
            }

            //新的容量
            this->hashtable_expanding_index = -1;
            uint8_t new_hash_power = this->hash_power + 1;
            if (new_hash_power > HASH_POWER_MAX){
                this->is_hashtable_full = true;
                return;
            }

            //申请新的空间
            uint32_t new_hash_size = HASH_SIZE(new_hash_power);
            this->primary_hashtable = (entry_t **) calloc(new_hash_size, sizeof(entry_t *));
            if (this->primary_hashtable == nullptr){
                this->primary_hashtable = this->secondary_hashtable;
                this->is_hashtable_expanding = false;
                return;
            }
            for (uint32_t i = 0; i < new_hash_size; i++){
                this->primary_hashtable[i] = nullptr;
            }

            entry_t *old_hash_item;
            //拷贝数据
            uint32_t old_hash_size = HASH_SIZE(this->hash_power);
            for (int i = 0; i < old_hash_size; i++){
                old_hash_item = this->secondary_hashtable[i];
                //忽略掉空的
                if (old_hash_item == nullptr){
                    continue;
                }

                //此时会锁hash表的
                std::mutex *hash_mtx = this->get_hashtable_lock(old_hash_item->hash());
                hash_mtx->lock();

                //迁移数据
                uint32_t hash_val = old_hash_item->hash();
                uint32_t bucket = hash_val & HASH_MASK(new_hash_power);
                while (old_hash_item != nullptr){
                    old_hash_item->hash_next = this->primary_hashtable[bucket];
                    this->primary_hashtable[bucket] = old_hash_item;
                    old_hash_item = old_hash_item->hash_next;
                }
                this->hashtable_expanding_index = i;
                hash_mtx->unlock();
            }

            //扩容完毕
            entry_t **tmp = this->secondary_hashtable;
            this->secondary_hashtable = this->primary_hashtable;
            this->hash_power = new_hash_power;
            this->is_hashtable_expanding = false;
            this->hashtable_expanding_index = -1;
            free(tmp);
        }

        /**
         * 获取hashtable锁
         */
        std::mutex *get_hashtable_lock(uint32_t hash_val){
            return this->hashtable_locks[hash_val & HASH_MASK(HASHTABLE_LOCK_POWER)];
        }

        /**
         * 获取所要操作的hashtable bucket，需要考虑是否在扩容
         */
        entry_t **get_hashtable_bucket(uint64_t hash_val){
            uint32_t old_bucket = hash_val & HASH_MASK(this->hash_power);
            uint32_t new_bucket = hash_val & HASH_MASK(this->hash_power + 1);
            //没有扩容的、或者相应的bucket已扩容完成要用primary表
            if (!this->is_hashtable_expanding){
                return &(this->primary_hashtable[old_bucket]);
            }
            if (this->hashtable_expanding_index >= 0 && old_bucket <= this->hashtable_expanding_index){
                return &(this->primary_hashtable[new_bucket]);
            }
            return &(this->secondary_hashtable[old_bucket]);
        }

        /**
         * hash表
         */
        entry_t **primary_hashtable;
        entry_t **secondary_hashtable;

        /**
         * 全局锁列表
         */
        std::vector< std::mutex * > hashtable_locks;

        /**
         * 当前的容量
         */
        std::atomic< uint8_t > hash_power;

        /**
         * 几个标志
         */
        std::atomic< bool > is_thread_stop;
        std::atomic< bool > is_hashtable_full;
        std::atomic< bool > is_hashtable_expanding;
        std::atomic< int64_t > hashtable_expanding_index;

        /**
         * 环形缓冲区
         */
        std::vector< ring_buffer_t * > buffers;
        uint64_t buffer_size;
        stats_t *stats;
    };
}
#endif //_RINGCACHE_RINGBUFFER_H_202103111139_
