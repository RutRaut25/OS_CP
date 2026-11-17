# MOS Phase 2 — Multiprogramming Operating System Simulator
## Detailed Project Report

---

## 1. Project Overview

This project is a complete implementation of **MOS Phase 2** — a software simulation of a multiprogramming operating system, written in C (C11) with a live browser-based monitoring dashboard built on top of it.

The core simulator (`ph2.c`) models the fundamental mechanisms of a real operating system:
- Virtual memory with demand paging and a hardware page table
- Three-tier interrupt system (Program, Time, System) with correct priority ordering
- A two-pass job loader that loads all jobs into physical memory simultaneously
- A round-robin CPU scheduler that interleaves execution one instruction at a time
- Per-job context structs that are saved and restored on every context switch

The dashboard (`index.html` + `server.js`) wraps the simulator with a full web UI: live register display, memory map visualization, per-job page table viewer, charts, an input file editor, and keyboard shortcuts — all served locally over HTTP with no external dependencies.

---

## 2. System Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     BROWSER (index.html)                │
│                                                         │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │ Metrics  │ │ Registers│ │  Charts  │ │ Mem Map  │  │
│  │ cards    │ │ & limits │ │ (4 types)│ │ 300 cells│  │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘  │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │All-jobs  │ │ Program  │ │ Activity │ │  Input   │  │
│  │  table   │ │  output  │ │   logs   │ │  editor  │  │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘  │
│  ┌──────────────────────────────────────────────────┐  │
│  │          Page table visualizer                   │  │
│  └──────────────────────────────────────────────────┘  │
└───────────────────────┬─────────────────────────────────┘
                        │ HTTP  localhost:4173
                        │ GET/POST /api/run
                        │ GET/POST /api/input
                        │ GET static files
              ┌─────────▼──────────┐
              │    server.js        │
              │    (Node.js)        │
              └─────────┬──────────┘
                        │ execFile()
                        │
              ┌─────────▼──────────┐
              │      ph2.exe        │
              │  (compiled ph2.c)   │
              └─────────┬──────────┘
                        │
         ┌──────────────┼──────────────┐
         ▼              ▼              ▼
  input_phase2.txt  output_phase2.txt  sim_status.json
                    terminal_logs.txt
```

**File inventory:**

| File | Language | Lines | Size | Purpose |
|---|---|---|---|---|
| `ph2.c` | C11 | 691 | 20 KB | OS simulator |
| `index.html` | HTML/CSS/JS | 1437 | 94 KB | Live dashboard |
| `server.js` | Node.js | 225 | 8 KB | HTTP server |
| `run.bat` | Batch | 22 | 0.5 KB | Compile + launch |
| `input_phase2.txt` | Text | 35 | 0.7 KB | Job definitions |
| `output_phase2.txt` | Text | — | varies | Simulator output |
| `terminal_logs.txt` | Text | — | varies | Execution trace |
| `sim_status.json` | JSON | — | ~4 KB | Live register/memory dump |

---

## 3. The C Simulator (`ph2.c`) — Detailed Design

### 3.1 Memory Architecture

The simulator models a flat physical memory of **300 words**, each word being 4 characters (the natural unit of one instruction or one data item).

```
Physical Memory Layout (300 words × 4 chars)
┌────────────────────────────────────────────────────┐
│  Word 0  │  Word 1  │  Word 2  │ ... │  Word 299  │
│ 4 chars  │ 4 chars  │ 4 chars  │     │  4 chars   │
└────────────────────────────────────────────────────┘

