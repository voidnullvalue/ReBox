#include "artwork.h"
#include "image.h"
/* The sole decode worker bounds library allocations as well as dimensions.
 * Compressed-byte and dimension limits alone do not bound decoder workspace. */
typedef union { size_t size;uint64_t align; } ArtAllocation;
static size_t decode_allocated;
static void *decode_malloc(size_t size){if(size>4*1024*1024||size>8*1024*1024-decode_allocated)return NULL;ArtAllocation *p=malloc(sizeof(*p)+size);if(!p)return NULL;p->size=size;decode_allocated+=size;return p+1;}
static void decode_free(void *memory){if(memory){ArtAllocation *p=(ArtAllocation *)memory-1;decode_allocated-=p->size;free(p);}}
static void *decode_realloc(void *memory,size_t size){if(!memory)return decode_malloc(size);ArtAllocation *p=(ArtAllocation *)memory-1;size_t old=p->size;if(size>4*1024*1024||size>8*1024*1024-(decode_allocated-old))return NULL;p=realloc(p,sizeof(*p)+size);if(!p)return NULL;p->size=size;decode_allocated=decode_allocated-old+size;return p+1;}
#define STBI_MALLOC decode_malloc
#define STBI_REALLOC decode_realloc
#define STBI_FREE decode_free
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 1024
#include "../vendor/stb_image.h"
struct ArtJob { unsigned char *bytes;size_t length;int ok,notification[2];unsigned generation;ArtEntry result; };
void artwork_reset(Artwork *a){for(int i=0;i<ART_ENTRIES;i++){free(a->entry[i].pixels);memset(&a->entry[i],0,sizeof(a->entry[i]));}a->pending[0]=0;}
void artwork_free(Artwork *a){if(a->job){pthread_join(a->worker,NULL);close(a->job->notification[0]);close(a->job->notification[1]);free(a->job->bytes);free(a->job->result.pixels);free(a->job);}for(int i=0;i<ART_ENTRIES;i++)free(a->entry[i].pixels);memset(a,0,sizeof(*a));}
const ArtEntry *artwork_find(Artwork *a,const char *id){for(int i=0;i<ART_ENTRIES;i++)if(!strcmp(a->entry[i].id,id)&&*id){a->entry[i].used=ui_now();return &a->entry[i];}return NULL;}
int artwork_accept(Artwork *a,const unsigned char *bytes,size_t length,int ok){if(!*a->pending)return -1;int slot=0;for(int i=0;i<ART_ENTRIES;i++)if(!*a->entry[i].id||a->entry[i].used<a->entry[slot].used)slot=i;ArtEntry *e=&a->entry[slot];free(e->pixels);memset(e,0,sizeof(*e));ui_copy(e->id,sizeof(e->id),a->pending);a->pending[0]=0;e->used=ui_now();int w=0,h=0,n=0;if(!ok||length>2*1024*1024||!stbi_info_from_memory(bytes,(int)length,&w,&h,&n)||w>512||h>512||w<1||h<1){e->failed=1;return -1;}unsigned char *source=stbi_load_from_memory(bytes,(int)length,&w,&h,&n,4);if(!source){e->failed=1;return -1;}e->pixels=calloc(ART_W*ART_H,4);if(!e->pixels){stbi_image_free(source);e->failed=1;return -1;}/* Fit preserves the complete poster; no stretched covers. */int tw=ART_W,th=h*ART_W/w;if(th>ART_H){th=ART_H;tw=w*ART_H/h;}if(tw<1)tw=1;if(th<1)th=1;unsigned char *scaled=malloc((size_t)tw*th*4);if(!scaled){stbi_image_free(source);free(e->pixels);e->pixels=NULL;e->failed=1;return -1;}image_scale(source,w,h,scaled,tw,th);for(int y=0;y<th;y++)memcpy(e->pixels+((y+(ART_H-th)/2)*ART_W+(ART_W-tw)/2)*4,scaled+y*tw*4,(size_t)tw*4);free(scaled);stbi_image_free(source);return 0;}
void artwork_draw(UiFramebuffer *f,const ArtEntry *e,int x,int y){draw_round(f,x,y,ART_W,ART_H,8,UI_PANEL,UI_PANEL);if(e&&e->pixels)ui_fb_blit(f,x,y,e->pixels,ART_W,ART_H,ART_W*4);else{draw_line(f,x+ART_W/2-22,y+ART_H/2-12,x+ART_W/2+22,y+ART_H/2+12,1,UI_EDGE);draw_line(f,x+ART_W/2+22,y+ART_H/2-12,x+ART_W/2-22,y+ART_H/2+12,1,UI_EDGE);}}

/* A single bounded decoder job owns its input and result. The worker never
 * touches application/cache state. A pipe transfers completion to the reactor. */
static void *decode_job(void *context){ArtJob *job=context;Artwork temporary={0};ui_copy(temporary.pending,sizeof(temporary.pending),job->result.id);artwork_accept(&temporary,job->bytes,job->length,job->ok);for(int i=0;i<ART_ENTRIES;i++)if(*temporary.entry[i].id){job->result=temporary.entry[i];break;}unsigned char done=1;while(write(job->notification[1],&done,1)<0&&errno==EINTR){}return NULL;}
int artwork_queue(Artwork *a,const unsigned char *bytes,size_t length,int ok,unsigned generation){
    if(a->job||!*a->pending)return -1;ArtJob *job=calloc(1,sizeof(*job));if(!job)return -1;
    job->ok=ok;job->length=length;job->generation=generation;ui_copy(job->result.id,sizeof(job->result.id),a->pending);
    if(length>2*1024*1024){job->length=0;job->ok=0;}if(job->length){job->bytes=malloc(length);if(!job->bytes){free(job);return -1;}memcpy(job->bytes,bytes,length);}
    if(pipe(job->notification)){free(job->bytes);free(job);return -1;}fcntl(job->notification[0],F_SETFL,O_NONBLOCK);fcntl(job->notification[0],F_SETFD,FD_CLOEXEC);fcntl(job->notification[1],F_SETFD,FD_CLOEXEC);
    if(pthread_create(&a->worker,NULL,decode_job,job)){close(job->notification[0]);close(job->notification[1]);free(job->bytes);free(job);return -1;}
    a->job=job;a->pending[0]=0;return 0;
}
int artwork_poll(Artwork *a,unsigned generation){
    if(!a->job)return 0;unsigned char done;ssize_t n=read(a->job->notification[0],&done,1);if(n<0&&(errno==EAGAIN||errno==EINTR))return 0;if(n!=1)return 0;
    ArtJob *job=a->job;pthread_join(a->worker,NULL);close(job->notification[0]);close(job->notification[1]);free(job->bytes);int changed=job->generation==generation;
    if(changed){int slot=0;for(int i=0;i<ART_ENTRIES;i++)if(!*a->entry[i].id||a->entry[i].used<a->entry[slot].used)slot=i;free(a->entry[slot].pixels);a->entry[slot]=job->result;a->entry[slot].used=ui_now();}else free(job->result.pixels);
    free(job);a->job=NULL;return changed;
}
