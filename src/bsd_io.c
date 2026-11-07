/* SPDX-License-Identifier: GPL-2.0-or-later

Copyright (C) 2020-2026  Boian Bonev
Copyright (C) 2026  iotop FreeBSD port

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

/*
 * bsd_io.c - the FreeBSD byte counter backend.
 *
 * Linux gets per-task byte counters (read_bytes/rchar/write_bytes/wchar) from
 * the taskstats netlink interface. FreeBSD has no such accounting: struct
 * rusage counts block operations that are not maintained for regular files,
 * rctl/racct keeps decaying rates rather than cumulative bytes, and the
 * io:::start probe only fires for bio/GEOM traffic - measured to produce zero
 * events for ZFS, which is the default root filesystem.
 *
 * What is left is the syscall boundary. A long running dtrace(1) child
 * aggregates the bytes moved by the read/write family of syscalls, by
 * sendfile and by the results of the asynchronous read/write pair, per
 * thread, and prints the aggregates once per second; this process reads that
 * stream and keeps running totals per tid. The resulting numbers are the
 * equivalent of rchar/wchar (the VFS columns of iotop), not of read_bytes
 * (the disk columns), which is why the port reports them in both.
 */

#include "iotop.h"

#ifdef __FreeBSD__

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* the syscalls that move bytes for a task; the readv family is included so that
   the common libc wrappers are accounted too

   sendfile reports the bytes in *sbytes, not in the return value, so the sbytes
   pointer of the entry probe is dereferenced on return; the low 4 bytes of the
   off_t hold the count of any single call below 2 GiB on a little endian
   machine, whatever the data model of the caller is (the psinfo translator of
   FreeBSD dtrace has no pr_dmodel member to tell them apart, and arm64 has no
   32 bit compatibility layer anyway), so a larger single call undercounts. A
   call that passes no sbytes pointer can not be counted. aio_read and aio_write
   return 0 when the
   operation is queued, the bytes arrive in the return value of aio_return, so
   the direction of an aiocbp is remembered at submission time in an associative
   array keyed by (pid, aiocbp) - the pid is part of the key because the user
   virtual address of a control block is not unique across processes - and
   consumed at aio_return (not an aggregation: an aggregation reads the last
   committed snapshot and would lag the submission); a result fetched with
   lio_listio bookkeeping instead of aio_return is not counted, and such a
   submission leaves its dir[] entry behind (plain associative arrays have no
   trunc, so the leak can only be bounded by dtrace dynvarsize) */
#define DTRACE_SCRIPT \
	"syscall::read:return /arg1 > 0/ { @r[tid] = sum(arg1); }\n" \
	"syscall::pread:return /arg1 > 0/ { @r[tid] = sum(arg1); }\n" \
	"syscall::readv:return /arg1 > 0/ { @r[tid] = sum(arg1); }\n" \
	"syscall::preadv:return /arg1 > 0/ { @r[tid] = sum(arg1); }\n" \
	"syscall::write:return /arg1 > 0/ { @w[tid] = sum(arg1); }\n" \
	"syscall::pwrite:return /arg1 > 0/ { @w[tid] = sum(arg1); }\n" \
	"syscall::writev:return /arg1 > 0/ { @w[tid] = sum(arg1); }\n" \
	"syscall::pwritev:return /arg1 > 0/ { @w[tid] = sum(arg1); }\n" \
	"syscall::sendfile:entry /arg5 != 0/ { self->sb = arg5; }\n" \
	"syscall::sendfile:return\n" \
	"{\n" \
	"	this->n = 0;\n" \
	"	this->n = self->sb == 0 ? 0 : *(int32_t *)copyin(self->sb, 4);\n" \
	"	self->sb = 0;\n" \
	"}\n" \
	"syscall::sendfile:return /this->n > 0/ { @w[tid] = sum(this->n); }\n" \
	"syscall::aio_read:entry { dir[pid,arg0] = 1; }\n" \
	"syscall::aio_write:entry { dir[pid,arg0] = 2; }\n" \
	"syscall::aio_return:entry { self->cb = arg0; }\n" \
	"syscall::aio_return:return\n" \
	"{\n" \
	"	this->d = dir[pid,self->cb];\n" \
	"	dir[pid,self->cb] = 0;\n" \
	"	self->cb = 0;\n" \
	"}\n" \
	"syscall::aio_return:return /this->d == 1 && arg1 > 0/ { @r[tid] = sum(arg1); }\n" \
	"syscall::aio_return:return /this->d == 2 && arg1 > 0/ { @w[tid] = sum(arg1); }\n" \
	"tick-1s { printf(\"\\nR\\n\"); printa(@r); printf(\"\\nW\\n\"); printa(@w); trunc(@r); trunc(@w); }\n"

