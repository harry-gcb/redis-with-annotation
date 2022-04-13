#ifndef STREAM_H
#define STREAM_H

#include "rax.h"
#include "listpack.h"

/* Stream item ID: a 128 bit number composed of a milliseconds time and
 * a sequence counter. IDs generated in the same millisecond (or in a past
 * millisecond if the clock jumped backward) will use the millisecond time
 * of the latest generated ID and an incremented sequence. */
/* 消息id，创建时间+序列号*/
typedef struct streamID {
    uint64_t ms;        /* Unix time in milliseconds. */
    uint64_t seq;       /* Sequence number. */
} streamID;

typedef struct stream {
    rax *rax;               /* rax存储消息生产者生产的具体消息，每个消息有唯一的ID 
                             * 以消息ID为键，消息内容为值存储在rax中 
                             * rax中的一个节点可能存储多个消息 The radix tree holding the stream. */
    uint64_t length;        /* 当前stream中的消息个数（不包括已经删除的消息 Number of elements inside this stream. */
    streamID last_id;       /* 当前stream中最后插入的消息的ID, stream为空时，设置为0 Zero if there are yet no items. */
    rax *cgroups;           /* 存储了当前stream相关的消费组，以消费组的组名为键，streamCG为值存储在rax中 Consumer groups dictionary: name -> streamCG */
} stream;

/* We define an iterator to iterate stream items in an abstract way, without
 * caring about the radix tree + listpack representation. Technically speaking
 * the iterator is only used inside streamReplyWithRange(), so could just
 * be implemented inside the function, but practically there is the AOF
 * rewriting code that also needs to iterate the stream to emit the XADD
 * commands. */
/* 迭代器。为了遍历stream中的消息 */
typedef struct streamIterator {
    stream *stream;         /* 当前迭代器正在遍历的消息流 The stream we are iterating. */
    streamID master_id;     /* 消息内容实际存储在listpack中，每个listpack都有一个master entry（也就是第一个插入的消息）, master_id为该消息id ID of the master entry at listpack head. */
    uint64_t master_fields_count;       /* master entry中field域的个数 Master entries # of fields. */
    unsigned char *master_fields_start; /* master entry field域存储的首地址 Master entries start in listpack. */
    unsigned char *master_fields_ptr;   /* 当listpack中消息的field域与master entry的field域完全相同时，该消息会复用master entry的field域，
                                         * 在我们遍历该消息时，需要记录当前所在的field域的具体位置，
                                         * master_fields_ptr就是实现这个功能的 Master field to emit next. */
    int entry_flags;                    /* 当前遍历的消息的标志位 Flags of entry we are emitting. */
    int rev;                /* 当前迭代器的方向 True if iterating end to start (reverse). */
    /* tart_key, end_key为该迭代器处理的消息ID的范围 */
    uint64_t start_key[2];  /* Start key as 128 bit big endian. */
    uint64_t end_key[2];    /* End key as 128 bit big endian. */
    raxIterator ri;         /* rax迭代器，用于遍历rax中所有的key Rax iterator. */
    unsigned char *lp;      /* 当前listpack指针 Current listpack. */
    unsigned char *lp_ele;  /* 当前正在遍历的listpack中的元素 Current listpack cursor. */
    unsigned char *lp_flags; /* 当前消息的flag域 Current entry flags pointer. */
    /* Buffers used to hold the string of lpGet() when the element is
     * integer encoded, so that there is no string representation of the
     * element inside the listpack itself. */
    /* 用于从listpack读取数据时的缓存 */
    unsigned char field_buf[LP_INTBUF_SIZE];
    unsigned char value_buf[LP_INTBUF_SIZE];
} streamIterator;

/* Consumer group. */
/* 消费组是Stream中的一个重要概念，每个Stream会有多个消费组，
 * 每个消费组通过组名称进行唯一标识，同时关联一个streamCG结构 
 */
typedef struct streamCG {
    streamID last_id;       /* 该消费组已经确认的最后一个消息的ID 
                               Last delivered (not acknowledged) ID for this
                               group. Consumers that will just ask for more
                               messages will served with IDs > than this. */
    rax *pel;               /* 该消费组尚未确认的消息，并以消息ID为键，streamNACK（代表一个尚未确认的消息）为值
                               Pending entries list. This is a radix tree that
                               has every message delivered to consumers (without
                               the NOACK option) that was yet not acknowledged
                               as processed. The key of the radix tree is the
                               ID as a 64 bit big endian number, while the
                               associated value is a streamNACK structure.*/
    rax *consumers;         /* onsumers为该消费组中所有的消费者，并以消费者的名称为键，streamConsumer（代表一个消费者）为值
                               A radix tree representing the consumers by name
                               and their associated representation in the form
                               of streamConsumer structures. */
} streamCG;

