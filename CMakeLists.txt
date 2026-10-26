# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(libtetra VERSION 0.1.0 LANGUAGES C)

if(CMAKE_CROSSCOMPILING)
    set(TETRA_HOST_DEFAULT OFF)
else()
    set(TETRA_HOST_DEFAULT ON)
endif()

option(TETRA_BUILD_TOOLS "Build host tools" ${TETRA_HOST_DEFAULT})
option(TETRA_BUILD_TESTS "Build unit tests (Catch2)" ${TETRA_HOST_DEFAULT})

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(tetra STATIC
    src/common.c
    src/dmo_rx.c
    src/l1/burst.c
    src/l1/l1.c
    src/l2/coding.c
    src/l2/conv.c
    src/l2/dmo_mac.c
    src/l2/lmac.c
    src/l2/speech.c
    src/l2/tmo_mac.c
    src/l3/dmo_cc.c
    src/l3/lip.c
    src/l3/sds.c
    src/l3/tmo_l3.c
    src/l3/tmo_mle.c
    src/tmo_rx.c
    src/util/bits.c
)

set_target_properties(tetra PROPERTIES
    C_STANDARD 17
    C_EXTENSIONS ON
    OUTPUT_NAME tetra
)

target_include_directories(tetra
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(tetra PRIVATE
    -Wall -Wextra -ffunction-sections -fdata-sections
)

install(TARGETS tetra ARCHIVE DESTINATION lib)
install(DIRECTORY include/tetra DESTINATION include)

if(TETRA_BUILD_TOOLS OR TETRA_BUILD_TESTS)
    # Synthetic DMO traffic, shared by tetra_gen and the tests.
    add_library(dmo_scenario STATIC tools/dmo_scenario.c)
    set_target_properties(dmo_scenario PROPERTIES C_STANDARD 17
                                                  C_EXTENSIONS ON)
    target_include_directories(dmo_scenario PRIVATE src)
    target_link_libraries(dmo_scenario PUBLIC tetra)
endif()

if(TETRA_BUILD_TOOLS)
    foreach(tool tetra_demod tetra_rx tetra_gen)
        add_executable(${tool} tools/${tool}.c)
        set_target_properties(${tool} PROPERTIES C_STANDARD 17
                                                 C_EXTENSIONS ON)
        target_compile_options(${tool} PRIVATE -Wall -Wextra)
    endforeach()
    target_link_libraries(tetra_demod PRIVATE m)
    target_link_libraries(tetra_rx PRIVATE tetra)
    target_link_libraries(tetra_gen PRIVATE dmo_scenario m)
endif()

if(TETRA_BUILD_TESTS)
    enable_language(CXX)
    enable_testing()
    find_package(Catch2 3 REQUIRED)

    set(TETRA_TESTS coding dmo sds speech tmo tmo_l3)
    foreach(t ${TETRA_TESTS})
        add_executable(test_${t} tests/unit/test_${t}.cpp)
        set_target_properties(test_${t} PROPERTIES CXX_STANDARD 14
                                                   CXX_EXTENSIONS OFF)
        target_include_directories(test_${t} PRIVATE src)
        target_compile_definitions(test_${t} PRIVATE
            TETRA_TEST_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
        target_link_libraries(test_${t} PRIVATE
            tetra dmo_scenario Catch2::Catch2WithMain)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
endif()
