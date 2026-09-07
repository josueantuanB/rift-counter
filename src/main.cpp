// Riftbound point counter - ES3C28P (ESP32-S3 + ILI9341V 240x320 + FT6336)

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <esp_sleep.h>

// Numero de build: es lo unico que se compara con el manifiesto. Subirlo en
// cada release. La cadena solo se muestra en pantalla.
#define FW_VERSION 7
#define VERSION    "v0.7"

// --- OTA -----------------------------------------------------------------
// Rellenar con el repositorio. El manifiesto es un JSON de dos campos en la
// rama principal; el binario, un asset del release.
#define GH_USER "josueantuanB"
#define GH_REPO "rift-counter"

// El manifiesto va como asset del release, no en la rama: asi "latest" lo
// resuelve GitHub y publicar es un solo paso, sin commit de vuelta.
#define MANIFEST_URL \
  "https://github.com/" GH_USER "/" GH_REPO "/releases/latest/download/firmware.json"

// En Riftbound la partida se gana al llegar a 8 puntos.
#define TARGET_POINTS 8

// Tiempo de partida, en minutos. La cuenta atras arranca al elegir formato.
#define MATCH_MIN_BO1 30
#define MATCH_MIN_BO3 60
#define CLOCK_WARN_S  300  // ultimos 5 minutos, en rojo

// Minutos sin tocar la pantalla antes de apagarse solo.
#define IDLE_OFF_MIN 10

// ---------------------------------------------------------------- hardware
// ES3C28P: LCD ILI9341V por SPI (pines en platformio.ini) + tactil FT6336 I2C.
#define TP_SDA   16
#define TP_SCL   15
#define TP_RST   18
#define TP_INT   17
#define TP_ADDR  0x38
#define BAT_ADC  9

#define SCREEN_ROTATION 1  // 1 = apaisado 320x240 (3 = apaisado al reves)

// Perilla de calibracion: como caen los ejes del tactil sobre la pantalla ya
// rotada. Si los taps salen espejados o girados, probar 0..3 hasta que cuadre.
#define TOUCH_MAP   1
#define TOUCH_DEBUG 0  // 1 => imprime crudo + mapeado por Serial en cada toque

// Rango crudo que el panel llega a reportar de verdad. Si los botones pegados a
// un borde no responden, es que el panel no alcanza el extremo: mirar que
// imprime TOUCH_DEBUG al tocar las esquinas y meter aqui esos valores.
#define TP_RAW_X_MIN 0
#define TP_RAW_X_MAX 239
#define TP_RAW_Y_MIN 0
#define TP_RAW_Y_MAX 319

// Perilla de calibracion: la placa mide VBAT por un divisor resistivo. Si el
// porcentaje sale desviado, medir VBAT con un multimetro y ajustar el factor.
#define BAT_DIVIDER 2.0f
#define BAT_MIN_MV  3000  // por debajo se asume que no hay bateria (solo USB)

TFT_eSPI tft;

// ------------------------------------------------------------------ estilo
#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define C_BG       RGB(0x0A, 0x14, 0x28)  // negro-azul hextech
#define C_PANEL    RGB(0x10, 0x20, 0x3C)
#define C_GOLD     RGB(0xC8, 0xAA, 0x6E)
#define C_GOLD_DIM RGB(0x78, 0x5A, 0x28)
#define C_TEAL     RGB(0x0A, 0xC8, 0xB9)
#define C_TEXT     RGB(0xF0, 0xE6, 0xD2)
#define C_MUTED    RGB(0x5B, 0x5A, 0x56)
#define C_MODAL    RGB(0x06, 0x0C, 0x18)  // fondo del modal, mas oscuro que C_PANEL
#define C_FURY     RGB(0xE0, 0x41, 0x3E)  // dominio Fury, aqui bateria baja

// Resto de dominios de Riftbound, para cuando haga falta:
// Calm #3B8FE8  Mind #F0C040  Body #E8873B  Chaos #9B59D0  Order #46C46E

// --------------------------------------------------------------- primitivas
struct Btn {
  int16_t x, y, w, h;
  const char *label;
};

static bool hit(const Btn &b, int16_t x, int16_t y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

static void diamond(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  tft.fillTriangle(cx, cy - r, cx - r, cy, cx + r, cy, c);
  tft.fillTriangle(cx, cy + r, cx - r, cy, cx + r, cy, c);
}

static void drawRule(int16_t y) {
  tft.drawFastHLine(46, y, 228, C_GOLD_DIM);
  diamond(36, y, 5, C_GOLD);
  diamond(284, y, 5, C_GOLD);
}

#define BEVEL 8

// Solo el contorno biselado, para repintar el borde sin borrar el texto.
static void panelOutline(const Btn &b, uint16_t c) {
  const int16_t x2 = b.x + b.w - 1, y2 = b.y + b.h - 1;
  tft.drawFastHLine(b.x + BEVEL, b.y, b.w - 2 * BEVEL, c);
  tft.drawFastHLine(b.x + BEVEL, y2, b.w - 2 * BEVEL, c);
  tft.drawFastVLine(b.x, b.y + BEVEL, b.h - 2 * BEVEL, c);
  tft.drawFastVLine(x2, b.y + BEVEL, b.h - 2 * BEVEL, c);
  tft.drawLine(b.x, b.y + BEVEL, b.x + BEVEL, b.y, c);
  tft.drawLine(x2 - BEVEL, b.y, x2, b.y + BEVEL, c);
  tft.drawLine(b.x, y2 - BEVEL, b.x + BEVEL, y2, c);
  tft.drawLine(x2 - BEVEL, y2, x2, y2 - BEVEL, c);
}

static void drawPanel(const Btn &b, uint16_t border, uint16_t fill = C_PANEL,
                      uint16_t around = C_BG) {
  const int16_t x2 = b.x + b.w - 1, y2 = b.y + b.h - 1;
  tft.fillRect(b.x, b.y, b.w, b.h, fill);
  // recorta las esquinas contra el fondo => octogono hextech
  tft.fillTriangle(b.x, b.y, b.x + BEVEL, b.y, b.x, b.y + BEVEL, around);
  tft.fillTriangle(x2 - BEVEL, b.y, x2, b.y, x2, b.y + BEVEL, around);
  tft.fillTriangle(b.x, y2 - BEVEL, b.x, y2, b.x + BEVEL, y2, around);
  tft.fillTriangle(x2, y2 - BEVEL, x2, y2, x2 - BEVEL, y2, around);
  panelOutline(b, border);
}

// El + y el - se dibujan, no se escriben: TFT_eSPI centra las fuentes GFX por
// la altura de la fuente y no la del glifo, y estos dos no tienen ni ascendente
// ni descendente, asi que quedaban unos 7 px por debajo del centro del boton.
static void drawSign(const Btn &b, bool plus, uint16_t c) {
  const int16_t cx = b.x + b.w / 2, cy = b.y + b.h / 2;
  const int16_t len = 26, th = 6;
  tft.fillRect(cx - len / 2, cy - th / 2, len, th, c);
  if (plus) tft.fillRect(cx - th / 2, cy - len / 2, th, len, c);
}

static void drawBtn(const Btn &b, uint16_t border, uint16_t textColor, const GFXfont *f,
                    uint16_t fill = C_PANEL, uint16_t around = C_BG) {
  drawPanel(b, border, fill, around);
  tft.setFreeFont(f);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(textColor, fill);
  tft.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2);
}

