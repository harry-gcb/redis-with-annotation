/*
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

#include "server.h"
#include <math.h> /* isnan(), isinf() */

/* Forward declarations */
/* 前向声明 */
int getGenericCommand(client *c);

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/
/*
 * 检查给定字符串长度 len 是否超过限制值 512 MB
 * 超过返回 REDIS_ERR ，未超过返回 REDIS_OK
 */
static int checkStringLength(client *c, long long size) {
    if (!(c->flags & CLIENT_MASTER) && size > server.proto_max_bulk_len) {
        addReplyError(c,"string exceeds maximum allowed size (proto-max-bulk-len)");
        return C_ERR;
    }
    return C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX, GETSET.
 *
 * 'flags' changes the behavior of the command (NX, XX or GET, see below).
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. */

#define OBJ_NO_FLAGS 0
#define OBJ_SET_NX (1<<0)          /* Set if key not exists. */
#define OBJ_SET_XX (1<<1)          /* Set if key exists. */
#define OBJ_EX (1<<2)              /* Set if time in seconds is given */
#define OBJ_PX (1<<3)              /* Set if time in ms in given */
#define OBJ_KEEPTTL (1<<4)         /* Set and keep the ttl */
#define OBJ_SET_GET (1<<5)         /* Set if want to get key before set */
#define OBJ_EXAT (1<<6)            /* Set if timestamp in second is given */
#define OBJ_PXAT (1<<7)            /* Set if timestamp in ms is given */
#define OBJ_PERSIST (1<<8)         /* Set if we need to remove the ttl */
/* set/get命令接口 */
void setGenericCommand(client *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    long long milliseconds = 0, when = 0; /* initialized to avoid any harmness warning */
    /* 取出过期时间 */
    if (expire) {
        /* 取出 expire 参数的值 */
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != C_OK)
            return;
        /* expire 参数的值不正确时报错 */
        if (milliseconds <= 0 || (unit == UNIT_SECONDS && milliseconds > LLONG_MAX / 1000)) {
            /* Negative value provided or multiplication is gonna overflow. */
            addReplyErrorFormat(c, "invalid expire time in %s", c->cmd->name);
            return;
        }
        /* 不论输入的过期时间是秒还是毫秒
         * Redis 实际都以毫秒的形式保存过期时间
         * 如果输入的过期时间为秒，那么将它转换为毫秒 */
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
        when = milliseconds;
        /* 检查过期时间是否可用 */
        if ((flags & OBJ_PX) || (flags & OBJ_EX))
            when += mstime();
        if (when <= 0) {
            /* Overflow detected. */
            addReplyErrorFormat(c, "invalid expire time in %s", c->cmd->name);
            return;
        }
    }
    /* 如果设置了 NX 或者 XX 参数，那么检查条件是否符合这两个设置 */
    if ((flags & OBJ_SET_NX && lookupKeyWrite(c->db,key) != NULL) ||
        (flags & OBJ_SET_XX && lookupKeyWrite(c->db,key) == NULL))
    {
        addReply(c, abort_reply ? abort_reply : shared.null[c->resp]);
        return;
    }
    /* 如果有get参数，获取key对应的value字符串 */
    if (flags & OBJ_SET_GET) {
        if (getGenericCommand(c) == C_ERR) return;
    }
    /* 将键值关联到数据库 */
    genericSetKey(c,c->db,key, val,flags & OBJ_KEEPTTL,1);
    /* 将数据库设为脏 */
    server.dirty++;
    /* 发送事件通知 */
    notifyKeyspaceEvent(NOTIFY_STRING,"set",key,c->db->id);
    /* 为键设置过期时间 */
    if (expire) {
        setExpire(c,c->db,key,when);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"expire",key,c->db->id);

        /* Propagate as SET Key Value PXAT millisecond-timestamp if there is EXAT/PXAT or
         * propagate as SET Key Value PX millisecond if there is EX/PX flag.
         *
         * Additionally when we propagate the SET with PX (relative millisecond) we translate
         * it again to SET with PXAT for the AOF.
         *
         * Additional care is required while modifying the argument order. AOF relies on the
         * exp argument being at index 3. (see feedAppendOnlyFile)
         * */
        robj *exp = (flags & OBJ_PXAT) || (flags & OBJ_EXAT) ? shared.pxat : shared.px;
        robj *millisecondObj = createStringObjectFromLongLong(milliseconds);
        rewriteClientCommandVector(c,5,shared.set,key,val,exp,millisecondObj);
        decrRefCount(millisecondObj);
    }
    /* 如果没有get参数，直接发送ok结果给client */
    if (!(flags & OBJ_SET_GET)) {
        addReply(c, ok_reply ? ok_reply : shared.ok);
    }

    /* 不带 GET 参数的响应
     * 如果我们已经过期则不需要，因为在这种情况下我们完全重写了命令argv */
    if ((flags & OBJ_SET_GET) && !expire) {
        int argc = 0;
        int j;
        robj **argv = zmalloc((c->argc-1)*sizeof(robj*));
        for (j=0; j < c->argc; j++) {
            char *a = c->argv[j]->ptr;
            /* Skip GET which may be repeated multiple times. */
            if (j >= 3 &&
                (a[0] == 'g' || a[0] == 'G') &&
                (a[1] == 'e' || a[1] == 'E') &&
                (a[2] == 't' || a[2] == 'T') && a[3] == '\0')
                continue;
            argv[argc++] = c->argv[j];
            incrRefCount(c->argv[j]);
        }
        replaceClientCommandVector(c, argc, argv);
    }
}

