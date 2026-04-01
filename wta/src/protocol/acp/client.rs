use super::prompt;
use acp::Agent as _;
use agent_client_protocol as acp;
use anyhow::Result;
use std::pin::Pin;
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc, Mutex, OnceLock,
};
use std::task::{Context, Poll};
use tokio::io::{AsyncBufReadExt, AsyncRead, AsyncWrite, BufReader, ReadBuf};
use tokio::sync::mpsc;
use tokio_util::compat::{TokioAsyncReadCompatExt, TokioAsyncWriteCompatExt};

use crate::app::{AppEvent, PermOption, PlanEntry, PlanEntryStatus};
use crate::coordinator::default_supported_delegate_agents;
use crate::shared_host::PaneContext;
use crate::shell::{ActivePaneSnapshot, ShellManager, TerminalConfig};

const ACTIVE_PANE_CONTEXT_MAX_LINES: u32 = 80;
const ACTIVE_PANE_CONTEXT_MAX_CHARS: usize = 4000;

#[derive(Debug, Clone)]
pub struct PromptSubmission {
    pub id: u64,
    pub text: String,
    pub pane_context: Option<PaneContext>,
    pub submitted_at_unix_s: f64,
}

impl PromptSubmission {
    pub fn new(text: String, pane_context: Option<PaneContext>) -> Self {
        static NEXT_PROMPT_ID: AtomicU64 = AtomicU64::new(1);
        Self {
            id: NEXT_PROMPT_ID.fetch_add(1, Ordering::Relaxed),
            text,
            pane_context,
            submitted_at_unix_s: now_unix_s(),
        }
    }

    pub fn from_parts(
        id: u64,
        text: String,
        pane_context: Option<PaneContext>,
        submitted_at_unix_s: f64,
    ) -> Self {
        Self {
            id,
            text,
            pane_context,
            submitted_at_unix_s,
        }
    }

    pub fn preview(&self) -> String {
        prompt_preview(&self.text)
    }
}

fn now_unix_s() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs_f64()
}

fn prompt_preview(text: &str) -> String {
    const MAX_CHARS: usize = 80;

    let normalized = text.replace("\r\n", "\n").replace('\r', "\n");
    let escaped = normalized.replace('\n', "\\n");
    let mut preview = String::new();
    let mut chars = escaped.chars();
    for _ in 0..MAX_CHARS {
        match chars.next() {
            Some(ch) => preview.push(ch),
            None => return preview,
        }
    }

    if chars.next().is_some() {
        preview.push_str("...");
    }

    preview
}

fn format_elapsed(start: Option<f64>, end: Option<f64>) -> String {
    match (start, end) {
        (Some(start), Some(end)) if end >= start => format!("{:.3}s", end - start),
        _ => "n/a".to_string(),
    }
}

fn first_visible_text_gap(
    first_event_at_unix_s: Option<f64>,
    first_stdout_byte_at_unix_s: Option<f64>,
    first_text_at_unix_s: Option<f64>,
) -> (String, &'static str) {
    if first_event_at_unix_s.is_some() {
        return (
            format_elapsed(first_event_at_unix_s, first_text_at_unix_s),
            "first_event",
        );
    }

    if first_stdout_byte_at_unix_s.is_some() {
        return (
            format_elapsed(first_stdout_byte_at_unix_s, first_text_at_unix_s),
            "first_transport_read",
        );
    }

    ("n/a".to_string(), "n/a")
}

fn final_timing_note(
    submitted_at_unix_s: f64,
    context_ready_at_unix_s: Option<f64>,
    prompt_sent_at_unix_s: Option<f64>,
    completed_at_unix_s: f64,
) -> String {
    format!(
        "submit->context_ready {} | prompt_sent->options_shown {}",
        format_elapsed(Some(submitted_at_unix_s), context_ready_at_unix_s),
        format_elapsed(prompt_sent_at_unix_s, Some(completed_at_unix_s))
    )
}

pub fn prompt_timing_log(turn_id: u64, submitted_at_unix_s: f64, phase: &str, details: &str) {
    let since_submit = (now_unix_s() - submitted_at_unix_s).max(0.0);
    if details.is_empty() {
        acp_log(&format!(
            "prompt_timing turn={} phase={} since_submit={:.3}s",
            turn_id, phase, since_submit
        ));
    } else {
        acp_log(&format!(
            "prompt_timing turn={} phase={} since_submit={:.3}s {}",
            turn_id, phase, since_submit, details
        ));
    }
}

#[derive(Debug)]
struct ActivePromptTiming {
    id: u64,
    preview: String,
    submitted_at_unix_s: f64,
    received_at_unix_s: Option<f64>,
    context_ready_at_unix_s: Option<f64>,
    prompt_sent_at_unix_s: Option<f64>,
    first_stdin_write_at_unix_s: Option<f64>,
    bytes_written_after_prompt: u64,
    first_stdout_byte_at_unix_s: Option<f64>,
    bytes_read_after_prompt: u64,
    first_event_at_unix_s: Option<f64>,
    first_event_kind: Option<String>,
    first_text_at_unix_s: Option<f64>,
    first_tool_call_at_unix_s: Option<f64>,
    first_permission_at_unix_s: Option<f64>,
    permission_requested_at_unix_s: Option<f64>,
    permission_wait_total_s: f64,
    event_count: u64,
}

#[derive(Default)]
struct PromptTimingState {
    active: Mutex<Option<ActivePromptTiming>>,
}

impl PromptTimingState {
    fn activate(&self, prompt: &PromptSubmission) {
        let now = now_unix_s();
        let preview = prompt.preview();
        let mut active = self.active.lock().unwrap();
        *active = Some(ActivePromptTiming {
            id: prompt.id,
            preview: preview.clone(),
            submitted_at_unix_s: prompt.submitted_at_unix_s,
            received_at_unix_s: Some(now),
            context_ready_at_unix_s: None,
            prompt_sent_at_unix_s: None,
            first_stdin_write_at_unix_s: None,
            bytes_written_after_prompt: 0,
            first_stdout_byte_at_unix_s: None,
            bytes_read_after_prompt: 0,
            first_event_at_unix_s: None,
            first_event_kind: None,
            first_text_at_unix_s: None,
            first_tool_call_at_unix_s: None,
            first_permission_at_unix_s: None,
            permission_requested_at_unix_s: None,
            permission_wait_total_s: 0.0,
            event_count: 0,
        });
        drop(active);

        prompt_timing_log(
            prompt.id,
            prompt.submitted_at_unix_s,
            "prompt_received",
            &format!(
                "queue_delay={} preview={:?}",
                format_elapsed(Some(prompt.submitted_at_unix_s), Some(now)),
                preview
            ),
        );
    }

