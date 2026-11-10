/* Files read from any number of threads at once (shared_file.h) */

#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>
#include <retro_atomic.h>

#include "shared_file.h"

/* streams kept for reads, where the file is not mapped: one for each
 * thread that reads such a file at once, as many as the core has */
#define SHARED_FILE_STREAM_SLOTS 4

struct shared_file
{
   RFILE *map_file; /* the mapping's, where mapped */
   const uint8_t *mapped; /* its octets, while map_file lives */
   char *path; /* where not mapped, for the streams past the first */
#ifdef RETRO_ATOMIC_HAS_PTR
   retro_atomic_ptr_t streams[SHARED_FILE_STREAM_SLOTS];
#else
   RFILE *first; /* one-core targets: one stream, the reads one at a time */
#endif
   retro_atomic_int_t opened;
   uint64_t size;
};

/* A stream for a read: one the file keeps if one is free, a new one if not */
static RFILE *shared_file_acquire(shared_file_t *file)
{
   RFILE *stream;
#ifdef RETRO_ATOMIC_HAS_PTR
   unsigned i;
   for (i = 0; i < SHARED_FILE_STREAM_SLOTS; i++)
   {
      if (!retro_atomic_load_relaxed_ptr(&file->streams[i]))
         continue;
      stream = (RFILE*)retro_atomic_exchange_ptr(&file->streams[i], NULL);
      if (stream)
         return stream;
   }
#else
   if (file->first)
   {
      stream = file->first;
      file->first = NULL;
      return stream;
   }
#endif
   stream = filestream_open(file->path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (stream)
      retro_atomic_fetch_add_int(&file->opened, 1);
   return stream;
}

/* Kept in a free slot, closed if there is none */
static void shared_file_release(shared_file_t *file, RFILE *stream)
{
#ifdef RETRO_ATOMIC_HAS_PTR
   unsigned i;
   for (i = 0; i < SHARED_FILE_STREAM_SLOTS; i++)
   {
      if (     !retro_atomic_load_relaxed_ptr(&file->streams[i])
            && retro_atomic_cas_ptr(&file->streams[i], NULL, stream))
         return;
   }
#else
   if (!file->first)
   {
      file->first = stream;
      return;
   }
#endif
   filestream_close(stream);
}

shared_file_t *shared_file_open(const char *path, unsigned flags)
{
   int64_t size;
   int64_t mapped_len = 0;
   const uint8_t *mapped = NULL;
   shared_file_t *file;
   RFILE *stream;
#ifdef RETRO_ATOMIC_HAS_PTR
   unsigned i;
#endif

   /* a directory is not a file: some opens succeed on one */
   if (!path || path_is_directory(path))
      return NULL;
   stream = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
         (flags & SHARED_FILE_NO_MAP) ? RETRO_VFS_FILE_ACCESS_HINT_NONE
         : RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
   if (!stream)
      return NULL;
   size = filestream_get_size(stream);
   file = (shared_file_t*)calloc(1, sizeof(*file));
   if (!file || size < 0)
   {
      free(file);
      filestream_close(stream);
      return NULL;
   }
   file->size = (uint64_t)size;
   retro_atomic_int_init(&file->opened, 0);
#ifdef RETRO_ATOMIC_HAS_PTR
   for (i = 0; i < SHARED_FILE_STREAM_SLOTS; i++)
      retro_atomic_ptr_init(&file->streams[i], NULL);
#endif
   if (!(flags & SHARED_FILE_NO_MAP))
      mapped = filestream_get_mapped_ptr(stream, &mapped_len);
   /* the whole of it, or it is read as if not mapped; an empty file has
    * nothing to map and nothing to read */
   if ((mapped && mapped_len == size) || size == 0)
   {
      file->map_file = stream;
      file->mapped = size ? mapped : NULL;
      return file;
   }
   {
      size_t path_len = strlen(path);
      file->path = (char*)malloc(path_len + 1);
      if (!file->path)
      {
         filestream_close(stream);
         free(file);
         return NULL;
      }
      memcpy(file->path, path, path_len + 1);
   }
   /* the stream opened is the first one lent */
   retro_atomic_fetch_add_int(&file->opened, 1);
   shared_file_release(file, stream);
   return file;
}

void shared_file_close(shared_file_t *file)
{
#ifdef RETRO_ATOMIC_HAS_PTR
   unsigned i;
#endif
   if (!file)
      return;
   if (file->map_file)
      filestream_close(file->map_file);
#ifdef RETRO_ATOMIC_HAS_PTR
   for (i = 0; i < SHARED_FILE_STREAM_SLOTS; i++)
   {
      RFILE *stream = (RFILE*)retro_atomic_exchange_ptr(&file->streams[i], NULL);
      if (stream)
         filestream_close(stream);
   }
#else
   if (file->first)
      filestream_close(file->first);
#endif
   free(file->path);
   free(file);
}

uint64_t shared_file_size(const shared_file_t *file)
{
   return file->size;
}

const uint8_t *shared_file_mapped(const shared_file_t *file)
{
   return file->mapped;
}

unsigned shared_file_streams_opened(const shared_file_t *file)
{
   return (unsigned)retro_atomic_load_relaxed_int((retro_atomic_int_t*)&file->opened);
}

int64_t shared_file_read_at(shared_file_t *file, void *dst, int64_t len, uint64_t offset)
{
   RFILE *stream;
   int64_t got = -1;
   if (len < 0)
      return -1;
   if (offset >= file->size || len == 0)
      return 0;
   if ((uint64_t)len > file->size - offset)
      len = (int64_t)(file->size - offset);
   if (file->mapped)
   {
      memcpy(dst, file->mapped + offset, (size_t)len);
      return len;
   }
   stream = shared_file_acquire(file);
   if (!stream)
      return -1;
   if (filestream_seek(stream, (int64_t)offset, RETRO_VFS_SEEK_POSITION_START) == 0)
      got = filestream_read(stream, dst, len);
   shared_file_release(file, stream);
   return got;
}
