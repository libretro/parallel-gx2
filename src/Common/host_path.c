/* Paths on the host's file systems (host_path.h) */

#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>
#include <retro_dirent.h>
#include <streams/file_stream.h>

#include "host_path.h"

static char *dup_n(const char *s, size_t n)
{
   char *c = (char*)malloc(n + 1);
   if (c)
   {
      memcpy(c, s, n);
      c[n] = '\0';
   }
   return c;
}

/* dir, then '/' unless it ends in one, then name */
static char *join(const char *dir, const char *name)
{
   size_t d = strlen(dir), n = strlen(name);
#ifdef _WIN32
   int sep = d && dir[d - 1] != '/' && dir[d - 1] != '\\';
#else
   int sep = d && dir[d - 1] != '/';
#endif
   char *out = (char*)malloc(d + (size_t)sep + n + 1);
   if (!out)
      return NULL;
   memcpy(out, dir, d);
   if (sep)
      out[d] = '/';
   memcpy(out + d + (size_t)sep, name, n + 1);
   return out;
}

#ifndef _WIN32
static int same_ignoring_case(const char *a, const char *b)
{
   for (;; a++, b++)
   {
      char x = *a, y = *b;
      if (x >= 'A' && x <= 'Z')
         x = (char)(x - 'A' + 'a');
      if (y >= 'A' && y <= 'Z')
         y = (char)(y - 'A' + 'a');
      if (x != y)
         return 0;
      if (!x)
         return 1;
   }
}

/* the entry of dir named name ignoring case, joined to dir; name as given
 * when there is none */
static char *find_in(const char *dir, const char *name)
{
   struct RDIR *d = retro_opendir_include_hidden(dir, true);
   char *found = NULL;
   if (d)
   {
      while (retro_readdir(d))
      {
         const char *entry = retro_dirent_get_name(d);
         if (entry && same_ignoring_case(entry, name))
         {
            found = join(dir, entry);
            break;
         }
      }
      retro_closedir(d);
   }
   return found ? found : join(dir, name);
}
#endif

char *host_path_resolve(const char *path)
{
#ifdef _WIN32
   return dup_n(path, strlen(path));
#else
   const char *slash;
   char *parent, *out;
   if (path_is_valid(path))
      return dup_n(path, strlen(path));
   /* the parent: as given if it is there, else resolved itself */
   slash = strrchr(path, '/');
   if (!slash)
      parent = dup_n(".", 1);
   else if (slash == path)
      parent = dup_n("/", 1);
   else
   {
      parent = dup_n(path, (size_t)(slash - path));
      if (parent && !path_is_valid(parent))
      {
         char *resolved = host_path_resolve(parent);
         free(parent);
         parent = resolved;
      }
   }
   if (!parent)
      return NULL;
   out = find_in(parent, slash ? slash + 1 : path);
   free(parent);
   return out;
#endif
}

bool host_remove_all(const char *path)
{
   struct RDIR *d;
   /* a file, an empty directory or a link goes at once - a link to a
    * directory without what that holds; only a directory that is not
    * empty has its entries removed first */
   if (filestream_delete(path) == 0)
      return true;
   if (!(path_stat(path) & RETRO_VFS_STAT_IS_VALID))
      return true;
   if (!path_is_directory(path))
      return false;
   if ((d = retro_opendir_include_hidden(path, true)))
   {
      while (retro_readdir(d))
      {
         const char *name = retro_dirent_get_name(d);
         char *child;
         if (!name || !strcmp(name, ".") || !strcmp(name, ".."))
            continue;
         if ((child = join(path, name)))
         {
            host_remove_all(child);
            free(child);
         }
      }
      retro_closedir(d);
   }
   return filestream_delete(path) == 0;
}
