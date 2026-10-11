#ifndef JPQ_VIEW_H
#define JPQ_VIEW_H
/* Native renderer view model.
 *
 * The renderer is a dumb consumer: it paints only the redacted public display
 * written by the authoritative service. It never sees the host snapshot, never
 * reads the LAN, and never computes rules. This header is shared by the uClibc
 * renderer, the host preview tool and the host tests. */
#include <stddef.h>
#include <stdint.h>

#define JPQ_VIEW_COLS 6
#define JPQ_VIEW_ROWS 5
#define JPQ_VIEW_PLAYERS 6
#define JPQ_NAME_MAX 24
/* Clue/response text is stored whole from the importer (JEPDB1 lengths are
 * u16); the view keeps the full importer cap rather than truncating mid
 * sentence. The renderer wraps and adapts the font to fit. */
#define JPQ_TEXT_MAX 2048
#define JPQ_ANSWER_VIEW_MAX 96
#define JPQ_LABEL_MAX 96
#define JPQ_URL_MAX 256

/* Phases mirror jeopardy.h; the renderer branches on these only. */
enum {
    JV_LOBBY = 0, JV_BOARD, JV_CLUE, JV_DD_WAGER, JV_DD_CLUE, JV_FINAL_WAGER,
    JV_FINAL_CLUE, JV_FINAL_JUDGE, JV_RESULTS, JV_TIEBREAK_CLUE, JV_REVEAL
};
enum { JV_ROUND_J = 1, JV_ROUND_DJ = 2, JV_ROUND_FINAL = 3, JV_ROUND_TIEBREAK = 4 };
enum { JV_TIMER_NONE = 0, JV_TIMER_READ, JV_TIMER_BUZZ, JV_TIMER_ANSWER, JV_TIMER_DD_WAGER,
       JV_TIMER_DD_ANSWER, JV_TIMER_FINAL_WAGER, JV_TIMER_FINAL_ANSWER, JV_TIMER_TIEBREAK_ANSWER };

/* Remote / controller keys. Codes match the native broker and the phone map. */
typedef enum {
    JV_KEY_NONE = 0, JV_KEY_UP, JV_KEY_DOWN, JV_KEY_LEFT, JV_KEY_RIGHT,
    JV_KEY_OK, JV_KEY_RED, JV_KEY_INFO, JV_KEY_BACK, JV_KEY_STOP
} JvKey;

typedef struct {
    uint32_t id;
    char name[JPQ_NAME_MAX + 1];
    int32_t score;
    uint8_t ready, connected, active, can_buzz, excluded, eligible, wager_set;
    uint8_t answered, judged, correct;
    int32_t wager, wager_min, wager_max;
    uint8_t has_wager, has_wager_bounds;
    uint8_t has_answer;                 /* public submitted final answer */
    char answer[JPQ_ANSWER_VIEW_MAX + 1];
    char color[16];                     /* service-chosen podium colour name */
} JvPlayer;

typedef struct {
    char name[JPQ_LABEL_MAX + 1];
    int32_t value[JPQ_VIEW_ROWS];
    uint8_t used[JPQ_VIEW_ROWS];
    uint8_t daily_double[JPQ_VIEW_ROWS]; /* public display always 0 */
} JvCategory;

typedef struct {
    int present, has_text, has_response;
    int col, row;
    int32_t value;
    uint8_t daily_double;
    char category[JPQ_LABEL_MAX + 1];
    char text[JPQ_TEXT_MAX + 1];
    char response[JPQ_TEXT_MAX + 1];
} JvClue;

typedef struct {
    int present, has_text, has_response;
    char category[JPQ_LABEL_MAX + 1];
    char text[JPQ_TEXT_MAX + 1];
    char response[JPQ_TEXT_MAX + 1];
} JvFinal;

typedef struct {
    uint32_t window;
    uint64_t now_ms;
    uint32_t epoch, revision;
    uint8_t phase, round, paused, tiebreak;
    int32_t buzzed;                    /* signed answerer slot, -1 for none */
    int32_t selector;                  /* board controller slot */
    int32_t dd_selector;               /* daily-double wager slot, -1 for none */
    int32_t tiebreak_winner;
    uint8_t judge_cursor;
    uint8_t quit_confirm, restart_confirm;
    int quit;
    uint8_t timer_kind;
    uint64_t timer_remaining_ms;
    uint32_t cfg_read_ms, cfg_buzz_ms, cfg_answer_ms;
    int player_count;
    JvPlayer players[JPQ_VIEW_PLAYERS];
    JvCategory categories[JPQ_VIEW_COLS];
    JvClue clue;
    JvFinal final;
    int leader, tie;
    int valid;                 /* 1 once a snapshot has been parsed */
} JpqView;

/* Private metadata written by the service for the TV remote. */
typedef struct {
    char url[JPQ_URL_MAX + 1];
    char join_pin[16];
    char host_pin[16];
    char display_path[512];
    char command_socket[512];
    int port, protocol, version;
} JpqMetadata;

/* Parse a public-display.json buffer (the wrapper {"window","now_ms","state"}). */
int jpq_view_parse(JpqView *, const char *text, size_t len);
/* Parse metadata.json. Host PIN is present; the renderer only shows it behind
 * an explicit host-controls screen. */
int jpq_metadata_parse(JpqMetadata *, const char *text, size_t len);

/* Longest/shortest active-clue timer remaining at now_ms (never negative). */
uint64_t jpq_view_timer_remaining(const JpqView *view, uint64_t now_ms);
#endif
