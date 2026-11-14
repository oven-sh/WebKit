/*
 * Copyright (C) 2011-2025 Apple Inc. All rights reserved.
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
#include <wtf/HashSet.h>
#include <wtf/MainThread.h>
#include <wtf/StringPrintStream.h>
#include <wtf/URL.h>
#include <wtf/URLParser.h>
#include <wtf/Vector.h>
#include <wtf/text/StringHash.h>

namespace TestWebKitAPI {

class WTF_URL : public testing::Test {
public:
    virtual void SetUp()
    {
        WTF::initializeMainThread();
    }
};

TEST_F(WTF_URL, URLConstructorDefault)
{
    URL kurl;

    EXPECT_TRUE(kurl.isEmpty());
    EXPECT_TRUE(kurl.isNull());
    EXPECT_FALSE(kurl.isValid());
}

TEST_F(WTF_URL, URLConstructorConstChar)
{
    URL kurl("http://username:password@www.example.com:8080/index.html?var=val#fragment"_s);

    EXPECT_FALSE(kurl.isEmpty());
    EXPECT_FALSE(kurl.isNull());
    EXPECT_TRUE(kurl.isValid());

    EXPECT_TRUE(kurl.protocol() == "http"_s);
    EXPECT_EQ(String("www.example.com"_s), kurl.host().toString());
    EXPECT_TRUE(!!kurl.port());
    EXPECT_EQ(8080, kurl.port().value());
    EXPECT_EQ(String("username"_s), kurl.user());
    EXPECT_EQ(String("password"_s), kurl.password());
    EXPECT_EQ(String("/index.html"_s), kurl.path());
    EXPECT_EQ(String("index.html"_s), kurl.lastPathComponent());
    EXPECT_EQ(String("var=val"_s), kurl.query());
    EXPECT_TRUE(kurl.hasFragmentIdentifier());
    EXPECT_EQ(String("fragment"_s), kurl.fragmentIdentifier());
}

static URL createURL(ASCIILiteral urlAsString)
{
    return URL(urlAsString);
};

TEST_F(WTF_URL, URLProtocolHostAndPort)
{
    auto url = createURL("http://username:password@www.example.com:8080/index.html?var=val#fragment"_s);
    EXPECT_EQ(String("http://www.example.com:8080"_s), url.protocolHostAndPort());

    url = createURL("http://username:@www.example.com:8080/index.html?var=val#fragment"_s);
    EXPECT_EQ(String("http://www.example.com:8080"_s), url.protocolHostAndPort());

    url = createURL("http://:password@www.example.com:8080/index.html?var=val#fragment"_s);
    EXPECT_EQ(String("http://www.example.com:8080"_s), url.protocolHostAndPort());

    url = createURL("http://username@www.example.com:8080/index.html?var=val#fragment"_s);
    EXPECT_EQ(String("http://www.example.com:8080"_s), url.protocolHostAndPort());

    url = createURL("http://www.example.com:8080/index.html?var=val#fragment"_s);
    EXPECT_EQ(String("http://www.example.com:8080"_s), url.protocolHostAndPort());

    url = createURL("http://www.example.com:/index.html?var=val#fragment"_s);
    EXPECT_EQ(String("http://www.example.com"_s), url.protocolHostAndPort());

    url = createURL("http://www.example.com/index.html?var=val#fragment"_s);
    EXPECT_EQ(String("http://www.example.com"_s), url.protocolHostAndPort());

    url = createURL("file:///a/b/c"_s);
    EXPECT_EQ(String("file://"_s), url.protocolHostAndPort());

    url = createURL("file:///a/b"_s);
    EXPECT_EQ(String("file://"_s), url.protocolHostAndPort());

    url = createURL("file:///a"_s);
    EXPECT_EQ(String("file://"_s), url.protocolHostAndPort());

    url = createURL("file:///a"_s);
    EXPECT_EQ(String("file://"_s), url.protocolHostAndPort());

    url = createURL("asdf://username:password@www.example.com:8080/index.html?var=val#fragment"_s);
    EXPECT_EQ(String("asdf://www.example.com:8080"_s), url.protocolHostAndPort());

    url = createURL("asdf:///a/b/c"_s);
    EXPECT_EQ(String("asdf://"_s), url.protocolHostAndPort());
}

TEST_F(WTF_URL, URLDataURIStringSharing)
{
    URL baseURL("http://www.webkit.org/"_s);
    String threeApples = "data:text/plain;charset=utf-8;base64,76O/76O/76O/"_s;

    URL url(baseURL, threeApples);
    EXPECT_EQ(threeApples.impl(), url.string().impl());
}

TEST_F(WTF_URL, URLSetQuery)
{
    URL url = createURL("http://www.webkit.org/?test"_s);
    URL url1 = createURL("http://www.webkit.org/"_s);
    URL url2 = createURL("http://www.webkit.org/?"_s);
    URL url3 = createURL("http://www.webkit.org/?test"_s);
    URL url4 = createURL("http://www.webkit.org/?test1"_s);

    url1.setQuery("test"_s);
    url2.setQuery("test"_s);
    url3.setQuery("test"_s);
    url4.setQuery("test"_s);

    EXPECT_EQ(url.string(), url1.string());
    EXPECT_EQ(url.string(), url2.string());
    EXPECT_EQ(url.string(), url3.string());
    EXPECT_EQ(url.string(), url4.string());

    URL urlWithFragmentIdentifier = createURL("http://www.webkit.org/?test%C3%83%C2%A5#newFragment"_s);
    URL urlWithFragmentIdentifier1 = createURL("http://www.webkit.org/#newFragment"_s);
    URL urlWithFragmentIdentifier2 = createURL("http://www.webkit.org/?#newFragment"_s);
    URL urlWithFragmentIdentifier3 = createURL("http://www.webkit.org/?test1#newFragment"_s);

    urlWithFragmentIdentifier1.setQuery(StringView::fromLatin1("test\xc3\xa5"));
    urlWithFragmentIdentifier2.setQuery(StringView::fromLatin1("test\xc3\xa5"));
    urlWithFragmentIdentifier3.setQuery(StringView::fromLatin1("test\xc3\xa5"));

    EXPECT_EQ(urlWithFragmentIdentifier.string(), urlWithFragmentIdentifier1.string());
    EXPECT_EQ(urlWithFragmentIdentifier.string(), urlWithFragmentIdentifier2.string());
    EXPECT_EQ(urlWithFragmentIdentifier.string(), urlWithFragmentIdentifier3.string());
}

TEST_F(WTF_URL, URLSetFragmentIdentifier)
{
    URL url = createURL("http://www.webkit.org/#newFragment%C3%83%C2%A5"_s);
    URL url1 = createURL("http://www.webkit.org/"_s);
    URL url2 = createURL("http://www.webkit.org/#test2"_s);
    URL url3 = createURL("http://www.webkit.org/#"_s);

    url1.setFragmentIdentifier(StringView::fromLatin1("newFragment\xc3\xa5"));
    url2.setFragmentIdentifier(StringView::fromLatin1("newFragment\xc3\xa5"));
    url3.setFragmentIdentifier(StringView::fromLatin1("newFragment\xc3\xa5"));

    EXPECT_EQ(url.string(), url1.string());
    EXPECT_EQ(url.string(), url2.string());
    EXPECT_EQ(url.string(), url3.string());

    URL urlWithQuery = createURL("http://www.webkit.org/?test1#newFragment"_s);
    URL urlWithQuery1 = createURL("http://www.webkit.org/?test1"_s);
    URL urlWithQuery2 = createURL("http://www.webkit.org/?test1#"_s);
    URL urlWithQuery3 = createURL("http://www.webkit.org/?test1#test2"_s);

    urlWithQuery1.setFragmentIdentifier("newFragment"_s);
    urlWithQuery2.setFragmentIdentifier("newFragment"_s);
    urlWithQuery3.setFragmentIdentifier("newFragment"_s);

    EXPECT_EQ(urlWithQuery.string(), urlWithQuery1.string());
    EXPECT_EQ(urlWithQuery.string(), urlWithQuery2.string());
    EXPECT_EQ(urlWithQuery.string(), urlWithQuery3.string());
}

enum class ParsesBack : bool { No, Yes };

static void checkSetPath(const String& urlString, const String& path, ASCIILiteral expected, ParsesBack parsesBack = ParsesBack::Yes)
{
    URL original { urlString };
    EXPECT_TRUE(original.isValid());

    auto check = [&](const String& urlStringToParse, const String& pathToSet) {
        URL url { urlStringToParse };
        url.setPath(pathToSet);
        EXPECT_TRUE(url.isValid());
        EXPECT_EQ(String(expected), url.string());
        EXPECT_TRUE(WTF::URLParser::internalValuesConsistent(url));
        EXPECT_EQ(original.protocol().toString(), url.protocol().toString());
        EXPECT_EQ(original.encodedUser().toString(), url.encodedUser().toString());
        EXPECT_EQ(original.encodedPassword().toString(), url.encodedPassword().toString());
        EXPECT_EQ(original.hostAndPort(), url.hostAndPort());
        EXPECT_EQ(original.query().toString(), url.query().toString());
        EXPECT_EQ(original.fragmentIdentifier().toString(), url.fragmentIdentifier().toString());
        EXPECT_EQ(parsesBack == ParsesBack::Yes, WTF::URLParser::allValuesEqual(url, URL { url.string() }));

        url.setPath(pathToSet);
        EXPECT_EQ(String(expected), url.string());
    };
    auto in16Bit = [](String string) {
        string.convertTo16Bit();
        return string;
    };

    check(urlString, path);
    check(in16Bit(urlString), path);
    if (path.is8Bit())
        check(urlString, in16Bit(path));
}

// URL::setPath is the pathname setter of the URL Standard: the value is parsed from the path start state, so none of it
// can become anything but the path.
TEST_F(WTF_URL, URLSetPath)
{
    // A value that starts with "//" is not an authority. A URL without a host writes it after "/.".
    checkSetPath("zz:/p"_s, "//"_s, "zz:/.//"_s);
    checkSetPath("zz:/p?q#f"_s, "//"_s, "zz:/.//?q#f"_s);
    checkSetPath("zz:/p"_s, "//x"_s, "zz:/.//x"_s);
    checkSetPath("zz:/p"_s, "///"_s, "zz:/.///"_s);
    checkSetPath("zz:/p"_s, "//x/.."_s, "zz:/.//"_s);
    checkSetPath("zz:/p"_s, "//x/../.."_s, "zz:/"_s);
    checkSetPath("zz:/p"_s, "//h:99999"_s, "zz:/.//h:99999"_s);
    checkSetPath("zz:/p"_s, "//a:b@c"_s, "zz:/.//a:b@c"_s);
    checkSetPath("zz:/p"_s, "//["_s, "zz:/.//["_s);
    checkSetPath("zz:/.//x"_s, "/y"_s, "zz:/y"_s);
    checkSetPath("zz:/.//x"_s, "//"_s, "zz:/.//"_s);
    checkSetPath("zz:/.//x?q"_s, "//y"_s, "zz:/.//y?q"_s);
    checkSetPath("zz://h/p"_s, "//"_s, "zz://h//"_s);
    checkSetPath("zz://h:8/p"_s, "//x"_s, "zz://h:8//x"_s);
    checkSetPath("zz:///p"_s, "//"_s, "zz:////"_s);
    checkSetPath("zz://[::1]/p#f"_s, "//x"_s, "zz://[::1]//x#f"_s);
    checkSetPath("http://h/p"_s, "//"_s, "http://h//"_s);
    checkSetPath("file:///p"_s, "//"_s, "file:////"_s);

    // The empty value leaves no path only in a URL that is not special and has a host.
    checkSetPath("zz:/p"_s, ""_s, "zz:/"_s);
    checkSetPath("zz:/.//x"_s, ""_s, "zz:/"_s);
    checkSetPath("zz://h/p"_s, ""_s, "zz://h"_s);
    checkSetPath("zz://h/p?q#f"_s, ""_s, "zz://h?q#f"_s);
    checkSetPath("zz:///p"_s, ""_s, "zz://"_s);
    checkSetPath("http://h/p?q#f"_s, ""_s, "http://h/?q#f"_s);
    checkSetPath("file:///p"_s, ""_s, "file:///"_s);

    // A value without a leading slash gets one.
    checkSetPath("zz:/p"_s, "x"_s, "zz:/x"_s);
    checkSetPath("zz:/p"_s, "x/y"_s, "zz:/x/y"_s);
    checkSetPath("zz://h"_s, "x"_s, "zz://h/x"_s);
    checkSetPath("zz://h/p"_s, "x/y"_s, "zz://h/x/y"_s);
    checkSetPath("http://h/p"_s, "x"_s, "http://h/x"_s);
    checkSetPath("https://u:pw@h:8/p?q#f"_s, "/x"_s, "https://u:pw@h:8/x?q#f"_s);
    checkSetPath("zz://u:pw@h:8/p?q#f"_s, "x"_s, "zz://u:pw@h:8/x?q#f"_s);

    // A backslash is a slash only in a special URL.
    checkSetPath("http://h/p"_s, "\\\\x"_s, "http://h//x"_s);
    checkSetPath("http://h/p"_s, "\\x\\y"_s, "http://h/x/y"_s);
    checkSetPath("http://h/p"_s, "/a\\..\\b"_s, "http://h/b"_s);
    checkSetPath("zz://h/p"_s, "\\x"_s, "zz://h/\\x"_s);
    checkSetPath("zz://h/p"_s, "/a\\b"_s, "zz://h/a\\b"_s);
    checkSetPath("zz:/p"_s, "\\\\x"_s, "zz:/\\\\x"_s);

    // Tab and newline are removed before anything looks at the value. No other character is.
    checkSetPath("zz:/p"_s, "/\t/"_s, "zz:/.//"_s);
    checkSetPath("zz:/p"_s, "\t//"_s, "zz:/.//"_s);
    checkSetPath("zz:/p"_s, "//\n"_s, "zz:/.//"_s);
    checkSetPath("zz:/p"_s, "/\r/x"_s, "zz:/.//x"_s);
    checkSetPath("zz:/p"_s, "/\n/x"_s, "zz:/.//x"_s);
    checkSetPath("zz:/p"_s, "/\t/["_s, "zz:/.//["_s);
    checkSetPath("zz:/p"_s, "\t/x"_s, "zz:/x"_s);
    checkSetPath("zz:/p"_s, "\tx"_s, "zz:/x"_s);
    checkSetPath("zz:/p"_s, "\n/evil.com/y"_s, "zz:/evil.com/y"_s);
    checkSetPath("zz:/p"_s, "\r/evil.com/y"_s, "zz:/evil.com/y"_s);
    checkSetPath("zz:/p"_s, "\t"_s, "zz:/"_s);
    checkSetPath("zz://h/p"_s, "\n"_s, "zz://h"_s);
    checkSetPath("zz://h/p"_s, "\n/x/y"_s, "zz://h/x/y"_s);
    checkSetPath("http://h/p"_s, "\r\n"_s, "http://h/"_s);
    checkSetPath("http://h/p"_s, "\t/x"_s, "http://h/x"_s);
    checkSetPath("http://h/p"_s, "\t\\x"_s, "http://h/x"_s);
    checkSetPath("http://h/p"_s, "/x\t"_s, "http://h/x"_s);
    checkSetPath("http://h/p"_s, "/a/.\t./b"_s, "http://h/b"_s);
    checkSetPath("http://h/p"_s, "/%\t2e/x"_s, "http://h/x"_s);
    checkSetPath("file:///p"_s, "\t/x"_s, "file:///x"_s);
    checkSetPath("zz:/p"_s, " //x"_s, "zz:/%20//x"_s);
    checkSetPath("zz:/p"_s, "/\f/x"_s, "zz:/%0C/x"_s);
    checkSetPath("http://h/p"_s, " x"_s, "http://h/%20x"_s);
    checkSetPath("data:/nospace"_s, "space "_s, "data:/space%20"_s);

    // '?' and '#' are path characters, so they do not end a dot segment either.
    checkSetPath("zz:/p"_s, "?x#y"_s, "zz:/%3Fx%23y"_s);
    checkSetPath("zz:/p"_s, "//?"_s, "zz:/.//%3F"_s);
    checkSetPath("http://h/p?q#f"_s, "/a?b#c"_s, "http://h/a%3Fb%23c?q#f"_s);
    checkSetPath("http://h/a/b"_s, "/x/..?y"_s, "http://h/x/..%3Fy"_s);
    checkSetPath("http://h/p"_s, "/a/..#/c"_s, "http://h/a/..%23/c"_s);
    checkSetPath("http://h/p"_s, "/.#x"_s, "http://h/.%23x"_s);
    checkSetPath("http://h/p"_s, "/%2e?"_s, "http://h/%2e%3F"_s);
    checkSetPath("zz://h/p"_s, "/a/%2E%2e#"_s, "zz://h/a/%2E%2e%23"_s);

    checkSetPath("http://h/p"_s, "/a/./b/../c"_s, "http://h/a/c"_s);
    checkSetPath("http://h/p"_s, "/a/%2e%2E/b"_s, "http://h/b"_s);
    checkSetPath("http://h/p"_s, "/a/.%2e/%2e./b"_s, "http://h/b"_s);
    checkSetPath("http://h/p"_s, "/a/.."_s, "http://h/"_s);
    checkSetPath("http://h/p"_s, "/a/b/."_s, "http://h/a/b/"_s);
    checkSetPath("http://h/p"_s, "/a/b/../"_s, "http://h/a/"_s);
    checkSetPath("http://h/p"_s, "/a/b/%2E/"_s, "http://h/a/b/"_s);
    checkSetPath("http://h/p"_s, ".."_s, "http://h/"_s);
    checkSetPath("http://h/p"_s, "."_s, "http://h/"_s);
    checkSetPath("http://h/p"_s, "/../../x"_s, "http://h/x"_s);
    checkSetPath("zz://h/p"_s, "/a/../.."_s, "zz://h/"_s);
    checkSetPath("zz:/p"_s, "/a/../../b"_s, "zz:/b"_s);
    checkSetPath("http://h/p"_s, "/a/.../b"_s, "http://h/a/.../b"_s);
    checkSetPath("http://h/p"_s, "/a/.b/..c/d"_s, "http://h/a/.b/..c/d"_s);
    checkSetPath("http://h/p"_s, "/a/%2ex/b"_s, "http://h/a/%2ex/b"_s);
    checkSetPath("zz:/p"_s, "/.x"_s, "zz:/.x"_s);
    checkSetPath("zz:/p"_s, "/..x/y"_s, "zz:/..x/y"_s);

    // A Windows drive letter is written with ':' when it starts the path of a file URL without a host, and ".." keeps it.
    checkSetPath("file:///p"_s, "C|/x"_s, "file:///C:/x"_s);
    checkSetPath("file:///p"_s, "/C|"_s, "file:///C:"_s);
    checkSetPath("file:///p"_s, "/C\t|/x"_s, "file:///C:/x"_s);
    checkSetPath("file:///C:/p"_s, "D|\\x"_s, "file:///D:/x"_s);
    checkSetPath("file:///p"_s, "/C|/../x"_s, "file:///C:/x"_s);
    checkSetPath("file:///p"_s, "/C|/../.."_s, "file:///C:/"_s);
    checkSetPath("file:///p"_s, "/c:/.."_s, "file:///c:/"_s);
    checkSetPath("file:///C:/p"_s, "/../x"_s, "file:///x"_s);
    checkSetPath("file:///p"_s, "/C|?x"_s, "file:///C|%3Fx"_s);
    checkSetPath("file:///p"_s, "/C|x"_s, "file:///C|x"_s);
    checkSetPath("file:///p"_s, "/CC|/x"_s, "file:///CC|/x"_s);
    checkSetPath("file:///p"_s, "//C|/x"_s, "file:////C|/x"_s);
    checkSetPath("file:///p"_s, "/x/C|/.."_s, "file:///x/"_s);
    checkSetPath("http://h/p"_s, "/C|/x"_s, "http://h/C|/x"_s);

    checkSetPath("http://h/p"_s, "/a b"_s, "http://h/a%20b"_s);
    checkSetPath("http://h/p"_s, "/\x01\x1f\x7f"_s, "http://h/%01%1F%7F"_s);
    checkSetPath("http://h/p"_s, "/^`{}|"_s, "http://h/%5E%60%7B%7D|"_s);
    checkSetPath("http://h/p"_s, "/\"<>"_s, "http://h/%22%3C%3E"_s);
    checkSetPath("http://h/p"_s, "/[]@:;=&$"_s, "http://h/[]@:;=&$"_s);
    checkSetPath("http://h/p"_s, "/%zz%2"_s, "http://h/%zz%2"_s);
    checkSetPath("http://h/p"_s, "/a%2Fb%2f"_s, "http://h/a%2Fb%2f"_s);
    checkSetPath("http://h/p"_s, String { "/a\0b"_span8 }, "http://h/a%00b"_s);
    checkSetPath("http://h/p"_s, String::fromUTF8("/\xc3\xa9"), "http://h/%C3%A9"_s);
    checkSetPath("http://h/p"_s, String::fromUTF8("/\xf0\x9f\x98\x80"), "http://h/%F0%9F%98%80"_s);
    checkSetPath("http://h/p"_s, utf16String(u"/\xD83D"), "http://h/%EF%BF%BD"_s);

    // Longer than one step of the run scanner, and longer than the parser's inline buffer.
    checkSetPath("http://h/p?q#f"_s, "/0123456789abcdef/0123456789abcdef"_s, "http://h/0123456789abcdef/0123456789abcdef?q#f"_s);
    checkSetPath("http://h/p?q#f"_s, "/0123456789abcdef/./0123456789abcde f"_s, "http://h/0123456789abcdef/0123456789abcde%20f?q#f"_s);
    StringBuilder longPath;
    StringBuilder longExpected;
    longPath.append("//"_s);
    longExpected.append("zz:/.//"_s);
    for (unsigned i = 0; i < 40; ++i) {
        longPath.append("0123456789/"_s);
        longExpected.append("0123456789/"_s);
    }
    longPath.append("x/../y z"_s);
    longExpected.append("y%20z?q#f"_s);
    URL longURL { "zz:/p?q#f"_s };
    longURL.setPath(longPath.toString());
    EXPECT_EQ(longExpected.toString(), longURL.string());
    EXPECT_TRUE(WTF::URLParser::internalValuesConsistent(longURL));
    EXPECT_TRUE(WTF::URLParser::allValuesEqual(longURL, URL { longURL.string() }));

    // The new path can be a view of the URL's own string.
    URL self { "zz:/.//a?q"_s };
    self.setPath(self.path());
    EXPECT_EQ(String("zz:/.//a?q"_s), self.string());
    self.setPath(self.path().substring(1));
    EXPECT_EQ(String("zz:/a?q"_s), self.string());

    // The setter runs the path loop of URLParser::parse, so where that differs from the URL Standard the setter does too.
    // parse() ends a dot segment at a backslash in every URL, not only in a special one.
    checkSetPath("zz://h/p"_s, "/a/..\\b"_s, "zz://h/b"_s);
    checkSetPath("zz:/p"_s, "/.\\x"_s, "zz:/x"_s);
    // parse() writes a drive letter with ':' only at the start of the path and only without a host. The results of the
    // second and third call are strings that parse() changes again.
    checkSetPath("file://h/p"_s, "C|/x"_s, "file://h/C|/x"_s);
    checkSetPath("file:///p"_s, "/./C|/x"_s, "file:///C|/x"_s, ParsesBack::No);
    checkSetPath("file:///p"_s, "/a/../C|/x"_s, "file:///C|/x"_s, ParsesBack::No);
    // parse() puts a slash behind an authority with userinfo that ends at '?' or '#', also in a URL that is not special.
    // The setter leaves the path empty there, as the URL Standard does.
    checkSetPath("zz://u@h/p?q"_s, ""_s, "zz://u@h?q"_s, ParsesBack::No);
    checkSetPath("zz://u:pw@h:8/p#f"_s, "\t"_s, "zz://u:pw@h:8#f"_s, ParsesBack::No);

    URL invalid { "http://"_s };
    EXPECT_FALSE(invalid.isValid());
    invalid.setPath("/x"_s);
    EXPECT_EQ(String("http://"_s), invalid.string());
}

TEST_F(WTF_URL, URLRemoveQueryAndFragmentIdentifier)
{
    URL url = createURL("http://www.webkit.org/"_s);
    URL url1 = createURL("http://www.webkit.org/?"_s);
    URL url2 = createURL("http://www.webkit.org/?test1"_s);
    URL url3 = createURL("http://www.webkit.org/?test1#test2"_s);
    URL url4 = createURL("http://www.webkit.org/#test2"_s);
    URL url5 = createURL("http://www.webkit.org/#"_s);

    url.removeQueryAndFragmentIdentifier();
    url1.removeQueryAndFragmentIdentifier();
    url2.removeQueryAndFragmentIdentifier();
    url3.removeQueryAndFragmentIdentifier();
    url4.removeQueryAndFragmentIdentifier();
    url5.removeQueryAndFragmentIdentifier();

    EXPECT_EQ(url.string(), url.string());
    EXPECT_EQ(url.string(), url1.string());
    EXPECT_EQ(url.string(), url2.string());
    EXPECT_EQ(url.string(), url3.string());
    EXPECT_EQ(url.string(), url4.string());
    EXPECT_EQ(url.string(), url5.string());
}

TEST_F(WTF_URL, EqualIgnoringFragmentIdentifier)
{
    struct TestCase {
        ASCIILiteral url1;
        ASCIILiteral url2;
        bool expected;
    } cases[] = {
        {"http://example.com/"_s, "http://example.com/"_s, true},
        {"http://example.com/#hash"_s, "http://example.com/"_s, true},
        {"http://example.com/path"_s, "http://example.com/"_s, false},
        {"http://example.com/path"_s, "http://example.com/path"_s, true},
        {"http://example.com/path#hash"_s, "http://example.com/path"_s, true},
        {"http://example.com/path?query"_s, "http://example.com/path"_s, false},
        {"http://example.com/path?query#hash"_s, "http://example.com/path"_s, false},
        {"http://example.com/otherpath"_s, "http://example.com/path"_s, false},
        {"http://example.com:80/"_s, "http://example.com/"_s, true},
        {"http://example.com:80/#hash"_s, "http://example.com/"_s, true},
        {"http://example.com:80/path"_s, "http://example.com/"_s, false},
        {"http://example.com:80/path#hash"_s, "http://example.com/path"_s, true},
        {"http://example.com:80/path?query"_s, "http://example.com/path"_s, false},
        {"http://example.com:80/path?query#hash"_s, "http://example.com/path"_s, false},
        {"http://example.com:80/otherpath"_s, "http://example.com/path"_s, false},
        {"http://not-example.com:80/"_s, "http://example.com/"_s, false},
        {"http://example.com:81/"_s, "http://example.com/"_s, false},
        {"http://example.com:81/#hash"_s, "http://example.com:81/"_s, true},
        {"http://example.com:81/path"_s, "http://example.com:81"_s, false},
        {"http://example.com:81/path#hash"_s, "http://example.com:81/path"_s, true},
        {"http://example.com:81/path?query"_s, "http://example.com:81/path"_s, false},
        {"http://example.com:81/path?query#hash"_s, "http://example.com:81/path"_s, false},
        {"http://example.com:81/otherpath"_s, "http://example.com:81/path"_s, false},
        {"file:///path/to/file.html"_s, "file:///path/to/file.html"_s, true},
        {"file:///path/to/file.html#hash"_s, "file:///path/to/file.html"_s, true},
        {"file:///path/to/file.html?query"_s, "file:///path/to/file.html"_s, false},
        {"file:///path/to/file.html?query#hash"_s, "file:///path/to/file.html"_s, false},
        {"file:///path/to/other_file.html"_s, "file:///path/to/file.html"_s, false},
        {"file:///path/to/other/file.html"_s, "file:///path/to/file.html"_s, false},
        {"data:text/plain;charset=utf-8;base64,76O/76O/76O/"_s, "data:text/plain;charset=utf-8;base64,760/760/760/"_s, false},
        {"http://example.com"_s, "file://example.com"_s, false},
        {"http://example.com/#hash"_s, "file://example.com"_s, false},
        {"http://example.com/?query"_s, "file://example.com/"_s, false},
        {"http://example.com/?query#hash"_s, "file://example.com/"_s, false},
    };

    for (const auto& test : cases) {
        URL url1 = createURL(test.url1);
        URL url2 = createURL(test.url2);
        EXPECT_EQ(test.expected, equalIgnoringFragmentIdentifier(url1, url2))
            << "Test failed for " << test.url1.characters() << " vs. " << test.url2.characters();
    }
}

TEST_F(WTF_URL, ProtocolIsInHTTPFamily)
{
    EXPECT_FALSE(WTF::protocolIsInHTTPFamily({ }));
    EXPECT_FALSE(WTF::protocolIsInHTTPFamily(""_s));
    EXPECT_FALSE(WTF::protocolIsInHTTPFamily("a"_s));
    EXPECT_FALSE(WTF::protocolIsInHTTPFamily("ab"_s));
    EXPECT_FALSE(WTF::protocolIsInHTTPFamily("abc"_s));
    EXPECT_FALSE(WTF::protocolIsInHTTPFamily("abcd"_s));
    EXPECT_FALSE(WTF::protocolIsInHTTPFamily("abcde"_s));
    EXPECT_FALSE(WTF::protocolIsInHTTPFamily("abcdef"_s));
    EXPECT_FALSE(WTF::protocolIsInHTTPFamily("abcdefg"_s));
    EXPECT_TRUE(WTF::protocolIsInHTTPFamily("http:"_s));
    EXPECT_FALSE(WTF::protocolIsInHTTPFamily("http"_s));
    EXPECT_TRUE(WTF::protocolIsInHTTPFamily("https:"_s));
    EXPECT_FALSE(WTF::protocolIsInHTTPFamily("https"_s));
    EXPECT_TRUE(WTF::protocolIsInHTTPFamily("https://!@#$%^&*()"_s));
}

TEST_F(WTF_URL, HostIsIPAddress)
{
    EXPECT_FALSE(URL::hostIsIPAddress({ }));
    EXPECT_FALSE(URL::hostIsIPAddress(""_s));
    EXPECT_FALSE(URL::hostIsIPAddress("localhost"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("127.localhost"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("localhost.127"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("127.0.0"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("127.0 .0.1"_s));
    EXPECT_FALSE(URL::hostIsIPAddress(" 127.0.0.1"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("127..0.0.1"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("127.0.0."_s));
    EXPECT_FALSE(URL::hostIsIPAddress("256.0.0.1"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("0123:4567:89AB:cdef:3210:7654:ba98"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("012x:4567:89AB:cdef:3210:7654:ba98:FeDc"_s));
#if !PLATFORM(COCOA)
    // FIXME: This fails in Mac.
    EXPECT_FALSE(URL::hostIsIPAddress("127.0.0.01"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("00123:4567:89AB:cdef:3210:7654:ba98:FeDc"_s));
#endif
    EXPECT_FALSE(URL::hostIsIPAddress("0123:4567:89AB:cdef:3210:123.45.67.89"_s));
    EXPECT_FALSE(URL::hostIsIPAddress(":::"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("0123::89AB:cdef:3210:7654::FeDc"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("0123:4567:89AB:cdef:3210:7654:ba98:"_s));
    EXPECT_FALSE(URL::hostIsIPAddress("0123:4567:89AB:cdef:3210:7654:ba98:FeDc:"_s));
    EXPECT_FALSE(URL::hostIsIPAddress(":4567:89AB:cdef:3210:7654:ba98:FeDc"_s));
    EXPECT_FALSE(URL::hostIsIPAddress(":0123:4567:89AB:cdef:3210:7654:ba98:FeDc"_s));

    EXPECT_TRUE(URL::hostIsIPAddress("127.0.0.1"_s));
    EXPECT_TRUE(URL::hostIsIPAddress("255.1.10.100"_s));
    EXPECT_TRUE(URL::hostIsIPAddress("0.0.0.0"_s));
    EXPECT_TRUE(URL::hostIsIPAddress("::1"_s));
    EXPECT_TRUE(URL::hostIsIPAddress("::"_s));
    EXPECT_TRUE(URL::hostIsIPAddress("0123:4567:89AB:cdef:3210:7654:ba98:FeDc"_s));
    EXPECT_TRUE(URL::hostIsIPAddress("0123:4567:89AB:cdef:3210:7654:ba98::"_s));
    EXPECT_TRUE(URL::hostIsIPAddress("::4567:89AB:cdef:3210:7654:ba98:FeDc"_s));
    EXPECT_TRUE(URL::hostIsIPAddress("0123:4567:89AB:cdef:3210:7654:123.45.67.89"_s));
    EXPECT_TRUE(URL::hostIsIPAddress("::123.45.67.89"_s));
}

TEST_F(WTF_URL, HostIsMatchingDomain)
{
    URL url = createURL("http://www.webkit.org"_s);

    EXPECT_TRUE(url.isMatchingDomain(String { }));
    EXPECT_TRUE(url.isMatchingDomain(emptyString()));
    EXPECT_TRUE(url.isMatchingDomain("org"_s));
    EXPECT_TRUE(url.isMatchingDomain("webkit.org"_s));
    EXPECT_TRUE(url.isMatchingDomain("www.webkit.org"_s));

    EXPECT_FALSE(url.isMatchingDomain("rg"_s));
    EXPECT_FALSE(url.isMatchingDomain(".org"_s));
    EXPECT_FALSE(url.isMatchingDomain("ww.webkit.org"_s));
    EXPECT_FALSE(url.isMatchingDomain("http://www.webkit.org"_s));

    url = createURL("file:///www.webkit.org"_s);

    EXPECT_TRUE(url.isMatchingDomain(String { }));
    EXPECT_TRUE(url.isMatchingDomain(emptyString()));
    EXPECT_FALSE(url.isMatchingDomain("org"_s));
    EXPECT_FALSE(url.isMatchingDomain("webkit.org"_s));
    EXPECT_FALSE(url.isMatchingDomain("www.webkit.org"_s));

    URL emptyURL;
    EXPECT_FALSE(emptyURL.isMatchingDomain(String { }));
    EXPECT_FALSE(emptyURL.isMatchingDomain(emptyString()));
}

TEST_F(WTF_URL, PrintStream)
{
    String urlString("http://www.webkit.org/"_s);
    URL url(urlString);
    StringPrintStream out;
    out.print(url);
    EXPECT_EQ(out.toString(), urlString);
}

TEST_F(WTF_URL, URLDifferingQueryParameters)
{
    URL url1 = createURL("www.webkit.org/?"_s);
    URL url2 = createURL("http://www.webkit.org/?key1=val1"_s);
    Vector<KeyValuePair<String, String>> testVector12 { { "key1"_s, "val1"_s } };
    EXPECT_EQ(differingQueryParameters(url1, url2), testVector12);
    
    URL url33 = createURL("http://www.webkit.org/?key1=val1"_s);
    URL url34 = createURL("webkit.org/?"_s);
    EXPECT_EQ(differingQueryParameters(url33, url34), testVector12);
    
    URL url35 = createURL(".org/path/?key1=val1"_s);
    URL url36 = createURL("/path/?key1=val1"_s);
    Vector<KeyValuePair<String, String>> testVector { };
    EXPECT_EQ(differingQueryParameters(url35, url36), testVector);
    
    URL url = createURL("http://www.webkit.org/?key1=val1"_s);
    EXPECT_EQ(differingQueryParameters(url, url), testVector);
    
    URL url9 = createURL("http://www.webkit.org/?key1=val1"_s);
    URL url10 = createURL("http://www.webkit.org/?key1=val1"_s);
    EXPECT_EQ(differingQueryParameters(url9, url10) , testVector);
    
    URL url7 = createURL("http://www.webkit.org/?"_s);
    URL url8 = createURL("http://www.webkit.org/?"_s);
    EXPECT_EQ(differingQueryParameters(url7, url8), testVector);
    
    URL url3 = createURL("http://www.webkit.org/?"_s);
    URL url4 = createURL("http://www.webkit.org/?key1=val1"_s);
    Vector<KeyValuePair<String, String>> testVector34 { { "key1"_s, "val1"_s } };
    EXPECT_EQ(differingQueryParameters(url3, url4), testVector34);
    
    URL url5 = createURL("http://www.webkit.org/?key1=val1"_s);
    URL url6 = createURL("http://www.webkit.org/?"_s);
    Vector<KeyValuePair<String, String>> testVector56 { { "key1"_s, "val1"_s } };
    EXPECT_EQ(differingQueryParameters(url5, url6), testVector56);
    
    URL url13 = createURL("http://www.webkit.org/?key1=val1&key2=val2"_s);
    URL url14 = createURL("http://www.webkit.org/?key1=val1&key1=val1"_s);
    Vector<KeyValuePair<String, String>> testVector1314 { { "key1"_s, "val1"_s }, { "key2"_s, "val2"_s } };
    EXPECT_EQ(differingQueryParameters(url13, url14), testVector1314);
    
    URL url15 = createURL("http://www.webkit.org/?key1=val1"_s);
    URL url16 = createURL("http://www.webkit.org/?key2=val2"_s);
    Vector<KeyValuePair<String, String>> testVector1516 { { "key1"_s, "val1"_s }, { "key2"_s, "val2"_s } };
    EXPECT_EQ(differingQueryParameters(url15, url16), testVector1516);
    
    URL url11 = createURL("http://www.webkit.org/?key2=val2&key1=val1"_s);
    URL url12 = createURL("http://www.webkit.org/?key3=val3&key1=val1"_s);
    Vector<KeyValuePair<String, String>> testVector1112 { { "key2"_s, "val2"_s }, { "key3"_s, "val3"_s } };
    EXPECT_EQ(differingQueryParameters(url11, url12), testVector1112);
    
    URL url17 = createURL("http://www.webkit.org/?key1=val1&key2=val2"_s);
    URL url18 = createURL("http://www.webkit.org/?key1&key3=val3"_s);
    Vector<KeyValuePair<String, String>> testVector1718 { {"key1"_s, ""_s }, { "key1"_s, "val1"_s }, { "key2"_s, "val2"_s }, { "key3"_s, "val3"_s } };
    EXPECT_EQ(differingQueryParameters(url17, url18), testVector1718);
    
    URL url19 = createURL("http://www.webkit.org/?key2=val2&key1=val1&key2=val2"_s);
    URL url20 = createURL("http://www.webkit.org/?key3=val3&key1"_s);
    Vector<KeyValuePair<String, String>> testVector1920 { { "key1"_s, ""_s }, { "key1"_s, "val1"_s }, { "key2"_s, "val2"_s }, { "key2"_s, "val2"_s }, { "key3"_s, "val3"_s } };
    EXPECT_EQ(differingQueryParameters(url19, url20), testVector1920);
    
    URL url21 = createURL("http://www.webkit.org/??"_s);
    URL url22 = createURL("http://www.webkit.org/?/?test=test"_s);
    Vector<KeyValuePair<String, String>> testVector2122 { { "/?test"_s, "test"_s }, { "?"_s, ""_s } };
    EXPECT_EQ(differingQueryParameters(url21, url22), testVector2122);
    
    URL url23 = createURL("http://www.webkit.org/?=test"_s);
    URL url24 = createURL("http://www.webkit.org/?=="_s);
    Vector<KeyValuePair<String, String>> testVector2324 { { ""_s, "="_s }, { ""_s, "test"_s } };
    EXPECT_EQ(differingQueryParameters(url23, url24), testVector2324);
    
    URL url27 = createURL("http://www.webkit.org??"_s);
    URL url28 = createURL("http://www.webkit.org?/?test=test"_s);
    Vector<KeyValuePair<String, String>> testVector2728 { { "/?test"_s, "test"_s }, { "?"_s, ""_s } };
    EXPECT_EQ(differingQueryParameters(url27, url28), testVector2728);
    
    URL url29 = createURL("http://www.webkit.org?=test"_s);
    URL url30 = createURL("http://www.webkit.org?=="_s);
    Vector<KeyValuePair<String, String>> testVector2930 { { ""_s, "="_s }, { ""_s, "test"_s } };
    EXPECT_EQ(differingQueryParameters(url29, url30), testVector2930);
    
    URL url31 = createURL("http://www.webkit.org?=?"_s);
    URL url32 = createURL("http://www.webkit.org=?"_s);
    Vector<KeyValuePair<String, String>> testVector3132 { { ""_s, "?"_s } };
    EXPECT_EQ(differingQueryParameters(url31, url32), testVector3132);
    
    URL url25 = createURL("http://www.webkit.org/?=?"_s);
    URL url26 = createURL("http://www.webkit.org/=?"_s);
    Vector<KeyValuePair<String, String>> testVector2526 { { ""_s, "?"_s } };
    EXPECT_EQ(differingQueryParameters(url25, url26), testVector2526);
}

TEST_F(WTF_URL, URLIsEqualIgnoringQueryAndFragments)
{
    URL url1 = createURL("www.webkit.org/?"_s);
    URL url2 = createURL("http://www.webkit.org/?key1=val1"_s);
    EXPECT_FALSE(isEqualIgnoringQueryAndFragments(url1, url2));
    
    URL url13 = createURL("webkit.org/?=?"_s);
    URL url14 = createURL("webkit.org/?=?"_s);
    EXPECT_TRUE(isEqualIgnoringQueryAndFragments(url13, url14));
    
    URL url15 = createURL("http://www.webkit.org/"_s);
    URL url16 = createURL("webkit.org/"_s);
    EXPECT_FALSE(isEqualIgnoringQueryAndFragments(url15, url16));
    
    URL url17 = createURL("webkit.org/?=?"_s);
    URL url18 = createURL("kit.org/?=?"_s);
    EXPECT_FALSE(isEqualIgnoringQueryAndFragments(url17, url18));

    URL url = createURL("http://www.webkit.org/?key1=val1"_s);
    EXPECT_TRUE(isEqualIgnoringQueryAndFragments(url, url));
    
    URL url3 = createURL("http://www.webkit.org/?"_s);
    URL url4 = createURL("http://www.webkit.org/?key1=val1"_s);
    EXPECT_TRUE(isEqualIgnoringQueryAndFragments(url3, url4));
    
    URL url7 = createURL("http://www.webkit.org?"_s);
    URL url8 = createURL("http://www.webkit.org/?key1=val1"_s);
    EXPECT_TRUE(isEqualIgnoringQueryAndFragments(url7, url8));
    
    URL url5 = createURL("http://www.example.org/path?"_s);
    URL url6 = createURL("http://www.webkit.org/?key1=val1"_s);
    EXPECT_FALSE(isEqualIgnoringQueryAndFragments(url5, url6));
    
    URL url9 = createURL("http://example.com?a=b"_s);
    URL url10 = createURL("http://example.com/?a=b"_s);
    EXPECT_TRUE(isEqualIgnoringQueryAndFragments(url9, url10));
    
    URL url11 = createURL("http://www.webkit.org/?"_s);
    URL url12 = createURL("http://www.webkit.org?"_s);
    EXPECT_TRUE(isEqualIgnoringQueryAndFragments(url11, url12));
    
    URL url21 = createURL("http://www.webkit.org/??"_s);
    URL url22 = createURL("http://www.webkit.org/?/?"_s);
    EXPECT_TRUE(isEqualIgnoringQueryAndFragments(url21, url22));
    
    URL url23 = createURL("http://www.webkit.org/?=&"_s);
    URL url24 = createURL("http://www.webkit.org/?==&"_s);
    EXPECT_TRUE(isEqualIgnoringQueryAndFragments(url23, url24));
    
    URL url27 = createURL("http://www.webkit.org?&?"_s);
    URL url28 = createURL("http://www.webkit.org??&"_s);
    EXPECT_TRUE(isEqualIgnoringQueryAndFragments(url27, url28));
    
    URL url31 = createURL("http://www.webkit.org?=?"_s);
    URL url32 = createURL("http://www.webkit.org=?"_s);
    EXPECT_FALSE(isEqualIgnoringQueryAndFragments(url31, url32));
    
    URL url25 = createURL("http://www.webkit.org/?=?"_s);
    URL url26 = createURL("http://www.webkit.org/=?"_s);
    EXPECT_FALSE(isEqualIgnoringQueryAndFragments(url25, url26));
}

TEST_F(WTF_URL, URLRemoveQueryParameters)
{
    const auto url = createURL("http://www.webkit.org/?key=val"_s);
    auto url1 = createURL("http://www.webkit.org/?key=val&key1=val1"_s);
    auto url2 = createURL("http://www.webkit.org/?"_s);
    auto url3 = createURL("http://www.webkit.org/?key=val#fragment"_s);
    auto url4 = createURL("http://www.webkit.org/?key=val&key=val#fragment"_s);
    auto url5 = createURL("http://www.webkit.org/?key&key=#fragment"_s);
    auto url6 = createURL("http://www.webkit.org/#fragment"_s);
    auto url7 = createURL("http://www.webkit.org/?key=val#fragment"_s);
    const auto url8 = createURL("http://www.webkit.org/"_s);
    const auto url9 = createURL("http://www.webkit.org/#fragment"_s);
    const auto url10 = createURL("http://www.webkit.org/?key=val#fragment"_s);
    auto url11 = createURL("http://www.webkit.org/?key=val&key1=val1#fragment"_s);
    auto url12 = createURL("http://www.webkit.org/?key=val&key1=val1#fragment"_s);
    auto url13 = createURL("http://www.webkit.org"_s);
    auto url14 = createURL("http://www.webkit.org/?u+v=x+%20y&key1=foo"_s);
    auto url15 = createURL("http://www.webkit.org/?u+v=x+%20y"_s);

    HashSet<String> keyRemovalSet1 { "key"_s };
    HashSet<String> keyRemovalSet2 { "key1"_s };
    HashSet<String> keyRemovalSet3 { "key2"_s };
    HashSet<String> keyRemovalSet4 { "key"_s, "key1"_s };

    auto checkRemovedParameters = [](int lineNumber, const Vector<String>& removedParameters, std::initializer_list<String>&& expected) {
        Vector<String> expectedParameters { WTF::move(expected) };
        bool areEqual = removedParameters == expectedParameters;
        EXPECT_TRUE(areEqual);
        if (areEqual)
            return;

        WTFLogAlways("Test failed at %s:%d", __FILE__, lineNumber);
        WTFLogAlways("- Got %zu parameter(s)", removedParameters.size());
        for (auto& parameter : removedParameters)
            SAFE_WTFLOGALWAYS("    %s", parameter.utf8());
        WTFLogAlways("- Expected %zu parameter(s)", expectedParameters.size());
        for (auto& parameter : expectedParameters)
            SAFE_WTFLOGALWAYS("    %s", parameter.utf8());
    };

    auto removedParameters1 = removeQueryParameters(url1, keyRemovalSet2);
    EXPECT_EQ(url1.string(), url.string());
    checkRemovedParameters(__LINE__, removedParameters1, { "key1"_s });

    const auto originalURL2 = url2;
    auto removedParameters2 = removeQueryParameters(url2, keyRemovalSet1);
    EXPECT_EQ(url2.string(), originalURL2.string());
    checkRemovedParameters(__LINE__, removedParameters2, { });

    auto removedParameters3 = removeQueryParameters(url3, keyRemovalSet1);
    EXPECT_EQ(url3.string(), url6.string());
    checkRemovedParameters(__LINE__, removedParameters3, { "key"_s });

    auto removedParameters4 = removeQueryParameters(url4, keyRemovalSet1);
    EXPECT_EQ(url4.string(), url6.string());
    checkRemovedParameters(__LINE__, removedParameters4, { "key"_s, "key"_s });

    auto removedParameters5 = removeQueryParameters(url5, keyRemovalSet1);
    EXPECT_EQ(url5.string(), url6.string());
    checkRemovedParameters(__LINE__, removedParameters5, { "key"_s, "key"_s });

    auto removedParameters6 = removeQueryParameters(url6, keyRemovalSet1);
    EXPECT_EQ(url6.string(), url9.string());
    checkRemovedParameters(__LINE__, removedParameters6, { });

    auto removedParameters7 = removeQueryParameters(url7, keyRemovalSet2);
    EXPECT_EQ(url7.string(), url10.string());
    checkRemovedParameters(__LINE__, removedParameters7, { });

    auto removedParameters11 = removeQueryParameters(url11, keyRemovalSet3);
    EXPECT_EQ(url11.string(), url12.string());
    checkRemovedParameters(__LINE__, removedParameters11, { });

    auto removedParameters12 = removeQueryParameters(url12, keyRemovalSet4);
    EXPECT_EQ(url12.string(), url9.string());
    checkRemovedParameters(__LINE__, removedParameters12, { "key"_s, "key1"_s });

    auto removedParameters13 = removeQueryParameters(url13, keyRemovalSet1);
    EXPECT_EQ(url13.string(), url8.string());
    checkRemovedParameters(__LINE__, removedParameters13, { });

    auto removedParameters14 = removeQueryParameters(url14, keyRemovalSet4);
    EXPECT_EQ(url14.string(), url15.string());
    checkRemovedParameters(__LINE__, removedParameters14, { "key1"_s });
}

TEST_F(WTF_URL, URLStrippedForUseAsReport)
{
    const auto expectedHTTPSURL = createURL("https://example.com/api"_s);
    const auto expectedHTTPURL = createURL("http://example.com/api"_s);

    const auto url = createURL("https://example.com/api"_s);
    const auto url1 = createURL("https://example.com/api?key=value"_s);
    auto url2 = createURL("https://user:password@example.com/api"_s);
    auto url3 = createURL("https://user:password@example.com/api?key=value&key1=value1"_s);
    auto url4 = createURL("https://example.com/api#fragment"_s);
    auto url5 = createURL("https://user:password@example.com/api#fragment"_s);
    auto url6 = createURL("https://example.com/api?key=value&key1=value1#fragment"_s);
    auto url7 = createURL("https://user:password@example.com/api?key=value#fragment"_s);

    const auto url8 = createURL("http://example.com/api#fragment"_s);
    auto url9 = createURL("http://example.com/api?key=value#fragment"_s);
    const auto url10 = createURL("http://user:password@example.com/api#fragment"_s);
    auto url11 = createURL("http://user:password@example.com/api?key=value#fragment"_s);

    EXPECT_EQ(expectedHTTPSURL.string(), url.strippedForUseAsReport());
    EXPECT_EQ(expectedHTTPSURL.string(), url1.strippedForUseAsReport());
    EXPECT_EQ(expectedHTTPSURL.string(), url2.strippedForUseAsReport());
    EXPECT_EQ(expectedHTTPSURL.string(), url3.strippedForUseAsReport());
    EXPECT_EQ(expectedHTTPSURL.string(), url4.strippedForUseAsReport());
    EXPECT_EQ(expectedHTTPSURL.string(), url5.strippedForUseAsReport());
    EXPECT_EQ(expectedHTTPSURL.string(), url6.strippedForUseAsReport());
    EXPECT_EQ(expectedHTTPSURL.string(), url7.strippedForUseAsReport());

    EXPECT_EQ(expectedHTTPURL.string(), url8.strippedForUseAsReport());
    EXPECT_EQ(expectedHTTPURL.string(), url9.strippedForUseAsReport());
    EXPECT_EQ(expectedHTTPURL.string(), url10.strippedForUseAsReport());
    EXPECT_EQ(expectedHTTPURL.string(), url11.strippedForUseAsReport());
}

TEST_F(WTF_URL, IsolatedCopy)
{
    URL url1 { "http://www.apple.com"_str };
    auto* originalStringImpl = url1.string().impl();
    URL url1Copy = url1.isolatedCopy();
    EXPECT_EQ(url1Copy, url1);
    EXPECT_NE(url1Copy.string().impl(), originalStringImpl); // Should have done a deep copy of the String.

    // Tests optimization for URL::isolatedCopy() on a r-value reference.
    URL url2 { "https://www.webkit.org"_str };
    originalStringImpl = url2.string().impl();
    URL url2Copy = WTF::move(url2).isolatedCopy();
    EXPECT_EQ(url2Copy, URL { "https://www.webkit.org"_str });
    EXPECT_EQ(url2Copy.string().impl(), originalStringImpl); // Should have adopted the StringImpl of url2.
}

TEST_F(WTF_URL, MoveInvalidatesURL)
{
    URL url1 { "http://www.apple.com"_str };
    EXPECT_TRUE(url1.isValid());
    URL url2 { WTF::move(url1) };
    EXPECT_TRUE(url2.isValid());
    SUPPRESS_USE_AFTER_MOVE EXPECT_FALSE(url1.isValid());

    URL url3 { "http://www.webkit.org"_str };
    url3 = WTF::move(url2);
    EXPECT_TRUE(url3.isValid());
    SUPPRESS_USE_AFTER_MOVE EXPECT_FALSE(url2.isValid());

    url3 = { };
    EXPECT_FALSE(url3.isValid());

    URL url4 { "http://www.webkit.org"_str };
    static_assert(std::is_same_v<decltype(url4.string()), const String&>);
    static_assert(std::is_same_v<decltype(WTF::move(url4).string()), String>);
    EXPECT_TRUE(url4.isValid());
    String string4 = WTF::move(url4).string();
    SUPPRESS_USE_AFTER_MOVE EXPECT_FALSE(url4.isValid());
}

TEST_F(WTF_URL, ProtocolIsSecure)
{
    const auto httpsURL = createURL("https://example.com/api"_s);
    EXPECT_TRUE(httpsURL.protocolIsSecure());

    const auto httpURL = createURL("http://example.com/api"_s);
    EXPECT_FALSE(httpURL.protocolIsSecure());

    // Note: FTPS is not considered secure for WebKit purposes.
    const auto ftpsURL = createURL("ftps://example.com/api"_s);
    EXPECT_FALSE(ftpsURL.protocolIsSecure());

    const auto ftpURL = createURL("ftp://example.com/api"_s);
    EXPECT_FALSE(ftpURL.protocolIsSecure());

    const auto wssURL = createURL("wss://example.com/api"_s);
    EXPECT_TRUE(wssURL.protocolIsSecure());

    const auto wsURL = createURL("ws://example.com/api"_s);
    EXPECT_FALSE(wsURL.protocolIsSecure());

    const auto asdfURL = createURL("asdf:///a/b/c"_s);
    EXPECT_FALSE(asdfURL.protocolIsSecure());

    const auto blobURL = createURL("blob://apple.com"_s);
    EXPECT_FALSE(blobURL.protocolIsSecure());

    const auto aboutURL = createURL("about://example.com/"_s);
    EXPECT_FALSE(aboutURL.protocolIsSecure());
}

} // namespace TestWebKitAPI
