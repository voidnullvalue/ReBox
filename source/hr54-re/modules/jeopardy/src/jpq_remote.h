#ifndef JPQ_REMOTE_H
#define JPQ_REMOTE_H
/* TV remote model: maps broker keys to trusted native commands.
 *
 * The TV is the host controller. It keeps only UI cursor/menu state locally;
 * every gameplay change is expressed as a JSON command for the private
 * command.sock, where the service validates the actor. No rules run here. */
#include "jpq_view.h"

enum {
    JRM_NONE = 0, JRM_PAUSE, JRM_SETTINGS, JRM_CONFIRM_QUIT, JRM_CONFIRM_RESTART,
    JRM_HOST, JRM_TIMERS, JRM_SCORE
};

typedef struct {
    int col, row;      /* board focus */
    int menu;          /* active overlay */
    int cursor;        /* overlay cursor */
    int stop_pending;  /* STOP arms a confirmation before quitting */
    int show_host;     /* settings shows the host PIN when opened */
    int timer_field;   /* timers submenu field index */
    int32_t t_read, t_buzz, t_answer; /* local timer edits */
    int score_player;  /* score-edit submenu player index */
    int32_t score_value; /* local score edit */
} JpqRemote;

void jpq_remote_init(JpqRemote *);
/* Apply a key. Returns 1 when `out` holds a NUL-terminated JSON command line
 * (no newline), 0 when only local UI state changed, -1 on a bad argument. */
int jpq_remote_key(JpqRemote *, JvKey, const JpqView *, char *out, size_t cap);
/* Label for the active overlay, or "" when none. */
const char *jpq_remote_menu_label(const JpqRemote *);
#endif
