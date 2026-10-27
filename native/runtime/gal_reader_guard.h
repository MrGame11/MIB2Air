#ifndef WAA_GAL_READER_GUARD_H
#define WAA_GAL_READER_GUARD_H
/* The supported stock GAL reader does not check malloc() inside allocateBuffer.
 * Its next operation copies a four-byte header to the returned data pointer.
 * Interpose only that call site, only for our TCP transport, when data is NULL.
 * The following read then fails normally, so stock requestStop/join/destruction
 * releases the partially allocated IoBuffer instead of crashing the process.
 * No library file or instruction is changed; the process-local memcpy GOT slot
 * is writable relocation data. Every other memcpy retains its resolved target. */
static uintptr_t gal_header_return;
static void *(*gal_copy_original)(void *, const void *, unsigned);
static unsigned gal_guard_ready;
#define TCP_BUFFER_FAILED 16u
static void *gal_copy_checked(void *dst, const void *src, unsigned n, uintptr_t caller) {
    if (!dst && n == 4 && caller == __atomic_load_n(&gal_header_return, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&g_owner, __ATOMIC_ACQUIRE) == OWNER_TCP) {
        __atomic_fetch_or(&g_tcp_io_failed, TCP_BUFFER_FAILED, __ATOMIC_RELEASE);
        return 0;
    }
    return gal_copy_original(dst, src, n);
}
__attribute__((noinline)) static void *gal_copy_hook(void *dst, const void *src, unsigned n) {
    return gal_copy_checked(dst, src, n, (uintptr_t)__builtin_return_address(0));
}
static uint32_t gal_arm_immediate(uint32_t insn) {
    unsigned shift = ((insn >> 8) & 15) * 2;
    uint32_t value = insn & 255;
    return shift ? (value >> shift) | (value << (32 - shift)) : value;
}
static int gal_guard_bind(uintptr_t reader, void *copy) {
    const uint32_t *code = (const void *)reader;
    /* Verified stock reader: allocateBuffer -> header memcpy -> readOrFail. */
    const unsigned char *bytes = (const void *)reader;
    for (unsigned i = 0; i < sizeof(gal_shape); i++)
        if (bytes[gal_shape_offsets[i]] != gal_shape[i])
            return -1;
    if ((code[GAL_COPY_CALL / 4] & 0xff000000u) != 0xeb000000u ||
        (code[GAL_READ_CALL / 4] & 0xff000000u) != 0xeb000000u)
        return -1;
    int32_t delta = (int32_t)(code[GAL_COPY_CALL / 4] << 8) >> 6;
    uintptr_t plt_address = reader + GAL_COPY_CALL + 8 + delta;
    if (plt_address < reader - GAL_PLT_MAX_DISTANCE || plt_address > reader)
        return -1;
    const uint32_t *plt = (const void *)plt_address;
    if ((plt[0] & 0xfffff000u) != 0xe28fc000u || (plt[1] & 0xfffff000u) != 0xe28cc000u ||
        (plt[2] & 0xfffff000u) != 0xe5bcf000u)
        return -1;
    uintptr_t slot =
        plt_address + 8 + gal_arm_immediate(plt[0]) + gal_arm_immediate(plt[1]) + (plt[2] & 4095);
    /* Use the reader address resolved from the captured GAL ELF.
     * Validate ELF and writable PT_LOAD before touching relocation data. */
    uintptr_t base = reader - GAL_READER;
    if (base % 4096 || slot != base + GAL_MEMCPY_GOT)
        return -1;
    const uint32_t *elf = (const void *)base;
    if (elf[0] != 0x464c457fu || elf[4] != 0x00280003u || elf[7] > 4096 || (elf[10] >> 16) != 32 ||
        (elf[11] & 65535) > 32 || slot % 4 || !copy)
        return -1;
    int writable = 0;
    for (unsigned i = 0; i < (elf[11] & 65535); i++) {
        const uint32_t *ph = (const void *)(base + elf[7] + i * 32);
        if (ph[0] == 1 && (ph[6] & 2) && slot >= base + ph[2] && slot + 4 <= base + ph[2] + ph[5])
            writable = 1;
        /* Do not write a relocation protected after loading. */
        if (ph[0] == 0x6474e552 && slot >= base + ph[2] && slot < base + ph[2] + ph[5])
            return -1;
    }
    if (!writable)
        return -1;
    gal_copy_original = copy;
    __atomic_store_n(&gal_header_return, reader + GAL_COPY_CALL + 4, __ATOMIC_RELEASE);
    __atomic_store_n((uintptr_t *)slot, (uintptr_t)gal_copy_hook, __ATOMIC_RELEASE);
    gal_guard_ready = 1;
    return 0;
}
static int gal_guard_install(void) {
    if (gal_guard_ready)
        return 0;
    if (!qr_match(qr_base + QR_DLOPEN, qr_sig_dlopen) ||
        !qr_match(qr_base + QR_DLSYM, qr_sig_dlsym) ||
        !qr_match(qr_base + QR_DLCLOSE, qr_sig_dlclose))
        return -1;
    void *h = ((void *(*)(const char *, int))(qr_base + QR_DLOPEN))(0, 2);
    if (!h)
        return -1;
    void *(*symbol)(void *, const char *) = (void *)(qr_base + QR_DLSYM);
    void *reader = symbol(h, "_ZN15GalReaderThread3runEv");
    void *copy = symbol(h, "memcpy");
    int result = reader ? gal_guard_bind((uintptr_t)reader, copy) : -1;
    ((int (*)(void *))(qr_base + QR_DLCLOSE))(h);
    return result;
}
#endif
