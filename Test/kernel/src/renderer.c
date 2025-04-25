#include "renderer.h"

static uint32_t color = 0xFFFFFFFF;
static SceUID gui_buffer_uids[2] = {0, 0};
int8_t buf_index = 0;
uint32_t *fb_bases[2] = {0, 0};
const size_t buffer_size = (544 * 960 * sizeof(uint32_t) + 0xfff) & ~0xfff;

void renderer_drawImage(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const unsigned char *img)
{
    if (!fb_bases[buf_index] || !img || w == 0 || h == 0 || x >= UI_WIDTH || y >= UI_HEIGHT)
        return;
    uint32_t endX = x + w;
    uint32_t endY = y + h;
    if (endX > UI_WIDTH)
        endX = UI_WIDTH;
    if (endY > UI_HEIGHT)
        endY = UI_HEIGHT;
    if (x >= endX || y >= endY)
        return;
    uint32_t bytes_per_row = (w + 7) / 8;
    for (uint32_t j = y; j < endY; ++j)
    {
        uint32_t img_row_start_byte = (j - y) * bytes_per_row;
        uint32_t *row_ptr = fb_bases[buf_index] + j * UI_WIDTH;
        for (uint32_t i = x; i < endX; ++i)
        {
            uint32_t img_col = i - x;
            uint32_t img_byte_index = img_row_start_byte + (img_col / 8);
            uint32_t bit_pos = img_col % 8;
            if ((img[img_byte_index] >> (7 - bit_pos)) & 1)
                row_ptr[i] = color; 
        }
    }
}

void renderer_drawChar(char c, int x, int y)
{
    if (c < 32 || c > 126)
        return;
    renderer_drawImage(x, y, FONT_WIDTH, FONT_HEIGHT, &font[(uint32_t)(c - 32) * 40]);
}

void renderer_drawRectangle(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t clr)
{
    if (!fb_bases[buf_index] || w == 0 || h == 0 || x >= UI_WIDTH || y >= UI_HEIGHT)
        return;
    uint32_t endX = x + w;
    uint32_t endY = y + h;
    if (endX > UI_WIDTH)
        endX = UI_WIDTH;
    if (endY > UI_HEIGHT)
        endY = UI_HEIGHT;
    if (x >= endX || y >= endY)
        return;
    for (uint32_t j = y; j < endY; ++j)
    {
        uint32_t *row_ptr = fb_bases[buf_index] + j * UI_WIDTH + x;
        for (uint32_t i = 0; i < (endX - x); ++i)
            row_ptr[i] = clr;
    }
}

void renderer_clearRectangle(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    renderer_drawRectangle(x, y, w, h, 0x80171717);
}

void renderer_drawString(int x, int y, const char *str)
{
    if (!str || !fb_bases[buf_index] || y < -FONT_HEIGHT || y >= UI_HEIGHT)
        return;
    int cx = x;
    char c;
    while ((c = *str++) != '\0')
    {
        if (cx >= UI_WIDTH)
            break;
        if (cx + FONT_WIDTH > 0 && c != ' ')
            renderer_drawChar(c, cx, y);
        cx += FONT_WIDTH;
    }
}

void renderer_drawStringF(int x, int y, const char *format, ...)
{
    char str[512];
    va_list va;
    va_start(va, format);
    vsnprintf(str, sizeof(str), format, va);
    va_end(va);
    renderer_drawString(x, y, str);
}

void renderer_destroy(void)
{
    while (ksceKernelTryLockMutex(pebble_mtx_uid, 1) < 0) 
        ksceKernelDelayThread(500);
    fb_bases[0] = NULL;
    fb_bases[1] = NULL;
    if (gui_buffer_uids[0])
        ksceKernelFreeMemBlock(gui_buffer_uids[0]);
    if (gui_buffer_uids[1])
        ksceKernelFreeMemBlock(gui_buffer_uids[1]);

    gui_buffer_uids[0] = 0;
    gui_buffer_uids[1] = 0;
    ksceKernelUnlockMutex(pebble_mtx_uid, 1);
}

int renderer_init(void)
{
    if (!gui_buffer_uids[0])
        gui_buffer_uids[0] =
            ksceKernelAllocMemBlock("gui_buffer1", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, buffer_size, NULL);
    if (gui_buffer_uids[0] <= 0)
    {
        ksceKernelPrintf("!!!Failed guiBUF 11111!!!\n");
        return -1;
    }

    if (!gui_buffer_uids[1])
        gui_buffer_uids[1] =
            ksceKernelAllocMemBlock("gui_buffer2", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, buffer_size, NULL);
    if (gui_buffer_uids[1] <= 0)
    {
        ksceKernelFreeMemBlock(gui_buffer_uids[0]);
        gui_buffer_uids[0] = 0;
        ksceKernelPrintf("!!!Failed guiBUF 22222!!!\n");
        return -1;
    }

    if (!fb_bases[0])
        ksceKernelGetMemBlockBase(gui_buffer_uids[0], (void **)&fb_bases[0]);
    if (!fb_bases[1])
        ksceKernelGetMemBlockBase(gui_buffer_uids[1], (void **)&fb_bases[1]);
    if (!fb_bases[0] || !fb_bases[1])
    {
        renderer_destroy();
        return -1;
    }
    
    renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
    buf_index ^= 1;
    renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
    return 0;
}

void renderer_setColor(uint32_t c)
{
    color = c;
}