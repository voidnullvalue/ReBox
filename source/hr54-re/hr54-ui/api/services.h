#ifndef HR54_UI_SERVICES_H
#define HR54_UI_SERVICES_H
#include "models.h"
int api_system_ready(ApiClient *);
int api_system_prepare(ApiClient *);
int api_playback_stop(ApiClient *);
int api_playback_pause(ApiClient *,int);
int api_playback_seek(ApiClient *,int);
int api_playback_state(ApiClient *);
int api_tv_exit(ApiClient *);
#endif
