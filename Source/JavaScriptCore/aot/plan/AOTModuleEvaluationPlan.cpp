/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleEvaluationPlan.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTSort.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThinSummary.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

// ---- The operations of reached code that name a module

namespace {

enum class FoundModuleSiteKind : uint8_t {
    // EvaluateModule in the body of its referrer, where every run of the body passes once and no handler stands around it.
    Bare,
    // EvaluateModule anywhere else.
    Late,
    // RequireModule.
    Required,
    // Any other operation that may be the first to evaluate the module it names: ImportModule, ModuleNamespace of a phase other than Evaluation.
    Named,
    // RecordRequire.
    Record,
};

struct FoundModuleSite {
    FunctionID function;
    ValueID value;
    ModuleID target;
    FoundModuleSiteKind kind { FoundModuleSiteKind::Late };
    // Of a Record alone.
    ModuleID referrer;
};

class ModuleSiteFinder {
public:
    static constexpr OpcodeSet opcodesOfSites { Opcode::EvaluateModule, Opcode::RequireModule, Opcode::ImportModule, Opcode::ModuleNamespace, Opcode::RecordRequire };

    ModuleSiteFinder(const Program&, const BodyIndex&, std::span<const BlockID> blocksNeverEntered, FunctionID, ModuleID moduleWithBody, const BitVector& isCalledWhereRequired, Vector<FoundModuleSite>&);

    void run();

private:
    void visit(const BodyBlock&, const BodyValue&);
    void add(const BodyValue&, ModuleID target, FoundModuleSiteKind, ModuleID referrer = { });
    bool isBare(const BodyBlock&, BodyOperation<Opcode::EvaluateModule>);
    bool isPassedOnceByEveryRunThatReturns(BlockID);
    BlockID nearestDominatorOfReturns() const;

    const Program& m_program;
    const BodyIndex& m_body;
    // ThinSummary::blocksNeverEntered: the walk builds nothing for them.
    std::span<const BlockID> m_blocksNeverEntered;
    const FunctionID m_function;
    // The module whose evaluation is a run of the body. Null where no site of the body can be bare.
    const ModuleID m_evaluated;
    const BitVector& m_isCalledWhereRequired;
    Vector<FoundModuleSite>& m_found;
    // Null where no block that is reached returns.
    BlockID m_dominatorOfReturns;
    bool m_hasLookedForReturns { false };
};

} // anonymous namespace

// A body that waits lets other code run between two of its sites.
ModuleSiteFinder::ModuleSiteFinder(const Program& program, const BodyIndex& body, std::span<const BlockID> blocksNeverEntered, FunctionID function, ModuleID moduleWithBody, const BitVector& isCalledWhereRequired, Vector<FoundModuleSite>& found)
    : m_program(program)
    , m_body(body)
    , m_blocksNeverEntered(blocksNeverEntered)
    , m_function(function)
    , m_evaluated(program.at(function).isCoroutine() ? ModuleID() : moduleWithBody)
    , m_isCalledWhereRequired(isCalledWhereRequired)
    , m_found(found)
{
}

void ModuleSiteFinder::run()
{
    for (uint32_t number = 0; number < m_body.numberOfBlocks(); ++number) {
        if (std::ranges::binary_search(m_blocksNeverEntered, BlockID(number)))
            continue;
        BodyBlock block = m_body.block(BlockID(number));
        for (BodyValue value : block.values()) {
            if (opcodesOfSites.contains(value.opcode()))
                visit(block, value);
        }
    }
}

void ModuleSiteFinder::visit(const BodyBlock& block, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::EvaluateModule) {
        auto evaluation = value.as<Opcode::EvaluateModule>();
        add(value, evaluation.module(), isBare(block, evaluation) ? FoundModuleSiteKind::Bare : FoundModuleSiteKind::Late);
        return;
    }
    if (opcode == Opcode::RequireModule) {
        add(value, value.as<Opcode::RequireModule>().module(), FoundModuleSiteKind::Required);
        return;
    }
    if (opcode == Opcode::RecordRequire) {
        auto record = value.as<Opcode::RecordRequire>();
        RELEASE_ASSERT(record.referrer());
        add(value, record.module(), FoundModuleSiteKind::Record, record.referrer());
        return;
    }
    if (opcode == Opcode::ModuleNamespace) {
        // Whoever fetches the namespace object of an import that is not deferred evaluates nothing.
        auto fetch = value.as<Opcode::ModuleNamespace>();
        if (fetch.phase() != ImportPhase::Evaluation)
            add(value, fetch.module(), FoundModuleSiteKind::Named);
        return;
    }

    ASSERT(opcode == Opcode::ImportModule);
    auto loading = value.as<Opcode::ImportModule>();
    add(value, loading.module(), FoundModuleSiteKind::Named);
    // The import attributes are known only at run time, and they choose which form of the file is loaded.
    if (!loading.optionsOperand())
        return;
    for (ModuleID form : formsInImage(m_program, loading.module()))
        add(value, form, FoundModuleSiteKind::Named);
}

void ModuleSiteFinder::add(const BodyValue& value, ModuleID target, FoundModuleSiteKind kind, ModuleID referrer)
{
    // The scan of Facts stops the build where a body names a module that the program lacks.
    RELEASE_ASSERT(target);
    m_found.append(FoundModuleSite { m_function, value.id(), target, kind, referrer });
}

// The front end marks the sites that it wrote as bare, and only checking builds verify its word.
bool ModuleSiteFinder::isBare(const BodyBlock& block, BodyOperation<Opcode::EvaluateModule> evaluation)
{
    if (!evaluation.isBare() || !m_evaluated || evaluation.referrer() != m_evaluated || block.hasHandler())
        return false;
    ModuleID target = evaluation.module();
    if (!m_isCalledWhereRequired.get(target.index()))
        return false;
    // Of two bare sites that name one module the first alone calls it.
    // `require("./a.cjs"); delete require.cache[path]; require("./a.cjs")` runs the file at both: each site asks the status of the file.
    if (m_program.at(target).flags.containsAny({ ModuleFlags::MayBeEvicted, ModuleFlags::RunsAgain }))
        return false;
    return isPassedOnceByEveryRunThatReturns(block.id());
}

// A run that throws leaves the module failed, whatever it has passed.
bool ModuleSiteFinder::isPassedOnceByEveryRunThatReturns(BlockID block)
{
    if (!m_hasLookedForReturns) {
        m_dominatorOfReturns = nearestDominatorOfReturns();
        m_hasLookedForReturns = true;
    }
    return m_dominatorOfReturns && !m_body.loopOf(block) && m_body.dominates(block, m_dominatorOfReturns);
}

BlockID ModuleSiteFinder::nearestDominatorOfReturns() const
{
    BlockID dominator;
    for (BlockID block : m_body.blocksInDominatorOrder()) {
        TerminatorKind kind = m_body.terminator(block).kind();
        if (kind != TerminatorKind::Return && kind != TerminatorKind::ReturnCall)
            continue;
        if (!dominator) {
            dominator = block;
            continue;
        }
        while (!m_body.dominates(dominator, block)) {
            dominator = m_body.immediateDominator(dominator);
            RELEASE_ASSERT(dominator);
        }
    }
    return dominator;
}

// In the order of function ids and, within a function, of the blocks as stored. `moduleWithBody`: by function, the module that has it as its body.
static Vector<FoundModuleSite> findModuleSites(const Program& program, const Facts& facts, const ParallelLoops& loops, const BitVector& isCalledWhereRequired, std::span<const ModuleID> moduleWithBody)
{
    RELEASE_ASSERT(moduleWithBody.size() == program.functions.size());
    const BodyIndexTable& bodies = facts.bodies();
    return collectInParallel<FoundModuleSite>(loops, static_cast<unsigned>(program.functions.size()), [&](unsigned number, Vector<FoundModuleSite>& found) {
        FunctionID function(number);
        const BodyIndex* body = bodies.find(function);
        if (!body || !body->holdsAnyOf(ModuleSiteFinder::opcodesOfSites) || !facts.mayBeReached(function))
            return;
        ModuleSiteFinder(program, *body, facts.summaries().at(function).blocksNeverEntered.span(), function, moduleWithBody[number], isCalledWhereRequired, found).run();
    });
}

