#include "kernel.h"
#include "renderer.h"

State guistate;
uint32_t lowest_vaddr = 0x84000000;
uint32_t highest_vaddr = 0x85000000;

static const MemLayoutInfo layout_info[] = {
    [MEM_LAYOUT_8BIT] = {"%02X", 1}, [MEM_LAYOUT_16BIT] = {"%04X", 2}, [MEM_LAYOUT_32BIT] = {"%08X", 4}};

static bool cache_dirty = true;
static const char *bp_types[] = {
    "", "Software-Thumb", "Software-Arm", "Hardware", "Watchpoint-R", "Watchpoint-W", "Watchpoint-RW", "SingleStep"};

static inline char nibble_to_hex(uint8_t nibble)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    return hex_chars[nibble & 0xF];
}

static void button_to_string(uint32_t buttons, char *buffer, size_t bufsize)
{
    static const struct {
        uint32_t flag;
        const char *name;} 
    btn_map[] = {{0x1000, "Triangle"}, {0x2000, "Circle"}, {0x4000, "Cross"}, {0x8000, "Square"},
                   {0x100, "L"},         {0x200, "R"},       {0x40, "Down"},    {0x80, "Left"},
                   {0x10, "Up"},         {0x20, "Right"},    {0x1, "Select"},   {0x8, "Start"}};

    if (buttons == 0)
    {
        strncpy(buffer, "None", bufsize);
        return;
    }

    buffer[0] = '\0';
    size_t remaining = bufsize - 1;
    char *ptr = buffer;
    bool first = true;

    for (size_t i = 0; i < sizeof(btn_map) / sizeof(btn_map[0]); i++)
    {
        if (!(buttons & btn_map[i].flag))
            continue;

        if (!first)
        {
            if (remaining <= 1)
                break;
            *ptr++ = '+';
            remaining--;
        }

        size_t namelen = strlen(btn_map[i].name);
        if (namelen >= remaining)
            break;

        memcpy(ptr, btn_map[i].name, namelen);
        ptr += namelen;
        remaining -= namelen;
        first = false;
    }

    *ptr = '\0';

    if (first)
        snprintf(buffer, bufsize, "0x%X", buttons);
}

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

static void check_button_repeat(uint32_t current_buttons, uint32_t *released)
{
    uint64_t current_time = ksceKernelGetSystemTimeWide();
    uint32_t dir_buttons = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
    uint32_t held_dirs = current_buttons & dir_buttons & guistate.pressed_buttons;

    if (held_dirs)
    {
        uint64_t elapsed = current_time - guistate.button_press_time;

        if (!guistate.repeating && elapsed >= 1000000ULL)
        {
            *released |= held_dirs;
            guistate.repeating = true;
            guistate.last_repeat_time = current_time;
        }
        else if (guistate.repeating && current_time - guistate.last_repeat_time >= 150000ULL)
        {
            *released |= held_dirs;
            guistate.last_repeat_time = current_time;
        }
    }
    else
        guistate.repeating = false;

    uint32_t new_presses = current_buttons & ~guistate.pressed_buttons;
    if (new_presses & dir_buttons)
        guistate.button_press_time = current_time;

    guistate.pressed_buttons = current_buttons;
}

static void update_memview_state(void)
{
    kernel_list_breakpoints(guistate.breakpoints);

    guistate.has_active_bp = false;
    for (int i = 0; i < MAX_SLOT; i++)
    {
        if (guistate.breakpoints[i].type && guistate.breakpoints[i].pid == g_target_process.pid)
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
    guistate.stack_size = kernel_read_memory((void *)guistate.regs.sp, guistate.stack, sizeof(guistate.stack)) / 4;

    guistate.callstack_size = MAX_CALL_STACK_DEPTH;
    kernel_get_callstack(guistate.callstack, guistate.callstack_size);
}

static void read_memview_cache(void)
{
    if (!cache_dirty)
        return;

    if (kernel_read_memory((void *)guistate.base_addr, guistate.cached_mem, sizeof(guistate.cached_mem)) < 0)
        memset(guistate.cached_mem, 0xFF, sizeof(guistate.cached_mem));

    cache_dirty = false;
}

static void draw_hex_row_highlight(int ypos, int hex_chars)
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
            underline_x =
                (9 + value_index * (hex_chars + 1)) * FONT_WIDTH + (hex_chars - 1 - guistate.edit_offset) * FONT_WIDTH;
        }
    }

    renderer_drawRectangle(2 + underline_x, underline_y, underline_w, 1, 0xFFFFFFFF);
}

