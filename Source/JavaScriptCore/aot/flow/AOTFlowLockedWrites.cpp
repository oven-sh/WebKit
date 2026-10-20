/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowAnalysis.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTFlowApplication.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPropertyAccessElements.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class WriteForm : uint8_t {
    None,
    BuiltinCall,
    Operation,
};

struct LockedWrite {
    BlockID block;
    ValueID value;
};

struct LockedWriteContext {
    const Program& program;
    const FlowResults& results;
    BuildContext& build;
    BitVector alwaysEvaluatedModules;
};

} // anonymous namespace

static BitVector findAlwaysEvaluatedModules(const Program& program)
{
    BitVector evaluated(program.modules.size());
    Vector<ModuleID, 16> worklist;
    auto visit = [&](ModuleID module) {
        if (!evaluated.quickSet(module.index()))
            worklist.append(module);
    };

    for (const Entry& entry : program.entries)
        visit(entry.module);
    while (!worklist.isEmpty()) {
        for (const ModuleRequest& request : program.at(worklist.takeLast()).requests) {
            if (request.phase == ImportPhase::Evaluation)
                visit(request.target);
        }
    }
    return evaluated;
}

static WriteForm writeFormOf(AccessKind kind)
{
    switch (kind) {
    case AccessKind::Store:
    case AccessKind::Define:
    case AccessKind::Delete:
        return WriteForm::Operation;
    case AccessKind::Call:
    case AccessKind::Construct:
        return WriteForm::BuiltinCall;
    case AccessKind::Unknown:
    case AccessKind::Read:
    case AccessKind::Has:
        return WriteForm::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static WriteForm writeFormAt(const FlowResults& results, const Function& function, ValueID id, SourceLocation location)
{
    const Value& value = function.at(id);
    if (!hasAccessSite(value.opcode) || function.locationOf(id) != location)
        return WriteForm::None;
    return writeFormOf(results.at(value.accessSite()).kind);
}

static std::optional<LockedWrite> findWrite(const FlowResults& results, const Function& function, const CFG& cfg, SourceLocation location)
{
    // Object.assign and Reflect.set write through a call, which counts only where no operation at the location writes.
    std::optional<LockedWrite> builtinCall;
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            switch (writeFormAt(results, function, value, location)) {
            case WriteForm::None:
                break;
            case WriteForm::BuiltinCall:
                if (!builtinCall)
                    builtinCall = LockedWrite { block, value };
                break;
            case WriteForm::Operation:
                return LockedWrite { block, value };
            }
        }
    }
    return builtinCall;
}

static bool dominatesEveryReturn(FunctionAnalyses& analyses, BlockID block)
{
    const Function& function = analyses.function();
    const Dominators& dominators = analyses.dominators();
    bool hasReturn = false;
    for (BlockID other : analyses.cfg().reversePostOrder()) {
        if (!returnsToCaller(function.at(other).terminator.kind))
            continue;
        if (!dominators.dominates(block, other))
            return false;
        hasReturn = true;
    }
    return hasReturn;
}

