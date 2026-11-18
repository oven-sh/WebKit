/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeldRegExps.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFacts.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTProgramVerifier.h"
#include "AOTThinSummary.h"
#include <algorithm>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr uint32_t holdsNothingYet = 0;
constexpr uint32_t holdsAnything = 1;
constexpr uint32_t firstLiteral = 2;

struct CapturePassedOn {
    uint32_t to;
    uint32_t from;
};

struct FunctionsWithClosuresOfOperation {
    template<typename IDType, typename... Role>
    void operator()(IDType id, Role... role)
    {
        if constexpr (std::is_same_v<IDType, FunctionID>) {
            if (id && ((role == FunctionReferenceKind::Closure) || ...))
                functions.append(id);
        }
    }

    Vector<FunctionID, 8>& functions;
};

} // anonymous namespace

HeldRegExps::HeldRegExps(const Program& program, const Facts& facts)
{
    findPrivateFields(program, facts);
    findVariables(program, facts);
    findNames(program, facts);
    const ThinSummaries& summaries = facts.summaries();
    Vector<uint32_t> held;
    BitVector functionsAsked(program.functions.size());
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> placesOfLiterals;
    auto calleeOf = [](const BodyValue& site) -> FunctionID {
        if (site.opcode() == Opcode::NewClosure)
            return site.as<Opcode::NewClosure>().function();
        return site.opcode() == Opcode::CallDirect ? site.as<Opcode::CallDirect>().function() : FunctionID();
    };
    auto operandsOf = [](const BodyValue& site) {
        return site.opcode() == Opcode::NewClosure ? site.as<Opcode::NewClosure>().capturesOperands() : site.as<Opcode::CallDirect>().argumentsOperands();
    };
    auto madeBy = [](const BodyIndex& index, ValueID operand) {
        while (operand && !index.isParameter(operand) && inputPassThroughOpcodes.contains(index.opcodeOf(operand)))
            operand = index.value(operand).operand(0);
        return operand;
    };
    auto ownCapture = [&](const BodyIndex& index, FunctionID id, ValueID operand) -> std::optional<uint32_t> {
        std::optional<uint32_t> own = operand && index.isParameter(operand) ? index.block(entryBlock).indexOfParameter(operand) : std::nullopt;
        return own && *own < program.at(id).captures.size() ? own : std::nullopt;
    };
    auto forEachSite = [&](FunctionID id, const BodyIndex& index, const Invocable<void(const BodyValue&)> auto& functor) {
        for (ValueID site : summaries.at(id).sitesThatHandOverCaptures)
            functor(index.value(site));
    };

    Vector<FunctionID, 64> toSpread;
    BitVector functionsWithLiterals;
    for (uint32_t number = 0; number < summaries.size(); ++number) {
        FunctionID id(number);
        if (summaries.at(id).sitesThatHandOverCaptures.isEmpty())
            continue;
        const BodyIndex* index = facts.bodies().find(id);
        if (!index)
            return;
        if (!index->opcodes().contains(Opcode::NewRegExp))
            continue;
        if (index->opcodes().contains(Opcode::PutEnvironmentField))
            findFields(program, *index);
        forEachSite(id, *index, [&](const BodyValue& site) {
            FunctionID callee = calleeOf(site);
            if (!callee)
                return;
            for (ValueID operand : operandsOf(site)) {
                ValueID made = madeBy(*index, operand);
                if (made && !index->isParameter(made) && index->opcodeOf(made) == Opcode::NewRegExp) {
                    functionsWithLiterals.set(number);
                    if (!functionsAsked.set(callee.index()))
                        toSpread.append(callee);
                    return;
                }
            }
        });
    }
    if (toSpread.isEmpty())
        return;
    while (!toSpread.isEmpty()) {
        FunctionID id = toSpread.takeLast();
        if (summaries.at(id).sitesThatHandOverCaptures.isEmpty())
            continue;
        const BodyIndex& index = *facts.bodies().find(id);
        forEachSite(id, index, [&](const BodyValue& site) {
            FunctionID callee = calleeOf(site);
            if (!callee || functionsAsked.get(callee.index()))
                return;
            bool handsOwnOver = std::ranges::any_of(operandsOf(site), [&](ValueID operand) {
                return !!ownCapture(index, id, madeBy(index, operand));
            });
            if (handsOwnOver) {
                functionsAsked.set(callee.index());
                toSpread.append(callee);
            }
        });
    }

    for (size_t number : functionsAsked) {
        m_firstCaptures.add(static_cast<uint32_t>(number), static_cast<uint32_t>(held.size()));
        held.insertFill(held.size(), holdsNothingYet, program.at(FunctionID(static_cast<uint32_t>(number))).captures.size());
    }
    auto captureOf = [&](FunctionID function, uint32_t capture) {
        return m_firstCaptures.get(function.index()) + capture;
    };
    Vector<uint32_t> changed;
    Vector<CapturePassedOn> passedOn;
    BitVector escaped;
    auto note = [&](uint32_t capture, uint32_t literal) {
        if (held[capture] != holdsNothingYet && held[capture] != literal) {
            for (uint32_t lost : { held[capture], literal }) {
                if (lost >= firstLiteral)
                    escaped.set(lost - firstLiteral);
            }
        }
        if (held[capture] == literal || held[capture] == holdsAnything)
            return;
        held[capture] = held[capture] == holdsNothingYet ? literal : holdsAnything;
        changed.append(capture);
    };
    for (uint32_t number = 0; number < summaries.size(); ++number) {
        FunctionID id(number);
        if (summaries.at(id).sitesThatHandOverCaptures.isEmpty())
            continue;
        const BodyIndex& index = *facts.bodies().find(id);
        forEachSite(id, index, [&](const BodyValue& site) {
            FunctionID callee = calleeOf(site);
            if (!callee) {
                Vector<FunctionID, 8> functions;
                FunctionsWithClosuresOfOperation made { functions };
                site.forEachReference(made);
                for (FunctionID function : functions) {
                    for (uint32_t capture = 0; functionsAsked.get(function.index()) && capture < program.at(function).captures.size(); ++capture)
                        note(captureOf(function, capture), holdsAnything);
                }
                return;
            }
            if (!functionsAsked.get(callee.index()))
                return;
            std::span<const ValueID> operands = operandsOf(site);
            for (uint32_t capture = 0; capture < program.at(callee).captures.size(); ++capture) {
                uint32_t to = captureOf(callee, capture);
                ValueID operand = capture < operands.size() ? madeBy(index, operands[capture]) : ValueID();
                if (operand && !index.isParameter(operand) && index.opcodeOf(operand) == Opcode::NewRegExp) {
                    auto made = index.value(operand).as<Opcode::NewRegExp>();
                    auto added = placesOfLiterals.add(static_cast<uint64_t>(made.pattern().index()) << 16 | made.regExpFlags().toRaw(), m_literals.size());
                    if (added.isNewEntry)
                        m_literals.append(HeldRegExp { made.pattern(), made.regExpFlags() });
                    note(to, firstLiteral + added.iterator->value);
                    continue;
                }
                std::optional<uint32_t> own = functionsAsked.get(number) ? ownCapture(index, id, operand) : std::nullopt;
                if (own)
                    passedOn.append({ to, captureOf(id, *own) });
                else
                    note(to, holdsAnything);
            }
        });
    }
    std::ranges::sort(passedOn, { }, &CapturePassedOn::from);
    while (!changed.isEmpty()) {
        uint32_t capture = changed.takeLast();
        for (auto [to, from] : std::ranges::equal_range(passedOn, capture, { }, &CapturePassedOn::from))
            note(to, held[from]);
    }
    m_literalsOfCaptures = WTF::move(held);
    if (!m_literals.isEmpty())
        findPrivateLiterals(program, facts, functionsWithLiterals, WTF::move(escaped));
}

