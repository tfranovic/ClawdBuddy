//! The agent team view's data: which sessions are up, what each one's main
//! agent is doing, and how many sub-agents it has running.
//!
//! The hook appends one line per event to `team.log` in the state dir
//! (`<ts> <event> <session> <agent|-> <detail|-> [+extras] <project>`, written
//! by the app's `hooks` module). [`TeamTracker`] follows that file and folds
//! the last hour into at most [`MAX_DESKS`] sessions, sent to the device as
//! `tm` (what firmware 0.1 reads) and `tx` (the finer state, the context
//! used, the permission mode and the model).
//!
//! Two more sources sharpen it when they are there: the numbers Claude Code
//! gives its status line ([`crate::statusline`]: the context used, the model)
//! and its own list of running sessions ([`crate::sessions`]: which are still
//! alive, and whether each is busy, idle or waiting).

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use crate::attention::to_ascii;
use crate::sessions::{RunStatus, Running};
use crate::statusline::StatusNumbers;

/// Desks in one office on the device.
pub const OFFICE_DESKS: usize = 6;
/// Sessions sent: two offices, which is what fits one Bluetooth write.
pub const MAX_DESKS: usize = 12;
/// A project's name once a second office is open (to fit the write).
pub const NAME_LIMIT_CROWDED: usize = 8;
/// A project's name on a desk.
pub const NAME_LIMIT: usize = 12;
/// A session's title, as the detail card shows it.
pub const TITLE_LIMIT: usize = 40;
/// With no tool running, a main agent whose last tool finished this long ago
/// is thinking, not typing.
pub const THINK_AFTER_SECS: i64 = 10;
/// A sub-agent with no event for this long is gone (its stop can go missing).
pub const SUB_TTL_SECS: i64 = 600;
/// A sub-agent with no event for this long is on a break (at the water
/// cooler); so is a main agent that has been idle this long without having
/// finished a turn.
pub const BREAK_AFTER_SECS: i64 = 120;
/// A finished turn shows as done (your move) for this long, then the main
/// agent dozes.
pub const NAP_AFTER_SECS: i64 = 180;
/// ... and goes for a break this long after the turn ended.
pub const DONE_BREAK_AFTER_SECS: i64 = 360;
/// In a turn with no tool running and no event at all for this long, the
/// turn is taken as over (an interrupt has no event of its own).
pub const STALE_TURN_SECS: i64 = 300;
/// Compaction that never reported its end is over after this long.
pub const COMPACT_TTL_SECS: i64 = 120;
/// A desk keeps the state it shows at least this long ([`TeamTracker`]).
pub const MIN_SHOW_SECS: i64 = 8;
/// An idle session leaves the office after this long.
pub const IDLE_TTL_SECS: i64 = 900;
/// Any session leaves after this long without an event.
pub const SESSION_TTL_SECS: i64 = 3600;

/// What a session's main agent is doing.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum State {
    /// A tool is running, or one just finished.
    Working,
    /// In a turn, no tool running.
    Thinking,
    /// Waiting for the user: a permission prompt or a question.
    Waiting,
    /// Waiting for its sub-agents: in a turn (a foreground agent call), or
    /// after it with some still running.
    Delegating,
    /// Compacting its context.
    Compacting,
    /// Its turn just ended: your move.
    Done,
    /// Its turn ended on an API error.
    Failed,
    /// Between turns.
    #[default]
    Idle,
    /// Between turns for a while.
    Break,
}

impl State {
    /// The letter the device reads in `tx`.
    pub fn code(self) -> char {
        match self {
            State::Working => 'w',
            State::Thinking => 't',
            State::Waiting => 'q',
            State::Delegating => 'd',
            State::Compacting => 'c',
            State::Done => 'f',
            State::Failed => 'e',
            State::Idle => 'i',
            State::Break => 'b',
        }
    }

    /// The letter in `tm`, which firmware 0.1 reads: it knows six states.
    pub fn coarse(self) -> char {
        match self {
            State::Compacting => 't',
            State::Done | State::Failed => 'i',
            other => other.code(),
        }
    }

    /// For the detail card.
    pub fn words(self) -> &'static str {
        match self {
            State::Working => "Working",
            State::Thinking => "Thinking",
            State::Waiting => "Waiting for you",
            State::Delegating => "Waiting for sub-agents",
            State::Compacting => "Compacting",
            State::Done => "Done",
            State::Failed => "Stopped on an error",
            State::Idle => "Idle",
            State::Break => "On a break",
        }
    }

    fn resting(self) -> bool {
        matches!(self, State::Done | State::Failed | State::Idle | State::Break)
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
    /// Tool name (tool events), notification type or sub-agent type.
    pub detail: Option<String>,
    /// `+k=v,k=v`: the permission mode `pm`, model `md`, title `tt`,
    /// background agents at a Stop `bg`, an interrupted tool `int`, a
    /// StopFailure's error `er`.
    pub extras: Vec<(String, String)>,
    /// Name of the session's working directory.
    pub project: String,
}

impl TeamEvent {
    fn extra(&self, key: &str) -> Option<&str> {
        self.extras.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str())
    }
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
    let mut rest = it.next().unwrap_or("");
    let mut extras = Vec::new();
    if let Some(x) = rest.strip_prefix('+') {
        let (list, project) = x.split_once(' ').unwrap_or((x, ""));
        extras =
            list.split(',').filter_map(|kv| kv.split_once('=')).map(|(k, v)| (k.to_owned(), v.to_owned())).collect();
        rest = project;
    }
    (!session.is_empty() && session != "-").then_some(TeamEvent {
        ts,
        event,
        session,
        agent,
        detail,
        extras,
        project: rest.to_owned(),
    })
}

