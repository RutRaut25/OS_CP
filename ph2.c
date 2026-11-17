#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

// --- Global Variables ---
char M[300][4]; // Main memory (300 words of 4 characters each)
char IR[5];     // Instruction Register
char R[4];      // General-purpose Register
int IC;         // Instruction Counter
int C;          // Toggle/flag for comparison results
int PTR;        // Page Table Register

int SI; // System Interrupt code
int PI; // Program Interrupt code
int TI; // Time Interrupt code

struct PCB
{
    char JID[5];
    int TTL; // Total Time Limit
    int TLL; // Total Line Limit
} info;

int TTC; // Total Time Counter
int LLC; // Total Line Limit Counter

int generated_number[100];
int generated_no_index;
int Program_card_couter;

int currentLine;
char str[200];
int T;
int gen_address;
int flag = 0;
int VERBOSE = 1; // when 1, print process events to terminal
// Log file name for terminal/debug logs
#define TERM_LOG_FILE "terminal_logs.txt"

// --- Prototypes ---
int Allocate();
void print_memory_block();
int ADDRESS_MAP(int VA);
void HALT(int msg);
void Load();
void INIT();
void EXECUTE();
void EXECUTE_USER_PROGRAM();
void MOS();
void GD();
void PD();
void H();
void LR();
void SR();
void BT();
void CR();
void init_output_file();
void write_status_file();

// Helper to write terminal logs both to a persistent file and to stdout (when VERBOSE)
void LOG(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    FILE *lf = fopen(TERM_LOG_FILE, "a");
    if (lf)
    {
        fprintf(lf, "%s", buf);
        /* Ensure newline for easier display in UI */
        if (buf[strlen(buf)-1] != '\n')
            fprintf(lf, "\n");
        fclose(lf);
    }

    if (VERBOSE)
    {
        /* print to stdout as before */
        printf("%s", buf);
        if (buf[strlen(buf)-1] != '\n')
            printf("\n");
    }
}

// Write a small JSON status file used by the web UI to show registers/counters.
void write_status_file()
{
    FILE *sf = fopen("sim_status.json", "w");
    if (!sf) return;
    // IR and R may contain non-printable spaces; write them as 4-char strings
    char irs[5];
    char rs[5];
    for (int i = 0; i < 4; ++i) { irs[i] = IR[i] ? IR[i] : ' '; rs[i] = R[i] ? R[i] : ' '; }
    irs[4] = '\0'; rs[4] = '\0';

    fprintf(sf, "{\n");
    fprintf(sf, "  \"JID\": \"%.4s\",\n", info.JID);
    fprintf(sf, "  \"IC\": %d,\n", IC);
    fprintf(sf, "  \"IR\": \"%s\",\n", irs);
    fprintf(sf, "  \"R\": \"%s\",\n", rs);
    fprintf(sf, "  \"PTR\": %d,\n", PTR);
    fprintf(sf, "  \"C\": %d,\n", C);
    fprintf(sf, "  \"TTC\": %d,\n", TTC);
    fprintf(sf, "  \"TLL\": %d,\n", info.TLL);
    fprintf(sf, "  \"TTL\": %d,\n", info.TTL);
    fprintf(sf, "  \"LLC\": %d\n", LLC);
    fprintf(sf, "}\n");
    fclose(sf);
}

// Allocate a free frame (0..29)
int Allocate()
{
    int address, l;
    while (1)
    {
        address = rand() % 30;
        l = 0;
        for (int i = 0; i <= generated_no_index; i++)
        {
            if (generated_number[i] == address)
            {
                l = 1;
                break;
            }
        }
        if (l == 0)
            break;
    }
    generated_no_index++;
    generated_number[generated_no_index] = address;
    /* Always save an allocation event to terminal log; LOG will also print when VERBOSE is true */
    LOG("[ALLOC] Allocated frame %d", address);
    return address;
}

// Map virtual address to real address using page table
int ADDRESS_MAP(int VA)
{
    int page_no = VA / 10;
    int offset = VA % 10;
    int PTE = PTR + page_no;
    if (M[PTE][2] == '*' && M[PTE][3] == '*')
    {
        PI = 3; // Page Fault
        LOG("[ADDR_MAP] Page fault for VA=%d (page %d) at PTE=%d", VA, page_no, PTE);
        return -1;
    }
    int frame = (M[PTE][2] - '0') * 10 + (M[PTE][3] - '0');
    LOG("[ADDR_MAP] VA=%d -> frame=%d offset=%d -> RA=%d", VA, frame, offset, frame * 10 + offset);
    return frame * 10 + offset;
}

