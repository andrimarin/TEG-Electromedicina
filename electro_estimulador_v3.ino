// ============================================================
// ELECTROTERAPIA TENS - CON WiFiManager + Recetas JSON
// Pin de salida: GPIO8 (Para ESP32-C3 SuperMini)
// Controla: INTENSIDAD, FRECUENCIA y ANCHO DE PULSO
// Corrección: Lógica inversa segura y pulsos por software.
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>

// ================== PINES Y HARDWARE ==================
const int PIN_TENS = 8;        // GPIO8 para ESP32-C3 SuperMini
const int PIN_LED_BUILTIN = 2; // Ajusta si tu C3 tiene otro pin para el LED

// SEGURIDAD: Lógica Inversa para el optoacoplador PC817 + IRFZ44N
const int ESTADO_ON = LOW;     // LOW enciende el MOSFET
const int ESTADO_OFF = HIGH;   // HIGH apaga el MOSFET de forma segura

// ================== MÁQUINA DE ESTADOS ==================
enum EstadoTerapia {
  ESTADO_IDLE,
  ESTADO_TRABAJANDO,
  ESTADO_PAUSA,
  ESTADO_FINALIZADO
};
EstadoTerapia estadoActual = ESTADO_IDLE;

// ================== PARÁMETROS ==================
int intensidad = 0;             // mA simulados (0 - MAX_ALLOWED_MA)
int frecuenciaHz = 50;          // Hz
float anchoPulsoMs = 0.5;       // ms
bool terapiaActiva = false;

const int MAX_ALLOWED_MA = 100; // Límite seguro
int pwmValor = 0;               // Mantenido por compatibilidad con la UI

void actualizarIntensidadMa(int ma) {
  if (ma < 0) ma = 0;
  if (ma > MAX_ALLOWED_MA) ma = MAX_ALLOWED_MA;
  intensidad = ma;
  if (intensidad == 0) pwmValor = 0;
  else pwmValor = map(intensidad, 0, MAX_ALLOWED_MA, 0, 255);
}

// ================== RECETA ==================
#define MAX_CICLOS 10
struct Ciclo {
  unsigned long trabajoSeg;
  unsigned long pausaSeg;
};
String recetaId = "";
int numCiclos = 0;
Ciclo ciclos[MAX_CICLOS];
int cicloActualIdx = 0;
unsigned long inicioFaseMs = 0;

// ================== GENERACIÓN DE ONDA ==================
unsigned long ultimoPulso = 0;
bool pulsoEncendido = false;

WebServer server(80);

// ================== APAGADO GARANTIZADO ==================
void apagarElectrodoTotal() {
  pulsoEncendido = false;
  digitalWrite(PIN_TENS, ESTADO_OFF); // Seguridad: HIGH apaga el circuito
}

// ================== GENERACIÓN DE ONDA (Software) ==================

// Aritmética unsigned ya maneja overflow de micros() correctamente
bool haPasadoTiempo(unsigned long &marca, unsigned long intervaloUs) {
  unsigned long ahora = micros();
  if ((ahora - marca) >= intervaloUs) {
    marca = ahora;
    return true;
  }
  return false;
}

// Valores precalculados de la onda (se actualizan solo cuando cambian parámetros)
unsigned long anchoOnUs = 0;
unsigned long anchoOffUs = 0;
int lastIntensidad = -1;
int lastFrecuencia = -1;
float lastAnchoPulso = -1;

void recalcularParametrosOnda() {
  if (intensidad == lastIntensidad && frecuenciaHz == lastFrecuencia && anchoPulsoMs == lastAnchoPulso) return;
  
  lastIntensidad = intensidad;
  lastFrecuencia = frecuenciaHz;
  lastAnchoPulso = anchoPulsoMs;
  
  if (frecuenciaHz < 1) frecuenciaHz = 1;
  unsigned long periodoUs = 1000000UL / frecuenciaHz;
  
  // Corrección: mantener float hasta después de multiplicar para no truncar decimales
  // Ej: 0.5ms * 1000 = 500µs, luego escalar por intensidad
  anchoOnUs = (unsigned long)(anchoPulsoMs * 1000.0f * (float)intensidad / (float)MAX_ALLOWED_MA);
  
  if (anchoOnUs < 10 && intensidad > 0) anchoOnUs = 10;  // Mínimo absoluto 10µs
  if (anchoOnUs >= periodoUs) anchoOnUs = periodoUs / 2;  // Máximo 50% duty cycle
  
  anchoOffUs = periodoUs - anchoOnUs;
  if (anchoOffUs < 1) anchoOffUs = 1;
}

