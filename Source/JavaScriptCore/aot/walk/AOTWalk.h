/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTBody.h"
#include "AOTBodyIndex.h"
#include "AOTCode.h"
#include "AOTCompileUnit.h"
#include "AOTDiagnostics.h"
#include "AOTMechanisms.h"
#include "AOTWalkLinks.h"
#include "AOTWalkLoopsOverGeneratorsOfUnit.h"
#include "AOTWalkCoroutineState.h"
#include "AOTWalkState.h"
#include <array>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/IterationStatus.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class Walker;
// AOTWalkLeaveRecord.h
struct LeaveFrame;
struct LeaveAtHead;
struct LeaveAtHeadInChain;

// ONE OPERATION GOES ONCE THROUGH ONE FIXED CHAIN. Walker::walkOperation() asks these in this order, each only while its mechanism is not withheld.
// True: it has built what stands for the operation and bound its value. False: it does not apply, and the next is asked. It has then built nothing.
// Each begins with a test of the opcode against a constant OpcodeSet, so an operation that it has nothing to say about costs a few instructions.
// The last link always applies: Walker::copy(), the operation in its closed form.
bool buildDeferredDefinition(Walker&, const BodyValue&); // AOTWalkDeferredDefinitions.cpp . never withheld here: the plan has decided for the whole program (Deferral), and whoever defines or reads such a variable goes by it
// AOTWalkEnvironmentCells.cpp . PlannedLayout::isEscapedOffset. Walker::build() asks the first of every draft of a unit that may name such an environment, its operands as they came.
OpcodeSet operationsThatMayKeepEnvironments();
inline bool mayKeepEnvironment(Opcode opcode)
{
    static const OpcodeSet opcodes = operationsThatMayKeepEnvironments();
    return opcodes.contains(opcode);
}
void markEnvironmentsThatOperationKeeps(Walker&, const Draft&);
// Some capture of the function is written to be an environment whose cells are handed back.
bool isHandedEnvironmentWithByte(const Walker&, FunctionID);
void noteEnvironmentThatUnitMakes(Walker&, const Draft& allocation);
// In front of a Return of the body being read: a ReleaseEnvironment for each scope that the body has made above it.
void handBackEnvironmentsAtReturn(Walker&, BlockID inputBlock);
void markEnvironmentsThatCoroutineIsHanded(Walker&);
// AOTWalkDeferredDefinitions.cpp. A read of the variable may find the mark, and then makes the object and stores it. Whatever runs between two such reads may fill the variable: a callee, another turn of a loop.
// So the read stands where it is written: it is neither shared nor built in front of a loop.
bool isVariableFilledAtFirstRead(const Plan&, VariableID);
bool foldOperation(Walker&, const BodyValue&); // AOTWalkFolding.cpp . Folding
bool buildFreshObject(Walker&, const BodyValue&); // AOTWalkEscapeAnalysis.cpp . EscapeAnalysis: an allocation is built as ever, and noted as fresh with what its fields hold
bool callBuiltin(Walker&, const BodyValue&); // AOTWalkBuiltins.cpp . BuiltinCalls
// AOTWalkBuiltins.cpp. Of a call that stays as it is written: `table.get(key)`, where the receiver is, or is expected to be, of one class of the engine. It claims nothing: the stub looks at the receiver.
void nameMethodOfReceiverAsMade(Walker&, const BodyValue& call, Draft&);
inline constexpr OpcodeSet opcodesThatCallFunctionGivenToVariableTakes { Opcode::Call };
AOT_LINK_OF_WALK(callFunctionGivenToVariable, opcodesThatCallFunctionGivenToVariableTakes) // AOTWalkGuesses.cpp . Guesses
inline constexpr OpcodeSet opcodesThatOpenGuessAtElementOfNumbersTakes { Opcode::ElemGet };
AOT_LINK_OF_WALK(openGuessAtElementOfNumbers, opcodesThatOpenGuessAtElementOfNumbersTakes) // AOTWalkGuesses.cpp . Guesses. True: both arms are built and have met.
// AOTWalkTextBuilderCallees.cpp . Inlining. Of a Call or a CallDirect that findGrowingTexts() has taken for a growth of a text in a builder: `s = join(s, x)`. The copy of the callee appends.
// Where no copy is made the call takes a string, and the builder is filled anew with what the call gives. False: the call is no such growth, and nothing was built.
extern const OpcodeSet opcodesThatGrowTextThroughCalleeTakes;
AOT_LINK_OF_WALK(growTextThroughCallee, opcodesThatGrowTextThroughCalleeTakes)
bool inlineCallee(Walker&, const BodyValue&); // AOTWalkInlining.cpp . Inlining
bool callDirectly(Walker&, const BodyValue&); // AOTWalkCalls.cpp . DirectCalls
bool buildTypedOperation(Walker&, const BodyValue&); // AOTWalkTyped.cpp . TypedOperations. It hands on by family to the three below.
enum class GuessOpened : uint8_t {
    None,
    // The operation is built, and tests the guess for itself. The walk goes on behind it as behind any other.
    AsOneOperation,
    // Both arms have walked the operation, and whatever else of the stretch, through the chain.
    WithArms,
};
GuessOpened openGuessAt(Walker&, const BodyValue&); // AOTWalkGuesses.cpp . Guesses
bool openGuessAtAllocation(Walker&, const BodyValue&); // AOTWalkGuesses.cpp . Guesses, of a NewObject, in front of the chain
bool replaceOperation(Walker&, const BodyValue&); // AOTWalkReplacements.cpp . never withheld: the 15 whose range ends before the emitters, a call that is done in place, a call that makes the symbol of a key, and a call of a built-in without a function object. It hands on to the four below.

// The one target of a Branch, a Switch or a SwitchString whose operand is a constant by now. The scan has asked the same rule of the constants of the body: AOTFoldingOfConstants.h.
std::optional<uint32_t> onlyTargetKnownToFolding(Walker&, const BodyTerminator&); // AOTWalkFolding.cpp . Folding
bool buildTypedAccess(Walker&, const BodyValue&); // AOTWalkTypedAccesses.cpp: properties, slots, environments, variables, private names, super
bool buildTypedElementAccess(Walker&, const BodyValue&); // AOTWalkTypedElements.cpp: elements of arrays, typed arrays, strings, arguments
bool buildTypedOperator(Walker&, const BodyValue&); // AOTWalkTypedOperators.cpp: operators, comparisons, conversions, casts, tests. With UnboxedNumbers: AOTWalkNumbers.cpp
bool buildTypedConstruction(Walker&, const BodyValue&); // AOTWalkCalls.cpp: what the constructor of a class allocates where new.target is proven to be one class object
std::optional<uint32_t> positionOfTagAmongArguments(const Walker&, FunctionID constructor); // AOTWalkUnionTaggedMakers.cpp: among the positional arguments of a construction. Nothing: whoever makes `this` for the function has no layout to choose
bool buildObjectOfTaggedClassConstructor(Walker&, const BodyValue& allocation); // AOTWalkUnionTaggedMakers.cpp: TaggedMaker::allocation, by the tag where new.target is the class itself. False: nothing was built
Reference fillerForSlotInAllocation(Walker&, FieldReference); // AOTWalkUnionFolds.cpp: what an allocation lists in the place of a value that is not proven to fit the slot. Null: the allocation ends in front of the field
bool everyReceiverKeepsFieldInSlotThatIsNeverEmpty(const Walker&, std::span<const LayoutID> receivers, PropertyKeyID); // AOTWalkUnionSlots.cpp: the form of the slot has no empty state, so the property is there for the life of each object
TypeID typeOfPairsThatStepsHandOut(Walker&, TypeID iterable); // AOTWalkIteration.cpp
// AOTWalkIteration.cpp. By the rows of the table nobody can see a list that is as it was made being iterated.
bool listsAreIteratedUnseen(Walker&);
// AOTWalkIteration.cpp. A link of the chain at the read of `values`, `keys` or `entries` whose call a loop alone takes. False: nothing was built.
inline constexpr OpcodeSet opcodesThatOpenLoopOverIteratorThatEngineMakesTakes { Opcode::PropGet };
AOT_LINK_OF_WALK(openLoopOverIteratorThatEngineMakes, opcodesThatOpenLoopOverIteratorThatEngineMakesTakes)
// `for (const [key, value] of Object.entries(object))`: the loop alone takes the list, its one step stands in its head, and a pattern of two takes each pair apart at once. So nobody gets hold of the list or of a pair,
// and neither the pairs nor a list of them need be made.
bool isListOfEntriesTakenApartAtOnce(Walker&, const BodyValue& call);
bool buildTypedIteration(Walker&, const BodyValue&); // AOTWalkIteration.cpp: loops, patterns and spreads over a list that nobody can see being iterated, and what is appended to a list that the state knows
bool replaceIteration(Walker&, const BodyValue&); // AOTWalkIteration.cpp
bool replaceCoroutineOperation(Walker&, const BodyValue&); // AOTWalkCoroutines.cpp
bool replaceCallDoneInPlace(Walker&, const BodyValue&); // AOTWalkCallsDoneInPlace.cpp
bool replaceCallOfCompiledBuiltin(Walker&, const BodyValue&); // AOTWalkCalls.cpp: of whatever enters its callee by value
// The call is marked as done in place, and the row of the built-in that it names forwards it to an operand. It is no call of the built-in, so BuiltinCalls leaves it to replaceOperation().
bool isCallForwardedInPlace(Walker&, const BodyValue&); // AOTWalkCallsDoneInPlace.cpp
// `f.call(t, x)`, `f.apply(t, list)`, `Reflect.apply(f, t, list)` as a program writes them. The callee is proven to be a built-in whose row does nothing but forward the call to an operand: that operand is called where the call stands.
// BuiltinCalls asks it in front of the rows. False: nothing was built.
bool forwardCallOfBuiltin(Walker&, const BodyValue&); // AOTWalkCallsDoneInPlace.cpp
// `x.call(t, y)` where nothing says what the read of `call` has found. Above every function lies one member under the key, for good, and its row forwards the call to the receiver. So that member is expected:
// one test of what was read, the forwarded call, the call as written in a rare block, and the two meet behind the call. Guesses asks it. True: both arms are built, and have left what they had to leave.
bool forwardCallBehindTestOfCallee(Walker&, const BodyValue&); // AOTWalkCallsDoneInPlace.cpp
class InputsOfTypedOperation;
using TypesOfInputs = ScopedLambda<TypeID(ValueID)>;
// A CallDirect of the input whose function linked() does not reach: Link has proven that no run comes here: Walker::endAsUnreachable(). WHATEVER IS WITHHELD, in every role: there is no code to call. With the tables at their defaults every function is reached. False: nothing was built.
bool endWhereCalleeIsNeverReached(Walker&, const BodyValue& call); // AOTWalkUnionCalls.cpp
// A ResolveHeritage whose operand can only be class objects, constructors of the table and null: built as written with effects that only allocate. False: nothing was built.
bool buildLookAtHeritage(Walker&, const BodyValue&); // AOTWalkUnionFolds.cpp
// Folding: a read by a known index or of `length` off a NewTemplateObject in sight. False: nothing was built.
bool foldReadOfTemplateObject(Walker&, const BodyValue& read); // AOTWalkUnionFolds.cpp
// DirectCalls: a Call whose callee is a read by name off its `this`, whose receiver is PROVEN of layouts that all have a row, and which findOnlyCallee() does not answer. It builds a CallTable and leaves arrivals. False: nothing was built.
bool callThroughMethodTable(Walker&, const BodyValue& call); // AOTWalkUnionCalls.cpp
// For Guesses, a TERM of the row in openGuessAt(): the same behind ONE test of the receiver. True: both arms are built and have met.
bool callThroughMethodTableBehindTest(Walker&, const BodyValue& call); // AOTWalkUnionCalls.cpp
// For typedFormForGuesses(), in front of `FunctionID callee;`: with operands of these types callThroughMethodTable() builds.
bool wouldCallThroughMethodTable(Walker&, const BodyValue& call, const TypesOfInputs&); // AOTWalkUnionCalls.cpp
// A look at a typed copy whose failing tests begin the turn again has typed the operation, and all behind it that the block dominates, as if the operand had passed the test for what is expected of it.
struct GuessCountedOnByLooks {
    ValueID operation;
    ValueID operand;
    TypeID expected;
};
// THE ONE TYPED FORM OF THE MECHANISM, in buildTypedOperation() in front of buildTypedIteration(), and its twin in typedFormAsReceiverStands() in front of buildInstanceOfBuiltin(). By opcode: InstanceOf . RequireObjectCoercible: buildCheckThatTypesPass(). It builds where the inputs are what the walk has built, and only answers where they are types.
bool buildFormOnShapes(Walker&, const BodyValue&, const InputsOfTypedOperation&); // AOTWalkUnionInstanceOf.cpp
// A check whose operand, by these types, is neither undefined nor null: the operand stands in its place. Asked ahead it lets a stretch of a guess begin at the check. False: nothing was built.
bool buildCheckThatTypesPass(Walker&, const BodyValue& check, const InputsOfTypedOperation&); // AOTWalkNarrowing.cpp
// In answerThatTypesDecideInFolding(), first: `x < x`, `x < undefined`, and a relation that a branch above has answered. Nothing: the types decide as ever.
std::optional<bool> answerOfRelationWithoutCompare(Walker&, const BodyValue& relation); // AOTWalkNarrowingOfBranches.cpp
// For AOTWalkTypedAccesses.cpp. A set of several layouts that is no family: all of them lie in one family, so a test or a case has left them of it. False: the access stays closed.
bool mayAccessThroughSetOfLayouts(const Walker&, LayoutSetID); // AOTWalkUnionSlots.cpp
// The receivers keep the field in their cells in one form at several offsets (FieldAccess::findsOffsetByShape). True: one operation that finds the offset by the shape pays here. `isStore`: PutSlot, PutSlotOrAdd.
bool paysToFindOffsetByShape(const Walker&, std::span<const LayoutID> receivers, PropertyKeyID, bool isStore); // AOTWalkUnionSlots.cpp
// The layouts in use that an object may have which `new` of the function makes for itself: the constructed class, with the members that a tag picks. Null: it constructs no class, or no such layout is in use. An expectation.
TypeID typeOfObjectsMadeBy(const Walker&, FunctionID constructor); // AOTWalkUnionTaggedMakers.cpp
// A PROOF, of `new F(..)` by value with F itself for new.target: the unit AllocatingConstructEntry makes `this` with one of those layouts, and the body is proven to return no object. Null: no such unit, or the body may return one.
TypeID typeOfObjectsMadeThroughEntryOf(const Walker&, FunctionID constructor); // AOTWalkUnionTaggedMakers.cpp
// walkConstructOfFunctionInWalkCalls(), behind `LayoutID constructed = ..`. `constructed` becomes the member that the tag at the call picks. False: the tag picks one of several, so the construction stays a call of the unit that holds the switch.
bool chooseLayoutByTagAtConstruction(Walker&, const BodyValue& construction, FunctionID constructor, LayoutID& constructed); // AOTWalkUnionTaggedMakers.cpp
// BuilderOfEntries::allocateThis(): the object, by a switch over the tags. `tag`: the argument as the entry took it. Null: the function is no tagged maker, and nothing was built.
Reference buildThisByTag(Walker&, FunctionID constructor, Reference prototype, Reference tag, Origin); // AOTWalkUnionTaggedMakers.cpp
// In findExpectationForGuesses(), behind its call of findExpectationAnewForGuesses(), in front of the question whether the functions may be mentioned. `found`, `isDeclared`: that answer; both may be changed, `found` may be null on the way in and out.
void refineExpectationOfValue(Walker&, ValueID input, TypeID& found, bool& isDeclared); // AOTWalkNarrowingOfGuesses.cpp
// In openGuessWithTest(), first thing under the ClosedArm, where ONE test leads to the closed block: the names are bound to what the refusal leaves of the value. `test` null, or nothing is taken away: nothing is built.
void bindNamesBehindFailedTest(Walker&, Reference value, Reference test, uint32_t operandIndexOfTest, std::span<const ValueID> names, Origin); // AOTWalkNarrowingOfGuesses.cpp
// AOTWalkStableBindings.cpp . what the front end's binder has found, off the wire. Link is not asked.
// The closure was read out of the slot of the scope that declares `callee`, and the capture takes that scope: the environment that was read from. It builds nothing. Null: the capture is read off the closure.
Reference findCaptureAtHand(Walker&, FunctionID callee, uint32_t capture, Reference closure);
// A GetModuleVariable or GetEnvironmentField of a binding that nothing assigns to, which a read, a store or a check has seen filled on every way to here. Null: the read is built.
Reference valueOfStableBinding(Walker&, const BodyValue& read);
void noteValueOfStableBinding(Walker&, VariableID, Reference value);
void noteValueOfStableBinding(Walker&, Reference environment, FieldReference, Reference value);
// The same of a variable that its first read fills: what a read has found behind its test, or made. Null: the read is built, with its test.
Reference objectOfVariablePutOff(Walker&, VariableID);
void noteObjectOfVariablePutOff(Walker&, VariableID, Reference object);
// AOTWalkKnownCopiesLetGo.cpp . Both for walkInline(). The operands of the call that another call has handed back to it alone. It builds nothing.
// `closure` is read out of a slot that holds a mark: the scope that the allocation of the object lists, where that is in sight, or else what was read beside the slot. Null: it is no such read.
Reference findScopeOfClosureReadOffObjectInSight(const Walker&, FunctionID, Reference closure);
Vector<Reference, 2> collectWhatCopyAloneHolds(Walker&, const BodyValue& call);
// Behind the copy: a ReleaseHint [isBehindCopiedBody] for each name of each of them that may lie in a place of its own.
void letGoWhatCopyHeld(Walker&, std::span<const Reference> held, Origin);
// The one function of the library that all rows of calls of the built-in name, where the program holds it and it takes nothing of a function object. `typeOfCallee`: all that is called is one built-in. Null: there is none.
FunctionID findLibraryFunctionBehindBuiltin(const Walker&, TypeID typeOfCallee); // AOTWalkCalls.cpp
// `const name = Symbol("text")` in the body of a module that runs once. The image holds one symbol under the key that finds the fields under `[name]`, and the call gives that one: replaceOperation() puts it there.
// Null: the call makes a symbol of its own, which finds no field. It is no call of the built-in either, so BuiltinCalls leaves it alone.
PropertyKeyID keyOfSymbolMadeOnce(Walker&, const BodyValue&); // AOTWalkReplacements.cpp

// `o[k]`, where all that k can be is one symbol that is made once, is `o.name`: the same operation in another closed form, which every link of the chain takes as an access by name.
// TypedOperations: Link says so (LinkTables::keyAt()), and has listed the function among the referrers of whatever lies under the name. With Link's tables at their defaults: the walk has put the symbol itself in the place of k.
// Null: the access goes by the value of k. It is asked of an operation of the body being read, and answers the same wherever the walk stands.
PropertyKeyID keyThatAccessGoesBy(const Walker&, const BodyValue& access); // AOTWalkReplacements.cpp
inline constexpr OpcodeSet accessesUnderComputedKeys { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine };

// The words of an access by name that no body holds, in the form of the wire: ir/AOTBody.h.
class AccessUnderProvenKey {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(AccessUnderProvenKey);
    WTF_MAKE_NONCOPYABLE(AccessUnderProvenKey);
public:
    AccessUnderProvenKey(const BodyValue& written, PropertyKeyID); // AOTWalkReplacements.cpp

    ValueID id() const { return m_id; }
    PropertyKeyID key() const { return PropertyKeyID::fromBits(m_entryOfKey - 1); }
    BodyValue value() const LIFETIME_BOUND { return BodyValue::operation(m_words.data(), &m_unit, m_id); }

private:
    // The head, the flags, four fields, the second position.
    std::array<uint32_t, BodyValue::numberOfHeadWords + 6> m_words { };
    // IDRemapTable::entries, for the one number that the words give a key.
    uint32_t m_entryOfKey { 0 };
    UnitIDRemap m_unit;
    ValueID m_id;
};

