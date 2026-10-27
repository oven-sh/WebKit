/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallModules.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTCFG.h"
#include "AOTDominators.h"
#include "AOTFlowResults.h"
#include "AOTFlowUnseenCode.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTModuleGraph.h"
#include "AOTSort.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using ModuleList = Vector<uint32_t, 4>;
using ModuleCallees = Vector<FunctionID, 1>;

enum class SiteKind : uint8_t {
    // EvaluateModule in the body of its referrer, where every run of the body passes once and no handler stands around it.
    Bare,
    // EvaluateModule anywhere else: in a function, a branch, a loop, under a handler.
    Late,
    Required,
    // Any other operation that may be the first to evaluate the module it names.
    Named,
    Record,
};

struct ModuleCallSite {
    FunctionID function;
    ValueID value;
    ModuleID target;
    SiteKind kind;
};

struct ModuleToStart {
    ModuleID module;
    FunctionID start;
    FunctionID code;
    bool mayBeLeftByCaughtError { false };
    unsigned numberOfStepsBeforeItBegins { 0 };
    bool beginsInCode { false };
    // By step before it begins: the file that the step runs. Only where the run time calls the start to run the files alone, while it loads the module.
    Vector<ModuleID> filesRunWhileLoaded { };
};

// What is found out about the modules before anything is changed. Lists and sets are by the number of the module.
struct EvaluationPlan {
    const Program& program;
    // The requests that run in front of the body, in their order. A request that a `require` makes runs where the `require` stands.
    Vector<ModuleList> imports;
    // To evaluate it is to call functions: the run time's own algorithm has nothing to wait for, to ask the host for or to find out.
    BitVector isWalkedByCode;
    // And it has code to run, or leads to some.
    BitVector startsItself;
    // For these the loader would do nothing but evaluate them.
    BitVector isCalledWhereRequired;
    BitVector hasNoState;
    // In the order in which the body comes to them.
    Vector<Vector<ModuleCallSite>> bareSites;
    // What stands in the place of each of them.
    Vector<Vector<ModuleCallees>> calleesAtBareSites;
    // For a module with a state: what evaluates all that it imports.
    Vector<ModuleCallees> steps;
};

} // anonymous namespace

static bool hasCode(const Program& program, const Module& module)
{
    return module.body && program.at(module.body).hasBody();
}

static bool onlyReturns(const Function& body)
{
    if (body.blocks.size() != 1)
        return false;
    const Block& block = body.blocks.first();
    return block.terminator.kind == TerminatorKind::Return && std::ranges::all_of(block.values, [&](ValueID id) {
        return body.at(id).opcode == Opcode::Undefined;
    });
}

// Whoever evaluates a module has nothing to pass but `this`, which is undefined in an ES module. The body of a CommonJS module takes nothing.
static bool canBeCalledByCode(const Function& body)
{
    if (body.role != FunctionRole::Source || !body.isModuleBody() || body.isUnsplitCoroutine() || body.results.size() != 1)
        return false;
    // The function that takes the place of the body returns undefined, whether or not it runs the code.
    bool returnsOnlyUndefined = std::ranges::all_of(body.blocks, [&](const Block& block) {
        switch (block.terminator.kind) {
        case TerminatorKind::Return: {
            ValueID returned = block.terminator.valueOperand();
            return !returned || body.at(returned).type == TypeTable::undefined;
        }
        case TerminatorKind::ReturnCall:
            return false;
        case TerminatorKind::Jump:
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            return true;
        }
    });
    if (!returnsOnlyUndefined)
        return false;
    for (unsigned index = 0; index < body.numberOfEntryParameters(); ++index) {
        if (body.entryParameterKind(index) != EntryParameter::This)
            return false;
    }
    return true;
}

