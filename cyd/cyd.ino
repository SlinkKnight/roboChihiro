#include <TFT_eSPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

/*
 * Controlador ESP32 (CYD) + Tela TFT com botoes touch
 *
 * Eixos (botoes - / + no painel direito):
 *   Base    -> giro da base (vista de cima)
 *   Braco   -> inclinacao do braco (coluna)
 *   Cotov.  -> cotovelo
 *   Pulso   -> pulso
 *   Garra   -> abertura da garra
 */

// ===================== CONFIGURAÇÃO ESP-NOW =====================
#define ENVIAR_ESPNOW      true
#define ESPNOW_CHANNEL     1           // igual ao da ponte
#define PERIODO_ENVIO_MS   100         // 10 Hz

uint8_t MAC_PONTE[6] = {0x9C, 0x9E, 0x6E, 0xE2, 0xD2, 0xFC};

// Posicao de cada eixo na mensagem "P e1 e2 e3 e4 e5 g":
//   Braco = e1, Cotovelo = e2, Pulso = e3, Base = e5 (o eixo que sobrava).
#define SLOT_BASE          5

const float ESC_BRACO = 1.0f, ESC_COTOVELO = 1.0f, ESC_PULSO = 1.0f, ESC_BASE = 1.0f;
// ================================================================

// ===================== CONFIGURAÇÃO DO TOUCH (XPT2046) ==========
#define TOUCH_CS    33
#define TOUCH_IRQ   36
#define TOUCH_CLK   25
#define TOUCH_MOSI  32
#define TOUCH_MISO  39

// Faixa dos valores brutos do XPT2046 (ajuste aqui se o toque ficar deslocado)
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

// Velocidade de movimento por ciclo do loop enquanto o botao esta pressionado
const float VELOCIDADE = 1.0f;
// ================================================================

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

TFT_eSPI tft;
TFT_eSprite arm = TFT_eSprite(&tft);   // metade esquerda (160x240)

// geometria do desenho (pixels)
const int L0 = 36, L1 = 40, L2 = 28, LG = 12;
const int X0 = 80, Y0 = 202;

// ângulos (graus)
float giro = 90, braco = 90, cotovelo = 90, pulso = 90, garra = 90;

uint16_t LARANJA, AMARELO, CIANO, CINZA_BTN;

bool espnowOk = false;
volatile unsigned long ultimaResposta = 0;   // última resposta da ponte

// ---------- layout dos botoes (painel direito) ----------
const int BTN_W = 50, BTN_H = 34;
const int BTN_MINUS_X = 168;
const int BTN_PLUS_X  = 268;
const int LABEL_X     = 243;
const int ROW_Y0 = 22, ROW_STEP = 38;
const int N_EIXOS = 5;
const char* NOMES[N_EIXOS] = {"Base", "Braco", "Cotov.", "Pulso", "Garra"};

float* valorEixo(int i) {
  switch (i) {
    case 0: return &giro;
    case 1: return &braco;
    case 2: return &cotovelo;
    case 3: return &pulso;
    default: return &garra;
  }
}

// ---------- ESP-NOW ----------
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  ultimaResposta = millis();
}
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  ultimaResposta = millis();
}
#endif

bool iniciarEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm);   // menos corrente de pico
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MAC_PONTE, 6);
  peer.channel = 0;                    // canal atual
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

void enviarPosicao() {
  float e[5] = {0, 0, 0, 0, 0};
  e[0] = (braco    - 90) * ESC_BRACO;
  e[1] = (cotovelo - 90) * ESC_COTOVELO;
  e[2] = (pulso    - 90) * ESC_PULSO;
  e[SLOT_BASE - 1] = (giro - 90) * ESC_BASE;
  float g = constrain(garra, 0, 180);

  char msg[80];
  int n = snprintf(msg, sizeof(msg), "P %.1f %.1f %.1f %.1f %.1f %.0f\n",
                   e[0], e[1], e[2], e[3], e[4], g);
  esp_now_send(MAC_PONTE, (const uint8_t *)msg, n);
}

// ---------- TOUCH ----------
bool lerToque(int &x, int &y) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  x = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 320), 0, 319);
  y = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 240), 0, 239);
  return true;
}

void atualizarToque() {
  int x, y;
  if (!lerToque(x, y)) return;

  for (int i = 0; i < N_EIXOS; i++) {
    int by = ROW_Y0 + i * ROW_STEP;
    if (y < by || y > by + BTN_H) continue;

    float *v = valorEixo(i);
    if (x >= BTN_MINUS_X && x <= BTN_MINUS_X + BTN_W)
      *v = constrain(*v - VELOCIDADE, 0, 180);
    else if (x >= BTN_PLUS_X && x <= BTN_PLUS_X + BTN_W)
      *v = constrain(*v + VELOCIDADE, 0, 180);
    break;
  }
}

