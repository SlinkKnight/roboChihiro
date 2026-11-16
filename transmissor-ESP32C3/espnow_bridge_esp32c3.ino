/*
 * Ponte ESP-NOW -> Serial2 do Arduino Mega  (ESP32-C3)
 *
 * Recebe pacotes ESP-NOW cujo payload é o texto de um comando do driver do Mega
 * (ex: "P 10 20 30 40 50 90", "M3 45.5", "T", "?") e repassa para a Serial2
 * do Mega (pinos 16/17). Aceita vários comandos no mesmo pacote separados por
 * '\n'; se o pacote não terminar com '\n', ele é acrescentado.
 *
 * As respostas do Mega ("OK", "POS ...", "ERR ...") são enviadas de volta,
 * via ESP-NOW, para o último MAC que mandou um pacote.
 *
 * Também aceita comandos digitados no Serial Monitor do ESP (USB, 115200),
 * úteis para testar a ligação com o Mega sem precisar do transmissor ESP-NOW.
 *
 * LIGAÇÃO (mesma do demo que funcionou; Mega = 5V, ESP32-C3 = 3,3V):
 *   Mega pino 16 (TX2) --[ 1k em série + 2k para GND ]--> ESP32-C3 GPIO9 (RX)
 *   Mega pino 17 (RX2) <------------------------------- ESP32-C3 GPIO8 (TX)
 *   GND ESP32 ------------------------------------------ GND Mega
 *
 * ATENÇÃO (pinos de boot do C3): o divisor de tensão puxa o GPIO9 para GND
 * quando o Mega está desligado ou em reset, e GPIO9 baixo no reset coloca o C3
 * em modo de gravação (não roda o programa). Se o ESP não iniciar:
 *   - ligue o Mega antes (ou junto) do ESP; ou
 *   - troque para pinos livres, por exemplo MEGA_RX_PIN 4 e MEGA_TX_PIN 5,
 *     movendo os fios (o divisor vai no fio que sai do pino 16 do Mega).
 *
 * O baud deve ser igual ao BAUD_SW do código do Mega (115200).
 * Transmissor e receptor ESP-NOW precisam estar no mesmo canal (ESPNOW_CHANNEL).
 * Para o monitor serial do C3 com USB nativa: Tools -> USB CDC On Boot: Enabled.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ===================== CONFIGURAÇÃO =====================
#define MEGA_RX_PIN     9      // recebe do TX2 (pino 16) do Mega, pelo divisor
#define MEGA_TX_PIN     8      // envia para o RX2 (pino 17) do Mega
#define MEGA_BAUD       115200 // se der lixo, use 9600 aqui E em BAUD_SW no Mega
#define ESPNOW_CHANNEL  1
#define MAX_LINE        95      // buffer de linha do Mega é 96 bytes
#define QUEUE_LEN       16
// ========================================================

struct Packet {
  uint8_t mac[6];
  uint8_t len;
  uint8_t data[250];
};

QueueHandle_t rxQueue;
volatile uint32_t pacotesRecebidos = 0;   // diagnóstico
uint8_t lastMac[6];
bool    haveMac = false;

// ---------- recepção ESP-NOW (roda na task do WiFi: só enfileira) ----------
static void handleRx(const uint8_t *mac, const uint8_t *data, int len) {
  if (len <= 0 || len > 250) return;
  pacotesRecebidos++;
  Packet p;
  memcpy(p.mac, mac, 6);
  p.len = len;
  memcpy(p.data, data, len);
  xQueueSend(rxQueue, &p, 0);   // se a fila encher, descarta o pacote
}

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  handleRx(info->src_addr, data, len);
}
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  handleRx(mac, data, len);
}
#endif

// ---------- envia ao Mega, linha a linha ----------
void forwardToMega(const uint8_t *data, int len) {
  char line[MAX_LINE + 1];
  int n = 0;
  for (int i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '\n' || c == '\r' || c == '\0') {
      if (n > 0) {
        Serial1.write((uint8_t *)line, n);
        Serial1.write('\n');
        Serial.printf("-> Mega: %.*s\n", n, line);
        n = 0;
      }
    } else if (n < MAX_LINE) {
      line[n++] = c;
    }
  }
  if (n > 0) {                       // pacote sem '\n' no final
    Serial1.write((uint8_t *)line, n);
    Serial1.write('\n');
    Serial.printf("-> Mega: %.*s\n", n, line);
  }
}

// ---------- responde ao último remetente ----------
void sendReply(const char *text, int len) {
  if (!haveMac || len <= 0) return;
  if (len > 250) len = 250;
  esp_now_send(lastMac, (const uint8_t *)text, len);
}

void rememberSender(const uint8_t *mac) {
  memcpy(lastMac, mac, 6);
  haveMac = true;
  if (!esp_now_is_peer_exist(lastMac)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, lastMac, 6);
    peer.channel = 0;               // usa o canal atual
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);   // espera o monitor USB
  delay(500);
  Serial.println("\n[ponte] boot");
  Serial1.begin(MEGA_BAUD, SERIAL_8N1, MEGA_RX_PIN, MEGA_TX_PIN);

  rxQueue = xQueueCreate(QUEUE_LEN, sizeof(Packet));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Erro ao iniciar ESP-NOW");
    delay(2000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onRecv);

  Serial.print("Ponte ESP-NOW pronta. MAC deste ESP: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("Canal %d | Mega @ %d baud | RX=GPIO%d TX=GPIO%d\n",
                ESPNOW_CHANNEL, MEGA_BAUD, MEGA_RX_PIN, MEGA_TX_PIN);
  Serial.println("Digite um comando (ex: ?) no monitor para testar o Mega.");
}

void loop() {
  // batimento: prova que a ponte está viva e quantos pacotes chegaram
  static unsigned long ultimoBeat = 0;
  if (millis() - ultimoBeat >= 2000) {
    ultimoBeat = millis();
    Serial.printf("[ponte] viva | canal %d | pacotes recebidos: %u\n",
                  ESPNOW_CHANNEL, (unsigned)pacotesRecebidos);
  }

  // 0) comandos digitados no monitor USB -> Mega (teste sem ESP-NOW)
  static char ubuf[MAX_LINE + 1];
  static int ulen = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (ulen > 0) {
        forwardToMega((const uint8_t *)ubuf, ulen);
        ulen = 0;
      }
    } else if (ulen < MAX_LINE) {
      ubuf[ulen++] = c;
    }
  }

  // 1) pacotes ESP-NOW -> serial do Mega
  Packet p;
  while (xQueueReceive(rxQueue, &p, 0) == pdTRUE) {
    rememberSender(p.mac);
    forwardToMega(p.data, p.len);
  }

  // 2) respostas do Mega -> ESP-NOW (uma por linha)
  static char rbuf[200];
  static int rlen = 0;
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n' || c == '\r') {
      if (rlen > 0) {
        rbuf[rlen] = '\0';
        Serial.printf("<- Mega: %s\n", rbuf);
        sendReply(rbuf, rlen);
        rlen = 0;
      }
    } else if (rlen < (int)sizeof(rbuf) - 1) {
      rbuf[rlen++] = c;
    }
  }
}