// The mechanisms that are no link of the chain, and where the walk asks them.
void takeArrivals(Walker&); // AOTArrivals.cpp . Arrivals . at the entry of a Body or CoroutineBody: what callers proved of the parameters. Withheld: nothing is proven of them
void markEscapingOperands(Walker&, const Draft&, const Effects* foundByCaller); // AOTWalkEscapeAnalysis.cpp . EscapeAnalysis . Walker::build() asks it of every operation, before it appends
// AOTWalkEscapeAnalysis.cpp . EscapeAnalysis. What the state knows of a list beyond its elements. Each takes any name of the list. Withheld: nothing is known of any.
// `list`: a NewArray that was built just now, which no value of the input stands for. It is noted as buildFreshObject() notes one.
void noteFreshList(Walker&, Reference list);
// `list` was made for this code alone by whoever entered it, as a list is made: the list of the other arguments.
void noteListMadeByCaller(Walker&, Reference list);
// The list has passed IsPristine, or was made here: it has the names and the prototype that a list is made with, until something may change shapes.
void noteListAsMade(Walker&, Reference list);
bool isListAsMade(Walker&, Reference list);
// Of a fresh list that only operations which the state follows have written into. Nothing: it is not known.
std::optional<uint32_t> lengthOfFreshList(Walker&, Reference list);
// Null: a hole, or what lies there is not known.
Reference elementOfFreshList(Walker&, Reference list, uint32_t position);
// Every element, where the length is known and none of them is a hole.
std::optional<Vector<Reference, 8>> elementsOfFreshList(Walker&, Reference list);
// Of a CallList, a ConstructList or a SuperConstructList whose list is such a list: the Call, the Construct or the SuperConstruct that writes the elements out as its arguments. AOTWalkIteration.cpp.
std::optional<Draft> draftOfCallWithListWrittenOut(Walker&, const BodyValue& call);
// Nothing but this code has the list, and it is as it was made: a read out of it by a number finds an element of its own or nothing, and runs no code.
bool isFreshListAsMade(Walker&, Reference list);
// `object`: what a NewObject that no value of the input stands for has given, with `numberOfFieldsWritten` fields in its draft. It is noted as buildFreshObject() notes one.
void noteFreshObject(Walker&, Reference object, size_t numberOfFieldsWritten);
// Every property of an object of a layout that nothing but this code has, in the order in which the object got them, with what each holds now. Each is a plain value in its slot. Nothing: it is not known.
struct PropertyOfFreshObject {
    FieldReference field;
    Reference value;
};
std::optional<Vector<PropertyOfFreshObject, 8>> propertiesOfFreshObject(Walker&, Reference object);
// `arguments` while nothing but this code has it. Each takes any name of the object. The object is built all the same: what these answer names it no more, so the sweep drops it where nothing else needs it.
// What `arguments.length` gives. Null: it is not known.
Reference lengthOfFreshArguments(Walker&, Reference arguments);
// What `arguments[position]` gives. Null: it is not known.
Reference elementOfFreshArguments(Walker&, Reference arguments, uint32_t position);
// The elements from `firstPosition` on are those of `list`, one for one, and behind the last of them the object has none. The program has no name for the list, so it stays what it is.
struct ListBehindParametersOfArguments {
    Reference list;
    uint32_t firstPosition { 0 };
};
// `arguments[i]` with i from `lowest` on: each position below the list is a parameter whose value the state has, at most mostParametersOfArgumentsAskedOneByOne of them, and the rest lies in the list. It builds nothing.
inline constexpr uint32_t mostParametersOfArgumentsAskedOneByOne = 4;
bool positionsOfFreshArgumentsAreInSight(Walker&, Reference arguments, int64_t lowest);
// `forItsOwnElementsAlone`: whoever asks reads the elements that the list has and nothing behind them. Nothing but this code has the list, so they are what its maker laid there, whatever has run since.
std::optional<ListBehindParametersOfArguments> listBehindParametersOfFreshArguments(Walker&, Reference arguments, bool forItsOwnElementsAlone = false);
// The list whose elements are all the elements of the object, which still finds the @@iterator that it was made with. Null: there is none.
Reference listStandingForFreshArguments(Walker&, Reference arguments);
// Every element of an object that is as it was made, where the count is known.
std::optional<Vector<Reference, 8>> elementsOfFreshArguments(Walker&, Reference arguments);
// For a look at a loop. `read` names `object`, a value from in front of the loop, as its operand number `operandIndex`, and takes a key of the type `typeOfKey` where it takes one.
// True: the object is fresh, and in every turn the walk answers the read out of the state or builds a read of a list that runs no code: the object is as fresh behind it as in front of it, and the state knows the same of it.
bool readLeavesFreshObjectAsItIs(Walker&, const BodyValue& read, uint32_t operandIndex, Reference object, TypeID typeOfKey);
struct LoopPreview;
struct ListTestedInFrontOfLoop;
void hoistInvariants(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering); // AOTWalkLoopInvariants.cpp . LoopInvariants . in front of the head of a loop. What a turn builds leaves the loop by Walker::build()
void enterHandler(Walker&, BlockID inputHandler); // AOTWalkHandlers.cpp . Handlers . withheld: the state of a handler is empty
void enterHandlerWithWhatIsKnown(Walker&, BlockID inputHandler); // AOTWalkKnownHandlers.cpp . Handlers . enterHandler() ends in it: a cell that the handler does not name is read again
// The operand that the store lays cells in. Nothing: it lays none, or in more than one.
std::optional<uint32_t> ownerOperandOfStoreOfCells(Walker&, const Draft& store); // AOTWalkKnownBarriers.cpp
Reference nameOfCellBehindForms(const Code&, Reference name); // AOTWalkKnownBarriers.cpp
// `next` is what is about to be built, null in front of a terminator. KnownBarriers::severalStoresHaveMet() holds.
void buildBarrierWhereStoresHaveMet(Walker&, const Draft* next); // AOTWalkKnownBarriersBehindArms.cpp . TypedOperations
// For whoever builds arms that each store into `owner`, behind the arms: one RememberOwner, or nothing where no barrier would run. The sweep drops it where no store waits for it.
void buildBarrierBehindArms(Walker&, Reference owner); // AOTWalkKnownBarriersBehindArms.cpp . TypedOperations
Representation representationOfNumber(const Walker&, TypeID); // AOTWalkNumbers.cpp . UnboxedNumbers . withheld: boxed

// AOTWalkCoroutines.cpp. Never withheld.
bool walkRampOfCoroutine(Walker&); // Walker::walk(), for the role Body of a coroutine. Code is reset and holds no block
Reference addDriverParameterOfCoroutine(Walker&, BlockID entry); // Walker::takeEntryParameters(), for the role CoroutineBody, behind the function's own parameters
Reference convertReturnedValueOfCoroutine(Walker&, Reference returned, Origin); // Walker::returnFromUnit(), of every Return. Any other unit: `returned` as it came
Reference buildAwaitBySavingFrame(Walker&, Reference awaited, std::span<const Reference> held, bool takesExtraTurn, Origin); // for AOTWalkIteration.cpp: for await, async yield*, an async close

// AOTWalkInt32Promises.cpp. In front of whatever stands in the place of a cast into a position with PositionFlags::IsInt32Promise, and of a ReturnCall with such a cast. `entering`: the value of the body being read. It builds nothing.
void checkInt32Promise(Walker&, const DeclaredType&, ValueID entering, Origin);

// AOTWalkKnownTextsAtJoins.cpp . TypedOperations. The record is a join of numbers and written texts alone, made in the loops in which the walk stands: whoever joins it with more may take its operands in its place.
bool isTextBuiltOfParts(Walker&, Reference text);
// For buildTypedJoinOfTexts(). `parts`: one for each operand of the join. True: the parts of each such text that the join alone reads stand in the place of the text. False: `parts` is as it was.
bool takePartsOfTextsThatJoinAloneReads(Walker&, const BodyValue& join, Vector<Reference, 8>& parts, size_t mostParts);

// AOTWalkIteration.cpp. A pattern over a literal that nothing else in the body names, `[a, b] = [b, a]`: the step that is written as number k hands on what the literal lists at k. The walk and the looks both go by this.
struct StepOfListLiteral {
    bool isDone { false };
    // Of the body. Null: undefined.
    ValueID element;
};
// Of an IteratorResume. Nothing: it is no such step, or TypedOperations is withheld.
std::optional<StepOfListLiteral> stepOfListLiteral(Walker&, const BodyIndex&, const BodyValue& step);
// Of an IteratorOpen, IteratorResume or IteratorClose: the iteration goes over such a literal, so the operation runs no code and writes nothing.
bool isPartOfIterationOverListLiteral(Walker&, const BodyIndex&, const BodyValue&);

// AOTWalkCalls.cpp. Who is called, for DirectCalls and for Inlining, which leans on it. Neither builds anything.
// The one function that the call surely enters: the type of `callee` names it, or Link lists it and all that `callee` can be are named closures. Null: the call may enter several, or throw in front of the one.
// `x.m(..)` on a proven receiver: the read of `m` has the type of the closures of the one method (PositionTypes::foundAboveObjectsOf()), and runs no code. Where the callee takes nothing of its function object the sweep drops the read.
FunctionID findOnlyCallee(Walker&, const BodyValue& call, Reference callee);
// The same of whatever has the type.
FunctionID findOnlyCallee(Walker&, const BodyValue& call, TypeID typeOfCallee);
// `new F(..)` of a constructor function: where F is known, the object is made in the place of the call, under the layout of the class, and the body is entered directly. It builds nothing.
bool makesThisOfConstruction(const Walker&, FunctionID constructor);
// TestingSwitch::CheckProofsAtRunTime, for whoever goes by findOnlyCallee(): which of the closures that the type names is called here is Link's word.
void checkProofOfOnlyCallee(Walker&, Reference callee, FunctionID only, Origin);
// The function that replaceCallOfCompiledBuiltin() enters in the place of a Call of a value of the type. Null: the type does not name one built-in without a function object, or the rows of that name no one function.
FunctionID findFunctionBehindCompiledBuiltin(const Walker&, TypeID typeOfCallee);
// An access that surely runs the one accessor is a call of it, with the object as `this` and, of a setter, with `stored` as its one argument: a copy of the callee where Inlining would copy such a call, else a direct call.
// What a getter gives is bound to the access. False: nothing was built. Nobody has the function object in hand, so an accessor that takes something of it is turned away.
// `closure`: the function object, where somebody has it in hand after all.
bool callAccessor(Walker&, const BodyValue& access, FunctionID accessor, Reference thisValue, Reference stored, Reference closure = { });
// What callAccessor() asks of the function itself, for whoever is asked ahead. No: the access stays as it is written.
bool accessorAdmitsDirectCall(Walker&, FunctionID accessor);
// A function of the library that no value of the body holds, and that Link has entered for anybody: it has no captures and takes nothing of its function object. The first builds nothing.
// The second, only where the first has said yes: a direct call beside the operation being walked, with undefined as `this`. It answers with what the call gives. Null: no way leads on.
bool canCallCompilerHelper(Walker&, FunctionID helper);
Reference callCompilerHelper(Walker&, FunctionID helper, std::span<const Reference> arguments, Origin);
// `operation` as a whole is a call of such a function: a copy of it where Inlining would copy such a call, else a direct call. What it gives is bound to `operation`. `thisValue`: null for undefined. False: nothing was built.
bool callFunctionDirectly(Walker&, const BodyValue& operation, FunctionID, Reference thisValue, std::span<const Reference> arguments);
// For Guesses, of a PropGet at which Facts::held() expects a getter that a descriptor has laid (LinkTables::gettersExpectedAtReads): the name is looked up without running code, what it finds is tested, and the code calls it itself.
// Whatever else the name finds goes by the read as it is written. False: nothing was built.
bool callGetterExpectedAtRead(Walker&, const BodyValue& read);
// AOTWalkGuesses.cpp. `$findsDeclaredMember(object, "name")`: A GUESS THAT IS ONE OPERATION. It is stamped with the layouts that are proven or expected of the object, above each of which the records declare a member under the name,
// and asks by itself: the shape, whether the object stands where its class has put it, and the guard that reads ask of each class and the name (GuardEmitter::defineWhetherDeclaredMemberIsFound()). Unstamped it answers false.
bool askGuardsAboutDeclaredMember(Walker&, const BodyValue&);
// AOTWalkGuesses.cpp. `Object.hasOwn(object, "name")`: A GUESS THAT IS ONE OPERATION too. The layouts that are proven or expected of the value of the body being read, where the cell of an object of any of them says by itself
// whether the object has the property: cellAnswersForOwnProperty(). PropHasOwn is stamped with them, tests for them, and asks the run time where the test fails. Null: there are none, or the test does not pay.
LayoutSetID receiversWhoseCellsAnswerForOwnProperty(Walker&, ValueID input, PropertyKeyID);
// AOTWalkTypedAccesses.cpp. Under a name that is no field of its layout a compiled object has a property only among its extra properties, and one bit of the order word says whether it has any.
// A slot is filled only while the property is the object's own, and an empty slot says that there is none while no field of the object has left its slot, which another bit says.
bool cellAnswersForOwnProperty(Walker&, TypeID typeOfObject, PropertyKeyID);
// AOTWalkTypedAccesses.cpp. No object of the type has a field under the key, and from the class of each upward the first record that declares anything under it declares a method or an accessor whose functions it names.
// Whether that still lies there, and nothing in front of it, the guards say.
bool recordsDeclareMemberAboveObjectsOf(Walker&, TypeID typeOfObjects, PropertyKeyID);
// The getter that a PropGet or a SuperGet under the key enters, given the type of what the lookup starts at. Null: it enters none, or nobody can tell.
FunctionID findGetterOfRead(Walker&, Opcode, TypeID lookedAt, PropertyKeyID);
// `forwarded`: a Call or a CallList that stands in the place of `call`, which is written as a call of something else. Where it surely enters one function: a copy of the callee where Inlining would copy such a call, else a direct call.
// What it gives is bound to `call`. False: nothing was built.
bool callForwardedDirectly(Walker&, const BodyValue& call, const Draft& forwarded);
// Of a SuperConstructList whose parent is a closure of `constructor`: the links of the chain hand the list over: the state knows what it holds, or the parameters are read out of it. For whoever asks ahead.
bool listOfSuperCallCanBeHandedOver(Walker&, const BodyValue& call, FunctionID constructor);
// AOTWalkBuiltins.cpp. `has.call(o, k)`, `slice.apply(list, [1])`: what `forwarded` calls is proven to be one built-in. It goes by the rows of that built-in, as `o.hasOwnProperty(k)` does. What it gives is bound to `call`. False: nothing was built.
bool callBuiltinForwarded(Walker&, const BodyValue& call, const Draft& forwarded);
// AOTWalkTailCalls.cpp. For frames. A PROOF: whatever the call in tail position enters comes back, so the caller may keep its frame, and what lies in it, until then. `calleeOfDirectCall`: null where the call goes by value. False claims nothing.
bool tailCallMayKeepFrame(const Walker&, const BodyValue& call, FunctionID calleeOfDirectCall);
// AOTWalkTailCalls.cpp. Of the record that a ReturnCall returns. Allowed: a direct call of a function that ends its tail calls.
TailCallRequirement requirementOfTailCall(const Walker&, Reference call);
// AOTWalkTailCalls.cpp. `return node.visit(x)`: the read is there for the one call that its block returns at once. It goes by the body alone, so the builder and the twin that is asked ahead get one answer.
bool readIsForCallReturnedAtOnce(const Walker&, const BodyValue& read);
// AOTWalkTailCallsForwarded.cpp. `return Reflect.apply(f, t, list)`, `return f.apply(t, list)`: tests of the operands, each arm walks the call through the chain and returns. False: nothing was built.
inline constexpr OpcodeSet opcodesThatForwardTailCallBehindTestsOfOperandsTakes { Opcode::Call };
AOT_LINK_OF_WALK(forwardTailCallBehindTestsOfOperands, opcodesThatForwardTailCallBehindTestsOfOperandsTakes)
// AOTWalkTailCallsForwarded.cpp. `return f.apply(this, arguments)`: an arm for each number of arguments that the parameters hold. `written`: the call of the built-in. False: nothing was built.
bool passOnOwnArgumentsByCount(Walker&, const BodyValue& call, const Draft& written, Reference callee, Reference thisValueOrNull, Reference arguments);

// For Inlining: the tests of the callee's guard stand in front of a copy of the callee, and where one fails the slow version is called in the place of the copy. `entryArguments`: one for each entry parameter, as they came.
Reference callSlowVersion(Walker&, const Draft& site, FunctionID callee, std::span<const Reference> entryArguments, bool isConstruction);

// AOTWalkInlining.cpp. The questions that Inlining asks of a call before it looks at an operand or at the state. A look at a loop asks the same, to learn whether the walk may put a copy in the place of the call.
// `caller`: the function whose body holds the call. `isInLoop`: Walker::isInLoop() of the call.
struct PlaceOfCall {
    FunctionID caller;
    bool isInLoop { false };
    // Where the call is written, for whoever goes by its text. Null: nobody says.
    const BodyIndex* body { nullptr };
    ValueID call;
};
// `site`: the draft of the call. Of its operands it is only asked which are there and which are the same. False: no walk copies the callee there, whatever the operands are.
// AOTWalkTurnsWrittenOut.cpp. For Inlining and for the looks at loops, by the text of the callee alone. By entry parameter: the integer, and the length of the list, that the call is known to hand over.
// By loop of the callee's body: how often its test holds, where every loop is a counter's that stays a known number of times. Nothing: some loop is none, or the turns are too many operations to walk.
struct TextOfCopyRead {
    // By loop of the body.
    Vector<uint32_t, 2> turnsOfLoops;
    // By block of the body: some way leads there, and of a Branch on a known answer the one edge that is taken.
    BitVector isEntered;
    BitVector leadsToTakenAlone;
    BitVector leadsToNotTakenAlone;
};
std::optional<TextOfCopyRead> turnsOfLoopsOfCopyWrittenOut(const Walker&, FunctionID callee, std::span<const std::optional<int64_t>> integersHandedOver, std::span<const std::optional<uint32_t>> lengthsOfListsHandedOver);
// AOTWalkInlining.cpp. Of a call as it is written: by how many arguments it has and by the integers among them, every loop of the callee stays a known number of times. The same answer, and the entry parameter that takes the other arguments as a list.
struct TurnsOfCopyWrittenOut {
    std::optional<unsigned> entryParameterOfList;
    uint32_t firstArgumentListed { 0 };
    TextOfCopyRead text;
};
std::optional<TurnsOfCopyWrittenOut> turnsOfCopyWrittenOutAt(const Walker&, const Draft& site, FunctionID callee, const PlaceOfCall&);
bool mayCopyCallee(const Walker&, const Draft& site, FunctionID callee, const PlaceOfCall&);
// For the walk that stands at the call: whether it copies. `handedOver`: by entry parameter of the callee, what the call hands over, in Code. Null: nothing.
// `callsParameterOfCopy`: what is called is an entry parameter of the body being read.
bool shouldCopyCallee(const Walker&, const Draft& site, FunctionID callee, std::span<const Reference> handedOver, bool callsParameterOfCopy = false);
// By entry parameter of the callee: the operand of the call that a copy binds to it. Null: the call hands nothing over, and the copy makes what the callee then finds.
Vector<Reference, 8> operandsHandedToCopy(const Draft& site, const Function& callee);

