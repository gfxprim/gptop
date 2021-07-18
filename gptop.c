//SPDX-License-Identifier: GPL-2.0-or-later

/*

   Copyright (c) 2014-2021 Cyril Hrubis <metan@ucw.cz>

 */

#include <string.h>
#include <errno.h>
#include <proc/readproc.h>

#include <widgets/gp_widgets.h>

#include "pidhash.h"
#include "cpustats.h"

static proc_t **procs;
static unsigned int procs_cnt;

static gp_widget *plist, *tasks_total, *tasks_running, *tasks_sleeping,
                 *tasks_stopped, *tasks_zombie;

static gp_widget *cpus_usr, *cpus_sys, *cpus_nice,
		 *cpus_idle, *cpus_iowait, *cpus_steal;

static struct pidhash pidhash;

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

static int proc_get_elem(gp_widget *self, gp_widget_table_cell *cell, int col)
{
	static char buf[100];
	size_t page_size = getpagesize();

	cell->text = buf;
	cell->tattr = GP_TATTR_MONO;

	proc_t *p = procs[self->tbl->row_idx];

	switch (col) {
	case PID:
		snprintf(buf, sizeof(buf), "%i", p->tid);
		cell->tattr |= GP_TATTR_RIGHT;
	break;
	case USR:
		cell->tattr = GP_TATTR_LEFT;
		cell->text = p->euser;
	break;
	case CPU:
		snprintf(buf, sizeof(buf), "%.1f", 100.00 * p->pcpu / clk_ticks / (refresh_ms / 1000));
	break;
	case MEM:
		gp_str_file_size(buf, sizeof(buf), p->resident * page_size);
	break;
	case STATE:
		cell->tattr = GP_TATTR_CENTER;
		snprintf(buf, sizeof(buf), "%c", p->state);
	break;
	case CMD:
		cell->tattr = GP_TATTR_LEFT;
		cell->text = p->cmd;
	break;
	}

	if (p->state == 'R')
		cell->tattr |= GP_TATTR_BOLD;

	return 1;
}

int proc_get_pid(gp_widget *self, gp_widget_table_cell *cell)
{
	return proc_get_elem(self, cell, PID);
}

int proc_get_usr(gp_widget *self, gp_widget_table_cell *cell)
{
	return proc_get_elem(self, cell, USR);
}

int proc_get_cpu(gp_widget *self, gp_widget_table_cell *cell)
{
	return proc_get_elem(self, cell, CPU);
}

int proc_get_mem(gp_widget *self, gp_widget_table_cell *cell)
{
	return proc_get_elem(self, cell, MEM);
}

int proc_get_state(gp_widget *self, gp_widget_table_cell *cell)
{
	return proc_get_elem(self, cell, STATE);
}

int proc_get_cmd(gp_widget *self, gp_widget_table_cell *cell)
{
	return proc_get_elem(self, cell, CMD);
}

int proc_set_row(gp_widget *self, int op, unsigned int pos)
{
	switch (op) {
	case GP_TABLE_ROW_RESET:
		self->tbl->row_idx = 0;
	break;
	case GP_TABLE_ROW_ADVANCE:
		self->tbl->row_idx += pos;
	break;
	case GP_TABLE_ROW_TELL:
		return procs_cnt;
	break;
	}

	if (self->tbl->row_idx < procs_cnt)
		return 1;

	return 0;
}

#define MAKE_CMP_ASC_INT(NAME)                                        \
static int procs_cmp_##NAME##_asc(const void *ptr1, const void *ptr2) \
{                                                                     \
	const proc_t *p1 = *((proc_t**)ptr1);                         \
	const proc_t *p2 = *((proc_t**)ptr2);                         \
                                                                      \
	return p1->NAME - p2->NAME;                                   \
}

#define CMP_ASC(NAME) procs_cmp_##NAME##_asc

#define MAKE_CMP_DESC_INT(NAME)                                        \
static int procs_cmp_##NAME##_desc(const void *ptr1, const void *ptr2) \
{                                                                      \
	const proc_t *p1 = *((proc_t**)ptr1);                          \
	const proc_t *p2 = *((proc_t**)ptr2);                          \
                                                                       \
	return p2->NAME - p1->NAME;                                    \
}

#define CMP_DESC(NAME) procs_cmp_##NAME##_desc

MAKE_CMP_ASC_INT(tid);
MAKE_CMP_DESC_INT(tid);

MAKE_CMP_ASC_INT(pcpu);
MAKE_CMP_DESC_INT(pcpu);

MAKE_CMP_ASC_INT(resident);
MAKE_CMP_DESC_INT(resident);

MAKE_CMP_ASC_INT(state);
MAKE_CMP_DESC_INT(state);

static int (*cmps_asc[])(const void *, const void *) = {
	[PID] = CMP_ASC(tid),
	[CPU] = CMP_ASC(pcpu),
	[MEM] = CMP_ASC(resident),
	[STATE] = CMP_ASC(state),
};

static int (*cmps_desc[])(const void *, const void *) = {
	[PID] = CMP_DESC(tid),
	[CPU] = CMP_DESC(pcpu),
	[MEM] = CMP_DESC(resident),
	[STATE] = CMP_DESC(state),
};

static int (*procs_cmp)(const void *, const void *);

static void proc_sort(unsigned int col, int desc)
{
	if (desc)
		procs_cmp = cmps_desc[col];
	else
		procs_cmp = cmps_asc[col];
}

void proc_sort_by_cpu(gp_widget *self, int desc)
{
	(void) self;

	proc_sort(CPU, desc);
}

void proc_sort_by_pid(gp_widget *self, int desc)
{
	(void) self;

	proc_sort(PID, desc);
}

void proc_sort_by_mem(gp_widget *self, int desc)
{
	(void) self;

	proc_sort(MEM, desc);
}

void proc_sort_by_state(gp_widget *self, int desc)
{
	(void) self;

	proc_sort(MEM, desc);
}

static void sort_procs(void)
{
	if (!procs_cmp)
		return;

	qsort(procs, procs_cnt, sizeof(proc_t *), procs_cmp);
}

static void load_procs(void)
{
	unsigned int running = 0;
	unsigned int sleeping = 0;
	unsigned int stopped = 0;
	unsigned int zombie = 0;

	proc_t **p;

	procs_cnt = 0;

	procs = readproctab(PROC_FILLCOM | PROC_FILLSTAT | PROC_FILLMEM | PROC_FILLUSR);
	if (!procs)
		return;

	for (p = procs; *p; p++) {
		proc_t *pp = *p;
		int pcpu;

		struct pid *pid = pidhash_lookup(&pidhash, pp->tid);

		if (pid) {
			pcpu = pp->utime + pp->stime;

			if (pid->pcpu)
				pp->pcpu = pcpu - pid->pcpu;
			else
				pp->pcpu = 0;

			pid->pcpu = pcpu;
		}

		procs_cnt++;

		switch (pp->state) {
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

	pidhash_trim(&pidhash);

	sort_procs();

	if (tasks_total)
		gp_widget_label_printf(tasks_total, "%u", procs_cnt);

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
	proc_t **p;

	for (p = procs; *p; p++)
		freeproc(*p);

	free(procs);

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
	pidhash_init(&pidhash);

	load_procs();
	if (!procs)
		return 0;

	clk_ticks = sysconf(_SC_CLK_TCK);

	gp_widgets_timer_ins(&refresh_timer);

	gp_widgets_main_loop(layout, "gptop", NULL, argc, argv);

	return 0;
}
