/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTOpcode.h"
#include "AOTVector.h"
#include "AOTWireEnums.h"
#include <algorithm>
#include <iterator>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static_assert(static_cast<unsigned>(Form::Open) == static_cast<unsigned>(Wire::CodeForm::Open));
static_assert(static_cast<unsigned>(Form::Closed) == static_cast<unsigned>(Wire::CodeForm::Closed));
static_assert(static_cast<unsigned>(Form::Runtime) == static_cast<unsigned>(Wire::CodeForm::Generic));
static_assert(numberOfEnumerators<Form> == numberOfEnumerators<Wire::CodeForm>);

constexpr Wire::CodeForm reportedFormOf(Form form) { return static_cast<Wire::CodeForm>(form); }
constexpr Form formReportedAs(Wire::CodeForm form) { return static_cast<Form>(form); }

constexpr bool hasOneForm(auto opcode)
{
    return openUnderOf(opcode).isEmpty() && closedUnderOf(opcode).isEmpty() && runtimeUnderOf(opcode).isEmpty();
}

constexpr std::optional<Form> namedFormOf(auto opcode, StateOfKnowledge state, Place place)
{
    if (place != Place::Anywhere) {
        if (openInOf(opcode).contains(place))
            return Form::Open;
        if (closedInOf(opcode).contains(place))
            return Form::Closed;
        if (runtimeInOf(opcode).contains(place))
            return Form::Runtime;
        return std::nullopt;
    }
    if (hasOneForm(opcode))
        return state == StateOfKnowledge::Always ? std::optional<Form>(formOf(opcode)) : std::nullopt;
    if (openUnderOf(opcode).contains(state))
        return Form::Open;
    if (closedUnderOf(opcode).contains(state))
        return Form::Closed;
    if (runtimeUnderOf(opcode).contains(state))
        return Form::Runtime;
    return std::nullopt;
}

struct FormAtPlace {
    Form form;
    Place place;
};
constexpr FormAtPlace formByLoopAlone(auto opcode, StateOfKnowledge state, bool isInLoop)
{
    if (isInLoop) {
        if (std::optional<Form> formInLoops = namedFormOf(opcode, state, Place::Loop))
            return { *formInLoops, Place::Loop };
    }
    return { namedFormOf(opcode, state, Place::Anywhere).value_or(formOf(opcode)), Place::Anywhere };
}

struct FormSaidFalsely {
    Opcode opcode;
    Form form;
};
JS_EXPORT_PRIVATE void setFormSaidFalsely(std::optional<FormSaidFalsely>);
std::optional<FormSaidFalsely> formSaidFalsely();

struct SiteOfForm {
    ValueID value;
    BlockID block;
    Opcode opcode;
    Form form { Form::Open };
    StateOfKnowledge state { StateOfKnowledge::Always };
    Place place { Place::Anywhere };
    bool hasSaid { false };
    bool hasContradictedItself { false };
};

class FormsSaid {
public:
    static constexpr uint32_t noSite = UINT32_MAX;

    bool areNoted() const { return m_areNoted; }
    void beginNoting(bool isTrapOfProxy)
    {
        m_areNoted = true;
        m_isTrapOfProxy = isTrapOfProxy;
        m_saidFalsely = formSaidFalsely();
    }

    uint32_t enter(ValueID value, Opcode opcode, BlockID block, uint32_t numberOfMachineOperations)
    {
        if (!m_areNoted) [[likely]]
            return noSite;
        uint32_t siteOfUser = m_current;
        m_current = static_cast<uint32_t>(m_sites.size());
        m_sites.append(SiteOfForm { value, block, opcode });
        m_turns.append({ numberOfMachineOperations, m_current });
        if (m_saidFalsely && m_saidFalsely->opcode == opcode)
            say(m_saidFalsely->form, StateOfKnowledge::Always, Place::Anywhere);
        return siteOfUser;
    }
    void leave(uint32_t siteOfUser, uint32_t numberOfMachineOperations)
    {
        if (!m_areNoted) [[likely]]
            return;
        m_current = siteOfUser;
        m_turns.append({ numberOfMachineOperations, siteOfUser });
    }

    void say(Form form, StateOfKnowledge state, Place place)
    {
        if (m_current == noSite)
            return;
        SiteOfForm& site = m_sites[m_current];
        if (site.hasSaid) {
            bool isControl = m_saidFalsely && m_saidFalsely->opcode == site.opcode;
            site.hasContradictedItself |= !isControl && (site.form != form || site.state != state || site.place != place);
            return;
        }
        site.form = form;
        site.state = state;
        site.place = place;
        site.hasSaid = true;
    }
    std::optional<Opcode> opcodeBeingGenerated() const { return m_current == noSite ? std::nullopt : std::optional<Opcode>(m_sites[m_current].opcode); }

    bool isTrapOfProxy() const { return m_isTrapOfProxy; }
    std::span<const SiteOfForm> sites() const LIFETIME_BOUND { return m_sites.span(); }
    uint32_t siteOf(uint32_t machineOperation) const
    {
        auto behind = std::ranges::upper_bound(m_turns, machineOperation, { }, &Turn::firstMachineOperation);
        return behind == m_turns.begin() ? noSite : std::prev(behind)->site;
    }

private:
    struct Turn {
        uint32_t firstMachineOperation;
        uint32_t site;
    };

    Vector<SiteOfForm> m_sites;
    Vector<Turn> m_turns;
    std::optional<FormSaidFalsely> m_saidFalsely;
    bool m_isTrapOfProxy { false };
    uint32_t m_current { noSite };
    bool m_areNoted { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
