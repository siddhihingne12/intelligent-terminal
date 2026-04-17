use ratatui::prelude::*;
use ratatui::widgets::{Block, Borders, Paragraph, Wrap};

use crate::app::App;
use crate::preflight::CheckStatus;

const TITLE_STYLE: Style = Style::new()
    .fg(Color::Cyan)
    .add_modifier(Modifier::BOLD);

const LABEL_STYLE: Style = Style::new().fg(Color::White);
const HINT_STYLE: Style = Style::new().fg(Color::DarkGray);
const PASS_STYLE: Style = Style::new().fg(Color::Green);
const FAIL_STYLE: Style = Style::new().fg(Color::Red);
const SKIP_STYLE: Style = Style::new().fg(Color::DarkGray);
const CHECK_STYLE: Style = Style::new().fg(Color::Yellow);
const SELECTED_INDICATOR: Style = Style::new()
    .fg(Color::Yellow)
    .add_modifier(Modifier::BOLD);

pub fn render(frame: &mut Frame, app: &App, area: Rect) {
    let setup = match &app.setup {
        Some(s) => s,
        None => return,
    };
    let pf = &setup.preflight;

    let mut lines: Vec<Line> = Vec::new();

    // Title
    lines.push(Line::default());
    lines.push(Line::from(Span::styled(
        "  AI Assistant Setup",
        TITLE_STYLE,
    )));
    lines.push(Line::default());

    // Agent identity
    lines.push(Line::from(vec![
        Span::styled("  Agent: ", HINT_STYLE),
        Span::styled(&pf.display_name, LABEL_STYLE),
    ]));
    lines.push(Line::default());

    // ── Check 1: CLI installed ──
    let cli_indicator = if setup.selected_index == 0 { ">" } else { " " };
    let cli_indicator_style = if setup.selected_index == 0 {
        SELECTED_INDICATOR
    } else {
        HINT_STYLE
    };

    let (cli_icon, cli_icon_style, cli_detail) = match &pf.cli_status {
        CheckStatus::Passed => {
            let detail = match &pf.cli_path {
                Some(path) => format!("Found at {}", path),
                None => "Installed".to_string(),
            };
            ("✓", PASS_STYLE, detail)
        }
        CheckStatus::Failed(reason) => ("✗", FAIL_STYLE, reason.clone()),
        CheckStatus::Checking => ("⠋", CHECK_STYLE, "Checking...".to_string()),
        CheckStatus::Skipped => ("-", SKIP_STYLE, "Skipped".to_string()),
    };

    lines.push(Line::from(vec![
        Span::styled(format!("  {} ", cli_indicator), cli_indicator_style),
        Span::styled(cli_icon, cli_icon_style),
        Span::styled(
            format!(" {} CLI", pf.agent_id),
            LABEL_STYLE,
        ),
        Span::styled(format!("  {}", cli_detail), HINT_STYLE),
    ]));

    // Show install hint if CLI not found
    if matches!(pf.cli_status, CheckStatus::Failed(_)) {
        if !pf.install_hint.is_empty() {
            for hint_line in pf.install_hint.lines() {
                lines.push(Line::from(vec![
                    Span::styled("      ", HINT_STYLE),
                    Span::styled(format!("Install: {}", hint_line), HINT_STYLE),
                ]));
            }
        }
        if !pf.install_url.is_empty() {
            lines.push(Line::from(vec![
                Span::styled("      ", HINT_STYLE),
                Span::styled(
                    format!("  Info: {}", pf.install_url),
                    HINT_STYLE,
                ),
            ]));
            if setup.selected_index == 0 {
                lines.push(Line::from(Span::styled(
                    "      [Press Enter to open install page]",
                    CHECK_STYLE,
                )));
            }
        }
    }

    lines.push(Line::default());

    // ── Check 2: Authentication ──
    let auth_indicator = if setup.selected_index == 1 { ">" } else { " " };
    let auth_indicator_style = if setup.selected_index == 1 {
        SELECTED_INDICATOR
    } else {
        HINT_STYLE
    };

    let (auth_icon, auth_icon_style, auth_detail) = match &pf.auth_status {
        CheckStatus::Passed => ("✓", PASS_STYLE, "Authenticated".to_string()),
        CheckStatus::Failed(reason) => ("✗", FAIL_STYLE, reason.clone()),
        CheckStatus::Checking => ("⠋", CHECK_STYLE, "Checking...".to_string()),
        CheckStatus::Skipped => {
            let reason = if matches!(pf.cli_status, CheckStatus::Failed(_)) {
                "(requires CLI first)".to_string()
            } else {
                "(not required)".to_string()
            };
            ("-", SKIP_STYLE, reason)
        }
    };

    lines.push(Line::from(vec![
        Span::styled(format!("  {} ", auth_indicator), auth_indicator_style),
        Span::styled(auth_icon, auth_icon_style),
        Span::styled(" Authentication", LABEL_STYLE),
        Span::styled(format!("  {}", auth_detail), HINT_STYLE),
    ]));

    // Show auth hint if failed
    if matches!(pf.auth_status, CheckStatus::Failed(_)) && !pf.auth_hint.is_empty() {
        for hint_line in pf.auth_hint.lines() {
            lines.push(Line::from(vec![
                Span::styled("      ", HINT_STYLE),
                Span::styled(hint_line.to_string(), HINT_STYLE),
            ]));
        }
    }

    lines.push(Line::default());

    // ── Separator and actions ──
    let separator_width = area.width.saturating_sub(4) as usize;
    lines.push(Line::from(Span::styled(
        format!("  {}", "─".repeat(separator_width.min(40))),
        HINT_STYLE,
    )));

    // Tell the user to restart the terminal after fixing.
    lines.push(Line::from(Span::styled(
        "  After fixing, close and reopen Windows Terminal.",
        HINT_STYLE,
    )));

    let block = Block::default().borders(Borders::NONE);
    let paragraph = Paragraph::new(lines)
        .block(block)
        .wrap(Wrap { trim: false });

    frame.render_widget(paragraph, area);
}
