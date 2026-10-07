#include "../../modules/shared/sdk.h"
#include <assert.h>
int main(int argc,char **argv){assert(argc==3);return rb_module_copy_defaults(argv[1],argv[2])?1:0;}