static bool isUsedInModuleEvaluationPlan(const Facts& facts, BuiltinID builtin)
{
    return findFactRow(facts.builtinUses(), builtin.index(), &BuiltinUseRecord::builtin);
}

static bool someFileAsksHowItWasLoaded(const Program& program, const Facts& facts)
{
    static constexpr OpcodeSet askers { Opcode::WasLoadedSynchronously };
    const BodyIndexTable& bodies = facts.bodies();
    for (uint32_t number = 0; number < program.functions.size(); ++number) {
        const BodyIndex* body = bodies.find(FunctionID(number));
        if (body && body->holdsAnyOf(askers) && facts.mayBeReached(FunctionID(number)))
            return true;
    }
    return false;
}

// `children`, `parent` and `id` of a module object say which require() came first and which has run at all.
// Where the object goes out of sight each of its members has a row of its own: Facts::builtinUses().
static bool asksWhoRequiredWhom(const Program& program, const Facts& facts)
{
    if (!namesWaysToModuleObjects(program))
        return false;
    if (facts.unseenCodeCanRun() || facts.unseenScriptsCanRun() || facts.storesPathOfMainModule())
        return true;
    // `require.cache[path].children`: nobody follows an entry of the table to the module object that it is.
    bool entryOfCacheShowsModuleObject = std::ranges::any_of(program.modules, [](const Module& file) {
        return file.flags.contains(ModuleFlags::IsShownByEntryOfCache);
    });
    if (entryOfCacheShowsModuleObject || isUsedInModuleEvaluationPlan(facts, Builtins::ModuleObject) || someFileAsksHowItWasLoaded(program, facts))
        return true;
    // `require.main.children`: what the two hold is a module object that no operation names, and no row says what is read of it.
    if (isUsedInModuleEvaluationPlan(facts, Builtins::ModuleRequireMain) || isUsedInModuleEvaluationPlan(facts, Builtins::ProcessMainModule))
        return true;
    const BuiltinTable& builtins = program.builtins();
    for (ASCIILiteral name : { "parent"_s, "children"_s, "id"_s }) {
        BuiltinID member = builtins.findOwnMember(Builtins::ModuleObject, builtins.nameOf(byteCast<uint8_t>(name.span())));
        if (!member || isUsedInModuleEvaluationPlan(facts, member))
            return true;
    }
    return false;
}

// Code that nobody has read has import() and require().
static bool unseenCodeMayLoadModules(const Facts& facts)
{
    return facts.unseenCodeCanRun() || facts.unseenScriptsCanRun() || !facts.unseenCode().empty();
}

// ---- The planner

namespace {

// The lists of steps are numbered: first the list of each module, then the list of each bare site.
struct EvaluationStepRecord {
    uint32_t list;
    ModuleStep step;
};

struct PlannedStartOfModule {
    uint32_t module { 0 };
    uint32_t numberOfStepsBeforeItBegins { 0 };
    uint32_t firstFileRunWhileLoaded { 0 };
    uint32_t numberOfFilesRunWhileLoaded { 0 };
    bool mayBeLeftByCaughtError { false };
    bool beginsInBody { false };
};

// Lists and sets are by the number of the module.
struct ModuleEvaluationPlanner {
    enum Trait : uint8_t {
        HasCode = 1 << 0,
        BodyOnlyReturns = 1 << 1,
        BodyCanBeCalledByCode = 1 << 2,
        IsFaceOfFile = 1 << 3,
    };

    ModuleEvaluationPlanner(const Program& program, const Facts& facts, const ParallelLoops& loops)
        : program(program)
        , facts(facts)
        , loops(loops)
        , numberOfModules(static_cast<uint32_t>(program.modules.size()))
    {
    }

    void run();

    bool has(size_t module, Trait trait) const { return traits[module] & trait; }
    std::span<const uint32_t> importsOf(size_t module) const { return imports.subspan(firstImport[module], firstImport[module + 1] - firstImport[module]); }
    std::span<const FoundModuleSite> bareSitesOf(size_t module) const { return bareSites.subspan(firstBareSiteOf[module], numberOfBareSitesOf[module]); }
    uint32_t numberOfStepsOfList(uint32_t list) const { return firstStepOfList[list + 1] - firstStepOfList[list]; }
    std::span<const ModuleStep> stepsOfList(uint32_t list) const { return steps.subspan(firstStepOfList[list], numberOfStepsOfList(list)); }
    bool recordsNothingThatIsSeen(uint32_t module) const;

    static ValueID valueInFrontOfCasts(const BodyIndex&, ValueID);
    bool canBeCalledByCode(FunctionID body) const;
    bool isEvaluatedWithImporter(uint32_t module) const;
    bool needsLoaderItself(uint32_t module) const;
    bool hasLinkWorkItself(uint32_t module) const;
    bool runsCode(uint32_t module) const { return isEvaluatedWithImporter(module) && has(module, HasCode) && !has(module, BodyOnlyReturns); }
    bool importsCommonJSBeforeItBegins(size_t module) const { return isAskedOfLoader.quickGet(module) && program.modules[module].kind == ModuleKind::SourceText; }
    FoundModuleSiteKind kindInPlanOf(const FoundModuleSite&) const;

    template<typename Functor> BitVector modulesThatReach(const StronglyConnectedComponents&, const Functor& hasProperty) const;
    BitVector filesThatRequireModuleThatAwaits(const StronglyConnectedComponents&) const;
    bool hostLoadsSeveralGraphsInOneGlobal() const;
    bool mayLoadTwoGraphsAtOnce() const;
    bool mayLoadModulesByName() const;

    void collectImports();
    void describeModules();
    void chooseModulesWalkedByCode();
    void sortSitesByKind();
    void findWhereEvaluationsStart();
    void findWhetherRequireEvaluatesESModule();
    void findModulesWithoutState();
    void findModulesLeftByCaughtErrors();
    void walkFrom(uint32_t first);
    void sortStepsByList();
    void planStarts();

    void addStep(uint32_t list, const ModuleStep& step) { records.append(EvaluationStepRecord { list, step }); }

    const Program& program;
    const Facts& facts;
    const ParallelLoops& loops;
    const uint32_t numberOfModules;

    Vector<uint8_t> traits;
    // The requests that run in front of the body, in their order. A request that a `require` makes runs where the `require` stands.
    Vector<uint32_t> firstImport;
    Vector<uint32_t> imports;
    // By function.
    Vector<ModuleID> moduleWithBody;
    bool somebodyAsksWhoRequiredWhom { true };

    // To evaluate it is to call functions: the run time's own algorithm has nothing to wait for, to ask the host for or to find out.
    BitVector isWalkedByCode;
    // And it has code to run, or leads to some.
    BitVector startsItself;
    // For these the loader would do nothing but evaluate them.
    BitVector isCalledWhereRequired;
    Vector<uint32_t> componentOfImports;
    BitVector hasNoState;

    Vector<FoundModuleSite> sites;
    // By module, in the order in which the body comes to them.
    Vector<FoundModuleSite> bareSites;
    Vector<uint32_t> firstBareSiteOf;
    Vector<uint32_t> numberOfBareSitesOf;

    // Something other than a walk of compiled code may be the first to come to it.
    BitVector isStart;
    BitVector isRequiredLate;
    BitVector isAskedOfLoader;
    BitVector isAskedForByHost;
    bool noRequireEvaluatesESModule { false };
    BitVector mayBeLeftByCaughtError;
    BitVector startIsCalled;

    Vector<uint32_t> walkThatVisited;
    Vector<EvaluationStepRecord> records;
    Vector<ModuleStep> steps;
    Vector<uint32_t> firstStepOfList;

    Vector<PlannedStartOfModule> starts;
    Vector<ModuleID> filesRunWhileLoaded;
};

} // anonymous namespace

