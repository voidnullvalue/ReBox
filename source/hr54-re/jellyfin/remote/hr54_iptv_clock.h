/* HTTPS needs a valid certificate clock. Stock cold boots can start in 2024.
 * A bounded SNTP bootstrap runs only for an implausibly old Linux clock.
 * This uses ordinary unauthenticated SNTP, not an HTTPS verification bypass. */
#include <netdb.h>
static int iptv_ntp_epoch(const unsigned char *reply,size_t n,
                          const unsigned char *nonce,time_t *epoch){
    if(n<48||(reply[0]&7)!=4||((reply[0]>>3)&7)<3||
       (reply[0]>>6)==3||reply[1]<1||reply[1]>15||
       memcmp(reply+24,nonce,8))return -1;
    uint64_t stamp=((uint64_t)reply[40]<<24)|((uint64_t)reply[41]<<16)|
                   ((uint64_t)reply[42]<<8)|reply[43];
    if(stamp<UINT64_C(2208988800))stamp+=UINT64_C(4294967296);
    stamp-=UINT64_C(2208988800);
    if(stamp<UINT64_C(1767225600)||stamp>UINT64_C(4102444800))return -1;
    *epoch=(time_t)stamp;return 0;
}
static void iptv_clock_bootstrap(void){
    if(time(NULL)>=(time_t)1767225600)return;
    unsigned char request[48]={0},reply[512];request[0]=0x23;
    int entropy=open("/dev/urandom",O_RDONLY);
    if(entropy<0)return;
    ssize_t got=read(entropy,request+40,8);close(entropy);if(got!=8)return;
    struct addrinfo hints={0},*addresses=NULL;
    hints.ai_family=AF_INET;hints.ai_socktype=SOCK_DGRAM;
    if(getaddrinfo("time.cloudflare.com","123",&hints,&addresses)){
        jf_log("IPTV clock: SNTP DNS failed; HTTPS requires correct Linux time");return;
    }
    int tried=0,ok=0;
    for(struct addrinfo *a=addresses;a&&tried++<2;a=a->ai_next){
        int fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol);if(fd<0)continue;
        if(!connect(fd,a->ai_addr,a->ai_addrlen)&&send(fd,request,48,0)==48){
            struct pollfd p={fd,POLLIN,0};
            if(poll(&p,1,2000)>0){
                ssize_t n=recv(fd,reply,sizeof reply,0);time_t epoch;
                if(n>0&&!iptv_ntp_epoch(reply,(size_t)n,request+40,&epoch)){
                    struct timespec current={epoch,0};
                    if(!clock_settime(CLOCK_REALTIME,&current))ok=1;
                }
            }
        }
        close(fd);if(ok)break;
    }
    freeaddrinfo(addresses);
    jf_log("IPTV clock: %s",ok?"Linux time synchronized by SNTP":"SNTP unavailable; HTTPS requires correct Linux time");
}
