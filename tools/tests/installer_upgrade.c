/* Real upgrade transaction; in-memory file device with injected I/O errors
 * and process interruptions. No physical disk or guest is accessed.
 * clang -O2 tools/tests/installer_upgrade.c -o work/installer_upgrade.exe */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "../../efi/platform/installer_upgrade.c"
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s (failure %u, cut %u)\n",__LINE__,#x,fail_at,cut_at); exit(1); } } while (0)
typedef struct { char path[100]; unsigned char *data; size_t size; int exists; } MockFile;
static MockFile disk[64];
static unsigned calls,fail_at,mutations,cut_at,activated,last_activation;
static int corrupt_stage;
static uint64_t free_bytes=1ull<<30;
static jmp_buf reboot;
static int failure(void) { return ++calls==fail_at; }
static void mutate(void) { if (++mutations==cut_at) longjmp(reboot,1); }
static int lookup(const char *path) {
    for (int i=0;i<64;i++) if (disk[i].path[0] && !strcmp(path,disk[i].path)) return i;
    return -1;
}
static int slot(const char *path) {
    int i=lookup(path); if (i>=0) return i;
    for (i=0;i<64;i++) if (!disk[i].path[0]) { snprintf(disk[i].path,100,"%s",path); return i; }
    abort();
}
static void permitted(const char *path) {
    CHECK(!strcmp(path,JOURNAL) || !strcmp(path,COMMITTED) || !strncmp(path,"2:",2));
    CHECK(strcmp(path,"2:386/config.ini") && strcmp(path,"2:386/native.ini"));
    if (!strcmp(path,JOURNAL) || !strcmp(path,COMMITTED)) return;
    for (unsigned i=0;i<FILES;i++) {
        char d[96],s[96],b[96]; paths(i,d,s,b);
        if (!strcmp(path,d) || !strcmp(path,s) || !strcmp(path,b)) return;
    }
    CHECK(0);
}
FRESULT f_stat(const char *path,FILINFO *info) {
    if (failure()) return FR_DISK_ERR;
    int i=lookup(path); if (i<0 || !disk[i].exists) return FR_NO_FILE;
    if (info) { memset(info,0,sizeof(*info)); info->fsize=disk[i].size; }
    return FR_OK;
}
FRESULT f_open(FIL *f,const char *path,BYTE mode) {
    if (mode&FA_WRITE) permitted(path);
    if (failure()) return FR_DISK_ERR;
    int i=lookup(path);
    if (mode&FA_CREATE_NEW) {
        if (i>=0 && disk[i].exists) return FR_EXIST;
        i=slot(path); disk[i].exists=1; disk[i].size=0; mutate();
    } else if (i<0 || !disk[i].exists) return FR_NO_FILE;
    memset(f,0,sizeof(*f)); f->obj.handle=(uint32_t)i; f->obj.objsize=disk[i].size; f->flag=mode;
    if (corrupt_stage && !(mode&FA_WRITE) && strstr(path,".wup-new") && disk[i].size) {
        disk[i].data[0]^=1; corrupt_stage=0;
    }
    return FR_OK;
}
FRESULT f_close(FIL *f) { (void)f; return failure()?FR_DISK_ERR:FR_OK; }
FRESULT f_read(FIL *f,void *p,UINT n,UINT *got) {
    *got=0; if (failure()) return FR_DISK_ERR;
    MockFile *m=&disk[f->obj.handle];
    if (n>m->size-f->fptr) n=(UINT)(m->size-f->fptr);
    if (n) memcpy(p,m->data+f->fptr,n);
    *got=n; f->fptr+=n; return FR_OK;
}
FRESULT f_write(FIL *f,const void *p,UINT n,UINT *put) {
    MockFile *m=&disk[f->obj.handle]; permitted(m->path);
    *put=0; if (failure()) return FR_DISK_ERR;
    size_t end=(size_t)f->fptr+n;
    if (end>m->size) { m->data=realloc(m->data,end); CHECK(m->data); m->size=end; }
    memcpy(m->data+f->fptr,p,n); f->fptr+=n; f->obj.objsize=m->size; *put=n;
    mutate(); return FR_OK;
}
FRESULT f_sync(FIL *f) { (void)f; if (failure()) return FR_DISK_ERR; mutate(); return FR_OK; }
FRESULT f_unlink(const char *path) {
    permitted(path); if (failure()) return FR_DISK_ERR;
    int i=lookup(path); if (i<0 || !disk[i].exists) return FR_NO_FILE;
    disk[i].exists=0; mutate(); return FR_OK;
}
FRESULT f_rename(const char *a,const char *b) {
    permitted(a); permitted(b); if (failure()) return FR_DISK_ERR;
    int i=lookup(a),j=lookup(b);
    if (i<0 || !disk[i].exists) return FR_NO_FILE;
    if (j>=0 && disk[j].exists) return FR_EXIST;
    j=slot(b); free(disk[j].data);
    disk[j].data=disk[i].data; disk[j].size=disk[i].size; disk[j].exists=1;
    disk[i].data=NULL; disk[i].size=0; disk[i].exists=0;
    if (strstr(a,".wup-new")) {
        for (unsigned k=0;k<FILES;k++) if (!strcmp(b+2,files[k])) last_activation=k;
        activated++;
    }
    mutate(); return FR_OK;
}
uint64_t fatvol_free_bytes(int volume) { CHECK(volume==2); return free_bytes; }
static bool flush(void *context) { CHECK(context==disk); if (failure()) return false; mutate(); return true; }
static void put_fixture(const char *path,size_t size,unsigned byte) {
    int i=slot(path); disk[i].exists=1; disk[i].size=size; disk[i].data=malloc(size?size:1);
    CHECK(disk[i].data); memset(disk[i].data,byte,size);
}
static void fixture(void) {
    for (int i=0;i<64;i++) free(disk[i].data);
    memset(disk,0,sizeof(disk)); calls=mutations=activated=last_activation=0;
    fail_at=cut_at=0; free_bytes=1ull<<30; corrupt_stage=0;
    for (unsigned i=0;i<FILES;i++) {
        char path[96]; snprintf(path,96,"2:%s",files[i]);
        put_fixture(files[i],i==3?131101:512+i,0x60+i);
        if (i!=4) put_fixture(path,123+i,0x20+i); /* Also qualify adding a previously absent file. */
    }
    put_fixture("2:386/config.ini",100,0x91);
    put_fixture("2:386/native.ini",100,0x92);
    put_fixture("3:DRIVERS/personal.txt",4096,0x93);
}
static void unchanged(void) {
    const char *paths[]={"2:386/config.ini","2:386/native.ini","3:DRIVERS/personal.txt"};
    for (unsigned k=0;k<3;k++) {
        int i=lookup(paths[k]); CHECK(i>=0 && disk[i].exists);
        for (size_t n=0;n<disk[i].size;n++) CHECK(disk[i].data[n]==0x91+k);
    }
}
static void updated(void) {
    unchanged();
    for (unsigned i=0;i<FILES;i++) {
        char d[96],s[96],b[96]; paths(i,d,s,b);
        int a=lookup(files[i]),target=lookup(d);
        CHECK(target>=0 && disk[target].exists && disk[target].size==disk[a].size);
        CHECK(!memcmp(disk[target].data,disk[a].data,disk[a].size));
        CHECK(present(s)==0 && present(b)==0);
    }
    CHECK(present(JOURNAL)==0 && present(COMMITTED)==0);
}
int main(void) {
    const char *error;
    fixture(); CHECK(installer_upgrade_files(flush,disk,NULL,&error));
    unsigned all_calls=calls,all_mutations=mutations;
    CHECK(activated==FILES && last_activation==0); updated();
    for (unsigned n=1;n<=all_calls;n++) {
        fixture(); fail_at=n;
        installer_upgrade_files(flush,disk,NULL,&error); unchanged();
        fail_at=0; CHECK(installer_upgrade_files(flush,disk,NULL,&error)); updated();
    }
    for (unsigned n=1;n<=all_mutations;n++) {
        fixture(); cut_at=n;
        if (!setjmp(reboot)) installer_upgrade_files(flush,disk,NULL,&error);
        unchanged(); cut_at=0;
        CHECK(installer_upgrade_files(flush,disk,NULL,&error)); updated();
    }
    fixture(); free_bytes=1; CHECK(!installer_upgrade_files(flush,disk,NULL,&error));
    CHECK(mutations==0); unchanged();
    fixture(); disk[lookup(files[2])].exists=0;
    CHECK(!installer_upgrade_files(flush,disk,NULL,&error)); CHECK(mutations==0); unchanged();
    fixture(); put_fixture("2:386/bios.bin.wup-new",10,0x77);
    CHECK(!installer_upgrade_files(flush,disk,NULL,&error)); CHECK(mutations==0); unchanged();
    fixture(); corrupt_stage=1;
    CHECK(!installer_upgrade_files(flush,disk,NULL,&error)); unchanged();
    CHECK(installer_upgrade_files(flush,disk,NULL,&error)); updated();
    printf("PASS: upgrade preserves settings/layout/guest files, stages and verifies all assets, activates EFI last; %u I/O failure points and %u process interruptions recover; source/space/conflict/readback checks\n",all_calls,all_mutations);
    return 0;
}
