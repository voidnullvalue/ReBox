#include "internal.h"
int api_system_prepare(ApiClient *a){return post(a,API_CONTROL,API_PREPARE,"/api/system/frontend/prepare","{}",16000);}
int api_system_ready(ApiClient *a){return get(a,API_STATUS,API_READY,"/api/system/status",3000);}
int api_doom_start(ApiClient *a){return post(a,API_OPERATION,API_DOOM_START,"/api/doom/start","{}",10000);}
int api_doom_status(ApiClient *a){return get(a,API_STATUS,API_DOOM_STATUS,"/api/doom/status",5000);}
int api_doom_stop(ApiClient *a){return post(a,API_CONTROL,API_DOOM_STOP,"/api/doom/stop","{}",10000);}
int api_tv_exit(ApiClient *a){return post(a,API_CONTROL,API_EXIT,"/api/tv/exit","{}",20000);}