// ---- Texts in string builders. AOTWalkTextBuilders.h has the functions. The records stand here because the Walker keeps a list of them.
struct NameOfGrowingText {
    // A value of the loop that holds the text.
    ValueID value;
    // The parameter of the head, a growth, or a parameter where several edges meet. Names of one version hold the same characters.
    ValueID version;
};
// What findGrowingTexts() gives of a parameter of the head of a loop.
struct GrowingText {
    // Of the input.
    BlockID loopHead;
    // Of the head.
    uint32_t parameterIndex { 0 };
    // Ascending by value. The parameter of the head is the first.
    Vector<NameOfGrowingText, 8> names;
    Vector<ValueID, 2> parametersOfInnerHeads;
    // Nothing looks at what enters the loop but joins that give a string whatever it is.
    bool isOnlyConcatenated { false };
    // It says what pays, never what is right.
    bool takesTextOfJSONStringify { false };
    // Some block of the loop has a handler.
    bool standsUnderHandlers { false };
    // Some part behind the front is not hasTextByItself() by the types of the looks.
    bool hasPartsThatMayRunCode { false };
    // Some growth is a call.
    bool growsInCallees { false };
};
// Walker::m_textsInStringBuilders, in the order in which they were begun. A record stays to the end of the unit: a loop built twice has two, and the later one is the one being walked.
struct TextInStringBuilder {
    GrowingText web;
    // Into Walker::m_frames: the frame whose body holds the loop.
    uint32_t frameIndex { 0 };
    // StringBuilderBegin or StackStringBuilderBegin, in front of the loop. Of a text that went on from another loop: that of the other.
    Reference builder;
    // Nothing was appended in front of the loop: `let s = ""`.
    bool beganEmpty { false };
    bool liesInFrame { false };
    // The one string that was appended in front of the loop, and in the frame the cursor behind it.
    Reference firstPart;
    Reference endOfFirstPart;
    // What was made last of what the builder holds, and the block of Code in which. Good only in that block, and null behind every append.
    Reference stringOfState;
    Reference lengthOfState;
    BlockID blockOfState;
};

// The second of the two visits of a function. It reads the body once, forward, and appends to Code. Nothing built is changed.
// A block that no built edge reaches is not walked.
class Walker {
    WTF_MAKE_NONCOPYABLE(Walker);
public:
    Walker(const WalkInputs&, TypeArena&, Arrivals&);
    ~Walker();

    // False after a failure, which the build's diagnostics hold.
    bool walk(CodeUnit);
    // For whoever asks ahead about the body of a function that nothing is built for (typedFormOf()): the walk stands in front of the body, and no value of it has a record. False: the function has no body.
    bool standInFrontOfBodyOf(FunctionID);
    Code& code() LIFETIME_BOUND { return m_code; }
    // AOTWalkBuiltins.cpp, findMethodThatSomeClassGivesUnderKey(): by key and number of arguments.
    struct MethodGivenUnderKey {
        BuiltinClassID builtinClass;
        BuiltinID method;
    };
    UncheckedKeyHashMap<uint64_t, Vector<MethodGivenUnderKey, 2>> methodsGivenUnderKeys;
    // callGetterExpectedAtRead(): the reads of the function asked about last in LinkTables::gettersExpectedAtReads of what is held. Nearly no function has one.
    struct GettersExpectedInFunction {
        FunctionID function;
        std::span<const LinkedGetterAtRead> reads;
    };
    GettersExpectedInFunction gettersExpectedInFunction;
    const Code& code() const LIFETIME_BOUND { return m_code; }

    // ---- What every handler may ask.
    const Program& program() const { return m_inputs.program; }
    const Facts& facts() const { return m_inputs.facts; }
    const Plan& plan() const { return m_inputs.plan; }
    TypeArena& types() const { return m_types; }
    Arrivals& arrivals() { return m_arrivals; }
    WalkState& state() { return m_state; }
    // What lies in the field from here on. Whatever cannot be the mark of an empty field ends the dead zone of the field for good.
    void noteEnvironmentField(Reference environment, FieldReference, Reference value);
    // Of the unit being walked. A SlowBody has Folding and DirectCalls alone.
    bool has(Mechanism mechanism) const { return m_mechanisms.contains(mechanism); }
    bool hidesNames() const { return m_inputs.options.hidesNames; }
    RegExpSummaries* regExpSummaries() const { return m_inputs.regExpSummaries; }
    FormatPatternPlans* formatPatternPlans() const { return m_inputs.formatPatternPlans; }
    LeavePoints* leavePoints() const { return m_inputs.leavePoints; }
    ScratchRealm* scratchRealm() const { return m_inputs.scratchRealm; }
    CodeUnit unit() const { return m_code.unit(); }
    // What holds of an entry parameter of the unit's function whichever way the function is entered: what is laid down without a test, cut by what the callers have proven.
    TypeID typeArrivingAtEntryParameter(unsigned entryParameter, bool believesCallers = true);
    // False in the closed arm of an opened guess.
    bool opensGuesses() const { return m_opensGuesses; }
    // unions-and-narrowing. callThroughMethodTable() builds a CallTable. Walker::staysTailCall() takes one. Both are set where a unit begins.
    bool buildsTableCalls() const { return m_buildsTableCalls; }
    bool tableCallsHaveTailForm() const { return m_tableCallsHaveTailForm; }
    Origin originOfOperation() const { return m_origin; }
    // The function whose body is being read: that of the unit, or a callee that is walked in line.
    ALWAYS_INLINE FunctionID currentFunction() const { return m_frames.last().function; }
    const Body& currentBody() const;
    ALWAYS_INLINE const BodyIndex& currentBodyIndex() const { return *m_frames.last().index; }
    // The operation being walked stands in a loop of the unit, a loop around a call walked in line included.
    ALWAYS_INLINE bool isInLoop() const
    {
        const Frame& frame = m_frames.last();
        if (frame.loopDepthAtCall)
            return true;
        if (!frame.index || !frame.block)
            return false;
        return frame.index->loopDepthOf(frame.block) > (frame.standsInFrontOfHead ? 1u : 0u);
    }
    // The function of the unit, or a callee in whose copy the walk stands.
    bool isBeingWalked(FunctionID) const;
    // AOTWalkLoops.cpp. Of a loop of the body of the function, for the looks: what a walk of this unit that was given up has found of it. walkUnitAgainWithWiderHead(), walkUnitAgainWithLoopsThatMayDoAnything().
    // What the parameter of the head holds at least. Null: nothing was found.
    TypeID leastTypeOfHeadParameter(FunctionID, BlockID inputHead, uint32_t parameter) const;
    bool isLoopThatMayDoAnything(FunctionID, BlockID inputHead) const;
    // How many copies stand around the body being read. 0: it is the unit's own.
    size_t depthOfCopies() const { return m_frames.size() - 1; }
    // The operations of all the bodies that have been copied, one in another, at the call of the unit's own body that is being walked.
    uint32_t operationsCopiedAtWrittenCall() const { return m_operationsCopiedAtWrittenCall; }
    // Of the allocation that is being walked, where its layout has slots that hold marks: the closures that its own block has made and that it alone takes.
    std::span<const Reference> closuresThatAllocationAloneTakes() const LIFETIME_BOUND { return m_closuresThatAllocationAloneTakes.span(); }
    // The call in whose place the body being read is walked stands in a loop of the unit.
    bool copyStandsInLoop() const { return m_frames.last().loopDepthAtCall; }
    BlockID currentBlock() const { return m_block; }
    // With the inline frame of the body being read.
    Origin originOf(const BodyValue& value) const
    {
        Origin origin = value.origin();
        origin.inlineFrame = m_frames.last().inlineFrame;
        return origin;
    }

    // ---- Values.
    // What was built for a value of the body being read, under the narrowest name that holds in the block being walked: boundsProvenOnEdge().
    // No test has narrowed a name where the walk stands and no check has handed one on: what was bound.
    ALWAYS_INLINE Reference referenceOf(ValueID input) const
    {
        const Frame& frame = m_frames.last();
        if (frame.firstNarrowerName == noNarrowerName && !m_state.hasCheckedNames()) [[likely]]
            return frame.references[input.index()];
        return narrowestReferenceOf(input);
    }
    // What bind() was handed. Whoever binds a value anew for a while and binds it back hands bind() this: a narrower name holds only where its test has passed.
    Reference referenceBoundTo(ValueID input) const { return m_frames.last().references[input.index()]; }
    ALWAYS_INLINE void bind(ValueID input, Reference reference)
    {
        Reference& bound = m_frames.last().references[input.index()];
        if (m_numberOfClosedArms) [[unlikely]]
            m_bindings.append(Binding { static_cast<uint32_t>(m_frames.size() - 1), input, bound });
        bound = reference;
    }
    // The proven type: the value's own, or what the state knows of its shape if that is narrower.
    ALWAYS_INLINE TypeID typeOf(Reference reference) const
    {
        if (hasTextsInStringBuilders()) [[unlikely]]
            return typeOfTextOrRecord(reference);
        return typeAsBuilt(reference);
    }
    // THE SECOND WORD BEHIND THE OPCODE (AOTWalkLinks.h has the first). The operand is a value of Code, no constant and nothing that is still to be made, the state has no shape for it, and its type says nothing.
    // A link that leans on what is proven of its operands asks this in front of all else that it does. It says nothing of what is expected or written of the value: findExpectationForGuesses().
    ALWAYS_INLINE bool nothingIsProvenOf(Reference reference) const { return reference && reference.isValue() && m_types.saysNothing(typeOf(reference)); }
    ALWAYS_INLINE Representation representationOf(Reference reference) const
    {
        if (!reference || !reference.isRecord())
            return Representation::Unchosen;
        return m_code.at(reference.id()).representation;
    }
    // Another name of the value comes to be that the state does not follow: the object behind it is fresh no longer. Nothing while EscapeAnalysis is withheld.
    void letLeave(Reference reference) { letLeave(m_state, reference); }

    // An operation of the body being read as it goes through the chain: an access for which keyThatAccessGoesBy() answers is the access by that name, under the id and with the operands of the one that is written.
    // Any other operation comes back as it is written. What comes back lives as long as the body is being read.
    BodyValue asWalked(const BodyValue& written) { return accessesUnderComputedKeys.contains(written.opcode()) ? accessAsWalked(written) : written; }

    // ---- Iterations by index. AOTWalkIteration.cpp.
    // IteratorState is no value of Code. To an IteratorOpen that TypedOperations has taken, Reference::virtualObject() of a place in this list is bound.
    struct IterationByIndex {
        // What the steps read, by the heap that the table gives the contents of the class.
        enum class Contents : uint8_t { List, View, Map, Set, Text };
        // What is iterated. `mayGoByProtocol`: a parameter that holds it, or undefined where the protocol has opened the iteration.
        Reference list;
        // The index that the next step reads, of a text in code units. positionBehindEveryEnd once a step of a list, a view or a text has found nothing there. It is the value that holds where the walk stands: each step stands below the one in front of it.
        Reference position;
        // Of a Map or a Set: the table that the position counts in, and the closed mark once a step has found nothing. It comes round with the position.
        Reference table;
        // While `mayGoByProtocol`. `state`: a parameter that holds the list of the three words of the protocol, or undefined beside a list. Null: the two parameters hold what the protocol has opened, and `position` says whether it is done.
        Reference state;
        Reference iterator;
        Reference next;
        // The list is this literal of the body, and nothing but the iteration names it.
        ValueID literal;
        // Every step was taken where the open stands, and the iterator is closed: what the step that is written as number k hands out lies at k.
        Vector<Reference, 4> takenAtOpen;
        bool isTakenAtOpen { false };
        // The head of the one loop whose turns step an iteration that was opened in front of it: of the body, and of Code once the walk has come there. The position comes round in a parameter of that head.
        BlockID inputHead;
        BlockID head;
        // Frame::inlineFrame of the body that holds the iteration.
        uint32_t inlineFrame { 0 };
        BuiltinClassID builtinClass;
        Contents contents { Contents::List };
        JSC::IterationKind kind { JSC::IterationKind::Values };
        bool mayGoByProtocol { false };
        // The list in `state` has a fourth element: WordOfIteration::StepsInLibrary.
        bool stateHoldsVerdictOnNext { false };
        // %CompilerHelpers%.ownEntriesFlat has made the list: each key with its value behind it. A step hands out the key, and this is what lies behind the key: the pattern that takes the pair apart gets the two.
        bool holdsFlatEntries { false };
        Reference valueBesideKey;
        // Who steps it. None: AOTWalkIteration.cpp. Any other: buildTypedFormOfIteration() takes every operation on the state, and `mayGoByProtocol` is false.
        enum class OtherForm : uint8_t {
            None,
            // AOTWalkForIn.cpp. `list`: the enumerator. `subject`: what is enumerated. `position`: where the next step begins to look. `count`: ForInStepResult::CurrentNames, or null.
            Enumeration,
            // AOTWalkIterationSteps.cpp. `list`, `contents`, `kind`, `position`, `table` as above.
            LoopOverInstance,
            // AOTWalkIterationSplits.cpp. `list`: the text. `subject`: the separator, a text that is not empty. `position`: where the next piece begins.
            PiecesOfSplit,
            // AOTWalkIterationProtocol.cpp. `iterator` and `next` stand in front of the loop. Nothing comes round and nothing lies in memory: `position` and `inputHead` are null.
            Protocol,
        };
        OtherForm otherForm { OtherForm::None };
        Reference subject;
        // A uint32 that comes round behind `position` and `table`. Null: none does.
        Reference count;
        // Of an Enumeration, by its newest step: the ForInStep of the body, where it found its key (ForInStepResult::Position), and the key as an access may take it:
        // a parameter that holds the index itself where the step stood in the indexed part of a list, else the name. Null: the step has one arm (F5 did not apply).
        ValueID newestStep;
        Reference foundAt;
        Reference keyForAccesses;
        // The subject has the names, the prototype and the storage of its making in every run (Link's word, not the state's): a step of a list runs no code.
        bool isAsMadeForGood { false };
        // Nothing reads `position`, `table` or `count` behind the loop: each mention there is a close that does nothing. So each copy of a loop that is built twice may step it from what entered.
        bool isDeadBehindLoop { false };
    };
    // No list has 2^32 elements, and no view 2^35.
    static constexpr int64_t positionBehindEveryEnd = int64_t { 1 } << 35;
    IterationByIndex& iterationByIndex(Reference state) LIFETIME_BOUND { return m_iterationsByIndex[state.object().index()]; }
    Reference addIterationByIndex(IterationByIndex&&);
    // The call of Object.entries, a value of the body being read, was built as %CompilerHelpers%.ownEntriesFlat: isListOfEntriesTakenApartAtOnce().
    void noteListOfFlatEntries(ValueID call) { m_frames.last().listsOfFlatEntries.append(call); }
    bool isListOfFlatEntries(ValueID call) const { return m_frames.last().listsOfFlatEntries.contains(call); }
    // The rows of the table let a list be iterated by index. Nothing: nobody has asked yet.
    std::optional<bool>& listsAreIteratedByIndex() { return m_listsAreIteratedByIndex; }

    // ---- Building. The only ways anything gets into Code.
    // A draft of the opcode with the row's defaults, the fields of the input copied in, operands renamed, flags and origin copied.
    Draft draftOf(const BodyValue&);
    // Brings operands into the representations the row wants, takes the type from the row's result rule and the effects from its declared effects, ends in the state what those write, appends.
    // `foundByCaller`: what effectsOf() of ir/AOTEffectsOfOperations.h takes under that name. It holds in every run, and is cut by the declared bound.
    Reference build(Draft&&, const Effects* foundByCaller = nullptr);
    // Equal constants of constantsInFormOfUses in one form are one record: one number is one Reference, whoever asks for it.
    Reference constant(Draft&&);
    // The last link of every chain: draftOf(), chooseConservativeFields(), build(), bind().
    bool copy(const BodyValue&);
    // AOTWalkChoices.cpp. Every field of the row that must be chosen before the emitters gets the choice that claims nothing.
    void chooseConservativeFields(Draft&);

    // ---- Control.
    BlockID addBlock(BlockFrequency = BlockFrequency::Normal);
    // Ends the current block with a jump and goes on in `next`, which has these parameters. Any number of jumps may lead to one block.
    // Each argument leaves (letLeave()) and is brought into the form of its parameter, the parameter gets the join of the types that arrive, and the states that arrive are met.
    void jumpTo(BlockID next, std::span<const Reference> arguments = { });
    // Ends the current block on `condition`. Neither side is entered: continueIn() does that, with the state as it was at the branch.
    void branch(Reference condition, BlockID taken, BlockID notTaken);
    // Ends the current block on the cases. No side is entered: continueIn() does that, with the state as it was here. The blocks come from addBlock(); several cases may name one block. As branch().
    void switchOn(Reference subject, std::span<const int64_t> cases, std::span<const BlockID> targets, BlockID otherwise); // AOTWalkUnionSwitches.cpp
    void switchOnTexts(Reference subject, std::span<const StringID> cases, std::span<const BlockID> targets, BlockID otherwise); // AOTWalkUnionSwitches.cpp
    // AOTWalkNarrowingOfBranches.cpp. What a Branch that dominates the block being walked has answered: lesser < greater, or lesser <= greater where `admitsEquality`. The two are records behind every name. Nothing: nobody has asked.
    std::optional<bool> outcomeOfRelationAnsweredAbove(bool admitsEquality, ValueID lesser, ValueID greater) const;
    // Ends the current block with a TryCallInline. Neither side is entered. Both blocks come from addBlock(), `done` has no parameter yet, and nothing else leads to it.
    // What the code in line gives arrives in a parameter of `done`, which is the answer. Null: nothing was built. `foundByCaller`: as for build(). Both sides go on without what those effects write.
    Reference tryCallInline(TerminatorDraft&&, BlockID done, BlockID miss, const Effects* foundByCaller = nullptr);
    // A block that was left without an end is found with the state that held there. One that the Walker has never seen, made by Code::addBlock(), is entered with the empty state.
    void continueIn(BlockID);
    // The type may be null: each jumpTo() joins the type of its argument in, whatever was given here.
    Reference addParameter(BlockID, TypeID, Representation);
    // The form of a parameter of a join. `type`: the join of what arrives. `arguments`: the forms in which it arrives. None: the form of the type.
    Representation formOfParameter(TypeID, std::span<const Representation> arguments);
    // They end the current block, under a handler with its edge. The walk stands nowhere behind them.
    // One value for each result of the unit, brought into the form of its place.
    void endWithReturn(std::span<const Reference>, Origin);
    void endWithThrow(Reference, Origin);
    // AOTWalkTextBuilderHandlers.cpp. Of what CatchException has given, at the end of a handler block of a mechanism's own.
    void endWithRethrow(Reference exception, Origin);
    void endAsUnreachable(Origin);
    // AOTWalkLeaveRecord.h says what it takes. It ends the block.
    bool buildLeave(std::span<const LeaveFrame>, Origin);
    bool buildLeaveAtHead(const LeaveAtHead&, Origin);
    Reference buildLeaveOfChain(std::span<const LeaveAtHeadInChain>, bool comesBack, Origin);
    Reference inFormThatCrosses(Reference, Origin);
    bool endWithLeave(std::span<const uint32_t> words, std::span<const Reference> operands, Origin);
    // AOTWalkBranches.cpp. One edge of a terminator that the walk puts in the place of a written one. It leads where target number `targetIndex` of the terminator of `inputBlock` leads, with the arguments and the proofs of that target.
    // `inputBlock`: the block being walked, or a block that the terminator passes by (a later link of a chain of comparisons), to which then no edge may be led.
    struct EdgeToTargetOfBody {
        BlockID inputBlock;
        uint32_t targetIndex { 0 };
    };
    // THE DOOR. Ends the current block with the draft, a Jump, Branch, Switch, SwitchString or SwitchOnShape over references of Code, and leads edge number k of it to edges[k], each with the state that holds here.
    // It does what walkTerminator() does behind its draft: the truth of the condition, kinds, forms, effects, the handler edge, Code::end(), leadEdge(); a Jump that comes round to a head without proofs is closed in the block that it leaves.
    // A proof on edge k goes by k: the taken side for 0 of a Branch, case k of a switch, `otherwise` for the last. edges.size() is the number of edges of the draft. NO TARGET STANDS TWICE. The walk stands nowhere behind it.
    void endBlockLeadingTo(TerminatorDraft&&, std::span<const EdgeToTargetOfBody>);
    // The unit returns at once what the draft gives: a ReturnCall takes it, in the unit's own body or in that of a callee that returns in place of the unit.
    bool unitReturnsAtOnce(const Draft& draft) const { return draft.resultIsReturnedAtOnce && (m_frames.size() == 1 || m_frames.last().returnsInPlaceOfUnit); }
    // What that ReturnCall does, for a way through the operation that does not come to it: `call` leaves by a tail call where it can, and else the unit returns what it gave.
    void returnAtOnce(Reference call);

