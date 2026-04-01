use std::collections::BTreeSet;
use std::sync::Arc;

use anyhow::{anyhow, bail, Context, Result};
use serde::{Deserialize, Serialize};
use tokio::sync::mpsc;
use tokio::time::{sleep, Duration};

use crate::app::AppEvent;
use crate::shell::ShellManager;

#[derive(Debug, Clone, Serialize)]
pub struct SupportedDelegateAgent {
    pub id: String,
    pub name: String,
    pub description: String,
}

#[derive(Debug, Clone)]
pub struct DelegateAgentRuntime {
    pub id: String,
    pub name: String,
    pub description: String,
    pub commandline: String,
    pub prompt_delivery: DelegatePromptDelivery,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DelegatePromptDelivery {
    LaunchThenSend,
    LaunchWithStartupPrompt,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RecommendationSet {
    #[serde(default)]
    pub recommended_choice: Option<usize>,
    pub choices: Vec<RecommendationChoice>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RecommendationChoice {
    pub choice: usize,
    pub title: String,
    #[serde(default)]
    pub rationale: String,
    pub actions: Vec<RecommendedAction>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum OpenTarget {
    Tab,
    Panel,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum RecommendedAction {
    Send {
        parent: String,
        input: String,
    },
    OpenAndSend {
        target: OpenTarget,
        #[serde(default)]
        parent: Option<String>,
        input: String,
        #[serde(default)]
        agent: Option<String>,
        #[serde(default)]
        cwd: Option<String>,
        #[serde(default)]
        title: Option<String>,
    },
}

pub fn default_supported_delegate_agents() -> Vec<SupportedDelegateAgent> {
    vec![SupportedDelegateAgent {
        id: "copilot".to_string(),
        name: "GitHub Copilot".to_string(),
        description:
            "Launches `copilot` in a new terminal target with a self-contained startup task prompt."
                .to_string(),
    }]
}

pub fn default_delegate_agent_runtimes(
    delegate_agent_cmd: Option<&str>,
    agent_cmd: Option<&str>,
) -> Vec<DelegateAgentRuntime> {
    let commandline = resolve_delegate_runtime_commandline(delegate_agent_cmd, agent_cmd)
        .unwrap_or_else(|| "copilot".to_string());
    vec![DelegateAgentRuntime {
        id: "copilot".to_string(),
        name: "GitHub Copilot".to_string(),
        description:
            "Launches `copilot` directly in a new terminal target with an interactive startup task prompt."
                .to_string(),
        commandline,
        prompt_delivery: DelegatePromptDelivery::LaunchWithStartupPrompt,
    }]
}

pub fn parse_recommendation_set(text: &str) -> Result<RecommendationSet> {
    let json = extract_json_code_block(text)
        .or_else(|| extract_first_json_object(text))
        .context("no recommendation JSON block found")?;

    let mut parsed: RecommendationSet =
        serde_json::from_str(json).context("failed to parse recommendation JSON")?;
    validate_recommendation_set(&parsed)?;
    parsed.choices.sort_by_key(|c| c.choice);
    Ok(parsed)
}

pub fn validate_recommendation_set_for_coordinator_target(
    set: &RecommendationSet,
    coordinator_target: Option<&str>,
) -> Result<()> {
    let Some(coordinator_target) = coordinator_target
        .map(str::trim)
        .filter(|id| !id.is_empty())
    else {
        return Ok(());
    };

    for choice in &set.choices {
        for action in &choice.actions {
            if let RecommendedAction::Send { parent, .. } = action {
                if parent == coordinator_target {
                    bail!(
                        "choice {} send targets the current coordinator pane {}; use another existing pane or open_and_send instead",
                        choice.choice,
                        coordinator_target
                    );
                }
            }
        }
    }

    Ok(())
}

pub fn recommended_choice_index(set: &RecommendationSet) -> usize {
    if let Some(choice_no) = set.recommended_choice {
        if let Some(idx) = set
            .choices
            .iter()
            .position(|choice| choice.choice == choice_no)
        {
            return idx;
        }
    }
    0
}

pub async fn run_recommendation_executor(
    mut rx: mpsc::UnboundedReceiver<RecommendationChoice>,
    event_tx: mpsc::UnboundedSender<AppEvent>,
    shell_mgr: Arc<ShellManager>,
    delegate_agents: Vec<DelegateAgentRuntime>,
) {
    while let Some(choice) = rx.recv().await {
        match execute_choice(&choice, &shell_mgr, &delegate_agents, &event_tx).await {
            Ok(()) => {}
            Err(err) => {
                let _ = event_tx.send(AppEvent::SystemMessage(format!(
                    "Choice {} failed: {:#}",
                    choice.choice, err
                )));
            }
        }
    }
}

async fn execute_choice(
    choice: &RecommendationChoice,
    shell_mgr: &ShellManager,
    delegate_agents: &[DelegateAgentRuntime],
    event_tx: &mpsc::UnboundedSender<AppEvent>,
) -> Result<()> {
    for action in &choice.actions {
        match action {
            RecommendedAction::Send { parent, input } => {
                ensure_non_empty("parent", parent)?;
                ensure_non_empty("input", input)?;
                coordinator_log(&format!(
                    "send begin parent={} input_chars={} input_preview={:?}",
                    parent,
                    input.chars().count(),
                    truncate_for_log(input, 120)
                ));
                let _ = event_tx.send(AppEvent::ExecutionInfo(format!(
                    "Sending input to pane {}.",
                    parent
                )));
                let payload = format!("{input}\r");
                let result = shell_mgr
                    .wt_send_input(parent, &payload)
                    .await
                    .with_context(|| format!("failed to send input to pane {}", parent))?;
                coordinator_log(&format!(
                    "send success parent={} response={}",
                    parent,
                    summarize_json_for_log(&result)
                ));
                let _ = event_tx.send(AppEvent::ExecutionInfo(format!(
                    "Sent input to pane {}.",
                    parent
                )));
            }
            RecommendedAction::OpenAndSend {
                target,
                parent,
                input,
                agent,
                cwd,
                title,
            } => {
                ensure_non_empty("input", input)?;
                let runtime = match agent.as_deref() {
                    Some(agent) => Some(lookup_delegate_agent(delegate_agents, agent)?),
                    None => None,
                };
                let runtime_name = runtime.map(|agent| agent.name.as_str());
                let delivery_mode = runtime
                    .map(|agent| agent.prompt_delivery)
                    .unwrap_or(DelegatePromptDelivery::LaunchThenSend);
                let target_label = open_target_label(target);
                coordinator_log(&format!(
                    "open_and_send begin target={} parent={:?} agent={:?} cwd={:?} title={:?} delivery_mode={} input_chars={} input_preview={:?}",
                    target_label,
                    parent,
                    agent,
                    cwd,
                    title,
                    delegate_prompt_delivery_label(delivery_mode),
                    input.chars().count(),
                    truncate_for_log(input, 120)
                ));
                let _ = event_tx.send(AppEvent::ExecutionInfo(match runtime_name {
                    Some(name) => format!("Opening {} for {}.", target_label, name),
                    None => format!("Opening {}.", target_label),
                }));
                let commandline = runtime
                    .map(|runtime| build_delegate_launch_commandline(runtime, input))
                    .transpose()?;
                let pane_id = match target {
                    OpenTarget::Tab => {
                        let result = shell_mgr
                            .wt_create_tab(
                                commandline.as_deref(),
                                cwd.as_deref(),
                                title.as_deref().or(runtime_name),
                            )
                            .await
                            .context("failed to create tab")?;
                        coordinator_log(&format!(
                            "open_and_send create_tab response={}",
                            summarize_json_for_log(&result)
                        ));
                        resolve_created_pane_id(&result, "create_tab")?
                    }
                    OpenTarget::Panel => {
                        let parent = required_parent(parent.as_deref(), "open_and_send")?;
                        let result = shell_mgr
                            .wt_split_pane(
                                parent,
                                commandline.as_deref(),
                                cwd.as_deref(),
                                None,
                                None,
                            )
                            .await
                            .with_context(|| format!("failed to split pane {}", parent))?;
                        coordinator_log(&format!(
                            "open_and_send split_pane parent={} response={}",
                            parent,
                            summarize_json_for_log(&result)
                        ));
                        resolve_created_pane_id(&result, "split_pane")?
                    }
                };
                coordinator_log(&format!(
                    "open_and_send resolved target={} pane_id={}",
                    target_label, pane_id
                ));
                let _ = event_tx.send(AppEvent::ExecutionInfo(format!(
                    "Opened {} pane {}.",
                    target_label, pane_id
                )));
                if matches!(delivery_mode, DelegatePromptDelivery::LaunchThenSend) {
                    send_input_to_new_pane(shell_mgr, &pane_id, input, event_tx).await?;
                } else {
                    coordinator_log(&format!(
                        "open_and_send startup_prompt_delivery target={} pane_id={} commandline={:?}",
                        target_label,
                        pane_id,
                        commandline
                    ));
                    let _ = event_tx.send(AppEvent::ExecutionInfo(format!(
                        "Passed startup prompt to pane {} on launch.",
                        pane_id
                    )));
                }
            }
        }
    }

    Ok(())
}

fn validate_recommendation_set(set: &RecommendationSet) -> Result<()> {
    if !(1..=3).contains(&set.choices.len()) {
        bail!("expected 1 to 3 choices, got {}", set.choices.len());
    }

    let mut seen = BTreeSet::new();
    for choice in &set.choices {
        if !(1..=3).contains(&choice.choice) {
            bail!("choice numbers must be 1..=3");
        }
        if !seen.insert(choice.choice) {
            bail!("duplicate choice number {}", choice.choice);
        }
        ensure_non_empty("title", &choice.title)?;
        if choice.actions.is_empty() {
            bail!("choice {} has no actions", choice.choice);
        }
        for action in &choice.actions {
            validate_action(action)?;
        }
    }

    Ok(())
}

fn validate_action(action: &RecommendedAction) -> Result<()> {
    match action {
        RecommendedAction::Send { parent, input } => {
            ensure_non_empty("parent", parent)?;
            ensure_non_empty("input", input)?;
        }
        RecommendedAction::OpenAndSend {
            target,
            parent,
            input,
            agent,
            ..
        } => {
            ensure_non_empty("input", input)?;
            if let Some(parent) = parent.as_deref() {
                ensure_non_empty("parent", parent)?;
            }
            if let Some(agent) = agent.as_deref() {
                ensure_non_empty("agent", agent)?;
            }
            if matches!(target, OpenTarget::Panel) {
                required_parent(parent.as_deref(), "open_and_send")?;
            }
        }
    }

    Ok(())
}

fn lookup_delegate_agent<'a>(
    delegate_agents: &'a [DelegateAgentRuntime],
    id: &str,
) -> Result<&'a DelegateAgentRuntime> {
    delegate_agents
        .iter()
        .find(|agent| agent.id == id)
        .ok_or_else(|| anyhow!("unsupported delegate agent '{}'", id))
}

fn build_delegate_launch_commandline(
    runtime: &DelegateAgentRuntime,
    input: &str,
) -> Result<String> {
    let commandline = runtime.commandline.trim();
    if commandline.is_empty() {
        bail!("delegate agent runtime commandline is empty");
    }
    match runtime.prompt_delivery {
        DelegatePromptDelivery::LaunchThenSend => Ok(commandline.to_string()),
        DelegatePromptDelivery::LaunchWithStartupPrompt => {
            ensure_non_empty("input", input)?;
            build_delegate_startup_prompt_commandline(commandline, input)
        }
    }
}

fn resolve_delegate_runtime_commandline(
    delegate_agent_cmd: Option<&str>,
    agent_cmd: Option<&str>,
) -> Option<String> {
    if let Some(commandline) = delegate_agent_cmd
        .map(str::trim)
        .filter(|cmd| !cmd.is_empty())
    {
        return Some(commandline.to_string());
    }

    resolve_copilot_delegate_runtime(agent_cmd)
}

fn resolve_copilot_delegate_runtime(agent_cmd: Option<&str>) -> Option<String> {
    let agent_cmd = agent_cmd?;
    let tokens = split_windows_commandline(agent_cmd);
    let command = tokens.first()?.clone();
    if !is_copilot_command(&command) {
        return None;
    }

    let mut args = vec![command.as_str()];
    let model = extract_model_from_args(&tokens[1..]);
    if let Some(model) = model.as_deref() {
        args.push("--model");
        args.push(model);
    }
    Some(join_windows_commandline(&args))
}

fn extract_model_from_args(args: &[String]) -> Option<String> {
    let mut iter = args.iter().map(String::as_str);
    while let Some(arg) = iter.next() {
        if arg == "--model" || arg == "-m" {
            if let Some(value) = iter.next() {
                let trimmed = value.trim_matches(|ch| ch == '"' || ch == '\'');
                if !trimmed.is_empty() {
                    return Some(trimmed.to_string());
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
                return Some(trimmed.to_string());
            }
        }
    }

    None
}

fn is_copilot_command(command: &str) -> bool {
    let executable = command
        .rsplit(|ch| ch == '\\' || ch == '/')
        .next()
        .unwrap_or(command);
    executable
        .strip_suffix(".exe")
        .unwrap_or(executable)
        .eq_ignore_ascii_case("copilot")
}

fn split_windows_commandline(commandline: &str) -> Vec<String> {
    let mut args = Vec::new();
    let mut current = String::new();
    let mut in_quotes = false;

    for ch in commandline.chars() {
        match ch {
            '"' => in_quotes = !in_quotes,
            ch if ch.is_whitespace() && !in_quotes => {
                if !current.is_empty() {
                    args.push(std::mem::take(&mut current));
                }
            }
            _ => current.push(ch),
        }
    }

    if !current.is_empty() {
        args.push(current);
    }

    args
}

fn required_parent<'a>(parent: Option<&'a str>, action_type: &str) -> Result<&'a str> {
    let parent = parent.context(format!(
        "field 'parent' is required for {} target panel",
        action_type
    ))?;
    ensure_non_empty("parent", parent)?;
    Ok(parent)
}

async fn send_input_to_new_pane(
    shell_mgr: &ShellManager,
    pane_id: &str,
    input: &str,
    event_tx: &mpsc::UnboundedSender<AppEvent>,
) -> Result<()> {
    ensure_non_empty("pane_id", pane_id)?;
    ensure_non_empty("input", input)?;
    coordinator_log(&format!(
        "open_and_send send_input_begin pane_id={} wait_ms=700 input_chars={} input_preview={:?}",
        pane_id,
        input.chars().count(),
        truncate_for_log(input, 120)
    ));
    let _ = event_tx.send(AppEvent::ExecutionInfo(format!(
        "Sending input to pane {}.",
        pane_id
    )));
    sleep(Duration::from_millis(700)).await;
    let result = shell_mgr
        .wt_send_input(pane_id, &format!("{input}\r"))
        .await
        .with_context(|| format!("failed to send input to pane {}", pane_id))?;
    coordinator_log(&format!(
        "open_and_send send_input_success pane_id={} response={}",
        pane_id,
        summarize_json_for_log(&result)
    ));
    let _ = event_tx.send(AppEvent::ExecutionInfo(format!(
        "Sent input to pane {}.",
        pane_id
    )));
    Ok(())
}

fn join_windows_commandline(args: &[&str]) -> String {
    args.iter()
        .map(|arg| quote_windows_commandline_arg(arg))
        .collect::<Vec<_>>()
        .join(" ")
}

fn build_delegate_startup_prompt_commandline(commandline: &str, input: &str) -> Result<String> {
    let tokens = split_windows_commandline(commandline);
    if tokens.is_empty() {
        bail!("delegate agent runtime commandline is empty");
    }

    let mut args = Vec::with_capacity(tokens.len() + 2);
    args.extend(tokens.iter().map(String::as_str));
    args.push("-i");
    args.push(input);
    Ok(join_windows_commandline(&args))
}

// Quote arguments using the standard Windows CommandLineToArgvW escaping rules.
fn quote_windows_commandline_arg(arg: &str) -> String {
    if arg.is_empty() {
        return "\"\"".to_string();
    }

    let needs_quotes = arg.chars().any(|ch| ch.is_whitespace() || ch == '"');
    if !needs_quotes {
        return arg.to_string();
    }

    let mut quoted = String::with_capacity(arg.len() + 2);
    quoted.push('"');
    let mut backslashes = 0usize;
    for ch in arg.chars() {
        match ch {
            '\\' => backslashes += 1,
            '"' => {
                quoted.push_str(&"\\".repeat(backslashes * 2 + 1));
                quoted.push('"');
                backslashes = 0;
            }
            _ => {
                if backslashes > 0 {
                    quoted.push_str(&"\\".repeat(backslashes));
                    backslashes = 0;
                }
                quoted.push(ch);
            }
        }
    }

    if backslashes > 0 {
        quoted.push_str(&"\\".repeat(backslashes * 2));
    }
    quoted.push('"');
    quoted
}

fn ensure_non_empty(field: &str, value: &str) -> Result<()> {
    if value.trim().is_empty() {
        bail!("field '{}' must not be empty", field);
    }
    Ok(())
}

fn resolve_created_pane_id(result: &serde_json::Value, action_name: &str) -> Result<String> {
    value_to_string(result.get("pane_id"))
        .filter(|pane_id| !pane_id.trim().is_empty())
        .ok_or_else(|| {
            anyhow!(
                "{} response missing pane_id: {}",
                action_name,
                summarize_json_for_log(result)
            )
        })
}

fn value_to_string(value: Option<&serde_json::Value>) -> Option<String> {
    match value {
        Some(serde_json::Value::String(s)) => Some(s.clone()),
        Some(serde_json::Value::Number(n)) => Some(n.to_string()),
        _ => None,
    }
}

fn open_target_label(target: &OpenTarget) -> &'static str {
    match target {
        OpenTarget::Tab => "tab",
        OpenTarget::Panel => "panel",
    }
}

fn delegate_prompt_delivery_label(delivery: DelegatePromptDelivery) -> &'static str {
    match delivery {
        DelegatePromptDelivery::LaunchThenSend => "launch_then_send",
        DelegatePromptDelivery::LaunchWithStartupPrompt => "launch_with_startup_prompt",
    }
}