#define COMMAND_GET 0
#define COMMAND_SET 1
/*
 * The parseExtendedStringArgumentsOrReply() function performs the common validation for extended
 * string arguments used in SET and GET command.
 *
 * Get specific commands - PERSIST/DEL
 * Set specific commands - XX/NX/GET
 * Common commands - EX/EXAT/PX/PXAT/KEEPTTL
 *
 * Function takes pointers to client, flags, unit, pointer to pointer of expire obj if needed
 * to be determined and command_type which can be COMMAND_GET or COMMAND_SET.
 *
 * If there are any syntax violations C_ERR is returned else C_OK is returned.
 *
 * Input flags are updated upon parsing the arguments. Unit and expire are updated if there are any
 * EX/EXAT/PX/PXAT arguments. Unit is updated to millisecond if PX/PXAT is set.
 */
int parseExtendedStringArgumentsOrReply(client *c, int *flags, int *unit, robj **expire, int command_type) {
    int j = command_type == COMMAND_GET ? 2 : 3; /* set命令3个必要参数 */
    for (; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1];

        if ((opt[0] == 'n' || opt[0] == 'N') &&
            (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' &&
            !(*flags & OBJ_SET_XX) && !(*flags & OBJ_SET_GET) && (command_type == COMMAND_SET))
        {
            *flags |= OBJ_SET_NX; /* nx/NX选项 */
        } else if ((opt[0] == 'x' || opt[0] == 'X') &&
                   (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' &&
                   !(*flags & OBJ_SET_NX) && (command_type == COMMAND_SET))
        {
            *flags |= OBJ_SET_XX;
        } else if ((opt[0] == 'g' || opt[0] == 'G') &&
                   (opt[1] == 'e' || opt[1] == 'E') &&
                   (opt[2] == 't' || opt[2] == 'T') && opt[3] == '\0' &&
                   !(*flags & OBJ_SET_NX) && (command_type == COMMAND_SET))
        {
            *flags |= OBJ_SET_GET; /* get/GET选项 */
        } else if (!strcasecmp(opt, "KEEPTTL") && !(*flags & OBJ_PERSIST) &&
            !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) &&
            !(*flags & OBJ_PX) && !(*flags & OBJ_PXAT) && (command_type == COMMAND_SET))
        {
            *flags |= OBJ_KEEPTTL; /* KEEPTTL 选项 */
        } else if (!strcasecmp(opt,"PERSIST") && (command_type == COMMAND_GET) &&
               !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) &&
               !(*flags & OBJ_PX) && !(*flags & OBJ_PXAT) &&
               !(*flags & OBJ_KEEPTTL))
        {
            *flags |= OBJ_PERSIST;
        } else if ((opt[0] == 'e' || opt[0] == 'E') &&
                   (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' &&
                   !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) &&
                   !(*flags & OBJ_EXAT) && !(*flags & OBJ_PX) &&
                   !(*flags & OBJ_PXAT) && next)
        {
            *flags |= OBJ_EX; /* ex/EX选项 */
            *expire = next;
            j++;
        } else if ((opt[0] == 'p' || opt[0] == 'P') &&
                   (opt[1] == 'x' || opt[1] == 'X') && opt[2] == '\0' &&
                   !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) &&
                   !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) &&
                   !(*flags & OBJ_PXAT) && next)
        {
            *flags |= OBJ_PX; /* px/PX选项 */
            *unit = UNIT_MILLISECONDS;
            *expire = next;
            j++;
        } else if ((opt[0] == 'e' || opt[0] == 'E') &&
                   (opt[1] == 'x' || opt[1] == 'X') &&
                   (opt[2] == 'a' || opt[2] == 'A') &&
                   (opt[3] == 't' || opt[3] == 'T') && opt[4] == '\0' &&
                   !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) &&
                   !(*flags & OBJ_EX) && !(*flags & OBJ_PX) &&
                   !(*flags & OBJ_PXAT) && next)
        {
            *flags |= OBJ_EXAT; /* exat/EXAT选项 */
            *expire = next;
            j++;
        } else if ((opt[0] == 'p' || opt[0] == 'P') &&
                   (opt[1] == 'x' || opt[1] == 'X') &&
                   (opt[2] == 'a' || opt[2] == 'A') &&
                   (opt[3] == 't' || opt[3] == 'T') && opt[4] == '\0' &&
                   !(*flags & OBJ_KEEPTTL) && !(*flags & OBJ_PERSIST) &&
                   !(*flags & OBJ_EX) && !(*flags & OBJ_EXAT) &&
                   !(*flags & OBJ_PX) && next)
        {
            *flags |= OBJ_PXAT; /* pxat/PXAT选项 */
            *unit = UNIT_MILLISECONDS;
            *expire = next;
            j++;
        } else {
            /* 参数不可用 */
            addReplyErrorObject(c,shared.syntaxerr);
            return C_ERR;
        }
    }
    return C_OK;
}

