/*
 * vm.h — 32-bit Forth VM: opcodes, data structures, and public interface
 *
 * DESIGN OVERVIEW
 * ---------------
 * This is a 32-bit Forth VM targeting a native macOS process. Memory is
 * byte-addressed (uint8_t mem[256KB]), and the native cell size is 32 bits.
 * The inner interpreter uses computed goto for best dispatch performance.
 *
 * EXECUTION MODEL
 * ---------------
 * The VM has two stacks:
 *   - Data stack (ds): holds working values — operands for all words
 *   - Return stack (rs): holds return addresses for nested word calls
 *
 * Words are executed by setting IP to the word's body address and calling
 * vm_run(). The body is a sequence of opcodes. A word ends with OP_RET,
 * which pops the saved IP from the return stack — or exits vm_run() if
 * the return stack is empty (top-level call from the outer interpreter).
 *
 * DICTIONARY FORMAT
 * -----------------
 * Each entry in memory:
 *
 *   [link: 4 bytes][flags: 1 byte][namelen: 1 byte][name: N bytes][body: ...]
 *
 *   link    — byte address of the previous entry (0 = end of chain)
 *   flags   — F_IMMEDIATE, F_HIDDEN, F_PRIMITIVE (see below)
 *   namelen — byte count of name
 *   name    — raw bytes, NOT null-terminated
 *   body    — bytecode starting immediately after the name
 *
 * LATEST points to the most recent entry. Dictionary search walks the chain
 * from LATEST back to 0.
 *
 * MEMORY LAYOUT
 * -------------
 *   0x000000 - 0x000003 : null sentinel (address 0 = null link terminator)
 *   0x000004 - 0x00001F : memory-mapped VM variables (STATE, BASE, >IN, ...)
 *   0x000020 - 0x00007F : (reserved / unused)
 *   0x000080 - 0x0000BF : PAD buffer (64 bytes, grows downward; top at 0x00C0)
 *   0x000100 - 0x0002FF : data stack (128 cells × 4 bytes)
 *   0x000300 - 0x0004FF : return stack (128 cells × 4 bytes)
 *   0x000500 - 0x000CFF : TIB — terminal input buffer (2048 bytes)
 *   0x000D00 - 0x03FFFF : dictionary (HERE starts here, grows upward)
 *
 * VM VARIABLES (memory-mapped, accessible from Forth via @ and !)
 * ---------------------------------------------------------------
 *   ADDR_STATE  — 0=interpret, 1=compile
 *   ADDR_BASE   — number base (default 10)
 *   ADDR_IN     — >IN: offset into current input line
 *   ADDR_SRCLEN — SOURCE-LEN: length of current input line
 *   ADDR_HLD    — pictured numeric output hold pointer
 */

#ifndef VM_H
#define VM_H

#include <stdint.h>
#include <setjmp.h>

/* -------------------------------------------------------------------------
 * Opcodes (one byte each)
 *
 * Instructions with operands:
 *   OP_LIT  — followed by 4 bytes (32-bit literal, little-endian)
 *   OP_JMP  — followed by 4 bytes (target address)
 *   OP_JZ   — followed by 4 bytes (target, jump if TOS == 0)
 *   OP_CALL — followed by 4 bytes (target address)
 * ------------------------------------------------------------------------- */

/* Control flow */
#define OP_HALT  0x00   /* stop the VM */
#define OP_RET   0x01   /* return: pop IP from return stack (or exit if empty) */
#define OP_CALL  0x02   /* call: push IP, jump to 32-bit address operand */
#define OP_JMP   0x03   /* jump to 32-bit address operand */
#define OP_JZ    0x04   /* pop TOS; if zero, jump to 32-bit address operand */
#define OP_LIT   0x05   /* push 32-bit literal operand onto data stack */

/* Arithmetic */
#define OP_ADD    0x06   /* ( a b -- a+b ) */
#define OP_SUB    0x07   /* ( a b -- a-b ) */
#define OP_MUL    0x08   /* ( a b -- a*b ) */
#define OP_DIV    0x09   /* ( a b -- a/b ) signed */
#define OP_NEGATE 0x23   /* ( n -- -n )   two's complement negation */

/* Stack manipulation */
#define OP_DUP   0x0A   /* ( a   -- a a   ) */
#define OP_DROP  0x0B   /* ( a   --       ) */
#define OP_SWAP  0x0C   /* ( a b -- b a   ) */
#define OP_OVER  0x0D   /* ( a b -- a b a ) */

