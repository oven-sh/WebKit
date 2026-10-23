/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringUses.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTOpcodesNamingRecords.h"
#include "AOTPipeline.h"
#include "AOTPlacement.h"
#include "AOTPropertyAccess.h"
#include "AOTRegExpRequests.h"
#include "AOTSchemaWalkers.h"
#include "AOTSlotPlanSteps.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <array>
#include <concepts>
#include <functional>
#include <wtf/Atomics.h>
#include <wtf/FastBitVector.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT {

using namespace Typed;

static_assert(FunctionID(0u) < FunctionID(), "a minimum over functions starts at the null id");
static_assert(ModuleID(0u) < ModuleID(), "a function without a module sorts last");

static constexpr OpcodeSet keyOrComparisonOpcodes {
    Opcode::StrictEq, Opcode::Eq, Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine, Opcode::SuperGet,
    Opcode::SuperSet, Opcode::GetByValWithThis, Opcode::ToPropertyKey, Opcode::CallBuiltin, Opcode::CallInline,
};
static constexpr TerminatorKindSet keyOrComparisonTerminators { TerminatorKind::TryCallInline };

static constexpr bool usesOperandsAsKeys(Opcode opcode) { return keyOrComparisonOpcodes.contains(opcode); }
static constexpr bool usesOperandsAsKeys(TerminatorKind kind) { return keyOrComparisonTerminators.contains(kind); }

// A fused access reads its keys by name, also when the reports count it as a call.
template<typename Operation>
static constexpr bool looksUpKeysByName(Operation operation)
{
    return accessClassOf(operation) == AccessClass::ByName || familyOf(operation) == OperationFamily::FusedAccess;
}

// The text is read when an error is made: the name of what is called, of a variable that has no value yet, of what is null, the message itself.
static bool hasStringsForMessagesOnly(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Call:
    case Opcode::CallList:
    case Opcode::CallMethod:
    case Opcode::CallResolved:
    case Opcode::Construct:
    case Opcode::ConstructList:
    case Opcode::CheckInitialized:
    case Opcode::RequireObjectCoercible:
    case Opcode::ToObject:
    case Opcode::NewRuntimeError:
    case Opcode::ThrowNewError:
        return true;
    default:
        return false;
    }
}

static bool hasStringsForMessagesOnly(TerminatorKind)
{
    return false;
}

namespace {

struct SharedUses {
    explicit SharedUses(const Program& program)
        : isNamed(program.strings.size())
        , isNamedBesideCreation(program.strings.size())
        , isConstant(program.strings.size())
        , isCreatedRepeatedly(program.strings.size())
        , isAtom(program.strings.size())
        , keys(program.types.numberOfPropertyKeys())
    {
    }

    FastBitVector isNamed;
    FastBitVector isNamedBesideCreation;
    FastBitVector isConstant;
    FastBitVector isCreatedRepeatedly;
    FastBitVector isAtom;
    Vector<KeyUse> keys;
};

class FunctionWalk {
public:
    FunctionWalk(const PipelineOutput&, BuildContext&, FunctionID, SharedUses&);

    Vector<uint32_t> run();

private:
    bool runsAtMostOnce(BlockID);
    void visit(const Value&, bool runsAtMostOnce);
    template<typename InstructionType, typename Operation> void visit(const InstructionType&, Operation, bool runsAtMostOnce);
    template<typename InstructionType> void markAtomStrings(const InstructionType&, bool usesEveryOperandAsKey);
    void nameString(StringID);
    void countKey(PropertyKeyID, Access, bool looksUpInTables);
    void countCreationSite(StringID, bool runsAtMostOnce);

    const Program& m_program;
    const Function& m_function;
    FunctionID m_id;
    AnalysisContext m_context;
    bool m_functionRunsAtMostOnce;
    std::optional<FunctionAnalyses> m_analyses;
    SharedUses& m_shared;
    Vector<uint32_t> m_namedStrings;
    UncheckedKeyHashSet<StringID> m_namedStringSet;
};

} // anonymous namespace

static void addRelaxed(uint32_t& counter)
{
    WTF::atomicExchangeAdd(&counter, 1u, std::memory_order_relaxed);
}

