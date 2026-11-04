/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTEffects.h"

namespace JSC { namespace AOT { namespace Typed {

// What a state knows of the collector: the part of a WalkState that is copied, met and ended with it. Each costs the same whatever the state holds.
// No heap says that something may collect, so nothing here rests on one: it ends at whatever may collect or run other code.
class KnownBarriers {
public:
    bool isEmpty() const { return !m_youngCell && !m_owner; }
    void clear() { *this = { }; }

    // The record of an allocation. On every way to here the cell was made behind the last safepoint, and no memory and no other value names it: the collector cannot have marked it.
    // An allocation may collect, so no older cell is young beside this one.
    Reference youngCell() const { return m_youngCell; }
    void noteYoungCell(Reference made) { m_youngCell = made; }
    void endYoungCell() { m_youngCell = { }; }
    // Walker::letLeave(): a parameter of a block takes the cell, or a terminator that is code in line. A parameter is a name that nobody follows back: whoever lays it in memory lays the cell there.
    void endYoungCellIfItIs(Reference cell)
    {
        if (m_youngCell == cell)
            m_youngCell = { };
    }
    // The head of a loop, whatever the look says that the turns do: what lets the cell be seen further down comes round to a store above it, and what is built in front of the loop later on may collect.
    // The head of a handler block: the error was made on the way.
    void enterHead() { clear(); }

    // THIS PART DECIDES NOTHING. The sweep finds which barrier a later one covers. This says where a barrier behind stores may pay.
    // The name, behind conversions, of the cell that the newest store with a barrier went into. Every way to here has built a record that takes the name, so the name is in sight here.
    Reference owner() const { return m_owner; }
    // Two ways have met that each bring a store of their own, and nothing has collected since.
    bool severalStoresHaveMet() const { return m_severalStoresHaveMet; }
    // `store` is null on a way that is seldom taken and on which no store waits: it stands in nobody's way.
    void noteStore(Reference owner, Reference store)
    {
        m_owner = owner;
        m_store = store;
        m_severalStoresHaveMet = false;
    }
    void endStores() { noteStore({ }, { }); }

    void endWhatOperationEnds(const Effects& effects)
    {
        if (effects.mayCollect || effects.mayRunOtherCode())
            clear();
    }

    void meetWith(const KnownBarriers& other)
    {
        if (m_youngCell != other.m_youngCell)
            m_youngCell = { };
        if (m_owner != other.m_owner) {
            endStores();
            return;
        }
        m_severalStoresHaveMet |= other.m_severalStoresHaveMet || (m_store && other.m_store && m_store != other.m_store);
        if (!m_store)
            m_store = other.m_store;
    }

private:
    Reference m_youngCell;
    Reference m_owner;
    Reference m_store;
    bool m_severalStoresHaveMet { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
