#include "kernel.h"
#include "reVita.h"

State guistate;

static const MemLayoutInfo layout_info[] = {
    [MEM_LAYOUT_8BIT]  = {"%02X", 1},
    [MEM_LAYOUT_16BIT] = {"%04X", 2},
    [MEM_LAYOUT_32BIT] = {"%08X", 4}
};

static void save_hotkeys() {
    SceUID fd = ksceIoOpen(HOTKEY_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "%d %d %d %d %d",
                           guistate.hotkeys.show_gui,
                           guistate.hotkeys.area_select,
                           guistate.hotkeys.edit,
                           guistate.hotkeys.cancel,
                           guistate.hotkeys.confirm);
        ksceIoWrite(fd, buf, len);
        ksceIoClose(fd);
    }
}

void load_hotkeys() {
    guistate.hotkeys = (HotkeyConfig){ DEFAULT_SHOW_GUI, DEFAULT_AREA_SELECT,
                                       DEFAULT_EDIT_MODE, DEFAULT_CANCEL, DEFAULT_CONFIRM };
    SceUID fd = ksceIoOpen(HOTKEY_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char buf[64];
        ksceIoRead(fd, buf, sizeof(buf));
        sscanf(buf, "%d %d %d %d %d",
               &guistate.hotkeys.show_gui,
               &guistate.hotkeys.area_select,
               &guistate.hotkeys.edit,
               &guistate.hotkeys.cancel,
               &guistate.hotkeys.confirm);
        ksceIoClose(fd);
    }
}

static void update_memview_state() {
    kernel_list_breakpoints(guistate.breakpoints);
    for (int i = 0; i < 16; i++) {
        if (guistate.breakpoints[i].type) {
            kernel_get_registers(&guistate.regs);
            guistate.stack_size = kernel_read_memory((void *)guistate.regs.sp, guistate.stack, sizeof(guistate.stack)) / 4;
            guistate.callstack[0] = guistate.regs.pc;
            guistate.callstack[1] = guistate.regs.lr;
            guistate.callstack_size = 2;
            break;
        }
    }
}

static void draw_hex_row(uint32_t addr) {
    uint8_t data[16];
    kernel_read_memory((void *)addr, data, 16);
    char line[64], ascii[17] = {0};
    snprintf(line, 12, "%08X: ", addr);
    for (int i = 0; i < 16; i += layout_info[guistate.mem_layout].bytes) {
        uint32_t val;
        memcpy(&val, data + i, layout_info[guistate.mem_layout].bytes);
        snprintf(line + 10 + i * 3, 9, layout_info[guistate.mem_layout].format, val);
        for (int j = 0; j < layout_info[guistate.mem_layout].bytes; j++) {
            ascii[i + j] = data[i + j] >= 32 && data[i + j] < 127 ? data[i + j] : '.';
        }
    }
    if (addr == guistate.addr) {
        rendererv_drawRectangle(10, 80 + ((addr - guistate.base_addr) / 16) * 20, 400, 20, 0x80FFFFFF);
    }
    rendererv_drawStringF(10, 80 + ((addr - guistate.base_addr) / 16) * 20, "%s  %s", line, ascii);
}

