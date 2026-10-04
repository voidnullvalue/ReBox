#ifndef HR54_UI_SERVICES_H
#define HR54_UI_SERVICES_H
#include "models.h"
typedef enum { SOURCE_JELLYFIN,SOURCE_IPTV,SOURCE_FRIGATE,SOURCE_YOUTUBE,SOURCE_DOOM } Source;
int api_system_ready(ApiClient *);
int api_system_prepare(ApiClient *);
int api_auth_status(ApiClient *);
int api_auth_pair(ApiClient *);
int api_auth_poll(ApiClient *);
int api_auth_logout(ApiClient *);
int api_jellyfin_libraries(ApiClient *);
int api_jellyfin_items(ApiClient *,const char *,const char *,int,int);
int api_iptv_groups(ApiClient *,int);
int api_iptv_channels(ApiClient *,const char *,const char *,int,int);
int api_frigate_cameras(ApiClient *);
int api_youtube_search(ApiClient *,const char *,int);
int api_playback_start(ApiClient *,Source,const MediaItem *);
int api_playback_stop(ApiClient *,Source);
int api_playback_pause(ApiClient *,int);
int api_playback_seek(ApiClient *,int);
int api_playback_state(ApiClient *);
int api_settings_get(ApiClient *);
int api_settings_save(ApiClient *,int);
int api_doom_start(ApiClient *);
int api_doom_status(ApiClient *);
int api_doom_stop(ApiClient *);
int api_tv_exit(ApiClient *);
int api_artwork_get(ApiClient *,const char *);
#endif
