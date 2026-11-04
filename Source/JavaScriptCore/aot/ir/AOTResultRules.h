/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDeclaredType.h"
#include "AOTLayout.h"
#include "AOTOpcode.h"
#include "AOTSchemaResultRules.h"
#include "AOTType.h"
#include "AOTTypeArena.h"
#include "TypedArrayType.h"
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class BodyIndex;
class Program;

// The type of what an operation gives, by the rule the schema names for it. One set of rules serves the solver and the walk.
// A rule is a pure function of the operation's own words, its operands' types and what the context answers. It never looks at another operation.
// No operation of Code states a type: the type of a record is resultTypeOf() of its words and its operands' proven types, and checking builds compute it again and compare.
// A type gets narrower only on the passing edge of a test (AOTTypeNarrowing.h) and by a fact, which a rule reads through PositionTypes and nowhere else.
//
// `Operation` in the templates below is anything with the schema's getters under their names and an opcode: Draft and Operation of walk/AOTCode.h.
// The templates are defined in the .cpp files, which instantiate them for those two at their ends. typeOfConstant() is instantiated for BodyValue as well.

enum class TypeLevel : uint8_t { Proven, Expected };

// What is known of the values that a position holds. Each may answer null: nothing is known.
// Then the rule falls back on what holds by construction: the type of an established position (AOTDeclaredType.h), else anything.
// At the level Proven the answer holds in the body being typed.
// At the level Expected it is the guess (PositionFacts::expected), or the written type.
// The walk's implementation is the one function through which it uses facts: it marks the record under construction with the fact.
// Facts' rules have one that reads the nodes of the graph.
class PositionTypes {
public:
    virtual ~PositionTypes() = default;

