# Project 1: Instruction Level Simulator (isaSim)

* Authors: Cheikh Lame, Mira Allis, Randy Bauer, Ahmad Rao, Jeremiah Robinson
* Class: Computer Architecture
* Semester: Fall 2026

## Overview

isaSim simulates our team's 32-bit ISA one instruction at a time. It reads a benchmark written in our
assembly language and a memory file giving the starting contents of data
memory. It runs the benchmark either straight through (`-r`) or step by step
(`-s`). At HALT it prints how many times each instruction ran, writes those
counts to a `.instrCount` file, and writes the final data memory to a
`.mem.out` file.

## Compiling and Using

isaSim is a single C99 source file with no dependencies beyond the standard
C library. To build it on onyx, run this in this directory:

    make

That produces `./isaSim`. If `make` isn't available, use
`gcc -std=c99 -O2 -o isaSim isaSim.c -lm`.

Usage:

    ./isaSim -<r|s> <assembly file> <memory file>

    ./isaSim -r testAll.asm testAll.mem              # run straight through
    ./isaSim -s maxFinderInt.asm maxFinderInt.mem    # step by step

**Run mode (`-r`)** runs the benchmark to HALT. It then prints the number of
times each instruction executed (all 36, including the ones that ran 0
times) and the total, and writes the output files.

**Step mode (`-s`)** displays the machine state before the first instruction
and after every step:

- the number of instructions executed so far
- the instruction just executed and the next one, with each one's address,
  32-bit machine word, and assembly. A branch also shows whether it was taken,
  and a store shows the memory word it wrote.
- PC
- R0-R15 in hex and signed decimal, and F0-F15 in hex and as a decimal value.
  A `*` marks every register the last instruction changed.

At the `>` prompt:

| Input | Action |
| --- | --- |
| Enter | Execute the next instruction |
| A number N | Execute N instructions, then display the state |
| `r` | Run to HALT |
| `q` | Quit without writing output files |

At HALT, step mode prints the counts and writes the same files as run mode.
If input ends (for example, when it is piped from a file), the simulator runs
to HALT.

**Output files.** Both files go in the current directory and are named after
the **memory file**:

| Memory file | Counts | Final memory |
| --- | --- | --- |
| `maxFinderInt.mem` | `maxFinderInt.instrCount` | `maxFinderInt.mem.out` |
| `maxFinderIntNeg.mem` | `maxFinderIntNeg.instrCount` | `maxFinderIntNeg.mem.out` |

The MAXFINDER programs run on two data sets each, and naming the outputs after
the memory file keeps them separate. It also puts each `.mem.out` next to the
`.mem.expected` file it should match. To check a run:

    ./isaSim -r maxFinderInt.asm maxFinderIntNeg.mem
    diff maxFinderIntNeg.mem.out maxFinderIntNeg.mem.expected

`make test` runs all five benchmark runs and compares each `.mem.out` with its
`.mem.expected`. `make clean` removes `isaSim` and every output file.

**Errors.** Each error message names the file and line, or the PC and
instruction, then the simulator exits with status 1. If the error happens
while the program runs, no output files are written. The simulator reports:

- a mistake in the assembly or memory file
- a load or store address that is not a multiple of 4, or outside data memory
- a branch or jump to an address outside the program
- running past the last instruction without a HALT
- more than 100,000,000 instructions executed (an infinite loop)

## Files

| File | Contents |
| --- | --- |
| `isaSim.c` | The simulator |
| `Makefile` | `make`, `make test`, `make clean` |
| `testAll.asm`, `testAll.mem`, `testAll.mem.expected` | Testing benchmark (uses all 36 instructions) |
| `maxFinderInt.asm`, `maxFinderInt.mem`, `maxFinderIntNeg.mem` (+ `.expected`) | Integer MAXFINDER and its two data sets |
| `maxFinderFP.asm`, `maxFinderFP.mem`, `maxFinderFPNeg.mem` (+ `.expected`) | Floating-point MAXFINDER and its two data sets |

## How the simulator works

`isaSim.c` follows the hardware closely:

1. **Assemble.** Each line of the assembly file is encoded into its 32-bit
   machine word using the R, I, J and HALT formats in our specification. This
   is instruction memory, and the first instruction is at address 0.
