#include "internal.h"
int api_frigate_cameras(ApiClient *a){return get(a,API_BROWSE,API_CAMERAS,"/api/frigate/cameras",15000);}
