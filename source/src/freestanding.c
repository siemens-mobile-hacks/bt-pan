/*
 * Our SDK's crt provides memcpy but not memset. In a freestanding (-D__NO_LIBC)
 * build GCC still lowers struct/array zeroing to a memset call, so supply one.
 */
#include <string.h>

void *memset(void *dst, int c, size_t n) {
	unsigned char *p = (unsigned char *)dst;
	while (n--)
		*p++ = (unsigned char)c;
	return dst;
}
