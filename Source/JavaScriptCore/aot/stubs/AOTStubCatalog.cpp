/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubCatalog.h"

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTStubAssembler.h"
#include "AOTStubBlob.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubRecording.h"
#include "AOTStubsOperations.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT {

static unsigned findIndex(const HashMap<uint64_t, unsigned>& indexByKey, StubKey key)
{
    auto iterator = indexByKey.find(key.bits());
    RELEASE_ASSERT(iterator != indexByKey.end());
    return iterator->value;
}

static bool usesCallConvention(const StubRecording& recording)
{
    return std::ranges::any_of(recording.entries, [](StubKey entry) {
        return signatureOf(entry).usesCallConvention;
    });
}

static void addClobbersOfEveryCall(RegisterSet& clobbered, const StubSignature& signature)
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

static bool tailCallsPreserveMostLeaf(StubKey head)
{
    if (head.family() != StubFamily::TailCallOperation)
        return false;
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entryAtOffset(*head.immediate()));
    return operation && preservesMostRegisters(*operation);
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

    if (usesCallConvention(recording)) {
        clobbered.merge(registersClobberedByCompiledCalls());
        effects.merge(Typed::Effects::unknownCode());
        leavesStubs = true;
    }
}

bool StubCatalog::Node::mergeReference(const StubReference& reference, const Node& target)
{
    // The caller of this stub calls the address, and states the effects of that call itself.
    if (reference.kind == StubReference::Kind::ReturnedAddress)
        return false;
    bool changed = false;

    // The head of an operation call recorded its operation: to this head, the body all heads share calls nothing unknown.
    bool entersSharedBody = isOperationCallHead(recording.body) && reference.target == recording.body.withoutImmediate();
    // A caller plans for the error of a failed allocation wherever a call may collect. What the poll does is not an effect of the stub.
    bool isPollAfterOperation = reference.kind == StubReference::Kind::PollAfterOperation;
    RELEASE_ASSERT(!isPollAfterOperation || leavesStubs);
    if (!entersSharedBody && !isPollAfterOperation)
        changed = effects.merge(target.effects);

    if (target.leavesStubs && !leavesStubs) {
        leavesStubs = true;
        changed = true;
    }

    // A clobber set describes the normal return.
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
        // Whoever gets the address may call it after the saved registers are restored.
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
    if (entersSharedBody && tailCallsPreserveMostLeaf(recording.body)) {
        // The shared body has the clobbers of any operation. It loads the function into a register that such a leaf preserves.
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

StubCatalog::StubCatalog(const StubOptions& options, StubPolicy&& policy)
    : m_options(options)
    , m_policy(WTF::move(policy))
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
        m_nodeOfKeyWithoutImmediate.add(key.bits(), node);
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
}

const StubCatalog::Node& StubCatalog::closedNodeAt(unsigned index) const
{
    if (!m_isClosed[index].load(std::memory_order_acquire)) [[unlikely]]
        generateAndClose(index);
    return *m_nodes[index];
}

// Every stub that a stub refers to is listed. What a closed node says is final: all that it reaches is closed with it.
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
            worklist.append(findIndex(m_nodeOfBody, bodyOf(reference.target)));
    }
    Node::closeOverReferences(addedNodes.span(), [&](StubKey body) -> const Node& {
        return *m_nodes[findIndex(m_nodeOfBody, body)];
    });
    checkDeclarations(addedNodes.span());
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

void StubCatalog::checkDeclarations(std::span<Node* const> nodes)
{
    bool allDeclarationsHold = true;
    auto reportFalseDeclaration = [&](StubKey body, const auto&... description) {
        PrintStream& out = WTF::dataFile();
        out.print("AOT: the stub "_s);
        AOT::dump(out, body);
        out.println(" "_s, description...);
        allDeclarationsHold = false;
    };

    for (const Node* added : nodes) {
        const Node& node = *added;
        StubKey body = node.recording.body;
        const StubDescriptor& descriptor = descriptorOf(body.family());

        bool leaves = node.leavesStubs || node.effects.mayThrow || node.effects.mayCollect || node.effects.mayCallUnknownCode;
        if (descriptor.leafness == StubLeafness::Leaf && leaves)
            reportFalseDeclaration(body, "is declared Leaf, but leaves the stubs or has the effects "_s, node.effects);
        if (descriptor.leafness == StubLeafness::CallsOnlyBookkeeping && (node.effects.mayThrow || node.effects.mayCollect || node.effects.mayCallUnknownCode))
            reportFalseDeclaration(body, "is declared CallsOnlyBookkeeping, but has the effects "_s, node.effects);

        if (descriptor.clobbers == StubClobbers::ClobbersOnlyScratch && !hasFrameAndKeepsFewerRegisters(body.callerFrame())) {
            RegisterSet lost = node.clobbered;
            lost.exclude(node.clobberedInline);
            lost.exclude(RegisterSet::macroClobberedGPRs());
            lost.filter(RegisterSet::allGPRs());
            if (!lost.isEmpty())
                reportFalseDeclaration(body, "is declared ClobbersOnlyScratch, but a callee may clobber "_s, lost);
        }

        for (const StubExitRecord& record : node.recording.exits) {
            bool hasReason = record.reason != RuntimeCounterID::None;
            switch (record.exit) {
            case StubExit::Miss:
                if (hasReason)
                    reportFalseDeclaration(body, "has a Miss exit to "_s, nameOf(record.operation), " with a reason"_s);
                break;
            case StubExit::Cannot:
                if (!hasReason)
                    reportFalseDeclaration(body, "has a Cannot exit to "_s, nameOf(record.operation), " without a reason"_s);
                break;
            }
        }

        if (descriptor.ownEffects.isDeclared && usesCallConvention(node.recording))
            reportFalseDeclaration(body, "declares its own effects, but is entered with the call convention"_s);
    }

    if (!allDeclarationsHold)
        CRASH();
}

StubCatalog::Answer StubCatalog::Node::createAnswer(StubKey key) const
{
    Answer answer { { signatureOf(key), clobbered, clobberedInline }, effects, leavesStubs };
    addClobbersOfEveryCall(answer.summary.clobbered, answer.summary.signature);
    addClobbersOfEveryCall(answer.summary.clobberedInline, answer.summary.signature);
    return answer;
}

// The answer for the key itself: every key without an immediate has one, and the entries of the bodies that are listed with theirs.
const StubCatalog::Answer* StubCatalog::tryFindOwnAnswer(StubKey key) const
{
    const IndexByKey& nodeOfKey = key.immediate() ? m_nodeOfBody : m_nodeOfKeyWithoutImmediate;
    auto iterator = nodeOfKey.find((key.immediate() ? bodyOf(key) : key).bits());
    if (iterator == nodeOfKey.end())
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

bool StubCatalog::bindsImmediate(StubKey key, uint32_t immediate) const
{
    ASSERT(exists(key) && !key.immediate());
    switch (descriptorOf(key.family()).binding) {
    case ImmediateBinding::Never:
        return false;
    case ImmediateBinding::OperationOffset:
        return true;
    case ImmediateBinding::ByPolicy:
        break;
    }
    if (key.operandRegister() || (key.callerFrame() != CallerFrame::HasFrame && !hasFrameAndKeepsFewerRegisters(key.callerFrame())))
        return false;

    switch (summaryOf(key).signature.boundImmediates) {
    case StubBoundImmediates::None:
        return false;
    case StubBoundImmediates::Every:
        return true;
    case StubBoundImmediates::NamesRead:
        return m_policy.namesWithOwnStub(Typed::Access::Read).get(immediate);
    case StubBoundImmediates::NamesStored:
        return m_policy.namesWithOwnStub(Typed::Access::Store).get(immediate);
    case StubBoundImmediates::NamesDefined:
        return m_policy.namesWithOwnStub(Typed::Access::Define).get(immediate);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const StubCatalog::Node* StubCatalog::tryFindNode(StubKey body) const
{
    auto iterator = m_nodeOfBody.find(body.bits());
    if (iterator == m_nodeOfBody.end())
        return nullptr;
    return &closedNodeAt(iterator->value);
}

const StubCatalog::Node& StubCatalog::nodeOf(StubKey body) const
{
    return closedNodeAt(findIndex(m_nodeOfBody, body));
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

RegisterSet StubCatalog::closedClobbersOf(const StubRecording& recording) const
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
        return *nodes[findIndex(nodeOfBody, body)];
    });
    return nodes.first()->clobbered;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
