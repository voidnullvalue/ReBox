#include "internal.h"
int api_artwork_get(ApiClient *a,const char *id){char s[512],path[768];if(api_encode(s,sizeof(s),id))return -1;snprintf(path,sizeof(path),"/art/native/%s.jpg",s);return get(a,API_ARTWORK,API_IMAGE,path,6000);}
