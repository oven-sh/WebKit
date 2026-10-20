/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTJSONRuntime.h"
#include "AOTNativeTest.h"
#include "AOTRealmTestSupport.h"
#include "AOTStringBuilderStorage.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSONObject.h"
#include "JSTypedArrays.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

enum class TestRealm : uint8_t { Plain, Locked };
enum class TextEncoding : uint8_t { Latin1, UTF16, UTF8 };

inline constexpr std::array<TextEncoding, 3> allTextEncodings { TextEncoding::Latin1, TextEncoding::UTF16, TextEncoding::UTF8 };

inline String text16(std::u16string_view characters)
{
    return String(std::span<const char16_t> { characters });
}

inline bool fitsLatin1(const String& text)
{
    if (text.is8Bit())
        return true;
    for (char16_t character : text.span16()) {
        if (character > 0xff)
            return false;
    }
    return true;
}

inline String to8Bit(const String& text)
{
    AOT_TEST_CHECK(fitsLatin1(text));
    return text.is8Bit() ? text : String::make8Bit(text.span16());
}

inline void* stackPointerAtDepth(unsigned depth)
{
    AOT_TEST_CHECK(depth < 4096);
    return reinterpret_cast<void*>(uintptr_t(1 * MB) - depth * 256);
}

struct SourceWords {
    const uint8_t* start;
    const uint8_t* end;
    void* owner;
    JSONSourceEncoding encoding;
    bool canEndInline;

    friend bool operator==(const SourceWords&, const SourceWords&) = default;
};

inline SourceWords sourceWordsOf(const JSONWords& words)
{
    return { words.sourceStart, words.sourceEnd, words.sourceOwnerStackPointer, words.sourceEncoding, words.canEndSourceInline };
}

inline bool hasZeroPadding(const uint8_t* end)
{
    for (unsigned i = 0; i < jsonSourcePadding; ++i) {
        if (end[i])
            return false;
    }
    return true;
}

class PaddedText {
public:
    explicit PaddedText(std::span<const uint8_t> bytes)
        : m_bytes(bytes.size() + jsonSourcePadding, [&](size_t i) -> uint8_t { return i < bytes.size() ? bytes[i] : 0; })
        , m_size(bytes.size())
    {
    }

    explicit PaddedText(ASCIILiteral literal)
        : PaddedText(byteCast<uint8_t>(literal.span()))
    {
    }

    const uint8_t* start() const { return m_bytes.span().data(); }
    const uint8_t* end() const { return start() + m_size; }
    size_t size() const { return m_size; }

private:
    Vector<uint8_t> m_bytes;
    size_t m_size;
};

class OperationCallScope {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(OperationCallScope);
public:
    explicit OperationCallScope(VM& vm)
        : m_deferGC(vm)
        , m_topCallFrame(vm.topCallFrame, vm.topCallFrame)
        , m_topEntryFrame(vm.topEntryFrame, static_cast<EntryFrame*>(Thread::currentSingleton().stack().origin()))
    {
    }

private:
    DeferGC m_deferGC;
    SetForScope<CallFrame*> m_topCallFrame;
    SetForScope<EntryFrame*> m_topEntryFrame;
};

class DetachedInstance {
    WTF_MAKE_NONCOPYABLE(DetachedInstance);
public:
    explicit DetachedInstance(TestRealm = TestRealm::Plain);
    ~DetachedInstance();

    VM& vm() { return m_realms.vm(); }
    JSGlobalObject* globalObject() { return m_globalObject; }
    Instance& instance() { return m_storage->instance; }
    JSONWords& words() { return m_storage->instance.json; }
    JSONRuntime& runtime() { return *m_runtime; }
    void finalizeCaches(CollectionScope scope) { m_registry.finalize(vm(), scope, CacheValidation::Off); }

    JSValue evaluate(ASCIILiteral source) { return m_realms.evaluate(m_globalObject, source); }
    JSString* string(const String&);
    JSValue bytes(std::span<const uint8_t>);
    JSValue text(const String&, TextEncoding);
    const uint8_t* open(const String& source, TextEncoding, unsigned depth = 1);
    JSValue parse(const String& source) { return JSONParse(globalObject(), source); }
    String stringify(JSValue value) { return JSONStringify(globalObject(), value, 0u); }

    StringBuilderStorage* acquireBuilder() { return StringBuilderStorage::acquire(instance()); }
    String finish(StringBuilderStorage*);

private:
    struct Storage {
        InstancePrefix prefix;
        Instance instance;
    };
    static_assert(std::is_trivially_default_constructible_v<Storage>);
    static_assert(OBJECT_OFFSETOF(Storage, instance) == sizeof(InstancePrefix));

    Realms m_realms;
    JSGlobalObject* m_globalObject;
    std::unique_ptr<Storage> m_storage;
    CacheRegistry m_registry;
    std::optional<JSONRuntime> m_runtime;
};

inline DetachedInstance::DetachedInstance(TestRealm realm)
    : m_globalObject(realm == TestRealm::Locked ? m_realms.locked() : m_realms.plain())
    , m_storage(makeUniqueWithoutFastMallocCheck<Storage>())
{
    AOT_TEST_CHECK(!vm().hasCompiledCode() && !Instance::of(m_globalObject));
    zeroBytes(*m_storage);
    instance().vm = &vm();
    instance().globalObject = m_globalObject;
    m_runtime.emplace(instance(), m_registry);
    AOT_TEST_CHECK(words().runtime == &*m_runtime);

    vm().aotState().stringBuilderStorageStructure.setWithoutWriteBarrier(StringBuilderStorage::createStructure(vm(), nullptr, jsNull()));
    m_registry.addClearedEachCollection("string builder pool"_s, asMutableByteSpan(instance().stringBuilder.freeStorage));
    instance().stringBuilder.isPoolRegistered = true;
}

inline DetachedInstance::~DetachedInstance()
{
    AOT_TEST_CHECK(!vm().exceptionForInspection());
    m_runtime.reset();
}

inline JSString* DetachedInstance::string(const String& value)
{
    return value.length() > 1 ? jsNontrivialString(vm(), value) : jsString(vm(), value);
}

inline JSValue DetachedInstance::bytes(std::span<const uint8_t> span)
{
    auto* array = JSUint8Array::create(globalObject(), globalObject()->typedArrayStructureWithTypedArrayType<TypeUint8>(), span.size());
    AOT_TEST_CHECK(array);
    memcpySpan(array->typedSpan(), span);
    return array;
}

inline JSValue DetachedInstance::text(const String& source, TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Latin1:
        return string(to8Bit(source));
    case TextEncoding::UTF16:
        AOT_TEST_CHECK(!source.is8Bit() && source.length() > 1);
        return string(source);
    case TextEncoding::UTF8: {
        CString utf8 = source.utf8();
        return bytes(byteCast<uint8_t>(utf8.span()));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline const uint8_t* DetachedInstance::open(const String& source, TextEncoding encoding, unsigned depth)
{
    AOT_TEST_CHECK(runtime().open(text(source, encoding), stackPointerAtDepth(depth)));
    return words().sourceStart;
}

inline String DetachedInstance::finish(StringBuilderStorage* builder)
{
    JSString* string = builder->finish(globalObject(), instance(), StringBuilderStorage::StringUse::Unknown);
    AOT_TEST_CHECK(string);
    finalizeCaches(CollectionScope::Eden);
    AOT_TEST_CHECK(!instance().stringBuilder.freeStorage);
    return string->value(globalObject());
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
