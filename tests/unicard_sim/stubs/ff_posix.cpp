#include "ff.h"
#include <cstring>
#include <string>
#include <vector>
#include <cctype>
void* posix_opendir(const char*); void posix_closedir(void*); bool posix_readdir(void*, std::string&);
#include <sys/stat.h>
#include <unistd.h>
static std::string g_root[2];          // 0 = sd, 1 = flash
static std::string g_cwd[2] = {"/", "/"};
static int g_drv = 0;
static FATFS g_fs = {1000 + 2, 8};
void ffstub_init(const char* sd, const char* fl) { g_root[0] = sd; g_root[1] = fl; g_cwd[0] = g_cwd[1] = "/"; g_drv = 0; }
static int vol_of(const char*& p) {
    if (!strncasecmp(p, "sd:", 3)) { p += 3; return 0; }
    if (!strncasecmp(p, "flash:", 6)) { p += 6; return 1; }
    if (strchr(p, ':')) return -1;
    return g_drv;
}
// FatFS-style logical path -> (volume, absolute path within volume)
static bool resolve(const char* in, int& vol, std::string& out) {
    const char* p = in; vol = vol_of(p); if (vol < 0) return false;
    std::string rel = p;
    std::string full = (!rel.empty() && rel[0] == '/') ? rel : (g_cwd[vol] == "/" ? "/" + rel : g_cwd[vol] + "/" + rel);
    // normalise . and ..
    std::string res; size_t i = 0;
    std::vector<std::string> parts;
    while (i <= full.size()) {
        size_t j = full.find('/', i); if (j == std::string::npos) j = full.size();
        std::string seg = full.substr(i, j - i);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        i = j + 1;
    }
    for (auto& s : parts) res += "/" + s;
    out = res.empty() ? "/" : res;
    return true;
}
static std::string host(int vol, const std::string& abs) { return g_root[vol] + abs; }
static void fill_info(const std::string& hostpath, const std::string& name, FILINFO* fi) {
    struct stat st; memset(fi, 0, sizeof(*fi));
    if (stat(hostpath.c_str(), &st) == 0) {
        fi->fsize = S_ISDIR(st.st_mode) ? 0 : (FSIZE_t)st.st_size;
        fi->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : AM_ARC;
        fi->fdate = (WORD)(((2026 - 1980) << 9) | (9 << 5) | 6); fi->ftime = (WORD)((12 << 11) | (30 << 5));
    }
    strncpy(fi->fname, name.c_str(), FF_MAX_LFN);
    // 8.3 alias: uppercase, truncated, like FatFS does for names that need one
    std::string sfn; for (char c : name) { if (c == '.' && sfn.find('.') == std::string::npos) sfn += c; else if (isalnum((unsigned char)c)) sfn += (char)toupper(c); }
    if (name.size() > 12 || sfn != name) { size_t dot = sfn.find('.'); std::string b = dot == std::string::npos ? sfn : sfn.substr(0, dot), e = dot == std::string::npos ? "" : sfn.substr(dot + 1, 3); if (b.size() > 8) b = b.substr(0, 6) + "~1"; sfn = e.empty() ? b : b + "." + e; }
    else sfn = "";
    strncpy(fi->altname, sfn.c_str(), 12);
}
FRESULT f_open(FIL* f, const char* path, BYTE mode) {
    int v; std::string a; if (!resolve(path, v, a)) return FR_INVALID_DRIVE;
    std::string h = host(v, a); struct stat st; bool exists = stat(h.c_str(), &st) == 0;
    if ((mode & FA_CREATE_NEW) && exists) return FR_EXIST;
    if (!(mode & (FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS)) && !exists) return FR_NO_FILE;
    const char* m = (mode & FA_CREATE_ALWAYS) || ((mode & FA_CREATE_NEW) && !exists) ? "w+b" : (mode & FA_WRITE) ? (exists ? "r+b" : "w+b") : "rb";
    f->fp = fopen(h.c_str(), m); if (!f->fp) return FR_DENIED;
    fseek(f->fp, 0, SEEK_END); f->obj.objsize = (FSIZE_t)ftell(f->fp); fseek(f->fp, 0, SEEK_SET); f->fptr = 0;
    return FR_OK;
}
FRESULT f_close(FIL* f) { if (f->fp) fclose(f->fp); f->fp = nullptr; return FR_OK; }
FRESULT f_read(FIL* f, void* b, UINT n, UINT* br) { fseek(f->fp, f->fptr, SEEK_SET); *br = (UINT)fread(b, 1, n, f->fp); f->fptr += *br; return FR_OK; }
FRESULT f_write(FIL* f, const void* b, UINT n, UINT* bw) { fseek(f->fp, f->fptr, SEEK_SET); *bw = (UINT)fwrite(b, 1, n, f->fp); f->fptr += *bw; if (f->fptr > f->obj.objsize) f->obj.objsize = f->fptr; return FR_OK; }
FRESULT f_lseek(FIL* f, FSIZE_t p) { if (p > f->obj.objsize) { fseek(f->fp, 0, SEEK_END); while (f->obj.objsize < p) { fputc(0, f->fp); f->obj.objsize++; } } f->fptr = p; return FR_OK; }
FRESULT f_truncate(FIL* f) { fflush(f->fp); if (ftruncate(fileno(f->fp), f->fptr)) return FR_DISK_ERR; f->obj.objsize = f->fptr; return FR_OK; }
FRESULT f_sync(FIL* f) { fflush(f->fp); return FR_OK; }
FRESULT f_opendir(DIR* d, const char* path) { int v; std::string a; if (!resolve(path, v, a)) return FR_INVALID_DRIVE; void* dd = posix_opendir(host(v, a).c_str()); if (!dd) return FR_NO_PATH; d->d = dd; d->path = new std::string(host(v, a)); return FR_OK; }
FRESULT f_closedir(DIR* d) { if (d->d) posix_closedir(d->d); d->d = nullptr; delete d->path; d->path = nullptr; return FR_OK; }
FRESULT f_readdir(DIR* d, FILINFO* fi) {
    std::string name;
    if (!posix_readdir(d->d, name)) { fi->fname[0] = 0; return FR_OK; }
    fill_info(*d->path + "/" + name, name, fi); return FR_OK;
}
FRESULT f_mkdir(const char* p) { int v; std::string a; if (!resolve(p, v, a)) return FR_INVALID_DRIVE; return mkdir(host(v, a).c_str(), 0777) ? FR_EXIST : FR_OK; }
FRESULT f_unlink(const char* p) { int v; std::string a; if (!resolve(p, v, a)) return FR_INVALID_DRIVE; std::string h = host(v, a); if (rmdir(h.c_str()) == 0) return FR_OK; return unlink(h.c_str()) ? FR_NO_FILE : FR_OK; }
FRESULT f_rename(const char* o, const char* n) { int v1, v2; std::string a, b; if (!resolve(o, v1, a) || !resolve(n, v2, b)) return FR_INVALID_DRIVE; return rename(host(v1, a).c_str(), host(v2, b).c_str()) ? FR_NO_FILE : FR_OK; }
FRESULT f_stat(const char* p, FILINFO* fi) { int v; std::string a; if (!resolve(p, v, a)) return FR_INVALID_DRIVE; struct stat st; std::string h = host(v, a); if (stat(h.c_str(), &st)) return FR_NO_FILE; size_t s = a.rfind('/'); fill_info(h, a.substr(s + 1), fi); return FR_OK; }
FRESULT f_chdir(const char* p) { int v; std::string a; if (!resolve(p, v, a)) return FR_INVALID_DRIVE; struct stat st; if (stat(host(v, a).c_str(), &st) || !S_ISDIR(st.st_mode)) return FR_NO_PATH; g_cwd[v] = a; return FR_OK; }
FRESULT f_chdrive(const char* p) { const char* q = p; int v = vol_of(q); if (v < 0 || !strchr(p, ':')) return FR_INVALID_DRIVE; g_drv = v; return FR_OK; }
FRESULT f_getcwd(char* b, UINT n) { snprintf(b, n, "%s:%s", g_drv == 0 ? "sd" : "flash", g_cwd[g_drv].c_str()); return FR_OK; }
FRESULT f_getfree(const char*, DWORD* n, FATFS** fs) { *n = 500; *fs = &g_fs; return FR_OK; }
