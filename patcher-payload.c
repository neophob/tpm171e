#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "patcher-payload.h"

#define LOGFILE "/data/local/tmp/patcher-payload.log"
#define LOGFILE_MODE "w" // fopen() mode; 'a'ppend or 'w'rite (with truncation).

#define OWN_LIBNAME "patcher-payload" // Our own name, for skipping the GOT patching.

#define MAIN_PROCESS_NAME "dtv_svc" // Name of the main binary (dl* just return "").

#define UNUSED __attribute__((unused))

// Log file, opened in init().
static FILE *logf = NULL;

// A machine word.
typedef uint32_t word;

// Log handler; to be used with the log() macro that passes file and line.
__attribute__((format(printf, 3, 4)))
static void do_log(const char *filename, int lineno, const char *format, ...) {
    if (!logf)
        return;

    char tsbuf[256] = {0};
    time_t t = time(NULL);
    if (!strftime(tsbuf, sizeof(tsbuf)-1, "[%F %T] ", localtime(&t)))
        tsbuf[0] = '\0';
    fprintf(logf, "%s%s:%d: ", tsbuf, filename, lineno);

    va_list args;
    va_start(args, format);
    vfprintf(logf, format, args);
    va_end(args);

    if (*format && format[strlen(format)-1] != '\n')
        fputc('\n', logf);

    fflush(logf);
}

// Log with current filename and line.
#define log(...) do_log(__FILE__, __LINE__, __VA_ARGS__)

// A request to patch each loaded library’s GOT with pointers to oldval pointing to newval.
struct patch_got_req {
    word oldval;
    word newval;
};

// An item in the undo_list, for unpatching GOTs in fini().
struct undo_item {
    word *addr;
    word oldval;
};

// Patched words with their old value.
#define UNDO_LIST_SIZE 256
static struct undo_item undo_list[UNDO_LIST_SIZE];

// Number of undo_list elements populated. Top of stack is undo_list[undo_size-1].
static unsigned int undo_size = 0;

// Define a function to be overridden. The original function is available as orig_symbol, this
// macro emits the declaration for my_symbol.
#define DEFINE_OVERRIDE(rettype, symname, ...) \
    static rettype (*orig_##symname)(__VA_ARGS__) = NULL; \
    \
    static rettype my_##symname(__VA_ARGS__)

// Initializes an override defined by DEFINE_OVERRIDE.
#define INIT_OVERRIDE(libname, symname) \
    do_override(libname, #symname, (void (**)())&orig_##symname, my_##symname)

// Override a_mtktvapi_config_get_value and print args and return value.
DEFINE_OVERRIDE(char *, a_mtktvapi_config_get_value, int16_t grp, char *cfg, int32_t *value) {
    char *ret = orig_a_mtktvapi_config_get_value(grp, cfg, value);
    log("a_mtktvapi_config_get_value(grp=%d, cfg=%s, *value=%ld) = %s", grp, cfg, (value?*value:0xDEAD), ret);
    return ret;
}

DEFINE_OVERRIDE(int32_t, mtktvapi_config_custom_map_id_2_string, int32_t id, char *string) {
    int32_t ret = orig_mtktvapi_config_custom_map_id_2_string(id, string);
    log("mtktvapi_config_custom_map_id_2_string(%ld, %s) = %ld", id, string, ret);
    return ret;
}

// Patch a GOT as described in req. The GOT is expected to be within start-end and contain the
// value to patch at most once. (We can’t really easily tell where the GOT in the ELF segment
// is, so we search all of it and if we found the value twice, the chances of either of them
// being some other random data is nonzero.)
static int patch_got(word *start, word *end, const char *filename, struct patch_got_req *req) {
    if (strstr(filename, OWN_LIBNAME))
        return 0;

    if (!*filename)
        filename = MAIN_PROCESS_NAME;

    word *off = NULL;

    for (word *p = start; p < end; p++) {
        if (*p != req->oldval)
            continue;

        if (off) {
            log("patch_got(%p, %p, \"%s\"): found old value twice (at %p and at %p), not patching.",
                    start, end, filename, off, p);
            return -1;
        }
        off = p;
    }

    if (!off)
        return 0;

    if (++undo_size > sizeof(undo_list)) {
        log("Can't store more than %d undo items, increase UNDO_LIST_SIZE.", sizeof(undo_list));
        return -1;
    }

    undo_list[undo_size-1].addr = off;
    undo_list[undo_size-1].oldval = *off;
    *off = req->newval;
    log("Patched GOT of %s at %p (from 0x%08lx to 0x%08lx)", filename, off,
            undo_list[undo_size-1].oldval, *off);

    return 0;
}

// Callback for dl_iterate_phdr. Walks over all ELF segments and calls patch_got for each that
// is likely to contain a GOT (i.e. is of type LOAD and read- and writable).
static int find_got_phdr(struct dl_phdr_info *info, size_t size UNUSED, struct patch_got_req *req) {
    for (int i = 0; i < info->phnum; i++) {
        if (info->phdr[i].type != PT_LOAD || (info->phdr[i].flags & PF_RW) != PF_RW)
            continue;

        void *start = info->addr + info->phdr[i].vaddr;
        void *end = start + info->phdr[i].memsz;

        if (patch_got(start, end, info->name, req) < 0)
            return -1;
    }

    return 0;
}

// Overrides symname from libname with override, storing the original symbol address in *orig.
static void do_override(const char *libname, const char *symname, void (**orig)(), void *override) {
    if (*orig) {
        log("orig_%s already points to %p, not overriding again", symname, *orig);
        return;
    }

    void *hdl = dlopen(libname, RTLD_NOW|RTLD_NOLOAD);
    *orig = dlsym(hdl, symname);

    if (hdl)
        dlclose(hdl);

    if (!*orig) {
        log("Can't find %s in hdl %p (%s)", symname, hdl, libname);
        return;
    }

    log("Patching GOTs referencing %s = %p", symname, *orig);
    struct patch_got_req req = {
        .oldval = (word)*orig,
        .newval = (word)override,
    };
    dl_iterate_phdr(find_got_phdr, &req);
}

// Sets up logging and installs all hooks.
__attribute__((constructor)) static void init() {
    if (logf)
        fclose(logf);

    logf = fopen(LOGFILE, LOGFILE_MODE);
    if (!logf)
        return;

    log("Initializing");
    INIT_OVERRIDE("libmtkapp.so", a_mtktvapi_config_get_value);
    INIT_OVERRIDE("libmtkapp.so", mtktvapi_config_custom_map_id_2_string);
    log("Initialized");
}

// Uninstalls all hooks and closes the logfile.
__attribute__((destructor)) static void fini() {
    log("Tearing down.");

    while (undo_size > 0) {
        *(undo_list[undo_size-1].addr) = undo_list[undo_size-1].oldval;
        log("Undid GOT patch at %p", undo_list[undo_size-1].addr);
        undo_size--;
    }

    if (fclose(logf) == 0)
        logf = NULL;
}
