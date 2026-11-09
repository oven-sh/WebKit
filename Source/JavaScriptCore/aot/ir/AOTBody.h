/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTExternalFunctionBodies.h"
#include "AOTID.h"
#include "AOTOpcode.h"
#include "AOTSchemaRecords.h"
#include "AOTSchemaSupport.h"
#include "AOTTypeKinds.h"
#include "AOTWire.h"
#include "IterationKind.h"
#include "TypedArrayType.h"
#include <array>
#include <bit>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

inline constexpr BlockID entryBlock { 0u };

template<typename IDType> inline constexpr RemappedIDKind remappedIDKindOf = RemappedIDKind::String;
template<> inline constexpr RemappedIDKind remappedIDKindOf<PropertyKeyID> = RemappedIDKind::PropertyKey;
template<> inline constexpr RemappedIDKind remappedIDKindOf<TypeID> = RemappedIDKind::Type;
template<> inline constexpr RemappedIDKind remappedIDKindOf<TypeSiteID> = RemappedIDKind::TypeSite;
template<> inline constexpr RemappedIDKind remappedIDKindOf<ExpectedResultID> = RemappedIDKind::ExpectedResult;
template<> inline constexpr RemappedIDKind remappedIDKindOf<LayoutID> = RemappedIDKind::Layout;
template<> inline constexpr RemappedIDKind remappedIDKindOf<VariableID> = RemappedIDKind::Variable;
template<> inline constexpr RemappedIDKind remappedIDKindOf<ModuleID> = RemappedIDKind::Module;
template<> inline constexpr RemappedIDKind remappedIDKindOf<ModuleFailureID> = RemappedIDKind::ModuleFailure;
template<> inline constexpr RemappedIDKind remappedIDKindOf<EvalScopeID> = RemappedIDKind::EvalScope;
template<> inline constexpr RemappedIDKind remappedIDKindOf<FunctionID> = RemappedIDKind::Function;
template<> inline constexpr RemappedIDKind remappedIDKindOf<DataID> = RemappedIDKind::Data;

template<typename IDType>
ALWAYS_INLINE IDType programIDOf(const UnitIDRemap& unit, uint32_t word)
{
    static_assert(std::is_same_v<IDType, StringID> || remappedIDKindOf<IDType> != RemappedIDKind::String);
    const IDRemapTable& table = unit.tables[static_cast<size_t>(remappedIDKindOf<IDType>)];
    ASSERT(word < table.size);
    return IDType::fromBits(table.entries[word] - 1);
}

template<typename IDType>
ALWAYS_INLINE IDType optionalProgramIDOf(const UnitIDRemap& unit, uint32_t word)
{
    return word == Wire::none ? IDType() : programIDOf<IDType>(unit, word);
}

template<typename T> struct BodyElement;

template<typename Tag>
struct BodyElement<ID<Tag>> {
    static constexpr uint32_t numberOfWords = 1;
    static ID<Tag> decode(const uint32_t* words, const UnitIDRemap& unit) { return programIDOf<ID<Tag>>(unit, words[0]); }
};

template<>
struct BodyElement<int64_t> {
    static constexpr uint32_t numberOfWords = 2;
    static int64_t decode(const uint32_t* words, const UnitIDRemap&) { return static_cast<int64_t>(static_cast<uint64_t>(words[1]) << 32 | words[0]); }
};

template<>
struct BodyElement<double> {
    static constexpr uint32_t numberOfWords = 2;
    static double decode(const uint32_t* words, const UnitIDRemap&) { return std::bit_cast<double>(static_cast<uint64_t>(words[1]) << 32 | words[0]); }
};

#include "AOTSchemaBodyElements.inc"

template<typename T>
class BodyList {
public:
    static constexpr uint32_t wordsPerElement = BodyElement<T>::numberOfWords;

    class Iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = T;

        Iterator() = default;
        Iterator(const uint32_t* words, const UnitIDRemap* unit)
            : m_words(words)
            , m_unit(unit)
        {
        }

        T operator*() const { return BodyElement<T>::decode(m_words, *m_unit); }
        Iterator& operator++()
        {
            m_words += wordsPerElement;
            return *this;
        }
        Iterator operator++(int)
        {
            Iterator result = *this;
            ++*this;
            return result;
        }
        bool operator==(const Iterator& other) const { return m_words == other.m_words; }

