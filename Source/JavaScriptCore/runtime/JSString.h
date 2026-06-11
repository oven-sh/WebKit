/*
 *  Copyright (C) 1999-2001 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2003-2023 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include "ArgList.h"
#include "CallFrame.h"
#include "CommonIdentifiers.h"
#include "EnsureStillAliveHere.h"
#include "ExceptionEventLocation.h"
#include "GCOwnedDataScope.h"
#include "GetVM.h"
#include "Identifier.h"
#include "PropertyDescriptor.h"
#include "PropertySlot.h"
#include "Structure.h"
#include "ThrowScope.h"
#include <array>
#include <wtf/CheckedArithmetic.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Atomics.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringView.h>

#if USE(BUN_JSC_ADDITIONS)
#include <wtf/ForkExtras.h>
#endif

#if OS(DARWIN)
#include <mach/vm_param.h>
#endif

namespace JSC {

class JSString;
class JSRopeString;
class LLIntOffsetsExtractor;

JSString* jsEmptyString(VM&);
JSString* jsString(VM&, const String&); // returns empty string if passed null string
JSString* jsString(VM&, String&&); // returns empty string if passed null string
JSString* jsString(VM&, const AtomString&); // returns empty string if passed null string
JSString* jsString(VM&, AtomString&&); // returns empty string if passed null string
JSString* jsString(VM&, StringView); // returns empty string if passed null string

JSString* jsString(VM&, RefPtr<AtomStringImpl>&&);
JSString* jsString(VM&, Ref<AtomStringImpl>&&);
JSString* jsString(VM&, Ref<StringImpl>&&);

JSString* jsSingleCharacterString(VM&, char16_t);
JSString* jsSingleCharacterString(VM&, Latin1Character);
JSString* jsSubstring(VM&, const String&, unsigned offset, unsigned length);

// Non-trivial strings are two or more characters long.
// These functions are faster than just calling jsString.
JSString* jsNontrivialString(VM&, const String&);
JSString* jsNontrivialString(VM&, String&&);

// Should be used for strings that are owned by an object that will
// likely outlive the JSValue this makes, such as the parse tree or a
// DOM object that contains a String
JSString* jsOwnedString(VM&, const String&);

bool isJSString(JSCell*);
bool isJSString(JSValue);
JSString* asString(JSValue);

// In 64bit architecture, JSString and JSRopeString have the following memory layout to make sizeof(JSString) == 16 and sizeof(JSRopeString) == 32.
// JSString has only one pointer. We use it for String. length() and is8Bit() queries go to StringImpl. In JSRopeString, we reuse the above pointer
// place for the 1st fiber. JSRopeString has three fibers so its size is 48. To keep length and is8Bit flag information in JSRopeString, JSRopeString
// encodes these information into the fiber pointers. is8Bit flag is encoded in the 1st fiber pointer. length is embedded directly, and two fibers
// are compressed into 12bytes. isRope information is encoded in the first fiber's LSB.
//
// Since length of JSRopeString should be frequently accessed compared to each fiber, we put length in contiguous 32byte field, and compress 2nd
// and 3rd fibers into the following 80byte fields. One problem is that now 2nd and 3rd fibers are split. Storing and loading 2nd and 3rd fibers
// are not one pointer load operation. To make concurrent collector work correctly, we must initialize 2nd and 3rd fibers at JSRopeString creation
// and we must not modify these part later.
//
//              0                        8        10               16          20           24           26           28           32
// JSString     [   ID      ][  header  ][   String pointer      0]
// JSRopeString [   ID      ][  header  ][   1st fiber         xyz][  length  ][2nd lower32][2nd upper16][3rd lower16][3rd upper32]
//                                                               ^
//                                            x:(is8Bit),y:(isSubstring),z:(isRope) bit flags
class JSString : public JSCell {
public:
    friend class JIT;
    friend class VM;
    friend class SpecializedThunkJIT;
    friend class JSRopeString;
    friend class MarkStack;
    friend class SlotVisitor;
    friend class SmallStrings;

    typedef JSCell Base;
    // Do we really need OverridesGetOwnPropertySlot?
    // FIXME: https://bugs.webkit.org/show_bug.cgi?id=212956
    // Do we really need InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero?
    // FIXME: https://bugs.webkit.org/show_bug.cgi?id=212958
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | StructureIsImmortal | OverridesPut;
    static constexpr uint8_t numberOfLowerTierPreciseCells = 0;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    // We specialize the string subspace to get the fastest possible sweep. This wouldn't be
    // necessary if JSString didn't have a destructor.
    template<typename, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.stringSpace();
    }

    // We employ overflow checks in many places with the assumption that MaxLength
    // is INT_MAX. Hence, it cannot be changed into another length value without
    // breaking all the bounds and overflow checks that assume this.
    static constexpr unsigned MaxLength = std::numeric_limits<int32_t>::max();
    static_assert(MaxLength == String::MaxLength);

    // Minimum rope length for rope-walk optimizations (tryFindOneChar, tryReplaceOneChar).
    static constexpr unsigned minLengthForRopeWalk = 0x128;

    static constexpr uintptr_t isRopeInPointer = 0x1;

    static constexpr unsigned maxLengthForOnStackResolve = 2048;

    template<typename CharacterType>
    inline void resolveToBuffer(std::span<CharacterType>);

private:
    String& uninitializedValueInternal() const
    {
        return *std::bit_cast<String*>(&m_fiber);
    }

    String& valueInternal() const
    {
        ASSERT(!isRope());
        return uninitializedValueInternal();
    }

    static constexpr TypeInfo defaultTypeInfo() { return TypeInfo(StringType, StructureFlags); }
    static constexpr int32_t defaultTypeInfoBlob()
    {
        return TypeInfoBlob::typeInfoBlob(NonArray, defaultTypeInfo().type(), defaultTypeInfo().inlineTypeFlags());
    }

    JSString(VM& vm, Ref<StringImpl>&& value)
        : JSCell(CreatingWellDefinedBuiltinCell, vm.stringStructure.get()->id(), defaultTypeInfoBlob())
    {
        // TSAN family rope-stringimpl (OM ground truth: shared cells are racy
        // with re-dispatch): a recycled cell can still be probed by stale
        // lock-free readers (concurrent compilers / GC) through
        // fiberConcurrently() while this constructor initializes it, so the
        // init store must be an annotated relaxed atomic store. Same one
        // pointer-sized store and ownership transfer as the placement-new
        // String construction it replaces (String holds exactly one
        // RefPtr<StringImpl>); identical codegen on every supported target.
        static_assert(sizeof(String) == sizeof(StringImpl*));
#if TSAN_ENABLED
        // TSAN (r11 reports 17/18/20/21): release so a sibling Thread's
        // TSAN-gated fiberConcurrently() acquire synchronizes with this
        // publication and the fresh impl's contents (allocated just before on
        // this thread) become visible to TSAN's happens-before model.
        // Production stays relaxed: ownership transfer + the address
        // dependency into the immutable impl order the contents on every
        // supported target (see the fiberConcurrently protocol comment).
        WTF::atomicStore(&m_fiber, std::bit_cast<uintptr_t>(&value.leakRef()), std::memory_order_release);
#else
        WTF::atomicStore(&m_fiber, std::bit_cast<uintptr_t>(&value.leakRef()), std::memory_order_relaxed);
#endif
    }

    JSString(VM& vm)
        : JSCell(CreatingWellDefinedBuiltinCell, vm.stringStructure.get()->id(), defaultTypeInfoBlob())
    {
        // See above: relaxed atomic init store for stale lock-free readers of
        // a recycled cell.
        WTF::atomicStore(&m_fiber, isRopeInPointer, std::memory_order_relaxed);
    }

    void finishCreation(VM& vm, unsigned length)
    {
        ASSERT_UNUSED(length, length > 0);
        ASSERT(!valueInternal().isNull());
        Base::finishCreation(vm);
    }

    void finishCreation(VM& vm, unsigned length, size_t cost)
    {
        ASSERT_UNUSED(length, length > 0);
        ASSERT(!valueInternal().isNull());
        Base::finishCreation(vm);
        vm.heap.reportExtraMemoryAllocated(this, cost);
    }

    void finishCreation(VM& vm, GCDeferralContext* deferralContext, unsigned length, size_t cost)
    {
        ASSERT_UNUSED(length, length > 0);
        ASSERT(!valueInternal().isNull());
        Base::finishCreation(vm);
        vm.heap.reportExtraMemoryAllocated(deferralContext, this, cost);
    }

    static JSString* createEmptyString(VM&);

    static JSString* create(VM& vm, Ref<StringImpl>&& value)
    {
        unsigned length = value->length();
        ASSERT(length > 0);
        size_t cost = value->cost();
        JSString* newString = new (NotNull, allocateCell<JSString>(vm)) JSString(vm, WTF::move(value));
        newString->finishCreation(vm, length, cost);
        return newString;
    }

    static JSString* create(VM& vm, GCDeferralContext* deferralContext, Ref<StringImpl>&& value)
    {
        unsigned length = value->length();
        ASSERT(length > 0);
        size_t cost = value->cost();
        JSString* newString = new (NotNull, allocateCell<JSString>(vm, deferralContext)) JSString(vm, WTF::move(value));
        newString->finishCreation(vm, deferralContext, length, cost);
        return newString;
    }
    static JSString* createHasOtherOwner(VM& vm, Ref<StringImpl>&& value)
    {
        unsigned length = value->length();
        JSString* newString = new (NotNull, allocateCell<JSString>(vm)) JSString(vm, WTF::move(value));
        newString->finishCreation(vm, length);
        return newString;
    }

protected:
    DECLARE_DEFAULT_FINISH_CREATION;

public:
    Identifier toIdentifier(JSGlobalObject*) const;
    GCOwnedDataScope<AtomStringImpl*> toAtomString(JSGlobalObject*) const;
    GCOwnedDataScope<AtomStringImpl*> toExistingAtomString(JSGlobalObject*) const;

    GCOwnedDataScope<StringView> view(JSGlobalObject*) const;

    ALWAYS_INLINE bool equalInline(JSGlobalObject*, JSString* other) const;
    inline bool equal(JSGlobalObject*, JSString* other) const;
    GCOwnedDataScope<const String&> value(JSGlobalObject*) const;
    inline GCOwnedDataScope<const String&> tryGetValue(bool allocationAllowed = true) const;
    GCOwnedDataScope<const String&> tryGetValueWithoutGC() const;
    StringImpl* getValueImpl() const;
    StringImpl* tryGetValueImpl() const;
    ALWAYS_INLINE unsigned length() const;

    JSValue NODELETE toPrimitive(JSGlobalObject*, PreferredPrimitiveType) const;
    bool toBoolean() const { return !!length(); }
    JSObject* toObject(JSGlobalObject*) const;
    double toNumber(JSGlobalObject*) const;

    bool getStringPropertySlot(JSGlobalObject*, PropertyName, PropertySlot&);
    bool getStringPropertySlot(JSGlobalObject*, unsigned propertyName, PropertySlot&);
    bool getStringPropertyDescriptor(JSGlobalObject*, PropertyName, PropertyDescriptor&);

    bool canGetIndex(unsigned i) { return i < length(); }
    JSString* getIndex(JSGlobalObject*, unsigned);

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    static constexpr ptrdiff_t offsetOfValue() { return OBJECT_OFFSETOF(JSString, m_fiber); }

    DECLARE_EXPORT_INFO;

    static void dumpToStream(const JSCell*, PrintStream&);
    static size_t estimatedSize(JSCell*, VM&);
    DECLARE_VISIT_CHILDREN;

    ALWAYS_INLINE bool isRope() const
    {
        return fiberConcurrently() & isRopeInPointer;
    }
    ALWAYS_INLINE JSRopeString* asRope()
    {
        ASSERT(isRope());
        return uncheckedDowncast<JSRopeString>(this);
    }

    ALWAYS_INLINE bool isNonSubstringRope() const
    {
        return isRope() && !isSubstring();
    }

    bool is8Bit() const;

#if USE(BUN_JSC_ADDITIONS)
    inline void value(jsstring_iterator* iterator) const;
#endif

    ALWAYS_INLINE JSString* tryReplaceOneChar(JSGlobalObject*, char16_t, JSString* replacement);
    inline std::optional<size_t> tryFindOneChar(JSGlobalObject*, char16_t character, unsigned& startPosition) const;
    inline std::optional<size_t> tryFindLastOneChar(JSGlobalObject*, char16_t character, unsigned& startPosition) const;
    ALWAYS_INLINE std::optional<char16_t> tryGetCharAt(JSGlobalObject*, unsigned index) const;

    bool isSubstring() const;
protected:
    friend class JSValue;
    friend class JSCell;

    JS_EXPORT_PRIVATE bool equalSlowCase(JSGlobalObject*, JSString* other) const;

    inline JSString* tryReplaceOneCharImpl(JSGlobalObject*, char16_t search, JSString* replacement, uint8_t* stackLimit, bool& found);

    // UNGIL V7 (read side): m_fiber can be republished by a concurrent
    // JSRopeString::convertToNonRope (JSStringInlines.h) or
    // JSString::swapToAtomString while lock-free readers snapshot it, so the
    // read must be an annotated relaxed atomic load (same codegen as the
    // plain load on every supported target). Write sides now atomic in this
    // header: constructors + JSRopeString initialize* are relaxed stores
    // (recycled-cell stale readers), swapToAtomString is a release-ordered
    // atomicExchange (contents of the published impl happen-before any
    // acquire reader; relaxed readers rely on the address-dependency into the
    // immutable impl, per the OM ground-truth re-dispatch rule).
    // The publish in convertToNonRope (JSStringInlines.h) is the release
    // companion: a release-ordered atomicStore of the resolved impl pointer
    // (TSAN-TRIAGE §11.17), mirroring swapToAtomString.
    uintptr_t fiberConcurrently() const
    {
#if TSAN_ENABLED
        // TSAN-gated acquire (recorded in TSAN-TRIAGE; §13.4-style gate): the
        // production protocol relies on address-dependency ordering from this
        // relaxed load to the resolved StringImpl's contents (convertToNonRope /
        // swapToAtomString publish with a release store). TSAN cannot model
        // dependency ordering, so without an acquire here every content read of
        // a concurrently-resolved rope is reported against the resolver's
        // copyElements. Acquire is a plain load on x86-64; production stays
        // relaxed (dependency ordering is sufficient on all supported targets).
        return WTF::atomicLoad(&m_fiber, std::memory_order_acquire);
#else
        return WTF::atomicLoad(&m_fiber, std::memory_order_relaxed);
#endif
    }

    mutable uintptr_t m_fiber;

private:
    friend class LLIntOffsetsExtractor;

    void swapToAtomString(VM&, RefPtr<AtomStringImpl>&&) const;

    friend JSString* jsString(VM&, const String&);
    friend JSString* jsString(VM&, String&&);
    friend JSString* jsString(VM&, StringView);
    friend JSString* jsString(JSGlobalObject*, JSString*, JSString*);
    friend JSString* jsString(JSGlobalObject*, const String&, JSString*);
    friend JSString* jsString(JSGlobalObject*, JSString*, const String&);
    friend JSString* jsString(JSGlobalObject*, const String&, const String&);
    friend JSString* jsString(JSGlobalObject*, JSString*, JSString*, JSString*);
    friend JSString* jsString(JSGlobalObject*, const String&, const String&, const String&);
    friend JS_EXPORT_PRIVATE JSString* jsStringWithCacheSlowCase(VM&, StringImpl&);
    friend JSString* jsSingleCharacterString(VM&, char16_t);
    friend JSString* jsSingleCharacterString(VM&, Latin1Character);
    friend JSString* jsNontrivialString(VM&, const String&);
    friend JSString* jsNontrivialString(VM&, String&&);
    friend JSString* jsSubstring(VM&, const String&, unsigned, unsigned);
    friend JSString* jsSubstring(JSGlobalObject*, VM&, JSString*, unsigned, unsigned);
    friend JSString* tryJSSubstringImpl(VM&, JSString*, unsigned, unsigned);
    friend JSString* jsSubstringOfResolved(VM&, GCDeferralContext*, JSString*, unsigned, unsigned);
    friend JSString* jsOwnedString(VM&, const String&);
    friend JSString* jsAtomString(JSGlobalObject*, VM&, JSString*);
    friend JSString* jsAtomString(JSGlobalObject*, VM&, JSString*, JSString*);
    friend JSString* jsAtomString(JSGlobalObject*, VM&, JSString*, JSString*, JSString*);
};

// NOTE: This class cannot override JSString's destructor. JSString's destructor is called directly
// from JSStringSubspace::
class JSRopeString final : public JSString {
    friend class JSString;
    friend class RegExpObject;
    friend class RegExpSubstringGlobalAtomCache;
public:
    static constexpr DestructionMode needsDestruction = MayNeedDestruction;
    static constexpr uint8_t numberOfLowerTierPreciseCells = 0;
    static void destroy(JSCell*);

    template<typename, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.ropeStringSpace();
    }

    // We use lower 3bits of fiber0 for flags. These bits are usable due to alignment, and it is OK even in 32bit architecture.
    static constexpr uintptr_t is8BitInPointer = static_cast<uintptr_t>(StringImpl::flagIs8Bit());
    static constexpr uintptr_t isSubstringInPointer = 0x2;
    static_assert(is8BitInPointer == 0b100);
    static_assert(isSubstringInPointer == 0b010);
    static_assert(isRopeInPointer == 0b001);
    static constexpr uintptr_t stringMask = ~(isRopeInPointer | is8BitInPointer | isSubstringInPointer);
#if CPU(ADDRESS64)
    static_assert(sizeof(uintptr_t) == sizeof(uint64_t));
    class CompactFibers {
    public:
        static constexpr uintptr_t addressMask = (1ULL << OS_CONSTANT(EFFECTIVE_ADDRESS_WIDTH)) - 1;
        // TSAN family rope-stringimpl (OM ground truth: shared cells are racy
        // with re-dispatch): the split fiber/length words are written only at
        // rope creation, but recycled cells can still be probed by stale
        // lock-free readers (concurrent compilers / GC) while a new rope's
        // constructor initializes them, so every C++ access must be an
        // annotated relaxed atomic. Wave-5 review amendment: the LITTLE_ENDIAN
        // single unaligned wide load is KEPT for non-TSAN builds — replacing
        // it unconditionally with two narrow relaxed loads was a flag-off
        // codegen change on hot rope paths (resolveRope*, iterRope*, GC
        // visitChildren), which the campaign rules forbid. The value race on
        // these words is exactly the blessed §5.7/OM-ground-truth class the
        // wide load always had. Under TSAN_ENABLED ONLY (unaligned atomics do
        // not exist, so the wide load cannot be annotated), readers compose
        // per-field relaxed loads instead — same values, TSAN-visible — the
        // precedent shape of Structure.h tsanRelaxedLoad. This TSAN-only
        // divergence is recorded in docs/threads/TSAN-TRIAGE.md (§13).
        // PRECONDITION (load-bearing): under TSAN the two relaxed loads are
        // independent, so a reader racing initializeFiber* can assemble a TORN
        // pointer that no thread ever wrote. This is sound only under the OM
        // ground-truth re-dispatch rule: no concurrent consumer may
        // dereference a fiber value without first re-validating the cell.
        // fiber1()/fiber2() values are NOT dereference-safe in concurrent
        // code — the unaligned wide load relies on the same rule.
        JSString* fiber1() const
        {
#if CPU(LITTLE_ENDIAN) && !TSAN_ENABLED
            return std::bit_cast<JSString*>(WTF::unalignedLoad<uintptr_t>(&m_fiber1Lower) & addressMask);
#else
            return std::bit_cast<JSString*>(static_cast<uintptr_t>(WTF::atomicLoad(&m_fiber1Lower, std::memory_order_relaxed)) | (static_cast<uintptr_t>(WTF::atomicLoad(&m_fiber1Upper, std::memory_order_relaxed)) << 32));
#endif
        }

        void initializeFiber1(JSString* fiber)
        {
            uintptr_t pointer = std::bit_cast<uintptr_t>(fiber);
            WTF::atomicStore(&m_fiber1Lower, static_cast<uint32_t>(pointer), std::memory_order_relaxed);
            WTF::atomicStore(&m_fiber1Upper, static_cast<uint16_t>(pointer >> 32), std::memory_order_relaxed);
        }

        JSString* fiber2() const
        {
#if CPU(LITTLE_ENDIAN) && !TSAN_ENABLED
            return std::bit_cast<JSString*>(WTF::unalignedLoad<uintptr_t>(&m_fiber1Upper) >> 16);
#else
            return std::bit_cast<JSString*>(static_cast<uintptr_t>(WTF::atomicLoad(&m_fiber2Lower, std::memory_order_relaxed)) | (static_cast<uintptr_t>(WTF::atomicLoad(&m_fiber2Upper, std::memory_order_relaxed)) << 16));
#endif
        }
        void initializeFiber2(JSString* fiber)
        {
            uintptr_t pointer = std::bit_cast<uintptr_t>(fiber);
            WTF::atomicStore(&m_fiber2Lower, static_cast<uint16_t>(pointer), std::memory_order_relaxed);
            WTF::atomicStore(&m_fiber2Upper, static_cast<uint32_t>(pointer >> 16), std::memory_order_relaxed);
        }

        unsigned length() const { return WTF::atomicLoad(&m_length, std::memory_order_relaxed); }
        void initializeLength(unsigned length)
        {
            WTF::atomicStore(&m_length, length, std::memory_order_relaxed);
        }

        static constexpr ptrdiff_t offsetOfLength() { return OBJECT_OFFSETOF(CompactFibers, m_length); }
        static constexpr ptrdiff_t offsetOfFiber1() { return OBJECT_OFFSETOF(CompactFibers, m_length); }
        static constexpr ptrdiff_t offsetOfFiber2() { return OBJECT_OFFSETOF(CompactFibers, m_fiber1Upper); }
        static constexpr ptrdiff_t offsetOfFiber1Lower() { return OBJECT_OFFSETOF(CompactFibers, m_fiber1Lower); }
        static constexpr ptrdiff_t offsetOfFiber2Lower() { return OBJECT_OFFSETOF(CompactFibers, m_fiber2Lower); }

    private:
        friend class LLIntOffsetsExtractor;

        // mutable so the const readers above can take non-const addresses for
        // WTF::atomicLoad (same pattern as JSString::m_fiber).
        mutable uint32_t m_length { 0 };
        mutable uint32_t m_fiber1Lower { 0 };
        mutable uint16_t m_fiber1Upper { 0 };
        mutable uint16_t m_fiber2Lower { 0 };
        mutable uint32_t m_fiber2Upper { 0 };
    };
    static_assert(sizeof(CompactFibers) == sizeof(void*) * 2);
#else
    class CompactFibers {
    public:
        // TSAN family rope-stringimpl: relaxed atomic accesses for stale
        // lock-free readers of recycled cells — see the ADDRESS64 variant.
        JSString* fiber1() const
        {
            return WTF::atomicLoad(&m_fiber1, std::memory_order_relaxed);
        }
        void initializeFiber1(JSString* fiber)
        {
            WTF::atomicStore(&m_fiber1, fiber, std::memory_order_relaxed);
        }

        JSString* fiber2() const
        {
            return WTF::atomicLoad(&m_fiber2, std::memory_order_relaxed);
        }
        void initializeFiber2(JSString* fiber)
        {
            WTF::atomicStore(&m_fiber2, fiber, std::memory_order_relaxed);
        }

        unsigned length() const { return WTF::atomicLoad(&m_length, std::memory_order_relaxed); }
        void initializeLength(unsigned length)
        {
            WTF::atomicStore(&m_length, length, std::memory_order_relaxed);
        }

        static constexpr ptrdiff_t offsetOfLength() { return OBJECT_OFFSETOF(CompactFibers, m_length); }
        static constexpr ptrdiff_t offsetOfFiber1() { return OBJECT_OFFSETOF(CompactFibers, m_fiber1); }
        static constexpr ptrdiff_t offsetOfFiber2() { return OBJECT_OFFSETOF(CompactFibers, m_fiber2); }

    private:
        friend class LLIntOffsetsExtractor;

        // mutable so the const readers above can take non-const addresses for
        // WTF::atomicLoad (same pattern as JSString::m_fiber).
        mutable uint32_t m_length { 0 };
        mutable JSString* m_fiber1 { nullptr };
        mutable JSString* m_fiber2 { nullptr };
    };
#endif

    template <class OverflowHandler = CrashOnOverflow>
    class RopeBuilder : public OverflowHandler {
        WTF_FORBID_HEAP_ALLOCATION;
    public:
        RopeBuilder(VM& vm)
            : m_vm(vm)
        {
        }

        bool append(JSString* jsString)
        {
            if (this->hasOverflowed()) [[unlikely]]
                return false;
            if (!jsString->length())
                return true;
            if (m_strings.size() == JSRopeString::s_maxInternalRopeLength)
                expand();

            static_assert(JSString::MaxLength == std::numeric_limits<int32_t>::max());
            auto sum = checkedSum<int32_t>(m_length, jsString->length());
            if (sum.hasOverflowed()) {
                this->overflowed();
                return false;
            }
            ASSERT(static_cast<unsigned>(sum) <= MaxLength);
            m_strings.append(jsString);
            m_length = static_cast<unsigned>(sum);
            return true;
        }

        JSString* release()
        {
            RELEASE_ASSERT(!this->hasOverflowed());
            JSString* result = nullptr;
            switch (m_strings.size()) {
            case 0: {
                ASSERT(!m_length);
                result = jsEmptyString(m_vm);
                break;
            }
            case 1: {
                result = asString(m_strings.at(0));
                break;
            }
            case 2: {
                result = JSRopeString::create(m_vm, asString(m_strings.at(0)), asString(m_strings.at(1)));
                break;
            }
            case 3: {
                result = JSRopeString::create(m_vm, asString(m_strings.at(0)), asString(m_strings.at(1)), asString(m_strings.at(2)));
                break;
            }
            default:
                ASSERT_NOT_REACHED();
                break;
            }
            ASSERT(result->length() == m_length);
            m_strings.clear();
            m_length = 0;
            return result;
        }

        unsigned length() const
        {
            ASSERT(!this->hasOverflowed());
            return m_length;
        }

    private:
        void expand();

        VM& m_vm;
        MarkedArgumentBuffer m_strings;
        unsigned m_length { 0 };
    };

    inline unsigned length() const
    {
        return m_compactFibers.length();
    }

private:
    friend class LLIntOffsetsExtractor;

    void convertToNonRope(String&&) const;

    // TSAN family rope-stringimpl: these initializers run only inside the
    // constructor of a freshly allocated (possibly recycled) cell — a single
    // writer — but stale lock-free readers (concurrent compilers / GC) can
    // still probe the recycled cell concurrently (OM ground truth: shared
    // cells are racy with re-dispatch), so the writes must be annotated
    // relaxed atomic stores. No atomic RMW is needed: there is exactly one
    // writer during construction, so a relaxed load + relaxed store of the
    // recombined word is sufficient and keeps the plain-store codegen.
    void initializeIs8Bit(bool flag) const
    {
        uintptr_t fiber = WTF::atomicLoad(&m_fiber, std::memory_order_relaxed);
        if (flag)
            fiber |= is8BitInPointer;
        else
            fiber &= ~is8BitInPointer;
        WTF::atomicStore(&m_fiber, fiber, std::memory_order_relaxed);
    }

    void initializeIsSubstring(bool flag) const
    {
        uintptr_t fiber = WTF::atomicLoad(&m_fiber, std::memory_order_relaxed);
        if (flag)
            fiber |= isSubstringInPointer;
        else
            fiber &= ~isSubstringInPointer;
        WTF::atomicStore(&m_fiber, fiber, std::memory_order_relaxed);
    }

    ALWAYS_INLINE void initializeLength(unsigned length)
    {
        ASSERT(length <= MaxLength);
        m_compactFibers.initializeLength(length);
    }

    JSRopeString(VM& vm)
        : JSString(vm)
    {
        initializeIsSubstring(false);
        initializeLength(0);
        initializeIs8Bit(true);
        initializeFiber0(nullptr);
        initializeFiber1(nullptr);
        initializeFiber2(nullptr);
    }

    JSRopeString(VM& vm, unsigned length, bool is8Bit, JSString* s1, JSString* s2)
        : JSString(vm)
    {
        ASSERT(!sumOverflows<int32_t>(s1->length(), s2->length()));
        initializeIsSubstring(false);
        initializeLength(length);
        initializeIs8Bit(is8Bit);
        initializeFiber0(s1);
        initializeFiber1(s2);
        initializeFiber2(nullptr);
        ASSERT((s1->length() + s2->length()) == this->length());
    }

    JSRopeString(VM& vm, unsigned length, bool is8Bit, JSString* s1, JSString* s2, JSString* s3)
        : JSString(vm)
    {
        ASSERT(!sumOverflows<int32_t>(s1->length(), s2->length(), s3->length()));
        initializeIsSubstring(false);
        initializeLength(length);
        initializeIs8Bit(is8Bit);
        initializeFiber0(s1);
        initializeFiber1(s2);
        initializeFiber2(s3);
        ASSERT((s1->length() + s2->length() + s3->length()) == this->length());
    }

    JSRopeString(VM& vm, unsigned length, bool is8Bit, JSString* base, unsigned offset)
        : JSString(vm)
    {
        ASSERT(!sumOverflows<int32_t>(offset, length));
        ASSERT(offset + length <= base->length());
        initializeIsSubstring(true);
        initializeLength(length);
        initializeIs8Bit(is8Bit);
        initializeSubstringBase(base);
        initializeSubstringOffset(offset);
        ASSERT(length == this->length());
        ASSERT(!base->isRope());
    }

    ALWAYS_INLINE void finishCreationSubstringOfResolved(VM& vm)
    {
        Base::finishCreation(vm);
    }

public:
    static constexpr ptrdiff_t offsetOfLength() { return OBJECT_OFFSETOF(JSRopeString, m_compactFibers) + CompactFibers::offsetOfLength(); } // 32byte width.
    static constexpr ptrdiff_t offsetOfFlags() { return offsetOfValue(); }
    static constexpr ptrdiff_t offsetOfFiber0() { return offsetOfValue(); }
    static constexpr ptrdiff_t offsetOfFiber1() { return OBJECT_OFFSETOF(JSRopeString, m_compactFibers) + CompactFibers::offsetOfFiber1(); }
    static constexpr ptrdiff_t offsetOfFiber2() { return OBJECT_OFFSETOF(JSRopeString, m_compactFibers) + CompactFibers::offsetOfFiber2(); }
#if CPU(ADDRESS64)
    static constexpr ptrdiff_t offsetOfFiber1Lower() { return OBJECT_OFFSETOF(JSRopeString, m_compactFibers) + CompactFibers::offsetOfFiber1Lower(); }
    static constexpr ptrdiff_t offsetOfFiber2Lower() { return OBJECT_OFFSETOF(JSRopeString, m_compactFibers) + CompactFibers::offsetOfFiber2Lower(); }
#endif

    static constexpr unsigned s_maxInternalRopeLength = 3;

    // If nullOrExecForOOM is null, resolveRope() will be do nothing in the event of an OOM error.
    // The rope value will remain a null string in that case.
    JS_EXPORT_PRIVATE const String& resolveRope(JSGlobalObject* nullOrGlobalObjectForOOM) const;
    JS_EXPORT_PRIVATE const String& resolveRopeWithoutGC() const;

    template<typename CharacterType>
    static void resolveToBuffer(JSString*, JSString*, JSString*, std::span<CharacterType> buffer, uint8_t* stackLimit);

private:
    template<typename CharacterType>
    static void resolveToBufferSlow(JSString*, JSString*, JSString*, std::span<CharacterType> buffer, uint8_t* stackLimit);

    static JSRopeString* create(VM& vm, JSString* s1, JSString* s2)
    {
        unsigned length = s1->length() + s2->length();
        bool is8Bit = !!(static_cast<unsigned>(!!s1->is8Bit()) & static_cast<unsigned>(!!s2->is8Bit()));
        JSRopeString* newString = new (NotNull, allocateCell<JSRopeString>(vm)) JSRopeString(vm, length, is8Bit, s1, s2);
        newString->finishCreation(vm);
        ASSERT(newString->length());
        ASSERT(newString->isRope());
        return newString;
    }
    static JSRopeString* create(VM& vm, JSString* s1, JSString* s2, JSString* s3)
    {
        unsigned length = s1->length() + s2->length() + s3->length();
        bool is8Bit = !!(static_cast<unsigned>(!!s1->is8Bit()) & static_cast<unsigned>(!!s2->is8Bit()) & static_cast<unsigned>(!!s3->is8Bit()));
        JSRopeString* newString = new (NotNull, allocateCell<JSRopeString>(vm)) JSRopeString(vm, length, is8Bit, s1, s2, s3);
        newString->finishCreation(vm);
        ASSERT(newString->length());
        ASSERT(newString->isRope());
        return newString;
    }

    ALWAYS_INLINE static JSRopeString* createSubstringOfResolved(VM& vm, GCDeferralContext* deferralContext, JSString* base, unsigned offset, unsigned length, bool is8Bit)
    {
        JSRopeString* newString = new (NotNull, allocateCell<JSRopeString>(vm, deferralContext)) JSRopeString(vm, length, is8Bit, base, offset);
        newString->finishCreationSubstringOfResolved(vm);
        ASSERT(newString->length());
        ASSERT(newString->isRope());
        return newString;
    }

    friend JSValue jsStringFromRegisterArray(JSGlobalObject*, Register*, unsigned);

    template<bool reportAllocation, typename Function> const String& resolveRopeWithFunction(JSGlobalObject* nullOrGlobalObjectForOOM, Function&&) const;
    JS_EXPORT_PRIVATE GCOwnedDataScope<AtomStringImpl*> resolveRopeToAtomString(JSGlobalObject*) const;
    JS_EXPORT_PRIVATE GCOwnedDataScope<AtomStringImpl*> resolveRopeToExistingAtomString(JSGlobalObject*) const;
    template<typename CharacterType> void resolveRopeInternalNoSubstring(std::span<CharacterType>, uint8_t* stackLimit) const;
    Identifier toIdentifier(JSGlobalObject*) const;
    void outOfMemory(JSGlobalObject* nullOrGlobalObjectForOOM) const;
    GCOwnedDataScope<StringView> view(JSGlobalObject*) const;

    JSString* fiber0() const
    {
        return std::bit_cast<JSString*>(fiberConcurrently() & stringMask);
    }

    JSString* fiber1() const
    {
        return m_compactFibers.fiber1();
    }

    JSString* fiber2() const
    {
        return m_compactFibers.fiber2();
    }

    JSString* fiber(unsigned i) const
    {
        ASSERT(!isSubstring());
        ASSERT(i < s_maxInternalRopeLength);
        switch (i) {
        case 0:
            return fiber0();
        case 1:
            return fiber1();
        case 2:
            return fiber2();
        }
        ASSERT_NOT_REACHED();
        return nullptr;
    }

    void initializeFiber0(JSString* fiber)
    {
        // TSAN family rope-stringimpl: constructor-only single-writer store
        // racing stale lock-free readers of a recycled cell; relaxed atomic
        // load + store (see initializeIs8Bit).
        uintptr_t pointer = std::bit_cast<uintptr_t>(fiber);
        ASSERT(!(pointer & ~stringMask));
        uintptr_t bits = WTF::atomicLoad(&m_fiber, std::memory_order_relaxed);
        WTF::atomicStore(&m_fiber, pointer | (bits & ~stringMask), std::memory_order_relaxed);
    }

    void initializeFiber1(JSString* fiber)
    {
        m_compactFibers.initializeFiber1(fiber);
    }

    void initializeFiber2(JSString* fiber)
    {
        m_compactFibers.initializeFiber2(fiber);
    }

    void initializeSubstringBase(JSString* fiber)
    {
        initializeFiber1(fiber);
    }

    JSString* substringBase() const { return fiber1(); }

    void initializeSubstringOffset(unsigned offset)
    {
        m_compactFibers.initializeFiber2(std::bit_cast<JSString*>(static_cast<uintptr_t>(offset)));
    }

    unsigned substringOffset() const
    {
        return static_cast<unsigned>(std::bit_cast<uintptr_t>(fiber2()));
    }

    static_assert(s_maxInternalRopeLength >= 2);
    mutable CompactFibers m_compactFibers;

    friend JSString* jsString(JSGlobalObject*, JSString*, JSString*);
    friend JSString* jsString(JSGlobalObject*, const String&, JSString*);
    friend JSString* jsString(JSGlobalObject*, JSString*, const String&);
    friend JSString* jsString(JSGlobalObject*, const String&, const String&);
    friend JSString* jsString(JSGlobalObject*, JSString*, JSString*, JSString*);
    friend JSString* jsString(JSGlobalObject*, const String&, const String&, const String&);
    friend JSString* jsSubstringOfResolved(VM&, GCDeferralContext*, JSString*, unsigned, unsigned);
    friend JSString* jsSubstring(JSGlobalObject*, VM&, JSString*, unsigned, unsigned);

#if USE(BUN_JSC_ADDITIONS)
    JS_EXPORT_PRIVATE void iterRope(jsstring_iterator*) const;
    NEVER_INLINE void iterRopeSlowCase(jsstring_iterator*) const;
    void iterRopeInternalNoSubstring(jsstring_iterator*) const;
#endif

    friend JSString* tryJSSubstringImpl(VM&, JSString*, unsigned, unsigned);
    friend JSString* jsAtomString(JSGlobalObject*, VM&, JSString*);
    friend JSString* jsAtomString(JSGlobalObject*, VM&, JSString*, JSString*);
    friend JSString* jsAtomString(JSGlobalObject*, VM&, JSString*, JSString*, JSString*);
};

template<> void JSRopeString::RopeBuilder<RecordOverflow>::expand();

JS_EXPORT_PRIVATE JSString* jsStringWithCacheSlowCase(VM&, StringImpl&);

// JSString::is8Bit is safe to be called concurrently. Concurrent threads can access is8Bit even if the main thread
// is in the middle of converting JSRopeString to JSString.
ALWAYS_INLINE bool JSString::is8Bit() const
{
    uintptr_t pointer = fiberConcurrently();
    if (pointer & isRopeInPointer) {
        // Do not load m_fiber twice. We should use the information in pointer.
        // Otherwise, JSRopeString may be converted to JSString between the first and second accesses.
        return pointer & JSRopeString::is8BitInPointer;
    }
    return std::bit_cast<StringImpl*>(pointer)->is8Bit();
}

// JSString::length is safe to be called concurrently. Concurrent threads can access length even if the main thread
// is in the middle of converting JSRopeString to JSString. This is OK because we never override the length bits
// when we resolve a JSRopeString.
ALWAYS_INLINE unsigned JSString::length() const
{
    uintptr_t pointer = fiberConcurrently();
    if (pointer & isRopeInPointer) {
        // GIL-off (SPEC-ungil §N.2 reader arm): the rope decision comes from
        // the ONE snapshot above. uncheckedDowncast must not be used here:
        // its debug is<JSRopeString> check re-reads m_fiber and can observe a
        // concurrent resolver's convertToNonRope republish (TypeCasts.h
        // assert). static_cast on the snapshot decision is correct in both
        // modes: the cell's dynamic type is immutable, and rope length bits
        // (m_compactFibers) are never overwritten by resolution.
        return static_cast<const JSRopeString*>(this)->length();
    }
    return std::bit_cast<StringImpl*>(pointer)->length();
}

inline StringImpl* JSString::getValueImpl() const
{
    ASSERT(!isRope());
    return std::bit_cast<StringImpl*>(fiberConcurrently());
}

inline StringImpl* JSString::tryGetValueImpl() const
{
    uintptr_t pointer = fiberConcurrently();
    if (pointer & isRopeInPointer)
        return nullptr;
    return std::bit_cast<StringImpl*>(pointer);
}

inline JSString* asString(JSValue value)
{
    ASSERT(value.isStringSlow());
    return uncheckedDowncast<JSString>(value.asCell());
}

// This MUST NOT GC.
inline JSString* jsEmptyString(VM& vm)
{
    return vm.smallStrings.emptyString();
}

ALWAYS_INLINE JSString* jsSingleCharacterString(VM& vm, char16_t c)
{
    if constexpr (validateDFGDoesGC)
        vm.verifyCanGC();
    if (c <= maxSingleCharacterString)
        return vm.smallStrings.singleCharacterString(c);
    return JSString::create(vm, StringImpl::create(std::span { &c, 1 }));
}

ALWAYS_INLINE JSString* jsSingleCharacterString(VM& vm, Latin1Character c)
{
    if constexpr (validateDFGDoesGC)
        vm.verifyCanGC();
    ASSERT(maxSingleCharacterString >= 0xff);
    return vm.smallStrings.singleCharacterString(c);
}

inline JSString* jsNontrivialString(VM& vm, const String& s)
{
    ASSERT(s.length() > 1);
    return JSString::create(vm, *s.impl());
}

inline JSString* jsNontrivialString(VM& vm, String&& s)
{
    ASSERT(s.length() > 1);
    return JSString::create(vm, s.releaseImpl().releaseNonNull());
}

ALWAYS_INLINE Identifier JSRopeString::toIdentifier(JSGlobalObject* globalObject) const
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto atomString = static_cast<const JSRopeString*>(this)->resolveRopeToAtomString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return Identifier::fromString(vm, Ref { *atomString });
}

ALWAYS_INLINE void JSString::swapToAtomString(VM& vm, RefPtr<AtomStringImpl>&& atom) const
{
    // We replace currently held string with new AtomString. But the old string can be accessed from concurrent compilers and GC threads at any time.
    // So, we keep the old string alive by appending it to Heap::m_possiblyAccessedStringsFromConcurrentThreads. And GC clears that list when GC finishes.
    // This is OK since (1) when finishing GC concurrent compiler threads and GC threads are stopped, and (2) AtomString is already held in the atom table,
    // and we anyway keep this old string until this JSString* is GC-ed. So it does not increase any memory pressure, we release at the same timing.
    //
    // TSAN family rope-stringimpl: this republish of m_fiber races lock-free
    // readers (fiberConcurrently), so it must be an annotated atomic store.
    // The release ordering replaces (and subsumes) the storeStoreFence the
    // plain String::swap publication used: the atom impl's contents
    // happen-before any reader that observes the new bits.
    ASSERT(!isCompilationThread() && !Thread::mayBeGCThread());
    RefPtr<StringImpl> newImpl = WTF::move(atom);
    uintptr_t newBits = std::bit_cast<uintptr_t>(newImpl.leakRef());
    uintptr_t oldBits;
    if (vm.gilOff()) [[unlikely]] {
        // GIL-off: N mutators can race to atomize the same cell. A pair of
        // plain read-old/store-new swaps would hand BOTH losers the same old
        // impl (double-adopt -> over-deref/UAF) and leak a winner's atom ref.
        // A release atomicExchange transfers ownership of exactly one prior
        // value to exactly one thread regardless of interleaving (every
        // published impl is either still in the cell or owned by exactly one
        // exchanger below).
        oldBits = WTF::atomicExchange(&m_fiber, newBits, std::memory_order_release);
    } else {
        // GIL-on / flag-off: single mutator; same one-load/one-store shape as
        // the String::swap it replaces (release store == plain store + the
        // old storeStoreFence on every supported target).
        oldBits = WTF::atomicLoad(&m_fiber, std::memory_order_relaxed);
        WTF::atomicStore(&m_fiber, newBits, std::memory_order_release);
    }
    ASSERT(!(oldBits & isRopeInPointer));
    String target(adoptRef(std::bit_cast<StringImpl*>(oldBits)));
    vm.heap.appendPossiblyAccessedStringFromConcurrentThreads(WTF::move(target));
}

ALWAYS_INLINE Identifier JSString::toIdentifier(JSGlobalObject* globalObject) const
{
    if constexpr (validateDFGDoesGC)
        getVM(globalObject).verifyCanGC();
    if (isRope())
        return static_cast<const JSRopeString*>(this)->toIdentifier(globalObject);
    VM& vm = getVM(globalObject);
    // TSAN family rope-stringimpl: snapshot the published impl ONCE through
    // the annotated relaxed load (getValueImpl). Each valueInternal().impl()
    // is a PLAIN load of m_fiber that races a concurrent swapToAtomString
    // republish (release atomicExchange) under gilOff. The snapshot is the
    // same single load instruction, so flag-off codegen is unchanged, and
    // single-mutator flag-off semantics are identical (m_fiber cannot change
    // under our feet there).
    StringImpl* impl = getValueImpl();
    if (impl->isAtom())
        return Identifier::fromString(vm, Ref { *static_cast<AtomStringImpl*>(impl) });
    // GIL-off: vm.lastAtomizedIdentifier{String,AtomString}Impl is a
    // SINGLE-MUTATOR memoization pair (two plain Ref members) — N threads
    // racing it interleave the two writes and the read below, returning the
    // WRONG identifier for this string (cross-thread key confusion: property
    // reads/writes land on another thread's last-atomized key), and the
    // unsynchronized Ref assignments race ref/deref (UAF). Bypass the cache:
    // atomize through the shared atom table directly (thread-safe under
    // useSharedAtomStringTable) and keep the swap-publication path.
    // GIL-on / flag-off: branch dead, memoization byte-identical.
    if (vm.gilOff()) [[unlikely]] {
        // Note: re-checking isAtom() on the SNAPSHOT (not a fresh m_fiber
        // read) is sound: AtomStringImpl::add can atomize `impl` in place
        // (flag read is the atomic hashAndFlags), and if another thread
        // already swapped the cell to an atom, our extra swapToAtomString is
        // an idempotent atom-for-atom exchange.
        Ref<AtomStringImpl> atom = AtomStringImpl::add(impl).releaseNonNull();
        if (!impl->isAtom())
            swapToAtomString(vm, RefPtr { atom.ptr() });
        return Identifier::fromString(vm, WTF::move(atom));
    }
    if (vm.lastAtomizedIdentifierStringImpl.ptr() != impl) {
        vm.lastAtomizedIdentifierStringImpl = *impl;
        vm.lastAtomizedIdentifierAtomStringImpl = AtomStringImpl::add(impl).releaseNonNull();
    }
    // It is possible that AtomStringImpl::add converts the existing StringImpl to AtomicStringImpl in place,
    // thus we need to recheck atomicity status here.
    if (!impl->isAtom())
        swapToAtomString(vm, RefPtr { vm.lastAtomizedIdentifierAtomStringImpl.ptr() });
    return Identifier::fromString(vm, Ref { vm.lastAtomizedIdentifierAtomStringImpl });
}

ALWAYS_INLINE GCOwnedDataScope<AtomStringImpl*> JSString::toAtomString(JSGlobalObject* globalObject) const
{
    if constexpr (validateDFGDoesGC)
        getVM(globalObject).verifyCanGC();
    if (isRope())
        return { this, static_cast<const JSRopeString*>(this)->resolveRopeToAtomString(globalObject) };
    // TSAN family rope-stringimpl: snapshot the published impl through the
    // annotated relaxed load (getValueImpl) instead of plain m_fiber reads
    // racing a concurrent swapToAtomString republish (see toIdentifier). The
    // final return re-reads the cell: after our swap the cell holds an atom
    // (ours, or a racing winner's — non-rope cells are only ever republished
    // with atoms), so the cast remains valid either way.
    StringImpl* impl = getValueImpl();
    if (impl->isAtom())
        return { this, static_cast<AtomStringImpl*>(impl) };
    AtomString atom(impl);
    swapToAtomString(getVM(globalObject), atom.releaseImpl());
    return { this, static_cast<AtomStringImpl*>(getValueImpl()) };
}

ALWAYS_INLINE GCOwnedDataScope<AtomStringImpl*> JSString::toExistingAtomString(JSGlobalObject* globalObject) const
{
    if constexpr (validateDFGDoesGC)
        getVM(globalObject).verifyCanGC();
    if (isRope())
        return static_cast<const JSRopeString*>(this)->resolveRopeToExistingAtomString(globalObject);
    // TSAN family rope-stringimpl: annotated relaxed snapshot of the
    // published impl; see toAtomString above.
    StringImpl* impl = getValueImpl();
    if (impl->isAtom())
        return { this, static_cast<AtomStringImpl*>(impl) };
    if (auto atom = AtomStringImpl::lookUp(impl)) {
        swapToAtomString(getVM(globalObject), WTF::move(atom));
        return { this, static_cast<AtomStringImpl*>(getValueImpl()) };
    }
    return { };
}

inline GCOwnedDataScope<const String&> JSString::value(JSGlobalObject* globalObject) const
{
    if constexpr (validateDFGDoesGC)
        getVM(globalObject).verifyCanGC();
    if (isRope())
        return { this, static_cast<const JSRopeString*>(this)->resolveRope(globalObject) };
    return { this, valueInternal() };
}
#if USE(BUN_JSC_ADDITIONS)
inline void JSString::value(jsstring_iterator* iterator) const
{
      if (isRope()) {
          static_cast<const JSRopeString*>(this)->iterRope(iterator);
          return;
      }


    // TSAN family rope-stringimpl: snapshot the published impl through the
    // annotated relaxed load, and derive 8-bit-ness from the SAME snapshot
    // (a second m_fiber load could observe a different impl republished by a
    // concurrent swapToAtomString). Flag-off: identical loads and behavior.
    auto* internal = getValueImpl();
    if (internal->is8Bit()) {
        auto span8 = internal->span8();
        iterator->append8(iterator, (void*)span8.data(), span8.size());
    } else {
        auto span16 = internal->span16();
        iterator->append16(iterator, (void*)span16.data(), span16.size());
    }
}
#endif

inline GCOwnedDataScope<const String&> JSString::tryGetValue(bool allocationAllowed) const
{
    if (allocationAllowed) {
        if (isRope()) {
            // Pass nullptr for the JSGlobalObject so that resolveRope does not throw in the event of an OOM error.
            return { this, static_cast<const JSRopeString*>(this)->resolveRope(nullptr) };
        }
    } else
        RELEASE_ASSERT(!isRope());
    return { this, valueInternal() };
}

inline JSString* JSString::getIndex(JSGlobalObject* globalObject, unsigned i)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(canGetIndex(i));
    auto view = this->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return jsSingleCharacterString(vm, view[i]);
}

inline JSString* jsString(VM& vm, const String& s)
{
    int size = s.length();
    if (!size)
        return vm.smallStrings.emptyString();
    if (size == 1) {
        if (auto c = s.codeUnitAt(0); c <= maxSingleCharacterString)
            return vm.smallStrings.singleCharacterString(c);
    }
    return JSString::create(vm, *s.impl());
}

inline JSString* jsString(VM& vm, String&& s)
{
    int size = s.length();
    if (!size)
        return vm.smallStrings.emptyString();
    if (size == 1) {
        if (auto c = s.codeUnitAt(0); c <= maxSingleCharacterString)
            return vm.smallStrings.singleCharacterString(c);
    }
    return JSString::create(vm, s.releaseImpl().releaseNonNull());
}

ALWAYS_INLINE JSString* jsString(VM& vm, const AtomString& s)
{
    return jsString(vm, s.string());
}

ALWAYS_INLINE JSString* jsString(VM& vm, AtomString&& s)
{
    return jsString(vm, s.releaseString());
}

inline JSString* jsString(VM& vm, StringView s)
{
    int size = s.length();
    if (!size)
        return vm.smallStrings.emptyString();
    if (size == 1) {
        if (auto c = s.codeUnitAt(0); c <= maxSingleCharacterString)
            return vm.smallStrings.singleCharacterString(c);
    }
    auto impl = s.is8Bit() ? StringImpl::create(s.span8()) : StringImpl::create(s.span16());
    return JSString::create(vm, WTF::move(impl));
}

ALWAYS_INLINE JSString* jsString(VM& vm, RefPtr<AtomStringImpl>&& s)
{
    return jsString(vm, String { WTF::move(s) });
}

ALWAYS_INLINE JSString* jsString(VM& vm, Ref<AtomStringImpl>&& s)
{
    return jsString(vm, String { WTF::move(s) });
}

ALWAYS_INLINE JSString* jsString(VM& vm, Ref<StringImpl>&& s)
{
    return jsString(vm, String { WTF::move(s) });
}

inline JSString* tryJSSubstringImpl(VM& vm, JSString* base, unsigned offset, unsigned length)
{
    // Cap traversal depth to avoid O(n^2) slicing on deep ropes (e.g. repeated s += 'A').
    // Exceeding the limit returns nullptr, letting jsSubstring flatten via resolveRope.
    static constexpr unsigned maxTraversalDepth = 8;

    for (unsigned depth = 0; ; ++depth) {
        ASSERT(offset <= base->length());
        ASSERT(length <= base->length());
        ASSERT(offset + length <= base->length());
        if (!length)
            return vm.smallStrings.emptyString();
        if (!offset && length == base->length())
            return base;

        // For now, let's not allow substrings with a rope base.
        // Resolve non-substring rope bases so we don't have to deal with it.
        // FIXME: Evaluate if this would be worth adding more branches.
        // GIL-off (§N.2 reader arm): one m_fiber snapshot decides
        // substring / rope / resolved AND supplies fiber0. Re-reading
        // isRope()/isSubstring()/fiber0() after a concurrent resolver
        // republishes m_fiber would misread the published StringImpl* as
        // flag bits and a JSString* fiber; uncheckedDowncast's debug check
        // re-reads m_fiber the same way. substringBase/substringOffset and
        // fiber1/fiber2 live in m_compactFibers, which resolution never
        // clears, so they remain valid reads under the snapshot decision.
        // GIL-on / flag-off: the snapshot is byte-identical to the direct
        // reads it replaces (single mutator; m_fiber cannot change here).
        uintptr_t fiberBits = base->fiberConcurrently();
        if (fiberBits & JSRopeString::isSubstringInPointer) {
            JSRopeString* baseRope = static_cast<JSRopeString*>(base);
            ASSERT(!baseRope->substringBase()->isRope());
            return jsSubstringOfResolved(vm, nullptr, baseRope->substringBase(), baseRope->substringOffset() + offset, length);
        }

        if (!(fiberBits & JSString::isRopeInPointer))
            return jsSubstringOfResolved(vm, nullptr, base, offset, length);

        if (depth >= maxTraversalDepth)
            return nullptr;

        auto* rope = static_cast<JSRopeString*>(base);
        auto* fiber0 = std::bit_cast<JSString*>(fiberBits & JSRopeString::stringMask);
        ASSERT(fiber0);
        if (offset < fiber0->length()) {
            if ((offset + length) <= fiber0->length()) {
                base = fiber0;
                continue;
            }
            return nullptr; // Crossing multiple fibers.
        }

        unsigned adjustedOffset = offset - fiber0->length();
        auto* fiber1 = rope->fiber1();
        ASSERT(fiber1);
        if (adjustedOffset < fiber1->length()) {
            if ((adjustedOffset + length) <= fiber1->length()) {
                base = fiber1;
                offset = adjustedOffset;
                continue;
            }
            return nullptr; // Crossing multiple fibers.
        }

        adjustedOffset -= fiber1->length();
        auto* fiber2 = rope->fiber2();
        ASSERT(fiber2);
        ASSERT(adjustedOffset < fiber2->length());
        ASSERT((adjustedOffset + length) <= fiber2->length());
        base = fiber2;
        offset = adjustedOffset;
    }
}

inline JSString* jsSubstring(JSGlobalObject* globalObject, VM& vm, JSString* base, unsigned offset, unsigned length)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* result = tryJSSubstringImpl(vm, base, offset, length);
    RETURN_IF_EXCEPTION(scope, nullptr);

    if (!result) {
        // §N.2 reader arm: tryJSSubstringImpl returned nullptr only after
        // observing rope bits, so the cell IS a JSRopeString (dynamic type is
        // immutable). uncheckedDowncast's debug check re-reads m_fiber and
        // races a concurrent resolver; resolveRope itself re-snapshots and
        // treats already-resolved as success.
        static_cast<JSRopeString*>(base)->resolveRope(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return jsSubstringOfResolved(vm, nullptr, base, offset, length);
    }

    return result;
}

inline JSString* jsSubstringOfResolved(VM& vm, JSString* s, unsigned offset, unsigned length)
{
    return jsSubstringOfResolved(vm, nullptr, s, offset, length);
}

inline JSString* jsSubstring(JSGlobalObject* globalObject, JSString* s, unsigned offset, unsigned length)
{
    return jsSubstring(globalObject, getVM(globalObject), s, offset, length);
}

inline JSString* jsSubstring(VM& vm, const String& s, unsigned offset, unsigned length)
{
    ASSERT(offset <= s.length());
    ASSERT(length <= s.length());
    ASSERT(offset + length <= s.length());
    if (!length)
        return vm.smallStrings.emptyString();
    if (length == 1) {
        if (auto c = s.codeUnitAt(offset); c <= maxSingleCharacterString)
            return vm.smallStrings.singleCharacterString(c);
    }
    auto impl = StringImpl::createSubstringSharingImpl(*s.impl(), offset, length);
    if (impl->isSubString())
        return JSString::createHasOtherOwner(vm, WTF::move(impl));
    return JSString::create(vm, WTF::move(impl));
}

inline JSString* jsOwnedString(VM& vm, const String& s)
{
    int size = s.length();
    if (!size)
        return vm.smallStrings.emptyString();
    if (size == 1) {
        if (auto c = s.codeUnitAt(0); c <= maxSingleCharacterString)
            return vm.smallStrings.singleCharacterString(c);
    }
    return JSString::createHasOtherOwner(vm, *s.impl());
}

ALWAYS_INLINE JSString* jsStringWithCache(VM& vm, const String& s)
{
    unsigned length = s.length();
    if (!length)
        return jsEmptyString(vm);

    auto& stringImpl = *s.impl();
    if (length == 1) {
        if (auto c = stringImpl[0]; c <= maxSingleCharacterString)
            return vm.smallStrings.singleCharacterString(c);
    }

    if (auto* lastCachedString = vm.lastCachedString.get()) {
        if (lastCachedString->getValueImpl() == &stringImpl)
            return lastCachedString;
    }

    return jsStringWithCacheSlowCase(vm, stringImpl);
}

ALWAYS_INLINE bool JSString::getStringPropertySlot(JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (propertyName == vm.propertyNames->length) {
        slot.setValue(this, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly, jsNumber(length()));
        return true;
    }

    std::optional<uint32_t> index = parseIndex(propertyName);
    if (index && index.value() < length()) {
        JSValue value = getIndex(globalObject, index.value());
        RETURN_IF_EXCEPTION(scope, false);
        slot.setValue(this, PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly, value);
        return true;
    }

    return false;
}

ALWAYS_INLINE bool JSString::getStringPropertySlot(JSGlobalObject* globalObject, unsigned propertyName, PropertySlot& slot)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (propertyName < length()) {
        JSValue value = getIndex(globalObject, propertyName);
        RETURN_IF_EXCEPTION(scope, false);
        slot.setValue(this, PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly, value);
        return true;
    }

    return false;
}

inline bool isJSString(JSCell* cell)
{
    return cell->type() == StringType;
}

inline bool isJSString(JSValue v)
{
    return v.isCell() && isJSString(v.asCell());
}

ALWAYS_INLINE GCOwnedDataScope<StringView> JSRopeString::view(JSGlobalObject* globalObject) const
{
    if constexpr (validateDFGDoesGC)
        getVM(globalObject).verifyCanGC();
    if (isSubstring()) {
        // TSAN family rope-stringimpl: read the base's published impl through
        // the annotated relaxed load; a plain String read of the base's
        // m_fiber races a concurrent swapToAtomString republish.
        auto* baseImpl = substringBase()->getValueImpl();
        // We return the substring as that's the owner and JSStringJoiner will end up retaining a reference to the underlying string.
        return { substringBase(), StringView { *baseImpl }.substring(substringOffset(), length()) };
    }
    resolveRope(globalObject);
    if (JSString::isRope()) [[unlikely]] // OOM: resolveRope failed; surface an empty view (caller sees the exception).
        return { this, StringView { } };
    // TSAN family rope-stringimpl: snapshot the published impl through the
    // annotated relaxed load (a plain String read of m_fiber races another
    // thread's resolver/atomizer republish).
    return { this, StringView { *getValueImpl() } };
}

ALWAYS_INLINE GCOwnedDataScope<StringView> JSString::view(JSGlobalObject* globalObject) const
{
    if (isRope())
        return static_cast<const JSRopeString&>(*this).view(globalObject);
    // TSAN family rope-stringimpl: build the view from the annotated relaxed
    // impl snapshot rather than a plain String read of m_fiber (which races a
    // concurrent swapToAtomString republish). Same single load flag-off.
    return { this, StringView { *getValueImpl() } };
}

inline bool JSString::isSubstring() const
{
    return fiberConcurrently() & JSRopeString::isSubstringInPointer;
}

} // namespace JSC

SPECIALIZE_TYPE_TRAITS_BEGIN(JSC::JSRopeString)
    static bool isType(const JSC::JSCell& cell)
    {
        auto* string = dynamicDowncast<JSC::JSString>(cell);
        return string && string->isRope();
    }
SPECIALIZE_TYPE_TRAITS_END()

namespace WTF {

template<>
class StringTypeAdapter<JSC::JSString*> {
public:
    StringTypeAdapter(JSC::JSString* string)
        : m_string(string)
    {
    }

    unsigned length() const { return m_string->length(); }
    bool is8Bit() const { return m_string->is8Bit(); }
    template<typename CharacterType>
    void writeTo(std::span<CharacterType> destination) const
    {
        m_string->resolveToBuffer(destination.first(m_string->length()));
    }

private:
    JSC::JSString* m_string { nullptr };
};

} // namespace WTF
