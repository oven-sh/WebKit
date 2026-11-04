/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The parser of a written type, written out of the type: one unit for each container type, (Cursor) -> (JSONOutcome, value, Cursor), and the three operations at the call.
// That the text is of the written type is a guess. Whatever the code cannot take it answers with Rejected or Unknown, and only a failure of the root abandons the source and runs the call as written.
// A parser unit has no body behind it, so its blocks are made over Code's own doors: a join gets its block when it is entered, once all that arrives is known. The walk keeps no state in it.
// At a suspension: a parser never suspends. At a throw: nothing in a parser unit catches, and an open source that is left behind is overwritten by the next one.
// Withheld with BuiltinCalls, TypedOperations or Guesses: no call names a parser, so no parser unit is walked.

#include "config.h"
#include "AOTWalkStringParsers.h"

#if ENABLE(AOT)

#include "AOTFamilies.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTSort.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp, AOTWalkNumbers.cpp.
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);
Reference boxedUnlessNumbersAreUnboxed(Walker&, Reference, Origin);

namespace {

// In the order in which a union looks for the arms of a class.
constexpr std::pair<JSONValueClass, ASCIILiteral> firstBytesOfJSONValueClasses[] {
    { JSONValueClass::String, "\""_s },
    { JSONValueClass::Number, "-0123456789"_s },
    { JSONValueClass::Boolean, "tf"_s },
    { JSONValueClass::Null, "n"_s },
    { JSONValueClass::Array, "["_s },
    { JSONValueClass::Object, "{"_s },
};

using JoinID = ID<struct JoinOfParserUnitIDTag>;

class WalkOfParserUnit {
public:
    explicit WalkOfParserUnit(Walker& walker)
        : m_walker(walker)
        , m_code(walker.code())
        , m_types(walker.types())
        , m_context { walker.program(), walker.facts(), walker.plan() }
        , m_origin(walker.originOfOperation())
    {
    }

    struct Parsed {
        Reference value;
        Reference next;
    };

    struct Exits {
        // The text is none of the type.
        JoinID rejected;
        // This code cannot say: the value is left to the parser of untyped values.
        JoinID unknown;
        // Set where the object is one of several types that are tried in turn: a member that this type does not declare says that the text is of another.
        JoinID undeclaredMember { };
    };

    void walkUnit(ParserNodeRef);

private:
    enum class MissingMember : uint8_t { Absent, Rejected, Unknown };

    struct Member {
        const ParserMember* member { nullptr };
        const Field* field { nullptr };
        bool hasPlainName { false };
        bool canBeFilled { false };
        // What a value must be to lie in the slot.
        TypeID stored;
        MissingMember missing { MissingMember::Unknown };
        // In the order in which the program creates the fields.
        size_t placeAmongCreatedFields { 0 };
    };

    // The value of each member and whether the text had it, then the cursor, the key order and the extra members.
    struct ObjectState {
        Reference& value(size_t member) { return slots[2 * member]; }
        Reference& seen(size_t member) { return slots[2 * member + 1]; }
        Reference& cursor() { return slots[slots.size() - 3]; }
        Reference& order() { return slots[slots.size() - 2]; }
        Reference& extra() { return slots[slots.size() - 1]; }

        Vector<Reference, 19> slots;
    };

    struct ObjectParse {
        const ParserNode& node;
        Vector<Member, 8> members;
        // Where the ways meet on which a key was not the next one. Behind it stands the loop that takes the members in any order.
        JoinID notInOrder;
        JoinID closing;
        Exits exits;
        BuiltLoopOfParsers* anyOrder { nullptr };
    };

    // An edge that is open until the join is entered, and what it brings.
    struct Arrival {
        EdgeID edge;
        BlockFrequency frequency { BlockFrequency::Normal };
        Vector<Reference, 4> arguments;
    };

    struct Join {
        Vector<Arrival, 2> arrivals;
        // The block is no more frequent than this, whatever arrives.
        BlockFrequency frequencyAtMost { BlockFrequency::Normal };
    };

    class LoopScope {
    public:
        explicit LoopScope(WalkOfParserUnit& walk)
            : m_walk(walk)
        {
            ++m_walk.m_numberOfOpenLoops;
        }
        ~LoopScope() { --m_walk.m_numberOfOpenLoops; }

    private:
        WalkOfParserUnit& m_walk;
    };

    const Program& program() const { return m_context.program; }
    bool isUnreachable() const { return m_walker.isUnreachable(); }

    // ---- Small operations

    Draft draft(Opcode opcode) { return Draft(opcode, m_origin); }
    // Of a draft whose fields are set.
    Reference build(Draft&& operation)
    {
        if (isUnreachable())
            return { };
        m_walker.chooseConservativeFields(operation);
        return isUnreachable() ? Reference() : m_walker.build(WTF::move(operation));
    }

    Reference constantOf(Opcode opcode) { return m_walker.constant(draft(opcode)); }
    Reference boolean(bool value)
    {
        Draft constant = draft(Opcode::Boolean);
        constant.setBoolean(value);
        return m_walker.constant(WTF::move(constant));
    }
    Reference integer(int64_t value)
    {
        Draft constant = draft(Opcode::Integer);
        constant.setInteger(value);
        return m_walker.constant(WTF::move(constant));
    }
    Reference string(StringID text)
    {
        Draft constant = draft(Opcode::String);
        constant.setString(text);
        return m_walker.constant(WTF::move(constant));
    }
    Reference resultAt(Reference results, uint32_t index)
    {
        Draft read = draft(Opcode::ResultAt);
        read.setResultsOperand(results);
        read.setIndex(index);
        return build(WTF::move(read));
    }
    Reference onCursor(Opcode opcode, Reference cursor)
    {
        Draft operation = draft(opcode);
        operation.setCursorOperand(cursor);
        return build(WTF::move(operation));
    }
    Reference strictEqual(Reference left, Reference right)
    {
        if (isUnreachable())
            return { };
        Draft comparison = draft(Opcode::StrictEq);
        m_walker.chooseConservativeFields(comparison);
        comparison.setLeftOperand(left);
        comparison.setRightOperand(right);
        comparison.setForm(formAllowedForTypedOperator(m_walker, comparison));
        return m_walker.build(WTF::move(comparison));
    }
    Reference isByte(Reference cursor, uint8_t byte) { return strictEqual(onCursor(Opcode::JSONGetByte, cursor), integer(byte)); }
    Reference skipWhitespace(Reference cursor) { return onCursor(Opcode::JSONSkipWhitespace, cursor); }
    Reference skipPunctuator(Reference cursor) { return skipWhitespace(onCursor(Opcode::JSONSkipByte, cursor)); }
    Reference beginKeyOrder(LayoutID layout)
    {
        Draft begin = draft(Opcode::JSONObjectBegin);
        begin.setLayout(layout);
        return build(WTF::move(begin));
    }

    // ---- Control flow

    JoinID openJoin(BlockFrequency frequencyAtMost = BlockFrequency::Normal)
    {
        m_joins.append(Join { { }, frequencyAtMost });
        return JoinID(static_cast<uint32_t>(m_joins.size() - 1));
    }

    BlockFrequency frequencyHere() const { return m_code.at(m_walker.currentBlock()).frequency; }

    BlockID addBlock(BlockFrequency frequency)
    {
        BlockID block = m_code.addBlock(BlockRole::Ordinary, frequency);
        if (m_numberOfOpenLoops)
            m_code.setLoopMarks(block, true, false);
        return block;
    }

    void arrive(JoinID join, EdgeID edge, BlockFrequency frequency, std::span<const Reference> arguments)
    {
        Arrival arrival { edge, frequency, { } };
        arrival.arguments.append(arguments);
        m_joins[join.index()].arrivals.append(WTF::move(arrival));
    }

