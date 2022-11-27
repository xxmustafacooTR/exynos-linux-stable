__attribute__ ((section(".rodata"), unused))
const unsigned char last_crypto_asm_rodata = 0x20;

__attribute__ ((section(".text"), unused))
void last_crypto_asm_text(void){}

#ifdef CC_USE_CLANG
__attribute__ ((section(".init.text"), unused))
#else
__attribute__ ((section(".init.text"), optimize("-O0"), unused))
#endif
static void last_crypto_asm_init(void){};
