/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifier.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowResults.h"
#include "AOTLayout.h"
#include "AOTSchemaWalkers.h"
#include "AOTSlotRepresentation.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTVerifierInternal.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <tuple>
#include "AOTBitVector.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

template<typename List, typename Projection = std::identity>
std::optional<uint32_t> findUnsortedEntry(const List& list, Projection projection = { })
{
    auto lastSorted = std::ranges::adjacent_find(list, std::ranges::greater_equal { }, projection);
    if (lastSorted == list.end())
        return std::nullopt;
    return static_cast<uint32_t>(lastSorted - list.begin() + 1);
}

bool canConstruct(const Function& constructor, const Layout& layout)
{
    bool hasClassConstructorFlags = constructor.has(FunctionFlags::HasNewTarget) && !constructor.has(FunctionFlags::HasThis);
    switch (constructor.constructorForm()) {
    case ConstructorForm::Base:
        return !layout.has(LayoutFlags::HasHeritage) && (constructor.kind == FunctionKind::Plain || hasClassConstructorFlags);
    case ConstructorForm::Derived:
        return layout.has(LayoutFlags::HasHeritage) && hasClassConstructorFlags;
    case ConstructorForm::NotConstructor:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class ReferenceChecker {
public:
    ReferenceChecker(const Program& program, IRForm form, const VerifierInputs& inputs)
        : m_program(program)
        , m_form(form)
        , m_inputs(inputs)
    {
    }

    bool hasInvalidReference() const { return m_hasInvalidReference; }

    void operator()(StringID id) { check(id, m_program.strings.size()); }
    void operator()(PropertyKeyID id) { check(id, m_program.types.numberOfPropertyKeys()); }
    void operator()(FunctionID id) { check(id, m_program.functions.size()); }
    void operator()(LayoutID id) { check(id, m_program.layouts.size()); }
    void operator()(VariableID id) { check(id, m_program.variables.size()); }
    void operator()(ModuleID id) { check(id, m_program.modules.size()); }
    void operator()(ModuleFailureID id) { check(id, m_program.moduleFailures.size()); }
    void operator()(TypeID id) { check(id, m_program.types.size()); }
    void operator()(TypeSiteID id) { check(id, m_program.typeSites.size()); }
    void operator()(EvalScopeID id) { check(id, m_program.evalScopes.size()); }
    void operator()(DataID id) { check(id, m_program.data.size()); }
    void operator()(ExpectedResultID id) { check(id, m_program.expectedResults.size()); }
    void operator()(LayoutSetID id) { check(id, m_program.types.numberOfLayoutSets()); }
    void operator()(CalleeSetID id) { check(id, m_program.types.numberOfCalleeSets()); }
    void operator()(AccessSiteID id) { check(id, flowTableSize(&FlowResults::numberOfAccessSites)); }
    void operator()(AllocationSiteID id) { check(id, flowTableSize(&FlowResults::numberOfAllocationSites)); }
    void operator()(AllocationClassID id) { check(id, flowTableSize(&FlowResults::numberOfAllocationClasses)); }
    void operator()(FieldAccessID id) { check(id, placedTableSize(&PlacedTableSizes::fieldAccesses)); }
    void operator()(AllocationID id) { check(id, placedTableSize(&PlacedTableSizes::allocations)); }
    void operator()(RegExpRecordID id) { check(id, placedTableSize(&PlacedTableSizes::regExpRecords)); }
    void operator()(GuardBitID id) { check(id, placedTableSize(&PlacedTableSizes::guardBits)); }
    void operator()(SiteCacheID) { }
    void operator()(GlobalBindingID) { }

    template<typename Tag, typename Storage>
    void operator()(BuiltinTableID<Tag, Storage> id)
    {
        if (!m_program.builtins().contains(id))
            m_hasInvalidReference = true;
    }

    template<typename IDType>
    void checkIfSet(IDType id)
    {
        if (id)
            (*this)(id);
    }

private:
    std::optional<size_t> flowTableSize(unsigned (FlowResults::*size)() const) const
    {
        if (!m_inputs.flow)
            return std::nullopt;
        return (m_inputs.flow->*size)();
    }

    std::optional<size_t> placedTableSize(unsigned PlacedTableSizes::*size) const
    {
        if (m_form != IRForm::Placed)
            return std::nullopt;
        return m_inputs.placed->*size;
    }

    template<typename IDType>
    void check(IDType id, std::optional<size_t> size)
    {
        if (size && id.index() >= *size)
            m_hasInvalidReference = true;
    }

    const Program& m_program;
    IRForm m_form;
    const VerifierInputs& m_inputs;
    bool m_hasInvalidReference { false };
};

bool hasValidReferences(const Program& program, const Type& type, ReferenceChecker checker)
{
    for (StringID literal : type.stringLiterals)
        checker(literal);
    for (PropertyKeyID symbol : type.symbolLiterals)
        checker(symbol);
    for (FunctionID function : type.functions)
        checker(function);
    for (BuiltinID builtin : type.builtins)
        checker(builtin);
    for (RegExpLiteralInType literal : type.regExpLiteralSet)
        checker(literal.pattern);
    for (const ClassPart& part : type.classes) {
        checker(part.builtinClass);
        for (TypeID contents : part.writtenContents)
            checker.checkIfSet(contents);
    }
    checker.checkIfSet(type.layouts);
    checker.checkIfSet(type.environment);
    if (checker.hasInvalidReference())
        return false;
    return std::ranges::all_of(type.symbolLiterals, [&](PropertyKeyID symbol) {
        return isSymbolKind(program.at(symbol).kind);
    });
}

std::optional<InternalError> findTypeError(const Program& program, TypeID id, ReferenceChecker checker)
{
    const Type& type = program.at(id);
    if (!hasValidReferences(program, type, checker))
        return InternalError { .kind = InternalErrorKind::IdOutOfRange, .count = id.index() };

    if (type.layouts) {
        const LayoutSet& set = program.at(type.layouts);
        if (!set.listsEveryLayout() || set.written().isEmpty())
            return InternalError { .kind = InternalErrorKind::InvalidLayout, .count = id.index() };
        for (LayoutID member : set.written()) {
            if (!member || program.at(member).isEnvironment())
                return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = member, .count = id.index() };
        }
    }
    if (type.environment && !program.at(type.environment).isEnvironment())
        return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = type.environment, .count = id.index() };

    Type canonical = type;
    canonical.canonicalize();
    if (canonical != type)
        return InternalError { .kind = InternalErrorKind::InvalidLayout, .count = id.index() };
    return std::nullopt;
}