// ----------------------------------------------------------------- bateria
// Curva de descarga de una LiPo 1S. Un mapeo lineal 3.3-4.2V mentiria: la
// celda pasa la mayor parte de su vida entre 3.7 y 3.9V.
static const struct { uint16_t mv; uint8_t pct; } BAT_CURVE[] = {
    {4200, 100}, {4100, 92}, {4000, 84}, {3900, 74}, {3800, 60}, {3750, 50},
    {3700, 40},  {3650, 30}, {3600, 20}, {3500, 10}, {3400, 5},  {3300, 0},
};

static uint8_t batPercent(uint16_t mv) {
  if (mv >= BAT_CURVE[0].mv) return 100;
  const size_t n = sizeof(BAT_CURVE) / sizeof(BAT_CURVE[0]);
  for (size_t i = 1; i < n; i++) {
    if (mv >= BAT_CURVE[i].mv) {
      const uint16_t lo = BAT_CURVE[i].mv, hi = BAT_CURVE[i - 1].mv;
      const uint8_t lp = BAT_CURVE[i].pct, hp = BAT_CURVE[i - 1].pct;
      return lp + (uint32_t)(mv - lo) * (hp - lp) / (hi - lo);
    }
  }
  return 0;
}

static uint16_t batMillivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(BAT_ADC);
  return (uint16_t)(sum / 8 * BAT_DIVIDER);
}

static int16_t batPct = -1;  // -1 = sin bateria, alimentado por USB

// Siempre en la misma esquina, asi vale para todas las pantallas.
static void drawBattery() {
  const int16_t w = 26, h = 13, x = 312 - w, cy = 15, y = cy - h / 2;
  tft.fillRect(x - 44, y - 2, 44 + w + 4, h + 4, C_BG);
  tft.drawRect(x, y, w, h, C_GOLD_DIM);
  tft.fillRect(x + w, y + 4, 2, h - 8, C_GOLD_DIM);  // borne

  tft.setTextFont(2);
  tft.setTextDatum(MR_DATUM);
  if (batPct < 0) {
    tft.setTextColor(C_MUTED, C_BG);
    tft.drawString("USB", x - 6, cy);
    return;
  }
  tft.fillRect(x + 2, y + 2, (w - 4) * batPct / 100, h - 4,
               batPct <= 20 ? C_FURY : C_GOLD);
  char buf[8];
  snprintf(buf, sizeof buf, "%d%%", batPct);
  tft.setTextColor(batPct <= 20 ? C_FURY : C_MUTED, C_BG);
  tft.drawString(buf, x - 6, cy);
}

// --------------------------------------------------------------- pantallas
enum Screen : uint8_t {
  SCREEN_MENU, SCREEN_FORMAT, SCREEN_GAME, SCREEN_WIN,
  SCREEN_DICE,     // tirada de d20
  SCREEN_UPDATE,   // estado de red y busqueda de actualizacion
  SCREEN_WIFI,     // lista de redes
  SCREEN_KEYB,     // teclado para la clave
  SCREEN_ASK_XP,   // preguntar si la partida lleva XP
  SCREEN_ASK_OFF,  // confirmar apagado
  SCREEN_ASK_WIN,  // confirmar victoria
};
enum Fmt : uint8_t { FMT_BO1, FMT_BO3 };

static Screen screen = SCREEN_MENU;
static Fmt fmt = FMT_BO1;
static bool dirty = true;  // repinta solo al cambiar de estado

static uint8_t score[2] = {0, 0};
static uint8_t xp[2] = {0, 0};
static bool useXp = false;  // se pregunta al elegir formato
static uint8_t dice = 0;

// XP: toque corto suma, pulsacion larga resta. Cual de las dos es no se sabe
// hasta que pasa el tiempo o se suelta, asi que no se puede resolver al pulsar.
#define XP_LONG_MS 600
static int8_t xpHeld = -1;    // caja XP bajo el dedo, -1 = ninguna
static uint32_t xpSince = 0;
static bool xpDone = false;   // la pulsacion larga ya resto
static uint8_t rounds[2] = {0, 0};
static uint8_t winner = 0;

static uint8_t roundsToWin() { return fmt == FMT_BO3 ? 2 : 1; }

static uint32_t gameStart = 0;
static uint32_t lastTouch = 0;  // para el apagado por inactividad

static uint32_t matchSeconds() {
  return (fmt == FMT_BO3 ? MATCH_MIN_BO3 : MATCH_MIN_BO1) * 60UL;
}

static uint32_t remainingSeconds() {
  const uint32_t elapsed = (millis() - gameStart) / 1000;
  const uint32_t total = matchSeconds();
  return elapsed >= total ? 0 : total - elapsed;
}

static const char *PLAYER[2] = {"YO", "RIVAL"};

// Los botones chicos pegados a un borde fallaban: el panel no siempre reporta
// los extremos del rango. Van con area de toque generosa y algo hacia dentro.
static const Btn BTN_NEW   = {50, 88, 220, 44, "NUEVO JUEGO"};
static const Btn BTN_UPD   = {50, 138, 220, 44, "ACTUALIZACIONES"};
static const Btn BTN_OFF   = {90, 188, 140, 44, "APAGAR"};
static const Btn BTN_BACK  = {0, 0, 100, 46, "< VOLVER"};
static const Btn BTN_BO1   = {24, 76, 132, 124, "BO1"};
static const Btn BTN_BO3   = {164, 76, 132, 124, "BO3"};
static const Btn BTN_MENU  = {0, 0, 92, 42, "< MENU"};

// Centro de cada mitad: 0..159 y 160..319.
static const int16_t HALF_CX[2] = {80, 240};

// El par [-][+] mide 140 y va centrado en su mitad: 80-70 y 240-70.
static const Btn BTN_MINUS[2] = {{10, 118, 64, 52, "-"}, {170, 118, 64, 52, "-"}};
static const Btn BTN_PLUS[2]  = {{86, 118, 64, 52, "+"}, {246, 118, 64, 52, "+"}};

// XP hacia fuera: bajo el "-" de YO y bajo el "+" del RIVAL.
static const Btn BTN_XP[2] = {{6, 176, 76, 46, "XP"}, {238, 176, 76, 46, "XP"}};
static const Btn BTN_AGAIN = {30, 164, 120, 44, "REVANCHA"};
static const Btn BTN_HOME  = {170, 164, 120, 44, "MENU"};

static const Btn MODAL   = {36, 52, 248, 136, ""};
static const Btn BTN_YES = {52, 128, 104, 44, ""};
static const Btn BTN_NO  = {164, 128, 104, 44, ""};

static void drawMenu() {
  tft.fillScreen(C_BG);
  drawRule(30);

  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold18pt7b);
  tft.setTextColor(C_GOLD, C_BG);
  tft.drawString("RIFTBOUND", 160, 56);
  drawRule(78);

  drawBtn(BTN_NEW, C_GOLD, C_TEXT, &FreeSansBold12pt7b);
  drawBtn(BTN_UPD, C_GOLD_DIM, C_MUTED, &FreeSansBold9pt7b);
  drawBtn(BTN_OFF, C_GOLD_DIM, C_TEXT, &FreeSansBold9pt7b);

  tft.setTextFont(2);
  tft.setTextDatum(BL_DATUM);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(VERSION, 8, 238);
  drawBattery();
}

