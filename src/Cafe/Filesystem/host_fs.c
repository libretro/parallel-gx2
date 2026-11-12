/* A file or directory of a mounted host folder (host_fs.h) */

#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>
#include <retro_dirent.h>
#include <streams/file_stream.h>

#include "Common/host_path.h"
#include "host_fs.h"

#define TWO_GIB 0x80000000UL

struct host_fs_node
{
   RFILE *file;          /* a file's */
   uint64_t size;
   uint64_t pos;
   int writable;
   char *dir_path;       /* a directory's */
   struct RDIR *dir;
   int dir_opened;
};

/* the file at path, resolved, opened with mode; NULL for none, or for a
 * directory (some opens succeed on one) */
static RFILE *open_file(const char *path, unsigned mode)
{
   char *p = host_path_resolve(path);
   RFILE *f = NULL;
   if (!p)
      return NULL;
   if (!path_is_directory(p))
      f = filestream_open(p, mode, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   free(p);
   return f;
}

host_fs_node_t *host_fs_open(const char *path, unsigned flags, bool *create_failed)
{
   const unsigned existing = (flags & HOST_FS_WRITE)
      ? RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING
      : RETRO_VFS_FILE_ACCESS_READ;
   host_fs_node_t *n;
   if (create_failed)
      *create_failed = false;
   if (flags & HOST_FS_OPEN_FILE)
   {
      RFILE *f;
      if (flags & HOST_FS_ALLOW_CREATE)
      {
         if (!(f = open_file(path, existing))
               && !(f = open_file(path, RETRO_VFS_FILE_ACCESS_READ_WRITE))
               && create_failed)
            *create_failed = true;
      }
      else if (flags & HOST_FS_ALWAYS_CREATE)
      {
         if (!(f = open_file(path, RETRO_VFS_FILE_ACCESS_READ_WRITE)) && create_failed)
            *create_failed = true;
      }
      else
         f = open_file(path, existing);
      if (f)
      {
         int64_t size = filestream_get_size(f);
         if (!(n = (host_fs_node_t*)calloc(1, sizeof(*n))))
         {
            filestream_close(f);
            return NULL;
         }
         n->file = f;
         n->writable = (flags & HOST_FS_WRITE) != 0;
         n->size = size < 0 ? 0 : (uint64_t)size;
         return n;
      }
   }
   if ((flags & HOST_FS_OPEN_DIR) && path_is_directory(path))
   {
      size_t len = strlen(path);
      if (!(n = (host_fs_node_t*)calloc(1, sizeof(*n))))
         return NULL;
      if (!(n->dir_path = (char*)malloc(len + 1)))
      {
         free(n);
         return NULL;
      }
      memcpy(n->dir_path, path, len + 1);
      return n;
   }
   return NULL;
}

void host_fs_close(host_fs_node_t *n)
{
   if (!n)
      return;
   if (n->file)
      filestream_close(n->file);
   if (n->dir)
      retro_closedir(n->dir);
   free(n->dir_path);
   free(n);
}

bool host_fs_is_dir(const host_fs_node_t *n)
{
   return n->file == NULL;
}

bool host_fs_writable(const host_fs_node_t *n)
{
   return n->writable != 0;
}

uint64_t host_fs_size(const host_fs_node_t *n)
{
   return n->file ? n->size : 0;
}

uint64_t host_fs_tell(const host_fs_node_t *n)
{
   return n->file ? n->pos : 0;
}

uint32_t host_fs_read(host_fs_node_t *n, void *data, uint32_t size)
{
   uint64_t left;
   int64_t got;
   if (!n->file || size >= TWO_GIB)
      return 0;
   left = n->pos < n->size ? n->size - n->pos : 0;
   if (left > TWO_GIB - 1)
      left = TWO_GIB - 1;
   if (size > left)
      size = (uint32_t)left;
   got = filestream_read(n->file, data, (int64_t)size);
   if (got <= 0)
      return 0;
   n->pos += (uint64_t)got;
   return (uint32_t)got;
}

uint32_t host_fs_write(host_fs_node_t *n, const void *data, uint32_t size)
{
   int64_t put;
   if (!n->file || size >= TWO_GIB)
      return 0;
   put = filestream_write(n->file, data, (int64_t)size);
   if (put <= 0)
      return 0;
   n->pos += (uint64_t)put;
   if (n->pos > n->size)
      n->size = n->pos;
   return (uint32_t)put;
}

void host_fs_seek(host_fs_node_t *n, uint64_t pos)
{
   if (!n->file)
      return;
   n->pos = pos;
   filestream_seek(n->file, (int64_t)pos, RETRO_VFS_SEEK_POSITION_START);
}

bool host_fs_set_length(host_fs_node_t *n, uint64_t end)
{
   bool ok;
   if (!n->file)
      return false;
   ok = filestream_truncate(n->file, (int64_t)end) == 0;
   if (n->pos > end)
      n->pos = end;
   n->size = n->pos;
   filestream_seek(n->file, (int64_t)n->pos, RETRO_VFS_SEEK_POSITION_START);
   return ok;
}

void host_fs_flush(host_fs_node_t *n)
{
   if (n->file && n->writable)
      filestream_flush(n->file);
}

bool host_fs_dir_next(host_fs_node_t *n, char *name, size_t name_size,
      bool *is_dir, uint64_t *file_size, bool *failed)
{
   if (failed)
      *failed = false;
   if (n->file)
      return false;
   if (!n->dir_opened)
   {
      n->dir_opened = 1;
      n->dir = retro_opendir_include_hidden(n->dir_path, true);
      if (!n->dir && failed)
         *failed = true;
   }
   if (!n->dir)
      return false;
   while (retro_readdir(n->dir))
   {
      const char *entry = retro_dirent_get_name(n->dir);
      size_t len;
      int64_t size = 0;
      if (!entry || !strcmp(entry, ".") || !strcmp(entry, ".."))
         continue;
      if (name_size)
      {
         len = strlen(entry);
         if (len > name_size - 1)
            len = name_size - 1;
         memcpy(name, entry, len);
         name[len] = '\0';
      }
      *is_dir = retro_dirent_is_dir(n->dir, NULL);
      *file_size = 0;
      if (!*is_dir && retro_dirent_stat(n->dir, &size, NULL) && size > 0)
         *file_size = (uint64_t)size;
      return true;
   }
   return false;
}
