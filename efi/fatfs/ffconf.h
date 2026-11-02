/*---------------------------------------------------------------------------/
/  FatFs configuration for wyseDOS
/
/  Every volume is one partition (or a whole floppy-like medium) that
/  fatvol.c maps onto a UEFI Block IO device, so FatFs itself never sees a
/  partition table: no GPT logic, no 64-bit LBA and no exFAT are needed.
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	80286	/* Revision ID */

#define FF_FS_READONLY	0
#define FF_FS_MINIMIZE	0
#define FF_USE_FIND		0
#define FF_USE_MKFS		1	/* the installer formats the eMMC */
#define FF_USE_FASTSEEK	1	/* disk images: cluster link maps */
#define FF_USE_EXPAND	1	/* disk images are created contiguous */
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	1
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0

#define FF_CODE_PAGE	437

#define FF_USE_LFN		1	/* static working buffer: one caller at a time */
#define FF_MAX_LFN		255
#define FF_LFN_UNICODE	0
#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
#define FF_FS_RPATH		0

#define FF_VOLUMES		4	/* 0 data, 1 installer source, 2 target ESP, 3 target data */
#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"RAM","NAND","CF","SD","SD2","USB","USB2","USB3"
#define FF_MULTI_PARTITION	0

#define FF_MIN_SS		512
#define FF_MAX_SS		512
#define FF_LBA64		0
#define FF_MIN_GPT		0x10000000
#define FF_USE_TRIM		0

#define FF_FS_TINY		0
#define FF_FS_EXFAT		0
#define FF_FS_NORTC		0	/* get_fattime() from the Wyse's clock */
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026
#define FF_FS_NOFSINFO	0

#define FF_FS_LOCK		0
#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000
