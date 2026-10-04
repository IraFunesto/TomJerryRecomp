#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"

/* Tom & Jerry in House-Trap: one opt-in widescreen feature with an "aspect"
 * choice. Fit starts from 16:9 and follows the window shape with no upper
 * limit. Fixed views use the same native-wide renderer. */
#define PKG "tomjerry.widescreen"
#define FEATURE "widescreen"

/* In-game HUD (overlay at 0x8003D000). Every HUD sprite -- the character
 * portrait and the triangle/square/circle prompts of both split-screen views --
 * goes through one helper, DrawHudSprite(x, y, sprite, colour). It writes an
 * optional 8-byte DR_TPAGE and then a 20-byte SPRT at the primitive cursor and
 * links them into the OT. Scenery sprites (the skirting-board row) use another
 * path, so tagging here moves only the HUD. */
#define HUD_SPRITE_FN   0x8004883Cu
#define HUD_SPRITE_W0   0x27BDFFC8u   /* addiu sp,sp,-56 */
#define HUD_SPRITE_W1   0xAFB20018u   /* sw s2,24(sp)    */
#define PRIM_CURSOR     0x8006B808u   /* next free primitive (24-bit address) */
#define SPRITE_TABLE    0x8006B0F0u   /* -> 16-byte HUD sprite descriptors */
#define TPAGE_CURRENT   0x8006B10Au   /* last texture page DrawHudSprite linked */
#define OT_POINTER      0x8006B6E0u   /* HUD ordering table; slot at +4 */
/* Health bars are three double-buffered TILE packets (16 bytes) per player,
 * held in the player object at +368+52, +84, +116. */
#define PLAYER_TABLE    0x8006CA20u
#define PLAYER_COUNT    2
#define BAR_PACKETS     0x1A4u        /* 368 + 52 */
#define BAR_PACKET_N    6

/* libgpu's video mode (SetVideoMode/GetVideoMode at 0x8001F33C/0x8001F350):
 * 1 = PAL, 0 = NTSC. Only libgpu reads it -- PutDispEnv turns it into the
 * GP1(08h) PAL bit every frame -- so holding it at NTSC runs the console on
 * NTSC video timing (60 Hz VBlank, 263 lines) while the CPU, SPU and timers
 * keep their real speed, like an NTSC console or DuckStation's
 * "Force NTSC Timings". */
#define LIBGPU_VIDEO_MODE 0x8002C110u
static int s_force_ntsc_timing = 0;

/* The runtime only lets the driver vsync when the monitor refresh equals the
 * guest rate (within 2%); on a 120/144/165 Hz panel it presents unsynced and
 * fast motion tears. Like DuckStation, keep the swap synced anyway: the
 * runtime's wall-clock pacer still holds the game at 60 fps and every frame
 * waits for the next panel refresh. OpenGL only (gpu_gl_renderer.c; a no-op
 * without a GL context). Not part of the mod API, but this trusted plugin is
 * linked into the same executable and VBlank callbacks run on the thread
 * that owns the GL context, right before it presents. */
extern void gl_renderer_set_swap_interval(int interval);
#define VSYNC_REASSERT_VBLANKS 60u
static int s_force_vsync = 0;

/* In-match flag for the runtime's [widescreen] gameplay_state gate (game.toml).
 * The gameplay overlay is already resident while the LOADING screen still
 * shows, so "overlay loaded" is not "in a match": a match draws its HUD every
 * frame (and the pause menu sets PAUSE_MENU_FLAG). Publish that in a spare,
 * never-written word -- the unused tail of the top view's DR_ENV slot -- only
 * while the gameplay overlay owns that memory. */
#define PAUSE_MENU_FLAG  0x8006A69Cu   /* 1 while the pause menu loop runs */
#define GATE_WORD        0x8006B730u
#define GATE_IN_MATCH    0x534A5754u   /* "TWJS" */
#define GATE_HUD_VBLANKS 8u
static uint32_t s_last_hud_vblank;

