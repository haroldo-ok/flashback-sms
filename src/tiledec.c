/*
 * Expands bit-plane tiles (tools/tilepack.py) into 32 planar bytes.
 *
 * A record is a 2-byte header of four plane codes (0 = zeros, 1 = ones,
 * 2+j = stored plane j, 6+j = its complement) followed by the stored planes,
 * 8 bytes each.  Expansion is plane-major: one decision per plane, then a
 * tight loop of eight writes - an earlier colour-mask format took 2,200 and
 * 6,700 cycles a tile to rebuild and made cutscenes stall at shot changes.
 *
 * Region sizes (512 / 6x256 / 7x128 / 512 records per bank) let the bank and
 * offset come from small tables and shifts: no division, no multiply.
 */
#include "SMSlib.h"
#include "tiledec.h"

static unsigned char buf[32];

#define DIVTAB(q) \
    q(0),q(1),q(2),q(3),q(4),q(5),q(6),q(7),q(8),q(9),q(10),q(11),q(12),q(13),q(14),q(15), \
    q(16),q(17),q(18),q(19),q(20),q(21),q(22),q(23),q(24),q(25),q(26),q(27),q(28),q(29),q(30),q(31), \
    q(32),q(33),q(34),q(35),q(36),q(37),q(38),q(39),q(40),q(41),q(42),q(43),q(44),q(45),q(46),q(47), \
    q(48),q(49),q(50),q(51),q(52),q(53),q(54),q(55),q(56),q(57),q(58),q(59),q(60),q(61),q(62),q(63), \
    q(64),q(65),q(66),q(67),q(68),q(69),q(70),q(71),q(72),q(73),q(74),q(75),q(76),q(77),q(78),q(79), \
    q(80),q(81),q(82),q(83),q(84),q(85),q(86),q(87),q(88),q(89),q(90),q(91),q(92),q(93),q(94),q(95), \
    q(96),q(97),q(98),q(99),q(100),q(101),q(102),q(103),q(104),q(105),q(106),q(107),q(108),q(109),q(110),q(111), \
    q(112),q(113),q(114),q(115),q(116),q(117),q(118),q(119),q(120),q(121),q(122),q(123),q(124),q(125),q(126),q(127), \
    q(128),q(129),q(130),q(131),q(132),q(133),q(134),q(135),q(136),q(137),q(138),q(139),q(140),q(141),q(142),q(143), \
    q(144),q(145),q(146),q(147),q(148),q(149),q(150),q(151),q(152),q(153),q(154),q(155),q(156),q(157),q(158),q(159), \
    q(160),q(161),q(162),q(163),q(164),q(165),q(166),q(167),q(168),q(169),q(170),q(171),q(172),q(173),q(174),q(175), \
    q(176),q(177),q(178),q(179),q(180),q(181),q(182),q(183),q(184),q(185),q(186),q(187),q(188),q(189),q(190),q(191), \
    q(192),q(193),q(194),q(195),q(196),q(197),q(198),q(199),q(200),q(201),q(202),q(203),q(204),q(205),q(206),q(207), \
    q(208),q(209),q(210),q(211),q(212),q(213),q(214),q(215),q(216),q(217),q(218),q(219),q(220),q(221),q(222),q(223), \
    q(224),q(225),q(226),q(227),q(228),q(229),q(230),q(231),q(232),q(233),q(234),q(235),q(236),q(237),q(238),q(239), \
    q(240),q(241),q(242),q(243),q(244),q(245),q(246),q(247),q(248),q(249),q(250),q(251),q(252),q(253),q(254),q(255)
#define D6(n) ((n) / 6)
#define M6(n) ((n) % 6)
#define D7(n) ((n) / 7)
#define M7(n) ((n) % 7)
static const unsigned char div6[256] = { DIVTAB(D6) };
static const unsigned char mod6[256] = { DIVTAB(M6) };
static const unsigned char div7[256] = { DIVTAB(D7) };
static const unsigned char mod7[256] = { DIVTAB(M7) };

/* Plane-major expansion in assembly (the C version cost 4,300 cycles a tile).
 * IX walks buf + plane; each plane is one decision then eight stores at
 * offsets 0,4,...,28 - the planar layout interleaves the four planes. */
static const unsigned char *t_src;
static const unsigned char *t_st;
static unsigned char t_hdr0, t_hdr1;

