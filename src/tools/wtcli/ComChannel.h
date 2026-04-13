// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "Channel.h"

#include <wrl/client.h>
#include <wrl/implements.h>

// COM-based channel that calls ITerminalProtocolServer methods directly.
// No JSON serialization — typed structs cross the process boundary via
// the MIDL-generated proxy/stub in OpenConsoleProxy.dll.
class ComChannel : public Channel
{
public:
    // Connect to the Terminal's COM server and authenticate.
    // clsid: e.g. "{D5B7C9E1-4F6A-4B8C-D9E0-F1A2B3C4D5E6}" from WT_COM_CLSID
    // token: auth token from WT_MCP_TOKEN (empty = dev bypass)
    static std::unique_ptr<ComChannel> Connect(const std::wstring& clsid, const std::wstring& token);

    // Channel interface
    HRESULT Authenticate(const std::wstring& token, bool& authenticated, std::wstring& protocolVersion) override;
    HRESULT GetCapabilities(std::wstring& protocolVersion, std::wstring& supportedMethodsJson) override;

    HRESULT GetActivePane(PROTOCOL_PANE_INFO& result) override;
    HRESULT ListWindows(std::vector<PROTOCOL_WINDOW_INFO>& results) override;
    HRESULT ListTabs(const std::wstring& windowIdFilter, std::vector<PROTOCOL_TAB_INFO>& results) override;
    HRESULT ListPanes(const std::wstring& windowIdFilter, const std::wstring& tabIdFilter, std::vector<PROTOCOL_PANE_INFO>& results) override;
    HRESULT ReadPaneOutput(const std::wstring& paneId, const std::wstring& source, int maxLines, PROTOCOL_PANE_OUTPUT& result) override;
    HRESULT GetProcessStatus(const std::wstring& paneId, PROTOCOL_PROCESS_STATUS& result) override;
    HRESULT GetSessionVariable(const std::wstring& paneId, const std::wstring& name, PROTOCOL_SESSION_VARIABLE& result) override;
    HRESULT GetSettings(std::wstring& settingsJson) override;

    HRESULT CreateTab(const std::wstring& windowId, const std::wstring& profile,
                      const std::wstring& commandline, const std::wstring& title,
                      bool suppressAppTitle, bool injectMcpCredentials, bool background,
                      PROTOCOL_TAB_CREATION_RESULT& result) override;
    HRESULT SplitPane(const std::wstring& paneId, const std::wstring& direction, float size,
                      const std::wstring& profile, const std::wstring& commandline,
                      bool injectMcpCredentials, bool background,
                      PROTOCOL_TAB_CREATION_RESULT& result) override;
    HRESULT ClosePane(const std::wstring& paneId) override;
    HRESULT SendInput(const std::wstring& paneId, const std::wstring& text) override;
    HRESULT SetSessionVariable(const std::wstring& paneId, const std::wstring& name, const std::wstring& value) override;
    HRESULT SetSettings(const std::wstring& settingsContent, std::wstring& backupPath) override;

    // Interactive
    HRESULT QuickPick(const std::wstring& title,
                      const std::vector<std::wstring>& choices,
                      bool allowFreeInput,
                      bool& cancelled, std::wstring& selected) override;

    // Events — push-based via callback
    HRESULT Subscribe(std::function<void(const std::wstring&)> onEvent) override;
    HRESULT Unsubscribe() override;

private:
    Microsoft::WRL::ComPtr<ITerminalProtocolServer> _server;
    Microsoft::WRL::ComPtr<ITerminalEventCallback> _callbackInstance;
};

// WRL implementation of ITerminalEventCallback that delegates to a std::function.
class EventCallback : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::RuntimeClassType::ClassicCom>,
    ITerminalEventCallback>
{
public:
    EventCallback(std::function<void(const std::wstring&)> handler) :
        _handler(std::move(handler)) {}

    STDMETHODIMP OnEvent(BSTR eventJson) override
    {
        if (_handler && eventJson)
        {
            _handler(std::wstring(eventJson, SysStringLen(eventJson)));
        }
        return S_OK;
    }

private:
    std::function<void(const std::wstring&)> _handler;
};
