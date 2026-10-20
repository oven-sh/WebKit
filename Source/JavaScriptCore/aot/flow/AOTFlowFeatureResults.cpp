/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowUnseenCode.h"
#include <algorithm>
#include <span>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BuiltinCodeUse {
    bool entersCode { false };
    bool hasUnseenOperands { false };
    Witness witness { noWitness };
};

struct ConditionOperand {
    BuiltinID builtin;
    const FlowBuiltinOperandValue* value { nullptr };
};

enum class ConditionState : uint8_t {
    Unknown,
    Refuted,
};

struct FeatureLineUse {
    ConditionState condition { ConditionState::Unknown };
    Witness witness { noWitness };
};

struct FeatureLineInputs {
    const FlowFamilyInputs& inputs;
    std::span<const ConditionOperand> conditionOperands;
    const BitVector& selectedOverloads;
};

struct LocaleOperand {
    const FlowFamilyInputs& inputs;
    BuiltinID builtin;
    Witness witness;
    Vector<FlowLocaleUse>& uses;
};

enum class LocaleListContents : uint8_t {
    Unknown,
    OnlyTags,
};

} // anonymous namespace

static Vector<BuiltinUseRecord> collectBuiltinUses(const FlowFamilyInputs& inputs)
{
    Vector<BuiltinUseRecord> uses;
    for (uint32_t index = 0, count = inputs.solution.builtins().builtinCount(); index < count; ++index) {
        const FlowBuiltinUse& use = inputs.records().builtinUse(BuiltinID(index));
        if (use.uses.isEmpty())
            continue;
        uses.append(BuiltinUseRecord { index, static_cast<BuiltinUse>(use.uses.toRaw()), use.firstUse });
    }
    return uses;
}

static Vector<BuiltinID> findConstructorsWithFeatures(const BuiltinTable& builtins)
{
    Vector<BuiltinID> constructorsByClass(builtins.classCount());
    for (uint32_t index = 0; index < constructorsByClass.size(); ++index) {
        BuiltinID constructor = builtins.at(BuiltinClassID(index)).constructor;
        if (!constructor)
            continue;
        constructor = builtins.canonicalID(constructor);
        if (!builtins.featureUsesOf(constructor).empty())
            constructorsByClass[index] = constructor;
    }
    return constructorsByClass;
}

static void addConstructionsByOperations(const FlowFamilyInputs& inputs, Vector<BuiltinCodeUse>& codeUses)
{
    const FlowObjectTable& objects = inputs.objects();
    const FlowRecords& records = inputs.records();
    Vector<BuiltinID> constructorsByClass = findConstructorsWithFeatures(inputs.solution.builtins());
    for (uint32_t index = 0; index < objects.size(); ++index) {
        FlowObjectID id(index);
        const FlowObject& object = objects.at(id);
        if (!object.builtinClass || !object.site)
            continue;
        BuiltinID constructor = constructorsByClass[object.builtinClass.index()];
        if (!constructor || objects.at(object.site).description.origin != FlowSiteOrigin::Operation)
            continue;
        if (!records.eventsOf(id).contains(FlowObjectEvent::Allocated))
            continue;
        BuiltinCodeUse& use = codeUses[constructor.index()];
        use.entersCode = true;
        use.hasUnseenOperands = true;
        keepLowerWitness(use.witness, records.witnessOf(id, FlowObjectEvent::Allocated));
    }
}

static BitVector findNamespacesOfModules(const BuiltinTable& builtins)
{
    BitVector namespaces(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.moduleCount(); ++index) {
        if (BuiltinID namespaceObject = builtins.at(RuntimeModuleID(index)).namespaceObject)
            namespaces.quickSet(namespaceObject.index());
    }
    return namespaces;
}

