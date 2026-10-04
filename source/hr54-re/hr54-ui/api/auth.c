#include "internal.h"
int api_auth_status(ApiClient *a){return get(a,API_BROWSE,API_AUTH,"/api/auth/status",8000);}
int api_auth_pair(ApiClient *a){return post(a,API_OPERATION,API_PAIR,"/api/auth/start","{}",15000);}
int api_auth_poll(ApiClient *a){return get(a,API_STATUS,API_POLL,"/api/auth/poll",8000);}
int api_auth_logout(ApiClient *a){return post(a,API_OPERATION,API_LOGOUT,"/api/auth/logout","{}",8000);}
