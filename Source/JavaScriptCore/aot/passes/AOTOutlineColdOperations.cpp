/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOutlineColdOperations.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTOutliningCandidates.h"
#include "AOTPass.h"
#include "AOTPhaseTimes.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t outlinedBodyOverheadBytes = 48;
static constexpr size_t sitesInLongHashRun = 1024;

namespace {

struct SiteGroup {
    Vector<uint32_t, 2> sites;
    FunctionID body;
};

struct WorkLists {
    std::span<const uint32_t> sitesOf(unsigned index) const
    {
        size_t begin = firstSiteOf[index];
        size_t end = index + 1 < firstSiteOf.size() ? firstSiteOf[index + 1] : sites.size();
        return sites.subspan(begin, end - begin);
    }

    Vector<uint32_t> sites;
    Vector<uint32_t> firstSiteOf;
    Vector<FunctionID> functions;
};

struct ErasedValues {
    BlockID block;
    BitVector indexes;
};

struct RewriteInputs {
    AnalysisContext contextOf(FunctionID id) const { return { build, editor.program(), &optimization.flow, &optimization.effects, id }; }

    // Says whether the function was read: one that holds an id which the loop replaces at its end is not.
    bool readIfFinal(FunctionID id, const TaskArenas& arenas) const
    {
        if (!readers || arenas.types.hasUsedProvisionalRecords() || arenas.strings.hasUsedProvisionalStrings())
            return false;
        readers->read(id);
        return true;
    }

    ProgramEditor& editor;
    const OptimizationInputs& optimization;
    BuildContext& build;
    const ParallelArray<OutliningSite>& sites;
    const Vector<FunctionID>& bodyOfSite;
    const ReadersOfOutlinedFunctions* readers;
    // By function.
    Vector<uint8_t>& wasRead;
};

class BodyBuilder {
public:
    BodyBuilder(const AnalysisContext&, const Function& source, const OutliningSite&, Function& body, TypeArena&);

    void build();

private:
    BlockID addBlockLike(BlockID original);
    void addEntryBlock();
    void addInteriorBlocks();
    void addReturnBlock();
    void copyOutsideConstants();
    void copyValues(std::span<const ValueID>);
    void copyValue(ValueID);
    void copyTerminator(const Terminator&);

    bool hasCopy(ValueID) const;
    ValueID copyOf(ValueID) const;
    BlockID copyOf(BlockID) const;

    const Function& m_source;
    const OutliningSite& m_site;
    IRBuilder m_builder;
    BlockID m_entryBlock;
    BlockID m_returnBlock;
    Vector<std::pair<ValueID, ValueID>, 32> m_valueCopies;
    Vector<std::pair<BlockID, BlockID>, 4> m_blockCopies;
};

} // anonymous namespace

static bool isDefinedInSite(const Function& function, const OutliningSite& site, ValueID id)
{
    if (std::ranges::contains(entryValuesOf(function, site), id))
        return true;
    return std::ranges::any_of(site.interiorBlocks, [&](BlockID block) {
        return function.at(block).values.contains(id);
    });
}

static void convertToCall(Value& value, const OutliningSite& site, FunctionID callee)
{
    value.become(Opcode::CallDirect);
    value.type = site.resultType;
    value.setExpected({ });
    value.setFunction(callee);
    value.setArgumentsOperands(site.liveIns.span());
}

static Value createCall(const OutliningSite& site, FunctionID callee)
{
    Value call = IRBuilder::make(Opcode::CallDirect);
    convertToCall(call, site, callee);
    return call;
}

uint32_t estimatedCallBytes(const OutliningSite& site)
{
    return estimatedDirectCallBytes(site.liveIns.size());
}