static void draw_hex_row(uint32_t addr, const uint8_t *data)
{
    if (addr < guistate.base_addr || addr >= guistate.base_addr + sizeof(guistate.cached_mem))
        return;

    const int ypos = 10 + ((addr - guistate.base_addr) / 8) * FONT_HEIGHT;
    const bool is_selected_row = (addr == guistate.addr);

    // Draw address
    char addr_str[9];
    uint32_t temp_addr = (is_selected_row && guistate.edit_mode == EDIT_ADDRESS) ? guistate.modified_addr : addr;

    for (int i = 7; i >= 0; i--)
    {
        addr_str[i] = nibble_to_hex(temp_addr & 0xF);
        temp_addr >>= 4;
    }
    addr_str[8] = '\0';
    renderer_drawString(1, ypos, addr_str);

    if (!data)
    {
        renderer_drawString(2 + 8 * FONT_WIDTH, ypos, " Read Error");
        renderer_drawString(2 + (10 + 10) * FONT_WIDTH, ypos, "........");
        return;
    }

    const MemLayoutInfo *layout = &layout_info[guistate.mem_layout];
    const int bytes_per_value = layout->bytes;
    const int values_per_row = 8 / bytes_per_value;
    const int hex_chars = bytes_per_value * 2;

    // Prepare hex and ASCII
    char hex_str[28];
    hex_str[0] = ' ';
    int hex_pos = 1;

    char ascii_str[9];

    for (int i = 0; i < values_per_row; i++)
    {
        const bool is_edited = is_selected_row && guistate.edit_mode == EDIT_VALUE && guistate.cursor_column - 1 == i;
        uint32_t value_addr = addr + i * bytes_per_value;

        // Highlight breakpoints
        if (kernel_get_breakpoint_index(value_addr) >= 0)
            renderer_drawRectangle(2 + (9 + i * (hex_chars + 1)) * FONT_WIDTH, ypos, hex_chars * FONT_WIDTH,
                                   FONT_HEIGHT, 0xFF0000FF);

        // Space between values
        if (i > 0)
            hex_str[hex_pos++] = ' ';

        // Get value from either modified data or cache
        const uint8_t *src = is_edited ? guistate.modified_value : data + i * bytes_per_value;

        // Convert value to hex
        for (int j = 0; j < bytes_per_value; j++)
        {
            hex_str[hex_pos++] = nibble_to_hex(src[j] >> 4);
            hex_str[hex_pos++] = nibble_to_hex(src[j] & 0xF);
        }

        // Convert value to ASCII
        for (int b = 0; b < bytes_per_value; b++)
            ascii_str[i * bytes_per_value + b] = (src[b] >= 32 && src[b] < 127) ? src[b] : '.';
    }

    hex_str[hex_pos] = '\0';
    ascii_str[8] = '\0';

    // Draw hex and ASCII parts
    renderer_drawString(2 + 8 * FONT_WIDTH, ypos, hex_str);
    renderer_drawString(2 + (9 + hex_pos) * FONT_WIDTH, ypos, ascii_str);

    // Draw cursor for current row
    if (is_selected_row && guistate.active_area == MEMVIEW_HEX)
        draw_hex_row_highlight(ypos, hex_chars);
}

static void draw_registers(int x, int y)
{
    static const char *reg_names[] = {"R0", "R1", "R2",  "R3",  "R4",  "R5", "R6", "R7",
                                      "R8", "R9", "R10", "R11", "R12", "SP", "LR", "PC"};

    uint32_t *reg_ptr = (uint32_t *)&guistate.regs;
    for (int i = 0; i < 16; i++, y += FONT_HEIGHT)
        renderer_drawStringF(x, y, (i >= 10 && i <= 12) ? "%s:%08X" : "%-3s: %08X", reg_names[i], reg_ptr[i]);
}

static void handle_area_selection(uint32_t released)
{
    if (released & SCE_CTRL_LEFT)
    {
        guistate.active_area = MEMVIEW_HEX;
        return;
    }
    if (released & SCE_CTRL_RIGHT)
    {
        guistate.active_area = MEMVIEW_REGS;
        return;
    }
    if (released & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))
    {
        guistate.ui_state = UI_FEATURES;
        guistate.edit_feature = 0;
    }
}

static void handle_memview_stack(uint32_t released)
{
    if (released & SCE_CTRL_UP)
    {
        guistate.active_area = MEMVIEW_REGS;
        return;
    }

    if (released & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER))
        guistate.view_state = (guistate.view_state + 1) % 3;
    else if (released & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER))
        guistate.view_state = (guistate.view_state + 2) % 3;
}

static void handle_breakpoint_toggle(uint32_t released, uint32_t address)
{
    int bp_index = kernel_get_breakpoint_index(address);
    if (bp_index >= 0)
    {
        kernel_clear_breakpoint(bp_index);
        return;
    }

    uint32_t mem_type;
    if (kernel_get_memblockinfo((void *)address, &mem_type) < 0)
        return;

    if (released & SCE_CTRL_TRIANGLE)
    {
        if (mem_type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RX)
            kernel_set_hardware_breakpoint(address);
        else
            kernel_set_watchpoint(address, BREAK_READ_WRITE);
    }
    else if ((released & SCE_CTRL_SQUARE) && (mem_type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RX))
        kernel_set_software_breakpoint(address & ~1, SW_BREAKPOINT_THUMB);
}

