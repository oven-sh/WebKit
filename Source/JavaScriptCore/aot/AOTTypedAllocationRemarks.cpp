/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTTypedThreads.h"
#include "Options.h"
#include <tuple>
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

AllocationNote& remarkFor(Function& function, AllocationKind kind, const Value& site)
{
    uint64_t key = static_cast<uint64_t>(kind) << 56 | static_cast<uint64_t>(site.inlineFrame) << 32 | site.position;
    auto result = function.allocationNoteIndices.add(key, function.allocationNotes.size());
    if (result.isNewEntry) {
        AllocationNote note;
        note.kind = kind;
        note.inlineFrame = site.inlineFrame;
        note.position = site.position;
        function.allocationNotes.append(note);
    }
    return function.allocationNotes[result.iterator->value];
}

void describeSite(AllocationNote& note, const Value& site)
{
    note.opcode = site.opcode;
    note.layout = site.opcode == Opcode::NewObjectWithShape ? site.layout() : LayoutID();
    note.closureOf = site.opcode == Opcode::NewClosure || site.opcode == Opcode::NewClosureOnce ? site.function() : FunctionID();
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

ValueID originOf(const Function& function, ValueID id)
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
    BuiltinID builtin;
    FunctionID callee;
    uint32_t argument { 0 };
    ValueID into;
};

enum class Need : uint8_t { None, Store, Read, Held, Object };

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
        return child && originOf(function, child) == object;
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
        uint16_t uses = builtin.hasReceiver() && is(user.child(0)) ? builtin.thisUses : 0;
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

