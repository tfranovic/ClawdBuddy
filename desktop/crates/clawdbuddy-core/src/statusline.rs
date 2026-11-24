//! The numbers Claude Code gives its status line.
//!
//! Claude Code runs the `statusLine` command from `settings.json` after each
//! reply and hands it the session's state as JSON: the context used, the
//! model, and for subscribers the plan's usage windows. `clawdbuddy hook
//! statusline` is that command: it keeps a few of those numbers in
//! `<state>/statusline/<session id>.json` and prints one short line. The app
//! reads the files here: the context for the team view's desks, the usage
//! windows as a reading that costs no request at all.

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};

/// A file not rewritten for this long says nothing about a session's
/// context any more (the session is gone, or idle past caring).
pub const NUMBERS_TTL_SECS: i64 = 6 * 3600;
/// Usage numbers older than this are not offered as a reading.
pub const USAGE_FRESH_SECS: i64 = 120;

/// One usage window as the status line has it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct StatusWindow {
    /// % used, 0-100.
    pub pct: f64,
    /// When it resets, Unix seconds.
    pub resets_at: i64,
}

/// What one session's status line last said.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct StatusNumbers {
    /// When, Unix seconds.
    pub at: i64,
    /// % of the context window used.
    pub context: Option<u8>,
    /// The model's display name.
    pub model: String,
    /// The 5-hour window.
    pub five_hour: Option<StatusWindow>,
    /// The 7-day window.
    pub seven_day: Option<StatusWindow>,
    /// The Claude Code config dir (account) the session runs under.
    pub config_dir: Option<PathBuf>,
    /// The session's cost so far, USD (Claude Code's own estimate).
    pub cost_usd: Option<f64>,
    /// Lines added and removed in the session.
    pub lines: Option<(u64, u64)>,
    /// Tokens the session has spent, as far as its status line saw
    /// ([`digest_after`]): the sum of each reply's input and output.
    pub tokens: Option<u64>,
}

fn window(v: Option<&Value>) -> Option<StatusWindow> {
    let v = v?;
    Some(StatusWindow { pct: v.get("used_percentage")?.as_f64()?, resets_at: v.get("resets_at")?.as_i64()? })
}

/// The file's text for one status-line input, and the session it is for.
/// `None` without a session id.
pub fn digest(input: &Value, now: i64) -> Option<(String, String)> {
    digest_after(input, now, None)
}

/// [`digest`], counting the session's tokens on from `prev`, the file's
/// text before this call.
///
/// Claude Code gives the latest reply's tokens only (`context_window`'s
/// `total_input_tokens`, cached context included, and
/// `total_output_tokens`), so the session's total is kept here: when the
/// pair differs from the one in `prev`, a new reply has come and its tokens
/// are added. A reply the status line never ran for is not counted.
pub fn digest_after(input: &Value, now: i64, prev: Option<&str>) -> Option<(String, String)> {
    let sid = input.get("session_id")?.as_str()?;
    let safe = !sid.is_empty() && sid.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '-' | '_'));
    if !safe {
        return None;
    }
    let ctx = input.get("context_window").and_then(|c| c.get("used_percentage")).and_then(Value::as_f64);
    let reply = |k: &str| input.get("context_window").and_then(|c| c.get(k)).and_then(Value::as_u64);
    let reply = reply("total_input_tokens").zip(reply("total_output_tokens"));
    let before: Option<Value> = prev.and_then(|t| serde_json::from_str(t).ok());
    let before = |k: &str| before.as_ref().and_then(|v| v.get(k)).and_then(Value::as_u64);
    let last = before("ti").zip(before("to"));
    let tokens = match reply {
        Some((i, o)) if Some((i, o)) != last => before("tok").unwrap_or(0).saturating_add(i).saturating_add(o),
        _ => before("tok").unwrap_or(0),
    };
    let (ti, to) = reply.or(last).unzip();
    let limits = input.get("rate_limits");
    let win = |k: &str| window(limits.and_then(|l| l.get(k))).map(|w| json!({ "pct": w.pct, "reset": w.resets_at }));
    let cost = input.get("cost");
    let dir = input
        .get("transcript_path")
        .and_then(Value::as_str)
        .map(Path::new)
        .and_then(|p| p.ancestors().find(|a| a.file_name().is_some_and(|n| n == "projects"))?.parent())
        .and_then(Path::to_str);
    let out = json!({
        "t": now,
        "ctx": ctx.map(|c| c.round().clamp(0.0, 100.0) as u8),
        "model": input.get("model").and_then(|m| m.get("display_name")).and_then(Value::as_str),
        "five": win("five_hour"),
        "week": win("seven_day"),
        "dir": dir,
        "cost": cost.and_then(|c| c.get("total_cost_usd")).and_then(Value::as_f64),
        "la": cost.and_then(|c| c.get("total_lines_added")).and_then(Value::as_u64),
        "lr": cost.and_then(|c| c.get("total_lines_removed")).and_then(Value::as_u64),
        "tok": tokens,
        "ti": ti,
        "to": to,
    });
    Some((sid.to_owned(), out.to_string() + "\n"))
}

/// The line the status line shows: the model, the context used, the plan's
/// windows.
pub fn line(input: &Value) -> String {
    let mut parts: Vec<String> = Vec::new();
    if let Some(m) = input.get("model").and_then(|m| m.get("display_name")).and_then(Value::as_str) {
        parts.push(m.to_owned());
    }
    if let Some(c) = input.get("context_window").and_then(|c| c.get("used_percentage")).and_then(Value::as_f64) {
        parts.push(format!("context {:.0}%", c));
    }
    let limits = input.get("rate_limits");
    for (key, label) in [("five_hour", "5h"), ("seven_day", "week")] {
        if let Some(w) = window(limits.and_then(|l| l.get(key))) {
            parts.push(format!("{label} {:.0}%", w.pct));
        }
    }
    parts.join(" · ")
}

