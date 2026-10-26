# SPDX-License-Identifier: GPL-3.0-or-later
#
# Toolchain file for Cortex-M33 with FPU.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_AR arm-none-eabi-ar)

set(TETRA_CPU_FLAGS "-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16")
set(CMAKE_C_FLAGS_INIT "${TETRA_CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${TETRA_CPU_FLAGS}")
set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "")