/* HUD size option, in percent. 100 keeps the stock SPRT path untouched. */
static int s_hud_percent = 100;

static int32_t display_width(void) {
    uint32_t w = psx_mod_display_width();
    return w ? (int32_t)w : 512;
}

static int hud_edge_for_x(int32_t x) {
    return x < display_width() / 2 ? -1 : 1;
}

static int ram_address(uint32_t a) {
    a &= 0x1FFFFFFFu;
    return a >= 0x10000u && a < 0x200000u;
}

static int hud_overlay_resident(void) {
    return psx_mod_read_word(HUD_SPRITE_FN) == HUD_SPRITE_W0 &&
           psx_mod_read_word(HUD_SPRITE_FN + 4u) == HUD_SPRITE_W1;
}

/* Scale about the HUD element's own corner: the viewport top and the screen
 * edge it belongs to, so a widget's pieces shrink together and stay put in
 * their corner. */
/* Round to nearest (halves away from zero): truncation would pull the integer
 * bar tiles half a pixel off the frame art drawn inside the scaled quads. */
static int32_t scale_round(int32_t v) {
    int32_t q = v * s_hud_percent;
    return q >= 0 ? (q + 50) / 100 : -((-q + 50) / 100);
}

static int32_t scale_x(int32_t x, int edge) {
    int32_t pivot = edge < 0 ? 0 : display_width();
    return pivot + scale_round(x - pivot);
}

/* Edges, not lengths, are scaled: two pieces that share an edge (the bar's
 * red and green segments, a frame and its fill) get the same scaled edge and
 * stay flush instead of drifting a pixel apart through separate rounding. */
static int32_t scale_y(int32_t y) {
    return scale_round(y);
}

/* ---- health bars ------------------------------------------------------ */

/* The game rewrites some bar fields every frame (x of the inner segments,
 * every width) and sets others once (y, height). Remember, per field, the
 * value the game last wrote and the value written back here, so a field is
 * rescaled only when the game changed it and never compounds. */
static struct {
    uint32_t packet;
    int16_t orig[4];
    int16_t ours[4];
} s_bar_shadow[PLAYER_COUNT * BAR_PACKET_N];

static void scale_health_bars(void) {
    for (uint32_t i = 0; i < PLAYER_COUNT; ++i) {
        uint32_t player = psx_mod_read_word(PLAYER_TABLE + 4u * i);
        if (!ram_address(player)) continue;
        for (uint32_t k = 0; k < BAR_PACKET_N; ++k) {
            uint32_t packet = player + BAR_PACKETS + 16u * k;
            uint32_t slot = i * BAR_PACKET_N + k;
            if (s_bar_shadow[slot].packet != packet) {
                memset(&s_bar_shadow[slot], 0, sizeof s_bar_shadow[slot]);
                s_bar_shadow[slot].packet = packet;
                for (int f = 0; f < 4; ++f)
                    s_bar_shadow[slot].ours[f] =
                        (int16_t)(psx_mod_read_half(packet + 8u + 2u * f) ^ 0x8000);
            }
            int16_t cur[4];
            for (int f = 0; f < 4; ++f) {
                cur[f] = (int16_t)psx_mod_read_half(packet + 8u + 2u * f);
                if (cur[f] != s_bar_shadow[slot].ours[f])
                    s_bar_shadow[slot].orig[f] = cur[f];
            }
            const int16_t* o = s_bar_shadow[slot].orig;
            int edge = hud_edge_for_x(o[0]);
            int32_t x0 = scale_x(o[0], edge), x1 = scale_x(o[0] + o[2], edge);
            int32_t y0 = scale_y(o[1]), y1 = scale_y(o[1] + o[3]);
            int16_t out[4] = {
                (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0),
            };
            for (int f = 0; f < 4; ++f) {
                s_bar_shadow[slot].ours[f] = out[f];
                if (out[f] != cur[f])
                    psx_mod_write_half(packet + 8u + 2u * f, (uint16_t)out[f]);
            }
        }
    }
}

