/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTFlowApplication.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPropertyAccessElements.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class LockedWriteForm : uint8_t { None, BuiltinCall, Operation };

struct LockedWrite {
    BlockID block;
    ValueID value;
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
            if (request.kind == ModuleRequestKind::Import && request.phase == ImportPhase::Evaluation)
                visit(request.target);
        }
    }
    return evaluated;
}

static LockedWriteForm writeFormOf(AccessKind kind)
{
    switch (kind) {
    case AccessKind::Store:
    case AccessKind::Define:
    case AccessKind::Delete:
        return LockedWriteForm::Operation;
    case AccessKind::Call:
    case AccessKind::Construct:
        return LockedWriteForm::BuiltinCall;
    case AccessKind::Unknown:
    case AccessKind::Read:
    case AccessKind::Has:
    case AccessKind::Truthiness:
    case AccessKind::Equality:
    case AccessKind::Arithmetic:
    case AccessKind::Comparison:
    case AccessKind::Conversion:
    case AccessKind::TypeOf:
    case AccessKind::InstanceOf:
    case AccessKind::Iteration:
    case AccessKind::Spread:
        return LockedWriteForm::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<LockedWrite> findWrite(const FlowResults& results, const Function& function, const CFG& cfg, SourceLocation location)
{
    std::optional<LockedWrite> builtinCall;
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            if (!hasAccessSite(function.at(value).opcode) || function.locationOf(value) != location)
                continue;
            switch (writeFormOf(results.at(function.at(value).accessSite()).kind)) {
            case LockedWriteForm::None:
                break;
            case LockedWriteForm::BuiltinCall:
                if (!builtinCall)
                    builtinCall = LockedWrite { block, value };
                break;
            case LockedWriteForm::Operation:
                return LockedWrite { block, value };
            }
        }
    }
    return builtinCall;
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

static bool alwaysRuns(const Program& program, const BitVector& alwaysEvaluatedModules, FunctionAnalyses& analyses, BlockID block)
{
    const Function& function = analyses.function();
    if (!function.module)
        return false;

    const Module& module = program.at(function.module);
    if (module.body != analyses.context().id || !isProgramModule(module.kind) || module.flags.contains(ModuleFlags::IsLibrary))
        return false;
    if (!alwaysEvaluatedModules.get(function.module.index()) || function.at(block).handler)
        return false;

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

static bool writesOnlyTo(const Program& program, const FlowResults& results, const Function& function, const Value& value, BuiltinID holder)
{
    auto isOnlyHolder = [&](ValueID operand) {
        if (!operand)
            return false;
        const Type& type = program.typeOf(function, operand);
        return !type.primitives && type.singleBuiltin() == holder;
    };
    if (value.opcode == Opcode::Call) {
        std::span<const ValueID> arguments = value.argumentsOperands();
        return arguments.size() >= 2 && isOnlyHolder(arguments[0]) && program.typeOf(function, arguments[1]).singleStringLiteral();
    }
    if (writeFormOf(results.at(value.accessSite()).kind) != LockedWriteForm::Operation)
        return false;
    if (elementAccessOf(value.opcode) && !constantKeyOf(program, program.typeOf(function, value.propertyOperand())))
        return false;

    ValueID receiver;
    value.forEachOperand([&](ValueID operand, uint32_t index) {
        if (!index)
            receiver = operand;
    });
    return isOnlyHolder(receiver);
}

enum class RefusalInSloppyCode : uint8_t { Throws, MayThrow, IsLikelySilent };

static RefusalInSloppyCode refusalInSloppyCode(const BuiltinTable& table, BuiltinID member)
{
    if (!member)
        return RefusalInSloppyCode::Throws;
    const Builtin& entry = table.at(member);
    switch (entry.kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        return entry.origin == BuiltinOrigin::Engine && !table.isSymbolName(entry.name) ? RefusalInSloppyCode::Throws : RefusalInSloppyCode::MayThrow;
    case BuiltinKind::Prototype:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return RefusalInSloppyCode::IsLikelySilent;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mayStore(const Type& stored, BuiltinID member)
{
    return stored.anyObject || stored.anyFunction || stored.builtins.contains(member);
}

static bool mayHoldObject(const BuiltinTable& table, BuiltinID member)
{
    if (table.at(member).kind == BuiltinKind::Constant)
        return false;
    bool isDeclared = false;
    for (const BuiltinOverload& overload : table.overloadsOf(member)) {
        if (overload.mode != BuiltinCallMode::Get)
            continue;
        isDeclared = true;
        if (table.at(overload.result.type).mayBeObject())
            return true;
    }
    return !isDeclared;
}

using LiteralProperties = Vector<std::pair<PropertyKeyID, ValueID>, 4>;
static std::optional<LiteralProperties> propertiesOfLiteralArgument(const Program& program, const Function& function, const CFG& cfg, ValueID call, ValueID argument)
{
    ValueID literal = function.at(argument).opcode == Opcode::FinishObject ? function.at(argument).operand(0) : argument;
    const Value& allocation = function.at(literal);
    if (allocation.opcode != Opcode::NewObject)
        return std::nullopt;

    LiteralProperties properties;
    const Layout& layout = program.at(allocation.layout());
    for (size_t listed = 0; listed < allocation.fields().size(); ++listed)
        properties.append({ layout.fields[allocation.fields()[listed]].key, allocation.valuesOperands()[listed] });

    bool isSeenByOthers = false;
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& user = function.at(id);
            user.forEachOperand([&](ValueID operand, uint32_t index) {
                if ((operand != literal && operand != argument) || id == call || id == argument)
                    return;
                if (user.opcode == Opcode::PropDefine && operand == literal && !index)
                    properties.append({ user.key(), user.valueOperand() });
                else
                    isSeenByOthers = true;
            });
        }
    }
    if (isSeenByOthers)
        return std::nullopt;
    return properties;
}

static bool mayLeaveMemberAsItIs(const Program& program, const Function& function, const CFG& cfg, ValueID id, BuiltinID callee, BuiltinID member)
{
    const Value& write = function.at(id);
    if (write.opcode != Opcode::Call || !member)
        return false;
    std::span<const ValueID> arguments = write.argumentsOperands();
    if ((callee != Builtins::ObjectDefineProperty && callee != Builtins::ReflectDefineProperty) || arguments.size() < 3)
        return false;

    std::optional<LiteralProperties> literal = propertiesOfLiteralArgument(program, function, cfg, id, arguments[2]);
    const Type& descriptorType = program.typeOf(function, arguments[2]);
    LayoutID layout = descriptorType.primitives ? LayoutID() : descriptorType.singleLayout(program);
    if (!literal && !layout)
        return true;
    auto typeUnder = [&](BuiltinName name) -> const Type* {
        PropertyKeyID key = program.keyOf(name);
        if (literal) {
            for (size_t index = literal->size(); index--;) {
                if (literal->at(index).first == key)
                    return &program.typeOf(function, literal->at(index).second);
            }
            return nullptr;
        }
        std::optional<uint32_t> index = program.at(layout).findField(key);
        if (!index)
            return nullptr;
        const Field& field = program.at(layout).fields[*index];
        return field.flags.containsAll({ FieldFlags::IsRequired, FieldFlags::IsEnforced }) ? &program.at(field.declared.expected().predicted()) : nullptr;
    };
    bool hasAccessorFunction = typeUnder(BuiltinNames::get) || typeUnder(BuiltinNames::set);
    const Type* value = typeUnder(BuiltinNames::value);
    const BuiltinTable& table = program.builtins();
    switch (table.at(member).kind) {
    case BuiltinKind::Accessor:
        return !value;
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
        return !hasAccessorFunction && (!value || value->primitives || mayHoldObject(table, member));
    case BuiltinKind::NativeBinding:
        return true;
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
        return !hasAccessorFunction && (!value || mayStore(*value, member));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool keepsPrototype(const Program& program, const Function& function, const Value& write, BuiltinID callee, BuiltinID holder)
{
    if (write.opcode != Opcode::Call || (callee != Builtins::ObjectSetPrototypeOf && callee != Builtins::ReflectSetPrototypeOf))
        return false;
    const BuiltinTable& table = program.builtins();
    std::span<const ValueID> arguments = write.argumentsOperands();
    if (arguments.size() < 2 || table.at(holder).prototypeLink != BuiltinPrototypeLink::Builtin)
        return false;
    const Type& prototype = program.typeOf(function, arguments[1]);
    return !prototype.primitives && prototype.singleBuiltin() == table.canonicalID(table.at(holder).inheritsFrom);
}

static void reportWrite(const Program& program, const FlowResults& results, BuildContext& build, const BitVector& alwaysEvaluatedModules, const BuiltinLocationFlow& location)
{
    const BuiltinTable& table = program.builtins();
    const Witness& witness = location.witness;
    FunctionID id(witness.function);
    const Function& function = program.at(id);
    if (!function.hasBody())
        return;

    FunctionAnalyses analyses(AnalysisContext { .build = build, .program = program, .flow = &results, .id = id }, function);
    std::optional<LockedWrite> write = findWrite(results, function, analyses.cfg(), witness.location);
    if (!write)
        return;

    const Value& value = function.at(write->value);
    bool isCall = value.opcode == Opcode::Call;
    BuiltinID callee = isCall ? program.typeOf(function, value.calleeOperand()).singleBuiltin() : BuiltinID();
    if (build.options().isCheckOnly() && isCall && table.has(BuiltinMark::IsSpy, callee))
        return;
    BuiltinID member = findOwnMember(program, location.holder, location.key);
    if (member)
        member = table.canonicalID(member);
    const Type* stored = member && value.opcode == Opcode::PropSet ? &program.typeOf(function, value.operand(*storedOf(value.opcode))) : nullptr;
    if (stored && !stored->primitives && stored->singleBuiltin() == member)
        return;
    if (mayLeaveMemberAsItIs(program, function, analyses.cfg(), write->value, callee, member) || keepsPrototype(program, function, value, callee, location.holder))
        return;

    bool isInSloppyCode = Value::opcodesWithMode.contains(value.opcode) && value.mode() == ECMAMode::Sloppy;
    auto kind = [&] {
        if (isCall ? callee && table.at(callee).holder == Builtins::Reflect : isInSloppyCode && refusalInSloppyCode(table, member) == RefusalInSloppyCode::IsLikelySilent)
            return UserDiagnosticKind::LockedBuiltinWriteIsIgnored;
        if (stored && mayStore(*stored, member))
            return UserDiagnosticKind::LockedBuiltinWriteMayRun;
        if (isInSloppyCode && refusalInSloppyCode(table, member) != RefusalInSloppyCode::Throws)
            return UserDiagnosticKind::LockedBuiltinWriteMayRun;
        bool alwaysThrows = writesOnlyTo(program, results, function, value, location.holder) && alwaysRuns(program, alwaysEvaluatedModules, analyses, write->block);
        return alwaysThrows ? UserDiagnosticKind::LockedBuiltinWriteAlwaysRuns : UserDiagnosticKind::LockedBuiltinWriteMayRun;
    };
    build.diagnostics().add(UserDiagnostic {
        .kind = kind(),
        .function = id,
        .block = write->block,
        .value = write->value,
        .location = witness.location,
    });
}

void checkLockedBuiltinWrites(const Program& program, const FlowResults& results, BuildContext& build)
{
    auto isLockedWrite = [&](const BuiltinLocationFlow& write) {
        const BuiltinLocationFlow* location = results.findBuiltinLocation(write.holder, write.key);
        return location && location->state == BuiltinLocationState::Constant;
    };
    if (std::ranges::none_of(results.writesOfBuiltinLocations(), isLockedWrite))
        return;

    BitVector alwaysEvaluatedModules = findAlwaysEvaluatedModules(program);
    for (const BuiltinLocationFlow& write : results.writesOfBuiltinLocations()) {
        if (isLockedWrite(write))
            reportWrite(program, results, build, alwaysEvaluatedModules, write);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
