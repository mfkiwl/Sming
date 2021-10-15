/****
 * Sming Framework Project - Open Source framework for high efficiency native ESP8266 development.
 * Created 2015 by Skurydin Alexey
 * http://github.com/SmingHub/Sming
 * All files of the Sming Core are provided under the LGPL v3 license.
 *
 * esp_cplusplus.cpp
 *
 ****/

#include <esp_systemapi.h>
#include <cstdlib>

namespace std
{
const nothrow_t nothrow;
}

extern "C" void __cxa_pure_virtual(void)
{
	SYSTEM_ERROR("Bad pure_virtual_call");
	abort();
}

extern "C" void __cxa_deleted_virtual(void)
{
	SYSTEM_ERROR("Bad deleted_virtual_call");
	abort();
}
