cmake_minimum_required(VERSION 3.18)
project(winddroid C)

# The engine sources live at the repository root; the app only adds the JNI bridge.
set(WD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

add_library(winddroid SHARED
    jni_bridge.c
    ${WD_ROOT}/core/cpu.c
    ${WD_ROOT}/core/mmu.c
    ${WD_ROOT}/core/sysreg.c
    ${WD_ROOT}/devices/gic.c
    ${WD_ROOT}/devices/simple.c
    ${WD_ROOT}/host/machine.c)

target_include_directories(winddroid PRIVATE ${WD_ROOT}/include ${WD_ROOT}/core)
target_compile_options(winddroid PRIVATE -O2 -std=c11 -D_GNU_SOURCE -Wall -Wno-unused-parameter)
target_link_libraries(winddroid log)
