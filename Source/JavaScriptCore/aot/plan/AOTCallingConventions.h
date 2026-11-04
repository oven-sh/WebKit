/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTCodeRole.h"
#include "AOTID.h"
#include "AOTOpcode.h"
#include "AOTSchemaEnums.h"
#include "AOTStackTestPlan.h"
#include "AOTTailCallPlan.h"
#include "AOTTypeIDs.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

class Facts;
class Program;
struct Function;

enum class StackCheck : uint8_t { Required, CoveredByCallers };

// The boxed form of a position that the record of a function declares (a capture, `this`, a parameter, a result): JSValue where Program::provenTypeOf() of it is a value of JavaScript.
// Else representationOf() of that type: Cell for an environment, and by its kind for an opaque value.

struct FunctionConvention {
    bool has(CodeRole role) const { return roles >> static_cast<unsigned>(role) & 1; }

    // A bit for each of the first numberOfCodeRolesOfOneFunction roles: the unit may be mentioned.
    uint8_t roles { 0 };
    // The function reads the list of its arguments: a rest parameter, `arguments`, their count.
    bool takesArgumentsInBuffer : 1 { false };
    bool callEntryTakesNewTarget : 1 { false };
    // An async function in which nothing suspends. Milestone one: false.
    bool coroutineNeverSuspends : 1 { false };
    // Milestone one: false.
    bool hasSplitResult : 1 { false };
    uint16_t numberOfEntryParameters { 0 };
    // Function::results.size(), and where their forms begin in CallingConventionParts::formsOfResults.
    uint16_t numberOfResults { 1 };
    uint32_t firstResult { 0 };
    // Where the forms of its entry parameters begin in CallingConventionParts::formsOfEntryParameters.
    uint32_t firstEntryParameter { 0 };
    // Of a function with a ParameterGuard: where its entry parameters begin in CallingConventionParts::typesBehindGuards.
    uint32_t firstTypeBehindGuard { 0 };
};

// A function has one body, or a fast and a slow version behind one guard at its entry.
struct ParameterGuardPlan {
    FunctionID function;
    // By entry parameter: what the guard lets through. It is proven of the parameter in the whole of the fast version. Null: the guard does not look at the parameter.
    Vector<TypeID, 8> typesBehindGuard;
};

// The function returns an object that it has just made, and hands back the parts in its place: a result for each of `types`, then the value as it is where there is a `whole`,
// then a mark that says which of them speak, then the parts that other shapes alone have. Milestone one: no function has one.
struct SplitResult {
    unsigned numberOfParts() const { return static_cast<unsigned>(types.size()); }
    bool hasMark() const { return whole || mayBeNullish; }
    bool markIsTag() const { return numberOfOtherShapes; }
    unsigned resultOfWhole() const { return numberOfParts(); }
    unsigned resultOfMark() const { return numberOfParts() + !!whole; }
    unsigned firstOtherPart() const { return resultOfMark() + hasMark(); }
    unsigned numberOfResults() const { return firstOtherPart() + static_cast<unsigned>(typesOfOtherParts.size()); }
    // 0 where the value speaks, 1 and up for the shapes in their order.
    Range rangeOfTag() const { return { mayBeNullish || (whole && !isNeverType(whole)) ? 0 : 1, static_cast<int64_t>(numberOfOtherShapes) + 1 }; }
    // Of every result but the mark.
    TypeID typeOfResult(unsigned result) const
    {
        if (result < numberOfParts())
            return types[result];
        return result < firstOtherPart() ? whole : typesOfOtherParts[result - firstOtherPart()];
    }

    TypeID whole;
    bool mayBeNullish { false };
    unsigned numberOfOtherShapes { 0 };
    Vector<TypeID, 4> types;
    Vector<TypeID, 0> typesOfOtherParts;
};

struct GenericEntryPlan {
    bool takesNewTarget { false };
    bool allocatesThis { false };
    // Of the entry for calls. Bit i: nobody holds argument i any more when the function returns or throws, so a caller may pass a closure that lies in its own frame. Milestone one: 0.
    uint8_t borrowedArguments { 0 };
    // What the entry goes on to. Null, or a unit that does not exist: nothing enters the function. The entry throws.
    CodeUnit target;
    // Of the entry for calls: what a construction enters in its place, if anything.
    CodeUnit constructEntry;
};

