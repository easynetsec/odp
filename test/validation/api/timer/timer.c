/* Copyright (c) 2015-2018, Linaro Limited
 * Copyright (c) 2019-2022, Nokia
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

/* For rand_r and nanosleep */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <time.h>
#include <odp_api.h>
#include <odp/helper/odph_api.h>
#include "odp_cunit_common.h"

#define MAX_WORKERS 32

#define GLOBAL_SHM_NAME	"GlobalTimerTest"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#define MAX_TIMER_POOLS  1024

/* Timeout range in milliseconds (ms) */
#define RANGE_MS 2000

/* Number of timers per thread */
#define NTIMERS 2000

/* Number of extra timers per thread */
#define EXTRA_TIMERS 256

#define NAME "timer_pool"
#define THREE_POINT_THREE_MSEC (10 * ODP_TIME_MSEC_IN_NS / 3)
#define USER_PTR ((void *)0xdead)
#define TICK_INVALID (~(uint64_t)0)

/* Test case options */
#define PRIV        1
#define EXP_RELAX   1
#define WAIT        0
#define CANCEL      1
#define RESTART     1
#define FIRST_TICK  1

/* Timer helper structure */
struct test_timer {
	odp_timer_t tim; /* Timer handle */
	odp_event_t ev;  /* Timeout event */
	odp_event_t ev2; /* Copy of event handle */
	uint64_t tick; /* Expiration tick or TICK_INVALID */
};

typedef struct {
	/* Clock source support flags */
	uint8_t clk_supported[ODP_CLOCK_NUM_SRC];

	/* Default resolution / timeout parameters */
	struct {
		uint64_t res_ns;
		uint64_t min_tmo;
		uint64_t max_tmo;
		odp_bool_t queue_type_sched;
		odp_bool_t queue_type_plain;
	} param;

	/* Timeout pool handle used by all threads */
	odp_pool_t tbp;

	/* Timer pool handle used by all threads */
	odp_timer_pool_t tp;

	/* Barrier for thread synchronization */
	odp_barrier_t test_barrier;

	/* Count of timeouts delivered too late */
	odp_atomic_u32_t ndelivtoolate;

	/* Sum of all allocated timers from all threads. Thread-local
	 * caches may make this number lower than the capacity of the pool */
	odp_atomic_u32_t timers_allocated;

	/* Number of timers allocated per thread */
	uint32_t timers_per_thread;

	/* Periodic timers supported */
	int periodic;

	/* Queue type to be tested */
	odp_queue_type_t test_queue_type;

} global_shared_mem_t;

static global_shared_mem_t *global_mem;

static int timer_global_init(odp_instance_t *inst)
{
	odp_shm_t global_shm;
	odp_init_t init_param;
	odph_helper_options_t helper_options;
	odp_timer_capability_t capa;
	odp_timer_res_capability_t res_capa;
	uint64_t res_ns, min_tmo, max_tmo;
	unsigned int range;
	int i;

	if (odph_options(&helper_options)) {
		fprintf(stderr, "error: odph_options() failed.\n");
		return -1;
	}

	odp_init_param_init(&init_param);
	init_param.mem_model = helper_options.mem_model;

	if (0 != odp_init_global(inst, &init_param, NULL)) {
		fprintf(stderr, "error: odp_init_global() failed.\n");
		return -1;
	}
	if (0 != odp_init_local(*inst, ODP_THREAD_CONTROL)) {
		fprintf(stderr, "error: odp_init_local() failed.\n");
		return -1;
	}

	global_shm = odp_shm_reserve(GLOBAL_SHM_NAME,
				     sizeof(global_shared_mem_t),
				     ODP_CACHE_LINE_SIZE, 0);
	if (global_shm == ODP_SHM_INVALID) {
		fprintf(stderr, "Unable reserve memory for global_shm\n");
		return -1;
	}

	global_mem = odp_shm_addr(global_shm);
	memset(global_mem, 0, sizeof(global_shared_mem_t));

	/* Configure scheduler */
	odp_schedule_config(NULL);

	memset(&capa, 0, sizeof(capa));
	if (odp_timer_capability(ODP_CLOCK_DEFAULT, &capa)) {
		fprintf(stderr, "Timer capability failed\n");
		return -1;
	}

	/* By default 20 msec resolution */
	res_ns = 20 * ODP_TIME_MSEC_IN_NS;
	if (res_ns < capa.max_res.res_ns)
		res_ns = capa.max_res.res_ns;

	memset(&res_capa, 0, sizeof(res_capa));
	res_capa.res_ns = res_ns;

	if (odp_timer_res_capability(ODP_CLOCK_DEFAULT, &res_capa)) {
		fprintf(stderr, "Timer resolution capability failed\n");
		return -1;
	}

	/* Try to keep min timeout error margin within +-20% */
	min_tmo = 5 * res_ns;
	if (min_tmo < res_capa.min_tmo)
		min_tmo = res_capa.min_tmo;

	/* Max 1 hour */
	max_tmo = 3600 * ODP_TIME_SEC_IN_NS;
	if (max_tmo > res_capa.max_tmo)
		max_tmo = res_capa.max_tmo;

	range = (RANGE_MS * 1000) + THREE_POINT_THREE_MSEC;
	if ((max_tmo - min_tmo) < range) {
		fprintf(stderr, "Validation test needs %u msec range\n", range);
		return -1;
	}

	/* Default parameters for test cases */
	global_mem->clk_supported[0] = 1;
	global_mem->param.res_ns  = res_ns;
	global_mem->param.min_tmo = min_tmo;
	global_mem->param.max_tmo = max_tmo;
	global_mem->param.queue_type_plain = capa.queue_type_plain;
	global_mem->param.queue_type_sched = capa.queue_type_sched;

	/* Check which other source clocks are supported */
	for (i = 1; i < ODP_CLOCK_NUM_SRC; i++) {
		if (odp_timer_capability(ODP_CLOCK_SRC_0 + i, &capa) == 0)
			global_mem->clk_supported[i] = 1;
	}

	/* Check if periodic timers are supported */
	if (capa.periodic.max_pools > 0)
		global_mem->periodic = 1;

	return 0;
}

static int timer_global_term(odp_instance_t inst)
{
	odp_shm_t shm;

	shm = odp_shm_lookup(GLOBAL_SHM_NAME);
	if (0 != odp_shm_free(shm)) {
		fprintf(stderr, "error: odp_shm_free() failed.\n");
		return -1;
	}

	if (0 != odp_term_local()) {
		fprintf(stderr, "error: odp_term_local() failed.\n");
		return -1;
	}

	if (0 != odp_term_global(inst)) {
		fprintf(stderr, "error: odp_term_global() failed.\n");
		return -1;
	}

	return 0;
}

static int
check_sched_queue_support(void)
{
	if (global_mem->param.queue_type_sched)
		return ODP_TEST_ACTIVE;

	return ODP_TEST_INACTIVE;
}

static int
check_plain_queue_support(void)
{
	if (global_mem->param.queue_type_plain)
		return ODP_TEST_ACTIVE;

	return ODP_TEST_INACTIVE;
}

static int check_periodic_sched_support(void)
{
	if (global_mem->periodic && global_mem->param.queue_type_sched)
		return ODP_TEST_ACTIVE;

	return ODP_TEST_INACTIVE;
}

static int check_periodic_plain_support(void)
{
	if (global_mem->periodic && global_mem->param.queue_type_plain)
		return ODP_TEST_ACTIVE;

	return ODP_TEST_INACTIVE;
}

static void timer_test_capa_run(odp_timer_clk_src_t clk_src)
{
	odp_timer_capability_t capa;
	odp_timer_res_capability_t res_capa;
	int ret;

	memset(&capa, 0, sizeof(capa));
	ret = odp_timer_capability(clk_src, &capa);
	CU_ASSERT_FATAL(ret == 0);

	CU_ASSERT(capa.highest_res_ns == capa.max_res.res_ns);
	/* Assuming max resoultion to be 100 msec or better */
	CU_ASSERT(capa.max_res.res_ns <= 100000000);
	CU_ASSERT(capa.max_res.res_hz >= 10);
	CU_ASSERT(capa.max_res.res_ns  < capa.max_res.max_tmo);
	CU_ASSERT(capa.max_res.min_tmo < capa.max_res.max_tmo);

	/* With max timeout, resolution may be low (worse than 1 sec) */
	CU_ASSERT(capa.max_tmo.res_ns  < capa.max_tmo.max_tmo);
	CU_ASSERT(capa.max_tmo.min_tmo < capa.max_tmo.max_tmo);
	CU_ASSERT(capa.max_tmo.res_ns != 0 || capa.max_tmo.res_hz != 0);
	if (capa.max_tmo.res_hz == 0)
		CU_ASSERT(capa.max_tmo.res_ns > 1000000000);

	/* Set max resolution in nsec */
	memset(&res_capa, 0, sizeof(res_capa));
	res_capa.res_ns = capa.max_res.res_ns;

	ret = odp_timer_res_capability(clk_src, &res_capa);
	CU_ASSERT_FATAL(ret == 0);
	CU_ASSERT(res_capa.res_ns  == capa.max_res.res_ns);
	CU_ASSERT(res_capa.min_tmo == capa.max_res.min_tmo);
	CU_ASSERT(res_capa.max_tmo == capa.max_res.max_tmo);

	/* Set max resolution in hz */
	memset(&res_capa, 0, sizeof(res_capa));
	res_capa.res_hz = capa.max_res.res_hz;

	ret = odp_timer_res_capability(clk_src, &res_capa);
	CU_ASSERT_FATAL(ret == 0);
	CU_ASSERT(res_capa.res_hz  == capa.max_res.res_hz);
	CU_ASSERT(res_capa.min_tmo == capa.max_res.min_tmo);
	CU_ASSERT(res_capa.max_tmo == capa.max_res.max_tmo);

	/* Set max timeout */
	memset(&res_capa, 0, sizeof(res_capa));
	res_capa.max_tmo = capa.max_tmo.max_tmo;

	ret = odp_timer_res_capability(clk_src, &res_capa);
	CU_ASSERT_FATAL(ret == 0);
	CU_ASSERT(res_capa.max_tmo == capa.max_tmo.max_tmo);
	CU_ASSERT(res_capa.min_tmo == capa.max_tmo.min_tmo);
	CU_ASSERT(res_capa.res_ns  == capa.max_tmo.res_ns);
	CU_ASSERT(res_capa.res_hz  == capa.max_tmo.res_hz);
}

static void timer_test_capa(void)
{
	odp_timer_clk_src_t clk_src;
	int i;

	/* Check that all API clock source enumeration values exist */
	CU_ASSERT_FATAL(ODP_CLOCK_DEFAULT == ODP_CLOCK_SRC_0);
	CU_ASSERT_FATAL(ODP_CLOCK_SRC_0 + 1 == ODP_CLOCK_SRC_1);
	CU_ASSERT_FATAL(ODP_CLOCK_SRC_0 + 2 == ODP_CLOCK_SRC_2);
	CU_ASSERT_FATAL(ODP_CLOCK_SRC_0 + 3 == ODP_CLOCK_SRC_3);
	CU_ASSERT_FATAL(ODP_CLOCK_SRC_0 + 4 == ODP_CLOCK_SRC_4);
	CU_ASSERT_FATAL(ODP_CLOCK_SRC_0 + 5 == ODP_CLOCK_SRC_5);
	CU_ASSERT_FATAL(ODP_CLOCK_SRC_5 + 1 == ODP_CLOCK_NUM_SRC);
	CU_ASSERT_FATAL(ODP_CLOCK_CPU == ODP_CLOCK_DEFAULT);
	CU_ASSERT_FATAL(ODP_CLOCK_EXT == ODP_CLOCK_SRC_1);

	for (i = 0; i < ODP_CLOCK_NUM_SRC; i++) {
		clk_src = ODP_CLOCK_SRC_0 + i;
		if (global_mem->clk_supported[i]) {
			ODPH_DBG("\nTesting clock source: %i\n", clk_src);
			timer_test_capa_run(clk_src);
		}
	}
}

static void test_param_init(uint8_t fill)
{
	odp_timer_pool_param_t tp_param;

	memset(&tp_param, fill, sizeof(tp_param));

	odp_timer_pool_param_init(&tp_param);
	CU_ASSERT(tp_param.res_ns == 0);
	CU_ASSERT(tp_param.res_hz == 0);
	CU_ASSERT(tp_param.min_tmo == 0);
	CU_ASSERT(tp_param.priv == 0);
	CU_ASSERT(tp_param.clk_src == ODP_CLOCK_DEFAULT);
	CU_ASSERT(tp_param.exp_mode == ODP_TIMER_EXP_AFTER);
	CU_ASSERT(tp_param.timer_type == ODP_TIMER_TYPE_SINGLE);
	CU_ASSERT(tp_param.periodic.base_freq_hz.integer == 0);
	CU_ASSERT(tp_param.periodic.base_freq_hz.numer == 0);
	CU_ASSERT(tp_param.periodic.base_freq_hz.denom == 0);
}