    private:
        const uint32_t* m_words { nullptr };
        const UnitIDRemap* m_unit { nullptr };
    };

    BodyList() = default;
    BodyList(const uint32_t* elements, uint32_t size, const UnitIDRemap* unit)
        : m_elements(elements)
        , m_unit(unit)
        , m_size(size)
    {
    }

    uint32_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }
    T operator[](uint32_t index) const
    {
        ASSERT(index < m_size);
        return BodyElement<T>::decode(m_elements + index * wordsPerElement, *m_unit);
    }
    Iterator begin() const { return { m_elements, m_unit }; }
    Iterator end() const { return { m_elements + m_size * wordsPerElement, m_unit }; }

private:
    const uint32_t* m_elements { nullptr };
    const UnitIDRemap* m_unit { nullptr };
    uint32_t m_size { 0 };
};

class BodyFields {
public:
    BodyFields() = default;
    BodyFields(const uint32_t* fields, const UnitIDRemap* unit, uint16_t flags)
        : m_fields(fields)
        , m_unit(unit)
        , m_flags(flags)
    {
    }

    const uint32_t* words() const { return m_fields; }
    const UnitIDRemap& unit() const { return *m_unit; }
    uint16_t flags() const { return m_flags; }

    uint32_t word(uint32_t offset) const { return m_fields[offset]; }

    static_assert(!ValueID::fromBits(Wire::none) && sizeof(ValueID) == sizeof(uint32_t));
    ValueID operandAt(uint32_t offset) const { return ValueID::fromBits(word(offset)); }
    std::span<const ValueID> operandsAt(uint32_t offsetOfCount) const { return unsafeMakeSpan(reinterpret_cast<const ValueID*>(m_fields + offsetOfCount + 1), word(offsetOfCount)); }

    template<typename IDType> IDType idAt(uint32_t offset) const { return programIDOf<IDType>(*m_unit, word(offset)); }
    template<typename IDType> IDType optionalIDAt(uint32_t offset) const { return optionalProgramIDOf<IDType>(*m_unit, word(offset)); }
    template<typename T> T plainAt(uint32_t offset) const { return decodePayloadWord<T>(word(offset)); }
    int64_t int64At(uint32_t offset) const { return BodyElement<int64_t>::decode(m_fields + offset, *m_unit); }
    double doubleAt(uint32_t offset) const { return BodyElement<double>::decode(m_fields + offset, *m_unit); }
    template<typename Record> Record recordAt(uint32_t offset) const { return BodyElement<Record>::decode(m_fields + offset, *m_unit); }

    std::span<const uint32_t> numbersAt(uint32_t offsetOfCount) const { return unsafeMakeSpan(m_fields + offsetOfCount + 1, word(offsetOfCount)); }
    template<typename T> BodyList<T> listAt(uint32_t offsetOfCount) const { return { m_fields + offsetOfCount + 1, word(offsetOfCount), m_unit }; }

private:
    const uint32_t* m_fields { nullptr };
    const UnitIDRemap* m_unit { nullptr };
    uint16_t m_flags { 0 };
};

template<auto entry> class BodyOperation;

struct ConstructionStart {
    uint32_t reported { 0 };
    uint32_t start { 0 };
    bool isWrittenAsCall { false };
    bool isForCallers { false };
    bool isShownAtStart { false };

    friend bool operator==(const ConstructionStart&, const ConstructionStart&) = default;
};

enum class WrittenPositionRole : uint8_t {
    None,
    WhereWritten,
    StartOfConstruction,
    StartForCallers,
    WhereListIsWritten,
};

JS_EXPORT_PRIVATE WrittenPositionRole writtenPositionRoleOf(uint32_t opcodeWord, uint16_t flags);

constexpr uint8_t hasFlagsWordOnWire(Opcode opcode) { return !!wireFlagsOf(opcode); }

inline uint32_t numberOfFieldWords(Opcode, const uint32_t* fields);
inline uint32_t numberOfFieldWords(TerminatorKind, const uint32_t* fields);
inline uint32_t numberOfWordsBehindHead(Opcode, const uint32_t* words);

