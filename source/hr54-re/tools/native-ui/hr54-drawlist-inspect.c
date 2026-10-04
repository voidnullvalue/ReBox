/* Optional read-only diagnostic, separate from presentation. Recovered offsets
 * apply to the hash-matched vendor library. No locks, writes or vendor init.
 * Concurrent snapshots may be stale; never use these records as an owned ABI. */
#include "receiver_runtime.h"
#include "compositor_protocol.h"
extern int shmget(int,size_t,int);
extern void *shmat(int,const void *,int);
extern int shmdt(const void *);
static unsigned word(const volatile uint8_t *p) {
    return (unsigned)p[0]<<24 | (unsigned)p[1]<<16 | (unsigned)p[2]<<8 | p[3];
}
static unsigned half(const volatile uint8_t *p) { return (unsigned)p[0]<<8 | p[1]; }
int main(int argc,char **argv) {
    (void)argc; (void)argv;
    int id=shmget(HR54_DRAWLIST_SHM_KEY,HR54_DRAWLIST_SHM_BYTES,0);
    if (id<0) { fprintf(stderr,"drawlist: existing shm unavailable\n"); return 1; }
    const volatile uint8_t *base=shmat(id,0,0x1000); /* SHM_RDONLY */
    if (base==(void *)-1) return 1;
    fprintf(stderr,"drawlist: READ ONLY asynchronous snapshot shmid=%d\n",id);
    for (unsigned host=0;host<9;host++) {
        unsigned at=word(base+0x80+40*host),count=0;
        uint8_t seen[HR54_DRAWLIST_MAX_SURFACES]={0};
        fprintf(stderr,"host=%u head=%d refs=%u retirement=0x%x\n",host,(int)at,
                word(base+0x94+40*host),word(base+0x98+40*host));
        while (at<HR54_DRAWLIST_MAX_SURFACES && !seen[at] && count++<HR54_DRAWLIST_MAX_SURFACES) {
            seen[at]=1;
            const volatile uint8_t *r=base+HR54_DRAWLIST_SURFACE_BASE+60*at;
            fprintf(stderr,"surface=%u target=%u depth=%d res=0x%x host=%u owner=%u flags=0x%x pending=%u build=%u head=%u tail=%u retained=%u next=%d\n",
                    at,half(r+4),(int)word(r+8),word(r+0x14),r[0x24],r[0x25],r[0x3a],
                    word(r+0x2c),half(r+0x30),half(r+0x32),half(r+0x34),half(r+0x36),(int)word(r));
            at=word(r);
        }
        if (at!=0xffffffff && at>=HR54_DRAWLIST_MAX_SURFACES)
            fprintf(stderr,"host=%u invalid/changing chain=%u\n",host,at);
    }
    return shmdt((const void *)base) ? 1 : 0;
}