static void timer_test_param_init(void)
{
	test_param_init(0);
	test_param_init(0xff);
}

static void timer_test_timeout_pool_alloc(void)
{
	odp_pool_t pool;
	const int num = 3;
	odp_timeout_t tmo[num];
	odp_event_t ev;
	int index;
	odp_bool_t wrong_type = false, wrong_subtype = false;
	odp_pool_param_t params;

	odp_pool_param_init(&params);
	params.type    = ODP_POOL_TIMEOUT;
	params.tmo.num = num;

	pool = odp_pool_create("timeout_pool_alloc", &params);
	CU_ASSERT_FATAL(pool != ODP_POOL_INVALID);

	odp_pool_print(pool);

	/* Try to allocate num items from the pool */
	for (index = 0; index < num; index++) {
		odp_event_subtype_t subtype;

		tmo[index] = odp_timeout_alloc(pool);

		if (tmo[index] == ODP_TIMEOUT_INVALID)
			break;

		ev = odp_timeout_to_event(tmo[index]);
		if (odp_event_type(ev) != ODP_EVENT_TIMEOUT)
			wrong_type = true;
		if (odp_event_subtype(ev) != ODP_EVENT_NO_SUBTYPE)
			wrong_subtype = true;
		if (odp_event_types(ev, &subtype) != ODP_EVENT_TIMEOUT)
			wrong_type = true;
		if (subtype != ODP_EVENT_NO_SUBTYPE)
			wrong_subtype = true;
	}

	/* Check that the pool had at least num items */
	CU_ASSERT(index == num);
	/* index points out of buffer[] or it point to an invalid buffer */
	index--;

	/* Check that the pool had correct buffers */
	CU_ASSERT(!wrong_type);
	CU_ASSERT(!wrong_subtype);

	for (; index >= 0; index--)
		odp_timeout_free(tmo[index]);

	CU_ASSERT(odp_pool_destroy(pool) == 0);
}

static void timer_test_timeout_pool_free(void)
{
	odp_pool_t pool;
	odp_timeout_t tmo;
	odp_pool_param_t params;

	odp_pool_param_init(&params);
	params.type    = ODP_POOL_TIMEOUT;
	params.tmo.num = 1;

	pool = odp_pool_create("timeout_pool_free", &params);
	CU_ASSERT_FATAL(pool != ODP_POOL_INVALID);
	odp_pool_print(pool);

	/* Allocate the only timeout from the pool */
	tmo = odp_timeout_alloc(pool);
	CU_ASSERT_FATAL(tmo != ODP_TIMEOUT_INVALID);

	/* Pool should have only one timeout */
	CU_ASSERT_FATAL(odp_timeout_alloc(pool) == ODP_TIMEOUT_INVALID)

	odp_timeout_free(tmo);

	/* Check that the timeout was returned back to the pool */
	tmo = odp_timeout_alloc(pool);
	CU_ASSERT_FATAL(tmo != ODP_TIMEOUT_INVALID);

	odp_timeout_free(tmo);
	CU_ASSERT(odp_pool_destroy(pool) == 0);
}

static void timer_pool_create_destroy(void)
{
	odp_timer_pool_param_t tparam;
	odp_queue_param_t queue_param;
	odp_timer_capability_t capa;
	odp_timer_pool_info_t info;
	odp_timer_pool_t tp[2];
	odp_timer_t tim;
	odp_queue_t queue;
	int ret;

	memset(&capa, 0, sizeof(capa));
	ret = odp_timer_capability(ODP_CLOCK_DEFAULT, &capa);
	CU_ASSERT_FATAL(ret == 0);

	odp_queue_param_init(&queue_param);
	if (capa.queue_type_plain) {
		queue_param.type = ODP_QUEUE_TYPE_PLAIN;
	} else if (capa.queue_type_sched) {
		queue_param.type = ODP_QUEUE_TYPE_SCHED;
		queue_param.sched.sync = ODP_SCHED_SYNC_ATOMIC;
	}
	queue = odp_queue_create("timer_queue", &queue_param);
	CU_ASSERT_FATAL(queue != ODP_QUEUE_INVALID);

	odp_timer_pool_param_init(&tparam);
	tparam.res_ns     = global_mem->param.res_ns;
	tparam.min_tmo    = global_mem->param.min_tmo;
	tparam.max_tmo    = global_mem->param.max_tmo;
	tparam.num_timers = 100;
	tparam.priv       = 0;
	tparam.clk_src    = ODP_CLOCK_DEFAULT;

	tp[0] = odp_timer_pool_create("timer_pool_a", &tparam);
	CU_ASSERT(tp[0] != ODP_TIMER_POOL_INVALID);

	odp_timer_pool_start();

	tim = odp_timer_alloc(tp[0], queue, USER_PTR);
	CU_ASSERT(tim != ODP_TIMER_INVALID);
	CU_ASSERT(odp_timer_free(tim) == ODP_EVENT_INVALID);

	odp_timer_pool_destroy(tp[0]);

	tp[0] = odp_timer_pool_create("timer_pool_b", &tparam);
	CU_ASSERT(tp[0] != ODP_TIMER_POOL_INVALID);
	tp[1] = odp_timer_pool_create("timer_pool_c", &tparam);
	CU_ASSERT(tp[1] != ODP_TIMER_POOL_INVALID);

	odp_timer_pool_start();

	odp_timer_pool_destroy(tp[0]);

	tp[0] = odp_timer_pool_create("timer_pool_d", &tparam);
	CU_ASSERT(tp[0] != ODP_TIMER_POOL_INVALID);

	odp_timer_pool_start();

	memset(&info, 0, sizeof(odp_timer_pool_info_t));
	CU_ASSERT(odp_timer_pool_info(tp[1], &info) == 0);
	CU_ASSERT(strcmp(info.name, "timer_pool_c") == 0);

	tim = odp_timer_alloc(tp[1], queue, USER_PTR);
	CU_ASSERT(tim != ODP_TIMER_INVALID);
	CU_ASSERT(odp_timer_free(tim) == ODP_EVENT_INVALID);

	odp_timer_pool_destroy(tp[1]);

	memset(&info, 0, sizeof(odp_timer_pool_info_t));
	CU_ASSERT(odp_timer_pool_info(tp[0], &info) == 0);
	CU_ASSERT(strcmp(info.name, "timer_pool_d") == 0);

	tim = odp_timer_alloc(tp[0], queue, USER_PTR);
	CU_ASSERT(tim != ODP_TIMER_INVALID);
	CU_ASSERT(odp_timer_free(tim) == ODP_EVENT_INVALID);

	odp_timer_pool_destroy(tp[0]);

	CU_ASSERT(odp_queue_destroy(queue) == 0);
}

static void timer_pool_create_max(void)
{
	odp_timer_capability_t capa;
	odp_timer_pool_param_t tp_param;
	odp_queue_param_t queue_param;
	odp_queue_t queue;
	uint32_t i;
	int ret;
	uint64_t tmo_ns = ODP_TIME_SEC_IN_NS;
	uint64_t res_ns = ODP_TIME_SEC_IN_NS / 10;

	memset(&capa, 0, sizeof(capa));
	ret = odp_timer_capability(ODP_CLOCK_DEFAULT, &capa);
	CU_ASSERT_FATAL(ret == 0);

	uint32_t num = capa.max_pools;

	if (num > MAX_TIMER_POOLS)
		num = MAX_TIMER_POOLS;

	odp_timer_pool_t tp[num];
	odp_timer_t timer[num];

	if (capa.max_tmo.max_tmo < tmo_ns) {
		tmo_ns = capa.max_tmo.max_tmo;
		res_ns = capa.max_tmo.res_ns;
	}

	odp_queue_param_init(&queue_param);

	if (capa.queue_type_sched)
		queue_param.type = ODP_QUEUE_TYPE_SCHED;

	queue = odp_queue_create("timer_queue", &queue_param);
	CU_ASSERT_FATAL(queue != ODP_QUEUE_INVALID);

	odp_timer_pool_param_init(&tp_param);

	tp_param.res_ns     = res_ns;
	tp_param.min_tmo    = tmo_ns / 2;
	tp_param.max_tmo    = tmo_ns;
	tp_param.num_timers = 1;

	for (i = 0; i < num; i++) {
		tp[i] = odp_timer_pool_create("test_max", &tp_param);
		if (tp[i] == ODP_TIMER_POOL_INVALID)
			ODPH_ERR("Timer pool create failed: %u / %u\n", i, num);

		CU_ASSERT_FATAL(tp[i] != ODP_TIMER_POOL_INVALID);
	}

	odp_timer_pool_start();

	for (i = 0; i < num; i++) {
		timer[i] = odp_timer_alloc(tp[i], queue, USER_PTR);

		if (timer[i] == ODP_TIMER_INVALID)
			ODPH_ERR("Timer alloc failed: %u / %u\n", i, num);

		CU_ASSERT_FATAL(timer[i] != ODP_TIMER_INVALID);
	}

	for (i = 0; i < num; i++)
		CU_ASSERT(odp_timer_free(timer[i]) == ODP_EVENT_INVALID);

	for (i = 0; i < num; i++)
		odp_timer_pool_destroy(tp[i]);

	CU_ASSERT(odp_queue_destroy(queue) == 0);
}

static void timer_pool_max_res(void)
{
	odp_timer_capability_t capa;
	odp_timer_pool_param_t tp_param;
	odp_queue_param_t queue_param;
	odp_timer_pool_t tp;
	odp_timer_t timer;
	odp_pool_param_t pool_param;
	odp_pool_t pool;
	odp_queue_t queue;
	odp_timeout_t tmo;
	odp_event_t ev;
	uint64_t tick;
	int ret, i;

	memset(&capa, 0, sizeof(capa));
	ret = odp_timer_capability(ODP_CLOCK_DEFAULT, &capa);
	CU_ASSERT_FATAL(ret == 0);

	odp_pool_param_init(&pool_param);
	pool_param.type    = ODP_POOL_TIMEOUT;
	pool_param.tmo.num = 10;
	pool = odp_pool_create("timeout_pool", &pool_param);
	CU_ASSERT_FATAL(pool != ODP_POOL_INVALID);

	odp_queue_param_init(&queue_param);
	if (capa.queue_type_plain) {
		queue_param.type = ODP_QUEUE_TYPE_PLAIN;
	} else if (capa.queue_type_sched) {
		queue_param.type = ODP_QUEUE_TYPE_SCHED;
		queue_param.sched.sync = ODP_SCHED_SYNC_ATOMIC;
	}
	queue = odp_queue_create("timer_queue", &queue_param);
	CU_ASSERT_FATAL(queue != ODP_QUEUE_INVALID);

	/* Highest resolution: first in nsec, then in hz */
	for (i = 0; i < 2; i++) {
		odp_timer_pool_param_init(&tp_param);

		if (i == 0) {
			printf("\n    Highest resolution %" PRIu64 " nsec\n",
			       capa.max_res.res_ns);
			tp_param.res_ns = capa.max_res.res_ns;
		} else {
			printf("    Highest resolution %" PRIu64 " Hz\n",
			       capa.max_res.res_hz);
			tp_param.res_hz = capa.max_res.res_hz;
		}

		tp_param.min_tmo    = capa.max_res.min_tmo;
		tp_param.max_tmo    = capa.max_res.max_tmo;
		tp_param.num_timers = 100;
		tp_param.priv       = 0;
		tp_param.clk_src    = ODP_CLOCK_DEFAULT;

		tp = odp_timer_pool_create("high_res_tp", &tp_param);
		CU_ASSERT_FATAL(tp != ODP_TIMER_POOL_INVALID);

		odp_timer_pool_start();

		/* Maximum timeout length with maximum resolution */
		tick = odp_timer_ns_to_tick(tp, capa.max_res.max_tmo);

		timer = odp_timer_alloc(tp, queue, USER_PTR);
		CU_ASSERT_FATAL(timer != ODP_TIMER_INVALID);

		tmo = odp_timeout_alloc(pool);
		ev  = odp_timeout_to_event(tmo);
		CU_ASSERT_FATAL(ev != ODP_EVENT_INVALID);

		ret = odp_timer_set_rel(timer, tick, &ev);
		CU_ASSERT(ret == ODP_TIMER_SUCCESS);

		ev = ODP_EVENT_INVALID;
		ret = odp_timer_cancel(timer, &ev);
		CU_ASSERT(ret == 0);

		if (ret == 0) {
			CU_ASSERT(ev != ODP_EVENT_INVALID);
			odp_event_free(ev);
		}

		CU_ASSERT(odp_timer_free(timer) == ODP_EVENT_INVALID);
		odp_timer_pool_destroy(tp);
	}

	CU_ASSERT(odp_queue_destroy(queue) == 0);
	CU_ASSERT(odp_pool_destroy(pool) == 0);
}

