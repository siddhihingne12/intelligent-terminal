use std::sync::OnceLock;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

fn ui_trace_enabled() -> bool {
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| match std::env::var("WTA_UI_TRACE").ok().as_deref() {
        Some("0") => false,
        Some("1") => true,
        _ => cfg!(debug_assertions) || std::env::var("WTA_DEBUG_LOG").as_deref() == Ok("1"),
    })
}

pub fn log(message: &str) {
    use std::io::Write;

    if !ui_trace_enabled() {
        return;
    }

    if let Ok(mut file) = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(crate::runtime_paths::runtime_log_path("wta-ui-debug.log"))
    {
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs_f64();
        let _ = writeln!(file, "[{timestamp:.3}] {message}");
        let _ = file.flush();
    }
}

pub fn log_slow<F>(scope: &str, elapsed: Duration, details: F)
where
    F: FnOnce() -> String,
{
    if elapsed < Duration::from_millis(75) {
        return;
    }

    log(&format!(
        "slow {scope} {:.1}ms {}",
        elapsed.as_secs_f64() * 1000.0,
        details()
    ));
}