fn summarize_json_for_log(value: &serde_json::Value) -> String {
    let json = serde_json::to_string(value).unwrap_or_else(|_| "<unserializable json>".to_string());
    truncate_for_log(&json, 512)
}

fn truncate_for_log(text: &str, max_chars: usize) -> String {
    let mut chars = text.chars();
    let truncated: String = chars.by_ref().take(max_chars).collect();
    if chars.next().is_some() {
        format!("{}...", truncated)
    } else {
        truncated
    }
}

fn coordinator_log(msg: &str) {
    use std::io::Write;

    if let Ok(mut file) = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(crate::runtime_paths::runtime_log_path("wta-acp-debug.log"))
    {
        let elapsed = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default();
        let _ = writeln!(file, "[{:.3}] {}", elapsed.as_secs_f64(), msg);
    }
}

fn extract_json_code_block(text: &str) -> Option<&str> {
    let start = text.find("```json").or_else(|| text.find("```JSON"))?;
    let after_marker = &text[start + 7..];
    let trimmed = after_marker.strip_prefix('\r').unwrap_or(after_marker);
    let trimmed = trimmed.strip_prefix('\n').unwrap_or(trimmed);
    let end = trimmed.find("```")?;
    Some(trimmed[..end].trim())
}

fn extract_first_json_object(text: &str) -> Option<&str> {
    let start = text.find('{')?;
    let end = text.rfind('}')?;
    if end <= start {
        return None;
    }
    Some(text[start..=end].trim())
}