static ValueID retainingOperand(const Function& function, const Value& call, ValueID object)
{
    auto& builtin = Builtins::singleton().at(call.builtin());
    uint16_t objectUses = 0;
    ValueID holder;
    unsigned writtenCount = 0;
    auto look = [&](ValueID operand, uint16_t uses) {
        if (!operand)
            return;
        ValueID origin = originOf(function, operand);
        if (origin == object) {
            objectUses |= uses;
            return;
        }
        if (uses == OperandUse::Everything || !(uses & (OperandUse::WritesProperties | OperandUse::WritesContents)))
            return;
        holder = origin;
        ++writtenCount;
    };
    if (builtin.hasReceiver())
        look(call.child(0), builtin.thisUses);
    for (unsigned i = 0; i < call.arguments.size(); ++i)
        look(call.arguments[i], builtin.argumentUses(i));
    if (writtenCount != 1 || !(objectUses & OperandUse::Keeps) || objectUses & (OperandUse::Calls | OperandUse::MayBeResult | OperandUse::EscapesToUserCode))
        return { };
    return holder;
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
                uses |= originOf(function, child) == object;
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
        bool uses = terminator.value && (terminator.kind == TerminatorKind::Return || terminator.kind == TerminatorKind::Throw) && originOf(function, terminator.value) == object;
        TerminatorKind kind = terminator.kind;
        if (!uses) {
            kind = TerminatorKind::Jump;
            for (auto& target : terminator.targets) {
                for (ValueID argument : target.arguments)
                    uses |= originOf(function, argument) == object;
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
            findings.append("assertNotAllocated is a function of one parameter with an empty body, declared where it captures nothing"_s);
    }
    for (auto& function : program.functions) {
        for (auto& value : function.values) {
            if ((value.opcode != Opcode::CallDirect && value.opcode != Opcode::FunctionReference) || !isAssertion.get(value.function().index()))
                continue;
            if (value.opcode != Opcode::CallDirect || value.arguments.size() != 1) {
                findings.append("assertNotAllocated is only ever called, with one argument"_s);
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
                ValueID object = originOf(function, assertion.child(0));
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

void recordAllocation(const Program& program, Function& function, const Value& site, AllocationOutcome outcome, unsigned, const Value* forcedBy, ASCIILiteral why)
{
    if (!program.recordsAllocations)
        return;
    auto kind = allocationKindOf(program, site);
    if (!kind)
        return;
    auto& note = remarkFor(function, *kind, site);
    note.outcome = outcome;
    note.why = why;
    note.forcedBy = { };
    if (forcedBy) {
        note.forcedBy = Typed::nameOf(forcedBy->opcode);
        note.forcedByInlineFrame = forcedBy->inlineFrame;
        note.forcedByPosition = forcedBy->position;
    }
}

void recordAllocationSites(const Program& program, Function& function)
{
    if (!program.recordsAllocations)
        return;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if (auto kind = allocationKindOf(program, value))
                describeSite(remarkFor(function, *kind, value), value);
        }
    }
}

void finalizeAllocationNotes(const Program& program, Function& function)
{
    if (!program.recordsAllocations)
        return;
    recordAllocationSites(program, function);

    struct Candidate {
        unsigned note;
        unsigned allocationSite;
        Need inAllocationBlock { Need::None };
        Need elsewhere { Need::None };
        Use useInAllocationBlock;
        Use useElsewhere;
        Vector<ValueID, 2> holders;
    };
    Vector<Candidate> candidates;
    static constexpr unsigned none = UINT_MAX;
    Vector<unsigned> candidateOf;
    candidateOf.fill(none, function.values.size());
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        for (ValueID id : function.blocks[b].values) {
            auto& value = function.at(id);
            auto kind = allocationKindOf(program, value);
            if (!kind)
                continue;
            auto& note = remarkFor(function, *kind, value);
            note.left.append(id);
            if (note.left.size() > 1 || note.forcedBy)
                continue;
            candidateOf[id.index()] = candidates.size();
            candidates.append(Candidate { static_cast<unsigned>(&note - function.allocationNotes.begin()), b });
        }
    }
    for (unsigned b = 0; b < function.blocks.size() && !candidates.isEmpty(); ++b) {
        auto& block = function.blocks[b];
        auto see = [&](ValueID operand, Need need, const Use& use) {
            auto& candidate = candidates[candidateOf[operand.index()]];
            Need& greatest = candidate.allocationSite == b ? candidate.inAllocationBlock : candidate.elsewhere;
            if (need <= greatest)
                return;
            greatest = need;
            (candidate.allocationSite == b ? candidate.useInAllocationBlock : candidate.useElsewhere) = use;
        };
        for (ValueID id : block.values) {
            auto& user = function.at(id);
            user.forEachChild([&](ValueID child) {
                ValueID object = originOf(function, child);
                if (candidateOf[object.index()] == none)
                    return;
                Use use { Typed::nameOf(user.opcode), user.inlineFrame, user.position, { }, { }, { }, 0, { } };
                if (user.opcode == Opcode::CallBuiltin) {
                    use.builtin = user.builtin();
                    use.into = retainingOperand(function, user, object);
                } else if (user.opcode == Opcode::PutField || user.opcode == Opcode::PutById || user.opcode == Opcode::PutByVal) {
                    if (originOf(function, user.child(0)) != object)
                        use.into = originOf(function, user.child(0));
                } else if (user.opcode == Opcode::NewClosure || user.opcode == Opcode::NewClosureOnce || user.opcode == Opcode::NewArray || user.opcode == Opcode::NewObjectWithShape)
                    use.into = id;
                else if (user.opcode == Opcode::CallDirect) {
                    use.callee = user.function();
                    use.argument = user.arguments.find(child);
                }
                Need need = needFor(program, function, user, object);
                if (need == Need::Object && use.into && candidateOf[use.into.index()] != none) {
                    need = Need::Held;
                    auto& holders = candidates[candidateOf[object.index()]].holders;
                    if (!holders.contains(use.into))
                        holders.append(use.into);
                }
                see(object, need, use);
            });
        }
        auto& terminator = block.terminator;
        auto leave = [&](ValueID operand, TerminatorKind kind) {
            ValueID object = originOf(function, operand);
            if (candidateOf[object.index()] == none)
                return;
            auto& last = function.at(block.values.isEmpty() ? object : block.values.last());
            see(object, Need::Object, Use { nameOf(kind), last.inlineFrame, last.position, { }, { }, { }, 0, { } });
        };
        if (terminator.value && (terminator.kind == TerminatorKind::Return || terminator.kind == TerminatorKind::Throw))
            leave(terminator.value, terminator.kind);
        for (auto& target : terminator.targets) {
            for (ValueID argument : target.arguments)
                leave(argument, TerminatorKind::Jump);
        }
    }
    for (auto& candidate : candidates) {
        Need greatest = std::max(candidate.inAllocationBlock, candidate.elsewhere);
        if (greatest == Need::None)
            continue;
        bool isInAllocationBlock = candidate.inAllocationBlock == greatest;
        auto& use = isInAllocationBlock ? candidate.useInAllocationBlock : candidate.useElsewhere;
        auto& note = function.allocationNotes[candidate.note];
        note.forcedBy = use.operation;
        note.forcedByInlineFrame = use.inlineFrame;
        note.forcedByPosition = use.position;
        note.forcedByBuiltin = use.builtin;
        note.forcedByCallee = use.callee;
        note.forcedByArgument = use.argument;
        note.forcedIntoValue = use.into;
        if (greatest == Need::Held)
            note.referencedBy = candidate.holders;
        if (note.why.isNull())
            note.why = greatest == Need::Store ? "only stored to"_s : greatest == Need::Read ? "only read"_s : isInAllocationBlock ? "in its allocation block"_s : ASCIILiteral();
    }

    function.allocationNoteIndices.clear();
    std::ranges::sort(function.allocationNotes, [](auto& a, auto& b) {
        return std::tuple(a.inlineFrame, a.position, a.kind) < std::tuple(b.inlineFrame, b.position, b.kind);
    });
}

void printAllocationRemarks(const Program& program, PrintStream& out)
{
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (!program.hasCode(FunctionID(i)))
            continue;
        auto& function = program.functions[i];
        for (auto& note : function.allocationNotes) {
            printPlace(out, program, function, note.inlineFrame, note.position);
            out.print(": ");
            printSite(out, program, note.opcode, note.layout, note.closureOf);
            if (note.left.isEmpty())
                out.print(": removed");
            else {
                size_t inFrame = std::ranges::count_if(note.left, [&](ValueID id) {
                    return function.at(id).isStackAllocatedClosure() || function.stackAllocatedEnvironments.get(id.index());
                });
                if (inFrame == note.left.size())
                    out.print(": frame");
                else if (note.outcome == AllocationOutcome::BuiltLate)
                    out.print(": built at ", note.left.size(), note.left.size() == 1 ? " place" : " places");
                else
                    out.print(": heap");
                if (note.forcedBy) {
                    out.print(": ", note.forcedBy, " at ");
                    printPlace(out, program, function, note.forcedByInlineFrame, note.forcedByPosition);
                    if (note.forcedByBuiltin) {
                        auto& builtin = Builtins::singleton().at(note.forcedByBuiltin);
                        out.print(" (of ", builtin.holder, builtin.holder.isEmpty() ? "" : ".", builtin.name, ")");
                    } else if (note.forcedByCallee)
                        out.print(" (to f", note.forcedByCallee.index(), " #", note.forcedByArgument, ")");
                    if (!note.referencedBy.isEmpty()) {
                        out.print(" (referenced only by");
                        for (ValueID holder : note.referencedBy)
                            out.print(" v", holder.index());
                        out.print(")");
                    } else if (note.forcedIntoValue) {
                        auto& holder = function.at(note.forcedIntoValue);
                        out.print(" (into v", note.forcedIntoValue.index(), " ", holder.opcode == Opcode::Parameter && holder.immediates[0] ? "BlockParameter"_s : Typed::nameOf(holder.opcode), ")");
                    }
                }
                if (inFrame && note.layout && std::ranges::any_of(function.values, [&](auto& value) { return value.opcode == Opcode::MaterializeEnvironment && note.left.contains(value.child(1)); }))
                    out.print(" (moves to the heap on one path)");
                if (inFrame && inFrame < note.left.size())
                    out.print(" (", inFrame, " in the frame)");
                if (note.outcome != AllocationOutcome::BuiltLate && note.left.size() > 1)
                    out.print(" (", note.left.size(), " copies)");
            }
            if (auto use = program.environmentHeapReasons.find(note.layout ? note.layout.index() : UINT32_MAX - 2); use != program.environmentHeapReasons.end() && !note.left.isEmpty()) {
                auto& user = program.at(use->value.function);
                out.print(" (not in the frame: ", use->value.operation, " at ");
                printPlace(out, program, user, use->value.inlineFrame, use->value.position);
                out.print(" in ");
                printName(out, program, user.name);
                out.print(")");
            }
            if (note.why)
                out.print(" (", note.why, ")");
            out.print(" in ");
            printName(out, program, function.name);
            out.print(" f", i);
            for (ValueID id : note.left)
                out.print(" v", id.index());
            if (note.inlineFrame) {
                out.print(" < ");
                printName(out, program, program.at(function.inlineFrames[note.inlineFrame].callee).name);
            }
            out.print("\n");
        }
    }
}

namespace {

namespace AllocationUse {
static constexpr uint16_t IsRetained = 1 << 0;
static constexpr uint16_t IsRead = 1 << 1;
static constexpr uint16_t IsReadByName = 1 << 2;
static constexpr uint16_t IsWritten = 1 << 3;
static constexpr uint16_t IsWrittenByName = 1 << 4;
static constexpr uint16_t IsIterated = 1 << 5;
static constexpr uint16_t IsPassedToInlineBuiltin = 1 << 6;
static constexpr uint16_t IsPassedToBuiltin = 1 << 7;
static constexpr uint16_t IsWrittenByBuiltin = 1 << 8;
static constexpr uint16_t IsCompared = 1 << 9;
static constexpr uint16_t IsForwarded = 1 << 10;
static constexpr uint16_t IsReturned = 1 << 11;
static constexpr ASCIILiteral letters = "KrnwmibBgecR"_s;
static constexpr uint16_t isTouchedByCallee = IsRead | IsReadByName | IsWritten | IsWrittenByName | IsIterated | IsPassedToInlineBuiltin | IsPassedToBuiltin | IsWrittenByBuiltin;
static constexpr uint16_t leavesCompiledCode = IsWrittenByName | IsReadByName | IsPassedToBuiltin | IsWrittenByBuiltin;
}

namespace FunctionResult {
static constexpr uint8_t MayBeFresh = 1 << 0;
static constexpr uint8_t MayBeOperand = 1 << 1;
static constexpr uint8_t MayBeAnything = 1 << 2;
}

struct AllocationUses {
    uint16_t own { 0 };
    uint16_t inCallees { 0 };
    std::optional<Use> retainedBy;
    std::optional<Use> returnedBy;

    uint16_t all() const { return own | inCallees; }
    bool isKept() const { return own & AllocationUse::IsRetained; }
    bool isNotRetained() const { return !(own & (AllocationUse::IsRetained | AllocationUse::IsReturned)); }
    bool canBeStackAllocated(bool mayGrowByStore) const { return isNotRetained() && !(inCallees & AllocationUse::isTouchedByCallee) && !(own & (AllocationUse::leavesCompiledCode | (mayGrowByStore ? AllocationUse::IsWritten : 0))); }
};

struct FunctionSummaries {
    Vector<Vector<uint16_t>> operands;
    Vector<uint8_t> results;
};

bool accessesArgumentsIndirectly(const Program& program, const Function& function)
{
    return std::ranges::any_of(function.values, [&](const Value& value) {
        switch (value.opcode) {
        case Opcode::GetArgument:
        case Opcode::CreateRest:
        case Opcode::GetRestLength:
        case Opcode::GetMyArgumentByVal:
        case Opcode::CallForwardVarargs:
        case Opcode::ConstructSuperForwardVarargs:
            return true;
        case Opcode::Operation:
        case Opcode::CallBuiltin:
            return isOperation(program, value, LanguageOperation::CreateArguments);
        default:
            return false;
        }
    });
}

bool isPlainCallDirect(const Value& value)
{
    return value.opcode == Opcode::CallDirect && !value.isJumpToContinuation();
}

uint16_t lettersOf(const Program& program, const Function& function, const Value& user, ValueID object, Need need)
{
    using namespace AllocationUse;
    if (need == Need::Object || need == Need::Held)
        return IsRetained;
    switch (user.opcode) {
    case Opcode::PutField:
    case Opcode::PutByVal:
        return IsWritten;
    case Opcode::PutById:
        return IsWrittenByName;
    case Opcode::GetField:
    case Opcode::GetByVal:
    case Opcode::InByVal:
        return IsRead;
    case Opcode::GetById:
    case Opcode::InById:
        return IsReadByName;
    case Opcode::IteratorOpen:
        return IsIterated;
    case Opcode::StrictEq: {
        ValueID other = originOf(function, user.child(0)) == object ? user.child(1) : user.child(0);
        return program.typeOf(function, other).hasObjects() ? IsCompared : 0;
    }
    case Opcode::Operation:
    case Opcode::CallBuiltin: {
        auto& builtins = Builtins::singleton();
        BuiltinID entry = program.operationOf(user);
        uint16_t result = entry && builtins.at(entry).implementation.kind == BuiltinImplementation::Inline ? IsPassedToInlineBuiltin : IsPassedToBuiltin;
        if (need == Need::Store)
            result |= IsWrittenByBuiltin;
        return result;
    }
    default:
        return 0;
    }
}

Vector<AllocationUses> usesOf(const Program& program, const Function& function, std::span<const ValueID> objects, const FunctionSummaries& summaries)
{
    using namespace AllocationUse;
    RELEASE_ASSERT(objects.size() <= 64);
    Vector<AllocationUses> result(objects.size());
    Vector<uint64_t> mayBe;
    mayBe.fill(0, function.values.size());
    for (unsigned i = 0; i < objects.size(); ++i)
        mayBe[objects[i].index()] |= 1ull << i;
    auto possibleObjectsOf = [&](ValueID id) {
        return id ? mayBe[originOf(function, id).index()] : 0;
    };
    auto add = [&](ValueID to, uint64_t more) {
        bool isNew = more & ~mayBe[to.index()];
        mayBe[to.index()] |= more;
        return isNew;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::Cast || value.opcode == Opcode::IteratorOpen)
                    changed |= add(id, possibleObjectsOf(value.child(0)));
                if (!isPlainCallDirect(value))
                    continue;
                auto& calleeOperands = summaries.operands[value.function().index()];
                for (unsigned i = 0; i < value.arguments.size() && i < calleeOperands.size(); ++i) {
                    if (calleeOperands[i] & IsReturned)
                        changed |= add(id, possibleObjectsOf(value.arguments[i]));
                }
            }
            for (auto& target : block.terminator.targets) {
                auto& parameters = function.at(target.block).parameters;
                for (size_t i = 0; i < parameters.size() && i < target.arguments.size(); ++i)
                    changed |= add(parameters[i], possibleObjectsOf(target.arguments[i]));
            }
        }
    }
    auto note = [&](uint64_t which, uint16_t own, uint16_t inCallees, const Use& use) {
        for (; which; which &= which - 1) {
            auto& uses = result[WTF::ctz(which)];
            uses.own |= own;
            uses.inCallees |= inCallees;
            if (own & IsRetained && !uses.retainedBy)
                uses.retainedBy = use;
            if (own & IsReturned && !uses.returnedBy)
                uses.returnedBy = use;
        }
    };
    for (auto& block : function.blocks) {
        const Value* last = nullptr;
        for (ValueID id : block.values) {
            auto& user = function.at(id);
            last = &user;
            if (user.opcode == Opcode::Cast)
                continue;
            Use use;
            use.operation = Typed::nameOf(user.opcode);
            use.inlineFrame = user.inlineFrame;
            use.position = user.position;
            if (isPlainCallDirect(user)) {
                auto& calleeOperands = summaries.operands[user.function().index()];
                for (ValueID child : user.children)
                    note(possibleObjectsOf(child), IsRetained, 0, use);
                for (unsigned i = 0; i < user.arguments.size(); ++i) {
                    uint64_t which = possibleObjectsOf(user.arguments[i]);
                    if (!which)
                        continue;
                    uint16_t summary = i < calleeOperands.size() ? calleeOperands[i] : IsRetained;
                    use.callee = user.function();
                    use.argument = i;
                    note(which, IsForwarded | (summary & IsRetained), summary & ~(IsRetained | IsForwarded | IsReturned), use);
                }
                continue;
            }
            user.forEachChild([&](ValueID child) {
                uint64_t which = possibleObjectsOf(child);
                if (!which)
                    return;
                ValueID object = originOf(function, child);
                if (user.opcode == Opcode::CallBuiltin)
                    use.builtin = user.builtin();
                note(which, lettersOf(program, function, user, object, needFor(program, function, user, object)), 0, use);
            });
        }
        auto& terminator = block.terminator;
        if (terminator.value && (terminator.kind == TerminatorKind::Return || terminator.kind == TerminatorKind::Throw)) {
            auto& at = last ? *last : function.at(terminator.value);
            Use use;
            use.operation = nameOf(terminator.kind);
            use.inlineFrame = at.inlineFrame;
            use.position = at.position;
            note(possibleObjectsOf(terminator.value), terminator.kind == TerminatorKind::Return ? IsReturned : IsRetained, 0, use);
        }
    }
    return result;
}

