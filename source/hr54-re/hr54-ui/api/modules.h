#ifndef HR54_UI_MODULES_H
#define HR54_UI_MODULES_H
#include "models.h"
#define REBOX_MAX_MODULES 32
#define MODULE_FIELDS 12
#define MODULE_CHOICES 8
#define MODULE_ACTIONS 12
typedef struct {
    char id[64],name[128],version[64],description[256],error[160];
    int installed,enabled,healthy,compatible,core,home,order,native_app;
    int browse,search,settings,auth,playback,release_surface,release_input;
} ModuleDescriptor;
typedef struct {ModuleDescriptor items[REBOX_MAX_MODULES];int count;} ModuleList;
typedef enum { FIELD_BOOL,FIELD_STRING,FIELD_INTEGER,FIELD_CHOICE } FieldType;
typedef struct {char value[256],label[128];int quoted;} ModuleChoice;
typedef struct {
    char key[64],label[128],value[256];FieldType type;int quoted;
    ModuleChoice choices[MODULE_CHOICES];int choice_count;
} ModuleField;
typedef struct {char id[64],label[128];} ModuleAction;
typedef struct {ModuleField fields[MODULE_FIELDS];ModuleAction actions[MODULE_ACTIONS];int field_count,action_count;} ModuleSettings;
typedef struct {int ready,prepared,busy,running,authenticated,pending;char code[40],message[256],poll_action[64],native_module[64];} OperationResult;
int api_module_id(const char *);
int api_parse_modules(const ApiResponse *,ModuleList *);
int api_parse_media(const ApiResponse *,MediaList *);
int api_parse_playback(const ApiResponse *,PlaybackState *);
int api_parse_settings(const ApiResponse *,ModuleSettings *);
int api_parse_operation(const ApiResponse *,OperationResult *);
int api_modules(ApiClient *);
int api_module_browse(ApiClient *,const char *,const char *,const char *,int);
int api_module_play(ApiClient *,const char *,const MediaItem *);
int api_module_settings(ApiClient *,const char *);
int api_module_save(ApiClient *,const char *,const ModuleField *);
int api_module_action(ApiClient *,const char *,const char *);
int api_module_manage(ApiClient *,const char *,const char *);
int api_module_install(ApiClient *,const char *);
int api_management_pair_open(ApiClient *);
int api_module_native(ApiClient *,const char *,const char *);
int api_module_image(ApiClient *,const char *,const char *);
#endif
