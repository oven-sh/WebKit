// Copyright (C) 2024 Apple Inc. All rights reserved.
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

#if ENABLE_SWIFTUI && compiler(>=6.0)

import Foundation
import Observation

@_spi(Private)
@MainActor
@Observable
public class WebPage_v0 {
    public static func handlesURLScheme(_ scheme: String) -> Bool {
        WKWebView.handlesURLScheme(scheme)
    }

    private init(configuration: Configuration, _navigationDecider navigationDecider: (any NavigationDeciding)?) {
        self.configuration = configuration

        // FIXME: Consider whether we want to have a single value here or if the getter for `navigations` should return a fresh sequence every time.
        let (stream, continuation) = AsyncStream.makeStream(of: NavigationEvent.self)
        navigations = Navigations(source: stream)

        backingNavigationDelegate = WKNavigationDelegateAdapter(navigationProgressContinuation: continuation, navigationDecider: navigationDecider)
        backingNavigationDelegate.owner = self
    }

    public convenience init(configuration: Configuration = Configuration(), navigationDecider: some NavigationDeciding) {
        self.init(configuration: configuration, _navigationDecider: navigationDecider)
    }

    public convenience init(configuration: Configuration = Configuration()) {
        self.init(configuration: configuration, _navigationDecider: nil)
    }

    public let navigations: Navigations

    public let configuration: Configuration

    public internal(set) var backForwardList: BackForwardList = BackForwardList()

    public var url: URL? {
        backingProperty(\.url, backedBy: \.url)
    }

    public var title: String {
        backingProperty(\.title, backedBy: \.title) { backingValue in
            // The title property is annotated as optional in WKWebView, but is never actually `nil`.
            backingValue!
        }
    }

    public var estimatedProgress: Double {
        backingProperty(\.estimatedProgress, backedBy: \.estimatedProgress)
    }

    public var isLoading: Bool {
        backingProperty(\.isLoading, backedBy: \.isLoading)
    }

    public var serverTrust: SecTrust? {
        backingProperty(\.serverTrust, backedBy: \.serverTrust)
    }

    public var hasOnlySecureContent: Bool {
        backingProperty(\.hasOnlySecureContent, backedBy: \.hasOnlySecureContent)
    }

    public var isWritingToolsActive: Bool {
        backingProperty(\.isWritingToolsActive, backedBy: \.isWritingToolsActive)
    }

    public var mediaType: String? {
        get { backingWebView.mediaType }
        set { backingWebView.mediaType = newValue }
    }

    public var customUserAgent: String? {
        get { backingWebView.customUserAgent }
        set { backingWebView.customUserAgent = newValue }
    }

    public var isInspectable: Bool {
        get { backingWebView.isInspectable }
        set { backingWebView.isInspectable = newValue }
    }

    private let backingNavigationDelegate: WKNavigationDelegateAdapter

    @ObservationIgnored
    private var observations = KeyValueObservations()

    @ObservationIgnored
    var isBoundToWebView = false

    @ObservationIgnored
    lazy var backingWebView: WKWebView = {
        let webView = WKWebView(frame: .zero, configuration: WKWebViewConfiguration(configuration))
        webView.navigationDelegate = backingNavigationDelegate
        return webView
    }()

    @discardableResult
    public func load(_ request: URLRequest) -> NavigationID? {
        backingWebView.load(request).map(NavigationID.init(_:))
    }

    @discardableResult
    public func load(_ data: Data, mimeType: String, characterEncoding: String.Encoding, baseURL: URL) -> NavigationID? {
        let cfEncoding = CFStringConvertNSStringEncodingToEncoding(characterEncoding.rawValue)
        guard cfEncoding != kCFStringEncodingInvalidId else {
            preconditionFailure("\(characterEncoding) is not a valid character encoding")
        }

        guard let convertedEncoding = CFStringConvertEncodingToIANACharSetName(cfEncoding) as? String else {
            preconditionFailure("\(characterEncoding) is not a valid character encoding")
        }

        return backingWebView.load(data, mimeType: mimeType, characterEncodingName: convertedEncoding, baseURL: baseURL).map(NavigationID.init(_:))
    }

    @discardableResult
    public func load(htmlString: String, baseURL: URL) -> NavigationID? {
        backingWebView.loadHTMLString(htmlString, baseURL: baseURL).map(NavigationID.init(_:))
    }

    @discardableResult
    public func load(fileURL url: URL, allowingReadAccessTo readAccessURL: URL) -> NavigationID? {
        backingWebView.loadFileURL(url, allowingReadAccessTo: readAccessURL).map(NavigationID.init(_:))
    }

