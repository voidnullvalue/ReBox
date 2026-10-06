#ifndef HR54_UI_SCREENS_H
#define HR54_UI_SCREENS_H
#include "../apps/state.h"
void render_module_icon(UiFramebuffer *,App *,const char *,int,int,int,int);
void render_home(UiFramebuffer *,App *,uint64_t);
void render_browser(UiFramebuffer *,App *);
void render_keyboard(UiFramebuffer *,App *);
void render_player(UiFramebuffer *,App *);
void render_settings(UiFramebuffer *,App *);
void render_screen(UiFramebuffer *,App *,uint64_t);
void render_header(UiFramebuffer *,const char *,const char *);
void render_footer(UiFramebuffer *,const char *,const char *);
void render_button(UiFramebuffer *,int,int,int,const char *,int);
#endif
