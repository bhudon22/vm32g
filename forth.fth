\ forth.fth — vm32 self-tests
\
\ Loaded automatically at startup by main.c.
\ All word definitions live in vm.c (vm_init).  This file is tests only.
\
\ assert ( flag n -- )  silent on pass; prints "FAIL n" on failure.

: assert ( flag n -- )  swap 0= if 70 emit 65 emit 73 emit 76 emit space . cr else drop then ;

\ =========================================================================
\ Self-tests — 127 tests, 1 per assert, silent = all pass
\ =========================================================================

\ ---- Tests 1-5: arithmetic primitives -----------------------------------

2 3 +   5 =   1 assert
9 4 -   5 =   2 assert
3 4 *  12 =   3 assert
10 2 /  5 =   4 assert
3 negate  -3 =  5 assert

\ ---- Tests 6-9: arithmetic sugar ----------------------------------------

4 1+    5 =   6 assert
6 1-    5 =   7 assert
5 2*   10 =   8 assert
10 2/   5 =   9 assert

\ ---- Tests 10-13: comparison primitives ---------------------------------

5 5 =   -1 =  10 assert
4 5 <   -1 =  11 assert
5 4 >   -1 =  12 assert
0 0=    -1 =  13 assert

\ ---- Tests 14-22: stack primitives and derived words --------------------

1 dup      =  14 assert               ( dup: two copies equal )
1 2 swap  1 =  15 assert  drop        ( swap: top becomes 1 )
1 2 over  1 =  16 assert  2drop       ( over: copies second to top )
1 2 2dup  2 =  17 assert  1 =  18 assert  drop drop  ( 2dup: TOS=b NOS=a )
1 2 2drop  depth  0 =  19 assert      ( 2drop: stack empty )
1 2 3      depth  3 =  20 assert  2drop drop         ( depth: 3 items )
1 2 nip   2 =  21 assert              ( nip: removes second, leaves b )
1 2 tuck  2 =  22 assert  drop drop   ( tuck: b a b, TOS=b )

\ ---- Tests 23-26: control flow ------------------------------------------

: cf-if    1 if 42 else 0 then ;   cf-if  42 =  23 assert
: cf-else  0 if 0  else 42 then ;  cf-else 42 =  24 assert
: cf-begin  0 begin 1+ dup 5 = until ;  cf-begin  5 =  25 assert
: cf-while  0 begin dup 5 < while 1+ repeat ;  cf-while  5 =  26 assert

\ ---- Tests 27-34: abs max min mod /mod ----------------------------------

-5 abs    5 =  27 assert
 5 abs    5 =  28 assert
3 7 max   7 =  29 assert
3 7 min   3 =  30 assert
7 3 mod   1 =  31 assert
7 3 /mod  2 =  32 assert  1 =  33 assert  ( /mod: TOS=quot=2, then NOS=rem=1 )
-7 3 mod  -1 =  34 assert

\ ---- Tests 35-40: bitwise -----------------------------------------------

12 10 and   8 =  35 assert   ( 0b1100 & 0b1010 = 0b1000 = 8 )
12 10 or   14 =  36 assert   ( 0b1100 | 0b1010 = 0b1110 = 14 )
12 10 xor   6 =  37 assert   ( 0b1100 ^ 0b1010 = 0b0110 = 6 )
15 invert  -16 =  38 assert  ( ~0b00001111 = 0xFFFFFFF0 = -16 )
1 3 lshift  8 =  39 assert
8 3 rshift  1 =  40 assert

\ ---- Tests 41-46: 0< 0> <> u< true false not ---------------------------

-1 0<   -1 =  41 assert
 1 0<    0 =  42 assert
 1 0>   -1 =  43 assert
-1 0>    0 =  44 assert
1 2 <>  -1 =  45 assert
1 1 <>   0 =  46 assert

\ ---- Tests 47-51: true false not u< ------------------------------------

true   -1 =  47 assert
false   0 =  48 assert
0 not  -1 =  49 assert
1 not   0 =  50 assert
2 4 u<  -1 =  51 assert

\ ---- Tests 52-63: rot ?dup 2swap 2over pick roll -rot -------------------

