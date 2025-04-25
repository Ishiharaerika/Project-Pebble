#include "kernel.h"
#include "renderer.h"

State guistate;
static SceDisplayFrameBuf original_fb = {0};
static uint32_t lowest_vaddr = 0x84000000;
static uint32_t highest_vaddr = 0x8FFFFFFF;

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
    if (guistate.has_active_bp)
    {
        kernel_get_registers(&guistate.regs);
        guistate.stack_size = kernel_read_memory((void *)guistate.regs.sp, guistate.stack, sizeof(guistate.stack));
        if (guistate.stack_size > 0)
            guistate.stack_size /= 4;
        else
            guistate.stack_size = 0;
        guistate.callstack_size = MAX_CALL_STACK_DEPTH;
        kernel_get_callstack(guistate.callstack, guistate.callstack_size);
    }
    else
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

static inline char nibble_to_hex(uint8_t nibble)
{
    nibble &= 0xF;
    return (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
}

static void draw_hex_row(uint32_t addr, const uint8_t *data)
{
    if (addr < guistate.base_addr || addr >= guistate.base_addr + sizeof(guistate.cached_mem))
        return;
    const int ypos = 10 + ((addr - guistate.base_addr) / 8) * FONT_HEIGHT;
    bool is_selected_row = (addr == guistate.addr);
    char addr_str[9];
    uint32_t temp_addr = addr;
    addr_str[8] = '\0';
    for (int i = 7; i >= 0; --i)
    {
        addr_str[i] = nibble_to_hex(temp_addr & 0xF);
        temp_addr >>= 4;
    }
    renderer_drawString(0, ypos, addr_str);
    char hex_str[28];
    char ascii_str[9];
    int hex_pos = 0;
    const MemLayoutInfo *layout = &layout_info[guistate.mem_layout];
    const int bytes_per_value = layout->bytes;
    const int hex_chars_per_value = bytes_per_value * 2;
    hex_str[hex_pos++] = ' ';
    if (data)
    {
        for (int i = 0; i < 8; ++i)
        {
            uint8_t byte_val = data[i];
            if (i % bytes_per_value == 0)
            {
                hex_str[hex_pos++] = ' ';
                uint32_t val = 0;
                int bytes_to_copy = (i + bytes_per_value <= 8) ? bytes_per_value : (8 - i);
                memcpy(&val, data + i, bytes_to_copy);
                for (int k = hex_chars_per_value - 1; k >= 0; --k)
                {
                    hex_str[hex_pos + k] = nibble_to_hex(val & 0xF);
                    val >>= 4;
                }
                hex_pos += hex_chars_per_value;
            }
            ascii_str[i] = (byte_val > 31 && byte_val < 127) ? byte_val : '.';
        }
        ascii_str[8] = '\0';
    }
    else
    {
        strcpy(hex_str + hex_pos, " Read Error");
        hex_pos += strlen(" Read Error");
        memset(ascii_str, '.', 8);
        ascii_str[8] = '\0';
    }
    hex_str[hex_pos] = '\0';
    renderer_drawString(8 * FONT_WIDTH, ypos, hex_str);
    renderer_drawString((8 * FONT_WIDTH) + hex_pos * FONT_WIDTH + FONT_WIDTH, ypos, ascii_str);
    if (is_selected_row && guistate.active_area == MEMVIEW_HEX)
    {
        int underline_x = 0;
        int underline_w = 0;
        int underline_y = ypos + FONT_HEIGHT - 2;
        int underline_h = 1;
        if (guistate.edit_mode == EDIT_NONE)
        {
            if (guistate.cursor_column == 0)
            {
                underline_x = 0;
                underline_w = 8 * FONT_WIDTH;
            }
            else
            {
                underline_x = (9 + ((addr % 8) / bytes_per_value) * (hex_chars_per_value + 1)) * FONT_WIDTH;
                underline_w = hex_chars_per_value * FONT_WIDTH;
            }
        }
        else
        {
            underline_w = FONT_WIDTH;
            if (guistate.edit_mode == EDIT_ADDRESS)
                underline_x = guistate.edit_offset * FONT_WIDTH;
            else
            {
                int base_hex_x = (9 + ((addr % 8) / bytes_per_value) * (hex_chars_per_value + 1)) * FONT_WIDTH;
                underline_x = base_hex_x + guistate.edit_offset * FONT_WIDTH;
            }
        }
        renderer_drawRectangle(underline_x, underline_y, underline_w, underline_h, 0xFFFFFFFF);
    }
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
        if (released & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))
        {
            guistate.ui_state = UI_MEMVIEW;
            return;
        }
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
    if (cancelled && guistate.ui_state >= UI_FEATURE_HW_BREAK)
        guistate.ui_state = UI_FEATURES;
}


