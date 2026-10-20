/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowBodyCopies.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Body = ValueFlowBody;
using NodeName = ValueFlowNodeName;

class BodyCopier {
    WTF_MAKE_NONCOPYABLE(BodyCopier);
public:
    BodyCopier(const Program& program, BuildContext& build, ParallelArray<Body>& bodies)
        : m_program(program)
        , m_build(build)
        , m_bodies(bodies)
        , m_numberOfFunctions(program.functions.size())
        , m_descriptions(m_numberOfFunctions)
    {
        RELEASE_ASSERT(bodies.size() == m_numberOfFunctions + 1);
    }

    void run()
    {
        if (!m_build.limit(LimitID::MaxValueFlowCopiedNodesPercent))
            return;
        findVariablesStoredTo();
        forEachIndexInParallel(m_build.parallelLoops(), m_numberOfFunctions, [&](unsigned index) {
            describe(index);
        });
        Vector<uint32_t> calleesFirst = leaveOutCycles();
        for (uint32_t function : calleesFirst)
            findWhatIsHandedOn(function);
        choose();
        copy();
    }

private:
    struct KnownCall {
        uint32_t statement { 0 };
        FunctionID callee;
    };

    struct Description {
        Vector<KnownCall, 4> knownCalls;
        // Nothing about the function's form or its calls stands against a copy.
        bool hasFormToCopy { false };
        // A bit for each entry parameter that goes to the result, and for each that is called or stored into an object.
        uint64_t parametersInResult { 0 };
        uint64_t parametersCalledOrStored { 0 };
        bool mayBeCopied { false };
        bool isChosen { false };
        // The body itself and its copies.
        uint64_t instances { 1 };
    };

    void findVariablesStoredTo()
    {
        m_isStoredTo.ensureSize(m_program.variables.size());
        for (unsigned index = 0; index < m_numberOfFunctions; ++index) {
            for (const Body::Edge& edge : m_bodies[index].edgesOnceReached) {
                if (edge.to.kind == NodeName::Kind::Variable)
                    m_isStoredTo.quickSet(edge.to.owner);
            }
            for (const Body::Seed& seed : m_bodies[index].seedsOnceReached) {
                if (seed.node.kind == NodeName::Kind::Variable)
                    m_isStoredTo.quickSet(seed.node.owner);
            }
        }
    }

