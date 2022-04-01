/* SDSLib 2.0 -- A C dynamic strings library
 *
 * Copyright (c) 2006-2015, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2015, Oran Agra
 * Copyright (c) 2015, Redis Labs, Inc
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

#ifndef __SDS_H
#define __SDS_H

#define SDS_MAX_PREALLOC (1024 * 1024)
extern const char *SDS_NOINIT;

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>

/* 动态字符串结构体 */

/* SDS兼容C字符串 */
typedef char *sds;

/* 为了适应不同长度的字符串，配备了不同空间的结构体 */
/* Note: sdshdr5 is never used, we just access the flags byte directly.
 * However is here to document the layout of type 5 SDS strings. */
/* sdshdr5实际没被使用过，仅仅用来读取flag字段，作为初始flag. */
struct __attribute__((__packed__)) sdshdr5
{
    unsigned char flags; /* 3 lsb of type, and 5 msb of string length */
    char buf[];
};
/* __attribute__ ((__packed__)) 防止编译器优化，避免C语言自动内存对齐*/

/* uint8_t长度，最大255 */
struct __attribute__((__packed__)) sdshdr8
{
    uint8_t len;         /* 已使用空间 */
    uint8_t alloc;       /* 排除字符串头和终止符\0的剩余空间 */
    unsigned char flags; /* 字符串类型，低三位存储类型，高五位保留 */
    char buf[];
};

