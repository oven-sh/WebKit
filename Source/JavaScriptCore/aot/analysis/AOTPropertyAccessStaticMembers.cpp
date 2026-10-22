/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccessInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowNamespaces.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class StaticFieldDefinition : uint8_t { HasRun, HasNotRun };

} // anonymous namespace

static MemberAnswer findFromClassObject(const PropertyAccessContext&, const MemberQuery&, StaticFieldDefinition);

static bool isOwnFunctionProperty(const Program& program, PropertyKeyID key)
{
    NameClass nameClass = program.nameClassOf(key);
    return nameClass == NameClass::Length || nameClass == NameClass::Prototype || program.builtinNameOf(key) == BuiltinNames::name;
}

static MemberAnswer createProvenAnswer(MemberKind kind)
{
    MemberAnswer answer;
    answer.kind = kind;
    answer.level = KnowledgeLevel::Proven;
    return answer;
}

static void addConditionWithCause(MemberAnswer& walked, const MemberCondition& condition, UnprovenCause cause)
{
    addCondition(walked.conditions, condition);
    if (walked.cause == UnprovenCause::Unknown)
        walked.cause = cause;
}

static MemberCondition createMemberUnchangedCondition(const MemberQuery& query)
{
    return { MemberCondition::Kind::MemberUnchanged, query.layout, ClassSide::Static, query.key };
}

