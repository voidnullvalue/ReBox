#ifndef REBOX_JEOPARDY_H
#define REBOX_JEOPARDY_H
/*
 * Jeopardy stage 1: authoritative rules / state machine, question database and
 * atomic persistence.
 *
 * This translation unit set is the single source of truth for gameplay. The
 * later stage 2 networking service is the same code compiled for static musl;
 * the native uClibc renderer is a dumb consumer of the JSON display snapshots
 * produced here and is never authoritative. No client-supplied timing is
 * trusted: every action carries the server monotonic millisecond clock and the
 * expected epoch/revision/sequence, which also provides replay protection.
 */
#include <stddef.h>
#include <stdint.h>

#define JPQ_MAX_PLAYERS 6
#define JPQ_COLS 6
#define JPQ_ROWS 5
#define JPQ_CELLS (JPQ_COLS * JPQ_ROWS)
#define JPQ_MAX_DD 2
#define JPQ_NAME_MAX 24
#define JPQ_ANSWER_MAX 96
/* Whole clue/response text is preserved (JEPDB1 lengths are u16); the view and
 * renderer keep the importer cap and wrap/adapt rather than truncate. */
#define JPQ_TEXT_MAX 2048

/* Rounds. Final and tiebreak share the single-clue machinery. */
enum {
    JPQ_ROUND_J = 1,
    JPQ_ROUND_DJ = 2,
    JPQ_ROUND_FINAL = 3,
    JPQ_ROUND_TIEBREAK = 4
};

/* Phases. JPQ_PHASE_REVEAL is a host-advanced answer-reveal hold that keeps the
 * active clue and its correct response on screen before returning to the board. */
enum {
    JPQ_PHASE_LOBBY = 0,
    JPQ_PHASE_BOARD,        /* board displayed, host selects a clue */
    JPQ_PHASE_CLUE,         /* reading / buzzing / answering a normal clue */
    JPQ_PHASE_DD_WAGER,     /* daily double selector wagers privately */
    JPQ_PHASE_DD_CLUE,      /* daily double selector answers privately */
    JPQ_PHASE_FINAL_WAGER,  /* eligible players wager privately */
    JPQ_PHASE_FINAL_CLUE,   /* eligible players answer privately */
    JPQ_PHASE_FINAL_JUDGE,  /* host judges answers sequentially */
    JPQ_PHASE_RESULTS,
    JPQ_PHASE_TIEBREAK_CLUE,
    JPQ_PHASE_REVEAL        /* resolved clue held for host reveal, then advance */
};

/* Timer slots; kind selects which configured duration is in force. */
enum {
    JPQ_TIMER_NONE = 0,
    JPQ_TIMER_READ,
    JPQ_TIMER_BUZZ,
    JPQ_TIMER_ANSWER,
    JPQ_TIMER_DD_WAGER,
    JPQ_TIMER_DD_ANSWER,
    JPQ_TIMER_FINAL_WAGER,
    JPQ_TIMER_FINAL_ANSWER,
    JPQ_TIMER_TIEBREAK_ANSWER
};

/* Actions. */
enum {
    JPQ_ACT_READY = 1,
    JPQ_ACT_START,
    JPQ_ACT_SELECT_CLUE,
    JPQ_ACT_BUZZ,
    JPQ_ACT_SUBMIT,
    JPQ_ACT_WAGER,
    JPQ_ACT_JUDGE,
    JPQ_ACT_PASS,
    JPQ_ACT_REVEAL,
    JPQ_ACT_SCORE,
    JPQ_ACT_ADVANCE,
    JPQ_ACT_PAUSE,
    JPQ_ACT_RESUME,
    JPQ_ACT_RESTART,
    JPQ_ACT_QUIT,
    JPQ_ACT_TIEBREAK,
    JPQ_ACT_RESET_HISTORY,
    JPQ_ACT_CANCEL        /* disarm a pending quit/restart confirmation */
};

enum { JPQ_JUDGE_INCORRECT = -1, JPQ_JUDGE_CORRECT = 1 };

/* Actor / viewer sentinels. Players are 0..JPQ_MAX_PLAYERS-1. The host
 * control channel is not a contestant and never appears on a public display;
 * JPQ_PUBLIC is the shared native TV viewer, distinct from the host so the host
 * snapshot can never be handed to the public renderer. */
#define JPQ_HOST 0xFF
#define JPQ_NOBODY 0xFF
#define JPQ_PUBLIC 0xFE