    // Walks operations [begin, end) of a block of the body being read through their chains, into the current block. For an opened guess, which builds one stretch twice.
    void walkStretch(BlockID inputBlock, uint32_t begin, uint32_t end);
    // For an arm that has walked the call which the block returns at once. True: a return of this frame ends the unit, and the arm has ended so: a call that stays a tail call in one arm need not in the other.
    // False: whoever has walked the callee in line goes on behind the call, so the arms meet as ever.
    bool endArmWithReturnOf(BlockID inputBlock);
    // AOTWalkKnownThreading.cpp. For an arm that has walked its stretch of the block and has bound what the stretch defines: it walks what is left of the block and the Branch or Switch that ends it for itself,
    // so that nothing meets in a join only to be branched on. True: the arm has ended so, and the walk stands nowhere. False: nothing was built, and the arms meet as ever. Each arm asks for itself.
    bool endArmWithBranchOf(BlockID inputBlock);
    // The arms of a guess at `value`, whose stretch ends at `end`, walk the branch of their block though it may go by nothing that they make: a target of it looks at the value again,
    // and walkBlockApartFromClosedWays() will keep the way that holds the proof apart from the way of the closed arm. It builds nothing.
    bool armsTakeBranchForProofOf(BlockID inputBlock, uint32_t end, Reference value);
    // AOTWalkKnownTextsAtJoins.cpp. The walk stands in a loop that was entered behind the record.
    bool standsInLoopEnteredBehind(Reference) const;
    // The `end` of the innermost walkStretch() at work on the body being read.
    uint32_t endOfStretchBeingWalked() const { return m_frames.last().endOfStretch; }
    // Somebody walks a stretch of the block as an arm, and lets what it binds meet what another arm binds by means of his own.
    bool walksStretchInsideAnother() const { return m_frames.last().numberOfStretchesBeingWalked > 1; }
    // AOTWalkGuesses.cpp: placeTakenByEndOfBlockForGuesses() of the block of the body being read that was asked about last. It follows from the body.
    struct PlaceTakenByEndOfBlock {
        BlockID block;
        uint32_t place { 0 };
    };
    PlaceTakenByEndOfBlock& placeTakenByEndOfBlockAskedLast() { return m_frames.last().placeTakenByEndOfBlockAskedLast; }
    // StatisticID::TestsThatRejoin, of the unit.
    void noteGuessOpened() { ++m_numberOfGuessesOpened; }
    // AOTWalkEnforcedFillers.h. The fields of an allocation of this unit in which a filler stands for a value that is stored behind it, under the record of the allocation.
    // The record lists the filler, and the store has run before anybody notes what the allocation has filled in: of such a field the record says nothing.
    void noteFillersOfAllocation(Reference allocation, Vector<FieldIndex, 2>&& fields) { m_fillersOfAllocations.set(allocation.id().index(), WTF::move(fields)); }
    std::span<const FieldIndex> fillersOfAllocation(Reference allocation) const LIFETIME_BOUND
    {
        auto found = m_fillersOfAllocations.find(allocation.id().index());
        return found == m_fillersOfAllocations.end() ? std::span<const FieldIndex>() : found->value.span();
    }
    // AOTWalkGuesses.cpp. What was found to be expected of a value of the body being read, kept while its frame stands: every operation that looks at the value asks.
    struct RememberedExpectation {
        TypeID type;
        bool isDeclared { false };
        bool isHeld { false };
        bool isKnown { false };
        bool admitsNoNumberOrBoolean { false };
    };
    RememberedExpectation& rememberedExpectationOf(ValueID);
    // Of a parameter of a block: ALL that is expected of the value behind the tests that it has passed or failed, the members that no one test tells among them.
    std::optional<RememberedExpectation> wholeExpectationKeptOf(ValueID parameter) const
    {
        const auto& kept = m_frames.last().wholeExpectations;
        auto found = kept.isEmpty() ? kept.end() : kept.find(parameter.index());
        return found == kept.end() ? std::nullopt : std::optional(found->value);
    }
    void keepWholeExpectationOf(ValueID parameter, RememberedExpectation whole) { m_frames.last().wholeExpectations.set(parameter.index(), whole); }
    // Non-null while a look at a loop asks: the values of which something was remembered since. The operands on the road are not built yet, so the walk asks again when they are.
    Vector<ValueID, 16>* expectationsRememberedForLook() { return m_expectationsRememberedForLook; }
    void setExpectationsRememberedForLook(Vector<ValueID, 16>* list) { m_expectationsRememberedForLook = list; }
    // Non-null while a look at a loop asks: the types that the look has given the values of the loop, which are not built yet.
    const ScopedLambda<TypeID(ValueID)>* typesOfInputsInLook() const { return m_typesOfInputsInLook; }
    void setTypesOfInputsInLook(const ScopedLambda<TypeID(ValueID)>* types) { m_typesOfInputsInLook = types; }
    // Some answer since the mark was last taken down has gone by a value that is not built yet. What has not follows from what is built, and the walk would find the same: it stays remembered.
    bool lookHasAskedAboutUnbuiltValue() const { return m_lookHasAskedAboutUnbuiltValue; }
    void setLookHasAskedAboutUnbuiltValue(bool has) const { m_lookHasAskedAboutUnbuiltValue = has; }
    // AOTWalkGuesses.cpp. The names that the body being walked reads, stores or calls off the value, off what only hands it on, and off the parameters of blocks that it is handed to.
    std::span<const PropertyKeyID> namesUsedOff(ValueID);
    // AOTWalkGuesses.cpp. WHAT A ROUND OVER THE LAYOUTS OF A WIDE SET HAS FOUND, by the asker, the set and a number of the asker's. Of sets of the program's table alone: their ids hold for as long as the walker lives, in every unit.
    // TypeArena::rememberRound() keeps such answers for Compile. The planner of parameter guards has an arena that keeps nothing, and a list of names is no word of the arena's.
    enum class RoundKeptByWalker : uint8_t { OnlyLayoutWithAllNames, ShapesOfLayouts, DescribesBagOfOptions, OnlyLayoutWithName };
    static bool keepsRoundsOver(const TypeArena&, LayoutSetID);
    std::optional<uint32_t> roundKept(RoundKeptByWalker, LayoutSetID, uint32_t second) const;
    void keepRound(RoundKeptByWalker, LayoutSetID, uint32_t second, uint32_t answer);
    // Equal lists have one number.
    uint32_t numberOfListOfNames(std::span<const PropertyKeyID>);
    // AOTWalkGuesses.cpp. The edges that enter the block, but for those to a handler. They are listed for the whole body when one of its blocks is first asked about.
    std::span<const BodyIncomingEdge> incomingEdgesOf(const BodyIndex&, BlockID);
    uint32_t numberOfGuessesOpened() const { return m_numberOfGuessesOpened; }
    uint32_t numberOfLoopsBuiltTwice() const { return m_numberOfLoopsBuiltTwice; }
    uint32_t numberOfTurnsThatBeginAgain() const { return m_numberOfTurnsThatBeginAgain; }
    // StatisticID::TypedResultsSelected, of the unit.
    void noteTypedResultSelected() { ++m_numberOfTypedResultsSelected; }
    uint32_t numberOfTypedResultsSelected() const { return m_numberOfTypedResultsSelected; }
    // TestingSwitch::CheckProofsAtRunTime. A value has come into the unit with a type for which Facts::linked() alone vouches: behind it stands a CheckProof, which ends the process where the value is not of the type.
    // AT THE SOURCES, not at each use: tests/README.md, Options. build() asks of every operation that it builds. Whoever decides something by Link's word without a record that has the type calls checkProof() himself.
    // `withoutProof`: what holds of the value whatever Link says.
    // An entry or a guard hands on what it is handed and returns what its body returns: it leans on nothing, and a call of it that returns in its place has nothing behind it.
    bool checksProofs() const { return m_positionsWithoutProofs && (unit().role == CodeRole::Body || unit().role == CodeRole::CoroutineBody); }
    void checkProof(Reference value, TypeID withoutProof, ProofSource, Origin);
    // The value is of `proven`, though its record does not say so.
    void checkProof(Reference value, TypeID proven, TypeID withoutProof, ProofSource, Origin);
    // What Link has proven of a parameter or of the result of a function that is walked at a call, held against the value that stands in that place in the copy.
    void checkProofOfCopy(Reference value, TypeID proven, ProofSource, Origin);
    void checkThatSlotTakes(Reference value, TypeID taken, Origin);
    // No body of a function that is no coroutine awaits. Where what its copy brings is given by an await, the caller's await was built with the call that the copy ends in, and the value is what was awaited.
    void checkProofOfResultOfCopy(Reference value, TypeID proven, Origin origin)
    {
        if (value && value.isValue() && (m_code.at(value.id()).opcode == Opcode::AwaitCallBySavingFrame || m_code.at(value.id()).opcode == Opcode::AwaitBySavingFrame))
            return;
        checkProofOfCopy(value, proven, ProofSource::ResultOfCall, origin);
    }
    // What WalkState::factsOfCell() and orderWordOf() say of the object: AOTWalkTypedAccesses.cpp.
    void checkFactsRememberedOfCell(Reference object, Origin);
    // What elementsOfFreshList() gave of the list: AOTWalkTypedAccesses.cpp.
    void checkElementsRememberedOfList(Reference list, std::span<const Reference> elements, Origin);
    uint32_t numberOfProofsChecked(ProofSource source) const { return m_numbersOfProofsChecked[static_cast<unsigned>(source)]; }
    uint32_t numberOfProofsThatNoTestTells() const { return m_numberOfProofsThatNoTestTells; }
    // While it lives, guesses are not opened and what was bound is forgotten at its end: the closed arm of an opened guess.
    class ClosedArm;
    // While it lives guesses are opened again: the typed run behind the second test of a guess. It stands under the ClosedArm of the first test, which was built where guesses are opened.
    class OpenArm;
    // While it lives: what the rows of the operations that are built rely on beyond the types of their operands holds in every run wherever the operands do, and not only where the walk stands.
    // Without it an operation that relies on a filled slot stays where it is built: LoopInvariants.
    class ProofsHoldEverywhere;

    // Asked ahead (InputsOfTypedOperation::areOnlyTypes()), the builder of an access to an element says here what it would build.
    struct FormAskedAhead {
        Opcode opcode { Opcode::Undefined };
        KnowledgeLevel level { KnowledgeLevel::Unknown };
    };
    // What a file of the walk keeps from unit to unit: it follows from the program, Link and the plan alone.
    struct KeptForBuild {
        virtual ~KeptForBuild() = default;
    };
    std::unique_ptr<KeptForBuild>& keptByTypedAccesses() { return m_keptByTypedAccesses; }
    // What the files AOTWalkCoroutine*.cpp keep while one unit is walked.
    CoroutineStateOfUnit& coroutineState() { return m_coroutineState; }
    const CoroutineStateOfUnit& coroutineState() const { return m_coroutineState; }
    LoopsOverGeneratorsOfUnit& loopsOverGenerators() { return m_loopsOverGenerators; }
    // AOTWalkEnvironmentCells.cpp. Of the unit: whether it has made a cell of an environment that may be handed back, and the values of the bodies being read that name such cells.
    struct EnvironmentCellsOfUnit {
        // A value of a body as one copy of the body is being read: Origin::inlineFrame.
        struct Name {
            const BodyIndex* body { nullptr };
            uint32_t copy { 0 };
            ValueID name;
            // The block of the body that holds the allocation.
            BlockID making;
            // It was drafted more than once.
            bool hasSeveralRecords { false };
        };
        bool hasMadeSome { false };
        // Some record of the unit may be an environment whose byte is clear: it has come as a capture of the function of the unit, or the unit has made it.
        // What is read out of a closure has its byte set: whoever made the closure has set it.
        bool mayNameSome { false };
        Vector<Name, 4> names;
    };
    EnvironmentCellsOfUnit& environmentCellsOfUnit() { return m_environmentCellsOfUnit; }
    // AOTWalkStringStrays.cpp. By function whose body is read in this unit. beginUnit() drops it.
    std::unique_ptr<KeptForBuild>& keptByLadderOfNames() { return m_keptByLadderOfNames; }
    // AOTWalkNumberNotes.cpp
    std::unique_ptr<KeptForBuild>& keptByNumbers() { return m_keptByNumbers; }
    // AOTWalkStrings.h, NotesOfStrings. Of the unit being walked: beginUnit() drops it.
    std::unique_ptr<KeptForBuild>& keptByStrings() { return m_keptByStrings; }
    // AOTWalkPositionsInTexts.cpp. From unit to unit: it follows from the program alone.
    std::unique_ptr<KeptForBuild>& keptByPositionsInTexts() { return m_keptByPositionsInTexts; }
    // AOTWalkLoops.cpp. Of the frame that the walk stands in: what the looks at a loop with loops inside it have found, for the looks at those.
    std::unique_ptr<KeptForBuild>& looksKeptInFrame() { return m_frames.last().looksKept; }

    // ---- AOTWalkLoopInvariants.cpp . LoopInvariants. For whoever makes in front of a loop what build() does not move there by itself: AOTWalkStringFlattening.cpp.
    // Into m_openLoops. Nothing: the walk stands in no open loop, or in a copy built for size.
    std::optional<uint32_t> innermostOpenLoop() const;
    bool standsInFrontOfOpenLoop(uint32_t indexOfLoop, Reference) const;
    // Whether the draft may stand there is the caller's to ask. Null: nothing was built.
    Reference buildInFrontOfOpenLoop(uint32_t indexOfLoop, Draft&&, const Effects* foundByCaller = nullptr);
    // The outermost of the open loops around the walk in front of which the operands stand and something may be made of them that does `ofText`. Nothing: there is none, or the walk stands under a handler.
    std::optional<uint32_t> outermostLoopToMakeTextInFrontOf(std::span<const Reference> operands, const Effects& ofText) const;

    void noteFormAskedAhead(Opcode opcode, KnowledgeLevel level) { m_formAskedAhead = { opcode, level }; }
    FormAskedAhead takeFormAskedAhead() { return std::exchange(m_formAskedAhead, { }); }

    // AOTWalkInlining.cpp. Reads the callee's body in place of the call. The results are bound to `call`.
    // `writtenOut`: draftOfCallWithListWrittenOut() of the call, which then stands for the call as it is written.
    bool walkInline(const BodyValue& call, FunctionID callee, std::span<const Reference> arguments, const Draft* writtenOut = nullptr);

    // ---- Arrivals. See AOTArrivals.h for why every listed call must leave something.
    // Whoever builds a direct call says so here: the proven type of each argument is left for the callee, undefined for the parameters that get none.
    void leaveArrivals(FunctionID callee, std::span<const Reference> arguments);
    // walkOperation(), behind the chain: the operation is a call that Link lists with one callee (calleeAt(currentFunction(), id)), and neither leaveArrivals() nor walkInline() was called for it.
    // It was built in some other form, so Arrivals::leaveAnything().

    void fail(ASCIILiteral what);
    // What the program's author is told. The worker hands it to the build behind the walk.
    void report(const UserDiagnostic& diagnostic) { m_reports.append(diagnostic); }
    Vector<UserDiagnostic> takeReports() { return std::exchange(m_reports, { }); }
    bool hasFailed() const { return m_hasFailed; }
    // What fail() was told first.
    ASCIILiteral failure() const { return m_failure; }
    // No built edge reaches the place where the walk stands. build() gives nothing there, and jumpTo() and branch() lead no edge.
    ALWAYS_INLINE bool isUnreachable() const { return !m_block || m_code.at(m_block).hasEnded(); }

    // The rule of the draft's row over typeOf() of its operands. walk/AOTRepresentationRules.cpp asks it too.
    TypeID resultTypeOf(const Draft&);
    // A handler of the unit stands round what is built next: in the block itself, or round a call that a frame of the walk is copied at.
    bool standsUnderHandler() const { return isUnderHandler(); }
    // Round one build() of a call: what its rule is handed in the place of what Link has united over every call. Null: nothing. Whoever sets it files it as FactUse { OperandSummaries } and clears it.
    void setResultOfCallBeingTyped(TypeID type) { m_resultOfCallBeingTyped = type; }
    // Round the typing of a read under a computed key by a look at a loop, which builds nothing: it hands back what stood there.
    TypeID exchangeResultOfReadBeingTyped(TypeID type) { return std::exchange(m_resultOfReadBeingTyped, type); }
    // What the result rules read of positions: Link's tables and Plan's slots while TypedOperations is on, else nothing. verifyTypesOfCode() has to be handed the same.
    const PositionTypes& positions() const { return *m_positions; }
    // By Facts::held(). NO PROOF: for what is expected of a value, which a test of a guess then asks at each use.
    const PositionTypes& heldPositions() const { return *m_heldPositions; }
    // The type without the closures of each function that LinkTables::mayBeMentionedBy() refuses to the unit being walked.
    TypeID withoutFunctionsThatMayNotBeMentioned(TypeID);
    // As PositionTypes::knowingNothing() answers. For whoever asks what is written, of sets that may be wide.
    const PositionTypes& positionsKnowingNothing() const { return *m_positionsKnowingNothing; }
    // What the rules of types and of effects are asked with here.
    TypingContext typing() const { return { program(), m_types, *m_positions, TypeLevel::Proven }; }

