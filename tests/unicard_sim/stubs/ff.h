// Minimal FatFS API over POSIX for the host-side Unicard harness.
// Volumes: "sd:" and "flash:" map to directories given at init.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
typedef unsigned int UINT; typedef uint32_t DWORD; typedef uint16_t WORD; typedef uint8_t BYTE; typedef uint32_t FSIZE_t;
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH, FR_INVALID_NAME, FR_DENIED, FR_EXIST,
    FR_INVALID_OBJECT, FR_WRITE_PROTECTED, FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM, FR_MKFS_ABORTED, FR_TIMEOUT,
    FR_LOCKED, FR_NOT_ENOUGH_CORE, FR_TOO_MANY_OPEN_FILES, FR_INVALID_PARAMETER } FRESULT;
#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_NEW 0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS 0x10
#define AM_RDO 0x01
#define AM_HID 0x02
#define AM_SYS 0x04
#define AM_DIR 0x10
#define AM_ARC 0x20
#define FF_USE_LFN 1
#define FF_MAX_LFN 255
typedef struct { FSIZE_t objsize; } FFOBJID;
typedef struct { FFOBJID obj; FSIZE_t fptr; FILE* fp; } FIL;
typedef struct { void* d; std::string* path; } DIR;
typedef struct { FSIZE_t fsize; WORD fdate; WORD ftime; BYTE fattrib; char altname[13]; char fname[FF_MAX_LFN + 1]; } FILINFO;
typedef struct { DWORD n_fatent; WORD csize; } FATFS;
#define f_size(fp) ((fp)->obj.objsize)
#define f_tell(fp) ((fp)->fptr)
#define f_eof(fp) ((int)((fp)->fptr == (fp)->obj.objsize))
FRESULT f_open(FIL*, const char*, BYTE);
FRESULT f_close(FIL*);
FRESULT f_read(FIL*, void*, UINT, UINT*);
FRESULT f_write(FIL*, const void*, UINT, UINT*);
FRESULT f_lseek(FIL*, FSIZE_t);
FRESULT f_truncate(FIL*);
FRESULT f_sync(FIL*);
FRESULT f_opendir(DIR*, const char*);
FRESULT f_closedir(DIR*);
FRESULT f_readdir(DIR*, FILINFO*);
FRESULT f_mkdir(const char*);
FRESULT f_unlink(const char*);
FRESULT f_rename(const char*, const char*);
FRESULT f_stat(const char*, FILINFO*);
FRESULT f_chdir(const char*);
FRESULT f_chdrive(const char*);
FRESULT f_getcwd(char*, UINT);
FRESULT f_getfree(const char*, DWORD*, FATFS**);
void ffstub_init(const char* sd_root, const char* flash_root);