    virtual TypeID variable(TypeLevel, VariableID) const = 0;
    virtual TypeID environmentField(TypeLevel, LayoutID, uint32_t field) const = 0;
    // What lies in the field while it is in its slot: Plan's type of the slot, which every store is held to.
    virtual TypeID slot(TypeLevel, LayoutID, uint32_t field) const = 0;
    // All that arrives at the field, whoever stores it and whether or not it stays in the slot: PositionFacts::proven.
    // It holds ranges, single values and lists of functions, which no slot is laid out for. A read of the slot finds what lies in this and in slot(). No store is held to it, and no representation follows it.
    // Null: some arrival is not known, the slot holds an accessor or belongs to an environment, or the level is Expected, where slot() has the guess.
    virtual TypeID readOfSlot(TypeLevel, LayoutID, uint32_t) const { return { }; }
    // `{ get total() { .. } }`: the closures of the one function that every object of the layout has as that half of the accessor under the name of the field: LinkedAccessorsOfField::getterIsProven, setterIsProven.
    // A proof and never less: null at the level Expected, and where nobody knows.
    virtual TypeID halfOfAccessorOfField(TypeLevel, LayoutID, uint32_t, bool) const { return { }; }
    // The index of the field that Facts appended to the layout under the key. It lies behind the declared fields, and slot() takes it. None: there is none, or nothing is known.
    virtual std::optional<uint32_t> appendedField(LayoutID, PropertyKeyID) const { return std::nullopt; }
    // What a read under the name finds above every object of the layout, in every run: the closures of the one method of its classes. The layout lists no field under the name,
    // and neither an object of it nor what lies above can come to answer otherwise. `never`: the layout has no objects. A proof and never less: null at the level Expected, and where nobody knows.
    virtual TypeID foundAboveObjectsOf(TypeLevel, LayoutID, PropertyKeyID) const { return { }; }
    // What a read under the name finds on any object of the layout, which lists a field under it, whatever form the read has: all that arrives at the field, in its slot or moved out of it,
    // and undefined where some read may find an object without the property. `never`: the layout has no objects. A proof and never less: null at the level Expected, and where something above the object may answer.
    virtual TypeID readByNameOffObjectsOf(TypeLevel, LayoutID, PropertyKeyID) const { return { }; }
    // The class that is written above the class stands above it in every run: its prototype object above the prototype object, its class object above the class object.
    // Null: nobody knows, or no class of the program is written there.
    virtual LayoutID classAbove(LayoutID) const { return { }; }
    // The cell of a named object under a name, an own property or what a lookup through its prototypes finds.
    virtual TypeID property(TypeLevel, ObjectID, PropertyKeyID) const = 0;
    // What lies in the named object's own property under the name, for an operation whose words say that it finds the property there. No undefined is in it for an object that lacks the property.
    // A proof and never less: null at the level Expected, where property() has the guess.
    virtual TypeID ownProperty(TypeLevel, ObjectID, PropertyKeyID) const { return { }; }
    virtual TypeID contents(TypeLevel, ObjectID, ContentsSlot) const = 0;
    // What `await` of a named object goes on with, whoever awaits it. What its `then`, its `constructor` and the species of that do is counted.
    // A proof and never less: null at the level Expected, where contents() has the guess.
    virtual TypeID awaited(TypeLevel, ObjectID) const { return { }; }
    virtual TypeID result(TypeLevel, FunctionID, uint32_t resultIndex) const = 0;
    // What the CoroutineBody of an async function hands back where nothing in it suspends: what `return` hands over in its text. Null too where something in it may suspend.
    virtual TypeID resultOfBodyThatNeverSuspends(TypeLevel, FunctionID) const { return { }; }
    // At the entry of the Body. Of a function with a ParameterGuard: what the guard has tested.
    virtual TypeID entryParameter(TypeLevel, FunctionID, uint32_t entryParameterIndex) const = 0;
    // No caller hands the entry parameter over: Plan gave it no word. What stands in its place at a direct call is read by nobody, so requiredTypeOfOperand() holds it to nothing.
    virtual bool entryParameterIsLeftOutOfCalls(FunctionID, uint32_t) const { return false; }
    // What a closure of the function holds there, whichever closure it is and wherever it is read. At the level Proven: PositionFacts::proven of the entry parameter.
    virtual TypeID capture(TypeLevel, FunctionID, uint32_t) const { return { }; }
    // What one named closure holds there: the cell of that object. A small function that makes closures has an object for each call that enters it, so this tells apart what capture() unites.
    virtual TypeID captureOfObject(TypeLevel, ObjectID, uint32_t) const { return { }; }
    // Of the call being typed: the summaries of its callees applied to the arguments at hand, united. The rules Unknown and Declared(function) of a call ask this first.
    virtual TypeID resultOfCallBeingTyped(TypeLevel, uint32_t resultIndex) const = 0;
    // Of the read under a computed key that is being typed: what Link has proven of the operation of the input that it stands for. Null: nothing.
    virtual TypeID resultOfReadBeingTyped(TypeLevel) const { return { }; }
    // Of the record being typed, which reads one slot of a Map or a Set: what Link has proven of that slot at the operation of the input that the record stands for. Null: nothing.
    virtual TypeID contentsBeingTyped(TypeLevel) const { return { }; }
    // Of a CallTable: all that the methods return which the selector may enter. Null: nothing is proven.
    virtual TypeID resultOfSelector(TypeLevel, uint32_t) const { return { }; }

    // A rule that asks property() or contents() of each object of a set does it through here. `askEachObject` goes over the set and unites; what it gives depends on the set, the word and the level alone.
    // So whoever answers from what no longer changes may keep the answer and spare the round: a set may name every object of its kind in the program, and as many functions may ask.
    enum class QuestionOfEachObject : uint8_t { Property, Contents };
    virtual TypeID askedOfEachObject(QuestionOfEachObject, TypeLevel, ObjectSetID, uint32_t, NOESCAPE const ScopedLambda<TypeID()>& askEachObject) const { return askEachObject(); }
    // The same of a rule that asks slot(), readOfSlot() and appendedField() of the field that each layout of a set has under a name: an operation on a slot is typed several times, and a name may stand in thousands of layouts.
    // And of a rule that asks foundAboveObjectsOf() or readByNameOffObjectsOf() of each layout. Those two are asked at the level Proven alone.
    enum class QuestionOfEachLayout : uint8_t { ContentsOfSlot, FieldReadByName, FoundAboveObjects, ReadByNameOffObjects };
    virtual TypeID askedOfEachLayout(QuestionOfEachLayout, TypeLevel, LayoutSetID, PropertyKeyID, NOESCAPE const ScopedLambda<TypeID()>& askEachLayout) const { return askEachLayout(); }

