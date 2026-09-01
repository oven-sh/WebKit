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

#include "CallData.h"
#include "CellState.h"
#include "ConstructData.h"
#include "DestructionMode.h"
#include "EnumerationMode.h"
#include "Heap.h"
#include "HeapCell.h"
#include "IndexingType.h"
#include "JSLock.h"
#include "JSTypeInfo.h"
#include "SlotVisitor.h"
#include "SlotVisitorMacros.h"
#include "SubspaceAccess.h"
#include "TypedArrayType.h"
#include "WriteBarrier.h"

namespace JSC {

class CallFrame;
class CompleteSubspace;
class CopyVisitor;
class GCDeferralContext;
class Identifier;
class JSArrayBufferView;
class JSDestructibleObject;
class JSGlobalObject;
class LLIntOffsetsExtractor;
class PropertyDescriptor;
class PropertyName;
class PropertyNameArrayBuilder;
class Structure;
class JSCellLock;

enum class GCDeferralContextArgPresense {
    HasArg,
    DoesNotHaveArg
};

template<typename T> void* allocateCell(VM&, size_t = sizeof(T));
template<typename T> void* tryAllocateCell(VM&, size_t = sizeof(T));
template<typename T> void* allocateCell(VM&, GCDeferralContext*, size_t = sizeof(T));
template<typename T> void* tryAllocateCell(VM&, GCDeferralContext*, size_t = sizeof(T));

// V7 (TSAN, Race C): with shared-memory threads GIL-off, transition protocols
// atomically swap the 16-byte {header, butterfly} cell prefix via
// dcasHeaderAndButterfly while foreign threads plain-load the header bytes
// (m_type / m_indexingTypeAndMisc / m_flags / m_structureID) — atomic on one
// side, plain on the other, the exact asymmetry TSAN reports. These are the
// hottest loads in the engine and run flag-OFF on every property access, so
// the annotation is TSAN-BUILD-ONLY: non-TSAN builds compile to the identical
// plain load (zero codegen delta — the V5b bench rung is unaffected by
// construction). Under TSAN the load is a relaxed atomic, pairing with the
// (instrumented) 128-bit CAS publish. NOTE (review amendment D, recorded
// residual): this assumes clang lowers the __sync 128-bit CAS to
// TSAN-instrumented atomic IR (__tsan_atomic128_*); confirm empirically on
// the rebuilt WebKitBuild/TSan binary — if the publish is invisible to TSAN,
// these pairs exempt the header WORDS but the dcas frame keeps reporting.
//
// Wave-3 review amendment (§9.1): the load helper DELIBERATELY stays
// TSAN-build-only. These are the hottest loads in the engine (every type
// check / structure() / cellState() read); unconditional atomics would be
// the same mov per access but would pin the optimizer (no CSE/merge of
// adjacent header-byte loads) on exactly the paths the V5b bench gate has
// historically punished. The reader side is the OM §3.0/GT#2-blessed half
// of the protocol; the retained production-UB acceptance, the resulting
// permanent TSAN blindness through this helper, and the designated
// alternate coverage (object-model protocol tests + amplifier) are recorded
// in docs/threads/TSAN-TRIAGE.md §9.1.
template<typename T>
ALWAYS_INLINE T cellHeaderConcurrentLoad(const T& field)
{
#if TSAN_ENABLED
    static_assert(std::is_trivially_copyable_v<T>);
    T result;
    __atomic_load(const_cast<T*>(&field), &result, __ATOMIC_RELAXED);
    return result;
#else
    return field;
#endif
}

// V7 (TSAN, cell-header family; wave-3 review amendment §9.1): the
// WRITER-side counterpart of cellHeaderConcurrentLoad. The reader side above
// is blessed by SPEC-objectmodel §3.0/GT#2 (stale header bytes re-dispatch),
// but a relaxed atomic load pairing with a PLAIN store is still a data race
// in the C++ model. Every non-constructor header-byte/word writer
// (setStructure, setStructureIDDirectly, clearStructure, setCellState,
// early-cell init) stores through this helper, which is an UNCONDITIONAL
// relaxed atomic per the campaign convention: a single-word relaxed store is
// the identical mov flag-off (these are cold/warm slow-path writers, not the
// hot allocation ctor — that ctor keeps its member-init-list / TSAN-only
// 64-bit assembly, see JSCellInlines.h and TSAN-TRIAGE §9.1).
template<typename T>
ALWAYS_INLINE void cellHeaderConcurrentStore(T& field, std::type_identity_t<T> value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    __atomic_store(&field, &value, __ATOMIC_RELAXED);
}

#define DECLARE_EXPORT_INFO                                                  \
    protected:                                                               \
        static JS_EXPORT_PRIVATE const ::JSC::ClassInfo s_info;              \
    public:                                                                  \
        static constexpr const ::JSC::ClassInfo* info() { return &s_info; }

#define DECLARE_INFO                                                         \
    protected:                                                               \
        static const ::JSC::ClassInfo s_info;                                \
    public:                                                                  \
        static constexpr const ::JSC::ClassInfo* info() { return &s_info; }

#if ASSERT_ENABLED
#define DECLARE_DEFAULT_FINISH_CREATION \
    ALWAYS_INLINE void finishCreation(JSC::VM& vm) \
    { \
        Base::finishCreation(vm); \
        ASSERT(inheritsSlow(info())); \
    } \
    static constexpr int __unusedFooterAfterDefaultFinishCreation = 0
#else
#define DECLARE_DEFAULT_FINISH_CREATION \
    using Base::finishCreation
#endif

class JSCell : public HeapCell {
    WTF_ALLOW_COMPACT_POINTERS;
    WTF_MAKE_NONCOPYABLE(JSCell);
    WTF_MAKE_NONMOVABLE(JSCell);
    friend class JSValue;
    friend class MarkedBlock;
    template<typename T>
    friend void* tryAllocateCellHelper(Heap&, size_t, GCDeferralContext*, AllocationFailureMode);

public:
    static constexpr unsigned StructureFlags = 0;

    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    static constexpr uint8_t numberOfLowerTierPreciseCells = 8;

