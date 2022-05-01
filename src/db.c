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
#include "cluster.h"
#include "atomicvar.h"
#include "latency.h"

#include <signal.h>
#include <ctype.h>

/* Database backup. */
struct dbBackup {
    redisDb *dbarray;
    rax *slots_to_keys;
    uint64_t slots_keys_count[CLUSTER_SLOTS];
};

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

int keyIsExpired(redisDb *db, robj *key);

/* Update LFU when an object is accessed.
 * Firstly, decrement the counter if the decrement time is reached.
 * Then logarithmically increment the counter, and update the access time.
 *
 * 当一个对象被访问时，更新其LFU
 * 首先，如果达到递减时间，则递减计数器。
 * 然后，对数递增计数器，并更新访问时间。
 * */
void updateLFU(robj *val) {
    /* LFUDecrAndReturn提供了一个随时间衰减的过程，避免老的数据访问次数越来越大 */
    unsigned long counter = LFUDecrAndReturn(val);
    counter = LFULogIncr(counter);
    /* lru的低8位存储的是对象的访问次数，高16位存储的是对象上次访问时间，以分钟为单位 */
    val->lru = (LFUGetTimeInMinutes()<<8) | counter;
}

/* 低级键查找 API，实际上并不直接从命令实现中调用，而是应该依赖
 * lookupKeyRead()、lookupKeyWrite() 和 lookupKeyReadWithFlags()。 */
robj *lookupKey(redisDb *db, robj *key, int flags) {
    dictEntry *de = dictFind(db->dict,key->ptr);
    if (de) {
        /* 查找dict中，key对应的entry，返回entry中的value对象 */
        robj *val = dictGetVal(de);

        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        /* 没有rdb和aof线程运行，且flag不是LOOKUP_NOTOUCH则根据淘汰策略更新参数
         */
        if (!hasActiveChildProcess() && !(flags & LOOKUP_NOTOUCH)){
            if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
                updateLFU(val); /* LFU淘汰策略，更新LFU */
            } else {
                val->lru = LRU_CLOCK(); /* LRU淘汰策略，更新LRU */
            }
        }
        return val;
    } else {
        return NULL;
    }
}

/* Lookup a key for read operations, or return NULL if the key is not found
 * in the specified DB.
 *
 * As a side effect of calling this function:
 * 1. A key gets expired if it reached it's TTL.
 * 2. The key last access time is updated.
 * 3. The global keys hits/misses stats are updated (reported in INFO).
 * 4. If keyspace notifications are enabled, a "keymiss" notification is fired.
 *
 * This API should not be used when we write to the key after obtaining
 * the object linked to the key, but only for read only operations.
 *
 * Flags change the behavior of this command:
 *
 *  LOOKUP_NONE (or zero): no special flags are passed.
 *  LOOKUP_NOTOUCH: don't alter the last access time of the key.
 *
 * Note: this function also returns NULL if the key is logically expired
 * but still existing, in case this is a slave, since this API is called only
 * for read operations. Even if the key expiry is master-driven, we can
 * correctly report a key is expired on slaves even if the master is lagging
 * expiring our key via DELs in the replication link. */
/* 查找读取操作的键，如果在指定的数据库中没有找到该键，则返回NULL。
 * 作为调用此函数的一个副作用：
 * 1. 达到TTL的key会过期
 * 2. key的最后一次访问时间会更新
 * 3. 全局key的命中/未命中状态会更新
 * 4. 如果keyspace通知被启用了，将会触发“keymiss”通知
 *
 * 这个API只应用于读操作
 * 
 * LOOKUP_NONE：无特殊操作
 * LOOKUP_NOTOUCH：不要更新最后访问时间
 *
 * 如果这个key存在但已经过期，会返回NULL
 */
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    robj *val;

    /* 检查key是否已经过期，如果已过期的话，那么将它删除
     * 可以避免处理过期的键
     */
    if (expireIfNeeded(db,key) == 1) {
        /* If we are in the context of a master, expireIfNeeded() returns 1
         * when the key is no longer valid, so we can return NULL ASAP. */
        if (server.masterhost == NULL)
            goto keymiss;

        /* However if we are in the context of a slave, expireIfNeeded() will
         * not really try to expire the key, it only returns information
         * about the "logical" status of the key: key expiring is up to the
         * master in order to have a consistent view of master's data set.
         *
         * However, if the command caller is not the master, and as additional
         * safety measure, the command invoked is a read-only command, we can
         * safely return NULL here, and provide a more consistent behavior
         * to clients accessing expired values in a read-only fashion, that
         * will say the key as non existing.
         *
         * Notably this covers GETs when slaves are used to scale reads. */
        if (server.current_client &&
            server.current_client != server.master &&
            server.current_client->cmd &&
            server.current_client->cmd->flags & CMD_READONLY)
        {
            goto keymiss;
        }
    }
    /* 这里获取的是value对象，而非key对象 */
    val = lookupKey(db,key,flags);
    if (val == NULL)
        goto keymiss;
    /* 更新命中信息 */
    server.stat_keyspace_hits++;
    return val;

keymiss:
    if (!(flags & LOOKUP_NONOTIFY)) {
        notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id);
    }
    /* 更新不命中信息 */
    server.stat_keyspace_misses++;
    return NULL;
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the common case. */
/* 为执行读取操作而取出键 key 在数据库 db 中的值。
 * 并根据是否成功找到值，更新服务器的命中/不命中信息。
 * 找到时返回值对象，没找到返回 NULL 。
 */
robj *lookupKeyRead(redisDb *db, robj *key) {
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* 查找用于写操作的键，如果键达到TTL时间，使键过期
 * 如果键存在，则返回链接的值对象；如果指定的 DB 中不存在键，则返回 NULL.
 * */
robj *lookupKeyWriteWithFlags(redisDb *db, robj *key, int flags) {
    /* 删除过期键 */
    expireIfNeeded(db, key);
    /* 查找并返回 key 的值对象 */
    return lookupKey(db, key, flags);
}

/* 为执行写入操作而取出键 key 在数据库 db 中的值
 * 和 lookupKeyRead 不同，这个函数会更新服务器的命中/不命中信息
 * 找到时返回值对象，没找到返回 NULL
 */
robj *lookupKeyWrite(redisDb *db, robj *key) {
    /* 查找并返回 key 的值对象 */
    return lookupKeyWriteWithFlags(db, key, LOOKUP_NONE);
}
/* 返回key不存在的响应给client */
void SentReplyOnKeyMiss(client *c, robj *reply){
    serverAssert(sdsEncodedObject(reply));
    sds rep = reply->ptr;
    if (sdslen(rep) > 1 && rep[0] == '-'){
        addReplyErrorObject(c, reply);
    } else {
        addReply(c,reply);
    }
}
robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);    /* 查找键对应的值对象 */
    if (!o) SentReplyOnKeyMiss(c, reply);   /* 如果值对象为空，发送响应到client */
    return o;                               /* 返回值对象 */
}

robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) SentReplyOnKeyMiss(c, reply);
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * The program is aborted if the key already exists. */
void dbAdd(redisDb *db, robj *key, robj *val) {
    sds copy = sdsdup(key->ptr);
    int retval = dictAdd(db->dict, copy, val);

    serverAssertWithInfo(NULL,key,retval == DICT_OK);
    signalKeyAsReady(db, key, val->type);
    if (server.cluster_enabled) slotToKeyAdd(key->ptr);
}

/* This is a special version of dbAdd() that is used only when loading
 * keys from the RDB file: the key is passed as an SDS string that is
 * retained by the function (and not freed by the caller).
 *
 * Moreover this function will not abort if the key is already busy, to
 * give more control to the caller, nor will signal the key as ready
 * since it is not useful in this context.
 *
 * The function returns 1 if the key was added to the database, taking
 * ownership of the SDS string, otherwise 0 is returned, and is up to the
 * caller to free the SDS string. */
