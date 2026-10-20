/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAssertedObjects.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCalleesBeforeFlow.h"
#include "AOTFunctionAnalyses.h"
#include "AOTSchemaResultRules.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename ProgramType>
static auto findPositionIn(ProgramType& program, PositionReference position) -> decltype(&program.variables[0].declared)
{
    switch (position.kind) {
    case PositionReference::Kind::EntryParameter:
        if (position.index >= program.at(position.function()).numberOfEntryParameters())
            return nullptr;
        return program.at(position.function()).declaredEntryParameter(position.index);
    case PositionReference::Kind::Result: {
        auto& results = program.at(position.function()).results;
        return position.index < results.size() ? &results[position.index] : nullptr;
    }
    case PositionReference::Kind::Variable:
        return &program.at(position.variable()).declared;
    case PositionReference::Kind::Field: {
        auto& fields = program.at(position.layout()).fields;
        return position.index < fields.size() ? &fields[position.index].declared : nullptr;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const DeclaredType* findPosition(const Program& program, PositionReference position)
{
    return findPositionIn(program, position);
}

DeclaredType* findPosition(Program& program, PositionReference position)
{
    return findPositionIn(program, position);
}

ValueKinds AdmittedKinds::at(PositionReference position) const
{
    auto entry = std::ranges::lower_bound(m_entriesByPosition, position, { }, &Entry::position);
    return entry != m_entriesByPosition.end() && entry->position == position ? entry->kinds : ValueKinds();
}

bool AdmittedKinds::add(PositionReference position, ValueKinds kinds)
{
    size_t index = std::ranges::lower_bound(m_entriesByPosition, position, { }, &Entry::position) - m_entriesByPosition.begin();
    if (index == m_entriesByPosition.size() || m_entriesByPosition[index].position != position)
        m_entriesByPosition.insert(index, Entry { position, { } });
    Entry& entry = m_entriesByPosition[index];
    if (entry.kinds.containsAll(kinds))
        return false;
    entry.kinds.add(kinds);
    return true;
}

std::optional<Mention> mentionOf(const Value& value)
{
    switch (value.opcode) {
    case Opcode::GetModuleVariable:
        return Mention { Mention::Kind::Variable, value.variable().index() };
    case Opcode::NewClosure:
    case Opcode::CallDirect:
        return Mention { Mention::Kind::Function, value.function().index() };
    case Opcode::GetEnvironmentField:
    case Opcode::PrivateGet:
        return Mention { Mention::Kind::Layout, value.layout().index() };
    case Opcode::PropGet:
    case Opcode::SuperGet:
        if (!value.key())
            return std::nullopt;
        return Mention { Mention::Kind::Key, value.key().index() };
    default:
        return std::nullopt;
    }
}

bool mayYieldUnnamedObject(const Program& program, const DeclaredType& request)
{
    if (!request.site || program.at(request.site).kind != TypeSiteKind::Assertion || !request.written)
        return false;
    return !program.at(request.type).withOnlyObjects().isSubsetOf(program.at(request.written).withoutProofs(), program);
}

namespace {

struct AssertedParts {
    static AssertedParts itself(AssertedOrigins origins) { return { origins, { }, { } }; }
    static AssertedParts contents(AssertedOrigins origins) { return { { }, origins, { } }; }
    static AssertedParts ofOtherKinds(ValueKinds kinds) { return { { }, { }, kinds }; }

    bool isEmpty() const { return itselfFrom.isEmpty() && contentsFrom.isEmpty() && otherKinds.isEmpty(); }
    bool containsAll(AssertedParts other) const { return itselfFrom.containsAll(other.itselfFrom) && contentsFrom.containsAll(other.contentsFrom) && otherKinds.containsAll(other.otherKinds); }
    uint32_t numberOfSources() const { return !(itselfFrom.isEmpty() && contentsFrom.isEmpty()) + otherKinds.size(); }

    void add(AssertedParts other)
    {
        itselfFrom.add(other.itselfFrom);
        contentsFrom.add(other.contentsFrom);
        otherKinds.add(other.otherKinds);
    }

    // Not empty: the value may be an asserted object.
    AssertedOrigins itselfFrom;
    // Not empty: the value is a rest array, a promise or a generator that may hand out an asserted object.
    AssertedOrigins contentsFrom;
    ValueKinds otherKinds;
};

class AssertedObjectAnalysis {
public:
    AssertedObjectAnalysis(const AssertedObjectInputs& inputs, FunctionID id, Function& function, FunctionAnalyses& analyses, TypeArena& types, BuildContext& build)
        : m_inputs(inputs)
        , m_program(inputs.program)
        , m_id(id)
        , m_function(function)
        , m_analyses(analyses)
        , m_types(types)
        , m_policy(build.options().recorded.policy())
        , m_typing { inputs.program, function, types, m_policy == MismatchPolicy::Report ? TypeLevel::Written : TypeLevel::Proven }
        , m_callees(CalleesBeforeFlowInputs { inputs.closures, inputs.noFlowResults, id, analyses, m_typing })
        , m_propagationInputs { inputs.program, id, function, analyses, types, build }
    {
    }

    AssertedObjectFindings run(uint32_t previousNumberOfSources);

    AssertedParts partsOf(ValueID id) const { return id && !m_parts.isEmpty() ? m_parts[id.index()] : AssertedParts(); }
    bool holds(ValueID id) const { return !partsOf(id).itselfFrom.isEmpty(); }
    ValueKinds otherKindsOf(ValueID id) const { return partsOf(id).otherKinds; }
    ValueKinds kindsAdmittedAt(PositionReference position) const { return m_inputs.admittedKinds.at(position); }
    ValueKinds otherKindsOfFieldsRead(const Value& read, PropertyKeyID) const;
    bool isAssertion(const DeclaredType&) const;
    bool isAssertion(const Value&) const;
    bool givesOnlyObjectsNamedBy(const DeclaredType& assertion, ValueID operand) const;
    uint32_t numberOfAdmittingFieldsRead(const Value& read, PropertyKeyID) const;
    bool readsAdmittingField(const Value& read, PropertyKeyID key) const { return numberOfAdmittingFieldsRead(read, key); }
    AssertedOrigins originsOfContentsRead(const Value&) const;
    bool mayCallFunctionWithAdmittingResult(const Value& call) const;
    AssertedParts partsOfCallResult(const Value& call) const;
    AssertedParts partsOfResult(FunctionID) const;
    AssertedParts partsOfResult(FunctionID, const Value& call) const;
    uint32_t numberOfOriginsOfCallResult(const Value& call) const;

    const Program& program() const { return m_program; }
    const Function& function() const { return m_function; }

private:
    void retypeDependentsOf(std::span<const ValueID>);
    uint32_t countSources() const;
    void collectValues();
    Vector<ValueID> valuesReadFromPositions() const;
    AssertedParts computeParameter(BlockID, uint32_t parameterIndex) const;
    AssertedParts computeValue(const Value&) const;

    bool hasObjectsOutside(const Type& value, const DeclaredType&) const;
    bool admit(DeclaredType&, const Type& entering);
    bool letsAssertedObjectsThrough(const Value& request) const;
    Vector<ValueID> admitInRequests();
    void admitInTailCasts(BlockID);
    ValueKinds otherKindsOutside(const Type& entering, const DeclaredType&, ValueKinds followed) const;
    bool admitOtherKinds(DeclaredType&, const Type& entering, ValueKinds followed);
    Vector<ValueID> admitInRequestsThatAlwaysFail();

    void collectReachedPositions();
    void visitUse(const Value& user, ValueID operand, uint32_t operandIndex);
    void visitArgument(ValueID callee, uint32_t position, AssertedObjectUse);
    void visitTerminator(const Terminator&);
    std::optional<PositionReference> positionOfDeclaredOperand(const Value& user, uint32_t operandIndex) const;
    void reach(PositionReference, AssertedObjectUse, const Type& entering);
    void reach(PositionReference position, AssertedObjectUse use) { reach(position, use, m_typing.typeOf(use.value)); }
    void reachFields(ValueID receiver, PropertyKeyID, AssertedObjectUse);
    void reachParameter(FunctionID, uint32_t position, AssertedObjectUse);
    void reachResult(AssertedObjectUse, const Type& entering, AssertedOrigins);
    void reachContentsOfResult(AssertedObjectUse, AssertedOrigins);
    void reachResultWithOtherKinds(AssertedObjectUse, const Type& entering);

    const AssertedObjectInputs& m_inputs;
    const Program& m_program;
    FunctionID m_id;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    TypeArena& m_types;
    MismatchPolicy m_policy;
    TypingContext m_typing;
    CalleesBeforeFlow m_callees;
    TypePropagationInputs m_propagationInputs;
    Vector<AssertedParts> m_parts;
    Vector<std::pair<ValueID, ValueKinds>> m_requestsThatAlwaysFailed;
    AssertedObjectFindings m_findings;
};

struct AssertedObjectRules {
    using Result = AssertedParts;

    const AssertedObjectAnalysis& analysis;

    static AssertedParts itselfIf(bool condition) { return condition ? AssertedParts::itself(AssertedOrigins::function()) : AssertedParts(); }
    AssertedParts readFrom(PositionReference position)
    {
        const DeclaredType* declared = findPosition(analysis.program(), position);
        AssertedParts parts = itselfIf(declared && admitsObjects(*declared));
        parts.otherKinds = analysis.kindsAdmittedAt(position);
        return parts;
    }

    AssertedParts sameAs(const Value& value, uint32_t operandIndex)
    {
        AssertedParts parts = analysis.partsOf(value.operand(operandIndex));
        parts.add(itselfIf(analysis.isAssertion(value)));
        parts.itselfFrom.add(analysis.originsOfContentsRead(value));
        return parts;
    }

    AssertedParts cast(const Value& value, uint32_t operandIndex, TypeID = { }) { return analysis.partsOf(value.operand(operandIndex)); }
    AssertedParts resultOperand(const Value& value, uint32_t resultIndex) { return analysis.partsOf(value.variadicOperands()[resultIndex]); }

    AssertedParts declared(const Value&, VariableID variable) { return readFrom(PositionReference::variable(variable)); }
    AssertedParts declared(const Value&, LayoutID layout, uint32_t field) { return readFrom(PositionReference::field({ layout, field })); }

    AssertedParts declared(const Value& value, PropertyKeyID key)
    {
        AssertedParts parts = itselfIf(analysis.readsAdmittingField(value, key));
        parts.otherKinds = analysis.otherKindsOfFieldsRead(value, key);
        return parts;
    }

    AssertedParts declared(const Value& value, FunctionID function, uint32_t resultIndex)
    {
        if (value.opcode == Opcode::GetCapture)
            return readFrom(PositionReference::entryParameter(function, value.index()));
        return resultIndex ? AssertedParts() : analysis.partsOfResult(function);
    }

    // What is no thenable is the result of awaiting it, and a promise hands over what the function behind it returned.
    AssertedParts declaredContents(const Value& value, uint32_t operandIndex, std::optional<ContentsSlot>)
    {
        if (!protocolsOf(value.opcode, operandIndex).contains(Protocol::Thenable))
            return { };
        AssertedParts awaited = analysis.partsOf(value.operand(operandIndex));
        AssertedParts result = AssertedParts::itself(awaited.itselfFrom);
        result.itselfFrom.add(awaited.contentsFrom);
        result.otherKinds = awaited.otherKinds;
        return result;
    }

    AssertedParts declaredResult(const Value& value, uint32_t operandIndex, uint32_t index)
    {
        const Value& producer = analysis.function().at(value.operand(operandIndex));
        std::optional<uint32_t> numberOfResults = numberOfResultsOf(analysis.program(), analysis.function(), producer);
        if (!numberOfResults || index >= *numberOfResults)
            return { };
        return applyResultRule(*this, producer, index, ResultSide::Proven);
    }

    AssertedParts unknown(const Value& value) { return analysis.partsOfCallResult(value); }

    AssertedParts none(const Instruction&) { return { }; }
    AssertedParts constant(const Value&) { return { }; }
    AssertedParts declaredParameter(const Value&) { return { }; }
    AssertedParts natural(const Instruction&, const auto&) { return { }; }
    AssertedParts allocation(const Value&, const auto&...) { return { }; }
    AssertedParts declared(const Instruction&, const auto&...) { return { }; }
};

} // anonymous namespace

bool AssertedObjectAnalysis::isAssertion(const DeclaredType& declared) const
{
    return mayYieldUnnamedObject(m_program, declared);
}

bool AssertedObjectAnalysis::isAssertion(const Value& value) const
{
    return value.opcode == Opcode::ClaimDeclared && isAssertion(value.declared()) && !givesOnlyObjectsNamedBy(value.declared(), value.valueOperand());
}

// `xs.slice(0, i) as U[]`, `filter(xs, f) as U[]`: a call reads as a value of no type, but where its callee is certain before flow the callee's own declaration says what it gives.
bool AssertedObjectAnalysis::givesOnlyObjectsNamedBy(const DeclaredType& assertion, ValueID operand) const
{
    const Value& call = m_function.at(definitionThroughSameValues(m_function, operand));
    if (call.opcode != Opcode::Call)
        return false;
    std::optional<Type> result = m_callees.tryTypeOfResultOfCertainCallee(call);
    if (!result)
        return false;
    const Type& named = m_types.at(assertion.written ? assertion.written : assertion.type);
    return result->withOnlyObjects().isSubsetOf(named.withoutProofs(), m_program);
}

AssertedOrigins AssertedObjectAnalysis::originsOfContentsRead(const Value& value) const
{
    if (value.opcode != Opcode::ClaimRead || value.source().kind() != ClaimSource::Kind::Contents)
        return { };
    return partsOf(value.receiverOperand()).contentsFrom;
}

// One read by name reads a field in each layout expected at its receiver.
uint32_t AssertedObjectAnalysis::numberOfAdmittingFieldsRead(const Value& read, PropertyKeyID key) const
{
    if (read.opcode != Opcode::PropGet || !key || !m_inputs.keysOfAdmittingFields.get(key.index()))
        return 0;
    uint32_t count = 0;
    m_callees.forEachExpectedLayout(read.objectOperand(), [&](LayoutID layout) {
        std::optional<FieldReference> field = findField(m_program, layout, key, BaseChain::Predicted);
        count += field && admitsObjects(fieldAt(m_program, *field).declared);
        m_callees.forEachInheritedClosure(ClosureHolder::Kind::Getter, layout, ClassSide::Prototype, key, [&](FunctionID getter) {
            count += !partsOfResult(getter).itselfFrom.isEmpty();
        });
    });
    m_callees.forEachStaticField(read.objectOperand(), key, [&](VariableID variable) {
        count += admitsObjects(m_program.at(variable).declared);
    });
    return count;
}

ValueKinds AssertedObjectAnalysis::otherKindsOfFieldsRead(const Value& read, PropertyKeyID key) const
{
    ValueKinds kinds;
    if (read.opcode != Opcode::PropGet || !key || !m_inputs.keysOfAdmittingFields.get(key.index()))
        return kinds;
    m_callees.forEachExpectedLayout(read.objectOperand(), [&](LayoutID layout) {
        if (std::optional<FieldReference> field = findField(m_program, layout, key, BaseChain::Predicted))
            kinds.add(kindsAdmittedAt(PositionReference::field(*field)));
        m_callees.forEachInheritedClosure(ClosureHolder::Kind::Getter, layout, ClassSide::Prototype, key, [&](FunctionID getter) {
            kinds.add(partsOfResult(getter).otherKinds);
        });
    });
    m_callees.forEachStaticField(read.objectOperand(), key, [&](VariableID variable) {
        kinds.add(kindsAdmittedAt(PositionReference::variable(variable)));
    });
    return kinds;
}

bool AssertedObjectAnalysis::mayCallFunctionWithAdmittingResult(const Value& call) const
{
    if (call.opcode != Opcode::Call)
        return false;
    std::optional<Mention> holder = mentionOf(m_function.at(definitionThroughSameValues(m_function, call.calleeOperand())));
    return !holder || m_inputs.holdersOfAdmittingResults.contains(*holder);
}

AssertedParts AssertedObjectAnalysis::partsOfCallResult(const Value& call) const
{
    AssertedParts result;
    if (!mayCallFunctionWithAdmittingResult(call))
        return result;
    m_callees.forEachKnownCallee(call.calleeOperand(), [&](FunctionID callee) {
        result.add(partsOfResult(callee, call));
    });
    return result;
}

uint32_t AssertedObjectAnalysis::numberOfOriginsOfCallResult(const Value& call) const
{
    uint32_t count = 0;
    if (!mayCallFunctionWithAdmittingResult(call))
        return count;
    m_callees.forEachKnownCallee(call.calleeOperand(), [&](FunctionID callee) {
        AssertedParts parts = partsOfResult(callee);
        if (!parts.isEmpty())
            count += m_inputs.originsOfResults[callee.index()].size() + parts.otherKinds.size();
    });
    return count;
}

// For whoever reads the result without passing arguments of its own.
AssertedParts AssertedObjectAnalysis::partsOfResult(FunctionID id) const
{
    const Function& function = m_program.at(id);
    if (function.results.size() != 1)
        return { };
    AssertedParts parts = AssertedParts::ofOtherKinds(kindsAdmittedAt(PositionReference::result(id, 0)));
    if (admitsObjects(function.results[0]))
        parts.add(function.isUnsplitCoroutine() ? AssertedParts::contents(AssertedOrigins::function()) : AssertedParts::itself(AssertedOrigins::function()));
    return parts;
}

AssertedParts AssertedObjectAnalysis::partsOfResult(FunctionID id, const Value& call) const
{
    AssertedParts forAnyCaller = partsOfResult(id);
    if (forAnyCaller.isEmpty())
        return { };

    // The origins as the callee sees them, told as this function sees them.
    const Function& callee = m_program.at(id);
    AssertedOrigins originsInCallee = m_inputs.originsOfResults[id.index()];
    AssertedOrigins origins;
    if (originsInCallee.containsFunction())
        origins.add(AssertedOrigins::function());
    originsInCallee.forEachEntryParameter([&](uint32_t entryParameter) {
        if (callee.thisParameterIndex() == entryParameter) {
            origins.add(partsOf(call.thisValueOperand()).itselfFrom);
            return;
        }
        std::span<const ValueID> arguments = call.variadicOperands();
        size_t position = entryParameter - callee.firstParameterIndex();
        if (position >= arguments.size())
            return;
        bool isRestParameter = callee.has(FunctionFlags::HasRestParameter) && position == callee.numberOfPositionalParameters();
        for (ValueID argument : arguments.subspan(position, isRestParameter ? std::dynamic_extent : 1))
            origins.add(partsOf(argument).itselfFrom);
    });
    AssertedParts parts = callee.isUnsplitCoroutine() ? AssertedParts::contents(origins) : AssertedParts::itself(origins);
    parts.otherKinds = forAnyCaller.otherKinds;
    return parts;
}

void AssertedObjectAnalysis::retypeDependentsOf(std::span<const ValueID> values)
{
    if (values.empty())
        return;
    TypePropagation propagation(m_propagationInputs);
    propagation.restartDependentsOf(values);
    propagation.solveTypes();
    propagation.solvePredictions();
}

AssertedParts AssertedObjectAnalysis::computeParameter(BlockID block, uint32_t parameterIndex) const
{
    if (block != entryBlock) {
        AssertedParts result;
        m_callees.forEachIncomingValue(block, parameterIndex, [&](ValueID incoming) {
            result.add(partsOf(incoming));
        });
        return result;
    }
    PositionReference position = PositionReference::entryParameter(m_id, parameterIndex);
    AssertedParts parts = AssertedParts::ofOtherKinds(kindsAdmittedAt(position));
    const DeclaredType* declared = findPosition(m_program, position);
    if (!declared || !admitsObjects(*declared))
        return parts;
    if (m_function.has(FunctionFlags::HasRestParameter) && declared == &m_function.parameters.last()) {
        parts.add(AssertedParts::contents(AssertedOrigins::entryParameter(parameterIndex)));
        return parts;
    }
    // What a closure has captured does not depend on who calls it.
    bool isPassedByCaller = parameterIndex >= m_function.firstParameterIndex() || m_function.thisParameterIndex() == parameterIndex;
    parts.add(AssertedParts::itself(isPassedByCaller ? AssertedOrigins::entryParameter(parameterIndex) : AssertedOrigins::function()));
    return parts;
}

AssertedParts AssertedObjectAnalysis::computeValue(const Value& value) const
{
    if (numberOfResultsOf(m_program, m_function, value))
        return { };
    AssertedObjectRules rules { *this };
    return applyResultRule(rules, value, 0, ResultSide::Proven);
}

void AssertedObjectAnalysis::collectValues()
{
    m_parts.fill(AssertedParts(), m_function.values.size());
    for (auto [request, kinds] : m_requestsThatAlwaysFailed)
        m_parts[request.index()].otherKinds.add(kinds);
    auto add = [&](ValueID id, AssertedParts parts) {
        if (m_parts[id.index()].containsAll(parts))
            return false;
        m_parts[id.index()].add(parts);
        return true;
    };
    for (bool didChange = true; didChange;) {
        didChange = false;
        for (BlockID id : m_analyses.cfg().reversePostOrder()) {
            const Block& block = m_function.at(id);
            for (uint32_t index = 0; index < block.parameters.size(); ++index)
                didChange |= add(block.parameters[index], computeParameter(id, index));
            for (ValueID value : block.values)
                didChange |= add(value, computeValue(m_function.at(value)));
        }
    }
}

// Each position counts, not each value that reads one: the type of a read by name depends on every field it reads.
// Before any value is known to hold an asserted object the rules answer for the sources alone.
uint32_t AssertedObjectAnalysis::countSources() const
{
    uint32_t count = 0;
    for (uint32_t index = 0; index < m_function.numberOfEntryParameters(); ++index)
        count += computeParameter(entryBlock, index).numberOfSources();
    for (const Value& value : m_function.values) {
        if (value.opcode == Opcode::Call)
            count += numberOfOriginsOfCallResult(value);
        else if (value.opcode == Opcode::PropGet)
            count += numberOfAdmittingFieldsRead(value, value.key()) + otherKindsOfFieldsRead(value, value.key()).size();
        else if (resultRuleOf(value.opcode) == ResultRule::Declared || resultRuleOf(value.opcode) == ResultRule::Unknown || value.opcode == Opcode::ClaimDeclared)
            count += computeValue(value).numberOfSources();
    }
    for (const Block& block : m_function.blocks) {
        if (block.terminator.kind != TerminatorKind::ReturnCall)
            continue;
        for (TailCast cast : block.terminator.casts())
            count += isAssertion(cast.declared);
    }
    return count;
}

// The values whose type a widened position changes: no operand of theirs holds an asserted object, so they have it from a position.
Vector<ValueID> AssertedObjectAnalysis::valuesReadFromPositions() const
{
    Vector<ValueID> result;
    for (size_t index = 0; index < m_parts.size(); ++index) {
        if (m_parts[index].isEmpty())
            continue;
        ValueID id(index);
        ValueID operand = sameValueOperandOf(m_function.at(id));
        bool isJoin = m_function.at(id).opcode == Opcode::Parameter && m_analyses.useLists().locationOf(id).block != entryBlock;
        if (!isJoin && partsOf(operand).isEmpty())
            result.append(id);
    }
    return result;
}

bool AssertedObjectAnalysis::hasObjectsOutside(const Type& value, const DeclaredType& declared) const
{
    const Type& tested = m_types.at(declared.*testedPartOf(declared, m_policy));
    if (!value.hasObjects() || tested.anyObject || declared.flags.contains(PositionFlags::IsInt32Promise))
        return false;
    return !value.withOnlyObjects().isSubsetOf(tested.withoutProofs(), m_program);
}

bool AssertedObjectAnalysis::admit(DeclaredType& declared, const Type& entering)
{
    if (!hasObjectsOutside(entering, declared))
        return false;
    TypeID& tested = declared.*testedPartOf(declared, m_policy);
    tested = m_types.unionOf(tested, TypeTable::object);
    declared.flags.add(PositionFlags::AdmitsObjects);
    return true;
}

ValueKinds AssertedObjectAnalysis::otherKindsOutside(const Type& entering, const DeclaredType& declared, ValueKinds followed) const
{
    if (followed.isEmpty() || declared.flags.containsAny({ PositionFlags::IsInt32Promise, PositionFlags::IsFromCompiler }))
        return { };
    return kindsOutside(m_program, entering, m_types.at(declared.type), followed, m_types);
}

bool AssertedObjectAnalysis::admitOtherKinds(DeclaredType& declared, const Type& entering, ValueKinds followed)
{
    ValueKinds outside = otherKindsOutside(entering, declared, followed);
    if (outside.isEmpty())
        return false;
    admitKinds(declared, outside, m_types);
    ++m_findings.numberOfRequestsAdmittingOtherKinds;
    return true;
}

Vector<ValueID> AssertedObjectAnalysis::admitInRequestsThatAlwaysFail()
{
    Vector<ValueID> changedRequests;
    if (m_policy == MismatchPolicy::Report)
        return changedRequests;
    for (BlockID id : m_analyses.cfg().reversePostOrder()) {
        for (ValueID valueID : m_function.at(id).values) {
            Value& request = m_function.at(valueID);
            if (request.opcode != Opcode::ClaimDeclared)
                continue;
            DeclaredType declared = request.declared();
            if (declared.flags.containsAny({ PositionFlags::IsInt32Promise, PositionFlags::IsFromCompiler }))
                continue;
            const Type& entering = m_typing.typeOf(request.valueOperand());
            if (!alwaysFailsConversion(entering, m_types.at(declared.type), m_types))
                continue;
            ValueKinds kinds = ValueKinds::of(entering);
            admitKinds(declared, kinds, m_types);
            request.setDeclared(declared);
            m_requestsThatAlwaysFailed.append({ valueID, kinds });
            changedRequests.append(valueID);
        }
    }
    m_findings.numberOfRequestsAdmittingOtherKinds += changedRequests.size();
    return changedRequests;
}

void AssertedObjectAnalysis::admitInTailCasts(BlockID block)
{
    Terminator& terminator = m_function.at(block).terminator;
    if (terminator.kind != TerminatorKind::ReturnCall)
        return;

    bool didChange = false;
    bool holdsAssertedObject = holds(terminator.callOperand());
    ValueKinds followed = otherKindsOf(terminator.callOperand());
    Type entering = m_typing.typeOf(terminator.callOperand());
    Vector<TailCast, 4> casts;
    for (TailCast cast : terminator.casts()) {
        bool letsObjectsThrough = holdsAssertedObject || (m_policy == MismatchPolicy::Report && isAssertion(cast.declared));
        holdsAssertedObject |= isAssertion(cast.declared);
        if (letsObjectsThrough && admit(cast.declared, entering)) {
            ++m_findings.numberOfAdmittingRequests;
            didChange = true;
        }
        didChange |= admitOtherKinds(cast.declared, entering, followed);
        entering = entering.castTo(m_typing.typeOf(cast.declared), m_types);
        casts.append(cast);
    }
    if (didChange)
        terminator.setCasts(casts.span());
}

// The front end gives the type of an assertion its object part. A build that reports compares with the written part, which has none.
bool AssertedObjectAnalysis::letsAssertedObjectsThrough(const Value& request) const
{
    return holds(request.valueOperand()) || (m_policy == MismatchPolicy::Report && isAssertion(request));
}

Vector<ValueID> AssertedObjectAnalysis::admitInRequests()
{
    Vector<ValueID> changedRequests;
    for (BlockID id : m_analyses.cfg().reversePostOrder()) {
        for (ValueID valueID : m_function.at(id).values) {
            Value& request = m_function.at(valueID);
            if (request.opcode != Opcode::ClaimDeclared)
                continue;
            bool letsObjectsThrough = letsAssertedObjectsThrough(request);
            ValueKinds followed = otherKindsOf(request.valueOperand());
            if (!letsObjectsThrough && followed.isEmpty())
                continue;
            DeclaredType declared = request.declared();
            const Type& entering = m_typing.typeOf(request.valueOperand());
            bool didAdmitObjects = letsObjectsThrough && admit(declared, entering);
            bool didAdmitOtherKinds = admitOtherKinds(declared, entering, followed);
            if (!didAdmitObjects && !didAdmitOtherKinds)
                continue;
            m_findings.numberOfAdmittingRequests += didAdmitObjects;
            request.setDeclared(declared);
            changedRequests.append(valueID);
        }
    }
    return changedRequests;
}

// A position that takes every object anyway is left alone, and what is read from it is no asserted object: its type names whatever it holds.
void AssertedObjectAnalysis::reach(PositionReference position, AssertedObjectUse use, const Type& entering)
{
    const DeclaredType* declared = findPosition(m_program, position);
    if (!declared)
        return;
    if (holds(use.value) && hasObjectsOutside(entering, *declared))
        m_findings.positions.append({ position, PositionPart::Value, use });
    if (ValueKinds outside = otherKindsOutside(entering, *declared, otherKindsOf(use.value)); !outside.isEmpty())
        m_findings.positions.append({ position, PositionPart::Value, use, { }, outside });
}

void AssertedObjectAnalysis::reachFields(ValueID receiver, PropertyKeyID key, AssertedObjectUse use)
{
    m_callees.forEachExpectedLayout(receiver, [&](LayoutID layout) {
        if (std::optional<FieldReference> field = findField(m_program, layout, key, BaseChain::Predicted))
            reach(PositionReference::field(*field), use);
        m_callees.forEachInheritedClosure(ClosureHolder::Kind::Setter, layout, ClassSide::Prototype, key, [&](FunctionID setter) {
            reachParameter(setter, 0, use);
        });
    });
    m_callees.forEachStaticField(receiver, key, [&](VariableID variable) {
        reach(PositionReference::variable(variable), use);
    });
}

void AssertedObjectAnalysis::reachResult(AssertedObjectUse use, const Type& entering, AssertedOrigins origins)
{
    if (m_function.results.size() != 1)
        return;
    if (!m_function.isUnsplitCoroutine()) {
        const DeclaredType& declared = m_function.results[0];
        if (hasObjectsOutside(entering, declared) || (admitsObjects(declared) && entering.hasObjects()))
            m_findings.positions.append({ PositionReference::result(m_id, 0), PositionPart::Value, use, origins });
        return;
    }
    if (m_function.has(FunctionFlags::IsAsync) && !m_function.has(FunctionFlags::IsGenerator))
        reachContentsOfResult(use, origins);
}

// The result of a coroutine is a promise or a generator, which is an object anyway. What it is fulfilled with or yields comes out where it is awaited or iterated.
void AssertedObjectAnalysis::reachContentsOfResult(AssertedObjectUse use, AssertedOrigins origins)
{
    if (m_function.results.size() == 1 && m_typing.typeOf(use.value).hasObjects())
        m_findings.positions.append({ PositionReference::result(m_id, 0), PositionPart::Value, use, origins });
}

void AssertedObjectAnalysis::reachResultWithOtherKinds(AssertedObjectUse use, const Type& entering)
{
    if (m_function.results.size() != 1 || m_function.isUnsplitCoroutine())
        return;
    if (ValueKinds outside = otherKindsOutside(entering, m_function.results[0], otherKindsOf(use.value)); !outside.isEmpty())
        m_findings.positions.append({ PositionReference::result(m_id, 0), PositionPart::Value, use, { }, outside });
}

std::optional<PositionReference> AssertedObjectAnalysis::positionOfDeclaredOperand(const Value& user, uint32_t operandIndex) const
{
    auto variadicIndex = [&] {
        return *user.variadicOperandIndex(operandIndex);
    };
    switch (user.opcode) {
    case Opcode::PutModuleVariable:
        return PositionReference::variable(user.variable());
    case Opcode::PutEnvironmentField:
    case Opcode::PrivateDefine:
        return PositionReference::field({ user.layout(), user.field() });
    case Opcode::NewClosure:
    case Opcode::CallDirect:
        return PositionReference::entryParameter(user.function(), variadicIndex());
    case Opcode::NewObject: {
        std::span<const uint32_t> fields = user.fields();
        if (variadicIndex() >= fields.size())
            return std::nullopt;
        return PositionReference::field({ user.layout(), fields[variadicIndex()] });
    }
    default:
        return std::nullopt;
    }
}

void AssertedObjectAnalysis::reachParameter(FunctionID id, uint32_t position, AssertedObjectUse use)
{
    const Function& function = m_program.at(id);
    uint32_t numberOfPositionalParameters = function.numberOfPositionalParameters();
    if (position < numberOfPositionalParameters) {
        reach(PositionReference::entryParameter(id, function.firstParameterIndex() + position), use);
        return;
    }
    if (!function.has(FunctionFlags::HasRestParameter) || !holds(use.value))
        return;
    const ClassPart* array = m_types.at(function.parameters.last().type).findClassPart(BuiltinClasses::Array);
    if (!array)
        return;
    DeclaredType elements;
    elements.type = array->contentsAt(m_program.builtins(), ContentsSlot::Element);
    if (hasObjectsOutside(m_typing.typeOf(use.value), elements))
        m_findings.positions.append({ PositionReference::entryParameter(id, function.firstParameterIndex() + numberOfPositionalParameters), PositionPart::Elements, use });
}

void AssertedObjectAnalysis::visitArgument(ValueID callee, uint32_t position, AssertedObjectUse use)
{
    bool hasKnownCallee = false;
    m_callees.forEachKnownCallee(callee, [&](FunctionID id) {
        hasKnownCallee = true;
        reachParameter(id, position, use);
    });
    bool mayBeCompiledFunction = m_types.at(m_function.at(callee).predictionOrType()).builtins.isEmpty();
    if (!hasKnownCallee && mayBeCompiledFunction && holds(use.value) && m_typing.typeOf(use.value).hasObjects())
        m_findings.argumentsOfUnknownCallees.append(use);
}

void AssertedObjectAnalysis::visitUse(const Value& user, ValueID operand, uint32_t operandIndex)
{
    AssertedObjectUse use { operand, user.origin() };
    if (user.operandKind(operandIndex) == OperandKind::Declared) {
        if (std::optional<PositionReference> position = positionOfDeclaredOperand(user, operandIndex))
            reach(*position, use);
        return;
    }

    switch (user.opcode) {
    case Opcode::PropSet:
    case Opcode::PropDefine:
        if (storedOf(user.opcode) == operandIndex)
            reachFields(user.objectOperand(), user.key(), use);
        return;
    case Opcode::PrivateSet:
        if (storedOf(user.opcode) == operandIndex)
            reach(PositionReference::field({ user.layout(), user.field() }), use);
        return;
    case Opcode::Call:
    case Opcode::Construct:
        if (std::optional<uint32_t> position = user.variadicOperandIndex(operandIndex))
            visitArgument(user.calleeOperand(), *position, use);
        return;
    case Opcode::Yield:
        if (holds(use.value))
            reachContentsOfResult(use, partsOf(use.value).itselfFrom);
        return;
    case Opcode::SuperConstruct: {
        LayoutID base = predictedHeritageOf(m_program.at(user.layout()));
        std::optional<uint32_t> position = user.variadicOperandIndex(operandIndex);
        if (position && base && m_program.at(base).constructor)
            reachParameter(m_program.at(base).constructor, *position, use);
        return;
    }
    default:
        return;
    }
}

void AssertedObjectAnalysis::visitTerminator(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Return: {
        ValueID returned = terminator.valueOperand();
        if (holds(returned))
            reachResult({ returned, terminator.origin() }, m_typing.typeOf(returned), partsOf(returned).itselfFrom);
        if (!otherKindsOf(returned).isEmpty())
            reachResultWithOtherKinds({ returned, terminator.origin() }, m_typing.typeOf(returned));
        return;
    }
    case TerminatorKind::ReturnCall: {
        AssertedOrigins origins = partsOf(terminator.callOperand()).itselfFrom;
        Type entering = m_typing.typeOf(terminator.callOperand());
        for (TailCast cast : terminator.casts()) {
            if (isAssertion(cast.declared))
                origins.add(AssertedOrigins::function());
            entering = entering.castTo(m_typing.typeOf(cast.declared), m_types);
        }
        if (!origins.isEmpty())
            reachResult({ terminator.callOperand(), terminator.origin() }, entering, origins);
        reachResultWithOtherKinds({ terminator.callOperand(), terminator.origin() }, entering);
        return;
    }
    default:
        return;
    }
}

void AssertedObjectAnalysis::collectReachedPositions()
{
    for (BlockID id : m_analyses.cfg().reversePostOrder()) {
        const Block& block = m_function.at(id);
        for (ValueID valueID : block.values) {
            const Value& user = m_function.at(valueID);
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (holds(operand) || !otherKindsOf(operand).isEmpty())
                    visitUse(user, operand, operandIndex);
            });
        }
        visitTerminator(block.terminator);
    }
}

