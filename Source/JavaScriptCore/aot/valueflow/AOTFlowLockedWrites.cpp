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
        // A `require` may stand in a function or a branch.
        for (const ModuleRequest& request : program.at(worklist.takeLast()).requests) {
            if (request.kind == ModuleRequestKind::Import && request.phase == ImportPhase::Evaluation)
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

// The lock's refusal throws, in sloppy code too. But some changes the language itself refuses, and of those sloppy code is not told: a store to a property that is read-only or has no setter,
// a deletion of one that is not configurable. The table holds no attributes, so this goes by what the language says of kinds.
enum class RefusalInSloppyCode : uint8_t {
    Throws,
    MayThrow,
    IsLikelySilent,
};

static RefusalInSloppyCode refusalInSloppyCode(const BuiltinTable& table, BuiltinID member)
{
    // A new name, which the holder does not take.
    if (!member)
        return RefusalInSloppyCode::Throws;
    const Builtin& entry = table.at(member);
    switch (entry.kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        // What the language puts under a string is writable and configurable where it is one of these. Under a symbol some are not, and the host does as it likes.
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

static bool isInSloppyCode(const Value& write)
{
    return Value::opcodesWithMode.contains(write.opcode) && write.mode() == ECMAMode::Sloppy;
}

// A function of Reflect answers false.
static bool isIgnoredWhenRefused(const LockedWriteContext& context, const Function& function, const Value& write, BuiltinID member)
{
    if (write.opcode == Opcode::Call) {
        BuiltinID callee = context.program.typeOf(function, write.calleeOperand()).singleBuiltin();
        return callee && context.program.builtins().at(callee).holder == Builtins::Reflect;
    }
    return isInSloppyCode(write) && refusalInSloppyCode(context.program.builtins(), member) == RefusalInSloppyCode::IsLikelySilent;
}

static bool isSpy(const LockedWriteContext& context, const Function& function, const Value& write)
{
    if (write.opcode != Opcode::Call)
        return false;
    BuiltinID callee = context.program.typeOf(function, write.calleeOperand()).singleBuiltin();
    return callee == Builtins::BunTestSpyOn || callee == Builtins::BunTestJestSpyOn || callee == Builtins::BunTestViSpyOn;
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

// What an object literal that stands in the place of an argument gives its properties. Nothing where anything else fills or sees the object: a spread, a computed key, a variable that holds it.
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

// The run time accepts a definition that repeats what the member is, or that only takes from what it admits: a polyfill that defines what is there, and whoever writes out what Object.freeze() does.
// A definition changes the member for sure only where its descriptor is known to hold another value or an accessor function: the call spells it out, or every object of the descriptor's layout has the field.
static bool mayLeaveMemberAsItIs(const LockedWriteContext& context, const Function& function, const CFG& cfg, ValueID id, BuiltinID member)
{
    const Program& program = context.program;
    const Value& write = function.at(id);
    if (write.opcode != Opcode::Call || !member)
        return false;
    BuiltinID callee = program.typeOf(function, write.calleeOperand()).singleBuiltin();
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
        return field.flags.containsAll({ FieldFlags::IsRequired, FieldFlags::IsEnforced }) ? &program.at(field.declared.type) : nullptr;
    };
    bool hasAccessorFunction = typeUnder(BuiltinNames::get) || typeUnder(BuiltinNames::set);
    const Type* value = typeUnder(BuiltinNames::value);
    const BuiltinTable& table = program.builtins();
    switch (table.at(member).kind) {
    case BuiltinKind::Accessor:
        // It may be given the functions that it has. A value makes a data property of it.
        return !value;
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
        // It may be given the value that it has, and no type says which string or number a value is.
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
    if (context.build.options().isCheckOnly() && isSpy(context, function, value))
        return;
    BuiltinID member = memberAt(context, location);
    const Type* stored = member ? storedTypeOf(context, function, value) : nullptr;
    if (stored && !stored->primitives && stored->singleBuiltin() == member)
        return;
    if (mayLeaveMemberAsItIs(context, function, analyses.cfg(), write->value, member))
        return;

    auto kind = [&] {
        if (isIgnoredWhenRefused(context, function, value, member))
            return UserDiagnosticKind::LockedBuiltinWriteIsIgnored;
        if (stored && mayStore(*stored, member))
            return UserDiagnosticKind::LockedBuiltinWriteMayRun;
        if (isInSloppyCode(value) && refusalInSloppyCode(context.program.builtins(), member) != RefusalInSloppyCode::Throws)
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
