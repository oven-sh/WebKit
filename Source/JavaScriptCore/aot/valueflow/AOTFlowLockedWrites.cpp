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
#include "AOTKnownClosures.h"
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

static bool alwaysRuns(const Program& program, const BitVector& alwaysEvaluatedModules, FunctionAnalyses& analyses, BlockID block)
{
    const Function& function = analyses.function();
    if (!function.module)
        return false;

    const Module& module = program.at(function.module);
    if (module.body != analyses.context().id || !isProgramModuleOf(module.kind) || module.flags.contains(ModuleFlags::IsLibrary))
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
    if (hasReturn)
        return true;
    bool hasEnd = false;
    for (BlockID other : analyses.cfg().reversePostOrder()) {
        if (!analyses.cfg().successors(other).empty())
            continue;
        if (!dominators.dominates(block, other))
            return false;
        hasEnd = true;
    }
    return hasEnd;
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

static void reportWrite(const Program& program, const FlowResults& results, BuildContext& build, const BitVector& alwaysEvaluatedModules, const BuiltinLocationFlow& location, UncheckedKeyHashSet<uint64_t>& reportedCalls)
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

    BitVector alwaysEvaluatedModules = findAlwaysEvaluatedModules(program);
    UncheckedKeyHashSet<uint64_t> reportedCalls;
    for (const BuiltinLocationFlow& write : results.writesOfBuiltinLocations()) {
        if (isLockedWrite(write))
            reportWrite(program, results, build, alwaysEvaluatedModules, write, reportedCalls);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
