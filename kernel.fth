\ kernel.fth — vm32k Forth library
\ Loaded at startup by main.c after vm_init().
\ Words here are pure Forth compositions of C primitives.
\ Dependency order: each word uses only words defined above it or C primitives.

\ ── Section 1: Stack conveniences ───────────────────────────────────────────
\ No dependencies on other kernel words.

: 2dup    ( a b -- a b a b )   over over ;
: 2drop   ( a b -- )           drop drop ;
: nip     ( a b -- b )         swap drop ;
: tuck    ( a b -- b a b )     swap over ;
: -rot    ( a b c -- c a b )   rot rot ;

\ ── Section 2: Number base ───────────────────────────────────────────────────
\ Depends on: base (C constant), ! (C primitive)

: hex     ( -- )   16 base ! ;
: decimal ( -- )   10 base ! ;

\ ── Section 3: I/O helpers ───────────────────────────────────────────────────
\ spaces must come before u.r and .r (which call it).

: .sl     ( -- )   .s cr ;
: spaces  ( n -- )   0 max begin dup while space 1- repeat drop ;
: erase   ( addr u -- )   0 fill ;

\ ── Section 4: File loading ──────────────────────────────────────────────────
\ Depends on: parse-name include-file (C primitives)

: include   parse-name include-file ;

\ ── Section 5: Math compositions ─────────────────────────────────────────────
\ Depends on: m* sm/rem cell+ @ ! (C primitives)

: */mod   ( n1 n2 n3 -- rem quot )  >r m* r> sm/rem ;
: */      ( n1 n2 n3 -- quot )       */mod swap drop ;
: 2@      ( a-addr -- x1 x2 )        dup cell+ @ swap @ ;
: 2!      ( x1 x2 a-addr -- )        swap over ! cell+ ! ;

\ ── Section 6: Pictured numeric output ──────────────────────────────────────
\ Define in this order — each word depends on the ones above it.
\ Depends on: hld pad (C constants), ud/mod base (C primitives)
\             0< abs negate type space (C primitives)
\ Note: . here shadows the C primitive . after #> is defined.

: <#    ( -- )            pad hld ! ;
: hold  ( char -- )       hld @ 1- dup hld !  c! ;
: #     ( ud -- ud' )     base @ ud/mod rot 9 over < if 7 + then 48 + hold ;
: #s    ( ud -- 0 0 )     begin # 2dup or 0= until ;
: sign  ( n -- )          0< if 45 hold then ;
: #>    ( ud -- addr u )  drop drop hld @ pad over - ;
: u.    ( u -- )          0 <# #s #> type space ;
: .     ( n -- )          dup abs 0 <# #s rot sign #> type space ;
: u.r   ( u width -- )    >r 0 <# #s #> r> over - spaces type ;
: .r    ( n width -- )    >r dup abs 0 <# #s rot sign #> r> over - spaces type ;

\ ── Section 7: ANS compatibility words ──────────────────────────────────────
\ Depends on: quit evaluate >in word find execute (C primitives)

: bl          32 ;
: '           bl word find drop ;
: abort       quit ;
: >body       5 + ;        \ xt -> data-field (5 = OP_DOES_CALL + 4-byte slot)
: environment?   2drop  0 ;
: source-id   0x18 @ ;     \ reads ADDR_SRCID (0x18); 0=keyboard, -1=file

\ ── Section 8: Marker (ANS Forth) ───────────────────────────────────────────
\ Depends on: here latest set-here set-latest create does> , cell+ @ (C primitives)
\ marker saves here+latest at definition time; invoking the word restores both,
\ erasing itself and everything defined after it.

: marker
  here latest
  create , ,
  does>
    dup @ swap cell+ @
    set-here set-latest ;

\ ── Section 9: Dictionary inspection ────────────────────────────────────────
\ Depends on: latest (C word), c@, @, type, space (C primitives)
\ Dictionary entry layout: [link:4][flags:1][namelen:1][name:N][body...]

: entry-flags    ( e -- flags )    4 + c@ ;
: entry-namelen  ( e -- u )        5 + c@ ;
: entry-name     ( e -- c-addr u ) dup 6 + swap entry-namelen ;

2 constant F_HIDDEN

: words
  latest
  begin dup while
    dup entry-flags F_HIDDEN and 0= if
      dup entry-name type space
    then
    @
  repeat
  drop ;
