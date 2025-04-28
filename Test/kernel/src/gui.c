#include "kernel.h"
#include "renderer.h"

State guistate;
uint32_t lowest_vaddr = 0x84000000;
uint32_t highest_vaddr = 0x85000000;

static const MemLayoutInfo layout_info[] = {
    [MEM_LAYOUT_8BIT] = {"%02X", 1}, [MEM_LAYOUT_16BIT] = {"%04X", 2}, [MEM_LAYOUT_32BIT] = {"%08X", 4}};

static bool cache_dirty = true;

void load_hotkeys(void)
{
    guistate.hotkeys = (HotkeyConfig){DEFAULT_SHOW_GUI, DEFAULT_AREA_SELECT, DEFAULT_CANCEL, DEFAULT_CONFIRM};
    SceUID fd = ksceIoOpen(HOTKEY_PATH, SCE_O_RDONLY, 0);
    if (fd < 0)
        return;

    char buf[64] = {0};
    ksceIoRead(fd, buf, sizeof(buf) - 1);
    sscanf(buf, "%x %x %x %x", &guistate.hotkeys.show_gui, &guistate.hotkeys.area_select, &guistate.hotkeys.cancel,
           &guistate.hotkeys.confirm);
    ksceIoClose(fd);
}

static void save_hotkeys(void)
{
    SceUID fd = ksceIoOpen(HOTKEY_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return;

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%x %x %x %x", guistate.hotkeys.show_gui, guistate.hotkeys.area_select,
                       guistate.hotkeys.cancel, guistate.hotkeys.confirm);
    if (len > 0)
        ksceIoWrite(fd, buf, len);
    ksceIoClose(fd);
    load_hotkeys();
}

static void update_memview_state(void)
{
    guistate.has_active_bp = false;
    kernel_list_breakpoints(guistate.breakpoints);

    for (int i = 0; i < MAX_SLOT; i++)
    {
        if (guistate.breakpoints[i].type)
        {
            guistate.has_active_bp = true;
            break;
        }
    }

    if (!guistate.has_active_bp)
    {
        memset(&guistate.regs, 0, sizeof(guistate.regs));
        guistate.stack_size = 0;
        guistate.callstack_size = 0;
        return;
    }

    kernel_get_registers(&guistate.regs);
    guistate.stack_size = kernel_read_memory((void *)guistate.regs.sp, guistate.stack, sizeof(guistate.stack));
    if (guistate.stack_size > 0)
        guistate.stack_size /= 4;
    else
        guistate.stack_size = 0;

    guistate.callstack_size = MAX_CALL_STACK_DEPTH;
    kernel_get_callstack(guistate.callstack, guistate.callstack_size);
}

static void read_memview_cache(void)
{
    if (!cache_dirty)
        return;

    int ret = kernel_read_memory((void *)guistate.base_addr, guistate.cached_mem, sizeof(guistate.cached_mem));
    if (ret < 0)
        memset(guistate.cached_mem, 0xFF, sizeof(guistate.cached_mem));

    cache_dirty = false;
}

static inline char nibble_to_hex(uint8_t nibble)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    return hex_chars[nibble & 0xF];
}

