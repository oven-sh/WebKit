/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypedIR.h"

namespace JSC {

class VM;

namespace B3 {
class ProcedureFingerprint;
}

namespace AOT {

class ImageBuilder;
struct CompiledCode;
struct ImageKey;

namespace Typed {

Vector<String> optimize(Program&);

void prepare(Program&);
void expandDisposableStacks(Program&);
Vector<std::pair<FunctionID, Function>> splitCoroutines(Program&);
void splitOptimizedGenerators(Program&, Vector<std::pair<FunctionID, Function>>&&);
void recordCellsLiveAcrossAwaits(const Program&, Function&);
void awaitDirectCallsWithoutPromises(Program&);
BitVector objectsLiveAcrossSuspensions(const Function&);
BitVector blocksInLoops(const Function&);
BitVector blocksInLoops(const Functions&, size_t index);
constexpr unsigned notInCycle = UINT_MAX;
Vector<unsigned> computeBlockCycles(const Function&);
Vector<unsigned> computeBlockCycles(const Function&, Vector<unsigned>* componentRank);
bool canUseConstructBody(const Program&, FunctionID, const Function&, const Value&);
void createGenericEntrypoints(Program&);
void createValueEntrypoints(Program&);
void createResultAdapters(Program&);
void clearCallerCheckedResultTypes(Program&);
void resolveCallsBySignature(Program&);
void createJSONSerializers(Program&);
void planStringAppends(Program&, Vector<String>& findings);
void specializeConstantFormatters(Program&);
void findDeclaredSuperclasses(Program&);
void createBoundFunctions(Program&);
void assignAllocations(Program&);
void assignAllocations(const Program&, Function&);
void findRetagTargets(Program&);
void linkAllocations(Program&);
void computeEffectSummaries(Program&);
void place(Program&);
Vector<uint8_t> encodeData(const Program&);

template<typename NodeAt, typename Predecessors>
bool tryFindImmediateDominators(unsigned count, const NodeAt& nodeAt, const Predecessors& predecessors, Vector<unsigned>& dominator)
{
    static constexpr unsigned none = UINT_MAX;
    if (count <= 2048)
        return false;
    unsigned root = nodeAt(0);
    Vector<unsigned> depth(FillWith { }, dominator.size(), 0u);
    Vector<unsigned> jump(FillWith { }, dominator.size(), root);
    auto intersect = [&](unsigned a, unsigned b) {
        if (depth[a] < depth[b])
            std::swap(a, b);
        while (depth[a] > depth[b])
            a = depth[jump[a]] >= depth[b] ? jump[a] : dominator[a];
        while (a != b) {
            bool jumps = jump[a] != jump[b];
            a = jumps ? jump[a] : dominator[a];
            b = jumps ? jump[b] : dominator[b];
        }
        return a;
    };
    auto find = [&](unsigned node) {
        unsigned found = none;
        for (unsigned other : predecessors[node]) {
            if (dominator[other] != none)
                found = found == none ? other : intersect(found, other);
        }
        return found;
    };
    bool isRight = true;
    for (unsigned i = 1; i < count && isRight; ++i) {
        unsigned node = nodeAt(i);
        unsigned found = find(node);
        if (found == none) {
            isRight = false;
            break;
        }
        dominator[node] = found;
        depth[node] = depth[found] + 1;
        unsigned middle = jump[found];
        jump[node] = depth[found] - depth[middle] == depth[middle] - depth[jump[middle]] ? jump[middle] : found;
    }
    for (unsigned i = 1; i < count && isRight; ++i)
        isRight = find(nodeAt(i)) == dominator[nodeAt(i)];
    if (isRight)
        return true;
    dominator.fill(none, dominator.size());
    dominator[root] = root;
    return false;
}

enum class VerifierScope : uint8_t { Everything, EverythingButDefinitions, DefinitionsOnly };
void verify(const Program&, FunctionID, const Function&, Vector<String>& findings, VerifierScope = VerifierScope::Everything);
void verifyRegExpLiterals(const Program&, Vector<String>& findings);
void cleanUp(const Program&, Function&);
void compactBlocks(Function&);
void cleanUpIfFoldable(const Program&, Function&);
enum class CleanUpTiming : bool { Immediate, Deferred };
bool threadForInOwnKeys(const Program&, Function&);
unsigned flattenIteratedObjectEntries(const Program&, Function&);
unsigned fuseOwnPropertyDescriptorCopies(const Program&, Function&);
unsigned splitDefinePropertiesOverLiterals(const Program&, Function&);
unsigned hoistDefinePropertyIntoPredecessors(Function&);
void hoistAllocationsIntoPredecessors(const Program&, Function&);
void removeTrivialParameters(const Program&, Function&);
void removeUnusedValues(const Program&, Function&);
bool removeDeadCountedLoops(const Program&, Function&);
bool sinkToBooleanIntoPredecessors(const Program&, Function&);
void convertAllocationAssertions(Program&, Vector<String>& findings);
void reportAllocationAssertions(const Program&, Vector<String>& findings);
void recordAllocation(const Program&, Function&, const Value& site, AllocationOutcome, unsigned builtAt = 0, const Value* forcedBy = nullptr, ASCIILiteral why = { });
void recordAllocationSites(const Program&, Function&);
void finalizeAllocationNotes(const Program&, Function&);
void printAllocationRemarks(const Program&, PrintStream&);
void printAllocationUses(const Program&, PrintStream&);

static constexpr unsigned stringBuilderCapacity = 128;
static constexpr unsigned maxStringBuilderAppendRun = 64;
static constexpr size_t maxStringBuilderInlineConstantLength = 16;

struct StringBuilderAnalysis {
    static constexpr uint32_t invalidBuilder = UINT32_MAX;
    using Operands = Vector<ValueID, 8>;