1 2 3 rot    1 =  52 assert  3 =  53 assert  drop  ( rot: b c a -- TOS=a=1, then c=3 )
0 ?dup       0 =  54 assert                    ( ?dup 0: not duplicated )
5 ?dup       5 =  55 assert  5 =  56 assert         ( ?dup nonzero: both copies verified )
1 2 3 4 2swap  2 =  57 assert  1 =  58 assert  2drop  ( 2swap: a b c d -- c d a b, TOS=b=2, NOS=a=1 )
1 2 3 4 2over  2 =  59 assert  1 =  60 assert  2drop 2drop  ( 2over: a b c d -- a b c d a b, TOS=b=2 )
1 2 3  1 pick  2 =  61 assert  drop drop drop  ( pick: 1 pick = NOS = 2 )
1 2 3  1 roll  2 =  62 assert  3 =  63 assert  drop  ( roll: a b c 1 roll -- a c b, TOS=b=2, then c=3 )

\ ---- Tests 64-66: R@ EXIT AGAIN ----------------------------------------

: cf-rat   1 >r r@ r> + ;  cf-rat  2 =  64 assert
: cf-exit  1 if 42 exit then 0 ;  cf-exit  42 =  65 assert
: cf-again  0 begin 1+ dup 3 = if exit then again ;  cf-again  3 =  66 assert

\ ---- Tests 67-72: C@ C! +! ---------------------------------------------

here dup  0x41 swap c!  c@  0x41 =  67 assert    ( store/fetch byte )
here  5 swap c!  here  3 swap +!  here c@  8 =  68 assert  ( +! adds to byte )
here  100 swap !  here  50 swap +!  here @  150 =  69 assert  ( +! on cell )
0x42 c,  here 1-  c@  0x42 =  70 assert              ( c, appends byte at HERE )
0x1234 ,  here 4 -  @  0x1234 =  71 assert          ( , appends cell at HERE )

\ ---- Tests 73-75: aligned align cell+ cells char+ chars -----------------

3 aligned   4 =  72 assert
4 aligned   4 =  73 assert
1 cells     4 =  74 assert
1 chars     1 =  75 assert

\ ---- Tests 76-80: state base hex decimal --------------------------------

base @  10 =  76 assert                ( default base is decimal )
hex   base @  0x10 =  77 assert
decimal  base @  10 =  78 assert
state @  0 =  79 assert               ( interpret mode )

\ ---- Tests 80-83: [ ] literal -------------------------------------------

: cf-lit  [ 6 7 * ] literal ;  cf-lit  42 =  80 assert

\ ---- Tests 81-88: here allot count --------------------------------------

here  4 allot  here swap -  4 =  81 assert    ( allot advances HERE by 4 )
here  5 over c!  count  5 =  82 assert  drop   ( count: TOS = byte at addr )
here  1 over c!  count  1 =  83 assert  drop   ( count with byte=1 returns 1 )

\ ---- Tests 84-88: type move fill erase spaces ---------------------------

here  0x41 over c!  here 1  type  drop  ( prints A — visual check only )
here  0x0A over c!  here 1  type  drop  ( prints newline )
\ test 84 removed (TYPE has no stack effect; visual check on lines above suffices)

here  0x48 over c!  dup  dup 1+  1 move  1+ c@  0x48 =  85 assert  ( move: copy 1 byte src→dest )

here  3  0xAB  fill  here c@  0xAB =  86 assert  ( fill: c-addr u char -- )
here 1+  c@  0xAB =  87 assert
here 2 +  c@  0xAB =  88 assert

here  4  erase  here c@  0 =  89 assert     ( erase zeroes bytes )

3 spaces  depth  0 =  90 assert             ( spaces leaves stack clean )

\ ---- Tests 91-99: DO LOOP +LOOP I J LEAVE ?DO UNLOOP ------------------

\ Basic DO LOOP with I
: cf-do1   0  5 0 do  i +  loop ;   cf-do1   10 =  91 assert   ( 0+1+2+3+4=10 )

\ +LOOP with step 2
: cf-ploop  0  10 0 do  i +  2 +loop ;  cf-ploop  20 =  92 assert  ( 0+2+4+6+8=20 )