bool isValidPosition(const Program& program, const DeclaredType& declared)
{
    if (!declared.written())
        return false;
    if (!declared.has(PositionFlags::IsInt32Promise))
        return true;
    const Type& type = program.at(declared.written());
    return !declared.isExpected() && !(type.primitives & Primitive::Double) && type.integers.isSubsetOf(Range::int32());
}

bool canOutliveActivation(const Program& program, const DeclaredType& declared)
{
    std::optional<OpaqueKind> opaque = program.at(declared.written()).opaque;
    return !opaque || !isFunctionLocal(*opaque);
}

static bool listsFieldsOnce(const Layout& layout, const Vector<uint32_t>& order)
{
    BitVector isListed(layout.fields.size());
    for (uint32_t field : order) {
        if (field >= layout.fields.size() || isListed.set(field))
            return false;
    }
    return true;
}

std::optional<InternalError> findLayoutError(const Program& program, IRForm form, uint64_t maxFields, LayoutID id, const Layout& layout)
{
    bool isWireKind = static_cast<unsigned>(layout.kind) < numberOfWireEnumerators<LayoutKind>;
    bool hasValidBuiltinLayout = layout.kind == LayoutKind::BuiltinResult ? program.builtins().contains(layout.builtinLayout) : !layout.builtinLayout;
    if (!hasValidBuiltinLayout
        || !!layout.namespaceOf != (layout.kind == LayoutKind::ModuleNamespace)
        || layout.ownFieldCount > layout.fields.size()
        || findUnsortedEntry(layout.supertypes)
        || layout.supertypes.contains(LayoutID())
        || !listsFieldsOnce(layout, layout.declaredOrder)
        || (layout.constructor && layout.kind != LayoutKind::Instance)
        || (form == IRForm::AsRead && id.index() < program.numberOfLayoutsAsRead() && !isWireKind))
        return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id };
    if (isObjectLayout(layout.kind) && layout.fields.size() > maxFields)
        return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id, .limit = LimitID::FieldsInKeyOrder, .count = static_cast<uint32_t>(layout.fields.size()) };
    return std::nullopt;
}

