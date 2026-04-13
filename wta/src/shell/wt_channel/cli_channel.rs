use std::sync::atomic::{AtomicBool, Ordering};

use anyhow::{bail, Context};
use tokio::sync::mpsc;

use crate::app::DebugMessage;
use super::WtChannel;

/// Channel that invokes `wtcli.exe` for protocol operations.
/// Replaces the old PipeChannel (named-pipe transport).
pub struct CliChannel {
    available: AtomicBool,
    debug_tx: Option<mpsc::UnboundedSender<DebugMessage>>,
    event_tx: std::sync::Mutex<Option<mpsc::UnboundedSender<serde_json::Value>>>,
}

impl CliChannel {
    pub async fn connect() -> anyhow::Result<Self> {
        // WT_COM_CLSID must be set — wtcli reads it from the environment.
        if std::env::var("WT_COM_CLSID").is_err() && std::env::var("WT_PIPE_NAME").is_err() {
            bail!("Neither WT_COM_CLSID nor WT_PIPE_NAME set. Must run inside a Windows Terminal pane.");
        }

        Ok(Self {
            available: AtomicBool::new(true),
            debug_tx: None,
            event_tx: std::sync::Mutex::new(None),
        })
    }

    pub async fn connect_with(pipe_name: &str, _token: &str) -> anyhow::Result<Self> {
        // For backward compat: pipe_name may be a COM CLSID or an actual pipe name.
        // Either way, wtcli handles it via its own environment.
        if pipe_name.is_empty() {
            bail!("Empty connection identifier");
        }

        Ok(Self {
            available: AtomicBool::new(true),
            debug_tx: None,
            event_tx: std::sync::Mutex::new(None),
        })
    }

    pub fn with_debug_sender(mut self, tx: mpsc::UnboundedSender<DebugMessage>) -> Self {
        self.debug_tx = Some(tx);
        self
    }

    pub fn subscribe_events(&self) -> mpsc::UnboundedReceiver<serde_json::Value> {
        let (tx, rx) = mpsc::unbounded_channel();
        *self.event_tx.lock().unwrap() = Some(tx);
        rx
    }

    /// Start background event listener (wraps `wtcli listen --json`).
    pub async fn start_reader(self: &std::sync::Arc<Self>) {
        let weak = std::sync::Arc::downgrade(self);
        tokio::spawn(async move {
            let Ok(mut child) = tokio::process::Command::new("wtcli")
                .args(["listen", "--json"])
                .stdout(std::process::Stdio::piped())
                .stderr(std::process::Stdio::null())
                .spawn()
            else {
                return;
            };

            let stdout = child.stdout.take().unwrap();
            let mut reader = tokio::io::BufReader::new(stdout);
            let mut line = String::new();

            loop {
                line.clear();
                use tokio::io::AsyncBufReadExt;
                match reader.read_line(&mut line).await {
                    Ok(0) => break,
                    Ok(_) => {
                        let Some(this) = weak.upgrade() else { break };
                        if let Ok(val) = serde_json::from_str::<serde_json::Value>(line.trim()) {
                            let tx = this.event_tx.lock().unwrap();
                            if let Some(tx) = tx.as_ref() {
                                let _ = tx.send(val);
                            }
                        }
                    }
                    Err(_) => break,
                }
            }
        });
    }

    /// Run a wtcli subcommand and return the parsed JSON output.
    async fn run_wtcli(&self, args: &[&str]) -> anyhow::Result<serde_json::Value> {
        let output = tokio::process::Command::new("wtcli")
            .args(args)
            .arg("--json")
            .output()
            .await
            .context("Failed to run wtcli")?;

        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr);
            bail!("wtcli failed: {}", stderr.trim());
        }

        let stdout = String::from_utf8_lossy(&output.stdout);
        let val: serde_json::Value = serde_json::from_str(stdout.trim())
            .context("Failed to parse wtcli JSON output")?;
        Ok(val)
    }
}

