//! Claude Code's own list of running sessions.
//!
//! Claude Code keeps one small file per running session in
//! `<config dir>/sessions/<pid>.json`, the data behind `claude agents
//! --json`: the process id, the session id, and whether the session is busy,
//! idle or waiting for the user. The files' format is not documented, so
//! everything here is optional: a file that doesn't read as expected is
//! skipped, and the team view then goes by the hooks alone.

use std::collections::HashMap;
use std::fs;
use std::path::Path;

use serde_json::Value;

/// What Claude Code says a session is doing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RunStatus {
    /// In a turn.
    Busy,
    /// Between turns.
    Idle,
    /// Waiting for the user.
    Waiting,
    /// Anything else.
    Unknown,
}

/// One running session.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Running {
    /// Its process still exists.
    pub alive: bool,
    /// Its status.
    pub status: RunStatus,
    /// When the status last changed, Unix seconds.
    pub status_at: i64,
}

/// Parse one file: the session id and what it says. `alive` is left true.
pub fn parse(text: &str) -> Option<(String, u32, Running)> {
    let v: Value = serde_json::from_str(text).ok()?;
    let sid = v.get("sessionId")?.as_str()?.to_owned();
    let pid = u32::try_from(v.get("pid")?.as_u64()?).ok()?;
    let status = match v.get("status").and_then(Value::as_str) {
        Some("busy") => RunStatus::Busy,
        Some("idle") => RunStatus::Idle,
        Some("waiting") => RunStatus::Waiting,
        _ => RunStatus::Unknown,
    };
    let status_at = v.get("statusUpdatedAt").and_then(Value::as_i64).unwrap_or(0) / 1000;
    Some((sid, pid, Running { alive: true, status, status_at }))
}

/// Whether a process exists.
#[cfg(unix)]
pub fn process_alive(pid: u32) -> bool {
    let Ok(pid) = libc::pid_t::try_from(pid) else { return true };
    // SAFETY: signal 0 only checks that the process exists; nothing is sent.
    let r = unsafe { libc::kill(pid, 0) };
    // No permission to signal it still means it exists.
    r == 0 || std::io::Error::last_os_error().raw_os_error() == Some(libc::EPERM)
}

/// Whether a process exists: not checked here, so every session counts as
/// running and leaves by the hooks' rules.
#[cfg(not(unix))]
pub fn process_alive(_pid: u32) -> bool {
    true
}

/// The sessions listed under each config dir, by session id.
pub fn read_all<P: AsRef<Path>>(config_dirs: &[P]) -> HashMap<String, Running> {
    let mut out = HashMap::new();
    for dir in config_dirs {
        let Ok(entries) = fs::read_dir(dir.as_ref().join("sessions")) else { continue };
        for e in entries.flatten() {
            let path = e.path();
            if path.extension().is_none_or(|x| x != "json") {
                continue;
            }
            let Some((sid, pid, mut r)) = fs::read_to_string(&path).ok().as_deref().and_then(parse) else { continue };
            r.alive = process_alive(pid);
            out.insert(sid, r);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_file() {
        let (sid, pid, r) =
            parse(r#"{"pid":42,"sessionId":"s1","status":"waiting","statusUpdatedAt":5000,"waitingFor":"x"}"#).unwrap();
        assert_eq!((sid.as_str(), pid), ("s1", 42));
        assert_eq!((r.status, r.status_at), (RunStatus::Waiting, 5));
        assert_eq!(parse(r#"{"pid":42,"sessionId":"s1"}"#).unwrap().2.status, RunStatus::Unknown);
        assert!(parse(r#"{"sessionId":"s1"}"#).is_none());
        assert!(parse("nope").is_none());
    }

    #[test]
    fn this_process_is_alive() {
        assert!(process_alive(std::process::id()));
    }
}