    // Answers null to everything. A program compiled with it is slow and right.
    static const PositionTypes& knowingNothing();
};

struct TypingContext {
    const Program& program;
    TypeArena& types;
    const PositionTypes& positions;
    TypeLevel level { TypeLevel::Proven };

    Type at(TypeID id) const { return types.at(id); }
    // At the level Expected: what is written. At the level Proven: what is written of an established position, else anything.
    // Whether the guard of a guarded position has passed where the rule is asked is for PositionTypes to say.
    TypeID typeIDOf(const DeclaredType&) const;
    TypingContext atLevel(TypeLevel newLevel) const { return { program, types, positions, newLevel }; }
    TypingContext expecting() const { return atLevel(TypeLevel::Expected); }
};

// By the schema's operand index: named operands first, absent ones null, then the variadic ones. At the context's level.
// Where an operand names a producer of several results, as ResultAt's does, its place holds the type of the result asked for: resultTypeOf() of the producer at that index.
struct OperandTypes {
    std::span<const TypeID> types;

    uint32_t size() const { return static_cast<uint32_t>(types.size()); }
    TypeID operator[](uint32_t operandIndex) const { return operandIndex < types.size() ? types[operandIndex] : TypeID(); }
};

// An access that rests on a guess and tests it for itself: its `level` is KnowledgeLevel::Expected. The passing side does what the opcode says, the failing side does the generic operation, and both go on together.
// Nothing is proven of such an operation, so every rule treats it as the generic one:
//   findIllTypedOperand()        holds each operand to OperandKind::Any
//   requiredTypeOfOperand()      answers null
//   resultTypeOf()               at the level Proven: what the generic operation's rule gives: anything with its seed for a read, boolean for HasArrayElement, none for a store. At the level Expected: what the opcode's own rule gives
//   effectsOf()                  declaredBoundOf() the generic operation
//   wantedRepresentationOf(), resultRepresentationOf() of walk/AOTRepresentationRules.h      JSValue
// Only these opcodes have the form: codegen/AOTGuessedAccessEmitter.h.
constexpr std::optional<Opcode> genericOperationOf(Opcode access)
{
    switch (access) {
    case Opcode::GetSlot:
    case Opcode::GetSlotOrUndefined:
    case Opcode::GetDeferredClosureSlot:
    case Opcode::GetDeferredClosureSlotOrUndefined:
        return Opcode::PropGet;
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        return Opcode::PropSet;
    case Opcode::GetArrayElement:
    case Opcode::GetTypedArrayElement:
        return Opcode::ElemGet;
    case Opcode::PutArrayElement:
    case Opcode::PutTypedArrayElement:
        return Opcode::ElemSet;
    case Opcode::HasArrayElement:
        return Opcode::ElemHas;
    default:
        return std::nullopt;
    }
}

template<HoldsOpcode Operation>
bool testsItsOwnGuess(const Operation& operation)
{
    return genericOperationOf(opcodeOf(operation)) && operation.level() == KnowledgeLevel::Expected;
}

// `created`: what stands for the object that the operation makes. ObjectTable::siteObject() of the operation of the input that it came from, or Facts' copy of that
// for the instance being walked. Null: nobody numbered it, and the result is `unknown` within the layout or class that the operation names.
template<typename Operation> TypeID resultTypeOf(const TypingContext&, const Operation&, OperandTypes, ObjectID created = { }, uint32_t resultIndex = 0);
// What the target of an edge takes from the terminator itself: the results of TryCallInline and its like. What a test proves on an edge is AOTTypeNarrowing.h.
template<typename Terminator> TypeID edgeResultTypeOf(const TypingContext&, const Terminator&, OperandTypes, uint32_t targetIndex, uint32_t resultIndex);
template<typename Operation> std::optional<uint32_t> numberOfResultsOf(const Program&, const Operation&);
// What is worth keeping of a type computed at the level Expected beside the proven one: their intersection, or nothing where it says no more or contradicts.
ExpectedType expectationBeside(TypeArena&, TypeID proven, TypeID computedAtLevelExpected);

