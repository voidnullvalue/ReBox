#ifndef HR54_UI_CLIENT_H
#define HR54_UI_CLIENT_H
#include "json.h"
#define API_SLOTS 5
#define API_BROWSE 0
#define API_OPERATION 1
#define API_STATUS 2
#define API_ARTWORK 3
#define API_CONTROL 4
typedef enum { API_NONE,API_READY,API_AUTH,API_PAIR,API_POLL,API_LOGOUT,API_LIBRARIES,API_ITEMS,API_RESUME,API_GROUPS,API_CHANNELS,API_CAMERAS,API_SEARCH,API_PLAY,API_STOP,API_PAUSE,API_SEEK,API_STATE,API_SETTINGS,API_SAVE_SETTINGS,API_DOOM_START,API_DOOM_STATUS,API_DOOM_STOP,API_EXIT,API_IMAGE,API_PREPARE } ApiKind;
typedef enum { API_OK,API_UNAVAILABLE,API_TIMEOUT,API_MALFORMED,API_UNAUTHORIZED,API_FAILED } ApiError;
typedef struct { int fd,phase,slot,attempt,status; ApiKind kind; unsigned generation; uint64_t deadline,retry_at; unsigned timeout; char request[4096]; size_t sent,size,used,cap,header,body_length; char *data; int is_get,has_length; } ApiRequest;
typedef struct { int port; unsigned generation; ApiRequest r[API_SLOTS]; } ApiClient;
typedef struct { ApiKind kind; ApiError error; unsigned generation; int status; const unsigned char *bytes; size_t length; } ApiResponse;
typedef void (*ApiSink)(void *,const ApiResponse *);
void api_init(ApiClient *,int);
void api_cancel(ApiClient *,int);
void api_close(ApiClient *);
int api_send(ApiClient *,int,ApiKind,const char *,const char *,const char *,unsigned);
void api_pump(ApiClient *,uint64_t,ApiSink,void *);
int api_busy(const ApiClient *,int);
int api_encode(char *,size_t,const char *);
const char *api_error_message(ApiError);
#endif
