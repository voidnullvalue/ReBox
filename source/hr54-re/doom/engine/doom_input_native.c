/* Same keydispatcher session/per-key transport as the existing input_probe.c.
 * No libuserinput load, global defaults, Druid polling or browser delivery. */
#include "doom_input_native.h"
#include "doomkeys.h"
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#ifdef HR54_RECEIVER
#include "receiver-include/libc.h"
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif
static int fd=-1;
static uint32_t owner;
static unsigned char rx[256];
static size_t used;
static unsigned word(const unsigned char *p) {
    return (unsigned)p[0]<<24 | (unsigned)p[1]<<16 | (unsigned)p[2]<<8 | p[3];
}
static int words(const uint32_t *p,size_t n) {
    unsigned char data[128];
    if (n>sizeof(data)/4) return -1;
    for (size_t i=0;i<n;i++) {
        data[4*i]=p[i]>>24; data[4*i+1]=p[i]>>16; data[4*i+2]=p[i]>>8; data[4*i+3]=p[i];
    }
    return send(fd,data,4*n,MSG_NOSIGNAL)==(ssize_t)(4*n) ? 0 : -1;
}
static int full(void *data,size_t n) {
    size_t off=0;
    while (off<n) {
        ssize_t r=recv(fd,(char *)data+off,n-off,0);
        if (r<0 && errno==EINTR) continue;
        if (r<=0) return -1;
        off+=(size_t)r;
    }
    return 0;
}
void doom_input_close(void) {
    if (fd<0) return;
    uint32_t message[]={12,4,owner};
    int rc=words(message,3); close(fd); fd=-1; used=0;
    fprintf(stderr,"doom-input: close owner=%u command=%d; socket released\n",owner,rc);
}
int doom_input_open(void) {
    /* Direction, select, fire (record/red), use (info), escape (back), enter,
     * and application quit (stop). GUIDE/EXIT have live stock exclusive owners. MENU and global default registrations are untouched. */
    static const uint32_t keys[]={0xe100,0xe101,0xe102,0xe103,0xe001,0xe403,0xe200,0xe00e,0xe002,0xe505,0xe402};
    unsigned char greeting[4],ack[256];
    fd=socket(AF_INET,SOCK_STREAM,0);
    if (fd<0) return -1;
    struct timeval timeout={3,0};
    if (setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) ||
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))) goto fail;
    struct sockaddr_in address={0};
    address.sin_family=AF_INET;
    /* Explicit network bytes keep this source portable to host builds. */
    unsigned char *port=(unsigned char *)&address.sin_port;
    /* Production games are broker consumers. Direct vendor ownership remains
     * available in hr54-ui --probe-input for maintenance diagnostics. */
    port[0]=47953>>8; port[1]=47953&255;
    unsigned char *ip=(unsigned char *)&address.sin_addr;
    ip[0]=127; ip[3]=1;
    if (connect(fd,(const struct sockaddr *)&address,sizeof(address)) || full(greeting,4)) goto fail;
    owner=word(greeting);
    uint32_t session[]={12,16,0},map[6+sizeof(keys)/sizeof(*keys)];
    map[0]=sizeof(map); map[1]=0; map[2]=owner; map[3]=1; map[4]=1;
    map[5]=sizeof(keys)/sizeof(*keys)+1;
    for (size_t i=0;i<sizeof(keys)/sizeof(*keys);i++) map[6+i]=keys[i];
    if (words(session,3) || words(map,sizeof(map)/4) || full(ack,4)) goto fail;
    unsigned len=word(ack);
    if (len<12 || len>sizeof(ack) || len%4 || full(ack+4,len-4)) goto fail;
    if (word(ack+4)!=7 || len!=12 || word(ack+8)!=1) {
        fprintf(stderr,"doom-input: registration rejected (length=%u)\n",len);
        for (unsigned i=8;i<len;i+=4) fprintf(stderr,"doom-input: ack word[%u]=0x%x\n",i/4,word(ack+i));
        goto fail;
    }
    int flags=fcntl(fd,F_GETFL,0);
    if (flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) goto fail;
    fprintf(stderr,"doom-input: owner=%u session=0 device=1 exclusive keys=%u accepted\n",
            owner,(unsigned)(sizeof(keys)/sizeof(*keys)));
    return 0;
fail:
    doom_input_close(); return -1;
}
int doom_input_pump(void (*sink)(unsigned char,int)) {
    if (fd<0) return -1;
    /* Nonblocking stream parser retains partial packets across engine ticks.
     * Called by input acquisition (DG_GetKey), never by texture upload. */
    for (int budget=0;budget<32;budget++) {
        if (used>=4) {
            unsigned n=word(rx);
            if (n<8 || n>sizeof(rx) || n%4) return -1;
            if (used>=n) {
                if (word(rx+4)==8 && (n==12 || n==24 || n==28)) {
                    unsigned raw=word(rx+8),code=raw&65535;
                    int pressed=(raw&0x10000)!=0;
                    unsigned char key=0;
                    switch (code) {
                    case 0xe100:key=KEY_UPARROW;break;
                    case 0xe101:key=KEY_DOWNARROW;break;
                    case 0xe102:key=KEY_LEFTARROW;break;
                    case 0xe103:key=KEY_RIGHTARROW;break;
                    case 0xe001:case 0xe505:key=KEY_ENTER;break;
                    case 0xe403:case 0xe200:key=KEY_RCTRL;break;
                    case 0xe00e:key=' ';break;
                    case 0xe002:key=KEY_ESCAPE;break;
                    case 0xe402:if (pressed) { fprintf(stderr,"doom-input: STOP requested exit\n"); return 1; }break;
                    }
                    fprintf(stderr,"doom-input: raw=0x%x key=0x%x pressed=%d\n",raw,key,pressed);
                    if (key) sink(key,pressed);
                }
                used-=n;
                for (size_t i=0;i<used;i++) rx[i]=rx[n+i];
                continue;
            }
        }
        ssize_t r=recv(fd,rx+used,sizeof(rx)-used,0);
        if (r<0 && (errno==EAGAIN || errno==EINTR)) return 0;
        if (r<=0) return -1;
        used+=(size_t)r;
    }
    return 0;
}
