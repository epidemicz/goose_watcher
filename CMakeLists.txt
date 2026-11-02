# The watcher package's native part: goose_watcher, the C layer over the
# OS's own directory notifications that watcher.goose declares its extern
# fns against (src/). Built by the Goose compiler through its wrapper
# project when a program needs the package (docs/design/packages.md §5.2),
# or into the compiler itself as a builtin package (§7.1). No dependencies
# beyond the C runtime and the OS: kernel32 only on Windows.

cmake_minimum_required(VERSION 3.20)
project(goose_watcher_package C)

add_library(goose_watcher STATIC
    src/watcher_api.h
    src/watcher_internal.h
    src/watcher_poll.c
)
# C17, as the runtime's C is.
set_target_properties(goose_watcher PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON
                      INTERPROCEDURAL_OPTIMIZATION OFF)
if(MSVC)
    target_compile_options(goose_watcher PRIVATE /W4 /utf-8)
    target_compile_definitions(goose_watcher PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(goose_watcher PRIVATE -Wall -Wextra)
endif()
