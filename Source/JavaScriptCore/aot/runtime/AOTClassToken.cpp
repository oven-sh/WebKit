/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassToken.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "Symbol.h"
#include <wtf/text/SymbolImpl.h>

namespace JSC { namespace AOT {

static ALWAYS_INLINE unsigned nameSlotOf(const JSCellButterfly* butterfly, uint32_t field)
{
    RELEASE_ASSERT(static_cast<uint64_t>(ClassToken::firstNameSlot) + field < butterfly->length());
    return ClassToken::firstNameSlot + field;
}

JSCellButterfly* ClassToken::create(VM& vm, const LayoutData& classLayout)
{
    JSCellButterfly* butterfly = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, firstNameSlot + classLayout.numberOfFields);
    butterfly->setIndex(vm, classIndexSlot, jsNumber(classLayout.index));
    return butterfly;
}

uint32_t ClassToken::classIndexOf(JSCell* token)
{
    return uncheckedDowncast<JSCellButterfly>(token)->get(classIndexSlot).asUInt32();
}

const LayoutData& ClassToken::classLayoutOf(State& state, JSCell* token)
{
    const LayoutData* classLayout = state.objects->classes.classWithIndex(classIndexOf(token));
    RELEASE_ASSERT(classLayout);
    return *classLayout;
}

Symbol* ClassToken::privateNameIfCreated(JSCell* token, uint32_t field)
{
    auto* butterfly = uncheckedDowncast<JSCellButterfly>(token);
    JSValue name = butterfly->get(nameSlotOf(butterfly, field));
    return name ? asSymbol(name) : nullptr;
}

Symbol* ClassToken::privateNameOf(VM& vm, State& state, JSCell* token, uint32_t field)
{
    if (Symbol* name = privateNameIfCreated(token, field))
        return name;
    const ImageText& text = Image::current()->text();
    String description = text.createString(text.identifier(classLayoutOf(state, token).fields()[field].name).text());
    Symbol* name = Symbol::create(vm, PrivateSymbolImpl::create(*description.impl()).get());
    auto* butterfly = uncheckedDowncast<JSCellButterfly>(token);
    butterfly->setIndex(vm, nameSlotOf(butterfly, field), name);
    return name;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
