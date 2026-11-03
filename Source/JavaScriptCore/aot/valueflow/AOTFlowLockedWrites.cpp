/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTFlowApplication.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFunctionAnalyses.h"
#include "AOTKnownClosures.h"
#include "AOTPropertyAccessElements.h"
#include "AOTSchemaWalkers.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class LockedWriteForm : uint8_t { None, BuiltinCall, Operation };

struct LockedWrite {
    BlockID block;
    ValueID value;
};

} // anonymous namespace

static Vector<ModuleID> computeModuleEvaluationOrder(const Program& program)
{
    struct Frame {
        ModuleID module;
        uint32_t nextRequest { 0 };
    };
    Vector<ModuleID> order;
    BitVector visited(program.modules.size());
    Vector<Frame, 16> stack;
    for (const Entry& entry : program.entries) {
        if (!visited.quickSet(entry.module.index()))
            stack.append({ entry.module });
        while (!stack.isEmpty()) {
            const auto& requests = program.at(stack.last().module).requests;
            if (stack.last().nextRequest == requests.size()) {
                order.append(stack.takeLast().module);
                continue;
            }
            const ModuleRequest& request = requests[stack.last().nextRequest++];
            if (request.kind == ModuleRequestKind::Import && request.phase == ImportPhase::Evaluation && !visited.quickSet(request.target.index()))
                stack.append({ request.target });
        }
    }
    return order;
}

namespace {

class EarlyExitAnalysis {
public:
    EarlyExitAnalysis(const Program& program, const FlowResults& results)
        : m_program(program)
        , m_results(results)
        , m_neverExits(program.functions.size())
        , m_isQueued(program.functions.size())
    {
    }

    static bool isUserModule(const Module& module) { return isProgramModuleOf(module.kind) && !module.flags.contains(ModuleFlags::IsLibrary); }

    bool mayExitAt(FunctionID id, const Value& value)
    {
        Vector<FunctionID, 4> callees;
        auto visitCallee = [&](FunctionID callee) {
            callees.append(callee);
        };
        if (mayExitDirectly(codeKindOf(id), m_program.at(id), value, UnknownCallees::AnyProgramFunction, visitCallee))
            return true;
        if (hasAccessSite(value.opcode)) {
            for (const FlowIndirectCall& call : m_results.indirectCallsFrom(id)) {
                if (call.site == value.accessSite())
                    visitCallee(call.callee);
            }
        }
        return std::ranges::any_of(callees, [&](FunctionID callee) {
            return mayExit(callee);
        });
    }

    bool mayExitAtTerminator(FunctionID id, const Block& block) const { return throwsExplicitly(codeKindOf(id), block); }

    bool mayExitAtUnknownPosition(FunctionID id)
    {
        if (!m_hostCalleeMayExit) {
            m_hostCalleeMayExit = std::ranges::any_of(m_results.indirectCalls(), [&](const FlowIndirectCall& call) {
                return !call.caller && codeKindOf(call.callee) == CodeKind::ProgramFunction && mayExit(call.callee);
            });
        }
        return *m_hostCalleeMayExit || std::ranges::any_of(m_results.indirectCallsFrom(id), [&](const FlowIndirectCall& call) {
            return !call.site && mayExit(call.callee);
        });
    }

    bool mayExit(FunctionID root)
    {
        if (m_neverExits.quickGet(root.index()))
            return false;
        Vector<FunctionID, 16> worklist { root };
        m_isQueued.quickSet(root.index());
        bool foundExit = false;
        for (size_t next = 0; next < worklist.size() && !foundExit; ++next) {
            foundExit = bodyMayExitDirectly(worklist[next], UnknownCallees::AnyProgramFunction, [&](FunctionID callee) {
                if (!m_neverExits.quickGet(callee.index()) && !m_isQueued.quickSet(callee.index()))
                    worklist.append(callee);
            });
        }
        // With recursion, no function is proven safe until every function reachable from the root is.
        for (FunctionID function : worklist) {
            m_isQueued.quickClear(function.index());
            if (!foundExit)
                m_neverExits.quickSet(function.index());
        }
        return foundExit;
    }

private:
    enum class CodeKind : uint8_t { ModuleBody, ProgramFunction, Library };
    enum class UnknownCallees : bool { AnyProgramFunction, Ignored };

