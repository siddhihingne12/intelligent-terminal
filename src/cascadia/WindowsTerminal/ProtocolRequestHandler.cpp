// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ProtocolRequestHandler.h"
#include "TerminalProtocolServer.h"
#include "WindowEmperor.h"
#include "AppHost.h"

#include <json/json.h>
#include <til/io.h>

using namespace winrt;
using namespace winrt::TerminalApp;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::Foundation;

const std::vector<std::string> ProtocolRequestHandler::_supportedMethods = {
    "authenticate",
    "get_capabilities",
    "get_active_pane",
    "list_windows",
    "list_tabs",
    "list_panes",
    "read_pane_output",
    "get_process_status",
    "get_session_variable",
    "get_settings",
    "create_tab",
    "split_pane",
    "close_pane",
    "send_input",
    "set_session_variable",
    "set_settings",
    "quick_pick",
};

ProtocolRequestHandler::ProtocolRequestHandler(WindowEmperor& emperor) :
    _emperor(emperor)
{
}

ProtocolRequestHandler::~ProtocolRequestHandler()
{
    s_pipeServer.store(nullptr, std::memory_order_release);
}

std::atomic<TerminalProtocolServer*> ProtocolRequestHandler::s_pipeServer{ nullptr };

void ProtocolRequestHandler::SetAuthToken(const std::string& token)
{
    _authToken = token;
}

void ProtocolRequestHandler::SetServer(TerminalProtocolServer* server)
{
    _server = server;
    s_pipeServer.store(server, std::memory_order_release);
}

Json::Value ProtocolRequestHandler::_makeError(const std::string& code, const std::string& message)
{
    Json::Value error;
    error["code"] = code;
    error["message"] = message;
    return error;
}

Json::Value ProtocolRequestHandler::_makeResponse(const std::string& id, const Json::Value& result)
{
    Json::Value response;
    response["type"] = "response";
    response["id"] = id;
    response["result"] = result;
    response["error"] = Json::nullValue;
    return response;
}

Json::Value ProtocolRequestHandler::_makeErrorResponse(const std::string& id, const std::string& code, const std::string& message)
{
    Json::Value response;
    response["type"] = "response";
    response["id"] = id;
    response["result"] = Json::nullValue;
    response["error"] = _makeError(code, message);
    return response;
}

Json::Value ProtocolRequestHandler::HandleRequest(const Json::Value& request, bool& isAuthenticated)
{
    const auto id = request.get("id", "").asString();
    const auto type = request.get("type", "").asString();
    const auto method = request.get("method", "").asString();
    const auto& params = request.get("params", Json::objectValue);

    if (type != "request")
    {
        return _makeErrorResponse(id, "invalid_params", "Message type must be 'request'.");
    }

    if (method.empty())
    {
        return _makeErrorResponse(id, "invalid_method", "Method is required.");
    }

    // The authenticate method is always allowed.
    if (method == "authenticate")
    {
        try
        {
            auto result = _handleAuthenticate(params, isAuthenticated);
            return _makeResponse(id, result);
        }
        catch (...)
        {
            return _makeErrorResponse(id, "auth_failed", "Invalid authentication token.");
        }
    }

    // All other methods require authentication.
    if (!isAuthenticated)
    {
        return _makeErrorResponse(id, "auth_required", "Authentication required. Send 'authenticate' first.");
    }

    // Lazily register for page VT events on first authenticated request.
    // By this point windows and pages are fully initialized.
    _ensurePageEventsRegistered();

    // Check per-action confirmation for non-read operations.
    if (!_checkConfirmation(method, params))
    {
        return _makeErrorResponse(id, "confirmation_denied", "User denied the operation.");
    }

    try
    {
        Json::Value result;

        if (method == "get_capabilities")
            result = _handleGetCapabilities(params);
        else if (method == "get_active_pane")
            result = _handleGetActivePane(params);
        else if (method == "list_windows")
            result = _handleListWindows(params);
        else if (method == "list_tabs")
            result = _handleListTabs(params);
        else if (method == "list_panes")
            result = _handleListPanes(params);
        else if (method == "read_pane_output")
            result = _handleReadPaneOutput(params);
        else if (method == "get_process_status")
            result = _handleGetProcessStatus(params);
        else if (method == "get_session_variable")
            result = _handleGetSessionVariable(params);
        else if (method == "get_settings")
            result = _handleGetSettings(params);
        else if (method == "create_tab")
            result = _handleCreateTab(params);
        else if (method == "split_pane")
            result = _handleSplitPane(params);
        else if (method == "close_pane")
            result = _handleClosePane(params);
        else if (method == "send_input")
            result = _handleSendInput(params);
        else if (method == "set_session_variable")
            result = _handleSetSessionVariable(params);
        else if (method == "set_settings")
            result = _handleSetSettings(params);
        else if (method == "quick_pick")
            result = _handleQuickPick(params);
        else
            return _makeErrorResponse(id, "invalid_method", "Unknown method: " + method);

        return _makeResponse(id, result);
    }
    catch (const std::exception& e)
    {
        return _makeErrorResponse(id, "internal_error", e.what());
    }
    catch (const winrt::hresult_error& e)
    {
        return _makeErrorResponse(id, "internal_error", winrt::to_string(e.message()));
    }
}