static uint64_t keyOfField(const FieldReference& field)
{
    return static_cast<uint64_t>(field.layout.index()) << 32 | field.field;
}

static bool isLockedMember(const Program& program, const Facts& facts, BuiltinID member)
{
    const BuiltinTable& table = program.builtins();
    if (program.isUnlocked(member) || program.isUnlocked(table.canonicalID(member)))
        return false;
    return table.at(member).mutability == BuiltinMutability::AlwaysLocked || table.isLocationOfLockedObject(member) || facts.builtinLocation(member) == BuiltinLocationState::Constant;
}

namespace {

class UsesOfHeldRegExps {
public:
    using LiteralsOfFields = HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    UsesOfHeldRegExps(const Program& program, const BodyIndex& index, BitVector& escaped, const LiteralsOfFields* literalsOfFields = nullptr)
        : m_program(program)
        , m_index(index)
        , m_escaped(escaped)
        , m_literalsOfFields(literalsOfFields)
        , m_literals(FillWith { }, index.numberOfValues(), 0u)
    {
        for (ASCIILiteral name : { "exec"_s, "test"_s })
            m_methods.append(keyOf(name));
        for (ASCIILiteral name : { "lastIndex"_s, "source"_s, "flags"_s, "global"_s, "ignoreCase"_s, "multiline"_s, "sticky"_s, "unicode"_s, "unicodeSets"_s, "dotAll"_s, "hasIndices"_s })
            m_properties.append(keyOf(name));
    }

