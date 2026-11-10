/* A file's octets, mapped or read once (mapped_file.h) */

#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>
#include <streams/file_stream.h>

#include "mapped_file.h"

bool mapped_file_open(mapped_file_t *mf, const char *path)
{
   const uint8_t *map;
   int64_t len = 0;
   int64_t size;
   memset(mf, 0, sizeof(*mf));
   /* a directory is not a file, and some opens succeed on one */
   if (!path || !*path || path_is_directory(path))
      return false;
   mf->stream = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
   if (!mf->stream)
      return false;
   map = filestream_get_mapped_ptr(mf->stream, &len);
   if (map && len > 0 && (uint64_t)len <= (uint64_t)(size_t)-1)
   {
      /* MAP_SHARED: what is written into the mapped range shows, and a
       * rename over the path leaves these pages as they were */
      mf->data = map;
      mf->size = (size_t)len;
      mf->opened = true;
      mf->mapped = true;
      return true;
   }
   /* what cannot be mapped is read, once */
   size = filestream_get_size(mf->stream);
   if (size > 0 && (uint64_t)size <= (uint64_t)(size_t)-1)
   {
      int64_t got;
      mf->copy = (uint8_t*)malloc((size_t)size);
      got = mf->copy && filestream_seek(mf->stream, 0, RETRO_VFS_SEEK_POSITION_START) == 0
         ? filestream_read(mf->stream, mf->copy, size) : -1;
      if (got < 0)
      {
         mapped_file_close(mf);
         return false;
      }
      mf->data = mf->copy;
      mf->size = (size_t)got;
   }
   else if (size < 0)
   {
      mapped_file_close(mf);
      return false;
   }
   filestream_close(mf->stream);
   mf->stream = NULL;
   mf->opened = true;
   return true;
}

void mapped_file_close(mapped_file_t *mf)
{
   if (mf->stream)
      filestream_close(mf->stream);
   free(mf->copy);
   memset(mf, 0, sizeof(*mf));
}