// ============================================================================
// Meta Operations
// ============================================================================

Json::Value ProtocolRequestHandler::_handleAuthenticate(const Json::Value& params, bool& isAuthenticated)
{
    const auto token = params.get("token", "").asString();

    // DEV BYPASS: allow empty token to authenticate without credentials.
    // This lets wta connect using just WT_PIPE_NAME without needing the token.
    // TODO: Remove this bypass before shipping.
    if (token.empty())
    {
        isAuthenticated = true;
        Json::Value result;
        result["authenticated"] = true;
        result["protocol_version"] = "1.0";
        return result;
    }

    // Constant-time comparison to prevent timing attacks.
    bool match = (token.size() == _authToken.size());
    volatile bool dummy = false;
    for (size_t i = 0; i < std::min(token.size(), _authToken.size()); ++i)
    {
        if (token[i] != _authToken[i])
        {
            dummy = true;
        }
    }
    match = match && !dummy;

    isAuthenticated = match;

    Json::Value result;
    result["authenticated"] = match;
    result["protocol_version"] = "1.0";

    if (!match)
    {
        throw std::runtime_error("auth_failed");
    }

    return result;
}

Json::Value ProtocolRequestHandler::_handleGetCapabilities(const Json::Value& /*params*/)
{
    Json::Value result;
    result["protocol_version"] = "1.0";

    Json::Value methods(Json::arrayValue);
    for (const auto& m : _supportedMethods)
    {
        methods.append(m);
    }
    result["methods"] = methods;

    return result;
}

// ============================================================================
// Helper: Get TerminalPage from AppHost
// ============================================================================

static TerminalApp::TerminalPage _getPage(AppHost* host)
{
    if (!host)
    {
        return nullptr;
    }
    const auto logic = host->Logic();
    if (!logic)
    {
        return nullptr;
    }
    // GetRoot() returns UIElement which is the TerminalPage
    const auto root = logic.GetRoot();
    if (!root)
    {
        return nullptr;
    }
    return root.try_as<TerminalApp::TerminalPage>();
}

void ProtocolRequestHandler::_ensurePageEventsRegistered()
{
    if (_pageEventsRegistered || !_server)
        return;
    _pageEventsRegistered = true;

    // Subscribe directly — til::typed_event is thread-safe for subscription
    // from any thread.  The handler may be invoked on the UI thread (since
    // TerminalPage raises the event there for safe _tabs access), so we
    // dispatch pipe broadcast to the thread pool.
    // NOTE: COM client notification is handled by TerminalProtocolComServer's
    // own subscription — do NOT call s_NotifyEventToComClients here to avoid
    // duplicate events.
    for (const auto& host : _emperor._windows)
    {
        const auto page = _getPage(host.get());
        if (!page)
            continue;

        page.ProtocolVtSequenceReceived(
            [](auto&&, const winrt::hstring& eventJson) {
                auto* svr = ProtocolRequestHandler::GetPipeServer();
                if (!svr)
                    return;
                auto* ctx = new std::string(winrt::to_string(eventJson));
                TrySubmitThreadpoolCallback(
                    [](PTP_CALLBACK_INSTANCE, PVOID p) {
                        auto* str = static_cast<std::string*>(p);
                        if (auto* svr = ProtocolRequestHandler::GetPipeServer())
                        {
                            svr->BroadcastEvent(*str);
                        }
                        delete str;
                    },
                    ctx, nullptr);
            });
        break; // Single-window for now
    }
}

