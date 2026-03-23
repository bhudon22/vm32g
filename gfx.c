#include "gfx.h"
#include <stdio.h>
void gfx_print(const char *s) { fputs(s, stdout); }
void gfx_run(struct VM *vm) { (void)vm; }