static odp_event_t wait_event(odp_queue_type_t queue_type, odp_queue_t queue,
			      odp_time_t t1, uint64_t wait_ns)
{
	odp_time_t t2;
	odp_event_t ev;
	odp_queue_t from = ODP_QUEUE_INVALID;

	while (1) {
		if (queue_type == ODP_QUEUE_TYPE_SCHED)
			ev = odp_schedule(&from, ODP_SCHED_NO_WAIT);
		else
			ev = odp_queue_deq(queue);

		if (ev != ODP_EVENT_INVALID) {
			if (queue_type == ODP_QUEUE_TYPE_SCHED)
				CU_ASSERT(from == queue);

			return ev;
		}

		t2 = odp_time_global();
		if (odp_time_diff_ns(t2, t1) > wait_ns)
			break;
	}

	return ODP_EVENT_INVALID;
}

static void free_schedule_context(odp_queue_type_t queue_type)
{
	if (queue_type != ODP_QUEUE_TYPE_SCHED)
		return;

	while (1) {
		odp_event_t ev = odp_schedule(NULL, ODP_SCHED_NO_WAIT);

		CU_ASSERT(ev == ODP_EVENT_INVALID);

		if (ev == ODP_EVENT_INVALID)
			break;

		odp_event_free(ev);
	}
}

static void timer_single_shot(odp_queue_type_t queue_type, odp_timer_tick_type_t tick_type,
			      int restart, int cancel)
{
	odp_timer_capability_t capa;
	odp_timer_pool_param_t tp_param;
	odp_queue_param_t queue_param;
	odp_pool_param_t pool_param;
	odp_timer_start_t start_param;
	odp_timer_pool_t tp;
	odp_timer_t timer;
	odp_pool_t pool;
	odp_queue_t queue;
	odp_timeout_t tmo;
	odp_event_t ev;
	odp_time_t t1, t2;
	uint64_t tick, nsec;
	int ret;
	uint64_t tmo_ns = ODP_TIME_SEC_IN_NS;
	uint64_t res_ns = ODP_TIME_SEC_IN_NS / 10;

	memset(&capa, 0, sizeof(capa));
	ret = odp_timer_capability(ODP_CLOCK_DEFAULT, &capa);
	CU_ASSERT_FATAL(ret == 0);
	CU_ASSERT_FATAL(capa.max_tmo.max_tmo > 0);

	if (capa.max_tmo.max_tmo < tmo_ns) {
		tmo_ns = capa.max_tmo.max_tmo;
		res_ns = capa.max_tmo.res_ns;
	}

	odp_pool_param_init(&pool_param);
	pool_param.type    = ODP_POOL_TIMEOUT;
	pool_param.tmo.num = 10;
	pool = odp_pool_create("timeout_pool", &pool_param);
	CU_ASSERT_FATAL(pool != ODP_POOL_INVALID);

	odp_queue_param_init(&queue_param);
	queue_param.type = queue_type;

	if (queue_type == ODP_QUEUE_TYPE_SCHED)
		queue_param.sched.sync = ODP_SCHED_SYNC_ATOMIC;

	queue = odp_queue_create("timer_queue", &queue_param);
	CU_ASSERT_FATAL(queue != ODP_QUEUE_INVALID);

	odp_timer_pool_param_init(&tp_param);

	tp_param.res_ns     = res_ns;
	tp_param.min_tmo    = tmo_ns / 4;
	tp_param.max_tmo    = tmo_ns;
	tp_param.num_timers = 1;

	tp = odp_timer_pool_create("test_single", &tp_param);
	CU_ASSERT_FATAL(tp != ODP_TIMER_POOL_INVALID);

	odp_timer_pool_start();

	timer = odp_timer_alloc(tp, queue, USER_PTR);
	CU_ASSERT_FATAL(timer != ODP_TIMER_INVALID);

	tmo = odp_timeout_alloc(pool);
	ev  = odp_timeout_to_event(tmo);
	CU_ASSERT_FATAL(ev != ODP_EVENT_INVALID);

	nsec = tmo_ns;
	if (restart)
		nsec = tmo_ns / 2;

	tick = odp_timer_ns_to_tick(tp, nsec);
	if (tick_type == ODP_TIMER_TICK_ABS)
		tick += odp_timer_current_tick(tp);

	start_param.tick_type = tick_type;
	start_param.tick      = tick;
	start_param.tmo_ev    = ev;

	ret = odp_timer_start(timer, &start_param);
	CU_ASSERT_FATAL(ret == ODP_TIMER_SUCCESS);

	if (restart) {
		tick = odp_timer_ns_to_tick(tp, tmo_ns);
		if (tick_type == ODP_TIMER_TICK_ABS)
			tick += odp_timer_current_tick(tp);

		start_param.tick   = tick;
		start_param.tmo_ev = ODP_EVENT_INVALID;

		ret = odp_timer_restart(timer, &start_param);
		CU_ASSERT_FATAL(ret == ODP_TIMER_SUCCESS);
	}

	ev = ODP_EVENT_INVALID;

	if (cancel) {
		ret = odp_timer_cancel(timer, &ev);
		CU_ASSERT(ret == 0);

		if (ret == 0)
			CU_ASSERT(ev != ODP_EVENT_INVALID);
	} else {
		uint64_t diff_ns;

		t1 = odp_time_global();
		ev = wait_event(queue_type, queue, t1, 10 * tmo_ns);
		t2 = odp_time_global();
		diff_ns = odp_time_diff_ns(t2, t1);

		CU_ASSERT(ev != ODP_EVENT_INVALID);
		CU_ASSERT(diff_ns < 2 * tmo_ns);
		CU_ASSERT((double)diff_ns > 0.5 * tmo_ns);
	}

	if (ev != ODP_EVENT_INVALID) {
		CU_ASSERT_FATAL(odp_event_type(ev) == ODP_EVENT_TIMEOUT);
		tmo = odp_timeout_from_event(ev);
		CU_ASSERT(odp_timeout_user_ptr(tmo) == USER_PTR);
		CU_ASSERT(odp_timeout_timer(tmo) == timer);

		if (!cancel) {
			if (tick_type == ODP_TIMER_TICK_ABS) {
				/* CU_ASSERT needs these extra brackets */
				CU_ASSERT(odp_timeout_tick(tmo) == tick);
			} else {
				CU_ASSERT(odp_timeout_tick(tmo) > tick);
			}
		}

		odp_event_free(ev);
	}

	free_schedule_context(queue_type);

	CU_ASSERT(odp_timer_free(timer) == ODP_EVENT_INVALID);
	odp_timer_pool_destroy(tp);

	CU_ASSERT(odp_queue_destroy(queue) == 0);
	CU_ASSERT(odp_pool_destroy(pool) == 0);
}

static void timer_plain_rel_wait(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_PLAIN, ODP_TIMER_TICK_REL, 0, WAIT);
}

static void timer_plain_abs_wait(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_PLAIN, ODP_TIMER_TICK_ABS, 0, WAIT);
}

static void timer_plain_rel_cancel(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_PLAIN, ODP_TIMER_TICK_REL, 0, CANCEL);
}

static void timer_plain_abs_cancel(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_PLAIN, ODP_TIMER_TICK_ABS, 0, CANCEL);
}

static void timer_plain_rel_restart_wait(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_PLAIN, ODP_TIMER_TICK_REL, RESTART, WAIT);
}

static void timer_plain_abs_restart_wait(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_PLAIN, ODP_TIMER_TICK_ABS, RESTART, WAIT);
}

static void timer_plain_rel_restart_cancel(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_PLAIN, ODP_TIMER_TICK_REL, RESTART, CANCEL);
}

static void timer_plain_abs_restart_cancel(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_PLAIN, ODP_TIMER_TICK_ABS, RESTART, CANCEL);
}

static void timer_sched_rel_wait(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_SCHED, ODP_TIMER_TICK_REL, 0, WAIT);
}

static void timer_sched_abs_wait(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_SCHED, ODP_TIMER_TICK_ABS, 0, WAIT);
}

static void timer_sched_rel_cancel(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_SCHED, ODP_TIMER_TICK_REL, 0, CANCEL);
}

static void timer_sched_abs_cancel(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_SCHED, ODP_TIMER_TICK_ABS, 0, CANCEL);
}

static void timer_sched_rel_restart_wait(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_SCHED, ODP_TIMER_TICK_REL, RESTART, WAIT);
}

static void timer_sched_abs_restart_wait(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_SCHED, ODP_TIMER_TICK_ABS, RESTART, WAIT);
}

static void timer_sched_rel_restart_cancel(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_SCHED, ODP_TIMER_TICK_REL, RESTART, CANCEL);
}

static void timer_sched_abs_restart_cancel(void)
{
	timer_single_shot(ODP_QUEUE_TYPE_SCHED, ODP_TIMER_TICK_ABS, RESTART, CANCEL);
}

static void timer_pool_tick_info_run(odp_timer_clk_src_t clk_src)
{
	odp_timer_capability_t capa;
	odp_timer_pool_param_t tp_param;
	odp_timer_pool_t tp;
	odp_timer_pool_info_t info;
	uint64_t ticks_per_sec;
	double tick_hz, tick_nsec, tick_to_nsec, tick_low;

	memset(&capa, 0, sizeof(capa));
	CU_ASSERT_FATAL(odp_timer_capability(clk_src, &capa) == 0);

	/* Highest resolution */
	odp_timer_pool_param_init(&tp_param);
	tp_param.res_hz     = capa.max_res.res_hz;
	tp_param.min_tmo    = capa.max_res.min_tmo;
	tp_param.max_tmo    = capa.max_res.max_tmo;
	tp_param.num_timers = 100;
	tp_param.priv       = 0;
	tp_param.clk_src    = clk_src;

	tp = odp_timer_pool_create("tick_info_tp", &tp_param);
	CU_ASSERT_FATAL(tp != ODP_TIMER_POOL_INVALID);

	odp_timer_pool_start();

	memset(&info, 0, sizeof(odp_timer_pool_info_t));
	CU_ASSERT_FATAL(odp_timer_pool_info(tp, &info) == 0);

	/* Tick frequency in hertz. Allow 1 hz rounding error between odp_timer_ns_to_tick()
	 * and tick_info. */
	ticks_per_sec = odp_timer_ns_to_tick(tp, ODP_TIME_SEC_IN_NS);
	tick_hz       = odp_fract_u64_to_dbl(&info.tick_info.freq);

	CU_ASSERT(((double)(ticks_per_sec - 1)) <= tick_hz);
	CU_ASSERT(((double)(ticks_per_sec + 1)) >= tick_hz);

	/* Tick frequency must be the same or higher that resolution */
	CU_ASSERT(tick_hz >= tp_param.res_hz);

	printf("\nClock source %i\n", clk_src);
	printf("  Ticks per second:     %" PRIu64 "\n", ticks_per_sec);
	printf("  Tick info freq:       %" PRIu64 " + %" PRIu64 " / %" PRIu64 "\n",
	       info.tick_info.freq.integer,
	       info.tick_info.freq.numer,
	       info.tick_info.freq.denom);
	printf("  Tick info freq dbl:   %f\n", tick_hz);

	/* One tick on nsec. For better resolution, convert 1000 ticks (and use double)
	 * instead of one tick. Allow 1 nsec rounding error between odp_timer_tick_to_ns()
	 * and tick_info. */
	tick_to_nsec = odp_timer_tick_to_ns(tp, 1000) / 1000.0;
	tick_nsec    = odp_fract_u64_to_dbl(&info.tick_info.nsec);
	tick_low = tick_to_nsec - 1.0;
	if (tick_to_nsec < 1.0)
		tick_low = 0.0;

	CU_ASSERT(tick_low <= tick_nsec);
	CU_ASSERT((tick_to_nsec + 1.0) >= tick_nsec);

	printf("  Tick in nsec:         %f\n", tick_to_nsec);
	printf("  Tick info nsec:       %" PRIu64 " + %" PRIu64 " / %" PRIu64 "\n",
	       info.tick_info.nsec.integer,
	       info.tick_info.nsec.numer,
	       info.tick_info.nsec.denom);
	printf("  Tick info nsec dbl:   %f\n", tick_nsec);

	/* One tick in source clock cycles. Depending on clock source it may be zero.
	 * Print the values to have a reference to the fields. */
	printf("  Tick info clk cycles: %" PRIu64 " + %" PRIu64 " / %" PRIu64 "\n",
	       info.tick_info.clk_cycle.integer,
	       info.tick_info.clk_cycle.numer,
	       info.tick_info.clk_cycle.denom);

	odp_timer_pool_destroy(tp);
}