// ============================================================================
// Query Operations
// ============================================================================

Json::Value ProtocolRequestHandler::_handleGetActivePane(const Json::Value& /*params*/)
{
    // Find the most recently focused window and get its active pane.
    const auto host = _emperor._mostRecentWindow();
    if (!host)
    {
        throw std::runtime_error("No windows available.");
    }

    const auto page = _getPage(host);
    if (!page)
    {
        throw std::runtime_error("Terminal page not available.");
    }

    const auto info = page.GetProtocolActivePane();
    if (info.PaneId.empty())
    {
        throw std::runtime_error("No active pane.");
    }

    const auto& props = host->Logic().WindowProperties();

    Json::Value result;
    result["pane_id"] = winrt::to_string(info.PaneId);
    result["tab_id"] = winrt::to_string(info.TabId);
    result["window_id"] = std::to_string(props.WindowId());
    result["title"] = winrt::to_string(info.Title);
    result["profile"] = winrt::to_string(info.Profile);
    result["is_active"] = info.IsActive;
    result["pid"] = static_cast<Json::UInt>(info.Pid);
    return result;
}

Json::Value ProtocolRequestHandler::_handleListWindows(const Json::Value& /*params*/)
{
    Json::Value result;
    Json::Value windows(Json::arrayValue);

    const auto mostRecent = _emperor._mostRecentWindow();

    for (const auto& host : _emperor._windows)
    {
        const auto logic = host->Logic();
        if (!logic)
        {
            continue;
        }

        const auto& props = logic.WindowProperties();

        Json::Value win;
        win["window_id"] = std::to_string(props.WindowId());
        win["title"] = winrt::to_string(props.WindowNameForDisplay());
        win["is_focused"] = (host.get() == mostRecent);
        win["tab_count"] = static_cast<Json::UInt>(logic.TabCount());
        windows.append(win);
    }

    result["windows"] = windows;
    return result;
}

Json::Value ProtocolRequestHandler::_handleListTabs(const Json::Value& params)
{
    const auto windowIdFilter = params.get("window_id", "").asString();

    Json::Value result;
    Json::Value allTabs(Json::arrayValue);

    for (const auto& host : _emperor._windows)
    {
        const auto logic = host->Logic();
        if (!logic)
        {
            continue;
        }

        const auto& props = logic.WindowProperties();
        const auto windowIdStr = std::to_string(props.WindowId());

        if (!windowIdFilter.empty() && windowIdStr != windowIdFilter)
        {
            continue;
        }

        const auto page = _getPage(host.get());
        if (!page)
        {
            continue;
        }

        const auto tabs = page.GetProtocolTabs();
        for (uint32_t i = 0; i < tabs.Size(); ++i)
        {
            const auto& t = tabs.GetAt(i);
            Json::Value tab;
            tab["tab_id"] = winrt::to_string(t.TabId);
            tab["window_id"] = windowIdStr;
            tab["title"] = winrt::to_string(t.Title);
            tab["is_active"] = t.IsActive;
            tab["pane_count"] = t.PaneCount;
            allTabs.append(tab);
        }
    }

    result["tabs"] = allTabs;
    return result;
}

Json::Value ProtocolRequestHandler::_handleListPanes(const Json::Value& params)
{
    const auto tabIdFilter = params.get("tab_id", "").asString();
    const auto windowIdFilter = params.get("window_id", "").asString();

    Json::Value result;
    Json::Value allPanes(Json::arrayValue);

    for (const auto& host : _emperor._windows)
    {
        const auto logic = host->Logic();
        if (!logic)
        {
            continue;
        }

        const auto& props = logic.WindowProperties();
        const auto windowIdStr = std::to_string(props.WindowId());

        if (!windowIdFilter.empty() && windowIdStr != windowIdFilter)
        {
            continue;
        }

        const auto page = _getPage(host.get());
        if (!page)
        {
            continue;
        }

        const auto panes = page.GetProtocolPanes(winrt::to_hstring(tabIdFilter));
        for (uint32_t i = 0; i < panes.Size(); ++i)
        {
            const auto& p = panes.GetAt(i);
            Json::Value pane;
            pane["pane_id"] = winrt::to_string(p.PaneId);
            pane["tab_id"] = winrt::to_string(p.TabId);
            pane["window_id"] = windowIdStr;
            pane["title"] = winrt::to_string(p.Title);
            pane["profile"] = winrt::to_string(p.Profile);
            pane["is_active"] = p.IsActive;
            pane["pid"] = static_cast<Json::UInt>(p.Pid);
            pane["size"]["rows"] = p.Rows;
            pane["size"]["columns"] = p.Columns;
            allPanes.append(pane);
        }
    }

    result["panes"] = allPanes;
    return result;
}

Json::Value ProtocolRequestHandler::_handleReadPaneOutput(const Json::Value& params)
{
    const auto paneIdStr = params.get("pane_id", "").asString();
    const auto source = params.get("source", "scrollback").asString();
    const auto maxLines = params.get("max_lines", 200).asInt();

    if (paneIdStr.empty())
    {
        throw std::runtime_error("pane_id is required.");
    }

    // Search across all windows for the pane.
    for (const auto& host : _emperor._windows)
    {
        const auto page = _getPage(host.get());
        if (!page)
        {
            continue;
        }

        const auto info = page.ReadProtocolPaneOutput(
            winrt::to_hstring(paneIdStr),
            winrt::to_hstring(source),
            maxLines);

        if (!info.PaneId.empty())
        {
            Json::Value result;
            result["pane_id"] = winrt::to_string(info.PaneId);
            result["content"] = winrt::to_string(info.Content);
            result["line_count"] = info.LineCount;
            result["truncated"] = info.Truncated;
            return result;
        }
    }

    throw std::runtime_error("Pane not found: " + paneIdStr);
}

Json::Value ProtocolRequestHandler::_handleGetProcessStatus(const Json::Value& params)
{
    const auto paneIdStr = params.get("pane_id", "").asString();
    if (paneIdStr.empty())
    {
        throw std::runtime_error("pane_id is required.");
    }

    for (const auto& host : _emperor._windows)
    {
        const auto page = _getPage(host.get());
        if (!page)
        {
            continue;
        }

        const auto info = page.GetProtocolProcessStatus(winrt::to_hstring(paneIdStr));
        if (!info.PaneId.empty())
        {
            Json::Value result;
            result["pane_id"] = winrt::to_string(info.PaneId);
            result["state"] = winrt::to_string(info.State);
            result["pid"] = static_cast<Json::UInt>(info.Pid);
            if (info.HasExitCode)
                result["exit_code"] = info.ExitCode;
            return result;
        }
    }

    throw std::runtime_error("Pane not found: " + paneIdStr);
}

Json::Value ProtocolRequestHandler::_handleGetSessionVariable(const Json::Value& params)
{
    const auto paneIdStr = params.get("pane_id", "").asString();
    const auto name = params.get("name", "").asString();

    if (paneIdStr.empty() || name.empty())
    {
        throw std::runtime_error("pane_id and name are required.");
    }

    for (const auto& host : _emperor._windows)
    {
        const auto page = _getPage(host.get());
        if (!page)
        {
            continue;
        }

        const auto info = page.GetProtocolSessionVariable(
            winrt::to_hstring(paneIdStr),
            winrt::to_hstring(name));

        if (!info.PaneId.empty())
        {
            Json::Value result;
            result["pane_id"] = winrt::to_string(info.PaneId);
            result["name"] = winrt::to_string(info.Name);
            result["value"] = info.Exists ? winrt::to_string(info.Value) : "";
            result["exists"] = info.Exists;
            return result;
        }
    }

    throw std::runtime_error("Pane not found: " + paneIdStr);
}

Json::Value ProtocolRequestHandler::_handleGetSettings(const Json::Value& /*params*/)
{
    const std::filesystem::path settingsPath{ std::wstring_view{ CascadiaSettings::SettingsPath() } };
    const auto settingsContent = til::io::read_file_as_utf8_string_if_exists(settingsPath);

    Json::Value result;
    result["settings"] = settingsContent;
    return result;
}

// ============================================================================
// Mutation Operations
// ============================================================================

Json::Value ProtocolRequestHandler::_handleCreateTab(const Json::Value& params)
{
    const auto windowIdStr = params.get("window_id", "").asString();
    const auto profile = params.get("profile", "").asString();
    const auto commandline = params.get("commandline", "").asString();
    const auto title = params.get("title", "").asString();
    const auto suppressAppTitle = params.get("suppress_application_title", false).asBool();
    const auto injectMcpCredentials = params.get("inject_mcp_credentials", false).asBool();

    // Find target window.
    AppHost* targetHost = nullptr;
    if (!windowIdStr.empty())
    {
        const auto windowId = std::stoull(windowIdStr);
        targetHost = _emperor.GetWindowById(windowId);
    }
    else
    {
        targetHost = _emperor._mostRecentWindow();
    }

    if (!targetHost)
    {
        throw std::runtime_error("Window not found.");
    }

    const auto page = _getPage(targetHost);
    if (!page)
    {
        throw std::runtime_error("Terminal page not available.");
    }

    // Build NewTerminalArgs.
    NewTerminalArgs newTermArgs;
    if (!profile.empty())
    {
        newTermArgs.Profile(winrt::to_hstring(profile));
    }
    if (!commandline.empty())
    {
        newTermArgs.Commandline(winrt::to_hstring(commandline));
    }
    if (!title.empty())
    {
        newTermArgs.TabTitle(winrt::to_hstring(title));
        if (suppressAppTitle)
        {
            newTermArgs.SuppressApplicationTitle(true);
        }
    }

    // Only inject MCP credentials when explicitly requested (for delegate AI CLIs).
    if (injectMcpCredentials && !_authToken.empty())
    {
        page.SetPendingProtocolEnv(L"WT_MCP_TOKEN", winrt::to_hstring(_authToken));
        page.SetPendingProtocolEnv(L"WT_PIPE_NAME", winrt::hstring{ _emperor.GetProtocolPipeName() });
        const auto& comClsid = _emperor.GetComClsid();
        if (!comClsid.empty())
        {
            page.SetPendingProtocolEnv(L"WT_COM_CLSID", winrt::hstring{ comClsid });
        }
    }

    const auto background = params.get("background", true).asBool();

    const auto cr = page.CreateProtocolTab(newTermArgs, background);
    // Note: CreateProtocolTab clears pending env vars internally.
    if (cr.TabId.empty())
    {
        throw std::runtime_error("Failed to create tab.");
    }

    const auto& props = targetHost->Logic().WindowProperties();

    Json::Value result;
    result["tab_id"] = winrt::to_string(cr.TabId);
    result["pane_id"] = winrt::to_string(cr.PaneId);
    result["window_id"] = std::to_string(props.WindowId());
    if (cr.Pid != 0)
        result["pid"] = static_cast<Json::UInt>(cr.Pid);
    return result;
}

Json::Value ProtocolRequestHandler::_handleSplitPane(const Json::Value& params)
{
    const auto paneIdStr = params.get("pane_id", "").asString();
    const auto directionStr = params.get("direction", "right").asString();
    const auto profile = params.get("profile", "").asString();
    const auto commandline = params.get("commandline", "").asString();
    const auto size = params.get("size", 0.5).asFloat();
    const auto injectMcpCredentials = params.get("inject_mcp_credentials", false).asBool();

    if (paneIdStr.empty())
    {
        throw std::runtime_error("pane_id is required.");
    }

    // Map direction string to SplitDirection enum.
    SplitDirection splitDir = SplitDirection::Right;
    if (directionStr == "left")
        splitDir = SplitDirection::Left;
    else if (directionStr == "up")
        splitDir = SplitDirection::Up;
    else if (directionStr == "down")
        splitDir = SplitDirection::Down;

    // Build NewTerminalArgs.
    NewTerminalArgs newTermArgs;
    if (!profile.empty())
    {
        newTermArgs.Profile(winrt::to_hstring(profile));
    }
    if (!commandline.empty())
    {
        newTermArgs.Commandline(winrt::to_hstring(commandline));
    }

    const auto background = params.get("background", true).asBool();

    // Search across all windows for the target pane.
    for (const auto& host : _emperor._windows)
    {
        const auto page = _getPage(host.get());
        if (!page)
        {
            continue;
        }

        // Only inject MCP credentials when explicitly requested (for delegate AI CLIs).
        if (injectMcpCredentials && !_authToken.empty())
        {
            page.SetPendingProtocolEnv(L"WT_MCP_TOKEN", winrt::to_hstring(_authToken));
            page.SetPendingProtocolEnv(L"WT_PIPE_NAME", winrt::hstring{ _emperor.GetProtocolPipeName() });
        }

        // Note: SplitProtocolPane clears pending env vars internally.
        const auto cr = page.SplitProtocolPane(
            winrt::to_hstring(paneIdStr), splitDir, size, newTermArgs, background);

        if (!cr.TabId.empty())
        {
            Json::Value result;
            result["tab_id"] = winrt::to_string(cr.TabId);
            result["pane_id"] = winrt::to_string(cr.PaneId);
            if (cr.Pid != 0)
                result["pid"] = static_cast<Json::UInt>(cr.Pid);
            return result;
        }
    }

    throw std::runtime_error("Pane not found: " + paneIdStr);
}

