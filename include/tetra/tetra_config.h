/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_CONFIG_H
#define TETRA_CONFIG_H

/*
 * Compile-time sizing. Override with -D on the compiler command line.
 * All memory is statically sized inside the receiver contexts.
 */

/** Largest reassembled TMO TM-SDU in bits (N.202 is 2632). */
#ifndef TETRA_TMO_MAX_SDU_BITS
#define TETRA_TMO_MAX_SDU_BITS 2632
#endif

/** Largest reassembled DMO DM-SDU in bits. */
#ifndef TETRA_DMO_MAX_SDU_BITS
#define TETRA_DMO_MAX_SDU_BITS 2632
#endif

/** Number of TMO calls tracked at the same time. */
#ifndef TETRA_TMO_MAX_CALLS
#define TETRA_TMO_MAX_CALLS 8
#endif

/** Hard bit errors tolerated when detecting the 38-bit sync sequence. */
#ifndef TETRA_SYNC_MAX_ERRORS
#define TETRA_SYNC_MAX_ERRORS 4
#endif

#endif /* TETRA_CONFIG_H */
