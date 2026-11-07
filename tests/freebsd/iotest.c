/* SPDX-License-Identifier: GPL-2.0-or-later

Copyright (C) 2026  iotop FreeBSD port

A deterministic io generator for the tests: it moves a known number of bytes
at a known rate, from a known pid, and reports what it moved, so that the
numbers printed by iotop can be compared with them.

	iotest r|w <path> <total_bytes> <seconds> [threads] [linger] [go]
	iotest s|a <path> <total_bytes> <seconds> [linger] [go]

The rate is total_bytes/seconds spread over the given number of threads. When
the transfer is over the process sleeps for linger seconds (3 by default) so
that a sampler that runs once per second can still see the last counters.

The s mode sends the file over a loopback socket with sendfile(2); the moved
bytes are the ones reported in sbytes. The a mode reads with aio_read(2) and
writes the same bytes with aio_write(2) to <path>.aw, and reports both totals;
the moved bytes are the ones reported by aio_return(2).
*/

#include <aio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pthread_np.h> /* pthread_getthreadid_np, the lwpid of the kernel */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#define CHUNK (64*1024)
/* a sendfile call on loopback costs tens of milliseconds whatever the size,
   so the s mode moves eight times fewer, eight times bigger blocks */
#define SF_CHUNK (512*1024)

struct job {
	const char *path;
	int wr;
	unsigned long long bytes;
	long long chunk_ns; // the time a chunk should take
	unsigned long long done;
	int err;
	int linger; // keep the thread alive after the transfer so that the last counters can be sampled
	unsigned lwpid; // the thread id the kernel knows, which is what iotop shows
};

// the tid of the calling thread as kinfo_proc reports it
static unsigned self_lwpid(void) {
	return (unsigned)pthread_getthreadid_np();
}

static void *worker(void *arg) {
	struct job *j=arg;
	unsigned char *buf;
	unsigned long long left=j->bytes;
	int fd;

	j->lwpid=self_lwpid();
	buf=malloc(CHUNK);
	if (!buf) {
		j->err=1;
		return NULL;
	}
	memset(buf,'x',CHUNK);
	fd=open(j->path,j->wr?(O_WRONLY|O_CREAT|O_TRUNC):O_RDONLY,0600);
	if (fd<0) {
		fprintf(stderr,"iotest: %s: %s\n",j->path,strerror(errno));
		j->err=1;
		free(buf);
		return NULL;
	}
	while (left) {
		size_t want=left<(unsigned long long)CHUNK?(size_t)left:CHUNK;
		ssize_t n;
		struct timespec ts;

		if (j->wr)
			n=write(fd,buf,want);
		else
			n=read(fd,buf,want);
		if (n<0) {
			if (errno==EINTR)
				continue;
			break;
		}
		if (!n) { // end of a file that is shorter than the wanted total
			if (j->wr||lseek(fd,0,SEEK_SET)<0)
				break;
			continue;
		}
		j->done+=(unsigned long long)n;
		left-=(unsigned long long)n;
		if (j->chunk_ns>0) {
			ts.tv_sec=(time_t)(j->chunk_ns/1000000000LL);
			ts.tv_nsec=(long)(j->chunk_ns%1000000000LL);
			nanosleep(&ts,NULL);
		}
	}
	close(fd);
	free(buf);
	if (j->linger>0) // a sampler that runs once per second must be able to see the final counters
		sleep((unsigned)j->linger);
	return NULL;
}

static void pace(long long chunk_ns) {
	if (chunk_ns>0) {
		struct timespec ts;

		ts.tv_sec=(time_t)(chunk_ns/1000000000LL);
		ts.tv_nsec=(long)(chunk_ns%1000000000LL);
		nanosleep(&ts,NULL);
	}
}

/* send the file out over a loopback socket; the peer is drained with recv so
   that the socket buffer never fills and every call reports real progress */
