/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosureObservers.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRPrinter.h"
#include "AOTKnownClosures.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr uint16_t observedByNothing = UINT16_MAX;
constexpr uint16_t observedByControlFlow = UINT16_MAX - 1;

struct ObservedSite {
    uint16_t observer { observedByNothing };
    uint8_t operandIndex { 0 };
    bool runsAtMostOnce { false };
    bool hasCaptures { false };
    // A read of a variable that is born as a function: the closure is made by the first such read that needs it.
    bool isReadOfDeclaration { false };

    friend bool operator==(const ObservedSite&, const ObservedSite&) = default;
    friend auto operator<=>(const ObservedSite&, const ObservedSite&) = default;
};

// Everything that is done with one created closure, for the question whether it could lie in the frame of its creator.
enum class ClosureFate : uint8_t {
    IsArgumentOfCallByValue = 1 << 0,
    IsArgumentOfDirectCall = 1 << 1,
    IsArgumentOfBuiltin = 1 << 2,
    IsKept = 1 << 3,
};

constexpr OperandUses usesHandingOn {
    OperandUse::Retains,
    OperandUse::Returns,
    OperandUse::RetainsContents,
    OperandUse::BecomesPrototype,
    OperandUse::PassesToUserCode,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
};

struct SiteDetail {
    FunctionID closureFunction;
    LayoutID layout;
    FunctionID creator;
    ObservedSite site;
    uint32_t numberOfCaptures { 0 };
    OptionSet<ClosureFate> fates;
    PropertyKeyID keyOfFirstCall;
};

} // anonymous namespace

static std::optional<Use> findObserver(const Function& function, const UseLists& useLists, ValueID id)
{
    for (const Use& use : useLists.usesOf(id)) {
        if (use.kind != Use::Kind::Operand)
            return use;
        const Value& user = function.at(use.user);
        if (sameValueOperandOf(user) == id) {
            if (std::optional<Use> observer = findObserver(function, useLists, use.user))
                return observer;
            continue;
        }
        if (!callingUses.containsAll(declaredBehaviorOf(user.opcode, use.operandIndex).uses) || isWrittenOperand(user.opcode, use.operandIndex))
            return use;
    }
    return std::nullopt;
}

static void collectFates(const Function& function, const UseLists& useLists, ValueID id, SiteDetail& detail)
{
    for (const Use& use : useLists.usesOf(id)) {
        if (use.kind != Use::Kind::Operand) {
            detail.fates.add(ClosureFate::IsKept);
            continue;
        }
        const Value& user = function.at(use.user);
        if (sameValueOperandOf(user) == id) {
            collectFates(function, useLists, use.user, detail);
            continue;
        }
        OperandUses uses = declaredBehaviorOf(user.opcode, use.operandIndex).uses;
        if (detail.layout ? !uses.containsAny(usesHandingOn) : callingUses.containsAll(uses) && !isWrittenOperand(user.opcode, use.operandIndex))
            continue;
        bool isArgument = std::ranges::contains(user.variadicOperands(), id);
        if (isArgument && user.opcode == Opcode::Call) {
            detail.fates.add(ClosureFate::IsArgumentOfCallByValue);
            if (!detail.keyOfFirstCall)
                detail.keyOfFirstCall = user.calleeKey();
        } else if (isArgument && user.opcode == Opcode::CallDirect)
            detail.fates.add(ClosureFate::IsArgumentOfDirectCall);
        else if (isArgument && user.opcode == Opcode::CallBuiltin)
            detail.fates.add(ClosureFate::IsArgumentOfBuiltin);
        else
            detail.fates.add(ClosureFate::IsKept);
    }
}

static void printDetail(PrintStream& out, IRPrinter& printer, const SiteDetail& detail)
{
    if (detail.layout)
        out.print("object site L", detail.layout.index());
    else
        out.print("closure site f", detail.closureFunction.index());
    out.print(" in f", detail.creator.index(), detail.site.runsAtMostOnce ? " once" : " again", detail.layout ? " fields " : " captures ", detail.numberOfCaptures, " fates");
    if (detail.fates.isEmpty())
        out.print(detail.layout ? " only-local" : " only-called");
    if (detail.fates.contains(ClosureFate::IsArgumentOfCallByValue))
        out.print(" call-by-value");
    if (detail.fates.contains(ClosureFate::IsArgumentOfDirectCall))
        out.print(" direct-call");
    if (detail.fates.contains(ClosureFate::IsArgumentOfBuiltin))
        out.print(" builtin");
    if (detail.fates.contains(ClosureFate::IsKept))
        out.print(" kept");
    out.print(" key ");
    if (detail.keyOfFirstCall)
        printer.print(detail.keyOfFirstCall);
    else
        out.print("-");
    out.println();
}

