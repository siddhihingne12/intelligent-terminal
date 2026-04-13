// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "Formatting.h"

#include <fmt/format.h>

#include <cstdio>

// ── JSON output ──

void PrintJson(const Json::Value& val)
{
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "  ";
    printf("%s\n", Json::writeString(wb, val).c_str());
}

// ── Human-readable formatters ──

void FormatWindowsHuman(const std::vector<PROTOCOL_WINDOW_INFO>& windows)
{
    if (windows.empty())
    {
        printf("No windows found.\n");
        return;
    }
    printf("%-12s %-30s %s\n", "WINDOW_ID", "TITLE", "FOCUSED");
    for (const auto& w : windows)
    {
        auto id = WideToUtf8(BstrToWstring(w.WindowId));
        auto title = WideToUtf8(BstrToWstring(w.Title));
        printf("%-12s %-30s %s\n", id.c_str(), title.c_str(), w.IsFocused ? "*" : "");
    }
}

void FormatTabsHuman(const std::vector<PROTOCOL_TAB_INFO>& tabs)
{
    if (tabs.empty())
    {
        printf("No tabs found.\n");
        return;
    }
    printf("%-10s %-30s %s\n", "TAB_ID", "TITLE", "FOCUSED");
    for (const auto& t : tabs)
    {
        auto id = WideToUtf8(BstrToWstring(t.TabId));
        auto title = WideToUtf8(BstrToWstring(t.Title));
        printf("%-10s %-30s %s\n", id.c_str(), title.c_str(), t.IsActive ? "*" : "");
    }
}

void FormatPanesHuman(const std::vector<PROTOCOL_PANE_INFO>& panes)
{
    if (panes.empty())
    {
        printf("No panes found.\n");
        return;
    }
    printf("%-10s %-8s %-8s %-10s %s\n", "PANE_ID", "PID", "ACTIVE", "ROWS", "COLS");
    for (const auto& p : panes)
    {
        auto id = WideToUtf8(BstrToWstring(p.PaneId));
        printf("%-10s %-8lu %-8s %-10d %d\n",
               id.c_str(),
               p.Pid,
               p.IsActive ? "*" : "",
               p.Rows,
               p.Columns);
    }
}

void FormatActivePaneHuman(const PROTOCOL_PANE_INFO& info)
{
    auto pane = WideToUtf8(BstrToWstring(info.PaneId));
    auto tab = WideToUtf8(BstrToWstring(info.TabId));
    auto win = WideToUtf8(BstrToWstring(info.WindowId));
    printf("Active pane: %s (tab: %s, window: %s)\n", pane.c_str(), tab.c_str(), win.c_str());
}

void FormatPaneStatusHuman(const PROTOCOL_PROCESS_STATUS& status)
{
    auto state = WideToUtf8(BstrToWstring(status.State));
    printf("State:     %s\n", state.c_str());
    printf("PID:       %lu\n", status.Pid);
    if (status.HasExitCode)
        printf("Exit code: %d\n", status.ExitCode);
}

void FormatCreatedTabHuman(const PROTOCOL_TAB_CREATION_RESULT& result)
{
    auto tabId = WideToUtf8(BstrToWstring(result.TabId));
    auto paneId = WideToUtf8(BstrToWstring(result.PaneId));
    printf("Created tab %s (pane %s)\n", tabId.c_str(), paneId.c_str());
}

void FormatCreatedPaneHuman(const PROTOCOL_TAB_CREATION_RESULT& result)
{
    auto paneId = WideToUtf8(BstrToWstring(result.PaneId));
    printf("Created pane %s\n", paneId.c_str());
}

// ── JSON serialization of MIDL structs ──

Json::Value WindowInfoToJson(const PROTOCOL_WINDOW_INFO& w)
{
    Json::Value v;
    v["window_id"] = WideToUtf8(BstrToWstring(w.WindowId));
    v["title"] = WideToUtf8(BstrToWstring(w.Title));
    v["is_focused"] = w.IsFocused != FALSE;
    v["tab_count"] = static_cast<Json::UInt>(w.TabCount);
    return v;
}

Json::Value TabInfoToJson(const PROTOCOL_TAB_INFO& t)
{
    Json::Value v;
    v["tab_id"] = WideToUtf8(BstrToWstring(t.TabId));
    v["window_id"] = WideToUtf8(BstrToWstring(t.WindowId));
    v["title"] = WideToUtf8(BstrToWstring(t.Title));
    v["is_active"] = t.IsActive != FALSE;
    v["pane_count"] = static_cast<Json::UInt>(t.PaneCount);
    return v;
}

Json::Value PaneInfoToJson(const PROTOCOL_PANE_INFO& p)
{
    Json::Value v;
    v["pane_id"] = WideToUtf8(BstrToWstring(p.PaneId));
    v["tab_id"] = WideToUtf8(BstrToWstring(p.TabId));
    v["window_id"] = WideToUtf8(BstrToWstring(p.WindowId));
    v["title"] = WideToUtf8(BstrToWstring(p.Title));
    v["profile"] = WideToUtf8(BstrToWstring(p.Profile));
    v["is_active"] = p.IsActive != FALSE;
    v["pid"] = static_cast<Json::UInt>(p.Pid);
    v["size"]["rows"] = p.Rows;
    v["size"]["columns"] = p.Columns;
    return v;
}

Json::Value PaneOutputToJson(const PROTOCOL_PANE_OUTPUT& o)
{
    Json::Value v;
    v["pane_id"] = WideToUtf8(BstrToWstring(o.PaneId));
    v["content"] = WideToUtf8(BstrToWstring(o.Content));
    v["line_count"] = o.LineCount;
    v["truncated"] = o.Truncated != FALSE;
    return v;
}

Json::Value CreationResultToJson(const PROTOCOL_TAB_CREATION_RESULT& r)
{
    Json::Value v;
    v["tab_id"] = WideToUtf8(BstrToWstring(r.TabId));
    v["pane_id"] = WideToUtf8(BstrToWstring(r.PaneId));
    v["window_id"] = WideToUtf8(BstrToWstring(r.WindowId));
    v["pid"] = static_cast<Json::UInt>(r.Pid);
    return v;
}
