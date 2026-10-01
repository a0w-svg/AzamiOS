/* Run the production driver against a deterministic AC'97 register model. */
#include "../../include/azami/defs.h"
#include "../../drivers/sound/ac97.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/uaccess.h"

extern int printf(const char *, ...);
static u8 civ, lvi, cr;
static u16 sr = AC97_SR_DCH, codec[64];
static unsigned lock_depth, lvi_writes, cr_writes, io_reads, copies_locked;
static bool stuck_reset, stuck_semaphore, bad_copy;
static u32 global_status = AC97_GLOB_READY;

static u8 mock_inb(u16 port)
{
    io_reads++;
    if (port == 0x2000 + AC97_PO_CIV) return civ;
    if (port == 0x2000 + AC97_PO_CR) return cr;
    if (port == 0x2000 + AC97_CAS) return stuck_semaphore;
    return 0;
}
static u16 mock_inw(u16 port)
{
    io_reads++;
    if (port == 0x2000 + AC97_PO_SR) return sr;
    return codec[(port - 0x1000) / 2];
}
static u32 mock_inl(u16 port)
{
    return port == 0x2000 + AC97_GLOB_STA ? global_status : 0;
}
static void mock_outb(u16 port, u8 value)
{
    if (port == 0x2000 + AC97_PO_LVI) {
        lvi_writes++;
        if ((cr & AC97_CR_RUN) && (sr & AC97_SR_DCH)) {
            civ = (civ + 1) & 31;
            sr &= ~(AC97_SR_DCH | AC97_SR_CELV);
        }
        lvi = value;
    }
    if (port == 0x2000 + AC97_PO_CR) {
        cr_writes++;
        cr = value;
        if (value & AC97_CR_RESET) {
            if (!stuck_reset) { cr = 0; civ = lvi = 0; sr = AC97_SR_DCH; }
        } else if (value & AC97_CR_RUN) {
            sr &= ~AC97_SR_DCH;
        } else sr |= AC97_SR_DCH;
    }
}
static void mock_outw(u16 port, u16 value)
{
    if (port == 0x2000 + AC97_PO_SR) { sr &= ~(value & AC97_SR_ACK); return; }
    if (port == 0x1000 + AC97_NAMBAR_PCM_FRONT_RATE &&
        !(codec[AC97_NAMBAR_EXT_AUDIO_CTRL / 2] & AC97_EXT_VRA)) return;
    codec[(port - 0x1000) / 2] = value;
}
static void mock_outl(u16 port, u32 value)
{
    if (port == 0x2000 + AC97_GLOB_STA) global_status &= ~value;
}
size_t copy_from_user(void *dst, const void *src, size_t n)
{
    copies_locked += !!lock_depth;
    if (bad_copy) return n;
    __builtin_memcpy(dst, src, n);
    return 0;
}
size_t copy_to_user(void *dst, const void *src, size_t n)
{
    return copy_from_user(dst, src, n);
}
#define spinlock_lock_irqsave(lock) ((void)(lock), lock_depth++, (irqflags_t)0)
#define spinlock_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags), lock_depth--)
#define inb mock_inb
#define inw mock_inw
#define inl mock_inl
#define outb mock_outb
#define outw mock_outw
#define outl mock_outl
#undef PHYS_TO_VIRT
#undef VIRT_TO_PHYS
#define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
#define VIRT_TO_PHYS(v) ((phys_addr_t)(uintptr_t)(v))
#include "../../drivers/sound/ac97.c"

static u8 dma_pages[33][4096] __attribute__((aligned(4096)));
static unsigned alloc_calls, fail_alloc, live_pages, registrations;
static u16 pci_command;
phys_addr_t pmm_alloc_pages_32(size_t count)
{
    (void)count;
    if (++alloc_calls == fail_alloc || alloc_calls > 33) return 0;
    live_pages++;
    return (phys_addr_t)(uintptr_t)dma_pages[alloc_calls - 1];
}
void pmm_free_pages(phys_addr_t addr, size_t count)
{
    (void)addr; (void)count;
    live_pages--;
}
u16 pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset)
{
    (void)bus; (void)slot; (void)func; (void)offset;
    return pci_command;
}
void pci_config_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 value)
{
    (void)bus; (void)slot; (void)func; (void)offset;
    pci_command = value;
}
void sound_register_device(sound_device_t *dev) { (void)dev; registrations++; }
void kprintf(const char *fmt, ...) { (void)fmt; }

