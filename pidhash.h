//SPDX-License-Identifier: GPL-2.0-or-later

/*

   Copyright (c) 2014-2021 Cyril Hrubis <metan@ucw.cz>

 */

#ifndef PIDHASH_H__
#define PIDHASH_H__

#include <sysinfo/read_proc.h>
#include <sysinfo/guid_map_cache.h>
#include <utils/gp_vec.h>
#include <utils/gp_htable2.h>

struct pid {
	struct read_proc_stat stat;
	int pcpu;
	uint64_t lcpu;
	int seen;
};

static gp_htable *pid_hash;
static struct pid **pid_table;

static inline void pidhash_init(void)
{
	pid_hash = gp_htable_new(0, 0);
	pid_table = gp_vec_new(0, sizeof(void *));
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

static inline struct pid *pidhash_lookup(unsigned int pid)
{
	struct pid *val;

	val = gp_htable_get2(pid_hash, pidhash_hash, pidhash_cmp, (void*)((uintptr_t)pid));
	if (val) {
		val->seen = 1;
		return val;
	}

	val = malloc(sizeof(*val));
	if (!val)
		return NULL;

	memset(val, 0, sizeof(*val));

	val->seen = 1;

	if (!GP_VEC_APPEND(pid_table, val)) {
		free(val);
		return NULL;
	}

	gp_htable_put2(pid_hash, pidhash_hash, val, (void*)((uintptr_t)pid));

	return val;
}

static inline int pidhash_should_trim(void *val)
{
	struct pid *pid = val;
	int ret = !pid->seen;

	pid->seen = 0;

	return ret;
}

static inline void pidhash_trim(void)
{
	size_t i;

	for (i = 0; i < gp_vec_len(pid_table); i++) {
		while (i < gp_vec_len(pid_table) && !pid_table[i]->seen)
			pid_table = gp_vec_move_shrink(pid_table, i);
	}

	gp_htable_trim2(pid_hash, pidhash_hash, pidhash_cmp, pidhash_should_trim, free);
}

static inline size_t pidhash_cnt(void)
{
	return gp_vec_len(pid_table);
}

#endif /* PIDHASH_H__ */