static OutliningDecision decide(const OutliningSite& firstSite, uint64_t numberOfSites)
{
    uint64_t inlineBytes = firstSite.codeBytes;
    uint64_t callBytes = firstSite.callBytes;
    if (numberOfSites * inlineBytes <= numberOfSites * callBytes + inlineBytes + outlinedBodyOverheadBytes)
        return OutliningDecision::NotProfitable;
    return OutliningDecision::Outlined;
}

static uint64_t createSortKey(unsigned hash, uint32_t site)
{
    return static_cast<uint64_t>(hash) << 32 | site;
}

static unsigned hashInSortKey(uint64_t key)
{
    return static_cast<unsigned>(key >> 32);
}

static uint32_t siteInSortKey(uint64_t key)
{
    return static_cast<uint32_t>(key);
}

// The savings of a group grow with its size, and no group is larger than its hash run.
static bool canAnyGroupBeProfitable(const ParallelArray<OutliningSite>& sites, std::span<const uint64_t> keysWithEqualHash)
{
    if (keysWithEqualHash.size() < 2)
        return false;
    return std::ranges::any_of(keysWithEqualHash, [&](uint64_t key) {
        return decide(sites[siteInSortKey(key)], keysWithEqualHash.size()) == OutliningDecision::Outlined;
    });
}

static void groupEqualSites(const Program& program, const ParallelArray<OutliningSite>& sites, std::span<const uint64_t> keysWithEqualHash, Vector<SiteGroup>& groups)
{
    size_t firstGroupWithHash = groups.size();
    for (uint64_t key : keysWithEqualHash) {
        uint32_t site = siteInSortKey(key);
        std::span<SiteGroup> candidates = groups.mutableSubspan(firstGroupWithHash);
        auto equalGroup = std::ranges::find_if(candidates, [&](const SiteGroup& group) {
            return areEqualSites(program, sites[group.sites.first()], sites[site]);
        });
        if (equalGroup != candidates.end())
            equalGroup->sites.append(site);
        else
            groups.append(SiteGroup { { site }, { } });
    }
}

static Vector<SiteGroup> findProfitableGroups(const Program& program, const ParallelLoops& loops, const ParallelArray<OutliningSite>& sites)
{
    Vector<uint64_t> sortKeys(sites.size());
    forEachIndexInParallel(loops, sites.size(), [&](unsigned site) {
        sortKeys[site] = createSortKey(sites[site].hash, site);
    });
    parallelSort(loops, sortKeys.mutableSpan(), std::less<uint64_t> { });
    Vector<uint32_t> hashRunStarts = findIndicesInParallel(loops, sortKeys.size(), [&](unsigned position) {
        return !position || hashInSortKey(sortKeys[position - 1]) != hashInSortKey(sortKeys[position]);
    });

    auto keysOfRun = [&](unsigned run) {
        size_t begin = hashRunStarts[run];
        size_t end = run + 1 < hashRunStarts.size() ? hashRunStarts[run + 1] : sortKeys.size();
        return sortKeys.subspan(begin, end - begin);
    };
    Vector<uint32_t> longestRunsFirst = findIndicesInParallel(loops, hashRunStarts.size(), [&](unsigned run) {
        return keysOfRun(run).size() >= sitesInLongHashRun;
    });
    std::ranges::sort(longestRunsFirst, [&](uint32_t a, uint32_t b) {
        return std::pair { keysOfRun(b).size(), a } < std::pair { keysOfRun(a).size(), b };
    });

    Vector<SiteGroup> groups = collectInParallel<SiteGroup>(loops, hashRunStarts.size(), longestRunsFirst.span(), [&](unsigned run, Vector<SiteGroup>& found) {
        if (!canAnyGroupBeProfitable(sites, keysOfRun(run)))
            return;
        size_t firstGroupWithHash = found.size();
        groupEqualSites(program, sites, keysOfRun(run), found);
        found.removeAllMatching([&](const SiteGroup& group) {
            return decide(sites[group.sites.first()], group.sites.size()) != OutliningDecision::Outlined;
        }, firstGroupWithHash);
    });

    std::ranges::sort(groups.mutableSpan(), { }, [](const SiteGroup& group) {
        return group.sites.first();
    });
    return groups;
}

static Function createBodyHead(const Program& program, const OutliningSite& site)
{
    const Function& function = program.at(site.function);
    Function created;
    created.name = program.strings.emptyString();
    created.module = function.module;
    created.position = function.position;
    created.kind = FunctionKind::Plain;
    created.role = FunctionRole::OutlinedBody;
    created.length = static_cast<uint32_t>(site.liveIns.size());
    created.thisType = DeclaredType { .type = TypeTable::anything };
    for (ValueID liveIn : site.liveIns)
        created.parameters.append(DeclaredType { .type = function.at(liveIn).type });
    created.results.append(DeclaredType { .type = site.resultType });
    created.capabilities = initialCapabilitiesOf(created);
    return created;
}

static Vector<FunctionID> addBodyHeads(ProgramEditor& editor, const ParallelLoops& loops, const ParallelArray<OutliningSite>& sites, Vector<SiteGroup>& groups)
{
    Vector<FunctionID> bodyOfSite(FillWith { }, sites.size(), FunctionID());
    FunctionID firstBody = editor.reserveFunctions(groups.size());
    forEachIndexInParallel(loops, groups.size(), [&](unsigned index) {
        SiteGroup& group = groups[index];
        group.body = FunctionID(firstBody.index() + index);
        editor.replaceFunction(group.body, createBodyHead(editor.program(), sites[group.sites.first()]));
        for (uint32_t site : group.sites)
            bodyOfSite[site] = group.body;
    });
    return bodyOfSite;
}

// The sites come in the order of their functions.
static WorkLists createWorkLists(const ParallelLoops& loops, const ParallelArray<OutliningSite>& sites, const Vector<FunctionID>& bodyOfSite)
{
    WorkLists work;
    work.sites = findIndicesInParallel(loops, sites.size(), [&](unsigned site) {
        return !!bodyOfSite[site];
    });
    work.firstSiteOf = findIndicesInParallel(loops, work.sites.size(), [&](unsigned position) {
        return !position || sites[work.sites[position - 1]].function != sites[work.sites[position]].function;
    });
    work.functions = work.firstSiteOf.map([&](uint32_t position) {
        return sites[work.sites[position]].function;
    });
    return work;
}

BodyBuilder::BodyBuilder(const AnalysisContext& context, const Function& source, const OutliningSite& site, Function& body, TypeArena& types)
    : m_source(source)
    , m_site(site)
    , m_builder(context, body, types)
{
    RELEASE_ASSERT(!body.hasBody());
    m_builder.setOrigin({ source.position, 0 });
}

void BodyBuilder::build()
{
    addEntryBlock();
    if (m_site.isRegion()) {
        addInteriorBlocks();
        addReturnBlock();
    }

    m_builder.moveToEndOf(m_entryBlock);
    copyOutsideConstants();
    copyValues(entryValuesOf(m_source, m_site));
    if (!m_site.isRegion()) {
        m_builder.emitReturn(m_site.rootIsUsed ? std::optional(copyOf(m_site.root)) : std::nullopt);
        return;
    }

    copyTerminator(m_source.at(m_site.entryBlock).terminator);
    for (BlockID block : m_site.interiorBlocks) {
        m_builder.moveToEndOf(copyOf(block));
        copyValues(m_source.at(block).values.span());
        copyTerminator(m_source.at(block).terminator);
    }
}

BlockID BodyBuilder::addBlockLike(BlockID original)
{
    const Block& block = m_source.at(original);
    BlockID added = m_builder.addBlock(block.frequency);
    m_builder.function().edit(added, BlockEdit::Rest).shape = block.shape;
    return added;
}

