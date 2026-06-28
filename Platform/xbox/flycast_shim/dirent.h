// flycast_shim/dirent.h -- minimal POSIX directory stub. The desktop directory
// listers that use it are excluded on Xbox (oslib/directory.cpp etc.); this
// exists only so any transitive include resolves.
#pragma once
#include <sys/types.h>

struct dirent { char d_name[260]; };
typedef struct _FLY_DIR DIR;

#ifdef __cplusplus
extern "C" {
#endif
DIR*           opendir(const char* name);
struct dirent* readdir(DIR* dir);
int            closedir(DIR* dir);
#ifdef __cplusplus
}
#endif
