#define _GNU_SOURCE
#include "pe.h"

#include <asm/prctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define DIE(...) do { fprintf(stderr, "peloader: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

#define RD16(p) (*(uint16_t *)(p))
#define RD32(p) (*(uint32_t *)(p))
#define RD64(p) (*(uint64_t *)(p))

enum { DIR_EXPORT = 0, DIR_IMPORT = 1, DIR_RELOC = 5, DIR_TLS = 9 };

static uint8_t *data_dir(pe_image *img, uint8_t *opt, int idx, uint32_t *size)
{
    uint32_t rva = RD32(opt + 112 + idx * 8);
    if (size) *size = RD32(opt + 112 + idx * 8 + 4);
    return rva ? img->base + rva : NULL;
}

/* Calls to imports we don't implement land here with the import's name. */
static void WINAPI unimplemented(const char *name)
{
    DIE("unimplemented import called: %s", name);
}

static void *make_trap(const char *dll, const char *name)
{
    static uint8_t *page;
    static size_t used = 4096;
    if (used + 32 > 4096) {
        page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        used = 0;
    }
    char *label;
    if (asprintf(&label, "%s!%s", dll, name) < 0) abort();
    uint8_t *t = page + used;
    t[0] = 0x48; t[1] = 0xb9; memcpy(t + 2, &label, 8);             /* mov rcx, label */
    void *fn = (void *)unimplemented;
    t[10] = 0x48; t[11] = 0xb8; memcpy(t + 12, &fn, 8);             /* mov rax, fn */
    t[20] = 0xff; t[21] = 0xe0;                                     /* jmp rax */
    used += 32;
    return t;
}

static void apply_relocs(pe_image *img, uint8_t *opt, int64_t delta)
{
    uint32_t size;
    uint8_t *p = data_dir(img, opt, DIR_RELOC, &size), *end = p + size;
    while (p && p < end) {
        uint32_t page = RD32(p), block = RD32(p + 4);
        for (uint32_t i = 8; i < block; i += 2) {
            uint16_t e = RD16(p + i);
            if (e >> 12 == 10) *(uint64_t *)(img->base + page + (e & 0xfff)) += delta;
            else if (e >> 12) DIE("unsupported relocation type %d", e >> 12);
        }
        p += block;
    }
}

static void resolve_imports(pe_image *img, uint8_t *opt)
{
    for (uint8_t *d = data_dir(img, opt, DIR_IMPORT, NULL); d && RD32(d + 12); d += 20) {
        const char *dll = (char *)img->base + RD32(d + 12);
        uint64_t *lookup = (uint64_t *)(img->base + (RD32(d) ? RD32(d) : RD32(d + 16)));
        uint64_t *iat = (uint64_t *)(img->base + RD32(d + 16));
        for (; *lookup; lookup++, iat++) {
            const char *name = NULL;
            uint16_t ord = 0;
            char ordname[16];
            if (*lookup >> 63) {
                ord = *lookup & 0xffff;
                snprintf(ordname, sizeof ordname, "#%u", ord);
            } else {
                name = (char *)img->base + (*lookup & 0x7fffffff) + 2;
            }
            void *fn = winapi_resolve(dll, name, ord);
            *iat = (uint64_t)(fn ? fn : make_trap(dll, name ? name : ordname));
        }
    }
}

static void protect_sections(pe_image *img, uint8_t *nt)
{
    uint16_t nsec = RD16(nt + 6), optsz = RD16(nt + 20);
    uint8_t *sec = nt + 24 + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t va = RD32(sec + 12), vsize = RD32(sec + 8), ch = RD32(sec + 36);
        int prot = PROT_READ;
        if (ch & 0x20000000) prot |= PROT_EXEC;
        if (ch & 0x80000000) prot |= PROT_WRITE;
        uintptr_t start = (uintptr_t)img->base + va, end = (start + vsize + 4095) & ~4095UL;
        mprotect((void *)start, end - start, prot);
    }
}

