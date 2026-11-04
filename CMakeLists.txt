# The watcher package's native part: goose_watcher, the C layer over the
# OS's own directory notifications that watcher.goose declares its extern
# fns against (src/). Linux, macOS and Windows (inotify, kqueue,
# ReadDirectoryChangesW).

cmake_minimum_required(VERSION 3.20)
project(goose_watcher C)

add_library(goose_watcher STATIC
    src/watcher_poll.c
)
# C17. No -Werror, deliberate: warnings stay visible without breaking builds.
set_target_properties(goose_watcher PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(goose_watcher PRIVATE /W4 /utf-8)
    target_compile_definitions(goose_watcher PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(goose_watcher PRIVATE -Wall -Wextra)
endif()
find_package(Threads)
target_link_libraries(goose_watcher PRIVATE Threads::Threads)
