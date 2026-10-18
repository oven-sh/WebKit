/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "Options.h"
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

bool isOperation(const Program& program, const Value& value, LanguageOperation operation)
{
    auto& builtins = Builtins::singleton();
    BuiltinID entry = program.operationOf(value);
    return entry && builtins.at(entry).implementation.kind == BuiltinImplementation::Operation && builtins.at(entry).implementation.operation == operation;
}

std::optional<AllocationKind> allocationKindOf(const Program& program, const Value& value)
{
    switch (value.opcode) {
    case Opcode::NewObjectWithShape: {
        auto& layout = program.at(value.layout());
        if (layout.isCoroutineFrame)
            return AllocationKind::CoroutineFrame;
        return isObjectLayout(layout.kind) ? AllocationKind::Object : AllocationKind::Environment;
    }
    case Opcode::NewObject:
    case Opcode::CreateThis:
        return AllocationKind::Object;
    case Opcode::NewArray:
        return AllocationKind::Array;
    case Opcode::NewClosure:
    case Opcode::NewClosureOnce:
        return AllocationKind::Closure;
    case Opcode::CreateRest:
        return AllocationKind::Rest;
    case Opcode::Operation:
    case Opcode::CallBuiltin:
        if (isOperation(program, value, LanguageOperation::CreateArguments))
            return AllocationKind::Arguments;
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

bool aliasesOperand(const Value& value)
{
    return value.opcode == Opcode::Cast && value.castReason() != CastReason::As && value.castReason() != CastReason::Coerce;
}

static ValueID returnedOperand(const Value& value)
{
    if (value.opcode != Opcode::CallBuiltin)
        return { };
    auto& builtin = Builtins::singleton().at(value.builtin());
    if (builtin.result.kinds == BuiltinResult::This && !builtin.result.arguments)
        return value.child(0);
    if (builtin.result.kinds || !hasOneBitSet(builtin.result.arguments))
        return { };
    unsigned index = getLSBSet(builtin.result.arguments);
    return index < value.arguments.size() ? value.arguments[index] : ValueID();
}

ValueID underlyingValue(const Function& function, ValueID id)
{
    for (;;) {
        auto& value = function.at(id);
        if (aliasesOperand(value))
            id = value.child(0);
        else if (ValueID operand = returnedOperand(value))
            id = operand;
        else
            return id;
    }
}

ASCIILiteral nameOf(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Return:
        return "Return"_s;
    case TerminatorKind::Throw:
        return "Throw"_s;
    default:
        return "a block parameter"_s;
    }
}

struct Use {
    ASCIILiteral operation;
    uint32_t inlineFrame { 0 };
    uint32_t position { 0 };
    ASCIILiteral why;
};

enum class Need : uint8_t { None, Store, Read, Object };

static bool isUsed(const Function& function, ValueID id)
{
    bool found = false;
    for (auto& block : function.blocks) {
        for (ValueID user : block.values) {
            function.at(user).forEachChild([&](ValueID child) {
                found |= child == id;
            });
        }
        const_cast<Terminator&>(block.terminator).forEachChild([&](ValueID& child) {
            found |= child == id;
        });
        if (found)
            return true;
    }
    return false;
}

Need needFor(const Program& program, const Function& function, const Value& user, ValueID object)
{
    auto is = [&](ValueID child) {
        return child && underlyingValue(function, child) == object;
    };
    switch (user.opcode) {
    case Opcode::AssertNotAllocated:
        return Need::None;
    case Opcode::Cast:
        return aliasesOperand(user) ? Need::None : Need::Object;
    case Opcode::PutField:
    case Opcode::PutById:
        return is(user.child(1)) ? Need::Object : Need::Store;
    case Opcode::PutByVal:
        return is(user.child(1)) || is(user.child(2)) ? Need::Object : Need::Store;
    case Opcode::Operation:
    case Opcode::CallBuiltin: {
        auto& builtins = Builtins::singleton();
        BuiltinID entry = program.operationOf(user);
        if (!entry)
            return Need::Object;
        auto& builtin = builtins.at(entry);
        uint16_t uses = builtin.hasReceiver() && is(user.child(0)) ? builtin.receiverUses : 0;
        for (unsigned i = 0; i < user.arguments.size(); ++i) {
            if (is(user.arguments[i]))
                uses |= builtin.argumentUses(i);
        }
        if (uses & OperandUse::MayBeResult && ((returnedOperand(user) && is(returnedOperand(user))) || !isUsed(function, ValueID(static_cast<unsigned>(&user - function.values.begin())))))
            uses &= ~OperandUse::MayBeResult;
        if (uses & (OperandUse::Keeps | OperandUse::Calls | OperandUse::MayBeResult | OperandUse::EscapesToUserCode))
            return Need::Object;
        if (uses & (OperandUse::WritesProperties | OperandUse::WritesContents) && !(uses & OperandUse::RetainsContents))
            return Need::Store;
        return Need::Read;
    }
    case Opcode::GetByVal:
    case Opcode::InByVal:
    case Opcode::IteratorHasNext:
    case Opcode::IteratorValue:
        return is(user.child(1)) ? Need::Object : Need::Read;
    case Opcode::GetField:
    case Opcode::GetById:
    case Opcode::InById:
    case Opcode::IteratorOpen:
    case Opcode::IteratorClose:
    case Opcode::StrictEq:
    case Opcode::IsUndefinedOrNull:
    case Opcode::HasShapeOf:
    case Opcode::IsClosureOf:
    case Opcode::IsInstanceOfClass:
    case Opcode::TypeOf:
    case Opcode::TypeOfIs:
    case Opcode::ToBoolean:
    case Opcode::LogicalNot:
        return Need::Read;
    default:
        return Need::Object;
    }
}

std::optional<Use> forcingUseOf(const Program& program, const Function& function, ValueID object)
{
    Need greatest = Need::None;
    Use found;
    auto look = [&](const Block& block) {
        const Value* last = &function.at(object);
        for (ValueID id : block.values) {
            auto& user = function.at(id);
            last = &user;
            bool uses = false;
            user.forEachChild([&](ValueID child) {
                uses |= underlyingValue(function, child) == object;
            });
            if (!uses)
                continue;
            Need need = needFor(program, function, user, object);
            if (need > greatest) {
                greatest = need;
                found = { Typed::nameOf(user.opcode), user.inlineFrame, user.position, { } };
            }
        }
        if (greatest == Need::Object)
            return;
        auto& terminator = block.terminator;
        bool uses = terminator.value && (terminator.kind == TerminatorKind::Return || terminator.kind == TerminatorKind::Throw) && underlyingValue(function, terminator.value) == object;
        TerminatorKind kind = terminator.kind;
        if (!uses) {
            kind = TerminatorKind::Jump;
            for (auto& target : terminator.targets) {
                for (ValueID argument : target.arguments)
                    uses |= underlyingValue(function, argument) == object;
            }
        }
        if (uses) {
            greatest = Need::Object;
            found = { nameOf(kind), last->inlineFrame, last->position, { } };
        }
    };
    const Block* allocationSite = nullptr;
    for (auto& block : function.blocks) {
        if (block.values.contains(object))
            allocationSite = &block;
    }
    if (allocationSite)
        look(*allocationSite);
    if (greatest == Need::Object)
        found.why = "in its allocation block"_s;
    for (auto& block : function.blocks) {
        if (&block != allocationSite)
            look(block);
    }
    if (greatest == Need::Store)
        found.why = "only stored to"_s;
    if (greatest == Need::Read)
        found.why = "only read"_s;
    if (greatest == Need::None)
        return std::nullopt;
    return found;
}

void printPlace(PrintStream& out, const Program& program, const Function& function, uint32_t inlineFrame, uint32_t position)
{
    ModuleID module = inlineFrame ? program.at(function.inlineFrames[inlineFrame].callee).module : function.module;
    if (!module) {
        out.print("-");
        return;
    }
    auto place = program.lineColumnFor(module, position);
    auto& described = program.modules[module.index()];
    out.print(String::fromUTF8(program.string(described.displayPath ? described.displayPath : described.path)), ":", place.line, ":", place.column);
}

void printName(PrintStream& out, const Program& program, StringID name)
{
    if (name)
        out.print(String::fromUTF8(program.string(name)));
}

void printSite(PrintStream& out, const Program& program, Opcode opcode, LayoutID layout, FunctionID closureOf)
{
    out.print(Typed::nameOf(opcode));
    if (layout) {
        out.print(" ");
        if (!isObjectLayout(program.at(layout).kind))
            out.print(program.at(layout).isCoroutineFrame ? "frame " : "environment ");
        printName(out, program, program.at(layout).name);
        out.print(".", layout.index());
    }
    if (closureOf) {
        out.print(" ");
        printName(out, program, program.at(closureOf).name);
        auto general = program.genericEntrypoints.find(closureOf.index());
        out.print(" f", (general == program.genericEntrypoints.end() ? closureOf : general->value).index());
    }
}

bool isAllocationAssertion(const Function& function)
{
    return function.kind == FunctionKind::Plain && function.captureTypes.isEmpty() && !function.has(Format::FunctionHasThis) && function.parameterTypes.size() == 1 && function.blocks.size() == 1
        && function.blocks[0].terminator.kind == TerminatorKind::Return && (!function.blocks[0].terminator.value || function.at(function.blocks[0].terminator.value).opcode == Opcode::Undefined)
        && std::ranges::all_of(function.blocks[0].values, [&](ValueID id) { return function.at(id).opcode == Opcode::Undefined; });
}

} // anonymous namespace