int dbAddRDBLoad(redisDb *db, sds key, robj *val) {
    int retval = dictAdd(db->dict, key, val);
    if (retval != DICT_OK) return 0;
    if (server.cluster_enabled) slotToKeyAdd(key);
    return 1;
}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * The program is aborted if the key was not already present. */
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    dictEntry *de = dictFind(db->dict,key->ptr); /* 查找键是否存在，返回存在的节点*/
    serverAssertWithInfo(NULL,key,de != NULL);   /* 不存在键则中断执行*/
    dictEntry auxentry = *de;
    robj *old = dictGetVal(de); /* 获取节点的val的字段值*/
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        val->lru = old->lru;
    }
    /* Although the key is not really deleted from the database, we regard 
    overwrite as two steps of unlink+add, so we still need to call the unlink 
    callback of the module. */
    moduleNotifyKeyUnlink(key,old);
    dictSetVal(db->dict, de, val); /* 给节点设置新的值 */

    if (server.lazyfree_lazy_server_del) {
        freeObjAsync(key,old);
        dictSetVal(db->dict, &auxentry, NULL);
    }

    dictFreeVal(db->dict, &auxentry);   /* 释放节点中旧val的内存 */
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent),
 *    unless 'keepttl' is true.
 *
 * All the new keys in the database should be created via this interface.
 * The client 'c' argument may be set to NULL if the operation is performed
 * in a context where there is no clear client performing the operation. */
void genericSetKey(client *c, redisDb *db, robj *key, robj *val, int keepttl, int signal) {
    if (lookupKeyWrite(db,key) == NULL) {
        dbAdd(db,key,val);
    } else {
        dbOverwrite(db,key,val);
    }
    incrRefCount(val);
    if (!keepttl) removeExpire(db,key);
    if (signal) signalModifiedKey(c,db,key);
}

/* Common case for genericSetKey() where the TTL is not retained. */
void setKey(client *c, redisDb *db, robj *key, robj *val) {
    genericSetKey(c,db,key,val,0,1);
}

/* 随机从数据库中取出一个键，并以字符串对象的方式返回这个键
 * 如果数据库为空，那么返回 NULL
 *
 * 这个函数保证被返回的键都是未过期的 */
robj *dbRandomKey(redisDb *db) {
    dictEntry *de;
    int maxtries = 100;
    /* 数据库键数量与过期数量一致，每个键都有个过期时间？ */
    int allvolatile = dictSize(db->dict) == dictSize(db->expires);

    while(1) {
        sds key;
        robj *keyobj;
        /* 从键空间中随机取出一个键节点 */
        de = dictGetFairRandomKey(db->dict);
        /* 数据库为空 */
        if (de == NULL) return NULL;
        /* 取出键 */
        key = dictGetKey(de);
        /* 为键创建一个字符串对象，对象的值为键的名字 */
        keyobj = createStringObject(key,sdslen(key));
        /* 检查键是否带有过期时间 */
        if (dictFind(db->expires,key)) {
            if (allvolatile && server.masterhost && --maxtries == 0) {
                /* 如果数据库仅由设置了过期的键组成，则可能会发生所有键在从属服务器中已经在逻辑上过期，因此函数无法停止，
                 * 因为 expireIfNeeded() 为假，也无法停止，
                 * 因为 dictGetRandomKey() 返回不为 NULL （有返回键）。
                 * 为了防止无限循环，我们做了一些尝试，但是如果有无限循环的条件，最终我们会返回一个可能已经过期的键名。
                 */
                return keyobj;
            }
            /* 如果键已经过期，那么将它删除，并继续随机下个键 */
            if (expireIfNeeded(db,keyobj)) {
                decrRefCount(keyobj);
                continue; /* search for another key. This expired. */
            }
        }
        /* 返回被随机到的键（的名字） */
        return keyobj;
    }
}

/* 从数据库中删除键、值和关联的过期条目（如果有） */
int dbSyncDelete(redisDb *db, robj *key) {
    /* 从 expires 字典中删除条目不会释放 key 的 sds，因为它与主字典共享。 */
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    dictEntry *de = dictUnlink(db->dict,key->ptr);
    if (de) {
        robj *val = dictGetVal(de);
        /* 告诉模块key已从数据库中移除了 */
        moduleNotifyKeyUnlink(key,val);
        /* 释放节点数据 */
        dictFreeUnlinkedEntry(db->dict,de);
        if (server.cluster_enabled) slotToKeyDel(key->ptr);
        return 1;
    } else {
        return 0;
    }
}

/* This is a wrapper whose behavior depends on the Redis lazy free
 * configuration. Deletes the key synchronously or asynchronously. */
int dbDelete(redisDb *db, robj *key) {
    return server.lazyfree_lazy_server_del ? dbAsyncDelete(db,key) :
                                             dbSyncDelete(db,key);
}

/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 */
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
    serverAssert(o->type == OBJ_STRING);
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);
        dbOverwrite(db,key,o);
    }
    return o;
}

/* Remove all keys from the database(s) structure. The dbarray argument
 * may not be the server main DBs (could be a backup).
 *
 * The dbnum can be -1 if all the DBs should be emptied, or the specified
 * DB index if we want to empty only a single database.
 * The function returns the number of keys removed from the database(s). */
long long emptyDbStructure(redisDb *dbarray, int dbnum, int async,
                           void(callback)(void*))
{
    long long removed = 0;
    int startdb, enddb;

    if (dbnum == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        startdb = enddb = dbnum;
    }

    for (int j = startdb; j <= enddb; j++) {
        removed += dictSize(dbarray[j].dict);
        if (async) {
            emptyDbAsync(&dbarray[j]);
        } else {
            dictEmpty(dbarray[j].dict,callback);
            dictEmpty(dbarray[j].expires,callback);
        }
        /* Because all keys of database are removed, reset average ttl. */
        dbarray[j].avg_ttl = 0;
        dbarray[j].expires_cursor = 0;
    }

    return removed;
}

/* Remove all keys from all the databases in a Redis server.
 * If callback is given the function is called from time to time to
 * signal that work is in progress.
 *
 * The dbnum can be -1 if all the DBs should be flushed, or the specified
 * DB number if we want to flush only a single Redis database number.
 *
 * Flags are be EMPTYDB_NO_FLAGS if no special flags are specified or
 * EMPTYDB_ASYNC if we want the memory to be freed in a different thread
 * and the function to return ASAP.
 *
 * On success the function returns the number of keys removed from the
 * database(s). Otherwise -1 is returned in the specific case the
 * DB number is out of range, and errno is set to EINVAL. */
long long emptyDb(int dbnum, int flags, void(callback)(void*)) {
    int async = (flags & EMPTYDB_ASYNC);
    RedisModuleFlushInfoV1 fi = {REDISMODULE_FLUSHINFO_VERSION,!async,dbnum};
    long long removed = 0;

    if (dbnum < -1 || dbnum >= server.dbnum) {
        errno = EINVAL;
        return -1;
    }

    /* Fire the flushdb modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_FLUSHDB,
                          REDISMODULE_SUBEVENT_FLUSHDB_START,
                          &fi);

    /* Make sure the WATCHed keys are affected by the FLUSH* commands.
     * Note that we need to call the function while the keys are still
     * there. */
    signalFlushedDb(dbnum, async);

    /* Empty redis database structure. */
    removed = emptyDbStructure(server.db, dbnum, async, callback);

    /* Flush slots to keys map if enable cluster, we can flush entire
     * slots to keys map whatever dbnum because only support one DB
     * in cluster mode. */
    if (server.cluster_enabled) slotToKeyFlush(async);

    if (dbnum == -1) flushSlaveKeysWithExpireList();

    /* Also fire the end event. Note that this event will fire almost
     * immediately after the start event if the flush is asynchronous. */
    moduleFireServerEvent(REDISMODULE_EVENT_FLUSHDB,
                          REDISMODULE_SUBEVENT_FLUSHDB_END,
                          &fi);

    return removed;
}

/* Store a backup of the database for later use, and put an empty one
 * instead of it. */