    void track(ValueID value, uint32_t literal) { m_literals[value.index()] = literal + 1; }
    void visit(NOESCAPE const Invocable<std::optional<uint32_t>(const BodyValue&)> auto& literalMadeBy)
    {
        for (BlockID id : m_index.blocksInDominatorOrder()) {
            BodyBlock block = m_index.block(id);
            for (BodyValue value : block.values()) {
                if (std::optional<uint32_t> made = literalMadeBy(value))
                    track(value.id(), *made);
                else
                    visit(value);
            }
            auto lose = [&](ValueID operand, auto...) { escape(operand); };
            block.terminator().forEachOperand(lose);
            auto loseAtEdge = [&](BodyTarget target, bool) {
                for (uint32_t position = 0; position < target.arguments().size(); ++position) {
                    if (!m_index.isUnused(m_index.block(target.block()).parameter(position)))
                        escape(target.arguments()[position]);
                }
            };
            block.forEachOutgoingEdge(loseAtEdge);
            m_index.forEachEdgeResult(id, [&](const BodyEdgeResult& handed) {
                if (!m_index.isUnused(handed.parameter))
                    escape(handed.subject);
            });
        }
    }

private:
    PropertyKeyID keyOf(ASCIILiteral text) const
    {
        BuiltinName name = m_program.builtins().nameOf(byteCast<uint8_t>(text.span()));
        return name ? m_program.keyOf(name) : PropertyKeyID();
    }
    uint32_t literalOf(ValueID value) const { return value ? m_literals[value.index()] : 0; }
    void escape(ValueID operand)
    {
        if (uint32_t literal = literalOf(operand))
            m_escaped.set(literal - 1);
    }

    bool isMethodOf(ValueID callee, uint32_t literal) const
    {
        while (callee && !m_index.isParameter(callee) && inputPassThroughOpcodes.contains(m_index.opcodeOf(callee)))
            callee = m_index.value(callee).operand(0);
        if (!callee || m_index.isParameter(callee) || m_index.opcodeOf(callee) != Opcode::PropGet)
            return false;
        auto read = m_index.value(callee).as<Opcode::PropGet>();
        return !read.isFreeName() && literalOf(read.objectOperand()) == literal && m_methods.contains(read.key());
    }

    void visit(const BodyValue& user)
    {
        static constexpr OpcodeSet inspections { Opcode::ToBoolean, Opcode::TypeTest, Opcode::StrictEq };
        const Opcode opcode = user.opcode();
        if (inputPassThroughOpcodes.contains(opcode)) {
            m_literals[user.id().index()] = literalOf(user.operand(0));
            return;
        }
        if (inspections.contains(opcode))
            return;
        auto escapeAll = [&](std::span<const ValueID> operands) {
            for (ValueID operand : operands)
                escape(operand);
        };
        switch (opcode) {
        case Opcode::PropGet: {
            auto read = user.as<Opcode::PropGet>();
            if (read.isFreeName() || !(m_methods.contains(read.key()) || m_properties.contains(read.key())))
                escape(read.objectOperand());
            return;
        }
        case Opcode::PropSet: {
            auto store = user.as<Opcode::PropSet>();
            escape(store.valueOperand());
            if (store.isFreeName() || store.key() != m_properties[0])
                escape(store.objectOperand());
            return;
        }
        case Opcode::Call: {
            auto call = user.as<Opcode::Call>();
            escape(call.calleeOperand());
            escapeAll(call.argumentsOperands());
            if (uint32_t literal = literalOf(call.thisValueOperand()); literal && !isMethodOf(call.calleeOperand(), literal))
                escape(call.thisValueOperand());
            return;
        }
        case Opcode::NewClosure:
            return;
        case Opcode::PrivateDefine:
        case Opcode::PrivateSet: {
            const ValueID stored = opcode == Opcode::PrivateSet ? user.as<Opcode::PrivateSet>().valueOperand() : user.as<Opcode::PrivateDefine>().valueOperand();
            auto field = m_literalsOfFields ? m_literalsOfFields->find(keyOfField({ user.layout(), user.field() })) : LiteralsOfFields::const_iterator();
            const bool isOwn = m_literalsOfFields && field != m_literalsOfFields->end() && literalOf(stored) == field->value + 1;
            user.forEachOperand([&](ValueID operand, uint32_t) {
                if (operand != stored || !isOwn)
                    escape(operand);
            });
            if (m_literalsOfFields && field != m_literalsOfFields->end() && !isOwn)
                m_escaped.set(field->value);
            return;
        }
        case Opcode::CallDirect: {
            auto call = user.as<Opcode::CallDirect>();
            std::span<const ValueID> operands = call.argumentsOperands();
            escapeAll(operands.subspan(std::min<size_t>(operands.size(), m_program.at(call.function()).captures.size())));
            return;
        }
        default:
            user.forEachOperand([&](ValueID operand, uint32_t) { escape(operand); });
            return;
        }
    }

