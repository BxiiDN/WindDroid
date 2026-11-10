cmake_minimum_required(VERSION 3.18)
project(winddroid C)

# The engine sources live at the repository root; the app only adds the JNI bridge.
set(WD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

add_library(winddroid SHARED
    jni_bridge.c
    ${WD_ROOT}/core/cpu.c
    ${WD_ROOT}/core/mmu.c
    ${WD_ROOT}/core/sysreg.c
    ${WD_ROOT}/core/simd.c
    ${WD_ROOT}/core/fpu.c
    ${WD_ROOT}/core/jit.c
    ${WD_ROOT}/devices/gic.c
    ${WD_ROOT}/devices/its.c
    ${WD_ROOT}/devices/simple.c
    ${WD_ROOT}/devices/flash.c
    ${WD_ROOT}/devices/fwcfg.c
    ${WD_ROOT}/devices/pci.c
    ${WD_ROOT}/devices/ramfb.c
    ${WD_ROOT}/devices/disk.c
    ${WD_ROOT}/devices/nvme.c
    ${WD_ROOT}/devices/image.c
    ${WD_ROOT}/devices/usb.c
    ${WD_ROOT}/devices/usb_storage.c
    ${WD_ROOT}/devices/usb_net.c
    ${WD_ROOT}/devices/nat.c
    ${WD_ROOT}/devices/xhci.c
    ${WD_ROOT}/host/machine.c
    ${WD_ROOT}/host/diag.c
    ${WD_ROOT}/host/platform.c
    ${WD_ROOT}/host/fdt.c
    ${WD_ROOT}/host/acpi.c)

target_include_directories(winddroid PRIVATE ${WD_ROOT}/include ${WD_ROOT}/core)
target_compile_options(winddroid PRIVATE -O2 -frounding-math -std=c11 -D_GNU_SOURCE -Wall -Wno-unused-parameter)
target_link_libraries(winddroid log m jnigraphics)
