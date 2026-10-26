/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowSiteData.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTSortedSet.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/BitVector.h>
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
    bool hasUniquePath { false };
};

struct VerifiedRequestTargets {
    const Vector<ModuleRequest>& requests;

    size_t size() const { return requests.size(); }
    ModuleID operator[](size_t index) const { return requests[index].target; }
};

} // anonymous namespace

static ModuleKindRules rulesOf(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
        return { .body = BodyRule::Optional, .hasExportTable = true, .hasRequests = true, .hasRequiredNames = true, .hasUniquePath = true };
    case ModuleKind::CommonJs:
        return { .body = BodyRule::Required, .forbiddenBodyFlags = FunctionFlags::HasThis, .hasRequests = true, .hasUniquePath = true };
    case ModuleKind::Data:
        return { .body = BodyRule::Optional, .forbiddenBodyFlags = { FunctionFlags::IsAsync, FunctionFlags::IsGenerator }, .hasExportTable = true, .canHaveData = true, .hasUniquePath = true };
    case ModuleKind::RuntimeSource:
        return { .body = BodyRule::Required, .forbiddenBodyFlags = { FunctionFlags::IsAsync, FunctionFlags::IsGenerator }, .hasRuntimeModule = true, .hasUniquePath = true };
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

static bool hasRequiredIDs(const Module& module)
{
    auto isSet = [](auto id) { return !!id; };
    return module.path
        && module.displayPath
        && std::ranges::all_of(module.requests, isSet, &ModuleRequest::target)
        && std::ranges::all_of(module.exports, isSet, &ModuleExport::name)
        && std::ranges::all_of(module.syntheticExports, isSet, &SyntheticExport::name)
        && std::ranges::all_of(module.requiredNames, isSet, &RequiredName::module)
        && std::ranges::all_of(module.requiredNames, isSet, &RequiredName::name);
}

static bool isCommonJsRule(SyntheticExportRule rule)
{
    return rule == SyntheticExportRule::CommonJs || rule == SyntheticExportRule::CommonJsIgnoringAnnotation;
}

static bool hasValidInterop(const Program& program, const Module& module)
{
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

static bool hasOneBinding(const Program& program, const ModuleExport& entry)
{
    switch (entry.kind) {
    case ModuleExportKind::Variable:
        return entry.variable && !entry.module && !entry.importName && entry.phase == ImportPhase::Evaluation;
    case ModuleExportKind::Namespace:
        return entry.module && !entry.variable && !entry.importName;
    case ModuleExportKind::Indirect:
        return entry.module && entry.importName && !entry.variable && entry.phase == ImportPhase::Evaluation && isUncompiledCode(program.at(entry.module));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasDuplicateRequests(const Vector<ModuleRequest>& requests)
{
    auto keys = WTF::map(requests, [](const ModuleRequest& request) {
        return std::tuple { request.target, request.phase, request.kind };
    });
    AOT::sort(keys);
    return std::ranges::adjacent_find(keys) != keys.end();
}

static bool isValidRequest(const ModuleRequest& request)
{
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

static std::optional<InternalErrorKind> findModuleError(const Program& program, const Module& module, bool hasValidBody)
{
    ModuleKindRules rules = rulesOf(module.kind);
    bool isLibrary = module.flags.contains(ModuleFlags::IsLibrary);
    bool hasValidLists = (rules.hasExportTable ? module.syntheticExports.isEmpty() || module.exportRule != SyntheticExportRule::None : module.exports.isEmpty())
        && (rules.hasRequests || module.requests.isEmpty())
        && (rules.hasRequiredNames || module.requiredNames.isEmpty())
        && std::ranges::all_of(module.requests, isValidRequest);
    bool hasValidLibraryFlag = !isLibrary || (rules.body != BodyRule::Absent && !module.body && module.exports.isEmpty());
    bool hasValidImportOnlyFlag = !module.flags.contains(ModuleFlags::IsLoadedByImportOnly) || module.runtimeModule;
    bool hasValidFlagsOfFiles = !module.flags.containsAny({ ModuleFlags::HasNoSideEffects, ModuleFlags::KeepsError, ModuleFlags::NamesWayToModuleObjects }) || isFileOfProgram(module);
    bool hasValidRuntimeModule = module.runtimeModule
        ? rules.hasRuntimeModule && program.builtins().contains(module.runtimeModule)
        : !rules.hasRuntimeModule || isLibrary;
    bool hasValidLinks = (!!module.referrer == rules.hasReferrer || (module.referrer && module.kind == ModuleKind::CommonJs))
        && (!module.data || rules.canHaveData)
        && !!module.namespaceLayout == (module.kind == ModuleKind::SourceText);
    auto isBound = [&](const ModuleExport& entry) {
        return hasOneBinding(program, entry);
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
    if (!isAsRead())
        return;
    auto& modules = m_program.modules;
    Vector<uint32_t> invalidModules = findIndicesInParallel(m_build.parallelLoops(), modules.size(), [&](unsigned index) {
        return findModuleError(m_program, modules[index], hasValidBody(modules[index])).has_value();
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
        if (rulesOf(module.kind).hasUniquePath && usedPaths.quickSet(module.path.index()))
            fail({ .kind = InternalErrorKind::InvalidModule, .count = index });
        if (module.body && usedBodies.quickSet(module.body.index()))
            fail({ .kind = InternalErrorKind::InvalidModule, .function = module.body, .count = index });
        if (module.exportsVariable && usedExportsVariables.quickSet(module.exportsVariable.index()))
            fail({ .kind = InternalErrorKind::InvalidModule, .count = index });
    }
}

void ProgramVerifier::verifyModule(ModuleID id, const Module& module)
{
    if (auto kind = findModuleError(m_program, module, hasValidBody(module)))
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
        && isAnything(body->results[0].type)
        && !body->flags.containsAny(rules.forbiddenBodyFlags);
}

namespace {

enum class EvaluationRule : uint32_t {
    TargetIsFileThatEndsAtOnce = 1,
    ReferrerRequestsTarget,
    BareSiteStandsInBodyOfReferrer,
    BareSiteStandsInBodyThatNeverWaits,
    BareSiteHasNoHandler,
    BareSiteRunsOnce,
    BareSitesRunInOneOrder,
    EveryRunThatEndsPassesBareSite,
};

} // anonymous namespace

void FunctionVerifier::verifyModuleEvaluation(BlockID block, ValueID id, const Value& value)
{
    auto failRule = [&](EvaluationRule rule) {
        fail(InternalErrorKind::InvalidModuleEvaluation, block, id, static_cast<uint32_t>(rule));
    };
    const Module& target = m_program.at(value.module());
    if (!isFileOfProgram(target) || target.flags.contains(ModuleFlags::HasTopLevelAwait))
        return failRule(EvaluationRule::TargetIsFileThatEndsAtOnce);

    bool isBare = value.opcode == Opcode::EvaluateModule && value.isBare() && form() == IRForm::AsRead && !isOptimizing();
    m_moduleEvaluations.append(ModuleEvaluationSite { value.referrer(), value.module(), block, id, isBare });
    if (!isBare)
        return;
    if (m_program.at(value.referrer()).body != m_id)
        return failRule(EvaluationRule::BareSiteStandsInBodyOfReferrer);
    if (m_function.has(FunctionFlags::IsAsync))
        return failRule(EvaluationRule::BareSiteStandsInBodyThatNeverWaits);
    if (m_function.at(block).handler)
        return failRule(EvaluationRule::BareSiteHasNoHandler);
    if (analyses().isInCycle(block))
        failRule(EvaluationRule::BareSiteRunsOnce);
}

void FunctionVerifier::verifyBareEvaluations()
{
    if (hasFailed())
        return;
    const ModuleEvaluationSite* last = nullptr;
    for (const ModuleEvaluationSite& site : m_moduleEvaluations) {
        if (!site.isBare)
            continue;
        if (last && !analyses().dominators().dominates(last->block, site.block))
            return fail(InternalErrorKind::InvalidModuleEvaluation, site.block, site.value, static_cast<uint32_t>(EvaluationRule::BareSitesRunInOneOrder));
        last = &site;
    }
    if (!last)
        return;
    for (BlockID block : analyses().cfg().reversePostOrder()) {
        TerminatorKind kind = m_function.at(block).terminator.kind;
        bool returns = kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall;
        if (returns && !analyses().dominators().dominates(last->block, block))
            return fail(InternalErrorKind::InvalidModuleEvaluation, last->block, last->value, static_cast<uint32_t>(EvaluationRule::EveryRunThatEndsPassesBareSite));
    }
}

void FunctionVerifier::verifyRequestsOfEvaluations()
{
    if (hasFailed() || m_moduleEvaluations.isEmpty())
        return;
    AOT::sort(m_moduleEvaluations, { }, [](const ModuleEvaluationSite& site) {
        return std::tuple { site.referrer, site.target, site.value };
    });
    ModuleID referrer;
    Vector<ModuleID> requiredModules;
    for (const ModuleEvaluationSite& site : m_moduleEvaluations) {
        if (site.referrer != referrer) {
            referrer = site.referrer;
            requiredModules.shrink(0);
            for (const ModuleRequest& request : m_program.at(referrer).requests) {
                if (request.kind == ModuleRequestKind::Require)
                    requiredModules.append(request.target);
            }
            AOT::sort(requiredModules);
        }
        if (!std::ranges::binary_search(requiredModules, site.target))
            return fail(InternalErrorKind::InvalidModuleEvaluation, site.block, site.value, static_cast<uint32_t>(EvaluationRule::ReferrerRequestsTarget));
    }
}

template<typename Functor>
static void forEachNamedModule(const Module& module, const Functor& functor)
{
    for (auto& entry : module.exports) {
        if (entry.module)
            functor(entry.module);
    }
    for (auto& required : module.requiredNames)
        functor(required.module);
}

static Vector<uint32_t> findUnreachedNames(const Vector<Module>& modules)
{
    Vector<ModuleID> allNamedModules;
    for (auto& module : modules) {
        forEachNamedModule(module, [&](ModuleID named) {
            allNamedModules.append(named);
        });
    }
    if (allNamedModules.isEmpty())
        return { };
    SortedSet<ModuleID> namedModules(allNamedModules.span());
    auto rankOf = [&](ModuleID module) -> size_t {
        return std::lower_bound(namedModules.begin(), namedModules.end(), module) - namedModules.begin();
    };

    unsigned numberOfModules = modules.size();
    auto components = computeStronglyConnectedComponents(numberOfModules, [&](unsigned module) {
        return VerifiedRequestTargets { modules[module].requests };
    });
    auto& componentOf = components.componentOf;

    Vector<uint32_t> modulesByComponent(numberOfModules, [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::sort(modulesByComponent, { }, [&](uint32_t module) {
        return componentOf[module];
    });

    Vector<BitVector> reachedNames(components.numberOfComponents);
    for (size_t rank = 0; rank < namedModules.size(); ++rank)
        reachedNames[componentOf[namedModules[rank].index()]].set(rank);
    for (uint32_t index : modulesByComponent) {
        uint32_t component = componentOf[index];
        for (auto& request : modules[index].requests) {
            uint32_t requestedComponent = componentOf[request.target.index()];
            if (requestedComponent != component)
                reachedNames[component].merge(reachedNames[requestedComponent]);
        }
    }

    Vector<uint32_t> holders;
    for (unsigned index = 0; index < numberOfModules; ++index) {
        const BitVector& reached = reachedNames[componentOf[index]];
        bool reachesEveryName = true;
        forEachNamedModule(modules[index], [&](ModuleID named) {
            reachesEveryName &= reached.get(rankOf(named));
        });
        if (!reachesEveryName)
            holders.append(index);
    }
    return holders;
}

void ProgramVerifier::verifyModuleGraph()
{
    if (!isAsRead())
        return;
    auto& modules = m_program.modules;
    for (unsigned index = 0; index < modules.size(); ++index) {
        if (modules[index].flags.contains(ModuleFlags::IsLibrary) && !modules[index].requests.isEmpty())
            fail({ .kind = InternalErrorKind::InvalidModule, .count = index });
    }
    for (uint32_t index : findUnreachedNames(modules))
        fail({ .kind = InternalErrorKind::InvalidModule, .count = index });
    verifyModuleFailures();
}

void ProgramVerifier::verifyModuleFailures()
{
    for (unsigned index = 0; index < m_program.moduleFailures.size(); ++index) {
        const ModuleFailure& failure = m_program.moduleFailures[index];
        if (!failure.importer || !failure.specifier)
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
    if (!isAsRead())
        return;
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

static bool canHaveExtraKeys(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
        return true;
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasValidMembers(const Program& program, const DataNode::Object& object, bool mayLackLayout, Vector<PropertyKeyID, 16>& keys)
{
    if (object.layout ? !hasObjectKind(program, object.layout) : !mayLackLayout)
        return false;

    const Layout* layout = object.layout ? &program.at(object.layout) : nullptr;
    bool hasOnlyFields = layout && !canHaveExtraKeys(layout->kind);
    keys.shrink(0);
    for (const DataMember& member : object.members) {
        if (!member.key || member.value.is<DataNode::Hole>() || (hasOnlyFields && !layout->findField(member.key)))
            return false;
        keys.append(member.key);
    }
    AOT::sort(keys);
    return std::ranges::adjacent_find(keys) == keys.end();
}

static bool isValidDataNode(const Program& program, const DataNode& node, bool mayLackLayout, Vector<PropertyKeyID, 16>& keys)
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
        return hasValidMembers(program, node.as<DataNode::Object>(), mayLackLayout, keys);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isValidDataTree(const Program& program, const DataNode& root, bool mayLackLayout)
{
    bool isValid = !root.is<DataNode::Hole>();
    Vector<PropertyKeyID, 16> keys;
    forEachDataNodeInPreorder(root, [&](uint32_t, const DataNode& node) {
        isValid = isValid && isValidDataNode(program, node, mayLackLayout, keys);
    });
    return isValid;
}

void ProgramVerifier::verifyData()
{
    if (!hasUncheckedRecords())
        return;
    auto& data = m_program.data;
    Vector<uint32_t> invalidRecords = findIndicesInParallel(m_build.parallelLoops(), data.size(), [&](unsigned index) {
        return !isValidDataTree(m_program, data[index].root, !isAsRead());
    });
    for (uint32_t index : invalidRecords)
        verifyDataNode(DataID(index), data[index].root);
}

void ProgramVerifier::verifyDataNode(DataID id, const DataNode& root)
{
    if (!isValidDataTree(m_program, root, !isAsRead()))
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

static bool isValidEvalBinding(const Program& program, const EvalBinding& binding)
{
    bool hasFunctions = binding.function || binding.setter;
    switch (binding.kind) {
    case EvalBindingKind::PrivateName:
        return !hasFunctions && hasValidPrivateHolder(program, binding);
    case EvalBindingKind::PrivateMethod:
        return binding.function && !binding.setter && hasValidPrivateHolder(program, binding);
    case EvalBindingKind::PrivateAccessor:
        return hasFunctions && hasValidPrivateHolder(program, binding);
    case EvalBindingKind::MappedArgument:
        return !hasFunctions && !binding.variable && binding.operand && binding.field && !binding.layout;
    case EvalBindingKind::Variable:
    case EvalBindingKind::Constant:
    case EvalBindingKind::FunctionName:
    case EvalBindingKind::This:
    case EvalBindingKind::NewTarget:
    case EvalBindingKind::HomeObject:
    case EvalBindingKind::ClassObject:
    case EvalBindingKind::Arguments:
    case EvalBindingKind::WithObject:
    case EvalBindingKind::EvalVariables:
    case EvalBindingKind::SimpleCatchParameter:
        return !hasFunctions && hasValidStorage(program, binding);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isValidExpectedResult(const Program& program, const ExpectedResult& expected)
{
    return std::ranges::all_of(expected.layouts, [&](LayoutID layout) {
        return hasObjectKind(program, layout);
    });
}

void ProgramVerifier::verifySmallRecords()
{
    if (!hasUncheckedRecords())
        return;

    for (unsigned index = 0; index < m_program.evalScopes.size(); ++index) {
        bool hasValidBindings = std::ranges::all_of(m_program.evalScopes[index].bindings, [&](const EvalBinding& binding) {
            return isValidEvalBinding(m_program, binding);
        });
        if (!hasValidBindings)
            fail({ .kind = InternalErrorKind::PayloadOutOfRange, .count = index });
    }

    auto& runtimeFunctions = m_program.runtimeFunctions;
    auto lastSorted = std::ranges::adjacent_find(runtimeFunctions, std::ranges::greater_equal { }, &RuntimeFunctionEntry::id);
    if (lastSorted != runtimeFunctions.end())
        fail({ .kind = InternalErrorKind::UnsortedList, .count = static_cast<uint32_t>(lastSorted - runtimeFunctions.begin() + 1) });
    for (auto& entry : runtimeFunctions) {
        if (!entry.id || !m_program.builtins().contains(entry.id) || !entry.function || !m_program.at(entry.function).captures.isEmpty())
            fail({ .kind = InternalErrorKind::InvalidCreatorRecord, .function = entry.function });
    }

    for (unsigned index = 0; index < m_program.expectedResults.size(); ++index) {
        if (!isValidExpectedResult(m_program, m_program.expectedResults[index]))
            fail({ .kind = InternalErrorKind::PayloadOutOfRange, .count = index });
    }

    for (unsigned index = 0; index < m_program.webAssemblyModules.size(); ++index) {
        if (!m_program.webAssemblyModules[index].module)
            fail({ .kind = InternalErrorKind::PayloadOutOfRange, .count = index });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
