/* Targets without the UEFI image provider keep their existing image menu. */
#include "image_media.h"
__attribute__((weak)) int image_media_available(int s) { (void)s; return 0; }
__attribute__((weak)) int image_media_owned(int s) { (void)s; return 0; }
__attribute__((weak)) int image_media_begin(int s) { (void)s; return -1; }
__attribute__((weak)) int image_media_hold(int v) { (void)v; return 0; }
__attribute__((weak)) int image_media_end(int v) { (void)v; return 0; }
__attribute__((weak)) int image_media_live(int v) { (void)v; return 0; }
