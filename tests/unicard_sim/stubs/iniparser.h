#pragma once
typedef struct _dictionary_ { int dummy; } dictionary;
inline const char* iniparser_getstring(dictionary*, const char*, const char* def) { return def; }
inline int iniparser_getboolean(dictionary*, const char*, int def) { return def; }
inline int iniparser_getint(dictionary*, const char*, int def) { return def; }