static void drawCard(const Btn &b, const char *sub) {
  drawPanel(b, C_GOLD);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold18pt7b);
  tft.setTextColor(C_GOLD, C_PANEL);
  tft.drawString(b.label, b.x + b.w / 2, b.y + 46);
  diamond(b.x + b.w / 2, b.y + 74, 4, C_GOLD_DIM);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(C_TEAL, C_PANEL);
  tft.drawString(sub, b.x + b.w / 2, b.y + 98);
}

static void drawFormat() {
  tft.fillScreen(C_BG);

  tft.setTextFont(2);  // fuente chica: el volver no debe competir con el titulo
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_GOLD_DIM, C_BG);
  tft.drawString(BTN_BACK.label, BTN_BACK.x + 8, BTN_BACK.y + BTN_BACK.h / 2);

  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString("FORMATO", 160, 32);
  drawRule(54);

  drawCard(BTN_BO1, "1 PARTIDA");
  drawCard(BTN_BO3, "MEJOR DE 3");
  drawBattery();
}

// Repinta solo el numero: un fillScreen por cada punto parpadearia.
static void drawScore(uint8_t i) {
  const int16_t cx = HALF_CX[i];
  tft.fillRect(cx - 44, 66, 88, 48, C_BG);
  tft.setFreeFont(&FreeSansBold24pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_GOLD, C_BG);
  char b[4];
  snprintf(b, sizeof b, "%u", score[i]);
  tft.drawString(b, cx, 90);
}

static void drawXp(uint8_t i) {
  const Btn &b = BTN_XP[i];
  const int16_t cx = b.x + b.w / 2;
  tft.fillRect(b.x + 6, b.y + 20, b.w - 12, 22, C_PANEL);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_GOLD, C_PANEL);
  char t[4];
  snprintf(t, sizeof t, "%u", xp[i]);
  tft.drawString(t, cx, b.y + 31);
}

// Repinta solo el reloj: se actualiza cada segundo y no puede costar una pantalla.
static void drawClock() {
  const uint32_t r = remainingSeconds();
  char t[8];
  snprintf(t, sizeof t, "%02u:%02u", (unsigned)(r / 60), (unsigned)(r % 60));

  tft.fillRect(110, 185, 100, 28, C_BG);
  tft.setTextFont(4);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(r <= CLOCK_WARN_S ? C_FURY : C_TEAL, C_BG);
  tft.drawString(t, 160, 199);
}

static void drawGame() {
  tft.fillScreen(C_BG);

  tft.setTextFont(2);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_GOLD_DIM, C_BG);
  tft.drawString(BTN_MENU.label, BTN_MENU.x + 8, BTN_MENU.y + BTN_MENU.h / 2);

  char bar[24];
  if (fmt == FMT_BO3) snprintf(bar, sizeof bar, "BO3   RONDAS %u-%u", rounds[0], rounds[1]);
  else                snprintf(bar, sizeof bar, "BO1");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TEAL, C_BG);
  tft.drawString(bar, 160, 16);

  drawRule(36);
  tft.drawFastVLine(160, 44, 126, C_GOLD_DIM);

  for (uint8_t i = 0; i < 2; i++) {
    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_TEAL, C_BG);
    tft.drawString(PLAYER[i], HALF_CX[i], 52);
    drawScore(i);
    drawPanel(BTN_MINUS[i], C_GOLD_DIM);
    drawSign(BTN_MINUS[i], false, C_TEXT);
    drawPanel(BTN_PLUS[i], C_GOLD);
    drawSign(BTN_PLUS[i], true, C_TEXT);

    if (!useXp) continue;
    drawPanel(BTN_XP[i], C_GOLD_DIM);
    tft.setTextFont(2);
    tft.setTextColor(C_TEAL, C_PANEL);
    tft.drawString("XP", BTN_XP[i].x + BTN_XP[i].w / 2, BTN_XP[i].y + 12);
    drawXp(i);
  }

  drawClock();
  drawBattery();
}

static void drawWin() {
  tft.fillScreen(C_BG);
  drawRule(40);

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_TEAL, C_BG);
  tft.drawString("GANADOR", 160, 66);

  tft.setFreeFont(&FreeSansBold24pt7b);
  tft.setTextColor(C_GOLD, C_BG);
  tft.drawString(PLAYER[winner], 160, 102);

  if (fmt == FMT_BO3) {
    tft.setTextFont(2);
    tft.setTextColor(C_MUTED, C_BG);
    char r[20];
    snprintf(r, sizeof r, "RONDAS %u-%u", rounds[0], rounds[1]);
    tft.drawString(r, 160, 134);
  }

  drawBtn(BTN_AGAIN, C_GOLD, C_TEXT, &FreeSansBold12pt7b);
  drawBtn(BTN_HOME, C_GOLD_DIM, C_MUTED, &FreeSansBold12pt7b);
  drawBattery();
}

// Se pinta encima de la pantalla actual, que se dibuja antes sin borrarla.
static void drawModal(const char *title, const char *body, const char *yes, const char *no) {
  drawPanel(MODAL, C_GOLD, C_MODAL);

  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_GOLD, C_MODAL);
  tft.drawString(title, 160, 80);
  diamond(160, 102, 4, C_GOLD_DIM);

  tft.setTextFont(2);
  tft.setTextColor(C_TEXT, C_MODAL);
  tft.drawString(body, 160, 116);

  Btn y = BTN_YES, n = BTN_NO;
  y.label = yes;
  n.label = no;
  drawBtn(y, C_GOLD, C_TEXT, &FreeSansBold9pt7b, C_PANEL, C_MODAL);
  drawBtn(n, C_GOLD_DIM, C_MUTED, &FreeSansBold9pt7b, C_PANEL, C_MODAL);
}

// ============================== red y OTA ==============================
static Preferences prefs;
static char wifiSsid[33] = "";
static char wifiPass[65] = "";
static char otaMsg[42] = "";
static uint8_t otaPct = 0;

static uint8_t netCount = 0;  // resultado del ultimo escaneo
static uint8_t netPage = 0;

static const Btn BTN_UPD_BACK = {0, 0, 100, 46, "< VOLVER"};
static const Btn BTN_WIFI     = {40, 92, 240, 44, "ELEGIR RED"};
static const Btn BTN_CHECK    = {40, 146, 240, 44, "BUSCAR ACTUALIZACION"};

#define NET_PER_PAGE 5
static const Btn BTN_NET[NET_PER_PAGE] = {
    {10, 44, 300, 26, ""},  {10, 74, 300, 26, ""},  {10, 104, 300, 26, ""},
    {10, 134, 300, 26, ""}, {10, 164, 300, 26, ""},
};
static const Btn BTN_NET_BACK = {6, 198, 88, 34, "VOLVER"};
static const Btn BTN_NET_MORE = {100, 198, 66, 34, "MAS"};
static const Btn BTN_NET_OFF  = {172, 198, 142, 34, "DESCONECTAR"};