#[cfg(test)]
mod tests {
    use super::{
        build_delegate_launch_commandline, default_delegate_agent_runtimes,
        parse_recommendation_set, resolve_created_pane_id,
        validate_recommendation_set_for_coordinator_target, DelegatePromptDelivery, OpenTarget,
        RecommendedAction,
    };
    use serde_json::json;

    #[test]
    fn default_delegate_runtime_uses_cli_default_model() {
        let runtime = default_delegate_agent_runtimes(None, None)
            .into_iter()
            .find(|runtime| runtime.id == "copilot")
            .expect("copilot runtime should exist");

        assert_eq!(runtime.commandline, "copilot");
        assert_eq!(
            runtime.prompt_delivery,
            DelegatePromptDelivery::LaunchWithStartupPrompt
        );
    }

    #[test]
    fn delegate_launch_commandline_omits_model_when_not_configured() {
        let runtime = default_delegate_agent_runtimes(None, None)
            .into_iter()
            .find(|runtime| runtime.id == "copilot")
            .expect("copilot runtime should exist");

        let commandline =
            build_delegate_launch_commandline(&runtime, "Fix the build and report back").unwrap();

        assert!(!commandline.contains("--model"));
        assert_eq!(commandline, "copilot -i \"Fix the build and report back\"");
    }