#define CHECK(expr) do { if (!(expr)) { printf("FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
static ac97_bdl_entry_t descriptors[32];
static u8 buffers[32][4096], data[32 * 4096];

int main(void)
{
    g_nam_bar = 0x1000;
    g_nabm_bar = 0x2000;
    g_bdl = descriptors;
    g_ac97_ready = true;
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (i / 4096) + 1;
    for (unsigned i = 0; i < 32; i++) {
        g_audio_buffers[i] = buffers[i];
        descriptors[i].ptr = 0x100000 + i * 4096;
    }
    CHECK(ac97_write_pcm(data, 0) == 0);
    CHECK(ac97_write_pcm(data, 3) == -EINVAL);
    CHECK(ac97_write_pcm(data, sizeof(data)) == sizeof(data));
    CHECK(lvi == 31 && civ == 0 && cr == AC97_CR_RUN);
    CHECK(lvi_writes == 1 && cr_writes == 1);
    for (unsigned i = 0; i < 32; i++) {
        CHECK(descriptors[i].ptr == 0x100000 + i * 4096);
        CHECK(descriptors[i].samples == 2048 && descriptors[i].flags == 0);
        CHECK(buffers[i][0] == i + 1 && buffers[i][4095] == i + 1);
    }
    CHECK(ac97_write_pcm(data, 4096) == -EAGAIN);
    ac97_audio_buf_info_t space;
    CHECK(ac97_ioctl(AC97_DSP_GETOSPACE, &space) == 0);
    CHECK(space.bytes == 0 && space.fragstotal == 32 && space.fragsize == 4096);
    civ = 4; /* four descriptors have completed */
    CHECK(ac97_write_pcm(data, 5 * 4096) == 4 * 4096);
    CHECK(lvi == 3 && lvi_writes == 2 && cr_writes == 1);
    CHECK(buffers[4][0] == 5); /* in-flight descriptor was not overwritten */
    /* Drain the queue; CIV remains on the last descriptor. Resume at PIV. */
    civ = lvi;
    sr = AC97_SR_DCH | AC97_SR_CELV;
    CHECK(ac97_write_pcm(data, 4096) == 4096);
    CHECK(civ == 4 && lvi == 4 && !(sr & AC97_SR_DCH));
    CHECK(cr_writes == 1);
    CHECK(ac97_ioctl(AC97_DSP_RESET, NULL) == 0);
    CHECK(!g_ac97_started && cr == 0 && civ == 0);
    CHECK(ac97_ioctl(AC97_DSP_GETOSPACE, &space) == 0 && space.bytes == sizeof(data));
    CHECK(ac97_write_pcm(data, 7) == 4 && descriptors[0].samples == 2);

    s32 value = 44100;
    g_ac97_vra = true;
    codec[AC97_NAMBAR_EXT_AUDIO_CTRL / 2] = AC97_EXT_VRA;
    CHECK(ac97_ioctl(AC97_DSP_SPEED, &value) == -EBUSY);
    CHECK(ac97_ioctl(AC97_DSP_RESET, NULL) == 0);
    CHECK(ac97_ioctl(AC97_DSP_SPEED, &value) == 0 && value == 44100);
    CHECK(codec[AC97_NAMBAR_PCM_FRONT_RATE / 2] == 44100);
    value = 0;
    CHECK(ac97_ioctl(AC97_DSP_SPEED, &value) == 0 && value == 44100);
    value = 100000;
    CHECK(ac97_ioctl(AC97_DSP_SPEED, &value) == 0 && value == 48000);
    value = 1;
    CHECK(ac97_ioctl(AC97_DSP_SPEED, &value) == 0 && value == 8000);
    value = -1;
    CHECK(ac97_ioctl(AC97_DSP_SPEED, &value) == -EINVAL);
    g_ac97_vra = false;
    g_ac97_rate = 48000;
    value = 22050;
    CHECK(ac97_ioctl(AC97_DSP_SPEED, &value) == 0 && value == 48000);
    value = 8;
    CHECK(ac97_ioctl(AC97_DSP_SETFMT, &value) == 0 && value == AC97_AFMT_S16_LE);
    value = 1;
    CHECK(ac97_ioctl(AC97_DSP_CHANNELS, &value) == 0 && value == 2);
    value = 0;
    CHECK(ac97_ioctl(AC97_DSP_STEREO, &value) == 0 && value == 1);
    CHECK(ac97_ioctl(AC97_PCM_READ_BITS, &value) == 0 && value == 16);
    CHECK(ac97_ioctl(AC97_DSP_GETBLKSIZE, &value) == 0 && value == 4096);
    value = 0xffff;
    CHECK(ac97_ioctl(SOUND_PCM_WRITE_VOLUME, &value) == 0);
    CHECK(ac97_ioctl(SOUND_PCM_READ_VOLUME, &value) == 0 && value == 0x6464);
    value = 0;
    CHECK(ac97_ioctl(SOUND_PCM_WRITE_VOLUME, &value) == 0);
    CHECK(codec[AC97_NAMBAR_MASTER_VOL / 2] & 0x8000);
    CHECK(ac97_ioctl(0xffffffff, NULL) == -ENOTTY);
    CHECK(ac97_ioctl(AC97_DSP_SPEED, NULL) == -EFAULT);
    CHECK(ac97_ioctl(AC97_DSP_GETOSPACE, (void *)(TASK_SIZE_MAX - 8)) == -EFAULT);
    bad_copy = true;
    CHECK(ac97_ioctl(AC97_DSP_SPEED, &value) == -EFAULT);
    CHECK(ac97_ioctl(SOUND_PCM_READ_RATE, &value) == -EFAULT);
    bad_copy = false;
    stuck_semaphore = true;
    io_reads = 0;
    CHECK(ac97_ioctl(SOUND_PCM_WRITE_VOLUME, &value) == -ETIMEDOUT);
    CHECK(io_reads == 2000);
    stuck_semaphore = false;
    stuck_reset = true;
    CHECK(ac97_ioctl(AC97_DSP_RESET, NULL) == -ETIMEDOUT);
    CHECK(ac97_write_pcm(data, 4096) == -ENODEV);
    CHECK(lock_depth == 0 && copies_locked == 0);

    /* Probe validates raw BAR type bits, negotiates VRA, and unwinds every
     * possible allocation failure without publishing a half-ready device. */
    g_bdl = NULL;
    __builtin_memset(g_audio_buffers, 0, sizeof(g_audio_buffers));
    g_nam_bar = g_nabm_bar = 0;
    stuck_reset = false;
    codec[AC97_NAMBAR_EXT_AUDIO_ID / 2] = AC97_EXT_VRA;
    pci_device_info_t pci = { .bar = { 0x1001, 0x2001 } };
    device_t hal = { 0 };
    dm_device_t dm = { .hal = &hal, .bus_data = &pci };
    for (fail_alloc = 1; fail_alloc <= 33; fail_alloc++) {
        alloc_calls = 0;
        CHECK(ac97_probe(&dm, NULL) == -ENOMEM);
        CHECK(!live_pages && !registrations && !g_bdl && !g_ac97_ready);
        CHECK(!g_nam_bar && !g_nabm_bar && !(pci_command & 4));
    }
    alloc_calls = 0;
    fail_alloc = 0;
    pci.bar[0] = 0x1000; /* MMIO cannot be accessed using inw/outw. */
    CHECK(ac97_probe(&dm, NULL) == -ENODEV && !alloc_calls);
    pci.bar[0] = 0x1001;
    codec[AC97_NAMBAR_EXT_AUDIO_CTRL / 2] = 0;
    CHECK(ac97_probe(&dm, NULL) == 0);
    CHECK(live_pages == 33 && registrations == 1 && g_ac97_ready && g_ac97_vra);
    CHECK(codec[AC97_NAMBAR_EXT_AUDIO_CTRL / 2] & AC97_EXT_VRA);
    CHECK(codec[AC97_NAMBAR_PCM_FRONT_RATE / 2] == 48000);
    CHECK(codec[AC97_NAMBAR_PCM_OUT_VOL / 2] == 0x0808);
    CHECK(ac97_probe(&dm, NULL) == -EBUSY && live_pages == 33);
    ac97_remove(&dm);
    CHECK(!g_ac97_ready && !live_pages && !g_bdl && !(pci_command & 4));
    CHECK(lock_depth == 0);
    printf("AC97 DMA and OSS regressions passed\n");
    return 0;
}
