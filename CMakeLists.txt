# FlashDump: the fx-CG50's flash as a read-only USB drive (gint add-in).
# Build with the fxSDK: fxsdk build-cg -> FLASHDMP.g3a
cmake_minimum_required(VERSION 3.15)
project(flashdump)

include(GenerateG3A)
include(Fxconv)
find_package(Gint 2.9 REQUIRED)

set(SOURCES src/main.c src/usbdev.c src/msc.c src/fatimg.c)

add_executable(flashdump ${SOURCES})
target_compile_options(flashdump PRIVATE -Wall -Wextra -Os)
target_link_libraries(flashdump Gint::Gint)

generate_g3a(TARGET flashdump OUTPUT "FLASHDMP.g3a"
  NAME "FlashDump" ICONS assets-cg/icon-uns.png assets-cg/icon-sel.png)
