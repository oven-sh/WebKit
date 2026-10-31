/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTNativeTest.h"
#include "AOTRealmTestSupport.h"
#include "AOTState.h"
#include "AOTVector.h"
#include <initializer_list>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC::AOT {

class LoadedProgram {
    WTF_MAKE_NONCOPYABLE(LoadedProgram);
public:
    LoadedProgram();
    ~LoadedProgram();

    Realms& realms() { return m_realms; }
    VM& vm() { return m_realms.vm(); }
    JSGlobalObject* realm() { return m_realms.locked(); }
    Instance& instance() { return *Instance::of(realm()); }
    State& state() { return *instance().state; }

private:
    Realms m_realms;
};

using LoadedProgramTest = void (*)(LoadedProgram&);

// The words are those of an `# OPTIONS:` line of an .ir test, such as "heat=EverythingIsHot".
Vector<uint8_t> compileProgramText(ASCIILiteral programText, std::initializer_list<ASCIILiteral> optionWords = { });
void runWithLoadedProgram(ASCIILiteral testName, ASCIILiteral programText, LoadedProgramTest body, std::initializer_list<ASCIILiteral> optionWords = { });
// The body stops the process, which prints the words before it does.
void runWithLoadedProgramThatStops(ASCIILiteral testName, ASCIILiteral programText, LoadedProgramTest body, ASCIILiteral lastWords);
// No page of a loaded image is writable. A test that wants a table the compiler does not write changes it through this.
void overwriteWordInImage(const uint32_t& word, uint32_t value);

template<std::span<const NativeTest> (*provider)(), const ASCIILiteral& programText, LoadedProgramTest body>
void runNativeTestWithLoadedProgram()
{
    for (const NativeTest& test : provider()) {
        if (test.run == &runNativeTestWithLoadedProgram<provider, programText, body>) {
            runWithLoadedProgram(test.name, programText, body);
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