Json::Value ProtocolRequestHandler::_handleClosePane(const Json::Value& params)
{
    const auto paneIdStr = params.get("pane_id", "").asString();
    if (paneIdStr.empty())
    {
        throw std::runtime_error("pane_id is required.");
    }

    for (const auto& host : _emperor._windows)
    {
        const auto page = _getPage(host.get());
        if (!page)
        {
            continue;
        }

        if (page.CloseProtocolPane(winrt::to_hstring(paneIdStr)))
        {
            Json::Value result;
            result["closed"] = true;
            return result;
        }
    }

    throw std::runtime_error("Pane not found: " + paneIdStr);
}

Json::Value ProtocolRequestHandler::_handleSendInput(const Json::Value& params)
{
    const auto paneIdStr = params.get("pane_id", "").asString();
    const auto text = params.get("text", "").asString();

    if (paneIdStr.empty())
    {
        throw std::runtime_error("pane_id is required.");
    }
    if (text.empty())
    {
        throw std::runtime_error("text is required.");
    }

    for (const auto& host : _emperor._windows)
    {
        const auto page = _getPage(host.get());
        if (!page)
        {
            continue;
        }

        if (page.SendProtocolInput(winrt::to_hstring(paneIdStr), winrt::to_hstring(text)))
        {
            Json::Value result;
            result["sent"] = true;
            return result;
        }
    }

    throw std::runtime_error("Pane not found: " + paneIdStr);
}

Json::Value ProtocolRequestHandler::_handleSetSessionVariable(const Json::Value& params)
{
    const auto paneIdStr = params.get("pane_id", "").asString();
    const auto name = params.get("name", "").asString();

    if (paneIdStr.empty() || name.empty())
    {
        throw std::runtime_error("pane_id and name are required.");
    }

    const auto value = params["value"].isNull() ? "" : params.get("value", "").asString();
    const auto isDelete = params["value"].isNull();

    for (const auto& host : _emperor._windows)
    {
        const auto page = _getPage(host.get());
        if (!page)
        {
            continue;
        }

        // SetProtocolSessionVariable handles both set and delete (empty value = delete).
        if (page.SetProtocolSessionVariable(
                winrt::to_hstring(paneIdStr),
                winrt::to_hstring(name),
                isDelete ? L"" : winrt::to_hstring(value)))
        {
            Json::Value result;
            result["set"] = true;
            return result;
        }
    }

    throw std::runtime_error("Pane not found: " + paneIdStr);
}

Json::Value ProtocolRequestHandler::_handleSetSettings(const Json::Value& params)
{
    const auto settingsContent = params.get("settings", "").asString();
    if (settingsContent.empty())
    {
        throw std::runtime_error("settings content is required.");
    }

    // Validate that it's valid JSON.
    Json::Value parsedSettings;
    Json::CharReaderBuilder readerBuilder;
    std::string parseErrors;
    std::istringstream stream(settingsContent);
    if (!Json::parseFromStream(readerBuilder, stream, &parsedSettings, &parseErrors))
    {
        throw std::runtime_error("Invalid JSON in settings: " + parseErrors);
    }

    // Get the settings path and create a backup.
    const std::filesystem::path settingsPath{ std::wstring_view{ CascadiaSettings::SettingsPath() } };
    const auto settingsDir = settingsPath.parent_path();

    // Create timestamped backup.
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &time);

    wchar_t timeStr[64];
    wcsftime(timeStr, std::size(timeStr), L"%Y-%m-%dT%H-%M-%S", &tm);

    const auto backupPath = settingsDir / fmt::format(L"settings.backup.{}.json", timeStr);

    // Copy current settings to backup.
    std::error_code ec;
    std::filesystem::copy_file(settingsPath, backupPath, std::filesystem::copy_options::overwrite_existing, ec);

    // Clean up old backups - keep only the most recent 5.
    std::vector<std::filesystem::path> backups;
    for (const auto& entry : std::filesystem::directory_iterator(settingsDir, ec))
    {
        if (entry.is_regular_file() && entry.path().filename().wstring().starts_with(L"settings.backup."))
        {
            backups.push_back(entry.path());
        }
    }

    if (backups.size() > 5)
    {
        std::sort(backups.begin(), backups.end());
        for (size_t i = 0; i < backups.size() - 5; ++i)
        {
            std::filesystem::remove(backups[i], ec);
        }
    }

    // Write the new settings.
    til::io::write_utf8_string_to_file_atomic(settingsPath, settingsContent);

    Json::Value result;
    result["applied"] = true;
    result["backup_path"] = winrt::to_string(backupPath.wstring());
    return result;
}

