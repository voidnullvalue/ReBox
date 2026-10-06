#!/usr/bin/env python3
"""Fault-inject native fork failures; reject broad signals and leaked FIFOs."""
import pathlib,tempfile,subprocess,os
R=pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='yt-relay-fault-') as tmp:
 t=pathlib.Path(tmp);src=t/'fault.c';src.write_text('''#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
static int calls;
pid_t fork(void){int fail=atoi(getenv("FAIL_FORK"));if(++calls==fail){errno=ENOMEM;return -1;}pid_t(*f)(void)=dlsym(RTLD_NEXT,"fork");return f();}
int kill(pid_t p,int sig){if(p<=1){fprintf(stderr,"INVALID_SIGNAL_TARGET=%ld\\n",(long)p);errno=EPERM;return -1;}int(*f)(pid_t,int)=dlsym(RTLD_NEXT,"kill");return f(p,sig);}
''')
 subprocess.run(['cc','-shared','-fPIC','-o',str(t/'fault.so'),str(src),'-ldl'],check=True)
 subprocess.run(['cc','-O1','-o',str(t/'relay'),str(R.parent/'modules/youtube/relay.c')],check=True)
 for n in (1,2,3):
  p=subprocess.Popen([str(t/'relay'),'https://invalid.example/video','https://invalid.example/audio'],env=dict(os.environ,LD_PRELOAD=str(t/'fault.so'),FAIL_FORK=str(n)),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  out,err=p.communicate(timeout=5);assert p.returncode==1,(n,p.returncode,err);assert b'INVALID_SIGNAL_TARGET' not in err;assert not pathlib.Path('/tmp/hr54-youtube-'+str(p.pid)).exists()
 print('PASS first/second feeder and remux fork failures: exact positive-PID cleanup, bounded exit, no session FIFO leak')