bool bodyOfModuleOnlyReturns(const Body& body)
{
    if (body.numberOfBlocks() != 1)
        return false;
    BodyBlock block = body.entry();
    for (BodyValue value : block.values()) {
        if (value.opcode() != Opcode::Undefined)
            return false;
    }
    return block.terminator().kind() == TerminatorKind::Return;
}

ValueID ModuleEvaluationPlanner::valueInFrontOfCasts(const BodyIndex& body, ValueID id)
{
    while (id) {
        BodyValue value = body.value(id);
        if (value.opcode() == Opcode::Convert)
            id = value.as<Opcode::Convert>().valueOperand();
        else if (value.opcode() == Opcode::Upcast)
            id = value.as<Opcode::Upcast>().valueOperand();
        else
            break;
    }
    return id;
}

// Whoever evaluates a module has nothing to pass but `this`, which is undefined in an ES module. The body of a CommonJS module takes nothing.
// A start returns undefined, whether or not it runs the body.
bool ModuleEvaluationPlanner::canBeCalledByCode(FunctionID id) const
{
    const Function& function = program.at(id);
    if (function.role != FunctionRole::Source || !function.isModuleBody() || function.isCoroutine() || function.results.size() != 1)
        return false;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        if (function.entryParameterKind(index) != EntryParameter::This)
            return false;
    }
    const BodyIndex* body = facts.bodies().find(id);
    if (!body)
        return false;
    for (uint32_t block = 0; block < body->numberOfBlocks(); ++block) {
        BodyTerminator terminator = body->terminator(BlockID(block));
        if (terminator.kind() == TerminatorKind::ReturnCall)
            return false;
        if (terminator.kind() != TerminatorKind::Return)
            continue;
        ValueID returned = valueInFrontOfCasts(*body, terminator.as<TerminatorKind::Return>().valueOperand());
        if (returned && body->opcodeOf(returned) != Opcode::Undefined)
            return false;
    }
    return true;
}

// A module of any other kind ran when its importer was linked.
bool ModuleEvaluationPlanner::isEvaluatedWithImporter(uint32_t module) const
{
    switch (program.modules[module].kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        return true;
    case ModuleKind::CommonJs:
        return !has(module, HasCode) || has(module, BodyCanBeCalledByCode);
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The run time's own algorithm waits for promises, asks the host for code that was not compiled, knows whether a mock stands in place of a module, and finds out what a deferred import has to evaluate.
bool ModuleEvaluationPlanner::needsLoaderItself(uint32_t index) const
{
    const Module& module = program.modules[index];
    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        break;
    case ModuleKind::External:
        return module.externalKind == ExternalKind::UncompiledCode;
    case ModuleKind::CommonJs:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
        return false;
    }
    if (module.flags.containsAny({ ModuleFlags::HasTopLevelAwait, ModuleFlags::IsLoadedByLoader, ModuleFlags::MayBeMocked }))
        return true;
    if (std::ranges::any_of(module.requests, [](const ModuleRequest& request) { return request.phase == ImportPhase::Defer; }))
        return true;
    return has(index, HasCode) && !has(index, BodyCanBeCalledByCode);
}

// What the loader does for a module before it evaluates it: ownPropertiesOf() in AOTModuleGraph.cpp finds the same in the tables.
bool ModuleEvaluationPlanner::hasLinkWorkItself(uint32_t index) const
{
    const Module& module = program.modules[index];
    if (!module.syntheticExports.isEmpty() || !module.requiredNames.isEmpty())
        return true;
    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
        return module.exportRule != SyntheticExportRule::None;
    case ModuleKind::CommonJs:
        return !isEvaluatedWithImporter(index);
    case ModuleKind::External:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A walk comes to a module once, so of two bare sites the first alone calls it. `require("./a.cjs"); delete require.cache[path]; require("./a.cjs")` runs the file at both: each site asks the status of the file.
FoundModuleSiteKind ModuleEvaluationPlanner::kindInPlanOf(const FoundModuleSite& site) const
{
    if (site.kind == FoundModuleSiteKind::Bare && program.at(site.target).flags.containsAny({ ModuleFlags::MayBeEvicted, ModuleFlags::RunsAgain }))
        return FoundModuleSiteKind::Late;
    return site.kind;
}

// What is recorded is who required whom and, of an ES module, the mark that require() leaves on the namespace object of a module that does not export `__esModule` itself.
bool ModuleEvaluationPlanner::recordsNothingThatIsSeen(uint32_t index) const
{
    if (somebodyAsksWhoRequiredWhom)
        return false;
    const Module& module = program.modules[index];
    if (module.kind != ModuleKind::SourceText || std::ranges::contains(module.exports, program.stringOf(BuiltinNames::__esModule), &ModuleExport::name))
        return true;
    // Nobody follows the namespace object to whoever may look for the mark.
    return false;
}

// The modules that have the property themselves or import, directly or not, a module that has it.
template<typename Functor>
BitVector ModuleEvaluationPlanner::modulesThatReach(const StronglyConnectedComponents& components, const Functor& hasProperty) const
{
    const Vector<uint32_t>& componentOf = components.componentOf;

    // A component has a higher number than every component it imports.
    BitVector reachingComponents(components.numberOfComponents);
    for (uint32_t index : components.members) {
        bool reaches = hasProperty(index) || std::ranges::any_of(importsOf(index), [&](uint32_t target) {
            return reachingComponents.quickGet(componentOf[target]);
        });
        if (reaches)
            reachingComponents.quickSet(componentOf[index]);
    }

    BitVector reachingModules(numberOfModules);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (reachingComponents.quickGet(componentOf[index]))
            reachingModules.quickSet(index);
    }
    return reachingModules;
}

// The CommonJS files, and their faces. The body of a face evaluates its file.
BitVector ModuleEvaluationPlanner::filesThatRequireModuleThatAwaits(const StronglyConnectedComponents& components) const
{
    auto awaitsItself = [&](uint32_t index) {
        return program.modules[index].flags.contains(ModuleFlags::HasTopLevelAwait);
    };
    BitVector files(numberOfModules);
    bool someModuleAwaits = false;
    for (uint32_t index = 0; index < numberOfModules && !someModuleAwaits; ++index)
        someModuleAwaits = awaitsItself(index);
    if (!someModuleAwaits)
        return files;

    BitVector awaits = modulesThatReach(components, awaitsItself);
    Vector<uint32_t> referrers = collectInParallel<uint32_t>(loops, program.functions.size(), [&](unsigned index, Vector<uint32_t>& found) {
        const BodyIndex* body = facts.bodies().find(FunctionID(index));
        if (!body || !body->holdsAnyOf({ Opcode::RequireModule }))
            return;
        body->body().forEachBlock([&](BodyBlock block) {
            for (BodyValue value : block.values()) {
                if (value.opcode() != Opcode::RequireModule)
                    continue;
                auto require = value.as<Opcode::RequireModule>();
                if (awaits.quickGet(require.module().index()) && require.referrer())
                    found.append(require.referrer().index());
            }
        });
    });
    for (uint32_t referrer : referrers)
        files.quickSet(referrer);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (has(index, IsFaceOfFile) && files.quickGet(program.modules[index].requests[0].target.index()))
            files.quickSet(index);
    }
    return files;
}

// The host starts a global with one entry point, and a Worker has a global and a loader of its own. Whoever else comes to an entry point names it in a load, which mayLoadTwoGraphsAtOnce() counts by itself.
// The runner of a test run starts its entry points in one global, one behind the other, and some of them again. The `preload` among the options of a Worker names files that run ahead of its own.
// Jobs that the earlier file has left run among the steps of the next load.
bool ModuleEvaluationPlanner::hostLoadsSeveralGraphsInOneGlobal() const
{
    bool someEntryRunsAgain = std::ranges::any_of(program.entries, [&](const Entry& entry) {
        return program.at(entry.module).flags.contains(ModuleFlags::RunsAgain);
    });
    if (someEntryRunsAgain)
        return true;
    // The loader answers the host with an entry point under any name, and with another file of the image only where the program itself loads files by name.
    if (program.entries.size() <= 1 && !facts.findsFilesOfProgramByName())
        return false;
    return program.admitsReplacedBuiltins || facts.workerStartWithoutName().has_value() || std::ranges::any_of(facts.workerEntries(), &WorkerEntry::hasOptions);
}