static Vector<BuiltinCodeUse> collectCodeUses(const FlowFamilyInputs& inputs)
{
    const BuiltinTable& builtins = inputs.solution.builtins();
    BitVector namespacesOfModules = findNamespacesOfModules(builtins);
    Vector<BuiltinCodeUse> codeUses(builtins.builtinCount());
    for (uint32_t index = 0; index < codeUses.size(); ++index) {
        BuiltinID builtin(index);
        const FlowBuiltinUse& use = inputs.records().builtinUse(builtin);
        bool isLoadedModule = namespacesOfModules.quickGet(index) && use.uses.contains(BuiltinUse::Read);
        if (!isLoadedModule && !use.uses.containsAny(usesEnteringCode(builtins, builtin)))
            continue;

        // The table attaches features to the entry of the object, and an access is recorded on the location that was accessed.
        BuiltinCodeUse& codeUse = codeUses[builtins.canonicalID(builtin).index()];
        codeUse.entersCode = true;
        codeUse.hasUnseenOperands |= use.uses.contains(BuiltinUse::Escaped);
        keepLowerWitness(codeUse.witness, use.firstUse);
    }
    addConstructionsByOperations(inputs, codeUses);
    return codeUses;
}

static bool isOperandOf(const BuiltinFeatureUse& line, const FlowBuiltinOperandValue& value)
{
    return line.condition && line.condition->operand == value.operand && (!line.overload || line.overload == value.overload);
}

static Vector<ConditionOperand> collectConditionOperands(const FlowFamilyInputs& inputs)
{
    const BuiltinTable& builtins = inputs.solution.builtins();
    Vector<ConditionOperand> operands;
    for (const FlowBuiltinOperandValue& value : inputs.records().builtinOperandValues()) {
        BuiltinID builtin = builtins.at(value.overload).builtin;
        bool isTested = std::ranges::any_of(builtins.featureUsesOf(builtin), [&](const BuiltinFeatureUse& line) {
            return isOperandOf(line, value);
        });
        if (isTested)
            operands.append(ConditionOperand { builtin, &value });
    }
    parallelSort(inputs.loops(), operands.mutableSpan(), [](const ConditionOperand& a, const ConditionOperand& b) {
        return a.builtin < b.builtin;
    });
    return operands;
}

static BitVector collectSelectedOverloads(const FlowFamilyInputs& inputs)
{
    BitVector overloads(inputs.solution.builtins().overloadCount());
    for (const FlowBuiltinCallLink& link : inputs.records().builtinCallLinks()) {
        if (link.overload)
            overloads.quickSet(link.overload.index());
    }
    return overloads;
}

static ConditionState computeConditionState(const FlowFamilyInputs& inputs, const BuiltinCondition& condition, FlowNodeID operand)
{
    if (!operand)
        return ConditionState::Refuted;
    bool mayHold = conditionMayHold(inputs.program(), inputs.solution.values(), inputs.graph().valueOf(operand), condition);
    return mayHold ? ConditionState::Unknown : ConditionState::Refuted;
}

static FeatureLineUse findUseOf(const FeatureLineInputs& lineInputs, const BuiltinFeatureUse& line, const BuiltinCodeUse& codeUse, std::span<const ConditionOperand> operandsOfBuiltin)
{
    const FlowFamilyInputs& inputs = lineInputs.inputs;
    if (codeUse.hasUnseenOperands)
        return { ConditionState::Unknown, codeUse.witness };
    if (line.overload && !lineInputs.selectedOverloads.quickGet(line.overload.index()))
        return { ConditionState::Refuted, noWitness };
    if (!line.condition)
        return { ConditionState::Unknown, codeUse.witness };

    bool hasOperand = false;
    FeatureLineUse use { ConditionState::Refuted, noWitness };
    for (const ConditionOperand& operand : operandsOfBuiltin) {
        const FlowBuiltinOperandValue& value = *operand.value;
        if (!isOperandOf(line, value))
            continue;
        hasOperand = true;
        if (computeConditionState(inputs, *line.condition, value.node) == ConditionState::Refuted)
            continue;
        use.condition = ConditionState::Unknown;
        keepLowerWitness(use.witness, inputs.graph().witnessOf(value.callerInstance, value.site));
    }
    if (!hasOperand)
        return { ConditionState::Unknown, codeUse.witness };
    return use;
}

