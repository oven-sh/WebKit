/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypedIR.h"

namespace JSC {

class VM;

namespace AOT {

class ImageBuilder;
struct CompiledCode;
struct ImageKey;

namespace Typed {

// No pass finds out a type. A value a pass makes gets a type that is in the program already: that of the value it stands in for, of the
// place it is read from, of what the function called is declared to yield, or one of TypeTable's.
//
// No pass, and none of the questions below, goes by what is expected of a value (Value::prediction), nor makes one prediction out of
// others. A value that stands for another, or takes its number, takes what is expected of it along.
//
// What looks at one function at a time runs on all functions side by side, and reads of other functions only what was there before.

// From what was read to what is lowered. Yields what the verifier has found, which is for whoever wrote the pass or the front end.
Vector<String> optimize(Program&);

// Over the whole program, one after the other.

// The strings by their text, the layout of closures.
void prepare(Program&);
void expandDisposableStacks(Program&);
// Splits each async function and generator into a ramp and a resume function with a coroutine frame.
Vector<std::pair<FunctionID, Function>> splitCoroutines(Program&);
void splitOptimizedGenerators(Program&, Vector<std::pair<FunctionID, Function>>&&);
void recordCellsLiveAcrossAwaits(const Program&, Function&);
BitVector objectsLiveAcrossSuspensions(const Function&);
BitVector blocksInLoops(const Function&);
constexpr unsigned isInNoCycle = UINT_MAX;
Vector<unsigned> cycleOfEachBlock(const Function&);
Vector<unsigned> cycleOfEachBlock(const Function&, Vector<unsigned>* rankOfComponent);
bool mayLeaveConstructionToBody(const Program&, FunctionID, const Function&, const Value&);
// For every function that is used as a value, every method and every constructor of a class: what someone calls who does not know the
// function. It holds the arguments to the types of the parameters (Cast: As where the type is written down, else Store) and calls the function.
void createGenericEntrypoints(Program&);
void createValueEntrypoints(Program&);
void createAdaptersOfResults(Program&);
void forgetResultsThatCallersCheck(Program&);
void callValuesBySignature(Program&);
void createJSONSerializers(Program&);
// `class Q extends P` in untyped text: Layout::declaredParentConstructor of Q, read off `MakeClass (Q, P)` where both are constants.
void findDeclaredParents(Program&);
// `f.bind(t, a)` where `f` is known: a function that captures `t` and `a` and calls `f`, for each `f` and number of arguments that a site names. resolveCalls() makes closures of them.
void createBoundFunctions(Program&);
// Gives every NewObjectWithShape its Allocation, in Function::allocations. Of a record: the fields its initializing stores create, in
// that order, all of them born. Of anything else: every field, in the order of the layout, all of them born.
void assignAllocations(Program&);
void assignAllocations(const Program&, Function&);
// Program::viewsRetaggedTo, from the lists of specializations as they are given, and viewsLeftAlone.
void findViewsRetaggedTo(Program&);
// After the functions have been optimized: Function::allocations become Program::allocations, in the order of the functions. Then come
// those the run time makes objects with: Function::allocationOfThis.
void linkAllocations(Program&);
// What a call of each function may change: Function::writes and writesShapeWords, and the same of a call that comes back. And what
// casts may: Program::castsMayRetag.
void computeEffectSummaries(Program&);
// Slots, representations, offsets, shape numbers and tests, orders.
void place(Program&);
// What the program needs at run time.
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

// One function at a time. `function` is a copy of the one in the program, which stays as it was.

// Every operand is of the type its operation needs, every argument of the type of the parameter, every value stored of the type of the
// place. Appends what is not.
enum class VerifierScope : uint8_t { Everything, EverythingButDefinitions, DefinitionsAlone };
void verify(const Program&, FunctionID, const Function&, Vector<String>& findings, VerifierScope = VerifierScope::Everything);
void verifyRegExpLiterals(const Program&, Vector<String>& findings);
void cleanUp(const Program&, Function&);
void compactBlocks(Function&);
void cleanUpIfAnythingFolds(const Program&, Function&);
enum class CleanUpTiming : bool { Immediate, Deferred };
bool threadOwnKeysOfForIn(const Program&, Function&);
unsigned flattenIteratedObjectEntries(const Program&, Function&);
unsigned fuseOwnPropertyDescriptorCopies(const Program&, Function&);
unsigned hoistDefinePropertyIntoPredecessors(Function&);
// Behind all passes on the function, once: the birth that a wrapper (Function::worker) has brought along and that is still there goes from the block where the ways out of the worker meet into each of them.
void moveBirthsIntoArms(const Program&, Function&);
void removeTrivialParameters(const Program&, Function&);
bool askForTruthOnTheWaysIn(const Program&, Function&);
// AssertNotAllocated: at that point of the program the operand is no allocated object. It is no use of the operand: a pass that does away with the allocation on the ways that lead
// there takes the assertion out. Under Options::useDollarVM() a call of `function assertNotAllocated(_: unknown): void {}` is one. What is left of them in a function that has code is a finding.
void convertAllocationAssertions(Program&, Vector<String>& findings);
void reportAllocationAssertions(const Program&, Vector<String>& findings);
// For dumpAtStage(.., "allocations"), and nothing otherwise. What is printed of a site goes by how many allocations of it are left in the end. A pass adds why.
void noteAllocation(const Program&, Function&, const Value& site, AllocationOutcome, unsigned builtAt = 0, const Value* forcedBy = nullptr, ASCIILiteral why = { });
void noteAllocationSites(const Program&, Function&);
void concludeAllocationNotes(const Program&, Function&);
void printAllocationRemarks(const Program&, PrintStream&);
void printUsesOfAllocations(const Program&, PrintStream&);

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
    UncheckedKeyHashMap<uint32_t, StringID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> initialText;
    unsigned builderCount { 0 };
    Vector<std::pair<ValueID, ASCIILiteral>> rejections;
};
StringBuilderAnalysis analyzeStringBuilders(const Program&, FunctionID, const Function&);
void dumpStringBuilderRemarks(const Program&, PrintStream&);
BitVector computeMapKeyValues(const Program&, FunctionID, const Function&);
void computeFunctionsReturningMapKeys(Program&);
void findOperandsOnlyCalled(Program&);
void verifyParametersOnlyCalled(const Program&, Vector<String>& findings);
void findClosuresInFrames(Program&);
void findEnvironmentsInFrames(Program&, Vector<String>& findings);
void reuseEnvironments(Program&);
// `===` or `==` of two constants: nil, booleans, numbers, strings. Nothing: one is no constant, or it is not told here.
std::optional<bool> areEqualConstants(const Program&, const Value&, const Value&, bool isStrict);
// Part of cleanUp(): a read of a field or a variable that repeats one whose outcome still holds becomes an Upcast of that one. Yields whether there was one.
bool eliminateRedundantReads(const Program&, Function&);
enum class AllocationSinkingPolicy : uint8_t { EliminateOnly, AllowMaterialization };
bool sinkObjectAllocations(const Program&, Function&, AllocationSinkingPolicy);
BlockID splitEdge(Function&, BlockID from, unsigned targetIndex);
// A call of a method, an accessor or a constructor through a view becomes a call of the function, or a choice among the functions by
// shape. So does an access to a static method or accessor on the class object of a class that is evaluated once, and one through `super`
// in such a class. Yields how much it changed.
unsigned resolveCalls(const Program&, Function&, bool mayGuess, CleanUpTiming = CleanUpTiming::Immediate);
// By size. Yields how much it changed.
unsigned inlineCalls(const Program&, FunctionID, Function&, CleanUpTiming = CleanUpTiming::Immediate);
void internNamesForGeneratorLoopFusion(Program&);
unsigned fuseGeneratorLoops(const Program&, Function&);
void prepareForInlining(Program&);
// A cast whose operand is of the type goes. An object that is made, stored to and then cast to a specialization of its layout before
// anyone has seen it is made with that. Yields how much it changed.
unsigned simplifyCasts(const Program&, Function&, CleanUpTiming = CleanUpTiming::Immediate);
unsigned countInIntegers(const Program&, Function&);
// A catch block that only closes the iterator of an array and rethrows is removed. Yields how many were.
unsigned removeRedundantCatchBlocks(const Program&, Function&);
// A call of a built-in method that the table binds to a function of the prelude becomes a direct call of it, behind a check that the
// receiver has its original structure. Yields how much it changed.
unsigned resolvePreludeCalls(const Program&, Function&, CleanUpTiming = CleanUpTiming::Immediate);
enum class ReceiversThatMayYetBeProven : bool { Wait, AreTaken };
unsigned callGuardedPreludeEntries(const Program&, FunctionID, Function&, ReceiversThatMayYetBeProven);
// A call of a built-in method on a receiver of unknown class gets a fast path for the class it is speculated to be of. Yields how much it changed.
unsigned speculateOnReceivers(const Program&, FunctionID, Function&);
unsigned eliminateGuardsOnUnescapedReceivers(const Program&, Function&);
void eliminateGuardsOnFreshResults(Program&);
void eliminateGuardsOnUnescapedModuleVariables(Program&);
// What comes down to one entry of the table of built-ins says so from here on.
void resolveBuiltins(const Program&, Function&);
void sinkEmptyArrayLiterals(Function&);
// `$call(f, thisValue, ..)` of the prelude, an Operation, becomes a Call.
void convertCallIntrinsicToCall(Value&);

// Questions.

// Whether carrying the value out can throw. Where a handler watches, such a value ends its block.
bool mayThrow(const Program&, const Function&, const Value&);
// Whether IsInstanceOfClass may come to walk the prototype chain of its operand, where a proxy runs its trap. Otherwise the shape says it.
bool mayWalkPrototypeChain(const Program&, const Function&, const Value&);
// Whether the operation by itself makes sure that what it yields is of the type of the value. If not, the lowering tests.
bool vouchesForItsType(const Program&, const Function&, const Value&);
// What an object of the class finds under the name on its prototype chain, if that is a method or accessor of that kind which stays what
// it is. None also where the program is seen to replace it: then an object may have anything under the name, of its own or inherited.
FunctionID findMethod(const Program&, LayoutID, StringID, MethodKind);
struct FieldAccess {
    LayoutID layout;
    uint32_t field;
};
// The field an access by that name comes down to: the receiver is looked at as one type of the program, which has it.
std::optional<FieldAccess> fieldOf(const Program&, const Type& receiver, StringID name);
class WholeNumbers {
public:
    WholeNumbers(const Program& program, const Function& function)
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
// Whether an object of a member of the view that has no own property of the name may inherit one, whether or not its layout has a field of
// the name. Then a read, `in` and [[Set]] ask the prototype chain where the field is absent.
bool mayInherit(const Program&, LayoutID view, StringID name, bool isForStore = false);
bool hasMethodField(const Program&, LayoutID member, StringID name);
bool storeMayAskPrototypeChain(const Program&, const Function&, const Block&, size_t index, LayoutID view, StringID name);
bool mayGiveUp(const Program&, LayoutID view);
bool storeMayMakeObjectGiveUp(const Program&, const Function&, const Block&, size_t index, LayoutID view);
bool onlyExtraPropertiesSupply(const Program&, LayoutID view, StringID name);
// The part of that which holds of every such object: the prototype that its layout says may supply the name. For the rest, a member with
// Layout::mayHaveOtherPrototype, the object says whether to ask: orderWordHasExtraProperties, see Typed::Object::noteWhetherToAsk().
bool inheritsByItsLayout(const Program&, LayoutID view, StringID name, bool isForStore = false);
// Nothing can be added to it.
bool isOnObjectPrototype(const Program&, StringID name);
// Of an object that the function makes, as long as nobody else has seen it: it has the prototype it was born with.
bool mayInheritAtBirth(const Program&, const Function&, ValueID object, StringID name);
// Allocation::constructedBy of a NewObjectWithShape. From assignAllocations() on.
FunctionID constructedBy(const Program&, const Function&, ValueID object);
// Of a NewObjectWithShape: nothing is known of the `prototype` of the function it is made for.
bool isBornUnderAnyPrototype(const Program&, const Function&, ValueID object);
// Of a field of the view: whether an object of a member that has no own property of the name has nothing else to go by either, no accessor,
// no method, nothing on its prototype chain. Then a read of it yields the field, an extra property or `undefined`, and a store goes to
// the field or makes an extra property. Otherwise the access is carried out the general way where there is no own property, which may run
// any code. `ofUnmarkedObject`: of an object without orderWordHasExtraProperties, which has the prototype that its layout says.
bool onlyFieldsSupply(const Program&, LayoutID view, StringID name, bool ofUnmarkedObject = false);
bool onlyFieldsSupplyToMembers(const Program&, LayoutID view, StringID name);
void indexViewsOfLayouts(Program&);
bool membersHoldOnlyDeclaredType(const Program&, LayoutID view, uint32_t field);
// Whether a store by the name to a member of the view that has no such field may be a shape transition to a specialization that has one,
// instead of making an extra property: see State::specializeToStore(). An upper bound: the run time has further conditions.
bool storeMayTransition(const Program&, LayoutID view, StringID name);
// Of a layout that is no Record: Allocation::order and Allocation::born. A cell has all its fields from its allocation on, in the order of the
// layout, but for those of a class that no constructor defines (fieldIsAbsentAtBirth), which are absent until they are stored to.
void setBornFields(const Layout&, Allocation&);
// Whether the value at `index` in the block is a store to a field that comes right after the allocation of the object, with only such
// stores in between: nothing has happened to the object yet.
bool isInitializingStore(const Program&, const Function&, const Block&, size_t index);
class UnchangingFunctionScope {
    WTF_MAKE_NONCOPYABLE(UnchangingFunctionScope);
public:
    UnchangingFunctionScope();
    ~UnchangingFunctionScope();

private:
    friend bool isInitializingStore(const Program&, const Function&, const Block&, size_t index);

