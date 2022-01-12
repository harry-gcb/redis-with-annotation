/* adlist.h - A generic doubly linked list implementation
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

#ifndef __ADLIST_H__
#define __ADLIST_H__

/* Node, List, and Iterator are the only data structures used currently. */

/* 双端链表节点 */
typedef struct listNode
{
    struct listNode *prev; /* 前置节点 */
    struct listNode *next; /* 后置节点 */
    void *value;           /* 节点的值 */
} listNode;

/* 双端链表迭代器 */
typedef struct listIter
{
    listNode *next; /* 当前迭代到的节点 */
    int direction;  /* 迭代的方向 */
} listIter;

/* 双端链表结构 */
typedef struct list
{
    listNode *head;                     /* 表头节点 */
    listNode *tail;                     /* 表尾节点 */
    void *(*dup)(void *ptr);            /* 节点值复制函数 */
    void (*free)(void *ptr);            /* 节点值释放函数 */
    int (*match)(void *ptr, void *key); /* 节点值对比函数 */
    unsigned long len;                  /* 链表所包含的节点数量 */
} list;

/* Functions implemented as macros */
#define listLength(l) ((l)->len)      /* 返回给定链表所包含的节点数量 */
#define listFirst(l) ((l)->head)      /* 返回给定链表的表头节点 */
#define listLast(l) ((l)->tail)       /* 返回给定链表的表尾节点 */
#define listPrevNode(n) ((n)->prev)   /* 返回给定节点的前置节点 */
#define listNextNode(n) ((n)->next)   /* 返回给定节点的后置节点 */
#define listNodeValue(n) ((n)->value) /* 返回给定节点的值 */

#define listSetDupMethod(l, m) ((l)->dup = (m))     /* 将链表 l 的值复制函数设置为 m */
#define listSetFreeMethod(l, m) ((l)->free = (m))   /* 将链表 l 的值释放函数设置为 m */
#define listSetMatchMethod(l, m) ((l)->match = (m)) /* 将链表的对比函数设置为 m */

#define listGetDupMethod(l) ((l)->dup)     /* 返回给定链表的值复制函数 */
#define listGetFreeMethod(l) ((l)->free)   /* 返回给定链表的值释放函数 */
#define listGetMatchMethod(l) ((l)->match) /* 返回给定链表的值对比函数 */

/* Prototypes */
list *listCreate(void);                                                       /* 创建一个新的链表 */
void listRelease(list *list);                                                 /* 释放整个链表，以及链表中所有节点 */
void listEmpty(list *list);                                                   /* 清空链表 */
list *listAddNodeHead(list *list, void *value);                               /* 将一个包含有给定值指针 value 的新节点添加到链表的表头 */
list *listAddNodeTail(list *list, void *value);                               /* 将一个包含有给定值指针 value 的新节点添加到链表的表尾 */
list *listInsertNode(list *list, listNode *old_node, void *value, int after); /* 创建一个包含值 value 的新节点，并将它插入到 old_node 的之前或之后 */
void listDelNode(list *list, listNode *node);                                 /* 从链表 list 中删除给定节点 node  */
listIter *listGetIterator(list *list, int direction);                         /* 为给定链表创建一个迭代器 */
listNode *listNext(listIter *iter);                                           /* 返回迭代器当前所指向的节点。 */
void listReleaseIterator(listIter *iter);                                     /* 释放迭代器 */
list *listDup(list *orig);                                                    /* 复制整个链表。 */
listNode *listSearchKey(list *list, void *key);                               /* 查找链表 list 中值和 key 匹配的节点。 */
listNode *listIndex(list *list, long index);                                  /* 返回链表在给定索引上的值。 */
void listRewind(list *list, listIter *li);                                    /* 将迭代器的方向设置为 AL_START_HEAD, 并将迭代指针重新指向表头节点。*/
void listRewindTail(list *list, listIter *li);                                /* 将迭代器的方向设置为 AL_START_TAIL, 并将迭代指针重新指向表尾节点。*/
void listRotateTailToHead(list *list);                                        /* 取出链表的表尾节点，并将它移动到表头，成为新的表头节点 */
void listRotateHeadToTail(list *list);                                        /* 取出链表的表头节点，并将它移动到表尾，成为新的表尾节点 */
void listJoin(list *l, list *o);                                              /* 将链表o插入到链表l的末尾 */

/* Directions for iterators */
/* 迭代器进行迭代的方向 */
/* 从头到尾 */
#define AL_START_HEAD 0
/* 从尾到头 */
#define AL_START_TAIL 1

#endif /* __ADLIST_H__ */
