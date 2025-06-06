// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0 OR ISC OR MIT-0

// ----------------------------------------------------------------------------
// Reduce modulo basepoint order, z := x mod n_25519
// Input x[k]; output z[4]
//
//    extern void bignum_mod_n25519
//     (uint64_t z[static 4], uint64_t k, uint64_t *x);
//
// Reduction is modulo the order of the curve25519/edwards25519 basepoint,
// which is n_25519 = 2^252 + 27742317777372353535851937790883648493
//
// Standard x86-64 ABI: RDI = z, RSI = k, RDX = x
// Microsoft x64 ABI:   RCX = z, RDX = k, R8 = x
// ----------------------------------------------------------------------------

#include "_internal_s2n_bignum.h"


        S2N_BN_SYM_VISIBILITY_DIRECTIVE(bignum_mod_n25519)
        S2N_BN_SYM_PRIVACY_DIRECTIVE(bignum_mod_n25519)
        .text

#define z %rdi
#define k %rsi
#define x %rcx

#define m0 %r8
#define m1 %r9
#define m2 %r10
#define m3 %r11
#define d %r12

#define q %rbx

S2N_BN_SYMBOL(bignum_mod_n25519):

#if WINDOWS_ABI
        pushq   %rdi
        pushq   %rsi
        movq    %rcx, %rdi
        movq    %rdx, %rsi
        movq    %r8, %rdx
#endif

// Save extra registers

        pushq   %rbx
        pushq   %rbp
        pushq   %r12

// If the input is already <= 3 words long, go to a trivial "copy" path

        cmpq    $4, k
        jc      shortinput

// Otherwise load the top 4 digits (top-down) and reduce k by 4
// This [m3;m2;m1;m0] is the initial x where we begin reduction.

        subq    $4, k
        movq    24(%rdx,k,8), m3
        movq    16(%rdx,k,8), m2
        movq    8(%rdx,k,8), m1
        movq    (%rdx,k,8), m0

// Move x into another register to leave %rdx free for multiplies

        movq    %rdx, x

// Get the quotient estimate q = floor(x/2^252).
// Also delete it from m3, in effect doing x' = x - q * 2^252

        movq    m3, q
        shrq    $60, q

        shlq    $4, m3
        shrq    $4, m3

// Let [%rdx;d;%rbp] = q * (n_25519 - 2^252)

        movq    $0x5812631a5cf5d3ed, %rax
        mulq    q
        movq    %rax, %rbp
        movq    %rdx, d

        movq    $0x14def9dea2f79cd6, %rax
        mulq    q
        addq    %rax, d
        adcq    $0, %rdx

// Subtract to get x' - q * (n_25519 - 2^252) = x - q * n_25519

        subq    %rbp, m0
        sbbq    d, m1
        sbbq    %rdx, m2
        sbbq    $0, m3

// Get a bitmask for the borrow and create a masked version of
// non-trivial digits of [%rbx;0;%rdx;%rax] = n_25519, then add it.
// The masked n3 digit exploits the fact that bit 60 of n0 is set.

        sbbq    %rbx, %rbx

        movq    $0x5812631a5cf5d3ed, %rax
        andq    %rbx, %rax
        movq    $0x14def9dea2f79cd6, %rdx
        andq    %rbx, %rdx
        movq    $0x1000000000000000, %rbx
        andq    %rax, %rbx

        addq    %rax, m0
        adcq    %rdx, m1
        adcq    $0, m2
        adcq    %rbx, m3

// Now do (k-4) iterations of 5->4 word modular reduction. Each one
// is similar to the sequence above except for the more refined quotient
// estimation process.

        testq   k, k
        jz      writeback

bignum_mod_n25519_loop:

// Assume that the new 5-digit x is 2^64 * previous_x + next_digit.
// Get the quotient estimate q = max (floor(x/2^252)) (2^64 - 1)
// and first compute x' = x - 2^252 * q.

        movq    m3, q
        shldq   $4, m2, q
        shrq    $60, m3
        subq    m3, q
        shlq    $4, m2
        shrdq   $4, m3, m2

// Let [%rdx;m3;%rbp] = q * (n_25519 - 2^252)

        movq    $0x5812631a5cf5d3ed, %rax
        mulq    q
        movq    %rax, %rbp
        movq    %rdx, m3

        movq    $0x14def9dea2f79cd6, %rax
        mulq    q
        addq    %rax, m3
        adcq    $0, %rdx

// Load the next digit

        movq    -8(x,k,8), d

// Subtract to get x' - q * (n_25519 - 2^252) = x - q * n_25519

        subq    %rbp, d
        sbbq    m3, m0
        sbbq    %rdx, m1
        sbbq    $0, m2

// Get a bitmask for the borrow and create a masked version of
// non-trivial digits of [%rbx;0;%rdx;%rax] = n_25519, then add it.
// The masked n3 digit exploits the fact that bit 60 of n0 is set.

        sbbq    %rbx, %rbx

        movq    $0x5812631a5cf5d3ed, %rax
        andq    %rbx, %rax
        movq    $0x14def9dea2f79cd6, %rdx
        andq    %rbx, %rdx
        movq    $0x1000000000000000, %rbx
        andq    %rax, %rbx

        addq    %rax, d
        adcq    %rdx, m0
        adcq    $0, m1
        adcq    %rbx, m2

// Now shuffle registers up and loop

        movq    m2, m3
        movq    m1, m2
        movq    m0, m1
        movq    d, m0

        decq    k
        jnz     bignum_mod_n25519_loop

// Write back

writeback:

        movq    m0, (z)
        movq    m1, 8(z)
        movq    m2, 16(z)
        movq    m3, 24(z)

// Restore registers and return

        popq    %r12
        popq    %rbp
        popq    %rbx
#if WINDOWS_ABI
        popq   %rsi
        popq   %rdi
#endif
        ret

shortinput:

        xorq    m0, m0
        xorq    m1, m1
        xorq    m2, m2
        xorq    m3, m3

        testq   k, k
        jz      writeback
        movq    (%rdx), m0
        decq    k
        jz      writeback
        movq    8(%rdx), m1
        decq    k
        jz      writeback
        movq    16(%rdx), m2
        jmp     writeback

#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif
