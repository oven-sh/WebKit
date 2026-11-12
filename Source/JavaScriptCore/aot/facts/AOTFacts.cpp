/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFacts.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTCompileOptions.h"
#include "AOTEnforcedFields.h"
#include "AOTFactFaults.h"
#include "AOTIRProgram.h"
#include "AOTLinkCoroutines.h"
#include "AOTLinkFlow.h"
#include "AOTLinkStrings.h"
#include "AOTPhaseTimes.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <numeric>
#include <tuple>
#include <wtf/threads/BinarySemaphore.h>

namespace JSC { namespace AOT { namespace Typed {

bool hasLockableInstances(const Program& program, const LinkTables& linked, TypeID testedType)
{
    std::span<const LayoutID> layouts = program.types.listOf(program.at(testedType).layouts);
    return layouts.empty() || std::ranges::any_of(layouts, [&](LayoutID id) {
        return linked.layout(id).hasLockableInstances;
    });
}

ASCIILiteral nameOf(FactKind kind)
{
    switch (kind) {
    case FactKind::NarrowedTypes:
        return "NarrowedTypes"_s;
    case FactKind::ProvenContents:
        return "ProvenContents"_s;
    case FactKind::OperandSummaries:
        return "OperandSummaries"_s;
    case FactKind::HeapWords:
        return "HeapWords"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

RefusedCode refusedCodeOf(const Program& program)
{
    return {
        .evaluatedText = hasFlag(program.excludedCode, ExcludedCode::EvaluatedText),
        .hostEvaluatedText = hasFlag(program.excludedCode, ExcludedCode::HostEvaluatedText),
        .codeOutsideImage = !program.admitsInspector && (isHardenedBuild || hasFlag(program.excludedCode, ExcludedCode::CodeOutsideImage)),
    };
}

bool refusesTextAsUnseenCode(const BuiltinTable& builtins, BuiltinID canonical, RefusedCode refused)
{
    const Builtin& entry = builtins.at(canonical);
    if (entry.origin == BuiltinOrigin::Engine)
        return refused.evaluatedText && entry.flags.contains(BuiltinFlag::EvaluatesText);
    return refused.hostEvaluatedText && (entry.flags.contains(BuiltinFlag::EvaluatesText) || builtins.has(BuiltinMark::EvaluatesTextInGlobalScope, canonical));
}

std::optional<UnseenCodeCause> eventOfCalling(const BuiltinTable& builtins, BuiltinID builtin, RefusedCode refused)
{
    builtin = builtins.canonicalID(builtin);
    OptionSet<BuiltinFlag> flags = builtins.at(builtin).flags;
    if (refusesTextAsUnseenCode(builtins, builtin, refused))
        return std::nullopt;
    bool loadsFromImageAlone = refused.codeOutsideImage || builtin == Builtins::ProcessGetBuiltinModule;
    if (flags.contains(BuiltinFlag::EvaluatesText) && (loadsFromImageAlone || !flags.contains(BuiltinFlag::LoadsModuleByName)))
        return UnseenCodeCause::EvaluatesText;
    if (flags.contains(BuiltinFlag::LoadsModuleByName))
        return loadsFromImageAlone ? UnseenCodeCause::LoadsModuleByName : UnseenCodeCause::LoadsCodeByName;

    const BuiltinUnseenCode* row = builtins.unseenCodeOf(builtin);
    if (row && row->condition && row->condition->hasProperty)
        return std::nullopt;
    if (row ? !row->condition || !row->condition->buildAdmitsRuntimeCode : flags.contains(BuiltinFlag::RunsUnseenCode))
        return UnseenCodeCause::RunsUnseenCode;
    return std::nullopt;
}

bool raisesEventWhenCalled(const BuiltinTable& builtins, BuiltinID builtin)
{
    builtin = builtins.canonicalID(builtin);
    if (eventOfCalling(builtins, builtin, { }))
        return true;
    return std::ranges::any_of(builtins.loadersOfMachineCode(), [&](const BuiltinLoaderOfMachineCode& loader) {
        return builtins.canonicalID(loader.callee) == builtin;
    });
}

bool isMadeWithCodeOfLibrary(const BuiltinTable& builtins, BuiltinID member)
{
    member = builtins.canonicalID(member);
    const Builtin& entry = builtins.at(member);
    if (entry.origin != BuiltinOrigin::Host)
        return false;
    if (!builtins.calledFunctionsOf(member).empty() || !builtins.moduleLoadsOf(member).empty())
        return true;
    return !isObjectKind(entry.kind) && std::ranges::any_of(builtins.overloadsOf(member), [](const BuiltinOverload& overload) {
        return !!overload.code.implementation.function;
    });
}

static void endStableNumbersOfKeysForFacts(Program& program, const ParallelLoops& loops)
{
    program.computeLocationBits(loops);
    program.types.endStableNumbersOfPropertyKeys(loops);
    program.types.addKeysOfTexts(static_cast<uint32_t>(program.strings.size()), loops);
}

static std::unique_ptr<BodyIndexTable> indexBodiesForFacts(const Program& program, const ParallelLoops& loops, std::unique_ptr<BodyIndexTable>&& indexesOfVerifier)
{
    auto numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    std::unique_ptr<BodyIndexTable> bodies = WTF::move(indexesOfVerifier);
    if (!bodies)
        bodies = makeUniqueWithoutFastMallocCheck<BodyIndexTable>(numberOfFunctions, loops.width());
    else
        bodies->addFunctionsWithoutBodies(numberOfFunctions);
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index, unsigned worker) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (function.hasBody() && !bodies->has(id))
            bodies->build(id, function, worker);
    });
    return bodies;
}

static BuiltinID builtinWrittenAsHeritageForFacts(TypeReader types, BodyValue heritage)
{
    Opcode opcode = heritage.opcode();
    if (opcode == Opcode::Builtin)
        return heritage.as<Opcode::Builtin>().builtin();
    if (opcode == Opcode::ModuleBuiltin)
        return heritage.as<Opcode::ModuleBuiltin>().builtin();
    if (opcode != Opcode::Convert)
        return { };
    TypeID written = heritage.as<Opcode::Convert>().declared().written();
    return written ? singleBuiltinOf(types, types.at(written)) : BuiltinID();
}

static BuiltinClassID builtinClassPassedToMixin(const Program& program, TypeReader types, const BodyIndex& index, ValueID heritage)
{
    if (!heritage || index.isParameter(heritage) || index.opcodeOf(heritage) != Opcode::Call)
        return { };
    for (ValueID argument : index.value(heritage).as<Opcode::Call>().argumentsOperands()) {
        if (!argument || index.isParameter(argument))
            continue;
        BuiltinID builtin = builtinWrittenAsHeritageForFacts(types, index.value(argument));
        if (BuiltinClassID builtinClass = builtin ? classMadeByHeritage(program.builtins(), builtin) : BuiltinClassID(); builtinClass == BuiltinClasses::Array)
            return builtinClass;
    }
    return { };
}

static BuiltinClassID builtinClassOfMixin(const Program& program, TypeReader types, const BodyIndex& index, ValueID heritage)
{
    heritage = index.stripCasts(heritage);
    if (!index.isParameter(heritage))
        return builtinClassPassedToMixin(program, types, index, heritage);
    BlockID joined = index.blockOf(heritage);
    uint32_t place = heritage.index() - index.block(joined).firstParameter().index();
    for (uint32_t from = 0; from < index.numberOfBlocks(); ++from) {
        for (BodyTarget target : index.terminator(BlockID(from)).targets()) {
            if (target.block() != joined || place >= target.arguments().size() || !target.arguments()[place])
                continue;
            if (BuiltinClassID builtinClass = builtinClassPassedToMixin(program, types, index, index.stripCasts(target.arguments()[place])))
                return builtinClass;
        }
    }
    return { };
}

static void noteSubclassesForFacts(Program& program, BodyIndexTable& bodies)
{
    Vector<ClassBelowBuiltinClass> classes;
    Vector<ClassBelowBuiltinClass> mixedSubclasses;
    TypeReader types(program);
    for (const EvaluationOfClass& evaluation : bodies.takeEvaluationsOfClasses()) {
        const BodyIndex& index = *evaluation.index;
        BodyOperation<Opcode::NewClass> newClass = index.value(evaluation.value).as<Opcode::NewClass>();
        ValueID heritage = newClass.heritageOperand();
        if (!heritage)
            continue;
        bool isBelowWrittenClass = !!writtenBaseOf(program.at(newClass.layout()));
        BuiltinClassID builtinClass;
        if (isBelowWrittenClass)
            builtinClass = builtinClassOfMixin(program, types, index, heritage);
        else if (BuiltinID builtin = builtinWrittenAsHeritageForFacts(types, index.value(heritage)))
            builtinClass = classMadeByHeritage(program.builtins(), builtin);
        if (builtinClass)
            (isBelowWrittenClass ? mixedSubclasses : classes).append(ClassBelowBuiltinClass { newClass.layout(), builtinClass });
    }
    AOT::sort(classes);
    removeRepeatedElements(classes);
    AOT::sort(mixedSubclasses);
    removeRepeatedElements(mixedSubclasses);
    noteSubclassesOfBuiltinClasses(program, classes.span());
    noteSubclassesOfMixins(program, mixedSubclasses.span());
}

static void updateProgramBehindIndexesForFacts(Program& program, BodyIndexTable& bodies)
{
    program.setConstructionStarts(bodies.takeConstructionStarts());
    noteSubclassesForFacts(program, bodies);
}

static void fillFunctionTablesForFacts(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, FactTables& tables)
{
    size_t numberOfFunctions = program.functions.size();
    RELEASE_ASSERT(linked.functions.size() == numberOfFunctions && summaries.size() == numberOfFunctions);

    for (uint32_t layout = 0; layout < program.layouts.size(); ++layout) {
        if (program.layouts[layout].kind != LayoutKind::ModuleNamespace)
            continue;
        if (!tables.endOfLayoutsOfNamespaces)
            tables.firstLayoutOfNamespace = layout;
        tables.endOfLayoutsOfNamespaces = layout + 1;
    }

    tables.summaries.reserveInitialCapacity(numberOfFunctions);

    tables.functionsNeverReached.ensureSize(numberOfFunctions);
    tables.entryParameters.firstRowOfOwner.reserveInitialCapacity(numberOfFunctions + 1);

    for (size_t index = 0; index < numberOfFunctions; ++index) {
        FunctionID id(static_cast<uint32_t>(index));
        const Function& function = program.at(id);
        const LinkedFunction& link = linked.function(id);
        const ThinSummary& summary = summaries.at(id);

        tables.summaries.append(function.hasBody() ? Summary { summary.numberOfOperations, !summary.has(SummaryTrait::HasLoops) } : Summary { });

        if (!link.isReached)
            tables.functionsNeverReached.quickSet(index);

        Vector<PositionFacts>& rows = tables.entryParameters.rows;
        tables.entryParameters.firstRowOfOwner.append(static_cast<uint32_t>(rows.size()));
        std::span<const LinkedParameter> parameters = linked.parametersOf(id);
        RELEASE_ASSERT(parameters.size() == function.numberOfEntryParameters());
        if (!link.everyCallerIsKnown)
            continue;
        for (const LinkedParameter& parameter : parameters)
            rows.append(PositionFacts { parameter.proven });
    }
    tables.entryParameters.firstRowOfOwner.append(static_cast<uint32_t>(tables.entryParameters.rows.size()));
}

static void layKeysThatHeldAloneAppendsOver(const Program& program, LinkTables& linked, const LinkTables& held, size_t numberOfLayouts)
{
    if (held.appendedKeys.isEmpty())
        return;
    linked.layoutsFillingLateFieldsTogether.merge(held.layoutsFillingLateFieldsTogether);
    if (linked.inlineCapacities.isEmpty())
        linked.inlineCapacities = held.inlineCapacities;
    linked.initialNames.appendVector(held.initialNames);
    linked.computedLiterals.merge(held.computedLiterals);
    linked.sortInitialNames();
    linked.lateFieldsStoredThroughThis.appendVector(held.lateFieldsStoredThroughThis);
    std::ranges::sort(linked.lateFieldsStoredThroughThis);
    linked.lateFieldsStoredThroughThis.shrink(linked.lateFieldsStoredThroughThis.size() - std::ranges::unique(linked.lateFieldsStoredThroughThis).size());
    linked.firstKeyAppendedByHeldAloneOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    for (size_t index = 0; index < numberOfLayouts; ++index) {
        linked.firstKeyAppendedByHeldAloneOfLayout.append(static_cast<uint32_t>(linked.keysAppendedByHeldAlone.size()));
        LayoutID id(static_cast<uint32_t>(index));
        std::span<const PropertyKeyID> proven = linked.appendedKeysOf(id);
        size_t most = maxAppendedFieldsOf(program.layouts[index].fields.size());
        size_t room = most - std::min<size_t>(most, proven.size());
        std::span<const PropertyKeyID> keysOfHeld = held.appendedKeysOf(id);
        for (size_t place = 0; place < keysOfHeld.size(); ++place) {
            PropertyKeyID key = keysOfHeld[place];
            bool isStoredByFollowerOfMaker = held.appendedFieldsThatFollowersOfMakersStore.get(held.firstAppendedFieldOfLayout[index] + place);
            bool isLaidLate = held.appendedFieldsLaidLate.get(held.firstAppendedFieldOfLayout[index] + place);
            if (auto found = std::ranges::find(proven, key); found != proven.end()) {
                if (isLaidLate)
                    linked.appendedFieldsLaidLate.set(linked.firstAppendedFieldOfLayout[index] + (found - proven.begin()));
                if (isStoredByFollowerOfMaker)
                    linked.appendedFieldsThatFollowersOfMakersStore.set(linked.firstAppendedFieldOfLayout[index] + (found - proven.begin()));
                continue;
            }
            if (!room)
                continue;
            if (held.appendedFieldsThatTypeOfThisRequires.get(held.firstAppendedFieldOfLayout[index] + place))
                linked.keysAppendedByHeldAloneThatLieInCell.set(linked.keysAppendedByHeldAlone.size());
            if (isLaidLate)
                linked.keysAppendedByHeldAloneLaidLate.set(linked.keysAppendedByHeldAlone.size());
            if (isStoredByFollowerOfMaker)
                linked.keysAppendedByHeldAloneThatFollowersOfMakersStore.set(linked.keysAppendedByHeldAlone.size());
            linked.keysAppendedByHeldAlone.append(key);
            --room;
        }
    }
    linked.firstKeyAppendedByHeldAloneOfLayout.append(static_cast<uint32_t>(linked.keysAppendedByHeldAlone.size()));
}

static void fillFieldTablesForFacts(const Program& program, const LinkTables& linked, Mechanisms mechanisms, FactTables& tables)
{
    size_t numberOfLayouts = program.layouts.size();
    RELEASE_ASSERT(linked.layouts.size() == numberOfLayouts);

    bool holdsAccessorsInSlots = withoutMechanismsMissingDependencies(mechanisms).contains(Mechanism::TypedSlots);
    tables.firstFieldOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    for (size_t index = 0; index < numberOfLayouts; ++index) {
        tables.firstFieldOfLayout.append(static_cast<uint32_t>(tables.fields.size()));
        LayoutID id(static_cast<uint32_t>(index));
        const LinkedLayout& layout = linked.layouts[index];
        uint32_t numberDeclared = static_cast<uint32_t>(program.layouts[index].fields.size());
        uint32_t numberOfFields = numberDeclared + static_cast<uint32_t>(linked.appendedKeysOf(id).size());
        LayoutID base = layout.takesCellsOfBase ? writtenBaseOf(program.layouts[index]) : LayoutID();
        auto whatLayoutSaysOfRows = [](const LinkedLayout& layout) {
            return std::tuple { layout.possibleChanges.contains(LayoutChange::LosesProperty), layout.possibleChanges.contains(LayoutChange::GetsAccessor), layout.reachesUnseenCode, layout.isRedefinedByLiteralsAlone };
        };
        bool takesRowsOfBase = base && base.index() < index && numberDeclared > program.layouts[index].ownFieldCount && whatLayoutSaysOfRows(layout) == whatLayoutSaysOfRows(linked.layouts[base.index()]);
        if (takesRowsOfBase)
            tables.layoutsThatTakeRowsOfBase.set(index);
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            if (takesRowsOfBase && field == program.layouts[index].ownFieldCount) {
                size_t numberTaken = numberDeclared - field;
                size_t first = tables.fields.size();
                tables.fields.grow(first + numberTaken);
                std::ranges::copy(tables.fields.span().subspan(tables.firstFieldOfLayout[base.index()], numberTaken), tables.fields.mutableSpan().subspan(first).begin());
                field = numberDeclared - 1;
                continue;
            }
            FieldFacts facts;
            if (const LinkedCell* cell = field >= numberDeclared && holdsAccessorsInSlots ? linked.findCellOfField({ id, field }) : nullptr) {
                facts.formOfValue = cell->formThatDefinitionsCreate;
                if (facts.formOfValue.contains(FieldForm::IsAccessor))
                    facts.facts.add(FieldFact::IsAccessor);
            }
            if (const LinkedCell* cell = holdsAccessorsInSlots ? linked.findCellOfField({ id, field }) : nullptr; cell && cell->isFilledByAccessorsAlone) {
                facts.facts.add(FieldFact::IsAccessor);
                if (field < numberDeclared)
                    facts.formOfValue = cell->formThatDefinitionsCreate;
            } else if (cell && field < numberDeclared && !cell->formThatDefinitionsCreate.contains(FieldForm::IsAccessor))
                facts.formOfValue = cell->formThatDefinitionsCreate;
            RELEASE_ASSERT(!facts.formOfValue.contains(FieldForm::IsAccessor) || facts.facts.contains(FieldFact::IsAccessor));
            if (!layout.possibleChanges.contains(LayoutChange::LosesProperty))
                facts.stability.add(FieldStability::IsNeverDeleted);
            if (!layout.reachesUnseenCode) {
                bool isDeclaredPlain = field >= numberDeclared ? facts.formOfValue.isEmpty() : fieldAt(program, declarationOf(program, { id, field })).form.isEmpty();
                const LinkedCell* cell = layout.isRedefinedByLiteralsAlone ? linked.findCellOfField({ id, field }) : nullptr;
                const LinkedCell* own = cell ? cell : linked.findCellOfField({ id, field });
                if (isDeclaredPlain && (!layout.possibleChanges.contains(LayoutChange::GetsAccessor) || (cell && cell->isFilled) || (own && own->liesBesideWhatDescriptorsDefine)))
                    facts.stability.add(FieldStability::IsNeverRedefined);
            }
            tables.fields.append(facts);
        }
    }
    tables.firstFieldOfLayout.append(static_cast<uint32_t>(tables.fields.size()));
}

static bool mayHoldDictionariesForFacts(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    if (layout.kind != LayoutKind::Record || layout.name || layout.constructor || !layout.fields.isEmpty() || !layout.staticFields.isEmpty() || layout.computedKeyCount)
        return false;
    if (layout.base || layout.writtenBuiltinBase || !layout.supertypes.isEmpty() || !program.layoutsExpectedBelow(id).empty())
        return false;
    return layout.flags.isEmpty() || layout.flags == OptionSet<LayoutFlags> { LayoutFlags::IsWrittenUnderDictionaryType };
}

static void fillLayoutTablesForFacts(const Program& program, const LinkTables& linked, const LinkTables& held, FactTables& tables)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    BitVector holdsDictionaries(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        if (!linked.layouts[index].isFilledUnderComputedKeys && !held.layouts[index].isFilledUnderComputedKeys)
            continue;
        if (mayHoldDictionariesForFacts(program, id) && linked.appendedKeysOf(id).empty() && held.appendedKeysOf(id).empty())
            holdsDictionaries.quickSet(index);
    }
    if (holdsDictionaries.isEmpty())
        return;

    for (const Layout& layout : program.layouts) {
        for (LayoutID supertype : layout.supertypes)
            holdsDictionaries.quickClear(supertype.index());
    }
    for (const ExpectedResult& expected : program.expectedResults) {
        for (LayoutID layout : expected.layouts)
            holdsDictionaries.quickClear(layout.index());
    }
    BitVector isDataOfModule(program.data.size());
    for (const Module& module : program.modules) {
        if (module.data)
            isDataOfModule.quickSet(module.data.index());
    }
    Vector<const DataNode*, 32> nodesToVisit;
    for (uint32_t index = 0; index < program.data.size(); ++index) {
        nodesToVisit.append(&program.data[index].root);
        while (!nodesToVisit.isEmpty()) {
            const DataNode& node = *nodesToVisit.takeLast();
            if (const auto* array = node.tryAs<DataNode::Array>()) {
                for (const DataNode& element : array->elements)
                    nodesToVisit.append(&element);
            } else if (const auto* object = node.tryAs<DataNode::Object>()) {
                if (object->layout && (!object->members.isEmpty() || isDataOfModule.quickGet(index)))
                    holdsDictionaries.quickClear(object->layout.index());
                for (const DataMember& member : object->members)
                    nodesToVisit.append(&member.value);
            }
        }
    }
    const TypeTable& types = program.types;
    Vector<uint64_t> layoutsNamedByTypes;
    layoutsNamedByTypes.fill(0, (numberOfLayouts + 63) / 64);
    BitVector typesLookedAt(types.numberOfTypes());
    HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> setsLookedAt;
    for (TypeID type : types.typesOfWireIndexes()) {
        if (!type || holdsOnlyPrimitives(type) || typesLookedAt.quickSet(TypeTable::positionInProgramOf(type)))
            continue;
        LayoutSetID named = types.typeKeptAt(type).unnamedLayouts;
        if (!named)
            continue;
        if (holdsOneMember(named)) {
            layoutsNamedByTypes[positionOf(named) / 64] |= 1ull << (positionOf(named) % 64);
            continue;
        }
        if (!setsLookedAt.add(named.bits()).isNewEntry)
            continue;
        types.at(named).untyped().anyWordOf([&](uint32_t word, uint64_t members) {
            layoutsNamedByTypes[word] |= members;
            return false;
        });
    }

    tables.layouts.fill(LayoutFacts { }, numberOfLayouts);
    for (size_t index : holdsDictionaries) {
        if (!(layoutsNamedByTypes[index / 64] >> (index % 64) & 1))
            tables.layouts[index].facts.add(LayoutFact::ObjectsAreDictionaries);
    }
}

static void fillVariableTablesForFacts(const LinkTables& linked, FactTables& tables)
{
    tables.variables.fill(VariableFacts { }, linked.variables.size());
    for (size_t index = 0; index < linked.variables.size(); ++index) {
        if (linked.variables[index].isFilledAtEveryRead)
            tables.variables[index].location.initialization = InitializationState::StoredBeforeEveryRead;
    }
}

static void fillFunctionsGivenToVariablesForFacts(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, FactTables& tables)
{
    if (summaries.size() != program.functions.size() || linked.functions.size() != program.functions.size())
        return;
    struct Given {
        FunctionID function;
        bool areSeveral { false };
    };
    using GivenByVariable = HashMap<uint32_t, Given, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    GivenByVariable givenTo;
    auto noteIn = [](GivenByVariable& table, uint32_t variable, FunctionID function) {
        Given& given = table.add(variable, Given { }).iterator->value;
        if (!given.function)
            given.function = function;
        else if (given.function != function)
            given.areSeveral = true;
    };
    auto note = [&](uint32_t variable, FunctionID function) {
        noteIn(givenTo, variable, function);
    };
    auto functionAt = [&](const SummaryNode& node) -> FunctionID {
        if (node.kind == SummaryNode::Kind::Closure)
            return FunctionID(node.first);
        if (node.kind != SummaryNode::Kind::Variable || node.first >= linked.variables.size())
            return { };
        TypeID proven = linked.variables[node.first].proven;
        if (!proven || holdsOnlyPrimitives(proven))
            return { };
        Type type = program.types.at(proven);
        return type.isObject() ? singleFunctionOf(program.types, type) : FunctionID();
    };
    struct ParameterStored {
        FunctionID function;
        uint32_t entryParameter { 0 };
        uint32_t variable { 0 };
    };
    Vector<ParameterStored> parametersStored;
    BitVector functionsThatStoreParameter;
    BitVector variablesThatTestsMayReplace(program.variables.size());
    GivenByVariable laidByProgram;
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (!variable.flags.contains(VariableFlags::IsReplaceable))
            continue;
        variablesThatTestsMayReplace.quickSet(index);
        if (variable.initialization == VariableInitialization::Function)
            noteIn(laidByProgram, index, variable.function);
    }
    std::span<const FieldReference> exportedFields = program.exportedFields();
    auto findReplaceableField = [&](const SummaryNode& node) -> std::optional<uint32_t> {
        if (node.kind != SummaryNode::Kind::Field || exportedFields.empty())
            return std::nullopt;
        FieldReference field { LayoutID(node.first), node.second };
        auto found = std::ranges::lower_bound(exportedFields, field);
        if (found == exportedFields.end() || *found != field || !fieldAt(program, field).flags.contains(FieldFlags::IsReplaceable))
            return std::nullopt;
        return static_cast<uint32_t>(found - exportedFields.begin());
    };
    GivenByVariable storedInFields;
    bool someVariableMayBeReplaced = !variablesThatTestsMayReplace.isEmpty() || !exportedFields.empty();
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID function(index);
        bool isModuleBody = program.at(function).isModuleBody();
        if (!linked.function(function).isReached || (isModuleBody && !someVariableMayBeReplaced))
            continue;
        const ThinSummary& summary = summaries.at(function);
        for (const SummaryEdge& edge : summary.edges) {
            const SummaryNode& to = summary.nodes[edge.to];
            if (std::optional<uint32_t> field = findReplaceableField(to)) {
                if (FunctionID stored = functionAt(summary.nodes[edge.from]))
                    noteIn(storedInFields, *field, stored);
                else
                    storedInFields.add(*field, Given { }).iterator->value.areSeveral = true;
                continue;
            }
            if (to.kind != SummaryNode::Kind::Variable)
                continue;
            const SummaryNode& from = summary.nodes[edge.from];
            if (variablesThatTestsMayReplace.get(to.first)) {
                if (FunctionID stored = functionAt(from)) {
                    noteIn(laidByProgram, to.first, stored);
                    continue;
                }
                laidByProgram.add(to.first, Given { }).iterator->value.areSeveral = true;
            }
            if (isModuleBody)
                continue;
            if (FunctionID stored = functionAt(from))
                note(to.first, stored);
            else if (from.kind == SummaryNode::Kind::Parameter) {
                parametersStored.append({ function, from.first, to.first });
                functionsThatStoreParameter.set(index);
            }
        }
    }
    for (const auto& [field, stored] : storedInFields) {
        if (stored.function && !stored.areSeveral)
            tables.functionsStoredInFields.append({ exportedFields[field], stored.function });
    }
    std::ranges::sort(tables.functionsStoredInFields, { }, &FactTables::FunctionStoredInField::field);
    for (const auto& [variable, laid] : laidByProgram) {
        if (laid.function && !laid.areSeveral)
            tables.functionsGivenToVariables.append({ variable, laid.function, { } });
    }
    if (givenTo.isEmpty() && parametersStored.isEmpty()) {
        std::ranges::sort(tables.functionsGivenToVariables, { }, &FactTables::FunctionGivenToVariable::variable);
        return;
    }
    FunctionID callerWhoseCallsAreListed;
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> callsBySite;
    for (const LinkedCall& listed : linked.calls) {
        if (!functionsThatStoreParameter.get(listed.callee.index()))
            continue;
        const ThinSummary& summary = summaries.at(listed.caller);
        if (callerWhoseCallsAreListed != listed.caller) {
            callerWhoseCallsAreListed = listed.caller;
            callsBySite.clear();
            for (uint32_t index = 0; index < summary.calls.size(); ++index) {
                if (!summary.calls[index].isMadeByProtocol)
                    callsBySite.add(summary.calls[index].site.index(), index);
            }
        }
        auto found = callsBySite.find(listed.site.index());
        if (found == callsBySite.end())
            continue;
        const SummaryCall* call = &summary.calls[found->value];
        if (call->passesList)
            continue;
        uint32_t firstPositional = program.at(listed.callee).firstParameterIndex();
        auto storedByCallee = std::ranges::equal_range(parametersStored, listed.callee.index(), { }, [](const ParameterStored& stored) {
            return stored.function.index();
        });
        for (const ParameterStored& stored : storedByCallee) {
            uint32_t place = stored.entryParameter;
            if (call->callee != SummaryCall::Callee::Binding) {
                if (place < firstPositional)
                    continue;
                place -= firstPositional;
            }
            if (place >= call->numberOfArguments)
                continue;
            if (FunctionID handed = functionAt(summary.nodes[summary.arguments[call->firstArgument + place]]))
                note(stored.variable, handed);
        }
    }
    GivenByVariable atStart;
    BitVector modulesOfVariables;
    for (const auto& [variable, given] : givenTo) {
        if (!given.areSeveral && variable < program.variables.size())
            modulesOfVariables.set(program.variables[variable].module.index());
    }
    for (size_t module : modulesOfVariables) {
        FunctionID body = program.modules[module].body;
        if (!body || !linked.function(body).isReached)
            continue;
        const ThinSummary& summary = summaries.at(body);
        for (const SummaryEdge& edge : summary.edges) {
            const SummaryNode& to = summary.nodes[edge.to];
            if (to.kind != SummaryNode::Kind::Variable || !givenTo.contains(to.first))
                continue;
            if (FunctionID stored = functionAt(summary.nodes[edge.from]))
                noteIn(atStart, to.first, stored);
        }
    }
    for (const auto& [variable, given] : givenTo) {
        if (given.areSeveral)
            continue;
        auto start = atStart.find(variable);
        FunctionID first = start != atStart.end() && !start->value.areSeveral && start->value.function != given.function ? start->value.function : FunctionID();
        tables.functionsGivenToVariables.append({ variable, given.function, first });
    }
    std::ranges::sort(tables.functionsGivenToVariables, { }, &FactTables::FunctionGivenToVariable::variable);
}

static void fillClassTablesForFacts(const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries, const LinkTables& linked, const LinkTables& held, FactTables& tables)
{
    size_t numberOfLayouts = program.layouts.size();
    BitVector bodiesEnteredOnce = bodiesThatLoaderEntersAtMostOnce(program);
    auto loaderEntersOnce = [&](FunctionID id) {
        return bodiesEnteredOnce.get(id.index());
    };

    Vector<uint32_t> numberOfEvaluations;
    numberOfEvaluations.fill(0, numberOfLayouts);
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        for (LayoutID evaluated : summaries.at(FunctionID(index)).classesEvaluated)
            ++numberOfEvaluations[evaluated.index()];
    }
    auto classEvaluatedForVariable = [&](const Variable& variable) {
        return variable.initialization == VariableInitialization::Function ? classOfConstructorFunction(program, variable.function) : LayoutID();
    };
    for (const Variable& variable : program.variables) {
        if (LayoutID evaluated = classEvaluatedForVariable(variable))
            ++numberOfEvaluations[evaluated.index()];
    }

    tables.classes.fill(ClassFacts { }, numberOfLayouts);
    for (const Variable& variable : program.variables) {
        LayoutID evaluated = classEvaluatedForVariable(variable);
        if (evaluated && numberOfEvaluations[evaluated.index()] == 1 && variable.module && program.at(variable.module).body && loaderEntersOnce(program.at(variable.module).body))
            tables.classes[evaluated.index()].evaluations = ExecutionCount::AtMostOnce;
    }

    Vector<uint32_t> numberOfEvaluationsInBody;
    numberOfEvaluationsInBody.fill(0, numberOfLayouts);
    Vector<LayoutID, 16> classesEvaluatedInBody;
    for (uint32_t number = 0; number < program.functions.size(); ++number) {
        FunctionID id(number);
        bool runsAtMostOnce = loaderEntersOnce(id) || (number < linked.functions.size() && linked.functions[number].runsAtMostOnce);
        ModuleID module = program.at(id).module;
        bool runsOncePerInstance = module && program.at(module).body == id;
        if (summaries.at(id).classesEvaluated.isEmpty() || (!runsAtMostOnce && !runsOncePerInstance))
            continue;
        const BodyIndex* index = bodies.find(id);
        if (!index || index->hasIrreducibleFlow())
            continue;
        classesEvaluatedInBody.shrink(0);
        for (uint32_t block = 0; block < index->numberOfBlocks(); ++block) {
            bool comesAgain = !!index->loopOf(BlockID(block));
            for (BodyValue value : index->block(BlockID(block)).values()) {
                LayoutID evaluated;
                if (value.opcode() == Opcode::NewClass)
                    evaluated = value.as<Opcode::NewClass>().layout();
                else if (value.opcode() == Opcode::NewClosure)
                    evaluated = classOfConstructorFunction(program, value.as<Opcode::NewClosure>().function());
                if (!evaluated)
                    continue;
                if (!numberOfEvaluationsInBody[evaluated.index()])
                    classesEvaluatedInBody.append(evaluated);
                numberOfEvaluationsInBody[evaluated.index()] += comesAgain ? 2 : 1;
            }
        }
        for (LayoutID evaluated : classesEvaluatedInBody) {
            if (std::exchange(numberOfEvaluationsInBody[evaluated.index()], 0) != 1 || numberOfEvaluations[evaluated.index()] != 1)
                continue;
            tables.classes[evaluated.index()].isEvaluatedOncePerInstance = runsOncePerInstance;
            if (!runsAtMostOnce)
                continue;
            tables.classes[evaluated.index()].evaluations = ExecutionCount::AtMostOnce;
            if (!loaderEntersOnce(id) || isBodyWrittenInAnotherFile(program, id))
                tables.functionsTakenToRunOnce.set(number);
        }
    }

    auto heritageSeenToArrive = [&](LayoutID id) {
        bool isAsked = id.index() < held.layouts.size() && !program.at(id).base && program.at(id).has(LayoutFlags::HasHeritage);
        return isAsked ? held.layouts[id.index()].heritage : LayoutID();
    };
    for (size_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID heritage = heritageSeenToArrive(LayoutID(static_cast<uint32_t>(index)));
        size_t stepsLeft = numberOfLayouts;
        LayoutID current = heritage;
        while (current && stepsLeft--)
            current = program.at(current).base ? program.at(current).base : heritageSeenToArrive(current);
        if (!heritage || current)
            continue;
        tables.classes[index].baseState = BaseState::None;
        tables.classes[index].prototypeBase = heritage;
    }
}

static void fillAssignedMembers(const Program& program, const LinkTables& held, FactTables& tables)
{
    auto isDefinedByClass = [&](LayoutID id) {
        const Layout& layout = program.at(id);
        return layout.kind == LayoutKind::Instance && layout.constructor && !layout.isDerived() && !layout.has(LayoutFlags::IsConstructorFunction) && !layout.has(LayoutFlags::IsOfCreatedObject);
    };
    auto isConstructorFunction = [&](LayoutID id) {
        const Layout& layout = program.at(id);
        return layout.kind == LayoutKind::Instance && layout.constructor && layout.has(LayoutFlags::IsConstructorFunction) && !layout.isDerived() && !layout.has(LayoutFlags::IsOfCreatedObject);
    };
    auto findAssignment = [&](LayoutID classLayout, PropertyKeyID key) -> const LinkedAssignment* {
        auto found = std::ranges::equal_range(held.lastAssignments, std::pair { classLayout, key }, { }, &LinkedAssignment::place);
        return found.empty() ? nullptr : &found.front();
    };
    for (const LinkedMember& declared : held.members) {
        if (!declared.key || declared.side != ClassSide::Prototype || declared.kind != MethodKind::Method || !isDefinedByClass(declared.classLayout))
            continue;
        const LinkedAssignment* assignment = held.lastAssignments.isEmpty() ? nullptr : findAssignment(declared.classLayout, declared.key);
        if (!assignment && (!declared.isReplaced || !declared.isAssignedToClass))
            continue;
        FunctionID method = assignment ? assignment->method : declared.replacement;
        if (method)
            tables.assignedMembers.append({ declared.classLayout, declared.key, { method, { }, { }, true, !!assignment } });
    }
    auto placeOf = [](const LinkedNameOnPrototype& name) {
        return std::pair { name.layout, name.key };
    };
    for (const LinkedNameOnPrototype& name : held.accessorsAssignedToClasses) {
        if (!isDefinedByClass(name.layout))
            continue;
        AssignedMember assigned { { }, name.expectedGetter, name.expectedSetter, false, !!findAssignment(name.layout, name.key) };
        for (uint32_t place : held.membersUnder(name.layout, ClassSide::Prototype, name.key)) {
            assigned.isDeclared = true;
            if (const LinkedMember& declared = held.members[place]; declared.kind == MethodKind::Getter && !assigned.getter)
                assigned.getter = declared.written;
            else if (declared.kind == MethodKind::Setter && !assigned.setter)
                assigned.setter = declared.written;
        }
        tables.assignedMembers.append({ name.layout, name.key, assigned });
    }
    for (const LinkedNameOnPrototype& name : held.namesOnPrototypes) {
        if (!name.key || !held.membersUnder(name.layout, ClassSide::Prototype, name.key).empty())
            continue;
        if (isConstructorFunction(name.layout)) {
            AssignedMember assigned;
            assigned.holdsValue = !name.expected && !name.expectedGetter && !name.expectedSetter && !name.mayHoldAccessor;
            bool holdsClosure = name.expected && !name.expectedGetter && !name.expectedSetter && !name.mayHoldAccessor && !program.at(name.expected).captures.isEmpty();
            if (holdsClosure)
                assigned.method = name.expected;
            if (assigned.holdsValue || holdsClosure)
                tables.assignedMembers.append({ name.layout, name.key, assigned });
            continue;
        }
        if (!isDefinedByClass(name.layout))
            continue;
        if (!name.expected && std::ranges::binary_search(held.accessorsAssignedToClasses, placeOf(name), { }, placeOf))
            continue;
        const LinkedAssignment* assignment = findAssignment(name.layout, name.key);
        FunctionID method = name.expected ? name.expected : assignment && !name.mayHoldAccessor ? assignment->method : FunctionID();
        bool isOneMember = !method || (!name.expectedGetter && !name.expectedSetter);
        AssignedMember assigned = isOneMember ? AssignedMember { method, name.expectedGetter, name.expectedSetter } : AssignedMember();
        assigned.holdsValue = isOneMember && !assigned && (assignment || !name.mayHoldAccessor);
        tables.assignedMembers.append({ name.layout, name.key, assigned });
    }
    std::ranges::stable_sort(tables.assignedMembers, { }, &AssignedMemberOfClass::place);
    Vector<AssignedMemberOfClass> values;
    for (const LinkedAssignment& assignment : held.lastAssignments) {
        bool isListed = std::ranges::binary_search(tables.assignedMembers, assignment.place(), { }, &AssignedMemberOfClass::place);
        if (isListed || !isDefinedByClass(assignment.layout) || !held.membersUnder(assignment.layout, ClassSide::Prototype, assignment.key).empty())
            continue;
        values.append({ assignment.layout, assignment.key, { assignment.method, { }, { }, false, true } });
        values.last().member.holdsValue = !assignment.method;
    }
    tables.assignedMembers.appendVector(values);
    tables.assignedMembers.removeAllMatching([&](const AssignedMemberOfClass& assigned) {
        return classHolderIsMadeWithName(program, ClassSide::Prototype, assigned.key);
    });
    std::ranges::stable_sort(tables.assignedMembers, { }, &AssignedMemberOfClass::place);
    for (const AssignedMemberOfClass& assigned : tables.assignedMembers)
        tables.assignedMemberKeys.set(assigned.key.index());
}

static constexpr uint32_t noMemberPosition = UINT32_MAX;

static uint64_t wordOfMember(ClassSide side, PropertyKeyID key)
{
    return static_cast<uint64_t>(side) << 32 | key.bits();
}

static uint64_t memberPositionWord(std::span<const ClassMember> members, uint32_t place)
{
    return place == noMemberPosition ? UINT64_MAX : wordOfMember(members[place].side, members[place].key);
}