/// What the detail card says about a desk.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Detail {
    /// The session's title, or its project when it has none.
    pub title: String,
    /// The project (working directory name).
    pub project: String,
    /// Seconds in the current state.
    pub since: i64,
    /// The tool running now, else the last one.
    pub tool: String,
    /// The model's name, as Claude Code shows it, or its id.
    pub model: String,
    /// The permission mode, when not the default.
    pub mode: String,
    /// Sub-agents by type, most first.
    pub sub_kinds: Vec<(String, u8)>,
    /// Tasks completed and created.
    pub tasks: (u32, u32),
    /// Lines added and removed in the session, from the status line.
    pub lines: Option<(u64, u64)>,
}

/// One desk.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Desk {
    /// Stable for the session's life; unique among the desks sent.
    pub id: u8,
    /// The main agent.
    pub state: State,
    /// Sub-agents running.
    pub subs: u8,
    /// How many of them have had no event for [`BREAK_AFTER_SECS`].
    pub quiet: u8,
    /// The desk's label: the session's title, else its project. ASCII, at
    /// most [`NAME_LIMIT`] chars.
    pub name: String,
    /// % of the context window used, when the status line reports it.
    pub context: Option<u8>,
    /// The permission mode's letter in `tx` (`-` default).
    pub mode: char,
    /// The model's letter in `tx` (`-` unknown).
    pub model: char,
    /// The session is held up by the user: a question or a permission
    /// prompt is up, or its turn ended (or failed) within
    /// [`IDLE_TTL_SECS`] and nobody has picked it up. The pet's mood.
    pub blocked: bool,
    /// For the detail card.
    pub detail: Detail,
}

#[derive(Debug, Default)]
struct Sub {
    seen: i64,
    kind: String,
}

#[derive(Debug, Default)]
struct Session {
    first: i64,
    last: i64,
    /// The main agent's last event.
    last_main: i64,
    state: Option<State>,
    /// When the state last changed hands.
    since: i64,
    last_tool: i64,
    last_tool_name: String,
    /// Tools started and not yet finished.
    open: Vec<String>,
    compacting: Option<i64>,
    /// A sub-agent is compacting (a `PreCompact` that was not the main
    /// agent's): the `SessionStart` that ends it is the sub-agent's too.
    sub_compacting: bool,
    project: String,
    title: String,
    mode: String,
    model: String,
    tasks: (u32, u32),
    subs: BTreeMap<String, Sub>,
    ended: bool,
}

impl Session {
    fn set(&mut self, state: State, ts: i64) {
        if self.state != Some(state) {
            self.since = ts;
        }
        self.state = Some(state);
    }
}

/// What else is known about the sessions, beside the hook log.
#[derive(Debug, Default, Clone)]
pub struct Beside {
    /// The status line's numbers, by session id.
    pub status: HashMap<String, StatusNumbers>,
    /// Claude Code's running sessions, by session id.
    pub running: HashMap<String, Running>,
}

fn spawns_agents(tool: &str) -> bool {
    matches!(tool, "Agent" | "Task")
}

fn mode_letter(mode: &str) -> char {
    match mode {
        "plan" => 'p',
        "acceptEdits" => 'e',
        "auto" => 'a',
        "bypassPermissions" => 'y',
        "dontAsk" => 'n',
        _ => '-',
    }
}

fn mode_words(mode: &str) -> &'static str {
    match mode {
        "plan" => "plan mode",
        "acceptEdits" => "accepting edits",
        "auto" => "auto mode",
        "bypassPermissions" => "no permission prompts",
        "dontAsk" => "don't ask",
        _ => "",
    }
}

fn model_letter(model: &str) -> char {
    let m = model.to_ascii_lowercase();
    [("fable", 'f'), ("mythos", 'm'), ("opus", 'o'), ("sonnet", 's'), ("haiku", 'h')]
        .iter()
        .find(|(name, _)| m.contains(name))
        .map_or('-', |(_, c)| *c)
}

/// Fold events (oldest first) into the desks to show at `now`.
pub fn desks(events: &[TeamEvent], now: i64) -> Vec<Desk> {
    desks_with(events, now, &Beside::default())
}

/// [`desks`], with the status line's numbers and Claude Code's list of
/// running sessions.
pub fn desks_with(events: &[TeamEvent], now: i64, beside: &Beside) -> Vec<Desk> {
    desks_seated(events, now, beside, &HashMap::new())
}