// ---------- desenho do braco ----------
void desenharBraco() {
  arm.fillSprite(TFT_BLACK);

  // mostrador da base (vista de cima): 90 = apontando para cima
  arm.drawCircle(32, 34, 22, TFT_DARKGREY);
  float b = radians(giro);
  arm.drawWideLine(32, 34, 32 + 18 * cosf(b), 34 - 18 * sinf(b), 3, TFT_GREEN, TFT_BLACK);
  char txt[24];
  snprintf(txt, sizeof(txt), "Base %d", (int)giro);
  arm.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  arm.drawString(txt, 62, 28, 2);

  // chão e base fixa
  arm.drawFastHLine(0, 214, 160, TFT_DARKGREY);
  arm.fillRect(55, 202, 50, 12, TFT_DARKGREY);

  // braco (coluna): 90 = vertical
  float rb = radians(braco);
  float ex = X0 + L0 * cosf(rb), ey = Y0 - L0 * sinf(rb);
  arm.drawWideLine(X0, Y0, ex, ey, 7, TFT_LIGHTGREY, TFT_BLACK);

  // cotovelo: 90 = alinhado com o braco
  float a1 = radians(cotovelo + braco - 90);
  float x1 = ex + L1 * cosf(a1), y1 = ey - L1 * sinf(a1);
  // pulso: 90 = alinhado com o antebraço
  float a2 = a1 + radians(pulso - 90);
  float x2 = x1 + L2 * cosf(a2), y2 = y1 - L2 * sinf(a2);

  arm.drawWideLine(ex, ey, x1, y1, 6, LARANJA, TFT_BLACK);
  arm.drawWideLine(x1, y1, x2, y2, 5, AMARELO, TFT_BLACK);

  // garra: ângulo maior = mais FECHADA (invertido em relacao ao original)
  float s = radians(5 + ((180.0f - garra) / 180.0f) * 35);
  for (int k = -1; k <= 1; k += 2) {
    float gx = x2 + LG * cosf(a2 + k * s), gy = y2 - LG * sinf(a2 + k * s);
    arm.drawWideLine(x2, y2, gx, gy, 3, CIANO, TFT_BLACK);
  }

  // juntas
  arm.fillCircle(X0, Y0, 5, TFT_RED);
  arm.fillCircle((int)ex, (int)ey, 5, TFT_RED);
  arm.fillCircle((int)x1, (int)y1, 4, TFT_RED);
  arm.fillCircle((int)x2, (int)y2, 3, TFT_RED);

  // leitura dos ângulos embaixo
  snprintf(txt, sizeof(txt), "A%d C%d P%d G%d", (int)braco, (int)cotovelo, (int)pulso, (int)garra);
  arm.setTextColor(TFT_DARKGREY, TFT_BLACK);
  arm.drawString(txt, 4, 224, 2);

  arm.pushSprite(0, 0);
}

// ---------- painel de botoes ----------
void desenharBotao(int x, int y, const char* simbolo) {
  tft.fillRoundRect(x, y, BTN_W, BTN_H, 6, CINZA_BTN);
  tft.drawRoundRect(x, y, BTN_W, BTN_H, 6, TFT_LIGHTGREY);
  tft.setTextColor(TFT_WHITE, CINZA_BTN);
  tft.drawCentreString(simbolo, x + BTN_W / 2, y + (BTN_H - 26) / 2, 4);
}

void desenharPainel() {
  for (int i = 0; i < N_EIXOS; i++) {
    int y = ROW_Y0 + i * ROW_STEP;
    desenharBotao(BTN_MINUS_X, y, "-");
    desenharBotao(BTN_PLUS_X, y, "+");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString(NOMES[i], LABEL_X, y + (BTN_H - 16) / 2, 2);
  }
}

void desenharEnlace(bool ok) {
  tft.fillCircle(306, 10, 5, ok ? TFT_GREEN : TFT_RED);
}

void setup() {
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);           // backlight

  tft.init();
  tft.setRotation(1);               // paisagem 320x240
  tft.fillScreen(TFT_BLACK);

  // touch em barramento SPI separado
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  LARANJA   = tft.color565(255, 152, 0);
  AMARELO   = tft.color565(255, 213, 79);
  CIANO     = tft.color565(77, 208, 225);
  CINZA_BTN = tft.color565(60, 60, 70);

  arm.createSprite(160, 240);
  tft.drawFastVLine(160, 8, 224, TFT_DARKGREY);   // divisória
  desenharPainel();
  desenharEnlace(false);

  if (ENVIAR_ESPNOW) {
    delay(300);
    espnowOk = iniciarEspNow();
  }
}

void loop() {
  atualizarToque();

  // envia os valores atuais para a ponte ESP-NOW
  if (espnowOk) {
    static unsigned long ultimoEnvio = 0;
    if (millis() - ultimoEnvio >= PERIODO_ENVIO_MS) {
      ultimoEnvio = millis();
      enviarPosicao();
    }
  }

  desenharBraco();

  // estado da garra (só redesenha quando muda) - invertido: angulo alto = fechada
  static int antes = -1;
  int aberta = garra < 90;
  if (aberta != antes) {
    antes = aberta;
    tft.setTextPadding(140);
    tft.setTextColor(aberta ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
    tft.drawString(aberta ? "Garra aberta" : "Garra fechada", 172, 218, 2);
    tft.setTextPadding(0);
  }

  // estado do enlace (só redesenha quando muda)
  static int enlaceAntes = -1;
  unsigned long r = ultimaResposta;
  int enlace = (r != 0 && millis() - r < 1000) ? 1 : 0;
  if (enlace != enlaceAntes) {
    enlaceAntes = enlace;
    desenharEnlace(enlace);
  }

  delay(30);
}