\ Nested loops — J gives outer index
: cf-nest   0  3 0 do  3 0 do  j i +  + loop loop ;  cf-nest  18 =  93 assert
\ sum of i+j for i,j in 0..2: 3+6+9=18

\ LEAVE exits early — jumps past LOOP immediately
: cf-leave  0  10 0 do  i 5 = if leave then  i +  loop ;  cf-leave  10 =  94 assert
  ( 0+1+2+3+4=10; LEAVE fires at i=5, body skipped for i=5 )

\ ?DO skips when limit = index
: cf-qdo0   99  5 5 ?do  drop 0  loop ;   cf-qdo0  99 =  95 assert  ( body skipped )

\ ?DO runs when limit != index
: cf-qdo1   0  3 0 ?do  i +  loop ;   cf-qdo1  3 =  96 assert   ( 0+1+2=3 )

\ UNLOOP + EXIT inside a loop
: cf-unloop  10 0 do  i 3 = if unloop 42 exit then  loop  0 ;
  cf-unloop  42 =  97 assert

\ Multiple LEAVEs — exercises full chain-patching walk
: cf-leave2  0  10 0 do
    i 3 = if leave then
    i 7 = if leave then
    i +
  loop ;
  cf-leave2  3 =  98 assert   ( exits at i=3: 0+1+2=3 )

\ +LOOP with negative step  ( limit=0 index=4 step=-1: runs i=4,3,2,1,0 )
: cf-neg  0  0 4 do  i +  -1 +loop ;  cf-neg  10 =  99 assert  ( 4+3+2+1+0=10 )

\ ---- Tests 100-103: S>D ---------------------------------------------------

5 s>d  0 = 100 assert  5 = 101 assert
-1 s>d  -1 = 102 assert  -1 = 103 assert

\ ---- Tests 104-109: UM* M* ------------------------------------------------

2 3 um*  0 = 104 assert  6 = 105 assert
0x80000000 2 um*  1 = 106 assert  0 = 107 assert
\ ^ 0x80000000 * 2 = 0x100000000 → hi=1, lo=0 (tests carry into hi word)

3 -2 m*  -1 = 108 assert  -6 = 109 assert
\ ^ -6 as int64: lo=-6, hi=-1 (sign-extended)

\ ---- Tests 110-111: UM/MOD ------------------------------------------------

7 0 3 um/mod  2 = 110 assert  1 = 111 assert
\ ^ ud=7, u=3 → quot=2, rem=1. Stack: ( rem quot ) → TOS=quot=2

\ ---- Tests 112-115: FM/MOD ------------------------------------------------

7 0 2 fm/mod   3 = 112 assert  1 = 113 assert
\ ^ d=7, n=2 → quot=3, rem=1 (no adjustment needed — both positive)

-7 -1 2 fm/mod  -4 = 114 assert  1 = 115 assert
\ ^ d=-7 (lo=-7 hi=-1), n=2 → floored: quot=-4, rem=1
\ (C truncated gives quot=-3, rem=-1; floor adjustment: rem+=2, quot--)

\ ---- Tests 116-119: SM/REM ------------------------------------------------

7 0 2 sm/rem   3 = 116 assert  1 = 117 assert
-7 -1 2 sm/rem  -3 = 118 assert  -1 = 119 assert
\ ^ d=-7, n=2 → truncated: quot=-3, rem=-1 (no adjustment — C does this natively)

\ ---- Tests 120-123: D+ ---------------------------------------------------

1 0 2 0 d+  0 = 120 assert  3 = 121 assert
\ ^ 1 + 2 = 3 as double: hi=0 lo=3

0xFFFFFFFF 0 1 0 d+  1 = 122 assert  0 = 123 assert
\ ^ 0xFFFFFFFF + 1 = 0x100000000: hi=1 lo=0 — tests carry propagation

\ ---- Tests 124-127: DNEGATE -----------------------------------------------

1 0 dnegate  -1 = 124 assert  -1 = 125 assert
\ ^ -1 as double: lo=-1 hi=-1 (0xFFFFFFFF_FFFFFFFF)

0 0 dnegate   0 = 126 assert   0 = 127 assert
\ ^ negating zero double gives zero double

\ ---- Tests 128-135: u. and . (decimal) -----------------------------------