static void fillMembersOfClassesForFacts(const Program& program, const ThinSummaries& summaries, const LinkTables& linked, FactTables& tables)
{
    struct DefinedMember {
        LayoutID classLayout;
        uint32_t index { 0 };
        ClassMember member;
        bool isRedefined { false };
    };
    Vector<DefinedMember> defined;
    for (uint32_t number = 0; number < program.functions.size(); ++number) {
        const ThinSummary& summary = summaries.at(FunctionID(number));
        for (const SummaryMember& written : summary.members) {
            const SummaryNode& value = summary.nodes[written.value];
            ClassMember member;
            member.key = written.key;
            member.side = written.side;
            member.kind = written.kind;
            if (value.kind == SummaryNode::Kind::Closure)
                member.function = FunctionID(value.first);
            defined.append({ written.classLayout, written.index, member, written.isRedefined });
        }
    }
    AOT::stableSort(defined, { }, [](const DefinedMember& row) {
        return std::pair(row.classLayout, row.index);
    });

    size_t numberOfLayouts = program.layouts.size();
    FactRows<ClassMember>& rows = tables.membersOfClass;
    BitVector redefinedRows;
    rows.firstRowOfOwner.reserveInitialCapacity(numberOfLayouts + 1);
    size_t next = 0;
    for (size_t layout = 0; layout < numberOfLayouts; ++layout) {
        rows.firstRowOfOwner.append(static_cast<uint32_t>(rows.rows.size()));
        for (size_t first = next; next < defined.size() && defined[next].classLayout.index() == layout; ++next) {
            const DefinedMember& row = defined[next];
            if (next == first || row.index != defined[next - 1].index) {
                if (row.isRedefined)
                    redefinedRows.set(rows.rows.size());
                rows.rows.append(row.member);
                continue;
            }
            ClassMember& listed = rows.rows.last();
            if (listed.key != row.member.key || listed.side != row.member.side || listed.kind != row.member.kind || listed.function != row.member.function) {
                listed.key = { };
                listed.function = { };
            }
        }
        appendMembersAssignedByProgram(program, linked, LayoutID(static_cast<uint32_t>(layout)), rows.rows);
    }
    rows.firstRowOfOwner.append(static_cast<uint32_t>(rows.rows.size()));

    for (const ClassMember& member : rows.rows) {
        if (member.key)
            tables.classMemberKeys.set(member.key.index());
    }
    tables.placesOfMembersOfClassByKey.resize(rows.rows.size());
    for (uint32_t layout = 0; layout < numberOfLayouts; ++layout) {
        std::span<const ClassMember> members = rows.of(layout);
        std::span<uint32_t> places = tables.placesOfMembersOfClassByKey.mutableSpan().subspan(rows.firstRowOfOwner[layout], members.size());
        for (uint32_t place = 0; place < places.size(); ++place)
            places[place] = redefinedRows.get(rows.firstRowOfOwner[layout] + place) ? noMemberPosition : place;
        std::ranges::sort(places, { }, [&](uint32_t place) {
            return std::pair { memberPositionWord(members, place), place };
        });
    }
}

std::span<const uint32_t> Facts::placesOfMembersUnder(LayoutID classLayout, ClassSide side, PropertyKeyID key) const
{
    if (key && !m_tables.classMemberKeys.get(key.index()))
        return { };
    std::span<const ClassMember> members = membersOf(classLayout);
    if (members.empty())
        return { };
    std::span<const uint32_t> places = m_tables.placesOfMembersOfClassByKey.span().subspan(m_tables.membersOfClass.firstRowOfOwner[classLayout.index()], members.size());
    auto wordOfPlace = [&](uint32_t place) {
        return memberPositionWord(members, place);
    };
    const uint64_t word = wordOfMember(side, key);
    auto first = std::ranges::lower_bound(places, word, { }, wordOfPlace);
    auto end = first;
    while (end != places.end() && wordOfPlace(*end) == word)
        ++end;
    return { first, end };
}

BlockID blockWhereListsOfArgumentsAreFirstNamed(const Program& program, const BodyIndexTable& bodies, FunctionID id)
{
    const Function& function = program.at(id);
    const BodyIndex* found = bodies.find(id);
    if (!found || found->hasIrreducibleFlow() || function.isCoroutine() || function.isModuleBody() || function.kind == FunctionKind::BaseConstructor)
        return { };
    if (function.role == FunctionRole::Library || function.enclosingRuntimeFunction || (function.module && program.at(function.module).kind == ModuleKind::RuntimeSource))
        return { };
    const BodyIndex& body = *found;
    BodyBlock entry = body.block(entryBlock);
    Vector<ValueID, 2> lists;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        EntryParameter kind = function.entryParameterKind(index);
        if (kind == EntryParameter::HiddenRest || (kind == EntryParameter::Parameter && index - function.firstParameterIndex() >= function.numberOfPositionalParameters()))
            lists.append(entry.parameter(index));
    }
    if (lists.isEmpty())
        return { };

    auto takes = [&](BodyTarget target) {
        return std::ranges::any_of(target.arguments(), [&](ValueID argument) {
            return lists.contains(argument);
        });
    };
    size_t numberOfLists = lists.size();
    Vector<BlockID, 4> blocksOfLengthsLeftOut;
    PropertyKeyID length = program.keyOf(BuiltinNames::length);
    BlockID whereNamed;
    bool isOnlyForwarded = true;
    auto handsObjectToApply = [&](auto call) {
        std::span<const ValueID> arguments = call.argumentsOperands();
        if (arguments.size() != 2 || !call.calleeKey() || lists.contains(call.calleeOperand()) || lists.contains(call.thisValueOperand()) || lists.contains(arguments[0]))
            return false;
        if (!std::ranges::contains(lists.span().subspan(numberOfLists), arguments[1]))
            return false;
        BuiltinID member = findOwnMember(program, Builtins::FunctionPrototype, call.calleeKey());
        return member && program.builtins().canonicalID(member) == Builtins::FunctionPrototypeApply;
    };
    static constexpr OpcodeSet arrayCallOpcodes { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };
    auto isElidableCallSpread = [&](BodyValue spread) {
        ValueID array = spread.as<Opcode::AppendSpread>().arrayOperand();
        ValueID call(spread.id().index() + 1);
        if (lists.contains(array) || body.isParameter(array) || body.opcodeOf(array) != Opcode::NewArray || body.useCount(array) != 2 || !body.value(array).as<Opcode::NewArray>().elementsOperands().empty())
            return false;
        if (call.index() >= body.numberOfValues() || body.isParameter(call) || body.blockOf(call) != body.blockOf(spread.id()) || !arrayCallOpcodes.contains(body.opcodeOf(call)))
            return false;
        return body.value(call).listOperand() == array;
    };
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        BodyBlock block = body.block(BlockID(number));
        bool names = lists.contains(block.subject()) || (block.hasHandler() && takes(block.handler()));
        isOnlyForwarded &= !names;
        for (BodyValue value : block.values()) {
            if (value.opcode() == Opcode::CreateUnmappedArguments && std::ranges::contains(lists.span().first(numberOfLists), value.as<Opcode::CreateUnmappedArguments>().restOperand())) {
                lists.append(value.id());
                continue;
            }
            if (value.opcode() == Opcode::PropGet && value.key() == length && std::ranges::contains(lists.span().subspan(numberOfLists), value.objectOperand())) {
                blocksOfLengthsLeftOut.append(BlockID(number));
                continue;
            }
            bool isNamedHere = false;
            value.forEachOperand([&](ValueID operand, uint32_t) {
                isNamedHere |= lists.contains(operand);
            });
            names |= isNamedHere;
            if (isNamedHere && isOnlyForwarded)
                isOnlyForwarded = (value.opcode() == Opcode::Call && handsObjectToApply(value.as<Opcode::Call>())) || (value.opcode() == Opcode::AppendSpread && isElidableCallSpread(value)) || (arrayCallOpcodes.contains(value.opcode()) && body.hasOneUse(value.listOperand()) && lists.contains(value.listOperand()));
        }
        bool isNamedAtEnd = false;
        BodyTerminator terminator = block.terminator();
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            isNamedAtEnd |= lists.contains(operand);
        });
        for (BodyTarget target : terminator.targets())
            isNamedAtEnd |= takes(target);
        names |= isNamedAtEnd;
        isOnlyForwarded &= !isNamedAtEnd;
        if (!names)
            continue;
        BlockID other(number);
        if (!whereNamed)
            whereNamed = other;
        while (!body.dominates(whereNamed, other)) {
            whereNamed = body.immediateDominator(whereNamed);
            if (!whereNamed)
                return { };
        }
    }
    if (!whereNamed)
        return { };
    while (whereNamed != entryBlock && (body.loopDepthOf(whereNamed) || body.block(whereNamed).isHandlerBlock() || body.block(whereNamed).hasHandler())) {
        whereNamed = body.immediateDominator(whereNamed);
        if (!whereNamed)
            return { };
    }
    if (whereNamed == entryBlock)
        return isOnlyForwarded ? whereNamed : BlockID();

    if (!blocksOfLengthsLeftOut.isEmpty()) {
        BitVector liesBehind(body.numberOfBlocks());
        Vector<BlockID, 16> toLookAt;
        toLookAt.append(whereNamed);
        while (!toLookAt.isEmpty()) {
            BodyBlock block = body.block(toLookAt.takeLast());
            auto leadsTo = [&](BlockID next) {
                if (!liesBehind.quickSet(next.index()))
                    toLookAt.append(next);
            };
            if (block.hasHandler())
                leadsTo(block.handler().block());
            for (BodyTarget target : block.terminator().targets())
                leadsTo(target.block());
        }
        for (BlockID block : blocksOfLengthsLeftOut) {
            if (liesBehind.quickGet(block.index()) && !body.dominates(whereNamed, block))
                return { };
        }
    }
    if (isOnlyForwarded)
        return whereNamed;

    BitVector reached(body.numberOfBlocks());
    reached.quickSet(entryBlock.index());
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        if (!reached.quickGet(number) || BlockID(number) == whereNamed)
            continue;
        BodyTerminator terminator = body.block(BlockID(number)).terminator();
        if (terminator.kind() == TerminatorKind::Return || terminator.kind() == TerminatorKind::ReturnCall)
            return whereNamed;
        for (BodyTarget target : terminator.targets())
            reached.quickSet(target.block().index());
    }
    return { };
}

