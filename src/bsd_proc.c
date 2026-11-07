/* SPDX-License-Identifier: GPL-2.0-or-later

Copyright (C) 2020-2026  Boian Bonev
Copyright (C) 2026  iotop FreeBSD port

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

/*
 * bsd_proc.c - the FreeBSD task list.
 *
 * One kern.proc.all sysctl per cycle replaces the /proc walk and the taskstats
 * queries of the Linux build: it returns every thread with the data iotop
 * needs (uid, nice, scheduling class, start time, comm, thread name). The
 * array is kept for the whole cycle, because make_stats() asks for the data of
 * a task that was just enumerated.
 *
 * One difference has to be hidden here. iotop identifies a task by the pair
 * (pid, tid) and relies on the tid of the main thread being equal to the pid,
 * which is how Linux numbers them; it uses that to attach the threads to their
 * process (arr_find(a,s->pid)) and to filter with --pid. FreeBSD gives every
 * thread its own small id from a separate counter, so a process leader has
 * tid 100123 and pid 812. The task list therefore reports the pid as the tid
 * of the thread that was created first, and keeps the kernel tid aside for the
 * byte counters, which are keyed by it.
 */

#include "iotop.h"

#ifdef __FreeBSD__

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bsd_task {
	struct kinfo_proc *kp;
	pid_t rtid; // the tid that iotop sees; the pid for the first thread of a process
};

struct bsd_proc_ent {
	pid_t key;
	struct kinfo_proc *kp;
};

static struct bsd_task *pt=NULL;
static int pt_cnt=0;
static struct bsd_proc_ent *pt_rtid=NULL; // report tid -> kinfo_proc
static struct bsd_proc_ent *pt_tid=NULL; // kernel tid -> kinfo_proc
static unsigned pt_sz=0;
static int64_t pt_wall=0; // wall clock of the snapshot in ms, for the start times

static void pt_clear(void) {
	free(pt_rtid);
	free(pt_tid);
	free(pt);
	pt_rtid=NULL;
	pt_tid=NULL;
	pt=NULL;
	pt_sz=0;
	pt_cnt=0;
}

// fill the two open addressing tables
static void pt_index(void) {
	int n;

	for (pt_sz=1024;pt_sz<(unsigned)pt_cnt*4;pt_sz*=2)
		;
	pt_rtid=calloc(pt_sz,sizeof *pt_rtid);
	pt_tid=calloc(pt_sz,sizeof *pt_tid);
	if (!pt_rtid||!pt_tid) { // no index: the lookups fall back to a linear search
		free(pt_rtid);
		free(pt_tid);
		pt_rtid=pt_tid=NULL;
		pt_sz=0;
		return;
	}
	for (n=0;n<pt_cnt;n++) {
		struct bsd_proc_ent *tbl;
		pid_t key;
		unsigned j;
		int pass;

		for (pass=0;pass<2;pass++) {
			tbl=pass?pt_tid:pt_rtid;
			key=pass?pt[n].kp->ki_tid:pt[n].rtid;
			j=((unsigned)key*2654435761u)&(pt_sz-1);
			while (tbl[j].kp&&tbl[j].key!=key)
				j=(j+1)&(pt_sz-1);
			if (!tbl[j].kp) {
				tbl[j].key=key;
				tbl[j].kp=pt[n].kp;
			}
		}
	}
}

static struct kinfo_proc *pt_lookup(struct bsd_proc_ent *tbl,pid_t key) {
	unsigned j;

	if (!tbl||!key)
		return NULL;
	j=((unsigned)key*2654435761u)&(pt_sz-1);
	while (tbl[j].kp) {
		if (tbl[j].key==key)
			return tbl[j].kp;
		j=(j+1)&(pt_sz-1);
	}
	return NULL;
}

// take the task list; returns the number of threads or -1
inline int bsd_proc_snapshot(void) {
	struct timeval tv;
	size_t len=0;
	int mib[3]={CTL_KERN,KERN_PROC,KERN_PROC_ALL};
	struct kinfo_proc *raw;
	int i;

	pt_clear();

	// a 3 element mib returns every thread; the 4 element form that older
	// releases wanted is gone in 15 and the libutil wrapper gives only the
	// process leaders
	if (sysctl(mib,3,NULL,&len,NULL,0)||!len)
		return -1;
	// tasks are born between the size query and the read; leave room for them
	// and retry once with a fresh size when even that was not enough
	for (i=0;i<2;i++) {
		len+=len/8+16*sizeof(struct kinfo_proc);
		raw=malloc(len);
		if (!raw)
			return -1;
		if (!sysctl(mib,3,raw,&len,NULL,0))
			break;
		free(raw);
		raw=NULL;
		if (errno!=ENOMEM||sysctl(mib,3,NULL,&len,NULL,0)||!len)
			return -1;
	}
	if (!raw)
		return -1;
	pt_cnt=(int)(len/sizeof *raw);
	if (!pt_cnt) {
		free(raw);
		return -1;
	}

	pt=calloc((size_t)pt_cnt,sizeof *pt);
	if (!pt) {
		free(raw);
		pt_cnt=0;
		return -1;
	}
	for (i=0;i<pt_cnt;i++) {
		pt[i].kp=&raw[i];
		// the threads of a process are adjacent and the one created first, the
		// leader, comes first; the pid is what iotop expects as its tid
		if (!i||raw[i-1].ki_pid!=raw[i].ki_pid)
			pt[i].rtid=raw[i].ki_pid;
		else
			pt[i].rtid=raw[i].ki_tid;
	}
	// the raw array is owned by the snapshot: bsd_proc_release frees it via pt[0].kp
	gettimeofday(&tv,NULL);
	pt_wall=(int64_t)tv.tv_sec*1000+tv.tv_usec/1000;
	pt_index();
	return pt_cnt;
}

inline void bsd_proc_release(void) {
	if (pt)
		free(pt[0].kp);
	pt_clear();
	pt_wall=0;
}

inline int64_t bsd_proc_wall(void) {
	return pt_wall;
}

// by the tid that iotop uses
inline struct kinfo_proc *bsd_proc_find(pid_t tid) {
	struct kinfo_proc *kp;
	int i;

	kp=pt_lookup(pt_rtid,tid);
	if (kp)
		return kp;
	if (pt_rtid)
		return NULL;
	for (i=0;i<pt_cnt;i++)
		if (pt[i].rtid==tid)
			return pt[i].kp;
	return NULL;
}

// by the tid of the kernel, which the byte counters are keyed by
inline struct kinfo_proc *bsd_proc_find_ktid(pid_t tid) {
	struct kinfo_proc *kp;
	int i;

	kp=pt_lookup(pt_tid,tid);
	if (kp)
		return kp;
	if (pt_tid)
		return NULL;
	for (i=0;i<pt_cnt;i++)
		if (pt[i].kp->ki_tid==tid)
			return pt[i].kp;
	return NULL;
}

inline int bsd_proc_count(void) {
	return pt_cnt;
}

inline struct kinfo_proc *bsd_proc_at(int i) {
	if (i<0||i>=pt_cnt)
		return NULL;
	return pt[i].kp;
}

inline pid_t bsd_proc_rtid_at(int i) {
	if (i<0||i>=pt_cnt)
		return -1;
	return pt[i].rtid;
}

// the pid that owns the task, also when a thread id was given
inline pid_t bsd_proc_pid_of(pid_t tid) {
	struct kinfo_proc *kp=bsd_proc_find(tid);

	if (!kp)
		kp=bsd_proc_find_ktid(tid);
	return kp?kp->ki_pid:-1;
}

#endif // __FreeBSD__