class BodyValue {
public:
    static constexpr uint32_t noPosition = UINT32_MAX;
    static constexpr uint32_t opcodeMask = ~(Wire::WrittenPositionFollows | Wire::StandsInTailPosition | Wire::WrittenPositionIsForCallers);
    static constexpr uint32_t numberOfHeadWords = sizeof(Wire::ValueHead) / sizeof(uint32_t);

    BodyValue() = default;
    static BodyValue operation(const uint32_t* head, const UnitIDRemap* unit, ValueID id) { return { head, unit, id, false }; }
    static BodyValue parameter(const uint32_t* parameterHead, const UnitIDRemap* unit, ValueID id) { return { parameterHead, unit, id, true }; }

    explicit operator bool() const { return m_words; }
    ValueID id() const { return m_id; }
    bool isParameter() const { return m_isParameter; }
    const uint32_t* words() const { return m_words; }
    const UnitIDRemap& unit() const { return *m_unit; }

    Opcode opcode() const { return m_opcode; }
    TypeID parameterType() const
    {
        ASSERT(m_isParameter);
        return optionalProgramIDOf<TypeID>(*m_unit, m_words[0]);
    }

    uint16_t flags() const { return hasFlagsWord() ? static_cast<uint16_t>(m_words[numberOfHeadWords]) : 0; }
    ALWAYS_INLINE BodyFields fieldWords() const
    {
        ASSERT(!m_isParameter);
        bool hasFlags = hasFlagsWord();
        return { m_words + numberOfHeadWords + hasFlags, m_unit, hasFlags ? static_cast<uint16_t>(m_words[numberOfHeadWords]) : static_cast<uint16_t>(0) };
    }
    ALWAYS_INLINE uint16_t flagsWord() const
    {
        ASSERT(hasFlagsWord());
        return static_cast<uint16_t>(m_words[numberOfHeadWords]);
    }
    ALWAYS_INLINE BodyFields fieldWordsBehindFlagsWord() const { return { m_words + numberOfHeadWords + 1, m_unit, flagsWord() }; }
    ALWAYS_INLINE BodyFields fieldWordsBehindHead() const
    {
        ASSERT(!m_isParameter && !hasFlagsWord());
        return { m_words + numberOfHeadWords, m_unit, 0 };
    }
    ALWAYS_INLINE BodyFields wordsBehindHead(uint32_t numberOfWordsPassedOver = 0) const
    {
        ASSERT(!m_isParameter);
        return { m_words + numberOfHeadWords + numberOfWordsPassedOver, m_unit, 0 };
    }

    uint32_t sizeInWords() const
    {
        if (m_isParameter)
            return sizeof(Wire::ParameterHead) / sizeof(uint32_t);
        return numberOfHeadWords + numberOfWordsBehindHead(opcode(), m_words + numberOfHeadWords) + hasSecondPosition();
    }

