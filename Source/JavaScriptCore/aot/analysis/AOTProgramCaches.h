/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTPropertyAccessCache.h"
#include "AOTResultRules.h"
#include "AOTSharedTypeMemo.h"
#include <memory>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>

namespace JSC { namespace AOT { namespace Typed {

class ProgramCaches {
    WTF_MAKE_NONCOPYABLE(ProgramCaches);
    WTF_MAKE_NONMOVABLE(ProgramCaches);
public:
    ProgramCaches(const Program& program, BuildContext& build)
        : m_program(program)
        , m_build(build)
    {
        if (m_program.sharedFieldTypes()) {
            RELEASE_ASSERT(m_program.propertyAccessCache());
            return;
        }
        RELEASE_ASSERT(!m_program.propertyAccessCache());
        m_fieldTypes = makeUnique<SharedFieldTypes>();
        m_propertyAccesses = makeUnique<PropertyAccessCache>();
        m_propertyAccesses->checksAnswers = build.options().verifiesAfterEveryPass;
        m_program.setCaches(m_fieldTypes.get(), m_propertyAccesses.get());

        m_answersAboutTypes = m_program.types.takeSavedSharedMemo();
        if (m_answersAboutTypes && !shouldSaveAnswersAboutTypes())
            m_build.destroyLater(std::exchange(m_answersAboutTypes, nullptr));
        if (!m_answersAboutTypes)
            m_answersAboutTypes = createSharedTypeMemo();
        m_answersAboutProgram = createSharedTypeMemo();
        m_program.types.setSharedMemos(m_answersAboutTypes.get(), m_answersAboutProgram.get());
    }

    ~ProgramCaches()
    {
        if (!m_fieldTypes)
            return;
        RELEASE_ASSERT(m_program.sharedFieldTypes() == m_fieldTypes.get() && m_program.propertyAccessCache() == m_propertyAccesses.get());
        m_program.setCaches(nullptr, nullptr);
        m_build.destroyLater(WTF::move(m_fieldTypes));
        m_build.destroyLater(WTF::move(m_propertyAccesses));

        m_program.types.setSharedMemos(nullptr, nullptr);
        if (m_build.options().logging.logsPhaseTimes) [[unlikely]] {
            logCounts("about types"_s, *m_answersAboutTypes);
            logCounts("about the program"_s, *m_answersAboutProgram);
        }
        if (shouldSaveAnswersAboutTypes())
            m_program.types.saveSharedMemo(WTF::move(m_answersAboutTypes));
        else
            m_build.destroyLater(WTF::move(m_answersAboutTypes));
        m_build.destroyLater(WTF::move(m_answersAboutProgram));
    }

private:
    bool shouldSaveAnswersAboutTypes() const { return m_program.form() == IRForm::AsRead || m_program.form() == IRForm::Prepared; }

    std::unique_ptr<SharedTypeMemo> createSharedTypeMemo() const
    {
        return makeUnique<SharedTypeMemo>(m_build.options().verifiesAfterEveryPass, m_build.options().logging.logsPhaseTimes);
    }

    static void logCounts(ASCIILiteral answers, SharedTypeMemo& memo)
    {
        SharedTypeMemo::Counts counts = memo.counts();
        dataLogLn("AOT shared types: answers ", answers, " | ", counts.entries, " held | ", counts.found, " found | ", counts.notFound, " not found | ", counts.fullShards, " parts full");
    }

    const Program& m_program;
    BuildContext& m_build;
    std::unique_ptr<SharedFieldTypes> m_fieldTypes;
    std::unique_ptr<PropertyAccessCache> m_propertyAccesses;
    std::unique_ptr<SharedTypeMemo> m_answersAboutTypes;
    std::unique_ptr<SharedTypeMemo> m_answersAboutProgram;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
