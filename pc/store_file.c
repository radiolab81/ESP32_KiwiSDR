/**
 * @file store_file.c
 * @brief Datei als "Flash": loeschen = mit 0xFF fuellen, schreiben = pwrite.
 */
#include "store_file.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct { int fd; size_t slot_size; } file_ctx_t;

static int f_read(void *ctx, int slot, size_t off, void *buf, size_t len)
{
    file_ctx_t *c = ctx;
    if (off + len > c->slot_size) return -1;
    return pread(c->fd, buf, len, (off_t)((size_t)slot * c->slot_size + off)) == (ssize_t)len ? 0 : -1;
}

static int f_erase(void *ctx, int slot)
{
    file_ctx_t *c = ctx;
    uint8_t ff[4096];
    memset(ff, 0xFF, sizeof ff);
    for (size_t o = 0; o < c->slot_size; o += sizeof ff)
        if (pwrite(c->fd, ff, sizeof ff, (off_t)((size_t)slot * c->slot_size + o)) != (ssize_t)sizeof ff) return -1;
    return 0;
}

static int f_write(void *ctx, int slot, size_t off, const void *buf, size_t len)
{
    file_ctx_t *c = ctx;
    if (off + len > c->slot_size) return -1;
    return pwrite(c->fd, buf, len, (off_t)((size_t)slot * c->slot_size + off)) == (ssize_t)len ? 0 : -1;
}

kiwi_dir_store_t *store_file_open(const char *path, size_t slot_size)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return NULL;
    file_ctx_t *c = calloc(1, sizeof *c);
    kiwi_dir_store_t *st = calloc(1, sizeof *st);
    if (!c || !st) { close(fd); free(c); free(st); return NULL; }
    c->fd = fd;
    c->slot_size = slot_size;
    st->slot_size = slot_size;
    st->erase_size = 4096;
    st->read = f_read; st->erase = f_erase; st->write = f_write;
    st->ctx = c;
    /* Neue/zu kleine Datei: wie frischer Flash mit 0xFF fuellen. */
    if (lseek(fd, 0, SEEK_END) < (off_t)(slot_size * KIWI_DIR_SLOTS)) {
        for (int s = 0; s < KIWI_DIR_SLOTS; s++)
            if (f_erase(c, s)) { store_file_close(st); return NULL; }
    }
    return st;
}

void store_file_close(kiwi_dir_store_t *st)
{
    if (!st) return;
    close(((file_ctx_t *)st->ctx)->fd);
    free(st->ctx);
    free(st);
}