2. **Load memory.** The memory file fills a 2^28-byte data memory. Memory is
   stored in 4 KB pages that are allocated only when a non-zero byte is
   written, so the full 256 MB is never allocated.
3. **Fetch, decode, execute.** Each step fetches the word at PC, decodes its
   opcode, sub-opcode and fields, and carries out that instruction's RTL. PC
   becomes PC + 1 unless the instruction assigns PC. A word with a reserved
   opcode or sub-opcode, or with a non-zero unused field, is rejected.
4. **Finish.** At HALT the counts and the final memory are written.

### Conventions this simulator relies on

- **Assembly file:** one instruction per line, and `#` starts a comment.
  Mnemonics and operand order are as in the specification, for example
  `ADD rd, rs, rt`, `LW rt, offset(rs)` and `BEQ rs, rt, offset`.
  - Mnemonics and register names may be upper or lower case.
  - Numbers are decimal or `0x` hex.
- **No labels:** the handout says to replace them with numbers.
  - A branch operand is an offset counted from the next instruction, as in
    the specification's RTL `PC <- PC + 1 + SignExt(offset)`.
    `BGE R3, R5, -4` at address 10 branches to address 7.
  - A `J` or `JAL` operand is an absolute instruction address.
- **Memory file:**
  - Line 1 is a comment, and it is copied to the `.mem.out` file.
  - Line 2 must be `28,8`: 28 address bits, 8 bits at each address.
  - Every other line is `address,contents` in hex.
  - The `.mem.out` file uses the same format. It lists every 32-bit word that
    holds a non-zero byte, all 4 of its bytes, most significant byte first,
    in address order.
- **Floating point:** IEEE-754 single precision with round-to-nearest-even.
  - Dividing by zero gives an infinity or NaN and does not stop the program.
  - Any NaN that FADD, FSUB, FMUL or FDIV produces is stored as `0x7FC00000`,
    because IEEE-754 doesn't fix a NaN's bit pattern. FLW, FSW and FMOV copy
    all 32 bits unchanged.

## Testing

`make test` output:

    PASS  testAll.asm with testAll.mem
    PASS  maxFinderInt.asm with maxFinderInt.mem
    PASS  maxFinderInt.asm with maxFinderIntNeg.mem
    PASS  maxFinderFP.asm with maxFinderFP.mem
    PASS  maxFinderFP.asm with maxFinderFPNeg.mem

| Run | Instructions executed | Result |
| --- | --- | --- |
| testAll | 127 | All 36 instructions; matches `testAll.mem.expected` |
| maxFinderInt / maxFinderInt.mem | 59 | MAX = 7341 (0x00001CAD) |
| maxFinderInt / maxFinderIntNeg.mem | 35 | MAX = -12 (0xFFFFFFF4) |
| maxFinderFP / maxFinderFP.mem | 58 | MAX = 101.125 (0x42CA4000) |
| maxFinderFP / maxFinderFPNeg.mem | 31 | MAX = -0.25 (0xBE800000) |

These totals and the count for each instruction match the reference model our
team used to build the benchmarks (`isaref.py`). We also tested:

- **Random programs:** 3,000 random programs, which between them use all 36
  instructions. Each was compared with the reference model on final memory,
  every register, and the count for each instruction, and they all match. As a check on the test itself,
  bugs deliberately put into the simulator (wrong SRA, FLT acting like <=, a
  sign-extended ANDI, a signed BLTU, a 4-bit shift amount) were each caught.
- **Encodings:** the encoding examples in our specification come out exactly
  as listed: `ADD R3, R1, R2` = 0x01230000, `BLT R2, R3, -4` = 0x4232FFFC,
  `FLT R5, F3, F4` = 0xA3454000.
- **Step mode:** stepping one at a time, N at a time, with `r`, and with input
  ending all produce the same output files as run mode.
- **Error cases:** every error listed above gives a message, exit status 1,
  and no output files.
- **Build:** compiles with no warnings under
  `gcc -std=c99 -Wall -Wextra -pedantic`, and runs clean under the address and
  undefined-behavior sanitizers.