inline constexpr OpcodeSet opcodesThatMayHaveSeveralResults = []() consteval {
    OpcodeSet opcodes;
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        ResultRule rule = resultRuleOf(static_cast<Opcode>(index));
        if (rule == ResultRule::Results || rule == ResultRule::Declared)
            opcodes.add(static_cast<Opcode>(index));
    }
    return opcodes;
}();

// The rules that need no operation. Facts' summaries and rules call these.
// The type that a rule `natural(..)` of the schema writes out.
TypeID naturalTypeFrom(TypeArena&, const NaturalType&);
// What the schema's rule gives the opcode whatever its operands are, where that is primitives alone. Null otherwise.
// There is no arena to keep a record in, so a range of integers that no id holds by itself goes out to the next landmarks.
// Null too for an access whose generic operation gives anything: the opcode does not say whether the access tests its own guess.
TypeID widestPrimitiveResultOf(Opcode);
// The same of one result of an operation whose rule is `results(..)`, with as many results as the schema lists. The arena keeps the range that the rule writes.
TypeID widestPrimitiveResultOf(TypeArena&, Opcode producer, uint32_t resultIndex);
// Whose rules ask PositionTypes::contentsBeingTyped().
inline constexpr OpcodeSet recordsTypedByWhatIsProvenOfContents { Opcode::ResultAt, Opcode::CallInline, Opcode::CallBuiltin };
// Whose rules ask PositionTypes::resultOfReadBeingTyped().
inline constexpr OpcodeSet readsTypedByWhatIsProvenOfInput { Opcode::ElemGet, Opcode::GetArrayElement, Opcode::GetContiguousIndex, Opcode::GetOwnContiguousIndex };

// The seed of an operation whose result nobody describes: a call's is ResultOfUnknownCallee, a read's ContentsOfEscapedObject, a caught value's CaughtException, a native operation's UndescribedNativeResult.
UnknownSeed seedOfUnknownResultOf(Opcode);
TypeID resultTypeOfOperator(const TypingContext&, Opcode, std::span<const TypeID> operandTypes);
// x + 1, x - 1, ++x, --x of an integer: the closed form of a counter's step. Null: the rule does not apply.
TypeID typeAfterStepOfOne(TypeArena&, Opcode, std::span<const TypeID> operandTypes);
// From the least quotient of a dividend by a divisor, rounded down, to the greatest, rounded up. Neither range is empty, and no divisor is zero.
Range quotientsRoundedEitherWay(Range dividends, Range divisors);
template<typename Operation> TypeID typeOfConstant(const TypingContext&, const Operation&, ObjectID created = { });
TypeID typeOfKeyConstant(const TypingContext&, PropertyKeyID);
TypeID typeOfBuiltinObject(const TypingContext&, BuiltinID);
// `created` stands for the root, the site with the ordinal 0. An object inside it is the site whose ordinal is the number of its node in preorder, where every node counts, a leaf and a hole too.
TypeID typeOfData(const TypingContext&, const DataNode&, ObjectID created = { });
TypeID typeOfAllocatedLayout(const TypingContext&, LayoutID, ObjectID created = { });
TypeID typeOfDeclaredLayout(const TypingContext&, LayoutID);
TypeID typeOfContentsRead(const TypingContext&, TypeID container, ContentsSlot);
TypeID typeOfElementRead(const TypingContext&, TypeID container, TypeID key);
TypeID typeOfAwaited(const TypingContext&, TypeID awaited);
TypeID elementTypeOf(TypedArrayType);
// The prototype of `new.target` selects the layout, whichever class above it the allocation is written in. Empty: nothing is known of it.
LayoutSetID layoutsSelectedByNewTarget(const TypingContext&, LayoutID written, TypeID newTarget);

FunctionID onlyCalleeOf(const TypingContext&, TypeID callee);