    // The first of the edges, all of them open. Null: the walk has failed.
    EdgeID endBlock(TerminatorDraft&& terminator, uint32_t numberOfEdges)
    {
        WantedRepresentations wanted = wantedRepresentationsOf(m_walker, terminator);
        for (uint32_t operandIndex = 0; operandIndex < terminator.numberOfOperands(); ++operandIndex) {
            if (!hasRegister(wanted[operandIndex]))
                continue;
            Reference converted = inFormForParsers(m_walker, terminator.operand(operandIndex), wanted[operandIndex], m_origin);
            if (!converted || isUnreachable())
                return { };
            terminator.setOperand(operandIndex, converted);
        }
        BlockID from = m_walker.currentBlock();
        m_code.end(from, terminator, numberOfEdges);
        return m_code.at(from).terminator.edge(0);
    }

    void jumpTo(JoinID join, std::span<const Reference> arguments)
    {
        if (isUnreachable())
            return;
        BlockFrequency frequency = frequencyHere();
        if (EdgeID edge = endBlock(TerminatorDraft(TerminatorKind::Jump, m_origin), 1))
            arrive(join, edge, frequency, arguments);
    }
    void jumpTo(JoinID join, std::initializer_list<Reference> arguments = { }) { jumpTo(join, std::span(arguments.begin(), arguments.size())); }
    void jumpTo(JoinID join, Parsed parsed) { jumpTo(join, { parsed.value, parsed.next }); }

    std::optional<bool> knownBoolean(Reference test) const
    {
        if (!test.isConstant() || m_code.at(test.id()).opcode != Opcode::Boolean)
            return std::nullopt;
        return m_code.at(test.id()).boolean();
    }

    // Goes on where the test answers `holds`, in a block that this edge alone enters. The other side goes to the join.
    void goOnWhere(Reference test, bool holds, JoinID otherwise, BlockFrequency frequencyOfOtherSide)
    {
        if (isUnreachable())
            return;
        if (std::optional<bool> known = knownBoolean(test)) {
            if (*known != holds)
                jumpTo(otherwise);
            return;
        }
        BlockFrequency frequency = frequencyHere();
        TerminatorDraft terminator(TerminatorKind::Branch, m_origin);
        terminator.setConditionOperand(test);
        EdgeID taken = endBlock(WTF::move(terminator), 2);
        if (!taken)
            return;
        EdgeID notTaken(taken.index() + 1);
        BlockID next = addBlock(frequency);
        m_code.closeEdge(holds ? taken : notTaken, next, { });
        arrive(otherwise, holds ? notTaken : taken, std::max(frequency, frequencyOfOtherSide), { });
        m_walker.continueIn(next);
    }
    void require(Reference test, JoinID otherwise, BlockFrequency frequencyOfOtherSide = BlockFrequency::Rare) { goOnWhere(test, true, otherwise, frequencyOfOtherSide); }
    void requireNot(Reference test, JoinID otherwise, BlockFrequency frequencyOfOtherSide = BlockFrequency::Rare) { goOnWhere(test, false, otherwise, frequencyOfOtherSide); }
    void requireByte(Reference cursor, uint8_t byte, JoinID otherwise) { require(isByte(cursor, byte), otherwise); }

    TypeID typeOfRecord(Reference reference) const { return m_code.at(reference.id()).type; }

    void closeArrival(const Arrival& arrival, BlockID to, std::span<const uint32_t> joined, std::span<const ValueID> parameters)
    {
        auto isHandedOverAsItIs = [&](size_t place) {
            return isSameWord(m_walker.representationOf(arrival.arguments[joined[place]]), m_code.at(parameters[place]).representation);
        };
        Vector<ValueID, 8> arguments;
        bool needsConversions = false;
        for (size_t place = 0; place < joined.size(); ++place) {
            needsConversions |= !isHandedOverAsItIs(place);
            arguments.append(arrival.arguments[joined[place]].id());
        }
        if (!needsConversions) {
            m_code.closeEdge(arrival.edge, to, arguments.span());
            return;
        }

        // The conversions stand on the edge, in a block of their own.
        BlockID onEdge = addBlock(arrival.frequency);
        m_code.closeEdge(arrival.edge, onEdge, { });
        m_walker.continueIn(onEdge);
        for (size_t place = 0; place < joined.size(); ++place) {
            if (isHandedOverAsItIs(place))
                continue;
            Reference converted = inFormForParsers(m_walker, arrival.arguments[joined[place]], m_code.at(parameters[place]).representation, m_origin);
            if (!converted || isUnreachable())
                return;
            arguments[place] = converted.id();
        }
        m_code.end(onEdge, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
        m_code.closeEdge(m_code.at(onEdge).terminator.next(), to, arguments.span());
    }

    // The walk stands nowhere. False: nothing arrives, and it still does.
    // What every arrival brings alike is handed on as it is: it is defined above all of them. Everything else gets a parameter, whose type is the join of what arrives.
    template<size_t inlineCapacity>
    bool enter(JoinID id, Vector<Reference, inlineCapacity>& results)
    {
        results.shrink(0);
        Vector<Arrival, 2> arrivals = std::exchange(m_joins[id.index()].arrivals, { });
        if (arrivals.isEmpty() || m_walker.hasFailed())
            return false;
        RELEASE_ASSERT(isUnreachable());

        size_t numberOfValues = arrivals[0].arguments.size();
        BlockFrequency frequency = BlockFrequency::Rare;
        for (const Arrival& arrival : arrivals) {
            RELEASE_ASSERT(arrival.arguments.size() == numberOfValues);
            frequency = std::min(frequency, arrival.frequency);
        }
        BlockID block = addBlock(std::max(frequency, m_joins[id.index()].frequencyAtMost));

        Vector<uint32_t, 8> joined;
        Vector<ValueID, 8> parameters;
        Vector<Representation, 4> forms;
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            Reference first = arrivals[0].arguments[index];
            bool allBringOneValue = std::ranges::all_of(arrivals, [&](const Arrival& arrival) {
                return arrival.arguments[index] == first;
            });
            if (allBringOneValue) {
                results.append(first);
                continue;
            }
            TypeID type = typeOfRecord(first);
            forms.shrink(0);
            for (const Arrival& arrival : arrivals) {
                TypeID arriving = typeOfRecord(arrival.arguments[index]);
                if (arriving != type)
                    type = joinOf(m_types, m_walker.facts().families(), type, arriving);
                forms.append(m_walker.representationOf(arrival.arguments[index]));
            }
            parameters.append(m_code.appendParameter(block, type, m_walker.formOfParameter(type, forms.span()), m_origin));
            joined.append(index);
            results.append(Reference::value(parameters.last()));
        }
        if (!parameters.isEmpty())
            m_code.setParameters(block, parameters.span());

        for (const Arrival& arrival : arrivals) {
            closeArrival(arrival, block, joined.span(), parameters.span());
            if (m_walker.hasFailed())
                return false;
        }
        m_walker.continueIn(block);
        return true;
    }
    bool enter(JoinID join)
    {
        Vector<Reference, 4> results;
        return enter(join, results);
    }
    Parsed enterParsed(JoinID join)
    {
        Vector<Reference, 4> results;
        if (!enter(join, results))
            return { };
        return { results[0], results[1] };
    }

    // The value under the tested type. Whatever is outside goes to the join.
    Reference requireType(Reference value, TypeID tested, JoinID otherwise)
    {
        if (isUnreachable() || m_types.isSubset(m_walker.typeOf(value), tested))
            return value;
        Draft test = draft(Opcode::TypeTest);
        test.setValueOperand(value);
        test.setTestedType(tested);
        Reference passes = build(WTF::move(test));
        require(passes, otherwise);
        if (isUnreachable() || !passes.isValue())
            return value;
        Draft proof = draft(Opcode::Narrowed);
        proof.setValueOperand(value);
        proof.setTestOperand(passes);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(true);
        return build(WTF::move(proof));
    }