    // ---- Texts in string builders. AOTWalkTextBuilders.h.
    bool hasTextsInStringBuilders() const { return !m_textsInStringBuilders.isEmpty(); }
    // AOTWalkLoopsBuiltTwice.cpp. Of an edge of the body on top: each of the two copies has it.
    bool edgeLeavesLoopBeingBuiltTwice(BlockID to) const { return leavesLoopBeingBuiltTwice(m_frames.size() - 1, to); }
    // AOTWalkLoopsBuiltTwice.cpp. A TEST THAT FAILS WHILE NOTHING OF THE TURN HAS BEEN DONE BEGINS THE TURN AGAIN IN THE CLOSED COPY: both copies are the same program, and what the turn has run so far may be run again unseen
    // (WalkState::nothingWasDoneInTurn()). So no closed arm stands in the typed copy there, nothing comes back, and what the test has proven holds to the end of the turn. ClosedArm does both for whoever builds arms and says so.
    bool mayBeginTurnAgainInClosedCopy() const;
    // For the looks at a loop: the walk is about to enter the head of a typed copy whose failing tests begin the turn again, or asks what such a copy would do.
    bool looksCountOnTurnsThatBeginAgain(BlockID inputHead) const;
    // While such a look asks the operations of the copy of a callee what they do: the frame on top is the callee's, with nothing built for any value, so that whoever is asked about an operation of the copy answers about that body.
    // False: the frames have no room left, and whoever holds a reference to one may not see them move. Then the call does what Link says.
    bool layFrameForLookIntoCopy(FunctionID callee, const BodyIndex&);
    void takeAwayFrameForLookIntoCopy();
    // The walk stands in a turn of a typed copy whose looks count on turns that begin again, and nothing of the turn was done; or such a look stands there.
    bool standsInTurnThatLooksCountOn() const;
    // The look is made for the one answer whether the turns of a typed copy would run other code.
    bool looksAskOnlyWhetherTurnsRunOtherCode(BlockID inputHead) const { return m_inputHeadAskedAboutTypedCopy == inputHead; }
    // While a look stands where nothing of the turn has been done: a typed form that wants a look at a slot or at a cell in front of it counts as built. effectsOfTypedFormAhead().
    bool looksTakeFormsBehindLooksAsBuilt() const { return m_looksTakeFormsBehindLooksAsBuilt; }
    void setLooksTakeFormsBehindLooksAsBuilt(bool take) { m_looksTakeFormsBehindLooksAsBuilt = take; }
    // The walk stands nowhere behind it.
    void beginTurnAgainInClosedCopy();
    // A TEST THAT ANSWERS THE SAME IN EVERY TURN IS ASKED ONCE. mayBeginTurnAgainInClosedCopy() holds, and the reference stands in front of the typed copy, whose block in front is open.
    bool standsInFrontOfTypedCopy(Reference) const;
    // The test stands in front of the typed copy. True: the block in front of the copy has ended with the branch, its failing edge enters the closed copy from in front, and a new block in front is open behind the passing edge.
    // The walk stands where it stood. False: nothing was built.
    bool branchInFrontOfTypedCopy(Reference test, bool passesWithYes);
    // While it lives, what is built is appended to the block in front of the typed copy. The state is that of the inside. standsInFrontOfTypedCopy() has held of the operands.
    class InFrontOfTypedCopy;
    // AOTWalkStringURLs.cpp, NotesOfStrings::urlsThatAreNeverMade.
    bool hasURLsThatAreNeverMade() const { return m_hasURLsThatAreNeverMade; }
    void noteURLThatIsNeverMade() { m_hasURLsThatAreNeverMade = true; }
    Vector<TextInStringBuilder, 2>& textsInStringBuilders() { return m_textsInStringBuilders; }
    size_t indexOfFrameOnTop() const { return m_frames.size() - 1; }
    BlockID inputBlockBeingWalked() const { return m_frames.last().block; }
    // BodyLiveness of the body on top, computed at the first question.
    bool isLiveAtHeadOf(ValueID, BlockID inputBlock);
    // The value is bound anew wherever a block is entered, by what the edges bring: Frame::valuesBuiltTwice. Twice is once.
    void bindAnewAtEveryBlock(ValueID);

private:
    friend class ClosedArm;
    friend class InFrontOfTypedCopy;
    friend class OpenArm;
    friend class ProofsHoldEverywhere;
    friend class KnownJoin;
    friend class KnownThreading;

    static constexpr uint32_t noOpenEdge = UINT32_MAX;

    // An edge of Code that leads to a block of a body which is not entered yet.
    struct OpenEdge {
        // Null: the edge was closed where it was made, into `block`, which nothing else enters.
        EdgeID edge;
        BlockID block;
        uint32_t next { noOpenEdge };
        // One for each parameter of the target, with ownTypeOf() of each.
        Vector<Reference, 4> arguments;
        Vector<TypeID, 4> types;
        // Of a handler edge: empty. StateOfHandler has it.
        WalkState state;
        // For each of Frame::valuesBuiltTwice that is read behind the edge: what stands for it where the edge leaves.
        Vector<std::pair<ValueID, Reference>, 2> names;
        // It was led while no guess is opened: by a closed arm, or by a way of one that has been kept apart since.
        bool isLedByClosedArm { false };
        // Walker::m_firstNarrowerNameOnEdge where the edge was led. Of a handler edge it says nothing.
        uint32_t firstNarrowerName { noNarrowerName };
    };

    static constexpr uint32_t noNarrowerName = UINT32_MAX;
    static constexpr uint32_t narrowerNamesOfDominator = UINT32_MAX - 1;

    // AOTWalkTurnsWrittenOut.cpp. A for-in whose turns are being walked one behind the other, a name a turn.
    struct TurnsOfForInWrittenOut {
        // In m_iterationsByIndex: walking a turn may open iterations, and the list of them moves.
        size_t place { 0 };
        ValueID step;
        Reference enumerator;
        Reference object;
        // The open may have run code of the program, behind the look at the object's own names: a trap of a proxy above it. Whether its list is the object's still is for the enumerator to say, once, in front of the first name.
        bool enumeratorIsAsked { false };
        // Open edges into the head that wait for the loop as written: where a test has failed. They hang in no list meanwhile.
        Vector<uint32_t, 4> edgesIntoLoopAsWritten;
    };

    struct InputBlock {
        uint32_t firstOpenEdge { noOpenEdge };
        // Into Frame::narrowerNames. Until the block is entered: narrowerNamesOfDominator, unless the one edge that enters it has narrowed something.
        uint32_t firstNarrowerName { narrowerNamesOfDominator };
        // Of the head of a loop, once it is entered: where a back edge leads.
        BlockID head;
        bool isEntered { false };
    };

    // The meet of the states at the heads of the blocks that stand under the handler.
    struct StateOfHandler {
        BlockID handler;
        WalkState state;
    };

    // A block of Code that stands for no block of a body: what addBlock() has made, or what continueIn() has left without an end.
    struct PendingBlock {
        BlockID block;
        Vector<ValueID, 4> parameters;
        WalkState state;
        uint32_t numberOfArrivals { 0 };
        bool hasState { false };
        bool parametersAreSet { false };
        bool isMadeByAddBlock { false };
    };

    // While it lives, the frame that holds it is counted among the frames of its function.
    class CountOfFrame {
        WTF_MAKE_NONCOPYABLE(CountOfFrame);
    public:
        CountOfFrame() = default;
        CountOfFrame(CountOfFrame&& other)
            : m_numberOfFrames(std::exchange(other.m_numberOfFrames, nullptr))
        {
        }
        CountOfFrame& operator=(CountOfFrame&& other)
        {
            std::swap(m_numberOfFrames, other.m_numberOfFrames);
            return *this;
        }
        ~CountOfFrame()
        {
            if (m_numberOfFrames)
                --*m_numberOfFrames;
        }

        explicit operator bool() const { return m_numberOfFrames; }
        void count(uint32_t& numberOfFrames)
        {
            ASSERT(!m_numberOfFrames);
            m_numberOfFrames = &numberOfFrames;
            ++numberOfFrames;
        }

    private:
        uint32_t* m_numberOfFrames { nullptr };
    };

    // [0] is the unit's own. Without an index: no body stands behind the unit. What stands below `inlineFrame` the walk fills by itself.
    struct Frame {
        FunctionID function;
        const Body* body { nullptr };
        const BodyIndex* index { nullptr };
        // By value of the body.
        Vector<Reference> references;
        // By value of the body. Empty until the first is asked for.
        Vector<RememberedExpectation> expectations;
        UncheckedKeyHashMap<uint32_t, RememberedExpectation, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> wholeExpectations;
        uint32_t loopDepthAtCall { 0 };
        // Origin::inlineFrame of whatever is built for the body.
        uint32_t inlineFrame { 0 };
        // Of a callee: the unit returns at once what its call gives, so each of its returns ends the unit, and a tail call of its own stays one.
        bool returnsInPlaceOfUnit { false };
        // Of a copy: the plan has counted the frames below the calls of this body for the unit. Whoever lays a frame and does not ask leaves it false, and a call in it makes the unit test: AOTWalkStackTests.cpp.
        bool callsAreCountedByPlan { false };

        // The block being walked. Of a frame below the last: the block that holds the call.
        BlockID block;
        uint32_t endOfValues { 0 };
        // Among the operations of `block`: those in front of it are walked.
        uint32_t walkedUpTo { 0 };
        uint32_t endOfStretch { 0 };
        // walkStretch() inside walkStretch(). One: the walk of the block itself.
        uint32_t numberOfStretchesBeingWalked { 0 };
        PlaceTakenByEndOfBlock placeTakenByEndOfBlockAskedLast;
        bool hasHandler { false };
        // One more than the index of the nearest frame below this one that has a handler, zero where none has. The block that a frame below stands in does not change while this frame lives. Worked out when first asked for.
        static constexpr uint32_t notAskedYet = UINT32_MAX;
        mutable uint32_t numberOfFramesUpToHandlerBelow { notAskedYet };
        // Walker::isBeingWalked().
        mutable CountOfFrame countAmongFramesOfFunction;
        // Among the operations of `block`: those in front of it are the catches of a handler block that jumps reach. They were walked in the handler block of Code, or nothing is thrown on any way here and they are bound to undefined.
        uint32_t firstOperationToWalk { 0 };
        // What only feeds the Throw of `block` was passed by: the Throw becomes a jump or the build stops.
        bool hasPassedByWhatFeedsThrow { false };
        // `block` is the head of a loop, and the walk still stands in front of it.
        bool standsInFrontOfHead { false };
        // What a comparison has narrowed on the one edge that enters a block holds in all that the block dominates. Each block has a list, which goes on in the list of its dominator.
        struct NarrowerName {
            Reference value;
            Reference name;
            uint32_t next { noNarrowerName };
        };
        Vector<NarrowerName> narrowerNames;
        // A bit for each value that some list of the frame names. Most values have no narrower name, and whoever asks for one ends here.
        uint64_t narrowedValues { 0 };
        static constexpr uint64_t bitOfNarrowedValue(Reference value) { return 1ull << ((value.bits() * 0x9E3779B1u) >> 26); }
        // Of `block`.
        uint32_t firstNarrowerName { noNarrowerName };
        // A row of loops leaves a name behind each, which holds in all that follows. So in a long list a name is found by its value, or by the name, in tables that speak of one list.
        // The lists are the ways up a tree, and the walk goes from a block to one that stands near it: the tables are brought from list to list by what the two do not share.
        // (To keep every name of a value and ask of each whether the list holds it is the square again where each loop of the row narrows one value inside itself.)
        static constexpr uint32_t lengthOfLongListOfNarrowerNames = 32;
        bool listOfNarrowerNamesIsLong { false };
        // By name in narrowerNames.
        struct PlaceOfNarrowerName {
            // How many names the list has from this one on.
            uint32_t length { 1 };
            // One further on in the list, so chosen that the name at a given length is reached in as many steps as the logarithm of the list.
            uint32_t jump { noNarrowerName };
        };
        Vector<PlaceOfNarrowerName> placesOfNarrowerNames;
        // All of these from the first long list on.
        bool findsNarrowerNamesByReference { false };
        // By Reference::bits(): a number for whatever some list of the frame names as a value or as a name. Most questions end here.
        UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numbersOfNarrowedReferences;
        // By name in narrowerNames. The last two for a name of listInTables: the next one in that list for the same value, and the next one under the same name.
        struct NarrowerNameInTables {
            uint32_t numberOfValue { 0 };
            uint32_t numberOfName { 0 };
            uint32_t nextOfValue { noNarrowerName };
            uint32_t nextUnderName { noNarrowerName };
        };
        mutable Vector<NarrowerNameInTables> narrowerNamesInTables;
        // By number of the reference: the first name in listInTables.
        struct FirstNarrowerNames {
            uint32_t ofValue { noNarrowerName };
            uint32_t underName { noNarrowerName };
        };
        mutable Vector<FirstNarrowerNames> firstNarrowerNamesInTables;
        mutable uint32_t listInTables { noNarrowerName };

        uint32_t lengthOfNarrowerNames(uint32_t first) const { return first == noNarrowerName ? 0 : placesOfNarrowerNames[first].length; }
        bool listOfNarrowerNamesHolds(uint32_t first, uint32_t place) const;
        void numberReferencesOfNarrowerName(uint32_t place);
        void bringTablesOfNarrowerNamesTo(uint32_t first) const;
        void addNarrowerName(uint32_t& first, Reference value, Reference name);
        // functor(const NarrowerName&) -> IterationStatus, for each name in the list of `block` that is of that value, or under that name, from the first on.
        enum class NarrowerNamesAsked : uint8_t { OfValue, UnderName };
        template<NarrowerNamesAsked, typename Functor> void forEachNarrowerName(Reference, const Functor&) const;
        Reference narrowestNameInLongList(Reference) const;
        // LoopPreview::typesOfValues of the outermost loop whose looks found an invariant, and over it those of each loop inside whose own looks did. The walk has come to its head and may have left it.
        LoopID loopLookedAt;
        uint32_t firstValueLookedAt { 0 };
        Vector<TypeID> typesOfLooks;
        std::unique_ptr<KeptForBuild> looksKept;
        // AOTWalkLoopsBuiltTwice.cpp. What the loops that were built twice define and code behind them reads. Each copy has built its own,
        // so an edge brings what stands for such a value as it brings an argument, and a join at which the edges differ gets a parameter.
        Vector<ValueID, 4> valuesBuiltTwice;
        Vector<ValueID, 1> listsOfFlatEntries;
        // Those whose copies are being walked.
        Vector<LoopID, 2> loopsBeingBuiltTwice;
        // AOTWalkTurnsWrittenOut.cpp. The head of the loop whose turns are being walked one behind the other: it is entered as a join is, and an edge that comes round waits there for the next turn.
        BlockID headOfTurnsWrittenOut;
        // The turns are those of a for-in. Null: of a counter.
        TurnsOfForInWrittenOut* turnsOfForIn { nullptr };
        // What beginTurnsWrittenOut() has added to valuesBuiltTwice for the turns being walked. What a loop around had listed is not among them.
        Vector<ValueID, 4> valuesListedForTurnsWrittenOut;
        // For enterLoopHead(), of the loop as written behind such turns: the edges bring the position of the enumeration under the name of its step. No value of the input stands for it.
        ValueID stepWhosePositionEdgesBring;
        size_t placeOfForInWhosePositionEdgesBring { 0 };
        // AOTWalkKnownThreading.cpp. The block whose terminator is walked once for each of several ways. No edge out of it enters its target alone.
        Vector<BlockID, 2> blocksWalkedForEachWay;
        // So many operations at the head of the join that was entered last the ways into it have walked apart. The next walkStretch() passes over them.
        uint32_t headWalkedForEachWay { 0 };
        // By loop of the body: its place in m_openLoops and one more. 0: it is not open. Empty until the first is opened.
        Vector<uint32_t> placesOfOpenLoops;
        // The handler blocks of the body, in the order of the stream. Nothing: nobody has asked.
        std::optional<Vector<BlockID, 4>> handlerBlocks;
        // By block of the body. Empty until somebody asks.
        Vector<InputBlock> blocks;
        // In no order. The handler blocks of a body stand at the end of its stream, so each state waits until all the rest is walked.
        Vector<StateOfHandler, 1> statesOfHandlers;
        // By handler block, once they are many.
        HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> placesOfStatesOfHandlers;
        // notFound: there is none.
        size_t placeOfStateOfHandler(BlockID handler) const;
        // Null: there is none.
        WalkState* findStateOfHandler(BlockID handler);
        void addStateOfHandler(BlockID handler, WalkState&&);
        // False: there is none, and the state is left as it is.
        bool takeStateOfHandler(BlockID handler, WalkState&);
        Vector<std::unique_ptr<AccessUnderProvenKey>> accessesUnderProvenKeys;
        // AOTWalkNarrowingOfBranches.cpp. What a Branch on a relation of two primitives has answered on the one edge that enters `block`: it holds in all that the block dominates.
        struct AnsweredRelation {
            // Records of Code behind every name: lesser < greater, or lesser <= greater.
            ValueID lesser;
            ValueID greater;
            BlockID block;
            // The one noted before this one for the same two, in whichever block.
            uint32_t older { UINT32_MAX };
            bool admitsEquality { false };
            bool answer { false };
        };
        Vector<AnsweredRelation> answeredRelations;
        // `lesser` above `greater`: the newest in answeredRelations.
        HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> newestAnsweredRelations;
    };

    struct Binding {
        uint32_t frame { 0 };
        ValueID input;
        Reference previous;
    };

    void beginUnit(CodeUnit);
    bool walkBody();
    BodyValue accessAsWalked(const BodyValue& written);
    void walkBlock(BlockID inputBlock);
    void walkOperation(const BodyValue&);
    // True: a guess was opened, and each of its arms has walked the operation.
    bool walkThroughChain(const BodyValue&);
    template<Opcode known> bool walkThroughChainOf(const BodyValue& walked);
    void walkTerminator(const BodyTerminator&);
    void walkJump(const BodyTerminator&);
    // What switchOn() and switchOnTexts() do behind their drafts.
    void endWithSwitchOverOwnBlocks(TerminatorDraft&, std::span<const BlockID> targets, BlockID otherwise);
    // Blocks of the body being read that the block being walked dominates and that nobody enters: what they dominate finds the narrower names that hold here.
    void takeNarrowerNamesPast(std::span<const BlockID> blocksPassedBy);
    void enterJoin(BlockID inputBlock);
    void enterLoopHead(BlockID inputBlock);
    void enterHandlerBlock(BlockID inputBlock);

    void takeEntryParameters();
    void checkProofsOfWhatIsStoredBy(const Draft&);
    void checkProofsOfOperandsOfDirectCall(const Draft&);
    void checkProofsOfWhatComesThroughFusedEntry();
    void checkProofThatSlotIsFilled(const Draft& read);
    bool checkWhatIsRememberedOf(const BodyValue& read);
    void checkProofOfRecord(ValueID record, const Draft&, TypeID proven);
    void checkThatReadGivesValue(Reference value, ProofSource, Origin);
    void beginModuleInBody();
    void standIn(BlockID inputBlock);
    void noteStateAtHeadForHandler();
    // AOTWalkHandlers.cpp. Behind whatever may throw, once the state has lost what that writes.
    void noteStateAtThrowForHandler(std::span<const Reference> thrown = { });

    InputBlock& inputBlock(size_t frameIndex, BlockID);
    uint32_t addOpenEdge(size_t frameIndex, BlockID target);
    Vector<uint32_t, 4> takeOpenEdges(BlockID inputBlock);
    void releaseOpenEdge(uint32_t);
    BlockID addBlockFor(BlockID inputBlock, BlockRole = BlockRole::Ordinary, BlockFrequency = BlockFrequency::Normal);
    void closeEdgesInto(BlockID, BlockID inputBlock, std::span<const uint32_t> edges, bool isHandler);
    void closeEdgeWithArguments(EdgeID, BlockID to, std::span<const Reference> arguments);
    void closeBackEdge(EdgeID, BlockID head, std::span<const Reference> arguments);
    // AOTWalkLoops.cpp. THE LOOKS AT A LOOP AND THE WALK HAVE DISAGREED: the walk has built what lies outside the type, or does more than the effects, that the looks promised. Nothing built can be changed,
    // so the walk of the unit is given up and begun anew, and the looks of the new walk start from what this one has found. The first time a parameter is widened as a look widens it, the second time it goes to the tops of its kinds,
    // from the third on it holds anything. True: the walk is given up: whoever asked returns as behind fail(). False: the build stops, with both sides named: nothing wider is left,
    // or the verifier of Code is on (test runs, --aot-check, health), so that our own tests go on seeing every disagreement.
    bool walkUnitAgainWithWiderHead(BlockID inputHead, uint32_t parameter, TypeID previewed, TypeID found);
    bool walkUnitAgainWithWiderHeadOfCode(BlockID head, size_t parameter, TypeID previewed, TypeID found);
    bool walkUnitAgainWithLoopsThatMayDoAnything();
    // THE ALARM, of every operation that the walk BUILDS in a loop, whatever was written there: it writes nothing and runs nothing that the looks at the loops around have not counted.
    // A call that Link lists with one callee is held to what Link says of that callee, as LooksAtLoops::addEffectsOf() counts it. True: all is counted.
    bool looksAtLoopsAroundHaveCounted(const Draft&, const Effects&);
    // Of the newest call of walkOperation(): for the words of the alarm alone.
    Opcode m_opcodeBeingWalked { Opcode::Undefined };
    void giveUpWalkOfUnit();
    struct LoopOfWalkGivenUp {
        FunctionID function;
        BlockID inputHead;
        bool mayDoAnything { false };
        Vector<TypeID, 8> leastTypes;
        Vector<uint8_t, 8> numbersOfWalksGivenUp;
    };
    LoopOfWalkGivenUp& loopOfWalkGivenUp(FunctionID, BlockID inputHead);
    Vector<LoopOfWalkGivenUp, 2> m_loopsOfWalksGivenUp;
    bool m_walksUnitAgain { false };
    void endWithJumpInto(BlockID to, std::span<const Reference> arguments);