bool mayHaveUnnamedFields(const Program& program, const Layout& layout)
{
    switch (layout.kind) {
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return true;
    case LayoutKind::BuiltinResult:
        return program.builtins().contains(layout.builtinLayout) && program.builtins().at(layout.builtinLayout).isTuple;
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool hasValidOrigin(const Program& program, const Layout& layout, uint32_t index)
{
    const Field& field = layout.fields[index];
    bool isInherited = field.flags.contains(FieldFlags::IsInherited);
    if (!field.originLayout)
        return !field.originField && !isInherited;

    const Layout& origin = program.at(field.originLayout);
    if (!field.originField || *field.originField >= origin.fields.size())
        return false;
    if (isInherited != (index >= layout.ownFieldCount))
        return false;
    return !isInherited || !origin.fields[*field.originField].flags.contains(FieldFlags::IsInherited);
}

bool hasValidKey(const Program& program, const Layout& layout, uint32_t index, bool mayBeUnnamed)
{
    const Field& field = layout.fields[index];
    if (field.computedKey) {
        const Layout& declaringLayout = field.flags.contains(FieldFlags::IsInherited) ? program.at(field.originLayout) : layout;
        return !field.key && *field.computedKey < declaringLayout.computedKeyCount;
    }
    if (!field.key)
        return mayBeUnnamed || field.flags.contains(FieldFlags::IsHidden);
    if (layout.findField(field.key) != index)
        return false;
    return layout.kind == LayoutKind::ModuleNamespace || !program.isIndexLike(field.key);
}

bool isValidField(const Program& program, const Layout& layout, uint32_t index, bool mayBeUnnamed)
{
    const Field& field = layout.fields[index];
    if (!hasValidOrigin(program, layout, index) || !hasValidKey(program, layout, index, mayBeUnnamed))
        return false;
    bool hasPrivateName = field.key && program.at(field.key).kind == PropertyKeyKind::PrivateName;
    if (field.flags.contains(FieldFlags::IsPrivate) != hasPrivateName)
        return false;
    return !field.flags.contains(FieldFlags::IsBrand) || field.flags.containsAll({ FieldFlags::IsPrivate, FieldFlags::IsHidden });
}

std::optional<InternalError> findFieldError(const Program& program, LayoutID id, const Layout& layout)
{
    bool mayBeUnnamed = mayHaveUnnamedFields(program, layout);
    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        if (!isValidField(program, layout, index, mayBeUnnamed))
            return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id, .count = index };
    }
    return std::nullopt;
}

bool hasDistinctFields(const FieldOrder& order, size_t numberOfFields)
{
    BitVector isListed(numberOfFields);
    for (uint32_t field : order.fields) {
        if (field >= numberOfFields || isListed.quickSet(field))
            return false;
    }
    return true;
}

static bool isSortedByName(const Vector<ClassMember>& members)
{
    bool hasKeys = std::ranges::all_of(members, [](const ClassMember& member) {
        return !!member.key;
    });
    return hasKeys && !findUnsortedEntry(members, [](const ClassMember& member) {
        return std::tuple { member.side, member.key, member.kind, member.function };
    });
}

static bool hasValidClassMembers(const ClassFlow& classFlow)
{
    bool namesWithoutFunctionAreUnknown = std::ranges::all_of(classFlow.namedMembers, [](const ClassMember& member) {
        return member.function || (member.mutability == MemberMutability::Unknown && member.closureUse == ClosureUse::Unknown);
    });
    return namesWithoutFunctionAreUnknown && isSortedByName(classFlow.namedMembers) && std::ranges::all_of(classFlow.members, [](const ClassMember& member) {
        return !!member.function;
    });
}

static bool hasValidOwnAccessors(const LayoutFlow& flow)
{
    return isSortedByName(flow.ownAccessors) && std::ranges::all_of(flow.ownAccessors, [](const ClassMember& accessor) {
        return accessor.function && accessor.side == ClassSide::Prototype && accessor.kind != MethodKind::Method;
    });
}