Divided into 30 frames of 10 words each:
Frame 0  = words   0–9
Frame 1  = words  10–19
Frame 2  = words  20–29
...
Frame 29 = words 290–299
```

**Virtual address space per job:** 100 words (10 pages × 10 words). Each job has a page table of 10 entries. A virtual address `VA` maps as:

```
page   = VA / 10
offset = VA % 10
PTE    = M[PTR + page]           ← look up page table entry
frame  = PTE[2..3] as 2-digit number
RA     = frame × 10 + offset     ← real (physical) address
```

A PTE value of `  **` (asterisks in columns 2–3) means the page is not mapped — accessing it triggers a page fault (PI=3).

### 3.2 Data Structures

#### `JobContext` struct (per-job saved state)

This is the core innovation of the multiprogramming implementation. Every job has its own `JobContext` that stores its complete CPU state:

```c
typedef struct {
    char JID[5];        // 4-digit job identifier
    int  TTL;           // Total Time Limit (max instructions)
    int  TLL;           // Total Line Limit (max PD outputs)

    char IR[4];         // Instruction Register — current instruction
    char R[4];          // General-purpose Register
    int  IC;            // Instruction Counter — next virtual address to fetch
    int  C;             // Compare flag — set by CR, tested by BT
    int  PTR;           // Page Table Register — base address of page table in M

    int  SI;            // System Interrupt (1=GD, 2=PD, 3=H)
    int  PI;            // Program Interrupt (1=bad opcode, 2=operand, 3=page fault)
    int  TI;            // Time Interrupt (2=exceeded)

    int  TTC;           // Total Time Counter — instructions executed so far
    int  LLC;           // Line Limit Counter — PD outputs so far

    int  T;             // Halt flag — set when job terminates
    int  flag;          // PD output newline flag
    int  gen_address;   // Computed real address for current operand
    int  dataOffset;    // ftell() offset of job's first data line in input file
    int  currentLine;   // Current read position for GD() — advances per read
    int  status;        // STATUS_READY / STATUS_RUNNING / STATUS_DONE
    int  Program_card_counter; // Page table slot index during loading
} JobContext;
```

The `cur` global pointer always points to the currently executing job's context. All instruction functions (`GD`, `PD`, `LR`, `SR`, `CR`, `BT`, `ADDRESS_MAP`, `MOS`, `HALT`) operate exclusively through `cur->*` — this is what makes context switching trivial: changing `cur` to point to a different `JobContext` is the entire context switch.

#### Global frame allocator

```c
int allocated_frames[30];   // 1 = frame in use, 0 = free
int total_frames_used;      // current allocated count
int peak_frames_used;       // high-water mark
```

This is **global and never reset between jobs**. When a job halts, `HALT()` calls `Free_frame()` for every frame in its page table, returning them to the pool for other jobs. This is true shared physical memory.

### 3.3 Registers

| Register | Type | Description |
|---|---|---|
| `IR[4]` | `char[4]` | Instruction Register — 4-char instruction currently executing |
| `R[4]` | `char[4]` | General-purpose register — holds data between LR/SR/CR |
| `IC` | `int` | Instruction Counter — virtual address of next instruction to fetch |
| `PTR` | `int` | Page Table Register — physical base address of job's page table |
| `C` | `int` | Compare flag — set to 1 by `CR` if R == M[operand], tested by `BT` |

### 3.4 Interrupt System

Three interrupt types with **priority order PI > TI > SI**:

#### PI — Program Interrupt (hardware errors)
| Code | Cause | Effect |
|---|---|---|
| 1 | Invalid opcode | `HALT(4)` — OPERATION CODE ERROR |
| 2 | Invalid operand | `HALT(5)` — OPERAND ERROR |
| 3 | Page fault | Allocate new frame and retry, or `HALT(6)` if memory full |

#### TI — Time Interrupt (CPU scheduling)
| Code | Cause | Effect |
|---|---|---|
| 2 | `TTC > TTL` | `HALT(3)` — TIME LIMIT EXCEEDED (after completing current PD if active) |

The time interrupt is set when `TTC` exceeds `TTL` during the increment step in `execute_one_instruction()`. It fires at the top of the next instruction cycle, ensuring the job gets credit for the instruction that triggered it.

#### SI — System Interrupt (OS services)
| Code | Cause | Effect |
|---|---|---|
| 1 | `GD` instruction | Read next data line from input file into memory |
| 2 | `PD` instruction | Write 40 chars from memory to output file |
| 3 | `H` instruction | Normal halt |

**Priority enforcement in `MOS()`:** PI is checked first. If PI is set alongside TI, combined halt codes 7 and 8 are issued. TI is checked second. SI is only reached if both PI and TI are clear.

### 3.5 Instruction Set

| Mnemonic | Encoding | Operation | Interrupt raised |
|---|---|---|---|
| `GDxx` | `G`,`D`,digit,digit | Read next input line into memory at VA `xx` | SI=1 |
| `PDxx` | `P`,`D`,digit,digit | Write 40 chars from VA `xx` to output | SI=2 |
| `LRxx` | `L`,`R`,digit,digit | Copy M[VA `xx`] → R | — |
| `SRxx` | `S`,`R`,digit,digit | Copy R → M[VA `xx`] | — |
| `CRxx` | `C`,`R`,digit,digit | Set C=1 if R == M[VA `xx`], else C=0 | — |
| `BTxx` | `B`,`T`,digit,digit | If C==1: IC←`xx`, C←0 (branch taken) | — |
| `H   ` | `H`,space,space,space | Halt normally | SI=3 |

Each instruction occupies exactly one 4-character word in memory. The operand `xx` is a 2-digit virtual address (00–99).

### 3.6 Halt Codes

| Code | Message |
|---|---|
| 0 | No Error |
| 1 | OUT OF DATA |
| 2 | LINE LIMIT EXCEEDED |
| 3 | TIME LIMIT EXCEEDED |
| 4 | OPERATION CODE ERROR |
| 5 | OPERAND ERROR |
| 6 | INVALID PAGE FAULT |
| 7 | TIME LIMIT EXCEEDED AND OPERATION CODE ERROR |
| 8 | TIME LIMIT EXCEEDED AND OPERAND ERROR |

### 3.7 Multiprogramming — Two-Phase Execution

#### Phase 1: `load_all_jobs()` — Two-pass loader

The loader makes a single sequential pass through `input_phase2.txt` and loads **all jobs simultaneously** before any execution begins:

```
For each $AMJ card:
  1. Allocate one frame for this job's page table
  2. Initialize all 10 PTEs to ** (unmapped)
  3. Store TTL, TLL, JID into this job's JobContext

