#include "renderer.h"

static uint32_t color = 0xFFFFFFFF;
static SceUID gui_buffer_uids[2] = {0, 0};
const size_t buffer_size = (544 * 960 * sizeof(uint32_t) + 0xfff) & ~0xfff;

void renderer_drawImage(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const unsigned char *img)
{
    if (!current_display_ptr || !img || x >= UI_WIDTH || y >= UI_HEIGHT)
        return;
    
    uint32_t bytes_per_row = (w + 7) / 8;
    uint32_t endX = x + w < UI_WIDTH ? x + w : UI_WIDTH;
    uint32_t endY = y + h < UI_HEIGHT ? y + h : UI_HEIGHT;

    for (uint32_t j = y; j < endY; ++j)
    {
        uint32_t img_row_start_byte = (j - y) * bytes_per_row;
        uint32_t *row_ptr = current_display_ptr + j * UI_WIDTH;
        for (uint32_t i = x; i < endX; ++i)
        {
            uint32_t img_byte_index = img_row_start_byte + ((i - x) / 8);
            uint32_t bit_pos = (i - x) % 8;
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
    if (!current_display_ptr || w == 0 || h == 0)
        return;

    uint32_t startX = x < UI_WIDTH ? x : UI_WIDTH;
    uint32_t startY = y < UI_HEIGHT ? y : UI_HEIGHT;
    uint32_t endX = x + w < UI_WIDTH ? x + w : UI_WIDTH;
    uint32_t endY = y + h < UI_HEIGHT ? y + h : UI_HEIGHT;

    if (startX >= endX || startY >= endY)
        return;

    for (uint32_t j = startY; j < endY; ++j)
    {
        uint32_t *row_ptr = current_display_ptr + j * UI_WIDTH;
        for (uint32_t i = startX; i < endX; ++i)
            row_ptr[i] = clr;
    }
}

void renderer_clearRectangle(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    renderer_drawRectangle(x, y, w, h, 0x00171717);
}

void renderer_drawString(int x, int y, const char *str)
{
    if (!str || !current_display_ptr)
        return;
    int cx = x;
    for (; *str; ++str)
    {
        if (cx + FONT_WIDTH <= 0)
        {
            cx += FONT_WIDTH;
            continue;
        }
        if (cx >= UI_WIDTH)
            break;

        if (*str != ' ')
            renderer_drawChar(*str, cx, y);

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
    ksceKernelLockMutex(pebble_mtx_uid, 1, NULL);
    fb_bases[0] = NULL;
    fb_bases[1] = NULL;
    current_display_ptr = NULL;
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
    if (!gui_buffer_uids[1])
        gui_buffer_uids[1] =
            ksceKernelAllocMemBlock("gui_buffer2", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, buffer_size, NULL);

    if (gui_buffer_uids[0] < 0 || gui_buffer_uids[1] < 0 ||
        ksceKernelGetMemBlockBase(gui_buffer_uids[0], (void **)&fb_bases[0]) < 0 || !fb_bases[0] ||
        ksceKernelGetMemBlockBase(gui_buffer_uids[1], (void **)&fb_bases[1]) < 0 || !fb_bases[1])
    {
        renderer_destroy();
        return -1;
    }
    
    fb_now_idx = 0;
    current_display_ptr = fb_bases[fb_now_idx];
    renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
    current_display_ptr = fb_bases[fb_now_idx ^ 1];
    renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
    return 0;
}

void renderer_setColor(uint32_t c)
{
    color = c;
}