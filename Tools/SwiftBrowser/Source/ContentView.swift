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

import SwiftUI
import UniformTypeIdentifiers
@_spi(Private) import WebKit

fileprivate struct ToolbarBackForwardMenuView: View {
    struct LabelConfiguration {
        let text: String
        let systemImage: String
        let key: KeyEquivalent
    }

    let list: [WebPage_v0.BackForwardList.Item]
    let label: LabelConfiguration
    let navigateToItem: (WebPage_v0.BackForwardList.Item) -> Void

    var body: some View {
        Menu {
            ForEach(list) { item in
                Button(item.title ?? item.url.absoluteString) {
                    navigateToItem(item)
                }
            }
        } label: {
            Label(label.text, systemImage: label.systemImage)
                .labelStyle(.iconOnly)
        } primaryAction: {
            navigateToItem(list.first!)
        }
        .menuIndicator(.hidden)
        .disabled(list.isEmpty)
        .keyboardShortcut(label.key)
    }
}

struct ContentView: View {
    @Binding var url: URL?

    @AppStorage(AppStorageKeys.homepage) private var homepage = "https://www.webkit.org"

    @Environment(BrowserViewModel.self) private var viewModel

    #if os(iOS)
    private static let navigationToolbarItemPlacement = ToolbarItemPlacement.bottomBar
    #else
    private static let navigationToolbarItemPlacement = ToolbarItemPlacement.navigation
    #endif

    var body: some View {
        NavigationStack {
            @Bindable var viewModel = viewModel

            WebView_v0(viewModel.page)
                .webViewAllowsBackForwardNavigationGestures()
                .webViewAllowsTabFocusingLinks()
                .webViewAllowsElementFullscreen()
                .task {
                    for await event in viewModel.page.navigations {
                        viewModel.didReceiveNavigationEvent(event)
                    }
                }
                .onAppear {
                    viewModel.displayedURL = homepage
                    viewModel.navigateToSubmittedURL()
                }
                .onChange(of: viewModel.page.url) { _, newValue in
                    url = newValue
                }
                .navigationTitle(viewModel.page.title)
                #if os(iOS)
                .navigationBarTitleDisplayMode(.inline)
                #endif
                .focusedSceneValue(viewModel)
                .onOpenURL(perform: viewModel.openURL(_:))
                .fileExporter(isPresented: $viewModel.pdfExporterIsPresented, item: viewModel.exportedPDF, defaultFilename: viewModel.exportedPDF?.title, onCompletion: viewModel.didExportPDF(result:))
                .toolbar {
                    ToolbarItemGroup(placement: Self.navigationToolbarItemPlacement) {
                        ToolbarBackForwardMenuView(
                            list: viewModel.page.backForwardList.backList.reversed(),
                            label: .init(text: "Backward", systemImage: "chevron.backward", key: "[")
                        ) {
                            viewModel.page.load(backForwardItem: $0)
                        }

                        ToolbarBackForwardMenuView(
                            list: viewModel.page.backForwardList.forwardList,
                            label: .init(text: "Forward", systemImage: "chevron.forward", key: "]")
                        ) {
                            viewModel.page.load(backForwardItem: $0)
                        }
                    }
                    
                    ToolbarItemGroup(placement: .principal) {
                        HStack {
                            VStack(spacing: 0) {
                                @Bindable var viewModel = viewModel

                                TextField("URL", text: $viewModel.displayedURL)
                                    .textContentType(.URL)
                                    .onSubmit {
                                        viewModel.navigateToSubmittedURL()
                                    }
                                    .textFieldStyle(.roundedBorder)

                                ProgressView(value: viewModel.page.estimatedProgress, total: 1.0)
                                    .padding(.horizontal, 2)
                                    .padding(.top, -4)
                                    .padding(.bottom, -8)
                            }
                            .frame(minWidth: 300)

                            if viewModel.page.isLoading {
                                Button {
                                    viewModel.page.stopLoading()
                                } label: {
                                    Image(systemName: "xmark")
                                }
                            } else {
                                Button {
                                    viewModel.page.reload()
                                } label: {
                                    Image(systemName: "arrow.clockwise")
                                }
                                .keyboardShortcut("r")
                            }
                        }
                    }
                }
        }
    }
}

#Preview {
    @Previewable @State var viewModel = BrowserViewModel()

    @Previewable @State var url: URL? = nil

    ContentView(url: $url)
        .environment(viewModel)
}