    bool isState(ValueID id) const { return id.index() < builderForValue.size() && builderForValue[id.index()] != invalidBuilder; }

    Vector<uint32_t> builderForValue;
    BitVector isFinalState;
    UncheckedKeyHashMap<uint32_t, Operands, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> appendOperands;
    BitVector isFoldedIntoAppend;
    BitVector mayBeEmpty;
    UncheckedKeyHashMap<uint32_t, StringID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> initialString;
    unsigned builderCount { 0 };
    Vector<std::pair<ValueID, ASCIILiteral>> rejections;
};
StringBuilderAnalysis analyzeStringBuilders(const Program&, FunctionID, const Function&);
void dumpStringBuilderRemarks(const Program&, PrintStream&);
BitVector computeMapKeyValues(const Program&, FunctionID, const Function&);
struct SplitOnlyTakenApart {
    static constexpr unsigned maxFields = 16;
    unsigned numberOfPositions { 0 };
    bool countsAllSeparators { false };
    bool isPopped { false };
    ValueID iterator { };
    unsigned numberOfUses { 0 };
    StringID separator { };
};
using SplitsOnlyTakenApart = UncheckedKeyHashMap<uint32_t, SplitOnlyTakenApart, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
std::optional<unsigned> fieldReadAtConstantIndex(const Program&, const Function&, const Value&);
bool isReadOfLastElement(const Program&, const Function&, const Value&);
SplitsOnlyTakenApart findSplitsOnlyTakenApart(const Program&, const Function&);
using MatchesOnlyTakenApart = UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
enum class MatchProperty : uint8_t { Index, Input };
std::optional<MatchProperty> matchPropertyRead(const Program&, const Value&);
MatchesOnlyTakenApart findMatchesOnlyTakenApart(const Program&, const Function&);
ValueID matchBehindCasts(const Function&, ValueID);
void computeFunctionsReturningMapKeys(Program&);
void findCalledOnlyOperands(Program&);
void verifyCalledOnlyParameters(const Program&, Vector<String>& findings);
void findStackAllocatedClosures(Program&);
void findStackAllocatedEnvironments(Program&, Vector<String>& findings);
void reuseEnvironments(Program&);
std::optional<bool> areEqualConstants(const Program&, const Value&, const Value&, bool isStrict);
bool eliminateRedundantReads(const Program&, Function&);
bool hoistReadsOutOfLoops(const Program&, Function&);
enum class AllocationSinkingPolicy : uint8_t { EliminateOnly, AllowMaterialization };
bool sinkObjectAllocations(const Program&, Function&, AllocationSinkingPolicy);
BlockID splitEdge(Function&, BlockID from, unsigned targetIndex);
unsigned resolveCalls(const Program&, Function&, bool mayGuess, CleanUpTiming = CleanUpTiming::Immediate);
FunctionID functionPinnedToField(const Program&, const Type& receiver, StringID name);
bool keepsFunctionPinned(const Program&, const Function&, const Field&, ValueID object, ValueID stored);
bool replacesPinnedFunction(const Program&, const Function&, LayoutID created, const Value& store);
unsigned inlineCalls(const Program&, FunctionID, Function&, CleanUpTiming = CleanUpTiming::Immediate);
void internNamesForGeneratorLoopFusion(Program&);
unsigned fuseGeneratorLoops(const Program&, Function&);
unsigned stepGeneratorsInPlace(const Program&, Function&);
void prepareForInlining(Program&);
unsigned simplifyCasts(const Program&, Function&, CleanUpTiming = CleanUpTiming::Immediate);
unsigned convertCountersToIntegers(const Program&, Function&);
unsigned removeRedundantCatchBlocks(const Program&, Function&);
unsigned resolvePreludeCalls(const Program&, Function&, CleanUpTiming = CleanUpTiming::Immediate);
enum class UnprovenReceiverPolicy : bool { Wait, Speculate };
unsigned callGuardedPreludeEntries(const Program&, FunctionID, Function&, UnprovenReceiverPolicy);
unsigned speculateOnReceivers(const Program&, FunctionID, Function&);
unsigned eliminateGuardsOnUnescapedReceivers(const Program&, Function&);
void eliminateGuardsOnFreshResults(Program&);
void compareHeaderNamesWithPassedConstants(Program&);
void eliminateGuardsOnUnescapedModuleVariables(Program&);
void resolveBuiltins(const Program&, Function&);
void sinkEmptyArrayLiterals(Function&);
void convertCallIntrinsicToCall(Value&);

bool mayThrow(const Program&, const Function&, const Value&);
bool mayWalkPrototypeChain(const Program&, const Function&, const Value&);
bool guaranteesResultType(const Program&, const Function&, const Value&);
FunctionID findMethod(const Program&, LayoutID, StringID, MethodKind);
struct FieldAccess {
    LayoutID layout;
    uint32_t field;
};
std::optional<FieldAccess> fieldOf(const Program&, const Type& receiver, StringID name);
class IntegralNumberAnalysis {
public:
    IntegralNumberAnalysis(const Program& program, const Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    bool contains(ValueID id) { return contains(id, 8); }

private:
    bool contains(ValueID, unsigned depth);

    const Program& m_program;
    const Function& m_function;
    bool m_hasFoundIncoming { false };
    UncheckedKeyHashMap<unsigned, Vector<ValueID, 2>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_incoming;
    Vector<ValueID, 8> m_assumed;
};
bool mayInherit(const Program&, LayoutID supertype, StringID name, bool isForStore = false);
bool hasMethodField(const Program&, LayoutID subtype, StringID name);
bool storeMayConsultPrototypeChain(const Program&, const Function&, const Block&, size_t index, LayoutID supertype, StringID name);
bool mayDespecialize(const Program&, LayoutID supertype);
bool storeMayDespecializeObject(const Program&, const Function&, const Block&, size_t index, LayoutID supertype);
bool isOnlyInExtraProperties(const Program&, LayoutID supertype, StringID name);
bool inheritsThroughLayoutPrototype(const Program&, LayoutID supertype, StringID name, bool isForStore = false);
bool isOnObjectPrototype(const Program&, StringID name);
bool mayInheritAtCreation(const Program&, const Function&, ValueID object, StringID name);
FunctionID constructedBy(const Program&, const Function&, ValueID object);
bool hasUnknownPrototypeAtCreation(const Program&, const Function&, ValueID object);
bool prototypeChainLacksProperty(const Program&, LayoutID supertype, StringID name, bool forObjectWithoutExtraProperties = false);
bool prototypeChainLacksPropertyForSubtypes(const Program&, LayoutID supertype, StringID name);
void indexLayoutSupertypes(Program&);
bool subtypesStoreOnlyDeclaredType(const Program&, LayoutID supertype, uint32_t field);
bool storeMayTransition(const Program&, LayoutID supertype, StringID name);
void setInitializedFields(const Layout&, Allocation&);
bool isInitializingStore(const Program&, const Function&, const Block&, size_t index);
class ImmutableFunctionScope {
    WTF_MAKE_NONCOPYABLE(ImmutableFunctionScope);
public:
    ImmutableFunctionScope();
    ~ImmutableFunctionScope();

private:
    friend bool isInitializingStore(const Program&, const Function&, const Block&, size_t index);