static void draw_registers(int x, int y) {
    int has_bp = 0;
    for (int i = 0; i < 16 && !has_bp; has_bp = guistate.breakpoints[i++].type);
    if (!has_bp) return;
    const char *regs[] = {"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "R10", "R11", "R12", "SP", "LR", "PC"};
    for (int i = 0; i < 16; i++, y += 20) {
        rendererv_drawStringF(x, y, "%-3s: %08X", regs[i], ((uint32_t *)&guistate.regs)[i]);
    }
}

static void handle_feature_input(SceCtrlData *ctrl) {
    switch (guistate.ui_state) {
        case UI_FEATURE_HW_BREAK: {
            guistate.edit_feature += (ctrl->buttons & SCE_CTRL_UP ? 0x10 : 0) - (ctrl->buttons & SCE_CTRL_DOWN ? 0x10 : 0);
            if (ctrl->buttons & guistate.hotkeys.confirm) {
                kernel_set_hardware_breakpoint(guistate.edit_feature);
            }
            break;
        }
        case UI_FEATURE_SW_BREAK: {
            if (ctrl->buttons & SCE_CTRL_UP) {
                guistate.edit_feature++;
            }
            if (ctrl->buttons & guistate.hotkeys.confirm) {
                kernel_set_software_breakpoint(guistate.edit_feature, guistate.regs.cpsr & 0x20);
            }
            break;
        }
        case UI_FEATURE_CLEAR: {
            guistate.edit_feature = CLAMP(guistate.edit_feature + (ctrl->buttons & SCE_CTRL_UP ? 1 : -1), 0, 15);
            if (ctrl->buttons & guistate.hotkeys.confirm) {
                kernel_clear_breakpoint(guistate.edit_feature);
            }
            break;
        }
        case UI_FEATURE_HOTKEYS: {
            if (ctrl->buttons & SCE_CTRL_UP)   guistate.edit_feature = (guistate.edit_feature - 1 + 5) % 5;
            if (ctrl->buttons & SCE_CTRL_DOWN) guistate.edit_feature = (guistate.edit_feature + 1) % 5;
            int *keys = (int *)&guistate.hotkeys;
            if (ctrl->buttons & SCE_CTRL_LEFT)  keys[guistate.edit_feature] >>= 1;
            if (ctrl->buttons & SCE_CTRL_RIGHT) keys[guistate.edit_feature] <<= 1;
            if (ctrl->buttons & guistate.hotkeys.confirm) {
                save_hotkeys();
            }
            break;
        }
        default:
            break;
    }
    if (ctrl->buttons & (guistate.hotkeys.cancel | guistate.hotkeys.confirm)) {
        guistate.ui_state = UI_MEMVIEW;
    }
}

static void handle_memview_input(SceCtrlData *ctrl) {
    if (ctrl->buttons & guistate.hotkeys.area_select) {
        if (ctrl->buttons & SCE_CTRL_LEFT)       guistate.active_area = MEMVIEW_HEX;
        else if (ctrl->buttons & SCE_CTRL_UP)    guistate.active_area = MEMVIEW_REGS;
        else if (ctrl->buttons & SCE_CTRL_RIGHT) guistate.active_area = MEMVIEW_STACK;
        return;
    }

    if (guistate.active_area == MEMVIEW_HEX) {
        if (ctrl->buttons & guistate.hotkeys.edit) {
            if (guistate.edit_mode == EDIT_NONE) {
                guistate.edit_mode = EDIT_ADDRESS;
                guistate.modified_addr = guistate.addr;
            } else if (guistate.edit_mode == EDIT_ADDRESS) {
                guistate.edit_mode = EDIT_VALUE;
                SceSize size = layout_info[guistate.mem_layout].bytes;
                // Read current value into buffer
                memcpy(guistate.modified_value, (void *)guistate.addr, size);
                guistate.edit_offset = 0; // Start at first byte
            } else {
                guistate.edit_mode = EDIT_NONE;
            }
        }

        if (guistate.edit_mode == EDIT_ADDRESS) {
            // Handle address editing
            int delta = (ctrl->buttons & SCE_CTRL_UP ? 1 : 0) + (ctrl->buttons & SCE_CTRL_DOWN ? -1 : 0);
            delta += (ctrl->buttons & SCE_CTRL_RIGHT ? 1 : 0) + (ctrl->buttons & SCE_CTRL_LEFT ? -1 : 0);
            guistate.modified_addr += delta;
            
            // Clamp address to valid range
            guistate.modified_addr = CLAMP(guistate.modified_addr,
                (uint32_t)guistate.modinfo.segments[0].vaddr,
                (uint32_t)guistate.modinfo.segments[0].vaddr + guistate.modinfo.size);

            if (ctrl->buttons & guistate.hotkeys.confirm) {
                guistate.addr = guistate.modified_addr;
                guistate.edit_mode = EDIT_NONE;
            }
        } else if (guistate.edit_mode == EDIT_VALUE) {
            SceSize size = layout_info[guistate.mem_layout].bytes;
            
            // Navigate between bytes
            if (ctrl->buttons & SCE_CTRL_LEFT) {
                guistate.edit_offset = (guistate.edit_offset - 1 + size) % size;
            }
            if (ctrl->buttons & SCE_CTRL_RIGHT) {
                guistate.edit_offset = (guistate.edit_offset + 1) % size;
            }

            // Modify selected byte
            if (ctrl->buttons & SCE_CTRL_UP) {
                guistate.modified_value[guistate.edit_offset]++;
            }
            if (ctrl->buttons & SCE_CTRL_DOWN) {
                guistate.modified_value[guistate.edit_offset]--;
            }

            // Apply changes
            if (ctrl->buttons & guistate.hotkeys.confirm) {
                SceKernelMemBlockInfo info;
                if (sceKernelGetMemBlockInfoByAddr((void *)guistate.addr, &info) >= 0) {
                    if (info.access & SCE_KERNEL_MEMBLOCK_TYPE_USER_RX) {
                        kernel_write_instruction((void *)guistate.addr, guistate.modified_value, size);
                    } else {
                        kernel_write_memory((void *)guistate.addr, guistate.modified_value, size);
                    }
                }
                guistate.edit_mode = EDIT_NONE;
            }
        } else {
            int delta = (ctrl->buttons & SCE_CTRL_UP ? -16 : 0) + (ctrl->buttons & SCE_CTRL_DOWN ? 16 : 0);
            delta += (ctrl->buttons & SCE_CTRL_LEFT ? -layout_info[guistate.mem_layout].bytes : 0) +
                     (ctrl->buttons & SCE_CTRL_RIGHT ? layout_info[guistate.mem_layout].bytes : 0);
            guistate.addr = CLAMP((uint32_t)(guistate.addr + delta),
                (uint32_t)guistate.modinfo.segments[0].vaddr,
                (uint32_t)guistate.modinfo.segments[0].vaddr + guistate.modinfo.size);
        }

        guistate.base_addr = guistate.addr & ~0xF;
        if (ctrl->buttons & SCE_CTRL_L1) guistate.mem_layout = (guistate.mem_layout + 2) % 3;
        if (ctrl->buttons & SCE_CTRL_R1) guistate.mem_layout = (guistate.mem_layout + 1) % 3;
        if (ctrl->buttons & SCE_CTRL_TRIANGLE) kernel_set_hardware_breakpoint(guistate.addr);
        if (ctrl->buttons & SCE_CTRL_SQUARE)   kernel_set_software_breakpoint(guistate.addr, guistate.regs.cpsr & 0x20);
    } else if (guistate.active_area == MEMVIEW_STACK) {
        if (ctrl->buttons & SCE_CTRL_LEFT)  guistate.view_state = (guistate.view_state - 1 + 3) % 3;
        if (ctrl->buttons & SCE_CTRL_RIGHT) guistate.view_state = (guistate.view_state + 1) % 3;
    }
}

static void draw_ui() {
    rendererv_drawRectangle(0, 0, 960, 544, 0x80171717);

    switch (guistate.ui_state) {
        case UI_WELCOME: {
            rendererv_drawString(100, 80, "PebbleVita Debugger");
            if (guistate.pid > 0) {
                rendererv_drawStringF(100, 140, "Process ID: %08X", guistate.pid);
                rendererv_drawStringF(100, 170, "Module: %s", guistate.modinfo.module_name);
                rendererv_drawString(100, 200, "Press CROSS to attach");
            }
            break;
        }

        case UI_MEMVIEW: {
            for (uint32_t addr = guistate.base_addr; addr < guistate.base_addr + 0x100; addr += 16) {
                draw_hex_row(addr);
            }
            if (guistate.active_area == MEMVIEW_REGS) {
                draw_registers(700, 80);
            } else if (guistate.active_area == MEMVIEW_STACK) {
                switch (guistate.view_state) {
                    case VIEW_STACK:
                        for (int i = 0; i < guistate.stack_size && i < 16; i++) {
                            rendererv_drawStringF(700, 100 + i * 20, "%08X: %08X", guistate.regs.sp + i * 4, guistate.stack[i]);
                        }
                        break;
                    case VIEW_CALLSTACK:
                        for (int i = 0; i < guistate.callstack_size; i++) {
                            rendererv_drawStringF(700, 100 + i * 20, "[%d] %08X", i, guistate.callstack[i]);
                        }
                        break;
                    case VIEW_BREAKPOINTS:
                        for (int i = 0; i < 16; i++) {
                            if (guistate.breakpoints[i].type) {
                                const char *types[] = {"SW", "HW", "WP-R", "WP-W", "WP-RW"};
                                rendererv_drawStringF(700, 100 + i * 20, "[%d] %s @ %08X", i, types[guistate.breakpoints[i].type - 1], guistate.breakpoints[i].address);
                            }
                        }
                        break;
                }
            }
            break;
        }

        case UI_FEATURES: {
            const char *features[] = {"HW Break", "Watchpoint", "SW Break", "Clear BP", "Suspend",
                                      "Resume", "Step", "Hotkeys"};
            for (uint32_t i = 0; i < 8; i++) {
                rendererv_drawStringF(50, 60 + i * 25, features[i], i == guistate.edit_feature ? 0xFF00FF00 : 0xFFFFFFFF);
            }
            break;
        }

        default:
            break;
    }
}

int pebble_thread(SceSize args, void *argp) {
    ksceKernelDelayThread(5 * 1000 * 1000);
    
    (void) args;
    (void) argp;
    uint32_t bits;
    SceCtrlData ctrl;
    SceDisplayFrameBuf fb;
    fb.size = sizeof(SceDisplayFrameBuf);

    if (register_handler() < 0) {
        ksceKernelPrintf("RegisterHandler failed\n");
        return -1;
    } else {
        ksceKernelPrintf("RegisterHandler GOOD!!!!\n");
    }

    while (1) {
        ksceKernelPrintf("Pebble is loading!\n");
        ksceCtrlPeekBufferPositive(0, &ctrl, 1);

        if (guistate.ui_state == UI_WELCOME && (ctrl.buttons & guistate.hotkeys.confirm)) {
            kernel_debugger_attach(guistate.pid);
            SceKernelModuleInfo info = {.size = sizeof(SceKernelModuleInfo)};
            kernel_get_moduleinfo(&info);
            guistate.modinfo = info;
            guistate.ui_state = UI_MEMVIEW;
            guistate.active_area = MEMVIEW_HEX;
            guistate.addr = (uint32_t)info.segments[0].vaddr;
        } else if (guistate.ui_state >= UI_FEATURE_HW_BREAK) {
            handle_feature_input(&ctrl);
        } else if (guistate.ui_state == UI_FEATURES) {
            if (ctrl.buttons & SCE_CTRL_UP)   guistate.edit_feature = (guistate.edit_feature - 1 + 8) % 8;
            if (ctrl.buttons & SCE_CTRL_DOWN) guistate.edit_feature = (guistate.edit_feature + 1) % 8;
            if (ctrl.buttons & guistate.hotkeys.confirm) {
                switch (guistate.edit_feature) {
                    case 0: guistate.ui_state = UI_FEATURE_HW_BREAK; guistate.edit_feature = guistate.addr; break;
                    case 2: guistate.ui_state = UI_FEATURE_SW_BREAK; guistate.edit_feature = guistate.addr; break;
                    case 3: guistate.ui_state = UI_FEATURE_CLEAR;    guistate.edit_feature = 0;           break;
                    case 7: guistate.ui_state = UI_FEATURE_HOTKEYS;  guistate.edit_feature = 0;           break;
                    default: guistate.ui_state = UI_FEATURES + guistate.edit_feature + 1; break; // Potential issue: UI state enum mapping assumed
                }
            }
            if (ctrl.buttons & guistate.hotkeys.cancel) {
                guistate.ui_state = UI_MEMVIEW;
            }
        } else if ((ctrl.buttons & guistate.hotkeys.show_gui) || ksceKernelWaitEventFlag(g_breakpoint_triggered, 0x1, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR, &bits, NULL) >= 0) {
            handle_memview_input(&ctrl);
            update_memview_state();
            ksceKernelPrintf("Pebble is loaded!!!\n"); // Changed from original "loaded!!!"
            if (ksceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0) {
                renderer_setFB(&fb);
                draw_ui();
                renderer_writeFromVFB(0, false);
            }
        }

        ksceKernelDelayThread(16666);
    }
    return 0;
}