    UnchangingFunctionScope* m_outerScope;
    const Block* m_blockOfInitializingStores { nullptr };
    ValueID m_objectOfInitializingStores;
    size_t m_lastInitializingStore { 0 };
};
// Of a function whose every `return` yields an object it has just made in that block, an array literal of one length or an object of one layout with one store to every field, that nothing else uses: at most four values.
struct FreshResults {
    bool isArray { false };
    LayoutID layout;
    Vector<TypeID, 4> types;
    // By block that returns: what the object holds.
    Vector<std::pair<unsigned, Vector<ValueID, 4>>, 2> returns;
    // 1, 2: other returns yield `undefined`, `null`: in these blocks.
    uint8_t nilKind { 0 };
    Vector<unsigned, 2> nilReturns;
};
// The value is a direct call of a wrapper with Function::nilOfResults. Where the function only tests what that yields for nil, and reads it where a test has said that there is an object, the call goes to the worker and the tests and reads to what that yields. False: nothing is changed.
bool takeApartResultOrNil(const Program&, Function&, ValueID call);
std::optional<FreshResults> freshResultsOf(const Program&, const Function&);
// Of a call of such a function: what it yields is only looked into: an element under the length by a constant index, a field by its name, whether it is nil. Nothing keeps it, compares it or sees its keys. None: it is used otherwise.
struct ReadsOfFreshResult {
    // The call, and the casts that stand for what it yields.
    Vector<ValueID, 4> aliases;
    // Which of FreshResults::types each reads.
    Vector<std::pair<ValueID, uint32_t>, 8> reads;
    Vector<ValueID, 2> nilTests;
};
std::optional<ReadsOfFreshResult> readsOfFreshResult(const Program&, const Function&, ValueID call, const FreshResults&);
// Whether the object was made by this very run of the function, so that nobody who was running before can have looked into it.
bool isLocalAllocation(const Function&, ValueID);
// The first stores of a constructor function: the PutByIds to its `this` that prepare() has put in the place of its `Initialize`, in order. They
// are adjacent in its entry block. Empty: it has none. Copies of them that were inlined into the function are not among them.
std::span<const ValueID> initialStoresOf(const Function&);
// What carrying out the value at `index` of the block may change, as the effect summaries take it. Not of a CallDirect, which goes by what is summed up for the function.
// A store to an object that this run of the function has made counts as RuntimeState: that is for callers. Whoever asks about the function itself looks at stores first.
Writes writesOf(const Program&, const Function&, const Block&, size_t index);
// Of an Operation or a CallBuiltin: which operation of the language it is, if it is one.
std::optional<LanguageOperation> languageOperationOf(const Program&, const Value&);
// The names that are seen to be stored to the object or defined on it, and how many stores there are under symbols: in the function, and in those it calls by name with the object for `this` or for an argument, `depth` calls deep.
struct UserInBlock {
    unsigned block;
    ValueID value;
};
using UsersInBlocks = Vector<UserInBlock, 4>;
void collectPropertiesGivenTo(const Program&, const Function&, ValueID object, unsigned depth, Vector<StringID, 16>& names, unsigned& numberOfKeys, const UsersInBlocks* users = nullptr, bool goesByTheChain = false);
bool collectPropertiesGivenToThis(const Program&, FunctionID, Vector<StringID, 16>& names, unsigned& numberOfKeys, bool followsCallsOnThis = true);
// What carrying out a Cast may change, from the effect summaries on. A test changes nothing, and where it fails and that is the end
// nothing is changed either. As and Coerce go on: a plain object is copied (RuntimeState), and where Coerce converts an object to a
// primitive its methods run (Heap). No cast walks a container.
Writes writesOfCast(const Program&, const Function&, const Value&);
// What reading an array element as the type may change: see Lowering::ensureElement().
Writes writesOfElementRead(const Program&, const Type&);
// Of a Cast for the reason Coerce, with the type of its operand and the type after `as`: whether it may make anything of an object. If
// not, and if the operand is of the type of the cast, it is a way to look at the operand and no more.
bool coercionMayChangeObjects(const Program&, const Type& from, const Type& coercedTo, bool isOfFreshData);
// Whether a Cast to the type for the reason Flow goes on where the test fails: an object that was made as something the type extends is
// what a type guard or a discriminant says, and is turned into one of the type where it is. Nothing is copied or rewritten for that.
bool narrowingMayRetag(const Program&, const Type&);
bool readMayInstantiate(const Program&, const Type&);
// Whether it may turn the object into one of a specialization of its layout, where it is. See Function::writesShapeWords.
bool castWritesShapeWords(const Program&, const Function&, const Value&);
bool claimMayMakeObjects(const Program&, const Function&, const Value&);
bool fieldReadMayMakeObjects(const Program&, const Function&, const Value&);
// Of a CallBuiltin: whether it tests that the receiver has its original structure, and is carried out by name where it has not.
bool hasGenericFallback(const Program&, const Function&, const Value&);
static constexpr size_t maxLengthOfTextComparedInPlace = 32;
static constexpr size_t maxLengthOfTextInRegister = 8;
template<typename CharacterType>
inline uint64_t textInRegister(std::span<const CharacterType> text)
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
// What carrying out a CallBuiltin may change.
Writes writesOfBuiltinCall(const Program&, const Function&, const Value&);
// The same without the generic fallback.
Writes writesOfResolvedBuiltinCall(const Program&, const Function&, const Value&);
// What the code changes that carries out the entry in place. Nothing: there is no such code for these arguments, and the function of the
// engine is called.
std::optional<Writes> writesOfInlineBuiltin(InlineBuiltin, std::span<const Type> arguments);
// The same for the inline built-ins of one family, in AOTTypedBuiltins<Family>.cpp. It goes case by case with Lowering::lowerInline<Family>().
#define AOT_TYPED_DECLARE_WRITES_OF_FAMILY(family) std::optional<Writes> writesOfInline ## family(InlineBuiltin, std::span<const Type> arguments);
FOR_EACH_AOT_TYPED_BUILTIN_FAMILY(AOT_TYPED_DECLARE_WRITES_OF_FAMILY)
#undef AOT_TYPED_DECLARE_WRITES_OF_FAMILY
inline std::optional<Writes> writesIf(bool hasCode, Writes writes) { return hasCode ? std::optional<Writes>(writes) : std::nullopt; }
inline bool areNumbers(std::span<const Type> types)
{
    for (auto& type : types) {
        if (!type.isNumber())
            return false;
    }
    return true;
}
// How GetByVal, PutByVal, IteratorOpen, IteratorHasNext, IteratorValue and IteratorClose are carried out, going by the given types: in
// place, for a receiver that is an array, a typed array or a string and a key that is a number. Then no code of the program runs, on
// any path, but for a store to an array that is no longer as it was made (JSObject::becomeDerivedArray()), which does all the language
// says. Generic: the way the language says, whatever the receiver is.
enum class KeyedAccess : uint8_t {
    Generic,
    Array,
    TypedArray,
    String,
};
KeyedAccess keyedAccessOf(const Program&, const Function&, const Value&);
// A number, or `undefined` or `null`, which operations on numbers take for numbers. Arithmetic on such operands is carried out in place.
bool isNumericOperand(const Type&);
// What an arithmetic operation or a comparison may change, by the types of its operands. The second is null for a unary operation.
Writes writesOfArithmetic(const Type&, const Type*);
Type elementTypeOf(TypedArrayType);
// The entry among those of one built-in that holds for these arguments.
BuiltinID applicableEntry(BuiltinID first, std::span<const Type> arguments);

// How a function hands back what it yields. Void: that is only ever `undefined`.
Rep resultRepOf(const Program&, FunctionID);
uint8_t kindByteOf(const Program&, const Function&);

// A function in parts, for the back end alone. The entry part has what the entry reaches without a call that needs a frame. Where it would
// go on to such a call it jumps to a continuation, which takes what is live and never comes back. Values and blocks keep their numbers.
struct SplitFunction {
    Function entry;
    Vector<Function> continuations;
};
// False: it stays whole.
bool splitFunction(const Program&, FunctionID, const Function&, const BitVector& blocksThatEmitCalls, const BitVector& blocksThatAreNotRun, SplitFunction&);
Vector<size_t, 6> fieldsMadeWithAllocation(const Program&, const Function&, const Block&, size_t allocation, bool isCreateThis, bool hasNullPrototype);
Vector<unsigned> immediateDominatorsOf(const Function&);
bool isBornByItsRow(const Program&, const Function&, const Block&, size_t allocation);
void numberRowsOfBirths(Program&);
uint32_t maxSlotsOfFunction();

ImageKey keyOf(FunctionID);
ImageKey keyOfContinuation(FunctionID, unsigned index);
bool compile(VM&, const Program&, FunctionID, CompiledCode&);
// `/source/flags` and `RegExp("source", "flags")` of constants, wherever the program has one.
void addRegExpLiterals(VM&, const Program&, ImageBuilder&);
// Of `/source/flags`, `new RegExp("source", "flags")` without `y`: source and flags, as Program::offsetsOfSharedRegExpObjects has them.
std::optional<uint64_t> keyOfRegExpLiteral(const Program&, const Function&, const Value&);
// What the map of the image (Options::aotMapFilePath) says of each function, separated by tabs: its name, after that of its class; its
// entrypoint; its number; the path of its module; its position there. Entrypoints: typed or untyped (written in TypeScript or not), ramp,
// resume, module, entry, and with "generic-" before it the generic entrypoint of such a function.
Vector<String> describeForMap(const Program&);

// For tests. Stages: "builtins" (the table, whatever the program), "read", "optimized", "allocations". False, with the reason printed: the program cannot be read.
JS_EXPORT_PRIVATE bool dumpAtStage(std::span<const uint8_t>, const String& stage, PrintStream&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