Json::Value ProtocolRequestHandler::_handleQuickPick(const Json::Value& params)
{
    const auto title = params.get("title", "").asString();
    const auto& choices = params.get("choices", Json::arrayValue);
    const auto allowFreeInput = params.get("allow_free_input", false).asBool();

    if (!choices.isArray() || choices.empty())
    {
        throw std::runtime_error("choices array is required and must not be empty.");
    }

    // Serialize choices back to JSON string for the WinRT boundary.
    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "";
    const auto choicesJsonStr = Json::writeString(writerBuilder, choices);

    // Show the quick pick dialog in the most recently focused window.
    const auto host = _emperor._mostRecentWindow();
    if (!host)
    {
        throw std::runtime_error("No windows available.");
    }

    const auto page = _getPage(host);
    if (!page)
    {
        throw std::runtime_error("Terminal page not available.");
    }

    const auto resultJson = winrt::to_string(page.ShowProtocolQuickPick(
        winrt::to_hstring(title),
        winrt::to_hstring(choicesJsonStr),
        allowFreeInput));

    if (resultJson.empty())
    {
        throw std::runtime_error("Quick pick dialog failed.");
    }

    Json::Value result;
    Json::CharReaderBuilder readerBuilder;
    std::string parseErrors;
    std::istringstream stream(resultJson);
    if (!Json::parseFromStream(readerBuilder, stream, &result, &parseErrors))
    {
        throw std::runtime_error("Failed to parse quick pick result.");
    }

    return result;
}

// ============================================================================
// Thread Marshaling Helpers
// ============================================================================


ProtocolRequestHandler::PaneLookupResult ProtocolRequestHandler::_findPaneGlobally(uint32_t paneId)
{
    UNREFERENCED_PARAMETER(paneId);
    PaneLookupResult result{};
    // This is now handled by TerminalPage's protocol methods directly.
    return result;
}

// ============================================================================
// Per-Action Confirmation (Phase 4)
// ============================================================================

ProtocolRequestHandler::RiskLevel ProtocolRequestHandler::_getRiskLevel(const std::string& method)
{
    // Read operations - auto-approve by default
    if (method == "authenticate" ||
        method == "get_capabilities" ||
        method == "list_windows" ||
        method == "list_tabs" ||
        method == "list_panes" ||
        method == "read_pane_output" ||
        method == "get_process_status" ||
        method == "get_session_variable" ||
        method == "get_settings")
    {
        return RiskLevel::Read;
    }

    // Quick pick is a UI prompt, not a mutation — auto-approve
    if (method == "quick_pick")
    {
        return RiskLevel::Read;
    }

    // Input operations - highest risk
    if (method == "send_input")
    {
        return RiskLevel::Input;
    }

    // Create/mutation operations - medium risk
    // Includes: create_tab, split_pane, close_pane, set_session_variable, set_settings
    return RiskLevel::Create;
}

bool ProtocolRequestHandler::_checkConfirmation(const std::string& method, const Json::Value& /*params*/)
{
    // Phase 4 pass-through implementation:
    // Auto-approve all operations during development.
    // When _autoApproveAll is false, read operations are still auto-approved,
    // but create and input operations would need user confirmation.
    if (_autoApproveAll)
    {
        return true;
    }

    const auto risk = _getRiskLevel(method);
    switch (risk)
    {
    case RiskLevel::Read:
        return true; // Always auto-approve reads

    case RiskLevel::Create:
    case RiskLevel::Input:
        // TODO: Show confirmation dialog in target window.
        // For now, auto-approve.
        return true;
    }

    return true;
}
