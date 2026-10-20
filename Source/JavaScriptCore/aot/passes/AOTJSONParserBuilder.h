/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONEmission.h"
#include "AOTSelectTypedResultsNodes.h"
#include "AOTValidationRules.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct ParsedValue {
    ValueID value;
    ValueID next;
};

struct ParseExits {
    BlockID rejected;
    BlockID unknown;
};

std::optional<Vector<uint8_t>> quotedIfPlain(std::span<const uint8_t> text);

class JSONParserBuilder final : public Emission {
public:
    JSONParserBuilder(IRBuilder&, const EmissionInputs&);

    ParsedValue emitNode(ResultNodeRef, ValueID cursor, const ParseExits&);
    ParsedValue emitNodeOrGenericValue(ResultNodeRef, ValueID cursor, const ParseExits&);
    ParsedValue emitNodeBody(ResultNodeRef, ValueID cursor, const ParseExits&);
    void emitRequireStackSpace(BlockID unknown);

    static DerivedSignature signatureOf(Emission&, const ResultNode&);

private:
    enum class MissingMember : uint8_t {
        Absent,
        Rejected,
        Default,
        Unknown,
    };

    struct MemberParse {
        const ResultMember* member { nullptr };
        StringID name;
        StringID quotedKey;
        StringID quotedKeyAndColon;
        TypeID valueType;
        MissingMember missing { MissingMember::Unknown };
        bool hasPlainKey { false };
    };

    struct ObjectState {
        Vector<ValueID, 8> values;
        Vector<ValueID, 8> seen;
        ValueID cursor;
        ValueID order;
        ValueID extra;
    };

    struct ObjectParse {
        const ResultNode& node;
        Vector<MemberParse, 8> members;
        BlockID anyOrder;
        BlockID closing;
        ParseExits exits;
    };

    struct ArrayLoop {
        ValueID array;
        BlockID header;
        BlockID done;
        ValueID cursorAtHeader;
        ValueID cursorAtDone;
    };

    ParsedValue emitCallOfParser(ResultNodeRef, const ResultNode&, ValueID cursor, const ParseExits&);
    ParsedValue emitInterpretedNode(ResultNodeRef, const ResultNode&, ValueID cursor, const ParseExits&);
    ParsedValue emitGenericValue(ValueID cursor, BlockID unknown);
    ParsedValue emitGenericValueFor(ResultNodeRef, const ResultNode&, ValueID cursor, const ParseExits&);

    ParsedValue emitScalar(const ResultNode&, ValueID cursor, const ParseExits&);
    ParsedValue emitNumber(const ResultNode&, ValueID cursor, const ParseExits&);
    ParsedValue emitString(const ResultNode&, ValueID cursor, const ParseExits&);
    ParsedValue emitLiteral(const ResultNode&, ValueID cursor, const ParseExits&);
    ParsedValue emitParsedOrExit(ValueID results, BlockID whenNotParsed);

    ValueID emitExpectText(StringView text, ValueID cursor, BlockID otherwise, bool isLikely);
    void emitRequireByte(ValueID cursor, uint8_t byte, BlockID otherwise);
    void emitBranchOnByte(ValueID cursor, uint8_t byte, BlockID whenEqual, BlockID otherwise);
    ValueID appendSkipPunctuator(ValueID cursor);
    ValueID appendSkipWhitespace(ValueID cursor);

    ParsedValue emitArray(const ResultNode&, ValueID cursor, const ParseExits&);
    ParsedValue emitTuple(const ResultNode&, ValueID cursor, const ParseExits&);
    ArrayLoop beginArrayLoop(const ResultNode&, ValueID afterBracket);
    void emitElementSeparator(ValueID next, const ArrayLoop&, BlockID rejected);

    ParsedValue emitObject(const ResultNode&, ValueID cursor, const ParseExits&);
    ParsedValue emitDictionary(const ResultNode&, ValueID cursor, const ParseExits&);
    Vector<MemberParse, 8> resolveMembers(const ResultNode&);
    MissingMember classifyMissingMember(const ResultMember&, const ResultNode& memberNode) const;
    ObjectState createInitialState(const ObjectParse&, ValueID firstKey);
    ObjectState bindState(const ObjectParse&, BlockID);
    void emitJumpWithState(BlockID, const ObjectState&);
    ObjectState emitOrderedMembers(const ObjectParse&, ObjectState);
    void emitUnorderedMembers(const ObjectParse&);
    ParsedValue emitObjectClosing(const ObjectParse&);
    void emitMember(const ObjectParse&, size_t index, ValueID valueStart, ObjectState&, BlockID whenMoreMembers);
    void emitMemberSeparator(const ObjectParse&, ValueID next, ObjectState&, BlockID whenMoreMembers);
    void emitMissingMember(const ObjectParse&, size_t index, ObjectState&);
    void emitStoreOfMemberIfSeen(const ObjectParse&, size_t index, const ObjectState&, ValueID object);
    void emitExtraMembers(const ObjectParse&, ObjectState&, BlockID whenMoreMembers);
    void emitValidatedExtraMember(const ObjectParse&, ObjectState&, BlockID whenMoreMembers);
    void emitRejectionOfMember(StringID name, ValueID valueStart, const ParseExits&);
    void emitRejectionOfKey(ValueID keyStart, ValueID valueStart, const ParseExits&);
    ValueID emitExpectColon(ValueID cursor, BlockID rejected);

    ParsedValue emitUnion(ResultNodeRef, const ResultNode&, ValueID cursor, const ParseExits&);
    ParsedValue emitArmsInOrder(std::span<const ResultNodeRef> arms, ValueID cursor, const ParseExits&);
    ParsedValue emitObjectArms(std::span<const ResultNodeRef> arms, ValueID cursor, const ParseExits&);
    ParsedValue emitArmsByDiscriminant(const ResultDiscriminant&, std::span<const ResultNodeRef> arms, ValueID cursor, const ParseExits&);

    NodeDecisionInputs decisionInputs() const { return { inputs().program, inputs().flow, inputs().build }; }

    ValidationRuleEmitter m_rules;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