#define IO_TBL_SZ_INI 1024
#define IO_BUF_SZ_INI 8192

struct bsd_io_ent {
	pid_t tid;
	uint64_t rd;
	uint64_t wr;
};

static struct bsd_io_ent *io_tbl=NULL;
static unsigned io_tbl_sz=0; // power of 2
static unsigned io_tbl_cnt=0;
static char *rd_buf=NULL; // the unparsed tail of the dtrace stream
static size_t rd_len=0,rd_sz=0;
static pid_t dt_pid=-1;
static int dt_out=-1,dt_err=-1;
static int io_dead=0; // the dtrace child exited; the data can not grow any more
static uint64_t io_drops=0; // the aggregation lines dtrace dropped, from its stderr
static char err_buf[512]; // the unparsed tail of the stderr stream
static size_t err_len=0;
static char err_log[1024]; // the last lines of stderr, for the error report of a dead child
static size_t err_log_len=0;
static char io_sec='?'; // the aggregation the lines being parsed belong to

static unsigned io_hash(pid_t tid) {
	return ((unsigned)tid*2654435761u)&(io_tbl_sz-1);
}

static int io_tbl_grow(void) {
	unsigned nsz=io_tbl_sz?io_tbl_sz*2:IO_TBL_SZ_INI;
	struct bsd_io_ent *nt=calloc(nsz,sizeof *nt);
	unsigned i;

	if (!nt)
		return -1;
	for (i=0;i<io_tbl_sz;i++) {
		if (!io_tbl[i].tid)
			continue;
		{
			unsigned j=((unsigned)io_tbl[i].tid*2654435761u)&(nsz-1);

			while (nt[j].tid)
				j=(j+1)&(nsz-1);
			nt[j]=io_tbl[i];
		}
	}
	free(io_tbl);
	io_tbl=nt;
	io_tbl_sz=nsz;
	return 0;
}

static struct bsd_io_ent *io_ent_get(pid_t tid,int create) {
	struct bsd_io_ent *e;
	unsigned i;

	if (!io_tbl_sz||io_tbl_cnt*2>=io_tbl_sz) {
		if (io_tbl_grow())
			return NULL;
	}
	if (!tid)
		return NULL;
	i=io_hash(tid);
	for (;;) {
		e=&io_tbl[i];
		if (!e->tid) {
			if (!create)
				return NULL;
			e->tid=tid;
			io_tbl_cnt++;
			return e;
		}
		if (e->tid==tid)
			return e;
		i=(i+1)&(io_tbl_sz-1);
	}
}

static void io_word(char sec,unsigned long long val,pid_t tid) {
	struct bsd_io_ent *e;

	if (!tid||sec=='?')
		return;
	e=io_ent_get(tid,1);
	if (!e)
		return;
	// the aggregates cover one tick interval, the running totals are kept here
	if (sec=='R')
		e->rd+=val;
	else
		e->wr+=val;
}

/* parse the complete lines of the stream; the last partial line stays in the buffer */
static void io_parse(void) {
	size_t pos=0;

	while (pos<rd_len) {
		char *nl=memchr(rd_buf+pos,'\n',rd_len-pos);
		char *line;
		unsigned long long val;
		int tid;

		if (!nl)
			break;
		*nl=0;
		line=rd_buf+pos;
		pos=(size_t)(nl-rd_buf)+1;
		if (!*line)
			continue;
		if (!strcmp(line,"R")||!strcmp(line,"W")) { // the marker of an aggregation
			io_sec=line[0];
			continue;
		}
		// printa(@x) without a format yields "<key> <value>"; dtrace chatter does not parse
		if (sscanf(line,"%d %llu",&tid,&val)==2)
			io_word(io_sec,val,tid);
	}
	if (pos) {
		memmove(rd_buf,rd_buf+pos,rd_len-pos);
		rd_len-=pos;
	}
}