For each program card (between $AMJ and $DTA):
  1. Allocate one frame for the instructions
  2. Write the PTE: M[PTR + slot] = "1 FF" (frame number FF)
  3. Copy instruction characters into the frame, 4 chars per word

At $DTA:
  1. Record ftell() offset as job's dataOffset and currentLine
     (GD() will seek here when it needs to read data)
  2. Increment job_count — this job is fully loaded
  3. Skip remaining data lines (GD reads them lazily at runtime)
```

After `load_all_jobs()` returns, all jobs exist simultaneously in physical memory, each with their own frames and page tables. No execution has occurred yet.

#### Phase 2: `run_scheduler()` — Round-robin dispatcher

```
active = job_count
idx = 0

while active > 0:
    find next job with status == STATUS_RUNNING (starting from idx)
    set cur = &jobs[idx]
    result = execute_one_instruction()

    if result == EXEC_HALTED:
        active--
    else if result == EXEC_YIELD:   ← TI fired
        cur->TI = 0                 ← clear for next slice
        (job stays in rotation)

    idx = (idx + 1) % job_count     ← always advance, wrap around
```

The scheduler gives every active job exactly **one instruction per turn**. After each instruction:
- If the job halted or errored → it is marked DONE and removed from active count
- If TI fired → the job is yielded (stays RUNNING) and the next job gets a turn
- Otherwise → the job simply yields to the next one and will be dispatched again next round

This means in a 7-job run, instruction 1 of job 1 executes, then instruction 1 of job 2, ..., then instruction 1 of job 7, then instruction 2 of job 1, etc. This is genuine multiprogramming — all jobs make progress concurrently.

#### `execute_one_instruction()` — Single instruction cycle

```
1. ADDRESS_MAP(IC)      → fetch real address of next instruction
   if page fault:  MOS() → allocate frame or HALT(6)
                   return EXEC_HALTED

