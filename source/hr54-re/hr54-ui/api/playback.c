#include "internal.h"
int api_playback_stop(ApiClient *a){return post(a,API_CONTROL,API_STOP,"/api/playback/stop","{}",30000);}
int api_playback_pause(ApiClient *a,int resume){return post(a,API_CONTROL,API_PAUSE,resume?"/api/playback/resume":"/api/playback/pause","{}",120000);}
int api_playback_seek(ApiClient *a,int delta){char b[64];snprintf(b,sizeof b,"{\"delta\":%d}",delta);return post(a,API_CONTROL,API_SEEK,"/api/playback/seek",b,240000);}
int api_playback_state(ApiClient *a){return get(a,API_STATUS,API_STATE,"/api/state",5000);}
