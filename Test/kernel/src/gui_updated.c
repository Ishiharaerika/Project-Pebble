#include "kernel.h"
#include "renderer.h"

State guistate;
static uint32_t start_addr = 0;

static const MemLayoutInfo layout_info[] = {
    [MEM_LAYOUT_8BIT] = {"%02X", 1}, [MEM_LAYOUT_16BIT] = {"%04X", 2}, [MEM_LAYOUT_32BIT] = {"%08X", 4}};

void load_hotkeys(void)
{
    guistate.hotkeys =
        (HotkeyConfig){DEFAULT_SHOW_GUI, DEFAULT_AREA_SELECT, DEFAULT_EDIT_MODE, DEFAULT_CANCEL, DEFAULT_CONFIRM};
    SceUID fd = ksceIoOpen(HOTKEY_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0)
    {
        char buf[64] = {0};
        ksceIoRead(fd, buf, sizeof(buf) - 1);
        sscanf(buf, "%x %x %x %x %x", &guistate.hotkeys.show_gui, &guistate.hotkeys.area_select, &guistate.hotkeys.edit,
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
        int len = snprintf(buf, sizeof(buf), "%x %x %x %x %x", guistate.hotkeys.show_gui, guistate.hotkeys.area_select,
                           guistate.hotkeys.edit, guistate.hotkeys.cancel, guistate.hotkeys.confirm);
        if (len > 0)
            ksceIoWrite(fd, buf, len);
        ksceIoClose(fd);
    }
    load_hotkeys();
}

static void update_memview_state(void)
{
    kernel_list_breakpoints(guistate.breakpoints);
    guistate.has_active_bp = false;
    for (int i = 0; i < MAX_SLOT; i++)
    {
        if (guistate.breakpoints[i].type)
        {
            guistate.has_active_bp = true;
            kernel_get_registers(&guistate.regs);
            guistate.stack_size =
                kernel_read_memory((void *)guistate.regs.sp, guistate.stack, sizeof(guistate.stack)) / 4;
            if (guistate.stack_size < 0)
                guistate.stack_size = 0;
            guistate.callstack_size = MAX_CALL_STACK_DEPTH;
            kernel_get_callstack(guistate.callstack, guistate.callstack_size);
            break;
        }
    }
    if (!guistate.has_active_bp)
    {
        memset(&guistate.regs, 0, sizeof(guistate.regs));
        guistate.stack_size = 0;
        guistate.callstack_size = 0;
    }
}

static void read_memview_cache(void)
{
    int ret = kernel_read_memory((void *)guistate.base_addr, guistate.cached_mem, sizeof(guistate.cached_mem));
    if (ret < 0)
        memset(guistate.cached_mem, 0xFF, sizeof(guistate.cached_mem));
}

static void draw_hex_row(uint32_t addr, const uint8_t *data)
{
    char line[64];
    char ascii[9];
    const MemLayoutInfo *layout = &layout_info[guistate.mem_layout];
    uint32_t pos = snprintf(line, sizeof(line), "%08X ", addr);
    if (data && pos < sizeof(line) - 1 && addr >= start_addr)
    {
        for (int i = 0; i < 8; i += layout->bytes)
        {
            if (pos >= sizeof(line) - (layout->bytes * 2 + 2))
                break;
            uint32_t val = 0;
            memcpy(&val, data + i, layout->bytes);
            pos += snprintf(line + pos, sizeof(line) - pos, " %0*X", layout->bytes * 2, val);
            for (int j = 0; j < layout->bytes; j++)
            {
                if (i + j < 8)
                {
                    uint8_t c = data[i + j];
                    ascii[i + j] = (c > 31 && c < 127) ? c : '.';
                }
            }
        }
        ascii[8] = '\0';
    }
    else
    {
        if (pos < sizeof(line) - 1)
            strcpy(line + pos, " Read Error");
        memset(ascii, '.', 8);
        ascii[8] = '\0';
    }
    const int ypos = 20 + ((addr - guistate.base_addr) / 8) * FONT_HEIGHT;
    if (addr == guistate.addr)
        renderer_drawRectangle(0, ypos, 400, FONT_HEIGHT, 0x80FFFFFF);

    char final_line[128];
    snprintf(final_line, sizeof(final_line), "%s  %s", line, ascii);
    renderer_drawString(0, ypos, final_line);
}

static void draw_registers(int x, int y)
{
    if (!guistate.has_active_bp)
        return;
    const char *regs[] = {"R0", "R1", "R2",  "R3",  "R4",  "R5", "R6", "R7",
                          "R8", "R9", "R10", "R11", "R12", "SP", "LR", "PC"};
    uint32_t *reg_ptr = (uint32_t *)&guistate.regs;
    for (int i = 0; i < 16; i++, y += FONT_HEIGHT)
        renderer_drawStringF(x, y, "%-3s:%08X", regs[i], reg_ptr[i]);
}

static void handle_feature_input(uint32_t released)
{
    bool confirmed = (released & guistate.hotkeys.confirm);
    bool cancelled = (released & guistate.hotkeys.cancel);
    switch (guistate.ui_state)
    {
    case UI_FEATURES: {
        int cur_feat = guistate.edit_feature;
        if (released & SCE_CTRL_UP)
            cur_feat = (cur_feat - 1 + 8) % 8;
        if (released & SCE_CTRL_DOWN)
            cur_feat = (cur_feat + 1) % 8;
        guistate.edit_feature = cur_feat;
        if (confirmed)
        {
            switch (cur_feat)
            {
            case 0:
                guistate.ui_state = UI_FEATURE_HW_BREAK;
                guistate.edit_feature = guistate.addr;
                break;
            case 1:
                guistate.ui_state = UI_FEATURE_WATCH;
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
            case 4:
                guistate.ui_state = UI_FEATURE_SUSPEND;
                kernel_suspend_process();
                break;
            case 5:
                guistate.ui_state = UI_FEATURE_RESUME;
                kernel_resume_process();
                break;
            case 6:
                guistate.ui_state = UI_FEATURE_STEP;
                kernel_single_step();
                break;
            case 7:
                guistate.ui_state = UI_FEATURE_HOTKEYS;
                guistate.edit_feature = 0;
                break;
            }
            if (guistate.ui_state == UI_FEATURE_SUSPEND || guistate.ui_state == UI_FEATURE_RESUME ||
                guistate.ui_state == UI_FEATURE_STEP)
                guistate.ui_state = UI_MEMVIEW;
        }
        break;
    }
    case UI_FEATURE_HW_BREAK: {
        guistate.edit_feature += (released & SCE_CTRL_UP ? 0x10 : 0) - (released & SCE_CTRL_DOWN ? 0x10 : 0);
        if (confirmed)
        {
            kernel_set_hardware_breakpoint(guistate.edit_feature);
            guistate.ui_state = UI_MEMVIEW;
        }
        break;
    }
    case UI_FEATURE_WATCH: {
        guistate.edit_feature += (released & SCE_CTRL_UP ? 0x10 : 0) - (released & SCE_CTRL_DOWN ? 0x10 : 0);
        if (confirmed)
        {
            kernel_set_watchpoint(guistate.edit_feature, BREAK_READ_WRITE);
            guistate.ui_state = UI_MEMVIEW;
        }
        break;
    }
    case UI_FEATURE_SW_BREAK: {
        guistate.edit_feature += (released & SCE_CTRL_UP ? 1 : 0) - (released & SCE_CTRL_DOWN ? 1 : 0);
        if (confirmed)
        {
            kernel_set_software_breakpoint(guistate.edit_feature, SW_BREAKPOINT_THUMB);
            guistate.ui_state = UI_MEMVIEW;
        }
        break;
    }
    case UI_FEATURE_CLEAR: {
        int delta = (released & SCE_CTRL_UP ? 1 : 0) - (released & SCE_CTRL_DOWN ? 1 : 0);
        if (delta)
            guistate.edit_feature = CLAMP(guistate.edit_feature + delta, 0, MAX_SLOT - 1);
        if (confirmed)
        {
            kernel_clear_breakpoint(guistate.edit_feature);
            guistate.ui_state = UI_MEMVIEW;
        }
        break;
    }
    case UI_FEATURE_HOTKEYS: {
        int cur_idx = guistate.edit_feature;
        if (released & SCE_CTRL_UP)
            cur_idx = (cur_idx - 1 + 5) % 5;
        if (released & SCE_CTRL_DOWN)
            cur_idx = (cur_idx + 1) % 5;
        guistate.edit_feature = cur_idx;
        uint32_t *key_ptr = (uint32_t *)&((uint32_t *)&guistate.hotkeys)[cur_idx]; // Pointer to the selected hotkey
        if (released & SCE_CTRL_LEFT)
        {
            if (*key_ptr > 0)
                *key_ptr >>= 1;
        }
        if (released & SCE_CTRL_RIGHT)
        {
            if (*key_ptr < 0x80000000)
                *key_ptr = (*key_ptr == 0) ? 1 : (*key_ptr << 1);
        }
        if (confirmed)
        {
            save_hotkeys();
            guistate.ui_state = UI_MEMVIEW;
        }
        break;
    }
    default:
        break;
    }
    if (cancelled && guistate.ui_state >= UI_FEATURE_HW_BREAK)
        guistate.ui_state = UI_FEATURES;
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
    if (released & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))
    {
        guistate.ui_state = UI_FEATURES;
        guistate.edit_feature = 0;
        return;
    }
    if (guistate.active_area == MEMVIEW_HEX)
    {
        bool layout_changed = false;
        uint32_t prev_addr = guistate.addr;
        const MemLayoutInfo *layout = &layout_info[guistate.mem_layout];
        int layout_bytes = layout->bytes;
        if (released & guistate.hotkeys.edit)
        {
            guistate.edit_mode = (guistate.edit_mode + 1) % 3;
            if (guistate.edit_mode != EDIT_NONE)
            {
                guistate.modified_addr = guistate.addr;
                kernel_read_memory((void *)guistate.addr, guistate.modified_value, layout_bytes);
                guistate.edit_offset = 0;
            }
        }
        if (released & SCE_CTRL_L1)
        {
            guistate.mem_layout = (guistate.mem_layout + 2) % 3;
            layout_changed = true;
        }
        if (released & SCE_CTRL_R1)
        {
            guistate.mem_layout = (guistate.mem_layout + 1) % 3;
            layout_changed = true;
        }
        if (layout_changed && guistate.edit_mode != EDIT_NONE)
        {
            kernel_read_memory((void *)guistate.addr, guistate.modified_value, layout_info[guistate.mem_layout].bytes);
            guistate.edit_offset = 0;
        }
        if (guistate.edit_mode == EDIT_ADDRESS)
        {
            int delta = (released & SCE_CTRL_UP ? 0x10 : 0) - (released & SCE_CTRL_DOWN ? 0x10 : 0) +
                        (released & SCE_CTRL_RIGHT ? layout_bytes : 0) - (released & SCE_CTRL_LEFT ? layout_bytes : 0);
            if (delta)
            {
                guistate.modified_addr =
                    CLAMP(guistate.modified_addr + delta, (uint32_t)guistate.modinfo.segments[0].vaddr,
                          (uint32_t)guistate.modinfo.segments[0].vaddr + guistate.modinfo.size - layout_bytes);
                kernel_read_memory((void *)guistate.modified_addr, guistate.modified_value, layout_bytes);
                guistate.edit_offset = 0;
            }
            if (released & guistate.hotkeys.confirm)
            {
                guistate.addr = guistate.modified_addr;
                guistate.edit_mode = EDIT_NONE;
                if ((guistate.addr & ~0xFF) != (prev_addr & ~0xFF))
                {
                    guistate.base_addr = guistate.addr & ~0xFF;
                    read_memview_cache();
                }
            }
        }
        else if (guistate.edit_mode == EDIT_VALUE)
        {
            SceSize size = layout_bytes;
            if (released & SCE_CTRL_LEFT)
                guistate.edit_offset = (guistate.edit_offset - 1 + size) % size;
            if (released & SCE_CTRL_RIGHT)
                guistate.edit_offset = (guistate.edit_offset + 1) % size;
            int delta = (released & SCE_CTRL_UP ? 1 : (released & SCE_CTRL_DOWN ? -1 : 0));
            if (delta)
                guistate.modified_value[guistate.edit_offset] += delta;
            if (released & guistate.hotkeys.confirm)
            {
                kernel_write_memory((void *)guistate.addr, guistate.modified_value, size);
                guistate.edit_mode = EDIT_NONE;
                read_memview_cache();
            }
        }
        else
        {
            int delta = (released & SCE_CTRL_UP ? -8 : 0) + (released & SCE_CTRL_DOWN ? 8 : 0) +
                        (released & SCE_CTRL_RIGHT ? layout_bytes : 0) - (released & SCE_CTRL_LEFT ? layout_bytes : 0);
            if (delta)
            {
                guistate.addr =
                    CLAMP(guistate.addr + delta, (uint32_t)guistate.modinfo.segments[0].vaddr,
                          (uint32_t)guistate.modinfo.segments[0].vaddr + guistate.modinfo.size - layout_bytes);
                if ((guistate.addr & ~0xFF) != (guistate.base_addr))
                {
                    guistate.base_addr = guistate.addr & ~0xFF;
                    read_memview_cache();
                }
            }
        }
        if (released & SCE_CTRL_TRIANGLE)
            kernel_set_watchpoint(guistate.addr, BREAK_READ_WRITE);
        if (released & SCE_CTRL_SQUARE)
            kernel_set_software_breakpoint(guistate.addr, SW_BREAKPOINT_THUMB);
    }
    else if (guistate.active_area == MEMVIEW_STACK)
    {
        if (released & SCE_CTRL_LEFT)
            guistate.view_state = (guistate.view_state - 1 + 3) % 3;
        if (released & SCE_CTRL_RIGHT)
            guistate.view_state = (guistate.view_state + 1) % 3;
    }
    if (released & guistate.hotkeys.cancel)
    {
        if (guistate.edit_mode != EDIT_NONE)
            guistate.edit_mode = EDIT_NONE;
    }
}