// `bun run` loads the graph of a required ES module in steps. A program can tell only where another graph is being loaded at that time, or waits.
// An import() in code that nobody has read starts a load as ImportDynamic does.
bool ModuleEvaluationPlanner::mayLoadTwoGraphsAtOnce() const
{
    if (unseenCodeMayLoadModules(facts) || hostLoadsSeveralGraphsInOneGlobal())
        return true;
    bool someModuleWaits = std::ranges::any_of(program.modules, [](const Module& module) {
        return module.flags.containsAny({ ModuleFlags::HasTopLevelAwait, ModuleFlags::IsLoadedByLoader }) || (module.kind == ModuleKind::External && module.externalKind == ExternalKind::UncompiledCode);
    });
    if (someModuleWaits)
        return true;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const BodyIndex* body = facts.bodies().find(FunctionID(index));
        if (body && body->holdsAnyOf({ Opcode::ImportModule, Opcode::ImportDynamic }))
            return true;
    }
    return false;
}

// Without a record that says so the run time turns away every name that reaches a loader.
bool ModuleEvaluationPlanner::mayLoadModulesByName() const
{
    if (facts.findsFilesOfProgramByName())
        return true;
    return std::ranges::any_of(facts.unseenCode(), [](const UnseenCodeRecord& record) {
        switch (record.cause) {
        case UnseenCodeCause::Unknown:
        case UnseenCodeCause::LoadsModuleByName:
        case UnseenCodeCause::OptionAdmitsCode:
        case UnseenCodeCause::FlowResultDisabled:
        case UnseenCodeCause::LoadsCodeByName:
            return true;
        case UnseenCodeCause::EvaluatesText:
        case UnseenCodeCause::ImportsUncompiledModule:
        case UnseenCodeCause::RunsUnseenCode:
        case UnseenCodeCause::RunsMachineCode:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

void ModuleEvaluationPlanner::collectImports()
{
    firstImport.reserveInitialCapacity(numberOfModules + 1);
    for (const Module& module : program.modules) {
        firstImport.append(static_cast<uint32_t>(imports.size()));
        for (const ModuleRequest& request : module.requests) {
            if (request.kind == ModuleRequestKind::Import)
                imports.append(request.target.index());
        }
    }
    firstImport.append(static_cast<uint32_t>(imports.size()));
}

void ModuleEvaluationPlanner::describeModules()
{
    traits.fill(0, numberOfModules);
    forEachIndexInParallel(loops, numberOfModules, [&](unsigned index) {
        const Module& module = program.modules[index];
        uint8_t found = 0;
        if (isFaceOfCommonJSFile(program, module))
            found |= IsFaceOfFile;
        if (module.body && program.at(module.body).hasBody()) {
            found |= HasCode;
            if (bodyOfModuleOnlyReturns(program.at(module.body).body))
                found |= BodyOnlyReturns;
            if (canBeCalledByCode(module.body))
                found |= BodyCanBeCalledByCode;
        }
        traits[index] = found;
    });
}

void ModuleEvaluationPlanner::chooseModulesWalkedByCode()
{
    const Vector<Module>& modules = program.modules;
    StronglyConnectedComponents components = computeStronglyConnectedComponents(numberOfModules, [&](unsigned index) {
        return importsOf(index);
    });
    // A `require` of a module that needs the loader asks the loader where it stands. The module that holds it waits for nothing.
    BitVector needsLoader = modulesThatReach(components, [&](uint32_t index) {
        return needsLoaderItself(index);
    });
    // A `require` of a module that awaits throws. What becomes of the importers of a file that throws is the loader's to say.
    if (BitVector files = filesThatRequireModuleThatAwaits(components); !files.isEmpty()) {
        needsLoader = modulesThatReach(components, [&](uint32_t index) {
            return needsLoaderItself(index) || files.quickGet(index);
        });
        for (uint32_t index = 0; index < numberOfModules; ++index) {
            if (modules[index].kind == ModuleKind::CommonJs)
                needsLoader.quickClear(index);
        }
    }
    BitVector leadsToCode = modulesThatReach(components, [&](uint32_t index) {
        return runsCode(index);
    });
    BitVector needsLink = modulesThatReach(components, [&](uint32_t index) {
        return hasLinkWorkItself(index);
    });
    // What a require() evaluates is evaluated for good when it returns, whatever becomes of a cycle that it has left. The run time tells the two evaluations apart: Modules::callBodyUnderOwnMark().
    BitVector leadsToCycle = modulesThatReach(components, [&](uint32_t index) {
        return components.membersOf(components.componentOf[index]).size() > 1;
    });

    isWalkedByCode.ensureSize(numberOfModules);
    startsItself.ensureSize(numberOfModules);
    isCalledWhereRequired.ensureSize(numberOfModules);
    moduleWithBody.fill(ModuleID(), program.functions.size());
    bool loadsTwoGraphsAtOnce = mayLoadTwoGraphsAtOnce();
    somebodyAsksWhoRequiredWhom = asksWhoRequiredWhom(program, facts);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        const Module& module = modules[index];
        if (!isEvaluatedWithImporter(index) || needsLoader.quickGet(index))
            continue;
        isWalkedByCode.quickSet(index);
        // Whoever enters a graph of modules in which something is to be linked goes through the loader: the host with an entry point, import(), require().
        // So does a require() of an ES module where another graph may be in the middle of its load: the run time knows whether one is.
        bool isRequiredBetweenLoads = module.kind != ModuleKind::SourceText || (!loadsTwoGraphsAtOnce && !module.flags.contains(ModuleFlags::IsRequiredDuringLoad));
        // A file that the loader has asked for has no parent, and the run time knows which of several files it has asked for.
        bool loaderIsSeenToAsk = somebodyAsksWhoRequiredWhom && module.requests.size() > 1 && module.requests[1].isHoisted;
        bool isListedFromRequireOn = module.kind == ModuleKind::SourceText && module.flags.contains(ModuleFlags::IsListedInCache);
        if (!needsLink.quickGet(index) && !leadsToCycle.quickGet(index) && isRequiredBetweenLoads && !loaderIsSeenToAsk && !isListedFromRequireOn)
            isCalledWhereRequired.quickSet(index);
        if (!has(index, HasCode))
            continue;
        moduleWithBody[module.body.index()] = ModuleID(index);
        if (leadsToCode.quickGet(index))
            startsItself.quickSet(index);
    }
    componentOfImports = WTF::move(components.componentOf);
}

void ModuleEvaluationPlanner::sortSitesByKind()
{
    firstBareSiteOf.fill(0, numberOfModules);
    numberOfBareSitesOf.fill(0, numberOfModules);
    isStart.ensureSize(numberOfModules);
    isRequiredLate.ensureSize(numberOfModules);
    isAskedOfLoader.ensureSize(numberOfModules);
    for (const FoundModuleSite& site : sites) {
        uint32_t target = site.target.index();
        switch (kindInPlanOf(site)) {
        case FoundModuleSiteKind::Bare: {
            ModuleID referrer = moduleWithBody[site.function.index()];
            RELEASE_ASSERT(referrer && isCalledWhereRequired.quickGet(target));
            if (!numberOfBareSitesOf[referrer.index()])
                firstBareSiteOf[referrer.index()] = static_cast<uint32_t>(bareSites.size());
            RELEASE_ASSERT(firstBareSiteOf[referrer.index()] + numberOfBareSitesOf[referrer.index()] == bareSites.size());
            ++numberOfBareSitesOf[referrer.index()];
            bareSites.append(site);
            continue;
        }
        case FoundModuleSiteKind::Late:
            if (isCalledWhereRequired.quickGet(target))
                isRequiredLate.quickSet(target);
            else
                isAskedOfLoader.quickSet(target);
            break;
        // The walk calls nothing at a RequireModule: the run time evaluates what it names.
        case FoundModuleSiteKind::Required:
        case FoundModuleSiteKind::Named:
            isAskedOfLoader.quickSet(target);
            break;
        case FoundModuleSiteKind::Record:
            continue;
        }
        isStart.quickSet(target);
    }
}

void ModuleEvaluationPlanner::findWhereEvaluationsStart()
{
    const Vector<Module>& modules = program.modules;
    // A mock marks a CommonJS file as evaluated before anything has run it, so the file has a status.
    // Once the path of the main module is stored anew, `require.main` answers the object of any file that has begun, which is read from its status: Modules::mainModuleObject().
    bool anyFileMayBeMain = facts.storesPathOfMainModule();
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (modules[index].kind == ModuleKind::CommonJs && (anyFileMayBeMain || modules[index].flags.contains(ModuleFlags::MayBeMocked)))
            isStart.quickSet(index);
    }
    isAskedForByHost = modulesThatHostAsksFor(program, facts.findsFilesOfProgramByName());
    // A Worker asks the loader of its own global for its file, whether or not the front end took the file for an entry point.
    BitVector stringsThatArePathsOfWorkers;
    for (const WorkerEntry& worker : facts.workerEntries()) {
        if (worker.path)
            stringsThatArePathsOfWorkers.set(worker.path.index());
    }
    if (!stringsThatArePathsOfWorkers.isEmpty()) {
        for (uint32_t index = 0; index < numberOfModules; ++index) {
            if (stringsThatArePathsOfWorkers.get(modules[index].path.index()))
                isAskedForByHost.quickSet(index);
        }
    }
    isAskedOfLoader.merge(isAskedForByHost);
    // What a required module imports is evaluated with it.
    Vector<uint32_t, 16> reachedByRequire;
    for (size_t index : isAskedOfLoader)
        reachedByRequire.append(static_cast<uint32_t>(index));
    while (!reachedByRequire.isEmpty()) {
        for (uint32_t target : importsOf(reachedByRequire.takeLast())) {
            if (modules[target].kind == ModuleKind::SourceText && !isAskedOfLoader.quickSet(target))
                reachedByRequire.append(target);
        }
    }
    isStart.merge(isAskedForByHost);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        // `bun run` runs a CommonJS file that is imported while it links the importer, which has not begun by then: a `require` of the importer in that file evaluates the importer there and then, once.
        if (importsCommonJSBeforeItBegins(index)) {
            for (uint32_t target : importsOf(index)) {
                if (modules[target].kind == ModuleKind::CommonJs || has(target, IsFaceOfFile))
                    isStart.quickSet(target);
            }
        }
        // Behind the evaluation of a module that takes its names from an exports object the run time fills them in. It finds the place by the state.
        if ((modules[index].kind == ModuleKind::SourceText && modules[index].exportRule != SyntheticExportRule::None) || (modules[index].kind == ModuleKind::CommonJs && !modules[index].syntheticExports.isEmpty()))
            isStart.quickSet(index);
        if (isWalkedByCode.quickGet(index))
            continue;
        for (const ModuleRequest& request : modules[index].requests)
            isStart.quickSet(request.target.index());
    }
    // Whether the loader came to a file before a `require` did is read from the state of the file.
    if (somebodyAsksWhoRequiredWhom) {
        BitVector isImported(numberOfModules);
        for (uint32_t target : imports)
            isImported.quickSet(has(target, IsFaceOfFile) ? modules[target].requests[0].target.index() : target);
        for (const FoundModuleSite& site : sites) {
            if (site.kind == FoundModuleSiteKind::Record && isImported.quickGet(site.target.index()) && program.at(site.target).kind == ModuleKind::CommonJs)
                isStart.quickSet(site.target.index());
        }
    }
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        // A module that runs again comes once more to what it imports, and reads from the state that it has run.
        // In place of a module that was evicted its text from disk runs, and asks the loader for what it imports.
        if (modules[index].flags.containsAny({ ModuleFlags::RunsAgain, ModuleFlags::MayBeEvicted })) {
            if (modules[index].kind == ModuleKind::CommonJs && modules[index].flags.contains(ModuleFlags::RunsAgain))
                isStart.quickSet(index);
            for (const ModuleRequest& request : modules[index].requests)
                isStart.quickSet(request.target.index());
            for (const FoundModuleSite& site : bareSitesOf(index))
                isStart.quickSet(site.target.index());
        }
        // What `require` answers for a file of data goes by whether a module that imports the file has been loaded, which is read from the state of that module: Modules::isHeldForImport().
        if (modules[index].kind != ModuleKind::SourceText)
            continue;
        for (const ModuleRequest& request : modules[index].requests) {
            if (request.kind == ModuleRequestKind::Import && program.at(request.target).requireResult == RequireResult::NamespaceIfImported)
                isStart.quickSet(index);
        }
    }
}

