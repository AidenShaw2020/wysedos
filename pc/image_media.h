/* Exclusive ownership of external USB media used for disk-image files. */
#ifndef IMAGE_MEDIA_H
#define IMAGE_MEDIA_H
#include <stddef.h>
#define IMAGE_MEDIA_FIRST 4
#define IMAGE_MEDIA_COUNT 4
static inline int image_media_path_source(const char *p) {
    return p && p[0]>='4' && p[0]<='7' && p[1]==':'?p[0]-'4':-1;
}
/* Disk Manager asks for confirmation before begin; end releases its hold.
 * Open FILs hold independent references until their last successful close. */
int image_media_available(int source);
int image_media_owned(int source);
int image_media_begin(int source);
int image_media_hold(int volume);
int image_media_end(int volume);
int image_media_live(int volume);
#endif