/// [`desks_with`], seated by when each session was first seen (`arrived`,
/// by session id) rather than by its oldest event still in the window, which
/// moves as events age out and would have busy sessions swap desks.
fn desks_seated(events: &[TeamEvent], now: i64, beside: &Beside, arrived: &HashMap<String, i64>) -> Vec<Desk> {
    let mut sessions: BTreeMap<&str, Session> = BTreeMap::new();
    for ev in events.iter().filter(|e| e.ts <= now && now - e.ts <= SESSION_TTL_SECS) {
        let s = sessions.entry(&ev.session).or_default();
        if s.first == 0 || s.ended {
            *s = Session { first: ev.ts, since: ev.ts, ..Session::default() };
        }
        s.last = ev.ts;
        // A sub-agent may work in a folder of its own (a worktree): the
        // session's project is the main agent's, unless nothing else is known.
        if !ev.project.is_empty() && (ev.agent.is_none() || s.project.is_empty()) {
            s.project.clone_from(&ev.project);
        }
        let name = ev.event.as_str();
        match name {
            "TaskCreated" => s.tasks.1 += 1,
            "TaskCompleted" => s.tasks.0 += 1,
            _ => {}
        }
        if let Some(agent) = &ev.agent {
            match name {
                "SubagentStop" => {
                    s.subs.remove(agent);
                }
                // Any other event from inside a sub-agent shows it is alive,
                // which also covers a start that went missing.
                _ => {
                    let sub = s.subs.entry(agent.clone()).or_default();
                    sub.seen = ev.ts;
                    if name == "SubagentStart" {
                        sub.kind = ev.detail.clone().unwrap_or_default();
                    }
                }
            }
            continue;
        }
        // A compaction carries no agent id, a sub-agent's included. It is a
        // sub-agent's when there are some and the main agent is not in a
        // turn of its own (resting, or held in the call that runs them).
        if name == "PreCompact" {
            let subs_live = s.subs.values().any(|sub| ev.ts - sub.seen <= SUB_TTL_SECS);
            let own_turn =
                matches!(s.state, Some(State::Working | State::Thinking)) && !s.open.iter().any(|t| spawns_agents(t));
            s.sub_compacting = subs_live && !own_turn;
        }
        if s.sub_compacting && matches!(name, "PreCompact" | "PostCompact" | "SessionStart") {
            if name == "PostCompact" {
                s.sub_compacting = false;
            }
            continue;
        }
        s.sub_compacting = false;
        s.last_main = ev.ts;
        if let Some(m) = ev.extra("pm") {
            s.mode = m.to_owned();
        }
        if let Some(m) = ev.extra("md") {
            s.model = m.to_owned();
        }
        if let Some(t) = ev.extra("tt") {
            s.title = t.replace('_', " ");
        }
        if name != "PreCompact" {
            s.compacting = None;
        }
        let tool = ev.detail.as_deref().unwrap_or("");
        match name {
            "UserPromptSubmit" => {
                s.open.clear();
                s.set(State::Thinking, ev.ts);
            }
            "PreToolUse" => {
                // The question tool's start is logged by two hooks on an
                // install made by an older app: count it once.
                if !(tool == "AskUserQuestion" && s.open.iter().any(|t| t == tool)) {
                    s.open.push(tool.to_owned());
                }
                s.last_tool_name = tool.to_owned();
                s.set(if tool == "AskUserQuestion" { State::Waiting } else { State::Working }, ev.ts);
            }
            "PostToolUse" | "PostToolUseFailure" => {
                if let Some(i) = s.open.iter().position(|t| t == tool) {
                    s.open.remove(i);
                }
                if ev.extra("int") == Some("1") {
                    // The user interrupted: the turn is over, with no Stop.
                    s.open.clear();
                    s.set(State::Idle, ev.ts);
                } else {
                    s.last_tool = ev.ts;
                    s.last_tool_name = tool.to_owned();
                    s.set(State::Working, ev.ts);
                }
            }
            "PermissionRequest" => s.set(State::Waiting, ev.ts),
            "Notification" => match ev.detail.as_deref() {
                Some("permission_prompt" | "elicitation_dialog" | "agent_needs_input") => {
                    s.set(State::Waiting, ev.ts);
                }
                // The prompt has sat idle: whatever turn there was is over.
                Some("idle_prompt") if !s.state.is_some_and(State::resting) => s.set(State::Idle, ev.ts),
                _ => {}
            },
            "PreCompact" => s.compacting = Some(ev.ts),
            "Stop" | "StopFailure" => {
                s.open.clear();
                s.set(if name == "Stop" { State::Done } else { State::Failed }, ev.ts);
                // Claude Code says how many agents still run in the
                // background: the ones heard from last are the ones kept.
                if let Some(bg) = ev.extra("bg").and_then(|n| n.parse::<usize>().ok()) {
                    while s.subs.len() > bg {
                        let oldest = s.subs.iter().min_by_key(|(_, sub)| sub.seen).map(|(k, _)| k.clone());
                        match oldest {
                            Some(k) => s.subs.remove(&k),
                            None => break,
                        };
                    }
                }
            }
            "SessionEnd" => s.ended = true,
            _ => {}
        }
    }

    let mut live: Vec<(&str, Session)> = sessions
        .into_iter()
        .filter(|(_, s)| !s.ended)
        // Claude Code lists the session and its process is gone: it left
        // without saying so.
        .filter(|(id, _)| beside.running.get(*id).is_none_or(|r| r.alive))
        .map(|(id, mut s)| {
            s.subs.retain(|_, sub| now - sub.seen <= SUB_TTL_SECS);
            if let Some(r) = beside.running.get(id) {
                cross_check(&mut s, r);
            }
            (id, s)
        })
        .filter(|(_, s)| {
            let idle = s.state.is_none_or(State::resting) && s.subs.is_empty();
            !(idle && now - s.last > IDLE_TTL_SECS)
        })
        .collect();
    // Too many: the busy ones first, then the most recent.
    live.sort_by_key(|(_, s)| (s.state.is_none_or(State::resting), -s.last));
    live.truncate(MAX_DESKS);
    live.sort_by_key(|(id, s)| (arrived.get(*id).copied().unwrap_or(s.first), *id));

    let mut out: Vec<Desk> = Vec::with_capacity(live.len());
    for (sid, s) in live {
        let state = state_at(&s, now);
        let blocked = state == State::Waiting
            || (matches!(s.state, Some(State::Done | State::Failed))
                && s.subs.is_empty()
                && now - s.since < IDLE_TTL_SECS);
        let since = if state == s.state.unwrap_or_default() { s.since } else { derived_since(&s, state, now) };
        let subs = s.subs.len().min(9) as u8;
        let quiet = s.subs.values().filter(|sub| now - sub.seen >= BREAK_AFTER_SECS).count().min(9) as u8;
        let mut id = fnv8(sid);
        while out.iter().any(|d| d.id == id) {
            id = id.wrapping_add(1);
        }
        let numbers = beside.status.get(sid);
        let clean =
            |t: &str, limit| -> String { to_ascii(t, limit).chars().filter(|c| *c != '|' && *c != '"').collect() };
        // On the desk a long project name ends in dots; a title is simply cut.
        let project = clean(&s.project, TITLE_LIMIT);
        let (name, label) = if s.title.is_empty() {
            (project.clone(), clean(&s.project, NAME_LIMIT))
        } else {
            let t = clean(&s.title, TITLE_LIMIT);
            (t.clone(), t.chars().take(NAME_LIMIT).collect::<String>().trim_end().to_owned())
        };
        let mut kinds: BTreeMap<&str, u8> = BTreeMap::new();
        for sub in s.subs.values() {
            *kinds.entry(if sub.kind.is_empty() { "agent" } else { &sub.kind }).or_default() += 1;
        }
        let mut sub_kinds: Vec<(String, u8)> = kinds.into_iter().map(|(k, n)| (k.to_owned(), n)).collect();
        sub_kinds.sort_by_key(|(k, n)| (std::cmp::Reverse(*n), k.clone()));
        let model_name = numbers.map(|n| n.model.clone()).filter(|m| !m.is_empty()).unwrap_or_else(|| s.model.clone());
        out.push(Desk {
            id,
            state,
            subs,
            quiet: quiet.min(subs),
            name: label,
            context: numbers.and_then(|n| n.context),
            mode: mode_letter(&s.mode),
            model: model_letter(&model_name),
            blocked,
            detail: Detail {
                title: name,
                project,
                since: (now - since).max(0),
                tool: s.open.last().cloned().unwrap_or_else(|| s.last_tool_name.clone()),
                model: clean(&model_name, 24),
                mode: mode_words(&s.mode).to_owned(),
                sub_kinds,
                tasks: s.tasks,
                lines: numbers.and_then(|n| n.lines).filter(|l| *l != (0, 0)),
            },
        });
    }
    number_twins(&mut out);
    out
}