// Only the body of a module that awaits is asked about.
void ModuleEvaluationPlanner::findWhetherRequireEvaluatesESModule()
{
    const Vector<Module>& modules = program.modules;
    bool someModuleAwaits = std::ranges::any_of(modules, [](const Module& module) {
        return module.flags.contains(ModuleFlags::HasTopLevelAwait);
    });
    // Code that nobody has read may require a module that nobody has read.
    if (!someModuleAwaits || unseenCodeMayLoadModules(facts))
        return;

    // Only the host knows of what kind code is that was not compiled.
    auto mayBeESModule = [](const Module& module) {
        return module.kind == ModuleKind::SourceText || (module.kind == ModuleKind::External && module.externalKind == ExternalKind::UncompiledCode);
    };
    for (const FoundModuleSite& site : sites) {
        switch (site.kind) {
        case FoundModuleSiteKind::Bare:
        case FoundModuleSiteKind::Late:
        case FoundModuleSiteKind::Required:
        case FoundModuleSiteKind::Record:
            if (mayBeESModule(program.at(site.target)))
                return;
            break;
        case FoundModuleSiteKind::Named:
            // The loader evaluates these and waits for them.
            break;
        }
    }
    for (const Module& module : modules) {
        for (const ModuleRequest& request : module.requests) {
            if (request.kind == ModuleRequestKind::Require && mayBeESModule(program.at(request.target)))
                return;
        }
    }
    // A name made at run time finds what the host asks for. Where that is one module and nothing else, every global begins with it, before any require() runs.
    if (isAskedForByHost.bitCount() > 1 && mayLoadModulesByName()) {
        for (size_t index : isAskedForByHost) {
            if (mayBeESModule(modules[index]))
                return;
        }
    }
    noRequireEvaluatesESModule = true;
}

