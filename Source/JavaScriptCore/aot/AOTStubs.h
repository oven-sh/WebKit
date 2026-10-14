/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "CCallHelpers.h"
#include <wtf/Vector.h>

namespace JSC {

class LinkBuffer;

namespace AOT {

#define FOR_EACH_AOT_OPERATION_WITH_FRONT_END(v) \
    v(GetById) \
    v(GetByVal) \
    v(PutById) \
    v(PutByVal) \
    v(NewObjectLiteral) \
    v(CompareStrictEq) \
    v(CompareEq) \

static constexpr ptrdiff_t offsetOfInstanceRegisterInAdapter = -24;
static constexpr ptrdiff_t offsetOfNumberTagRegisterInAdapter = -16;
static constexpr ptrdiff_t offsetOfNotCellMaskRegisterInAdapter = -8;
#if CPU(X86_64)
static constexpr ptrdiff_t offsetOfR12InAdapter = -32;
static constexpr ptrdiff_t offsetOfRBXInAdapter = -40;
static constexpr ptrdiff_t offsetOfInstanceInAdapter = -48;
static constexpr unsigned adapterSaveAreaSize = 48;
#else
static constexpr ptrdiff_t offsetOfInstanceInAdapter = -32;
static constexpr unsigned adapterSaveAreaSize = 32;
#endif

static constexpr unsigned minCodeGranuleShift = 6;
static constexpr unsigned maxCodeGranuleShift = 12;

#define FOR_EACH_AOT_OPERATION_BEHIND_HELPER(v) \
    v(NewArray, operationAOTNewArray) \
    v(ValueAdd, operationAOTValueAdd) \

#define FOR_EACH_AOT_HELPER(v) \
    v(HelperNewArray) \
    v(HelperNewInt32Array) \
    v(HelperAddStrings) \

#define FOR_EACH_AOT_STUB(v) \
    v(ReturnFromCallWithList) \
    v(Call) \
    v(CallHostFunction) \
    v(CallInternalFunction) \
    v(ConstructInternalFunction) \
    v(CallList) \
    v(Enter) \
    v(EnterModule) \
    v(EnterClosure) \
    v(EnterSlimClosure) \
    v(EnterResume) \
    v(CallValue) \
    v(CallValueWithoutThis) \
    v(CallMethod) \
    v(CallMethodRead) \
    v(ApplyArray) \
    v(SpreadArray) \
    v(ConstructClosure) \
    v(EnterFunctionForCall) \
    v(EnterFunctionForConstruct) \
    v(EnterStaticFunctionForCall) \
    v(EnterStaticFunctionForConstruct) \
    v(ConstructViaCall) \
    v(CallBoundFunction) \
    v(OperationValue) \
    v(OperationVoid) \
    v(OperationDouble) \
    v(OperationValueWithGlobalObject) \
    v(OperationValueWithInstance) \
    v(OperationVoidWithInstance) \
    v(OperationDoubleWithInstance) \
    v(UncheckedOperationWithInstance) \
    v(UncheckedOperationDoubleWithInstance) \
    v(ThrowAtSite) \
    v(LeafThrowAtSite) \
    v(ThrowAtSiteByWord) \
    v(LeafThrowAtSiteByWord) \
    v(ThrowAtFault) \
    v(LeafThrowAtFault) \
    v(ThrowAtTrap) \
    v(LeafThrowAtTrap) \
    v(ColdOperationVoid) \
    v(LeafColdOperationVoid) \
    v(ColdOperationVoidWithoutFPRs) \
    v(LeafColdOperationVoidWithoutFPRs) \
    v(ColdOperationValue) \
    v(LeafColdOperationValue) \
    v(ColdOperationValueWithoutFPRs) \
    v(LeafColdOperationValueWithoutFPRs) \
    v(ColdPlainOperation) \
    v(LeafColdPlainOperation) \
    v(ColdPlainOperationWithoutFPRs) \
    v(LeafColdPlainOperationWithoutFPRs) \
    v(ColdPlainOperationWithoutInstance) \
    v(LeafColdPlainOperationWithoutInstance) \
    v(ColdPlainOperationDouble) \
    v(LeafColdPlainOperationDouble) \
    v(ColdPlainOperationDoubleWithoutInstance) \
    v(LeafColdPlainOperationDoubleWithoutInstance) \
    v(ColdOperationDouble) \
    v(LeafColdOperationDouble) \
    v(ColdCallFunction) \
    v(LeafColdCallFunction) \
    v(PlainOperation) \
    v(PlainOperationWithInstance) \
    v(DidCreateTypedField) \
    v(LeafDidCreateTypedField) \
    v(PutPropertiesWithStorage) \
    v(RememberElement) \
    v(LeafRememberElement) \
    v(AllocateTypedCell) \
    v(LeafAllocateTypedCell) \
    v(AllocateTypedObject) \
    v(LeafAllocateTypedObject) \
    v(AllocateTypedObjectFromTemplate) \
    v(LeafAllocateTypedObjectFromTemplate) \
    v(NewTypedClosure0) \
    v(LeafNewTypedClosure0) \
    v(NewTypedClosure1) \
    v(LeafNewTypedClosure1) \
    v(NewTypedClosure2) \
    v(LeafNewTypedClosure2) \
    v(NewTypedClosure3) \
    v(LeafNewTypedClosure3) \
    v(NewTypedClosure4) \
    v(LeafNewTypedClosure4) \
    v(NewTypedClosureOnce) \
    v(LeafNewTypedClosureOnce) \
    v(NewTypedClosureOnce32) \
    v(LeafNewTypedClosureOnce32) \
    v(InitializeTypedClosure0) \
    v(LeafInitializeTypedClosure0) \
    v(InitializeTypedClosure1) \
    v(LeafInitializeTypedClosure1) \
    v(InitializeTypedClosure2) \
    v(LeafInitializeTypedClosure2) \
    v(InitializeTypedClosure3) \
    v(LeafInitializeTypedClosure3) \
    v(InitializeTypedClosure4) \
    v(LeafInitializeTypedClosure4) \
    v(MaterializeEnvironment) \
    v(LeafMaterializeEnvironment) \
    v(TypedString) \
    v(LeafTypedString) \
    v(CallModuleFunction) \
    v(TypedStringMadeOnce) \
    v(LeafTypedStringMadeOnce) \
    v(TypedStringInBlock1) \
    v(LeafTypedStringInBlock1) \
    v(TypedStringMadeOnceInBlock1) \
    v(LeafTypedStringMadeOnceInBlock1) \
    v(TypedStringInBlock2) \
    v(LeafTypedStringInBlock2) \
    v(TypedStringMadeOnceInBlock2) \
    v(LeafTypedStringMadeOnceInBlock2) \
    v(TypedStringInBlock3) \
    v(LeafTypedStringInBlock3) \
    v(TypedStringMadeOnceInBlock3) \
    v(LeafTypedStringMadeOnceInBlock3) \
    v(TypedStringInBlock4) \
    v(LeafTypedStringInBlock4) \
    v(TypedStringMadeOnceInBlock4) \
    v(LeafTypedStringMadeOnceInBlock4) \
    v(TypedStringInBlock5) \
    v(LeafTypedStringInBlock5) \
    v(TypedStringMadeOnceInBlock5) \
    v(LeafTypedStringMadeOnceInBlock5) \
    v(TypedStringInBlock6) \
    v(LeafTypedStringInBlock6) \
    v(TypedStringMadeOnceInBlock6) \
    v(LeafTypedStringMadeOnceInBlock6) \
    v(TypedStringInBlock7) \
    v(LeafTypedStringInBlock7) \
    v(TypedStringMadeOnceInBlock7) \
    v(LeafTypedStringMadeOnceInBlock7) \
    v(StringIsText) \
    v(LeafStringIsText) \
    v(StringReplaceRegExpStringThatMayFindNothing) \
    v(NewRegExpLiteral) \
    v(UncheckedNewRegExpLiteral) \
    v(RegExpExecThatAsksBodyFirst) \
    v(RegExpTestThatAsksBodyFirst) \
    v(StringToLowerCase) \
    v(StringToUpperCase) \
    v(UncheckedStringToLowerCase) \
    v(UncheckedStringToUpperCase) \
    v(NewTypedArray) \
    v(UncheckedNewTypedArray) \
    v(HoldToType) \
    v(LeafHoldToType) \
    v(StringStartsWithText) \
    v(LeafStringStartsWithText) \
    v(StringEndsWithText) \
    v(LeafStringEndsWithText) \
    v(StringsAreEqual) \
    v(LeafStringsAreEqual) \
    v(ConcatenateTwo) \
    v(LeafConcatenateTwo) \
    v(ConcatenateThree) \
    v(LeafConcatenateThree) \
    v(StringOfInt32) \
    v(LeafStringOfInt32) \
    v(StringTrim) \
    v(LeafStringTrim) \
    v(StringTrimStart) \
    v(LeafStringTrimStart) \
    v(StringTrimEnd) \
    v(LeafStringTrimEnd) \
    v(StringTrimFlat8) \
    v(LeafStringTrimFlat8) \
    v(StringTrimStartFlat8) \
    v(LeafStringTrimStartFlat8) \
    v(StringTrimEndFlat8) \
    v(LeafStringTrimEndFlat8) \
    v(TypedClosure) \
    v(LeafTypedClosure) \
    v(TypedBuiltinCell) \
    v(LeafTypedBuiltinCell) \
    v(TypedClosureEntry) \
    v(LeafTypedClosureEntry) \
    v(IsArrayAsMade) \
    v(LeafIsArrayAsMade) \
    v(TypedNewPromise) \
    v(LeafTypedNewPromise) \
    v(AwaitMayContinue) \
    v(LeafAwaitMayContinue) \
    v(ArrayLiteral0) \
    v(LeafArrayLiteral0) \
    v(ArrayLiteral1) \
    v(LeafArrayLiteral1) \
    v(ArrayLiteral2) \
    v(LeafArrayLiteral2) \
    v(ArrayLiteral3) \
    v(LeafArrayLiteral3) \
    v(ArrayLiteral4) \
    v(LeafArrayLiteral4) \
    v(ElementOrNothing) \
    v(LeafElementOrNothing) \
    v(ArrayOfDoubles) \
    v(LeafArrayOfDoubles) \
    v(ArrayOfInt32s) \
    v(LeafArrayOfInt32s) \
    v(EmptyArrayOfDoubles) \
    v(LeafEmptyArrayOfDoubles) \
    v(EmptyArrayOfInt32s) \
    v(LeafEmptyArrayOfInt32s) \
    v(EmptyArrayOfDoublesWithRoomFor1) \
    v(LeafEmptyArrayOfDoublesWithRoomFor1) \
    v(EmptyArrayOfDoublesWithRoomFor3) \
    v(LeafEmptyArrayOfDoublesWithRoomFor3) \
    v(EmptyArrayWithRoomFor1) \
    v(LeafEmptyArrayWithRoomFor1) \
    v(EmptyArrayWithRoomFor3) \
    v(LeafEmptyArrayWithRoomFor3) \
    v(EmptyArrayWithRoom) \
    v(LeafEmptyArrayWithRoom) \
    v(ArrayOfValues) \
    v(LeafArrayOfValues) \
    v(EmptyArrayAtSite) \
    v(LeafEmptyArrayAtSite) \
    v(PushToFullVector) \
    v(LeafPushToFullVector) \
    v(PushDoubleToFullVector) \
    v(LeafPushDoubleToFullVector) \
    v(PartOfString) \
    v(LeafPartOfString) \
    v(StoreElement) \
    v(LeafStoreElement) \
    v(LoadElement) \
    v(LeafLoadElement) \
    v(ArraySlice) \
    v(LeafArraySlice) \
    v(ArrayCopy) \
    v(LeafArrayCopy) \
    v(WriteBarrier) \
    v(ToBoolean) \
    v(CharactersOfSubstring) \
    v(CodeUnitOfRope) \
    v(LeafCodeUnitOfRope) \
    v(StrictEqual) \
    v(LooseEqual) \
    v(Add) \
    v(Mod) \
    v(Sub) \
    v(Mul) \
    v(Div) \
    v(BitAnd) \
    v(BitOr) \
    v(BitXor) \
    v(LShift) \
    v(RShift) \
    v(URShift) \
    v(Less) \
    v(LessEq) \
    v(Greater) \
    v(GreaterEq) \
    v(AddBehindInt32Test) \
    v(SubBehindInt32Test) \
    v(MulBehindInt32Test) \
    v(BitAndBehindInt32Test) \
    v(BitOrBehindInt32Test) \
    v(BitXorBehindInt32Test) \
    v(LShiftBehindInt32Test) \
    v(RShiftBehindInt32Test) \
    v(URShiftBehindInt32Test) \
    v(LessBehindInt32Test) \
    v(LessEqBehindInt32Test) \
    v(GreaterBehindInt32Test) \
    v(GreaterEqBehindInt32Test) \
    v(GetByValAtIndex) \
    v(GetByVal) \
    v(PutByValAtIndex) \
    v(PutByVal) \
    v(GetLength) \
    v(GetById) \
    v(PutById) \
    v(TryGetByNameAndRow) \
    v(TryPutByNameAndRow) \
    v(DefineOwnProperty) \
    v(GetByValCached) \
    v(PutByValCached) \
    v(PutByValCachedSloppy) \
    v(PrivateGet) \
    v(PrivateSet) \
    v(PrivateHas) \
    v(PrivateFieldAdd) \
    v(InByValCached) \
    v(DeleteByValCached) \
    v(DeleteByValCachedSloppy) \
    v(InstanceOf) \
    v(InstanceOfCached) \
    v(MapGetAskingReceiver) \
    v(MapGet) \
    v(MapHasAskingReceiver) \
    v(MapHas) \
    v(MapSetAskingReceiver) \
    v(MapSet) \
    v(SetHasAskingReceiver) \
    v(SetHas) \
    v(SetAddAskingReceiver) \
    v(SetAdd) \
    v(MapSetWithStringKeyAskingReceiver) \
    v(MapSetWithStringKey) \
    v(SetAddWithStringKeyAskingReceiver) \
    v(SetAddWithStringKey) \
    v(MapGetWithStringKeyAskingReceiver) \
    v(MapGetWithStringKey) \
    v(MapHasWithStringKeyAskingReceiver) \
    v(MapHasWithStringKey) \
    v(SetHasWithStringKeyAskingReceiver) \
    v(SetHasWithStringKey) \
    v(MapDelete) \
    v(SetDelete) \
    v(MapGetByBits) \
    v(MapHasByBits) \
    v(MapSetByBits) \
    v(SetHasByBits) \
    v(SetAddByBits) \
    v(ArrayIncludes) \
    v(ArrayIndexOf) \
    v(ArrayIncludesOrCall) \
    v(ArrayIndexOfOrCall) \
    v(ArrayIncludesOrCallMethodRead) \
    v(ArrayIndexOfOrCallMethodRead) \
    v(ArrayPushOrCallMethod) \
    v(ArrayPushOrCallValue) \
    v(MapGetOrCallMethod) \
    v(MapGetOrCallValue) \
    v(MapHasOrCallMethod) \
    v(MapHasOrCallValue) \
    v(MapSetOrCallMethod) \
    v(MapSetOrCallValue) \
    v(SetHasOrCallMethod) \
    v(SetHasOrCallValue) \
    v(SetAddOrCallMethod) \
    v(SetAddOrCallValue) \
    v(TakeElements) \
    v(HandleException) \
    v(PollAtPrologue) \
    v(HandleExceptionAtPrologue) \
    v(ThrowStackOverflowAtPrologue) \
    v(ThrowStackOverflow) \
    v(ThrowCalledIndirectly) \
    v(Catch) \
    v(RejectInFrame) \
    v(VirtualCall) \
    v(VirtualConstruct) \
    v(VirtualTailCall) \
    v(FrontEndGetById) \
    v(FrontEndGetByVal) \
    v(FrontEndPutById) \
    v(FrontEndPutByVal) \
    v(FrontEndNewObjectLiteral) \
    v(FrontEndCompareStrictEq) \
    v(FrontEndCompareEq) \
    FOR_EACH_AOT_HELPER(v) \
    v(NewArrayWithFastPath) \
    v(ValueAddWithFastPath) \
    v(NewObjectLiteral0) \
    v(NewObjectLiteral1) \
    v(NewObjectLiteral2) \
    v(NewObjectLiteral3) \
    v(NewObjectLiteral4) \
    v(NewObjectLiteral5) \
    v(NewObjectLiteral6) \
    v(NewObjectOfRow0) \
    v(NewObjectOfRow1) \
    v(NewObjectOfRow2) \
    v(NewObjectOfRow3) \
    v(NewObjectOfRow4) \
    v(NewObjectOfRow5) \
    v(NewObjectOfRow6) \
    v(CreateThisByNewTarget) \
    v(ConstructClosureList) \

enum class Stub : uint16_t {
#define AOT_DEFINE_STUB(name) name,
    FOR_EACH_AOT_STUB(AOT_DEFINE_STUB)
#undef AOT_DEFINE_STUB
    NumberOfStubs
};
static constexpr unsigned numberOfStubs = static_cast<unsigned>(Stub::NumberOfStubs);
static constexpr unsigned numberOfBlocksOfTypedStrings = 8;
static constexpr unsigned numberOfTypedStringStubsInBlock = 4;
static_assert(static_cast<unsigned>(Stub::TypedStringInBlock7) == static_cast<unsigned>(Stub::TypedStringInBlock1) + numberOfTypedStringStubsInBlock * (numberOfBlocksOfTypedStrings - 2));
static_assert(static_cast<unsigned>(Stub::TypedStringMadeOnceInBlock7) == static_cast<unsigned>(Stub::TypedStringMadeOnceInBlock1) + numberOfTypedStringStubsInBlock * (numberOfBlocksOfTypedStrings - 2));
ASCIILiteral nameOf(Stub);
static constexpr Stub askingReceiver(Stub stub) { return static_cast<Stub>(static_cast<unsigned>(stub) - 1); }
static_assert(askingReceiver(Stub::MapGet) == Stub::MapGetAskingReceiver && askingReceiver(Stub::MapHas) == Stub::MapHasAskingReceiver && askingReceiver(Stub::MapSet) == Stub::MapSetAskingReceiver && askingReceiver(Stub::SetHas) == Stub::SetHasAskingReceiver && askingReceiver(Stub::SetAdd) == Stub::SetAddAskingReceiver && askingReceiver(Stub::MapSetWithStringKey) == Stub::MapSetWithStringKeyAskingReceiver && askingReceiver(Stub::SetAddWithStringKey) == Stub::SetAddWithStringKeyAskingReceiver && askingReceiver(Stub::MapGetWithStringKey) == Stub::MapGetWithStringKeyAskingReceiver && askingReceiver(Stub::MapHasWithStringKey) == Stub::MapHasWithStringKeyAskingReceiver && askingReceiver(Stub::SetHasWithStringKey) == Stub::SetHasWithStringKeyAskingReceiver);
static constexpr unsigned shortLiteralLengthBits = 4;
static constexpr unsigned shortLiteralLengthMask = (1u << shortLiteralLengthBits) - 1;
static constexpr bool isHelper(Stub stub) { return stub >= Stub::HelperNewArray && stub <= Stub::HelperAddStrings; }
static_assert(maxLiteralPropertiesInRegisters <= 6 && maxArrayElementsInRegisters <= 4);
// For code compiled from types. Each is followed by its variant for a caller without a frame.
// What Stub::ArrayOfDoubles and Stub::ArrayOfInt32s are handed: the length, and above it the capacity, which is odd.
static constexpr unsigned capacityShiftInArrayOfNumbers = 8;
static constexpr unsigned arraySiteBornShift = 8;
static constexpr uint32_t arraySiteInterval = 64;
static constexpr unsigned maxCapacityOfArrayOfNumbers = 127;
static constexpr Stub arrayLiteralStub(unsigned count) { return static_cast<Stub>(static_cast<unsigned>(Stub::ArrayLiteral0) + 2 * count); }

static constexpr bool usesStubs =
#if CPU(ARM64) || CPU(X86_64)
    true;
#else
    false;
#endif
inline bool usesDataStubs() { return usesStubs && Options::useAOTDataStubs(); }

struct Site {
    static constexpr unsigned identifierBits = 20;
    static constexpr unsigned extraBits = 32 - identifierBits;
    static bool fits(unsigned identifier, unsigned extra) { return identifier < (1u << identifierBits) && extra < (1u << extraBits); }

