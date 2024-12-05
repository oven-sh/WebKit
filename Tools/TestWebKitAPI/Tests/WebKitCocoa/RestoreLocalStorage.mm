/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
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

#import "config.h"

#import "DeprecatedGlobalValues.h"
#import "HTTPServer.h"
#import "PlatformUtilities.h"
#import "Test.h"
#import "TestNavigationDelegate.h"
#import "TestWKWebView.h"
#import "Utilities.h"
#import <WebKit/WKScriptMessageHandler.h>
#import <WebKit/WKWebViewPrivate.h>
#import <WebKit/WKWebsiteDataRecord.h>
#import <WebKit/WKWebsiteDataStorePrivate.h>
#import <WebKit/_WKWebsiteDataStoreConfiguration.h>
#import <wtf/RetainPtr.h>

static void testFetchLocalStorage(RetainPtr<WKWebsiteDataStore> websiteDataStore)
{
    RetainPtr viewConfiguration = adoptNS([WKWebViewConfiguration new]);
    [viewConfiguration setWebsiteDataStore:websiteDataStore.get()];
    RetainPtr webView = adoptNS([[WKWebView alloc] initWithFrame:CGRectMake(0, 0, 100, 100) configuration:viewConfiguration.get()]);

    auto navigationDelegate = adoptNS([[TestNavigationDelegate alloc] init]);
    [webView setNavigationDelegate:navigationDelegate.get()];
    [webView loadHTMLString:@"<body>webkit.org</body>" baseURL:[NSURL URLWithString:@"https://webkit.org"]];
    [navigationDelegate waitForDidFinishNavigation];

    __block bool doneEvaluatingJavaScript = false;
    [webView evaluateJavaScript:@"localStorage.setItem('key', 'value');" completionHandler:^(id value, NSError *error) {
        EXPECT_NULL(error);
        doneEvaluatingJavaScript = true;
    }];
    TestWebKitAPI::Util::run(&doneEvaluatingJavaScript);

    RetainPtr set = [NSSet setWithObject:WKWebsiteDataTypeLocalStorage];

    __block bool doneFetching = false;
    [[[webView configuration] websiteDataStore] _fetchDataOfTypes:set.get() completionHandler:^(NSData *data) {
        EXPECT_NOT_NULL(data);
        EXPECT_GT([data length], 0u);
        doneFetching = true;
    }];
    TestWebKitAPI::Util::run(&doneFetching);
}

TEST(WebKit, FetchLocalStorageFromPersistentDataStore)
{
    testFetchLocalStorage(adoptNS([WKWebsiteDataStore defaultDataStore]));
}

TEST(WebKit, FetchLocalStorageFromEphemeralDataStore)
{
    testFetchLocalStorage(adoptNS([WKWebsiteDataStore nonPersistentDataStore]));
}

@interface RestoreLocalStorageMessageHandler : NSObject <WKScriptMessageHandler>
@end

@implementation RestoreLocalStorageMessageHandler

- (void)userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message
{
    receivedScriptMessage = true;
    lastScriptMessage = message;
}

@end

static NSString *mainFrameString = @"<script> \
    function postResult(event) \
    { \
        window.webkit.messageHandlers.testHandler.postMessage(event.data); \
    } \
    addEventListener('message', postResult, false); \
    </script> \
    <iframe src='https://127.0.0.1:9091/'>";

static constexpr auto frameBytes = R"TESTRESOURCE(
<script>
function postMessage(message)
{
    parent.postMessage(message, '*');
}
async function setItem()
{
    try {
        localStorage.setItem('key', 'value');
        postMessage('Item is set');
    } catch(err) {
        postMessage('error: ' + err.name + ' - ' + err.message);
    }
}
setItem();
</script>
)TESTRESOURCE"_s;

static void testFetchLocalStorageThirdPartyIFrame(RetainPtr<WKWebsiteDataStore> websiteDataStore)
{
    RetainPtr viewConfiguration = adoptNS([[WKWebViewConfiguration alloc] init]);
    RetainPtr handler = adoptNS([[RestoreLocalStorageMessageHandler alloc] init]);
    [[viewConfiguration userContentController] addScriptMessageHandler:handler.get() name:@"testHandler"];
    [viewConfiguration setWebsiteDataStore:websiteDataStore.get()];

    RetainPtr webView = adoptNS([[WKWebView alloc] initWithFrame:CGRectMake(0, 0, 100, 100) configuration:viewConfiguration.get()]);
    RetainPtr navigationDelegate = adoptNS([TestNavigationDelegate new]);
    [navigationDelegate setDidReceiveAuthenticationChallenge:^(WKWebView *, NSURLAuthenticationChallenge *challenge, void (^callback)(NSURLSessionAuthChallengeDisposition, NSURLCredential *)) {
        EXPECT_WK_STREQ(challenge.protectionSpace.authenticationMethod, NSURLAuthenticationMethodServerTrust);
        callback(NSURLSessionAuthChallengeUseCredential, [NSURLCredential credentialForTrust:challenge.protectionSpace.serverTrust]);
    }];
    [navigationDelegate setDecidePolicyForNavigationAction:[&](WKNavigationAction *action, void (^decisionHandler)(WKNavigationActionPolicy)) {
        decisionHandler(WKNavigationActionPolicyAllow);
    }];
    [webView setNavigationDelegate:navigationDelegate.get()];

    TestWebKitAPI::HTTPServer server({
        { "/"_s, { frameBytes } },
    }, TestWebKitAPI::HTTPServer::Protocol::Https, nullptr, nullptr, 9091);

    [webView loadHTMLString:mainFrameString baseURL:[NSURL URLWithString:@"https://webkit.org"]];
    TestWebKitAPI::Util::run(&receivedScriptMessage);
    receivedScriptMessage = false;
    EXPECT_WK_STREQ(@"Item is set", [lastScriptMessage body]);

    RetainPtr set = [NSSet setWithObject:WKWebsiteDataTypeLocalStorage];

    __block bool doneFetching = false;
    [[[webView configuration] websiteDataStore] _fetchDataOfTypes:set.get() completionHandler:^(NSData *data) {
        EXPECT_NOT_NULL(data);
        EXPECT_GT([data length], 0u);
        doneFetching = true;
    }];
    TestWebKitAPI::Util::run(&doneFetching);
}

TEST(WebKit, FetchLocalStorageFromPersistentDataStoreThirdPartyIFrame)
{
    testFetchLocalStorageThirdPartyIFrame(adoptNS([WKWebsiteDataStore defaultDataStore]));
}

TEST(WebKit, FetchLocalStorageFromEphemeralDataStoreThirdPartyIFrame)
{
    testFetchLocalStorageThirdPartyIFrame(adoptNS([WKWebsiteDataStore nonPersistentDataStore]));
}