    #[test]
    fn delegate_runtime_inherits_model_from_agent_command() {
        let runtime = default_delegate_agent_runtimes(
            None,
            Some("copilot --acp --stdio --model claude-haiku-4.5"),
        )
        .into_iter()
        .find(|runtime| runtime.id == "copilot")
        .expect("copilot runtime should exist");

        assert_eq!(runtime.commandline, "copilot --model claude-haiku-4.5");
    }

    #[test]
    fn delegate_runtime_preserves_explicit_copilot_exe_path() {
        let runtime = default_delegate_agent_runtimes(None, Some(
            "\"C:\\Users\\kaitao\\AppData\\Local\\Microsoft\\WinGet\\Links\\copilot.exe\" --acp --stdio --model=claude-haiku-4.5",
        ))
        .into_iter()
        .find(|runtime| runtime.id == "copilot")
        .expect("copilot runtime should exist");

        assert_eq!(
            runtime.commandline,
            "C:\\Users\\kaitao\\AppData\\Local\\Microsoft\\WinGet\\Links\\copilot.exe --model claude-haiku-4.5"
        );
    }

    #[test]
    fn delegate_runtime_prefers_explicit_delegate_command() {
        let runtime = default_delegate_agent_runtimes(
            Some("copilot --model claude-haiku-4.5"),
            Some("copilot --acp --stdio --model gpt-5.2"),
        )
        .into_iter()
        .find(|runtime| runtime.id == "copilot")
        .expect("copilot runtime should exist");

        assert_eq!(runtime.commandline, "copilot --model claude-haiku-4.5");
    }

