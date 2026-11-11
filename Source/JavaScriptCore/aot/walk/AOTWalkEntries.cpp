/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

struct ValuesHandedOnByEntries {
    Reference closure;
    Reference thisValue;
    Reference newTarget;
    Vector<Reference, 8> positionalArguments;
};

class BuilderOfEntries {
    WTF_MAKE_NONCOPYABLE(BuilderOfEntries);
public:
    BuilderOfEntries(Walker&, CodeUnit);

    bool buildGenericEntry();
    bool buildParameterGuard();
    bool buildTableEntry();
    bool buildTableLookup();

private:
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
    Reference inFormThatUnitTakes(Reference operand, CodeUnit target, unsigned entryParameter);
    enum class ListsOfArguments : uint8_t { AreMade, AreLeftOut };
    void layOutOperands(const ValuesHandedOnByEntries&, CodeUnit target, Vector<Reference, 16>& operands, ListsOfArguments = ListsOfArguments::AreMade);
    Reference callUnit(CodeUnit target, std::span<const Reference> operands, bool isConstruction = false);
    void returnResultOf(Reference call);

    void refuseCall();
    void refuseUnlessConstructing(Reference newTarget);
    void throwInPlaceOfEntering();
    Reference allocateThis(std::span<const Reference> positionalArguments);
    void returnWhatWasConstructed(Reference returned, Reference object);

    Walker& m_walker;
    const Program& m_program;
    const CallingConventions& m_conventions;
    TypeArena& m_types;
    const CodeUnit m_unit;
    const Function& m_function;
    const Origin m_origin;
    Vector<Reference, 12> m_parameters;
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
    BlockID passedBlock = m_walker.addBlock();
    BlockID next = m_walker.addBlock();
    Reference either = m_walker.addParameter(next, m_types.unionOf(m_walker.typeOf(marker), m_walker.typeOf(argument)), Representation::JSValue);
    m_walker.branch(isLeftOut, leftOut, passedBlock);
    m_walker.continueIn(leftOut);
    m_walker.jumpTo(next, singleElementSpan(marker));
    m_walker.continueIn(passedBlock);
    m_walker.jumpTo(next, singleElementSpan(argument));
    return either;
}

Reference BuilderOfEntries::captureAt(const ValuesHandedOnByEntries& values, unsigned capture)
{
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

TypeID BuilderOfEntries::typeAtEntryOfFunction(unsigned entryParameter)
{
    return m_walker.typeArrivingAtEntryParameter(entryParameter);
}

Reference BuilderOfEntries::proveOrTrap(Reference value, TypeID promised)
{
    if (m_cannotGoOn)
        return { };
    Reference proven = m_walker.proveOrTrap(value, m_walker.typeThatUnitMayTestFor(m_types.testablePartOf(promised)), ProofSource::EntryParameter, m_origin);
    m_cannotGoOn |= !proven || m_walker.hasFailed();
    return proven;
}

Reference BuilderOfEntries::inFormThatUnitTakes(Reference operand, CodeUnit target, unsigned entryParameter)
{
    if (m_cannotGoOn)
        return { };
    Representation wanted = m_conventions.parameterRepresentation(m_program, target, entryParameter);
    auto allowsWantedForm = [&] {
        return isLegalRepresentation(m_types.at(m_walker.typeOf(operand)), wanted);
    };
    TypeID promised = typeAtEntryOfFunction(entryParameter);
    bool isAsked = m_types.at(promised).isJavaScriptValue() && m_types.at(m_walker.typeOf(operand)).isJavaScriptValue() && !m_types.isSubset(m_walker.typeOf(operand), m_walker.typeThatUnitMayTestFor(m_types.testablePartOf(promised)));
    if (!isAsked && allowsWantedForm())
        return operand;
    if (isAsked)
        operand = proveOrTrap(operand, promised);
    if (m_cannotGoOn)
        return { };
    if (!allowsWantedForm()) {
        fail("what is proven of a parameter does not allow the form that the plan gave it"_s);
        return { };
    }
    return operand;
}

void BuilderOfEntries::layOutOperands(const ValuesHandedOnByEntries& values, CodeUnit target, Vector<Reference, 16>& operands, ListsOfArguments lists)
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
            if (m_walker.plan().promotedEnvironments().isRemoved(m_unit.function, nextCapture)) {
                operands.append(wordInPlaceOfRemovedCapture(m_walker, m_unit.function, nextCapture++));
                continue;
            }
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
            if (nextParameter >= numberOfPositionalParameters && lists == ListsOfArguments::AreLeftOut) {
                ++nextParameter;
                operands.append(Reference());
                continue;
            }
            operand = nextParameter < numberOfPositionalParameters ? values.positionalArguments[nextParameter] : listOfArgumentsBehindParameters();
            ++nextParameter;
            break;
        case EntryParameter::HiddenRest:
            if (lists == ListsOfArguments::AreLeftOut) {
                operands.append(Reference());
                continue;
            }
            operand = listOfArgumentsBehindParameters();
            break;
        }
        operands.append(inFormThatUnitTakes(operand, target, index));
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

    RELEASE_ASSERT(!isTableRole(target.role));
    Draft call = draft(Opcode::CallDirect);
    call.setFunction(target.function);
    call.setArgumentsOperands(operands);
    m_walker.chooseConservativeFields(call);
    call.setRole(target.role);
    call.setNumberOfBoundArguments(target.numberOfBoundArguments);
    call.setIsConstruction(isConstruction);
    call.setIsNeverInlined(true);
    return append(WTF::move(call));
}

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

