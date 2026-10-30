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

void FlowResults::dump(PrintStream& out, const Program& program) const
{
    constexpr char accessSiteLetter = 'S';
    constexpr char allocationSiteLetter = 'A';
    constexpr char allocationClassLetter = 'C';
    constexpr OperandUses everyOperandUse = everyFlagOf<OperandUse>();
    const FlowFunctionTables& functions = m_tables.functions;
    const FlowSiteTables& sites = m_tables.sites;
    const auto& builtins = m_tables.builtins;
    const auto& wholeProgram = m_tables.wholeProgram;
    IRPrinter printer(out, program);

    auto beginLine = [&](ASCIILiteral keyword) {
        printer.beginLine(keyword);
        out.print(" "_s);
    };
    auto print = [&](const auto& value) {
        out.print(" "_s);
        printer.print(value);
    };
    auto printWithLetter = [&](char letter, auto id) {
        if (id)
            out.print(letter, id.index());
        else
            out.print("-"_s);
    };
    auto printWitness = [&](const Witness& witness) {
        printer.print(FunctionID(witness.function));
        if (witness.value == Wire::none)
            return;
        out.print(":"_s);
        printer.print(ValueID(witness.value));
    };
    auto printOptionalWitness = [&](const Witness& witness) {
        if (!hasFunction(witness))
            return;
        out.print(" "_s);
        printWitness(witness);
    };
    auto printOptionalBuiltin = [&](uint32_t index) {
        if (BuiltinID builtin { index })
            print(builtin);
    };
    auto dumpRecords = [&](ASCIILiteral keyword, char letter, const auto& records) {
        for (uint32_t index = 0; index < records.size(); ++index) {
            if (records[index] == std::decay_t<decltype(records[index])> { })
                continue;
            beginLine(keyword);
            out.print(letter, index);
            printFields(printer, records[index]);
            printer.endLine();
        }
    };
    auto dumpKeys = [&](ASCIILiteral keyword, const Vector<PropertyKeyID>& keys) {
        if (keys.isEmpty())
            return;
        beginLine(keyword);
        printer.list(keys, [&](PropertyKeyID key) {
            printer.print(key);
        });
        printer.endLine();
    };

    for (uint32_t index = 0; index < functions.functions.size(); ++index) {
        if (functions.functions[index] != FunctionFlow { }) {
            printer.beginLine("function"_s, IDPrefix::Function, index);
            printFields(printer, functions.functions[index]);
            printer.endLine();
        }
        const Vector<OperandSummary>& summaries = functions.parameterSummaries[index];
        for (uint32_t parameter = 0; parameter < summaries.size(); ++parameter) {
            const OperandSummary& summary = summaries[parameter];
            if (summary.uses.containsAll(everyOperandUse) && summary.retainedIn == RetainedIn::UnknownMemory)
                continue;
            printer.beginLine("function"_s, IDPrefix::Function, index);
            out.print(" parameter "_s, parameter);
            printer.field("uses"_s, summary.uses & everyOperandUse);
            printer.field("retainedIn"_s, summary.retainedIn);
            if (summary.retainedIn == RetainedIn::Operand)
                printer.field("retainingOperand"_s, summary.retainingOperand);
            printer.endLine();
        }
    }

    for (uint32_t index = 0; index < functions.functions.size(); ++index) {
        if (functions.functions[index].closureUse != ClosureUse::CalledOnly || functions.functionsWhoseClosuresAreNeverCompared.get(index))
            continue;
        beginLine("closures-compared"_s);
        printer.print(FunctionID(index));
        printer.endLine();
    }
    for (const FlowBoundFunctionUse& use : functions.boundFunctionUses) {
        if (use.closureUse == ClosureUse::Unknown)
            continue;
        beginLine("bound-function"_s);
        printer.print(use.target);
        out.print(" "_s, use.numberOfBoundArguments);
        printer.field("closureUse"_s, use.closureUse);
        printer.endLine();
    }
    for (const FlowBoundFunctionAtSite& bound : sites.boundFunctionsAtSites) {
        beginLine(bound.use == FlowBoundFunctionUseAtSite::Made ? "bound-function-made-at"_s : "bound-function-called-at"_s);
        printWithLetter(accessSiteLetter, bound.site);
        out.print(" "_s);
        printer.print(bound.target);
        out.print(" "_s, bound.numberOfBoundArguments);
        printer.endLine();
    }

    for (uint32_t index = 0; index < wholeProgram.modules.size(); ++index) {
        if (wholeProgram.modules[index] == ModuleFlow { })
            continue;
        printer.beginLine("module"_s, IDPrefix::Module, index);
        printFields(printer, wholeProgram.modules[index]);
        printer.endLine();
    }

    dumpRecords("class"_s, allocationClassLetter, sites.allocationClasses);
    dumpRecords("site"_s, allocationSiteLetter, sites.allocationSites);
    for (size_t index : sites.sitesOfListsNeverToldApart) {
        beginLine("never-told-apart"_s);
        printWithLetter(allocationSiteLetter, AllocationSiteID(static_cast<uint32_t>(index)));
        printer.endLine();
    }

    for (uint32_t index = 0; index < sites.accessSites.size(); ++index) {
        const AccessSite& site = sites.accessSites[index];
        FlowSiteLoss loss = index < sites.accessSiteLosses.size() ? sites.accessSiteLosses[index] : FlowSiteLoss { };
        if (site == AccessSite { } && loss == FlowSiteLoss { })
            continue;
        beginLine("access"_s);
        printWithLetter(accessSiteLetter, AccessSiteID(index));
        printFields(printer, site);
        if (loss != FlowSiteLoss { }) {
            out.print(" loss"_s);
            if (loss.cause != UnprovenCause::Unknown)
                printer.field("cause"_s, loss.cause);
            if (loss.limit != LimitID::None)
                printer.field("limit"_s, loss.limit);
            if (hasFunction(loss.witness)) {
                printer.beginField("witness"_s);
                printWitness(loss.witness);
            }
        }
        if (index < sites.callsOfBoundFunctions.size() && sites.callsOfBoundFunctions[index])
            printer.field("mayCallBoundFunction"_s, true);
        printer.endLine();
    }

    for (const FlowIndirectCall& call : functions.indirectCalls) {
        beginLine("indirect"_s);
        printer.print(call.caller);
        out.print(" "_s);
        printWithLetter(accessSiteLetter, call.site);
        out.print(" -> "_s);
        printer.print(call.callee);
        out.print(" "_s, nameOf(call.kind), " "_s, std::span(builtinCallbackTimesWords)[static_cast<size_t>(call.times.predicted())]);
        if (call.isInCycle)
            out.print(" cycle"_s);
        printer.endLine();
    }

    for (const BuiltinLocationFlow& location : builtins.locations) {
        beginLine("builtin-location"_s);
        printer.print(location.holder);
        out.print("."_s);
        printer.print(location.key);
        print(location.state);
        if (hasFunction(location.witness)) {
            out.print(" written at "_s);
            printWitness(location.witness);
        }
        printer.endLine();
    }
    for (const BuiltinUseRecord& use : builtins.uses) {
        beginLine("builtin-use"_s);
        printer.print(BuiltinID(use.builtin));
        print(OptionSet<BuiltinUse>::fromRaw(static_cast<uint32_t>(use.uses)));
        printOptionalWitness(use.witness);
        printer.endLine();
    }
    for (const FeatureUseRecord& use : builtins.featureUses) {
        beginLine("feature"_s);
        out.print(use.feature);
        printOptionalBuiltin(use.builtin);
        if (use.kind != FeatureUseKind::Named)
            out.print(" "_s, nameOf(use.kind));
        printOptionalWitness(use.witness);
        printer.endLine();
    }
    for (const FlowLocaleUse& use : builtins.localeUses) {
        beginLine("locale"_s);
        out.print(nameOf(use.kind));
        if (use.tag)
            print(use.tag);
        printOptionalWitness(use.witness);
        printer.endLine();
    }
    for (VariableID variable : builtins.exportsThatSpiesReplace) {
        beginLine("replaced-export"_s);
        out.print("g"_s, variable.index());
        printer.endLine();
    }
    for (BuiltinID member : builtins.builtinsThatTestsReplace) {
        printer.beginLine("replaced-builtin"_s);
        print(member);
        printer.endLine();
    }
    for (const FlowAddedName& added : builtins.namesThatTestsAdd) {
        printer.beginLine("added-name"_s);
        print(added.holder);
        out.print(' ', String::fromUTF8ReplacingInvalidSequences(added.name.span()));
        printer.endLine();
    }
    for (const FlowWorkerEntry& entry : builtins.workerEntries) {
        beginLine("worker-entry"_s);
        printer.print(entry.path);
        if (entry.isGuess)
            out.print(" guess"_s);
        if (entry.hasOptions)
            out.print(" with-options"_s);
        printOptionalWitness(entry.witness);
        printer.endLine();
    }
    if (builtins.workerStartWithoutName) {
        beginLine("worker-start-without-name"_s);
        printOptionalWitness(*builtins.workerStartWithoutName);
        printer.endLine();
    }

    for (const UnseenCodeRecord& record : wholeProgram.unseenCode) {
        beginLine("unseen-code"_s);
        printer.print(record.cause);
        printOptionalWitness(record.witness);
        printOptionalBuiltin(record.builtin);
        printer.endLine();
    }

    for (const FlowSinkOperand& operand : sites.sinkOperands) {
        const SinkCandidates& candidates = operand.candidates;
        if (candidates == SinkCandidates { })
            continue;
        beginLine("sink"_s);
        printWithLetter(accessSiteLetter, operand.call);
        out.print(" operand "_s, operand.operandIndex, " "_s);
        printer.list(candidates.strings, [&](StringID string) {
            printer.print(string);
        });
        if (candidates.mayBeUndefined)
            out.print(" mayBeUndefined"_s);
        if (candidates.isComplete)
            out.print(" complete"_s);
        printer.endLine();
    }

    for (const FlowTypedResultCall& call : sites.typedResultCalls) {
        beginLine("typed-result"_s);
        printWithLetter(accessSiteLetter, call.call);
        print(call.type);
        printer.endLine();
    }
    for (uint32_t layout = 0; layout < sites.typedResultSites.size(); ++layout) {
        AllocationSiteID site = sites.typedResultSites[layout];
        if (!site)
            continue;
        beginLine("typed-result-site"_s);
        printer.print(LayoutID(layout));
        out.print(" "_s);
        printWithLetter(allocationSiteLetter, site);
        printer.endLine();
    }

    for (const FlowDataNode& node : sites.dataNodes) {
        beginLine("data-node"_s);
        printWithLetter(allocationSiteLetter, node.rootSite);
        out.print(" "_s, node.ordinal, " "_s);
        printWithLetter(allocationSiteLetter, node.stamp.site);
        out.print(" "_s);
        printWithLetter(allocationClassLetter, node.stamp.allocationClass);
        if (node.stamp.elements) {
            out.print(" elements="_s);
            printer.print(node.stamp.elements);
        }
        if (node.stamp.arrayStorage != ArrayStorage::Other) {
            out.print(" arrayStorage="_s);
            printer.print(node.stamp.arrayStorage);
        }
        if (node.stamp.type) {
            out.print(" type="_s);
            printer.print(node.stamp.type);
        }
        printer.endLine();
    }

    switch (builtins.legacyRegExpStatics) {
    case LegacyRegExpStatics::Unknown:
        break;
    case LegacyRegExpStatics::NeverRead:
        beginLine("legacy-regexp-statics"_s);
        out.print("NeverRead"_s);
        printer.endLine();
        break;
    }

    const Vector<Vector<OperandUses>>& framePointerUses = functions.storedFramePointerUses;
    for (uint32_t layout = 0; layout < functions.storedValueUses.size(); ++layout) {
        const Vector<OperandUses>& usesByField = functions.storedValueUses[layout];
        for (uint32_t field = 0; field < usesByField.size(); ++field) {
            OperandUses uses = usesByField[field];
            if (uses.containsAll(everyOperandUse))
                continue;
            beginLine("stored-value"_s);
            printer.print(LayoutID(layout));
            out.print("."_s, field);
            printer.field("uses"_s, uses & everyOperandUse);
            if (layout < framePointerUses.size() && field < framePointerUses[layout].size() && framePointerUses[layout][field] != uses)
                printer.field("asFramePointer"_s, (framePointerUses[layout][field] - uses) & everyOperandUse);
            printer.endLine();
        }
    }

    switch (wholeProgram.instances) {
    case ProgramInstances::Unknown:
        break;
    case ProgramInstances::AtMostOne:
        beginLine("program instances"_s);
        out.print("AtMostOne"_s);
        printer.endLine();
        break;
    }
    dumpKeys("stored-through-unknown-receivers"_s, wholeProgram.keysStoredThroughUnknownReceivers);
    dumpKeys("defined-on-objects-without-known-layout"_s, wholeProgram.keysDefinedOnObjectsWithoutKnownLayout);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
