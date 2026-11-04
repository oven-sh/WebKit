/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkFlow.h"
#include "AOTPlan.h"
#include "AOTWalkStrings.h"
#include <optional>
#include <span>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

// ---- AOTWalkStringParserNodes.cpp. What a parser of JSON text makes for a written type, as pure functions of the program's tables, Facts and Plan. The walk writes the code: AOTWalkStringParsers.cpp.

struct ParserContext {
    const Program& program;
    const Facts& facts;
    const Plan& plan;
};

// Part 0 is the whole type. Part n is its n-th alternative.
struct ParserNodeRef {
    friend bool operator==(const ParserNodeRef&, const ParserNodeRef&) = default;

    TypeID type;
    uint32_t part { 0 };
};

enum class ParserShape : uint8_t {
    Unknown,
    Null,
    String,
    Number,
    Boolean,
    BooleanLiteral,
    StringLiteral,
    Array,
    Object,
    Union,
};

struct ParserLiteral {
    enum class Kind : uint8_t { Null, Boolean, String };

    friend bool operator==(const ParserLiteral&, const ParserLiteral&) = default;

    Kind kind { Kind::Null };
    bool boolean { false };
    StringID string;
};

struct ParserMember {
    PropertyKeyID key;
    ParserNodeRef node;
    uint32_t field { 0 };
    // The text may leave the member out, and the slot then stays absent.
    bool isOptional { false };
};

struct ParserNode {
    bool isContainer() const { return shape >= ParserShape::Array; }

    void forEachChild(NOESCAPE const Invocable<void(ParserNodeRef)> auto& visit) const
    {
        for (ParserNodeRef child : children)
            visit(child);
        for (const ParserMember& member : members)
            visit(member.node);
    }

    ParserShape shape { ParserShape::Unknown };
    TypeID type;
    ParserLiteral literal;
    LayoutID layout;
    Vector<ParserMember, 8> members;
    // Of an array: the node of its elements. Of a union: its arms.
    Vector<ParserNodeRef, 4> children;
};

ParserNode describeParserNode(const ParserContext&, ParserNodeRef);
// `User[] | null`: the only container among the alternatives of the type is an array, and the only container among those of its elements is an object. The layout that a parser would build that object in.
// Null for any other type.
LayoutID layoutOfObjectsInOnlyArrayOf(const Program&, TypeID);

// What JSON text can spell.
enum class JSONValueClass : uint8_t {
    Null = 1 << 0,
    Boolean = 1 << 1,
    Number = 1 << 2,
    String = 1 << 3,
    Array = 1 << 4,
    Object = 1 << 5,
};

OptionSet<JSONValueClass> valueClassesOf(const ParserContext&, ParserNodeRef);

struct ParserDiscriminant {
    PropertyKeyID key;
    Vector<ParserLiteral, 4> tagOfArm;
};

struct TaggedParserArms {
    ParserDiscriminant discriminant;
    Vector<ParserNodeRef, 4> tagged;
    Vector<ParserNodeRef, 4> untagged;
};

// A member whose types in at least two of the arms are literals that differ from one another. The member that tags every object among the arms is preferred.
std::optional<TaggedParserArms> findTaggedParserArms(const ParserContext&, std::span<const ParserNodeRef> arms);

// False: the value is parsed as an untyped one and then tested. A union that would look at one part of the text again and again, and an object whose layout no parser may make.
bool isParserNodeCompiled(const ParserContext&, ParserNodeRef);
// Plan has given the layout storage and a shape, and Facts says that parsers make objects of it: linked() claims nothing of such a layout.
bool parsersMayAllocate(const ParserContext&, LayoutID);
// The root is a container of which some part comes out typed.
bool hasTypedPartForParser(const ParserContext&, ParserNodeRef);

// A container that is the whole of a type has a function of its own, which Program::appendParsers() has made. Null: it has none.
FunctionID parserFunctionOf(const Program&, TypeID);
// What the parser of `root` and the parsers that it may call can make, by the tables alone.
struct ObjectsMadeByParsers {
    // Ascending, each once.
    Vector<LayoutID, 4> layouts;
    // The types of which one alternative is an array, `root` first, in the order in which Program::appendParsersFor() comes to them from `root`.
    Vector<TypeID, 4> typesWithArrays;
};
ObjectsMadeByParsers objectsMadeByParsersOf(const Program&, TypeID root);
// A parser fills the fields of which the first holds, each by the type that the second answers. It leaves every other field as the allocation makes it.
bool isMemberThatJSONSpells(const Program&, const Field&);
TypeID typeOfParsedMember(const Field&);