    fn mark_context_ready(&self, prompt_len: usize) {
        let now = now_unix_s();
        let mut guard = self.active.lock().unwrap();
        if let Some(active) = guard.as_mut() {
            active.context_ready_at_unix_s = Some(now);
            let turn_id = active.id;
            let submitted_at_unix_s = active.submitted_at_unix_s;
            let details = format!(
                "prompt_len={} context_build={}",
                prompt_len,
                format_elapsed(active.received_at_unix_s, Some(now))
            );
            drop(guard);
            prompt_timing_log(turn_id, submitted_at_unix_s, "context_ready", &details);
        }
    }

    fn mark_prompt_sent(&self) {
        let now = now_unix_s();
        let mut guard = self.active.lock().unwrap();
        if let Some(active) = guard.as_mut() {
            active.prompt_sent_at_unix_s = Some(now);
            let turn_id = active.id;
            let submitted_at_unix_s = active.submitted_at_unix_s;
            let details = format!(
                "after_context_ready={}",
                format_elapsed(active.context_ready_at_unix_s, Some(now))
            );
            drop(guard);
            prompt_timing_log(turn_id, submitted_at_unix_s, "prompt_sent", &details);
        }
    }

    fn observe_session_update(&self, kind: &str) {
        let now = now_unix_s();
        let mut guard = self.active.lock().unwrap();
        if let Some(active) = guard.as_mut() {
            active.event_count += 1;
            if active.first_event_at_unix_s.is_none() {
                active.first_event_at_unix_s = Some(now);
                active.first_event_kind = Some(kind.to_string());
                let turn_id = active.id;
                let submitted_at_unix_s = active.submitted_at_unix_s;
                let details = format!(
                    "event_kind={} since_prompt_sent={}",
                    kind,
                    format_elapsed(active.prompt_sent_at_unix_s, Some(now))
                );
                drop(guard);
                prompt_timing_log(turn_id, submitted_at_unix_s, "first_event", &details);
            }
        }
    }

    fn observe_stdin_write(&self, bytes: usize) {
        let now = now_unix_s();
        let mut guard = self.active.lock().unwrap();
        if let Some(active) = guard.as_mut() {
            if active.prompt_sent_at_unix_s.is_none() {
                return;
            }

            active.bytes_written_after_prompt += bytes as u64;
            if active.first_stdin_write_at_unix_s.is_none() {
                active.first_stdin_write_at_unix_s = Some(now);
                let turn_id = active.id;
                let submitted_at_unix_s = active.submitted_at_unix_s;
                let details = format!(
                    "bytes={} since_prompt_sent={}",
                    bytes,
                    format_elapsed(active.prompt_sent_at_unix_s, Some(now))
                );
                drop(guard);
                prompt_timing_log(
                    turn_id,
                    submitted_at_unix_s,
                    "first_transport_write",
                    &details,
                );
            }
        }
    }

    fn observe_stdout_read(&self, bytes: usize) {
        let now = now_unix_s();
        let mut guard = self.active.lock().unwrap();
        if let Some(active) = guard.as_mut() {
            if active.prompt_sent_at_unix_s.is_none() {
                return;
            }

            active.bytes_read_after_prompt += bytes as u64;
            if active.first_stdout_byte_at_unix_s.is_none() {
                active.first_stdout_byte_at_unix_s = Some(now);
                let turn_id = active.id;
                let submitted_at_unix_s = active.submitted_at_unix_s;
                let details = format!(
                    "bytes={} since_prompt_sent={}",
                    bytes,
                    format_elapsed(active.prompt_sent_at_unix_s, Some(now))
                );
                drop(guard);
                prompt_timing_log(
                    turn_id,
                    submitted_at_unix_s,
                    "first_transport_read",
                    &details,
                );
            }
        }
    }

    fn observe_first_text(&self, text_len: usize) {
        let now = now_unix_s();
        let mut guard = self.active.lock().unwrap();
        if let Some(active) = guard.as_mut() {
            if active.first_text_at_unix_s.is_none() {
                active.first_text_at_unix_s = Some(now);
                let (visible_gap, visible_gap_source) = first_visible_text_gap(
                    active.first_event_at_unix_s,
                    active.first_stdout_byte_at_unix_s,
                    Some(now),
                );
                let turn_id = active.id;
                let submitted_at_unix_s = active.submitted_at_unix_s;
                let details = format!(
                    "text_len={} since_prompt_sent={} first_visible_text_gap={} gap_source={}",
                    text_len,
                    format_elapsed(active.prompt_sent_at_unix_s, Some(now)),
                    visible_gap,
                    visible_gap_source
                );
                drop(guard);
                prompt_timing_log(turn_id, submitted_at_unix_s, "first_text", &details);
            }
        }
    }

    fn observe_first_tool_call(&self, title: Option<&str>) {
        let now = now_unix_s();
        let mut guard = self.active.lock().unwrap();
        if let Some(active) = guard.as_mut() {
            if active.first_tool_call_at_unix_s.is_none() {
                active.first_tool_call_at_unix_s = Some(now);
                let turn_id = active.id;
                let submitted_at_unix_s = active.submitted_at_unix_s;
                let title_preview = title.map(prompt_preview).unwrap_or_default();
                let details = format!(
                    "title={:?} since_prompt_sent={}",
                    title_preview,
                    format_elapsed(active.prompt_sent_at_unix_s, Some(now))
                );
                drop(guard);
                prompt_timing_log(turn_id, submitted_at_unix_s, "first_tool_call", &details);
            }
        }
    }

    fn permission_requested(&self, description: &str) {
        let now = now_unix_s();
        let mut guard = self.active.lock().unwrap();
        if let Some(active) = guard.as_mut() {
            if active.first_permission_at_unix_s.is_none() {
                active.first_permission_at_unix_s = Some(now);
            }
            active.permission_requested_at_unix_s = Some(now);
            let turn_id = active.id;
            let submitted_at_unix_s = active.submitted_at_unix_s;
            let details = format!(
                "description={:?} since_prompt_sent={}",
                prompt_preview(description),
                format_elapsed(active.prompt_sent_at_unix_s, Some(now))
            );
            drop(guard);
            prompt_timing_log(
                turn_id,
                submitted_at_unix_s,
                "permission_requested",
                &details,
            );
        }
    }

    fn permission_resolved(&self, outcome: &str) {
        let now = now_unix_s();
        let mut guard = self.active.lock().unwrap();
        if let Some(active) = guard.as_mut() {
            let wait_s = active
                .permission_requested_at_unix_s
                .map(|start| (now - start).max(0.0))
                .unwrap_or(0.0);
            active.permission_wait_total_s += wait_s;
            active.permission_requested_at_unix_s = None;
            let turn_id = active.id;
            let submitted_at_unix_s = active.submitted_at_unix_s;
            drop(guard);
            prompt_timing_log(
                turn_id,
                submitted_at_unix_s,
                "permission_resolved",
                &format!("outcome={} wait={:.3}s", outcome, wait_s),
            );
        }
    }