dbBackup *backupDb(void) {
    dbBackup *backup = zmalloc(sizeof(dbBackup));

    /* Backup main DBs. */
    backup->dbarray = zmalloc(sizeof(redisDb)*server.dbnum);
    for (int i=0; i<server.dbnum; i++) {
        backup->dbarray[i] = server.db[i];
        server.db[i].dict = dictCreate(&dbDictType,NULL);
        server.db[i].expires = dictCreate(&dbExpiresDictType,NULL);
    }

    /* Backup cluster slots to keys map if enable cluster. */
    if (server.cluster_enabled) {
        backup->slots_to_keys = server.cluster->slots_to_keys;
        memcpy(backup->slots_keys_count, server.cluster->slots_keys_count,
            sizeof(server.cluster->slots_keys_count));
        server.cluster->slots_to_keys = raxNew();
        memset(server.cluster->slots_keys_count, 0,
            sizeof(server.cluster->slots_keys_count));
    }

    moduleFireServerEvent(REDISMODULE_EVENT_REPL_BACKUP,
                          REDISMODULE_SUBEVENT_REPL_BACKUP_CREATE,
                          NULL);

    return backup;
}

/* Discard a previously created backup, this can be slow (similar to FLUSHALL)
 * Arguments are similar to the ones of emptyDb, see EMPTYDB_ flags. */
void discardDbBackup(dbBackup *buckup, int flags, void(callback)(void*)) {
    int async = (flags & EMPTYDB_ASYNC);

    /* Release main DBs backup . */
    emptyDbStructure(buckup->dbarray, -1, async, callback);
    for (int i=0; i<server.dbnum; i++) {
        dictRelease(buckup->dbarray[i].dict);
        dictRelease(buckup->dbarray[i].expires);
    }

    /* Release slots to keys map backup if enable cluster. */
    if (server.cluster_enabled) freeSlotsToKeysMap(buckup->slots_to_keys, async);

    /* Release buckup. */
    zfree(buckup->dbarray);
    zfree(buckup);

    moduleFireServerEvent(REDISMODULE_EVENT_REPL_BACKUP,
                          REDISMODULE_SUBEVENT_REPL_BACKUP_DISCARD,
                          NULL);
}

/* Restore the previously created backup (discarding what currently resides
 * in the db).
 * This function should be called after the current contents of the database
 * was emptied with a previous call to emptyDb (possibly using the async mode). */
void restoreDbBackup(dbBackup *buckup) {
    /* Restore main DBs. */
    for (int i=0; i<server.dbnum; i++) {
        serverAssert(dictSize(server.db[i].dict) == 0);
        serverAssert(dictSize(server.db[i].expires) == 0);
        dictRelease(server.db[i].dict);
        dictRelease(server.db[i].expires);
        server.db[i] = buckup->dbarray[i];
    }

    /* Restore slots to keys map backup if enable cluster. */
    if (server.cluster_enabled) {
        serverAssert(server.cluster->slots_to_keys->numele == 0);
        raxFree(server.cluster->slots_to_keys);
        server.cluster->slots_to_keys = buckup->slots_to_keys;
        memcpy(server.cluster->slots_keys_count, buckup->slots_keys_count,
                sizeof(server.cluster->slots_keys_count));
    }

    /* Release buckup. */
    zfree(buckup->dbarray);
    zfree(buckup);

    moduleFireServerEvent(REDISMODULE_EVENT_REPL_BACKUP,
                          REDISMODULE_SUBEVENT_REPL_BACKUP_RESTORE,
                          NULL);
}

int selectDb(client *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return C_ERR;
    c->db = &server.db[id];
    return C_OK;
}

long long dbTotalServerKeyCount() {
    long long total = 0;
    int j;
    for (j = 0; j < server.dbnum; j++) {
        total += dictSize(server.db[j].dict);
    }
    return total;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/

/* Note that the 'c' argument may be NULL if the key was modified out of
 * a context of a client. */
void signalModifiedKey(client *c, redisDb *db, robj *key) {
    touchWatchedKey(db,key);
    trackingInvalidateKey(c,key);
}

void signalFlushedDb(int dbid, int async) {
    int startdb, enddb;
    if (dbid == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        startdb = enddb = dbid;
    }

    for (int j = startdb; j <= enddb; j++) {
        touchAllWatchedKeysInDb(&server.db[j], NULL);
    }

    trackingInvalidateKeysOnFlush(async);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

/* Return the set of flags to use for the emptyDb() call for FLUSHALL
 * and FLUSHDB commands.
 *
 * sync: flushes the database in an sync manner.
 * async: flushes the database in an async manner.
 * no option: determine sync or async according to the value of lazyfree-lazy-user-flush.
 *
 * On success C_OK is returned and the flags are stored in *flags, otherwise
 * C_ERR is returned and the function sends an error to the client. */
int getFlushCommandFlags(client *c, int *flags) {
    /* Parse the optional ASYNC option. */
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"sync")) {
        *flags = EMPTYDB_NO_FLAGS;
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"async")) {
        *flags = EMPTYDB_ASYNC;
    } else if (c->argc == 1) {
        *flags = server.lazyfree_lazy_user_flush ? EMPTYDB_ASYNC : EMPTYDB_NO_FLAGS;
    } else {
        addReplyErrorObject(c,shared.syntaxerr);
        return C_ERR;
    }
    return C_OK;
}

/* Flushes the whole server data set. */
void flushAllDataAndResetRDB(int flags) {
    server.dirty += emptyDb(-1,flags,NULL);
    if (server.child_type == CHILD_TYPE_RDB) killRDBChild();
    if (server.saveparamslen > 0) {
        /* Normally rdbSave() will reset dirty, but we don't want this here
         * as otherwise FLUSHALL will not be replicated nor put into the AOF. */
        int saved_dirty = server.dirty;
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        rdbSave(server.rdb_filename,rsiptr);
        server.dirty = saved_dirty;
    }

    /* Without that extra dirty++, when db was already empty, FLUSHALL will
     * not be replicated nor put into the AOF. */
    server.dirty++;
#if defined(USE_JEMALLOC)
    /* jemalloc 5 doesn't release pages back to the OS when there's no traffic.
     * for large databases, flushdb blocks for long anyway, so a bit more won't
     * harm and this way the flush and purge will be synchroneus. */
    if (!(flags & EMPTYDB_ASYNC))
        jemalloc_purge();
#endif
}

/* FLUSHDB [ASYNC]
 *
 * Flushes the currently SELECTed Redis DB. */
void flushdbCommand(client *c) {
    int flags;

    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    server.dirty += emptyDb(c->db->id,flags,NULL);
    addReply(c,shared.ok);
#if defined(USE_JEMALLOC)
    /* jemalloc 5 doesn't release pages back to the OS when there's no traffic.
     * for large databases, flushdb blocks for long anyway, so a bit more won't
     * harm and this way the flush and purge will be synchroneus. */
    if (!(flags & EMPTYDB_ASYNC))
        jemalloc_purge();
#endif
}

/* FLUSHALL [ASYNC]
 *
 * Flushes the whole server data set. */
void flushallCommand(client *c) {
    int flags;
    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    flushAllDataAndResetRDB(flags);
    addReply(c,shared.ok);
}

