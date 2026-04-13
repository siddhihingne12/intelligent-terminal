// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "Channel.h"
#include "ComChannel.h"
#include "Formatting.h"

#include <Windows.h>
#include <objbase.h>
#include <wil/com.h>

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>

// ── Helpers ──

static std::wstring GetActivePaneId(Channel& ch)
{
    PROTOCOL_PANE_INFO info{};
    if (SUCCEEDED(ch.GetActivePane(info)))
    {
        auto id = BstrToWstring(info.PaneId);
        FreePaneInfo(info);
        return id;
    }
    return {};
}

static std::wstring GetFirstWindowId(Channel& ch)
{
    std::vector<PROTOCOL_WINDOW_INFO> windows;
    if (SUCCEEDED(ch.ListWindows(windows)) && !windows.empty())
    {
        auto id = BstrToWstring(windows[0].WindowId);
        for (auto& w : windows)
            FreeWindowInfo(w);
        return id;
    }
    return {};
}

static std::wstring GetFirstTabId(Channel& ch, const std::wstring& windowId)
{
    std::vector<PROTOCOL_TAB_INFO> tabs;
    if (SUCCEEDED(ch.ListTabs(windowId, tabs)) && !tabs.empty())
    {
        auto id = BstrToWstring(tabs[0].TabId);
        for (auto& t : tabs)
            FreeTabInfo(t);
        return id;
    }
    return {};
}

static std::wstring ResolvePaneId(Channel& ch, const std::string& target)
{
    if (!target.empty())
        return Utf8ToWide(target);
    return GetActivePaneId(ch);
}

// Translate tmux-style key names to actual characters.
static std::wstring TranslateKeys(const std::vector<std::string>& keys)
{
    std::wstring result;
    for (const auto& key : keys)
    {
        if (key == "Enter" || key == "enter")
            result += L"\r\n";
        else if (key == "Space" || key == "space")
            result += L" ";
        else if (key == "Tab" || key == "tab")
            result += L"\t";
        else if (key == "Escape" || key == "escape" || key == "Esc")
            result += L"\x1b";
        else if (key == "BSpace" || key == "bspace")
            result += L"\b";
        else if (key == "C-c")
            result += L"\x03";
        else if (key == "C-d")
            result += L"\x04";
        else if (key == "C-z")
            result += L"\x1a";
        else if (key == "C-l")
            result += L"\x0c";
        else if (key.size() == 3 && key[0] == 'C' && key[1] == '-' && key[2] >= 'a' && key[2] <= 'z')
            result += static_cast<wchar_t>(key[2] - 'a' + 1);
        else
            result += Utf8ToWide(key);
    }
    return result;
}

// ── Main ──