    CodeKind codeKindOf(FunctionID id) const
    {
        const Function& function = m_program.at(id);
        if (isLibraryCode(m_program, function))
            return CodeKind::Library;
        return function.module && m_program.at(function.module).body == id ? CodeKind::ModuleBody : CodeKind::ProgramFunction;
    }

    // A handler does not matter: a finally block rethrows.
    static bool throwsExplicitly(CodeKind codeKind, const Block& block) { return codeKind != CodeKind::Library && block.terminator.kind == TerminatorKind::Throw; }

    bool mayNeverResume(const Function& function, const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::Await:
            return protocolMayFindMember(m_program, m_program.typeOf(function, value.valueOperand()), Protocol::Thenable);
        case Opcode::Yield:
        case Opcode::InitialYield:
            return true;
        case Opcode::IteratorResume:
        case Opcode::IteratorClose:
            return value.iteratorKind() == IteratorKind::Async;
        default:
            return false;
        }
    }

    bool mayThrowReferenceError(const Value& value) const
    {
        if (value.opcode != Opcode::PropGet || !value.isFreeName() || value.isForTypeOf())
            return false;
        const BuiltinTable& table = m_program.builtins();
        BuiltinID member = findOwnMember(m_program, table.canonicalID(Builtins::GlobalThis), value.key());
        return !member || table.has(BuiltinMark::MayBeAbsent, member) || table.has(BuiltinMark::IsAddedByOption, member);
    }

    bool isMarkedAsTerminating(BuiltinID builtin) const
    {
        const BuiltinTable& table = m_program.builtins();
        return table.has(BuiltinMark::MayTerminateProcess, table.canonicalID(builtin));
    }

    bool mayTerminateProcess(BuiltinID builtin) const { return isMarkedAsTerminating(builtin) || m_program.builtins().overloadsOf(builtin).empty(); }

    bool passesTerminatingBuiltin(const Function& function, const Value& call) const
    {
        return std::ranges::any_of(call.operands(), [&](ValueID operand) {
            return operand && std::ranges::any_of(m_program.typeOf(function, operand).builtins, [&](BuiltinID builtin) {
                return isMarkedAsTerminating(builtin);
            });
        });
    }

    bool anyProgramFunctionMayExit()
    {
        if (!m_anyProgramFunctionMayExit) {
            auto isUsed = [&](const BuiltinMarkedEntry& entry) {
                return !m_results.usesOf(entry.builtin).isEmpty();
            };
            bool foundExit = m_results.mayRunUnseenCode() || std::ranges::any_of(m_program.builtins().marked(BuiltinMark::MayTerminateProcess), isUsed);
            for (uint32_t index = 0; index < m_program.functions.size() && !foundExit; ++index) {
                FunctionID id(index);
                foundExit = m_program.at(id).hasBody() && codeKindOf(id) != CodeKind::Library && bodyMayExitDirectly(id, UnknownCallees::Ignored, [](FunctionID) { });
            }
            m_anyProgramFunctionMayExit = foundExit;
        }
        return *m_anyProgramFunctionMayExit;
    }