    const Program& m_program;
    const BodyIndex& m_index;
    BitVector& m_escaped;
    const LiteralsOfFields* m_literalsOfFields;
    Vector<uint32_t> m_literals;
    Vector<PropertyKeyID, 2> m_methods;
    Vector<PropertyKeyID, 12> m_properties;
};

} // anonymous namespace

void HeldRegExps::findPrivateLiterals(const Program& program, const Facts& facts, const BitVector& functionsWithLiterals, BitVector&& escaped)
{
    if (!isLockedMember(program, facts, BuiltinID(Builtins::RegExpPrototypeExec)) || !isLockedMember(program, facts, BuiltinID(Builtins::RegExpPrototypeTest)))
        return;
    BitVector visited;
    auto visit = [&](FunctionID id) {
        if (visited.set(id.index()))
            return;
        const BodyIndex* index = facts.bodies().find(id);
        if (!index) {
            for (size_t literal = 0; literal < m_literals.size(); ++literal)
                escaped.set(literal);
            return;
        }
        UsesOfHeldRegExps uses(program, *index, escaped);
        if (auto first = m_firstCaptures.find(id.index()); first != m_firstCaptures.end()) {
            BodyBlock entry = index->block(entryBlock);
            for (uint32_t capture = 0; capture < program.at(id).captures.size(); ++capture) {
                if (uint32_t held = m_literalsOfCaptures[first->value + capture]; held >= firstLiteral)
                    uses.track(entry.parameter(capture), held - firstLiteral);
            }
        }
        uses.visit([&](const BodyValue& value) -> std::optional<uint32_t> {
            if (value.opcode() != Opcode::NewRegExp)
                return std::nullopt;
            auto made = value.as<Opcode::NewRegExp>();
            size_t literal = m_literals.findIf([&](const HeldRegExp& held) { return held.pattern == made.pattern() && held.flags == made.regExpFlags(); });
            return literal == notFound ? std::nullopt : std::optional<uint32_t>(static_cast<uint32_t>(literal));
        });
    };
    for (size_t number : functionsWithLiterals)
        visit(FunctionID(static_cast<uint32_t>(number)));
    for (auto& [number, first] : m_firstCaptures) {
        std::span<const uint32_t> captures = m_literalsOfCaptures.span().subspan(first, program.at(FunctionID(number)).captures.size());
        if (std::ranges::any_of(captures, [](uint32_t held) { return held >= firstLiteral; }))
            visit(FunctionID(number));
    }
    for (size_t literal = 0; literal < m_literals.size(); ++literal)
        m_literals[literal].isPrivate = !escaped.get(literal);
}

void HeldRegExps::findFields(const Program& program, const BodyIndex& index)
{
    for (uint32_t number = 0; number < index.numberOfValues(); ++number) {
        ValueID id(number);
        if (index.isParameter(id) || index.opcodeOf(id) != Opcode::PutEnvironmentField)
            continue;
        auto store = index.value(id).as<Opcode::PutEnvironmentField>();
        const ValueID stored = index.stripCasts(store.valueOperand());
        const FieldReference field { store.layout(), store.field() };
        if (index.isParameter(stored) || index.opcodeOf(stored) != Opcode::NewRegExp || !fieldAt(program, field).flags.contains(FieldFlags::IsImmutable))
            continue;
        auto made = index.value(stored).as<Opcode::NewRegExp>();
        const HeldRegExp literal { made.pattern(), made.regExpFlags() };
        auto added = m_fields.add(keyOfField(field), literal);
        if (!added.isNewEntry && added.iterator->value != std::optional(literal))
            added.iterator->value = std::nullopt;
    }
}