    fn complete(&self, success: bool, error: Option<&str>) -> Option<String> {
        let now = now_unix_s();
        let mut active = self.active.lock().unwrap();
        let Some(active_prompt) = active.take() else {
            return None;
        };
        drop(active);

        let (first_visible_text_gap, first_visible_text_gap_source) = first_visible_text_gap(
            active_prompt.first_event_at_unix_s,
            active_prompt.first_stdout_byte_at_unix_s,
            active_prompt.first_text_at_unix_s,
        );

        let mut details = vec![
            format!("success={}", success),
            format!(
                "queue_delay={}",
                format_elapsed(
                    Some(active_prompt.submitted_at_unix_s),
                    active_prompt.received_at_unix_s
                )
            ),
            format!(
                "context_build={}",
                format_elapsed(
                    active_prompt.received_at_unix_s,
                    active_prompt.context_ready_at_unix_s
                )
            ),
            format!(
                "prompt_send_gap={}",
                format_elapsed(
                    active_prompt.context_ready_at_unix_s,
                    active_prompt.prompt_sent_at_unix_s
                )
            ),
            format!(
                "first_transport_write={}",
                format_elapsed(
                    active_prompt.prompt_sent_at_unix_s,
                    active_prompt.first_stdin_write_at_unix_s
                )
            ),
            format!(
                "first_transport_read={}",
                format_elapsed(
                    active_prompt.prompt_sent_at_unix_s,
                    active_prompt.first_stdout_byte_at_unix_s
                )
            ),
            format!(
                "first_event={}",
                format_elapsed(
                    active_prompt.prompt_sent_at_unix_s,
                    active_prompt.first_event_at_unix_s
                )
            ),
            format!(
                "first_event_kind={}",
                active_prompt
                    .first_event_kind
                    .unwrap_or_else(|| "n/a".to_string())
            ),
            format!(
                "first_text={}",
                format_elapsed(
                    active_prompt.prompt_sent_at_unix_s,
                    active_prompt.first_text_at_unix_s
                )
            ),
            format!("first_visible_text_gap={}", first_visible_text_gap),
            format!(
                "first_visible_text_gap_source={}",
                first_visible_text_gap_source
            ),
            format!(
                "first_tool_call={}",
                format_elapsed(
                    active_prompt.prompt_sent_at_unix_s,
                    active_prompt.first_tool_call_at_unix_s
                )
            ),
            format!(
                "first_permission={}",
                format_elapsed(
                    active_prompt.prompt_sent_at_unix_s,
                    active_prompt.first_permission_at_unix_s
                )
            ),
            format!(
                "bytes_out_after_prompt={}",
                active_prompt.bytes_written_after_prompt
            ),
            format!(
                "bytes_in_after_prompt={}",
                active_prompt.bytes_read_after_prompt
            ),
            format!(
                "permission_wait_total={:.3}s",
                active_prompt.permission_wait_total_s
            ),
            format!(
                "prompt_rpc_total={}",
                format_elapsed(active_prompt.prompt_sent_at_unix_s, Some(now))
            ),
            format!(
                "total={}",
                format_elapsed(Some(active_prompt.submitted_at_unix_s), Some(now))
            ),
            format!("event_count={}", active_prompt.event_count),
            format!("preview={:?}", active_prompt.preview),
        ];

        if let Some(error) = error {
            details.push(format!("error={:?}", error));
        }

        prompt_timing_log(
            active_prompt.id,
            active_prompt.submitted_at_unix_s,
            "prompt_complete",
            &details.join(" "),
        );

        Some(final_timing_note(
            active_prompt.submitted_at_unix_s,
            active_prompt.context_ready_at_unix_s,
            active_prompt.prompt_sent_at_unix_s,
            now,
        ))
    }
}

fn summarize_agent_identity(program: &str, args: &[&str]) -> (String, Option<String>) {
    let brand = humanize_agent_brand(program);
    let model = extract_model_arg(args).map(humanize_model_name);
    (brand, model)
}

fn requested_model_id(args: &[&str]) -> Option<String> {
    extract_model_arg(args).map(ToOwned::to_owned)
}

fn complete_prompt_request<T, E: std::fmt::Display>(
    result: std::result::Result<T, E>,
    prompt_timing: &PromptTimingState,
    event_tx: &mpsc::UnboundedSender<AppEvent>,
) {
    match result {
        Ok(_) => {
            let timing_note = prompt_timing.complete(true, None);
            if let Some(note) = timing_note {
                let _ = event_tx.send(AppEvent::TimingMetric(note));
            }
            let _ = event_tx.send(AppEvent::AgentMessageEnd);
        }
        Err(e) => {
            let error_message = e.to_string();
            let timing_note = prompt_timing.complete(false, Some(&error_message));
            if let Some(note) = timing_note {
                let _ = event_tx.send(AppEvent::TimingMetric(note));
            }
            let _ = event_tx.send(AppEvent::AgentError(format!(
                "prompt error: {}",
                error_message
            )));
        }
    }
}

