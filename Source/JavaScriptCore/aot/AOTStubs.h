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
    v(NewObject) \
    v(NewObjectLiteral) \
    v(CreateThis) \
    v(CreateThisWithProperties) \
    v(NewFunction) \
    v(CompareStrictEq) \
    v(CompareEq) \
    v(InById) \
    v(InByVal) \

#define FOR_EACH_AOT_STUB_INTRINSIC(v) \
    v(CharCodeAt, "charCodeAt", 1, Any) \
    v(CodePointAt, "codePointAt", 1, Any) \
    v(CharAt, "charAt", 1, Any) \
    v(Push, "push", 1, Any) \
    v(Pop, "pop", 0, Any) \
    v(IsArray, "isArray", 1, Any) \
    v(Get, "get", 1, Any) \
    v(Has, "has", 1, Any) \
    v(Set, "set", 2, Wanted) \
    v(SetIgnoringResult, "set", 2, ResultUnused) \
    v(Add, "add", 1, Wanted) \
    v(AddIgnoringResult, "add", 1, ResultUnused) \
    v(Slice, "slice", 1, Any) \
    v(SliceWithEnd, "slice", 2, Any) \

enum class StubIntrinsic : uint8_t {
    None,
#define AOT_DEFINE_STUB_INTRINSIC(name, text, argumentCount, result) name,
    FOR_EACH_AOT_STUB_INTRINSIC(AOT_DEFINE_STUB_INTRINSIC)
#undef AOT_DEFINE_STUB_INTRINSIC
    NumberOfStubIntrinsics
};
static constexpr unsigned numberOfStubIntrinsics = static_cast<unsigned>(StubIntrinsic::NumberOfStubIntrinsics) - 1;
StubIntrinsic stubIntrinsicFor(UniquedStringImpl* name, unsigned argumentCountIncludingThis, bool usesResult);

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

static constexpr unsigned codeGranuleShift = 10;

#define FOR_EACH_AOT_OPERATION_BEHIND_HELPER(v) \
    v(NewArray, operationAOTNewArray) \
    v(NewArrayBuffer, operationAOTNewArrayBuffer) \
    v(NewArrayWithSpread, operationAOTNewArrayWithSpread) \
    v(NewArrayWithSpecies, operationAOTNewArrayWithSpecies) \
    v(NewArrayWithSize, operationAOTNewArrayWithSize) \
    v(CreateRest, operationAOTCreateRest) \
    v(CreateLexicalEnvironment, operationAOTCreateLexicalEnvironment) \
    v(NewInternalFieldObject, operationAOTNewInternalFieldObject) \
    v(NewResolvedPromise, operationNewResolvedPromise) \
    v(NewMapOrSet, operationAOTNewMapOrSet) \
    v(ToString, operationAOTToString) \
    v(Int32ToStringWithValidRadix, operationInt32ToStringWithValidRadix) \
    v(MakeRope2, operationMakeRope2) \
    v(MakeRope3, operationMakeRope3) \
    v(StringSliceWithEnd, operationStringSliceWithEnd) \
    v(StringSubstringWithEnd, operationStringSubstringWithEnd) \
    v(ObjectKeysObject, operationObjectKeysObject) \
    v(ValueAdd, operationAOTValueAdd) \
    v(Strcat, operationAOTStrcat) \

#define FOR_EACH_AOT_HELPER(v) \
    v(HelperNewArray) \
    v(HelperNewInt32Array) \
    v(HelperNewArrayBuffer) \
    v(HelperNewActivation) \
    v(HelperNewPromise) \
    v(HelperNewResolvedPromise) \
    v(HelperNewGenerator) \
    v(HelperNewAsyncFunctionGenerator) \
    v(HelperNewMap) \
    v(HelperNewSet) \
    v(HelperNewArrayWithSpread) \
    v(HelperNewArrayWithSpecies) \
    v(HelperNewArrayWithSize) \
    v(HelperStringSlice) \
    v(HelperStringSubstring) \
    v(HelperMakeRope2) \
    v(HelperMakeRope3) \
    v(HelperInt32ToString) \
    v(HelperObjectKeys) \
    v(HelperAddField) \
    v(HelperSetArrayLength) \
    v(HelperStrcat) \
    v(HelperAddStrings) \

#define FOR_EACH_AOT_STUB(v) \
    v(Prologue) \
    v(ReturnFromCallWithList) \
    v(Call) \
    v(Construct) \
    v(CallCached) \
    v(CallHostFunction) \
    v(CallInternalFunction) \
    v(ConstructInternalFunction) \
    v(CallList) \
    v(ConstructList) \
    v(CallVarargs) \
    v(ConstructVarargs) \
    v(TailCallVarargs) \
    v(TailCallList) \
    v(CallIntrinsic) \
    v(Enter) \
    v(EnterModule) \
    v(EnterFunctionForCall) \
    v(EnterFunctionForConstruct) \
    v(EnterStaticFunctionForCall) \
    v(EnterStaticFunctionForConstruct) \
    v(ConstructViaCall) \
    v(CallBoundFunction) \
    v(LinkFunction) \
    v(Constant) \
    v(TemplateObject) \
    v(TransientConstant) \
    v(ConstantFromSlot) \
    v(IsStringEqualToConstant) \
    v(IsStringEqualToLiteral1) \
    v(IsStringEqualToLiteral2To3) \
    v(IsStringEqualToLiteral4To7) \
    v(IsStringEqualToLiteral8) \
    v(IsStringEqualToLiteral9To16) \
    v(OperationValue) \
    v(OperationVoid) \
    v(OperationDouble) \
    v(OperationValueWithGlobalObject) \
    v(OperationVoidWithGlobalObject) \
    v(OperationDoubleWithGlobalObject) \
    v(OperationValueWithInstance) \
    v(OperationVoidWithInstance) \
    v(OperationDoubleWithInstance) \
    v(ColdOperationVoid) \
    v(LeafColdOperationVoid) \
    v(ColdOperationValue) \
    v(LeafColdOperationValue) \
    v(PlainOperation) \
    v(PlainOperationWithGlobalObject) \
    v(PlainOperationWithInstance) \
    v(PlainOperationWithVM) \
    v(WriteBarrier) \
    v(ToBoolean) \
    v(StrictEqual) \
    v(LooseEqual) \
    v(Add) \
    v(IsStringEqualTo) \
    v(Latin1Characters) \
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
    v(GetByValAtIndex) \
    v(GetByVal) \
    v(PutByValAtIndex) \
    v(PutByVal) \
    v(PutByValDirect) \
    v(GetLength) \
    v(GetGlobal) \
    v(GetById) \
    v(ReadSlot0) \
    v(ReadSlot1) \
    v(ReadSlot2) \
    v(ReadSlot3) \
    v(ReadSlot4) \
    v(ReadSlot5) \
    v(ReadSlot6) \
    v(ReadSlot7) \
    v(ReadSlot8) \
    v(ReadSlot9) \
    v(ReadSlot10) \
    v(ReadSlot11) \
    v(ReadSlot12) \
    v(ReadSlot13) \
    v(ReadSlot14) \
    v(ReadSlot15) \
    v(ReadSlotOrUndefined0) \
    v(ReadSlotOrUndefined1) \
    v(ReadSlotOrUndefined2) \
    v(ReadSlotOrUndefined3) \
    v(ReadSlotOrUndefined4) \
    v(ReadSlotOrUndefined5) \
    v(ReadSlotOrUndefined6) \
    v(ReadSlotOrUndefined7) \
    v(ReadSlotOrUndefined8) \
    v(ReadSlotOrUndefined9) \
    v(ReadSlotOrUndefined10) \
    v(ReadSlotOrUndefined11) \
    v(ReadSlotOrUndefined12) \
    v(ReadSlotOrUndefined13) \
    v(ReadSlotOrUndefined14) \
    v(ReadSlotOrUndefined15) \
    v(PutById) \
    v(GetPrivateName) \
    v(CheckPrivateBrand) \
    v(PutPrivateName) \
    v(DefinePrivateName) \
    v(SetPrivateBrand) \
    v(ResolveScope) \
    v(GetFromScope) \
    v(PutToScope) \
    v(GetByIdWellKnown) \
    v(InstanceOf) \
    v(InstanceOfCached) \
    v(IteratorNext) \
    v(IteratorOpen) \
    v(IteratorCloseCheck) \
    v(MapGet) \
    v(MapHas) \
    v(MapSet) \
    v(SetHas) \
    v(SetAdd) \
    v(WeakMapGet) \
    v(WeakMapHas) \
    v(WeakSetHas) \
    v(ArrayIncludes) \
    v(ArrayIndexOf) \
    v(ToLowerCase) \
    v(ToUpperCase) \
    v(HandleException) \
    v(ThrowStackOverflowAtPrologue) \
    v(ThrowStackOverflow) \
    v(ThrowCalledIndirectly) \
    v(Catch) \
    v(VirtualCall) \
    v(VirtualConstruct) \
    v(VirtualTailCall) \
    v(FrontEndGetById) \
    v(FrontEndGetByVal) \
    v(FrontEndPutById) \
    v(FrontEndPutByVal) \
    v(FrontEndNewObject) \
    v(FrontEndNewObjectLiteral) \
    v(FrontEndCreateThis) \
    v(FrontEndCreateThisWithProperties) \
    v(FrontEndNewFunction) \
    v(FrontEndCompareStrictEq) \
    v(FrontEndCompareEq) \
    v(FrontEndInById) \
    v(FrontEndInByVal) \
    FOR_EACH_AOT_HELPER(v) \
    v(NewArrayWithFastPath) \
    v(NewArrayBufferWithFastPath) \
    v(NewArrayWithSpreadWithFastPath) \
    v(NewArrayWithSpeciesWithFastPath) \
    v(NewArrayWithSizeWithFastPath) \
    v(CreateRestWithFastPath) \
    v(CreateLexicalEnvironmentWithFastPath) \
    v(NewInternalFieldObjectWithFastPath) \
    v(NewResolvedPromiseWithFastPath) \
    v(NewMapOrSetWithFastPath) \
    v(ToStringWithFastPath) \
    v(Int32ToStringWithValidRadixWithFastPath) \
    v(MakeRope2WithFastPath) \
    v(MakeRope3WithFastPath) \
    v(StringSliceWithEndWithFastPath) \
    v(StringSubstringWithEndWithFastPath) \
    v(ObjectKeysObjectWithFastPath) \
    v(ValueAddWithFastPath) \
    v(StrcatWithFastPath) \
    v(NewObjectLiteral1) \
    v(NewObjectLiteral2) \
    v(NewObjectLiteral3) \
    v(NewObjectLiteral4) \
    v(NewObjectLiteral5) \
    v(NewObjectLiteral6) \
    v(NewArrayLiteral1) \
    v(NewArrayLiteral2) \
    v(NewArrayLiteral3) \
    v(NewArrayLiteral4) \
    v(NewInt32ArrayLiteral1) \
    v(NewInt32ArrayLiteral2) \
    v(NewInt32ArrayLiteral3) \
    v(NewInt32ArrayLiteral4) \

