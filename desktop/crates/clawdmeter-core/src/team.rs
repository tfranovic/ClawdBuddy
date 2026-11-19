//! The agent team view's data: which sessions are up, what each one's main
//! agent is doing, and how many sub-agents it has running.
//!
//! The hook appends one line per event to `team.log` in the state dir
//! (`<ts> <event> <session> <agent|-> <detail|-> <project>`, written by the
//! app's `hooks` module). [`TeamTracker`] follows that file and folds the last
//! hour into at most [`MAX_DESKS`] sessions, sent to the device as `tm`.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use crate::attention::to_ascii;

/// Desks in one office on the device.
pub const OFFICE_DESKS: usize = 6;
/// Sessions sent: two offices, which is what fits one Bluetooth write.
pub const MAX_DESKS: usize = 12;
/// A project's name once a second office is open (to fit the write).
pub const NAME_LIMIT_CROWDED: usize = 8;
/// A project's name on a desk.
pub const NAME_LIMIT: usize = 12;
/// A main agent with no tool call for this long is thinking, not typing.
pub const THINK_AFTER_SECS: i64 = 10;
/// A sub-agent with no event for this long is gone (its stop can go missing).
pub const SUB_TTL_SECS: i64 = 600;
/// An idle main agent, or a sub-agent with no event, is on a break (at the
/// water cooler) after this long.
pub const BREAK_AFTER_SECS: i64 = 120;
/// An idle session leaves the office after this long.
pub const IDLE_TTL_SECS: i64 = 900;
/// Any session leaves after this long without an event.
pub const SESSION_TTL_SECS: i64 = 3600;

/// What a session's main agent is doing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum State {
    /// Running tools.
    Working,
    /// In a turn, no tool call lately.
    Thinking,
    /// Waiting for the user: a permission prompt or a question.
    Waiting,
    /// Its turn is over but its sub-agents are still running.
    Delegating,
    /// Between turns.
    Idle,
    /// Between turns for [`BREAK_AFTER_SECS`] or more.
    Break,
}

impl State {
    /// The letter the device reads.
    pub fn code(self) -> char {
        match self {
            State::Working => 'w',
            State::Thinking => 't',
            State::Waiting => 'q',
            State::Delegating => 'd',
            State::Idle => 'i',
            State::Break => 'b',
        }
    }
}

/// One line of `team.log`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TeamEvent {
    /// Unix seconds.
    pub ts: i64,
    /// Hook event name.
    pub event: String,
    /// Session id.
    pub session: String,
    /// Sub-agent id; `None` for the main agent.
    pub agent: Option<String>,
    /// Tool name (tool events) or notification type.
    pub detail: Option<String>,
    /// Name of the session's working directory.
    pub project: String,
}

/// Parse one `team.log` line.
pub fn parse_line(line: &str) -> Option<TeamEvent> {
    let mut it = line.trim_end().splitn(6, ' ');
    let ts = it.next()?.parse().ok()?;
    let event = it.next()?.to_owned();
    let session = it.next()?.to_owned();
    let opt = |s: &str| (s != "-" && !s.is_empty()).then(|| s.to_owned());
    let agent = opt(it.next()?);
    let detail = opt(it.next()?);
    let project = it.next().unwrap_or("").to_owned();
    (!session.is_empty() && session != "-").then_some(TeamEvent { ts, event, session, agent, detail, project })
}

/// One desk.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Desk {
    /// Stable for the session's life; unique among the desks sent.
    pub id: u8,
    /// The main agent.
    pub state: State,
    /// Sub-agents running.
    pub subs: u8,
    /// How many of them have had no event for [`BREAK_AFTER_SECS`].
    pub quiet: u8,
    /// Project name, ASCII, at most [`NAME_LIMIT`] chars.
    pub name: String,
}

#[derive(Debug, Default)]
struct Session {
    first: i64,
    last: i64,
    state: Option<State>,
    last_tool: i64,
    project: String,
    subs: BTreeMap<String, i64>,
    ended: bool,
}

