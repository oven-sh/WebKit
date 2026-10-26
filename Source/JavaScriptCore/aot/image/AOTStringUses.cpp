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
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTOpcodesNamingRecords.h"
#include "AOTPipeline.h"
#include "AOTPlacement.h"
#include "AOTPropertyAccess.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRegExpRequests.h"
#include "AOTSchemaWalkers.h"
#include "AOTSlotPlanSteps.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "SmallStrings.h"
#include <algorithm>
#include <array>
#include <concepts>
#include <functional>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/FastBitVector.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT {

static_assert(FunctionID(0u) < FunctionID(), "a minimum over functions starts at the null id");
static_assert(ModuleID(0u) < ModuleID(), "a function without a module sorts last");

static constexpr Typed::OpcodeSet keyOrComparisonOpcodes {
    Typed::Opcode::StrictEq, Typed::Opcode::Eq, Typed::Opcode::ElemGet, Typed::Opcode::ElemSet, Typed::Opcode::ElemHas, Typed::Opcode::ElemDelete, Typed::Opcode::ElemDefine, Typed::Opcode::SuperGet,
    Typed::Opcode::SuperSet, Typed::Opcode::GetByValWithThis, Typed::Opcode::ToPropertyKey, Typed::Opcode::CallBuiltin, Typed::Opcode::CallInline,
};
static constexpr Typed::TerminatorKindSet keyOrComparisonTerminators { Typed::TerminatorKind::TryCallInline };

static constexpr bool usesOperandsAsKeys(Typed::Opcode opcode) { return keyOrComparisonOpcodes.contains(opcode); }
static constexpr bool usesOperandsAsKeys(Typed::TerminatorKind kind) { return keyOrComparisonTerminators.contains(kind); }

// The value of `list[i] = "text"` is only stored: nothing compares it or looks it up.
static constexpr bool isOnlyKept(Typed::OperandUses uses) { return !uses.isEmpty() && uses.containsOnly({ Typed::OperandUse::Retains, Typed::OperandUse::Returns }); }

template<typename Operation>
static constexpr bool looksUpKeysByName(Operation operation)
{
    return accessClassOf(operation) == Typed::AccessClass::ByName || familyOf(operation) == Typed::OperationFamily::FusedAccess;
}

static bool hasStringsForMessagesOnly(Typed::Opcode opcode)
{
    switch (opcode) {
    case Typed::Opcode::Call:
    case Typed::Opcode::CallList:
    case Typed::Opcode::CallMethod:
    case Typed::Opcode::CallResolved:
    case Typed::Opcode::Construct:
    case Typed::Opcode::ConstructList:
    case Typed::Opcode::CheckInitialized:
    case Typed::Opcode::RequireObjectCoercible:
    case Typed::Opcode::ToObject:
    case Typed::Opcode::NewRuntimeError:
    case Typed::Opcode::NewFunctionThatThrows:
    case Typed::Opcode::ThrowNewError:
        return true;
    default:
        return false;
    }
}

static bool hasStringsForMessagesOnly(Typed::TerminatorKind)
{
    return false;
}

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

static StringID impliedNameOf(const Typed::Program& program, const Typed::Value& value)
{
    if (familyOf(value.opcode) != Typed::OperationFamily::PrivateAccess)
        return { };
    PropertyKeyID key = keyOfNamedAccess(program, value);
    return key ? program.at(key).text : StringID();
}

template<typename Operation>
static bool takesKeyAsOperand(Operation operation)
{
    uint32_t numberOfDeclaredOperands = Typed::numberOfNamedOperandsOf(operation) + (Typed::hasVariadicOperand(operation) ? 1 : 0);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (Typed::operandKindOf(operation, operandIndex) == Typed::OperandKind::PropertyKey)
            return true;
    }
    return false;
}

template<typename Operation, typename Set>
static Set withOperationsTakingKeys(Set operations)
{
    for (Operation operation : allMembersOf<Operation>) {
        if (takesKeyAsOperand(operation) || Typed::familyOf(operation) == Typed::OperationFamily::PrivateAccess)
            operations.add(operation);
    }
    return operations;
}

