// Rhythia pour NumWorks (application externe .nwa, API EADK) - v2
// Fleches : deplacer le curseur. Une note est validee quand le curseur
// est dessus au bon moment. Retour (Back) = quitter / abandonner.
// Menu de reglages : haut/bas = choisir, gauche/droite = modifier, OK = jouer.

#include "eadk.h"
#include <stdbool.h>
#include <stdint.h>

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "Rhythia";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define BG_R 15
#define BG_G 15
#define BG_B 30
#define BG      RGB(BG_R, BG_G, BG_B)
#define WHITE   RGB(255, 255, 255)
#define GREY    RGB(90, 90, 140)
#define YELLOW  RGB(255, 220, 80)

#define SCR_W 320
#define SCR_H 240
#define CX 160
#define CY 125
#define SP 50   // taille d'une case de la grille (pixels)
#define NS 38   // taille d'une note a la hauteur de la grille (pixels)

// ================== REGLAGES (modifiables dans le menu) ==================
// Valeurs en dixiemes (155 = 15.5), sauf l'opacite en pourcent.
enum { S_DIST, S_SPEED, S_SENS, S_OPACITY, S_COUNT };
static int settings[S_COUNT] = {155, 355, 35, 100};
//   S_DIST    : distance d'approche            (15.5)
//   S_SPEED   : vitesse d'approche             (35.5)
//   S_SENS    : sensibilite = cases/seconde    (3.5)
//   S_OPACITY : opacite des notes en %         (100)
static const int s_min[S_COUNT]  = {50, 100, 10, 10};
static const int s_max[S_COUNT]  = {400, 1000, 100, 100};
static const int s_step[S_COUNT] = {5, 5, 5, 10};
static const char *s_name[S_COUNT] = {
  "Distance d'approche", "Vitesse d'approche", "Sensibilite", "Opacite des notes (%)"
};

#define HIT_WINDOW 0.055f // secondes (55 ms)
#define HIT_RADIUS 0.7f   // zone de hit autour de la note (en cases)
// =========================================================================

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

// "prefix" + entier + espaces de remplissage (jusqu'a pad caracteres)
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

// Affiche v (en dixiemes) sous la forme "15.5"
static void tenths(char *buf, int v, int pad) {
  int n = 0, l = 0, ip = v / 10;
  char tmp[8];
  if (ip == 0) tmp[l++] = '0';
  while (ip > 0 && l < 7) { tmp[l++] = (char)('0' + ip % 10); ip /= 10; }
  while (l > 0) buf[n++] = tmp[--l];
  buf[n++] = '.';
  buf[n++] = (char)('0' + v % 10);
  while (n < pad && n < 22) buf[n++] = ' ';
  buf[n] = 0;
}

// Melange une couleur avec le fond : opacite en % (100 = pleine couleur)
static uint16_t mix(int r, int g, int b, int pct) {
  int rr = (r * pct + BG_R * (100 - pct)) / 100;
  int gg = (g * pct + BG_G * (100 - pct)) / 100;
  int bb = (b * pct + BG_B * (100 - pct)) / 100;
  return RGB(rr, gg, bb);
}

typedef struct { int16_t x, y, s; } box_t;

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static float absf(float v) { return v < 0 ? -v : v; }

// Grille 3x3 : 4 lignes verticales + 4 lignes horizontales
static void draw_grid(void) {
  int x0 = CX - SP * 3 / 2, y0 = CY - SP * 3 / 2, len = SP * 3 + 1;
  for (int i = 0; i <= 3; i++) {
    fill(x0 + i * SP, y0, 1, len, GREY);
    fill(x0, y0 + i * SP, len, 1, GREY);
  }
}

