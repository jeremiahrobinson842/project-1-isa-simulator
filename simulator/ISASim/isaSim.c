/*
 * isaSim.c - instruction level simulator for our Project 1 ISA
 *
 * Cheikh Lame, Mira Allis, Randy Bauer, Ahmad Rao, Jeremiah Robinson
 * Computer Architecture, Fall 2026
 *
 * Usage:  isaSim -<r|s> <assembly file> <memory file>
 *
 *     -r   run the benchmark straight through to HALT
 *     -s   step through the benchmark one instruction at a time
 *
 * How it works. The simulator first assembles every line of the assembly
 * file into its 32-bit machine word, using the formats in our ISA
 * specification. It then runs the machine's fetch-decode-execute loop on
 * those words: fetch the word at PC, decode its fields, carry out the
 * instruction's RTL, and update PC. When HALT executes it prints how many
 * times each instruction ran and writes two files to the current directory:
 *
 *     <name>.instrCount   the instruction counts
 *     <name>.mem.out      the final data memory, in the .mem file format
 *
 * <name> is the memory file's name without ".mem", so maxFinderIntNeg.mem
 * gives maxFinderIntNeg.mem.out, which is compared against
 * maxFinderIntNeg.mem.expected.
 *
 * The machine (see the ISA specification for every instruction's RTL):
 *   - Every instruction is 32 bits. Instruction memory holds one instruction
 *     per address, so PC counts instructions; the first one is at address 0.
 *   - Integer registers R0-R15. R0 always reads 0 and writes to it are
 *     ignored. JAL saves its return address in R15.
 *   - Floating-point registers F0-F15, each a 32-bit IEEE-754 single.
 *   - Data memory is 2^28 bytes, byte addressable. A 32-bit word occupies 4
 *     bytes, most significant byte first (big-endian), at an address that
 *     is a multiple of 4.
 *
 * Build with the Makefile (make), or: gcc -std=c99 -O2 -o isaSim isaSim.c -lm
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * Machine parameters
 * ====================================================================== */

#define ADDR_BITS     28                  /* data memory address bits */
#define CELL_BITS     8                   /* bits stored at each address */
#define MEM_SIZE      (1UL << ADDR_BITS)  /* data memory size in bytes */
#define NUM_REGS      16                  /* per register file (R and F) */
#define LINK_REG      15                  /* JAL saves the return address here */

/* Every NaN produced by FP arithmetic is stored as this quiet NaN. IEEE-754
 * does not fix a NaN's bit pattern, so this keeps results the same on every
 * host. FLW, FSW and FMOV still copy all 32 bits unchanged. */
#define CANONICAL_NAN 0x7FC00000u

/* A program that runs this many instructions without reaching HALT is
 * assumed to be stuck in a loop, and the simulator stops it. */
#define MAX_INSTRUCTIONS 100000000ULL

#define MAX_LINE 4096                     /* longest line accepted in a file */

/* Floats are moved in and out of registers as raw bits with memcpy, which
 * needs a 32-bit float. This line fails to compile if that is not true. */
typedef char float_must_be_32_bits[sizeof(float) == 4 ? 1 : -1];

/* ======================================================================
 * The instruction set
 * ====================================================================== */

/* Opcodes (bits 31-28). 1011, 1101 and 1110 are reserved. */
enum {
    OP_ALU_R = 0x0, OP_ALU_I = 0x1, OP_LW = 0x2, OP_SW = 0x3,
    OP_BRANCH = 0x4, OP_J = 0x5, OP_JAL = 0x6, OP_JR = 0x7,
    OP_FLW = 0x8, OP_FSW = 0x9, OP_FP = 0xA, OP_LUI = 0xC, OP_HALT = 0xF
};

/*
 * The four formats (bit positions):
 *
 *   R:    opcode 31-28 | rs 27-24 | rt 23-20 | rd 19-16 | sub 15-12 | unused 11-0
 *   I:    opcode 31-28 | rs 27-24 | rt 23-20 | sub 19-16 | immediate 15-0
 *   J:    opcode 31-28 | address 27-0
 *   HALT: opcode 31-28 | unused 27-0
 *
 * An operand form says how an instruction is written in assembly and which
 * fields its operands go into.
 */
typedef enum {
    FORM_RRR,    /* rd, rs, rt      R-format, integer registers (ADD ... SRA) */
    FORM_FFF,    /* fd, fs, ft      R-format, FP registers (FADD ... FDIV)    */
    FORM_RFF,    /* rd, fs, ft      R-format, FP compare into R[rd] (FLT, FEQ) */
    FORM_FF,     /* fd, fs          R-format, rt unused (FMOV)                */
    FORM_R,      /* rs              R-format, rt and rd unused (JR)           */
    FORM_RRI,    /* rt, rs, imm     I-format, signed immediate (ADDI)         */
    FORM_RRU,    /* rt, rs, imm     I-format, unsigned immediate (ANDI ...)   */
    FORM_RU,     /* rt, imm         I-format, rs unused (LUI)                 */
    FORM_RM,     /* rt, offset(rs)  I-format, integer load/store (LW, SW)     */
    FORM_FM,     /* ft, offset(rs)  I-format, FP load/store (FLW, FSW)        */
    FORM_RRB,    /* rs, rt, offset  I-format, branches                        */
    FORM_ADDR,   /* address         J-format (J, JAL)                         */
    FORM_NONE    /* (no operands)   HALT                                      */
} Form;

/* Every instruction, in the order the instruction counts are printed. */
typedef enum {
    I_ADD, I_SUB, I_AND, I_OR, I_XOR, I_SLT, I_SLTU, I_SLL, I_SRL, I_SRA,
    I_ADDI, I_ANDI, I_ORI, I_XORI,
    I_LW, I_SW,
    I_BEQ, I_BNE, I_BLT, I_BGE, I_BLTU, I_BGEU,
    I_J, I_JAL, I_JR,
    I_FLW, I_FSW,
    I_FADD, I_FSUB, I_FMUL, I_FDIV, I_FLT, I_FEQ, I_FMOV,
    I_LUI, I_HALT,
    NUM_INSTRS
} InstrId;

#define NO_SUB (-1)              /* J, JAL and HALT have no sub-opcode field */

typedef struct {
    const char *name;            /* mnemonic */
    unsigned opcode;             /* bits 31-28 */
    int sub;                     /* sub-opcode, or NO_SUB */
    Form form;
} InstrInfo;