static void adjust_edit_offset(bool left_pressed, bool is_address, int bytes)
{
    const int max_offset = is_address ? 7 : (bytes * 2 - 1);

    if (left_pressed)
    {
        guistate.edit_offset = (guistate.edit_offset + 1) % (max_offset + 1);
        if (is_address && guistate.edit_offset == 0)
            guistate.edit_offset = max_offset;
    }
    else
    {
        guistate.edit_offset = (guistate.edit_offset + max_offset) % (max_offset + 1);
        if (is_address && guistate.edit_offset == 0)
            guistate.edit_offset = 1;
    }
}

static void change_nibble_value(bool increase, int byte_idx, int shift, uint8_t *target)
{
    const int change = increase ? 1 : 15; // +15 is -1 with wrapping
    const uint8_t mask = 0xF << shift;
    uint8_t val = (target[byte_idx] >> shift) & 0xF;
    val = (val + change) & 0xF;
    target[byte_idx] = (target[byte_idx] & ~mask) | (val << shift);
}

static bool handle_memview_navigation(uint32_t released, int values_per_row)
{
    bool needs_reread = false;

    // Horizontal movement
    if (released & SCE_CTRL_RIGHT && guistate.cursor_column < values_per_row)
        guistate.cursor_column++;
    else if (released & SCE_CTRL_LEFT && guistate.cursor_column > 0)
        guistate.cursor_column--;

    // Vertical movement
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

    return needs_reread;
}

static void handle_memview_value_edit(bool increase)
{
    if (guistate.edit_mode == EDIT_ADDRESS)
    {
        int shift = 4 * guistate.edit_offset;
        uint32_t mask = 0xF << shift;
        uint32_t val = (guistate.modified_addr >> shift) & 0xF;
        val = (val + (increase ? 1 : 15)) & 0xF;
        guistate.modified_addr = (guistate.modified_addr & ~mask) | (val << shift);
    }
    else
    {
        int byte_idx = guistate.edit_offset / 2;
        int shift = 4 * (guistate.edit_offset % 2);
        change_nibble_value(increase, byte_idx, shift, guistate.modified_value);
    }
}

static bool handle_memview_confirm(int bytes)
{
    if (guistate.edit_mode == EDIT_NONE)
    {
        // Start editing
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
        return false;
    }

    // Finish editing
    if (guistate.edit_mode == EDIT_ADDRESS)
    {
        guistate.addr = CLAMP(guistate.modified_addr, lowest_vaddr, highest_vaddr);
        guistate.base_addr = guistate.addr & ~7;
        guistate.edit_mode = EDIT_NONE;
        cache_dirty = true;
        return true;
    }
    else if (bytes)
    {
        kernel_write_memory(guistate.modified_addr, guistate.modified_value, bytes);
        guistate.edit_mode = EDIT_NONE;
        cache_dirty = true;
        return true;
    }

    guistate.edit_mode = EDIT_NONE;
    return false;
}

static void handle_memview_input(uint32_t released, uint32_t current)
{
    // Area selection
    if (current & guistate.hotkeys.area_select)
    {
        handle_area_selection(released);
        return;
    }

    // Single step
    if (released == (SCE_CTRL_START | SCE_CTRL_RTRIGGER))
    {
        kernel_single_step();
        return;
    }

    // Handle other areas first
    if (guistate.active_area == MEMVIEW_REGS)
    {
        if (released & SCE_CTRL_DOWN)
            guistate.active_area = MEMVIEW_STACK;
        return;
    }

    if (guistate.active_area == MEMVIEW_STACK)
    {
        handle_memview_stack(released);
        return;
    }

    // Handle hex view
    const MemLayoutInfo *layout = &layout_info[guistate.mem_layout];
    const int bytes = layout->bytes;
    const int values_per_row = 8 / bytes;
    bool needs_reread = false;

    // Handle breakpoint toggling
    if ((released & (SCE_CTRL_TRIANGLE | SCE_CTRL_SQUARE)) && guistate.edit_mode == EDIT_NONE &&
        guistate.cursor_column > 0)
    {

        uint32_t address = guistate.addr + (guistate.cursor_column - 1) * bytes;
        handle_breakpoint_toggle(released, address);
        needs_reread = true;
    }

    // Handle layout change
    if (released & SCE_CTRL_LTRIGGER)
    {
        guistate.mem_layout = (guistate.mem_layout + 1) % 3;
        if (guistate.cursor_column > values_per_row)
            guistate.cursor_column = values_per_row;
        needs_reread = true;
    }
    else if (released & SCE_CTRL_RTRIGGER)
    {
        guistate.mem_layout = (guistate.mem_layout + 2) % 3;
        if (guistate.cursor_column > values_per_row)
            guistate.cursor_column = values_per_row;
        needs_reread = true;
    }

    // Handle confirm/cancel in edit mode
    if (released & guistate.hotkeys.confirm)
        needs_reread = handle_memview_confirm(bytes);
    else if ((released & guistate.hotkeys.cancel) && guistate.edit_mode != EDIT_NONE)
        guistate.edit_mode = EDIT_NONE;

    // Handle navigation in non-edit mode
    if (guistate.edit_mode == EDIT_NONE)
        needs_reread = handle_memview_navigation(released, values_per_row);
    else
    {
        // Handle edit mode input
        if (released & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))
            adjust_edit_offset(released & SCE_CTRL_LEFT, guistate.edit_mode == EDIT_ADDRESS, bytes);

        if (released & (SCE_CTRL_UP | SCE_CTRL_DOWN))
            handle_memview_value_edit(released & SCE_CTRL_UP);
    }

    if (needs_reread)
    {
        cache_dirty = true;
        read_memview_cache();
    }
}

