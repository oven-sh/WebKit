/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "JSExportMacros.h"
#include <array>
#include <atomic>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class AbstractSlotVisitor;
class SlotVisitor;
class VM;

namespace AOT {

class Image;

// For an image built with --aot-testing=CheckProofsAtRunTime or --aot-validate. The compiler keeps a cell in the frame of a function where it has proven that nothing holds the cell once the function is left.
// In such an image the frame keeps the slot, and the cell lies in a range of its own where no address is handed out again for a long time. When the frame is left the cell becomes unreadable,
// so whoever still holds it stops at the next read or write, and the stop names the allocation.
class CheckedFrameCells {
    WTF_MAKE_TZONE_ALLOCATED(CheckedFrameCells);
    WTF_MAKE_NONCOPYABLE(CheckedFrameCells);
public:
    // Generated code tells a pointer into the range by its upper half.
#if ASAN_ENABLED
    // The allocator of the address sanitizer reserves the four terabytes in which the other address lies.
    static constexpr uintptr_t startOfRange = 0x200000000000;
#else
    static constexpr uintptr_t startOfRange = 0x7e0000000000;
#endif
    static constexpr unsigned bitsOfOffsetInRange = 32;

    struct Allocation {
        uint32_t function { 0 };
        uint32_t value { 0 };
        uint32_t module { 0 };
        uint32_t line { 0 };
        uint32_t column { 0 };
    };

    explicit CheckedFrameCells(VM&);
    ~CheckedFrameCells();

    static bool isInRange(const void* address) { return std::bit_cast<uintptr_t>(address) >> bitsOfOffsetInRange == startOfRange >> bitsOfOffsetInRange; }
    // False in every process whose image was built otherwise.
    static bool isInUse() { return s_isInUse.load(std::memory_order_relaxed); }

    // Of the VM, where a cell has been made for it.
    static CheckedFrameCells* find(VM&);
    static CheckedFrameCells& ensure(VM&);
    static void willDestroy(VM&);

    // The slot is where the cell lies in any other image. The frame is named by the address of one word in it. Gives the slot back where the range has no room,
    // and for most of the cells of an allocation that has run often: a cell in the range costs two system calls.
    void* make(void* slot, const void* wordOfFrame, size_t, const Allocation&);
    // The frame with this word is left, and so is every frame that it has called.
    void didLeaveFrameOf(const void* wordOfFrame);
    // Every frame that lies below the address is left.
    void didLeaveFramesBelow(const void* address);
    // What a cell of a frame refers to is alive as long as the frame is, as what a word of the stack refers to.
    void visitLiveCells(SlotVisitor&);
    void visitLiveCells(AbstractSlotVisitor&);

    // Prints which allocation the address belongs to.
    static void report(const void* addressInRange);

private:
    void end(uint32_t granule);
    template<typename Visitor> void visitLiveCellsWith(Visitor&);

    JS_EXPORT_PRIVATE static std::atomic<bool> s_isInUse;

    VM& m_vm;
    const Image& m_image;
    CheckedFrameCells* m_next { nullptr };
    // How often each allocation has run, by a hash of its ids.
    std::array<uint32_t, 4096> m_numberOfBirths { };
    // In the order of their births. A frame makes no cell while a frame that it has called is there, so the cells of the frames that are left first come last.
    Vector<uint32_t, 16> m_liveGranules;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
