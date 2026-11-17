/*
 * MOS Phase 2 — Multiprogramming Operating System Simulator
 *
 * Design:
 *   - All jobs are loaded into memory simultaneously (each gets its own page table frame)
 *   - A round-robin scheduler dispatches one instruction at a time to each active job
 *   - On TI (time interrupt) the job is switched out; the scheduler moves to the next one
 *   - On halt/error the job is marked DONE and removed from the queue
 *   - Physical frames are shared globally — frames stay allocated across the entire run
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

/* ── Constants ──────────────────────────────────────────────────────────── */
#define MAX_JOBS     20
#define MEM_SIZE     300
#define FRAME_COUNT  30
#define WORDS_PER_FRAME 10
#define TERM_LOG_FILE "terminal_logs.txt"

/* Job execution status */
#define STATUS_READY   0
#define STATUS_RUNNING 1
#define STATUS_DONE    2

/* Return codes from execute_one_instruction() */
#define EXEC_OK       0  /* instruction completed, job still running */
#define EXEC_HALTED   1  /* job halted (normal or error) */
#define EXEC_YIELD    2  /* time interrupt fired — reschedule */

/* ── Per-job context (saved/restored on every context switch) ─────────── */
typedef struct {
    /* PCB */
    char JID[5];
    int  TTL;           /* Total Time Limit */
    int  TLL;           /* Total Line Limit */

    /* CPU registers */
    char IR[4];
    char R[4];
    int  IC;
    int  C;             /* compare flag */
    int  PTR;           /* page table base address in M */

    /* Interrupt flags */
    int  SI, PI, TI;

    /* Counters */
    int  TTC;           /* total time counter */
    int  LLC;           /* line limit counter */

    /* Misc per-job state */
    int  T;             /* halt flag */
    int  flag;          /* PD newline flag */
    int  gen_address;   /* computed real address for current instruction */
    int  dataOffset;    /* ftell offset of first data line after $DTA */
    int  currentLine;   /* current read position in input file for GD */
    int  status;        /* STATUS_READY / STATUS_RUNNING / STATUS_DONE */
    int  Program_card_counter; /* page table slot index during load */
} JobContext;

/* ── Global shared state ────────────────────────────────────────────────── */
char M[MEM_SIZE][4];     /* main memory — shared across all jobs */

/* Global frame allocator — never reset between jobs */
int  allocated_frames[FRAME_COUNT];  /* frame index → 1 if occupied */
int  frame_alloc_order[FRAME_COUNT]; /* FIFO queue for page replacement */
int  frame_alloc_head = 0;           /* next slot to evict (FIFO) */
int  total_frames_used = 0;
int  peak_frames_used  = 0;

/* Job table */
JobContext jobs[MAX_JOBS];
int        job_count = 0;

/* Currently executing job (pointer into jobs[]) — used by instruction helpers */
JobContext *cur = NULL;

int  VERBOSE = 2; /* 0=errors only, 1=halts, 2=all instructions */

/* ── Prototypes ─────────────────────────────────────────────────────────── */
int  Allocate();
int  ADDRESS_MAP(int VA);
void HALT(int msg);
void write_status_file();
void init_output_file();
int  execute_one_instruction();
void MOS();
void GD();
void PD();
void LR();
void SR();
void CR();
void BT();
void load_all_jobs();
void run_scheduler();

/* ── Logging ────────────────────────────────────────────────────────────── */
void LOG(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    FILE *lf = fopen(TERM_LOG_FILE, "a");
    if (lf) {
        fprintf(lf, "%s", buf);
        if (buf[strlen(buf)-1] != '\n') fprintf(lf, "\n");
        fclose(lf);
    }
    if (VERBOSE >= 2) {
        printf("%s", buf);
        if (buf[strlen(buf)-1] != '\n') printf("\n");
    }
}