    bool mayExitDirectly(CodeKind codeKind, const Function& function, const Value& value, UnknownCallees unknownCallees, NOESCAPE const Invocable<void(FunctionID)> auto& visitCallee)
    {
        if ((codeKind == CodeKind::ModuleBody && mayNeverResume(function, value)) || (codeKind != CodeKind::Library && mayThrowReferenceError(value)))
            return true;
        switch (value.opcode) {
        case Opcode::RequireModule:
        case Opcode::EvaluateModule:
            if (const Module& target = m_program.at(value.module()); isUserModule(target) && target.body)
                visitCallee(target.body);
            return false;
        case Opcode::RequireDynamic:
            return true;
        default:
            break;
        }
        bool isCall = Value::opcodesWithCallees.contains(value.opcode);
        forEachFunctionReference(value, [&](FunctionID, FunctionReferenceKind referenceKind) {
            isCall |= referenceKind == FunctionReferenceKind::Call;
        });
        if (!isCall)
            return false;
        CalleeList callees = calleesOfCall(m_program, value);
        if (callees.runsNothing())
            return false;
        if (codeKind != CodeKind::Library && (value.type == TypeTable::never || passesTerminatingBuiltin(function, value)))
            return true;
        for (FunctionID callee : callees.listed().functions)
            visitCallee(callee);
        if (std::ranges::any_of(callees.listed().builtins, [&](BuiltinID builtin) { return mayTerminateProcess(builtin); }))
            return true;
        return !callees.listsEveryCallee() && unknownCallees == UnknownCallees::AnyProgramFunction && anyProgramFunctionMayExit();
    }

    bool bodyMayExitDirectly(FunctionID id, UnknownCallees unknownCallees, NOESCAPE const Invocable<void(FunctionID)> auto& visitCallee)
    {
        const Function& function = m_program.at(id);
        if (!function.hasBody())
            return true;
        CodeKind codeKind = codeKindOf(id);
        CFG cfg(function);
        for (BlockID blockID : cfg.reversePostOrder()) {
            const Block& block = function.at(blockID);
            for (ValueID value : block.values) {
                if (mayExitDirectly(codeKind, function, function.at(value), unknownCallees, visitCallee))
                    return true;
            }
            if (throwsExplicitly(codeKind, block))
                return true;
        }
        for (const FlowIndirectCall& call : m_results.indirectCallsFrom(id))
            visitCallee(call.callee);
        return false;
    }

    const Program& m_program;
    const FlowResults& m_results;
    BitVector m_neverExits;
    BitVector m_isQueued;
    std::optional<bool> m_anyProgramFunctionMayExit;
    std::optional<bool> m_hostCalleeMayExit;
};

} // anonymous namespace

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

