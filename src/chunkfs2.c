/*
 *  ChunkFS2 - mount files or block devices as a tree of chunk files
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
#include <sys/ioctl.h>
#if CONFIG_LINUX_BLKDEV
    #include <linux/fs.h>
#elif CONFIG_MACOS_DISK
    #include <sys/disk.h>
#endif

#define FUSE_USE_VERSION 30
#include <fuse.h>

#define MAX_DIR_DEPTH   3
#define MAX_CHUNKS      16777216

struct chunk_stat {
    int level;
    mode_t mode;
    off_t chunk;
    off_t offset;
    nlink_t nentry;
    off_t size;
    blkcnt_t blocks;
    nlink_t nlink;
};

struct chunkfs_t {
    const char *filename;
    int image_fd;
    struct stat image_stat;
    off_t image_size;
    off_t chunk_size;
    off_t image_chunks;
    int debug;
    int readonly;
};

static struct chunkfs_t chunkfs;
static char zero[4096];


static int resolve_path (const char *path, struct chunk_stat *st)
{
    size_t pathlen;
    off_t chunks_per_entry;
    off_t r;
    uint8_t tmp;

    pathlen = strlen(path);
    r = 0;

    if ((pathlen < 3) || (pathlen > MAX_DIR_DEPTH * 3) || (pathlen % 3)) {
        if (strcmp(path, "/")) {
            return -ENOENT;
        }
    } else {
        for (size_t x = 0; x < pathlen; x++) {
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

    chunks_per_entry = (off_t)1 << ((MAX_DIR_DEPTH - st->level - 1) * 8);
    st->nentry = (chunks_per_entry + (chunkfs.image_chunks - st->chunk) - 1) / chunks_per_entry;
    if (st->nentry > 256) {
        st->nentry = 256;
    }

    if (st->level < MAX_DIR_DEPTH) {
        st->mode = S_IFDIR;
        st->size = 0;
        st->blocks = 0;
        if (st->level < (MAX_DIR_DEPTH - 1)) {
            st->nlink = st->nentry + 2;
        } else {
            st->nlink = 2;
        }
    } else {
        st->mode = S_IFREG;
        st->size = (chunkfs.image_size - st->offset);
        if (st->size > chunkfs.chunk_size) {
            st->size = chunkfs.chunk_size;
        }
        st->blocks = (st->size + 4095) / 4096 * 8;
        st->nlink = 1;
    }

    return 0;
}


static int chunkfs_getattr (const char *path, struct stat *buf, struct fuse_file_info *fi)
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
    }

    buf->st_size = st.size;
    buf->st_blocks = st.blocks;
    buf->st_nlink = st.nlink;

    return 0;
}


static int chunkfs_readdir (const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    struct chunk_stat st;
    char nbuf[3];
    int rc;

    rc = resolve_path(path, &st);
    if (rc < 0) {
        return rc;
    }

    if (!S_ISDIR(st.mode)) {
        return -ENOTDIR;
    }

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    for (nlink_t x = 0; x < 256 && x < st.nentry; x++) {
        snprintf(nbuf, sizeof(nbuf), "%02" PRIx8, (unsigned char)x);
        filler(buf, nbuf, NULL, 0, 0);
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

    fi->fh = chunkfs.image_fd;

    return 0;
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

    if ((off_t)(offset + count) > st.size) {
        if (offset <= st.size) {
            count = st.size - offset;
        } else {
            return 0;
        }
    }

    if (chunkfs.debug) {
        printf("READ = %08" PRIx64 " - %08" PRIx64 "\n", st.offset + offset, st.offset + offset + count - 1);
    }

    rc = pread(fi->fh, buf, count, st.offset + offset);
    if (rc != (int)count) {
        return -EIO;
    }

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

    if ((off_t)(offset + count) > st.size) {
        return -EFBIG;
    }

    if (chunkfs.debug) {
        printf("WRITE = %08" PRIx64 " - %08" PRIx64 "\n", st.offset + offset, st.offset + offset + count - 1);
    }

    rc = pwrite(fi->fh, buf, count, st.offset + offset);
    if (rc != (int)count) {
        return -EIO;
    }

    return count;
}


static int chunkfs_truncate (const char *path, off_t count, struct fuse_file_info *fi)
{
    char buf[sizeof(zero)];
    struct chunk_stat st;
    int rc;

    rc = resolve_path(path, &st);
    if (rc < 0) {
        return rc;
    }

    if (S_ISDIR(st.mode)) {
        return -EISDIR;
    }

    if (count > st.size) {
        return -EFBIG;
    }

    if (chunkfs.debug) {
        printf("TRUNCATE = %08" PRIx64 " - %08" PRIx64 "\n", st.offset + count, st.offset + (st.size - count) - 1);
    }

    off_t n = (st.size - count);
    off_t offset = 0;

    while (n > 0) {

        off_t nz;
        if (n > (off_t)sizeof(zero)) {
            nz = sizeof(zero);
        } else {
            nz = n;
        }

        // Check if area we are "truncating" does already contain all zeros
        rc = pread(fi->fh, buf, nz, st.offset + count + offset);
        if (rc != (int)nz) {
            return -EIO;
        }

        // No point in overwriting zeros, as it will ruin sparse files
        if (memcmp(buf, zero, nz)) {
            rc = pwrite(fi->fh, zero, nz, st.offset + count + offset);
            if (rc != (int)nz) {
                return -EIO;
            }
        }

        n -= nz;
        offset += nz;

    }

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
    .mknod = (void *)chunkfs_permission_denied,
    .mkdir = (void *)chunkfs_permission_denied,
    .unlink = (void *)chunkfs_permission_denied,
    .rmdir = (void *)chunkfs_permission_denied,
    .symlink = (void *)chunkfs_permission_denied,
    .rename = (void *)chunkfs_permission_denied,
    .link = (void *)chunkfs_permission_denied,
    .chown = (void *)chunkfs_permission_denied,
    .chmod = (void *)chunkfs_permission_denied,
    .utimens = (void *)chunkfs_permission_denied,
};


static int mmap_image(const char *image_filename)
{
    int rc = -1;
    int fd;

    fd = open(image_filename, (chunkfs.readonly ? O_RDONLY : O_RDWR));
    if (fd < 0) {
        perror("open");
        goto err;
    }

    rc = fstat(fd, &chunkfs.image_stat);
    if (rc < 0) {
        perror("fstat");
        goto err;
    }

    if (S_ISREG(chunkfs.image_stat.st_mode)) {
        chunkfs.image_size = chunkfs.image_stat.st_size;
#if CONFIG_LINUX_BLKDEV
    } else if (S_ISBLK(chunkfs.image_stat.st_mode)) {
        uint64_t blksize;
        rc = ioctl(fd, BLKGETSIZE64, &blksize);
        if (rc == -1) {
            perror("ioctl");
            goto err;
        }
        chunkfs.image_size = blksize;
#elif CONFIG_MACOS_DISK
    } else if (S_ISBLK(chunkfs.image_stat.st_mode)) {
        uint32_t blksize;
        uint64_t blkcount;
        rc = ioctl(fd, DKIOCGETBLOCKSIZE, &blksize);
        if (rc == -1) {
            perror("ioctl(DKIOCGETBLOCKSIZE)");
            goto err;
        }
        rc = ioctl(fd, DKIOCGETBLOCKCOUNT, &blkcount);
        if (rc == -1) {
            perror("ioctl(DKIOCGETBLOCKCOUNT)");
            goto err;
        }
        chunkfs.image_size = (off_t)blksize * blkcount;
#endif
    } else {
        fprintf(stderr, "Not a file nor a block device: %s\n", image_filename);
        rc = -1;
        goto err;
    }

    chunkfs.image_chunks = (chunkfs.image_size + chunkfs.chunk_size - 1) / chunkfs.chunk_size;
    if (chunkfs.image_chunks > MAX_CHUNKS) {
        fprintf(stderr, "Maximum number of allowed chunks (%d) exceeded\n", MAX_CHUNKS);
        rc = -1;
        goto err;
    }

    chunkfs.image_fd = fd;

    return 0;

err:

    if (fd >= 0) {
        close(fd);
    }

    return rc;
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
    if (fuse_argv == NULL) {
        fprintf(stderr, "Failed to allocate memory\n");
        return -1;
    }

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
