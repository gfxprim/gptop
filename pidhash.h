//SPDX-License-Identifier: GPL-2.0-or-later

/*

   Copyright (c) 2014-2021 Cyril Hrubis <metan@ucw.cz>

 */

#ifndef PIDHASH_H__
#define PIDHASH_H__

#include <utils/gp_htable2.h>

struct pid {
	int pcpu;
	int seen;
};

struct pidhash {
	gp_htable *pids;
};

static inline void pidhash_init(struct pidhash *pidhash)
{
	pidhash->pids = gp_htable_new(0, 0);
}

static inline size_t pidhash_hash(const void *key, size_t htable_size)
{
	unsigned int pid = (uintptr_t)key;

	return (pid * 13) % htable_size;
}

static inline int pidhash_cmp(const void *key1, const void *key2)
{
	return key1 == key2;
}

static inline struct pid *pidhash_lookup(struct pidhash *pidhash, unsigned int pid)
{
	struct pid *val;

	val = gp_htable_get2(pidhash->pids, pidhash_hash, pidhash_cmp, (void*)((uintptr_t)pid));
	if (val) {
		val->seen = 1;
		return val;
	}

	val = malloc(sizeof(*val));
	if (!val)
		return NULL;

	memset(val, 0, sizeof(*val));

	val->seen = 1;

	gp_htable_put2(pidhash->pids, pidhash_hash, val, (void*)((uintptr_t)pid));

	return val;
}

static inline int pidhash_should_trim(void *val)
{
	struct pid *pid = val;
	int ret = !pid->seen;

	pid->seen = 0;

	return ret;
}

static inline void pidhash_trim(struct pidhash *pidhash)
{
	gp_htable_trim2(pidhash->pids, pidhash_hash, pidhash_cmp, pidhash_should_trim, free);
}

#endif /* PIDHASH_H__ */
