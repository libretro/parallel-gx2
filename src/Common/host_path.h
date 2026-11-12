#ifndef COMMON_HOST_PATH_H
#define COMMON_HOST_PATH_H

/* Paths on the host's file systems, through libretro-common's VFS like the
 * files themselves. C89. */

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* The path as the file system has it, matched without regard to ASCII case
 * where it does not exist as given: titles name their files in whatever
 * case, on a console whose file system does not care. A component that
 * matches nothing is kept as given. On Windows, whose file systems do not
 * care either, path as it is. A malloc'd string; NULL for no memory. */
char *host_path_resolve(const char *path);

/* A file, or a directory with all it holds (a link in it goes, not what it
 * points at); true if nothing is left there */
bool host_remove_all(const char *path);

RETRO_END_DECLS

#endif