pe_image *pe_load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) DIE("cannot open %s", path);
    struct stat st;
    fstat(fd, &st);
    uint8_t *file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED || RD16(file) != 0x5a4d) DIE("%s is not a PE file", path);

    uint8_t *nt = file + RD32(file + 0x3c);
    uint8_t *opt = nt + 24;
    if (RD32(nt) != 0x4550 || RD16(opt) != 0x20b) DIE("%s is not a PE32+ image", path);

    pe_image *img = calloc(1, sizeof *img);
    uint64_t pref = RD64(opt + 24);
    img->size = RD32(opt + 56);
    img->base = mmap((void *)pref, img->size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (img->base == MAP_FAILED)
        img->base = mmap(NULL, img->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (img->base == MAP_FAILED) DIE("cannot map %zu bytes", img->size);

    memcpy(img->base, file, RD32(opt + 60));
    uint16_t nsec = RD16(nt + 6), optsz = RD16(nt + 20);
    for (uint8_t *sec = nt + 24 + optsz; nsec--; sec += 40) {
        uint32_t va = RD32(sec + 12), rawsz = RD32(sec + 16), rawoff = RD32(sec + 20), vsize = RD32(sec + 8);
        memcpy(img->base + va, file + rawoff, rawsz < vsize || !vsize ? rawsz : vsize);
    }
    munmap(file, st.st_size);

    /* From here on read headers from the mapped image. */
    nt = img->base + RD32(img->base + 0x3c);
    opt = nt + 24;
    if ((uint64_t)img->base != pref) apply_relocs(img, opt, (int64_t)img->base - (int64_t)pref);
    winapi_set_module(img->base);
    resolve_imports(img, opt);

    uint8_t *tls = data_dir(img, opt, DIR_TLS, NULL);
    if (tls) {
        img->tls_template = (uint8_t *)RD64(tls);
        img->tls_template_size = RD64(tls + 8) - RD64(tls);
        img->tls_zero_size = RD32(tls + 32);
        img->tls_index = (uint32_t *)RD64(tls + 16);
        img->tls_callbacks = (void *)RD64(tls + 24);
        *img->tls_index = 0;
    }

    protect_sections(img, nt);

    pe_thread_attach(img);
    for (void (WINAPI **cb)(void *, uint32_t, void *) = img->tls_callbacks; cb && *cb; cb++)
        (*cb)(img->base, 1, NULL);
    int (WINAPI *entry)(void *, uint32_t, void *) = (void *)(img->base + RD32(opt + 16));
    if (RD32(opt + 16) && !entry(img->base, 1, NULL)) DIE("DllMain(PROCESS_ATTACH) failed");
    return img;
}

void *pe_export(pe_image *img, const char *prefix)
{
    uint8_t *opt = img->base + RD32(img->base + 0x3c) + 24;
    uint8_t *exp = data_dir(img, opt, DIR_EXPORT, NULL);
    uint32_t *names = (uint32_t *)(img->base + RD32(exp + 32));
    uint16_t *ords = (uint16_t *)(img->base + RD32(exp + 36));
    uint32_t *funcs = (uint32_t *)(img->base + RD32(exp + 28));
    size_t n = strlen(prefix);
    for (uint32_t i = 0; i < RD32(exp + 24); i++)
        if (!strncmp((char *)img->base + names[i], prefix, n)) return img->base + funcs[ords[i]];
    DIE("export %s not found", prefix);
}

/* Windows code reaches per-thread state through GS: 0x08/0x10 stack bounds,
   0x30 TEB self pointer, 0x58 TLS slot array, 0x60 PEB. */
static __thread uint8_t *thread_teb;

void pe_thread_attach(pe_image *img)
{
    if (thread_teb) return;
    static uint8_t peb[0x1000];
    uint8_t *teb = calloc(1, 0x2000);
    *(uint64_t *)(teb + 0x08) = ~0ULL;
    *(uint64_t *)(teb + 0x10) = 0;
    *(void **)(teb + 0x30) = teb;
    *(void **)(teb + 0x60) = peb;
    if (img->tls_template) {
        uint8_t *block = calloc(1, img->tls_template_size + img->tls_zero_size + 16);
        memcpy(block, img->tls_template, img->tls_template_size);
        void **slots = calloc(64, sizeof(void *));
        slots[0] = block;
        *(void ***)(teb + 0x58) = slots;
    }
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, teb)) DIE("arch_prctl(ARCH_SET_GS) failed");
    thread_teb = teb;
}