void draw_gui(void)
{
    renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
    update_memview_state();
    switch (guistate.ui_state)
    {
    case UI_WELCOME:
        renderer_setColor(0xFFFFFFFF);
        renderer_drawString(100, 80, "Welcome to Pebble Vita Debugger!!!");
        if (g_target_process.pid)
        {
            renderer_drawStringF(100, 140, "Process ID: %#X", g_target_process.pid);
            renderer_drawStringF(100, 200, "Press O to continue to HEX view...");
        }
        else
            renderer_drawString(100, 140, "Nothing's here...");
        break;
    case UI_MEMVIEW: {
        renderer_setColor(0xFFFFFFFF);
        const int visible_lines = (UI_HEIGHT - FONT_HEIGHT) / FONT_HEIGHT;
        renderer_setColor(0x80808080);
        if (guistate.active_area == MEMVIEW_HEX)
            renderer_drawRectangle(0, 20, 300 + FONT_WIDTH + 8 * FONT_WIDTH, visible_lines * FONT_HEIGHT, 0x40FFFFFF);
        else if (guistate.active_area == MEMVIEW_REGS)
            renderer_drawRectangle(700, 60, 200, 17 * FONT_HEIGHT, 0x40FFFFFF);
        else if (guistate.active_area == MEMVIEW_STACK)
            renderer_drawRectangle(700, 80, 200, 17 * FONT_HEIGHT, 0x40FFFFFF);
        renderer_setColor(0xFFFFFFFF);
        for (int i = 0; i < visible_lines; i++)
        {
            const uint32_t line_addr = guistate.base_addr + i * 8;
            const uint8_t *data_ptr = NULL;
            if (line_addr >= guistate.base_addr && line_addr < (guistate.base_addr + sizeof(guistate.cached_mem)))
                data_ptr = guistate.cached_mem + (line_addr - guistate.base_addr);
            if (data_ptr && line_addr >= start_addr)
                draw_hex_row(line_addr, data_ptr);
        }
        if (guistate.has_active_bp)
        {
            int right_panel_x = 700;
            int right_panel_y = 80;
            renderer_setColor(0xFFFFFFFF);
            if (guistate.active_area == MEMVIEW_REGS)
            {
                renderer_drawString(right_panel_x, 60, "Registers:");
                draw_registers(right_panel_x, right_panel_y);
            }
            else if (guistate.active_area == MEMVIEW_STACK)
            {
                const char *view_titles[] = {"Stack:", "Callstack:", "Breakpoints:"};
                renderer_drawString(right_panel_x, right_panel_y, view_titles[guistate.view_state]);
                int y_pos = right_panel_y + FONT_HEIGHT;
                switch (guistate.view_state)
                {
                case VIEW_STACK:
                    for (int i = 0; i < guistate.stack_size && i < 16; i++, y_pos += FONT_HEIGHT)
                        renderer_drawStringF(right_panel_x, y_pos, "%08X:%08X", guistate.regs.sp + i * 4,
                                             guistate.stack[i]);
                    break;
                case VIEW_CALLSTACK:
                    for (int i = 0; i < guistate.callstack_size && i < MAX_CALL_STACK_DEPTH; i++, y_pos += FONT_HEIGHT)
                        renderer_drawStringF(right_panel_x, y_pos, "[%d] %08X", i, guistate.callstack[i]);
                    break;
                case VIEW_BREAKPOINTS: {
                    const char *types[] = {"SW-Thumb", "SW-Arm", "Hardware",  "WP-Read",
                                           "WP-Write", "WP-RW",  "SingleStep"};
                    const int num_types = sizeof(types) / sizeof(types[0]);
                    for (int i = 0; i < MAX_SLOT; i++, y_pos += FONT_HEIGHT)
                    {
                        ActiveBKPTSlot *bp = &guistate.breakpoints[i];
                        if (bp->type)
                        {
                            renderer_setColor(bp->pid == g_target_process.pid ? 0xFF00FF00 : 0xFFFFFFFF);
                            int type_idx = bp->type - 1;
                            const char *type_str = (type_idx >= 0 && type_idx < num_types) ? types[type_idx] : "?";
                            renderer_drawStringF(right_panel_x, y_pos, "[%d]%s@%08X", i, type_str, bp->address);
                        }
                        else
                        {
                            renderer_setColor(0xFF808080);
                            renderer_drawStringF(right_panel_x, y_pos, "[%d] Empty", i);
                        }
                    }
                    break;
                }
                }
            }
        }
        else
        {
            renderer_setColor(0xFF808080);
            if (guistate.active_area == MEMVIEW_REGS)
                renderer_drawString(700, 80, "No Active BP");
            if (guistate.active_area == MEMVIEW_STACK)
                renderer_drawString(700, 100, "No Active BP");
        }
        break;
    }
    case UI_FEATURES: {
        renderer_setColor(0xFFFFFFFF);
        const char *features[] = {"HW Breakpoint",   "Watchpoint",     "SW Breakpoint", "Clear Breakpoint",
                                  "Suspend Process", "Resume Process", "Single Step",   "Hotkeys"};
        const int num_features = sizeof(features) / sizeof(features[0]);
        renderer_drawString(50, 30, "Features:");
        int y_pos = 60;
        for (uint32_t i = 0; i < num_features; i++, y_pos += 25)
        {
            renderer_setColor(i == guistate.edit_feature ? 0xFF00FF00 : 0xFFFFFFFF);
            renderer_drawString(50, y_pos, features[i]);
        }
        break;
    }
    case UI_FEATURE_HW_BREAK:
    case UI_FEATURE_WATCH:
    case UI_FEATURE_SW_BREAK:
    case UI_FEATURE_CLEAR: {
        renderer_setColor(0xFFFFFF00);
        const char *editing_feature = "Unknown";
        if (guistate.ui_state == UI_FEATURE_HW_BREAK)
            editing_feature = "HW Breakpoint Address";
        else if (guistate.ui_state == UI_FEATURE_WATCH)
            editing_feature = "Watchpoint Address";
        else if (guistate.ui_state == UI_FEATURE_SW_BREAK)
            editing_feature = "SW Breakpoint Address";
        else if (guistate.ui_state == UI_FEATURE_CLEAR)
            editing_feature = "Breakpoint Slot Index";
        renderer_drawStringF(50, 60, "Editing: %s", editing_feature);
        renderer_drawStringF(50, 90, "Value: %08X", guistate.edit_feature);
        renderer_setColor(0xFFFFFFFF);
        renderer_drawString(50, 120, "Use D-Pad Up/Down to Change");
        renderer_drawString(50, 145, "Press O to Confirm");
        renderer_drawString(50, 170, "Press X to Cancel");
        break;
    }
    case UI_FEATURE_HOTKEYS: {
        renderer_setColor(0xFFFFFFFF);
        const char *hotkey_names[] = {"Show GUI", "Area Select", "Edit Mode", "Cancel", "Confirm"};
        const int num_hotkey_names = sizeof(hotkey_names) / sizeof(hotkey_names[0]);
        uint32_t *keys = (uint32_t *)&guistate.hotkeys;
        renderer_drawString(50, 30, "Edit Hotkeys:");
        int y_pos = 60;
        for (uint32_t i = 0; i < num_hotkey_names; ++i, y_pos += 25)
        {
            renderer_setColor(i == guistate.edit_feature ? 0xFF00FF00 : 0xFFFFFFFF);
            renderer_drawStringF(50, y_pos, "%s: %04X", hotkey_names[i], keys[i]);
        }
        renderer_setColor(0xFFFFFFFF);
        y_pos += 10;
        renderer_drawString(50, y_pos, "Use Left/Right to change bit");
        y_pos += 25;
        renderer_drawString(50, y_pos, "Use Up/Down to select hotkey");
        y_pos += 25;
        renderer_drawString(50, y_pos, "Press O to Save");
        y_pos += 25;
        renderer_drawString(50, y_pos, "Press X to Cancel");
        break;
    }
    case UI_FEATURE_SUSPEND:
    case UI_FEATURE_RESUME:
    case UI_FEATURE_STEP:
        break;
    default:
        renderer_setColor(0xFFFF0000);
        renderer_drawStringF(100, 80, "Unhandled UI State: %d", guistate.ui_state);
        break;
    }
}