    // AOTWalkLoopInvariants.cpp. A loop whose last block the walk has not passed, and that never suspends.
    struct OpenLoop {
        // It has no end yet. Null: LoopInvariants is withheld, and nothing leaves the loop.
        BlockID blockInFront;
        BlockID head;
        // What the jump to the head hands over, in the forms of the parameters.
        Vector<ValueID, 8> entering;
        // They have ended with branches on tests of guesses: branchInFrontOfTypedCopy().
        Vector<BlockID, 2> earlierBlocksInFront;
        // Appended to `blockInFront` since the loop was entered.
        Vector<ValueID, 8> builtInFront;
        // Empty while they are few. A power of two of places, at most half of them taken, each found from the hash of its record on.
        Vector<ValueID> tableOfBuiltInFront;
        // LoopPreview::effects.
        Effects effects;
        // Into m_frames, and the loop of that body.
        uint32_t frame { 0 };
        LoopID loop;
        // The records of the loop have the numbers from here on.
        uint32_t firstRecord { 0 };
        Origin origin { };
        // AOTWalkExceptionsPolls.cpp. The LoopHint of the head, once it is built.
        ValueID poll;
        // LoopHint [throwsOwedError] in a block of its own in front of the block in front of the loop.
        ValueID pollInFront;
        uint32_t numberOfEdgesThatCameRound { 0 };
        // Or adds a field: something may come to be owed in a turn.
        bool likelyTurnMayCollect { false };
        // They stand in a turn of this loop, and go where its own poll leaves its handler.
        Vector<ValueID, 2> pollsInFrontOfLoopsInside;
    };
    void openLoop(OpenLoop&&);
    void closeNewestLoop();
    // ---- AOTWalkBranches.cpp . Folding
    // walkTerminator(), in front of its draft. True: the block has ended, or the walk has failed. False: nothing was built.
    // Throw, Rethrow: jumpToHandlerInPlaceOfThrow(). Branch: onlyTargetWhereAllComeToOnePlace() . branchOnTestBelowNegations(). Switch, SwitchString, SwitchOnShape: endBlockWithCasesLeft().
    bool leaveBlockAnotherWay(const BodyTerminator&);
    // Of a Branch. Both targets come to one block with the same values through blocks in which nothing is seen: the one that is walked, as written. Nothing: the branch decides something.
    std::optional<uint32_t> onlyTargetWhereAllComeToOnePlace(const BodyTerminator&) const;
    // The condition that was built is a LogicalNot: the Branch asks what stands below the negations, with its edges changed round where their number is odd. False: it is none.
    bool branchOnTestBelowNegations(const BodyTerminator&);
    // While endBlockLeadingTo() leads an edge: the number of that edge in Code. draftOfNarrowedOnEdge() goes by it in the place of the number of the written target. Nothing: the two are one.
    std::optional<uint32_t> m_edgeOfCodeBeingLed;
    // ---- AOTWalkBranchesSwitches.cpp . Folding
    // casesLeftOfSwitch() lists fewer cases than are written: a Jump where none is left, else the terminator with the cases that are left. False: all are left, and nothing was built.
    bool endBlockWithCasesLeft(const BodyTerminator&);
    // ---- AOTWalkExceptions.cpp . Handlers
    // The handler that leadHandlerEdge() would lead to, of whichever frame, is a `catch` of the program of which readsNothingOfWhatItCatches() holds, is no head of a loop, is not entered yet, and its body has no irreducible flow.
    bool handlerInForceIgnoresWhatIsThrown() const;
    // True: the block has ended with a Jump, and an open edge brings the handler block what the handler edge would bring.
    bool jumpToHandlerInPlaceOfThrow(const BodyTerminator&);
    // walkBlock(). From Frame::firstOperationToWalk on, and up to the operations that only feed a Throw which becomes a jump, where nobody sees more of each than whether it throws.
    void walkOperationsOf(BlockID inputBlock);
    // enterHandlerBlock(), in front of its block. False: every edge is a handler edge, and nothing was done.
    // True: the walk stands in an ordinary block behind the head of the handler, with the state of the handler, and Frame::firstOperationToWalk passes the catches by.
    bool enterBehindHeadOfHandlerBlock(BlockID inputBlock, std::span<const uint32_t> edges);
    // ---- AOTWalkExceptionsPolls.cpp
    // build(), last. A poll behind a store that may leave an error owed under a handler: never withheld. What the loops around learn of the record. It may build a LoopHint and go on in another block.
    void noteRecordBuiltForPolls(ValueID, const Draft&, const Effects&);
    // enterLoopHead(), while the parameters of the head are still bound to what enters.
    void buildPollInFrontOfLoop(BlockID inputHead, const LoopPreview&);
    // closeNewestLoop(), first.
    void settleFateOfPoll(const OpenLoop&);
    // closeBackEdge(), first.
    void noteEdgeCameRound(BlockID head);
    // From buildPollInFrontOfLoop() to openLoop(), which takes it.
    ValueID m_pollInFrontOfLoopBeingEntered;
    // functor(place in m_openLoops) -> IterationStatus, for each of the first `numberOfLoops` open loops that stands around the place of the walk, the newest first.
    // A row of loops may all be open at once: the stream may have their bodies behind all that follows them.
    template<typename Functor> void forEachOpenLoopAround(uint32_t numberOfLoops, size_t firstFrame, const Functor&) const;
    void leaveBlockOpenInFrontOfLoop(BlockID inputHead, BlockID head, std::span<const Reference> entering, const Effects& effectsOfLoop);
    void endBlocksInFrontOfLoops(size_t frameIndex, BlockID next);
    bool standsInFrontOfLoop(uint32_t indexOfLoop, Reference) const;
    // Into m_openLoops. Nothing: the draft is built where the walk stands.
    std::optional<uint32_t> findLoopToBuildInFrontOf(const Draft&, const Effects&) const;
    ValueID findSameInFrontOfLoop(uint32_t indexOfLoop, const Draft&) const;
    void noteBuiltInFrontOfLoop(uint32_t indexOfLoop, ValueID);
    void endBlocksInFrontOfLoopsFrom(size_t numberOfLoopsKept);

    // AOTWalkLoopsBuiltTwice.cpp. The walk stands in the block in front of the loop, and the parameters of the head are bound to `entering`.
    // True: both copies are walked, and the walk stands nowhere. False: nothing was built.
    bool buildLoopTwice(BlockID inputHead, const LoopPreview&, std::span<const Reference> entering);
    // AOTWalkTurnsWrittenOut.cpp. The walk stands in the block in front of the loop, and the parameters of the head are bound to `entering`. True: the turns are walked, and the walk stands nowhere. False: nothing was built.
    bool writeOutTurnsOfLoop(BlockID inputHead, std::span<const Reference> entering);
    // `for (const name in point) s += point[name]`: behind the tests that the object has its own keys in one order, a turn a name, and the loop as written for whatever else and for the step that finds the end. As above.
    bool writeOutTurnsOfForIn(BlockID inputHead, std::span<const Reference> entering);
    // What both ask first of the frame, and what they leave behind them. Between the two the edges that leave the loop bring what the turns define.
    void beginTurnsWrittenOut(BlockID inputHead);
    // The walk stands in the block in front of the first turn, or nowhere. It walks the blocks of the loop so many times, and then once more as a loop for whatever still comes round. The walk stands nowhere behind it.
    void walkTurnsWrittenOut(BlockID inputHead, std::span<const Reference> entering, uint32_t numberOfWalks, TurnsOfForInWrittenOut*);
    // The walk stands in a block without an end, which goes on in the loop as written from the position.
    void leadIntoLoopAsWrittenBehindTurns(TurnsOfForInWrittenOut&, BlockID inputHead, std::span<const Reference> arriving, Reference position);
    // For walkBlock(), behind the join at the head of a turn.
    void standAtHeadOfTurnOfForIn(BlockID inputHead);
    // Frame::handlerBlocks of the frame on top, listed where nobody has asked yet.
    std::span<const BlockID> handlerBlocksOfFrameOnTop();
    // Of the typed copy that is being walked: the block that leads into the closed copy. It takes what the head takes, in the types that the look in front of both copies has settled for every turn.
    struct WayIntoClosedCopy {
        size_t frameIndex { 0 };
        BlockID inputHead;
        BlockID entry;
        // The parameters of the head of the typed copy. Empty: the head is not entered yet, or `entry` does not take what it holds.
        Vector<Reference, 8> heldAtHead { };
        // It hands the closed copy what enters the loop. No parameters.
        BlockID startFromInFront { };
        Vector<GuessCountedOnByLooks, 4> guessesCountedOn { };
        // previewTypedCopy() has found that the turns of the typed copy run no other code.
        bool looksCountOnTurnsThatBeginAgain { false };
        bool isEntered { false };
        // Given where the head of the typed copy is entered, never twice in a unit: WalkState::nothingWasDoneInTurn().
        uint32_t numberOfTurn { 0 };
    };
    Vector<WayIntoClosedCopy, 2> m_waysIntoClosedCopies;
    uint32_t m_numberOfTypedCopiesEntered { 0 };
    // buildLoopTwice(), while it asks what a typed copy would do.
    BlockID m_inputHeadAskedAboutTypedCopy;
    // buildLoopTwice() has made it under the very types with which the typed copy is entered.
    std::unique_ptr<LoopPreview> m_previewOfTypedCopyAboutToBeEntered;
    Vector<ValueID, 16>* m_expectationsRememberedForLook { nullptr };
    const ScopedLambda<TypeID(ValueID)>* m_typesOfInputsInLook { nullptr };
    mutable bool m_lookHasAskedAboutUnbuiltValue { false };
    bool m_looksTakeFormsBehindLooksAsBuilt { false };
    // layFrameForLookIntoCopy(): how many frames on top are laid for looks, and m_operationsCopiedAtWrittenCall as the walk left it.
    uint32_t m_numberOfFramesLaidForLooks { 0 };
    uint32_t m_operationsCopiedAtWrittenCallInWalk { 0 };
    // enterLoopHead(), of every head, where the walk goes on in it.
    void noteHeadOfTurn(BlockID inputHead);
    void proveWhatLooksCountOnAt(const BodyValue&);
    // buildLoopTwice(): what the looks find of a typed copy whose failing tests begin the turn again. Nothing: no such copy can be built, or the turns run no other code as they are.
    // `lists`: what the row in front of the loop will have shown.
    std::unique_ptr<LoopPreview> previewTypedCopy(BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, std::span<const ListTestedInFrontOfLoop> lists);
    // By the text and by where the walk stands alone.
    bool turnsOfTypedCopyMayBeginAgain(BlockID inputHead, const LoopPreview&) const;
    // Into m_openLoops. Nothing: the walk does not stand in a turn that may begin again, or the block in front of the copy takes no more.
    std::optional<uint32_t> openLoopOfTypedCopy() const;
    // The first operation of the head behind the hint, where it is a read by name off a value from in front of the loop that is typed on an object as made and on no other. Null: there is none.
    // `tested`: what the row in front of the loop asks of values, by value.
    ValueID findReadAskedAtHeadOfLoop(BlockID inputHead, std::span<const std::pair<Reference, TypeID>> tested);
    // The walk stands at that read in the head of a typed copy. True: the object is asked whether it is as made, the failing edge leads into the closed copy, and the walk goes on where the test has passed.
    bool askObjectAtHeadOfTypedCopy(const BodyValue& read);
    struct IterationsSteppedAtHead {
        // The place in m_iterationsByIndex, and the record as it stood in front of the loop.
        Vector<std::pair<uint32_t, IterationByIndex>, 2> asTheyEntered;
    };
    // Each iteration whose inputHead this is has isDeadBehindLoop. It goes through m_iterationsByIndex from the back and stops as enterLoopHead() does.
    bool iterationsSteppedAtMayBeBuiltTwice(BlockID inputHead) const;
    IterationsSteppedAtHead takeIterationsSteppedAt(BlockID inputHead) const;
    // Behind the first copy: the records are what they were, without a head, and count again among m_numberOfIterationsInFrontOfTheirLoops.
    void putBackIterationsSteppedAt(const IterationsSteppedAtHead&);
    void walkCopyOfLoop(BlockID inputHead, std::span<const Reference> entering);
    bool leavesLoopBeingBuiltTwice(size_t frameIndex, BlockID to) const;
    void takeNamesOntoEdge(OpenEdge&, size_t frameIndex, BlockID to);
    bool isWithoutNameBehindLoopBuiltTwice(ValueID);
    bool isWalkingBlockForEachWay(size_t frameIndex) const;
    // For walkBlock(). True: the block is walked, once closed for the ways that closed arms have led and once for the others, which hold a proof that the block leans on. False: nothing was built.
    bool walkBlockApartFromClosedWays(BlockID inputBlock);
    // For walkThroughChain(): inlineCallee(), as the link of the chain. Behind that link alone nobody goes on with the call.
    bool inlineCalleeAsLinkOfChain(const BodyValue& call);
    bool inlineCalleeAsLinkOfChainOfItsOpcodes(const BodyValue& call);
    bool isCopiedByLinkOfChain(const BodyValue& call) const { return m_callCopiedByLinkOfChain == call.id() && m_numberOfFramesAtCallCopiedByLinkOfChain == m_frames.size(); }
    ValueID m_callCopiedByLinkOfChain;
    size_t m_numberOfFramesAtCallCopiedByLinkOfChain { 0 };
    // Of what is built for an access of the body being read: it does no more than the looks at the loops of that body around it have counted on, by effectsOfTypedFormAhead().
    bool looksAtLoopsAroundHaveSeen(const Effects&) const;
    void leadEdge(EdgeID, BlockID from, const BodyTerminator&, uint32_t targetIndex, BodyTarget);
    // Nothing where the block has a handler edge, whoever gave it one.
    void leadHandlerEdge();
    bool isUnderHandler() const;
    // One more than the index of the innermost frame that has a handler. Zero: none has. Every operation that may throw asks, in a copy however deep.
    size_t numberOfFramesUpToHandlerInForce() const
    {
        size_t numberOfFrames = m_frames.size();
        if (!numberOfFrames || m_frames[numberOfFrames - 1].hasHandler)
            return numberOfFrames;
        uint32_t below = m_frames[numberOfFrames - 1].numberOfFramesUpToHandlerBelow;
        return below != Frame::notAskedYet ? below : findFramesUpToHandlersBelow();
    }
    size_t findFramesUpToHandlersBelow() const;
    // AOTWalkTextBuilderHandlers.cpp . Handlers, of texts that lie in builders.
    // The place in m_textsInStringBuilders of the text that the value names and that lies in its builder where the walk stands in the frame.
    std::optional<size_t> findTextHeldInFrame(size_t frameIndex, ValueID) const;
    // leadHandlerEdge(), once the arguments are collected. A handler block in the loop of the text is handed the builder. False: whoever asked leads the edge.
    // For one behind the loop a handler block of Code's own makes the strings and throws on. True: the edge is led.
    bool leadHandlerEdgeBehindStringsOfTexts(size_t frameIndex, BodyTarget, Vector<Reference, 4>& arguments, Vector<TypeID, 4>& types);
    // enterHandlerBlock(), behind the edges: the names that are live at the head of a handler block in the loop are bound to the builder.
    void bindTextsAtHeadOfHandlerBlock(BlockID inputBlock);
    // AOTWalkTextBuilderSpans.cpp. closeEdgesInto(), in front of the parameters. `s += c ? "a" : b`: each edge appends what it brings for the parameter in a block of its own, and brings the empty text.
    void appendPartsOfTextsOnEdgesInto(BlockID inputBlock, BlockID entered, std::span<const uint32_t> edges);
    // A Narrowed where the test or the case cuts the type of the subject. The subject itself where TypedOperations is withheld, the subject is fresh or no value,
    // the branch was folded, the test that was built speaks of another value, or the type stays as it is.
    Reference narrowOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult&);
    // Folding. `value` is a Boolean, and the Branch that ends `from` asks it, a negation of it or what it negates: the constant that it is on that edge. Null: the Branch asks something else.
    Reference booleanAskedByBranchOnEdge(BlockID from, uint32_t targetIndex, Reference value);
    // leadEdge(), of an edge that enters `to` alone. Whoever reads such a Boolean in what `to` dominates reads the constant.
    void nameBooleansAskedByBranch(BlockID from, const BodyTerminator&, uint32_t targetIndex, BlockID to);
    // Nothing: as above where the subject itself is handed on. It builds nothing.
    // `holderOfSubject`: what the subject was read out of, which is what is narrowed then.
    std::optional<Draft> draftOfNarrowedOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult&, Reference holderOfSubject = { });
    bool narrowHolderOfTestedFieldOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult&, BlockID to);
    struct MembersByTag {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(MembersByTag);
        UncheckedKeyHashMap<uint64_t, Vector<LayoutID, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> members;
    };
    const MembersByTag* findMembersByTag(LayoutSetID, const Operation& read);
    void shareMembersByTag(LayoutSetID whole, LayoutSetID part, PropertyKeyID);
    Vector<LayoutID, 4> membersThatHoldTag(const MembersByTag&, LayoutSetID, uint64_t tag);
    std::optional<TerminatorDraft> draftOfSwitchOverShapes(const TerminatorDraft& overTags, Vector<uint32_t, 16>& casesLeft);
    bool endBlockWithSwitchOverShapes(const BodyTerminator&);
    void askShapeInPlaceOfTagAtBranch(const BodyTerminator&);
    bool isNameOfCellAskedForShape(Reference name, ValueID asked) const;
    std::optional<Draft> draftOfHolderNarrowedByShape(BlockID from, uint32_t targetIndex, Reference holder, Reference test);
    Vector<std::unique_ptr<MembersByTag>> m_listsOfMembersByTag;
    // By the set of layouts above the key: the place in the list above, + 1. 0: there is no list for them.
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_membersByTagOfSets;
    bool isReadOfSlotOffHolder(Reference read, Reference holder, PropertyKeyID) const;
public:
    // AOTWalkFolding.cpp asks. Null: the comparison is built as it is written.
    Reference buildTestOfTagAsTestOfShape(const BodyValue& comparison);
    bool isAskedByBranchOfItsBlock(const BodyValue& comparison) const;
    Reference tagReadInFrontOfTest(const BodyValue& read);