// ---------- Jeu ----------
static void play(void) {
  const float approach_distance = (float)settings[S_DIST] / 10.0f;
  const float approach_speed = (float)settings[S_SPEED] / 10.0f;
  const float approach_time = approach_distance / approach_speed;
  const float cursor_speed = (float)settings[S_SENS] / 10.0f;
  const int opacity = settings[S_OPACITY];

  const uint16_t cols[3] = {
    mix(255, 80, 120, opacity), mix(80, 200, 255, opacity), mix(255, 220, 80, opacity)
  };
  const uint16_t hole = BG;

  int hits = 0, miss = 0, combo = 0, best = 0;
  int32_t score = 0;
  float cx = 0, cy = 0;
  box_t prev[64];
  int prev_n = 0;
  int first = 0;
  char buf[24];

  for (int i = 0; i < note_count; i++) state[i] = 0;
  fill(0, 0, SCR_W, SCR_H, BG);

  // Textes fixes du HUD
  text("SCORE", 8, 90, GREY);
  text("COMBO", 255, 90, GREY);

  uint64_t t0 = eadk_timing_millis() + 2000;
  float last = -2.0f;

  while (first < note_count) {
    eadk_keyboard_state_t kb = eadk_keyboard_scan();
    if (eadk_keyboard_key_down(kb, eadk_key_back)) return;

    // Le temps vient de l'horloge : les notes avancent a vitesse constante
    float now = ((float)((int64_t)eadk_timing_millis() - (int64_t)t0)) / 1000.0f;
    float dt = now - last;
    last = now;
    if (dt > 0.1f) dt = 0.1f;

    float sp = cursor_speed * dt;
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
      if (dtm > approach_time) break;
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

    // Effacer les anciens elements mobiles
    for (int i = 0; i < prev_n; i++) fill(prev[i].x, prev[i].y, prev[i].s, prev[i].s, BG);
    prev_n = 0;

    draw_grid();

    // Notes : les plus lointaines d'abord.
    // Position et taille evoluent lineairement avec le temps => vitesse constante.
    for (int i = draw_n - 1; i >= 0; i--) {
      int j = draw[i];
      float dtm = (float)notes[j].ms / 1000.0f - now;
      float z = dtm * approach_speed;      // distance restante
      if (z < 0) z = 0;
      float s = 0.15f + 0.85f * (1.0f - z / approach_distance);
      int sz = (int)(NS * s);
      int px = (int)(CX + notes[j].x * SP * s);
      int py = (int)(CY + notes[j].y * SP * s);
      int x0 = px - sz / 2, y0 = py - sz / 2;
      fill(x0, y0, sz, sz, cols[j % 3]);
      if (sz > 8) fill(x0 + 3, y0 + 3, sz - 6, sz - 6, hole);
      if (prev_n < 64) { prev[prev_n].x = x0 - 1; prev[prev_n].y = y0 - 1; prev[prev_n].s = sz + 2; prev_n++; }
    }

    // Curseur
    int kx = (int)(CX + cx * SP) - 7;
    int ky = (int)(CY + cy * SP) - 7;
    fill(kx, ky, 14, 14, WHITE);
    fill(kx + 2, ky + 2, 10, 10, BG);
    if (prev_n < 64) { prev[prev_n].x = kx - 1; prev[prev_n].y = ky - 1; prev[prev_n].s = 16; prev_n++; }

    // Score et combo, de part et d'autre de la grille
    label(buf, "", score, 9);
    text(buf, 8, 108, WHITE);
    label(buf, "x", combo, 8);
    text(buf, 255, 108, YELLOW);
  }

  // Ecran de fin
  fill(0, 0, SCR_W, SCR_H, BG);
  int total = hits + miss;
  int acc = total ? (100 * hits) / total : 0;
  text("FIN", 145, 40, WHITE);
  label(buf, "Score : ", score, 0);         text(buf, 90, 80, WHITE);
  label(buf, "Precision % : ", acc, 0);     text(buf, 90, 105, WHITE);
  label(buf, "Meilleur combo : ", best, 0); text(buf, 90, 130, WHITE);
  label(buf, "Rates : ", miss, 0);          text(buf, 90, 155, WHITE);
  text("OK : menu   Retour : quitter", 50, 205, GREY);
}

// ---------- Menu de reglages ----------
// Renvoie true pour jouer, false pour quitter l'application.
static bool menu(void) {
  int sel = 0;
  bool redraw = true;
  char buf[24];
  eadk_timing_msleep(300);
  while (true) {
    if (redraw) {
      fill(0, 0, SCR_W, SCR_H, BG);
      text("RHYTHIA - REGLAGES", 85, 20, WHITE);
      for (int i = 0; i < S_COUNT; i++) {
        uint16_t c = (i == sel) ? YELLOW : WHITE;
        int y = 70 + i * 28;
        if (i == sel) text(">", 14, y, YELLOW);
        text(s_name[i], 30, y, c);
        if (i == S_OPACITY) label(buf, "", settings[i], 5);
        else tenths(buf, settings[i], 5);
        text(buf, 255, y, c);
      }
      text("Haut/Bas : choisir  Gauche/Droite : changer", 8, 190, GREY);
      text("OK : jouer   Retour : quitter", 45, 210, GREY);
      redraw = false;
    }
    eadk_keyboard_state_t kb = eadk_keyboard_scan();
    if (eadk_keyboard_key_down(kb, eadk_key_back)) return false;
    if (eadk_keyboard_key_down(kb, eadk_key_ok)) { eadk_timing_msleep(200); return true; }
    if (eadk_keyboard_key_down(kb, eadk_key_up) && sel > 0) {
      sel--; redraw = true; eadk_timing_msleep(150);
    } else if (eadk_keyboard_key_down(kb, eadk_key_down) && sel < S_COUNT - 1) {
      sel++; redraw = true; eadk_timing_msleep(150);
    } else if (eadk_keyboard_key_down(kb, eadk_key_left)) {
      settings[sel] -= s_step[sel];
      if (settings[sel] < s_min[sel]) settings[sel] = s_min[sel];
      redraw = true; eadk_timing_msleep(130);
    } else if (eadk_keyboard_key_down(kb, eadk_key_right)) {
      settings[sel] += s_step[sel];
      if (settings[sel] > s_max[sel]) settings[sel] = s_max[sel];
      redraw = true; eadk_timing_msleep(130);
    }
    eadk_timing_msleep(20);
  }
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  build_demo_map();

  while (true) {
    if (!menu()) return 0;
    play();
    eadk_timing_msleep(400);
    while (true) {
      eadk_keyboard_state_t kb = eadk_keyboard_scan();
      if (eadk_keyboard_key_down(kb, eadk_key_back)) return 0;
      if (eadk_keyboard_key_down(kb, eadk_key_ok)) break;
      eadk_timing_msleep(30);
    }
  }
}
