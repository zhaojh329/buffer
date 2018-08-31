/*
 * Copyright (C) 2018 Jianhui Zhao <jianhuizhao329@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>

#include "buffer.h"

static bool buffer_chain_should_align(struct buffer_chain *chain, int len)
{
	int maxlen;
	int offset;

	/* nothing to squeeze */
	if (chain->data == chain->head)
		return false;

	maxlen = chain->end - chain->head;
	offset = chain->data - chain->head;

	/* less than half is available */
	if (offset > maxlen / 2)
		return true;

	/* less than 32 bytes data but takes more than 1/4 space */
	if (chain->tail - chain->data < 32 && offset > maxlen / 4)
		return true;

	/* no need to move if len is available at the tail */
	return (chain->end - chain->tail < len);
}

static void buffer_chain_align(struct buffer_chain *chain)
{
	int datlen = chain->tail - chain->data;

	memmove(chain->head, chain->data, datlen);
	chain->data = chain->head;
	chain->tail = chain->data + datlen;
}

static struct buffer_chain *buffer_chain_new(size_t len)
{
    struct buffer_chain *chain;
    size_t buf_len = 512;   /* min buf size */

    while (buf_len < len)
        buf_len <<= 1;

    chain = malloc(buf_len + sizeof(struct buffer_chain));
    if (!chain)
        return NULL;

    chain->data = chain->tail = chain->head;
    chain->end = chain->head + buf_len;

    return chain;
}

static void buffer_add_chain(struct buffer *b, struct buffer_chain *chain)
{
	if (!b->tail)
		b->head = chain;
	else
		b->tail->next = chain;

	chain->next = NULL;
	b->tail = chain;
}

static void buffer_del_chain(struct buffer *b, struct buffer_chain *chain)
{
    if (chain == b->head)
        b->head = chain->next;
    
    if (chain == b->tail)
        b->tail = NULL;
    
    free(chain);
}

static struct buffer_chain *buffer_expand(struct buffer *b, int len)
{
    struct buffer_chain *chain = buffer_chain_new(len);

    if (!chain)
        return NULL;

    buffer_add_chain(b, chain);
    return chain;
}

void buffer_free(struct buffer *b)
{
	struct buffer_chain *chain = b->head;

	while (chain) {
		struct buffer_chain *next = chain->next;

		free(chain);
		chain = next;
	}

	b->head = NULL;
	b->tail = NULL;
}

int buffer_add(struct buffer *b, const void *source, size_t len)
{
    struct buffer_chain *chain = b->tail;
    int remain;

    if (chain) {
        if (buffer_chain_should_align(chain, len))
			buffer_chain_align(chain);

		remain = chain->end - chain->tail;
		if (remain >= len) {
			goto copy;
		} else if (remain > 0) {
			memcpy(chain->tail, source, remain);
			chain->tail += remain;
			source += remain;
			len -= remain;
		}
    }

    chain = buffer_expand(b, len);
	if (!chain)
        return len;

copy:
    memcpy(chain->tail, source, len);
	chain->tail += len;
	return 0;
}

void buffer_drain(struct buffer *b, size_t len)
{
	struct buffer_chain *chain = b->head;
	struct buffer_chain *next;
	int datlen;

	if (!len)
		return;

	do {
		if (!chain)
			break;

		next = chain->next;
		datlen = chain->tail - chain->data;

		if (len < datlen) {
			chain->data += len;
			break;
		}

		len -= datlen;
        buffer_del_chain(b, chain);
		chain = next;
	} while(len);
}

int buffer_remove(struct buffer *b, char *dest, size_t len)
{
	int remain = len;

	do {
		int datlen;
		struct buffer_chain *chain = b->head;
		if (!chain)
            break;

        datlen = chain->tail - chain->data;
        if (datlen == 0)
            break;

		if (datlen > remain)
			datlen = remain;

		memcpy(dest, chain->data, datlen);
		dest += datlen;
		remain -= datlen;

        buffer_drain(b, datlen);
	} while (remain);

	return len - remain;
}

int buffer_add_string(struct buffer *b, const char *s)
{
    return buffer_add(b, s, strlen(s));
}

int buffer_add_vprintf(struct buffer *b, const char *fmt, va_list ap)
{
	int res;
	char *strp;

	if (vasprintf(&strp, fmt, ap) < 0)
		return -1;

	res = buffer_add_string(b, (const char *)strp);

	free(strp);

	return res;
}

int buffer_add_printf(struct buffer *b, const char *fmt, ...)
{
	int res = -1;
	va_list ap;

	va_start(ap, fmt);
	res = buffer_add_vprintf(b, fmt, ap);
	va_end(ap);

	return (res);
}

int buffer_add_fd(struct buffer *b, int fd, int len, bool *eof)
{
	struct buffer_chain *chain;
	int avalible, remain, res;

	*eof = false;

	if (len < 0)
		len = INT_MAX;
	remain = len;

	do {
		chain = b->tail;

		if (chain) {
			if (buffer_chain_should_align(chain, len))
				buffer_chain_align(chain);

			if (chain->tail < chain->end)
				goto begin;
		}

		chain = buffer_expand(b, 4096);
		if (!chain)
			return -1;

begin:
		avalible = chain->end - chain->tail;
		if (remain < avalible)
			avalible = remain;

		res = read(fd, chain->tail, avalible);
		if (res < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == ENOTCONN)
				break;

			return -1;
		}

		if (!res) {
			*eof = true;
			break;
		}

		chain->tail += res;
		remain -= res;
	} while (remain);

	return len - remain;
}