// A module of any other kind ran when its importer was linked.
static bool isEvaluatedWithImporter(const Program& program, const Module& module)
{
    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        return true;
    case ModuleKind::CommonJs:
        return !hasCode(program, module) || canBeCalledByCode(program.at(module.body));
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The run time's own algorithm waits for promises, asks the host for code that was not compiled, and finds out what a deferred import has to evaluate.
static bool needsLoaderItself(const Program& program, const Module& module)
{
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
    if (module.flags.contains(ModuleFlags::HasTopLevelAwait))
        return true;
    if (module.flags.contains(ModuleFlags::IsLoadedByLoader))
        return true;
    if (std::ranges::any_of(module.requests, [](const ModuleRequest& request) { return request.phase == ImportPhase::Defer; }))
        return true;
    return hasCode(program, module) && !canBeCalledByCode(program.at(module.body));
}

// What the loader does for a module before it evaluates it: ownPropertiesOf() in AOTModuleGraph.cpp finds the same in the tables.
static bool hasLinkWorkItself(const Program& program, const Module& module)
{
    if (!module.syntheticExports.isEmpty() || !module.requiredNames.isEmpty())
        return true;
    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
        return module.exportRule != SyntheticExportRule::None;
    case ModuleKind::CommonJs:
        return !isEvaluatedWithImporter(program, module);
    case ModuleKind::External:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool runsCode(const Program& program, const Module& module)
{
    return isEvaluatedWithImporter(program, module) && hasCode(program, module) && !onlyReturns(program.at(module.body));
}

// The body of a face evaluates its file, whether the run time lists the names of the file behind that or the body copies them itself.
static bool isFaceOfCommonJSFile(const Program& program, const Module& module)
{
    if (module.kind != ModuleKind::SourceText || module.requests.isEmpty() || module.requests[0].kind != ModuleRequestKind::Require)
        return false;
    const Module& file = program.at(module.requests[0].target);
    return file.kind == ModuleKind::CommonJs && file.displayPath == module.displayPath;
}

// The modules that have the property themselves or import, directly or not, a module that has it.
static BitVector findModulesThatReach(const EvaluationPlan& plan, const StronglyConnectedComponents& components, const Invocable<bool(const Program&, const Module&)> auto& hasProperty)
{
    const Vector<uint32_t>& componentOf = components.componentOf;

    // A component has a higher number than every component it imports.
    BitVector reachingComponents(components.numberOfComponents);
    for (uint32_t index : components.members) {
        bool reaches = hasProperty(plan.program, plan.program.modules[index]) || std::ranges::any_of(plan.imports[index], [&](uint32_t target) {
            return reachingComponents.quickGet(componentOf[target]);
        });
        if (reaches)
            reachingComponents.quickSet(componentOf[index]);
    }

    BitVector reachingModules(plan.imports.size());
    for (size_t index = 0; index < plan.imports.size(); ++index) {
        if (reachingComponents.quickGet(componentOf[index]))
            reachingModules.quickSet(index);
    }
    return reachingModules;
}

// The CommonJS files, and their faces.
static BitVector findFilesThatRequireModuleThatAwaits(const EvaluationPlan& plan, const StronglyConnectedComponents& components, const ParallelLoops& loops)
{
    const Program& program = plan.program;
    BitVector files(program.modules.size());
    bool someModuleAwaits = std::ranges::any_of(program.modules, [](const Module& module) {
        return module.flags.contains(ModuleFlags::HasTopLevelAwait);
    });
    if (!someModuleAwaits)
        return files;
    BitVector awaits = findModulesThatReach(plan, components, [](const Program&, const Module& module) {
        return module.flags.contains(ModuleFlags::HasTopLevelAwait);
    });
    Vector<uint32_t> referrers = collectInParallel<uint32_t>(loops, program.functions.size(), [&](unsigned index, Vector<uint32_t>& found) {
        if (!mayListValueWith(program, FunctionID(index), OpcodeSet { Opcode::RequireModule }))
            return;
        for (const Value& value : program.functions[index].values) {
            if (value.opcode == Opcode::RequireModule && awaits.quickGet(value.module().index()) && value.referrer())
                found.append(value.referrer().index());
        }
    });
    for (uint32_t referrer : referrers)
        files.quickSet(referrer);
    for (size_t index = 0; index < program.modules.size(); ++index) {
        if (isFaceOfCommonJSFile(program, program.modules[index]) && files.quickGet(program.modules[index].requests[0].target.index()))
            files.quickSet(index);
    }
    return files;
}

// `bun run` loads the graph of a required ES module in steps. A program can tell only where another graph is being loaded at that time, or waits.
static bool mayLoadTwoGraphsAtOnce(const Program& program, const ParallelLoops& loops)
{
    bool someModuleWaits = std::ranges::any_of(program.modules, [](const Module& module) {
        return module.flags.containsAny({ ModuleFlags::HasTopLevelAwait, ModuleFlags::IsLoadedByLoader }) || (module.kind == ModuleKind::External && module.externalKind == ExternalKind::UncompiledCode);
    });
    if (someModuleWaits)
        return true;
    return !findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return anyValueListedOrNot(program, FunctionID(index), OpcodeSet { Opcode::ImportModule, Opcode::ImportDynamic }, [](ValueID, const Value&) {
            return true;
        });
    }).isEmpty();
}

// Whoever fetches a namespace object evaluates nothing.
static std::optional<SiteKind> siteKindOf(const Value& value)
{
    switch (value.opcode) {
    case Opcode::EvaluateModule:
        return SiteKind::Late;
    case Opcode::RequireModule:
        return SiteKind::Required;
    case Opcode::ImportModule:
        return SiteKind::Named;
    case Opcode::ModuleNamespace:
        if (value.phase() != ImportPhase::Evaluation)
            return SiteKind::Named;
        return std::nullopt;
    case Opcode::RecordRequire:
        return SiteKind::Record;
    default:
        return std::nullopt;
    }
}

