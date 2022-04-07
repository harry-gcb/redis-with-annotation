/* Listpack -- A lists of strings serialization format
 *
 * This file implements the specification you can find at:
 *
 *  https://github.com/antirez/listpack
 *
 * Copyright (c) 2017, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __LISTPACK_H
#define __LISTPACK_H

#include <stdlib.h>
#include <stdint.h>

#define LP_INTBUF_SIZE 21 /* 20 digits of -2^63 + 1 null term = 21. */

/* lpInsert() where argument possible values: */
#define LP_BEFORE 0
#define LP_AFTER 1
#define LP_REPLACE 2

/* 创建一个listpack*/
unsigned char *lpNew(size_t capacity);
/* 释放listpack */
void lpFree(unsigned char *lp);
unsigned char* lpShrinkToFit(unsigned char *lp);
/* 插入、删除、更新数据到listpack */
unsigned char *lpInsert(unsigned char *lp, unsigned char *ele, uint32_t size, unsigned char *p, int where, unsigned char **newp);   /* 插入元素 */
/* 添加元素到listpack末尾，lpInsert实现 */
unsigned char *lpAppend(unsigned char *lp, unsigned char *ele, uint32_t size);
/* 删除listpack的某个元素，lpInsert实现*/
unsigned char *lpDelete(unsigned char *lp, unsigned char *p, unsigned char **newp);
/* 获取listpack的元素个数 */
uint32_t lpLength(unsigned char *lp);
/* 读取元素中的数据 */
unsigned char *lpGet(unsigned char *p, int64_t *count, unsigned char *intbuf);
/* 获取第一个位置的元素 */
unsigned char *lpFirst(unsigned char *lp);
/* 获取最后一个位置的元素 */
unsigned char *lpLast(unsigned char *lp);
/* 获取下一个位置的元素 */
unsigned char *lpNext(unsigned char *lp, unsigned char *p);
/* 获取上一个位置的元素 */
unsigned char *lpPrev(unsigned char *lp, unsigned char *p);
/* 获取listpack占用的字节数 */
uint32_t lpBytes(unsigned char *lp);
/* 查找第index个元素 */
unsigned char *lpSeek(unsigned char *lp, long index);
int lpValidateIntegrity(unsigned char *lp, size_t size, int deep);
unsigned char *lpValidateFirst(unsigned char *lp);
int lpValidateNext(unsigned char *lp, unsigned char **pp, size_t lpbytes);

#endif
