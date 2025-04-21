#include "kernel.h"
#include "reVita.h"

State guistate;

static const MemLayoutInfo layout_info[] = {
    [MEM_LAYOUT_8BIT] = {"%02X", 1}, [MEM_LAYOUT_16BIT] = {"%04X", 2}, [MEM_LAYOUT_32BIT] = {"%08X", 4}};

void load_hotkeys(void)
{
    guistate.hotkeys =
        (HotkeyConfig){DEFAULT_SHOW_GUI, DEFAULT_AREA_SELECT, DEFAULT_EDIT_MODE, DEFAULT_CANCEL, DEFAULT_CONFIRM};
    SceUID fd = ksceIoOpen(HOTKEY_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0)
    {
        char buf[64];
        ksceIoRead(fd, buf, sizeof(buf));
        sscanf(buf, "%d %d %d %d %d", &guistate.hotkeys.show_gui, &guistate.hotkeys.area_select, &guistate.hotkeys.edit,
               &guistate.hotkeys.cancel, &guistate.hotkeys.confirm);
        ksceIoClose(fd);
    }
}

static void save_hotkeys(void)
{
    SceUID fd = ksceIoOpen(HOTKEY_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0)
    {
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "%d %d %d %d %d", guistate.hotkeys.show_gui, guistate.hotkeys.area_select,
                           guistate.hotkeys.edit, guistate.hotkeys.cancel, guistate.hotkeys.confirm);
        ksceIoWrite(fd, buf, len);
        ksceIoClose(fd);
    }
    load_hotkeys();
}

static void update_memview_state(void)
{
    kernel_list_breakpoints(guistate.breakpoints);
    for (int i = 0; i < 16; i++)
    {
        if (guistate.breakpoints[i].type)
        {
            kernel_get_registers(&guistate.regs);
            guistate.stack_size =
                kernel_read_memory((void *)guistate.regs.sp, guistate.stack, sizeof(guistate.stack)) / 4;
            guistate.callstack[0] = guistate.regs.pc;
            guistate.callstack[1] = guistate.regs.lr;
            guistate.callstack_size = 2;
            break;
        }
    }
}

static void draw_hex_row(uint32_t addr)
{
    uint8_t data[16];
    kernel_read_memory((void *)addr, data, 16);
    char line[64], ascii[17] = {0};
    snprintf(line, 12, "%08X: ", addr);
    for (int i = 0; i < 16; i += layout_info[guistate.mem_layout].bytes)
    {
        uint32_t val;
        memcpy(&val, data + i, layout_info[guistate.mem_layout].bytes);
        snprintf(line + 10 + i * 3, 9, layout_info[guistate.mem_layout].format, val);
        for (int j = 0; j < layout_info[guistate.mem_layout].bytes; j++)
            ascii[i + j] = data[i + j] >= 32 && data[i + j] < 127 ? data[i + j] : '.';
    }
    if (addr == guistate.addr)
        rendererv_drawRectangle(10, 80 + ((addr - guistate.base_addr) / 16) * 20, 400, 20, 0x80FFFFFF);

    rendererv_drawStringF(10, 80 + ((addr - guistate.base_addr) / 16) * 20, "%s  %s", line, ascii);
}

static void draw_registers(int x, int y)
{
    int has_bp = 0;
    for (int i = 0; i < 16 && !has_bp; has_bp = guistate.breakpoints[i++].type);

    if (!has_bp) return;
    const char *regs[] = {"R0", "R1", "R2",  "R3",  "R4",  "R5", "R6", "R7",
                          "R8", "R9", "R10", "R11", "R12", "SP", "LR", "PC"};
    for (int i = 0; i < 16; i++, y += 20)
        rendererv_drawStringF(x, y, "%-3s: %08X", regs[i], ((uint32_t *)&guistate.regs)[i]);
}

static void handle_feature_input(uint32_t released)
{
    switch (guistate.ui_state)
    {
    case UI_FEATURE_HW_BREAK: {
        guistate.edit_feature += (released & SCE_CTRL_UP ? 0x10 : 0) - (released & SCE_CTRL_DOWN ? 0x10 : 0);
        if (released & guistate.hotkeys.confirm)
            kernel_set_hardware_breakpoint(guistate.edit_feature);

        break;
    }
    case UI_FEATURE_SW_BREAK: {
        guistate.edit_feature += (released & SCE_CTRL_UP ? 1 : 0) - (released & SCE_CTRL_DOWN ? 1 : 0);
        if (released & guistate.hotkeys.confirm)
            kernel_set_software_breakpoint(guistate.edit_feature, guistate.regs.cpsr & 0x20);

        break;
    }
    case UI_FEATURE_CLEAR: {
        guistate.edit_feature += (released & SCE_CTRL_UP ? 1 : 0) - (released & SCE_CTRL_DOWN ? 1 : 0);
        if (released & guistate.hotkeys.confirm)
            kernel_clear_breakpoint(guistate.edit_feature);

        break;
    }
    case UI_FEATURE_HOTKEYS: {
        if (released & SCE_CTRL_UP)
            guistate.edit_feature = (guistate.edit_feature - 1 + 5) % 5;
        if (released & SCE_CTRL_DOWN)
            guistate.edit_feature = (guistate.edit_feature + 1) % 5;
        int *keys = (int *)&guistate.hotkeys;
        if (released & SCE_CTRL_LEFT)
            keys[guistate.edit_feature] >>= 1;
        if (released & SCE_CTRL_RIGHT)
            keys[guistate.edit_feature] <<= 1;
        if (released & guistate.hotkeys.confirm)
            save_hotkeys();
        break;
    }
    default:
        break;
    }
    if (released & (guistate.hotkeys.cancel | guistate.hotkeys.confirm))
        guistate.ui_state = UI_MEMVIEW;
}

static void handle_memview_input(uint32_t released, uint32_t current)
{
    if (current & guistate.hotkeys.area_select)
    {
        if (released & SCE_CTRL_LEFT)
            guistate.active_area = MEMVIEW_HEX;
        else if (released & SCE_CTRL_UP)
            guistate.active_area = MEMVIEW_REGS;
        else if (released & SCE_CTRL_RIGHT)
            guistate.active_area = MEMVIEW_STACK;
        return;
    }

    if (guistate.active_area == MEMVIEW_HEX)
    {
        if (released & guistate.hotkeys.edit)
        {
            if (guistate.edit_mode == EDIT_NONE)
            {
                guistate.edit_mode = EDIT_ADDRESS;
                guistate.modified_addr = guistate.addr;
                kernel_read_memory((void *)guistate.addr, guistate.modified_value,
                                   layout_info[guistate.mem_layout].bytes);
            }
            else
                guistate.edit_mode = (guistate.edit_mode + 1) % 3;
        }

        if (guistate.edit_mode == EDIT_ADDRESS)
        {
            int delta = (released & SCE_CTRL_UP ? 1 : 0) + (released & SCE_CTRL_DOWN ? -1 : 0) +
                        (released & SCE_CTRL_RIGHT ? 1 : 0) + (released & SCE_CTRL_LEFT ? -1 : 0);
            guistate.modified_addr += delta * layout_info[guistate.mem_layout].bytes;
            guistate.modified_addr = CLAMP(guistate.modified_addr, (uint32_t)guistate.modinfo.segments[0].vaddr,
                                           (uint32_t)guistate.modinfo.segments[0].vaddr + guistate.modinfo.size);

            if (released & guistate.hotkeys.confirm)
            {
                guistate.addr = guistate.modified_addr;
                guistate.edit_mode = EDIT_NONE;
            }
        }
        else if (guistate.edit_mode == EDIT_VALUE)
        {
            SceSize size = layout_info[guistate.mem_layout].bytes;
            if (released & SCE_CTRL_LEFT)
                guistate.edit_offset = (guistate.edit_offset - 1 + size) % size;
            if (released & SCE_CTRL_RIGHT)
                guistate.edit_offset = (guistate.edit_offset + 1) % size;

            if (released & SCE_CTRL_UP)
                guistate.modified_value[guistate.edit_offset]++;
            if (released & SCE_CTRL_DOWN)
                guistate.modified_value[guistate.edit_offset]--;

            if (released & guistate.hotkeys.confirm)
            {
                kernel_write_memory((void *)guistate.addr, guistate.modified_value, size);
                guistate.edit_mode = EDIT_NONE;
            }
        }
        else
        {
            int delta = (released & SCE_CTRL_UP ? -16 : 0) + (released & SCE_CTRL_DOWN ? 16 : 0) +
                        (released & SCE_CTRL_LEFT ? -layout_info[guistate.mem_layout].bytes : 0) +
                        (released & SCE_CTRL_RIGHT ? layout_info[guistate.mem_layout].bytes : 0);
            guistate.addr = CLAMP(guistate.addr + delta, (uint32_t)guistate.modinfo.segments[0].vaddr,
                                  (uint32_t)guistate.modinfo.segments[0].vaddr + guistate.modinfo.size);
        }

        if (released & SCE_CTRL_L1)
            guistate.mem_layout = (guistate.mem_layout + 2) % 3;
        if (released & SCE_CTRL_R1)
            guistate.mem_layout = (guistate.mem_layout + 1) % 3;
        if (released & SCE_CTRL_TRIANGLE)
            kernel_set_hardware_breakpoint(guistate.addr);
        if (released & SCE_CTRL_SQUARE)
            kernel_set_software_breakpoint(guistate.addr, guistate.regs.cpsr & 0x20);
    }
    else if (guistate.active_area == MEMVIEW_STACK)
    {
        if (released & SCE_CTRL_LEFT)
            guistate.view_state = (guistate.view_state - 1 + 3) % 3;
        if (released & SCE_CTRL_RIGHT)
            guistate.view_state = (guistate.view_state + 1) % 3;
    }
}