static void findSites(const EvaluationPlan& plan, const Vector<ModuleID>& moduleWithBody, FunctionID id, Vector<ModuleCallSite>& found)
{
    constexpr OpcodeSet opcodesOfSites { Opcode::EvaluateModule, Opcode::RequireModule, Opcode::ImportModule, Opcode::ModuleNamespace, Opcode::RecordRequire };
    if (!mayListValueWith(plan.program, id, opcodesOfSites))
        return;
    const Function& function = plan.program.at(id);
    bool hasSite = std::ranges::any_of(function.values, [](const Value& value) {
        return siteKindOf(value).has_value();
    });
    if (!hasSite)
        return;

    ModuleID evaluated = moduleWithBody[id.index()];
    Vector<std::pair<BlockID, ModuleCallSite>, 8> flagged;
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const Block& block = function.blocks[blockIndex];
        for (ValueID valueID : block.values) {
            const Value& value = function.at(valueID);
            std::optional<SiteKind> kind = siteKindOf(value);
            if (!kind)
                continue;
            ModuleCallSite site { id, valueID, value.module(), *kind };
            bool isFlagged = value.opcode == Opcode::EvaluateModule && value.isBare() && evaluated && value.referrer() == evaluated && !block.handler;
            if (isFlagged && plan.isCalledWhereRequired.quickGet(site.target.index()))
                flagged.append({ BlockID(blockIndex), site });
            else
                found.append(site);
        }
    }

    // The front end says which sites are bare, and the passes between it and this one move code.
    if (flagged.isEmpty())
        return;
    if (function.blocks.size() == 1) {
        for (auto& [block, site] : flagged) {
            site.kind = SiteKind::Bare;
            found.append(site);
        }
        return;
    }

    CFG cfg(function);
    Dominators dominators(cfg, false);
    std::optional<BlockCycles> cycles;
    if (cfg.hasCycle())
        cycles.emplace(cfg);
    Vector<BlockID, 4> ends;
    for (BlockID block : cfg.reversePostOrder()) {
        if (returnsToCaller(function.at(block).terminator.kind))
            ends.append(block);
    }
    AOT::stableSort(flagged, { }, [&](const std::pair<BlockID, ModuleCallSite>& entry) {
        return cfg.isReachable(entry.first) ? cfg.reversePostOrderIndex(entry.first) : UINT32_MAX;
    });
    for (auto& [block, site] : flagged) {
        bool isPassedOnceByEveryRun = !ends.isEmpty() && cfg.isReachable(block) && !(cycles && cycles->isInCycle(block)) && std::ranges::all_of(ends, [&](BlockID end) {
            return dominators.dominates(block, end);
        });
        if (isPassedOnceByEveryRun)
            site.kind = SiteKind::Bare;
        found.append(site);
    }
}

// Something starts the evaluation of a module: the host, for an entry point; an operation that names the module and is no bare site; the loader, on its way through a module that it evaluates itself.
static BitVector findModulesWithoutState(const EvaluationPlan& plan, BitVector& isStart)
{
    size_t numberOfModules = plan.program.modules.size();
    uint32_t root = numberOfModules;

    Vector<ModuleList> successors(numberOfModules + 1);
    for (size_t index : plan.isWalkedByCode) {
        for (uint32_t target : plan.imports[index]) {
            if (plan.isWalkedByCode.quickGet(target))
                successors[index].append(target);
        }
        for (const ModuleCallSite& site : plan.bareSites[index])
            successors[index].append(site.target.index());
    }

    BitVector hasNoState(numberOfModules);
    while (true) {
        successors[root].shrink(0);
        for (size_t index : isStart) {
            if (plan.isWalkedByCode.quickGet(index))
                successors[root].append(index);
        }
        Vector<uint32_t> dominatorOf = computeImmediateDominators(successors, root);

        bool didFindStart = false;
        hasNoState.clearAll();
        for (size_t index : plan.isWalkedByCode) {
            // Nothing that is seen here comes to it. Whatever does finds a module with a state.
            if (dominatorOf[index] == UINT32_MAX) {
                isStart.quickSet(index);
                didFindStart = true;
                continue;
            }
            if (dominatorOf[index] != root) {
                hasNoState.quickSet(index);
                continue;
            }
            // The loader goes through a module that has nothing to run, and calls what that module imports.
            if (plan.startsItself.quickGet(index))
                continue;
            for (uint32_t successor : successors[index])
                didFindStart |= !isStart.quickSet(successor);
        }
        if (!didFindStart)
            return hasNoState;
    }
}

static void walkFrom(EvaluationPlan& plan, ModuleID first)
{
    struct Frame {
        uint32_t module;
        uint32_t nextEdge;
        // The steps of the first module, or what stands at the bare site that led here.
        ModuleCallees* callees;
    };
    const Program& program = plan.program;

    UncheckedKeyHashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> visited;
    visited.add(first.index());
    Vector<Frame, 32> stack { Frame { first.index(), 0, &plan.steps[first.index()] } };
    while (!stack.isEmpty()) {
        Frame& frame = stack.last();
        uint32_t index = frame.module;
        const ModuleList& imports = plan.imports[index];
        const Vector<ModuleCallSite>& bareSites = plan.bareSites[index];
        uint32_t edge = frame.nextEdge++;
        if (edge == imports.size() + bareSites.size()) {
            if (index != first.index() && plan.hasNoState.quickGet(index) && plan.startsItself.quickGet(index)) {
                FunctionID body = program.modules[index].body;
                if (!onlyReturns(program.at(body)))
                    frame.callees->append(body);
            }
            stack.removeLast();
            continue;
        }

        ModuleCallees* callees = frame.callees;
        uint32_t target;
        if (edge < imports.size())
            target = imports[edge];
        else {
            callees = &plan.calleesAtBareSites[index][edge - imports.size()];
            target = bareSites[edge - imports.size()].target.index();
        }
        if (!visited.add(target).isNewEntry)
            continue;
        // A module that has nothing to run passes the walk on.
        if (plan.hasNoState.quickGet(target) || (plan.isWalkedByCode.quickGet(target) && !plan.startsItself.quickGet(target)))
            stack.append(Frame { target, 0, callees });
        else if (plan.startsItself.quickGet(target))
            callees->append(program.modules[target].body);
    }
}

static Value makeCall(FunctionID callee, std::span<const ValueID> arguments)
{
    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(callee);
    call.setArgumentsOperands(arguments);
    return call;
}

static void addCallOfModule(const Program& program, FunctionID callee, BuiltinID calledBuiltin, NOESCAPE const Invocable<ValueID(Value&&)> auto& add)
{
    Vector<ValueID, 1> arguments;
    unsigned numberOfArguments = program.at(callee).numberOfEntryParameters();
    for (unsigned index = 0; index < numberOfArguments; ++index)
        arguments.append(add(IRBuilder::make(Opcode::Undefined)));
    Value call = makeCall(callee, arguments.span());
    call.setCalledBuiltin(calledBuiltin);
    add(WTF::move(call));
}

static Value makeModuleOperation(Opcode opcode, ModuleID module)
{
    Value value = IRBuilder::make(opcode);
    value.setModule(module);
    return value;
}

// To evaluate a module with a state is to call this function: it evaluates what the module imports, in the order of the requests, and then runs the code of the module.
static void buildStart(const AnalysisContext& context, Function& start, const ModuleToStart& module, const ModuleCallees& steps, TypeArena& types)
{
    const Function& code = context.program.at(module.code);
    IRBuilder builder(context, start, types);
    builder.setOrigin({ start.position, 0 });
    BlockID entry = builder.addBlock();
    Vector<ValueID, 1> arguments;
    for (ValueID parameter : code.at(entryBlock).parameters) {
        ValueID copy = builder.addParameter(entry, code.at(parameter).type);
        start.edit(copy, ValueEdit::Type).setExpected(code.at(parameter).expected());
        arguments.append(copy);
    }
    builder.moveToEndOf(entry);
    auto appendCallsOf = [&](std::span<const FunctionID> callees) {
        for (FunctionID callee : callees) {
            addCallOfModule(context.program, callee, { }, [&](Value&& value) {
                return builder.append(WTF::move(value));
            });
        }
    };

    if (module.filesRunWhileLoaded.isEmpty())
        appendCallsOf(steps.span().first(module.numberOfStepsBeforeItBegins));
    else {
        // Jobs that a file has queued run before the loader comes to the next file.
        BlockID stopped = builder.addBlock();
        for (unsigned index = 0; index < module.numberOfStepsBeforeItBegins; ++index) {
            appendCallsOf(steps.span().subspan(index, 1));
            ValueID stops = builder.append(makeModuleOperation(Opcode::StopLoadingBehindFile, module.filesRunWhileLoaded[index]));
            BlockID next = builder.addBlock();
            builder.emitBranch(stops, Target { stopped, { }, { } }, Target { next, { }, { } });
            builder.moveToEndOf(next);
        }
        BlockID current = builder.currentBlock();
        builder.moveToEndOf(stopped);
        builder.emitReturn(builder.appendUndefined());
        builder.moveToEndOf(current);
    }
    ValueID begins = module.beginsInCode ? ValueID() : builder.append(makeModuleOperation(Opcode::BeginModuleEvaluation, module.module));
    // Whoever has required the module may catch what its evaluation throws and go on.
    BlockID threw;
    if (module.mayBeLeftByCaughtError) {
        threw = builder.addHandlerBlock(BlockRole::CleanupHandler, ValueID());
        builder.setHandler(Target { threw, { }, { } });
    }
    BlockID run = builder.addBlock();
    builder.setHandler(std::nullopt);
    BlockID done = begins ? builder.addBlock() : BlockID();
    if (begins)
        builder.emitBranch(begins, Target { run, { }, { } }, Target { done, { }, { } });
    else
        builder.emitJump(run);
    if (threw) {
        builder.moveToEndOf(threw);
        ValueID exception = builder.append(IRBuilder::make(Opcode::CatchException));
        Value abandon = makeModuleOperation(Opcode::AbandonModuleEvaluation, module.module);
        abandon.setExceptionOperand(exception);
        builder.append(WTF::move(abandon));
        builder.emitRethrow(exception);
    }

    builder.moveToEndOf(run);
    appendCallsOf(steps.span().subspan(module.numberOfStepsBeforeItBegins));
    if (!onlyReturns(code))
        builder.append(makeCall(module.code, arguments.span()));
    if (begins)
        builder.append(makeModuleOperation(Opcode::FinishModuleEvaluation, module.module));
    builder.emitReturn(builder.appendUndefined());
    if (!begins)
        return;

    builder.moveToEndOf(done);
    builder.emitReturn(builder.appendUndefined());
}