// Halt function
void HALT(int msg)
{
    T = 1;
    FILE *fp = fopen("output_phase2.txt", "a");
    if (!fp)
    {
        printf("Error opening output file!\n");
        return;
    }

    fprintf(fp, "\nJob ID: %.4s\n", info.JID);
    switch (msg)
    {
    case 0:
        fputs("  No Error\n", fp);
        break;
    case 1:
        fputs("  OUT OF DATA\n", fp);
        break;
    case 2:
        fputs("  LINE LIMIT EXCEEDED\n", fp);
        break;
    case 3:
        fputs("  TIME LIMIT EXCEEDED\n", fp);
        break;
    case 4:
        fputs("  OPERATION CODE ERROR\n", fp);
        break;
    case 5:
        fputs("  OPERAND ERROR\n", fp);
        break;
    case 6:
        fputs("  INVALID PAGE FAULT\n", fp);
        break;
    case 7:
        fputs("  TIME LIMIT EXCEEDED AND OPERATION CODE ERROR\n", fp);
        break;
    case 8:
        fputs("  TIME LIMIT EXCEEDED AND OPERAND ERROR\n", fp);
        break;
    }
    fprintf(fp, "IC : %d\n", IC);
    fprintf(fp, "IR : %c%c%c%c\n", IR[0], IR[1], IR[2], IR[3]);
    fprintf(fp, "TTC : %d\n", TTC);
    fprintf(fp, "TTL : %d\n", info.TTL);
    fprintf(fp, "LLC : %d\n", LLC);
    fprintf(fp, "TLL : %d\n\n", info.TLL);
    fclose(fp);
    /* Log halt summary to terminal logs and stdout (LOG handles VERBOSE printing) */
    LOG("[HALT] Job ID: %.4s", info.JID);
    switch (msg)
    {
    case 0:
        LOG("  No Error");
        break;
    case 1:
        LOG("  OUT OF DATA");
        break;
    case 2:
        LOG("  LINE LIMIT EXCEEDED");
        break;
    case 3:
        LOG("  TIME LIMIT EXCEEDED");
        break;
    case 4:
        LOG("  OPERATION CODE ERROR");
        break;
    case 5:
        LOG("  OPERAND ERROR");
        break;
    case 6:
        LOG("  INVALID PAGE FAULT");
        break;
    case 7:
        LOG("  TIME LIMIT EXCEEDED AND OPERATION CODE ERROR");
        break;
    case 8:
        LOG("  TIME LIMIT EXCEEDED AND OPERAND ERROR");
        break;
    }
    LOG("IC : %d", IC);
    LOG("IR : %c%c%c%c", IR[0], IR[1], IR[2], IR[3]);
    LOG("TTC : %d", TTC);
    LOG("TTL : %d", info.TTL);
    LOG("LLC : %d", LLC);
    LOG("TLL : %d", info.TLL);
    /* write final status for UI */
    write_status_file();
}

// Initialize memory and registers
void INIT()
{
    for (int i = 0; i < 300; i++)
        for (int j = 0; j < 4; j++)
            M[i][j] = ' ';
    for (int i = 0; i < 4; i++)
    {
        IR[i] = ' ';
        R[i] = ' ';
    }
    IC = 0;
    SI = 0;
    C = 0;
    T = 0;
    currentLine = 0;
    info.TTL = 0;
    info.TLL = 0;
    TTC = 0;
    LLC = 0;
    PI = 0;
    TI = 0;
    gen_address = 0;
    flag = 0;
    generated_no_index = -1;
    Program_card_couter = 0;
    srand((unsigned)time(NULL));
    write_status_file();
}

