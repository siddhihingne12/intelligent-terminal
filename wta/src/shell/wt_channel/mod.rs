mod cli_channel;

pub use cli_channel::CliChannel;

// Re-export CliChannel as PipeChannel for backward compatibility.
// All callers that used PipeChannel now get CliChannel (wraps wtcli.exe).
pub use cli_channel::CliChannel as PipeChannel;

/// Connection info discovered from environment variables.
#[derive(Debug, Clone)]
pub struct ConnectionInfo {
    pub pipe_name: String,
    pub token: String,
    pub source: DiscoverySource,
}

#[derive(Debug, Clone)]
pub enum DiscoverySource {
    VtOsc,
    EnvVar,
    ComClsid,
}

/// Discover WT protocol connection info from environment variables.
/// Checks WT_COM_CLSID (COM protocol) first, then WT_PIPE_NAME (pipe fallback).
pub fn discover_connection_info() -> Option<ConnectionInfo> {
    // Prefer COM CLSID (new protocol)
    if let Ok(clsid) = std::env::var("WT_COM_CLSID") {
        return Some(ConnectionInfo {
            pipe_name: clsid,
            token: String::new(),
            source: DiscoverySource::ComClsid,
        });
    }

    // Fallback: pipe name
    if let Ok(pipe_name) = std::env::var("WT_PIPE_NAME") {
        let token = std::env::var("WT_MCP_TOKEN").unwrap_or_default();
        return Some(ConnectionInfo {
            pipe_name,
            token,
            source: DiscoverySource::EnvVar,
        });
    }

    None
}

/// Channel for communicating with the Windows Terminal protocol server.
#[async_trait::async_trait]
pub trait WtChannel: Send + Sync {
    async fn request(
        &self,
        method: &str,
        params: serde_json::Value,
    ) -> anyhow::Result<serde_json::Value>;

    fn is_available(&self) -> bool;
}