// A module needs no state where every way to it leads through one module that starts itself: that module runs its steps once, and this one with them.
// A module that has nothing to run guards nothing: whoever comes to it goes on to what it imports.
void ModuleEvaluationPlanner::findModulesWithoutState()
{
    constexpr uint32_t none = UINT32_MAX;
    const uint32_t root = numberOfModules;
    const uint32_t numberOfNodes = numberOfModules + 1;

    Vector<uint32_t> firstSuccessor;
    Vector<uint32_t> successors;
    firstSuccessor.reserveInitialCapacity(numberOfNodes + 1);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        firstSuccessor.append(static_cast<uint32_t>(successors.size()));
        if (!isWalkedByCode.quickGet(index))
            continue;
        for (uint32_t target : importsOf(index)) {
            if (isWalkedByCode.quickGet(target))
                successors.append(target);
        }
        for (const FoundModuleSite& site : bareSitesOf(index))
            successors.append(site.target.index());
    }
    firstSuccessor.append(static_cast<uint32_t>(successors.size()));

    // Nothing that is seen here comes to the rest. Whatever does finds a module with a state.
    BitVector isReached(numberOfModules);
    Vector<uint32_t> work;
    for (size_t index : isStart) {
        if (isWalkedByCode.quickGet(index) && !isReached.quickSet(index))
            work.append(static_cast<uint32_t>(index));
    }
    while (!work.isEmpty()) {
        uint32_t node = work.takeLast();
        for (uint32_t edge = firstSuccessor[node]; edge < firstSuccessor[node + 1]; ++edge) {
            if (!isReached.quickSet(successors[edge]))
                work.append(successors[edge]);
        }
    }
    for (size_t index : isWalkedByCode) {
        if (!isReached.quickGet(index))
            isStart.quickSet(index);
    }
    for (size_t index : isStart) {
        if (isWalkedByCode.quickGet(index))
            successors.append(static_cast<uint32_t>(index));
    }
    firstSuccessor.append(static_cast<uint32_t>(successors.size()));

    Vector<uint32_t> firstPredecessor;
    firstPredecessor.fill(0, numberOfNodes + 1);
    for (uint32_t successor : successors)
        ++firstPredecessor[successor + 1];
    for (uint32_t node = 0; node < numberOfNodes; ++node)
        firstPredecessor[node + 1] += firstPredecessor[node];
    Vector<uint32_t> predecessors;
    predecessors.fill(0, successors.size());
    Vector<uint32_t> nextPredecessor = firstPredecessor;
    for (uint32_t node = 0; node < numberOfNodes; ++node) {
        for (uint32_t edge = firstSuccessor[node]; edge < firstSuccessor[node + 1]; ++edge)
            predecessors[nextPredecessor[successors[edge]]++] = node;
    }

    struct Frame {
        uint32_t node;
        uint32_t nextEdge;
    };
    Vector<uint32_t> postorderNumber;
    postorderNumber.fill(none, numberOfNodes);
    Vector<uint32_t> nodesInPostorder;
    BitVector isSeen(numberOfNodes);
    isSeen.quickSet(root);
    Vector<Frame, 64> path { Frame { root, firstSuccessor[root] } };
    while (!path.isEmpty()) {
        Frame& frame = path.last();
        if (frame.nextEdge == firstSuccessor[frame.node + 1]) {
            postorderNumber[frame.node] = static_cast<uint32_t>(nodesInPostorder.size());
            nodesInPostorder.append(frame.node);
            path.removeLast();
            continue;
        }
        uint32_t successor = successors[frame.nextEdge++];
        if (!isSeen.quickSet(successor))
            path.append(Frame { successor, firstSuccessor[successor] });
    }
    ASSERT(nodesInPostorder.last() == root);

    // Cooper, Harvey and Kennedy. On a graph without cycles the first round finds the answer and the second sees that nothing changes.
    Vector<uint32_t> dominator;
    dominator.fill(none, numberOfNodes);
    dominator[root] = root;
    auto nearestCommonDominator = [&](uint32_t first, uint32_t second) {
        while (first != second) {
            while (postorderNumber[first] < postorderNumber[second])
                first = dominator[first];
            while (postorderNumber[second] < postorderNumber[first])
                second = dominator[second];
        }
        return first;
    };
    for (bool didChange = true; didChange;) {
        didChange = false;
        for (size_t place = nodesInPostorder.size() - 1; place--;) {
            uint32_t node = nodesInPostorder[place];
            uint32_t found = none;
            for (uint32_t edge = firstPredecessor[node]; edge < firstPredecessor[node + 1]; ++edge) {
                uint32_t predecessor = predecessors[edge];
                if (dominator[predecessor] == none)
                    continue;
                found = found == none ? predecessor : nearestCommonDominator(predecessor, found);
            }
            if (dominator[node] != found) {
                dominator[node] = found;
                didChange = true;
            }
        }
    }

    // A dominator stands in front of what it dominates.
    hasNoState.ensureSize(numberOfModules);
    for (size_t place = nodesInPostorder.size() - 1; place--;) {
        uint32_t node = nodesInPostorder[place];
        uint32_t above = dominator[node];
        if (above != root && (startsItself.quickGet(above) || hasNoState.quickGet(above)))
            hasNoState.quickSet(node);
    }
    // The loader goes through a module that has nothing to run, and calls what that module imports.
    for (size_t index : isWalkedByCode) {
        if (hasNoState.quickGet(index) || startsItself.quickGet(index))
            continue;
        for (uint32_t edge = firstSuccessor[index]; edge < firstSuccessor[index + 1]; ++edge) {
            ASSERT(!hasNoState.quickGet(successors[edge]));
            isStart.quickSet(successors[edge]);
        }
    }
}

// What compiled code starts where a handler of the program may stand around it, and everything that is evaluated below that.
void ModuleEvaluationPlanner::findModulesLeftByCaughtErrors()
{
    mayBeLeftByCaughtError = isRequiredLate;
    Vector<uint32_t, 32> work;
    for (size_t index : mayBeLeftByCaughtError)
        work.append(static_cast<uint32_t>(index));
    auto addToWork = [&](ModuleID target) {
        if (!mayBeLeftByCaughtError.quickSet(target.index()))
            work.append(target.index());
    };
    while (!work.isEmpty()) {
        uint32_t index = work.takeLast();
        for (const ModuleRequest& request : program.modules[index].requests)
            addToWork(request.target);
        for (const FoundModuleSite& site : bareSitesOf(index))
            addToWork(site.target);
    }
}

// Requests in their order, depth first, each module once. It fills the list of the first module and the lists of the bare sites of the modules without a state that it alone comes to.
void ModuleEvaluationPlanner::walkFrom(uint32_t first)
{
    struct Frame {
        uint32_t module;
        uint32_t nextEdge;
        // Of the first module, or of the bare site that led here.
        uint32_t list;
        // It lies on a cycle of imports with its importer, which keeps no status either: both have run when the one that the walk entered first has. Of a module with a status the run time knows: Modules::hasRun().
        bool hasRunWithImporter { false };
        uint32_t numberThatWaitedAtEntry { 0 };
    };
    const Vector<Module>& modules = program.modules;
    const uint32_t mark = first + 1;
    Vector<uint32_t, 8> waitForCycle;

    walkThatVisited[first] = mark;
    Vector<Frame, 32> path { Frame { first, 0, first } };
    while (!path.isEmpty()) {
        Frame& frame = path.last();
        uint32_t index = frame.module;
        uint32_t list = frame.list;
        std::span<const uint32_t> importsOfModule = importsOf(index);
        uint32_t edge = frame.nextEdge++;
        if (edge == importsOfModule.size() + numberOfBareSitesOf[index]) {
            bool isListed = index != first && modules[index].flags.contains(ModuleFlags::IsListedInCache);
            if (index != first && hasNoState.quickGet(index) && startsItself.quickGet(index) && !has(index, BodyOnlyReturns)) {
                // A CommonJS file finds its own module in require.cache while it runs.
                if (isListed && modules[index].kind == ModuleKind::CommonJs)
                    addStep(list, ModuleStep { .moduleThatBegins = ModuleID(index) });
                addStep(list, ModuleStep { .callee = modules[index].body });
            }
            // require.cache of a test run lists the files that have run.
            if (isListed)
                waitForCycle.append(index);
            if (!frame.hasRunWithImporter) {
                for (uint32_t ran : waitForCycle.span().subspan(frame.numberThatWaitedAtEntry))
                    addStep(list, ModuleStep { .moduleThatHasRun = ModuleID(ran) });
                waitForCycle.shrink(frame.numberThatWaitedAtEntry);
            }
            path.removeLast();
            continue;
        }

        uint32_t target;
        if (edge < importsOfModule.size())
            target = importsOfModule[edge];
        else {
            uint32_t site = firstBareSiteOf[index] + (edge - static_cast<uint32_t>(importsOfModule.size()));
            list = numberOfModules + site;
            target = bareSites[site].target.index();
        }
        if (walkThatVisited[target] == mark)
            continue;
        walkThatVisited[target] = mark;
        // A module that has nothing to run passes the walk on.
        if (hasNoState.quickGet(target) || (isWalkedByCode.quickGet(target) && !startsItself.quickGet(target))) {
            bool hasRunWithImporter = index != first && edge < importsOfModule.size() && componentOfImports[target] == componentOfImports[index];
            path.append(Frame { target, 0, list, hasRunWithImporter, static_cast<uint32_t>(waitForCycle.size()) });
        } else if (startsItself.quickGet(target)) {
            startIsCalled.quickSet(target);
            addStep(list, ModuleStep { .callee = modules[target].body, .calleeIsStart = true });
        }
    }

    // It has nothing to run, so it has no start that would store a status: whoever requires it late marks it.
    if (!startsItself.quickGet(first) && modules[first].flags.contains(ModuleFlags::IsListedInCache))
        addStep(first, ModuleStep { .moduleThatHasRun = ModuleID(first) });
}

