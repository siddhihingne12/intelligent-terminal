// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "ComChannel.h"

#include <combaseapi.h>
#include <wil/resource.h>

// Helper: convert wstring to wil::unique_bstr for [in] params.
static wil::unique_bstr ToBstr(const std::wstring& s)
{
    return wil::unique_bstr{ SysAllocString(s.c_str()) };
}

std::unique_ptr<ComChannel> ComChannel::Connect(const std::wstring& clsidStr, const std::wstring& token)
{
    CLSID clsid{};
    HRESULT hr = CLSIDFromString(clsidStr.c_str(), &clsid);
    if (FAILED(hr))
    {
        fprintf(stderr, "[wtcli] Invalid CLSID: %ls (0x%08lX)\n", clsidStr.c_str(), hr);
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ITerminalProtocolServer> server;
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&server));
    if (FAILED(hr))
    {
        fprintf(stderr, "[wtcli] CoCreateInstance failed: 0x%08lX\n", hr);
        return nullptr;
    }

    auto channel = std::make_unique<ComChannel>();
    channel->_server = server;

    // Authenticate
    bool authenticated = false;
    std::wstring version;
    hr = channel->Authenticate(token, authenticated, version);
    if (FAILED(hr) || !authenticated)
    {
        fprintf(stderr, "[wtcli] Authentication failed (0x%08lX)\n", hr);
        return nullptr;
    }

    return channel;
}

HRESULT ComChannel::Authenticate(const std::wstring& token, bool& authenticated, std::wstring& protocolVersion)
{
    auto bstrToken = ToBstr(token);
    BOOL auth = FALSE;
    wil::unique_bstr version;

    HRESULT hr = _server->Authenticate(bstrToken.get(), &auth, &version);
    if (SUCCEEDED(hr))
    {
        authenticated = (auth != FALSE);
        protocolVersion = BstrToWstring(version.get());
    }
    return hr;
}

HRESULT ComChannel::GetCapabilities(std::wstring& protocolVersion, std::wstring& supportedMethodsJson)
{
    wil::unique_bstr ver;
    wil::unique_bstr methods;
    HRESULT hr = _server->GetCapabilities(&ver, &methods);
    if (SUCCEEDED(hr))
    {
        protocolVersion = BstrToWstring(ver.get());
        supportedMethodsJson = BstrToWstring(methods.get());
    }
    return hr;
}

HRESULT ComChannel::GetActivePane(PROTOCOL_PANE_INFO& result)
{
    return _server->GetActivePane(&result);
}

HRESULT ComChannel::ListWindows(std::vector<PROTOCOL_WINDOW_INFO>& results)
{
    UINT32 count = 0;
    PROTOCOL_WINDOW_INFO* raw = nullptr;
    HRESULT hr = _server->ListWindows(&count, &raw);
    if (SUCCEEDED(hr) && raw)
    {
        results.assign(raw, raw + count);
        CoTaskMemFree(raw);
    }
    return hr;
}

HRESULT ComChannel::ListTabs(const std::wstring& windowIdFilter, std::vector<PROTOCOL_TAB_INFO>& results)
{
    auto filter = ToBstr(windowIdFilter);
    UINT32 count = 0;
    PROTOCOL_TAB_INFO* raw = nullptr;
    HRESULT hr = _server->ListTabs(filter.get(), &count, &raw);
    if (SUCCEEDED(hr) && raw)
    {
        results.assign(raw, raw + count);
        CoTaskMemFree(raw);
    }
    return hr;
}

HRESULT ComChannel::ListPanes(const std::wstring& windowIdFilter, const std::wstring& tabIdFilter, std::vector<PROTOCOL_PANE_INFO>& results)
{
    auto wf = ToBstr(windowIdFilter);
    auto tf = ToBstr(tabIdFilter);
    UINT32 count = 0;
    PROTOCOL_PANE_INFO* raw = nullptr;
    HRESULT hr = _server->ListPanes(wf.get(), tf.get(), &count, &raw);
    if (SUCCEEDED(hr) && raw)
    {
        results.assign(raw, raw + count);
        CoTaskMemFree(raw);
    }
    return hr;
}

HRESULT ComChannel::ReadPaneOutput(const std::wstring& paneId, const std::wstring& source, int maxLines, PROTOCOL_PANE_OUTPUT& result)
{
    auto pid = ToBstr(paneId);
    auto src = ToBstr(source);
    return _server->ReadPaneOutput(pid.get(), src.get(), maxLines, &result);
}

HRESULT ComChannel::GetProcessStatus(const std::wstring& paneId, PROTOCOL_PROCESS_STATUS& result)
{
    auto pid = ToBstr(paneId);
    return _server->GetProcessStatus(pid.get(), &result);
}

HRESULT ComChannel::GetSessionVariable(const std::wstring& paneId, const std::wstring& name, PROTOCOL_SESSION_VARIABLE& result)
{
    auto pid = ToBstr(paneId);
    auto n = ToBstr(name);
    return _server->GetSessionVariable(pid.get(), n.get(), &result);
}

