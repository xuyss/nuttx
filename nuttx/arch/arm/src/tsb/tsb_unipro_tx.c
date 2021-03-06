/*
 * Copyright (c) 2015 Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <pthread.h>
#include <string.h>

#include <nuttx/util.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/list.h>
#include <nuttx/unipro/unipro.h>

#include "debug.h"
#include "up_arch.h"
#include "tsb_unipro.h"

struct worker {
    pthread_t thread;
    sem_t tx_fifo_lock;
};

static struct worker worker;

struct unipro_buffer {
    struct list_head list;
    unipro_send_completion_t callback;
    void *priv;
    bool som;
    int byte_sent;
    int len;
    const void *data;
};

static int unipro_send_sync(unsigned int cportid,
                            const void *buf, size_t len, bool som);

/**
 * @brief           Set EOM (End Of Message) flag
 * @param[in]       cport: CPort handle
 */
static inline void unipro_set_eom_flag(struct cport *cport)
{
    putreg8(1, CPORT_EOM_BIT(cport));
}

static void unipro_dequeue_tx_buffer(struct unipro_buffer *buffer, int status)
{
    irqstate_t flags;

    DEBUGASSERT(buffer);

    flags = irqsave();
    list_del(&buffer->list);
    irqrestore(flags);

    if (buffer->callback) {
        buffer->callback(status, buffer->data, buffer->priv);
    }

    free(buffer);
}

static void unipro_flush_cport(struct cport *cport)
{
    struct unipro_buffer *buffer;

    if (list_is_empty(&cport->tx_fifo)) {
        goto reset;
    }

    buffer = list_entry(cport->tx_fifo.next, struct unipro_buffer, list);

    if (buffer->som) {
        unipro_set_eom_flag(cport);
    }

    while (!list_is_empty(&cport->tx_fifo)) {
        buffer = list_entry(cport->tx_fifo.next, struct unipro_buffer, list);
        unipro_dequeue_tx_buffer(buffer, -ECONNRESET);
    }

reset:
    _unipro_reset_cport(cport->cportid);
    cport->pending_reset = false;
    if (cport->reset_completion_cb) {
        cport->reset_completion_cb(cport->cportid,
                                   cport->reset_completion_cb_priv);
    }
    cport->reset_completion_cb = cport->reset_completion_cb_priv = NULL;
}


/**
 * @brief           send data over given UniPro CPort
 * @return          0 on success, -EINVAL on invalid parameter,
 *                  -EBUSY when buffer could not be completely transferred
 *                  (unipro_send_tx_buffer() shall be called again until
 *                  buffer is entirely sent (return value == 0)).
 * @param[in]       operation: greybus loopback operation
 */
static int unipro_send_tx_buffer(struct cport *cport)
{
    irqstate_t flags;
    struct unipro_buffer *buffer;
    int retval;

    if (!cport) {
        return -EINVAL;
    }

    flags = irqsave();

    if (list_is_empty(&cport->tx_fifo)) {
        if (cport->pending_reset) {
            unipro_flush_cport(cport);
        }
        irqrestore(flags);
        return 0;
    }

    buffer = list_entry(cport->tx_fifo.next, struct unipro_buffer, list);

    irqrestore(flags);

    if (cport->pending_reset) {
        unipro_flush_cport(cport);
    }

    retval = unipro_send_sync(cport->cportid,
                              buffer->data + buffer->byte_sent,
                              buffer->len - buffer->byte_sent, buffer->som);
    if (retval < 0) {
        unipro_dequeue_tx_buffer(buffer, retval);
        lldbg("unipro_send_sync failed. Dropping message...\n");
        return -EINVAL;
    }

    /* Only update state if the CPort TX FIFO was able to accept any data */
    if (retval > 0) {
        buffer->som = false;
        buffer->byte_sent += retval;
    }

    if (buffer->byte_sent >= buffer->len) {
        unipro_set_eom_flag(cport);
        unipro_dequeue_tx_buffer(buffer, 0);
        return 0;
    }

    return -EBUSY;
}

/**
 * @brief           Send data buffer(s) on CPort whenever ready.
 *                  Ensure that TX queues are reinspected until
 *                  all CPorts have no work available.
 *                  Then suspend again until new data is available.
 */
static void *unipro_tx_worker(void *data)
{
    int i;
    bool is_busy;
    int retval;
    unsigned int cport_count = unipro_cport_count();

    while (1) {
        /* Block until a buffer is pending on any CPort */
        sem_wait(&worker.tx_fifo_lock);

        do {
            is_busy = false;

            for (i = 0; i < cport_count; i++) {
                /* Browse all CPorts sending any pending buffers */
                retval = unipro_send_tx_buffer(cport_handle(i));
                if (retval == -EBUSY) {
                    /*
                     * Buffer only partially sent, have to try again for
                     * remaining part.
                     */
                    is_busy = true;
                }
            }
        } while (is_busy); /* exit when CPort(s) current pending buffer sent */
    }

    return NULL;
}