    uint32_t position() const
    {
        if (m_isParameter)
            return noPosition;
        if (!hasSecondPosition()) [[likely]]
            return m_words[1];
        return writtenPositionRole() == WrittenPositionRole::WhereListIsWritten ? secondPosition() : m_words[1];
    }
    bool hasPosition() const { return position() != noPosition; }
    bool standsInTailPosition() const { return !m_isParameter && m_words[0] & Wire::StandsInTailPosition; }
    WrittenPositionRole writtenPositionRole() const { return hasSecondPosition() ? writtenPositionRoleOf(m_words[0], flags()) : WrittenPositionRole::None; }
    bool isPartOfConstruction() const
    {
        switch (writtenPositionRole()) {
        case WrittenPositionRole::None:
        case WrittenPositionRole::WhereWritten:
            return false;
        case WrittenPositionRole::StartOfConstruction:
        case WrittenPositionRole::StartForCallers:
        case WrittenPositionRole::WhereListIsWritten:
            return true;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    ALWAYS_INLINE std::optional<uint32_t> writtenPosition() const
    {
        if (writtenPositionRole() != WrittenPositionRole::WhereWritten)
            return std::nullopt;
        return secondPosition();
    }
    JS_EXPORT_PRIVATE std::optional<ConstructionStart> constructionStart() const;
    ALWAYS_INLINE Origin origin() const
    {
        if (!hasSecondPosition()) [[likely]]
            return { .position = m_isParameter ? noPosition : m_words[1], .inlineFrame = 0, .isTailCall = standsInTailPosition(), .isPartOfConstruction = false };
        return originWithSecondPosition();
    }

    template<Opcode wanted>
    BodyOperation<wanted> as() const
    {
        ASSERT(opcode() == wanted);
        constexpr bool hasFlags = hasFlagsWordOnWire(wanted);
        return BodyOperation<wanted>(BodyFields { m_words + numberOfHeadWords + hasFlags, m_unit, hasFlags ? static_cast<uint16_t>(m_words[numberOfHeadWords]) : static_cast<uint16_t>(0) });
    }

    template<typename Functor> void forEachOperand(const Functor&) const;
    ValueID operand(uint32_t operandIndex) const;
    std::span<const ValueID> variadicOperands() const;
    template<typename Visitor> void forEachReference(Visitor&&) const;

    uint32_t numberOfVariadicOperands() const { return static_cast<uint32_t>(variadicOperands().size()); }
    OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(opcode(), operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(opcode(), operandIndex); }

#include "AOTSchemaBodyValueAccessors.inc"

private:
    BodyValue(const uint32_t* words, const UnitIDRemap* unit, ValueID id, bool isParameter)
        : m_words(words)
        , m_unit(unit)
        , m_id(id)
        , m_opcode(isParameter ? Opcode::Parameter : static_cast<Opcode>(words[0] & opcodeMask))
        , m_isParameter(isParameter)
    {
    }

    static_assert(!hasFlagsWordOnWire(Opcode::Parameter));
    ALWAYS_INLINE bool hasFlagsWord() const { return CaseTable<Opcode, hasFlagsWordOnWire>::at(m_opcode); }
    bool hasSecondPosition() const { return !m_isParameter && m_words[0] & Wire::WrittenPositionFollows; }
    uint32_t secondPosition() const { return m_words[sizeInWords() - 1]; }
    JS_EXPORT_PRIVATE Origin originWithSecondPosition() const;
    ValueID operandFoundByCase(uint32_t operandIndex) const;
    std::span<const ValueID> variadicOperandsFoundByCase() const;
    std::span<const ValueID> variadicOperandsBy(uint64_t outline) const;

    const uint32_t* m_words { nullptr };
    const UnitIDRemap* m_unit { nullptr };
    ValueID m_id;
    Opcode m_opcode { Opcode::Parameter };
    bool m_isParameter { false };
};

class BodyTarget {
public:
    static constexpr uint32_t numberOfHeadWords = sizeof(Wire::TargetHead) / sizeof(uint32_t);

    BodyTarget() = default;
    explicit BodyTarget(const uint32_t* head)
        : m_words(head)
    {
    }

    explicit operator bool() const { return m_words; }
    BlockID block() const { return BlockID::fromBits(m_words[0]); }
    std::span<const ValueID> arguments() const { return unsafeMakeSpan(reinterpret_cast<const ValueID*>(m_words + numberOfHeadWords), m_words[1]); }
    std::span<const uint32_t> proofs() const { return unsafeMakeSpan(m_words + numberOfHeadWords + m_words[1], m_words[2]); }
    uint32_t numberOfIncomingValues() const { return m_words[1] + m_words[2]; }
    const uint32_t* end() const { return m_words + numberOfHeadWords + m_words[1] + m_words[2]; }

private:
    const uint32_t* m_words { nullptr };
};

class BodyTargets {
public:
    class Iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = BodyTarget;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = BodyTarget;

        Iterator() = default;
        Iterator(const uint32_t* words, uint32_t remaining)
            : m_words(words)
            , m_remaining(remaining)
        {
        }

        BodyTarget operator*() const { return BodyTarget(m_words); }
        Iterator& operator++()
        {
            m_words = BodyTarget(m_words).end();
            --m_remaining;
            return *this;
        }
        Iterator operator++(int)
        {
            Iterator result = *this;
            ++*this;
            return result;
        }
        bool operator==(const Iterator& other) const { return m_remaining == other.m_remaining; }

    private:
        const uint32_t* m_words { nullptr };
        uint32_t m_remaining { 0 };
    };

    BodyTargets() = default;
    BodyTargets(const uint32_t* first, uint32_t size)
        : m_first(first)
        , m_size(size)
    {
    }

    uint32_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }
    Iterator begin() const { return { m_first, m_size }; }
    Iterator end() const { return { nullptr, 0 }; }
    BodyTarget operator[](uint32_t index) const
    {
        ASSERT(index < m_size);
        const uint32_t* words = m_first;
        while (index--)
            words = BodyTarget(words).end();
        return BodyTarget(words);
    }

private:
    const uint32_t* m_first { nullptr };
    uint32_t m_size { 0 };
};

