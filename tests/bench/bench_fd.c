#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ITERATIONS 20000
struct result { uint64_t elapsed, checksum; int good, cpu; };
struct worker { int gate, ready, output, cpu; struct result result; };
static uint64_t now(void) {
    struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static int transfer(int fd, void *data, size_t len, int writing) {
    unsigned char *p=data;
    while(len) { ssize_t n=writing ? write(fd,p,len) : read(fd,p,len);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return 0;
        p+=n; len-=(size_t)n;
    } return 1;
}
static void *worker(void *data) {
    struct worker *w=data; struct result *r=&w->result;
    cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(w->cpu,&cpus);
    r->cpu=w->cpu; r->good=!sched_setaffinity(0,sizeof(cpus),&cpus);
    int fd=eventfd(0,EFD_CLOEXEC); if(fd<0) r->good=0;
    uint64_t one=1,value=0;
    for(int i=0;i<256 && r->good;i++)
        r->good=write(fd,&one,8)==8 && read(fd,&value,8)==8 && value==1;
    unsigned char token=1;
    if(!transfer(w->ready,&token,1,1) || !transfer(w->gate,&token,1,0)) r->good=0;
    uint64_t start=now();
    if(!start) r->good=0;
    for(int i=0;i<ITERATIONS && r->good;i++) {
        r->good=write(fd,&one,8)==8 && read(fd,&value,8)==8 && value==1;
        r->checksum+=value;
    }
    uint64_t end=now();r->elapsed=end>start ? end-start : 0;
    r->good=r->good && r->elapsed && r->checksum==ITERATIONS;
    if(fd>=0) close(fd);
    if(w->output>=0 && !transfer(w->output,r,sizeof(*r),1)) _exit(91);
    return NULL;
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);alarm(120);
    cpu_set_t cpus; CPU_ZERO(&cpus); int ids[4],ncpu=0;
    if(!sched_getaffinity(0,sizeof(cpus),&cpus))
        for(int i=0;i<CPU_SETSIZE && ncpu<4;i++) if(CPU_ISSET(i,&cpus)) ids[ncpu++]=i;
    if(ncpu<4) { puts("== probe complete: 0 passed, 1 failed ==");return 1; }
    int passed=0,failed=0;
    for(int mode=0;mode<3;mode++) for(int rep=-1;rep<10;rep++) {
        int n=mode ? 4 : 1,gate[2],ready[2],output[2];
        if(pipe(gate)||pipe(ready)||pipe(output)) return 2;
        struct worker w[4]={0};pthread_t threads[4];pid_t children[4];int created=0;
        for(int i=0;i<n;i++) {
            w[i]=(struct worker){.gate=gate[0],.ready=ready[1],.output=mode==2 ? output[1] : -1,.cpu=ids[i]};
            if(mode==2) { children[i]=fork(); if(!children[i]) {worker(&w[i]);_exit(w[i].result.good ? 0 : 1);}
                if(children[i]<0) break;
            } else if(pthread_create(&threads[i],NULL,worker,&w[i])) break;
            created++;
        }
        if(created!=n) return 3;
        unsigned char token;
        for(int i=0;i<n;i++) if(!transfer(ready[0],&token,1,0)) return 4;
        for(int i=0;i<n;i++) if(!transfer(gate[1],&token,1,1)) return 5;
        struct result results[4]={0};
        if(mode==2) {
            for(int i=0;i<n;i++) if(!transfer(output[0],&results[i],sizeof(results[i]),0)) return 6;
            for(int i=0;i<n;i++) { int status; if(waitpid(children[i],&status,0)!=children[i] ||
                !WIFEXITED(status)||WEXITSTATUS(status)) return 7; }
        } else for(int i=0;i<n;i++) {
            if(pthread_join(threads[i],NULL)) return 8;
            results[i]=w[i].result;
        }
        for(int i=0;i<n;i++) {
            if(results[i].good) passed++; else failed++;
            printf("FDBENCH {\"mode\":\"%s\",\"repetition\":%d,\"worker\":%d,\"cpu\":%d,\"iterations\":%d,\"elapsed_ns\":%llu,\"checksum\":%llu,\"passed\":%s}\n",
                mode==0 ? "one-thread" : mode==1 ? "four-threads" : "four-processes",rep,i,results[i].cpu,
                ITERATIONS,(unsigned long long)results[i].elapsed,(unsigned long long)results[i].checksum,
                results[i].good ? "true" : "false");
        }
        close(gate[0]);close(gate[1]);close(ready[0]);close(ready[1]);close(output[0]);close(output[1]);
    }
    printf("== probe complete: %d passed, %d failed ==\n",passed,failed);return failed ? 1 : 0;
}