static void unipro_reset_notify_memcpy(unsigned int cportid)
{
    /*
     * if the tx worker is blocked on the semaphore, post something on it
     * in order to unlock it and have the reset happen right away.
     */
    sem_post(&worker.tx_fifo_lock);
}

/**
 * @brief           send data over UniPro asynchronously (not blocking)
 * @return          0 on success, <0 otherwise
 * @param[in]       cportid: target CPort ID
 * @param[in]       buf: data buffer
 * @param[in]       len: data buffer length (in bytes)
 * @param[in]       callback: function called upon Tx completion
 * @param[in]       priv: optional argument passed to callback
 */
static int unipro_send_async_memcpy(unsigned int cportid, const void *buf, size_t len,
                                    unipro_send_completion_t callback, void *priv)
{
    struct cport *cport;
    struct unipro_buffer *buffer;
    irqstate_t flags;

    if (len > CPORT_BUF_SIZE) {
        return -EINVAL;
    }

    cport = cport_handle(cportid);
    if (!cport) {
        return -EINVAL;
    }

    if (cport->pending_reset) {
        return -EPIPE;
    }

    if (!cport_is_connected(cportid)) {
        lldbg("CP%u unconnected\n", cport->cportid);
        return -EPIPE;
    }

    DEBUGASSERT(TRANSFER_MODE == 2);

    buffer = zalloc(sizeof(*buffer));
    if (!buffer) {
        return -ENOMEM;
    }
    list_init(&buffer->list);
    buffer->som = true;
    buffer->len = len;
    buffer->callback = callback;
    buffer->priv = priv;
    buffer->data = buf;

    flags = irqsave();
    list_add(&cport->tx_fifo, &buffer->list);
    irqrestore(flags);

    sem_post(&worker.tx_fifo_lock);
    return 0;
}

struct unipro_send_data {
    sem_t sem;
    int status;
};

static int unipro_send_memcpy_cb(int status, const void *buf, void *priv)
{
    struct unipro_send_data *data = priv;

    data->status = status;
    sem_post(&data->sem);

    return 0;
}

/**
 * @brief send data down a CPort
 * @param cportid cport to send down
 * @param buf data buffer
 * @param len size of data to send
 * @param 0 on success, <0 on error
 */
static int unipro_send_memcpy(unsigned int cportid, const void *buf, size_t len)
{
    int retval;
    struct unipro_send_data data;

    sem_init(&data.sem, 0, 0);

    retval = unipro_send_async_memcpy(cportid, buf, len, unipro_send_memcpy_cb,
                                      &data);
    if (retval) {
        goto out;
    }

    sem_wait(&data.sem);
    retval = data.status;

out:
    sem_destroy(&data.sem);
    return retval;
}

/**
 * @brief           Send data down to a CPort
 * @return          number of bytes effectively sent (>= 0), or error code (< 0)
 * @param[in]       cportid: cport to send down
 * @param[in]       buf: data buffer
 * @param[in]       len: size of data to send
 * @param[in]       som: "start of message" flag
 */
static int unipro_send_sync(unsigned int cportid,
                            const void *buf, size_t len, bool som)
{
    struct cport *cport;
    uint16_t count;
    uint8_t *tx_buf;

    if (len > CPORT_BUF_SIZE) {
        return -EINVAL;
    }

    cport = cport_handle(cportid);
    if (!cport) {
        return -EINVAL;
    }

    if (!cport_is_connected(cportid)) {
        lldbg("CP%d unconnected\n", cport->cportid);
        return -EPIPE;
    }

    DEBUGASSERT(TRANSFER_MODE == 2);

    /*
     * If this is not the start of a new message,
     * message data must be written to first address of CPort Tx Buffer + 1.
     */
    if (!som) {
        tx_buf = cport->tx_buf + sizeof(uint32_t);
    } else {
        tx_buf = cport->tx_buf;
    }

    count = unipro_get_tx_free_buffer_space(cport);
    if (!count) {
        /* No free space in TX FIFO, cannot send anything. */
        DBG_UNIPRO("No free space in CP%d Tx Buffer\n", cportid);
        return 0;
    } else if (count > len) {
        count = len;
    }
    /* Copy message data in CPort Tx FIFO */
    DBG_UNIPRO("Sending %u bytes to CP%d\n", count, cportid);
    memcpy(tx_buf, buf, count);

    return (int) count;
}

static struct unipro_tx_calltable calltable = {
    unipro_reset_notify_memcpy,
    unipro_send_memcpy,
    unipro_send_async_memcpy
};

int unipro_tx_init_memcpy(struct unipro_tx_calltable **table)
{
    int retval;

    sem_init(&worker.tx_fifo_lock, 0, 0);

    retval = pthread_create(&worker.thread, NULL, unipro_tx_worker, NULL);
    if (retval) {
        lldbg("Failed to create worker thread: %s.\n", strerror(errno));
        return retval;
    }

    *table = &calltable;

    return 0;
}
