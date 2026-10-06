#include "../api/modules.h"
#include <assert.h>
static ApiResponse response(const char *json){ApiResponse r={.error=API_OK,.bytes=(const unsigned char *)json,.length=strlen(json)};return r;}
int main(void){
    ModuleList list;char json[20000];
    for(int count=0;count<=33;count++){
        int used=snprintf(json,sizeof json,"{\"moduleApi\":1,\"modules\":[");
        for(int i=0;i<count;i++)used+=snprintf(json+used,sizeof json-used,"%s{\"id\":\"unrecognized-%d\",\"name\":\"Module %d\",\"installed\":true,\"enabled\":true,\"healthy\":true,\"capabilities\":{\"browse\":true,\"search\":true}}",i?",":"",i,i);
        snprintf(json+used,sizeof json-used,"]}");ApiResponse r=response(json);
        assert((api_parse_modules(&r,&list)==0)==(count<=REBOX_MAX_MODULES));if(count<=32)assert(list.count==count);
    }
    ApiResponse r=response("{\"moduleApi\":1,\"modules\":[{\"id\":\"../escape\",\"name\":\"x\"}]}");assert(api_parse_modules(&r,&list));
    r=response("{\"moduleApi\":1,\"modules\":[{\"id\":\"one\",\"name\":\"x\"},{\"id\":\"one\",\"name\":\"y\"}]}");assert(api_parse_modules(&r,&list));
    r=response("{\"moduleApi\":1,\"modules\":[{\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"name\":\"x\"}]}");assert(api_parse_modules(&r,&list));
    MediaList *media=calloc(1,sizeof *media);assert(media);
    r=response("{\"items\":[{\"id\":\"opaque:item/1\",\"title\":\"One\",\"kind\":\"item\",\"playable\":true,\"artwork\":\"picture/1\",\"duration\":12,\"resume\":2},{\"id\":\"folder\",\"title\":\"Folder\",\"kind\":\"folder\"}],\"offset\":60,\"total\":100,\"hasMore\":true}");
    assert(!api_parse_media(&r,media));assert(media->count==2&&media->offset==60&&media->has_more);assert(media->item[1].folder);assert(!strcmp(media->item[0].id,"opaque:item/1"));free(media);
    PlaybackState p;r=response("{\"playing\":true,\"source\":\"unknown-service\",\"generation\":8,\"transport\":{\"stop\":true}}");assert(!api_parse_playback(&r,&p));assert(!strcmp(p.source,"unknown-service")&&p.can_stop&&p.generation==8);
    ModuleSettings *settings=calloc(1,sizeof *settings);assert(settings);
    r=response("{\"fields\":[{\"key\":\"quality\",\"label\":\"Quality\",\"type\":\"choice\",\"value\":12,\"choices\":[{\"value\":8,\"label\":\"Low\"},{\"value\":12,\"label\":\"High\"}]}],\"actions\":[{\"id\":\"connect.start\",\"label\":\"Connect\"}]}");
    assert(!api_parse_settings(&r,settings));assert(settings->field_count==1&&settings->action_count==1);assert(settings->fields[0].choice_count==2&&!settings->fields[0].quoted);free(settings);
    Json j;assert(!json_open(&j,"\"abcdef\"",8));char bounded[4];assert(json_string(&j,0,bounded,sizeof bounded));json_close(&j);
    assert(!api_module_id("-bad")&&!api_module_id("Upper")&&api_module_id("new.one-2"));
    printf("generic module models: PASS (descriptor=%zu, module list=%zu, media list=%zu, settings=%zu bytes)\n",sizeof(ModuleDescriptor),sizeof(ModuleList),sizeof(MediaList),sizeof(ModuleSettings));return 0;
}