/// Claude Code's own word on a session, where it is newer than the hooks':
/// idle after an interrupt (which has no hook), busy again once a permission
/// was given (the tool's end is the next hook), waiting before the
/// notification for it arrives.
fn cross_check(s: &mut Session, r: &Running) {
    let newer = r.status_at > s.last_main;
    let state = s.state.unwrap_or_default();
    match r.status {
        RunStatus::Idle if newer && !state.resting() => {
            s.open.clear();
            s.compacting = None;
            s.set(State::Idle, r.status_at);
        }
        RunStatus::Busy if newer && state == State::Waiting => {
            s.last_tool = r.status_at;
            s.set(State::Working, r.status_at);
        }
        RunStatus::Waiting if r.status_at >= s.last_main && matches!(state, State::Working | State::Thinking) => {
            s.set(State::Waiting, r.status_at);
        }
        _ => {}
    }
}

fn state_at(s: &Session, now: i64) -> State {
    if s.compacting.is_some_and(|t| now - t <= COMPACT_TTL_SECS) {
        return State::Compacting;
    }
    match s.state {
        Some(State::Waiting) => State::Waiting,
        Some(State::Working | State::Thinking) => {
            if s.open.iter().any(|t| spawns_agents(t)) {
                State::Delegating
            } else if !s.open.is_empty() || (s.state == Some(State::Working) && now - s.last_tool <= THINK_AFTER_SECS) {
                State::Working
            } else if s.subs.is_empty() && now - s.last > STALE_TURN_SECS {
                if now - s.last >= STALE_TURN_SECS + BREAK_AFTER_SECS {
                    State::Break
                } else {
                    State::Idle
                }
            } else {
                State::Thinking
            }
        }
        _ if !s.subs.is_empty() => State::Delegating,
        Some(State::Failed) => State::Failed,
        Some(State::Done) => match now - s.since {
            t if t < NAP_AFTER_SECS => State::Done,
            t if t < DONE_BREAK_AFTER_SECS => State::Idle,
            _ => State::Break,
        },
        _ if now - s.since.max(s.last_main) >= BREAK_AFTER_SECS => State::Break,
        _ => State::Idle,
    }
}

/// When a state that time alone brought on began.
fn derived_since(s: &Session, state: State, now: i64) -> i64 {
    match (s.state, state) {
        (_, State::Compacting) => s.compacting.unwrap_or(now),
        (Some(State::Working), State::Thinking) => s.last_tool + THINK_AFTER_SECS,
        (Some(State::Done), State::Idle) => s.since + NAP_AFTER_SECS,
        (Some(State::Done), State::Break) => s.since + DONE_BREAK_AFTER_SECS,
        (Some(State::Working | State::Thinking), State::Idle | State::Break) => s.last + STALE_TURN_SECS,
        (_, State::Break) => s.since.max(s.last_main) + BREAK_AFTER_SECS,
        _ => s.since,
    }
}

/// Desks that would read the same get a number after the name, in the order
/// they sit: `api`, `api 2`.
fn number_twins(desks: &mut [Desk]) {
    // The number has to survive the cut a second office brings ([`encode`]).
    let limit = if desks.len() > OFFICE_DESKS { NAME_LIMIT_CROWDED } else { NAME_LIMIT };
    let names: Vec<String> = desks.iter().map(|d| d.name.clone()).collect();
    let mut seen: HashMap<&str, u32> = HashMap::new();
    for (d, name) in desks.iter_mut().zip(&names) {
        let n = seen.entry(name).or_default();
        *n += 1;
        if *n > 1 && !name.is_empty() {
            let tail = format!(" {n}");
            let base = name.trim_end_matches("...");
            let keep = limit.saturating_sub(tail.len()).min(base.len());
            d.name = format!("{}{tail}", base[..keep].trim_end());
        }
    }
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
            format!("{:02x}{}{}{}{}", d.id, d.state.coarse(), d.subs.min(9), d.quiet.min(9), name.trim_end())
        })
        .collect::<Vec<_>>()
        .join("|")
}

/// The `tx` field: four characters per desk, in `tm`'s order: the state
/// ([`State::code`]), the context used (`0`-`9`, `a`-`z`: thirty-fifths of
/// the window; `-` unknown), the permission mode and the model.
pub fn encode_extra(desks: &[Desk]) -> String {
    desks
        .iter()
        .map(|d| {
            let ctx = d
                .context
                .map_or('-', |pct| std::char::from_digit(u32::from(pct.min(100)) * 35 / 100, 36).unwrap_or('-'));
            let or_dash = |c: char| if c.is_ascii_graphic() { c } else { '-' };
            format!("{}{ctx}{}{}", d.state.code(), or_dash(d.mode), or_dash(d.model))
        })
        .collect()
}

/// The pet's mood can be this high.
pub const MOOD_MAX: u8 = 4;
/// ... and its energy this high.
pub const ENERGY_MAX: u8 = 5;
/// How far back the mood remembers: a moment's weight falls to about a
/// third after this long.
pub const MOOD_MEMORY_SECS: f64 = 900.0;