enum class BoundCaptureKind : uint8_t { TargetCapture, BoundThis, BoundArgument, TargetClosure };

struct BoundCapture {
    BoundCaptureKind kind { BoundCaptureKind::BoundArgument };
    // A number that the run time hands on as a value when it constructs through the bound function.
    bool isHeldAsValue { false };
    Representation representation { Representation::JSValue };
    // Among the captures of the target, or among its positional parameters.
    uint16_t index { 0 };
};
// The entry parameter of the target that the capture is passed to. nullopt for TargetClosure: it is passed to none.
std::optional<unsigned> targetEntryParameterOf(const Function& target, const BoundCapture&);

struct BoundFunctionPlan {
    FunctionID target;
    uint16_t numberOfBoundArguments { 0 };
    uint16_t numberOfCapturedArguments { 0 };
    bool capturesTargetClosure { false };
    // Full or Slim: the form of every closure that NewBoundClosure makes for the plan.
    ClosureForm closureForm { ClosureForm::Full };
    uint32_t length { 0 };
    // The captures of the bound function's closure, in their order.
    Vector<BoundCapture, 8> captures;
};

// An entry in front of the primary entry of a unit, which puts undefined into every argument GPR from the firstOmittedGPR-th on and falls through.
struct SecondaryEntry {
    FunctionID function;
    CodeRole role { CodeRole::Body };
    uint8_t firstOmittedGPR { 0 };
    // Among the entries of the unit, from 1 on. The primary entry is 0.
    uint16_t id { 0 };
};

// A generic call entry that only moves is emitted in front of the primary entry of its target, as this entry of that unit, and is never compiled as a unit.
inline constexpr uint16_t fusedCallEntry = UINT16_MAX;

struct CallEntryMove {
    enum class Source : uint8_t { This, Closure, Argument, Capture, Undefined };

    Source source { Source::Undefined };
    // Argument: its index. Capture: closureCaptureSlotOf().
    uint32_t index { 0 };
    // A register.
    ArgumentLocation destination;
};

struct MethodTableRow {
    LayoutID layout;
    // What an object of the layout finds under the name as the classes are defined.
    FunctionID method;
    // While the guard of the name on the prototype side of this class holds. Null: it is proven.
    LayoutID guardedClass;
};

struct MethodFamily {
    bool entersLookup() const { return hasGuardedRow; }

    PropertyKeyID key;
    // Ascending by layout.
    Vector<MethodTableRow> rows;
    // Ascending, each once.
    Vector<FunctionID> methods;
    // By argument, maxArgumentsOfTableCall of them: what a typed selector of the family takes there, a type of numbers. Null: any value, boxed. Empty: the family has no typed selector.
    Vector<TypeID, 4> typesOfArguments;
    bool hasGuardedRow { false };
    // The layouts of the rows that have shapes, as one set: what is expected of a receiver of which nothing else is. Null: none has, or Guesses is withheld.
    LayoutSetID receivers;
};

struct MethodSelector {
    uint32_t family { 0 };
    // Behind the receiver.
    uint32_t numberOfArguments { 0 };
    // Among the units of the table: the lookup, then an entry for each method of the family.
    uint32_t firstUnit { 0 };
    // It stands right behind the boxed selector of its family and count. Argument i is proven to lie within MethodFamily::typesOfArguments[i] and crosses in the form of that type. Only a CallTable that the walk has built names it.
    bool isTyped { false };
    // In which every unit of the selector returns, and what: all that the methods of the family return. Raw only where nothing puts the lookup into the table.
    Representation result { Representation::JSValue };
    TypeID typeOfResult;
};

struct MethodTablePlan {
    // Ascending by key: one for a key.
    Vector<MethodFamily> families;
    // Ascending by firstUnit.
    Vector<MethodSelector> selectors;
    uint32_t numberOfUnits { 0 };
};

// In the order in which a Serializer takes them: the object (Cell), the builder (Cell), how many objects and lists are open around it (Int32). It returns a JSONOutcome (Int32).
enum class SerializerParameter : uint8_t { Object, Builder, LevelsAround };
inline constexpr unsigned numberOfSerializerParameters = 3;