    static constexpr unsigned storesLazyClosure = 4;
    static constexpr unsigned isOfRowOfThis = 8;

    uint32_t identifierAndExtra { 0 };
};

struct ImageKey {
    uint32_t module { 0 };
    uint32_t start { 0 };
    uint32_t kind { 0 };
    uint32_t record { 0 };

    static constexpr uint32_t typedBody = 1u << 16;
    static constexpr uint32_t typedFunction = 1u << 17;
    static constexpr uint32_t isEntryInsideFunction = 1u << 31;
    bool sameFunction(const ImageKey& other) const { return module == other.module && start == other.start && kind == other.kind; }
    unsigned hash() const
    {
        uint64_t h = (static_cast<uint64_t>(module) << 32 | start) * 0x9e3779b97f4a7c15ULL;
        h ^= (h >> 29) + kind * 0x85ebca6bU;
        h *= 0xbf58476d1ce4e5b9ULL;
        return static_cast<unsigned>(h >> 32);
    }
};
static_assert(sizeof(ImageKey) == 16);

struct StubCall {
    static constexpr uint32_t noFunction = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t noCallSite = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t noArgumentMoves = std::numeric_limits<uint32_t>::max();
    static constexpr uint64_t noImmediate = std::numeric_limits<uint64_t>::max();

