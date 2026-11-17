/*
 * Copyright (C) 2016 Apple Inc. All rights reserved.
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

#include "config.h"

#include "Helpers/Test.h"
#include "Helpers/WTFTestUtilities.h"
#include <string>
#include <wtf/MainThread.h>
#include <wtf/URLParser.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/unicode/icu/ICUHelpers.h>

namespace TestWebKitAPI {

class WTF_URLParser : public testing::Test {
public:
    void SetUp() final
    {
        WTF::initializeMainThread();
    }
};

struct ExpectedParts {
    StringView protocol;
    StringView user;
    StringView password;
    StringView host;
    unsigned short port;
    StringView path;
    StringView query;
    StringView fragment;
    StringView string;

    bool isInvalid() const
    {
        return protocol.isEmpty()
            && user.isEmpty()
            && password.isEmpty()
            && host.isEmpty()
            && !port
            && path.isEmpty()
            && query.isEmpty()
            && fragment.isEmpty();
    }
};

bool eq(StringView s1, StringView s2)
{
    EXPECT_EQ(s1, s2);
    return s1.utf8() == s2.utf8();
}

static String insertTabAtLocation(StringView string, size_t location)
{
    ASSERT(location <= string.length());
    return makeString(string.left(location), '\t', string.substring(location));
}

static ExpectedParts invalidParts(StringView urlStringWithTab)
{
    return { ""_s, ""_s, ""_s, ""_s, 0, ""_s , ""_s, ""_s, urlStringWithTab };
}

enum class TestTabs : bool { No, Yes };

// Inserting tabs between surrogate pairs changes the encoded value instead of being skipped by the URLParser.
const TestTabs testTabsValueForSurrogatePairs = TestTabs::No;

static void checkURL(StringView urlString, const ExpectedParts& parts, TestTabs testTabs = TestTabs::Yes)
{
    URL url { urlString.toString() };
    
    EXPECT_TRUE(eq(parts.protocol, url.protocol()));
    EXPECT_TRUE(eq(parts.user, url.user()));
    EXPECT_TRUE(eq(parts.password, url.password()));
    EXPECT_TRUE(eq(parts.host, url.host()));
    EXPECT_EQ(parts.port, url.port().value_or(0));
    EXPECT_TRUE(eq(parts.path, url.path()));
    EXPECT_TRUE(eq(parts.query, url.query()));
    EXPECT_TRUE(eq(parts.fragment, url.fragmentIdentifier()));
    EXPECT_TRUE(eq(parts.string, url.string()));

    EXPECT_TRUE(WTF::URLParser::internalValuesConsistent(url));

    if (testTabs == TestTabs::No)
        return;

    for (size_t i = 0; i < urlString.length(); ++i) {
        String urlStringWithTab = insertTabAtLocation(urlString, i);
        checkURL(urlStringWithTab,
            parts.isInvalid() ? invalidParts(urlStringWithTab) : parts,
            TestTabs::No);
    }
}

static void checkRelativeURL(StringView urlString, StringView baseURL, const ExpectedParts& parts, TestTabs testTabs = TestTabs::Yes)
{
    auto baseURLString = baseURL.toString();
    auto url = URL(URL(baseURLString), urlString.toString());
    
    EXPECT_TRUE(eq(parts.protocol, url.protocol()));
    EXPECT_TRUE(eq(parts.user, url.user()));
    EXPECT_TRUE(eq(parts.password, url.password()));
    EXPECT_TRUE(eq(parts.host, url.host()));
    EXPECT_EQ(parts.port, url.port().value_or(0));
    EXPECT_TRUE(eq(parts.path, url.path()));
    EXPECT_TRUE(eq(parts.query, url.query()));
    EXPECT_TRUE(eq(parts.fragment, url.fragmentIdentifier()));
    EXPECT_TRUE(eq(parts.string, url.string()));
    
    EXPECT_TRUE(WTF::URLParser::internalValuesConsistent(url));
    
    if (testTabs == TestTabs::No)
        return;
    
    for (size_t i = 0; i < urlString.length(); ++i) {
        String urlStringWithTab = insertTabAtLocation(urlString, i);
        checkRelativeURL(urlStringWithTab,
            baseURLString,
            parts.isInvalid() ? invalidParts(urlStringWithTab) : parts,
            TestTabs::No);
    }
}

static void checkURLDifferences(StringView urlString, const ExpectedParts& parts, TestTabs testTabs = TestTabs::Yes)
{
    URL url { urlString.toString() };
    
    EXPECT_TRUE(eq(parts.protocol, url.protocol()));
    EXPECT_TRUE(eq(parts.user, url.user()));
    EXPECT_TRUE(eq(parts.password, url.password()));
    EXPECT_TRUE(eq(parts.host, url.host()));
    EXPECT_EQ(parts.port, url.port().value_or(0));
    EXPECT_TRUE(eq(parts.path, url.path()));
    EXPECT_TRUE(eq(parts.query, url.query()));
    EXPECT_TRUE(eq(parts.fragment, url.fragmentIdentifier()));
    EXPECT_TRUE(eq(parts.string, url.string()));
    
    EXPECT_TRUE(WTF::URLParser::internalValuesConsistent(url));
    
    if (testTabs == TestTabs::No)
        return;
    
    for (size_t i = 0; i < urlString.length(); ++i) {
        String urlStringWithTab = insertTabAtLocation(urlString, i);
        checkURLDifferences(urlStringWithTab,
            parts.isInvalid() ? invalidParts(urlStringWithTab) : parts,
            TestTabs::No);
    }
}

static void checkRelativeURLDifferences(StringView urlString, StringView baseURLString, const ExpectedParts& parts, TestTabs testTabs = TestTabs::Yes)
{
    auto url = URL(URL(baseURLString.toString()), urlString.toString());
    
    EXPECT_TRUE(eq(parts.protocol, url.protocol()));
    EXPECT_TRUE(eq(parts.user, url.user()));
    EXPECT_TRUE(eq(parts.password, url.password()));
    EXPECT_TRUE(eq(parts.host, url.host()));
    EXPECT_EQ(parts.port, url.port().value_or(0));
    EXPECT_TRUE(eq(parts.path, url.path()));
    EXPECT_TRUE(eq(parts.query, url.query()));
    EXPECT_TRUE(eq(parts.fragment, url.fragmentIdentifier()));
    EXPECT_TRUE(eq(parts.string, url.string()));
    
    EXPECT_TRUE(WTF::URLParser::internalValuesConsistent(url));
    
    if (testTabs == TestTabs::No)
        return;
    
    for (size_t i = 0; i < urlString.length(); ++i) {
        String urlStringWithTab = insertTabAtLocation(urlString, i);
        checkRelativeURLDifferences(urlStringWithTab, baseURLString,
            parts.isInvalid() ? invalidParts(urlStringWithTab) : parts,
            TestTabs::No);
    }
}

static void shouldFail(StringView urlString)
{
    checkURL(urlString, { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, urlString });
}

static void shouldFail(StringView urlString, StringView baseString)
{
    checkRelativeURL(urlString, baseString, { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, urlString });
}

TEST_F(WTF_URLParser, Idempotence)
{
    checkURL("a://"_s, { "a"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "a://"_s });
    checkURL("b:///"_s, { "b"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "b:///"_s });
    checkURL("c:/.//"_s, { "c"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, ""_s, "c:/.//"_s });
    checkURL("d:/..//"_s, { "d"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, ""_s, "d:/.//"_s });
    checkURL("e:/../..//"_s, { "e"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, ""_s, "e:/.//"_s });
    checkURL("f:/../../"_s, { "f"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "f:/"_s });
    checkURL("g:/././"_s, { "g"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "g:/"_s });
    checkURL("h:/./../"_s, { "h"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "h:/"_s });
    checkURL("i:/.././"_s, { "i"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "i:/"_s });
    checkURL("j:/./..//"_s, { "j"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, ""_s, "j:/.//"_s });
    checkURL("k:/.././/"_s, { "k"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, ""_s, "k:/.//"_s });
    checkURL("l:/.?"_s, { "l"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "l:/?"_s });
    checkURL("m:/./?"_s, { "m"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "m:/?"_s });
    checkURL("n:/.//?"_s, { "n"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, ""_s, "n:/.//?"_s });
    checkURL("o:/.#"_s, { "o"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "o:/#"_s });
    checkURL("p:/%2e//"_s, { "p"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, ""_s, "p:/.//"_s });
    checkURL("q:/%2e%2e//"_s, { "q"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, ""_s, "q:/.//"_s });
    checkURL("r:/%2e%2e/"_s, { "r"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "r:/"_s });
    checkURL("s:/%2e/"_s, { "s"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "s:/"_s });
    checkURL("t:/.//p/../../../..//x"_s, { "t"_s, ""_s, ""_s, ""_s, 0, "//x"_s, ""_s, ""_s, "t:/.//x"_s });
    checkRelativeURL("../path"_s, "u:/.//p"_s, { "u"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, ""_s, "u:/path"_s });
    checkURL("v:/.//.."_s, { "v"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "v:/"_s });
    checkURL("w:/.//..//"_s, { "w"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, ""_s, "w:/.//"_s });
    checkURL("x:/.//../a"_s, { "x"_s, ""_s, ""_s, ""_s, 0, "/a"_s, ""_s, ""_s, "x:/a"_s });
    checkURL("http://host/./"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURL("http://host/../"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURL("http://host/.../"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/.../"_s, ""_s, ""_s, "http://host/.../"_s });
    checkURL("http://host/.."_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURL("http://host/."_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
}

TEST_F(WTF_URLParser, Basic)
{
    checkURL("http://user:pass@webkit.org:123/path?query#fragment"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 123, "/path"_s, "query"_s, "fragment"_s, "http://user:pass@webkit.org:123/path?query#fragment"_s });
    checkURL("http://user:pass@webkit.org:123/path?query"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 123, "/path"_s, "query"_s, ""_s, "http://user:pass@webkit.org:123/path?query"_s });
    checkURL("http://user:pass@webkit.org:123/path"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 123, "/path"_s, ""_s, ""_s, "http://user:pass@webkit.org:123/path"_s });
    checkURL("http://user:pass@webkit.org:123/"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 123, "/"_s, ""_s, ""_s, "http://user:pass@webkit.org:123/"_s });
    checkURL("http://user:pass@webkit.org:123"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 123, "/"_s, ""_s, ""_s, "http://user:pass@webkit.org:123/"_s });
    checkURL("http://user:pass@webkit.org"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 0, "/"_s, ""_s, ""_s, "http://user:pass@webkit.org/"_s });
    checkURL("http://user:\t\t\tpass@webkit.org"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 0, "/"_s, ""_s, ""_s, "http://user:pass@webkit.org/"_s });
    checkURL("http://us\ter:pass@webkit.org"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 0, "/"_s, ""_s, ""_s, "http://user:pass@webkit.org/"_s });
    checkURL("http://user:pa\tss@webkit.org"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 0, "/"_s, ""_s, ""_s, "http://user:pass@webkit.org/"_s });
    checkURL("http://user:pass\t@webkit.org"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 0, "/"_s, ""_s, ""_s, "http://user:pass@webkit.org/"_s });
    checkURL("http://\tuser:pass@webkit.org"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 0, "/"_s, ""_s, ""_s, "http://user:pass@webkit.org/"_s });
    checkURL("http://user\t:pass@webkit.org"_s, { "http"_s, "user"_s, "pass"_s, "webkit.org"_s, 0, "/"_s, ""_s, ""_s, "http://user:pass@webkit.org/"_s });
    checkURL("http://webkit.org"_s, { "http"_s, ""_s, ""_s, "webkit.org"_s, 0, "/"_s, ""_s, ""_s, "http://webkit.org/"_s });
    checkURL("http://127.0.0.1"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    checkURL("http://webkit.org/"_s, { "http"_s, ""_s, ""_s, "webkit.org"_s, 0, "/"_s, ""_s, ""_s, "http://webkit.org/"_s });
    checkURL("http://webkit.org/path1/path2/index.html"_s, { "http"_s, ""_s, ""_s, "webkit.org"_s, 0, "/path1/path2/index.html"_s, ""_s, ""_s, "http://webkit.org/path1/path2/index.html"_s });
    checkURL("about:blank"_s, { "about"_s, ""_s, ""_s, ""_s, 0, "blank"_s, ""_s, ""_s, "about:blank"_s });
    checkURL("about:blank?query"_s, { "about"_s, ""_s, ""_s, ""_s, 0, "blank"_s, "query"_s, ""_s, "about:blank?query"_s });
    checkURL("about:blank#fragment"_s, { "about"_s, ""_s, ""_s, ""_s, 0, "blank"_s, ""_s, "fragment"_s, "about:blank#fragment"_s });
    checkURL("http://[0::0]/"_s, { "http"_s, ""_s, ""_s, "[::]"_s, 0, "/"_s, ""_s, ""_s, "http://[::]/"_s });
    checkURL("http://[0::]/"_s, { "http"_s, ""_s, ""_s, "[::]"_s, 0, "/"_s, ""_s, ""_s, "http://[::]/"_s });
    checkURL("http://[::]/"_s, { "http"_s, ""_s, ""_s, "[::]"_s, 0, "/"_s, ""_s, ""_s, "http://[::]/"_s });
    checkURL("http://[::0]/"_s, { "http"_s, ""_s, ""_s, "[::]"_s, 0, "/"_s, ""_s, ""_s, "http://[::]/"_s });
    checkURL("http://[::0:0]/"_s, { "http"_s, ""_s, ""_s, "[::]"_s, 0, "/"_s, ""_s, ""_s, "http://[::]/"_s });
    checkURL("http://[f::0:0]/"_s, { "http"_s, ""_s, ""_s, "[f::]"_s, 0, "/"_s, ""_s, ""_s, "http://[f::]/"_s });
    checkURL("http://[f:0::f]/"_s, { "http"_s, ""_s, ""_s, "[f::f]"_s, 0, "/"_s, ""_s, ""_s, "http://[f::f]/"_s });
    checkURL("http://[::0:ff]/"_s, { "http"_s, ""_s, ""_s, "[::ff]"_s, 0, "/"_s, ""_s, ""_s, "http://[::ff]/"_s });
    checkURL("http://[::00:0:0:0]/"_s, { "http"_s, ""_s, ""_s, "[::]"_s, 0, "/"_s, ""_s, ""_s, "http://[::]/"_s });
    checkURL("http://[::0:00:0:0]/"_s, { "http"_s, ""_s, ""_s, "[::]"_s, 0, "/"_s, ""_s, ""_s, "http://[::]/"_s });
    checkURL("http://[::0:0.0.0.0]/"_s, { "http"_s, ""_s, ""_s, "[::]"_s, 0, "/"_s, ""_s, ""_s, "http://[::]/"_s });
    checkURL("http://[0:f::f:f:0:0]"_s, { "http"_s, ""_s, ""_s, "[0:f::f:f:0:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[0:f::f:f:0:0]/"_s });
    checkURL("http://[0:f:0:0:f::]"_s, { "http"_s, ""_s, ""_s, "[0:f:0:0:f::]"_s, 0, "/"_s, ""_s, ""_s, "http://[0:f:0:0:f::]/"_s });
    checkURL("http://[::f:0:0:f:0:0]"_s, { "http"_s, ""_s, ""_s, "[::f:0:0:f:0:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[::f:0:0:f:0:0]/"_s });
    checkURL("http://[0:f:0:0:f::]:"_s, { "http"_s, ""_s, ""_s, "[0:f:0:0:f::]"_s, 0, "/"_s, ""_s, ""_s, "http://[0:f:0:0:f::]/"_s });
    checkURL("http://[0:f:0:0:f::]:\t"_s, { "http"_s, ""_s, ""_s, "[0:f:0:0:f::]"_s, 0, "/"_s, ""_s, ""_s, "http://[0:f:0:0:f::]/"_s });
    checkURL("http://[0:f:0:0:f::]\t:"_s, { "http"_s, ""_s, ""_s, "[0:f:0:0:f::]"_s, 0, "/"_s, ""_s, ""_s, "http://[0:f:0:0:f::]/"_s });
    checkURL("http://\t[::f:0:0:f:0:0]"_s, { "http"_s, ""_s, ""_s, "[::f:0:0:f:0:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[::f:0:0:f:0:0]/"_s });
    checkURL("http://[\t::f:0:0:f:0:0]"_s, { "http"_s, ""_s, ""_s, "[::f:0:0:f:0:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[::f:0:0:f:0:0]/"_s });
    checkURL("http://[:\t:f:0:0:f:0:0]"_s, { "http"_s, ""_s, ""_s, "[::f:0:0:f:0:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[::f:0:0:f:0:0]/"_s });
    checkURL("http://[::\tf:0:0:f:0:0]"_s, { "http"_s, ""_s, ""_s, "[::f:0:0:f:0:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[::f:0:0:f:0:0]/"_s });
    checkURL("http://[::f\t:0:0:f:0:0]"_s, { "http"_s, ""_s, ""_s, "[::f:0:0:f:0:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[::f:0:0:f:0:0]/"_s });
    checkURL("http://[::f:\t0:0:f:0:0]"_s, { "http"_s, ""_s, ""_s, "[::f:0:0:f:0:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[::f:0:0:f:0:0]/"_s });
    checkURL("http://example.com/path1/path2/."_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/path2/"_s, ""_s, ""_s, "http://example.com/path1/path2/"_s });
    checkURL("http://example.com/path1/path2/.."_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/"_s, ""_s, ""_s, "http://example.com/path1/"_s });
    checkURL("http://example.com/path1/path2/./path3"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/path2/path3"_s, ""_s, ""_s, "http://example.com/path1/path2/path3"_s });
    checkURL("http://example.com/path1/path2/.\\path3"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/path2/path3"_s, ""_s, ""_s, "http://example.com/path1/path2/path3"_s });
    checkURL("http://example.com/path1/path2/../path3"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/path3"_s, ""_s, ""_s, "http://example.com/path1/path3"_s });
    checkURL("http://example.com/path1/path2/..\\path3"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/path3"_s, ""_s, ""_s, "http://example.com/path1/path3"_s });
    checkURL("http://example.com/."_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURL("http://example.com/.."_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURL("http://example.com/./path1"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1"_s, ""_s, ""_s, "http://example.com/path1"_s });
    checkURL("http://example.com/../path1"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1"_s, ""_s, ""_s, "http://example.com/path1"_s });
    checkURL("http://example.com/../path1/../../path2/path3/../path4"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path2/path4"_s, ""_s, ""_s, "http://example.com/path2/path4"_s });
    checkURL("http://example.com/path1/.%2"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/.%2"_s, ""_s, ""_s, "http://example.com/path1/.%2"_s });
    checkURL("http://example.com/path1/%2"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/%2"_s, ""_s, ""_s, "http://example.com/path1/%2"_s });
    checkURL("http://example.com/path1/%"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/%"_s, ""_s, ""_s, "http://example.com/path1/%"_s });
    checkURL("http://example.com/path1/.%"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/.%"_s, ""_s, ""_s, "http://example.com/path1/.%"_s });
    checkURL("http://example.com//."_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "//"_s, ""_s, ""_s, "http://example.com//"_s });
    checkURL("http://example.com//./"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "//"_s, ""_s, ""_s, "http://example.com//"_s });
    checkURL("http://example.com//.//"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "///"_s, ""_s, ""_s, "http://example.com///"_s });
    checkURL("http://example.com//.."_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURL("http://example.com//../"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURL("http://example.com//..//"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "//"_s, ""_s, ""_s, "http://example.com//"_s });
    checkURL("http://example.com//.."_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURL("http://example.com/.//"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "//"_s, ""_s, ""_s, "http://example.com//"_s });
    checkURL("http://example.com/..//"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "//"_s, ""_s, ""_s, "http://example.com//"_s });
    checkURL("http://example.com/./"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURL("http://example.com/../"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURL("http://example.com/path1/.../path3"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/.../path3"_s, ""_s, ""_s, "http://example.com/path1/.../path3"_s });
    checkURL("http://example.com/path1/..."_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/..."_s, ""_s, ""_s, "http://example.com/path1/..."_s });
    checkURL("http://example.com/path1/.../"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/.../"_s, ""_s, ""_s, "http://example.com/path1/.../"_s });
    checkURL("http://example.com/.path1/"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/.path1/"_s, ""_s, ""_s, "http://example.com/.path1/"_s });
    checkURL("http://example.com/..path1/"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/..path1/"_s, ""_s, ""_s, "http://example.com/..path1/"_s });
    checkURL("http://example.com/path1/.path2"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/.path2"_s, ""_s, ""_s, "http://example.com/path1/.path2"_s });
    checkURL("http://example.com/path1/..path2"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/..path2"_s, ""_s, ""_s, "http://example.com/path1/..path2"_s });
    checkURL("http://example.com/path1/path2/.?query"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/path2/"_s, "query"_s, ""_s, "http://example.com/path1/path2/?query"_s });
    checkURL("http://example.com/path1/path2/..?query"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/"_s, "query"_s, ""_s, "http://example.com/path1/?query"_s });
    checkURL("http://example.com/path1/path2/.#fragment"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/path2/"_s, ""_s, "fragment"_s, "http://example.com/path1/path2/#fragment"_s });
    checkURL("http://example.com/path1/path2/..#fragment"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/"_s, ""_s, "fragment"_s, "http://example.com/path1/#fragment"_s });

    checkURL("file:"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "file:///"_s });
    checkURL("file:/"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "file:///"_s });
    checkURL("file://"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "file:///"_s });
    checkURL("file:///"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "file:///"_s });
    checkURL("file:////"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, ""_s, "file:////"_s }); // This matches Firefox and URL::parse which I believe are correct, but not Chrome.
    checkURL("file:/path"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, ""_s, "file:///path"_s });
    checkURL("file://host/path"_s, { "file"_s, ""_s, ""_s, "host"_s, 0, "/path"_s, ""_s, ""_s, "file://host/path"_s });
    checkURL("file://host"_s, { "file"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "file://host/"_s });
    checkURL("file://host/"_s, { "file"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "file://host/"_s });
    checkURL("file:///path"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, ""_s, "file:///path"_s });
    checkURL("file:////path"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "//path"_s, ""_s, ""_s, "file:////path"_s });
    checkURL("file://localhost/path"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, ""_s, "file:///path"_s });
    checkURL("file://localhost/"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "file:///"_s });
    checkURL("file://localhost"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "file:///"_s });
    checkURL("file://lOcAlHoSt"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "file:///"_s });
    checkURL("file://lOcAlHoSt/"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "file:///"_s });
    checkURL("file:/pAtH/"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/pAtH/"_s, ""_s, ""_s, "file:///pAtH/"_s });
    checkURL("file:/pAtH"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/pAtH"_s, ""_s, ""_s, "file:///pAtH"_s });
    checkURL("file:?query"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, "query"_s, ""_s, "file:///?query"_s });
    checkURL("file:#fragment"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, "fragment"_s, "file:///#fragment"_s });
    checkURL("file:?query#fragment"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, "query"_s, "fragment"_s, "file:///?query#fragment"_s });
    checkURL("file:#fragment?notquery"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, "fragment?notquery"_s, "file:///#fragment?notquery"_s });
    checkURL("file:/?query"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, "query"_s, ""_s, "file:///?query"_s });
    checkURL("file:/#fragment"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, "fragment"_s, "file:///#fragment"_s });
    checkURL("file://?query"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, "query"_s, ""_s, "file:///?query"_s });
    checkURL("file://#fragment"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, "fragment"_s, "file:///#fragment"_s });
    checkURL("file:///?query"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, "query"_s, ""_s, "file:///?query"_s });
    checkURL("file:///#fragment"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, "fragment"_s, "file:///#fragment"_s });
    checkURL("file:////?query"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "//"_s, "query"_s, ""_s, "file:////?query"_s });
    checkURL("file:////#fragment"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, "fragment"_s, "file:////#fragment"_s });
    checkURL("file://?Q"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, "Q"_s, ""_s, "file:///?Q"_s });
    checkURL("file://#F"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, "F"_s, "file:///#F"_s });
    checkURL("file://host?Q"_s, { "file"_s, ""_s, ""_s, "host"_s, 0, "/"_s, "Q"_s, ""_s, "file://host/?Q"_s });
    checkURL("file://host#F"_s, { "file"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, "F"_s, "file://host/#F"_s });
    checkURL("file://host\\P"_s, { "file"_s, ""_s, ""_s, "host"_s, 0, "/P"_s, ""_s, ""_s, "file://host/P"_s });
    checkURL("file://host\\?Q"_s, { "file"_s, ""_s, ""_s, "host"_s, 0, "/"_s, "Q"_s, ""_s, "file://host/?Q"_s });
    checkURL("file://host\\../P"_s, { "file"_s, ""_s, ""_s, "host"_s, 0, "/P"_s, ""_s, ""_s, "file://host/P"_s });
    checkURL("file://host\\/../P"_s, { "file"_s, ""_s, ""_s, "host"_s, 0, "/P"_s, ""_s, ""_s, "file://host/P"_s });
    checkURL("file://host\\/P"_s, { "file"_s, ""_s, ""_s, "host"_s, 0, "//P"_s, ""_s, ""_s, "file://host//P"_s });
    checkURL("http://host/A b"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/A%20b"_s, ""_s, ""_s, "http://host/A%20b"_s });
    checkURL("http://host/a%20B"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/a%20B"_s, ""_s, ""_s, "http://host/a%20B"_s });
    checkURL("http://host?q=@ <>!#fragment"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, "q=@%20%3C%3E!"_s, "fragment"_s, "http://host/?q=@%20%3C%3E!#fragment"_s });
    checkURL("http://user:@host"_s, { "http"_s, "user"_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://user@host/"_s });
    checkURL("http://user:@\thost"_s, { "http"_s, "user"_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://user@host/"_s });
    checkURL("http://user:\t@host"_s, { "http"_s, "user"_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://user@host/"_s });
    checkURL("http://user\t:@host"_s, { "http"_s, "user"_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://user@host/"_s });
    checkURL("http://use\tr:@host"_s, { "http"_s, "user"_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://user@host/"_s });
    checkURL("http://127.0.0.1:10100/path"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 10100, "/path"_s, ""_s, ""_s, "http://127.0.0.1:10100/path"_s });
    checkURL("http://127.0.0.1:/path"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/path"_s, ""_s, ""_s, "http://127.0.0.1/path"_s });
    checkURL("http://127.0.0.1\t:/path"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/path"_s, ""_s, ""_s, "http://127.0.0.1/path"_s });
    checkURL("http://127.0.0.1:\t/path"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/path"_s, ""_s, ""_s, "http://127.0.0.1/path"_s });
    checkURL("http://127.0.0.1:/\tpath"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/path"_s, ""_s, ""_s, "http://127.0.0.1/path"_s });
    checkURL("http://127.0.0.1:123"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 123, "/"_s, ""_s, ""_s, "http://127.0.0.1:123/"_s });
    checkURL("http://127.0.0.1:"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    shouldFail("ws://08./"_s);
    checkURL("http://[0:f::f:f:0:0]:123/path"_s, { "http"_s, ""_s, ""_s, "[0:f::f:f:0:0]"_s, 123, "/path"_s, ""_s, ""_s, "http://[0:f::f:f:0:0]:123/path"_s });
    checkURL("http://[0:f::f:f:0:0]:123"_s, { "http"_s, ""_s, ""_s, "[0:f::f:f:0:0]"_s, 123, "/"_s, ""_s, ""_s, "http://[0:f::f:f:0:0]:123/"_s });
    checkURL("http://[0:f:0:0:f:\t:]:123"_s, { "http"_s, ""_s, ""_s, "[0:f:0:0:f::]"_s, 123, "/"_s, ""_s, ""_s, "http://[0:f:0:0:f::]:123/"_s });
    checkURL("http://[0:f:0:0:f::\t]:123"_s, { "http"_s, ""_s, ""_s, "[0:f:0:0:f::]"_s, 123, "/"_s, ""_s, ""_s, "http://[0:f:0:0:f::]:123/"_s });
    checkURL("http://[0:f:0:0:f::]\t:123"_s, { "http"_s, ""_s, ""_s, "[0:f:0:0:f::]"_s, 123, "/"_s, ""_s, ""_s, "http://[0:f:0:0:f::]:123/"_s });
    checkURL("http://[0:f:0:0:f::]:\t123"_s, { "http"_s, ""_s, ""_s, "[0:f:0:0:f::]"_s, 123, "/"_s, ""_s, ""_s, "http://[0:f:0:0:f::]:123/"_s });
    checkURL("http://[0:f:0:0:f::]:1\t23"_s, { "http"_s, ""_s, ""_s, "[0:f:0:0:f::]"_s, 123, "/"_s, ""_s, ""_s, "http://[0:f:0:0:f::]:123/"_s });
    checkURL("http://[0:f::f:f:0:0]:/path"_s, { "http"_s, ""_s, ""_s, "[0:f::f:f:0:0]"_s, 0, "/path"_s, ""_s, ""_s, "http://[0:f::f:f:0:0]/path"_s });
    checkURL("a://[::2:]"_s, { "a"_s, ""_s, ""_s, "[::2]"_s, 0, ""_s, ""_s, ""_s, "a://[::2]"_s });
    checkURL("http://[0:f::f:f:0:0]:"_s, { "http"_s, ""_s, ""_s, "[0:f::f:f:0:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[0:f::f:f:0:0]/"_s });
    checkURL("http://host:10100/path"_s, { "http"_s, ""_s, ""_s, "host"_s, 10100, "/path"_s, ""_s, ""_s, "http://host:10100/path"_s });
    checkURL("http://host:/path"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/path"_s, ""_s, ""_s, "http://host/path"_s });
    checkURL("http://host:123"_s, { "http"_s, ""_s, ""_s, "host"_s, 123, "/"_s, ""_s, ""_s, "http://host:123/"_s });
    checkURL("http://host:"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURL("http://hos\tt\n:\t1\n2\t3\t/\npath"_s, { "http"_s, ""_s, ""_s, "host"_s, 123, "/path"_s, ""_s, ""_s, "http://host:123/path"_s });
    checkURL("http://user@example.org/path3"_s, { "http"_s, "user"_s, ""_s, "example.org"_s, 0, "/path3"_s, ""_s, ""_s, "http://user@example.org/path3"_s });
    checkURL("sc:/pa/pa"_s, { "sc"_s, ""_s, ""_s, ""_s, 0, "/pa/pa"_s, ""_s, ""_s, "sc:/pa/pa"_s });
    checkURL("sc:/pa"_s, { "sc"_s, ""_s, ""_s, ""_s, 0, "/pa"_s, ""_s, ""_s, "sc:/pa"_s });
    checkURL("sc:/pa/"_s, { "sc"_s, ""_s, ""_s, ""_s, 0, "/pa/"_s, ""_s, ""_s, "sc:/pa/"_s });
    checkURL("notspecial:/notuser:notpassword@nothost"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, "/notuser:notpassword@nothost"_s, ""_s, ""_s, "notspecial:/notuser:notpassword@nothost"_s });
    checkURL("sc://pa/"_s, { "sc"_s, ""_s, ""_s, "pa"_s, 0, "/"_s, ""_s, ""_s, "sc://pa/"_s });
    checkURL("sc://\tpa/"_s, { "sc"_s, ""_s, ""_s, "pa"_s, 0, "/"_s, ""_s, ""_s, "sc://pa/"_s });
    checkURL("sc:/\t/pa/"_s, { "sc"_s, ""_s, ""_s, "pa"_s, 0, "/"_s, ""_s, ""_s, "sc://pa/"_s });
    checkURL("sc:\t//pa/"_s, { "sc"_s, ""_s, ""_s, "pa"_s, 0, "/"_s, ""_s, ""_s, "sc://pa/"_s });
    checkURL("http://host   \a   "_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURL("notspecial:/a"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, "/a"_s, ""_s, ""_s, "notspecial:/a"_s });
    checkURL("notspecial:"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "notspecial:"_s });
    checkURL("http:/a"_s, { "http"_s, ""_s, ""_s, "a"_s, 0, "/"_s, ""_s, ""_s, "http://a/"_s });
    checkURL("http://256../"_s, { "http"_s, ""_s, ""_s, "256.."_s, 0, "/"_s, ""_s, ""_s, "http://256../"_s });
    checkURL("http://256.."_s, { "http"_s, ""_s, ""_s, "256.."_s, 0, "/"_s, ""_s, ""_s, "http://256../"_s });
    shouldFail("http://127..1/"_s);
    shouldFail("http://127.a.0.1/"_s);
    checkURL("http://127.0.0.1/"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    checkURL("http://12\t7.0.0.1/"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    checkURL("http://127.\t0.0.1/"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    checkURL("http://./"_s, { "http"_s, ""_s, ""_s, "."_s, 0, "/"_s, ""_s, ""_s, "http://./"_s });
    checkURL("http://."_s, { "http"_s, ""_s, ""_s, "."_s, 0, "/"_s, ""_s, ""_s, "http://./"_s });
    checkURL("notspecial:/a"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, "/a"_s, ""_s, ""_s, "notspecial:/a"_s });
    checkURL("notspecial:"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "notspecial:"_s });
    checkURL("notspecial:/"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "notspecial:/"_s });
    checkURL("data:image/png;base64,encoded-data-follows-here"_s, { "data"_s, ""_s, ""_s, ""_s, 0, "image/png;base64,encoded-data-follows-here"_s, ""_s, ""_s, "data:image/png;base64,encoded-data-follows-here"_s });
    checkURL("data:image/png;base64,encoded/data-with-slash"_s, { "data"_s, ""_s, ""_s, ""_s, 0, "image/png;base64,encoded/data-with-slash"_s, ""_s, ""_s, "data:image/png;base64,encoded/data-with-slash"_s });
    checkURL("about:~"_s, { "about"_s, ""_s, ""_s, ""_s, 0, "~"_s, ""_s, ""_s, "about:~"_s });
    checkURL("https://@test@test@example:800\\path@end"_s, { "https"_s, "@test@test"_s, ""_s, "example"_s, 800, "/path@end"_s, ""_s, ""_s, "https://%40test%40test@example:800/path@end"_s });
    checkURL("http://www.example.com/#a\nb\rc\td"_s, { "http"_s, ""_s, ""_s, "www.example.com"_s, 0, "/"_s, ""_s, "abcd"_s, "http://www.example.com/#abcd"_s });
    checkURL("http://[A:b:c:DE:fF:0:1:aC]/"_s, { "http"_s, ""_s, ""_s, "[a:b:c:de:ff:0:1:ac]"_s, 0, "/"_s, ""_s, ""_s, "http://[a:b:c:de:ff:0:1:ac]/"_s });
    checkURL("http:////////user:@webkit.org:99?foo"_s, { "http"_s, "user"_s, ""_s, "webkit.org"_s, 99, "/"_s, "foo"_s, ""_s, "http://user@webkit.org:99/?foo"_s });
    checkURL("http:////////user:@webkit.org:99#foo"_s, { "http"_s, "user"_s, ""_s, "webkit.org"_s, 99, "/"_s, ""_s, "foo"_s, "http://user@webkit.org:99/#foo"_s });
    checkURL("http:////\t////user:@webkit.org:99?foo"_s, { "http"_s, "user"_s, ""_s, "webkit.org"_s, 99, "/"_s, "foo"_s, ""_s, "http://user@webkit.org:99/?foo"_s });
    checkURL("http://\t//\\///user:@webkit.org:99?foo"_s, { "http"_s, "user"_s, ""_s, "webkit.org"_s, 99, "/"_s, "foo"_s, ""_s, "http://user@webkit.org:99/?foo"_s });
    checkURL("http:/\\user:@webkit.org:99?foo"_s, { "http"_s, "user"_s, ""_s, "webkit.org"_s, 99, "/"_s, "foo"_s, ""_s, "http://user@webkit.org:99/?foo"_s });
    checkURL("http://127.0.0.1"_s, { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    checkURLDifferences("http://127.0.0.1."_s,
        { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    checkURLDifferences("http://127.0.0.1./"_s,
        { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    checkURL("http://127.0.0.1../"_s, { "http"_s, ""_s, ""_s, "127.0.0.1.."_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1../"_s });
    checkURLDifferences("http://0x100.0/"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://0x100.0/"_s });
    checkURLDifferences("http://0.0.0x100.0/"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://0.0.0x100.0/"_s });
    checkURLDifferences("http://0.0.0.0x100/"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://0.0.0.0x100/"_s });
    checkURL("http://host:123?"_s, { "http"_s, ""_s, ""_s, "host"_s, 123, "/"_s, ""_s, ""_s, "http://host:123/?"_s });
    checkURL("http://host:123?query"_s, { "http"_s, ""_s, ""_s, "host"_s, 123, "/"_s, "query"_s, ""_s, "http://host:123/?query"_s });
    checkURL("http://host:123#"_s, { "http"_s, ""_s, ""_s, "host"_s, 123, "/"_s, ""_s, ""_s, "http://host:123/#"_s });
    checkURL("http://host:123#fragment"_s, { "http"_s, ""_s, ""_s, "host"_s, 123, "/"_s, ""_s, "fragment"_s, "http://host:123/#fragment"_s });
    checkURLDifferences("foo:////"_s,
        { "foo"_s, ""_s, ""_s, ""_s, 0, "//"_s, ""_s, ""_s, "foo:////"_s });
    checkURLDifferences("foo:///?"_s,
        { "foo"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "foo:///?"_s });
    checkURLDifferences("foo:///#"_s,
        { "foo"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "foo:///#"_s });
    checkURLDifferences("foo:///"_s,
        { "foo"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "foo:///"_s });
    checkURLDifferences("foo://?"_s,
        { "foo"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "foo://?"_s });
    checkURLDifferences("foo://#"_s,
        { "foo"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "foo://#"_s });
    checkURLDifferences("foo://"_s,
        { "foo"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "foo://"_s });
    checkURL("foo:/?"_s, { "foo"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "foo:/?"_s });
    checkURL("foo:/#"_s, { "foo"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "foo:/#"_s });
    checkURL("foo:/"_s, { "foo"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "foo:/"_s });
    checkURL("foo:?"_s, { "foo"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "foo:?"_s });
    checkURL("foo:#"_s, { "foo"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "foo:#"_s });
    checkURLDifferences("A://"_s,
        { "a"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "a://"_s });
    checkURLDifferences("aA://"_s,
        { "aa"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "aa://"_s });
    checkURL(utf16String(u"foo://host/#ПП\u0007 a</"), { "foo"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, "%D0%9F%D0%9F%07%20a%3C/"_s, "foo://host/#%D0%9F%D0%9F%07%20a%3C/"_s });
    checkURL(utf16String(u"foo://host/#\u0007 a</"), { "foo"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, "%07%20a%3C/"_s, "foo://host/#%07%20a%3C/"_s });
    checkURL(utf16String(u"http://host?ß😍#ß😍"), { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, "%C3%9F%F0%9F%98%8D"_s, "%C3%9F%F0%9F%98%8D"_s, "http://host/?%C3%9F%F0%9F%98%8D#%C3%9F%F0%9F%98%8D"_s }, testTabsValueForSurrogatePairs);
    checkURL(utf16String(u"http://host/path#💩\t💩"), { "http"_s, ""_s, ""_s, "host"_s, 0, "/path"_s, ""_s, "%F0%9F%92%A9%F0%9F%92%A9"_s, "http://host/path#%F0%9F%92%A9%F0%9F%92%A9"_s }, testTabsValueForSurrogatePairs);
    checkURL(utf16String(u"http://host/#ПП\u0007 a</"), { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, "%D0%9F%D0%9F%07%20a%3C/"_s, "http://host/#%D0%9F%D0%9F%07%20a%3C/"_s });
    checkURL(utf16String(u"http://host/#\u0007 a</"), { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, "%07%20a%3C/"_s, "http://host/#%07%20a%3C/"_s });

    // This disagrees with the web platform test for http://:@www.example.com but agrees with Chrome and URL::parse,
    // and Firefox fails the web platform test differently. Maybe the web platform test ought to be changed.
    checkURL("http://:@host"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
}

static void testUserPassword(StringView value, StringView decoded, StringView encoded)
{
    URL userURL { makeString("http://"_s, value, "@example.com/"_s) };
    URL passURL { makeString("http://user:"_s, value, "@example.com/"_s) };
    EXPECT_EQ(encoded, userURL.encodedUser());
    EXPECT_EQ(encoded, passURL.encodedPassword());
    EXPECT_EQ(decoded, userURL.user());
    EXPECT_EQ(decoded, passURL.password());
}

static void testUserPassword(StringView value, StringView encoded)
{
    testUserPassword(value, value, encoded);
}

TEST_F(WTF_URLParser, Credentials)
{
    auto validSurrogate = utf16String<3>({0xD800, 0xDD55, '\0'});
    auto invalidSurrogate = utf16String<3>({0xD800, 'A', '\0'});
    auto replacementA = utf16String<3>({0xFFFD, 'A', '\0'});

    testUserPassword("a"_s, "a"_s);
    testUserPassword("%"_s, "%"_s);
    testUserPassword("%25"_s, "%"_s, "%25"_s);
    testUserPassword("%2525"_s, "%25"_s, "%2525"_s);
    testUserPassword("%FX"_s, "%FX"_s);
    testUserPassword("%00"_s, String::fromUTF8({ "\0", 1 }), "%00"_s);
    testUserPassword("%F%25"_s, "%F%"_s, "%F%25"_s);
    testUserPassword("%X%25"_s, "%X%"_s, "%X%25"_s);
    testUserPassword("%%25"_s, "%%"_s, "%%25"_s);
    testUserPassword(StringView::fromLatin1("💩"), "%C3%B0%C2%9F%C2%92%C2%A9"_s);
    testUserPassword(StringView::fromLatin1("%💩"), "%%C3%B0%C2%9F%C2%92%C2%A9"_s);
    testUserPassword(validSurrogate, "%F0%90%85%95"_s);
    testUserPassword(replacementA, "%EF%BF%BDA"_s);
    testUserPassword(invalidSurrogate, replacementA, "%EF%BF%BDA"_s);
}

TEST_F(WTF_URLParser, ParseRelative)
{
    checkRelativeURL("/index.html"_s, "http://webkit.org/path1/path2/"_s, { "http"_s, ""_s, ""_s, "webkit.org"_s, 0, "/index.html"_s, ""_s, ""_s, "http://webkit.org/index.html"_s });
    checkRelativeURL("http://whatwg.org/index.html"_s, "http://webkit.org/path1/path2/"_s, { "http"_s, ""_s, ""_s, "whatwg.org"_s, 0, "/index.html"_s, ""_s, ""_s, "http://whatwg.org/index.html"_s });
    checkRelativeURL("index.html"_s, "http://webkit.org/path1/path2/page.html?query#fragment"_s, { "http"_s, ""_s, ""_s, "webkit.org"_s, 0, "/path1/path2/index.html"_s, ""_s, ""_s, "http://webkit.org/path1/path2/index.html"_s });
    checkRelativeURL("//whatwg.org/index.html"_s, "https://www.webkit.org/path"_s, { "https"_s, ""_s, ""_s, "whatwg.org"_s, 0, "/index.html"_s, ""_s, ""_s, "https://whatwg.org/index.html"_s });
    checkRelativeURL("http://example\t.\norg"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/"_s, ""_s, ""_s, "http://example.org/"_s });
    checkRelativeURL("test"_s, "file:///path1/path2"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path1/test"_s, ""_s, ""_s, "file:///path1/test"_s });
    checkRelativeURL(utf16String(u"http://www.foo。bar.com"), "http://other.com/"_s, { "http"_s, ""_s, ""_s, "www.foo.bar.com"_s, 0, "/"_s, ""_s, ""_s, "http://www.foo.bar.com/"_s });
    checkRelativeURLDifferences(utf16String(u"sc://ñ.test/"), "about:blank"_s,
        { "sc"_s, ""_s, ""_s, "%C3%B1.test"_s, 0, "/"_s, ""_s, ""_s, "sc://%C3%B1.test/"_s });
    checkRelativeURL("#fragment"_s, "http://host/path"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/path"_s, ""_s, "fragment"_s, "http://host/path#fragment"_s });
    checkRelativeURL("#fragment"_s, "file:///path"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, "fragment"_s, "file:///path#fragment"_s });
    checkRelativeURL("#fragment"_s, "file:///path#old"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, "fragment"_s, "file:///path#fragment"_s });
    checkRelativeURL("#"_s, "file:///path#old"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, ""_s, "file:///path#"_s });
    checkRelativeURL("  "_s, "file:///path#old"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, ""_s, "file:///path"_s });
    checkRelativeURL("#"_s, "file:///path"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, ""_s, "file:///path#"_s });
    checkRelativeURL("#"_s, "file:///path?query"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, "query"_s, ""_s, "file:///path?query#"_s });
    checkRelativeURL("#"_s, "file:///path?query#old"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, "query"_s, ""_s, "file:///path?query#"_s });
    checkRelativeURL("?query"_s, "http://host/path"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/path"_s, "query"_s, ""_s, "http://host/path?query"_s });
    checkRelativeURL("?query#fragment"_s, "http://host/path"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/path"_s, "query"_s, "fragment"_s, "http://host/path?query#fragment"_s });
    checkRelativeURL("?new"_s, "file:///path?old#fragment"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, "new"_s, ""_s, "file:///path?new"_s });
    checkRelativeURL("?"_s, "file:///path?old#fragment"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, ""_s, "file:///path?"_s });
    checkRelativeURL("?"_s, "file:///path"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, ""_s, "file:///path?"_s });
    checkRelativeURL("?query"_s, "file:///path"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, "query"_s, ""_s, "file:///path?query"_s });
    checkRelativeURL(utf16String(u"?β"), "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, "%CE%B2"_s, ""_s, "http://example.org/foo/bar?%CE%B2"_s });
    checkRelativeURL("?"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, ""_s, ""_s, "http://example.org/foo/bar?"_s });
    checkRelativeURL("#"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, ""_s, ""_s, "http://example.org/foo/bar#"_s });
    checkRelativeURL("?#"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, ""_s, ""_s, "http://example.org/foo/bar?#"_s });
    checkRelativeURL("#?"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, ""_s, "?"_s, "http://example.org/foo/bar#?"_s });
    checkRelativeURL("/"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/"_s, ""_s, ""_s, "http://example.org/"_s });
    checkRelativeURL("http://@host"_s, "about:blank"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkRelativeURL("http://:@host"_s, "about:blank"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkRelativeURL("http://foo.com/\\@"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "foo.com"_s, 0, "//@"_s, ""_s, ""_s, "http://foo.com//@"_s });
    checkRelativeURL("\\@"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/@"_s, ""_s, ""_s, "http://example.org/@"_s });
    checkRelativeURL("/path3"_s, "http://user@example.org/path1/path2"_s, { "http"_s, "user"_s, ""_s, "example.org"_s, 0, "/path3"_s, ""_s, ""_s, "http://user@example.org/path3"_s });
    checkRelativeURL(""_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, ""_s, ""_s, "http://example.org/foo/bar"_s });
    checkRelativeURL("\t"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, ""_s, ""_s, "http://example.org/foo/bar"_s });
    checkRelativeURL(" "_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, ""_s, ""_s, "http://example.org/foo/bar"_s });
    checkRelativeURL("  \a  \t\n"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, ""_s, ""_s, "http://example.org/foo/bar"_s });
    checkRelativeURL(":foo.com\\"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/:foo.com/"_s, ""_s, ""_s, "http://example.org/foo/:foo.com/"_s });
    checkRelativeURL("http:/example.com/"_s, "about:blank"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkRelativeURL("http:example.com/"_s, "about:blank"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkRelativeURL("http:\\\\foo.com\\"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "foo.com"_s, 0, "/"_s, ""_s, ""_s, "http://foo.com/"_s });
    checkRelativeURL("http:\\\\foo.com/"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "foo.com"_s, 0, "/"_s, ""_s, ""_s, "http://foo.com/"_s });
    checkRelativeURL("http:\\\\foo.com"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "foo.com"_s, 0, "/"_s, ""_s, ""_s, "http://foo.com/"_s });
    checkRelativeURL("http://ExAmPlE.CoM"_s, "http://other.com"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkRelativeURL("http:"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, ""_s, ""_s, "http://example.org/foo/bar"_s });
    checkRelativeURL("#x"_s, "data:,"_s, { "data"_s, ""_s, ""_s, ""_s, 0, ","_s, ""_s, "x"_s, "data:,#x"_s });
    checkRelativeURL("#x"_s, "about:blank"_s, { "about"_s, ""_s, ""_s, ""_s, 0, "blank"_s, ""_s, "x"_s, "about:blank#x"_s });
    checkRelativeURL("  foo.com  "_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/foo.com"_s, ""_s, ""_s, "http://example.org/foo/foo.com"_s });
    checkRelativeURL(" \a baz"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/baz"_s, ""_s, ""_s, "http://example.org/foo/baz"_s });
    checkRelativeURL("~"_s, "http://example.org"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/~"_s, ""_s, ""_s, "http://example.org/~"_s });
    checkRelativeURL("notspecial:"_s, "about:blank"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "notspecial:"_s });
    checkRelativeURL("notspecial:"_s, "http://host"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "notspecial:"_s });
    checkRelativeURL("http:"_s, "http://host"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkRelativeURL("i"_s, "sc:/pa/po"_s, { "sc"_s, ""_s, ""_s, ""_s, 0, "/pa/i"_s, ""_s, ""_s, "sc:/pa/i"_s });
    checkRelativeURL("i    "_s, "sc:/pa/po"_s, { "sc"_s, ""_s, ""_s, ""_s, 0, "/pa/i"_s, ""_s, ""_s, "sc:/pa/i"_s });
    checkRelativeURL("i\t\n  "_s, "sc:/pa/po"_s, { "sc"_s, ""_s, ""_s, ""_s, 0, "/pa/i"_s, ""_s, ""_s, "sc:/pa/i"_s });
    checkRelativeURL("i"_s, "sc://ho/pa"_s, { "sc"_s, ""_s, ""_s, "ho"_s, 0, "/i"_s, ""_s, ""_s, "sc://ho/i"_s });
    checkRelativeURL("!"_s, "sc://ho/pa"_s, { "sc"_s, ""_s, ""_s, "ho"_s, 0, "/!"_s, ""_s, ""_s, "sc://ho/!"_s });
    checkRelativeURL("!"_s, "sc:/ho/pa"_s, { "sc"_s, ""_s, ""_s, ""_s, 0, "/ho/!"_s, ""_s, ""_s, "sc:/ho/!"_s });
    checkRelativeURL("notspecial:/"_s, "about:blank"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "notspecial:/"_s });
    checkRelativeURL("notspecial:/"_s, "http://host"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "notspecial:/"_s });
    checkRelativeURL("foo:/"_s, "http://example.org/foo/bar"_s, { "foo"_s, ""_s, ""_s, ""_s, 0, "/"_s, ""_s, ""_s, "foo:/"_s });
    checkRelativeURL("://:0/"_s, "http://webkit.org/"_s, { "http"_s, ""_s, ""_s, "webkit.org"_s, 0, "/://:0/"_s, ""_s, ""_s, "http://webkit.org/://:0/"_s });
    checkRelativeURL(String(), "http://webkit.org/"_s, { "http"_s, ""_s, ""_s, "webkit.org"_s, 0, "/"_s, ""_s, ""_s, "http://webkit.org/"_s });
    checkRelativeURL("https://@test@test@example:800\\path@end"_s, "http://doesnotmatter/"_s, { "https"_s, "@test@test"_s, ""_s, "example"_s, 800, "/path@end"_s, ""_s, ""_s, "https://%40test%40test@example:800/path@end"_s });
    checkRelativeURL("http://f:0/c"_s, "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "f"_s, 0, "/c"_s, ""_s, ""_s, "http://f:0/c"_s });
    checkRelativeURL(String(), "http://host/#fragment"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkRelativeURL(""_s, "http://host/#fragment"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkRelativeURL("  "_s, "http://host/#fragment"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkRelativeURL("  "_s, "http://host/path?query#fra#gment"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/path"_s, "query"_s, ""_s, "http://host/path?query"_s });
    checkRelativeURL(" \a "_s, "http://host/#fragment"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkRelativeURLDifferences("foo://"_s, "http://example.org/foo/bar"_s,
        { "foo"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "foo://"_s });
    checkRelativeURL(utf16String(u"#β"), "http://example.org/foo/bar"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, ""_s, "%CE%B2"_s, "http://example.org/foo/bar#%CE%B2"_s });
    checkRelativeURL("index.html"_s, "applewebdata://Host/"_s, { "applewebdata"_s, ""_s, ""_s, "Host"_s, 0, "/index.html"_s, ""_s, ""_s, "applewebdata://Host/index.html"_s });
    checkRelativeURL("index.html"_s, "applewebdata://Host"_s, { "applewebdata"_s, ""_s, ""_s, "Host"_s, 0, "/index.html"_s, ""_s, ""_s, "applewebdata://Host/index.html"_s });
    checkRelativeURL(""_s, "applewebdata://Host"_s, { "applewebdata"_s, ""_s, ""_s, "Host"_s, 0, ""_s, ""_s, ""_s, "applewebdata://Host"_s });
    checkRelativeURL("?query"_s, "applewebdata://Host"_s, { "applewebdata"_s, ""_s, ""_s, "Host"_s, 0, ""_s, "query"_s, ""_s, "applewebdata://Host?query"_s });
    checkRelativeURL("#fragment"_s, "applewebdata://Host"_s, { "applewebdata"_s, ""_s, ""_s, "Host"_s, 0, ""_s, ""_s, "fragment"_s, "applewebdata://Host#fragment"_s });
    checkRelativeURL("notspecial://something?"_s, "file:////var//containers//stuff/"_s, { "notspecial"_s, ""_s, ""_s, "something"_s, 0, ""_s, ""_s, ""_s, "notspecial://something?"_s }, TestTabs::No);
    checkRelativeURL("notspecial://something#"_s, "file:////var//containers//stuff/"_s, { "notspecial"_s, ""_s, ""_s, "something"_s, 0, ""_s, ""_s, ""_s, "notspecial://something#"_s }, TestTabs::No);
    checkRelativeURL("http://something?"_s, "file:////var//containers//stuff/"_s, { "http"_s, ""_s, ""_s, "something"_s, 0, "/"_s, ""_s, ""_s, "http://something/?"_s }, TestTabs::No);
    checkRelativeURL("http://something#"_s, "file:////var//containers//stuff/"_s, { "http"_s, ""_s, ""_s, "something"_s, 0, "/"_s, ""_s, ""_s, "http://something/#"_s }, TestTabs::No);
    checkRelativeURL("file:"_s, "file:///path?query#fragment"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, "query"_s, ""_s, "file:///path?query"_s });
    checkRelativeURL("/"_s, "file:///C:/a/b"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/C:/"_s, ""_s, ""_s, "file:///C:/"_s });
    checkRelativeURL("/abc"_s, "file:///C:/a/b"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/C:/abc"_s, ""_s, ""_s, "file:///C:/abc"_s });
    checkRelativeURL("/abc"_s, "file:///C:"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/C:/abc"_s, ""_s, ""_s, "file:///C:/abc"_s });
    checkRelativeURL("/abc"_s, "file:///"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/abc"_s, ""_s, ""_s, "file:///abc"_s });
    checkRelativeURL("//d:"_s, "file:///C:/a/b"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/d:"_s, ""_s, ""_s, "file:///d:"_s }, TestTabs::No);
    checkRelativeURL("//d|"_s, "file:///C:/a/b"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/d:"_s, ""_s, ""_s, "file:///d:"_s }, TestTabs::No);
    checkRelativeURL("//A|"_s, "file:///C:/a/b"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/A:"_s, ""_s, ""_s, "file:///A:"_s }, TestTabs::No);

    // The checking of slashes in SpecialAuthoritySlashes needed to get this to pass contradicts what is in the spec,
    // but it is included in the web platform tests.
    checkRelativeURL("http:\\\\host\\foo"_s, "about:blank"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/foo"_s, ""_s, ""_s, "http://host/foo"_s });
}

// These are differences between the new URLParser and the old URL::parse which make URLParser more standards compliant.
TEST_F(WTF_URLParser, ParserDifferences)
{
    checkURLDifferences("http://127.0.1"_s,
        { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    checkURLDifferences("http://011.11.0X11.0x011"_s,
        { "http"_s, ""_s, ""_s, "9.11.17.17"_s, 0, "/"_s, ""_s, ""_s, "http://9.11.17.17/"_s });
    checkURLDifferences("http://[1234:0078:90AB:CdEf:0123:0007:89AB:0000]"_s,
        { "http"_s, ""_s, ""_s, "[1234:78:90ab:cdef:123:7:89ab:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[1234:78:90ab:cdef:123:7:89ab:0]/"_s });
    checkURLDifferences("http://[0:f:0:0:f:f:0:0]"_s,
        { "http"_s, ""_s, ""_s, "[0:f::f:f:0:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[0:f::f:f:0:0]/"_s });
    checkURLDifferences("http://[0:f:0:0:f:0:0:0]"_s,
        { "http"_s, ""_s, ""_s, "[0:f:0:0:f::]"_s, 0, "/"_s, ""_s, ""_s, "http://[0:f:0:0:f::]/"_s });
    checkURLDifferences("http://[0:0:f:0:0:f:0:0]"_s,
        { "http"_s, ""_s, ""_s, "[::f:0:0:f:0:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[::f:0:0:f:0:0]/"_s });
    checkURLDifferences("http://[a:0:0:0:b:c::d]"_s,
        { "http"_s, ""_s, ""_s, "[a::b:c:0:d]"_s, 0, "/"_s, ""_s, ""_s, "http://[a::b:c:0:d]/"_s });
    checkURLDifferences("http://[::7f00:0001]/"_s,
        { "http"_s, ""_s, ""_s, "[::7f00:1]"_s, 0, "/"_s, ""_s, ""_s, "http://[::7f00:1]/"_s });
    checkURLDifferences("http://[::7f00:00]/"_s,
        { "http"_s, ""_s, ""_s, "[::7f00:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[::7f00:0]/"_s });
    checkURLDifferences("http://[::0:7f00:0001]/"_s,
        { "http"_s, ""_s, ""_s, "[::7f00:1]"_s, 0, "/"_s, ""_s, ""_s, "http://[::7f00:1]/"_s });
    checkURLDifferences("http://127.00.0.1/"_s,
        { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    checkURLDifferences("http://127.0.0.01/"_s,
        { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    checkURLDifferences("http://example.com/path1/.%2e"_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURLDifferences("http://example.com/path1/.%2E"_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURLDifferences("http://example.com/path1/.%2E/"_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURLDifferences("http://example.com/path1/%2e."_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURLDifferences("http://example.com/path1/%2E%2e"_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
    checkURLDifferences("http://example.com/path1/%2e"_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/"_s, ""_s, ""_s, "http://example.com/path1/"_s });
    checkURLDifferences("http://example.com/path1/%2E"_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/"_s, ""_s, ""_s, "http://example.com/path1/"_s });
    checkURLDifferences("http://example.com/path1/%2E/"_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/"_s, ""_s, ""_s, "http://example.com/path1/"_s });
    checkURLDifferences("http://example.com/path1/path2/%2e?query"_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/path2/"_s, "query"_s, ""_s, "http://example.com/path1/path2/?query"_s });
    checkURLDifferences("http://example.com/path1/path2/%2e%2e?query"_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/"_s, "query"_s, ""_s, "http://example.com/path1/?query"_s });
    checkURLDifferences("http://example.com/path1/path2/%2e#fragment"_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/path2/"_s, ""_s, "fragment"_s, "http://example.com/path1/path2/#fragment"_s });
    checkURLDifferences("http://example.com/path1/path2/%2e%2e#fragment"_s,
        { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/"_s, ""_s, "fragment"_s, "http://example.com/path1/#fragment"_s });
    checkURL("http://example.com/path1/path2/A%2e%2e#fragment"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/path1/path2/A%2e%2e"_s, ""_s, "fragment"_s, "http://example.com/path1/path2/A%2e%2e#fragment"_s });
    checkURLDifferences("file://[0:a:0:0:b:c:0:0]/path"_s,
        { "file"_s, ""_s, ""_s, "[0:a::b:c:0:0]"_s, 0, "/path"_s, ""_s, ""_s, "file://[0:a::b:c:0:0]/path"_s });
    checkURLDifferences("http://"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://"_s });
    checkRelativeURLDifferences("//"_s, "https://www.webkit.org/path"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "//"_s });
    checkURLDifferences("http://127.0.0.1:65536/path"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://127.0.0.1:65536/path"_s });
    checkURLDifferences("http://host:65536"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://host:65536"_s });
    checkURLDifferences("http://127.0.0.1:65536"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://127.0.0.1:65536"_s });
    checkURLDifferences("http://[0:f::f:f:0:0]:65536"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[0:f::f:f:0:0]:65536"_s });
    checkRelativeURLDifferences(":foo.com\\"_s, "notspecial://example.org/foo/bar"_s,
        { "notspecial"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/:foo.com\\"_s, ""_s, ""_s, "notspecial://example.org/foo/:foo.com\\"_s });
    checkURL("sc://pa"_s, { "sc"_s, ""_s, ""_s, "pa"_s, 0, ""_s, ""_s, ""_s, "sc://pa"_s });
    checkRelativeURLDifferences("notspecial:\\\\foo.com\\"_s, "http://example.org/foo/bar"_s,
        { "notspecial"_s, ""_s, ""_s, ""_s, 0, "\\\\foo.com\\"_s, ""_s, ""_s, "notspecial:\\\\foo.com\\"_s });
    checkRelativeURLDifferences("notspecial:\\\\foo.com/"_s, "http://example.org/foo/bar"_s,
        { "notspecial"_s, ""_s, ""_s, ""_s, 0, "\\\\foo.com/"_s, ""_s, ""_s, "notspecial:\\\\foo.com/"_s });
    checkRelativeURLDifferences("notspecial:\\\\foo.com"_s, "http://example.org/foo/bar"_s,
        { "notspecial"_s, ""_s, ""_s, ""_s, 0, "\\\\foo.com"_s, ""_s, ""_s, "notspecial:\\\\foo.com"_s });
    checkURLDifferences("file://notuser:notpassword@test"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "file://notuser:notpassword@test"_s });
    checkURLDifferences("file://notuser:notpassword@test/"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "file://notuser:notpassword@test/"_s });
    checkRelativeURLDifferences("http:/"_s, "about:blank"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http:/"_s });
    checkRelativeURL("http:"_s, "about:blank"_s, { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http:"_s });
    checkRelativeURLDifferences("http:/"_s, "http://host"_s,
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURLDifferences("http:/"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http:/"_s });
    checkURL("http:"_s, { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http:"_s });
    checkRelativeURLDifferences("http:/example.com/"_s, "http://example.org/foo/bar"_s,
        { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/example.com/"_s, ""_s, ""_s, "http://example.org/example.com/"_s });

    // This behavior matches Chrome and Firefox, but not WebKit using URL::parse.
    // The behavior of URL::parse is clearly wrong because reparsing file://path would make path the host.
    // The spec is unclear.
    checkURLDifferences("file:path"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/path"_s, ""_s, ""_s, "file:///path"_s });
    checkURLDifferences("file:pAtH"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/pAtH"_s, ""_s, ""_s, "file:///pAtH"_s });
    checkURLDifferences("file:pAtH/"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/pAtH/"_s, ""_s, ""_s, "file:///pAtH/"_s });
    checkURLDifferences("http://example.com%A0"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://example.com%A0"_s });
    checkURLDifferences("http://%E2%98%83"_s,
        { "http"_s, ""_s, ""_s, "xn--n3h"_s, 0, "/"_s, ""_s, ""_s, "http://xn--n3h/"_s });
    checkURLDifferences("http://host%73"_s,
        { "http"_s, ""_s, ""_s, "hosts"_s, 0, "/"_s, ""_s, ""_s, "http://hosts/"_s });
    checkURLDifferences("http://host%53"_s,
        { "http"_s, ""_s, ""_s, "hosts"_s, 0, "/"_s, ""_s, ""_s, "http://hosts/"_s });
    checkURLDifferences("http://%"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://%"_s });
    checkURLDifferences("http://%7"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://%7"_s });
    checkURLDifferences("http://%7s"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://%7s"_s });
    checkURLDifferences("http://%73"_s,
        { "http"_s, ""_s, ""_s, "s"_s, 0, "/"_s, ""_s, ""_s, "http://s/"_s });
    checkURLDifferences("http://abcdefg%"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://abcdefg%"_s });
    checkURLDifferences("http://abcd%7Xefg"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://abcd%7Xefg"_s });
    checkURL(StringView::fromLatin1("ws://äAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), { "ws"_s, ""_s, ""_s, "xn--aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-rsb254a"_s, 0, "/"_s, ""_s, ""_s, "ws://xn--aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-rsb254a/"_s }, TestTabs::No);
    checkURL(StringView::fromLatin1("ws://äAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), { "ws"_s, ""_s, ""_s, "xn--aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-stb515a"_s, 0, "/"_s, ""_s, ""_s, "ws://xn--aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-stb515a/"_s }, TestTabs::No);
    checkURL(StringView::fromLatin1("ws://&äAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), { "ws"_s, ""_s, ""_s, "xn--&aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-ssb254a"_s, 0, "/"_s, ""_s, ""_s, "ws://xn--&aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-ssb254a/"_s }, TestTabs::No);
    checkURL(StringView::fromLatin1("ws://&äAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), { "ws"_s, ""_s, ""_s, "xn--&aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-ttb515a"_s, 0, "/"_s, ""_s, ""_s, "ws://xn--&aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-ttb515a/"_s }, TestTabs::No);

    // URLParser matches Chrome and the spec, but not URL::parse or Firefox.
    checkURLDifferences(utf16String(u"http://０Ｘｃ０．０２５０．０１"),
        { "http"_s, ""_s, ""_s, "192.168.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://192.168.0.1/"_s });

    checkURL("http://host/path%2e.%2E"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/path%2e.%2E"_s, ""_s, ""_s, "http://host/path%2e.%2E"_s });

    checkRelativeURLDifferences(utf16String(u"http://foo:💩@example.com/bar"), "http://other.com/"_s,
        { "http"_s, "foo"_s, utf16String(u"💩"), "example.com"_s, 0, "/bar"_s, ""_s, ""_s, "http://foo:%F0%9F%92%A9@example.com/bar"_s }, testTabsValueForSurrogatePairs);
    checkRelativeURLDifferences("http://&a:foo(b]c@d:2/"_s, "http://example.org/foo/bar"_s,
        { "http"_s, "&a"_s, "foo(b]c"_s, "d"_s, 2, "/"_s, ""_s, ""_s, "http://&a:foo(b%5Dc@d:2/"_s });
    checkRelativeURLDifferences("http://`{}:`{}@h/`{}?`{}"_s, "http://doesnotmatter/"_s,
        { "http"_s, "`{}"_s, "`{}"_s, "h"_s, 0, "/%60%7B%7D"_s, "`{}"_s, ""_s, "http://%60%7B%7D:%60%7B%7D@h/%60%7B%7D?`{}"_s });
    checkURLDifferences("http://[0:f::f::f]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[0:f::f::f]"_s });
    checkURLDifferences("http://123"_s,
        { "http"_s, ""_s, ""_s, "0.0.0.123"_s, 0, "/"_s, ""_s, ""_s, "http://0.0.0.123/"_s });
    checkURLDifferences("http://123.234/"_s,
        { "http"_s, ""_s, ""_s, "123.0.0.234"_s, 0, "/"_s, ""_s, ""_s, "http://123.0.0.234/"_s });
    checkURLDifferences("http://123.234.012"_s,
        { "http"_s, ""_s, ""_s, "123.234.0.10"_s, 0, "/"_s, ""_s, ""_s, "http://123.234.0.10/"_s });
    checkURLDifferences("http://123.234.12"_s,
        { "http"_s, ""_s, ""_s, "123.234.0.12"_s, 0, "/"_s, ""_s, ""_s, "http://123.234.0.12/"_s });
    checkRelativeURLDifferences("file:c:\\foo\\bar.html"_s, "file:///tmp/mock/path"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/c:/foo/bar.html"_s, ""_s, ""_s, "file:///c:/foo/bar.html"_s });
    checkRelativeURLDifferences("  File:c|////foo\\bar.html"_s, "file:///tmp/mock/path"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/c:////foo/bar.html"_s, ""_s, ""_s, "file:///c:////foo/bar.html"_s });
    checkRelativeURLDifferences("  Fil\t\n\te\n\t\n:\t\n\tc\t\n\t|\n\t\n/\t\n\t/\n\t\n//foo\\bar.html"_s, "file:///tmp/mock/path"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/c:////foo/bar.html"_s, ""_s, ""_s, "file:///c:////foo/bar.html"_s });
    checkRelativeURLDifferences("C|/foo/bar"_s, "file:///tmp/mock/path"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/C:/foo/bar"_s, ""_s, ""_s, "file:///C:/foo/bar"_s });
    checkRelativeURLDifferences("/C|/foo/bar"_s, "file:///tmp/mock/path"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/C:/foo/bar"_s, ""_s, ""_s, "file:///C:/foo/bar"_s });
    checkRelativeURLDifferences("https://@test@test@example:800/"_s, "http://doesnotmatter/"_s,
        { "https"_s, "@test@test"_s, ""_s, "example"_s, 800, "/"_s, ""_s, ""_s, "https://%40test%40test@example:800/"_s });
    checkRelativeURLDifferences("https://@test@test@example:800/path@end"_s, "http://doesnotmatter/"_s,
        { "https"_s, "@test@test"_s, ""_s, "example"_s, 800, "/path@end"_s, ""_s, ""_s, "https://%40test%40test@example:800/path@end"_s });
    checkURLDifferences("notspecial://@test@test@example:800/path@end"_s,
        { "notspecial"_s, "@test@test"_s, ""_s, "example"_s, 800, "/path@end"_s, ""_s, ""_s, "notspecial://%40test%40test@example:800/path@end"_s });
    checkURLDifferences("notspecial://@test@test@example:800\\path@end"_s,
        { "notspecial"_s, "@test@test@example"_s, "800\\path"_s, "end"_s, 0, ""_s, ""_s, ""_s, "notspecial://%40test%40test%40example:800%5Cpath@end"_s });
    checkURLDifferences("http://%48OsT"_s,
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURLDifferences("http://h%4FsT"_s,
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURLDifferences("http://h%4fsT"_s,
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURLDifferences("http://h%6fsT"_s,
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURLDifferences("http://host/`"_s,
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/%60"_s, ""_s, ""_s, "http://host/%60"_s });
    checkURLDifferences("http://://"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://://"_s });
    checkURLDifferences("http://:123?"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://:123?"_s });
    checkURLDifferences("http:/:"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http:/:"_s });
    checkURLDifferences("asdf://:"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "asdf://:"_s });
    checkURLDifferences("http://:"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://:"_s });
    checkURLDifferences("http:##foo"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http:##foo"_s });
    checkURLDifferences("http:??bar"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http:??bar"_s });
    checkURL("asdf:##foo"_s, { "asdf"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, "#foo"_s, "asdf:##foo"_s });
    checkURL("asdf:??bar"_s, { "asdf"_s, ""_s, ""_s, ""_s, 0, ""_s, "?bar"_s, ""_s, "asdf:??bar"_s });
    checkRelativeURLDifferences("//C|/foo/bar"_s, "file:///tmp/mock/path"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/C:/foo/bar"_s, ""_s, ""_s, "file:///C:/foo/bar"_s });
    checkRelativeURLDifferences("//C:/foo/bar"_s, "file:///tmp/mock/path"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/C:/foo/bar"_s, ""_s, ""_s, "file:///C:/foo/bar"_s });
    checkRelativeURLDifferences("//C|?foo/bar"_s, "file:///tmp/mock/path"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/C:/"_s, "foo/bar"_s, ""_s, "file:///C:/?foo/bar"_s });
    checkRelativeURLDifferences("//C|#foo/bar"_s, "file:///tmp/mock/path"_s,
        { "file"_s, ""_s, ""_s, ""_s, 0, "/C:/"_s, ""_s, "foo/bar"_s, "file:///C:/#foo/bar"_s });
    checkURLDifferences("http://0xFFFFFfFF/"_s,
        { "http"_s, ""_s, ""_s, "255.255.255.255"_s, 0, "/"_s, ""_s, ""_s, "http://255.255.255.255/"_s });
    checkURLDifferences("http://0000000000000000037777777777/"_s,
        { "http"_s, ""_s, ""_s, "255.255.255.255"_s, 0, "/"_s, ""_s, ""_s, "http://255.255.255.255/"_s });
    checkURLDifferences("http://4294967295/"_s,
        { "http"_s, ""_s, ""_s, "255.255.255.255"_s, 0, "/"_s, ""_s, ""_s, "http://255.255.255.255/"_s });
    checkURLDifferences("http://256/"_s,
        { "http"_s, ""_s, ""_s, "0.0.1.0"_s, 0, "/"_s, ""_s, ""_s, "http://0.0.1.0/"_s });
    checkURLDifferences("http://256./"_s,
        { "http"_s, ""_s, ""_s, "0.0.1.0"_s, 0, "/"_s, ""_s, ""_s, "http://0.0.1.0/"_s });
    checkURLDifferences("http://123.256/"_s,
        { "http"_s, ""_s, ""_s, "123.0.1.0"_s, 0, "/"_s, ""_s, ""_s, "http://123.0.1.0/"_s });
    checkURLDifferences("http://127.%.0.1/"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://127.%.0.1/"_s });
    checkURLDifferences("http://[1:2:3:4:5:6:7:8:]/"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[1:2:3:4:5:6:7:8:]/"_s });
    checkURLDifferences("http://[:2:3:4:5:6:7:8:]/"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[:2:3:4:5:6:7:8:]/"_s });
    checkURLDifferences("http://[1:2:3:4:5:6:7::]/"_s,
        { "http"_s, ""_s, ""_s, "[1:2:3:4:5:6:7:0]"_s, 0, "/"_s, ""_s, ""_s, "http://[1:2:3:4:5:6:7:0]/"_s });
    checkURLDifferences("http://[1:2:3:4:5:6:7:::]/"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[1:2:3:4:5:6:7:::]/"_s });
    checkURLDifferences("http://127.0.0.1~/"_s,
        { "http"_s, ""_s, ""_s, "127.0.0.1~"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1~/"_s });
    checkURLDifferences("http://127.0.1~/"_s,
        { "http"_s, ""_s, ""_s, "127.0.1~"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.1~/"_s });
    checkURLDifferences("http://127.0.1./"_s,
        { "http"_s, ""_s, ""_s, "127.0.0.1"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.0.1/"_s });
    checkURLDifferences("http://127.0.1.~/"_s,
        { "http"_s, ""_s, ""_s, "127.0.1.~"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.1.~/"_s });
    checkURLDifferences("http://127.0.1.~"_s,
        { "http"_s, ""_s, ""_s, "127.0.1.~"_s, 0, "/"_s, ""_s, ""_s, "http://127.0.1.~/"_s });
    checkRelativeURLDifferences("http://f:000/c"_s, "http://example.org/foo/bar"_s,
        { "http"_s, ""_s, ""_s, "f"_s, 0, "/c"_s, ""_s, ""_s, "http://f:0/c"_s });
    checkRelativeURLDifferences("http://f:010/c"_s, "http://example.org/foo/bar"_s,
        { "http"_s, ""_s, ""_s, "f"_s, 10, "/c"_s, ""_s, ""_s, "http://f:10/c"_s });
    checkURL("notspecial://HoSt"_s, { "notspecial"_s, ""_s, ""_s, "HoSt"_s, 0, ""_s, ""_s, ""_s, "notspecial://HoSt"_s });
    checkURL("notspecial://H%6FSt"_s, { "notspecial"_s, ""_s, ""_s, "H%6FSt"_s, 0, ""_s, ""_s, ""_s, "notspecial://H%6FSt"_s });
    checkURL("notspecial://H%4fSt"_s, { "notspecial"_s, ""_s, ""_s, "H%4fSt"_s, 0, ""_s, ""_s, ""_s, "notspecial://H%4fSt"_s });
    checkURLDifferences(utf16String(u"notspecial://H😍ßt"),
        { "notspecial"_s, ""_s, ""_s, "H%F0%9F%98%8D%C3%9Ft"_s, 0, ""_s, ""_s, ""_s, "notspecial://H%F0%9F%98%8D%C3%9Ft"_s }, testTabsValueForSurrogatePairs);
    checkURLDifferences("http://[ffff:aaaa:cccc:eeee:bbbb:dddd:255.255.255.255]/"_s,
        { "http"_s, ""_s, ""_s, "[ffff:aaaa:cccc:eeee:bbbb:dddd:ffff:ffff]"_s, 0, "/"_s, ""_s, ""_s, "http://[ffff:aaaa:cccc:eeee:bbbb:dddd:ffff:ffff]/"_s }, TestTabs::No);
    checkURLDifferences("http://[::123.234.12.210]/"_s,
        { "http"_s, ""_s, ""_s, "[::7bea:cd2]"_s, 0, "/"_s, ""_s, ""_s, "http://[::7bea:cd2]/"_s });
    checkURLDifferences("http://[::a:255.255.255.255]/"_s,
        { "http"_s, ""_s, ""_s, "[::a:ffff:ffff]"_s, 0, "/"_s, ""_s, ""_s, "http://[::a:ffff:ffff]/"_s });
    checkURLDifferences("http://[::0.00.255.255]/"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[::0.00.255.255]/"_s });
    checkURLDifferences("http://[::0.0.255.255]/"_s,
        { "http"_s, ""_s, ""_s, "[::ffff]"_s, 0, "/"_s, ""_s, ""_s, "http://[::ffff]/"_s });
    checkURLDifferences("http://[::0:1.0.255.255]/"_s,
        { "http"_s, ""_s, ""_s, "[::100:ffff]"_s, 0, "/"_s, ""_s, ""_s, "http://[::100:ffff]/"_s });
    checkURLDifferences("http://[::A:1.0.255.255]/"_s,
        { "http"_s, ""_s, ""_s, "[::a:100:ffff]"_s, 0, "/"_s, ""_s, ""_s, "http://[::a:100:ffff]/"_s });
    checkURLDifferences("http://[:127.0.0.1]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[:127.0.0.1]"_s });
    checkURLDifferences("http://[127.0.0.1]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[127.0.0.1]"_s });
    checkURLDifferences("http://[a:b:c:d:e:f:127.0.0.1]"_s,
        { "http"_s, ""_s, ""_s, "[a:b:c:d:e:f:7f00:1]"_s, 0, "/"_s, ""_s, ""_s, "http://[a:b:c:d:e:f:7f00:1]/"_s });
    checkURLDifferences("http://[a:b:c:d:e:f:127.0.0.101]"_s,
        { "http"_s, ""_s, ""_s, "[a:b:c:d:e:f:7f00:65]"_s, 0, "/"_s, ""_s, ""_s, "http://[a:b:c:d:e:f:7f00:65]/"_s });
    checkURLDifferences("http://[::a:b:c:d:e:f:127.0.0.1]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[::a:b:c:d:e:f:127.0.0.1]"_s });
    checkURLDifferences("http://[a:b::c:d:e:f:127.0.0.1]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[a:b::c:d:e:f:127.0.0.1]"_s });
    checkURLDifferences("http://[a:b:c:d:e:127.0.0.1]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[a:b:c:d:e:127.0.0.1]"_s });
    checkURLDifferences("http://[a:b:c:d:e:f:127.0.0.0.1]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[a:b:c:d:e:f:127.0.0.0.1]"_s });
    checkURLDifferences("http://[a:b:c:d:e:f:127.0.1]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[a:b:c:d:e:f:127.0.1]"_s });
    checkURLDifferences("http://[a:b:c:d:e:f:127.0.0.011]"_s, // Chrome treats this as octal, Firefox and the spec fail
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[a:b:c:d:e:f:127.0.0.011]"_s });
    checkURLDifferences("http://[a:b:c:d:e:f:127.0.00.1]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[a:b:c:d:e:f:127.0.00.1]"_s });
    checkURLDifferences("http://[a:b:c:d:e:f:127.0.0.1.]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[a:b:c:d:e:f:127.0.0.1.]"_s });
    checkURLDifferences("http://[a:b:c:d:e:f:127.0..0.1]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[a:b:c:d:e:f:127.0..0.1]"_s });
    checkURLDifferences("http://[a:b:c:d:e:f::127.0.0.1]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[a:b:c:d:e:f::127.0.0.1]"_s });
    checkURLDifferences("http://[a:b:c:d:e::127.0.0.1]"_s,
        { "http"_s, ""_s, ""_s, "[a:b:c:d:e:0:7f00:1]"_s, 0, "/"_s, ""_s, ""_s, "http://[a:b:c:d:e:0:7f00:1]/"_s });
    checkURLDifferences("http://[a:b:c:d::e:127.0.0.1]"_s,
        { "http"_s, ""_s, ""_s, "[a:b:c:d:0:e:7f00:1]"_s, 0, "/"_s, ""_s, ""_s, "http://[a:b:c:d:0:e:7f00:1]/"_s });
    checkURLDifferences("http://[a:b:c:d:e:f::127.0.0.]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[a:b:c:d:e:f::127.0.0.]"_s });
    checkURLDifferences("http://[a:b:c:d:e:f::127.0.0.256]"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://[a:b:c:d:e:f::127.0.0.256]"_s });
    checkURLDifferences("http://123456"_s, { "http"_s, ""_s, ""_s, "0.1.226.64"_s, 0, "/"_s, ""_s, ""_s, "http://0.1.226.64/"_s });
    checkURL("asdf://123456"_s, { "asdf"_s, ""_s, ""_s, "123456"_s, 0, ""_s, ""_s, ""_s, "asdf://123456"_s });
    checkURLDifferences("http://[0:0:0:0:a:b:c:d]"_s,
        { "http"_s, ""_s, ""_s, "[::a:b:c:d]"_s, 0, "/"_s, ""_s, ""_s, "http://[::a:b:c:d]/"_s });
    checkURLDifferences("asdf://[0:0:0:0:a:b:c:d]"_s,
        { "asdf"_s, ""_s, ""_s, "[::a:b:c:d]"_s, 0, ""_s, ""_s, ""_s, "asdf://[::a:b:c:d]"_s }, TestTabs::No);
    shouldFail("a://%:a"_s);
    checkURL("a://%:/"_s, { "a"_s, ""_s, ""_s, "%"_s, 0, "/"_s, ""_s, ""_s, "a://%/"_s });
    checkURL("a://%:"_s, { "a"_s, ""_s, ""_s, "%"_s, 0, ""_s, ""_s, ""_s, "a://%"_s });
    checkURL("a://%:1/"_s, { "a"_s, ""_s, ""_s, "%"_s, 1, "/"_s, ""_s, ""_s, "a://%:1/"_s });
    checkURLDifferences("http://%:"_s,
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "http://%:"_s });
    checkURL("a://123456"_s, { "a"_s, ""_s, ""_s, "123456"_s, 0, ""_s, ""_s, ""_s, "a://123456"_s });
    checkURL("a://123456:7"_s, { "a"_s, ""_s, ""_s, "123456"_s, 7, ""_s, ""_s, ""_s, "a://123456:7"_s });
    checkURL("a://123456:7/"_s, { "a"_s, ""_s, ""_s, "123456"_s, 7, "/"_s, ""_s, ""_s, "a://123456:7/"_s });
    checkURL("a://A"_s, { "a"_s, ""_s, ""_s, "A"_s, 0, ""_s, ""_s, ""_s, "a://A"_s });
    checkURL("a://A:2"_s, { "a"_s, ""_s, ""_s, "A"_s, 2, ""_s, ""_s, ""_s, "a://A:2"_s });
    checkURL("a://A:2/"_s, { "a"_s, ""_s, ""_s, "A"_s, 2, "/"_s, ""_s, ""_s, "a://A:2/"_s });
    checkURLDifferences(StringView::fromLatin1(reinterpret_cast<const char*>(u8"asd://ß")),
        { "asd"_s, ""_s, ""_s, "%C3%83%C2%9F"_s, 0, ""_s, ""_s, ""_s, "asd://%C3%83%C2%9F"_s }, TestTabs::No);
    checkURLDifferences(StringView::fromLatin1(reinterpret_cast<const char*>(u8"asd://ß:4")),
        { "asd"_s, ""_s, ""_s, "%C3%83%C2%9F"_s, 4, ""_s, ""_s, ""_s, "asd://%C3%83%C2%9F:4"_s }, TestTabs::No);
    checkURLDifferences(StringView::fromLatin1(reinterpret_cast<const char*>(u8"asd://ß:4/")),
        { "asd"_s, ""_s, ""_s, "%C3%83%C2%9F"_s, 4, "/"_s, ""_s, ""_s, "asd://%C3%83%C2%9F:4/"_s }, TestTabs::No);
    checkURLDifferences("a://[A::b]:4"_s,
        { "a"_s, ""_s, ""_s, "[a::b]"_s, 4, ""_s, ""_s, ""_s, "a://[a::b]:4"_s });
    shouldFail("http://[~]"_s);
    shouldFail("a://[~]"_s);
    checkRelativeURLDifferences("a://b"_s, "//[aBc]"_s,
        { "a"_s, ""_s, ""_s, "b"_s, 0, ""_s, ""_s, ""_s, "a://b"_s });
    checkURL(utf16String(u"http://öbb.at"), { "http"_s, ""_s, ""_s, "xn--bb-eka.at"_s, 0, "/"_s, ""_s, ""_s, "http://xn--bb-eka.at/"_s });
    checkURL(utf16String(u"http://ÖBB.at"), { "http"_s, ""_s, ""_s, "xn--bb-eka.at"_s, 0, "/"_s, ""_s, ""_s, "http://xn--bb-eka.at/"_s });
    checkURL(utf16String(u"http://√.com"), { "http"_s, ""_s, ""_s, "xn--19g.com"_s, 0, "/"_s, ""_s, ""_s, "http://xn--19g.com/"_s });
    checkURLDifferences(utf16String(u"http://faß.de"),
        { "http"_s, ""_s, ""_s, "xn--fa-hia.de"_s, 0, "/"_s, ""_s, ""_s, "http://xn--fa-hia.de/"_s });
    checkURL(utf16String(u"http://ԛәлп.com"), { "http"_s, ""_s, ""_s, "xn--k1ai47bhi.com"_s, 0, "/"_s, ""_s, ""_s, "http://xn--k1ai47bhi.com/"_s });
    checkURLDifferences(utf16String(u"http://Ⱥbby.com"),
        { "http"_s, ""_s, ""_s, "xn--bby-iy0b.com"_s, 0, "/"_s, ""_s, ""_s, "http://xn--bby-iy0b.com/"_s });
    if (WTF::ICU::majorVersion() >= 76) {
        checkURLDifferences(utf16String(u"http://\u2132"),
            { "http"_s, ""_s, ""_s, "xn--73g"_s, 0, "/"_s, ""_s, ""_s, utf16String(u"http://xn--73g/") });
    } else {
        checkURLDifferences(utf16String(u"http://\u2132"),
            { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, utf16String(u"http://Ⅎ") });
    }
    checkURLDifferences(utf16String(u"http://\u05D9\u05B4\u05D5\u05D0\u05B8/"),
        { "http"_s, ""_s, ""_s, "xn--cdbi5etas"_s, 0, "/"_s, ""_s, ""_s, "http://xn--cdbi5etas/"_s }, TestTabs::No);
    checkURLDifferences(utf16String(u"http://bidirectional\u0786\u07AE\u0782\u07B0\u0795\u07A9\u0793\u07A6\u0783\u07AA/"),
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, utf16String(u"http://bidirectionalކޮންޕީޓަރު/") }, TestTabs::No);
    checkURLDifferences(utf16String(u"http://contextj\u200D"),
        { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, utf16String(u"http://contextj\u200D") });
    checkURL(utf16String(u"http://contexto\u30FB"), { "http"_s, ""_s, ""_s, "xn--contexto-wg5g"_s, 0, "/"_s, ""_s, ""_s, "http://xn--contexto-wg5g/"_s });
    checkURLDifferences(utf16String(u"http://\u321D\u321E/"),
        { "http"_s, ""_s, ""_s, "xn--()()-bs0sc174agx4b"_s, 0, "/"_s, ""_s, ""_s, "http://xn--()()-bs0sc174agx4b/"_s });
}

TEST_F(WTF_URLParser, DefaultPort)
{
    checkURL("FtP://host:21/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURL("ftp://host:21/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURL("f\ttp://host:21/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURL("f\ttp://host\t:21/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURL("f\ttp://host:\t21/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURL("f\ttp://host:2\t1/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURL("f\ttp://host:21\t/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURL("ftp://host\t:21/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURL("ftp://host:\t21/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURL("ftp://host:2\t1/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURL("ftp://host:21\t/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURL("ftp://host:22/"_s, { "ftp"_s, ""_s, ""_s, "host"_s, 22, "/"_s, ""_s, ""_s, "ftp://host:22/"_s });
    checkURLDifferences("ftp://host:21"_s,
        { "ftp"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ftp://host/"_s });
    checkURLDifferences("ftp://host:22"_s,
        { "ftp"_s, ""_s, ""_s, "host"_s, 22, "/"_s, ""_s, ""_s, "ftp://host:22/"_s });
    
    checkURL("gOpHeR://host:70/"_s, { "gopher"_s, ""_s, ""_s, "host"_s, 70, "/"_s, ""_s, ""_s, "gopher://host:70/"_s });
    checkURL("gopher://host:70/"_s, { "gopher"_s, ""_s, ""_s, "host"_s, 70, "/"_s, ""_s, ""_s, "gopher://host:70/"_s });
    checkURL("gopher://host:71/"_s, { "gopher"_s, ""_s, ""_s, "host"_s, 71, "/"_s, ""_s, ""_s, "gopher://host:71/"_s });
    
    checkURL("hTtP://host:80"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURL("http://host:80"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURL("http://host:80/"_s, { "http"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "http://host/"_s });
    checkURL("http://host:81"_s, { "http"_s, ""_s, ""_s, "host"_s, 81, "/"_s, ""_s, ""_s, "http://host:81/"_s });
    checkURL("http://host:81/"_s, { "http"_s, ""_s, ""_s, "host"_s, 81, "/"_s, ""_s, ""_s, "http://host:81/"_s });
    
    checkURL("hTtPs://host:443"_s, { "https"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "https://host/"_s });
    checkURL("https://host:443"_s, { "https"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "https://host/"_s });
    checkURL("https://host:443/"_s, { "https"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "https://host/"_s });
    checkURL("https://host:444"_s, { "https"_s, ""_s, ""_s, "host"_s, 444, "/"_s, ""_s, ""_s, "https://host:444/"_s });
    checkURL("https://host:444/"_s, { "https"_s, ""_s, ""_s, "host"_s, 444, "/"_s, ""_s, ""_s, "https://host:444/"_s });
    
    checkURL("wS://host:80/"_s, { "ws"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ws://host/"_s });
    checkURL("ws://host:80/"_s, { "ws"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ws://host/"_s });
    checkURL("ws://host:81/"_s, { "ws"_s, ""_s, ""_s, "host"_s, 81, "/"_s, ""_s, ""_s, "ws://host:81/"_s });
    // URLParser matches Chrome and Firefox, but not URL::parse
    checkURLDifferences("ws://host:80"_s,
        { "ws"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "ws://host/"_s });
    checkURLDifferences("ws://host:81"_s,
        { "ws"_s, ""_s, ""_s, "host"_s, 81, "/"_s, ""_s, ""_s, "ws://host:81/"_s });
    
    checkURL("WsS://host:443/"_s, { "wss"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "wss://host/"_s });
    checkURL("wss://host:443/"_s, { "wss"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "wss://host/"_s });
    checkURL("wss://host:444/"_s, { "wss"_s, ""_s, ""_s, "host"_s, 444, "/"_s, ""_s, ""_s, "wss://host:444/"_s });
    // URLParser matches Chrome and Firefox, but not URL::parse
    checkURLDifferences("wss://host:443"_s,
        { "wss"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "wss://host/"_s });
    checkURLDifferences("wss://host:444"_s,
        { "wss"_s, ""_s, ""_s, "host"_s, 444, "/"_s, ""_s, ""_s, "wss://host:444/"_s });

    checkURL("fTpS://host:990/"_s, { "ftps"_s, ""_s, ""_s, "host"_s, 990, "/"_s, ""_s, ""_s, "ftps://host:990/"_s });
    checkURL("ftps://host:990/"_s, { "ftps"_s, ""_s, ""_s, "host"_s, 990, "/"_s, ""_s, ""_s, "ftps://host:990/"_s });
    checkURL("ftps://host:991/"_s, { "ftps"_s, ""_s, ""_s, "host"_s, 991, "/"_s, ""_s, ""_s, "ftps://host:991/"_s });
    checkURL("ftps://host:990"_s, { "ftps"_s, ""_s, ""_s, "host"_s, 990, ""_s, ""_s, ""_s, "ftps://host:990"_s });
    checkURL("ftps://host:991"_s, { "ftps"_s, ""_s, ""_s, "host"_s, 991, ""_s, ""_s, ""_s, "ftps://host:991"_s });

    checkURL("uNkNoWn://host:80/"_s, { "unknown"_s, ""_s, ""_s, "host"_s, 80, "/"_s, ""_s, ""_s, "unknown://host:80/"_s });
    checkURL("unknown://host:80/"_s, { "unknown"_s, ""_s, ""_s, "host"_s, 80, "/"_s, ""_s, ""_s, "unknown://host:80/"_s });
    checkURL("unknown://host:81/"_s, { "unknown"_s, ""_s, ""_s, "host"_s, 81, "/"_s, ""_s, ""_s, "unknown://host:81/"_s });
    checkURL("unknown://host:80"_s, { "unknown"_s, ""_s, ""_s, "host"_s, 80, ""_s, ""_s, ""_s, "unknown://host:80"_s });
    checkURL("unknown://host:81"_s, { "unknown"_s, ""_s, ""_s, "host"_s, 81, ""_s, ""_s, ""_s, "unknown://host:81"_s });

    checkURL("file://host/"_s, { "file"_s, ""_s, ""_s, "host"_s, 0, "/"_s, ""_s, ""_s, "file://host/"_s });
    checkURL("file://host:"_s, { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "file://host:"_s });
    checkURL("file://host:0"_s, { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "file://host:0"_s });
    checkURL("file://host:80"_s, { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "file://host:80"_s });
    checkURL("file://host:80/path"_s, { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "file://host:80/path"_s });
    checkURL("file://:80/path"_s, { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "file://:80/path"_s });
    checkURL("file://:0/path"_s, { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "file://:0/path"_s });
    
    checkURL("http://example.com:0000000000000077"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 77, "/"_s, ""_s, ""_s, "http://example.com:77/"_s });
    checkURL("http://example.com:0000000000000080"_s, { "http"_s, ""_s, ""_s, "example.com"_s, 0, "/"_s, ""_s, ""_s, "http://example.com/"_s });
}

TEST_F(WTF_URLParser, ParserFailures)
{
    shouldFail("    "_s);
    shouldFail("  \a  "_s);
    shouldFail(""_s);
    shouldFail(String());
    shouldFail(""_s, "about:blank"_s);
    shouldFail(String(), "about:blank"_s);
    shouldFail("http://127.0.0.1:abc"_s);
    shouldFail("http://host:abc"_s);
    shouldFail("http://:abc"_s);
    shouldFail("http://a:@"_s, "about:blank"_s);
    shouldFail("http://:b@"_s, "about:blank"_s);
    shouldFail("http://:@"_s, "about:blank"_s);
    shouldFail("http://a:@"_s);
    shouldFail("http://:b@"_s);
    shouldFail("http://@"_s);
    shouldFail("http://[0:f::f:f:0:0]:abc"_s);
    shouldFail("../i"_s, "sc:sd"_s);
    shouldFail("../i"_s, "sc:sd/sd"_s);
    shouldFail("/i"_s, "sc:sd"_s);
    shouldFail("/i"_s, "sc:sd/sd"_s);
    shouldFail("?i"_s, "sc:sd"_s);
    shouldFail("?i"_s, "sc:sd/sd"_s);
    shouldFail("http://example example.com"_s, "http://other.com/"_s);
    shouldFail("http://[www.example.com]/"_s, "about:blank"_s);
    shouldFail("http://192.168.0.1 hello"_s, "http://other.com/"_s);
    shouldFail("http://[example.com]"_s, "http://other.com/"_s);
    shouldFail("i"_s, "sc:sd"_s);
    shouldFail("i"_s, "sc:sd/sd"_s);
    shouldFail("i"_s);
    shouldFail("asdf"_s);
    shouldFail("~"_s);
    shouldFail("%"_s);
    shouldFail("//%"_s);
    shouldFail("~"_s, "about:blank"_s);
    shouldFail("~~~"_s);
    shouldFail("://:0/"_s);
    shouldFail("://:0/"_s, ""_s);
    shouldFail("://:0/"_s, "about:blank"_s);
    shouldFail("about~"_s);
    shouldFail("//C:asdf/foo/bar"_s, "file:///tmp/mock/path"_s);
    shouldFail("wss://[c::]abc/"_s);
    shouldFail("abc://[c::]:abc/"_s);
    shouldFail("abc://[c::]01"_s);
    shouldFail("http://[1234::ab#]"_s);
    shouldFail("http://[1234::ab/]"_s);
    shouldFail("http://[1234::ab?]"_s);
    shouldFail("http://[1234::ab@]"_s);
    shouldFail("http://[1234::ab~]"_s);
    shouldFail("http://[2001::1"_s);
    shouldFail(StringView::fromLatin1("http://4:b\xE1"));
    shouldFail("http://[1:2:3:4:5:6:7:8~]/"_s);
    shouldFail("http://[a:b:c:d:e:f:g:127.0.0.1]"_s);
    shouldFail("http://[a:b:c:d:e:f:g:h:127.0.0.1]"_s);
    shouldFail("http://[a:b:c:d:e:f:127.0.0.0x11]"_s); // Chrome treats this as hex, Firefox and the spec fail
    shouldFail("http://[a:b:c:d:e:f:127.0.-0.1]"_s);
    shouldFail("asdf://space In\aHost"_s);
    shouldFail("asdf://[0:0:0:0:a:b:c:d"_s);
    shouldFail("http://[::0:0.0.00000.0]/"_s);
}

// These are in the spec but not in the web platform tests.
TEST_F(WTF_URLParser, AdditionalTests)
{
    checkURL("about:\a\aabc"_s, { "about"_s, ""_s, ""_s, ""_s, 0, "%07%07abc"_s, ""_s, ""_s, "about:%07%07abc"_s });
    checkURL("notspecial:\t\t\n\t"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "notspecial:"_s });
    checkURL("notspecial\t\t\n\t:\t\t\n\t/\t\t\n\t/\t\t\n\thost"_s, { "notspecial"_s, ""_s, ""_s, "host"_s, 0, ""_s, ""_s, ""_s, "notspecial://host"_s });
    checkRelativeURL("http:"_s, "http://example.org/foo/bar?query#fragment"_s, { "http"_s, ""_s, ""_s, "example.org"_s, 0, "/foo/bar"_s, "query"_s, ""_s, "http://example.org/foo/bar?query"_s });
    checkRelativeURL("ws:"_s, "http://example.org/foo/bar"_s, { ""_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "ws:"_s });
    checkRelativeURL("notspecial:"_s, "http://example.org/foo/bar"_s, { "notspecial"_s, ""_s, ""_s, ""_s, 0, ""_s, ""_s, ""_s, "notspecial:"_s });

    const wchar_t surrogateBegin = 0xD800;
    const wchar_t validSurrogateEnd = 0xDD55;
    const wchar_t invalidSurrogateEnd = 'A';
    const wchar_t replacementCharacter = 0xFFFD;
    checkURL(utf16String<12>({'h', 't', 't', 'p', ':', '/', '/', 'w', '/', surrogateBegin, validSurrogateEnd, '\0'}),
        { "http"_s, ""_s, ""_s, "w"_s, 0, "/%F0%90%85%95"_s, ""_s, ""_s, "http://w/%F0%90%85%95"_s }, testTabsValueForSurrogatePairs);
    shouldFail(utf16String<10>({'h', 't', 't', 'p', ':', '/', surrogateBegin, invalidSurrogateEnd, '/', '\0'}));
    shouldFail(utf16String<9>({'h', 't', 't', 'p', ':', '/', replacementCharacter, '/', '\0'}));
    // A surrogate pair in a host split by a tab is two unpaired surrogates, like elsewhere in the URL.
    checkURL(utf16String<10>({'h', 't', 't', 'p', ':', '/', '/', 0xD83D, 0xDCA9, '\0'}),
        { "http"_s, ""_s, ""_s, "xn--ls8h"_s, 0, "/"_s, ""_s, ""_s, "http://xn--ls8h/"_s }, testTabsValueForSurrogatePairs);
    shouldFail(utf16String<11>({'h', 't', 't', 'p', ':', '/', '/', 0xD83D, '\t', 0xDCA9, '\0'}));
    shouldFail(utf16String<12>({'f', 'i', 'l', 'e', ':', '/', '/', 0xD83D, '\n', 0xDCA9, '/', '\0'}));
    
    // URLParser matches Chrome and Firefox but not URL::parse.
    checkURLDifferences(utf16String<12>({'h', 't', 't', 'p', ':', '/', '/', 'w', '/', surrogateBegin, invalidSurrogateEnd}),
        { "http"_s, ""_s, ""_s, "w"_s, 0, "/%EF%BF%BDA"_s, ""_s, ""_s, "http://w/%EF%BF%BDA"_s });
    checkURLDifferences(utf16String<13>({'h', 't', 't', 'p', ':', '/', '/', 'w', '/', '?', surrogateBegin, invalidSurrogateEnd, '\0'}),
        { "http"_s, ""_s, ""_s, "w"_s, 0, "/"_s, "%EF%BF%BDA"_s, ""_s, "http://w/?%EF%BF%BDA"_s });
    checkURLDifferences(utf16String<11>({'h', 't', 't', 'p', ':', '/', '/', 'w', '/', surrogateBegin, '\0'}),
        { "http"_s, ""_s, ""_s, "w"_s, 0, "/%EF%BF%BD"_s, ""_s, ""_s, "http://w/%EF%BF%BD"_s });
    checkURLDifferences(utf16String<12>({'h', 't', 't', 'p', ':', '/', '/', 'w', '/', '?', surrogateBegin, '\0'}),
        { "http"_s, ""_s, ""_s, "w"_s, 0, "/"_s, "%EF%BF%BD"_s, ""_s, "http://w/?%EF%BF%BD"_s });
    checkURLDifferences(utf16String<13>({'h', 't', 't', 'p', ':', '/', '/', 'w', '/', '?', surrogateBegin, ' ', '\0'}),
        { "http"_s, ""_s, ""_s, "w"_s, 0, "/"_s, "%EF%BF%BD"_s, ""_s, "http://w/?%EF%BF%BD"_s });
}

TEST_F(WTF_URLParser, FilePathStartBackslash)
{
    // Backslash after empty host in file URL should be treated like forward slash in FilePathStart,
    // triggering Windows drive letter detection. The pipe in C| must be normalized to a colon.
    checkURL("file://\\C|\\path"_s, { "file"_s, ""_s, ""_s, ""_s, 0, "/C:/path"_s, ""_s, ""_s, "file:///C:/path"_s }, TestTabs::No);
}

TEST_F(WTF_URLParser, Utf16BulkAppend)
{
    // Covers appending a run of 16-bit code units to the ASCII buffer in bulk, which needs a
    // 16-bit input string (forced here by a non-Latin-1 code point, since a Latin-1-only string
    // stays 8-bit), a syntax violation before the run, and a run of at least one SIMD stride.
    // The tab sweep in checkURL additionally supplies a violation at every offset.
    checkURL(utf16String(u"http://host/aaaaaaaaaaaaaaaa^bbbbbbbbbbbbbbbb?cccccccccccccccc#ddddddddddddddddП"),
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/aaaaaaaaaaaaaaaa%5Ebbbbbbbbbbbbbbbb"_s, "cccccccccccccccc"_s, "dddddddddddddddd%D0%9F"_s,
            "http://host/aaaaaaaaaaaaaaaa%5Ebbbbbbbbbbbbbbbb?cccccccccccccccc#dddddddddddddddd%D0%9F"_s });
    checkURL(utf16String(u"about:Пaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbb"),
        { "about"_s, ""_s, ""_s, ""_s, 0, "%D0%9Faaaaaaaaaaaaaaaabbbbbbbbbbbbbbbb"_s, ""_s, ""_s,
            "about:%D0%9Faaaaaaaaaaaaaaaabbbbbbbbbbbbbbbb"_s });
}

TEST_F(WTF_URLParser, DotSegmentsInLongPaths)
{
    // A ".." segment reports a syntax violation and pops the buffer, so everything after it is
    // parsed while building the output buffer. These cases make the surrounding segments long
    // enough for the vectorized scan to consume them in bulk on both sides of the dot segments,
    // and the tab sweep in checkURL moves the violation through every offset.
    checkURL("http://host/aaaaaaaaaaaaaaaa/../bbbbbbbbbbbbbbbb/./cccccccccccccccc"_s,
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/bbbbbbbbbbbbbbbb/cccccccccccccccc"_s, ""_s, ""_s,
            "http://host/bbbbbbbbbbbbbbbb/cccccccccccccccc"_s });
    checkURL("http://host/aaaaaaaaaaaaaaaa/%2e%2e/bbbbbbbbbbbbbbbb"_s,
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/bbbbbbbbbbbbbbbb"_s, ""_s, ""_s,
            "http://host/bbbbbbbbbbbbbbbb"_s });
    checkURL(utf16String(u"http://host/aaaaaaaaaaaaaaaa/../bbbbbbbbbbbbbbbb/./ccccccccccccccccП"),
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/bbbbbbbbbbbbbbbb/cccccccccccccccc%D0%9F"_s, ""_s, ""_s,
            "http://host/bbbbbbbbbbbbbbbb/cccccccccccccccc%D0%9F"_s });
    // Resolving against a base copies the base into the buffer first, so here the pop walks back
    // over copied bytes rather than bulk-appended ones.
    checkRelativeURL("../cccccccccccccccc"_s, "http://host/aaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbb"_s,
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/cccccccccccccccc"_s, ""_s, ""_s, "http://host/cccccccccccccccc"_s });
    checkRelativeURL("./cccccccccccccccc"_s, "http://host/aaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbb"_s,
        { "http"_s, ""_s, ""_s, "host"_s, 0, "/aaaaaaaaaaaaaaaa/cccccccccccccccc"_s, ""_s, ""_s,
            "http://host/aaaaaaaaaaaaaaaa/cccccccccccccccc"_s });
}

TEST_F(WTF_URLParser, FragmentOnOpaquePath)
{
    // The URL that the parser makes from a fragment and a base URL with an opaque path has that opaque path.
    for (auto base : { "mailto:a@b.c"_s, "about:blank"_s, "foo:"_s, "foo:a/b?q#f"_s }) {
        URL url { URL { String { base } }, "#x"_s };
        EXPECT_TRUE(url.isValid());
        EXPECT_TRUE(url.hasOpaquePath());
        EXPECT_TRUE(WTF::URLParser::allValuesEqual(url, URL { url.string() }));
        URL relativeToIt { url, "y"_s };
        EXPECT_FALSE(relativeToIt.isValid());
    }
    URL withPath { URL { "foo:/a/b"_s }, "#x"_s };
    EXPECT_FALSE(withPath.hasOpaquePath());
}

// URLParser::setHost parses a string alone, from https://url.spec.whatwg.org/#host-state: with the host state as state
// override, as the host setter of the URL API does, and with the hostname state, as the hostname setter does. Each
// expected URL is the result of whatwg-url 14.1.0.
struct HostStateTestCase {
    ASCIILiteral url;
    std::u16string_view value;
    // Null when the value does not set the host: setHost returns false and leaves the URL as it is.
    ASCIILiteral afterHostState;
    ASCIILiteral afterHostnameState;
};

static constexpr std::array hostStateTestCases {
    // The port state fails after the host is set: the URL keeps its port.
    HostStateTestCase { "http://x:8080/"_s, u"newhost:99999", "http://newhost:8080/"_s, { } },
    HostStateTestCase { "http://x:8080/p"_s, u"a:b:80", "http://a:8080/p"_s, { } },
    HostStateTestCase { "http://x:8080/"_s, u"a:", "http://a:8080/"_s, { } },
    HostStateTestCase { "http://x:8080/"_s, u"a:y", "http://a:8080/"_s, { } },
    HostStateTestCase { "http://x:8080/"_s, u"a:65536", "http://a:8080/"_s, { } },
    HostStateTestCase { "http://x:8080/"_s, u"[::1]:", "http://[::1]:8080/"_s, { } },
    // The port is the digits up to the first other code point. The default port of the scheme gives no port.
    HostStateTestCase { "http://x:8080/p"_s, u"[::1]:9000:80", "http://[::1]:9000/p"_s, { } },
    HostStateTestCase { "http://x:8080/"_s, u"a:81y", "http://a:81/"_s, { } },
    HostStateTestCase { "http://x:8080/"_s, u"a:65535", "http://a:65535/"_s, { } },
    HostStateTestCase { "http://x:8080/"_s, u"a:80", "http://a/"_s, { } },
    HostStateTestCase { "http://x:8080/"_s, u"a:00080", "http://a/"_s, { } },
    HostStateTestCase { "https://x:8080/"_s, u"a:80", "https://a:80/"_s, { } },
    HostStateTestCase { "http://x:8080/"_s, u"a:80@b", "http://a/"_s, { } },
    HostStateTestCase { "http://u:p@x:8080/p?q#f"_s, u"a:81", "http://u:p@a:81/p?q#f"_s, { } },
    HostStateTestCase { "http://x:8080/"_s, u"a", "http://a:8080/"_s, "http://a:8080/"_s },
    // Tab and newline are removed before anything else.
    HostStateTestCase { "http://x:8080/"_s, u"a:\t8", "http://a:8/"_s, { } },
    HostStateTestCase { "http://x:8080/"_s, u"a:8\t1", "http://a:81/"_s, { } },
    HostStateTestCase { "http://x/"_s, u"\t[::1]", "http://[::1]/"_s, "http://[::1]/"_s },
    HostStateTestCase { "http://x/"_s, u"[:\t:1]", "http://[::1]/"_s, "http://[::1]/"_s },
    HostStateTestCase { "http://x/"_s, u"\n\ra\t", "http://a/"_s, "http://a/"_s },
    HostStateTestCase { "https://h/evil.example/p"_s, u"\t", { }, { } },
    HostStateTestCase { "foo://x/"_s, u"\t:", { }, { } },
    // The host ends at '/', '?' or '#', and at '\\' in a special URL. An empty host fails in a special URL.
    HostStateTestCase { "http://x/p?q#f"_s, u"a/b:81", "http://a/p?q#f"_s, "http://a/p?q#f"_s },
    HostStateTestCase { "http://x/"_s, u"a?b", "http://a/"_s, "http://a/"_s },
    HostStateTestCase { "http://x/"_s, u"a#b", "http://a/"_s, "http://a/"_s },
    HostStateTestCase { "http://x/"_s, u"a\\b", "http://a/"_s, "http://a/"_s },
    HostStateTestCase { "foo://x/"_s, u"a\\b", { }, { } },
    HostStateTestCase { "foo://x:8080/p"_s, u"a:81\\", "foo://a:81/p"_s, { } },
    HostStateTestCase { "https://h/evil.example/p"_s, u"", { }, { } },
    HostStateTestCase { "https://h/evil.example/p"_s, u"/", { }, { } },
    HostStateTestCase { "https://h/evil.example/p"_s, u"?", { }, { } },
    HostStateTestCase { "https://h/evil.example/p"_s, u"\\", { }, { } },
    HostStateTestCase { "ws://x:81/a/b/c"_s, u"#z", { }, { } },
    HostStateTestCase { "http://x:8080/"_s, u":80", { }, { } },
    // The host parser refuses the host.
    HostStateTestCase { "http://x/"_s, u"a@b", { }, { } },
    HostStateTestCase { "http://x/"_s, u" a", { }, { } },
    HostStateTestCase { "http://x/"_s, u"a ", { }, { } },
    HostStateTestCase { "http://x/"_s, u"[::1]a", { }, { } },
    HostStateTestCase { "http://x/"_s, u"[::1", { }, { } },
    HostStateTestCase { "http://x/"_s, u"a[b:c]d:80", { }, { } },
    HostStateTestCase { "http://x/"_s, u"a%2Fb", { }, { } },
    HostStateTestCase { "http://x/"_s, u"1.2.3.4.5", { }, { } },
    HostStateTestCase { "http://x/"_s, u"09", { }, { } },
    HostStateTestCase { "http://u:p@x:8080/p"_s, u"a.09", { }, { } },
    HostStateTestCase { "foo://x/p"_s, u"09", "foo://09/p"_s, "foo://09/p"_s },
    HostStateTestCase { "foo://x"_s, u"a ", { }, { } },
    // The host parser changes the host.
    HostStateTestCase { "http://x/"_s, u"EX%41mple", "http://example/"_s, "http://example/"_s },
    HostStateTestCase { "http://x/"_s, u"0x7f.1", "http://127.0.0.1/"_s, "http://127.0.0.1/"_s },
    HostStateTestCase { "foo://x/p"_s, u"A%41", "foo://A%41/p"_s, "foo://A%41/p"_s },
    HostStateTestCase { "foo://x/p"_s, u"\u00E9", "foo://%C3%A9/p"_s, "foo://%C3%A9/p"_s },
    HostStateTestCase { "foo://x"_s, u"a\u0001", "foo://a%01"_s, "foo://a%01"_s },
    // A file URL has no port, and no Windows drive letter in place of a host. "localhost" and the empty string give the empty host.
    HostStateTestCase { "file:///a/b"_s, u"h", "file://h/a/b"_s, "file://h/a/b"_s },
    HostStateTestCase { "file:///a/b"_s, u"h:81", { }, { } },
    HostStateTestCase { "file:///a/b"_s, u"[::1]", "file://[::1]/a/b"_s, "file://[::1]/a/b"_s },
    HostStateTestCase { "file:///a/b"_s, u"[::1]:81", { }, { } },
    HostStateTestCase { "file:///a/b"_s, u"c:", { }, { } },
    HostStateTestCase { "file:///a/b"_s, u"C|", { }, { } },
    HostStateTestCase { "file:///a/b"_s, u"a@b", { }, { } },
    HostStateTestCase { "file://h/a/b"_s, u"", "file:///a/b"_s, "file:///a/b"_s },
    HostStateTestCase { "file://h/a/b"_s, u"LOCALHOST", "file:///a/b"_s, "file:///a/b"_s },
    HostStateTestCase { "file://h/a/b"_s, u"\\a", "file:///a/b"_s, "file:///a/b"_s },
    HostStateTestCase { "file:///C:/a"_s, u"h", "file://h/C:/a"_s, "file://h/C:/a"_s },
    // A URL that is not special can have the empty host, but not with credentials or a port.
    HostStateTestCase { "foo://x/p"_s, u"", "foo:///p"_s, "foo:///p"_s },
    HostStateTestCase { "foo://x?q"_s, u"", "foo://?q"_s, "foo://?q"_s },
    HostStateTestCase { "foo://x:8080/p"_s, u"", { }, { } },
    HostStateTestCase { "foo://u@x/p"_s, u"", { }, { } },
    // A URL without a host gets one. "/." in front of its path is not part of the path.
    HostStateTestCase { "foo:/p"_s, u"h", "foo://h/p"_s, "foo://h/p"_s },
    HostStateTestCase { "foo:/p"_s, u"h:81", "foo://h:81/p"_s, { } },
    HostStateTestCase { "foo:/p"_s, u"", "foo:///p"_s, "foo:///p"_s },
    HostStateTestCase { "foo:/.//p"_s, u"h", "foo://h//p"_s, "foo://h//p"_s },
    HostStateTestCase { "foo:/.//p"_s, u"", "foo:////p"_s, "foo:////p"_s },
    HostStateTestCase { "foo:/.p"_s, u"h:81", "foo://h:81/.p"_s, { } },
};

static bool setHost(URL& url, const String& value, WTF::URLParser::HostStateOverride stateOverride)
{
    return WTF::URLParser::setHost(url, value, stateOverride);
}

static Vector<String, 2> stringsOfEachWidth(std::u16string_view characters)
{
    auto string16Bit = String(std::span { characters });
    Vector<String, 2> strings { string16Bit };
    if (string16Bit.containsOnlyLatin1())
        strings.append(String::make8Bit(std::span { characters }));
    return strings;
}

static void testHostState(ASCIILiteral urlString, std::u16string_view value, ASCIILiteral expected, WTF::URLParser::HostStateOverride stateOverride)
{
    String urlString16Bit { urlString };
    urlString16Bit.convertTo16Bit();

    for (auto& value : stringsOfEachWidth(value)) {
        for (auto& string : { String { urlString }, urlString16Bit }) {
            SCOPED_TRACE(testing::Message() << urlString << (string.is8Bit() ? " (8-bit) <= " : " (16-bit) <= ") << value << (value.is8Bit() ? " (8-bit)" : " (16-bit)"));
            URL original { string };
            EXPECT_EQ(original.string(), urlString);
            URL url = original;
            EXPECT_EQ(setHost(url, value, stateOverride), !expected.isNull());
            EXPECT_TRUE(url.isValid());
            EXPECT_EQ(url.string(), expected.isNull() ? original.string() : String { expected });
            EXPECT_TRUE(WTF::URLParser::allValuesEqual(url, URL { url.string() }));
        }
    }
}

// The host of a special URL is parsed once, by the host parser: tab and newline removal, percent-decoding, then
// domain to ASCII. A value with no ASCII delimiter gives the host that the same text gives inside a URL. Domain to
// ASCII never sees the value before that parse, so what it maps to a delimiter cannot become URL structure.
struct InternationalHostTestCase {
    std::u16string_view value;
    ASCIILiteral expectedHost; // Null when the value is refused.
};

static constexpr std::array internationalHostTestCases {
    InternationalHostTestCase { u"other.example", "other.example"_s },
    InternationalHostTestCase { u"B\u00FCcher.example", "xn--bcher-kva.example"_s },
    InternationalHostTestCase { u"\u05D0.example", "xn--4db.example"_s },
    InternationalHostTestCase { u"\uFF45\uFF58\uFF41\uFF4D\uFF50\uFF4C\uFF45\u3002com", "example.com"_s },
    InternationalHostTestCase { u"%C3%9F.de", "xn--zca.de"_s },
    // Percent-decoding comes before domain to ASCII.
    InternationalHostTestCase { u"\u00DF%41.de", "xn--a-pfa.de"_s },
    InternationalHostTestCase { u"%C3%BC\u00FC.de", "xn--tdaa.de"_s },
    InternationalHostTestCase { u"\u00DF%2e.de", "xn--zca..de"_s },
    InternationalHostTestCase { u"\u00E9.%78n--a", { } },
    InternationalHostTestCase { u"\u00E9%2Fx", { } },
    // So does tab and newline removal.
    InternationalHostTestCase { u"\t\u00DF.de", "xn--zca.de"_s },
    InternationalHostTestCase { u"\u00DF\n.d\re", "xn--zca.de"_s },
    // Domain to ASCII maps these to '/', '\\', '?', '#', '@', ':' and '%', which are forbidden in a host.
    InternationalHostTestCase { u"a\uFF0Fb", { } },
    InternationalHostTestCase { u"bad.c\u2100.good.com", { } },
    InternationalHostTestCase { u"a\uFF3Cb", { } },
    InternationalHostTestCase { u"a\uFF1Fb", { } },
    InternationalHostTestCase { u"a\uFF03b", { } },
    InternationalHostTestCase { u"a\uFF20b", { } },
    InternationalHostTestCase { u"a\uFF1A81", { } },
    InternationalHostTestCase { u"a\uFF0541", { } },
    InternationalHostTestCase { u"\u00DF\uFF20b", { } },
    // An IPv6 address is never mapped: a fullwidth digit or an ignored code point makes it invalid.
    InternationalHostTestCase { u"[::1]", "[::1]"_s },
    InternationalHostTestCase { u"[::\uFF11]", { } },
    InternationalHostTestCase { u"[::\u00B9]", { } },
    InternationalHostTestCase { u"[::1]\u00AD", { } },
};

static void testInternationalHost(const InternationalHostTestCase& test, ASCIILiteral portInValue, WTF::URLParser::HostStateOverride stateOverride)
{
    auto expectedPort = portInValue.isNull() ? ":444"_s : portInValue;
    for (auto& host : stringsOfEachWidth(test.value)) {
        SCOPED_TRACE(testing::Message() << host << (host.is8Bit() ? " (8-bit)" : " (16-bit)"));
        URL original { "https://user:pass@example.com:444/p?q#f"_s };
        URL parsed { makeString("https://user:pass@"_s, host, expectedPort, "/p?q#f"_s) };
        URL url = original;
        EXPECT_EQ(setHost(url, makeString(host, portInValue), stateOverride), !test.expectedHost.isNull());
        if (test.expectedHost.isNull()) {
            EXPECT_FALSE(parsed.isValid());
            EXPECT_EQ(url.string(), original.string());
            continue;
        }
        EXPECT_EQ(url.string(), makeString("https://user:pass@"_s, test.expectedHost, expectedPort, "/p?q#f"_s));
        EXPECT_EQ(url.string(), parsed.string());
    }
}

TEST_F(WTF_URLParser, SetHost)
{
    using enum WTF::URLParser::HostStateOverride;
    for (auto& test : hostStateTestCases) {
        testHostState(test.url, test.value, test.afterHostState, Host);
        testHostState(test.url, test.value, test.afterHostnameState, Hostname);
    }
    for (auto& test : internationalHostTestCases) {
        testInternationalHost(test, { }, Host);
        testInternationalHost(test, ":8080"_s, Host);
        testInternationalHost(test, { }, Hostname);
    }

    // A host that is not special is opaque: percent-encoded, never converted.
    URL url { "foo://example.com/p"_s };
    EXPECT_TRUE(setHost(url, utf16String(u"\u00DF%41.de"), Hostname));
    EXPECT_EQ(url.string(), "foo://%C3%9F%41.de/p"_s);

    // maybeCanonicalizeHostname gives the host that the hostname state sets, without a URL.
    struct HostnameTestCase {
        std::u16string_view hostname;
        ASCIILiteral scheme;
        ASCIILiteral expected; // Null when the hostname state fails.
    };
    for (auto& test : {
        HostnameTestCase { u"EX%41mple", "https"_s, "example"_s },
        HostnameTestCase { u"\t[:\n:1]", "https"_s, "[::1]"_s },
        HostnameTestCase { u"a/b:81", "https"_s, "a"_s },
        HostnameTestCase { u"a\\b", "https"_s, "a"_s },
        HostnameTestCase { u"a:81", "https"_s, { } },
        HostnameTestCase { u"", "https"_s, { } },
        HostnameTestCase { u"09", "https"_s, { } },
        HostnameTestCase { u"a\uFF20b", "https"_s, { } },
        HostnameTestCase { u"\u00DF%41.de", "ws"_s, "xn--a-pfa.de"_s },
        HostnameTestCase { u"\u00DF%41.de", "foo"_s, "%C3%9F%41.de"_s },
        HostnameTestCase { u"", "foo"_s, ""_s },
        HostnameTestCase { u"a\\b", "foo"_s, { } },
        HostnameTestCase { u"LOCALHOST", "file"_s, ""_s },
        HostnameTestCase { u"h:81", "file"_s, { } },
        HostnameTestCase { u"C|", "file"_s, { } },
    }) {
        for (auto& hostname : stringsOfEachWidth(test.hostname)) {
            SCOPED_TRACE(testing::Message() << hostname << (hostname.is8Bit() ? " (8-bit) in " : " (16-bit) in ") << test.scheme);
            auto host = WTF::URLParser::maybeCanonicalizeHostname(hostname, test.scheme);
            EXPECT_EQ(host.has_value(), !test.expected.isNull());
            if (host)
                EXPECT_EQ(*host, String { test.expected });
        }
    }

    for (auto stateOverride : { Host, Hostname }) {
        URL opaquePath { "mailto:a@b.c"_s };
        EXPECT_FALSE(setHost(opaquePath, "h"_s, stateOverride));
        EXPECT_EQ(opaquePath.string(), "mailto:a@b.c"_s);

        URL invalid { "http://"_s };
        EXPECT_FALSE(setHost(invalid, "h"_s, stateOverride));
        EXPECT_FALSE(invalid.isValid());
        EXPECT_EQ(invalid.string(), "http://"_s);
    }

    // A long host is a host. The parser refuses one that could make its buffer grow past the limit of a Vector: it
    // writes at most nine characters for one code unit.
    auto hostOfLength = [](size_t length) {
        std::span<Latin1Character> characters;
        auto host = String::createUninitialized(length, characters);
        memsetSpan(characters, 'a');
        return host;
    };
    size_t tooLong = 1;
    while (WTF::isValidCapacityForVector<Latin1Character>(WTF::VectorBufferMalloc::nextCapacity(9 * tooLong)))
        tooLong *= 2;
    auto tooLongHost = hostOfLength(tooLong);
    for (auto base : { "https://example.com/p"_s, "foo://example.com/p"_s }) {
        URL longHost { base };
        EXPECT_TRUE(setHost(longHost, hostOfLength(70000), Hostname));
        EXPECT_EQ(longHost.host().length(), 70000u);
        auto before = longHost.string();
        EXPECT_FALSE(setHost(longHost, tooLongHost, Hostname));
        EXPECT_EQ(longHost.string(), before);
        EXPECT_FALSE(WTF::URLParser::maybeCanonicalizeHostname(tooLongHost, longHost.protocol()));
    }
}

// URLs that the parser makes from a reference and a base URL, or that differ from the parse of their own text.
struct ResolvedHostStateTestCase {
    ASCIILiteral base;
    ASCIILiteral reference; // Null for the URL of `base` alone.
    ASCIILiteral value;
    ASCIILiteral afterHostState;
    ASCIILiteral afterHostnameState;
};

static constexpr std::array resolvedHostStateTestCases {
    // An opaque path that the parser copies from the base URL.
    ResolvedHostStateTestCase { "about:blank"_s, "#x"_s, "h"_s, { }, { } },
    ResolvedHostStateTestCase { "mailto:a@b.c"_s, "#x"_s, "h"_s, { }, { } },
    ResolvedHostStateTestCase { "mailto:a@b.c"_s, "#x"_s, "h:81"_s, { }, { } },
    ResolvedHostStateTestCase { "foo:"_s, "#x"_s, "h"_s, { }, { } },
    ResolvedHostStateTestCase { "foo:a/b"_s, "#x"_s, "h"_s, { }, { } },
    ResolvedHostStateTestCase { "foo://u@h"_s, "?z"_s, ""_s, { }, { } },
    ResolvedHostStateTestCase { "foo://h:9"_s, "?z"_s, "h2"_s, "foo://h2:9?z"_s, "foo://h2:9?z"_s },
    ResolvedHostStateTestCase { "foo://h"_s, "?z"_s, ""_s, "foo://?z"_s, "foo://?z"_s },
    ResolvedHostStateTestCase { "foo://h/a/b?q#f"_s, "../x"_s, "h2:81"_s, "foo://h2:81/x"_s, { } },
    ResolvedHostStateTestCase { "foo:/a/b?q#f"_s, "x"_s, "h"_s, "foo://h/a/x"_s, "foo://h/a/x"_s },
    ResolvedHostStateTestCase { "foo:/a/b"_s, "/.//x"_s, "h"_s, "foo://h//x"_s, "foo://h//x"_s },
    ResolvedHostStateTestCase { "http://h/a/b?q#f"_s, "?z"_s, "h2"_s, "http://h2/a/b?z"_s, "http://h2/a/b?z"_s },
    ResolvedHostStateTestCase { "http://h/a/b?q#f"_s, "#x"_s, "h2:81"_s, "http://h2:81/a/b?q#x"_s, { } },
    ResolvedHostStateTestCase { "http://u:p@h:81/a/b"_s, "//x"_s, "h2"_s, "http://h2/"_s, "http://h2/"_s },
    ResolvedHostStateTestCase { "file:///a/b"_s, "c"_s, "h"_s, "file://h/a/c"_s, "file://h/a/c"_s },
    ResolvedHostStateTestCase { "file://h/a/b"_s, "/C:/x"_s, "localhost"_s, "file:///C:/x"_s, "file:///C:/x"_s },
    // Windows drive letters. The parser reads one at the start of the path of a file URL only when the host is empty.
    ResolvedHostStateTestCase { "file://C:"_s, { }, "server"_s, "file://server/C:"_s, "file://server/C:"_s },
    ResolvedHostStateTestCase { "file://C:"_s, { }, ""_s, "file:///C:"_s, "file:///C:"_s },
    ResolvedHostStateTestCase { "file://C|"_s, { }, "h"_s, "file://h/C:"_s, "file://h/C:"_s },
    ResolvedHostStateTestCase { "file://h/C|/a"_s, { }, ""_s, "file:///C:/a"_s, "file:///C:/a"_s },
    ResolvedHostStateTestCase { "file://h/C|/a"_s, { }, "LOCALHOST"_s, "file:///C:/a"_s, "file:///C:/a"_s },
};

TEST_F(WTF_URLParser, SetHostOfResolvedURL)
{
    using enum WTF::URLParser::HostStateOverride;
    auto resolve = [](ASCIILiteral base, ASCIILiteral reference) {
        return reference.isNull() ? URL { String { base } } : URL { URL { String { base } }, String { reference } };
    };
    for (auto& test : resolvedHostStateTestCases) {
        for (auto stateOverride : { Host, Hostname }) {
            SCOPED_TRACE(testing::Message() << test.reference << " on " << test.base << " <= " << test.value << (stateOverride == Host ? " (host)" : " (hostname)"));
            auto expected = stateOverride == Host ? test.afterHostState : test.afterHostnameState;
            URL original = resolve(test.base, test.reference);
            ASSERT_TRUE(original.isValid());
            URL url = original;
            EXPECT_EQ(setHost(url, test.value, stateOverride), !expected.isNull());
            if (expected.isNull()) {
                EXPECT_TRUE(WTF::URLParser::allValuesEqual(url, original));
                continue;
            }
            EXPECT_EQ(url.string(), expected);
            EXPECT_TRUE(WTF::URLParser::allValuesEqual(url, URL { url.string() }));
        }
    }

    // The parser writes "/" in front of the query when it reads the text of this URL.
    URL url { URL { "foo://u@h"_s }, "?z"_s };
    EXPECT_EQ(url.string(), "foo://u@h?z"_s);
    EXPECT_TRUE(setHost(url, "h2:81"_s, Host));
    EXPECT_EQ(url.hostAndPort(), "h2:81"_s);
    EXPECT_TRUE(WTF::URLParser::allValuesEqual(url, URL { url.string() }));
}

// The state override changes only what the URL Standard says it changes. For every other value, the host that
// setHost gives is the host that the parser gives for the same text in the place of the host.
struct HostInURL {
    ASCIILiteral url;
    ASCIILiteral beforeHost;
    ASCIILiteral afterHost;
};

static constexpr std::array hostsInURLs {
    HostInURL { "https://user:pass@example.com:444/p?q#f"_s, "https://user:pass@"_s, ":444/p?q#f"_s },
    HostInURL { "http://x/"_s, "http://"_s, "/"_s },
    HostInURL { "ws://x/a/b?c"_s, "ws://"_s, "/a/b?c"_s },
    HostInURL { "file:///a/b"_s, "file://"_s, "/a/b"_s },
    HostInURL { "file://h/a/b?q#f"_s, "file://"_s, "/a/b?q#f"_s },
    HostInURL { "foo://x/p"_s, "foo://"_s, "/p"_s },
    HostInURL { "foo://u:p@x:81/p?q#f"_s, "foo://u:p@"_s, ":81/p?q#f"_s },
    HostInURL { "foo://x"_s, "foo://"_s, ""_s },
    HostInURL { "foo://x?q"_s, "foo://"_s, "?q"_s },
    HostInURL { "foo:/p"_s, "foo://"_s, "/p"_s },
};

static constexpr std::u16string_view hostAlphabet = u"aB14.-% \t\\|^[]:<\u00E9\uFF0F/?#@";

static constexpr std::array<std::u16string_view, 26> hostWords {
    u"localhost", u"LOCALHOST", u"loc\talhost", u"localhost.", u"xlocalhost", u"%6Cocalhost",
    u"[::1]", u"[::1]\n", u"[:\t:1]", u"0x7f.1", u"1.2.3.4.5", u"a.example",
    u"a\\b.example", u"a.example\\b", u"EX%41mple", u"\u00DF%41.de",
    u"\t[::1]", u"\n[::1]", u"\r\t[::1]", u"\tlocalhost", u"\n\ta.example",
    u"09", u"0x", u"a.09", u"1.09", u"0x7f",
};

// False for a value that the state override treats in another way than the parser. Sets `hasDelimiter` when the
// host ends before the end of the value.
static bool parserReadsTheSameHost(std::u16string_view value, const URL& url, bool& hasDelimiter)
{
    std::u16string host;
    char16_t delimiter = 0;
    for (auto character : value) {
        if (character == '\t' || character == '\n' || character == '\r')
            continue;
        if (character == '/' || character == '?' || character == '#' || (character == '\\' && url.hasSpecialScheme())) {
            delimiter = character;
            break;
        }
        host.push_back(character);
    }
    hasDelimiter = delimiter;
    // An empty host: the state override returns. The parser does the same in front of '?' or '#'. In front of '/' or
    // '\\', and at the end of the value, it goes on with what follows.
    if (host.empty())
        return delimiter == '?' || delimiter == '#';
    // '@': the parser reads credentials before it.
    if (host.find(u'@') != std::u16string::npos)
        return false;
    if (!url.protocolIsFile()) {
        // ':' outside of "[...]": the port state with a state override.
        bool insideBrackets = false;
        for (auto character : host) {
            if (character == '[')
                insideBrackets = true;
            else if (character == ']')
                insideBrackets = false;
            else if (character == ':' && !insideBrackets)
                return false;
        }
    } else if (host.size() == 2 && isASCIIAlpha(host[0]) && (host[1] == ':' || host[1] == '|')) {
        // A Windows drive letter: a path to the parser, a host with a state override.
        return false;
    }
    return true;
}

static void compareHostWithParser(const HostInURL& test, std::u16string_view hostCharacters, std::u16string_view port, WTF::URLParser::HostStateOverride stateOverride, unsigned& comparisons)
{
    std::u16string characters { hostCharacters };
    characters += port;
    URL original { String { test.url } };
    ASSERT_EQ(original.string(), test.url);
    bool hasDelimiter = false;
    if (!parserReadsTheSameHost(hostCharacters, original, hasDelimiter))
        return;
    // With nothing after the host, the parser removes a C0 control or space at the end of the value as the end of its input.
    if (!test.afterHost.length() && characters.back() <= 0x20)
        return;
    // A port after the host: only where the URL has no port there, and the value has no delimiter in front of it.
    if (!port.empty() && (hasDelimiter || StringView { test.afterHost }.startsWith(':')))
        return;
    for (auto& value : stringsOfEachWidth(characters)) {
        URL parsed { makeString(test.beforeHost, value, test.afterHost) };
        URL url = original;
        bool didSetHost = setHost(url, value, stateOverride);
        ++comparisons;
        bool agrees = didSetHost == parsed.isValid();
        if (agrees && didSetHost)
            agrees = url.host() == parsed.host() && (hasDelimiter || url.string() == parsed.string());
        else if (agrees)
            agrees = url.string() == original.string();
        EXPECT_TRUE(agrees) << test.url << " <= " << value << (value.is8Bit() ? " (8-bit)" : " (16-bit)")
            << ": setHost gives " << (didSetHost ? url.string() : String { "no host"_s })
            << ", the parser gives " << (parsed.isValid() ? parsed.string() : String { "no URL"_s });
        // Without credentials or a port in the URL, maybeCanonicalizeHostname gives the host that setHost sets.
        if (stateOverride == WTF::URLParser::HostStateOverride::Hostname && !original.hasCredentials() && !original.port()) {
            auto host = WTF::URLParser::maybeCanonicalizeHostname(value, original.protocol());
            EXPECT_EQ(host.has_value(), didSetHost) << test.url << " <= " << value;
            if (host && didSetHost)
                EXPECT_EQ(*host, url.host()) << test.url << " <= " << value;
        }
    }
}

TEST_F(WTF_URLParser, SetHostAgreesWithParser)
{
    using enum WTF::URLParser::HostStateOverride;
    struct Variant {
        WTF::URLParser::HostStateOverride stateOverride;
        std::u16string_view port;
    };
    unsigned comparisons = 0;
    for (auto variant : { Variant { Hostname, { } }, Variant { Host, { } }, Variant { Host, u":81" }, Variant { Host, u":80" } }) {
        for (auto& test : hostsInURLs) {
            for (auto word : hostWords)
                compareHostWithParser(test, word, variant.port, variant.stateOverride, comparisons);
            for (auto first : hostAlphabet) {
                compareHostWithParser(test, std::u16string { first }, variant.port, variant.stateOverride, comparisons);
                for (auto second : hostAlphabet) {
                    compareHostWithParser(test, std::u16string { first, second }, variant.port, variant.stateOverride, comparisons);
                    for (auto third : hostAlphabet)
                        compareHostWithParser(test, std::u16string { first, second, third }, variant.port, variant.stateOverride, comparisons);
                }
            }
        }
    }
    // The values that the comparison leaves out do not make it empty.
    EXPECT_GT(comparisons, 400000u);
}

} // namespace TestWebKitAPI
