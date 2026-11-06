// SPDX-License-Identifier: GPL-3.0-or-later
// Source of DYNAMIC_FPS_ROUTINE in src/core/file_sys/patch_manager.cpp (Ocarina of Time, main+0x3c46200).
// Build: llvm-mc -triple=aarch64 -filetype=obj -o r.o dynamic_fps_steady.s && llvm-objcopy -O binary --only-section=.text r.o r.bin
// At 0x3c46200, called in place of "fdiv s8, s1, s0" (the per-frame step: s1 / s0, where s0 is
// steps per second). The time since the last frame (capped at 50 ms) plus the carried remainder
// advances the game a whole number of steps (at least 1); the remainder carries to the next frame.
// Uses only the registers and the 16 bytes of state Dark's routine uses.
    fdiv    s8, s1, s0              // one step
    .inst   0xb00092ca              // adrp x10, state page (same bytes as Dark's routine)
    .inst   0x9138014a              // add  x10, x10, #0xe00
    mrs     x11, CNTPCT_EL0
    ldr     x12, [x10]              // last counter
    str     x11, [x10]
    cbz     x12, done               // first frame: one step
    sub     x11, x11, x12           // ticks since the last frame
    mrs     x13, CNTFRQ_EL0
    mov     x14, #20
    udiv    x14, x13, x14           // 50 ms
    cmp     x11, x14
    csel    x11, x14, x11, hi
    ldr     x12, [x10, #8]          // carried remainder
    add     x11, x11, x12
    ucvtf   s2, x13
    fdiv    s2, s2, s0
    fcvtzu  x13, s2                 // ticks per step
    cbz     x13, done
    udiv    x14, x11, x13           // whole steps
    cmp     x14, #1
    csinc   x14, x14, xzr, hs       // at least one
    msub    x12, x14, x13, x11      // remainder (negative when one step was forced)
    cmp     x12, #0
    csel    x12, xzr, x12, lt       // no debt above 60 fps
    str     x12, [x10, #8]
    ucvtf   s2, x14
    fmul    s8, s8, s2
done:
    ret