static void draw_ui(void)
{
    rendererv_drawRectangle(0, 0, 960, 544, 0x80171717);
    ksceKernelPrintf("BLABLABLA!!!!!.");

    switch (guistate.ui_state)
    {
    case UI_WELCOME: {
        rendererv_drawString(100, 80, "Welcome to PebbleVita Debugger!!!");

        //if (g_target_process.pid > 0)
        //{
        //    rendererv_drawStringF(100, 140, "Process ID: %08X", g_target_process.pid);
        //    rendererv_drawStringF(100, 200, "Press %d to continue to HEX view...", guistate.hotkeys.confirm);
        //}
        break;
    }

    case UI_MEMVIEW: {
        for (uint32_t addr = guistate.base_addr; addr < guistate.base_addr + 0x100; addr += 16)
            draw_hex_row(addr);

        if (guistate.active_area == MEMVIEW_REGS)
            draw_registers(700, 80);
        else if (guistate.active_area == MEMVIEW_STACK)
        {
            switch (guistate.view_state)
            {
            case VIEW_STACK:
                for (int i = 0; i < guistate.stack_size && i < 16; i++)
                    rendererv_drawStringF(700, 100 + i * 20, "%08X: %08X", guistate.regs.sp + i * 4, guistate.stack[i]);

                break;
            case VIEW_CALLSTACK:
                for (int i = 0; i < guistate.callstack_size; i++)
                    rendererv_drawStringF(700, 100 + i * 20, "[%d] %08X", i, guistate.callstack[i]);

                break;
            case VIEW_BREAKPOINTS:
                for (int i = 0; i < 16; i++)
                {
                    if (guistate.breakpoints[i].type)
                    {
                        const char *types[] = {"SW", "HW", "WP-R", "WP-W", "WP-RW"};
                        rendererv_drawStringF(700, 100 + i * 20, "[%d] %s @ %08X", i,
                                              types[guistate.breakpoints[i].type - 1], guistate.breakpoints[i].address);
                    }
                }
                break;
            }
        }
        break;
    }

    case UI_FEATURES: {
        const char *features[] = {"HW Break", "Watchpoint", "SW Break", "Clear BP",
                                  "Suspend",  "Resume",     "Step",     "Hotkeys"};
        for (uint32_t i = 0; i < 8; i++)
            rendererv_drawStringF(50, 60 + i * 25, features[i], i == guistate.edit_feature ? 0xFF00FF00 : 0xFFFFFFFF);
        
        break;
    }

    default:
        break;
    }
}

int pebble_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    SceCtrlData ctrl;

    while (1)
    {
        ksceCtrlPeekBufferPositive(0, &ctrl, 1);
        uint32_t released = (guistate.prev_buttons & ~ctrl.buttons);
        uint32_t current = ctrl.buttons;

        if (guistate.ui_state == UI_WELCOME && (released & guistate.hotkeys.confirm))
        {
            register_handler();
            SceKernelModuleInfo info = {.size = sizeof(SceKernelModuleInfo)};
            kernel_get_moduleinfo(&info);
            guistate.modinfo = info;
            guistate.ui_state = UI_MEMVIEW;
            guistate.addr = (uint32_t)info.segments[0].vaddr;
        }
        else if (guistate.ui_state >= UI_FEATURE_HW_BREAK)
            handle_feature_input(released);
        else if (guistate.ui_state == UI_FEATURES)
        {
            if (released & SCE_CTRL_UP)
                guistate.edit_feature = (guistate.edit_feature - 1 + 8) % 8;
            if (released & SCE_CTRL_DOWN)
                guistate.edit_feature = (guistate.edit_feature + 1) % 8;
            if (released & guistate.hotkeys.confirm)
            {
                switch (guistate.edit_feature)
                {
                case 0:
                    guistate.ui_state = UI_FEATURE_HW_BREAK;
                    guistate.edit_feature = guistate.addr;
                    break;
                case 2:
                    guistate.ui_state = UI_FEATURE_SW_BREAK;
                    guistate.edit_feature = guistate.addr;
                    break;
                case 3:
                    guistate.ui_state = UI_FEATURE_CLEAR;
                    guistate.edit_feature = 0;
                    break;
                case 7:
                    guistate.ui_state = UI_FEATURE_HOTKEYS;
                    guistate.edit_feature = 0;
                    break;
                default:
                    guistate.ui_state = UI_FEATURES + guistate.edit_feature + 1;
                    break;
                }
            }
            if (released & guistate.hotkeys.cancel)
                guistate.ui_state = UI_MEMVIEW;
        }

        if (released & guistate.hotkeys.show_gui)
            guistate.gui_visible = !guistate.gui_visible;

        if (guistate.gui_visible)
        {
            if (rendererv_allocVirtualFB() >= 0)
            {
                SceDisplayFrameBuf fb;
                fb.size = sizeof(SceDisplayFrameBuf);
                if (ksceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0)
                {
                    renderer_setFB(&fb);
                    draw_ui();
                    renderer_writeFromVFB(0, false);
                    ksceKernelPrintf("Success checkpoint 0000.");
                } 
                rendererv_freeVirtualFB();
                ksceKernelPrintf("Success checkpoint 8888.");
            }

            if (guistate.ui_state == UI_MEMVIEW)
            {
                handle_memview_input(released, current);
                update_memview_state();
            }
        }

        guistate.prev_buttons = current;
        ksceKernelDelayThread(33333);
    }
    return 0;
}