/// The pet's mood: how well the sessions are kept unblocked. It follows the
/// share of the sessions in the office that are held up by the user
/// ([`Desk::blocked`]), averaged over about the last quarter hour, so one
/// slow answer among many busy sessions costs little and a team left
/// waiting costs a lot. Time away from the computer is not counted.
#[derive(Debug, Clone, PartialEq)]
pub struct PetMood {
    share: f64,
    last: Option<i64>,
}

impl Default for PetMood {
    /// Neutral until there is something to go by.
    fn default() -> Self {
        PetMood { share: 0.2, last: None }
    }
}

impl PetMood {
    /// Take in the desks at `now`; `away` = nobody is at the computer.
    /// Returns the mood, 0 to [`MOOD_MAX`].
    pub fn update(&mut self, now: i64, desks: &[Desk], away: bool) -> u8 {
        let dt = self.last.map_or(0, |t| (now - t).clamp(0, 60)) as f64;
        self.last = Some(now);
        if !away && dt > 0.0 {
            let blocked = desks.iter().filter(|d| d.blocked).count() as f64;
            let share = if desks.is_empty() { 0.0 } else { blocked / desks.len() as f64 };
            let keep = (-dt / MOOD_MEMORY_SECS).exp();
            self.share = self.share * keep + share * (1.0 - keep);
        }
        self.mood()
    }

    /// The mood as it stands.
    pub fn mood(&self) -> u8 {
        match self.share {
            s if s < 0.06 => 4,
            s if s < 0.15 => 3,
            s if s < 0.30 => 2,
            s if s < 0.45 => 1,
            _ => 0,
        }
    }
}

/// The pet's energy: the context headroom of the fullest session, 0 to
/// [`ENERGY_MAX`]. `None` when no session's context is known (no status
/// line): the device then keeps its own rule.
pub fn pet_energy(desks: &[Desk]) -> Option<u8> {
    let fullest = desks.iter().filter_map(|d| d.context).max()?;
    Some(match fullest {
        0..=19 => 5,
        20..=39 => 4,
        40..=59 => 3,
        60..=74 => 2,
        75..=89 => 1,
        _ => 0,
    })
}

/// The `ps` field: the mood's digit and the energy's (`-` unknown).
pub fn encode_pet(mood: u8, energy: Option<u8>) -> String {
    let e = energy.map_or('-', |e| char::from(b'0' + e.min(ENERGY_MAX)));
    format!("{}{e}", mood.min(MOOD_MAX))
}

/// A span for the detail card: `45s`, `12m`, `2h 5m`.
fn span(secs: i64) -> String {
    match secs {
        s if s < 60 => format!("{s}s"),
        s if s < 3600 => format!("{}m", s / 60),
        s => format!("{}h {}m", s / 3600, s % 3600 / 60),
    }
}

/// A detail card's longest text.
pub const DETAIL_LIMIT: usize = 200;

/// The `td` field for one desk: its id and the card's lines, joined with
/// `|`: the title; the state, how long, the tool; the model, mode and
/// context; the sub-agents by type; the project and the task list.
pub fn encode_detail(d: &Desk) -> String {
    let x = &d.detail;
    let mut what = format!("{} {}", d.state.words(), span(x.since));
    if matches!(d.state, State::Working) && !x.tool.is_empty() {
        what.push_str(&format!(" - {}", x.tool.chars().take(24).collect::<String>()));
    }
    let mut about: Vec<String> = Vec::new();
    if !x.model.is_empty() {
        about.push(x.model.clone());
    }
    if !x.mode.is_empty() {
        about.push(x.mode.clone());
    }
    if let Some(c) = d.context {
        about.push(format!("context {c}%"));
    }
    let team = if x.sub_kinds.is_empty() {
        "No sub-agents".to_owned()
    } else {
        let list: Vec<String> = x.sub_kinds.iter().take(3).map(|(k, n)| format!("{n} {k}")).collect();
        format!("Sub-agents: {}", list.join(", "))
    };
    let mut foot: Vec<String> = Vec::new();
    if x.project != x.title {
        foot.push(x.project.clone());
    }
    if x.tasks.1 > 0 {
        foot.push(format!("tasks {}/{}", x.tasks.0.min(x.tasks.1), x.tasks.1));
    }
    if let Some((added, removed)) = x.lines {
        foot.push(format!("+{added} -{removed} lines"));
    }
    let text = format!("{:02x}|{}|{what}|{}|{team}|{}", d.id, x.title, about.join(" - "), foot.join(" - "));
    to_ascii(&text, DETAIL_LIMIT).replace('"', "'")
}

/// What [`TeamTracker::poll`] returns.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Team {
    /// The `tm` field.
    pub tm: String,
    /// The `tx` field.
    pub tx: String,
    /// The desks behind them.
    pub desks: Vec<Desk>,
}

/// Follows `team.log` (and the rolled `team.log.1`), re-reading only when a
/// file changed.
#[derive(Debug)]
pub struct TeamTracker {
    dir: PathBuf,
    stamp: Option<[Option<(u64, SystemTime)>; 2]>,
    events: Vec<TeamEvent>,
    /// When each session now in the log was first seen.
    arrived: HashMap<String, i64>,
    /// The state each desk shows, and since when, by desk id.
    shown: HashMap<u8, (State, i64)>,
}

impl TeamTracker {
    /// Follow the files in `dir` (the state dir).
    pub fn new(dir: impl Into<PathBuf>) -> Self {
        TeamTracker { dir: dir.into(), stamp: None, events: Vec::new(), arrived: HashMap::new(), shown: HashMap::new() }
    }

    fn files(&self) -> [PathBuf; 2] {
        [self.dir.join("team.log.1"), self.dir.join("team.log")]
    }

    /// The `tm` field at `now`.
    pub fn poll(&mut self, now: i64) -> String {
        self.poll_with(now, &Beside::default()).tm
    }