static void atomicMinRelaxed(FunctionID& minimum, FunctionID candidate)
{
    for (FunctionID current = WTF::atomicLoad(&minimum, std::memory_order_relaxed); candidate < current && !WTF::atomicCompareExchangeWeakRelaxed(&minimum, current, candidate);)
        current = WTF::atomicLoad(&minimum, std::memory_order_relaxed);
}

static BitVector toBitVector(const FastBitVector& bits)
{
    BitVector result(bits.numBits());
    bits.forEachSetBit([&](size_t index) {
        result.quickSet(index);
    });
    return result;
}

static StringID impliedNameOf(const Program& program, const Value& value)
{
    if (familyOf(value.opcode) != OperationFamily::PrivateAccess)
        return { };
    PropertyKeyID key = keyOfNamedAccess(program, value);
    return key ? program.at(key).text : StringID();
}

FunctionWalk::FunctionWalk(const PipelineOutput& pipeline, BuildContext& context, FunctionID id, SharedUses& shared)
    : m_program(*pipeline.program)
    , m_function(m_program.at(id))
    , m_id(id)
    , m_context { context, m_program, pipeline.flow.get(), nullptr, id }
    , m_functionRunsAtMostOnce(pipeline.flow->runsAtMostOnce(m_program, id))
    , m_shared(shared)
{
}

bool FunctionWalk::runsAtMostOnce(BlockID block)
{
    if (!m_functionRunsAtMostOnce)
        return false;
    if (!m_analyses)
        m_analyses.emplace(m_context, m_function);
    return m_analyses->executionCountOf(block) == ExecutionCount::AtMostOnce;
}

template<typename Operation>
static bool takesKeyAsOperand(Operation operation)
{
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(operation) + (hasVariadicOperand(operation) ? 1 : 0);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (operandKindOf(operation, operandIndex) == OperandKind::PropertyKey)
            return true;
    }
    return false;
}

static OpcodeSet findOpcodesWithUses()
{
    OpcodeSet opcodes = findOpcodesNaming<StringID, PropertyKeyID>() | keyOrComparisonOpcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (takesKeyAsOperand(opcode) || familyOf(opcode) == OperationFamily::PrivateAccess)
            opcodes.add(opcode);
    }
    return opcodes;
}

static TerminatorKindSet findTerminatorKindsWithUses()
{
    TerminatorKindSet kinds = findTerminatorKindsNaming<StringID, PropertyKeyID>() | keyOrComparisonTerminators;
    for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
        if (takesKeyAsOperand(kind))
            kinds.add(kind);
    }
    return kinds;
}

void FunctionWalk::visit(const Value& value, bool runsAtMostOnce)
{
    visit(value, value.opcode, runsAtMostOnce);
    nameString(impliedNameOf(m_program, value));
}

Vector<uint32_t> FunctionWalk::run()
{
    static const OpcodeSet opcodesWithUses = findOpcodesWithUses();
    static const TerminatorKindSet terminatorKindsWithUses = findTerminatorKindsWithUses();
    if (!mayEndBlockWith(m_program, m_id, terminatorKindsWithUses)) {
        BlockID current;
        bool currentRunsAtMostOnce = false;
        forEachListedValue(m_program, m_id, opcodesWithUses, [&](BlockID block, uint32_t, ValueID, const Value& value) {
            if (block != current) {
                current = block;
                currentRunsAtMostOnce = runsAtMostOnce(block);
            }
            visit(value, currentRunsAtMostOnce);
        });
        return WTF::move(m_namedStrings);
    }

    for (uint32_t blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
        BlockID id(blockIndex);
        const Block& block = m_function.at(id);
        bool blockRunsAtMostOnce = runsAtMostOnce(id);
        for (ValueID valueID : block.values)
            visit(m_function.at(valueID), blockRunsAtMostOnce);
        visit(block.terminator, block.terminator.kind, blockRunsAtMostOnce);
    }
    return WTF::move(m_namedStrings);
}