std::unique_ptr<Facts> computeFacts(Program& program, BuildContext& build, const FactsRequest& request)
{
    return computeFacts(program, build, request, nullptr);
}

void printBreachesOfClosureOfLink(PrintStream& out, Program& program, BuildContext& build, Mechanisms mechanisms)
{
    const ParallelLoops& loops = build.parallelLoops();
    endStableNumbersOfKeysForFacts(program, loops);
    program.types.keepAddressesOfPropertyKeys();
    program.types.keepAddressesOfTypes();
    std::unique_ptr<BodyIndexTable> bodies = indexBodiesForFacts(program, loops, { });
    if (build.diagnostics().hasErrors())
        return;
    updateProgramBehindIndexesForFacts(program, *bodies);
    mechanisms = withoutMechanismsMissingDependencies(mechanisms);
    ThinSummaries summaries = scanBodies(program, build.diagnostics(), loops, mechanisms);
    if (build.diagnostics().hasErrors())
        return;
    Families families(program, Families::ClassesAlone, loops);
    OwnedLooksAtBodies looks = makeEmptyLooksAtBodies(*bodies);
    lookAtBodies(*looks, program, summaries, build);
    printBreachesOfClosureOfLink(out, program, summaries, families, *looks, mechanisms, build);
}

FunctionID Facts::functionThatCallEntersByBinding(const BodyIndex& index, const BodyValue& call, ValueID* checkThatPasses) const
{
    if (call.opcode() == Opcode::CallDirect)
        return call.function();
    if (call.opcode() != Opcode::Call)
        return { };
    ValueID read = index.stripCasts(call.calleeOperand());
    if (!index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized) {
        if (checkThatPasses)
            *checkThatPasses = read;
        read = index.stripCasts(index.value(read).valueOperand());
    }
    if (index.isParameter(read) || index.opcodeOf(read) != Opcode::GetModuleVariable)
        return { };
    VariableID variable = index.value(read).variable();
    const Variable& declaration = m_program.at(variable);
    if (declaration.owner || declaration.initialization != VariableInitialization::Function || variable.index() >= m_linked.variables.size() || m_linked.variable(variable).isStoredInto)
        return { };
    return declaration.function;
}

bool Facts::testMentionsWhatIsOutOfReach(TypeReader types, TypeID tested, NOESCAPE const ScopedLambda<bool(FunctionID)>& isInReach) const
{
    if (holdsOnlyPrimitives(tested))
        return false;
    const Type& type = types.recordOf(tested);
    if (!type.hasObjects())
        return false;
    bool mentionsIt = types.at(type.layouts).anyOfWithin(m_tables.firstLayoutOfNamespace, m_tables.endOfLayoutsOfNamespaces, [&](LayoutID layout) {
        const Layout& record = m_program.at(layout);
        if (record.kind != LayoutKind::ModuleNamespace)
            return false;
        FunctionID body = record.namespaceOf ? m_program.at(record.namespaceOf).body : FunctionID();
        return !body || !isInReach(body);
    });
    types.at(type.objects).forEach([&](ObjectID object) {
        const ObjectRecord& record = types.at(object);
        FunctionID behind;
        switch (record.kind) {
        case ObjectKind::Closure:
            behind = FunctionID(record.payload);
            break;
        case ObjectKind::Namespace:
            behind = m_program.at(ModuleID(record.payload)).body;
            mentionsIt |= !behind;
            break;
        default:
            return;
        }
        mentionsIt |= behind && !isInReach(behind);
    });
    return mentionsIt;
}

TypeID Facts::typeBelievedOfEntryParameter(TypeArena& types, FunctionID id, unsigned entryParameter, TypeID knownOfCallers) const
{
    TypingContext withoutFacts { m_program, types, PositionTypes::knowingNothing(), TypeLevel::Proven };
    const DeclaredType* declared = m_program.at(id).declaredEntryParameter(entryParameter);
    TypeID type = declared ? m_program.provenTypeOf(*declared) : declaredEntryTypeOf(withoutFacts, id, entryParameter);
    if (!type)
        type = TypeTable::anything;

    TypeID both = knownOfCallers && types.at(type).isJavaScriptValue() ? types.intersectionOf(type, knownOfCallers) : TypeID(TypeTable::never);
    if (isNeverType(both))
        return !declared || !declared->isEstablished() ? types.withSeeds(type, UnknownSeed::ParameterOfEscapedFunction) : type;

    TypeID asked = types.withoutBoundsOfFractions(both);
    TypeID testable = types.testablePartOf(both);
    if ((types.at(testable).anyObject && !types.at(both).anyObject) || (types.at(testable).anyFunction && !types.at(both).anyFunction))
        asked = testable;
    CodeUnit entryByValue { id, CodeRole::GenericCallEntry };
    bool entryMayNotAsk = testMentionsWhatIsOutOfReach(types, asked, scopedLambda<bool(FunctionID)>([&](FunctionID named) {
        return m_linked.mayBeMentionedBy(entryByValue, named);
    }));
    if (entryMayNotAsk)
        asked = types.unionOf(types.withoutObjects(asked), types.at(asked).hasOnlyClosures() ? TypeTable::function : TypeTable::object);
    if (declared && !declared->isEstablished()) {
        TypeID written = declared->written();
        return written && types.at(written).isJavaScriptValue() && types.isSubset(asked, written) ? types.intersectionOf(asked, written) : asked;
    }
    if (asked == both || !declared)
        return asked;
    return types.intersectionOf(type, asked);
}

static void orderFunctionsByReferrersForFacts([[maybe_unused]] Program& program, const ParallelLoops& loops, BuildContext& build, Facts::Parts& parts)
{
    StepTimes steps(build, "facts"_s);
    layReferrersOfHeldReadingOver(parts.linked, parts.held, loops);
    steps.didFinish("lay referrers over"_s);
    rankReferrersFirst(parts.linked);
    findParametersThatAreOnlyCalled(program, *parts.bodies, *parts.summaries, loops, parts.mechanisms, AOT::hasFlag(build.options().testing, Wire::TestingSwitch::AssumeCalleesRetainNothing), parts.linked);
    findParametersThatAreOnlyLookedAt(program, *parts.bodies, *parts.summaries, parts.mechanisms, AOT::hasFlag(build.options().testing, Wire::TestingSwitch::AssumeCalleesRetainNothing), parts.linked);
    carryExpectationOfThisIntoCaptures(program, *parts.bodies, *parts.summaries, parts.linked);
    steps.didFinish("rank referrers"_s);
}

static void fillTablesOfFunctionsForFacts(const Program& program, const FactsRequest&, BuildContext& build, Facts::Parts& parts)
{
    StepTimes steps(build, "facts"_s);
    fillFunctionTablesForFacts(program, *parts.summaries, parts.linked, parts.tables);
    steps.didFinish("tables of functions"_s);
}

static LayoutSetID layoutsThatStayWithCompiledCodeForFacts(Program& program, const LinkTables& linked)
{
    Vector<LayoutID> staying;
    for (uint32_t index = 0; index < linked.layouts.size(); ++index) {
        if (linked.layouts[index].isAllocated && !linked.layouts[index].reachesUnseenCode)
            staying.append(LayoutID(index));
    }
    if (staying.isEmpty())
        return { };
    TypeArena types(program.types, ArenaKeeps::InProgram);
    return types.layoutSets().ofSorted(staying.span());
}

static void fillTablesOfFieldsForFacts(Program& program, const OwnedLooksAtBodies& looks, BuildContext& build, Facts::Parts& parts)
{
    StepTimes steps(build, "facts"_s);
    if (parts.mechanisms.contains(Mechanism::Guesses))
        layKeysThatHeldAloneAppendsOver(program, parts.linked, parts.held, program.layouts.size());
    build.statistics().count(StatisticID::FlowAppendedFields, parts.linked.appendedKeys.size() + parts.linked.keysAppendedByHeldAlone.size());
    fillFieldTablesForFacts(program, parts.linked, parts.mechanisms, parts.tables);
    parts.linked.fieldsReadAsIntegers = fieldsReadAsIntegersIn(*looks);
    parts.linked.namesReadAsIntegers = namesReadAsIntegersIn(*looks);
    parts.linked.fieldsReadForArithmetic = fieldsReadForArithmeticIn(*looks);
    parts.linked.fieldsStoredWhatIsNoInt32 = fieldsStoredWhatIsNoInt32In(*looks);
    parts.linked.fieldsFilledByInitializers = initializerFields(*looks);
    parts.linked.initializerMethods = initializerMethodsIn(*looks, program);
    parts.linked.functionsCalledInLoops = functionsCalledInLoopsIn(*looks, program, *parts.summaries, parts.linked, parts.held, parts.linked.keysCalledInLoops, parts.linked.elementsAreCalledInLoops);
    parts.linked.functionsCalledInLoopsOfOwnFile = functionsCalledInLoopsOfOwnFileIn(*looks, program, *parts.summaries, parts.linked, parts.held);
    parts.linked.functionsCalledByValueInLoops = functionsCalledByValueInLoopsIn(program, *parts.summaries, parts.linked, parts.linked.functionsInElements);
    parts.linked.accessorOrStaticFieldKeys = parts.linked.keysOfAccessorsExpectedInFields;
    parts.linked.accessorOrStaticFieldKeys.merge(parts.linked.keysOfStaticFields);
    parts.linked.functionsCalledByComparators = comparatorCallees(program, *parts.summaries, parts.linked, parts.held);
    markLiteralsStoredByCallees(*parts.summaries, parts.linked, parts.held);
    FunctionsDoneInIterations iterations = findFunctionsFinishedInIterations(program, *parts.summaries, parts.linked, parts.held);
    parts.linked.functionsDoneInIterations = WTF::move(iterations.functions);
    parts.linked.functionsFinishedInNestedIterations = WTF::move(iterations.inNestedIterations);
    parts.linked.constructorsCalledInLoops = constructorsCalledInLoopsIn(program, *parts.summaries, parts.linked, parts.held);
    if (program.someClassExtendsMixin)
        parts.linked.mixedClassesConstructed = mixedClassesConstructedIn(program, *parts.summaries, parts.linked, parts.held);
    parts.linked.fieldsUsedInProducts = fieldsUsedInProductsIn(*looks, parts.linked.functionsCalledInLoops);
    parts.linked.fieldsStoredCounts = fieldsStoredCountsIn(*looks, program, parts.linked);
    parts.linked.keysStoredWhatIsNoInt32 = keysStoredWhatIsNoInt32In(*looks);
    EnforcedFieldCounts enforcedFields = fillEnforcedFieldsForFacts(program, parts.linked, parts.held, *parts.families, parts.mechanisms, parts.tables);
    build.statistics().count(StatisticID::FieldTypesEnforced, enforcedFields.seen);
    build.statistics().count(StatisticID::FieldTypesOfFamily, enforcedFields.ofFamily);
    if (parts.mechanisms.contains(Mechanism::Guesses))
        parts.tables.layoutsThatStayWithCompiledCode = layoutsThatStayWithCompiledCodeForFacts(program, parts.linked);
    steps.didFinish("tables of fields"_s);
}