static void appendFeatureUsesOf(const FeatureLineInputs& lineInputs, BuiltinID builtin, const BuiltinCodeUse& codeUse, Vector<FeatureUseRecord>& uses)
{
    auto operandsOfBuiltin = std::ranges::equal_range(lineInputs.conditionOperands, builtin, { }, &ConditionOperand::builtin);
    size_t firstOfBuiltin = uses.size();
    for (const BuiltinFeatureUse& line : lineInputs.inputs.solution.builtins().featureUsesOf(builtin)) {
        FeatureLineUse use = findUseOf(lineInputs, line, codeUse, operandsOfBuiltin);
        if (use.condition == ConditionState::Refuted)
            continue;
        uint32_t feature = static_cast<uint32_t>(line.feature);
        std::span<FeatureUseRecord> usesOfBuiltin = uses.mutableSpan().subspan(firstOfBuiltin);
        auto recorded = std::ranges::find(usesOfBuiltin, feature, &FeatureUseRecord::feature);
        if (recorded == usesOfBuiltin.end())
            uses.append(FeatureUseRecord { feature, builtin.index(), use.witness });
        else
            keepLowerWitness(recorded->witness, use.witness);
    }
}

static Vector<FeatureUseRecord> collectFeatureUses(const FlowFamilyInputs& inputs)
{
    Vector<BuiltinCodeUse> codeUses = collectCodeUses(inputs);
    Vector<ConditionOperand> operands = collectConditionOperands(inputs);
    BitVector selectedOverloads = collectSelectedOverloads(inputs);
    FeatureLineInputs lineInputs { inputs, operands.span(), selectedOverloads };
    Vector<FeatureUseRecord> uses;
    for (uint32_t index = 0; index < codeUses.size(); ++index) {
        if (codeUses[index].entersCode)
            appendFeatureUsesOf(lineInputs, BuiltinID(index), codeUses[index], uses);
    }
    auto orderOf = [](const FeatureUseRecord& use) {
        return std::tuple(use.feature, use.witness.location.module, use.witness.location.position, use.builtin);
    };
    parallelSort(inputs.loops(), uses.mutableSpan(), [&](const FeatureUseRecord& a, const FeatureUseRecord& b) {
        return orderOf(a) < orderOf(b);
    });
    return uses;
}

static void addLocaleUse(const LocaleOperand& operand, LocaleUseKind kind, StringID tag = { })
{
    operand.uses.append(FlowLocaleUse { kind, tag, operand.builtin, operand.witness });
}

static void addLocaleTags(const LocaleOperand& operand, const FlowValue& value)
{
    for (StringID tag : operand.inputs.solution.values().stringsOf(value.strings))
        addLocaleUse(operand, LocaleUseKind::Constant, tag);
}

static LocaleListContents addLocaleList(const LocaleOperand& operand, FlowObjectID list)
{
    const FlowObjectTable& objects = operand.inputs.objects();
    const FlowGraph& graph = operand.inputs.graph();
    if (objects.kindOf(list) != FlowObjectKind::Instance || !objects.isInstanceOf(list, BuiltinClasses::Array))
        return LocaleListContents::Unknown;

    // An empty list asks for the default locale.
    if (!objects.at(list).numberOfPositions)
        addLocaleUse(operand, LocaleUseKind::HostDefault);

    LocaleListContents contents = LocaleListContents::OnlyTags;
    forEachElementPart(graph, list, arrayIndexRange, [&](FlowNodeID part) {
        const FlowValue& element = graph.valueOf(part);
        addLocaleTags(operand, element);
        if (element.mayBeObject() || (element.primitives & ~(Primitive::LiteralString | Primitive::Absent)))
            contents = LocaleListContents::Unknown;
    });
    return contents;
}

