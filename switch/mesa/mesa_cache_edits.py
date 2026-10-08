"""LSWTCS: enable Mesa's shader disk cache on the Switch (horizon) build of switch-mesa 20.1.0-rc3.
Applied on top of devkitPro's switch-mesa-20.1.0-5.patch. Usage: python mesa_cache_edits.py <mesa-src-dir>"""
import sys, os
root = sys.argv[1]

def edit(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, newline='').read()
    for old, new in pairs:
        assert s.count(old) == 1, (rel, old[:70])
        s = s.replace(old, new)
    open(p, 'w', newline='').write(s)

# 0. Build host: Python 3.12 has no distutils (Mesa 20.1's mako check imports it).
edit('meson.build', [
    ("from distutils.version import StrictVersion\nimport mako\nassert StrictVersion(mako.__version__) > StrictVersion(\"0.8.0\")",
     "import mako\nassert tuple(int(x) for x in mako.__version__.split('.')[:2]) > (0, 8)"),
])

# 1. meson: allow the shader cache on horizon, and give it zlib (devkitPro nulls zlib there).
edit('meson.build', [
    ("  if ['windows','horizon'].contains(host_machine.system())\n    if _shader_cache == 'true'",
     "  if host_machine.system() == 'windows'\n    if _shader_cache == 'true'"),
    ("else\n  dep_zlib = null_dep\n  dep_zstd = null_dep\n  dep_thread = null_dep\n  dep_expat = null_dep\nendif",
     "else\n  # LSWTCS: zlib for the shader disk cache (switch-zlib)\n  dep_zlib = dependency('zlib', version : '>= 1.2.3')\n"
     "  pre_args += '-DHAVE_ZLIB'\n"
     "  # LSWTCS: current newlib declares timespec_get (the cross has_function check cannot link-test it)\n"
     "  pre_args += '-DHAVE_TIMESPEC_GET'\n"
     "  dep_zstd = null_dep\n  dep_thread = null_dep\n  dep_expat = null_dep\nendif"),
])

# 2. Driver identity: no dladdr/build-id in a static NRO, so use a fixed id (bump it when Mesa changes).
edit('src/util/disk_cache.h', [
    ("static inline bool\ndisk_cache_get_function_identifier(void *ptr, struct mesa_sha1 *ctx)\n{\n   return false;\n#endif\n}",
     "static inline bool\ndisk_cache_get_function_identifier(void *ptr, struct mesa_sha1 *ctx)\n{\n"
     "#ifdef __SWITCH__\n   /* LSWTCS: static build, no build-id: fixed driver identity. */\n"
     "   static const char id[] = \"switch-mesa-20.1.0-rc3-5+lswtcs-cache-1\";\n"
     "   _mesa_sha1_update(ctx, id, sizeof(id));\n   return true;\n#else\n   return false;\n#endif\n#endif\n}"),
])