static void fillTablesOfLayoutsAndClassesForFacts(const Program& program, BuildContext& build, Facts::Parts& parts)
{
    StepTimes steps(build, "facts"_s);
    fillLayoutTablesForFacts(program, parts.linked, parts.held, parts.tables);
    fillTablesOfStringsForFacts(program, *parts.summaries, parts.linked, parts.held, parts.tables);
    fillVariableTablesForFacts(parts.linked, parts.tables);
    if (parts.mechanisms.contains(Mechanism::Guesses))
        fillFunctionsGivenToVariablesForFacts(program, *parts.summaries, parts.linked, parts.tables);
    if (parts.mechanisms.contains(Mechanism::TypedOperations))
        fillClassTablesForFacts(program, *parts.bodies, *parts.summaries, parts.linked, parts.held, parts.tables);
    fillMembersOfClassesForFacts(program, *parts.summaries, parts.held, parts.tables);
    fillAssignedMembers(program, parts.held, parts.tables);
    steps.didFinish("tables of layouts, strings, variables, classes"_s);
}

static void indexLayoutsByNamesForFacts(const Program& program, BuildContext& build, Facts::Parts& parts)
{
    StepTimes steps(build, "facts"_s);
    if (parts.mechanisms.contains(Mechanism::Guesses))
        parts.layoutsByNames = LayoutsByNames(program, *parts.summaries, parts.linked, parts.held);
    steps.didFinish("layouts by names"_s);
}

static void fillTablesOfProgramForFacts(const Program& program, const OwnedLooksAtBodies& looks, BuildContext& build, Facts::Parts& parts)
{
    StepTimes steps(build, "facts"_s);
    computeFactsOfProgram({ program, *parts.bodies, *looks, *parts.summaries, parts.linked, build }, parts.tables);
    parts.linked.exportsSpiedOnThatWireLeavesShut.appendVector(parts.held.exportsSpiedOnThatWireLeavesShut);
    finishFactsOfTestRun(program, build, parts.tables, WTF::move(parts.linked.exportsSpiedOnThatWireLeavesShut));
    steps.didFinish("facts of the program"_s);
}

static bool isEnteredByCallsAlone(const Function& function)
{
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return !function.isCoroutine();
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<BitVector> findFunctionsReachingEvaluation(const Program& program, const ThinSummaries& summaries, const LinkTables& linked)
{
    BitVector found(program.functions.size());
    Vector<FunctionID> pending;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (linked.functions[index].isReached && summaries.at(FunctionID(index)).has(SummaryTrait::HasDirectEval)) {
            found.quickSet(index);
            pending.append(FunctionID(index));
        }
    }
    Vector<std::pair<FunctionID, FunctionID>> callersByCallee;
    for (const LinkedCall& call : linked.calls)
        callersByCallee.append({ call.callee, call.caller });
    std::ranges::sort(callersByCallee);
    while (!pending.isEmpty()) {
        FunctionID id = pending.takeLast();
        const Function& function = program.at(id);
        if (function.isModuleBody()) {
            if (!std::ranges::any_of(program.entries, [&](const Entry& entry) { return program.at(entry.module).body == id; }))
                return std::nullopt;
            continue;
        }
        const LinkedFunction& callee = linked.functions[id.index()];
        bool isEnteredByListedCalls = callee.everyCallerIsKnown && callee.everyCallHasOneCallee && !isClassConstructorOf(function.kind) && isEnteredByCallsAlone(function);
        if (!isEnteredByListedCalls)
            return std::nullopt;
        for (auto [called, caller] : std::ranges::equal_range(callersByCallee, id, { }, &std::pair<FunctionID, FunctionID>::first)) {
            if (!found.quickSet(caller.index()))
                pending.append(caller);
        }
    }
    return found;
}

static void narrowToNumberStoredOnce(Program& program, const BodyIndexTable& bodies, const LinkTables& linked, BeforeEvaluation& before)
{
    static constexpr uint32_t storedOtherwise = UINT32_MAX;
    Vector<uint32_t> numberOfStores;
    Vector<int64_t> stored;
    numberOfStores.fill(0, program.variables.size());
    stored.fill(0, program.variables.size());
    for (uint32_t number = 0; number < program.functions.size(); ++number) {
        if (!linked.functions[number].isReached)
            continue;
        const BodyIndex* index = bodies.find(FunctionID(number));
        if (!index && program.at(FunctionID(number)).hasBody())
            return;
        if (!index || !index->holdsAnyOf(OpcodeSet { Opcode::PutModuleVariable }))
            continue;
        for (uint32_t block = 0; block < index->numberOfBlocks(); ++block) {
            for (BodyValue store : index->block(BlockID(block)).values()) {
                if (store.opcode() != Opcode::PutModuleVariable)
                    continue;
                uint32_t variable = store.variable().index();
                ValueID value = index->stripCasts(store.valueOperand());
                bool isWritten = value && !index->isParameter(value) && index->opcodeOf(value) == Opcode::Integer;
                if (!isWritten || numberOfStores[variable])
                    numberOfStores[variable] = storedOtherwise;
                else {
                    numberOfStores[variable] = 1;
                    stored[variable] = index->value(value).as<Opcode::Integer>().integer();
                }
            }
        }
    }
    TypeArena types(program.types, ArenaKeeps::InProgram);
    for (uint32_t number = 0; number < program.variables.size(); ++number) {
        TypeID& known = before.variables[number];
        if (!known || numberOfStores[number] != 1 || program.variables[number].initialization != VariableInitialization::Undefined)
            continue;
        TypeID integer = types.integer(Range::one(stored[number]));
        if (types.isSubset(known, types.unionOf(TypeTable::undefined, TypeTable::number)) && types.isSubset(integer, known))
            known = types.at(known).primitives & Primitive::Undefined ? types.unionOf(TypeTable::undefined, integer) : integer;
    }
}

static void fillBeforeEvaluation(Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries, const LinkFlow& flow, BeforeEvaluation& before)
{
    if (!flow.mechanisms.containsAll({ Mechanism::ParameterGuards, Mechanism::TypedOperations, Mechanism::Guesses }) || program.admitsInspector)
        return;
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        if (program.variables[index].flags.contains(VariableFlags::MarksEvaluation))
            before.marks.append(VariableID(index));
    }
    static constexpr size_t mostMarks = 2;
    if (before.marks.isEmpty() || before.marks.size() > mostMarks)
        return;
    LinkTables linked = linkBeforeEvaluation(flow, WrappedBodies::StoreLikeModuleBodies);
    std::optional<BitVector> leadToEvaluation = findFunctionsReachingEvaluation(program, summaries, linked);
    if (!leadToEvaluation)
        return;
    bool someClaimHasNoHolder = std::ranges::any_of(linked.variablesStoredFirst, [&](const LinkedVariableStoredFirst& claim) {
        return isBodyOfWrappedModule(program, claim.body) && leadToEvaluation->quickGet(claim.body.index());
    });
    if (someClaimHasNoHolder)
        linked = linkBeforeEvaluation(flow, WrappedBodies::AreFunctions);
    before.firstStores = LinkedFirstStores { linked.variablesStoredFirst, linked.classesInVariables, linked.methodsOnPrototypes };
    before.variables.fill(TypeID(), program.variables.size());
    bool someVariableIsTyped = false;
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.variable && linked.variable(binding.variable).everyWriterIsKnown && linked.variable(binding.variable).proven) {
                before.variables[binding.variable.index()] = linked.variable(binding.variable).proven;
                someVariableIsTyped = true;
            }
            if (TypeID proven = binding.layout && binding.field ? linked.provenOfField({ binding.layout, *binding.field }) : TypeID())
                before.fields.append({ FieldReference { binding.layout, *binding.field }, proven });
        }
    }
    auto compareFields = [](const std::pair<FieldReference, TypeID>& first, const std::pair<FieldReference, TypeID>& second) { return first.first < second.first; };
    std::ranges::sort(before.fields, compareFields);
    before.fields.shrink(std::ranges::unique(before.fields, { }, &std::pair<FieldReference, TypeID>::first).begin() - before.fields.begin());
    if (!someVariableIsTyped && before.fields.isEmpty())
        return;
    narrowToNumberStoredOnce(program, bodies, linked, before);
    before.results.fill(TypeID(), program.functions.size());
    before.functions = BitVector(program.functions.size());
    for (uint32_t number = 0; number < program.functions.size(); ++number) {
        FunctionID id(number);
        bool mayRunBeforeEvaluation = linked.functions[number].isReached && !leadToEvaluation->quickGet(number) && isEnteredByCallsAlone(program.at(id));
        if (mayRunBeforeEvaluation)
            before.results[number] = linked.functions[number].result;
        const BodyIndex* index = mayRunBeforeEvaluation ? bodies.find(id) : nullptr;
        if (!index)
            continue;
        bool holdsClaims = !before.firstStores.variablesStoredFirstIn(id).empty();
        if (!holdsClaims && !index->holdsAnyOf(OpcodeSet { Opcode::GetModuleVariable, Opcode::GetEnvironmentField }))
            continue;
        bool readsTypedVariable = false;
        for (uint32_t block = 0; block < index->numberOfBlocks() && !readsTypedVariable && !holdsClaims; ++block) {
            for (BodyValue value : index->block(BlockID(block)).values()) {
                if (value.opcode() == Opcode::GetModuleVariable)
                    readsTypedVariable |= !!before.typeOf(value.as<Opcode::GetModuleVariable>().variable());
                else if (value.opcode() == Opcode::GetEnvironmentField)
                    readsTypedVariable |= !!before.typeOf(FieldReference { value.layout(), value.field() });
            }
        }
        if (readsTypedVariable || holdsClaims)
            before.functions.quickSet(number);
    }
}