// The six operations of the wire that speak of a type: Convert, Check, Expect, AssumeType, Upcast, NarrowByField. None of them is built into Code and none has a result rule there.
// A written type describes a position. Convert marks a value that enters one and claims nothing. These say what the walk and Facts' scan make of each.
enum class ConvertStep : uint8_t {
    // The proven type lies inside the target: the Convert is its operand.
    Nothing,
    // The value is not proven to fit. A test for `tested` stands here or at the first use, and what goes on is its edge result.
    Test,
    // The compiler laid down both the value and the position (an established position with IsClaim): what the library's own code promises of an operand of an intrinsic.
    // Code has no way to state it, so the walk tests for `tested` and traps on the failing side.
    // What goes on is the edge result of that test. Facts' scan makes it a Filter that is no guess.
    TakeAsFact,
};

struct ConvertResolution {
    ConvertStep step { ConvertStep::Nothing };
    // What enters the position: targetOfConvert().
    TypeID target;
    // Test: what the test asks for, TypeArena::testablePartOf(target). It may be wider than the target. Then the value behind the test is narrowByTest(), not the target.
    TypeID tested;
};

// `x as int32` lets every number in: the operation that follows makes the integer.
TypeID targetOfConvert(const TypingContext&, const DeclaredType&);
ConvertResolution resolveConvert(const TypingContext&, TypeID proven, const DeclaredType&);
// What the layouts that an object of the layout is written as (Program::viewsAbove()) write of a name that the layout itself does not declare. A guess: null at the level Proven, and where one of them says nothing.
TypeID tryTypeWrittenAbove(const TypingContext&, LayoutID, PropertyKeyID);
// A type that holds any object names no layout: code that has it proven reads the object by name, as code that knows nothing does.
// So where the step is Test and this says no of `tested`, a cast of the input tests nothing and the value stays expected, and no entry tests a parameter for such a type: typeTestedAtEntry() of facts/AOTSummary.h.
// Facts' scan and the walk both go by it. An operation that takes a value into a position of such a type still has to prove it: convertToDeclared() of walk/AOTWalkLanguage.h.
bool changesCodeWhereProven(const TypeArena&, TypeID tested);
// `compilerOptions.strict`: a record type of dozens of fields, every one of them optional, describes a bag of options. Such an object is filled piece by piece and seldom has the layout of the record,
// so a test for the type would fail nearly every time. Neither an entry nor a cast nor an operation that takes the value into a position tests for it. The value goes on as it came, and its guess is a weak one (GuessSource::NamesRead).
// It looks at the layouts of the written type alone. The walk, Facts' scan, typeTestedAtEntry() of facts/AOTSummary.h and computePositionTypes() all go by it.
bool describesBagOfOptions(const TypeArena&, TypeID written);
// The expectation that a value has from a Convert, a Check or an Expect on: the written type cut by what the value may be. Empty where that is nothing, or no more than `proven` says.
// Where the written objects are none of those proven to come the declaration says nothing: `list: { length: number }` with arrays handed in.
ExpectedType expectationFromWrittenType(TypeArena&, TypeID proven, ExpectedType before, TypeID written);
// In a summary a Convert is Filter(targetOfConvert()): TypeArena::intersectionOf().
// Upcast is Copy everywhere: it only widens, so the value keeps the type it has. NarrowByField is narrowHolderByFieldValue() on the passing edge of the test of the field that was read.

// What an operation takes for granted of an operand of the kind Declared: the type of the slot it stores into, of the parameter it passes to. Null: nothing.
// Of an argument of CallDirect and AwaitCallBySavingFrame with the role Body or CoroutineBody: declaredEntryTypeOf(). With the role ParameterGuard or SlowBody: the same without PositionTypes::entryParameter().
// Null for every other role: such a unit lays out its own parameters.
// AOTOperandKinds.h checks it together with the kinds.
const OpcodeSet& opcodesWithRequiredOperandTypes();
template<typename Operation> TypeID requiredTypeOfOperand(const TypingContext&, const Operation& user, uint32_t operandIndex);
TypeID declaredEntryTypeOf(const TypingContext&, FunctionID callee, unsigned entryParameterIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
