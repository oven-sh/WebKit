/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramVerifier.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBody.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTPhaseTimes.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTSortedSet.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <compare>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class BodyRule : uint8_t { Absent, Optional, Required };

struct ModuleKindRules {
    BodyRule body { BodyRule::Absent };
    OptionSet<FunctionFlags> forbiddenBodyFlags;
    bool hasExportTable { false };
    bool hasRequests { false };
    bool hasRequiredNames { false };
    bool canHaveData { false };
    bool hasRuntimeModule { false };
    bool hasReferrer { false };
};

struct VerifiedRequestTargets {
    const Vector<ModuleRequest>& requests;

    size_t size() const { return requests.size(); }
    ModuleID operator[](size_t index) const { return requests[index].target; }
};

} // anonymous namespace

bool hoistsFunctionsIntoVariables(ModuleKind kind) { return hoistsFunctionsIntoVariablesOf(kind); }

static ModuleKindRules rulesOf(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
        return { .body = BodyRule::Optional, .hasExportTable = true, .hasRequests = true, .hasRequiredNames = true };
    case ModuleKind::CommonJs:
        return { .body = BodyRule::Required, .forbiddenBodyFlags = FunctionFlags::HasThis, .hasRequests = true };
    case ModuleKind::Data:
        return { .body = BodyRule::Optional, .forbiddenBodyFlags = { FunctionFlags::IsAsync, FunctionFlags::IsGenerator }, .hasExportTable = true, .canHaveData = true };
    case ModuleKind::RuntimeSource:
        return { .body = BodyRule::Required, .forbiddenBodyFlags = { FunctionFlags::IsAsync, FunctionFlags::IsGenerator }, .hasRuntimeModule = true };
    case ModuleKind::Runtime:
        return { .body = BodyRule::Absent, .hasRuntimeModule = true };
    case ModuleKind::External:
        return { .body = BodyRule::Absent, .hasReferrer = true };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isUncompiledCode(const Module& module)
{
    return module.kind == ModuleKind::External && module.externalKind == ExternalKind::UncompiledCode;
}

static bool findsNamesAtRunTime(const Module& module)
{
    return isUncompiledCode(module) || module.kind == ModuleKind::Runtime || module.requests.containsIf([](const ModuleRequest& request) {
        return request.isDynamicStarExport;
    });
}

static BitVector findModulesWithVariableThatHoldsMock(const Program& program)
{
    BitVector modules(program.modules.size());
    for (const Variable& variable : program.variables) {
        if (variable.flags.contains(VariableFlags::HoldsMock))
            modules.quickSet(variable.module.index());
    }
    return modules;
}

static BitVector findModulesWithInstances(const Program& program)
{
    BitVector modules(program.modules.size());
    for (const Variable& variable : program.variables) {
        if (variable.flags.contains(VariableFlags::HoldsInstance))
            modules.quickSet(variable.module.index());
    }
    return modules;
}

static bool hasRequiredIDs(const Module& module)
{
    auto isSet = [](auto id) { return !!id; };
    return module.path
        && module.displayPath
        && std::ranges::all_of(module.requests, isSet, &ModuleRequest::target)
        && std::ranges::all_of(module.exports, isSet, &ModuleExport::name)
        && std::ranges::all_of(module.syntheticExports, isSet, &SyntheticExport::name)
        && std::ranges::all_of(module.requiredNames, isSet, &RequiredName::module)
        && std::ranges::all_of(module.requiredNames, isSet, &RequiredName::name)
        && std::ranges::all_of(module.requiredNames, [](const RequiredName& required) { return !required.exportName == !required.specifier; });
}

static bool isCommonJsRule(SyntheticExportRule rule)
{
    return rule == SyntheticExportRule::CommonJs || rule == SyntheticExportRule::CommonJsIgnoringAnnotation;
}

static bool hasValidInterop(const Program& program, const Module& module)
{
    if (module.requireResult == RequireResult::NamespaceIfImported && module.kind != ModuleKind::Data)
        return false;
    if (module.flags.contains(ModuleFlags::LoaderReturnsNamespace) && module.kind != ModuleKind::Data)
        return false;
    switch (module.kind) {
    case ModuleKind::SourceText:
        if (module.requireResult == RequireResult::ModuleExports)
            return false;
        return module.exportRule == SyntheticExportRule::None || (isCommonJsRule(module.exportRule) && commonJsModuleCapturedBy(program, module));
    case ModuleKind::Data:
        return module.exportRule == SyntheticExportRule::None && module.requireResult != RequireResult::ModuleExports;
    case ModuleKind::CommonJs:
        if (module.flags.contains(ModuleFlags::IsLibrary))
            return isCommonJsRule(module.exportRule);
        return (module.exportRule == SyntheticExportRule::None || isCommonJsRule(module.exportRule)) && module.syntheticExports.isEmpty();
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
        return module.exportRule == SyntheticExportRule::RuntimeModule;
    case ModuleKind::External:
        switch (module.externalKind) {
        case ExternalKind::UncompiledCode:
            return module.exportRule == SyntheticExportRule::None && module.syntheticExports.isEmpty();
        case ExternalKind::HostValue:
            return module.exportRule != SyntheticExportRule::None;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool handsOnNameThatRunFinds(const Program& program, const Module& module, StringID name)
{
    auto isLess = [](auto one, auto other) {
        return compareProgramStrings(one, other) == std::strong_ordering::less;
    };
    auto textOfName = [&](const ModuleExport& entry) {
        return program.string(entry.name);
    };
    auto entry = std::ranges::lower_bound(module.exports, program.string(name), isLess, textOfName);
    return entry != module.exports.end() && entry->kind == ModuleExportKind::Indirect && compareProgramStrings(program.string(entry->name), program.string(name)) == std::strong_ordering::equal;
}

static bool isOmittedDataExport(const Program& program, const ModuleExport& entry)
{
    if (entry.kind != ModuleExportKind::Variable || !entry.variable)
        return false;
    ModuleID owner = program.at(entry.variable).module;
    return owner && program.at(owner).kind == ModuleKind::Data;
}

static bool isInstanceLayout(const Program& program, LayoutID id, const BitVector& modulesWithInstances)
{
    const Layout& layout = program.at(id);
    return layout.kind == LayoutKind::Environment && layout.module && modulesWithInstances.quickGet(layout.module.index());
}

static bool hasOneBinding(const Program& program, const ModuleExport& entry, const BitVector& modulesWithVariableThatHoldsMock, const BitVector& modulesWithInstances)
{
    if (entry.isOmitted && !isOmittedDataExport(program, entry))
        return false;
    if ((entry.layout || entry.field) && entry.kind != ModuleExportKind::Field)
        return false;
    if (entry.link && entry.kind != ModuleExportKind::Field && entry.kind != ModuleExportKind::Namespace)
        return false;
    if (entry.requested && entry.kind != ModuleExportKind::Namespace && entry.kind != ModuleExportKind::Variable)
        return false;
    switch (entry.kind) {
    case ModuleExportKind::Variable:
        return entry.variable && !entry.module && !entry.importName == !entry.requested && entry.phase == ImportPhase::Evaluation;
    case ModuleExportKind::Namespace:
        return entry.module && !entry.variable && (!entry.link || entry.requested) && !entry.importName == !entry.requested;
    case ModuleExportKind::Indirect:
        return entry.module && entry.importName && !entry.variable && entry.phase == ImportPhase::Evaluation && (findsNamesAtRunTime(program.at(entry.module)) || modulesWithVariableThatHoldsMock.quickGet(entry.module.index()) || handsOnNameThatRunFinds(program, program.at(entry.module), entry.importName));
    case ModuleExportKind::Field:
        return entry.layout && isInstanceLayout(program, entry.layout, modulesWithInstances) && entry.field && *entry.field < program.at(entry.layout).fields.size()
            && !entry.variable && entry.link.has_value() == !!entry.module && !entry.module == !entry.importName && entry.phase == ImportPhase::Evaluation;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasDuplicateRequests(const Vector<ModuleRequest>& requests)
{
    auto keys = WTF::map(requests, [](const ModuleRequest& request) {
        return std::tuple { request.target, request.phase, request.kind, request.isHoisted };
    });
    AOT::sort(keys);
    return std::ranges::adjacent_find(keys) != keys.end();
}

static bool hasValidPlacesOfStars(const Module& module)
{
    size_t numberOfPlaces = std::ranges::count_if(module.requests, [](const ModuleRequest& request) {
        return request.starPlace.has_value();
    });
    if (!numberOfPlaces)
        return true;
    BitVector isTaken(numberOfPlaces);
    std::optional<uint32_t> last;
    bool isInOrder = true;
    bool everyStarIsFollowed = true;
    bool someStarIsFollowed = false;
    for (const ModuleRequest& request : module.requests) {
        if (request.isDynamicStarExport && !request.starPlace)
            return false;
        if (!request.starPlace)
            continue;
        if (request.kind != ModuleRequestKind::Import || request.isHoisted || *request.starPlace >= numberOfPlaces || isTaken.quickSet(*request.starPlace))
            return false;
        isInOrder &= !last || *last < *request.starPlace;
        last = request.starPlace;
        everyStarIsFollowed &= request.isDynamicStarExport;
        someStarIsFollowed |= request.isDynamicStarExport;
    }
    return someStarIsFollowed && (!isInOrder || !everyStarIsFollowed);
}

static bool isValidRequest(const ModuleRequest& request)
{
    if (request.isHoisted && (request.kind != ModuleRequestKind::Import || request.phase != ImportPhase::Evaluation || request.isDynamicStarExport))
        return false;
    switch (request.kind) {
    case ModuleRequestKind::Import:
        return request.completes || request.phase == ImportPhase::Defer;
    case ModuleRequestKind::Require:
        return request.phase == ImportPhase::Evaluation && !request.isDynamicStarExport;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isFileOfProgram(const Module& module)
{
    return (module.kind == ModuleKind::SourceText || module.kind == ModuleKind::CommonJs) && !module.flags.contains(ModuleFlags::IsLibrary);
}

static bool fillsItsVariablesWhenItLoads(const Module& module)
{
    return isFileOfProgram(module) || module.exportRule == SyntheticExportRule::RuntimeModule;
}

static bool hasValidExportsVariable(const Program& program, const Module& module)
{
    if (!module.exportsVariable)
        return true;
    return module.kind == ModuleKind::CommonJs && isFileOfProgram(module) && program.at(module.exportsVariable).initialization == VariableInitialization::Undefined;
}

template<typename Export>
static std::optional<InternalErrorKind> findNameOrderError(const Program& program, const Vector<Export>& exports)
{
    for (size_t index = 1; index < exports.size(); ++index) {
        std::strong_ordering order = compareProgramStrings(program.string(exports[index - 1].name), program.string(exports[index].name));
        if (order == std::strong_ordering::equal)
            return InternalErrorKind::DuplicateListEntry;
        if (order == std::strong_ordering::greater)
            return InternalErrorKind::UnsortedList;
    }
    return std::nullopt;
}

static bool hasValidBundlerHelperFlag(const Program& program, const Module& module)
{
    bool isBundlerHelper = module.flags.contains(ModuleFlags::IsBundlerHelper);
    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs: {
        bool hasBodyOfLibrary = module.body && program.at(module.body).role == FunctionRole::Library;
        if (!isBundlerHelper)
            return !hasBodyOfLibrary;
        return (!module.body || hasBodyOfLibrary) && !module.flags.containsAny({ ModuleFlags::IsLibrary, ModuleFlags::HasNoSideEffects });
    }
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return !isBundlerHelper;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<InternalErrorKind> findModuleError(const Program& program, const Module& module, bool hasValidBody, const BitVector& modulesWithVariableThatHoldsMock, const BitVector& modulesWithInstances)
{
    ModuleKindRules rules = rulesOf(module.kind);
    bool isLibrary = module.flags.contains(ModuleFlags::IsLibrary);
    bool hasValidLists = (rules.hasExportTable ? module.syntheticExports.isEmpty() || module.exportRule != SyntheticExportRule::None : module.exports.isEmpty())
        && (rules.hasRequests || module.requests.isEmpty())
        && (rules.hasRequiredNames || module.requiredNames.isEmpty())
        && std::ranges::all_of(module.requests, isValidRequest)
        && std::ranges::is_partitioned(module.requests, &ModuleRequest::isHoisted)
        && hasValidPlacesOfStars(module);
    bool hasValidLibraryFlag = !isLibrary || (rules.body != BodyRule::Absent && !module.body && module.exports.isEmpty());
    bool hasValidImportOnlyFlag = !module.flags.contains(ModuleFlags::IsLoadedByImportOnly) || module.runtimeModule;
    bool hasValidFlagsOfFiles = !module.flags.containsAny({ ModuleFlags::HasNoSideEffects, ModuleFlags::CachesEvaluationError, ModuleFlags::ReachesModuleObjects }) || isFileOfProgram(module);
    bool hasValidRuntimeModule = module.runtimeModule
        ? rules.hasRuntimeModule && program.builtins().contains(module.runtimeModule)
        : !rules.hasRuntimeModule || isLibrary;
    bool hasValidLinks = (!!module.referrer == rules.hasReferrer || (module.referrer && module.kind == ModuleKind::CommonJs))
        && (!module.data || rules.canHaveData)
        && (!module.typeAttribute || isUncompiledCode(module))
        && !!module.namespaceLayout == hasNamespaceLayout(module.kind);
    auto isBound = [&](const ModuleExport& entry) {
        return hasOneBinding(program, entry, modulesWithVariableThatHoldsMock, modulesWithInstances);
    };
    if (!hasValidBody || !hasRequiredIDs(module) || !hasValidLists || !hasValidLibraryFlag || !hasValidImportOnlyFlag || !hasValidFlagsOfFiles || !hasValidExportsVariable(program, module) || !hasValidRuntimeModule || !hasValidLinks
        || !hasValidBundlerHelperFlag(program, module) || !hasValidInterop(program, module) || !std::ranges::all_of(module.exports, isBound))
        return InternalErrorKind::InvalidModule;
    if (hasDuplicateRequests(module.requests))
        return InternalErrorKind::DuplicateListEntry;
    if (auto kind = findNameOrderError(program, module.exports))
        return kind;
    return findNameOrderError(program, module.syntheticExports);
}

void ProgramVerifier::verifyModules()
{
    auto& modules = m_program.modules;
    BitVector modulesWithVariableThatHoldsMock = findModulesWithVariableThatHoldsMock(m_program);
    BitVector modulesWithInstances = findModulesWithInstances(m_program);
    Vector<uint32_t> invalidModules = findIndicesInParallel(m_build.parallelLoops(), modules.size(), [&](unsigned index) {
        return findModuleError(m_program, modules[index], hasValidBody(modules[index]), modulesWithVariableThatHoldsMock, modulesWithInstances).has_value();
    });
    for (uint32_t index : invalidModules)
        verifyModule(ModuleID(index), modules[index]);
    if (hasFailed())
        return;

    BitVector usedPaths(m_program.strings.size());
    BitVector usedBodies(m_program.functions.size());
    BitVector usedExportsVariables(m_program.variables.size());
    for (unsigned index = 0; index < modules.size(); ++index) {
        const Module& module = modules[index];
        if (answersToPath(m_program, module) && usedPaths.quickSet(module.path.index()))
            fail({ .kind = InternalErrorKind::InvalidModule, .count = index });
        if (module.body && usedBodies.quickSet(module.body.index()))
            fail({ .kind = InternalErrorKind::InvalidModule, .function = module.body, .count = index });
        if (module.exportsVariable && usedExportsVariables.quickSet(module.exportsVariable.index()))
            fail({ .kind = InternalErrorKind::InvalidModule, .count = index });
    }
}

void ProgramVerifier::verifyModule(ModuleID id, const Module& module)
{
    if (auto kind = findModuleError(m_program, module, hasValidBody(module), findModulesWithVariableThatHoldsMock(m_program), findModulesWithInstances(m_program)))
        fail({ .kind = *kind, .count = id.index() });
}

bool ProgramVerifier::hasValidBody(const Module& module) const
{
    ModuleKindRules rules = rulesOf(module.kind);
    const Function* body = module.body ? &m_program.at(module.body) : nullptr;
    if (module.flags.contains(ModuleFlags::HasTopLevelAwait) != (body && body->has(FunctionFlags::IsAsync)))
        return false;
    if (!body)
        return rules.body != BodyRule::Required || module.flags.contains(ModuleFlags::IsLibrary);

    auto isAnything = [](TypeID type) { return type == TypeTable::anything; };
    return rules.body != BodyRule::Absent
        && body->captures.isEmpty()
        && body->parameters.isEmpty()
        && body->results.size() == 1
        && isAnything(body->results[0].written())
        && !body->flags.containsAny(rules.forbiddenBodyFlags);
}

template<typename Functor>
static void forEachNamedModule(const Program& program, const Module& module, const Functor& functor)
{
    for (auto& entry : module.exports) {
        ModuleID named = entry.module ? entry.module : entry.layout ? program.at(entry.layout).module : program.at(entry.variable).module;
        if (named && &program.at(named) != &module)
            functor(named);
    }
    for (auto& required : module.requiredNames)
        functor(required.module);
}

namespace {

struct NameFurtherOff {
    uint32_t holder;
    ModuleID named;
};

} // anonymous namespace

static Vector<NameFurtherOff> findNamesFurtherOff(const ParallelLoops& loops, unsigned numberOfModules, const auto& targetsOf, const auto& forEachNamed)
{
    return collectInParallel<NameFurtherOff>(loops, numberOfModules, [&](unsigned index, Vector<NameFurtherOff>& found) {
        auto targets = targetsOf(index);
        Vector<ModuleID, 16> sortedTargets;
        for (size_t target = 0; target < targets.size(); ++target)
            sortedTargets.append(targets[target]);
        AOT::sort(sortedTargets);
        forEachNamed(index, [&](ModuleID named) {
            if (!std::ranges::binary_search(sortedTargets, named))
                found.append({ index, named });
        });
    });
}

static Vector<uint32_t> findUnreachedNames(const ParallelLoops& loops, unsigned numberOfModules, const auto& targetsOf, std::span<const NameFurtherOff> namesFurtherOff)
{
    constexpr size_t wordsPerSlice = 8;
    constexpr size_t namesPerSlice = wordsPerSlice * 64;
    constexpr uint32_t isNotNamed = UINT32_MAX;
    if (namesFurtherOff.empty())
        return { };

    Vector<uint32_t> rankOfModule(FillWith { }, numberOfModules, isNotNamed);
    for (const NameFurtherOff& name : namesFurtherOff)
        rankOfModule[name.named.index()] = 0;
    Vector<uint32_t> namedModules;
    for (uint32_t module = 0; module < numberOfModules; ++module) {
        if (rankOfModule[module] == isNotNamed)
            continue;
        rankOfModule[module] = static_cast<uint32_t>(namedModules.size());
        namedModules.append(module);
    }

    auto components = computeStronglyConnectedComponents(numberOfModules, targetsOf);
    auto& componentOf = components.componentOf;
    struct EdgeBetweenComponents {
        uint32_t from;
        uint32_t to;
    };
    Vector<EdgeBetweenComponents> edgesInOrderOfComponents;
    Vector<uint32_t> lastRequesters(FillWith { }, components.numberOfComponents, UINT32_MAX);
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        for (uint32_t module : components.membersOf(component)) {
            auto targets = targetsOf(module);
            for (size_t target = 0; target < targets.size(); ++target) {
                uint32_t requestedComponent = componentOf[targets[target].index()];
                if (requestedComponent != component && std::exchange(lastRequesters[requestedComponent], component) != component)
                    edgesInOrderOfComponents.append({ component, requestedComponent });
            }
        }
    }

    size_t numberOfSlices = (namedModules.size() + namesPerSlice - 1) / namesPerSlice;
    Vector<Vector<uint32_t>> holdersBySlice(numberOfSlices);
    forEachIndexInParallel(loops, static_cast<unsigned>(numberOfSlices), [&](unsigned slice) {
        size_t firstRank = slice * namesPerSlice;
        size_t endRank = std::min(namedModules.size(), firstRank + namesPerSlice);
        Vector<uint64_t> reachedNames(FillWith { }, components.numberOfComponents * wordsPerSlice, static_cast<uint64_t>(0));
        auto wordOf = [&](uint32_t component, size_t rank) -> uint64_t& {
            return reachedNames[component * wordsPerSlice + (rank - firstRank) / 64];
        };
        for (size_t rank = firstRank; rank < endRank; ++rank)
            wordOf(componentOf[namedModules[rank]], rank) |= 1ull << (rank % 64);
        std::span<uint64_t> words = reachedNames.mutableSpan();
        for (EdgeBetweenComponents edge : edgesInOrderOfComponents) {
            std::array<uint64_t, wordsPerSlice> brought;
            std::ranges::copy(words.subspan(edge.to * wordsPerSlice).template first<wordsPerSlice>(), brought.begin());
            std::span<uint64_t, wordsPerSlice> joined = words.subspan(edge.from * wordsPerSlice).template first<wordsPerSlice>();
            for (size_t word = 0; word < wordsPerSlice; ++word)
                joined[word] |= brought[word];
        }
        for (const NameFurtherOff& name : namesFurtherOff) {
            size_t rank = rankOfModule[name.named.index()];
            if (rank >= firstRank && rank < endRank && !(wordOf(componentOf[name.holder], rank) >> (rank % 64) & 1))
                holdersBySlice[slice].append(name.holder);
        }
    });

    Vector<uint32_t> holders;
    for (const Vector<uint32_t>& holdersOfSlice : holdersBySlice)
        holders.appendVector(holdersOfSlice);
    AOT::sort(holders);
    removeRepeatedElements(holders);
    return holders;
}

void MentionsInCode::beginFunction(const Function& function)
{
    m_file = function.module && isFileOfProgram(m_program.at(function.module)) ? function.module : ModuleID();
    m_first = m_found.size();
    m_mentionsOfLongList.clear();
}

ALWAYS_INLINE_INTO_OTHER_FILES void MentionsInCode::add(MentionInCode::Kind kind, ModuleID other)
{
    constexpr size_t mostMentionsGoneThrough = 16;
    auto keyOf = [](const MentionInCode& mention) {
        return static_cast<uint64_t>(mention.kind) << 32 | mention.other.index();
    };
    if (other.index() >= m_program.modules.size() || other == m_file)
        return;
    MentionInCode mention { kind, m_file, other };
    std::span<const MentionInCode> listed = m_found.span().subspan(m_first);
    if (listed.size() <= mostMentionsGoneThrough) {
        if (std::ranges::contains(listed, mention))
            return;
    } else {
        if (m_mentionsOfLongList.isEmpty()) {
            for (const MentionInCode& earlier : listed)
                m_mentionsOfLongList.add(keyOf(earlier));
        }
        if (!m_mentionsOfLongList.add(keyOf(mention)).isNewEntry)
            return;
    }
    m_found.append(mention);
}

ALWAYS_INLINE_INTO_OTHER_FILES void MentionsInCode::noteWhatValueNames(const BodyValue& value)
{
    if (!m_file)
        return;
    auto isTarget = [](ModuleReferenceKind kind) {
        return kind == ModuleReferenceKind::Target;
    };
    value.forEachReference([&](auto reference, auto... role) -> void {
        using Reference = decltype(reference);
        if constexpr (std::is_same_v<Reference, VariableID>) {
            if (reference.index() >= m_program.variables.size())
                return;
            if (ModuleID owner = m_program.at(reference).module; owner && fillsItsVariablesWhenItLoads(m_program.at(owner)))
                add(MentionInCode::Kind::NamesVariable, owner);
        } else if constexpr (std::is_same_v<Reference, ModuleID>) {
            if (isTarget(role...))
                add(MentionInCode::Kind::LoadsModule, reference);
        }
    });
}

void MentionsInCode::noteWhatFunctionNames(const Function& function)
{
    beginFunction(function);
    if (!m_file || !function.hasBody())
        return;
    const Body& body = function.body;
    BodyBlock block = body.entry();
    for (uint32_t blockIndex = 0; blockIndex < body.numberOfBlocks(); ++blockIndex) {
        BodyValues::Iterator cursor = block.values().begin();
        for (uint32_t remaining = block.numberOfValues(); remaining--; ++cursor)
            noteWhatValueNames(*cursor);
        block = BodyBlock(BodyTerminator(cursor.words(), &body.unit(), block.id()).end(), nullptr, &body.unit(), BlockID(blockIndex + 1), block.endOfValues());
    }
}

static void addModulesWhoseBodiesLieInOtherFiles(const Program& program, Vector<MentionInCode>& mentions)
{
    Vector<ModuleID> moduleWithBody(program.functions.size());
    for (unsigned index = 0; index < program.modules.size(); ++index) {
        if (FunctionID body = program.modules[index].body)
            moduleWithBody[body.index()] = ModuleID(index);
    }
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        ModuleID file = function.module;
        ModuleID beside = moduleWithBody[index];
        if (!beside || !file || beside == file || !function.hasBody() || !isFileOfProgram(program.at(file)))
            continue;
        mentions.append({ MentionInCode::Kind::LoadsModule, file, beside });
        mentions.append({ MentionInCode::Kind::LoadsModule, beside, file });
    }
}

void ProgramVerifier::verifyModuleGraph()
{
    auto& modules = m_program.modules;
    StepTimes steps(m_build, "ReadAndVerify: module graph"_s);
    for (unsigned index = 0; index < modules.size(); ++index) {
        if (modules[index].flags.contains(ModuleFlags::IsLibrary) && !modules[index].requests.isEmpty())
            fail({ .kind = InternalErrorKind::InvalidModule, .count = index });
    }

    const ParallelLoops& loops = m_build.parallelLoops();
    auto targetsOfRequests = [&](unsigned index) {
        return VerifiedRequestTargets { modules[index].requests };
    };
    Vector<NameFurtherOff> namesInTables = findNamesFurtherOff(loops, modules.size(), targetsOfRequests, [&](unsigned index, const auto& functor) {
        forEachNamedModule(m_program, modules[index], functor);
    });
    steps.didFinish("names that lie further off"_s);

    for (uint32_t index : findUnreachedNames(loops, modules.size(), targetsOfRequests, namesInTables.span()))
        fail({ .kind = InternalErrorKind::InvalidModule, .count = index });
    steps.didFinish("what requests lead to"_s);
    verifyModuleFailures();
}

void ProgramVerifier::verifyModulesNamedInBodies(Vector<MentionInCode>&& mentionsInBodies)
{
    auto& modules = m_program.modules;
    const ParallelLoops& loops = m_build.parallelLoops();
    StepTimes steps(m_build, "ReadAndVerify: modules named in bodies"_s);
    Vector<MentionInCode> mentions = WTF::move(mentionsInBodies);
    addModulesWhoseBodiesLieInOtherFiles(m_program, mentions);
    sortAndRemoveDuplicates(loops, mentions);
#if ASSERT_ENABLED || ASAN_ENABLED
    constexpr bool isCheckingBuild = true;
#else
    constexpr bool isCheckingBuild = false;
#endif
    if (isCheckingBuild || m_build.validates()) {
        MentionsInCode mentionsOfWholeBodies(m_program);
        for (const Function& function : m_program.functions)
            mentionsOfWholeBodies.noteWhatFunctionNames(function);
        Vector<MentionInCode> expected = mentionsOfWholeBodies.take();
        addModulesWhoseBodiesLieInOtherFiles(m_program, expected);
        sortAndRemoveDuplicates(loops, expected);
        RELEASE_ASSERT(mentions == expected);
    }
    steps.didFinish("mentions in code"_s);

    auto mentionsOf = [&](MentionInCode::Kind kind, unsigned index) {
        return std::ranges::equal_range(mentions, std::pair { kind, ModuleID(index) }, { }, [](const MentionInCode& mention) {
            return std::pair { mention.kind, mention.file };
        });
    };
    Vector<Vector<ModuleID>> targetsOfRequestsAndCode(modules.size());
    forEachIndexInParallel(loops, modules.size(), [&](unsigned index) {
        Vector<ModuleID> found = WTF::map(modules[index].requests, [](const ModuleRequest& request) {
            return request.target;
        });
        for (const MentionInCode& mention : mentionsOf(MentionInCode::Kind::LoadsModule, index))
            found.append(mention.other);
        targetsOfRequestsAndCode[index] = WTF::move(found);
    });
    auto targetsOfCode = [&](unsigned index) {
        return targetsOfRequestsAndCode[index].span();
    };
    Vector<NameFurtherOff> namesInCode = findNamesFurtherOff(loops, modules.size(), targetsOfCode, [&](unsigned index, const auto& functor) {
        for (const MentionInCode& mention : mentionsOf(MentionInCode::Kind::NamesVariable, index))
            functor(mention.other);
    });
    steps.didFinish("names that lie further off"_s);

    for (uint32_t index : findUnreachedNames(loops, modules.size(), targetsOfCode, namesInCode.span()))
        fail({ .kind = InternalErrorKind::InvalidModule, .count = index });
    steps.didFinish("what code leads to"_s);
}

void ProgramVerifier::verifyModuleFailures()
{
    for (unsigned index = 0; index < m_program.moduleFailures.size(); ++index) {
        const ModuleFailure& failure = m_program.moduleFailures[index];
        if (!failure.importer || !failure.specifier || (failure.exportSpecifier && !failure.exporter) || (failure.failedLoadRoot && failure.exporter))
            fail({ .kind = InternalErrorKind::InvalidModuleFailure, .count = index });
    }
}

static bool isValidHostRead(const BuiltinTable& builtins, const HostRead& read)
{
    if (!read.exportName)
        return false;
    if (!read.builtin)
        return !read.parameter;
    if (!builtins.contains(read.builtin) || read.parameter > BuiltinOperand::maxArgumentIndex)
        return false;
    BuiltinOperand argument = BuiltinOperand::argument(read.parameter);
    return std::ranges::any_of(builtins.overloadsOf(builtins.canonicalID(read.builtin)), [&](const BuiltinOverload& overload) {
        return overload.mode == BuiltinCallMode::Call && builtins.parameterFor(overload, argument);
    });
}

void ProgramVerifier::verifyEntries()
{
    if (m_program.entries.isEmpty())
        fail({ .kind = InternalErrorKind::NoEntryPoint });
    BitVector isEntry(m_program.modules.size());
    for (unsigned index = 0; index < m_program.entries.size(); ++index) {
        const Entry& entry = m_program.entries[index];
        bool hasValidHostReads = std::ranges::all_of(entry.hostReads, [&](const HostRead& read) {
            return isValidHostRead(m_program.builtins(), read);
        });
        if (!entry.module || !hasValidHostReads) {
            fail({ .kind = InternalErrorKind::InvalidEntry, .count = index });
            continue;
        }
        if (isEntry.quickSet(entry.module.index()))
            fail({ .kind = InternalErrorKind::DuplicateListEntry, .count = index });
    }
}

static constexpr uint32_t maxNestingDepthOfDataTree = 2000;

static bool isDecimalInteger(std::span<const uint8_t> text)
{
    if (!text.empty() && text.front() == '-')
        text = text.subspan(1);
    return !text.empty() && std::ranges::all_of(text, isASCIIDigit<uint8_t>);
}

static bool hasObjectKind(const Program& program, LayoutID layout)
{
    return layout && isObjectLayout(program.at(layout).kind);
}

static bool hasValidMembers(const Program& program, const DataNode::Object& object, Vector<PropertyKeyID, 16>& keys)
{
    if (!hasObjectKind(program, object.layout))
        return false;

    const Layout& layout = program.at(object.layout);
    bool hasOnlyFields = !canHaveExtraKeysOf(layout.kind);
    keys.shrink(0);
    for (const DataMember& member : object.members) {
        if (!member.key || member.value.is<DataNode::Hole>() || (hasOnlyFields && !layout.findField(member.key)))
            return false;
        keys.append(member.key);
    }
    AOT::sort(keys);
    return std::ranges::adjacent_find(keys) == keys.end();
}

static bool isValidDataNode(const Program& program, const DataNode& node, Vector<PropertyKeyID, 16>& keys)
{
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
    case DataNode::Kind::Boolean:
    case DataNode::Kind::Number:
        return true;
    case DataNode::Kind::String:
        return !!node.as<DataNode::String>().value;
    case DataNode::Kind::BigInt: {
        StringID digits = node.as<DataNode::BigInt>().digits;
        return digits && isDecimalInteger(program.string(digits));
    }
    case DataNode::Kind::Array:
        return !!node.as<DataNode::Array>().contents;
    case DataNode::Kind::Object:
        return hasValidMembers(program, node.as<DataNode::Object>(), keys);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isValidDataTree(const Program& program, const DataNode& root)
{
    struct NodeAtDepth {
        const DataNode* node;
        uint32_t depth;
    };

    if (root.is<DataNode::Hole>())
        return false;
    Vector<PropertyKeyID, 16> keys;
    Vector<NodeAtDepth, 32> stack;
    stack.append({ &root, 1 });
    while (!stack.isEmpty()) {
        auto [node, depth] = stack.takeLast();
        if (!isValidDataNode(program, *node, keys))
            return false;
        switch (node->kind()) {
        case DataNode::Kind::Array:
            if (depth > maxNestingDepthOfDataTree)
                return false;
            for (const DataNode& element : node->as<DataNode::Array>().elements)
                stack.append({ &element, depth + 1 });
            break;
        case DataNode::Kind::Object:
            if (depth > maxNestingDepthOfDataTree)
                return false;
            for (const DataMember& member : node->as<DataNode::Object>().members)
                stack.append({ &member.value, depth + 1 });
            break;
        case DataNode::Kind::Undefined:
        case DataNode::Kind::Null:
        case DataNode::Kind::Hole:
        case DataNode::Kind::Boolean:
        case DataNode::Kind::Number:
        case DataNode::Kind::String:
        case DataNode::Kind::BigInt:
            break;
        }
    }
    return true;
}

void ProgramVerifier::verifyData()
{
    auto& data = m_program.data;
    Vector<uint32_t> invalidRecords = findIndicesInParallel(m_build.parallelLoops(), data.size(), [&](unsigned index) {
        return !isValidDataTree(m_program, data[index].root);
    });
    for (uint32_t index : invalidRecords)
        verifyDataNode(DataID(index), data[index].root);
}

void ProgramVerifier::verifyDataNode(DataID id, const DataNode& root)
{
    if (!isValidDataTree(m_program, root))
        fail({ .kind = InternalErrorKind::InvalidData, .count = id.index() });
}

static bool hasValidPrivateHolder(const Program& program, const EvalBinding& binding)
{
    if (!binding.operand)
        return false;
    if (!binding.layout)
        return !binding.field;
    const Layout& layout = program.at(binding.layout);
    if (!binding.field || *binding.field >= layout.fields.size())
        return false;
    if (layout.kind == LayoutKind::Environment)
        return binding.kind == EvalBindingKind::PrivateName;
    return layout.kind == LayoutKind::Instance && layout.fields[*binding.field].flags.contains(FieldFlags::IsPrivate);
}

static bool hasValidStorage(const Program& program, const EvalBinding& binding)
{
    if (!!binding.variable == binding.operand.has_value())
        return false;
    if (!binding.layout)
        return !binding.field;
    const Layout& layout = program.at(binding.layout);
    return binding.operand && layout.kind == LayoutKind::Environment && binding.field && *binding.field < layout.fields.size();
}

static bool slotTakesWhateverEvalStores(const Program& program, const EvalBinding& binding)
{
    if (!binding.layout)
        return true;
    const DeclaredType& declared = program.at(binding.layout).fields[*binding.field].declared;
    return !declared.isEstablished() || TypeReader(program).at(declared.written()).isAnything();
}

static bool namesBindingOfKind(const EvalScope& scope, std::optional<uint32_t> index, EvalBindingKind kind)
{
    return index && *index < scope.bindings.size() && scope.bindings[*index].kind == kind;
}

static bool namesItsFunctions(const EvalScope& scope, const EvalBinding& binding)
{
    if (namesBindingOfKind(scope, binding.function, EvalBindingKind::PrivateAccessorPair))
        return binding.kind == EvalBindingKind::PrivateAccessor && binding.setter == binding.function;
    return (binding.function || binding.setter)
        && (!binding.function || namesBindingOfKind(scope, binding.function, EvalBindingKind::PrivateFunction))
        && (!binding.setter || namesBindingOfKind(scope, binding.setter, EvalBindingKind::PrivateFunction));
}

static bool isValidEvalBinding(const Program& program, const EvalScope& scope, const EvalBinding& binding)
{
    bool hasFunctions = binding.function || binding.setter;
    switch (binding.kind) {
    case EvalBindingKind::PrivateName:
        return !hasFunctions && hasValidPrivateHolder(program, binding);
    case EvalBindingKind::PrivateMethod:
        return !binding.setter && namesItsFunctions(scope, binding) && hasValidPrivateHolder(program, binding);
    case EvalBindingKind::PrivateAccessor:
        return namesItsFunctions(scope, binding) && hasValidPrivateHolder(program, binding);
    case EvalBindingKind::PrivateFunction:
    case EvalBindingKind::PrivateAccessorPair:
        return !hasFunctions && !binding.name && hasValidStorage(program, binding);
    case EvalBindingKind::MappedArgument:
        return !hasFunctions && !binding.variable && binding.operand && binding.field && !binding.layout;
    case EvalBindingKind::This:
        return (binding.function || !binding.setter) && hasValidStorage(program, binding);
    case EvalBindingKind::Variable:
    case EvalBindingKind::Arguments:
    case EvalBindingKind::SimpleCatchParameter:
        return !hasFunctions && hasValidStorage(program, binding) && slotTakesWhateverEvalStores(program, binding);
    case EvalBindingKind::Constant:
    case EvalBindingKind::FunctionName:
    case EvalBindingKind::NewTarget:
    case EvalBindingKind::HomeObject:
    case EvalBindingKind::ClassObject:
    case EvalBindingKind::WithObject:
    case EvalBindingKind::EvalVariables:
        return !hasFunctions && hasValidStorage(program, binding);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isReachedWithoutOperand(const EvalBinding& binding)
{
    bool isName = binding.kind == EvalBindingKind::Variable || binding.kind == EvalBindingKind::Constant || binding.kind == EvalBindingKind::FunctionName;
    return isName && binding.name && binding.variable;
}

static bool isValidExpectedResult(const Program& program, const ExpectedResult& expected)
{
    return std::ranges::all_of(expected.layouts, [&](LayoutID layout) {
        return hasObjectKind(program, layout);
    });
}

static bool isHelperOfCompiler(const BuiltinTable& builtins, RuntimeFunctionID id)
{
    return std::ranges::any_of(builtins.at(id).implements, [&](BuiltinID implemented) {
        return builtins.at(implemented).holder == Builtins::CompilerHelpers;
    });
}

void ProgramVerifier::verifyModuleInstances()
{
    TypeReader types(m_program);
    BitVector modulesWithInstances = findModulesWithInstances(m_program);
    auto failAt = [&](uint32_t index) {
        fail({ .kind = InternalErrorKind::InvalidModule, .count = index });
    };
    auto environmentOf = [&](const ModuleInstance& instance, uint32_t field) -> LayoutID {
        std::span<const Field> fields = m_program.at(instance.layout).fields.span();
        if (field >= fields.size() || !fields[field].declared.isEstablished() || holdsOnlyPrimitives(fields[field].declared.written()))
            return { };
        return types.at(fields[field].declared.written()).environment;
    };
    auto isUntyped = [&](const ModuleInstance& instance, uint32_t field) {
        std::span<const Field> fields = m_program.at(instance.layout).fields.span();
        return field < fields.size() && (!fields[field].declared.isEstablished() || types.at(fields[field].declared.written()).isAnything());
    };

    uint32_t reloadableModuleCount = 0;
    for (size_t module : modulesWithInstances)
        reloadableModuleCount += !!m_program.findModuleInstance(ModuleID(module));
    if (reloadableModuleCount != m_program.moduleInstances.size() || modulesWithInstances.bitCount() != m_program.moduleInstances.size())
        failAt(reloadableModuleCount);
    for (uint32_t index = 0; index < m_program.moduleInstances.size(); ++index) {
        const ModuleInstance& instance = m_program.moduleInstances[index];
        bool isSorted = !index || m_program.moduleInstances[index - 1].module < instance.module;
        bool hasLayout = instance.module && instance.layout && isInstanceLayout(m_program, instance.layout, modulesWithInstances) && m_program.at(instance.layout).module == instance.module;
        if (!isSorted || !hasLayout || instance.namespaceField == instance.deferredNamespaceField || !isUntyped(instance, instance.namespaceField) || !isUntyped(instance, instance.deferredNamespaceField))
            return failAt(index);
    }
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> resolvedLinks;
    auto keyOf = [](ModuleID module, uint32_t field) {
        return static_cast<uint64_t>(module.index()) << 32 | field;
    };
    for (uint32_t index = 0; index < m_program.moduleLinks.size(); ++index) {
        const ModuleLink& link = m_program.moduleLinks[index];
        if (link.module && link.link && !link.path.isEmpty())
            resolvedLinks.add(keyOf(link.module, *link.link), index);
    }
    auto findRest = [&](const ModuleLink& link) -> std::optional<uint32_t> {
        auto found = link.path.isEmpty() ? resolvedLinks.end() : resolvedLinks.find(keyOf(link.path[link.path.size() - 1], *link.rest));
        return found != resolvedLinks.end() ? std::optional(found->value) : std::nullopt;
    };
    BitVector linksWithEnd(m_program.moduleLinks.size());
    for (uint32_t index = 0; index < m_program.moduleLinks.size(); ++index) {
        Vector<uint32_t, 8> path;
        for (std::optional<uint32_t> at = index; at && !linksWithEnd.get(*at); at = m_program.moduleLinks[*at].rest ? findRest(m_program.moduleLinks[*at]) : std::nullopt) {
            if (path.size() > resolvedLinks.size())
                return failAt(index);
            path.append(*at);
        }
        for (uint32_t link : path)
            linksWithEnd.set(link);
    }
    for (uint32_t index = 0; index < m_program.moduleLinks.size(); ++index) {
        const ModuleLink& link = m_program.moduleLinks[index];
        bool isSorted = !index || !(link.module < m_program.moduleLinks[index - 1].module);
        const ModuleInstance* instance = link.module ? m_program.findModuleInstance(link.module) : nullptr;
        if (!isSorted || !instance || !link.target || (!link.link && !link.mock) || (!link.path.isEmpty() && link.link && link.mock))
            return failAt(index);
        if (!std::ranges::all_of(link.path, [&](ModuleID module) { return m_program.findModuleInstance(module); }))
            return failAt(index);
        if (link.rest) {
            std::optional<uint32_t> rest = link.link ? findRest(link) : std::nullopt;
            if (!rest || m_program.moduleLinks[*rest].target != link.target)
                return failAt(index);
        } else if (!link.path.isEmpty() && link.link && link.path[link.path.size() - 1] != link.target)
            return failAt(index);
        const ModuleInstance* target = m_program.findModuleInstance(link.target);
        if (link.link && (!target || environmentOf(*instance, *link.link) != target->layout))
            return failAt(index);
        bool mayBeMocked = m_program.at(link.target).flags.contains(ModuleFlags::MayBeMocked) || !isProgramModuleOf(m_program.at(link.target).kind);
        if (link.mock && (!isUntyped(*instance, *link.mock) || !mayBeMocked))
            return failAt(index);
    }
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        for (const ModuleExport& entry : m_program.modules[index].exports) {
            const ModuleInstance* instance = m_program.findModuleInstance(ModuleID(index));
            if (entry.kind == ModuleExportKind::Namespace && entry.link) {
                const ModuleInstance* target = m_program.findModuleInstance(entry.module);
                if (!instance || !target || environmentOf(*instance, *entry.link) != target->layout)
                    return failAt(index);
            }
            if (entry.kind != ModuleExportKind::Field)
                continue;
            bool isOwn = !instance || entry.layout == instance->layout;
            if (isOwn == entry.link.has_value() || (entry.link && environmentOf(*instance, *entry.link) != entry.layout))
                return failAt(index);
        }
    }
}

void ProgramVerifier::verifySmallRecords()
{
    BitVector scopesThatMayLieAroundOthers(m_program.evalScopes.size());
    for (unsigned index = 0; index < m_program.evalScopes.size(); ++index) {
        const EvalScope& scope = m_program.evalScopes[index];
        bool hasValidBindings = std::ranges::all_of(scope.bindings, [&](const EvalBinding& binding) {
            return isValidEvalBinding(m_program, scope, binding);
        });
        if (!hasValidBindings)
            fail({ .kind = InternalErrorKind::PayloadOutOfRange, .count = index });
        if (!scope.outer && std::ranges::all_of(scope.bindings, isReachedWithoutOperand))
            scopesThatMayLieAroundOthers.set(index);
    }
    for (unsigned index = 0; index < m_program.evalScopes.size(); ++index) {
        EvalScopeID outer = m_program.evalScopes[index].outer;
        if (outer && !scopesThatMayLieAroundOthers.get(outer.index()))
            fail({ .kind = InternalErrorKind::PayloadOutOfRange, .count = index, .detail = "what lies around the scope of a direct eval needs an operand of a call or has a scope around itself"_s });
    }

    auto& runtimeFunctions = m_program.runtimeFunctions;
    auto lastSorted = std::ranges::adjacent_find(runtimeFunctions, std::ranges::greater_equal { }, &RuntimeFunctionEntry::id);
    if (lastSorted != runtimeFunctions.end())
        fail({ .kind = InternalErrorKind::UnsortedList, .count = static_cast<uint32_t>(lastSorted - runtimeFunctions.begin() + 1) });
    for (auto& entry : runtimeFunctions) {
        if (!entry.id || !m_program.builtins().contains(entry.id) || !entry.function || !m_program.at(entry.function).captures.isEmpty())
            fail({ .kind = InternalErrorKind::InvalidCreatorRecord, .function = entry.function });
        else if (isHelperOfCompiler(m_program.builtins(), entry.id) && std::ranges::count(runtimeFunctions, entry.function, &RuntimeFunctionEntry::function) != 1)
            fail({ .kind = InternalErrorKind::InvalidCreatorRecord, .function = entry.function, .detail = "a function that stands for a helper of the compiler stands for another function of the library too"_s });
    }

    for (unsigned index = 0; index < m_program.expectedResults.size(); ++index) {
        if (!isValidExpectedResult(m_program, m_program.expectedResults[index]))
            fail({ .kind = InternalErrorKind::PayloadOutOfRange, .count = index });
    }

    for (unsigned index = 0; index < m_program.ambiguousExports.size(); ++index) {
        const AmbiguousExport& ambiguous = m_program.ambiguousExports[index];
        if (!ambiguous.module || !ambiguous.name || !rulesOf(m_program.at(ambiguous.module).kind).hasExportTable)
            fail({ .kind = InternalErrorKind::PayloadOutOfRange, .count = index });
    }

    verifyModuleInstances();

    for (unsigned index = 0; index < m_program.sourceStamps.size(); ++index) {
        if (!m_program.sourceStamps[index].module)
            fail({ .kind = InternalErrorKind::PayloadOutOfRange, .count = index });
    }

    for (unsigned index = 0; index < m_program.declaredRecords.size(); ++index) {
        if (!m_program.declaredRecords[index].module)
            fail({ .kind = InternalErrorKind::PayloadOutOfRange, .count = index });
    }

    for (unsigned index = 0; index < m_program.webAssemblyModules.size(); ++index) {
        if (!m_program.webAssemblyModules[index].module)
            fail({ .kind = InternalErrorKind::PayloadOutOfRange, .count = index });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