    static constexpr size_t atomSize = 16; // This needs to be larger or equal to 16.

    static constexpr bool isResizableOrGrowableSharedTypedArray = false;

    static JSCell* seenMultipleCalleeObjects() { return std::bit_cast<JSCell*>(static_cast<uintptr_t>(1)); }

    enum CreatingEarlyCellTag { CreatingEarlyCell };
    JSCell(CreatingEarlyCellTag);
    enum CreatingWellDefinedBuiltinCellTag { CreatingWellDefinedBuiltinCell };
    JSCell(CreatingWellDefinedBuiltinCellTag, StructureID, uint32_t typeInfoBlob);

    JS_EXPORT_PRIVATE static void destroy(JSCell*);

protected:
    JSCell(VM&, Structure*);

public:
    // Querying the type.
    // V7 (TSAN): all type predicates route through type() so the header-byte
    // load is the single (TSAN-annotated) load; codegen is identical non-TSAN.
    bool isString() const { return type() == StringType; }
    bool isHeapBigInt() const { return type() == HeapBigIntType; }
    bool isSymbol() const { return type() == SymbolType; }
    JS_EXPORT_PRIVATE bool isObjectSlow() const;
    bool isObject() const { return TypeInfo::isObject(type()); }
    bool isGetterSetter() const { return type() == GetterSetterType; }
    bool isCustomGetterSetter() const { return type() == CustomGetterSetterType; }
    bool isProxy() const { JSType t = type(); return t == GlobalProxyType || t == ProxyObjectType; }
    bool isCallable();
    bool isConstructor();
    template<Concurrency> TriState isCallableWithConcurrency();
    template<Concurrency> TriState isConstructorWithConcurrency();
    inline bool inherits(const ClassInfo*) const; // Defined inline in Structure.h
    JS_EXPORT_PRIVATE bool inheritsSlow(const ClassInfo*) const;
    template<typename Target> inline bool inherits() const; // Defined inline in Structure.h
    JS_EXPORT_PRIVATE bool NODELETE isValidCallee() const;
    bool isAPIValueWrapper() const { return type() == APIValueWrapperType; }
    
    // Each cell has a built-in lock. Currently it's simply available for use if you need it. It's
    // a full-blown WTF::Lock. Note that this lock is currently used in JSArray and that lock's
    // ordering with the Structure lock is that the cell lock must be acquired first.

    // We use this abstraction to make it easier to grep for places where we lock cells.
    // to lock a cell you can just do:
    // Locker locker { cell->cellLock() };
    JSCellLock& cellLock() const { return *reinterpret_cast<JSCellLock*>(const_cast<JSCell*>(this)); }
    
