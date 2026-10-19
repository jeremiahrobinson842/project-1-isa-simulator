# maxFinderFP: stores the largest single-precision value in A[0..N-1] at MAX.
# If N <= 0, nothing is stored.
#
#   0x000  N (integer)
#   0x004  MAX
#   0x008  A[0] ... A[N-1]
#
# R1 = N, R2 = element address, R5 = compare result, R6 = end address,
# F1 = max, F2 = A[i]
#
# "N:" is the instruction address. Branch operands are offsets from the
# next instruction. J and JAL operands are instruction addresses.

    LW    R1, 0(R0)           #   0: R1 = N
    BGE   R0, R1, 13          #   1: N <= 0  [DONE = address 15]
    ADDI  R2, R0, 8           #   2: R2 = address of A[0]
    ADDI  R7, R0, 2           #   3:
    SLL   R6, R1, R7          #   4: R6 = 4N
    ADD   R6, R6, R2          #   5: R6 = address of A[N]
    FLW   F1, 0(R2)           #   6: max = A[0]
# LOOP = address 7
    ADDI  R2, R2, 4           #   7: next element
    BGEU  R2, R6, 5           #   8: end of array  [STORE = address 14]
    FLW   F2, 0(R2)           #   9: F2 = A[i]
    FLT   R5, F1, F2          #  10: R5 = (max < A[i])
    BEQ   R5, R0, -5          #  11: max >= A[i]  [LOOP = address 7]
    FMOV  F1, F2              #  12: max = A[i]
    J     7                   #  13: [LOOP = address 7]
# STORE = address 14
    FSW   F1, 4(R0)           #  14: MAX = max
# DONE = address 15
    HALT                      #  15:
