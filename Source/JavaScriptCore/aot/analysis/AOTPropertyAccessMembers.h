/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyAccessReceivers.h"

namespace JSC { namespace AOT { namespace Typed {

enum class MemberStart : uint8_t { Instance, ClassPrototype, ClassObject };

struct MemberQuery {
    MemberStart start { MemberStart::Instance };
    LayoutID layout;
    PropertyKeyID key;
    Access access { Access::Read };
    bool isOwnLookup { false };
};

enum class MemberKind : uint8_t { Unknown, Absent, Field, Method, Accessor, Variable, Namespace, Builtin };

struct MemberAnswer {
    MemberKind kind { MemberKind::Unknown };
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    LayoutID holder;
    uint32_t field { 0 };
    FunctionID method;
    FunctionID getter;
    FunctionID setter;
    VariableID variable;
    ModuleID module;
    BuiltinID builtin;
    ProofSet receiverProofs;
    MemberConditions conditions;
    UnprovenCause cause { UnprovenCause::Unknown };

    static MemberAnswer unknown(UnprovenCause cause) { return { .cause = cause }; }

    bool isKnown() const { return kind != MemberKind::Unknown; }
    bool needsNoTest() const { return level == KnowledgeLevel::Proven; }
    FunctionID accessorFor(Access) const;

    void dump(PrintStream&) const;

    friend bool operator==(const MemberAnswer&, const MemberAnswer&) = default;
};

MemberAnswer findMember(const PropertyAccessContext&, const MemberQuery&);

MemberAnswer findInstanceMember(const PropertyAccessContext&, const MemberQuery&);
MemberAnswer findMemberOnPrototypes(const PropertyAccessContext&, const MemberQuery&);
MemberAnswer findPrototypeMember(const PropertyAccessContext&, const MemberQuery&);
MemberAnswer findStaticMember(const PropertyAccessContext&, const MemberQuery&);
MemberAnswer findNamespaceExport(const PropertyAccessContext&, const MemberQuery&);
MemberAnswer findBuiltinInstanceMember(const PropertyAccessContext&, BuiltinClassID, PropertyKeyID, Access);
MemberAnswer findBuiltinObjectMember(const PropertyAccessContext&, BuiltinID holder, PropertyKeyID, Access);

enum class KeyLookup : uint8_t { ByKey, Ordinary };

KeyLookup keyLookupOf(const Program&, PropertyKeyID);

LayoutID classOfClassObject(const Program&, const Type& classObject);

struct MemberArm {
    MemberAnswer answer;
    LayoutList layouts;
};

struct MemberPartition {
    Vector<MemberArm, 2> arms;
    LayoutList rest;

    bool hasOneArm() const { return arms.size() == 1 && rest.isEmpty(); }
    const MemberArm* findArm(MemberKind) const LIFETIME_BOUND;

    void dump(PrintStream&) const;
};

MemberPartition partitionMembers(const PropertyAccessContext&, std::span<const LayoutID> candidates, PropertyKeyID, Access);
MemberPartition partitionOwnMembers(const PropertyAccessContext&, std::span<const LayoutID> candidates, PropertyKeyID);

bool belongToOneArm(const MemberAnswer&, const MemberAnswer&);
void mergeIntoArm(MemberArm&, LayoutID, const MemberAnswer&);
// One test takes the layouts of one arm. Where the receiver is tested in any case, the layouts of which a member is proven go behind the test of those of which it is expected, unless they are the greater part: those are tested for by themselves and asked nothing else.
enum class ArmsToMerge : bool { WhereFewerAreProven, All };
void mergeArmsWithSameMember(MemberPartition&, ArmsToMerge);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
