#include <stddef.h>
#include <stdint.h>
/* Clang compiler-rt supplies operations, but Zig 0.13 lacks GCC's query ABI.
 * MIPS32 is lock-free only for naturally aligned <=32-bit objects. */
_Bool hr54_atomic_query(size_t n,const volatile void *p) __asm__("__atomic_is_lock_free");
_Bool hr54_atomic_query(size_t n,const volatile void *p){return (n==1||n==2||n==4)&&(!p||((uintptr_t)p&(n-1))==0);}
