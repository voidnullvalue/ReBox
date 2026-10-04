#include "animation.h"
#include "theme.h"
int ui_transition_progress(uint64_t start,uint64_t now) {
    if(!start||now-start>=UI_TRANSITION_MS)return 1000;
    int t=(int)(now-start)*1000/UI_TRANSITION_MS;
    return t*t*(3000-2*t)/1000000;
}
