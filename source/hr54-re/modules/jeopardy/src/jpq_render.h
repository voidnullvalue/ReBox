#ifndef JPQ_RENDER_H
#define JPQ_RENDER_H
/* Theatrical native scene renderer. Pure function of the redacted view. */
#include "jpq_view.h"
#include "jpq_remote.h"
#include "../../../tools/native-ui/framebuffer.h"

/* Render the scene implied by view->phase into an RGBA framebuffer.
 * now_ms is the receiver monotonic clock; anim_ms is the monotonic time the
 * current scene (phase/window) began, used for reveal/tween animation. Both may
 * be 0 for a static preview frame. metadata may be NULL (metadata-free hosts).
 * prev_score (may be NULL) supplies the scoreboard values to tween from. */
void jpq_render(UiFramebuffer *, const JpqView *, const JpqMetadata *metadata,
                uint64_t now_ms, uint64_t anim_ms, const int32_t *prev_score);

/* Build the lobby QR payload: the LAN URL with the join PIN in the fragment. */
void jpq_qr_payload(char *out, size_t cap, const JpqMetadata *metadata);

/* Draw the active remote overlay (pause/settings/confirm/host/timers). */
void jpq_render_overlay(UiFramebuffer *, const JpqRemote *, const JpqMetadata *, const JpqView *);

/* Draw a one-line command-channel status/error banner (empty = nothing). */
void jpq_render_status(UiFramebuffer *, const char *status);

/* Podium index the TV highlights (buzzed/wagerer during a clue, else leader). */
int jpq_render_focus_podium(const JpqView *);
#endif