/// Parse one file.
pub fn parse(text: &str) -> Option<StatusNumbers> {
    let v: Value = serde_json::from_str(text).ok()?;
    let win = |k: &str| {
        let w = v.get(k)?;
        Some(StatusWindow { pct: w.get("pct")?.as_f64()?, resets_at: w.get("reset")?.as_i64()? })
    };
    Some(StatusNumbers {
        at: v.get("t")?.as_i64()?,
        context: v.get("ctx").and_then(Value::as_u64).map(|c| c.min(100) as u8),
        model: v.get("model").and_then(Value::as_str).unwrap_or("").to_owned(),
        five_hour: win("five"),
        seven_day: win("week"),
        config_dir: v.get("dir").and_then(Value::as_str).map(PathBuf::from),
        cost_usd: v.get("cost").and_then(Value::as_f64),
        lines: v.get("la").and_then(Value::as_u64).zip(v.get("lr").and_then(Value::as_u64)),
        tokens: v.get("tok").and_then(Value::as_u64),
    })
}

/// The folder the files are in.
pub fn dir(state_dir: &Path) -> PathBuf {
    state_dir.join("statusline")
}

/// Every session's numbers, by session id. Files past [`NUMBERS_TTL_SECS`]
/// are removed.
pub fn read_all(state_dir: &Path, now: i64) -> HashMap<String, StatusNumbers> {
    let mut out = HashMap::new();
    let Ok(entries) = fs::read_dir(dir(state_dir)) else { return out };
    for e in entries.flatten() {
        let path = e.path();
        let Some(sid) = path.file_stem().and_then(|s| s.to_str()).map(str::to_owned) else { continue };
        if path.extension().is_none_or(|x| x != "json") {
            continue;
        }
        match fs::read_to_string(&path).ok().as_deref().and_then(parse) {
            Some(n) if now - n.at <= NUMBERS_TTL_SECS => {
                out.insert(sid, n);
            }
            _ => {
                let _ = fs::remove_file(&path);
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input() -> Value {
        json!({
            "session_id": "abc-123",
            "transcript_path": "/home/u/.claude/projects/-home-u-api/abc-123.jsonl",
            "model": {"id": "claude-fable-5-2", "display_name": "Fable 5.2"},
            "context_window": {"used_percentage": 41.6},
            "cost": {"total_cost_usd": 1.25, "total_lines_added": 120, "total_lines_removed": 7},
            "rate_limits": {
                "five_hour": {"used_percentage": 45.2, "resets_at": 2000},
                "seven_day": {"used_percentage": 28.0, "resets_at": 9000}
            }
        })
    }

    #[test]
    fn the_file_round_trips() {
        let (sid, text) = digest(&input(), 1000).unwrap();
        assert_eq!(sid, "abc-123");
        let n = parse(&text).unwrap();
        assert_eq!(n.at, 1000);
        assert_eq!(n.context, Some(42));
        assert_eq!(n.model, "Fable 5.2");
        assert_eq!(n.five_hour, Some(StatusWindow { pct: 45.2, resets_at: 2000 }));
        assert_eq!(n.seven_day.unwrap().resets_at, 9000);
        assert_eq!(n.config_dir.as_deref(), Some(Path::new("/home/u/.claude")));
        assert_eq!(n.lines, Some((120, 7)));
    }

    #[test]
    fn tokens_add_up_reply_by_reply() {
        let with = |i: u64, o: u64| {
            let mut v = input();
            v["context_window"]["total_input_tokens"] = json!(i);
            v["context_window"]["total_output_tokens"] = json!(o);
            v
        };
        // The first reply seen.
        let (_, a) = digest_after(&with(50_000, 800), 1, None).unwrap();
        assert_eq!(parse(&a).unwrap().tokens, Some(50_800));
        // The status line runs again for the same reply: nothing is added.
        let (_, b) = digest_after(&with(50_000, 800), 2, Some(&a)).unwrap();
        assert_eq!(parse(&b).unwrap().tokens, Some(50_800));
        // A new reply.
        let (_, c) = digest_after(&with(51_000, 300), 3, Some(&b)).unwrap();
        assert_eq!(parse(&c).unwrap().tokens, Some(102_100));
        // An input without the figures keeps the count and the last pair.
        let (_, d) = digest_after(&input(), 4, Some(&c)).unwrap();
        assert_eq!(parse(&d).unwrap().tokens, Some(102_100));
        let (_, e) = digest_after(&with(51_000, 300), 5, Some(&d)).unwrap();
        assert_eq!(parse(&e).unwrap().tokens, Some(102_100));
        // A file that does not read starts the session's count again.
        let (_, f) = digest_after(&with(10, 5), 6, Some("junk")).unwrap();
        assert_eq!(parse(&f).unwrap().tokens, Some(15));
    }

    #[test]
    fn missing_numbers_stay_unknown() {
        let (_, text) = digest(&json!({"session_id": "s1"}), 5).unwrap();
        let n = parse(&text).unwrap();
        assert_eq!((n.context, n.five_hour, n.seven_day), (None, None, None));
        assert!(digest(&json!({"session_id": "../x"}), 5).is_none());
        assert!(digest(&json!({}), 5).is_none());
    }

    #[test]
    fn the_line() {
        assert_eq!(line(&input()), "Fable 5.2 · context 42% · 5h 45% · week 28%");
        assert_eq!(line(&json!({})), "");
    }
}