    #[test]
    fn delegate_launch_commandline_appends_startup_prompt_and_model() {
        let runtime = default_delegate_agent_runtimes(
            Some("copilot --model claude-haiku-4.5"),
            Some("copilot --acp --stdio --model gpt-5.2"),
        )
        .into_iter()
        .find(|runtime| runtime.id == "copilot")
        .expect("copilot runtime should exist");

        let commandline = build_delegate_launch_commandline(
            &runtime,
            "Fix the Rust build error and run cargo build",
        )
        .unwrap();

        assert_eq!(
            commandline,
            "copilot --model claude-haiku-4.5 -i \"Fix the Rust build error and run cargo build\""
        );
    }

    #[test]
    fn delegate_launch_commandline_preserves_explicit_exe_path_with_startup_prompt() {
        let runtime = default_delegate_agent_runtimes(
            Some(
                "\"C:\\Users\\kaitao\\AppData\\Local\\Microsoft\\WinGet\\Links\\copilot.exe\" --model claude-haiku-4.5",
            ),
            None,
        )
        .into_iter()
        .find(|runtime| runtime.id == "copilot")
        .expect("copilot runtime should exist");

        let commandline =
            build_delegate_launch_commandline(&runtime, "Inspect the repo and summarize").unwrap();

        assert_eq!(
            commandline,
            "C:\\Users\\kaitao\\AppData\\Local\\Microsoft\\WinGet\\Links\\copilot.exe --model claude-haiku-4.5 -i \"Inspect the repo and summarize\""
        );
    }