static const InstrInfo ISA[NUM_INSTRS] = {
    /* ALU-R (opcode 0000, R-format) */
    [I_ADD]  = {"ADD",  OP_ALU_R,  0x0, FORM_RRR},
    [I_SUB]  = {"SUB",  OP_ALU_R,  0x1, FORM_RRR},
    [I_AND]  = {"AND",  OP_ALU_R,  0x2, FORM_RRR},
    [I_OR]   = {"OR",   OP_ALU_R,  0x3, FORM_RRR},
    [I_XOR]  = {"XOR",  OP_ALU_R,  0x4, FORM_RRR},
    [I_SLT]  = {"SLT",  OP_ALU_R,  0x5, FORM_RRR},
    [I_SLTU] = {"SLTU", OP_ALU_R,  0x6, FORM_RRR},
    [I_SLL]  = {"SLL",  OP_ALU_R,  0x7, FORM_RRR},
    [I_SRL]  = {"SRL",  OP_ALU_R,  0x8, FORM_RRR},
    [I_SRA]  = {"SRA",  OP_ALU_R,  0x9, FORM_RRR},
    /* ALU-I (opcode 0001, I-format) */
    [I_ADDI] = {"ADDI", OP_ALU_I,  0x0, FORM_RRI},
    [I_ANDI] = {"ANDI", OP_ALU_I,  0x1, FORM_RRU},
    [I_ORI]  = {"ORI",  OP_ALU_I,  0x2, FORM_RRU},
    [I_XORI] = {"XORI", OP_ALU_I,  0x3, FORM_RRU},
    /* integer load and store (I-format, sub-opcode 0000) */
    [I_LW]   = {"LW",   OP_LW,     0x0, FORM_RM},
    [I_SW]   = {"SW",   OP_SW,     0x0, FORM_RM},
    /* BRANCH (opcode 0100, I-format) */
    [I_BEQ]  = {"BEQ",  OP_BRANCH, 0x0, FORM_RRB},
    [I_BNE]  = {"BNE",  OP_BRANCH, 0x1, FORM_RRB},
    [I_BLT]  = {"BLT",  OP_BRANCH, 0x2, FORM_RRB},
    [I_BGE]  = {"BGE",  OP_BRANCH, 0x3, FORM_RRB},
    [I_BLTU] = {"BLTU", OP_BRANCH, 0x4, FORM_RRB},
    [I_BGEU] = {"BGEU", OP_BRANCH, 0x5, FORM_RRB},
    /* jumps */
    [I_J]    = {"J",    OP_J,      NO_SUB, FORM_ADDR},
    [I_JAL]  = {"JAL",  OP_JAL,    NO_SUB, FORM_ADDR},
    [I_JR]   = {"JR",   OP_JR,     0x0, FORM_R},
    /* floating-point load and store (I-format, sub-opcode 0000) */
    [I_FLW]  = {"FLW",  OP_FLW,    0x0, FORM_FM},
    [I_FSW]  = {"FSW",  OP_FSW,    0x0, FORM_FM},
    /* FP-ALU (opcode 1010, R-format) */
    [I_FADD] = {"FADD", OP_FP,     0x0, FORM_FFF},
    [I_FSUB] = {"FSUB", OP_FP,     0x1, FORM_FFF},
    [I_FMUL] = {"FMUL", OP_FP,     0x2, FORM_FFF},
    [I_FDIV] = {"FDIV", OP_FP,     0x3, FORM_FFF},
    [I_FLT]  = {"FLT",  OP_FP,     0x4, FORM_RFF},
    [I_FEQ]  = {"FEQ",  OP_FP,     0x5, FORM_RFF},
    [I_FMOV] = {"FMOV", OP_FP,     0x6, FORM_FF},
    /* single-opcode instructions */
    [I_LUI]  = {"LUI",  OP_LUI,    0x0, FORM_RU},
    [I_HALT] = {"HALT", OP_HALT,   NO_SUB, FORM_NONE},
};

static int operand_count(Form form)
{
    switch (form) {
    case FORM_RRR: case FORM_FFF: case FORM_RFF:
    case FORM_RRI: case FORM_RRU: case FORM_RRB:
        return 3;
    case FORM_FF: case FORM_RU: case FORM_RM: case FORM_FM:
        return 2;
    case FORM_R: case FORM_ADDR:
        return 1;
    default:
        return 0;
    }
}

/* The sub-opcode is in bits 15-12 for R-format opcodes and in bits 19-16
 * for I-format opcodes. J, JAL and HALT have none. */
static int is_r_format(unsigned op) { return op == OP_ALU_R || op == OP_JR || op == OP_FP; }
static int has_no_sub(unsigned op)  { return op == OP_J || op == OP_JAL || op == OP_HALT; }

/* Bits that must be 0 in a valid instruction of each form: the unused bits
 * of its format, plus any register field the instruction does not use. */
static uint32_t must_be_zero(Form form)
{
    switch (form) {
    case FORM_RRR: case FORM_FFF: case FORM_RFF:
        return 0x00000FFFu;                  /* unused 11-0 */
    case FORM_FF:
        return 0x00F00FFFu;                  /* rt, unused 11-0 */
    case FORM_R:
        return 0x00FF0FFFu;                  /* rt, rd, unused 11-0 */
    case FORM_RU:
        return 0x0F000000u;                  /* rs */
    case FORM_NONE:
        return 0x0FFFFFFFu;                  /* unused 27-0 */
    default:
        return 0;
    }
}

/* ======================================================================
 * Small helpers
 * ====================================================================== */

/* Print an error and exit. */
static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "isaSim: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

/* The file and line being read, for error messages about its contents. */
static const char *cur_path;
static int cur_line;

