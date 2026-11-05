#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "fatvol.h"
#include "installer_upgrade.h"

/* Deliberately not recursive: no user files, settings, layout marker or guest
 * driver directory can become a destination. EFI is activated last. */
static const char *const files[]={
    "EFI/BOOT/BOOTX64.EFI", "386/bios.bin", "386/vgabios.bin", "386/gm.sf2",
    "386/gm.sf2.LICENSE.txt", "386/logo.bin", "386/SeaBIOS-COPYING.txt",
    "386/SeaBIOS-COPYING.LESSER.txt", "386/SeaBIOS-NOTICE.txt"
};
#define FILES (sizeof(files)/sizeof(files[0]))
#define JOURNAL "2:386/upgrade.wup"
#define COMMITTED "2:386/upgrade.done"
typedef struct { uint32_t magic, old_mask, committed, check; } Journal;
static uint8_t buffer[65536], verify[65536];

static uint32_t checksum(const Journal *j) {
    return 0x9e3779b9u ^ j->magic ^ (j->old_mask*0x85ebca6bu) ^ (j->committed*0xc2b2ae35u);
}
static void paths(unsigned i,char *dest,char *stage,char *backup) {
    snprintf(dest,96,"2:%s",files[i]);
    snprintf(stage,96,"2:%s.wup-new",files[i]);
    snprintf(backup,96,"2:%s.wup-old",files[i]);
}
/* -1 is an I/O error, not an absent file. */
static int present(const char *path) {
    FILINFO info; FRESULT r=f_stat(path,&info);
    if (r==FR_NO_FILE || r==FR_NO_PATH) return 0;
    return r==FR_OK && !(info.fattrib&AM_DIR)?1:-1;
}
static bool remove_if_present(const char *path) {
    int p=present(path);
    return p==0 || (p==1 && f_unlink(path)==FR_OK);
}
static bool load_journal(const char *path,Journal *j) {
    FIL f; UINT got=0;
    if (f_open(&f,path,FA_READ)!=FR_OK) return false;
    bool ok=f_size(&f)==sizeof(*j) && f_read(&f,j,sizeof(*j),&got)==FR_OK && got==sizeof(*j);
    if (f_close(&f)!=FR_OK) ok=false;
    return ok && j->magic==0x31505557u && j->committed<=1 &&
           !(j->old_mask&~((1u<<FILES)-1)) && j->check==checksum(j);
}
static bool save_journal(Journal *j,const char *path) {
    j->check=checksum(j);
    FIL f; UINT put=0;
    if (f_open(&f,path,FA_WRITE|FA_CREATE_NEW)!=FR_OK) return false;
    bool ok=f_write(&f,j,sizeof(*j),&put)==FR_OK && put==sizeof(*j);
    if (f_sync(&f)!=FR_OK) ok=false;
    if (f_close(&f)!=FR_OK) ok=false;
    return ok;
}
bool installer_upgrade_pending(void) { return present(JOURNAL)==1 || present(COMMITTED)==1; }

static bool recover(const Journal *j,UpgradeFlush flush,void *context) {
    char dest[96],stage[96],backup[96];
    for (unsigned i=0;i<FILES;i++) {
        paths(i,dest,stage,backup);
        int b=present(backup),d=present(dest);
        if (b<0 || d<0) return false;
        if (j->committed) {
            if (d!=1 || !remove_if_present(backup)) return false;
        } else if (b) {
            if (!remove_if_present(dest) || f_rename(backup,dest)!=FR_OK) return false;
        } else if (!(j->old_mask&(1u<<i))) {
            if (!remove_if_present(dest)) return false;
        } else if (!d) return false; /* Never delete the only surviving copy. */
        if (!remove_if_present(stage)) return false;
    }
    if (!flush(context)) return false;
    /* Retain the committed record until all other cleanup is durable. The
     * initial record is never overwritten, so a torn commit record can still
     * use the valid initial record and intact backups to roll back. */
    if (!j->committed && !remove_if_present(COMMITTED)) return false;
    if (!remove_if_present(JOURNAL) || !flush(context)) return false;
    return (!j->committed || remove_if_present(COMMITTED)) && flush(context);
}

static bool stage_file(unsigned i) {
    char dest[96],stage[96],backup[96]; paths(i,dest,stage,backup);
    FIL in,out; UINT got=0,put=0;
    if (f_open(&in,files[i],FA_READ)!=FR_OK) return false;
    uint64_t size=f_size(&in);
    if (f_open(&out,stage,FA_WRITE|FA_CREATE_NEW)!=FR_OK) { f_close(&in); return false; }
    bool ok=true;
    for (uint64_t at=0;at<size && ok;) {
        UINT n=size-at<sizeof(buffer)?(UINT)(size-at):sizeof(buffer);
        ok=f_read(&in,buffer,n,&got)==FR_OK && got==n &&
           f_write(&out,buffer,n,&put)==FR_OK && put==n;
        at+=n;
    }
    if (f_close(&in)!=FR_OK) ok=false;
    if (f_sync(&out)!=FR_OK) ok=false;
    if (f_close(&out)!=FR_OK) ok=false;
    if (!ok) return false;
    /* Independent readback of the new file, compared byte-for-byte. */
    if (f_open(&in,files[i],FA_READ)!=FR_OK) return false;
    if (f_open(&out,stage,FA_READ)!=FR_OK) { f_close(&in); return false; }
    ok=f_size(&in)==size && f_size(&out)==size;
    for (uint64_t at=0;at<size && ok;) {
        UINT n=size-at<sizeof(buffer)?(UINT)(size-at):sizeof(buffer);
        ok=f_read(&in,buffer,n,&got)==FR_OK && got==n &&
           f_read(&out,verify,n,&put)==FR_OK && put==n && !memcmp(buffer,verify,n);
        at+=n;
    }
    if (f_close(&in)!=FR_OK) ok=false;
    if (f_close(&out)!=FR_OK) ok=false;
    return ok;
}

bool installer_upgrade_files(UpgradeFlush flush,void *context,UpgradeProgress progress,const char **error) {
    Journal j={0x31505557u,0,0,0};
    char dest[96],stage[96],backup[96];
    *error="Cannot inspect firmware files";
    int pending=present(JOURNAL),done=present(COMMITTED);
    if (pending<0 || done<0) return false;
    if (pending || done) {
        *error="Interrupted update journal invalid - firmware left unchanged";
        bool valid=done && load_journal(COMMITTED,&j) && j.committed;
        if (!valid) valid=pending && load_journal(JOURNAL,&j) && !j.committed;
        if (!valid) {
            /* Power loss while first creating the initial record: no stage or
             * backup can exist yet. Remove only that incomplete reserved file. */
            FILINFO info;
            bool empty_start=pending && !done && f_stat(JOURNAL,&info)==FR_OK && info.fsize<sizeof(j);
            for (unsigned i=0;i<FILES && empty_start;i++) {
                paths(i,dest,stage,backup);
                empty_start=present(stage)==0 && present(backup)==0;
            }
            if (!empty_start || !remove_if_present(JOURNAL) || !flush(context)) return false;
        } else {
        *error="Cannot recover interrupted update - keep USB available";
        if (!recover(&j,flush,context)) return false;
        }
        j=(Journal){0x31505557u,0,0,0};
    }
    uint64_t bytes=0;
    for (unsigned i=0;i<FILES;i++) {
        FILINFO info;
        *error="Update source files missing or empty";
        if (f_stat(files[i],&info)!=FR_OK || (info.fattrib&AM_DIR) || !info.fsize) return false;
        bytes+=info.fsize;
        paths(i,dest,stage,backup);
        *error="Update temporary-name conflict or firmware read error";
        int d=present(dest);
        if (d<0 || present(stage)!=0 || present(backup)!=0) return false;
        if (d) j.old_mask|=1u<<i;
    }
    *error="Not enough free space to stage the complete firmware update";
    if (bytes+131072>fatvol_free_bytes(FATVOL_TARGET_ESP)) return false;
    *error="Cannot create update journal";
    if (!save_journal(&j,JOURNAL) || !flush(context)) {
        /* Nothing except the initial record has changed yet. */
        remove_if_present(JOURNAL); flush(context);
        return false;
    }
    *error="Copy or verification failed; update can be retried from USB";
    for (unsigned i=0;i<FILES;i++) {
        if (progress) progress(files[i],5+(unsigned)i*65/FILES);
        if (!stage_file(i)) goto rollback;
    }
    if (!flush(context)) goto rollback;
    *error="Firmware activation failed; update can be retried from USB";
    for (unsigned n=0;n<FILES;n++) {
        unsigned i=n+1<FILES?n+1:0; /* Executable last, after its runtime assets. */
        paths(i,dest,stage,backup);
        if (progress) progress(files[i],75+n*20/FILES);
        if ((j.old_mask&(1u<<i)) && f_rename(dest,backup)!=FR_OK) goto rollback;
        if (f_rename(stage,dest)!=FR_OK || !flush(context)) goto rollback;
    }
    j.committed=1;
    *error="Update written; retry from USB to complete journal cleanup";
    if (!save_journal(&j,COMMITTED) || !flush(context)) {
        j.committed=0;
        recover(&j,flush,context);
        return false;
    }
    if (!recover(&j,flush,context)) return false;
    if (progress) progress("Firmware update complete",100);
    *error=NULL;
    return true;
rollback:
    /* Recovery is idempotent; on persistent I/O failure retain the journal. */
    recover(&j,flush,context);
    return false;
}
