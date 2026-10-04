#include "internal.h"
int api_youtube_search(ApiClient *a,const char *query,int page){char q[512],path[768];if(api_encode(q,sizeof(q),query))return -1;snprintf(path,sizeof(path),"/api/youtube/search?q=%s&page=%d",q,page);return get(a,API_BROWSE,API_SEARCH,path,70000);}