static void tag_health_bars(void) {
    for (uint32_t i = 0; i < PLAYER_COUNT; ++i) {
        uint32_t player = psx_mod_read_word(PLAYER_TABLE + 4u * i);
        if (!ram_address(player)) continue;
        for (uint32_t k = 0; k < BAR_PACKET_N; ++k) {
            uint32_t packet = player + BAR_PACKETS + 16u * k;
            int32_t x = (int16_t)psx_mod_read_half(packet + 8u);
            psx_mod_tag_hud_primitive(packet, hud_edge_for_x(x));
        }
    }
}

/* ---- HUD tags ---------------------------------------------------------- */

/* The game builds a frame every second VBlank into one of two primitive
 * buffers and draws it two or three VBlanks later, but a HUD tag lapses two
 * frames after it is written. Remember each sprite tag and renew it from the
 * VBlank callback for HUD_TAG_VBLANKS. Each buffer is rebuilt four VBlanks
 * after its previous build, so a renewed tag lapses before a later frame can
 * reuse the address for scenery. */
#define HUD_TAG_SLOTS   64u
#define HUD_TAG_VBLANKS 3u

static struct {
    uint32_t packet;
    uint32_t born;
    int edge;
} s_hud_tags[HUD_TAG_SLOTS];
static uint32_t s_hud_tag_next;
static uint32_t s_vblank;

static void remember_hud_tag(uint32_t packet, int edge) {
    uint32_t i = s_hud_tag_next++ % HUD_TAG_SLOTS;
    s_hud_tags[i].packet = packet;
    s_hud_tags[i].born = s_vblank;
    s_hud_tags[i].edge = edge;
    psx_mod_tag_hud_primitive(packet, edge);
}

/* ---- DrawHudSprite ----------------------------------------------------- */

/* libgpu AddPrim: link packet p in front of the OT slot. */
static void add_prim(uint32_t ot, uint32_t p) {
    uint32_t pt = psx_mod_read_word(p), ott = psx_mod_read_word(ot);
    psx_mod_write_word(p, (pt & 0xFF000000u) | (ott & 0x00FFFFFFu));
    psx_mod_write_word(ot, (ott & 0xFF000000u) | (p & 0x00FFFFFFu));
}

/* Same work as DrawHudSprite, but the 20-byte SPRT becomes a 40-byte
 * POLY_FT4 so the sprite can be drawn smaller (a SPRT is always 1:1). The
 * texture-page bookkeeping is reproduced exactly: the DR_TPAGE, its value
 * and its OT position are what the game's later sprites rely on. */
