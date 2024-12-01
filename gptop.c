//SPDX-License-Identifier: GPL-2.0-or-later

/*

   Copyright (c) 2021-2023 Cyril Hrubis <metan@ucw.cz>

 */

#include <string.h>
#include <errno.h>

#include <widgets/gp_widgets.h>

#include "pidhash.h"
#include "cpustats.h"

static gp_widget *plist, *tasks_total, *tasks_running, *tasks_sleeping,
                 *tasks_stopped, *tasks_zombie;

static gp_widget *cpus_usr, *cpus_sys, *cpus_nice,
		 *cpus_idle, *cpus_iowait, *cpus_steal;

static struct cpustats cpustats;

/* this is the multiplier for utime and stime */
static long clk_ticks;

/* refresh timer in miliseconds */
static int refresh_ms = 2000;

enum elem {
	PID,
	USR,
	CPU,
	MEM,
	STATE,
	CMD,
};

static int procs_get_cell(gp_widget *self, gp_widget_table_cell *cell, unsigned int col)
{
	static char buf[100];
	size_t page_size = getpagesize();
        gp_widget_table_priv *tbl_priv = gp_widget_table_priv_get(self);

	cell->text = buf;
	cell->tattr = GP_TATTR_MONO;

	struct pid *p = pid_table[tbl_priv->row_idx];

	switch (col) {
	case PID:
		snprintf(buf, sizeof(buf), "%i", p->stat.pid);
		cell->tattr |= GP_TATTR_RIGHT;
	break;
	case USR:
		cell->tattr = GP_TATTR_LEFT;
		cell->text = uid_map_get(p->stat.euid);
	break;
	case CPU:
		snprintf(buf, sizeof(buf), "%.1f", 100.00 * p->pcpu / clk_ticks / (refresh_ms / 1000));
	break;
	case MEM:
		gp_str_file_size(buf, sizeof(buf), p->stat.rss * page_size);
	break;
	case STATE:
		cell->tattr = GP_TATTR_CENTER;
		snprintf(buf, sizeof(buf), "%c", p->stat.state);
	break;
	case CMD:
		cell->tattr = GP_TATTR_LEFT;
		cell->text = p->stat.comm;
	break;
	}

	if (p->stat.state == 'R')
		cell->tattr |= GP_TATTR_BOLD;

	return 1;
}

static int procs_seek_row(gp_widget *self, int op, unsigned int pos)
{
	gp_widget_table_priv *tbl_priv = gp_widget_table_priv_get(self);

	switch (op) {
	case GP_TABLE_ROW_RESET:
		tbl_priv->row_idx = 0;
	break;
	case GP_TABLE_ROW_ADVANCE:
		tbl_priv->row_idx += pos;
	break;
	case GP_TABLE_ROW_MAX:
		return pidhash_cnt();
	break;
	}

	if (tbl_priv->row_idx < pidhash_cnt())
		return 1;

	return 0;
}

#define MAKE_CMP_ASC_INT(NAME, MEMB)                                  \
static int procs_cmp_##NAME##_asc(const void *ptr1, const void *ptr2) \
{                                                                     \
	const struct pid *p1 = *((struct pid**)ptr1);                 \
	const struct pid *p2 = *((struct pid**)ptr2);                 \
                                                                      \
	return p1->MEMB - p2->MEMB;                                   \
}

#define CMP_ASC(NAME) procs_cmp_##NAME##_asc

#define MAKE_CMP_DESC_INT(NAME, MEMB)                                  \
static int procs_cmp_##NAME##_desc(const void *ptr1, const void *ptr2) \
{                                                                      \
	const struct pid *p1 = *((struct pid**)ptr1);                  \
	const struct pid *p2 = *((struct pid**)ptr2);                  \
                                                                       \
	return p2->MEMB - p1->MEMB;                                    \
}

#define CMP_DESC(NAME) procs_cmp_##NAME##_desc

MAKE_CMP_ASC_INT(pid, stat.pid);
MAKE_CMP_DESC_INT(pid, stat.pid);

MAKE_CMP_ASC_INT(pcpu, pcpu);
MAKE_CMP_DESC_INT(pcpu, pcpu);

MAKE_CMP_ASC_INT(rss, stat.rss);
MAKE_CMP_DESC_INT(rss, stat.rss);

MAKE_CMP_ASC_INT(state, stat.state);
MAKE_CMP_DESC_INT(state, stat.state);

static int (*cmps_asc[])(const void *, const void *) = {
	[PID] = CMP_ASC(pid),
	[CPU] = CMP_ASC(pcpu),
	[MEM] = CMP_ASC(rss),
	[STATE] = CMP_ASC(state),
};

static int (*cmps_desc[])(const void *, const void *) = {
	[PID] = CMP_DESC(pid),
	[CPU] = CMP_DESC(pcpu),
	[MEM] = CMP_DESC(rss),
	[STATE] = CMP_DESC(state),
};

static int (*procs_cmp)(const void *, const void *);

static void procs_sort(gp_widget *self, int desc, unsigned int col)
{
	(void) self;

	if (desc)
		procs_cmp = cmps_desc[col];
	else
		procs_cmp = cmps_asc[col];
}

gp_widget_table_col_ops procs_ops = {
	.sort = procs_sort,
	.seek_row = procs_seek_row,
	.get_cell = procs_get_cell,
	.col_map = {
		{.id = "pid", .idx = PID, .sortable = 1},
		{.id = "usr", .idx = USR},
		{.id = "cpu", .idx = CPU, .sortable = 1},
		{.id = "mem", .idx = MEM, .sortable = 1},
		{.id = "state", .idx = STATE, .sortable = 1},
		{.id = "cmd", .idx = CMD},
		{}
	}
};

