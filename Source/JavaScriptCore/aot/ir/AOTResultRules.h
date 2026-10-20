/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTIRFunction.h"
#include "AOTLayout.h"
#include "AOTType.h"
#include "AOTTypeArena.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class Program;

enum class TypeLevel : uint8_t { Proven, Written, Predicted };

// What reads of a name from receivers with many layouts give. That depends on the tables of the program only, and costs a lookup and a union per layout.
class FieldReadTypes {
public:
    static constexpr size_t fewestReceivers = 8;

    struct Entry {
        LayoutSetID receivers;
        PropertyKeyID key;
        TypeLevel level { TypeLevel::Proven };
        TypeID type;
    };

    const Entry* find(LayoutSetID receivers, PropertyKeyID, TypeLevel) const LIFETIME_BOUND;
    void add(const Entry&);

private:
    Vector<Entry> m_entries;
    HashedIndex m_index;
};

struct TypingContext {
    const Program& program;
    const Function& function;
    TypeArena& types;
    TypeLevel level { TypeLevel::Proven };
    FieldReadTypes* fieldReadTypes { nullptr };

    bool predicts() const { return level == TypeLevel::Predicted; }

    TypeID typeIDOf(const Value& value) const
    {
        switch (level) {
        case TypeLevel::Proven:
            return value.type;
        case TypeLevel::Written:
            return value.writtenOrType();
        case TypeLevel::Predicted:
            return value.predictionOrType();
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    TypeID typeIDOf(ValueID id) const { return typeIDOf(function.at(id)); }
    const Type& typeOf(ValueID id) const { return types.at(typeIDOf(id)); }
    TypeID typeIDOf(const DeclaredType&) const;
    const Type& typeOf(const DeclaredType& declared) const { return types.at(typeIDOf(declared)); }
    TypingContext atLevel(TypeLevel newLevel) const { return { program, function, types, newLevel, fieldReadTypes }; }
    TypingContext predicting() const { return atLevel(TypeLevel::Predicted); }
};

Type computeResultType(const TypingContext&, const Value&);
Type computeResultType(const TypingContext&, const Value& producer, uint32_t resultIndex);
Type computeOperatorResultType(const TypingContext&, Opcode, std::span<const Type> operandTypes);

TypeID resultTypeOf(const Program&, const Function&, const Value&, TypeArena&);
TypeID writtenTypeOf(const Program&, const Function&, const Value&, TypeArena&);
TypeID predictionOf(const Program&, const Function&, const Value&, TypeArena&);

std::optional<uint32_t> numberOfResultsOf(const Program&, const Function&, const Value&);

Type typeOfConstant(const TypingContext&, const Value&);
Type typeOfData(const TypingContext&, const DataNode&);
Type typeOfAllocatedLayout(const TypingContext&, LayoutID, bool keepsShape);
// A constructor allocates what new.target asks for: the prototype of a class below the layout selects an instance of that class.
bool mayAllocateOtherLayout(const Program&, const Value& allocation);
Type typeOfDeclaredLayout(const TypingContext&, LayoutID);
Type typeOfFieldRead(const TypingContext&, std::span<const LayoutID> receivers, PropertyKeyID);
Type typeOfFieldRead(const TypingContext&, const Type& receiver, PropertyKeyID);
// What the read gives where its object has one of these layouts.
Type typeOfSlotRead(const TypingContext&, const Value& read, std::span<const LayoutID> receivers);
// A written object type also lists the primitives that have every member it requires (a string is a `{ message?: unknown }`).
// Under a name that no built-in instance has they read undefined, so the fields of the layouts say what the read gives.
bool hasWrittenField(const Program&, const Type& receiver, PropertyKeyID);
Type typeOfContentsRead(const TypingContext&, const Type& container, ContentsSlot);
Type typeOfElementRead(const TypingContext&, const Type& container, const Type& key);
Type typeOfAwaited(const TypingContext&, const Type& awaited);
Type elementTypeOf(TypedArrayType);

enum class FieldTypeState : uint8_t { Unknown, Proven };

FieldTypeState fieldTypeStateOf(const Program&, FieldReference);

TypeID requiredTypeOfOperand(const TypingContext&, const Value& user, uint32_t operandIndex);
TypeID requiredTypeOfOperand(const TypingContext&, const Terminator& user, uint32_t operandIndex);
TypeID declaredEntryTypeOf(const TypingContext&, const Function& callee, unsigned entryParameterIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
