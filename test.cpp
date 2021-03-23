
/*************************************************************************
 * File:	test.cpp
 * Author:	liuyongshuai<liuyongshuai@hotmail.com>
 * Time:	2021-03-23 11:52
 ************************************************************************/
#include<stdlib.h>
#include<unistd.h>
#include<string.h>
#include<stdio.h>
#include<stdint.h>

//目前划分为2个缓冲区
#define RING_BUFFER_NUM 2
#include "ringcache/ringcache.h"

int main(){
    //16M大小的缓存
    ringcache::ringcache *cache = new ringcache::ringcache(16);
    cache->set("key1", "value1", 0);
    cache->set("key2", "value2", 0);
    cache->set("key3", "value3", 0);
    std::string val;
    cache->get("key1", val);
    std::cout << "val1=" << val << std::endl;
    cache->get("key2", val);
    std::cout << "val2=" << val << std::endl;
    cache->get("key3", val);
    std::cout << "val3=" << val << std::endl;
}

