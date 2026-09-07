// POSIX directory access isolated from ff.h (whose DIR typedef clashes with dirent's)
#include <dirent.h>
#include <cstring>
#include <string>
void* posix_opendir(const char* p) { return opendir(p); }
void posix_closedir(void* d) { closedir((DIR*)d); }
bool posix_readdir(void* d, std::string& name) {
    for (;;) { struct dirent* e = readdir((DIR*)d); if (!e) return false;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        name = e->d_name; return true; }
}
