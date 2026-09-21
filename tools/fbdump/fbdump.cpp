/*
 * fbdump - headless Flashback asset/frame extractor for the SMS port.
 *
 * Links the REminiscence engine (GPL, Gregory Montoir) against a SystemStub
 * that renders nowhere: every updateScreen() is captured with its palette and
 * a virtual timestamp.  This gives pixel-exact "oracle" frames from the real
 * renderer, which the SMS converters then re-express as tiles.
 *
 *   fbdump DATA list                  list archive entries
 *   fbdump DATA extract OUT           dump every archive entry as a raw file
 *   fbdump DATA cutscenes OUT         OUT/cut_XX.fbv  (all cutscenes present)
 *   fbdump DATA rooms OUT             OUT/rooms.fbr   (level 1 room layers)
 *   fbdump DATA play OUT INPUT FRAMES OUT/play.fbv   (whole game, scripted pad)
 *
 * .fbv  = "FBV1" then records: 'F' u32 time_ms, u8 pal[768], u8 pix[256*224]
 * .fbr  = "FBR1" then records: 'R' u8 room, u8 slots[4], u8 pal[768], u8 pix[256*224]
 *         pixel bit 0x80 = foreground (drawn over characters)
 */
#include <sys/stat.h>
#include "file.h"
#include "fs.h"
#include "game.h"
#include "resource_aba.h"
#include "systemstub.h"
#include "unpack.h"
#include "util.h"

Options g_options;
const char *g_caption = "fbdump";
const Features *g_features;
static const Features kFeaturesDOS = { true, false, 1, true };

/* ---------------------------------------------------------------- stub --- */
struct SystemStub_Dump : SystemStub {
	uint8_t _pal[256 * 3];
	uint8_t _fb[256 * 224];
	uint32_t _now;
	FILE *_out;
	unsigned _frames, _maxFrames;
	FILE *_input;          /* play mode: one line per frame "dirmask buttons" */

	SystemStub_Dump() : _now(0), _out(0), _frames(0), _maxFrames(0), _input(0) {
		memset(_pal, 0, sizeof(_pal));
		memset(_fb, 0, sizeof(_fb));
	}
	virtual void init(const char *, int, int, bool, int, bool, const ScalerParameters *, int) {}
	virtual void destroy() {}
	virtual bool hasWidescreen() const { return false; }
	virtual void setScreenSize(int, int) {}
	virtual void setPalette(const uint8_t *pal, int n) { memcpy(_pal, pal, n * 3); }
	virtual void getPalette(uint8_t *pal, int n) { memcpy(pal, _pal, n * 3); }
	virtual void setPaletteEntry(int i, const Color *c) { _pal[i*3] = c->r; _pal[i*3+1] = c->g; _pal[i*3+2] = c->b; }
	virtual void getPaletteEntry(int i, Color *c) { c->r = _pal[i*3]; c->g = _pal[i*3+1]; c->b = _pal[i*3+2]; }
	virtual void setOverscanColor(int) {}
	virtual void copyRect(int x, int y, int w, int h, const uint8_t *buf, int pitch) {
		for (int j = 0; j < h; ++j) {
			if (y + j < 0 || y + j >= 224) continue;
			for (int i = 0; i < w; ++i) {
				if (x + i < 0 || x + i >= 256) continue;
				_fb[(y + j) * 256 + x + i] = buf[j * pitch + i];
			}
		}
	}
	virtual void copyRectRgb24(int, int, int, int, const uint8_t *) {}
	virtual void zoomRect(int, int, int, int) {}
	virtual void copyWidescreenLeft(int, int, const uint8_t *) {}
	virtual void copyWidescreenRight(int, int, const uint8_t *) {}
	virtual void copyWidescreenMirror(int, int, const uint8_t *) {}
	virtual void copyWidescreenBlur(int, int, const uint8_t *) {}
	virtual void copyWidescreenCDi(int, int, const uint8_t *, const uint8_t *) {}
	virtual void clearWidescreen() {}
	virtual void enableWidescreen(bool) {}
	virtual void fadeScreen() {}
	virtual void updateScreen(int) {
		if (_out) {
			fputc('F', _out);
			fwrite(&_now, 4, 1, _out);
			fwrite(_pal, 1, sizeof(_pal), _out);
			fwrite(_fb, 1, sizeof(_fb), _out);
		}
		++_frames;
		if (_maxFrames && _frames >= _maxFrames) _pi.quit = true;
	}
	virtual void processEvents() {
		if (!_input) return;
		int dir = 0, btn = 0;
		char line[64];
		if (fgets(line, sizeof(line), _input)) {
			sscanf(line, "%i %i", &dir, &btn);
		}
		_pi.dirMask = dir & 15;
		_pi.enter = (btn & 1) != 0;
		_pi.space = (btn & 2) != 0;
		_pi.shift = (btn & 4) != 0;
		_pi.backspace = (btn & 8) != 0;
	}
	virtual void sleep(int ms) { if (ms > 0) _now += ms; }
	virtual uint32_t getTimeStamp() { return _now; }
	virtual void startAudio(AudioCallback, void *) {}
	virtual void stopAudio() {}
	virtual uint32_t getOutputSampleRate() { return 22050; }
	virtual void lockAudio() {}
	virtual void unlockAudio() {}
};