static inline uint32_t hex_power(int exp)
{
    uint32_t res = 1;
    while (exp-- > 0)
        res *= 16;
    return res;
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
        const MemLayoutInfo *layout = &layout_info[guistate.mem_layout];
        uint32_t layout_bytes = layout->bytes;
        bool needs_reread = false;
        if (released & guistate.hotkeys.edit)
        {
            guistate.edit_mode = guistate.cursor_column == 0 ? EDIT_ADDRESS : EDIT_VALUE;
            guistate.modified_addr = guistate.addr;
            kernel_read_memory((void *)guistate.addr, guistate.modified_value, layout_bytes);
            guistate.edit_offset = (guistate.edit_mode == EDIT_ADDRESS) ? 7 : (layout_bytes * 2) - 1;
        }
        int layout_delta = (released & SCE_CTRL_R1 ? 1 : 0) - (released & SCE_CTRL_L1 ? 1 : 0);
        if (layout_delta != 0)
        {
            guistate.mem_layout = (guistate.mem_layout + layout_delta + 3) % 3;
            if (guistate.edit_mode == EDIT_VALUE)
            {
                layout_bytes = layout_info[guistate.mem_layout].bytes;
                kernel_read_memory((void *)guistate.addr, guistate.modified_value, layout_bytes);
                guistate.edit_offset = (layout_bytes * 2) - 1;
            }
            else
                guistate.addr &= ~(layout_info[guistate.mem_layout].bytes - 1);
        }
        if (guistate.edit_mode == EDIT_ADDRESS)
        {
            int max_offset = 7;
            int offset_delta = (released & SCE_CTRL_RIGHT ? 1 : 0) - (released & SCE_CTRL_LEFT ? 1 : 0);
            if (offset_delta)
                guistate.edit_offset = (guistate.edit_offset + offset_delta + max_offset + 1) % (max_offset + 1);
            int value_delta_nibble = (released & SCE_CTRL_UP ? 1 : 0) - (released & SCE_CTRL_DOWN ? -1 : 0);
            if (value_delta_nibble)
            {
                uint32_t power = hex_power(max_offset - guistate.edit_offset);
                uint32_t current_nibble = (guistate.modified_addr / power) % 16;
                uint32_t new_nibble = (current_nibble + value_delta_nibble + 16) % 16;
                uint32_t temp_addr = guistate.modified_addr + (new_nibble - current_nibble) * power;
                if (temp_addr >= lowest_vaddr && temp_addr <= highest_vaddr)
                    guistate.modified_addr = temp_addr;
            }
            if (released & guistate.hotkeys.confirm)
            {
                guistate.addr = guistate.modified_addr;
                guistate.base_addr = guistate.addr & ~7;
                needs_reread = true;
                guistate.edit_mode = EDIT_NONE;
                guistate.cursor_column = 0;
            }
        }
        else if (guistate.edit_mode == EDIT_VALUE)
        {
            int max_offset = (layout_bytes * 2) - 1;
            int offset_delta = (released & SCE_CTRL_RIGHT ? 1 : 0) - (released & SCE_CTRL_LEFT ? 1 : 0);
            if (offset_delta)
                guistate.edit_offset = (guistate.edit_offset + offset_delta + max_offset + 1) % (max_offset + 1);
            int value_delta_nibble = (released & SCE_CTRL_UP ? 1 : 0) - (released & SCE_CTRL_DOWN ? -1 : 0);
            if (value_delta_nibble)
            {
                int byte_idx = (max_offset - guistate.edit_offset) / 2;
                int nibble_pos = (max_offset - guistate.edit_offset) % 2;
                uint8_t current_byte = guistate.modified_value[byte_idx];
                uint8_t current_nibble = (nibble_pos == 0) ? (current_byte & 0x0F) : (current_byte >> 4);
                uint8_t new_nibble = (current_nibble + value_delta_nibble + 16) % 16;
                if (nibble_pos == 0)
                    current_byte = (current_byte & 0xF0) | new_nibble;
                else
                    current_byte = (current_byte & 0x0F) | (new_nibble << 4);
                guistate.modified_value[byte_idx] = current_byte;
            }
            if (released & guistate.hotkeys.confirm)
            {
                kernel_write_memory((void *)guistate.addr, guistate.modified_value, layout_bytes);
                needs_reread = true;
                guistate.edit_mode = EDIT_NONE;
                guistate.cursor_column = 1;
            }
        }
        else
        {
            int dx = (released & SCE_CTRL_RIGHT ? 1 : 0) - (released & SCE_CTRL_LEFT ? 1 : 0);
            int dy = (released & SCE_CTRL_DOWN ? 1 : 0) - (released & SCE_CTRL_UP ? 1 : 0);
            if (dx != 0)
                guistate.cursor_column = 1 - guistate.cursor_column;
            if (dy != 0)
            {
                int delta = dy * 8;
                uint32_t new_addr = guistate.addr + delta;
                if (new_addr < lowest_vaddr)
                    new_addr = lowest_vaddr;
                else if (new_addr > highest_vaddr)
                    new_addr = highest_vaddr;
                if (new_addr != guistate.addr)
                {
                    guistate.addr = new_addr;
                    const int visible_lines = (UI_HEIGHT - FONT_HEIGHT) / FONT_HEIGHT;
                    uint32_t first_visible_addr = guistate.base_addr;
                    uint32_t last_visible_addr = first_visible_addr + (visible_lines - 1) * 8;
                    if (guistate.addr < first_visible_addr)
                    {
                        guistate.base_addr = guistate.addr & ~7;
                        needs_reread = true;
                    }
                    else if (guistate.addr > last_visible_addr)
                    {
                        guistate.base_addr = (guistate.addr - (visible_lines - 1) * 8) & ~7;
                        needs_reread = true;
                    }
                    if (guistate.base_addr < lowest_vaddr)
                    {
                        guistate.base_addr = lowest_vaddr;
                        needs_reread = true;
                    }
                }
            }
        }
        if (released & guistate.hotkeys.cancel)
        {
            if (guistate.edit_mode != EDIT_NONE)
                guistate.edit_mode = EDIT_NONE;
        }
        if (released & SCE_CTRL_TRIANGLE)
            kernel_set_watchpoint(guistate.addr, BREAK_READ_WRITE);
        if (released & SCE_CTRL_SQUARE)
            kernel_set_software_breakpoint(guistate.addr, SW_BREAKPOINT_THUMB);
        if (needs_reread)
            read_memview_cache();
    }
    else if (guistate.active_area == MEMVIEW_STACK)
    {
        int view_delta = (released & SCE_CTRL_RIGHT ? 1 : 0) - (released & SCE_CTRL_LEFT ? 1 : 0);
        if (view_delta != 0)
            guistate.view_state = (guistate.view_state + view_delta + 3) % 3;
    }
}

