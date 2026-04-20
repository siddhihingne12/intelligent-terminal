// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AcpConnection.h"

#include <conpty-static.h>
#include <unicode.hpp>

#include "AcpConnection.g.cpp"

#include "../../types/inc/utils.hpp"

using namespace ::Microsoft::Console;

namespace WDJ = ::winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;

static void _acpLog(const std::wstring& msg)
{
    OutputDebugStringW(msg.c_str());
    wchar_t localAppData[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0)
        return;
    const auto logDir = std::wstring(localAppData) + L"\\AgenticTerminal\\logs";
    std::filesystem::create_directories(logDir);
    const auto logPath = logDir + L"\\wta-acp-connection.log";
    if (auto f = std::wofstream(logPath, std::ios::app))
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        f << L"[" << (now / 1000.0) << L"] " << msg;
    }
}

static constexpr int AGENT_TEXT_COLOR = 37; // white
static constexpr int TOOL_CALL_COLOR = 90; // gray
static constexpr int PLAN_COLOR = 96; // cyan
static constexpr int PERMISSION_COLOR = 93; // yellow
static constexpr int PROMPT_COLOR = 92; // green
static constexpr int ERROR_COLOR = 91; // red

static inline std::wstring _colorize(const unsigned int colorCode, const std::wstring_view text)
{
    return fmt::format(FMT_COMPILE(L"\x1b[{0}m{1}\x1b[m"), colorCode, text);
}

namespace winrt::Microsoft::Terminal::TerminalConnection::implementation
{
    void AcpConnection::_closePseudoConsole(HPCON hPC) noexcept
    {
        ::ConptyClosePseudoConsole(hPC);
    }

    void AcpConnection::Initialize(const Windows::Foundation::Collections::ValueSet& settings)
    {
        if (settings)
        {
            _agentCliPath = unbox_prop_or<winrt::hstring>(settings, L"agentCliPath", L"");
            _workingDirectory = unbox_prop_or<winrt::hstring>(settings, L"startingDirectory", L"");
            _initialPrompt = unbox_prop_or<winrt::hstring>(settings, L"initialPrompt", L"");
            _initialRows = unbox_prop_or<uint32_t>(settings, L"initialRows", _initialRows);
            _initialCols = unbox_prop_or<uint32_t>(settings, L"initialCols", _initialCols);
            _sessionId = unbox_prop_or<guid>(settings, L"sessionId", _sessionId);
        }

        if (_sessionId == guid{})
        {
            _sessionId = Utils::CreateGuid();
        }
    }

    // Helper: write a string with newline to the terminal
    void AcpConnection::_WriteStringWithNewline(const std::wstring_view str)
    {
        auto out = std::wstring{ str };
        out.append(L"\r\n");
        TerminalOutput.raise(winrt_wstring_to_array_view(out));
    }

    // Helper: write raw VT text to the terminal
    void AcpConnection::_WriteVt(const std::wstring_view str)
    {
        TerminalOutput.raise(winrt_wstring_to_array_view(str));
    }

    // Helper: write the prompt indicator
    void AcpConnection::_WritePromptIndicator()
    {
        _WriteVt(fmt::format(FMT_COMPILE(L"\r\n\x1b[{}m> \x1b[m"), PROMPT_COLOR));
    }

    // Method description:
    // - Start the ACP connection: launch agent subprocess, begin protocol handshake.
    void AcpConnection::Start()
    {
        _hOutputThread.reset(CreateThread(
            nullptr,
            0,
            [](LPVOID lpParameter) noexcept {
                const auto pInstance = static_cast<AcpConnection*>(lpParameter);
                if (pInstance)
                {
                    return pInstance->_OutputThread();
                }
                return gsl::narrow<DWORD>(E_INVALIDARG);
            },
            this,
            0,
            nullptr));

        THROW_LAST_ERROR_IF_NULL(_hOutputThread);

        LOG_IF_FAILED(SetThreadDescription(_hOutputThread.get(), L"AcpConnection Output Thread"));

        _transitionToState(ConnectionState::Connecting);
    }

    // Method description:
    // - Handles user keyboard input, following AzureConnection's line-buffering pattern.
    void AcpConnection::WriteInput(const winrt::array_view<const char16_t> buffer)
    {
        _writeInput(winrt_array_to_wstring_view(buffer));
    }

    void AcpConnection::_writeInput(const std::wstring_view data)
    {
        if (!_isStateOneOf(ConnectionState::Connected, ConnectionState::Connecting))
        {
            return;
        }

        // Handle Ctrl+C: cancel the active agent prompt
        if (data.size() > 0 && gsl::at(data, 0) == 0x03)
        {
            if (_agentStreaming.load())
            {
                _acpLog(L"[AcpConnection] Ctrl+C: cancelling agent prompt\n");
                try
                {
                    WDJ::JsonObject params;
                    params.SetNamedValue(L"sessionId", WDJ::JsonValue::CreateStringValue(_acpSessionId));
                    _SendJsonRpc(L"session/cancel", params, _nextRequestId++);
                }
                CATCH_LOG()
            }
            return;
        }

        std::lock_guard<std::mutex> lock{ _inputMutex };
        if (data.size() > 0 && (gsl::at(data, 0) == UNICODE_BACKSPACE || gsl::at(data, 0) == UNICODE_DEL))
        {
            if (_userInput.size() > 0)
            {
                _userInput.pop_back();
                TerminalOutput.raise(winrt_wstring_to_array_view(L"\x08 \x08"));
            }
        }
        else
        {
            TerminalOutput.raise(winrt_wstring_to_array_view(data)); // echo back

            switch (_currentInputMode)
            {
            case InputMode::Line:
                if (data.size() > 0 && gsl::at(data, 0) == UNICODE_CARRIAGERETURN)
                {
                    TerminalOutput.raise(winrt_wstring_to_array_view(L"\r\n"));
                    _currentInputMode = InputMode::None;
                    _inputEvent.notify_one();
                    break;
                }
                [[fallthrough]];
            default:
                std::copy(data.cbegin(), data.cend(), std::back_inserter(_userInput));
                break;
            }
        }
    }

