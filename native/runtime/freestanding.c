/* Clang's aggregate initialization helpers; no dependency on another ELF. */
void __aeabi_memclr4(void *p, unsigned n) {
    volatile unsigned char *d = p;
    while (n--)
        *d++ = 0;
}
void __aeabi_memclr(void *p, unsigned n) { __aeabi_memclr4(p, n); }
void __aeabi_memcpy(void *p, const void *q, unsigned n) {
    volatile unsigned char *d = p;
    const volatile unsigned char *s = q;
    while (n--)
        *d++ = *s++;
}
void __aeabi_memcpy4(void *p, const void *q, unsigned n) { __aeabi_memcpy(p, q, n); }
void __aeabi_memclr8(void *p, unsigned n) { __aeabi_memclr4(p, n); }
void __aeabi_memcpy8(void *p, const void *q, unsigned n) { __aeabi_memcpy(p, q, n); }