static void draw_hex_row(uint32_t addr, const uint8_t *data)
{
    if (addr < guistate.base_addr || addr >= guistate.base_addr + sizeof(guistate.cached_mem))
        return;

    const int ypos = 10 + ((addr - guistate.base_addr) / 8) * FONT_HEIGHT;
    const bool is_selected_row = (addr == guistate.addr);

    char addr_str[9];
    uint32_t temp_addr = (is_selected_row && guistate.edit_mode == EDIT_ADDRESS) ? guistate.modified_addr : addr;

    for (int i = 7; i >= 0; i--)
    {
        addr_str[i] = nibble_to_hex(temp_addr & 0xF);
        temp_addr >>= 4;
    }
    addr_str[8] = '\0';
    renderer_drawString(0, ypos, addr_str);

    if (!data)
    {
        renderer_drawString(8 * FONT_WIDTH, ypos, " Read Error");
        renderer_drawString((10 + 10) * FONT_WIDTH, ypos, "........");
        return;
    }

    const MemLayoutInfo *layout = &layout_info[guistate.mem_layout];
    const int bytes_per_value = layout->bytes;
    const int values_per_row = 8 / bytes_per_value;
    const int hex_chars = bytes_per_value * 2;

    char hex_str[28] = " "; 
    int hex_pos = 1;        
    char ascii_str[9];

    for (int i = 0; i < values_per_row; i++)
    {
        const bool is_edited = is_selected_row && guistate.edit_mode == EDIT_VALUE && guistate.cursor_column - 1 == i;

        if (i > 0)
            hex_str[hex_pos++] = ' ';

        uint32_t val = 0;
        const uint8_t *src = is_edited ? guistate.modified_value : data + i * bytes_per_value;

        memcpy(&val, src, bytes_per_value);

        for (int j = 0; j < hex_chars; j++)
            hex_str[hex_pos++] = nibble_to_hex((val >> (4 * (hex_chars - j - 1))) & 0xF);
        for (int b = 0; b < bytes_per_value; b++)
        {
            uint8_t byte = src[b];
            ascii_str[i * bytes_per_value + b] = (byte >= 32 && byte < 127) ? byte : '.';
        }
    }

    hex_str[hex_pos] = '\0';
    ascii_str[8] = '\0';

    renderer_drawString(8 * FONT_WIDTH, ypos, hex_str);
    renderer_drawString((9 + hex_pos) * FONT_WIDTH, ypos, ascii_str);

    if (is_selected_row && guistate.active_area == MEMVIEW_HEX)
    {
        int underline_x = 0, underline_w = 0;
        const int underline_y = ypos + FONT_HEIGHT - 2;

        if (guistate.edit_mode == EDIT_NONE)
        {
            if (guistate.cursor_column == 0)
            {
                underline_x = 0;
                underline_w = 8 * FONT_WIDTH;
            }
            else
            {
                const int value_index = guistate.cursor_column - 1;
                underline_x = (9 + value_index * (hex_chars + 1)) * FONT_WIDTH;
                underline_w = hex_chars * FONT_WIDTH;
            }
        }
        else
        {
            underline_w = FONT_WIDTH;
            if (guistate.edit_mode == EDIT_ADDRESS)
                underline_x = (7 - guistate.edit_offset) * FONT_WIDTH;
            else
            {
                const int value_index = guistate.cursor_column - 1;
                underline_x = (9 + value_index * (hex_chars + 1)) * FONT_WIDTH +
                              (hex_chars - 1 - guistate.edit_offset) * FONT_WIDTH;
            }
        }
        renderer_drawRectangle(underline_x, underline_y, underline_w, 1, 0xFFFFFFFF);
    }
}

static void draw_registers(int x, int y)
{
    if (!guistate.has_active_bp)
        return;

    static const char *reg_names[] = {"R0", "R1", "R2",  "R3",  "R4",  "R5", "R6", "R7",
                                      "R8", "R9", "R10", "R11", "R12", "SP", "LR", "PC"};

    uint32_t *reg_ptr = (uint32_t *)&guistate.regs;
    for (int i = 0; i < 16; i++, y += FONT_HEIGHT)
        renderer_drawStringF(x, y, "%-3s:%08X", reg_names[i], reg_ptr[i]);
}

