/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLedger.h"
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

// The part of the ledger for the own properties and the prototype links of function objects, of the prototype objects of classes
// and of the objects that a program makes the prototype object of a function or of what Object.create() makes.
// It answers for a place with one holder. A write of elements through every object opens elements() alone.
class LedgerOfClassesAndClosures final : public LedgerPart {
    WTF_MAKE_NONCOPYABLE(LedgerOfClassesAndClosures);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LedgerOfClassesAndClosures);
public:
    using HandedToObjectCreate = Vector<std::pair<LayoutID, ValueSetID>>;
    using ClassesOfStoredPrototypeObjects = UncheckedKeyHashMap<uint32_t, LayoutID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    explicit LedgerOfClassesAndClosures(const ValueFlowResultInputs&);

    std::optional<Writers> writersOf(const Place&) const final;
    std::optional<Reachers> reachersOf(ValueFlowObjectID) const final;
    // A method that a class defines needs no function object where every read of the member is a call: who has looked at the reads of members says which closures are taken out as values.
    std::optional<Reachers> reachersOf(ValueFlowObjectID, ValueSetID closuresTakenOutOfMembers) const;

    Writers writersOfProperty(ValueFlowObjectID holder, PropertyKeyID) const;
    Writers writersOfEveryOwnProperty(ValueFlowObjectID holder) const;
    Writers writersOfElements(ValueFlowObjectID holder) const;
    Writers writersOfPrototypeLink(ValueFlowObjectID holder) const;

    // What is filed on a holder but for its links, whoever else may write there.
    std::span<const Write> writesFiledOn(ValueFlowObjectID holder) const LIFETIME_BOUND;
    OptionSet<Actor> handsOpenOn(ValueFlowObjectID) const;

    // A write whose receiver may be any object at all is filed under no holder. It opens every place of its kind.
    std::span<const Write> writesThroughEveryObject() const LIFETIME_BOUND { return m_writesThroughEveryObject.span(); }

    const HandedToObjectCreate& handedToObjectCreate() const LIFETIME_BOUND { return m_handedToObjectCreate; }
    const ClassesOfStoredPrototypeObjects& classesOfStoredPrototypeObjects() const LIFETIME_BOUND { return m_classesOfStoredPrototypeObjects; }
    std::span<const ValueFlowObjectID> prototypeObjectsStoredFor(LayoutID classLayout) const LIFETIME_BOUND;

    std::array<ValueFlowObjectID, 3> standInsOf(ValueFlowObjectID) const;
    bool covers(ValueSetID, ValueFlowObjectID) const;
    ValueSetID linkedPrototypes(ValueSetMaker&, ValueFlowObjectID) const;
    bool mayBeLinkedAbove(ValueFlowObjectID object) const { return covers(m_objectsLinkedAbove, object); }
    ValueSetID classPrototypes() const { return m_classPrototypes; }
    ValueSetID functionsWithNonObjectPrototype() const { return m_functionsWithNonObjectPrototype; }
    ValueFlowNodeID nodeOfMark(ObjectMutation mutation) const { return m_nodesOfMarks.get(static_cast<uint32_t>(mutation)); }

private:
    struct WritesOfHolder {
        uint32_t holder { 0 };
        uint32_t first { 0 };
        uint32_t endOfNames { 0 };
        uint32_t endOfWholeObject { 0 };
        uint32_t endOfElements { 0 };
        uint32_t end { 0 };
        bool isStoredToUnderKeysNobodyKnows { false };
        bool isDeletedFromOrStoredToUnderIndexes { false };
    };

    bool isHolder(ValueFlowObjectID) const;
    const WritesOfHolder* findWritesOf(ValueFlowObjectID) const;
    enum class Deletions : uint8_t { MayHitPlace, CannotHitPlace };
    OptionSet<Actor> openOn(ValueFlowObjectID holder, PlaceKind, const WritesOfHolder*, Deletions = Deletions::MayHitPlace) const;
    Writers accountOf(ValueFlowObjectID holder, PlaceKind, ValueFlowName) const;

    void findPrototypeObjectsThatProgramStores(ValueSetMaker&);
    void fileWrites(ValueSetMaker&);
    void findLinks(ValueSetMaker&);
    void findCallablesLookedInto(ValueSetMaker&);

    const ValueFlowResultInputs& m_inputs;

    Vector<Write> m_writes;
    Vector<WritesOfHolder> m_writesOfHolders;
    Vector<Write> m_writesThroughEveryObject;
    bool m_namesAreWrittenThroughEveryObject { false };
    bool m_elementsAreWrittenThroughEveryObject { false };
    bool m_linksAreWrittenThroughEveryObject { false };

    HandedToObjectCreate m_handedToObjectCreate;
    ClassesOfStoredPrototypeObjects m_classesOfStoredPrototypeObjects;
    Vector<std::pair<LayoutID, ValueFlowObjectID>> m_storedPrototypeObjectsByClass;
    Vector<ValueFlowObjectID> m_storedPrototypeObjects;
    ValueSetID m_otherHolders { ValueSets::nothing };

    ValueSetID m_classPrototypes { ValueSets::nothing };
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_linksByObject;
    ValueSetID m_linksOfAllObjects { ValueSets::nothing };
    ValueSetID m_objectsLinkedAbove { ValueSets::nothing };
    ValueSetID m_functionsWithNonObjectPrototype { ValueSets::nothing };
    HashMap<uint32_t, ValueFlowNodeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfMarks;

    ValueSetID m_callablesLookedInto { ValueSets::nothing };
    std::array<Vector<Reach, numberOfReachForms>, 1u << numberOfReachForms> m_reachesOfForms;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