    /// The team at `now`, with what else is known about the sessions.
    pub fn poll_with(&mut self, now: i64, beside: &Beside) -> Team {
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
            // A session that ended and came back is a new arrival; one with
            // nothing left in the log is forgotten.
            for e in &self.events {
                if e.event == "SessionEnd" {
                    self.arrived.remove(&e.session);
                } else {
                    self.arrived.entry(e.session.clone()).or_insert(e.ts);
                }
            }
            let events = &self.events;
            self.arrived.retain(|id, _| events.iter().any(|e| &e.session == id));
        }
        let mut desks = desks_seated(&self.events, now, beside, &self.arrived);
        self.steady(&mut desks, now);
        Team { tm: encode(&desks), tx: encode_extra(&desks), desks }
    }

    /// Keep a desk in the state it shows for [`MIN_SHOW_SECS`], so a main
    /// agent woken by each of its sub-agents' results does not flicker. A
    /// session that starts or stops waiting for the user shows at once.
    fn steady(&mut self, desks: &mut [Desk], now: i64) {
        self.shown.retain(|id, _| desks.iter().any(|d| d.id == *id));
        for d in desks {
            match self.shown.get(&d.id).copied() {
                Some((shown, since)) if shown != d.state => {
                    let urgent = shown == State::Waiting || d.state == State::Waiting;
                    // A clock set back must not hold a state for ever.
                    if urgent || !(0..MIN_SHOW_SECS).contains(&(now - since)) {
                        self.shown.insert(d.id, (d.state, now));
                    } else {
                        d.state = shown;
                    }
                }
                Some(_) => {}
                None => {
                    self.shown.insert(d.id, (d.state, now));
                }
            }
        }
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
        // A finished turn is done (your move), then dozes, then takes a break.
        assert_eq!(one(&evs, t + 32).state, State::Done);
        assert_eq!(one(&evs, t + 31 + NAP_AFTER_SECS - 1).state, State::Done);
        assert_eq!(one(&evs, t + 31 + NAP_AFTER_SECS).state, State::Idle);
        assert_eq!(one(&evs, t + 31 + DONE_BREAK_AFTER_SECS).state, State::Break, "idle for a while: a break");
        // Idle sessions leave after a while; an ended one leaves at once.
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
    fn a_sub_agents_compaction_and_folder_are_not_the_main_agents() {
        let t = 1000;
        let mut evs = vec![
            ev(t, "UserPromptSubmit s1 - - +md=claude-sonnet-5-5 api"),
            ev(t + 1, "PreToolUse s1 - Agent api"),
            ev(t + 2, "PreToolUse s1 a1 Bash T-1"),
            // Held in the Agent call: this compaction is the sub-agent's.
            ev(t + 3, "PreCompact s1 - - api"),
            ev(t + 4, "PostToolUse s1 a1 Bash T-1"),
        ];
        let d = &desks(&evs, t + 5)[0];
        assert_eq!((d.state, d.name.as_str()), (State::Delegating, "api"));
        evs.push(ev(t + 9, "SessionStart s1 - - +md=claude-opus-5-5 api"));
        evs.push(ev(t + 9, "PostCompact s1 - - api"));
        assert_eq!(desks(&evs, t + 10)[0].model, 's');
        // Back in a turn of its own, a compaction is the main agent's, and
        // one that never reports its end is over in two minutes.
        evs.push(ev(t + 20, "PostToolUse s1 - Agent api"));
        evs.push(ev(t + 21, "PreCompact s1 - - api"));
        assert_eq!(desks(&evs, t + 22)[0].state, State::Compacting);
        assert_eq!(desks(&evs, t + 21 + COMPACT_TTL_SECS)[0].state, State::Compacting);
        assert_ne!(desks(&evs, t + 22 + COMPACT_TTL_SECS)[0].state, State::Compacting);
        // With nobody else about, a compaction between turns (/compact) shows.
        let alone = [ev(t, "Stop s2 - - api"), ev(t + 5, "PreCompact s2 - - api")];
        assert_eq!(desks(&alone, t + 6)[0].state, State::Compacting);
    }

    #[test]
    fn desks_stay_put_and_states_hold() {
        let dir = std::env::temp_dir().join(format!("clawdbuddy-team-seats-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let log = dir.join("team.log");
        let mut t = TeamTracker::new(&dir);
        let names = |tm: &str| tm.split('|').map(|d| d[5..].to_owned()).collect::<Vec<_>>();
        fs::write(&log, "100 UserPromptSubmit s1 - - one\n200 UserPromptSubmit s2 - - two\n").unwrap();
        assert_eq!(names(&t.poll(300)), ["one", "two"]);
        // An hour on, s1's first event has aged out and its oldest is now
        // newer than s2's: they keep their desks.
        fs::write(
            &log,
            "200 UserPromptSubmit s2 - - two\n3650 PostToolUse s1 - Bash one\n3660 PostToolUse s2 - Bash two\n",
        )
        .unwrap();
        assert_eq!(names(&t.poll(3750)), ["one", "two"]);
        assert_eq!(names(&TeamTracker::new(&dir).poll(3750)), ["two", "one"], "a fresh start has only the log");

        // A turn that stops and is woken again within seconds: no flicker.
        let state = |tm: String| tm.split('|').next().unwrap().as_bytes()[2] as char;
        fs::write(&log, "5000 UserPromptSubmit s1 - - one\n").unwrap();
        assert_eq!(state(t.poll(5001)), 't');
        fs::write(&log, "5000 UserPromptSubmit s1 - - one\n5002 PreToolUse s1 - Bash one\n").unwrap();
        assert_eq!(state(t.poll(5003)), 'w', "thinking had shown long enough");
        fs::write(&log, "5000 UserPromptSubmit s1 - - one\n5004 Stop s1 - - one\n").unwrap();
        assert_eq!(state(t.poll(5005)), 'w', "held");
        assert_eq!(state(t.poll(5003 + MIN_SHOW_SECS)), 'i');
        fs::write(&log, "5000 UserPromptSubmit s1 - - one\n").unwrap();
        assert_eq!(state(t.poll(5020)), 't');
        // Waiting for you shows at once, and so does its end.
        fs::write(&log, "5000 UserPromptSubmit s1 - - one\n5021 PermissionRequest s1 - Bash one\n").unwrap();
        assert_eq!(state(t.poll(5022)), 'q');
        fs::write(&log, "5000 UserPromptSubmit s1 - - one\n5023 PostToolUse s1 - Bash one\n").unwrap();
        assert_eq!(state(t.poll(5024)), 'w');
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_field() {
        let d = vec![
            Desk { id: 0xa3, state: State::Working, subs: 2, quiet: 1, name: "TestApp".into(), ..Desk::default() },
            Desk {
                id: 7,
                state: State::Done,
                name: "ClawdBuddy".into(),
                context: Some(60),
                mode: 'p',
                model: 'f',
                ..Desk::default()
            },
        ];
        // `tm` keeps to the six states firmware 0.1 knows; `tx` has the rest.
        assert_eq!(encode(&d), "a3w21TestApp|07i00ClawdBuddy");
        assert_eq!(encode_extra(&d), "w---flpf");
        assert_eq!(encode_extra(&[Desk { context: Some(100), ..Desk::default() }]), "iz--");
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
        fs::write(dir.join("team.log"), "90 UserPromptSubmit s1 - - api\n95 SubagentStart s1 a1 Explore api\n")
            .unwrap();
        assert!(t.poll(100).ends_with("t10api"));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_running_tool_is_work_however_long_it_takes() {
        let t = 1000;
        let mut evs = vec![ev(t, "UserPromptSubmit s1 - - api"), ev(t + 2, "PreToolUse s1 - Bash api")];
        let one = |evs: &[TeamEvent], now| desks(evs, now).pop().unwrap();
        assert_eq!(one(&evs, t + 200).state, State::Working);
        assert_eq!(one(&evs, t + 200).detail.tool, "Bash");
        evs.push(ev(t + 300, "PostToolUse s1 - Bash api"));
        assert_eq!(one(&evs, t + 305).state, State::Working);
        assert_eq!(one(&evs, t + 311).state, State::Thinking);
        // A failed call ends the tool too.
        evs.push(ev(t + 320, "PreToolUse s1 - Edit api"));
        evs.push(ev(t + 321, "PostToolUseFailure s1 - Edit api"));
        assert_eq!(one(&evs, t + 340).state, State::Thinking);
        // A foreground agent call: the main agent waits for it.
        evs.push(ev(t + 350, "PreToolUse s1 - Agent api"));
        evs.push(ev(t + 351, "SubagentStart s1 a1 Explore api"));
        assert_eq!(one(&evs, t + 500).state, State::Delegating);
        evs.push(ev(t + 600, "SubagentStop s1 a1 Explore api"));
        evs.push(ev(t + 600, "PostToolUse s1 - Agent api"));
        assert_eq!(one(&evs, t + 601).state, State::Working);
    }

    #[test]
    fn an_interrupt_ends_the_turn() {
        let t = 1000;
        let evs = [
            ev(t, "UserPromptSubmit s1 - - api"),
            ev(t + 2, "PreToolUse s1 - Bash api"),
            ev(t + 9, "PostToolUseFailure s1 - Bash +int=1 api"),
        ];
        assert_eq!(desks(&evs, t + 10)[0].state, State::Idle);
        // With no event at all (an interrupt while thinking), the turn is
        // taken as over after a while.
        let evs = [ev(t, "UserPromptSubmit s1 - - api")];
        assert_eq!(desks(&evs, t + STALE_TURN_SECS)[0].state, State::Thinking);
        assert_eq!(desks(&evs, t + STALE_TURN_SECS + 1)[0].state, State::Idle);
    }

    #[test]
    fn extras_titles_modes_and_twins() {
        let e = ev(5, "Stop s1 - - +pm=plan,tt=Fix_the_login_page,bg=1 my project");
        assert_eq!(e.extra("pm"), Some("plan"));
        assert_eq!(e.extra("bg"), Some("1"));
        assert_eq!(e.project, "my project");
        let t = 1000;
        let evs = [
            ev(t, "SessionStart s1 - - +md=claude-fable-5-2 api"),
            ev(t + 1, "UserPromptSubmit s1 - - +pm=plan,tt=Fix_the_login_page api"),
            ev(t + 2, "UserPromptSubmit s2 - - api"),
            ev(t + 3, "UserPromptSubmit s3 - - api"),
            ev(t + 4, "TaskCreated s1 - - api"),
            ev(t + 4, "TaskCreated s1 - - api"),
            ev(t + 5, "TaskCompleted s1 - - api"),
        ];
        let d = desks(&evs, t + 6);
        assert_eq!(d[0].name, "Fix the logi");
        assert_eq!((d[0].mode, d[0].model), ('p', 'f'));
        assert_eq!(d[0].detail.title, "Fix the login page");
        assert_eq!(d[0].detail.tasks, (1, 2));
        // Two sessions in one project read apart.
        assert_eq!((d[1].name.as_str(), d[2].name.as_str()), ("api", "api 2"));
        let long = [ev(t, "Stop a - - a-long-project-name"), ev(t + 1, "Stop b - - a-long-project-name")];
        let twins = desks(&long, t + 2);
        assert_eq!((twins[0].name.as_str(), twins[1].name.as_str()), ("a-long-pr...", "a-long-pr 2"));
        assert_eq!(twins[0].detail.title, "a-long-project-name");
        let card = encode_detail(&d[0]);
        assert!(
            card.contains("|Fix the login page|Thinking 5s|claude-fable-5-2 - plan mode|No sub-agents|api - tasks 1/2"),
            "{card}"
        );
    }

    #[test]
    fn a_stop_says_how_many_agents_still_run() {
        let t = 1000;
        let mut evs = vec![
            ev(t, "UserPromptSubmit s1 - - api"),
            ev(t + 1, "SubagentStart s1 a1 Explore api"),
            ev(t + 2, "SubagentStart s1 a2 Plan api"),
            ev(t + 3, "PostToolUse s1 a2 Read api"),
            ev(t + 9, "Stop s1 - - +bg=1 api"),
        ];
        let d = &desks(&evs, t + 10)[0];
        assert_eq!((d.subs, d.state), (1, State::Delegating));
        assert_eq!(d.detail.sub_kinds, vec![("Plan".to_owned(), 1)]);
        evs.push(ev(t + 20, "Stop s1 - - +bg=0 api"));
        let d = &desks(&evs, t + 21)[0];
        assert_eq!((d.subs, d.state), (0, State::Done));
        // An API error ends the turn differently; compaction shows.
        evs.push(ev(t + 30, "UserPromptSubmit s1 - - api"));
        evs.push(ev(t + 31, "PreCompact s1 - - api"));
        assert_eq!(desks(&evs, t + 40)[0].state, State::Compacting);
        evs.push(ev(t + 50, "PostCompact s1 - - api"));
        assert_eq!(desks(&evs, t + 51)[0].state, State::Thinking);
        evs.push(ev(t + 60, "StopFailure s1 - - +er=rate_limit api"));
        assert_eq!(desks(&evs, t + 61)[0].state, State::Failed);
    }

    #[test]
    fn claude_codes_own_list_corrects_the_hooks() {
        let t = 1000;
        let run = |status, status_at, alive| Running { alive, status, status_at };
        let with = |evs: &[TeamEvent], now, r: Running| {
            let beside = Beside { running: HashMap::from([("s1".to_owned(), r)]), ..Beside::default() };
            desks_with(evs, now, &beside)
        };
        let evs = [ev(t, "UserPromptSubmit s1 - - api"), ev(t + 2, "PreToolUse s1 - Bash api")];
        // Interrupted: idle by Claude Code's word, after the last hook.
        assert_eq!(with(&evs, t + 20, run(RunStatus::Idle, t + 10, true))[0].state, State::Idle);
        // An older "idle" says nothing about this turn.
        assert_eq!(with(&evs, t + 20, run(RunStatus::Idle, t - 5, true))[0].state, State::Working);
        // Waiting before the notification arrives.
        assert_eq!(with(&evs, t + 4, run(RunStatus::Waiting, t + 3, true))[0].state, State::Waiting);
        // A permission given: busy again while the tool runs.
        let mut asked = evs.to_vec();
        asked.push(ev(t + 8, "Notification s1 - permission_prompt api"));
        assert_eq!(desks(&asked, t + 60)[0].state, State::Waiting);
        assert_eq!(with(&asked, t + 60, run(RunStatus::Busy, t + 15, true))[0].state, State::Working);
        // Its process is gone: so is the desk.
        assert!(with(&evs, t + 20, run(RunStatus::Busy, t, false)).is_empty());
        // The context used comes from the status line.
        let beside = Beside {
            status: HashMap::from([(
                "s1".to_owned(),
                StatusNumbers {
                    context: Some(42),
                    model: "Fable 5.2".into(),
                    lines: Some((120, 7)),
                    ..StatusNumbers::default()
                },
            )]),
            ..Beside::default()
        };
        let d = &desks_with(&evs, t + 3, &beside)[0];
        assert_eq!((d.context, d.model), (Some(42), 'f'));
        assert!(encode_detail(d).contains("Fable 5.2 - context 42%"));
        assert!(encode_detail(d).ends_with("+120 -7 lines"), "{}", encode_detail(d));
    }

    #[test]
    fn the_pets_mood_follows_sessions_left_waiting() {
        let t = 1000;
        let evs = [
            ev(t, "UserPromptSubmit s1 - - a"),
            ev(t, "UserPromptSubmit s2 - - b"),
            ev(t + 1, "Notification s2 - permission_prompt b"),
        ];
        let d = desks(&evs, t + 2);
        assert_eq!((d[0].blocked, d[1].blocked), (false, true));
        // A finished turn waits for you too, for a quarter hour.
        let done = [ev(t, "Stop s1 - - a")];
        assert!(desks(&done, t + 600)[0].blocked);
        assert!(!desks(&done, t + IDLE_TTL_SECS)[0].blocked);
        // ... but not while its sub-agents still work.
        let busy = [ev(t, "SubagentStart s1 a1 Explore a"), ev(t + 1, "Stop s1 - - a")];
        assert!(!desks(&busy, t + 5)[0].blocked);

        let mut mood = PetMood::default();
        assert_eq!(mood.update(t, &d, false), 2, "neutral to start");
        // Nothing blocked for a good while: content.
        let free = desks(&evs[..2], t + 2);
        let mut now = t;
        for _ in 0..3600 {
            now += 1;
            mood.update(now, &free, false);
        }
        assert_eq!(mood.mood(), 4);
        // Half the team waits while you are away: no blame.
        for _ in 0..3600 {
            now += 1;
            mood.update(now, &d, true);
        }
        assert_eq!(mood.mood(), 4);
        // ... and while you are there: it sours within minutes.
        for _ in 0..300 {
            now += 1;
            mood.update(now, &d, false);
        }
        assert!(mood.mood() <= 3);
        for _ in 0..3600 {
            now += 1;
            mood.update(now, &d, false);
        }
        assert_eq!(mood.mood(), 0);
    }

    #[test]
    fn the_pets_energy_is_the_fullest_sessions_headroom() {
        let desk = |c| Desk { context: c, ..Desk::default() };
        assert_eq!(pet_energy(&[]), None);
        assert_eq!(pet_energy(&[desk(None)]), None, "no status line: the device's own rule");
        assert_eq!(pet_energy(&[desk(Some(5)), desk(None)]), Some(5));
        assert_eq!(pet_energy(&[desk(Some(5)), desk(Some(62))]), Some(2));
        assert_eq!(pet_energy(&[desk(Some(95))]), Some(0));
        assert_eq!(encode_pet(4, Some(2)), "42");
        assert_eq!(encode_pet(1, None), "1-");
    }
}
