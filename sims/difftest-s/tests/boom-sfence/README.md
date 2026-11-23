# BOOM SFENCE.VMA regression

Run both fixtures against a four-hart `QuadSmallBoomV4Ldq16Mshr8DifftestSConfig`
simulator with the native Difftest-S session and BOOM Spike reference enabled:

```sh
bash run.sh /path/to/simulator /path/to/results
```

Use the [BOOM build instructions](../../README.md#build-a-complete-session)
with this configuration, adding
`EXTRA_SIM_PREPROC_DEFINES=+define+RANDOM=0` to initialize the otherwise
unspecified Bare SATP PPN consistently in the Verilator test. The runner
requires Bash, Python 3, and the `riscv64-unknown-elf-` GCC/binutils toolchain
(`CROSS_COMPILE` overrides the prefix). All generated ELFs, disassemblies,
hashes, logs, and structured verdicts go to the output directory.

Bare checks one SFENCE.VMA retirement per hart. Sv39 remaps a shared leaf PTE
at runtime. It checks the old and new values at one virtual address on all
four harts, the Spike store trace for the remap, and SFENCE.VMA counts
`[3, 2, 2, 2]`. Both fixtures require all ordinary stores audited, all
accepted loads checked, and eight drained producers with no pending work.

The Sv39 program supplies the ordering and each hart executes its own
SFENCE.VMA. The test does not check missing fences or TLB internals.
FENCE.I is only the terminal retirement marker; the program does not modify
instruction bytes.