    void leadEdges(EdgeID first, BlockFrequency frequency, std::span<const JoinID> targets, JoinID otherwise)
    {
        if (!first)
            return;
        for (size_t index = 0; index <= targets.size(); ++index)
            arrive(index < targets.size() ? targets[index] : otherwise, EdgeID(first.index() + static_cast<uint32_t>(index)), frequency, { });
    }
    void switchOn(Reference subject, std::span<const int64_t> cases, std::span<const JoinID> targets, JoinID otherwise)
    {
        if (isUnreachable())
            return;
        BlockFrequency frequency = frequencyHere();
        TerminatorDraft terminator(TerminatorKind::Switch, m_origin);
        terminator.setSubjectOperand(subject);
        terminator.setCases(cases);
        leadEdges(endBlock(WTF::move(terminator), static_cast<uint32_t>(cases.size() + 1)), frequency, targets, otherwise);
    }
    void switchOnString(Reference subject, std::span<const StringID> cases, std::span<const JoinID> targets, JoinID otherwise)
    {
        if (isUnreachable())
            return;
        BlockFrequency frequency = frequencyHere();
        TerminatorDraft terminator(TerminatorKind::SwitchString, m_origin);
        terminator.setSubjectOperand(subject);
        terminator.setStrings(cases);
        leadEdges(endBlock(WTF::move(terminator), static_cast<uint32_t>(cases.size() + 1)), frequency, targets, otherwise);
    }

    // ---- Texts

    bool isPlainText(StringID text) const
    {
        return std::ranges::all_of(program().string(text), [](uint8_t byte) { return byte >= 0x20 && byte <= 0x7E && byte != '"' && byte != '\\'; });
    }

    struct Text {
        StringID string;
        JSONWord word { JSONWord::None };
        bool isFollowedByColon { false };
    };
    static Text wordOf(const ParserLiteral& literal) { return { { }, literal.kind == ParserLiteral::Kind::Null ? JSONWord::Null : literal.boolean ? JSONWord::True : JSONWord::False }; }

    void fill(Draft& operation, const Text& text, Reference cursor)
    {
        if (text.string)
            operation.setString(text.string);
        operation.setLiteralWord(text.word);
        operation.setIsQuoted(!!text.string);
        operation.setIsFollowedByColon(text.isFollowedByColon);
        operation.setCursorOperand(cursor);
    }
    Reference startsWith(const Text& text, Reference cursor, bool isLikely = false)
    {
        Draft test = draft(Opcode::JSONStartsWith);
        fill(test, text, cursor);
        test.setIsLikely(isLikely);
        return build(WTF::move(test));
    }
    // The cursor behind the text. Where the text does not stand there the other side goes to the join.
    Reference expectText(const Text& text, Reference cursor, JoinID otherwise, bool isLikely)
    {
        require(startsWith(text, cursor, isLikely), otherwise, isLikely ? BlockFrequency::Unlikely : BlockFrequency::Normal);
        Draft advance = draft(Opcode::JSONAdvance);
        fill(advance, text, cursor);
        return build(WTF::move(advance));
    }
    Reference expectColon(Reference cursor, JoinID rejected)
    {
        Reference colon = skipWhitespace(cursor);
        requireByte(colon, ':', rejected);
        return skipPunctuator(colon);
    }

    // ---- Types

    TypeID testableTypeOf(const ParserNode& node) { return m_types.testablePartOf(node.type); }

    // ---- Values

    Parsed parseBy(Opcode opcode, Reference cursor, JoinID whenNotParsed)
    {
        Reference results = onCursor(opcode, cursor);
        require(resultAt(results, 0), whenNotParsed);
        return { resultAt(results, 1), resultAt(results, 2) };
    }

    // Goes on where the outcome is Parsed.
    void branchOnOutcome(Reference outcome, JoinID rejected, JoinID unknown)
    {
        if (isUnreachable())
            return;
        BlockFrequency frequency = frequencyHere();
        int64_t cases[] { static_cast<int64_t>(JSONOutcome::Parsed), static_cast<int64_t>(JSONOutcome::Rejected) };
        TerminatorDraft terminator(TerminatorKind::Switch, m_origin);
        terminator.setSubjectOperand(outcome);
        terminator.setCases(cases);
        EdgeID first = endBlock(WTF::move(terminator), 3);
        if (!first)
            return;
        BlockID parsed = addBlock(frequency);
        m_code.closeEdge(first, parsed, { });
        arrive(rejected, EdgeID(first.index() + 1), std::max(frequency, BlockFrequency::Unlikely), { });
        arrive(unknown, EdgeID(first.index() + 2), BlockFrequency::Rare, { });
        m_walker.continueIn(parsed);
    }

    Parsed parseGenericValueFor(const ParserNode& node, Reference cursor, const Exits& exits)
    {
        Parsed generic = parseBy(Opcode::JSONParseValue, cursor, exits.unknown);
        return { requireType(generic.value, testableTypeOf(node), exits.unknown), generic.next };
    }

    Parsed parseNode(ParserNodeRef ref, Reference cursor, const Exits& exits)
    {
        ParserNode node = describeParserNode(m_context, ref);
        if (!isParserNodeCompiled(m_context, ref))
            return parseGenericValueFor(node, cursor, exits);
        FunctionID function = node.isContainer() && !ref.part ? parserFunctionOf(program(), ref.type) : FunctionID();
        if (!function)
            return parseNodeBody(ref, cursor, exits);
        // What a parser may make is asked layout by layout (parsersMayAllocate()). Here: whether the queue comes to the unit from this one, and whether there is such a unit.
        const LinkTables& linked = m_context.facts.linked();
        if (!linked.function(function).isReached || !linked.mayBeMentionedBy(m_walker.unit().function, function) || !m_context.plan.conventions().exists(CodeUnit { function, CodeRole::Body }))
            return parseGenericValueFor(node, cursor, exits);

        Draft call = draft(Opcode::JSONParseNode);
        call.setFunction(function);
        call.setCursorOperand(cursor);
        Reference results = build(WTF::move(call));
        branchOnOutcome(resultAt(results, 0), exits.rejected, exits.unknown);
        return { requireType(resultAt(results, 1), testableTypeOf(node), exits.unknown), resultAt(results, 2) };
    }

    // One odd element costs one element: it is parsed again from its own start as an untyped value, tested and joined back.
    Parsed parseNodeOrGenericValue(ParserNodeRef ref, Reference cursor, const Exits& exits)
    {
        ParserNode node = describeParserNode(m_context, ref);
        if (node.shape == ParserShape::Unknown || !isParserNodeCompiled(m_context, ref))
            return parseNode(ref, cursor, exits);

        JoinID merge = openJoin();
        JoinID again = openJoin();
        jumpTo(merge, parseNode(ref, cursor, { again, again }));
        if (enter(again))
            jumpTo(merge, parseGenericValueFor(node, cursor, exits));
        return enterParsed(merge);
    }

