#ifndef ECAT_CRC_H
#define ECAT_CRC_H
/* ── CRC32C (Castagnoli) + Ethernet CRC32 (FCS) ─────────────────────────────
 * Hardware SSE4.2 CRC32C when available (Ryzen 5300U has it), portable
 * table-driven fallback otherwise; both ends use the same function so the
 * choice is transparent to correctness. eth_crc32 is the standard reflected
 * Ethernet FCS polynomial, used to verify the delivered FCS trailer.
 * Tables are SHARED (defined once in crc.c) — per-TU static copies would be
 * zero in any TU that never ran the init, silently degenerating the CRC to a
 * constant. crc32c_init() MUST be called once at startup (and by every test
 * harness) before any CRC use. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>   /* memcpy */
#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>   /* _mm_crc32_u8/u64 */
#endif

extern int      g_have_sse42;
extern uint32_t g_crc32c_tab[256];
extern uint32_t g_crc32_tab[256];

void crc32c_init(void);   /* detect SSE4.2 + build both tables */

/* eth_crc32 computed over a good frame INCLUDING its FCS trailer yields a
 * constant residual (hardware-confirmed 0x2144DF1C on this rig; the
 * bit-ordering variant 0xDEBB20E3 accepted as alternate). */
#define ETH_FCS_RESIDUAL  0x2144DF1Cu
#define ETH_FCS_RESIDUAL2 0xDEBB20E3u

/* Ethernet FCS over p[0..n). Standard init/xor (0xFFFFFFFF / final XOR). */
static inline uint32_t eth_crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        crc = g_crc32_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static inline uint32_t crc32c_sw(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = g_crc32c_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static inline uint32_t crc32c(const uint8_t *p, size_t n) {
#if defined(__x86_64__) || defined(__i386__)
    if (g_have_sse42) {
        uint32_t crc = ~0u;
        size_t i = 0;
    #if defined(__x86_64__)
        for (; i + 8 <= n; i += 8) {
            uint64_t v; memcpy(&v, p + i, 8);
            crc = (uint32_t)_mm_crc32_u64(crc, v);
        }
    #endif
        for (; i < n; i++) crc = _mm_crc32_u8(crc, p[i]);
        return ~crc;
    }
#endif
    return crc32c_sw(0, p, n);
}

/* Incremental CRC32C over non-contiguous regions (seq ++ random payload,
 * skipping the CRC hole) as one logical byte stream. */
static inline uint32_t crc32c_begin(void) { return ~0u; }
static inline uint32_t crc32c_update(uint32_t crc, const uint8_t *p, size_t n) {
#if defined(__x86_64__) || defined(__i386__)
    if (g_have_sse42) {
        size_t i = 0;
    #if defined(__x86_64__)
        for (; i + 8 <= n; i += 8) {
            uint64_t v; memcpy(&v, p + i, 8);
            crc = (uint32_t)_mm_crc32_u64(crc, v);
        }
    #endif
        for (; i < n; i++) crc = _mm_crc32_u8(crc, p[i]);
        return crc;
    }
#endif
    for (size_t i = 0; i < n; i++)
        crc = g_crc32c_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static inline uint32_t crc32c_final(uint32_t crc) { return ~crc; }

static inline uint32_t crc32c_chain(const uint8_t *a, size_t na,
                                    const uint8_t *b, size_t nb) {
    uint32_t c = crc32c_begin();
    c = crc32c_update(c, a, na);
    c = crc32c_update(c, b, nb);
    return crc32c_final(c);
}

#endif /* ECAT_CRC_H */
