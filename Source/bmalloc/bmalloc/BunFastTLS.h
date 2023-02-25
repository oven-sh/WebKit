/*
 * Copyright (c) 2003-2013 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright 1996 1995 by Open Software Foundation, Inc. 1997 1996 1995 1994 1993 1992 1991  
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */
/*
  Mimalloc:

  MIT License

  Copyright (c) 2018-2021 Microsoft Corporation, Daan Leijen

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/
#ifndef __PTHREAD_TSD_H__
#define __PTHREAD_TSD_H__


#ifndef __ASSEMBLER__


/* Constant TSD slots for inline pthread_getspecific() usage. */

// /* Keys 0 - 9 are for Libsyscall/libplatform usage */
// #define _PTHREAD_TSD_SLOT_PTHREAD_SELF              __TSD_THREAD_SELF
// #define _PTHREAD_TSD_SLOT_ERRNO                     __TSD_ERRNO
// #define _PTHREAD_TSD_SLOT_MIG_REPLY                 __TSD_MIG_REPLY
// #define _PTHREAD_TSD_SLOT_MACH_THREAD_SELF          __TSD_MACH_THREAD_SELF
// #define _PTHREAD_TSD_SLOT_PTHREAD_QOS_CLASS         __TSD_THREAD_QOS_CLASS
// #define _PTHREAD_TSD_SLOT_RETURN_TO_KERNEL          __TSD_RETURN_TO_KERNEL
// #define _PTHREAD_TSD_SLOT_PTR_MUNGE                 __TSD_PTR_MUNGE
// #define _PTHREAD_TSD_SLOT_MACH_SPECIAL_REPLY        __TSD_MACH_SPECIAL_REPLY
// #define _PTHREAD_TSD_SLOT_SEMAPHORE_CACHE           __TSD_SEMAPHORE_CACHE

// #define _PTHREAD_TSD_SLOT_PTHREAD_SELF_TYPE         pthread_t
// #define _PTHREAD_TSD_SLOT_ERRNO_TYPE                int *
// #define _PTHREAD_TSD_SLOT_MIG_REPLY_TYPE            mach_port_t
// #define _PTHREAD_TSD_SLOT_MACH_THREAD_SELF_TYPE     mach_port_t
// #define _PTHREAD_TSD_SLOT_PTHREAD_QOS_CLASS_TYPE    pthread_priority_t
// #define _PTHREAD_TSD_SLOT_RETURN_TO_KERNEL_TYPE     uintptr_t
// #define _PTHREAD_TSD_SLOT_PTR_MUNGE_TYPE            uintptr_t
// #define _PTHREAD_TSD_SLOT_MACH_SPECIAL_REPLY_TYPE   mach_port_t
// #define _PTHREAD_TSD_SLOT_SEMAPHORE_CACHE_TYPE      semaphore_t

/*
 * Windows 64-bit ABI bakes %gs relative accesses into its code in the same
 * range as our TSD keys.  To allow some limited interoperability for code
 * targeting that ABI, we leave slots 6 and 11 unused.
 *
 * The Go runtime on x86_64 also uses this because their ABI doesn't reserve a
 * register for the TSD base.  They were previously using an arbitrarily chosen
 * dynamic key and relying on being able to get it at runtime, but switched to
 * this slot to avoid issues with that approach.  It's assumed that Go and
 * Windows code won't run in the same address space.
 */
//#define _PTHREAD_TSD_SLOT_RESERVED_WIN64 6

/* Keys 10 - 29 are for Libc/Libsystem internal usage */
/* used as __pthread_tsd_first + Num  */
#define __PTK_LIBC_LOCALE_KEY		10
//#define __PTK_LIBC_RESERVED_WIN64	11
#define __PTK_LIBC_LOCALTIME_KEY	12
#define __PTK_LIBC_GMTIME_KEY		13
#define __PTK_LIBC_GDTOA_BIGINT_KEY	14
#define __PTK_LIBC_PARSEFLOAT_KEY	15
#define __PTK_LIBC_TTYNAME_KEY		16
/* for usage by dyld */
#define __PTK_LIBC_DYLD_Unwind_SjLj_Key	18