static void draw_memview_contents(void)
{
    const int visible_lines = (UI_HEIGHT - FONT_HEIGHT) / FONT_HEIGHT;
    const MemLayoutInfo *layout = &layout_info[guistate.mem_layout];
    const int bytes_per_value = layout->bytes;
    const int values_per_row = 8 / bytes_per_value;
    const int hex_chars = bytes_per_value * 2;
    int hex_width = (8 + 1 + values_per_row * (hex_chars + 1) + 1 + 8) * FONT_WIDTH;

    // Draw frame
    if (guistate.active_area == MEMVIEW_HEX)
    {
        renderer_drawRectangle(0, 10, hex_width, visible_lines * FONT_HEIGHT, 0x80171717);
        draw_frame(0, 10, hex_width, visible_lines * FONT_HEIGHT, 0xFFFF64AA);
    }

    renderer_setColor(0xFFFFFFFF);
    read_memview_cache();

    // Draw memory rows
    for (int i = 0; i < visible_lines; i++)
    {
        const uint32_t line_addr = guistate.base_addr + i * 8;
        if (line_addr < guistate.base_addr + sizeof(guistate.cached_mem))
        {
            const uint8_t *data_ptr = guistate.cached_mem + (line_addr - guistate.base_addr);
            draw_hex_row(line_addr, data_ptr);
        }
    }
}

static void draw_stack_panel(int x, int y, int width, int height)
{
    renderer_drawRectangle(x - 3, y - 3, width, height, 0x80000000);
    if (guistate.active_area == MEMVIEW_STACK)
        draw_frame(x - 3, y - 3, width, height, 0xFFFF64AA);

    static const char *view_titles[] = {"Stack:", "Callstack:", "Breakpoints:"};
    renderer_setColor(0xFFFFFFFF);
    renderer_drawString(x, y, view_titles[guistate.view_state]);

    y += FONT_HEIGHT;
    const uint32_t max_items = (guistate.active_area == MEMVIEW_STACK) ? 16 : 3;

    switch (guistate.view_state)
    {
    case VIEW_STACK:
        for (uint32_t i = 0; i < guistate.stack_size && i < max_items; i++, y += FONT_HEIGHT)
            renderer_drawStringF(x, y, "%08X:%08X", guistate.regs.sp + i * 4, guistate.stack[i]);
        break;

    case VIEW_CALLSTACK:
        for (uint32_t i = 0; i < guistate.callstack_size && i < max_items; i++, y += FONT_HEIGHT)
            renderer_drawStringF(x, y, "[%d] %08X", i, guistate.callstack[i]);
        break;

    case VIEW_BREAKPOINTS:
        for (uint8_t i = 0, count = 0; i < MAX_SLOT && count < max_items; i++)
        {
            ActiveBKPTSlot *bp = &guistate.breakpoints[i];
            if (bp->type != SLOT_NONE && bp->pid == g_target_process.pid)
            {
                if (guistate.active_area == MEMVIEW_STACK)
                {
                    const char *type_str = (bp->type <= SINGLE_STEP_HW_BREAKPOINT) ? bp_types[bp->type] : "?";
                    renderer_drawStringF(x, y, "[%d]%s@%08X", i, type_str, bp->address);
                }
                else
                    renderer_drawStringF(x, y, "[%d]@%08X", i, bp->address);

                y += FONT_HEIGHT;
                count++;
            }
        }
        break;
    }
}