class BodyTerminator {
public:
    static constexpr uint32_t numberOfHeadWords = sizeof(Wire::TerminatorHead) / sizeof(uint32_t);

    BodyTerminator() = default;
    BodyTerminator(const uint32_t* head, const UnitIDRemap* unit, BlockID block)
        : m_words(head)
        , m_unit(unit)
        , m_block(block)
    {
    }

    explicit operator bool() const { return m_words; }
    BlockID block() const { return m_block; }
    const uint32_t* words() const { return m_words; }
    const UnitIDRemap& unit() const { return *m_unit; }

    TerminatorKind kind() const { return static_cast<TerminatorKind>(m_words[0]); }
    uint32_t position() const { return m_words[1]; }
    bool hasPosition() const { return position() != BodyValue::noPosition; }
    Origin origin() const { return { .position = position(), .inlineFrame = 0, .isTailCall = false, .isPartOfConstruction = false }; }
    BodyFields fieldWords() const { return { m_words + numberOfHeadWords, m_unit, 0 }; }

    uint32_t numberOfTargets() const { return m_words[2]; }
    BodyTargets targets() const { return { m_words + numberOfHeadWords + numberOfFieldWords(kind(), m_words + numberOfHeadWords), numberOfTargets() }; }
    const uint32_t* end() const
    {
        const uint32_t* words = m_words + numberOfHeadWords + numberOfFieldWords(kind(), m_words + numberOfHeadWords);
        for (uint32_t remaining = numberOfTargets(); remaining--;)
            words = BodyTarget(words).end();
        return words;
    }

    bool isConditionalBranch() const { return kind() == TerminatorKind::Branch; }
    bool hasCases() const { return !numberOfFixedTargetsOf(kind()); }
    uint32_t numberOfCases() const { return hasCases() ? numberOfTargets() - 1 : 0; }

    BodyTarget next() const
    {
        ASSERT(kind() == TerminatorKind::Jump);
        return targets()[0];
    }
    BodyTarget taken() const
    {
        ASSERT(isConditionalBranch());
        return targets()[0];
    }
    BodyTarget notTaken() const
    {
        ASSERT(isConditionalBranch());
        return targets()[1];
    }
    BodyTarget otherwise() const
    {
        ASSERT(hasCases());
        return targets()[numberOfTargets() - 1];
    }

    template<TerminatorKind wanted>
    BodyOperation<wanted> as() const
    {
        ASSERT(kind() == wanted);
        return BodyOperation<wanted>(fieldWords());
    }

    template<typename Functor> void forEachOperand(const Functor&) const;
    ValueID operand(uint32_t operandIndex) const;
    std::span<const ValueID> variadicOperands() const;
    template<typename Visitor> void forEachReference(Visitor&&) const;

    OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(kind(), operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(kind(), operandIndex); }

#include "AOTSchemaBodyTerminatorAccessors.inc"

private:
    const uint32_t* m_words { nullptr };
    const UnitIDRemap* m_unit { nullptr };
    BlockID m_block;
};

class BodyValues {
public:
    class Iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = BodyValue;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = BodyValue;

        Iterator() = default;
        Iterator(const uint32_t* words, const UnitIDRemap* unit, uint32_t id)
            : m_words(words)
            , m_unit(unit)
            , m_id(id)
        {
        }

        BodyValue operator*() const { return BodyValue::operation(m_words, m_unit, ValueID(m_id)); }
        Iterator& operator++()
        {
            m_words += (**this).sizeInWords();
            ++m_id;
            return *this;
        }
        Iterator operator++(int)
        {
            Iterator result = *this;
            ++*this;
            return result;
        }
        bool operator==(const Iterator& other) const { return m_id == other.m_id; }
        const uint32_t* words() const { return m_words; }

