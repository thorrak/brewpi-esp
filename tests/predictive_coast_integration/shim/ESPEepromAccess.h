#pragma once
#include <cstdio>
#define FS_PREFIX "."
FILE* fs_open(const char* name, const char* mode);
bool fs_exists(const char* name);
bool fs_remove(const char* name);
