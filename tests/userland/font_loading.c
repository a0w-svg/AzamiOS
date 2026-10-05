#define _POSIX_C_SOURCE 200809L
#include "../../userland/libc/include/azami/font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

static int inject_interrupt, chunk_size;
/* font.o calls this wrapper; fixture writes and host libc still use real I/O. */
ssize_t font_test_read(int fd, void *buffer, size_t count)
{
    if (inject_interrupt) { inject_interrupt = 0; errno = EINTR; return -1; }
    if (chunk_size && count > (size_t)chunk_size) count = (size_t)chunk_size;
    return read(fd, buffer, count);
}
static void write_fixture(int fd, const void *header, size_t size, size_t bytes)
{
    unsigned char glyphs[4096]; memset(glyphs, 0x81, sizeof(glyphs));
    assert(bytes <= sizeof(glyphs));
    assert(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0);
    assert(write(fd, header, size) == (ssize_t)size);
    assert(write(fd, glyphs, bytes) == (ssize_t)bytes);
}
static void expect_bad(const char *path)
{
    az_font_t *font = az_font_load(path);
    if (font) { az_font_free(font); assert(!"malformed font accepted"); }
}
int main(void)
{
    char path[] = "/tmp/azami-font-test.XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    azf_header_t azf = {.magic=AZF_MAGIC, .version=AZF_VERSION, .header_sz=sizeof(azf),
        .family="Test", .style="Regular", .glyph_w=10, .glyph_h=7,
        .first_char='A', .last_char='A', .num_glyphs=1, .bytes_per_glyph=14};
    write_fixture(fd, &azf, sizeof(azf), 14);
    inject_interrupt=1; chunk_size=3;
    az_font_t *font = az_font_load(path);
    assert(font && font->glyph_w == 10 && font->bytes_per_glyph == 14);
    az_font_free(font);
    azf.header_sz=0; write_fixture(fd,&azf,sizeof(azf),14); expect_bad(path);
    azf.header_sz=sizeof(azf); azf.bytes_per_glyph=7;
    write_fixture(fd,&azf,sizeof(azf),14); expect_bad(path);
    azf.bytes_per_glyph=14; azf.last_char='B';
    write_fixture(fd,&azf,sizeof(azf),14); expect_bad(path);
    azf.last_char='A'; write_fixture(fd,&azf,sizeof(azf),13); expect_bad(path);
    azf.num_glyphs=0; write_fixture(fd,&azf,sizeof(azf),14); expect_bad(path);
    psf2_header_t psf = {.magic=PSF2_MAGIC, .headersize=sizeof(psf),
        .length=1, .charsize=14, .width=10, .height=7};
    write_fixture(fd,&psf,sizeof(psf),14);
    font=az_font_load(path); assert(font && font->glyph_w == 10); az_font_free(font);
    psf.width=256; write_fixture(fd,&psf,sizeof(psf),14); expect_bad(path);
    psf.width=10; psf.charsize=7; write_fixture(fd,&psf,sizeof(psf),14); expect_bad(path);
    psf.charsize=14; psf.length=65536; write_fixture(fd,&psf,sizeof(psf),14); expect_bad(path);
    psf.length=1; psf.headersize=0; write_fixture(fd,&psf,sizeof(psf),14); expect_bad(path);
    psf1_header_t psf1 = {.magic=PSF1_MAGIC, .charsize=8};
    write_fixture(fd,&psf1,sizeof(psf1),256*8);
    font=az_font_load(path); assert(font && font->glyph_h == 8); az_font_free(font);
    psf1.charsize=0; write_fixture(fd,&psf1,sizeof(psf1),0); expect_bad(path);
    close(fd); unlink(path);
    puts("AZF/PSF loading: interrupted/short reads and invalid headers: PASS");
    return 0;
}