std::optional<InternalError> findLayoutFlowError(const Program& program, LayoutID id, const Layout& layout)
{
    const LayoutFlow& flow = layout.flow;
    bool hasValidBaseClass = flow.baseState == BaseState::BuiltinClass ? program.builtins().contains(flow.baseClass) : !flow.baseClass;
    if (!!flow.base != (flow.baseState == BaseState::Layout) || !hasValidBaseClass)
        return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id };

    bool hasOnlyObjectMembers = std::ranges::all_of(flow.members, [&](LayoutID member) {
        return member && !program.at(member).isEnvironment();
    });
    if (!hasOnlyObjectMembers || findUnsortedEntry(flow.members))
        return InternalError { .kind = InternalErrorKind::FlowMembersInvalid, .layout = id };

    bool hasValidOrders = std::ranges::all_of(flow.constructorOrders, [&](const FieldOrder& order) {
        return hasDistinctFields(order, layout.fields.size());
    });
    if (!hasValidOrders || !hasValidClassMembers(layout.classFlow) || !hasValidOwnAccessors(flow))
        return InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id };
    return std::nullopt;
}

template<typename Step>
std::optional<LayoutID> findCyclicLayoutChain(const Program& program, const Step& stepOf)
{
    enum class WalkState : uint8_t { Unvisited, OnPath, Done };

    auto& layouts = program.layouts;
    Vector<WalkState> states(FillWith { }, layouts.size(), WalkState::Unvisited);
    for (uint32_t index = 0; index < layouts.size(); ++index) {
        LayoutID id(index);
        for (; id && states[id.index()] == WalkState::Unvisited; id = stepOf(layouts[id.index()]))
            states[id.index()] = WalkState::OnPath;
        if (id && states[id.index()] == WalkState::OnPath)
            return id;
        for (LayoutID walked(index); walked && states[walked.index()] == WalkState::OnPath; walked = stepOf(layouts[walked.index()]))
            states[walked.index()] = WalkState::Done;
    }
    return std::nullopt;
}

} // anonymous namespace

bool hoistsFunctionsIntoVariables(ModuleKind kind) { return hoistsFunctionsIntoVariablesOf(kind); }