static void draw_hud_sprite_scaled(int32_t x, int32_t y, uint32_t index,
                                   uint32_t colour) {
    uint32_t e = psx_mod_read_word(SPRITE_TABLE) + 16u * (index & 0xFFFFu);
    uint32_t tx = psx_mod_read_half(e + 0u);
    uint32_t ty = psx_mod_read_half(e + 2u);
    int32_t cx = (int16_t)psx_mod_read_half(e + 4u);
    int32_t cy = (int16_t)psx_mod_read_half(e + 6u);
    int32_t wfield = (int16_t)psx_mod_read_half(e + 8u);
    int32_t h = psx_mod_read_half(e + 10u);
    int32_t depth = (int16_t)psx_mod_read_half(e + 12u);
    uint32_t shift = (uint32_t)(2 - depth) & 31u;
    int32_t w = (int32_t)((uint32_t)wfield << shift);
    uint32_t tpage = ((ty >> 4) & 0x10u) | ((tx & 0x3FFu) >> 6) |
                     ((ty & 0x200u) << 2);
    uint32_t u = ((tx & 0x3Fu) << shift) & 0xFFu;
    uint32_t v = ty & 0xFFu;
    uint32_t ot = psx_mod_read_word(OT_POINTER) + 4u;

    if (tpage != psx_mod_read_half(TPAGE_CURRENT)) {
        uint32_t p = psx_mod_read_word(PRIM_CURSOR);
        psx_mod_write_byte(p + 3u, 1);
        psx_mod_write_word(p + 4u, 0xE1000000u |
                           (psx_mod_read_half(TPAGE_CURRENT) & 0x9FFu));
        psx_mod_write_word(PRIM_CURSOR, p + 8u);
        add_prim(ot, p);
        psx_mod_write_half(TPAGE_CURRENT, (uint16_t)tpage);
    }

    int edge = hud_edge_for_x(x);
    int32_t x0 = scale_x(x, edge), y0 = scale_y(y);
    /* A quad's far UV is the texel edge, not the last texel: u + w maps the
     * whole sprite. A sprite that ends on the page edge (u + w == 256) cannot
     * say 256 in 8 bits; it loses that last column and its quad shrinks with
     * it so the art is not stretched. */
    uint32_t u1 = u + (uint32_t)w, v1 = v + (uint32_t)h;
    int32_t qw = w, qh = h;
    if (u1 > 0xFFu) { qw -= (int32_t)(u1 - 0xFFu); u1 = 0xFFu; }
    if (v1 > 0xFFu) { qh -= (int32_t)(v1 - 0xFFu); v1 = 0xFFu; }
    int32_t x1 = scale_x(x + qw, edge), y1 = scale_y(y + qh);
    uint32_t clut = ((uint32_t)cy << 6) | (((uint32_t)cx >> 4) & 0x3Fu);
    uint32_t page = tpage | (((uint32_t)depth & 3u) << 7);
    uint32_t xy0 = ((uint32_t)y0 << 16) | ((uint32_t)x0 & 0xFFFFu);
    uint32_t xy1 = ((uint32_t)y0 << 16) | ((uint32_t)x1 & 0xFFFFu);
    uint32_t xy2 = ((uint32_t)y1 << 16) | ((uint32_t)x0 & 0xFFFFu);
    uint32_t xy3 = ((uint32_t)y1 << 16) | ((uint32_t)x1 & 0xFFFFu);

    uint32_t p = psx_mod_read_word(PRIM_CURSOR);
    uint32_t rgb = psx_mod_read_byte(colour) |
                   ((uint32_t)psx_mod_read_byte(colour + 1u) << 8) |
                   ((uint32_t)psx_mod_read_byte(colour + 2u) << 16);
    psx_mod_write_byte(p + 3u, 9);
    psx_mod_write_word(p + 4u, 0x2C000000u | rgb);   /* textured quad, opaque */
    psx_mod_write_word(p + 8u, xy0);
    psx_mod_write_word(p + 12u, (clut << 16) | (v << 8) | u);
    psx_mod_write_word(p + 16u, xy1);
    psx_mod_write_word(p + 20u, (page << 16) | (v << 8) | u1);
    psx_mod_write_word(p + 24u, xy2);
    psx_mod_write_word(p + 28u, (v1 << 8) | u);
    psx_mod_write_word(p + 32u, xy3);
    psx_mod_write_word(p + 36u, (v1 << 8) | u1);
    psx_mod_write_word(PRIM_CURSOR, p + 40u);
    add_prim(ot, p);
    remember_hud_tag((p & 0x00FFFFFFu) | 0x80000000u, edge);
}

/* ---- split-screen gap ----------------------------------------------------- */

/* Each view has a static 9-word packet in a 64-byte slot: DR_ENV (E3/E4/E5
 * area and offset, E1/E2/E6) plus a black 512x109 TILE that clears it. The
 * top view covers rows 10-118 of its buffer and the bottom view rows 120-228,
 * so row 119 between them belongs to neither: clean in 4:3, but its
 * native-wide side strips are never redrawn and keep whatever crossed them
 * (the "CHASE!" banner). Grow the bottom view's area up one row and append two
 * one-row black TILEs (x -512..1522, wider than any supported aspect) to its
 * packet; the slot's spare 24 bytes hold exactly those 6 words. The separator
 * stays black and the top view is untouched. */
