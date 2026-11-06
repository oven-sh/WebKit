/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFamilyTables.h"
#include "AOTGuardAvailability.h"
#include "AOTIR.h"
#include "AOTSortedSet.h"
#include "AOTVector.h"
#include <compare>
#include <limits>
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

class FlowResults;

struct PropertyAccessContext {
    const Program& program;
    const FlowResults* flow { nullptr };
    // Whoever asks uses an answer only if it holds without a test. A lookup may then end, as unknown, where it notes its first condition. No such answer is kept for others.
    bool usesOnlyAnswersWithoutTests { false };
};

ReceiverState receiverStateOf(const Program&, LayoutID);
ReceiverState receiverStateOf(const Program&, LayoutSetID);
ReceiverState receiverStateOf(const Program&, std::span<const LayoutID>);

enum class OwnPropertyGain : uint8_t { Unknown, Impossible };

OwnPropertyGain ownPropertyGainOf(const Program&, LayoutID, PropertyKeyID);

enum class InstanceCount : uint8_t { Unknown, AtMostOne };

InstanceCount instanceCountOf(const PropertyAccessContext&);

struct MemberCondition {
    using Kind = MemberConditionKind;

    Kind kind { Kind::MemberUnchanged };
    LayoutID layout;
    ClassSide side { ClassSide::Prototype };
    PropertyKeyID key;

    friend bool operator==(const MemberCondition&, const MemberCondition&) = default;
    friend auto operator<=>(const MemberCondition&, const MemberCondition&) = default;
};

using MemberConditions = Vector<MemberCondition, 1>;

void addCondition(MemberConditions&, const MemberCondition&);
void addConditions(MemberConditions&, std::span<const MemberCondition>);

enum class ReceiverProof : uint8_t { Unknown, Guard, Type };

struct ReceiverCandidates {
    ReceiverProof proof { ReceiverProof::Unknown };
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    // Every layout of the receiver, where isProven().
    LayoutList proven;
    // What the expected type and the stamp of flow name. A test makes it true.
    Expected<LayoutList> expected;
    Expected<LayoutList> ofExpectedType;
    bool hasOtherParts { false };
    bool mayBeNullish { false };
    // Apart from undefined and null the receiver is an instance of a built-in class, a function or a built-in object: nothing that a constructor of the program allocates.
    bool isMadeByHostAlone { false };

    static ReceiverCandidates underGuard(std::span<const LayoutID> testedLayouts);

    bool isProven() const { return proof != ReceiverProof::Unknown; }

    // A test for them has failed.
    void stopExpecting(const LayoutList& layouts)
    {
        LayoutList rest = expected.predicted();
        rest.subtract(layouts);
        expected = WTF::move(rest);
    }

    void dump(PrintStream&) const;
};

bool hasMemberToFindUnder(const Program&, LayoutID, PropertyKeyID name);
LayoutList allocatedLayoutsWith(const Program&, std::span<const PropertyKeyID> names, size_t stopsCountingAbove = std::numeric_limits<size_t>::max());

ReceiverCandidates candidatesOf(const Program&, const Type& receiver, const Type* expected, LayoutSetID stamp);
ReceiverCandidates receiverCandidates(const Program&, const Function&, ValueID receiver, LayoutSetID stamp = { });

enum class UnionPartKind : uint8_t { OtherObjects, Nullish, Boolean, Number, String, Symbol, BigInt, Absent, Layouts, BuiltinClass, Functions, Builtins };

struct UnionPart {
    UnionPartKind kind { UnionPartKind::OtherObjects };
    Type type;
};

using UnionParts = Vector<UnionPart, 4>;

UnionParts unionPartsOf(const Type&);
size_t maxNumberOfUnionParts(const Type&);
size_t maxNumberOfUnionPartsWithObjects(const Type&);
bool isClosedUnion(const UnionParts&);
inline bool isClosedUnion(const Type& type) { return !type.anyObject && !type.anyFunction && !type.environment && !type.opaque; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
