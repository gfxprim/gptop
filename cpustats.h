//SPDX-License-Identifier: GPL-2.0-or-later

/*

   Copyright (c) 2014-2021 Cyril Hrubis <metan@ucw.cz>

 */

#ifndef CPUSTATS_H__
#define CPUSTATS_H__

#include <stdio.h>

struct cpucnts {
	unsigned long long usr;
	unsigned long long nice;
	unsigned long long sys;
	unsigned long long idle;
	unsigned long long iowait;
	unsigned long long irq;
	unsigned long long softirq;
	unsigned long long steal;
	unsigned long long guest;
	unsigned long long guest_nice;
};

struct cpustats {
	struct cpucnts cnts[2];
	unsigned int cur_cnts;

	struct cpucnts diff;
	unsigned long long sum;
};

static inline int cpucnts_read(struct cpucnts *cnts)
{
	FILE *fp = fopen("/proc/stat", "r");
	int ret;

	if (!fp)
		return 0;

	ret = fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
	             &cnts->usr, &cnts->nice, &cnts->sys, &cnts->idle,
	             &cnts->iowait, &cnts->irq, &cnts->softirq, &cnts->steal,
	             &cnts->guest, &cnts->guest_nice);

	fclose(fp);

	return ret;
}

static inline void cpustats_init(struct cpustats *stats)
{
	cpucnts_read(&stats->cnts[0]);
	stats->cur_cnts = 0;
}

#define CPUCNTS_DIFF(STATS, CNT, NEW, OLD) STATS->diff.CNT = STATS->cnts[NEW].CNT - STATS->cnts[OLD].CNT

static inline void cpustats_update(struct cpustats *stats)
{
	unsigned int old_cnts = stats->cur_cnts;
	unsigned int cur_cnts = !stats->cur_cnts;

	cpucnts_read(&stats->cnts[cur_cnts]);

	CPUCNTS_DIFF(stats, usr, cur_cnts, old_cnts);
	CPUCNTS_DIFF(stats, nice, cur_cnts, old_cnts);
	CPUCNTS_DIFF(stats, sys, cur_cnts, old_cnts);
	CPUCNTS_DIFF(stats, idle, cur_cnts, old_cnts);

	/* iowait may jump back under some circumstances */
	if (stats->cnts[cur_cnts].iowait > stats->cnts[old_cnts].iowait)
		CPUCNTS_DIFF(stats, iowait, cur_cnts, old_cnts);
	else
		stats->diff.iowait = 0;

	CPUCNTS_DIFF(stats, irq, cur_cnts, old_cnts);
	CPUCNTS_DIFF(stats, softirq, cur_cnts, old_cnts);
	CPUCNTS_DIFF(stats, steal, cur_cnts, old_cnts);
	CPUCNTS_DIFF(stats, guest, cur_cnts, old_cnts);
	CPUCNTS_DIFF(stats, guest_nice, cur_cnts, old_cnts);

	stats->sum = stats->diff.usr + stats->diff.nice + \
	             stats->diff.sys + stats->diff.idle + \
	             stats->diff.iowait + stats->diff.irq + \
	             stats->diff.irq + stats->diff.softirq + \
	             stats->diff.steal + stats->diff.guest + \
	             stats->diff.guest_nice;

	stats->cur_cnts = cur_cnts;
}

#endif /* CPUSTATS_H__ */
