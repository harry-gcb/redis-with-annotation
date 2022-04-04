/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
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

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#include "redisassert.h"

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */
/* 指示字典是否启用 rehash 的标识 */
static int dict_can_resize = 1;
/* 强制 rehash 的比率 */
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict *ht);
static unsigned long _dictNextPower(unsigned long size);
static long _dictKeyIndex(dict *ht, const void *key, uint64_t hash, dictEntry **existing);
static int _dictInit(dict *ht, dictType *type, void *privDataPtr);

/* -------------------------- hash functions -------------------------------- */

static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed)
{
    memcpy(dict_hash_function_seed, seed, sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void)
{
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, int len)
{
    return siphash(key, len, dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len)
{
    return siphash_nocase(buf, len, dict_hash_function_seed);
}

/* ----------------------------- API implementation ------------------------- */

/* Reset a hash table already initialized with ht_init().
 * NOTE: This function should only be called by ht_destroy(). */
/* 重置（或初始化）给定哈希表的各项属性值 */
static void _dictReset(dictht *ht)
{
    ht->table = NULL;
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}

/* Create a new hash table */
/* 创建一个新的字典 */
dict *dictCreate(dictType *type,
                 void *privDataPtr)
{
    /* 分配内存 */
    dict *d = zmalloc(sizeof(*d));
    /* 初始化hash table*/
    _dictInit(d, type, privDataPtr);
    return d;
}

/* Initialize the hash table */
/* 初始化哈希表 */
int _dictInit(dict *d, dictType *type,
              void *privDataPtr)
{
    /* 初始化两个哈希表的各项属性值，但暂时还不分配内存给哈希表数组 */
    _dictReset(&d->ht[0]);
    _dictReset(&d->ht[1]);
    d->type = type;            /* 设置类型特定函数 */
    d->privdata = privDataPtr; /* 设置私有数据 */
    d->rehashidx = -1;         /* 设置哈希表 rehash 状态 */
    d->pauserehash = 0;        /* rehash过程暂停 */
    return DICT_OK;
}

/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USED/BUCKETS ratio near to <= 1 */
/**
 * 调整字典大小，让它的已用节点数和字典大小之间的比率接近 1:1
 * 返回 DICT_ERR 表示字典已经在 rehash ，或者 dict_can_resize 为假
 * 成功创建体积更小的 ht[1] ，可以开始 resize 时，返回 DICT_OK。
 * */

int dictResize(dict *d)
{
    /* 字典数组最小长度 */
    unsigned long minimal;

    /* 不允许调整或者正在rehash时禁止调整，直接返回*/
    if (!dict_can_resize || dictIsRehashing(d))
        return DICT_ERR;
    /* 获取hash表的元素数量，计算让比率接近 1：1 所需要的最少节点数量 */
    minimal = d->ht[0].used;
    if (minimal < DICT_HT_INITIAL_SIZE) /* 容量最小为4 */
        minimal = DICT_HT_INITIAL_SIZE;
    /* 调整字典的大小 */
    return dictExpand(d, minimal);
}

/* Expand or create the hash table,
 * when malloc_failed is non-NULL, it'll avoid panic if malloc fails (in which case it'll be set to 1).
 * Returns DICT_OK if expand was performed, and DICT_ERR if skipped. */
/**
 * 创建或扩展一个新的哈希表，并根据字典的情况，选择以下其中一个动作来进行： 
 * 1) 如果字典的 0 号哈希表为空，那么将新哈希表设置为 0 号哈希表
 * 2) 如果字典的 0 号哈希表非空，那么将新哈希表设置为 1 号哈希表，
 *    并打开字典的 rehash 标识，使得程序可以开始对字典进行 rehash
 * 
 * size 参数不够大，或者 rehash 已经在进行时，返回 DICT_ERR 。
 * 成功创建 0 号哈希表，或者 1 号哈希表时，返回 DICT_OK 。
 * T = O(N)
 */
int _dictExpand(dict *d, unsigned long size, int *malloc_failed)
{
    /* 参数校验 */
    if (malloc_failed)
        *malloc_failed = 0;

    /* the size is invalid if it is smaller than the number of
     * elements already inside the hash table */
    /**字典正在rehash时禁止扩展 
     * size 的值也不能小于 0 号哈希表的当前已使用节点（扩展的大小放不下节点）
     */
    if (dictIsRehashing(d) || d->ht[0].used > size)
        return DICT_ERR;

    /* 新的哈希表 */
    dictht n; /* the new hash table */
    /* 根据 size 参数，计算哈希表的大小 */
    unsigned long realsize = _dictNextPower(size);

    /* Detect overflows */
    /* 检测是否溢出 */
    if (realsize < size || realsize * sizeof(dictEntry *) < realsize)
        return DICT_ERR;

    /* Rehashing to the same table size is not useful. */
    /* 重新散列到相同的表大小没有用 */
    if (realsize == d->ht[0].size)
        return DICT_ERR;

    /* Allocate the new hash table and initialize all pointers to NULL */
    /* 赋值 */
    n.size = realsize;
    n.sizemask = realsize - 1;
    /* 为哈希表分配空间，并将所有指针指向 NULL*/
    if (malloc_failed)
    {
        /* 避免分配失败报错 */
        /* 尝试分配hash数组 */
        n.table = ztrycalloc(realsize * sizeof(dictEntry *));
        /* 设置是否分配失败, n.table == NULL 就是分配失败 */
        *malloc_failed = n.table == NULL;
        /* 如果分配失败, 则返回错误 */
        if (*malloc_failed)
            return DICT_ERR;
    }
    else
        /* 分配hash数组, 分配失败则终止程序 */
        n.table = zcalloc(realsize * sizeof(dictEntry *));

    /* 设置节点数为0 */
    n.used = 0;

    /* Is this the first initialization? If so it's not really a rehashing
     * we just set the first hash table so that it can accept keys. */
    /**
     * 如果 0 号哈希表为空，那么这是一次初始化： 
     * 程序将新哈希表赋给 0 号哈希表的指针，然后字典就可以开始处理键值对了。
     * */
    if (d->ht[0].table == NULL)
    {
        d->ht[0] = n;
        return DICT_OK;
    }

    /* Prepare a second hash table for incremental rehashing */
    /* 如果 0 号哈希表非空，那么这是一次 rehash ： 
     * 程序将新哈希表设置为 1 号哈希表，
     * 并将字典的 rehash 标识打开，让程序可以开始对字典进行 rehash
     */
    d->ht[1] = n;
    d->rehashidx = 0;
    return DICT_OK;
}

/* return DICT_ERR if expand was not performed */
int dictExpand(dict *d, unsigned long size)
{
    return _dictExpand(d, size, NULL);
}

/* return DICT_ERR if expand failed due to memory allocation failure */
int dictTryExpand(dict *d, unsigned long size)
{
    int malloc_failed;
    _dictExpand(d, size, &malloc_failed);
    return malloc_failed ? DICT_ERR : DICT_OK;
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
/**执行 N 步渐进式 rehash
 * 返回 1 表示仍有键需要从 0 号哈希表移动到 1 号哈希表，
 * 返回 0 则表示所有键都已经迁移完毕。
 * 
 * 注意，每步 rehash 都是以一个哈希表索引（桶）作为单位的，
 * 一个桶里可能会有多个节点，
 * 被 rehash 的桶里的所有节点都会被移动到新哈希表。
 */
int dictRehash(dict *d, int n)
{
    /* 访问空位置次数最大值，以为 rehash 转移都是 一部分一部分转移的，
     * 因为有一些位置是空的，所以并不能保证每一步都至少有一个位置转移，
     * 所以我们规定这个操作只能访问最多 n*10 个空位置，否则太多遍历会造成阻塞时间过久 
     * */
    int empty_visits = n * 10; /* Max number of empty buckets to visit. */
    /* 只可以在 rehash 进行中时执行 */
    if (!dictIsRehashing(d))
        return 0;

    /* 进行 N 步迁移 */
    while (n-- && d->ht[0].used != 0)
    {
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        /* rehash的索引rehashidx不可以溢出，确保rehashidx不越界 */
        assert(d->ht[0].size > (unsigned long)d->rehashidx);
        /* 略过数组中为空的索引，找到下一个非空索引 */
        while (d->ht[0].table[d->rehashidx] == NULL)
        {
            d->rehashidx++;
            /* 如果空桶的次数超过n*10次了，则直接退出，检测的次数太多了*/
            if (--empty_visits == 0)
                return 1;
        }
        /* 指向该索引的链表表头节点 */
        de = d->ht[0].table[d->rehashidx];
        /* Move all the keys in this bucket from the old to the new hash HT */
        /* 将链表中的所有节点迁移到新哈希表 */
        while (de)
        {
            uint64_t h;
            /* 保存下个节点的指针 */
            nextde = de->next;
            /* Get the index in the new hash table */
            /* 计算新哈希表的哈希值，得到节点插入的索引位置 */
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;
            /* 插入节点到新哈希表 */
            de->next = d->ht[1].table[h];
            d->ht[1].table[h] = de;
            /* 更新计数器 */
            d->ht[0].used--;
            d->ht[1].used++;
            /* 继续处理下个节点 */
            de = nextde;
        }
        /* 将刚迁移完的哈希表索引的指针设为空 */
        d->ht[0].table[d->rehashidx] = NULL;
        /* 更新 rehash 索引 */
        d->rehashidx++;
    }

    /* Check if we already rehashed the whole table... */
    /* 如果rehash过程将哈希表0的元素都迁移完，则将哈希表1的数据转移到哈希表0（指针重新指一下就好了），并清空哈希表1*/
    if (d->ht[0].used == 0)
    {
        zfree(d->ht[0].table); /* 释放 ht[0] */
        d->ht[0] = d->ht[1];   /* 用新的hash表替换旧hash表，赋值相当于拷贝 */
        _dictReset(&d->ht[1]); /* 重置 ht[1] 和 rehashidx，留作下次使用 */
        d->rehashidx = -1;
        return 0;
    }

    /* More to rehash... */
    return 1;
}

/* 返回以毫秒为单位的 UNIX 时间戳 */
long long timeInMilliseconds(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec) * 1000) + (tv.tv_usec / 1000);
}

/* Rehash in ms+"delta" milliseconds. The value of "delta" is larger 
 * than 0, and is smaller than 1 in most cases. The exact upper bound 
 * depends on the running time of dictRehash(d,100).*/
/* 在给定毫秒数内，以 100 步为单位，对字典进行 rehash */
int dictRehashMilliseconds(dict *d, int ms)
{
    /* 如果rehash是暂停状态，直接返回 */
    if (d->pauserehash > 0)
        return 0;
    /* 记录开始时间 */
    long long start = timeInMilliseconds();
    int rehashes = 0;

    while (dictRehash(d, 100))
    {
        /* 记录rehash的个数 */
        rehashes += 100;
        /* 如果时间已过，跳出 */
        if (timeInMilliseconds() - start > ms)
            break;
    }
    /* 返回ms内，rehash的次数 */
    return rehashes;
}

/* This function performs just a step of rehashing, and only if hashing has
 * not been paused for our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some element can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
/**在字典不存在安全迭代器的情况下，对字典进行单步 rehash 
 * 字典有安全迭代器的情况下不能进行 rehash，因为两种不同的迭代和修改操作可能会弄乱字典。
 * 这个函数被多个通用的查找、更新操作调用，它可以让字典在被使用的同时进行 rehash 。
 */
static void _dictRehashStep(dict *d)
{
    if (d->pauserehash == 0)
        dictRehash(d, 1);
}

/* Add an element to the target hash table */
/**
 * 尝试将给定键值对添加到字典中，只有给定键 key 不存在于字典时，添加操作才会成功
 */
int dictAdd(dict *d, void *key, void *val)
{
    /* 尝试添加键到字典，并返回包含了这个键的新哈希节点 */
    dictEntry *entry = dictAddRaw(d, key, NULL);
    /* 键已存在，添加失败 */
    if (!entry)
        return DICT_ERR;
    /* 键不存在，设置节点的值 */
    dictSetVal(d, entry, val);
    /* 添加成功 */
    return DICT_OK;
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as they wish.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */

/**
 * @brief 尝试将键插入到字典中
 * 如果键已经在字典存在，那么返回 NULL
 * 如果键不存在，那么程序创建新的哈希节点，
 * 将节点和键关联，并插入到字典，然后返回节点本身。
 *
 */
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing)
{
    long index;
    dictEntry *entry;
    dictht *ht;

    /* 如果条件允许的话，进行单步 rehash */
    if (dictIsRehashing(d))
        _dictRehashStep(d);

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    /* 计算键在哈希表中的索引值，如果值为 -1 ，那么表示键已经存在 */
    if ((index = _dictKeyIndex(d, key, dictHashKey(d, key), existing)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */
    /* 如果字典正在 rehash ，那么将新键添加到 1 号哈希表，否则，将新键添加到 0 号哈希表*/
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
    /* 为新节点分配空间 */
    entry = zmalloc(sizeof(*entry));
    /* 将新节点插入到链表表头 */
    entry->next = ht->table[index];
    ht->table[index] = entry;
    /* 更新哈希表已使用节点数量 */
    ht->used++;

    /* Set the hash entry fields. */
    /* 设置新节点的键 */
    dictSetKey(d, entry, key);
    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
/**
 * @brief 将给定的键值对添加到字典中，如果键已经存在，那么删除旧有的键值对。
 * 果键值对为全新添加，那么返回 1 。
 * 如果键值对是通过对原有的键值对更新得来的，那么返回 0 。
 * @param d
 * @param key
 * @param val
 * @return int
 */
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, *existing, auxentry;

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
    /* 尝试直接将键值对添加到字典, 如果键 key 不存在的话，添加会成功*/
    entry = dictAddRaw(d, key, &existing);
    if (entry)
    {
        dictSetVal(d, entry, val);
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    /* 先保存原有的值的指针，这里保存的主要是之前的val，后面用来释放；
     * 为什么不先释放再重新设置值呢？
     * 设置新值并释放旧值。请注意，按此顺序执行此操作非常重要，因为值可能与上一个值完全相同。
     * 在这种情况下，考虑引用计数，您应该递增（set），然后递减（free），而不是相反。
     */
    auxentry = *existing;
    /* 然后设置新的值 */
    dictSetVal(d, existing, val);
    /* 然后释放旧值 */
    dictFreeVal(d, &auxentry);
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
/* 添加新的节点或者查找已存在节点 */
dictEntry *dictAddOrFind(dict *d, void *key)
{
    dictEntry *entry, *existing;
    entry = dictAddRaw(d, key, &existing);
    return entry ? entry : existing;
}

/* Search and remove an element. This is an helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions. */

/**
 * @brief 查找并删除包含给定键的节点，参数 nofree 决定是否调用键和值的释放函数
 *  0 表示调用，1 表示不调用
 * 找到并成功删除返回 DICT_OK ，没找到则返回 DICT_ERR
 */
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree)
{
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;

    /* 字典的哈希表为空 */
    if (d->ht[0].used == 0 && d->ht[1].used == 0)
        return NULL;

    /* 进行单步rehash */
    if (dictIsRehashing(d))
        _dictRehashStep(d);
    /* 计算哈希值 */
    h = dictHashKey(d, key);

    /* 遍历哈希表 */
    for (table = 0; table <= 1; table++)
    {
        /* 计算索引值 */
        idx = h & d->ht[table].sizemask;
        /* 指向该索引上的链表 */
        he = d->ht[table].table[idx];
        prevHe = NULL;
        /* 遍历链表上的所有节点 */
        while (he)
        {
            /* 查找目标节点 */
            if (key == he->key || dictCompareKeys(d, key, he->key))
            {
                /* Unlink the element from the list */
                /* 从链表上移除节点*/
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht[table].table[idx] = he->next;
                /* 调用释放键和值的free函数 */
                if (!nofree)
                {
                    dictFreeKey(d, he);
                    dictFreeVal(d, he);
                    zfree(he);
                }
                /* 更新已使用的节点数量*/
                d->ht[table].used--;
                /* 返回链表节点指针 */
                return he;
            }
            prevHe = he;
            he = he->next;
        }
        /* 如果执行到这里，说明在 0 号哈希表中找不到给定键 
         * 那么根据字典是否正在进行 rehash ，决定要不要查找 1 号哈希表
         */
        if (!dictIsRehashing(d))
            break;
    }
    return NULL; /* not found */
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. */
/**
 * 从字典中删除包含给定键的节点
 * 并且调用键值的释放函数来删除键值
 * 找到并成功删除返回 DICT_OK ，没找到则返回 DICT_ERR
 */
int dictDelete(dict *ht, const void *key)
{
    return dictGenericDelete(ht, key, 0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 */
/* 删除给定key的节点，并释放内存 */
dictEntry *dictUnlink(dict *ht, const void *key)
{
    return dictGenericDelete(ht, key, 1);
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
/* 释放节点内存 */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he)
{
    if (he == NULL)
        return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    zfree(he);
}

/* Destroy an entire dictionary */
/* 删除哈希表上的所有节点，并重置哈希表的各项属性 */
int _dictClear(dict *d, dictht *ht, void(callback)(void *))
{
    unsigned long i;

    /* Free all the elements */
    /* 遍历整个哈希表 */
    for (i = 0; i < ht->size && ht->used > 0; i++)
    {
        dictEntry *he, *nextHe;
        /* 处理私有数据，(i & 65535) == 0 是为了跳过 0 的位置 */
        if (callback && (i & 65535) == 0)
            callback(d->privdata);
        /* 跳过空索引 */
        if ((he = ht->table[i]) == NULL)
            continue;
        /* 遍历整个链表 */
        while (he)
        {
            nextHe = he->next;
            dictFreeKey(d, he); /* 释放key对象 */
            dictFreeVal(d, he); /* 释放节点值对象 */
            zfree(he);          /* 释放节点 */
            ht->used--;
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
    /* 释放哈希表内存 */
    zfree(ht->table);
    /* Re-initialize the table */
    /* 重置哈希表结构 */
    _dictReset(ht);
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
/* 删除并释放整个字典 */
void dictRelease(dict *d)
{
    /* 删除并清空两个哈希表 */
    _dictClear(d, &d->ht[0], NULL);
    _dictClear(d, &d->ht[1], NULL);
    zfree(d);
}

/**返回字典中包含键 key 的节点
 * 找到返回节点，找不到返回 NULL
 */
dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    uint64_t h, idx, table;

    /* 字典（的哈希表）为空 */
    if (dictSize(d) == 0)
        return NULL; /* dict is empty */
    /* 如果条件允许的话，进行单步 rehash */
    if (dictIsRehashing(d))
        _dictRehashStep(d);
    /* 计算键的哈希值 */
    h = dictHashKey(d, key);
    /* 在字典的哈希表中查找这个键 */
    for (table = 0; table <= 1; table++)
    {
        /* 计算索引值 slot */
        idx = h & d->ht[table].sizemask;
        /* 遍历给定索引上的链表的所有节点，查找 key */
        he = d->ht[table].table[idx];
        while (he)
        {
            if (key == he->key || dictCompareKeys(d, key, he->key))
                return he;
            he = he->next;
        }
        /**如果程序遍历完 0 号哈希表，仍然没找到指定的键的节点
         * 那么程序会检查字典是否在进行 rehash ，
         * 然后才决定是直接返回 NULL ，还是继续查找 1 号哈希表
         */
        if (!dictIsRehashing(d))
            return NULL;
    }
    /* 进行到这里时，说明两个哈希表都没找到 */
    return NULL;
}

/* 获取包含给定键的节点的值 */
void *dictFetchValue(dict *d, const void *key)
{
    dictEntry *he;

    he = dictFind(d, key);
    return he ? dictGetVal(he) : NULL;
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
long long dictFingerprint(dict *d)
{
    long long integers[6], hash = 0;
    int j;

    integers[0] = (long)d->ht[0].table;
    integers[1] = d->ht[0].size;
    integers[2] = d->ht[0].used;
    integers[3] = (long)d->ht[1].table;
    integers[4] = d->ht[1].size;
    integers[5] = d->ht[1].used;

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++)
    {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

/* 创建并返回给定字典的不安全迭代器 */
dictIterator *dictGetIterator(dict *d)
{
    dictIterator *iter = zmalloc(sizeof(*iter));

    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

/* 创建并返回给定字典的安全迭代器 */
dictIterator *dictGetSafeIterator(dict *d)
{
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;
    return i;
}

/* 返回迭代器指向的当前节点，字典迭代完毕时，返回 NULL */
dictEntry *dictNext(dictIterator *iter)
{
    while (1)
    {
        /* 进入这个循环有两种可能：
         * 1) 这是迭代器第一次运行
         * 2) 当前索引链表中的节点已经迭代完（NULL 为链表的表尾）
         */
        if (iter->entry == NULL)
        {
            /* 指向被迭代的哈希表 */
            dictht *ht = &iter->d->ht[iter->table];
            if (iter->index == -1 && iter->table == 0)
            {
                /* 初次迭代时执行 */
                if (iter->safe)
                    dictPauseRehashing(iter->d); /* 如果是安全迭代器，那么更新安全迭代器计数器 */
                else
                    iter->fingerprint = dictFingerprint(iter->d); /* 如果是不安全迭代器，那么计算指纹 */
            }
            /* 更新hash表索引位置 */
            iter->index++;
            /* 如果迭代器的当前索引大于当前被迭代的哈希表的大小, 那么说明这个哈希表已经迭代完毕 */
            if (iter->index >= (long)ht->size)
            {
                /* 如果正在 rehash 的话，那么说明 1 号哈希表也正在使用中, 那么继续对 1 号哈希表进行迭代*/
                if (dictIsRehashing(iter->d) && iter->table == 0)
                {
                    iter->table++;
                    iter->index = 0;
                    ht = &iter->d->ht[1];
                }
                else
                {
                    /* 如果没有 rehash ，那么说明迭代已经完成 */
                    break;
                }
            }
            /* 如果进行到这里，说明这个哈希表并未迭代完，更新节点指针，指向下个索引链表的表头节点*/
            iter->entry = ht->table[iter->index];
        }
        else
        {
            /* 执行到这里，说明程序正在迭代某个链表，将节点指针指向链表的下个节点 */
            iter->entry = iter->nextEntry;
        }
        if (iter->entry)
        {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            /* 如果当前节点不为空，那么也记录下该节点的下个节点，因为安全迭代器有可能会将迭代器返回的当前节点删除*/
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    /* 迭代完毕 */
    return NULL;
}

/* 释放给定字典迭代器 */
void dictReleaseIterator(dictIterator *iter)
{
    if (!(iter->index == -1 && iter->table == 0))
    {
        /* 释放安全迭代器时，继续rehash*/
        if (iter->safe)
            dictResumeRehashing(iter->d);
        else
            /* 释放不安全迭代器时，验证指纹是否变化 */
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
/**
 * 随机返回字典中任意一个节点。
 * 可用于实现随机化算法。
 * 如果字典为空，返回 NULL 。
 * */
dictEntry *dictGetRandomKey(dict *d)
{
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;

    /* 字典为空 */
    if (dictSize(d) == 0)
        return NULL;
    /* 进行单步 rehash */
    if (dictIsRehashing(d))
        _dictRehashStep(d);
    /* 如果正在 rehash ，那么将 1 号哈希表也作为随机查找的目标 */
    if (dictIsRehashing(d))
    {
        /* 循环随机获取, 直接到hash槽有节点存在为止 */
        do
        {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            /* 获取一个随机数, 然后根据两个hash表的长度计算hash槽. */
            /* randomULong() % (dictSlots(d) - d->rehashidx) 保证随机值不包括 rehashidx 之前的, 注意, 这里是取模不是& */
            h = d->rehashidx + (randomULong() % (dictSlots(d) - d->rehashidx));
            /* 如果算出来的随机hash槽大于旧hash表的长度, 则表示要获取新hash表的随机槽首节点, 否则获取旧hash表的随机槽首节点 */
            he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] : d->ht[0].table[h];
        } while (he == NULL);
    }
    else
    {
        /* 不在rehash, 只有一个hash表，只从 0 号哈希表中查找节点 */
        do
        {
            /* 生成随机数, 计算随机hash槽 */
            h = randomULong() & d->ht[0].sizemask;
            /* 获取随机hash槽的首节点 */
            he = d->ht[0].table[h];
            /* 节点为NULL, 则继续随机 */
        } while (he == NULL);
    }

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    /* 目前 he 已经指向一个非空的节点链表, 程序将从这个链表随机返回一个节点 */
    listlen = 0;
    orighe = he;
    /* 计算节点数量 */
    while (he)
    {
        he = he->next;
        listlen++;
    }
    /* 取模，得出随机节点的索引 */
    listele = random() % listlen;
    he = orighe;
    /* 按索引查找节点 */
    while (listele--)
        he = he->next;
    /* 返回随机节点 */
    return he;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
/* 随机采集指定数量的节点. 有可能返回的数量达不到 count 的个数. 
 * 如果要返回一些随机key, 这个函数比 dictGetRandomKey 快很多 
 */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count)
{
    unsigned long j; /* internal hash table id, 0 or 1. */
    /* hash表的数量, 值为1或者2 */
    unsigned long tables; /* 1 or 2 tables? */
    /* stored 表示已经采集的节点数, maxsizemask 表示容量的最大hash表的掩码 */
    unsigned long stored = 0, maxsizemask;
    /* 采集次数上限 */
    unsigned long maxsteps;

    /* 最多只能返回字典的总节点数. */
    if (dictSize(d) < count)
        count = dictSize(d);
    /* 采集次数上限为元素个数的10倍 */
    maxsteps = count * 10;

    /* Try to do a rehashing work proportional to 'count'. */
    /* 根据返回key的个数, 执行渐进式rehash操作 */
    for (j = 0; j < count; j++)
    {
        if (dictIsRehashing(d))
            _dictRehashStep(d);
        else
            break;
    }

    /* 如果字典正在rehash, 则需要遍历两个hash表, 否则就遍历一个 */
    tables = dictIsRehashing(d) ? 2 : 1;
    /* 获取hash表0的掩码作为最大掩码 */
    maxsizemask = d->ht[0].sizemask;
    /* 如果hash表数量大于1, 表示字典现在是rehash状态, 如果字典是rehash状态, 则对比两个hash表的掩码, 取最大的作为 maxsizemask*/
    if (tables > 1 && maxsizemask < d->ht[1].sizemask)
        maxsizemask = d->ht[1].sizemask;

    /* Pick a random point inside the larger table. */
    /* 获取随机数然后计算出一个随机hash槽. */
    unsigned long i = randomULong() & maxsizemask;
    /* 统计遍历了空的hash槽个数 */
    unsigned long emptylen = 0; /* Continuous empty entries so far. */
    /* 如果采样的key已经足够或者达到采样上限, 则退出循环 */
    while (stored < count && maxsteps--)
    {
        /* 遍历hash表数组进行采集 */
        for (j = 0; j < tables; j++)
        {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            /* 跳过已经迁移到新hash表的hash槽索引,tables == 2 字典表示正在rehash,j == 0 , 表示目前正在遍历旧hash表 
             * i < (unsigned long) d->rehashidx 表示i属于已经迁移的hash槽索引
             */
            if (tables == 2 && j == 0 && i < (unsigned long)d->rehashidx)
            {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                /* 如果当前随机索引大于hash表1的长度, 表示只能在hash表0中获取, 那么跳过 rehashidx 前面已经迁移的槽 */
                if (i >= d->ht[1].size)
                    i = d->rehashidx;
                else
                    continue; /* i 小于 rehashidx, 但是没有大于hash表1的容量, 直接跳过hash表0, 从hash表1中采样 */
            }
            /* 如果随机hash槽索引大于当前hash表数组的长度, 则不处理 */
            if (i >= d->ht[j].size)
                continue; /* Out of range for this table. */
            /* 获取hash表的首节点 */
            dictEntry *he = d->ht[j].table[i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            /* 如果首节点为 NULL */
            if (he == NULL)
            {
                /* 统计空的hash槽 */
                emptylen++;
                /* 如果空的hash槽个数超过5且超过 count, 重新生成随机hash槽索引, 并且重置空的hash槽统计 */
                if (emptylen >= 5 && emptylen > count)
                {
                    i = randomULong() & maxsizemask;
                    emptylen = 0;
                }
            }
            else
            {
                /* 首节点不为空, 重置空的hash槽统计 */
                emptylen = 0;
                /* 遍历链表 */
                while (he)
                {
                    /* Collect all the elements of the buckets found non
                     * empty while iterating. */
                    /* 将节点放进 dictEntry * 数组 */
                    *des = he;
                    /* 数组指针移动到下一个索引 */
                    des++;
                    /* 获取下一个节点 */
                    he = he->next;
                    /* 获取的节点数加1 */
                    stored++;
                    /* 如果获取的节点数已经满足, 则直接反回 */
                    if (stored == count)
                        return stored;
                }
            }
        }
        /* 获取下一个hash槽位置 */
        i = (i + 1) & maxsizemask;
    }
    return stored;
}

/* This is like dictGetRandomKey() from the POV of the API, but will do more
 * work to ensure a better distribution of the returned element.
 *
 * This function improves the distribution because the dictGetRandomKey()
 * problem is that it selects a random bucket, then it selects a random
 * element from the chain in the bucket. However elements being in different
 * chain lengths will have different probabilities of being reported. With
 * this function instead what we do is to consider a "linear" range of the table
 * that may be constituted of N buckets with chains of different lengths
 * appearing one after the other. Then we report a random element in the range.
 * In this way we smooth away the problem of different chain lengths. */
#define GETFAIR_NUM_ENTRIES 15
/* 公平地获取一个随机key. 
 * 为什么比 dictGetRandomKey 公平一点呢?. dictGetRandomKey 由于不同的槽, 链表的长度可能不一样, 就会导致概率的分布不一样
 * dictGetSomeKeys 返回的长度是固定的, 从固定的链表长度中随机节点, 相对于 dictGetRandomKey 链表长度不固定会公平一点
 */
dictEntry *dictGetFairRandomKey(dict *d)
{
    /* 节点数组 */
    dictEntry *entries[GETFAIR_NUM_ENTRIES];
    /* 随机获取15个节点 */
    unsigned int count = dictGetSomeKeys(d, entries, GETFAIR_NUM_ENTRIES);
    /* Note that dictGetSomeKeys() may return zero elements in an unlucky
     * run() even if there are actually elements inside the hash table. So
     * when we get zero, we call the true dictGetRandomKey() that will always
     * yield the element if the hash table has at least one. */
    /* 如果没有获取到, 则随机返回一个key */
    if (count == 0)
        return dictGetRandomKey(d);
    /* 在这一组节点中, 生成随机索引 */
    unsigned int idx = rand() % count;
    /* 在这一组节点中, 随机获取一个 */
    return entries[idx];
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
/* 对 v 进行二进制逆序操作, 这个算法有点意思, 
 * 可以看一下 https://www.cnblogs.com/gqtcgq/p/7247077.html */
static unsigned long rev(unsigned long v)
{
    /* CHAR_BIT 一般是8位, sizeof 表示v的字节, long是4个字节, 也就是 s=32, 也就是二进制的 100000 */
    unsigned long s = CHAR_BIT * sizeof(v); // bit size; must be power of 2
    /* ~0UL 相当于32个1 */
    unsigned long mask = ~0UL;
    /* s >>= 1 第一次移动之后就是 010000, 也就是16, 第三次为8, 依次类推4, 2, 1, 最多向右移动6次就为0了, 也就是说while有5次的遍历操作 */
    while ((s >>= 1) > 0)
    {
        /* mask << s相当于左移s位, 也就是保留低s位, 最终变成高s位为1, 低s位为0 */
        /* mask ^= (mask << s), mask结果为只有低s位都为1, 高s位都是0, 如: 也就是高16位都为0, 低16位都为1 */
        mask ^= (mask << s);
        /* (v >> s) & mask相当于将高s位移动到低s位 */
        /* ~mask表示高s位都是1, 低s位都是0, (v << s) & ~mask 相当将低s位移动到高s位 */
        /* 将两者 | , 表示将高s位与低s位互换了 */
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    /* 返回倒置的二进制 */
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
/**dictScan() 函数用于迭代给定字典中的元素 
 * 迭代的工作方式如下： 
 * 1）最初使用游标(v)值调用函数
 * 2）该函数执行迭代的一个步骤，并返回下一次调用中必须使用的新游标值
 * 3）当返回的游标为0时，迭代完成
 * 
 * 函数保证，在迭代从开始到结束期间，一直存在于字典的元素肯定会被迭代到，但一个元素可能会被返回多次。
 * 
 * 每当一个元素被返回时，回调函数 fn 就会被执行，fn 函数的第一个参数是 privdata ，而第二个参数则是字典节点 de 。
 * 
 * 工作原理
 * 
 * 迭代所使用的算法是由 Pieter Noordhuis 设计的，算法的主要思路是在二进制高位上对游标进行加法计算
 * 也即是说，不是按正常的办法来对游标进行加法计算，而是首先将游标的二进制位翻转（reverse）过来，
 * 然后对翻转后的值进行加法计算，最后再次对加法计算之后的结果进行翻转。
 * 
 * 这一策略是必要的，因为在一次完整的迭代过程中，哈希表的大小有可能在两次迭代之间发生改变。
 * 
 * 哈希表的大小总是 2 的某个次方，并且哈希表使用链表来解决冲突，
 * 因此一个给定元素在一个给定表的位置总可以通过 Hash(key) & SIZE-1公式来计算得出，
 * 其中 SIZE-1 是哈希表的最大索引值，这个最大索引值就是哈希表的 mask （掩码）。
 * 
 * 举个例子，如果当前哈希表的大小为 16 ，那么它的掩码就是二进制值 1111 ，
 * 这个哈希表的所有位置都可以使用哈希值的最后四个二进制位来记录。
 * 
 * 如果哈希表的大小改变了怎么办？
 * 当对哈希表进行扩展时，元素可能会从一个槽移动到另一个槽，
 * 举个例子，假设我们刚好迭代至 4 位游标 1100 ，
 * 而哈希表的 mask 为 1111 （哈希表的大小为 16 ）。
 * 
 * 如果这时哈希表将大小改为 64 ，那么哈希表的 mask 将变为 111111 ，
 * 
 * 限制
 * 
 * 这个迭代器是完全无状态的，这是一个巨大的优势，因为迭代可以在不使用任何额外内存的情况下进行。
 * 
 * 这个设计的缺陷在于：
 * 函数可能会返回重复的元素，不过这个问题可以很容易在应用层解决。
 * 
 * 为了不错过任何元素，迭代器需要返回给定桶上的所有键，以及因为扩展哈希表而产生出来的新表，所以迭代器必须在一次迭代中返回多个元素。
 * 
 * 对游标进行翻转（reverse）的原因初看上去比较难以理解，不过阅读这份注释应该会有所帮助。
 */
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       dictScanBucketFunction *bucketfn,
                       void *privdata)
{
    dictht *t0, *t1;
    const dictEntry *de, *next;
    unsigned long m0, m1;

    /* 跳过空字典 */
    if (dictSize(d) == 0)
        return 0;

    /* This is needed in case the scan callback tries to do dictFind or alike. */

    /* 如果字典正在rehash, 则停顿rehash */
    dictPauseRehashing(d);

    /* 如果字典没有在rehash, 迭代只有一个哈希表的字典 */
    if (!dictIsRehashing(d))
    {
        /* 获取hash表0的指针 */
        t0 = &(d->ht[0]);
        /* 获取hash表0的掩码 */
        m0 = t0->sizemask;

        /* Emit entries at cursor */
        /* 如果桶的回调函数存在, 则用回调函数处理要获取的桶 */
        if (bucketfn)
            bucketfn(privdata, &t0->table[v & m0]);
        /* 获取桶上的首节点 */
        de = t0->table[v & m0];
        /* 如果节点存在, 则遍历链表上的节点, 并且使用 fn 函数处理 */
        while (de)
        {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        /* 假如hash表0长度为8, 那么m0就应该为前29位为0, 后三位为1, 也就是 ...000111 */
        /* ~m0 也就是, ...111000, v |= ~m0 就相当于保留低位的数据, v最终结果为, 高29位为1, 低3位为实际数据, ...111xxx */
        v |= ~m0;

        /* Increment the reverse cursor */
        /* 反转游标, 就变成 xxx111...111 */
        v = rev(v);
        /* 游标加1, 因为低位都是1, 加1之后, 就会进1, 最终相当于实际数据加1, 其实就相当于xx(x + 1)000...000 */
        v++;
        /* 再次返回转回原来的顺序 */
        v = rev(v);
    }
    else
    {
        /* 获取字段的hash表0的引用 */
        t0 = &d->ht[0];
        /* 获取字典的hash表1的引用 */
        t1 = &d->ht[1];

        /* Make sure t0 is the smaller and t1 is the bigger table */
        /* 判断那个hash表的容量最小, 小容量的hash表为t0 */
        if (t0->size > t1->size)
        {
            t0 = &d->ht[1];
            t1 = &d->ht[0];
        }

        /* 获取t0的掩码 */
        m0 = t0->sizemask;
        /* 获取t1的掩码 */
        m1 = t1->sizemask;

        /* Emit entries at cursor */
        /* 如果 bucketfn 函数不为null, 则使用bucketfn对链表进行处理 */
        if (bucketfn)
            bucketfn(privdata, &t0->table[v & m0]);
        /* 获取游标对应的首节点 */
        de = t0->table[v & m0];
        /* 遍历链表 */
        while (de)
        {
            /* 获取下一个节点 */
            next = de->next;
            /* 处理当前节点 */
            fn(privdata, de);
            de = next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        /* 处理大hash表t1，小表的槽, 在按大表rehash后的槽都是相对固定的 */
        /* 假如小表容量是8, 则他的槽二进制就是三位, 如: 001, 010等等, 我们以abc表示3位二进制变量 */
        /* 当扩容到32, 则他们二进制位为5位, 如: 00010, 01010等, 我们以xxabc来表示5位后的二进制变量 */
        /* 也就是扩容后, 落在二进制abc的值, 很有可能会重hash后会落在xxabc中, */
        /* 所以我们扫描小表的abc后, 再将abc作为后缀, 穷举xxabc中的xx, 就可以获取rehash两张表中原来在同一个槽的key值 */
        /* 如果是大表变小表同理 */

        /* 具体实现就是在遍历完小表Cursor位置后，将小表Cursor位置可能Rehash到的大表所有位置全部遍历一遍，然后再返回遍历元素和下一个小表遍历位置。 */
        do
        {
            /* Emit entries at cursor */
            /* 首先用桶函数处理 */
            if (bucketfn)
                bucketfn(privdata, &t1->table[v & m1]);
            /* 获取游标在大表t1对应的槽 */
            de = t1->table[v & m1];
            /* 遍历槽上的链表, 使用函数处理获得的节点 */
            while (de)
            {
                next = de->next;
                fn(privdata, de);
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            /* 为什么这里能直接往上递增呢? */
            /* 假如是小表变大表, 上个游标xxabc的xx肯定是00, 所以在读大表时, 可以直接倒序往上加, 直到xx再次变00, 也就是穷举xx */
            /* 假如是大表变小表, 上个游标xxabc的xx很可能不为00, 假如为01, 那么就代表着00和10是被访问过的了(因为大表变小表, 当前游标之前的都被扫描过了), 最终才会返回01的, 所以遍历大表时高位序遍历不仅能把迁移的节点后槽遍历完, 还不重复 */
            /* 假如m1为...011111, ~m1就是...100000, v |= ~m1 就相当于 ...1xxabc */
            v |= ~m1;
            /* 反转, 结果是 abcxx11...111 */
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
            /* 如果m0是...000111, m1是...011111, 那么 m0^m1就是...011000, 也就是只保留m1的高位 */
            /* v & (m0 ^ m1) 就是, 当v相对于m0的高位都为0时, 退出循环 */
        } while (v & (m0 ^ m1));
    }

    /* 减少停顿rehash的状态 */
    dictResumeRehashing(d);

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* Because we may need to allocate huge memory chunk at once when dict
 * expands, we will check this allocation is allowed or not if the dict
 * type has expandAllowed member function. */
static int dictTypeExpandAllowed(dict *d)
{
    if (d->type->expandAllowed == NULL)
        return 1;
    return d->type->expandAllowed(
        _dictNextPower(d->ht[0].used + 1) * sizeof(dictEntry *),
        (double)d->ht[0].used / d->ht[0].size);
}

/* Expand the hash table if needed */
/* 根据需要，初始化字典（的哈希表），或者对字典（的现有哈希表）进行扩展 */
static int _dictExpandIfNeeded(dict *d)
{
    /* Incremental rehashing already in progress. Return. */
    /* 渐进式 rehash 已经在进行了，直接返回 */
    if (dictIsRehashing(d))
        return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    /* 如果字典（的 0 号哈希表）为空，那么创建并返回初始化大小的 0 号哈希表 */
    if (d->ht[0].size == 0)
        return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    /**两个条件之一为真时，对字典进行扩展 
     * 字典已使用节点数和字典大小之间的比率接近 1：1，并且 dict_can_resize 为真
     * 已使用节点数和字典大小之间的比率超过 dict_force_resize_ratio
     */
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize ||
         d->ht[0].used / d->ht[0].size > dict_force_resize_ratio) &&
        dictTypeExpandAllowed(d))
    {
        /* 新哈希表的大小至少是目前已使用节点数的两倍 T = O(N) */
        return dictExpand(d, d->ht[0].used + 1);
    }
    return DICT_OK;
}

/* Our hash table capability is a power of two */
/* 计算第一个大于等于 size 的 2 的 N 次方，用作哈希表的大小 */
static unsigned long _dictNextPower(unsigned long size)
{
    unsigned long i = DICT_HT_INITIAL_SIZE;

    if (size >= LONG_MAX)
        return LONG_MAX + 1LU;
    while (1)
    {
        if (i >= size)
            return i;
        i *= 2;
    }
}

/* Returns the index of a free slot that can be populated with
 * a hash entry for the given 'key'.
 * If the key already exists, -1 is returned
 * and the optional output parameter may be filled.
 *
 * Note that if we are in the process of rehashing the hash table, the
 * index is always returned in the context of the second (new) hash table. */

/**
 * @brief 返回可以将 key 插入到哈希表的索引位置
 * 如果 key 已经存在于哈希表，那么返回 -1
 * 注意，如果字典正在进行 rehash ，那么总是返回 1 号哈希表的索引。
 * 因为在字典进行 rehash 时，新节点总是插入到 1 号哈希表。
 */
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing)
{
    unsigned long idx, table;
    dictEntry *he;
    if (existing)
        *existing = NULL;

    /* Expand the hash table if needed */
    /* 根据需要，初始化字典（的哈希表），或者对字典（的现有哈希表）进行扩展 */
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;
    /**这里是否需要根据是否rehash，只选定哈希表0或者1查找，而不进行遍历呢？
     * 不可以。因为如果无rehash，只查找哈希表0；正在rehash，则需要查找哈希表0和1；用于冲突处理
     */
    for (table = 0; table <= 1; table++)
    {
        /* 计算索引值 */
        idx = hash & d->ht[table].sizemask;
        /* Search if this slot does not already contain the given key */
        /* 查找 key 是否存在 */
        he = d->ht[table].table[idx];
        while (he)
        {
            if (key == he->key || dictCompareKeys(d, key, he->key))
            {
                /* 如果key值存在，则直接返回 */
                if (existing)
                    *existing = he;
                return -1;
            }
            he = he->next;
        }
        /* 如果运行到这里时，说明 0 号哈希表中所有节点都不包含 key */
        /* 如果此时没有进行rehash，则直接退出；否则继续对1号哈希表进行索引值计算*/
        if (!dictIsRehashing(d))
            break;
    }
    return idx;
}

/* 清空字典上的所有哈希表节点，并重置字典属性 */
void dictEmpty(dict *d, void(callback)(void *))
{
    /* 删除两个哈希表上的所有节点 */
    _dictClear(d, &d->ht[0], callback);
    _dictClear(d, &d->ht[1], callback);
    /* 重置属性  */
    d->rehashidx = -1;
    d->pauserehash = 0;
}

/**通过 dictEnableResize() 和 dictDisableResize() 两个函数，程序可以手动地允许或阻止哈希表进行 rehash ，
 * 这在 Redis 使用子进程进行保存操作时，可以有效地利用 copy-on-write 机制。
 */
void dictEnableResize(void)
{
    dict_can_resize = 1;
}

/**
 * 需要注意的是，并非所有 rehash 都会被 dictDisableResize 阻止：
 * 如果已使用节点的数量和字典大小之间的比率，大于字典强制 rehash 比率 dict_force_resize_ratio ，
 * 那么 rehash 仍然会（强制）进行。
 */
void dictDisableResize(void)
{
    dict_can_resize = 0;
}

/* 获取哈希值 */
uint64_t dictGetHash(dict *d, const void *key)
{
    return dictHashKey(d, key);
}

/* Finds the dictEntry reference by using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is the reference to the dictEntry if found, or NULL if not found. */
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash)
{
    dictEntry *he, **heref;
    unsigned long idx, table;

    if (dictSize(d) == 0)
        return NULL; /* dict is empty */
    for (table = 0; table <= 1; table++)
    {
        idx = hash & d->ht[table].sizemask;
        heref = &d->ht[table].table[idx];
        he = *heref;
        while (he)
        {
            if (oldptr == he->key)
                return heref;
            heref = &he->next;
            he = *heref;
        }
        if (!dictIsRehashing(d))
            return NULL;
    }
    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

#define DICT_STATS_VECTLEN 50
size_t _dictGetStatsHt(char *buf, size_t bufsize, dictht *ht, int tableid)
{
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];
    size_t l = 0;

    if (ht->used == 0)
    {
        return snprintf(buf, bufsize,
                        "No stats available for empty dictionaries\n");
    }

    /* Compute stats. */
    for (i = 0; i < DICT_STATS_VECTLEN; i++)
        clvector[i] = 0;
    for (i = 0; i < ht->size; i++)
    {
        dictEntry *he;

        if (ht->table[i] == NULL)
        {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = ht->table[i];
        while (he)
        {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN - 1)]++;
        if (chainlen > maxchainlen)
            maxchainlen = chainlen;
        totchainlen += chainlen;
    }

    /* Generate human readable stats. */
    l += snprintf(buf + l, bufsize - l,
                  "Hash table %d stats (%s):\n"
                  " table size: %lu\n"
                  " number of elements: %lu\n"
                  " different slots: %lu\n"
                  " max chain length: %lu\n"
                  " avg chain length (counted): %.02f\n"
                  " avg chain length (computed): %.02f\n"
                  " Chain length distribution:\n",
                  tableid, (tableid == 0) ? "main hash table" : "rehashing target",
                  ht->size, ht->used, slots, maxchainlen,
                  (float)totchainlen / slots, (float)ht->used / slots);

    for (i = 0; i < DICT_STATS_VECTLEN - 1; i++)
    {
        if (clvector[i] == 0)
            continue;
        if (l >= bufsize)
            break;
        l += snprintf(buf + l, bufsize - l,
                      "   %s%ld: %ld (%.02f%%)\n",
                      (i == DICT_STATS_VECTLEN - 1) ? ">= " : "",
                      i, clvector[i], ((float)clvector[i] / ht->size) * 100);
    }

    /* Unlike snprintf(), return the number of characters actually written. */
    if (bufsize)
        buf[bufsize - 1] = '\0';
    return strlen(buf);
}

/* 获取hash表状态 */
void dictGetStats(char *buf, size_t bufsize, dict *d)
{
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf, bufsize, &d->ht[0], 0);
    buf += l;
    bufsize -= l;
    if (dictIsRehashing(d) && bufsize > 0)
    {
        _dictGetStatsHt(buf, bufsize, &d->ht[1], 1);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize)
        orig_buf[orig_bufsize - 1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef REDIS_TEST

uint64_t hashCallback(const void *key)
{
    return dictGenHashFunction((unsigned char *)key, strlen((char *)key));
}

int compareCallback(void *privdata, const void *key1, const void *key2)
{
    int l1, l2;
    DICT_NOTUSED(privdata);

    l1 = strlen((char *)key1);
    l2 = strlen((char *)key2);
    if (l1 != l2)
        return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    zfree(val);
}

char *stringFromLongLong(long long value)
{
    char buf[32];
    int len;
    char *s;

    len = sprintf(buf, "%lld", value);
    s = zmalloc(len + 1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

dictType BenchmarkDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL,
    NULL};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg)                                      \
    do                                                          \
    {                                                           \
        elapsed = timeInMilliseconds() - start;                 \
        printf(msg ": %ld items in %lld ms\n", count, elapsed); \
    } while (0)

/* ./redis-server test dict [<count> | --accurate] */
int dictTest(int argc, char **argv, int accurate)
{
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType, NULL);
    long count = 0;

    if (argc == 4)
    {
        if (accurate)
        {
            count = 5000000;
        }
        else
        {
            count = strtol(argv[3], NULL, 10);
        }
    }
    else
    {
        count = 5000;
    }

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        int retval = dictAdd(dict, stringFromLongLong(j), (void *)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict))
    {
        dictRehashMilliseconds(dict, 100);
    }

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict, key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict, key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        char *key = stringFromLongLong(rand() % count);
        dictEntry *de = dictFind(dict, key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        dictEntry *de = dictGetRandomKey(dict);
        assert(de != NULL);
    }
    end_benchmark("Accessing random keys");

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        char *key = stringFromLongLong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict, key);
        assert(de == NULL);
        zfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++)
    {
        char *key = stringFromLongLong(j);
        int retval = dictDelete(dict, key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict, key, (void *)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
    dictRelease(dict);
    return 0;
}
#endif