static void draw_right_panel(void)
{
    int right_panel_x = 550;
    int right_panel_y = 10;

    if (!guistate.has_active_bp)
    {
        renderer_setColor(0xFFFFFFFF);
        renderer_drawString(right_panel_x, right_panel_y, "Debugger Inactive");
        renderer_drawString(right_panel_x, right_panel_y + 20, "(No Breakpoint Created...)");
        return;
    }

    // Draw registers panel
    int highlight_y = (guistate.active_area == MEMVIEW_REGS) ? right_panel_y : right_panel_y + FONT_HEIGHT;
    int highlight_h = 17 * FONT_HEIGHT;

    renderer_drawRectangle(right_panel_x - 3, highlight_y - 3, 400, highlight_h,
                           (guistate.active_area == MEMVIEW_REGS || guistate.active_area == MEMVIEW_STACK) ? 0x40171717
                                                                                                           : 0);

    if (guistate.active_area == MEMVIEW_REGS)
        draw_frame(right_panel_x - 3, highlight_y - 3, 400, highlight_h, 0xFFFF64AA);

    renderer_setColor(0xFFFFFFFF);
    renderer_drawString(right_panel_x, right_panel_y, "Registers:");

    // Draw registers - either full view or just a summary
    if (guistate.active_area == MEMVIEW_REGS)
        draw_registers(right_panel_x, right_panel_y + FONT_HEIGHT);
    else
    {
        renderer_drawStringF(right_panel_x, right_panel_y + FONT_HEIGHT, "R0:%08X R1:%08X", guistate.regs.r0,
                             guistate.regs.r1);
        renderer_drawStringF(right_panel_x, right_panel_y + 2 * FONT_HEIGHT, "R2:%08X R3:%08X", guistate.regs.r2,
                             guistate.regs.r3);
        renderer_drawStringF(right_panel_x, right_panel_y + 3 * FONT_HEIGHT, "R4:%08X R5:%08X", guistate.regs.r4,
                             guistate.regs.r5);
        renderer_drawStringF(right_panel_x, right_panel_y + 4 * FONT_HEIGHT, "R6:%08X R7:%08X", guistate.regs.r6,
                             guistate.regs.r7);
        renderer_drawStringF(right_panel_x, right_panel_y + 5 * FONT_HEIGHT, "R8:%08X SP:%08X", guistate.regs.r8,
                             guistate.regs.sp);
        renderer_drawStringF(right_panel_x, right_panel_y + 6 * FONT_HEIGHT, "LR:%08X PC:%08X", guistate.regs.lr,
                             guistate.regs.pc);
    }

    // Draw stack/callstack/breakpoints panel
    right_panel_y += (guistate.active_area == MEMVIEW_REGS ? 17 : 7) * FONT_HEIGHT + 10;
    draw_stack_panel(right_panel_x, right_panel_y, 400, 17 * FONT_HEIGHT);
}

static void find_next_breakpoint(bool up)
{
    uint8_t original = guistate.edit_feature;
    uint8_t next = original;

    do
    {
        next = up ? (next - 1 + MAX_SLOT) % MAX_SLOT : (next + 1) % MAX_SLOT;
        if (next == original)
            break;
    } while (guistate.breakpoints[next].type == SLOT_NONE);

    guistate.edit_feature = next;
}

static void handle_breakpoint_list_input(uint32_t released)
{
    if (released & SCE_CTRL_UP)
        find_next_breakpoint(true);
    else if (released & SCE_CTRL_DOWN)
        find_next_breakpoint(false);

    if (released & guistate.hotkeys.confirm)
    {
        if (guistate.breakpoints[guistate.edit_feature].type != SLOT_NONE)
        {
            kernel_clear_breakpoint(guistate.edit_feature);
            kernel_list_breakpoints(guistate.breakpoints);
            find_next_breakpoint(false);
        }
    }
}

static void start_breakpoint_list(void)
{
    guistate.ui_state = UI_FEATURE_LIST_ALL_BKPT;
    guistate.stored_edit_feature = guistate.edit_feature;
    kernel_list_breakpoints(guistate.breakpoints);

    // Find first active breakpoint
    guistate.edit_feature = 0;
    for (int i = 0; i < MAX_SLOT; i++)
    {
        if (guistate.breakpoints[i].type != SLOT_NONE)
        {
            guistate.edit_feature = i;
            break;
        }
    }
}

static void handle_breakpoint_edit_input(uint32_t released)
{
    if (released & guistate.hotkeys.confirm)
    {
        guistate.edit_feature = guistate.modified_addr;

        if (guistate.ui_state == UI_FEATURE_HW_BREAK)
            kernel_set_hardware_breakpoint(guistate.edit_feature);
        else if (guistate.ui_state == UI_FEATURE_WATCH)
            kernel_set_watchpoint(guistate.edit_feature, BREAK_READ_WRITE);
        else if (guistate.ui_state == UI_FEATURE_SW_BREAK)
            kernel_set_software_breakpoint(guistate.edit_feature, SW_BREAKPOINT_THUMB);

        guistate.bkpt_edit_offset = 0;
        return;
    }

    // Edit address
    if (released & SCE_CTRL_LEFT)
        guistate.bkpt_edit_offset = (guistate.bkpt_edit_offset + 1) % 8;
    else if (released & SCE_CTRL_RIGHT)
        guistate.bkpt_edit_offset = (guistate.bkpt_edit_offset + 7) % 8;

    if (released & (SCE_CTRL_UP | SCE_CTRL_DOWN))
    {
        const int change = (released & SCE_CTRL_UP) ? 1 : 15;
        const int shift = 4 * guistate.bkpt_edit_offset;
        const uint32_t mask = 0xF << shift;
        uint32_t val = (guistate.modified_addr >> shift) & 0xF;
        val = (val + change) & 0xF;
        guistate.modified_addr = (guistate.modified_addr & ~mask) | (val << shift);
    }
}

