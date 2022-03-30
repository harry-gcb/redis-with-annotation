/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef _ZIPLIST_H
#define _ZIPLIST_H

#define ZIPLIST_HEAD 0
#define ZIPLIST_TAIL 1

/* Each entry in the ziplist is either a string or an integer. */
typedef struct
{
    /* When string is used, it is provided with the length (slen). */
    unsigned char *sval;
    unsigned int slen;
    /* When integer is used, 'sval' is NULL, and lval holds the value. */
    long long lval;
} ziplistEntry;

unsigned char *ziplistNew(void); /* 创建并返回一个新的 ziplist  */
unsigned char *ziplistMerge(unsigned char **first, unsigned char **second);
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where);          /* 将长度为 slen 的字符串 s 推入到 zl 中 */
unsigned char *ziplistIndex(unsigned char *zl, int index);                                              /* 根据给定索引，遍历列表，并返回索引指定节点的指针。 */
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p);                                        /* 返回 p 所指向节点的后置节点。 */
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p);                                        /* 返回 p 所指向节点的前置节点 */
unsigned int ziplistGet(unsigned char *p, unsigned char **sval, unsigned int *slen, long long *lval);   /* 取出 p 所指向节点的值 */
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen); /* 将包含给定值 s 的新节点插入到给定的位置 p 中 */
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p);                                     /* 从 zl 中删除 *p 所指向的节点，并且原地更新 *p 所指向的位置，使得可以在迭代列表的过程中对节点进行删除 */
unsigned char *ziplistDeleteRange(unsigned char *zl, int index, unsigned int num);                      /* 从 index 索引指定的节点开始，连续地从 zl 中删除 num 个节点 */
unsigned char *ziplistReplace(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen);
unsigned int ziplistCompare(unsigned char *p, unsigned char *s, unsigned int slen);                                         /* 将 p 所指向的节点的值和 s 进行对比, 如果节点值和 s 的值相等，返回 1 ，不相等则返回 0*/
unsigned char *ziplistFind(unsigned char *zl, unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip); /* 寻找节点值和 s 相等的列表节点，并返回该节点的指针。 */
unsigned int ziplistLen(unsigned char *zl);                                                                                 /* 返回 ziplist 中的节点个数 */
size_t ziplistBlobLen(unsigned char *zl);                                                                                   /* 返回整个 ziplist 占用的内存字节数 */
void ziplistRepr(unsigned char *zl);
typedef int (*ziplistValidateEntryCB)(unsigned char *p, void *userdata);
int ziplistValidateIntegrity(unsigned char *zl, size_t size, int deep,
                             ziplistValidateEntryCB entry_cb, void *cb_userdata);
void ziplistRandomPair(unsigned char *zl, unsigned long total_count, ziplistEntry *key, ziplistEntry *val);
void ziplistRandomPairs(unsigned char *zl, unsigned int count, ziplistEntry *keys, ziplistEntry *vals);
unsigned int ziplistRandomPairsUnique(unsigned char *zl, unsigned int count, ziplistEntry *keys, ziplistEntry *vals);
int ziplistSafeToAdd(unsigned char *zl, size_t add);

#ifdef REDIS_TEST
int ziplistTest(int argc, char *argv[], int accurate);
#endif

#endif /* _ZIPLIST_H */