#define VIEW_ENV_FIRST 0x8006B708u
#define VIEW_ENV_COUNT 4u
#define VIEW_ENV_SLOT  0x40u

static void patch_bottom_view_env(uint32_t p) {
    if (psx_mod_read_byte(p + 3u) != 9u) return;        /* stock or already done */
    uint32_t e3 = psx_mod_read_word(p + 4u);
    uint32_t e5 = psx_mod_read_word(p + 12u);
    if ((e3 >> 24) != 0xE3u || (e5 >> 24) != 0xE5u) return;
    uint32_t top = (e3 >> 10) & 0x3FFu;
    uint32_t ofs = (e5 >> 11) & 0x7FFu;
    if (top != ofs || (ofs != 120u && ofs != 360u)) return;  /* bottom views only */

    psx_mod_write_word(p + 4u, (e3 & ~(0x3FFu << 10)) | ((top - 1u) << 10));
    psx_mod_write_word(p + 40u, 0x60000000u);
    psx_mod_write_word(p + 44u, (0xFFFFu << 16) | ((uint32_t)-512 & 0xFFFFu));
    psx_mod_write_word(p + 48u, (1u << 16) | 1023u);
    psx_mod_write_word(p + 52u, 0x60000000u);
    psx_mod_write_word(p + 56u, (0xFFFFu << 16) | 500u);
    psx_mod_write_word(p + 60u, (1u << 16) | 1023u);
    psx_mod_write_byte(p + 3u, 15u);
}

static void patch_view_gap(void) {
    for (uint32_t k = 0; k < VIEW_ENV_COUNT; ++k)
        patch_bottom_view_env(VIEW_ENV_FIRST + VIEW_ENV_SLOT * k);
}

/* ---- display position (PutDispEnv) ----------------------------------------- */

/* Gameplay centres its picture on a PAL TV with DISPENV screen.y = 24. The
 * LOADING screens draw their art 12 (or 4) rows down in VRAM and compensate
 * with screen.y = 12 (or 20), lifting the picture on the TV. The recompiled
 * presenter shows display rows from the VRAM start and ignores that vertical
 * position, so those screens appeared with a black band on top and their
 * bottom cut off. Turn the vertical position into a display-start offset
 * instead: disp.y += 24 - screen.y, screen.y = 24. Gameplay (24) is untouched.
 * Each DISPENV remembers its offset so a later disp.y rewrite (buffer flip)
 * is shifted again, never twice. */
#define PUT_DISP_ENV      0x80022788u
#define PUT_DISP_ENV_W0   0x27BDFFE0u   /* addiu sp,sp,-32 */
#define SCREEN_Y_GAMEPLAY 24
#define DISPENV_SLOTS     8u

static struct {
    uint32_t env;
    int16_t shift;
    int16_t last_y;
} s_dispenv[DISPENV_SLOTS];

static void tomjerry_put_disp_env_entry(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (psx_mod_read_word(PUT_DISP_ENV) != PUT_DISP_ENV_W0) return;
    uint32_t env = cpu->gpr[4];                         /* a0 = DISPENV* */
    if (!ram_address(env)) return;
    int16_t disp_y = (int16_t)psx_mod_read_half(env + 2u);
    int16_t screen_y = (int16_t)psx_mod_read_half(env + 10u);

    uint32_t slot = DISPENV_SLOTS;
    for (uint32_t i = 0; i < DISPENV_SLOTS; ++i)
        if (s_dispenv[i].env == env) { slot = i; break; }

    if (screen_y != SCREEN_Y_GAMEPLAY) {
        int shift = SCREEN_Y_GAMEPLAY - screen_y;
        if (shift <= 0 || shift > 32) return;           /* only the known lift */
        /* screen.y counts scanlines; an interlaced (480-line) picture has two
         * VRAM rows per scanline. */
        if (psx_mod_read_byte(env + 16u) || psx_mod_read_half(env + 6u) > 256u)
            shift *= 2;
        if (slot == DISPENV_SLOTS) {
            for (uint32_t i = 0; i < DISPENV_SLOTS; ++i)
                if (!s_dispenv[i].env) { slot = i; break; }
            if (slot == DISPENV_SLOTS) slot = 0;
        }
        disp_y = (int16_t)(disp_y + shift);
        psx_mod_write_half(env + 2u, (uint16_t)disp_y);
        psx_mod_write_half(env + 10u, (uint16_t)SCREEN_Y_GAMEPLAY);
        s_dispenv[slot].env = env;
        s_dispenv[slot].shift = (int16_t)shift;
        s_dispenv[slot].last_y = disp_y;
    } else if (slot != DISPENV_SLOTS && disp_y != s_dispenv[slot].last_y) {
        disp_y = (int16_t)(disp_y + s_dispenv[slot].shift);
        psx_mod_write_half(env + 2u, (uint16_t)disp_y);
        s_dispenv[slot].last_y = disp_y;
    }
}