    JSType type() const { return cellHeaderConcurrentLoad(m_type); }
    IndexingType indexingTypeAndMisc() const { return cellHeaderConcurrentLoad(m_indexingTypeAndMisc); }
    IndexingType indexingMode() const { return indexingTypeAndMisc() & AllArrayTypes; }
    IndexingType indexingType() const { return indexingTypeAndMisc() & AllWritableArrayTypes; }
    // SPEC-objectmodel M5: structureID() returns the RAW bits, NEVER nuke-masked —
    // GC visitation (visitButterflyImpl) and every isNuked()/didRace test depend
    // on observing the nuke bit (history §16.2).
    StructureID structureID() const { return cellHeaderConcurrentLoad(m_structureID); }
    // SPEC-objectmodel M5: with shared-memory threads (Options::useJSThreads())
    // foreign readers can observe a transiently nuked StructureID mid-transition.
    // structure() — and ONLY structure() — clears the nuke bit before decoding;
    // the resulting pre-transition structure's offsets are always satisfied by
    // the not-yet-replaced or superset storage, because live storage never
    // shrinks (deletes quarantine slots — I18/I30; flat->segmented re-pairs old
    // offsets via the TAG dispatch, not the structure; history §15.4). The
    // decontaminate() is unconditional: flag-off no nuked ID is ever decoded
    // through structure() (the nuking thread is the only observer and does not
    // call it inside the window), so clearing an always-clear bit is
    // behavior-identical (I22). Exact-decode paths (transition protocols, GC)
    // use structureID() raw + StructureID::tryDecode/didRace instead.
    Structure* structure() const { return cellHeaderConcurrentLoad(m_structureID).decontaminate().decode(); }
    void setStructure(VM&, Structure*);
    // V7 (TSAN): header writers store via cellHeaderConcurrentStore so they
    // pair with the cellHeaderConcurrentLoad readers above (plain store
    // non-TSAN — identical codegen).
    void setStructureIDDirectly(StructureID id) { cellHeaderConcurrentStore(m_structureID, id); }
    void clearStructure() { cellHeaderConcurrentStore(m_structureID, StructureID()); }

    TypeInfo::InlineTypeFlags inlineTypeFlags() const { return cellHeaderConcurrentLoad(m_flags); }
    
    ASCIILiteral NODELETE className() const;

    // Extracting the value.
    JS_EXPORT_PRIVATE bool getString(JSGlobalObject*, String&) const;
    JS_EXPORT_PRIVATE String getString(JSGlobalObject*) const; // null string if not a string
    JS_EXPORT_PRIVATE JSObject* NODELETE getObject(); // NULL if not an object
    const JSObject* getObject() const; // NULL if not an object
        
    // Returns information about how to call/construct this cell as a function/constructor. May tell
    // you that the cell is not callable or constructor (default is that it's not either). If it
    // says that the function is callable, and the OverridesGetCallData type flag is set, and
    // this is an object, then typeof will return "function" instead of "object". These methods
    // cannot change their minds and must be thread-safe. They are sometimes called from compiler
    // threads.
    JS_EXPORT_PRIVATE static CallData NODELETE getCallData(JSCell*);
    JS_EXPORT_PRIVATE static CallData NODELETE getConstructData(JSCell*);

    // Basic conversions.
    JS_EXPORT_PRIVATE JSValue toPrimitive(JSGlobalObject*, PreferredPrimitiveType) const;
    bool toBoolean(JSGlobalObject*) const;
    inline TriState pureToBoolean() const;
    JS_EXPORT_PRIVATE double toNumber(JSGlobalObject*) const;
    JSObject* toObject(JSGlobalObject*) const;

    JSString* toStringInline(JSGlobalObject*) const;
    JS_EXPORT_PRIVATE JSString* toStringSlowCase(JSGlobalObject*) const;

    void dump(PrintStream&) const;
    JS_EXPORT_PRIVATE static void dumpToStream(const JSCell*, PrintStream&);

    size_t estimatedSizeInBytes(VM&) const;
    JS_EXPORT_PRIVATE static size_t NODELETE estimatedSize(JSCell*, VM&);

    DECLARE_VISIT_CHILDREN_WITH_MODIFIER(inline);
    DECLARE_VISIT_OUTPUT_CONSTRAINTS_WITH_MODIFIER(inline);

    JS_EXPORT_PRIVATE static void NODELETE analyzeHeap(JSCell*, HeapAnalyzer&);