static void expand_planes_asm(void) __naked
{
    __asm
    push ix
    ld   hl, (_t_src)
    ld   a, (hl)
    ld   (_t_hdr0), a
    inc  hl
    ld   a, (hl)
    ld   (_t_hdr1), a
    inc  hl
    ld   (_t_st), hl
    ld   ix, #_buf
    ld   a, (_t_hdr0)
    rrca
    rrca
    rrca
    rrca
    and  #0x0F
    call 00010$
    inc  ix
    ld   a, (_t_hdr0)
    and  #0x0F
    call 00010$
    inc  ix
    ld   a, (_t_hdr1)
    rrca
    rrca
    rrca
    rrca
    and  #0x0F
    call 00010$
    inc  ix
    ld   a, (_t_hdr1)
    and  #0x0F
    call 00010$
    pop  ix
    ret

; one plane: A = code, IX = first byte of this plane in buf
00010$:
    or   a
    jr   z, 00020$            ; all zeros
    cp   #1
    jr   z, 00021$            ; all ones
    cp   #6
    jr   nc, 00030$           ; complement of a stored plane
    sub  #2                   ; stored plane j
    add  a, a
    add  a, a
    add  a, a
    ld   e, a
    ld   d, #0
    ld   hl, (_t_st)
    add  hl, de
    ld   a, (hl)
    ld   0 (ix), a
    inc  hl
    ld   a, (hl)
    ld   4 (ix), a
    inc  hl
    ld   a, (hl)
    ld   8 (ix), a
    inc  hl
    ld   a, (hl)
    ld   12 (ix), a
    inc  hl
    ld   a, (hl)
    ld   16 (ix), a
    inc  hl
    ld   a, (hl)
    ld   20 (ix), a
    inc  hl
    ld   a, (hl)
    ld   24 (ix), a
    inc  hl
    ld   a, (hl)
    ld   28 (ix), a
    ret
00030$:
    sub  #6
    add  a, a
    add  a, a
    add  a, a
    ld   e, a
    ld   d, #0
    ld   hl, (_t_st)
    add  hl, de
    ld   a, (hl)
    cpl
    ld   0 (ix), a
    inc  hl
    ld   a, (hl)
    cpl
    ld   4 (ix), a
    inc  hl
    ld   a, (hl)
    cpl
    ld   8 (ix), a
    inc  hl
    ld   a, (hl)
    cpl
    ld   12 (ix), a
    inc  hl
    ld   a, (hl)
    cpl
    ld   16 (ix), a
    inc  hl
    ld   a, (hl)
    cpl
    ld   20 (ix), a
    inc  hl
    ld   a, (hl)
    cpl
    ld   24 (ix), a
    inc  hl
    ld   a, (hl)
    cpl
    ld   28 (ix), a
    ret
00020$:
    xor  a
    jr   00022$
00021$:
    ld   a, #0xFF
00022$:
    ld   0 (ix), a
    ld   4 (ix), a
    ld   8 (ix), a
    ld   12 (ix), a
    ld   16 (ix), a
    ld   20 (ix), a
    ld   24 (ix), a
    ld   28 (ix), a
    ret
    __endasm;
}

static void expand_planes(const unsigned char *r)
{
    t_src = r;
    expand_planes_asm();
}

void tile_upload(const TileDict *d, unsigned int id, unsigned int vram_tile)
{
    unsigned int k, t;
    unsigned char hi;
    if (id < d->start1) {                        /* raw: straight from ROM */
        SMS_mapROMBank(d->bank0 + (id >> 9));
        SMS_loadTiles((const unsigned char *)(0x8000 + ((id & 511) << 5)), vram_tile, 32);
        return;
    }
    if (id < d->start2) {                        /* one stored plane: 10 bytes, 6x256/bank */
        k = id - d->start1;
        hi = (unsigned char)(k >> 8);
        SMS_mapROMBank(d->bank1 + div6[hi]);
        t = ((unsigned int)mod6[hi] << 8) | (k & 255);
        expand_planes((const unsigned char *)(0x8000 + (t << 3) + (t << 1)));
    } else if (id < d->start3) {                 /* two planes: 18 bytes, 7x128/bank */
        k = id - d->start2;
        hi = (unsigned char)(k >> 7);
        SMS_mapROMBank(d->bank2 + div7[hi]);
        t = ((unsigned int)mod7[hi] << 7) | (k & 127);
        expand_planes((const unsigned char *)(0x8000 + (t << 4) + (t << 1)));
    } else {                                     /* three planes: 26 bytes, 512/bank */
        k = id - d->start3;
        SMS_mapROMBank(d->bank3 + (unsigned char)(k >> 9));
        t = k & 511;
        expand_planes((const unsigned char *)(0x8000 + (t << 4) + (t << 3) + (t << 1)));
    }
    SMS_loadTiles(buf, vram_tile, 32);
}