static void timer_pool_tick_info(void)
{
	odp_timer_clk_src_t clk_src;
	int i;

	for (i = 0; i < ODP_CLOCK_NUM_SRC; i++) {
		clk_src = ODP_CLOCK_SRC_0 + i;
		if (global_mem->clk_supported[i]) {
			ODPH_DBG("\nTesting clock source: %i\n", clk_src);
			timer_pool_tick_info_run(clk_src);
		}
	}
}

static void timer_test_event_type(odp_queue_type_t queue_type,
				  odp_event_type_t event_type)
{
	odp_pool_t pool;
	odp_pool_param_t pool_param;
	odp_queue_param_t queue_param;
	odp_timer_pool_param_t timer_param;
	odp_timer_pool_t timer_pool;
	odp_queue_t queue;
	odp_timeout_t tmo;
	odp_buffer_t buf;
	odp_packet_t pkt;
	odp_event_t ev;
	odp_time_t t1, t2;
	uint64_t period_ns, period_tick, duration_ns;
	int i, ret, num_tmo;
	const char *user_ctx = "User context";
	int test_print = 0;
	int num = 5;
	odp_timer_t timer[num];

	odp_timer_pool_param_init(&timer_param);
	timer_param.res_ns     = global_mem->param.res_ns;
	timer_param.min_tmo    = global_mem->param.min_tmo;
	period_ns              = 2 * global_mem->param.min_tmo;
	timer_param.max_tmo    = global_mem->param.max_tmo;
	timer_param.num_timers = num;
	timer_param.clk_src    = ODP_CLOCK_DEFAULT;

	timer_pool = odp_timer_pool_create("timer_pool", &timer_param);
	if (timer_pool == ODP_TIMER_POOL_INVALID)
		CU_FAIL_FATAL("Timer pool create failed");

	odp_timer_pool_start();

	odp_pool_param_init(&pool_param);

	if (event_type == ODP_EVENT_BUFFER) {
		pool_param.type    = ODP_POOL_BUFFER;
		pool_param.buf.num = num;
	} else if (event_type == ODP_EVENT_PACKET) {
		pool_param.type    = ODP_POOL_PACKET;
		pool_param.pkt.num = num;
	} else if (event_type == ODP_EVENT_TIMEOUT) {
		pool_param.type    = ODP_POOL_TIMEOUT;
		pool_param.tmo.num = num;
		test_print = 1;
	} else {
		CU_FAIL("Bad event_type");
		return;
	}

	pool = odp_pool_create("timeout_pool", &pool_param);
	CU_ASSERT_FATAL(pool != ODP_POOL_INVALID);

	odp_queue_param_init(&queue_param);
	if (queue_type == ODP_QUEUE_TYPE_SCHED) {
		queue_param.type       = ODP_QUEUE_TYPE_SCHED;
		queue_param.sched.sync = ODP_SCHED_SYNC_ATOMIC;
	}

	queue = odp_queue_create("timeout_queue", &queue_param);
	if (queue == ODP_QUEUE_INVALID)
		CU_FAIL_FATAL("Queue create failed");

	period_tick = odp_timer_ns_to_tick(timer_pool, period_ns);
	duration_ns = num * period_ns;

	ODPH_DBG("\nTimer pool parameters:\n");
	ODPH_DBG("  res_ns  %" PRIu64 "\n", timer_param.res_ns);
	ODPH_DBG("  min_tmo %" PRIu64 "\n", timer_param.min_tmo);
	ODPH_DBG("  max_tmo %" PRIu64 "\n", timer_param.max_tmo);
	ODPH_DBG("  period_ns %" PRIu64 "\n", period_ns);
	ODPH_DBG("  period_tick %" PRIu64 "\n", period_tick);
	ODPH_DBG("  duration_ns %" PRIu64 "\n", duration_ns);
	ODPH_DBG("  user_ptr %p\n\n", (const void *)user_ctx);

	for (i = 0; i < num; i++) {
		if (event_type == ODP_EVENT_BUFFER) {
			buf = odp_buffer_alloc(pool);
			ev  = odp_buffer_to_event(buf);
		} else if (event_type == ODP_EVENT_PACKET) {
			pkt = odp_packet_alloc(pool, 10);
			ev  = odp_packet_to_event(pkt);
		} else {
			tmo = odp_timeout_alloc(pool);
			ev  = odp_timeout_to_event(tmo);
		}

		CU_ASSERT(ev != ODP_EVENT_INVALID);

		timer[i] = odp_timer_alloc(timer_pool, queue, user_ctx);
		CU_ASSERT_FATAL(timer[i] != ODP_TIMER_INVALID);

		ret = odp_timer_set_rel(timer[i], (i + 1) * period_tick, &ev);

		if (ret == ODP_TIMER_TOO_NEAR)
			ODPH_DBG("Timer set failed. Too near %i.\n", i);
		else if (ret == ODP_TIMER_TOO_FAR)
			ODPH_DBG("Timer set failed. Too far %i.\n", i);
		else if (ret == ODP_TIMER_FAIL)
			ODPH_DBG("Timer set failed %i\n", i);

		CU_ASSERT(ret == ODP_TIMER_SUCCESS);
	}

	if (test_print) {
		printf("\n");
		odp_timer_pool_print(timer_pool);
		odp_timer_print(timer[0]);
	}

	ev = ODP_EVENT_INVALID;
	num_tmo = 0;
	t1 = odp_time_local();

	/* Wait for timers. Make sure that scheduler context is not held when
	 * exiting the loop. */
	do {
		if (queue_type == ODP_QUEUE_TYPE_SCHED)
			ev = odp_schedule(NULL, ODP_SCHED_NO_WAIT);
		else
			ev = odp_queue_deq(queue);

		if (ev == ODP_EVENT_INVALID) {
			t2 = odp_time_local();
			if (odp_time_diff_ns(t2, t1) > (10 * duration_ns))
				break;

			continue;
		}

		CU_ASSERT(odp_event_type(ev) == event_type);

		if (test_print) {
			test_print = 0;
			tmo = odp_timeout_from_event(ev);
			odp_timeout_print(tmo);
			printf("\n");
		}

		odp_event_free(ev);
		num_tmo++;

	} while (num_tmo < num || ev != ODP_EVENT_INVALID);

	CU_ASSERT(num_tmo == num);

	for (i = 0; i < num; i++)
		CU_ASSERT(odp_timer_free(timer[i]) == ODP_EVENT_INVALID);

	odp_timer_pool_destroy(timer_pool);
	CU_ASSERT(odp_queue_destroy(queue) == 0);
	CU_ASSERT(odp_pool_destroy(pool) == 0);
}

static void timer_test_tmo_event_plain(void)
{
	timer_test_event_type(ODP_QUEUE_TYPE_PLAIN, ODP_EVENT_TIMEOUT);
}

static void timer_test_tmo_event_sched(void)
{
	timer_test_event_type(ODP_QUEUE_TYPE_SCHED, ODP_EVENT_TIMEOUT);
}

static void timer_test_buf_event_plain(void)
{
	timer_test_event_type(ODP_QUEUE_TYPE_PLAIN, ODP_EVENT_BUFFER);
}

static void timer_test_buf_event_sched(void)
{
	timer_test_event_type(ODP_QUEUE_TYPE_SCHED, ODP_EVENT_BUFFER);
}

static void timer_test_pkt_event_plain(void)
{
	timer_test_event_type(ODP_QUEUE_TYPE_PLAIN, ODP_EVENT_PACKET);
}

static void timer_test_pkt_event_sched(void)
{
	timer_test_event_type(ODP_QUEUE_TYPE_SCHED, ODP_EVENT_PACKET);
}

static void timer_test_queue_type(odp_queue_type_t queue_type, int priv, int exp_relax)
{
	odp_pool_t pool;
	const int num = 10;
	odp_timeout_t tmo;
	odp_event_t ev;
	odp_queue_param_t queue_param;
	odp_timer_pool_param_t tparam;
	odp_timer_pool_t tp;
	odp_queue_t queue;
	odp_timer_t tim;
	int i, ret, num_tmo;
	uint64_t tick_base, tick, nsec_base, nsec;
	uint64_t res_ns, period_ns, period_tick, test_period;
	uint64_t diff_test;
	odp_pool_param_t params;
	odp_time_t t0, t1;
	odp_timer_t timer[num];
	uint64_t target_tick[num];
	uint64_t target_nsec[num];
	void *user_ptr[num];

	odp_pool_param_init(&params);
	params.type    = ODP_POOL_TIMEOUT;
	params.tmo.num = num;

	pool = odp_pool_create("timeout_pool", &params);
	CU_ASSERT_FATAL(pool != ODP_POOL_INVALID);

	res_ns = global_mem->param.res_ns;

	odp_timer_pool_param_init(&tparam);
	tparam.res_ns     = global_mem->param.res_ns;
	tparam.min_tmo    = global_mem->param.min_tmo;
	tparam.max_tmo    = global_mem->param.max_tmo;
	tparam.num_timers = num + 1;
	tparam.priv       = priv;
	tparam.clk_src    = ODP_CLOCK_DEFAULT;

	if (exp_relax)
		tparam.exp_mode = ODP_TIMER_EXP_RELAXED;

	ODPH_DBG("\nTimer pool parameters:\n");
	ODPH_DBG("  res_ns  %" PRIu64 "\n", tparam.res_ns);
	ODPH_DBG("  min_tmo %" PRIu64 "\n", tparam.min_tmo);
	ODPH_DBG("  max_tmo %" PRIu64 "\n", tparam.max_tmo);

	tp = odp_timer_pool_create("timer_pool", &tparam);
	if (tp == ODP_TIMER_POOL_INVALID)
		CU_FAIL_FATAL("Timer pool create failed");

	odp_timer_pool_start();

	odp_queue_param_init(&queue_param);
	if (queue_type == ODP_QUEUE_TYPE_SCHED) {
		queue_param.type = ODP_QUEUE_TYPE_SCHED;
		queue_param.sched.sync  = ODP_SCHED_SYNC_ATOMIC;
		queue_param.sched.group = ODP_SCHED_GROUP_ALL;
	}

	queue = odp_queue_create("timer_queue", &queue_param);
	if (queue == ODP_QUEUE_INVALID)
		CU_FAIL_FATAL("Queue create failed");

	period_ns   = 4 * tparam.min_tmo;
	period_tick = odp_timer_ns_to_tick(tp, period_ns);
	test_period = num * period_ns;

	ODPH_DBG("  period_ns %" PRIu64 "\n", period_ns);
	ODPH_DBG("  period_tick %" PRIu64 "\n\n", period_tick);

	tick_base = odp_timer_current_tick(tp);
	t0 = odp_time_local();
	t1 = t0;
	nsec_base = odp_time_to_ns(t0);

	for (i = 0; i < num; i++) {
		tmo = odp_timeout_alloc(pool);
		CU_ASSERT_FATAL(tmo != ODP_TIMEOUT_INVALID);
		ev  = odp_timeout_to_event(tmo);
		CU_ASSERT_FATAL(ev != ODP_EVENT_INVALID);

		user_ptr[i] = (void *)(uintptr_t)i;
		tim = odp_timer_alloc(tp, queue, user_ptr[i]);
		CU_ASSERT_FATAL(tim != ODP_TIMER_INVALID);
		timer[i] = tim;

		tick = tick_base + ((i + 1) * period_tick);
		ret = odp_timer_set_abs(tim, tick, &ev);
		target_tick[i] = tick;
		target_nsec[i] = nsec_base + ((i + 1) * period_ns);

		ODPH_DBG("abs timer tick %" PRIu64 "\n", tick);
		if (ret == ODP_TIMER_TOO_NEAR)
			ODPH_DBG("Timer set failed. Too near %" PRIu64 ".\n", tick);
		else if (ret == ODP_TIMER_TOO_FAR)
			ODPH_DBG("Timer set failed. Too far %" PRIu64 ".\n", tick);
		else if (ret == ODP_TIMER_FAIL)
			ODPH_DBG("Timer set failed %" PRIu64 "\n", tick);

		CU_ASSERT(ret == ODP_TIMER_SUCCESS);
	}

	num_tmo = 0;

	do {
		if (queue_type == ODP_QUEUE_TYPE_SCHED)
			ev = odp_schedule(NULL, ODP_SCHED_NO_WAIT);
		else
			ev = odp_queue_deq(queue);

		t1 = odp_time_local();
		nsec = odp_time_to_ns(t1);
		diff_test = nsec - nsec_base;

		if (ev != ODP_EVENT_INVALID) {
			uint64_t target;

			tmo = odp_timeout_from_event(ev);
			tim = odp_timeout_timer(tmo);
			tick = odp_timeout_tick(tmo);
			target = target_nsec[num_tmo];

			CU_ASSERT(timer[num_tmo] == tim);
			CU_ASSERT(target_tick[num_tmo] == tick);
			CU_ASSERT(user_ptr[num_tmo] == odp_timeout_user_ptr(tmo));

			CU_ASSERT(nsec < (target + (5 * res_ns)));

			if (exp_relax) {
				CU_ASSERT(nsec > (target - (5 * res_ns)));
			} else {
				/* Timeout must not arrive before the current time has passed
				 * the target time (in timer ticks). */
				CU_ASSERT(target_tick[num_tmo] <= odp_timer_current_tick(tp));

				/* Timeout should not arrive before the target wall clock time.
				 * However, allow small drift or error between wall clock and
				 * timer. */
				CU_ASSERT(nsec > (target - res_ns));
			}

			ODPH_DBG("timeout tick %" PRIu64 ", nsec %" PRIu64 ", "
				 "target nsec %" PRIu64 ", diff nsec %" PRIi64 "\n",
				 tick, nsec, target, (int64_t)(nsec - target));

			odp_timeout_free(tmo);
			CU_ASSERT(odp_timer_free(tim) == ODP_EVENT_INVALID);

			num_tmo++;
		}

	} while (diff_test < (2 * test_period) && num_tmo < num);

	ODPH_DBG("test period %" PRIu64 "\n", diff_test);

	CU_ASSERT(num_tmo == num);
	CU_ASSERT(diff_test > (test_period - period_ns));
	CU_ASSERT(diff_test < (test_period + period_ns));

	/* Reset scheduler context for the next test case */
	if (queue_type == ODP_QUEUE_TYPE_SCHED) {
		odp_schedule_pause();
		while (1) {
			ev = odp_schedule(NULL, ODP_SCHED_NO_WAIT);
			if (ev == ODP_EVENT_INVALID)
				break;

			CU_FAIL("Drop extra event\n");
			odp_event_free(ev);
		}
		odp_schedule_resume();
	}

	odp_timer_pool_destroy(tp);

	CU_ASSERT(odp_queue_destroy(queue) == 0);
	CU_ASSERT(odp_pool_destroy(pool) == 0);
}