void dumpClosureObservers(PrintStream& out, const Program& program, const FlowResults& flow, BuildContext& build)
{
    Vector<SiteDetail> details = collectInParallel<SiteDetail>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<SiteDetail>& found) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;
        std::optional<FunctionAnalyses> analyses;
        for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            for (ValueID id : function.blocks[blockIndex].values) {
                const Value& closure = function.at(id);
                if (closure.opcode == Opcode::NewObject && closure.storage() == AllocationStorage::Heap && !program.at(closure.layout()).isEnvironment()) {
                    if (!analyses)
                        analyses.emplace(AnalysisContext { build, program, &flow, nullptr, FunctionID(index) }, function);
                    SiteDetail detail { { }, closure.layout(), FunctionID(index), { }, static_cast<uint32_t>(closure.fields().size()), { }, { } };
                    detail.site.runsAtMostOnce = analyses->executionCountOf(BlockID(blockIndex)) == ExecutionCount::AtMostOnce;
                    collectFates(function, analyses->useLists(), id, detail);
                    found.append(detail);
                    continue;
                }
                bool isReadOfDeclaration = closure.opcode == Opcode::GetModuleVariable && program.at(closure.variable()).initialization == VariableInitialization::Function;
                if (!isReadOfDeclaration && (closure.opcode != Opcode::NewClosure || closure.storage() != AllocationStorage::Heap))
                    continue;
                if (!analyses)
                    analyses.emplace(AnalysisContext { build, program, &flow, nullptr, FunctionID(index) }, function);
                ObservedSite site;
                site.runsAtMostOnce = analyses->executionCountOf(BlockID(blockIndex)) == ExecutionCount::AtMostOnce;
                site.hasCaptures = !isReadOfDeclaration && !closure.variadicOperands().empty();
                site.isReadOfDeclaration = isReadOfDeclaration;
                if (std::optional<Use> observer = findObserver(function, analyses->useLists(), id)) {
                    site.observer = observer->kind == Use::Kind::Operand ? static_cast<uint16_t>(function.at(observer->user).opcode) : observedByControlFlow;
                    site.operandIndex = static_cast<uint8_t>(std::min<uint32_t>(observer->operandIndex, UINT8_MAX));
                }
                SiteDetail detail { isReadOfDeclaration ? program.at(closure.variable()).function : closure.function(), { }, FunctionID(index), site, static_cast<uint32_t>(isReadOfDeclaration ? 0 : closure.variadicOperands().size()), { }, { } };
                collectFates(function, analyses->useLists(), id, detail);
                found.append(detail);
            }
        }
    });

    // A closure names the entrypoint that a call by value enters, and so does the census of a run.
    Vector<FunctionID> entrypoints(program.functions.size());
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (function.role == FunctionRole::GenericEntrypoint && function.derivation.source)
            entrypoints[function.derivation.source.index()] = FunctionID(index);
    }

    IRPrinter printer(out, program);
    Vector<ObservedSite> sites;
    for (SiteDetail& detail : details) {
        if (detail.closureFunction && entrypoints[detail.closureFunction.index()])
            detail.closureFunction = entrypoints[detail.closureFunction.index()];
        if (!detail.layout)
            sites.append(detail.site);
        if (!detail.site.isReadOfDeclaration)
            printDetail(out, printer, detail);
    }
    std::ranges::sort(sites);
    for (size_t first = 0; first < sites.size();) {
        size_t end = first;
        while (end < sites.size() && sites[end] == sites[first])
            ++end;
        const ObservedSite& site = sites[first];
        out.print("closure sites ", end - first, site.runsAtMostOnce ? " once" : " again", site.isReadOfDeclaration ? " declaration" : site.hasCaptures ? " captures" : " plain", " observed by ");
        if (site.observer == observedByNothing)
            out.println("nothing");
        else if (site.observer == observedByControlFlow)
            out.println("a terminator or an edge");
        else
            out.println(nameOf(static_cast<Opcode>(site.observer)), " operand ", site.operandIndex);
        first = end;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
