/* Same startup method as modules/iptv/fetch.c. The receiver's
 * __uClibc_main initializes libc and performs normal exit/finalization. */
extern int main(int, char **);
extern void __uClibc_main(int (*)(int,char **),int,char **,void *,void *,void *,void *);
void hr54_entry(long *stack) {
    __uClibc_main(main,(int)stack[0],(char **)(stack+1),0,0,0,stack);
}
__asm__(".text\n.globl __start\n.ent __start\n__start:\n"
        "lui $28,%hi(_gp)\naddiu $28,$28,%lo(_gp)\nmove $4,$29\n"
        "addiu $29,$29,-32\nla $25,hr54_entry\njalr $25\nnop\nb .\nnop\n.end __start\n");
