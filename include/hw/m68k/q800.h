/*
 * QEMU Motorla 680x0 Macintosh hardware System Emulator
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_Q800_H
#define HW_Q800_H

/*
 * The GLUE (General Logic Unit) is an Apple custom integrated circuit chip
 * that performs a variety of functions (RAM management, clock generation, ...).
 * The GLUE chip receives interrupt requests from various devices,
 * assign priority to each, and asserts one or more interrupt line to the
 * CPU.
 */

#define TYPE_GLUE "q800-glue"
OBJECT_DECLARE_SIMPLE_TYPE(GLUEState, GLUE)

struct GLUEState {
    SysBusDevice parent_obj;
    M68kCPU *cpu;
    uint8_t ipr;
};


/*
 * The main Q800 machine
 */

typedef struct Q800MachineState {
    MachineState parent_obj;

    M68kCPU *cpu;
    MemoryRegion rom;
    DeviceState *djmemc;
    MemoryRegion ramio;

    MemoryRegion macio;
    MemoryRegion macio_alias;
    MemoryRegion machine_id;
} Q800MachineState;

#define TYPE_Q800_MACHINE MACHINE_TYPE_NAME("q800")
OBJECT_DECLARE_SIMPLE_TYPE(Q800MachineState, Q800_MACHINE)

#endif
