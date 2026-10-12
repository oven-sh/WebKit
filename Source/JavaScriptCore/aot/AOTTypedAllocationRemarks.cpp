/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "Options.h"
#include <tuple>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

bool isOperation(const Program& program, const Value& value, LanguageOperation operation)
{
    auto& builtins = Builtins::singleton();
    BuiltinID entry;
    if (value.opcode == Opcode::Operation)
        entry = builtins.find(BuiltinKind::Operation, { }, program.builtinName(value.string()));
    else if (value.opcode == Opcode::CallBuiltin)
        entry = value.builtin();
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

AllocationNote& noteFor(Function& function, AllocationKind kind, const Value& site)
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

bool standsForOperand(const Value& value)
{
    return value.opcode == Opcode::Cast && value.castReason() != CastReason::As && value.castReason() != CastReason::Coerce;
}

static ValueID operandThatIsResult(const Value& value)
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
        if (standsForOperand(value))
            id = value.child(0);
        else if (ValueID operand = operandThatIsResult(value))
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

Need needOf(const Program& program, const Function& function, const Value& user, ValueID object)
{
    auto is = [&](ValueID child) {
        return child && originOf(function, child) == object;
    };
    switch (user.opcode) {
    case Opcode::AssertNotAllocated:
        return Need::None;
    case Opcode::Cast:
        return standsForOperand(user) ? Need::None : Need::Object;
    case Opcode::PutField:
    case Opcode::PutById:
        return is(user.child(1)) ? Need::Object : Need::Store;
    case Opcode::PutByVal:
        return is(user.child(1)) || is(user.child(2)) ? Need::Object : Need::Store;
    case Opcode::Operation:
    case Opcode::CallBuiltin: {
        auto& builtins = Builtins::singleton();
        BuiltinID entry = user.opcode == Opcode::CallBuiltin ? user.builtin() : builtins.find(BuiltinKind::Operation, { }, program.builtinName(user.string()));
        if (!entry)
            return Need::Object;
        auto& builtin = builtins.at(entry);
        uint16_t uses = builtin.takesReceiver() && is(user.child(0)) ? builtin.usesOfThis : 0;
        for (unsigned i = 0; i < user.arguments.size(); ++i) {
            if (is(user.arguments[i]))
                uses |= builtin.usesOfArgument(i);
        }
        if (uses & OperandUse::MayBeResult && ((operandThatIsResult(user) && is(operandThatIsResult(user))) || !isUsed(function, ValueID(static_cast<unsigned>(&user - function.values.begin())))))
            uses &= ~OperandUse::MayBeResult;
        if (uses & (OperandUse::Keeps | OperandUse::Calls | OperandUse::MayBeResult | OperandUse::HandsToUserCode))
            return Need::Object;
        if (uses & (OperandUse::WritesProperties | OperandUse::WritesContents) && !(uses & OperandUse::KeepsWhatItHolds))
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

static ValueID operandThatHolds(const Function& function, const Value& call, ValueID object)
{
    auto& builtin = Builtins::singleton().at(call.builtin());
    uint16_t usesOfObject = 0;
    ValueID holder;
    unsigned numberWritten = 0;
    auto look = [&](ValueID operand, uint16_t uses) {
        if (!operand)
            return;
        ValueID origin = originOf(function, operand);
        if (origin == object) {
            usesOfObject |= uses;
            return;
        }
        if (uses == OperandUse::Everything || !(uses & (OperandUse::WritesProperties | OperandUse::WritesContents)))
            return;
        holder = origin;
        ++numberWritten;
    };
    if (builtin.takesReceiver())
        look(call.child(0), builtin.usesOfThis);
    for (unsigned i = 0; i < call.arguments.size(); ++i)
        look(call.arguments[i], builtin.usesOfArgument(i));
    if (numberWritten != 1 || !(usesOfObject & OperandUse::Keeps) || usesOfObject & (OperandUse::Calls | OperandUse::MayBeResult | OperandUse::HandsToUserCode))
        return { };
    return holder;
}

// The operation on account of which the object is there: one that takes the object itself, else a read of it, else a store to it.
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
            Need need = needOf(program, function, user, object);
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
    const Block* birth = nullptr;
    for (auto& block : function.blocks) {
        if (block.values.contains(object))
            birth = &block;
    }
    if (birth)
        look(*birth);
    if (greatest == Need::Object)
        found.why = "in the block of its birth"_s;
    for (auto& block : function.blocks) {
        if (&block != birth)
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
    auto name = program.find("assertNotAllocated"_s.span8());
    if (!name)
        return;
    BitVector isAssertion(program.functions.size());
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        auto& function = program.functions[i];
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
            value.prediction = { };
            value.predictedClosure = { };
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
                    out.print(" is not seen to be made here");
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

void noteAllocation(const Program& program, Function& function, const Value& site, AllocationOutcome outcome, unsigned, const Value* forcedBy, ASCIILiteral why)
{
    if (!program.notesAllocations)
        return;
    auto kind = allocationKindOf(program, site);
    if (!kind)
        return;
    auto& note = noteFor(function, *kind, site);
    note.outcome = outcome;
    note.why = why;
    note.forcedBy = { };
    if (forcedBy) {
        note.forcedBy = Typed::nameOf(forcedBy->opcode);
        note.forcedByInlineFrame = forcedBy->inlineFrame;
        note.forcedByPosition = forcedBy->position;
    }
}

void noteAllocationSites(const Program& program, Function& function)
{
    if (!program.notesAllocations)
        return;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            if (auto kind = allocationKindOf(program, value))
                describeSite(noteFor(function, *kind, value), value);
        }
    }
}

void concludeAllocationNotes(const Program& program, Function& function)
{
    if (!program.notesAllocations)
        return;
    noteAllocationSites(program, function);

    // Of a site that is left, the first allocation of it: what needs the object most, in the block of its birth and elsewhere. As forcingUseOf(), for all of them at once.
    struct Candidate {
        unsigned note;
        unsigned birth;
        Need inBirthBlock { Need::None };
        Need elsewhere { Need::None };
        Use useInBirthBlock;
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
            auto& note = noteFor(function, *kind, value);
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
            Need& greatest = candidate.birth == b ? candidate.inBirthBlock : candidate.elsewhere;
            if (need <= greatest)
                return;
            greatest = need;
            (candidate.birth == b ? candidate.useInBirthBlock : candidate.useElsewhere) = use;
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
                    use.into = operandThatHolds(function, user, object);
                } else if (user.opcode == Opcode::PutField || user.opcode == Opcode::PutById || user.opcode == Opcode::PutByVal) {
                    if (originOf(function, user.child(0)) != object)
                        use.into = originOf(function, user.child(0));
                } else if (user.opcode == Opcode::NewClosure || user.opcode == Opcode::NewClosureOnce || user.opcode == Opcode::NewArray || user.opcode == Opcode::NewObjectWithShape)
                    use.into = id;
                else if (user.opcode == Opcode::CallDirect) {
                    use.callee = user.function();
                    use.argument = user.arguments.find(child);
                }
                Need need = needOf(program, function, user, object);
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
        Need greatest = std::max(candidate.inBirthBlock, candidate.elsewhere);
        if (greatest == Need::None)
            continue;
        bool isInBirthBlock = candidate.inBirthBlock == greatest;
        auto& use = isInBirthBlock ? candidate.useInBirthBlock : candidate.useElsewhere;
        auto& note = function.allocationNotes[candidate.note];
        note.forcedBy = use.operation;
        note.forcedByInlineFrame = use.inlineFrame;
        note.forcedByPosition = use.position;
        note.forcedByBuiltin = use.builtin;
        note.forcedByCallee = use.callee;
        note.forcedByArgument = use.argument;
        note.forcedByInto = use.into;
        if (greatest == Need::Held)
            note.heldBy = candidate.holders;
        if (note.why.isNull())
            note.why = greatest == Need::Store ? "only stored to"_s : greatest == Need::Read ? "only read"_s : isInBirthBlock ? "in the block of its birth"_s : ASCIILiteral();
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
                if (note.outcome == AllocationOutcome::BuiltLate)
                    out.print(": built at ", note.left.size(), note.left.size() == 1 ? " place" : " places");
                else
                    out.print(note.layout && program.at(note.layout).isInFrame ? ": frame" : ": heap");
                if (note.forcedBy) {
                    out.print(": ", note.forcedBy, " at ");
                    printPlace(out, program, function, note.forcedByInlineFrame, note.forcedByPosition);
                    if (note.forcedByBuiltin) {
                        auto& builtin = Builtins::singleton().at(note.forcedByBuiltin);
                        out.print(" (of ", builtin.holder, builtin.holder.isEmpty() ? "" : ".", builtin.name, ")");
                    } else if (note.forcedByCallee)
                        out.print(" (to f", note.forcedByCallee.index(), " #", note.forcedByArgument, ")");
                    if (!note.heldBy.isEmpty()) {
                        out.print(" (held only by");
                        for (ValueID holder : note.heldBy)
                            out.print(" v", holder.index());
                        out.print(")");
                    } else if (note.forcedByInto) {
                        auto& holder = function.at(note.forcedByInto);
                        out.print(" (into v", note.forcedByInto.index(), " ", holder.opcode == Opcode::Parameter && holder.immediates[0] ? "BlockParameter"_s : Typed::nameOf(holder.opcode), ")");
                    }
                }
                if (note.outcome != AllocationOutcome::BuiltLate && note.left.size() > 1)
                    out.print(" (", note.left.size(), " copies)");
            }
            if (auto use = program.whyEnvironmentsAreOnHeap.find(note.layout ? note.layout.index() : UINT32_MAX - 2); use != program.whyEnvironmentsAreOnHeap.end() && !note.left.isEmpty()) {
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
