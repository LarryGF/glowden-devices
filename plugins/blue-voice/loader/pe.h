#pragma once
#include <stddef.h>
#include <stdint.h>

#define WINAPI __attribute__((ms_abi))

typedef uint16_t WCHAR;

typedef struct pe_image {
    uint8_t *base;
    size_t size;
    uint8_t *tls_template;
    size_t tls_template_size;
    size_t tls_zero_size;
    uint32_t *tls_index;
    void (WINAPI **tls_callbacks)(void *, uint32_t, void *);
} pe_image;

pe_image *pe_load(const char *path);
void *pe_export(pe_image *img, const char *prefix);
/* Must run on every thread before it calls into the DLL. */
void pe_thread_attach(pe_image *img);

void *winapi_resolve(const char *dll, const char *name, uint16_t ordinal);
void winapi_set_module(uint8_t *base);