/* Report a problem in an input file at cur_path:cur_line, then exit. */
static void file_error(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "isaSim: %s, line %d: ", cur_path, cur_line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

static FILE *open_file(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (f == NULL)
        die("cannot open %s: %s", path, strerror(errno));
    return f;
}

/* Read the next line of f into buf without its line ending (\n or \r\n).
 * Returns 0 at end of file. */
static int read_line(FILE *f, char *buf, size_t size)
{
    size_t len;

    if (fgets(buf, (int)size, f) == NULL)
        return 0;
    cur_line++;
    len = strlen(buf);
    if (len == size - 1 && buf[len - 1] != '\n' && !feof(f))
        file_error("line is longer than %d characters", MAX_LINE - 2);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return 1;
}

/* Remove leading and trailing white space in place; returns the start. */
static char *trim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

/* Two's-complement value of a 32-bit pattern. (A plain cast to int32_t is
 * implementation-defined in C when the top bit is set.) */
static int32_t as_signed(uint32_t x)
{
    return (x & 0x80000000u) ? -(int32_t)(~x) - 1 : (int32_t)x;
}

/* Arithmetic shift right: the sign bit is copied into the vacated bits.
 * Built from logical shifts, because >> of a negative int is
 * implementation-defined in C. */
static uint32_t shift_right_arith(uint32_t x, unsigned n)
{
    return (x & 0x80000000u) ? ~(~x >> n) : x >> n;
}

static float bits_to_float(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static uint32_t float_to_bits(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    return bits;
}

/* The bits an FP arithmetic result is stored as (see CANONICAL_NAN). */
static uint32_t fp_result(float f)
{
    return isnan(f) ? CANONICAL_NAN : float_to_bits(f);
}

/* ======================================================================
 * Data memory
 *
 * 2^28 bytes is 256 MB, so memory is stored as 4 KB pages that are only
 * allocated once a non-zero byte is written to them. A missing page reads
 * as all zeros.
 * ====================================================================== */

#define PAGE_BITS 12
#define PAGE_SIZE (1u << PAGE_BITS)
#define NUM_PAGES (1u << (ADDR_BITS - PAGE_BITS))

typedef struct {
    uint8_t *page[NUM_PAGES];
} Memory;

static uint8_t mem_read_byte(const Memory *m, uint32_t addr)
{
    const uint8_t *p = m->page[addr >> PAGE_BITS];
    return p ? p[addr & (PAGE_SIZE - 1)] : 0;
}

static void mem_write_byte(Memory *m, uint32_t addr, uint8_t value)
{
    uint8_t **p = &m->page[addr >> PAGE_BITS];

    if (*p == NULL) {
        if (value == 0)
            return;                          /* already reads as 0 */
        *p = calloc(PAGE_SIZE, 1);
        if (*p == NULL)
            die("out of memory");
    }
    (*p)[addr & (PAGE_SIZE - 1)] = value;
}

/* M[addr]: the big-endian word in bytes addr..addr+3. */
static uint32_t mem_load_word(const Memory *m, uint32_t addr)
{
    return (uint32_t)mem_read_byte(m, addr) << 24
         | (uint32_t)mem_read_byte(m, addr + 1) << 16
         | (uint32_t)mem_read_byte(m, addr + 2) << 8
         | (uint32_t)mem_read_byte(m, addr + 3);
}

static void mem_store_word(Memory *m, uint32_t addr, uint32_t value)
{
    mem_write_byte(m, addr,     (uint8_t)(value >> 24));
    mem_write_byte(m, addr + 1, (uint8_t)(value >> 16));
    mem_write_byte(m, addr + 2, (uint8_t)(value >> 8));
    mem_write_byte(m, addr + 3, (uint8_t)value);
}

/* Parse a whole string as a hex number, with or without "0x". Returns 0 if
 * it is not one or is larger than max. */
static int parse_hex(const char *s, unsigned long max, unsigned long *out)
{
    unsigned long v = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (*s == '\0')
        return 0;
    for (; *s; s++) {
        int digit;
        if (!isxdigit((unsigned char)*s))
            return 0;
        digit = isdigit((unsigned char)*s) ? *s - '0'
                                           : toupper((unsigned char)*s) - 'A' + 10;
        if (v > (max - (unsigned long)digit) / 16)
            return 0;
        v = v * 16 + (unsigned long)digit;
    }
    *out = v;
    return 1;
}

/*
 * Load a memory file:
 *   line 1:      a comment naming the benchmark (kept for the .mem.out file)
 *   line 2:      address bits, bits per address ("28,8" for our ISA)
 *   other lines: address,contents (both hex)
 * Addresses that are not listed hold 0. Blank lines are skipped.
 */
static void read_mem_file(const char *path, Memory *m, char *header, size_t header_size)
{
    char line[MAX_LINE];
    FILE *f = open_file(path, "r");
    int addr_bits, cell_bits;
    char extra;

    cur_path = path;
    cur_line = 0;
    if (!read_line(f, line, sizeof line))
        file_error("the memory file is empty");
    if (strlen(line) >= header_size)
        file_error("the first line is too long");
    strcpy(header, line);

    if (!read_line(f, line, sizeof line))
        file_error("missing line 2 (address bits, bits per address)");
    if (sscanf(line, " %d , %d %c", &addr_bits, &cell_bits, &extra) != 2)
        file_error("line 2 should be \"%d,%d\", got \"%s\"", ADDR_BITS, CELL_BITS, line);
    if (addr_bits != ADDR_BITS || cell_bits != CELL_BITS)
        file_error("our ISA has %d address bits and %d bits per address, so line 2 "
                   "must be \"%d,%d\" (got \"%s\")",
                   ADDR_BITS, CELL_BITS, ADDR_BITS, CELL_BITS, line);

    while (read_line(f, line, sizeof line)) {
        char *s = trim(line), *comma;
        unsigned long addr, value;

        if (*s == '\0')
            continue;
        comma = strchr(s, ',');
        if (comma == NULL)
            file_error("expected \"address,contents\" in hex, got \"%s\"", s);
        *comma = '\0';
        if (!parse_hex(trim(s), MEM_SIZE - 1, &addr))
            file_error("\"%s\" is not a hex address from 0 to %lX", trim(s), MEM_SIZE - 1);
        if (!parse_hex(trim(comma + 1), 0xFF, &value))
            file_error("\"%s\" is not a hex byte from 00 to FF", trim(comma + 1));
        mem_write_byte(m, (uint32_t)addr, (uint8_t)value);
    }
    fclose(f);
}

/* Write memory in the .mem file format: the input's first line, "28,8",
 * then every word that holds a non-zero byte, as four "address,byte" lines,
 * in address order. */
static void write_mem_file(const char *path, const Memory *m, const char *header)
{
    FILE *f = open_file(path, "w");
    uint32_t pg, off;
    int i;

    fprintf(f, "%s\n%d,%d\n", header, ADDR_BITS, CELL_BITS);
    for (pg = 0; pg < NUM_PAGES; pg++) {
        const uint8_t *p = m->page[pg];
        if (p == NULL)
            continue;
        for (off = 0; off < PAGE_SIZE; off += 4) {
            if ((p[off] | p[off + 1] | p[off + 2] | p[off + 3]) == 0)
                continue;
            for (i = 0; i < 4; i++)
                fprintf(f, "%07lX,%02X\n",
                        (unsigned long)(pg << PAGE_BITS | (off + (uint32_t)i)),
                        (unsigned)p[off + (uint32_t)i]);
        }
    }
    if (fclose(f) != 0)
        die("cannot write %s: %s", path, strerror(errno));
}

/* ======================================================================
 * Assembler: assembly text -> 32-bit instruction words
 *
 * One instruction per line; "#" starts a comment; blank lines are ignored.
 * Mnemonics and register names may be upper or lower case. Numbers are
 * decimal (-4) or hex (0x100). Labels are not allowed: a branch operand is
 * an offset counted from the next instruction (PC <- PC + 1 + offset), and
 * a J or JAL operand is an absolute instruction address.
 * ====================================================================== */

typedef struct {
    uint32_t word;               /* the machine instruction */
    int line;                    /* its line in the assembly file */
} Instruction;

typedef struct {
    Instruction *code;           /* instruction memory: code[PC] */
    uint32_t count;
    uint32_t capacity;
} Program;

static uint32_t r_word(unsigned op, unsigned rs, unsigned rt, unsigned rd, unsigned sub)
{
    return (uint32_t)op << 28 | (uint32_t)rs << 24 | (uint32_t)rt << 20
         | (uint32_t)rd << 16 | (uint32_t)sub << 12;
}

static uint32_t i_word(unsigned op, unsigned rs, unsigned rt, unsigned sub, uint32_t imm16)
{
    return (uint32_t)op << 28 | (uint32_t)rs << 24 | (uint32_t)rt << 20
         | (uint32_t)sub << 16 | (imm16 & 0xFFFFu);
}

/* A register operand: kind is 'R' (R0-R15) or 'F' (F0-F15). */
static unsigned parse_reg(const char *tok, char kind)
{
    const char *what = kind == 'R' ? "an integer register (R0-R15)"
                                   : "a floating-point register (F0-F15)";
    unsigned n;

    if (toupper((unsigned char)tok[0]) != kind || !isdigit((unsigned char)tok[1])
        || (tok[2] != '\0' && (!isdigit((unsigned char)tok[2]) || tok[3] != '\0')))
        file_error("expected %s, got \"%s\"", what, tok);
    n = (unsigned)atoi(tok + 1);
    if (n >= NUM_REGS)
        file_error("expected %s, got \"%s\"", what, tok);
    return n;
}

/* A decimal or 0x-hex number, optionally signed. */
static long long parse_number(const char *tok)
{
    const char *p = tok;
    int negative = 0, base = 10;
    unsigned long long v;

    if (*p == '+' || *p == '-')
        negative = *p++ == '-';
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    if (*p == '\0')
        file_error("expected a number, got \"%s\"", tok);
    for (const char *q = p; *q; q++)
        if (base == 16 ? !isxdigit((unsigned char)*q) : !isdigit((unsigned char)*q))
            file_error("expected a number, got \"%s\"", tok);
    errno = 0;
    v = strtoull(p, NULL, base);
    if (errno == ERANGE || v > 0xFFFFFFFFull)
        file_error("%s is too large", tok);
    return negative ? -(long long)v : (long long)v;
}

/* A 16-bit field that is sign-extended when used: -32768 to 32767. */
static uint32_t parse_signed16(const char *tok)
{
    long long v = parse_number(tok);
    if (v < -32768 || v > 32767)
        file_error("%s does not fit a signed 16-bit field (-32768 to 32767)", tok);
    return (uint32_t)v & 0xFFFFu;
}

/* A 16-bit field that is zero-extended when used: 0 to 65535 (0xFFFF). */
static uint32_t parse_unsigned16(const char *tok)
{
    long long v = parse_number(tok);
    if (v < 0 || v > 0xFFFF)
        file_error("%s does not fit an unsigned 16-bit field (0 to 0xFFFF)", tok);
    return (uint32_t)v;
}

/* A memory operand "offset(Rn)". An empty offset means 0. */
static void parse_mem_operand(char *tok, uint32_t *offset16, unsigned *base)
{
    char *open = strchr(tok, '('), *close = strrchr(tok, ')');
    char *off;

    if (open == NULL || close == NULL || close < open || close[1] != '\0')
        file_error("expected offset(Rn), got \"%s\"", tok);
    *open = '\0';
    *close = '\0';
    off = trim(tok);
    *offset16 = *off ? parse_signed16(off) : 0;
    *base = parse_reg(trim(open + 1), 'R');
}

/* Assemble one instruction from its mnemonic and operand strings. */
static uint32_t encode(InstrId id, char **ops)
{
    const InstrInfo *in = &ISA[id];
    unsigned sub = in->sub == NO_SUB ? 0 : (unsigned)in->sub;
    uint32_t imm;
    unsigned base;
    long long addr;

    switch (in->form) {
    case FORM_RRR:               /* rd, rs, rt */
        return r_word(in->opcode, parse_reg(ops[1], 'R'), parse_reg(ops[2], 'R'),
                      parse_reg(ops[0], 'R'), sub);
    case FORM_FFF:               /* fd, fs, ft */
        return r_word(in->opcode, parse_reg(ops[1], 'F'), parse_reg(ops[2], 'F'),
                      parse_reg(ops[0], 'F'), sub);
    case FORM_RFF:               /* rd, fs, ft */
        return r_word(in->opcode, parse_reg(ops[1], 'F'), parse_reg(ops[2], 'F'),
                      parse_reg(ops[0], 'R'), sub);
    case FORM_FF:                /* fd, fs */
        return r_word(in->opcode, parse_reg(ops[1], 'F'), 0, parse_reg(ops[0], 'F'), sub);
    case FORM_R:                 /* rs */
        return r_word(in->opcode, parse_reg(ops[0], 'R'), 0, 0, sub);
    case FORM_RRI:               /* rt, rs, signed imm */
        return i_word(in->opcode, parse_reg(ops[1], 'R'), parse_reg(ops[0], 'R'), sub,
                      parse_signed16(ops[2]));
    case FORM_RRU:               /* rt, rs, unsigned imm */
        return i_word(in->opcode, parse_reg(ops[1], 'R'), parse_reg(ops[0], 'R'), sub,
                      parse_unsigned16(ops[2]));
    case FORM_RU:                /* rt, unsigned imm */
        return i_word(in->opcode, 0, parse_reg(ops[0], 'R'), sub, parse_unsigned16(ops[1]));
    case FORM_RM:                /* rt, offset(rs) */
        parse_mem_operand(ops[1], &imm, &base);
        return i_word(in->opcode, base, parse_reg(ops[0], 'R'), sub, imm);
    case FORM_FM:                /* ft, offset(rs) */
        parse_mem_operand(ops[1], &imm, &base);
        return i_word(in->opcode, base, parse_reg(ops[0], 'F'), sub, imm);
    case FORM_RRB:               /* rs, rt, offset */
        return i_word(in->opcode, parse_reg(ops[0], 'R'), parse_reg(ops[1], 'R'), sub,
                      parse_signed16(ops[2]));
    case FORM_ADDR:              /* 28-bit instruction address */
        addr = parse_number(ops[0]);
        if (addr < 0 || addr > 0x0FFFFFFF)
            file_error("jump address %s does not fit 28 bits", ops[0]);
        return (uint32_t)in->opcode << 28 | (uint32_t)addr;
    case FORM_NONE:              /* HALT */
    default:
        return (uint32_t)in->opcode << 28;
    }
}

static int lookup_mnemonic(const char *name)
{
    int i;
    size_t k;

    for (i = 0; i < NUM_INSTRS; i++) {
        const char *s = ISA[i].name;
        for (k = 0; s[k] && toupper((unsigned char)name[k]) == s[k]; k++)
            ;
        if (s[k] == '\0' && name[k] == '\0')
            return i;
    }
    return -1;
}

static void add_instruction(Program *prog, uint32_t word, int line)
{
    if (prog->count == prog->capacity) {
        uint32_t cap = prog->capacity ? prog->capacity * 2 : 64;
        Instruction *code = realloc(prog->code, cap * sizeof *code);
        if (code == NULL)
            die("out of memory");
        prog->code = code;
        prog->capacity = cap;
    }
    prog->code[prog->count].word = word;
    prog->code[prog->count].line = line;
    prog->count++;
}

/* Assemble every instruction in the file into prog. The first instruction
 * goes to instruction address 0. Exits with a message on the first error. */
static void assemble_file(const char *path, Program *prog)
{
    char line[MAX_LINE];
    FILE *f = open_file(path, "r");

    cur_path = path;
    cur_line = 0;
    while (read_line(f, line, sizeof line)) {
        char *s, *mnemonic, *rest, *hash;
        char *ops[4];
        int id, nops = 0;

        if ((hash = strchr(line, '#')) != NULL)
            *hash = '\0';                    /* drop the comment */
        s = trim(line);
        if (*s == '\0')
            continue;

        /* split off the mnemonic */
        mnemonic = s;
        while (*s && !isspace((unsigned char)*s))
            s++;
        if (*s)
            *s++ = '\0';
        rest = trim(s);

        if (mnemonic[strlen(mnemonic) - 1] == ':')
            file_error("labels are not allowed; replace each label with its "
                       "number (a branch offset or a jump address)");
        id = lookup_mnemonic(mnemonic);
        if (id < 0)
            file_error("unknown instruction \"%s\"", mnemonic);

        /* split the operands at commas */
        if (*rest) {
            char *tok = rest;
            for (;;) {
                char *comma = strchr(tok, ',');
                if (nops == 4)
                    break;
                if (comma)
                    *comma = '\0';
                ops[nops] = trim(tok);
                if (*ops[nops] == '\0')
                    file_error("missing operand");
                nops++;
                if (!comma)
                    break;
                tok = comma + 1;
            }
        }
        if (nops != operand_count(ISA[id].form))
            file_error("%s takes %d operand%s", ISA[id].name, operand_count(ISA[id].form),
                       operand_count(ISA[id].form) == 1 ? "" : "s");

        add_instruction(prog, encode((InstrId)id, ops), cur_line);
    }
    fclose(f);
    if (prog->count == 0)
        die("%s has no instructions", path);
}

/* ======================================================================
 * Decoder and disassembler: 32-bit word -> fields
 * ====================================================================== */

typedef struct {
    InstrId id;
    unsigned rs, rt, rd;         /* register fields: bits 27-24, 23-20, 19-16 */
    uint32_t imm;                /* bits 15-0, zero-extended: ZeroExt(imm) */
    int32_t simm;                /* bits 15-0, sign-extended: SignExt(imm) */
    uint32_t addr;               /* bits 27-0 (J-format) */
} Decoded;

/* decode_table[opcode][sub-opcode] is the instruction, or -1 if that
 * combination is reserved. Opcodes without a sub-opcode use column 0. */
static int decode_table[16][16];

static void build_decode_table(void)
{
    int i, j;

    for (i = 0; i < 16; i++)
        for (j = 0; j < 16; j++)
            decode_table[i][j] = -1;
    for (i = 0; i < NUM_INSTRS; i++)
        decode_table[ISA[i].opcode][ISA[i].sub == NO_SUB ? 0 : ISA[i].sub] = i;
}

/* Split a word into its fields. Returns 0 if it is not a valid instruction:
 * a reserved opcode or sub-opcode, or a non-zero bit in an unused field. */
static int decode(uint32_t w, Decoded *d)
{
    unsigned op = w >> 28;
    unsigned sub = has_no_sub(op) ? 0 : is_r_format(op) ? (w >> 12) & 0xF : (w >> 16) & 0xF;
    int id = decode_table[op][sub];

    if (id < 0 || (w & must_be_zero(ISA[id].form)) != 0)
        return 0;
    d->id = (InstrId)id;
    d->rs = (w >> 24) & 0xF;
    d->rt = (w >> 20) & 0xF;
    d->rd = (w >> 16) & 0xF;
    d->imm = w & 0xFFFFu;
    d->simm = (int32_t)(d->imm ^ 0x8000u) - 0x8000;
    d->addr = w & 0x0FFFFFFFu;
    return 1;
}

/* Turn a word back into assembly, e.g. "ADDI  R2, R0, 8". */
static void disassemble(uint32_t w, char *buf, size_t size)
{
    Decoded d;
    const char *nm;

    if (!decode(w, &d)) {
        snprintf(buf, size, "(not a valid instruction)");
        return;
    }
    nm = ISA[d.id].name;
    switch (ISA[d.id].form) {
    case FORM_RRR:  snprintf(buf, size, "%-5s R%u, R%u, R%u", nm, d.rd, d.rs, d.rt); break;
    case FORM_FFF:  snprintf(buf, size, "%-5s F%u, F%u, F%u", nm, d.rd, d.rs, d.rt); break;
    case FORM_RFF:  snprintf(buf, size, "%-5s R%u, F%u, F%u", nm, d.rd, d.rs, d.rt); break;
    case FORM_FF:   snprintf(buf, size, "%-5s F%u, F%u", nm, d.rd, d.rs); break;
    case FORM_R:    snprintf(buf, size, "%-5s R%u", nm, d.rs); break;
    case FORM_RRI:  snprintf(buf, size, "%-5s R%u, R%u, %ld", nm, d.rt, d.rs, (long)d.simm); break;
    case FORM_RRU:  snprintf(buf, size, "%-5s R%u, R%u, 0x%lX", nm, d.rt, d.rs, (unsigned long)d.imm); break;
    case FORM_RU:   snprintf(buf, size, "%-5s R%u, 0x%lX", nm, d.rt, (unsigned long)d.imm); break;
    case FORM_RM:   snprintf(buf, size, "%-5s R%u, %ld(R%u)", nm, d.rt, (long)d.simm, d.rs); break;
    case FORM_FM:   snprintf(buf, size, "%-5s F%u, %ld(R%u)", nm, d.rt, (long)d.simm, d.rs); break;
    case FORM_RRB:  snprintf(buf, size, "%-5s R%u, R%u, %ld", nm, d.rs, d.rt, (long)d.simm); break;
    case FORM_ADDR: snprintf(buf, size, "%-5s %lu", nm, (unsigned long)d.addr); break;
    case FORM_NONE: snprintf(buf, size, "%s", nm); break;
    }
}

/* ======================================================================
 * CPU: executes one instruction at a time
 * ====================================================================== */

typedef struct {
    uint32_t pc;                 /* address of the next instruction */
    uint32_t R[NUM_REGS];        /* integer registers */
    uint32_t F[NUM_REGS];        /* FP registers, as raw IEEE-754 bits */
    unsigned long long executed; /* instructions executed so far */
    unsigned long long count[NUM_INSTRS];  /* executions of each instruction */
    int halted;
} Cpu;

/* What one instruction did, for the step-by-step display. */
typedef struct {
    uint32_t pc;                 /* where it was */
    uint32_t word;
    int stored;                  /* 1 if it wrote data memory */
    uint32_t store_addr, store_value;
    int branched;                /* 1 if it was a conditional branch */
    int taken;
} StepInfo;

/* Holds the message for the runtime error cpu_step() reports. */
static char sim_error[256];

/* R[r] <- value. Writes to R0 are ignored, so R0 always reads 0. */
static void set_reg(Cpu *c, unsigned r, uint32_t value)
{
    if (r != 0)
        c->R[r] = value;
}

/* The address R[rs] + SignExt(offset) for a load or store. Returns 0 (and
 * sets sim_error) if it is not a multiple of 4 or is outside data memory. */
static int data_address(uint32_t base, int32_t offset, uint32_t *addr)
{
    uint32_t a = base + (uint32_t)offset;

    if (a % 4 != 0) {
        snprintf(sim_error, sizeof sim_error,
                 "address 0x%lX is not a multiple of 4", (unsigned long)a);
        return 0;
    }
    if (a > MEM_SIZE - 4) {
        snprintf(sim_error, sizeof sim_error,
                 "address 0x%lX is outside data memory (0x0 to 0x%lX)",
                 (unsigned long)a, MEM_SIZE - 1);
        return 0;
    }
    *addr = a;
    return 1;
}

/* A branch or jump target must be an instruction in the program. */
static int check_target(long long target, const Program *prog)
{
    if (target < 0 || target >= (long long)prog->count) {
        snprintf(sim_error, sizeof sim_error,
                 "jump to address %lld, outside the program (addresses 0 to %lu)",
                 target, (unsigned long)prog->count - 1);
        return 0;
    }
    return 1;
}

/*
 * Fetch, decode and execute the instruction at PC. Every instruction does
 * PC <- PC + 1 unless its RTL assigns PC. Returns NULL on success, or an
 * error message; after an error the machine state is unchanged.
 */
static const char *cpu_step(Cpu *c, Memory *m, const Program *prog, StepInfo *info)
{
    uint32_t pc = c->pc, npc = pc + 1, w, a, b, addr, value;
    float fs, ft;
    Decoded d;
    int taken = 0;

    /* fetch */
    if (pc >= prog->count) {
        snprintf(sim_error, sizeof sim_error,
                 "PC %lu is past the last instruction; the program ended without HALT",
                 (unsigned long)pc);
        return sim_error;
    }
    w = prog->code[pc].word;

    /* decode */
    if (!decode(w, &d)) {
        snprintf(sim_error, sizeof sim_error,
                 "0x%08lX is not a valid instruction", (unsigned long)w);
        return sim_error;
    }
    memset(info, 0, sizeof *info);
    info->pc = pc;
    info->word = w;
    a = c->R[d.rs];
    b = c->R[d.rt];
    fs = bits_to_float(c->F[d.rs]);
    ft = bits_to_float(c->F[d.rt]);

    /* execute */
    switch (d.id) {
    /* ALU-R: R[rd] <- R[rs] op R[rt]. Adds and subtracts wrap. */
    case I_ADD:  set_reg(c, d.rd, a + b); break;
    case I_SUB:  set_reg(c, d.rd, a - b); break;
    case I_AND:  set_reg(c, d.rd, a & b); break;
    case I_OR:   set_reg(c, d.rd, a | b); break;
    case I_XOR:  set_reg(c, d.rd, a ^ b); break;
    case I_SLT:  set_reg(c, d.rd, as_signed(a) < as_signed(b)); break;
    case I_SLTU: set_reg(c, d.rd, a < b); break;
    /* shifts use only the low 5 bits of R[rt] */
    case I_SLL:  set_reg(c, d.rd, a << (b & 31)); break;
    case I_SRL:  set_reg(c, d.rd, a >> (b & 31)); break;
    case I_SRA:  set_reg(c, d.rd, shift_right_arith(a, b & 31)); break;

    /* ALU-I: R[rt] <- R[rs] op imm */
    case I_ADDI: set_reg(c, d.rt, a + (uint32_t)d.simm); break;
    case I_ANDI: set_reg(c, d.rt, a & d.imm); break;
    case I_ORI:  set_reg(c, d.rt, a | d.imm); break;
    case I_XORI: set_reg(c, d.rt, a ^ d.imm); break;
    case I_LUI:  set_reg(c, d.rt, d.imm << 16); break;

    /* loads and stores: address R[rs] + SignExt(offset) */
    case I_LW:
    case I_FLW:
        if (!data_address(a, d.simm, &addr))
            return sim_error;
        if (d.id == I_LW)
            set_reg(c, d.rt, mem_load_word(m, addr));
        else
            c->F[d.rt] = mem_load_word(m, addr);
        break;
    case I_SW:
    case I_FSW:
        if (!data_address(a, d.simm, &addr))
            return sim_error;
        value = d.id == I_SW ? b : c->F[d.rt];
        mem_store_word(m, addr, value);
        info->stored = 1;
        info->store_addr = addr;
        info->store_value = value;
        break;

    /* branches: if taken, PC <- PC + 1 + SignExt(offset) */
    case I_BEQ:  taken = a == b; goto branch;
    case I_BNE:  taken = a != b; goto branch;
    case I_BLT:  taken = as_signed(a) < as_signed(b); goto branch;
    case I_BGE:  taken = as_signed(a) >= as_signed(b); goto branch;
    case I_BLTU: taken = a < b; goto branch;
    case I_BGEU: taken = a >= b;
    branch:
        if (taken) {
            long long target = (long long)pc + 1 + d.simm;
            if (!check_target(target, prog))
                return sim_error;
            npc = (uint32_t)target;
        }
        info->branched = 1;
        info->taken = taken;
        break;

    /* jumps */
    case I_J:
        if (!check_target(d.addr, prog))
            return sim_error;
        npc = d.addr;
        break;
    case I_JAL:
        if (!check_target(d.addr, prog))
            return sim_error;
        set_reg(c, LINK_REG, pc + 1);
        npc = d.addr;
        break;
    case I_JR:
        if (!check_target(a, prog))
            return sim_error;
        npc = a;
        break;

    /* FP-ALU: IEEE-754 single precision. Dividing by zero gives an infinity
     * or NaN and does not stop the program. */
    case I_FADD: c->F[d.rd] = fp_result(fs + ft); break;
    case I_FSUB: c->F[d.rd] = fp_result(fs - ft); break;
    case I_FMUL: c->F[d.rd] = fp_result(fs * ft); break;
    case I_FDIV: c->F[d.rd] = fp_result(fs / ft); break;
    /* compares write 1 or 0 to an integer register; NaN compares false */
    case I_FLT:  set_reg(c, d.rd, fs < ft); break;
    case I_FEQ:  set_reg(c, d.rd, fs == ft); break;
    case I_FMOV: c->F[d.rd] = c->F[d.rs]; break;

    case I_HALT:
        c->halted = 1;
        npc = pc;                            /* PC stays on the HALT */
        break;

    default:
        snprintf(sim_error, sizeof sim_error, "instruction %d is not implemented", (int)d.id);
        return sim_error;
    }

    c->count[d.id]++;
    c->executed++;
    c->pc = npc;
    return NULL;
}

/* ======================================================================
 * Step-by-step display
 * ====================================================================== */

/* The shortest decimal that reads back as exactly this single-precision
 * value, e.g. "6.5" or "0.1". */
static void format_float(uint32_t bits, char *buf, size_t size)
{
    float f = bits_to_float(bits);
    int digits;

    if (isnan(f)) {
        snprintf(buf, size, "NaN");
        return;
    }
    if (isinf(f)) {
        snprintf(buf, size, f > 0 ? "+inf" : "-inf");
        return;
    }
    for (digits = 1; digits <= 9; digits++) {    /* 9 digits always suffice */
        snprintf(buf, size, "%.*g", digits, (double)f);
        if (float_to_bits(strtof(buf, NULL)) == bits)
            return;
    }
}

/* One line describing the instruction at address pc: "[  7] 0x12220004  ADDI ..." */
static void print_instruction(const Program *prog, uint32_t pc)
{
    char text[64];

    if (pc >= prog->count) {
        printf("[%3lu] (past the last instruction)\n", (unsigned long)pc);
        return;
    }
    disassemble(prog->code[pc].word, text, sizeof text);
    printf("[%3lu] 0x%08lX  %s\n", (unsigned long)pc,
           (unsigned long)prog->code[pc].word, text);
}

/* Print the machine state: PC, the instruction count, R0-R15 and F0-F15.
 * prev and last are NULL before the first instruction; otherwise prev is
 * the state before the last instruction ran, and registers that the last
 * instruction changed are marked with "*". */
static void print_state(const Cpu *c, const Program *prog, const Cpu *prev, const StepInfo *last)
{
    int i, col;

    printf("\n----------------------------------------------------------------------\n");
    printf("Instructions executed: %llu\n", c->executed);
    if (last != NULL) {
        printf("Last executed:  ");
        print_instruction(prog, last->pc);
        if (last->branched)
            printf("                branch %s\n", last->taken ? "taken" : "not taken");
        if (last->stored)
            printf("                M[0x%07lX] <- 0x%08lX\n",
                   (unsigned long)last->store_addr, (unsigned long)last->store_value);
    }
    if (c->halted) {
        printf("Next:           (halted)\n");
    } else {
        printf("Next:           ");
        print_instruction(prog, c->pc);
    }
    printf("PC = %lu\n\n", (unsigned long)c->pc);

    printf("Integer registers (hex, signed decimal)%s\n",
           prev ? "    * = changed by the last instruction" : "");
    for (i = 0; i < NUM_REGS / 2; i++) {
        for (col = 0; col < 2; col++) {
            int r = i + col * NUM_REGS / 2;
            int changed = prev && prev->R[r] != c->R[r];
            printf("%s%c R%-2d 0x%08lX %11ld", col ? "     " : " ", changed ? '*' : ' ', r,
                   (unsigned long)c->R[r], (long)as_signed(c->R[r]));
        }
        printf("\n");
    }

    printf("Floating-point registers (hex, value)\n");
    for (i = 0; i < NUM_REGS / 2; i++) {
        for (col = 0; col < 2; col++) {
            int r = i + col * NUM_REGS / 2;
            int changed = prev && prev->F[r] != c->F[r];
            char value[32];
            format_float(c->F[r], value, sizeof value);
            printf("%s%c F%-2d 0x%08lX  %-*s", col ? "  " : " ", changed ? '*' : ' ', r,
                   (unsigned long)c->F[r], col ? 0 : 15, value);
        }
        printf("\n");
    }
}

/* ======================================================================
 * Results: instruction counts and the final memory
 * ====================================================================== */

static void print_counts(FILE *f, const Cpu *c, const char *asm_path, const char *mem_path)
{
    int i;

    fprintf(f, "Instruction counts for %s with %s\n", asm_path, mem_path);
    fprintf(f, "%-12s %12s\n", "Instruction", "Executed");
    for (i = 0; i < NUM_INSTRS; i++)
        fprintf(f, "%-12s %12llu\n", ISA[i].name, c->count[i]);
    fprintf(f, "%-12s %12llu\n", "Total", c->executed);
}

/* "<dir>/maxFinderIntNeg.mem" -> "maxFinderIntNeg" */
static void benchmark_name(const char *mem_path, char *name, size_t size)
{
    const char *base = strrchr(mem_path, '/');
    size_t len;

    base = base ? base + 1 : mem_path;
    len = strlen(base);
    if (len > 4 && strcmp(base + len - 4, ".mem") == 0)
        len -= 4;
    if (len + 16 > size)
        die("memory file name is too long: %s", mem_path);
    memcpy(name, base, len);
    name[len] = '\0';
}

/* At HALT: print the counts, then write <name>.instrCount and <name>.mem.out. */
static void finish(const Cpu *c, const Memory *m, const char *header,
                   const char *asm_path, const char *mem_path)
{
    char name[MAX_LINE], count_path[MAX_LINE + 16], out_path[MAX_LINE + 16];
    FILE *f;

    benchmark_name(mem_path, name, sizeof name);
    snprintf(count_path, sizeof count_path, "%s.instrCount", name);
    snprintf(out_path, sizeof out_path, "%s.mem.out", name);

    printf("\nHALT at PC %lu after %llu instructions.\n\n", (unsigned long)c->pc, c->executed);
    print_counts(stdout, c, asm_path, mem_path);

    f = open_file(count_path, "w");
    print_counts(f, c, asm_path, mem_path);
    if (fclose(f) != 0)
        die("cannot write %s: %s", count_path, strerror(errno));
    write_mem_file(out_path, m, header);

    printf("\nWrote %s\nWrote %s\n", count_path, out_path);
}

/* A runtime error stops the simulation; no output files are written. */
static void runtime_error(const Cpu *c, const Program *prog, const char *msg)
{
    fflush(stdout);
    if (c->pc < prog->count) {
        char text[64];
        disassemble(prog->code[c->pc].word, text, sizeof text);
        fprintf(stderr, "isaSim: runtime error at PC %lu (%s, line %d of the assembly file): %s\n",
                (unsigned long)c->pc, text, prog->code[c->pc].line, msg);
    } else {
        fprintf(stderr, "isaSim: runtime error: %s\n", msg);
    }
    fprintf(stderr, "isaSim: stopped after %llu instruction%s; no output files were written\n",
            c->executed, c->executed == 1 ? "" : "s");
    exit(EXIT_FAILURE);
}

/* ======================================================================
 * The two modes
 * ====================================================================== */

/* -r: execute until HALT. */
static void run_mode(Cpu *c, Memory *m, const Program *prog)
{
    StepInfo info;
    const char *err;

    while (!c->halted) {
        if (c->executed >= MAX_INSTRUCTIONS)
            runtime_error(c, prog, "no HALT after 100,000,000 instructions (infinite loop?)");
        if ((err = cpu_step(c, m, prog, &info)) != NULL)
            runtime_error(c, prog, err);
    }
}

#define QUIT        0ULL
#define RUN_TO_HALT ULLONG_MAX

/* Ask how many instructions to execute next: Enter for 1, a number N for
 * N, "r" to run to HALT, "q" to quit. End of input means run to HALT. */
static unsigned long long ask_steps(void)
{
    char buf[128];

    for (;;) {
        char *s, *end;
        size_t len;

        printf("\n[Enter] next instruction   [N] run N instructions   "
               "[r] run to HALT   [q] quit\n> ");
        fflush(stdout);
        if (fgets(buf, sizeof buf, stdin) == NULL) {
            printf("\n(end of input: running to HALT)\n");
            return RUN_TO_HALT;
        }
        len = strlen(buf);
        if (len > 0 && buf[len - 1] != '\n') {    /* discard the rest of a long line */
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF)
                ;
        }
        s = trim(buf);
        if (*s == '\0')
            return 1;
        if ((s[0] == 'r' || s[0] == 'R') && s[1] == '\0')
            return RUN_TO_HALT;
        if ((s[0] == 'q' || s[0] == 'Q') && s[1] == '\0')
            return QUIT;
        if (isdigit((unsigned char)s[0])) {
            unsigned long long n;
            errno = 0;
            n = strtoull(s, &end, 10);
            if (*end == '\0' && errno == 0 && n > 0)
                return n;
        }
        printf("Please press Enter, type a number of instructions, r, or q.\n");
    }
}

/* -s: show the state, wait for the user, execute, repeat. Returns 0 at
 * HALT, or 1 if the user quit first. */
static int step_mode(Cpu *c, Memory *m, const Program *prog)
{
    Cpu prev;
    StepInfo info;
    const char *err;

    print_state(c, prog, NULL, NULL);
    while (!c->halted) {
        unsigned long long n = ask_steps();
        if (n == QUIT)
            return 1;
        prev = *c;
        memset(&info, 0, sizeof info);
        while (n-- > 0 && !c->halted) {
            if (c->executed >= MAX_INSTRUCTIONS)
                runtime_error(c, prog, "no HALT after 100,000,000 instructions (infinite loop?)");
            prev = *c;
            if ((err = cpu_step(c, m, prog, &info)) != NULL)
                runtime_error(c, prog, err);
        }
        print_state(c, prog, &prev, &info);
    }
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f,
            "Usage: isaSim -<r|s> <assembly file> <memory file>\n"
            "  -r   run straight through to HALT\n"
            "  -s   step-by-step: press Enter for the next instruction, type a number N\n"
            "       to run N instructions, r to run to HALT, or q to quit\n"
            "At HALT the instruction counts are printed and written to <name>.instrCount,\n"
            "and the final memory is written to <name>.mem.out, where <name> is the\n"
            "memory file's name without .mem.\n");
}

