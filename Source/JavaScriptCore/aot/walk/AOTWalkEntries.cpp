/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The units that no body stands behind. Each takes its arguments by one convention and enters a unit of its function by another.
// At a suspension: none stands here. Of a coroutine the target is the ramp, which returns. At a throw: nothing here catches, so it goes on to the caller.
// Never withheld, and it asks about no mechanism: every operation is built in its closed form. Its tests are those that the language asks for,
// and one that traps, in front of a parameter whose form the plan chose by what Link has proven of every caller.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTCallingConventions.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTRepresentation.h"
#include "AOTTypeArena.h"
#include "AOTWalkBirths.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// What a unit hands on to the Body of its function.
struct ValuesHandedOnByEntries {
    // Empty: the unit has no function object of the function.
    Reference closure;
    // Empty, like new.target: undefined.
    Reference thisValue;
    Reference newTarget;
    // By capture of the function. One that is empty or missing is read out of `closure`.
    Vector<Reference, 8> captures;
    Vector<Reference, 8> positionalArguments;
};

// Once something has failed, or what was built never comes back, nothing more is built.
class BuilderOfEntries {
    WTF_MAKE_NONCOPYABLE(BuilderOfEntries);
public:
    BuilderOfEntries(Walker&, CodeUnit);

    bool buildGenericEntry();
    bool buildParameterGuard();
    bool buildTableEntry();
    bool buildTableLookup();
    bool buildBoundFunction();
    bool buildGenericEntryOfBoundFunction();

private:
    // The generic convention: the closure, `this`, and in registers the arguments behind them.
    static constexpr unsigned numberOfParametersInFrontOfArguments = 2;

    Reference closure() const { return m_parameters[0]; }
    Reference thisValue() const { return m_parameters[1]; }

    Draft draft(Opcode opcode) const { return Draft(opcode, m_origin); }
    void fail(ASCIILiteral);
    Reference append(Draft&&);
    Reference build(Draft&&);
    Reference undefined();
    void returnValue(Reference);
    void throwValue(Reference);
    void endAsUnreachable();
    void branch(Reference condition, BlockID taken, BlockID notTaken);
    void continueIn(BlockID);

    void takeParameters(NOESCAPE const Invocable<TypeID(unsigned)> auto& typeOfParameter);
    void takeParametersOfGenericConvention(TypeID typeOfClosure);
    void takeParametersOfTableCall();
    void returnWhatCallByNameGives();

    Reference argumentAt(uint32_t argumentIndex);
    Reference numberOfArgumentsFrom(uint32_t firstArgument);
    Reference listOfArgumentsBehindParameters();
    Reference positionalArgument(uint32_t argumentIndex, const DeclaredType&);
    Reference captureAt(const ValuesHandedOnByEntries&, unsigned capture);
    TypeID typeAtEntryOfFunction(unsigned entryParameter);
    Reference proveOrTrap(Reference value, TypeID promised);
    Reference inFormThatUnitTakes(Reference operand, CodeUnit target, unsigned parameterOfTarget, unsigned entryParameter);
    void layOutOperands(const ValuesHandedOnByEntries&, CodeUnit target, Vector<Reference, 16>& operands);

    Reference callUnit(CodeUnit target, std::span<const Reference> operands, bool isConstruction = false);
    void returnResultOf(Reference call);

    void refuseCall();
    void refuseUnlessConstructing(Reference newTarget);
    void throwInPlaceOfEntering();
    Reference allocateThis(std::span<const Reference> positionalArguments);
    void returnWhatWasConstructed(Reference returned, Reference object);

    const BoundFunctionPlan* findPlanOfBoundFunction();
    TypeID typeOfBoundCapture(const BoundCapture&);

    Walker& m_walker;
    const Program& m_program;
    const CallingConventions& m_conventions;
    TypeArena& m_types;
    const CodeUnit m_unit;
    // Of a bound role: the target.
    const Function& m_function;
    const Origin m_origin;
    // Of the entry block, as CallingConventions::locationsOf() lists them.
    Vector<Reference, 12> m_parameters;
    // new.target, where the entry takes it, is the first argument and is counted with the others.
    uint32_t m_firstSourceArgument { 0 };
    bool m_cannotGoOn { false };
};

BuilderOfEntries::BuilderOfEntries(Walker& walker, CodeUnit unit)
    : m_walker(walker)
    , m_program(walker.program())
    , m_conventions(walker.plan().conventions())
    , m_types(walker.types())
    , m_unit(unit)
    , m_function(m_program.at(unit.function))
    // The program wrote neither the call of the target nor a tail call, so a stack reads as it does where the function is called directly.
    , m_origin { m_function.position, 0, false, false }
{
}