// A CommonJS module begins behind the stack check of the frame of its code.
static void beginEvaluationInCode(const AnalysisContext& context, Function& code, ModuleID module, TypeArena& types)
{
    IRBuilder builder(context, code, types);
    builder.setOrigin({ code.position, 0 });
    for (uint32_t index = 0; index < code.blocks.size(); ++index) {
        const Block& block = code.at(BlockID(index));
        if (block.terminator.kind == TerminatorKind::Return)
            builder.insertBefore({ BlockID(index), static_cast<uint32_t>(block.values.size()) }, makeModuleOperation(Opcode::FinishModuleEvaluation, module));
    }
    builder.applyInsertions();

    BlockID body = builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
    builder.removeHandler(entryBlock);
    code.edit(entryBlock, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
    builder.moveToEndOf(entryBlock);
    ValueID begins = builder.append(makeModuleOperation(Opcode::BeginModuleEvaluation, module));
    BlockID done = builder.addBlock();
    builder.emitBranch(begins, Target { body, { }, { } }, Target { done, { }, { } });
    builder.moveToEndOf(done);
    builder.emitReturn(builder.appendUndefined());
}

// Behind the evaluation, require() marks the namespace object of a module that has no export `__esModule`: everybody who holds that object reads `true` under the name from then on.
static bool isMarkOfRequireSpelled(const Program& program, const ParallelLoops& loops)
{
    PropertyKeyID marker = program.keyOf(BuiltinNames::__esModule);
    if (!marker || !program.types.isSpelledByOperations(marker))
        return false;
    return !findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        constexpr OpcodeSet opcodesThatSpellKeys {
            Opcode::PropGet, Opcode::PropSet, Opcode::PropDefine, Opcode::PropHas, Opcode::PropHasOwn, Opcode::PropDelete, Opcode::GetOwnProperty, Opcode::KeyConstant, Opcode::NarrowByField,
        };
        if (listsAllValues(program, FunctionID(index))) {
            bool isSpelled = false;
            forEachListedName(program, FunctionID(index), opcodesThatSpellKeys, [&](BlockID, ValueID, Opcode, NameKind kind, uint32_t name) {
                isSpelled |= kind == NameKind::Key && name == marker.index();
            });
            return isSpelled;
        }
        return anyValueListedOrNot(program, FunctionID(index), opcodesThatSpellKeys, [&](ValueID, const Value& value) {
            return value.key() == marker;
        });
    }).isEmpty();
}

// `children` and `parent` of a module object say which `require` came first and which has run at all.
static bool somebodyAsksWhoRequiredWhom(const Program& program, const FlowResults& flow)
{
    if (!namesWaysToModuleObjects(program))
        return false;
    if (!flow.hasResults() || !flow.unseenCode().empty())
        return true;
    // `if (!module.parent)` asks whether somebody has required the file. `children` is a list whoever has required what.
    return !flow.usesOf(Builtins::ModuleObjectParent).isEmpty() || !(flow.usesOf(Builtins::ModuleObjectChildren) - BuiltinUse::Tested).isEmpty();
}

static void replaceSites(const EvaluationPlan& plan, NOESCAPE const Invocable<bool(ModuleID)> auto& recordsNothingThatIsSeen, ModuleID evaluated, Function& function, IRBuilder& builder)
{
    const Program& program = plan.program;
    // By the number of the value: which of the bare sites of the module it is, counted from one.
    Vector<uint32_t> bareSiteOf(FillWith { }, evaluated ? function.values.size() : 0, 0u);
    if (evaluated) {
        uint32_t numberOfSites = 0;
        for (const ModuleCallSite& site : plan.bareSites[evaluated.index()])
            bareSiteOf[site.value.index()] = ++numberOfSites;
    }

    BitVector isErased(function.values.size());
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        for (uint32_t index = 0; index < function.blocks[blockIndex].values.size(); ++index) {
            ProgramPoint point { BlockID(blockIndex), index };
            ValueID id = function.blocks[blockIndex].values[index];
            const Value& value = function.at(id);
            bool isRecord = value.opcode == Opcode::RecordRequire;
            if (isRecord ? !recordsNothingThatIsSeen(value.module()) : value.opcode != Opcode::EvaluateModule)
                continue;
            isErased.quickSet(id.index());
            if (isRecord)
                continue;

            ModuleID target = value.module();
            BuiltinID calledBuiltin = value.isCallOfRequire() ? functionOfHostThatRequireCallsFor(program.at(target)) : BuiltinID();
            builder.setOrigin(value.origin());
            std::span<const FunctionID> callees;
            if (evaluated && bareSiteOf[id.index()])
                callees = plan.calleesAtBareSites[evaluated.index()][bareSiteOf[id.index()] - 1].span();
            else if (!plan.isCalledWhereRequired.quickGet(target.index())) {
                // The loader links what the module imports, waits or refuses to, or has the module itself.
                Value require = makeModuleOperation(Opcode::EvaluateRequiredModule, target);
                require.setReferrer(value.referrer());
                require.setSpecifier(program.at(target).path);
                require.setCalledBuiltin(calledBuiltin);
                builder.insertBefore(point, WTF::move(require));
            } else if (plan.startsItself.quickGet(target.index()))
                callees = singleElementSpan(program.at(target).body);
            else
                callees = plan.steps[target.index()].span();
            for (FunctionID callee : callees) {
                addCallOfModule(program, callee, calledBuiltin, [&](Value&& added) {
                    return builder.insertBefore(point, WTF::move(added));
                });
            }
        }
    }
    builder.applyInsertions();

    BitVector erasedIndexes;
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.blocks[blockIndex].values;
        erasedIndexes.clearAll();
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (isErased.get(values[index].index()))
                erasedIndexes.set(index);
        }
        if (!erasedIndexes.isEmpty())
            builder.eraseValues(BlockID(blockIndex), erasedIndexes);
    }
    // Whoever looks through all values of a function finds no operation there that names a module.
    for (size_t index : isErased) {
        Value& erased = function.edit(ValueID(index));
        erased.become(Opcode::Undefined);
        erased.type = TypeTable::undefined;
    }
}