/* Comparison — Forth convention: -1 (0xFFFFFFFF) = true, 0 = false */
#define OP_EQ    0x0E   /* =  */
#define OP_LT    0x0F   /* <  (signed) */
#define OP_GT    0x10   /* >  (signed) */
#define OP_ZEROEQ 0x11  /* 0= logical not */

/* I/O */
#define OP_EMIT  0x12   /* ( char -- )     print low byte as ASCII */
#define OP_KEY   0x13   /* ( -- char )     read one char; -1 on EOF */
#define OP_DOT   0x14   /* ( n -- )        print signed decimal + space */
#define OP_DOTS  0x15   /* ( -- )          print entire stack non-destructively */

/* Memory */
#define OP_FETCH 0x16   /* @ ( addr -- n ) read 32-bit cell */
#define OP_STORE 0x17   /* ! ( n addr -- ) write 32-bit cell */

/* Return stack */
#define OP_TOR   0x18   /* >R ( n -- )  move TOS to return stack */
#define OP_RFROM 0x19   /* R> ( -- n )  move top of return stack to data stack */

/* Output */
#define OP_CR    0x1A   /* cr    ( -- )  emit newline */
#define OP_SPACE 0x1B   /* space ( -- )  emit space  */

/* Stack inspection */
#define OP_DEPTH  0x24   /* depth ( -- n )      number of items on the stack */

/* Arithmetic (step 1) */
#define OP_ABS    0x25   /* abs   ( n -- u )          absolute value */
#define OP_MAX    0x26   /* max   ( a b -- max )       signed maximum */
#define OP_MIN    0x27   /* min   ( a b -- min )       signed minimum */
#define OP_MOD    0x28   /* mod   ( a b -- rem )       signed remainder (truncated) */
#define OP_DIVMOD 0x29   /* /mod  ( a b -- rem quot )  remainder and quotient */

/* Bitwise (step 2) */
#define OP_AND    0x2A   /* and    ( a b -- n )   bitwise AND */
#define OP_OR     0x2B   /* or     ( a b -- n )   bitwise OR */
#define OP_XOR    0x2C   /* xor    ( a b -- n )   bitwise XOR */
#define OP_INVERT 0x2D   /* invert ( a -- ~a )    bitwise complement */
#define OP_LSHIFT 0x2E   /* lshift ( n u -- n' )  logical left shift */
#define OP_RSHIFT 0x2F   /* rshift ( n u -- n' )  logical right shift */

/* Comparison (step 3) */
#define OP_ZEROLT 0x30   /* 0<  ( n -- flag )     true if n < 0 */
#define OP_ZEROGT 0x31   /* 0>  ( n -- flag )     true if n > 0 */
#define OP_NEQ    0x32   /* <>  ( a b -- flag )   not equal */
#define OP_ULT    0x33   /* u<  ( u1 u2 -- flag ) unsigned less-than */

/* Stack (step 4) */
#define OP_ROT    0x34   /* rot   ( a b c -- b c a )              */
#define OP_QDUP   0x35   /* ?dup  ( n -- n n | 0 )  dup if nonzero */
#define OP_2SWAP  0x36   /* 2swap ( a b c d -- c d a b )          */
#define OP_2OVER  0x37   /* 2over ( a b c d -- a b c d a b )      */
#define OP_PICK   0x38   /* pick  ( xu..x0 u -- xu..x0 xu )       */
#define OP_ROLL   0x39   /* roll  ( xu..x0 u -- xu-1..x0 xu )     */

/* Return stack (step 5) */
#define OP_RFETCH 0x3A   /* r@   ( -- n )   copy top of return stack, non-destructive */
#define OP_EXIT   0x3B   /* exit ( -- )     early return from word (same as OP_RET)   */
#define OP_AGAIN  0x3C   /* again IMMEDIATE — emit JMP back to begin address          */

/* Byte memory + +! (step 6) */
#define OP_CFETCH 0x3D   /* c@  ( addr -- char )   fetch byte                        */
#define OP_CSTORE 0x3E   /* c!  ( char addr -- )   store low byte                    */
#define OP_PLUSST 0x3F   /* +!  ( n addr -- )      add n to cell at addr             */

/* Dictionary access (step 7) */
#define OP_HERE   0x40   /* here  ( -- addr )      push HERE (next free byte)        */
#define OP_ALLOT  0x41   /* allot ( n -- )         advance HERE by n bytes           */
#define OP_COMMA  0x42   /* ,     ( n -- )         store cell at HERE, HERE += 4     */
#define OP_CCOMMA 0x43   /* c,    ( char -- )      store byte at HERE, HERE += 1     */
#define OP_COUNT  0x44   /* count ( c-addr -- c-addr+1 u )  counted string length    */