static int tomjerry_hud_sprite_filter(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (psx_mod_widescreen_x_margin() <= 0) return 0;
    /* Another overlay can occupy the same address outside gameplay. */
    if (!hud_overlay_resident()) return 0;
    s_last_hud_vblank = s_vblank;
    patch_view_gap();

    if (s_hud_percent < 100) {
        draw_hud_sprite_scaled((int16_t)cpu->gpr[4], (int16_t)cpu->gpr[5],
                               cpu->gpr[6], cpu->gpr[7]);
        tag_health_bars();
        return 1;   /* replaces the stock body; return via $ra */
    }

    uint32_t cursor = (psx_mod_read_word(PRIM_CURSOR) & 0x00FFFFFFu) | 0x80000000u;
    if (!ram_address(cursor)) return 0;
    int edge = hud_edge_for_x((int16_t)cpu->gpr[4]);   /* a0 = screen x */
    /* The SPRT starts at the cursor, or after a DR_TPAGE when the texture page
     * changes; tagging both is harmless because a DR_TPAGE has no position. */
    remember_hud_tag(cursor, edge);
    remember_hud_tag(cursor + 8u, edge);
    tag_health_bars();
    return 0;   /* stock body draws the sprite */
}

static void tomjerry_hud_vblank(void) {
    ++s_vblank;
    /* Re-assert periodically: the runtime resets the interval whenever the
     * window changes display or the panel refresh changes. */
    if (s_force_vsync && s_vblank % VSYNC_REASSERT_VBLANKS == 1u)
        gl_renderer_set_swap_interval(1);
    if (hud_overlay_resident()) {
        int in_match = s_vblank - s_last_hud_vblank <= GATE_HUD_VBLANKS ||
                       psx_mod_read_word(PAUSE_MENU_FLAG) == 1u;
        uint32_t want = in_match ? GATE_IN_MATCH : 0u;
        if (psx_mod_read_word(GATE_WORD) != want)
            psx_mod_write_word(GATE_WORD, want);
    }
    if (s_force_ntsc_timing && psx_mod_game_started() &&
        psx_mod_read_word(LIBGPU_VIDEO_MODE) == 1u)
        psx_mod_write_word(LIBGPU_VIDEO_MODE, 0u);
    if (psx_mod_widescreen_x_margin() <= 0) return;
    for (uint32_t i = 0; i < HUD_TAG_SLOTS; ++i) {
        if (!s_hud_tags[i].packet) continue;
        if (s_vblank - s_hud_tags[i].born > HUD_TAG_VBLANKS) {
            s_hud_tags[i].packet = 0;
            continue;
        }
        psx_mod_tag_hud_primitive(s_hud_tags[i].packet, s_hud_tags[i].edge);
    }
    if (hud_overlay_resident()) {
        patch_view_gap();
        if (s_hud_percent < 100) scale_health_bars();
        tag_health_bars();
    }
}

/* ---- pause ------------------------------------------------------------- */

