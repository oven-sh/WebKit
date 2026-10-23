/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResults.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableWords.h"
#include "AOTFlowRecords.h"
#include "AOTIRPrinter.h"
#include "AOTVector.h"
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static ASCIILiteral nameOf(FlowIndirectCallKind kind)
{
    switch (kind) {
    case FlowIndirectCallKind::Accessor:
        return "Accessor"_s;
    case FlowIndirectCallKind::BuiltinCallback:
        return "BuiltinCallback"_s;
    case FlowIndirectCallKind::Protocol:
        return "Protocol"_s;
    case FlowIndirectCallKind::Host:
        return "Host"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(LegacyRegExpStatics statics)
{
    switch (statics) {
    case LegacyRegExpStatics::Unknown:
        return "Unknown"_s;
    case LegacyRegExpStatics::NeverRead:
        return "NeverRead"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(ProgramInstances instances)
{
    switch (instances) {
    case ProgramInstances::Unknown:
        return "Unknown"_s;
    case ProgramInstances::AtMostOne:
        return "AtMostOne"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

class FlowResultsDumper {
    WTF_MAKE_NONCOPYABLE(FlowResultsDumper);
public:
    FlowResultsDumper(PrintStream& out, const Program& program, const FlowResultTables& tables)
        : m_out(out)
        , m_tables(tables)
        , m_printer(out, program)
    {
    }

    void run()
    {
        dumpFunctions();
        dumpBoundFunctionUses();
        dumpModules();
        dumpRecords("class"_s, allocationClassLetter, m_tables.sites.allocationClasses);
        dumpRecords("site"_s, allocationSiteLetter, m_tables.sites.allocationSites);
        dumpAccessSites();
        dumpIndirectCalls();
        dumpBuiltinLocations();
        dumpBuiltinUses();
        dumpFeatureUses();
        dumpLocaleUses();
        dumpUnseenCode();
        dumpSinkOperands();
        dumpTypedResults();
        dumpDataNodes();
        dumpRegExpResults();
        dumpStoredValueUses();
        dumpProgramInstances();
        dumpKeysStoredThroughUnknownReceivers();
    }

private:
    static constexpr char accessSiteLetter = 'S';
    static constexpr char allocationSiteLetter = 'A';
    static constexpr char allocationClassLetter = 'C';
    static constexpr OperandUses everyOperandUse = everyFlagOf<OperandUse>();

    static bool isUnknown(OperandUses uses) { return uses.containsAll(everyOperandUse); }
    static bool isUnknown(const OperandSummary& summary) { return isUnknown(summary.uses) && summary.retainedIn == RetainedIn::UnknownMemory; }
    static ASCIILiteral wordOf(BuiltinCallbackTimes times) { return std::span(builtinCallbackTimesWords)[static_cast<size_t>(times)]; }

    void beginLine(ASCIILiteral keyword)
    {
        m_printer.beginLine(keyword);
        m_out.print(" "_s);
    }

    template<typename Tag>
    void printWithLetter(char letter, ID<Tag> id)
    {
        if (!id) {
            m_out.print("-"_s);
            return;
        }
        m_out.print(letter, id.index());
    }

    void printAccessSite(AccessSiteID site) { printWithLetter(accessSiteLetter, site); }

    void printUses(OperandUses uses)
    {
        m_printer.field("uses"_s, uses & everyOperandUse);
    }

    void printWitness(const Witness& witness)
    {
        m_printer.print(FunctionID(witness.function));
        if (witness.value == Wire::none)
            return;
        m_out.print(":"_s);
        m_printer.print(ValueID(witness.value));
    }

    void printOptionalWitness(const Witness& witness)
    {
        if (!hasFunction(witness))
            return;
        m_out.print(" "_s);
        printWitness(witness);
    }

    void printOptionalBuiltin(uint32_t index)
    {
        BuiltinID builtin(index);
        if (!builtin)
            return;
        m_out.print(" "_s);
        m_printer.print(builtin);
    }

    template<typename Record>
    void dumpRecords(ASCIILiteral keyword, char letter, const Vector<Record>& records)
    {
        for (uint32_t index = 0; index < records.size(); ++index) {
            if (records[index] == Record { })
                continue;
            beginLine(keyword);
            m_out.print(letter, index);
            printFields(m_printer, records[index]);
            m_printer.endLine();
        }
    }

    void dumpParameterSummaries(uint32_t function, const Vector<OperandSummary>& summaries)
    {
        for (uint32_t parameter = 0; parameter < summaries.size(); ++parameter) {
            const OperandSummary& summary = summaries[parameter];
            if (isUnknown(summary))
                continue;
            m_printer.beginLine("function"_s, IDPrefix::Function, function);
            m_out.print(" parameter "_s, parameter);
            printUses(summary.uses);
            m_printer.field("retainedIn"_s, summary.retainedIn);
            if (summary.retainedIn == RetainedIn::Operand)
                m_printer.field("retainingOperand"_s, summary.retainingOperand);
            m_printer.endLine();
        }
    }

    void dumpFunctions()
    {
        const FlowFunctionTables& tables = m_tables.functions;
        for (uint32_t index = 0; index < tables.functions.size(); ++index) {
            if (tables.functions[index] != FunctionFlow { }) {
                m_printer.beginLine("function"_s, IDPrefix::Function, index);
                printFields(m_printer, tables.functions[index]);
                m_printer.endLine();
            }
            dumpParameterSummaries(index, tables.parameterSummaries[index]);
        }
    }

    void dumpBoundFunctionUses()
    {
        for (uint32_t index = 0; index < m_tables.functions.functions.size(); ++index) {
            if (m_tables.functions.functions[index].closureUse != ClosureUse::CalledOnly || m_tables.functions.functionsWhoseClosuresAreNeverCompared.get(index))
                continue;
            beginLine("closures-compared"_s);
            m_printer.print(FunctionID(index));
            m_printer.endLine();
        }
        for (const FlowBoundFunctionUse& use : m_tables.functions.boundFunctionUses) {
            if (use.closureUse == ClosureUse::Unknown)
                continue;
            beginLine("bound-function"_s);
            m_printer.print(use.target);
            m_out.print(" "_s, use.numberOfBoundArguments);
            m_printer.field("closureUse"_s, use.closureUse);
            m_printer.endLine();
        }
    }

    void dumpModules()
    {
        const Vector<ModuleFlow>& modules = m_tables.wholeProgram.modules;
        for (uint32_t index = 0; index < modules.size(); ++index) {
            if (modules[index] == ModuleFlow { })
                continue;
            m_printer.beginLine("module"_s, IDPrefix::Module, index);
            printFields(m_printer, modules[index]);
            m_printer.endLine();
        }
    }

    void printLoss(const FlowSiteLoss& loss)
    {
        if (loss == FlowSiteLoss { })
            return;
        m_out.print(" loss"_s);
        if (loss.cause != UnprovenCause::Unknown)
            m_printer.field("cause"_s, loss.cause);
        if (loss.limit != LimitID::None)
            m_printer.field("limit"_s, loss.limit);
        if (hasFunction(loss.witness)) {
            m_printer.beginField("witness"_s);
            printWitness(loss.witness);
        }
    }

    void dumpAccessSites()
    {
        const FlowSiteTables& tables = m_tables.sites;
        for (uint32_t index = 0; index < tables.accessSites.size(); ++index) {
            const AccessSite& site = tables.accessSites[index];
            FlowSiteLoss loss = index < tables.accessSiteLosses.size() ? tables.accessSiteLosses[index] : FlowSiteLoss { };
            if (site == AccessSite { } && loss == FlowSiteLoss { })
                continue;
            beginLine("access"_s);
            printAccessSite(AccessSiteID(index));
            printFields(m_printer, site);
            printLoss(loss);
            if (index < tables.callsOfBoundFunctions.size() && tables.callsOfBoundFunctions[index])
                m_printer.field("mayCallBoundFunction"_s, true);
            m_printer.endLine();
        }
    }

    void dumpIndirectCalls()
    {
        for (const FlowIndirectCall& call : m_tables.functions.indirectCalls) {
            beginLine("indirect"_s);
            m_printer.print(call.caller);
            m_out.print(" "_s);
            printAccessSite(call.site);
            m_out.print(" -> "_s);
            m_printer.print(call.callee);
            m_out.print(" "_s, nameOf(call.kind), " "_s, wordOf(call.times));
            if (call.isInCycle)
                m_out.print(" cycle"_s);
            m_printer.endLine();
        }
    }

    void dumpBuiltinLocations()
    {
        for (const BuiltinLocationFlow& location : m_tables.builtins.locations) {
            beginLine("builtin-location"_s);
            m_printer.print(location.holder);
            m_out.print("."_s);
            m_printer.print(location.key);
            m_out.print(" "_s);
            m_printer.print(location.state);
            if (hasFunction(location.witness)) {
                m_out.print(" written at "_s);
                printWitness(location.witness);
            }
            m_printer.endLine();
        }
    }

    void dumpBuiltinUses()
    {
        for (const BuiltinUseRecord& use : m_tables.builtins.uses) {
            beginLine("builtin-use"_s);
            m_printer.print(BuiltinID(use.builtin));
            m_out.print(" "_s);
            m_printer.print(OptionSet<BuiltinUse>::fromRaw(static_cast<uint32_t>(use.uses)));
            printOptionalWitness(use.witness);
            m_printer.endLine();
        }
    }

    void dumpFeatureUses()
    {
        for (const FeatureUseRecord& use : m_tables.builtins.featureUses) {
            beginLine("feature"_s);
            m_out.print(use.feature);
            printOptionalBuiltin(use.builtin);
            if (use.kind != FeatureUseKind::Named)
                m_out.print(" "_s, nameOf(use.kind));
            printOptionalWitness(use.witness);
            m_printer.endLine();
        }
    }

    void dumpLocaleUses()
    {
        for (const FlowLocaleUse& use : m_tables.builtins.localeUses) {
            beginLine("locale"_s);
            m_out.print(nameOf(use.kind));
            if (use.tag) {
                m_out.print(" "_s);
                m_printer.print(use.tag);
            }
            printOptionalWitness(use.witness);
            m_printer.endLine();
        }
    }

    void dumpUnseenCode()
    {
        for (const UnseenCodeRecord& record : m_tables.wholeProgram.unseenCode) {
            beginLine("unseen-code"_s);
            m_printer.print(record.cause);
            printOptionalWitness(record.witness);
            printOptionalBuiltin(record.builtin);
            m_printer.endLine();
        }
    }

    void dumpSinkOperands()
    {
        for (const FlowSinkOperand& operand : m_tables.sites.sinkOperands) {
            const SinkCandidates& candidates = operand.candidates;
            if (candidates == SinkCandidates { })
                continue;
            beginLine("sink"_s);
            printAccessSite(operand.call);
            m_out.print(" operand "_s, operand.operandIndex, " "_s);
            m_printer.list(candidates.strings, [&](StringID string) {
                m_printer.print(string);
            });
            if (candidates.mayBeUndefined)
                m_out.print(" mayBeUndefined"_s);
            if (candidates.isComplete)
                m_out.print(" complete"_s);
            m_printer.endLine();
        }
    }

    void dumpTypedResults()
    {
        const FlowSiteTables& tables = m_tables.sites;
        for (const FlowTypedResultCall& call : tables.typedResultCalls) {
            beginLine("typed-result"_s);
            printAccessSite(call.call);
            m_out.print(" "_s);
            m_printer.print(call.type);
            m_printer.endLine();
        }

        for (uint32_t layout = 0; layout < tables.typedResultSites.size(); ++layout) {
            AllocationSiteID site = tables.typedResultSites[layout];
            if (!site)
                continue;
            beginLine("typed-result-site"_s);
            m_printer.print(LayoutID(layout));
            m_out.print(" "_s);
            printWithLetter(allocationSiteLetter, site);
            m_printer.endLine();
        }
    }

    void dumpDataNodes()
    {
        for (const FlowDataNode& node : m_tables.sites.dataNodes) {
            beginLine("data-node"_s);
            printWithLetter(allocationSiteLetter, node.rootSite);
            m_out.print(" "_s, node.ordinal, " "_s);
            printWithLetter(allocationSiteLetter, node.stamp.site);
            m_out.print(" "_s);
            printWithLetter(allocationClassLetter, node.stamp.allocationClass);
            if (node.stamp.elements) {
                m_out.print(" elements="_s);
                m_printer.print(node.stamp.elements);
            }
            if (node.stamp.type) {
                m_out.print(" type="_s);
                m_printer.print(node.stamp.type);
            }
            m_printer.endLine();
        }
    }

    void dumpRegExpResults()
    {
        LegacyRegExpStatics statics = m_tables.builtins.legacyRegExpStatics;
        if (statics == LegacyRegExpStatics::Unknown)
            return;
        beginLine("legacy-regexp-statics"_s);
        m_out.print(nameOf(statics));
        m_printer.endLine();
    }

    void printFurtherUsesOfFramePointer(uint32_t layout, uint32_t field, OperandUses uses)
    {
        const Vector<Vector<OperandUses>>& usesByLayout = m_tables.functions.storedFramePointerUses;
        if (layout >= usesByLayout.size() || field >= usesByLayout[layout].size() || usesByLayout[layout][field] == uses)
            return;
        m_printer.field("asFramePointer"_s, (usesByLayout[layout][field] - uses) & everyOperandUse);
    }

    void dumpStoredValueUses()
    {
        const Vector<Vector<OperandUses>>& usesByLayout = m_tables.functions.storedValueUses;
        for (uint32_t layout = 0; layout < usesByLayout.size(); ++layout) {
            const Vector<OperandUses>& usesByField = usesByLayout[layout];
            for (uint32_t field = 0; field < usesByField.size(); ++field) {
                if (isUnknown(usesByField[field]))
                    continue;
                beginLine("stored-value"_s);
                m_printer.print(LayoutID(layout));
                m_out.print("."_s, field);
                printUses(usesByField[field]);
                printFurtherUsesOfFramePointer(layout, field, usesByField[field]);
                m_printer.endLine();
            }
        }
    }

    void dumpProgramInstances()
    {
        ProgramInstances instances = m_tables.wholeProgram.instances;
        if (instances == ProgramInstances::Unknown)
            return;
        beginLine("program instances"_s);
        m_out.print(nameOf(instances));
        m_printer.endLine();
    }

    void dumpKeysStoredThroughUnknownReceivers()
    {
        const Vector<PropertyKeyID>& keys = m_tables.wholeProgram.keysStoredThroughUnknownReceivers;
        if (keys.isEmpty())
            return;
        beginLine("stored-through-unknown-receivers"_s);
        m_printer.list(keys, [&](PropertyKeyID key) {
            m_printer.print(key);
        });
        m_printer.endLine();
    }

    PrintStream& m_out;
    const FlowResultTables& m_tables;
    IRPrinter m_printer;
};

} // anonymous namespace

void FlowResults::dump(PrintStream& out, const Program& program) const
{
    FlowResultsDumper(out, program, m_tables).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