/* SET key value [NX] [XX] [KEEPTTL] [GET] [EX <seconds>] [PX <milliseconds>]
 *     [EXAT <seconds-timestamp>][PXAT <milliseconds-timestamp>] */
/* set命令
 * NX：当数据库中key不存在时，可以将key-value添加到数据库
 * XX：当数据库中key存在时，可以将key-value设置到数据库，与NX参数互斥
 * EX: key的超时秒数
 * PX: key的超时毫秒数，与EX参数互斥
 * EXAT: key的超时时间，单位秒
 * PXAT：key的超时时间，单位毫秒
 */
void setCommand(client *c) {
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = OBJ_NO_FLAGS;
    /* 设置选项参数 */
    if (parseExtendedStringArgumentsOrReply(c,&flags,&unit,&expire,COMMAND_SET) != C_OK) {
        return;
    }
    /* 尝试对值对象进行编码 */
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    /* 执行set命令 */
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL);
}
/* 在调用setGenericCommand时，
 * 将flags赋值为OBJ_SET_NX，表示只有key不存在时才可以执行函数 */
void setnxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,OBJ_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}
/* 将key-value设置到数据库，并且指定key的超时秒数 */
void setexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_EX,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

/* 将key-value设置到数据库，并且指定key的超时毫秒数 */
void psetexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_PX,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}

int getGenericCommand(client *c) {
    robj *o;

    /* 尝试从数据库中取出键 c->argv[1] 对应的值对象 
     * 如果键不存在时，向客户端发送回复信息，并返回 NULL
     */
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL)
        return C_OK;

    /* 值对象存在，检查它的类型 */
    if (checkType(c,o,OBJ_STRING)) {
        /* 类型错误 */
        return C_ERR;
    }
    /* 类型正确，向客户端返回对象的值 */
    addReplyBulk(c,o);
    return C_OK;
}

/* get命令 */
void getCommand(client *c) {
    getGenericCommand(c);
}

/*
 * GETEX <key> [PERSIST][EX seconds][PX milliseconds][EXAT seconds-timestamp][PXAT milliseconds-timestamp]
 *
 * The getexCommand() function implements extended options and variants of the GET command. Unlike GET
 * command this command is not read-only.
 *
 * The default behavior when no options are specified is same as GET and does not alter any TTL.
 *
 * Only one of the below options can be used at a given time.
 *
 * 1. PERSIST removes any TTL associated with the key.
 * 2. EX Set expiry TTL in seconds.
 * 3. PX Set expiry TTL in milliseconds.
 * 4. EXAT Same like EX instead of specifying the number of seconds representing the TTL
 *      (time to live), it takes an absolute Unix timestamp
 * 5. PXAT Same like PX instead of specifying the number of milliseconds representing the TTL
 *      (time to live), it takes an absolute Unix timestamp
 *
 * Command would either return the bulk string, error or nil.
 */
