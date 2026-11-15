# Host tests for the bit-level G64 emulation (not part of the firmware build)
#
#   cmake -S firmware/tests -B build-tests
#   cmake --build build-tests --config Release
#   ctest --test-dir build-tests -C Release
#
# gcr_resolve_test also accepts G64 files as arguments for the full checks
# (comparison with the VICE read circuit model, rotation and seek tests):
#   gcr_resolve_test path/to/image.g64 ...
# drive_harness takes one optional G64 (ideally with half-tracks): it is
# loaded with the firmware loader, stepped through 1.0-2.5 and saved again
#   drive_harness path/to/bbsb.g64

cmake_minimum_required(VERSION 3.13)
project(FluxPico_host_tests C)

set(CMAKE_C_STANDARD 11)
set(FW ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(gcr_resolve_test gcr_resolve_test.c ${FW}/src/gcr_resolve.c)
target_include_directories(gcr_resolve_test PRIVATE ${FW}/include)

# drive.c with fake timer/GPIO (stub SDK headers in ./stubs)
add_executable(drive_harness
    drive_harness.c
    ${FW}/src/drive.c
    ${FW}/src/globals.c
    ${FW}/src/gcr_resolve.c
    ${FW}/src/rw_routines.c
    ${FW}/src/gcr.c)
target_include_directories(drive_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${FW}/include)
target_compile_definitions(drive_harness PRIVATE _CRT_SECURE_NO_WARNINGS)

# sorted directory listing with a fake FatFS directory
add_executable(dirlist_test dirlist_test.c ${FW}/src/dirlist.c)
target_include_directories(dirlist_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${FW}/include)

if(MSVC)
    target_compile_definitions(gcr_resolve_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(dirlist_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

enable_testing()
add_test(NAME gcr_resolve_synthetic COMMAND gcr_resolve_test)
add_test(NAME drive_harness COMMAND drive_harness)
add_test(NAME dirlist COMMAND dirlist_test)
