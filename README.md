# Rift Counter

Contador de puntos para el TCG **Riftbound**, en una placa **ES3C28P**
(ESP32-S3, LCD ILI9341V 240x320 SPI, tactil FT6336 I2C).

## Que hace

- Formatos BO1 y BO3, con marcador de rondas.
- Marcador a 8 puntos por jugador, con confirmacion al cerrar la ronda.
- Cuenta atras de partida: 30 min en BO1, 60 min en BO3.
- Contador de XP opcional, que se pregunta al empezar.
- Tirada de d20 para decidir quien empieza.
- Porcentaje de bateria (LiPo 1S por el divisor de IO9).
- Apagado por sueno ligero: despierta al tocar la pantalla.
- Actualizacion por WiFi desde los releases de este repositorio.

## Compilar

```
pio run -e es3c28p            # firmware
pio run -e es3c28p -t upload  # flashear por USB
pio run -e probe -t upload    # sonda I2C, para diagnosticar el hardware
```

Si esptool no conecta, la placa tiene tomado el USB: manten BOOT, pulsa y
suelta RESET, suelta BOOT y reintenta.

## Publicar una actualizacion

1. Subir `FW_VERSION` y `VERSION` en `src/main.cpp`.
2. `git tag v0.4 && git push --tags`

El workflow compila, genera `firmware.json` leyendo `FW_VERSION` del codigo y
sube ambos al release. El aparato lee
`releases/latest/download/firmware.json`, compara la version y descarga solo si
hay algo mas nuevo.

La descarga va por HTTPS **sin validar el certificado**: esta cifrada, pero no
protege de un intermediario activo en la red. Fijar la CA dentro del binario
dejaria el aparato sin poder actualizarse el dia que caduque.

## Pinout de la placa

| | |
|---|---|
| LCD ILI9341V | CS 10, DC 46, SCK 12, MOSI 11, MISO 13, BL 45 |
| Tactil FT6336 | SDA 16, SCL 15, RST 18, INT 17, I2C 0x38 |
| Audio ES8311 | I2C 0x18, amplificador FM8002E |
| SD | SDIO 4-bit, IO38-41/47/48 |
| Bateria | ADC en IO9, divisor 1:2 |
| LED RGB | WS2812B en IO42 |

TFT_eSPI necesita `-DUSE_FSPI_PORT=1` en el S3: sin el resuelve `SPI_PORT` a 0
y `REG_SPI_BASE(0)` da 0, con lo que `tft.init()` escribe en la direccion 0x10
y entra en panico.
