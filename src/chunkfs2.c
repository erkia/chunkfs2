/*
 *  ChunkFS2 - mount files and images as a tree of chunk files
 *  Copyright (C) 2023  Erki Aring <erki@example.ee>
 *
 *  Original ChunkFS - https://chunkfs.florz.de/
 *  Copyright (C) 2007-2013  Florian Zumbiehl <florz@florz.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#define FUSE_USE_VERSION 29
#include <fuse.h>

#define MAX_DIR_DEPTH   3
#define MAX_CHUNKS      16777216

struct chunk_stat {
    int level;
    mode_t mode;
    off_t chunk;
    off_t offset;
    off_t size;
};

struct chunkfs_t {
    const char *filename;
    void *image;
    struct stat image_stat;
    off_t image_size;
    off_t chunk_size;
    off_t image_chunks;
    int debug;
    int readonly;
};

struct chunkfs_thd_t {
    int last_chunk_exists;
    off_t last_chunk_offset;
    off_t last_chunk_size;
};

static struct chunkfs_t chunkfs;
static __thread struct chunkfs_thd_t chunkfs_thd;


static int resolve_path (const char *path, struct chunk_stat *st)
{
    size_t pathlen;
    off_t r;
    uint8_t tmp;

    pathlen = strlen(path);
    r = 0;

    if ((pathlen < 3) || (pathlen > MAX_DIR_DEPTH * 3) || (pathlen % 3)) {
        if (strcmp(path, "/")) {
            return -ENOENT;
        }
    } else {
        for (int x = 0; x < pathlen; x++) {
            tmp = path[x];
            if (x % 3) {
                if (tmp >= '0' && tmp <= '9') {
                    tmp -= '0';
                } else if (tmp >= 'a' && tmp <= 'f') {
                    tmp -= 'a' - 10;
                } else {
                    return -ENOENT;
                }
                r |= (off_t)tmp << ((MAX_DIR_DEPTH * 2 - x / 3 * 2 - x % 3) * 4);
            } else if (tmp != '/') {
                return -ENOENT;
            }
        }
        if (r >= chunkfs.image_chunks) {
            return -ENOENT;
        }
    }

    st->level = pathlen / 3;
    st->chunk = r;
    st->offset = st->chunk * chunkfs.chunk_size;

    if (st->level < MAX_DIR_DEPTH) {
        st->mode = S_IFDIR;
    } else {
        st->mode = S_IFREG;
        st->size = (chunkfs.image_size - st->offset);
        if (st->size > chunkfs.chunk_size) {
            st->size = chunkfs.chunk_size;
        }
    }

    return 0;
}


static int chunkfs_getattr (const char *path, struct stat *buf)
{
    struct chunk_stat st;
    int rc;

    rc = resolve_path(path, &st);
    if (rc < 0) {
        return rc;
    }

    memcpy(buf, &chunkfs.image_stat, sizeof(struct stat));

    buf->st_mode = (buf->st_mode & ~(S_IFMT | S_IXUSR | S_IXGRP | S_IXOTH)) | (st.mode & S_IFMT);

    if (S_ISDIR(st.mode)) {
        buf->st_mode = buf->st_mode | ((buf->st_mode & S_IRUSR) ? S_IXUSR : 0) | ((buf->st_mode & S_IRGRP) ? S_IXGRP : 0) | ((buf->st_mode & S_IROTH) ? S_IXOTH : 0);
        buf->st_nlink = 256 + 2; // TODO: wrong!
        buf->st_size = 0;
        buf->st_blocks = 0;
    } else {
        buf->st_nlink = 1;
        buf->st_size = st.size;
        buf->st_blocks = (buf->st_size + 4095) / 4096 * 8;
    }

    return 0;
}


static int chunkfs_readdir (const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    struct chunk_stat st;
    off_t chunks_per_entry;
    int rc;

    rc = resolve_path(path, &st);
    if (rc < 0) {
        return rc;
    }

    if (!S_ISDIR(st.mode)) {
        return -ENOTDIR;
    }

    chunks_per_entry = (off_t)1 << ((MAX_DIR_DEPTH - st.level - 1) * 8);

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for (off_t x = 0; x < 256 && st.chunk + x * chunks_per_entry < chunkfs.image_chunks; x++) {
        char nbuf[3];
        snprintf(nbuf, sizeof(nbuf), "%02" PRIx64, x);
        filler(buf, nbuf, NULL, 0);
    }

    return 0;
}


static int chunkfs_open (const char *path, struct fuse_file_info *fi)
{
    struct chunk_stat st;
    int rc;

    rc = resolve_path(path, &st);
    if (rc < 0) {
        return rc;
    }

    if (chunkfs.readonly) {
        if ((fi->flags & O_ACCMODE) != O_RDONLY) {
            return -EROFS;
        }
    }

    return 0;
}


static void chunkfs_free (off_t offset, off_t size)
{
    if (chunkfs_thd.last_chunk_exists) {
        if (chunkfs_thd.last_chunk_offset != offset) {
            if (chunkfs.debug) {
                printf("madvise(0x%08lX, 0x%08lX, MADV_DONTNEED)\n", chunkfs_thd.last_chunk_offset, chunkfs_thd.last_chunk_size);
            }
            madvise(chunkfs.image + chunkfs_thd.last_chunk_offset, chunkfs_thd.last_chunk_size, MADV_DONTNEED);
        }
    }

    chunkfs_thd.last_chunk_exists = 1;
    chunkfs_thd.last_chunk_offset = offset;
    chunkfs_thd.last_chunk_size = size;
}


static int chunkfs_read (const char *path, char *buf, size_t count, off_t offset, struct fuse_file_info *fi)
{
    struct chunk_stat st;
    int rc;

    rc = resolve_path(path, &st);
    if (rc < 0) {
        return rc;
    }

    if (S_ISDIR(st.mode)) {
        return -EISDIR;
    }

    chunkfs_free(st.offset, st.size);

    if (chunkfs.debug) {
        printf("READ = %08lX - %08lX\n", st.offset + offset, st.offset + offset + count - 1);
    }

    memcpy(buf, chunkfs.image + st.offset + offset, count);

    return count;
}


static int chunkfs_write (const char *path, const char *buf, size_t count, off_t offset, struct fuse_file_info *fi)
{
    struct chunk_stat st;
    int rc;

    rc = resolve_path(path, &st);
    if (rc < 0) {
        return rc;
    }

    if (S_ISDIR(st.mode)) {
        return -EISDIR;
    }

    chunkfs_free(st.offset, st.size);

    if (chunkfs.debug) {
        printf("WRITE = %08lX - %08lX\n", st.offset + offset, st.offset + offset + count - 1);
    }

    memcpy(chunkfs.image + st.offset + offset, buf, count);

    return count;
}


static int chunkfs_truncate (const char *path, off_t count)
{
    struct chunk_stat st;
    int rc;

    rc = resolve_path(path, &st);
    if (rc < 0) {
        return rc;
    }

    if (S_ISDIR(st.mode)) {
        return -EISDIR;
    }

    if (count > chunkfs.chunk_size) {
        return -EINVAL;
    }

    chunkfs_free(st.offset, st.size);

    if (chunkfs.debug) {
        printf("TRUNCATE = %08lX - %08lX\n", st.offset + count, st.offset + (chunkfs.chunk_size - count) - 1);
    }

    memset(chunkfs.image + st.offset + count, 0, chunkfs.chunk_size - count);

    return 0;
}


static int chunkfs_permission_denied ()
{
    return -EPERM;
}


static struct fuse_operations chunkfs_ops = {
    .getattr = chunkfs_getattr,
    .readdir = chunkfs_readdir,
    .open = chunkfs_open,
    .read = chunkfs_read,
    .write = chunkfs_write,
    .truncate = chunkfs_truncate,
    .mknod = chunkfs_permission_denied,
    .mkdir = chunkfs_permission_denied,
    .unlink = chunkfs_permission_denied,
    .rmdir = chunkfs_permission_denied,
    .symlink = chunkfs_permission_denied,
    .rename = chunkfs_permission_denied,
    .link = chunkfs_permission_denied,
    .chown = chunkfs_permission_denied,
    .chmod = chunkfs_permission_denied,
    .utimens = chunkfs_permission_denied,
};


static int mmap_image(const char *image_filename)
{
    int rc;
    int fd;

    fd = open(image_filename, (chunkfs.readonly ? O_RDONLY : O_RDWR));
    if (fd < 0) {
        perror("open");
        return fd;
    }

    rc = fstat(fd, &chunkfs.image_stat);
    if (rc < 0) {
        perror("fstat");
        return rc;
    }

    chunkfs.image_size = chunkfs.image_stat.st_size;

    chunkfs.image_chunks = (chunkfs.image_size + chunkfs.chunk_size - 1) / chunkfs.chunk_size;
    if (chunkfs.image_chunks > MAX_CHUNKS) {
        fprintf(stderr, "Maximum number of allowed chunks (%d) exceeded\n", MAX_CHUNKS);
        return -1;
    }

    if (chunkfs.image_size) {
        chunkfs.image = mmap(NULL, chunkfs.image_size, PROT_READ | (!chunkfs.readonly ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
        if (chunkfs.image == MAP_FAILED) {
            perror("mmap");
            return -1;
        }
    }

    return 0;
}


int main (int argc, char *argv[])
{
    char **fuse_argv;
    int fuse_argc = 0;
    int invalid_opts = 0;
    int opt;
    int rc;

    chunkfs.chunk_size = 1048576;
    chunkfs.debug = 0;

    fuse_argv = malloc((argc + 1) * sizeof(char *));
    fuse_argv[fuse_argc++] = strdup(argv[0]);

    // "dfso:" are fuse options, copy them over and filter out "z:", which is chunkfs option
    while ((opt = getopt(argc, argv, "dfso:z:")) != -1) {
        if (fuse_argc >= argc) {
            fprintf(stderr, "Failed to create fuse options\n");
            invalid_opts = 1;
            break;
        }
        if (opt == 'z') {
            char *end = NULL;
            errno = 0;
            chunkfs.chunk_size = strtoll(optarg, &end, 10);
            if (errno || *end || chunkfs.chunk_size < 4096 || (chunkfs.chunk_size % 4096)) {
                fprintf(stderr, "Invalid chunk size: %s\n", optarg);
                invalid_opts = 1;
            }
        } else if (opt == '?') {
            invalid_opts = 1;
        } else {
            char optbuf[4];
            snprintf(optbuf, sizeof(optbuf), "-%c", opt);
            fuse_argv[fuse_argc++] = strdup(optbuf);
            if (optarg) {
                fuse_argv[fuse_argc++] = strdup(optarg);
            }
            if (opt == 'o') {
                if (!strcmp(optarg, "debug")) {
                    chunkfs.debug = 1;
                } else if (!strcmp(optarg, "ro")) {
                    chunkfs.readonly = 1;
                }
            }
        }
    }

    if (argc - optind != 2) {
        invalid_opts = 1;
    }

    if (invalid_opts) {
        fprintf(stderr,
            "Usage: %s [options] <image> <mount_point>\n"
            "\n"
            "chunkfs options:\n"
            "    -z chunk_size          chunk size, must be multiple of 4096 (default = 1048576)\n"
            "\n",
            argv[0]
        );

        char *help_argv[3] = {argv[0], "-ho", NULL};
        return fuse_main(sizeof(help_argv) / sizeof(help_argv[0]) - 1, help_argv, &chunkfs_ops, NULL);
    }

    fuse_argv[fuse_argc++] = strdup(argv[optind + 1]);
    fuse_argv[fuse_argc] = NULL;

    chunkfs.filename = argv[optind];

    if (chunkfs.debug) {
        printf("chunkfs.filename = %s\n", chunkfs.filename);

        for (int x = 0; x < fuse_argc; x++) {
            printf("fuse_argv[%d] = %s\n", x, fuse_argv[x]);
        }
    }

    rc = mmap_image(chunkfs.filename);
    if (rc < 0) {
        return rc;
    }

    return fuse_main(fuse_argc, fuse_argv, &chunkfs_ops, NULL);
}
