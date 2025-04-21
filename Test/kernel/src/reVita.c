#include "reVita.h"

static SceUID mutex_gui_uid = -1;

static uint32_t color = 0x80171717;
uint32_t *fb_base;
uint32_t fbWidth, fbHeight, fbPitch;
static const unsigned char UI_CORNER_OFF[UI_CORNER_RADIUS] = {9, 7, 5, 4, 3, 2, 2, 1, 1};
static bool isStripped;

uint32_t *vfb_base;
uint32_t uiWidth, uiHeight;
SceUID vfb_uid;

// gui.c:
void gui_init(void)
{
    mutex_gui_uid = ksceKernelCreateMutex("pebble_gui_mutex", 0, 0, NULL);
    rendererv_init(UI_WIDTH, UI_HEIGHT);
}

void gui_destroy(void)
{
    if (mutex_gui_uid >= 0)
    {
        ksceKernelDeleteMutex(mutex_gui_uid);
        mutex_gui_uid = -1;
    }
}

// /renderer.c:
void renderer_writeFromVFB(int64_t tickOpened, bool anim)
{
    int64_t tick = ksceKernelGetSystemTimeWide();

    uint32_t ui_x = (max(fbWidth - uiWidth, 0)) / 2;
    uint32_t ui_y = (max(fbHeight - uiHeight, 0)) / 2;

    float multiplyer = 0;
    if (ANIMATION_TIME >= tick - tickOpened && anim)
        multiplyer = ((float)(ANIMATION_TIME - (int)(tick - tickOpened))) / ANIMATION_TIME;

    int32_t ui_yAnimated = ui_y - (uiHeight + ui_y) * multiplyer;
    uint32_t ui_yCalculated = max(ui_yAnimated, 0);
    uint32_t ui_cutout = ui_yAnimated >= 0 ? 0 : -ui_yAnimated;

    for (uint32_t i = 0; i < uiHeight - ui_cutout; i++)
    {
        uint32_t off = 0;
        if (i < UI_CORNER_RADIUS)
        {
            off = UI_CORNER_OFF[i];
        }
        else if (i > uiHeight - UI_CORNER_RADIUS)
        {
            off = UI_CORNER_OFF[uiHeight - i];
        }
        ksceKernelMemcpyKernelToUser(&fb_base[(ui_yCalculated + i) * fbPitch + ui_x + off],
                                     &vfb_base[(i + ui_cutout) * uiWidth + off],
                                     sizeof(uint32_t) * (uiWidth - 2 * off));
    }
}

void renderer_setFB(const SceDisplayFrameBuf *param)
{
    fbWidth = param->width;
    fbHeight = param->height;
    fb_base = param->base;
    fbPitch = param->pitch;
}

// rendererv.c
void rendererv_drawImage(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const unsigned char *img)
{
    uint32_t idx = 0;
    uint8_t bitN = 0;
    for (uint32_t j = 0; j < h; j++)
    {
        for (uint32_t i = 0; i < w; i++)
        {
            if (bitN >= 8)
            {
                idx++;
                bitN = 0;
            }
            if (READ(img[idx], (7 - bitN)))
            {
                vfb_base[(y + j) * uiWidth + x + i] = color;
            }
            bitN++;
        }
        if (bitN != 0)
        {
            idx++;
            bitN = 0;
        }
    }
}

void rendererv_drawCharIcon(char character, int x, int y)
{
    rendererv_drawImage(x, y - 1, ICON_W, ICON_H, &ICON[character * 60]);
}

void rendererv_drawChar(char character, int x, int y)
{
    character = character % ICON_ID__NUM;
    rendererv_drawImage(x, y, FONT_WIDTH, FONT_HEIGHT, &font[character * 40]);
}

void rendererv_drawString(int x, int y, const char *str)
{
    for (size_t i = 0; i < strlen(str); i++)
    {
        if (str[i] == ' ')
            continue;
        if (str[i] == '$')
        {
            rendererv_drawCharIcon(str[i + 1], x + i * 12, y);
            i++;
        }
        else
            rendererv_drawChar(str[i], x + i * 12, y);
    }
    if (isStripped)
        rendererv_drawRectangle(x, y + CHA_H / 2, (strlen(str))*CHA_W, 2, color);
}

void rendererv_drawRectangle(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t clr)
{
    if ((x + w) > uiWidth || (y + h) > uiHeight)
        return;
    for (uint32_t i = x; i < x + w; i++)
    {
        for (uint32_t j = y; j < y + h; j++)
        {
            vfb_base[j * uiWidth + i] = clr;
        }
    }
}

void rendererv_drawStringF(int x, int y, const char *format, ...)
{
    char str[512] = {0};
    va_list va;

    va_start(va, format);
    vsnprintf(str, (int)512, format, va);
    va_end(va);

    rendererv_drawString(x, y, str);
}

int rendererv_allocVirtualFB(void)
{
    int ret = ksceKernelAllocMemBlock("vfb_base", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
                                      (uiWidth * uiHeight * sizeof(uint32_t) + 0xfff) & ~0xfff, NULL);
    if (ret > 0)
    {
        vfb_uid = ret;
        ksceKernelGetMemBlockBase(vfb_uid, (void **)&vfb_base);
    }
    return ret;
}

int rendererv_freeVirtualFB(void)
{
    return ksceKernelFreeMemBlock(vfb_uid);
}

void rendererv_setColor(uint32_t c)
{
    color = c;
}

void rendererv_init(uint32_t w, uint32_t h)
{
    uiWidth = w;
    uiHeight = h;
}