static bool isProgramModule(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        return true;
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool alwaysRuns(const LockedWriteContext& context, FunctionAnalyses& analyses, BlockID block)
{
    const Function& function = analyses.function();
    if (!function.module)
        return false;

    const Module& module = context.program.at(function.module);
    if (module.body != analyses.context().id || !isProgramModule(module.kind) || module.flags.contains(ModuleFlags::IsLibrary))
        return false;
    if (!context.alwaysEvaluatedModules.get(function.module.index()))
        return false;
    if (function.at(block).handler)
        return false;
    return dominatesEveryReturn(analyses, block);
}

static bool isOnly(const LockedWriteContext& context, const Function& function, ValueID operand, BuiltinID holder)
{
    if (!operand)
        return false;
    const Type& type = context.program.typeOf(function, operand);
    return !type.primitives && type.singleBuiltin() == holder;
}

// Flow lists every write that may reach a locked object. One throws in every run only where the object written to can be nothing else and the key is one name.
static bool writesOnlyTo(const LockedWriteContext& context, const Function& function, ValueID write, BuiltinID holder)
{
    const Value& value = function.at(write);
    if (value.opcode == Opcode::Call) {
        // Object.defineProperty, Reflect.set and the like take the object first and the key second.
        std::span<const ValueID> arguments = value.argumentsOperands();
        return arguments.size() >= 2 && isOnly(context, function, arguments[0], holder) && context.program.typeOf(function, arguments[1]).singleStringLiteral();
    }
    if (writeFormOf(context.results.at(value.accessSite()).kind) != WriteForm::Operation)
        return false;
    if (elementAccessOf(value.opcode) && !constantKeyOf(context.program, context.program.typeOf(function, value.propertyOperand())))
        return false;

    ValueID receiver;
    value.forEachOperand([&](ValueID operand, uint32_t index) {
        if (!index)
            receiver = operand;
    });
    return isOnly(context, function, receiver, holder);
}

// In sloppy code a store or a deletion that is refused does nothing, and a function of Reflect answers false.
static bool isIgnoredWhenRefused(const LockedWriteContext& context, const Function& function, const Value& write)
{
    if (write.opcode == Opcode::Call) {
        BuiltinID callee = context.program.typeOf(function, write.calleeOperand()).singleBuiltin();
        return callee && context.program.builtins().at(callee).holder == Builtins::Reflect;
    }
    return Value::opcodesWithMode.contains(write.opcode) && write.mode() == ECMAMode::Sloppy;
}

static const Type* storedTypeOf(const LockedWriteContext& context, const Function& function, const Value& write)
{
    if (write.opcode != Opcode::PropSet)
        return nullptr;
    return &context.program.typeOf(function, write.operand(*storedOf(write.opcode)));
}

static BuiltinID memberAt(const LockedWriteContext& context, const BuiltinLocationFlow& location)
{
    BuiltinID member = findOwnMember(context.program, location.holder, location.key);
    return member ? context.program.builtins().canonicalID(member) : member;
}

// The run time accepts a store of the value the location holds: `Object.assign = Object.assign || polyfill`.
static bool mayStore(const Type& stored, BuiltinID member)
{
    return stored.anyObject || stored.anyFunction || stored.builtins.contains(member);
}

static void reportWrite(const LockedWriteContext& context, const BuiltinLocationFlow& location)
{
    BuiltinID holder = location.holder;
    const Witness& witness = location.witness;
    FunctionID id(witness.function);
    const Function& function = context.program.at(id);
    if (!function.hasBody())
        return;

    FunctionAnalyses analyses(AnalysisContext { .build = context.build, .program = context.program, .flow = &context.results, .id = id }, function);
    std::optional<LockedWrite> write = findWrite(context.results, function, analyses.cfg(), witness.location);
    if (!write)
        return;

    const Value& value = function.at(write->value);
    BuiltinID member = memberAt(context, location);
    const Type* stored = member ? storedTypeOf(context, function, value) : nullptr;
    if (stored && !stored->primitives && stored->singleBuiltin() == member)
        return;

    auto kind = [&] {
        if (isIgnoredWhenRefused(context, function, value))
            return UserDiagnosticKind::LockedBuiltinWriteIsIgnored;
        if (stored && mayStore(*stored, member))
            return UserDiagnosticKind::LockedBuiltinWriteMayRun;
        bool alwaysThrows = writesOnlyTo(context, function, write->value, holder) && alwaysRuns(context, analyses, write->block);
        return alwaysThrows ? UserDiagnosticKind::LockedBuiltinWriteAlwaysRuns : UserDiagnosticKind::LockedBuiltinWriteMayRun;
    };
    context.build.diagnostics().add(UserDiagnostic {
        .kind = kind(),
        .function = id,
        .block = write->block,
        .value = write->value,
        .location = witness.location,
    });
}

void checkLockedBuiltinWrites(const Program& program, const FlowResults& results, BuildContext& build)
{
    auto isLockedWrite = [](const BuiltinLocationFlow& record) {
        return isWritten(record) && record.state == BuiltinLocationState::Constant;
    };
    if (std::ranges::none_of(results.builtinLocations(), isLockedWrite))
        return;

    LockedWriteContext context { program, results, build, findAlwaysEvaluatedModules(program) };
    for (const BuiltinLocationFlow& record : results.builtinLocations()) {
        if (isLockedWrite(record))
            reportWrite(context, record);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