/* ── Frame allocator ─────────────────────────────────────────────────────── */
int Allocate()
{
    /* Find a free frame */
    for (int f = 0; f < FRAME_COUNT; f++) {
        if (!allocated_frames[f]) {
            allocated_frames[f] = 1;
            frame_alloc_order[frame_alloc_head % FRAME_COUNT] = f;
            frame_alloc_head++;
            total_frames_used++;
        if (total_frames_used > peak_frames_used) peak_frames_used = total_frames_used;
            LOG("[ALLOC] Allocated frame %d (JID=%.4s)", f, cur ? cur->JID : "----");
            return f;
        }
    }
    LOG("[ALLOC] ERROR: All %d frames occupied!", FRAME_COUNT);
    return -1;
}

void Free_frame(int frame)
{
    if (frame >= 0 && frame < FRAME_COUNT) {
        allocated_frames[frame] = 0;
        total_frames_used--;
        /* Clear the frame's memory words */
        for (int w = 0; w < WORDS_PER_FRAME; w++)
            for (int b = 0; b < 4; b++)
                M[frame * WORDS_PER_FRAME + w][b] = ' ';
        LOG("[FREE] Freed frame %d", frame);
    }
}

/* ── Address translation ─────────────────────────────────────────────────── */
int ADDRESS_MAP(int VA)
{
    int page_no = VA / WORDS_PER_FRAME;
    int offset  = VA % WORDS_PER_FRAME;
    int PTE     = cur->PTR + page_no;

    if (M[PTE][2] == '*' && M[PTE][3] == '*') {
        cur->PI = 3;
        LOG("[ADDR_MAP] JID=%.4s Page fault VA=%d (page %d) PTE=%d",
            cur->JID, VA, page_no, PTE);
        return -1;
    }
    int frame = (M[PTE][2] - '0') * 10 + (M[PTE][3] - '0');
    int ra    = frame * WORDS_PER_FRAME + offset;
    LOG("[ADDR_MAP] JID=%.4s VA=%d -> frame=%d offset=%d -> RA=%d",
        cur->JID, VA, frame, offset, ra);
    return ra;
}

/* ── Status file (JSON snapshot of current job + memory) ────────────────── */
void write_status_file()
{
    FILE *sf = fopen("sim_status.json", "w");
    if (!sf) return;

    char irs[5], rs[5];
    for (int i = 0; i < 4; i++) {
        irs[i] = (cur && cur->IR[i]) ? cur->IR[i] : ' ';
        rs[i]  = (cur && cur->R[i])  ? cur->R[i]  : ' ';
    }
    irs[4] = rs[4] = '\0';

    fprintf(sf, "{\n");
    fprintf(sf, "  \"JID\": \"%.4s\",\n",  cur ? cur->JID : "----");
    fprintf(sf, "  \"IC\": %d,\n",         cur ? cur->IC  : 0);
    fprintf(sf, "  \"IR\": \"%s\",\n",     irs);
    fprintf(sf, "  \"R\": \"%s\",\n",      rs);
    fprintf(sf, "  \"PTR\": %d,\n",        cur ? cur->PTR : 0);
    fprintf(sf, "  \"C\": %d,\n",          cur ? cur->C   : 0);
    fprintf(sf, "  \"TTC\": %d,\n",        cur ? cur->TTC : 0);
    fprintf(sf, "  \"TLL\": %d,\n",        cur ? cur->TLL : 0);
    fprintf(sf, "  \"TTL\": %d,\n",        cur ? cur->TTL : 0);
    fprintf(sf, "  \"LLC\": %d,\n",        cur ? cur->LLC : 0);

    /* Active jobs list */
    fprintf(sf, "  \"jobs\": [");
    int first = 1;
    for (int i = 0; i < job_count; i++) {
        if (!first) fprintf(sf, ",");
        fprintf(sf, "{\"JID\":\"%.4s\",\"status\":%d,\"TTC\":%d,\"TTL\":%d}",
                jobs[i].JID, jobs[i].status, jobs[i].TTC, jobs[i].TTL);
        first = 0;
    }
    fprintf(sf, "],\n");

    /* Memory dump */
    fprintf(sf, "  \"M\": [\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        char word[5];
        for (int j = 0; j < 4; j++) {
            char c = M[i][j];
            word[j] = (c >= 32 && c < 127) ? c : ' ';
        }
        word[4] = '\0';
        fprintf(sf, "    \"");
        for (int j = 0; j < 4; j++) {
            if (word[j] == '\\')      fprintf(sf, "\\\\");
            else if (word[j] == '"')  fprintf(sf, "\\\"");
            else                      fputc(word[j], sf);
        }
        fprintf(sf, (i < MEM_SIZE - 1) ? "\",\n" : "\"\n");
    }
    fprintf(sf, "  ]\n}\n");
    fclose(sf);
}