/* Compile state control (step 8) */
#define OP_LBRACKET 0x45 /* [         IMMEDIATE — enter interpret mode (STATE=0)     */
#define OP_RBRACKET 0x46 /* ]         enter compile mode (STATE=1)                   */
#define OP_LITERAL  0x47 /* literal   IMMEDIATE — pop val, emit OP_LIT+val           */
#define OP_SETIMM   0x48 /* immediate set F_IMMEDIATE on most recent word            */

/* String and memory (step 9) */
#define OP_TYPE    0x49  /* type   ( c-addr u -- )       print u chars from c-addr   */
#define OP_ACCEPT  0x4A  /* accept ( c-addr +n -- +n )   read chars from stdin       */
#define OP_MOVE    0x4B  /* move   ( addr1 addr2 u -- )  copy u bytes (memmove)      */
#define OP_FILL    0x4C  /* fill   ( c-addr u char -- )  fill u bytes with char      */

/* DO/LOOP (step 10) */
/* Compile-time IMMEDIATE handlers — run at compile time, emit runtime opcodes */
#define OP_DO      0x4D   /* do    IMMEDIATE: emit OP_DO_RT + push 3 compile slots    */
#define OP_QDO     0x4E   /* ?do   IMMEDIATE: emit OP_QDO_RT + skip patch + 3 slots  */
#define OP_LOOP    0x4F   /* loop  IMMEDIATE: emit OP_LOOP_RT + body, patch chains    */
#define OP_PLOOP   0x50   /* +loop IMMEDIATE: emit OP_PLOOP_RT + body, patch chains  */
#define OP_LEAVE   0x51   /* leave IMMEDIATE: emit OP_LEAVE_RT, thread leave chain   */

/* Runtime opcodes (emitted into compiled words by the IMMEDIATE handlers above) */
#define OP_DO_RT     0x52  /* pop index+limit from DS; push limit,index onto RS       */
#define OP_QDO_RT    0x53  /* same as DO_RT; skip loop if limit==index (4-byte op)   */
#define OP_LOOP_RT   0x54  /* increment index; branch back (4-byte op) or drop frame */
#define OP_PLOOP_RT  0x55  /* pop n; step index; XOR boundary check; branch or drop  */
#define OP_LEAVE_RT  0x56  /* drop loop frame (rsp-=2); jump to 4-byte op (exit addr)*/

/* Pure runtime primitives */
#define OP_I       0x57   /* i      ( -- n )  copy loop index (rs[rsp-1])            */
#define OP_J       0x58   /* j      ( -- n )  copy outer index (rs[rsp-3])           */
#define OP_UNLOOP  0x59   /* unloop ( -- )    drop loop frame: rsp -= 2              */

/* Double-cell arithmetic (step 11) */
#define OP_S2D       0x5A  /* s>d     ( n -- lo hi )               sign-extend single to double */
#define OP_UMSTAR    0x5B  /* um*     ( u1 u2 -- lo hi )           unsigned 32×32→64 multiply   */
#define OP_MSTAR     0x5C  /* m*      ( n1 n2 -- lo hi )           signed 32×32→64 multiply     */
#define OP_UMDIVMOD  0x5D  /* um/mod  ( lo hi u -- rem quot )      unsigned 64÷32→32,32         */
#define OP_FMDIVMOD  0x5E  /* fm/mod  ( lo hi n -- rem quot )      floored signed division      */
#define OP_SMDIVREM  0x5F  /* sm/rem  ( lo hi n -- rem quot )      symmetric signed division    */
#define OP_DPLUS     0x60  /* d+      ( lo1 hi1 lo2 hi2 -- lo hi ) double-cell addition         */
#define OP_DNEGATE   0x61  /* dnegate ( lo hi -- lo' hi' )         double-cell negation         */
#define OP_INCLUDE   0x62  /* include-file ( addr u -- ) load Forth file by path */
#define OP_SOURCE    0x63  /* source ( -- addr u ) TIB address and current line length */
#define OP_PARSENAME 0x64  /* parse-name ( -- addr u ) parse next whitespace-delimited token from TIB */
#define OP_WORD  0x65   /* ( char -- c-addr ) parse counted string at HERE (transient) */
#define OP_CHAR  0x66   /* ( -- char ) first char of next space-delimited token */
#define OP_FIND  0x67   /* ( c-addr -- c-addr 0 | xt 1 | xt -1 ) */
#define OP_EXECUTE  0x68   /* ( xt -- ) execute word at body address */
#define OP_TONUMBER 0x69   /* ( ud c-addr u -- ud' c-addr' u' ) */
#define OP_QUIT     0x6A   /* quit: escape to REPL top via longjmp */
#define OP_EVALUATE  0x6B   /* ( addr u -- ) execute string as Forth */
#define OP_CREATE        0x6C   /* ( -- ) parse name, create dict entry + data area */
#define OP_BACKSLASH     0x6D   /* \ ( -- ) line comment: set >IN = SRCLEN */
#define OP_COMPILE_CALL  0x6E   /* compile, ( xt -- ) emit OP_CALL+xt at HERE */
#define OP_CONSTANT      0x6F   /* constant: parse name from TIB, pop val, create constant */
#define OP_COLON_RT      0x70   /* runtime ':': parse name, dict_create(F_HIDDEN), STATE=1 */
#define OP_SEMICOLON_RT  0x71   /* runtime ';': emit RET, un-hide latest, STATE=0 */
#define OP_DOES_CALL     0x72   /* body of CREATE'd word: read 4B does_ptr, push data_addr */
#define OP_DOES_SETUP    0x73   /* runtime: read 4B runtime_addr, patch latest's does_ptr  */
#define OP_DOES_IMM      0x74   /* IMMEDIATE compile-time action of DOES>                  */
#define OP_UDDIVMOD      0x75   /* ud/mod ( lo hi u -- rem lo' hi' ) 64÷32→64r32 unsigned  */