static unsigned long long do_sendfile(const char *path,unsigned long long total,long long chunk_ns) {
	struct sockaddr_in sin;
	unsigned long long done=0;
	unsigned char *buf;
	socklen_t sl;
	off_t off=0;
	int fd,ls,cs,as;

	fd=open(path,O_RDONLY);
	if (fd<0) {
		fprintf(stderr,"iotest: %s: %s\n",path,strerror(errno));
		return 0;
	}
	memset(&sin,0,sizeof sin);
	sin.sin_family=AF_INET;
	sin.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
	ls=socket(AF_INET,SOCK_STREAM,0);
	if (ls<0||bind(ls,(struct sockaddr *)&sin,sizeof sin)||listen(ls,1)) {
		fprintf(stderr,"iotest: can not set up the loopback listener: %s\n",strerror(errno));
		close(fd);
		if (ls>=0)
			close(ls);
		return 0;
	}
	sl=sizeof sin;
	if (getsockname(ls,(struct sockaddr *)&sin,&sl)) {
		fprintf(stderr,"iotest: getsockname: %s\n",strerror(errno));
		close(ls);close(fd);
		return 0;
	}
	cs=socket(AF_INET,SOCK_STREAM,0);
	as=-1;
	if (cs>=0&&!connect(cs,(struct sockaddr *)&sin,sizeof sin))
		as=accept(ls,NULL,NULL);
	close(ls);
	if (cs<0||as<0) {
		fprintf(stderr,"iotop: can not connect the loopback pair: %s\n",strerror(errno));
		if (cs>=0)
			close(cs);
		if (as>=0)
			close(as);
		close(fd);
		return 0;
	}
	buf=malloc(SF_CHUNK);
	{
		// a sendfile call blocks until the whole block is accepted, and the
		// peer is drained only after the call returns, so the socket buffers
		// must hold a full block or the pair deadlocks
		int bs=SF_CHUNK*4;

		setsockopt(cs,SOL_SOCKET,SO_SNDBUF,&bs,sizeof bs);
		setsockopt(as,SOL_SOCKET,SO_RCVBUF,&bs,sizeof bs);
	}
	while (buf&&done<total) {
		size_t want=total-done<(unsigned long long)SF_CHUNK?(size_t)(total-done):SF_CHUNK;
		off_t sent=0;

		// the byte count is in sbytes, the return value is 0 or -1
		if (sendfile(fd,cs,off,want,NULL,&sent,0)<0&&errno!=EINTR&&errno!=EAGAIN)
			break;
		if (sent<=0)
			break;
		{
			off_t drained=0;

			while (drained<sent) { // keep the socket empty for the next round
				ssize_t n=recv(as,buf,CHUNK,0);

				if (n<=0)
					break;
				drained+=n;
			}
			if (drained<sent)
				break;
		}
		off+=sent;
		done+=(unsigned long long)sent;
		pace(chunk_ns);
	}
	free(buf);
	close(as);close(cs);close(fd);
	return done;
}

/* wait for an asynchronous operation and return its byte count, -1 on error */
static long long aio_done(struct aiocb *cb) {
	int e=aio_error(cb);

	while (e==EINPROGRESS) {
		usleep(1000);
		e=aio_error(cb);
	}
	if (e)
		return -1;
	return (long long)aio_return(cb);
}

/* read with aio_read and write the same bytes with aio_write; the counts are
   the values that aio_return reports, which is what the backend counts */
static int do_aio(const char *path,unsigned long long total,long long chunk_ns,unsigned long long *done_r,unsigned long long *done_w) {
	char wpath[4096];
	unsigned long long left=total;
	unsigned char *buf;
	off_t roff=0,woff=0;
	int fr,fw=-1;

	*done_r=*done_w=0;
	fr=open(path,O_RDONLY);
	if (fr<0) {
		fprintf(stderr,"iotest: %s: %s\n",path,strerror(errno));
		return -1;
	}
	snprintf(wpath,sizeof wpath,"%s.aw",path);
	fw=open(wpath,O_WRONLY|O_CREAT|O_TRUNC,0600);
	if (fw<0) {
		fprintf(stderr,"iotest: %s: %s\n",wpath,strerror(errno));
		close(fr);
		return -1;
	}
	buf=malloc(CHUNK);
	while (buf&&left) {
		size_t want=left<(unsigned long long)CHUNK?(size_t)left:CHUNK;
		struct aiocb cr,cw;
		long long n1,n2;

		memset(&cr,0,sizeof cr);
		cr.aio_fildes=fr;
		cr.aio_buf=buf;
		cr.aio_nbytes=want;
		cr.aio_offset=roff;
		memset(&cw,0,sizeof cw);
		cw.aio_fildes=fw;
		cw.aio_buf=buf;
		cw.aio_nbytes=want;
		cw.aio_offset=woff;
		if (aio_read(&cr)||aio_write(&cw)) {
			fprintf(stderr,"iotest: aio submission failed: %s\n",strerror(errno));
			break;
		}
		n1=aio_done(&cr);
		n2=n1>0?aio_done(&cw):-1;
		if (n1<=0||n2<=0) // a short file or a failed write ends the transfer
			break;
		*done_r+=(unsigned long long)n1;
		*done_w+=(unsigned long long)n2;
		roff+=n1;
		woff+=n2;
		left-=(unsigned long long)n1;
		pace(chunk_ns);
	}
	free(buf);
	close(fw);close(fr);
	return 0;
}