2. IR ← M[real_addr]   copy 4 chars into IR
3. IC++                 advance instruction counter
4. TTC++               increment time counter
5. if TTC > TTL: TI=2  set time interrupt

6. if T==1 or TI==2:   job has halted or time expired
   MOS()
   return EXEC_HALTED or EXEC_YIELD

7. Decode IR and dispatch:
   H    → SI=3, MOS()          (normal halt)
   GD   → lazy map, ADDRESS_MAP(operand), SI=1, MOS()
   PD   → lazy map, ADDRESS_MAP(operand), SI=2, MOS()
   LR/SR/CR/BT → execute directly
   else → PI=1, MOS()          (bad opcode)

8. return EXEC_OK (or EXEC_HALTED if job terminated during step 7)
```

#### Context switching

Because all CPU state lives in `JobContext` and all functions operate through `cur`, switching jobs is a single pointer assignment:

```c
cur = &jobs[next_job_index];
```

No register saving, no stack switching — the entire context is already in the struct.

### 3.8 Page Management

**At load time:** One frame is allocated per program card. All data pages start as `**` (unmapped).

**At runtime (lazy/demand paging):** When `GD` or `PD` is about to execute, `execute_one_instruction()` checks if the operand's page is mapped. If not, it allocates a new frame and writes the PTE before calling `ADDRESS_MAP`. This is demand paging.

**On page fault (PI=3):** If `ADDRESS_MAP` returns -1 during instruction fetch (not operand resolution), `MOS()` handles PI=3 by allocating a new frame for the faulting page — but only if the instruction is `GD`, `PD`, `LR`, or `SR`. For other instructions (including `BT`), page faults at the fetch stage result in `HALT(6)`.

**On job termination:** `HALT()` iterates over all 10 PTE slots, frees every mapped frame, then frees the page table frame itself. Physical memory is immediately available for other jobs.

**Frame capacity:** With 30 frames and 7 jobs each needing at minimum 2 frames (1 page table + 1 program frame), all 7 jobs can be loaded simultaneously. Peak observed usage: **24 of 30 frames** during the test run.

### 3.9 I/O Implementation

**GD (Get Data):** Opens `input_phase2.txt`, seeks to `cur->currentLine` (the per-job file offset), reads one line, writes it into physical memory at `cur->gen_address` (40 chars max, 10 words), advances `cur->currentLine`, and closes the file. Each job's `currentLine` starts at its `$DTA` offset and advances independently — this is how two jobs can be reading different data lines from the same file without interfering.

**PD (Print Data):** Opens `output_phase2.txt` in append mode, writes a job header on first call (`--- Output for Job XXXX ---`), writes 10 words (40 chars) sanitized of non-printable characters. Increments `cur->LLC` and checks against `cur->TLL` before writing.

**Output interleaving:** Because jobs run round-robin and PD is a system call that happens within a single instruction turn, PD output from different jobs is interleaved in the output file in execution order. Each job's output block is labeled with its job ID.

---

## 4. Test Jobs

### 4.1 Input File Format

```
$AMJ<JID><TTL><TLL>    ← fixed-width: 4+4+4 = 16 chars minimum
<program card>          ← 4-char instruction pairs, up to 40 chars (10 instructions)
$DTA                   ← marks end of program, start of data
<data line 1>          ← read by first GD instruction
<data line 2>          ← read by second GD instruction (if any)
$END<JID>              ← closes this job block
```

Field widths: JID = 4 chars (zero-padded), TTL = 4 chars (right-aligned integer), TLL = 4 chars. No delimiters between fields — columns are fixed.

### 4.2 Job Definitions and Expected Results

| Job | TTL | TLL | Instructions | Data | Halt reason | Result |
|---|---|---|---|---|---|---|
| 0001 | 10 | 10 | GD20 PD20 LR20 SR23 BT07 GD19 H | "Hello, Myself Rut" | Second GD finds no more data | OUT OF DATA |
| 0002 | 15 | 12 | GD15 PD10 LR15 SR18 BT05 GD12 H | "Operating System Simulation..." | Second GD finds no more data | OUT OF DATA |
| 0003 | 20 | 15 | GD25 PD15 LR25 SR30 BT10 GD20 H | Two data lines | H reached | No Error |
| 0004 | 10 | 8  | GD10 PD05 LR10 SR12 BT04 GD08 H | "Short Program" | Second GD finds no data | OUT OF DATA |
| 0005 | 25 | 20 | GD30 PD25 LR30 SR35 BT12 GD28 H | Three data lines | H reached | No Error |
| 0006 | 5  | 10 | LR99 PD99 H | "Page Fault Test" | H reached (page fault handled) | No Error |
| 0007 | 2  | 10 | Chain of 10×(LR SR) + BT H | "TTL Error Test" | TTL=2, hits limit at instruction 3 | TIME LIMIT EXCEEDED |

### 4.3 Execution Order with Round-Robin

With 7 jobs interleaving, the first few turns look like:

```
Turn 1:  Job 0001 executes GD20     (loads "Hello, Myself Rut" into memory)
Turn 2:  Job 0002 executes GD15     (loads "Operating System..." into memory)
Turn 3:  Job 0003 executes GD25     (loads "This is a test..." into memory)
Turn 4:  Job 0004 executes GD10     (loads "Short Program" into memory)
Turn 5:  Job 0005 executes GD30     (loads "Large test program..." into memory)
Turn 6:  Job 0006 executes LR99     (page fault → frame allocated, retried)
Turn 7:  Job 0007 executes GD10     (loads "TTL Error Test" into memory)
Turn 8:  Job 0001 executes PD20     (prints "Hello, Myself Rut" to output)
Turn 9:  Job 0002 executes PD10     (prints output)
...
Turn 18: Job 0006 executes H        (halts normally — No Error, IC=2)
Turn 19: Job 0007 executes LR10     (TTC=3 > TTL=2 → TIME LIMIT EXCEEDED)
...
```

Jobs 0006 and 0007 terminate early (small TTL). The remaining 5 jobs continue interleaving until they each reach their own termination condition.

---

## 5. The Node.js Server (`server.js`)

A minimal HTTP server with no external dependencies — pure Node.js standard library.

### 5.1 API Endpoints

| Method | Path | Behaviour |
|---|---|---|
| `GET` | `/api/health` | Returns `{"ok":true}` — used by dashboard on startup |
| `POST` | `/api/run` | Executes `ph2.exe` via `execFile`. Returns 409 if already running, 500 if binary missing. On success: `{ok, stdout, durationMs}`. 30-second timeout. |
| `GET` | `/api/input` | Reads and returns `input_phase2.txt` as plain text |
| `POST` | `/api/input` | Writes request body to `input_phase2.txt`. Validates that body contains at least one `$AMJ` marker. Returns 400 on invalid content, 409 if a run is in progress. |

### 5.2 Static File Serving

An allowlist controls which files can be fetched. Any path not in this list returns 404:

```
index.html         → text/html
sim_status.json    → application/json
output_phase2.txt  → text/plain
terminal_logs.txt  → text/plain
input_phase2.txt   → text/plain
```

Binds to `127.0.0.1:4173` only — no external network access.

---

## 6. The Dashboard (`index.html`)

A single self-contained HTML file. 37 JavaScript functions, all vanilla JS with inline SVG charts — no external libraries or build tools.

### 6.1 Architecture

The dashboard maintains a `state` object in memory:

```js
const state = {
    status: null,          // parsed sim_status.json (registers + memory array)
    selectedJob: '',       // '' | 'all' | '0001'...'0007'
    outputText: '',        // raw output_phase2.txt
    logText: '',           // raw terminal_logs.txt
    outputAvailable: false,
    logAvailable: false,
    statusAvailable: false,
    autoRefresh: true,
    jobIds: [],            // dynamically built from output file
    runBusy: false,
    refreshBusy: false
};
```

`refreshAll()` fetches all three data files in parallel (`Promise.allSettled`), updates state, then calls all render functions. This runs every 3 seconds automatically, pausing when the browser tab is hidden.

### 6.2 Dashboard Sections

#### Overview metrics (4 cards)
Current JID, IC, TTC/TTL, LLC/TLL — sourced from `sim_status.json`.

#### Machine registers
IR, R, C, PTR, IC displayed as individual register cells. When a specific job is selected, values come from the parsed output file (R, C, PTR are unavailable per-job — only the last simulator state has them).

#### Job limits
Two animated progress bars: execution time (TTC/TTL) and output lines (LLC/TLL). Bars turn red at 85% usage. Halt badge shown with color coding: green for No Error, amber for Out of Data, red for errors.

#### All-jobs table
Dynamically built from `output_phase2.txt` — no hardcoded job IDs. Each row shows: JID, halt badge, TTC/TTL, LLC/TLL, final IR. Click any row to filter the entire dashboard to that job.

#### Charts (4 panels)
- **Time usage** — horizontal bar chart, bar width = TTC as % of TTL, turns red when exceeded
- **Line usage** — same style for LLC/TLL
- **Halt reasons** — SVG donut chart grouping all jobs by outcome category (No Error / Out of Data / Time Exceeded / Page Fault / Other)
- **Instructions per job** — vertical SVG bar chart of final IC per job with y-axis grid lines

All charts use inline SVG, no canvas or external charting library.

#### Program output
Filtered by selected job or "All jobs". Strips trailing whitespace per line before display. Download and copy buttons. Job selector rebuilt dynamically from output file on each refresh.

#### Execution activity
Filtered log trace from `terminal_logs.txt`. Searchable with real-time filter. Newest-first toggle. Color-coded: red dots for errors/faults/halts, amber for warnings, green for success events.

#### Memory map
A 30×10 grid of 300 cells, each representing one memory word. Color-coded by content type:
- **Purple** — page table frame (PTR to PTR+9)
- **Green** — program code frame (detected by opcode pattern matching)
- **Amber** — data frame (mapped but not code)
- **Blue with outline** — word currently pointed to by IC
- **Grey** — free/unmapped

Each cell shows its 4-char content at tiny scale. Hover shows a tooltip with physical address, frame number, offset, and raw content.

#### Page table visualizer
Automatically scans the memory snapshot for frames that look like page tables (majority of words match PTE format). For each found frame, renders a card with 10 rows — one per virtual page — showing:
- Virtual page number → physical frame number (with colored arrow)
- Code or Data type badge based on what the frame contains
- `** (fault)` for unmapped pages
- Current job's page table highlighted with a purple border

#### Input file editor
Full textarea loaded from `/api/input`. Shows an unsaved-changes dot (amber) when content differs from the saved version. Three action buttons:
- **Save** — POST to `/api/input`
- **Save & Run** — Save then immediately POST to `/api/run`
- **Reset** — restore to last saved version

Ctrl+S saves from anywhere on the page (even when focus is elsewhere).

### 6.3 Additional Features

**Dark mode:** Full CSS custom property dark theme. Persists to `localStorage`. Respects `prefers-color-scheme` on first load. Toggle button in top bar.

**Auto-refresh:** 3-second polling interval. Pauses when tab is hidden (`visibilitychange`). Connection badge shows: online (green) / partial (amber) / offline (red) / paused (grey).

**Keyboard shortcuts:**

| Key | Action |
|---|---|
| `R` | Run simulation |
| `F5` | Refresh data |
| `D` | Toggle dark mode |
| `1`–`7` | Select job by number |
| `E` | Jump to input editor |
| `Ctrl+S` | Save input file |
| `Esc` | Clear selection / close overlay |
| `?` | Show shortcuts overlay |

---

## 7. Complete Data Flow

```
User clicks "Run simulation"
  │
  ▼