void callModules(ProgramEditor& editor, const FlowResults& flow, BuildContext& build)
{
    const Program& program = editor.program();
    const Vector<Module>& modules = program.modules;
    const ParallelLoops& loops = build.parallelLoops();
    constexpr OptionSet<FunctionCapabilities> calledByCodeAlone { FunctionCapabilities::HasOnlyDirectCallers, FunctionCapabilities::IsNeverCalledByHost };

    EvaluationPlan plan { .program = program };
    plan.imports = Vector<ModuleList>(modules.size(), [&](size_t index) {
        ModuleList imports;
        for (const ModuleRequest& request : modules[index].requests) {
            if (request.kind == ModuleRequestKind::Import)
                imports.append(request.target.index());
        }
        return imports;
    });
    StronglyConnectedComponents components = computeStronglyConnectedComponents(modules.size(), [&](unsigned index) -> const ModuleList& {
        return plan.imports[index];
    });
    // A `require` of a module that needs the loader asks the loader where it stands. The module that holds it waits for nothing.
    BitVector needsLoader = findModulesThatReach(plan, components, needsLoaderItself);
    // A `require` of a module that awaits throws. What becomes of the importers of a file that throws is the loader's to say.
    if (BitVector requiresModuleThatAwaits = findFilesThatRequireModuleThatAwaits(plan, components, loops); !requiresModuleThatAwaits.isEmpty()) {
        needsLoader = findModulesThatReach(plan, components, [&](const Program& program, const Module& module) {
            return needsLoaderItself(program, module) || requiresModuleThatAwaits.quickGet(&module - program.modules.span().data());
        });
        for (size_t index = 0; index < modules.size(); ++index) {
            if (modules[index].kind == ModuleKind::CommonJs)
                needsLoader.quickClear(index);
        }
    }
    BitVector leadsToCode = findModulesThatReach(plan, components, runsCode);
    BitVector needsLink = findModulesThatReach(plan, components, hasLinkWorkItself);

    // A module that only passes the walk on is being evaluated while what it imports runs, as any other.
    plan.isWalkedByCode.ensureSize(modules.size());
    plan.startsItself.ensureSize(modules.size());
    plan.isCalledWhereRequired.ensureSize(modules.size());
    Vector<ModuleID> moduleWithBody(program.functions.size());
    bool loadsTwoGraphsAtOnce = mayLoadTwoGraphsAtOnce(program, loops);
    for (size_t index = 0; index < modules.size(); ++index) {
        const Module& module = modules[index];
        if (!isEvaluatedWithImporter(program, module) || needsLoader.quickGet(index))
            continue;
        plan.isWalkedByCode.quickSet(index);
        // Whoever enters a graph of modules in which something is to be linked goes through the loader: the host with an entry point, import(), require(). So does a require() of an ES module where another graph may be in the middle of its load: the run time knows whether one is.
        bool isRequiredBetweenLoads = module.kind != ModuleKind::SourceText || (!loadsTwoGraphsAtOnce && !module.flags.contains(ModuleFlags::IsRequiredDuringLoad));
        if (!needsLink.quickGet(index) && isRequiredBetweenLoads)
            plan.isCalledWhereRequired.quickSet(index);
        if (!hasCode(program, module))
            continue;
        moduleWithBody[module.body.index()] = ModuleID(static_cast<uint32_t>(index));
        if (leadsToCode.quickGet(index))
            plan.startsItself.quickSet(index);
    }

    Vector<ModuleCallSite> sites = collectInParallel<ModuleCallSite>(loops, program.functions.size(), [&](unsigned index, Vector<ModuleCallSite>& found) {
        findSites(plan, moduleWithBody, FunctionID(index), found);
    });

    plan.bareSites.grow(modules.size());
    BitVector isStart(modules.size());
    BitVector isRequiredLate(modules.size());
    BitVector isAskedOfLoader(modules.size());
    bool recordsRequires = false;
    unsigned numberOfSitesBound = 0;
    for (const ModuleCallSite& site : sites) {
        size_t target = site.target.index();
        switch (site.kind) {
        case SiteKind::Bare:
            plan.bareSites[moduleWithBody[site.function.index()].index()].append(site);
            ++numberOfSitesBound;
            continue;
        case SiteKind::Late:
            if (plan.isCalledWhereRequired.quickGet(target)) {
                isRequiredLate.quickSet(target);
                ++numberOfSitesBound;
            } else
                isAskedOfLoader.quickSet(target);
            break;
        case SiteKind::Required:
        case SiteKind::Named:
            isAskedOfLoader.quickSet(target);
            break;
        case SiteKind::Record:
            recordsRequires = true;
            continue;
        }
        isStart.quickSet(target);
    }
    // The host asks the loader for an entry point.
    BitVector isEntryPoint(modules.size());
    for (const Entry& entry : program.entries) {
        isEntryPoint.quickSet(entry.module.index());
        isAskedOfLoader.quickSet(entry.module.index());
    }
    // What a required module imports is evaluated with it.
    Vector<uint32_t, 16> reachedByRequire;
    for (size_t index : isAskedOfLoader)
        reachedByRequire.append(index);
    while (!reachedByRequire.isEmpty()) {
        for (uint32_t target : plan.imports[reachedByRequire.takeLast()]) {
            if (modules[target].kind == ModuleKind::SourceText && !isAskedOfLoader.quickSet(target))
                reachedByRequire.append(target);
        }
    }
    // `bun run` runs a CommonJS file that is imported while it links the importer, which has not begun by then: a `require` of the importer in that file evaluates the importer there and then, once.
    auto importsCommonJSBeforeItBegins = [&](size_t index) {
        return isAskedOfLoader.quickGet(index) && modules[index].kind == ModuleKind::SourceText;
    };
    for (const Entry& entry : program.entries)
        isStart.quickSet(entry.module.index());
    for (size_t index = 0; index < modules.size(); ++index) {
        if (importsCommonJSBeforeItBegins(index)) {
            for (uint32_t target : plan.imports[index]) {
                if (modules[target].kind == ModuleKind::CommonJs || isFaceOfCommonJSFile(program, modules[target]))
                    isStart.quickSet(target);
            }
        }
        // Behind the evaluation of a module that takes its names from an exports object the run time fills them in. It finds the place by the state.
        if ((modules[index].kind == ModuleKind::SourceText && modules[index].exportRule != SyntheticExportRule::None) || (modules[index].kind == ModuleKind::CommonJs && !modules[index].syntheticExports.isEmpty()))
            isStart.quickSet(index);
        if (plan.isWalkedByCode.quickGet(index))
            continue;
        for (const ModuleRequest& request : modules[index].requests)
            isStart.quickSet(request.target.index());
    }
    plan.hasNoState = findModulesWithoutState(plan, isStart);

    // What a program can tell of a `require` beyond the evaluation of the module.
    bool markIsSpelled = !recordsRequires || isMarkOfRequireSpelled(program, loops);
    bool isAskedWhoRequiredWhom = !recordsRequires || somebodyAsksWhoRequiredWhom(program, flow);
    // What is recorded is who required whom and, of an ES module, the mark, which a module that exports the name itself does not get.
    auto recordsNothingThatIsSeen = [&](ModuleID target) {
        if (isAskedWhoRequiredWhom)
            return false;
        const Module& module = program.at(target);
        if (module.kind != ModuleKind::SourceText || std::ranges::contains(module.exports, program.stringOf(BuiltinNames::__esModule), &ModuleExport::name))
            return true;
        return flow.hasResults() && !markIsSpelled && flow.module(target).namespaceUse != NamespaceUse::Unknown;
    };

    // What compiled code starts where a handler of the program may stand around it, and everything that is evaluated below that.
    BitVector mayBeLeftByCaughtError = isRequiredLate;
    Vector<uint32_t, 32> work;
    for (size_t index : mayBeLeftByCaughtError)
        work.append(index);
    auto addToWork = [&](ModuleID target) {
        if (!mayBeLeftByCaughtError.quickSet(target.index()))
            work.append(target.index());
    };
    while (!work.isEmpty()) {
        uint32_t index = work.takeLast();
        for (const ModuleRequest& request : modules[index].requests)
            addToWork(request.target);
        for (const ModuleCallSite& site : plan.bareSites[index])
            addToWork(site.target);
    }

    plan.steps.grow(modules.size());
    plan.calleesAtBareSites = Vector<Vector<ModuleCallees>>(modules.size(), [&](size_t index) {
        return Vector<ModuleCallees>(plan.bareSites[index].size());
    });
    Vector<uint32_t> modulesWithState;
    for (size_t index : plan.isWalkedByCode) {
        if (!plan.hasNoState.quickGet(index))
            modulesWithState.append(index);
    }
    // Each walk fills the lists of the modules without a state that it alone comes to.
    forEachIndexInParallel(loops, modulesWithState.size(), [&](unsigned index) {
        walkFrom(plan, ModuleID(modulesWithState[index]));
    });

    BitVector isCalled(program.functions.size());
    auto noteCallees = [&](const ModuleCallees& callees) {
        for (FunctionID callee : callees)
            isCalled.quickSet(callee.index());
    };
    for (size_t index = 0; index < modules.size(); ++index) {
        noteCallees(plan.steps[index]);
        for (const ModuleCallees& callees : plan.calleesAtBareSites[index])
            noteCallees(callees);
    }
    for (size_t index : isRequiredLate) {
        if (plan.startsItself.quickGet(index))
            isCalled.quickSet(modules[index].body.index());
    }

    // The loader calls a module that nothing of the image evaluates and that imports nothing, and there is no call to save.
    Vector<ModuleToStart> starts;
    for (uint32_t index : modulesWithState) {
        const Module& module = modules[index];
        const ModuleCallees& steps = plan.steps[index];
        if (!plan.startsItself.quickGet(index) || (steps.isEmpty() && !isCalled.quickGet(module.body.index())))
            continue;
        // An ES module that failed throws the same error at everybody who asks for it later. What a CommonJS module does is written in its record: a wrapper of a bundler counts as done once it has begun.
        bool keepsError = module.kind != ModuleKind::CommonJs || module.flags.contains(ModuleFlags::KeepsError);
        starts.append(ModuleToStart { ModuleID(index), module.body, { }, keepsError && mayBeLeftByCaughtError.quickGet(index) });
        starts.last().beginsInCode = module.kind == ModuleKind::CommonJs && steps.isEmpty() && !onlyReturns(program.at(module.body));
        if (!importsCommonJSBeforeItBegins(index))
            continue;
        auto firstOther = std::ranges::find_if(steps, [&](FunctionID callee) {
            size_t called = moduleWithBody[callee.index()].index();
            bool runsCommonJS = modules[called].kind == ModuleKind::CommonJs || isFaceOfCommonJSFile(program, modules[called]);
            return !runsCommonJS || plan.hasNoState.quickGet(called);
        });
        starts.last().numberOfStepsBeforeItBegins = firstOther - steps.begin();
        if (!isEntryPoint.quickGet(index))
            continue;
        for (FunctionID callee : steps.span().first(starts.last().numberOfStepsBeforeItBegins))
            starts.last().filesRunWhileLoaded.append(moduleWithBody[callee.index()]);
    }

    Vector<FunctionID> codeOfModule(modules.size());
    Vector<Function> codes(starts.size());
    forEachIndexInParallel(loops, starts.size(), [&](unsigned index) {
        codes[index] = program.at(starts[index].start);
        codes[index].capabilities.add(calledByCodeAlone);
    });
    // The bodies are built at the same time, and each reads the headers of those it calls: the headers are final before the first body is begun.
    for (size_t index = 0; index < starts.size(); ++index) {
        ModuleToStart& module = starts[index];
        module.code = editor.addFunctionLike(module.start, WTF::move(codes[index]));
        codeOfModule[module.module.index()] = module.code;
        editor.removeBody(module.start);
        Function& start = editor.at(module.start);
        start.capabilities.add(FunctionCapabilities::IsHiddenInStackTraces);
        start.capabilities.remove({ FunctionCapabilities::MayChangeSignature, FunctionCapabilities::MayBeCloned, FunctionCapabilities::MayBeInlined });
    }

    unsigned numberOfModulesWithoutState = 0;
    for (size_t index : plan.hasNoState) {
        if (!plan.startsItself.quickGet(index))
            continue;
        ++numberOfModulesWithoutState;
        editor.at(modules[index].body).capabilities.add(calledByCodeAlone);
        // For the loader it is a module that has nothing to run.
        editor.at(ModuleID(static_cast<uint32_t>(index))).body = FunctionID();
    }

    Vector<FunctionID> startIDs = starts.map([](const ModuleToStart& module) {
        return module.start;
    });
    editFunctionsInParallel(editor, build, startIDs.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        buildStart({ build, program, nullptr, nullptr, id }, function, starts[index], plan.steps[starts[index].module.index()], arenas.types);
    });
    for (const ModuleToStart& module : starts) {
        if (isStart.quickGet(module.module.index())) {
            editor.at(module.module).flags.add(ModuleFlags::EvaluatesItself);
            continue;
        }
        // Two walks meet at it, and that is all it has a state for. Nothing but the starts of other modules comes to it, and they call it: for the loader it is a module that has nothing to run.
        editor.at(module.start).capabilities.add(calledByCodeAlone);
        editor.at(module.module).body = FunctionID();
    }

    // The sites come function by function. The code of a module with a start has moved.
    Vector<FunctionID> functionsWithSites;
    Vector<ModuleID> modulesEvaluatedByThem;
    unsigned numberOfRequiresRecorded = 0;
    FunctionID last;
    for (const ModuleCallSite& site : sites) {
        bool isReplaced = false;
        switch (site.kind) {
        case SiteKind::Bare:
        case SiteKind::Late:
            isReplaced = true;
            break;
        case SiteKind::Required:
        case SiteKind::Named:
            break;
        case SiteKind::Record:
            isReplaced = recordsNothingThatIsSeen(site.target);
            numberOfRequiresRecorded += !isReplaced;
            break;
        }
        if (!isReplaced || site.function == last)
            continue;
        last = site.function;
        ModuleID evaluated = moduleWithBody[site.function.index()];
        FunctionID code = evaluated ? codeOfModule[evaluated.index()] : FunctionID();
        functionsWithSites.append(code ? code : site.function);
        modulesEvaluatedByThem.append(evaluated);
    }
    editFunctionsInParallel(editor, build, functionsWithSites.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        AnalysisContext context { build, program, nullptr, nullptr, id };
        IRBuilder builder(context, function, arenas.types);
        replaceSites(plan, recordsNothingThatIsSeen, modulesEvaluatedByThem[index], function, builder);
    });
    Vector<FunctionID> codesThatBegin;
    Vector<ModuleID> modulesThatBeginInCode;
    for (const ModuleToStart& module : starts) {
        if (!module.beginsInCode)
            continue;
        codesThatBegin.append(module.code);
        modulesThatBeginInCode.append(module.module);
    }
    editFunctionsInParallel(editor, build, codesThatBegin.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        beginEvaluationInCode({ build, program, nullptr, nullptr, id }, function, modulesThatBeginInCode[index], arenas.types);
    });

    build.statistics().count(StatisticID::ModulesWithState, starts.size());
    build.statistics().count(StatisticID::ModulesWithoutState, numberOfModulesWithoutState);
    build.statistics().count(StatisticID::RequireSitesBound, numberOfSitesBound);
    build.statistics().count(StatisticID::RequiresRecorded, numberOfRequiresRecorded);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