struct CallingConventionParts {
    // By function.
    Vector<FunctionConvention> conventions;
    // The results of each function side by side, as its record declares them. Milestone one: the boxed form of each.
    Vector<Representation> formsOfResults;
    // The entry parameters of each function side by side, as its Body takes them. Empty: every parameter of every function has its boxed form. Milestone one: empty.
    Vector<Representation> formsOfEntryParameters;
    // The entry parameters of each function with a ParameterGuard side by side: ParameterGuardPlan::typesBehindGuard. Milestone one: empty.
    Vector<TypeID> typesBehindGuards;
    // Ascending by target, then by the number of bound arguments.
    Vector<BoundFunctionPlan> boundFunctions;
    MethodTablePlan methodTable;
    // By layout: CodeUnit::function of its serializer. Null: the layout has none. Empty: no layout has one, and no unit has a place. planSerializers().
    Vector<FunctionID> functionsOfSerializers;
};

// Every argument lies in a register.
static constexpr unsigned maxArgumentsOfTableCall = 4;

class CallingConventions {
public:
    CallingConventions() = default;
    JS_EXPORT_PRIVATE explicit CallingConventions(CallingConventionParts&&);

    const FunctionConvention& of(FunctionID id) const LIFETIME_BOUND { return id.index() < m_parts.conventions.size() ? m_parts.conventions[id.index()] : m_conventionOfNoFunction; }
    uint32_t numberOfFunctions() const { return static_cast<uint32_t>(m_parts.conventions.size()); }

    // Milestone one: of a role of one function, the bit in FunctionConvention::roles and numberOfBoundArguments == 0. Of a bound role, a plan in boundFunctions(). No unit of the table exists.
    // Of a function with LinkedFunction::isInQueueFromStart: the unit is in the queue of Compile from the start, so whoever mentions it needs no place in the order.
    // Whoever finds such a function may have seen a test of its guard fail, and enters the slow version at its start. Without a guard only a fast version that is left enters the slow one, and Link leads from there to the function.
    bool isSeedOfFunctionInQueueFromStart(CodeUnit unit) const { return !namesOnlyUnitsOfItsFunction(unit.role) && (unit.role != CodeRole::SlowBody || of(unit.function).has(CodeRole::ParameterGuard)); }
    bool exists(CodeUnit unit) const
    {
        if (isRoleOfOneFunction(unit.role))
            return !unit.numberOfBoundArguments && of(unit.function).has(unit.role);
        return existsBehindUnitsOfFunctions(unit);
    }
    bool isCoroutine(FunctionID id) const { return of(id).has(CodeRole::CoroutineBody); }
    // What a direct call of the function enters that has proven nothing of what it hands over. Of a coroutine it is the ramp.
    CodeUnit directEntryOf(FunctionID id) const { return { id, hasParameterGuard(id) ? CodeRole::ParameterGuard : CodeRole::Body }; }
    // What a direct call enters that has proven typeBehindGuard() of each operand. A function that every caller enters so gets no guard and no slow version: nothing mentions them.
    CodeUnit entryBehindGuardOf(FunctionID id) const { return { id, CodeRole::Body }; }
    // Milestone one: false.
    bool hasParameterGuard(FunctionID id) const { return of(id).has(CodeRole::ParameterGuard); }
    // Null: the function has no guard, or its guard does not look at the parameter.
    TypeID typeBehindGuard(FunctionID id, unsigned entryParameter) const
    {
        const FunctionConvention& convention = of(id);
        if (!convention.has(CodeRole::ParameterGuard) || entryParameter >= convention.numberOfEntryParameters)
            return { };
        return m_parts.typesBehindGuards[convention.firstTypeBehindGuard + entryParameter];
    }
    // planProgram(), once all else of the plan stands: whoever says where a guard pays asks the plan. Each function has a Body, and is listed once.
    JS_EXPORT_PRIVATE void setParameterGuards(std::span<const ParameterGuardPlan>);
    // What the word of a closure of the function names.
    CodeUnit closureEntryOf(FunctionID id) const { return { id, CodeRole::GenericCallEntry }; }
    // The unit that a CallDirect or an AwaitCallBySavingFrame enters: Draft or Operation of walk/AOTCode.h.
    template<typename Call>
    CodeUnit unitEnteredBy(const Call& call) const { return { call.function(), call.role(), static_cast<uint16_t>(call.numberOfBoundArguments()) }; }

