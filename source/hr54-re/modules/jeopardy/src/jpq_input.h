#ifndef JPQ_INPUT_H
#define JPQ_INPUT_H
#include "jpq_view.h"
/* Native input broker, same protocol as native Doom/Games. MENU/GUIDE/EXIT
 * keep their stock owners; STOP is always delivered to the remote model. */
int jpq_input_open(void);
int jpq_input_pump(void (*sink)(JvKey));
void jpq_input_close(void);
JvKey jpq_key_translate(unsigned raw_code);
#endif