    std::optional<std::wstring> AcpConnection::_ReadUserInput(InputMode mode)
    {
        std::unique_lock<std::mutex> inputLock{ _inputMutex };

        if (_isStateAtOrBeyond(ConnectionState::Closing))
        {
            return std::nullopt;
        }

        _currentInputMode = mode;

        _inputEvent.wait(inputLock, [this, mode]() {
            return _currentInputMode != mode || _isStateAtOrBeyond(ConnectionState::Closing);
        });

        if (_isStateAtOrBeyond(ConnectionState::Closing))
        {
            return std::nullopt;
        }

        std::wstring readInput{};
        _userInput.swap(readInput);
        return readInput;
    }

    void AcpConnection::Resize(uint32_t rows, uint32_t columns)
    {
        if (!_isConnected())
        {
            _initialRows = rows;
            _initialCols = columns;
        }
        // ACP doesn't have a resize method, but we track dimensions for terminal/create
    }

    void AcpConnection::Close() noexcept
    try
    {
        if (_transitionToState(ConnectionState::Closing))
        {
            _inputEvent.notify_all();

            // Terminate agent process if still running.
            // This breaks the pipe and unblocks the reader thread's ReadFile.
            if (_agentProcess.hProcess)
            {
                TerminateProcess(_agentProcess.hProcess, 0);
            }

            // Clean up managed terminals
            {
                std::lock_guard<std::mutex> lock{ _terminalsMutex };
                for (auto& [id, term] : _managedTerminals)
                {
                    if (term->process.hProcess)
                    {
                        TerminateProcess(term->process.hProcess, 0);
                    }
                    term->exitEvent.SetEvent();
                    if (term->hPC)
                    {
                        term->hPC.reset();
                    }
                }
                _managedTerminals.clear();
            }

            // Wait for the output (reader) thread to exit BEFORE closing pipes.
            // The reader thread will exit naturally once the pipe breaks from
            // the agent process being terminated above.
            if (_hOutputThread)
            {
                WaitForSingleObject(_hOutputThread.get(), 5000);
                _hOutputThread.reset();
            }

            // Now safe to close pipes - no threads are using them
            _pipeToAgent.reset();
            _pipeFromAgent.reset();

            // Reject any pending request promises so blocked threads unblock
            {
                std::lock_guard<std::mutex> lock{ _pendingMutex };
                for (auto& [reqId, req] : _pendingRequests)
                {
                    try
                    {
                        req.promise.set_exception(
                            std::make_exception_ptr(std::runtime_error("Connection closed")));
                    }
                    CATCH_LOG()
                }
                _pendingRequests.clear();
            }

            _agentProcess.reset();

            _transitionToState(ConnectionState::Closed);
        }
    }
    CATCH_LOG()

    // ======================================================================
    // Subprocess management
    // ======================================================================