template<typename Functor>
void forEachGroupOf64(std::span<const ValueID> objects, const Functor& functor)
{
    for (size_t first = 0; first < objects.size(); first += 64)
        functor(first, objects.subspan(first, std::min<size_t>(64, objects.size() - first)));
}

bool callReturnsFreshObject(const Value& value, const FunctionSummaries& summaries)
{
    using namespace FunctionResult;
    if (!isPlainCallDirect(value))
        return false;
    uint8_t result = summaries.results[value.function().index()];
    return result & MayBeFresh && !(result & MayBeAnything);
}

Vector<ValueID> allocationSitesOf(const Program& program, const Function& function, const FunctionSummaries& summaries)
{
    Vector<ValueID> sites;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            auto kind = allocationKindOf(program, value);
            if ((kind && (*kind == AllocationKind::Array || *kind == AllocationKind::Object || *kind == AllocationKind::Rest)) || callReturnsFreshObject(value, summaries))
                sites.append(id);
        }
    }
    return sites;
}

uint8_t resultOf(const Program& program, const Function& function, const FunctionSummaries& summaries)
{
    using namespace FunctionResult;
    Vector<uint8_t> mayBe;
    mayBe.fill(0, function.values.size());
    BitVector isSettled(function.values.size());
    for (ValueID operand : function.blocks[0].parameters)
        mayBe[operand.index()] = MayBeOperand;
    Vector<ValueID> sites = allocationSitesOf(program, function, summaries);
    forEachGroupOf64(sites.span(), [&](size_t, std::span<const ValueID> group) {
        auto uses = usesOf(program, function, group, summaries);
        for (size_t k = 0; k < group.size(); ++k) {
            mayBe[group[k].index()] = uses[k].isKept() ? MayBeAnything : MayBeFresh;
            if (!isPlainCallDirect(function.at(group[k])))
                isSettled.quickSet(group[k].index());
        }
    });
    auto possibleObjectsOf = [&](ValueID id) {
        return mayBe[originOf(function, id).index()];
    };
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if (isSettled.quickGet(id.index()) || value.opcode == Opcode::Cast || !program.typeOf(function, id).hasObjects())
                continue;
            if (!isPlainCallDirect(value) || summaries.results[value.function().index()] & MayBeAnything)
                mayBe[id.index()] |= MayBeAnything;
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        auto add = [&](ValueID to, uint8_t more) {
            changed |= more & ~mayBe[to.index()];
            mayBe[to.index()] |= more;
        };
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::Cast && value.child(0))
                    add(id, possibleObjectsOf(value.child(0)));
                if (!isPlainCallDirect(value))
                    continue;
                auto& calleeOperands = summaries.operands[value.function().index()];
                for (unsigned i = 0; i < value.arguments.size() && i < calleeOperands.size(); ++i) {
                    if (calleeOperands[i] & AllocationUse::IsReturned)
                        add(id, possibleObjectsOf(value.arguments[i]));
                }
            }
            for (auto& target : block.terminator.targets) {
                auto& parameters = function.at(target.block).parameters;
                for (size_t i = 0; i < parameters.size() && i < target.arguments.size(); ++i)
                    add(parameters[i], possibleObjectsOf(target.arguments[i]));
            }
        }
    }
    uint8_t result = 0;
    for (auto& block : function.blocks) {
        auto& terminator = block.terminator;
        if (terminator.kind == TerminatorKind::Return && terminator.value && program.typeOf(function, terminator.value).hasObjects())
            result |= possibleObjectsOf(terminator.value);
    }
    return result;
}

