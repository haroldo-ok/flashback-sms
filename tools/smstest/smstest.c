/*
 * smstest - minimal headless Sega Master System emulator for automated
 * testing of homebrew ROMs built with devkitSMS.
 *
 * Emulates: Z80 (superzazu core), Sega mapper, 8KB RAM, VDP mode 4
 * (VRAM/CRAM, frame+line IRQ, vcounter, full background+sprite renderer
 * for screenshots), joypad ports.  PSG writes are accepted and logged.
 *
 * Driven by a simple script language:
 *   symbols <file.noi>          load NoICE symbols (DEF _name 0xADDR)
 *   hold <btn[+btn...]|none>    set held pad buttons (up,down,left,right,1,2)
 *   pause                       press the console's PAUSE button (an NMI)
 *   psglog <file> / psgstop     record sound chip writes (frame, byte)
 *   tap <btns>                  hold for 2 frames, then release
 *   run <n>                     run n frames
 *   waituntil <sym> <op> <val> <maxframes> [w1|w2|s16]
 *   expect <sym|0xADDR> <op> <val> [w1|w2|s16]   (default w2 unsigned LE)
 *   poke <sym|0xADDR> <val> [w1|w2]
 *   print <sym> [w1|w2|s16]
 *   screenshot <file.ppm>
 *   expectvdp display <0|1>
 *   echo <text>
 * Exit code 0 = all expectations met.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "z80.h"

#define RAM_SIZE 0x2000
#define MAX_ROM  (4*1024*1024)  /* full Sega mapper: 256 x 16K banks */
#define LINES_PER_FRAME 262
#define CYCLES_PER_LINE 228
#define ACTIVE_LINES 192

static uint8_t rom[MAX_ROM];
static uint32_t rom_size, rom_banks;
static uint8_t ram[RAM_SIZE];
static uint8_t mapper[4] = {0, 0, 1, 2}; /* FFFC..FFFF */

static uint8_t vram[0x4000];
static uint8_t cram[32];
static uint8_t vdp_reg[16];
static uint16_t vdp_addr;
static uint8_t vdp_code, vdp_latch, vdp_first, vdp_buf;
static uint8_t vdp_status;          /* bit7 = frame irq flag */
static uint8_t vdp_line_pending;    /* line irq flag */
static uint8_t line_counter;
static int cur_line;
static uint8_t pad1_state = 0xFF;   /* active low: U D L R B1 B2 */
static FILE* psg_log = NULL;        /* sound chip writes, for checking music */
static unsigned long psg_writes = 0;

static z80 cpu;
static unsigned long frame_no;
static int failures;

/* ------------------------------------------------------------------ */
static uint8_t mem_read(void* ud, uint16_t addr) {
    (void)ud;
    if (addr < 0x0400) return rom[addr];                 /* fixed first 1KB */
    if (addr < 0x4000) return rom[((mapper[1] % rom_banks) * 0x4000u) + addr];
    if (addr < 0x8000) return rom[((mapper[2] % rom_banks) * 0x4000u) + (addr - 0x4000)];
    if (addr < 0xC000) return rom[((mapper[3] % rom_banks) * 0x4000u) + (addr - 0x8000)];
    return ram[addr & 0x1FFF];
}
static void mem_write(void* ud, uint16_t addr, uint8_t val) {
    (void)ud;
    if (addr >= 0xFFFC) mapper[addr - 0xFFFC] = val;
    if (addr >= 0xC000) ram[addr & 0x1FFF] = val;
}

