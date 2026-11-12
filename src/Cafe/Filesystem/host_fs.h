#ifndef CAFE_FILESYSTEM_HOST_FS_H
#define CAFE_FILESYSTEM_HOST_FS_H

/* A file or directory of a host folder mounted into the virtual file
 * system (fscDeviceHostFS.cpp), through libretro-common's VFS. C89.
 *
 * A file's path is matched without regard to case where it does not exist
 * as given (host_path.h); a directory's is taken as given. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct host_fs_node host_fs_node_t;

#define HOST_FS_OPEN_FILE     1u  /* a file, if there is one */
#define HOST_FS_OPEN_DIR      2u  /* else a directory, if there is one */
#define HOST_FS_WRITE         4u  /* the file to be written too */
#define HOST_FS_ALLOW_CREATE  8u  /* the file made if it is not there */
#define HOST_FS_ALWAYS_CREATE 16u /* the file made anew, emptied */

/* NULL for nothing there to open; *create_failed (when not NULL) true when
 * a file was to be made and could not be */
host_fs_node_t *host_fs_open(const char *path, unsigned flags, bool *create_failed);
void host_fs_close(host_fs_node_t *n);

bool host_fs_is_dir(const host_fs_node_t *n);
bool host_fs_writable(const host_fs_node_t *n);
/* a file's size, as it was opened and as writes and set_length made it */
uint64_t host_fs_size(const host_fs_node_t *n);
uint64_t host_fs_tell(const host_fs_node_t *n);

/* At most size octets from the position, none past the end; what was read
 * or written, the position moved past it. 0 for a directory, or for 2 GiB
 * or more asked for at once. */
uint32_t host_fs_read(host_fs_node_t *n, void *data, uint32_t size);
uint32_t host_fs_write(host_fs_node_t *n, const void *data, uint32_t size);
void host_fs_seek(host_fs_node_t *n, uint64_t pos);
/* the file cut or grown to end there, the position no further; false if
 * the host would not */
bool host_fs_set_length(host_fs_node_t *n, uint64_t end);
/* a file opened to be written: what it holds put in the host file */
void host_fs_flush(host_fs_node_t *n);

/* A directory's next entry but . and .., listed the first time it is
 * asked: its name cut to fit name_size, whether it is a directory, and a
 * file's size. False at the end, and *failed (when not NULL) true when the
 * directory could not be listed at all. */
bool host_fs_dir_next(host_fs_node_t *n, char *name, size_t name_size,
      bool *is_dir, uint64_t *file_size, bool *failed);

RETRO_END_DECLS

#endif