    uint32_t offset;
    Stub stub;
    bool isTailCall;
    uint16_t thunk { 0 };
    uint32_t function { noFunction };
    uint32_t callSite { noCallSite };
    uint32_t argumentMoves { noArgumentMoves };
    uint32_t body { 0 };
};

std::optional<unsigned> thunkFor(Stub, uint32_t t9Value);

enum class ArgumentSource : uint8_t { Register, Stack32, Stack64, Constant };
using ArgumentMoves = Vector<uint64_t, 8>;
inline void appendArgumentMove(ArgumentMoves& moves, GPRReg destination, ArgumentSource source, uint64_t payload)
{
    moves.append(static_cast<uint64_t>(source) | static_cast<uint64_t>(destination) << 8);
    moves.append(payload);
}
struct ArgumentThunks {
    Vector<uint8_t> bytes;
    Vector<uint32_t> offsets;
    Vector<StubCall> jumps;
};
ArgumentThunks generateArgumentThunks(const Vector<const ArgumentMoves*>&);
struct OutlinedPlace {
    uint32_t offset;
    uint32_t length;
    uint32_t body;
};
struct OutlinedBody {
    uint32_t function;
    uint32_t offset;
    uint32_t length;
    uint32_t numberOfPlaces;
    uint32_t onlyCaller;
    bool endsInCall;
};
struct Outlining {
    static constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
    Vector<Vector<OutlinedPlace>> places;
    Vector<OutlinedBody> bodies;
};
struct OutlinedBodies {
    static constexpr uint32_t noJump = std::numeric_limits<uint32_t>::max();
    Vector<uint8_t> bytes;
    Vector<uint32_t> offsets;
    Vector<StubCall> jumps;
    size_t sizeOf(uint32_t number) const { return (number + 1 < offsets.size() ? offsets[number + 1] : bytes.size()) - offsets[number]; }
};

void emitCharactersOfSubstring(CCallHelpers&);
GPRReg defaultOperandRegister(Stub);
bool preservesOperandRegister(Stub);
std::optional<RegisterSet> registersChangedBy(Stub);
void dumpRegistersChangedByStubs(PrintStream&);

struct StubBlob {
    Vector<uint8_t> bytes;
    unsigned offsets[numberOfStubs];
    Vector<unsigned> thunkOffsets;
    Vector<unsigned> returnsIntoAdapters;
    Vector<unsigned> returnsFromFindingTargets;
    Vector<std::pair<String, unsigned>> names;
};
const StubBlob& stubBlob(unsigned codeGranuleShift, bool functionLengthsAreBytes);

struct IndexReference {
    uint32_t offset;
    uint32_t addend;
    uint16_t scale;
};
class IndexReferences {
public:
    void load(CCallHelpers&, GPRReg base, GPRReg dest, uint32_t addend, uint32_t scale);
    void load16(CCallHelpers&, GPRReg base, GPRReg dest, uint32_t addend, uint32_t scale);
    Vector<IndexReference> link(LinkBuffer&);
    static void fill(uint8_t* code, const IndexReference&, uint32_t index);

private:
    struct Reference {
        CCallHelpers::Label instructions;
        uint32_t addend;
        uint32_t scale;
    };
    Vector<Reference, 2> m_references;
};

struct ListDescriptor {
    enum Kind : uint32_t {
        Value,
        Spread,
        Passed,
    };
    static constexpr unsigned maxItems = 13;
    static_assert(5 + 2 * maxItems <= 32 && maxItems < 16);

