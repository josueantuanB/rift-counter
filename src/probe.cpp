// Sonda de hardware. No forma parte del firmware: env "probe".
// El PCB serigrafia el I2C en IO15(SCL) / IO16(SDA); se escanea eso primero.

#include <Arduino.h>
#include <Wire.h>

static const char *chipName(uint8_t a) {
  switch (a) {
    case 0x14: case 0x5D: return "GT911 tactil";
    case 0x15: return "CST816S/CST820 tactil";
    case 0x1A: return "CST328 tactil";
    case 0x24: return "TT21100 tactil";
    case 0x38: return "FT6236/FT6336 tactil";
    case 0x3B: return "AXS15231B tactil";
    case 0x18: case 0x1B: return "ES8311/audio codec";
    case 0x20: case 0x21: return "expansor IO (TCA9554/PCF8574)";
    case 0x36: return "MAX17048 fuel gauge";
    case 0x51: return "PCF8563 RTC";
    case 0x68: case 0x6A: case 0x6B: return "IMU (MPU6050/QMI8658)";
    default: return "?";
  }
}

static void scan(uint8_t sda, uint8_t scl) {
  Serial.printf("SDA=%u SCL=%u:", sda, scl);
  if (!Wire.begin(sda, scl, 100000)) { Serial.println(" no se pudo iniciar"); return; }
  Wire.setTimeOut(10);
  int n = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("\n  0x%02X  %s", a, chipName(a));
      n++;
    }
  }
  Serial.println(n ? "" : " nada");
  Wire.end();
}

void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  delay(800);

  Serial.println("\n=== sonda I2C dirigida ===");
  Serial.printf("chip=%s rev=%d flash=%uMB psram=%uKB\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getFlashChipSize() / (1024 * 1024),
                ESP.getPsramSize() / 1024);

  scan(16, 15);  // como dice la serigrafia
  scan(15, 16);  // por si las etiquetas estan cruzadas
  Serial.println("=== fin ===");
}

void loop() { delay(1000); }