    private:
        const uint32_t* m_words { nullptr };
        const UnitIDRemap* m_unit { nullptr };
        uint32_t m_id { 0 };
    };

    BodyValues() = default;
    BodyValues(const uint32_t* first, const UnitIDRemap* unit, ValueID firstID, uint32_t size)
        : m_first(first)
        , m_unit(unit)
        , m_firstID(firstID)
        , m_size(size)
    {
    }

    uint32_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }
    Iterator begin() const { return { m_first, m_unit, m_firstID.index() }; }
    Iterator end() const { return { nullptr, m_unit, m_firstID.index() + m_size }; }

private:
    const uint32_t* m_first { nullptr };
    const UnitIDRemap* m_unit { nullptr };
    ValueID m_firstID;
    uint32_t m_size { 0 };
};

class BodyBlock {
public:
    static constexpr uint32_t numberOfHeadWords = sizeof(Wire::BlockHead) / sizeof(uint32_t);
    static_assert(!offsetof(Wire::BlockHead, parameterCount) && offsetof(Wire::BlockHead, valueCount) == 4 && offsetof(Wire::BlockHead, role) == 8 && offsetof(Wire::BlockHead, subject) == 12 && offsetof(Wire::BlockHead, hasHandler) == 16 && offsetof(Wire::BlockHead, frequency) == 20);

    BodyBlock() = default;
    BodyBlock(const uint32_t* head, const uint32_t* terminator, const UnitIDRemap* unit, BlockID id, ValueID firstParameter)
        : m_words(head)
        , m_terminator(terminator)
        , m_unit(unit)
        , m_id(id)
        , m_firstParameter(firstParameter)
    {
    }

    explicit operator bool() const { return m_words; }
    BlockID id() const { return m_id; }
    const uint32_t* words() const { return m_words; }

    BlockRole role() const { return static_cast<BlockRole>(m_words[2]); }
    bool isHandlerBlock() const { return role() != BlockRole::Ordinary; }
    ValueID subject() const { return ValueID::fromBits(m_words[3]); }
    BlockFrequency frequency() const { return static_cast<BlockFrequency>(m_words[5]); }

    uint32_t numberOfParameters() const { return m_words[0]; }
    ValueID firstParameter() const { return m_firstParameter; }
    ValueID parameter(uint32_t index) const
    {
        ASSERT(index < numberOfParameters());
        return ValueID(m_firstParameter.index() + index);
    }
    BodyValue parameterValue(uint32_t index) const { return BodyValue::parameter(m_words + numberOfHeadWords + index, m_unit, parameter(index)); }
    TypeID parameterType(uint32_t index) const { return parameterValue(index).parameterType(); }
    std::optional<uint32_t> indexOfParameter(ValueID value) const
    {
        uint32_t index = value.index() - m_firstParameter.index();
        return value.index() >= m_firstParameter.index() && index < numberOfParameters() ? std::optional(index) : std::nullopt;
    }

    bool hasHandler() const { return m_words[4]; }
    BodyTarget handler() const { return hasHandler() ? BodyTarget(m_words + numberOfHeadWords + numberOfParameters()) : BodyTarget(); }

    uint32_t numberOfValues() const { return m_words[1]; }
    ValueID firstValue() const { return ValueID(m_firstParameter.index() + numberOfParameters()); }
    ValueID endOfValues() const { return ValueID(m_firstParameter.index() + numberOfParameters() + numberOfValues()); }
    bool holds(ValueID value) const { return value.index() >= m_firstParameter.index() && value.index() < endOfValues().index(); }
    BodyValues values() const { return { firstValueWords(), m_unit, firstValue(), numberOfValues() }; }

    BodyTerminator terminator() const { return { terminatorWords(), m_unit, m_id }; }
    BodyBlock next() const { return { terminator().end(), nullptr, m_unit, BlockID(m_id.index() + 1), endOfValues() }; }

    template<typename Functor>
    void forEachOutgoingEdge(const Functor& functor) const
    {
        for (BodyTarget target : terminator().targets())
            functor(target, false);
        if (hasHandler())
            functor(handler(), true);
    }

private:
    const uint32_t* firstValueWords() const { return hasHandler() ? handler().end() : m_words + numberOfHeadWords + numberOfParameters(); }
    const uint32_t* terminatorWords() const
    {
        if (m_terminator)
            return m_terminator;
        BodyValues::Iterator value = values().begin();
        for (uint32_t remaining = numberOfValues(); remaining--;)
            ++value;
        return value.words();
    }