enum class Stub : uint8_t {
#define AOT_DEFINE_STUB(name) name,
    FOR_EACH_AOT_STUB(AOT_DEFINE_STUB)
#undef AOT_DEFINE_STUB
    NumberOfStubs
};
static constexpr unsigned numberOfStubs = static_cast<unsigned>(Stub::NumberOfStubs);
ASCIILiteral nameOf(Stub);
static constexpr unsigned shortLiteralLengthBits = 4;
static constexpr unsigned shortLiteralLengthMask = (1u << shortLiteralLengthBits) - 1;
static constexpr bool isHelper(Stub stub) { return stub >= Stub::HelperNewArray && stub <= Stub::HelperAddStrings; }
static_assert(maxLiteralPropertiesInRegisters <= 6 && maxArrayElementsInRegisters <= 4);
static constexpr Stub newObjectLiteralStub(unsigned count) { return static_cast<Stub>(static_cast<unsigned>(Stub::NewObjectLiteral1) + count - 1); }
static constexpr Stub newArrayLiteralStub(unsigned count, bool areInt32) { return static_cast<Stub>(static_cast<unsigned>(areInt32 ? Stub::NewInt32ArrayLiteral1 : Stub::NewArrayLiteral1) + count - 1); }

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

    static constexpr unsigned resolvesInGlobalScopes = (1u << extraBits) - 1;
    static constexpr unsigned throwsIfNotFound = 1;
    static constexpr unsigned isImport = 2;
    static constexpr uint32_t isCalleeCache = 0xfff00000;

    uint32_t identifierAndExtra { 0 };
};

struct KnownShape {
    unsigned inlineCapacity { 0 };
    Vector<UniquedStringImpl*, 8> names;
    uint32_t number { 0 };
    Vector<uint16_t, 8> slots;
    uint16_t layoutID { 0 };
    uint16_t reserved { 0 };
    uint16_t inlineSlots { 0 };
    bool hasSlotsOutside() const { return layoutID && reserved > inlineSlots; }
    unsigned numberOfSlots() const { return slots.isEmpty() ? names.size() : std::max<unsigned>(*std::ranges::max_element(slots) + 1, reserved); }