void BuilderOfEntries::fail(ASCIILiteral what)
{
    if (!std::exchange(m_cannotGoOn, true))
        m_walker.fail(what);
}

Reference BuilderOfEntries::append(Draft&& draft)
{
    if (m_cannotGoOn)
        return { };
    Reference built = m_walker.build(WTF::move(draft));
    m_cannotGoOn |= m_walker.isUnreachable();
    return built;
}

Reference BuilderOfEntries::build(Draft&& draft)
{
    if (m_cannotGoOn)
        return { };
    m_walker.chooseConservativeFields(draft);
    return append(WTF::move(draft));
}

Reference BuilderOfEntries::undefined()
{
    return m_walker.constant(draft(Opcode::Undefined));
}

void BuilderOfEntries::returnValue(Reference value)
{
    if (m_cannotGoOn)
        return;
    m_walker.endWithReturn(singleElementSpan(value), m_origin);
    m_cannotGoOn |= m_walker.hasFailed();
}

void BuilderOfEntries::throwValue(Reference value)
{
    if (m_cannotGoOn)
        return;
    m_walker.endWithThrow(value, m_origin);
    m_cannotGoOn |= m_walker.hasFailed();
}

void BuilderOfEntries::endAsUnreachable()
{
    if (!m_cannotGoOn)
        m_walker.endAsUnreachable(m_origin);
}

void BuilderOfEntries::branch(Reference condition, BlockID taken, BlockID notTaken)
{
    if (!m_cannotGoOn)
        m_walker.branch(condition, taken, notTaken);
}

void BuilderOfEntries::continueIn(BlockID block)
{
    if (!m_cannotGoOn)
        m_walker.continueIn(block);
}

// ---- What arrives

void BuilderOfEntries::takeParameters(NOESCAPE const Invocable<TypeID(unsigned)> auto& typeOfParameter)
{
    Code& code = m_walker.code();
    ArgumentLocations locations = m_conventions.locationsOf(m_program, m_unit);
    bool hasEntry = !code.entries().empty();
    BlockID entry = hasEntry ? code.entries()[0].block : m_walker.addBlock();
    for (unsigned index = 0; index < locations.parameters.size(); ++index)
        m_parameters.append(m_walker.addParameter(entry, typeOfParameter(index), locations.parameters[index].representation));
    if (!hasEntry)
        code.addEntry(entry);
    m_walker.continueIn(entry);
}

void BuilderOfEntries::takeParametersOfGenericConvention(TypeID typeOfClosure)
{
    RELEASE_ASSERT(m_conventions.entryConventionOf(m_unit) != EntryConvention::Direct);
    TypeID typeOfValue = m_types.anything(UnknownSeed::ParameterOfEscapedFunction);
    takeParameters([&](unsigned index) {
        return index ? typeOfValue : typeOfClosure;
    });
    RELEASE_ASSERT(m_parameters.size() >= numberOfParametersInFrontOfArguments);
}

