# ChunkFS2

This is a successor of the original ChunkFS by Florian Zumbiehl (https://chunkfs.florz.de/)

## Features compared to ChunkFS

* Adds all the missing white-space that did not make it to the original code :)
* Adds write support
* Supports libfuse3, libfuse and macfuse
* Fixes reported chunk file block counts (so, for example, `du` reports the correct size)
* Reduces directory tree depth

## Why?

ChunkFS2 allows making incremental hardlinked rsync backups of a large file by splitting up single file
to many smaller fixed-sized files (chunks). This way, if part of the large file changes between backups,
only the changed chunks need to be re-created by rsync, all the un-modified chunks can be kept hardlinked
on the backup.

To restore from the backup, just rsync the backed up chunks back to the ChunkFS2 mount. The sizes of the
backed up file and file that is being restored need to be the same. ChunkFS2 does not change the size
of the original mounted file, so for successful restore, the chunk sizes and the number of chunks need to
be the same as they were at the time of the backup.

## How to build

As any other CMake project:

```
$ cmake -B build .
$ cmake --build build
$ sudo cmake --build build --target install
```

## Example

This example shows how to create snapshots of a file using rsync, so that the incremental snapshots will
only take as much disk space as large is the changed part of the file between two snapshots.

Create a random file with a size of 1 MiB and calculate its initial checksum

```
$ head -c 1048576 /dev/urandom > test.img
$ sha256sum test.img
ec5f10f79988ffba3489883f1b5a429f5b26e2fe8c4690e3ac91fcadb17db8d5  test.img
```

Mount the image as 256 x 4K chunks

```
$ mkdir mountpoint
$ chunkfs2 -orw -z 4096 test.img mountpoint
```

Verify that the mountpoint indeed contains 256 files, totalling 1 MiB

```
$ du -s -b mountpoint
1048576 mountpoint
$ find mountpoint -type f | wc -l
256
```

Create snapshots directory and make the first (full) snapshot. `--checksum` must be used when making snapshots, because file size and modification time cannot
be used the detect if the chunks contents are changed in chunkfs2. `--inplace` option must not be used when making snapshots.

```
$ mkdir -p snapshots
$ rsync --recursive --checksum mountpoint/ snapshots/snapshot1/
```

Check the size of the snapshots directory (note that the size is slightly bigger because of the additional directory entries)

```
$ du -s -b snapshots
1064960 snapshots
```

Make a change in the middle of the file, i.e. simulating random write on the disk image

```
$ dd if=/dev/urandom of=test.img bs=60000 count=1 seek=5 conv=notrunc
1+0 records in
1+0 records out
60000 bytes (60 kB, 59 KiB) copied, 0,000781176 s, 76,8 MB/s
```

Remember the new checksum

```
$ sha256sum test.img
b56b88b4d60d4660a7200b837febe3f3fc3ded03ca10d0da6a0a7c1a9611c4aa  test.img
```

Create a second (incremental) snapshot by first making hardlinked copy of snapshot1 to snapshot2 and then rsyncing chunks to snapshot2

```
$ cp -rl snapshots/snapshot1 snapshots/snapshot2
$ rsync --recursive --checksum mountpoint/ snapshots/snapshot2/
```

Note that the actual size of snapshots directory is increased roughly by the size of the changed chunks

```
$ du -s -b snapshots
1138688 snapshots
```

Now restore the initial image from snapshot1 and verify the checksum. Again, `--checksum` option must be used. `--inplace` option is also required, because
otherwise rsync will try to create temporary files on the chunkfs2 mount, which is not possible and also not necessary.

```
$ rsync --recursive --checksum --inplace snapshots/snapshot1/ mountpoint/
$ sha256sum test.img
ec5f10f79988ffba3489883f1b5a429f5b26e2fe8c4690e3ac91fcadb17db8d5  test.img
```

Great success! Let's try to restore the second snapshot and verify the checksum

```
$ rsync --recursive --checksum --inplace snapshots/snapshot2/ mountpoint/
$ sha256sum test.img
b56b88b4d60d4660a7200b837febe3f3fc3ded03ca10d0da6a0a7c1a9611c4aa  test.img
```

Again, great success.