namespace {

struct SharedUses {
    size_t numberOfStrings;
    Vector<KeyUse> keys;
    FastBitVector isNamed { numberOfStrings };
    FastBitVector isNamedBesideCreation { numberOfStrings };
    FastBitVector isConstant { numberOfStrings };
    FastBitVector isCreatedRepeatedly { numberOfStrings };
    FastBitVector isAtom { numberOfStrings };
};

struct FunctionWalk {
    bool runsAtMostOnce(Typed::BlockID block)
    {
        if (!functionRunsAtMostOnce)
            return false;
        if (!analyses)
            analyses.emplace(context, function);
        return analyses->executionCountOf(block) == Typed::ExecutionCount::AtMostOnce;
    }

    void nameString(StringID string)
    {
        if (!string || !namedStringSet.add(string).isNewEntry)
            return;
        if (hasCode)
            shared.isNamed.atomicSetAndCheck(string.index(), true);
        namedStrings.append(string.index());
    }

    void countKey(PropertyKeyID key, Typed::Access access, bool looksUpInTables)
    {
        KeyUse& use = shared.keys[key.index()];
        addRelaxed(use.uses);
        addRelaxed(use.usesByAccess[static_cast<size_t>(access)]);
        nameString(program.at(key).text);
        if (!looksUpInTables)
            return;
        addRelaxed(use.tableLookupUses);
        atomicMinRelaxed(use.firstTableLookupFunction, id);
    }

    void countCreationSite(StringID string, bool runsAtMostOnce)
    {
        bool isFirstSite = shared.isConstant.atomicSetAndCheck(string.index(), true);
        if (!isFirstSite || !runsAtMostOnce)
            shared.isCreatedRepeatedly.atomicSetAndCheck(string.index(), true);
    }

    template<typename InstructionType, typename Operation>
    void visit(const InstructionType& instruction, Operation operation, bool runsAtMostOnce)
    {
        bool createsStringConstant = runtimeOf(operation) == Typed::RuntimeOperation::StringConstant;
        bool looksUpInTables = !runsAtMostOnce && looksUpKeysByName(operation);
        forEachReference(instruction, [&]<typename Reference>(const Reference& reference) {
            if constexpr (std::same_as<Reference, StringID>) {
                nameString(reference);
                if (createsStringConstant)
                    countCreationSite(reference, runsAtMostOnce);
                else if (reference && !hasStringsForMessagesOnly(operation))
                    shared.isNamedBesideCreation.atomicSetAndCheck(reference.index(), true);
            } else if constexpr (std::same_as<Reference, PropertyKeyID>)
                countKey(reference, accessOf(operation), looksUpInTables);
        });
        bool usesEveryOperandAsKey = usesOperandsAsKeys(operation);
        instruction.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!usesEveryOperandAsKey && instruction.operandKind(operandIndex) != Typed::OperandKind::PropertyKey)
                return;
            if (isOnlyKept(instruction.operandUses(operandIndex)))
                return;
            const Typed::Value& definition = function.at(sameObjectThroughCasts(function, operand));
            if (definition.opcode == Typed::Opcode::String)
                shared.isAtom.atomicSetAndCheck(definition.string().index(), true);
        });
    }

    void visit(const Typed::Value& value, bool runsAtMostOnce)
    {
        visit(value, value.opcode, runsAtMostOnce);
        nameString(impliedNameOf(program, value));
    }

    Vector<uint32_t> run()
    {
        static const Typed::OpcodeSet opcodesWithUses = withOperationsTakingKeys<Typed::Opcode>(Typed::findOpcodesNaming<StringID, PropertyKeyID>() | keyOrComparisonOpcodes);
        static const Typed::TerminatorKindSet terminatorKindsWithUses = withOperationsTakingKeys<Typed::TerminatorKind>(Typed::findTerminatorKindsNaming<StringID, PropertyKeyID>() | keyOrComparisonTerminators);
        if (!mayEndBlockWith(program, id, terminatorKindsWithUses)) {
            Typed::BlockID current;
            bool currentRunsAtMostOnce = false;
            forEachListedValue(program, id, opcodesWithUses, [&](Typed::BlockID block, uint32_t, ValueID, const Typed::Value& value) {
                if (block != current) {
                    current = block;
                    currentRunsAtMostOnce = runsAtMostOnce(block);
                }
                visit(value, currentRunsAtMostOnce);
            });
            return WTF::move(namedStrings);
        }

        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            Typed::BlockID blockID(blockIndex);
            const Typed::Block& block = function.at(blockID);
            bool blockRunsAtMostOnce = runsAtMostOnce(blockID);
            for (ValueID valueID : block.values)
                visit(function.at(valueID), blockRunsAtMostOnce);
            visit(block.terminator, block.terminator.kind, blockRunsAtMostOnce);
        }
        return WTF::move(namedStrings);
    }

    const Typed::Program& program;
    FunctionID id;
    const Typed::Function& function;
    Typed::AnalysisContext context;
    bool hasCode;
    bool functionRunsAtMostOnce;
    SharedUses& shared;
    std::optional<Typed::FunctionAnalyses> analyses { };
    Vector<uint32_t> namedStrings { };
    UncheckedKeyHashSet<StringID> namedStringSet { };
};

} // anonymous namespace