// Un mensaje de una linea centrado, para operaciones que bloquean.
static void status(const char *msg) {
  tft.fillRect(0, 96, 320, 40, C_BG);
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(msg, 160, 116);
}

// ------------------------------------------------------------ pantalla OTA
static void drawUpdate() {
  tft.fillScreen(C_BG);

  tft.setTextFont(2);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_GOLD_DIM, C_BG);
  tft.drawString(BTN_UPD_BACK.label, BTN_UPD_BACK.x + 8, BTN_UPD_BACK.y + BTN_UPD_BACK.h / 2);

  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString("ACTUALIZAR", 160, 32);
  drawRule(54);

  const bool up = WiFi.status() == WL_CONNECTED;
  const bool saved = wifiSsid[0] != 0;
  char line[48];
  if (up)         snprintf(line, sizeof line, "CONECTADO: %s", wifiSsid);
  else if (saved) snprintf(line, sizeof line, "GUARDADA: %s", wifiSsid);
  else            snprintf(line, sizeof line, "SIN RED");
  tft.setTextFont(2);
  tft.setTextColor(up ? C_TEAL : C_MUTED, C_BG);
  tft.drawString(line, 160, 72);

  drawBtn(BTN_WIFI, C_GOLD_DIM, C_TEXT, &FreeSansBold9pt7b);
  // Basta con tener red guardada: BUSCAR enciende la radio si hace falta.
  drawBtn(BTN_CHECK, saved ? C_GOLD : C_GOLD_DIM, saved ? C_TEXT : C_MUTED,
          &FreeSansBold9pt7b);

  tft.setTextFont(2);
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(otaMsg[0] ? otaMsg : VERSION, 160, 236);
  drawBattery();
}

static void drawOtaProgress() {
  tft.drawRect(40, 200, 240, 16, C_GOLD_DIM);
  tft.fillRect(42, 202, 236 * otaPct / 100, 12, C_GOLD);
}

// -------------------------------------------------------- lista de redes
static void drawWifiList() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString("REDES", 160, 22);

  const uint8_t first = netPage * NET_PER_PAGE;
  for (uint8_t i = 0; i < NET_PER_PAGE; i++) {
    const uint8_t n = first + i;
    if (n >= netCount) break;
    drawPanel(BTN_NET[i], C_GOLD_DIM);
    tft.setTextFont(2);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_TEXT, C_PANEL);
    tft.drawString(WiFi.SSID(n).substring(0, 24), BTN_NET[i].x + 12, BTN_NET[i].y + 13);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(WiFi.RSSI(n) > -70 ? C_TEAL : C_MUTED, C_PANEL);
    char db[8];
    snprintf(db, sizeof db, "%d", WiFi.RSSI(n));
    tft.drawString(db, BTN_NET[i].x + BTN_NET[i].w - 12, BTN_NET[i].y + 13);
  }
  if (!netCount) {
    tft.setTextFont(2);
    tft.setTextColor(C_MUTED, C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("NO SE VE NINGUNA RED", 160, 110);
  }

  drawBtn(BTN_NET_BACK, C_GOLD_DIM, C_TEXT, &FreeSansBold9pt7b);
  if (netCount > NET_PER_PAGE) drawBtn(BTN_NET_MORE, C_GOLD, C_TEXT, &FreeSansBold9pt7b);

  const bool up = WiFi.status() == WL_CONNECTED;
  drawBtn(BTN_NET_OFF, up ? C_FURY : C_GOLD_DIM, up ? C_TEXT : C_MUTED, &FreeSansBold9pt7b);
}

static void scanNets() {
  tft.fillScreen(C_BG);
  status("BUSCANDO REDES...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  const int n = WiFi.scanNetworks();
  netCount = n < 0 ? 0 : (n > 40 ? 40 : n);
  netPage = 0;
}

// ---------------------------------------------------------------- teclado
#define KB_ROWS 4
#define KB_Y0   48
#define KB_H    28
#define KB_GAP  3

static const char *KB_LAYER[3][KB_ROWS] = {
    {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
    {"1234567890", "!@#$%^&*()", "-_=+[]{}|\\", ";:'\",.<>/?"},
};
static uint8_t kbLayer = 0;

static const Btn KB_MOD[4] = {
    {4, 172, 60, 28, "ABC"},
    {68, 172, 60, 28, "#+="},
    {132, 172, 118, 28, "ESPACIO"},
    {254, 172, 62, 28, "BORRAR"},
};
static const Btn KB_CANCEL = {4, 204, 150, 32, "CANCELAR"};
static const Btn KB_OK     = {166, 204, 150, 32, "CONECTAR"};

// Geometria de una fila: ancho de tecla y borde izquierdo, para que dibujar y
// acertar usen exactamente el mismo calculo.
static void kbRow(uint8_t r, int16_t &x0, int16_t &w, uint8_t &n) {
  n = strlen(KB_LAYER[kbLayer][r]);
  w = (312 - (n - 1) * KB_GAP) / n;
  x0 = (320 - (w * n + (n - 1) * KB_GAP)) / 2;
}

static char kbKeyAt(int16_t px, int16_t py) {
  for (uint8_t r = 0; r < KB_ROWS; r++) {
    const int16_t y = KB_Y0 + r * (KB_H + KB_GAP);
    if (py < y || py >= y + KB_H) continue;
    int16_t x0, w;
    uint8_t n;
    kbRow(r, x0, w, n);
    for (uint8_t i = 0; i < n; i++) {
      const int16_t x = x0 + i * (w + KB_GAP);
      if (px >= x && px < x + w) return KB_LAYER[kbLayer][r][i];
    }
  }
  return 0;
}

// La clave puede ser mas larga que la pantalla: se muestra la cola.
static void drawKbField() {
  tft.fillRect(6, 22, 308, 22, C_PANEL);
  tft.drawRect(6, 22, 308, 22, C_GOLD_DIM);
  tft.setTextFont(2);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_GOLD, C_PANEL);
  const size_t len = strlen(wifiPass);
  const char *tail = len > 36 ? wifiPass + len - 36 : wifiPass;
  char shown[40];
  snprintf(shown, sizeof shown, "%s_", tail);
  tft.drawString(shown, 12, 33);
}

static void drawKeyboard() {
  tft.fillScreen(C_BG);

  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TEAL, C_BG);
  tft.drawString(wifiSsid, 160, 10);
  drawKbField();

  for (uint8_t r = 0; r < KB_ROWS; r++) {
    int16_t x0, w;
    uint8_t n;
    kbRow(r, x0, w, n);
    const int16_t y = KB_Y0 + r * (KB_H + KB_GAP);
    for (uint8_t i = 0; i < n; i++) {
      const int16_t x = x0 + i * (w + KB_GAP);
      tft.fillRoundRect(x, y, w, KB_H, 3, C_PANEL);
      tft.drawRoundRect(x, y, w, KB_H, 3, C_GOLD_DIM);
      const char c[2] = {KB_LAYER[kbLayer][r][i], 0};
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(C_TEXT, C_PANEL);
      tft.drawString(c, x + w / 2, y + KB_H / 2);
    }
  }

  for (uint8_t i = 0; i < 4; i++) {
    const Btn &b = KB_MOD[i];
    tft.fillRoundRect(b.x, b.y, b.w, b.h, 3, C_PANEL);
    tft.drawRoundRect(b.x, b.y, b.w, b.h, 3, C_GOLD_DIM);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_TEXT, C_PANEL);
    tft.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2);
  }

  drawBtn(KB_CANCEL, C_GOLD_DIM, C_MUTED, &FreeSansBold9pt7b);
  drawBtn(KB_OK, C_GOLD, C_TEXT, &FreeSansBold9pt7b);
}

