/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTInstance.h"
#include "AOTPropertyCachesWords.h"
#include "AOTRuntimeType.h"
#include "AOTSlotRepresentation.h"
#include "AOTVector.h"
#include "ArrayAllocationProfile.h"
#include "ArrayConventions.h"
#include "CompleteSubspace.h"
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
#include <memory>
#include <optional>
#include <wtf/Noncopyable.h>
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

struct Instance;

inline constexpr uint32_t kindsOfPresentProperties = [] {
    uint32_t kinds = 0;
    for (Slot::Kind kind : allSlotKinds) {
        if (Slot::presenceOf(kind) == SlotPresence::Present)
            kinds |= 1u << static_cast<unsigned>(kind);
    }
    return kinds;
}();
static_assert(numberOfSlotKinds <= 32);

// What llint/LowLevelInterpreterAOT.asm spells out by hand.
static_assert(HashedSlot::ways == 4 && HashedSlot::offsetOfWay(0, 1) == sizeof(HashedSlot) && HashedSlot::offsetOfWay(0, 2) == 2 * sizeof(HashedSlot) && HashedSlot::offsetOfWay(0, 3) == 3 * sizeof(HashedSlot));
static_assert(!static_cast<uintptr_t>(SlotAccess::Read));
static_assert(undefinedCell32 == JSValue::ValueUndefined && emptyLiteralStringWord > undefinedCell32 && minCell32 > undefinedCell32);
static_assert(std::max({ absentInt32, undefinedInt32, nullInt32 }) < minInlineInt32);
static_assert(RuntimeType::sizeInBytes(1, 1, 0) == sizeof(RuntimeType) + 2 * sizeof(uint32_t) + sizeof(uint32_t) && RuntimeType::sizeInBytes(0, 0, RuntimeType::hasIntegerRange) == sizeof(RuntimeType) + 2 * sizeof(int64_t));
static_assert(traitsOf(SlotRepresentation::JSValue).hasUndefined && traitsOf(SlotRepresentation::OptionalBoolean).hasUndefined && traitsOf(SlotRepresentation::OptionalDouble).hasUndefined && traitsOf(SlotRepresentation::OptionalInt32).hasUndefined && traitsOf(SlotRepresentation::OptionalCell32).hasUndefined);
static_assert(ClosureWord::isNotCallableBit == 63);
static_assert(functionTypes.first == JSFunctionType && functionTypes.last == NullSetterFunctionType && functionTypes.last < slimClosureTypes.first);

// A rope of two strings: the header of the cell, the first string and the flags, then the length under the low half of the second string, then its high half.
static_assert(sizeof(JSRopeString) == 4 * sizeof(uint64_t) && JSRopeString::offsetOfFiber0() == sizeof(uint64_t) && JSRopeString::offsetOfLength() == 2 * sizeof(uint64_t));
static_assert(JSRopeString::CompactFibers::shiftOfFiber1LowerInLengthWord == 32 && JSRopeString::CompactFibers::widthOfFiber1LowerInLengthWord == 32);
static_assert(JSRopeString::is8BitInPointer == StringImpl::flagIs8Bit() && JSString::MaxLength == static_cast<unsigned>(INT32_MAX));

