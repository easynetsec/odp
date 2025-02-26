/* Copyright (c) 2018-2018, Linaro Limited
 * Copyright (c) 2021, Nokia
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include <odp/api/queue.h>
#include <odp/api/atomic.h>
#include <odp/api/plat/atomic_inlines.h>
#include <odp/api/shared_memory.h>

#include <odp_debug_internal.h>
#include <odp_event_internal.h>
#include <odp_queue_basic_internal.h>
#include <odp_types_internal.h>

#include <string.h>
#include <stdio.h>

#define RING_LF_SIZE   32
#define QUEUE_LF_NUM   128
#define ENQ_RETRIES    (RING_LF_SIZE / 4)
#define DEQ_RETRIES    (RING_LF_SIZE / 8)

#ifdef __SIZEOF_INT128__

static inline void lockfree_zero_u128(_odp_u128_t *atomic)
{
	__atomic_store_n(atomic, 0, __ATOMIC_RELAXED);
}

#include <odp_cpu.h>

#else

/* These definitions enable build in non 128 bit compatible systems.
 * Implementation is active only when 128 bit lockfree atomics are available.
 * So, these are never actually used. */
typedef struct ODP_ALIGNED(16) {
	uint64_t u64[2];
} _odp_u128_t;

static inline _odp_u128_t lockfree_load_u128(_odp_u128_t *atomic)
{
	return *atomic;
}

static inline void lockfree_zero_u128(_odp_u128_t *atomic)
{
	atomic->u64[0] = 0;
	atomic->u64[1] = 0;
}

static inline int lockfree_cas_acq_rel_u128(_odp_u128_t *atomic, _odp_u128_t old_val,
					    _odp_u128_t new_val)
{
	if (atomic->u64[0] == old_val.u64[0] &&
	    atomic->u64[1] == old_val.u64[1]) {
		atomic->u64[0] = new_val.u64[0];
		atomic->u64[1] = new_val.u64[1];
		return 1;
	}

	return 0;
}

static inline int lockfree_check_u128(void)
{
	return 0;
}

#endif

/* Node in lock-free ring */
typedef union {
	_odp_u128_t u128;

	struct {
		/* Data with lowest counter value is the head. Empty node has
		 * counter value 0. */
		uint64_t counter;

		/* Data pointer */
		uint64_t ptr;
	} s;

} ring_lf_node_t;

/* Lock-free ring */
typedef struct ODP_ALIGNED_CACHE {
	ring_lf_node_t   node[RING_LF_SIZE];
	int              used;
	odp_atomic_u64_t enq_counter;

} queue_lf_t;

/* Lock-free queue globals */
typedef struct ODP_ALIGNED_CACHE {
	queue_lf_t queue_lf[QUEUE_LF_NUM];
	odp_shm_t  shm;

} queue_lf_global_t;

static queue_lf_global_t *queue_lf_glb;

static inline int next_idx(int idx)
{
	int next = idx + 1;

	if (next == RING_LF_SIZE)
		next = 0;

	return next;
}

static int queue_lf_enq(odp_queue_t handle, _odp_event_hdr_t *event_hdr)
{
	queue_entry_t *queue;
	queue_lf_t *queue_lf;
	int i, j, idx;
	int found;
	ring_lf_node_t node_val;
	ring_lf_node_t new_val;
	ring_lf_node_t *node;

	queue    = qentry_from_handle(handle);
	queue_lf = queue->queue_lf;

	new_val.s.ptr     = (uintptr_t)event_hdr;
	new_val.s.counter = odp_atomic_fetch_inc_u64(&queue_lf->enq_counter);

	idx = 0;

	for (j = 0; j < ENQ_RETRIES; j++) {
		found = 0;

		/* Find empty node */
		for (i = 0; i < RING_LF_SIZE; i++) {
			node = &queue_lf->node[idx];
			idx  = next_idx(idx);

			node_val.u128 = lockfree_load_u128(&node->u128);

			if (node_val.s.counter == 0) {
				found = 1;
				break;
			}
		}

		/* Queue is full */
		if (found == 0)
			return -1;

		/* Try to insert data */
		if (lockfree_cas_acq_rel_u128(&node->u128, node_val.u128,
					    new_val.u128))
			return 0;
	}

	return -1;
}

static int queue_lf_enq_multi(odp_queue_t handle, _odp_event_hdr_t **event_hdr,
			      int num)
{
	(void)num;

	if (queue_lf_enq(handle, event_hdr[0]) == 0)
		return 1;

	return 0;
}