private:
    void noteNarrowerName(BlockID to, Reference value, Reference name);
    // ---- unions-and-narrowing.
    // walkTerminator(), behind the casts of a ReturnCall. True: the block has ended and its edges are led. It asks, in this order: decideBranchWhereWalkStands . endBlockWithSwitchOverAlternatives . endSwitchWithFewCasesLeft.
    bool endBlockInAnotherWay(const BodyTerminator&); // AOTWalkNarrowingOfBranches.cpp
    // `return k === 1 || k === 4 || k === 9`, `const is = k === 1 || k === 4`: each link hands its own answer to ONE join, the last by a Jump: one Switch whose listed cases lead there with `true`, the rest with `false`.
    bool endBlockWithSwitchOverAlternatives(const BodyTerminator&); // AOTWalkUnionSwitches.cpp
    // ---- AOTWalkTestsOnFields.cpp
    // walkTerminator(), in front of endBlockInAnotherWay(). TypedOperations. `if (n.kind === A || n.kind === B) ..`, `if (n.kind === A) .. else if (n.kind === B) ..`, n a local: a Branch on a StrictEq with a constant whose two edges hand on the proof of a field that was read out of its slot.
    // The block ends in ONE Switch | SwitchString, and the blocks between the comparisons are never walked. True: the block has ended, or the walk has failed. False: nothing was built.
    bool endBlockWithSwitchOverTestsOfField(const BodyTerminator&);
    struct NameOnEdge {
        // As referenceOf() gives it in the block that the edge leaves.
        Reference latest;
        // What a Narrowed on the edge takes: the newest name that the verifier can follow back to what the test has looked at.
        Reference name;
        // `latest` is no such name: a check or a parameter of a join stands between.
        bool isOlderThanLatest { false };
    };
    struct EdgeBeingNarrowed {
        EdgeID& edge;
        BlockID to;
        bool entersAlone;
        // False: the edge comes round to the head of a loop, where nobody asks for the names of an edge.
        bool keepsNames;
        Vector<Reference, 8>& arguments;
        // For TestingSwitch::CheckProofsAtRunTime: each holder that a field has narrowed, with the type that it had in front.
        Vector<std::pair<Reference, TypeID>, 2> holdersNarrowed;
    };
    // From narrowFurtherOnEdge() to the OpenEdge: the list of the block that the edge leaves, and in front of it what the edge itself has narrowed.
    uint32_t m_firstNarrowerNameOnEdge { noNarrowerName };
    // Set by whoever hands closeEdgesInto() every edge that enters the block. Any other caller closes some of them, and they agree on nothing.
    bool m_closesEveryEdgeIntoBlock { false };
    // The last question of fieldTellsHoldersApart(). It is emptied with each unit: a set of the thread dies with it.
    LayoutSetID m_holdersAskedAboutLast;
    PropertyKeyID m_keyAskedAboutLast;
    bool m_fieldTellsHoldersApart { false };
    void endWithoutEdges(TerminatorDraft&);

    void walkReturn(const BodyTerminator&);
    void returnFromUnit(Reference returned);
    bool staysTailCall(Reference call) const;
    // AOTWalkStackTests.cpp
    void noteDirectCallForStackTests(const Draft&);
    bool callsOfCopyAreCountedForStackTests(const BodyValue& call, FunctionID callee) const;
    // AOTWalkTailCallsPastCleanups.cpp
    bool throwsPastHandlerOf(BodyBlock) const;

    PendingBlock* findPendingBlock(BlockID);
    PendingBlock& pendingBlock(BlockID);
    void settleParametersOf(BlockID);
    void arriveAt(BlockID, WalkState&&);

    OperandTypes typesOfOperands(const DraftWords&);
    // `listsOwnTypes`: m_operandTypes holds typeOf() of each operand afterward.
    TypeID resultTypeOf(const Draft&, bool& listsOwnTypes);
    // What Code says of the record, which never ends. Whatever outlives the present point is typed by it: a parameter of a block, an entry parameter of a callee.
    ALWAYS_INLINE TypeID ownTypeOf(Reference reference) const { return reference && reference.isRecord() ? m_code.at(reference.id()).type : TypeID(); }
    // What typeOf() gave before a text could lie in a builder: for whoever holds operands against the kinds that an operation takes.
    ALWAYS_INLINE TypeID typeAsBuilt(Reference reference) const
    {
        if (!reference || !reference.isRecord())
            return { };
        if (reference.isValue()) {
            if (TypeID shape = m_state.shapeOf(reference))
                return shape;
        }
        return m_code.at(reference.id()).type;
    }
    TypeID typeOfTextOrRecord(Reference) const;
    Reference narrowestReferenceOf(ValueID input) const;
    bool checkOperandKinds(const TerminatorDraft&);
    // There is no operand, or the one of a Branch is a boolean in the form in which a Branch takes it: askTruthOfOperandsNotProvenBoolean(), checkOperandKinds() and convertOperands() would leave the draft as it is.
    bool operandsAreAsTaken(const TerminatorDraft&) const;
    // Its declared bound is pure: it ends nothing in the state and leads no handler edge.
    static bool isDeclaredPure(TerminatorKind);
    template<typename DraftType> void askTruthOfOperandsNotProvenBoolean(DraftType&);
    TypeID typeOfResultAskedFor(const Draft& resultAt);
    TypeID typeOfNewTargetBehind(Reference prototype) const;
    TypeID joinTypes(TypeID, TypeID);
    Representation boxedFormOf(TypeID);
    Reference referenceTo(ValueID id) const { return m_code.isConstant(id) ? Reference::constant(id) : Reference::value(id); }
    Reference convert(Reference, Representation wanted, Origin);
    // Null: build() has to be asked.
    Reference tryBuildConversionOnStraightRoad(Opcode, Representation to, Reference, Origin);
    // Null: the form takes code.
    Reference convertWithoutCode(Reference, Representation wanted);
    template<typename DraftType> void convertOperands(DraftType&);
    void letLeave(WalkState&, Reference) const;
    // In m_sharedConstants, from the hash of the words on.
    void addSharedConstant(unsigned hashOfWords, uint32_t place);
    ValueID findCopyOfConstant(ValueID, Representation) const;
    void noteCopyOfConstant(ValueID, Representation, ValueID copy);

    const WalkInputs& m_inputs;
    TypeArena& m_types;
    Arrivals& m_arrivals;
    const Mechanisms m_mechanismsOfBuild;
    Mechanisms m_mechanisms;
    Code m_code;
    NumbersOfWalkStates m_numbersOfStates;
    AnswersOfWalk m_answersOfStates;
    WalkState m_state;
    // All but the origin.
    TerminatorDraft m_draftOfJump { TerminatorKind::Jump };
    BlockID m_block;
    // By function. Empty until the frames are many. In front of the frames, which count down in it when they go.
    mutable Vector<uint32_t> m_numbersOfFramesOfFunctions;
    Vector<Frame, 4> m_frames;
    bool m_opensGuesses { true };
    PrintStream* const m_explainsClosedAccessesTo;
    uint32_t m_numberOfGuessesOpened { 0 };
    uint32_t m_numberOfLoopsBuiltTwice { 0 };
    uint32_t m_numberOfTurnsThatBeginAgain { 0 };
    UncheckedKeyHashMap<uint32_t, Vector<FieldIndex, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fillersOfAllocations;
    uint32_t m_numberOfTypedResultsSelected { 0 };
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_roundsKept;
    // By number, and by hash the numbers of the lists that have it.
    Vector<Vector<PropertyKeyID>> m_listsOfNames;
    HashMap<uint64_t, Vector<uint32_t, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_numbersOfListsOfNames;
    std::array<uint32_t, numberOfEnumerators<ProofSource>> m_numbersOfProofsChecked { };
    uint32_t m_numberOfProofsThatNoTestTells { 0 };
    // LinkTables::leadsTo() from the function of the unit, as far as checkProof() has asked.
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_functionsInReachOfUnit;
    bool m_callIsAccountedFor { false };
    // Whose arrivals that speaks of.
    FunctionID m_calleeAccountedFor;
    bool m_hasFailed { false };

    std::unique_ptr<PositionTypes> m_positionsOfBuild;
    std::unique_ptr<PositionTypes> m_positionsOfSlowBody;
    // One of the two.
    const PositionTypes* m_positions { nullptr };
    std::unique_ptr<PositionTypes> m_positionsKnowingNothing;
    std::unique_ptr<PositionTypes> m_heldPositions;
    TypeID m_resultOfCallBeingTyped;
    // While a read under a computed key is walked: LinkTables::resultOfReadByKey() of it. Each record that stands for the read is typed by it, and build() files it as FactUse { ProvenContents }.
    TypeID m_resultOfReadBeingTyped;
    // Null unless proofs are checked.
    std::unique_ptr<PositionTypes> m_positionsWithoutProofs;
    ASCIILiteral m_failure;
    Vector<UserDiagnostic> m_reports;
    // Of the operation or the terminator being walked.
    Origin m_origin { };
    Vector<OpenEdge> m_openEdges;
    uint32_t m_firstFreeOpenEdge { noOpenEdge };
    Vector<PendingBlock> m_pendingBlocks;
    // While a ClosedArm lives.
    Vector<Binding> m_bindings;
    uint32_t m_numberOfClosedArms { 0 };
    Vector<TypeID, 16> m_operandTypes;
    // A power of two of places, at most half of them taken, each found from the hash of its record's words on.
    Vector<uint32_t, 16> m_sharedConstants;
    uint32_t m_numberOfSharedConstants { 0 };
    // The last question of resultTypeOf() about a Narrowed, and its answer. No type: none.
    struct NarrowedTypedLast {
        Reference value;
        Reference test;
        BlockID block;
        TypeID typeOfValue;
        TypeID type;
        uint8_t flags { 0 };
        uint8_t numberOfPayloadWords { 0 };
        std::array<uint32_t, 4> payload { };
    };
    NarrowedTypedLast m_narrowedTypedLast;
    // What convertWithoutCode() has made of a constant for a form.
    struct CopyOfConstant {
        uint32_t constantAndForm { UINT32_MAX };
        ValueID copy;
    };
    Vector<CopyOfConstant, 16> m_copiesOfConstants;
    uint32_t m_numberOfCopiesOfConstants { 0 };
    Vector<IterationByIndex, 4> m_iterationsByIndex;
    // A body may have thousands of iterations, one loop behind the other, so nobody goes through them all for one loop.
    // By IterationByIndex::head: the places in m_iterationsByIndex, in the order in which enterLoopHead() gave them their parameters.
    HashMap<uint32_t, Vector<uint32_t, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_iterationsOfHeads;
    // IterationByIndex::inlineFrame above IterationByIndex::inputHead, of every iteration that was opened in front of its loop.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_inputHeadsOfIterations;
    static uint64_t inputHeadInFrame(uint32_t inlineFrame, BlockID inputHead) { return static_cast<uint64_t>(inlineFrame) << 32 | inputHead.index(); }
    // Those with an inputHead to which the walk has yet to come.
    uint32_t m_numberOfIterationsInFrontOfTheirLoops { 0 };
    std::optional<bool> m_listsAreIteratedByIndex;
    bool m_isLeadingHandlerEdge { false };
    Vector<OpenLoop, 4> m_openLoops;
    // By block of Code: the place in m_openLoops, and one more, of the loop whose OpenLoop::blockInFront it is. It may be shorter than Code.
    Vector<uint32_t> m_placesOfLoopsBehindBlocks;
    // While something is built in front of a loop: the place of that loop in m_openLoops.
    uint32_t m_numberOfLoopsThatMayBeLeft { UINT32_MAX };
    uint32_t m_numberOfScopesWhereProofsHoldEverywhere { 0 };
    FormAskedAhead m_formAskedAhead;
    std::unique_ptr<KeptForBuild> m_keptByTypedAccesses;
    CoroutineStateOfUnit m_coroutineState;
    LoopsOverGeneratorsOfUnit m_loopsOverGenerators;
    EnvironmentCellsOfUnit m_environmentCellsOfUnit;
    std::unique_ptr<KeptForBuild> m_keptByLadderOfNames;
    std::unique_ptr<KeptForBuild> m_keptByNumbers;
    std::unique_ptr<KeptForBuild> m_keptByStrings;
    std::unique_ptr<KeptForBuild> m_keptByPositionsInTexts;
    BodyLiveness m_liveness;
    uint32_t m_operationsCopiedAtWrittenCall { 0 };
    void noteClosuresThatAllocationAloneTakes(const BodyValue& allocation);
    Vector<Reference, 8> m_closuresThatAllocationAloneTakes;
    // The closed copies of loops in which the walk stands. Their blocks are rare and count as outside of loops, so that every emitter picks its shortest form.
    uint32_t m_numberOfCopiesBuiltForSize { 0 };
    // The operation being walked has the rule of effects Access.
    bool m_isWalkingAccess { false };
    bool m_buildsTableCalls { false };
    bool m_tableCallsHaveTailForm { false };
    Vector<TextInStringBuilder, 2> m_textsInStringBuilders;
    bool m_hasURLsThatAreNeverMade { false };
};

class Walker::ClosedArm {
    WTF_MAKE_NONCOPYABLE(ClosedArm);
public:
    // `beginsTurnAgainWhereItMay`: nobody expects the test to fail. Where Walker::mayBeginTurnAgainInClosedCopy(), the arm is the jump into the closed copy, and all that is walked in it is built nowhere.
    explicit ClosedArm(Walker&, bool beginsTurnAgainWhereItMay = false);
    ~ClosedArm();

private:
    Walker& m_walker;
    size_t m_firstBinding;
    bool m_openedGuesses;
};

class Walker::InFrontOfTypedCopy {
    WTF_MAKE_NONCOPYABLE(InFrontOfTypedCopy);
public:
    // `isInFront` false: it does nothing.
    InFrontOfTypedCopy(Walker&, bool isInFront);
    ~InFrontOfTypedCopy();

private:
    Walker& m_walker;
    bool m_isInFront;
    BlockID m_blockInside;
    uint32_t m_numberOfLoopsThatMightBeLeft { 0 };
};

class Walker::OpenArm {
    WTF_MAKE_NONCOPYABLE(OpenArm);
public:
    explicit OpenArm(Walker& walker)
        : m_walker(walker)
        , m_openedGuesses(std::exchange(walker.m_opensGuesses, true))
    {
    }
    ~OpenArm() { m_walker.m_opensGuesses = m_openedGuesses; }

private:
    Walker& m_walker;
    bool m_openedGuesses;
};

class Walker::ProofsHoldEverywhere {
    WTF_MAKE_NONCOPYABLE(ProofsHoldEverywhere);
public:
    explicit ProofsHoldEverywhere(Walker&);
    ~ProofsHoldEverywhere();

private:
    Walker& m_walker;
};

// AOTWalkLoops.cpp. Never withheld: the walk needs it at every loop. It looks at the blocks of a loop before they are walked, by the rows of the table and nothing else.
// The types must be an invariant of the loop: with them at the head, what comes round lies within them. So:
//   look 1, from what enters. What comes round lies within it: done.
//   Else each parameter that grew is widened, and look 2 starts from that. What still grows goes to the tops of its kinds, and look 3 starts from that.
//   A parameter that grows even then is `anything`, with every parameter that is made of it, which needs no look.
// The head gets what enters joined with what came round in the last look, which lies within what that look assumed.
// Each operation is typed by widestTypeOfOperation(). So what the walk binds to it lies within the preview.
struct LoopPreview {
    Effects effects;
    // In the order of the stream. The walk proves each at its operation: Walker::proveWhatLooksCountOnAt().
    Vector<GuessCountedOnByLooks, 4> guessesCountedOn;
    // Of looks that count on nothing: the first operation, in the order of the stream, that was found to run other code. Null: none does, or the looks were not made anew. In a loop inside: no turn that may begin again comes so far.
    ValueID firstOperationThatRunsOtherCode;
    bool otherCodeRunsInLoopInside { false };
    // By parameter of the head.
    Vector<TypeID, 8> typesOfParameters;
    // By parameter of the head. True: some turn steps the parameter itself by a sum or a difference of integers that may lie past the end of the exact integers, which is made as a double,
    // and no turn takes it, or what steps and joins make of it, as an integer or in a box. Empty: none.
    Vector<bool, 8> parametersSteppedOnDoubles;
    // A turn may let leave what is fresh in front of the loop, and the next turn comes to the head with that.
    // True: a look came to every operation, terminator, edge and proof of the loop and called Walker::letLeave() for each value in front of the loop that they name.
    // False, while EscapeAnalysis is on: the walk enters the loop with an empty state.
    bool hasLetMentionsLeave { false };
    // By value of the body, less `firstValue`: what the last look gave the values of the loop. Empty: the looks found no invariant.
    // At the parameters of the head of a loop inside stands an invariant of that loop, with which the look went on behind it.
    Vector<TypeID> typesOfValues;
    uint32_t firstValue { 0 };
    // No look was made: all was kept, and Walker::Frame::typesOfLooks holds these types since the looks that found them.
    bool typesOfValuesLieInFrame { false };
};
// For Walker::enterLoopHead(): LoopPreview::typesOfValues of the last preview lie in Walker::Frame::typesOfLooks.
void noteTypesOfLooksLieInFrame(Walker&);
// A relation between numbers says on each edge behind its Branch in which bounds its operands lie, whether or not the edge hands on a proof. The walk and the looks both narrow by it, and both go by this.
// The two results: of the left operand and of the right one. Nothing: the terminator asks no such relation.
std::optional<std::array<BodyEdgeResult, 2>> boundsProvenOnEdge(const BodyIndex&, const BodyTerminator&, uint32_t targetIndex);
// The relation that a terminator of the body being read asks, where on some edge it leaves less than all of an operand, given what the walk has built for the operands so far. Null: there is none.
ValueID relationThatNarrowsBehind(const Walker&, const BodyTerminator&);
// `settledAround`: by parameter of the head, what the look at a loop around this one left in LoopPreview::typesOfValues. Empty: there is none.
// That look has promised what comes round its own head on the strength of it, so this loop is promised no more: its own looks count where they stay within it, and in place of `anything` stands `settledAround`.
LoopPreview previewLoop(Walker&, BlockID inputHead, std::span<const TypeID> entryTypes, std::span<const TypeID> settledAround = { });
// The widest type of whatever the walk may bind to an operation of the body being read, given what it has built for the operands. The looks ask the same function with the types of a look.
// It is what the row gives the closed form. Of a call whose callee the walk may copy (mayCopyCallee()) it holds as well what the rows give the returns of the callee's body,
// and of a call of a built-in without a function object what the row of the direct call gives that is its closed form (findFunctionBehindCompiledBuiltin()).
// What the walk BUILDS for an operation lies within it by the tables: no form has a wider rule than the closed one. What it HANDS ON in the place of an operation is typed by the row of some other operation,
// and a row that answers with Link's proof of a place (a variable, a field of an environment, a capture, the result of a function) may say less than the rows say of the value that lies there.
// So Walker::walkOperation() asks this of whatever was handed on in a loop.
TypeID widestTypeOfOperation(Walker&, const BodyValue&);

// What buildTypedOperation() makes of an operation follows from the types of its operands and, for an access by name, from what the state has seen of the slot.
// So it can be asked ahead, about types that a test would prove. The code that builds answers, and builds nothing.
enum class TypedForm : uint8_t {
    None,
    Built,
    // Asked ahead only: it is built as a call of an accessor, which may do anything.
    BuiltAsCall,
    // An access by name that is built where the state holds what lies in the slot (WalkState::slot()), and nowhere else. The source says that every receiver has the property.
    BuiltBehindFilledSlot,
    // A definition under a private name that is built where the state knows the slot to be empty, and nowhere else: where the object has the name it throws.
    BuiltBehindEmptySlot,
    // A store by name that is built where the state knows the cell of the object to take it (WalkState::factsOfCell()), and nowhere else.
    BuiltBehindLookAtCell,
    // Both that and BuiltBehindFilledSlot.
    BuiltBehindFilledSlotAndLookAtCell,
    // A read by name off an object of one of the engine's classes that is built where the state has seen the object as its class makes it (isListAsMade()), and nowhere else. The source says that the class answers to the name.
    BuiltOnObjectAsMade,
};
// Of a value of the body being read.
using TypesOfInputs = ScopedLambda<TypeID(ValueID)>;
TypedForm typedFormOf(Walker&, const BodyValue&, const TypesOfInputs&); // AOTWalkTyped.cpp
// AOTWalkLoopInvariants.cpp. For a look at a loop, of an access by name or by key: with operands of these types the walk builds a typed form that runs no code, and this bounds what that form does.
// Nothing: the closed form is to be counted on. The types hold in every turn, so whatever the walk builds for narrower ones does no more.
std::optional<Effects> effectsOfTypedFormAhead(Walker&, const BodyValue&, const TypesOfInputs&);
// Whether the frame may be suspended in the operation, by its fields too.
bool maySuspendAsWritten(const BodyValue&);

