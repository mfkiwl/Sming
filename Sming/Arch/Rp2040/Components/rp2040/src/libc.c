
#include "include/esp_libc.h"
#include <hardware/structs/rosc.h>

/* Misc */

uint32_t os_random(void)
{
	uint32_t res = 0;
	for(unsigned i = 0; i < 32; ++i) {
		res <<= 1;
		res |= rosc_hw->randombit;
	}
	return res;
}

int os_get_random(uint8_t* buf, size_t len)
{
	while(len-- != 0) {
		uint8_t res = 0;
		for(unsigned i = 0; i < 8; ++i) {
			res <<= 1;
			res |= rosc_hw->randombit;
		}
		*buf++ = res;
	}
	return 0;
}

void ets_install_putc1(void (*p)(char c))
{
	// Not implemented
}

void system_set_os_print(bool onoff)
{
	// Not implemented
}