static _odp_event_hdr_t *queue_lf_deq(odp_queue_t handle)
{
	queue_entry_t *queue;
	queue_lf_t *queue_lf;
	int i, j, i_lowest;
	int found;
	ring_lf_node_t node_val, old_val, new_val;
	ring_lf_node_t *node, *old;
	uint64_t lowest, counter;
	_odp_event_hdr_t *event_hdr;

	queue    = qentry_from_handle(handle);
	queue_lf = queue->queue_lf;
	new_val.s.counter = 0;
	new_val.s.ptr     = 0;
	old = NULL;

	for (j = 0; j < DEQ_RETRIES; j++) {
		found  = 0;
		lowest = -1;

		/* Find the head node. The one with data and
		 * the lowest counter. */
		for (i = 0; i < RING_LF_SIZE; i++) {
			node          = &queue_lf->node[i];
			node_val.u128 = lockfree_load_u128(&node->u128);
			counter       = node_val.s.counter;

			if (counter && counter < lowest) {
				old          = node;
				old_val.u128 = node_val.u128;
				lowest       = counter;
				i_lowest     = i;
				found        = 1;
			}
		}

		/* Queue is empty */
		if (found == 0)
			return NULL;

		/* New data may have been written to the area we searched before
		 * finding the current lowest. Check that there are no lower
		 * values. */
		for (i = 0; i < i_lowest; i++) {
			node          = &queue_lf->node[i];
			node_val.u128 = lockfree_load_u128(&node->u128);
			counter       = node_val.s.counter;

			if (counter && counter < lowest) {
				old          = node;
				old_val.u128 = node_val.u128;
				lowest       = counter;
			}
		}

		event_hdr = (void *)(uintptr_t)old_val.s.ptr;

		/* Try to remove data */
		if (lockfree_cas_acq_rel_u128(&old->u128, old_val.u128,
					    new_val.u128))
			return event_hdr;
	}

	return NULL;
}

static int queue_lf_deq_multi(odp_queue_t handle, _odp_event_hdr_t **event_hdr,
			      int num)
{
	_odp_event_hdr_t *buf;

	(void)num;

	buf = queue_lf_deq(handle);

	if (buf == NULL)
		return 0;

	event_hdr[0] = buf;
	return 1;
}

uint32_t _odp_queue_lf_init_global(uint32_t *queue_lf_size,
				   queue_lf_func_t *lf_func)
{
	odp_shm_t shm;
	int lockfree;

	/* 16 byte lockfree CAS operation is needed. */
	lockfree = lockfree_check_u128();

	ODP_DBG("\nLock-free queue init\n");
	ODP_DBG("  u128 lock-free: %i\n\n", lockfree);

	if (!lockfree)
		return 0;

	shm = odp_shm_reserve("_odp_queues_lf_global", sizeof(queue_lf_global_t),
			      ODP_CACHE_LINE_SIZE,
			      0);
	if (shm == ODP_SHM_INVALID)
		return 0;

	queue_lf_glb = odp_shm_addr(shm);
	memset(queue_lf_glb, 0, sizeof(queue_lf_global_t));

	queue_lf_glb->shm = shm;

	memset(lf_func, 0, sizeof(queue_lf_func_t));
	lf_func->enq       = queue_lf_enq;
	lf_func->enq_multi = queue_lf_enq_multi;
	lf_func->deq       = queue_lf_deq;
	lf_func->deq_multi = queue_lf_deq_multi;

	*queue_lf_size = RING_LF_SIZE;

	return QUEUE_LF_NUM;
}

void _odp_queue_lf_term_global(void)
{
	odp_shm_t shm;

	if (queue_lf_glb == NULL)
		return;

	shm = queue_lf_glb->shm;

	if (odp_shm_free(shm) < 0)
		ODP_ERR("shm free failed");
}

static void init_queue(queue_lf_t *queue_lf)
{
	int i;

	odp_atomic_init_u64(&queue_lf->enq_counter, 1);

	for (i = 0; i < RING_LF_SIZE; i++)
		lockfree_zero_u128(&queue_lf->node[i].u128);
}

void *_odp_queue_lf_create(queue_entry_t *queue)
{
	int i;
	queue_lf_t *queue_lf = NULL;

	if (queue_lf_glb == NULL) {
		ODP_ERR("No lock-free queues available\n");
		return NULL;
	}

	if (queue->type != ODP_QUEUE_TYPE_PLAIN)
		return NULL;

	for (i = 0; i < QUEUE_LF_NUM; i++) {
		if (queue_lf_glb->queue_lf[i].used == 0) {
			queue_lf = &queue_lf_glb->queue_lf[i];
			memset(queue_lf, 0, sizeof(queue_lf_t));
			init_queue(queue_lf);
			queue_lf->used = 1;
			break;
		}
	}

	return queue_lf;
}

void _odp_queue_lf_destroy(void *queue_lf_ptr)
{
	queue_lf_t *queue_lf = queue_lf_ptr;

	queue_lf->used = 0;
}

uint32_t _odp_queue_lf_length(void *queue_lf_ptr)
{
	queue_lf_t *queue_lf = queue_lf_ptr;
	ring_lf_node_t node_val;
	uint32_t i;
	uint32_t num = 0;

	for (i = 0; i < RING_LF_SIZE; i++) {
		node_val.u128 =  lockfree_load_u128(&queue_lf->node[i].u128);
		if (node_val.s.counter)
			num++;
	}
	return num;
}

uint32_t _odp_queue_lf_max_length(void)
{
	return RING_LF_SIZE;
}

