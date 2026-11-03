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

    // Generators lie by themselves, so that whoever has to find some of them looks at generators alone: CoroutineTable::willMakePrototypeOfGeneratorFunction().
    static constexpr uint8_t numberOfLowerTierPreciseCells = 0;
    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm) { return vm.aotGeneratorSpace<mode>(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static Generator* create(VM&, Structure*);
    // The object that the generator inherits from is the `prototype` of its function, and a function that nobody has asked for it has none yet. The generator finds the function in a word of its
    // heap frame that a generator has no other use for: whoever makes the generator makes the frame at once. Its Structure names an empty object that stands for the one to come:
    // whoever looks for a name finds the same, and whoever asks for the prototype comes to getPrototype().
    static Structure* createStructureWithoutPrototype(VM&, JSGlobalObject*, JSObject* standIn);
    static Generator* createWithoutPrototype(VM&, Structure*);
    static JSValue getPrototype(JSObject*, JSGlobalObject*);
    bool takesPrototypeFromFunction() const { return m_takesPrototypeFromFunction; }
    uint32_t closureNumberOfFunction() const;
    Instance* instanceOfFunction() const;
    // From here on its Structure names what it inherits from, as that of every other object does.
    void takePrototypeFromFunction(VM&);
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
    bool isSuspendedAtStart() const { return m_state == State::SuspendedAtStart; }

    State state() const { return m_state; }
    SavedFrame* savedFrame() const { return m_savedFrame; }
    void setSavedFrame(VM&, SavedFrame*);
    void didThrow() { m_state = State::Completed; }

    static constexpr int32_t generatorSlotInStubFrame = -1;

    static constexpr ptrdiff_t offsetOfSavedFrame() { return OBJECT_OFFSETOF(Generator, m_savedFrame); }
    static constexpr ptrdiff_t offsetOfState() { return OBJECT_OFFSETOF(Generator, m_state); }
    static constexpr ptrdiff_t offsetOfHasOtherPrototype() { return OBJECT_OFFSETOF(Generator, m_hasOtherPrototype); }
    static constexpr ptrdiff_t offsetOfTakesPrototypeFromFunction() { return OBJECT_OFFSETOF(Generator, m_takesPrototypeFromFunction); }

private:
    Generator(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    SavedFrame* m_savedFrame { nullptr };
    State m_state { State::SuspendedAtStart };
    // Set while the prototype stored in the Structure is not an object.
    bool m_hasOtherPrototype { false };
    bool m_takesPrototypeFromFunction { false };

    JSObject* functionThatMadeIt() const;
};
static_assert(sizeof(Generator) == 32);
static_assert(Generator::offsetOfTakesPrototypeFromFunction() == Generator::offsetOfState() + 5);
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

bool isStructureOfGeneratorsWithoutPrototype(const Structure*);
// Whether a search for a name may not go by the prototype that the Structure names.
ALWAYS_INLINE bool hidesObjectsAbove(const Structure* structure)
{
    return structure->typeInfo().overridesGetPrototype() && !isStructureOfGeneratorsWithoutPrototype(structure);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