static void defaultOptions() {
	memset(&g_options, 0, sizeof(g_options));
	g_options.bypass_protection = true;
	g_options.use_seq_cutscenes = false;
	g_options.restore_memo_cutscene = true;
	g_options.fade_out_palette = true;
}

static Game *makeGame(SystemStub_Dump *stub, FileSystem *fs) {
	File f;
	if (!f.open("DEMO_UK.ABA", "rb", fs)) {
		error("DEMO_UK.ABA not found - point DATA at the unzipped Flashback DOS demo");
	}
	g_features = &kFeaturesDOS;
	Game *g = new Game(stub, fs, ".", 0, kResourceTypeDOS, LANG_EN, kWidescreenNone, false, 0, 0, 0);
	g->_res.init();
	g->_res.load_TEXT();
	g->_res.load("FB_TXT", Resource::OT_FNT);
	/* global gameplay resources (Game::run loads these before any level) */
	g->_res.load("GLOBAL", Resource::OT_ICN);
	g->_res.load("GLOBAL", Resource::OT_SPC);
	g->_res.load("PERSO", Resource::OT_SPR);
	g->_res.load_SPR_OFF("PERSO", g->_res._spr1);
	g->_res.load_FIB("GLOBAL");
	return g;
}

static void mkdirp(const char *d) { mkdir(d, 0755); }

/* ------------------------------------------------------- gameplay trace --
 * .fbt : "FBT1" then per drawn game frame (30 Hz)
 *   'G' u32 frame, u8 level, u8 room, i16 conrad_x, i16 conrad_y,
 *       u8 pal[768], u8 layer[256*224],
 *       u16 npges, per object 12 bytes (x, y, anim_number, obj_type, flags,
 *       room, anim_seq, first_obj), u8 demo_input, u16 npieces,
 *       and per piece: u8 variant, u8 colMask, u8 x, u8 y, u8 w, u8 h,
 *       u8 pgeIndex, u8 pgeFlags, u16 animNumber, i16 pgeX, i16 pgeY, u8 pix[w*h]
 * A piece is one sprite blit the engine performed, reconstructed exactly as
 * the blit loop would read it (0 = transparent).  variant 1/2 = the blit that
 * ignores foreground priority, 3..6 = the ones that respect it.
 */
struct Piece {
	uint8_t variant, colMask, x, y, w, h;
	uint8_t pgeIndex, pgeFlags;
	uint16_t animNumber;
	int16_t pgeX, pgeY;
	uint8_t pix[256 * 224];
};
static Piece g_pieces[256];
static int g_pieceCount;
static FILE *g_traceOut;
static uint32_t g_traceFrame;

static void tracePiece(int variant, const uint8_t *src, int dstOffset, int pitch, int h, int w, uint8_t colMask) {
	if (g_pieceCount == 256 || w <= 0 || h <= 0) return;
	Piece *p = &g_pieces[g_pieceCount];
	p->variant = variant;
	p->colMask = colMask;
	p->x = dstOffset % 256;
	p->y = dstOffset / 256;
	p->w = w;
	p->h = h;
	p->pgeIndex = (uint8_t)(g_drawPgeIndex < 0 ? 0xFF : g_drawPgeIndex);
	p->pgeFlags = (uint8_t)g_drawPgeFlags;
	p->animNumber = (uint16_t)g_drawAnimNumber;
	p->pgeX = (int16_t)g_drawPgeX;
	p->pgeY = (int16_t)g_drawPgeY;
	for (int r = 0; r < h; ++r) {
		for (int i = 0; i < w; ++i) {
			uint8_t v;
			switch (variant) {
			case 1: case 3: v = src[r * pitch + i]; break;
			case 2: case 4: v = src[r * pitch - i]; break;
			case 5:         v = src[r + i * pitch]; break;
			default:        v = src[r - i * pitch]; break;
			}
			p->pix[r * w + i] = v;
		}
	}
	++g_pieceCount;
}