    // Whoever enters the function through the word of a closure is an indirect call whose row in FactCallGraph lists the function, so UnitQueue may close its generic entries. Milestone one: false.
    bool indirectCallersAreAllListed(FunctionID) const { return false; }
    // closureEntryOf(id) is fused into its target: planCallEntryMoves() has an answer.
    bool hasFusedCallEntry(FunctionID id) const { return m_functionsWithFusedCallEntry.get(id.index()); }
    // planProgram(), behind setParameterGuards(): an entry goes on to the guard where there is one.
    JS_EXPORT_PRIVATE void fuseCallEntriesThatOnlyMove(const Program&, const ParallelLoops&);
    bool holdsFusedCallEntry(CodeUnit unit) const { return hasFusedCallEntry(unit.function) && unit == genericEntry(closureEntryOf(unit.function)).target; }
    // Image counts the units for which isGenericEntry() holds and that have code, in the order of indexOf(): those are the closure numbers. True where no function between the two has such a unit.
    // Milestone one: the same, by FunctionConvention::roles.
    bool haveNeighboringClosureNumbers(FunctionID lower, FunctionID higher) const
    {
        ASSERT(lower < higher && higher.index() < m_parts.conventions.size());
        return m_functionsWithGenericEntry.findBit(lower.index() + 1, true) >= higher.index();
    }

    // A dense number for each unit that a role and a function, or a planned bound function, can name, whether or not the unit exists:
    // function * numberOfCodeRolesOfOneFunction + role, and behind all of those, for the k-th of boundFunctions(), 2 * k + (role - numberOfCodeRolesOfOneFunction). placeInCodeOf() of image/AOTCompiledCode.h leans on it.
    // Behind the bound functions: MethodSelector::firstUnit for the lookup of a selector, and behind it an entry for each method of the family, in their order.
    // Behind the units of the table: a place for each layout, whether or not it has a serializer, or none at all.
    uint32_t numberOfUnits() const { return static_cast<uint32_t>(m_parts.conventions.size() * numberOfCodeRolesOfOneFunction + m_parts.boundFunctions.size() * numberOfBoundRoles) + m_parts.methodTable.numberOfUnits + numberOfPlacesOfSerializers(); }
    uint32_t firstSerializer() const { return numberOfUnits() - numberOfPlacesOfSerializers(); }
    uint32_t indexOf(CodeUnit unit) const
    {
        if (!isRoleOfOneFunction(unit.role))
            return indexBehindUnitsOfFunctions(unit);
        RELEASE_ASSERT(unit.function.index() < numberOfFunctions() && !unit.numberOfBoundArguments);
        return unit.function.index() * numberOfCodeRolesOfOneFunction + static_cast<unsigned>(unit.role);
    }
    JS_EXPORT_PRIVATE CodeUnit unitAt(uint32_t index) const;

    // Milestone one: a generic entry takes a buffer where FunctionConvention::takesArgumentsInBuffer, and registers elsewhere. Every other unit is Direct.
    EntryConvention entryConventionOf(CodeUnit unit) const
    {
        if (!isGenericEntry(unit.role))
            return EntryConvention::Direct;
        return of(unit.function).takesArgumentsInBuffer ? EntryConvention::GenericBuffer : EntryConvention::GenericRegisters;
    }
    // The entry pushes the argument registers in front of the stack arguments, so that all arguments lie side by side.
    bool spillsArgumentRegisters(CodeUnit unit) const { return entryConventionOf(unit) == EntryConvention::GenericBuffer; }

