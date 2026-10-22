#include "lib.h"

/* Every library entry the elf calls or reads. Standard ones (0x38E CreateICL, 0x154 KillGBSproc)
 * are missing from some libraries too, so they are checked as well. */
static const uint16_t used[] = {
	0x154, 0x38E,
	0x420, 0x421, 0x422, 0x423, 0x42C, 0x42D, 0x433, 0x43E, 0x442, 0x44B, 0x44C,
	0x45F, 0x460, 0x461, 0x462, 0x463, 0x464, 0x465, 0x466, 0x467, 0x468, 0x469,
	0x46A, 0x46B, 0x46C, 0x46D, 0x46E, 0x46F, 0x470, 0x8471,
};

unsigned lib_check(void) {
	unsigned i;

	for (i = 0; i < sizeof(used) / sizeof(used[0]); ++i) {
		uintptr_t address = LIB_ENTRY(used[i]);
		int ram = used[i] & 0x8000u;

		/* an empty slot reads FFFFFFFF; functions are in flash, data in RAM */
		if (ram ? address < 0xA8000000u || address >= 0xAC000000u
				: address < 0xA0000000u || address >= 0xA2000000u)
			return used[i];
	}
	return 0;
}