// Undefined stands for what the caller left out: the caller has put it in the register, and the code of a read out of memory asks the count.
Reference BuilderOfEntries::argumentAt(uint32_t argumentIndex)
{
    switch (m_conventions.entryConventionOf(m_unit)) {
    case EntryConvention::GenericRegisters:
        RELEASE_ASSERT(numberOfParametersInFrontOfArguments + argumentIndex < m_parameters.size());
        return m_parameters[numberOfParametersInFrontOfArguments + argumentIndex];
    case EntryConvention::GenericBuffer: {
        Draft read = draft(Opcode::GetArgument);
        read.setArgumentIndex(argumentIndex);
        return build(WTF::move(read));
    }
    case EntryConvention::Direct:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Reference BuilderOfEntries::numberOfArgumentsFrom(uint32_t firstArgument)
{
    if (m_conventions.entryConventionOf(m_unit) == EntryConvention::Direct) {
        fail("a unit that is entered directly hands on the count of its arguments"_s);
        return { };
    }
    Draft length = draft(Opcode::GetRestLength);
    length.setFirstArgument(firstArgument);
    return build(WTF::move(length));
}

// On the heap: the unit leaves by a tail call, which gives up the frame.
Reference BuilderOfEntries::listOfArgumentsBehindParameters()
{
    if (!m_conventions.spillsArgumentRegisters(m_unit)) {
        fail("a unit whose arguments do not lie side by side hands them on as a list"_s);
        return { };
    }
    Draft list = draft(Opcode::CreateRest);
    list.setFirstArgument(m_firstSourceArgument + m_function.numberOfPositionalParameters());
    list.setStorage(AllocationStorage::Heap);
    return build(WTF::move(list));
}

Reference BuilderOfEntries::positionalArgument(uint32_t argumentIndex, const DeclaredType& declared)
{
    Reference argument = argumentAt(argumentIndex);
    TypeID written = declared.written();
    if (m_cannotGoOn || !written || !(m_types.at(written).primitives & Primitive::Absent))
        return argument;

    // The convention hands over undefined for what the caller left out. Only the count tells the two apart.
    Draft zero = draft(Opcode::Integer);
    zero.setInteger(0);
    Draft comparison = draft(Opcode::StrictEq);
    comparison.setLeftOperand(numberOfArgumentsFrom(argumentIndex));
    comparison.setRightOperand(m_walker.constant(WTF::move(zero)));
    Reference isLeftOut = build(WTF::move(comparison));
    if (m_cannotGoOn)
        return { };

    Draft absent = draft(Opcode::Absent);
    absent.representation = Representation::JSValue;
    Reference marker = m_walker.constant(WTF::move(absent));

    BlockID leftOut = m_walker.addBlock();
    BlockID handedOver = m_walker.addBlock();
    BlockID next = m_walker.addBlock();
    Reference either = m_walker.addParameter(next, m_types.unionOf(m_walker.typeOf(marker), m_walker.typeOf(argument)), Representation::JSValue);
    m_walker.branch(isLeftOut, leftOut, handedOver);
    m_walker.continueIn(leftOut);
    m_walker.jumpTo(next, singleElementSpan(marker));
    m_walker.continueIn(handedOver);
    m_walker.jumpTo(next, singleElementSpan(argument));
    return either;
}

// ---- What is handed on

Reference BuilderOfEntries::captureAt(const ValuesHandedOnByEntries& values, unsigned capture)
{
    if (capture < values.captures.size() && values.captures[capture])
        return values.captures[capture];
    if (!values.closure) {
        fail("the function has captures, and the unit has neither them nor a function object"_s);
        return { };
    }
    Draft read = draft(Opcode::GetCapture);
    read.setFunction(m_unit.function);
    read.setIndex(capture);
    read.setClosureOperand(values.closure);
    return build(WTF::move(read));
}

// What holds of every caller of the function, those that come through a unit of this file among them. The plan chose the forms of the parameters by it. verifyTypesOfCode() asks the same three of a GetBoundCapture.
TypeID BuilderOfEntries::typeAtEntryOfFunction(unsigned entryParameter)
{
    if (TypeID proven = m_walker.facts().typeOfEntryParameter(m_unit.function, entryParameter).proven)
        return proven;
    if (const DeclaredType* declared = m_function.declaredEntryParameter(entryParameter))
        return m_program.provenTypeOf(*declared);
    return m_types.anything(UnknownSeed::ParameterOfEscapedFunction);
}

// It is no guess and has no closed arm: only a fault of the compiler takes the failing side.
Reference BuilderOfEntries::proveOrTrap(Reference value, TypeID promised)
{
    Draft test = draft(Opcode::TypeTest);
    test.setValueOperand(value);
    test.setTestedType(m_types.testablePartOf(promised));
    Reference passes = build(WTF::move(test));

    BlockID passing = m_walker.addBlock();
    BlockID failing = m_walker.addBlock(BlockFrequency::Rare);
    branch(passes, passing, failing);
    continueIn(failing);
    endAsUnreachable();
    continueIn(passing);

    Draft narrowed = draft(Opcode::Narrowed);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(passes);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    return append(WTF::move(narrowed));
}

// What arrives by value is boxed and of no known type, and no operation of Code states one. Where the form needs a proof, a test gives it. Walker::build() of the call then finds the conversion.
Reference BuilderOfEntries::inFormThatUnitTakes(Reference operand, CodeUnit target, unsigned parameterOfTarget, unsigned entryParameter)
{
    if (m_cannotGoOn)
        return { };
    Representation wanted = m_conventions.parameterRepresentation(m_program, target, parameterOfTarget);
    auto allowsWantedForm = [&] {
        return isLegalRepresentation(m_types.at(m_walker.typeOf(operand)), wanted);
    };
    if (allowsWantedForm())
        return operand;

    // A test asks a value of JavaScript for a type of such values.
    TypeID promised = typeAtEntryOfFunction(entryParameter);
    if (m_types.at(promised).isJavaScriptValue() && m_types.at(m_walker.typeOf(operand)).isJavaScriptValue())
        operand = proveOrTrap(operand, promised);
    if (m_cannotGoOn)
        return { };
    if (!allowsWantedForm()) {
        fail("what is proven of a parameter does not allow the form that the plan gave it"_s);
        return { };
    }
    return operand;
}

// `target` takes the entry parameters of the function: its Body, or the start of a module.
void BuilderOfEntries::layOutOperands(const ValuesHandedOnByEntries& values, CodeUnit target, Vector<Reference, 16>& operands)
{
    unsigned numberOfPositionalParameters = m_function.numberOfPositionalParameters();
    unsigned numberOfEntryParameters = m_function.numberOfEntryParameters();
    RELEASE_ASSERT(m_cannotGoOn || values.positionalArguments.size() == numberOfPositionalParameters);
    unsigned nextCapture = 0;
    unsigned nextParameter = 0;
    for (unsigned index = 0; index < numberOfEntryParameters && !m_cannotGoOn; ++index) {
        Reference operand;
        switch (m_function.entryParameterKind(index)) {
        case EntryParameter::Capture:
            operand = captureAt(values, nextCapture++);
            break;
        case EntryParameter::This:
            operand = values.thisValue ? values.thisValue : undefined();
            break;
        case EntryParameter::Callee:
            if (!values.closure)
                fail("the function takes its function object, and the unit has none"_s);
            operand = values.closure;
            break;
        case EntryParameter::NewTarget:
            operand = values.newTarget ? values.newTarget : undefined();
            break;
        case EntryParameter::ArgumentCount:
            operand = numberOfArgumentsFrom(m_firstSourceArgument);
            break;
        case EntryParameter::Parameter:
            operand = nextParameter < numberOfPositionalParameters ? values.positionalArguments[nextParameter] : listOfArgumentsBehindParameters();
            ++nextParameter;
            break;
        case EntryParameter::HiddenRest:
            operand = listOfArgumentsBehindParameters();
            break;
        }
        operands.append(inFormThatUnitTakes(operand, target, index, index));
    }
}

Reference BuilderOfEntries::callUnit(CodeUnit target, std::span<const Reference> operands, bool isConstruction)
{
    if (!m_conventions.exists(target))
        fail("a unit goes on to a unit that does not exist"_s);
    else if (m_conventions.numberOfResults(target) != 1 || m_conventions.numberOfResults(m_unit) != 1)
        fail("a unit goes on to a unit that does not return one value as it does itself"_s);
    if (m_cannotGoOn)
        return { };

    Draft call = draft(Opcode::CallDirect);
    call.setFunction(target.function);
    call.setArgumentsOperands(operands);
    m_walker.chooseConservativeFields(call);
    call.setRole(target.role);
    call.setNumberOfBoundArguments(target.numberOfBoundArguments);
    call.setIsConstruction(isConstruction);
    call.setIsNeverInlined(true);
    // No arrival is left. Whoever comes through here is a call by value, which has left `anything` where it stands.
    return append(WTF::move(call));
}

// By a tail call: words that find no room are passed from an argument list frame, and the one result lies in a register. A raw result is boxed here, so the unit comes back for it.
void BuilderOfEntries::returnResultOf(Reference call)
{
    if (m_cannotGoOn)
        return;
    bool isLastOperation = call.isValue() && m_walker.code().lastOperationOf(m_walker.currentBlock()) == call.id();
    if (!isLastOperation || !haveSameRegisterRepresentation(m_walker.representationOf(call), m_conventions.resultRepresentation(m_unit))) {
        returnValue(call);
        return;
    }
    TerminatorDraft terminator(TerminatorKind::ReturnCall, m_origin);
    terminator.setCallOperand(call);
    terminator.setRequirement(TailCallRequirement::Allowed);
    m_walker.code().end(m_walker.currentBlock(), terminator, 0);
}

// ---- GenericCallEntry, GenericConstructEntry, AllocatingConstructEntry

// The word of the closure of a class constructor says that it is not callable, and whoever calls by value goes by that word. So the call by value throws what every such call throws, and never comes back.
void BuilderOfEntries::refuseCall()
{
    Draft call = draft(Opcode::Call);
    call.setCalleeOperand(closure());
    call.setThisValueOperand(thisValue());
    call.setIsNeverInlined(true);
    build(WTF::move(call));
    endAsUnreachable();
}

void BuilderOfEntries::refuseUnlessConstructing(Reference newTarget)
{
    Draft test = draft(Opcode::TypeTest);
    test.setValueOperand(newTarget);
    test.setTestedType(TypeTable::undefined);
    Reference isCalled = build(WTF::move(test));

    BlockID called = m_walker.addBlock(BlockFrequency::Rare);
    BlockID constructing = m_walker.addBlock();
    branch(isCalled, called, constructing);
    continueIn(called);
    refuseCall();
    continueIn(constructing);
}

// The build has not followed the function. A closure of it may be made all the same, its word names this unit, and the program may well call it.
void BuilderOfEntries::throwInPlaceOfEntering()
{
    // An obfuscated executable says nothing about its sources.
    StringID name = m_walker.hidesNames() ? StringID() : m_function.name;

    Draft error = draft(Opcode::NewRuntimeError);
    error.setRuntimeError(RuntimeErrorKind::FunctionNotCompiled);
    if (name)
        error.setName(name);
    throwValue(build(WTF::move(error)));
}

// new.target is the closure: whoever enters has seen to that.
Reference BuilderOfEntries::allocateThis(std::span<const Reference> positionalArguments)
{
    LayoutID constructed = m_function.constructedClass;
    Draft read = draft(Opcode::GetConstructPrototype);
    read.setNewTargetOperand(closure());
    read.setLayout(declaredLayoutOf(m_program, constructed));

    Reference prototype = build(WTF::move(read));
    if (std::optional<uint32_t> position = positionOfTagAmongArguments(m_walker, m_unit.function); position && *position < positionalArguments.size() && !m_cannotGoOn) {
        Reference object = buildThisByTag(m_walker, m_unit.function, prototype, positionalArguments[*position], m_origin);
        m_cannotGoOn |= m_walker.isUnreachable() || m_walker.hasFailed();
        if (object || m_cannotGoOn)
            return object;
    }

    Draft allocation = draft(Opcode::NewObject);
    allocation.setLayout(constructed);
    allocation.setPrototypeOperand(prototype);
    return build(WTF::move(allocation));
}

void BuilderOfEntries::returnWhatWasConstructed(Reference returned, Reference object)
{
    // The construction is over: whoever sees the object from here on may ask whether it is finished.
    Draft finish = draft(Opcode::TryFinishBirth);
    finish.setObjectOperand(object);
    if (lookMayFinishBirthOf(m_walker, object))
        build(WTF::move(finish));

    Draft test = draft(Opcode::TypeTest);
    test.setValueOperand(returned);
    test.setTestedType(TypeTable::object);
    Reference returnedObject = build(WTF::move(test));

    BlockID takesReturned = m_walker.addBlock(BlockFrequency::Unlikely);
    BlockID takesThis = m_walker.addBlock();
    branch(returnedObject, takesReturned, takesThis);
    continueIn(takesReturned);
    returnValue(returned);
    continueIn(takesThis);
    returnValue(object);
}

bool BuilderOfEntries::buildGenericEntry()
{
    GenericEntryPlan entry = m_conventions.genericEntry(m_unit);
    m_firstSourceArgument = entry.takesNewTarget;
    unsigned numberOfPositionalParameters = m_function.numberOfPositionalParameters();

    // The run time enters the bodies of all modules with one closure, which it made for the first of them.
    takeParametersOfGenericConvention(m_function.isModuleBody() ? TypeTable::function : m_types.forFunction(m_unit.function));
    RELEASE_ASSERT(m_conventions.spillsArgumentRegisters(m_unit) || m_parameters.size() == numberOfParametersInFrontOfArguments + m_firstSourceArgument + numberOfPositionalParameters);

    bool isClassConstructor = m_function.kind == FunctionKind::BaseConstructor || m_function.kind == FunctionKind::DerivedConstructor;
    bool isCallEntryOfClassConstructor = m_unit.role == CodeRole::GenericCallEntry && isClassConstructor;
    // Constructions enter another unit, so whoever comes here calls.
    if (isCallEntryOfClassConstructor && entry.constructEntry) {
        refuseCall();
        return !m_walker.hasFailed();
    }
    if (!entry.target || !m_conventions.exists(entry.target)) {
        throwInPlaceOfEntering();
        return !m_walker.hasFailed();
    }

    ValuesHandedOnByEntries values;
    values.closure = closure();
    values.thisValue = thisValue();
    if (entry.takesNewTarget) {
        values.newTarget = argumentAt(0);
        if (isCallEntryOfClassConstructor)
            refuseUnlessConstructing(values.newTarget);
    }

    for (unsigned index = 0; index < numberOfPositionalParameters; ++index)
        values.positionalArguments.append(positionalArgument(m_firstSourceArgument + index, m_function.parameters[index]));
    Reference object;
    if (entry.allocatesThis) {
        object = allocateThis(values.positionalArguments.span());
        values.thisValue = object;
    }

    Vector<Reference, 16> operands;
    layOutOperands(values, entry.target, operands);
    Reference result = callUnit(entry.target, operands.span(), entry.allocatesThis);
    if (entry.allocatesThis)
        returnWhatWasConstructed(result, object);
    else
        returnResultOf(result);
    return !m_walker.hasFailed();
}

// ---- ParameterGuard

// The tests stand in a row, and each leads to the one block that goes on to the slow version. Nothing has happened yet, so nothing is carried over but the arguments as they came.
// Both ways leave by a tail call that moves nothing: the three units take their parameters in the same places. The Body lies behind this unit, so the jump to it goes: rankInCodeOf().
bool BuilderOfEntries::buildParameterGuard()
{
    CodeUnit fastVersion = m_conventions.entryBehindGuardOf(m_unit.function);
    CodeUnit slowVersion { m_unit.function, CodeRole::SlowBody };
    takeParameters([&](unsigned index) {
        return m_walker.typeArrivingAtEntryParameter(index);
    });
    RELEASE_ASSERT(m_parameters.size() == m_function.numberOfEntryParameters());

    // The plan chose the tests before any caller had left anything. Where what has arrived since rules a test out, or leaves something behind it that no type says, no call comes through.
    for (unsigned index = 0; index < m_parameters.size(); ++index) {
        TypeID tested = m_conventions.typeBehindGuard(m_unit.function, index);
        if (!tested)
            continue;
        TypeID behindTest = m_types.intersectionOf(m_walker.typeOf(m_parameters[index]), tested);
        if (!isNeverType(behindTest) && m_types.isSubset(behindTest, tested))
            continue;
        returnResultOf(callUnit(slowVersion, m_parameters.span()));
        return !m_walker.hasFailed();
    }

    BlockID failing;
    Vector<Reference, 16> passed(m_parameters.span());
    for (unsigned index = 0; index < m_parameters.size() && !m_cannotGoOn; ++index) {
        TypeID tested = m_conventions.typeBehindGuard(m_unit.function, index);
        if (!tested || m_types.isSubset(m_walker.typeOf(m_parameters[index]), tested))
            continue;

        Draft test = draft(Opcode::TypeTest);
        test.setValueOperand(m_parameters[index]);
        test.setTestedType(tested);
        Reference passes = build(WTF::move(test));

        BlockID passing = m_walker.addBlock();
        if (!failing)
            failing = m_walker.addBlock(BlockFrequency::Rare);
        branch(passes, passing, failing);
        continueIn(passing);

        Draft narrowed = draft(Opcode::Narrowed);
        narrowed.setValueOperand(m_parameters[index]);
        narrowed.setTestOperand(passes);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        passed[index] = append(WTF::move(narrowed));
    }
    // A passed test may leave a type that says less than the test did: an object of a built-in class that is also of a layout. The fast version takes only what is proven.
    bool allIsProven = true;
    for (unsigned index = 0; index < passed.size() && !m_cannotGoOn; ++index) {
        TypeID tested = m_conventions.typeBehindGuard(m_unit.function, index);
        allIsProven &= !tested || m_types.isSubset(m_walker.typeOf(passed[index]), tested);
    }
    if (allIsProven)
        returnResultOf(callUnit(fastVersion, passed.span()));
    else
        returnResultOf(callUnit(slowVersion, m_parameters.span()));
    if (m_walker.hasFailed() || !failing)
        return !m_walker.hasFailed();

    // Nothing is left of a parameter behind its test: no call comes through, and the way on has ended there.
    m_cannotGoOn = false;
    continueIn(failing);
    returnResultOf(callUnit(slowVersion, m_parameters.span()));
    return !m_walker.hasFailed();
}

// ---- TableEntry, TableLookup

// Whoever calls through the table has proven that the receiver is of a layout with a row. An entry is reached from the rows of its method alone.
void BuilderOfEntries::takeParametersOfTableCall()
{
    const MethodFamily& family = m_conventions.familyOf(m_conventions.methodSelector(m_unit.numberOfBoundArguments));
    Vector<LayoutID, 8> receivers;
    for (const MethodTableRow& row : family.rows) {
        if (m_unit.role == CodeRole::TableLookup || row.method == m_unit.function)
            receivers.append(row.layout);
    }
    TypeID receiver = m_types.forLayouts(receivers.span());
    TypeID anything = m_types.anything(UnknownSeed::LostByWalk);
    takeParameters([&](unsigned index) {
        TypeID proven = index && m_conventions.methodSelector(m_unit.numberOfBoundArguments).isTyped ? family.typesOfArguments[index - 1] : TypeID();
        return !index ? receiver : proven ? proven : anything;
    });
}

// The call as it was written: the name is looked up on the receiver, and what is found is called.
void BuilderOfEntries::returnWhatCallByNameGives()
{
    PropertyKeyID key = m_conventions.familyOf(m_conventions.methodSelector(m_unit.numberOfBoundArguments)).key;
    Draft read = draft(Opcode::PropGet);
    read.setObjectOperand(m_parameters[0]);
    read.setKey(key);
    m_walker.chooseConservativeFields(read);
    Reference callee = build(WTF::move(read));
    if (m_cannotGoOn)
        return;

    Draft call = draft(Opcode::Call);
    call.setCalleeOperand(callee);
    call.setThisValueOperand(m_parameters[0]);
    call.setArgumentsOperands(m_parameters.span().subspan(1));
    call.setCalleeKey(key);
    m_walker.chooseConservativeFields(call);
    returnValue(build(WTF::move(call)));
}

bool BuilderOfEntries::buildTableLookup()
{
    takeParametersOfTableCall();
    returnWhatCallByNameGives();
    return !m_walker.hasFailed();
}

// The row says what the class of the receiver has under the name. The receiver itself says whether it still stands below what its class made.
bool BuilderOfEntries::buildTableEntry()
{
    const MethodSelector& selector = m_conventions.methodSelector(m_unit.numberOfBoundArguments);
    takeParametersOfTableCall();

    Draft test = draft(Opcode::HasLayoutPrototype);
    test.setObjectOperand(m_parameters[0]);
    Reference standsWhereItWasMade = build(WTF::move(test));
    BlockID enters = m_walker.addBlock();
    BlockID looksUp = m_walker.addBlock(BlockFrequency::Rare);
    branch(standsWhereItWasMade, enters, looksUp);

    continueIn(looksUp);
    returnWhatCallByNameGives();
    if (m_walker.hasFailed())
        return false;
    m_cannotGoOn = false;

    continueIn(enters);
    ValuesHandedOnByEntries values;
    values.thisValue = m_parameters[0];
    for (unsigned argument = 0; argument < m_function.numberOfPositionalParameters(); ++argument) {
        bool isPassed = argument < selector.numberOfArguments;
        const DeclaredType& declared = m_function.parameters[argument];
        bool tellsAbsentFromUndefined = declared.written() && (m_program.at(declared.written()).primitives & Primitive::Absent);
        values.positionalArguments.append(isPassed ? m_parameters[1 + argument] : tellsAbsentFromUndefined ? m_walker.constant(draft(Opcode::Absent)) : undefined());
    }
    CodeUnit target = m_conventions.directEntryOf(m_unit.function);
    Vector<Reference, 16> operands;
    layOutOperands(values, target, operands);
    returnResultOf(callUnit(target, operands.span()));
    return !m_walker.hasFailed();
}

// ---- BoundFunction, GenericEntryOfBoundFunction

const BoundFunctionPlan* BuilderOfEntries::findPlanOfBoundFunction()
{
    const BoundFunctionPlan* bound = m_conventions.boundFunction(m_unit.function, m_unit.numberOfBoundArguments);
    if (!bound)
        fail("the plan has no such bound function"_s);
    else
        RELEASE_ASSERT(bound->target == m_unit.function && bound->numberOfCapturedArguments <= m_function.numberOfPositionalParameters());
    return bound;
}

// Whatever a bound function holds or is handed goes on to a position of the target.
TypeID BuilderOfEntries::typeOfBoundCapture(const BoundCapture& capture)
{
    if (std::optional<unsigned> entryParameter = targetEntryParameterOf(m_function, capture))
        return typeAtEntryOfFunction(*entryParameter);
    return m_types.forFunction(m_unit.function);
}

bool BuilderOfEntries::buildBoundFunction()
{
    const BoundFunctionPlan* bound = findPlanOfBoundFunction();
    if (!bound)
        return false;

    unsigned numberOfCaptures = static_cast<unsigned>(bound->captures.size());
    unsigned firstUnboundParameter = m_function.firstParameterIndex() + bound->numberOfCapturedArguments;
    takeParameters([&](unsigned index) {
        return index < numberOfCaptures ? typeOfBoundCapture(bound->captures[index]) : typeAtEntryOfFunction(firstUnboundParameter + index - numberOfCaptures);
    });
    RELEASE_ASSERT(m_parameters.size() == numberOfCaptures + m_function.numberOfPositionalParameters() - bound->numberOfCapturedArguments);

    // new.target stays undefined: the run time answers a construction through a bound function, and enters neither unit.
    ValuesHandedOnByEntries values;
    values.captures.fill(Reference(), m_function.captures.size());
    for (unsigned index = 0; index < numberOfCaptures; ++index) {
        const BoundCapture& capture = bound->captures[index];
        Reference held = m_parameters[index];
        switch (capture.kind) {
        case BoundCaptureKind::TargetCapture:
            values.captures[capture.index] = held;
            break;
        case BoundCaptureKind::BoundThis:
            values.thisValue = held;
            break;
        case BoundCaptureKind::BoundArgument:
            RELEASE_ASSERT(capture.index == values.positionalArguments.size());
            values.positionalArguments.append(held);
            break;
        case BoundCaptureKind::TargetClosure:
            values.closure = held;
            break;
        }
    }
    RELEASE_ASSERT(values.positionalArguments.size() == bound->numberOfCapturedArguments && !!values.closure == bound->capturesTargetClosure);
    values.positionalArguments.append(m_parameters.span().subspan(numberOfCaptures));

    CodeUnit target = m_conventions.directEntryOf(bound->target);
    Vector<Reference, 16> operands;
    layOutOperands(values, target, operands);
    returnResultOf(callUnit(target, operands.span()));
    return !m_walker.hasFailed();
}

// The caller's `this` is ignored. The unit is entered through the word of a closure that NewBoundClosure has made for the plan, so the captures lie where the plan says.
bool BuilderOfEntries::buildGenericEntryOfBoundFunction()
{
    const BoundFunctionPlan* bound = findPlanOfBoundFunction();
    if (!bound)
        return false;
    GenericEntryPlan entry = m_conventions.genericEntry(m_unit);
    RELEASE_ASSERT(!entry.takesNewTarget && !entry.allocatesThis && entry.target == CodeUnit(bound->target, CodeRole::BoundFunction, bound->numberOfBoundArguments));

    takeParametersOfGenericConvention(TypeTable::function);

    Vector<Reference, 16> operands;
    for (uint32_t index = 0; index < bound->captures.size(); ++index) {
        const BoundCapture& capture = bound->captures[index];
        Draft read = draft(Opcode::GetBoundCapture);
        read.setFunction(bound->target);
        read.setNumberOfBoundArguments(bound->numberOfBoundArguments);
        read.setIndex(index);
        read.setValueType(typeOfBoundCapture(capture));
        read.setClosureOperand(closure());
        read.representation = capture.representation;
        operands.append(build(WTF::move(read)));
    }
    unsigned numberOfUnboundParameters = m_function.numberOfPositionalParameters() - bound->numberOfCapturedArguments;
    unsigned firstUnboundParameter = m_function.firstParameterIndex() + bound->numberOfCapturedArguments;
    for (unsigned index = 0; index < numberOfUnboundParameters; ++index) {
        Reference argument = positionalArgument(index, m_function.parameters[bound->numberOfCapturedArguments + index]);
        operands.append(inFormThatUnitTakes(argument, entry.target, static_cast<unsigned>(operands.size()), firstUnboundParameter + index));
    }

    returnResultOf(callUnit(entry.target, operands.span()));
    return !m_walker.hasFailed();
}

} // anonymous namespace

bool walkEntryUnit(Walker& walker, CodeUnit unit)
{
    BuilderOfEntries builder(walker, unit);
    switch (unit.role) {
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
        return builder.buildGenericEntry();
    case CodeRole::ParameterGuard:
        return builder.buildParameterGuard();
    case CodeRole::ModuleStart:
        walker.fail("the start of a module has no code: a call of it is a call of the stub StartModule"_s);
        return false;
    case CodeRole::BoundFunction:
        return builder.buildBoundFunction();
    case CodeRole::GenericEntryOfBoundFunction:
        return builder.buildGenericEntryOfBoundFunction();
    case CodeRole::Body:
    case CodeRole::SlowBody:
    case CodeRole::CoroutineBody:
    case CodeRole::Serializer:
        walker.fail("a unit that a body stands behind is walked as one that has none"_s);
        return false;
    case CodeRole::TableEntry:
        return builder.buildTableEntry();
    case CodeRole::TableLookup:
        return builder.buildTableLookup();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
