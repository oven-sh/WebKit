/*
 * Copyright (C) 2024-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <WebCore/QuirkBehaviors.h>
#include <WebCore/URLMatch.h>
#include <span>
#include <wtf/Vector.h>

namespace WebCore {

struct ParameterizedQuirkBehavior {
    QuirkBehaviorID id;
    std::optional<URLMatch> urlCondition { std::nullopt };
    QuirkParameters parameters;
};

class QuirksData {
public:
    inline bool isBehaviorEnabled(const QuirkBehavior& quirk) const
    {
        return m_activeQuirks.get(static_cast<size_t>(quirk.id));
    }

    inline bool isSite(QuirkSite site) const
    {
        return m_sites.get(static_cast<size_t>(site));
    }

    inline bool hasEnabledBehaviors() const
    {
        return !m_activeQuirks.isEmpty();
    }

    inline const QuirkBitSet& enabledBehaviors() const LIFETIME_BOUND
    {
        return m_activeQuirks;
    }

    inline Vector<QuirkParameters, 1> parametersFor(const QuirkBehavior& behavior, const URLMatchContext& context) const
    {
        if (!isBehaviorEnabled(behavior))
            return { };

        Vector<QuirkParameters, 1> matching;
        for (auto& candidate : m_parameterizedBehaviors) {
            if (candidate.id != behavior.id)
                continue;

            if (!candidate.urlCondition || candidate.urlCondition->matches(context))
                matching.append(candidate.parameters);
        }

        return matching;
    }

    inline void applyTableRow(std::span<const QuirkBehavior> behaviors)
    {
        for (auto& behavior : behaviors) {
            m_activeQuirks.set(static_cast<size_t>(behavior.id));

            if (behavior.parameters)
                m_parameterizedBehaviors.append({ behavior.id, behavior.urlCondition, *behavior.parameters });
        }
    }

    inline void addSite(QuirkSite site)
    {
        m_sites.set(static_cast<size_t>(site));
    }

    inline void setEnabled(QuirkBehaviorID id, bool state)
    {
        m_activeQuirks.set(static_cast<size_t>(id), state);
    }

    void merge(const QuirksData& other)
    {
        auto& [otherActiveQuirks, otherSites, otherParameterizedBehaviors] = other;
        m_activeQuirks.merge(otherActiveQuirks);
        m_sites.merge(otherSites);
        m_parameterizedBehaviors.appendVector(otherParameterizedBehaviors);
    }

private:
    QuirkBitSet m_activeQuirks;
    QuirkSiteBitSet m_sites;
    Vector<ParameterizedQuirkBehavior> m_parameterizedBehaviors;
};

} // namespace WebCore

