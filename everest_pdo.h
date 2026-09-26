/* GENERATED from TwinCAT - Wireshark Test_01 - 20260923.pcapng.
 * The PDO mapping an Everest NET drive was actually given, read back out of
 * the SDO downloads in the capture. DO NOT EDIT BY HAND.
 *
 * These were hand-written once, from a hex dump my own tooling had
 * truncated, plus memory of CiA 402. Four of the eight were wrong: the
 * input map came to 9 bytes where the SyncManager said 11, and the drive
 * refused SAFEOP with AL code 0x001E. Generated now so that cannot recur;
 * t_opseq T7 checks every byte against the captured sequence. */
#ifndef EVEREST_PDO_H
#define EVEREST_PDO_H
#include <stdint.h>

/* 0x1A00: 0x6041:00 16b, 0x6064:00 32b, 0x606C:00 32b, 0x6061:00 8b
 *         total 88 bits = 11 bytes */
static const uint8_t pdo_1a00[18] = {0x04,0x00,0x10,0x00,0x41,0x60,0x20,0x00,0x64,0x60,0x20,0x00,0x6c,0x60,0x08,0x00,0x61,0x60};
/* 0x1A01: 0x6041:00 16b, 0x6064:00 32b
 *         total 48 bits = 6 bytes */
static const uint8_t pdo_1a01[10] = {0x02,0x00,0x10,0x00,0x41,0x60,0x20,0x00,0x64,0x60};
/* 0x1A02: 0x6041:00 16b, 0x606C:00 32b
 *         total 48 bits = 6 bytes */
static const uint8_t pdo_1a02[10] = {0x02,0x00,0x10,0x00,0x41,0x60,0x20,0x00,0x6c,0x60};
/* 0x1600: 0x6040:00 16b, 0x607A:00 32b, 0x60FF:00 32b, 0x6060:00 8b
 *         total 88 bits = 11 bytes */
static const uint8_t pdo_1600[18] = {0x04,0x00,0x10,0x00,0x40,0x60,0x20,0x00,0x7a,0x60,0x20,0x00,0xff,0x60,0x08,0x00,0x60,0x60};
/* 0x1601: 0x6040:00 16b, 0x607A:00 32b
 *         total 48 bits = 6 bytes */
static const uint8_t pdo_1601[10] = {0x02,0x00,0x10,0x00,0x40,0x60,0x20,0x00,0x7a,0x60};
/* 0x1602: 0x6040:00 16b, 0x60FF:00 32b
 *         total 48 bits = 6 bytes */
static const uint8_t pdo_1602[10] = {0x02,0x00,0x10,0x00,0x40,0x60,0x20,0x00,0xff,0x60};
/* 0x1C12: assigns PDO 0x1600 to the SyncManager. */
static const uint8_t pdo_1c12[4] = {0x01,0x00,0x00,0x16};
/* 0x1C13: assigns PDO 0x1A00 to the SyncManager. */
static const uint8_t pdo_1c13[4] = {0x01,0x00,0x00,0x1a};

/* In the order TwinCAT wrote them: the maps, then the assignments. */
typedef struct { uint16_t index; const uint8_t *data; uint16_t len; } PdoObj;
static const PdoObj everest_pdo[] = {
  { 0x1A00, pdo_1a00, (uint16_t)sizeof pdo_1a00 },
  { 0x1A01, pdo_1a01, (uint16_t)sizeof pdo_1a01 },
  { 0x1A02, pdo_1a02, (uint16_t)sizeof pdo_1a02 },
  { 0x1600, pdo_1600, (uint16_t)sizeof pdo_1600 },
  { 0x1601, pdo_1601, (uint16_t)sizeof pdo_1601 },
  { 0x1602, pdo_1602, (uint16_t)sizeof pdo_1602 },
  { 0x1C12, pdo_1c12, (uint16_t)sizeof pdo_1c12 },
  { 0x1C13, pdo_1c13, (uint16_t)sizeof pdo_1c13 },
};
#define EVEREST_PDO_N ((int)(sizeof(everest_pdo)/sizeof(everest_pdo[0])))

/* Process-data size the maps imply, and what TwinCAT set the
 * SyncManagers and FMMUs to: 11 bytes each way. */
#define EVEREST_PD_BYTES 11

#endif
