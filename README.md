# MOS Phase 2 — Multiprogramming OS Simulator

A software simulation of a multiprogramming operating system built for an Operating Systems course (Phase 2). Written in C with a live browser dashboard.

---

## What it does

- Loads all jobs into memory simultaneously (each with its own page table)
- Runs a **round-robin scheduler** that executes one instruction per job per turn
- Implements **virtual memory with demand paging** — frames allocated lazily at runtime
- Handles a full **interrupt system**: Program (PI), Time (TI), System (SI) with priority PI > TI > SI
- Outputs per-job results with halt codes, register state, and counters

---

## Files

| File | Description |
|---|---|
| `ph2.c` | C simulator — loader, scheduler, MMU, MOS interrupt handler |
| `server.js` | Node.js HTTP server — serves dashboard, exposes `/api/run` and `/api/input` |
| `index.html` | Browser dashboard — live memory map, charts, page table viewer, input editor |
| `run.bat` | Compile + launch script (Windows) |
| `input_phase2.txt` | Job definitions — edit to change what runs |
| `PROJECT_REPORT.md` | Full detailed project report |

---

## How to run

**Prerequisites:** GCC (MinGW-w64) and Node.js, both in PATH.

```bat
run.bat
```

This compiles `ph2.c` → `ph2.exe`, starts the server on `localhost:4173`, and opens the dashboard in your browser.

To run the simulator without the dashboard:
```
ph2.exe
```

---

## Input format

```
$AMJ<JID><TTL><TLL>    ← job header (4+4+4 fixed-width chars)
<instructions>H        ← program: GD PD LR SR CR BT H, each 4 chars
$DTA                   ← start of data section
<data lines>           ← read by GD instructions at runtime
$END<JID>
```

---

## Dashboard features

- Live register display (IR, R, C, PTR, IC)
- 300-cell color-coded memory map with hover tooltips
- Per-job page table visualizer (virtual page → physical frame)
- 4 charts: time usage, line usage, halt-reason donut, IC per job
- Input file editor with Save & Run
- Dark mode, keyboard shortcuts (`R` run, `D` dark mode, `1`–`7` select job, `?` help)

---

## OS concepts demonstrated

Virtual memory · Demand paging · Page fault handling · Round-robin scheduling · Context switching · PCB (Process Control Block) · Interrupt priority · System calls · Job accounting