/* ------------------------------ VDP ------------------------------- */
static void vdp_ctrl_write(uint8_t v) {
    if (!vdp_first) { vdp_latch = v; vdp_first = 1; return; }
    vdp_first = 0;
    vdp_code = v >> 6;
    vdp_addr = ((v & 0x3F) << 8) | vdp_latch;
    if (vdp_code == 0) { vdp_buf = vram[vdp_addr & 0x3FFF]; vdp_addr++; }
    else if (vdp_code == 2) vdp_reg[v & 0x0F] = vdp_latch;
}
static void vdp_data_write(uint8_t v) {
    vdp_first = 0;
    if (vdp_code == 3) cram[vdp_addr & 0x1F] = v;
    else vram[vdp_addr & 0x3FFF] = v;
    vdp_addr = (vdp_addr + 1) & 0x3FFF;
    vdp_buf = v;
}
static uint8_t vdp_data_read(void) {
    uint8_t r = vdp_buf;
    vdp_first = 0;
    vdp_buf = vram[vdp_addr & 0x3FFF];
    vdp_addr = (vdp_addr + 1) & 0x3FFF;
    return r;
}
static uint8_t vdp_status_read(void) {
    uint8_t r = vdp_status;
    vdp_status = 0; vdp_line_pending = 0; vdp_first = 0;
    cpu.int_pending = false;
    return r;
}
static uint8_t vcount_read(void) {
    /* NTSC 192-line mode: 0x00-0xDA then 0xD5-0xFF */
    return (cur_line <= 0xDA) ? (uint8_t)cur_line : (uint8_t)(cur_line - 6);
}
static int vdp_irq_active(void) {
    if ((vdp_status & 0x80) && (vdp_reg[1] & 0x20)) return 1;
    if (vdp_line_pending && (vdp_reg[0] & 0x10)) return 1;
    return 0;
}

/* ------------------------------ I/O ------------------------------- */
static uint8_t port_in(z80* z, uint8_t port) {
    (void)z;
    if (port < 0x40) return 0xFF;
    if (port < 0x80) return (port & 1) ? 0xFF /*hcount*/ : vcount_read();
    if (port < 0xC0) return (port & 1) ? vdp_status_read() : vdp_data_read();
    if (!(port & 1)) return pad1_state | 0xC0;   /* 0xDC: P1 all + P2 UD (unpressed) */
    return 0xFF;                                  /* 0xDD */
}
static void port_out(z80* z, uint8_t port, uint8_t val) {
    (void)z;
    if (port >= 0x40 && port < 0x80) {                    /* PSG */
        if (psg_log) {
            fprintf(psg_log, "%lu %02X\n", frame_no, val);
            psg_writes++;
        }
        return;
    }
    if (port >= 0x80 && port < 0xC0) {
        if (port & 1) vdp_ctrl_write(val); else vdp_data_write(val);
    }
}

/* ------------------------- sampling profiler ----------------------
 * Counts cycles against the PC, in 32-byte buckets, so the cost of a frame can
 * be attributed to functions via the linker symbols.  Code-removal timing is
 * misleading in a simulation (removing work changes what the game does), so
 * measure instead. */
#define PROF_BUCKETS 2048
#define PROF_SHIFT   5
static unsigned long prof[PROF_BUCKETS];
static int profiling = 0;

static void prof_reset(void) { memset(prof, 0, sizeof prof); }

static void prof_dump(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) { printf("FAIL: cannot write %s\n", path); failures++; return; }
    for (int i = 0; i < PROF_BUCKETS; i++)
        if (prof[i]) fprintf(f, "%04X %lu\n", i << PROF_SHIFT, prof[i]);
    fclose(f);
    printf("profdump: %s (frame %lu)\n", path, frame_no);
}

/* --------------------------- frame loop --------------------------- */
static void run_line(void) {
    unsigned long target = cpu.cyc + CYCLES_PER_LINE;
    while (cpu.cyc < target) {
        if (vdp_irq_active() && cpu.iff1) cpu.int_pending = true;
        if (profiling) {
            unsigned long before = cpu.cyc;
            uint16_t pc = cpu.pc;
            z80_step(&cpu);
            prof[(pc >> PROF_SHIFT) & (PROF_BUCKETS - 1)] += cpu.cyc - before;
        } else {
            z80_step(&cpu);
        }
    }
}
static void run_frame(void) {
    for (cur_line = 0; cur_line < LINES_PER_FRAME; cur_line++) {
        if (cur_line <= ACTIVE_LINES) {
            if (cur_line == ACTIVE_LINES) vdp_status |= 0x80;   /* vblank */
            if (line_counter == 0) { vdp_line_pending = 1; line_counter = vdp_reg[10]; }
            else line_counter--;
        } else line_counter = vdp_reg[10];
        run_line();
    }
    frame_no++;
}