template<typename InstructionType, typename Operation>
void FunctionWalk::visit(const InstructionType& instruction, Operation operation, bool runsAtMostOnce)
{
    bool createsStringConstant = runtimeOf(operation) == RuntimeOperation::StringConstant;
    bool looksUpInTables = !runsAtMostOnce && looksUpKeysByName(operation);
    forEachReference(instruction, [&]<typename Reference>(const Reference& reference) {
        if constexpr (std::same_as<Reference, StringID>) {
            nameString(reference);
            if (createsStringConstant)
                countCreationSite(reference, runsAtMostOnce);
            else if (reference && !hasStringsForMessagesOnly(operation))
                m_shared.isNamedBesideCreation.atomicSetAndCheck(reference.index(), true);
        } else if constexpr (std::same_as<Reference, PropertyKeyID>)
            countKey(reference, accessOf(operation), looksUpInTables);
    });
    markAtomStrings(instruction, usesOperandsAsKeys(operation));
}

template<typename InstructionType>
void FunctionWalk::markAtomStrings(const InstructionType& instruction, bool usesEveryOperandAsKey)
{
    instruction.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!usesEveryOperandAsKey && instruction.operandKind(operandIndex) != OperandKind::PropertyKey)
            return;
        const Value& definition = m_function.at(sameObjectThroughCasts(m_function, operand));
        if (definition.opcode == Opcode::String)
            m_shared.isAtom.atomicSetAndCheck(definition.string().index(), true);
    });
}

void FunctionWalk::nameString(StringID string)
{
    if (!string || !m_namedStringSet.add(string).isNewEntry)
        return;
    m_shared.isNamed.atomicSetAndCheck(string.index(), true);
    m_namedStrings.append(string.index());
}

void FunctionWalk::countKey(PropertyKeyID key, Access access, bool looksUpInTables)
{
    KeyUse& use = m_shared.keys[key.index()];
    addRelaxed(use.uses);
    addRelaxed(use.usesByAccess[static_cast<size_t>(access)]);
    nameString(m_program.at(key).text);
    if (!looksUpInTables)
        return;
    addRelaxed(use.tableLookupUses);
    atomicMinRelaxed(use.firstTableLookupFunction, m_id);
}

void FunctionWalk::countCreationSite(StringID string, bool runsAtMostOnce)
{
    bool isFirstSite = m_shared.isConstant.atomicSetAndCheck(string.index(), true);
    if (!isFirstSite || !runsAtMostOnce)
        m_shared.isCreatedRepeatedly.atomicSetAndCheck(string.index(), true);
}

static void addStringsOfRegExps(const RegExpRequests& requests, StringUses& strings)
{
    requests.forEachString([&](StringID string) {
        strings.isNamed.quickSet(string.index());
        strings.isNamedBesideCreation.quickSet(string.index());
    });
    for (const RegExpRequest& request : requests.all()) {
        for (StringID name : request.captureGroupNames) {
            if (name)
                strings.isAtom.quickSet(name.index());
        }
        for (const RegExpNamedGroupStrings& group : request.namedGroups)
            strings.isAtom.quickSet(group.name.index());
    }
}

static void addLiteralStringsWithoutWord(const Typed::LiteralStringTable& literalStrings, StringUses& strings)
{
    for (StringID string : literalStrings.stringsWithoutWord()) {
        strings.isNamed.quickSet(string.index());
        strings.isNamedBesideCreation.quickSet(string.index());
    }
}

static void addStringConstantsInTables(const Placement& placement, StringUses& strings)
{
    strings.isConstantInTables = placement.stringConstantsInTables();
    strings.isNamed.merge(strings.isConstantInTables);
    strings.isConstant.merge(strings.isConstantInTables);
    strings.isCreatedRepeatedly.merge(strings.isConstantInTables);
}

static Vector<uint32_t> sortFunctionsBySourcePosition(const Program& program, const ParallelLoops& loops)
{
    using PositionAndIndex = std::array<uint32_t, 3>;
    Vector<PositionAndIndex> positionsAndIndices(program.functions.size(), [&](size_t index) {
        const Function& function = program.functions[index];
        return PositionAndIndex { function.module.index(), function.position, static_cast<uint32_t>(index) };
    });
    parallelSort(loops, positionsAndIndices.mutableSpan(), std::less<PositionAndIndex> { });
    return Vector<uint32_t>(program.functions.size(), [&](size_t index) {
        return positionsAndIndices[index][2];
    });
}

