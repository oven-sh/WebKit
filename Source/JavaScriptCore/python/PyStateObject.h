/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#include "JSObject.h"
#include "PyInstance.h"
#include <wtf/TZoneMalloc.h>

namespace JSC {

class JSArrayBufferView;

namespace Python {

// What an instance of a class that is written in C++ has besides its attributes: what in CPython are the fields of the C struct after PyObject_HEAD. Each such class has a struct derived from this, and an instance
// of it, or of a class derived from it, is a PyStateObject that has one.
//
//     struct CounterState final : NativeState {
//         PYTHON_NATIVE_STATE(CounterState);
//         WriteBarrier<Unknown> callback;
//         int64_t count { 0 };
//     };
//     template<typename Visitor> void CounterState::visit(Visitor& visitor) { visitor.append(callback); }
//
// The collector goes through it while the program is running. What it is told of is to be in a WriteBarrier that is always there, and not in anything that grows.
class OwedOutput;

class NativeState {
    WTF_MAKE_TZONE_ALLOCATED(NativeState);
    WTF_MAKE_NONCOPYABLE(NativeState);
public:
    NativeState() = default;
    virtual ~NativeState() = default;
    virtual void visitChildren(SlotVisitor&) { }
    virtual void visitChildren(AbstractSlotVisitor&) { }
    // Which struct it is: something that no other has.
    virtual const void* kind() const = 0;

    // For a class whose instances have bytes to show, as a bytearray has: one that has bf_getbuffer in CPython. The bytes are kept in a typed array, so that they are the collector's to account for and to free, and all of it is
    // what there is to show. Nobody keeps where they are: see Python::Buffer. So it can be made longer or shorter at any time.
    struct ExportedBytes {
        JSArrayBufferView* storage;
        char format; // What an item is, to a memoryview
        unsigned itemSize;
        bool isReadOnly { false };
    };
    virtual std::optional<ExportedBytes> exportedBytes() const { return std::nullopt; }
    // It is about to be asked for them, and may raise: what bf_getbuffer does before it fills anything in.
    virtual void willExportBytes(JSGlobalObject*) const { }
    // For a class whose instances have none of their own, and show those of something else as that shows them: one whose bf_getbuffer asks another object. Nothing if it is no such class. It is empty if there is no
    // longer anything to show, which willExportBytes() raises for.
    virtual std::optional<JSValue> showsBytesOf() const { return std::nullopt; }

    // What PySequence_GetItem() gives, for a class that has the flag HasSequenceItemOfItsOwn. Empty if it raised.
    virtual JSValue sequenceItem(JSGlobalObject*, int64_t) const { return { }; }

    // For a stream that keeps what is written to it for a while before it passes it on. Null if it is no such thing. See Python::OwedOutput.
    virtual OwedOutput* owedOutput() { return nullptr; }

    // How much memory it keeps that is not the collector's, if that is a good deal: what a library has allocated for it. Nothing is freed until the collector runs, and how soon it runs goes by how much it takes there to
    // be. It is asked when the object is made, and by the collector while the program is running, so it is to be something that can be read at any time.
    virtual size_t memoryOutsideTheHeap() const { return 0; }
};

#define PYTHON_NATIVE_STATE(Name) \
    WTF_MAKE_TZONE_ALLOCATED_INLINE(Name); \
public: \
    static const void* staticKind() { static const char tag = 0; return &tag; } \
    const void* kind() const final { return staticKind(); } \
    void visitChildren(SlotVisitor& visitor) final { visit(visitor); } \
    void visitChildren(AbstractSlotVisitor& visitor) final { visit(visitor); } \
    template<typename Visitor> void visit(Visitor&)

} // namespace Python

class PyStateObject final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetCallData;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.pyStateObjectSpace<mode>();
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;
    static size_t estimatedSize(JSCell*, VM&);
    PYTHON_OVERLOADS_OPERATORS
    JS_EXPORT_PRIVATE static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static CallData getCallData(JSCell*);

    JS_EXPORT_PRIVATE static PyStateObject* create(VM&, Structure*, std::unique_ptr<Python::NativeState>&&);

    // A method of a class is only ever given an instance of it, which has what the __new__() of the class gave it. So this does not fail, and if it did it would not do to go on.
    template<typename State>
    State& state() const
    {
        RELEASE_ASSERT(m_state->kind() == State::staticKind());
        return static_cast<State&>(*m_state);
    }
    template<typename State>
    State* tryState() const { return m_state->kind() == State::staticKind() ? static_cast<State*>(m_state.get()) : nullptr; }
    std::optional<Python::NativeState::ExportedBytes> exportedBytes() const { return m_state->exportedBytes(); }
    void willExportBytes(JSGlobalObject* globalObject) const { m_state->willExportBytes(globalObject); }
    std::optional<JSValue> showsBytesOf() const { return m_state->showsBytesOf(); }
    JSValue sequenceItem(JSGlobalObject* globalObject, int64_t index) const { return m_state->sequenceItem(globalObject, index); }
    Python::OwedOutput* owedOutput() const { return m_state->owedOutput(); }

private:
    PyStateObject(VM& vm, Structure* structure, std::unique_ptr<Python::NativeState>&& state)
        : Base(vm, structure)
        , m_state(WTF::move(state))
    {
    }

    const std::unique_ptr<Python::NativeState> m_state;
};

namespace Python {

// The state of something, if it is a PyStateObject and has that kind. Otherwise null.
template<typename State>
inline State* tryStateOf(JSValue value)
{
    if (!value.isCell() || !value.asCell()->inherits<PyStateObject>())
        return nullptr;
    return uncheckedDowncast<PyStateObject>(value.asCell())->tryState<State>();
}

template<typename State>
inline State& stateOf(JSValue value)
{
    return uncheckedDowncast<PyStateObject>(value.asCell())->state<State>();
}

} // namespace Python

} // namespace JSC
