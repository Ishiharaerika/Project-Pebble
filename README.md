# Project Pebble.

## Description

**Project Pebble.** is a lightweight debugger tool-set for the PlayStation Vita, designed to provide basic debugging capabilities for user/kernel processes.\
\
The debugger works by installing kernel exception handlers and utilizing privileged kernel functions/taiHEN functions to manage hardware/software breakpoints and process states.\

## Features
** IMPORTANT NOTICE: This plugin may only work on firmware 3.65 currently.**
** Currently developing. Please open an issue for ideas or bugs.**
* **Process Attachment:** Attach to a target process using its Process ID (PID).
* **Hardware Breakpoints:** Set hardware execution breakpoints at specific memory addresses.
* **Watchpoints:** Set hardware watchpoints on memory addresses to break on read, write, or read/write access.
* **Software Breakpoints:** Inject software breakpoint instructions (Thumb `0xBE00` or ARM `0xE1200070`) into the target process code.
* **Breakpoint Management:** Individual breakpoints/watchpoints list.
* **State Inspection:**
    * Retrieve the general-purpose CPU registers (r0-r12, sp, lr, pc) of the thread that triggered an exception.
    * Retrieve the call stack of the thread that triggered an exception.
* **Execution Control:**
    * Suspend the entire target process.
    * Resume the entire target process.
    * Perform single-step execution after a breakpoint is hit.
* **Exception Handling:** Catches Prefetch Abort (PABT), Data Abort (DABT), and Undefined Instruction exceptions within the target process.

## Future/Roadmap
* **Implement user-interface for GUI debugging.
* **Add user-mode application (VPK) for sending commands through TCP.

## Prerequisites

* enso_ex with [PLUGIN](https://github.com/Ishiharaerika/setdip/raw/refs/heads/main/bin/HWBKPTdip.skprx) to perform DIP Switch Bit flipping.
* Follow [INSTRUCTIONS](https://github.com/SKGleba/enso_ex?tab=readme-ov-file#synchronize-enso_ex-plugins) on how to install that plugin.

## Building

1.  Clone or download the repository.
2.  Navigate to kernel/.
3.  One-Liner:
    ```bash
    mkdir build && cd build && cmake .. && make
    ```

This generates the kernel module `pebble_k.skprx`.

## Usage (API)

**Call Exported Functions:** Use the following functions exported by the kernel module:

    int kernel_debugger_attach(SceUID pid);
     * Attaches the debugger to the specified process PID.
    int kernel_set_hardware_breakpoint(SceUID pid, uint32_t address);
     * Sets a hardware execution breakpoint.
    int kernel_set_watchpoint(SceUID pid, uint32_t address, WatchPointBreakType type);
     * Sets a hardware watchpoint (`type` can be `BREAK_READ`, `BREAK_WRITE`, `BREAK_READ_WRITE`).
    int kernel_set_software_breakpoint(SceUID pid, uint32_t address, SlotType type);
     * Sets a software breakpoint (`type` should be `SW_BREAKPOINT_THUMB` or `SW_BREAKPOINT_ARM`).
    int kernel_clear_breakpoint(int index);
     * Clears the breakpoint/watchpoint at the given index (obtainable from `kernel_list_breakpoints`).
    int kernel_list_breakpoints(ActiveBKPTSlot *user_dst);
     * Copies the list of active breakpoints/watchpoints into the user-provided buffer `user_dst`. Returns the number of active slots or an error code.
    int kernel_get_registers(ExceptionContext *user_dst);
     * Copies the saved CPU context from the last exception into `user_dst`.
    int kernel_get_callstack(uint32_t *user_dst, int depth);
     * Copies the call stack (up to `depth` entries) into `user_dst`. Returns the number of frames retrieved or an error code.
    int kernel_suspend_process(SceUID pid);
     * Suspends the target process.
    int kernel_resume_process(SceUID pid);
     * Resumes the target process (usually after a breakpoint/exception).
    int kernel_single_step(void);
     * Executes the next instruction after a breakpoint hit.

## Credits
Princess of Sleeping: Hardware Breakpoint/Watchpoint usage.\
DaveeFTW: kvdb usage.
