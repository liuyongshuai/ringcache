/*************************************************************************
 * File:	entry.h
 * Author:	liuyongshuai<liuyongshuai@hotmail.com>
 * Time:	2021-03-11 11:30
 ************************************************************************/
#ifndef _RINGCACHE_COMMON_H_202103111130_
#define _RINGCACHE_COMMON_H_202103111130_

#include <string>
#include <vector>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <atomic>
#include <assert.h>
#include <mutex>
#include "jenkins_hash.h"

//hash计算
#define HASH_SIZE(n) ((uint32_t)1<<(n))
#define HASH_MASK(n) (HASH_SIZE(n)-1)

//hash初始容量及最大容量
#define HASH_POWER_INIT 16
#define HASH_POWER_MAX 32

//大小定义
#define KB (1<<10)
#define MB (1<<20)
#define GB (1<<30)

//全局锁相关
#define HASHTABLE_LOCK_POWER 10

//key && value 的最大长度
#define MAX_KEY_SIZE 255
#define MAX_VALUE_SIZE ((uint32_t)(4*MB))

//预估的数据平均大小
#define AVG_DATA_SIZE 512

//buffer相关，最小的buffer大小及固定的buffer数量，QPS过高时可调大buffer数量
#ifndef RING_BUFFER_NUM
#define RING_BUFFER_NUM 256
#endif
#define RING_BUFFER_MIN_SIZE (MAX_VALUE_SIZE*2)

//错误码相关
#define RINGCACHE_ERRNO_OK 0
#define RINGCACHE_ERRNO_KEY_TOO_LONG 2
#define RINGCACHE_ERRNO_VALUE_TOO_LONG 3
#define RINGCACHE_ERRNO_NOT_FOUND 4
#define RINGCACHE_ERRNO_ALLOC_MEMORY_FAILED 5
#define RINGCACHE_ERRNO_KEY_EXPIRED 6

inline uint32_t hash(const std::string &key){
    return jenkins_hash(key.c_str(), key.length());
}

namespace ringcache{
    /**
     * buffer的统计信息
     */
    typedef struct __attribute__ ((__packed__)) _buffer_stats_t{
        /**
         * 编号
         */
        uint32_t index;

        /**
         * 总数量
         */
        uint64_t item_num;

        /**
        * 总写入数量
        */
        uint64_t set_num;

        /**
        * 总删除数量
        */
        uint64_t del_num;

        /**
         * 此环形缓存区有多少次直接跳到header
         */
        uint64_t reset_header_times;

        /**
         * 缓存大小
         */
        uint64_t cache_byte_size;

        /**
         * 转化为字符串
         */
        std::string to_string(){
            std::string stats;
            stats.append("buffer" + std::to_string(this->index) + ": ");
            stats.append("\titem_num=" + std::to_string(this->item_num));
            stats.append("\tset_num=" + std::to_string(this->set_num));
            stats.append("\tdel_num=" + std::to_string(this->del_num));
            stats.append("\tcache_byte_size=" + std::to_string(this->cache_byte_size / MB) + "MB");
            stats.append("\treset_header_times=" + std::to_string(this->reset_header_times));
            return stats;
        }
    } buffer_stats_t;

    /**
     * 环形缓冲区
     */
    typedef struct __attribute__ ((__packed__)) _ring_buffer_t{
        /**
         * 基地址，释放空间用的
         */
        char *mem_begin;
        char *mem_end;

        /**
         * 当前缓冲区的字节数
         */
        uint64_t mem_size;

        /**
         * 统计信息
         */
        buffer_stats_t *stats;

        /**
         * 当前可以操作的指针
         */
        char *mem_cur_ptr;

        /**
         * 取空间时锁定
         */
        std::mutex *mtx;
    } ring_buffer_t;


#pragma pack (1)
    /**
     * 存数据的结构体
     */
    typedef struct __attribute__ ((__packed__))  _entry_t{
        /**
         * 当前entry所占用的全部字节数
         * 包括元数据（sizeof(entry_t)）、数据、补齐填充用的字节数
         */
        uint64_t entry_len;

        /**
         * hash表
         */
        struct _entry_t *hash_next;

        /**
         * 过期时间
         */
        uint32_t expire_time;

        /**
         * key的长度
         */
        uint8_t key_len;

        /**
         * value长度
         */
        uint32_t value_len;

        /**
         * 存储数据的地址
         */
        char data[];

        /**
         * 提取key
         */
        void key(std::string &key) const{
            key.clear();
            key.append(this->data, this->key_len);
        }

        /**
         * 提取value
         */
        void value(std::string &value) const{
            value.clear();
            value.append(this->data + this->key_len, this->value_len);
        }

        /**
         * hash值
         */
        uint32_t hash() const{
            std::string key;
            this->key(key);
            return ::hash(key);
        }

        /**
         * 计算所处的hashtable的bucket
         */
        uint32_t hash_table_bucket(uint32_t hash_mask) const{
            return (this->hash() & hash_mask);
        }

        /**
         * 总长度
         */
        uint64_t len() const{
            return this->entry_len;
        }
    } entry_t;
#pragma pack ()

    /**
     * 总的统计信息
     */
    typedef struct _stats_t{
        /**
         * 缓存个数
         */
        uint64_t buffer_num;

        /**
         * 各个缓冲区的统计信息
         */
        std::vector< buffer_stats_t * > buffer_stats;

        /**
         *  总数量大小
         */
        uint64_t item_num() const{
            uint64_t ret = 0;
            for (auto it:this->buffer_stats){
                ret += it->item_num;
            }
            return ret;
        }

        /**
         * 总缓存大小
         */
        std::string cache_size() const{
            std::string csize;
            uint64_t msize = 0;
            for (auto it:this->buffer_stats){
                msize += it->cache_byte_size;
            }
            char buf[32] = {0};
            msize = msize / MB;
            if (msize >= 1024){
                sprintf(buf, "%.2fG", (double) msize / 1024.0);
            }
            else{
                sprintf(buf, "%lluM", msize);
            }
            csize.append(buf, strlen(buf));
            return csize;
        }

        /**
         * 转化为字符串
         */
        std::string to_string() const{
            std::string stats;
            stats.append("\n---------------------stats---------------------\n");
            stats.append("item_num=" + std::to_string(this->item_num()));
            stats.append("\tcache_size=" + this->cache_size());
            stats.append("\tbuffer_num=" + std::to_string(this->buffer_num));
            for (auto it:this->buffer_stats){
                stats.append("\n\t -" + it->to_string());
            }
            return stats;
        }
    } stats_t;
}
#endif //_RINGCACHE_COMMON_H_202103111130_