/* ── HALT ────────────────────────────────────────────────────────────────── */
void HALT(int msg)
{
    cur->T = 1;

    static const char *msgs[] = {
        "  No Error",
        "  OUT OF DATA",
        "  LINE LIMIT EXCEEDED",
        "  TIME LIMIT EXCEEDED",
        "  OPERATION CODE ERROR",
        "  OPERAND ERROR",
        "  INVALID PAGE FAULT",
        "  TIME LIMIT EXCEEDED AND OPERATION CODE ERROR",
        "  TIME LIMIT EXCEEDED AND OPERAND ERROR"
    };
    const char *text = (msg >= 0 && msg <= 8) ? msgs[msg] : "  UNKNOWN";

    int ic_printed = cur->IC > 0 ? cur->IC - 1 : cur->IC;

    FILE *fp = fopen("output_phase2.txt", "a");
    if (fp) {
        fprintf(fp, "\nJob ID: %.4s\n%s\n", cur->JID, text);
        fprintf(fp, "IC : %d\n",  ic_printed);
        fprintf(fp, "IR : %c%c%c%c\n", cur->IR[0], cur->IR[1], cur->IR[2], cur->IR[3]);
        fprintf(fp, "TTC : %d\n", cur->TTC);
        fprintf(fp, "TTL : %d\n", cur->TTL);
        fprintf(fp, "LLC : %d\n", cur->LLC);
        fprintf(fp, "TLL : %d\n\n", cur->TLL);
        fclose(fp);
    }

    if (VERBOSE >= 1) {
        LOG("[HALT] JID=%.4s %s", cur->JID, text);
        LOG("IC : %d  IR : %c%c%c%c  TTC : %d  TTL : %d  LLC : %d  TLL : %d",
            ic_printed,
            cur->IR[0], cur->IR[1], cur->IR[2], cur->IR[3],
            cur->TTC, cur->TTL, cur->LLC, cur->TLL);
    }

    /* Free all frames belonging to this job */
    for (int pg = 0; pg < WORDS_PER_FRAME; pg++) {
        int pte = cur->PTR + pg;
        if (M[pte][2] != '*' && M[pte][2] != ' ') {
            int frame = (M[pte][2] - '0') * 10 + (M[pte][3] - '0');
            Free_frame(frame);
        }
    }
    /* Free the page table frame itself */
    Free_frame(cur->PTR / WORDS_PER_FRAME);

    cur->status = STATUS_DONE;
    write_status_file();
}

