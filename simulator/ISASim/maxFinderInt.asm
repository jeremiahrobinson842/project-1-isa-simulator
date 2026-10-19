# maxFinderInt: stores the largest signed integer in A[0..N-1] at MAX.
# If N <= 0, nothing is stored.
#
#   0x000  N
#   0x004  MAX
#   0x008  A[0] ... A[N-1]
#
# R1 = N, R2 = element address, R3 = max, R5 = A[i], R6 = end address
#
# "N:" is the instruction address. Branch operands are offsets from the
# next instruction. J and JAL operands are instruction addresses.

    LW    R1, 0(R0)           #   0: R1 = N
    BGE   R0, R1, 12          #   1: N <= 0  [DONE = address 14]
    ADDI  R2, R0, 8           #   2: R2 = address of A[0]
    ADDI  R7, R0, 2           #   3:
    SLL   R6, R1, R7          #   4: R6 = 4N
    ADD   R6, R6, R2          #   5: R6 = address of A[N]
    LW    R3, 0(R2)           #   6: max = A[0]
# LOOP = address 7
    ADDI  R2, R2, 4           #   7: next element
    BGEU  R2, R6, 4           #   8: end of array  [STORE = address 13]
    LW    R5, 0(R2)           #   9: R5 = A[i]
    BGE   R3, R5, -4          #  10: max >= A[i]  [LOOP = address 7]
    ADDI  R3, R5, 0           #  11: max = A[i]
    J     7                   #  12: [LOOP = address 7]
# STORE = address 13
    SW    R3, 4(R0)           #  13: MAX = max
# DONE = address 14
    HALT                      #  14:
