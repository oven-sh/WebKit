/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowWidenedReport.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTThreshold.h"
#include "AOTValueFlowResultsInternal.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueFlowWidenedSolve.h"
#include <wtf/HashSet.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

ASCIILiteral wordFor(ValueFlowStatementKind kind)
{
    switch (kind) {
    case ValueFlowStatementKind::Operation:
        return "Operation"_s;
    case ValueFlowStatementKind::Lookup:
        return "Lookup"_s;
    case ValueFlowStatementKind::CallGroup:
        return "CallGroup"_s;
    case ValueFlowStatementKind::Behavior:
        return "Behavior"_s;
    case ValueFlowStatementKind::Escape:
        return "Escape"_s;
    case ValueFlowStatementKind::ModuleEvaluation:
        return "ModuleEvaluation"_s;
    case ValueFlowStatementKind::Coroutine:
        return "Coroutine"_s;
    case ValueFlowStatementKind::HostRoots:
        return "HostRoots"_s;
    case ValueFlowStatementKind::UsedBuiltins:
        return "UsedBuiltins"_s;
    case ValueFlowStatementKind::UnseenCode:
        return "UnseenCode"_s;
    case ValueFlowStatementKind::ModulesLoadedByName:
        return "ModulesLoadedByName"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class SolvesCompared {
public:
    SolvesCompared(ValueFlowSolution& solution, std::span<const UnseenCodeRecord> unseenCodeOfProgramAsItIs, BuildContext& build)
        : m_solution(solution)
        , m_asItIs(*solution.solverOfProgramAsItIs())
        , m_givenMore(solution.solver())
        , m_sets(solution.sets())
        , m_universe(solution.bodies().universe())
        , m_nodes(solution.bodies().nodes())
        , m_program(solution.program())
        , m_unseenCodeOfProgramAsItIs(unseenCodeOfProgramAsItIs)
        , m_build(build)
        , m_makers(solution.makerPool(), build.parallelLoops())
    {
    }

    void report()
    {
        findObjectsNeverMadeGivenMore();
        listNodesGivenMore();
        compareFunctions();
        compareUsesOfBuiltins();
        compareUnseenCode();
        compareMarks();
        compareNodes();
    }

private:
    AOT_CLASS_THRESHOLD(size_t, mostPlacesReportedOfOneSort, 12);
    AOT_CLASS_THRESHOLD(size_t, mostWaysFollowed, 400);
    AOT_CLASS_THRESHOLD(size_t, mostNodesGivenMoreListed, 12);

    struct NodeThatLacks {
        ValueFlowNodeID node;
        ValueSetID lacking;
    };

    void add(ASCIILiteral sort, const String& rule, Subject subject, const String& rest)
    {
        m_build.diagnostics().add(InternalError {
            .kind = InternalErrorKind::FlowFindsLessFromMore,
            .function = subject.function,
            .value = subject.value,
            .pass = PassID::AnalyzeFlow,
            .detail = makeString(sort, " | rule "_s, rule, " | "_s, rest, " | seed "_s, *m_givenMore.input().seedOfWidening, ", "_s, m_givenMore.numberOfNodesGivenMore(), " nodes given more"_s, m_nodesGivenMore),
        });
    }

    // The first nodes that the seed drew, each with the position of its value in the text of the program and with what it was given: from these a program can be written that brings the same.
    void listNodesGivenMore()
    {
        StringPrintStream out;
        size_t listed = 0;
        for (uint32_t index = 0; index < m_program.functions.size() && listed < mostNodesGivenMoreListed; ++index) {
            FunctionID function(index);
            if (!m_givenMore.isReached(function))
                continue;
            for (uint32_t node = m_nodes.firstNodeOf(function); node < m_nodes.endOfNodesOf(function) && listed < mostNodesGivenMoreListed; ++node) {
                ValueFlowSolver::GivenToNode given = m_givenMore.whatIsGivenTo(ValueFlowNodeID(node));
                if (given == ValueFlowSolver::GivenToNode::Nothing)
                    continue;
                out.print(listed++ ? ", " : ": ");
                printNodeGivenMore(out, ValueFlowNodeID(node));
                if (given == ValueFlowSolver::GivenToNode::WhatAnotherNodeOfBodyHolds) {
                    out.print(" what ");
                    printNodeGivenMore(out, m_givenMore.nodeWhoseValueIsGivenTo(ValueFlowNodeID(node)));
                    out.print(" holds");
                } else
                    out.print(given == ValueFlowSolver::GivenToNode::UnknownObject ? " the unknown object" : " a value nobody knows");
            }
        }
        m_nodesGivenMore = out.toString();
    }

    void printNodeGivenMore(PrintStream& out, ValueFlowNodeID node) const
    {
        printNode(out, node);
        Subject subject = m_givenMore.subjectOf(node);
        if (subject.function && subject.value && m_program.at(subject.function).at(subject.value).hasPosition())
            out.print("@", m_program.at(subject.function).at(subject.value).position);
    }

    void printMember(PrintStream& out, ValueFlowObjectID member) const
    {
        if (member == ValueFlowUniverse::unknownObject())
            out.print("the unknown object");
        else if (member == ValueFlowUniverse::untrackedObject())
            out.print("the untracked object");
        else if (PropertyKeyID key = m_universe.keyOf(member)) {
            out.print("key", key.index());
            if (StringID text = m_program.at(key).text)
                out.print(" \"", String::fromUTF8(m_program.string(text)), "\"");
        } else if (!m_universe.isObject(member))
            out.print("primitive", member.index());
        else if (FunctionID function = m_universe.closureFunction(member))
            out.print("closure f", function.index());
        else if (BuiltinID builtin = m_universe.builtinOf(member))
            out.print(m_program.builtins().tokenOf(builtin));
        else if (FunctionID function = m_universe.functionOf(member))
            out.print("object", member.index(), "@f", function.index(), " kind ", static_cast<unsigned>(m_universe.kindOf(member)));
        else
            out.print("object", member.index(), " kind ", static_cast<unsigned>(m_universe.kindOf(member)));
    }

    void printMembers(PrintStream& out, ValueSetID set) const
    {
        constexpr unsigned mostMembersPrinted = 4;
        if (ValueSets::isComplement(set)) {
            out.print("all but ", m_sets.size(ValueSets::complement(set)));
            return;
        }
        unsigned printed = 0;
        m_sets.forEachMember(set, [&](ValueFlowObjectID member) {
            if (++printed > mostMembersPrinted)
                return;
            if (printed > 1)
                out.print(", ");
            printMember(out, member);
        });
        if (printed > mostMembersPrinted)
            out.print(", .. (", printed, ")");
    }

    // Without a number of the program, so that one rule has one name in every program.
    void printRuleOfKey(PrintStream& out, const ValueFlowNodeKey& key) const
    {
        if (key.kind != ValueFlowMadeNodeKind::CallGroup) {
            out.print(":", key.detail);
            return;
        }
        out.print(":", key.detail, "(");
        if (!key.set || ValueSets::isComplement(key.set) || m_sets.size(key.set) > 2)
            out.print("many");
        else {
            bool isFirst = true;
            m_sets.forEachMember(key.set, [&](ValueFlowObjectID callee) {
                if (!std::exchange(isFirst, false))
                    out.print(",");
                BuiltinID builtin = m_universe.isObject(callee) && !m_universe.closureFunction(callee) ? m_universe.builtinOf(callee) : BuiltinID();
                if (builtin)
                    out.print(m_program.builtins().tokenOf(builtin));
                else if (callee == ValueFlowUniverse::unknownObject())
                    out.print("unknown");
                else if (callee == ValueFlowUniverse::untrackedObject())
                    out.print("untracked");
                else
                    out.print(m_universe.closureFunction(callee) ? "closure" : "object");
            });
        }
        out.print(")");
    }

    void printRule(PrintStream& out, ValueFlowStatementID id) const
    {
        const ValueFlowStatement& statement = m_asItIs.statementAt(id);
        if (statement.kind == ValueFlowStatementKind::Operation) {
            out.print(statement.opcode);
            return;
        }
        out.print(wordFor(statement.kind));
        if (m_asItIs.isMadeByRule(id) && statement.kind != ValueFlowStatementKind::Escape)
            printRuleOfKey(out, m_asItIs.keyOf(id));
    }

    void printKey(PrintStream& out, const ValueFlowNodeKey& key) const
    {
        out.print("kind ", static_cast<unsigned>(key.kind), " detail ", key.detail, " payload ", key.payload);
        bool hasName = key.kind == ValueFlowMadeNodeKind::Lookup || key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::Written;
        if (hasName && key.name().kind == ValueFlowNameKind::Property) {
            if (StringID text = m_program.at(PropertyKeyID(key.payload)).text)
                out.print(" \"", String::fromUTF8(m_program.string(text)), "\"");
        }
        if (key.set) {
            out.print(" of {");
            printMembers(out, key.set);
            out.print("}");
        }
    }

    void printStatement(PrintStream& out, ValueFlowStatementID id) const
    {
        const ValueFlowStatement& statement = m_asItIs.statementAt(id);
        if (statement.kind == ValueFlowStatementKind::Operation) {
            out.print(statement.opcode, " f", statement.function.index(), ":v", statement.site.index());
            return;
        }
        out.print(wordFor(statement.kind));
        if (statement.function)
            out.print(" f", statement.function.index());
        if (m_asItIs.isMadeByRule(id) && statement.kind != ValueFlowStatementKind::Escape) {
            out.print(" ");
            printKey(out, m_asItIs.keyOf(id));
        }
    }

    void printNode(PrintStream& out, ValueFlowNodeID node) const
    {
        if (const ValueFlowNodeKey* key = m_asItIs.keyOfMadeNode(node)) {
            out.print("a node made: ");
            printKey(out, *key);
            return;
        }
        if (node == ValueFlowProgramNodes::escaped()) {
            out.print("escaped");
            return;
        }
        if (node == ValueFlowProgramNodes::lost()) {
            out.print("lost");
            return;
        }
        uint32_t firstOfHost = m_nodes.ofHost(static_cast<ValueFlowHostNode>(0)).index();
        if (node.index() >= firstOfHost) {
            out.print("host node ", node.index() - firstOfHost);
            return;
        }
        Subject subject = m_asItIs.subjectOf(node);
        if (!subject.function) {
            out.print("a variable or field, node ", node.index());
            return;
        }
        out.print("f", subject.function.index());
        if (subject.value)
            out.print(":v", subject.value.index());
        else
            out.print(" +", node.index() - m_nodes.firstNodeOf(subject.function));
    }

    // Where a callee may be anything, the call makes an object nobody knows and not the object of its site. An object of the program that no node of the wider solve holds is one of those that the unknown object stands for there.
    void findObjectsNeverMadeGivenMore()
    {
        m_neverMadeGivenMore = objectsOfProgramThatNoNodeHolds(m_givenMore, m_universe, m_makers.of(0));
    }

    ValueSetID allThatStandsFor(ValueSetID givenMore, ValueSetMaker& maker) const
    {
        ValueSetID all = m_givenMore.withWhatMembersCover(givenMore, maker);
        return m_sets.contains(all, ValueFlowUniverse::unknownObject()) ? maker.unite(all, m_neverMadeGivenMore) : all;
    }
    ValueSetID allThatNodeStandsForGivenMore(ValueFlowNodeID node, ValueSetMaker& maker) const { return allThatStandsFor(m_givenMore.possibleValuesOf(node), maker); }

    std::optional<ValueFlowNodeID> sameNodeGivenMore(ValueFlowNodeID node) const
    {
        const ValueFlowNodeKey* key = m_asItIs.keyOfMadeNode(node);
        if (!key)
            return node;
        bool namesNode = key->kind == ValueFlowMadeNodeKind::Masked || key->isInstanceOfNode();
        if (namesNode && key->payload >= m_asItIs.input().numberOfNodes)
            return std::nullopt;
        std::optional<ValueFlowNodeID> first = m_asItIs.findMadeNode(*key);
        std::optional<ValueFlowNodeID> firstGivenMore = m_givenMore.findMadeNode(*key);
        if (!first || !firstGivenMore)
            return std::nullopt;
        return ValueFlowNodeID(firstGivenMore->index() + node.index() - first->index());
    }

    void compareFunctions()
    {
        size_t reported = 0;
        for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
            FunctionID function(index);
            if (!m_asItIs.isReached(function) || m_givenMore.isReached(function))
                continue;
            ValueFlowReachWitness witness = m_asItIs.reachedBy(function);
            FunctionID from = witness.statement ? m_asItIs.statementAt(witness.statement).function : witness.enteringBody < m_program.functions.size() ? FunctionID(witness.enteringBody) : FunctionID();
            if (from && !m_givenMore.isReached(from))
                continue;
            if (++reported > mostPlacesReportedOfOneSort)
                continue;
            StringPrintStream rule;
            StringPrintStream rest;
            rest.print("f", index, " is reached only from less");
            if (witness.statement) {
                printRule(rule, witness.statement);
                rest.print("; reached by ");
                printStatement(rest, witness.statement);
            } else
                rule.print("entered with a body");
            add("function"_s, rule.toString(), Subject { .function = function }, rest.toString());
        }
    }

    void compareUsesOfBuiltins()
    {
        size_t reported = 0;
        const BuiltinTable& builtins = m_program.builtins();
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            OptionSet<BuiltinUse> lacking = m_asItIs.usesOf(BuiltinID(index)) - m_givenMore.usesOf(BuiltinID(index));
            if (lacking.isEmpty() || ++reported > mostPlacesReportedOfOneSort)
                continue;
            add("use"_s, makeString("use of "_s, builtins.tokenOf(BuiltinID(index))), { }, makeString("uses "_s, lacking.toRaw(), " of "_s, builtins.tokenOf(BuiltinID(index)), " are found only from less"_s));
        }
    }

    void compareUnseenCode()
    {
        size_t reported = 0;
        Vector<UnseenCodeRecord> givenMore = unseenCodeRecordsOf(m_solution);
        for (const UnseenCodeRecord& record : m_unseenCodeOfProgramAsItIs) {
            bool isFound = std::ranges::any_of(givenMore, [&](const UnseenCodeRecord& other) {
                return other.cause == record.cause && other.builtin == record.builtin;
            });
            if (isFound || ++reported > mostPlacesReportedOfOneSort)
                continue;
            String builtin = record.builtin != Wire::none ? m_program.builtins().tokenOf(BuiltinID(record.builtin)) : "no built-in"_str;
            Subject subject;
            if (record.witness.function != Wire::none)
                subject.function = FunctionID(record.witness.function);
            if (record.witness.value != Wire::none)
                subject.value = ValueID(record.witness.value);
            add("unseen code"_s, makeString(nameOf(record.cause), ' ', builtin), subject, "the record is made only from less"_str);
        }
    }

    void compareMarks()
    {
        size_t reported = 0;
        ValueSetMaker& maker = m_makers.of(0);
        ValueSetID outOfSight = maker.unite(m_neverMadeGivenMore, maker.unite(m_givenMore.possibleValuesOf(ValueFlowProgramNodes::escaped()), m_givenMore.possibleValuesOf(ValueFlowProgramNodes::lost())));
        for (uint32_t index = 0; index < m_universe.numberOfObjects(); ++index) {
            ValueFlowObjectID object(index);
            OptionSet<ObjectMutation> lacking = m_asItIs.marksOf(object) - m_givenMore.marksOf(object);
            if (lacking.isEmpty() || m_sets.contains(outOfSight, object) || ++reported > mostPlacesReportedOfOneSort)
                continue;
            StringPrintStream rest;
            printMember(rest, object);
            rest.print(" has marks ", lacking.toRaw(), " only from less");
            add("mark"_s, makeString("marks "_s, lacking.toRaw()), Subject { .function = m_universe.isObject(object) ? m_universe.functionOf(object) : FunctionID() }, rest.toString());
        }
    }

    bool comesFromNodeThatLacksIt(ValueFlowNodeID node, ValueFlowObjectID member, ValueSetMaker& maker) const
    {
        bool result = false;
        m_asItIs.graph().forEachPredecessor(node, [&](ValueFlowNodeID from) {
            if (result || from.index() >= m_asItIs.input().numberOfNodes || !m_sets.contains(m_asItIs.possibleValuesOf(from), member))
                return;
            result = !m_sets.contains(allThatNodeStandsForGivenMore(from, maker), member);
        });
        return result;
    }

    void compareNodes()
    {
        constexpr uint32_t nodesPerShare = 1 << 12;
        uint32_t numberOfNodes = m_asItIs.input().numberOfNodes;
        Vector<Vector<NodeThatLacks>> foundInShare((numberOfNodes + nodesPerShare - 1) / nodesPerShare);
        forEachIndexInParallel(m_build.parallelLoops(), foundInShare.size(), [&](unsigned share, unsigned worker) {
            ValueSetMaker& maker = m_makers.of(worker);
            for (uint32_t index = share * nodesPerShare; index < std::min(numberOfNodes, (share + 1) * nodesPerShare); ++index) {
                ValueFlowNodeID node(index);
                ValueSetID asItIs = m_asItIs.possibleValuesOf(node);
                ValueSetID givenMore = m_givenMore.possibleValuesOf(node);
                if (asItIs == givenMore || m_sets.isSubset(asItIs, givenMore))
                    continue;
                ValueSetID lacking = maker.intersect(asItIs, ValueSets::complement(allThatStandsFor(givenMore, maker)));
                if (lacking != ValueSets::nothing)
                    foundInShare[share].append(NodeThatLacks { node, lacking });
            }
        });

        ValueSetMaker& maker = m_makers.of(0);
        size_t numberOfNodesThatLack = 0;
        for (const Vector<NodeThatLacks>& found : foundInShare)
            numberOfNodesThatLack += found.size();
        size_t waysFollowed = 0;
        size_t reported = 0;
        HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> firstPlacesSeen;
        for (const Vector<NodeThatLacks>& found : foundInShare) {
            for (const NodeThatLacks& entry : found) {
                if (reported >= mostPlacesReportedOfOneSort || waysFollowed >= mostWaysFollowed)
                    return;
                ValueFlowObjectID member;
                m_sets.forEachMember(entry.lacking, [&](ValueFlowObjectID candidate) {
                    if (!member && !comesFromNodeThatLacksIt(entry.node, candidate, maker))
                        member = candidate;
                });
                if (!member)
                    continue;
                ++waysFollowed;
                reported += reportFirstPlaceThatLacks(entry.node, member, numberOfNodesThatLack, firstPlacesSeen, maker);
            }
        }
    }

    bool reportFirstPlaceThatLacks(ValueFlowNodeID node, ValueFlowObjectID member, size_t numberOfNodesThatLack, auto& firstPlacesSeen, ValueSetMaker& maker)
    {
        // From the node back to where the member was made.
        Vector<ValueFlowNodeID> way = m_asItIs.explain(node, member);
        size_t lacks = 0;
        size_t holds = way.size();
        for (size_t index = way.size(); index--;) {
            std::optional<ValueFlowNodeID> same = sameNodeGivenMore(way[index]);
            if (!same)
                continue;
            if (!m_sets.contains(allThatNodeStandsForGivenMore(*same, maker), member)) {
                lacks = index;
                break;
            }
            holds = index;
        }
        ValueFlowNodeID firstThatLacks = way.isEmpty() ? node : way[lacks];
        if (!firstPlacesSeen.add(static_cast<uint64_t>(firstThatLacks.index()) << 32 | member.index()).isNewEntry)
            return false;

        StringPrintStream rule;
        StringPrintStream rest;
        printNode(rest, firstThatLacks);
        rest.print(" lacks ");
        printMember(rest, member);
        if (std::optional<ValueFlowNodeID> same = sameNodeGivenMore(firstThatLacks)) {
            rest.print(" and holds {");
            printMembers(rest, maker.intersect(m_givenMore.possibleValuesOf(*same), m_universe.objects()));
            rest.print("}");
        }
        Subject subject = m_asItIs.subjectOf(firstThatLacks);
        if (holds < way.size()) {
            rest.print("; ");
            printNode(rest, way[holds]);
            rest.print(" holds it");
        }
        bool hasStatement = false;
        for (size_t index = std::min(holds, way.isEmpty() ? 0 : way.size() - 1); index-- > lacks;) {
            ValueFlowStatementID statement = m_asItIs.statementThatLogged(ValueFlowEdge { way[index + 1], way[index] });
            if (!statement)
                continue;
            rule.print(hasStatement ? ">" : "");
            printRule(rule, statement);
            rest.print(hasStatement ? ", " : "; on by ");
            printStatement(rest, statement);
            hasStatement = true;
            const ValueFlowStatement& found = m_asItIs.statementAt(statement);
            if (found.kind == ValueFlowStatementKind::Operation)
                subject = Subject { .function = found.function, .value = found.site };
        }
        if (!hasStatement)
            rule.print("an edge of the bodies");
        rest.print("; ", numberOfNodesThatLack, " nodes lack something");
        add("node"_s, rule.toString(), subject, rest.toString());
        return true;
    }

    ValueFlowSolution& m_solution;
    const ValueFlowSolver& m_asItIs;
    const ValueFlowSolver& m_givenMore;
    const ValueSets& m_sets;
    const ValueFlowUniverse& m_universe;
    const ValueFlowProgramNodes& m_nodes;
    const Program& m_program;
    std::span<const UnseenCodeRecord> m_unseenCodeOfProgramAsItIs;
    BuildContext& m_build;
    ValueSetMakers m_makers;
    ValueSetID m_neverMadeGivenMore { ValueSets::nothing };
    String m_nodesGivenMore;
};

} // anonymous namespace

void reportWhatIsFoundOnlyFromLessInWords(ValueFlowSolution& solution, std::span<const UnseenCodeRecord> unseenCodeOfProgramAsItIs, BuildContext& build)
{
    SolvesCompared(solution, unseenCodeOfProgramAsItIs, build).report();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