fn extract_model_arg<'a>(args: &'a [&'a str]) -> Option<&'a str> {
    let mut iter = args.iter().copied();
    while let Some(arg) = iter.next() {
        if arg == "--model" || arg == "-m" {
            if let Some(value) = iter.next() {
                let trimmed = value.trim_matches(|ch| ch == '"' || ch == '\'');
                if !trimmed.is_empty() {
                    return Some(trimmed);
                }
            }
            continue;
        }

        if let Some(value) = arg
            .strip_prefix("--model=")
            .or_else(|| arg.strip_prefix("-m="))
        {
            let trimmed = value.trim_matches(|ch| ch == '"' || ch == '\'');
            if !trimmed.is_empty() {
                return Some(trimmed);
            }
        }
    }

    None
}

fn humanize_agent_brand(program: &str) -> String {
    let executable = program
        .rsplit(|ch| ch == '\\' || ch == '/')
        .next()
        .unwrap_or(program);
    let lower = executable.to_ascii_lowercase();
    let key = lower.strip_suffix(".exe").unwrap_or(&lower);

    match key {
        "copilot" => "GitHub Copilot".to_string(),
        "claude" => "Claude".to_string(),
        "codex" => "Codex".to_string(),
        _ => humanize_identifier(key),
    }
}

fn humanize_model_name(model: &str) -> String {
    let tokens: Vec<String> = model
        .split(|ch| ch == '-' || ch == '_')
        .filter(|token| !token.is_empty())
        .map(humanize_identifier)
        .collect();

    if tokens.is_empty() {
        model.to_string()
    } else {
        tokens.join(" ")
    }
}

fn humanize_identifier(token: &str) -> String {
    if token.is_empty() {
        return String::new();
    }

    let lower = token.to_ascii_lowercase();
    match lower.as_str() {
        "gpt" => "GPT".to_string(),
        "claude" => "Claude".to_string(),
        "haiku" => "Haiku".to_string(),
        "sonnet" => "Sonnet".to_string(),
        "opus" => "Opus".to_string(),
        "codex" => "Codex".to_string(),
        "mini" => "Mini".to_string(),
        "turbo" => "Turbo".to_string(),
        "max" => "Max".to_string(),
        _ if lower.chars().all(|ch| ch.is_ascii_digit() || ch == '.') => token.to_string(),
        _ => {
            let mut chars = lower.chars();
            match chars.next() {
                Some(first) => {
                    let mut title = String::with_capacity(token.len());
                    title.push(first.to_ascii_uppercase());
                    title.extend(chars);
                    title
                }
                None => String::new(),
            }
        }
    }
}

fn truncate_for_prompt(text: &str, max_chars: usize) -> String {
    if text.chars().count() <= max_chars {
        text.to_string()
    } else {
        let truncated: String = text.chars().take(max_chars).collect();
        format!("{truncated}\n...<truncated>")
    }
}

fn format_active_pane_context(snapshot: &ActivePaneSnapshot) -> String {
    let mut context = format!(
        "Focused pane context (this may be the assistant pane, not the original source pane):\n\
         - window_id={}\n\
         - tab_id={}\n\
         - pane_id={}\n",
        snapshot.window_id, snapshot.tab_id, snapshot.pane_id
    );

    if let Some(title) = &snapshot.title {
        context.push_str(&format!("         - title={}\n", title));
    }
    if let Some(profile) = &snapshot.profile {
        context.push_str(&format!("         - profile={}\n", profile));
    }
    if let Some(line_count) = snapshot.line_count {
        context.push_str(&format!("         - captured_line_count={}\n", line_count));
    }
    if snapshot.truncated {
        context.push_str("         - captured_output_truncated=true\n");
    }

    if let Some(content) = &snapshot.content {
        context.push_str("         - recent pane content:\n");
        context.push_str("```text\n");
        context.push_str(&truncate_for_prompt(content, ACTIVE_PANE_CONTEXT_MAX_CHARS));
        if !content.ends_with('\n') {
            context.push('\n');
        }
        context.push_str("```\n");
    }

    context
}

async fn live_active_pane_context(shell_mgr: &ShellManager) -> Option<String> {
    let snapshot = shell_mgr
        .wt_active_pane_snapshot(Some(ACTIVE_PANE_CONTEXT_MAX_LINES))
        .await
        .ok()?;
    Some(format_active_pane_context(&snapshot))
}

fn format_pane_context_summary(pane_context: Option<&PaneContext>) -> String {
    match pane_context {
        Some(context) => format!(
            "pane_id={:?} tab_id={:?} window_id={:?} source_pane_id={:?} effective_source_pane_id={:?} cwd={:?}",
            context.pane_id,
            context.tab_id,
            context.window_id,
            context.source_pane_id,
            context.effective_source_pane_id(),
            context.cwd
        ),
        None => "none".to_string(),
    }
}

fn json_str_or_num(value: Option<&serde_json::Value>) -> Option<String> {
    match value {
        Some(serde_json::Value::String(s)) => Some(s.clone()),
        Some(serde_json::Value::Number(n)) => Some(n.to_string()),
        _ => None,
    }
}

async fn build_terminal_context_json(
    shell_mgr: &ShellManager,
    pane_context: Option<&PaneContext>,
) -> Option<String> {
    let coordinator_target = pane_context.and_then(|context| context.pane_id.clone());
    let source_pane_id = pane_context
        .and_then(|context| context.effective_source_pane_id())
        .map(str::to_string);
    let source_tab_id = pane_context.and_then(|context| context.tab_id.clone());
    let source_window_id = pane_context.and_then(|context| context.window_id.clone());
    let source_cwd = pane_context.and_then(|context| context.cwd.clone());
    let active = shell_mgr.wt_get_active_pane().await.ok();
    let active_pane_id = active
        .as_ref()
        .and_then(|v| json_str_or_num(v.get("pane_id")));

    let mut highlighted_panes = std::collections::BTreeSet::new();
    if let Some(pane_id) = &source_pane_id {
        highlighted_panes.insert(pane_id.clone());
    }
    if let Some(pane_id) = &active_pane_id {
        highlighted_panes.insert(pane_id.clone());
    }

    let windows = shell_mgr.wt_list_windows().await.ok()?;
    let windows_arr = windows.get("windows")?.as_array()?;
    let mut tabs_json = Vec::new();

    for win in windows_arr {
        let window_id = json_str_or_num(win.get("window_id"))?;
        let window_title = win
            .get("title")
            .and_then(|value| value.as_str())
            .unwrap_or("")
            .to_string();
        let tabs = shell_mgr.wt_list_tabs(&window_id).await.ok()?;
        let tabs_arr = tabs.get("tabs")?.as_array()?;

        for tab in tabs_arr {
            let tab_id = json_str_or_num(tab.get("tab_id"))?;
            let panes = shell_mgr.wt_list_panes(&tab_id).await.ok()?;
            let panes_arr = panes.get("panes")?.as_array()?;
            let mut panels_json = Vec::new();

            for pane in panes_arr {
                let pane_id = json_str_or_num(pane.get("pane_id"))?;
                let pid = pane.get("pid").and_then(|value| value.as_u64());
                let is_active = pane
                    .get("is_active")
                    .and_then(|value| value.as_bool())
                    .unwrap_or(false);

                let buffer = if highlighted_panes.contains(&pane_id) {
                    shell_mgr
                        .wt_read_pane_output(&pane_id, Some(24))
                        .await
                        .ok()
                        .and_then(|value| {
                            value
                                .get("content")
                                .and_then(|content| content.as_str())
                                .map(|content| {
                                    truncate_for_prompt(content, ACTIVE_PANE_CONTEXT_MAX_CHARS / 2)
                                })
                        })
                } else {
                    None
                };

                let pane_role = if source_pane_id.as_deref() == Some(pane_id.as_str()) {
                    Some("source")
                } else if active_pane_id.as_deref() == Some(pane_id.as_str()) {
                    Some("active")
                } else {
                    None
                };
                let is_coordinator_target = coordinator_target.as_deref() == Some(pane_id.as_str());

                panels_json.push(serde_json::json!({
                    "id": pane_id.clone(),
                    "pane_id": pane_id.clone(),
                    "window_id": window_id.clone(),
                    "tab_id": tab_id.clone(),
                    "window_title": window_title.clone(),
                    "is_active": is_active,
                    "pid": pid,
                    "role": pane_role,
                    "isCoordinatorTarget": is_coordinator_target,
                    "sendPromptAllowed": !is_coordinator_target,
                    "cwd": if source_pane_id.as_deref() == Some(pane_id.as_str()) { source_cwd.clone() } else { None },
                    "buffer": buffer,
                }));
            }

            tabs_json.push(serde_json::json!({
                "id": tab_id.clone(),
                "tab_id": tab_id.clone(),
                "window_id": window_id.clone(),
                "label": tab.get("title").and_then(|value| value.as_str()).unwrap_or(""),
                "is_active": tab.get("is_active").and_then(|value| value.as_bool()).unwrap_or(false),
                "panels": panels_json,
            }));
        }
    }

    serde_json::to_string_pretty(&serde_json::json!({
        "activeTarget": active_pane_id,
        "coordinatorTarget": coordinator_target,
        "sourceTarget": source_pane_id,
        "sourceTabId": source_tab_id,
        "sourceWindowId": source_window_id,
        "sourceCwd": source_cwd,
        "tabs": tabs_json,
    }))
    .ok()
}

async fn build_prompt_text(
    prompt_id: u64,
    submitted_at_unix_s: f64,
    user_text: &str,
    shell_mgr: &ShellManager,
    wt_connected: bool,
    pane_context: Option<&PaneContext>,
) -> (String, String, String) {
    let total_started = std::time::Instant::now();
    let mut runtime_sections = Vec::new();

    let template_started = std::time::Instant::now();
    let planner_template = prompt::load_planner_prompt_template();
    prompt_timing_log(
        prompt_id,
        submitted_at_unix_s,
        "planner_template_ready",
        &format!(
            "name={:?} source={} dt={:.3}s",
            planner_template.display_name,
            planner_template.source_label,
            template_started.elapsed().as_secs_f64()
        ),
    );

    let agents_started = std::time::Instant::now();
    let supported_agents_json = serde_json::to_string_pretty(&default_supported_delegate_agents())
        .unwrap_or_else(|_| "[]".to_string());
    runtime_sections.push(format!(
        "## Supported Delegate Agents\n```json\n{}\n```",
        supported_agents_json
    ));
    prompt_timing_log(
        prompt_id,
        submitted_at_unix_s,
        "delegate_agents_ready",
        &format!("dt={:.3}s", agents_started.elapsed().as_secs_f64()),
    );

    if wt_connected {
        let terminal_context_started = std::time::Instant::now();
        let terminal_context_json = build_terminal_context_json(shell_mgr, pane_context).await;
        prompt_timing_log(
            prompt_id,
            submitted_at_unix_s,
            "terminal_context_ready",
            &format!(
                "present={} dt={:.3}s",
                terminal_context_json.is_some(),
                terminal_context_started.elapsed().as_secs_f64()
            ),
        );
        if let Some(terminal_context_json) = terminal_context_json {
            runtime_sections.push(format!(
                "## Terminal Context JSON\n```json\n{}\n```",
                terminal_context_json
            ));
        }
    } else {
        prompt_timing_log(
            prompt_id,
            submitted_at_unix_s,
            "terminal_context_skipped",
            "wt_connected=false",
        );
    }

    let active_pane_started = std::time::Instant::now();
    let active_pane = live_active_pane_context(shell_mgr).await;
    prompt_timing_log(
        prompt_id,
        submitted_at_unix_s,
        "active_pane_context_ready",
        &format!(
            "present={} dt={:.3}s",
            active_pane.is_some(),
            active_pane_started.elapsed().as_secs_f64()
        ),
    );
    if let Some(active_pane) = active_pane {
        runtime_sections.push(format!(
            "## Focused Pane Context\n{}",
            active_pane.trim_end()
        ));
    }

    let assemble_started = std::time::Instant::now();
    let prompt_body = prompt::merge_runtime_sections(&planner_template.content, &runtime_sections);
    let prompt = format!("{}\n\n## User Request\n{}", prompt_body, user_text);
    prompt_timing_log(
        prompt_id,
        submitted_at_unix_s,
        "prompt_assembled",
        &format!(
            "assemble_dt={:.3}s total_context_dt={:.3}s prompt_len={}",
            assemble_started.elapsed().as_secs_f64(),
            total_started.elapsed().as_secs_f64(),
            prompt.len()
        ),
    );
    (
        prompt,
        planner_template.source_label,
        planner_template.display_name,
    )
}

/// Write a line to wta-acp-debug.log.
fn acp_log_enabled() -> bool {
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| true)
}