/* ── Instruction helpers (operate on cur->*) ────────────────────────────── */
void GD()
{
    FILE *fp = fopen("input_phase2.txt", "r");
    if (!fp) { HALT(1); return; }

    fseek(fp, cur->currentLine, SEEK_SET);
    char line[200];
    if (fgets(line, sizeof(line), fp) == NULL || strncmp(line, "$END", 4) == 0) {
        fclose(fp);
        HALT(1);
        return;
    }
    cur->currentLine = ftell(fp);
    fclose(fp);

    /* Write line into memory at gen_address */
    int row = cur->gen_address;
    for (int i = 0; i < WORDS_PER_FRAME; i++)
        for (int j = 0; j < 4; j++)
            M[row + i][j] = ' ';
    int pos = 0;
    for (int i = 0; line[i] && line[i] != '\n' && line[i] != '\r' && pos < 40; i++) {
        int word = pos / 4, offset = pos % 4;
        if (row + word < MEM_SIZE)
            M[row + word][offset] = line[i];
        pos++;
    }

    cur->SI = 0;
    LOG("[GD] JID=%.4s loaded data at addr=%d: %.40s", cur->JID, row, line);
}

void PD()
{
    cur->LLC++;
    if (cur->LLC > cur->TLL) { HALT(2); return; }

    FILE *fp = fopen("output_phase2.txt", "a");
    if (!fp) { LOG("[PD] ERROR: cannot open output file"); return; }

    /* On first PD for this job, print a header line */
    if (cur->flag == 0)
        fprintf(fp, "\n--- Output for Job %.4s ---\n", cur->JID);
    else
        fprintf(fp, "\n");
    cur->flag++;
    for (int i = cur->gen_address; i < cur->gen_address + WORDS_PER_FRAME; i++) {
        char c0 = (M[i][0]>=32&&M[i][0]<127)?M[i][0]:' ';
        char c1 = (M[i][1]>=32&&M[i][1]<127)?M[i][1]:' ';
        char c2 = (M[i][2]>=32&&M[i][2]<127)?M[i][2]:' ';
        char c3 = (M[i][3]>=32&&M[i][3]<127)?M[i][3]:' ';
        fprintf(fp, "%c%c%c%c", c0, c1, c2, c3);
    }
    fclose(fp);

    cur->SI = 0;
    LOG("[PD] JID=%.4s printed block at addr=%d", cur->JID, cur->gen_address);
}

void LR() { for (int i=0;i<4;i++) cur->R[i] = M[cur->gen_address][i]; }
void SR() { for (int i=0;i<4;i++) M[cur->gen_address][i] = cur->R[i]; }
void CR() { cur->C = (memcmp(cur->R, M[cur->gen_address], 4) == 0); }
void BT() {
    if (cur->C) {
        cur->IC = (cur->IR[2]-'0')*10 + (cur->IR[3]-'0');
        cur->C  = 0;
    }
}

/* ── MOS interrupt handler ───────────────────────────────────────────────── */
void MOS()
{
    /* PI > TI > SI */

    if (cur->PI != 0) {
        if (cur->TI == 2) {
            if (cur->PI == 1) { HALT(7); cur->PI=0; cur->TI=0; return; }
            if (cur->PI == 2) { HALT(8); cur->PI=0; cur->TI=0; return; }
        }
        switch (cur->PI) {
        case 1: HALT(4); break;
        case 2: HALT(5); break;
        case 3:
            if ((cur->IR[0]=='G'&&cur->IR[1]=='D') ||
                (cur->IR[0]=='P'&&cur->IR[1]=='D') ||
                (cur->IR[0]=='S'&&cur->IR[1]=='R') ||
                (cur->IR[0]=='L'&&cur->IR[1]=='R')) {
                int page  = ((cur->IR[2]-'0')*10 + (cur->IR[3]-'0')) / WORDS_PER_FRAME;
                int frame = Allocate();
                if (frame == -1) { HALT(6); break; }
                M[cur->PTR+page][2] = (frame/10)+'0';
                M[cur->PTR+page][3] = (frame%10)+'0';
                cur->PI = 0;
                return; /* retry the instruction */
            }
            HALT(6);
            break;
        }
        cur->PI = 0;
        return;
    }

    if (cur->TI == 2) {
        if (cur->IR[0]=='P'&&cur->IR[1]=='D') PD();
        else if (cur->IR[0]=='H'&&cur->IR[1]==' ') { HALT(0); cur->TI=0; return; }
        HALT(3);
        cur->TI = 0;
        return;
    }

    switch (cur->SI) {
    case 1: GD(); break;
    case 2: PD(); break;
    case 3: HALT(0); cur->SI=0; return;
    }
    cur->SI = 0;
}