    #[test]
    fn parse_recommendations_accepts_open_and_send_tab_actions_without_parent() {
        let text = r#"```json
{
  "recommended_choice": 1,
  "choices": [
    {
      "choice": 1,
      "title": "Open a shell tab",
      "actions": [
        {
          "type": "open_and_send",
          "target": "tab",
          "input": "pwd",
          "cwd": "C:\\repo",
          "title": "Repo shell"
        }
      ]
    },
    {
      "choice": 2,
      "title": "Delegate in a new tab",
      "actions": [
        {
          "type": "open_and_send",
          "target": "tab",
          "input": "Inspect the repo",
          "agent": "copilot",
          "cwd": "C:\\repo",
          "title": "Copilot delegate"
        }
      ]
    },
    {
      "choice": 3,
      "title": "Run locally",
      "actions": [
        {
          "type": "send",
          "parent": "1",
          "input": "pwd"
        }
      ]
    }
  ]
}
```"#;

        let parsed = parse_recommendation_set(text).expect("recommendation set should parse");

        assert!(matches!(
            parsed.choices[0].actions[0],
            RecommendedAction::OpenAndSend {
                target: OpenTarget::Tab,
                ..
            }
        ));
        assert!(matches!(
            parsed.choices[1].actions[0],
            RecommendedAction::OpenAndSend {
                target: OpenTarget::Tab,
                ..
            }
        ));
    }

    #[test]
    fn rejects_send_to_current_coordinator_target() {
        let text = r#"```json
{
  "recommended_choice": 1,
  "choices": [
    {
      "choice": 1,
      "title": "Reply in the current pane",
      "actions": [
        {
          "type": "send",
          "parent": "14",
          "input": "Continue in this pane"
        }
      ]
    },
    {
      "choice": 2,
      "title": "Run locally",
      "actions": [
        {
          "type": "send",
          "parent": "1",
          "input": "pwd"
        }
      ]
    },
    {
      "choice": 3,
      "title": "Delegate",
      "actions": [
        {
          "type": "open_and_send",
          "target": "tab",
          "input": "Inspect the repo",
          "agent": "copilot",
          "cwd": "C:\\repo"
        }
      ]
    }
  ]
}
```"#;

        let parsed = parse_recommendation_set(text).expect("recommendation set should parse");
        let err = validate_recommendation_set_for_coordinator_target(&parsed, Some("14"))
            .expect_err("self-targeted send should be rejected");

        assert!(format!("{:#}", err).contains("send targets the current coordinator pane 14"));
    }

    #[test]
    fn rejects_open_and_send_panel_without_parent() {
        let text = r#"```json
{
  "recommended_choice": 1,
  "choices": [
    {
      "choice": 1,
      "title": "Split a panel",
      "actions": [
        {
          "type": "open_and_send",
          "target": "panel",
          "input": "pwd"
        }
      ]
    },
    {
      "choice": 2,
      "title": "Run locally",
      "actions": [
        {
          "type": "send",
          "parent": "1",
          "input": "pwd"
        }
      ]
    },
    {
      "choice": 3,
      "title": "Open a tab",
      "actions": [
        {
          "type": "open_and_send",
          "target": "tab",
          "input": "pwd"
        }
      ]
    }
  ]
}
```"#;

        let err =
            parse_recommendation_set(text).expect_err("panel without parent should be rejected");
        assert!(format!("{err:#}")
            .contains("field 'parent' is required for open_and_send target panel"));
    }

    #[test]
    fn parse_recommendations_accepts_single_choice() {
        let text = r#"```json
{
  "recommended_choice": 1,
  "choices": [
    {
      "choice": 1,
      "title": "Run locally",
      "actions": [
        {
          "type": "send",
          "parent": "1",
          "input": "pwd"
        }
      ]
    }
  ]
}
```"#;

        let parsed =
            parse_recommendation_set(text).expect("single-choice recommendation should parse");
        assert_eq!(parsed.choices.len(), 1);
        assert_eq!(parsed.choices[0].choice, 1);
    }

    #[test]
    fn parse_recommendations_rejects_four_choices() {
        let text = r#"```json
{
  "recommended_choice": 1,
  "choices": [
    {
      "choice": 1,
      "title": "One",
      "actions": [
        {
          "type": "send",
          "parent": "1",
          "input": "pwd"
        }
      ]
    },
    {
      "choice": 2,
      "title": "Two",
      "actions": [
        {
          "type": "send",
          "parent": "1",
          "input": "pwd"
        }
      ]
    },
    {
      "choice": 3,
      "title": "Three",
      "actions": [
        {
          "type": "send",
          "parent": "1",
          "input": "pwd"
        }
      ]
    },
    {
      "choice": 4,
      "title": "Four",
      "actions": [
        {
          "type": "send",
          "parent": "1",
          "input": "pwd"
        }
      ]
    }
  ]
}
```"#;

        let err =
            parse_recommendation_set(text).expect_err("four-choice recommendation should fail");
        assert!(format!("{err:#}").contains("expected 1 to 3 choices"));
    }

    #[test]
    fn resolve_created_pane_id_accepts_numeric_ids() {
        let result = json!({ "pane_id": 42 });

        let pane_id = resolve_created_pane_id(&result, "create_tab").unwrap();

        assert_eq!(pane_id, "42");
    }

    #[test]
    fn resolve_created_pane_id_rejects_missing_pane_id() {
        let result = json!({ "tab_id": "7" });

        let err = resolve_created_pane_id(&result, "create_tab")
            .expect_err("missing pane_id should fail");

        assert!(format!("{err:#}").contains("create_tab response missing pane_id"));
    }
}
