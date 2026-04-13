// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "Channel.h"

#include <json/json.h>

#include <vector>

// JSON output
void PrintJson(const Json::Value& val);

// Human-readable formatters
void FormatWindowsHuman(const std::vector<PROTOCOL_WINDOW_INFO>& windows);
void FormatTabsHuman(const std::vector<PROTOCOL_TAB_INFO>& tabs);
void FormatPanesHuman(const std::vector<PROTOCOL_PANE_INFO>& panes);
void FormatActivePaneHuman(const PROTOCOL_PANE_INFO& info);
void FormatPaneStatusHuman(const PROTOCOL_PROCESS_STATUS& status);
void FormatCreatedTabHuman(const PROTOCOL_TAB_CREATION_RESULT& result);
void FormatCreatedPaneHuman(const PROTOCOL_TAB_CREATION_RESULT& result);

// JSON serialization of MIDL structs
Json::Value WindowInfoToJson(const PROTOCOL_WINDOW_INFO& w);
Json::Value TabInfoToJson(const PROTOCOL_TAB_INFO& t);
Json::Value PaneInfoToJson(const PROTOCOL_PANE_INFO& p);
Json::Value PaneOutputToJson(const PROTOCOL_PANE_OUTPUT& o);
Json::Value CreationResultToJson(const PROTOCOL_TAB_CREATION_RESULT& r);