void HeldRegExps::noteStore(const FieldReference& field, const BodyIndex& index, ValueID stored)
{
    std::optional<HeldRegExp> literal;
    if (!index.isParameter(stored) && index.opcodeOf(stored) == Opcode::NewRegExp) {
        auto made = index.value(stored).as<Opcode::NewRegExp>();
        literal = HeldRegExp { made.pattern(), made.regExpFlags() };
    }
    auto added = m_fields.add(keyOfField(field), literal);
    if (!added.isNewEntry && added.iterator->value != literal)
        added.iterator->value = std::nullopt;
}

void HeldRegExps::findPrivateFields(const Program& program, const Facts& facts)
{
    static constexpr OpcodeSet stores { Opcode::PrivateDefine, Opcode::PrivateSet };
    static constexpr OpcodeSet accesses = stores | OpcodeSet { Opcode::PrivateGet };
    if (facts.unseenCodeCanRun())
        return;
    auto forEachAccess = [&](const BodyIndex& index, OpcodeSet opcodes, const Invocable<void(const BodyValue&)> auto& functor) {
        for (uint32_t number = 0; number < index.numberOfValues(); ++number) {
            if (ValueID id(number); !index.isParameter(id) && opcodes.contains(index.opcodeOf(id)))
                functor(index.value(id));
        }
    };
    auto storedBy = [](const BodyIndex& index, const BodyValue& store) {
        return index.stripCasts(store.opcode() == Opcode::PrivateSet ? store.as<Opcode::PrivateSet>().valueOperand() : store.as<Opcode::PrivateDefine>().valueOperand());
    };
    Vector<FunctionID, 64> users;
    for (uint32_t number = 0; number < program.functions.size(); ++number) {
        const FunctionID id(number);
        const BodyIndex* index = facts.bodies().find(id);
        if (!index && program.at(id).hasBody()) {
            m_fields.clear();
            return;
        }
        if (!index || !index->holdsAnyOf(accesses))
            continue;
        users.append(id);
        if (index->holdsAnyOf(stores)) {
            forEachAccess(*index, stores, [&](const BodyValue& store) {
                noteStore({ store.layout(), store.field() }, *index, storedBy(*index, store));
            });
        }
    }
    UsesOfHeldRegExps::LiteralsOfFields literalsOfFields;
    for (auto& entry : m_fields) {
        if (entry.value)
            literalsOfFields.add(entry.key, static_cast<uint32_t>(literalsOfFields.size()));
    }
    if (literalsOfFields.isEmpty() || !isLockedMember(program, facts, BuiltinID(Builtins::RegExpPrototypeExec)) || !isLockedMember(program, facts, BuiltinID(Builtins::RegExpPrototypeTest)))
        return;
    BitVector escaped;
    for (FunctionID user : users) {
        const BodyIndex& index = *facts.bodies().find(user);
        HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> yielded;
        forEachAccess(index, accesses, [&](const BodyValue& access) {
            auto literal = literalsOfFields.find(keyOfField({ access.layout(), access.field() }));
            if (literal != literalsOfFields.end())
                yielded.add((access.opcode() == Opcode::PrivateGet ? access.id() : storedBy(index, access)).index(), literal->value);
        });
        if (yielded.isEmpty())
            continue;
        UsesOfHeldRegExps uses(program, index, escaped, &literalsOfFields);
        uses.visit([&](const BodyValue& value) -> std::optional<uint32_t> {
            auto literal = yielded.find(value.id().index());
            return literal == yielded.end() ? std::nullopt : std::optional<uint32_t>(literal->value);
        });
    }
    for (auto& [key, literal] : literalsOfFields)
        m_fields.find(key)->value->isPrivate = !escaped.get(literal);
}