    // A rule may still add to what the operand holds. The rule of the call follows every other callee as it follows any.
    FunctionID knownCalleeOf(const Body& body, const Body::Statement& statement) const
    {
        if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Call)
            return { };
        const Body::Operand& callee = body.operands[statement.firstOperand];
        if (callee.mask)
            return { };
        if (!callee.node) {
            const Body::Constant& constant = body.constants[callee.constant];
            bool isOneClosure = !constant.primitives && constant.objects.size() == 1 && constant.objects[0].kind == ValueFlowObjectKind::Closure;
            return isOneClosure ? FunctionID(constant.objects[0].index) : FunctionID();
        }
        if (callee.constant || callee.node.kind != NodeName::Kind::Variable)
            return { };
        const Variable& variable = m_program.variables[callee.node.owner];
        if (variable.initialization != VariableInitialization::Function || m_isStoredTo.quickGet(callee.node.owner))
            return { };
        return variable.function;
    }

    static bool callsOperand(Opcode opcode)
    {
        switch (opcode) {
        case Opcode::Call:
        case Opcode::CallList:
        case Opcode::Construct:
        case Opcode::ConstructList:
        case Opcode::SuperConstruct:
        case Opcode::SuperConstructList:
            return true;
        default:
            return false;
        }
    }

    void describe(unsigned index)
    {
        const Function& function = m_program.functions[index];
        const Body& body = m_bodies[index];
        Description& description = m_descriptions[index];
        for (uint32_t statement = 0; statement < body.statements.size(); ++statement) {
            if (FunctionID callee = knownCalleeOf(body, body.statements[statement]))
                description.knownCalls.append({ statement, callee });
        }

        constexpr OptionSet<FunctionFlags> flagsOfOtherEntries { FunctionFlags::IsAsync, FunctionFlags::IsGenerator, FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest };
        bool mayBeCalled = function.constructorForm() == ConstructorForm::NotConstructor || function.kind == FunctionKind::Plain;
        description.hasFormToCopy = function.hasBody() && !function.isModuleBody() && !function.hasAny(flagsOfOtherEntries) && mayBeCalled && body.numberOfResults == 1 && body.numberOfEntryParameters <= 64;
    }

    // A copy pays where this or an argument goes to the result, is called, or is stored into an object. An argument of a call inside the function counts
    // only where that call gets a copy in which the same happens to it. The callees have been looked at before.
    void findWhatIsHandedOn(uint32_t index)
    {
        const Function& function = m_program.functions[index];
        const Body& body = m_bodies[index];
        Description& description = m_descriptions[index];

        Vector<uint64_t, 32> parametersIn;
        parametersIn.fill(0, body.nodes.size());
        for (unsigned parameter = 0; parameter < body.numberOfEntryParameters; ++parameter) {
            EntryParameter kind = function.entryParameterKind(parameter);
            if (kind == EntryParameter::This || kind == EntryParameter::Parameter)
                parametersIn[parameter] = 1ull << parameter;
        }
        auto parametersOf = [&](const Body::Operand& operand) {
            return operand.node.kind == NodeName::Kind::OfBody ? parametersIn[operand.node.owner] : 0;
        };
        bool hasChanged = true;
        auto add = [&](NodeName node, uint64_t parameters) {
            if (node.kind != NodeName::Kind::OfBody || !(parameters & ~parametersIn[node.owner]))
                return;
            parametersIn[node.owner] |= parameters;
            hasChanged = true;
        };
        auto forEachOperandOfCopy = [&](const KnownCall& call, NOESCAPE const Invocable<void(unsigned parameter, uint64_t parameters)> auto& functor) {
            const Function& callee = m_program.at(call.callee);
            std::span<const Body::Operand> operands = body.operandsOf(body.statements[call.statement]);
            if (auto parameter = callee.thisParameterIndex())
                functor(*parameter, parametersOf(operands[1]));
            for (unsigned argument = 0; argument < callee.numberOfPositionalParameters() && 2 + argument < operands.size(); ++argument)
                functor(callee.firstParameterIndex() + argument, parametersOf(operands[2 + argument]));
        };

        while (hasChanged) {
            hasChanged = false;
            for (const Body::Edge& edge : body.edges) {
                if (edge.from.kind == NodeName::Kind::OfBody)
                    add(edge.to, parametersIn[edge.from.owner]);
            }
            forEachCopiedCall(index, [&](const KnownCall& call) {
                forEachOperandOfCopy(call, [&](unsigned parameter, uint64_t parameters) {
                    if (m_descriptions[call.callee.index()].parametersInResult >> parameter & 1)
                        add(body.statements[call.statement].result, parameters);
                });
            }, &Description::mayBeCopied);
        }

        description.parametersInResult = parametersIn[body.result(0)];
        for (const Body::Statement& statement : body.statements) {
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;
            std::span<const Body::Operand> operands = body.operandsOf(statement);
            if (callsOperand(statement.opcode)) {
                description.parametersCalledOrStored |= parametersOf(operands[0]);
                continue;
            }
            for (uint32_t operand = 0; operand < operands.size(); ++operand) {
                if (operandInfoOf(statement.opcode, operand).uses.contains(OperandUse::Retains))
                    description.parametersCalledOrStored |= parametersOf(operands[operand]);
            }
        }
        forEachCopiedCall(index, [&](const KnownCall& call) {
            forEachOperandOfCopy(call, [&](unsigned parameter, uint64_t parameters) {
                if (m_descriptions[call.callee.index()].parametersCalledOrStored >> parameter & 1)
                    description.parametersCalledOrStored |= parameters;
            });
        }, &Description::mayBeCopied);
        description.mayBeCopied = description.parametersInResult || description.parametersCalledOrStored;
    }

    void forEachCopiedCall(unsigned index, NOESCAPE const Invocable<void(const KnownCall&)> auto& functor, bool Description::* isCopied) const
    {
        for (const KnownCall& call : m_descriptions[index].knownCalls) {
            if (m_descriptions[call.callee.index()].*isCopied)
                functor(call);
        }
    }

    // A function that reaches itself through calls that would be copied has no last copy. Answers the others, each behind the functions it calls.
    Vector<uint32_t> leaveOutCycles()
    {
        constexpr uint32_t unvisited = UINT32_MAX;
        struct Frame {
            uint32_t function;
            uint32_t nextCall;
        };
        Vector<uint32_t> order;
        Vector<uint32_t> lowest;
        order.fill(unvisited, m_numberOfFunctions);
        lowest.fill(unvisited, m_numberOfFunctions);
        BitVector isOnStack(m_numberOfFunctions);
        BitVector isInCycle(m_numberOfFunctions);
        Vector<uint32_t> stack;
        Vector<Frame> frames;
        Vector<uint32_t> calleesFirst;
        uint32_t next = 0;
        auto enter = [&](uint32_t function) {
            order[function] = lowest[function] = next++;
            stack.append(function);
            isOnStack.quickSet(function);
            frames.append({ function, 0 });
        };

        for (uint32_t root = 0; root < m_numberOfFunctions; ++root) {
            if (!m_descriptions[root].hasFormToCopy || order[root] != unvisited)
                continue;
            enter(root);
            while (!frames.isEmpty()) {
                uint32_t function = frames.last().function;
                const auto& calls = m_descriptions[function].knownCalls;
                if (frames.last().nextCall < calls.size()) {
                    uint32_t callee = calls[frames.last().nextCall++].callee.index();
                    if (!m_descriptions[callee].hasFormToCopy)
                        continue;
                    if (callee == function)
                        isInCycle.quickSet(function);
                    if (order[callee] == unvisited)
                        enter(callee);
                    else if (isOnStack.quickGet(callee))
                        lowest[function] = std::min(lowest[function], order[callee]);
                    continue;
                }
                frames.removeLast();
                if (!frames.isEmpty())
                    lowest[frames.last().function] = std::min(lowest[frames.last().function], lowest[function]);
                if (lowest[function] != order[function])
                    continue;
                bool isAlone = stack.last() == function;
                for (uint32_t member = unvisited; member != function;) {
                    member = stack.takeLast();
                    isOnStack.quickClear(member);
                    if (!isAlone)
                        isInCycle.quickSet(member);
                    calleesFirst.append(member);
                }
            }
        }
        calleesFirst.removeAllMatching([&](uint32_t function) {
            return isInCycle.quickGet(function);
        });
        return calleesFirst;
    }

    // How many nodes that many more instances of the function add, with the copies inside them. Nothing where that is more than `most`.
    std::optional<uint64_t> addInstances(uint32_t function, uint64_t count, uint64_t most, bool isCounted)
    {
        struct Addition {
            uint32_t function;
            uint64_t count;
        };
        Vector<Addition, 16> worklist { Addition { function, count } };
        uint64_t added = 0;
        while (!worklist.isEmpty()) {
            Addition addition = worklist.takeLast();
            added += addition.count * m_bodies[addition.function].nodes.size();
            if (added > most)
                return std::nullopt;
            if (isCounted)
                m_descriptions[addition.function].instances += addition.count;
            forEachCopiedCall(addition.function, [&](const KnownCall& call) {
                worklist.append({ call.callee.index(), addition.count });
            }, &Description::isChosen);
        }
        return added;
    }

    // The smallest bodies first, for as long as the limit lasts.
    void choose()
    {
        Vector<Vector<uint32_t, 2>> callersOf(m_numberOfFunctions);
        uint64_t numberOfNodes = 0;
        for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
            numberOfNodes += m_bodies[index].nodes.size();
            forEachCopiedCall(index, [&](const KnownCall& call) {
                callersOf[call.callee.index()].append(index);
            }, &Description::mayBeCopied);
        }

        auto sizeOf = [&](uint32_t function) {
            return m_bodies[function].nodes.size() + m_bodies[function].statements.size();
        };
        Vector<uint32_t> candidates;
        for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
            if (!callersOf[index].isEmpty())
                candidates.append(index);
        }
        std::ranges::stable_sort(candidates, { }, sizeOf);

        uint64_t left = numberOfNodes * m_build.limit(LimitID::MaxValueFlowCopiedNodesPercent) / 100;
        for (uint32_t candidate : candidates) {
            uint64_t count = 0;
            for (uint32_t caller : callersOf[candidate])
                count += m_descriptions[caller].instances;
            std::optional<uint64_t> added = addInstances(candidate, count, left, false);
            if (!added) {
                m_build.gaveUp(LimitID::MaxValueFlowCopiedNodesPercent, Subject { });
                return;
            }
            addInstances(candidate, count, left, true);
            m_descriptions[candidate].isChosen = true;
            left -= *added;
        }
    }

    void copy()
    {
        Vector<std::optional<Body>> bodiesWithCopies(m_numberOfFunctions);
        forEachIndexInParallel(m_build.parallelLoops(), m_numberOfFunctions, [&](unsigned index) {
            forEachCopiedCall(index, [&](const KnownCall& call) {
                std::optional<Body>& body = bodiesWithCopies[index];
                if (!body)
                    body = m_bodies[index];
                appendCopy(*body, call.statement, call.callee);
            }, &Description::isChosen);
        });
        for (unsigned index = 0; index < m_numberOfFunctions; ++index) {
            if (bodiesWithCopies[index])
                m_bodies[index] = WTF::move(*bodiesWithCopies[index]);
        }
    }

    static uint32_t constantOf(Body& body, Body::Constant&& constant)
    {
        body.constants.append(WTF::move(constant));
        return body.constants.size() - 1;
    }

    static void connect(Body& body, const Body::Operand& operand, NodeName target)
    {
        if (operand.mask) {
            NodeName filter = NodeName::ofBody(body.nodes.size());
            body.nodes.append({ ValueFlowBodyNodeKind::Filter, { }, operand.mask });
            body.edges.append({ filter, target });
            target = filter;
        }
        if (operand.node)
            body.edges.append({ operand.node, target });
        if (operand.constant)
            body.seedsOnceReached.append({ target, operand.constant, operand.mask });
    }

    void appendCopy(Body& into, uint32_t callIndex, FunctionID calleeID)
    {
        const Body& from = m_bodies[calleeID.index()];
        const Function& callee = m_program.at(calleeID);
        uint32_t firstNode = into.nodes.size();
        uint32_t firstSite = into.sites.size();
        uint32_t firstOperand = into.operands.size();
        uint32_t firstStatement = into.statements.size();
        // Entry 0 of both lists is the same in every body.
        uint32_t constantBefore = into.constants.size() - 1;
        uint32_t maskBefore = into.masks.size() - 1;
        auto constantIn = [&](uint32_t constant) {
            return constant ? constantBefore + constant : 0;
        };
        auto maskIn = [&](uint32_t mask) {
            return mask ? maskBefore + mask : 0;
        };
        auto nameIn = [&](NodeName name) {
            if (name.kind == NodeName::Kind::OfBody)
                name.owner += firstNode;
            return name;
        };
        auto nodeIn = [&](uint32_t node) {
            return NodeName::ofBody(firstNode + node);
        };

        for (const Body::Constant& constant : from.constants.span().subspan(1)) {
            into.constants.append(constant);
            for (ValueFlowObjectName& object : into.constants.last().objects) {
                if (object.kind == ValueFlowObjectKind::Site)
                    object.index += firstSite;
            }
        }
        into.masks.append(from.masks.span().subspan(1));
        for (const Body::Node& node : from.nodes) {
            bool isNamedByOthers = node.kind == ValueFlowBodyNodeKind::EntryParameter || node.kind == ValueFlowBodyNodeKind::Result || node.kind == ValueFlowBodyNodeKind::Thrown;
            into.nodes.append({ isNamedByOthers ? ValueFlowBodyNodeKind::Merge : node.kind, node.value, maskIn(node.mask) });
        }
        for (ValueFlowSite site : from.sites) {
            site.function = calleeID;
            into.sites.append(site);
        }
        for (const Body::Edge& edge : from.edges)
            into.edges.append({ nameIn(edge.from), nameIn(edge.to) });
        for (const Body::Edge& edge : from.edgesOnceReached)
            into.edgesOnceReached.append({ nameIn(edge.from), nameIn(edge.to) });
        for (const Body::Seed& seed : from.seedsOnceReached)
            into.seedsOnceReached.append({ nameIn(seed.node), constantIn(seed.constant), maskIn(seed.mask) });
        into.functionsEnteredOnceReached.appendVector(from.functionsEnteredOnceReached);
        for (const Body::Operand& operand : from.operands)
            into.operands.append({ nameIn(operand.node), constantIn(operand.constant), maskIn(operand.mask) });
        for (Body::Statement statement : from.statements) {
            statement.firstOperand += firstOperand;
            statement.result = nameIn(statement.result);
            statement.thrown = nameIn(statement.thrown);
            statement.function = calleeID;
            if (statement.firstSite != Body::none)
                statement.firstSite += firstSite;
            into.statements.append(statement);
        }
        into.numberOfCopiedStatements += from.statements.size();
        into.copies.append({ calleeID, firstNode, static_cast<uint32_t>(from.nodes.size()) });

        Body::Statement& call = into.statements[callIndex];
        call.copiedCallee = calleeID;
        const Value& operation = m_program.at(call.function ? call.function : into.function).at(call.site);
        Vector<Body::Operand, 8> operands(into.operandsOf(call));
        NodeName result = call.result;
        NodeName thrown = call.thrown;
        uint32_t undefined = constantOf(into, { Primitive::Undefined, { } });
        unsigned firstParameter = callee.firstParameterIndex();
        for (unsigned index = 0; index < from.numberOfEntryParameters; ++index) {
            switch (callee.entryParameterKind(index)) {
            case EntryParameter::Capture:
                into.edges.append({ NodeName::entryParameter(calleeID, index), nodeIn(index) });
                break;
            case EntryParameter::This:
                if (operation.operand(1))
                    connect(into, operands[1], nodeIn(index));
                else
                    into.seedsOnceReached.append({ nodeIn(index), undefined, 0 });
                break;
            case EntryParameter::Callee:
                into.seedsOnceReached.append({ nodeIn(index), constantOf(into, { 0, { ValueFlowObjectName { ValueFlowObjectKind::Closure, calleeID.index(), { } } } }), 0 });
                break;
            case EntryParameter::NewTarget:
                into.seedsOnceReached.append({ nodeIn(index), undefined, 0 });
                break;
            case EntryParameter::Parameter:
                if (2 + index - firstParameter < operands.size())
                    connect(into, operands[2 + index - firstParameter], nodeIn(index));
                else
                    into.seedsOnceReached.append({ nodeIn(index), undefined, 0 });
                break;
            case EntryParameter::ArgumentCount:
            case EntryParameter::HiddenRest:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
        if (result)
            into.edges.append({ nodeIn(from.result(0)), result });
        if (thrown)
            into.edges.append({ nodeIn(from.thrown()), thrown });

        forEachCopiedCall(calleeID.index(), [&](const KnownCall& inner) {
            appendCopy(into, firstStatement + inner.statement, inner.callee);
        }, &Description::isChosen);
    }

    const Program& m_program;
    BuildContext& m_build;
    ParallelArray<Body>& m_bodies;
    unsigned m_numberOfFunctions;
    Vector<Description> m_descriptions;
    BitVector m_isStoredTo;
};

} // namespace

void copyBodiesIntoCallSites(const Program& program, BuildContext& build, ParallelArray<ValueFlowBody>& bodies)
{
    BodyCopier(program, build, bodies).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