void BodyBuilder::addEntryBlock()
{
    m_entryBlock = addBlockLike(m_site.entryBlock);
    for (ValueID liveIn : m_site.liveIns)
        m_valueCopies.append({ liveIn, m_builder.addParameter(m_entryBlock, m_source.at(liveIn).type) });
}

void BodyBuilder::addInteriorBlocks()
{
    for (BlockID block : m_site.interiorBlocks) {
        BlockID copy = addBlockLike(block);
        m_blockCopies.append({ block, copy });
        for (ValueID parameter : m_source.at(block).parameters)
            m_valueCopies.append({ parameter, m_builder.addParameter(copy, m_source.at(parameter).type) });
    }
}

void BodyBuilder::addReturnBlock()
{
    m_returnBlock = addBlockLike(m_site.entryBlock);
    std::optional<ValueID> result;
    if (m_site.exitArgument)
        result = m_builder.addParameter(m_returnBlock, m_site.resultType);
    m_builder.moveToEndOf(m_returnBlock);
    m_builder.emitReturn(result);
}

void BodyBuilder::copyOutsideConstants()
{
    auto visit = [&](ValueID id) {
        if (!hasCopy(id) && !isDefinedInSite(m_source, m_site, id) && isRematerializable(m_source.at(id)))
            copyValue(id);
    };
    auto visitValues = [&](std::span<const ValueID> values) {
        for (ValueID id : values) {
            m_source.at(id).forEachOperand([&](ValueID operand, uint32_t) {
                visit(operand);
            });
        }
    };
    auto visitTerminator = [&](const Terminator& terminator) {
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            visit(operand);
        });
        for (const Target& target : terminator.targets) {
            if (target.block == m_site.exitBlock) {
                if (m_site.exitArgument)
                    visit(target.arguments[*m_site.exitArgument]);
                continue;
            }
            for (ValueID argument : target.arguments)
                visit(argument);
        }
    };

    visitValues(entryValuesOf(m_source, m_site));
    if (!m_site.isRegion())
        return;
    visitTerminator(m_source.at(m_site.entryBlock).terminator);
    for (BlockID block : m_site.interiorBlocks) {
        visitValues(m_source.at(block).values.span());
        visitTerminator(m_source.at(block).terminator);
    }
}

void BodyBuilder::copyValues(std::span<const ValueID> values)
{
    for (ValueID id : values)
        copyValue(id);
}

void BodyBuilder::copyValue(ValueID id)
{
    const Value& original = m_source.at(id);
    Value copy(original);
    copy.forEachOperand([&](ValueID& operand, uint32_t) {
        operand = copyOf(operand);
    });
    ValueID copyID = m_builder.append(WTF::move(copy));
    ASSERT(m_builder.function().at(copyID).type == original.type);
    m_valueCopies.append({ id, copyID });
}

void BodyBuilder::copyTerminator(const Terminator& terminator)
{
    Terminator copy(terminator);
    copy.forEachOperand([&](ValueID& operand, uint32_t) {
        operand = copyOf(operand);
    });
    for (Target& target : copy.targets) {
        if (target.block == m_site.exitBlock) {
            Arguments result;
            if (m_site.exitArgument)
                result.append(copyOf(target.arguments[*m_site.exitArgument]));
            target = Target { m_returnBlock, WTF::move(result), { } };
            continue;
        }
        target.block = copyOf(target.block);
        for (ValueID& argument : target.arguments)
            argument = copyOf(argument);
    }
    m_builder.emitTerminator(WTF::move(copy));
}

bool BodyBuilder::hasCopy(ValueID id) const
{
    return m_valueCopies.containsIf([&](const auto& entry) {
        return entry.first == id;
    });
}

ValueID BodyBuilder::copyOf(ValueID id) const
{
    size_t index = m_valueCopies.findIf([&](const auto& entry) {
        return entry.first == id;
    });
    RELEASE_ASSERT(index != notFound);
    return m_valueCopies[index].second;
}