/* Arithmetic sugar — promoted from kernel.fth */
#define OP_ONEPLUS   0x76  /* 1+  ( n -- n+1 )        */
#define OP_ONEMINUS  0x77  /* 1-  ( n -- n-1 )        */
#define OP_TWOSTAR   0x78  /* 2*  ( n -- n*2 )        */
#define OP_TWOSLASH  0x79  /* 2/  ( n -- n/2, arith ) */

/* Address/cell arithmetic — promoted from kernel.fth */
#define OP_CELLPLUS  0x7A  /* cell+   ( addr -- addr+4 ) */
#define OP_CELLS     0x7B  /* cells   ( n -- n*4 )       */
#define OP_CHARPLUS  0x7C  /* char+   ( addr -- addr+1 ) */
#define OP_CHARS     0x7D  /* chars   ( n -- n )  no-op  */
#define OP_ALIGNED   0x7E  /* aligned ( addr -- a-addr ) */
#define OP_ALIGN     0x7F  /* align   ( -- )             */

/* Turnkey image support */
#define OP_BYE        0x80  /* bye        ( -- )    exit(0)                        */
#define OP_SAVEIMAGE  0x81  /* save-image ( -- )    write forth.img, turnkey_xt=0  */
#define OP_TURNKEY    0x82  /* turnkey    ( xt -- ) write forth.img, turnkey_xt=xt */
#define OP_LATEST     0x83  /* latest     ( -- addr ) push address of most recent dict entry */
#define OP_SETHERE    0x84  /* set-here   ( addr -- ) set vm->here                          */
#define OP_SETLATEST  0x85  /* set-latest ( addr -- ) set vm->latest                        */

/* Graphics and input (vm32g additions) */
#define OP_DRAW_PIXEL  0x86  /* draw-pixel  ( x y -- )        */
#define OP_DRAW_LINE   0x87  /* draw-line   ( x1 y1 x2 y2 -- )*/
#define OP_DRAW_RECT   0x88  /* draw-rect   ( x y w h -- )    */
#define OP_DRAW_CIRCLE 0x89  /* draw-circle ( x y r -- )      */
#define OP_SET_COLOR   0x8A  /* set-color   ( r g b a -- )    */
#define OP_CLEAR       0x8B  /* clear       ( -- )            */
#define OP_CANVAS_W    0x8C  /* canvas-w    ( -- w )          */
#define OP_CANVAS_H    0x8D  /* canvas-h    ( -- h )          */
#define OP_KEY_PRESSED 0x8E  /* key-pressed?( -- flag )       */
#define OP_LAST_KEY    0x8F  /* last-key    ( -- keycode )    */
#define OP_KEY_DOWN    0x90  /* key-down?   ( keycode -- flag)*/
#define OP_SIN_DEG     0x91  /* sin-deg     ( deg -- n*1000 ) */
#define OP_COS_DEG     0x92  /* cos-deg     ( deg -- n*1000 ) */

