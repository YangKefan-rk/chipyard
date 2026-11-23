.option norvc
.option norelax
.section .text.init, "ax"
.globl _start, terminal_fence, success, alias_pte, old_load, new_load, update_pte
.equ ALIAS_VA, 0x40000000
.equ LEAF_FLAGS, 0xef  # V/R/W/X/G/A/D, supervisor only

# Build a descriptor from the linked physical address of a page.
.macro pte dest, label, flags
    la \dest, \label
    srli \dest, \dest, 12
    slli \dest, \dest, 10
    ori \dest, \dest, \flags
.endm

_start:
    bnez a0, wait_setup
    la t0, root_table
    pte t1, l1_table, 1
    sd t1, 8(t0)                    # VPN[2]=1, alias range
    pte t1, _start, LEAF_FLAGS
    sd t1, 16(t0)                   # VPN[2]=2, identity gigapage
    la t0, l1_table
    pte t1, l0_table, 1
    sd t1, 0(t0)
    la t0, alias_pte
    pte t1, old_page, LEAF_FLAGS
    sd t1, 0(t0)
    fence rw, rw
    la t0, setup_done
    li t1, 1
    sd t1, 0(t0)
wait_setup:
    la t0, setup_done
1:  ld t1, 0(t0)
    beqz t1, 1b
    fence rw, rw

    li t0, 0x24000000              # TOR upper bound: 0x90000000
    csrw pmpaddr0, t0
    li t0, 15                      # TOR, RWX
    csrw pmpcfg0, t0
    la t0, root_table
    srli t0, t0, 12
    li t1, 0x8000000000000000
    or t0, t0, t1
    csrw satp, t0                   # MODE=Sv39, ASID=0
    la t0, supervisor
    csrw mepc, t0
    li t0, 0x800                   # MPP=S, MPRV/MIE=0
    csrw mstatus, t0
    sfence.vma x0, x0
    mret

supervisor:
    li t2, ALIAS_VA
old_load:
    ld t3, 0(t2)
    li t4, 0x31
    bne t3, t4, parked

    fence rw, rw
    la t0, old_seen
    slli t1, a0, 6
    add t1, t1, t0
    li t3, 1
    sd t3, 0(t1)
    bnez a0, wait_update
    li t4, 4
2:  ld t3, 0(t0)
    beqz t3, 2b
    addi t0, t0, 64
    addi t4, t4, -1
    bnez t4, 2b
    fence rw, rw
    la t0, alias_pte
    pte t1, new_page, LEAF_FLAGS
update_pte:
    sd t1, 0(t0)
    fence rw, rw
    sfence.vma x0, x0
    la t0, update_done
    li t1, 1
    sd t1, 0(t0)
wait_update:
    la t0, update_done
3:  ld t1, 0(t0)
    beqz t1, 3b
    fence rw, rw
    sfence.vma x0, x0
new_load:
    ld t3, 0(t2)
    li t4, 0x72
    bne t3, t4, parked
    bnez a0, terminal_fence
    la t0, success
    li t1, 1
    sd t1, 0(t0)
terminal_fence:
    fence.i
parked:
    wfi
4:  j 4b

.section .data, "aw"
.balign 4096
root_table: .zero 4096
l1_table: .zero 4096
l0_table:
alias_pte: .zero 4096
old_page: .dword 0x31
.balign 4096
new_page: .dword 0x72
.balign 64
setup_done: .dword 0
.balign 64
old_seen: .zero 256
.balign 64
update_done: .dword 0
.balign 64
success: .dword 0