#[async_trait::async_trait]
impl WtChannel for CliChannel {
    async fn request(
        &self,
        method: &str,
        params: serde_json::Value,
    ) -> anyhow::Result<serde_json::Value> {
        // Map protocol method names to wtcli subcommands + args.
        match method {
            "list_windows" => self.run_wtcli(&["list-windows"]).await,
            "list_tabs" => {
                let mut args = vec!["list-tabs"];
                let wid = params.get("window_id").and_then(|v| v.as_str()).unwrap_or("");
                let wid_owned;
                if !wid.is_empty() {
                    wid_owned = wid.to_string();
                    args.extend(["-w", &wid_owned]);
                }
                self.run_wtcli(&args).await
            }
            "list_panes" => {
                let mut args = vec!["list-panes"];
                let wid = params.get("window_id").and_then(|v| v.as_str()).unwrap_or("");
                let tid = params.get("tab_id").and_then(|v| v.as_str()).unwrap_or("");
                let wid_owned;
                let tid_owned;
                if !wid.is_empty() {
                    wid_owned = wid.to_string();
                    args.extend(["-w", &wid_owned]);
                }
                if !tid.is_empty() {
                    tid_owned = tid.to_string();
                    args.extend(["-t", &tid_owned]);
                }
                self.run_wtcli(&args).await
            }
            "get_active_pane" => self.run_wtcli(&["active-pane"]).await,
            "read_pane_output" => {
                let pane_id = params.get("pane_id").and_then(|v| v.as_str()).unwrap_or("");
                let max_lines = params.get("max_lines").and_then(|v| v.as_i64()).unwrap_or(200);
                let pane_owned = pane_id.to_string();
                let lines_owned = max_lines.to_string();
                let mut args = vec!["capture-pane"];
                if !pane_owned.is_empty() {
                    args.extend(["-t", &pane_owned]);
                }
                args.extend(["-l", &lines_owned]);
                self.run_wtcli(&args).await
            }
            "get_process_status" => {
                let pane_id = params.get("pane_id").and_then(|v| v.as_str()).unwrap_or("");
                let pane_owned = pane_id.to_string();
                let mut args = vec!["pane-status"];
                if !pane_owned.is_empty() {
                    args.extend(["-t", &pane_owned]);
                }
                self.run_wtcli(&args).await
            }
            "create_tab" => {
                let mut args = vec!["new-tab"];
                let cmd = params.get("commandline").and_then(|v| v.as_str()).unwrap_or("");
                let title = params.get("title").and_then(|v| v.as_str()).unwrap_or("");
                let cmd_owned;
                let title_owned;
                if !cmd.is_empty() {
                    cmd_owned = cmd.to_string();
                    args.extend(["-c", &cmd_owned]);
                }
                if !title.is_empty() {
                    title_owned = title.to_string();
                    args.extend(["-n", &title_owned]);
                }
                self.run_wtcli(&args).await
            }
            "split_pane" => {
                let pane_id = params.get("pane_id").and_then(|v| v.as_str()).unwrap_or("");
                let cmd = params.get("commandline").and_then(|v| v.as_str()).unwrap_or("");
                let dir = params.get("direction").and_then(|v| v.as_str()).unwrap_or("");
                let pane_owned = pane_id.to_string();
                let cmd_owned;
                let mut args = vec!["split-pane"];
                if !pane_owned.is_empty() {
                    args.extend(["-t", &pane_owned]);
                }
                if dir == "horizontal" || dir == "down" || dir == "up" {
                    args.push("-H");
                } else {
                    args.push("-v");
                }
                if !cmd.is_empty() {
                    cmd_owned = cmd.to_string();
                    args.extend(["-c", &cmd_owned]);
                }
                self.run_wtcli(&args).await
            }
            "close_pane" => {
                let pane_id = params.get("pane_id").and_then(|v| v.as_str()).unwrap_or("");
                let pane_owned = pane_id.to_string();
                self.run_wtcli(&["kill-pane", "-t", &pane_owned]).await
            }
            "send_input" => {
                let pane_id = params.get("pane_id").and_then(|v| v.as_str()).unwrap_or("");
                let text = params.get("text").and_then(|v| v.as_str()).unwrap_or("");
                let pane_owned = pane_id.to_string();
                let text_owned = text.to_string();
                let mut args = vec!["send-keys"];
                if !pane_owned.is_empty() {
                    args.extend(["-t", &pane_owned]);
                }
                args.push(&text_owned);
                self.run_wtcli(&args).await
            }
            "get_capabilities" => self.run_wtcli(&["info"]).await,
            "quick_pick" => {
                let title = params.get("title").and_then(|v| v.as_str()).unwrap_or("");
                let title_owned = title.to_string();
                let mut args = vec!["quick-pick"];
                if !title_owned.is_empty() {
                    args.extend(["--title", &title_owned]);
                }
                if let Some(choices) = params.get("choices").and_then(|v| v.as_array()) {
                    for c in choices {
                        if let Some(s) = c.as_str() {
                            args.push(s);
                        }
                    }
                }
                self.run_wtcli(&args).await
            }
            other => bail!("Unsupported method: {}", other),
        }
    }

    fn is_available(&self) -> bool {
        self.available.load(Ordering::Relaxed)
    }
}