/* Clue flags stored in the database. */
#define JPQ_QFLAG_DD 1
#define JPQ_QFLAG_FINAL 2

typedef struct {
    uint32_t read_ms;          /* clue visible before the buzzer opens */
    uint32_t buzz_ms;          /* buzzer window per attempt */
    uint32_t answer_ms;        /* time a buzzing player has to answer */
    uint32_t dd_wager_ms;      /* daily double wager entry */
    uint32_t dd_answer_ms;     /* daily double answer entry */
    uint32_t final_wager_ms;   /* final wager entry */
    uint32_t final_answer_ms;  /* final answer entry */
    uint32_t tiebreak_answer_ms;
    uint8_t allow_negative;    /* 1 = wrong answers may drop a score below 0 */
    uint8_t lockout_rebuzz;    /* 1 = a wrong player cannot rebuzz the clue */
    uint8_t random_categories; /* 1 = random coherent categories, 0 = episode */
    uint8_t allow_tiebreak;    /* 1 = offer an optional sudden-death tiebreak */
    uint8_t dd_count_j;        /* daily doubles in single Jeopardy (1) */
    uint8_t dd_count_dj;       /* daily doubles in double Jeopardy (2) */
    uint8_t min_dd_wager;      /* authentic floor, normally 5 */
    uint8_t _pad;
} JpqConfig;

typedef struct {
    uint32_t id;               /* stable identity across reconnects */
    char name[JPQ_NAME_MAX];
    int32_t score;
    uint8_t ready, connected, active;
} JpqPlayer;

/* One coherent 6x5 board (or a single-clue Final board). Category and clue
 * indices refer to the loaded database. Values are the original broadcast
 * values, never recomputed. */
typedef struct {
    uint8_t round;
    int32_t cat[JPQ_COLS];
    int32_t clue[JPQ_COLS][JPQ_ROWS];
    int32_t value[JPQ_COLS][JPQ_ROWS];
    uint8_t used[JPQ_COLS][JPQ_ROWS];
    uint8_t dd_count;
    uint8_t dd_col[JPQ_MAX_DD];
    uint8_t dd_row[JPQ_MAX_DD];
    int32_t final_clue;
} JpqBoard;

typedef struct {
    uint32_t seq;              /* client action id, strictly increasing */
    uint32_t revision;         /* expected state revision (optimistic lock) */
    uint32_t epoch;            /* expected game epoch (replay protection) */
    uint8_t actor;             /* player index or JPQ_HOST */
    uint8_t type;
    uint8_t target;            /* player index or JPQ_NOBODY */
    int32_t value;             /* wager / score override / judge verdict */
    char text[JPQ_ANSWER_MAX]; /* submitted answer text */
} JpqAction;

/* ------------------------------------------------------------------ */
/* Question database                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name;
    uint8_t round, difficulty;
    uint32_t year, episode, airdate;
    uint32_t first_clue;
    uint16_t clue_count;
} JpqCategory;

typedef struct {
    uint32_t category;
    uint32_t value;
    uint32_t episode, airdate;
    uint8_t round, difficulty, flags, reserved;
    const char *clue;
    const char *response;
    uint16_t clue_len, resp_len;
} JpqClue;

typedef struct {
    char *strings;
    size_t strings_len;
    JpqCategory *cats;
    uint32_t cat_count;
    JpqClue *clues;
    uint32_t clue_count;
} JpqDb;

/* Clue history: one bit per clue id, kept separate from the game snapshot. */
typedef struct {
    uint32_t count;
    uint8_t *bits;
} JpqHistory;

/* Selection filter; zero fields are "no constraint". */
typedef struct {
    uint32_t year_min, year_max;
    int difficulty_min, difficulty_max;
    int category;              /* exact category index, or -1 */
    uint32_t episode;          /* exact episode, or 0 for any */
} JpqFilter;