void ModuleEvaluationPlanner::sortStepsByList()
{
    uint32_t numberOfLists = numberOfModules + static_cast<uint32_t>(bareSites.size());
    firstStepOfList.fill(0, numberOfLists + 1);
    for (const EvaluationStepRecord& record : records)
        ++firstStepOfList[record.list + 1];
    for (uint32_t list = 0; list < numberOfLists; ++list)
        firstStepOfList[list + 1] += firstStepOfList[list];
    steps.fill(ModuleStep { }, records.size());
    Vector<uint32_t> nextStepOfList = firstStepOfList;
    for (const EvaluationStepRecord& record : records)
        steps[nextStepOfList[record.list]++] = record.step;
    records.clear();
}

void ModuleEvaluationPlanner::planStarts()
{
    const Vector<Module>& modules = program.modules;
    for (size_t index : isWalkedByCode) {
        if (hasNoState.quickGet(index) || !startsItself.quickGet(index))
            continue;
        const Module& module = modules[index];
        std::span<const ModuleStep> stepsOfModule = stepsOfList(static_cast<uint32_t>(index));
        // The loader calls a module that nothing of the image evaluates and that imports nothing, and there is no call to save.
        if (stepsOfModule.empty() && !startIsCalled.quickGet(index))
            continue;

        PlannedStartOfModule start;
        start.module = static_cast<uint32_t>(index);
        // An ES module that failed throws the same error at everybody who asks for it later. What a CommonJS module does is written in its record: a wrapper of a bundler counts as done once it has begun.
        bool keepsError = module.kind != ModuleKind::CommonJs || module.flags.contains(ModuleFlags::KeepsError);
        start.mayBeLeftByCaughtError = keepsError && mayBeLeftByCaughtError.quickGet(index);
        start.beginsInBody = module.kind == ModuleKind::CommonJs && stepsOfModule.empty() && !has(index, BodyOnlyReturns);
        start.firstFileRunWhileLoaded = static_cast<uint32_t>(filesRunWhileLoaded.size());
        if (importsCommonJSBeforeItBegins(index)) {
            auto firstOther = std::ranges::find_if(stepsOfModule, [&](const ModuleStep& step) {
                if (!step.callee)
                    return false;
                uint32_t called = moduleWithBody[step.callee.index()].index();
                bool runsCommonJS = modules[called].kind == ModuleKind::CommonJs || has(called, IsFaceOfFile);
                return !runsCommonJS || hasNoState.quickGet(called);
            });
            while (firstOther != stepsOfModule.begin() && !std::prev(firstOther)->callee)
                --firstOther;
            start.numberOfStepsBeforeItBegins = static_cast<uint32_t>(firstOther - stepsOfModule.begin());
            // Only where the run time calls the start to run the files alone, while it loads the module.
            if (isAskedForByHost.quickGet(index)) {
                for (const ModuleStep& step : stepsOfModule.first(start.numberOfStepsBeforeItBegins))
                    filesRunWhileLoaded.append(step.callee ? moduleWithBody[step.callee.index()] : ModuleID());
                start.numberOfFilesRunWhileLoaded = start.numberOfStepsBeforeItBegins;
            }
        }
        starts.append(start);
    }
}

void ModuleEvaluationPlanner::run()
{
    collectImports();
    describeModules();
    chooseModulesWalkedByCode();
    sites = findModuleSites(program, facts, loops, isCalledWhereRequired, moduleWithBody.span());
    sortSitesByKind();
    findWhereEvaluationsStart();
    findWhetherRequireEvaluatesESModule();
    findModulesWithoutState();
    findModulesLeftByCaughtErrors();

    startIsCalled = isRequiredLate;
    walkThatVisited.fill(0, numberOfModules);
    for (size_t index : isWalkedByCode) {
        if (!hasNoState.quickGet(index))
            walkFrom(static_cast<uint32_t>(index));
    }
    sortStepsByList();
    planStarts();
}

ModuleEvaluationPlan planModuleEvaluation(const Program& program, const Facts& facts, const ParallelLoops& loops, Statistics& statistics)
{
    uint32_t numberOfModules = static_cast<uint32_t>(program.modules.size());
    ModuleEvaluationParts plan;
    plan.stepsOfLateSites.fill({ }, numberOfModules);
    plan.modulesWhoseRequiresRecordNothingThatIsSeen.ensureSize(numberOfModules);
    plan.modulesEvaluatedByCodeAlone.ensureSize(numberOfModules);

    ModuleEvaluationPlanner planner(program, facts, loops);
    planner.run();

    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (planner.recordsNothingThatIsSeen(index))
            plan.modulesWhoseRequiresRecordNothingThatIsSeen.quickSet(index);
        if (planner.hasNoState.quickGet(index) && planner.has(index, ModuleEvaluationPlanner::HasCode)) {
            plan.modulesEvaluatedByCodeAlone.quickSet(index);
            statistics.count(StatisticID::ModulesWithoutState);
        }
        if (planner.isWalkedByCode.quickGet(index) && planner.has(index, ModuleEvaluationPlanner::HasCode)) {
            FunctionID body = program.modules[index].body;
            Effects effects = facts.effectsOnReturn(body);
            effects.merge(facts.effectsOnThrow(body));
            if (effects.isRemovableWhenUnused())
                plan.bodiesThatRowsLeaveOut.set(body.index());
        }
    }

    // siteOf() asks the loader at every site of a module that is not called where it is required, and no bare site names such a module.
    plan.modulesWhosePathNoSiteNames.ensureSize(numberOfModules);
    for (uint32_t index = 0; index < numberOfModules; ++index)
        plan.modulesWhosePathNoSiteNames.quickSet(index);
    for (const FoundModuleSite& site : planner.sites) {
        switch (site.kind) {
        case FoundModuleSiteKind::Bare:
        case FoundModuleSiteKind::Late:
            if (!planner.isCalledWhereRequired.quickGet(site.target.index()))
                plan.modulesWhosePathNoSiteNames.quickClear(site.target.index());
            else
                statistics.count(StatisticID::RequireSitesBound);
            break;
        case FoundModuleSiteKind::Record:
            if (!planner.recordsNothingThatIsSeen(site.target.index())) {
                plan.modulesWhosePathNoSiteNames.quickClear(site.target.index());
                plan.modulesWhosePathNoSiteNames.quickClear(site.referrer.index());
                statistics.count(StatisticID::RequiresRecorded);
            }
            break;
        case FoundModuleSiteKind::Required:
        case FoundModuleSiteKind::Named:
            break;
        }
    }

    plan.bareSites.reserveInitialCapacity(planner.bareSites.size());
    for (uint32_t site = 0; site < planner.bareSites.size(); ++site) {
        uint32_t list = numberOfModules + site;
        plan.bareSites.append(BareModuleSite { planner.bareSites[site].function, planner.bareSites[site].value, planner.firstStepOfList[list], planner.numberOfStepsOfList(list) });
    }
    RELEASE_ASSERT(std::ranges::is_sorted(plan.bareSites, { }, [](const BareModuleSite& site) {
        return std::pair { site.function, site.value };
    }));

    // What stands at a site that is not bare.
    for (size_t index : planner.isCalledWhereRequired) {
        if (planner.hasNoState.quickGet(index))
            continue;
        if (!planner.startsItself.quickGet(index)) {
            uint32_t list = static_cast<uint32_t>(index);
            plan.stepsOfLateSites[index] = { planner.firstStepOfList[list], planner.numberOfStepsOfList(list) };
            continue;
        }
        if (!planner.isRequiredLate.quickGet(index))
            continue;
        plan.stepsOfLateSites[index] = { static_cast<uint32_t>(planner.steps.size()), 1 };
        planner.steps.append(ModuleStep { .callee = program.modules[index].body, .calleeIsStart = true });
    }

    // From here on neither list grows: the starts point into them.
    plan.steps = WTF::move(planner.steps);
    plan.filesRunWhileLoaded = WTF::move(planner.filesRunWhileLoaded);
    plan.starts.reserveInitialCapacity(planner.starts.size());
    for (const PlannedStartOfModule& planned : planner.starts) {
        ModuleStart start;
        start.module = ModuleID(planned.module);
        start.body = program.modules[planned.module].body;
        start.steps = plan.steps.subspan(planner.firstStepOfList[planned.module], planner.numberOfStepsOfList(planned.module));
        start.numberOfStepsBeforeItBegins = planned.numberOfStepsBeforeItBegins;
        start.filesRunWhileLoaded = plan.filesRunWhileLoaded.subspan(planned.firstFileRunWhileLoaded, planned.numberOfFilesRunWhileLoaded);
        start.mayBeLeftByCaughtError = planned.mayBeLeftByCaughtError;
        start.beginsInBody = planned.beginsInBody;
        start.bodyOnlyReturns = planner.has(planned.module, ModuleEvaluationPlanner::BodyOnlyReturns);
        plan.starts.append(start);
        // Such a body stores the status of its module, of which Facts says nothing, and returns where the module has begun.
        if (start.beginsInBody)
            plan.bodiesThatRowsLeaveOut.clear(start.body.index());
        // Two walks meet at it, and that is all it has a state for. Nothing but the starts of other modules comes to it, and they call it.
        if (!planner.isStart.quickGet(planned.module))
            plan.modulesEvaluatedByCodeAlone.quickSet(planned.module);
    }
    plan.startsByBody = Vector<uint32_t>(plan.starts.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::sort(plan.startsByBody, { }, [&](uint32_t start) {
        return plan.starts[start].body;
    });

    plan.modulesCalledWhereRequired = WTF::move(planner.isCalledWhereRequired);
    plan.modulesThatStartThemselves = WTF::move(planner.startsItself);
    plan.noRequireEvaluatesESModule = planner.noRequireEvaluatesESModule;

    return ModuleEvaluationPlan(WTF::move(plan));
}