static void collectNamesInTables(const Program& program, const FlowResults& flow, const Placement& placement, KeyUses& keys)
{
    keys.keysNamedByTables.ensureSize(program.types.numberOfPropertyKeys());
    keys.stringsNamedAsKeysByTables.ensureSize(program.strings.size());
    auto nameKey = [&](PropertyKeyID key) {
        if (key)
            keys.keysNamedByTables.quickSet(key.index());
    };
    auto nameText = [&](StringID text) {
        if (text)
            keys.stringsNamedAsKeysByTables.quickSet(text.index());
    };

    for (const Layout& layout : program.layouts) {
        if (layout.isEnvironment())
            continue;
        for (const Field& field : layout.fields)
            nameKey(field.key);
        for (const StaticField& field : layout.staticFields)
            nameKey(field.key);
        for (const ClassMember& member : layout.classFlow.members)
            nameKey(member.key);
    }
    for (size_t type : placement.typesWithCells()) {
        for (PropertyKeyID symbol : program.at(TypeID(type)).symbolLiterals)
            nameKey(symbol);
    }
    for (const PlacedPropertyRun& run : placement.propertyRuns().runs()) {
        for (PropertyKeyID key : run.keys)
            nameKey(key);
    }
    for (const PlacedGuardBit& guardBit : placement.guardBits().bits)
        nameKey(guardBit.key);
    for (PropertyKeyID key : placement.keyCells())
        nameKey(key);
    for (const RequestRecord& record : placement.requests().records()) {
        DataID data = WTF::switchOn(record,
            [](const DataRecord& request) { return request.data; },
            [](const CollectionDataRecord& request) { return request.data; },
            [](const auto&) { return DataID(); });
        if (!data)
            continue;
        forEachDataNodeInPreorder(program.at(data).root, [&](uint32_t, const DataNode& node) {
            const DataNode::Object* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            const Layout* layout = object->layout ? &program.at(object->layout) : nullptr;
            for (const DataMember& member : object->members) {
                if (!layout || !layout->findField(member.key))
                    nameKey(member.key);
            }
        });
    }

    for (unsigned index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        for (const ModuleExport& moduleExport : module.exports) {
            if (!isDescribedToRunTime(program, flow, ModuleID(index), moduleExport))
                continue;
            nameText(moduleExport.name);
            nameText(moduleExport.importName);
        }
        for (const SyntheticExport& syntheticExport : module.syntheticExports)
            nameText(syntheticExport.name);
        for (const RequiredName& requiredName : module.requiredNames)
            nameText(requiredName.name);
    }
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings)
            nameText(binding.name);
    }
}

ProgramUses collectProgramUses(const PipelineOutput& pipeline, BuildContext& context)
{
    const Program& program = *pipeline.program;
    const ParallelLoops& loops = context.parallelLoops();
    unsigned numberOfFunctions = program.functions.size();

    SharedUses shared(program);
    ProgramUses uses;
    uses.strings.namedByFunction.grow(numberOfFunctions);
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        if (program.functions[index].hasBody())
            uses.strings.namedByFunction[index] = FunctionWalk(pipeline, context, FunctionID(index), shared).run();
    });

    uses.strings.isNamed = toBitVector(shared.isNamed);
    uses.strings.isNamedBesideCreation = toBitVector(shared.isNamedBesideCreation);
    uses.strings.isConstant = toBitVector(shared.isConstant);
    uses.strings.isCreatedRepeatedly = toBitVector(shared.isCreatedRepeatedly);
    uses.strings.isAtom = toBitVector(shared.isAtom);
    addStringsOfRegExps(*pipeline.regExpRequests, uses.strings);
    addLiteralStringsWithoutWord(*pipeline.literalStrings, uses.strings);
    addStringConstantsInTables(*pipeline.placement, uses.strings);
    uses.strings.functionsInSourceOrder = sortFunctionsBySourcePosition(program, loops);

    uses.keys.byKey = WTF::move(shared.keys);
    collectNamesInTables(program, *pipeline.flow, *pipeline.placement, uses.keys);
    return uses;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