// Load program and data
void Load()
{
    FILE *fp = fopen("input_phase2.txt", "r");
    if (!fp)
    {
        LOG("Input file not found!");
        exit(1);
    }

    while (fgets(str, sizeof(str), fp) != NULL)
    {
        currentLine = ftell(fp);
        if (strncmp(str, "$AMJ", 4) == 0)
        {
            INIT();
            strncpy(info.JID, str + 4, 4);
            info.JID[4] = '\0';
            char tmp[5];
            strncpy(tmp, str + 8, 4);
            tmp[4] = '\0';
            info.TTL = atoi(tmp);
            strncpy(tmp, str + 12, 4);
            tmp[4] = '\0';
            info.TLL = atoi(tmp);
            PTR = Allocate() * 10;
            for (int i = PTR; i < PTR + 10; i++)
                for (int j = 0; j < 4; j++)
                    M[i][j] = ' ';
            for (int i = PTR; i < PTR + 10; i++)
                M[i][2] = '*', M[i][3] = '*';
            LOG("\n[LOAD] New Job: JID=%.4s TTL=%d TLL=%d PTR=%d", info.JID, info.TTL, info.TLL, PTR);
        }
        else if (strncmp(str, "$DTA", 4) == 0)
        {
            LOG("[LOAD] Starting execution phase for Job: %.4s", info.JID);
            EXECUTE();
        }
        else if (strncmp(str, "$END", 4) == 0)
            printf("\nEND Job.\n\n");
        else
        {
            int temp = Allocate();
            int temp1 = temp * 10;
            M[PTR + Program_card_couter][0] = '1';
            M[PTR + Program_card_couter][2] = (temp / 10) + '0';
            M[PTR + Program_card_couter][3] = (temp % 10) + '0';
            Program_card_couter++;
            int col = 0, wordIndex = temp1;
            LOG("[LOAD] Loading program card into frame %d (real addr %d..%d): %s", temp, temp1, temp1 + 9, str);
            for (int i = 0; i < 4; i++)
                M[wordIndex][i] = ' ';
            for (int i = 0; i < strlen(str); i++)
            {
                if (str[i] == '\n' || str[i] == '\r')
                    break;
                M[wordIndex][col++] = str[i];
                if (col == 4)
                {
                    col = 0;
                    wordIndex++;
                    if (wordIndex < temp1 + 10)
                        for (int k = 0; k < 4; k++)
                            M[wordIndex][k] = ' ';
                }
            }
        }
    }
    fclose(fp);
}

// Execute program
void EXECUTE()
{
    IC = 0;
    EXECUTE_USER_PROGRAM();
}

void EXECUTE_USER_PROGRAM()
{
    while (1)
    {
        int addr = ADDRESS_MAP(IC);
        if (addr == -1)
        {
            MOS();
            break;
        }
        for (int i = 0; i < 4; i++)
            IR[i] = M[addr][i];
        IC++;
        TTC++;
        if (TTC > info.TTL)
            TI = 2;
        if (T == 1 || TI == 2)
        {
            MOS();
            break;
        }

        if (VERBOSE)
            LOG("[EXEC] JID=%.4s IC=%d IR=%c%c%c%c TTC=%d", info.JID, IC-1, IR[0], IR[1], IR[2], IR[3], TTC);

        int operand = (IR[2] - '0') * 10 + (IR[3] - '0');

        // Allocate data page if GD/PD and not yet mapped
        if ((IR[0] == 'G' && IR[1] == 'D') || (IR[0] == 'P' && IR[1] == 'D'))
        {
            int page = operand / 10;
            int PTE = PTR + page;
            if (M[PTE][2] == '*' && M[PTE][3] == '*')
            {
                int newFrame = Allocate();
                M[PTE][2] = (newFrame / 10) + '0';
                M[PTE][3] = (newFrame % 10) + '0';
            }
        }

        gen_address = ADDRESS_MAP(operand);

        if (IR[0] == 'G' && IR[1] == 'D')
        {
            SI = 1;
            LOG("[EXEC] GD instruction - requesting input for operand %d (gen_address=%d)", operand, gen_address);
            MOS();
        }
        else if (IR[0] == 'P' && IR[1] == 'D')
        {
            SI = 2;
            LOG("[EXEC] PD instruction - will write memory at gen_address=%d", gen_address);
            MOS();
        }
        else if (IR[0] == 'H' && IR[1] == ' ')
        {
            SI = 3;
            LOG("[EXEC] H (HALT) encountered");
            MOS();
            break;
        }
        else if (IR[0] == 'L' && IR[1] == 'R')
        {
            LOG("[EXEC] LR - load register from gen_address=%d", gen_address);
            LR();
        }
        else if (IR[0] == 'S' && IR[1] == 'R')
        {
            LOG("[EXEC] SR - store register to gen_address=%d", gen_address);
            SR();
        }
        else if (IR[0] == 'C' && IR[1] == 'R')
        {
            LOG("[EXEC] CR - compare register with memory at gen_address=%d", gen_address);
            CR();
        }
        else if (IR[0] == 'B' && IR[1] == 'T')
        {
            LOG("[EXEC] BT - branch if toggle set to address %d", (IR[2]-'0')*10 + (IR[3]-'0'));
            BT();
        }
        else
        {
            PI = 1;
            MOS();
            break;
        }
        /* Update UI status file after each instruction cycle so the web UI can show current registers/counters */
        write_status_file();
    }
}