/* Keys 20-29 for libdispatch usage */
#define __PTK_LIBDISPATCH_KEY0		20
#define __PTK_LIBDISPATCH_KEY1		21
#define __PTK_LIBDISPATCH_KEY2		22
#define __PTK_LIBDISPATCH_KEY3		23
#define __PTK_LIBDISPATCH_KEY4		24
#define __PTK_LIBDISPATCH_KEY5		25
#define __PTK_LIBDISPATCH_KEY6		26
#define __PTK_LIBDISPATCH_KEY7		27
#define __PTK_LIBDISPATCH_KEY8		28
#define __PTK_LIBDISPATCH_KEY9		29

/* Keys 30-255 for Non Libsystem usage */

/* Keys 30-39 for Graphic frameworks usage */
#define _PTHREAD_TSD_SLOT_OPENGL	30	/* backwards compat sake */
#define __PTK_FRAMEWORK_OPENGL_KEY	30
#define __PTK_FRAMEWORK_GRAPHICS_KEY1	31
#define __PTK_FRAMEWORK_GRAPHICS_KEY2	32
#define __PTK_FRAMEWORK_GRAPHICS_KEY3	33
#define __PTK_FRAMEWORK_GRAPHICS_KEY4	34
#define __PTK_FRAMEWORK_GRAPHICS_KEY5	35
#define __PTK_FRAMEWORK_GRAPHICS_KEY6	36
#define __PTK_FRAMEWORK_GRAPHICS_KEY7	37
#define __PTK_FRAMEWORK_GRAPHICS_KEY8	38
#define __PTK_FRAMEWORK_GRAPHICS_KEY9	39

/* Keys 40-49 for Objective-C runtime usage */
#define __PTK_FRAMEWORK_OBJC_KEY0	40
#define __PTK_FRAMEWORK_OBJC_KEY1	41
#define __PTK_FRAMEWORK_OBJC_KEY2	42
#define __PTK_FRAMEWORK_OBJC_KEY3	43
#define __PTK_FRAMEWORK_OBJC_KEY4	44
#define __PTK_FRAMEWORK_OBJC_KEY5	45
#define __PTK_FRAMEWORK_OBJC_KEY6	46
#define __PTK_FRAMEWORK_OBJC_KEY7	47
#define __PTK_FRAMEWORK_OBJC_KEY8	48
#define __PTK_FRAMEWORK_OBJC_KEY9	49

/* Keys 50-59 for Core Foundation usage */
#define __PTK_FRAMEWORK_COREFOUNDATION_KEY0	50
#define __PTK_FRAMEWORK_COREFOUNDATION_KEY1	51
#define __PTK_FRAMEWORK_COREFOUNDATION_KEY2	52
#define __PTK_FRAMEWORK_COREFOUNDATION_KEY3	53
#define __PTK_FRAMEWORK_COREFOUNDATION_KEY4	54
#define __PTK_FRAMEWORK_COREFOUNDATION_KEY5	55
#define __PTK_FRAMEWORK_COREFOUNDATION_KEY6	56
#define __PTK_FRAMEWORK_COREFOUNDATION_KEY7	57
#define __PTK_FRAMEWORK_COREFOUNDATION_KEY8	58
#define __PTK_FRAMEWORK_COREFOUNDATION_KEY9	59

/* Keys 60-69 for Foundation usage */
#define __PTK_FRAMEWORK_FOUNDATION_KEY0		60
#define __PTK_FRAMEWORK_FOUNDATION_KEY1		61
#define __PTK_FRAMEWORK_FOUNDATION_KEY2		62
#define __PTK_FRAMEWORK_FOUNDATION_KEY3		63
#define __PTK_FRAMEWORK_FOUNDATION_KEY4		64
#define __PTK_FRAMEWORK_FOUNDATION_KEY5		65
#define __PTK_FRAMEWORK_FOUNDATION_KEY6		66
#define __PTK_FRAMEWORK_FOUNDATION_KEY7		67
#define __PTK_FRAMEWORK_FOUNDATION_KEY8		68
#define __PTK_FRAMEWORK_FOUNDATION_KEY9		69

/* Keys 70-79 for Core Animation/QuartzCore usage */
#define __PTK_FRAMEWORK_QUARTZCORE_KEY0		70
#define __PTK_FRAMEWORK_QUARTZCORE_KEY1		71
#define __PTK_FRAMEWORK_QUARTZCORE_KEY2		72
#define __PTK_FRAMEWORK_QUARTZCORE_KEY3		73
#define __PTK_FRAMEWORK_QUARTZCORE_KEY4		74
#define __PTK_FRAMEWORK_QUARTZCORE_KEY5		75
#define __PTK_FRAMEWORK_QUARTZCORE_KEY6		76
#define __PTK_FRAMEWORK_QUARTZCORE_KEY7		77
#define __PTK_FRAMEWORK_QUARTZCORE_KEY8		78
#define __PTK_FRAMEWORK_QUARTZCORE_KEY9		79


