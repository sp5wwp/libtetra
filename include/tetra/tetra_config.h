/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_CONFIG_H
#define TETRA_CONFIG_H

/*
 * Fallback defaults for direct source-tree consumers. CMake builds use the
 * generated header instead. For ABI-affecting settings, configure CMake cache
 * options rather than overriding these macros on the compiler command line.
 */
#if defined(TETRA_ENABLE_CRYPTO) && TETRA_ENABLE_CRYPTO != 0
#error "TETRA_ENABLE_CRYPTO must be configured through CMake"
#endif
#undef TETRA_ENABLE_CRYPTO
#define TETRA_ENABLE_CRYPTO 0

#if defined(TETRA_TMO_MAX_SDU_BITS) && TETRA_TMO_MAX_SDU_BITS != 2632
#error "TETRA_TMO_MAX_SDU_BITS must be configured through CMake"
#endif
#undef TETRA_TMO_MAX_SDU_BITS
#define TETRA_TMO_MAX_SDU_BITS 2632

#if defined(TETRA_DMO_MAX_SDU_BITS) && TETRA_DMO_MAX_SDU_BITS != 2632
#error "TETRA_DMO_MAX_SDU_BITS must be configured through CMake"
#endif
#undef TETRA_DMO_MAX_SDU_BITS
#define TETRA_DMO_MAX_SDU_BITS 2632

#if defined(TETRA_TMO_MAX_CALLS) && TETRA_TMO_MAX_CALLS != 8
#error "TETRA_TMO_MAX_CALLS must be configured through CMake"
#endif
#undef TETRA_TMO_MAX_CALLS
#define TETRA_TMO_MAX_CALLS 8

#if defined(TETRA_SYNC_MAX_ERRORS) && TETRA_SYNC_MAX_ERRORS != 4
#error "TETRA_SYNC_MAX_ERRORS must be configured through CMake"
#endif
#undef TETRA_SYNC_MAX_ERRORS
#define TETRA_SYNC_MAX_ERRORS 4

#if defined(TETRA_UPLINK_DETECT) && TETRA_UPLINK_DETECT != 1
#error "TETRA_UPLINK_DETECT must be configured through CMake"
#endif
#undef TETRA_UPLINK_DETECT
#define TETRA_UPLINK_DETECT 1

#endif /* TETRA_CONFIG_H */