\ u. leaves stack clean
0 u.  depth 0 =  128 assert
42 u.  depth 0 =  129 assert

\ . handles zero, positive, negative
0 .  depth 0 =  130 assert
42 .  depth 0 =  131 assert
-1 .  depth 0 =  132 assert

\ u. is BASE-aware
hex  255 u.  depth 0 =  133 assert
decimal

\ . leaves stack clean with negative and large positive
-42 .  depth 0 =  134 assert
0x7FFFFFFF .  depth 0 =  135 assert

\ ---- Tests 136-139: u.r and .r (right-justified) -------------------------

42 6 u.r  depth 0 =  136 assert
0  3 u.r  depth 0 =  137 assert
-42 6 .r  depth 0 =  138 assert
42  6 .r  depth 0 =  139 assert

\ ---- Tests 140-143: <# # #s #> directly ---------------------------------

42 0 <# #s #>  2 =  140 assert
drop

hex
0xFF 0 <# #s #>  2 =  141 assert
drop decimal

1   0 <# #s -1 sign #>  2 =  142 assert   ( "1" + "-" = length 2 )
drop
1   0 <# #s  1 sign #>  1 =  143 assert   ( positive: no sign added )
drop

\ ---- Tests 144-145: bl and >in ------------------------------------------
bl  32 =  144 assert
>in  0x0c =  145 assert    \ >in returns address 0x0c (ADDR_IN cell address)

\ ---- Test 146: source ----------------------------------------------------
source nip  0 >  146 assert    \ source gives nonzero length on a non-empty line

\ ---- Test 147: include + parse-name ------------------------------------
include test_include.fth
included-word  77 =  147 assert

\ ---- Tests 148-150: word char [char] ------------------------------------
bl word x c@ 1 =  148 assert        \ "x": count byte = 1
char A  65 =  149 assert
: test-lchar  [char] Z ;   test-lchar  90 =  150 assert

\ ---- Tests 151-152: find -----------------------------------------------
bl word bl find  nip  0 <>  151 assert   \ "bl" found: flag nonzero (nip drops xt)
bl word bl find  drop  0 <>  152 assert  \ xt returned is a nonzero address

\ ---- Tests 153-155: execute, tick, ['] ----------------------------------
bl word bl find drop execute  32 =  153 assert   \ execute bl → pushes 32
' bl execute  32 =  154 assert                   \ tick + execute
: test-bracket-tick  ['] bl execute ;
test-bracket-tick  32 =  155 assert              \ ['] + execute

\ ---- Tests 156-157: >number ---------------------------------------------
0 0 s" 123" >number  nip  0 =  156 assert    \ u2=0: all chars consumed
2drop                                         \ clean up lo hi left by >number
0 0 s" 123" >number  2drop drop  123 =  157 assert  \ ud lo-cell = 123

\ ---- Tests 158-159: abort and recurse -----------------------------------
bl word abort find  nip  0 <>  158 assert  \ abort is in the dictionary (nip drops xt)
: count-down  dup if dup 1- recurse then ;
3 count-down depth  4 =  159 assert    \ stack has 3 2 1 0 (depth=4)
drop drop drop drop                    \ clean up the 3 2 1 0

\ ---- Test 160: evaluate -------------------------------------------------
s" 3 4 +" evaluate  7 =  160 assert

\ ---- Tests 161-162: environment? and source-id --------------------------
s" /COUNTED-STRING" environment?  0=  161 assert
source-id  0 =  162 assert    \ 0 = keyboard during normal execution

\ ---- Test 163: create ---------------------------------------------------
create test-create-arr 8 allot   \ allocates 8 bytes at data area
test-create-arr 0 >  163 assert  \ address returned is > 0

\ ---- Tests 164-165: variable and constant -------------------------------
variable test-var
test-var @ 0 =  164 assert       \ variable initialized to 0
42 constant test-const
test-const 42 =  165 assert      \ constant returns its value

\ ---- Tests 166-167: [if] [else] [then] ---------------------------------
0 [if] 99 [else] 0 [then]  0 =  166 assert   \ false: takes [else] path
-1 [if] -1 [else] 99 [then]  -1 =  167 assert \ true: skips [else] path