BlockID BodyBuilder::copyOf(BlockID block) const
{
    size_t index = m_blockCopies.findIf([&](const auto& entry) {
        return entry.first == block;
    });
    RELEASE_ASSERT(index != notFound);
    return m_blockCopies[index].second;
}

static void replaceRun(Function& function, const OutliningSite& site, FunctionID body, Vector<ErasedValues, 4>& erasedValues)
{
    convertToCall(function.edit(site.root), site, body);
    if (site.numberOfEntryValues == 1)
        return;

    if (erasedValues.isEmpty() || erasedValues.last().block != site.entryBlock)
        erasedValues.append(ErasedValues { site.entryBlock, { } });
    uint32_t rootIndex = site.firstIndex + site.numberOfEntryValues - 1;
    for (uint32_t index = site.firstIndex; index < rootIndex; ++index)
        erasedValues.last().indexes.set(index);
}

static const Target* findTarget(const Terminator& terminator, BlockID block)
{
    for (const Target& target : terminator.targets) {
        if (target.block == block)
            return &target;
    }
    return nullptr;
}

static Arguments copyFirstExitArguments(const Function& function, const OutliningSite& site)
{
    if (const Target* exit = findTarget(function.at(site.entryBlock).terminator, site.exitBlock))
        return exit->arguments;
    for (BlockID block : site.interiorBlocks) {
        if (const Target* exit = findTarget(function.at(block).terminator, site.exitBlock))
            return exit->arguments;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isCoveredByOneHandler(const Function& function, const OutliningSite& region)
{
    const std::optional<Target>& handler = function.at(region.entryBlock).handler;
    return std::ranges::all_of(region.interiorBlocks, [&](BlockID block) {
        return function.at(block).handler == handler;
    });
}

static void replaceRegion(IRBuilder& builder, const OutliningSite& site, FunctionID body)
{
    Function& function = builder.function();
    RELEASE_ASSERT(isCoveredByOneHandler(function, site));

    Arguments arguments = copyFirstExitArguments(function, site);
    Vector<uint32_t, 4> constantPositions;
    for (uint32_t position = 0; position < arguments.size(); ++position) {
        if (position != site.exitArgument && isDefinedInSite(function, site, arguments[position]))
            constantPositions.append(position);
    }

    builder.setOrigin(function.at(site.entryBlock).terminator.origin());
    builder.splitBlock({ site.entryBlock, site.firstIndex });
    ProgramPoint point { site.entryBlock, site.firstIndex };
    for (uint32_t position : constantPositions) {
        Value constant(function.at(arguments[position]));
        RELEASE_ASSERT(isRematerializable(constant));
        arguments[position] = builder.insertBefore(point, WTF::move(constant));
    }
    ValueID call = builder.insertBefore(point, createCall(site, body));
    if (site.exitArgument)
        arguments[*site.exitArgument] = call;
    builder.applyInsertions();
    builder.redirectEdge(Edge { site.entryBlock, 0 }, site.exitBlock, WTF::move(arguments));
}

// A function that comes early holds the first site of most groups, so the bodies are a loop of their own, over the functions as they are before a site is replaced.
static void buildBodiesInParallel(const RewriteInputs& inputs, const Vector<SiteGroup>& groups)
{
    const Program& program = inputs.editor.program();
    Vector<FunctionID> bodies = groups.map([](const SiteGroup& group) {
        return group.body;
    });
    editFunctionsInParallel(inputs.editor, inputs.build, bodies.span(), [&](unsigned index, FunctionID id, Function& body, TaskArenas& arenas) {
        const OutliningSite& site = inputs.sites[groups[index].sites.first()];
        BodyBuilder(inputs.contextOf(id), program.at(site.function), site, body, arenas.types).build();
        inputs.wasRead[id.index()] = inputs.readIfFinal(id, arenas);
    });
}

static void rewriteFunction(const RewriteInputs& inputs, FunctionID id, std::span<const uint32_t> sites, Function& function, TypeArena& types)
{
    IRBuilder builder(inputs.contextOf(id), function, types);
    for (uint32_t siteIndex : sites) {
        if (inputs.sites[siteIndex].isRegion())
            replaceRegion(builder, inputs.sites[siteIndex], inputs.bodyOfSite[siteIndex]);
    }

    Vector<ErasedValues, 4> erasedValues;
    for (uint32_t siteIndex : sites) {
        if (!inputs.sites[siteIndex].isRegion())
            replaceRun(function, inputs.sites[siteIndex], inputs.bodyOfSite[siteIndex], erasedValues);
    }
    for (const ErasedValues& erased : erasedValues)
        builder.eraseValues(erased.block, erased.indexes);

    compactFunction(function);
}

static void rewriteFunctionsInParallel(const RewriteInputs& inputs, const WorkLists& work)
{
    lowerFunctionsInParallel(inputs.editor, inputs.build, work.functions.span(), { }, [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        rewriteFunction(inputs, id, work.sitesOf(index), function, arenas.types);
        inputs.wasRead[id.index()] = inputs.readIfFinal(id, arenas);
    });
}

// What outlining has left as it was, and what held an id that was replaced when its loop ended.
static void readRemainingFunctions(const Program& program, BuildContext& build, const OutliningSiteFinder& finder, const ReadersOfOutlinedFunctions& readers, std::span<const uint8_t> wasRead)
{
    std::span<const uint8_t> hasSites = finder.hasSites();
    Vector<FunctionID> remaining = collectInParallel<FunctionID>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
        bool isRead = index < wasRead.size() && wasRead[index];
        if (!isRead && (index >= hasSites.size() || hasSites[index]))
            found.append(FunctionID(index));
    });
    readFunctionsInParallel(program, build, remaining.span(), readers.read);
}

