/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Kernel {

ALWAYS_INLINE void cli()
{
    asm volatile("cli" ::
                     : "memory");
}
ALWAYS_INLINE void sti()
{
    asm volatile("sti" ::
                     : "memory");
}
ALWAYS_INLINE FlatPtr cpu_flags()
{
    FlatPtr flags;
    asm volatile(
        "pushf\n"
        "pop %0\n"
        : "=rm"(flags)::"memory");
    return flags;
}

#if ARCH(I386)
ALWAYS_INLINE void set_fs(u16 segment)
{
    asm volatile(
        "mov %%ax, %%fs" ::"a"(segment)
        : "memory");
}

ALWAYS_INLINE void set_gs(u16 segment)
{
    asm volatile(
        "mov %%ax, %%gs" ::"a"(segment)
        : "memory");
}

ALWAYS_INLINE u16 get_fs()
{
    u16 fs;
    asm("mov %%fs, %%eax"
        : "=a"(fs));
    return fs;
}

ALWAYS_INLINE u16 get_gs()
{
    u16 gs;
    asm("mov %%gs, %%eax"
        : "=a"(gs));
    return gs;
}
#endif

ALWAYS_INLINE u32 read_fs_u32(u32 offset)
{
    u32 val;
    asm volatile(
        "movl %%fs:%a[off], %k[val]"
        : [val] "=r"(val)
        : [off] "ir"(offset));
    return val;
}

ALWAYS_INLINE FlatPtr read_fs_ptr(u32 offset)
{
    return read_fs_u32(offset);
}

ALWAYS_INLINE void write_fs_u32(u32 offset, u32 val)
{
    asm volatile(
        "movl %k[val], %%fs:%a[off]" ::[off] "ir"(offset), [val] "ir"(val)
        : "memory");
}

ALWAYS_INLINE bool are_interrupts_enabled()
{
    return cpu_flags() & 0x200;
}

FlatPtr read_cr0();
FlatPtr read_cr2();
FlatPtr read_cr3();
FlatPtr read_cr4();
u64 read_xcr0();

void write_cr0(FlatPtr);
void write_cr3(FlatPtr);
void write_cr4(FlatPtr);
void write_xcr0(u64);

void flush_idt();

ALWAYS_INLINE void load_task_register(u16 selector)
{
    asm("ltr %0" ::"r"(selector));
}

FlatPtr read_dr0();
void write_dr0(FlatPtr);
FlatPtr read_dr1();
void write_dr1(FlatPtr);
FlatPtr read_dr2();
void write_dr2(FlatPtr);
FlatPtr read_dr3();
void write_dr3(FlatPtr);
FlatPtr read_dr6();
void write_dr6(FlatPtr);
FlatPtr read_dr7();
void write_dr7(FlatPtr);

ALWAYS_INLINE static bool is_kernel_mode()
{
    u16 cs;
    asm volatile(
        "mov %%cs, %[cs] \n"
        : [cs] "=g"(cs));
    return (cs & 3) == 0;
}

ALWAYS_INLINE void read_tsc(u32& lsw, u32& msw)
{
    asm volatile("rdtsc"
                 : "=d"(msw), "=a"(lsw));
}

ALWAYS_INLINE u64 read_tsc()
{
    u32 lsw;
    u32 msw;
    read_tsc(lsw, msw);
    return ((u64)msw << 32) | lsw;
}

void stac();
void clac();

[[noreturn]] ALWAYS_INLINE void halt_this()
{
    for (;;) {
        asm volatile("cli; hlt");
    }
}

}