fn acp_log(msg: &str) {
    if !acp_log_enabled() {
        return;
    }
    use std::io::Write;
    if let Ok(mut f) = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(crate::runtime_paths::runtime_log_path("wta-acp-debug.log"))
    {
        let elapsed = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default();
        let _ = writeln!(f, "[{:.3}] {}", elapsed.as_secs_f64(), msg);
    }
}

fn acp_log_built_prompt(
    user_text: &str,
    pane_context: Option<&PaneContext>,
    prompt_source: &str,
    prompt_text: &str,
) {
    if !acp_log_enabled() {
        return;
    }

    acp_log(&format!(
        "planner_prompt_begin user_text_len={} pane_context={} prompt_source={}",
        user_text.len(),
        format_pane_context_summary(pane_context),
        prompt_source
    ));
    acp_log(&format!("planner_prompt_text:\n{}", prompt_text));
    acp_log("planner_prompt_end");
}

#[derive(Clone)]
struct StartupProbe {
    begin: std::time::Instant,
}

impl StartupProbe {
    fn new() -> Self {
        Self {
            begin: std::time::Instant::now(),
        }
    }

    fn log(&self, msg: &str) {
        if acp_log_enabled() {
            acp_log(&format!(
                "{} (t+{:.3}s)",
                msg,
                self.begin.elapsed().as_secs_f64()
            ));
        }
    }
}

struct StartupInstrumentedReader<R> {
    inner: R,
    probe: StartupProbe,
    label: &'static str,
    saw_data: bool,
    prompt_timing: Arc<PromptTimingState>,
}

impl<R> StartupInstrumentedReader<R> {
    fn new(
        inner: R,
        probe: StartupProbe,
        label: &'static str,
        prompt_timing: Arc<PromptTimingState>,
    ) -> Self {
        Self {
            inner,
            probe,
            label,
            saw_data: false,
            prompt_timing,
        }
    }
}