static void handle_hotkey_config_input(uint32_t released)
{
    int cur_idx = guistate.edit_feature;

    if (released & SCE_CTRL_UP)
        cur_idx = (cur_idx - 1 + 5) % 5;
    if (released & SCE_CTRL_DOWN)
        cur_idx = (cur_idx + 1) % 5;

    guistate.edit_feature = cur_idx;

    if (cur_idx < 4)
    {
        uint32_t *key_ptr = &((uint32_t *)&guistate.hotkeys)[cur_idx];
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
    }

    if (released & guistate.hotkeys.confirm)
    {
        save_hotkeys();
        guistate.ui_state = UI_FEATURES;
    }
}

static void handle_features_menu_input(uint32_t released)
{
    // Exit to memory view
    if (released & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))
    {
        guistate.ui_state = UI_MEMVIEW;
        return;
    }

    // Feature selection navigation
    if (released & SCE_CTRL_UP)
        guistate.edit_feature = (guistate.edit_feature - 1 + 8) % 8;
    if (released & SCE_CTRL_DOWN)
        guistate.edit_feature = (guistate.edit_feature + 1) % 8;

    // Feature activation
    if (!(released & guistate.hotkeys.confirm))
        return;

    switch (guistate.edit_feature)
    {
    case 0: // Hardware Breakpoint
    case 1: // Watchpoint
    case 2: // Software Breakpoint
        guistate.edit_mode = EDIT_ADDRESS;
        guistate.bkpt_edit_offset = 0;
        guistate.ui_state = UI_FEATURE_HW_BREAK + guistate.edit_feature;
        guistate.stored_edit_feature = guistate.edit_feature;
        guistate.modified_addr = guistate.edit_feature = guistate.addr;
        break;
    case 3: // List All Breakpoints
        start_breakpoint_list();
        break;
    case 4: // Suspend process
        kernel_suspend_process();
        break;
    case 5: // Resume process
        kernel_resume_process();
        break;
    case 6: // Single step
        kernel_single_step();
        break;
    case 7: // Hotkeys
        guistate.ui_state = UI_FEATURE_HOTKEYS;
        guistate.stored_edit_feature = guistate.edit_feature;
        guistate.edit_feature = 0;
        break;
    }
}

static void handle_feature_input(uint32_t released)
{
    // Common cancel handling for all features
    if ((released & guistate.hotkeys.cancel) && guistate.ui_state >= UI_FEATURE_HW_BREAK)
    {
        guistate.edit_mode = EDIT_NONE;
        guistate.ui_state = UI_FEATURES;
        guistate.edit_feature = guistate.stored_edit_feature;
        return;
    }

    // Handle input based on UI state
    switch (guistate.ui_state)
    {
    case UI_FEATURES:
        handle_features_menu_input(released);
        break;
    case UI_FEATURE_HW_BREAK:
    case UI_FEATURE_WATCH:
    case UI_FEATURE_SW_BREAK:
        handle_breakpoint_edit_input(released);
        break;
    case UI_FEATURE_LIST_ALL_BKPT:
        handle_breakpoint_list_input(released);
        break;
    case UI_FEATURE_HOTKEYS:
        handle_hotkey_config_input(released);
        break;
    default:
        break;
    }
}

static void draw_welcome(void)
{
    renderer_setColor(0xFFFFFFFF);
    renderer_drawString(100, 80, "Welcome to Pebble Vita Debugger!!!");

    if (!ksceKernelCheckDipsw(0xE4))
        renderer_drawString(100, 140, "DIP switch is not set. Plaese check README on Github.");

    if (g_target_process.pid)
    {
        renderer_drawStringF(100, 140, "Process ID: %#X", g_target_process.pid);

        char confirm_btn[64];
        button_to_string(guistate.hotkeys.confirm, confirm_btn, sizeof(confirm_btn));
        renderer_drawStringF(100, 200, "Press %s to continue to HEX view...", confirm_btn);
    }
    else
        renderer_drawString(100, 140, "Hmm... You don't seem to have process opened.");
}

static void draw_feature(void)
{
    renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
    renderer_setColor(0xFFFFFFFF);
    renderer_drawString(50, 30, "Features:");

    static const char *features[] = {"Set Hardware Breakpoint",
                                     "Set Watchpoint",
                                     "Set Software Breakpoint",
                                     "List All Breakpoints",
                                     "Suspend Process",
                                     "Resume Process",
                                     "Single Step",
                                     "Hotkeys"};

    int y = 60;
    for (uint32_t i = 0; i < 8; i++, y += 25)
    {
        renderer_setColor(i == guistate.edit_feature ? 0xFF0000FF : 0xFFFFFFFF);
        renderer_drawString(50, y, features[i]);
    }

    renderer_setColor(0xFFFFFFFF);
    y += 10;
    renderer_drawString(50, y, "Use Up/Down to select");
    y += 25;

    char confirm_btn[64];
    button_to_string(guistate.hotkeys.confirm, confirm_btn, sizeof(confirm_btn));
    renderer_drawStringF(50, y, "Press %s to Activate/Edit", confirm_btn);

    y += 25;
    renderer_drawString(50, y, "Press L/R Trigger to return to Hex View");
}