/* Keys 80-89 for CoreData */
#define __PTK_FRAMEWORK_COREDATA_KEY0		80
#define __PTK_FRAMEWORK_COREDATA_KEY1		81
#define __PTK_FRAMEWORK_COREDATA_KEY2		82
#define __PTK_FRAMEWORK_COREDATA_KEY3		83
#define __PTK_FRAMEWORK_COREDATA_KEY4		84
#define __PTK_FRAMEWORK_COREDATA_KEY5		85
#define __PTK_FRAMEWORK_COREDATA_KEY6		86
#define __PTK_FRAMEWORK_COREDATA_KEY7		87
#define __PTK_FRAMEWORK_COREDATA_KEY8		88
#define __PTK_FRAMEWORK_COREDATA_KEY9		89

/* Keys 90-94 for JavaScriptCore Collection */
#define __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY0		90
#define __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY1		91
#define __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY2		92
#define __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY3		93
#define __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY4		94
/* Keys 95 for CoreText */
#define __PTK_FRAMEWORK_CORETEXT_KEY0			95

/* Keys 100-109 are for the Swift runtime */
#define __PTK_FRAMEWORK_SWIFT_KEY0		100
#define __PTK_FRAMEWORK_SWIFT_KEY1		101
#define __PTK_FRAMEWORK_SWIFT_KEY2		102
#define __PTK_FRAMEWORK_SWIFT_KEY3		103
#define __PTK_FRAMEWORK_SWIFT_KEY4		104
#define __PTK_FRAMEWORK_SWIFT_KEY5		105
#define __PTK_FRAMEWORK_SWIFT_KEY6		106
#define __PTK_FRAMEWORK_SWIFT_KEY7		107
#define __PTK_FRAMEWORK_SWIFT_KEY8		108
#define __PTK_FRAMEWORK_SWIFT_KEY9		109

/* Keys 110-115 for libmalloc */
#define __PTK_LIBMALLOC_KEY0		110
#define __PTK_LIBMALLOC_KEY1		111
#define __PTK_LIBMALLOC_KEY2		112
#define __PTK_LIBMALLOC_KEY3		113
#define __PTK_LIBMALLOC_KEY4		114

/* Keys 115-120 for libdispatch workgroups */
#define __PTK_LIBDISPATCH_WORKGROUP_KEY0		115
#define __PTK_LIBDISPATCH_WORKGROUP_KEY1		116
#define __PTK_LIBDISPATCH_WORKGROUP_KEY2		117
#define __PTK_LIBDISPATCH_WORKGROUP_KEY3		118
#define __PTK_LIBDISPATCH_WORKGROUP_KEY4		119

/* Keys 190 - 194 are for the use of PerfUtils */
#define __PTK_PERF_UTILS_KEY0		190
#define __PTK_PERF_UTILS_KEY1		191
#define __PTK_PERF_UTILS_KEY2		192
#define __PTK_PERF_UTILS_KEY3		193
#define __PTK_PERF_UTILS_KEY4		194

/* Keys 210 - 229 are for libSystem usage within the iOS Simulator */
/* They are offset from their corresponding libSystem keys by 200 */
#define __PTK_LIBC_SIM_LOCALE_KEY	210
#define __PTK_LIBC_SIM_TTYNAME_KEY	211
#define __PTK_LIBC_SIM_LOCALTIME_KEY	212
#define __PTK_LIBC_SIM_GMTIME_KEY	213
#define __PTK_LIBC_SIM_GDTOA_BIGINT_KEY	214
#define __PTK_LIBC_SIM_PARSEFLOAT_KEY	215


// extern void *pthread_getspecific(unsigned long);
// extern int pthread_setspecific(unsigned long, const void *);

/* setup destructor function for static key as it is not created with pthread_key_create() */
// if CPP
#ifdef __cplusplus
extern "C" int pthread_key_init_np(int, void (*)(void *));
#else
extern int pthread_key_init_np(int, void (*)(void *));
#endif

