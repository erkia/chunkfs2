# ChunkFS2

This is a successor of the original ChunkFS by Florian Zumbiehl (https://chunkfs.florz.de/)

## Features compared to ChunkFS

* Adds all the missing white-space that did not make it to the original code :)
* Adds write support
* Uses [mmap()](https://man7.org/linux/man-pages/man2/mmap.2.html) for better performance
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