// ------------------------------------------------------------------ enlace
static bool wifiConnect() {
  tft.fillScreen(C_BG);
  status("CONECTANDO...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);

  const bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) {  // solo se guardan credenciales que funcionan
    prefs.putString("ssid", wifiSsid);
    prefs.putString("pass", wifiPass);
  }
  snprintf(otaMsg, sizeof otaMsg, ok ? "CONECTADO" : "NO SE PUDO CONECTAR");
  return ok;
}

// --------------------------------------------------------------------- OTA
// Extrae un campo del manifiesto. Es un JSON de dos claves; montar un parser
// entero o traerse una libreria para eso no se paga.
static bool jsonField(const String &src, const char *key, char *out, size_t cap) {
  int i = src.indexOf(key);
  if (i < 0) return false;
  i = src.indexOf(':', i + strlen(key));
  if (i < 0) return false;
  i++;
  while (i < (int)src.length() && (src[i] == ' ' || src[i] == '"')) i++;
  size_t k = 0;
  while (i < (int)src.length() && src[i] != '"' && src[i] != ',' && src[i] != '}' &&
         k + 1 < cap)
    out[k++] = src[i++];
  out[k] = 0;
  return k > 0;
}

static void otaRun() {
  tft.fillScreen(C_BG);
  status("BUSCANDO ACTUALIZACION...");

  // Sin validar el certificado: fijar una CA aqui dentro condena al aparato el
  // dia que caduque. Va cifrado, pero no protege de un intermediario activo.
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // GitHub salta a otro host
  http.setTimeout(15000);
  if (!http.begin(client, MANIFEST_URL) || http.GET() != HTTP_CODE_OK) {
    snprintf(otaMsg, sizeof otaMsg, "NO SE PUDO LEER EL MANIFIESTO");
    http.end();
    return;
  }
  const String body = http.getString();
  http.end();

  char ver[12] = "", url[192] = "";
  if (!jsonField(body, "version", ver, sizeof ver) ||
      !jsonField(body, "url", url, sizeof url)) {
    snprintf(otaMsg, sizeof otaMsg, "MANIFIESTO INVALIDO");
    return;
  }
  if (atoi(ver) <= FW_VERSION) {
    snprintf(otaMsg, sizeof otaMsg, "YA ESTAS AL DIA (%s)", VERSION);
    return;
  }

  status("DESCARGANDO...");
  HTTPClient dl;
  dl.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  dl.setTimeout(20000);
  if (!dl.begin(client, url) || dl.GET() != HTTP_CODE_OK) {
    snprintf(otaMsg, sizeof otaMsg, "NO SE PUDO DESCARGAR");
    dl.end();
    return;
  }
  const int len = dl.getSize();
  if (len <= 0 || !Update.begin(len)) {
    snprintf(otaMsg, sizeof otaMsg, "NO CABE EN LA PARTICION");
    dl.end();
    return;
  }

  otaPct = 0;
  drawOtaProgress();
  Update.onProgress([](size_t done, size_t total) {
    const uint8_t p = total ? (uint8_t)(done * 100 / total) : 0;
    if (p == otaPct) return;
    otaPct = p;
    drawOtaProgress();
  });

  const size_t written = Update.writeStream(*dl.getStreamPtr());
  dl.end();
  if (written != (size_t)len || !Update.end(true)) {
    snprintf(otaMsg, sizeof otaMsg, "ACTUALIZACION CORRUPTA");
    Update.abort();
    return;
  }
  status("LISTO, REINICIANDO");
  delay(1200);
  ESP.restart();
}

static uint8_t rollD20() { return 1 + (uint8_t)(esp_random() % 20); }

// dice == 0 es "aun sin tirar".
static void drawDiceValue() {
  char t[4];
  if (dice) snprintf(t, sizeof t, "%u", dice);
  else      snprintf(t, sizeof t, "?");

  tft.fillRect(110, 84, 100, 44, C_MODAL);
  tft.setFreeFont(&FreeSansBold24pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(!dice ? C_GOLD_DIM : dice == 20 ? C_TEAL : dice == 1 ? C_FURY : C_GOLD,
                   C_MODAL);
  tft.drawString(t, 160, 106);
}

// La tirada se ve rodar. No es solo adorno: si el numero cambiara de golpe no
// se distinguiria una tirada nueva de la anterior cuando sale el mismo valor.
static void rollDice() {
  for (uint8_t i = 0; i < 8; i++) {
    dice = rollD20();
    drawDiceValue();
    delay(45);
  }
}

static void drawDice() {
  drawPanel(MODAL, C_GOLD, C_MODAL);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_TEAL, C_MODAL);
  tft.drawString("QUIEN EMPIEZA", 160, 70);

  Btn y = BTN_YES, n = BTN_NO;
  y.label = "TIRAR";
  n.label = "EMPEZAR";
  drawBtn(y, C_GOLD, C_TEXT, &FreeSansBold9pt7b, C_PANEL, C_MODAL);
  drawBtn(n, C_GOLD_DIM, C_TEXT, &FreeSansBold9pt7b, C_PANEL, C_MODAL);
  drawDiceValue();  // no tira solo: hay que pulsar TIRAR
}

static void draw() {
  switch (screen) {
    case SCREEN_MENU:   drawMenu();   break;
    case SCREEN_FORMAT: drawFormat(); break;
    case SCREEN_GAME:   drawGame();   break;
    case SCREEN_WIN:    drawWin();    break;
    case SCREEN_DICE:
      drawGame();
      drawDice();
      break;
    case SCREEN_UPDATE: drawUpdate();   break;
    case SCREEN_WIFI:   drawWifiList(); break;
    case SCREEN_KEYB:   drawKeyboard(); break;
    case SCREEN_ASK_XP:
      drawFormat();
      drawModal("XP", "NECESITAS CONTADOR?", "SI", "NO");
      break;
    case SCREEN_ASK_OFF:
      drawMenu();
      drawModal("APAGAR", "LA PANTALLA SE APAGARA", "APAGAR", "CANCELAR");
      break;
    case SCREEN_ASK_WIN:
      drawGame();
      drawModal("VICTORIA", PLAYER[winner], "CONFIRMAR", "CORREGIR");
      break;
  }
}