/* --------------------------- renderer ----------------------------- */
static void color_rgb(uint8_t c, uint8_t* rgb) {
    rgb[0] = (uint8_t)((c & 3) * 85);
    rgb[1] = (uint8_t)(((c >> 2) & 3) * 85);
    rgb[2] = (uint8_t)(((c >> 4) & 3) * 85);
}
static void dump_state(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot write %s\n", path); failures++; return; }
    fwrite(vram, 1, sizeof vram, f);
    fwrite(cram, 1, sizeof cram, f);
    fwrite(vdp_reg, 1, sizeof vdp_reg, f);
    fwrite(ram, 1, sizeof ram, f);        /* 8K work RAM, for inspecting game state */
    fclose(f);
    printf("dumpstate: %s (frame %lu)\n", path, frame_no);
}

static void render_screenshot(const char* path) {
    static uint8_t img[192][256][3];
    static uint8_t bg_over[192][256];   /* priority tile with a non-zero pixel */
    memset(bg_over, 0, sizeof bg_over);
    uint16_t ntbase = (uint16_t)(vdp_reg[2] & 0x0E) << 10;
    uint16_t satbase = (uint16_t)(vdp_reg[5] & 0x7E) << 7;
    uint16_t sprtiles = (vdp_reg[6] & 0x04) ? 0x2000 : 0x0000;
    int spr16 = vdp_reg[1] & 0x02;
    uint8_t xscroll = vdp_reg[8], yscroll = vdp_reg[9];
    int display_on = vdp_reg[1] & 0x40;
    for (int y = 0; y < 192; y++) for (int x = 0; x < 256; x++) {
        uint8_t rgb[3]; uint8_t bd = cram[16 + (vdp_reg[7] & 0x0F)];
        color_rgb(bd, rgb);
        memcpy(img[y][x], rgb, 3);
    }
    int lock_v = vdp_reg[0] & 0x80;   /* R0 b7: cols 24-31 ignore vscroll */
    int lock_h = vdp_reg[0] & 0x40;   /* R0 b6: rows 0-1 ignore hscroll  */
    if (display_on) {
        for (int y = 0; y < 192; y++) {
            for (int x = 0; x < 256; x++) {
                int ys = (lock_v && x >= 192) ? 0 : yscroll;
                int xs = (lock_h && y < 16)   ? 0 : xscroll;
                int sy = (y + ys) % 224;
                int sx = (x - xs) & 0xFF;
                uint16_t ent = ntbase + ((sy >> 3) * 32 + (sx >> 3)) * 2;
                uint16_t e = vram[ent] | (vram[ent + 1] << 8);
                uint16_t tile = e & 0x1FF;
                int hf = e & 0x200, vf = e & 0x400, pal = (e & 0x800) ? 16 : 0;
                int py = vf ? 7 - (sy & 7) : (sy & 7);
                int px = hf ? 7 - (sx & 7) : (sx & 7);
                uint16_t ta = tile * 32 + py * 4;
                int bit = 7 - px;
                uint8_t ci = ((vram[ta] >> bit) & 1) | (((vram[ta+1] >> bit) & 1) << 1)
                           | (((vram[ta+2] >> bit) & 1) << 2) | (((vram[ta+3] >> bit) & 1) << 3);
                color_rgb(cram[pal + ci], img[y][x]);
                bg_over[y][x] = (e & 0x1000) && ci;   /* priority bit: hides sprites */
            }
        }
        int sh = spr16 ? 16 : 8;
        /* find terminator */
        int nspr = 64;
        for (int i = 0; i < 64; i++) if (vram[satbase + i] == 0xD0) { nspr = i; break; }
        /* the VDP scans the SAT in order and displays at most 8 sprites per
         * scanline; the 9th and beyond are dropped on that line */
        static uint8_t shown[64][192];
        int percount[192];
        memset(shown, 0, sizeof shown);
        for (int y = 0; y < 192; y++) percount[y] = 0;
        for (int i = 0; i < nspr; i++) {
            int sy = vram[satbase + i] + 1;
            for (int ty = 0; ty < sh; ty++) {
                int y = sy + ty; if (y < 0 || y >= 192) continue;
                if (percount[y] < 8) shown[i][y] = 1;
            }
            for (int ty = 0; ty < sh; ty++) {
                int y = sy + ty; if (y < 0 || y >= 192) continue;
                percount[y]++;
            }
        }
        /* lower sprite index wins, so draw back to front */
        for (int i = nspr - 1; i >= 0; i--) {
            int sy = vram[satbase + i] + 1;
            int sx = vram[satbase + 128 + i * 2];
            uint16_t tile = vram[satbase + 128 + i * 2 + 1];
            if (spr16) tile &= 0xFE;
            for (int ty = 0; ty < sh; ty++) {
                int y = sy + ty; if (y < 0 || y >= 192) continue;
                if (!shown[i][y]) continue;
                uint16_t ta = sprtiles + (tile + (ty >> 3)) * 32 + (ty & 7) * 4;
                for (int tx = 0; tx < 8; tx++) {
                    int x = sx + tx; if (x < 0 || x >= 256) continue;
                    if (bg_over[y][x]) continue;
                    int bit = 7 - tx;
                    uint8_t ci = ((vram[ta] >> bit) & 1) | (((vram[ta+1] >> bit) & 1) << 1)
                               | (((vram[ta+2] >> bit) & 1) << 2) | (((vram[ta+3] >> bit) & 1) << 3);
                    if (ci) color_rgb(cram[16 + ci], img[y][x]);
                }
            }
        }
        if (vdp_reg[0] & 0x20)  /* left column blank */
            for (int y = 0; y < 192; y++) for (int x = 0; x < 8; x++) {
                color_rgb(cram[16 + (vdp_reg[7] & 0x0F)], img[y][x]);
            }
    }
    FILE* f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot write %s\n", path); failures++; return; }
    fprintf(f, "P6\n256 192\n255\n");
    fwrite(img, 1, sizeof img, f);
    fclose(f);
    printf("screenshot: %s (frame %lu)\n", path, frame_no);
}