void convertAllocationAssertions(Program& program, Vector<String>& findings)
{
    if (!Options::useDollarVM())
        return;
    auto name = program.find(asBytes("assertNotAllocated"_s.span8()));
    if (!name)
        return;
    BitVector isAssertion(program.functions.size());
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        auto& function = std::as_const(program).functions[i];
        if (function.name != *name || function.isGenericEntrypoint())
            continue;
        if (isAllocationAssertion(function))
            isAssertion.set(i);
        else
            findings.append("assertNotAllocated must be a function with one parameter and an empty body that captures nothing"_s);
    }
    for (auto& function : program.functions) {
        for (auto& value : function.values) {
            if ((value.opcode != Opcode::CallDirect && value.opcode != Opcode::FunctionReference) || !isAssertion.get(value.function().index()))
                continue;
            if (value.opcode != Opcode::CallDirect || value.arguments.size() != 1) {
                findings.append("assertNotAllocated can only be called directly, with exactly one argument"_s);
                continue;
            }
            program.hasAllocationAssertions = true;
            value.opcode = Opcode::AssertNotAllocated;
            value.children = { value.arguments[0], ValueID(), ValueID() };
            value.arguments.clear();
            value.immediates = { Format::none, Format::none };
            value.setPrediction({ });
            value.setPredictedClosure({ });
        }
    }
}

