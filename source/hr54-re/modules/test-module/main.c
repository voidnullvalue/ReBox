#include "../shared/sdk.h"
static void handle(int fd,const RbRequest *r) {
    if(!strcmp(r->path,"/status")){rb_http_json(fd,200,"{\"ok\":true,\"moduleApi\":1}");return;}
    char parent[256]="",q[256]="";const char *query=strchr(r->path,'?');query_param(query?query+1:"","parent",parent,sizeof parent);query_param(query?query+1:"","q",q,sizeof q);
    if(!strncmp(r->path,"/browse",7)||!strncmp(r->path,"/search",7)){
        const char *items=*parent?"[{\"id\":\"item-2\",\"title\":\"Item 2\",\"kind\":\"item\",\"playable\":true}]":
        *q?(!strcmp(q,"foo")?"[{\"id\":\"item-1\",\"title\":\"Item 1\",\"kind\":\"item\",\"playable\":true}]":"[]"):
        "[{\"id\":\"folder-a\",\"title\":\"Folder A\",\"kind\":\"folder\",\"playable\":false},{\"id\":\"item-1\",\"title\":\"Item 1\",\"kind\":\"item\",\"playable\":true}]";
        struct sb out={0};sb_fmt(&out,"{\"ok\":true,\"items\":%s,\"offset\":0,\"total\":%d,\"hasMore\":false}",items,*parent||*q?(!strcmp(items,"[]")?0:1):2);rb_http_json(fd,200,out.p);free(out.p);
    } else if(!strcmp(r->path,"/play")&&!strcmp(r->method,"POST"))rb_http_json(fd,200,"{\"ok\":true,\"type\":\"stream\",\"title\":\"Item 1\",\"live\":false,\"duration\":120,\"stream\":{\"kind\":\"moduleProxy\",\"token\":\"fixture-session-1\"},\"transport\":{\"stop\":true},\"session\":\"fixture-session-1\"}");
    else if(!strcmp(r->path,"/playback/stop"))rb_http_json(fd,200,"{\"ok\":true,\"stopped\":true}");
    else rb_http_error(fd,404,"not found");
}
int main(void){return rb_module_serve(handle);}