void generarOnda() {
  if (estadoActual != ESTADO_TRABAJANDO || intensidad == 0) {
    if (pulsoEncendido) apagarElectrodoTotal();
    return;
  }
  
  recalcularParametrosOnda();
  
  if (pulsoEncendido) {
    if (haPasadoTiempo(ultimoPulso, anchoOnUs)) {
      digitalWrite(PIN_TENS, ESTADO_OFF);
      pulsoEncendido = false;
    }
  } else {
    if (haPasadoTiempo(ultimoPulso, anchoOffUs)) {
      digitalWrite(PIN_TENS, ESTADO_ON);
      pulsoEncendido = true;
    }
  }
}
// ================== MÁQUINA DE ESTADOS DE TERAPIA ==================
void gestionarReceta() {
  if (estadoActual == ESTADO_IDLE || estadoActual == ESTADO_FINALIZADO) return;
  
  unsigned long tiempoEnFaseMs = millis() - inicioFaseMs;
  
  if (estadoActual == ESTADO_TRABAJANDO) {
    unsigned long duracionTrabajoMs = ciclos[cicloActualIdx].trabajoSeg * 1000UL;
    if (tiempoEnFaseMs >= duracionTrabajoMs) {
      apagarElectrodoTotal();
      if (ciclos[cicloActualIdx].pausaSeg > 0) {
        estadoActual = ESTADO_PAUSA;
        inicioFaseMs = millis();
        Serial.printf("⏸ Pausa ciclo %d/%d - %lu s\n", cicloActualIdx + 1, numCiclos, ciclos[cicloActualIdx].pausaSeg);
      } else {
        avanzarAlSiguienteCiclo();
      }
    }
  }
  else if (estadoActual == ESTADO_PAUSA) {
    unsigned long duracionPausaMs = ciclos[cicloActualIdx].pausaSeg * 1000UL;
    if (tiempoEnFaseMs >= duracionPausaMs) avanzarAlSiguienteCiclo();
  }
}

void avanzarAlSiguienteCiclo() {
  cicloActualIdx++;
  if (cicloActualIdx >= numCiclos) {
    finalizarTerapia();
  } else {
    estadoActual = ESTADO_TRABAJANDO;
    inicioFaseMs = millis();
    Serial.printf("▶ Ciclo %d/%d\n", cicloActualIdx + 1, numCiclos);
  }
}

void iniciarReceta() {
  if (numCiclos == 0 || intensidad == 0) return;
  
  cicloActualIdx = 0;
  estadoActual = ESTADO_TRABAJANDO;
  terapiaActiva = true;
  inicioFaseMs = millis();
  ultimoPulso = micros();
  Serial.printf("=== RECETA %s INICIADA ===\n", recetaId.c_str());
}

void finalizarTerapia() {
  estadoActual = ESTADO_FINALIZADO;
  terapiaActiva = false;
  intensidad = 0;
  apagarElectrodoTotal();
  Serial.println("=== RECETA FINALIZADA. 0V confirmado ===");
}

void detenerTerapiaManual() {
  estadoActual = ESTADO_IDLE;
  terapiaActiva = false;
  apagarElectrodoTotal();
  Serial.println("=== DETENIDA MANUALMENTE ===");
}

void emergenciaTotal() {
  estadoActual = ESTADO_IDLE;
  terapiaActiva = false;
  intensidad = 0;
  apagarElectrodoTotal();
  Serial.println("!!! PARADA DE EMERGENCIA !!!");
}