static int io_buf_room(size_t want) {
	if (rd_len+want<=rd_sz)
		return 0;
	{
		size_t nsz=rd_sz?rd_sz:IO_BUF_SZ_INI;
		char *t;

		while (rd_len+want>nsz)
			nsz*=2;
		t=realloc(rd_buf,nsz);
		if (!t)
			return -1;
		rd_buf=t;
		rd_sz=nsz;
	}
	return 0;
}

/* read whatever the child wrote on stderr; never blocks. An unread pipe would
   fill its 64 KB buffer and then block dtrace itself, freezing the counters on
   stdout, so this has to run on every drain. The chatter that matters is the
   drop report ("dtrace: N drops on CPU X"): each one means lost aggregations,
   so the totals are summed up and shown in the UI. */
static void io_drain_err(void) {
	if (dt_err<0)
		return;
	for (;;) {
		ssize_t n=read(dt_err,err_buf+err_len,sizeof err_buf-1-err_len);

		if (n>0) {
			err_len+=(size_t)n;
			err_buf[err_len]=0;
			for (;;) {
				char *nl=memchr(err_buf,'\n',err_len);
				unsigned long long d=0;
				size_t used;

				if (!nl)
					break;
				*nl=0;
				if (sscanf(err_buf,"dtrace: %llu drops on CPU",&d)==1)
					io_drops+=d;
				used=(size_t)(nl-err_buf)+1;
				{ // keep the line for the report of a child that died on its own
					size_t l=strlen(err_buf)+1; // with a newline

					if (l>=sizeof err_log) { // longer than the whole log: keep the tail
						memcpy(err_log,err_buf+l-sizeof err_log,sizeof err_log-1);
						err_log[sizeof err_log-1]='\n';
						err_log_len=sizeof err_log-1;
					} else {
						if (err_log_len+l>sizeof err_log-1) { // drop the oldest bytes
							if (err_log_len>l) {
								memmove(err_log,err_log+l,err_log_len-l);
								err_log_len-=l;
							} else
								err_log_len=0;
						}
						memcpy(err_log+err_log_len,err_buf,l-1);
						err_log_len+=l-1;
						err_log[err_log_len++]='\n';
					}
				}
				memmove(err_buf,err_buf+used,err_len-used);
				err_len-=used;
			}
			if (err_len==sizeof err_buf-1) // a line longer than the buffer is chatter; drop it
				err_len=0;
			continue;
		}
		if (n==0) { // eof: the child closed stderr
			close(dt_err);
			dt_err=-1;
			return;
		}
		return; // EAGAIN, EINTR or a real error: nothing more to do now
	}
}

/* read whatever the child produced; never blocks */
inline void bsd_io_drain(void) {
	io_drain_err();
	if (dt_out<0||io_dead)
		return;
	for (;;) {
		ssize_t n;

		if (io_buf_room(4096))
			return;
		n=read(dt_out,rd_buf+rd_len,rd_sz-rd_len);
		if (n>0) {
			rd_len+=(size_t)n;
			io_parse();
			continue;
		}
		if (n==0) { // eof: the child is gone
			io_dead=1;
			while (waitpid(dt_pid,NULL,WNOHANG)<0&&errno==EINTR) // no zombie left behind
				;
			return;
		}
		if (errno==EAGAIN||errno==EINTR)
			return;
		io_dead=1;
		return;
	}
}

inline int bsd_io_get(pid_t tid,uint64_t *rd,uint64_t *wr) {
	struct bsd_io_ent *e=io_ent_get(tid,0);

	if (!e) {
		if (rd)
			*rd=0;
		if (wr)
			*wr=0;
		return 0;
	}
	if (rd)
		*rd=e->rd;
	if (wr)
		*wr=e->wr;
	return 1;
}

inline int bsd_io_is_dead(void) {
	return io_dead;
}

// the number of aggregation drops the child reported on its stderr so far
inline uint64_t bsd_io_drops(void) {
	return io_drops;
}

/* drop the totals of the tasks that are gone: their tid will be given to a new
   task, which must not inherit the counters of the old one; an open addressing
   table can not have holes, so the survivors are reinserted */
inline void bsd_io_prune(void) {
	struct bsd_io_ent *old;
	unsigned old_sz,i;

	if (!io_tbl||!bsd_proc_count())
		return;
	old=io_tbl;
	old_sz=io_tbl_sz;
	io_tbl=NULL;
	io_tbl_sz=0;
	io_tbl_cnt=0;
	for (i=0;i<old_sz;i++) {
		struct bsd_io_ent *e;

		if (!old[i].tid)
			continue;
		if (!bsd_proc_find_ktid(old[i].tid)) // gone; the table is keyed by the kernel tid
			continue;
		e=io_ent_get(old[i].tid,1);
		if (!e) { // no memory: the next cycle tries again
			free(old);
			return;
		}
		e->rd=old[i].rd;
		e->wr=old[i].wr;
	}
	free(old);
}

