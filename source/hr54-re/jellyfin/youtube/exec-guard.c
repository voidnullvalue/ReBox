#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>
int main(int n,char **v){if(n<2)return 2;pid_t p=getppid();prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=p)return 1;execv(v[1],v+1);return 127;}