    @discardableResult
    public func load(fileRequest request: URLRequest, allowingReadAccessTo readAccessURL: URL) -> NavigationID? {
        // `WKWebView` annotates this method as returning non-nil, but it may return nil.

        let navigation = backingWebView.loadFileRequest(request, allowingReadAccessTo: readAccessURL) as WKNavigation?
        return navigation.map(NavigationID.init(_:))
    }

    @discardableResult
    public func load(simulatedRequest request: URLRequest, response: URLResponse, responseData: Data) -> NavigationID? {
        // `WKWebView` annotates this method as returning non-nil, but it may return nil.

        let navigation = backingWebView.loadSimulatedRequest(request, response: response, responseData: responseData) as WKNavigation?
        return navigation.map(NavigationID.init(_:))
    }

    @discardableResult
    public func load(simulatedRequest request: URLRequest, responseHTML: String) -> NavigationID? {
        // `WKWebView` annotates this method as returning non-nil, but it may return nil.

        let navigation = backingWebView.loadSimulatedRequest(request, responseHTML: responseHTML) as WKNavigation?
        return navigation.map(NavigationID.init(_:))
    }

    @discardableResult
    public func load(backForwardItem: BackForwardList.Item) -> NavigationID? {
        backingWebView.go(to: backForwardItem.wrapped).map(NavigationID.init(_:))
    }

    @discardableResult
    public func reload(fromOrigin: Bool = false) -> NavigationID? {
        let navigation = fromOrigin ? backingWebView.reloadFromOrigin() : backingWebView.reload()
        return navigation.map(NavigationID.init(_:))
    }

    public func stopLoading() {
        backingWebView.stopLoading()
    }

    public func callAsyncJavaScript(_ functionBody: String, arguments: [String : Any] = [:], in frame: FrameInfo? = nil, contentWorld: WKContentWorld = .page) async throws -> Any? {
        try await backingWebView.callAsyncJavaScript(functionBody, arguments: arguments, in: frame?.wrapped, contentWorld: contentWorld)
    }

#if canImport(UIKit)
    public func snapshot(configuration: WKSnapshotConfiguration = .init()) async throws -> UIImage {
        try await backingWebView.takeSnapshot(configuration: configuration)
    }
#else
    public func snapshot(configuration: WKSnapshotConfiguration = .init()) async throws -> NSImage {
        try await backingWebView.takeSnapshot(configuration: configuration)
    }
#endif

    public func pdf(configuration: WKPDFConfiguration = .init()) async throws -> Data {
        try await backingWebView.pdf(configuration: configuration)
    }

    public func webArchiveData() async throws -> Data {
        try await withCheckedThrowingContinuation { continuation in
            backingWebView.createWebArchiveData {
                continuation.resume(with: $0)
            }
        }
    }

    private func createObservation<Value, BackingValue>(for keyPath: KeyPath<WebPage_v0, Value>, backedBy backingKeyPath: KeyPath<WKWebView, BackingValue>) -> NSKeyValueObservation {
        let boxed = UncheckedSendableKeyPathBox(keyPath: keyPath)

        return backingWebView.observe(backingKeyPath, options: [.prior, .old, .new]) { [_$observationRegistrar, unowned self] _, change in
            if change.isPrior {
                _$observationRegistrar.willSet(self, keyPath: boxed.keyPath)
            } else {
                _$observationRegistrar.didSet(self, keyPath: boxed.keyPath)
            }
        }
    }

    func backingProperty<Value, BackingValue>(_ keyPath: KeyPath<WebPage_v0, Value>, backedBy backingKeyPath: KeyPath<WKWebView, BackingValue>, _ transform: (BackingValue) -> Value) -> Value {
        if observations.contents[keyPath] == nil {
            observations.contents[keyPath] = createObservation(for: keyPath, backedBy: backingKeyPath)
        }

        self.access(keyPath: keyPath)

        let backingValue = backingWebView[keyPath: backingKeyPath]
        return transform(backingValue)
    }

    func backingProperty<Value>(_ keyPath: KeyPath<WebPage_v0, Value>, backedBy backingKeyPath: KeyPath<WKWebView, Value>) -> Value {
        backingProperty(keyPath, backedBy: backingKeyPath) { $0 }
    }
}

extension WebPage_v0 {
    private struct KeyValueObservations: ~Copyable {
        var contents: [PartialKeyPath<WebPage_v0> : NSKeyValueObservation] = [:]

        deinit {
            for (_, observation) in contents {
                observation.invalidate()
            }
        }
    }
}

/// The key path used within `createObservation` must be Sendable.
/// This is safe as long as it is not used for object subscripting and isn't created with captured subscript key paths.
fileprivate struct UncheckedSendableKeyPathBox<Root, Value>: @unchecked Sendable {
    let keyPath: KeyPath<Root, Value>
}

#endif