/* uint16_t长度，最大65535*/
struct __attribute__((__packed__)) sdshdr16
{
    uint16_t len;        /* used */
    uint16_t alloc;      /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

/* uint32_t长度，最大4294967295*/
struct __attribute__((__packed__)) sdshdr32
{
    uint32_t len;        /* used */
    uint32_t alloc;      /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

/* uint64_t长度，最大18446744073709551615 */
struct __attribute__((__packed__)) sdshdr64
{
    uint64_t len;        /* used */
    uint64_t alloc;      /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

/* flag类型，内容为0-4 */
#define SDS_TYPE_5 0
#define SDS_TYPE_8 1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4
#define SDS_TYPE_MASK 7
#define SDS_TYPE_BITS 3
#define SDS_HDR_VAR(T, s) struct sdshdr##T *sh = (void *)((s) - (sizeof(struct sdshdr##T))); /* 获取SDS头结构体指针 */
#define SDS_HDR(T, s) ((struct sdshdr##T *)((s) - (sizeof(struct sdshdr##T))))               /* 获取SDS头结构指针地址*/
#define SDS_TYPE_5_LEN(f) ((f) >> SDS_TYPE_BITS)

/* 返回 sds 实际保存的字符串的长度 */
static inline size_t sdslen(const sds s)
{
    unsigned char flags = s[-1];
    switch (flags & SDS_TYPE_MASK)
    {
    case SDS_TYPE_5:
        return SDS_TYPE_5_LEN(flags);
    case SDS_TYPE_8:
        return SDS_HDR(8, s)->len;
    case SDS_TYPE_16:
        return SDS_HDR(16, s)->len;
    case SDS_TYPE_32:
        return SDS_HDR(32, s)->len;
    case SDS_TYPE_64:
        return SDS_HDR(64, s)->len;
    }
    return 0;
}

/* 返回 sds 可用空间的长度 */
static inline size_t sdsavail(const sds s)
{
    unsigned char flags = s[-1];
    switch (flags & SDS_TYPE_MASK)
    {
    case SDS_TYPE_5:
    {
        return 0;
    }
    case SDS_TYPE_8:
    {
        SDS_HDR_VAR(8, s);
        return sh->alloc - sh->len;
    }
    case SDS_TYPE_16:
    {
        SDS_HDR_VAR(16, s);
        return sh->alloc - sh->len;
    }
    case SDS_TYPE_32:
    {
        SDS_HDR_VAR(32, s);
        return sh->alloc - sh->len;
    }
    case SDS_TYPE_64:
    {
        SDS_HDR_VAR(64, s);
        return sh->alloc - sh->len;
    }
    }
    return 0;
}

/* 设置已使用空间长度 */
static inline void sdssetlen(sds s, size_t newlen)
{
    unsigned char flags = s[-1];
    switch (flags & SDS_TYPE_MASK)
    {
    case SDS_TYPE_5:
    {
        unsigned char *fp = ((unsigned char *)s) - 1;
        *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
    }
    break;
    case SDS_TYPE_8:
        SDS_HDR(8, s)->len = newlen;
        break;
    case SDS_TYPE_16:
        SDS_HDR(16, s)->len = newlen;
        break;
    case SDS_TYPE_32:
        SDS_HDR(32, s)->len = newlen;
        break;
    case SDS_TYPE_64:
        SDS_HDR(64, s)->len = newlen;
        break;
    }
}

/* 增加已使用空间长度 */
static inline void sdsinclen(sds s, size_t inc)
{
    unsigned char flags = s[-1];
    switch (flags & SDS_TYPE_MASK)
    {
    case SDS_TYPE_5:
    {
        unsigned char *fp = ((unsigned char *)s) - 1;
        unsigned char newlen = SDS_TYPE_5_LEN(flags) + inc;
        *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
    }
    break;
    case SDS_TYPE_8:
        SDS_HDR(8, s)->len += inc;
        break;
    case SDS_TYPE_16:
        SDS_HDR(16, s)->len += inc;
        break;
    case SDS_TYPE_32:
        SDS_HDR(32, s)->len += inc;
        break;
    case SDS_TYPE_64:
        SDS_HDR(64, s)->len += inc;
        break;
    }
}

/* SDS数据部分内存空间长度，不包括终止符*/
/* sdsalloc() = sdsavail() + sdslen() */
static inline size_t sdsalloc(const sds s)
{
    unsigned char flags = s[-1];
    switch (flags & SDS_TYPE_MASK)
    {
    case SDS_TYPE_5:
        return SDS_TYPE_5_LEN(flags);
    case SDS_TYPE_8:
        return SDS_HDR(8, s)->alloc;
    case SDS_TYPE_16:
        return SDS_HDR(16, s)->alloc;
    case SDS_TYPE_32:
        return SDS_HDR(32, s)->alloc;
    case SDS_TYPE_64:
        return SDS_HDR(64, s)->alloc;
    }
    return 0;
}

/* 设置SDS数据部分空间长度 */
static inline void sdssetalloc(sds s, size_t newlen)
{
    unsigned char flags = s[-1];
    switch (flags & SDS_TYPE_MASK)
    {
    case SDS_TYPE_5:
        /* Nothing to do, this type has no total allocation info. */
        break;
    case SDS_TYPE_8:
        SDS_HDR(8, s)->alloc = newlen;
        break;
    case SDS_TYPE_16:
        SDS_HDR(16, s)->alloc = newlen;
        break;
    case SDS_TYPE_32:
        SDS_HDR(32, s)->alloc = newlen;
        break;
    case SDS_TYPE_64:
        SDS_HDR(64, s)->alloc = newlen;
        break;
    }
}

/* 根据给定的初始化字符串 init 和字符串长度 initlen，创建一个新的 sds */
sds sdsnewlen(const void *init, size_t initlen);
sds sdstrynewlen(const void *init, size_t initlen);
/* 根据给定字符串 init ，创建一个包含同样字符串的 sds */
sds sdsnew(const char *init);
/* 创建并返回一个只保存了空字符串 "" 的 sds */
sds sdsempty(void);
/* 复制给定 sds 的副本 */
sds sdsdup(const sds s);
/* 释放给定的 sds */
void sdsfree(sds s);
/* 将 sds 扩充至指定长度，未使用的空间以 0 字节填充。 */
sds sdsgrowzero(sds s, size_t len);
/* 将长度为 len 的字符串 t 追加到 sds 的字符串末尾 */
sds sdscatlen(sds s, const void *t, size_t len);
/* 将给定字符串 t 追加到 sds 的末尾 */
sds sdscat(sds s, const char *t);
/* 将另一个 sds 追加到一个 sds 的末尾 */
sds sdscatsds(sds s, const sds t);
/* 将字符串 t 的前 len 个字符复制到 sds s 当中，并在字符串的最后添加终结符。*/
sds sdscpylen(sds s, const char *t, size_t len);
/* 将字符串复制到 sds 当中，覆盖原有的字符。如果 sds 的长度少于字符串的长度，那么扩展 sds 。*/
sds sdscpy(sds s, const char *t);

/* 打印函数，被 sdscatprintf 所调用 */
sds sdscatvprintf(sds s, const char *fmt, va_list ap);
#ifdef __GNUC__
sds sdscatprintf(sds s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...);
#endif

/* 格式化字符串 */
sds sdscatfmt(sds s, char const *fmt, ...);
/* 对 sds 左右两端进行修剪，清除其中 cset 指定的所有字符, 比如 sdsstrim(xxyyabcyyxy, "xy") 将返回 "abc" */
sds sdstrim(sds s, const char *cset);
/* 将sds修改为 sds[start, start+len]的子串*/
void sdssubstr(sds s, size_t start, size_t len);
/*按索引对截取 sds 字符串的其中一段
  start 和 end 都是闭区间（包含在内）
  索引从 0 开始，最大为 sdslen(s) - 1
  索引可以是负数， sdslen(s) - 1 == -1*/
void sdsrange(sds s, ssize_t start, ssize_t end);
/* 更新字符串最新的长度 */
void sdsupdatelen(sds s);
/* 清空字符串 */
void sdsclear(sds s);
/* 对比两个 sds ， strcmp 的 sds 版本 */
int sdscmp(const sds s1, const sds s2);
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count);
void sdsfreesplitres(sds *tokens, int count);
void sdstolower(sds s);
void sdstoupper(sds s);
sds sdsfromlonglong(long long value);
sds sdscatrepr(sds s, const char *p, size_t len);
sds *sdssplitargs(const char *line, int *argc);
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen);
sds sdsjoin(char **argv, int argc, char *sep);
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen);

/* Callback for sdstemplate. The function gets called by sdstemplate
 * every time a variable needs to be expanded. The variable name is
 * provided as variable, and the callback is expected to return a
 * substitution value. Returning a NULL indicates an error.
 */
typedef sds (*sdstemplate_callback_t)(const sds variable, void *arg);
sds sdstemplate(const char *template, sdstemplate_callback_t cb_func, void *cb_arg);

/* Low level functions exposed to the user API */
sds sdsMakeRoomFor(sds s, size_t addlen);
void sdsIncrLen(sds s, ssize_t incr);
sds sdsRemoveFreeSpace(sds s);
size_t sdsAllocSize(sds s);
void *sdsAllocPtr(sds s);

/* Export the allocator used by SDS to the program using SDS.
 * Sometimes the program SDS is linked to, may use a different set of
 * allocators, but may want to allocate or free things that SDS will
 * respectively free or allocate. */
void *sds_malloc(size_t size);
void *sds_realloc(void *ptr, size_t size);
void sds_free(void *ptr);

#ifdef REDIS_TEST
int sdsTest(int argc, char *argv[], int accurate);
#endif

#endif