static bool alwaysRuns(const Program& program, std::span<const ModuleID> evaluationOrder, EarlyExitAnalysis& earlyExits, FunctionAnalyses& analyses, const LockedWrite& write)
{
    const Function& function = analyses.function();
    if (!function.module)
        return false;

    FunctionID id = analyses.context().id;
    if (program.at(function.module).body != id || !EarlyExitAnalysis::isUserModule(program.at(function.module)) || function.at(write.block).handler)
        return false;
    auto position = std::ranges::find(evaluationOrder, function.module);
    if (position == evaluationOrder.end())
        return false;

    if (earlyExits.mayExitAtUnknownPosition(id))
        return false;

    const Dominators& dominators = analyses.dominators();
    for (BlockID other : analyses.cfg().reversePostOrder()) {
        if (dominators.strictlyDominates(write.block, other))
            continue;
        const Block& block = function.at(other);
        for (ValueID value : block.values) {
            if (value == write.value)
                break;
            if (earlyExits.mayExitAt(id, function.at(value)))
                return false;
        }
        if (other != write.block && (returnsToCaller(block.terminator.kind) || earlyExits.mayExitAtTerminator(id, block)))
            return false;
    }
    return std::ranges::none_of(std::span(evaluationOrder.begin(), position), [&](ModuleID earlier) {
        const Module& module = program.at(earlier);
        return EarlyExitAnalysis::isUserModule(module) && module.body && earlyExits.mayExit(module.body);
    });
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

enum class DescriptorFields : bool { InEveryDescriptor, InSomeDescriptor };

static bool describesSameMember(const Program& program, const Function& function, std::span<const ValueID> argumentsOfDefinition)
{
    const Value& descriptor = function.at(definitionThroughSameValues(function, argumentsOfDefinition[2]));
    if (descriptor.opcode != Opcode::Call || descriptor.argumentsOperands().size() < 2)
        return false;
    BuiltinID reader = program.typeOf(function, descriptor.calleeOperand()).singleBuiltin();
    if (reader != Builtins::ObjectGetOwnPropertyDescriptor && reader != Builtins::ReflectGetOwnPropertyDescriptor)
        return false;
    auto isSame = [&](unsigned index, auto single) {
        auto read = single(program.typeOf(function, descriptor.argumentsOperands()[index]));
        return read && read == single(program.typeOf(function, argumentsOfDefinition[index]));
    };
    return isSame(0, [](const Type& type) { return type.singleBuiltin(); }) && isSame(1, [](const Type& type) { return type.singleStringLiteral(); });
}

static bool mayLeaveMemberAsItIs(const Program& program, const Function& function, const CFG& cfg, ValueID id, BuiltinID callee, BuiltinID member, DescriptorFields fields)
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
        return fields == DescriptorFields::InEveryDescriptor || descriptorType.layouts || describesSameMember(program, function, arguments);
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
        bool mayBeThere = fields == DescriptorFields::InSomeDescriptor && field.flow.stability.contains(FieldStability::IsNeverDeleted);
        bool isRead = field.flags.contains(FieldFlags::IsEnforced) && (mayBeThere || field.flags.contains(FieldFlags::IsRequired));
        return isRead ? &program.at(field.declared.expected().predicted()) : nullptr;
    };
    const Type* getter = typeUnder(BuiltinNames::get);
    const Type* setter = typeUnder(BuiltinNames::set);
    bool hasAccessorFunction = getter || setter;
    auto isNoBuiltinFunction = [](const Type* type) {
        return type && !type->anyObject && !type->anyFunction && type->builtins.isEmpty();
    };
    const Type* value = typeUnder(BuiltinNames::value);
    const BuiltinTable& table = program.builtins();
    switch (table.at(member).kind) {
    case BuiltinKind::Accessor:
        return !value && !isNoBuiltinFunction(getter) && !isNoBuiltinFunction(setter);
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

static bool keepsPrototype(const Program& program, const FlowResults& results, const Function& function, const Value& write, BuiltinID callee, const BuiltinLocationFlow& location)
{
    const BuiltinTable& table = program.builtins();
    const Builtin& holder = table.at(location.holder);
    auto isWhatLiesAbove = [&](const Type& prototype) {
        bool holdsOnlyBuiltins = !prototype.anyObject && !prototype.anyFunction && !prototype.layouts && prototype.classes.isEmpty() && prototype.functions.isEmpty();
        return holder.prototypeLink == BuiltinPrototypeLink::Builtin && holdsOnlyBuiltins && prototype.builtins.size() == 1 && prototype.builtins[0] == table.canonicalID(holder.inheritsFrom);
    };
    if (write.opcode == Opcode::Call) {
        std::span<const ValueID> arguments = write.argumentsOperands();
        if ((callee != Builtins::ObjectSetPrototypeOf && callee != Builtins::ReflectSetPrototypeOf) || arguments.size() < 2)
            return false;
        const Type& prototype = program.typeOf(function, arguments[1]);
        return !prototype.primitives && isWhatLiesAbove(prototype);
    }

    std::optional<uint8_t> stored = storedOf(write.opcode);
    if (!stored || results.at(write.accessSite()).kind != AccessKind::Store || location.key != program.keyOf(BuiltinNames::__proto__))
        return false;
    constexpr uint16_t ignoredBySetter = Primitive::Undefined | Primitive::Boolean | Primitive::Number | Primitive::AnyString | Primitive::AnySymbol | Primitive::BigInt;
    uint16_t changesNothing = holder.prototypeLink == BuiltinPrototypeLink::Null ? ignoredBySetter | Primitive::Null : ignoredBySetter;
    const Type& prototype = program.typeOf(function, write.operand(*stored));
    if (prototype.isNever() || !prototype.isJavaScriptValue() || prototype.primitives & ~changesNothing)
        return false;
    return !prototype.hasObjects() || isWhatLiesAbove(prototype);
}

// A store to `__proto__` is answered by the accessor on Object.prototype. If a test replaces that accessor, what the store does is unknown.
static bool isAnsweredByReplacedAccessor(const Program& program, const FlowResults& results, const Value& write, BuiltinID callee, const BuiltinLocationFlow& location)
{
    if (location.key != program.keyOf(BuiltinNames::__proto__) || location.holder == Builtins::ObjectPrototype)
        return false;
    bool isStore = write.opcode == Opcode::Call ? callee == Builtins::ReflectSet : storedOf(write.opcode) && results.at(write.accessSite()).kind == AccessKind::Store;
    const BuiltinLocationFlow* accessor = results.findBuiltinLocation(Builtins::ObjectPrototype, location.key);
    return isStore && accessor && accessor->state != BuiltinLocationState::Constant;
}

// The front end shows nobody a place in Bun's own library.
static bool reportCallsThatNameLocation(const Program& program, BuildContext& build, const BuiltinLocationFlow& location, UncheckedKeyHashSet<uint64_t>& reportedCalls)
{
    const BuiltinTable& table = program.builtins();
    const PropertyKey& key = program.at(location.key);
    if (key.kind != PropertyKeyKind::Text)
        return false;
    bool isNamed = false;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || isLibraryCode(program, function))
            continue;
        for (unsigned block = 0; block < function.blocks.size(); ++block) {
            for (ValueID id : function.blocks[block].values) {
                const Value& call = function.at(id);
                if (call.opcode != Opcode::Call)
                    continue;
                std::span<const ValueID> arguments = call.argumentsOperands();
                bool namesHolder = std::ranges::any_of(arguments, [&](ValueID argument) {
                    BuiltinID builtin = program.typeOf(function, argument).singleBuiltin();
                    return builtin && table.canonicalID(builtin) == table.canonicalID(location.holder);
                });
                bool namesKey = std::ranges::any_of(arguments, [&](ValueID argument) {
                    return program.typeOf(function, argument).singleStringLiteral() == key.text;
                });
                if (!namesHolder || !namesKey)
                    continue;
                isNamed = true;
                if (reportedCalls.add((static_cast<uint64_t>(index) << 32 | id.index()) + 1).isNewEntry)
                    build.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::LockedBuiltinWriteMayRun, .function = FunctionID(index), .block = BlockID(block), .value = id, .location = function.locationOf(id) });
            }
        }
    }
    return isNamed;
}