static bool mayInterceptStore(MemberKind kind)
{
    switch (kind) {
    case MemberKind::Absent:
    case MemberKind::Method:
    case MemberKind::Variable:
        return false;
    case MemberKind::Unknown:
    case MemberKind::Field:
    case MemberKind::Accessor:
    case MemberKind::Namespace:
    case MemberKind::Builtin:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Until its definition has run, a store to a static field reaches the other members of the class and the classes above it.
static MemberAnswer findMemberBeforeDefinition(const PropertyAccessContext& context, const MemberQuery& store)
{
    MemberQuery read { MemberStart::ClassObject, store.layout, store.key, Access::Read, false };
    return findFromClassObject(context, read, StaticFieldDefinition::HasNotRun);
}

static MemberAnswer finishAtStaticField(const PropertyAccessContext& context, const MemberQuery& query, MemberAnswer&& walked, LayoutID holder, VariableID variable)
{
    const Program& program = context.program;
    bool isStore = query.access == Access::Store || query.access == Access::Define;
    if (isStore && holder != query.layout)
        return MemberAnswer::unknown(UnprovenCause::Unknown);

    // For a static field the proof covers every access through the class object but the definition itself.
    bool definitionHasRun = program.at(variable).flow.initialization != InitializationState::Unknown;
    if (query.access == Access::Store && !definitionHasRun) {
        // A store to a private name throws until the definition has run.
        bool isPrivateName = program.at(query.key).kind == PropertyKeyKind::PrivateName;
        MemberAnswer beforeDefinition = findMemberBeforeDefinition(context, query);
        if (isPrivateName || mayInterceptStore(beforeDefinition.kind))
            return MemberAnswer::unknown(UnprovenCause::PropertyAdded);
        for (const MemberCondition& condition : beforeDefinition.conditions)
            addConditionWithCause(walked, condition, beforeDefinition.cause);
    }

    walked.kind = MemberKind::Variable;
    walked.variable = variable;
    walked.holder = holder;
    if (!program.at(holder).classFlow.staticNamesAreComplete)
        addConditionWithCause(walked, createMemberUnchangedCondition(query), UnprovenCause::UnknownKeyStore);
    else if (program.at(variable).flow.writers != WriterState::AllKnown)
        addConditionWithCause(walked, createMemberUnchangedCondition(query), UnprovenCause::UnknownWriter);
    setLevel(walked);
    return WTF::move(walked);
}

static MemberAnswer finishAtBuiltinConstructor(const PropertyAccessContext& context, const MemberQuery& query, MemberAnswer&& walked, BuiltinClassID base)
{
    BuiltinID constructor = context.program.builtins().at(base).constructor;
    if (!constructor)
        return MemberAnswer::unknown(UnprovenCause::UnknownBase);
    return finishAtChainEnd(WTF::move(walked), findBuiltinObjectMember(context, constructor, query.key, query.access));
}

static MemberAnswer findFromClassObject(const PropertyAccessContext& context, const MemberQuery& query, StaticFieldDefinition definition)
{
    const Program& program = context.program;
    bool isPrivateName = program.at(query.key).kind == PropertyKeyKind::PrivateName;

    MemberAnswer walked;
    LayoutID current = query.layout;
    for (size_t unvisitedClasses = program.layouts.size(); unvisitedClasses; --unvisitedClasses) {
        const Layout& layout = program.at(current);
        if (layout.kind != LayoutKind::Instance || !layout.constructor)
            return MemberAnswer::unknown(UnprovenCause::NonLayoutObject);
        const ClassFlow& classFlow = layout.classFlow;
        if (classFlow.evaluations != ExecutionCount::AtMostOnce)
            addConditionWithCause(walked, { MemberCondition::Kind::ClassEvaluatedOnce, current, ClassSide::Prototype, { } }, UnprovenCause::ClassEvaluatedAgain);

        // A class may be created while a static initializer of its base runs, so no static field on the chain counts as defined.
        bool skipsStaticField = definition == StaticFieldDefinition::HasNotRun;
        if (VariableID variable = skipsStaticField ? VariableID() : findStaticFieldVariable(program, layout.constructor, query.key))
            return finishAtStaticField(context, query, WTF::move(walked), current, variable);

        DeclaredMember member = findDeclaredMember(classFlow, ClassSide::Static, query.key);
        if (isPrivateName) {
            bool isAbsent = classFlow.staticNamesAreComplete && !member.isDeclared() && !member.isListedWithoutDefinition();
            return isAbsent ? createProvenAnswer(MemberKind::Absent) : MemberAnswer::unknown(UnprovenCause::PropertyAdded);
        }
        if (member.isDeclared()) {
            if (member.mutability != MemberMutability::Fixed)
                addConditionWithCause(walked, createMemberUnchangedCondition(query), UnprovenCause::PropertyRedefined);
            setDeclaredMember(walked, member, current);
            setLevel(walked);
            return walked;
        }
        if (member.isListedWithoutDefinition())
            return MemberAnswer::unknown(UnprovenCause::PropertyAdded);
        if (!classFlow.staticNamesAreComplete)
            addConditionWithCause(walked, createMemberUnchangedCondition(query), UnprovenCause::UnknownKeyStore);
        if (query.isOwnLookup) {
            walked.kind = MemberKind::Absent;
            setLevel(walked);
            return walked;
        }

        NextHolder next = nextHolderOf(program, current, ClassSide::Static, walked.conditions);
        switch (next.step) {
        case ChainStep::Unknown:
            return MemberAnswer::unknown(UnprovenCause::UnknownBase);
        case ChainStep::CompiledClass:
            current = next.layout;
            continue;
        case ChainStep::BuiltinClass:
            return finishAtBuiltinConstructor(context, query, WTF::move(walked), next.builtinClass);
        case ChainStep::NoBase:
            return finishAtChainEnd(WTF::move(walked), findBuiltinInstanceMember(context, BuiltinClasses::Function, query.key, query.access));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return MemberAnswer::unknown(UnprovenCause::UnknownBase);
}

MemberAnswer findStaticMember(const PropertyAccessContext& context, const MemberQuery& query)
{
    const Program& program = context.program;
    if (keyLookupOf(program, query.key) != KeyLookup::Ordinary || isOwnFunctionProperty(program, query.key))
        return MemberAnswer::unknown(UnprovenCause::Unknown);
    return findFromClassObject(context, query, StaticFieldDefinition::HasRun);
}

static bool isAddedByRequire(const Program& program, PropertyKeyID key)
{
    return program.builtinNameOf(key) == BuiltinNames::__esModule;
}

static MemberAnswer createExportAnswer(const MemberQuery& query, const ModuleExport& entry, uint32_t field)
{
    switch (entry.kind) {
    case ModuleExportKind::Variable: {
        MemberAnswer answer = createProvenAnswer(MemberKind::Variable);
        answer.variable = entry.variable;
        answer.holder = query.layout;
        answer.field = field;
        return answer;
    }
    case ModuleExportKind::Namespace: {
        if (entry.phase != ImportPhase::Evaluation)
            return MemberAnswer::unknown(UnprovenCause::Unknown);
        MemberAnswer answer = createProvenAnswer(MemberKind::Namespace);
        answer.module = entry.module;
        return answer;
    }
    case ModuleExportKind::Indirect:
        return MemberAnswer::unknown(UnprovenCause::UnknownObject);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

MemberAnswer findNamespaceExport(const PropertyAccessContext& context, const MemberQuery& query)
{
    const Program& program = context.program;
    const Layout& layout = program.at(query.layout);
    RELEASE_ASSERT(layout.kind == LayoutKind::ModuleNamespace);
    if (query.access == Access::Store || query.access == Access::Define)
        return MemberAnswer::unknown(UnprovenCause::Unknown);

    const PropertyKey& key = program.at(query.key);
    switch (key.kind) {
    case PropertyKeyKind::WellKnownSymbol:
        if (!key.symbol || *key.symbol == WellKnownSymbol::ToStringTag)
            return MemberAnswer::unknown(UnprovenCause::Unknown);
        return createProvenAnswer(MemberKind::Absent);
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return createProvenAnswer(MemberKind::Absent);
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
        return MemberAnswer::unknown(UnprovenCause::PropertyAdded);
    case PropertyKeyKind::Text:
        break;
    }

    bool listsEveryName = exportListingOf(program, layout.namespaceOf) == FlowOwnListing::Complete;
    std::optional<uint32_t> field = layout.findField(query.key);
    if (!field) {
        if (!listsEveryName)
            return MemberAnswer::unknown(UnprovenCause::UnknownObject);
        if (isAddedByRequire(program, query.key))
            return MemberAnswer::unknown(UnprovenCause::PropertyAdded);
        return createProvenAnswer(MemberKind::Absent);
    }

    const ModuleExport& entry = program.at(layout.namespaceOf).exports[*field];
    bool mayBeAmbiguous = entry.isFromStarExport && !listsEveryName;
    if (mayBeAmbiguous)
        return MemberAnswer::unknown(UnprovenCause::UnknownObject);
    return createExportAnswer(query, entry, *field);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
