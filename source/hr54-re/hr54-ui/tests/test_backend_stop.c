/* Exercise the production backend adapter; no receiver services are used. */
#define main backend_main
#include "../../jellyfin/remote/hr54_jf.c"
#undef main
#include <assert.h>
int main(void) {
    native_frontend = 1;
    setenv("HR54_STOP_MOCK_REPLY", "pvruconnect.DirectTest success", 1);
    assert(receiver_stop() == 0); /* Vendor success with a nonzero exit code. */
    setenv("HR54_STOP_MOCK_REPLY", "pvruconnect.DirectTest failure", 1);
    assert(receiver_stop() != 0);
    setenv("HR54_STOP_MOCK_REPLY", "pvruconnect.DirectTest success Exception", 1);
    assert(receiver_stop() != 0);
    puts("PASS backend native decoder STOP: fixed watch/session/speed command, vendor reply validation, failure mapping");
    return 0;
}