/* A specific consumer in a consumer group.  */
/* 消费者。每个消费者通过streamConsumer唯一标识 */
typedef struct streamConsumer {
    mstime_t seen_time;         /* 该消费者最后一次活跃的时间 Last time this consumer was active. */
    sds name;                   /* name为消费者的名称 Consumer name. This is how the consumer
                                   will be identified in the consumer group
                                   protocol. Case sensitive. */
    rax *pel;                   /* pel为该消费者尚未确认的消息，以消息ID为键，streamNACK为值 
                                   Consumer specific pending entries list: all
                                   the pending messages delivered to this
                                   consumer not yet acknowledged. Keys are
                                   big endian message IDs, while values are
                                   the same streamNACK structure referenced
                                   in the "pel" of the consumer group structure
                                   itself, so the value is shared. */
} streamConsumer;

/* Pending (yet not acknowledged) message in a consumer group. */
/* 未确认消息。未确认消息（streamNACK）维护了消费组或者消费者尚未确认的消息，
 * 值得注意的是，消费组中的pel的元素与每个消费者的pel中的元素是共享的，
 * 即该消费组消费了某个消息，这个消息会同时放到消费组以及该消费者的pel队列中，
 * 并且二者是同一个streamNACK结构
 */
typedef struct streamNACK {
    mstime_t delivery_time;     /* 该消息最后发送给消费方的时间 Last time this message was delivered. */
    uint64_t delivery_count;    /* 该消息已经发送的次数（组内的成员可以通过xclaim命令获取某个消息的处理权，该消息已经分给组内另一个消费者但其并没有确认该消息） Number of times this message was delivered.*/
    streamConsumer *consumer;   /* 该消息当前归属的消费者 The consumer this message was delivered to
                                   in the last delivery. */
} streamNACK;

/* Stream propagation informations, passed to functions in order to propagate
 * XCLAIM commands to AOF and slaves. */
typedef struct streamPropInfo {
    robj *keyname;
    robj *groupname;
} streamPropInfo;

/* Prototypes of exported APIs. */
struct client;

/* Flags for streamLookupConsumer */
#define SLC_NONE      0
#define SLC_NOCREAT   (1<<0) /* Do not create the consumer if it doesn't exist */
#define SLC_NOREFRESH (1<<1) /* Do not update consumer's seen-time */
/* 实现stream的初始化 */
stream *streamNew(void);
/* 释放给定的stream */
void freeStream(stream *s);
unsigned long streamLength(const robj *subject);
size_t streamReplyWithRange(client *c, stream *s, streamID *start, streamID *end, size_t count, int rev, streamCG *group, streamConsumer *consumer, int flags, streamPropInfo *spi);
void streamIteratorStart(streamIterator *si, stream *s, streamID *start, streamID *end, int rev);
int streamIteratorGetID(streamIterator *si, streamID *id, int64_t *numfields);
void streamIteratorGetField(streamIterator *si, unsigned char **fieldptr, unsigned char **valueptr, int64_t *fieldlen, int64_t *valuelen);
void streamIteratorRemoveEntry(streamIterator *si, streamID *current);
void streamIteratorStop(streamIterator *si);
streamCG *streamLookupCG(stream *s, sds groupname);
streamConsumer *streamLookupConsumer(streamCG *cg, sds name, int flags, int *created);
streamCG *streamCreateCG(stream *s, char *name, size_t namelen, streamID *id);
streamNACK *streamCreateNACK(streamConsumer *consumer);
void streamDecodeID(void *buf, streamID *id);
int streamCompareID(streamID *a, streamID *b);
void streamFreeNACK(streamNACK *na);
int streamIncrID(streamID *id);
int streamDecrID(streamID *id);
void streamPropagateConsumerCreation(client *c, robj *key, robj *groupname, sds consumername);
robj *streamDup(robj *o);
int streamValidateListpackIntegrity(unsigned char *lp, size_t size, int deep);
int streamParseID(const robj *o, streamID *id);
robj *createObjectFromStreamID(streamID *id);
/* 向stream中添加一个新的消息 */
int streamAppendItem(stream *s, robj **argv, int64_t numfields, streamID *added_id, streamID *use_id);
int streamDeleteItem(stream *s, streamID *id);
int64_t streamTrimByLength(stream *s, long long maxlen, int approx);
int64_t streamTrimByID(stream *s, streamID minid, int approx);

#endif