FunctionSummaries summarizeFunctions(const Program& program, unsigned& numberOfRounds)
{
    using namespace AllocationUse;
    unsigned count = program.functions.size();
    FunctionSummaries summaries;
    summaries.operands.grow(count);
    summaries.results.fill(0, count);
    BitVector isDiscarded(count);
    BitVector isLeftOut(count);
    Vector<Vector<unsigned>> callees(count);
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        if (!program.hasCode(FunctionID(i)) || function.blocks.isEmpty()) {
            isLeftOut.quickSet(i);
            summaries.results[i] = FunctionResult::MayBeAnything;
            continue;
        }
        if (accessesArgumentsIndirectly(program, function))
            isDiscarded.quickSet(i);
        summaries.operands[i].fill(isDiscarded.quickGet(i) ? IsRetained : 0, function.blocks[0].parameters.size());
        for (auto& value : function.values) {
            if (isPlainCallDirect(value))
                callees[i].append(value.function().index());
        }
        std::ranges::sort(callees[i]);
        callees[i].shrink(std::ranges::unique(callees[i]).begin() - callees[i].begin());
    }
    Vector<uint8_t> hasChanged;
    hasChanged.fill(1, count);
    numberOfRounds = 0;
    for (bool changed = true; changed;) {
        bool isFirstRound = !numberOfRounds++;
        FunctionSummaries next = summaries;
        Vector<uint8_t> changedThisRound;
        changedThisRound.fill(0, count);
        forEachIndexInParallel(count, [&](unsigned i) {
            if (isLeftOut.quickGet(i))
                return;
            if (!isFirstRound && std::ranges::none_of(callees[i], [&](unsigned callee) { return hasChanged[callee]; }))
                return;
            auto& function = program.functions[i];
            if (!isDiscarded.quickGet(i)) {
                forEachGroupOf64(function.blocks[0].parameters.span(), [&](size_t first, std::span<const ValueID> group) {
                    auto uses = usesOf(program, function, group, summaries);
                    for (size_t k = 0; k < group.size(); ++k) {
                        if (program.typeOf(function, group[k]).hasObjects())
                            next.operands[i][first + k] |= uses[k].all();
                    }
                });
            }
            next.results[i] |= isDiscarded.quickGet(i) ? FunctionResult::MayBeAnything : resultOf(program, function, summaries);
            changedThisRound[i] = next.operands[i] != summaries.operands[i] || next.results[i] != summaries.results[i];
        });
        changed = std::ranges::any_of(changedThisRound, [](uint8_t value) { return value; });
        hasChanged = WTF::move(changedThisRound);
        summaries = WTF::move(next);
    }
    return summaries;
}