    // Object operations, with the toObject operation included.
    inline const ClassInfo* classInfo() const; // Defined inline in Structure.h
    JS_EXPORT_PRIVATE bool validateIsNotSweeping() const;
    const MethodTable* methodTable() const;
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned propertyName, JSValue, bool shouldThrow);
    bool putInline(JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
        
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    JS_EXPORT_PRIVATE static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName);
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned propertyName);

    static inline bool canUseFastGetOwnProperty(const Structure&);
    inline JSValue fastGetOwnProperty(VM&, Structure&, PropertyName);

    // The recommended idiom for using cellState() is to switch on it or perform an == comparison on it
    // directly. We deliberately avoid helpers for this, because we want transparency about how the various
    // CellState values influences our various algorithms. 
    // V7 (TSAN): m_cellState is flipped by CAS from GC threads
    // (atomicCompareExchangeCellState*) and dcasHeaderAndButterfly swaps the
    // whole header word; the plain load/store here must be (TSAN-only)
    // relaxed atomics to pair with those — identical codegen non-TSAN.
    CellState cellState() const { return cellHeaderConcurrentLoad(m_cellState); }

    void setCellState(CellState data) const { cellHeaderConcurrentStore(const_cast<JSCell*>(this)->m_cellState, data); }
    
    bool atomicCompareExchangeCellStateWeakRelaxed(CellState oldState, CellState newState)
    {
        return WTF::atomicCompareExchangeWeakRelaxed(&m_cellState, oldState, newState);
    }

    CellState atomicCompareExchangeCellStateStrong(CellState oldState, CellState newState)
    {
        return WTF::atomicCompareExchangeStrong(&m_cellState, oldState, newState);
    }

    static constexpr ptrdiff_t structureIDOffset()
    {
        return OBJECT_OFFSETOF(JSCell, m_structureID);
    }

    static constexpr ptrdiff_t typeInfoFlagsOffset()
    {
        return OBJECT_OFFSETOF(JSCell, m_flags);
    }

    static constexpr ptrdiff_t typeInfoTypeOffset()
    {
        return OBJECT_OFFSETOF(JSCell, m_type);
    }

    // DO NOT store to this field. Always use a CAS loop, since some bits are flipped using CAS
    // from other threads due to the internal lock. One exception: you don't need the CAS if the
    // object has not escaped yet.
    static constexpr ptrdiff_t indexingTypeAndMiscOffset()
    {
        return OBJECT_OFFSETOF(JSCell, m_indexingTypeAndMisc);
    }

    static constexpr ptrdiff_t cellStateOffset()
    {
        return OBJECT_OFFSETOF(JSCell, m_cellState);
    }
    
    static constexpr TypedArrayType TypedArrayStorageType = NotTypedArray;

    void setPerCellBit(bool);
    bool perCellBit() const;
protected:

    void finishCreation(VM&);
    void finishCreation(VM&, Structure*, CreatingEarlyCellTag);

    // Dummy implementations of override-able static functions for classes to put in their MethodTable
    static NO_RETURN_DUE_TO_CRASH void NODELETE getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static NO_RETURN_DUE_TO_CRASH void NODELETE getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);

    JS_EXPORT_PRIVATE static NO_RETURN_DUE_TO_CRASH bool NODELETE preventExtensions(JSObject*, JSGlobalObject*);
    JS_EXPORT_PRIVATE static NO_RETURN_DUE_TO_CRASH bool NODELETE isExtensible(JSObject*, JSGlobalObject*);
    JS_EXPORT_PRIVATE static NO_RETURN_DUE_TO_CRASH bool NODELETE setPrototype(JSObject*, JSGlobalObject*, JSValue, bool);
    JS_EXPORT_PRIVATE static NO_RETURN_DUE_TO_CRASH JSValue NODELETE getPrototype(JSObject*, JSGlobalObject*);

    JS_EXPORT_PRIVATE static bool NODELETE customHasInstance(JSObject*, JSGlobalObject*, JSValue);
    JS_EXPORT_PRIVATE static bool NODELETE defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    JS_EXPORT_PRIVATE static bool NODELETE getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    JS_EXPORT_PRIVATE static bool NODELETE getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned propertyName, PropertySlot&);

private:
    friend class LLIntOffsetsExtractor;
    friend class JSCellLock;

    JS_EXPORT_PRIVATE JSObject* toObjectSlow(JSGlobalObject*) const;

    StructureID m_structureID;
    union {
        uint32_t m_blob;
        struct {
            IndexingType m_indexingTypeAndMisc; // DO NOT store to this field. Always CAS.
            JSType m_type;
            TypeInfo::InlineTypeFlags m_flags;
            CellState m_cellState;
        };
    };
};

class JSCellLock : public JSCell {
public:
    inline void lock();
    inline bool tryLock();
    inline void unlock();
    inline bool isLocked() const;
private:
    JS_EXPORT_PRIVATE void lockSlow();
    JS_EXPORT_PRIVATE void unlockSlow();
};

// FIXME: Refer to Subspace by reference.
// https://bugs.webkit.org/show_bug.cgi?id=166988
template<typename Type>
inline auto subspaceFor(VM& vm)
{
    return Type::template subspaceFor<Type, SubspaceAccess::OnMainThread>(vm);
}

template<typename Type>
inline auto subspaceForConcurrently(VM& vm)
{
    return Type::template subspaceFor<Type, SubspaceAccess::Concurrently>(vm);
}

#if CPU(X86_64)
JS_EXPORT_PRIVATE NEVER_INLINE NO_RETURN_DUE_TO_CRASH NOT_TAIL_CALLED void reportZappedCellAndCrash(const JSCell*);
#endif

} // namespace JSC
