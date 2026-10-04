/* Host regression for production native stock-I-frame control. */
static int test_wm_port;
static char druid_path[512];
static const char *guard_command;
#define JF_DRUID_AUTO_START druid_path
#define JF_NATIVE_WM_PORT test_wm_port
#define JF_NATIVE_WM_GUARD guard_command
#define main receiver_main
#include "../hr54_jf.c"
#undef main
#include <assert.h>
static const char *good_guard="printf '0222999c41c9a57dabd8c9b3d714b69e  /opt/dtvwm/lib/libdtvwm.so\\nfed62d04663412e60388ac09a83cbc05  /opt/dtvwm/bin/dtvwm\\n'";
static void exchange(int fd,unsigned command,const unsigned *data,size_t count,
                     unsigned type,unsigned result,unsigned value){
    uint32_t packet[7],reply[16]={htonl(type),htonl(result),htonl(value)};
    assert(!native_wm_transfer(fd,packet,16+count*4,1));
    assert(ntohl(packet[0])==command&&ntohl(packet[1])==0xd123567a);
    assert(ntohl(packet[2])==count*4&&packet[3]==0);
    for(size_t i=0;i<count;i++)assert(ntohl(packet[4+i])==data[i]);
    /* Real TCP does not preserve control-message boundaries. */
    for(size_t i=0;i<sizeof reply;i++)assert(send(fd,(char *)reply+i,1,MSG_NOSIGNAL)==1);
}
static void test_case(int mode){
    int server=socket(AF_INET,SOCK_STREAM,0);assert(server>=0);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(server,(struct sockaddr *)&a,sizeof a));socklen_t len=sizeof a;
    assert(!getsockname(server,(struct sockaddr *)&a,&len));test_wm_port=ntohs(a.sin_port);
    assert(!listen(server,1));pid_t child=fork();assert(child>=0);
    if(!child){
        alarm(10);int fd=accept(server,NULL,NULL);assert(fd>=0);uint32_t reg[16];
        assert(!native_wm_transfer(fd,reg,sizeof reg,1));assert(ntohl(reg[0])==1&&reg[1]==0&&ntohl(reg[2])>0);
        for(int i=3;i<16;i++)assert(reg[i]==0);
        unsigned data[3]={0,0,1};
        exchange(fd,42,data,2,mode==2?2:3,0,6);
        if(mode!=2){exchange(fd,40,data,3,2,mode==3?1:0,0);
            if(mode!=3)exchange(fd,42,data,2,3,0,mode==1?4:6);}
        close(fd);close(server);_exit(0);
    }
    close(server);int rc;
    if(mode==4){struct sb out={0};rc=native_frontend_prepare(&out);assert(out.p&&strstr(out.p,"\"prepared\":true"));free(out.p);}
    else rc=native_clear_stock_iframe();
    assert(mode==0||mode==4?rc==0:rc==-1);
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
int main(int argc,char **argv){
    assert(argc==2);persist_root=argv[1];snprintf(druid_path,sizeof druid_path,"%s/context",argv[1]);
    FILE *f=fopen(druid_path,"w");assert(f);assert(fwrite("false",1,5,f)==5);assert(!fclose(f));
    native_frontend=1;guard_command=good_guard;
    for(int mode=0;mode<5;mode++)test_case(mode);
    guard_command="printf unsupported";assert(native_clear_stock_iframe()==-1);
    native_frontend=0;assert(!native_clear_stock_iframe());
    puts("PASS exact I-frame-only payload, fragmented replies, unchanged video/graphics, firmware/type/result guards, native Home preparation");
    return 0;
}