    ImmutableFunctionScope* m_outerScope;
    const Block* m_initializingStoresBlock { nullptr };
    ValueID m_initializingStoresObject;
    size_t m_lastInitializingStore { 0 };
};
struct FreshResults {
    bool isArray { false };
    LayoutID layout;
    Vector<StringID, 4> names;
    Vector<TypeID, 4> types;
    Vector<std::pair<unsigned, Vector<ValueID, 4>>, 2> returns;
    uint8_t nullishKind { 0 };
    Vector<unsigned, 2> nullishReturns;
};
bool scalarizeNullableResult(const Program&, Function&, ValueID call);
std::optional<FreshResults> computeFreshResults(const Program&, const Function&);
struct FreshResultReads {
    Vector<ValueID, 4> aliases;
    Vector<std::pair<ValueID, uint32_t>, 8> reads;
    Vector<ValueID, 2> nullishTests;
};
std::optional<FreshResultReads> computeFreshResultReads(const Program&, const Function&, ValueID call, const FreshResults&);
bool isLocalAllocation(const Function&, ValueID);
std::span<const ValueID> initialStoresOf(const Function&);
Writes writesOf(const Program&, const Function&, const Block&, size_t index);
TypeID widestStoredFieldType(const Program&, LayoutID supertype, StringID name);
std::optional<LanguageOperation> languageOperationOf(const Program&, const Value&);
struct UserInBlock {
    unsigned block;
    ValueID value;
};
using UsersInBlocks = Vector<UserInBlock, 4>;
void collectAssignedProperties(const Program&, const Function&, ValueID object, unsigned depth, Vector<StringID, 16>& names, unsigned& numberOfKeys, const UsersInBlocks* users = nullptr, bool followsPrototypeChain = false);
bool collectPropertiesAssignedToThis(const Program&, FunctionID, Vector<StringID, 16>& names, unsigned& numberOfKeys, bool followsCallsOnThis = true);
Writes writesOfCast(const Program&, const Function&, const Value&);
Writes writesOfElementRead(const Program&, const Type&);
bool coercionMayMutateObjects(const Program&, const Type& from, const Type& coercedTo, bool isFreshData);
bool narrowingMayRetag(const Program&, const Type&);
bool readMayInstantiate(const Program&, const Type&);
bool castWritesShapeWords(const Program&, const Function&, const Value&);
bool claimMayAllocate(const Program&, const Function&, const Value&);
bool fieldReadMayAllocate(const Program&, const Function&, const Value&);
bool hasGenericFallback(const Program&, const Function&, const Value&);
static constexpr size_t maxInlineStringCompareLength = 32;
static constexpr size_t maxRegisterStringLength = 8;
template<typename CharacterType>
inline uint64_t packStringIntoRegister(std::span<const CharacterType> text)
{
    uint64_t packed = 0;
    if (text.size() == 7) {
        for (size_t i = 0; i < 4; ++i)
            packed |= static_cast<uint64_t>(text[i]) << (8 * i) | static_cast<uint64_t>(text[3 + i]) << (32 + 8 * i);
        return packed;
    }
    for (size_t i = 0; i < text.size(); ++i)
        packed |= static_cast<uint64_t>(text[i]) << (8 * i);
    return packed;
}
Writes writesOfBuiltinCall(const Program&, const Function&, const Value&);
Writes writesOfResolvedBuiltinCall(const Program&, const Function&, const Value&);
std::optional<Writes> writesOfInlineBuiltin(InlineBuiltin, std::span<const Type> arguments);
#define AOT_TYPED_DECLARE_WRITES_OF_FAMILY(family) std::optional<Writes> writesOfInline ## family(InlineBuiltin, std::span<const Type> arguments);
FOR_EACH_AOT_TYPED_BUILTIN_FAMILY(AOT_TYPED_DECLARE_WRITES_OF_FAMILY)
#undef AOT_TYPED_DECLARE_WRITES_OF_FAMILY
inline std::optional<Writes> writesIf(bool hasCode, Writes writes) { return hasCode ? std::optional<Writes>(writes) : std::nullopt; }
inline bool isNumberOrUndefined(const Type& type)
{
    Rep rep = repOf(type);
    return type.isNumber() || (!(type.primitives & Primitive::Null) && (rep == Rep::OptionalDouble || rep == Rep::OptionalInt64));
}
inline bool areNumbers(std::span<const Type> types)
{
    for (auto& type : types) {
        if (!type.isNumber())
            return false;
    }
    return true;
}
enum class KeyedAccess : uint8_t {
    Generic,
    Array,
    TypedArray,
    String,
};
KeyedAccess keyedAccessOf(const Program&, const Function&, const Value&);
bool isNumericOperand(const Type&);
Writes writesOfArithmetic(const Type&, const Type*);
Type elementTypeOf(TypedArrayType);
BuiltinID applicableEntry(BuiltinID first, std::span<const Type> arguments);

Rep resultRepOf(const Program&, FunctionID);
uint8_t kindByteOf(const Program&, const Function&);

struct SplitFunction {
    Function entry;
    Vector<Function> continuations;
};
bool splitFunction(const Program&, FunctionID, const Function&, const BitVector& blocksWithCalls, const BitVector& unreachableBlocks, SplitFunction&);
Vector<size_t, 6> fieldsInitializedWithAllocation(const Program&, const Function&, const Block&, size_t allocation, bool isCreateThis, bool hasNullPrototype);
Vector<unsigned> immediateDominatorsOf(const Function&);
bool isAllocatedWithInitialStructure(const Program&, const Function&, const Block&, size_t allocation);
void assignStructureDescriptors(Program&);
uint32_t maxFunctionSlots();

ImageKey keyOf(FunctionID);
ImageKey continuationKey(FunctionID, unsigned index);
bool compile(VM&, const Program&, FunctionID, CompiledCode&);
uint32_t shapeHashForCompilationOrder(const Function&);

class CompiledProcedureCache {
    WTF_MAKE_NONCOPYABLE(CompiledProcedureCache);
public:
    struct Entry;

    CompiledProcedureCache();
    ~CompiledProcedureCache();

    static CompiledProcedureCache* current();

    Entry* find(const B3::ProcedureFingerprint&);
    Entry* add(std::unique_ptr<Entry>&&);
    void remove(Entry*);
    void didUse(Entry&);
    void suspend() { ++m_suspensionCount; }
    void resume() { --m_suspensionCount; }

private:
    static constexpr unsigned capacity = 32;

    Vector<std::unique_ptr<Entry>, capacity> m_entries;
    uint64_t m_lastUse { 0 };
    unsigned m_suspensionCount { 0 };
};
void addRegExpLiterals(VM&, const Program&, ImageBuilder&);
std::optional<uint64_t> regExpLiteralKey(const Program&, const Function&, const Value&);
Vector<String> describeForMap(const Program&);

JS_EXPORT_PRIVATE bool dumpAtStage(std::span<const uint8_t>, const String& stage, PrintStream&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