void reportAllocationAssertions(const Program& program, Vector<String>& findings)
{
    if (!program.hasAllocationAssertions)
        return;
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (!program.hasCode(FunctionID(i)))
            continue;
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& assertion = function.at(id);
                if (assertion.opcode != Opcode::AssertNotAllocated)
                    continue;
                ValueID object = underlyingValue(function, assertion.child(0));
                auto& site = function.at(object);
                StringPrintStream out;
                printPlace(out, program, function, assertion.inlineFrame, assertion.position);
                out.print(": assertNotAllocated: ");
                printSite(out, program, site.opcode, site.opcode == Opcode::NewObjectWithShape ? site.layout() : LayoutID(), site.opcode == Opcode::NewClosure || site.opcode == Opcode::NewClosureOnce || site.opcode == Opcode::CallDirect ? site.function() : FunctionID());
                out.print(" at ");
                printPlace(out, program, function, site.inlineFrame, site.position);
                if (!allocationKindOf(program, site))
                    out.print(" has no visible allocation here");
                else {
                    out.print(" is allocated");
                    if (auto use = forcingUseOf(program, function, object)) {
                        out.print(": ", use->operation, " at ");
                        printPlace(out, program, function, use->inlineFrame, use->position);
                        if (use->why)
                            out.print(" (", use->why, ")");
                    }
                }
                out.print(" in ");
                printName(out, program, function.name);
                findings.append(out.toString());
            }
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
