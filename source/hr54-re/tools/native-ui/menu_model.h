#ifndef HR54_MENU_MODEL_H
#define HR54_MENU_MODEL_H
#include "framebuffer.h"
/* Native dispatcher adapter normalizes existing raw keys to these actions. */
typedef enum { UI_KEY_MENU, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_SELECT,
               UI_KEY_BACK, UI_KEY_PLAY_PAUSE, UI_KEY_STOP } UiKey;
typedef struct {
    int visible, selected, previous, full_redraw, paused;
    unsigned progress, painted_progress;
    void *control;
    void (*select_source)(void *, int); /* existing playURL adapter */
    void (*set_rate)(void *, int);      /* existing rate 0 / 1000 */
    void (*stop)(void *);
} UiMenu;
void ui_menu_init(UiMenu *);
int ui_menu_key(UiMenu *, UiKey);
int ui_menu_progress(UiMenu *, unsigned);
void ui_menu_render(UiMenu *, UiFramebuffer *);
#endif
