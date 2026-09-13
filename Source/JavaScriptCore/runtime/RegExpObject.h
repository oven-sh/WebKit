/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2003-2023 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#pragma once

#include "JSObject.h"
#include "RegExp.h"
#include "ThrowScope.h"
#include "TypeError.h"

namespace JSC {
    
class RegExpObject final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnSpecialPropertyNames | OverridesPut;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        static_assert(CellType::needsDestruction == DoesNotNeedDestruction);
        return &vm.regExpObjectSpace();
    }

    static constexpr uintptr_t lastIndexIsNotWritableFlag = 0b01;
    static constexpr uintptr_t legacyFeaturesDisabledFlag = 0b10;
    // The object that one RegExp literal site hands out at every evaluation (op_new_reg_exp_shared). Its only reader is the
    // builtin test or exec it is the receiver of. Whatever is about to show it to anything else gives that a copy instead.
    static constexpr uintptr_t sharedLiteralFlag = 0b100;
    static constexpr uintptr_t flagsMask = lastIndexIsNotWritableFlag | legacyFeaturesDisabledFlag | sharedLiteralFlag;
    static constexpr uintptr_t regExpMask = ~flagsMask;
    static_assert(flagsMask < MarkedBlock::atomSize);

    static RegExpObject* create(VM& vm, Structure* structure, RegExp* regExp, bool areLegacyFeaturesEnabled = true)
    {
        RegExpObject* object = new (NotNull, allocateCell<RegExpObject>(vm)) RegExpObject(vm, structure, regExp, areLegacyFeaturesEnabled);
        object->finishCreation(vm);
        return object;
    }

    static RegExpObject* create(VM& vm, Structure* structure, RegExp* regExp, JSValue lastIndex)
    {
        static constexpr bool areLegacyFeaturesEnabled = true;
        auto* object = create(vm, structure, regExp, areLegacyFeaturesEnabled);
        object->m_lastIndex.set(vm, object, lastIndex);
        return object;
    }

    void setRegExp(VM& vm, RegExp* regExp)
    {
        uintptr_t result = (m_regExpAndFlags & flagsMask) | std::bit_cast<uintptr_t>(regExp);
        m_regExpAndFlags = result;
        vm.writeBarrier(this, regExp);
    }

    RegExp* regExp() const
    {
        return std::bit_cast<RegExp*>(m_regExpAndFlags & regExpMask);
    }

    bool setLastIndex(JSGlobalObject* globalObject, uint64_t lastIndex)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

        if (lastIndexIsWritable()) [[likely]] {
            m_lastIndex.setWithoutWriteBarrier(jsNumber(lastIndex));
            return true;
        }
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
        return false;
    }
    bool setLastIndex(JSGlobalObject* globalObject, JSValue lastIndex, bool shouldThrow)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);

        if (lastIndexIsWritable()) [[likely]] {
            m_lastIndex.set(vm, this, lastIndex);
            return true;
        }
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
    }
    JSValue getLastIndex() const
    {
        return m_lastIndex.get();
    }

    bool lastIndexIsWritable() const
    {
        return !(m_regExpAndFlags & lastIndexIsNotWritableFlag);
    }

    bool isSharedLiteral() const { return m_regExpAndFlags & sharedLiteralFlag; }
    // Still exactly what evaluating the literal makes. Nothing in the language can get at a shared object to change that, but the
    // inspector can (it hands out live cells by class): an object that is no longer in this state is not handed out again.
    bool isSharedLiteralInInitialState(Structure* regExpStructure, RegExp* regExp) const
    {
        return m_regExpAndFlags == (std::bit_cast<uintptr_t>(regExp) | sharedLiteralFlag) && structure() == regExpStructure && m_lastIndex.get() == jsNumber(0);
    }
    // For op_new_reg_exp_shared: whether, as things stand in this realm, a literal that is the receiver of a call to its own test
    // (forTest) or exec can only ever reach the original builtin, so that nothing can tell one object from a new one each time.
    static inline bool canShareLiteralAsReceiver(JSGlobalObject*, bool forTest);
    // What op_new_reg_exp_shared evaluates to: the site's object from cachedObject while it can stand for a new one, else a new one.
    static inline JSObject* literalAsReceiver(JSGlobalObject*, CodeBlock*, RegExp*, bool forTest, WriteBarrier<JSCell>& cachedObject);
    JS_EXPORT_PRIVATE static JSObject* literalAsReceiverSlow(JSGlobalObject*, CodeBlock*, RegExp*, bool forTest, WriteBarrier<JSCell>& cachedObject);
    static RegExpObject* createSharedLiteral(VM& vm, Structure* structure, RegExp* regExp)
    {
        RegExpObject* object = create(vm, structure, regExp);
        object->m_regExpAndFlags |= sharedLiteralFlag;
        return object;
    }
    // What evaluating the literal would have made: a new object in its initial state, which a shared one is always in.
    RegExpObject* copyOfSharedLiteral(VM& vm)
    {
        ASSERT(isSharedLiteral());
        return create(vm, structure(), regExp());
    }

    bool test(JSGlobalObject* globalObject, JSString* string) { return !!match(globalObject, string); }
    bool testInline(JSGlobalObject* globalObject, JSString* string) { return !!matchInline(globalObject, string); }
    JS_EXPORT_PRIVATE JSValue exec(JSGlobalObject*, JSString*);
    JSValue execInline(JSGlobalObject*, JSString*);
    JSValue execInline(JSGlobalObject*, JSString*, MatchResult&);
    MatchResult match(JSGlobalObject*, JSString*);
    JSValue matchGlobal(JSGlobalObject*, JSString*);

    bool isSymbolMatchFastAndNonObservable();
    bool isSymbolSearchFastAndNonObservable();
    bool isSymbolMatchAllFastAndNonObservable();
    bool isSymbolReplaceFastAndNonObservable();
    bool isSymbolSplitFastAndNonObservable();

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);

    DECLARE_EXPORT_INFO;

    DECLARE_VISIT_CHILDREN;

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    static constexpr ptrdiff_t offsetOfRegExpAndFlags()
    {
        return OBJECT_OFFSETOF(RegExpObject, m_regExpAndFlags);
    }

    static constexpr ptrdiff_t offsetOfLastIndex()
    {
        return OBJECT_OFFSETOF(RegExpObject, m_lastIndex);
    }

    static size_t allocationSize(Checked<size_t> inlineCapacity)
    {
        ASSERT_UNUSED(inlineCapacity, !inlineCapacity);
        return sizeof(RegExpObject);
    }

    bool areLegacyFeaturesEnabled() const { return !(m_regExpAndFlags & legacyFeaturesDisabledFlag); }

private:
    friend class LLIntOffsetsExtractor;

    JS_EXPORT_PRIVATE RegExpObject(VM&, Structure*, RegExp*, bool areLegacyFeaturesEnabled);
#if ASSERT_ENABLED
    JS_EXPORT_PRIVATE void finishCreation(VM&);
#endif

    void setLastIndexIsNotWritable()
    {
        m_regExpAndFlags = (m_regExpAndFlags | lastIndexIsNotWritableFlag);
    }

    JS_EXPORT_PRIVATE static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    JS_EXPORT_PRIVATE static void getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    JS_EXPORT_PRIVATE static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);

    MatchResult matchInline(JSGlobalObject*, JSString*);

    uintptr_t m_regExpAndFlags { 0 };
    WriteBarrier<Unknown> m_lastIndex;
};

} // namespace JSC