impl<R: AsyncRead + Unpin> AsyncRead for StartupInstrumentedReader<R> {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<std::io::Result<()>> {
        let filled_before = buf.filled().len();
        match Pin::new(&mut self.inner).poll_read(cx, buf) {
            Poll::Ready(Ok(())) => {
                let read_len = buf.filled().len().saturating_sub(filled_before);
                if read_len > 0 && !self.saw_data {
                    self.saw_data = true;
                    self.probe.log(&format!(
                        "first data received on agent {}: {} byte(s)",
                        self.label, read_len
                    ));
                }
                if read_len > 0 {
                    self.prompt_timing.observe_stdout_read(read_len);
                }
                Poll::Ready(Ok(()))
            }
            other => other,
        }
    }
}

struct InstrumentedAgentWriter<W> {
    inner: W,
    prompt_timing: Arc<PromptTimingState>,
}

impl<W> InstrumentedAgentWriter<W> {
    fn new(inner: W, prompt_timing: Arc<PromptTimingState>) -> Self {
        Self {
            inner,
            prompt_timing,
        }
    }
}

impl<W: AsyncWrite + Unpin> AsyncWrite for InstrumentedAgentWriter<W> {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        match Pin::new(&mut self.inner).poll_write(cx, buf) {
            Poll::Ready(Ok(written)) => {
                if written > 0 {
                    self.prompt_timing.observe_stdin_write(written);
                }
                Poll::Ready(Ok(written))
            }
            other => other,
        }
    }

    fn poll_flush(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.inner).poll_flush(cx)
    }

    fn poll_shutdown(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.inner).poll_shutdown(cx)
    }
}

/// Shared state accessible from the Client trait impl.
struct ClientState {
    event_tx: mpsc::UnboundedSender<AppEvent>,
    shell_mgr: Arc<ShellManager>,
    prompt_timing: Arc<PromptTimingState>,
}

/// Our Client trait implementation — handles incoming agent requests and notifications.
struct WtaClient {
    state: Arc<ClientState>,
}

fn session_update_kind(update: &acp::SessionUpdate) -> &'static str {
    match update {
        acp::SessionUpdate::AgentThoughtChunk(_) => "agent_thought_chunk",
        acp::SessionUpdate::AgentMessageChunk(_) => "agent_message_chunk",
        acp::SessionUpdate::ToolCall(_) => "tool_call",
        acp::SessionUpdate::ToolCallUpdate(_) => "tool_call_update",
        acp::SessionUpdate::Plan(_) => "plan",
        _ => "other",
    }
}

#[async_trait::async_trait(?Send)]
impl acp::Client for WtaClient {
    async fn request_permission(
        &self,
        args: acp::RequestPermissionRequest,
    ) -> acp::Result<acp::RequestPermissionResponse> {
        if acp_log_enabled() {
            acp_log(&format!(
                "request_permission: {:?}",
                args.tool_call.fields.title
            ));
        }
        let description = args
            .tool_call
            .fields
            .title
            .clone()
            .unwrap_or_else(|| "Permission requested".to_string());
        self.state.prompt_timing.permission_requested(&description);

        let options: Vec<PermOption> = args
            .options
            .iter()
            .map(|o| PermOption {
                id: o.option_id.to_string(),
                name: o.name.clone(),
                kind: format!("{:?}", o.kind),
            })
            .collect();

        let (resp_tx, resp_rx) = tokio::sync::oneshot::channel();

        let _ = self.state.event_tx.send(AppEvent::PermissionRequest {
            description,
            options,
            responder: resp_tx,
        });

        // Wait for user to choose
        match resp_rx.await {
            Ok(option_id) => {
                self.state.prompt_timing.permission_resolved("selected");
                Ok(acp::RequestPermissionResponse::new(
                    acp::RequestPermissionOutcome::Selected(acp::SelectedPermissionOutcome::new(
                        option_id,
                    )),
                ))
            }
            Err(_) => {
                self.state.prompt_timing.permission_resolved("cancelled");
                Ok(acp::RequestPermissionResponse::new(
                    acp::RequestPermissionOutcome::Cancelled,
                ))
            }
        }
    }

    async fn session_notification(&self, args: acp::SessionNotification) -> acp::Result<()> {
        if acp_log_enabled() {
            acp_log(&format!("session_notification: {:?}", args.update));
        }
        self.state
            .prompt_timing
            .observe_session_update(session_update_kind(&args.update));
        match args.update {
            acp::SessionUpdate::AgentThoughtChunk(chunk) => {
                if let acp::ContentBlock::Text(text_content) = chunk.content {
                    let _ = self
                        .state
                        .event_tx
                        .send(AppEvent::AgentThoughtChunk(text_content.text));
                }
            }
            acp::SessionUpdate::AgentMessageChunk(chunk) => {
                if let acp::ContentBlock::Text(text_content) = chunk.content {
                    self.state
                        .prompt_timing
                        .observe_first_text(text_content.text.len());
                    let _ = self
                        .state
                        .event_tx
                        .send(AppEvent::AgentMessageChunk(text_content.text));
                }
            }
            acp::SessionUpdate::ToolCall(tool_call) => {
                self.state
                    .prompt_timing
                    .observe_first_tool_call(Some(tool_call.title.as_str()));
                let _ = self.state.event_tx.send(AppEvent::ToolCall {
                    id: tool_call.tool_call_id.to_string(),
                    title: tool_call.title.clone(),
                    status: format!("{:?}", tool_call.status),
                });
            }
            acp::SessionUpdate::ToolCallUpdate(update) => {
                if let Some(status) = &update.fields.status {
                    let _ = self.state.event_tx.send(AppEvent::ToolCallUpdate {
                        id: update.tool_call_id.to_string(),
                        status: format!("{:?}", status),
                    });
                }
            }
            acp::SessionUpdate::Plan(plan) => {
                let entries = plan
                    .entries
                    .iter()
                    .map(|e| PlanEntry {
                        content: e.content.clone(),
                        status: match e.status {
                            acp::PlanEntryStatus::Completed => PlanEntryStatus::Completed,
                            acp::PlanEntryStatus::InProgress => PlanEntryStatus::InProgress,
                            _ => PlanEntryStatus::Pending,
                        },
                    })
                    .collect();
                let _ = self.state.event_tx.send(AppEvent::Plan(entries));
            }
            _ => {} // Ignore other update types for now
        }
        Ok(())
    }