// ---- AOTWalkStringTypedResults.cpp.
// The type by which what the call gives is parsed: what the scan has left for the call (ThinSummary::typesParsedAtCalls), which is ExpectedResult::type where the call names a record. Null: none, or the two differ.
TypeID typeParsedAtCall(const Walker&, const BodyValue& call);
// For CallInWalkBuiltins::build(), in front of buildCallByOneRowForStrings(), of a row with hasTypedResult(). True: what stands for the call is built and bound. False: the call goes on as if nobody had asked.
bool buildTypedResultOfCallByOneRow(Walker&, const CallByOneRow&);
// For openGuessAtOperandOfBuiltinCall(), of an operand for which no row has code in line. `function load(text: string) { return JSON.parse(text) as Config; }`, entered by anybody: nothing is proven of the text,
// so the call goes by no one row and nobody asks for a parser. The type to test the operand for: behind the test the call is walked again, goes by the row with the typed result, and comes to the door above.
// Null: the operand is not the text of such a row, or no parser would be built for the call.
TypeID typeOfTextParsedByTypeAtCall(const Walker&, const BodyValue& call, BuiltinID, BuiltinOperand position);
// A link of Walker::walkThroughChain(), in front of callBuiltin(). `await sql<User[]>..`: a Call that names a record with IsRowsOfQuery. The one row with TypedResultForm::Rows has no native entry, so BuiltinCalls never plans such a call:
// it is built closed, as written, with ExpectRows(what it gives, layout) behind it. False: nothing was built.
inline constexpr OpcodeSet opcodesThatCallQueryThatExpectsRowsTakes { Opcode::Call };
AOT_LINK_OF_WALK(callQueryThatExpectsRows, opcodesThatCallQueryThatExpectsRowsTakes)

// ---- AOTWalkStringParsers.cpp. For Walker::walk(), of a unit whose function has FunctionRole::Parser: Code is reset and holds no block. False after a failure of the walk.
bool walkParserUnit(Walker&);

// ---- AOTWalkStringParserLoops.cpp. A loop of a parser unit, over Code's own doors. The types of the parameters are said by whoever makes it, and what enters and what goes round has to lie within them.
// Every block that is made while the loop is open is marked as one of the loop by whoever makes it. Only in a parser unit, whose walk keeps no state.
class BuiltLoopOfParsers {
    WTF_MAKE_NONCOPYABLE(BuiltLoopOfParsers);
public:
    BuiltLoopOfParsers(Walker&, std::span<const Reference> entering, std::span<const TypeID> typesOfParameters); // the walk stands in the head behind it
    Reference parameter(uint32_t index) const { return m_parameters[index]; }
    void goRound(std::span<const Reference> values); // ends the current block with the back edge
    void close(); // no more edges come round: Code::setLoopMarks()
private:
    bool leadToHead(std::span<const Reference> values);

    Walker& m_walker;
    BlockID m_head;
    Vector<Reference, 8> m_parameters;
    ValueID m_poll;
    bool m_standsInLoopAround { false };
    bool m_hasBackEdge { false };
};

// ---- facts/AOTLinkStringParsers.cpp. For callBuiltin() of AOTLinkCalls.cpp, of a row with a typed result at a call at which `parsed` is parsed. `result`: the node of what the call gives, or null where whoever asks gives anything himself.
// Step one: each layout of objectsMadeByParsersOf(parsed) that a parser may make is allocated and escapes, the parser and those that it calls are reached, `result` holds what is unseen. Null `parsed`: the row says too little, as today.
void stateObjectsMadeByParsersInLink(LinkFlow&, FunctionID caller, ValueID site, TypeID parsed, FlowNodeID result);

// ---- From one file of the package to another, and to nobody else.
// AOTWalkStringParserNodes.cpp. By the record of the layout alone: a plain record with names, none of whose fields is written as a function. No parser, of a type or of untyped values, makes an object of any other layout.
bool isRecordThatParsersCanMake(const Program&, LayoutID);
// The types that the parser of `type` hands on to parsers of their own where they have one: the contents of its array, the members of its objects.
void forEachTypeParsedBelow(const Program&, TypeID, NOESCAPE const ScopedLambda<void(TypeID)>&);
// The layouts in which the parser of `type` itself builds objects.
void forEachLayoutParsedAt(const Program&, TypeID, NOESCAPE const ScopedLambda<void(LayoutID)>&);
// The members that a text may not leave out by the tables alone, as orderOfFieldsInAllocationOfParser() has them: what the allocation of a parser lists where neither the plan nor Facts asks for more.
Vector<uint32_t, 8> fieldsThatTextMayNotLeaveOut(const Program&, LayoutID);
// By the place in the declaration, and by number where the declaration has no place for a field. The order of the text is written over the word of the allocation, so this one only has to be one that the plan has heard of.
void orderOfFieldsInAllocationOfParser(const Program&, LayoutID, Vector<uint32_t, 8>& fields);
// AOTWalkStringParserLoops.cpp. As Walker::convert(), which is private. Null: the walk stands nowhere or has failed.
Reference inFormForParsers(Walker&, Reference, Representation wanted, Origin);
// AOTWalkStringParsers.cpp. Open, the parser of the root, close. Whatever the root gives up on abandons the source and runs the call as written. Null: no way leads on.
Reference buildParseByTypeAtCall(Walker&, const CallByOneRow&, Reference text, FunctionID parser);
// AOTWalkStringTypedResults.cpp. The CallBuiltin of the row on the operands as they are settled: no conversion of an operand runs twice.
Reference buildCallByOneRowAsWritten(Walker&, const CallByOneRow&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