int main()
{
    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);

    CLI::App app{ "wtcli — Windows Terminal CLI" };
    app.require_subcommand(0, 1);

    // Global options
    bool jsonMode = false;
    int exitCode = 0;
    app.add_flag("--json", jsonMode, "Output raw JSON");

    // Helper: connect to Windows Terminal via COM
    auto connect = [&]() -> std::unique_ptr<Channel> {
        auto ch = Channel::Connect();
        if (!ch)
            exitCode = 1;
        return ch;
    };

    // ── list-windows ──
    auto* listWindowsCmd = app.add_subcommand("list-windows", "List all windows")->alias("lsw");
    listWindowsCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;
        std::vector<PROTOCOL_WINDOW_INFO> windows;
        if (FAILED(ch->ListWindows(windows))) { fprintf(stderr, "ListWindows failed\n"); exitCode = 1; return; }
        if (jsonMode)
        {
            Json::Value arr(Json::objectValue);
            Json::Value list(Json::arrayValue);
            for (const auto& w : windows) list.append(WindowInfoToJson(w));
            arr["windows"] = list;
            PrintJson(arr);
        }
        else
        {
            FormatWindowsHuman(windows);
        }
        for (auto& w : windows) FreeWindowInfo(w);
    });

    // ── list-tabs ──
    std::string listTabsWindowId;
    auto* listTabsCmd = app.add_subcommand("list-tabs", "List tabs in a window")->alias("lst");
    listTabsCmd->add_option("-w,--window-id", listTabsWindowId, "Window ID");
    listTabsCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;
        auto wid = listTabsWindowId.empty() ? GetFirstWindowId(*ch) : Utf8ToWide(listTabsWindowId);
        std::vector<PROTOCOL_TAB_INFO> tabs;
        if (FAILED(ch->ListTabs(wid, tabs))) { fprintf(stderr, "ListTabs failed\n"); exitCode = 1; return; }
        if (jsonMode)
        {
            Json::Value arr(Json::objectValue);
            Json::Value list(Json::arrayValue);
            for (const auto& t : tabs) list.append(TabInfoToJson(t));
            arr["tabs"] = list;
            PrintJson(arr);
        }
        else
        {
            FormatTabsHuman(tabs);
        }
        for (auto& t : tabs) FreeTabInfo(t);
    });

    // ── list-panes ──
    std::string listPanesTabId, listPanesWindowId;
    auto* listPanesCmd = app.add_subcommand("list-panes", "List panes in a tab")->alias("lsp");
    listPanesCmd->add_option("-t,--tab-id", listPanesTabId, "Tab ID");
    listPanesCmd->add_option("-w,--window-id", listPanesWindowId, "Window ID");
    listPanesCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;
        auto wid = Utf8ToWide(listPanesWindowId);
        auto tid = Utf8ToWide(listPanesTabId);
        if (tid.empty())
        {
            if (wid.empty()) wid = GetFirstWindowId(*ch);
            tid = GetFirstTabId(*ch, wid);
        }
        std::vector<PROTOCOL_PANE_INFO> panes;
        if (FAILED(ch->ListPanes(wid, tid, panes))) { fprintf(stderr, "ListPanes failed\n"); exitCode = 1; return; }
        if (jsonMode)
        {
            Json::Value arr(Json::objectValue);
            Json::Value list(Json::arrayValue);
            for (const auto& p : panes) list.append(PaneInfoToJson(p));
            arr["panes"] = list;
            PrintJson(arr);
        }
        else
        {
            FormatPanesHuman(panes);
        }
        for (auto& p : panes) FreePaneInfo(p);
    });

    // ── active-pane ──
    auto* activePaneCmd = app.add_subcommand("active-pane", "Show the currently active pane");
    activePaneCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;
        PROTOCOL_PANE_INFO info{};
        if (FAILED(ch->GetActivePane(info))) { fprintf(stderr, "GetActivePane failed\n"); exitCode = 1; return; }
        if (jsonMode)
            PrintJson(PaneInfoToJson(info));
        else
            FormatActivePaneHuman(info);
        FreePaneInfo(info);
    });

    // ── capture-pane ──
    std::string capturePaneTarget;
    int captureMaxLines = 200;
    auto* capturePaneCmd = app.add_subcommand("capture-pane", "Capture pane output")->alias("capturep");
    capturePaneCmd->add_option("-t,--target", capturePaneTarget, "Pane ID");
    capturePaneCmd->add_option("-l,--max-lines", captureMaxLines, "Max lines");
    capturePaneCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;
        auto paneId = ResolvePaneId(*ch, capturePaneTarget);
        PROTOCOL_PANE_OUTPUT output{};
        if (FAILED(ch->ReadPaneOutput(paneId, L"scrollback", captureMaxLines, output)))
        {
            fprintf(stderr, "ReadPaneOutput failed\n");
            exitCode = 1;
            return;
        }
        if (jsonMode)
        {
            PrintJson(PaneOutputToJson(output));
        }
        else
        {
            auto content = WideToUtf8(BstrToWstring(output.Content));
            printf("%s\n", content.c_str());
        }
        FreePaneOutput(output);
    });

    // ── pane-status ──
    std::string paneStatusTarget;
    auto* paneStatusCmd = app.add_subcommand("pane-status", "Show pane process status");
    paneStatusCmd->add_option("-t,--target", paneStatusTarget, "Pane ID");
    paneStatusCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;
        auto paneId = ResolvePaneId(*ch, paneStatusTarget);
        PROTOCOL_PROCESS_STATUS status{};
        if (FAILED(ch->GetProcessStatus(paneId, status))) { fprintf(stderr, "GetProcessStatus failed\n"); exitCode = 1; return; }
        if (jsonMode)
        {
            Json::Value v;
            v["pane_id"] = WideToUtf8(BstrToWstring(status.PaneId));
            v["state"] = WideToUtf8(BstrToWstring(status.State));
            v["pid"] = static_cast<Json::UInt>(status.Pid);
            if (status.HasExitCode) v["exit_code"] = status.ExitCode;
            PrintJson(v);
        }
        else
        {
            FormatPaneStatusHuman(status);
        }
        FreeProcessStatus(status);
    });

    // ── send-keys ──
    std::string sendKeysTarget;
    std::vector<std::string> sendKeysArgs;
    auto* sendKeysCmd = app.add_subcommand("send-keys", "Send keys to a pane")->alias("send");
    sendKeysCmd->add_option("-t,--target", sendKeysTarget, "Pane ID");
    sendKeysCmd->add_option("keys", sendKeysArgs, "Keys to send")->required();
    sendKeysCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;
        auto paneId = ResolvePaneId(*ch, sendKeysTarget);
        auto text = TranslateKeys(sendKeysArgs);
        if (FAILED(ch->SendInput(paneId, text))) { fprintf(stderr, "SendInput failed\n"); exitCode = 1; return; }
    });

    // ── new-tab ──
    std::string newTabCommand, newTabTitle;
    auto* newTabCmd = app.add_subcommand("new-tab", "Create a new tab")->alias("neww");
    newTabCmd->add_option("-c,--command", newTabCommand, "Command to run");
    newTabCmd->add_option("-n,--title", newTabTitle, "Tab title");
    newTabCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;
        PROTOCOL_TAB_CREATION_RESULT result{};
        if (FAILED(ch->CreateTab(L"", L"", Utf8ToWide(newTabCommand), Utf8ToWide(newTabTitle),
                                  false, false, true, result)))
        {
            fprintf(stderr, "CreateTab failed\n");
            exitCode = 1;
            return;
        }
        if (jsonMode)
            PrintJson(CreationResultToJson(result));
        else
            FormatCreatedTabHuman(result);
        FreeTabCreationResult(result);
    });

    // ── split-pane ──
    std::string splitPaneTarget, splitPaneCommand;
    bool splitHorizontal = false, splitVertical = false;
    double splitSize = 0.5;
    auto* splitPaneCmd = app.add_subcommand("split-pane", "Split a pane")->alias("splitw");
    splitPaneCmd->add_option("-t,--target", splitPaneTarget, "Pane ID");
    splitPaneCmd->add_flag("-H,--horizontal", splitHorizontal, "Split horizontally");
    splitPaneCmd->add_flag("-v,--vertical", splitVertical, "Split vertically");
    splitPaneCmd->add_option("-s,--size", splitSize, "Size fraction");
    splitPaneCmd->add_option("-c,--command", splitPaneCommand, "Command to run");
    splitPaneCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;
        auto paneId = ResolvePaneId(*ch, splitPaneTarget);
        std::wstring dir = splitHorizontal ? L"horizontal" : (splitVertical ? L"vertical" : L"automatic");
        PROTOCOL_TAB_CREATION_RESULT result{};
        if (FAILED(ch->SplitPane(paneId, dir, static_cast<float>(splitSize),
                                  L"", Utf8ToWide(splitPaneCommand), false, true, result)))
        {
            fprintf(stderr, "SplitPane failed\n");
            exitCode = 1;
            return;
        }
        if (jsonMode)
            PrintJson(CreationResultToJson(result));
        else
            FormatCreatedPaneHuman(result);
        FreeTabCreationResult(result);
    });

    // ── kill-pane ──
    std::string killPaneTarget;
    auto* killPaneCmd = app.add_subcommand("kill-pane", "Close a pane")->alias("killp");
    killPaneCmd->add_option("-t,--target", killPaneTarget, "Pane ID");
    killPaneCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;
        auto paneId = ResolvePaneId(*ch, killPaneTarget);
        if (FAILED(ch->ClosePane(paneId))) { fprintf(stderr, "ClosePane failed\n"); exitCode = 1; return; }
        if (!jsonMode) printf("Pane %s closed.\n", WideToUtf8(paneId).c_str());
    });

    // ── test-pipe ──
    auto* testPipeCmd = app.add_subcommand("test-pipe", "Test connection to Windows Terminal");
    testPipeCmd->callback([&]() {
        printf("Connecting to Windows Terminal...\n");
        auto ch = connect();
        if (!ch) { fprintf(stderr, "Connection failed.\n"); return; }
        printf("Connected and authenticated!\n\n");

        std::vector<PROTOCOL_WINDOW_INFO> windows;
        if (SUCCEEDED(ch->ListWindows(windows)))
        {
            Json::Value arr(Json::objectValue);
            Json::Value list(Json::arrayValue);
            for (const auto& w : windows) list.append(WindowInfoToJson(w));
            arr["windows"] = list;
            printf("list_windows:\n");
            PrintJson(arr);
            for (auto& w : windows) FreeWindowInfo(w);
        }

        printf("\n");

        std::wstring version, methods;
        if (SUCCEEDED(ch->GetCapabilities(version, methods)))
        {
            Json::Value cap;
            cap["protocol_version"] = WideToUtf8(version);
            Json::CharReaderBuilder rb;
            std::string errs;
            std::istringstream ss(WideToUtf8(methods));
            Json::Value methodList;
            Json::parseFromStream(rb, ss, &methodList, &errs);
            cap["methods"] = methodList;
            printf("get_capabilities:\n");
            PrintJson(cap);
        }
    });

    // ── info ──
    auto* infoCmd = app.add_subcommand("info", "Show connection info");
    infoCmd->callback([&]() {
        printf("Windows Terminal Protocol Info\n");
        printf("========================================\n");

        wchar_t clsid[128]{};
        if (GetEnvironmentVariableW(L"WT_COM_CLSID", clsid, ARRAYSIZE(clsid)))
            printf("  COM CLSID: %ls\n", clsid);
        else
            printf("  COM CLSID: (not set)\n");

        wchar_t pipeName[256]{};
        if (GetEnvironmentVariableW(L"WT_PIPE_NAME", pipeName, ARRAYSIZE(pipeName)))
            printf("  Pipe:      %ls\n", pipeName);
        else
            printf("  Pipe:      (not set)\n");

        wchar_t token[256]{};
        if (GetEnvironmentVariableW(L"WT_MCP_TOKEN", token, ARRAYSIZE(token)))
            printf("  Token:     (set)\n");
        else
            printf("  Token:     (dev bypass)\n");

        printf("\n");

        auto ch = connect();
        if (!ch)
        {
            printf("  Connection: FAILED\n");
            return;
        }
        printf("  Connection: OK\n\n");

        std::wstring version, methods;
        if (SUCCEEDED(ch->GetCapabilities(version, methods)))
            printf("  Protocol:  %s\n", WideToUtf8(version).c_str());
    });

    // ── wait-for ──
    std::string waitForTarget;
    int waitInterval = 500;
    int waitTimeout = 0;
    auto* waitForCmd = app.add_subcommand("wait-for", "Wait for a pane to exit");
    waitForCmd->add_option("-t,--target", waitForTarget, "Pane ID")->required();
    waitForCmd->add_option("--interval", waitInterval, "Poll interval (ms)");
    waitForCmd->add_option("--timeout", waitTimeout, "Timeout (seconds, 0=forever)");
    waitForCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;
        auto paneId = Utf8ToWide(waitForTarget);
        auto start = std::chrono::steady_clock::now();

        while (true)
        {
            PROTOCOL_PROCESS_STATUS status{};
            if (FAILED(ch->GetProcessStatus(paneId, status))) { fprintf(stderr, "GetProcessStatus failed\n"); exitCode = 1; return; }

            auto state = WideToUtf8(BstrToWstring(status.State));
            if (state == "exited")
            {
                if (jsonMode)
                {
                    Json::Value v;
                    v["state"] = state;
                    v["exit_code"] = status.ExitCode;
                    PrintJson(v);
                }
                else
                {
                    printf("Process exited");
                    if (status.HasExitCode)
                        printf(" (code %d)", status.ExitCode);
                    printf("\n");
                }
                FreeProcessStatus(status);
                return;
            }
            FreeProcessStatus(status);

            if (waitTimeout > 0)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed >= waitTimeout)
                {
                    fprintf(stderr, "Timeout waiting for pane %s\n", waitForTarget.c_str());
                    exitCode = 1;
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(waitInterval));
        }
    });

    // ── pipe-id ──
    auto* pipeIdCmd = app.add_subcommand("pipe-id", "Show pipe name");
    pipeIdCmd->callback([&]() {
        wchar_t pipeName[256]{};
        if (GetEnvironmentVariableW(L"WT_PIPE_NAME", pipeName, ARRAYSIZE(pipeName)))
            printf("%ls\n", pipeName);
        else
        {
            fprintf(stderr, "WT_PIPE_NAME not set.\n");
            exitCode = 1;
        }
    });

    // ── set-env ──
    std::string setEnvShell = "powershell";
    auto* setEnvCmd = app.add_subcommand("set-env", "Print env setup commands")->alias("setenv");
    setEnvCmd->add_option("-s,--shell", setEnvShell, "Shell: powershell, bash, cmd");
    setEnvCmd->callback([&]() {
        wchar_t pipeName[256]{}, token[256]{}, clsid[128]{};
        GetEnvironmentVariableW(L"WT_PIPE_NAME", pipeName, ARRAYSIZE(pipeName));
        GetEnvironmentVariableW(L"WT_MCP_TOKEN", token, ARRAYSIZE(token));
        GetEnvironmentVariableW(L"WT_COM_CLSID", clsid, ARRAYSIZE(clsid));

        auto pn = WideToUtf8(pipeName);
        auto tk = WideToUtf8(token);
        auto cl = WideToUtf8(clsid);

        if (setEnvShell == "powershell" || setEnvShell == "pwsh")
        {
            if (!pn.empty()) printf("$env:WT_PIPE_NAME = '%s'\n", pn.c_str());
            if (!tk.empty()) printf("$env:WT_MCP_TOKEN = '%s'\n", tk.c_str());
            if (!cl.empty()) printf("$env:WT_COM_CLSID = '%s'\n", cl.c_str());
        }
        else if (setEnvShell == "bash" || setEnvShell == "sh" || setEnvShell == "zsh")
        {
            if (!pn.empty()) printf("export WT_PIPE_NAME='%s'\n", pn.c_str());
            if (!tk.empty()) printf("export WT_MCP_TOKEN='%s'\n", tk.c_str());
            if (!cl.empty()) printf("export WT_COM_CLSID='%s'\n", cl.c_str());
        }
        else if (setEnvShell == "cmd")
        {
            if (!pn.empty()) printf("set WT_PIPE_NAME=%s\n", pn.c_str());
            if (!tk.empty()) printf("set WT_MCP_TOKEN=%s\n", tk.c_str());
            if (!cl.empty()) printf("set WT_COM_CLSID=%s\n", cl.c_str());
        }
    });

    // ── quick-pick ──
    std::string quickPickTitle;
    std::vector<std::string> quickPickChoices;
    bool quickPickFreeInput = false;
    auto* quickPickCmd = app.add_subcommand("quick-pick", "Show a quick-pick dialog in Windows Terminal");
    quickPickCmd->add_option("choices", quickPickChoices, "Choices to present")->required();
    quickPickCmd->add_option("--title", quickPickTitle, "Dialog title");
    quickPickCmd->add_flag("--free-input", quickPickFreeInput, "Allow freeform text input");
    quickPickCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;

        std::vector<std::wstring> wideChoices;
        wideChoices.reserve(quickPickChoices.size());
        for (const auto& c : quickPickChoices)
            wideChoices.push_back(Utf8ToWide(c));

        bool cancelled = false;
        std::wstring selected;
        if (FAILED(ch->QuickPick(Utf8ToWide(quickPickTitle), wideChoices, quickPickFreeInput, cancelled, selected)))
        {
            fprintf(stderr, "QuickPick failed\n");
            exitCode = 1;
            return;
        }
        if (jsonMode)
        {
            Json::Value v;
            v["cancelled"] = cancelled;
            v["selected"] = WideToUtf8(selected);
            PrintJson(v);
        }
        else
        {
            if (cancelled)
                printf("(cancelled)\n");
            else
                printf("%s\n", WideToUtf8(selected).c_str());
        }
    });

    // ── listen ──
    std::string listenTarget;
    auto* listenCmd = app.add_subcommand("listen", "Stream real-time events from Windows Terminal");
    listenCmd->add_option("-t,--target", listenTarget, "Filter by pane ID");
    listenCmd->callback([&]() {
        auto ch = connect();
        if (!ch) return;

        // Set up Ctrl-C handler to unblock the wait.
        static HANDLE s_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler([](DWORD) -> BOOL {
            SetEvent(s_stopEvent);
            return TRUE;
        }, TRUE);

        if (!jsonMode)
            fprintf(stderr, "Listening for events... (Ctrl-C to stop)\n");

        auto hr = ch->Subscribe([&](const std::wstring& eventWide) {
            auto eventUtf8 = WideToUtf8(eventWide);

            // Optionally filter by pane_id
            if (!listenTarget.empty())
            {
                Json::Value ev;
                Json::CharReaderBuilder rb;
                std::string errs;
                std::istringstream ss(eventUtf8);
                if (Json::parseFromStream(rb, ss, &ev, &errs))
                {
                    auto paneId = ev["params"].get("pane_id", "").asString();
                    if (paneId != listenTarget)
                        return;
                }
            }

            printf("%s\n", eventUtf8.c_str());
            fflush(stdout);
        });

        if (FAILED(hr))
        {
            fprintf(stderr, "Subscribe failed: 0x%08lX\n", hr);
            exitCode = 1;
            return;
        }

        // Block until Ctrl-C.
        WaitForSingleObject(s_stopEvent, INFINITE);
        ch->Unsubscribe();
        CloseHandle(s_stopEvent);
    });

    // ── Default (no subcommand) ──
    app.callback([&]() {
        if (app.get_subcommands().empty())
        {
            printf("wtcli — Windows Terminal CLI\n\n");
            printf("Usage: wtcli [--json] [--pipe-name NAME] <subcommand>\n\n");
            printf("Run 'wtcli --help' for available subcommands.\n");
        }
    });

    CLI11_PARSE(app, __argc, __argv);

    return exitCode;
}