    uint32_t firstVarArg() const { return bits >> 1; }
    Kind kindOf(unsigned index) const { return static_cast<Kind>(bits >> (5 + 2 * index) & 3); }

    uint32_t bits;
};
static constexpr unsigned maxListItems = ListDescriptor::maxItems;

struct PackedSite {
    static constexpr unsigned shift = 24;
    static constexpr unsigned maxInlineFrames = 254;
    static constexpr uint32_t isTailCall = 1u << (shift - 1);
    static bool fits(uint32_t bits) { return !(bits >> (shift - 1)); }
    static uint32_t pack(unsigned inlineFrame, uint32_t bits, bool tailCall = false)
    {
        RELEASE_ASSERT(inlineFrame <= maxInlineFrames && fits(bits));
        return inlineFrame << shift | (tailCall ? isTailCall : 0) | bits;
    }
    static unsigned inlineFrame(uint32_t site) { return site >> shift; }
    static uint32_t bits(uint32_t site) { return site & (isTailCall - 1); }
    static bool isTailCallSite(uint32_t site) { return site & isTailCall; }
};

struct SpreadSite {
    uint32_t callSite;
    uint32_t item;
    uint32_t site;
};

struct CallSite {
    uint32_t bits { StubCall::noCallSite };
};

#if CPU(ARM64)
static constexpr GPRReg functionIndexGPR = ARM64Registers::x15;
static constexpr GPRReg throwWordGPR = ARM64Registers::x16;
#elif CPU(X86_64)
static constexpr GPRReg functionIndexGPR = X86Registers::r10;
static constexpr GPRReg throwWordGPR = X86Registers::r11;
#endif
#if CPU(ARM64) || CPU(X86_64)
void loadFunctionIndexAt(CCallHelpers&, GPRReg pc);
#endif

class StubCalls {
public:
    void call(CCallHelpers&, Stub, CallSite);
    void call(CCallHelpers&, Stub, uint32_t t9Value, CallSite);
    void call(CCallHelpers&, Stub, uint32_t t9Value, CallSite, const ArgumentMoves&);
    void call(CCallHelpers&, Stub, CallSite, const ArgumentMoves&);
    void tailCall(CCallHelpers&, Stub);
    void tailCall(CCallHelpers&, Stub, uint32_t t9Value);
    void callFunction(CCallHelpers&, uint32_t knownCallee, CallSite);
    void jumpToFunction(CCallHelpers&, uint32_t knownCallee);
    Vector<StubCall> link(LinkBuffer&);
    Vector<ArgumentMoves> takeArgumentMoves() { return std::exchange(m_argumentMoves, { }); }

private:
    struct Pending {
        CCallHelpers::Call call;
        Stub stub;
        bool isTailCall;
        uint32_t callSite;
        uint32_t function { StubCall::noFunction };
        uint16_t thunk { 0 };
        uint32_t argumentMoves { StubCall::noArgumentMoves };
    };
    Vector<Pending, 8> m_pending;
    Vector<ArgumentMoves> m_argumentMoves;
};

void retargetStubCall(uint8_t* base, size_t instruction, size_t target, bool isTailCall);
#if CPU(X86_64)
static constexpr size_t sizeOfJump = 5;
static constexpr size_t sizeOfNearCall = 5;
static constexpr size_t codeOffsetUnit = 1;
static constexpr size_t stubCallReach = 1536 * MB;
#else
static constexpr size_t sizeOfJump = sizeof(uint32_t);
static constexpr size_t sizeOfNearCall = sizeof(uint32_t);
static constexpr size_t codeOffsetUnit = sizeof(uint32_t);
static constexpr size_t stubCallReach = 96 * MB;
#endif
static constexpr size_t offsetOfReturnAddressInTrap = sizeOfNearCall - 1;
static constexpr size_t sizeOfVeneer = 3 * sizeof(uint32_t);
static constexpr size_t stubCallReachSlack = 24 * MB;
void writeVeneer(uint8_t* base, size_t veneer, size_t target);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