// The operands of an operation as the builders of typed forms take them: what the walk has built for them, or no more than types.
class InputsOfTypedOperation {
public:
    InputsOfTypedOperation(const Walker& walker, const TypesOfInputs* typesAskedAbout)
        : m_walker(walker)
        , m_typesAskedAbout(typesAskedAbout)
    {
    }

    bool areOnlyTypes() const { return m_typesAskedAbout; }
    TypeID typeOf(ValueID input) const { return m_typesAskedAbout ? (*m_typesAskedAbout)(input) : m_walker.typeOf(m_walker.referenceOf(input)); }

private:
    const Walker& m_walker;
    const TypesOfInputs* m_typesAskedAbout;
};

// AOTWalkBuiltins.cpp. What a read by name finds in every run, by the table of built-ins and the locks of the build. None of the three builds anything. Null: nothing is sure.
// From `start` upward: a member that lies where the table lists it and is never replaced.
BuiltinID findConstantMemberFrom(const Program&, const Facts&, BuiltinID start, PropertyKeyID);
// The same, kept by (start, key) for the build.
BuiltinID findConstantMemberOnceForBuild(const Walker&, BuiltinID start, PropertyKeyID);
// Off whatever has the type and is neither undefined nor null: primitives of one class, one built-in object, or objects of one of the engine's classes of which Facts says that each is as its class makes it.
BuiltinID findConstantMemberReadOff(const Walker&, TypeID object, PropertyKeyID);
// The same, and of an object of one of the engine's classes that the state has seen as its class makes it (isListAsMade()).
BuiltinID findConstantMemberReadOff(Walker&, Reference object, PropertyKeyID);
// For buildTypedAccess(), of a PropGet: the read finds such a member, which is an object with a cell, and its receiver is neither undefined nor null. The member itself stands in the place of the read. `object`: null where only `typeOfObject` is asked about.
TypedForm buildReadOfBuiltinMember(Walker&, const BodyValue& read, TypeID typeOfObject, Reference object);
// Of an access of the body that the walk stands in, by its object. Link has followed all that arrives there: LinkedParameter::staysAsItsClassMakesIt.
bool readsOffEntryParameterThatStaysAsMade(Walker&, const BodyValue& access);
// Of a PropGet, an ElemGet or a Call by name of the body that the walk stands in. Link has followed all that arrives at its receiver: LinkTables::receiversThatStayAsMade.
bool receiverStaysAsMadeByLink(Walker&, const BodyValue& operation);
// And nothing lies under a number above a list: a hole and the place behind the end ask upward.
bool readsOffListThatStaysAsMadeByLink(Walker&, const BodyValue& read);
// In front of a Builtin of the input. No mechanism: without it the keys of a holder come out in another order.
void buildFirstReadOfNamedBuiltin(Walker&, const BodyValue& named);
bool firstReadOfNamedBuiltinIsSeen(const Walker&, const BodyValue& named);
// For buildTypedOperation(), of an InstanceOf whose constructor is one built-in: both lookups on the constructor are answered by such members, and what is left is the walk up the chain of the value. It builds where the inputs are what the walk has built, and only answers where they are types.
bool buildInstanceOfBuiltin(Walker&, const BodyValue& test, const InputsOfTypedOperation&);
// For Guesses, of a read for which typedFormOf() answers BuiltOnObjectAsMade with the type that `object` has by now. The test of the object: one bit of the cell. Behind it: noteListAsMade().
Reference buildTestOfObjectAsMade(Walker&, Reference object, Origin);
// What such a read finds.
TypeID typeFoundOnObjectAsMade(Walker&, const BodyValue& read, TypeID typeOfObject);
// `o.m(..)` on an object that is out of sight: the closures of the method that the records of its classes declare, which the read finds behind HasLayoutPrototype and the guard that reads ask. Null: the read is no such read.
TypeID typeOfMethodFoundBehindLooks(Walker&, const BodyValue& read, TypeID typeOfObject);
bool readOffObjectAsMadeCallsGetter(Walker&, const BodyValue& read, TypeID typeOfObject);
// For Guesses: what callBuiltin() makes of the call with operands of these types. Each is worth more than the one in front of it.
enum class BuiltinCallAskedAbout : uint8_t {
    None,
    // The native code is entered with the operands as they are written, and converts them itself.
    OnAnyOperands,
    // With the effects of the row, and its code in line.
    ByOneRow,
};
// `standsInLine`: whether code of the row would stand in the place of the call, alone or with the function behind its miss.
BuiltinCallAskedAbout wouldCallBuiltin(Walker&, const BodyValue& call, const TypesOfInputs&, bool* standsInLine = nullptr);
// What callBuiltin() would hand build() as the effects of the call, with operands of these types. It builds nothing. Nothing: the call goes by no one row.
std::optional<Effects> effectsOfCallByOneRowAhead(Walker&, const BodyValue& call, const TypesOfInputs&);

// AOTWalkPositionsInTexts.cpp. A test of the body has come out so that `position` lies below `text.length`, on an edge that alone enters a block which dominates `block`. The looks at loops and the walk ask this one function.
// It says nothing of the lower end, nor of what the two are: the asker has proven `text` a string that is no object and `position` an integer that is not negative.
bool testOfBodyHoldsPositionBelowLengthOfText(Walker&, const BodyIndex&, BlockID block, ValueID text, ValueID position);
// For AOTWalkStringCharacters.cpp, of `text.charCodeAt(position)`, codePointAt, charAt and `text[position]` in the body that the walk stands in, where what is built for the position as written is an integer.
bool testOfBodyHoldsPositionOfCharacterReadBelowLength(Walker&, const BodyValue& read);
// For the looks at loops: what the walk binds to `text.charCodeAt(position)` or codePointAt with operands of these types, where the position lies inside the text. Null: the row answers.
TypeID typeOfReadInsideTextAhead(Walker&, const BodyIndex&, const BodyValue& call, const TypesOfInputs&);
TypeID typeOfStepOverTextAhead(Walker&, const BodyIndex&, const BodyValue& step, const TypesOfInputs&); // AOTWalkIteration.cpp
bool cellAnswersWhetherValueIsArray(const Walker&, TypeID asked); // AOTWalkBuiltins.cpp
bool isLookAtCellInFrontOfIsArray(const Walker&, const BodyIndex&, const BodyValue& test);
TypeID typeOfIsArrayAhead(Walker&, const BodyIndex&, const BodyValue& call, const TypesOfInputs&);
bool iterationGoesByIndexWithoutTest(Walker&, TypeID iterated); // AOTWalkIteration.cpp

// AOTWalkRegExps.cpp. For BuiltinCalls, of a call that goes by one row.
// `new RegExp("a+", "g")`, `RegExp("a+")`: both texts are known, and the run time makes a RegExp of them by a record of the image, so the call is the literal. `flagsOrNull`: null where none are passed. Null: nothing was built.
Reference buildRegExpOfKnownTexts(Walker&, Reference pattern, Reference flagsOrNull, Origin);
// A RegExp that this code has made of texts that are written, that nothing but this code has, and that the body being read mentions nowhere but as the one operand of the call and in the read that finds the callee.
// So it is as it was made wherever the call finds it, given that the call leaves it so.
struct RegExpWrittenForCall {
    Reference object;
    StringID pattern;
    OptionSet<RegExpFlags> flags;
    Origin whereWritten;
};
std::optional<RegExpWrittenForCall> findRegExpWrittenForCall(Walker&, const BodyValue& call, ValueID operand);
// The row hands the operand to nobody and leaves it as it was made: the one object that every evaluation shares may stand in its place, and the sweep drops the one that was made. Null: nothing was built, and the call takes the object that it has.
Reference buildRegExpSharedForCall(Walker&, BuiltinOverloadID, BuiltinOperand, const RegExpWrittenForCall&);
// The row matches one string from its start or from lastIndex, and the image has code for the pattern: the match enters that code by the record. `objectOfCall`: the object that the match takes where it takes one. Null: nothing was built.
Reference buildMatchByCodeOfRegExp(Walker&, const BodyValue& call, BuiltinOverloadID, const RegExpWrittenForCall&, Reference objectOfCall, Reference string, Origin, ExpectedType);

// AOTWalkTypedAccesses.cpp. What a typed access takes for granted of its receiver beyond the layout.
enum class TypedAccess : uint8_t {
    // The state has seen the slot filled: GetSlot, `in` answered with true.
    ReadsFilledSlot = 1 << 0,
    // PutSlot.
    StoresIntoFilledSlot = 1 << 1,
    // An empty slot is taken for "no such property, in the object or above it": GetSlotOrUndefined, IsSlotFilled.
    ReadsSlotThatMayBeEmpty = 1 << 2,
    // PutSlotOrAdd for an assignment, and for a definition, which asks nothing of what lies above the object.
    StoresIntoSlotThatMayBeEmpty = 1 << 3,
    DefinesSlotThatMayBeEmpty = 1 << 4,
    // No receiver has a field of the name: undefined, `in` answered with false.
    FindsNoProperty = 1 << 5,
    // findMemberAboveObjectsOf().
    FindsMethodAbove = 1 << 6,
};
// `possibleChanges`: LinkedLayout::possibleChanges of every receiver. True: one of them would make the access answer wrongly, and nothing that the access looks at would show it.
// `knownOfCell`: WalkState::factsOfCell() of the one receiver. What has not happened to that object so far forbids nothing.
bool possibleChangesForbid(OptionSet<LayoutChange> possibleChanges, TypedAccess, OptionSet<FactOfCell> knownOfCell = { }, bool guardOfReadsHolds = false);
// What a lookup under the name comes to on every object of the layout, in every run. The layout lists no field under the name, no object of it can get a property of its own in front of the member that Link has not followed into the member,
// and none can come to stand below anything else. Nothing: the classes of the program do not answer. Whether the object itself may have gained the name, and what lies above the classes, is for whoever asks.
MemberFound findMemberAboveObjectsOf(const Program&, const LinkTables&, LayoutID, PropertyKeyID);
// The same of a lookup that starts at the prototype object of a class, or at a class object, which is the closure of the constructor. Unknown of any other object.
MemberFound findMemberOfNamedObject(const Program&, const LinkTables&, const ObjectRecord&, PropertyKeyID);
// The class whose class object the closures of the function are. Null: it is no constructor of a class.
LayoutID classOfClassObject(const Program&, FunctionID);
// PositionTypes::classAbove().
LayoutID classProvenAbove(const Program&, const LinkTables&, LayoutID classLayout);
// PositionTypes::readByNameOffObjectsOf(), of a layout that has objects. It types a read that stays closed, so it holds whatever GetSlot and GetSlotOrUndefined give in the place of such a read.
TypeID typeReadByNameOffObjectsOf(const Program&, const LinkTables&, TypeArena&, LayoutID, PropertyKeyID);
// Of an access by name or under a private name: the key of the field that it goes to.
PropertyKeyID keyOfAccessToSlot(const Program&, const BodyValue& access);
// For `testaot why-closed` alone. The first of the questions of buildReadForTypedAccess() and buildStoreForTypedAccess(), in their order, that says no for a receiver of that type.
ASCIILiteral whyAccessByNameHasNoTypedForm(Walker&, const BodyValue&, TypeID typeOfObject);
// AOTWalkGuesses.cpp. For `testaot why-closed` alone: one line for a read or a store by name that is about to be built closed.
void explainClosedAccess(Walker&, const BodyValue&, PrintStream&);
// For Guesses, of an access for which typedFormOf() answers BuiltBehindFilledSlot or BuiltBehindEmptySlot with the type that `object` has by now.
// The test of the slot. Behind it: the slot is read, and the state is told what it holds. Where it has failed, the state is told that the slot is empty.
Reference buildTestOfSlot(Walker&, const BodyValue& access, Reference object);
// For Guesses, of a PropGet for which typedFormOf() answers BuiltBehindFilledSlot with the type `guessed`, which nothing proves of `object`. One operation that asks for the shape, looks at the slot, and does the closed read where either says no.
// Null, and nothing is built: the slot holds a raw number, which only arms hand on as it lies.
Reference buildReadThatTestsShapeAndSlot(Walker&, const BodyValue& read, Reference object, TypeID guessed, bool expectsCell);
// The same of a PropGet for which typedFormOf() answers BuiltBehindLookAtCell: a property that objects leave out. A filled slot is right whatever has become of the object, so the operation asks the cell and the guards of
// its classes only where it finds the slot empty: behind them that is undefined. Null, and nothing is built: a method above, a raw number, a look that reads do not make.
Reference buildReadThatTestsShapeAndCell(Walker&, const BodyValue& read, Reference object, TypeID guessed, bool expectsCell);
// It answers with what it has read.
Reference readSlotFoundFilled(Walker&, const BodyValue& access, Reference object);
// Asked ahead, of a read: the type of what readSlotFoundFilled() reads off an object of that type. Every store into the slot is held to it (PlacedSlot::type), or Link has proven it.
TypeID typeFoundInFilledSlot(Walker&, const BodyValue& read, TypeID typeOfObject);
// What held() says of the field itself: what was seen to arrive there. It is no proof: whoever goes by it tests what the slot holds.
TypeID typeSeenToArriveInFilledSlot(Walker&, const BodyValue& read, TypeID typeOfObject);
// Of a store by name into an object of that type: what every slot that it may go to takes (PlacedSlot::type). Null: any value.
TypeID typeTakenBySlotOfStore(Walker&, const BodyValue& store, TypeID typeOfObject);
void noteSlotFoundEmpty(Walker&, const BodyValue& access, Reference object);
// For Guesses, of the stores for which typedFormOf() answers BuiltBehindLookAtCell or BuiltBehindFilledSlotAndLookAtCell with the type that `object` has by now.
// However many stores there are, each thing is asked once for all of them: a byte of the order word, its keys, a bit of the cell for the prototype, a word for the guards of the class.
// Each test is built and handed to `goOnWhereTestPasses`, which ends the block on it, and then the state is told what it has shown.
void lookAtCellInFrontOfStores(Walker&, std::span<const ValueID> stores, Reference object, NOESCAPE const ScopedLambda<void(Reference test)>& goOnWhereTestPasses);
// `const sig = new Signature(..); sig.declaration = ..; sig.parameters = ..`: the order word that the object has in front of the stores if it gets its keys as some maker of the program hands them out (KeyOrders::continuationsCreating()).
// Nothing: the state knows the word, or no order of the plan goes on with what the stores create. It is a guess by itself: whoever fails it still has all that the looks at the cell have shown.
std::optional<uint32_t> findLikelyOrderWordInFrontOfStores(Walker&, ValueID firstOfStretch, std::span<const ValueID> stores, Reference object);
void testOrderWordInFrontOfStores(Walker&, ValueID firstStore, Reference object, uint32_t orderWord, NOESCAPE const ScopedLambda<void(Reference test)>& goOnWhereTestPasses);
// For Guesses, of an access for which typedFormOf() asks for a filled slot: the body that is being read is making the object, so a look at the slot fails unless something in front has filled it.
bool looksAtSlotOfObjectBeingMade(const Walker&, const BodyValue& access);
// Walker::takeEntryParameters(), of `this` of a Body.
void noteThisAtEntry(Walker&, Reference thisValue);

// AOTWalkGuesses.cpp. What is expected of a value of the body being read, for whoever builds the arms of a guess that spans several operations itself: an iteration. No type: nothing is expected.
struct ExpectationOfValue {
    TypeID type;
    bool isDeclared { false };
};
ExpectationOfValue findExpectation(Walker&, ValueID input);
// For the looks at a typed copy whose failing tests begin the turn again. Each operand of the operation at which the walk would open a guess, with what is left of `types(operand)` behind the test. It builds nothing.
void forEachTypeThatGuessWouldProveAt(Walker&, const BodyValue&, const TypesOfInputs&, NOESCAPE const ScopedLambda<void(ValueID operand, TypeID expected, TypeID proven)>&);
// The walk stands where Walker::mayBeginTurnAgainInClosedCopy(). The test for what is expected is built, in front of the loop where the value stands there, and where it fails the turn begins again. What is left of the value is its checked name.
void proveExpectedTypeOrBeginTurnAgain(Walker&, ValueID input, TypeID expected, Origin);
// What a TypeTest of the value asks for where that is expected of it. Null: no test, or a test of a shape.
TypeID typeThatTypeTestAsksForWhereExpected(Walker&, Reference value, TypeID expected);
// Of the read of a method for its call, off an object of the type: the closures of the one function that was seen to arrive at the field, with which the walk compares what the slot holds. Null: there is none.
TypeID typeThatLookAtCalledSlotWouldProve(Walker&, const BodyValue& read, TypeID typeOfObject);
// THE LOOK THAT IS TRIED COSTS AS MUCH AS THE LOOP IS LONG, and most loops call somebody whom nobody knows. Asked of the first operation that runs other code, under the types of the looks that have found it:
// false: whatever is tested in front of it, it goes on running other code, so no look need be tried.
bool guessesMayTypeOperationInCleanTurn(Walker&, const BodyValue&, const TypesOfInputs&);
// AOTWalkGuesses.cpp. The one predicate that says where a test of ours is put in. `numberOfTypedOperations`: how many operations of the stretch are built typed with the proof and closed without it.
bool shouldOpenGuess(const Walker&, Reference value, TypeID expected, bool isDeclaredInSource, uint32_t numberOfTypedOperations);
// test > the typed run > one closed arm > rejoin. [begin, end) is the stretch of the input block that leans on the proof.
void openGuess(Walker&, Reference value, TypeID expected, BlockID inputBlock, uint32_t begin, uint32_t end);

// AOTWalkGuesses.cpp. For a loop that may be built twice: a guess at a value from in front of the loop that turns some operation of the loop from closed into typed.
struct GuessInFrontOfLoop {
    Reference value;
    TypeID tested;
    // Null: a TypeTest.
    LayoutSetID layouts;
    // The values of the body that stand for `value` and that the loop mentions.
    Vector<ValueID, 2> names;
};
Vector<GuessInFrontOfLoop, 4> findGuessesInFrontOfLoop(Walker&, BlockID inputHead, const LoopPreview&);
Reference buildTestInFrontOfLoop(Walker&, const GuessInFrontOfLoop&, Origin);
// Where the test has passed.
Reference buildProofInFrontOfLoop(Walker&, const GuessInFrontOfLoop&, Reference test, Origin);

// AOTWalkGuesses.cpp . ParameterGuards. The one predicate that says which functions have a fast and a slow version behind a guard, and what the guard tests.
// By entry parameter: what the guard lets through, null where it does not look. Empty: the function has one body.
// `this` and each positional parameter is tested for what a guess would expect of it, unless that is proven without a test, and only where the body leans on it:
// with the type proven an operation is built typed that is closed without it. typedFormOf() answers, as for a guess. Enough of them have to lean for the size of the body: mostOperationsOfBodyForOneThatLeans.
// The Walker is at work on no unit. planProgram() asks once for each function, behind all else of the plan, and whoever enters, walks or verifies a unit goes by CallingConventions::typeBehindGuard().
Vector<TypeID, 8> typesBehindParameterGuard(Walker&, FunctionID);

// AOTWalkEntries.cpp. The units that no body stands behind.
bool walkEntryUnit(Walker&, CodeUnit);

} } } // namespace JSC::AOT::Typed

#include "AOTWalkCopies.h"

#include "AOTWalkLinksOfChains.h"
#include "AOTWalkForInLinks.h"

#endif // ENABLE(AOT)
