# Build configuration for the C version of the collage and zoom-tour tools.
#
# Two executables are produced:
#   collage  - renders the 9:16 Instagram Stories collage (and, with --video,
#              the zoom-tour MP4)
#   video    - renders the zoom-tour MP4 from an existing collage image
#
# External dependencies are located through pkg-config (libheif, libwebp) and
# the standard threading package. The stb single-header libraries live in
# third_party/ and need no installation.
#
# Author: Pasquale Marzaioli

cmake_minimum_required(VERSION 3.16)
project(collage_tools C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Enable optimisation and warnings. -O3 favours the heavy pixel loops; the
# warning flags keep the code honest without turning warnings into errors.
set(CMAKE_C_FLAGS_RELEASE "-O3")
add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HEIF REQUIRED libheif)
pkg_check_modules(WEBP REQUIRED libwebp)
find_package(Threads REQUIRED)

# Source files shared by both executables.
set(SHARED_SOURCES
    src/image.c
    src/video_render.c
    src/parallel.c
    src/stb_impl.c
)

include_directories(third_party src)

add_executable(collage src/collage.c ${SHARED_SOURCES})
add_executable(video src/video.c ${SHARED_SOURCES})

foreach(target collage video)
    target_include_directories(${target} PRIVATE ${HEIF_INCLUDE_DIRS} ${WEBP_INCLUDE_DIRS})
    target_link_directories(${target} PRIVATE ${HEIF_LIBRARY_DIRS} ${WEBP_LIBRARY_DIRS})
    target_link_libraries(${target} PRIVATE ${HEIF_LIBRARIES} ${WEBP_LIBRARIES} Threads::Threads m)
endforeach()