static void draw_feature_breakpoint(void)
{
    renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
    renderer_setColor(0xFFFFFFFF);

    const char *title = NULL;
    if (guistate.ui_state == UI_FEATURE_HW_BREAK)
        title = "Hardware Breakpoint Address:";
    else if (guistate.ui_state == UI_FEATURE_WATCH)
        title = "Watchpoint Address:";
    else
        title = "Software Breakpoint Address:";

    renderer_drawString(50, 30, title);
    int y = 60;

    renderer_drawStringF(50, y, "Value: %08X", guistate.modified_addr);
    int cursor_x = 50 + 7 * FONT_WIDTH + (7 - guistate.bkpt_edit_offset) * FONT_WIDTH;
    renderer_drawRectangle(cursor_x, y + FONT_HEIGHT - 2, FONT_WIDTH, 1, 0xFFFFFFFF);
    y += 30;
    renderer_drawString(50, y, "Use Left/Right to move cursor");
    y += 25;
    renderer_drawString(50, y, "Use Up/Down to change value");
    y += 25;

    char confirm_btn[64], cancel_btn[64];
    button_to_string(guistate.hotkeys.confirm, confirm_btn, sizeof(confirm_btn));
    button_to_string(guistate.hotkeys.cancel, cancel_btn, sizeof(cancel_btn));
    renderer_drawStringF(50, y, "Press %s to Confirm", confirm_btn);
    y += 25;
    renderer_drawStringF(50, y, "Press %s to Cancel", cancel_btn);
}

static void draw_feature_breakpoint_list(void)
{
    renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
    renderer_setColor(0xFFFFFFFF);
    renderer_drawString(50, 30, "All Breakpoints:");

    kernel_list_breakpoints(guistate.breakpoints);

    int y = 60;
    int count = 0;
    for (uint32_t i = 0; i < MAX_SLOT; i++)
    {
        if (guistate.breakpoints[i].type != SLOT_NONE)
        {
            const char *type_str = (guistate.breakpoints[i].type <= SINGLE_STEP_HW_BREAKPOINT)
                                       ? bp_types[guistate.breakpoints[i].type]
                                       : "?";
            renderer_setColor(i == guistate.edit_feature ? 0xFF0000FF : 0xFFFFFFFF);
            renderer_drawStringF(50, y, "[%d] PID:%08X %s@%08X", i, guistate.breakpoints[i].pid, type_str,
                                 guistate.breakpoints[i].address);
            y += 20;
            count++;
            if (y > UI_HEIGHT - 40)
            {
                y += 5;
                renderer_drawString(50, y, "... more breakpoints exist");
                break;
            }
        }
    }
    if (count == 0)
        renderer_drawString(50, y, "No breakpoint found");

    char confirm_btn[64], cancel_btn[64];
    button_to_string(guistate.hotkeys.confirm, confirm_btn, sizeof(confirm_btn));
    button_to_string(guistate.hotkeys.cancel, cancel_btn, sizeof(cancel_btn));
    renderer_setColor(0xFFFFFFFF);
    renderer_drawStringF(50, UI_HEIGHT - 40, "Press %s to delete, %s to return", confirm_btn, cancel_btn);
}

static void draw_hotkey_config(void)
{
    renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
    renderer_setColor(0xFFFFFFFF);
    renderer_drawString(50, 30, "Hotkey Configuration:");

    static const char *hotkey_names[] = {"Show GUI", "Area Select", "Cancel", "Confirm"};
    char button_str[64];

    int y = 60;
    for (uint32_t i = 0; i < 4; i++, y += 25)
    {
        uint32_t key_value = (i < 4) ? ((uint32_t *)&guistate.hotkeys)[i] : 0;
        button_to_string(key_value, button_str, sizeof(button_str));

        renderer_setColor(i == guistate.edit_feature ? 0xFF0000FF : 0xFFFFFFFF);
        renderer_drawStringF(50, y, "%-12s: %s", hotkey_names[i], button_str);
    }

    y += 10;
    renderer_setColor(0xFFFFFFFF);
    renderer_drawString(50, y, "Use Up/Down to select");
    y += 25;
    renderer_drawString(50, y, "Use Left/Right to change value");
    y += 25;

    char confirm_btn[64], cancel_btn[64];
    button_to_string(guistate.hotkeys.confirm, confirm_btn, sizeof(confirm_btn));
    button_to_string(guistate.hotkeys.cancel, cancel_btn, sizeof(cancel_btn));
    renderer_drawStringF(50, y, "Press %s to Save and Exit", confirm_btn);
    y += 25;
    renderer_drawStringF(50, y, "Press %s to Cancel", cancel_btn);
}