void printLetters(PrintStream& out, uint16_t uses)
{
    if (!uses) {
        out.print("-");
        return;
    }
    for (unsigned i = 0; i < AllocationUse::letters.length(); ++i) {
        if (uses & 1 << i)
            out.print(AllocationUse::letters[i]);
    }
}

void printUse(PrintStream& out, const Program& program, const Function& function, const Use& use)
{
    out.print(use.operation, " at ");
    printPlace(out, program, function, use.inlineFrame, use.position);
    if (use.callee)
        out.print(" (to f", use.callee.index(), " #", use.argument, ")");
    else if (use.builtin) {
        auto& builtin = Builtins::singleton().at(use.builtin);
        out.print(" (of ", builtin.holder, builtin.holder.isEmpty() ? "" : ".", builtin.name, ")");
    }
}

void printVerdict(PrintStream& out, const Program& program, const Function& function, const AllocationUses& uses)
{
    out.print(" own ");
    printLetters(out, uses.own);
    out.print(" callees ");
    printLetters(out, uses.inCallees);
    if (uses.retainedBy) {
        out.print(": kept: ");
        printUse(out, program, function, *uses.retainedBy);
    } else if (uses.returnedBy) {
        out.print(": returned: ");
        printUse(out, program, function, *uses.returnedBy);
    } else
        out.print(": lets go");
}

} // anonymous namespace