static void sort_procs(void)
{
	if (!procs_cmp)
		return;

	qsort(pid_table, pidhash_cnt(), sizeof(struct pid *), procs_cmp);
}

static void load_procs(void)
{
	unsigned int running = 0;
	unsigned int sleeping = 0;
	unsigned int stopped = 0;
	unsigned int zombie = 0;

	struct read_proc proc;

	read_proc_init(&proc);

	while (read_proc_next(&proc)) {
		struct pid *pid = pidhash_lookup(proc.pid);

		if (pid) {
			/*
			 * If process was removed while being parsed we marked
			 * it for removal.
			 */
			if (read_proc_stat(&proc, &pid->stat)) {
				printf("REMOVING PID %i\n", pid->stat.pid);
				pid->seen = 0;
				pid->stat.pid = -1000;
			}

			uint64_t pcpu = pid->stat.utime + pid->stat.stime;

			if (pid->lcpu)
				pid->pcpu = pcpu - pid->lcpu;
			else
				pid->pcpu = pcpu;

			pid->lcpu = pcpu;

			switch (pid->stat.state) {
			case 'T':
			case 't':
				stopped++;
			break;
			case 'R':
				running++;
			break;
			case 'Z':
				zombie++;
			break;
			default:
				sleeping++;
			}
		}
	}

	read_proc_exit(&proc);

	pidhash_trim();

	sort_procs();

	if (tasks_total)
		gp_widget_label_printf(tasks_total, "%zu", pidhash_cnt());

	if (tasks_running)
		gp_widget_label_printf(tasks_running, "%u", running);

	if (tasks_sleeping)
		gp_widget_label_printf(tasks_sleeping, "%u", sleeping);

	if (tasks_stopped)
		gp_widget_label_printf(tasks_stopped, "%u", stopped);

	if (tasks_zombie)
		gp_widget_label_printf(tasks_zombie, "%u", zombie);
}

static void update_cpustats(void)
{
	cpustats_update(&cpustats);

	if (cpus_usr)
		gp_widget_label_printf(cpus_usr, "%.1f", 100.00 * cpustats.diff.usr / cpustats.sum);

	if (cpus_sys)
		gp_widget_label_printf(cpus_sys, "%.1f", 100.00 * cpustats.diff.sys / cpustats.sum);

	if (cpus_nice)
		gp_widget_label_printf(cpus_nice, "%.1f", 100.00 * cpustats.diff.nice / cpustats.sum);

	if (cpus_idle)
		gp_widget_label_printf(cpus_idle, "%.1f", 100.00 * cpustats.diff.idle / cpustats.sum);

	if (cpus_iowait)
		gp_widget_label_printf(cpus_iowait, "%.1f", 100.00 * cpustats.diff.iowait / cpustats.sum);

	if (cpus_steal)
		gp_widget_label_printf(cpus_steal, "%.1f", 100.00 * cpustats.diff.steal / cpustats.sum);
}

static uint32_t refresh_callback(gp_timer *self)
{
	(void) self;

	load_procs();

	update_cpustats();

	gp_widget_redraw(plist);

	return refresh_ms;
}

static gp_timer refresh_timer = {
	.expires = 2000,
	.callback = refresh_callback,
	.id = "Refresh",
};

gp_app_info app_info = {
	.name = "gptop",
	.desc = "A top like application",
	.version = "1.0",
	.license = "GPL-2.0-or-later",
	.url = "http://github.com/gfxprim/elecalc",
	.authors = (gp_app_info_author []) {
		{.name = "Cyril Hrubis", .email = "metan@ucw.cz", .years = "2021-2023"},
		{}
	}
};

int main(int argc, char *argv[])
{
	gp_htable *uids;

	gp_widget *layout = gp_app_layout_load("gptop", &uids);
	if (!layout)
		return 0;

	plist = gp_widget_by_uid(uids, "proc_list", GP_WIDGET_TABLE);
	tasks_total = gp_widget_by_uid(uids, "tasks_total", GP_WIDGET_LABEL);
	tasks_running = gp_widget_by_uid(uids, "tasks_running", GP_WIDGET_LABEL);
	tasks_sleeping = gp_widget_by_uid(uids, "tasks_sleeping", GP_WIDGET_LABEL);
	tasks_stopped = gp_widget_by_uid(uids, "tasks_stopped", GP_WIDGET_LABEL);
	tasks_zombie = gp_widget_by_uid(uids, "tasks_zombie", GP_WIDGET_LABEL);

	cpus_usr = gp_widget_by_uid(uids, "cpus_usr", GP_WIDGET_LABEL);
	cpus_sys = gp_widget_by_uid(uids, "cpus_sys", GP_WIDGET_LABEL);
	cpus_nice = gp_widget_by_uid(uids, "cpus_nice", GP_WIDGET_LABEL);
	cpus_idle = gp_widget_by_uid(uids, "cpus_idle", GP_WIDGET_LABEL);
	cpus_iowait = gp_widget_by_uid(uids, "cpus_iowait", GP_WIDGET_LABEL);
	cpus_steal = gp_widget_by_uid(uids, "cpus_steal", GP_WIDGET_LABEL);

	cpustats_init(&cpustats);
	pidhash_init();

	load_procs();

	clk_ticks = sysconf(_SC_CLK_TCK);

	gp_widgets_timer_ins(&refresh_timer);

	gp_widgets_main_loop(layout, NULL, argc, argv);

	return 0;
}