/* 此命令实现 DEL 和 LAZYDEL. */
void delGenericCommand(client *c, int lazy) {
    int numdel = 0, j;

    /* 遍历所有输入键 */
    for (j = 1; j < c->argc; j++) {
        /* 先删除过期的键 */
        expireIfNeeded(c->db,c->argv[j]);
        int deleted  = lazy ? dbAsyncDelete(c->db,c->argv[j]) :
                              dbSyncDelete(c->db,c->argv[j]);
        if (deleted) {
            /* 删除键成功，发送通知 */
            signalModifiedKey(c,c->db,c->argv[j]);
            notifyKeyspaceEvent(NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            server.dirty++;
            /* 成功删除，增加 deleted 计数器的值 */
            numdel++;
        }
    }
    addReplyLongLong(c,numdel);
}

/* 以阻塞方式删除key */
void delCommand(client *c) {
    /* del调用函数delGenericCommand，再循环调用dbSyncDelete函数，
     * 同步删除key、value、过期字典里对应的key（如果有），
     * 如果是集群还会删除key与slot（槽位）的对应关系 */
    delGenericCommand(c,server.lazyfree_lazy_user_del);
}

/* 以非阻塞方式删除key */
void unlinkCommand(client *c) {
    delGenericCommand(c,1);
}

/* EXISTS key1 key2 ... key_N.
 * Return value is the number of keys existing. */
/* 检查键是否存在，返回存在的键的数量 */
void existsCommand(client *c) {
    long long count = 0;
    int j;
    /* 遍历参数，检查键是否存在 */
    for (j = 1; j < c->argc; j++) {
        if (lookupKeyReadWithFlags(c->db,c->argv[j],LOOKUP_NOTOUCH)) count++;
    }
    /* 返回存在的键的数量 */
    addReplyLongLong(c,count);
}

void selectCommand(client *c) {
    int id;

    if (getIntFromObjectOrReply(c, c->argv[1], &id, NULL) != C_OK)
        return;

    if (server.cluster_enabled && id != 0) {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }
    if (selectDb(c,id) == C_ERR) {
        addReplyError(c,"DB index is out of range");
    } else {
        addReply(c,shared.ok);
    }
}

/* 当前数据库中随机返回一个尚未过期的key */
void randomkeyCommand(client *c) {
    robj *key;
    /* 随机返回键 */
    if ((key = dbRandomKey(c->db)) == NULL) {
        addReplyNull(c);
        return;
    }

    addReplyBulk(c,key);
    decrRefCount(key);
}

/* 匹配合适的key并一次性返回，如果匹配的键较多，
 * 则可能阻塞服务器，因此该命令一般禁止在线上使用
 */
void keysCommand(client *c) {
    dictIterator *di;
    dictEntry *de;
    /* 查找模式 */
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern), allkeys;
    unsigned long numkeys = 0;
    void *replylen = addReplyDeferredLen(c);

    /* 遍历整个数据库，返回（名字）和模式匹配的键 */
    di = dictGetSafeIterator(c->db->dict);
    allkeys = (pattern[0] == '*' && plen == 1);
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetKey(de);
        robj *keyobj;
        /* 将键名和查找模式进行比对 */
        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            /* 创建一个保存键名字的字符串对象 */
            keyobj = createStringObject(key,sdslen(key));
            /* 键是否已过期 */
            if (!keyIsExpired(c->db,keyobj)) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    /* 释放迭代器 */
    dictReleaseIterator(di);
    setDeferredArrayLen(c,replylen,numkeys);
}