/* --------------------------- symbols ------------------------------ */
typedef struct { char name[64]; uint32_t addr; } Sym;
static Sym syms[4096]; static int nsyms;

static void load_symbols(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { printf("FAIL: cannot open symbols %s\n", path); failures++; return; }
    char a[64], b[64], c[64];
    while (fscanf(f, "%63s %63s %63s", a, b, c) == 3) {
        if (strcmp(a, "DEF") == 0 && nsyms < 4096) {
            strncpy(syms[nsyms].name, b, 63);
            syms[nsyms].addr = (uint32_t)strtoul(c, NULL, 0);
            nsyms++;
        }
    }
    fclose(f);
    printf("symbols: loaded %d from %s\n", nsyms, path);
}
static int sym_lookup(const char* name, uint32_t* out) {
    char alt[66];
    snprintf(alt, sizeof alt, "_%s", name);
    for (int i = 0; i < nsyms; i++)
        if (!strcmp(syms[i].name, name) || !strcmp(syms[i].name, alt)) {
            *out = syms[i].addr; return 1;
        }
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        *out = (uint32_t)strtoul(name, NULL, 16); return 1;
    }
    return 0;
}

/* --------------------------- script ------------------------------- */
static uint32_t parse_buttons(const char* s) {
    uint32_t m = 0;
    char buf[128]; strncpy(buf, s, 127); buf[127] = 0;
    for (char* t = strtok(buf, "+"); t; t = strtok(NULL, "+")) {
        if (!strcmp(t, "up")) m |= 0x01;
        else if (!strcmp(t, "down")) m |= 0x02;
        else if (!strcmp(t, "left")) m |= 0x04;
        else if (!strcmp(t, "right")) m |= 0x08;
        else if (!strcmp(t, "1")) m |= 0x10;
        else if (!strcmp(t, "2")) m |= 0x20;
        else if (!strcmp(t, "none")) m |= 0;
        else printf("WARN: unknown button '%s'\n", t);
    }
    return m;
}
static void set_pad(uint32_t mask) { pad1_state = (uint8_t)(~mask); }

