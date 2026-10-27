#include <stdint.h>
#ifdef HASH_TEST
#include <unistd.h>
#include <fcntl.h>
#define HOPEN open
#define HREAD read
#define HWRITE write
#define HCLOSE close
#else
#include "hash_generated.h"
#define HOPEN ((int (*)(const char *, int))HASH_OPEN)
#define HREAD ((int (*)(int, void *, unsigned))HASH_READ)
#define HWRITE ((int (*)(int, const void *, unsigned))HASH_WRITE)
#define HCLOSE ((int (*)(int))HASH_CLOSE)
#endif
#include "hash_constants.h"
static uint32_t rr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void block(uint32_t h[8], const unsigned char *p) {
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], v = h[7];
    for (unsigned i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (unsigned i = 16; i < 64; i++)
        w[i] = w[i - 16] + (rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10));
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t =
            v + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + hash_k[i] + w[i];
        uint32_t u = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        v = g;
        g = f;
        f = e;
        e = d + t;
        d = c;
        c = b;
        b = a;
        a = t + u;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += v;
}
#ifdef HASH_TEST
int main(int argc, char **argv)
#else
int waa_hash_main(int argc, char **argv)
#endif
{
    if (argc != 2 || !argv[1] || argv[1][0] != '/')
        return 2;
    int fd = HOPEN(argv[1], 0);
    if (fd < 0)
        return 3;
    uint32_t h[8];
    for (unsigned i = 0; i < 8; i++)
        h[i] = hash_initial[i];
    unsigned char buf[4096], tail[128];
    unsigned used = 0;
    uint32_t total = 0;
    for (;;) {
        int n = HREAD(fd, buf, sizeof(buf));
        if (n < 0) {
            HCLOSE(fd);
            return 4;
        }
        if (!n)
            break;
        if ((unsigned)n > sizeof(buf) || total > 0xffffffffu - (unsigned)n) {
            HCLOSE(fd);
            return 5;
        }
        total += (unsigned)n;
        for (int i = 0; i < n; i++) {
            tail[used++] = buf[i];
            if (used == 64) {
                block(h, tail);
                used = 0;
            }
        }
    }
    HCLOSE(fd);
    tail[used++] = 0x80;
    unsigned end = used <= 56 ? 64 : 128;
    while (used < end)
        tail[used++] = 0;
    uint32_t high = total >> 29, low = total << 3;
    for (unsigned i = 0; i < 4; i++) {
        tail[end - 8 + i] = (unsigned char)(high >> (24 - i * 8));
        tail[end - 4 + i] = (unsigned char)(low >> (24 - i * 8));
    }
    block(h, tail);
    if (end == 128)
        block(h, tail + 64);
    char text[65];
    const char *hex = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) {
        unsigned b = (h[i / 4] >> (24 - 8 * (i % 4))) & 255;
        text[i * 2] = hex[b >> 4];
        text[i * 2 + 1] = hex[b & 15];
    }
    text[64] = '\n';
    unsigned done = 0;
    while (done < 65) {
        int n = HWRITE(1, text + done, 65 - done);
        if (n <= 0 || (unsigned)n > 65 - done)
            return 6;
        done += (unsigned)n;
    }
    return 0;
}