static void timer_test_plain_queue(void)
{
	timer_test_queue_type(ODP_QUEUE_TYPE_PLAIN, 0, 0);
}

static void timer_test_sched_queue(void)
{
	timer_test_queue_type(ODP_QUEUE_TYPE_SCHED, 0, 0);
}

static void timer_test_plain_queue_priv(void)
{
	timer_test_queue_type(ODP_QUEUE_TYPE_PLAIN, PRIV, 0);
}

static void timer_test_sched_queue_priv(void)
{
	timer_test_queue_type(ODP_QUEUE_TYPE_SCHED, PRIV, 0);
}

static void timer_test_plain_queue_exp_relax(void)
{
	timer_test_queue_type(ODP_QUEUE_TYPE_PLAIN, 0, EXP_RELAX);
}

static void timer_test_sched_queue_exp_relax(void)
{
	timer_test_queue_type(ODP_QUEUE_TYPE_SCHED, 0, EXP_RELAX);
}

static void timer_test_cancel(void)
{
	odp_pool_t pool;
	odp_pool_param_t params;
	odp_timer_pool_param_t tparam;
	odp_queue_param_t queue_param;
	odp_timer_capability_t capa;
	odp_timer_pool_t tp;
	odp_queue_t queue;
	odp_timer_t tim;
	odp_event_t ev;
	odp_timeout_t tmo;
	odp_timer_set_t rc;
	uint64_t tick;
	int ret;

	memset(&capa, 0, sizeof(capa));
	ret = odp_timer_capability(ODP_CLOCK_DEFAULT, &capa);
	CU_ASSERT_FATAL(ret == 0);

	odp_pool_param_init(&params);
	params.type    = ODP_POOL_TIMEOUT;
	params.tmo.num = 1;

	pool = odp_pool_create("tmo_pool_for_cancel", &params);

	if (pool == ODP_POOL_INVALID)
		CU_FAIL_FATAL("Timeout pool create failed");

	odp_timer_pool_param_init(&tparam);
	tparam.res_ns	  = global_mem->param.res_ns;
	tparam.min_tmo    = global_mem->param.min_tmo;
	tparam.max_tmo    = global_mem->param.max_tmo;
	tparam.num_timers = 1;
	tparam.priv       = 0;
	tparam.clk_src    = ODP_CLOCK_DEFAULT;
	tp = odp_timer_pool_create(NULL, &tparam);
	if (tp == ODP_TIMER_POOL_INVALID)
		CU_FAIL_FATAL("Timer pool create failed");

	/* Start all created timer pools */
	odp_timer_pool_start();

	odp_queue_param_init(&queue_param);
	if (capa.queue_type_plain) {
		queue_param.type = ODP_QUEUE_TYPE_PLAIN;
	} else if (capa.queue_type_sched) {
		queue_param.type = ODP_QUEUE_TYPE_SCHED;
		queue_param.sched.sync = ODP_SCHED_SYNC_ATOMIC;
	}

	queue = odp_queue_create("timer_queue", &queue_param);
	if (queue == ODP_QUEUE_INVALID)
		CU_FAIL_FATAL("Queue create failed");

	tim = odp_timer_alloc(tp, queue, USER_PTR);
	if (tim == ODP_TIMER_INVALID)
		CU_FAIL_FATAL("Failed to allocate timer");
	ODPH_DBG("Timer handle: %" PRIu64 "\n", odp_timer_to_u64(tim));

	ev = odp_timeout_to_event(odp_timeout_alloc(pool));
	if (ev == ODP_EVENT_INVALID)
		CU_FAIL_FATAL("Failed to allocate timeout");

	tick = odp_timer_ns_to_tick(tp, tparam.max_tmo / 2);

	rc = odp_timer_set_rel(tim, tick, &ev);
	if (rc != ODP_TIMER_SUCCESS)
		CU_FAIL_FATAL("Failed to set timer (relative time)");

	ev = ODP_EVENT_INVALID;
	if (odp_timer_cancel(tim, &ev) != 0)
		CU_FAIL_FATAL("Failed to cancel timer (relative time)");

	if (ev == ODP_EVENT_INVALID)
		CU_FAIL_FATAL("Cancel did not return event");

	tmo = odp_timeout_from_event(ev);
	if (tmo == ODP_TIMEOUT_INVALID)
		CU_FAIL_FATAL("Cancel did not return timeout");
	ODPH_DBG("Timeout handle: %" PRIu64 "\n", odp_timeout_to_u64(tmo));

	if (odp_timeout_timer(tmo) != tim)
		CU_FAIL("Cancel invalid tmo.timer");

	if (odp_timeout_user_ptr(tmo) != USER_PTR)
		CU_FAIL("Cancel invalid tmo.user_ptr");

	odp_timeout_free(tmo);

	ev = odp_timer_free(tim);
	if (ev != ODP_EVENT_INVALID)
		CU_FAIL_FATAL("Free returned event");

	odp_timer_pool_destroy(tp);

	if (odp_queue_destroy(queue) != 0)
		CU_FAIL_FATAL("Failed to destroy queue");

	if (odp_pool_destroy(pool) != 0)
		CU_FAIL_FATAL("Failed to destroy pool");
}

static void timer_test_tmo_limit(odp_queue_type_t queue_type,
				 int max_res, int min)
{
	odp_timer_capability_t timer_capa;
	odp_pool_t pool;
	odp_pool_param_t pool_param;
	odp_queue_param_t queue_param;
	odp_timer_pool_param_t timer_param;
	odp_timer_pool_t timer_pool;
	odp_queue_t queue;
	odp_timeout_t tmo;
	odp_event_t ev;
	odp_time_t t1, t2;
	uint64_t res_ns, min_tmo, max_tmo;
	uint64_t tmo_ns, tmo_tick, diff_ns, max_wait;
	int i, ret, num_tmo;
	int num = 5;
	odp_timer_t timer[num];

	memset(&timer_capa, 0, sizeof(timer_capa));
	ret = odp_timer_capability(ODP_CLOCK_DEFAULT, &timer_capa);
	CU_ASSERT_FATAL(ret == 0);

	if (max_res) {
		/* Maximum resolution parameters */
		res_ns  = timer_capa.max_res.res_ns;
		min_tmo = timer_capa.max_res.min_tmo;
		max_tmo = timer_capa.max_res.max_tmo;
	} else {
		/* Maximum timeout parameters */
		res_ns  = timer_capa.max_tmo.res_ns;
		min_tmo = timer_capa.max_tmo.min_tmo;
		max_tmo = timer_capa.max_tmo.max_tmo;
	}

	odp_timer_pool_param_init(&timer_param);
	timer_param.res_ns     = res_ns;
	timer_param.min_tmo    = min_tmo;
	timer_param.max_tmo    = max_tmo;
	timer_param.num_timers = num;
	timer_param.clk_src    = ODP_CLOCK_DEFAULT;

	timer_pool = odp_timer_pool_create("timer_pool", &timer_param);
	if (timer_pool == ODP_TIMER_POOL_INVALID)
		CU_FAIL_FATAL("Timer pool create failed");

	odp_timer_pool_start();

	odp_pool_param_init(&pool_param);
	pool_param.type    = ODP_POOL_TIMEOUT;
	pool_param.tmo.num = num;

	pool = odp_pool_create("timeout_pool", &pool_param);
	CU_ASSERT_FATAL(pool != ODP_POOL_INVALID);

	odp_queue_param_init(&queue_param);
	if (queue_type == ODP_QUEUE_TYPE_SCHED) {
		queue_param.type       = ODP_QUEUE_TYPE_SCHED;
		queue_param.sched.sync = ODP_SCHED_SYNC_ATOMIC;
	}

	queue = odp_queue_create("timeout_queue", &queue_param);
	if (queue == ODP_QUEUE_INVALID)
		CU_FAIL_FATAL("Queue create failed");

	if (min)
		tmo_ns = min_tmo;
	else
		tmo_ns = max_tmo;

	tmo_tick    = odp_timer_ns_to_tick(timer_pool, tmo_ns);
	/* Min_tmo maybe zero. Wait min timeouts at least 20ms + resolution */
	max_wait    = (20 * ODP_TIME_MSEC_IN_NS + res_ns + 10 * tmo_ns);

	ODPH_DBG("\nTimer pool parameters:\n");
	ODPH_DBG("  res_ns      %" PRIu64 "\n",   timer_param.res_ns);
	ODPH_DBG("  min_tmo     %" PRIu64 "\n",   timer_param.min_tmo);
	ODPH_DBG("  max_tmo     %" PRIu64 "\n",   timer_param.max_tmo);
	ODPH_DBG("  tmo_ns      %" PRIu64 "\n",   tmo_ns);
	ODPH_DBG("  tmo_tick    %" PRIu64 "\n\n", tmo_tick);

	for (i = 0; i < num; i++) {
		timer[i] = odp_timer_alloc(timer_pool, queue, NULL);
		CU_ASSERT_FATAL(timer[i] != ODP_TIMER_INVALID);
	}

	num_tmo = 0;

	for (i = 0; i < num; i++) {
		tmo = odp_timeout_alloc(pool);
		ev  = odp_timeout_to_event(tmo);

		CU_ASSERT(ev != ODP_EVENT_INVALID);

		t1  = odp_time_local();
		ret = odp_timer_set_rel(timer[i], tmo_tick, &ev);

		if (ret == ODP_TIMER_TOO_NEAR)
			ODPH_DBG("Timer set failed. Too near %i.\n", i);
		else if (ret == ODP_TIMER_TOO_FAR)
			ODPH_DBG("Timer set failed. Too late %i.\n", i);
		else if (ret == ODP_TIMER_FAIL)
			ODPH_DBG("Timer set failed %i\n", i);

		CU_ASSERT(ret == ODP_TIMER_SUCCESS);

		if (min) {
			/* Min timeout - wait for events */
			int break_loop = 0;

			while (1) {
				if (queue_type == ODP_QUEUE_TYPE_SCHED)
					ev = odp_schedule(NULL,
							  ODP_SCHED_NO_WAIT);
				else
					ev = odp_queue_deq(queue);

				t2 = odp_time_local();
				diff_ns = odp_time_diff_ns(t2, t1);

				if (ev != ODP_EVENT_INVALID) {
					odp_event_free(ev);
					num_tmo++;
					break_loop = 1;
					ODPH_DBG("Timeout [%i]: %" PRIu64 " "
						 "nsec\n", i, diff_ns);
					continue;
				}

				/* Ensure that schedule context is free */
				if (break_loop)
					break;

				/* Give up after waiting max wait time */
				if (diff_ns > max_wait)
					break;
			}
		} else {
			/* Max timeout - cancel events */
			ev = ODP_EVENT_INVALID;

			ret = odp_timer_cancel(timer[i], &ev);
			t2 = odp_time_local();
			diff_ns = odp_time_diff_ns(t2, t1);

			CU_ASSERT(ret == 0);
			CU_ASSERT(ev != ODP_EVENT_INVALID);

			if (ev != ODP_EVENT_INVALID)
				odp_event_free(ev);

			ODPH_DBG("Cancelled [%i]: %" PRIu64 " nsec\n", i,
				 diff_ns);
		}
	}

	if (min)
		CU_ASSERT(num_tmo == num);

	for (i = 0; i < num; i++)
		CU_ASSERT(odp_timer_free(timer[i]) == ODP_EVENT_INVALID);

	odp_timer_pool_destroy(timer_pool);
	CU_ASSERT(odp_queue_destroy(queue) == 0);
	CU_ASSERT(odp_pool_destroy(pool) == 0);
}

