#ifndef HR54_UI_JSON_H
#define HR54_UI_JSON_H
#include "../platform.h"
#define JSON_MAX_TOKENS 8192
typedef struct { int start,end,next,type,count; } JsonToken;
typedef struct { const char *text; size_t length; int count; JsonToken *t; } Json;
int json_open(Json *,const char *,size_t);
void json_close(Json *);
int json_field(const Json *,int,const char *);
int json_nth(const Json *,int,int);
int json_string(const Json *,int,char *,size_t);
int json_bool(const Json *,int);
long json_number(const Json *,int);
int json_quote(char *,size_t,const char *);
#endif
