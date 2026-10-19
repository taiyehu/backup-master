cmake_minimum_required(VERSION 3.20)

project(BackupMaster VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(backup_core
    src/archive_format.cpp
    src/archive_service.cpp
    src/filter.cpp
    src/manifest.cpp
    src/metadata.cpp
    src/scanner.cpp
    src/special_files.cpp
    src/symlink.cpp
)
target_include_directories(backup_core PUBLIC include)
target_include_directories(backup_core PRIVATE src)

if(WIN32)
    target_link_libraries(backup_core PRIVATE advapi32)
endif()

if(MSVC)
    target_compile_options(backup_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(backup_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(backup-master src/main.cpp)
target_link_libraries(backup-master PRIVATE backup_core)
target_include_directories(backup-master PRIVATE src)

include(CTest)
if(BUILD_TESTING)
    add_executable(backup_core_tests tests/archive_tests.cpp)
    target_link_libraries(backup_core_tests PRIVATE backup_core)
    target_include_directories(backup_core_tests PRIVATE src)
    add_test(NAME backup_core_tests COMMAND backup_core_tests)
endif()