void getexCommand(client *c) {
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = OBJ_NO_FLAGS;

    if (parseExtendedStringArgumentsOrReply(c,&flags,&unit,&expire,COMMAND_GET) != C_OK) {
        return;
    }

    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL)
        return;

    if (checkType(c,o,OBJ_STRING)) {
        return;
    }

    long long milliseconds = 0, when = 0;

    /* Validate the expiration time value first */
    if (expire) {
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != C_OK)
            return;
        if (milliseconds <= 0 || (unit == UNIT_SECONDS && milliseconds > LLONG_MAX / 1000)) {
            /* Negative value provided or multiplication is gonna overflow. */
            addReplyErrorFormat(c, "invalid expire time in %s", c->cmd->name);
            return;
        }
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
        when = milliseconds;
        if ((flags & OBJ_PX) || (flags & OBJ_EX))
            when += mstime();
        if (when <= 0) {
            /* Overflow detected. */
            addReplyErrorFormat(c, "invalid expire time in %s", c->cmd->name);
            return;
        }
    }

    /* We need to do this before we expire the key or delete it */
    addReplyBulk(c,o);

    /* This command is never propagated as is. It is either propagated as PEXPIRE[AT],DEL,UNLINK or PERSIST.
     * This why it doesn't need special handling in feedAppendOnlyFile to convert relative expire time to absolute one. */
    if (((flags & OBJ_PXAT) || (flags & OBJ_EXAT)) && checkAlreadyExpired(milliseconds)) {
        /* When PXAT/EXAT absolute timestamp is specified, there can be a chance that timestamp
         * has already elapsed so delete the key in that case. */
        int deleted = server.lazyfree_lazy_expire ? dbAsyncDelete(c->db, c->argv[1]) :
                      dbSyncDelete(c->db, c->argv[1]);
        serverAssert(deleted);
        robj *aux = server.lazyfree_lazy_expire ? shared.unlink : shared.del;
        rewriteClientCommandVector(c,2,aux,c->argv[1]);
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty++;
    } else if (expire) {
        setExpire(c,c->db,c->argv[1],when);
        /* Propagate */
        robj *exp = (flags & OBJ_PXAT) || (flags & OBJ_EXAT) ? shared.pexpireat : shared.pexpire;
        robj* millisecondObj = createStringObjectFromLongLong(milliseconds);
        rewriteClientCommandVector(c,3,exp,c->argv[1],millisecondObj);
        decrRefCount(millisecondObj);
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"expire",c->argv[1],c->db->id);
        server.dirty++;
    } else if (flags & OBJ_PERSIST) {
        if (removeExpire(c->db, c->argv[1])) {
            signalModifiedKey(c, c->db, c->argv[1]);
            rewriteClientCommandVector(c, 2, shared.persist, c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"persist",c->argv[1],c->db->id);
            server.dirty++;
        }
    }
}

void getdelCommand(client *c) {
    if (getGenericCommand(c) == C_ERR) return;
    int deleted = server.lazyfree_lazy_user_del ? dbAsyncDelete(c->db, c->argv[1]) :
                  dbSyncDelete(c->db, c->argv[1]);
    if (deleted) {
        /* Propagate as DEL/UNLINK command */
        robj *aux = server.lazyfree_lazy_user_del ? shared.unlink : shared.del;
        rewriteClientCommandVector(c,2,aux,c->argv[1]);
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty++;
    }
}

void getsetCommand(client *c) {
    /* 取出并返回键的值对象 */
    if (getGenericCommand(c) == C_ERR) return;
    /* 编码键的新值 c->argv[2] */
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    /* 将数据库中关联键 c->argv[1] 和新值对象 c->argv[2] */
    setKey(c,c->db,c->argv[1],c->argv[2]);
    /* 发送事件通知 */
    notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[1],c->db->id);
    server.dirty++;

    /* 转发set命令 */
    rewriteClientCommandArgument(c,0,shared.set);
}

/* setrange命令主要用于设置value的部分子串，设置时将值从偏移量offset开始覆盖成value值。
 * 如果偏移值大于原值的长度，则偏移量之前的字符串由 \x00 填充 */