static SystemStub_Dump *g_dumpStub;
static unsigned g_opCount[3][256];

static int g_traceOps = -1;        /* trace opcodes up to this game frame */

static void traceOpcode(int slot, int opcode) {
	g_opCount[slot - 1][opcode & 0xFF]++;
	if (g_traceOps >= 0 && (int)g_traceFrame <= g_traceOps) {
		printf("OP frame %u pge %d slot %d op 0x%02X\n", g_traceFrame, g_traceObjIndex, slot, opcode);
	}
}

static void traceFrame(Game *g) {
	if (!g_traceOut) { g_pieceCount = 0; return; }
	fputc('G', g_traceOut);
	fwrite(&g_traceFrame, 4, 1, g_traceOut);
	fputc(g->_currentLevel, g_traceOut);
	fputc(g->_currentRoom, g_traceOut);
	int16_t cx = g->_pgeLive[0].pos_x, cy = g->_pgeLive[0].pos_y;
	fwrite(&cx, 2, 1, g_traceOut);
	fwrite(&cy, 2, 1, g_traceOut);
	fwrite(g_dumpStub->_pal, 1, 768, g_traceOut);
	fwrite(g->_vid._frontLayer, 1, 256 * 224, g_traceOut);
	/* per-object state, the oracle for the game-logic port */
	uint16_t np = g->_res._pgeNum;
	fwrite(&np, 2, 1, g_traceOut);
	for (int i = 0; i < np; ++i) {
		const LivePGE *l = &g->_pgeLive[i];
		uint8_t rec[12];
		rec[0] = l->pos_x & 0xFF; rec[1] = (l->pos_x >> 8) & 0xFF;
		rec[2] = l->pos_y & 0xFF; rec[3] = (l->pos_y >> 8) & 0xFF;
		rec[4] = l->anim_number & 0xFF; rec[5] = (l->anim_number >> 8) & 0xFF;
		rec[6] = l->obj_type & 0xFF; rec[7] = (l->obj_type >> 8) & 0xFF;
		rec[8] = l->flags; rec[9] = l->room_location; rec[10] = l->anim_seq;
		rec[11] = l->first_obj_number & 0xFF;
		fwrite(rec, 1, sizeof(rec), g_traceOut);
	}
	uint8_t inp = (uint8_t)(g->_res._dem && g->_inp_demPos > 0 && g->_inp_demPos <= g->_res._demLen
	                        ? g->_res._dem[g->_inp_demPos - 1] : 0);
	fputc(inp, g_traceOut);
	uint16_t n = g_pieceCount;
	fwrite(&n, 2, 1, g_traceOut);
	for (int i = 0; i < g_pieceCount; ++i) {
		Piece *p = &g_pieces[i];
		uint8_t hdr[6] = { p->variant, p->colMask, p->x, p->y, p->w, p->h };
		fwrite(hdr, 1, 6, g_traceOut);
		fputc(p->pgeIndex, g_traceOut);
		fputc(p->pgeFlags, g_traceOut);
		fwrite(&p->animNumber, 2, 1, g_traceOut);
		fwrite(&p->pgeX, 2, 1, g_traceOut);
		fwrite(&p->pgeY, 2, 1, g_traceOut);
		fwrite(p->pix, 1, (size_t)p->w * p->h, g_traceOut);
	}
	g_pieceCount = 0;
	++g_traceFrame;
}

