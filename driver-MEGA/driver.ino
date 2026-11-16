/*
 * Driver de motores de passo - Arduino Mega 2560
 * 5 eixos (STEP/DIR) + garra (servo em D9)
 *
 * Comunicação (mesmos comandos nos dois canais):
 *   - Serial USB (Serial)           -> 115200 baud
 *   - Serial2 (hardware)            -> 115200 baud (TX2 = pino 16, RX2 = pino 17)
 *
 * OBS: I2C no Mega fica nos pinos 20/21, que no seu pinout já são STEP/DIR
 * do eixo 1. Por isso o link com a ponte usa a Serial2 (pinos 16/17, livres).
 *
 * Comandos (texto, terminados em \n). Ângulos em GRAUS no eixo de saída:
 *   P a1 a2 a3 a4 a5 g   -> posição absoluta dos 5 eixos + garra
 *   M<n> <graus>         -> eixo n (1-5) para posição absoluta   ex: M3 45.5
 *   R<n> <graus>         -> eixo n movimento relativo            ex: R1 -10
 *   G <graus>            -> garra (0-180)                        ex: G 90
 *   S<n> <graus/s>       -> velocidade máxima do eixo n          ex: S2 90
 *   Z                    -> zera a posição atual de todos os eixos (home)
 *   X                    -> parada (com desaceleração)
 *   T                    -> teste de todos os motores
 *   T<n>                 -> teste só do eixo n                   ex: T4
 *   ?                    -> status (posições, ocupado)
 */

#include <Servo.h>

// ===================== CONFIGURAÇÃO =====================
#define BAUD_USB  115200
#define BAUD_SW   115200    // Serial2 (hardware): precisa ser igual ao MEGA_BAUD da ponte

#define N_AXES    5
#define PIN_GARRA 9

#define PULSE_US   3        // largura do pulso STEP (us)
#define MIN_SPEED  50.0f    // velocidade inicial/final (passos/s)

#define TEST_ON_BOOT true   // true = roda o teste ao ligar (~10 s sem ouvir comandos)
#define DEBUG_RX     true   // imprime na USB o que chegou pela Serial2 (16/17)
#define TEST_DEG     30.0f  // amplitude do teste (graus)

#define GARRA_MIN_DEG   0
#define GARRA_MAX_DEG 180
#define GARRA_TEST_A   20   // posições usadas no teste da garra
#define GARRA_TEST_B  100

// passos por grau = 200 * microstep * redução / 360
#define SPD(micro, ratio) (200.0f * (micro) * (ratio) / 360.0f)

struct Axis {
  uint8_t stepPin, dirPin;
  bool    invertDir;
  float   stepsPerDeg;
  float   minDeg, maxDeg;     // limites de software
  float   maxSpeed;           // passos/s
  float   accel;              // passos/s^2
  // estado interno
  long    pos, target;
  float   speed;
  int8_t  dir;
  unsigned long interval;     // us entre passos
  unsigned long lastStep;
};

//                 STEP DIR  inv   passos/grau       min    max   vmax  accel
Axis axes[N_AXES] = {
  { 20, 21, false, SPD(4, 1.0f),  -360, 360, 1500,  3000, 0,0,0,0,0,0 }, // 1: 2x Nema23, /4
  { 18, 19, false, SPD(16, 5.0f), -360, 360, 4000, 10000, 0,0,0,0,0,0 }, // 2: Nema17 5:1, /16
  {  4,  3, false, SPD(16, 1.0f), -360, 360, 4000, 10000, 0,0,0,0,0,0 }, // 3: Nema17, /16
  {  6,  5, false, SPD(16, 1.0f), -360, 360, 4000, 10000, 0,0,0,0,0,0 }, // 4: Nema14, /16
  { 38, 39, false, SPD(16, 1.0f), -360, 360, 4000, 10000, 0,0,0,0,0,0 }, // 5: Nema17 60mm, /16
};
// ========================================================

#define swSerial Serial2    // pinos 16 (TX2) e 17 (RX2)
Servo garra;
float garraDeg = 0;

struct LineBuf {
  char buf[96]; uint8_t len;
  unsigned long bytes, lines;   // diagnóstico
  char last[40];                // última linha recebida
};
LineBuf bufUsb = {{0}, 0, 0, 0, {0}};
LineBuf bufSw  = {{0}, 0, 0, 0, {0}};