void BuilderOfEntries::refuseCall()
{
    Draft call = draft(Opcode::Call);
    call.setCalleeOperand(closure());
    call.setThisValueOperand(thisValue());
    call.setIsNeverInlined(true);
    Reference refusal = build(WTF::move(call));
    if (m_walker.checksProofsOf(ProofSource::EntryParameter)) {
        endAsUnreachable();
        return;
    }
    returnResultOf(refusal);
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

void BuilderOfEntries::throwInPlaceOfEntering()
{
    StringID name = m_walker.hidesNames() ? StringID() : m_function.name;

    Draft error = draft(Opcode::NewRuntimeError);
    error.setRuntimeError(RuntimeErrorKind::FunctionNotCompiled);
    if (name)
        error.setName(name);
    throwValue(build(WTF::move(error)));
}

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
    Draft finish = draft(Opcode::TryFinishBirth);
    finish.setObjectOperand(object);
    if (lookMayFinishBirthOf(m_walker, object))
        build(WTF::move(finish));
    showWhatIsHeldFilledWhereConstructionEnds(m_walker, m_unit.function, object, m_origin);
    if (m_walker.isUnreachable())
        return;

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

    takeParametersOfGenericConvention(m_function.isModuleBody() ? TypeTable::function : m_types.forFunction(m_unit.function));
    RELEASE_ASSERT(m_conventions.spillsArgumentRegisters(m_unit) || m_parameters.size() == numberOfParametersInFrontOfArguments + m_firstSourceArgument + numberOfPositionalParameters);

    bool isClassConstructor = m_function.kind == FunctionKind::BaseConstructor || m_function.kind == FunctionKind::DerivedConstructor;
    bool isCallEntryOfClassConstructor = m_unit.role == CodeRole::GenericCallEntry && isClassConstructor;
    if (isCallEntryOfClassConstructor && entry.constructEntry) {
        refuseCall();
        return !m_walker.hasFailed();
    }
    if (!entry.target || !m_conventions.exists(entry.target)) {
        throwInPlaceOfEntering();
        return !m_walker.hasFailed();
    }
    if (m_walker.arrivals().entriesAreClosed(m_unit.function)) {
        m_walker.count(StatisticID::GenericEntriesProvenUnreachable);
        m_walker.buildCheckOfProof(thisValue(), TypeTable::never, TypeTable::anything, ProofSource::EntryParameter, m_origin);
        endAsUnreachable();
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
    BlockID whereListsAreNamed = m_unit.role == CodeRole::GenericCallEntry && m_walker.plan().genericCallEntryWalksBody(m_unit.function)
        ? blockWhereListsOfArgumentsAreFirstNamed(m_program, m_walker.facts().bodies(), m_unit.function) : BlockID();
    if (whereListsAreNamed) {
        layOutOperands(values, entry.target, operands, ListsOfArguments::AreLeftOut);
        if (m_cannotGoOn)
            return !m_walker.hasFailed();
        return m_walker.visitBodyFromEntry(operands.span(), whereListsAreNamed, scopedLambda<Reference(unsigned)>([&](unsigned entryParameter) {
            m_cannotGoOn = false;
            return inFormThatUnitTakes(listOfArgumentsBehindParameters(), entry.target, entryParameter);
        }));
    }
    layOutOperands(values, entry.target, operands);
    Reference result = callUnit(entry.target, operands.span(), entry.allocatesThis);
    if (entry.allocatesThis)
        returnWhatWasConstructed(result, object);
    else
        returnResultOf(result);
    return !m_walker.hasFailed();
}

bool BuilderOfEntries::buildParameterGuard()
{
    CodeUnit fastVersion = m_conventions.entryBehindGuardOf(m_unit.function);
    CodeUnit slowVersion { m_unit.function, CodeRole::SlowBody };
    ArgumentLocations locations = m_conventions.locationsOf(m_program, m_unit);
    takeParameters([&](unsigned index) {
        if (!m_walker.checksWalk() || !m_walker.checksProofsOf(ProofSource::EntryParameter))
            return m_walker.typeArrivingAtEntryParameter(index);
        TypeID withoutCallers = m_walker.facts().typeBelievedOfEntryParameter(m_types, m_unit.function, index, TypeID());
        Representation form = locations.parameters[index].representation;
        if (form == Representation::JSValue || form == Representation::Cell || !m_types.at(withoutCallers).isJavaScriptValue())
            return withoutCallers;
        return m_walker.typeArrivingAtEntryParameter(index);
    });
    RELEASE_ASSERT(m_parameters.size() == m_function.numberOfEntryParameters());

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
    if (!m_cannotGoOn)
        m_walker.letGuessFailUnasked(failing, m_origin);
    const BeforeEvaluation& beforeEvaluation = m_walker.facts().beforeEvaluation();
    for (VariableID mark : beforeEvaluation.holdsIn(m_unit.function) ? beforeEvaluation.marks.span() : std::span<const VariableID>()) {
        Draft read = draft(Opcode::GetModuleVariable);
        read.setVariable(mark);
        Reference marked = m_cannotGoOn ? Reference() : build(WTF::move(read));
        if (!marked || m_types.isSubset(m_walker.typeOf(marked), TypeTable::undefined))
            continue;
        Draft test = draft(Opcode::TypeTest);
        test.setValueOperand(marked);
        test.setTestedType(TypeTable::undefined);
        Reference passes = build(WTF::move(test));
        BlockID passing = m_walker.addBlock();
        if (!failing)
            failing = m_walker.addBlock(BlockFrequency::Rare);
        branch(passes, passing, failing);
        continueIn(passing);
    }
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

    m_cannotGoOn = false;
    continueIn(failing);
    returnResultOf(callUnit(slowVersion, m_parameters.span()));
    return !m_walker.hasFailed();
}

void BuilderOfEntries::takeParametersOfTableCall()
{
    const MethodFamily& family = m_conventions.familyOf(m_conventions.methodSelector(m_unit.placeOfSelector()));
    Vector<LayoutID, 8> receivers;
    for (const MethodTableRow& row : family.rows) {
        if (m_unit.role == CodeRole::TableLookup || row.method == m_unit.function)
            receivers.append(row.layout);
    }
    TypeID receiver = m_types.forLayouts(receivers.span());
    TypeID anything = m_types.anything(UnknownSeed::LostByWalk);
    takeParameters([&](unsigned index) {
        TypeID proven = index && m_conventions.methodSelector(m_unit.placeOfSelector()).isTyped ? family.typesOfArguments[index - 1] : TypeID();
        return !index ? receiver : proven ? proven : anything;
    });
}

void BuilderOfEntries::returnWhatCallByNameGives()
{
    PropertyKeyID key = m_conventions.familyOf(m_conventions.methodSelector(m_unit.placeOfSelector())).key;
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

bool BuilderOfEntries::buildTableEntry()
{
    const MethodSelector& selector = m_conventions.methodSelector(m_unit.placeOfSelector());
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
    case CodeRole::Body:
    case CodeRole::SlowBody:
    case CodeRole::CoroutineBody:
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
