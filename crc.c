/* CRC table definitions + one-time init. See crc.h for the hot-path inlines. */
#include "crc.h"

int      g_have_sse42 = 0;
uint32_t g_crc32c_tab[256];
uint32_t g_crc32_tab[256];

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
static void crc32c_detect(void) {
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        g_have_sse42 = (ecx & bit_SSE4_2) ? 1 : 0;
}
#else
static void crc32c_detect(void) { g_have_sse42 = 0; }
#endif

void crc32c_init(void) {
    crc32c_detect();
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        g_crc32c_tab[i] = c;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc32_tab[i] = c;
    }
}