/* ── Single instruction executor ─────────────────────────────────────────── */
int execute_one_instruction()
{
    /* Fetch */
    int addr = ADDRESS_MAP(cur->IC);
    if (addr == -1) { MOS(); return EXEC_HALTED; }

    for (int i = 0; i < 4; i++) cur->IR[i] = M[addr][i];
    cur->IC++;
    cur->TTC++;

    if (cur->TTC > cur->TTL) cur->TI = 2;
    if (cur->T == 1 || cur->TI == 2) {
        MOS();
        return (cur->status == STATUS_DONE) ? EXEC_HALTED : EXEC_YIELD;
    }

    if (VERBOSE >= 2)
        LOG("[EXEC] JID=%.4s IC=%d IR=%c%c%c%c TTC=%d",
            cur->JID, cur->IC-1, cur->IR[0],cur->IR[1],cur->IR[2],cur->IR[3], cur->TTC);

    int operand = (cur->IR[2]-'0')*10 + (cur->IR[3]-'0');

    /* Lazy page allocation for GD/PD */
    if ((cur->IR[0]=='G'&&cur->IR[1]=='D')||(cur->IR[0]=='P'&&cur->IR[1]=='D')) {
        int page = operand / WORDS_PER_FRAME;
        int PTE  = cur->PTR + page;
        if (M[PTE][2]=='*'&&M[PTE][3]=='*') {
            int nf = Allocate();
            if (nf == -1) { HALT(6); return EXEC_HALTED; }
            M[PTE][2] = (nf/10)+'0';
            M[PTE][3] = (nf%10)+'0';
        }
    }

    /* H doesn't need an operand address — handle before ADDRESS_MAP(operand) */
    if (cur->IR[0]=='H' && cur->IR[1]==' ') { cur->SI=3; MOS();
        return (cur->status==STATUS_DONE) ? EXEC_HALTED : EXEC_OK; }

    cur->gen_address = ADDRESS_MAP(operand);

    if      (cur->IR[0]=='G'&&cur->IR[1]=='D') { cur->SI=1; MOS(); }
    else if (cur->IR[0]=='P'&&cur->IR[1]=='D') { cur->SI=2; MOS(); }
    else if (cur->IR[0]=='L'&&cur->IR[1]=='R') { LR(); }
    else if (cur->IR[0]=='S'&&cur->IR[1]=='R') { SR(); }
    else if (cur->IR[0]=='C'&&cur->IR[1]=='R') { CR(); }
    else if (cur->IR[0]=='B'&&cur->IR[1]=='T') { BT(); }
    else { cur->PI=1; MOS(); }
    if (cur->status == STATUS_DONE) return EXEC_HALTED;
    return EXEC_OK;
}

