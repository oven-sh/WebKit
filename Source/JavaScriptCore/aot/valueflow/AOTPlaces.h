/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTActors.h"
#include "AOTValueFlow.h"

namespace JSC { namespace AOT { namespace Typed {

enum class PlaceKind : uint8_t {
    Property,
    Elements,
    EveryOwnProperty,
    Accessor,
    PrototypeLink,
    Variable,
    EnvironmentField,
    Field,
    Cell,
};
inline constexpr unsigned numberOfPlaceKinds = 9;

namespace PlaceTables {

inline constexpr bool yes = true;
inline constexpr bool no = false;
using Row = std::array<bool, numberOfActors>;

// A parameter that an arguments object names lives in that object, so an environment field has no holder that anybody could be handed.
inline constexpr std::array<Row, numberOfPlaceKinds> whoCanWrite { {
    //                      Statements Creation Table NativeCode Loader KeysNobodyKnows ReceiversNobodyKnows UnseenCode
    /* Property */         { yes,      yes,     yes,  yes,       yes,   yes,            yes,                 yes },
    /* Elements */         { yes,      yes,     yes,  yes,       yes,   yes,            yes,                 yes },
    /* EveryOwnProperty */ { yes,      yes,     yes,  yes,       yes,   yes,            yes,                 yes },
    /* Accessor */         { yes,      yes,     yes,  yes,       yes,   yes,            yes,                 yes },
    /* PrototypeLink */    { yes,      yes,     yes,  yes,       no,    yes,            yes,                 yes },
    /* Variable */         { yes,      yes,     yes,  yes,       yes,   yes,            yes,                 yes },
    /* EnvironmentField */ { yes,      yes,     no,   no,        no,    no,             no,                  yes },
    /* Field */            { yes,      yes,     yes,  yes,       yes,   yes,            yes,                 yes },
    /* Cell */             { yes,      yes,     yes,  yes,       yes,   no,             yes,                 yes },
} };

// Making an object reads nothing out of it.
inline constexpr std::array<Row, numberOfPlaceKinds> whoCanRead { {
    //                      Statements Creation Table NativeCode Loader KeysNobodyKnows ReceiversNobodyKnows UnseenCode
    /* Property */         { yes,      no,      yes,  yes,       yes,   yes,            yes,                 yes },
    /* Elements */         { yes,      no,      yes,  yes,       yes,   yes,            yes,                 yes },
    /* EveryOwnProperty */ { yes,      no,      yes,  yes,       yes,   yes,            yes,                 yes },
    /* Accessor */         { yes,      no,      yes,  yes,       yes,   yes,            yes,                 yes },
    /* PrototypeLink */    { yes,      no,      yes,  yes,       yes,   yes,            yes,                 yes },
    /* Variable */         { yes,      no,      yes,  yes,       yes,   yes,            yes,                 yes },
    /* EnvironmentField */ { yes,      no,      no,   no,        no,    no,             no,                  yes },
    /* Field */            { yes,      no,      yes,  yes,       yes,   yes,            yes,                 yes },
    /* Cell */             { yes,      no,      yes,  yes,       yes,   no,             yes,                 yes },
} };

constexpr OptionSet<Actor> actorsOf(const Row& row)
{
    OptionSet<Actor> actors;
    for (Actor actor : actorsInOrder) {
        if (row[indexOf(actor)])
            actors.add(actor);
    }
    return actors;
}

} // namespace PlaceTables

constexpr bool canWrite(PlaceKind kind, Actor actor) { return PlaceTables::whoCanWrite[static_cast<unsigned>(kind)][indexOf(actor)]; }
constexpr bool canRead(PlaceKind kind, Actor actor) { return PlaceTables::whoCanRead[static_cast<unsigned>(kind)][indexOf(actor)]; }
constexpr OptionSet<Actor> actorsThatCanWrite(PlaceKind kind) { return PlaceTables::actorsOf(PlaceTables::whoCanWrite[static_cast<unsigned>(kind)]); }
constexpr OptionSet<Actor> actorsThatCanRead(PlaceKind kind) { return PlaceTables::actorsOf(PlaceTables::whoCanRead[static_cast<unsigned>(kind)]); }

// The one producer of the name of a place.
class Place {
public:
    enum class Search : uint8_t { Own, Chain };
    enum class Part : uint8_t { Getter, Setter };
    enum class Enumerable : uint8_t { OnlyEnumerable, AlsoHidden };

    // A name that reads as a number is elements().
    static Place property(const Program& program, ValueSetID holders, PropertyKeyID key, Search search)
    {
        ValueFlowName name = ValueFlowName::property(program, key);
        return { name.kind == ValueFlowNameKind::Elements ? PlaceKind::Elements : PlaceKind::Property, holders, name, search };
    }
    static Place elements(ValueSetID holders, Search search = Search::Own) { return { PlaceKind::Elements, holders, elementsName, search }; }
    static Place everyOwnProperty(ValueSetID holders, Enumerable enumerable = Enumerable::AlsoHidden) { return { PlaceKind::EveryOwnProperty, holders, enumerable == Enumerable::AlsoHidden ? everyOwnPropertyName : everyPropertyName, Search::Own }; }
    static Place accessor(const Program& program, ValueSetID holders, PropertyKeyID key, Part part, Search search = Search::Own) { return { PlaceKind::Accessor, holders, part == Part::Getter ? ValueFlowName::getter(program, key) : ValueFlowName::setter(program, key), search }; }
    static Place prototypeLink(ValueSetID holders) { return { PlaceKind::PrototypeLink, holders, prototypeLinkName, Search::Own }; }
    static Place cell(ValueSetID holders, ValueFlowCellName cell) { return { PlaceKind::Cell, holders, cell, Search::Own }; }
    static Place variable(VariableID variable)
    {
        Place place { PlaceKind::Variable, { }, { }, Search::Own };
        place.m_owner = variable.index();
        return place;
    }
    static Place field(const Program& program, LayoutID layout, unsigned field)
    {
        Place place { program.at(layout).isEnvironment() ? PlaceKind::EnvironmentField : PlaceKind::Field, { }, { }, Search::Own };
        place.m_owner = layout.index();
        place.m_field = field;
        return place;
    }

    PlaceKind kind() const { return m_kind; }
    Search search() const { return m_search; }
    bool liesOnObjects() const { return m_kind != PlaceKind::Variable && m_kind != PlaceKind::EnvironmentField && m_kind != PlaceKind::Field; }

    ValueSetID holders() const
    {
        ASSERT(liesOnObjects());
        return m_holders;
    }
    ValueFlowName name() const
    {
        ASSERT(liesOnObjects());
        return m_name;
    }
    VariableID variable() const
    {
        ASSERT(m_kind == PlaceKind::Variable);
        return VariableID(m_owner);
    }
    LayoutID layout() const
    {
        ASSERT(m_kind == PlaceKind::EnvironmentField || m_kind == PlaceKind::Field);
        return LayoutID(m_owner);
    }
    unsigned field() const
    {
        ASSERT(m_kind == PlaceKind::EnvironmentField || m_kind == PlaceKind::Field);
        return m_field;
    }

    friend bool operator==(const Place&, const Place&) = default;

private:
    Place(PlaceKind kind, ValueSetID holders, ValueFlowName name, Search search)
        : m_kind(kind)
        , m_search(search)
        , m_holders(holders)
        , m_name(name)
    {
    }

    PlaceKind m_kind;
    Search m_search;
    ValueSetID m_holders;
    ValueFlowName m_name;
    uint32_t m_owner { 0 };
    uint32_t m_field { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