// extern int _pthread_setspecific_static(unsigned long, void *);

#ifndef MI_UNUSED
#define MI_UNUSED(x) ((void)(x))
#endif

#ifndef ___BUN_FAST_TLS_INLINE
#define ___BUN_FAST_TLS_INLINE inline __attribute__((__always_inline__))
#endif

static ___BUN_FAST_TLS_INLINE void* internal_tls_slot_get(unsigned long slot) {
  void* res;
  const unsigned long ofs = (slot*sizeof(void*));
  #if defined(__i386__)
    __asm__("movl %%gs:%1, %0" : "=r" (res) : "m" (*((void**)ofs)) : );  // x86 32-bit always uses GS
  #elif defined(__APPLE__) && defined(__x86_64__)
    __asm__("movq %%gs:%1, %0" : "=r" (res) : "m" (*((void**)ofs)) : );  // x86_64 macOSX uses GS
//   #elif defined(__x86_64__) && (sizeof(intptr_t)==4)
//     __asm__("movl %%fs:%1, %0" : "=r" (res) : "m" (*((void**)ofs)) : );  // x32 ABI
  #elif defined(__x86_64__)
    __asm__("movq %%fs:%1, %0" : "=r" (res) : "m" (*((void**)ofs)) : );  // x86_64 Linux, BSD uses FS
  #elif defined(__arm__)
    void** tcb; MI_UNUSED(ofs);
    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 3\nbic %0, %0, #3" : "=r" (tcb));
    res = tcb[slot];
  #elif defined(__aarch64__)
    void** tcb; MI_UNUSED(ofs);
    #if defined(__APPLE__) // M1, issue #343
    __asm__ volatile ("mrs %0, tpidrro_el0\nbic %0, %0, #7" : "=r" (tcb));
    #else
    __asm__ volatile ("mrs %0, tpidr_el0" : "=r" (tcb));
    #endif
    res = tcb[slot];
  #endif
  return res;
}

static ___BUN_FAST_TLS_INLINE void internal_tls_slot_set(unsigned long slot, void* value) {
  const unsigned long ofs = (slot*sizeof(void*));
  #if defined(__i386__)
    __asm__("movl %1,%%gs:%0" : "=m" (*((void**)ofs)) : "rn" (value) : );  // 32-bit always uses GS
  #elif defined(__APPLE__) && defined(__x86_64__)
    __asm__("movq %1,%%gs:%0" : "=m" (*((void**)ofs)) : "rn" (value) : );  // x86_64 macOS uses GS
//   #elif defined(__x86_64__) && (sizeof void* ==4)
//     __asm__("movl %1,%%fs:%0" : "=m" (*((void**)ofs)) : "rn" (value) : );  // x32 ABI
  #elif defined(__x86_64__)
    __asm__("movq %1,%%fs:%0" : "=m" (*((void**)ofs)) : "rn" (value) : );  // x86_64 Linux, BSD uses FS
  #elif defined(__arm__)
    void** tcb; MI_UNUSED(ofs);
    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 3\nbic %0, %0, #3" : "=r" (tcb));
    tcb[slot] = value;
  #elif defined(__aarch64__)
    void** tcb; MI_UNUSED(ofs);
    #if defined(__APPLE__) // M1, issue #343
    __asm__ volatile ("mrs %0, tpidrro_el0\nbic %0, %0, #7" : "=r" (tcb));
    #else
    __asm__ volatile ("mrs %0, tpidr_el0" : "=r" (tcb));
    #endif
    tcb[slot] = value;
  #endif
}

/* To be used with static constant keys only */
___BUN_FAST_TLS_INLINE void *
_pthread_getspecific_direct(unsigned long slot)
{
// #if TARGET_IPHONE_SIMULATOR
// 	return pthread_getspecific(slot);
// #else
	return internal_tls_slot_get(slot);
// #endif
}

/* To be used with static constant keys only, assumes destructor is
 * already setup (with pthread_key_init_np) */
___BUN_FAST_TLS_INLINE int
_pthread_setspecific_direct(unsigned long slot, void * val)
{
// #if TARGET_IPHONE_SIMULATOR
// 	return _pthread_setspecific_static(slot, val);
// #else
	internal_tls_slot_set(slot, val);
  return 0;
// #endif
}


#endif /* ! __ASSEMBLER__ */
#endif /* __PTHREAD_TSD_H__ */