# 3. disk_cache.c: no mmap / getpwuid on horizon; persistent in-memory index written back per change.
edit('src/util/disk_cache.c', [
    ("#include <sys/mman.h>\n", "#ifndef __SWITCH__\n#include <sys/mman.h>\n#endif\n"),
    ("#include \"zlib.h\"\n", "#include \"zlib.h\"\n#ifndef O_CLOEXEC\n#define O_CLOEXEC 0\n#endif\n"),
    ("   uint8_t *index_mmap;\n   size_t index_mmap_size;\n",
     "   uint8_t *index_mmap;\n   size_t index_mmap_size;\n#ifdef __SWITCH__\n"
     "   /* LSWTCS: index kept in memory (no mmap on horizon), written back to this fd on change. */\n"
     "   int index_fd;\n#endif\n"),
    # getpwuid fallback: horizon always uses MESA_GLSL_CACHE_DIR / XDG_CACHE_HOME
    ("   if (path == NULL) {\n      char *buf;\n      size_t buf_size;\n      struct passwd pwd, *result;\n",
     "#ifndef __SWITCH__\n   if (path == NULL) {\n      char *buf;\n      size_t buf_size;\n      struct passwd pwd, *result;\n"),
    ("      path = concatenate_and_mkdir(local, path, CACHE_DIR_NAME);\n      if (path == NULL)\n         goto path_fail;\n   }\n\n   cache->path = ralloc_strdup(cache, path);",
     "      path = concatenate_and_mkdir(local, path, CACHE_DIR_NAME);\n      if (path == NULL)\n         goto path_fail;\n   }\n"
     "#else\n   if (path == NULL)\n      goto path_fail;\n#endif\n\n   cache->path = ralloc_strdup(cache, path);"),
    ("   cache->index_mmap = mmap(NULL, size, PROT_READ | PROT_WRITE,\n                            MAP_SHARED, fd, 0);\n   if (cache->index_mmap == MAP_FAILED)\n      goto path_fail;",
     "#ifdef __SWITCH__\n   cache->index_mmap = calloc(1, size);\n   if (cache->index_mmap == NULL)\n      goto path_fail;\n"
     "   {\n      size_t got = 0;\n      lseek(fd, 0, SEEK_SET);\n      while (got < size) {\n"
     "         ssize_t n = read(fd, cache->index_mmap + got, size - got);\n         if (n <= 0)\n            break;\n"
     "         got += n;\n      }\n   }\n   cache->index_fd = fd;\n   fd = -1;   /* kept open for write-back */\n#else\n"
     "   cache->index_mmap = mmap(NULL, size, PROT_READ | PROT_WRITE,\n                            MAP_SHARED, fd, 0);\n"
     "   if (cache->index_mmap == MAP_FAILED)\n      goto path_fail;\n#endif"),
    ("      munmap(cache->index_mmap, cache->index_mmap_size);",
     "#ifdef __SWITCH__\n      free(cache->index_mmap);\n      close(cache->index_fd);\n#else\n"
     "      munmap(cache->index_mmap, cache->index_mmap_size);\n#endif"),
    ("#ifdef HAVE_FLOCK\n   err = flock(fd, LOCK_EX | LOCK_NB);\n#else",
     "#if defined(__SWITCH__)\n   err = 0;   /* LSWTCS: single process, no file locks on horizon */\n#elif defined(HAVE_FLOCK)\n"
     "   err = flock(fd, LOCK_EX | LOCK_NB);\n#else"),
    # No util_queue on horizon: devkitPro's c11 threads shim never initialises u_queue's global list
    # (call_once), so creating the cache's writer thread scribbles through NULL list pointers (Eden:
    # unmapped writes at 0x10, render thread then hangs at random points). Entries are written
    # synchronously by the caller instead: only on a compile, which is already a stall.
    ("   util_queue_init(&cache->cache_queue, \"disk$\", 32, 4,\n                   UTIL_QUEUE_INIT_RESIZE_IF_FULL |\n"
     "                   UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY |\n                   UTIL_QUEUE_INIT_SET_FULL_THREAD_AFFINITY);\n",
     "#ifndef __SWITCH__\n   util_queue_init(&cache->cache_queue, \"disk$\", 32, 4,\n                   UTIL_QUEUE_INIT_RESIZE_IF_FULL |\n"
     "                   UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY |\n                   UTIL_QUEUE_INIT_SET_FULL_THREAD_AFFINITY);\n#endif\n"),
    ("      util_queue_finish(&cache->cache_queue);\n      util_queue_destroy(&cache->cache_queue);\n",
     "#ifndef __SWITCH__\n      util_queue_finish(&cache->cache_queue);\n      util_queue_destroy(&cache->cache_queue);\n#endif\n"),
    ("disk_cache_wait_for_idle(struct disk_cache *cache)\n{\n   util_queue_finish(&cache->cache_queue);\n}",
     "disk_cache_wait_for_idle(struct disk_cache *cache)\n{\n#ifndef __SWITCH__\n   util_queue_finish(&cache->cache_queue);\n#endif\n}"),
    ("      util_queue_fence_init(&dc_job->fence);\n      util_queue_add_job(&cache->cache_queue, dc_job, &dc_job->fence,\n"
     "                         cache_put, destroy_put_job, dc_job->size);\n",
     "#ifdef __SWITCH__\n      cache_put(dc_job, 0);\n      destroy_put_job(dc_job, 0);\n#else\n"
     "      util_queue_fence_init(&dc_job->fence);\n      util_queue_add_job(&cache->cache_queue, dc_job, &dc_job->fence,\n"
     "                         cache_put, destroy_put_job, dc_job->size);\n#endif\n"),
    # The SD card file system cannot rename an open file (EIO): close the temp file first (its flock
    # only guards against other processes, and there are none).
    ("   ret = rename(filename_tmp, filename);\n",
     "#ifdef __SWITCH__\n   close(fd);\n   fd = -1;\n#endif\n   ret = rename(filename_tmp, filename);\n"),
    ("   entry = &cache->stored_keys[i * CACHE_KEY_SIZE];\n\n   memcpy(entry, key, CACHE_KEY_SIZE);\n}",
     "   entry = &cache->stored_keys[i * CACHE_KEY_SIZE];\n\n   memcpy(entry, key, CACHE_KEY_SIZE);\n"
     "#ifdef __SWITCH__\n   lswtcs_index_sync(cache, entry, CACHE_KEY_SIZE);\n#endif\n}"),
])

# Write-back helper + size updates. Inserted after the struct definition (before the first function).
p = os.path.join(root, 'src/util/disk_cache.c')
s = open(p, newline='').read()
anchor = "/* Create a directory named 'path' if it does not already exist."
assert s.count(anchor) == 1
helper = ("#ifdef __SWITCH__\n/* LSWTCS: write an index region (stored key or total size) back to the index file. */\n"
          "static void\nlswtcs_index_sync(struct disk_cache *cache, const void *p, size_t len)\n{\n"
          "   const off_t off = (const uint8_t *) p - cache->index_mmap;\n"
          "   if (cache->index_fd >= 0)\n      pwrite(cache->index_fd, p, len, off);\n}\n#endif\n\n")
s = s.replace(anchor, helper + anchor)
# every p_atomic_add on cache->size is followed by a size write-back
import re
count = 0
def addsync(m):
    global count
    count += 1
    return m.group(0) + "\n#ifdef __SWITCH__\n   lswtcs_index_sync(%s, %s, sizeof(uint64_t));\n#endif" % (m.group(1), m.group(1) + "->size")
s = re.sub(r"p_atomic_add\(((?:dc_job->)?cache)->size, [^;]*\);", addsync, s)
open(p, 'w', newline='').write(s)
print('edits applied; size write-backs:', count)