static void addLocaleUsesOf(const LocaleOperand& operand, FlowNodeID node)
{
    if (!node) {
        addLocaleUse(operand, LocaleUseKind::HostDefault);
        return;
    }

    const FlowValue& value = operand.inputs.graph().valueOf(node);
    if (value.primitives & Primitive::Undefined)
        addLocaleUse(operand, LocaleUseKind::HostDefault);
    addLocaleTags(operand, value);

    bool hasUnknownPart = value.primitives & ~(Primitive::LiteralString | Primitive::Undefined);
    value.objects.forEach([&](FlowObjectID object) {
        if (addLocaleList(operand, object) == LocaleListContents::Unknown)
            hasUnknownPart = true;
    });
    if (hasUnknownPart)
        addLocaleUse(operand, LocaleUseKind::Unknown);
}

static Vector<FlowLocaleUse> collectLocaleUses(const FlowFamilyInputs& inputs)
{
    const BuiltinTable& builtins = inputs.solution.builtins();
    Vector<FlowLocaleUse> uses;
    for (const FlowBuiltinOperandValue& value : inputs.records().builtinOperandValues()) {
        if (!value.roles.contains(BuiltinOperandRole::Locale))
            continue;
        addLocaleUsesOf(LocaleOperand { inputs, builtins.at(value.overload).builtin, inputs.graph().witnessOf(value.callerInstance, value.site), uses }, value.node);
    }
    // Code that the build has not seen names any locale.
    for (const UnseenCodeRecord& record : inputs.records().unseenCode()) {
        if (admitsUnknownJavaScript(record.cause)) {
            uses.append(FlowLocaleUse { LocaleUseKind::Unknown, { }, record.builtin == Wire::none ? BuiltinID() : BuiltinID(record.builtin), record.witness });
            break;
        }
    }
    auto orderOf = [](const FlowLocaleUse& use) {
        return std::tuple(use.kind, use.tag, use.witness.function, use.witness.value);
    };
    sortAndRemoveDuplicates(inputs.loops(), uses, [&](const FlowLocaleUse& a, const FlowLocaleUse& b) {
        return orderOf(a) < orderOf(b);
    }, [](const FlowLocaleUse& a, const FlowLocaleUse& b) {
        return a.kind == b.kind && a.tag == b.tag;
    });
    return uses;
}

static Witness declarationWitnessOf(const FlowGraph& graph, FunctionID function)
{
    if (graph.isHost(function))
        return noWitness;
    return witnessOf(Subject { .function = function, .location = graph.program().at(function).declarationLocation() });
}

static Vector<UnseenCodeRecord> collectUnseenCode(const FlowFamilyInputs& inputs)
{
    const FlowRecords& records = inputs.records();
    Vector<UnseenCodeRecord> unseenCode;
    for (const UnseenCodeRecord& record : records.unseenCode()) {
        if (record.cause != UnseenCodeCause::LimitReached)
            unseenCode.append(record);
    }
    for (size_t index : records.functionsPastWorkLimit())
        unseenCode.append(UnseenCodeRecord { .cause = UnseenCodeCause::LimitReached, .witness = declarationWitnessOf(inputs.graph(), FunctionID(index)) });
    if (!inputs.build().isEnabledForProgram(FlowResultKind::ReachedFunctions))
        unseenCode.append(UnseenCodeRecord { .cause = UnseenCodeCause::FlowResultDisabled });

    auto orderOf = [](const UnseenCodeRecord& record) {
        return std::tuple(record.cause, record.builtin, record.witness.function, record.witness.value);
    };
    parallelSort(inputs.loops(), unseenCode.mutableSpan(), [&](const UnseenCodeRecord& a, const UnseenCodeRecord& b) {
        return orderOf(a) < orderOf(b);
    });
    return unseenCode;
}

void computeFeatureResults(const FlowFamilyInputs& inputs, FlowBuiltinTables& tables, Vector<UnseenCodeRecord>& unseenCode)
{
    tables.uses = collectBuiltinUses(inputs);
    tables.featureUses = collectFeatureUses(inputs);
    tables.localeUses = collectLocaleUses(inputs);
    unseenCode = collectUnseenCode(inputs);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
