/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __DICT_H
#define __DICT_H

#include "mt19937-64.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * 这个文件实现了一个内存哈希表，
 * 它支持插入、删除、替换、查找和获取随机元素等操作。
 *
 * 哈希表会自动在表的大小的二次方之间进行调整。
 *
 * 键的冲突通过链表来解决
 * */

#define DICT_OK 0  /* 操作成功 */
#define DICT_ERR 1 /* 操作失败 */

/* Unused arguments generate annoying warnings... */
/* 如果字典的私有数据不使用时，用这个宏来避免编译器错误 */
#define DICT_NOTUSED(V) ((void)V)

/* hash表节点 */
typedef struct dictEntry
{
    /* 键 */
    void *key;
    /* 值 */
    union
    {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    /* 指向下个哈希表节点，形成链表 */
    struct dictEntry *next;
} dictEntry;

/* 字典类型特定函数 */
typedef struct dictType
{
    uint64_t (*hashFunction)(const void *key);                             /* 计算哈希值的函数 */
    void *(*keyDup)(void *privdata, const void *key);                      /* 复制键的函数 */
    void *(*valDup)(void *privdata, const void *obj);                      /* 复制值的函数 */
    int (*keyCompare)(void *privdata, const void *key1, const void *key2); /* 对比键的函数 */
    void (*keyDestructor)(void *privdata, void *key);                      /* 销毁键的函数 */
    void (*valDestructor)(void *privdata, void *obj);                      /* 销毁值的函数 */
    int (*expandAllowed)(size_t moreMem, double usedRatio);
} dictType;

/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table. */
/* hash表结构 */
typedef struct dictht
{
    dictEntry **table;      /* 哈希表数组 */
    unsigned long size;     /* 哈希表大小 */
    unsigned long sizemask; /* 哈希表大小掩码，用于计算索引值，一般是 size-1 */
    unsigned long used;     /* 已经使用的哈希表节点数量 */
} dictht;

/* 字典结构 */
typedef struct dict
{
    dictType *type;      /* 字典类型，保存一些用于操作特定类型键值对的函数，跟 下面的 privdata 一样都是为了实现 多态性字典而设置的为 特殊类型数据服务的 */
    void *privdata;      /* 私有数据 */
    dictht ht[2];        /* 哈希表，一般有两部分，正常哈希表数据部分 + rehash重置哈希表的暂存数据部分 */
    long rehashidx;      /* 是否正在进行哈希重置，默认为不重置 rehashidx == -1 rehashing not in progress if rehashidx == -1 */
    int16_t pauserehash; /* >0则表示rehashing暂停了 If >0 rehashing is paused (<0 indicates coding error) */
} dict;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
/*
 * 字典迭代器
 *
 * 如果 safe 属性的值为 1 ，那么在迭代进行的过程中，
 * 程序仍然可以执行 dictAdd 、 dictFind 和其他函数，对字典进行修改。
 *
 * 如果 safe 不为 1 ，那么程序只会调用 dictNext 对字典进行迭代，
 * 而不对字典进行修改。
 */
typedef struct dictIterator
{
    dict *d;              /* 被迭代的字典 */
    long index;           /* 迭代器当前所指向的哈希表索引位置。 */
    int table;            /* 正在被迭代的哈希表号码，值可以是 0 或 1 。 */
    int safe;             /* 标识这个迭代器是否安全 */
    dictEntry *entry;     /* 当前迭代到的节点的指针 */
    dictEntry *nextEntry; /* 当前迭代节点的下一个节点;因为在安全迭代器运作时， entry 所指向的节点可能会被修改，所以需要一个额外的指针来保存下一节点的位置，从而防止指针丢失 */
    /* unsafe iterator fingerprint for misuse detection. */
    long long fingerprint; /* 是一个64位的hash值, 是根据字典的内存地址生成的, 代表着字典当前的状态, 非安全迭代中, 如果字典内存发生了新的变化, 则 fingerprint 的值也会跟着变, 用于非安全迭代的快速失败 */
} dictIterator;

typedef void(dictScanFunction)(void *privdata, const dictEntry *de);
typedef void(dictScanBucketFunction)(void *privdata, dictEntry **bucketref);

/* This is the initial size of every hash table */
/* 哈希表的初始大小 */
#define DICT_HT_INITIAL_SIZE 4

/* ------------------------------- Macros ------------------------------------*/
/* 释放给定字典节点的值 */
#define dictFreeVal(d, entry)     \
    if ((d)->type->valDestructor) \
    (d)->type->valDestructor((d)->privdata, (entry)->v.val)

/* 设置给定字典节点的值 */
#define dictSetVal(d, entry, _val_)                                   \
    do                                                                \
    {                                                                 \
        if ((d)->type->valDup)                                        \
            (entry)->v.val = (d)->type->valDup((d)->privdata, _val_); \
        else                                                          \
            (entry)->v.val = (_val_);                                 \
    } while (0)

/* 将一个有符号整数设为节点的值 */
#define dictSetSignedIntegerVal(entry, _val_) \
    do                                        \
    {                                         \
        (entry)->v.s64 = _val_;               \
    } while (0)

/* 将一个无符号整数设为节点的值 */
#define dictSetUnsignedIntegerVal(entry, _val_) \
    do                                          \
    {                                           \
        (entry)->v.u64 = _val_;                 \
    } while (0)

/* 将一个double设为节点的值 */
#define dictSetDoubleVal(entry, _val_) \
    do                                 \
    {                                  \
        (entry)->v.d = _val_;          \
    } while (0)

/* 释放给定字典节点的键 */
#define dictFreeKey(d, entry)     \
    if ((d)->type->keyDestructor) \
    (d)->type->keyDestructor((d)->privdata, (entry)->key)

/* 设置给定字典节点的键 */
#define dictSetKey(d, entry, _key_)                                 \
    do                                                              \
    {                                                               \
        if ((d)->type->keyDup)                                      \
            (entry)->key = (d)->type->keyDup((d)->privdata, _key_); \
        else                                                        \
            (entry)->key = (_key_);                                 \
    } while (0)

/* 比对两个键 */
#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? (d)->type->keyCompare((d)->privdata, key1, key2) : (key1) == (key2))

#define dictHashKey(d, key) (d)->type->hashFunction(key) /* 计算给定键的哈希值 */
#define dictGetKey(he) ((he)->key)                       /* 返回获取给定节点的键 */
#define dictGetVal(he) ((he)->v.val)                     /* 返回获取给定节点的值 */
#define dictGetSignedIntegerVal(he) ((he)->v.s64)        /* 返回获取给定节点的有符号整数值 */
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)      /* 返回给定节点的无符号整数值 */
#define dictGetDoubleVal(he) ((he)->v.d)                 /* 返回给定节点的double值 */
#define dictSlots(d) ((d)->ht[0].size + (d)->ht[1].size) /* 返回给定字典的大小 */
#define dictSize(d) ((d)->ht[0].used + (d)->ht[1].used)  /* 返回字典的已有节点数量 */
#define dictIsRehashing(d) ((d)->rehashidx != -1)        /* 查看字典是否正在 rehash*/
#define dictPauseRehashing(d) (d)->pauserehash++         /* 暂停rehash */
#define dictResumeRehashing(d) (d)->pauserehash--        /* 继续rehash */

