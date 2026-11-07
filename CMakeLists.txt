cmake_minimum_required(VERSION 3.20)
project(AutoBackupSaveGame VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "AutoBackupSaveGame builds with Visual Studio (MSVC).")
endif()

# The platform submodule, for its header (fh2mod_api). The platform DLL itself is built by the workflow.
add_subdirectory(platform EXCLUDE_FROM_ALL)

add_library(AutoBackupSaveGame SHARED src/backup_mod.cpp)
target_link_libraries(AutoBackupSaveGame PRIVATE fh2mod_api bcrypt)
set_target_properties(AutoBackupSaveGame PROPERTIES OUTPUT_NAME AutoBackupSaveGame PREFIX "")
target_compile_definitions(AutoBackupSaveGame PRIVATE
    AUTOBACKUP_VERSION="${PROJECT_VERSION}"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# Static runtime: no second C++ runtime DLL loaded into Azimuth.
set_property(TARGET AutoBackupSaveGame PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(AutoBackupSaveGame PRIVATE /W4 /permissive- /utf-8 /EHsc)