static Vector<uint32_t> sortFunctionsBySourcePosition(const Typed::Program& program, const ParallelLoops& loops)
{
    using PositionAndIndex = std::array<uint32_t, 3>;
    Vector<PositionAndIndex> positionsAndIndices(program.functions.size(), [&](size_t index) {
        const Typed::Function& function = program.functions[index];
        return PositionAndIndex { function.module.index(), function.position, static_cast<uint32_t>(index) };
    });
    parallelSort(loops, positionsAndIndices.mutableSpan(), std::less<PositionAndIndex> { });
    return Vector<uint32_t>(program.functions.size(), [&](size_t index) {
        return positionsAndIndices[index][2];
    });
}

static void collectNamesInTables(const Typed::Program& program, const Typed::FlowResults& flow, const Typed::Placement& placement, KeyUses& keys)
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

    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        const Typed::Layout& layout = program.layouts[index];
        if (layout.isEnvironment())
            continue;
        // The tables describe the classes and the layouts that have a shape, where they are in use: see encodeLayoutRecords().
        if (!placement.isInUse(Typed::LayoutID(index)))
            continue;
        if (layout.kind != Typed::LayoutKind::Instance && placement.shapes().shapesOf(Typed::LayoutID(index)).isEmpty())
            continue;
        // The record of a compiled object lists the fields that have storage. A parse by type finds every declared member by its name, and a validating build the fields it took away.
        bool listsFieldsWithStorageOnly = placement.slots().storageOf(Typed::LayoutID(index)) == Typed::LayoutStorage::CompiledObject && !layout.flow.observedUses.contains(Typed::ObservedUse::IsParsedByType);
        for (Typed::FieldIndex field = 0; field < layout.fields.size(); ++field) {
            if (!listsFieldsWithStorageOnly || placement.slot(Typed::LayoutID(index), field).indexInLayoutRecord || layout.fields[field].flags.contains(Typed::FieldFlags::IsRemovedAsUnread))
                nameKey(layout.fields[field].key);
        }
        for (const Typed::StaticField& field : layout.staticFields)
            nameKey(field.key);
        for (const Typed::ClassMember& member : layout.classFlow.members)
            nameKey(member.key);
    }
    for (size_t type : placement.typesWithCells()) {
        for (PropertyKeyID symbol : program.at(Typed::TypeID(type)).symbolLiterals)
            nameKey(symbol);
    }
    for (const Typed::PlacedPropertyRun& run : placement.propertyRuns().runs()) {
        for (PropertyKeyID key : run.keys)
            nameKey(key);
    }
    for (const Typed::PlacedGuardBit& guardBit : placement.guardBits().bits)
        nameKey(guardBit.key);
    for (PropertyKeyID key : placement.keyCells())
        nameKey(key);
    for (const Typed::RequestRecord& record : placement.requests().records()) {
        Typed::DataID data = WTF::switchOn(record,
            [](const Typed::DataRecord& request) { return request.data; },
            [](const Typed::CollectionDataRecord& request) { return request.data; },
            [](const auto&) { return Typed::DataID(); });
        if (!data)
            continue;
        forEachDataNodeInPreorder(program.at(data).root, [&](uint32_t, const Typed::DataNode& node) {
            const Typed::DataNode::Object* object = node.tryAs<Typed::DataNode::Object>();
            if (!object)
                return;
            const Typed::Layout* layout = object->layout ? &program.at(object->layout) : nullptr;
            for (const Typed::DataMember& member : object->members) {
                if (!layout || !layout->findField(member.key))
                    nameKey(member.key);
            }
        });
    }

    for (unsigned index = 0; index < program.modules.size(); ++index) {
        const Typed::Module& module = program.modules[index];
        for (const Typed::ModuleExport& moduleExport : module.exports) {
            if (!isDescribedToRunTime(program, flow, ModuleID(index), moduleExport))
                continue;
            nameText(moduleExport.name);
            nameText(moduleExport.importName);
        }
        for (const Typed::SyntheticExport& syntheticExport : module.syntheticExports)
            nameText(syntheticExport.name);
        for (const Typed::RequiredName& requiredName : module.requiredNames)
            nameText(requiredName.name);
    }
    for (const Typed::EvalScope& scope : program.evalScopes) {
        for (const Typed::EvalBinding& binding : scope.bindings)
            nameText(binding.name);
    }
}

