#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int passed, failed;
static void check(const char *name, int good)
{
    printf("NSCASE {\"name\":\"%s\",\"value\":%d,\"expected\":1,\"passed\":%s}\n",
           name, !!good, good ? "true" : "false");
    if (good) passed++; else failed++;
}
static void fatal(const char *what) { perror(what); exit(90); }
static int byte_io(int fd, unsigned char *byte, int writing)
{
    ssize_t n;
    do { n=writing ? write(fd,byte,1) : read(fd,byte,1); } while(n<0 && errno==EINTR);
    return n==1;
}
static int reap(pid_t child)
{
    int status;
    pid_t n;
    do { n=waitpid(child,&status,0); } while(n<0 && errno==EINTR);
    return n==child && WIFEXITED(status) && WEXITSTATUS(status)==0;
}
static void basic(void)
{
    int source=eventfd(17,EFD_NONBLOCK);
    if(source<0) fatal("eventfd");
    int alias=fcntl(source,F_DUPFD,10), clo=fcntl(source,F_DUPFD_CLOEXEC,11);
    if(alias<0 || clo<0) fatal("fcntl duplicate");
    check("fd.dupfd.lowest",alias==10 && clo==11);
    check("fd.dupfd.flags",fcntl(alias,F_GETFD)==0 && fcntl(clo,F_GETFD)==FD_CLOEXEC);
    check("fd.setfd.slot",fcntl(alias,F_SETFD,FD_CLOEXEC)==0 &&
          fcntl(alias,F_GETFD)==FD_CLOEXEC && fcntl(source,F_GETFD)==0);
    uint64_t value=0;
    check("fd.dupfd.shared_description",read(alias,&value,8)==8 && value==17 &&
          read(source,&value,8)<0 && errno==EAGAIN);
    value=5;
    check("fd.dupfd.close_alias",close(alias)==0 && write(source,&value,8)==8 &&
          read(clo,&value,8)==8 && value==5);
    errno=0;
    check("fd.dupfd.invalid_min",fcntl(source,F_DUPFD,64)<0 && errno==EINVAL);
    check("fd.dupfd.cleanup",close(clo)==0 && close(source)==0);
}
static void failed_install(void)
{
    int pipefd[2], aliases[64], count=0, good=1;
    if(pipe(pipefd)) fatal("exhaustion pipe");
    while(count<64) {
        int fd=fcntl(pipefd[1],F_DUPFD,0);
        if(fd<0) { good &= errno==EMFILE; break; }
        aliases[count++]=fd;
    }
    check("fd.exhaustion.reached",good && count>40 && count<64);
    for(int i=0;i<1000;i++) {
        if(fcntl(pipefd[1],i&1 ? F_DUPFD : F_DUPFD_CLOEXEC,0)>=0 || errno!=EMFILE)
            good=0;
    }
    check("fd.dupfd.failed_install",good);
    for(int i=0;i<count;i++) if(close(aliases[i])) good=0;
    if(close(pipefd[1])) good=0;
    unsigned char byte;
    /* A leaked failed-install reference would retain the writer and block EOF. */
    check("fd.dupfd.failed_install_no_writer_leak",good && read(pipefd[0],&byte,1)==0);
    if(close(pipefd[0])) fatal("exhaustion reader close");
}