static void handle_feature_input(uint32_t released)
{
    bool confirmed = (released & guistate.hotkeys.confirm);
    bool cancelled = (released & guistate.hotkeys.cancel);

    if (cancelled && guistate.ui_state >= UI_FEATURE_HW_BREAK)
    {
        guistate.ui_state = UI_FEATURES;
        return;
    }

    switch (guistate.ui_state)
    {
    case UI_FEATURES: {
        // Exit to memory view
        if (released & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))
        {
            guistate.ui_state = UI_MEMVIEW;
            return;
        }

        // Feature navigation
        int cur_feat = guistate.edit_feature;
        if (released & SCE_CTRL_UP)
            cur_feat = (cur_feat - 1 + 8) % 8;
        if (released & SCE_CTRL_DOWN)
            cur_feat = (cur_feat + 1) % 8;
        guistate.edit_feature = cur_feat;

        // Feature activation
        if (confirmed)
        {
            switch (cur_feat)
            {
            case 0: // Hardware breakpoint
                guistate.ui_state = UI_FEATURE_HW_BREAK;
                guistate.edit_feature = guistate.addr;
                break;
            case 1: // Watchpoint
                guistate.ui_state = UI_FEATURE_WATCH;
                guistate.edit_feature = guistate.addr;
                break;
            case 2: // Software breakpoint
                guistate.ui_state = UI_FEATURE_SW_BREAK;
                guistate.edit_feature = guistate.addr;
                break;
            case 3: // Clear breakpoint
                guistate.ui_state = UI_FEATURE_CLEAR;
                guistate.edit_feature = 0;
                break;
            case 4: // Suspend process
                kernel_suspend_process();
                guistate.ui_state = UI_MEMVIEW;
                break;
            case 5: // Resume process
                kernel_resume_process();
                guistate.ui_state = UI_MEMVIEW;
                break;
            case 6: // Single step
                kernel_single_step();
                guistate.ui_state = UI_MEMVIEW;
                break;
            case 7: // Hotkeys
                guistate.ui_state = UI_FEATURE_HOTKEYS;
                guistate.edit_feature = 0;
                break;
            }
        }
        break;
    }
    case UI_FEATURE_HW_BREAK:
    case UI_FEATURE_WATCH: {
        guistate.edit_feature += (released & SCE_CTRL_UP ? 0x10 : 0) - (released & SCE_CTRL_DOWN ? 0x10 : 0);
        if (confirmed)
        {
            if (guistate.ui_state == UI_FEATURE_HW_BREAK)
                kernel_set_hardware_breakpoint(guistate.edit_feature);
            else
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

        uint32_t *key_ptr = (uint32_t *)&((uint32_t *)&guistate.hotkeys)[cur_idx];
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
}

static void handle_memview_input(uint32_t released, uint32_t current)
{
    if (current & guistate.hotkeys.area_select)
    {
        if (released & SCE_CTRL_LEFT)
        {
            guistate.active_area = MEMVIEW_HEX;
            return;
        }
        if (released & SCE_CTRL_UP)
        {
            guistate.active_area = MEMVIEW_REGS;
            return;
        }
        if (released & SCE_CTRL_RIGHT)
        {
            guistate.active_area = MEMVIEW_STACK;
            return;
        }
        if (released & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))
        {
            guistate.ui_state = UI_FEATURES;
            guistate.edit_feature = 0;
            return;
        }
        return;
    }

    if (guistate.active_area == MEMVIEW_STACK)
    {
        if (released & SCE_CTRL_RIGHT)
        {
            guistate.view_state = (guistate.view_state + 1) % 3;
            return;
        }
        if (released & SCE_CTRL_LEFT)
        {
            guistate.view_state = (guistate.view_state + 2) % 3;
            return;
        }
        return;
    }

    if (guistate.active_area != MEMVIEW_HEX)
        return;

    const MemLayoutInfo *layout = &layout_info[guistate.mem_layout];
    const int bytes = layout->bytes;
    const int values_per_row = 8 / bytes;
    bool needs_reread = false;

    if (released & SCE_CTRL_RTRIGGER)
    {
        guistate.mem_layout = (guistate.mem_layout + 1) % 3;
        if (guistate.cursor_column > values_per_row)
            guistate.cursor_column = values_per_row;
        needs_reread = true;
    }
    else if (released & SCE_CTRL_LTRIGGER)
    {
        guistate.mem_layout = (guistate.mem_layout + 2) % 3;
        if (guistate.cursor_column > values_per_row)
            guistate.cursor_column = values_per_row;
        needs_reread = true;
    }

    if (released & guistate.hotkeys.confirm)
    {
        if (guistate.edit_mode == EDIT_NONE)
        {
            if (guistate.cursor_column == 0)
            {
                guistate.edit_mode = EDIT_ADDRESS;
                guistate.modified_addr = guistate.addr;
                guistate.edit_offset = 1;
            }
            else
            {
                guistate.edit_mode = EDIT_VALUE;
                uint32_t offset = (guistate.cursor_column - 1) * bytes;
                uint32_t data_offset = guistate.addr - guistate.base_addr + offset;
                guistate.modified_addr = guistate.addr + offset;
                memcpy(guistate.modified_value, &guistate.cached_mem[data_offset], bytes);
                guistate.edit_offset = 0;
            }
        }
        else
        {
            if (guistate.edit_mode == EDIT_ADDRESS)
            {
                guistate.addr = CLAMP(guistate.modified_addr, lowest_vaddr, highest_vaddr);
                guistate.base_addr = guistate.addr & ~7;
                needs_reread = true;
            }
            else if (bytes)
            {
                kernel_write_memory(guistate.modified_addr, guistate.modified_value, bytes);
                needs_reread = true;
            }
            guistate.edit_mode = EDIT_NONE;
        }
    }
    else if ((released & guistate.hotkeys.cancel) && guistate.edit_mode != EDIT_NONE)
        guistate.edit_mode = EDIT_NONE;

    if (guistate.edit_mode == EDIT_NONE)
    {
        if (released & SCE_CTRL_RIGHT && guistate.cursor_column < values_per_row)
            guistate.cursor_column++;
        else if (released & SCE_CTRL_LEFT && guistate.cursor_column > 0)
            guistate.cursor_column--;

        if (released & SCE_CTRL_DOWN)
        {
            if (guistate.addr + 8 <= highest_vaddr)
            {
                guistate.addr += 8;

                const int visible_lines = (UI_HEIGHT - FONT_HEIGHT) / FONT_HEIGHT;
                uint32_t last_visible = guistate.base_addr + (visible_lines - 1) * 8;

                if (guistate.addr > last_visible)
                {
                    guistate.base_addr = (guistate.addr - (visible_lines / 2) * 8) & ~7;
                    guistate.base_addr = CLAMP(guistate.base_addr, lowest_vaddr, highest_vaddr - visible_lines * 8);
                    needs_reread = true;
                }
            }
        }
        else if (released & SCE_CTRL_UP)
        {
            if (guistate.addr > lowest_vaddr)
            {
                guistate.addr -= 8;

                if (guistate.addr < guistate.base_addr)
                {
                    guistate.base_addr = (guistate.addr - (UI_HEIGHT - FONT_HEIGHT) / (2 * FONT_HEIGHT) * 8) & ~7;
                    guistate.base_addr = CLAMP(guistate.base_addr, lowest_vaddr, highest_vaddr);
                    needs_reread = true;
                }
            }
        }
    }
    else
    {
        const int max_offset = (guistate.edit_mode == EDIT_ADDRESS) ? 7 : (bytes * 2 - 1);

        if (released & SCE_CTRL_LEFT)
        {
            guistate.edit_offset = (guistate.edit_offset + 1) % (max_offset + 1);
            if (guistate.edit_mode == EDIT_ADDRESS && guistate.edit_offset == 0)
                guistate.edit_offset = max_offset;
        }
        else if (released & SCE_CTRL_RIGHT)
        {
            guistate.edit_offset = (guistate.edit_offset + max_offset) % (max_offset + 1);
            if (guistate.edit_mode == EDIT_ADDRESS && guistate.edit_offset == 0)
                guistate.edit_offset = 1;
        }

        // Change nibble value
        if (released & (SCE_CTRL_UP | SCE_CTRL_DOWN))
        {
            const int change = (released & SCE_CTRL_UP) ? 1 : 15; // +15 is -1 with wrapping

            if (guistate.edit_mode == EDIT_ADDRESS)
            {
                const int shift = 4 * guistate.edit_offset;
                const uint32_t mask = 0xF << shift;
                uint32_t val = (guistate.modified_addr >> shift) & 0xF;
                val = (val + change) & 0xF;
                guistate.modified_addr = (guistate.modified_addr & ~mask) | (val << shift);
            }
            else
            {
                const int byte_idx = guistate.edit_offset / 2;
                const int shift = 4 * (guistate.edit_offset % 2);
                const uint8_t mask = 0xF << shift;
                uint8_t val = (guistate.modified_value[byte_idx] >> shift) & 0xF;
                val = (val + change) & 0xF;
                guistate.modified_value[byte_idx] = (guistate.modified_value[byte_idx] & ~mask) | (val << shift);
            }
        }
    }

    if (needs_reread)
    {
        cache_dirty = true;
        read_memview_cache();
    }
}

void draw_gui(void)
{
    renderer_setColor(0xFF171717);
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
        break;

    case UI_MEMVIEW: {
        renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
        const int visible_lines = (UI_HEIGHT - FONT_HEIGHT) / FONT_HEIGHT;

        if (guistate.active_area == MEMVIEW_HEX)
        {
            update_memview_state();
            int hex_width =
                (8 + 1 +
                 (8 / layout_info[guistate.mem_layout].bytes) * (layout_info[guistate.mem_layout].bytes * 2 + 1) + 1 +
                 8) *
                FONT_WIDTH;
            renderer_drawRectangle(0, 10, hex_width, visible_lines * FONT_HEIGHT, 0x40171717);
        }

        renderer_setColor(0xFFFFFFFF);
        read_memview_cache();

        for (int i = 0; i < visible_lines; i++)
        {
            const uint32_t line_addr = guistate.base_addr + i * 8;
            if (line_addr < guistate.base_addr + sizeof(guistate.cached_mem))
            {
                const uint8_t *data_ptr = guistate.cached_mem + (line_addr - guistate.base_addr);
                draw_hex_row(line_addr, data_ptr);
            }
        }

        int right_panel_x = 700;
        int right_panel_y = 60;
        if (guistate.has_active_bp)
        {
            int highlight_y = (guistate.active_area == MEMVIEW_REGS) ? right_panel_y : right_panel_y + FONT_HEIGHT;
            int highlight_h = 17 * FONT_HEIGHT;
            renderer_drawRectangle(
                right_panel_x - 5, highlight_y - 5, 210, highlight_h,
                (guistate.active_area == MEMVIEW_REGS || guistate.active_area == MEMVIEW_STACK) ? 0x40000000 : 0);

            renderer_setColor(0xFFFFFFFF);
            renderer_drawString(right_panel_x, right_panel_y, "Registers:");

            if (guistate.active_area == MEMVIEW_REGS)
                draw_registers(right_panel_x, right_panel_y + FONT_HEIGHT);
            else
            {
                renderer_drawStringF(right_panel_x, right_panel_y + FONT_HEIGHT, "R0:%08X SP:%08X", guistate.regs.r0,
                                     guistate.regs.sp);
                renderer_drawStringF(right_panel_x, right_panel_y + 2 * FONT_HEIGHT, "LR:%08X PC:%08X",
                                     guistate.regs.lr, guistate.regs.pc);
            }

            right_panel_y += (guistate.active_area == MEMVIEW_REGS ? 17 : 3) * FONT_HEIGHT + 10;
            const char *view_titles[] = {"Stack:", "Callstack:", "Breakpoints:"};
            renderer_drawString(right_panel_x, right_panel_y, view_titles[guistate.view_state]);

            if (guistate.active_area == MEMVIEW_STACK)
            {
                int y_pos = right_panel_y + FONT_HEIGHT;
                uint32_t max_items = 16;

                switch (guistate.view_state)
                {
                case VIEW_STACK:
                    for (uint32_t i = 0; i < guistate.stack_size && i < max_items; i++, y_pos += FONT_HEIGHT)
                        renderer_drawStringF(right_panel_x, y_pos, "%08X:%08X", guistate.regs.sp + i * 4,
                                             guistate.stack[i]);
                    break;

                case VIEW_CALLSTACK:
                    for (uint32_t i = 0; i < guistate.callstack_size && i < max_items; i++, y_pos += FONT_HEIGHT)
                        renderer_drawStringF(right_panel_x, y_pos, "[%d] %08X", i, guistate.callstack[i]);
                    break;

                case VIEW_BREAKPOINTS: {
                    const char *types[] = {"",
                                           "Software-Thumb",
                                           "Software-Arm",
                                           "Hardware",
                                           "Watchpoint-R",
                                           "Watchpoint-W",
                                           "Watchpoint-RW",
                                           "SingleStep"};

                    for (uint8_t i = 0; i < MAX_SLOT && (y_pos - right_panel_y) / FONT_HEIGHT < (int)max_items + 1;
                         i++, y_pos += FONT_HEIGHT)
                    {
                        ActiveBKPTSlot *bp = &guistate.breakpoints[i];
                        if (bp->type != SLOT_NONE)
                        {
                            const char *type_str =
                                (bp->type > SLOT_NONE && bp->type <= SINGLE_STEP_HW_BREAKPOINT) ? types[bp->type] : "?";
                            renderer_drawStringF(right_panel_x, y_pos, "[%d]%s@%08X", i, type_str, bp->address);
                        }
                    }
                    break;
                }
                }
            }
        }
        else
        {
            renderer_setColor(0xFFFFFFFF);
            renderer_drawString(right_panel_x, right_panel_y + FONT_HEIGHT, "Debugger Inactive");
            renderer_drawString(right_panel_x, right_panel_y + 3 * FONT_HEIGHT + 10, "(No Breakpoint Hit)");
        }
        break;
    }

    case UI_FEATURES:
    case UI_FEATURE_HW_BREAK:
    case UI_FEATURE_WATCH:
    case UI_FEATURE_SW_BREAK:
    case UI_FEATURE_CLEAR:
    case UI_FEATURE_HOTKEYS: {
        renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
        renderer_setColor(0xFFFFFFFF);
        const char *title = "Features:";
        const char **items = NULL;
        uint32_t num_items = 0;
        uint32_t current_value = guistate.edit_feature;
        bool is_editing_value = false;

        if (guistate.ui_state == UI_FEATURES)
        {
            static const char *features[] = {"Set Hardware Breakpoint",
                                             "Set Watchpoint",
                                             "Set Software Breakpoint",
                                             "Clear Breakpoint",
                                             "Suspend Process",
                                             "Resume Process",
                                             "Single Step",
                                             "Hotkeys"};
            items = features;
            num_items = sizeof(features) / sizeof(features[0]);
        }
        else if (guistate.ui_state == UI_FEATURE_HOTKEYS)
        {
            title = "Edit Hotkeys:";
            static const char *hotkey_names[] = {"Show GUI", "Area Select", "Edit Mode", "Cancel", "Confirm"};
            items = hotkey_names;
            num_items = sizeof(hotkey_names) / sizeof(hotkey_names[0]);
        }
        else
        {
            is_editing_value = true;
            switch (guistate.ui_state)
            {
            case UI_FEATURE_HW_BREAK:
                title = "Hardware Breakpoint Address:";
                break;
            case UI_FEATURE_WATCH:
                title = "Watchpoint Address:";
                break;
            case UI_FEATURE_SW_BREAK:
                title = "Software Breakpoint Address:";
                break;
            case UI_FEATURE_CLEAR:
                title = "Clear Breakpoint Index:";
                break;
            default:
                title = "Unknown Feature Edit:";
                break;
            }
        }

        renderer_drawString(50, 30, title);
        int y_pos = 60;

        if (is_editing_value)
        {
            renderer_drawStringF(50, y_pos, "Value: %08X", current_value);
            y_pos += 30;
            renderer_drawString(50, y_pos, "Use D-Pad Up/Down to Change");
            y_pos += 25;
            renderer_drawString(50, y_pos, "Press O to Confirm");
            y_pos += 25;
            renderer_drawString(50, y_pos, "Press X to Cancel");
        }
        else
        {
            uint32_t *hotkey_values = (uint32_t *)&guistate.hotkeys;
            for (uint32_t i = 0; i < num_items; i++, y_pos += 25)
            {
                renderer_setColor(i == guistate.edit_feature ? 0xFF0000FF : 0xFFFFFFFF);
                if (guistate.ui_state == UI_FEATURE_HOTKEYS)
                    renderer_drawStringF(50, y_pos, "%s: %04X", items[i], hotkey_values[i]);
                else
                    renderer_drawString(50, y_pos, items[i]);
            }

            renderer_setColor(0xFFFFFFFF);
            y_pos += 10;

            if (guistate.ui_state == UI_FEATURE_HOTKEYS)
            {
                renderer_drawString(50, y_pos, "Use Left/Right to change bits");
                y_pos += 25;
                renderer_drawString(50, y_pos, "Use Up/Down to select hotkey");
                y_pos += 25;
                renderer_drawString(50, y_pos, "Press O to Save");
                y_pos += 25;
                renderer_drawString(50, y_pos, "Press X to Cancel");
            }
            else
            {
                renderer_drawString(50, y_pos, "Use Up/Down to select");
                y_pos += 25;
                renderer_drawString(50, y_pos, "Press O to Activate/Edit");
                y_pos += 25;
                renderer_drawString(50, y_pos, "Press L/R Trigger to return to Hex View");
            }
        }
        break;
    }

    case UI_FEATURE_SUSPEND:
    case UI_FEATURE_RESUME:
    case UI_FEATURE_STEP:
        renderer_drawString(100, 80, "DONE...");
        break;

    default:
        renderer_setColor(0xFFFF0000);
        renderer_drawStringF(100, 80, "Unhandled UI State: %d", guistate.ui_state);
        renderer_setColor(0xFFFFFFFF);
        break;
    }
}