void printAllocationUses(const Program& program, PrintStream& out)
{
    using namespace AllocationUse;
    using namespace FunctionResult;
    unsigned numberOfRounds = 0;
    FunctionSummaries summaries = summarizeFunctions(program, numberOfRounds);
    struct Calls {
        unsigned notRetained { 0 };
        unsigned returned { 0 };
        unsigned kept { 0 };
    };
    Vector<Calls> callsOf(program.functions.size());
    unsigned numberOfOperands = 0;
    unsigned notRetainedCount = 0;
    unsigned returnedCount = 0;
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (!program.hasCode(FunctionID(i)) || program.functions[i].blocks.isEmpty())
            continue;
        auto& function = program.functions[i];
        bool isDiscarded = accessesArgumentsIndirectly(program, function);
        forEachGroupOf64(function.blocks[0].parameters.span(), [&](size_t first, std::span<const ValueID> group) {
            auto uses = usesOf(program, function, group, summaries);
            for (size_t k = 0; k < group.size(); ++k) {
                if (!program.typeOf(function, group[k]).hasObjects())
                    continue;
                ++numberOfOperands;
                out.print("operand f", i, " #", first + k);
                if (isDiscarded)
                    out.print(" own K callees -: kept: reaches its arguments otherwise");
                else {
                    notRetainedCount += uses[k].isNotRetained();
                    returnedCount += !uses[k].isKept() && !uses[k].isNotRetained();
                    printVerdict(out, program, function, uses[k]);
                }
                out.print(" in ");
                printName(out, program, function.name);
                out.print("\n");
            }
        });
        Vector<ValueID> sites = allocationSitesOf(program, function, summaries);
        forEachGroupOf64(sites.span(), [&](size_t, std::span<const ValueID> group) {
            auto uses = usesOf(program, function, group, summaries);
            for (size_t k = 0; k < group.size(); ++k) {
                auto& site = function.at(group[k]);
                auto kind = allocationKindOf(program, site);
                out.print("site f", i, " v", group[k].index());
                if (!kind) {
                    out.print(" call f", site.function().index(), summaries.results[site.function().index()] & MayBeOperand ? " fresh or an operand" : " fresh");
                    auto& calls = callsOf[site.function().index()];
                    ++(uses[k].isKept() ? calls.kept : uses[k].isNotRetained() ? calls.notRetained : calls.returned);
                } else {
                    out.print(*kind == AllocationKind::Array ? " array " : *kind == AllocationKind::Rest ? " rest " : " object ");
                    printSite(out, program, site.opcode, site.opcode == Opcode::NewObjectWithShape ? site.layout() : LayoutID(), FunctionID());
                    if (*kind == AllocationKind::Array)
                        out.print(" of ", site.arguments.size());
                }
                printVerdict(out, program, function, uses[k]);
                out.print(kind && uses[k].canBeStackAllocated(*kind != AllocationKind::Object) ? " | frame yes | " : " | frame no | ");
                printPlace(out, program, function, site.inlineFrame, site.position);
                out.print(" in ");
                printName(out, program, function.name);
                for (uint32_t frame = site.inlineFrame; frame; frame = function.inlineFrames[frame].parent) {
                    out.print(" < ");
                    printName(out, program, program.at(function.inlineFrames[frame].callee).name);
                }
                out.print("\n");
            }
        });
    }
    unsigned numberOfFunctionsWithFreshResult = 0;
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        uint8_t result = summaries.results[i];
        if (!program.hasCode(FunctionID(i)) || program.functions[i].blocks.isEmpty() || !result)
            continue;
        auto& function = program.functions[i];
        numberOfFunctionsWithFreshResult += result & MayBeFresh && !(result & MayBeAnything);
        out.print("result f", i, result & MayBeFresh ? " fresh" : "", result & MayBeOperand ? " operand" : "", result & MayBeAnything ? " anything" : "");
        out.print(" | direct calls whose result is let go ", callsOf[i].notRetained, ", returned ", callsOf[i].returned, ", kept ", callsOf[i].kept, function.has(Format::FunctionIsUsedAsValue) ? " | is used as a value" : " | is no value", " | kind ", static_cast<unsigned>(function.kind), " in ");
        printName(out, program, function.name);
        out.print("\n");
    }
    out.print("operands that may hold objects: ", numberOfOperands, "; that are let go: ", notRetainedCount, "; that are only returned: ", returnedCount, "; functions whose every result is fresh or an operand, and some fresh: ", numberOfFunctionsWithFreshResult, "; rounds: ", numberOfRounds, "\n");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
