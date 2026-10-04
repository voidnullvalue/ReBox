#include "internal.h"
int api_settings_get(ApiClient *a){return get(a,API_BROWSE,API_SETTINGS,"/api/settings",8000);}
int api_settings_save(ApiClient *a,int bitrate){char b[80];snprintf(b,sizeof(b),"{\"jellyfinVideoBitrate\":%d}",bitrate);return post(a,API_OPERATION,API_SAVE_SETTINGS,"/api/settings",b,8000);}
