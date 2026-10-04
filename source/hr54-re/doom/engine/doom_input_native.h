#ifndef DOOM_INPUT_NATIVE_H
#define DOOM_INPUT_NATIVE_H
int doom_input_open(void);
int doom_input_pump(void (*)(unsigned char,int)); /* <0 disconnect/error, 1 EXIT */
void doom_input_close(void);
#endif