/* ── Two-pass loader ──────────────────────────────────────────────────────── */
void load_all_jobs()
{
    FILE *fp = fopen("input_phase2.txt", "r");
    if (!fp) { fprintf(stderr, "Cannot open input_phase2.txt\n"); exit(1); }

    char line[200];
    JobContext *j = NULL;

    while (fgets(line, sizeof(line), fp) != NULL) {

        if (strncmp(line, "$AMJ", 4) == 0) {
            if (job_count >= MAX_JOBS) {
                LOG("[LOAD] Max job limit (%d) reached — skipping", MAX_JOBS);
                while (fgets(line, sizeof(line), fp))
                    if (strncmp(line, "$END", 4) == 0) break;
                continue;
            }

            /* Validate header length */
            if ((int)strlen(line) < 16) {
                LOG("[LOAD] Malformed $AMJ header — skipping");
                while (fgets(line, sizeof(line), fp))
                    if (strncmp(line, "$END", 4) == 0) break;
                continue;
            }

            j = &jobs[job_count];
            memset(j, 0, sizeof(JobContext));

            strncpy(j->JID, line + 4, 4); j->JID[4] = '\0';
            char tmp[5];
            strncpy(tmp, line+8,  4); tmp[4]='\0'; j->TTL = atoi(tmp);
            strncpy(tmp, line+12, 4); tmp[4]='\0'; j->TLL = atoi(tmp);

            if (j->TTL <= 0 || j->TLL <= 0) {
                LOG("[LOAD] Job %.4s has invalid TTL/TLL — skipping", j->JID);
                while (fgets(line, sizeof(line), fp))
                    if (strncmp(line, "$END", 4) == 0) break;
                j = NULL;
                continue;
            }

            /* Allocate page table frame */
            cur = j; /* Allocate() uses cur->JID for logging */
            int ptf = Allocate();
            if (ptf == -1) {
                LOG("[LOAD] Out of frames for page table of job %.4s — skipping", j->JID);
                while (fgets(line, sizeof(line), fp))
                    if (strncmp(line, "$END", 4) == 0) break;
                j = NULL; cur = NULL;
                continue;
            }
            j->PTR = ptf * WORDS_PER_FRAME;

            /* Mark all 10 PTE entries as unmapped (**) */
            for (int i = j->PTR; i < j->PTR + WORDS_PER_FRAME; i++) {
                M[i][0] = M[i][1] = ' ';
                M[i][2] = M[i][3] = '*';
            }

            j->status = STATUS_READY;
            j->Program_card_counter = 0;
            j->dataOffset   = -1;
            j->currentLine  = -1;

            LOG("[LOAD] Job %.4s  TTL=%d  TLL=%d  PTR=%d",
                j->JID, j->TTL, j->TLL, j->PTR);
        }

        else if (j && strncmp(line, "$DTA", 4) == 0) {
            /* Record the file position of the first data line */
            j->dataOffset  = (int)ftell(fp);
            j->currentLine = j->dataOffset;
            LOG("[LOAD] Job %.4s  data starts at offset %d", j->JID, j->dataOffset);
            job_count++;
            /* skip data lines — they will be read by GD() at runtime */
        }

        else if (j && strncmp(line, "$END", 4) == 0) {
            /* job_count was incremented at $DTA; nothing to do here */
            j = NULL;
        }

        else if (j && j->dataOffset < 0) {
            /* Program card — load into memory */
            cur = j;
            int frame = Allocate();
            if (frame == -1) {
                LOG("[LOAD] Out of frames loading job %.4s — aborting job", j->JID);
                while (fgets(line, sizeof(line), fp))
                    if (strncmp(line, "$END", 4) == 0) break;
                /* free previously allocated frames for this job */
                for (int pg = 0; pg < j->Program_card_counter; pg++) {
                    int pf = (M[j->PTR+pg][2]-'0')*10 + (M[j->PTR+pg][3]-'0');
                    Free_frame(pf);
                }
                Free_frame(j->PTR / WORDS_PER_FRAME);
                j = NULL; cur = NULL;
                continue;
            }

            /* Write PTE */
            int slot = j->Program_card_counter;
            M[j->PTR + slot][0] = '1';
            M[j->PTR + slot][2] = (frame/10)+'0';
            M[j->PTR + slot][3] = (frame%10)+'0';
            j->Program_card_counter++;

            /* Copy instruction words into the frame */
            int base = frame * WORDS_PER_FRAME;
            int col = 0, wi = base;
            for (int k = 0; k < WORDS_PER_FRAME; k++)
                for (int b = 0; b < 4; b++)
                    M[base + k][b] = ' ';
            for (int k = 0; line[k] && line[k]!='\n' && line[k]!='\r'; k++) {
                M[wi][col++] = line[k];
                if (col == 4) { col = 0; wi++; if (wi >= base+WORDS_PER_FRAME) break; }
            }
            LOG("[LOAD] Job %.4s  program card → frame %d (addr %d..%d)",
                j->JID, frame, base, base+9);
        }
        /* data lines after $DTA are skipped here — GD() reads them at runtime */
    }

    fclose(fp);
    LOG("[LOAD] Loaded %d job(s) into memory.", job_count);
}

