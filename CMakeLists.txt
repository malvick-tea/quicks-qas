# qas — build definition (seed build system: CMake, temporary; see ADR-0007).
#
# Kept deliberately small and tool-agnostic so the eventual port to our own build
# tool (qmk) is mechanical. CMake locates the seed compiler itself (e.g. the
# Visual Studio generator finds MSVC), so this configures from any shell.

cmake_minimum_required(VERSION 3.20)
project(qas LANGUAGES C)

# Language baseline: portable C11 with NO compiler extensions (ADR-0006). This is
# what keeps the host tools buildable under MSVC, GCC/Clang, and later qcc.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Warnings are errors (coding-standard.md §1). A warning is a defect.
if (MSVC)
    add_compile_options(/W4 /WX /permissive-)
    # We deliberately use the portable ISO C library functions (fopen, fread, …)
    # rather than Microsoft's *_s variants (ADR-0009). Silence the CRT's
    # "deprecation" of the standard functions so /WX does not trip on them.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# qas_core: all modules except the CLI entry point. Tests link this so they can
# exercise the logic without dragging in main().
add_library(qas_core STATIC
    src/status/status.c
    src/source/source.c
    src/diag/diag.c
    src/token/token.c
    src/lexer/lexer.c
    src/reg/reg.c
    src/buf/buf.c
    src/elf/elf.c
    src/elf/internal/serialize.c
    src/arena/arena.c
)

# Modules include each other as "module/header.h"; src/ is the single include
# root. PUBLIC so both the executable and the tests inherit it.
target_include_directories(qas_core PUBLIC src)

# The qas command-line tool.
add_executable(qas src/app/main.c)
target_link_libraries(qas PRIVATE qas_core)

# Unit tests (each suite is its own executable + CTest test).
enable_testing()
add_subdirectory(tests)
