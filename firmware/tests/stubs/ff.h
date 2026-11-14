#pragma once
#include <stdint.h>
typedef unsigned int UINT; typedef int FRESULT; enum { FR_OK=0 }; enum {FS_FAT12=1,FS_FAT16,FS_FAT32,FS_EXFAT}; enum {AM_RDO=1, AM_DIR=16};
typedef struct { int fs_type; } FATFS; typedef struct { struct { void* fs; } obj; void* host_file; } FIL;   /* host_file: FILE* in host tests */ typedef struct { const char* pat; } DIR;
typedef struct { uint32_t fsize; uint8_t fattrib; char fname[256]; } FILINFO;
#define FA_READ 1
#define FF_MAX_LFN 255
FRESULT f_open(FIL*,const char*,int); FRESULT f_close(FIL*);
FRESULT f_readdir(DIR*,FILINFO*); FRESULT f_read(FIL*,void*,UINT,UINT*); FRESULT f_write(FIL*,const void*,UINT,UINT*); FRESULT f_lseek(FIL*,uint32_t);