static void draw_unknown_state(void)
{
    renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
    renderer_setColor(0xFF0000FF);
    renderer_drawStringF(100, 80, "Unhandled UI State...");
    renderer_setColor(0xFFFFFFFF);
}

static void draw_memory_view(void)
{
    renderer_clearRectangle(0, 0, UI_WIDTH, UI_HEIGHT);
    update_memview_state();
    draw_memview_contents();
    draw_right_panel();
}

static void draw_gui(void)
{
    switch (guistate.ui_state)
    {
    case UI_WELCOME:
        draw_welcome();
        break;
    case UI_MEMVIEW:
        draw_memory_view();
        break;
    case UI_FEATURES:
        draw_feature();
        break;
    case UI_FEATURE_HW_BREAK:
    case UI_FEATURE_WATCH:
    case UI_FEATURE_SW_BREAK:
        draw_feature_breakpoint();
        break;
    case UI_FEATURE_LIST_ALL_BKPT:
        draw_feature_breakpoint_list();
        break;
    case UI_FEATURE_HOTKEYS:
        draw_hotkey_config();
        break;
    case UI_FEATURE_SUSPEND:
    case UI_FEATURE_RESUME:
    case UI_FEATURE_STEP:
        break;
    default:
        draw_unknown_state();
        break;
    }
}

static void handle_welcome_confirm(void)
{
    SceKernelModuleInfo info = {.size = sizeof(SceKernelModuleInfo)};
    if (kernel_get_moduleinfo(&info) < 0)
        return;

    guistate.modinfo = info;
    guistate.ui_state = UI_MEMVIEW;
    guistate.active_area = MEMVIEW_HEX;
    guistate.cursor_column = 1;

    // Find module memory range
    for (int i = 0; i < 4; ++i)
    {
        if (guistate.modinfo.segments[i].vaddr && guistate.modinfo.segments[i].memsz > 0)
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

static void update_module_info_if_needed(void)
{
    if (guistate.modinfo.size == sizeof(SceKernelModuleInfo))
        return;

    if (kernel_get_moduleinfo(&guistate.modinfo) < 0)
        return;

    for (int i = 0; i < 4; ++i)
    {
        if (guistate.modinfo.segments[i].vaddr && guistate.modinfo.segments[i].memsz > 0)
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

int pebble_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    // Wait for shell and initialize
    while (ksceKernelSysrootGetShellPid() <= 0)
        ksceKernelDelayThread(500 * 1000);

    ksceKernelDelayThread(9 * 1000 * 1000);

    // Setup scratchpad
    uint32_t buffer;
    ksceSysconReadCommand(0xFA, &buffer, sizeof(buffer));
    if ((buffer | 0x100040) != buffer)
    {
        buffer |= 0x100040;
        ksceSysconSendCommand(0xFA, &buffer, sizeof(buffer));
    }

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
            ksceKernelDelayThread(5 * 1000 * 1000);
            continue;
        }

        ksceCtrlPeekBufferPositive(0, &ctrl, 1);
        uint32_t current_buttons = ctrl.buttons;
        uint32_t released = (prev_buttons & ~current_buttons);

        // Toggle GUI
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
                    prev_buttons = current_buttons;
                    ksceKernelDelayThread(33333);
                    continue;
                }
            }
            else
            {
                guistate.gui_visible = false;
                ksceKernelDebugResumeThread(g_target_process.main_thread_id, 0x100);
                prev_buttons = current_buttons;
                ksceKernelDelayThread(33333);
                continue;
            }
        }

        // Skip if GUI is hidden
        if (!guistate.gui_visible)
        {
            prev_buttons = current_buttons;
            ksceKernelDelayThread(33333);
            continue;
        }

        // GUI is visible - handle input and drawing
        ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        check_button_repeat(current_buttons, &released);

        // Handle input based on UI state
        if (guistate.ui_state == UI_WELCOME && (released & guistate.hotkeys.confirm))
        {
            handle_welcome_confirm();
        }
        else if (guistate.ui_state == UI_MEMVIEW)
        {
            update_module_info_if_needed();
            cache_dirty = true;
            read_memview_cache();
            handle_memview_input(released, current_buttons);
        }
        else if (guistate.ui_state >= UI_FEATURES)
            handle_feature_input(released);

        // Draw the GUI
        if (ksceKernelLockMutex(pebble_mtx_uid, 1, NULL) == 0)
        {
            draw_gui();
            ksceKernelUnlockMutex(pebble_mtx_uid, 1);
            ksceKernelSetEventFlag(evtflag, buf_index + 1);
            buf_index ^= 1;
        }

        prev_buttons = current_buttons;
        ksceKernelDelayThread(33333);
    }

    return 0;
}