/* ------------------------------------------------------------------ */
/* Game state                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t epoch, revision;
    uint64_t created_ms;
    uint8_t phase, paused, round, player_count, host_player;
    JpqPlayer players[JPQ_MAX_PLAYERS];
    JpqConfig cfg;
    JpqFilter filter;              /* active game-level selection filter */
    uint8_t filter_set;            /* 1 once the lobby set a filter for START */
    JpqBoard board_j, board_dj;
    int32_t final_clue;
    int32_t act_col, act_row;
    int32_t act_value;
    uint8_t act_is_dd, dd_selector, selector;
    uint8_t excluded[JPQ_MAX_PLAYERS], answered[JPQ_MAX_PLAYERS];
    uint8_t judged[JPQ_MAX_PLAYERS], correct[JPQ_MAX_PLAYERS];
    uint8_t eligible[JPQ_MAX_PLAYERS];
    int32_t wager[JPQ_MAX_PLAYERS];
    uint8_t wager_set[JPQ_MAX_PLAYERS];
    char answer[JPQ_MAX_PLAYERS][JPQ_ANSWER_MAX];
    int8_t buzzed;
    int32_t tiebreak_winner;       /* index of sudden-death winner, or -1 */
    uint8_t judge_cursor;
    uint8_t timer_kind;
    uint64_t timer_remaining_ms;   /* persisted remainder, never a deadline */
    uint64_t timer_deadline_ms;    /* runtime-only monotonic deadline */
    uint64_t buzz_remaining_ms;    /* buzzer time left when an answer interrupts */
    uint32_t rng;
    uint8_t tiebreak_active, quit_confirm, restart_confirm, quit;
    uint32_t last_seq[8];          /* per actor: players 0..5 then host */
    char err[160];
    JpqDb *db;                     /* runtime pointer, never persisted */
    JpqHistory *history;           /* runtime pointer, never persisted */
} JpqGame;

void jpq_config_default(JpqConfig *);

/* Database: load a portable little-endian JEPDB1 file. */
int jpq_db_load(JpqDb *, const char *path);
void jpq_db_free(JpqDb *);
const char *jpq_db_clue_text(const JpqDb *, uint32_t clue);
const char *jpq_db_clue_response(const JpqDb *, uint32_t clue);
int jpq_db_count(const JpqDb *, const JpqFilter *);

/* History. */
int jpq_history_init(JpqHistory *, uint32_t clue_count);
void jpq_history_free(JpqHistory *);
void jpq_history_reset(JpqHistory *);
int jpq_history_used(const JpqHistory *, uint32_t clue);
void jpq_history_mark(JpqHistory *, uint32_t clue);
int jpq_history_save(const JpqHistory *, const char *path);
int jpq_history_load(JpqHistory *, const char *path);

/* Lifecycle and board construction. */
int jpq_init(JpqGame *, const JpqConfig *);
int jpq_attach(JpqGame *, JpqDb *, JpqHistory *);
int jpq_set_filter(JpqGame *, const JpqFilter *);
int jpq_add_player(JpqGame *, const char *name, uint32_t id);
int jpq_set_ready(JpqGame *, uint8_t idx, int ready);
int jpq_build_boards(JpqGame *, uint32_t seed);
int jpq_build_round(JpqGame *, const JpqFilter *, uint8_t round, uint32_t seed, JpqBoard *);
int jpq_build_final(JpqGame *, const JpqFilter *, uint32_t seed, int32_t *clue_out);
void jpq_reset_history(JpqGame *);

/* Authoritative action application. Returns 0 on success, negative otherwise
 * with the reason in game->err. now_ms is the server monotonic clock. */
int jpq_apply(JpqGame *, const JpqAction *, uint64_t now_ms);
/* Advance timers; safe to call from a poll loop. */
int jpq_tick(JpqGame *, uint64_t now_ms);

int jpq_leader(const JpqGame *);
int jpq_tie_count(const JpqGame *);
/* Derived phone-view helpers: whether a player may buzz right now, and the
 * inclusive wager bounds for a pending DD/final wager (has_wager=0 when none). */
int jpq_can_buzz(const JpqGame *, uint8_t p);
void jpq_wager_bounds(const JpqGame *, uint8_t p, int32_t *min_out, int32_t *max_out, int *has_wager);

/* Atomic, versioned, endian-independent persistence. */
int jpq_save(const JpqGame *, const char *dir);
int jpq_load(JpqGame *, const char *dir);

/* Portable display snapshot for the native renderer. viewer is a player index,
 * JPQ_PUBLIC (shared public display) or JPQ_HOST. Hidden clues, answers,
 * wagers, unused daily-double locations and the final clue are redacted for
 * everyone but the host until their reveal phase; JPQ_PUBLIC is never the
 * host. */
int jpq_snapshot_json(const JpqGame *, uint8_t viewer, char **out, size_t *outlen);

#endif /* REBOX_JEOPARDY_H */