enum { READERS=3, ROUNDS=4000, VICTIM=19 };
static int source_fds[2], cpus[4], ncpus;
static atomic_int ready, start, successful_aliases;
static int pin(unsigned index)
{
    cpu_set_t mask;
    CPU_ZERO(&mask); CPU_SET(cpus[index%(unsigned)ncpus],&mask);
    return sched_setaffinity(0,sizeof(mask),&mask)==0;
}
static void *reader(void *opaque)
{
    unsigned index=(unsigned)(uintptr_t)opaque;
    int bad=!pin(index+1);
    atomic_fetch_add(&ready,1);
    while(!atomic_load(&start)) sched_yield();
    for(int i=0;i<ROUNDS;i++) {
        int fd=fcntl(VICTIM,F_DUPFD_CLOEXEC,32);
        if(fd<0) {
            if(errno!=EBADF && errno!=EMFILE) bad=1;
        } else {
            atomic_fetch_add(&successful_aliases,1);
            if(fcntl(fd,F_GETFD)!=FD_CLOEXEC || !(fcntl(fd,F_GETFL)&O_NONBLOCK)) bad=1;
            uint64_t value=0;
            ssize_t n=read(fd,&value,8);
            if(!((n==8 && (value==1 || value==2)) || (n<0 && errno==EAGAIN))) bad=1;
            if(close(fd)) bad=1;
        }
        int flags=fcntl(VICTIM,F_GETFD);
        if(flags<0 ? errno!=EBADF : (flags!=0 && flags!=FD_CLOEXEC)) bad=1;
        if(fcntl(VICTIM,F_SETFD,i&1 ? FD_CLOEXEC : 0)<0 && errno!=EBADF) bad=1;
    }
    return (void *)(uintptr_t)bad;
}
static void races(void)
{
    cpu_set_t mask;
    if(sched_getaffinity(0,sizeof(mask),&mask)) fatal("getaffinity");
    for(int i=0;i<CPU_SETSIZE && ncpus<4;i++) if(CPU_ISSET(i,&mask)) cpus[ncpus++]=i;
    if(!ncpus) fatal("empty affinity");
    for(int i=0;i<2;i++) {
        source_fds[i]=eventfd((unsigned)i+1,EFD_NONBLOCK);
        if(source_fds[i]<0) fatal("race eventfd");
    }
    if(dup2(source_fds[0],VICTIM)!=VICTIM) fatal("race dup2");
    pthread_t threads[READERS];
    for(unsigned i=0;i<READERS;i++)
        if(pthread_create(&threads[i],NULL,reader,(void *)(uintptr_t)i)) fatal("race pthread_create");
    while(atomic_load(&ready)!=READERS) sched_yield();
    int good=pin(0);
    atomic_store(&start,1);
    for(int i=0;i<ROUNDS;i++) {
        if(i%3==0 && close(VICTIM)<0 && errno!=EBADF) good=0;
        if(dup2(source_fds[i&1],VICTIM)!=VICTIM) good=0;
        sched_yield();
    }
    for(unsigned i=0;i<READERS;i++) {
        void *result;
        if(pthread_join(threads[i],&result) || result) good=0;
    }
    check("fd.race.fcntl_close_dup2",good);
    check("fd.race.live_aliases",atomic_load(&successful_aliases)>100);
    check("fd.race.descriptor_flags_isolated",fcntl(source_fds[0],F_GETFD)==0 &&
          fcntl(source_fds[1],F_GETFD)==0);
    check("fd.race.cleanup",close(VICTIM)==0 && close(source_fds[0])==0 && close(source_fds[1])==0);
    if(sched_setaffinity(0,sizeof(mask),&mask)) fatal("restore affinity");
}
struct blocked_read {
    int fd, error;
    ssize_t result;
    unsigned char byte;
    atomic_int entered;
};
static void *blocked_reader(void *opaque)
{
    struct blocked_read *state=opaque;
    atomic_store(&state->entered,1);
    state->result=read(state->fd,&state->byte,1);
    state->error=errno;
    return NULL;
}
static void final_io_owner(void)
{
    int good=1, in_flight=0;
    for(int i=0;i<32;i++) {
        int fd[2];
        if(pipe(fd)) fatal("in-flight pipe");
        struct blocked_read state={.fd=fd[0]};
        pthread_t thread;
        if(pthread_create(&thread,NULL,blocked_reader,&state)) fatal("in-flight thread");
        while(!atomic_load(&state.entered)) sched_yield();
        /* Outcomes do not depend on a sleep or on assuming read has entered
         * the kernel. Once the sole read descriptor is closed, write succeeds
         * only when that read syscall has acquired its own live reference. */
        for(int j=0;j<8;j++) sched_yield();
        if(close(fd[0])) good=0;
        unsigned char byte=(unsigned char)i;
        ssize_t sent=write(fd[1],&byte,1);
        int error=errno;
        if(pthread_join(thread,NULL)) fatal("in-flight join");
        if(sent==1) {
            in_flight++;
            if(state.result!=1 || state.byte!=byte) good=0;
        } else if(!(sent<0 && error==EPIPE && state.result<0 && state.error==EBADF)) good=0;
        /* The borrowed read reference was the final owner. It must have
         * released the endpoint after returning, without a leaked ref. */
        if(write(fd[1],&byte,1)>=0 || errno!=EPIPE) good=0;
        if(close(fd[1])) good=0;
    }
    check("fd.final_owner.in_flight_read_close",good);
    check("fd.final_owner.in_flight_observed",in_flight>0);
}
static void lock_check(int commands, int answers, unsigned char expected)
{
    unsigned char answer=0;
    if(!byte_io(commands,&expected,1) || !byte_io(answers,&answer,0) || answer!=1)
        fatal("lock child check");
}
static void flock_lifetime(void)
{
    char path[96];
    snprintf(path,sizeof(path),"/tmp/azami-fd-lifetime-lock-%ld",(long)getpid());
    int file=open(path,O_CREAT|O_EXCL|O_RDWR,0600);
    if(file<0) fatal("lock file open");
    if(write(file,"L",1)!=1 || flock(file,LOCK_EX|LOCK_NB)) fatal("lock setup");
    int commands[2], answers[2];
    if(pipe(commands) || pipe(answers)) fatal("lock pipes");
    pid_t child=fork();
    if(child<0) fatal("lock fork");
    if(!child) {
        close(file); close(commands[1]); close(answers[0]);
        int probe=open(path,O_RDWR);
        if(probe<0) _exit(2);
        for(int i=0;i<4;i++) {
            unsigned char expected, answer;
            if(!byte_io(commands[0],&expected,0)) _exit(3);
            errno=0;
            int ret=flock(probe,LOCK_EX|LOCK_NB);
            answer=expected ? ret==0 : (ret<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
            if(ret==0 && flock(probe,LOCK_UN)) _exit(4);
            if(!byte_io(answers[1],&answer,1)) _exit(5);
        }
        _exit(close(probe) ? 6 : 0);
    }
    close(commands[0]); close(answers[1]);
    lock_check(commands[1],answers[0],0);
    check("fd.flock.acquired",1);
    char byte=0;
    if(lseek(file,0,SEEK_SET)<0 || read(file,&byte,1)!=1 || byte!='L') fatal("locked I/O");
    lock_check(commands[1],answers[0],0);
    check("fd.flock.temporary_io_preserves",1);
    if(fcntl(file,F_GETFL)<0 || fcntl(file,F_SETFL,O_NONBLOCK)) fatal("locked fcntl");
    lock_check(commands[1],answers[0],0);
    check("fd.flock.temporary_fcntl_preserves",1);
    if(close(file)) fatal("lock close");
    lock_check(commands[1],answers[0],1);
    check("fd.flock.final_same_process_close_releases",1);
    check("fd.flock.child_exit",reap(child));
    if(close(commands[1]) || close(answers[0]) || unlink(path)) fatal("lock cleanup");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0); alarm(45);
    if(signal(SIGPIPE,SIG_IGN)==SIG_ERR) fatal("ignore SIGPIPE");
    struct rlimit old, limit;
    if(getrlimit(RLIMIT_NOFILE,&old)) fatal("getrlimit");
    limit=old;
    if(limit.rlim_max<64) fatal("NOFILE hard limit below 64");
    limit.rlim_cur=64;
    if(setrlimit(RLIMIT_NOFILE,&limit)) fatal("setrlimit");
    basic(); failed_install(); races(); final_io_owner(); flock_lifetime();
    if(setrlimit(RLIMIT_NOFILE,&old)) fatal("restore rlimit");
    printf("== namespaces skipped: 0 ==\n");
    printf("== probe complete: %d passed, %d failed ==\n",passed,failed);
    return failed ? 1 : 0;
}
