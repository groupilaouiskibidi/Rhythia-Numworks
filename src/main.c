// Rhythia pour NumWorks (application externe .nwa, API EADK)
// Jeu de rythme 3x3 : deplace le curseur avec les fleches,
// une note est validee quand le curseur est dessus au bon moment.
// Retour (Back) = quitter.

#include "eadk.h"
#include <stdbool.h>
#include <stdint.h>

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "Rhythia";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;

// ================== REGLAGES ==================
#define APPROACH_DISTANCE 15.5f  // distance d'approche
#define APPROACH_SPEED    35.5f  // vitesse d'approche
#define CURSOR_SPEED      3.5f   // cases par seconde
#define HIT_WINDOW        0.055f // secondes (55 ms comme Rhythia)
#define HIT_RADIUS        0.7f   // taille de la zone de hit (en cases)
// ===============================================

#define APPROACH_TIME (APPROACH_DISTANCE / APPROACH_SPEED)

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define BG      RGB(15, 15, 30)
#define WHITE   RGB(255, 255, 255)
#define GREY    RGB(70, 70, 110)
#define YELLOW  RGB(255, 220, 80)

#define SCR_W 320
#define SCR_H 240
#define CX 160
#define CY 125
#define SP 50   // ecart entre cases a l'ecran (taille pleine)
#define NS 38   // taille d'une note a l'ecran (taille pleine)

// ---------- MAP ----------
// Format : {temps_en_ms, x, y} avec x et y dans -1, 0, 1 (triees par temps)
typedef struct { int32_t ms; int8_t x; int8_t y; } note_t;

#define MAX_NOTES 512
static note_t notes[MAX_NOTES];
static int note_count = 0;
static uint8_t state[MAX_NOTES]; // 0 = en attente, 1 = touchee, 2 = ratee

static void add_note(int32_t ms, int x, int y) {
  if (note_count < MAX_NOTES) {
    notes[note_count].ms = ms;
    notes[note_count].x = (int8_t)x;
    notes[note_count].y = (int8_t)y;
    note_count++;
  }
}

// Map de demo (128 BPM). Remplace par tes propres notes avec add_note(...)
static void build_demo_map(void) {
  static const int8_t pat[16][2] = {
    {-1,-1},{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},
    {0,0},{1,-1},{-1,1},{-1,-1},{1,1},{0,0},{-1,0},{1,0}
  };
  note_count = 0;
  int32_t beat = 60000 / 128;
  int32_t t = 2000;
  for (int k = 0; k < 96; k++) {
    add_note(t, pat[k % 16][0], pat[k % 16][1]);
    t += (k % 4 == 3) ? beat / 2 : beat;
  }
}

// ---------- Affichage ----------
static void fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCR_W) w = SCR_W - x;
  if (y + h > SCR_H) h = SCR_H - y;
  if (w <= 0 || h <= 0) return;
  eadk_rect_t r = {(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h};
  eadk_display_push_rect_uniform(r, c);
}

static void text(const char *s, int x, int y, uint16_t fg) {
  eadk_point_t p = {(uint16_t)x, (uint16_t)y};
  eadk_display_draw_string(s, p, false, fg, BG);
}

// Ecrit "prefix" + nombre + espaces de remplissage dans buf (taille >= 24)
static void label(char *buf, const char *prefix, int32_t v, int pad) {
  int n = 0;
  while (*prefix && n < 12) buf[n++] = *prefix++;
  char tmp[12];
  int l = 0;
  bool neg = v < 0;
  if (neg) v = -v;
  if (v == 0) tmp[l++] = '0';
  while (v > 0 && l < 11) { tmp[l++] = (char)('0' + v % 10); v /= 10; }
  if (neg) buf[n++] = '-';
  while (l > 0) buf[n++] = tmp[--l];
  while (n < pad && n < 22) buf[n++] = ' ';
  buf[n] = 0;
}

typedef struct { int16_t x, y, s; } box_t;

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static float absf(float v) { return v < 0 ? -v : v; }