int pebble_thread(SceSize args, void *argp)
{
    if (renderer_init() < 0)
        return -1;
    ksceKernelDelayThread(8 * 1000 * 1000);
    register_handler();
    (void)args;
    (void)argp;
    SceCtrlData ctrl;
    memset(&guistate, 0, sizeof(guistate));
    while (1)
    {
        if (g_target_process.pid)
        {
            ksceCtrlPeekBufferPositive(0, &ctrl, 1);
            uint32_t released = (guistate.prev_buttons & ~ctrl.buttons);
            uint32_t current = ctrl.buttons;
            uint32_t show_gui_key = guistate.hotkeys.show_gui;
            if ((current == show_gui_key) && (guistate.prev_buttons != show_gui_key))
            {
                if (guistate.gui_visible)
                {
                    guistate.gui_visible = false;
                    renderer_destroy();
                }
                else
                {
                    if (renderer_init() == 0)
                    {
                        guistate.gui_visible = true;
                        if (guistate.ui_state == UI_MEMVIEW)
                            read_memview_cache();
                    }
                }
            }
            else if (guistate.gui_visible && guistate.ui_state == UI_WELCOME && (released & guistate.hotkeys.cancel))
            {
                guistate.gui_visible = false;
                renderer_destroy();
            }
            if (guistate.gui_visible)
            {
                if (!g_target_process.pid)
                {
                    if (guistate.ui_state != UI_WELCOME)
                        guistate.ui_state = UI_WELCOME;
                }
                else
                {
                    if (guistate.ui_state == UI_WELCOME)
                    {
                        if (released & guistate.hotkeys.confirm)
                        {
                            kernel_debugger_attach();
                            SceKernelModuleInfo info = {.size = sizeof(SceKernelModuleInfo)};
                            if (kernel_get_moduleinfo(&info) >= 0)
                            {
                                guistate.modinfo = info;
                                uint32_t lowest_vaddr = 0;
                                uint32_t temp;

                                for (int i = 0; i < 4; ++i) 
                                {
                                    if (info.segments[i].vaddr != NULL) 
                                    {
                                        temp = (uint32_t)info.segments[i].vaddr;
                                        if (lowest_vaddr == 0 || temp < lowest_vaddr)
                                            lowest_vaddr = temp;
                                    }
                                }
                                if (lowest_vaddr) 
                                {
                                    guistate.ui_state = UI_MEMVIEW;
                                    guistate.addr = lowest_vaddr;
                                    guistate.base_addr = guistate.addr;
                                    read_memview_cache();
                                    start_addr = lowest_vaddr;
                                    lowest_vaddr = 0;
                                }
                                else
                                    start_addr = 0x84000000;
                            }
                            else
                                guistate.ui_state = UI_WELCOME;
                        }
                    }
                    else if (guistate.ui_state == UI_MEMVIEW)
                        handle_memview_input(released, current);
                    else if (guistate.ui_state >= UI_FEATURES)
                        handle_feature_input(released);
                }
                if (ksceKernelLockMutex(pebble_mtx_uid, 1, NULL) >= 0)
                {
                    draw_gui();
                    ksceKernelUnlockMutex(pebble_mtx_uid, 1);
                }
            }
            ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
            guistate.prev_buttons = current;
        }
        ksceKernelDelayThread(16666);
    }
    renderer_destroy();
    return 0;
}