/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. */
void scanCallback(void *privdata, const dictEntry *de) {
    void **pd = (void**) privdata;
    list *keys = pd[0];
    robj *o = pd[1];
    robj *key, *val = NULL;

    if (o == NULL) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey, sdslen(sdskey));
    } else if (o->type == OBJ_SET) {
        sds keysds = dictGetKey(de);
        key = createStringObject(keysds,sdslen(keysds));
    } else if (o->type == OBJ_HASH) {
        sds sdskey = dictGetKey(de);
        sds sdsval = dictGetVal(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObject(sdsval,sdslen(sdsval));
    } else if (o->type == OBJ_ZSET) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObjectFromLongDouble(*(double*)dictGetVal(de),0);
    } else {
        serverPanic("Type not handled in SCAN callback.");
    }

    listAddNodeTail(keys, key);
    if (val) listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns C_OK. Otherwise return C_ERR and send an error to the
 * client. */
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor) {
    char *eptr;

    /* Use strtoul() because we need an *unsigned* long, so
     * getLongLongFromObject() does not cover the whole cursor space. */
    errno = 0;
    *cursor = strtoul(o->ptr, &eptr, 10);
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
    {
        addReplyError(c, "invalid cursor");
        return C_ERR;
    }
    return C_OK;
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 * If object 'o' is passed, then it must be a Hash, Set or Zset object,
 * otherwise if 'o' is NULL the command will operate on the dictionary
 * associated with the current database.
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * In the case of a Hash object the function returns both the field and value
 * of every element on the Hash.
 *
 * 如果给定了对象 o ，那么它必须是一个哈希对象或者集合对象，
 * 如果 o 为 NULL 的话，函数将使用当前数据库作为迭代对象。
 *
 * 如果参数 o 不为 NULL ，那么说明它是一个键对象，函数将跳过这些键对象，
 * 对给定的命令选项进行分析（parse）
 *
 * 如果被迭代的是哈希对象，那么函数返回的是键值对。
 * */
void scanGenericCommand(client *c, robj *o, unsigned long cursor) {
    int i, j;
    list *keys = listCreate();
    listNode *node, *nextnode;
    long count = 10;
    sds pat = NULL;
    sds typename = NULL;
    int patlen = 0, use_pattern = 0;
    dict *ht;

    /* 输入类型检查
     * Object 必须为 NULL（以迭代键名），或者对象的类型必须是 Set、Sorted Set 或
     * Hash */
    serverAssert(o == NULL || o->type == OBJ_SET || o->type == OBJ_HASH ||
                o->type == OBJ_ZSET);

    /* 将 i 设置为第一个选项参数。 前一个是光标
     * 设置第一个选项参数的索引位置
     * 0    1      2      3
     * SCAN OPTION <op_arg>         SCAN 命令的选项值从索引 2 开始
     * HSCAN <key> OPTION <op_arg>  而其他 *SCAN 命令的选项值从索引 3 开始
     */
    i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. */

    /* 解析选项参数 */
    while (i < c->argc) {
        j = c->argc - i;
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            /* COUNT <number> */
            if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
                != C_OK)
            {
                goto cleanup;
            }

            if (count < 1) {
                addReplyErrorObject(c,shared.syntaxerr);
                goto cleanup;
            }

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
            /* MATCH <pattern> */
            pat = c->argv[i+1]->ptr;
            patlen = sdslen(pat);

            /* 如果模式正好是“*”，则该模式始终匹配，因此相当于禁用它 */
            use_pattern = !(pat[0] == '*' && patlen == 1);

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "type") && o == NULL && j >= 2) {
            /* 特定类型的 SCAN 仅适用于 db dict */
            typename = c->argv[i+1]->ptr;
            i+= 2;
        } else {
            /* error */
            addReplyErrorObject(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Step 2: Iterate the collection.
     *
     * Note that if the object is encoded with a ziplist, intset, or any other
     * representation that is not a hash table, we are sure that it is also
     * composed of a small number of elements. So to avoid taking state we
     * just return everything inside the object in a single call, setting the
     * cursor to zero to signal the end of the iteration. */

    /*
     * 开始迭代集合，如果key保存为ziplist或者intset，则一次性返回所有数据，游标为0
     *（scan命令的游标参数为0时表示新一轮迭代开始，命令返回的游标值为0时表示迭代结束）
     * 由于Redis设计只有数据量比较小的时候才会保存为ziplist或者intset，所以此处不会影响性能
     */

    /* 处理哈希表的情况 */
    ht = NULL;
    if (o == NULL) {
        ht = c->db->dict; /* 迭代目标为数据库 */
    } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr; /* 迭代目标为 HT 编码的集合 */
    } else if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr; /* 迭代目标为 HT 编码的哈希 */
        count *= 2;  /* 我们返回此类型的键/值 */
    } else if (o->type == OBJ_ZSET && o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        ht = zs->dict; /* 迭代目标为 HT 编码的跳跃表 */
        count *= 2;    /* 我们返回此类型的键/值 */
    }

    /* 游标在保存为Hash的时候发挥作用，具体入口函数为dictScan */
    if (ht) {
        void *privdata[2];
        /* We set the max number of iterations to ten times the specified
         * COUNT, so if the hash table is in a pathological state (very
         * sparsely populated) we avoid to block too much time at the cost
         * of returning no or very few elements. */
        long maxiterations = count*10;

        /* We pass two pointers to the callback: the list to which it will
         * add new elements, and the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way. */
        privdata[0] = keys;
        privdata[1] = o;
        do {
            cursor = dictScan(ht, cursor, scanCallback, NULL, privdata);
        } while (cursor &&
              maxiterations-- &&
              listLength(keys) < (unsigned long)count);
    } else if (o->type == OBJ_SET) {
        int pos = 0;
        int64_t ll;

        while(intsetGet(o->ptr,pos++,&ll))
            listAddNodeTail(keys,createStringObjectFromLongLong(ll));
        cursor = 0;
    } else if (o->type == OBJ_HASH || o->type == OBJ_ZSET) {
        unsigned char *p = ziplistIndex(o->ptr,0);
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;

        while(p) {
            ziplistGet(p,&vstr,&vlen,&vll);
            listAddNodeTail(keys,
                (vstr != NULL) ? createStringObject((char*)vstr,vlen) :
                                 createStringObjectFromLongLong(vll));
            p = ziplistNext(o->ptr,p);
        }
        cursor = 0;
    } else {
        serverPanic("Not handled encoding in SCAN.");
    }

    /* 根据match参数过滤返回值，并且如果这个键已经过期也会直接过滤掉
     *（Redis中键过期之后并不会立即删除） */
    node = listFirst(keys);
    while (node) {
        robj *kobj = listNodeValue(node);
        nextnode = listNextNode(node);
        int filter = 0;

        /* Filter element if it does not match the pattern. */
        if (use_pattern) {
            if (sdsEncodedObject(kobj)) {
                if (!stringmatchlen(pat, patlen, kobj->ptr, sdslen(kobj->ptr), 0))
                    filter = 1;
            } else {
                char buf[LONG_STR_SIZE];
                int len;

                serverAssert(kobj->encoding == OBJ_ENCODING_INT);
                len = ll2string(buf,sizeof(buf),(long)kobj->ptr);
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

        /* Filter an element if it isn't the type we want. */
        if (!filter && o == NULL && typename){
            robj* typecheck = lookupKeyReadWithFlags(c->db, kobj, LOOKUP_NOTOUCH);
            char* type = getObjectTypeName(typecheck);
            if (strcasecmp((char*) typename, type)) filter = 1;
        }

        /* Filter element if it is an expired key. */
        if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;

        /* Remove the element and its associated value if needed. */
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        /* If this is a hash or a sorted set, we have a flat list of
         * key-value elements, so if this element was filtered, remove the
         * value, or skip it if it was not filtered: we only match keys. */
        if (o && (o->type == OBJ_ZSET || o->type == OBJ_HASH)) {
            node = nextnode;
            serverAssert(node); /* assertion for valgrind (avoid NPD) */
            nextnode = listNextNode(node);
            if (filter) {
                kobj = listNodeValue(node);
                decrRefCount(kobj);
                listDelNode(keys, node);
            }
        }
        node = nextnode;
    }

    /* 返回结果到客户端，是一个数组，第1个值是游标，第2个值是具体的键值对 */
    addReplyArrayLen(c, 2);
    addReplyBulkLongLong(c,cursor);

    addReplyArrayLen(c, listLength(keys));
    while ((node = listFirst(keys)) != NULL) {
        robj *kobj = listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

cleanup:
    listSetFreeMethod(keys,decrRefCountVoid);
    listRelease(keys);
}

/* SCAN 命令完全依赖于 scanGenericCommand */
void scanCommand(client *c) {
    unsigned long cursor;
    /* 解析命令行游标参数 */
    if (parseScanCursorOrReply(c,c->argv[1],&cursor) == C_ERR) return;
    /* scan、sscan、hscan、zscan 统一入口函数 */
    scanGenericCommand(c,NULL,cursor);
}

void dbsizeCommand(client *c) {
    addReplyLongLong(c,dictSize(c->db->dict));
}

void lastsaveCommand(client *c) {
    addReplyLongLong(c,server.lastsave);
}

/* 获取对象type属性的名称 */
char* getObjectTypeName(robj *o) {
    char* type;
    if (o == NULL) {
        type = "none";
    } else {
        switch(o->type) {
        case OBJ_STRING: type = "string"; break;
        case OBJ_LIST: type = "list"; break;
        case OBJ_SET: type = "set"; break;
        case OBJ_ZSET: type = "zset"; break;
        case OBJ_HASH: type = "hash"; break;
        case OBJ_STREAM: type = "stream"; break;
        case OBJ_MODULE: {
            moduleValue *mv = o->ptr;
            type = mv->type->name;
        }; break;
        default: type = "unknown"; break;
        }
    }
    return type;
}

/* type命令接口 */
void typeCommand(client *c) {
    robj *o;
    /* 查找key对应的值对象，且不改变lru属性 */
    o = lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH);
    addReplyStatus(c, getObjectTypeName(o));
}

void shutdownCommand(client *c) {
    int flags = 0;

    if (c->argc > 2) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    } else if (c->argc == 2) {
        if (!strcasecmp(c->argv[1]->ptr,"nosave")) {
            flags |= SHUTDOWN_NOSAVE;
        } else if (!strcasecmp(c->argv[1]->ptr,"save")) {
            flags |= SHUTDOWN_SAVE;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }
    if (prepareForShutdown(flags) == C_OK) exit(0);
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

void renameGenericCommand(client *c, int nx) {
    robj *o;
    long long expire;
    int samekey = 0;

    /* 当 source 和 dest key 相同时，不执行任何操作，
     * 如果 key 不存在，但我们仍然返回 unexisting key 的错误。 */
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) samekey = 1;

    /* 查找键是否存在，如果不存在，则返回 ERR no such key */
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;
    /* 来源键和目标键不能相同 */
    if (samekey) {
        addReply(c,nx ? shared.czero : shared.ok);
        return;
    }
    /* 增加引用计数，因为后面目标键也会引用这个对象
     * 如果不增加的话，当来源键被删除时，这个值对象也会被删除
     */
    incrRefCount(o);
    /* 取出来源键的过期时间 */
    expire = getExpire(c->db,c->argv[1]);
    /* 检查目标键是否存在 */
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
        /* 如果目标键存在，并且执行的是 RENAMENX ，那么直接返回 */
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one
         * with the same name. */
        /* 如果执行的是 RENAME ，那么删除已有的目标键 */
        dbDelete(c->db,c->argv[2]);
    }
    /* 将来源键的值对象和目标键进行关联 */
    dbAdd(c->db,c->argv[2],o);
    /* 如果有过期时间，那么为目标键设置过期时间 */
    if (expire != -1) setExpire(c,c->db,c->argv[2],expire);
    /* 删除来源键 */
    dbDelete(c->db,c->argv[1]);
    signalModifiedKey(c,c->db,c->argv[1]);
    signalModifiedKey(c,c->db,c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_from",
        c->argv[1],c->db->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_to",
        c->argv[2],c->db->id);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

/* 重命名键 */
void renameCommand(client *c) {
    renameGenericCommand(c,0);
}

/* 重命名键 */
void renamenxCommand(client *c) {
    renameGenericCommand(c,1);
}

/* 将key移动到另一个数据库 */
void moveCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid, dbid;
    long long expire;
    /* move命令不可用于集群模式 */
    if (server.cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* 源数据库 */
    src = c->db;
    /* 源数据库id */
    srcid = c->db->id;
    /* 由参数中获取目标数据库id */
    if (getIntFromObjectOrReply(c, c->argv[2], &dbid, NULL) != C_OK)
        return;
    /* 切换到目标数据库 */
    if (selectDb(c,dbid) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    }
    /* 目标数据库 */
    dst = c->db;
    /* 切回源数据库 */
    selectDb(c,srcid); /* Back to the source DB */

    /* 如果源数据库和目标数据库相等，那么返回错误 */
    if (src == dst) {
        addReplyErrorObject(c,shared.sameobjecterr);
        return;
    }

    /* 检查并取出要移动的对象 */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
    /* 获取超时时间 */
    expire = getExpire(c->db,c->argv[1]);

    /* 如果键已经存在于目标数据库，那么返回 */
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
    /* 将键添加到目标数据库中 */
    dbAdd(dst,c->argv[1],o);
    /* 如果key存在expire，目标数据库expire字典中添加key */
    if (expire != -1) setExpire(c,dst,c->argv[1],expire);
    /* 增加引用计数，避免接下来源数据库中删除时 o 被清理 */
    incrRefCount(o);

    /* 将键从源数据库中删除 */
    dbDelete(src,c->argv[1]);
    signalModifiedKey(c,src,c->argv[1]);
    signalModifiedKey(c,dst,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,
                "move_from",c->argv[1],src->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,
                "move_to",c->argv[1],dst->id);

    server.dirty++;
    addReply(c,shared.cone);
}

void copyCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid, dbid;
    long long expire;
    int j, replace = 0, delete = 0;

    /* Obtain source and target DB pointers 
     * Default target DB is the same as the source DB 
     * Parse the REPLACE option and targetDB option. */
    src = c->db;
    dst = c->db;
    srcid = c->db->id;
    dbid = c->db->id;
    for (j = 3; j < c->argc; j++) {
        int additional = c->argc - j - 1;
        if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp(c->argv[j]->ptr, "db") && additional >= 1) {
            if (getIntFromObjectOrReply(c, c->argv[j+1], &dbid, NULL) != C_OK)
                return;

            if (selectDb(c, dbid) == C_ERR) {
                addReplyError(c,"DB index is out of range");
                return;
            }
            dst = c->db;
            selectDb(c,srcid); /* Back to the source DB */
            j++; /* Consume additional arg. */
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    if ((server.cluster_enabled == 1) && (srcid != 0 || dbid != 0)) {
        addReplyError(c,"Copying to another database is not allowed in cluster mode");
        return;
    }

    /* If the user select the same DB as
     * the source DB and using newkey as the same key
     * it is probably an error. */
    robj *key = c->argv[1];
    robj *newkey = c->argv[2];
    if (src == dst && (sdscmp(key->ptr, newkey->ptr) == 0)) {
        addReplyErrorObject(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    o = lookupKeyWrite(c->db, key);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
    expire = getExpire(c->db,key);

    /* Return zero if the key already exists in the target DB. 
     * If REPLACE option is selected, delete newkey from targetDB. */
    if (lookupKeyWrite(dst,newkey) != NULL) {
        if (replace) {
            delete = 1;
        } else {
            addReply(c,shared.czero);
            return;
        }
    }

    /* Duplicate object according to object's type. */
    robj *newobj;
    switch(o->type) {
        case OBJ_STRING: newobj = dupStringObject(o); break;
        case OBJ_LIST: newobj = listTypeDup(o); break;
        case OBJ_SET: newobj = setTypeDup(o); break;
        case OBJ_ZSET: newobj = zsetDup(o); break;
        case OBJ_HASH: newobj = hashTypeDup(o); break;
        case OBJ_STREAM: newobj = streamDup(o); break;
        case OBJ_MODULE:
            newobj = moduleTypeDupOrReply(c, key, newkey, o);
            if (!newobj) return;
            break;
        default:
            addReplyError(c, "unknown type object");
            return;
    }

    if (delete) {
        dbDelete(dst,newkey);
    }

    dbAdd(dst,newkey,newobj);
    if (expire != -1) setExpire(c, dst, newkey, expire);

    /* OK! key copied */
    signalModifiedKey(c,dst,c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"copy_to",c->argv[2],dst->id);

    server.dirty++;
    addReply(c,shared.cone);
}

/* Helper function for dbSwapDatabases(): scans the list of keys that have
 * one or more blocked clients for B[LR]POP or other blocking commands
 * and signal the keys as ready if they are of the right type. See the comment
 * where the function is used for more info. */
void scanDatabaseForReadyLists(redisDb *db) {
    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(db->blocking_keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        robj *value = lookupKey(db,key,LOOKUP_NOTOUCH);
        if (value) signalKeyAsReady(db, key, value->type);
    }
    dictReleaseIterator(di);
}

/* Swap two databases at runtime so that all clients will magically see
 * the new database even if already connected. Note that the client
 * structure c->db points to a given DB, so we need to be smarter and
 * swap the underlying referenced structures, otherwise we would need
 * to fix all the references to the Redis DB structure.
 *
 * Returns C_ERR if at least one of the DB ids are out of range, otherwise
 * C_OK is returned. */
int dbSwapDatabases(int id1, int id2) {
    if (id1 < 0 || id1 >= server.dbnum ||
        id2 < 0 || id2 >= server.dbnum) return C_ERR;
    if (id1 == id2) return C_OK;
    redisDb aux = server.db[id1];
    redisDb *db1 = &server.db[id1], *db2 = &server.db[id2];

    /* Swap hash tables. Note that we don't swap blocking_keys,
     * ready_keys and watched_keys, since we want clients to
     * remain in the same DB they were. */
    db1->dict = db2->dict;
    db1->expires = db2->expires;
    db1->avg_ttl = db2->avg_ttl;
    db1->expires_cursor = db2->expires_cursor;

    db2->dict = aux.dict;
    db2->expires = aux.expires;
    db2->avg_ttl = aux.avg_ttl;
    db2->expires_cursor = aux.expires_cursor;

    /* Now we need to handle clients blocked on lists: as an effect
     * of swapping the two DBs, a client that was waiting for list
     * X in a given DB, may now actually be unblocked if X happens
     * to exist in the new version of the DB, after the swap.
     *
     * However normally we only do this check for efficiency reasons
     * in dbAdd() when a list is created. So here we need to rescan
     * the list of clients blocked on lists and signal lists as ready
     * if needed.
     *
     * Also the swapdb should make transaction fail if there is any
     * client watching keys */
    scanDatabaseForReadyLists(db1);
    touchAllWatchedKeysInDb(db1, db2);
    scanDatabaseForReadyLists(db2);
    touchAllWatchedKeysInDb(db2, db1);
    return C_OK;
}

/* SWAPDB db1 db2 */
void swapdbCommand(client *c) {
    int id1, id2;

    /* Not allowed in cluster mode: we have just DB 0 there. */
    if (server.cluster_enabled) {
        addReplyError(c,"SWAPDB is not allowed in cluster mode");
        return;
    }

    /* Get the two DBs indexes. */
    if (getIntFromObjectOrReply(c, c->argv[1], &id1,
        "invalid first DB index") != C_OK)
        return;

    if (getIntFromObjectOrReply(c, c->argv[2], &id2,
        "invalid second DB index") != C_OK)
        return;

    /* Swap... */
    if (dbSwapDatabases(id1,id2) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    } else {
        RedisModuleSwapDbInfo si = {REDISMODULE_SWAPDBINFO_VERSION,id1,id2};
        moduleFireServerEvent(REDISMODULE_EVENT_SWAPDB,0,&si);
        server.dirty++;
        addReply(c,shared.ok);
    }
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/
/*
 * 移除键 key 的过期时间
 */
int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    /* 确保键存在 */
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    /* 删除过期时间 */
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}

/* Set an expire to the specified key. If the expire is set in the context
 * of an user calling a command 'c' is the client, otherwise 'c' is set
 * to NULL. The 'when' parameter is the absolute unix time in milliseconds
 * after which the key will no longer be considered valid. */
/* 将键 key 的过期时间设为 when */
void setExpire(client *c, redisDb *db, robj *key, long long when) {
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict */
    /* 从数据库中查找key */
    kde = dictFind(db->dict,key->ptr);
    serverAssertWithInfo(NULL,key,kde != NULL);
    /* 查找或者添加key节点到过期字典 */
    de = dictAddOrFind(db->expires,dictGetKey(kde));
    /* 设置键的过期时间，这里是直接使用整数值来保存过期时间，
     * 不是用 INT 编码的String 对象*/
    dictSetSignedIntegerVal(de,when);

    int writable_slave = server.masterhost && server.repl_slave_ro == 0;
    if (c && writable_slave && !(c->flags & CLIENT_MASTER))
        rememberSlaveKeyWithExpire(db,key);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
long long getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

    /* The entry was found in the expire dict, this means it should also
     * be present in the main dict (safety check). */
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictGetSignedIntegerVal(de);
}

/* Delete the specified expired key and propagate expire. */
void deleteExpiredKeyAndPropagate(redisDb *db, robj *keyobj) {
    mstime_t expire_latency;
    latencyStartMonitor(expire_latency);
    if (server.lazyfree_lazy_expire)
        dbAsyncDelete(db,keyobj);
    else
        dbSyncDelete(db,keyobj);
    latencyEndMonitor(expire_latency);
    latencyAddSampleIfNeeded("expire-del",expire_latency);
    notifyKeyspaceEvent(NOTIFY_EXPIRED,"expired",keyobj,db->id);
    signalModifiedKey(NULL, db, keyobj);
    propagateExpire(db,keyobj,server.lazyfree_lazy_expire);
    server.stat_expiredkeys++;
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
void propagateExpire(redisDb *db, robj *key, int lazy) {
    robj *argv[2];

    argv[0] = lazy ? shared.unlink : shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

    /* If the master decided to expire a key we must propagate it to replicas no matter what..
     * Even if module executed a command without asking for propagation. */
    int prev_replication_allowed = server.replication_allowed;
    server.replication_allowed = 1;
    propagate(server.delCommand,db->id,argv,2,PROPAGATE_AOF|PROPAGATE_REPL);
    server.replication_allowed = prev_replication_allowed;

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/* Check if the key is expired. */
int keyIsExpired(redisDb *db, robj *key) {
    mstime_t when = getExpire(db,key);
    mstime_t now;

    if (when < 0) return 0; /* No expire for this key */

    /* Don't expire anything while loading. It will be done later. */
    if (server.loading) return 0;

    /* If we are in the context of a Lua script, we pretend that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent.
     * See issue #1525 on Github for more information. */
    if (server.lua_caller) {
        now = server.lua_time_snapshot;
    }
    /* If we are in the middle of a command execution, we still want to use
     * a reference time that does not change: in that case we just use the
     * cached time, that we update before each call in the call() function.
     * This way we avoid that commands such as RPOPLPUSH or similar, that
     * may re-open the same key multiple times, can invalidate an already
     * open object in a next call, if the next call will see the key expired,
     * while the first did not. */
    else if (server.fixed_time_expire > 0) {
        now = server.mstime;
    }
    /* For the other cases, we want to use the most fresh time we have. */
    else {
        now = mstime();
    }

    /* The key expired if the current (virtual or real) time is greater
     * than the expire time of the key. */
    return now > when;
}

/* This function is called when we are going to perform some operation
 * in a given key, but such key may be already logically expired even if
 * it still exists in the database. The main way this function is called
 * is via lookupKey*() family of functions.
 *
 * The behavior of the function depends on the replication role of the
 * instance, because slave instances do not expire keys, they wait
 * for DELs from the master for consistency matters. However even
 * slaves will try to have a coherent return value for the function,
 * so that read commands executed in the slave side will be able to
 * behave like if the key is expired even if still present (because the
 * master has yet to propagate the DEL).
 *
 * In masters as a side effect of finding a key which is expired, such
 * key will be evicted from the database. Also this may trigger the
 * propagation of a DEL/UNLINK command in AOF / replication stream.
 *
 * The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. */

/*
 * 检查 key 是否已经过期，如果是的话，将它从数据库中删除。
 *
 * 返回 0 表示键没有过期时间，或者键未过期。
 *
 * 返回 1 表示键已经因为过期而被删除了。
 */

int expireIfNeeded(redisDb *db, robj *key) {
    /* 查询键是否已过期 */
    if (!keyIsExpired(db,key)) return 0;

    /* If we are running in the context of a slave, instead of
     * evicting the expired key from the database, we return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
    /* 当服务器运行在 replication 模式时，附属节点并不主动删除 key
     * 它只返回一个逻辑上正确的返回值，
     * 真正的删除操作要等待主节点发来删除命令时才执行
     * 从而保证数据的同步
     */
    if (server.masterhost != NULL) return 1;

    /* If clients are paused, we keep the current dataset constant,
     * but return to the client what we believe is the right state. Typically,
     * at the end of the pause we will properly expire the key OR we will
     * have failed over and the new primary will send us the expire. */
    if (checkClientPauseTimeoutAndReturnIfPaused()) return 1;

    /* Delete the key */
    /* 将过期键从数据库中删除 */
    deleteExpiredKeyAndPropagate(db,key);
    return 1;
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * ---------------------------------------------------------------------------*/

/* Prepare the getKeysResult struct to hold numkeys, either by using the
 * pre-allocated keysbuf or by allocating a new array on the heap.
 *
 * This function must be called at least once before starting to populate
 * the result, and can be called repeatedly to enlarge the result array.
 */
int *getKeysPrepareResult(getKeysResult *result, int numkeys) {
    /* GETKEYS_RESULT_INIT initializes keys to NULL, point it to the pre-allocated stack
     * buffer here. */
    if (!result->keys) {
        serverAssert(!result->numkeys);
        result->keys = result->keysbuf;
    }

    /* Resize if necessary */
    if (numkeys > result->size) {
        if (result->keys != result->keysbuf) {
            /* We're not using a static buffer, just (re)alloc */
            result->keys = zrealloc(result->keys, numkeys * sizeof(int));
        } else {
            /* We are using a static buffer, copy its contents */
            result->keys = zmalloc(numkeys * sizeof(int));
            if (result->numkeys)
                memcpy(result->keys, result->keysbuf, result->numkeys * sizeof(int));
        }
        result->size = numkeys;
    }

    return result->keys;
}

/* The base case is to use the keys position as given in the command table
 * (firstkey, lastkey, step). */
int getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, getKeysResult *result) {
    int j, i = 0, last, *keys;
    UNUSED(argv);

    if (cmd->firstkey == 0) {
        result->numkeys = 0;
        return 0;
    }

    last = cmd->lastkey;
    if (last < 0) last = argc+last;

    int count = ((last - cmd->firstkey)+1);
    keys = getKeysPrepareResult(result, count);

    for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
        if (j >= argc) {
            /* Modules commands, and standard commands with a not fixed number
             * of arguments (negative arity parameter) do not have dispatch
             * time arity checks, so we need to handle the case where the user
             * passed an invalid number of arguments here. In this case we
             * return no keys and expect the command implementation to report
             * an arity or syntax error. */
            if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
                result->numkeys = 0;
                return 0;
            } else {
                serverPanic("Redis built-in command declared keys positions not matching the arity requirements.");
            }
        }
        keys[i++] = j;
    }
    result->numkeys = i;
    return i;
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is a heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. */
int getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    if (cmd->flags & CMD_MODULE_GETKEYS) {
        return moduleGetCommandKeysViaAPI(cmd,argv,argc,result);
    } else if (!(cmd->flags & CMD_MODULE) && cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd,argv,argc,result);
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,result);
    }
}

/* Free the result of getKeysFromCommand. */
void getKeysFreeResult(getKeysResult *result) {
    if (result && result->keys != result->keysbuf)
        zfree(result->keys);
}

/* Helper function to extract keys from following commands:
 * COMMAND [destkey] <num-keys> <key> [...] <key> [...] ... <options>
 *
 * eg:
 * ZUNION <num-keys> <key> <key> ... <key> <options>
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 *
 * 'storeKeyOfs': destkey index, 0 means destkey not exists.
 * 'keyCountOfs': num-keys index.
 * 'firstKeyOfs': firstkey index.
 * 'keyStep': the interval of each key, usually this value is 1.
 * */
int genericGetKeys(int storeKeyOfs, int keyCountOfs, int firstKeyOfs, int keyStep,
                    robj **argv, int argc, getKeysResult *result) {
    int i, num, *keys;

    num = atoi(argv[keyCountOfs]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. (no input keys). */
    if (num < 1 || num > (argc - firstKeyOfs)/keyStep) {
        result->numkeys = 0;
        return 0;
    }

    int numkeys = storeKeyOfs ? num + 1 : num;
    keys = getKeysPrepareResult(result, numkeys);
    result->numkeys = numkeys;

    /* Add all key positions for argv[firstKeyOfs...n] to keys[] */
    for (i = 0; i < num; i++) keys[i] = firstKeyOfs+(i*keyStep);

    if (storeKeyOfs) keys[num] = storeKeyOfs;
    return result->numkeys;
}

int zunionInterDiffStoreGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(1, 2, 3, 1, argv, argc, result);
}

int zunionInterDiffGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 1, 2, 1, argv, argc, result);
}

int evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 2, 3, 1, argv, argc, result);
}

/* Helper function to extract keys from the SORT command.
 *
 * SORT <sort-key> ... STORE <store-key> ...
 *
 * The first argument of SORT is always a key, however a list of options
 * follow in SQL-alike style. Here we parse just the minimum in order to
 * correctly identify keys in the "STORE" option. */
int sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, j, num, *keys, found_store = 0;
    UNUSED(cmd);

    num = 0;
    keys = getKeysPrepareResult(result, 2); /* Alloc 2 places for the worst case. */
    keys[num++] = 1; /* <sort-key> is always present. */

    /* Search for STORE option. By default we consider options to don't
     * have arguments, so if we find an unknown option name we scan the
     * next. However there are options with 1 or 2 arguments, so we
     * provide a list here in order to skip the right number of args. */
    struct {
        char *name;
        int skip;
    } skiplist[] = {
        {"limit", 2},
        {"get", 1},
        {"by", 1},
        {NULL, 0} /* End of elements. */
    };

    for (i = 2; i < argc; i++) {
        for (j = 0; skiplist[j].name != NULL; j++) {
            if (!strcasecmp(argv[i]->ptr,skiplist[j].name)) {
                i += skiplist[j].skip;
                break;
            } else if (!strcasecmp(argv[i]->ptr,"store") && i+1 < argc) {
                /* Note: we don't increment "num" here and continue the loop
                 * to be sure to process the *last* "STORE" option if multiple
                 * ones are provided. This is same behavior as SORT. */
                found_store = 1;
                keys[num] = i+1; /* <store-key> */
                break;
            }
        }
    }
    result->numkeys = num + found_store;
    return result->numkeys;
}

int migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num, first, *keys;
    UNUSED(cmd);

    /* Assume the obvious form. */
    first = 3;
    num = 1;

    /* But check for the extended one with the KEYS option. */
    if (argc > 6) {
        for (i = 6; i < argc; i++) {
            if (!strcasecmp(argv[i]->ptr,"keys") &&
                sdslen(argv[3]->ptr) == 0)
            {
                first = i+1;
                num = argc-first;
                break;
            }
        }
    }

    keys = getKeysPrepareResult(result, num);
    for (i = 0; i < num; i++) keys[i] = first+i;
    result->numkeys = num;
    return num;
}

/* Helper function to extract keys from following commands:
 * GEORADIUS key x y radius unit [WITHDIST] [WITHHASH] [WITHCOORD] [ASC|DESC]
 *                             [COUNT count] [STORE key] [STOREDIST key]
 * GEORADIUSBYMEMBER key member radius unit ... options ... */
int georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num, *keys;
    UNUSED(cmd);

    /* Check for the presence of the stored key in the command */
    int stored_key = -1;
    for (i = 5; i < argc; i++) {
        char *arg = argv[i]->ptr;
        /* For the case when user specifies both "store" and "storedist" options, the
         * second key specified would override the first key. This behavior is kept
         * the same as in georadiusCommand method.
         */
        if ((!strcasecmp(arg, "store") || !strcasecmp(arg, "storedist")) && ((i+1) < argc)) {
            stored_key = i+1;
            i++;
        }
    }
    num = 1 + (stored_key == -1 ? 0 : 1);

    /* Keys in the command come from two places:
     * argv[1] = key,
     * argv[5...n] = stored key if present
     */
    keys = getKeysPrepareResult(result, num);

    /* Add all key positions to keys[] */
    keys[0] = 1;
    if(num > 1) {
         keys[1] = stored_key;
    }
    result->numkeys = num;
    return num;
}

