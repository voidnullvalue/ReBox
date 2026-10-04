#include "keys.h"
/* Raw codes from the proven stock key table (NATIVE_CONTROL_FINDINGS /
 * input-keys.md). Replay 0xe409 and advance 0xe408 are the remote's skip
 * buttons and are deliberately distinct from rewind 0xe406 and fast-forward
 * 0xe405. EXIT 0xe502 is a stock special key: it is mapped for correctness
 * of propagation, never requested for ownership. */
UiKey input_key(uint32_t raw){switch(raw&65535){case 0xe100:return KEY_UP;case 0xe101:return KEY_DOWN;case 0xe102:return KEY_LEFT;case 0xe103:return KEY_RIGHT;case 0xe001:case 0xe505:return KEY_SELECT;case 0xe002:return KEY_BACK;case 0xe00b:return KEY_GUIDE;case 0xe503:return KEY_MENU;case 0xe00e:return KEY_INFO;case 0xe400:return KEY_PLAY;case 0xe401:return KEY_PAUSE;case 0xe402:return KEY_STOP;case 0xe403:return KEY_RECORD;case 0xe405:return KEY_FORWARD;case 0xe406:return KEY_REWIND;case 0xe408:return KEY_SKIP_FORWARD;case 0xe409:return KEY_SKIP_BACK;case 0xe501:return KEY_LIST;case 0xe502:return KEY_EXIT;case 0xe200:return KEY_RED;case 0xe201:return KEY_GREEN;case 0xe202:return KEY_YELLOW;case 0xe203:return KEY_BLUE;default:return KEY_NONE;}}
