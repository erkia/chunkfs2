cmake_minimum_required (VERSION 3.18)

# Set up C project
project (chunkfs2 C)

# Set compiler flags
add_compile_options (-Wall -Wextra -Wno-unused-parameter -Werror -O2)

find_package (PkgConfig)
include (CheckSymbolExists)

pkg_check_modules (FUSE REQUIRED fuse)
check_symbol_exists(BLKGETSIZE64 "linux/fs.h" CONFIG_LINUX_BLKDEV)

# Add source files
add_executable (chunkfs2
    src/chunkfs2.c
)

target_compile_definitions (chunkfs2 PRIVATE "-D_FILE_OFFSET_BITS=64")
if(CONFIG_LINUX_BLKDEV)
    target_compile_definitions (chunkfs2 PRIVATE "-DCONFIG_LINUX_BLKDEV=1")
endif()

target_include_directories (chunkfs2 PRIVATE ${FUSE_INCLUDE_DIRS})
target_link_directories (chunkfs2 PRIVATE ${FUSE_LIBRARY_DIRS})
target_link_libraries (chunkfs2 ${FUSE_LIBRARIES})

install(TARGETS chunkfs2 DESTINATION bin)