POST /api/run  →  server.js checks runInProgress flag
  │
  ▼
execFile('ph2.exe')  →  starts C simulator process
  │
  ├─ main(): clear frame table, clear memory, init output files
  │
  ├─ load_all_jobs():
  │   ├─ For each $AMJ:  Allocate frame → init page table → store PCB
  │   ├─ For each card:  Allocate frame → write PTE → load instructions
  │   └─ At $DTA:        record dataOffset, increment job_count
  │
  └─ run_scheduler():
      ├─ Mark all jobs STATUS_RUNNING
      └─ Round-robin loop:
          ├─ cur = &jobs[idx]
          ├─ execute_one_instruction():
          │   ├─ Fetch: ADDRESS_MAP(IC) → get IR from M
          │   ├─ IC++, TTC++
          │   ├─ Check TI (time limit)
          │   ├─ Decode IR → call GD/PD/LR/SR/CR/BT/H/MOS
          │   │   ├─ GD: fseek to cur->currentLine, fgets, load into M
          │   │   ├─ PD: append to output_phase2.txt
          │   │   └─ MOS: handle PI/TI/SI → HALT if needed
          │   └─ return EXEC_OK / EXEC_HALTED / EXEC_YIELD
          ├─ if HALTED: active--, HALT() frees all frames
          ├─ if YIELD: TI reset, job stays in rotation
          └─ idx = (idx+1) % job_count  ← round-robin advance