int main(int argc,char **argv) {
	unsigned long long total;
	int threads=1,linger=3,nsec=1;
	struct job *jobs;
	pthread_t *th;
	long long per_chunk_ns;
	int i;

	if (argc<5) {
		fprintf(stderr,"usage: %s r|w <path> <total_bytes> <seconds> [threads] [linger] [go]\n"
			"       %s s|a <path> <total_bytes> <seconds> [linger] [go]\n",argv[0],argv[0]);
		return 2;
	}
	total=strtoull(argv[3],NULL,10);
	nsec=atoi(argv[4]);
	if (!nsec)
		nsec=1;
	if (argc>5)
		threads=atoi(argv[5]);
	if (threads<1)
		threads=1;
	if (argc>6)
		linger=atoi(argv[6]);

	jobs=calloc((size_t)threads,sizeof *jobs);
	th=calloc((size_t)threads,sizeof *th);
	if (!jobs||!th)
		return 2;

	// the pacing is per chunk of 64k; bytes are spread evenly over the threads
	per_chunk_ns=0;
	if (total/(unsigned long long)threads/CHUNK) {
		long long chunks=(long long)(total/(unsigned long long)threads/CHUNK);

		per_chunk_ns=(long long)nsec*1000000000LL/chunks;
	}

	printf("PID %d\n",(int)getpid());
	fflush(stdout);

	// the single threaded modes: the process leader does all the work and the
	// argument order has no threads slot
	if (argv[1][0]=='s'||argv[1][0]=='a') {
		unsigned long long dr=0,dw=0;
		int linger=argc>5?atoi(argv[5]):3;
		long long chunk_ns=0;

		if (total/CHUNK)
			chunk_ns=(long long)nsec*1000000000LL/(long long)(total/CHUNK);
		if (argc>6) { // an optional go file, as for the threaded modes
			struct timespec gs={0,10000000};
			int n=0;

			while (access(argv[6],F_OK)&&n<3000) {
				nanosleep(&gs,NULL);
				n++;
			}
		}
		if (argv[1][0]=='s') { // the pacing of the s mode is per 512k block
			if (total/SF_CHUNK)
				chunk_ns=(long long)nsec*1000000000LL/(long long)(total/SF_CHUNK);
			dw=do_sendfile(argv[2],total,chunk_ns);
			printf("THREAD %u %llu\n",self_lwpid(),dw);
			printf("TOTAL 0 %llu\n",dw);
		} else {
			if (!do_aio(argv[2],total,chunk_ns,&dr,&dw)) {
				printf("THREAD %u %llu\n",self_lwpid(),dr+dw);
				printf("TOTAL %llu %llu\n",dr,dw);
			} else
				printf("ERROR\n");
		}
		fflush(stdout);
		if (linger>0)
			sleep((unsigned)linger);
		sleep(2); // the process has to outlive the last sample
		return 0;
	}

	// an optional go file lets a test start the samplers before the load begins,
	// so that nothing is moved before the first sample
	if (argc>7) {
		struct timespec gs={0,10000000};
		int n=0;

		while (access(argv[7],F_OK)&&n<3000) {
			nanosleep(&gs,NULL);
			n++;
		}
	}

	for (i=0;i<threads;i++) {
		jobs[i].path=argv[2];
		jobs[i].wr=argv[1][0]=='w';
		jobs[i].bytes=total/(unsigned long long)threads;
		jobs[i].chunk_ns=per_chunk_ns;
		jobs[i].linger=linger;
		if (threads==1) { // the process leader does the work, no extra thread
			worker(&jobs[i]);
			continue;
		}
		if (pthread_create(&th[i],NULL,worker,&jobs[i])) {
			fprintf(stderr,"iotest: pthread_create failed\n");
			return 2;
		}
	}

	{
		unsigned long long sum=0;
		int errs=0;

		for (i=0;i<threads;i++) {
			pthread_join(th[i],NULL);
			sum+=jobs[i].done;
			errs+=jobs[i].err;
			printf("THREAD %u %llu\n",jobs[i].lwpid,jobs[i].done);
		}
		if (errs) {
			printf("ERROR\n");
			return 1;
		}
		if (argv[1][0]=='w')
			printf("TOTAL 0 %llu\n",sum);
		else
			printf("TOTAL %llu 0\n",sum);
		fflush(stdout);
	}

	// the process has to outlive the last sample of its threads
	sleep(2);
	return 0;
}
