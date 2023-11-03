cmake_minimum_required (VERSION 3.18)

# Set up C project
project (chunkfs2 C)

find_package (PkgConfig)
include (CheckSymbolExists)

# Set compiler flags
add_compile_options (-Wall -Wextra -Wno-unused-parameter -Werror -O2)

pkg_check_modules (FUSE REQUIRED fuse3)

# Add source files
add_executable (chunkfs2
    src/chunkfs2.c
)

target_compile_definitions (chunkfs2 PRIVATE "-D_FILE_OFFSET_BITS=64")

check_symbol_exists(BLKGETSIZE64 "linux/fs.h" CONFIG_LINUX_BLKDEV)
if(CONFIG_LINUX_BLKDEV)
    target_compile_definitions (chunkfs2 PRIVATE "-DCONFIG_LINUX_BLKDEV=1")
else()
    check_symbol_exists(DKIOCGETBLOCKCOUNT "sys/disk.h" CONFIG_MACOS_DISK)
    if(CONFIG_MACOS_DISK)
        target_compile_definitions (chunkfs2 PRIVATE "-DCONFIG_MACOS_DISK=1")
    endif()
endif()

target_include_directories (chunkfs2 PRIVATE ${FUSE_INCLUDE_DIRS})
target_link_directories (chunkfs2 PRIVATE ${FUSE_LIBRARY_DIRS})
target_link_libraries (chunkfs2 ${FUSE_LIBRARIES})

install(TARGETS chunkfs2 DESTINATION bin)