HRESULT ComChannel::GetSettings(std::wstring& settingsJson)
{
    wil::unique_bstr json;
    HRESULT hr = _server->GetSettings(&json);
    if (SUCCEEDED(hr))
    {
        settingsJson = BstrToWstring(json.get());
    }
    return hr;
}

HRESULT ComChannel::CreateTab(const std::wstring& windowId, const std::wstring& profile,
                               const std::wstring& commandline, const std::wstring& title,
                               bool suppressAppTitle, bool injectMcpCredentials, bool background,
                               PROTOCOL_TAB_CREATION_RESULT& result)
{
    auto wid = ToBstr(windowId);
    auto prof = ToBstr(profile);
    auto cmd = ToBstr(commandline);
    auto ttl = ToBstr(title);
    return _server->CreateTab(wid.get(), prof.get(), cmd.get(), ttl.get(),
                              suppressAppTitle ? TRUE : FALSE,
                              injectMcpCredentials ? TRUE : FALSE,
                              background ? TRUE : FALSE,
                              &result);
}

HRESULT ComChannel::SplitPane(const std::wstring& paneId, const std::wstring& direction, float size,
                               const std::wstring& profile, const std::wstring& commandline,
                               bool injectMcpCredentials, bool background,
                               PROTOCOL_TAB_CREATION_RESULT& result)
{
    auto pid = ToBstr(paneId);
    auto dir = ToBstr(direction);
    auto prof = ToBstr(profile);
    auto cmd = ToBstr(commandline);
    return _server->SplitPane(pid.get(), dir.get(), size, prof.get(), cmd.get(),
                              injectMcpCredentials ? TRUE : FALSE,
                              background ? TRUE : FALSE,
                              &result);
}

HRESULT ComChannel::ClosePane(const std::wstring& paneId)
{
    auto pid = ToBstr(paneId);
    return _server->ClosePane(pid.get());
}

HRESULT ComChannel::SendInput(const std::wstring& paneId, const std::wstring& text)
{
    auto pid = ToBstr(paneId);
    auto t = ToBstr(text);
    return _server->SendInput(pid.get(), t.get());
}

HRESULT ComChannel::SetSessionVariable(const std::wstring& paneId, const std::wstring& name, const std::wstring& value)
{
    auto pid = ToBstr(paneId);
    auto n = ToBstr(name);
    auto v = ToBstr(value);
    return _server->SetSessionVariable(pid.get(), n.get(), v.get());
}

HRESULT ComChannel::SetSettings(const std::wstring& settingsContent, std::wstring& backupPath)
{
    auto content = ToBstr(settingsContent);
    wil::unique_bstr backup;
    HRESULT hr = _server->SetSettings(content.get(), &backup);
    if (SUCCEEDED(hr))
    {
        backupPath = BstrToWstring(backup.get());
    }
    return hr;
}

HRESULT ComChannel::QuickPick(const std::wstring& title,
                               const std::vector<std::wstring>& choices,
                               bool allowFreeInput,
                               bool& cancelled, std::wstring& selected)
{
    auto bstrTitle = ToBstr(title);

    // Allocate BSTR array for choices
    std::vector<BSTR> bstrChoices(choices.size());
    auto cleanupChoices = wil::scope_exit([&]() {
        for (auto& b : bstrChoices)
            SysFreeString(b);
    });
    for (size_t i = 0; i < choices.size(); ++i)
    {
        bstrChoices[i] = SysAllocString(choices[i].c_str());
    }

    BOOL wasCancelled = TRUE;
    wil::unique_bstr selectedBstr;
    HRESULT hr = _server->QuickPick(bstrTitle.get(),
                                     static_cast<UINT32>(choices.size()),
                                     bstrChoices.data(),
                                     allowFreeInput ? TRUE : FALSE,
                                     &wasCancelled,
                                     &selectedBstr);
    if (SUCCEEDED(hr))
    {
        cancelled = (wasCancelled != FALSE);
        selected = BstrToWstring(selectedBstr.get());
    }
    return hr;
}

HRESULT ComChannel::Subscribe(std::function<void(const std::wstring&)> onEvent)
{
    auto callback = Microsoft::WRL::Make<EventCallback>(std::move(onEvent));
    HRESULT hr = _server->Subscribe(callback.Get());
    if (SUCCEEDED(hr))
    {
        _callbackInstance = callback;
    }
    return hr;
}

HRESULT ComChannel::Unsubscribe()
{
    HRESULT hr = _server->Unsubscribe();
    _callbackInstance.Reset();
    return hr;
}

// Channel::Connect factory — connects via COM using WT_COM_CLSID.
std::unique_ptr<Channel> Channel::Connect()
{
    wchar_t clsid[128]{};
    if (!GetEnvironmentVariableW(L"WT_COM_CLSID", clsid, ARRAYSIZE(clsid)))
    {
        fprintf(stderr, "[wtcli] WT_COM_CLSID not set. Must run inside a Windows Terminal pane.\n");
        return nullptr;
    }

    wchar_t token[256]{};
    GetEnvironmentVariableW(L"WT_MCP_TOKEN", token, ARRAYSIZE(token));

    return ComChannel::Connect(clsid, token);
}

// String conversion utilities
std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), result.data(), len, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(), len);
    return result;
}