// ================== INTERFAZ WEB ==================
// (Se mantiene intacta la interfaz HTML original generada por el usuario)
String getHTML() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=yes">
  <title>TENS - Electroterapia</title>
  <style>
    * { box-sizing: border-box; user-select: none; }
    body { font-family: 'Segoe UI', Roboto, sans-serif; background: linear-gradient(135deg, #0a0f1e 0%, #0a1a2a 100%); min-height: 100vh; margin: 0; padding: 20px; display: flex; justify-content: center; align-items: center; }
    .container { max-width: 550px; width: 100%; margin: auto; }
    .card { background: rgba(22, 34, 48, 0.95); backdrop-filter: blur(10px); border-radius: 32px; padding: 24px 20px 32px; box-shadow: 0 20px 35px -10px rgba(0,0,0,0.5); border: 1px solid rgba(255,255,255,0.08); }
    h1 { font-size: 1.8rem; margin: 0 0 8px 0; font-weight: 600; background: linear-gradient(135deg, #aaffdd, #2ecc71); -webkit-background-clip: text; background-clip: text; color: transparent; text-align: center; }
    .estado-panel { background: #07121c; border-radius: 48px; padding: 12px 20px; text-align: center; margin-bottom: 20px; border: 1px solid #2a4a6e; }
    .estado-led { display: inline-block; width: 14px; height: 14px; border-radius: 14px; background: #555; margin-right: 8px; }
    .estado-texto { font-weight: bold; letter-spacing: 1px; color: #ccc; }
    .activo .estado-led { background: #2ecc71; box-shadow: 0 0 8px #2ecc71; }
    .activo .estado-texto { color: #2ecc71; }
    .pausa .estado-led { background: #f39c12; box-shadow: 0 0 8px #f39c12; }
    .pausa .estado-texto { color: #f39c12; }
    .finalizado .estado-led { background: #3498db; box-shadow: 0 0 8px #3498db; }
    .finalizado .estado-texto { color: #3498db; }
    .inactivo .estado-led { background: #e74c3c; }
    .inactivo .estado-texto { color: #e74c3c; }
    .param-card { background: #0a1824; border-radius: 28px; padding: 16px 20px; margin-bottom: 16px; }
    .param-label { display: flex; justify-content: space-between; font-weight: 600; margin-bottom: 12px; color: #c0e0ff; }
    input[type=range] { width: 100%; height: 6px; -webkit-appearance: none; background: #2c4c6c; border-radius: 10px; outline: none; }
    input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 24px; height: 24px; background: #2ecc71; border-radius: 50%; cursor: pointer; box-shadow: 0 0 8px #2ecc71; border: none; }
    textarea { width: 100%; background: #071723; color: #aaffdd; border: 1px solid #2a4a6e; border-radius: 16px; padding: 12px; font-family: monospace; font-size: 0.8rem; resize: vertical; min-height: 120px; }
    .botonera { display: flex; gap: 12px; justify-content: center; margin: 20px 0 10px; }
    button { border: none; padding: 14px 24px; border-radius: 60px; font-weight: bold; font-size: 1rem; background: #1f3e54; color: white; cursor: pointer; transition: 0.2s; flex: 1; }
    .btn-cargar { background: #3498db; color: #fff; }
    .btn-iniciar { background: #2ecc71; color: #000; }
    .btn-detener { background: #e74c3c; }
    .btn-emergencia { background: #c0392b; border: 1px solid #ff8866; width: 100%; }
    .info-receta { background: #07121c; border-radius: 20px; padding: 14px; margin-bottom: 16px; font-size: 0.85rem; color: #8aaec0; }
    .info-receta b { color: #aaffdd; }
    .progreso-bar { background: #1a2a3a; border-radius: 10px; height: 8px; margin-top: 10px; overflow: hidden; }
    .progreso-bar-fill { background: linear-gradient(90deg, #2ecc71, #27ae60); height: 100%; width: 0%; transition: width 0.5s; border-radius: 10px; }
    footer, .ip-info { text-align: center; font-size: 0.7rem; color: #2c5a7a; margin-top: 15px; }
    .badge { background: #0e2a3a; border-radius: 24px; padding: 6px 12px; font-size: 0.75rem; display: inline-block; }
  </style>
</head>
<body>
<div class="container">
  <div class="card">
    <h1>⚡ TENS · Teleterapia</h1>
    <div style="text-align:center;margin-bottom:16px"><span class="badge">📡 Control WiFi · Recetas JSON</span></div>
    <div id="estadoWidget" class="estado-panel inactivo">
      <span class="estado-led"></span><span class="estado-texto" id="estadoTexto">SISTEMA INACTIVO</span>
    </div>
    <div class="info-receta" id="infoReceta">📋 <b>Sin receta cargada</b></div>
    <div class="progreso-bar"><div class="progreso-bar-fill" id="progresoFill"></div></div>
    
    <div class="param-card" style="margin-top:16px">
      <div class="param-label"><span>📝 RECETA JSON</span></div>
      <textarea id="recetaJson">{
  "receta_id": "terapia_01",
  "num_ciclos": 1,
  "ciclos": [ {"trabajo_seg": 300, "pausa_seg": 60} ],
  "parametros_pulso": { "frecuencia_hz": 50, "ancho_pulso_ms": 0.5 }
}</textarea>
      <div style="margin-top:10px;text-align:center"><button class="btn-cargar" id="btnCargar">📤 Cargar Receta</button></div>
    </div>
    
    <div class="param-card">
      <div class="param-label"><span>⚡ INTENSIDAD (amplitud)</span><span id="intensidadValor">0</span></div>
      <input type="range" id="intensidadSlider" min="0" max="100" value="0">
    </div>
    
    <div class="botonera">
      <button class="btn-iniciar" id="btnIniciar">▶ INICIAR</button>
      <button class="btn-detener" id="btnDetener">⏹ DETENER</button>
    </div>
    <button class="btn-emergencia" id="btnEmergencia">🛑 PARADA DE EMERGENCIA</button>
    
    <div style="margin-top:20px; background:#07121c; border-radius:20px; padding:12px; text-align:center;">
      <span>🔋 Estado: </span><span id="estadoTerapia" style="font-weight:bold; color:#ffaa66">SIN TERAPIA</span>
      <div id="feedback" style="font-size:12px; margin-top:8px;">✅ Conectado al ESP32</div>
    </div>
    <div class="ip-info" id="ipDisplay"></div>
  </div>
</div>
<script>
  const S = (id) => document.getElementById(id);
  let encendida = false, cargada = false;

  S('intensidadSlider').oninput = (e) => {
    S('intensidadValor').innerText = e.target.value;
    fetch('/set_intensidad?v=' + e.target.value).catch(()=>{});
  };

  S('btnCargar').onclick = async () => {
    try {
      const res = await fetch('/receta', { method: 'POST', body: S('recetaJson').value });
      if(res.ok) {
        const d = await res.json();
        cargada = true;
        S('infoReceta').innerHTML = `📋 <b>${d.receta_id}</b> — ${d.num_ciclos} ciclo(s) · ${d.frecuencia_hz} Hz`;
        S('feedback').innerText = "✅ Receta cargada";
      }
    } catch(e) { S('feedback').innerText = "❌ Error al cargar JSON"; }
  };

  S('btnIniciar').onclick = async () => {
    if(!cargada) return S('feedback').innerText = "⚠️ Carga receta primero";
    if(S('intensidadSlider').value === "0") return S('feedback').innerText = "⚠️ Sube la intensidad";
    await fetch('/iniciar?int=' + S('intensidadSlider').value);
  };
  S('btnDetener').onclick = () => fetch('/detener');
  S('btnEmergencia').onclick = () => { fetch('/emergencia'); S('intensidadSlider').value = 0; S('intensidadValor').innerText = 0; };

  async function sync() {
    try {
      const d = await (await fetch('/estado_completo')).json();
      S('intensidadSlider').value = S('intensidadValor').innerText = d.intensidad_ma;
      S('progresoFill').style.width = (d.progreso||0) + "%";
      
      const e = d.estado;
      if (e === "trabajando") { S('estadoWidget').className = "estado-panel activo"; S('estadoTexto').innerText = "TERAPIA ACTIVA"; S('estadoTerapia').innerText = "▶ ACTIVA"; S('estadoTerapia').style.color = "#2ecc71"; }
      else if (e === "pausa") { S('estadoWidget').className = "estado-panel pausa"; S('estadoTexto').innerText = "PAUSA"; S('estadoTerapia').innerText = "⏸ PAUSA"; S('estadoTerapia').style.color = "#f39c12"; }
      else { S('estadoWidget').className = "estado-panel inactivo"; S('estadoTexto').innerText = "INACTIVO"; S('estadoTerapia').innerText = "⏹ INACTIVA"; S('estadoTerapia').style.color = "#ffaa66"; }
    } catch(err) {}
  }
  fetch('/ip').then(r=>r.text()).then(ip => S('ipDisplay').innerText = '🌐 ' + ip);
  setInterval(sync, 1500);
</script>
</body>
</html>
)rawliteral";
  return html;
}

// ================== CONFIGURACIÓN DE SERVIDOR WEB ==================
void setupServer() {
  server.on("/", []() { server.send(200, "text/html", getHTML()); });
  server.on("/ip", []() { server.send(200, "text/plain", WiFi.localIP().toString()); });
  
  server.on("/set_intensidad", []() {
    if (server.hasArg("v")) actualizarIntensidadMa(server.arg("v").toInt());
    server.send(200, "text/plain", "OK");
  });
  
  server.on("/receta", HTTP_POST, []() {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) return server.send(400, "application/json", "{\"error\":\"JSON invalido\"}");
    
    recetaId = doc["receta_id"] | "sin_id";
    numCiclos = doc["num_ciclos"] | 0;
    if (numCiclos > MAX_CICLOS) numCiclos = MAX_CICLOS;
    
    frecuenciaHz = doc["parametros_pulso"]["frecuencia_hz"] | 50;
    anchoPulsoMs = doc["parametros_pulso"]["ancho_pulso_ms"] | 0.5;
    
    if (frecuenciaHz < 1) frecuenciaHz = 1;
    if (frecuenciaHz > 150) frecuenciaHz = 150;
    if (anchoPulsoMs < 0.01) anchoPulsoMs = 0.01;
    
    JsonArray ciclosArr = doc["ciclos"].as<JsonArray>();
    int idx = 0;
    for (JsonObject c : ciclosArr) {
      if (idx >= numCiclos) break;
      ciclos[idx].trabajoSeg = c["trabajo_seg"] | 0;
      ciclos[idx].pausaSeg = c["pausa_seg"] | 0;
      idx++;
    }
    numCiclos = idx;
    
    String resp = "{\"ok\":true, \"receta_id\":\"" + recetaId + "\", \"num_ciclos\":" + String(numCiclos) + ", \"frecuencia_hz\":" + String(frecuenciaHz) + "}";
    server.send(200, "application/json", resp);
  });

  server.on("/iniciar", []() {
    if (server.hasArg("int")) actualizarIntensidadMa(server.arg("int").toInt());
    iniciarReceta();
    server.send(200, "text/plain", "OK");
  });

  server.on("/detener", []() { detenerTerapiaManual(); server.send(200, "text/plain", "OK"); });
  server.on("/emergencia", []() { emergenciaTotal(); server.send(200, "text/plain", "OK"); });
  
  server.on("/estado_completo", []() {
    float progreso = 0;
    if (estadoActual == ESTADO_TRABAJANDO || estadoActual == ESTADO_PAUSA) {
      unsigned long transcurridoSeg = 0, totalSeg = 0;
      for (int i = 0; i < numCiclos; i++) totalSeg += ciclos[i].trabajoSeg + ciclos[i].pausaSeg;
      for (int i = 0; i < cicloActualIdx; i++) transcurridoSeg += ciclos[i].trabajoSeg + ciclos[i].pausaSeg;
      
      unsigned long enFaseSeg = (millis() - inicioFaseMs) / 1000;
      transcurridoSeg += (estadoActual == ESTADO_TRABAJANDO) ? enFaseSeg : (ciclos[cicloActualIdx].trabajoSeg + enFaseSeg);
      if (totalSeg > 0) progreso = (float)transcurridoSeg / totalSeg * 100.0;
    } else if (estadoActual == ESTADO_FINALIZADO) progreso = 100;
    
    String est = (estadoActual == ESTADO_TRABAJANDO) ? "trabajando" : (estadoActual == ESTADO_PAUSA) ? "pausa" : (estadoActual == ESTADO_FINALIZADO) ? "finalizado" : "idle";
    String json = "{\"activa\":" + String(terapiaActiva?"true":"false") + ",\"estado\":\"" + est + "\",\"intensidad_ma\":" + String(intensidad) + ",\"progreso\":" + String(progreso) + "}";
    server.send(200, "application/json", json);
  });
  
  server.begin();
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // SEGURIDAD: Inicializar pin en HIGH (Apagado) desde el primer milisegundo
  pinMode(PIN_TENS, OUTPUT);
  digitalWrite(PIN_TENS, ESTADO_OFF);
  pinMode(PIN_LED_BUILTIN, OUTPUT);
  digitalWrite(PIN_LED_BUILTIN, LOW);
  
  Serial.println("\n=================================");
  Serial.println("⚡ ELECTROTERAPIA TENS - ESP32-C3");
  Serial.println("=================================");
  
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);
  
  // Desactivar watchdog durante WiFiManager para evitar reset
  // en portales de configuración lentos
  Serial.println("📡 Conectando WiFi...");
  if (!wifiManager.autoConnect("TENS_Config_WiFi")) {
    Serial.println("❌ Error de conexión. Reiniciando...");
    delay(1000);
    ESP.restart();
  }
  
  Serial.println("✅ Conectado a WiFi! IP: " + WiFi.localIP().toString());
  setupServer();
  Serial.println("✅ Servidor web iniciado");
}

// ================== LOOP ==================
void loop() {
  server.handleClient();
  gestionarReceta();
  generarOnda();
  
  static unsigned long lastBlink = 0;
  if (estadoActual == ESTADO_TRABAJANDO && intensidad > 0) {
    if (millis() - lastBlink > 300) { lastBlink = millis(); digitalWrite(PIN_LED_BUILTIN, !digitalRead(PIN_LED_BUILTIN)); }
  } else if (estadoActual == ESTADO_PAUSA) {
    if (millis() - lastBlink > 1000) { lastBlink = millis(); digitalWrite(PIN_LED_BUILTIN, !digitalRead(PIN_LED_BUILTIN)); }
  } else {
    digitalWrite(PIN_LED_BUILTIN, LOW);
  }
  
  // CRÍTICO: Alimentar el watchdog y dar tiempo al WiFi stack
  yield();
  delay(1);
}