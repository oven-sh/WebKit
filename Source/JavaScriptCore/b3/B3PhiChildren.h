/*
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include <wtf/GraphNodeWorklist.h>
#include <wtf/IndexMap.h>
#include <wtf/SequesteredMalloc.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace B3 {

class PhiChildren {
    WTF_MAKE_SEQUESTERED_ARENA_ALLOCATED(PhiChildren);
public:
    JS_EXPORT_PRIVATE PhiChildren(Procedure&);
    JS_EXPORT_PRIVATE ~PhiChildren();

    class ValueCollection {
    public:
        ValueCollection(std::span<UpsilonValue* const> values = { })
            : m_values(values)
        {
        }

        unsigned size() const { return m_values.size(); }
        Value* at(unsigned index) const { return m_values[index]->child(0); }
        Value* operator[](unsigned index) const { return at(index); }

        bool contains(Value* value) const
        {
            for (unsigned i = size(); i--;) {
                if (at(i) == value)
                    return true;
            }
            return false;
        }

        class iterator {
        public:
            iterator(std::span<UpsilonValue* const> values = { }, unsigned index = 0)
                : m_values(values)
                , m_index(index)
            {
            }

            Value* operator*() const
            {
                return m_values[m_index]->child(0);
            }

            iterator& operator++()
            {
                m_index++;
                return *this;
            }

            bool operator==(const iterator& other) const
            {
                ASSERT(m_values.data() == other.m_values.data());
                return m_index == other.m_index;
            }

        private:
            std::span<UpsilonValue* const> m_values;
            unsigned m_index;
        };

        iterator begin() const { return iterator(m_values); }
        iterator end() const { return iterator(m_values, m_values.size()); }

    private:
        std::span<UpsilonValue* const> m_values;
    };
    
    class UpsilonCollection {
    public:
        UpsilonCollection()
        {
        }
        
        UpsilonCollection(PhiChildren* phiChildren, Value* value, std::span<UpsilonValue* const> values)
            : m_phiChildren(phiChildren)
            , m_value(value)
            , m_values(values)
        {
        }

        unsigned size() const { return m_values.size(); }
        Value* at(unsigned index) const { return m_values[index]; }
        Value* operator[](unsigned index) const { return at(index); }

        bool contains(Value* value) const { return std::ranges::find(m_values, value) != m_values.end(); }

        using iterator = std::span<UpsilonValue* const>::iterator;
        iterator begin() const { return m_values.begin(); }
        iterator end() const { return m_values.end(); }

        ValueCollection values() { return ValueCollection(m_values); }
        
        template<typename Functor>
        void forAllTransitiveIncomingValues(NOESCAPE const Functor& functor)
        {
            if (m_value->opcode() != Phi) {
                functor(m_value);
                return;
            }
            
            GraphNodeWorklist<Value*> worklist;
            worklist.push(m_value);
            while (Value* phi = worklist.pop()) {
                for (Value* child : m_phiChildren->at(phi).values()) {
                    if (child->opcode() == Phi)
                        worklist.push(child);
                    else
                        functor(child);
                }
            }
        }

        bool transitivelyUses(Value* candidate)
        {
            bool result = false;
            forAllTransitiveIncomingValues(
                [&] (Value* child) {
                    result |= child == candidate;
                });
            return result;
        }

    private:
        PhiChildren* m_phiChildren { nullptr };
        Value* m_value { nullptr };
        std::span<UpsilonValue* const> m_values;
    };

    UpsilonCollection at(Value* value)
    {
        if (m_ranges.isEmpty())
            return UpsilonCollection(this, value, { });
        Range range = m_ranges[value->index()];
        return UpsilonCollection(this, value, m_upsilons.span().subspan(range.begin, range.size));
    }
    UpsilonCollection operator[](Value* value) { return at(value); }

    const Vector<Value*, 8>& phis() const LIFETIME_BOUND { return m_phis; }

private:
    struct Range {
        unsigned begin { 0 };
        unsigned size { 0 };
    };

    Vector<Range> m_ranges;
    Vector<UpsilonValue*> m_upsilons;
    Vector<Value*, 8> m_phis;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