void HeldRegExps::findVariables(const Program& program, const Facts& facts)
{
    std::optional<BitVector> mayBeFilledByRunTime;
    for (const Module& module : program.modules) {
        const BodyIndex* index = module.body ? facts.bodies().find(module.body) : nullptr;
        if (!index || !index->opcodes().contains(Opcode::NewRegExp) || !index->opcodes().contains(Opcode::PutModuleVariable))
            continue;
        if (!mayBeFilledByRunTime)
            mayBeFilledByRunTime = variablesThatRunTimeMayFill(program);
        for (uint32_t number = 0; number < index->numberOfValues(); ++number) {
            ValueID id(number);
            if (index->isParameter(id) || index->opcodeOf(id) != Opcode::PutModuleVariable)
                continue;
            auto store = index->value(id).as<Opcode::PutModuleVariable>();
            const Variable& variable = program.at(store.variable());
            const ValueID stored = index->stripCasts(store.valueOperand());
            if (index->isParameter(stored) || index->opcodeOf(stored) != Opcode::NewRegExp || variable.owner || !variable.flags.contains(VariableFlags::IsImmutable) || mayBeFilledByRunTime->get(store.variable().index()))
                continue;
            auto made = index->value(stored).as<Opcode::NewRegExp>();
            const HeldRegExp literal { made.pattern(), made.regExpFlags() };
            auto added = m_variables.add(store.variable().index(), literal);
            if (!added.isNewEntry && added.iterator->value != std::optional(literal))
                added.iterator->value = std::nullopt;
        }
    }
}

void HeldRegExps::findNames(const Program& program, const Facts& facts)
{
    static constexpr OpcodeSet stores { Opcode::PropSet, Opcode::PropDefine, Opcode::NewObject };
    for (uint32_t function = 0; function < program.functions.size(); ++function) {
        const BodyIndex* index = facts.bodies().find(FunctionID(function));
        if (!index || !index->opcodes().contains(Opcode::NewRegExp) || !index->holdsAnyOf(stores))
            continue;
        auto note = [&](PropertyKeyID key, ValueID value) {
            const ValueID stored = index->stripCasts(value);
            if (!key || index->isParameter(stored) || index->opcodeOf(stored) != Opcode::NewRegExp)
                return;
            auto made = index->value(stored).as<Opcode::NewRegExp>();
            const HeldRegExp literal { made.pattern(), made.regExpFlags(), false, true };
            auto added = m_names.add(key.index(), literal);
            if (!added.isNewEntry && added.iterator->value != std::optional(literal))
                added.iterator->value = std::nullopt;
        };
        for (uint32_t number = 0; number < index->numberOfValues(); ++number) {
            const ValueID id(number);
            if (index->isParameter(id) || !stores.contains(index->opcodeOf(id)))
                continue;
            const BodyValue store = index->value(id);
            switch (store.opcode()) {
            case Opcode::PropSet:
                note(store.as<Opcode::PropSet>().key(), store.as<Opcode::PropSet>().valueOperand());
                break;
            case Opcode::PropDefine:
                note(store.as<Opcode::PropDefine>().key(), store.as<Opcode::PropDefine>().valueOperand());
                break;
            default: {
                auto allocation = store.as<Opcode::NewObject>();
                const Layout& layout = program.at(allocation.layout());
                std::span<const ValueID> values = allocation.valuesOperands();
                for (size_t place = 0; !layout.isEnvironment() && place < values.size() && place < allocation.fields().size(); ++place)
                    note(layout.fields[allocation.fields()[place]].key, values[place]);
                break;
            }
            }
        }
    }
}

const HeldRegExp* HeldRegExps::findExpected(PropertyKeyID key) const
{
    auto found = m_names.find(key.index());
    return found != m_names.end() && found->value ? &*found->value : nullptr;
}

const HeldRegExp* HeldRegExps::find(VariableID variable) const
{
    auto found = m_variables.find(variable.index());
    return found != m_variables.end() && found->value ? &*found->value : nullptr;
}

const HeldRegExp* HeldRegExps::find(const FieldReference& field) const
{
    auto found = m_fields.find(keyOfField(field));
    return found != m_fields.end() && found->value ? &*found->value : nullptr;
}

const HeldRegExp* HeldRegExps::find(FunctionID function, uint32_t capture) const
{
    auto first = m_firstCaptures.find(function.index());
    if (first == m_firstCaptures.end())
        return nullptr;
    uint32_t place = first->value + capture;
    if (m_literalsOfCaptures[place] < firstLiteral)
        return nullptr;
    return &m_literals[m_literalsOfCaptures[place] - firstLiteral];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
