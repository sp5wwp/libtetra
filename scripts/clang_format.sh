#!/usr/bin/env bash
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Format (default) or check (--check) the source files listed below.
# Add new or modified files to the list.

set -e
cd "$(dirname "$0")/.."

FILES=(
    include/tetra/tetra.h
    include/tetra/tetra_coding.h
    include/tetra/tetra_common.h
    include/tetra/tetra_config.h
    include/tetra/tetra_dmo.h
    include/tetra/tetra_sds.h
    include/tetra/tetra_speech.h
    include/tetra/tetra_tmo.h
    include/tetra/tetra_tx.h
    src/common.c
    src/dmo_int.h
    src/dmo_rx.c
    src/tmo_int.h
    src/tmo_rx.c
    src/l1/burst.c
    src/l1/l1.c
    src/l1/l1.h
    src/l2/coding.c
    src/l2/conv.c
    src/l2/dmo_mac.c
    src/l2/lmac.c
    src/l2/lmac.h
    src/l2/speech.c
    src/l2/tmo_mac.c
    src/l3/dmo_cc.c
    src/l3/sds.c
    src/l3/tmo_l3.c
    src/l3/tmo_mle.c
    src/util/bits.c
    src/util/bits.h
    tests/unit/test_coding.cpp
    tests/unit/test_dmo.cpp
    tests/unit/test_sds.cpp
    tests/unit/test_speech.cpp
    tests/unit/test_tmo.cpp
    tests/unit/test_tmo_l3.cpp
    tools/dmo_scenario.c
    tools/dmo_scenario.h
    tools/tetra_demod.c
    tools/tetra_gen.c
    tools/tetra_rx.c
)

if [ "$1" == "--check" ]; then
    clang-format --dry-run --Werror "${FILES[@]}"
else
    clang-format -i "${FILES[@]}"
fi
