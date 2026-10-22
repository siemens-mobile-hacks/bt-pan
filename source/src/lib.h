#ifndef BT_PANU_LIB_H
#define BT_PANU_LIB_H

#include <stdint.h>

/* Firmware functions and data come from the ElfLoader function library (patches/library-*.vkp),
 * not from fixed addresses: the same elf runs on every firmware whose library has these entries.
 * Numbers as in the SDK swilib (gprs.h, bluetooth.h, gbs.h); 0x8000 marks a data entry. */
extern const int __sys_switab_addres[];
#define LIB_ENTRY(number) ((uintptr_t)__sys_switab_addres[(number) & 0x7fffu])

/* 0 when every library entry this elf uses is filled in. Otherwise the first missing number. */
unsigned lib_check(void);

#endif
