/* Host build of the menus: MSVC's C library has no strings.h. */
#pragma once
#include <string.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