    const uint32_t* m_words { nullptr };
    const uint32_t* m_terminator { nullptr };
    const UnitIDRemap* m_unit { nullptr };
    BlockID m_id;
    ValueID m_firstParameter;
};

class Body {
public:
    Body() = default;
    Body(std::span<const uint32_t> words, uint32_t numberOfBlocks, uint32_t numberOfValues, const UnitIDRemap& unit)
        : m_words(words.data())
        , m_unit(&unit)
        , m_numberOfWords(static_cast<uint32_t>(words.size()))
        , m_numberOfBlocks(numberOfBlocks)
        , m_numberOfValues(numberOfValues)
    {
    }

    explicit operator bool() const { return m_numberOfBlocks; }
    std::span<const uint32_t> words() const { return unsafeMakeSpan(m_words, m_numberOfWords); }
    const UnitIDRemap& unit() const { return *m_unit; }
    uint32_t numberOfBlocks() const { return m_numberOfBlocks; }
    uint32_t numberOfValues() const { return m_numberOfValues; }

    BodyBlock entry() const { return { m_words, nullptr, m_unit, entryBlock, ValueID(0u) }; }

    BodyValue operationAt(uint32_t wordOffset, ValueID id) const { return BodyValue::operation(m_words + wordOffset, m_unit, id); }
    BodyValue parameterAt(uint32_t wordOffset, ValueID id) const { return BodyValue::parameter(m_words + wordOffset, m_unit, id); }
    BodyBlock blockAt(uint32_t wordOffset, uint32_t wordOffsetOfTerminator, BlockID id, ValueID firstParameter) const { return { m_words + wordOffset, m_words + wordOffsetOfTerminator, m_unit, id, firstParameter }; }
    BodyTerminator terminatorAt(uint32_t wordOffsetOfTerminator, BlockID id) const { return { m_words + wordOffsetOfTerminator, m_unit, id }; }

    template<typename Functor>
    void forEachBlock(const Functor& functor) const
    {
        BodyBlock block = entry();
        for (uint32_t index = 0; index < m_numberOfBlocks; ++index) {
            functor(block);
            if (index + 1 < m_numberOfBlocks)
                block = block.next();
        }
    }

private:
    const uint32_t* m_words { nullptr };
    const UnitIDRemap* m_unit { nullptr };
    uint32_t m_numberOfWords { 0 };
    uint32_t m_numberOfBlocks { 0 };
    uint32_t m_numberOfValues { 0 };
};

template<typename Holder>
concept OperationHolder = requires(Holder& holder, uint32_t number, typename Holder::OperandID operand, std::span<const uint32_t> words) {
    holder.setOperand(number, operand);
    holder.reserveVariadicOperands(number);
    holder.appendVariadicOperand(operand);
    holder.setPayloadWordAt(number, number);
    holder.setPayloadListFrom(number, words);
};

template<OperationHolder Holder, typename Functor> void copyFields(const BodyValue&, Holder&, const Functor& operandOf);
template<OperationHolder Holder, typename Functor> void copyFields(const BodyTerminator&, Holder&, const Functor& operandOf);

struct NumbersOfProgramRecords {
    std::array<size_t, numberOfRemappedIDKinds> sizes { };

    size_t& operator[](RemappedIDKind kind) { return sizes[static_cast<size_t>(kind)]; }
    size_t operator[](RemappedIDKind kind) const { return sizes[static_cast<size_t>(kind)]; }
};

class UnitIDTables {
    WTF_MAKE_NONCOPYABLE(UnitIDTables);
public:
    UnitIDTables() = default;

    void initialize(const ExternalFunctionBodies*, std::span<const TypeID> typesAsRead, const NumbersOfProgramRecords&, const ParallelLoops&);

    uint32_t numberOfUnits() const { return static_cast<uint32_t>(m_units.size()); }
    const UnitIDRemap& unit(uint32_t index) const { return m_units[index]; }
    const UnitIDRemap& unitOf(const ExternalFunctionBodies*, FunctionID) const;

private:
    Vector<UnitIDRemap> m_units;
    Vector<uint32_t> m_entries;
};

} } } // namespace JSC::AOT::Typed

#include "AOTSchemaBodyOperations.h"
#include "AOTSchemaBodyWalkers.h"

#endif // ENABLE(AOT)
