/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
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

#ifndef __AE_H__
#define __AE_H__

#include "monotonic.h"

/* 事件执行状态 */
#define AE_OK 0   /* 成功 */
#define AE_ERR -1 /* 出错 */

/* 文件事件状态 */
#define AE_NONE 0     /* 未设置 */
#define AE_READABLE 1 /* 可读 */
#define AE_WRITABLE 2 /* 可写 */
#define AE_BARRIER                                                                                                                 \
    4 /* With WRITABLE, never fire the event if the                                                                                \
         READABLE event already fired in the same event                                                                            \
         loop iteration. Useful when you want to persist                                                                           \
         things to disk before sending replies, and want                                                                           \
         to do that in a group fashion.                                                                                            \
         使用 WRITABLE，如果 READABLE 事件已经在同一个事件循环迭代中触发，则永远不要触发该事件。 \
         当您想在发送回复之前将内容保存到磁盘并希望以组方式执行此操作时很有用                    \
        */

/* 时间处理器的执行 flags */
#define AE_FILE_EVENTS (1 << 0)                         /* 文件事件 */
#define AE_TIME_EVENTS (1 << 1)                         /* 时间事件 */
#define AE_ALL_EVENTS (AE_FILE_EVENTS | AE_TIME_EVENTS) /* 所有事件 */
#define AE_DONT_WAIT (1 << 2)                           /* 不阻塞，也不进行等待 */
#define AE_CALL_BEFORE_SLEEP (1 << 3)                   /* sleep之前调用 */
#define AE_CALL_AFTER_SLEEP (1 << 4)                    /* sleep之后调用 */

/* 决定时间事件是否要持续执行的 flag */
#define AE_NOMORE -1
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
/* 事件接口 */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure */
/* 文件事件结构 */
typedef struct aeFileEvent {
    int         mask;       /* 监听事件类型掩码，one of AE_(READABLE|WRITABLE|BARRIER) */
    aeFileProc *rfileProc;  /* 读事件处理器 */
    aeFileProc *wfileProc;  /* 写事件处理器 */
    void *      clientData; /* 多路复用库的私有数据 */
} aeFileEvent;

/* Time event structure */
/* 时间事件结构 */
typedef struct aeTimeEvent {
    long long             id;            /* 时间事件的唯一标识符 time event identifier. */
    monotime              when;          /* 事件的到达时间 */
    aeTimeProc *          timeProc;      /* 事件处理函数 */
    aeEventFinalizerProc *finalizerProc; /* 事件释放函数 */
    void *                clientData;    /* 多路复用库的私有数据 */
    struct aeTimeEvent *  prev;          /* 指向上一个时间事件结构，形成链表 */
    struct aeTimeEvent *  next;          /* 指向下一个时间事件结构，形成链表 */
    int                   refcount;      /* refcount to prevent timer events from being
                                          * freed in recursive time event calls.
                                          * refcount 以防止在递归时间事件调用中释放计时器事件
                                          */
} aeTimeEvent;

/* A fired event */
/* 已就绪事件 */
typedef struct aeFiredEvent {
    int fd;   /* 已就绪文件描述符 */
    int mask; /* 事件类型掩码 */
} aeFiredEvent;

/* State of an event based program */
/* 事件处理器的状态 */
typedef struct aeEventLoop {
    int                maxfd;           /* 目前已注册的最大描述符 */
    int                setsize;         /* 目前已追踪的最大描述符 */
    long long          timeEventNextId; /* 用于生成时间事件 id */
    aeFileEvent *      events;          /* 已注册的文件事件 */
    aeFiredEvent *     fired;           /* 已就绪的文件事件 */
    aeTimeEvent *      timeEventHead;   /* 时间事件 */
    int                stop;            /* 事件处理器的开关 */
    void *             apidata;         /* 多路复用库的私有数据 */
    aeBeforeSleepProc *beforesleep;     /* 在处理事件前要执行的函数 */
    aeBeforeSleepProc *aftersleep;      /* 在处理事件后要执行的函数 */
    int flags;
} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);            /* 创建EventLoop */
void         aeDeleteEventLoop(aeEventLoop *eventLoop); /* 删除事件处理器 */
void         aeStop(aeEventLoop *eventLoop);            /* 停止事件处理器 */
int          aeCreateFileEvent(
             aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc,
             void *clientData); /* 根据 mask 参数的值，监听 fd 文件的状态， 当 fd 可用时，执行 proc 函数 */
void         aeDeleteFileEvent(aeEventLoop *eventLoop, int fd,
                               int mask); /* 将 fd 从 mask 指定的监听队列中删除 */
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
long long    aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds, aeTimeProc *proc,
                               void *                clientData,
                               aeEventFinalizerProc *finalizerProc); /* 创建时间事件 */
int   aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id); /* 删除给定 id 的时间事件 */
int   aeProcessEvents(aeEventLoop *eventLoop, int flags);      /* 事件处理主函数 */
int aeWait(int fd, int mask, long long milliseconds);
void  aeMain(aeEventLoop *eventLoop); /* 事件处理器的主循环 */
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);
void aeSetDontWait(aeEventLoop *eventLoop, int noWait);

#endif
