/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "JSObject.h"

namespace JSC {

class JSAsyncGenerator;

namespace AOT {

class SavedFrame;
struct Instance;

using Typed::GeneratorStepCheck;
using Typed::GeneratorStepStatus;
using Typed::ResumeMode;

class Generator final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    using ResumeMode = Typed::ResumeMode;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static Generator* create(VM&, Structure*);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    enum class State : int32_t {
        Running = -2,
        Completed = -1,
        SuspendedAtStart = 0,
        SuspendedAtYield = 1,
    };

    JSValue resume(JSGlobalObject*, ResumeMode, JSValue);
    JSValue resume(JSGlobalObject*, ResumeMode, JSValue, bool& isDone);
    bool isSuspendedAtStart() const { return m_state == State::SuspendedAtStart; }

    State state() const { return m_state; }
    SavedFrame* savedFrame() const { return m_savedFrame; }
    void setSavedFrame(VM&, SavedFrame*);
    void didThrow();

    static constexpr int32_t generatorSlotInStubFrame = -1;
    static Generator* fromStepStubFrame(CallFrame* stubFrame);

    static constexpr ptrdiff_t offsetOfSavedFrame() { return OBJECT_OFFSETOF(Generator, m_savedFrame); }
    static constexpr ptrdiff_t offsetOfState() { return OBJECT_OFFSETOF(Generator, m_state); }
    static constexpr ptrdiff_t offsetOfHasOtherPrototype() { return OBJECT_OFFSETOF(Generator, m_hasOtherPrototype); }

private:
    Generator(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    SavedFrame* m_savedFrame { nullptr };
    State m_state { State::SuspendedAtStart };
    // Set while the prototype stored in the Structure is not an object.
    bool m_hasOtherPrototype { false };
};
static_assert(sizeof(Generator) == 32);
static_assert(Generator::offsetOfSavedFrame() == sizeof(JSObjectWithButterfly) && Generator::offsetOfState() == Generator::offsetOfSavedFrame() + 8);
static_assert(Generator::offsetOfHasOtherPrototype() == Generator::offsetOfState() + 4 && Generator::offsetOfState() + 8 == sizeof(Generator));
static_assert(!static_cast<int32_t>(Generator::State::SuspendedAtStart));

Structure* structureOfGenerator(JSGlobalObject*, Instance&, JSObject* generatorFunction, CoroutineKind);
JSObject* createGenerator(JSGlobalObject*, Instance&, JSObject* generatorFunction, CoroutineKind);
SavedFrame* savedFrameOfGenerator(JSObject* generator);
void setSavedFrameOfGenerator(VM&, JSObject* generator, SavedFrame*);
void completeGeneratorLeftByThrow(CallFrame* stepStubFrame);
void installGeneratorMethodsForCompiledGenerators(JSGlobalObject*);

JS_EXPORT_PRIVATE bool isGeneratorObject(JSCell*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