static long read_val(uint32_t addr, const char* width) {
    if (!strcmp(width, "w1")) return ram[addr & 0x1FFF];
    long v = ram[addr & 0x1FFF] | (ram[(addr + 1) & 0x1FFF] << 8);
    if (!strcmp(width, "s16") && v >= 0x8000) v -= 0x10000;
    return v;
}
static int cmp_op(long a, const char* op, long b) {
    if (!strcmp(op, "==")) return a == b;
    if (!strcmp(op, "!=")) return a != b;
    if (!strcmp(op, "<"))  return a < b;
    if (!strcmp(op, ">"))  return a > b;
    if (!strcmp(op, "<=")) return a <= b;
    if (!strcmp(op, ">=")) return a >= b;
    printf("WARN: bad op %s\n", op);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s rom.sms script.txt\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("rom"); return 2; }
    rom_size = (uint32_t)fread(rom, 1, MAX_ROM, f);
    fclose(f);
    rom_banks = rom_size / 0x4000; if (!rom_banks) rom_banks = 1;
    printf("rom: %s (%u bytes, %u banks)\n", argv[1], rom_size, rom_banks);

    z80_init(&cpu);
    cpu.read_byte = mem_read; cpu.write_byte = mem_write;
    cpu.port_in = port_in; cpu.port_out = port_out;

    FILE* sf = fopen(argv[2], "r");
    if (!sf) { perror("script"); return 2; }
    char line[512];
    while (fgets(line, sizeof line, sf)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char c1[128], c2[128], c3[128], c4[128], c5[128], c6[128];
        int n = sscanf(line, "%127s %127s %127s %127s %127s %127s", c1, c2, c3, c4, c5, c6);
        if (n < 1) continue;
        if (!strcmp(c1, "symbols") && n >= 2) load_symbols(c2);
        else if (!strcmp(c1, "hold") && n >= 2) set_pad(parse_buttons(c2));
        else if (!strcmp(c1, "tap") && n >= 2) {
            set_pad(parse_buttons(c2)); run_frame(); run_frame();
            set_pad(0); run_frame();
        }
        else if (!strcmp(c1, "run") && n >= 2) {
            long k = atol(c2);
            while (k-- > 0) run_frame();
        }
        else if (!strcmp(c1, "waituntil") && n >= 5) {
            uint32_t addr; long maxf = atol(c5);
            const char* w = (n >= 6) ? c6 : "w2";
            if (!sym_lookup(c2, &addr)) { printf("FAIL: no symbol %s\n", c2); failures++; continue; }
            long target = strtol(c4, NULL, 0);
            long i;
            for (i = 0; i < maxf; i++) {
                if (cmp_op(read_val(addr, w), c3, target)) break;
                run_frame();
            }
            if (i >= maxf) {
                printf("FAIL: waituntil %s %s %ld timed out after %ld frames (val=%ld)\n",
                       c2, c3, target, maxf, read_val(addr, w));
                failures++;
            } else printf("ok: waituntil %s %s %ld (after %ld frames)\n", c2, c3, target, i);
        }
        else if (!strcmp(c1, "expect") && n >= 4) {
            uint32_t addr;
            const char* w = (n >= 5) ? c5 : "w2";
            if (!sym_lookup(c2, &addr)) { printf("FAIL: no symbol %s\n", c2); failures++; continue; }
            long v = read_val(addr, w), target = strtol(c4, NULL, 0);
            if (cmp_op(v, c3, target)) printf("ok: %s = %ld %s %ld\n", c2, v, c3, target);
            else { printf("FAIL: %s = %ld, expected %s %ld\n", c2, v, c3, target); failures++; }
        }
        else if (!strcmp(c1, "poke") && n >= 3) {
            uint32_t addr;
            if (!sym_lookup(c2, &addr)) { printf("FAIL: no symbol %s\n", c2); failures++; continue; }
            long v = strtol(c3, NULL, 0);
            ram[addr & 0x1FFF] = (uint8_t)(v & 0xFF);
            if (n < 4 || strcmp(c4, "w1")) ram[(addr + 1) & 0x1FFF] = (uint8_t)((v >> 8) & 0xFF);
            printf("poke: %s <- %ld\n", c2, v);
        }
        else if (!strcmp(c1, "print") && n >= 2) {
            uint32_t addr;
            const char* w = (n >= 3) ? c3 : "w2";
            if (sym_lookup(c2, &addr))
                printf("print: %s = %ld (frame %lu)\n", c2, read_val(addr, w), frame_no);
        }
        else if (!strcmp(c1, "screenshot") && n >= 2) render_screenshot(c2);
        else if (!strcmp(c1, "dumpstate") && n >= 2) dump_state(c2);
        else if (!strcmp(c1, "pause")) { z80_gen_nmi(&cpu); run_frame(); printf("pause (frame %lu)\n", frame_no); }
        else if (!strcmp(c1, "psglog") && n >= 2) {
            psg_log = fopen(c2, "w");
            printf("psglog: %s (frame %lu)\n", c2, frame_no);
        }
        else if (!strcmp(c1, "psgstop")) {
            if (psg_log) { fclose(psg_log); psg_log = NULL; }
            printf("psgstop: %lu writes (frame %lu)\n", psg_writes, frame_no);
        }
        else if (!strcmp(c1, "profstart")) { prof_reset(); profiling = 1; printf("profstart (frame %lu)\n", frame_no); }
        else if (!strcmp(c1, "profstop")) { profiling = 0; printf("profstop (frame %lu)\n", frame_no); }
        else if (!strcmp(c1, "profdump") && n >= 2) prof_dump(c2);
        else if (!strcmp(c1, "expectcolors") && n >= 2) {
            /* Guards against "passing" on a blank/black frame: renders the
               current frame and counts distinct colours actually shown. */
            render_screenshot("/tmp/_smstest_check.ppm");
            FILE* pf = fopen("/tmp/_smstest_check.ppm", "rb");
            int distinct = 0;
            if (pf) {
                unsigned char seen[1 << 24 >> 16] = {0};  /* unused */
                (void)seen;
                char hdr[64];
                if (fgets(hdr, sizeof hdr, pf) && fgets(hdr, sizeof hdr, pf) && fgets(hdr, sizeof hdr, pf)) {
                    static unsigned char pal[256][3];
                    unsigned char px3[3];
                    while (fread(px3, 1, 3, pf) == 3) {
                        int found = 0;
                        for (int k = 0; k < distinct; k++)
                            if (!memcmp(pal[k], px3, 3)) { found = 1; break; }
                        if (!found && distinct < 256) { memcpy(pal[distinct], px3, 3); distinct++; }
                    }
                }
                fclose(pf);
            }
            int want = atoi(c2);
            if (distinct >= want) printf("ok: frame shows %d distinct colours (>= %d)\n", distinct, want);
            else { printf("FAIL: frame shows only %d distinct colours, expected >= %d (blank screen?)\n",
                          distinct, want); failures++; }
        }
        else if (!strcmp(c1, "expectvdp") && n >= 3 && !strcmp(c2, "display")) {
            int on = (vdp_reg[1] & 0x40) ? 1 : 0;
            if (on == atoi(c3)) printf("ok: vdp display %d\n", on);
            else { printf("FAIL: vdp display=%d expected %s\n", on, c3); failures++; }
        }
        else if (!strcmp(c1, "echo")) printf("== %s\n", line + 5);
        else printf("WARN: bad command: %s\n", line);
    }
    fclose(sf);
    printf(failures ? "RESULT: %d FAILURE(S)\n" : "RESULT: ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