static void timer_test_max_res_min_tmo_plain(void)
{
	timer_test_tmo_limit(ODP_QUEUE_TYPE_PLAIN, 1, 1);
}

static void timer_test_max_res_min_tmo_sched(void)
{
	timer_test_tmo_limit(ODP_QUEUE_TYPE_SCHED, 1, 1);
}

static void timer_test_max_res_max_tmo_plain(void)
{
	timer_test_tmo_limit(ODP_QUEUE_TYPE_PLAIN, 1, 0);
}

static void timer_test_max_res_max_tmo_sched(void)
{
	timer_test_tmo_limit(ODP_QUEUE_TYPE_SCHED, 1, 0);
}

static void timer_test_max_tmo_min_tmo_plain(void)
{
	timer_test_tmo_limit(ODP_QUEUE_TYPE_PLAIN, 0, 1);
}

static void timer_test_max_tmo_min_tmo_sched(void)
{
	timer_test_tmo_limit(ODP_QUEUE_TYPE_SCHED, 0, 1);
}

static void timer_test_max_tmo_max_tmo_plain(void)
{
	timer_test_tmo_limit(ODP_QUEUE_TYPE_PLAIN, 0, 0);
}

static void timer_test_max_tmo_max_tmo_sched(void)
{
	timer_test_tmo_limit(ODP_QUEUE_TYPE_SCHED, 0, 0);
}

/* Handle a received (timeout) event */
static void handle_tmo(odp_event_t ev, bool stale, uint64_t prev_tick)
{
	odp_event_subtype_t subtype;
	odp_timeout_t tmo;
	odp_timer_t tim;
	uint64_t tick;
	struct test_timer *ttp;

	CU_ASSERT_FATAL(ev != ODP_EVENT_INVALID); /* Internal error */
	if (odp_event_type(ev) != ODP_EVENT_TIMEOUT) {
		/* Not a timeout event */
		CU_FAIL("Unexpected event type received");
		return;
	}
	if (odp_event_subtype(ev) != ODP_EVENT_NO_SUBTYPE) {
		/* Not a timeout event */
		CU_FAIL("Unexpected event subtype received");
		return;
	}
	if (odp_event_types(ev, &subtype) != ODP_EVENT_TIMEOUT) {
		/* Not a timeout event */
		CU_FAIL("Unexpected event type received");
		return;
	}
	if (subtype != ODP_EVENT_NO_SUBTYPE) {
		/* Not a timeout event */
		CU_FAIL("Unexpected event subtype received");
		return;
	}

	/* Read the metadata from the timeout */
	tmo  = odp_timeout_from_event(ev);
	tim  = odp_timeout_timer(tmo);
	tick = odp_timeout_tick(tmo);
	ttp  = odp_timeout_user_ptr(tmo);

	if (tim == ODP_TIMER_INVALID)
		CU_FAIL("odp_timeout_timer() invalid timer");

	if (ttp == NULL) {
		CU_FAIL("odp_timeout_user_ptr() null user ptr");
		return;
	}

	if (ttp->ev2 != ev)
		CU_FAIL("odp_timeout_user_ptr() wrong user ptr");

	if (ttp->tim != tim)
		CU_FAIL("odp_timeout_timer() wrong timer");

	if (!stale) {
		if (!odp_timeout_fresh(tmo))
			CU_FAIL("Wrong status (stale) for fresh timeout");

		/* tmo tick cannot be smaller than pre-calculated tick */
		if (tick < ttp->tick) {
			ODPH_DBG("Too small tick: pre-calculated %" PRIu64 " "
				 "timeout %" PRIu64 "\n", ttp->tick, tick);
			CU_FAIL("odp_timeout_tick() too small tick");
		}

		if (tick > odp_timer_current_tick(global_mem->tp))
			CU_FAIL("Timeout delivered early in ODP_TIMER_EXP_AFTER mode");

		if (tick < prev_tick) {
			ODPH_DBG("Too late tick: %" PRIu64 " prev_tick "
				 "%" PRIu64 "\n", tick, prev_tick);
			/* We don't report late timeouts using CU_FAIL */
			odp_atomic_inc_u32(&global_mem->ndelivtoolate);
		}
	}

	/* Internal error */
	CU_ASSERT_FATAL(ttp->ev == ODP_EVENT_INVALID);
	ttp->ev = ev;
}

/* Worker thread entrypoint which performs timer alloc/set/cancel/free
 * tests */
static int worker_entrypoint(void *arg ODP_UNUSED)
{
	int thr = odp_thread_id();
	uint32_t i, allocated;
	unsigned seed = thr;
	int rc;
	odp_queue_t queue;
	struct test_timer *tt;
	uint32_t nset;
	uint64_t tck;
	uint32_t nrcv;
	uint32_t nreset;
	uint32_t ncancel;
	uint32_t ntoolate;
	uint32_t ms;
	uint64_t prev_tick, late_margin, nsec;
	odp_event_t ev;
	struct timespec ts;
	uint32_t nstale;
	odp_timer_set_t timer_rc;
	odp_timer_pool_t tp = global_mem->tp;
	odp_pool_t tbp = global_mem->tbp;
	uint32_t num_timers = global_mem->timers_per_thread;
	uint64_t min_tmo = global_mem->param.min_tmo;
	odp_queue_param_t queue_param;
	odp_thrmask_t thr_mask;
	odp_schedule_group_t group;
	uint64_t sched_tmo;
	uint64_t res_ns = global_mem->param.res_ns;
	odp_queue_type_t queue_type = global_mem->test_queue_type;

	odp_queue_param_init(&queue_param);
	queue_param.type = queue_type;

	if (queue_type == ODP_QUEUE_TYPE_SCHED) {
		odp_thrmask_zero(&thr_mask);
		odp_thrmask_set(&thr_mask, odp_thread_id());
		group = odp_schedule_group_create(NULL, &thr_mask);
		if (group == ODP_SCHED_GROUP_INVALID)
			CU_FAIL_FATAL("Schedule group create failed");

		queue_param.sched.sync = ODP_SCHED_SYNC_PARALLEL;
		queue_param.sched.group = group;
	}

	queue = odp_queue_create("timer_queue", &queue_param);
	if (queue == ODP_QUEUE_INVALID)
		CU_FAIL_FATAL("Queue create failed");

	tt = malloc(sizeof(struct test_timer) * num_timers);
	if (!tt)
		CU_FAIL_FATAL("malloc failed");

	/* Prepare all timers */
	for (i = 0; i < num_timers; i++) {
		tt[i].ev = odp_timeout_to_event(odp_timeout_alloc(tbp));
		if (tt[i].ev == ODP_EVENT_INVALID) {
			ODPH_DBG("Failed to allocate timeout ("
				 "%" PRIu32 "/%d)\n", i, num_timers);
			break;
		}
		tt[i].tim = odp_timer_alloc(tp, queue, &tt[i]);
		if (tt[i].tim == ODP_TIMER_INVALID) {
			ODPH_DBG("Failed to allocate timer (%" PRIu32 "/%d)\n",
				 i, num_timers);
			odp_event_free(tt[i].ev);
			break;
		}
		tt[i].ev2 = tt[i].ev;
		tt[i].tick = TICK_INVALID;
	}
	allocated = i;
	if (allocated == 0)
		CU_FAIL_FATAL("unable to alloc a timer");
	odp_atomic_fetch_add_u32(&global_mem->timers_allocated, allocated);

	odp_barrier_wait(&global_mem->test_barrier);

	/* Initial set all timers with a random expiration time */
	nset = 0;
	for (i = 0; i < allocated; i++) {
		nsec = min_tmo + THREE_POINT_THREE_MSEC +
		       (rand_r(&seed) % RANGE_MS) * 1000000ULL;
		tck = odp_timer_current_tick(tp) +
		      odp_timer_ns_to_tick(tp, nsec);
		timer_rc = odp_timer_set_abs(tt[i].tim, tck, &tt[i].ev);
		if (timer_rc == ODP_TIMER_TOO_NEAR) {
			ODPH_ERR("Missed tick, setting timer\n");
		} else if (timer_rc != ODP_TIMER_SUCCESS) {
			ODPH_ERR("Failed to set timer: %d\n", timer_rc);
			CU_FAIL("Failed to set timer");
		} else {
			tt[i].tick = tck;
			nset++;
		}
	}

	/* Step through wall time, 1ms at a time and check for expired timers */
	nrcv = 0;
	nreset = 0;
	ncancel = 0;
	ntoolate = 0;
	late_margin = odp_timer_ns_to_tick(tp, 2 * res_ns);
	prev_tick = odp_timer_current_tick(tp);

	for (ms = 0; ms < 7 * RANGE_MS / 10 && allocated > 0; ms++) {
		while ((ev = queue_type == ODP_QUEUE_TYPE_PLAIN ?
			odp_queue_deq(queue) :
			odp_schedule(NULL, ODP_SCHED_NO_WAIT))
				!= ODP_EVENT_INVALID) {
			/* Allow timeouts to be delivered late_margin ticks late */
			handle_tmo(ev, false, prev_tick - late_margin);
			nrcv++;
		}
		prev_tick = odp_timer_current_tick(tp);
		i = rand_r(&seed) % allocated;
		if (tt[i].ev == ODP_EVENT_INVALID &&
		    (rand_r(&seed) % 2 == 0)) {
			if (odp_timer_current_tick(tp) >= tt[i].tick)
				/* Timer just expired. */
				goto sleep;
			/* Timer active, cancel it */
			rc = odp_timer_cancel(tt[i].tim, &tt[i].ev);
			if (rc != 0) {
				/* Cancel failed, timer already expired */
				ntoolate++;
				ODPH_DBG("Failed to cancel timer, probably already expired\n");
			} else {
				tt[i].tick = TICK_INVALID;
				ncancel++;
			}
		} else {
			odp_timer_set_t rc;
			uint64_t cur_tick;
			uint64_t tck;

			if (tt[i].ev != ODP_EVENT_INVALID)
				/* Timer inactive => set */
				nset++;
			else if (odp_timer_current_tick(tp) >= tt[i].tick)
				/* Timer just expired. */
				goto sleep;
			else
				/* Timer active => reset */
				nreset++;

			nsec = min_tmo + THREE_POINT_THREE_MSEC +
			       (rand_r(&seed) % RANGE_MS) * 1000000ULL;
			tck  = odp_timer_ns_to_tick(tp, nsec);

			cur_tick = odp_timer_current_tick(tp);
			rc = odp_timer_set_rel(tt[i].tim, tck, &tt[i].ev);

			if (rc == ODP_TIMER_TOO_NEAR) {
				CU_FAIL("Failed to set timer: TOO NEAR");
			} else if (rc == ODP_TIMER_TOO_FAR) {
				CU_FAIL("Failed to set timer: TOO FAR");
			} else if (rc == ODP_TIMER_FAIL) {
				/* Set/reset failed, timer already expired */
				ntoolate++;
			} else if (rc == ODP_TIMER_SUCCESS) {
				/* Save expected expiration tick on success */
				tt[i].tick = cur_tick + tck;
				/* ODP timer owns the event now */
				tt[i].ev = ODP_EVENT_INVALID;
			} else {
				CU_FAIL("Failed to set timer: bad return code");
			}
		}
sleep:
		ts.tv_sec = 0;
		ts.tv_nsec = 1000000; /* 1ms */
		if (nanosleep(&ts, NULL) < 0)
			CU_FAIL_FATAL("nanosleep failed");
	}

	/* Cancel and free all timers */
	nstale = 0;
	for (i = 0; i < allocated; i++) {
		(void)odp_timer_cancel(tt[i].tim, &tt[i].ev);
		tt[i].tick = TICK_INVALID;
		if (tt[i].ev == ODP_EVENT_INVALID)
			/* Cancel too late, timer already expired and
			 * timeout enqueued */
			nstale++;
	}

	ODPH_DBG("Thread %u: %" PRIu32 " timers set\n", thr, nset);
	ODPH_DBG("Thread %u: %" PRIu32 " timers reset\n", thr, nreset);
	ODPH_DBG("Thread %u: %" PRIu32 " timers cancelled\n", thr, ncancel);
	ODPH_DBG("Thread %u: %" PRIu32 " timers reset/cancelled too late\n",
		 thr, ntoolate);
	ODPH_DBG("Thread %u: %" PRIu32 " timeouts received\n", thr, nrcv);
	ODPH_DBG("Thread %u: %" PRIu32 " "
		 "stale timeout(s) after odp_timer_cancel()\n", thr, nstale);

	/* Delay some more to ensure timeouts for expired timers can be
	 * received. Can not use busy loop here to make background timer
	 * thread finish their work. */
	ts.tv_sec = 0;
	ts.tv_nsec = (3 * RANGE_MS / 10 + 50) * ODP_TIME_MSEC_IN_NS;
	if (nanosleep(&ts, NULL) < 0)
		CU_FAIL_FATAL("nanosleep failed");

	sched_tmo = odp_schedule_wait_time(ODP_TIME_MSEC_IN_NS * RANGE_MS);
	while (nstale != 0) {
		if (queue_type == ODP_QUEUE_TYPE_PLAIN)
			ev = odp_queue_deq(queue);
		else
			ev = odp_schedule(NULL, sched_tmo);
		if (ev != ODP_EVENT_INVALID) {
			handle_tmo(ev, true, 0/*Don't care for stale tmo's*/);
			nstale--;
		} else {
			CU_FAIL("Failed to receive stale timeout");
			break;
		}
	}

	for (i = 0; i < allocated; i++) {
		if (odp_timer_free(tt[i].tim) != ODP_EVENT_INVALID)
			CU_FAIL("odp_timer_free");
	}

	/* Check if there any more (unexpected) events */
	if (queue_type == ODP_QUEUE_TYPE_PLAIN)
		ev = odp_queue_deq(queue);
	else
		ev = odp_schedule(NULL, sched_tmo);
	if (ev != ODP_EVENT_INVALID)
		CU_FAIL("Unexpected event received");

	rc = odp_queue_destroy(queue);
	CU_ASSERT(rc == 0);
	for (i = 0; i < allocated; i++) {
		if (tt[i].ev != ODP_EVENT_INVALID)
			odp_event_free(tt[i].ev);
	}

	free(tt);
	ODPH_DBG("Thread %u: exiting\n", thr);
	return CU_get_number_of_failures();
}

