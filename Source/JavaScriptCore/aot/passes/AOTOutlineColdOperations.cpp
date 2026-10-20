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
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t outlinedBodyOverheadBytes = 48;

namespace {

struct SiteGroup {
    Vector<uint32_t, 2> sites;
    FunctionID body;
};

struct FunctionWork {
    FunctionID id;
    Vector<uint32_t> sites;
    Vector<std::pair<uint32_t, FunctionID>> bodiesToBuild;
};

struct ErasedValues {
    BlockID block;
    BitVector indexes;
};

struct RewriteInputs {
    AnalysisContext contextOf(FunctionID id) const { return { build, editor.program(), &optimization.flow, &optimization.effects, id }; }

    ProgramEditor& editor;
    const OptimizationInputs& optimization;
    BuildContext& build;
    const Vector<OutliningSite>& sites;
    const Vector<FunctionID>& bodyOfSite;
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
    value.setWritten({ });
    value.setPrediction({ });
    value.setFunction(callee);
    value.setArgumentsOperands(site.liveIns.span());
}

static Value createCall(const OutliningSite& site, FunctionID callee)
{
    Value call = IRBuilder::make(Opcode::CallDirect);
    convertToCall(call, site, callee);
    return call;
}

static uint64_t estimatedCallBytes(const Program& program, const OutliningSite& site)
{
    // The body does not exist yet, and the estimate of a direct call does not read the callee.
    Value call = createCall(site, site.function);
    return estimatedCodeBytes(program, program.at(site.function), call);
}

static OutliningDecision decide(const Program& program, const OutliningSite& firstSite, uint64_t numberOfSites)
{
    uint64_t inlineBytes = firstSite.codeBytes;
    uint64_t callBytes = estimatedCallBytes(program, firstSite);
    if (numberOfSites * inlineBytes <= numberOfSites * callBytes + inlineBytes + outlinedBodyOverheadBytes)
        return OutliningDecision::NotProfitable;
    return OutliningDecision::Outlined;
}

static void groupEqualSites(const Program& program, const Vector<OutliningSite>& sites, std::span<const uint32_t> sitesWithEqualHash, Vector<SiteGroup>& groups)
{
    size_t firstGroupWithHash = groups.size();
    for (uint32_t site : sitesWithEqualHash) {
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

static Vector<SiteGroup> findProfitableGroups(const Program& program, const ParallelLoops& loops, const Vector<OutliningSite>& sites)
{
    Vector<uint32_t> order(sites.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    parallelSort(loops, order.mutableSpan(), [&](uint32_t a, uint32_t b) {
        return std::pair { sites[a].hash, a } < std::pair { sites[b].hash, b };
    });
    Vector<uint32_t> hashRunStarts = findIndicesInParallel(loops, order.size(), [&](unsigned position) {
        return !position || sites[order[position - 1]].hash != sites[order[position]].hash;
    });

    Vector<SiteGroup> groups = collectInParallel<SiteGroup>(loops, hashRunStarts.size(), [&](unsigned run, Vector<SiteGroup>& found) {
        size_t begin = hashRunStarts[run];
        size_t end = run + 1 < hashRunStarts.size() ? hashRunStarts[run + 1] : order.size();
        size_t firstGroupWithHash = found.size();
        groupEqualSites(program, sites, order.subspan(begin, end - begin), found);
        found.removeAllMatching([&](const SiteGroup& group) {
            return decide(program, sites[group.sites.first()], group.sites.size()) != OutliningDecision::Outlined;
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

static Vector<FunctionID> addBodyHeads(ProgramEditor& editor, const Vector<OutliningSite>& sites, Vector<SiteGroup>& groups)
{
    Vector<FunctionID> bodyOfSite(FillWith { }, sites.size(), FunctionID());
    for (SiteGroup& group : groups) {
        const OutliningSite& firstSite = sites[group.sites.first()];
        group.body = editor.addFunction(createBodyHead(editor.program(), firstSite));
        for (uint32_t site : group.sites)
            bodyOfSite[site] = group.body;
    }
    return bodyOfSite;
}

static Vector<FunctionWork> createWorkLists(const Vector<OutliningSite>& sites, const Vector<SiteGroup>& groups, const Vector<FunctionID>& bodyOfSite)
{
    Vector<FunctionWork> work;
    size_t nextGroup = 0;
    for (uint32_t site = 0; site < sites.size(); ++site) {
        if (!bodyOfSite[site])
            continue;
        FunctionID id = sites[site].function;
        if (work.isEmpty() || work.last().id != id)
            work.append(FunctionWork { id, { }, { } });
        work.last().sites.append(site);
        if (nextGroup < groups.size() && groups[nextGroup].sites.first() == site)
            work.last().bodiesToBuild.append({ site, groups[nextGroup++].body });
    }
    RELEASE_ASSERT(nextGroup == groups.size());
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
    m_builder.function().at(added).shape = block.shape;
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
    convertToCall(function.at(site.root), site, body);
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

static void rewriteFunction(const RewriteInputs& inputs, const FunctionWork& work, Function& function, TypeArena& types)
{
    for (auto [siteIndex, body] : work.bodiesToBuild)
        BodyBuilder(inputs.contextOf(body), function, inputs.sites[siteIndex], inputs.editor.at(body), types).build();

    IRBuilder builder(inputs.contextOf(work.id), function, types);
    for (uint32_t siteIndex : work.sites) {
        if (inputs.sites[siteIndex].isRegion())
            replaceRegion(builder, inputs.sites[siteIndex], inputs.bodyOfSite[siteIndex]);
    }

    Vector<ErasedValues, 4> erasedValues;
    for (uint32_t siteIndex : work.sites) {
        if (!inputs.sites[siteIndex].isRegion())
            replaceRun(function, inputs.sites[siteIndex], inputs.bodyOfSite[siteIndex], erasedValues);
    }
    for (const ErasedValues& erased : erasedValues)
        builder.eraseValues(erased.block, erased.indexes);

    compactFunction(function);
}

static void rewriteFunctionsInParallel(const RewriteInputs& inputs, const Vector<FunctionWork>& work, const Vector<SiteGroup>& groups)
{
    Vector<FunctionID> functions = work.map([](const FunctionWork& functionWork) {
        return functionWork.id;
    });
    Vector<FunctionID> bodies = groups.map([](const SiteGroup& group) {
        return group.body;
    });
    editFunctionsInParallel(inputs.editor, inputs.build, functions.span(), bodies.span(), [&](unsigned index, FunctionID, Function& function, TaskArenas& arenas) {
        rewriteFunction(inputs, work[index], function, arenas.types);
    });
}

void outlineColdOperations(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build)
{
    const Program& program = editor.program();
    Vector<OutliningSite> sites = findOutliningSites(program, inputs, build);
    if (sites.isEmpty())
        return;

    Vector<SiteGroup> groups = findProfitableGroups(program, build.parallelLoops(), sites);
    if (groups.isEmpty())
        return;

    Vector<FunctionID> bodyOfSite = addBodyHeads(editor, sites, groups);
    Vector<FunctionWork> work = createWorkLists(sites, groups, bodyOfSite);
    rewriteFunctionsInParallel({ editor, inputs, build, sites, bodyOfSite }, work, groups);

    uint64_t numberOfOutlinedSites = 0;
    for (const SiteGroup& group : groups)
        numberOfOutlinedSites += group.sites.size();
    build.statistics().count(StatisticID::OutlinedBodiesCreated, groups.size());
    build.statistics().count(StatisticID::ColdSitesOutlined, numberOfOutlinedSites);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