AssertedObjectFindings AssertedObjectAnalysis::run(uint32_t previousNumberOfSources)
{
    bool wasTyped = hasTypedValues(m_function);
    if (!wasTyped) {
        typeValuesWithoutCutting(m_propagationInputs);
        for (Vector<ValueID> changedRequests = admitInRequestsThatAlwaysFail(); !changedRequests.isEmpty(); changedRequests = admitInRequestsThatAlwaysFail())
            retypeDependentsOf(changedRequests.span());
    }
    m_findings.didTypeAllValues = !wasTyped;
    m_findings.numberOfSources = countSources();
    if (m_requestsThatAlwaysFailed.isEmpty() && m_findings.numberOfSources == previousNumberOfSources)
        return WTF::move(m_findings);
    m_findings.didFollowAssertedObjects = true;
    collectValues();
    if (wasTyped)
        retypeDependentsOf(valuesReadFromPositions().span());

    // A request that lets objects through gives its result more objects, which the request behind it has to let through.
    for (Vector<ValueID> changedRequests = admitInRequests(); !changedRequests.isEmpty(); changedRequests = admitInRequests())
        retypeDependentsOf(changedRequests.span());
    for (BlockID block : m_analyses.cfg().reversePostOrder())
        admitInTailCasts(block);
    collectReachedPositions();
    return WTF::move(m_findings);
}

AssertedObjectFindings findAssertedObjects(const AssertedObjectInputs& inputs, FunctionID id, Function& function, uint32_t previousNumberOfSources, TypeArena& types, BuildContext& build)
{
    FunctionAnalyses analyses(AnalysisContext { build, inputs.program, nullptr, nullptr, id }, function);
    return AssertedObjectAnalysis(inputs, id, function, analyses, types, build).run(previousNumberOfSources);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