// ------------------------------------------------------------------- touch
// El FT6336 entrega coordenadas en la orientacion nativa del panel (240x320
// vertical); aqui se llevan a la pantalla ya rotada.
static void mapTouch(int16_t tx, int16_t ty, int16_t &x, int16_t &y) {
  tx = map(tx, TP_RAW_X_MIN, TP_RAW_X_MAX, 0, TFT_WIDTH - 1);
  ty = map(ty, TP_RAW_Y_MIN, TP_RAW_Y_MAX, 0, TFT_HEIGHT - 1);
#if TOUCH_MAP == 0
  x = ty;                  y = tx;
#elif TOUCH_MAP == 1
  x = ty;                  y = TFT_WIDTH - 1 - tx;
#elif TOUCH_MAP == 2
  x = TFT_HEIGHT - 1 - ty; y = tx;
#else
  x = TFT_HEIGHT - 1 - ty; y = TFT_WIDTH - 1 - tx;
#endif
  x = constrain(x, 0, tft.width() - 1);
  y = constrain(y, 0, tft.height() - 1);
}

static int16_t rawX = 0, rawY = 0;  // ultimo crudo, para TOUCH_DEBUG

// Devuelve true si hay un dedo encima, con la posicion ya en pixeles.
static bool tpRead(int16_t &x, int16_t &y) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x02);  // TD_STATUS, y a continuacion P1_XH..P1_YL
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)5) != 5) return false;

  const uint8_t n = Wire.read() & 0x0F;
  const uint8_t xh = Wire.read(), xl = Wire.read();
  const uint8_t yh = Wire.read(), yl = Wire.read();
  if (n == 0 || n > 2) return false;

  rawX = ((xh & 0x0F) << 8) | xl;
  rawY = ((yh & 0x0F) << 8) | yl;
  mapTouch(rawX, rawY, x, y);
  return true;
}

// Una sola pulsacion generaba varias: el FT6336 intercala lecturas invalidas en
// mitad de un toque y cada hueco se contaba como soltar y volver a pulsar. Un
// toque no se da por terminado hasta 4 lecturas vacias seguidas (~40 ms).
static bool touchDown(int16_t &x, int16_t &y) {
  static bool down = false;
  static uint8_t misses = 0;
  static int16_t lastX = 0, lastY = 0;

  if (tpRead(lastX, lastY)) {
    misses = 0;
    down = true;
  } else if (!down || ++misses >= 4) {
    down = false;
  }
  x = lastX;
  y = lastY;
  return down;
}

// ------------------------------------------------------------------ acciones
// Deja todo a cero y manda a tirar el dado, que es lo que decide quien empieza.
static void resetGame() {
  score[0] = score[1] = 0;
  rounds[0] = rounds[1] = 0;
  xp[0] = xp[1] = 0;
  dice = 0;              // sin tirar todavia
  gameStart = millis();  // el reloj arranca aqui, no al pulsar EMPEZAR
  screen = SCREEN_DICE;
  dirty = true;
}

static void startGame() {
  screen = SCREEN_GAME;  // el reloj ya viene corriendo: EMPEZAR no lo reinicia
  dirty = true;
}

static void pick(const Btn &b, Fmt f) {
  fmt = f;
  panelOutline(b, C_TEAL);
  delay(150);
  screen = SCREEN_ASK_XP;
  dirty = true;
}

static void addPoint(uint8_t i) {
  if (score[i] >= TARGET_POINTS) return;
  score[i]++;
  drawScore(i);
  if (score[i] < TARGET_POINTS) return;

  winner = i;
  screen = SCREEN_ASK_WIN;
  dirty = true;
}

// La victoria confirmada cierra la ronda; en BO3 puede quedar partida.
static void confirmWin() {
  rounds[winner]++;
  score[0] = score[1] = 0;
  screen = rounds[winner] >= roundsToWin() ? SCREEN_WIN : SCREEN_GAME;
  dirty = true;
}

// El apagado reinicia el chip, asi que la partida en curso se guarda en NVS y
// se recupera al arrancar. Sin esto, el apagado automatico te borraria el
// marcador cada vez que pasaran diez minutos sin tocar nada.
struct Session {
  uint8_t fmt, useXp, winner, score[2], rounds[2], xp[2];
  uint32_t remaining;
};

static void sessionSave(uint32_t remaining) {
  const Session v = {(uint8_t)fmt,   (uint8_t)useXp,       winner,
                     {score[0], score[1]}, {rounds[0], rounds[1]},
                     {xp[0], xp[1]},  remaining};
  prefs.putBytes("sess", &v, sizeof v);
}

static void sessionClear() { prefs.remove("sess"); }

static bool sessionLoad() {
  Session v;
  if (prefs.getBytes("sess", &v, sizeof v) != sizeof v) return false;
  fmt = (Fmt)v.fmt;
  useXp = v.useXp;
  winner = v.winner;
  score[0] = v.score[0];
  score[1] = v.score[1];
  rounds[0] = v.rounds[0];
  rounds[1] = v.rounds[1];
  xp[0] = v.xp[0];
  xp[1] = v.xp[1];

  const uint32_t total = matchSeconds();
  const uint32_t rem = v.remaining > total ? total : v.remaining;
  gameStart = millis() - (total - rem) * 1000UL;  // deja el reloj donde estaba
  return true;
}

// El ESP32 no se apaga de verdad. Sueno ligero, no profundo: el profundo no
// conserva el estado de los pines, asi que TP_RST se caia, el FT6336 se quedaba
// en reset y no despertaba al tocar. En ligero siguen vivos los pines y el I2C,
// asi que se le puede preguntar al tactil directamente y no dependemos del pin
// INT ni de su pull-up.
static void powerOff() {
  const bool inGame = screen == SCREEN_GAME || screen == SCREEN_DICE ||
                      screen == SCREEN_ASK_WIN;
  WiFi.mode(WIFI_OFF);
  digitalWrite(TFT_BL, LOW);
  tft.writecommand(0x10);  // ILI9341 sleep in

  // Sin esto el mismo dedo que confirmo el apagado cuenta ya como el toque que
  // la enciende, y la pantalla vuelve al instante.
  int16_t x, y;
  while (tpRead(x, y)) delay(10);
  delay(250);

  do {
    esp_sleep_enable_timer_wakeup(120000);  // 120 ms: un toque dura bastante mas
    esp_light_sleep_start();
  } while (!tpRead(x, y));

  while (tpRead(x, y)) delay(10);  // el dedo que enciende no cuenta como pulsacion

  // millis() sigue corriendo durante el sueno ligero, asi que el tiempo dormido
  // ya viene descontado: el reloj de partida no se para por apagarse.
  if (inGame) sessionSave(remainingSeconds());

  // El USB-CDC no sobrevive al sueno ligero y no vuelve solo: sin esto, una vez
  // apagada ya no se puede reflashear sin llegar al boton RESET, que dentro de
  // una carcasa no existe. Reiniciar lo devuelve, y arranca en el menu, que es
  // adonde iba igualmente.
  esp_restart();
}