int main(int argc, char *argv[]) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s DATA list|extract|cutscenes|rooms|play [OUT] [INPUT FRAMES]\n", argv[0]);
		return 1;
	}
	defaultOptions();
	g_debugMask = 0;
	FileSystem fs(argv[1]);
	const char *mode = argv[2];
	const char *out = argc > 3 ? argv[3] : ".";
	SystemStub_Dump *stub = new SystemStub_Dump;

	if (!strcmp(mode, "list") || !strcmp(mode, "extract")) {
		ResourceAba aba(&fs);
		aba.readEntries("DEMO_UK.ABA");
		if (!strcmp(mode, "extract")) mkdirp(out);
		for (int i = 0; i < aba._entriesCount; ++i) {
			const ResourceAbaEntry *e = &aba._entries[i];
			printf("%-14s %8u %8u\n", e->name, e->size, e->compressedSize);
			if (!strcmp(mode, "extract")) {
				uint32_t sz = 0;
				uint8_t *p = aba.loadEntry(e->name, &sz);
				if (p) {
					char path[512];
					snprintf(path, sizeof(path), "%s/%s", out, e->name);
					FILE *fp = fopen(path, "wb");
					fwrite(p, 1, sz, fp);
					fclose(fp);
					free(p);
				}
			}
		}
		return 0;
	}

	Game *g = makeGame(stub, &fs);

	if (!strcmp(mode, "cutscenes")) {
		mkdirp(out);
		stub->_pi.dbgMask = 0;
		for (int id = 0; id < 0x4B; ++id) {
			uint8_t cutName = Cutscene::_offsetsTableDOS[id * 2];
			if (cutName == 0xFF) continue;
			const char *name = Cutscene::_namesTableDOS[cutName];
			char entry[32];
			snprintf(entry, sizeof(entry), "%s.CMD", name);
			if (!g->_res._aba || !g->_res._aba->findEntry(entry)) {
				continue;
			}
			char path[512];
			snprintf(path, sizeof(path), "%s/cut_%02X.fbv", out, id);
			struct stat st;
			if (stat(path, &st) == 0) continue;          /* resumable */
			stub->_maxFrames = 2500;                     /* some scenes loop waiting for a key */
			stub->_out = fopen(path, "wb");
			fwrite("FBV1", 1, 4, stub->_out);
			stub->_frames = 0;
			stub->_pi.quit = false;
			g->_cut._id = id;
			g->_cut.play();
			fclose(stub->_out);
			stub->_out = 0;
			printf("cutscene 0x%02X %-10s %5u frames %6u ms\n", id, name, stub->_frames, stub->_now);
			stub->_now = 0;
		}
	} else if (!strcmp(mode, "rooms")) {
		/* demo levels: 0 = level1, 2 = level3, 5 = level5_1 */
		mkdirp(out);
		const char *levels = argc > 4 ? argv[4] : "0,2,5";
		for (const char *lp = levels; *lp; ) {
			int lvl = atoi(lp);
			while (*lp && *lp != ',') ++lp;
			if (*lp == ',') ++lp;
			g->_currentLevel = lvl;
			g->loadLevelData();
			char path[512];
			snprintf(path, sizeof(path), "%s/rooms_L%d.fbr", out, lvl);
			FILE *fp = fopen(path, "wb");
			fwrite("FBR1", 1, 4, fp);
			int count = 0;
			for (int room = 0; room < 0x40; ++room) {
				if (!g->_res._lev || READ_BE_UINT32(g->_res._lev + room * 4) == 0) continue;
				if (!bytekiller_unpack(g->_res._scratchBuffer, Resource::kScratchBufferSize, g->_res._lev,
				                       READ_BE_UINT32(g->_res._lev + room * 4))) continue;   /* not in demo */
				memset(stub->_pal, 0, sizeof(stub->_pal));
				memset(g->_vid._frontLayer, 0, g->_vid._layerSize);
				g->_vid.DOS_decodeLev(lvl, room);
				fputc('R', fp);
				fputc(room, fp);
				fputc(g->_vid._mapPalSlot1, fp);
				fputc(g->_vid._mapPalSlot2, fp);
				fputc(g->_vid._mapPalSlot3, fp);
				fputc(g->_vid._mapPalSlot4, fp);
				fwrite(stub->_pal, 1, sizeof(stub->_pal), fp);
				fwrite(g->_vid._frontLayer, 1, 256 * 224, fp);
				++count;
			}
			fclose(fp);
			snprintf(path, sizeof(path), "%s/ct_L%d.bin", out, lvl);
			fp = fopen(path, "wb");
			fwrite(g->_res._ctData, 1, sizeof(g->_res._ctData), fp);
			fclose(fp);
			printf("level %d: %d rooms\n", lvl, count);
		}
	} else if (!strcmp(mode, "level")) {
		/* object tables of a level, plus the engine's own initial live state
		 * so the Z80 port can be checked against it.
		 * .fbl : "FBL1" u16 numPges, u16 numObjectNodes, u16 numObjects,
		 *        InitPGE[numPges] (31 bytes each, little endian),
		 *        u16 node_first_object[256], u16 node_num_objects[256],
		 *        Object[numObjects] (18 bytes each),
		 *        then u16 live_first_obj[numPges], u8 live_flags[numPges],
		 *        i16 live_life[numPges], u8 live_room[numPges],
		 *        u16 live_anim[numPges], ctData[0x1D00], u32 aniSize, ANI blob */
		if (argc < 5) error("level needs OUT LEVEL");
		mkdirp(out);
		const int lvl = atoi(argv[4]);
		g->_demoBin = -1;
		g->_skillLevel = 1;
		g->_currentLevel = lvl;
		g->loadLevelData();
		g->resetGameState();
		char path[512];
		snprintf(path, sizeof(path), "%s/level_L%d.fbl", out, lvl);
		FILE *fp = fopen(path, "wb");
		fwrite("FBL1", 1, 4, fp);
		const uint16_t numPges = g->_res._pgeNum;
		uint16_t nodes = g->_res._numObjectNodes;
		/* the node map has many entries pointing at the SAME node: store each
		 * node once and let the map index it */
		ObjectNode *uniq[256];
		int uniqCount = 0;
		int nodeIndex[256];
		uint16_t numObjects = 0;
		for (int i = 0; i < nodes; ++i) {
			ObjectNode *on = g->_res._objectNodesMap[i];
			int found = -1;
			for (int k = 0; k < uniqCount; ++k) {
				if (uniq[k] == on) { found = k; break; }
			}
			if (found < 0) {
				found = uniqCount;
				uniq[uniqCount++] = on;
				numObjects += on->num_objects;
			}
			nodeIndex[i] = found;
		}
		fwrite(&numPges, 2, 1, fp);
		fwrite(&nodes, 2, 1, fp);
		fwrite(&numObjects, 2, 1, fp);
		for (int i = 0; i < numPges; ++i) {
			const InitPGE *p = &g->_res._pgeInit[i];
			uint8_t rec[31];
			int o = 0;
			#define W16(v) do { int16_t t_ = (int16_t)(v); rec[o++] = t_ & 0xFF; rec[o++] = (t_ >> 8) & 0xFF; } while (0)
			W16(p->type); W16(p->pos_x); W16(p->pos_y); W16(p->obj_node_number); W16(p->life);
			for (int k = 0; k < 4; ++k) W16(p->data[k]);
			rec[o++] = p->object_type; rec[o++] = p->init_room; rec[o++] = p->room_location;
			rec[o++] = p->init_flags; rec[o++] = p->colliding_icon_num; rec[o++] = p->icon_num;
			rec[o++] = p->object_id; rec[o++] = p->skill; rec[o++] = p->mirror_x;
			rec[o++] = p->flags; rec[o++] = p->collision_data_len;
			W16(p->text_num);
			fwrite(rec, 1, sizeof(rec), fp);
		}
		uint16_t firstObj[256], numObj[256];
		memset(firstObj, 0, sizeof(firstObj));
		memset(numObj, 0, sizeof(numObj));
		uint16_t uniqFirst[256], running = 0;
		for (int k = 0; k < uniqCount; ++k) {
			uniqFirst[k] = running;
			running += uniq[k]->num_objects;
		}
		for (int i = 0; i < nodes; ++i) {
			firstObj[i] = uniqFirst[nodeIndex[i]];
			numObj[i] = uniq[nodeIndex[i]]->num_objects;
		}
		fwrite(firstObj, 2, 256, fp);
		fwrite(numObj, 2, 256, fp);
		for (int u = 0; u < uniqCount; ++u) {
			ObjectNode *on = uniq[u];
			for (int k = 0; k < on->num_objects; ++k) {
				const Object *ob = &on->objects[k];
				uint8_t rec[18];
				int o = 0;
				W16(ob->type); rec[o++] = (uint8_t)ob->dx; rec[o++] = (uint8_t)ob->dy;
				W16(ob->init_obj_type);
				rec[o++] = ob->opcode2; rec[o++] = ob->opcode1; rec[o++] = ob->flags; rec[o++] = ob->opcode3;
				W16(ob->init_obj_number); W16(ob->opcode_arg1); W16(ob->opcode_arg2); W16(ob->opcode_arg3);
				fwrite(rec, 1, sizeof(rec), fp);
			}
		}
		/* the engine's initial live state, for the port to be checked against */
		for (int i = 0; i < numPges; ++i) { uint16_t v = g->_pgeLive[i].first_obj_number; fwrite(&v, 2, 1, fp); }
		for (int i = 0; i < numPges; ++i) fputc(g->_pgeLive[i].flags, fp);
		for (int i = 0; i < numPges; ++i) { int16_t v = g->_pgeLive[i].life; fwrite(&v, 2, 1, fp); }
		for (int i = 0; i < numPges; ++i) fputc(g->_pgeLive[i].room_location, fp);
		for (int i = 0; i < numPges; ++i) { uint16_t v = g->_pgeLive[i].anim_number; fwrite(&v, 2, 1, fp); }
		/* the collision table: room links plus a 16x7 grid per room */
		fwrite(g->_res._ctData, 1, sizeof(g->_res._ctData), fp);
		/* the level's animation table, straight from the archive */
		{
			char aniName[32];
			snprintf(aniName, sizeof(aniName), "%s.ANI", Game::_gameLevels[lvl].name2);
			uint32_t aniSize = 0;
			uint8_t *ani = g->_res._aba ? g->_res._aba->loadEntry(aniName, &aniSize) : 0;
			if (!ani) error("cannot read %s", aniName);
			fwrite(&aniSize, 4, 1, fp);
			fwrite(ani, 1, aniSize, fp);
			free(ani);
			printf("  %s: %u bytes\n", aniName, aniSize);
		}
		#undef W16
		fclose(fp);
		printf("level %d: %d pges, %d node slots (%d unique), %d objects\n", lvl, numPges, nodes, uniqCount, numObjects);
	} else if (!strcmp(mode, "title")) {
		/* One frame of the game's own title screen, captured as a room-shaped
		 * image so the room converter can turn it into SMS tiles. */
		mkdirp(out);
		g->_res.load_FIB("GLOBAL");
		g->_vid.setTextPalette();
		stub->_maxFrames = 2;          /* draw a couple of frames, then quit */
		stub->_pi.quit = false;
		g->_menu._res = &g->_res;
		g->_menu.handleTitleScreen();
		char path[512];
		snprintf(path, sizeof(path), "%s/title.fbr", out);
		FILE *fp = fopen(path, "wb");
		fwrite("FBR1", 1, 4, fp);
		fputc('R', fp);
		fputc(0, fp);                  /* room number 0 */
		fputc(0, fp); fputc(0, fp); fputc(0, fp); fputc(0, fp);
		fwrite(stub->_pal, 1, 768, fp);
		fwrite(stub->_fb, 1, 256 * 224, fp);
		fclose(fp);
		printf("title screen captured (%u frames drawn)\n", stub->_frames);
	} else if (!strcmp(mode, "anims")) {
		/* Render EVERY animation frame the engine can draw, not just the ones a
		 * recorded demo happened to show, so a played game always has a sprite
		 * for whatever the logic asks for.  Written in the same format as a
		 * gameplay trace: one "frame" per (animation, facing). */
		mkdirp(out);
		g->_demoBin = -1;
		g->_skillLevel = 1;
		g->_currentLevel = 0;
		g->_vid.setTextPalette();
		g->_vid.setPalette0xF();
		g->loadLevelData();
		g->resetGameState();
		g->_currentRoom = g->_res._pgeInit[0].init_room;
		g->_vid.DOS_decodeLev(0, g->_currentRoom);      /* sets the level palettes */
		char path[512];
		snprintf(path, sizeof(path), "%s/anims.fbt", out);
		g_traceOut = fopen(path, "wb");
		fwrite("FBT1", 1, 4, g_traceOut);
		g_dumpStub = stub;
		g_spriteHook = tracePiece;
		g_frameHook = 0;
		g_traceFrame = 0;
		LivePGE *pge = &g->_pgeLive[0];
		int written = 0;
		for (int mirror = 0; mirror < 2; ++mirror) {
			for (int kind = 0; kind < 2; ++kind) {
				const int count = kind ? 1024 : Resource::NUM_SPRITES;
				for (int n = 0; n < count; ++n) {
					if (kind == 0 && g->_res._sprData[n] == 0) continue;
					g->_animBuffers._curPos[0] = 0xFF;
					g->_animBuffers._curPos[1] = 0xFF;
					g->_animBuffers._curPos[2] = 0xFF;
					g->_animBuffers._curPos[3] = 0xFF;
					g_pieceCount = 0;
					pge->anim_number = n;
					/* bit 1 (0x02) is what actually mirrors a sprite */
					pge->flags = (uint8_t)((mirror ? 2 : 0) | (kind ? 8 : 0));
					pge->pos_x = 128;
					pge->pos_y = 100;
					pge->room_location = g->_currentRoom;
					g->prepareAnimsHelper(pge, 0, 0);
					g->drawAnims();
					if (g_pieceCount > 0) { traceFrame(g); ++written; }
					else g_pieceCount = 0;
				}
			}
		}
		g_spriteHook = 0;
		fclose(g_traceOut);
		g_traceOut = 0;
		printf("animation frames rendered: %d\n", written);
	} else if (!strcmp(mode, "replay")) {
		/* run one of the game's own recorded demos and trace every frame */
		if (argc < 5) error("replay needs OUT DEMOINDEX [MAXFRAMES]");
		mkdirp(out);
		const int di = atoi(argv[4]);
		const int maxFrames = argc > 5 ? atoi(argv[5]) : 0;
		g_traceOps = (argc > 6) ? atoi(argv[6]) : -1;
		const Demo *d = &Game::_demoInputs[di];
		g->_res.load_DEM(d->name);
		if (g->_res._demLen == 0) error("no demo inputs in '%s'", d->name);
		g->_demoBin = di;
		g->_skillLevel = 1;
		g->_currentLevel = d->level;
		g->_randSeed = 0;
		g->_vid.setTextPalette();
		g->_vid.setPalette0xF();
		g->_score = 0;
		g->clearStateRewind();
		g->loadLevelData();
		g->resetGameState();
		g->_endLoop = false;
		char path[512];
		snprintf(path, sizeof(path), "%s/trace_D%d.fbt", out, di);
		g_traceOut = fopen(path, "wb");
		fwrite("FBT1", 1, 4, g_traceOut);
		g_dumpStub = stub;
		memset(g_opCount, 0, sizeof(g_opCount));
		g_opcodeHook = traceOpcode;
		g_spriteHook = tracePiece;
		g_frameHook = traceFrame;
		g_traceFrame = 0;
		while (!stub->_pi.quit && !g->_endLoop) {
			g->mainLoop();
			if (g->_inp_demPos >= g->_res._demLen) break;
			if (maxFrames && (int)g_traceFrame >= maxFrames) break;
		}
		g_spriteHook = 0;
		g_frameHook = 0;
		g_opcodeHook = 0;
		{
			int distinct = 0;
			unsigned total = 0;
			printf("opcodes executed (opcode:count):\n");
			for (int op = 0; op < 256; ++op) {
				unsigned c = g_opCount[0][op] + g_opCount[1][op] + g_opCount[2][op];
				if (c) { ++distinct; total += c; printf("  0x%02X %u\n", op, c); }
			}
			printf("%d distinct opcodes, %u executions\n", distinct, total);
		}
		fclose(g_traceOut);
		g_traceOut = 0;
		{   /* the demo's start point for Conrad, applied by the engine on load:
		     * the logic harness needs it to start from the same state */
			char sp[512];
			snprintf(sp, sizeof(sp), "%s/demo_D%d.bin", out, di);
			FILE *sf = fopen(sp, "wb");
			fputc(d->level, sf); fputc(d->room, sf); fputc(d->x, sf); fputc(d->y, sf);
			fclose(sf);
			printf("  demo start: room %d x %d y %d\n", d->room, d->x, d->y);
		}
		printf("demo %d (%s, level %d): %u game frames traced\n", di, d->name, d->level, g_traceFrame);
	} else if (!strcmp(mode, "play")) {
		if (argc < 6) error("play needs OUT INPUT FRAMES");
		mkdirp(out);
		char path[512];
		snprintf(path, sizeof(path), "%s/play.fbv", out);
		stub->_out = fopen(path, "wb");
		fwrite("FBV1", 1, 4, stub->_out);
		stub->_input = fopen(argv[4], "r");
		stub->_maxFrames = atoi(argv[5]);
		g->run();
		fclose(stub->_out);
		printf("%u frames\n", stub->_frames);
	} else {
		error("unknown mode '%s'", mode);
	}
	return 0;
}