// ---- What the plan answers

ModuleSitePlan ModuleEvaluationPlan::siteOf(FunctionID function, ValueID evaluateModule, ModuleID target) const
{
    auto site = std::ranges::lower_bound(m_parts.bareSites, std::pair { function, evaluateModule }, { }, [](const BareModuleSite& candidate) {
        return std::pair { candidate.function, candidate.value };
    });
    if (site != m_parts.bareSites.end() && site->function == function && site->value == evaluateModule)
        return { false, stepsOf(*site) };
    if (!isCalledWhereRequired(target))
        return { };
    const StepsOfLateModuleSites& steps = m_parts.stepsOfLateSites[target.index()];
    // A module that has code to run and that no site but bare ones was seen to name may have no state: nothing here could tell whether it has run.
    RELEASE_ASSERT(steps.numberOfSteps || !startsItself(target));
    return { false, m_parts.steps.subspan(steps.firstStep, steps.numberOfSteps) };
}

const ModuleStart* ModuleEvaluationPlan::findStart(ModuleID module) const
{
    auto start = std::ranges::lower_bound(m_parts.starts, module, { }, &ModuleStart::module);
    return start != m_parts.starts.end() && start->module == module ? &*start : nullptr;
}

const ModuleStart* ModuleEvaluationPlan::findStartOfBody(FunctionID body) const
{
    auto place = std::ranges::lower_bound(m_parts.startsByBody, body, { }, [&](uint32_t start) {
        return m_parts.starts[start].body;
    });
    return place != m_parts.startsByBody.end() && m_parts.starts[*place].body == body ? &m_parts.starts[*place] : nullptr;
}

Vector<ModuleStartOperation, 8> ModuleEvaluationPlan::operationsOf(const ModuleStart& start) const
{
    using Kind = ModuleStartOperation::Kind;
    RELEASE_ASSERT(start.numberOfStepsBeforeItBegins <= start.steps.size());
    RELEASE_ASSERT(start.filesRunWhileLoaded.empty() || start.filesRunWhileLoaded.size() == start.numberOfStepsBeforeItBegins);

    Vector<ModuleStartOperation, 8> operations;
    auto appendStatusOperation = [&](Kind kind, ModuleID module) {
        operations.append(ModuleStartOperation { kind, FunctionID(), module });
    };
    auto appendCallOfBody = [&](FunctionID body) {
        if (!leavesOutCallOf(body))
            operations.append(ModuleStartOperation { Kind::CallBody, body, ModuleID() });
    };
    auto appendStep = [&](const ModuleStep& step) {
        if (step.callee) {
            if (step.calleeIsStart)
                operations.append(ModuleStartOperation { Kind::CallStart, step.callee, ModuleID() });
            else
                appendCallOfBody(step.callee);
            return;
        }
        if (step.moduleThatBegins) {
            appendStatusOperation(Kind::Begin, step.moduleThatBegins);
            return;
        }
        RELEASE_ASSERT(step.moduleThatHasRun);
        appendStatusOperation(Kind::Begin, step.moduleThatHasRun);
        appendStatusOperation(Kind::Finish, step.moduleThatHasRun);
    };

    for (uint32_t index = 0; index < start.numberOfStepsBeforeItBegins; ++index) {
        appendStep(start.steps[index]);
        if (!start.filesRunWhileLoaded.empty() && start.filesRunWhileLoaded[index])
            appendStatusOperation(Kind::ReturnIfLoadingStops, start.filesRunWhileLoaded[index]);
    }
    if (!start.beginsInBody)
        appendStatusOperation(Kind::BeginOrReturn, start.module);
    // Nothing in front of the evaluation is covered: the module has not begun yet.
    if (start.mayBeLeftByCaughtError)
        appendStatusOperation(Kind::AbandonOnThrow, start.module);
    for (const ModuleStep& step : start.steps.subspan(start.numberOfStepsBeforeItBegins))
        appendStep(step);
    if (!start.bodyOnlyReturns)
        appendCallOfBody(start.body);
    if (!start.beginsInBody)
        appendStatusOperation(Kind::Finish, start.module);
    return operations;
}

// A file that may be evicted is asked about where it is required.
bool ModuleEvaluationPlan::beginAsksRunTime(const Program& program, ModuleID module)
{
    return !program.sourceStamps.isEmpty() && !program.at(module).flags.containsAny({ ModuleFlags::IsLibrary, ModuleFlags::MayBeEvicted });
}

bool ModuleEvaluationPlan::finishLeavesStatusInRecord(const Program& program, ModuleID module) const
{
    OptionSet<ModuleFlags> flags = program.at(module).flags;
    bool evaluatesItself = findStart(module) && !isEvaluatedByCodeAlone(module);
    // A module without a start of its own is marked behind the site that requires it, whatever BeginModuleEvaluation has answered there: the loader may hold its status in a record.
    bool isMarkedWhereRequired = flags.contains(ModuleFlags::IsListedInCache) && !evaluatesItself;
    // The file may have taken itself out of require.cache while it ran. The mark of that stays, and the next require() asks the run time.
    return flags.contains(ModuleFlags::MayBeEvicted) || isMarkedWhereRequired;
}

bool ModuleEvaluationPlan::isBodyResumedByJobOfLoader(const Function& function) const
{
    return function.isModuleBody() && !m_parts.noRequireEvaluatesESModule;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