    // At entry 0 of the unit. Void: no caller hands the parameter over. Whether any caller hands an entry parameter over is asked of the Body. Milestone one: never Void.
    // Milestone one, of an entry parameter of the function: its boxed form where the record declares it, and JSValue for the callee, new.target, the count of arguments and the hidden rest.
    // A closure holds capture i in the form in which the Body takes entry parameter i.
    //   Body, ModuleStart, ParameterGuard
    //                        the entry parameters of the function
    //   SlowBody             the same, each in its boxed form
    //   CoroutineBody        those, then the driver: Cell of a generator, JSValue of every other coroutine, where undefined stands for no promise
    //   GenericBuffer        the closure (Cell) and `this` (JSValue) alone. Everything else is read out of the buffer
    //   GenericRegisters     those two, then new.target where genericEntry().takesNewTarget, then the positional parameters (of a bound entry: those behind the captured arguments), each a JSValue
    //   BoundFunction        BoundFunctionPlan::captures in their forms, then the positional parameters of the target behind the captured arguments, as its Body takes them
    //   a table role         never asked in milestone one, because no CallTable is built. RELEASE_ASSERT_NOT_REACHED
    //   Serializer           SerializerParameter: Cell, Cell, Int32
    Representation parameterRepresentation(const Program& program, CodeUnit unit, unsigned parameterIndex) const
    {
        if (takesEntryParametersAsListed(unit.role) && !m_parts.formsOfEntryParameters.isEmpty()) {
            const FunctionConvention& convention = m_parts.conventions[unit.function.index()];
            RELEASE_ASSERT(parameterIndex < convention.numberOfEntryParameters);
            Representation form = m_parts.formsOfEntryParameters[convention.firstEntryParameter + parameterIndex];
            if (form != Representation::JSValue || unit.role != CodeRole::Body || !convention.has(CodeRole::ParameterGuard))
                return form;
        }
        return parameterRepresentationByRole(program, unit, parameterIndex);
    }
    // Milestone one: a result for each that the record declares. Of every unit that does not return as declared: 1.
    unsigned numberOfResults(CodeUnit unit) const
    {
        const FunctionConvention& convention = of(unit.function);
        return returnsAsDeclared(unit.role, convention) ? convention.numberOfResults : 1;
    }
    // Milestone one: the boxed form of the declared result. Of every unit that does not return as declared: JSValue.
    Representation resultRepresentation(CodeUnit unit, unsigned resultIndex = 0) const
    {
        const FunctionConvention& convention = of(unit.function);
        if (returnsAsDeclared(unit.role, convention)) {
            RELEASE_ASSERT(resultIndex < convention.numberOfResults);
            return m_parts.formsOfResults[convention.firstResult + resultIndex];
        }
        RELEASE_ASSERT(!resultIndex);
        // A JSONOutcome.
        if (unit.role == CodeRole::Serializer)
            return Representation::Int32;
        return isTableRole(unit.role) ? methodSelector(unit.numberOfBoundArguments).result : Representation::JSValue;
    }
    // The parameters that parameterRepresentation() lists for the unit, no more and no fewer, and numberOfResults() results.
    // Milestone one: Direct is locationsOfDirectSignature() of those forms. A generic entry takes the closure in calleeGPR, `this` in thisGPR, and what follows them, counted from 0, in argumentGPR(i) and then in stack word i - numberOfArgumentGPRs.
    // It returns in returnValueGPR: AOTConvention.h. A table role: never asked, as above.
    JS_EXPORT_PRIVATE ArgumentLocations locationsOf(const Program&, CodeUnit) const;
    // The callers of all units of a function leave the same room, so that one unit can hand its arguments on to another.
    // Milestone one: 0 for a generic entry. Else the most stack words that locationsOf() gives a Body, a CoroutineBody or the unit itself. A table role: never asked, as above.
    JS_EXPORT_PRIVATE uint32_t stackWordsReservedByCallersOf(const Program&, CodeUnit callee) const;

    // The function sets the parameter to null while it still runs, so a caller that reads the argument no more keeps no copy of it across the call. Milestone one: false.
    bool letsGoOfParameter(FunctionID, unsigned) const { return false; }
    // The unit saves every register that it writes but those of StubBodyConvention::registersClobberedByCalls(). Milestone one: false.
    bool savesRegistersOfCallers(CodeUnit unit) const { return m_tailCalls.savesRegistersOfCallers(unit); }
    // Ascending by firstOmittedGPR. Milestone one: none.
    std::span<const SecondaryEntry> secondaryEntriesOf(CodeUnit) const { return { }; }
    // For a call that hands the constant undefined to every argument GPR from the firstUndefinedGPR-th on: the entry that fills the most of them. Milestone one: the primary entry, which fills none of the numberOfGPRs.
    SecondaryEntry findEntryOmittingArguments(CodeUnit unit, unsigned, unsigned numberOfGPRs) const { return { unit.function, unit.role, static_cast<uint8_t>(numberOfGPRs), 0 }; }

