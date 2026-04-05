# TEG-Electromedicina
Sistema de control para electroanalgesia TENS con ESP32

## Dependencias (Arduino IDE / PlatformIO)
- **WiFiManager** — Configuración WiFi sin hardcodear
- **ArduinoJson** (v7+) — Parseo de recetas JSON

## Formato de Receta JSON
```json
{
  "receta_id": "terapia_01",
  "num_ciclos": 1,
  "ciclos": [
    {"trabajo_seg": 300, "pausa_seg": 60}
  ],
  "parametros_pulso": {
    "frecuencia_hz": 50,
    "ancho_pulso_ms": 0.5
  }
}
```

## Endpoints
| Ruta | Método | Descripción |
|------|--------|-------------|
| `/` | GET | Interfaz web |
| `/receta` | POST | Cargar receta JSON |
| `/iniciar?int=N` | GET | Iniciar terapia con intensidad N |
| `/detener` | GET | Detener terapia |
| `/emergencia` | GET | Parada de emergencia (0V inmediato) |
| `/set_intensidad?v=N` | GET | Ajustar intensidad en tiempo real |
| `/estado_completo` | GET | Estado JSON completo con progreso |
