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
/* Health bars are three double-buffered TILE packets (16 bytes) per player,
 * held in the player object at +368+52, +84, +116. */
#define PLAYER_TABLE    0x8006CA20u
#define PLAYER_COUNT    2
#define BAR_PACKETS     0x1A4u        /* 368 + 52 */
#define BAR_PACKET_N    6

static int hud_edge_for_x(int32_t x) {
    uint32_t w = psx_mod_display_width();
    int32_t mid = w ? (int32_t)(w / 2u) : 256;
    return x < mid ? -1 : 1;
}

static int ram_address(uint32_t a) {
    a &= 0x1FFFFFFFu;
    return a >= 0x10000u && a < 0x200000u;
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

static void tomjerry_hud_sprite_entry(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (psx_mod_widescreen_x_margin() <= 0) return;
    /* Another overlay can occupy the same address outside gameplay. */
    if (psx_mod_read_word(HUD_SPRITE_FN) != HUD_SPRITE_W0 ||
        psx_mod_read_word(HUD_SPRITE_FN + 4u) != HUD_SPRITE_W1)
        return;

    uint32_t cursor = (psx_mod_read_word(PRIM_CURSOR) & 0x00FFFFFFu) | 0x80000000u;
    if (!ram_address(cursor)) return;
    int edge = hud_edge_for_x((int16_t)cpu->gpr[4]);   /* a0 = screen x */
    /* The SPRT starts at the cursor, or after a DR_TPAGE when the texture page
     * changes; tagging both is harmless because a DR_TPAGE has no position. */
    remember_hud_tag(cursor, edge);
    remember_hud_tag(cursor + 8u, edge);
    tag_health_bars();
}

static void tomjerry_hud_vblank(void) {
    ++s_vblank;
    if (psx_mod_widescreen_x_margin() <= 0) return;
    for (uint32_t i = 0; i < HUD_TAG_SLOTS; ++i) {
        if (!s_hud_tags[i].packet) continue;
        if (s_vblank - s_hud_tags[i].born > HUD_TAG_VBLANKS) {
            s_hud_tags[i].packet = 0;
            continue;
        }
        psx_mod_tag_hud_primitive(s_hud_tags[i].packet, s_hud_tags[i].edge);
    }
    if (psx_mod_read_word(HUD_SPRITE_FN) == HUD_SPRITE_W0 &&
        psx_mod_read_word(HUD_SPRITE_FN + 4u) == HUD_SPRITE_W1)
        tag_health_bars();
}

/* The pause menu (word at 0x8006A69C is 1 while its loop runs) stops
 * rendering the room: the last frame stays on screen and only the menu text
 * is drawn over it, so the classifier would fall back to a 4:3 menu frame.
 * Hold the gameplay classification for exactly that state. */
#define PAUSE_MENU_FLAG 0x8006A69Cu

static int tomjerry_retained_scene(void) {
    if (psx_mod_widescreen_x_margin() <= 0) return PSX_MOD_SCENE_RELEASE;
    if (psx_mod_read_word(HUD_SPRITE_FN) != HUD_SPRITE_W0 ||
        psx_mod_read_word(HUD_SPRITE_FN + 4u) != HUD_SPRITE_W1)
        return PSX_MOD_SCENE_RELEASE;
    return psx_mod_read_word(PAUSE_MENU_FLAG) == 1u ? PSX_MOD_SCENE_HOLD
                                                     : PSX_MOD_SCENE_RELEASE;
}

static void tomjerry_widescreen_activate(void) {
    char aspect[16];
    unsigned num = 16u, den = 9u;
    int fit = 1;

    if (!psx_mod_option_value(PKG, FEATURE, "aspect", aspect, sizeof aspect))
        strcpy(aspect, "fit");

    if (strcmp(aspect, "16-9") == 0) { fit = 0; }
    else if (strcmp(aspect, "21-9") == 0) { num = 21u; fit = 0; }
    else if (strcmp(aspect, "32-9") == 0) { num = 32u; fit = 0; }

    fprintf(stdout, "TOMJERRY WIDESCREEN PLUGIN ACTIVATED (%s)\n", aspect);
    memset(s_hud_tags, 0, sizeof s_hud_tags);
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
    (void)psx_mod_register_function_entry_plugin(
        "tomjerry.widescreen", HUD_SPRITE_FN, tomjerry_hud_sprite_entry);
    (void)psx_mod_register_vblank_plugin(
        "tomjerry.widescreen", tomjerry_hud_vblank);
}