    // CoveredByCallers: the unit tests the stack only where its frame is larger than maxFrameSizeWithoutStackCheck of AOTConvention.h.
    // Milestone one: a generic entry, a BoundFunction and a ParameterGuard are covered, because each goes on to a unit that tests. Every other unit tests, but for a small frame of a unit that calls nothing and jumps to nothing.
    StackCheck stackCheckOf(CodeUnit unit) const { return m_stackTests.isCoveredByCallers(isBodyOfCoroutine(unit.role, isCoroutine(unit.function)) ? CodeUnit(unit.function, CodeRole::CoroutineBody) : unit) ? StackCheck::CoveredByCallers : StackCheck::Required; }
    // A unit that tests the stack does so in front of a tail call too: the test is the poll of a cycle of tail calls. Milestone one: what stackCheckOf() says.
    bool testsStackInFrontOfTailCalls(CodeUnit unit) const { return unit.role != CodeRole::TableEntry && stackCheckOf(unit) == StackCheck::Required; }

    // The parts that the unit hands back in place of an object. Null: it returns one value. Milestone one: null.
    const SplitResult* splitResultOf(CodeUnit) const { return nullptr; }
    // Of a CallDirect. Milestone one: false.
    template<typename Call>
    bool handsBackPartsAtEveryReturn(const Call& call) const
    {
        const SplitResult* split = splitResultOf(unitEnteredBy(call));
        return split && !split->hasMark();
    }

    // Of a unit for which isGenericEntry() holds.
    // Milestone one: the target is directEntryOf(), the ModuleStart where the function has one and the entry is for calls, and the BoundFunction of a GenericEntryOfBoundFunction.
    // The entry for calls takes new.target where FunctionConvention::callEntryTakesNewTarget, and names the construct entry that the function has. Both construct entries take new.target, and the allocating one allocates `this`.
    JS_EXPORT_PRIVATE GenericEntryPlan genericEntry(CodeUnit) const;

    // Milestone one: none. computeCallingConventions() says why.
    std::span<const BoundFunctionPlan> boundFunctions() const LIFETIME_BOUND { return m_parts.boundFunctions.span(); }
    // None: whoever binds so calls the built-in function. Milestone one: none.
    JS_EXPORT_PRIVATE const BoundFunctionPlan* boundFunction(FunctionID target, unsigned numberOfBoundArguments) const LIFETIME_BOUND;

    const TailCallPlan& tailCalls() const LIFETIME_BOUND { return m_tailCalls; }
    const StackTestPlan& stackTests() const LIFETIME_BOUND { return m_stackTests; }
    void setTailCalls(TailCallPlan&& tailCalls) { m_tailCalls = WTF::move(tailCalls); }
    void setStackTests(StackTestPlan&& stackTests) { m_stackTests = WTF::move(stackTests); }

    void setMethodTable(MethodTablePlan&& methodTable) { m_parts.methodTable = WTF::move(methodTable); }
    // Milestone one: the table is empty, so nobody has a selector or a family to ask the others about.
    std::span<const MethodFamily> methodFamilies() const LIFETIME_BOUND { return m_parts.methodTable.families.span(); }
    std::span<const MethodSelector> methodSelectors() const LIFETIME_BOUND { return m_parts.methodTable.selectors.span(); }
    const MethodSelector& methodSelector(uint32_t selector) const LIFETIME_BOUND { return m_parts.methodTable.selectors[selector]; }
    const MethodFamily& familyOf(const MethodSelector& selector) const LIFETIME_BOUND { return m_parts.methodTable.families[selector.family]; }
    // The place of the selector under which a call by the name with so many arguments goes through the table. The receiver has to be of layouts that have a row in its family.
    JS_EXPORT_PRIVATE std::optional<uint32_t> findMethodSelector(PropertyKeyID, unsigned numberOfArguments) const;
    // It takes and returns as every unit of the selector does, whether or not it exists.
    CodeUnit tableLookupOf(const MethodSelector& selector) const { return { familyOf(selector).methods[0], CodeRole::TableLookup, placeOf(selector) }; }
    // What JSONAppendObject enters. It takes an object of the layout, the string builder, and how many objects and lists are open around the object, and returns a JSONOutcome: SerializerParameter.
    // It is listed under the body of a module: planSerializers() of AOTJSONSerializerPlan.h. Whether objects of the layout can be written at all is the walk's question: planJSONObject().
    // Null: the layout has none, and its objects are written in place or turned away.
    CodeUnit serializerOf(LayoutID layout) const
    {
        FunctionID listedUnder = layout.index() < m_parts.functionsOfSerializers.size() ? m_parts.functionsOfSerializers[layout.index()] : FunctionID();
        return listedUnder ? CodeUnit::serializer(listedUnder, layout) : CodeUnit();
    }
    uint32_t numberOfPlacesOfSerializers() const { return static_cast<uint32_t>(m_parts.functionsOfSerializers.size()); }
    // What the rows of the method hold.
    CodeUnit unitEnteredThroughTable(const Program&, const MethodSelector& selector, FunctionID method) const { return { method, CodeRole::TableEntry, placeOf(selector) }; }

private:
    // Every other unit returns one boxed value: an entry by value, the ramp and the body of a coroutine, and a bound function, whose target has one result.
    static bool returnsAsDeclared(CodeRole role, const FunctionConvention& convention) { return (returnsResultOfFunction(role) || role == CodeRole::ModuleStart) && !convention.has(CodeRole::CoroutineBody); }
    static constexpr bool isRoleOfOneFunction(CodeRole role) { return static_cast<unsigned>(role) < numberOfCodeRolesOfOneFunction; }
    static constexpr bool takesEntryParametersAsListed(CodeRole role) { return role == CodeRole::Body || role == CodeRole::ModuleStart || role == CodeRole::ParameterGuard; }
    JS_EXPORT_PRIVATE Representation parameterRepresentationByRole(const Program&, CodeUnit, unsigned parameterIndex) const;
    JS_EXPORT_PRIVATE bool existsBehindUnitsOfFunctions(CodeUnit) const;
    JS_EXPORT_PRIVATE uint32_t indexBehindUnitsOfFunctions(CodeUnit) const;

