# The office view

The second of ClawdBuddy's two views: Claude Code sessions as desks in an
office. For what you see and how to use it, start with the
[README](../README.md#the-office-view); this page has the rules behind it and
where the data comes from.

![The office view](images/office.png)

## Where the data comes from

Claude Code runs the app's hook on these events:

| Hook | Used for |
| --- | --- |
| `SessionStart`, `SessionEnd` | The session arrived or left; its model |
| `UserPromptSubmit` | A turn started: the main agent is thinking |
| `PreToolUse` | A tool started. Until it ends the session is working; an `Agent` call is waiting for sub-agents, the question tool is waiting for you |
| `PostToolUse`, `PostToolUseFailure` | The tool ended. A failure marked as interrupted ends the turn. Inside a sub-agent the event carries its `agent_id`, which shows that sub-agent is alive |
| `PermissionRequest`, `Notification` | Waiting for you |
| `Stop` | The turn ended, and how many agents still run in the background |
| `StopFailure` | The turn ended on an API error |
| `SubagentStart`, `SubagentStop` | A sub-agent came or went; its type |
| `PreCompact`, `PostCompact` | Compacting |
| `TaskCreated`, `TaskCompleted` | The task list's progress, for the card |

Every call appends one line to `team.log` in the app's folder: the time, the
event, the session id, the sub-agent id, the tool's name or the notification's
type, a few extras (`+pm=plan,tt=Fix_the_login_page,bg=1`: the permission
mode, the model, the session's title, the agents still running at a Stop, an
interrupted tool, a StopFailure's error) and the name of the working
directory. Never a prompt, or a tool's input or output.

Two more sources, both optional:

- **The status line.** `clawdbuddy hook statusline` is a Claude Code
  `statusLine` command. Claude Code hands it each session's context %, model
  and usage windows after every reply; it keeps them in
  `statusline/<session id>.json` in the app's folder. The context becomes the
  paper on the desk.
- **Claude Code's list of running sessions**, `sessions/<pid>.json` in its
  config folder (the data behind `claude agents --json`): the process id and
  a status of busy, idle or waiting. Its format is not documented, so a file
  that doesn't read as expected is skipped. Where it is newer than the last
  hook it wins: idle after an interrupt (which has no hook), busy again once
  a permission was given, waiting before the notification arrives. A session
  whose process is gone leaves at once.

The app folds the last hour of the log into the sessions to show
(`desktop/crates/clawdbuddy-core/src/team.rs`) and sends them to the board as
the `tm` and `tx` fields ([protocol](protocol.md)).

## A session's state

| State | `tx` | `tm` | Rule |
| --- | --- | --- | --- |
| Working | `w` | `w` | A tool is running, or the last one ended within 10 seconds |
| Thinking | `t` | `t` | In a turn, no tool running |
| Waiting for you | `q` | `q` | A permission prompt, a question or an elicitation dialog is up |
| Waiting for sub-agents | `d` | `d` | An `Agent` call is running, or the turn ended and sub-agents still run |
| Compacting | `c` | `t` | Between `PreCompact` and the next event (10 minutes at most) |
| Done | `f` | `i` | The turn ended less than 3 minutes ago |
| Failed | `e` | `i` | The turn ended on an API error |
| Idle | `i` | `i` | 3 to 6 minutes after the turn ended; or between turns without one having ended (an interrupt), for 2 minutes |
| On a break | `b` | `b` | After that |

What is still inferred:

- **An interrupt while thinking** has no event. With the list of running
  sessions it shows at once; without it, a turn with no tool running and no
  event for 5 minutes is taken as over.
- **A sub-agent's end** is sometimes not reported. `Stop` says how many still
  run, and the ones heard from longest ago are dropped to match; otherwise a
  sub-agent with no event for 10 minutes is taken as gone. After 2 minutes
  without an event it is counted as quiet and stands at the water cooler.

A session leaves when it exits (`SessionEnd`), when its process is gone, 15
minutes after its last turn ended, or after an hour without any event.

## The scene

The firmware keeps the scene and draws it at 480×480, fifteen times a second
(`firmware/src/pet/team_core.cpp`, pure and tested by `test_team`). The names
and the bottom line are text labels over it (`pet/team_view.cpp`).

- Sessions sit in the order the app sends them, which is the order they
  started: six to an office with the desks skin, four with cubicles. Twelve
  sessions is what fits one Bluetooth write (two offices of desks, three of
  cubicles); with more than six, names are cut to 8 characters.
- The skin comes in the `tv` field (1 desks, 2 cubicles; `team_skin` in the
  app's config). In the cubicle skin a plus of partitions makes four
  cubicles, with a gap either side of its centre where people walk. Each
  cubicle's sub-agent desks are a row, the far side of an island, or the
  upright of an L; which cubicle gets which is drawn from a number picked
  when the board starts, so neighbours never match.
- When a session leaves, every session after it moves up one desk. A person
  whose new desk is in the other office walks to the door of one and comes in
  at the door of the other.
- Each session keeps the shirt colour it was given when it arrived (the one
  fewest sessions were wearing). Sub-agents wear a lighter shade.
- The main agent goes to the whiteboard after 6 seconds of thinking (or of
  working, in plan mode), stays at least 4 seconds, and comes back when the
  state changes; a question brings it back at once. Sub-agents visit it at
  random while their session works.
- People on their feet who meet step aside for a moment, the one later in
  the list waiting for the other.
- A tap is a desk's when it lands on the desk itself (not its bench or its
  name); the board then asks the app for that desk's card.
- The context used is a stack of sheets beside the monitor, 42 pixels tall
  when full, which is the monitor's height.
- The room's fixed parts (walls, floor, door, board, cooler, plant) are drawn
  once into a second picture and copied each frame. The time a frame takes
  is written to the serial log every 300 frames (`team view: N us a frame`).
- While the view is off screen nobody walks: arrivals are simply at their
  desks the next time it comes up.

The view's picture and the copy of the room each take 460 KB of the board's
PSRAM, allocated the first time it is shown.

## Trying it

```bash
cd firmware
SIM_FAST=1 SIM_SCENARIO=sim/team.jsonl SIM_TAPS="3000@440,300" .pio/build/sim/program
SIM_FAST=1 SIM_SCENARIO=sim/team-cubicles.jsonl SIM_TAPS="3000@440,300" .pio/build/sim/program
# The finer states, the paper, and a desk's card (the second tap).
SIM_FAST=1 SIM_SCENARIO=sim/team-states.jsonl SIM_TAPS="3000@440,300;10000@80,170" .pio/build/sim/program
SIM_FAST=1 SIM_SCENARIO=sim/team-two-offices.jsonl \
  SIM_TAPS="3000@440,300;16000@440,300;40000@40,300" .pio/build/sim/program
```

A scenario is one JSON payload per line, with a `hold_ms`; edit the `tm`
values to stage other situations.
