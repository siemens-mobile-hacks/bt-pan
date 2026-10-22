#ifndef BT_PANU_FW_H
#define BT_PANU_FW_H

#include <stdint.h>

#define FW_THUMB(address) ((uintptr_t)(address) | 1u)
#define FW_ARM(address) ((uintptr_t)(address) & ~1u)

/* Firmware model, set once by fw_detect() before anything below is called.
 * Addresses differ between EL71 sw45 and E71 sw45 (map: e71/map.tsv, found by e71/find-e71.py). */
enum { FW_UNKNOWN, FW_EL71, FW_E71 };
extern int fw_model;

#define FW_SEL(el71, e71) ((uintptr_t)(fw_model == FW_E71 ? (e71) : (el71)))
#define FW_T(el71, e71) (FW_SEL(el71, e71) | 1u)
#define FW_A(el71, e71) (FW_SEL(el71, e71) & ~1u)

int fw_detect(void);
const char *fw_model_name(void);

#endif