ph2.exe exits (code 0)
  │
  ▼
server.js returns {ok: true, durationMs: N}
  │
  ▼
dashboard: refreshAll()
  ├─ fetch sim_status.json  →  registers + 300-word memory array
  ├─ fetch output_phase2.txt  →  job outputs + halt summaries
  └─ fetch terminal_logs.txt  →  full execution trace
  │
  ▼
Render: metrics, registers, job table, charts, output,
        logs, memory map, page tables
```

---

## 8. Bugs Fixed During Development

| # | Bug | Root cause | Impact | Fix applied |
|---|---|---|---|---|
| 1 | `GD()` double-read | Throwaway first `fgets` before the real read | Every GD read the wrong data line | Removed the throwaway read |
| 2 | Memory not freed between jobs | Old `INIT()` only cleared memory, not frame allocation table | Later jobs had fewer frames, could collide | Reset frame table per job in `INIT()`; now freed in `HALT()` with global table |
| 3 | `Program_card_counter` not reset | `INIT()` did not reset it | Later jobs wrote PTEs starting at wrong slot | Reset to 0 in `INIT()` (now per-job struct field) |
| 4 | `currentLine` reset in `INIT()` | Old code set `currentLine = 0` on each job init | Jobs after the first read data from byte 0 of the file | Removed reset; `currentLine` now per-job in `JobContext` |
| 5 | Duplicate HALT on job 0006 | `MOS()` fell through from SI handler into PI handler | Same job appeared twice in output file | `MOS()` returns immediately after `H` |
| 6 | Wrong page fault operand calculation | `(x)*10 + (y)/10` — operator precedence error | Wrong page number computed on fault | Fixed to `((x)*10 + (y)) / 10` |
| 7 | Interrupt priority wrong | Old `MOS()` checked TI first, then SI, then PI | Wrong halt code when PI and TI both set | Reordered to PI > TI > SI per MOS spec |
| 8 | Page fault handler wrong opcode check | `IR[0]=='G'\|\|IR[0]=='S'` caught `SR` and not just `GD` | Page fault allocated frames for `SR` to wrong pages | Changed to explicit 2-char checks: `GD`, `PD`, `SR`, `LR` |
| 9 | `Allocate()` infinite loop | No bounds check; random search with no exit if all frames occupied | Simulator hung permanently when memory full | Added guard `if (generated_no_index >= 29)` + linear fallback |
| 10 | IC off-by-one in halt output | IC incremented before execution; halt printed the next IC | Halt report showed wrong instruction address | Print `IC-1` in `HALT()` |
| 11 | `write_status_file()` on every instruction | Called at end of every instruction cycle | Very slow for large jobs (file I/O per instruction) | Write every 10 instructions |
| 12 | PD printed raw memory bytes | No sanitization of non-printable chars | Page table bytes appeared in output (e.g. `1 15`) | Sanitize chars: replace `< 32` or `>= 127` with space |
| 13 | `$AMJ` with zero TTL/TLL silently accepted | `atoi` returns 0 for bad input; no validation | Jobs with TTL=0 halted on every instruction | Validate `TTL > 0 && TLL > 0`; skip and log if invalid |
| 14 | Load+execute interleaved in single pass | Old `Load()` called `EXECUTE()` inline at `$DTA` | No possibility of loading multiple jobs before execution | Separated into `load_all_jobs()` + `run_scheduler()` |

---

## 9. OS Concepts Demonstrated

| Concept | Where implemented |
|---|---|
| **Virtual memory** | `ADDRESS_MAP()` — VA→PA translation via page table |
| **Demand paging** | Lazy frame allocation in `execute_one_instruction()` for GD/PD |
| **Page fault handling** | PI=3 path in `MOS()` — allocate frame and retry |
| **Physical memory management** | Global `allocated_frames[]` with `Allocate()` / `Free_frame()` |
| **Process Control Block** | `JobContext` struct — per-process saved state |
| **Context switching** | `cur = &jobs[next]` — pointer swap to restore saved state |
| **CPU scheduling** | `run_scheduler()` — round-robin, one instruction per quantum |
| **Interrupt handling** | `MOS()` with priority order PI > TI > SI |
| **System calls** | SI=1 (GD), SI=2 (PD), SI=3 (H) — user→OS transitions |
| **Time-sharing** | TI fired when TTC > TTL — job yielded, scheduler advances |
| **Memory protection** | Each job has private page table; unmapped pages → page fault |
| **I/O management** | GD/PD open/close files directly (simplified spooling) |
| **Job accounting** | TTC, LLC tracked per job; halt summary printed to output |

---

## 10. Remaining Limitations

| Limitation | Detail |
|---|---|
| **No page replacement** | `Allocate()` returns -1 when all 30 frames are full. With enough jobs/pages this would halt with INVALID PAGE FAULT rather than evict an old page. LRU or FIFO replacement not implemented. |
| **No memory protection between jobs** | A job that constructs a virtual address mapping to a frame owned by another job can read/write that memory. No cross-job access checking is done. |
| **No true I/O blocking** | GD/PD open files synchronously during execution. A real OS would put the job in a WAITING state and schedule another job while I/O completes. |
| **Fixed quantum of 1** | Each job gets exactly one instruction per turn. A configurable quantum (N instructions per turn) is not implemented. |
| **No priority scheduling** | All jobs are treated equally by the round-robin scheduler. Priority queues or multi-level feedback queues are not implemented. |
| **Single input file** | All jobs must exist in `input_phase2.txt` before the run. No dynamic job submission or arrival time simulation. |

---

## 11. How to Run

### Quick start (Windows)
```bat
run.bat
```
This compiles `ph2.c` → `ph2.exe` using GCC, starts `server.js` on port 4173, and opens `http://localhost:4173` in your browser automatically.

**Prerequisites:** GCC (MinGW-w64, added to PATH) and Node.js.

### Verify prerequisites
```powershell
gcc --version
node --version
```

### Run simulator only (no dashboard)
```
ph2.exe
```
Output → `output_phase2.txt`  
Full trace → `terminal_logs.txt`  
Live register+memory snapshot → `sim_status.json`

### Using the dashboard
1. Open `http://localhost:4173`
2. Click **Run simulation** (or press `R`)
3. Data auto-refreshes every 3 seconds
4. Click any job row in the All Jobs table to filter all panels to that job
5. Press `?` for the full keyboard shortcuts overlay
6. Use the Input Editor section to modify `input_phase2.txt` and re-run without leaving the browser

---

*Report generated from final project state — ph2.c (691 lines), index.html (1437 lines), server.js (225 lines).*