static void handleTap(int16_t x, int16_t y) {
  switch (screen) {
    case SCREEN_MENU:
      if (hit(BTN_NEW, x, y)) {
        screen = SCREEN_FORMAT;
        dirty = true;
      } else if (hit(BTN_UPD, x, y)) {
        otaMsg[0] = 0;  // entrar no enciende la radio: solo BUSCAR la necesita
        screen = SCREEN_UPDATE;
        dirty = true;
      } else if (hit(BTN_OFF, x, y)) {
        screen = SCREEN_ASK_OFF;
        dirty = true;
      }
      break;

    case SCREEN_FORMAT:
      if (hit(BTN_BACK, x, y)) {
        screen = SCREEN_MENU;
        dirty = true;
      } else if (hit(BTN_BO1, x, y)) {
        pick(BTN_BO1, FMT_BO1);
      } else if (hit(BTN_BO3, x, y)) {
        pick(BTN_BO3, FMT_BO3);
      }
      break;

    case SCREEN_GAME:
      if (hit(BTN_MENU, x, y)) {
        sessionClear();  // salir al menu abandona la partida
        screen = SCREEN_MENU;
        dirty = true;
        break;
      }
      for (uint8_t i = 0; i < 2; i++) {
        if (hit(BTN_PLUS[i], x, y)) {
          addPoint(i);
          break;
        }
        if (hit(BTN_MINUS[i], x, y)) {
          if (score[i]) {
            score[i]--;
            drawScore(i);
          }
          break;
        }
        if (useXp && hit(BTN_XP[i], x, y)) {
          xpHeld = i;  // se decide al soltar, en el bucle
          xpSince = millis();
          xpDone = false;
          break;
        }
      }
      break;

    case SCREEN_UPDATE:
      if (hit(BTN_UPD_BACK, x, y)) {
        WiFi.mode(WIFI_OFF);  // la radio encendida se come la bateria
        screen = SCREEN_MENU;
        dirty = true;
      } else if (hit(BTN_WIFI, x, y)) {
        scanNets();
        screen = SCREEN_WIFI;
        dirty = true;
      } else if (hit(BTN_CHECK, x, y) && wifiSsid[0]) {
        if (WiFi.status() != WL_CONNECTED) wifiConnect();
        if (WiFi.status() == WL_CONNECTED) otaRun();
        dirty = true;
      }
      break;

    case SCREEN_WIFI:
      if (hit(BTN_NET_BACK, x, y)) {
        screen = SCREEN_UPDATE;
        dirty = true;
        break;
      }
      if (hit(BTN_NET_OFF, x, y)) {
        // Corta y apaga la radio. Las credenciales se quedan guardadas: esto es
        // desconectar, no olvidar la red.
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        netCount = 0;  // el escaneo cacheado ya no vale sin radio
        snprintf(otaMsg, sizeof otaMsg, "WIFI APAGADO");
        screen = SCREEN_UPDATE;
        dirty = true;
        break;
      }
      if (netCount > NET_PER_PAGE && hit(BTN_NET_MORE, x, y)) {
        netPage = (netPage + 1) * NET_PER_PAGE < netCount ? netPage + 1 : 0;
        dirty = true;
        break;
      }
      for (uint8_t i = 0; i < NET_PER_PAGE; i++) {
        if (netPage * NET_PER_PAGE + i >= netCount) break;
        if (!hit(BTN_NET[i], x, y)) continue;
        strncpy(wifiSsid, WiFi.SSID(netPage * NET_PER_PAGE + i).c_str(), sizeof wifiSsid - 1);
        wifiSsid[sizeof wifiSsid - 1] = 0;
        wifiPass[0] = 0;
        kbLayer = 0;
        screen = SCREEN_KEYB;
        dirty = true;
        break;
      }
      break;

    case SCREEN_KEYB: {
      if (hit(KB_CANCEL, x, y)) {
        screen = SCREEN_WIFI;
        dirty = true;
        break;
      }
      if (hit(KB_OK, x, y)) {
        wifiConnect();
        screen = SCREEN_UPDATE;
        dirty = true;
        break;
      }
      if (hit(KB_MOD[0], x, y)) {  // may/min
        kbLayer = kbLayer == 0 ? 1 : 0;
        dirty = true;
        break;
      }
      if (hit(KB_MOD[1], x, y)) {  // simbolos
        kbLayer = kbLayer == 2 ? 0 : 2;
        dirty = true;
        break;
      }
      const size_t len = strlen(wifiPass);
      if (hit(KB_MOD[2], x, y)) {  // espacio
        if (len + 1 < sizeof wifiPass) {
          wifiPass[len] = ' ';
          wifiPass[len + 1] = 0;
          drawKbField();
        }
        break;
      }
      if (hit(KB_MOD[3], x, y)) {  // borrar
        if (len) wifiPass[len - 1] = 0;
        drawKbField();
        break;
      }
      const char c = kbKeyAt(x, y);
      if (c && len + 1 < sizeof wifiPass) {
        wifiPass[len] = c;
        wifiPass[len + 1] = 0;
        drawKbField();
      }
      break;
    }

    case SCREEN_DICE:
      if (hit(BTN_YES, x, y)) {
        rollDice();  // repinta solo el numero
      } else if (hit(BTN_NO, x, y)) {
        startGame();
      }
      break;

    case SCREEN_ASK_XP:
      if (hit(BTN_YES, x, y) || hit(BTN_NO, x, y)) {
        useXp = hit(BTN_YES, x, y);
        resetGame();
      }
      break;

    case SCREEN_ASK_OFF:
      if (hit(BTN_YES, x, y)) {
        screen = SCREEN_MENU;  // powerOff repinta al despertar
        powerOff();
      } else if (hit(BTN_NO, x, y)) {
        screen = SCREEN_MENU;
        dirty = true;
      }
      break;

    case SCREEN_ASK_WIN:
      if (hit(BTN_YES, x, y)) {
        confirmWin();
      } else if (hit(BTN_NO, x, y)) {
        score[winner]--;  // era un error de cuenta: vuelve a 7
        screen = SCREEN_GAME;
        dirty = true;
      }
      break;

    case SCREEN_WIN:
      if (hit(BTN_AGAIN, x, y)) {
        resetGame();  // la revancha tambien decide quien empieza
      } else if (hit(BTN_HOME, x, y)) {
        sessionClear();
        screen = SCREEN_MENU;
        dirty = true;
      }
      break;
  }
}