    static constexpr unsigned maxProperties = 4096;
    static unsigned inlineCapacityFor(unsigned numberOfProperties);
};

struct ImageKey {
    uint32_t module { 0 };
    uint32_t start { 0 };
    uint32_t kind { 0 };
    uint32_t record { 0 };

    static constexpr uint32_t typedBody = 1u << 16;
    ImageKey ofPublicBody() const
    {
        ImageKey result = *this;
        result.kind &= ~typedBody;
        return result;
    }
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

    uint32_t offset;
    Stub stub;
    bool isTailCall;
    uint16_t thunk { 0 };
    uint32_t function { noFunction };
    uint32_t callSite { noCallSite };
};

std::optional<unsigned> thunkFor(Stub, uint32_t t9Value);

bool acceptsOperandInAnyRegister(Stub, std::optional<uint32_t> t9Value);
GPRReg defaultOperandRegister(Stub);
bool operandAllowedInRegister(Stub, GPRReg);
bool preservesOperandRegister(Stub);
std::optional<RegisterSet> registersChangedBy(Stub);
void dumpRegistersChangedByStubs(PrintStream&);
unsigned thunkForOperandRegister(Stub, std::optional<uint32_t> t9Value, GPRReg);
bool acceptsTwoOperandsInAnyRegisters(Stub);
bool returnsResultInAnyRegister(Stub, std::optional<uint32_t> t9Value);

struct StubBlob {
    Vector<uint8_t> bytes;
    unsigned offsets[numberOfStubs];
    Vector<unsigned> thunkOffsets;
    Vector<unsigned> returnsIntoAdapters;
    Vector<std::pair<String, unsigned>> names;
};
const StubBlob& stubBlob();

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
    static constexpr uint32_t forList(uint32_t firstVarArg) { return firstVarArg << 1; }
    static constexpr uint32_t forItems(uint32_t count) { return 1 | count << 1; }
    static constexpr uint32_t itemKind(unsigned index, Kind kind) { return static_cast<uint32_t>(kind) << (5 + 2 * index); }
    static_assert(5 + 2 * maxItems <= 32 && maxItems < 16);

    bool describesItems() const { return bits & 1; }
    uint32_t firstVarArg() const { return bits >> 1; }
    unsigned numberOfItems() const { return bits >> 1 & 15; }
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
#elif CPU(X86_64)
static constexpr GPRReg functionIndexGPR = X86Registers::r10;
#endif
#if CPU(ARM64) || CPU(X86_64)
void loadFunctionIndexAt(CCallHelpers&, GPRReg pc);
#endif

class StubCalls {
public:
    void call(CCallHelpers&, Stub, CallSite);
    void call(CCallHelpers&, Stub, uint32_t t9Value, CallSite);
    void callWithOperandInRegister(CCallHelpers&, Stub, std::optional<uint32_t> t9Value, GPRReg, CallSite);
    void callWithOperandsInRegisters(CCallHelpers&, Stub, GPRReg first, GPRReg second, CallSite);
    void callWithResultInRegister(CCallHelpers&, Stub, uint32_t t9Value, GPRReg operand, GPRReg result, CallSite);
    void tailCall(CCallHelpers&, Stub);
    void tailCall(CCallHelpers&, Stub, uint32_t t9Value);
    void callFunction(CCallHelpers&, uint32_t knownCallee, CallSite);
    void jumpToFunction(CCallHelpers&, uint32_t knownCallee);
    Vector<StubCall> link(LinkBuffer&);

private:
    struct Pending {
        CCallHelpers::Call call;
        Stub stub;
        bool isTailCall;
        uint32_t callSite;
        uint32_t function { StubCall::noFunction };
        uint16_t thunk { 0 };
    };
    Vector<Pending, 8> m_pending;
};

void retargetStubCall(uint8_t* base, size_t instruction, size_t target, bool isTailCall);
#if CPU(X86_64)
static constexpr size_t sizeOfNearCall = 5;
static constexpr size_t codeOffsetUnit = 1;
static constexpr size_t stubCallReach = 1536 * MB;
#else
static constexpr size_t sizeOfNearCall = sizeof(uint32_t);
static constexpr size_t codeOffsetUnit = sizeof(uint32_t);
static constexpr size_t stubCallReach = 96 * MB;
#endif
static constexpr size_t sizeOfVeneer = 3 * sizeof(uint32_t);
static constexpr size_t stubCallReachSlack = 24 * MB;
void writeVeneer(uint8_t* base, size_t veneer, size_t target);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