/// Fold events (oldest first) into the desks to show at `now`.
pub fn desks(events: &[TeamEvent], now: i64) -> Vec<Desk> {
    let mut sessions: BTreeMap<&str, Session> = BTreeMap::new();
    for ev in events.iter().filter(|e| e.ts <= now && now - e.ts <= SESSION_TTL_SECS) {
        let s = sessions.entry(&ev.session).or_default();
        if s.first == 0 || s.ended {
            *s = Session { first: ev.ts, ..Session::default() };
        }
        s.last = ev.ts;
        if !ev.project.is_empty() {
            s.project.clone_from(&ev.project);
        }
        let name = ev.event.as_str();
        if let Some(agent) = &ev.agent {
            match name {
                "SubagentStop" => {
                    s.subs.remove(agent);
                }
                // Any other event from inside a sub-agent shows it is alive,
                // which also covers a start that went missing.
                _ => {
                    s.subs.insert(agent.clone(), ev.ts);
                }
            }
            continue;
        }
        match name {
            "UserPromptSubmit" => s.state = Some(State::Thinking),
            "PostToolUse" => {
                s.state = Some(State::Working);
                s.last_tool = ev.ts;
            }
            "PreToolUse" if ev.detail.as_deref() == Some("AskUserQuestion") => s.state = Some(State::Waiting),
            "Notification" => match ev.detail.as_deref() {
                Some("permission_prompt" | "elicitation_dialog") => s.state = Some(State::Waiting),
                Some("idle_prompt") => s.state = Some(State::Idle),
                _ => {}
            },
            // Background sub-agents outlive the turn, so they stay.
            "Stop" | "StopFailure" => s.state = Some(State::Idle),
            "SessionEnd" => s.ended = true,
            _ => {}
        }
    }

    let mut live: Vec<(&str, Session)> = sessions
        .into_iter()
        .filter(|(_, s)| !s.ended)
        .map(|(id, mut s)| {
            s.subs.retain(|_, seen| now - *seen <= SUB_TTL_SECS);
            (id, s)
        })
        .filter(|(_, s)| {
            let idle = matches!(s.state, None | Some(State::Idle)) && s.subs.is_empty();
            !(idle && now - s.last > IDLE_TTL_SECS)
        })
        .collect();
    // Too many: the busy ones first, then the most recent.
    live.sort_by_key(|(_, s)| (matches!(s.state, None | Some(State::Idle)), -s.last));
    live.truncate(MAX_DESKS);
    live.sort_by_key(|(id, s)| (s.first, *id));

    let mut out: Vec<Desk> = Vec::with_capacity(live.len());
    for (sid, s) in live {
        let state = match s.state {
            Some(State::Working) if now - s.last_tool > THINK_AFTER_SECS => State::Thinking,
            Some(State::Idle) | None if !s.subs.is_empty() => State::Delegating,
            Some(State::Idle) | None if now - s.last >= BREAK_AFTER_SECS => State::Break,
            Some(st) => st,
            None => State::Idle,
        };
        let subs = s.subs.len().min(9) as u8;
        let quiet = s.subs.values().filter(|seen| now - **seen >= BREAK_AFTER_SECS).count().min(9) as u8;
        let mut id = fnv8(sid);
        while out.iter().any(|d| d.id == id) {
            id = id.wrapping_add(1);
        }
        let name: String = to_ascii(&s.project, NAME_LIMIT).chars().filter(|c| *c != '|' && *c != '"').collect();
        out.push(Desk { id, state, subs, quiet: quiet.min(subs), name });
    }
    out
}

fn fnv8(s: &str) -> u8 {
    let h = s.bytes().fold(0x811c_9dc5_u32, |h, b| (h ^ u32::from(b)).wrapping_mul(0x0100_0193));
    (h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24)) as u8
}

/// The `tm` field: `<2 hex id><state><subs digit><quiet digit><name>` per desk, joined
/// with `|`. Empty when the office is empty.
pub fn encode(desks: &[Desk]) -> String {
    let limit = if desks.len() > OFFICE_DESKS { NAME_LIMIT_CROWDED } else { NAME_LIMIT };
    desks
        .iter()
        .map(|d| {
            let name: String = d.name.chars().take(limit).collect();
            format!("{:02x}{}{}{}{}", d.id, d.state.code(), d.subs.min(9), d.quiet.min(9), name)
        })
        .collect::<Vec<_>>()
        .join("|")
}

/// Follows `team.log` (and the rolled `team.log.1`), re-reading only when a
/// file changed.
#[derive(Debug)]
pub struct TeamTracker {
    dir: PathBuf,
    stamp: Option<[Option<(u64, SystemTime)>; 2]>,
    events: Vec<TeamEvent>,
}