// -------------------------------------------------------------- self check
#define CHECK(c)                            \
  do {                                      \
    if (!(c)) {                             \
      Serial.println("selfTest FAIL: " #c); \
      ok = false;                           \
    }                                       \
  } while (0)

static bool selfTestOk = true;

static void selfTest() {
  bool ok = true;

  const Btn b = {50, 116, 220, 46, "X"};
  CHECK(hit(b, 60, 126));    // dentro
  CHECK(!hit(b, 49, 126));   // 1 px a la izquierda
  CHECK(!hit(b, 60, 162));   // el borde inferior es exclusivo
  CHECK(!hit(b, 270, 126));  // el borde derecho es exclusivo

  int16_t x, y, x2, y2;
  mapTouch(0, 0, x, y);
  CHECK(x >= 0 && x < tft.width() && y >= 0 && y < tft.height());
  mapTouch(TFT_WIDTH - 1, TFT_HEIGHT - 1, x2, y2);
  CHECK(x2 >= 0 && x2 < tft.width() && y2 >= 0 && y2 < tft.height());
  CHECK(x != x2 && y != y2);  // esquinas opuestas, no colapsadas

  // Ningun boton de una misma pantalla puede solaparse con otro.
  CHECK(!hit(BTN_PLUS[0], BTN_MINUS[0].x, BTN_MINUS[0].y));
  CHECK(!hit(BTN_MINUS[1], BTN_PLUS[0].x + BTN_PLUS[0].w - 1, BTN_PLUS[0].y));
  CHECK(!hit(BTN_XP[0], BTN_MINUS[0].x, BTN_MINUS[0].y + BTN_MINUS[0].h - 1));
  CHECK(!hit(BTN_XP[1], BTN_XP[0].x + BTN_XP[0].w - 1, BTN_XP[0].y));

  uint32_t seen = 0;  // el d20 cae siempre en 1..20 y no se queda clavado
  for (int i = 0; i < 400; i++) {
    const uint8_t d = rollD20();
    if (d < 1 || d > 20) { Serial.println("selfTest FAIL: d20 fuera de rango"); ok = false; break; }
    seen |= 1UL << d;
  }
  CHECK(__builtin_popcountl(seen) >= 15);
  // cada par [-][+] centrado en su mitad
  for (uint8_t i = 0; i < 2; i++)
    CHECK((BTN_MINUS[i].x + BTN_PLUS[i].x + BTN_PLUS[i].w) / 2 == HALF_CX[i]);
  CHECK(BTN_OFF.y >= BTN_UPD.y + BTN_UPD.h);   // los del menu no se pisan
  CHECK(!hit(BTN_YES, BTN_NO.x, BTN_NO.y));    // ni los del modal
  CHECK(hit(MODAL, BTN_YES.x, BTN_YES.y) && hit(MODAL, BTN_NO.x + BTN_NO.w - 1,
                                                BTN_NO.y + BTN_NO.h - 1));

  CHECK(!hit(BTN_NET_OFF, BTN_NET_MORE.x + BTN_NET_MORE.w - 1, BTN_NET_MORE.y));
  CHECK(!hit(KB_OK, KB_CANCEL.x, KB_CANCEL.y));   // teclado sin solapes
  CHECK(!hit(KB_MOD[3], KB_MOD[2].x, KB_MOD[2].y));
  CHECK(kbKeyAt(160, 0) == 0);                    // fuera del teclado, nada
  CHECK(kbKeyAt(KB_Y0, KB_Y0 + 4) != 0);          // dentro, alguna tecla

  CHECK(batPercent(4200) == 100);
  CHECK(batPercent(4300) == 100);  // por encima del tope, no se desborda
  CHECK(batPercent(3300) == 0);
  CHECK(batPercent(3000) == 0);  // por debajo del suelo
  const Fmt saved = fmt;
  fmt = FMT_BO1;
  CHECK(matchSeconds() == 30 * 60);
  fmt = FMT_BO3;
  CHECK(matchSeconds() == 60 * 60);

  // Recuperar la partida debe devolver el reloj al mismo punto.
  const uint32_t rem = 900;
  gameStart = millis() - (matchSeconds() - rem) * 1000UL;
  CHECK(remainingSeconds() == rem);
  fmt = saved;

  CHECK(batPercent(3750) == 50);
  CHECK(batPercent(3850) > 60 && batPercent(3850) < 74);  // interpola

  selfTestOk = ok;
  Serial.println(ok ? "selfTest OK" : "selfTest FAILED");
}

// ------------------------------------------------------------------- setup
void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();  // margen corto: encender no debe hacerse esperar
  while (!Serial && millis() - t0 < 500) delay(10);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);  // se enciende al final, con la primera pantalla ya pintada

  tft.init();
  tft.setRotation(SCREEN_ROTATION);

  pinMode(TP_INT, INPUT_PULLUP);
  pinMode(TP_RST, OUTPUT);  // el FT6336 arranca en reset: hay que soltarlo
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(150);
  Wire.begin(TP_SDA, TP_SCL, 400000);

  WiFi.mode(WIFI_OFF);  // solo se enciende en la pantalla de actualizar
  prefs.begin("rift", false);
  if (prefs.isKey("ssid")) {  // sin el guardia, NVS escupe un error por clave
    prefs.getString("ssid", "").toCharArray(wifiSsid, sizeof wifiSsid);
    prefs.getString("pass", "").toCharArray(wifiPass, sizeof wifiPass);
  }

  if (prefs.isKey("sess") && sessionLoad()) {
    screen = score[0] >= TARGET_POINTS || score[1] >= TARGET_POINTS ? SCREEN_ASK_WIN
                                                                   : SCREEN_GAME;
  }
  lastTouch = millis();

  const uint16_t mv = batMillivolts();
  batPct = mv < BAT_MIN_MV ? -1 : batPercent(mv);

  selfTest();
  Serial.printf("pantalla %dx%d, bateria %u mV (%d%%)\n", tft.width(), tft.height(),
                mv, batPct);

  draw();  // pinta antes de encender: nada de basura de VRAM al arrancar
  dirty = false;
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
}

static void refreshClock() {
  static uint32_t lastShown = UINT32_MAX;
  if (screen != SCREEN_GAME && screen != SCREEN_DICE && screen != SCREEN_ASK_WIN) return;
  const uint32_t r = remainingSeconds();
  if (r == lastShown) return;
  lastShown = r;
  drawClock();
}

static void refreshBattery() {
  static uint32_t last = 0;
  if (last && millis() - last < 30000) return;
  last = millis();

  WiFi.mode(WIFI_OFF);  // solo se enciende en la pantalla de actualizar
  prefs.begin("rift", false);
  if (prefs.isKey("ssid")) {  // sin el guardia, NVS escupe un error por clave
    prefs.getString("ssid", "").toCharArray(wifiSsid, sizeof wifiSsid);
    prefs.getString("pass", "").toCharArray(wifiPass, sizeof wifiPass);
  }

  const uint16_t mv = batMillivolts();
  const int16_t p = mv < BAT_MIN_MV ? -1 : batPercent(mv);
  Serial.printf("bateria %u mV -> %d%%\n", mv, p);  // para ajustar BAT_DIVIDER
  if (!selfTestOk) Serial.println("selfTest FAILED");  // que no se pierda en el arranque
  if (p == batPct) return;
  batPct = p;
  drawBattery();
}

void loop() {
  static bool wasTouched = false;
  int16_t x = 0, y = 0;
  const bool down = touchDown(x, y);

  if (down) lastTouch = millis();

  if (down && !wasTouched) {  // solo el flanco: un tap = una accion
#if TOUCH_DEBUG
    Serial.printf("tap crudo=%d,%d  px=%d,%d\n", rawX, rawY, x, y);
#endif
    handleTap(x, y);
  } else if (xpHeld >= 0 && screen == SCREEN_GAME) {
    if (!down) {  // solto antes de tiempo: era un toque corto
      if (!xpDone && xp[xpHeld] < 99) {
        xp[xpHeld]++;
        drawXp(xpHeld);
      }
      xpHeld = -1;
    } else if (!xpDone && millis() - xpSince >= XP_LONG_MS) {
      if (xp[xpHeld]) xp[xpHeld]--;
      drawXp(xpHeld);  // resta en cuanto se cumple, para que se note al vuelo
      xpDone = true;
    }
  } else if (xpHeld >= 0) {
    xpHeld = -1;  // cambio de pantalla con el dedo puesto
  }
  wasTouched = down;

  if (dirty) {
    draw();
    dirty = false;
  }
  refreshClock();
  refreshBattery();
  if (millis() - lastTouch > IDLE_OFF_MIN * 60000UL) powerOff();
  delay(10);
}