void draw_gui(void)
{
    renderer_setColor(0xFFFFFFFF);
    update_memview_state();
    switch (guistate.ui_state)
    {
    case UI_WELCOME:
        renderer_drawString(100, 80, "Welcome to Pebble Vita Debugger!!!");
        if (g_target_process.pid)
        {
            renderer_drawStringF(100, 140, "Process ID: %#X", g_target_process.pid);
            renderer_drawStringF(100, 200, "Press O to continue to HEX view...");
        }
        break;
    case UI_MEMVIEW: {
        const int visible_lines = (UI_HEIGHT - FONT_HEIGHT) / FONT_HEIGHT;
        renderer_setColor(0x80FFFFFF);
        if (guistate.active_area == MEMVIEW_HEX)
        {
            int hex_width =
                (8 + 1 +
                 (8 / layout_info[guistate.mem_layout].bytes) * (layout_info[guistate.mem_layout].bytes * 2 + 1) + 1 +
                 8) *
                FONT_WIDTH;
            renderer_drawRectangle(0, 10, hex_width, visible_lines * FONT_HEIGHT, 0x40FFFFFF);
        }
        renderer_setColor(0xFFFFFFFF);
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
            renderer_setColor(0x80FFFFFF);
            int highlight_y = (guistate.active_area == MEMVIEW_REGS) ? right_panel_y : right_panel_y + FONT_HEIGHT;
            int highlight_h = 17 * FONT_HEIGHT;
            renderer_drawRectangle(
                right_panel_x - 5, highlight_y - 5, 210, highlight_h,
                (guistate.active_area == MEMVIEW_REGS || guistate.active_area == MEMVIEW_STACK) ? 0x80FFFFFF : 0);
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
                    const char *types[] = {"", "SW-T", "SW-A", "HW-B", "WP-R", "WP-W", "WP-RW", "Step"};
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
                        else
                        {
                            renderer_setColor(0xFF808080);
                            renderer_drawStringF(right_panel_x, y_pos, "[%d] ---", i);
                            renderer_setColor(0xFFFFFFFF);
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
            renderer_drawString(right_panel_x, right_panel_y + FONT_HEIGHT, "Debugger Inactive");
            renderer_drawString(right_panel_x, right_panel_y + 3 * FONT_HEIGHT + 10, "(No Breakpoint Hit)");
            renderer_setColor(0xFFFFFFFF);
        }
        break;
    }
    case UI_FEATURES:
    case UI_FEATURE_HW_BREAK:
    case UI_FEATURE_WATCH:
    case UI_FEATURE_SW_BREAK:
    case UI_FEATURE_CLEAR:
    case UI_FEATURE_HOTKEYS: {
        renderer_setColor(0xFFFFFFFF);
        const char *title = "Features:";
        const char **items = NULL;
        uint32_t num_items = 0;
        uint32_t current_value = guistate.edit_feature;
        bool is_editing_value = false;
        if (guistate.ui_state == UI_FEATURES)
        {
            static const char *features[] = {"HW Breakpoint",   "Watchpoint",     "SW Breakpoint", "Clear Breakpoint",
                                             "Suspend Process", "Resume Process", "Single Step",   "Hotkeys"};
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
                title = "Set HW Breakpoint Address:";
                break;
            case UI_FEATURE_WATCH:
                title = "Set Watchpoint Address:";
                break;
            case UI_FEATURE_SW_BREAK:
                title = "Set SW Breakpoint Address:";
                break;
            case UI_FEATURE_CLEAR:
                title = "Clear Breakpoint Slot Index:";
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
                renderer_setColor(i == guistate.edit_feature ? 0xFF00FF00 : 0xFFFFFFFF);
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
    renderer_init();
    ksceKernelDelayThread(8 * 1000 * 1000);
    register_handler();
    memset(&guistate, 0, sizeof(guistate));
    load_hotkeys();
    guistate.ui_state = UI_WELCOME;
    guistate.mem_layout = MEM_LAYOUT_8BIT;
    guistate.active_area = MEMVIEW_HEX;
    guistate.view_state = VIEW_STACK;
    SceCtrlData ctrl;
    uint32_t prev_buttons = 0;
    SceDisplayFrameBuf current_frame;
    current_frame.size = sizeof(SceDisplayFrameBuf);
    current_frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    current_frame.width = UI_WIDTH;
    current_frame.pitch = UI_WIDTH;
    current_frame.height = UI_HEIGHT;
    while (1)
    {
        if (!g_target_process.pid)
        {
            ksceKernelDelayThread(2 * 1000 * 1000);
            continue;
        }
        ksceCtrlPeekBufferPositive(0, &ctrl, 1);
        uint32_t current_buttons = ctrl.buttons;
        uint32_t released = (prev_buttons & ~current_buttons);
        uint32_t show_gui_key = guistate.hotkeys.show_gui;
        if ((current_buttons == show_gui_key) && (prev_buttons != show_gui_key))
        {
            if (!guistate.gui_visible)
            {
                //if (ksceDisplayGetFrameBuf(&original_fb, SCE_DISPLAY_SETBUF_IMMEDIATE) >= 0)
                //{
                    if(renderer_init() == 0)
                    {
                        if (g_target_process.main_thread_id)
                            ksceKernelDebugSuspendThread(g_target_process.main_thread_id, 0x100);
                        else
                        {
                            kernel_debugger_on_create();
                            continue;
                        }
                        guistate.gui_visible = true;
                    }
                //}
                //else
                //    ksceKernelPrintf("!!!Failed getting FB!!!\n");
            }
            else
            {
                guistate.gui_visible = false;
                renderer_destroy();
                //if (original_fb.size)
                //    ksceDisplaySetFrameBuf(&original_fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
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
                    for (int i = 0; i < 4; ++i)
                    {
                        if (guistate.modinfo.segments[i].vaddr != NULL && guistate.modinfo.segments[i].memsz > 0)
                        {
                            uint32_t seg_start = (uint32_t)guistate.modinfo.segments[i].vaddr;
                            uint32_t seg_end = seg_start + guistate.modinfo.segments[i].memsz;
                            if (seg_start > 0 && seg_start < lowest_vaddr)
                                lowest_vaddr = seg_start;
                            if (seg_end < highest_vaddr)
                                highest_vaddr = seg_end;
                        }
                    }
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
                    for (int i = 0; i < 4; ++i)
                    {
                        if (guistate.modinfo.segments[i].vaddr != NULL && guistate.modinfo.segments[i].memsz > 0)
                        {
                            uint32_t seg_start = (uint32_t)guistate.modinfo.segments[i].vaddr;
                            uint32_t seg_end = seg_start + guistate.modinfo.segments[i].memsz;
                            if (seg_start > 0 && seg_start < lowest_vaddr)
                                lowest_vaddr = seg_start;
                            if (seg_end < highest_vaddr)
                                highest_vaddr = seg_end;
                        }
                    }
                }
                handle_memview_input(released, current_buttons);
            }
            else
                handle_feature_input(released);
            if (ksceKernelLockMutex(pebble_mtx_uid, 1, NULL) >= 0)
            {
                draw_gui();
                current_frame.base = fb_bases[buf_index];
                ksceKernelPrintf("!!!Cuurent FB Address: %08X", current_frame.base);
                if (ksceDisplaySetFrameBuf(&current_frame, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0)
                    ksceKernelPrintf("!!!Failed using FB!!!/////");
                buf_index ^= 1;
                ksceKernelUnlockMutex(pebble_mtx_uid, 1);
            }
            prev_buttons = current_buttons;
        }
        ksceKernelDelayThread(33333);
    }
    return 0;
}