ProgramUses collectProgramUses(const Typed::PipelineOutput& pipeline, const BitVector& bodiesWithoutCode, BuildContext& context)
{
    const Typed::Program& program = *pipeline.program;
    const ParallelLoops& loops = context.parallelLoops();
    unsigned numberOfFunctions = program.functions.size();

    SharedUses shared { program.strings.size(), Vector<KeyUse>(program.types.numberOfPropertyKeys()) };
    ProgramUses uses;
    StringUses& strings = uses.strings;
    strings.namedByFunction.grow(numberOfFunctions);
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        FunctionID id(index);
        if (program.at(id).hasBody())
            strings.namedByFunction[index] = FunctionWalk { program, id, program.at(id), { context, program, pipeline.flow.get(), nullptr, id }, !bodiesWithoutCode.get(index), pipeline.flow->runsAtMostOnce(program, id), shared }.run();
    });

    strings.isNamed = toBitVector(shared.isNamed);
    strings.isNamedBesideCreation = toBitVector(shared.isNamedBesideCreation);
    strings.isConstant = toBitVector(shared.isConstant);
    strings.isCreatedRepeatedly = toBitVector(shared.isCreatedRepeatedly);
    strings.isAtom = toBitVector(shared.isAtom);
    auto nameBesideCreation = [&](StringID string) {
        strings.isNamed.quickSet(string.index());
        strings.isNamedBesideCreation.quickSet(string.index());
    };
    pipeline.regExpRequests->forEachString(nameBesideCreation);
    for (const RegExpRequest& request : pipeline.regExpRequests->all()) {
        for (StringID name : request.captureGroupNames) {
            if (name)
                strings.isAtom.quickSet(name.index());
        }
        for (const RegExpNamedGroupStrings& group : request.namedGroups)
            strings.isAtom.quickSet(group.name.index());
    }
    for (StringID string : pipeline.literalStrings->stringsWithoutWord())
        nameBesideCreation(string);
    strings.isConstantInTables = pipeline.placement->stringConstantsInTables();
    strings.isNamed.merge(strings.isConstantInTables);
    strings.isConstant.merge(strings.isConstantInTables);
    strings.isCreatedRepeatedly.merge(strings.isConstantInTables);
    strings.functionsInSourceOrder = sortFunctionsBySourcePosition(program, loops);

    uses.keys.byKey = WTF::move(shared.keys);
    collectNamesInTables(program, *pipeline.flow, *pipeline.placement, uses.keys);
    return uses;
}