/* LCS ... [KEYS <key1> <key2>] ... */
int lcsGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i;
    int *keys = getKeysPrepareResult(result, 2);
    UNUSED(cmd);

    /* We need to parse the options of the command in order to check for the
     * "KEYS" argument before the "STRINGS" argument. */
    for (i = 1; i < argc; i++) {
        char *arg = argv[i]->ptr;
        int moreargs = (argc-1) - i;

        if (!strcasecmp(arg, "strings")) {
            break;
        } else if (!strcasecmp(arg, "keys") && moreargs >= 2) {
            keys[0] = i+1;
            keys[1] = i+2;
            result->numkeys = 2;
            return result->numkeys;
        }
    }
    result->numkeys = 0;
    return result->numkeys;
}

/* Helper function to extract keys from memory command.
 * MEMORY USAGE <key> */
int memoryGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);

    getKeysPrepareResult(result, 1);
    if (argc >= 3 && !strcasecmp(argv[1]->ptr,"usage")) {
        result->keys[0] = 2;
        result->numkeys = 1;
        return result->numkeys;
    }
    result->numkeys = 0;
    return 0;
}

/* XREAD [BLOCK <milliseconds>] [COUNT <count>] [GROUP <groupname> <ttl>]
 *       STREAMS key_1 key_2 ... key_N ID_1 ID_2 ... ID_N */
int xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num = 0, *keys;
    UNUSED(cmd);

    /* We need to parse the options of the command in order to seek the first
     * "STREAMS" string which is actually the option. This is needed because
     * "STREAMS" could also be the name of the consumer group and even the
     * name of the stream key. */
    int streams_pos = -1;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i]->ptr;
        if (!strcasecmp(arg, "block")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "count")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "group")) {
            i += 2; /* Skip option argument. */
        } else if (!strcasecmp(arg, "noack")) {
            /* Nothing to do. */
        } else if (!strcasecmp(arg, "streams")) {
            streams_pos = i;
            break;
        } else {
            break; /* Syntax error. */
        }
    }
    if (streams_pos != -1) num = argc - streams_pos - 1;

    /* Syntax error. */
    if (streams_pos == -1 || num == 0 || num % 2 != 0) {
        result->numkeys = 0;
        return 0;
    }
    num /= 2; /* We have half the keys as there are arguments because
                 there are also the IDs, one per key. */

    keys = getKeysPrepareResult(result, num);
    for (i = streams_pos+1; i < argc-num; i++) keys[i-streams_pos-1] = i;
    result->numkeys = num;
    return num;
}

/* Slot to Key API. This is used by Redis Cluster in order to obtain in
 * a fast way a key that belongs to a specified hash slot. This is useful
 * while rehashing the cluster and in other conditions when we need to
 * understand if we have keys for a given hash slot. */
void slotToKeyUpdateKey(sds key, int add) {
    size_t keylen = sdslen(key);
    unsigned int hashslot = keyHashSlot(key,keylen);
    unsigned char buf[64];
    unsigned char *indexed = buf;

    server.cluster->slots_keys_count[hashslot] += add ? 1 : -1;
    if (keylen+2 > 64) indexed = zmalloc(keylen+2);
    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    memcpy(indexed+2,key,keylen);
    if (add) {
        raxInsert(server.cluster->slots_to_keys,indexed,keylen+2,NULL,NULL);
    } else {
        raxRemove(server.cluster->slots_to_keys,indexed,keylen+2,NULL);
    }
    if (indexed != buf) zfree(indexed);
}

void slotToKeyAdd(sds key) {
    slotToKeyUpdateKey(key,1);
}

void slotToKeyDel(sds key) {
    slotToKeyUpdateKey(key,0);
}

/* Release the radix tree mapping Redis Cluster keys to slots. If 'async'
 * is true, we release it asynchronously. */
void freeSlotsToKeysMap(rax *rt, int async) {
    if (async) {
        freeSlotsToKeysMapAsync(rt);
    } else {
        raxFree(rt);
    }
}

/* Empty the slots-keys map of Redis CLuster by creating a new empty one and
 * freeing the old one. */
void slotToKeyFlush(int async) {
    rax *old = server.cluster->slots_to_keys;

    server.cluster->slots_to_keys = raxNew();
    memset(server.cluster->slots_keys_count,0,
           sizeof(server.cluster->slots_keys_count));
    freeSlotsToKeysMap(old, async);
}

/* Populate the specified array of objects with keys in the specified slot.
 * New objects are returned to represent keys, it's up to the caller to
 * decrement the reference count to release the keys names. */
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,server.cluster->slots_to_keys);
    raxSeek(&iter,">=",indexed,2);
    while(count-- && raxNext(&iter)) {
        if (iter.key[0] != indexed[0] || iter.key[1] != indexed[1]) break;
        keys[j++] = createStringObject((char*)iter.key+2,iter.key_len-2);
    }
    raxStop(&iter);
    return j;
}

/* Remove all the keys in the specified hash slot.
 * The number of removed items is returned. */
unsigned int delKeysInSlot(unsigned int hashslot) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,server.cluster->slots_to_keys);
    while(server.cluster->slots_keys_count[hashslot]) {
        raxSeek(&iter,">=",indexed,2);
        raxNext(&iter);

        robj *key = createStringObject((char*)iter.key+2,iter.key_len-2);
        dbDelete(&server.db[0],key);
        decrRefCount(key);
        j++;
    }
    raxStop(&iter);
    return j;
}

unsigned int countKeysInSlot(unsigned int hashslot) {
    return server.cluster->slots_keys_count[hashslot];
}
