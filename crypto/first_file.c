__attribute__ ((section(".rodata"), unused))
const unsigned char first_crypto_rodata = 0x10;

__attribute__ ((section(".text"), unused))
void first_crypto_text(void){}

#ifdef CC_USE_CLANG
__attribute__ ((section(".init.text"), unused))
#else
__attribute__ ((section(".init.text"), optimize("-O0"), unused))
#endif
static void first_crypto_init(void){};