int main(int argc, char **argv)
{
    static Memory mem;           /* static: too big for the stack, starts all 0 */
    static char header[MAX_LINE];
    Program prog = {NULL, 0, 0};
    Cpu cpu;
    int step;

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(stdout);
        return EXIT_SUCCESS;
    }
    if (argc != 4 || (strcmp(argv[1], "-r") != 0 && strcmp(argv[1], "-s") != 0)) {
        usage(stderr);
        return 2;
    }
    step = argv[1][1] == 's';

    build_decode_table();
    assemble_file(argv[2], &prog);
    read_mem_file(argv[3], &mem, header, sizeof header);

    memset(&cpu, 0, sizeof cpu);             /* PC = 0, all registers 0 */
    printf("isaSim: %s with %s, %lu instructions loaded (%s mode)\n",
           argv[2], argv[3], (unsigned long)prog.count, step ? "step-by-step" : "run");

    if (step && step_mode(&cpu, &mem, &prog) != 0)
        printf("Quit before HALT after %llu instruction%s; no output files were written.\n",
               cpu.executed, cpu.executed == 1 ? "" : "s");
    else {
        if (!step)
            run_mode(&cpu, &mem, &prog);
        finish(&cpu, &mem, header, argv[2], argv[3]);
    }
    free(prog.code);
    return EXIT_SUCCESS;
}