std::unique_ptr<Facts> computeFacts(Program& program, BuildContext& build, const FactsRequest& request, std::unique_ptr<BodyIndexTable>&& indexesOfVerifier)
{
    const ParallelLoops& loops = build.parallelLoops();
    auto hasFailed = [&] {
        return build.diagnostics().hasErrors();
    };

    StepTimes steps(build, "facts"_s);
    Facts::Parts parts;
    parts.mechanisms = withoutMechanismsMissingDependencies(request.mechanisms);
    runLanes(loops, {
        [&] {
            StepTimes stepsOfLane(build, "facts"_s);
            endStableNumbersOfKeysForFacts(program, build.parallelLoops());
            stepsOfLane.didFinish("keys of texts"_s);
        },
        [&] {
            StepTimes stepsOfLane(build, "facts"_s);
            parts.tables.mocks = findMocksOfProgram(program);
            parts.bodies = indexBodiesForFacts(program, build.parallelLoops(), WTF::move(indexesOfVerifier));
            if (hasFailed())
                return;
            updateProgramBehindIndexesForFacts(program, *parts.bodies);
            stepsOfLane.didFinish("mocks, indexes of bodies"_s);
        },
    });
    program.types.keepAddressesOfPropertyKeys();
    program.types.keepAddressesOfTypes();
    steps.didFinish("keys of texts beside mocks and indexes of bodies"_s);
    if (hasFailed())
        return nullptr;

    PhaseTimes times(build, "scan and Link"_s);
    runLanes(loops, {
        [&] {
            parts.summaries = makeUniqueWithoutFastMallocCheck<ThinSummaries>(scanBodies(program, build.diagnostics(), build.parallelLoops(), parts.mechanisms));
        },
        [&] {
            StepTimes stepsOfLane(build, "facts"_s);
            parts.familiesOfFlow = makeUniqueWithoutFastMallocCheck<Families>(program, Families::ClassesAlone, build.parallelLoops());
            stepsOfLane.didFinish("families"_s);
        },
    });
    times.didFinish("scan"_s);
    if (hasFailed())
        return nullptr;
    if (build.options().misstatedFact) [[unlikely]]
        misstateSummaries(program, *parts.summaries, build);
    build.statistics().count(StatisticID::ValuesScanned, parts.summaries->numberOfValuesScanned());
    steps.didFinish("scan beside families"_s);
    OwnedLooksAtBodies looks = makeEmptyLooksAtBodies(*parts.bodies);
    std::unique_ptr<LinkFlow> flow;
    Vector<DeclaredSetForFamilies> setsDeclaredInProgram;
    runLanes(loops, {
        [&] {
            StepTimes stepsOfLane(build, "facts"_s);
            flow = makeUniqueWithoutFastMallocCheck<LinkFlow>(program, *parts.summaries, *parts.familiesOfFlow, *looks, parts.mechanisms, build, request.orderOfWorklist);
            stepsOfLane.didFinish("the flow is made"_s);
        },
        [&] {
            StepTimes stepsOfLane(build, "facts"_s);
            lookAtBodies(*looks, program, *parts.summaries, build);
            stepsOfLane.didFinish("look at bodies"_s);
        },
        [&] {
            StepTimes stepsOfLane(build, "facts"_s);
            setsDeclaredInProgram = setsDeclaredForFamilies(program, parts.summaries->setsOfLayoutsWrittenInBodies(), build.parallelLoops());
            stepsOfLane.didFinish("sets declared for families"_s);
        },
    });
    flow->setsDeclaredInProgram = &setsDeclaredInProgram;
    fillBeforeEvaluation(program, *parts.bodies, *parts.summaries, *flow, parts.tables.beforeEvaluation);
    times.didFinish("families"_s);
    steps.didFinish("the flow beside the look at bodies and the sets declared for families"_s);
    TurnsOfReadings turns;
    runLanes(loops, {
        [&] {
            parts.linked = link(WTF::move(flow), &turns);
        },
        [&] {
            if (parts.mechanisms.contains(Mechanism::BuiltinCalls))
                workOutRegExpTexts(program, *parts.bodies, *parts.summaries, parts.tables.program);
        },
    });
    if (hasFailed())
        return nullptr;
    for (const RegExpCallWithoutText& call : parts.tables.program.regExpCallsWithoutText) {
        const BodyIndex& index = *parts.bodies->find(call.function);
        build.diagnostics().add(UserDiagnostic {
            .kind = UserDiagnosticKind::RegExpPatternNotConstant,
            .function = call.function,
            .block = index.blockOf(call.site),
            .value = call.site,
            .count = 1,
            .location = program.at(call.function).locationOf(index.value(call.site)),
        });
    }
    for (uint32_t index = 0; index < parts.linked.functions.size(); ++index) {
        if (parts.mechanisms.contains(Mechanism::EscapeAnalysis) && program.at(FunctionID(index)).hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
            parts.linked.functions[index].callEntryWalksBody = !!blockWhereListsOfArgumentsAreFirstNamed(program, *parts.bodies, FunctionID(index));
    }
    const SizeOfLinkFlow& sizeOfFlow = parts.linked.sizeOfFlow;
    build.statistics().count(StatisticID::FactNodes, sizeOfFlow.nodes);
    build.statistics().count(StatisticID::FactEdges, sizeOfFlow.edges);
    build.statistics().count(StatisticID::FactsCarriedOverEdges, sizeOfFlow.nodesTakenOffWorklist);
    build.statistics().count(StatisticID::FactRuleCalls, sizeOfFlow.observersTold);
    build.statistics().count(StatisticID::FactRounds, sizeOfFlow.rounds);
    parts.held = WTF::move(turns.held);
    parts.families = WTF::move(parts.linked.familiesBehindFlow);
    if (!parts.families)
        parts.families = makeUniqueWithoutFastMallocCheck<Families>(program, setsDeclaredInProgram.span(), std::span<const LayoutSetID>(), build.parallelLoops());
    steps.didFinish("the two readings"_s);
    if (build.options().misstatedFact) [[unlikely]]
        misstateLinkTables(program, *parts.summaries, parts.linked, parts.held, build);
    openNamesFoundByLink(program, build, parts.linked, parts.held);
    steps.didFinish("open names"_s);
    times.didFinish("Link"_s);
    if (hasFailed())
        return nullptr;

    bool expectsByNamesAsked = parts.mechanisms.contains(Mechanism::Guesses) && turns.provingFlow;
    NamesAskedOfContentsOfFields namesAsked;
    runLanes(loops, {
        [&] { indexLayoutsByNamesForFacts(program, build, parts); },
        [&] {
            if (expectsByNamesAsked)
                namesAsked = gatherNamesAskedOfContentsOfFields(program, *turns.provingFlow, parts.held);
        },
    });
    if (expectsByNamesAsked)
        expectContentsOfFieldsByNamesAsked(program, *turns.provingFlow, namesAsked, parts.layoutsByNames, parts.held);
    steps.didFinish("layouts by names beside the names asked of what lies in fields"_s);

    runLanes(loops, {
        [&] {
            StepTimes stepsOfLane(build, "facts"_s);
            finishReadingsOfLink(program, parts.mechanisms, turns, parts.linked, parts.held);
            build.backgroundJobs().add([](const ParallelLoops&) {
                WTF::releaseFastMallocFreeMemory();
            });
            stepsOfLane.didFinish("finish the readings"_s);
            fillTablesOfFieldsForFacts(program, looks, build, parts);
        },
        [&] { fillTablesOfProgramForFacts(program, looks, build, parts); },
        [&] { orderFunctionsByReferrersForFacts(program, build.parallelLoops(), build, parts); },
        [&] { fillTablesOfLayoutsAndClassesForFacts(program, build, parts); },
        [&] { fillTablesOfFunctionsForFacts(program, request, build, parts); },
    });
    parts.layoutsByNames.dropFewSparseRecords(program, setsDeclaredInProgram.span());
    for (size_t layout = 0; layout < parts.linked.layouts.size() && layout < parts.tables.classes.size(); ++layout) {
        if (parts.tables.classes[layout].evaluations != ExecutionCount::AtMostOnce)
            parts.linked.layouts[layout].prototypeObjectIsSettledAheadOfEveryObject = false;
    }

    if (build.options().misstatedFact) [[unlikely]]
        misstateFactTables(program, parts.linked, parts.tables, build);
    steps.didFinish("referrers beside tables"_s);
    times.didFinish("referrers, tables"_s);
    if (hasFailed())
        return nullptr;
    return makeUnique<Facts>(program, WTF::move(parts));
}

AssignedMember findAssignedMember(const Program& program, const Facts& facts, LayoutID classLayout, PropertyKeyID key)
{
    const Vector<AssignedMemberOfClass>& members = facts.tables().assignedMembers;
    if (!key || liesInThread(key) || !facts.tables().assignedMemberKeys.get(key.index()))
        return { };
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = classLayout; current && stepsLeft; --stepsLeft) {
        const Layout& layout = program.at(current);
        if (layout.kind != LayoutKind::Instance || !layout.constructor || layout.isDerived() || layout.has(LayoutFlags::IsOfCreatedObject) || !facts.isEvaluatedAtMostOnce(current))
            return { };
        auto assigned = std::ranges::equal_range(members, std::pair { current, key }, { }, &AssignedMemberOfClass::place);
        if (!assigned.empty())
            return assigned.size() == 1 ? assigned.front().member : AssignedMember();
        if (!facts.placesOfMembersUnder(current, ClassSide::Prototype, key).empty())
            return { };
        ClassFacts classFacts = facts.classOf(current);
        switch (classFacts.baseState) {
        case BaseState::Layout:
            current = classFacts.base;
            break;
        case BaseState::Unknown:
            current = writtenBaseOf(layout);
            break;
        case BaseState::None:
            current = classFacts.prototypeBase;
            break;
        }
    }
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