    void AcpConnection::_LaunchAgentProcess()
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        // Create pipes for agent's stdin — RAII wrappers ensure cleanup on exception.
        HANDLE hStdinRead = nullptr, hStdinWrite = nullptr;
        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0));
        wil::unique_hfile stdinRead{ hStdinRead }, stdinWrite{ hStdinWrite };
        // Ensure our write end is not inherited
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stdinWrite.get(), HANDLE_FLAG_INHERIT, 0));

        // Create pipes for agent's stdout
        HANDLE hStdoutRead = nullptr, hStdoutWrite = nullptr;
        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0));
        wil::unique_hfile stdoutRead{ hStdoutRead }, stdoutWrite{ hStdoutWrite };
        // Ensure our read end is not inherited
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stdoutRead.get(), HANDLE_FLAG_INHERIT, 0));

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = stdinRead.get();
        si.hStdOutput = stdoutWrite.get();
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE); // let agent stderr go to our stderr for debugging

        auto cmdline = wil::ExpandEnvironmentStringsW<std::wstring>(_agentCliPath.c_str());
        _acpLog(fmt::format(FMT_COMPILE(L"[AcpConnection] Launching agent: {}\n"), cmdline));

        const auto startingDir = _workingDirectory.empty() ? nullptr : _workingDirectory.c_str();

        // Use the shell's environment if available, so the agent inherits the
        // The agent process inherits environment from the terminal process.
        DWORD creationFlags = CREATE_NO_WINDOW;
        LPVOID lpEnvironment = nullptr;

        THROW_IF_WIN32_BOOL_FALSE(CreateProcessW(
            nullptr,
            cmdline.data(),
            nullptr,
            nullptr,
            TRUE, // inherit handles
            creationFlags,
            lpEnvironment,
            startingDir,
            &si,
            &_agentProcess));

        // Child's ends are no longer needed — RAII handles close them.
        stdinRead.reset();
        stdoutWrite.reset();

        _pipeToAgent = std::move(stdinWrite);
        _pipeFromAgent = std::move(stdoutRead);

        _acpLog(fmt::format(FMT_COMPILE(L"[AcpConnection] Agent process launched, PID: {}\n"),
                            GetProcessId(_agentProcess.hProcess)));
    }

    // ======================================================================
    // JSON-RPC messaging
    // ======================================================================

    void AcpConnection::_SendJsonRpc(const winrt::hstring& method,
                                     const WDJ::JsonObject& params,
                                     std::optional<int64_t> id)
    {
        WDJ::JsonObject msg;
        msg.SetNamedValue(L"jsonrpc", WDJ::JsonValue::CreateStringValue(L"2.0"));
        msg.SetNamedValue(L"method", WDJ::JsonValue::CreateStringValue(method));
        if (params)
        {
            msg.SetNamedValue(L"params", params);
        }
        if (id.has_value())
        {
            msg.SetNamedValue(L"id", WDJ::JsonValue::CreateNumberValue(static_cast<double>(id.value())));
        }

        auto jsonStr = winrt::to_string(msg.Stringify());
        jsonStr.push_back('\n');

        _acpLog(fmt::format(FMT_COMPILE(L"[AcpConnection] SEND: {}\n"), winrt::to_hstring(jsonStr)));

        DWORD written;
        bool writeFailed;
        {
            std::lock_guard<std::mutex> lock{ _writeMutex };
            writeFailed = !WriteFile(_pipeToAgent.get(), jsonStr.data(), gsl::narrow<DWORD>(jsonStr.size()), &written, nullptr);
        }

        if (writeFailed)
        {
            // Pipe broken — reject all pending requests so callers don't hang forever.
            {
                std::lock_guard<std::mutex> pLock{ _pendingMutex };
                for (auto& [_, req] : _pendingRequests)
                {
                    try
                    {
                        req.promise.set_exception(
                            std::make_exception_ptr(std::runtime_error("Agent pipe closed")));
                    }
                    catch (...)
                    {
                    }
                }
                _pendingRequests.clear();
            }
            _transitionToState(ConnectionState::Failed);
        }
    }

    std::future<WDJ::JsonObject> AcpConnection::_SendRequest(const winrt::hstring& method,
                                                              const WDJ::JsonObject& params)
    {
        auto id = _nextRequestId++;

        PendingRequest req;
        auto future = req.promise.get_future();

        {
            std::lock_guard<std::mutex> lock{ _pendingMutex };
            _pendingRequests.emplace(id, std::move(req));
        }

        _SendJsonRpc(method, params, id);
        return future;
    }

    void AcpConnection::_SendResponse(int64_t id, const WDJ::JsonObject& result)
    {
        WDJ::JsonObject msg;
        msg.SetNamedValue(L"jsonrpc", WDJ::JsonValue::CreateStringValue(L"2.0"));
        msg.SetNamedValue(L"id", WDJ::JsonValue::CreateNumberValue(static_cast<double>(id)));
        msg.SetNamedValue(L"result", result);

        auto jsonStr = winrt::to_string(msg.Stringify());
        jsonStr.push_back('\n');

        _acpLog(fmt::format(FMT_COMPILE(L"[AcpConnection] SEND RESPONSE: {}\n"), winrt::to_hstring(jsonStr)));

        DWORD written;
        std::lock_guard<std::mutex> lock{ _writeMutex };
        WriteFile(_pipeToAgent.get(), jsonStr.data(), gsl::narrow<DWORD>(jsonStr.size()), &written, nullptr);
    }

    void AcpConnection::_SendErrorResponse(int64_t id, int code, const winrt::hstring& message)
    {
        WDJ::JsonObject msg;
        msg.SetNamedValue(L"jsonrpc", WDJ::JsonValue::CreateStringValue(L"2.0"));
        msg.SetNamedValue(L"id", WDJ::JsonValue::CreateNumberValue(static_cast<double>(id)));

        WDJ::JsonObject error;
        error.SetNamedValue(L"code", WDJ::JsonValue::CreateNumberValue(code));
        error.SetNamedValue(L"message", WDJ::JsonValue::CreateStringValue(message));
        msg.SetNamedValue(L"error", error);

        auto jsonStr = winrt::to_string(msg.Stringify());
        jsonStr.push_back('\n');

        DWORD written;
        std::lock_guard<std::mutex> lock{ _writeMutex };
        WriteFile(_pipeToAgent.get(), jsonStr.data(), gsl::narrow<DWORD>(jsonStr.size()), &written, nullptr);
    }

    // ======================================================================
    // Message routing
    // ======================================================================

    void AcpConnection::_RouteMessage(const WDJ::JsonObject& msg)
    {
        const auto hasMethod = msg.HasKey(L"method");
        const auto hasId = msg.HasKey(L"id");

        if (!hasMethod && hasId)
        {
            // This is a response to one of our requests.
            const auto id = static_cast<int64_t>(msg.GetNamedNumber(L"id"));

            std::lock_guard<std::mutex> lock{ _pendingMutex };
            auto it = _pendingRequests.find(id);
            if (it != _pendingRequests.end())
            {
                if (msg.HasKey(L"result"))
                {
                    it->second.promise.set_value(msg.GetNamedObject(L"result"));
                }
                else if (msg.HasKey(L"error"))
                {
                    auto errorObj = msg.GetNamedObject(L"error");
                    auto errorMsg = winrt::to_string(errorObj.GetNamedString(L"message"));
                    it->second.promise.set_exception(
                        std::make_exception_ptr(std::runtime_error(errorMsg)));
                }
                _pendingRequests.erase(it);
            }
        }
        else if (hasMethod && !hasId)
        {
            // Notification from agent (no response expected)
            auto method = msg.GetNamedString(L"method");
            auto params = msg.HasKey(L"params") ? msg.GetNamedObject(L"params") : WDJ::JsonObject{};
            _HandleNotification(method, params);
        }
        else if (hasMethod && hasId)
        {
            // Request from agent (we must respond)
            auto method = msg.GetNamedString(L"method");
            auto params = msg.HasKey(L"params") ? msg.GetNamedObject(L"params") : WDJ::JsonObject{};
            auto id = static_cast<int64_t>(msg.GetNamedNumber(L"id"));
            _HandleAgentRequest(method, params, id);
        }
    }

    void AcpConnection::_HandleNotification(const winrt::hstring& method, const WDJ::JsonObject& params)
    {
        _acpLog(fmt::format(FMT_COMPILE(L"[AcpConnection] NOTIFICATION: {}\n"), std::wstring_view{ method }));

        if (method == L"session/update")
        {
            // ACP format: params.update.sessionUpdate contains the update type
            // params.update contains the full update object (with content, etc.)
            auto update = params.GetNamedObject(L"update", WDJ::JsonObject{});
            auto type = update.GetNamedString(L"sessionUpdate", L"");

            if (type == L"agent_message_chunk")
            {
                _HandleAgentMessageChunk(update);
            }
            else if (type == L"tool_call")
            {
                _HandleToolCall(update);
            }
            else if (type == L"tool_call_update")
            {
                _HandleToolCallUpdate(update);
            }
            else if (type == L"plan")
            {
                _HandlePlan(update);
            }
            else
            {
                _acpLog(fmt::format(FMT_COMPILE(L"[AcpConnection] Unknown session/update type: {}\n"), std::wstring_view{ type }));
            }
        }
    }

    void AcpConnection::_HandleAgentRequest(const winrt::hstring& method, const WDJ::JsonObject& params, int64_t id)
    {
        _acpLog(fmt::format(FMT_COMPILE(L"[AcpConnection] AGENT REQUEST: {} (id={})\n"), std::wstring_view{ method }, id));

        if (method == L"terminal/create")
        {
            _HandleTerminalCreate(params, id);
        }
        else if (method == L"terminal/output")
        {
            _HandleTerminalOutput(params, id);
        }
        else if (method == L"terminal/wait_for_exit")
        {
            // Run on a separate thread to avoid blocking the reader loop.
            // The reader must stay free to process other messages while we wait.
            auto paramsCopy = params; // prevent ref invalidation
            auto strong = get_strong();
            std::thread([strong, paramsCopy, id]() {
                strong->_HandleTerminalWaitForExit(paramsCopy, id);
            }).detach();
        }
        else if (method == L"terminal/kill")
        {
            _HandleTerminalKill(params, id);
        }
        else if (method == L"terminal/release")
        {
            _HandleTerminalRelease(params, id);
        }
        else if (method == L"session/request_permission")
        {
            // Run on a separate thread to avoid blocking the reader loop
            // while waiting for user input.
            auto paramsCopy = params; // prevent ref invalidation
            auto strong = get_strong();
            std::thread([strong, paramsCopy, id]() {
                strong->_HandleRequestPermission(paramsCopy, id);
            }).detach();
        }
        else if (method == L"fs/read_text_file")
        {
            // Phase 5: file system operations - stub for now
            _SendErrorResponse(id, -32601, L"fs/read_text_file not yet implemented");
        }
        else if (method == L"fs/write_text_file")
        {
            _SendErrorResponse(id, -32601, L"fs/write_text_file not yet implemented");
        }
        else
        {
            _SendErrorResponse(id, -32601, winrt::hstring{ fmt::format(L"Method not found: {}", std::wstring_view{ method }) });
        }
    }

    // ======================================================================
    // Session update handlers
    // ======================================================================

    void AcpConnection::_HandleAgentMessageChunk(const WDJ::JsonObject& update)
    {
        // ACP format: update.content.text contains the text chunk
        auto content = update.GetNamedObject(L"content", WDJ::JsonObject{});
        auto text = content.GetNamedString(L"text", L"");

        if (text.empty())
        {
            return;
        }

        // Convert \n to \r\n for terminal display
        std::wstring output;
        output.reserve(text.size() * 2);
        for (auto ch : text)
        {
            if (ch == L'\n')
            {
                output += L"\r\n";
            }
            else
            {
                output += ch;
            }
        }
        _WriteVt(output);
    }

    void AcpConnection::_HandleToolCall(const WDJ::JsonObject& update)
    {
        // ACP format: title/status are directly on the update object (no content wrapper)
        auto title = update.GetNamedString(L"title", L"tool_call");
        auto status = update.GetNamedString(L"status", L"pending");

        _WriteVt(fmt::format(FMT_COMPILE(L"\r\n\x1b[{}m[{}] {}\x1b[m\r\n"), TOOL_CALL_COLOR, std::wstring_view{ title }, std::wstring_view{ status }));
    }

    void AcpConnection::_HandleToolCallUpdate(const WDJ::JsonObject& update)
    {
        // ACP format: title/status are directly on the update object (no content wrapper)
        auto title = update.GetNamedString(L"title", L"tool_call");
        auto status = update.GetNamedString(L"status", L"");

        if (!status.empty())
        {
            _WriteVt(fmt::format(FMT_COMPILE(L"\x1b[{}m[{}] {}\x1b[m\r\n"), TOOL_CALL_COLOR, std::wstring_view{ title }, std::wstring_view{ status }));
        }
    }

    void AcpConnection::_HandlePlan(const WDJ::JsonObject& update)
    {
        // ACP format: entries are directly on the update object (no content wrapper)
        if (update.HasKey(L"entries"))
        {
            auto entries = update.GetNamedArray(L"entries");
            _WriteVt(fmt::format(FMT_COMPILE(L"\r\n\x1b[{}mPlan:\x1b[m\r\n"), PLAN_COLOR));
            for (uint32_t i = 0; i < entries.Size(); i++)
            {
                auto entry = entries.GetObjectAt(i);
                auto entryContent = entry.GetNamedString(L"content", L"");
                auto status = entry.GetNamedString(L"status", L"pending");
                auto marker = (status == L"completed") ? L"[x]" : (status == L"in_progress") ? L"[>]" : L"[ ]";
                _WriteVt(fmt::format(FMT_COMPILE(L"\x1b[{}m  {} {}\x1b[m\r\n"), PLAN_COLOR, marker, std::wstring_view{ entryContent }));
            }
        }
    }

    // ======================================================================
    // Permission handling
    // ======================================================================

    void AcpConnection::_HandleRequestPermission(const WDJ::JsonObject& params, int64_t id)
    {
        // ACP format: params.toolCall.title for description, params.options for choices
        winrt::hstring description{ L"The agent is requesting permission to perform an action." };
        winrt::hstring allowOptionId;
        winrt::hstring rejectOptionId;

        if (params.HasKey(L"toolCall"))
        {
            auto toolCall = params.GetNamedObject(L"toolCall");
            auto title = toolCall.GetNamedString(L"title", L"");
            auto kind = toolCall.GetNamedString(L"kind", L"");
            if (!title.empty())
            {
                description = title;
            }
            if (!kind.empty())
            {
                description = winrt::hstring{ fmt::format(L"[{}] {}", std::wstring_view{ kind }, std::wstring_view{ description }) };
            }
        }

        // Parse options to find allow/reject option IDs
        if (params.HasKey(L"options"))
        {
            auto options = params.GetNamedArray(L"options");
            _WriteVt(fmt::format(FMT_COMPILE(L"\r\n\x1b[{}m[Permission Required]\x1b[m {}\r\n"), PERMISSION_COLOR, std::wstring_view{ description }));
            for (uint32_t i = 0; i < options.Size(); i++)
            {
                auto option = options.GetObjectAt(i);
                auto optionKind = option.GetNamedString(L"kind", L"");
                auto optionName = option.GetNamedString(L"name", L"");
                auto optionId = option.GetNamedString(L"optionId", L"");

                if (optionKind == L"allow_once" || optionKind == L"allow_always")
                {
                    if (allowOptionId.empty())
                    {
                        allowOptionId = optionId;
                    }
                }
                else if (optionKind == L"reject_once" || optionKind == L"reject_always")
                {
                    if (rejectOptionId.empty())
                    {
                        rejectOptionId = optionId;
                    }
                }

                _WriteVt(fmt::format(FMT_COMPILE(L"\x1b[{}m  [{}] {}\x1b[m\r\n"),
                                     PERMISSION_COLOR,
                                     std::wstring_view{ optionId },
                                     std::wstring_view{ optionName }));
            }
        }
        else
        {
            _WriteVt(fmt::format(FMT_COMPILE(L"\r\n\x1b[{}m[Permission Required]\x1b[m {}\r\n"), PERMISSION_COLOR, std::wstring_view{ description }));
        }

        _WriteVt(fmt::format(FMT_COMPILE(L"\x1b[{}mAllow? (y/n): \x1b[m"), PERMISSION_COLOR));

        auto input = _ReadUserInput(InputMode::Line);
        if (!input.has_value())
        {
            // Connection closing: respond with cancellation
            WDJ::JsonObject cancelResult;
            WDJ::JsonObject cancelOutcome;
            cancelOutcome.SetNamedValue(L"outcome", WDJ::JsonValue::CreateStringValue(L"cancelled"));
            cancelResult.SetNamedValue(L"outcome", cancelOutcome);
            _SendResponse(id, cancelResult);
            return;
        }

        bool allowed = !input->empty() && (input->at(0) == L'y' || input->at(0) == L'Y');

        // ACP format: respond with { outcome: { outcome: "selected", optionId: "..." } }
        WDJ::JsonObject result;
        WDJ::JsonObject outcome;
        if (allowed)
        {
            outcome.SetNamedValue(L"outcome", WDJ::JsonValue::CreateStringValue(L"selected"));
            outcome.SetNamedValue(L"optionId", WDJ::JsonValue::CreateStringValue(
                allowOptionId.empty() ? winrt::hstring{ L"allow" } : allowOptionId));
            _WriteStringWithNewline(_colorize(PROMPT_COLOR, L"Permission granted."));
        }
        else
        {
            outcome.SetNamedValue(L"outcome", WDJ::JsonValue::CreateStringValue(L"selected"));
            outcome.SetNamedValue(L"optionId", WDJ::JsonValue::CreateStringValue(
                rejectOptionId.empty() ? winrt::hstring{ L"reject" } : rejectOptionId));
            _WriteStringWithNewline(_colorize(ERROR_COLOR, L"Permission denied."));
        }
        result.SetNamedValue(L"outcome", outcome);
        _SendResponse(id, result);
    }

    // ======================================================================
    // terminal/create bridge (headless ConPTY)
    // ======================================================================

    DWORD WINAPI AcpConnection::_ManagedTerminalOutputThread(LPVOID lpParameter)
    {
        auto* term = static_cast<AcpManagedTerminal*>(lpParameter);
        char buffer[4096];
        DWORD read = 0;

        while (ReadFile(term->pipeRead.get(), buffer, sizeof(buffer), &read, nullptr) && read > 0)
        {
            std::lock_guard<std::mutex> lock{ term->outputMutex };
            term->outputBuffer.append(buffer, read);
        }

        // Process exited or pipe broken
        DWORD exitCode = 0;
        if (term->process.hProcess)
        {
            GetExitCodeProcess(term->process.hProcess, &exitCode);
        }
        term->exited = true;
        term->exitCode = exitCode;
        term->exitEvent.SetEvent();

        return 0;
    }

    void AcpConnection::_HandleTerminalCreate(const WDJ::JsonObject& params, int64_t id)
    {
        try
        {
            WDJ::JsonArray argsArray;
            if (params.HasKey(L"args"))
            {
                argsArray = params.GetNamedArray(L"args");
            }

            // Build full command line
            auto commandStr = params.GetNamedString(L"command", L"");
            std::wstring cmdline{ std::wstring_view{ commandStr } };
            for (uint32_t i = 0; i < argsArray.Size(); i++)
            {
                cmdline += L" ";
                auto arg = argsArray.GetStringAt(i);
                cmdline += std::wstring_view{ arg };
            }

            winrt::hstring wCwd = params.GetNamedString(L"cwd", L"");
            if (wCwd.empty())
            {
                wCwd = _workingDirectory;
            }

            // Echo to the ACP pane so user sees what's running
            _WriteVt(fmt::format(FMT_COMPILE(L"\r\n\x1b[{}m--- Running: {} ---\x1b[m\r\n"), TOOL_CALL_COLOR, cmdline));

            auto term = std::make_unique<AcpManagedTerminal>();
            term->id = fmt::format("term_{}", _nextTerminalId++);

            // Create ConPTY for the managed terminal
            auto pipe = Utils::CreateOverlappedPipe(PIPE_ACCESS_DUPLEX, 64 * 1024);

            COORD size = { 80, 24 };
            HPCON hPC = nullptr;
            THROW_IF_FAILED(ConptyCreatePseudoConsole(size, pipe.client.get(), pipe.client.get(), 0, &hPC));
            term->hPC.reset(hPC);

            // Set up process launch
            STARTUPINFOEX siEx{};
            siEx.StartupInfo.cb = sizeof(STARTUPINFOEX);
            SIZE_T attrSize = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
            auto attrList = std::make_unique<std::byte[]>(attrSize);
            siEx.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attrList.get());
            THROW_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrSize));
            THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(
                siEx.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                hPC, sizeof(HPCON), nullptr, nullptr));

            auto mutableCmdline = std::wstring{ cmdline };
            const auto startDir = wCwd.empty() ? nullptr : wCwd.c_str();

            // Use the shell's environment for managed terminals so commands
            // The managed terminal inherits environment from the terminal process.
            DWORD termCreationFlags = EXTENDED_STARTUPINFO_PRESENT;
            LPVOID lpTermEnv = nullptr;

            THROW_IF_WIN32_BOOL_FALSE(CreateProcessW(
                nullptr,
                mutableCmdline.data(),
                nullptr, nullptr,
                FALSE,
                termCreationFlags,
                lpTermEnv,
                startDir,
                &siEx.StartupInfo,
                &term->process));

            DeleteProcThreadAttributeList(siEx.lpAttributeList);

            term->pipeRead = std::move(pipe.server);

            // Start output capture thread
            term->outputThread.reset(CreateThread(nullptr, 0, _ManagedTerminalOutputThread, term.get(), 0, nullptr));

            auto termId = term->id;

            {
                std::lock_guard<std::mutex> lock{ _terminalsMutex };
                _managedTerminals.emplace(termId, std::move(term));
            }

            WDJ::JsonObject result;
            result.SetNamedValue(L"terminalId", WDJ::JsonValue::CreateStringValue(winrt::to_hstring(termId)));
            _SendResponse(id, result);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            _SendErrorResponse(id, -32000, L"Failed to create terminal");
        }
    }

    void AcpConnection::_HandleTerminalOutput(const WDJ::JsonObject& params, int64_t id)
    {
        auto termId = winrt::to_string(params.GetNamedString(L"terminalId", L""));

        std::lock_guard<std::mutex> lock{ _terminalsMutex };
        auto it = _managedTerminals.find(termId);
        if (it == _managedTerminals.end())
        {
            _SendErrorResponse(id, -32000, L"Terminal not found");
            return;
        }

        auto& term = it->second;
        std::string output;
        {
            std::lock_guard<std::mutex> outputLock{ term->outputMutex };
            output.swap(term->outputBuffer);
        }

        WDJ::JsonObject result;
        result.SetNamedValue(L"output", WDJ::JsonValue::CreateStringValue(winrt::to_hstring(output)));
        result.SetNamedValue(L"truncated", WDJ::JsonValue::CreateBooleanValue(false));

        if (term->exited)
        {
            WDJ::JsonObject exitStatus;
            exitStatus.SetNamedValue(L"exitCode", WDJ::JsonValue::CreateNumberValue(term->exitCode));
            result.SetNamedValue(L"exitStatus", exitStatus);
        }

        _SendResponse(id, result);
    }

    void AcpConnection::_HandleTerminalWaitForExit(const WDJ::JsonObject& params, int64_t id)
    {
        auto termId = winrt::to_string(params.GetNamedString(L"terminalId", L""));

        AcpManagedTerminal* termPtr = nullptr;
        {
            std::lock_guard<std::mutex> lock{ _terminalsMutex };
            auto it = _managedTerminals.find(termId);
            if (it == _managedTerminals.end())
            {
                _SendErrorResponse(id, -32000, L"Terminal not found");
                return;
            }
            termPtr = it->second.get();
        }

        // Wait for process exit (with timeout to check connection state)
        while (!termPtr->exited && !_isStateAtOrBeyond(ConnectionState::Closing))
        {
            termPtr->exitEvent.wait(1000);
        }

        if (_isStateAtOrBeyond(ConnectionState::Closing))
        {
            _SendErrorResponse(id, -32000, L"Connection closing");
            return;
        }

        // Echo exit status to ACP pane
        _WriteVt(fmt::format(FMT_COMPILE(L"\x1b[{}m--- Exit: {} ---\x1b[m\r\n"), TOOL_CALL_COLOR, termPtr->exitCode));

        WDJ::JsonObject result;
        result.SetNamedValue(L"exitCode", WDJ::JsonValue::CreateNumberValue(termPtr->exitCode));
        _SendResponse(id, result);
    }

    void AcpConnection::_HandleTerminalKill(const WDJ::JsonObject& params, int64_t id)
    {
        auto termId = winrt::to_string(params.GetNamedString(L"terminalId", L""));

        std::lock_guard<std::mutex> lock{ _terminalsMutex };
        auto it = _managedTerminals.find(termId);
        if (it == _managedTerminals.end())
        {
            _SendErrorResponse(id, -32000, L"Terminal not found");
            return;
        }

        auto& term = it->second;
        if (term->process.hProcess && !term->exited)
        {
            TerminateProcess(term->process.hProcess, 1);
        }

        _SendResponse(id, WDJ::JsonObject{});
    }

    void AcpConnection::_HandleTerminalRelease(const WDJ::JsonObject& params, int64_t id)
    {
        auto termId = winrt::to_string(params.GetNamedString(L"terminalId", L""));

        std::lock_guard<std::mutex> lock{ _terminalsMutex };
        auto it = _managedTerminals.find(termId);
        if (it == _managedTerminals.end())
        {
            _SendErrorResponse(id, -32000, L"Terminal not found");
            return;
        }

        auto& term = it->second;
        if (term->process.hProcess && !term->exited)
        {
            TerminateProcess(term->process.hProcess, 1);
        }
        if (term->outputThread)
        {
            WaitForSingleObject(term->outputThread.get(), 5000);
        }

        _managedTerminals.erase(it);
        _SendResponse(id, WDJ::JsonObject{});
    }

    // ======================================================================
    // Protocol handshake
    // ======================================================================

    void AcpConnection::_DoHandshake()
    {
        // 1. Send initialize
        WDJ::JsonObject initParams;
        initParams.SetNamedValue(L"protocolVersion", WDJ::JsonValue::CreateNumberValue(1));

        WDJ::JsonObject clientInfo;
        clientInfo.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(L"Windows Terminal"));
        clientInfo.SetNamedValue(L"version", WDJ::JsonValue::CreateStringValue(L"1.0"));
        initParams.SetNamedValue(L"clientInfo", clientInfo);

        WDJ::JsonObject clientCaps;
        clientCaps.SetNamedValue(L"terminal", WDJ::JsonValue::CreateBooleanValue(true));
        initParams.SetNamedValue(L"clientCapabilities", clientCaps);

        auto initFuture = _SendRequest(L"initialize", initParams);

        _acpLog(L"[AcpConnection] Waiting for initialize response...\n");
        auto initResult = initFuture.get();
        _acpLog(L"[AcpConnection] Got initialize response\n");

        // 2. Send session/new
        WDJ::JsonObject sessionParams;
        if (!_workingDirectory.empty())
        {
            sessionParams.SetNamedValue(L"cwd", WDJ::JsonValue::CreateStringValue(_workingDirectory));
        }
        sessionParams.SetNamedValue(L"protocolServers", WDJ::JsonArray{});

        auto sessionFuture = _SendRequest(L"session/new", sessionParams);

        _acpLog(L"[AcpConnection] Waiting for session/new response...\n");
        auto sessionResult = sessionFuture.get();

        _acpSessionId = sessionResult.GetNamedString(L"sessionId", L"");
        _acpLog(fmt::format(FMT_COMPILE(L"[AcpConnection] Session established: {}\n"), std::wstring_view{ _acpSessionId }));
    }

    // ======================================================================
    // Prompt loop
    // ======================================================================

    void AcpConnection::_PromptLoop()
    {
        // If we have an initial prompt, send it immediately
        if (!_initialPrompt.empty())
        {
            auto prompt = std::wstring{ _initialPrompt };
            _initialPrompt = L"";

            _WriteStringWithNewline(fmt::format(FMT_COMPILE(L"\x1b[{}m> {}\x1b[m"), PROMPT_COLOR, prompt));

            _agentStreaming.store(true);

            WDJ::JsonObject promptParams;
            promptParams.SetNamedValue(L"sessionId", WDJ::JsonValue::CreateStringValue(_acpSessionId));

            // ACP prompt format: array of content objects [{type:"text", text:"..."}]
            WDJ::JsonArray promptArray;
            WDJ::JsonObject promptContent;
            promptContent.SetNamedValue(L"type", WDJ::JsonValue::CreateStringValue(L"text"));
            promptContent.SetNamedValue(L"text", WDJ::JsonValue::CreateStringValue(winrt::hstring{ prompt }));
            promptArray.Append(promptContent);
            promptParams.SetNamedValue(L"prompt", promptArray);

            auto future = _SendRequest(L"session/prompt", promptParams);

            try
            {
                auto result = future.get();
            }
            catch (const std::exception& e)
            {
                _WriteStringWithNewline(_colorize(ERROR_COLOR, til::u8u16(e.what())));
            }

            _agentStreaming.store(false);
        }

        // Interactive prompt loop
        while (!_isStateAtOrBeyond(ConnectionState::Closing))
        {
            _WritePromptIndicator();

            auto input = _ReadUserInput(InputMode::Line);
            if (!input.has_value() || _isStateAtOrBeyond(ConnectionState::Closing))
            {
                break;
            }

            auto& prompt = input.value();
            if (prompt.empty())
            {
                continue;
            }

            _agentStreaming.store(true);

            WDJ::JsonObject promptParams;
            promptParams.SetNamedValue(L"sessionId", WDJ::JsonValue::CreateStringValue(_acpSessionId));

            // ACP prompt format: array of content objects [{type:"text", text:"..."}]
            WDJ::JsonArray promptArray;
            WDJ::JsonObject promptContent;
            promptContent.SetNamedValue(L"type", WDJ::JsonValue::CreateStringValue(L"text"));
            promptContent.SetNamedValue(L"text", WDJ::JsonValue::CreateStringValue(winrt::hstring{ prompt }));
            promptArray.Append(promptContent);
            promptParams.SetNamedValue(L"prompt", promptArray);

            auto future = _SendRequest(L"session/prompt", promptParams);

            try
            {
                auto result = future.get();
                // stopReason might be in the result
                auto stopReason = result.GetNamedString(L"stopReason", L"");
                if (!stopReason.empty())
                {
                    _acpLog(fmt::format(FMT_COMPILE(L"[AcpConnection] stopReason: {}\n"), stopReason.c_str()));
                }
            }
            catch (const std::exception& e)
            {
                _WriteStringWithNewline(_colorize(ERROR_COLOR, til::u8u16(e.what())));
            }

            _agentStreaming.store(false);
        }
    }

    // ======================================================================
    // Output thread (main background thread)
    // ======================================================================

    DWORD AcpConnection::_OutputThread()
    {
        try
        {
            _WriteStringWithNewline(_colorize(PROMPT_COLOR, L"Connecting to agent..."));

            // 1. Launch agent subprocess
            _LaunchAgentProcess();

            // 2. Start a reader thread that processes incoming JSON-RPC messages.
            //    We run the reader inline here and do the handshake/prompt on a
            //    separate thread, OR we can do handshake on this thread while
            //    also reading. The simplest approach: reader is this thread,
            //    handshake/prompt is on a separate thread that uses futures.

            // Start handshake + prompt loop on a background thread.
            // Capture a strong ref to prevent use-after-free if Close() races.
            auto strong = get_strong();
            auto handshakeThread = std::thread([strong]() {
                try
                {
                    strong->_DoHandshake();

                    strong->_transitionToState(ConnectionState::Connected);
                    strong->_WriteStringWithNewline(_colorize(PROMPT_COLOR, L"Connected to agent."));

                    strong->_PromptLoop();
                }
                catch (const std::exception& e)
                {
                    strong->_WriteStringWithNewline(_colorize(ERROR_COLOR, fmt::format(L"Agent error: {}", til::u8u16(e.what()))));
                    strong->_transitionToState(ConnectionState::Failed);
                }
                catch (...)
                {
                    LOG_CAUGHT_EXCEPTION();
                    strong->_WriteStringWithNewline(_colorize(ERROR_COLOR, L"Agent connection failed unexpectedly."));
                    strong->_transitionToState(ConnectionState::Failed);
                }
            });
            handshakeThread.detach();

            // 3. Reader loop: read from agent's stdout, parse JSON-RPC, route messages
            std::string lineBuffer;
            char readBuf[8192];
            DWORD bytesRead = 0;

            while (ReadFile(_pipeFromAgent.get(), readBuf, sizeof(readBuf), &bytesRead, nullptr) && bytesRead > 0)
            {
                if (_isStateAtOrBeyond(ConnectionState::Closing))
                {
                    break;
                }

                lineBuffer.append(readBuf, bytesRead);

                // Process complete lines (newline-delimited JSON-RPC)
                size_t pos = 0;
                while ((pos = lineBuffer.find('\n')) != std::string::npos)
                {
                    auto line = lineBuffer.substr(0, pos);
                    lineBuffer.erase(0, pos + 1);

                    // Skip empty lines
                    if (line.empty() || (line.size() == 1 && line[0] == '\r'))
                    {
                        continue;
                    }

                    // Trim trailing \r
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }

                    _acpLog(fmt::format(FMT_COMPILE(L"[AcpConnection] RECV: {}\n"), winrt::to_hstring(line)));

                    try
                    {
                        auto json = WDJ::JsonObject::Parse(winrt::to_hstring(line));
                        _RouteMessage(json);
                    }
                    catch (...)
                    {
                        _acpLog(fmt::format(FMT_COMPILE(L"[AcpConnection] Failed to parse JSON: {}\n"), winrt::to_hstring(line)));
                    }
                }
            }

            _acpLog(L"[AcpConnection] Reader thread exiting (pipe closed or EOF)\n");

            // Reject any pending promises so the handshake/prompt thread unblocks
            {
                std::lock_guard<std::mutex> lock{ _pendingMutex };
                for (auto& [reqId, req] : _pendingRequests)
                {
                    try
                    {
                        req.promise.set_exception(
                            std::make_exception_ptr(std::runtime_error("Agent pipe closed")));
                    }
                    CATCH_LOG()
                }
                _pendingRequests.clear();
            }

            // Wake up any input waiters
            _inputEvent.notify_all();

            // If we got here, the agent process has exited or the pipe broke
            if (!_isStateAtOrBeyond(ConnectionState::Closing))
            {
                DWORD exitCode = 0;
                if (_agentProcess.hProcess)
                {
                    GetExitCodeProcess(_agentProcess.hProcess, &exitCode);
                }
                _WriteStringWithNewline(fmt::format(FMT_COMPILE(L"\r\n\x1b[{}mAgent process exited (code: {})\x1b[m"),
                                                    ERROR_COLOR, exitCode));
                _transitionToState(ConnectionState::Failed);
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            _WriteStringWithNewline(_colorize(ERROR_COLOR, L"Failed to launch agent process."));
            _transitionToState(ConnectionState::Failed);
        }

        return 0;
    }

}