    Parsed parseNodeBody(ParserNodeRef ref, Reference cursor, const Exits& exits)
    {
        ParserNode node = describeParserNode(m_context, ref);
        switch (node.shape) {
        case ParserShape::Unknown:
            return parseBy(Opcode::JSONParseValue, cursor, exits.unknown);
        case ParserShape::Boolean: {
            JoinID merge = openJoin();
            JoinID isNotTrue = openJoin();
            Reference afterTrue = expectText({ { }, JSONWord::True }, cursor, isNotTrue, false);
            jumpTo(merge, { boolean(true), afterTrue });
            if (enter(isNotTrue)) {
                Reference afterFalse = expectText({ { }, JSONWord::False }, cursor, exits.rejected, false);
                jumpTo(merge, { boolean(false), afterFalse });
            }
            return enterParsed(merge);
        }
        case ParserShape::Number:
            return parseNumber(node, cursor, exits);
        case ParserShape::String:
            return parseString(cursor, exits);
        case ParserShape::Null:
        case ParserShape::BooleanLiteral:
        case ParserShape::StringLiteral:
            return parseLiteral(node.literal, cursor, exits);
        case ParserShape::Array:
            return parseArray(node, cursor, exits);
        case ParserShape::Object:
            return parseObject(node, cursor, exits);
        case ParserShape::Union:
            return parseUnion(node, cursor, exits);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The numbers of the type alone: an integer type reads digits into an integer, which never sees -0, a fraction or an exponent.
    Parsed parseNumber(const ParserNode& node, Reference cursor, const Exits& exits)
    {
        TypeID numbers = m_types.intersectionOf(testableTypeOf(node), TypeArena::fromPrimitives(Primitive::Number));
        Reference results = onCursor(m_types.at(numbers).isInteger() ? Opcode::JSONParseInteger : Opcode::JSONParseNumber, cursor);
        JoinID done = openJoin();
        JoinID notParsed = openJoin();
        require(resultAt(results, 0), notParsed);
        Parsed parsed { resultAt(results, 1), resultAt(results, 2) };
        parsed.value = requireType(parsed.value, numbers, exits.unknown);
        if (!isUnreachable())
            parsed.value = boxedUnlessNumbersAreUnboxed(m_walker, parsed.value, m_origin);
        jumpTo(done, parsed);

        // A number that this code does not read is still a number: the text is not of another type.
        if (enter(notParsed)) {
            Vector<int64_t, 11> starts;
            Vector<JoinID, 11> targets;
            for (char byte : "-0123456789"_s.span()) {
                starts.append(byte);
                targets.append(exits.unknown);
            }
            switchOn(onCursor(Opcode::JSONGetByte, cursor), starts.span(), targets.span(), exits.rejected);
        }
        return enterParsed(done);
    }

    Parsed parseString(Reference cursor, const Exits& exits)
    {
        requireByte(cursor, '"', exits.rejected);
        return parseBy(Opcode::JSONParseString, cursor, exits.unknown);
    }

    Parsed parseLiteral(const ParserLiteral& literal, Reference cursor, const Exits& exits)
    {
        switch (literal.kind) {
        case ParserLiteral::Kind::Null:
            return { constantOf(Opcode::Null), expectText(wordOf(literal), cursor, exits.rejected, false) };
        case ParserLiteral::Kind::Boolean:
            return { boolean(literal.boolean), expectText(wordOf(literal), cursor, exits.rejected, false) };
        case ParserLiteral::Kind::String:
            break;
        }
        Reference constant = string(literal.string);
        JoinID merge = openJoin();
        JoinID escapedSpelling = openJoin();
        if (isPlainText(literal.string))
            jumpTo(merge, { expectText({ literal.string }, cursor, escapedSpelling, true) });
        else
            jumpTo(escapedSpelling);
        if (enter(escapedSpelling)) {
            Parsed parsed = parseString(cursor, exits);
            require(strictEqual(parsed.value, constant), exits.rejected);
            jumpTo(merge, { parsed.next });
        }
        Vector<Reference, 4> results;
        if (!enter(merge, results))
            return { };
        return { constant, results[0] };
    }

    // ---- Arrays

    Parsed parseArray(const ParserNode& node, Reference cursor, const Exits& exits)
    {
        requireByte(cursor, '[', exits.rejected);
        Reference afterBracket = skipPunctuator(cursor);
        Draft allocation = draft(Opcode::NewArray);
        allocation.setContents(node.children[0].type);
        Reference array = build(WTF::move(allocation));
        afterBracket = appendElementsParsedBefore(node.children[0], array, afterBracket);
        JoinID done = openJoin();
        JoinID hasElements = openJoin();
        require(isByte(afterBracket, ']'), hasElements, BlockFrequency::Normal);
        jumpTo(done, { afterBracket });
        if (enter(hasElements)) {
            TypeID cursorType = m_walker.typeOf(afterBracket);
            LoopScope blocksLieInLoop(*this);
            BuiltLoopOfParsers loop(m_walker, singleElementSpan(afterBracket), singleElementSpan(cursorType));
            Parsed element = isUnreachable() ? Parsed() : parseNodeOrGenericValue(node.children[0], loop.parameter(0), exits);
            if (!isUnreachable()) {
                Draft append = draft(Opcode::AppendElement);
                append.setArrayOperand(array);
                append.setValueOperand(element.value);
                build(WTF::move(append));
                Reference afterElement = skipWhitespace(element.next);
                JoinID hasNoComma = openJoin();
                require(isByte(afterElement, ','), hasNoComma, BlockFrequency::Normal);
                Reference nextElement = skipPunctuator(afterElement);
                loop.goRound(singleElementSpan(nextElement));
                if (enter(hasNoComma)) {
                    requireByte(afterElement, ']', exits.rejected);
                    jumpTo(done, { afterElement });
                }
            }
            loop.close();
        }
        Vector<Reference, 4> results;
        if (!enter(done, results))
            return { };
        return { array, onCursor(Opcode::JSONSkipByte, results[0]) };
    }

    // The elements are objects of one layout. Those at the head of the list whose text has a form that the run time has parsed before are appended by it, in one loop of its own.
    // The cursor behind them: at the first element that is left to this code, or at the closing bracket.
    Reference appendElementsParsedBefore(ParserNodeRef element, Reference array, Reference firstElement)
    {
        ParserNode node = describeParserNode(m_context, element);
        if (node.shape == ParserShape::Union) {
            Vector<ParserNodeRef, 4> objects;
            for (ParserNodeRef arm : node.children) {
                if (valueClassesOf(m_context, arm).contains(JSONValueClass::Object))
                    objects.append(arm);
            }
            if (objects.size() != 1)
                return firstElement;
            element = objects[0];
            node = describeParserNode(m_context, element);
        }
        if (node.shape != ParserShape::Object || !isParserNodeCompiled(m_context, element) || isUnreachable())
            return firstElement;
        Draft members = draft(Opcode::JSONParseExtraMembers);
        members.setLayout(node.layout);
        members.setCursorOperand(firstElement);
        members.setExtraOperand(constantOf(Opcode::Undefined));
        members.setOrderOperand(beginKeyOrder(node.layout));
        members.setListOperand(array);
        return resultAt(build(WTF::move(members)), 2);
    }

    // ---- Objects

    Vector<Member, 8> resolveMembers(const ParserNode& node)
    {
        const Layout& layout = program().at(node.layout);
        Vector<Member, 8> members;
        for (const ParserMember& member : node.members) {
            const Field& field = layout.fields[member.field];
            const PlacedSlot& slot = m_context.plan.slot(node.layout, member.field);
            Member parse { .member = &member, .field = &field, .hasPlainName = isPlainText(program().at(member.key).text) };
            // What passes the test must lie in the slot. A slot that takes nothing of what JSON spells gives up, and so does whatever is no plain property with a word of its own:
            // JSON.parse() makes a property that can be written, listed and deleted, and nobody has shown that a member without storage is never looked for.
            TypeID stored = slot.typeInLayoutRecord() ? slot.typeInLayoutRecord() : TypeTable::anything;
            TypeID tested = m_types.testablePartOf(stored);
            constexpr uint16_t spelledByJSON = Primitive::Null | Primitive::Boolean | Primitive::Number | Primitive::AnyString;
            bool takesSomething = tested && (m_types.at(tested).hasObjects() || m_types.at(tested).primitives & spelledByJSON);
            bool isPlainProperty = field.form.isEmpty() && !slot.holdsAccessor && !slot.deferredFunction;
            bool formFollowsFromType = slot.representation == SlotRepresentation::JSValue || slot.typeInLayoutRecord();
            parse.canBeFilled = isPlainProperty && slot.hasStorage() && formFollowsFromType && takesSomething && m_types.isSubset(tested, stored);
            parse.stored = tested;
            parse.missing = member.isOptional ? MissingMember::Absent : MissingMember::Rejected;
            if (!member.isOptional && (!field.declared.written() || program().at(member.node.type).primitives & Primitive::Undefined))
                parse.missing = MissingMember::Unknown;
            members.append(parse);
        }
        // A text is most likely written from an object that the program's own code has made, or else by somebody who read the declaration of the type.
        // The order decides how soon a key is found and nothing else: whatever comes in another order is taken by parseMembersInAnyOrder().
        auto placeIn = [](std::span<const uint32_t> order, uint32_t field) {
            return static_cast<size_t>(std::ranges::find(order, field) - order.begin());
        };
        std::span<const uint32_t> declared = layout.declaredOrder.span();
        AOT::stableSort(members, { }, [&](const Member& parse) {
            return placeIn(declared, parse.member->field);
        });
        std::span<const FieldIndex> created = orderInWhichProgramCreatesFields(node.layout);
        if (created.empty())
            return members;
        // A member that the order leaves out stays behind the member that the declaration has in front of it.
        size_t place = 0;
        for (Member& parse : members) {
            if (size_t found = placeIn(created, parse.member->field); found < created.size())
                place = found;
            parse.placeAmongCreatedFields = place;
        }
        AOT::stableSort(members, { }, &Member::placeAmongCreatedFields);
        return members;
    }

    // The first order of the layout that stores of the program give. Empty: every order of the plan follows the declaration.
    std::span<const FieldIndex> orderInWhichProgramCreatesFields(LayoutID layout) const
    {
        const KeyOrders& orders = m_context.plan.keyOrders();
        for (uint32_t order = 0; order < orders.numberOfOrdersOf(layout); ++order) {
            if (std::ranges::contains(orders.sourcesOfFields(layout, order), KeyOrderSource::StoresInSequence))
                return orders.order(layout, order);
        }
        return { };
    }

    // The allocation of the object lists its value. Every other member is stored behind the allocation, where the text has it.
    static bool isListedByAllocation(const Member& member) { return member.missing != MissingMember::Absent && member.canBeFilled; }

    Reference keyOrderWith(const ObjectParse& parse, const Member& member, Reference order)
    {
        Draft step = draft(Opcode::JSONObjectKey);
        step.setOrderOperand(order);
        step.setLayout(parse.node.layout);
        step.setField(member.member->field);
        return build(WTF::move(step));
    }

    Reference memberRepeats(PropertyKeyID key, Reference valueStart)
    {
        Draft search = draft(Opcode::JSONMemberRepeats);
        search.setKey(key);
        search.setValueStartOperand(valueStart);
        return build(WTF::move(search));
    }
    // A member that comes again replaces the one before it, so the text is rejected for a value only where its key does not repeat.
    void rejectUnlessMemberRepeats(PropertyKeyID key, Reference valueStart, const Exits& exits)
    {
        if (isUnreachable())
            return;
        require(memberRepeats(key, valueStart), exits.rejected, BlockFrequency::Normal);
        jumpTo(exits.unknown);
    }

    // The members in front of this one came in their order, so each of them that the text may not leave out was seen. An object that ends here lacks every member behind it:
    // where one of those may not be left out, closeObject() would leave by this exit, and nothing of the state is needed to say so. Null: the object may end here.
    JoinID exitOfObjectThatEndsInOrderBehind(const ObjectParse& parse, size_t index) const
    {
        for (size_t later = index + 1; later < parse.members.size(); ++later) {
            switch (parse.members[later].missing) {
            case MissingMember::Absent:
                break;
            case MissingMember::Rejected:
                return parse.exits.rejected;
            case MissingMember::Unknown:
                return parse.exits.unknown;
            }
        }
        return { };
    }

    // Leaves the walk behind the separator, in front of the next key. An object that ends goes to `whenObjectEnds`, or with its state to `closing`.
    void parseMember(const ObjectParse& parse, size_t index, Reference valueStart, ObjectState& state, Reference cameBefore = { }, JoinID whenObjectEnds = { })
    {
        const Member& member = parse.members[index];
        if (!member.canBeFilled) {
            jumpTo(parse.exits.unknown);
            return;
        }
        JoinID valueRejected = openJoin();
        JoinID accepted = openJoin();
        jumpTo(accepted, parseNodeOrGenericValue(member.member->node, valueStart, { valueRejected, parse.exits.unknown }));
        if (enter(valueRejected))
            rejectUnlessMemberRepeats(member.member->key, valueStart, parse.exits);
        Parsed parsed = enterParsed(accepted);
        if (isUnreachable())
            return;

        state.value(index) = requireType(parsed.value, member.stored, parse.exits.unknown);
        state.seen(index) = boolean(true);
        // A member that comes again keeps the place of the first among the keys.
        if (!cameBefore)
            state.order() = keyOrderWith(parse, member, state.order());
        else {
            JoinID met = openJoin();
            JoinID repeats = openJoin();
            requireNot(cameBefore, repeats);
            jumpTo(met, { keyOrderWith(parse, member, state.order()) });
            if (enter(repeats))
                jumpTo(met, { state.order() });
            Vector<Reference, 4> order;
            if (!enter(met, order))
                return;
            state.order() = order[0];
        }

        Reference separator = skipWhitespace(parsed.next);
        JoinID more = openJoin();
        require(isByte(separator, '}'), more, BlockFrequency::Normal);
        state.cursor() = separator;
        if (whenObjectEnds)
            jumpTo(whenObjectEnds);
        else
            jumpTo(parse.closing, state.slots.span());
        if (!enter(more))
            return;
        requireByte(separator, ',', parse.exits.rejected);
        Reference key = skipPunctuator(separator);
        requireByte(key, '"', parse.exits.rejected);
        state.cursor() = key;
    }

    // Each member where resolveMembers() expects it. At the first key that is not the next one the rest is taken in any order.
    void parseMembersInOrder(const ObjectParse& parse, ObjectState& state)
    {
        for (size_t index = 0; index < parse.members.size() && !isUnreachable(); ++index) {
            const Member& member = parse.members[index];
            bool mayBeMissing = member.missing == MissingMember::Absent;
            if (!member.hasPlainName || !member.canBeFilled) {
                if (mayBeMissing)
                    continue;
                return;
            }
            ObjectState stateAtMiss = state;
            JoinID miss = openJoin();
            JoinID next = openJoin();
            Reference afterKey = expectText({ program().at(member.member->key).text, JSONWord::None, true }, state.cursor(), miss, true);
            parseMember(parse, index, skipWhitespace(afterKey), state, { }, exitOfObjectThatEndsInOrderBehind(parse, index));
            jumpTo(next, { state.value(index), state.seen(index), state.cursor(), state.order() });
            if (enter(miss)) {
                if (mayBeMissing)
                    jumpTo(next, { stateAtMiss.value(index), stateAtMiss.seen(index), stateAtMiss.cursor(), stateAtMiss.order() });
                else
                    jumpTo(parse.notInOrder, stateAtMiss.slots.span());
            }
            Vector<Reference, 4> merged;
            if (!enter(next, merged))
                return;
            state.value(index) = merged[0];
            state.seen(index) = merged[1];
            state.cursor() = merged[2];
            state.order() = merged[3];
        }
    }

    enum class ExtraMembersRequest : bool { RunOfMembers, WholeObject };

    void parseExtraMembers(const ObjectParse& parse, ObjectState& state, JoinID whenNothingWasParsed, ExtraMembersRequest request = ExtraMembersRequest::RunOfMembers)
    {
        Draft members = draft(Opcode::JSONParseExtraMembers);
        members.setLayout(parse.node.layout);
        members.setCursorOperand(state.cursor());
        members.setExtraOperand(state.extra());
        members.setOrderOperand(state.order());
        members.setParsesWholeObject(request == ExtraMembersRequest::WholeObject);
        Reference results = build(WTF::move(members));
        branchOnOutcome(resultAt(results, 0), whenNothingWasParsed, parse.exits.unknown);
        state.extra() = resultAt(results, 1);
        state.cursor() = resultAt(results, 2);
        state.order() = resultAt(results, 3);
    }

    // The keys do not come as expected. Where the run time has parsed an object of the same form before, it builds this one in one loop of its own, from the opening brace on, and what was parsed of it so far is dropped.
    // The walk goes on where it has not.
    void parseObjectAsParsedBefore(const ObjectParse& parse, Reference brace, JoinID done)
    {
        JoinID wasNotParsedBefore = openJoin();
        ObjectState whole;
        whole.slots.appendList({ brace, beginKeyOrder(parse.node.layout), constantOf(Opcode::Null) });
        parseExtraMembers(parse, whole, wasNotParsedBefore, ExtraMembersRequest::WholeObject);
        Reference object = requireType(whole.extra(), testableTypeOf(parse.node), parse.exits.unknown);
        jumpTo(done, { object, onCursor(Opcode::JSONSkipByte, whole.cursor()) });
        enter(wasNotParsedBefore);
    }

    // The walk stands at the head of the loop, whose parameters are the state.
    void parseMembersInAnyOrder(const ObjectParse& parse, ObjectState state)
    {
        const Exits& exits = parse.exits;
        JoinID more = openJoin();
        require(isByte(state.cursor(), '}'), more, BlockFrequency::Normal);
        jumpTo(parse.closing, state.slots.span());
        if (!enter(more))
            return;
        requireByte(state.cursor(), '"', exits.rejected);
        // A key with escapes may spell any name. Where a member that is not declared decides between types, nothing is decided by such a key.
        // Elsewhere the run time reads it among the members that are not declared, and gives up where it spells a name that is.
        bool hasNamesThatNeedEscapes = parse.members.containsIf([](const Member& member) { return !member.hasPlainName; });
        JoinID keyHasEscapes;
        auto branchOnEscapesInKey = [&](JoinID whenKeyHasEscapes) {
            requireNot(onCursor(Opcode::JSONKeyHasEscapes, state.cursor()), whenKeyHasEscapes);
        };
        if (exits.undeclaredMember)
            branchOnEscapesInKey(exits.unknown);

        auto parseMatchedMember = [&](size_t index, Reference valueStart) {
            ObjectState stateOfMember = state;
            parseMember(parse, index, valueStart, stateOfMember, state.seen(index));
            parse.anyOrder->goRound(stateOfMember.slots.span());
        };
        for (size_t index = 0; index < parse.members.size() && !isUnreachable(); ++index) {
            if (!parse.members[index].hasPlainName)
                continue;
            JoinID nextTest = openJoin();
            Reference afterKey = expectText({ program().at(parse.members[index].member->key).text }, state.cursor(), nextTest, false);
            parseMatchedMember(index, expectColon(afterKey, exits.rejected));
            enter(nextTest);
        }
        if (hasNamesThatNeedEscapes && !exits.undeclaredMember && !isUnreachable()) {
            keyHasEscapes = openJoin();
            branchOnEscapesInKey(keyHasEscapes);
        }
        if (hasNamesThatNeedEscapes && !isUnreachable()) {
            Parsed key = parseBy(Opcode::JSONParseKey, state.cursor(), exits.unknown);
            Reference valueStart = expectColon(key.next, exits.rejected);
            for (size_t index = 0; index < parse.members.size() && !isUnreachable(); ++index) {
                if (parse.members[index].hasPlainName)
                    continue;
                JoinID nextTest = openJoin();
                require(strictEqual(key.value, string(program().at(parse.members[index].member->key).text)), nextTest, BlockFrequency::Normal);
                parseMatchedMember(index, valueStart);
                enter(nextTest);
            }
        }
        if (exits.undeclaredMember) {
            jumpTo(exits.undeclaredMember);
            return;
        }
        if (keyHasEscapes) {
            jumpTo(keyHasEscapes);
            enter(keyHasEscapes);
        }
        if (isUnreachable())
            return;
        parseExtraMembers(parse, state, exits.unknown);
        parse.anyOrder->goRound(state.slots.span());
    }

    Parsed closeObject(const ObjectParse& parse)
    {
        LayoutID layout = parse.node.layout;
        ObjectState state;
        if (!enter(parse.closing, state.slots))
            return { };
        RELEASE_ASSERT(state.slots.size() == 2 * parse.members.size() + 3);
        for (size_t index = 0; index < parse.members.size(); ++index) {
            switch (parse.members[index].missing) {
            case MissingMember::Absent:
                break;
            case MissingMember::Rejected:
                require(state.seen(index), parse.exits.rejected);
                break;
            case MissingMember::Unknown:
                require(state.seen(index), parse.exits.unknown);
                break;
            }
        }
        Reference next = onCursor(Opcode::JSONSkipByte, state.cursor());

        Vector<uint32_t, 8> fields;
        for (const Member& member : parse.members) {
            if (isListedByAllocation(member))
                fields.append(member.member->field);
        }
        orderOfFieldsInAllocationOfParser(program(), layout, fields);
        Vector<Reference, 8> values;
        for (uint32_t field : fields) {
            size_t index = parse.members.findIf([&](const Member& member) { return member.member->field == field; });
            // The join has united the value with the undefined of the ways on which the member was not seen, and those have left.
            values.append(requireType(state.value(index), parse.members[index].stored, parse.exits.unknown));
        }
        if (isUnreachable())
            return { };
        // Walker::chooseConservativeFields() keeps of the list what fits the slots and has an order word, and defines the rest by name. JSONObjectFinish writes the order of the text over the word.
        Draft allocation = draft(Opcode::NewObject);
        allocation.setLayout(layout);
        allocation.setFields(fields.span());
        allocation.setValuesOperands(values.span());
        Reference object = build(WTF::move(allocation));

        // A field exists when its slot is filled, so the slot of a member that the text leaves out stays as the allocation made it: absent.
        for (size_t index = 0; index < parse.members.size() && !isUnreachable(); ++index) {
            const Member& member = parse.members[index];
            if (member.missing != MissingMember::Absent || !member.canBeFilled)
                continue;
            JoinID behind = openJoin();
            require(state.seen(index), behind, BlockFrequency::Normal);
            Draft store = draft(Opcode::PropDefine);
            store.setObjectOperand(object);
            store.setKey(member.member->key);
            store.setValueOperand(requireType(state.value(index), member.stored, parse.exits.unknown));
            build(WTF::move(store));
            jumpTo(behind);
            enter(behind);
        }
        if (isUnreachable())
            return { };

        Draft finish = draft(Opcode::JSONObjectFinish);
        finish.setOrderOperand(state.order());
        finish.setObjectOperand(object);
        finish.setExtraOperand(state.extra());
        finish.setLayout(layout);
        branchOnOutcome(build(WTF::move(finish)), parse.exits.unknown, parse.exits.unknown);
        return { object, next };
    }

    Parsed parseObject(const ParserNode& node, Reference cursor, const Exits& exits)
    {
        requireByte(cursor, '{', exits.rejected);
        if (isUnreachable())
            return { };
        Reference firstKey = skipPunctuator(cursor);
        ObjectParse parse { node, resolveMembers(node), openJoin(), openJoin(), exits };
        JoinID done = openJoin();

        Reference undefined = constantOf(Opcode::Undefined);
        ObjectState state;
        Vector<TypeID, 19> typesOfState;
        for (const Member& member : parse.members) {
            state.slots.appendList({ undefined, boolean(false) });
            typesOfState.append(m_types.unionOf(member.stored && member.canBeFilled ? member.stored : TypeTable::anything, TypeTable::undefined));
            typesOfState.append(TypeTable::boolean);
        }
        state.slots.appendList({ firstKey, beginKeyOrder(node.layout), undefined });
        typesOfState.appendList({ m_walker.typeOf(firstKey), m_walker.typeOf(state.order()), TypeTable::anything });

        parseMembersInOrder(parse, state);
        jumpTo(parse.notInOrder, state.slots.span());
        ObjectState entering;
        bool takesMembersInAnyOrder = enter(parse.notInOrder, entering.slots);
        // Where several types are tried in turn, a member that this one does not declare says that the text is of another. The run time would take it.
        if (takesMembersInAnyOrder && !exits.undeclaredMember) {
            parseObjectAsParsedBefore(parse, cursor, done);
            takesMembersInAnyOrder = !isUnreachable();
        }
        if (takesMembersInAnyOrder) {
            LoopScope blocksLieInLoop(*this);
            BuiltLoopOfParsers anyOrder(m_walker, entering.slots.span(), typesOfState.span());
            if (!isUnreachable()) {
                parse.anyOrder = &anyOrder;
                ObjectState stateInLoop;
                for (uint32_t index = 0; index < typesOfState.size(); ++index)
                    stateInLoop.slots.append(anyOrder.parameter(index));
                parseMembersInAnyOrder(parse, WTF::move(stateInLoop));
            }
            anyOrder.close();
        }
        jumpTo(done, closeObject(parse));
        return enterParsed(done);
    }

    // ---- Unions

    struct ClassGroup {
        Vector<ParserNodeRef, 4> arms;
        OptionSet<JSONValueClass> classes;
        JoinID join;
    };

    Vector<ClassGroup, 6> groupArmsByClass(std::span<const ParserNodeRef> arms)
    {
        Vector<OptionSet<JSONValueClass>, 4> classesOfArm(arms.size(), [&](size_t index) { return valueClassesOf(m_context, arms[index]); });
        Vector<ClassGroup, 6> groups;
        for (auto [valueClass, firstBytes] : firstBytesOfJSONValueClasses) {
            Vector<ParserNodeRef, 4> candidates;
            for (size_t index = 0; index < arms.size(); ++index) {
                if (classesOfArm[index].contains(valueClass))
                    candidates.append(arms[index]);
            }
            if (candidates.isEmpty())
                continue;
            size_t position = groups.findIf([&](const ClassGroup& group) { return group.arms == candidates; });
            if (position == notFound) {
                position = groups.size();
                groups.append(ClassGroup { WTF::move(candidates), { }, { } });
            }
            groups[position].classes.add(valueClass);
        }
        return groups;
    }

    std::optional<Vector<StringID, 8>> stringLiteralsOf(std::span<const ParserNodeRef> arms)
    {
        Vector<StringID, 8> literals;
        for (ParserNodeRef arm : arms) {
            ParserNode node = describeParserNode(m_context, arm);
            if (node.shape != ParserShape::StringLiteral)
                return std::nullopt;
            literals.appendIfNotContains(node.literal.string);
        }
        return literals;
    }

    // The quoted text is compared where it stands. Only an escaped spelling is made a string.
    void parseStringLiteralArms(std::span<const StringID> literals, JoinID merge, Reference cursor, const Exits& exits)
    {
        for (StringID literal : literals) {
            if (!isPlainText(literal) || isUnreachable())
                continue;
            JoinID nextLiteral = openJoin();
            Reference next = expectText({ literal }, cursor, nextLiteral, false);
            jumpTo(merge, { string(literal), next });
            enter(nextLiteral);
        }
        if (isUnreachable())
            return;
        Parsed text = parseBy(Opcode::JSONParseString, cursor, exits.unknown);
        Vector<JoinID, 8> matches(literals.size(), [&](size_t) { return openJoin(); });
        switchOnString(text.value, literals, matches.span(), exits.rejected);
        for (size_t index = 0; index < literals.size(); ++index) {
            if (enter(matches[index]))
                jumpTo(merge, { string(literals[index]), text.next });
        }
    }

    Parsed parseArmsInOrder(std::span<const ParserNodeRef> arms, Reference cursor, const Exits& exits)
    {
        if (arms.empty()) {
            jumpTo(exits.rejected);
            return { };
        }
        if (arms.size() == 1)
            return parseNode(arms[0], cursor, exits);

        JoinID merge = openJoin();
        for (const ParserNodeRef& arm : arms) {
            bool isLast = &arm == &arms.back();
            JoinID rejected = isLast ? exits.rejected : openJoin();
            jumpTo(merge, parseNode(arm, cursor, { rejected, exits.unknown, rejected }));
            if (!isLast && !enter(rejected))
                break;
        }
        return enterParsed(merge);
    }

    // The member that tells the arms apart is looked up ahead in the text, and only the arm with its value is parsed.
    Parsed parseArmsByDiscriminant(const ParserDiscriminant& discriminant, std::span<const ParserNodeRef> arms, Reference cursor, const Exits& exits)
    {
        using Kind = ParserLiteral::Kind;
        std::span<const ParserLiteral> tags = discriminant.tagOfArm.span();
        RELEASE_ASSERT(arms.size() == tags.size());
        // Behind the switch the walk stands nowhere until a join is entered, so what the ways behind it read is built in front of it.
        Draft find = draft(Opcode::JSONFindMember);
        find.setKey(discriminant.key);
        find.setCursorOperand(skipPunctuator(cursor));
        Reference member = build(WTF::move(find));
        Reference search = resultAt(member, 0);
        Reference tagValue = resultAt(member, 1);

        JoinID merge = openJoin();
        JoinID noTagMatches = openJoin();
        Vector<JoinID, 4> armJoins(arms.size(), [&](size_t) { return openJoin(); });
        JoinID found = openJoin();
        int64_t searches[] { static_cast<int64_t>(JSONMemberSearch::Found), static_cast<int64_t>(JSONMemberSearch::Absent) };
        JoinID targets[] { found, exits.rejected };
        switchOn(search, searches, targets, exits.unknown);

        if (enter(found)) {
            auto jumpIfStartsWith = [&](const Text& text, JoinID target) {
                requireNot(startsWith(text, tagValue), target, BlockFrequency::Normal);
            };
            for (size_t index = 0; index < tags.size(); ++index) {
                if (tags[index].kind == Kind::String && isPlainText(tags[index].string))
                    jumpIfStartsWith({ tags[index].string }, armJoins[index]);
            }
            for (size_t index = 0; index < tags.size(); ++index) {
                if (tags[index].kind != Kind::String)
                    jumpIfStartsWith(wordOf(tags[index]), armJoins[index]);
            }
            if (std::ranges::contains(tags, Kind::String, &ParserLiteral::kind)) {
                require(isByte(tagValue, '"'), noTagMatches, BlockFrequency::Normal);
                Reference text = parseBy(Opcode::JSONParseString, tagValue, exits.unknown).value;
                Vector<StringID, 4> cases;
                Vector<JoinID, 4> caseTargets;
                for (size_t index = 0; index < tags.size(); ++index) {
                    if (tags[index].kind == Kind::String) {
                        cases.append(tags[index].string);
                        caseTargets.append(armJoins[index]);
                    }
                }
                switchOnString(text, cases.span(), caseTargets.span(), noTagMatches);
            } else
                jumpTo(noTagMatches);
        }
        for (size_t index = 0; index < arms.size(); ++index) {
            if (enter(armJoins[index]))
                jumpTo(merge, parseNode(arms[index], cursor, { noTagMatches, exits.unknown }));
        }
        if (enter(noTagMatches))
            rejectUnlessMemberRepeats(discriminant.key, tagValue, exits);
        return enterParsed(merge);
    }

    Parsed parseObjectArms(std::span<const ParserNodeRef> arms, Reference cursor, const Exits& exits)
    {
        std::optional<TaggedParserArms> split = findTaggedParserArms(m_context, arms);
        if (!split)
            return parseArmsInOrder(arms, cursor, exits);
        if (split->untagged.isEmpty())
            return parseArmsByDiscriminant(split->discriminant, split->tagged.span(), cursor, exits);

        JoinID merge = openJoin();
        JoinID untagged = openJoin();
        jumpTo(merge, parseArmsByDiscriminant(split->discriminant, split->tagged.span(), cursor, { untagged, exits.unknown }));
        if (enter(untagged))
            jumpTo(merge, parseArmsInOrder(split->untagged.span(), cursor, exits));
        return enterParsed(merge);
    }

    // The first byte says which class of value stands there, and with it which arms can take it.
    Parsed parseUnion(const ParserNode& node, Reference cursor, const Exits& exits)
    {
        Vector<ClassGroup, 6> groups = groupArmsByClass(node.children.span());
        if (groups.isEmpty())
            return parseArmsInOrder({ }, cursor, exits);
        if (groups.size() == 1 && groups[0].arms.size() == 1)
            return parseNode(groups[0].arms[0], cursor, exits);

        JoinID merge = openJoin();
        Vector<int64_t, 16> cases;
        Vector<JoinID, 16> targets;
        for (ClassGroup& group : groups) {
            group.join = openJoin();
            for (JSONValueClass valueClass : group.classes) {
                for (char byte : std::ranges::find(firstBytesOfJSONValueClasses, valueClass, &std::pair<JSONValueClass, ASCIILiteral>::first)->second.span()) {
                    cases.append(byte);
                    targets.append(group.join);
                }
            }
        }
        switchOn(onCursor(Opcode::JSONGetByte, cursor), cases.span(), targets.span(), exits.rejected);

        for (const ClassGroup& group : groups) {
            if (!enter(group.join))
                continue;
            std::optional<Vector<StringID, 8>> literals;
            if (group.arms.size() > 1)
                literals = stringLiteralsOf(group.arms.span());
            if (literals)
                parseStringLiteralArms(literals->span(), merge, cursor, exits);
            else if (group.classes.contains(JSONValueClass::Object))
                jumpTo(merge, parseObjectArms(group.arms.span(), cursor, exits));
            else
                jumpTo(merge, parseArmsInOrder(group.arms.span(), cursor, exits));
        }
        return enterParsed(merge);
    }

    void returnOutcome(JSONOutcome outcome, Reference value, Reference next)
    {
        if (isUnreachable())
            return;
        Reference values[] { integer(static_cast<int64_t>(outcome)), value, next };
        m_walker.endWithReturn(values, m_origin);
    }

    Walker& m_walker;
    Code& m_code;
    TypeArena& m_types;
    ParserContext m_context;
    Origin m_origin;
    Vector<Join, 32> m_joins;
    unsigned m_numberOfOpenLoops { 0 };
};

void WalkOfParserUnit::walkUnit(ParserNodeRef ref)
{
    const Function& function = program().at(m_walker.unit().function);
    BlockID entry = m_walker.addBlock();
    Reference cursor = m_walker.addParameter(entry, program().provenTypeOf(function.parameters[0]), m_context.plan.conventions().parameterRepresentation(program(), m_walker.unit(), 0));
    m_code.addEntry(entry);
    m_walker.continueIn(entry);

    Exits exits { openJoin(BlockFrequency::Rare), openJoin(BlockFrequency::Rare) };
    require(build(draft(Opcode::JSONHasStackSpace)), exits.unknown);
    Parsed parsed = isParserNodeCompiled(m_context, ref) ? parseNodeBody(ref, cursor, exits) : parseGenericValueFor(describeParserNode(m_context, ref), cursor, exits);
    returnOutcome(JSONOutcome::Parsed, parsed.value, parsed.next);
    if (enter(exits.rejected))
        returnOutcome(JSONOutcome::Rejected, constantOf(Opcode::Null), cursor);
    if (enter(exits.unknown))
        returnOutcome(JSONOutcome::Unknown, constantOf(Opcode::Null), cursor);

    bool everyJoinWasEntered = std::ranges::all_of(m_joins, [](const Join& join) {
        return join.arrivals.isEmpty();
    });
    if (!everyJoinWasEntered && !m_walker.hasFailed())
        m_walker.fail("an edge of a parser leads to a join that nobody has entered"_s);
}

} // anonymous namespace

bool walkParserUnit(Walker& walker)
{
    const Function& function = walker.program().at(walker.unit().function);
    RELEASE_ASSERT(function.role == FunctionRole::Parser && walker.unit().role == CodeRole::Body);
    WalkOfParserUnit(walker).walkUnit(ParserNodeRef { function.thisType.expected().predicted() });
    return !walker.hasFailed();
}

// In a body, so by the Walker's own blocks: each of them meets the states of what arrives.
Reference buildParseByTypeAtCall(Walker& walker, const CallByOneRow& row, Reference text, FunctionID parser)
{
    Origin origin = row.origin;
    origin.isTailCall = false;
    auto build = [&](Draft&& operation) {
        walker.chooseConservativeFields(operation);
        return walker.build(WTF::move(operation));
    };
    auto resultAt = [&](Reference results, uint32_t index) {
        Draft read(Opcode::ResultAt, origin);
        read.setResultsOperand(results);
        read.setIndex(index);
        return build(WTF::move(read));
    };
    auto onCursor = [&](Opcode opcode, Reference cursor) {
        Draft operation(opcode, origin);
        operation.setCursorOperand(cursor);
        return build(WTF::move(operation));
    };

    BlockID opened = walker.addBlock();
    BlockID parsed = walker.addBlock();
    BlockID closed = walker.addBlock();
    BlockID abandon = walker.addBlock(BlockFrequency::Rare);
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID done = walker.addBlock();

    Draft open(Opcode::JSONSourceOpen, origin);
    open.setTextOperand(text);
    walker.branch(build(WTF::move(open)), opened, asWritten);

    walker.continueIn(opened);
    Draft parse(Opcode::JSONParseNode, origin);
    parse.setFunction(parser);
    parse.setCursorOperand(onCursor(Opcode::JSONSkipWhitespace, build(Draft(Opcode::JSONSourceStart, origin))));
    Reference results = build(WTF::move(parse));
    Draft outcomeOfParsed(Opcode::Integer, origin);
    outcomeOfParsed.setInteger(static_cast<int64_t>(JSONOutcome::Parsed));
    Draft isParsed(Opcode::StrictEq, origin);
    walker.chooseConservativeFields(isParsed);
    isParsed.setLeftOperand(resultAt(results, 0));
    isParsed.setRightOperand(walker.constant(WTF::move(outcomeOfParsed)));
    if (walker.isUnreachable())
        return { };
    isParsed.setForm(formAllowedForTypedOperator(walker, isParsed));
    walker.branch(walker.build(WTF::move(isParsed)), parsed, abandon);

    // Text behind the value is an error, which the call as written reports.
    walker.continueIn(parsed);
    walker.branch(onCursor(Opcode::JSONSourceClose, resultAt(results, 2)), closed, asWritten);
    walker.continueIn(closed);
    Reference byType = resultAt(results, 1);
    BlockID endOfParse = walker.currentBlock();

    walker.continueIn(abandon);
    build(Draft(Opcode::JSONSourceAbandon, origin));
    walker.jumpTo(asWritten, { });
    Reference called = buildCallByOneRowAsWritten(walker, row);
    if (walker.hasFailed() || !byType)
        return { };
    BlockID endOfCall = called ? walker.currentBlock() : BlockID();

    const Code& code = walker.code();
    TypeID type = called ? joinOf(walker.types(), walker.facts().families(), code.at(byType.id()).type, code.at(called.id()).type) : code.at(byType.id()).type;
    Representation forms[] { walker.representationOf(byType), called ? walker.representationOf(called) : walker.representationOf(byType) };
    Reference result = walker.addParameter(done, type, walker.formOfParameter(type, forms));
    walker.continueIn(endOfParse);
    walker.jumpTo(done, singleElementSpan(byType));
    if (endOfCall) {
        walker.continueIn(endOfCall);
        walker.jumpTo(done, singleElementSpan(called));
    }
    walker.continueIn(done);
    return walker.hasFailed() ? Reference() : result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
