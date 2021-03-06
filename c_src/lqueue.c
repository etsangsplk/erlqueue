/* -------------------------------------------------------------------
%%
%% Copyright (c) 2016 Luis Rascão.  All Rights Reserved.
%%
%% This file is provided to you under the Apache License,
%% Version 2.0 (the "License"); you may not use this file
%% except in compliance with the License.  You may obtain
%% a copy of the License at
%%
%%   http://www.apache.org/licenses/LICENSE-2.0
%%
%% Unless required by applicable law or agreed to in writing,
%% software distributed under the License is distributed on an
%% "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
%% KIND, either express or implied.  See the License for the
%% specific language governing permissions and limitations
%% under the License.
%%
%% ------------------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "lstats.h"
#include "lqueue.h"

#define SHMEM_PREFIX "/tmp/lqueue.shm."

lqueue_t *
lqueue_create(char *name, size_t size)
{
    char filename[256];
    strcpy(filename, SHMEM_PREFIX);
    strcat(filename, name);
    FILE *fp = fopen(filename, "ab+");
    fclose(fp);
    // add the lqueue struct overhead to the requested size
    // also add up a header's length, look for the next comment
    // below for the reason why
    int shmid = shmget(ftok(filename, 1),
                       size + sizeof(lqueue_t) + sizeof(header_t), IPC_CREAT | 0666);
    if (shmid == -1)
        return NULL;
    lqueue_t *q = shmat(shmid, NULL, 0);
    if (q == (void *) -1)
        return NULL;

    q->head = ATOMIC_VAR_INIT(0);
    q->tail = ATOMIC_VAR_INIT(0);
    // add a header length to the requested size so account for the empty
    // header that must be introduced at the end of the buffer
    // to signal dequeue that it must circle back to the beginning
    q->size = size + sizeof(header_t);
    strcpy(q->name, name);
#ifdef LSTATS
    lstats_init(&q->stats);
#endif
    return q;
}

lqueue_t *
lqueue_connect(char *name)
{
    char filename[256];
    strcpy(filename, SHMEM_PREFIX);
    strcat(filename, name);
    int shmid = shmget(ftok(filename, 1), 0, 0);
    if (shmid == -1)
        return NULL;
    lqueue_t *q = shmat(shmid, NULL, 0);
    if (q == (void *) -1)
        return NULL;
    strcpy(q->name, name);
    return q;
}

void
lqueue_free(lqueue_t *q)
{
    char filename[256];
    strcpy(filename, SHMEM_PREFIX);
    strcat(filename, q->name);
    int shmid = shmget(ftok(filename, 1), 0, 0);

    shmdt(q);
    shmctl(shmid, IPC_RMID, NULL);
}

lqueue_status_t
lqueue_queue(lqueue_t *q, void *v, size_t size)
{
    unsigned int tail = atomic_load(&q->tail);
    unsigned int next_tail;
    unsigned short wraparound = 0;

    STAT_TIME();

    STAT_SCORE(STAT_QUEUE_TRY, &q->stats);
    next_tail = tail + sizeof(header_t) + size;
    // if this write plus an extra header would exceed the buffer limits
    // then reset and start from the top
    // this gives the assurance that we always have enough
    // to write a special end of queue header
    if ((next_tail + sizeof(header_t)) > q->size) {
        next_tail = 0;
        wraparound = 1;
    }

    // perform the atomic CAS operation, if we don't get the expected value
    // return the status up where a retry will then be asked for.
    if (!atomic_compare_exchange_weak(&q->tail, &tail, next_tail))
        return LQUEUE_CAS;

    if (wraparound) {
        STAT_SCORE(STAT_OVERFLOW, &q->stats);
        header_t *header = (header_t *) (q->buffer + tail);
        // we have the assurance that there's always room for an header
        // so insert a special one with a size of the total queue size
        // dequeue will see this and know that it must circle back
        // to the beginning
        atomic_store(&header->size, q->size);
        atomic_store(&header->marker, SET_UNREAD(VALID_MASK(tail)));
        // but still we have to deal with this queue request
        // so just try again
        return lqueue_queue(q, v, size);
    } else {
        // first we read the header, if it's a valid unread one
        // then we know we've reached the top of the queue
        // and we can't write anything more without rewriting
        // unconsumed data
        header_t *header = (header_t *) (q->buffer + tail);
        marker_t marker = atomic_load(&header->marker);
        if (IS_VALID(marker, tail) && IS_UNREAD(marker)) {
            // restore the previous tail
            atomic_store(&q->tail, tail);
            return 1;
        }
        // copy the value onto the queue
        memcpy(q->buffer + tail + sizeof(header_t), v, size);
        // now set the header size and marker
        atomic_store(&header->size, size);
        // the marker must be stored atomically to make sure
        // a concurrent dequeue won't get us mid-copy
        atomic_store(&header->marker, SET_UNREAD(VALID_MASK(tail)));
    }

    STAT_VALUE_SCORE(STAT_MAX_QUEUE_TIME_MICROS, STAT_TIME_DIFF(), &q->stats);
    STAT_VALUE_SCORE(STAT_QUEUE_TIME_MICROS, STAT_TIME_DIFF(), &q->stats);
    STAT_SCORE(STAT_QUEUE, &q->stats);
    return LQUEUE_OK;
}

lqueue_status_t
lqueue_dequeue(lqueue_t *q, void **v, size_t *size)
{
    unsigned int head = atomic_load(&q->head);
    unsigned int tail = atomic_load(&q->tail);
    unsigned int next_head = 0;
    unsigned short wraparound = 0;
    header_t *header = NULL;
    marker_t marker = 0;
    unsigned int header_size;

    STAT_TIME();
    STAT_SCORE(STAT_DEQUEUE_TRY, &q->stats);

    header = (header_t *) (q->buffer + head);
    // load the marker atomically for the same
    // we store it atomically in queue
    marker = atomic_load(&header->marker);
    header_size = atomic_load(&header->size);
    // we're up against the end of the queue and there's
    // nothing more to read
    if (head == tail) {
        // this is an invalid block or valid one that's already been read
        if (!IS_VALID(marker, head) || IS_READ(marker))
            return LQUEUE_EMPTY;
    }
    // only try to read blocks that are valid and unread
    if (!IS_VALID(marker, head) || IS_READ(marker))
        return LQUEUE_EMPTY;

    next_head = head + sizeof(header_t) + header_size;
    if (next_head > q->size) {
        next_head = 0;
        wraparound = 1;
    }

    // perform the atomic CAS operation, if we don't get the expected value
    // return the status up where a retry will then be asked for.
    if (!atomic_compare_exchange_weak(&q->head, &head, next_head))
        return LQUEUE_CAS;

    if (wraparound) {
        STAT_SCORE(STAT_OVERFLOW, &q->stats);
        // we've reached the end of the queue
        // so just mark this block as read and try again
        atomic_store(&header->marker, SET_READ(VALID_MASK(head)));
        return LQUEUE_CAS;
    }
    else {
        // extract the queued value
        *size = header_size;
        *v = q->buffer + head + sizeof(header_t);
        // reset the header
        atomic_store(&header->size, 0);
        atomic_store(&header->marker, 0);
    }

    STAT_VALUE_SCORE(STAT_MAX_DEQUEUE_TIME_MICROS, STAT_TIME_DIFF(), &q->stats);
    STAT_VALUE_SCORE(STAT_DEQUEUE_TIME_MICROS, STAT_TIME_DIFF(), &q->stats);
    STAT_SCORE(STAT_DEQUEUE, &q->stats);
    return LQUEUE_OK;
}

size_t
lqueue_byte_size(size_t size)
{
    // the actual space that will be taken up on the queue is
    return sizeof(header_t) + size;
}

lstats_t *
lqueue_stats(lqueue_t *q)
{
#ifdef LSTATS
    return &q->stats;
#endif
    return NULL;
}

void
lqueue_inspect(lqueue_t *q, unsigned int position, marker_t *marker)
{
    header_t *header = (header_t *) (q->buffer + position);
    *marker = atomic_load(&header->marker);
}

void
lqueue_release(void *v, size_t size)
{
    // scrub the data that we just read
    // this is an important bit since it's thie srubbing
    // that will prevent a future queue to end up somewhere
    // along this buffer and mistakenly interpret random bytes
    // as a valid header
    memset(v, 0, size);
}
