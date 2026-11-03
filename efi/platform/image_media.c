/* External media are either guest USB disks or monitor-owned image files. */
#include "image_media.h"
#include "hostblk.h"
#include "fatvol.h"
#include "ff.h"
#include <stdio.h>
static struct { BlockDev disk; unsigned refs; int port, mounted, present;
    void *identity; uint32_t media_id; char which[8]; } sources[IMAGE_MEDIA_COUNT];
static void discover(void) {
    for (int s=0;s<IMAGE_MEDIA_COUNT;s++) sources[s].present=0;
    for (int n=1;n<=IMAGE_MEDIA_COUNT;n++) {
        char which[8]; void *identity; uint32_t media_id;
        snprintf(which,sizeof(which),"uhd%d",n);
        if (!hostblk_image_info(which,&identity,&media_id)) continue;
        int match=-1,free_slot=-1;
        for (int s=0;s<IMAGE_MEDIA_COUNT;s++) {
            if (sources[s].identity==identity && sources[s].media_id==media_id) match=s;
            if (!sources[s].mounted && !sources[s].present && free_slot<0) free_slot=s;
        }
        if (match<0) match=free_slot;
        if (match<0) continue;
        sources[match].identity=identity; sources[match].media_id=media_id;
        sources[match].present=1;
        snprintf(sources[match].which,sizeof(sources[match].which),"%s",which);
    }
}
int image_media_available(int s) {
    if (s<0 || s>=IMAGE_MEDIA_COUNT) return 0;
    discover();
    return sources[s].mounted || sources[s].present;
}
int image_media_owned(int s) { return s>=0 && s<IMAGE_MEDIA_COUNT && sources[s].mounted; }
int image_media_live(int vol) {
    int s=vol-IMAGE_MEDIA_FIRST;
    return s>=0 && s<IMAGE_MEDIA_COUNT && sources[s].mounted && blk_size(&sources[s].disk);
}
int image_media_hold(int vol) {
    int s=vol-IMAGE_MEDIA_FIRST;
    if (!image_media_live(vol) || !fatvol_attached(vol)) return 0;
    sources[s].refs++; return 1;
}
int image_media_begin(int s) {
    if (s<0 || s>=IMAGE_MEDIA_COUNT) return -1;
    int vol=IMAGE_MEDIA_FIRST+s;
    if (sources[s].mounted) {
        if (fatvol_attached(vol)) return image_media_hold(vol)?vol:-1;
        /* A failed mount may have retained ownership after a flush error.
         * Retry the release explicitly before attempting another mount. */
        if (!image_media_end(vol)) return -1;
    }
    /* Use the identity the user selected; an ordinal may have moved since. */
    if (!hostblk_image_take(sources[s].which,sources[s].identity,sources[s].media_id,
                           &sources[s].disk,&sources[s].port)) return -1;
    if (fatvol_attach_block(vol,&sources[s].disk)!=FR_OK) {
        fatvol_detach(vol);
        if (!hostblk_image_return(&sources[s].disk,sources[s].port))
            sources[s].mounted=1; /* Keep ownership until a later explicit retry. */
        return -1;
    }
    sources[s].mounted=1; sources[s].refs=1;
    return vol;
}
int image_media_end(int vol) {
    int s=vol-IMAGE_MEDIA_FIRST;
    if (s<0 || s>=IMAGE_MEDIA_COUNT || !sources[s].mounted) return 1;
    if (sources[s].refs) sources[s].refs--;
    if (sources[s].refs) return 1;
    if (blk_size(&sources[s].disk) && blk_sync(&sources[s].disk)<0) return 0;
    if (!hostblk_image_return(&sources[s].disk,sources[s].port)) return 0;
    fatvol_detach(vol);
    sources[s].mounted=0;
    return 1;
}