// MOS handles interrupts
void MOS()
{
    if (TI == 0)
    {
        switch (SI)
        {
        case 1:
            GD();
            break;
        case 2:
            PD();
            break;
        case 3:
            H();
            break;
        }
        switch (PI)
        {
        case 1:
            HALT(4);
            break;
        case 2:
            HALT(5);
            break;
        case 3:
            if (IR[0] == 'G' || IR[0] == 'S')
            {
                int page = (IR[2] - '0') * 10 + (IR[3] - '0') / 10;
                int frame = Allocate();
                M[PTR + page][2] = (frame / 10) + '0';
                M[PTR + page][3] = (frame % 10) + '0';
                PI = 0;
            }
            else
                HALT(6);
            break;
        }
    }
    else if (TI == 2)
    {
        if (PI == 0)
        {
            if (IR[0] == 'P' && IR[1] == 'D')
                PD();
            else if (IR[0] == 'H' && IR[1] == ' ')
                H();
            HALT(3);
        }
        else if (PI == 1)
            HALT(7);
        else if (PI == 2)
            HALT(8);
        else if (PI == 3)
            HALT(3);
    }
}

// GD: read next line from input file
void GD()
{
    FILE *fp = fopen("input_phase2.txt", "r");
    if (!fp)
    {
        HALT(1);
        return;
    }
    fseek(fp, currentLine, SEEK_SET);
    if (!fgets(str, sizeof(str), fp))
    {
        fclose(fp);
        HALT(1);
        return;
    }
    if (fgets(str, sizeof(str), fp) == NULL || strncmp(str, "$END", 4) == 0)
    {
        SI = 1;
        fclose(fp);
        return; // do not call HALT here, let MOS handle it once
    }

    int row = gen_address;
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 4; j++)
            M[row + i][j] = ' ';
    int pos = 0;
    for (int i = 0; i < strlen(str); i++)
    {
        if (str[i] == '\n' || str[i] == '\r')
            break;
        int word = pos / 4, offset = pos % 4;
        if (row + word < 300)
            M[row + word][offset] = str[i];
        pos++;
        if (pos >= 40)
            break;
    }
    currentLine = ftell(fp);
    fclose(fp);
    SI = 0;
    LOG("[GD] Read from input and loaded into memory at gen_address=%d: %s", gen_address, str);
}

// PD: write 40 chars to output file
void PD()
{
    LLC++;
    if (LLC > info.TLL)
    {
        HALT(2);
        return;
    }
    FILE *fp = fopen("output_phase2.txt", "a");
    if (!fp)
    {
        printf("Cannot open output file!\n");
        return;
    }
    if (flag > 0)
        fprintf(fp, "\n");
    flag++;
    LOG("[PD] Writing output for JID=%.4s from gen_address=%d..%d", info.JID, gen_address, gen_address+9);
    for (int i = gen_address; i < gen_address + 10; i++)
    {
        fprintf(fp, "%c%c%c%c", M[i][0], M[i][1], M[i][2], M[i][3]);
        LOG("%c%c%c%c", M[i][0], M[i][1], M[i][2], M[i][3]);
    }
    LOG("(PD) finished writing block starting at %d", gen_address);
    fclose(fp);
}

// Other instructions
void H() { HALT(0); }
void LR()
{
    for (int i = 0; i < 4; i++)
        R[i] = M[gen_address][i];
}
void SR()
{
    for (int i = 0; i < 4; i++)
        M[gen_address][i] = R[i];
}
void BT()
{
    if (C == 1)
    {
        IC = (IR[2] - '0') * 10 + (IR[3] - '0');
        C = 0;
    }
}
void CR() { C = (memcmp(R, M[gen_address], 4) == 0); }
void init_output_file()
{
    FILE *fp = fopen("output_phase2.txt", "w");
    if (!fp)
    {
        LOG("Cannot create output_phase2.txt!");
        exit(1);
    }
    fclose(fp);

    /* Also clear terminal_logs.txt so we have a fresh log for this run */
    FILE *lf = fopen(TERM_LOG_FILE, "w");
    if (lf)
        fclose(lf);

    LOG("[INIT] Cleared/created output_phase2.txt and %s", TERM_LOG_FILE);
}

int main()
{
    LOG("MOS Implementation\n");
    init_output_file();
    INIT();
    Load();
    LOG("\n[MAIN] All jobs processed. Check output_phase2.txt for program outputs.");
    return 0;
}
