// Copyright (C) 2026 Apple Inc. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
// BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.

// The Swift counterparts of SiteIsolationTestUtilities.h, for site isolation tests written in Swift.

public import CoreGraphics
import Foundation
public import TestWebKitAPILibrary.Helpers.cocoa.TestNavigationDelegate
public import TestWebKitAPILibrary.Helpers.cocoa.TestWKWebView
public import WebKit
private import WebKit_Private.WKFrameInfoPrivate
private import WebKit_Private.WKPreferencesPrivate
public import WebKit_Private.WKWebViewPrivate
private import WebKit_Private.WKWebsiteDataStorePrivate
private import WebKit_Private._WKFrameTreeNode
private import WebKit_Private._WKWebsiteDataStoreConfiguration

import struct Foundation.URL
import struct Swift.String

extension WKWebViewConfiguration {
    /// Creates a configuration whose website data store sends HTTPS loads through a server's proxy.
    ///
    /// The server must use the `.httpsProxy` protocol. Loads from every `https` host then reach the server, so a
    /// test can use several sites, such as `https://example.com` and `https://webkit.org`. The server routes
    /// requests by path alone, so each route needs a path that's distinct across all of those sites.
    ///
    /// - Parameter server: The configuration of the server.
    public convenience init(httpsProxyFor server: HTTPServer.Configuration) {
        self.init()

        let storeConfiguration = _WKWebsiteDataStoreConfiguration(nonPersistentConfiguration: ())
        storeConfiguration.httpsProxy = server.httpsProxy
        websiteDataStore = WKWebsiteDataStore._store(with: storeConfiguration)
    }
}

/// Creates a web view with site isolation enabled, and a navigation delegate that trusts any server certificate.
///
/// A web view only holds its navigation delegate weakly, so keep the delegate alive for as long as the web view
/// loads anything.
///
/// - Parameters:
///   - configuration: The configuration for the web view, such as one created with
///     ``WebKit/WKWebViewConfiguration/init(httpsProxyFor:)``. This enables site isolation in its preferences.
///   - frame: The frame of the web view.
/// - Returns: The web view and its navigation delegate.
@MainActor
public func siteIsolatedWebViewAndDelegate(
    configuration: WKWebViewConfiguration,
    frame: CGRect = .zero
) -> (webView: TestWKWebView, navigationDelegate: TestNavigationDelegate) {
    configuration.preferences._siteIsolationEnabled = true

    let navigationDelegate = TestNavigationDelegate()
    navigationDelegate.allowAnyTLSCertificate()

    let webView = TestWKWebView(frame: frame, configuration: configuration)
    webView.navigationDelegate = navigationDelegate

    return (webView, navigationDelegate)
}

/// A site-isolated web view that has loaded a page, and focused the cross-origin iframe in it.
@MainActor
public struct WebViewWithFocusedCrossOriginIframe {
    /// Markup for a main frame with some text and a 400x300 cross-origin iframe with ID `iframe`, loaded from
    /// `https://webkit.org/iframe`.
    public static let mainFrameHTML =
        "<body style='margin: 0'>main frame text"
        + "<iframe id='iframe' style='width: 400px; height: 300px; border: none;' src='https://webkit.org/iframe'></iframe></body>"

    /// The web view, which is 800x600.
    public let webView: TestWKWebView

    /// The navigation delegate of the web view, which trusts any server certificate.
    public let navigationDelegate: TestNavigationDelegate

    /// The focused iframe.
    public let childFrame: WKFrameInfo

    // swift-format-ignore: NeverForceUnwrap
    /// Loads `https://example.com/mainframe` in a site-isolated web view, then focuses its first child frame.
    ///
    /// The first child frame must be an iframe with ID `iframe`, as in ``mainFrameHTML``.
    ///
    /// - Parameter configuration: The configuration for the web view, which must send HTTPS loads to the test's
    ///   server, such as one created with ``WebKit/WKWebViewConfiguration/init(httpsProxyFor:)``.
    /// - Throws: The navigation error if the page fails to load, or ``ConditionTimedOut`` if the iframe doesn't get
    ///   focus.
    public init(configuration: WKWebViewConfiguration) async throws {
        let (webView, navigationDelegate) = siteIsolatedWebViewAndDelegate(
            configuration: configuration,
            frame: CGRect(x: 0, y: 0, width: 800, height: 600)
        )

        // Well formed, so this cannot fail.
        webView.load(URLRequest(url: URL(string: "https://example.com/mainframe")!))
        try await navigationDelegate.waitForDidFinishNavigation()
        await webView.nextPresentationUpdate()
        #if WTF_PLATFORM_IOS_FAMILY
        webView.focusInWindow()
        #endif

        try await webView.callJavaScript { "document.getElementById('iframe').focus()" }

        // Frame infos are snapshots, so get a new one each time to see whether the iframe has focus yet.
        var childFrame: WKFrameInfo?
        try await waitForCondition("the cross-origin iframe to have focus") {
            childFrame = await webView.firstChildFrame
            return childFrame?._isFocused ?? false
        }

        self.webView = webView
        self.navigationDelegate = navigationDelegate
        // waitForCondition only returns once childFrame is a focused frame.
        self.childFrame = childFrame!
    }
}

extension WKWebView {
    /// Runs a script that changes the selection in a frame, then waits for the selection attributes of the web view
    /// to reflect the change.
    ///
    /// Some commands check the UI process's editor state before sending anything to a web process, so they need it
    /// to be up to date with the selection.
    ///
    /// - Parameters:
    ///   - frame: The frame to run the script in.
    ///   - script: A script that changes the selection, such as `getSelection().selectAllChildren(document.body)`.
    ///   - expectedSelection: The kind of selection the script makes.
    /// - Throws: Any error raised while running the script, or ``ConditionTimedOut`` if the selection attributes don't
    ///   come to include `expectedSelection`.
    public func setSelection(
        in frame: WKFrameInfo,
        script: String,
        expecting expectedSelection: _WKSelectionAttributes
    ) async throws {
        try await callJavaScript(in: frame) { script }
        try await waitForCondition("the selection attributes to include \(expectedSelection)") {
            _selectionAttributes.contains(expectedSelection)
        }
    }

    /// The first child frame of the main frame, or `nil` if the main frame has no child frames.
    public var firstChildFrame: WKFrameInfo? {
        get async {
            let mainFrame: _WKFrameTreeNode? = await _frames()
            return mainFrame?.childFrames.first?.info
        }
    }
}
