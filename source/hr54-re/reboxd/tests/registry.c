#include "module_registry.h"
#include "module_manifest.h"
int main(int argc,char **argv) {
    if(argc<2)return 2;
    ReboxRegistry *r=calloc(1,sizeof *r);
    if(rb_registry_load(r,argv[1])){fprintf(stderr,"%s\n",g_err);free(r);return 1;}
    if(argc==4){ReboxModule *m=rb_registry_find(r,argv[2]);if(!m){free(r);return 2;}m->enabled=atoi(argv[3]);if(rb_state_save(r,m)){free(r);return 1;}}
    struct sb b={0};rb_registry_json(r,&b);puts(b.p);free(b.p);free(r);return 0;
}