static_assert(!RowDisplacementTable::rowMarkerColumn);
static_assert(defaultLimitValue(LimitID::LookupTableNames, hostArchitecture()) == RowDisplacementTable::maxColumn);
static_assert(StringID(0u) < StringID(), "a symbol without a description sorts last");
static_assert(PropertyKeyID(0u) < PropertyKeyID(), "a name without a key sorts last");

namespace {

struct NameCandidate {
    NumberedIdentifier identifier;
    bool hasPromisedColumn { false };
    bool isEngineSymbol { false };
    uint32_t uses { 0 };
    uint32_t tableLookupUses { 0 };
    FunctionID firstTableLookupFunction { };
};

} // anonymous namespace

static bool isFoundByText(const Typed::Program& program, const NumberedIdentifier& identifier)
{
    return !identifier.key || program.at(identifier.key).kind == PropertyKeyKind::Text;
}

// The run time creates the identifiers of all of these in one step, before it looks up the first uid.
static bool isEngineSymbol(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::EnginePrivateName:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<NameCandidate> collectNumberedNames(const Typed::Program& program, const KeyUses& uses, const BitVector& keysWithColumn)
{
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    RELEASE_ASSERT(uses.byKey.size() == numberOfKeys);

    Vector<NameCandidate> names;
    BitVector isTextOfNumberedKey(program.strings.size());
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        PropertyKeyID key(index);
        const KeyUse& use = uses.byKey[index];
        if (!use.uses && !uses.keysNamedByTables.get(index) && !program.builtinNameOf(key))
            continue;
        NumberedIdentifier identifier { key, program.at(key).text };
        if (isFoundByText(program, identifier))
            isTextOfNumberedKey.quickSet(identifier.text.index());
        names.append(NameCandidate {
            .identifier = identifier,
            .hasPromisedColumn = keysWithColumn.get(index),
            .isEngineSymbol = isEngineSymbol(program.at(key).kind),
            .uses = use.uses,
            .tableLookupUses = use.tableLookupUses,
            .firstTableLookupFunction = use.firstTableLookupFunction,
        });
    }
    for (size_t index : uses.stringsNamedAsKeysByTables) {
        if (!isTextOfNumberedKey.get(index))
            names.append(NameCandidate { .identifier = { PropertyKeyID(), StringID(index) } });
    }
    return names;
}

static void sortByUse(const ParallelLoops& loops, std::span<NameCandidate> names)
{
    parallelSort(loops, names, [](const NameCandidate& a, const NameCandidate& b) {
        if (a.hasPromisedColumn != b.hasPromisedColumn)
            return a.hasPromisedColumn;
        if (a.tableLookupUses != b.tableLookupUses)
            return a.tableLookupUses > b.tableLookupUses;
        if (a.uses != b.uses)
            return a.uses > b.uses;
        if (a.identifier.key != b.identifier.key)
            return a.identifier.key < b.identifier.key;
        return a.identifier.text < b.identifier.text;
    });
}

static void groupColumnsByFunction(std::span<NameCandidate> namesWithColumn)
{
    constexpr size_t hotNameCount = RowDisplacementTable::numberOfHotNames - 1;
    if (namesWithColumn.size() <= hotNameCount)
        return;
    auto remainingNamesWithColumn = namesWithColumn.subspan(hotNameCount);
    AOT::stableSort(remainingNamesWithColumn, [](const NameCandidate& a, const NameCandidate& b) {
        return a.firstTableLookupFunction < b.firstTableLookupFunction;
    });
}

static void reportNamesWithoutColumn(std::span<const NameCandidate> namesWithoutColumn, BuildContext& context)
{
    auto isLookedUp = [](const NameCandidate& name) {
        return name.tableLookupUses > 0;
    };
    auto mostLookedUp = std::ranges::find_if(namesWithoutColumn, isLookedUp);
    if (mostLookedUp == namesWithoutColumn.end())
        return;
    auto lookedUpCount = static_cast<uint64_t>(std::ranges::count_if(namesWithoutColumn, isLookedUp));
    context.gaveUp(LimitID::LookupTableNames, { .function = mostLookedUp->firstTableLookupFunction });
    context.statistics().count(hitsOf(LimitID::LookupTableNames), lookedUpCount - 1);
}

