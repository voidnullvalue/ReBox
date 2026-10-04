#include "internal.h"
int api_playback_start(ApiClient *a,Source source,const MediaItem *item){static const char *paths[]={"/api/play","/api/iptv/play","/api/frigate/play","/api/youtube/play"};static const char *keys[]={"itemId","channelId","cameraId","videoId"};if(source>SOURCE_YOUTUBE)return -1;char id[400],body[512];if(json_quote(id,sizeof(id),item->id))return -1;snprintf(body,sizeof(body),"{\"%s\":%s,\"startSeconds\":%ld,\"returnToTv\":false}",keys[source],id,item->resume);return post(a,API_OPERATION,API_PLAY,paths[source],body,240000);}
int api_playback_stop(ApiClient *a,Source source){const char *path=source==SOURCE_IPTV?"/api/iptv/stop":source==SOURCE_YOUTUBE?"/api/youtube/stop":"/api/playback/stop";return post(a,API_CONTROL,API_STOP,path,"{}",30000);}
int api_playback_pause(ApiClient *a,int paused){return post(a,API_CONTROL,API_PAUSE,paused?"/api/playback/resume":"/api/playback/pause","{}",30000);}
int api_playback_seek(ApiClient *a,int delta){char b[64];snprintf(b,sizeof(b),"{\"delta\":%d}",delta);return post(a,API_CONTROL,API_SEEK,"/api/playback/seek",b,240000);}
int api_playback_state(ApiClient *a){return get(a,API_STATUS,API_STATE,"/api/state",5000);}