void setrangeCommand(client *c) {
    robj *o;
    long offset;
    sds value = c->argv[3]->ptr;
    /* 取出 offset 参数 */
    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != C_OK)
        return;
    /* 检查 offset 参数 */
    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }
    /* 取出键现在的值对象 */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* 数据库中不存在该键 */
        /* value 为空，没有什么可设置的，向客户端返回 0 */
        if (sdslen(value) == 0) {
            addReply(c,shared.czero);
            return;
        }

        /* 如果设置后的长度会超过 Redis 的限制的话
         * 那么放弃设置，向客户端发送一个出错回复
         */
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;
        /* 如果 value 没有问题，可以设置，那么创建一个空字符串值对象
         * 并在数据库中关联键 c->argv[1] 和这个空字符串对象 */
        o = createObject(OBJ_STRING,sdsnewlen(NULL, offset+sdslen(value)));
        dbAdd(c->db,c->argv[1],o);
    } else {
        size_t olen;

        /* 值对象存在，检查值对象的类型*/
        if (checkType(c,o,OBJ_STRING))
            return;

        /* 取出原有字符串的长度，
         * value 为空，没有什么可设置的，向客户端返回 原字符串长度 */
        olen = stringObjectLen(o);
        if (sdslen(value) == 0) {
            addReplyLongLong(c,olen);
            return;
        }

        /* 如果设置后的长度会超过 Redis 的限制的话
         * 那么放弃设置，向客户端发送一个出错回复
         */
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;

        /* Create a copy when the object is shared or encoded. */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }
    /* 这里的 sdslen(value) > 0 其实可以去掉，前面已经做了检测了*/
    if (sdslen(value) > 0) {
        /* 扩展字符串值对象 */
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));
        /* 将 value 复制到字符串中的指定的位置 */
        memcpy((char*)o->ptr+offset,value,sdslen(value));
        /* 向数据库发送键被修改的信号 */
        signalModifiedKey(c,c->db,c->argv[1]);
        /* 发送事件通知 */
        notifyKeyspaceEvent(NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id);
        /* 将服务器设为脏 */
        server.dirty++;
    }
    /* 设置成功，返回新的字符串值给客户端 */
    addReplyLongLong(c,sdslen(o->ptr));
}

/* 获取字符串的部分子串 */
void getrangeCommand(client *c) {
    robj *o;
    long long start, end;
    char *str, llbuf[32];
    size_t strlen;
    /* 取出 start 参数 */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK)
        return;
    /* 取出 end 参数 */
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK)
        return;
    /* 从数据库中查找键 c->argv[1] ，检查值对象类型是否是字符串 */
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;

    /* 根据编码，对对象的值进行处理 */
    if (o->encoding == OBJ_ENCODING_INT) {
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr);
    } else {
        str = o->ptr;
        strlen = sdslen(str);
    }
    /* 当start和end都小于0，并且start大于end时，无法截取子串，此时start和end不合法
     */
    if (start < 0 && end < 0 && start > end) {
        addReply(c,shared.emptybulk);
        return;
    }
    /* 将负数索引转换为正数索引 */
    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((unsigned long long)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {
        /* 处理索引范围为空的情况 */
        addReply(c,shared.emptybulk);
    } else {
        /* 向客户端返回给定范围内的字符串内容 */
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}

/* 返回所有指定key的值 */
void mgetCommand(client *c) {
    int j;
    /* 查找并返回所有输入键的值 */
    addReplyArrayLen(c,c->argc-1);
    for (j = 1; j < c->argc; j++) {
        /* 查找键 c->argc[j] 的值 */
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
            /* 值不存在，向客户端发送空回复 */
            addReplyNull(c);
        } else {
            if (o->type != OBJ_STRING) {
                /* 值存在，但不是字符串类型 */
                addReplyNull(c);
            } else {
                /* 值存在，并且是字符串 */
                addReplyBulk(c,o);
            }
        }
    }
}

void msetGenericCommand(client *c, int nx) {
    int j;
    /* key/value是成对存在的，加上命令mset/msetnx，参数个数必须位奇数 */
    if ((c->argc % 2) == 0) {
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }

    /* 当nx参数为1时，遍历每个key在数据库中是否存在，
     * 当有任意一个key存在时，表示参数不合法 */
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                addReply(c, shared.czero);
                return;
            }
        }
    }
    /* 遍历参数表，将key/value写入数据库 */
    for (j = 1; j < c->argc; j += 2) {
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
        setKey(c,c->db,c->argv[j],c->argv[j+1]);
        notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[j],c->db->id);
    }
    server.dirty += (c->argc-1)/2;
    addReply(c, nx ? shared.cone : shared.ok);
}