static void sortByFirstMention(const ParallelLoops& loops, std::span<NameCandidate> namesWithoutColumn)
{
    parallelSort(loops, namesWithoutColumn, [](const NameCandidate& a, const NameCandidate& b) {
        if (a.isEngineSymbol != b.isEngineSymbol)
            return a.isEngineSymbol;
        if (a.identifier.text != b.identifier.text)
            return a.identifier.text < b.identifier.text;
        return a.identifier.key < b.identifier.key;
    });
}

IdentifierNumbering numberIdentifiers(const Typed::Program& program, const KeyUses& uses, const BitVector& keysWithColumn, BuildContext& context)
{
    Vector<NameCandidate> names = collectNumberedNames(program, uses, keysWithColumn);
    sortByUse(context.parallelLoops(), names.mutableSpan());
    auto columnCount = static_cast<uint32_t>(std::min<uint64_t>(names.size(), context.limit(LimitID::LookupTableNames)));
    groupColumnsByFunction(names.mutableSpan().first(columnCount));
    reportNamesWithoutColumn(names.span().subspan(columnCount), context);
    sortByFirstMention(context.parallelLoops(), names.mutableSpan().subspan(columnCount));

    IdentifierNumbering numbering;
    numbering.m_numbersByKey.fill(0, program.types.numberOfPropertyKeys());
    numbering.m_numbersByText.fill(0, program.strings.size());
    numbering.m_identifiers.grow(names.size() + 1);
    for (uint32_t index = 0; index < names.size(); ++index) {
        const NumberedIdentifier& identifier = names[index].identifier;
        uint32_t number = RowDisplacementTable::numberForRank(index + 1, columnCount);
        numbering.m_identifiers[number] = identifier;
        if (identifier.key)
            numbering.m_numbersByKey[identifier.key.index()] = number;
        if (isFoundByText(program, identifier))
            numbering.m_numbersByText[identifier.text.index()] = number;
    }
    numbering.m_countWithLookupTableColumn = columnCount + 1;

    context.statistics().count(StatisticID::IdentifiersNumbered, names.size());
    if (context.exceeds(LimitID::MaxIdentifiers, numbering.count()))
        context.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::TooManyIdentifiers, .limit = LimitID::MaxIdentifiers, .count = numbering.count() });
    return numbering;
}

static bool hasCellInVM(std::span<const uint8_t> text)
{
    constexpr size_t maxEncodedSizeOfSingleCharacterString = 2;
    if (text.size() > maxEncodedSizeOfSingleCharacterString)
        return false;
    Vector<char16_t> units = Typed::decodeProgramString(text);
    return units.isEmpty() || (units.size() == 1 && units[0] <= maxSingleCharacterString);
}

StringConstantNumbering numberStringConstants(const Typed::Program& program, const StringUses& uses, BuildContext& context)
{
    BitVector needsNumber(program.strings.size());
    needsNumber.merge(uses.isConstantInTables);
    for (size_t string : uses.isCreatedRepeatedly) {
        if (uses.isConstant.get(string) && !hasCellInVM(program.strings[StringID(string)]))
            needsNumber.quickSet(string);
    }

    StringConstantNumbering numbering;
    numbering.m_numbersPlusOne.fill(0, program.strings.size());
    numbering.m_strings.reserveInitialCapacity(needsNumber.bitCount());
    auto numberIfNeeded = [&](size_t string) {
        if (!needsNumber.quickGet(string) || numbering.m_numbersPlusOne[string])
            return;
        numbering.m_strings.append(StringID(string));
        numbering.m_numbersPlusOne[string] = numbering.count();
    };
    for (uint32_t function : uses.functionsInSourceOrder) {
        for (uint32_t string : uses.namedByFunction[function])
            numberIfNeeded(string);
    }
    for (size_t string : needsNumber)
        numberIfNeeded(string);

    context.statistics().count(StatisticID::StringConstantsNumbered, numbering.count());
    return numbering;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