bool isValidPropertyKey(const PropertyKey& key)
{
    bool hasCreationSite = key.module || key.callNumber || key.site;
    switch (key.kind) {
    case PropertyKeyKind::Text:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::RegisteredSymbol:
        return key.text && !key.owner && !key.symbol && !hasCreationSite;
    case PropertyKeyKind::PrivateName:
        return key.text && key.owner && !key.symbol && !hasCreationSite;
    case PropertyKeyKind::WellKnownSymbol:
        return key.symbol && !key.text && !key.owner && !hasCreationSite;
    case PropertyKeyKind::CreatedSymbol:
        return key.module && key.callNumber && !key.owner && !key.symbol;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ProgramVerifier::ProgramVerifier(const Program& program, IRForm form, BuildContext& build, const VerifierInputs& inputs)
    : m_program(program)
    , m_form(form)
    , m_build(build)
    , m_inputs(inputs)
{
}

bool ProgramVerifier::run()
{
    if (!runRecords())
        return false;
    verifyFunctionBodies();
    return !hasFailed();
}

bool ProgramVerifier::runRecords()
{
    using Group = void (ProgramVerifier::*)();
    static constexpr Group groups[] = {
        &ProgramVerifier::verifyIDs,
        &ProgramVerifier::verifyTypes,
        &ProgramVerifier::verifyPropertyKeys,
        &ProgramVerifier::verifyDeclaredPositions,
        &ProgramVerifier::verifyLayouts,
        &ProgramVerifier::verifyClasses,
        &ProgramVerifier::verifyVariables,
        &ProgramVerifier::verifyModules,
        &ProgramVerifier::verifyModuleGraph,
        &ProgramVerifier::verifyEntries,
        &ProgramVerifier::verifyData,
        &ProgramVerifier::verifyDataTypes,
        &ProgramVerifier::verifySmallRecords,
        &ProgramVerifier::verifyFunctionHeaders,
        &ProgramVerifier::verifyDerivationChains,
    };
    for (Group verifyGroup : groups) {
        (this->*verifyGroup)();
        if (hasFailed())
            return false;
    }
    return true;
}

bool ProgramVerifier::runIDs()
{
    verifyIDs();
    if (!hasFailed())
        verifyTypes();
    return !hasFailed();
}

void ProgramVerifier::fail(InternalError&& error)
{
    m_build.diagnostics().add(error);
    m_hasFailed = true;
}

void ProgramVerifier::verifyIDs()
{
    auto verifyRecords = [&](size_t count, const auto& recordAt, const auto& createError) {
        Vector<uint32_t> invalidRecords = findIndicesInParallel(m_build.parallelLoops(), count, [&](unsigned index) {
            ReferenceChecker checker(m_program, m_form, m_inputs);
            forEachReference(recordAt(index), checker);
            return checker.hasInvalidReference();
        });
        for (uint32_t index : invalidRecords)
            fail(createError(index));
    };
    auto inRecord = [](uint32_t index) {
        return InternalError { .kind = InternalErrorKind::IdOutOfRange, .count = index };
    };
    auto verifyList = [&](const auto& list) {
        verifyRecords(list.size(), [&](unsigned index) -> const auto& { return list[index]; }, inRecord);
    };

    verifyRecords(m_program.layouts.size(), [&](unsigned index) -> const LayoutRecord& { return m_program.layouts[index]; }, [](uint32_t index) {
        return InternalError { .kind = InternalErrorKind::IdOutOfRange, .layout = LayoutID(index), .count = index };
    });
    verifyRecords(m_program.functions.size(), [&](unsigned index) -> const FunctionRecord& { return m_program.functions[index]; }, [](uint32_t index) {
        return InternalError { .kind = InternalErrorKind::IdOutOfRange, .function = FunctionID(index), .count = index };
    });
    verifyList(m_program.typeSites);
    verifyList(m_program.expectedResults);
    verifyList(m_program.variables);
    verifyList(m_program.modules);
    verifyList(m_program.moduleFailures);
    verifyList(m_program.evalScopes);
    verifyList(m_program.data);
    verifyList(m_program.entries);
    verifyList(m_program.runtimeFunctions);
    verifyList(m_program.webAssemblyModules);

    const TypeTable& types = m_program.types;
    verifyRecords(types.numberOfPropertyKeys(), [&](unsigned index) -> const PropertyKey& { return types.at(PropertyKeyID(index)); }, inRecord);
    verifyRecords(types.numberOfLayoutSets(), [&](unsigned index) -> const LayoutSet& { return types.at(LayoutSetID(index)); }, inRecord);
    verifyRecords(types.numberOfCalleeSets(), [&](unsigned index) -> const CalleeSet& { return types.at(CalleeSetID(index)); }, inRecord);
}

void ProgramVerifier::verifyTypes()
{
    Vector<InternalError> errors = collectInParallel<InternalError>(m_build.parallelLoops(), m_program.types.size(), [&](unsigned index, Vector<InternalError>& found) {
        if (auto error = findTypeError(m_program, TypeID(index), ReferenceChecker(m_program, m_form, m_inputs)))
            found.append(WTF::move(*error));
    });
    for (InternalError& error : errors)
        fail(WTF::move(error));
}

void ProgramVerifier::verifyPropertyKeys()
{
    for (unsigned index = 0; index < m_program.types.numberOfPropertyKeys(); ++index) {
        const PropertyKey& key = m_program.at(PropertyKeyID(index));
        if (!isValidPropertyKey(key) || (key.kind == PropertyKeyKind::PrivateName && m_program.at(key.owner).isEnvironment()))
            fail({ .kind = InternalErrorKind::InvalidPropertyKey, .count = index });
    }
}

void ProgramVerifier::verifyDeclaredPositions()
{
    const ParallelLoops& loops = m_build.parallelLoops();
    auto isValid = [&](const DeclaredType& declared) {
        return isValidPosition(m_program, declared);
    };

    Vector<uint32_t> invalidLayouts = findIndicesInParallel(loops, m_program.layouts.size(), [&](unsigned index) {
        return !std::ranges::all_of(m_program.layouts[index].fields, [&](const Field& field) {
            return isValid(field.declared) && canOutliveActivation(m_program, field.declared);
        });
    });
    for (uint32_t index : invalidLayouts)
        fail({ .kind = InternalErrorKind::InvalidLayout, .layout = LayoutID(index) });

    Vector<uint32_t> invalidVariables = findIndicesInParallel(loops, m_program.variables.size(), [&](unsigned index) {
        const DeclaredType& declared = m_program.variables[index].declared;
        return !isValid(declared) || !canOutliveActivation(m_program, declared);
    });
    for (uint32_t index : invalidVariables)
        fail({ .kind = InternalErrorKind::InvalidVariable, .count = index });

    Vector<uint32_t> invalidFunctions = findIndicesInParallel(loops, m_program.functions.size(), [&](unsigned index) {
        const Function& function = m_program.functions[index];
        return !std::ranges::all_of(function.captures, isValid) || !isValid(function.thisType)
            || !std::ranges::all_of(function.parameters, isValid) || !std::ranges::all_of(function.results, isValid);
    });
    for (uint32_t index : invalidFunctions)
        fail({ .kind = InternalErrorKind::EntryParametersMismatch, .function = FunctionID(index) });
}

void ProgramVerifier::verifyLayouts()
{
    uint64_t maxFields = m_build.limit(LimitID::FieldsInKeyOrder);
    Vector<uint32_t> invalidLayouts = findIndicesInParallel(m_build.parallelLoops(), m_program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        const Layout& layout = m_program.layouts[index];
        return findLayoutError(m_program, m_form, maxFields, id, layout) || findFieldError(m_program, id, layout) || findLayoutFlowError(m_program, id, layout);
    });
    for (uint32_t index : invalidLayouts) {
        LayoutID id(index);
        const Layout& layout = m_program.layouts[index];
        verifyLayout(id, layout);
        verifyFields(id, layout);
        verifyLayoutFlow(id, layout);
    }
    if (hasFailed())
        return;

    std::optional<LayoutID> cyclicLayout = findCyclicLayoutChain(m_program, [](const Layout& layout) { return layout.flow.base; });
    if (cyclicLayout)
        fail({ .kind = InternalErrorKind::InvalidLayout, .layout = *cyclicLayout });
}

void ProgramVerifier::verifyLayout(LayoutID id, const Layout& layout)
{
    if (auto error = findLayoutError(m_program, m_form, m_build.limit(LimitID::FieldsInKeyOrder), id, layout))
        fail(WTF::move(*error));
}

void ProgramVerifier::verifyFields(LayoutID id, const Layout& layout)
{
    if (auto error = findFieldError(m_program, id, layout))
        fail(WTF::move(*error));
}

void ProgramVerifier::verifyLayoutFlow(LayoutID id, const Layout& layout)
{
    if (auto error = findLayoutFlowError(m_program, id, layout))
        fail(WTF::move(*error));
}

void ProgramVerifier::verifyClasses()
{
    Vector<uint32_t> invalidClasses = findIndicesInParallel(m_build.parallelLoops(), m_program.layouts.size(), [&](unsigned index) {
        return !isValidClass(LayoutID(index), m_program.layouts[index]);
    });
    for (uint32_t index : invalidClasses)
        fail({ .kind = InternalErrorKind::InvalidClass, .layout = LayoutID(index) });
}

bool ProgramVerifier::isValidClass(LayoutID id, const Layout& layout) const
{
    if (layout.base && m_program.at(layout.base).kind != LayoutKind::Instance)
        return false;
    if (layout.kind != LayoutKind::Instance)
        return !layout.constructor && layout.staticFields.isEmpty() && !layout.has(LayoutFlags::HasHeritage);
    if (layout.constructor) {
        const Function& constructor = m_program.at(layout.constructor);
        if (constructor.constructedClass != id || !canConstruct(constructor, layout))
            return false;
    }
    return std::ranges::all_of(layout.staticFields, [&](const StaticField& field) {
        return field.variable && m_program.at(field.variable).owner == id;
    });
}

void ProgramVerifier::verifyVariables()
{
    BitVector isStaticField(m_program.variables.size());
    for (const Layout& layout : m_program.layouts) {
        for (const StaticField& field : layout.staticFields)
            isStaticField.quickSet(field.variable.index());
    }

    auto isValid = [&](unsigned index) {
        const Variable& variable = m_program.variables[index];
        bool startsAsFunction = variable.initialization == VariableInitialization::Function;
        if (!variable.module || !!variable.function != startsAsFunction)
            return false;
        if (startsAsFunction && !hoistsFunctionsIntoVariables(m_program.at(variable.module).kind))
            return false;
        return !variable.owner || (m_program.at(variable.owner).kind == LayoutKind::Instance && isStaticField.quickGet(index));
    };
    for (unsigned index = 0; index < m_program.variables.size(); ++index) {
        if (!isValid(index))
            fail({ .kind = InternalErrorKind::InvalidVariable, .function = m_program.variables[index].function, .count = index });
    }
}

bool verifyProgram(const Program& program, IRForm form, BuildContext& build, const VerifierInputs& inputs)
{
    RELEASE_ASSERT(form != IRForm::Placed || inputs.placed);
    return ProgramVerifier(program, form, build, inputs).run();
}

bool verifyRecords(const Program& program, IRForm form, BuildContext& build, const VerifierInputs& inputs)
{
    return ProgramVerifier(program, form, build, inputs).runRecords();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