    uint16_t placeOf(const MethodSelector& selector) const
    {
        size_t place = static_cast<size_t>(&selector - m_parts.methodTable.selectors.begin());
        RELEASE_ASSERT(place < m_parts.methodTable.selectors.size() && place <= UINT16_MAX);
        return static_cast<uint16_t>(place);
    }

    CallingConventionParts m_parts;
    FunctionConvention m_conventionOfNoFunction;
    // The constructor fills it.
    BitVector m_functionsWithGenericEntry;
    BitVector m_functionsWithFusedCallEntry;
    TailCallPlan m_tailCalls;
    StackTestPlan m_stackTests;
};

// What closureEntryOf(function) does, where it only moves. Nothing: the entry does more, so it is a unit.
JS_EXPORT_PRIVATE std::optional<Vector<CallEntryMove, 8>> planCallEntryMoves(const Program&, const CallingConventions&, FunctionID);

// Milestone one, out of the records of the functions. Of Facts it asks mayBeReached() and nothing else.
// Of every function: numberOfEntryParameters, numberOfResults and the forms of its results.
// A function gets code where it has a body and hasSourceBodyOf() and canBeCalledByValueOf() hold of its role. No other function has a unit.
// One that gets code has a GenericCallEntry: a closure of it may be made though nothing enters it. Every other role it has only where facts.mayBeReached(id):
//   a Body, a coroutine a CoroutineBody too, and a body in `bodiesWithModuleStart` a ModuleStart
//   with HasNewTarget, a Plain function that is no coroutine: a GenericConstructEntry. The constructor of a class has none, and callEntryTakesNewTarget holds of it, reached or not
//   without HasNewTarget, a Plain function with `this` that is no coroutine, has one result and is not run-time code (the role Library, an enclosingRuntimeFunction, or a module of the kind RuntimeSource),
//   where declaredLayoutOf() its constructedClass has LayoutFlags::IsConstructorFunction and the constructedClass is in `layoutsMadeAsCompiledObjects`: an AllocatingConstructEntry.
//   The run time constructs the constructor functions of its own modules by name
// `layoutsMadeAsCompiledObjects`: the objects of the layout are compiled objects, and some may be allocated.
// takesArgumentsInBuffer must be true where the function reads the list: HasRestParameter, HasHiddenRest or HasArgumentCount, reached or not.
// A body gets at `arguments` through those parameters alone: CreateUnmappedArguments and CreateMappedArguments take the count and the hidden rest as operands.
// It plans no bound function and no method table. Milestone one's walk builds no NewBoundClosure, so `f.bind(..)` stays a call of the built-in function, and only tests fill CallingConventionParts::boundFunctions.
JS_EXPORT_PRIVATE CallingConventions computeCallingConventions(const Program&, const Facts&, const BitVector& bodiesWithModuleStart, const BitVector& layoutsMadeAsCompiledObjects);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