/* 一次性设置多个字符串，某个给定key已经存在，则mset会将原key的value值覆盖 */
void msetCommand(client *c) {
    msetGenericCommand(c,0);
}

/* 一次性设置多个字符串，msetnx是当所有的key都不存在时才可以写入数据库 */
void msetnxCommand(client *c) {
    msetGenericCommand(c,1);
}
/* key对应的value加/减指定值 */
void incrDecrCommand(client *c, long long incr) {
    long long value, oldvalue;
    robj *o, *new;
    /* 取出值对象 */
    o = lookupKeyWrite(c->db,c->argv[1]);
    /* 检查对象是否存在，以及类型是否正确 */
    if (checkType(c,o,OBJ_STRING)) return;
    /* 取出对象的整数值，并保存到 value 参数中 */
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != C_OK) return;

    /* 检查加法操作执行之后值是否会溢出
     * 如果溢出的话，就向客户端发送一个出错回复，并放弃设置操作 */
    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }

    /* 进行加法计算 */
    value += incr;
    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX)
    {
        /* robj的ptr是可以直接存储一个long值的，
         * 这里只要robj没有被引用且不是共享对象，ptr直接赋值为value值 */
        new = o;
        o->ptr = (void*)((long)value);
    } else {
        /* 将值保存到新的值对象中，然后用新的值对象替换原来的值对象 */
        new = createStringObjectFromLongLongForValue(value);
        if (o) {
            dbOverwrite(c->db,c->argv[1],new);
        } else {
            dbAdd(c->db,c->argv[1],new);
        }
    }
    /* 向数据库发送键被修改的信号 */
    signalModifiedKey(c,c->db,c->argv[1]);
    /* 发送事件通知 */
    notifyKeyspaceEvent(NOTIFY_STRING,"incrby",c->argv[1],c->db->id);
    /* 将服务器设为脏 */
    server.dirty++;
    /* 返回回复 */
    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);
}
/* 将key存储的值加1 */
void incrCommand(client *c) {
    incrDecrCommand(c,1);
}
/* 将key存储的值减1 */
void decrCommand(client *c) {
    incrDecrCommand(c,-1);
}
/* 将key存储的值加指定值 */
void incrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,incr);
}
/* 将key存储的值减指定值 */
void decrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,-incr);
}
/* 将key存储的值加指定flaot值 */
void incrbyfloatCommand(client *c) {
    long double incr, value;
    robj *o, *new;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (checkType(c,o,OBJ_STRING)) return;
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != C_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != C_OK)
        return;

    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }
    new = createStringObjectFromLongDouble(value,1);
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id);
    server.dirty++;
    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    rewriteClientCommandArgument(c,0,shared.set);
    rewriteClientCommandArgument(c,2,new);
    rewriteClientCommandArgument(c,3,shared.keepttl);
}

/* 将value追加到原值的末尾，如果key不存在，
 * 此命令等同于set key value命令 */
void appendCommand(client *c) {
    size_t totlen;
    robj *o, *append;
    /* 取出键相应的值对象 */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* 键值对不存在，创建一个新的key/value到数据库 */
        c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(c->db,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
        /* 键值对存在，检查类型，只有string类型能append */
        if (checkType(c,o,OBJ_STRING))
            return;

        /* 检查追加操作之后，字符串的长度是否符合 Redis 的限制 */
        append = c->argv[2];
        totlen = stringObjectLen(o)+sdslen(append->ptr);
        /* 追加后的字符串长度必须小于512MB */
        if (checkStringLength(c,totlen) != C_OK)
            return;
        /* 在服务端接收到命令的时候，就已经判断了命令的最大长度不能大于512 MB，
         * 所以set命令不需要再次判断 */

        /* 字符串追加会修改原字符串的值，所以必须保证字符串是非共享的。
         * 如果字符串是共享的，则需要解除共享，新创建一个值对象 */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
        /* 值对象创建好之后，将新字符串追加到原字符串末尾 */
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"append",c->argv[1],c->db->id);
    server.dirty++;
    addReplyLongLong(c,totlen);
}

/* 数据库中获取到value，返回value字符串的长度 */
void strlenCommand(client *c) {
    robj *o;
    /* 取出值对象，并进行类型检查 */
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;
    /* 返回字符串值的长度 */
    addReplyLongLong(c,stringObjectLen(o));
}


/* STRALGO -- Implement complex algorithms on strings.
 *
 * STRALGO <algorithm> ... arguments ... */
