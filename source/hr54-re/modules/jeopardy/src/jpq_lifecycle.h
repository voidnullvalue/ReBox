#ifndef JPQ_LIFECYCLE_H
#define JPQ_LIFECYCLE_H
#include <stddef.h>
/* Native surface lifecycle: retained lock, reaper, readiness and pid identity.
 *
 * Adapted from the games/doom supervisors but self-contained and free of any
 * game structs, so it is unit-testable with a mock child. The lock stays held
 * through initialization and reaping: a vanished PID alone does not prove its
 * retained graphics surface was released. */
int jpq_lifecycle_pid(const char *engine, const char *pid_path);
int jpq_lifecycle_busy(const char *lock_path);
/* Stop a running surface. Safe when nothing is running. *was_running set. */
int jpq_lifecycle_stop(const char *engine, const char *pid_path, const char *lock_path, int *was_running);
/* Launch the engine under a reaper. Waits for the readiness byte up to
 * timeout_ms. Returns 0 and *out_pid>1 on success, else -1 with err. */
int jpq_lifecycle_start(const char *engine, const char *pid_path, const char *lock_path,
                        const char *depth_arg, int timeout_ms, int *out_pid, char *err, size_t errcap);
#endif
