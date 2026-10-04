#define _GNU_SOURCE
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <stdlib.h>
static pid_t child;
static void expired(int sig){(void)sig;kill(-child,SIGKILL);}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int n,char **v){if(n<3)return 2;double begin=now();signal(SIGALRM,expired);child=fork();if(!child){setpgid(0,0);execv(v[2],v+2);_exit(127);}setpgid(child,child);alarm(atoi(v[1]));int status;struct rusage r;while(wait4(child,&status,0,&r)<0){}alarm(0);fprintf(stderr,"MEASURE elapsed=%.3f cpu=%.3f maxrss_kib=%ld status=%d\n",now()-begin,r.ru_utime.tv_sec+r.ru_utime.tv_usec/1e6+r.ru_stime.tv_sec+r.ru_stime.tv_usec/1e6,r.ru_maxrss,status);return WIFEXITED(status)?WEXITSTATUS(status):1;}