void outlineColdOperations(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build, OutliningSiteFinder* finder, const ReadersOfOutlinedFunctions* readers)
{
    RELEASE_ASSERT(finder || !readers);
    PhaseTimes times(build, PassID::OutlineColdOperations);
    const Program& program = editor.program();
    ParallelArray<OutliningSite> sites = finder ? finder->takeSites() : findOutliningSites(program, inputs, build);
    times.didFinish("find or take sites"_s);
    const ParallelLoops& loops = build.parallelLoops();
    Vector<SiteGroup> groups;
    if (sites.size())
        groups = findProfitableGroups(program, loops, sites);
    times.didFinish("findProfitableGroups"_s);
    if (groups.isEmpty()) {
        if (readers)
            readRemainingFunctions(program, build, *finder, *readers, { });
        return;
    }

    Vector<FunctionID> bodyOfSite = addBodyHeads(editor, loops, sites, groups);
    if (readers)
        readers->didAddFunctions();
    times.didFinish("addBodyHeads"_s);
    WorkLists work = createWorkLists(loops, sites, bodyOfSite);
    Vector<uint8_t> wasRead(FillWith { }, program.functions.size(), static_cast<uint8_t>(0));
    RewriteInputs rewriting { editor, inputs, build, sites, bodyOfSite, readers, wasRead };
    times.didFinish("createWorkLists"_s);
    buildBodiesInParallel(rewriting, groups);
    times.didFinish("buildBodiesInParallel"_s);
    rewriteFunctionsInParallel(rewriting, work);
    times.didFinish("rewriteFunctionsInParallel"_s);
    if (readers)
        readRemainingFunctions(program, build, *finder, *readers, wasRead.span());
    times.didFinish("readRemainingFunctions"_s);

    build.statistics().count(StatisticID::OutlinedBodiesCreated, groups.size());
    build.statistics().count(StatisticID::ColdSitesOutlined, work.sites.size());
    build.destroyLater(WTF::move(sites));
    build.destroyLater(WTF::move(groups));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