// ---------- Jeu ----------
static void play(void) {
  int hits = 0, miss = 0, combo = 0, best = 0;
  int32_t score = 0;
  float cx = 0, cy = 0;
  box_t prev[64];
  int prev_n = 0;
  int first = 0;
  char buf[24];

  for (int i = 0; i < note_count; i++) state[i] = 0;
  fill(0, 0, SCR_W, SCR_H, BG);

  uint64_t t0 = eadk_timing_millis() + 2000;
  float last = -2.0f;

  while (first < note_count) {
    eadk_keyboard_state_t kb = eadk_keyboard_scan();
    if (eadk_keyboard_key_down(kb, eadk_key_back)) return;

    float now = ((float)((int64_t)eadk_timing_millis() - (int64_t)t0)) / 1000.0f;
    float dt = now - last;
    last = now;
    if (dt > 0.1f) dt = 0.1f;

    float sp = CURSOR_SPEED * dt;
    if (eadk_keyboard_key_down(kb, eadk_key_left))  cx -= sp;
    if (eadk_keyboard_key_down(kb, eadk_key_right)) cx += sp;
    if (eadk_keyboard_key_down(kb, eadk_key_up))    cy -= sp;
    if (eadk_keyboard_key_down(kb, eadk_key_down))  cy += sp;
    cx = clampf(cx, -1.0f, 1.0f);
    cy = clampf(cy, -1.0f, 1.0f);

    // Jugement des notes + liste de celles a dessiner
    int draw[64];
    int draw_n = 0;
    for (int j = first; j < note_count; j++) {
      float dtm = (float)notes[j].ms / 1000.0f - now;
      if (dtm > APPROACH_TIME) break;
      if (state[j] != 0) continue;
      if (dtm <= HIT_WINDOW &&
          absf(cx - notes[j].x) < HIT_RADIUS &&
          absf(cy - notes[j].y) < HIT_RADIUS) {
        state[j] = 1;
        hits++;
        combo++;
        if (combo > best) best = combo;
        score += 100 + 10 * (combo < 50 ? combo : 50);
      } else if (dtm < -HIT_WINDOW) {
        state[j] = 2;
        miss++;
        combo = 0;
      } else if (draw_n < 60) {
        draw[draw_n++] = j;
      }
    }
    while (first < note_count && state[first] != 0) first++;

    eadk_display_wait_for_vblank();

    // Effacer les anciens elements
    for (int i = 0; i < prev_n; i++) fill(prev[i].x, prev[i].y, prev[i].s, prev[i].s, BG);
    prev_n = 0;

    // Points de la grille
    for (int gx = -1; gx <= 1; gx++)
      for (int gy = -1; gy <= 1; gy++)
        fill(CX + gx * SP - 1, CY + gy * SP - 1, 3, 3, GREY);

    // Notes : les plus lointaines d'abord
    for (int i = draw_n - 1; i >= 0; i--) {
      int j = draw[i];
      float dtm = (float)notes[j].ms / 1000.0f - now;
      float z = dtm * APPROACH_SPEED;
      if (z < 0) z = 0;
      float s = 0.15f + 0.85f * (1.0f - z / APPROACH_DISTANCE);
      int sz = (int)(NS * s);
      int px = (int)(CX + notes[j].x * SP * s);
      int py = (int)(CY + notes[j].y * SP * s);
      int x0 = px - sz / 2, y0 = py - sz / 2;
      uint16_t col = (j % 3 == 0) ? RGB(255, 80, 120)
                   : (j % 3 == 1) ? RGB(80, 200, 255) : YELLOW;
      fill(x0, y0, sz, sz, col);
      if (sz > 8) fill(x0 + 3, y0 + 3, sz - 6, sz - 6, BG);
      if (prev_n < 64) { prev[prev_n].x = x0 - 1; prev[prev_n].y = y0 - 1; prev[prev_n].s = sz + 2; prev_n++; }
    }

    // Curseur
    int kx = (int)(CX + cx * SP) - 7;
    int ky = (int)(CY + cy * SP) - 7;
    fill(kx, ky, 14, 14, WHITE);
    fill(kx + 2, ky + 2, 10, 10, BG);
    if (prev_n < 64) { prev[prev_n].x = kx - 1; prev[prev_n].y = ky - 1; prev[prev_n].s = 16; prev_n++; }

    // HUD
    label(buf, "Score ", score, 14);
    text(buf, 6, 4, WHITE);
    label(buf, "x", combo, 5);
    text(buf, 270, 4, YELLOW);
  }

  // Ecran de fin
  fill(0, 0, SCR_W, SCR_H, BG);
  int total = hits + miss;
  int acc = total ? (100 * hits) / total : 0;
  text("FIN", 145, 40, WHITE);
  label(buf, "Score : ", score, 0);       text(buf, 90, 80, WHITE);
  label(buf, "Precision % : ", acc, 0);   text(buf, 90, 105, WHITE);
  label(buf, "Meilleur combo : ", best, 0); text(buf, 90, 130, WHITE);
  label(buf, "Rates : ", miss, 0);        text(buf, 90, 155, WHITE);
  text("OK : rejouer   Retour : quitter", 40, 205, GREY);
}

int main(int argc, char *argv[]) {
  build_demo_map();

  fill(0, 0, SCR_W, SCR_H, BG);
  text("RHYTHIA", 125, 70, WHITE);
  text("Fleches : deplacer le curseur", 45, 110, WHITE);
  text("OK : jouer   Retour : quitter", 45, 135, GREY);

  // Attente du menu
  while (true) {
    eadk_keyboard_state_t kb = eadk_keyboard_scan();
    if (eadk_keyboard_key_down(kb, eadk_key_back)) return 0;
    if (eadk_keyboard_key_down(kb, eadk_key_ok)) break;
    eadk_timing_msleep(30);
  }
  eadk_timing_msleep(250);

  // Boucle de parties
  while (true) {
    play();
    eadk_timing_msleep(400);
    while (true) {
      eadk_keyboard_state_t kb = eadk_keyboard_scan();
      if (eadk_keyboard_key_down(kb, eadk_key_back)) return 0;
      if (eadk_keyboard_key_down(kb, eadk_key_ok)) break;
      eadk_timing_msleep(30);
    }
    eadk_timing_msleep(250);
  }
}