/* Compile-time control flow (IMMEDIATE words — run at compile time, not runtime)
 *
 * These opcodes manipulate HERE and the data stack to build JZ/JMP chains.
 * They are registered with F_IMMEDIATE so the compiler executes them instead
 * of inlining them.  The data stack is used as a compile-time scratchpad:
 *   if    pushes the address of its JZ operand (the "patch slot")
 *   then  pops that address and fills it with HERE
 *   else  pops if's slot, patches it, pushes a new slot for the JMP it emits
 *   begin pushes HERE (the loop-back target)
 *   until pops that target and emits JZ back to it
 */
#define OP_IF     0x1C
#define OP_THEN   0x1D
#define OP_ELSE   0x1E
#define OP_BEGIN  0x1F
#define OP_UNTIL  0x20
#define OP_WHILE  0x21  /* mid-test loop: emit JZ placeholder, leave begin_addr below */
#define OP_REPEAT 0x22  /* close while loop: emit JMP back to begin, patch while's JZ */

/* -------------------------------------------------------------------------
 * Memory constants
 * ------------------------------------------------------------------------- */

#define MEM_SIZE     (256 * 1024)   /* 262144 bytes total */
#define STACK_DEPTH  128            /* max cells per stack */

#define ADDR_STATE   0x0004         /* STATE:      0=interpret, 1=compile */
#define ADDR_BASE    0x0008         /* BASE:       number base (init=10) */
#define ADDR_IN      0x000C         /* >IN:        input position */
#define ADDR_SRCLEN  0x0010         /* SOURCE-LEN: input length */
#define ADDR_HLD     0x0014         /* picture hold pointer */
#define ADDR_SRCID   0x0018         /* source-id: 0=keyboard, -1=evaluate */
#define ADDR_SRCBASE 0x001C         /* source base address (SOURCE returns this) */
#define ADDR_PAD     0x00C0         /* top of picture buffer (grows down) */

#define TIB_BASE     0x0500         /* terminal input buffer */
#define TIB_SIZE     2048
#define DICT_BASE    0x0D00         /* dictionary starts here */

/* -------------------------------------------------------------------------
 * Word flags (stored in the flags byte of each dictionary entry)
 * ------------------------------------------------------------------------- */

#define F_IMMEDIATE  0x01   /* execute immediately even during compilation */
#define F_HIDDEN     0x02   /* not visible in dictionary search */
#define F_PRIMITIVE  0x04   /* single-opcode body — compiler inlines it directly */

/* -------------------------------------------------------------------------
 * VM state
 * ------------------------------------------------------------------------- */

typedef struct VM {
    uint8_t  mem[MEM_SIZE];     /* flat byte-addressed memory */

    int32_t  ds[STACK_DEPTH];   /* data stack — signed 32-bit cells */
    int      dsp;               /* data stack pointer: index of TOS, -1=empty */

    int32_t  rs[STACK_DEPTH];   /* return stack */
    int      rsp;               /* return stack depth (count of items) */

    uint32_t ip;                /* instruction pointer: byte offset into mem */
    uint32_t latest;            /* byte address of most recent dictionary entry */
    uint32_t here;              /* next free byte (Forth HERE) */

    jmp_buf  quit_jmp;          /* set by vm_repl; QUIT escapes to here */
    int      quit_active;       /* 1 if quit_jmp is valid */
    int      if_skip;           /* >0 = nesting depth of [IF] blocks being skipped */
    int      if_want_else;      /* 1 = stop at [ELSE] or [THEN] at depth 1; 0 = only [THEN] */
} VM;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Initialize VM, zero memory, register all built-in words */
void vm_init(VM *vm);

/* Dictionary lookup: returns entry address or 0 if not found */
uint32_t dict_find(VM *vm, const char *name, int len);

/* Return the byte address of a word's body (first bytecode byte) */
uint32_t dict_body(VM *vm, uint32_t entry);

/* Create a new dictionary entry; caller emits bytecode after */
void dict_create(VM *vm, const char *name, int len, uint8_t flags);

/* Emit bytes at HERE, advance HERE */
void emit_byte(VM *vm, uint8_t b);
void emit_u32(VM *vm, uint32_t v);

/* Data stack helpers */
void    dpush(VM *vm, int32_t v);
int32_t dpop(VM *vm);
int32_t dpeek(VM *vm);

/* Run bytecode from vm->ip until OP_HALT or OP_RET with empty return stack */
void vm_run(VM *vm);

/* Evaluate one line of Forth source; returns 0 on success, -1 on error */
int vm_eval(VM *vm, const char *line);

/* Load and evaluate a file; returns 0 on success */
int vm_load(VM *vm, const char *path);

/* Interactive REPL */
void vm_repl(VM *vm);

#endif /* VM_H */