/* ── Round-robin scheduler ───────────────────────────────────────────────── */
void run_scheduler()
{
    if (job_count == 0) { LOG("[SCHED] No jobs to run."); return; }

    int active = job_count;  /* instructions executed in current time slice */

    LOG("\n[SCHED] Starting round-robin scheduler with %d job(s)\n", job_count);

    /* Mark all loaded jobs as RUNNING (they are ready to go) */
    for (int i = 0; i < job_count; i++)
        jobs[i].status = STATUS_RUNNING;

    int idx = 0;   /* index into jobs[] of currently executing job */

    while (active > 0) {
        /* Find next active job */
        int found = 0;
        for (int t = 0; t < job_count; t++) {
            int candidate = (idx + t) % job_count;
            if (jobs[candidate].status == STATUS_RUNNING) {
                idx   = candidate;
                found = 1;
                break;
            }
        }
        if (!found) break;

        cur = &jobs[idx];

        LOG("[SCHED] Dispatching job %.4s (IC=%d TTC=%d/%d)",
            cur->JID, cur->IC, cur->TTC, cur->TTL);

        int result = execute_one_instruction();

        if (result == EXEC_HALTED) {
            LOG("[SCHED] Job %.4s terminated.", cur->JID);
            active--;
        } else if (result == EXEC_YIELD) {
            /* Time interrupt fired — log context switch */
            LOG("[SCHED] Job %.4s time-sliced (TTC=%d). Switching.",
                cur->JID, cur->TTC);
            cur->TI = 0; /* clear for next slice */
        }

        /* Write status file periodically */
        if ((cur->TTC % 10) == 0)
            write_status_file();

        /* Advance to next job in round-robin */
        idx = (idx + 1) % job_count;
    }

    LOG("\n[SCHED] All jobs finished.\n");
    write_status_file();
}

/* ── Output file init ────────────────────────────────────────────────────── */
void init_output_file()
{
    FILE *fp = fopen("output_phase2.txt", "w");
    if (!fp) { fprintf(stderr, "Cannot create output_phase2.txt\n"); exit(1); }
    fclose(fp);
    FILE *lf = fopen(TERM_LOG_FILE, "w");
    if (lf) fclose(lf);
    LOG("[INIT] Output files cleared.");
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main()
{
    srand((unsigned)time(NULL));

    /* Clear global frame table */
    memset(allocated_frames, 0, sizeof(allocated_frames));
    memset(frame_alloc_order, -1, sizeof(frame_alloc_order));
    total_frames_used = 0;
    frame_alloc_head  = 0;

    /* Clear memory */
    for (int i = 0; i < MEM_SIZE; i++)
        for (int j = 0; j < 4; j++)
            M[i][j] = ' ';

    init_output_file();

    LOG("MOS Phase 2 — Multiprogramming Simulator\n");
    LOG("=========================================\n");

    load_all_jobs();
    run_scheduler();

    /* Final summary to output file */
    FILE *fp = fopen("output_phase2.txt", "a");
    if (fp) {
        fprintf(fp, "\n========================================\n");
        fprintf(fp, "  MOS Simulation Complete — %d job(s)\n", job_count);
        fprintf(fp, "  Frames used peak: %d / %d\n", peak_frames_used, FRAME_COUNT);
        fprintf(fp, "========================================\n");
        fclose(fp);
    }

    LOG("[MAIN] Simulation complete. %d job(s) processed.", job_count);
    return 0;
}