    async fn create_terminal(
        &self,
        args: acp::CreateTerminalRequest,
    ) -> acp::Result<acp::CreateTerminalResponse> {
        if acp_log_enabled() {
            acp_log(&format!(
                "create_terminal called: cmd={} args={:?}",
                args.command, args.args
            ));
        }
        let env: Vec<(String, String)> = args
            .env
            .iter()
            .map(|e| (e.name.clone(), e.value.clone()))
            .collect();
        let cwd = args.cwd.as_ref().map(|p| p.to_string_lossy().to_string());

        let config = TerminalConfig {
            command: args.command.clone(),
            args: args.args.clone(),
            cwd,
            env,
        };

        match self.state.shell_mgr.create_terminal(config).await {
            Ok(id) => {
                // Show tool-call-like feedback
                let _ = self.state.event_tx.send(AppEvent::ToolCall {
                    id: id.clone(),
                    title: format!("{} {}", args.command, args.args.join(" ")),
                    status: "running".to_string(),
                });
                Ok(acp::CreateTerminalResponse::new(id))
            }
            Err(e) => Err(acp::Error::internal_error().data(e.to_string())),
        }
    }

    async fn terminal_output(
        &self,
        args: acp::TerminalOutputRequest,
    ) -> acp::Result<acp::TerminalOutputResponse> {
        match self
            .state
            .shell_mgr
            .get_output(&args.terminal_id.to_string())
            .await
        {
            Ok(output) => {
                let mut resp = acp::TerminalOutputResponse::new(output.data, false);
                if let Some(code) = output.exit_status {
                    resp = resp.exit_status(acp::TerminalExitStatus::new().exit_code(code));
                }
                Ok(resp)
            }
            Err(e) => Err(acp::Error::internal_error().data(e.to_string())),
        }
    }

    async fn wait_for_terminal_exit(
        &self,
        args: acp::WaitForTerminalExitRequest,
    ) -> acp::Result<acp::WaitForTerminalExitResponse> {
        let tid = args.terminal_id.to_string();

        match self.state.shell_mgr.wait_for_exit(&tid).await {
            Ok(code) => {
                // Update tool call status
                let _ = self.state.event_tx.send(AppEvent::ToolCallUpdate {
                    id: tid,
                    status: format!("exited ({})", code),
                });
                Ok(acp::WaitForTerminalExitResponse::new(
                    acp::TerminalExitStatus::new().exit_code(code),
                ))
            }
            Err(e) => Err(acp::Error::internal_error().data(e.to_string())),
        }
    }

    async fn release_terminal(
        &self,
        args: acp::ReleaseTerminalRequest,
    ) -> acp::Result<acp::ReleaseTerminalResponse> {
        let _ = self
            .state
            .shell_mgr
            .release(&args.terminal_id.to_string())
            .await;
        Ok(acp::ReleaseTerminalResponse::new())
    }

    async fn kill_terminal(
        &self,
        args: acp::KillTerminalRequest,
    ) -> acp::Result<acp::KillTerminalResponse> {
        let _ = self
            .state
            .shell_mgr
            .kill(&args.terminal_id.to_string())
            .await;
        Ok(acp::KillTerminalResponse::new())
    }
}

/// Top-level ACP client task: spawn agent, handshake, prompt loop.
pub async fn run_acp_client(
    agent_cmd: String,
    event_tx: mpsc::UnboundedSender<AppEvent>,
    mut prompt_rx: mpsc::UnboundedReceiver<PromptSubmission>,
    shell_mgr: Arc<ShellManager>,
    wt_connected: bool,
) {
    let startup_probe = StartupProbe::new();
    startup_probe.log(&format!(
        "run_acp_client task start agent_cmd={} wt_connected={}",
        agent_cmd, wt_connected
    ));
    startup_probe.log("run_acp_client entering run_inner");
    if let Err(e) = run_inner(
        agent_cmd,
        event_tx.clone(),
        &mut prompt_rx,
        shell_mgr,
        wt_connected,
    )
    .await
    {
        startup_probe.log(&format!("run_acp_client failed: {:#}", e));
        let _ = event_tx.send(AppEvent::AgentError(format!("{:#}", e)));
    } else {
        startup_probe.log("run_acp_client completed");
    }
}