// Up to MarkedSpace::preciseCutoff every multiple of MarkedSpace::sizeStep is a size class, so Butterfly::optimalContiguousVectorLength() rounds the indexing header and the elements up to two words.
inline constexpr unsigned maxLengthOfArrayLiteralAllocatedInline = (MarkedSpace::preciseCutoff - sizeof(IndexingHeader)) / sizeof(EncodedJSValue);
static_assert(MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(IndexingHeader) == sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
static_assert(maxLengthOfArrayLiteralAllocatedInline % 2 == 1 && BASE_CONTIGUOUS_VECTOR_LEN % 2 == 1 && BASE_CONTIGUOUS_VECTOR_LEN_EMPTY % 2 == 1 && BASE_CONTIGUOUS_VECTOR_LEN_EMPTY <= maxLengthOfArrayLiteralAllocatedInline);
static_assert(MarkedSpace::sizeClassToIndex(MarkedSpace::preciseCutoff) * MarkedSpace::sizeStep == MarkedSpace::preciseCutoff);
static_assert(!IndexingHeader::offsetOfPublicLength() && IndexingHeader::offsetOfVectorLength() == 4);
static_assert(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY == 5);
inline constexpr uint64_t holeInDoubleArray = std::bit_cast<uint64_t>(PNaN);

// An ArrayAllocationProfile is a CompactPointerTuple of the last array and (indexing type << 8 | vector length).
inline constexpr unsigned bitsOfPointerInArrayAllocationProfile = 48;
inline constexpr unsigned bitsOfTypeInArrayAllocationProfile = 64 - bitsOfPointerInArrayAllocationProfile;
inline constexpr unsigned shiftOfIndexingTypeInArrayAllocationProfile = bitsOfPointerInArrayAllocationProfile + 8;
static_assert(sizeof(ArrayAllocationProfile) == sizeof(uint64_t) && CompactPointerTuple<JSArray*, uint16_t>::maxNumberOfBitsInPointer == bitsOfPointerInArrayAllocationProfile);
static_assert(ArrayWithUndecided < ArrayWithContiguous && ArrayWithInt32 < ArrayWithContiguous && ArrayWithDouble < ArrayWithContiguous && ArrayWithArrayStorage > ArrayWithContiguous && CopyOnWrite > ArrayWithContiguous);

namespace Interpreter {

// Each answers nothing in a realm that runs no compiled code, and the keyed ones for a key that the property caches take for no name.
std::optional<JSValue> tryGetById(JSGlobalObject*, JSCell* base, const Identifier&);
std::optional<JSValue> tryGetByVal(JSGlobalObject*, JSCell* base, JSValue key);
std::optional<bool> tryInById(JSGlobalObject*, JSObject* base, const Identifier&);
std::optional<bool> tryInByVal(JSGlobalObject*, JSObject* base, JSValue key);
bool tryPutById(JSGlobalObject*, JSCell* base, const Identifier&, JSValue, bool isStrict);
bool tryPutByVal(JSGlobalObject*, JSCell* base, JSValue key, JSValue, bool isStrict);

// What the cache of a get_by_id holds for a field in the cell, whose slot may be empty.
inline constexpr PropertyOffset narrowReferenceInSiteCache = 1 << 30;
inline constexpr PropertyOffset extraPropertyInSiteCache = 1 << 29;
inline constexpr uint32_t maxHiddenWordOffsetInSiteCache = (extraPropertyInSiteCache >> 16) - 1;
std::optional<PropertyOffset> placeOfFieldInSiteCache(JSGlobalObject*, Structure* ofCompiledObject, const Identifier&);

// After the interpreter's lookup in an object that is not compiled. Files what the interpreter's own cache cannot hold: an absent name, an accessor, a property of a compiled object above the base.
void didGetById(JSGlobalObject*, JSObject* base, Structure* structureBefore, const Identifier&, const PropertySlot&);

// A new_object followed at once by direct put_by_ids to its result. llint_aot_op_new_object allocates the finished object and skips the stores.
struct ObjectLiteralPlan {
    static constexpr unsigned maxNumberOfValues = 16;
    static constexpr uintptr_t allocatorTag = 1;

    static constexpr ptrdiff_t offsetOfAllocator() { return OBJECT_OFFSETOF(ObjectLiteralPlan, allocator); }
    static constexpr ptrdiff_t offsetOfCellHeader() { return OBJECT_OFFSETOF(ObjectLiteralPlan, cellHeader); }
    static constexpr ptrdiff_t offsetOfSecondWord() { return OBJECT_OFFSETOF(ObjectLiteralPlan, secondWord); }
    static constexpr ptrdiff_t offsetOfNumberOfValues() { return OBJECT_OFFSETOF(ObjectLiteralPlan, numberOfValues); }
    static constexpr ptrdiff_t offsetOfNumberOfClearedWords() { return OBJECT_OFFSETOF(ObjectLiteralPlan, numberOfClearedWords); }
    static constexpr ptrdiff_t offsetOfOffsetOfClearedWords() { return OBJECT_OFFSETOF(ObjectLiteralPlan, offsetOfClearedWords); }
    static constexpr ptrdiff_t offsetOfValueOffsets() { return OBJECT_OFFSETOF(ObjectLiteralPlan, valueOffsets); }

    static const ObjectLiteralPlan* of(ObjectAllocationProfile&);
    // Every word is written but those of the values.
    uint8_t* allocateCell(VM&) const;
    // The words from the third on, but those in front that the values fill.
    void setClearedWords(unsigned numberOfWordsBehindSecond);

    LocalAllocator* allocator { nullptr };
    uint64_t cellHeader { 0 };
    uint64_t secondWord { 0 };
    uint32_t numberOfValues { 0 };
    uint32_t numberOfClearedWords { 0 };
    uint32_t offsetOfClearedWords { 2 * sizeof(uint64_t) };
    uint16_t valueOffsets[maxNumberOfValues] { };
};

class ObjectLiteralPlans {
    WTF_MAKE_TZONE_ALLOCATED(ObjectLiteralPlans);
    WTF_MAKE_NONCOPYABLE(ObjectLiteralPlans);
public:
    ObjectLiteralPlans() = default;

    const ObjectLiteralPlan* findOrCreate(Instance&, Structure* emptyStructure, std::span<UniquedStringImpl* const> names, bool mayBeRecord);
    // An object got its first property outside its cell. If a plan made it, the objects that the plan makes from now on have room for two more properties in their cells.
    void didOutgrowCell(Instance&, Structure* beforeAddition);

private:
    static constexpr size_t maxNumberOfPlans = 256;
    static constexpr unsigned maxPropertiesAddedInCell = 8;

    static std::unique_ptr<ObjectLiteralPlan> createPlanForRecord(Instance&, std::span<UniquedStringImpl* const> names);

    struct Entry {
        Structure* emptyStructure;
        Structure* structureOfFinalObject;
        bool mayBeRecord;
        Vector<RefPtr<UniquedStringImpl>, 4> names;
        std::unique_ptr<ObjectLiteralPlan> plan;
    };

    Vector<Entry> m_entries;
};

struct FusedClosureVariableRead {
    static constexpr unsigned typeBits = 8;
    static constexpr unsigned bytesToSkipBits = 8;
    static constexpr unsigned offsetShift = typeBits + bytesToSkipBits;
    static constexpr uint32_t typeMask = (1u << typeBits) - 1;
    static constexpr uint32_t bytesToSkipMask = (1u << bytesToSkipBits) - 1;
    static constexpr uint32_t maxOffset = UINT32_MAX >> offsetShift;
};
void fuseReadsOfClosureVariables(CodeBlock&);

// What llint_aot_op_create_lexical_environment reads of a SymbolTable, whose members are private. A table's first environments are made by the slow path, which calls checkPlacesInSymbolTable().
struct PlacesInSymbolTable {
    static constexpr ptrdiff_t offsetOfMaxScopeOffset = OBJECT_OFFSETOF(SymbolTable, m_lock) - sizeof(ScopeOffset);
    static constexpr ptrdiff_t offsetOfSingleton = sizeof(SymbolTable) - 2 * sizeof(void*);
    // InferredValue::encodeState(IsInvalidated).
    static constexpr uintptr_t invalidatedSingleton = IsInvalidated << 1 | 1;
};
void checkPlacesInSymbolTable(SymbolTable&);

inline constexpr unsigned customAccessorRecordShift = 5;
static_assert(sizeof(CustomAccessorRecord) == 1u << customAccessorRecordShift);

inline constexpr ptrdiff_t offsetOfPromiseAllocator = AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Promise)].allocator);
inline constexpr ptrdiff_t offsetOfPromiseHeader = AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Promise)].header);

void didLinkNewObject(ObjectAllocationProfile&);
// True where the profile had no plan yet.
bool planObjectLiteral(JSGlobalObject*, CodeBlock*, const JSInstruction*, ObjectAllocationProfile&);
void didAllocateFirstPropertyOutsideCell(JSGlobalObject*, Structure* beforeAddition);

void didLeaveInlineAllocation(VM&);
void didLeaveInlineBranch(VM&);

} // namespace Interpreter

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