int pebble_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    ksceKernelDelayThread(6 * 1000 * 1000);
    register_handler();
    memset(&guistate, 0, sizeof(guistate));
    load_hotkeys();
    guistate.ui_state = UI_WELCOME;
    guistate.mem_layout = MEM_LAYOUT_8BIT;
    guistate.active_area = MEMVIEW_HEX;
    guistate.view_state = VIEW_STACK;
    SceCtrlData ctrl;
    uint32_t prev_buttons = 0;

    while (1)
    {
        if (!g_target_process.pid)
        {
            ksceKernelDelayThread(4 * 1000 * 1000);
            continue;
        }

        ksceCtrlPeekBufferPositive(0, &ctrl, 1);
        uint32_t current_buttons = ctrl.buttons;
        uint32_t released = (prev_buttons & ~current_buttons);

        if ((current_buttons == guistate.hotkeys.show_gui) && (prev_buttons != guistate.hotkeys.show_gui))
        {
            if (!guistate.gui_visible && evtflag)
            {
                if (g_target_process.main_thread_id && renderer_init())
                {
                    ksceKernelDebugSuspendThread(g_target_process.main_thread_id, 0x100);
                    guistate.gui_visible = true;
                }
                else
                {
                    kernel_debugger_on_create();
                    continue;
                }
            }
            else
            {
                guistate.gui_visible = false;
                ksceKernelDebugResumeThread(g_target_process.main_thread_id, 0x100);
            }
        }

        if (guistate.gui_visible)
        {
            ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
            if (guistate.ui_state == UI_WELCOME && (released & guistate.hotkeys.confirm))
            {
                SceKernelModuleInfo info = {.size = sizeof(SceKernelModuleInfo)};
                if (kernel_get_moduleinfo(&info) >= 0)
                {
                    guistate.modinfo = info;
                    guistate.ui_state = UI_MEMVIEW;
                    guistate.active_area = MEMVIEW_HEX;
                    guistate.cursor_column = 1;
                    // Find memory range
                    for (int i = 0; i < 4; ++i)
                    {
                        if (guistate.modinfo.segments[i].vaddr != NULL && guistate.modinfo.segments[i].memsz > 0)
                        {
                            uint32_t seg_start = (uint32_t)guistate.modinfo.segments[i].vaddr;
                            uint32_t seg_end = seg_start + guistate.modinfo.segments[i].memsz;
                            if (seg_start > 0 && seg_start < lowest_vaddr)
                                lowest_vaddr = seg_start;
                            if (seg_end > highest_vaddr)
                                highest_vaddr = seg_end;
                        }
                    }

                    guistate.base_addr = lowest_vaddr;
                    guistate.addr = lowest_vaddr;
                    cache_dirty = true;
                    read_memview_cache();
                }
                else
                    guistate.ui_state = UI_WELCOME;
            }
            else if (guistate.ui_state == UI_MEMVIEW)
            {
                if (guistate.modinfo.size != sizeof(SceKernelModuleInfo))
                {
                    kernel_get_moduleinfo(&guistate.modinfo);
                    // Re-find memory range
                    for (int i = 0; i < 4; ++i)
                    {
                        if (guistate.modinfo.segments[i].vaddr != NULL && guistate.modinfo.segments[i].memsz > 0)
                        {
                            uint32_t seg_start = (uint32_t)guistate.modinfo.segments[i].vaddr;
                            uint32_t seg_end = seg_start + guistate.modinfo.segments[i].memsz;
                            if (seg_start > 0 && seg_start < lowest_vaddr)
                            {
                                lowest_vaddr = seg_start;
                                if (seg_end > 0)
                                    highest_vaddr = seg_end;
                            }
                        }
                    }
                    guistate.base_addr = lowest_vaddr;
                    guistate.addr = lowest_vaddr;
                    cache_dirty = true;
                    read_memview_cache();
                }

                handle_memview_input(released, current_buttons);
            }
            else if (guistate.ui_state >= UI_FEATURES)
                handle_feature_input(released);

            if (ksceKernelLockMutex(pebble_mtx_uid, 1, NULL) == 0)
            {
                draw_gui();
                ksceKernelUnlockMutex(pebble_mtx_uid, 1);
                ksceKernelSetEventFlag(evtflag, buf_index + 1);
                buf_index ^= 1;
            }
        }
        prev_buttons = current_buttons;
        ksceKernelDelayThread(33333);
    }
    return 0;
}