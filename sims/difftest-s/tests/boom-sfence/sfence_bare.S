.option norvc
.option norelax
.section .text.init, "ax"
.globl _start, sfence_test, terminal_fence, parked, success
_start:
    csrw mstatus, zero
    la t0, scratch
    slli t1, a0, 6
    add t0, t0, t1
    addi t2, a0, 42
sfence_test:
    sfence.vma x0, x0
    sd t2, 0(t0)
    ld t3, 0(t0)
    bne t2, t3, parked
    bnez a0, terminal_fence
    la t0, success
    addi t1, zero, 1
    sd t1, 0(t0)
terminal_fence:
    fence.i
parked:
    wfi
1:  j 1b

.section .data, "aw"
.balign 64
scratch:
    .zero 256
success:
    .dword 0