impl TeamTracker {
    /// Follow the files in `dir` (the state dir).
    pub fn new(dir: impl Into<PathBuf>) -> Self {
        TeamTracker { dir: dir.into(), stamp: None, events: Vec::new() }
    }

    fn files(&self) -> [PathBuf; 2] {
        [self.dir.join("team.log.1"), self.dir.join("team.log")]
    }

    /// The `tm` field at `now`.
    pub fn poll(&mut self, now: i64) -> String {
        let stamp = self.files().map(|f| stamp_of(&f));
        if self.stamp != Some(stamp) {
            self.events.clear();
            for f in self.files() {
                if let Ok(text) = fs::read_to_string(&f) {
                    self.events.extend(text.lines().filter_map(parse_line));
                }
            }
            self.events.retain(|e| now - e.ts <= SESSION_TTL_SECS);
            self.events.sort_by_key(|e| e.ts);
            self.stamp = Some(stamp);
        }
        encode(&desks(&self.events, now))
    }
}

fn stamp_of(path: &Path) -> Option<(u64, SystemTime)> {
    let m = fs::metadata(path).ok()?;
    Some((m.len(), m.modified().ok()?))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ev(ts: i64, line: &str) -> TeamEvent {
        parse_line(&format!("{ts} {line}")).unwrap()
    }

    #[test]
    fn lines() {
        let e = ev(5, "PostToolUse s1 a7 Bash my project");
        assert_eq!(e.agent.as_deref(), Some("a7"));
        assert_eq!(e.detail.as_deref(), Some("Bash"));
        assert_eq!(e.project, "my project");
        let e = ev(5, "Stop s1 - - api");
        assert_eq!((e.agent, e.detail), (None, None));
        assert!(parse_line("x Stop s1 - - api").is_none());
        assert!(parse_line("5 Stop - - - api").is_none());
        assert!(parse_line("5 Stop").is_none());
    }

    #[test]
    fn a_session_through_a_turn() {
        let t = 1000;
        let mut evs = vec![ev(t, "UserPromptSubmit s1 - - api")];
        let one = |evs: &[TeamEvent], now| desks(evs, now).pop().unwrap();
        assert_eq!(one(&evs, t + 1).state, State::Thinking);
        assert_eq!(one(&evs, t + 1).name, "api");
        evs.push(ev(t + 5, "PostToolUse s1 - Bash api"));
        assert_eq!(one(&evs, t + 6).state, State::Working);
        assert_eq!(one(&evs, t + 5 + THINK_AFTER_SECS + 1).state, State::Thinking);
        evs.push(ev(t + 20, "Notification s1 - permission_prompt api"));
        assert_eq!(one(&evs, t + 21).state, State::Waiting);
        evs.push(ev(t + 30, "PostToolUse s1 - Bash api"));
        evs.push(ev(t + 31, "Stop s1 - - api"));
        assert_eq!(one(&evs, t + 32).state, State::Idle);
        // Idle sessions leave after a while; an ended one leaves at once.
        assert_eq!(one(&evs, t + 31 + BREAK_AFTER_SECS - 1).state, State::Idle);
        assert_eq!(one(&evs, t + 31 + BREAK_AFTER_SECS).state, State::Break, "idle for a while: a break");
        assert!(desks(&evs, t + 31 + IDLE_TTL_SECS + 1).is_empty());
        evs.push(ev(t + 40, "SessionEnd s1 - - api"));
        assert!(desks(&evs, t + 41).is_empty());
        // The same id starting again (a resume) is a new sitting.
        evs.push(ev(t + 50, "UserPromptSubmit s1 - - api"));
        assert_eq!(one(&evs, t + 51).state, State::Thinking);
    }

    #[test]
    fn sub_agents_are_counted() {
        let t = 1000;
        let mut evs = vec![
            ev(t, "UserPromptSubmit s1 - - api"),
            ev(t + 1, "SubagentStart s1 a1 Explore api"),
            ev(t + 1, "SubagentStart s1 a2 Explore api"),
            // A start that went missing: the tool call reveals the agent.
            ev(t + 2, "PostToolUse s1 a3 Read api"),
        ];
        let d = desks(&evs, t + 3);
        assert_eq!(d[0].subs, 3);
        // A sub-agent's tool call is not the main agent's.
        assert_eq!(d[0].state, State::Thinking);
        // A stop for an agent never seen (Claude Code's internal ones).
        evs.push(ev(t + 4, "SubagentStop s1 zz - api"));
        evs.push(ev(t + 5, "SubagentStop s1 a1 Explore api"));
        assert_eq!(desks(&evs, t + 6)[0].subs, 2);
        assert_eq!(desks(&evs, t + 6)[0].quiet, 0);
        // a2 was last heard at t + 1, a3 at t + 2: both quiet by now.
        assert_eq!(desks(&evs, t + 2 + BREAK_AFTER_SECS)[0].quiet, 2);
        // Silent ones time out.
        evs.push(ev(t + 400, "PostToolUse s1 a2 Read api"));
        assert_eq!(desks(&evs, t + SUB_TTL_SECS + 10)[0].subs, 1);
        // The turn ends with a sub-agent still running: the main agent waits
        // for it, neither asleep nor on a break.
        evs.push(ev(t + 500, "Stop s1 - - api"));
        let d = &desks(&evs, t + 501)[0];
        assert_eq!((d.subs, d.state), (2, State::Delegating));
        assert_eq!(desks(&evs, t + 500 + BREAK_AFTER_SECS)[0].state, State::Delegating);
        evs.push(ev(t + 700, "SubagentStop s1 a2 Explore api"));
        // The last one stops (a3 timed out meanwhile): now it is idle.
        assert_eq!(desks(&evs, t + 701)[0].state, State::Idle);
    }

    #[test]
    fn at_most_twelve_desks_busy_first_with_unique_ids() {
        let t = 1000;
        let mut evs = Vec::new();
        for i in 0..14 {
            evs.push(ev(t + i, &format!("UserPromptSubmit s{i} - - p{i}")));
        }
        evs.push(ev(t + 20, "Stop s7 - - p7"));
        evs.push(ev(t + 21, "Stop s6 - - p6"));
        // Seven sessions open a second office; names get shorter to fit.
        let seven = desks(&evs[..7], t + 10);
        let tm = encode(&seven);
        assert_eq!(tm.split('|').count(), 7);
        let long = [ev(t, "UserPromptSubmit x - - a-long-project")];
        assert!(encode(&desks(&long, t + 1)).ends_with("a-long-pr..."));
        let mut crowd = seven.clone();
        crowd[0].name = "a-long-pr...".into();
        assert!(encode(&crowd).starts_with(&format!("{:02x}t00a-long-p|", crowd[0].id)));
        assert!(encode(&vec![crowd[0].clone(); MAX_DESKS]).len() <= 170);
        let d = desks(&evs, t + 30);
        assert_eq!(d.len(), MAX_DESKS);
        assert!(d.iter().all(|x| x.state == State::Thinking));
        let mut ids: Vec<u8> = d.iter().map(|x| x.id).collect();
        ids.sort_unstable();
        ids.dedup();
        assert_eq!(ids.len(), MAX_DESKS);
        // Seated in arrival order.
        assert_eq!(d[0].name, "p0");
    }

    #[test]
    fn the_field() {
        let d = vec![
            Desk { id: 0xa3, state: State::Working, subs: 2, quiet: 1, name: "magnemite".into() },
            Desk { id: 7, state: State::Break, subs: 0, quiet: 0, name: "Clawdmeter".into() },
        ];
        assert_eq!(encode(&d), "a3w21magnemite|07b00Clawdmeter");
        assert_eq!(encode(&[]), "");
        let evs = [ev(10, "UserPromptSubmit s1 - - a-very-long-project-name|x")];
        let name = &desks(&evs, 11)[0].name;
        assert!(name.len() <= NAME_LIMIT && !name.contains('|'), "{name}");
    }

    #[test]
    fn tracker_follows_the_file() {
        let dir = std::env::temp_dir().join(format!("cm-team-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let mut t = TeamTracker::new(&dir);
        assert_eq!(t.poll(100), "");
        fs::write(dir.join("team.log"), "90 UserPromptSubmit s1 - - api\n").unwrap();
        let tm = t.poll(100);
        assert!(tm.ends_with("t00api"), "{tm}");
        fs::write(dir.join("team.log"), "90 UserPromptSubmit s1 - - api\n95 SubagentStart s1 a1 Explore api\n").unwrap();
        assert!(t.poll(100).ends_with("t10api"));
        let _ = fs::remove_dir_all(&dir);
    }
}