// ---------- movimento ----------
void setSpeed(Axis &a, float s) {
  a.speed = s;
  a.interval = (s > 0) ? (unsigned long)(1000000.0f / s) : 0;
}

void moveTo(uint8_t i, float deg) {
  Axis &a = axes[i];
  deg = constrain(deg, a.minDeg, a.maxDeg);
  a.target = lroundf(deg * a.stepsPerDeg);
}

float posDeg(uint8_t i) { return axes[i].pos / axes[i].stepsPerDeg; }

void stopAxis(Axis &a) {
  if (a.speed > 0) {
    long brake = (long)ceilf(a.speed * a.speed / (2.0f * a.accel)) + 1;
    a.target = a.pos + a.dir * brake;
  } else {
    a.target = a.pos;
  }
}

// Executa no máximo um passo; retorna true se o eixo ainda tem trabalho
bool runAxis(Axis &a) {
  long dist = a.target - a.pos;
  if (dist == 0) { setSpeed(a, 0); return false; }

  unsigned long now = micros();

  if (a.speed == 0) {                         // partida
    a.dir = (dist > 0) ? 1 : -1;
    digitalWrite(a.dirPin, ((a.dir > 0) != a.invertDir) ? HIGH : LOW);
    delayMicroseconds(5);
    setSpeed(a, MIN_SPEED);
    a.lastStep = now - a.interval;            // primeiro passo imediato
  }

  if ((unsigned long)(now - a.lastStep) < a.interval) return true;

  // gera o passo
  digitalWrite(a.stepPin, HIGH);
  delayMicroseconds(PULSE_US);
  digitalWrite(a.stepPin, LOW);
  a.pos += a.dir;
  a.lastStep = now;

  long remaining = (a.dir > 0) ? (a.target - a.pos) : (a.pos - a.target);
  if (remaining == 0) { setSpeed(a, 0); return false; }

  float dt = 1.0f / a.speed;
  float brakeDist = a.speed * a.speed / (2.0f * a.accel);
  float s = a.speed;

  if (remaining < 0 || remaining <= brakeDist) {   // desacelera
    s -= a.accel * dt;
    if (s <= MIN_SPEED) {
      if (remaining < 0) { setSpeed(a, 0); return true; } // inverte sentido
      s = MIN_SPEED;
    }
  } else if (s < a.maxSpeed) {                      // acelera
    s += a.accel * dt;
    if (s > a.maxSpeed) s = a.maxSpeed;
  }
  setSpeed(a, s);
  return true;
}

bool runAll() {
  bool busy = false;
  for (uint8_t i = 0; i < N_AXES; i++) busy |= runAxis(axes[i]);
  return busy;
}

void waitAll() { while (runAll()) { } }

void setGarra(float deg) {
  garraDeg = constrain(deg, GARRA_MIN_DEG, GARRA_MAX_DEG);
  garra.write((int)garraDeg);
}

// ---------- teste ----------
void testeEixo(uint8_t i, Stream &out) {
  float inicio = posDeg(i);
  out.print(F("TESTE eixo ")); out.println(i + 1);
  moveTo(i, inicio + TEST_DEG); waitAll(); delay(300);
  moveTo(i, inicio - TEST_DEG); waitAll(); delay(300);
  moveTo(i, inicio);            waitAll(); delay(300);
}

void testeGarra(Stream &out) {
  out.println(F("TESTE garra"));
  float inicio = garraDeg;
  setGarra(GARRA_TEST_A); delay(700);
  setGarra(GARRA_TEST_B); delay(700);
  setGarra(inicio);       delay(500);
}

// n = 0 testa tudo; n = 1..5 testa só o eixo
void testeMotores(uint8_t n, Stream &out) {
  if (n == 0) {
    for (uint8_t i = 0; i < N_AXES; i++) testeEixo(i, out);
    testeGarra(out);
  } else {
    testeEixo(n - 1, out);
  }
  out.println(F("TESTE OK"));
}

// ---------- parser ----------
bool nextFloat(char *&p, float &v) {
  while (*p == ' ' || *p == ',' || *p == '\t') p++;
  char *end;
  v = strtod(p, &end);
  if (end == p) return false;
  p = end;
  return true;
}