/* If our unsigned long type can store a 64 bit number, use a 64 bit PRNG. */
#if ULONG_MAX >= 0xffffffffffffffff
#define randomULong() ((unsigned long)genrand64_int64())
#else
#define randomULong() random()
#endif

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);             /* 创建一个新的字典 */
int dictExpand(dict *d, unsigned long size);                     /* 创建或扩展一个新的哈希表 */
int dictTryExpand(dict *d, unsigned long size);                  /* try创建或扩展一个新的哈希表 */
int dictAdd(dict *d, void *key, void *val);                      /* 将给定键值对添加到字典中 */
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing); /* 将给定键值对添加到字典中 */
dictEntry *dictAddOrFind(dict *d, void *key);                    /* 添加新的节点或者查找已存在节点 */
int dictReplace(dict *d, void *key, void *val);                  /* 将给定的键值对添加到字典中，如果键已经存在，那么删除旧有的键值对。 */
int dictDelete(dict *d, const void *key);                        /* 从字典中删除包含给定键的节点 */
dictEntry *dictUnlink(dict *ht, const void *key);                /* 删除给定key的节点，并释放内存 */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);              /* 释放节点内存 */
void dictRelease(dict *d);                                       /* 删除并释放整个字典 */
dictEntry *dictFind(dict *d, const void *key);                   /* 返回字典中包含键 key 的节点 */
void *dictFetchValue(dict *d, const void *key);                  /* 获取包含给定键的节点的值 */
int dictResize(dict *d);                                         /* 调整字典大小，让它的已用节点数和字典大小之间的比率接近 1:1 */
dictIterator *dictGetIterator(dict *d);                          /* 创建并返回给定字典的不安全迭代器 */
dictIterator *dictGetSafeIterator(dict *d);                      /* 创建并返回给定字典的安全迭代器 */
dictEntry *dictNext(dictIterator *iter);                         /* 返回迭代器指向的当前节点，字典迭代完毕时，返回 NULL */
void dictReleaseIterator(dictIterator *iter);                    /* 释放给定字典迭代器 */
dictEntry *dictGetRandomKey(dict *d);                            /* 随机返回字典中任意一个节点。 */
dictEntry *dictGetFairRandomKey(dict *d);
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
void dictGetStats(char *buf, size_t bufsize, dict *d); /* 获取hash表状态 */
uint64_t dictGenHashFunction(const void *key, int len);
uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void *)); /* 清空字典上的所有哈希表节点，并重置字典属性 */
void dictEnableResize(void);                     /* 使能rehash */
void dictDisableResize(void);                    /* 禁用rehash */
int dictRehash(dict *d, int n);                  /* 执行 N 步渐进式 rehash */
int dictRehashMilliseconds(dict *d, int ms);     /* 在给定毫秒数内，以 100 步为单位，对字典进行 rehash */
void dictSetHashFunctionSeed(uint8_t *seed);
uint8_t *dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata); /* dictScan() 函数用于迭代给定字典中的元素 */
uint64_t dictGetHash(dict *d, const void *key); /* 获取哈希值 */                                                          /* 获取哈希值 */
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#ifdef REDIS_TEST
int dictTest(int argc, char *argv[], int accurate);
#endif

#endif /* __DICT_H */