static bool copiesProperties(const BuiltinTable& table, BuiltinID callee)
{
    return callee && std::ranges::any_of(table.overloadsOf(callee), [](const BuiltinOverload& overload) {
        return std::ranges::any_of(overload.flow.propertyActions, [](BuiltinPropertyActionKind kind) {
            return kind == BuiltinPropertyActionKind::AssignsFrom || kind == BuiltinPropertyActionKind::TakesDescriptorMap || kind == BuiltinPropertyActionKind::DefinesFrom;
        }, &BuiltinPropertyAction::kind);
    });
}

static void reportWrite(const Program& program, const FlowResults& results, BuildContext& build, std::span<const ModuleID> evaluationOrder, EarlyExitAnalysis& earlyExits, const BuiltinLocationFlow& location, UncheckedKeyHashSet<uint64_t>& reportedCalls)
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
    if (keepsPrototype(program, results, function, value, callee, location) || isAnsweredByReplacedAccessor(program, results, value, callee, location))
        return;
    if (isLibraryCode(program, function) && reportCallsThatNameLocation(program, build, location, reportedCalls))
        return;
    if (mayLeaveMemberAsItIs(program, function, analyses.cfg(), write->value, callee, member, DescriptorFields::InSomeDescriptor))
        return;

    bool isInSloppyCode = Value::opcodesWithMode.contains(value.opcode) && value.mode() == ECMAMode::Sloppy;
    auto kind = [&] {
        if (isCall ? callee && table.at(callee).holder == Builtins::Reflect : isInSloppyCode && refusalInSloppyCode(table, member) == RefusalInSloppyCode::IsLikelySilent)
            return UserDiagnosticKind::LockedBuiltinWriteIsIgnored;
        if (stored && mayStore(*stored, member))
            return UserDiagnosticKind::LockedBuiltinWriteMayRun;
        if (copiesProperties(table, callee))
            return UserDiagnosticKind::LockedBuiltinWriteMayRun;
        if (mayLeaveMemberAsItIs(program, function, analyses.cfg(), write->value, callee, member, DescriptorFields::InEveryDescriptor))
            return UserDiagnosticKind::LockedBuiltinWriteMayRun;
        if (isInSloppyCode && refusalInSloppyCode(table, member) != RefusalInSloppyCode::Throws)
            return UserDiagnosticKind::LockedBuiltinWriteMayRun;
        bool alwaysThrows = writesOnlyTo(program, results, function, value, location.holder) && alwaysRuns(program, evaluationOrder, earlyExits, analyses, *write);
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

static void reportWritesUnderUnreadNames(const Program& program, const FlowResults& results, BuildContext& build)
{
    UncheckedKeyHashSet<uint64_t> reportedPlaces;
    for (const BuiltinLocationFlow& write : results.writesOfBuiltinsUnderUnreadNames()) {
        FunctionID id(write.witness.function);
        const Function& function = program.at(id);
        if (!function.hasBody() || isLibraryCode(program, function) || !reportedPlaces.add((static_cast<uint64_t>(write.witness.function) << 32 | write.witness.value) + 1).isNewEntry)
            continue;
        FunctionAnalyses analyses(AnalysisContext { .build = build, .program = program, .flow = &results, .id = id }, function);
        std::optional<LockedWrite> found = findWrite(results, function, analyses.cfg(), write.witness.location);
        if (!found)
            continue;
        bool isRefusedUnderEveryName = write.state == BuiltinLocationState::Constant || program.builtins().at(write.holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames });
        build.diagnostics().add(UserDiagnostic {
            .kind = isRefusedUnderEveryName ? UserDiagnosticKind::LockedBuiltinWriteMayRun : UserDiagnosticKind::LockedBuiltinWriteUnderUnreadName,
            .function = id,
            .block = found->block,
            .value = found->value,
            .location = write.witness.location,
        });
    }
}

void checkLockedBuiltinWrites(const Program& program, const FlowResults& results, BuildContext& build)
{
    PropertyKeyID keyOfPrototypeLink = program.keyOf(BuiltinNames::__proto__);
    auto isLockedWrite = [&](const BuiltinLocationFlow& write) {
        // A native function keeps its prototype, and may get an own property of that name.
        if (write.key == keyOfPrototypeLink && program.builtins().isNativeFunction(write.holder))
            return write.state == BuiltinLocationState::Constant;
        const BuiltinLocationFlow* location = results.findBuiltinLocation(write.holder, write.key);
        return location && location->state == BuiltinLocationState::Constant;
    };
    reportWritesUnderUnreadNames(program, results, build);
    if (std::ranges::none_of(results.writesOfBuiltinLocations(), isLockedWrite))
        return;

    Vector<ModuleID> evaluationOrder = computeModuleEvaluationOrder(program);
    EarlyExitAnalysis earlyExits(program, results);
    UncheckedKeyHashSet<uint64_t> reportedCalls;
    for (const BuiltinLocationFlow& write : results.writesOfBuiltinLocations()) {
        if (isLockedWrite(write))
            reportWrite(program, results, build, evaluationOrder.span(), earlyExits, write, reportedCalls);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
