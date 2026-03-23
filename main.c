/*
 * main.c — entry point for the 32-bit Forth VM
 *
 * Boot sequence (in priority order):
 *   1. Image appended to the binary itself (turnkey executable)
 *   2. forth.img side-car file in the current directory
 *   3. Normal startup: vm_init() + kernel.fth + REPL
 *
 * Image format (FIMG):
 *   offset 0:   4 bytes  magic 'F','I','M','G'
 *   offset 4:   4 bytes  version = 1 (uint32_t)
 *   offset 8:   4 bytes  here (uint32_t)
 *   offset 12:  4 bytes  latest (uint32_t)
 *   offset 16:  4 bytes  turnkey_xt (uint32_t, 0 = no start word)
 *   offset 20:  here bytes  mem[0..here]
 */

#include "vm.h"
#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <mach-o/dyld.h>

#define IMAGE_HEADER_SIZE 20

/* -------------------------------------------------------------------------
 * init_vm_state — initialise non-memory fields after loading an image.
 * (Skips vm_init() which would also zero mem and register primitives.)
 * ------------------------------------------------------------------------- */
static void init_vm_state(VM *vm)
{
    vm->dsp          = -1;
    vm->rsp          = 0;
    vm->ip           = 0;
    vm->quit_active  = 0;
    vm->if_skip      = 0;
    vm->if_want_else = 0;
}

/* -------------------------------------------------------------------------
 * try_load_image — read and validate a FIMG image from file f at offset.
 * Returns 0 on success, -1 on failure.
 * ------------------------------------------------------------------------- */
static int try_load_image(VM *vm, FILE *f, long image_offset, uint32_t *out_xt)
{
    uint8_t header[IMAGE_HEADER_SIZE];

    if (fseek(f, image_offset, SEEK_SET) != 0)
        return -1;

    if (fread(header, 1, IMAGE_HEADER_SIZE, f) != IMAGE_HEADER_SIZE)
        return -1;

    /* Validate magic */
    if (header[0] != 'F' || header[1] != 'I' || header[2] != 'M' || header[3] != 'G')
        return -1;

    /* Validate version */
    uint32_t version;
    __builtin_memcpy(&version, header + 4, 4);
    if (version != 1)
        return -1;

    /* Read here, latest, turnkey_xt from header */
    uint32_t here, latest, turnkey_xt;
    __builtin_memcpy(&here,        header + 8,  4);
    __builtin_memcpy(&latest,      header + 12, 4);
    __builtin_memcpy(&turnkey_xt,  header + 16, 4);

    /* Validate here */
    if (here == 0 || here > MEM_SIZE)
        return -1;

    /* Read memory image */
    if (fread(vm->mem, 1, here, f) != here)
        return -1;

    vm->here   = here;
    vm->latest = latest;
    init_vm_state(vm);
    *out_xt = turnkey_xt;
    return 0;
}

/* -------------------------------------------------------------------------
 * try_load_file — try to load "forth.img" from the current directory.
 * Returns 0 on success, -1 if not found or invalid.
 * ------------------------------------------------------------------------- */
static int try_load_file(VM *vm, uint32_t *out_xt)
{
    FILE *f = fopen("forth.img", "rb");
    if (!f)
        return -1;

    int result = try_load_image(vm, f, 0, out_xt);
    fclose(f);
    return result;
}

/* -------------------------------------------------------------------------
 * try_load_appended — try to read an image appended to the binary itself.
 * Returns 0 on success, -1 if no appended image or on error.
 * ------------------------------------------------------------------------- */
static int try_load_appended(VM *vm, uint32_t *out_xt, char *argv0)
{
    /* Get the executable path */
    char exe_path[4096];
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) != 0) {
        /* Buffer too small or error — fall back to argv[0].
         * Note: argv[0] is a best-effort heuristic (not a reliable absolute path),
         * used only when _NSGetExecutablePath fails. */
        if (argv0) {
            size_t len = 0;
            while (argv0[len] && len < sizeof(exe_path) - 1)
                len++;
            __builtin_memcpy(exe_path, argv0, len);
            exe_path[len] = '\0';
        } else {
            return -1;
        }
    }

    FILE *f = fopen(exe_path, "rb");
    if (!f)
        return -1;

    /* Get file size */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long file_size = ftell(f);
    if (file_size < 0 || file_size < IMAGE_HEADER_SIZE) { fclose(f); return -1; }

    /* Read the last IMAGE_HEADER_SIZE bytes and check magic */
    uint8_t tail[IMAGE_HEADER_SIZE];
    if (fseek(f, file_size - IMAGE_HEADER_SIZE, SEEK_SET) != 0) {
        fclose(f); return -1;
    }
    if (fread(tail, 1, IMAGE_HEADER_SIZE, f) != IMAGE_HEADER_SIZE) {
        fclose(f); return -1;
    }

    if (tail[0] != 'F' || tail[1] != 'I' || tail[2] != 'M' || tail[3] != 'G') {
        fclose(f); return -1;  /* not an appended image — not an error */
    }

    /* Extract here from the tail header to compute image_offset */
    uint32_t here;
    __builtin_memcpy(&here, tail + 8, 4);
    if (here == 0 || here > MEM_SIZE) { fclose(f); return -1; }

    long image_offset = file_size - IMAGE_HEADER_SIZE - (long)here;
    if (image_offset < 0) { fclose(f); return -1; }

    int result = try_load_image(vm, f, image_offset, out_xt);
    fclose(f);
    return result;
}

/* -------------------------------------------------------------------------
 * main — boot sequence
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    (void)argc;  /* argv needed for fallback path */

    VM *vm = malloc(sizeof(VM));
    if (!vm) {
        fputs("error: out of memory\n", stderr);
        return 1;
    }

    uint32_t turnkey_xt = 0;

    /* 1. Try appended image */
    if (try_load_appended(vm, &turnkey_xt, argv[0]) == 0) goto run_turnkey;

    /* 2. Try forth.img side-car */
    if (try_load_file(vm, &turnkey_xt) == 0) goto run_turnkey;

    /* 3. Normal startup */
    vm_init(vm);
    if (vm_load(vm, "kernel.fth") != 0)
        fputs("warning: kernel.fth not found — bare session (C primitives only)\n", stderr);
    gfx_run((struct VM *)vm);
    goto done;  /* must not fall through to run_turnkey: */

run_turnkey:
    if (turnkey_xt != 0) {
        vm->ip = turnkey_xt;
        vm_run(vm);
    } else {
        gfx_run((struct VM *)vm);
    }

done:
    free(vm);
    return 0;
}