/* The pause menu (word at 0x8006A69C is 1 while its loop runs) stops
 * rendering the room: the last frame stays on screen and only the menu text
 * is drawn over it, so the classifier would fall back to a 4:3 menu frame.
 * Hold the gameplay classification for exactly that state. */

static int tomjerry_retained_scene(void) {
    if (psx_mod_widescreen_x_margin() <= 0) return PSX_MOD_SCENE_RELEASE;
    if (!hud_overlay_resident()) return PSX_MOD_SCENE_RELEASE;
    return psx_mod_read_word(PAUSE_MENU_FLAG) == 1u ? PSX_MOD_SCENE_HOLD
                                                     : PSX_MOD_SCENE_RELEASE;
}

static void tomjerry_widescreen_activate(void) {
    char aspect[16], hud[16], rate[16];
    unsigned num = 16u, den = 9u;
    int fit = 1;

    if (!psx_mod_option_value(PKG, FEATURE, "aspect", aspect, sizeof aspect))
        strcpy(aspect, "fit");

    if (strcmp(aspect, "16-9") == 0) { fit = 0; }
    else if (strcmp(aspect, "21-9") == 0) { num = 21u; fit = 0; }
    else if (strcmp(aspect, "32-9") == 0) { num = 32u; fit = 0; }

    s_hud_percent = 100;
    if (psx_mod_option_value(PKG, FEATURE, "hud_size", hud, sizeof hud)) {
        int pct = 0;
        if (sscanf(hud, "%d", &pct) == 1 && pct >= 50 && pct <= 100)
            s_hud_percent = pct;
    }

    /* Frame rate. The game advances one fixed step per frame and a frame
     * takes the PS1 CPU just over one VBlank, so it shows a new frame every
     * second VBlank: 25 fps on PAL timing, 30 fps on NTSC timing -- the speed
     * the game was designed for (the PAL release does not compensate). Music
     * runs off the SPU and timers, which keep their real speed either way. */
    s_force_ntsc_timing = 1;
    if (psx_mod_option_value(PKG, FEATURE, "frame_rate", rate, sizeof rate) &&
        strcmp(rate, "25") == 0)
        s_force_ntsc_timing = 0;

    char vsync[8];
    s_force_vsync = 1;
    if (psx_mod_option_value(PKG, FEATURE, "high_refresh_vsync", vsync, sizeof vsync) &&
        strcmp(vsync, "off") == 0)
        s_force_vsync = 0;

    fprintf(stdout, "TOMJERRY WIDESCREEN PLUGIN ACTIVATED (%s, HUD %d%%, %s fps, vsync %s)\n",
            aspect, s_hud_percent, s_force_ntsc_timing ? "30" : "25",
            s_force_vsync ? "forced" : "runtime");
    memset(s_hud_tags, 0, sizeof s_hud_tags);
    memset(s_bar_shadow, 0, sizeof s_bar_shadow);
    memset(s_dispenv, 0, sizeof s_dispenv);
    s_hud_tag_next = 0;
    s_vblank = 0;
    (void)psx_mod_set_fixed_display_aspect(num, den);
    if (fit)
        (void)psx_mod_set_adaptive_display_aspect(0u, 0u);
    psx_mod_set_retained_scene_predicate(tomjerry_retained_scene);
}

PSX_MOD_CONSTRUCTOR(psx_register_tomjerry_widescreen_plugin) {
    (void)psx_mod_register_activation_plugin(
        "tomjerry.widescreen", tomjerry_widescreen_activate);
    (void)psx_mod_register_function_filter_plugin(
        "tomjerry.widescreen", HUD_SPRITE_FN, tomjerry_hud_sprite_filter);
    (void)psx_mod_register_vblank_plugin(
        "tomjerry.widescreen", tomjerry_hud_vblank);
    (void)psx_mod_register_function_entry_plugin(
        "tomjerry.widescreen", PUT_DISP_ENV, tomjerry_put_disp_env_entry);
}