static void io_fail(const char *what) {
	// the stderr of the child was already collected by io_drain_err
	fprintf(stderr,"iotop: %s\n",what);
	if (err_log_len) {
		fprintf(stderr,"%.*s",(int)err_log_len,err_log);
		err_log_len=0;
	}
	if (err_len) { // a partial last line, without the newline
		fprintf(stderr,"%.*s\n",(int)err_len,err_buf);
		err_len=0;
	}
}

/* start the dtrace child; returns 0 on success */
inline int bsd_io_init(void) {
	int po[2],pe[2];
	pid_t pid;

	if (dt_pid>-1)
		return 0;
	if (pipe(po)) {
		perror("iotop: pipe");
		return -1;
	}
	if (pipe(pe)) {
		perror("iotop: pipe");
		close(po[0]);close(po[1]);
		return -1;
	}
	pid=fork();
	if (pid<0) {
		perror("iotop: fork");
		close(po[0]);close(po[1]);close(pe[0]);close(pe[1]);
		return -1;
	}
	if (!pid) { // child: dtrace -q -n <script>, unbuffered stdout via stdbuf
		setpgid(0,0); // own group, so that killing it also reaps a dtrace run by stdbuf
		dup2(po[1],STDOUT_FILENO);
		dup2(pe[1],STDERR_FILENO);
		close(po[0]);close(po[1]);close(pe[0]);close(pe[1]);
		execl("/usr/bin/stdbuf","stdbuf","-o0","/usr/sbin/dtrace","-q","-n",DTRACE_SCRIPT,(char *)NULL);
		execl("/usr/sbin/dtrace","dtrace","-q","-n",DTRACE_SCRIPT,(char *)NULL);
		fprintf(stderr,"iotop: can not execute dtrace: %s\n",strerror(errno));
		_exit(127);
	}
	setpgid(pid,pid); // the child may not have reached its own setpgid yet
	close(po[1]);close(pe[1]);
	dt_pid=pid;
	dt_out=po[0];
	dt_err=pe[0];
	{ // the reads must not block the UI
		int fl;

		fl=fcntl(dt_out,F_GETFL);
		if (fl>-1)
			fcntl(dt_out,F_SETFL,fl|O_NONBLOCK);
		fl=fcntl(dt_err,F_GETFL);
		if (fl>-1)
			fcntl(dt_err,F_SETFL,fl|O_NONBLOCK);
	}
	return 0;
}

/* the first cycle has no data yet; wait until the child produced something or died */
inline int bsd_io_warmup(int ms) {
	int64_t until=monotime()+ms;

	// a script that does not compile kills the child within a few tens of
	// milliseconds; waiting longer would only delay the first screen
	while (monotime()<until) {
		struct timespec ts={0,20000000};
		int st;

		bsd_io_drain();
		if (io_tbl_cnt)
			return 0;
		if (waitpid(dt_pid,&st,WNOHANG)==dt_pid) {
			io_fail("dtrace exited unexpectedly");
			return -1;
		}
		nanosleep(&ts,NULL);
	}
	bsd_io_drain();
	// an idle system produces no syscall events at all but the tick still prints,
	// so a live child is good enough even without any counter
	return 0;
}

inline void bsd_io_fini(void) {
	if (dt_pid>-1) {
		kill(-dt_pid,SIGTERM); // the group: stdbuf and the dtrace it runs
		kill(dt_pid,SIGTERM);
		while (waitpid(dt_pid,NULL,0)<0&&errno==EINTR)
			;
		dt_pid=-1;
	}
	if (dt_out>-1) {
		close(dt_out);
		dt_out=-1;
	}
	if (dt_err>-1) {
		close(dt_err);
		dt_err=-1;
	}
	free(io_tbl);
	io_tbl=NULL;
	io_tbl_sz=io_tbl_cnt=0;
	free(rd_buf);
	rd_buf=NULL;
	rd_len=rd_sz=0;
	io_dead=0;
	io_drops=0;
	err_len=err_log_len=0;
}

#endif // __FreeBSD__