async fn run_inner(
    agent_cmd: String,
    event_tx: mpsc::UnboundedSender<AppEvent>,
    prompt_rx: &mut mpsc::UnboundedReceiver<PromptSubmission>,
    shell_mgr: Arc<ShellManager>,
    wt_connected: bool,
) -> Result<()> {
    let startup_probe = StartupProbe::new();

    // Parse agent command into program + args
    let parts: Vec<&str> = agent_cmd.split_whitespace().collect();
    let program = parts
        .first()
        .ok_or_else(|| anyhow::anyhow!("empty agent command"))?;
    let args = &parts[1..];

    // Spawn agent subprocess
    let spawn_stage = format!("Spawning {}...", program);
    let _ = event_tx.send(AppEvent::ConnectionStage(spawn_stage.clone()));
    startup_probe.log(&format!("{} cmd={}", spawn_stage, agent_cmd));

    let mut child = tokio::process::Command::new(program)
        .args(args)
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .kill_on_drop(true)
        .spawn()
        .map_err(|e| anyhow::anyhow!("failed to spawn agent '{}': {}", agent_cmd, e))?;

    let child_pid = child.id();
    startup_probe.log(&format!("Spawned {} pid={:?}", program, child_pid));

    let prompt_timing = Arc::new(PromptTimingState::default());
    let outgoing = InstrumentedAgentWriter::new(child.stdin.take().unwrap(), prompt_timing.clone())
        .compat_write();
    startup_probe.log("Agent stdin pipe attached");

    let stdout = child.stdout.take().unwrap();
    startup_probe.log("Agent stdout pipe attached");
    let incoming = StartupInstrumentedReader::new(
        stdout,
        startup_probe.clone(),
        "stdout",
        prompt_timing.clone(),
    )
    .compat();

    if let Some(stderr) = child.stderr.take() {
        let stderr_probe = startup_probe.clone();
        tokio::task::spawn_local(async move {
            stderr_probe.log("Agent stderr pipe attached");
            let mut lines = BufReader::new(stderr).lines();
            let mut line_no = 0usize;
            loop {
                match lines.next_line().await {
                    Ok(Some(line)) => {
                        line_no += 1;
                        stderr_probe.log(&format!("agent stderr[{line_no}]: {}", line));
                    }
                    Ok(None) => {
                        stderr_probe.log("Agent stderr closed");
                        break;
                    }
                    Err(e) => {
                        stderr_probe.log(&format!("Agent stderr read error: {}", e));
                        break;
                    }
                }
            }
        });
    }

    let child_probe = startup_probe.clone();
    tokio::task::spawn_local(async move {
        match child.wait().await {
            Ok(status) => child_probe.log(&format!("Agent process exited: {}", status)),
            Err(e) => child_probe.log(&format!("Agent wait failed: {}", e)),
        }
    });

    let state = Arc::new(ClientState {
        event_tx: event_tx.clone(),
        shell_mgr: shell_mgr.clone(),
        prompt_timing,
    });

    let client = WtaClient {
        state: state.clone(),
    };

    let (conn, handle_io) = acp::ClientSideConnection::new(client, outgoing, incoming, |fut| {
        tokio::task::spawn_local(fut);
    });
    startup_probe.log("ACP client connection created");

    let io_probe = startup_probe.clone();
    tokio::task::spawn_local(async move {
        io_probe.log("ACP handle_io task started");
        if let Err(e) = handle_io.await {
            io_probe.log(&format!("ACP handle_io failed: {:#}", e));
            eprintln!("ACP I/O error: {:#}", e);
        } else {
            io_probe.log("ACP handle_io completed");
        }
    });

    // Initialize
    let _ = event_tx.send(AppEvent::ConnectionStage("Initializing ACP...".to_string()));
    startup_probe.log("Initializing ACP");
    let init_resp = conn
        .initialize(
            acp::InitializeRequest::new(acp::ProtocolVersion::V1)
                .client_capabilities(acp::ClientCapabilities::new().terminal(true))
                .client_info(
                    acp::Implementation::new("wta", env!("CARGO_PKG_VERSION"))
                        .title("Windows Terminal Agent"),
                ),
        )
        .await
        .map_err(|e| anyhow::anyhow!("initialize failed: {}", e))?;

    // Log the agent's initialize response for debugging
    startup_probe.log(&format!("Agent init response received: {:?}", init_resp));

    // Create session
    let _ = event_tx.send(AppEvent::ConnectionStage("Creating session...".to_string()));
    startup_probe.log("Creating session");
    let cwd = std::env::current_dir().unwrap_or_default();
    startup_probe.log(&format!("Using session cwd={}", cwd.display()));
    let session = conn
        .new_session(acp::NewSessionRequest::new(cwd))
        .await
        .map_err(|e| anyhow::anyhow!("new_session failed: {}", e))?;

    let session_id = session.session_id.clone();
    startup_probe.log(&format!("Session created: {}", session_id));

    if let Some(requested_model) = requested_model_id(args) {
        let _ = event_tx.send(AppEvent::ConnectionStage(format!(
            "Selecting model {}...",
            requested_model
        )));
        startup_probe.log(&format!("Setting ACP session model to {}", requested_model));
        conn.set_session_model(acp::SetSessionModelRequest::new(
            session_id.clone(),
            requested_model.clone(),
        ))
        .await
        .map_err(|e| {
            anyhow::anyhow!(
                "set_session_model failed for requested model {}: {}",
                requested_model,
                e
            )
        })?;
        startup_probe.log(&format!("ACP session model set to {}", requested_model));
    }

    // Notify app of connection
    let (agent_name, agent_model) = summarize_agent_identity(program, args);
    let _ = event_tx.send(AppEvent::AgentConnected {
        name: agent_name,
        model: agent_model,
        session_id: session_id.to_string(),
    });

    // Prompt loop: wait for user input, send to agent
    while let Some(prompt) = prompt_rx.recv().await {
        state.prompt_timing.activate(&prompt);
        let _ = event_tx.send(AppEvent::ProgressStatus("Preparing context...".to_string()));
        let (text, prompt_source, prompt_name) = build_prompt_text(
            prompt.id,
            prompt.submitted_at_unix_s,
            &prompt.text,
            &shell_mgr,
            wt_connected,
            prompt.pane_context.as_ref(),
        )
        .await;
        let _ = event_tx.send(AppEvent::PromptTemplateLoaded { name: prompt_name });
        state.prompt_timing.mark_context_ready(text.len());
        acp_log_built_prompt(
            &prompt.text,
            prompt.pane_context.as_ref(),
            &prompt_source,
            &text,
        );
        let _ = event_tx.send(AppEvent::ProgressStatus("Thinking...".to_string()));
        state.prompt_timing.mark_prompt_sent();
        let result = conn
            .prompt(acp::PromptRequest::new(
                session_id.clone(),
                vec![text.into()],
            ))
            .await;
        complete_prompt_request(result, &state.prompt_timing, &event_tx);
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{
        complete_prompt_request, extract_model_arg, requested_model_id, summarize_agent_identity,
        PromptTimingState,
    };
    use crate::app::AppEvent;
    use tokio::sync::mpsc;

    #[test]
    fn parses_model_from_separate_flag() {
        let args = ["--acp", "--stdio", "--model", "claude-haiku-4.5"];
        assert_eq!(extract_model_arg(&args), Some("claude-haiku-4.5"));
    }

    #[test]
    fn humanizes_brand_and_model_for_copilot() {
        let args = ["--acp", "--stdio", "--model=claude-haiku-4.5"];
        let (brand, model) = summarize_agent_identity("copilot", &args);

        assert_eq!(brand, "GitHub Copilot");
        assert_eq!(model.as_deref(), Some("Claude Haiku 4.5"));
    }

    #[test]
    fn humanizes_gpt_5_mini_for_copilot() {
        let args = ["--acp", "--stdio", "--model=gpt-5-mini"];
        let (brand, model) = summarize_agent_identity("copilot", &args);

        assert_eq!(brand, "GitHub Copilot");
        assert_eq!(model.as_deref(), Some("GPT 5 Mini"));
    }

    #[test]
    fn requested_model_returns_owned_value() {
        let args = ["--acp", "--stdio", "--model", "claude-haiku-4.5"];
        assert_eq!(
            requested_model_id(&args).as_deref(),
            Some("claude-haiku-4.5")
        );
    }

    #[test]
    fn successful_prompt_completion_emits_message_end_only() {
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();
        let prompt_timing = PromptTimingState::default();

        complete_prompt_request(Ok::<(), &str>(()), &prompt_timing, &event_tx);

        match event_rx.try_recv() {
            Ok(AppEvent::AgentMessageEnd) => {}
            Ok(_) => panic!("expected AgentMessageEnd"),
            Err(err) => panic!("expected AgentMessageEnd, got channel error: {err}"),
        }
        assert!(event_rx.try_recv().is_err());
    }

    #[test]
    fn failed_prompt_completion_emits_error_only() {
        let (event_tx, mut event_rx) = mpsc::unbounded_channel();
        let prompt_timing = PromptTimingState::default();

        complete_prompt_request(Err::<(), _>("boom"), &prompt_timing, &event_tx);

        match event_rx.try_recv() {
            Ok(AppEvent::AgentError(message)) => {
                assert_eq!(message, "prompt error: boom");
            }
            Ok(_) => panic!("expected AgentError"),
            Err(err) => panic!("expected AgentError, got channel error: {err}"),
        }
        assert!(event_rx.try_recv().is_err());
    }
}
