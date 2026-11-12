/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTInstance.h"
#include "AOTObjectBuilder.h"
#include "AOTPropertyCachesWords.h"
#include "AOTRuntimeType.h"
#include "AOTSlotRepresentation.h"
#include "AOTVector.h"
#include "ArrayAllocationProfile.h"
#include "ArrayConventions.h"
#include "CompleteSubspace.h"
#include "GetPutInfo.h"
#include "IndexingHeader.h"
#include "Instruction.h"
#include "JSCJSValue.h"
#include "JSString.h"
#include "JSType.h"
#include "ObjectAllocationProfile.h"
#include "PropertyOffset.h"
#include "PureNaN.h"
#include "SymbolTable.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class CodeBlock;
class Identifier;
class JSCell;
class JSGlobalObject;
class JSObject;
class LocalAllocator;
class PropertySlot;
class Structure;
class VM;

namespace AOT {

void willRunInInterpreter(CodeBlock*);

inline constexpr uint32_t presentPropertyKinds = [] {
    uint32_t kinds = 0;
    for (Slot::Kind kind : allSlotKinds) {
        if (Slot::presenceOf(kind) == SlotPresence::Present)
            kinds |= 1u << static_cast<unsigned>(kind);
    }
    return kinds;
}();
static_assert(numberOfSlotKinds <= 32);

static_assert(HashedSlot::ways == 4 && HashedSlot::offsetOfWay(0, 1) == sizeof(HashedSlot) && HashedSlot::offsetOfWay(0, 2) == 2 * sizeof(HashedSlot) && HashedSlot::offsetOfWay(0, 3) == 3 * sizeof(HashedSlot));
static_assert(!static_cast<uintptr_t>(SlotAccess::Read));
static_assert(undefinedCell32 == JSValue::ValueUndefined && minCell32 > undefinedCell32);
static_assert(std::max({ absentInt32, undefinedInt32, nullInt32 }) < minInlineInt32);
static_assert(RuntimeType::sizeInBytes(1, 1, 0) == sizeof(RuntimeType) + 2 * sizeof(uint32_t) + sizeof(uint32_t) && RuntimeType::sizeInBytes(0, 0, RuntimeType::hasIntegerRange) == sizeof(RuntimeType) + 2 * sizeof(int64_t));
static_assert(traitsOf(SlotRepresentation::JSValue).hasUndefined && traitsOf(SlotRepresentation::OptionalBoolean).hasUndefined && traitsOf(SlotRepresentation::OptionalDouble).hasUndefined && traitsOf(SlotRepresentation::OptionalInt32).hasUndefined && traitsOf(SlotRepresentation::OptionalCell32).hasUndefined);
static_assert(ClosureWord::isNotCallableBit == 63);
static_assert(functionTypes.first == JSFunctionType && functionTypes.last == NullSetterFunctionType && functionTypes.last < slimClosureTypes.first);

static_assert(sizeof(JSRopeString) == 4 * sizeof(uint64_t) && JSRopeString::offsetOfFiber0() == sizeof(uint64_t) && JSRopeString::offsetOfLength() == 2 * sizeof(uint64_t));
static_assert(JSRopeString::CompactFibers::shiftOfFiber1LowerInLengthWord == 32 && JSRopeString::CompactFibers::widthOfFiber1LowerInLengthWord == 32);
static_assert(JSRopeString::is8BitInPointer == StringImpl::flagIs8Bit() && JSString::MaxLength == static_cast<unsigned>(INT32_MAX));

inline constexpr unsigned maxFastPathArrayLiteralLength = (MarkedSpace::preciseCutoff - sizeof(IndexingHeader)) / sizeof(EncodedJSValue);
static_assert(MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(IndexingHeader) == sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
static_assert(maxFastPathArrayLiteralLength % 2 == 1 && BASE_CONTIGUOUS_VECTOR_LEN % 2 == 1 && BASE_CONTIGUOUS_VECTOR_LEN_EMPTY % 2 == 1 && BASE_CONTIGUOUS_VECTOR_LEN_EMPTY <= maxFastPathArrayLiteralLength);
static_assert(MarkedSpace::sizeClassToIndex(MarkedSpace::preciseCutoff) * MarkedSpace::sizeStep == MarkedSpace::preciseCutoff);
static_assert(!IndexingHeader::offsetOfPublicLength() && IndexingHeader::offsetOfVectorLength() == 4);
static_assert(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY == 5);
inline constexpr uint64_t holeInDoubleArray = std::bit_cast<uint64_t>(PNaN);

inline constexpr unsigned arrayAllocationProfilePointerBits = 48;
inline constexpr unsigned arrayAllocationProfileTypeBits = 64 - arrayAllocationProfilePointerBits;
inline constexpr unsigned arrayAllocationProfileIndexingTypeShift = arrayAllocationProfilePointerBits + 8;
static_assert(sizeof(JSC::ArrayAllocationProfile) == sizeof(uint64_t) && CompactPointerTuple<JSArray*, uint16_t>::maxNumberOfBitsInPointer == arrayAllocationProfilePointerBits);
static_assert(ArrayWithUndecided < ArrayWithContiguous && ArrayWithInt32 < ArrayWithContiguous && ArrayWithDouble < ArrayWithContiguous && ArrayWithArrayStorage > ArrayWithContiguous && CopyOnWrite > ArrayWithContiguous);

namespace Interpreter {

std::optional<JSValue> tryGetById(JSGlobalObject*, JSCell* base, const Identifier&);
std::optional<JSValue> tryGetByVal(JSGlobalObject*, JSCell* base, JSValue key);
std::optional<bool> tryInById(JSGlobalObject*, JSObject* base, const Identifier&);
std::optional<bool> tryInByVal(JSGlobalObject*, JSObject* base, JSValue key);
bool tryPutById(JSGlobalObject*, JSCell* base, const Identifier&, JSValue, bool isStrict);
std::optional<JSValue> tryGetFromContext(JSGlobalObject*, const Identifier&);
bool tryPutToContext(JSGlobalObject*, const Identifier&, JSValue, PutPropertySlot&);
bool tryPutByVal(JSGlobalObject*, JSCell* base, JSValue key, JSValue, bool isStrict);
std::optional<bool> tryInstanceOf(JSGlobalObject*, JSValue, JSValue constructor);
void didCreateFunction(JSGlobalObject*, FunctionExecutable*, JSFunction*);
void didCreateClonedArguments(JSGlobalObject*);
inline constexpr ptrdiff_t offsetOfExecutablesInStorage = FixedVector<WriteBarrier<FunctionExecutable>>::Storage::offsetOfData();
void rememberSwitchTarget(JSGlobalObject*, const JSInstruction*, const StringImpl& atom, int32_t offset);

void didGetById(JSGlobalObject*, JSCell* base, Structure* structureBefore, const Identifier&, const PropertySlot&, bool isStoredInTable);
void didPutById(JSGlobalObject*, JSObject* base, Structure* structureBefore, const Identifier&, const PutPropertySlot&);

inline constexpr PropertyOffset narrowReferenceInSiteCache = 1 << 30;
inline constexpr PropertyOffset extraPropertyInSiteCache = 1 << 29;
inline constexpr PropertyOffset fieldInSiteCache = 1 << 28;
inline constexpr PropertyOffset siteCacheHasOtherMode = std::numeric_limits<PropertyOffset>::min();
inline constexpr PropertyOffset otherModeOffsetMask = std::numeric_limits<PropertyOffset>::max();
inline constexpr uint32_t maxHiddenWordOffsetInSiteCache = (extraPropertyInSiteCache >> 16) - 1;
enum class StoreSiteKind : uint8_t {
    Other,
    Adds,
    Replaces,
    AddsWithChain,
};
inline constexpr unsigned storeSiteKindShift = 24;
inline constexpr unsigned storeSiteOffsetMask = (1u << storeSiteKindShift) - 1;
inline unsigned storeSiteOffset(PropertyOffset offset, StoreSiteKind kind)
{
    return isInlineOffset(offset) ? offset | static_cast<unsigned>(kind) << storeSiteKindShift : offset;
}
std::optional<PropertyOffset> fieldOffsetForSiteCache(JSGlobalObject*, Structure* before, const Identifier&);

bool isClassWithLazyMembers(VM&, JSCell* closure);

struct FusedClosureVariableRead {
    enum Kind : uint8_t {
        None,
        Read,
        ReadIntoScopeRegister,
        ReadAndCheck,
        ReadIntoScopeRegisterAndCheck,
        ReadOuter,
        ReadOuterIntoScopeRegister,
        ReadOuterAndCheck,
        ReadOuterIntoScopeRegisterAndCheck,
        ReadGlobal,
        ReadGlobalIntoScopeRegister,
    };
    static constexpr unsigned typeBits = 8;
    static constexpr unsigned kindBits = 8;
    static constexpr unsigned offsetShift = typeBits + kindBits;
    static constexpr unsigned offsetOfVariable = offsetShift / 8;
    static constexpr uint32_t maxOffset = UINT32_MAX >> offsetShift;
    static constexpr unsigned typeAndKind(Kind kind) { return ClosureVar | kind << typeBits; }
    static constexpr unsigned kindOnly(Kind kind) { return kind << typeBits; }
};
void fuseVariableReads(CodeBlock&, std::span<const JSInstruction* const> resolutions);

struct ObjectLiteralPlan {
    static constexpr uintptr_t allocatorTag = 1;
    static constexpr uintptr_t notPlannedYet = allocatorTag | 2;

    static constexpr ptrdiff_t offsetOfAllocator() { return OBJECT_OFFSETOF(ObjectLiteralPlan, allocator); }
    static constexpr ptrdiff_t offsetOfCellHeader() { return OBJECT_OFFSETOF(ObjectLiteralPlan, cellHeader); }
    static constexpr ptrdiff_t offsetOfSecondWord() { return OBJECT_OFFSETOF(ObjectLiteralPlan, secondWord); }
    static constexpr ptrdiff_t offsetOfNumberOfValues() { return OBJECT_OFFSETOF(ObjectLiteralPlan, numberOfValues); }
    static constexpr ptrdiff_t offsetOfNumberOfClearedWords() { return OBJECT_OFFSETOF(ObjectLiteralPlan, numberOfClearedWords); }
    static constexpr ptrdiff_t offsetOfOffsetOfClearedWords() { return OBJECT_OFFSETOF(ObjectLiteralPlan, offsetOfClearedWords); }
    static constexpr ptrdiff_t offsetOfValueOffsets() { return sizeof(ObjectLiteralPlan); }

    static std::unique_ptr<ObjectLiteralPlan> create(unsigned numberOfValues);
    void operator delete(void* plan) { fastFree(plan); }

    static const ObjectLiteralPlan* of(ObjectAllocationProfile&);
    uint8_t* allocateCell(VM&) const;
    void setClearedWords(unsigned numberOfWordsBehindSecond);
    std::span<uint16_t> valueOffsets();
    std::span<const uint16_t> valueOffsets() const;

    LocalAllocator* allocator { nullptr };
    uint64_t cellHeader { 0 };
    uint64_t secondWord { 0 };
    uint32_t numberOfValues { 0 };
    uint32_t numberOfClearedWords { 0 };
    uint32_t offsetOfClearedWords { 2 * sizeof(uint64_t) };
};

class ObjectLiteralPlans {
    WTF_MAKE_TZONE_ALLOCATED(ObjectLiteralPlans);
    WTF_MAKE_NONCOPYABLE(ObjectLiteralPlans);
public:
    ObjectLiteralPlans() = default;

    const ObjectLiteralPlan* findOrCreate(Instance&, std::span<UniquedStringImpl* const> names);

private:
    static std::unique_ptr<ObjectLiteralPlan> createPlan(Instance&, std::span<UniquedStringImpl* const> names);
    static bool storesValuesUnchanged(const ObjectBuilderPlan&);

    struct Entry {
        Vector<RefPtr<UniquedStringImpl>, 4> names;
        std::unique_ptr<ObjectLiteralPlan> plan;
    };

    Vector<Entry> m_entries;
};

void didLinkNewObject(ObjectAllocationProfile&);
void planObjectLiteral(JSGlobalObject*, CodeBlock*, const JSInstruction*, ObjectAllocationProfile&);
JSArray* createEmptyArray(JSGlobalObject*, JSC::ArrayAllocationProfile&);

struct SymbolTableOffsets {
    static constexpr ptrdiff_t offsetOfMaxScopeOffset = OBJECT_OFFSETOF(SymbolTable, m_lock) - sizeof(ScopeOffset);
    static constexpr ptrdiff_t offsetOfSingleton = sizeof(SymbolTable) - 2 * sizeof(void*);
    static constexpr uintptr_t invalidatedSingleton = IsInvalidated << 1 | 1;
};
void checkSymbolTableOffsets(SymbolTable&);

inline constexpr unsigned customAccessorRecordShift = 5;
static_assert(sizeof(CustomAccessorRecord) == 1u << customAccessorRecordShift);
JSValue callCustomGetter(const CustomAccessorRecord&, JSCell* base, const Identifier&);

inline constexpr uint8_t inheritedValueSlowPathFlags = Slot::inheritsGetter | Slot::emptySlotMayBeUndefined;

inline constexpr ptrdiff_t offsetOfPromiseAllocator = AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Promise)].allocator);
inline constexpr ptrdiff_t offsetOfPromiseHeader = AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Promise)].header);

} // namespace Interpreter

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
