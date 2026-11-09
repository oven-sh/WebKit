/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubCatalog.h"

#if ENABLE(AOT)

#include "AOTInliningAcrossFiles.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTStubAssembler.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubRecording.h"
#include "AOTStubsOperations.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT {

static_assert(descriptorOf(StubFamily::TailCallOperation).binding == ImmediateBinding::OperationOffset);

static unsigned findIndexInStubCatalog(const HashMap<uint64_t, unsigned>& indexByKey, StubKey key)
{
    auto iterator = indexByKey.find(key.bits());
    RELEASE_ASSERT(iterator != indexByKey.end());
    return iterator->value;
}

static bool isStubEnteredWithCallConvention(const StubRecording& recording)
{
    return std::ranges::any_of(recording.entries, [](StubKey entry) {
        return signatureOf(entry).usesCallConvention;
    });
}

static void addClobbersOfEveryStubCall(RegisterSet& clobbered, const StubSignature& signature)
{
    clobbered.merge(RegisterSet::macroClobberedGPRs());
    clobbered.merge(RegisterSet::macroClobberedFPRs());
    for (Reg result : { signature.firstResult, signature.secondResult }) {
        if (result)
            clobbered.add(result, IgnoreVectors);
    }
    if (signature.immediate)
        clobbered.add(*signature.immediate, IgnoreVectors);
}

static bool stubHeadTailCallsPreserveMostLeaf(StubKey head)
{
    if (head.withoutImmediate() != tailCallOperation(OperationCall::FirstArgument::None))
        return false;
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entryAtOffset(*head.immediate()));
    return operation && preservesMostRegisters(*operation);
}

static bool mergeStubEffects(Typed::Effects& effects, const Typed::Effects& added)
{
    Typed::Effects merged = effects;
    merged.merge(added);
    if (merged == effects)
        return false;
    effects = merged;
    return true;
}

struct StubCatalog::Node {
    explicit Node(StubRecording&&);

    bool mergeReference(const StubReference&, const Node& target);

    template<typename FindNode>
    static void closeOverReferences(std::span<Node* const>, const FindNode&);

    Answer createAnswer(StubKey) const;

    StubRecording recording;
    RegisterSet clobbered;
    RegisterSet clobberedInline;
    Typed::Effects effects;
    bool leavesStubs;
    Vector<std::pair<StubKey, Answer>, 2> answers;
};

StubCatalog::Node::Node(StubRecording&& generated)
    : recording(WTF::move(generated))
    , clobbered(recording.clobbered())
    , clobberedInline(recording.clobberedInline)
    , effects(descriptorOf(recording.body.family()).ownEffects.toEffects())
    , leavesStubs(recording.leavesStubs)
{
    for (StubKey entry : recording.entries)
        effects.merge(descriptorOf(entry.family()).ownEffects.toEffects());
    if (recording.hasUnknownEffects)
        effects.merge(Typed::Effects::unknownCode());
    for (size_t index : recording.operationsCalled)
        effects.merge(AOT::effectsOf(static_cast<Entry>(index)));

    if (isStubEnteredWithCallConvention(recording)) {
        clobbered.merge(registersClobberedByCompiledCalls());
        effects.merge(Typed::Effects::unknownCode());
        leavesStubs = true;
    }
}

bool StubCatalog::Node::mergeReference(const StubReference& reference, const Node& target)
{
    if (reference.kind == StubReference::Kind::ReturnedAddress)
        return false;
    bool changed = false;

    bool entersSharedBody = isOperationCallHead(recording.body) && reference.target == recording.body.withoutImmediate();
    bool isPollAfterOperation = reference.kind == StubReference::Kind::PollAfterOperation;
    RELEASE_ASSERT(!isPollAfterOperation || leavesStubs);
    if (!entersSharedBody && !isPollAfterOperation)
        changed = mergeStubEffects(effects, target.effects);

    if (target.leavesStubs && !leavesStubs) {
        leavesStubs = true;
        changed = true;
    }

    if (neverReturns(reference.target))
        return changed;

    RegisterSet restored;
    switch (reference.kind) {
    case StubReference::Kind::Call:
    case StubReference::Kind::PollAfterOperation:
    case StubReference::Kind::Jump:
        restored = reference.savedAround;
        break;
    case StubReference::Kind::Address:
        break;
    case StubReference::Kind::ReturnedAddress:
        RELEASE_ASSERT_NOT_REACHED();
    }
    auto mergeClobbers = [&](RegisterSet& clobberedHere, RegisterSet clobberedByTarget) {
        clobberedByTarget.exclude(restored);
        if (clobberedHere.subsumes(clobberedByTarget))
            return;
        clobberedHere.merge(clobberedByTarget);
        changed = true;
    };
    RegisterSet clobberedByTarget = target.clobbered;
    if (entersSharedBody && stubHeadTailCallsPreserveMostLeaf(recording.body)) {
        clobberedByTarget = target.clobberedInline;
        clobberedByTarget.merge(registersClobberedByPreserveMostLeaves());
        clobberedByTarget.add(gprOf(StubRegister::Operation), IgnoreVectors);
    }
    mergeClobbers(clobbered, clobberedByTarget);
    mergeClobbers(clobberedInline, target.clobberedInline);
    return changed;
}

template<typename FindNode>
void StubCatalog::Node::closeOverReferences(std::span<Node* const> nodes, const FindNode& findNode)
{
    bool changed;
    do {
        changed = false;
        for (Node* node : nodes) {
            for (const StubReference& reference : node->recording.references)
                changed |= node->mergeReference(reference, findNode(bodyOf(reference.target)));
        }
    } while (changed);
}

StubCatalog::Answer StubCatalog::Node::createAnswer(StubKey key) const
{
    Answer answer { { signatureOf(key), clobbered, clobberedInline }, effects, leavesStubs };
    addClobbersOfEveryStubCall(answer.summary.clobbered, answer.summary.signature);
    addClobbersOfEveryStubCall(answer.summary.clobberedInline, answer.summary.signature);
    return answer;
}

static std::atomic<uint64_t> nextStubCatalogIdentifier { 1 };

StubCatalog::StubCatalog(const StubOptions& options, StubPolicy&& policy)
    : m_options(options)
    , m_policy(WTF::move(policy))
    , m_identifier(nextStubCatalogIdentifier.fetch_add(1, std::memory_order_relaxed))
{
    listBodies();
}

StubCatalog::~StubCatalog() = default;

void StubCatalog::listBodies()
{
    auto ensureNode = [&](StubKey body) {
        auto result = m_nodeOfBody.add(body.bits(), m_bodies.size());
        if (result.isNewEntry) {
            m_bodies.append(body);
            m_keysWithoutImmediateOfNode.constructAndAppend();
        }
        return result.iterator->value;
    };

    forEachKeyWithoutImmediate([&](StubKey key) {
        unsigned node = ensureNode(bodyOf(key));
        m_answerOfKeyWithoutImmediate.add(key.bits(), PlaceOfAnswer { node, static_cast<unsigned>(m_keysWithoutImmediateOfNode[node].size()), m_answerOfKeyWithoutImmediate.size() });
        m_keysWithoutImmediateOfNode[node].append(key);
        if (descriptorOf(key.family()).binding != ImmediateBinding::OperationOffset)
            return;
        for (const OperationPrelude& prelude : operationPreludes()) {
            StubKey bound = key.withImmediate(offsetInRuntimeTable(prelude.operation));
            if (exists(bound))
                ensureNode(bodyOf(bound));
        }
        if (key != tailCallOperation(OperationCall::FirstArgument::None))
            return;
        for (unsigned index = 0; index < numberOfRuntimeOperations; ++index) {
            RuntimeOperation operation = static_cast<RuntimeOperation>(index);
            if (preservesMostRegisters(operation))
                ensureNode(bodyOf(key.withImmediate(offsetInRuntimeTable(entryOf(operation)))));
        }
    });
    m_nodes.grow(m_bodies.size());
    m_isClosed = Vector<std::atomic<bool>>(m_bodies.size());
    for (std::atomic<bool>& isClosed : m_isClosed)
        isClosed.store(false, std::memory_order_relaxed);
    m_clobberedByRegisterVariants.grow(m_answerOfKeyWithoutImmediate.size());
    m_knowsRegisterVariants = Vector<std::atomic<bool>>(m_answerOfKeyWithoutImmediate.size());
    for (std::atomic<bool>& isKnown : m_knowsRegisterVariants)
        isKnown.store(false, std::memory_order_relaxed);
    m_clobberedBesideResultByRegisterVariants.grow(m_answerOfKeyWithoutImmediate.size());
    m_knowsWhatRegisterVariantsClobberBesideResult = Vector<std::atomic<bool>>(m_answerOfKeyWithoutImmediate.size());
    for (std::atomic<bool>& isKnown : m_knowsWhatRegisterVariantsClobberBesideResult)
        isKnown.store(false, std::memory_order_relaxed);
}

const StubCatalog::Node& StubCatalog::closedNodeAt(unsigned index) const
{
    if (!m_isClosed[index].load(std::memory_order_acquire)) [[unlikely]]
        generateAndClose(index);
    return *m_nodes[index];
}

template<typename... Description>
static void reportFalseStubDeclaration(StubKey body, const Description&... description)
{
    const StubDescriptor& descriptor = descriptorOf(body.family());
    PrintStream& out = WTF::dataFile();
    out.print("AOT: the stub "_s, descriptor.name);
    if (body.form() < descriptor.formNames.size())
        out.print("."_s, descriptor.formNames[body.form()]);
    out.print(", count "_s, body.count(), ", caller frame "_s, static_cast<unsigned>(body.callerFrame()));
    if (std::optional<GPRReg> operandRegister = body.operandRegister())
        out.print(", operand in "_s, *operandRegister);
    if (std::optional<uint32_t> immediate = body.immediate())
        out.print(", operation "_s, nameOf(entryAtOffset(*immediate)));
    out.println(" "_s, description...);
}

#if ASSERT_ENABLED

static bool stubRecordingsGiveSameAnswers(const StubRecording& first, const StubRecording& second)
{
    return first.clobberedInline == second.clobberedInline
        && first.clobberedByCallees == second.clobberedByCallees
        && first.hasUnknownEffects == second.hasUnknownEffects
        && first.leavesStubs == second.leavesStubs
        && first.entries == second.entries
        && first.references == second.references
        && first.operationsCalled == second.operationsCalled;
}

#endif // ASSERT_ENABLED

void StubCatalog::checkDeclarations(std::span<Node* const> nodes)
{
    bool allDeclarationsHold = true;
    auto report = [&](StubKey body, const auto&... description) {
        reportFalseStubDeclaration(body, description...);
        allDeclarationsHold = false;
    };

    for (const Node* added : nodes) {
        const Node& node = *added;
        StubKey body = node.recording.body;
        const StubDescriptor& descriptor = descriptorOf(body.family());

        bool mayThrow = node.effects.mayThrow;
        bool mayCollect = node.effects.mayCollect;
        bool mayCallUnknownCode = node.effects.mayCallUnknownCode;
        bool hasEffectsOfCalls = mayThrow || mayCollect || mayCallUnknownCode;
        switch (descriptor.leafness) {
        case StubLeafness::MayCall:
            break;
        case StubLeafness::Leaf:
            if (node.leavesStubs || hasEffectsOfCalls)
                report(body, "is declared Leaf, but leaves the stubs: "_s, node.leavesStubs, ", may throw: "_s, mayThrow, ", may collect: "_s, mayCollect, ", may call unknown code: "_s, mayCallUnknownCode);
            break;
        case StubLeafness::CallsOnlyBookkeeping:
            if (hasEffectsOfCalls)
                report(body, "is declared CallsOnlyBookkeeping, but may throw: "_s, mayThrow, ", may collect: "_s, mayCollect, ", may call unknown code: "_s, mayCallUnknownCode);
            break;
        }

#if ASSERT_ENABLED
        switch (descriptor.clobbers) {
        case StubClobbers::ClobbersLikeCall:
            break;
        case StubClobbers::ClobbersOnlyScratch: {
            if (body.callerFrame() == CallerFrame::KeepsNoFPRs)
                break;
            RegisterSet lost = node.clobbered;
            lost.exclude(node.clobberedInline);
            lost.exclude(RegisterSet::macroClobberedGPRs());
            lost.filter(RegisterSet::allGPRs());
            if (!lost.isEmpty())
                report(body, "is declared ClobbersOnlyScratch, but a callee may clobber "_s, lost);
            break;
        }
        }

        if (descriptor.ownEffects.isDeclared && isStubEnteredWithCallConvention(node.recording))
            report(body, "declares its own effects, but is entered with the call convention"_s);
#endif
    }

    if (!allDeclarationsHold)
        CRASH();
}

NEVER_INLINE void StubCatalog::generateAndClose(unsigned first) const
{
    Locker locker { m_lock };
    if (m_isClosed[first].load(std::memory_order_relaxed))
        return;
    StubAssembler jit(m_options, *this);
    Vector<unsigned, 32> added;
    Vector<Node*, 32> addedNodes;
    Vector<unsigned, 32> worklist { first };
    while (!worklist.isEmpty()) {
        unsigned index = worklist.takeLast();
        if (m_nodes[index])
            continue;
        m_nodes[index] = makeUniqueWithoutFastMallocCheck<Node>(jit.generate(m_bodies[index]));
        added.append(index);
        addedNodes.append(m_nodes[index].get());
        for (const StubReference& reference : m_nodes[index]->recording.references)
            worklist.append(findIndexInStubCatalog(m_nodeOfBody, bodyOf(reference.target)));
    }
    Node::closeOverReferences(addedNodes.span(), [&](StubKey body) -> const Node& {
        return *m_nodes[findIndexInStubCatalog(m_nodeOfBody, body)];
    });
    for (const Node* node : addedNodes) {
        StubKey body = node->recording.body;
        RELEASE_ASSERT(!isOperationCallHead(body) || node->effects.writes.isSubsetOf(AOT::effectsOf(entryAtOffset(*body.immediate())).writes));
        const StubSignature signature = signatureOf(body);
        if (Reg filled = signature.filledOutOfResultInFrontOfBody; filled && !signature.usesCallConvention && !std::ranges::contains(std::span(signature.operands).first(signature.numberOfOperands), filled))
            RELEASE_ASSERT(!node->clobbered.contains(filled, IgnoreVectors));
        for (const StubReference& reference : node->recording.references) {
            if (reference.kind != StubReference::Kind::ReturnedAddress)
                continue;
            RegisterSet lost = m_nodes[findIndexInStubCatalog(m_nodeOfBody, bodyOf(reference.target))]->clobbered;
            lost.filter(RegisterSet::fromIterable(allocatableGPRs).merge(RegisterSet::allFPRs()));
            lost.exclude(registersClobberedByCompiledCalls());
            lost.exclude(RegisterSet::macroClobberedFPRs());
            RELEASE_ASSERT(lost.isEmpty());
        }
    }

    checkDeclarations(addedNodes.span());
#if ASSERT_ENABLED
    StubOptions optionsWithOtherFieldTable = m_options;
    optionsWithOtherFieldTable.maxNameInFieldTable = m_options.maxNameInFieldTable ? 0 : UINT16_MAX;
    StubAssembler jitWithOtherFieldTable(optionsWithOtherFieldTable, *this);
    for (unsigned index : added) {
        if (stubRecordingsGiveSameAnswers(jitWithOtherFieldTable.generate(m_bodies[index]), m_nodes[index]->recording))
            continue;
        reportFalseStubDeclaration(m_bodies[index], "has registers, references or effects that follow StubOptions::maxNameInFieldTable"_s);
        CRASH();
    }
#endif

    for (unsigned index : added) {
        Node& node = *m_nodes[index];
        for (StubKey key : m_keysWithoutImmediateOfNode[index])
            node.answers.append({ key, node.createAnswer(key) });
        if (!node.recording.body.immediate())
            continue;
        for (StubKey entry : node.recording.entries) {
            if (!entry.immediate())
                continue;
            RELEASE_ASSERT(bodyOf(entry) == node.recording.body);
            node.answers.append({ entry, node.createAnswer(entry) });
        }
    }
    for (unsigned index : added)
        m_isClosed[index].store(true, std::memory_order_release);
}

const StubCatalog::Node& StubCatalog::nodeOf(StubKey body) const
{
    return closedNodeAt(findIndexInStubCatalog(m_nodeOfBody, body));
}

ALWAYS_INLINE_INTO_OTHER_FILES const StubCatalog::Answer* StubCatalog::tryFindOwnAnswer(StubKey key) const
{
    if (!key.immediate()) {
        auto place = m_answerOfKeyWithoutImmediate.find(key.bits());
        if (place == m_answerOfKeyWithoutImmediate.end())
            return nullptr;
        auto& [answeredKey, answer] = closedNodeAt(place->value.node).answers[place->value.answer];
        RELEASE_ASSERT(answeredKey == key);
        return &answer;
    }

    auto iterator = m_nodeOfBody.find(bodyOf(key).bits());
    if (iterator == m_nodeOfBody.end())
        return nullptr;
    for (auto& [answeredKey, answer] : closedNodeAt(iterator->value).answers) {
        if (answeredKey == key)
            return &answer;
    }
    return nullptr;
}

const StubCatalog::Answer& StubCatalog::answerFor(StubKey key) const
{
    RELEASE_ASSERT(exists(key));
    if (const Answer* answer = tryFindOwnAnswer(key))
        return *answer;

    RELEASE_ASSERT(key.immediate());
    const Answer* answer = tryFindOwnAnswer(key.withoutImmediate());
    RELEASE_ASSERT(answer);
    return *answer;
}

const StubSummary& StubCatalog::summaryOf(StubKey key) const
{
    return answerFor(key).summary;
}

const RegisterSet& StubCatalog::clobberedByRegisterVariantsOf(StubKey key) const
{
    if (key.immediate())
        return summaryOf(key).clobbered;

    auto place = m_answerOfKeyWithoutImmediate.find(key.bits());
    RELEASE_ASSERT(place != m_answerOfKeyWithoutImmediate.end());
    unsigned number = place->value.numberOfKey;
    if (!m_knowsRegisterVariants[number].load(std::memory_order_acquire)) [[unlikely]] {
        RegisterSet clobbered;
        forEachRegisterVariant(key, [&](StubKey variant) {
            clobbered.merge(summaryOf(variant).clobbered);
        });
        Locker locker { m_lock };
        if (!m_knowsRegisterVariants[number].load(std::memory_order_relaxed)) {
            m_clobberedByRegisterVariants[number] = clobbered;
            m_knowsRegisterVariants[number].store(true, std::memory_order_release);
        }
    }
    return m_clobberedByRegisterVariants[number];
}

const RegisterSet& StubCatalog::clobberedBesideResultByRegisterVariantsOf(StubKey key) const
{
    RELEASE_ASSERT(!key.immediate() && !key.operandRegister());
    auto place = m_answerOfKeyWithoutImmediate.find(key.bits());
    RELEASE_ASSERT(place != m_answerOfKeyWithoutImmediate.end());
    unsigned number = place->value.numberOfKey;
    if (!m_knowsWhatRegisterVariantsClobberBesideResult[number].load(std::memory_order_acquire)) [[unlikely]] {
        RegisterSet clobbered;
        forEachRegisterVariant(key, [&](StubKey variant) {
            if (variant == key && !namesRegisterOfResult(descriptorOf(key.family()).operandRegisters))
                return;
            const StubSummary& summary = summaryOf(variant);
            RELEASE_ASSERT(summary.signature.result == StubResult::GPR && (variant == key || summary.signature.firstResult == Reg(*variant.operandRegister())));
            RegisterSet lost = summary.clobbered;
            lost.remove(summary.signature.firstResult);
            clobbered.merge(lost);
        });
        Locker locker { m_lock };
        if (!m_knowsWhatRegisterVariantsClobberBesideResult[number].load(std::memory_order_relaxed)) {
            m_clobberedBesideResultByRegisterVariants[number] = clobbered;
            m_knowsWhatRegisterVariantsClobberBesideResult[number].store(true, std::memory_order_release);
        }
    }
    return m_clobberedBesideResultByRegisterVariants[number];
}

Typed::Effects StubCatalog::effectsOf(StubKey key) const
{
    RELEASE_ASSERT(exists(key));
    if (!isOperationCallHead(key))
        return answerFor(key).effects;

    if (const Answer* answer = tryFindOwnAnswer(key))
        return answer->effects;

    Typed::Effects effects = descriptorOf(key.family()).ownEffects.toEffects();
    effects.merge(AOT::effectsOf(entryAtOffset(*key.immediate())));
    return effects;
}

bool StubCatalog::isLeaf(StubKey key) const
{
    return !answerFor(key).leavesStubs;
}

bool StubCatalog::bindsImmediate(StubKey key) const
{
    ASSERT(exists(key) && !key.immediate());
    switch (descriptorOf(key.family()).binding) {
    case ImmediateBinding::Never:
    case ImmediateBinding::InThunk:
        return false;
    case ImmediateBinding::OperationOffset:
        return true;
    case ImmediateBinding::Every:
        return !key.operandRegister() && (key.callerFrame() == CallerFrame::HasFrame || key.callerFrame() == CallerFrame::KeepsNoFPRs);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const StubRecording& StubCatalog::recordingOf(StubKey body) const
{
    return nodeOf(body).recording;
}

bool StubCatalog::hasRecordingOf(StubKey body) const
{
    return m_nodeOfBody.contains(body.bits());
}

StubRecording StubCatalog::dryRun(StubKey body) const
{
    StubAssembler jit(m_options, *this);
    return jit.generate(body);
}

const StubCatalog::Node* StubCatalog::tryFindNode(StubKey body) const
{
    auto iterator = m_nodeOfBody.find(body.bits());
    if (iterator == m_nodeOfBody.end())
        return nullptr;
    return &closedNodeAt(iterator->value);
}

bool StubCatalog::answersCover(const StubRecording& recording) const
{
    Vector<std::unique_ptr<Node>> nodes;
    IndexByKey nodeOfBody;
    Vector<StubKey> worklist;
    auto addNode = [&](StubRecording&& added) {
        nodeOfBody.add(added.body.bits(), nodes.size());
        for (const StubReference& reference : added.references)
            worklist.append(bodyOf(reference.target));
        nodes.append(makeUniqueWithoutFastMallocCheck<Node>(WTF::move(added)));
    };

    addNode(StubRecording(recording));
    while (!worklist.isEmpty()) {
        StubKey body = worklist.takeLast();
        if (!hasRecordingOf(body) && !nodeOfBody.contains(body.bits()))
            addNode(dryRun(body));
    }
    Vector<Node*> ownNodes = nodes.map([](const std::unique_ptr<Node>& node) {
        return node.get();
    });
    Node::closeOverReferences(ownNodes.span(), [&](StubKey body) -> const Node& {
        if (const Node* node = tryFindNode(body))
            return *node;
        return *nodes[findIndexInStubCatalog(nodeOfBody, body)];
    });

    const Node& closed = *nodes.first();
    return std::ranges::all_of(recording.entries, [&](StubKey entry) {
        Typed::Effects effects = effectsOf(entry);
        return summaryOf(entry).clobbered.subsumes(closed.clobbered) && !mergeStubEffects(effects, closed.effects) && (!closed.leavesStubs || !isLeaf(entry));
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
