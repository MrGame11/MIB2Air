#ifndef WAA_SPP_H
#define WAA_SPP_H
#include <stdint.h>
#define SPP_F_ERROR 0x2u
#define SPP_F_FLOW 0x4u
#define SPP_F_TXPEND 0x8u
int waa_spp_send(void *, uint32_t, const uint8_t *, uint32_t);
uint32_t waa_spp_read(uint32_t, uint8_t *, uint32_t);
uint32_t waa_spp_status(uint32_t);
int waa_spp_ready(uint32_t);
void waa_spp_tick(void *, uint32_t);
#endif