static void timer_test_all(odp_queue_type_t queue_type)
{
	int rc;
	odp_pool_param_t params;
	odp_timer_pool_param_t tparam;
	odp_timer_pool_info_t tpinfo;
	uint64_t ns, tick, ns2;
	uint64_t res_ns, min_tmo, max_tmo;
	uint32_t timers_allocated;
	odp_pool_capability_t pool_capa;
	odp_timer_capability_t timer_capa;
	odp_schedule_capability_t sched_capa;
	odp_pool_t tbp;
	odp_timer_pool_t tp;
	uint32_t num_timers;
	uint32_t num_workers;
	int timers_per_thread;

	CU_ASSERT_FATAL(odp_schedule_capability(&sched_capa) == 0);
	/* Reserve at least one core for running other processes so the timer
	 * test hopefully can run undisturbed and thus get better timing
	 * results. */
	num_workers = odp_cpumask_default_worker(NULL, 0);

	/* force to max CPU count */
	if (num_workers > MAX_WORKERS)
		num_workers = MAX_WORKERS;

	if (queue_type == ODP_QUEUE_TYPE_SCHED &&
	    num_workers > sched_capa.max_groups)
		num_workers = sched_capa.max_groups;

	/* On a single-CPU machine run at least one thread */
	if (num_workers < 1)
		num_workers = 1;

	num_timers = num_workers * NTIMERS;
	CU_ASSERT_FATAL(!odp_timer_capability(ODP_CLOCK_DEFAULT, &timer_capa));
	if (timer_capa.max_timers && timer_capa.max_timers < num_timers)
		num_timers = timer_capa.max_timers;

	CU_ASSERT_FATAL(!odp_pool_capability(&pool_capa));
	if (pool_capa.tmo.max_num && num_timers > pool_capa.tmo.max_num)
		num_timers = pool_capa.tmo.max_num;

	/* Create timeout pools */
	odp_pool_param_init(&params);
	params.type    = ODP_POOL_TIMEOUT;
	params.tmo.num = num_timers;

	timers_per_thread = (num_timers / num_workers) - EXTRA_TIMERS;
	global_mem->timers_per_thread = timers_per_thread > 1 ?
						timers_per_thread : 1;

	global_mem->tbp = odp_pool_create("tmo_pool", &params);
	if (global_mem->tbp == ODP_POOL_INVALID)
		CU_FAIL_FATAL("Timeout pool create failed");
	tbp = global_mem->tbp;

	/* Create a timer pool */
	res_ns  = global_mem->param.res_ns;
	max_tmo = global_mem->param.max_tmo;
	min_tmo = global_mem->param.min_tmo;

	odp_timer_pool_param_init(&tparam);
	tparam.res_ns  = res_ns;
	tparam.min_tmo = min_tmo;
	tparam.max_tmo = max_tmo;
	tparam.num_timers = num_timers;
	tparam.priv = 0;
	tparam.clk_src = ODP_CLOCK_DEFAULT;
	global_mem->tp = odp_timer_pool_create(NAME, &tparam);
	if (global_mem->tp == ODP_TIMER_POOL_INVALID)
		CU_FAIL_FATAL("Timer pool create failed");
	tp = global_mem->tp;

	/* Start all created timer pools */
	odp_timer_pool_start();

	if (odp_timer_pool_info(tp, &tpinfo) != 0)
		CU_FAIL("odp_timer_pool_info");
	CU_ASSERT(strcmp(tpinfo.name, NAME) == 0);
	CU_ASSERT(tpinfo.param.res_ns  == res_ns);
	CU_ASSERT(tpinfo.param.min_tmo == min_tmo);
	CU_ASSERT(tpinfo.param.max_tmo == max_tmo);
	CU_ASSERT(strcmp(tpinfo.name, NAME) == 0);

	ODPH_DBG("Timer pool handle: %" PRIu64 "\n", odp_timer_pool_to_u64(tp));
	ODPH_DBG("Resolution:   %" PRIu64 "\n", tparam.res_ns);
	ODPH_DBG("Min timeout:  %" PRIu64 "\n", tparam.min_tmo);
	ODPH_DBG("Max timeout:  %" PRIu64 "\n", tparam.max_tmo);
	ODPH_DBG("Num timers:   %u\n", tparam.num_timers);
	ODPH_DBG("Tmo range:    %u ms (%" PRIu64 " ticks)\n", RANGE_MS,
		 odp_timer_ns_to_tick(tp, 1000000ULL * RANGE_MS));
	ODPH_DBG("Max timers:   %" PRIu32 "\n", timer_capa.max_timers);
	ODPH_DBG("Max timer pools:       %" PRIu32 "\n", timer_capa.max_pools);
	ODPH_DBG("Max timer pools combined: %" PRIu32 "\n",
		 timer_capa.max_pools_combined);

	tick = odp_timer_ns_to_tick(tp, 0);
	CU_ASSERT(tick == 0);
	ns2  = odp_timer_tick_to_ns(tp, tick);
	CU_ASSERT(ns2 == 0);

	for (ns = res_ns; ns < max_tmo; ns += res_ns) {
		tick = odp_timer_ns_to_tick(tp, ns);
		ns2  = odp_timer_tick_to_ns(tp, tick);

		if (ns2 < ns - res_ns) {
			ODPH_DBG("FAIL ns:%" PRIu64 " tick:%" PRIu64 " ns2:"
				 "%" PRIu64 "\n", ns, tick, ns2);
			CU_FAIL("tick conversion: nsec too small\n");
		}

		if (ns2 > ns + res_ns) {
			ODPH_DBG("FAIL ns:%" PRIu64 " tick:%" PRIu64 " ns2:"
				 "%" PRIu64 "\n", ns, tick, ns2);
			CU_FAIL("tick conversion: nsec too large\n");
		}
	}

	/* Initialize barrier used by worker threads for synchronization */
	odp_barrier_init(&global_mem->test_barrier, num_workers);

	/* Initialize the shared timeout counter */
	odp_atomic_init_u32(&global_mem->ndelivtoolate, 0);

	/* Initialize the number of finally allocated elements */
	odp_atomic_init_u32(&global_mem->timers_allocated, 0);

	/* Create and start worker threads */
	global_mem->test_queue_type = queue_type;
	odp_cunit_thread_create(num_workers, worker_entrypoint, NULL, 0);

	/* Wait for worker threads to exit */
	odp_cunit_thread_join(num_workers);
	ODPH_DBG("Number of timeouts delivered/received too late: "
		 "%" PRIu32 "\n",
		 odp_atomic_load_u32(&global_mem->ndelivtoolate));

	/* Check some statistics after the test */
	if (odp_timer_pool_info(tp, &tpinfo) != 0)
		CU_FAIL("odp_timer_pool_info");
	CU_ASSERT(tpinfo.param.num_timers == num_timers);
	CU_ASSERT(tpinfo.cur_timers == 0);
	timers_allocated = odp_atomic_load_u32(&global_mem->timers_allocated);
	CU_ASSERT(tpinfo.hwm_timers == timers_allocated);

	/* Destroy timer pool, all timers must have been freed */
	odp_timer_pool_destroy(tp);

	/* Destroy timeout pool, all timeouts must have been freed */
	rc = odp_pool_destroy(tbp);
	CU_ASSERT(rc == 0);
}

static void timer_test_plain_all(void)
{
	timer_test_all(ODP_QUEUE_TYPE_PLAIN);
}

static void timer_test_sched_all(void)
{
	timer_test_all(ODP_QUEUE_TYPE_SCHED);
}

