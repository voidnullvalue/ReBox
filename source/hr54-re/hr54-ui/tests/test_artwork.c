#include "ui/artwork.h"
#include <assert.h>
static void finish(Artwork *art,unsigned generation) {
    uint64_t end=ui_now()+3000;
    while(art->job&&ui_now()<end){artwork_poll(art,generation);struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
    assert(!art->job);
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);
    unsigned char *bytes=malloc(2*1024*1024);assert(bytes);size_t length=fread(bytes,1,2*1024*1024,file);fclose(file);
    Artwork art={0};ui_copy(art.pending,sizeof(art.pending),"poster");
    assert(!artwork_queue(&art,bytes,length,1,1));
    assert(artwork_queue(&art,bytes,length,1,1)<0); /* Only one decode in flight. */
    memset(bytes,0,length);free(bytes); /* Worker owns a copy, not HTTP memory. */
    finish(&art,1);const ArtEntry *entry=artwork_find(&art,"poster");assert(entry&&entry->pixels&&!entry->failed);
    assert(entry->pixels[((ART_H/2)*ART_W+ART_W/2)*4+3]==255);
    ui_copy(art.pending,sizeof(art.pending),"stale");assert(!artwork_queue(&art,(const unsigned char *)"bad",3,1,2));finish(&art,3);assert(!artwork_find(&art,"stale"));
    ui_copy(art.pending,sizeof(art.pending),"broken");assert(!artwork_queue(&art,(const unsigned char *)"bad",3,1,3));finish(&art,3);assert(artwork_find(&art,"broken")->failed);
    artwork_free(&art);puts("PASS async artwork ownership, one-worker bound, fitting, stale decode cancellation, invalid image fallback");return 0;
}