void stralgoLCS(client *c);     /* This implements the LCS algorithm. */
void stralgoCommand(client *c) {
    /* Select the algorithm. */
    if (!strcasecmp(c->argv[1]->ptr,"lcs")) {
        stralgoLCS(c);
    } else {
        addReplyErrorObject(c,shared.syntaxerr);
    }
}

/* STRALGO <algo> [IDX] [LEN] [MINMATCHLEN <len>] [WITHMATCHLEN]
 *     STRINGS <string> <string> | KEYS <keya> <keyb>
 */
void stralgoLCS(client *c) {
    uint32_t i, j;
    long long minmatchlen = 0;
    sds a = NULL, b = NULL;
    int getlen = 0, getidx = 0, withmatchlen = 0;
    robj *obja = NULL, *objb = NULL;

    for (j = 2; j < (uint32_t)c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc-1) - j;

        if (!strcasecmp(opt,"IDX")) {
            getidx = 1;
        } else if (!strcasecmp(opt,"LEN")) {
            getlen = 1;
        } else if (!strcasecmp(opt,"WITHMATCHLEN")) {
            withmatchlen = 1;
        } else if (!strcasecmp(opt,"MINMATCHLEN") && moreargs) {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&minmatchlen,NULL)
                != C_OK) goto cleanup;
            if (minmatchlen < 0) minmatchlen = 0;
            j++;
        } else if (!strcasecmp(opt,"STRINGS") && moreargs > 1) {
            if (a != NULL) {
                addReplyError(c,"Either use STRINGS or KEYS");
                goto cleanup;
            }
            a = c->argv[j+1]->ptr;
            b = c->argv[j+2]->ptr;
            j += 2;
        } else if (!strcasecmp(opt,"KEYS") && moreargs > 1) {
            if (a != NULL) {
                addReplyError(c,"Either use STRINGS or KEYS");
                goto cleanup;
            }
            obja = lookupKeyRead(c->db,c->argv[j+1]);
            objb = lookupKeyRead(c->db,c->argv[j+2]);
            if ((obja && obja->type != OBJ_STRING) ||
                (objb && objb->type != OBJ_STRING))
            {
                addReplyError(c,
                    "The specified keys must contain string values");
                /* Don't cleanup the objects, we need to do that
                 * only after calling getDecodedObject(). */
                obja = NULL;
                objb = NULL;
                goto cleanup;
            }
            obja = obja ? getDecodedObject(obja) : createStringObject("",0);
            objb = objb ? getDecodedObject(objb) : createStringObject("",0);
            a = obja->ptr;
            b = objb->ptr;
            j += 2;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Complain if the user passed ambiguous parameters. */
    if (a == NULL) {
        addReplyError(c,"Please specify two strings: "
                        "STRINGS or KEYS options are mandatory");
        goto cleanup;
    } else if (getlen && getidx) {
        addReplyError(c,
            "If you want both the length and indexes, please "
            "just use IDX.");
        goto cleanup;
    }

    /* Detect string truncation or later overflows. */
    if (sdslen(a) >= UINT32_MAX-1 || sdslen(b) >= UINT32_MAX-1) {
        addReplyError(c, "String too long for LCS");
        goto cleanup;
    }

    /* Compute the LCS using the vanilla dynamic programming technique of
     * building a table of LCS(x,y) substrings. */
    uint32_t alen = sdslen(a);
    uint32_t blen = sdslen(b);

    /* Setup an uint32_t array to store at LCS[i,j] the length of the
     * LCS A0..i-1, B0..j-1. Note that we have a linear array here, so
     * we index it as LCS[j+(blen+1)*j] */
    #define LCS(A,B) lcs[(B)+((A)*(blen+1))]

    /* Try to allocate the LCS table, and abort on overflow or insufficient memory. */
    unsigned long long lcssize = (unsigned long long)(alen+1)*(blen+1); /* Can't overflow due to the size limits above. */
    unsigned long long lcsalloc = lcssize * sizeof(uint32_t);
    uint32_t *lcs = NULL;
    if (lcsalloc < SIZE_MAX && lcsalloc / lcssize == sizeof(uint32_t))
        lcs = ztrymalloc(lcsalloc);
    if (!lcs) {
        addReplyError(c, "Insufficient memory");
        goto cleanup;
    }

    /* Start building the LCS table. */
    for (uint32_t i = 0; i <= alen; i++) {
        for (uint32_t j = 0; j <= blen; j++) {
            if (i == 0 || j == 0) {
                /* If one substring has length of zero, the
                 * LCS length is zero. */
                LCS(i,j) = 0;
            } else if (a[i-1] == b[j-1]) {
                /* The len LCS (and the LCS itself) of two
                 * sequences with the same final character, is the
                 * LCS of the two sequences without the last char
                 * plus that last char. */
                LCS(i,j) = LCS(i-1,j-1)+1;
            } else {
                /* If the last character is different, take the longest
                 * between the LCS of the first string and the second
                 * minus the last char, and the reverse. */
                uint32_t lcs1 = LCS(i-1,j);
                uint32_t lcs2 = LCS(i,j-1);
                LCS(i,j) = lcs1 > lcs2 ? lcs1 : lcs2;
            }
        }
    }

    /* Store the actual LCS string in "result" if needed. We create
     * it backward, but the length is already known, we store it into idx. */
    uint32_t idx = LCS(alen,blen);
    sds result = NULL;        /* Resulting LCS string. */
    void *arraylenptr = NULL; /* Deffered length of the array for IDX. */
    uint32_t arange_start = alen, /* alen signals that values are not set. */
             arange_end = 0,
             brange_start = 0,
             brange_end = 0;

    /* Do we need to compute the actual LCS string? Allocate it in that case. */
    int computelcs = getidx || !getlen;
    if (computelcs) result = sdsnewlen(SDS_NOINIT,idx);

    /* Start with a deferred array if we have to emit the ranges. */
    uint32_t arraylen = 0;  /* Number of ranges emitted in the array. */
    if (getidx) {
        addReplyMapLen(c,2);
        addReplyBulkCString(c,"matches");
        arraylenptr = addReplyDeferredLen(c);
    }

    i = alen, j = blen;
    while (computelcs && i > 0 && j > 0) {
        int emit_range = 0;
        if (a[i-1] == b[j-1]) {
            /* If there is a match, store the character and reduce
             * the indexes to look for a new match. */
            result[idx-1] = a[i-1];

            /* Track the current range. */
            if (arange_start == alen) {
                arange_start = i-1;
                arange_end = i-1;
                brange_start = j-1;
                brange_end = j-1;
            } else {
                /* Let's see if we can extend the range backward since
                 * it is contiguous. */
                if (arange_start == i && brange_start == j) {
                    arange_start--;
                    brange_start--;
                } else {
                    emit_range = 1;
                }
            }
            /* Emit the range if we matched with the first byte of
             * one of the two strings. We'll exit the loop ASAP. */
            if (arange_start == 0 || brange_start == 0) emit_range = 1;
            idx--; i--; j--;
        } else {
            /* Otherwise reduce i and j depending on the largest
             * LCS between, to understand what direction we need to go. */
            uint32_t lcs1 = LCS(i-1,j);
            uint32_t lcs2 = LCS(i,j-1);
            if (lcs1 > lcs2)
                i--;
            else
                j--;
            if (arange_start != alen) emit_range = 1;
        }

        /* Emit the current range if needed. */
        uint32_t match_len = arange_end - arange_start + 1;
        if (emit_range) {
            if (minmatchlen == 0 || match_len >= minmatchlen) {
                if (arraylenptr) {
                    addReplyArrayLen(c,2+withmatchlen);
                    addReplyArrayLen(c,2);
                    addReplyLongLong(c,arange_start);
                    addReplyLongLong(c,arange_end);
                    addReplyArrayLen(c,2);
                    addReplyLongLong(c,brange_start);
                    addReplyLongLong(c,brange_end);
                    if (withmatchlen) addReplyLongLong(c,match_len);
                    arraylen++;
                }
            }
            arange_start = alen; /* Restart at the next match. */
        }
    }

    /* Signal modified key, increment dirty, ... */

    /* Reply depending on the given options. */
    if (arraylenptr) {
        addReplyBulkCString(c,"len");
        addReplyLongLong(c,LCS(alen,blen));
        setDeferredArrayLen(c,arraylenptr,arraylen);
    } else if (getlen) {
        addReplyLongLong(c,LCS(alen,blen));
    } else {
        addReplyBulkSds(c,result);
        result = NULL;
    }

    /* Cleanup. */
    sdsfree(result);
    zfree(lcs);

cleanup:
    if (obja) decrRefCount(obja);
    if (objb) decrRefCount(objb);
    return;
}