void printStatus(Stream &out) {
  bool busy = false;
  out.print(F("POS"));
  for (uint8_t i = 0; i < N_AXES; i++) {
    out.print(' '); out.print(posDeg(i), 2);
    if (axes[i].pos != axes[i].target) busy = true;
  }
  out.print(' '); out.print(garraDeg, 1);
  out.print(F(" BUSY ")); out.println(busy ? 1 : 0);
}

void handleLine(char *line, Stream &out) {
  char cmd = toupper(line[0]);
  char *p = line + 1;
  float v;

  switch (cmd) {
    case 'P': {
      float vals[N_AXES + 1];
      for (uint8_t k = 0; k < N_AXES + 1; k++) {
        if (!nextFloat(p, vals[k])) { out.println(F("ERR args")); return; }
      }
      for (uint8_t i = 0; i < N_AXES; i++) moveTo(i, vals[i]);
      setGarra(vals[N_AXES]);
      out.println(F("OK"));
      break;
    }
    case 'M':
    case 'R':
    case 'S': {
      int n = strtol(p, &p, 10);
      if (n < 1 || n > N_AXES || !nextFloat(p, v)) { out.println(F("ERR args")); return; }
      Axis &a = axes[n - 1];
      if (cmd == 'M') moveTo(n - 1, v);
      else if (cmd == 'R') moveTo(n - 1, (a.target / a.stepsPerDeg) + v);
      else a.maxSpeed = max(v * a.stepsPerDeg, MIN_SPEED * 2);
      out.println(F("OK"));
      break;
    }
    case 'G':
      if (!nextFloat(p, v)) { out.println(F("ERR args")); return; }
      setGarra(v);
      out.println(F("OK"));
      break;
    case 'Z':
      for (uint8_t i = 0; i < N_AXES; i++) { axes[i].pos = axes[i].target = 0; setSpeed(axes[i], 0); }
      out.println(F("OK"));
      break;
    case 'X':
      for (uint8_t i = 0; i < N_AXES; i++) stopAxis(axes[i]);
      out.println(F("OK"));
      break;
    case 'T': {
      int n = strtol(p, &p, 10);
      if (n < 0 || n > N_AXES) { out.println(F("ERR args")); return; }
      testeMotores(n, out);
      break;
    }
    case '?':
      printStatus(out);
      break;
    default:
      out.println(F("ERR cmd"));
  }
}

void pollStream(Stream &s, LineBuf &lb) {
  while (s.available()) {
    char c = s.read();
    lb.bytes++;
    if (c == '\n' || c == '\r') {
      if (lb.len > 0) {
        lb.buf[lb.len] = 0;
        lb.lines++;
        strncpy(lb.last, lb.buf, sizeof(lb.last) - 1);
        lb.last[sizeof(lb.last) - 1] = 0;
        handleLine(lb.buf, s);
        lb.len = 0;
      }
    } else if (lb.len < sizeof(lb.buf) - 1) {
      lb.buf[lb.len++] = c;
    }
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(BAUD_USB);
  swSerial.begin(BAUD_SW);

  for (uint8_t i = 0; i < N_AXES; i++) {
    pinMode(axes[i].stepPin, OUTPUT);
    pinMode(axes[i].dirPin, OUTPUT);
    digitalWrite(axes[i].stepPin, LOW);
  }
  garra.attach(PIN_GARRA);
  setGarra(GARRA_MIN_DEG);

  Serial.println(F("PRONTO"));
  swSerial.println(F("PRONTO"));

  if (TEST_ON_BOOT) {
    testeMotores(0, Serial);
    // descarta o que chegou durante o teste (o buffer da Serial2 é de 64 bytes)
    while (swSerial.available()) swSerial.read();
    bufSw.len = 0;
    bufUsb.len = 0;
    bufSw.bytes = 0;
    bufSw.lines = 0;
  }
}

void loop() {
  pollStream(Serial, bufUsb);
  pollStream(swSerial, bufSw);
  runAll();

#if DEBUG_RX
  // a cada 2 s mostra na USB o que chegou pela Serial2 (pinos 16/17)
  static unsigned long beat = 0;
  if (millis() - beat >= 2000) {
    beat = millis();
    Serial.print(F("[sw] bytes="));  Serial.print(bufSw.bytes);
    Serial.print(F(" linhas="));     Serial.print(bufSw.lines);
    Serial.print(F(" ultima=\""));   Serial.print(bufSw.last);
    Serial.println('"');
  }
#endif
}