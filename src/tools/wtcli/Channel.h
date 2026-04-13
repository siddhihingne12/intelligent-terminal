// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ITerminalProtocolServer.h"

#include <Windows.h>
#include <objbase.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Abstract channel interface for communicating with Windows Terminal.
// COM channel implements methods directly via typed COM calls.
struct Channel
{
    virtual ~Channel() = default;

    // Meta
    virtual HRESULT Authenticate(const std::wstring& token, bool& authenticated, std::wstring& protocolVersion) = 0;
    virtual HRESULT GetCapabilities(std::wstring& protocolVersion, std::wstring& supportedMethodsJson) = 0;

    // Queries
    virtual HRESULT GetActivePane(PROTOCOL_PANE_INFO& result) = 0;
    virtual HRESULT ListWindows(std::vector<PROTOCOL_WINDOW_INFO>& results) = 0;
    virtual HRESULT ListTabs(const std::wstring& windowIdFilter, std::vector<PROTOCOL_TAB_INFO>& results) = 0;
    virtual HRESULT ListPanes(const std::wstring& windowIdFilter, const std::wstring& tabIdFilter, std::vector<PROTOCOL_PANE_INFO>& results) = 0;
    virtual HRESULT ReadPaneOutput(const std::wstring& paneId, const std::wstring& source, int maxLines, PROTOCOL_PANE_OUTPUT& result) = 0;
    virtual HRESULT GetProcessStatus(const std::wstring& paneId, PROTOCOL_PROCESS_STATUS& result) = 0;
    virtual HRESULT GetSessionVariable(const std::wstring& paneId, const std::wstring& name, PROTOCOL_SESSION_VARIABLE& result) = 0;
    virtual HRESULT GetSettings(std::wstring& settingsJson) = 0;

    // Mutations
    virtual HRESULT CreateTab(const std::wstring& windowId, const std::wstring& profile,
                              const std::wstring& commandline, const std::wstring& title,
                              bool suppressAppTitle, bool injectMcpCredentials, bool background,
                              PROTOCOL_TAB_CREATION_RESULT& result) = 0;
    virtual HRESULT SplitPane(const std::wstring& paneId, const std::wstring& direction, float size,
                              const std::wstring& profile, const std::wstring& commandline,
                              bool injectMcpCredentials, bool background,
                              PROTOCOL_TAB_CREATION_RESULT& result) = 0;
    virtual HRESULT ClosePane(const std::wstring& paneId) = 0;
    virtual HRESULT SendInput(const std::wstring& paneId, const std::wstring& text) = 0;
    virtual HRESULT SetSessionVariable(const std::wstring& paneId, const std::wstring& name, const std::wstring& value) = 0;
    virtual HRESULT SetSettings(const std::wstring& settingsContent, std::wstring& backupPath) = 0;

    // Interactive
    virtual HRESULT QuickPick(const std::wstring& title,
                              const std::vector<std::wstring>& choices,
                              bool allowFreeInput,
                              bool& cancelled, std::wstring& selected) = 0;

    // Events — push-based via callback
    virtual HRESULT Subscribe(std::function<void(const std::wstring&)> onEvent) = 0;
    virtual HRESULT Unsubscribe() = 0;

    // Connect to Windows Terminal via COM (WT_COM_CLSID env var).
    static std::unique_ptr<Channel> Connect();
};

// Helper: free all BSTRs in a PROTOCOL_WINDOW_INFO.
inline void FreeWindowInfo(PROTOCOL_WINDOW_INFO& info) noexcept
{
    SysFreeString(info.WindowId);
    SysFreeString(info.Title);
}

inline void FreeTabInfo(PROTOCOL_TAB_INFO& info) noexcept
{
    SysFreeString(info.TabId);
    SysFreeString(info.WindowId);
    SysFreeString(info.Title);
}

inline void FreePaneInfo(PROTOCOL_PANE_INFO& info) noexcept
{
    SysFreeString(info.PaneId);
    SysFreeString(info.TabId);
    SysFreeString(info.WindowId);
    SysFreeString(info.Title);
    SysFreeString(info.Profile);
}

inline void FreePaneOutput(PROTOCOL_PANE_OUTPUT& info) noexcept
{
    SysFreeString(info.PaneId);
    SysFreeString(info.Content);
}

inline void FreeProcessStatus(PROTOCOL_PROCESS_STATUS& info) noexcept
{
    SysFreeString(info.PaneId);
    SysFreeString(info.State);
}

inline void FreeSessionVariable(PROTOCOL_SESSION_VARIABLE& info) noexcept
{
    SysFreeString(info.PaneId);
    SysFreeString(info.Name);
    SysFreeString(info.Value);
}

inline void FreeTabCreationResult(PROTOCOL_TAB_CREATION_RESULT& info) noexcept
{
    SysFreeString(info.TabId);
    SysFreeString(info.PaneId);
    SysFreeString(info.WindowId);
}

// Helper: convert BSTR to std::wstring (null-safe).
inline std::wstring BstrToWstring(BSTR bstr) noexcept
{
    return bstr ? std::wstring(bstr, SysStringLen(bstr)) : std::wstring{};
}

// Helper: convert std::wstring to narrow string (UTF-8).
std::string WideToUtf8(const std::wstring& wide);

// Helper: convert narrow string (UTF-8) to wide.
std::wstring Utf8ToWide(const std::string& utf8);