static void timer_test_periodic(odp_queue_type_t queue_type, int use_first)
{
	odp_timer_capability_t timer_capa;
	odp_timer_periodic_capability_t periodic_capa;
	odp_pool_t pool;
	odp_pool_param_t pool_param;
	odp_queue_param_t queue_param;
	odp_timer_pool_param_t timer_param;
	odp_timer_pool_t timer_pool;
	odp_timer_periodic_start_t start_param;
	odp_queue_t queue;
	odp_timeout_t tmo;
	odp_event_t ev;
	odp_timer_t timer;
	odp_time_t t1, t2;
	uint64_t tick, cur_tick, period_ns, duration_ns, diff_ns;
	double freq, freq_out, min_freq, max_freq;
	int ret;
	const char *user_ctx = "User context";
	int num_tmo = 0;
	int done = 0;
	const int num = 200;
	/* Test frequency: 1x 100Hz, or 1x min/max_base_freq */
	const uint64_t multiplier = 1;
	odp_fract_u64_t base_freq = {100, 0, 0};

	memset(&timer_capa, 0, sizeof(odp_timer_capability_t));
	CU_ASSERT_FATAL(odp_timer_capability(ODP_CLOCK_DEFAULT, &timer_capa) == 0);

	CU_ASSERT_FATAL(timer_capa.periodic.max_pools);
	CU_ASSERT_FATAL(timer_capa.periodic.max_timers);
	CU_ASSERT_FATAL(timer_capa.periodic.min_base_freq_hz.integer ||
			timer_capa.periodic.min_base_freq_hz.numer);
	CU_ASSERT_FATAL(timer_capa.periodic.max_base_freq_hz.integer ||
			timer_capa.periodic.max_base_freq_hz.numer);

	min_freq = odp_fract_u64_to_dbl(&timer_capa.periodic.min_base_freq_hz);
	max_freq = odp_fract_u64_to_dbl(&timer_capa.periodic.max_base_freq_hz);
	CU_ASSERT(min_freq <= max_freq);

	if (odp_fract_u64_to_dbl(&base_freq) < min_freq)
		base_freq = timer_capa.periodic.min_base_freq_hz;
	else if (odp_fract_u64_to_dbl(&base_freq) > max_freq)
		base_freq = timer_capa.periodic.max_base_freq_hz;

	freq = odp_fract_u64_to_dbl(&base_freq);

	/* No resolution requirement */
	memset(&periodic_capa, 0, sizeof(odp_timer_periodic_capability_t));
	periodic_capa.base_freq_hz   = base_freq;
	periodic_capa.max_multiplier = multiplier;

	ret = odp_timer_periodic_capability(ODP_CLOCK_DEFAULT, &periodic_capa);
	CU_ASSERT(ret == 0 || ret == 1);

	if (ret < 0) {
		ODPH_ERR("Periodic timer does not support tested frequency\n");
		return;
	}

	freq_out = odp_fract_u64_to_dbl(&periodic_capa.base_freq_hz);

	if (ret == 0) {
		/* Allow 10% difference in outputted base frequency */
		CU_ASSERT((freq_out > (0.9 * freq)) && (freq_out < (1.1 * freq)));
	} else {
		CU_ASSERT(base_freq.integer == periodic_capa.base_freq_hz.integer);
		CU_ASSERT(base_freq.numer   == periodic_capa.base_freq_hz.numer);
		CU_ASSERT(base_freq.denom   == periodic_capa.base_freq_hz.denom);
	}

	CU_ASSERT(periodic_capa.res_ns > 0);
	CU_ASSERT(periodic_capa.max_multiplier >= multiplier);

	base_freq = periodic_capa.base_freq_hz;
	freq = odp_fract_u64_to_dbl(&base_freq);
	period_ns = ODP_TIME_SEC_IN_NS / (freq * multiplier);
	duration_ns = num * period_ns;

	odp_timer_pool_param_init(&timer_param);
	timer_param.timer_type = ODP_TIMER_TYPE_PERIODIC;
	timer_param.res_ns     = 2 * periodic_capa.res_ns;
	timer_param.num_timers = 1;
	timer_param.clk_src    = ODP_CLOCK_DEFAULT;
	timer_param.periodic.base_freq_hz = base_freq;
	timer_param.periodic.max_multiplier = multiplier;

	ODPH_DBG("\n");
	ODPH_DBG("Periodic timer pool create params:\n");
	ODPH_DBG("  Resolution ns:     %" PRIu64 "\n", timer_param.res_ns);
	ODPH_DBG("  Base freq hz:      %" PRIu64 " + %" PRIu64 "/%" PRIu64 " (%f)\n",
		 timer_param.periodic.base_freq_hz.integer,
		 timer_param.periodic.base_freq_hz.numer,
		 timer_param.periodic.base_freq_hz.denom, freq);
	ODPH_DBG("  Max multiplier:    %" PRIu64 "\n", timer_param.periodic.max_multiplier);
	ODPH_DBG("Capabilities:\n");
	ODPH_DBG("  Max multiplier:    %" PRIu64 " (with %f hz)\n",
		 periodic_capa.max_multiplier, freq);
	ODPH_DBG("  Max resolution:    %" PRIu64 " ns (with %f hz)\n", periodic_capa.res_ns, freq);
	ODPH_DBG("  Min base freq:     %f hz\n", min_freq);
	ODPH_DBG("  Max base freq:     %f hz\n", max_freq);

	timer_pool = odp_timer_pool_create("periodic_timer", &timer_param);
	CU_ASSERT_FATAL(timer_pool != ODP_TIMER_POOL_INVALID);

	odp_timer_pool_start();

	odp_pool_param_init(&pool_param);
	pool_param.type    = ODP_POOL_TIMEOUT;
	pool_param.tmo.num = 1;

	pool = odp_pool_create("timeout_pool", &pool_param);
	CU_ASSERT_FATAL(pool != ODP_POOL_INVALID);

	odp_queue_param_init(&queue_param);
	if (queue_type == ODP_QUEUE_TYPE_SCHED) {
		queue_param.type       = ODP_QUEUE_TYPE_SCHED;
		queue_param.sched.sync = ODP_SCHED_SYNC_ATOMIC;
	}

	queue = odp_queue_create("timeout_queue", &queue_param);
	CU_ASSERT_FATAL(queue != ODP_QUEUE_INVALID);

	tmo = odp_timeout_alloc(pool);
	ev  = odp_timeout_to_event(tmo);
	CU_ASSERT_FATAL(ev != ODP_EVENT_INVALID);

	timer = odp_timer_alloc(timer_pool, queue, user_ctx);
	CU_ASSERT_FATAL(timer != ODP_TIMER_INVALID);

	memset(&start_param, 0, sizeof(odp_timer_periodic_start_t));
	cur_tick = odp_timer_current_tick(timer_pool);
	tick = cur_tick + odp_timer_ns_to_tick(timer_pool, period_ns / 2);

	if (use_first)
		start_param.first_tick = tick;

	start_param.freq_multiplier = multiplier;
	start_param.tmo_ev = ev;

	ODPH_DBG("Periodic timer start:\n");
	ODPH_DBG("  Current tick:    %" PRIu64 "\n", cur_tick);
	ODPH_DBG("  First tick:      %" PRIu64 "\n", start_param.first_tick);
	ODPH_DBG("  Multiplier:      %" PRIu64 "\n", start_param.freq_multiplier);
	ODPH_DBG("Test duration ns:  %" PRIu64 "\n", duration_ns);

	ret = odp_timer_periodic_start(timer, &start_param);

	if (ret == ODP_TIMER_TOO_NEAR)
		ODPH_ERR("First tick too near\n");
	else if (ret == ODP_TIMER_TOO_FAR)
		ODPH_ERR("First tick too far\n");
	else if (ret == ODP_TIMER_FAIL)
		ODPH_ERR("Periodic timer start failed\n");

	CU_ASSERT_FATAL(ret == ODP_TIMER_SUCCESS);

	t1 = odp_time_local();

	/* Wait for timeouts. Make sure that scheduler context is not held when
	 * exiting the loop. */
	while (1) {
		if (queue_type == ODP_QUEUE_TYPE_SCHED)
			ev = odp_schedule(NULL, ODP_SCHED_NO_WAIT);
		else
			ev = odp_queue_deq(queue);

		if (ev == ODP_EVENT_INVALID) {
			t2 = odp_time_local();
			diff_ns = odp_time_diff_ns(t2, t1);
			if (diff_ns > (10 * duration_ns))
				break;

			if (num_tmo >= num)
				break;

			continue;
		}

		CU_ASSERT(odp_event_type(ev) == ODP_EVENT_TIMEOUT);

		if (odp_event_type(ev) != ODP_EVENT_TIMEOUT) {
			odp_event_free(ev);
			continue;
		}

		CU_ASSERT(odp_timer_periodic_ack(timer, ev) == 0);
		num_tmo++;
	}

	CU_ASSERT(num_tmo == num);

	/* Allow +-30% error on test duration */
	CU_ASSERT((diff_ns > 0.7 * duration_ns) && (diff_ns < 1.3 * duration_ns));

	/* Stop periodic timer */
	ret = odp_timer_periodic_cancel(timer);
	CU_ASSERT_FATAL(ret == 0);

	t1 = odp_time_local();
	while (1) {
		if (queue_type == ODP_QUEUE_TYPE_SCHED)
			ev = odp_schedule(NULL, ODP_SCHED_NO_WAIT);
		else
			ev = odp_queue_deq(queue);

		if (ev == ODP_EVENT_INVALID) {
			t2 = odp_time_local();
			diff_ns = odp_time_diff_ns(t2, t1);
			if (diff_ns > (10 * duration_ns))
				break;

			if (done)
				break;

			continue;
		}

		CU_ASSERT(odp_event_type(ev) == ODP_EVENT_TIMEOUT);

		if (odp_event_type(ev) != ODP_EVENT_TIMEOUT) {
			odp_event_free(ev);
			continue;
		}

		ret = odp_timer_periodic_ack(timer, ev);
		CU_ASSERT(ret == 1 || ret == 2);

		if (ret == 2) {
			odp_event_free(ev);
			done = 1;
		}
	}

	/* Check that ack() returned 2 on the last event */
	CU_ASSERT(done);
	CU_ASSERT(ret == 2);

	CU_ASSERT(odp_timer_free(timer) == ODP_EVENT_INVALID);
	odp_timer_pool_destroy(timer_pool);
	CU_ASSERT(odp_queue_destroy(queue) == 0);
	CU_ASSERT(odp_pool_destroy(pool) == 0);
}

static void timer_test_periodic_sched(void)
{
	timer_test_periodic(ODP_QUEUE_TYPE_SCHED, 0);
}

static void timer_test_periodic_plain(void)
{
	timer_test_periodic(ODP_QUEUE_TYPE_PLAIN, 0);
}

static void timer_test_periodic_sched_first(void)
{
	timer_test_periodic(ODP_QUEUE_TYPE_SCHED, FIRST_TICK);
}

static void timer_test_periodic_plain_first(void)
{
	timer_test_periodic(ODP_QUEUE_TYPE_PLAIN, FIRST_TICK);
}

odp_testinfo_t timer_suite[] = {
	ODP_TEST_INFO(timer_test_capa),
	ODP_TEST_INFO(timer_test_param_init),
	ODP_TEST_INFO(timer_test_timeout_pool_alloc),
	ODP_TEST_INFO(timer_test_timeout_pool_free),
	ODP_TEST_INFO(timer_pool_create_destroy),
	ODP_TEST_INFO(timer_pool_create_max),
	ODP_TEST_INFO(timer_pool_max_res),
	ODP_TEST_INFO(timer_pool_tick_info),
	ODP_TEST_INFO_CONDITIONAL(timer_plain_rel_wait, check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_plain_abs_wait, check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_plain_rel_cancel, check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_plain_abs_cancel, check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_plain_rel_restart_wait, check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_plain_abs_restart_wait, check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_plain_rel_restart_cancel, check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_plain_abs_restart_cancel, check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_sched_rel_wait, check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_sched_abs_wait, check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_sched_rel_cancel, check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_sched_abs_cancel, check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_sched_rel_restart_wait, check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_sched_abs_restart_wait, check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_sched_rel_restart_cancel, check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_sched_abs_restart_cancel, check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_tmo_event_plain,
				  check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_tmo_event_sched,
				  check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_buf_event_plain,
				  check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_buf_event_sched,
				  check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_pkt_event_plain,
				  check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_pkt_event_sched,
				  check_sched_queue_support),
	ODP_TEST_INFO(timer_test_cancel),
	ODP_TEST_INFO_CONDITIONAL(timer_test_max_res_min_tmo_plain,
				  check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_max_res_min_tmo_sched,
				  check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_max_res_max_tmo_plain,
				  check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_max_res_max_tmo_sched,
				  check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_max_tmo_min_tmo_plain,
				  check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_max_tmo_min_tmo_sched,
				  check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_max_tmo_max_tmo_plain,
				  check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_max_tmo_max_tmo_sched,
				  check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_plain_queue,
				  check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_sched_queue,
				  check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_plain_queue_priv,
				  check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_sched_queue_priv,
				  check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_plain_queue_exp_relax,
				  check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_sched_queue_exp_relax,
				  check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_plain_all,
				  check_plain_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_sched_all,
				  check_sched_queue_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_periodic_sched,
				  check_periodic_sched_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_periodic_sched_first,
				  check_periodic_sched_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_periodic_plain,
				  check_periodic_plain_support),
	ODP_TEST_INFO_CONDITIONAL(timer_test_periodic_plain_first,
				  check_periodic_plain_support),
	ODP_TEST_INFO_NULL,
};

odp_suiteinfo_t timer_suites[] = {
	{"Timer", NULL, NULL, timer_suite},
	ODP_SUITE_INFO_NULL,
};

int main(int argc, char *argv[])
{
	/* parse common options: */
	if (odp_cunit_parse_options(argc, argv))
		return -1;

	odp_cunit_register_global_init(timer_global_init);
	odp_cunit_register_global_term(timer_global_term);

	int ret = odp_cunit_register(timer_suites);

	if (ret == 0)
		ret = odp_cunit_run();

	return ret;
}
