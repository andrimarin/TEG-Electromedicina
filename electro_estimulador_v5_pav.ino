// ============================================================
// ELECTROTERAPIA TENS - CON WiFiManager + Recetas JSON
// Configuración WiFi desde el celular (sin hardcodear)
// Pin de salida: GPIO23 (PWM)
// Controla: INTENSIDAD (0-255), FRECUENCIA y ANCHO DE PULSO
// Recetas con ciclos trabajo/pausa. Al finalizar → 0V garantizado.
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>      // Configuración WiFi fácil
#include <ArduinoJson.h>      // Parseo de recetas JSON
#include <HTTPClient.h> // Asegúrate de incluirla al inicio

// ================== PINES ==================
const int PIN_TENS = 23;        // GPIO23 - Salida PWM al optoacoplador/MOSFET
const int PIN_LED_BUILTIN = 2;  // LED integrado del ESP32
const int PIN_BOOT = 0;

const int FISICO_ENCENDIDO = LOW;  // LOW activa el optoacoplador -> Enciende MOSFET
const int FISICO_APAGADO = HIGH;   // HIGH apaga el optoacoplador -> Apaga MOSFET

// ================== MÁQUINA DE ESTADOS ==================
enum EstadoTerapia {
  ESTADO_IDLE,        // Sin terapia, electrodo apagado
  ESTADO_TRABAJANDO,  // Ciclo activo: generando pulsos
  ESTADO_PAUSA,       // Pausa entre ciclos: electrodo apagado
  ESTADO_FINALIZADO   // Receta completada: electrodo apagado
};

EstadoTerapia estadoActual = ESTADO_IDLE;

// ================== PARÁMETROS ==================
int intensidad = 0;             // 0-255 (amplitud del PWM durante pulso)
int frecuenciaHz = 50;          // Hz - de la receta
float anchoPulsoMs = 0.5;       // ms - ancho del pulso de la receta
bool terapiaActiva = false;     // Flag global de terapia en curso

// ================== RECETA ==================
#define MAX_CICLOS 10
struct Ciclo {
  unsigned long trabajoSeg;     // Segundos de estimulación
  unsigned long pausaSeg;       // Segundos de pausa
  // inicio_at se podría usar con NTP en el futuro
};

String recetaId = "";
int numCiclos = 0;
Ciclo ciclos[MAX_CICLOS];
int cicloActualIdx = 0;          // Índice del ciclo en ejecución
unsigned long inicioFaseMs = 0;  // Millis al inicio de la fase actual

// ================== GENERACIÓN DE ONDA ==================
unsigned long ultimoPulso = 0;   // Microsegundos
bool pulsoEncendido = false;

WebServer server(80);

// ================== CONFIGURACIÓN PWM ==================
void configurarPWM() {
  //ledcAttach(PIN_TENS, frecuenciaHz, 8);
  //ledcWrite(PIN_TENS, 0);
  pinMode(PIN_TENS, OUTPUT);
  digitalWrite(PIN_TENS, FISICO_APAGADO);
}

void actualizarPWM(int valor) {
  ledcWrite(PIN_TENS, valor);
}

// ================== APAGADO GARANTIZADO ==================
// Doble verificación: apaga PWM Y pone el pin en LOW

void apagarElectrodoTotal() {
  // En lógica inversa, 255 de duty cycle es el estado de menor energía
  // ledcWrite(PIN_TENS, 255); 
  pulsoEncendido = false;
  // Forzar estado físico HIGH (MOSFET bloqueado)
  digitalWrite(PIN_TENS, FISICO_APAGADO); 
  
  Serial.println("⚡ Estado Seguro: Pin en HIGH (MOSFET bloqueado)"); 
}


// ================== GENERACIÓN DE ONDA (por receta) ==================
// Solo genera pulsos cuando estadoActual == ESTADO_TRABAJANDO
// Usa frecuenciaHz y anchoPulsoMs de la receta
 void generarOnda() {
  if (estadoActual != ESTADO_TRABAJANDO || intensidad == 0) {
    if (pulsoEncendido) apagarElectrodoTotal();
    return;
  }

  // Protección contra división por cero
  if (frecuenciaHz < 1) frecuenciaHz = 1;
  if (frecuenciaHz > 150) frecuenciaHz = 150;

  unsigned long ahora = micros();
  unsigned long periodoUs = 1000000UL / frecuenciaHz; 
  
  // MAPEO DINÁMICO con mejor precisión
  float anchoAjustadoMs = (anchoPulsoMs * intensidad) / 255.0f;
  unsigned long anchoPulsoUs = (unsigned long)(anchoAjustadoMs * 1000.0f + 0.5f); // +0.5 para redondeo
  
  // Validaciones de seguridad
  if (anchoPulsoUs < 10) anchoPulsoUs = 10;  // Mínimo 10µs
  if (anchoPulsoUs > (periodoUs * 8) / 10) { // Máximo 80% del periodo
    anchoPulsoUs = (periodoUs * 8) / 10;
  }
  
  unsigned long tiempoOffUs = periodoUs - anchoPulsoUs;
  if (tiempoOffUs < 10) tiempoOffUs = 10;  // Mínimo tiempo OFF

  if (pulsoEncendido) {
    if ((ahora - ultimoPulso) >= anchoPulsoUs) {
      digitalWrite(PIN_TENS, FISICO_APAGADO); 
      pulsoEncendido = false;
      ultimoPulso = ahora;
    }
  } else {
    if ((ahora - ultimoPulso) >= tiempoOffUs) {
      digitalWrite(PIN_TENS, FISICO_ENCENDIDO); 
      pulsoEncendido = true;
      ultimoPulso = ahora;
    }
  }
}

// ================== MÁQUINA DE ESTADOS DE TERAPIA ==================
void gestionarReceta() {
  if (estadoActual == ESTADO_IDLE || estadoActual == ESTADO_FINALIZADO) {
    return;  // Nada que hacer
  }
  
  unsigned long tiempoEnFaseMs = millis() - inicioFaseMs;
  
  if (estadoActual == ESTADO_TRABAJANDO) {
    unsigned long duracionTrabajoMs = ciclos[cicloActualIdx].trabajoSeg * 1000UL;
    
    if (tiempoEnFaseMs >= duracionTrabajoMs) {
      // Terminó el tiempo de trabajo de este ciclo
      apagarElectrodoTotal();
      
      // ¿Hay pausa en este ciclo?
      if (ciclos[cicloActualIdx].pausaSeg > 0) {
        estadoActual = ESTADO_PAUSA;
        inicioFaseMs = millis();
        Serial.print("⏸ Pausa ciclo ");
        Serial.print(cicloActualIdx + 1);
        Serial.print("/");
        Serial.print(numCiclos);
        Serial.print(" - ");
        Serial.print(ciclos[cicloActualIdx].pausaSeg);
        Serial.println("s");
      } else {
        // Sin pausa, ir al siguiente ciclo o finalizar
        avanzarAlSiguienteCiclo();
      }
    }
  }
  else if (estadoActual == ESTADO_PAUSA) {
    unsigned long duracionPausaMs = ciclos[cicloActualIdx].pausaSeg * 1000UL;
    
    if (tiempoEnFaseMs >= duracionPausaMs) {
      avanzarAlSiguienteCiclo();
    }
  }
}

void avanzarAlSiguienteCiclo() {
  cicloActualIdx++;
  
  if (cicloActualIdx >= numCiclos) {
    // ====== RECETA COMPLETADA: APAGADO TOTAL ======
    finalizarTerapia();
  } else {
    // Iniciar siguiente ciclo de trabajo
    estadoActual = ESTADO_TRABAJANDO;
    inicioFaseMs = millis();
    Serial.print("▶ Ciclo ");
    Serial.print(cicloActualIdx + 1);
    Serial.print("/");
    Serial.println(numCiclos);
  }
}

void iniciarReceta() {
  if (numCiclos == 0) {
    Serial.println("⚠️ No hay receta cargada");
    return;
  }
  if (intensidad == 0) {
    Serial.println("⚠️ Intensidad en 0, no se inicia");
    return;
  }
  
  cicloActualIdx = 0;
  estadoActual = ESTADO_TRABAJANDO;
  terapiaActiva = true;
  inicioFaseMs = millis();
  ultimoPulso = micros();
  
  Serial.println("=== RECETA INICIADA ===");
  Serial.print("Receta: ");
  Serial.println(recetaId);
  Serial.print("Ciclos: ");
  Serial.println(numCiclos);
  Serial.print("Frecuencia: ");
  Serial.print(frecuenciaHz);
  Serial.println(" Hz");
  Serial.print("Ancho pulso: ");
  Serial.print(anchoPulsoMs);
  Serial.println(" ms");
  Serial.print("Intensidad: ");
  Serial.println(intensidad);
}

void finalizarTerapia() {
  estadoActual = ESTADO_FINALIZADO;
  terapiaActiva = false;
  intensidad = 0;
  apagarElectrodoTotal(); // Esta función ya pone el pin en HIGH
  
  // Triple seguridad: NUNCA pongas LOW aquí
  delay(1);
  actualizarPWM(255); // 255 es apagado en lógica inversa
  digitalWrite(PIN_TENS, FISICO_APAGADO); // Debe ser HIGH
  
  Serial.println("=== RECETA FINALIZADA ===");
  Serial.println("⚡ Electrodo APAGADO - 0V confirmado");
}



void detenerTerapiaManual() {
  estadoActual = ESTADO_IDLE;
  terapiaActiva = false;
  apagarElectrodoTotal();
  Serial.println("=== TERAPIA DETENIDA MANUALMENTE ===");
}

void emergenciaTotal() {
  estadoActual = ESTADO_IDLE;
  terapiaActiva = false;
  intensidad = 0;
  apagarElectrodoTotal();
  
  // Seguridad extra: Asegurar el estado HIGH
  delay(1);
  // actualizarPWM(255); 
  digitalWrite(PIN_TENS, FISICO_APAGADO); // Corregido: de LOW a HIGH
  Serial.println("!!! PARADA DE EMERGENCIA ACTIVADA !!!"); 
}

// ================== INTERFAZ WEB ==================
String getHTML() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=yes">
  <title>TENS - Electroterapia por Receta</title>
  <style>
    * { box-sizing: border-box; user-select: none; }
    body {
      font-family: 'Segoe UI', Roboto, sans-serif;
      background: linear-gradient(135deg, #0a0f1e 0%, #0a1a2a 100%);
      min-height: 100vh;
      margin: 0;
      padding: 20px;
      display: flex;
      justify-content: center;
      align-items: center;
    }
    .container { max-width: 550px; width: 100%; margin: auto; }
    .card {
      background: rgba(22, 34, 48, 0.95);
      backdrop-filter: blur(10px);
      border-radius: 32px;
      padding: 24px 20px 32px;
      margin-bottom: 20px;
      box-shadow: 0 20px 35px -10px rgba(0,0,0,0.5);
      border: 1px solid rgba(255,255,255,0.08);
    }
    h1 {
      font-size: 1.8rem; margin: 0 0 8px 0; font-weight: 600;
      background: linear-gradient(135deg, #aaffdd, #2ecc71);
      -webkit-background-clip: text; background-clip: text;
      color: transparent; text-align: center;
    }
    .estado-panel {
      background: #07121c; border-radius: 48px;
      padding: 12px 20px; text-align: center;
      margin-bottom: 20px; border: 1px solid #2a4a6e;
    }
    .estado-led {
      display: inline-block; width: 14px; height: 14px;
      border-radius: 14px; background: #555; margin-right: 8px;
    }
    .estado-texto { font-weight: bold; letter-spacing: 1px; color: #ccc; }
    .activo .estado-led { background: #2ecc71; box-shadow: 0 0 8px #2ecc71; }
    .activo .estado-texto { color: #2ecc71; }
    .pausa .estado-led { background: #f39c12; box-shadow: 0 0 8px #f39c12; }
    .pausa .estado-texto { color: #f39c12; }
    .finalizado .estado-led { background: #3498db; box-shadow: 0 0 8px #3498db; }
    .finalizado .estado-texto { color: #3498db; }
    .inactivo .estado-led { background: #e74c3c; }
    .inactivo .estado-texto { color: #e74c3c; }

    .param-card {
      background: #0a1824; border-radius: 28px;
      padding: 16px 20px; margin-bottom: 16px;
    }
    .param-label {
      display: flex; justify-content: space-between;
      font-weight: 600; margin-bottom: 12px; color: #c0e0ff;
    }
    input[type=range] {
      width: 100%; height: 6px; -webkit-appearance: none;
      background: #2c4c6c; border-radius: 10px; outline: none;
    }
    input[type=range]::-webkit-slider-thumb {
      -webkit-appearance: none; width: 24px; height: 24px;
      background: #2ecc71; border-radius: 50%; cursor: pointer;
      box-shadow: 0 0 8px #2ecc71; border: none;
    }
    textarea {
      width: 100%; background: #071723; color: #aaffdd;
      border: 1px solid #2a4a6e; border-radius: 16px;
      padding: 12px; font-family: monospace; font-size: 0.8rem;
      resize: vertical; min-height: 120px;
    }
    .botonera {
      display: flex; gap: 12px; justify-content: center; margin: 20px 0 10px;
    }
    button {
      border: none; padding: 14px 24px; border-radius: 60px;
      font-weight: bold; font-size: 1rem; background: #1f3e54;
      color: white; cursor: pointer; transition: 0.2s; flex: 1;
    }
    .btn-cargar { background: #3498db; color: #fff; }
    .btn-iniciar { background: #2ecc71; color: #000; }
    .btn-detener { background: #e74c3c; }
    .btn-emergencia { background: #c0392b; border: 1px solid #ff8866; width: 100%; }
    .info-receta {
      background: #07121c; border-radius: 20px; padding: 14px;
      margin-bottom: 16px; font-size: 0.85rem; color: #8aaec0;
    }
    .info-receta b { color: #aaffdd; }
    .progreso-bar {
      background: #1a2a3a; border-radius: 10px; height: 8px;
      margin-top: 10px; overflow: hidden;
    }
    .progreso-bar-fill {
      background: linear-gradient(90deg, #2ecc71, #27ae60);
      height: 100%; width: 0%; transition: width 0.5s;
      border-radius: 10px;
    }
    footer {
      text-align: center; font-size: 0.7rem; color: #2c5a7a; margin-top: 15px;
    }
    .badge {
      background: #0e2a3a; border-radius: 24px;
      padding: 6px 12px; font-size: 0.75rem; display: inline-block;
    }
    .ip-info {
      text-align: center; font-size: 0.7rem; color: #5a9ec2; margin-top: 10px;
    }
  </style>
</head>
<body>
<div class="container">
  <div class="card">
    <h1>⚡ TENS · Teleterapia</h1>
    <div style="text-align:center;margin-bottom:16px"><span class="badge">📡 Control WiFi · Recetas JSON</span></div>

    <div id="estadoWidget" class="estado-panel inactivo">
      <span class="estado-led"></span>
      <span class="estado-texto" id="estadoTexto">SISTEMA INACTIVO</span>
    </div>

    <!-- Info de receta cargada -->
    <div class="info-receta" id="infoReceta">
      📋 <b>Sin receta cargada</b> — Pega un JSON abajo y presiona "Cargar"
    </div>
    <div class="progreso-bar"><div class="progreso-bar-fill" id="progresoFill"></div></div>

    <!-- Editor de receta JSON -->
    <div class="param-card" style="margin-top:16px">
      <div class="param-label"><span>📝 RECETA JSON</span></div>
      <textarea id="recetaJson">{
  "receta_id": "terapia_01",
  "num_ciclos": 1,
  "ciclos": [
    {"trabajo_seg": 300, "pausa_seg": 60}
  ],
  "parametros_pulso": {
    "frecuencia_hz": 50,
    "ancho_pulso_ms": 0.5
  }
}</textarea>
      <div style="margin-top:10px;text-align:center">
        <button class="btn-cargar" id="btnCargar" style="flex:none;width:auto;padding:10px 30px">📤 Cargar Receta</button>
      </div>
    </div>

    <!-- Intensidad (amplitud del pulso, ajustable por el usuario) -->
    <div class="param-card">
      <div class="param-label">
        <span>�️ INTENSIDAD (amplitud)</span>
        <span id="intensidadValor">0</span>
      </div>
      <input type="range" id="intensidadSlider" min="0" max="255" value="0">
      <div style="font-size:12px; opacity:0.7">0 = apagado &nbsp;&nbsp; 255 = máximo</div>
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
    <footer>Electroterapia TENS · Control remoto por WiFi · Recetas JSON</footer>
  </div>
</div>

<script>
  const intensidadSlider = document.getElementById('intensidadSlider');
  const intensidadValor = document.getElementById('intensidadValor');
  const estadoTerapiaSpan = document.getElementById('estadoTerapia');
  const feedbackDiv = document.getElementById('feedback');
  const estadoWidget = document.getElementById('estadoWidget');
  const estadoTexto = document.getElementById('estadoTexto');
  const infoReceta = document.getElementById('infoReceta');
  const progresoFill = document.getElementById('progresoFill');
  const recetaJsonArea = document.getElementById('recetaJson');

  let terapiaEncendida = false;
  let recetaCargada = false;

  // ---- Intensidad en tiempo real ----
  intensidadSlider.oninput = () => {
    intensidadValor.innerText = intensidadSlider.value;
    enviarIntensidad(intensidadSlider.value);
  };

  async function enviarIntensidad(val) {
    try { await fetch('/set_intensidad?v=' + val); } catch(e) { console.log(e); }
  }

  // ---- Cargar receta JSON ----
  async function cargarReceta() {
    const txt = recetaJsonArea.value.trim();
    try {
      JSON.parse(txt);  // Validar JSON en el cliente
    } catch(e) {
      feedbackDiv.innerHTML = "❌ JSON inválido: " + e.message;
      return;
    }
    try {
      const resp = await fetch('/receta', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: txt
      });
      if (resp.ok) {
        const data = await resp.json();
        recetaCargada = true;
        infoReceta.innerHTML = "📋 <b>" + data.receta_id + "</b> — " +
          data.num_ciclos + " ciclo(s) · " +
          data.frecuencia_hz + " Hz · " +
          data.ancho_pulso_ms + " ms pulso";
        feedbackDiv.innerHTML = "✅ Receta cargada correctamente";
      } else {
        feedbackDiv.innerHTML = "❌ Error al cargar receta";
      }
    } catch(e) {
      feedbackDiv.innerHTML = "❌ Error de conexión: " + e.message;
    }
  }

  // ---- Iniciar terapia ----
  async function iniciarTerapia() {
    const intensidad = intensidadSlider.value;
    if (!recetaCargada) {
      feedbackDiv.innerHTML = "⚠️ Primero carga una receta JSON";
      return;
    }
    if (parseInt(intensidad) === 0) {
      feedbackDiv.innerHTML = "⚠️ La intensidad está en 0, sube el nivel antes de iniciar.";
      return;
    }
    const resp = await fetch('/iniciar?int=' + intensidad);
    if (resp.ok) {
      terapiaEncendida = true;
      feedbackDiv.innerHTML = "🟢 Terapia en curso";
    }
  }

  // ---- Detener ----
  async function detenerTerapia() {
    await fetch('/detener');
    terapiaEncendida = false;
    feedbackDiv.innerHTML = "⚫ Terapia detenida";
    await actualizarEstadoCompleto();
  }

  // ---- Emergencia ----
  async function emergencia() {
    await fetch('/emergencia');
    terapiaEncendida = false;
    intensidadSlider.value = 0;
    intensidadValor.innerText = "0";
    feedbackDiv.innerHTML = "🛑 PARADA DE EMERGENCIA ACTIVADA";
    await actualizarEstadoCompleto();
  }

  // ---- Polling de estado ----
  async function actualizarEstadoCompleto() {
    try {
      const res = await fetch('/estado_completo');
      const d = await res.json();
      intensidadSlider.value = d.intensidad;
      intensidadValor.innerText = d.intensidad;
      terapiaEncendida = d.activa;

      // Estado visual
      const est = d.estado; // "idle","trabajando","pausa","finalizado"
      if (est === "trabajando") {
        estadoWidget.className = "estado-panel activo";
        estadoTexto.innerText = "TERAPIA ACTIVA";
        estadoTerapiaSpan.innerText = "▶ ACTIVA";
        estadoTerapiaSpan.style.color = "#2ecc71";
      } else if (est === "pausa") {
        estadoWidget.className = "estado-panel pausa";
        estadoTexto.innerText = "PAUSA ENTRE CICLOS";
        estadoTerapiaSpan.innerText = "⏸ PAUSA";
        estadoTerapiaSpan.style.color = "#f39c12";
      } else if (est === "finalizado") {
        estadoWidget.className = "estado-panel finalizado";
        estadoTexto.innerText = "RECETA COMPLETADA";
        estadoTerapiaSpan.innerText = "✅ FINALIZADA";
        estadoTerapiaSpan.style.color = "#3498db";
      } else {
        estadoWidget.className = "estado-panel inactivo";
        estadoTexto.innerText = "SISTEMA INACTIVO";
        estadoTerapiaSpan.innerText = "⏹ INACTIVA";
        estadoTerapiaSpan.style.color = "#ffaa66";
      }

      // Progreso
      if (d.progreso !== undefined) {
        progresoFill.style.width = d.progreso + "%";
      }

      // Info del ciclo
      if (d.ciclo_actual !== undefined && d.num_ciclos !== undefined && d.activa) {
        infoReceta.innerHTML = "📋 <b>" + (d.receta_id || "Receta") + "</b> — Ciclo " +
          d.ciclo_actual + "/" + d.num_ciclos + " · " +
          d.frecuencia_hz + " Hz · " + d.ancho_pulso_ms + " ms";
      }

      if (recetaCargada && !d.activa && d.receta_id) {
        // Mantener info de receta cargada
      }
    } catch(e) { console.log("error sync", e); }
  }

  // Mostrar IP
  fetch('/ip').then(r=>r.text()).then(ip => {
    document.getElementById('ipDisplay').innerHTML = '🌐 ' + ip;
  });

  document.getElementById('btnCargar').onclick = cargarReceta;
  document.getElementById('btnIniciar').onclick = iniciarTerapia;
  document.getElementById('btnDetener').onclick = detenerTerapia;
  document.getElementById('btnEmergencia').onclick = emergencia;

  setInterval(actualizarEstadoCompleto, 1500);
  actualizarEstadoCompleto();
</script>
</body>
</html>
)rawliteral";
  return html;
}

// ================== CONFIGURACIÓN DE SERVIDOR WEB ==================
void setupServer() {
  server.on("/", []() {
    server.send(200, "text/html", getHTML());
  });
  
  server.on("/ip", []() {
    server.send(200, "text/plain", WiFi.localIP().toString());
  });
  
  // Endpoint para cambiar intensidad en tiempo real
  server.on("/set_intensidad", []() {
    if (server.hasArg("v")) {
      intensidad = server.arg("v").toInt();
      if (intensidad < 0) intensidad = 0;
      if (intensidad > 255) intensidad = 255;
      Serial.print("Intensidad: ");
      Serial.println(intensidad);
      server.send(200, "text/plain", "OK");
    }
  });
  
  // Endpoint para cargar receta JSON (POST)
  server.on("/receta", HTTP_POST, []() {
    String body = server.arg("plain");
    Serial.println("Receta recibida:");
    Serial.println(body);
    
    // Parsear JSON con ArduinoJson
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
      Serial.print("Error JSON: ");
      Serial.println(error.c_str());
      server.send(400, "application/json", "{\"error\":\"JSON invalido\"}");
      return;
    }
    
    // Extraer datos de la receta
    recetaId = doc["receta_id"] | "sin_id";
    numCiclos = doc["num_ciclos"] | 0;
    if (numCiclos > MAX_CICLOS) numCiclos = MAX_CICLOS;
    
    // Parámetros de pulso
    frecuenciaHz = doc["parametros_pulso"]["frecuencia_hz"] | 50;
    anchoPulsoMs = doc["parametros_pulso"]["ancho_pulso_ms"] | 0.5;
    
    // Validaciones
    if (frecuenciaHz < 1) frecuenciaHz = 1;
    if (frecuenciaHz > 150) frecuenciaHz = 150;
    if (anchoPulsoMs < 0.01) anchoPulsoMs = 0.01;
    if (anchoPulsoMs > 10.0) anchoPulsoMs = 10.0;
    
    // Cargar ciclos
    JsonArray ciclosArr = doc["ciclos"].as<JsonArray>();
    int idx = 0;
    for (JsonObject c : ciclosArr) {
      if (idx >= numCiclos) break;
      ciclos[idx].trabajoSeg = c["trabajo_seg"] | 0;
      ciclos[idx].pausaSeg = c["pausa_seg"] | 0;
      idx++;
    }
    
    // Si num_ciclos es mayor que los ciclos definidos, ajustar
    if (idx < numCiclos) numCiclos = idx;
    
    Serial.print("Receta cargada: ");
    Serial.println(recetaId);
    Serial.print("Ciclos: ");
    Serial.println(numCiclos);
    for (int i = 0; i < numCiclos; i++) {
        Serial.print("  Ciclo ");
        Serial.print(i + 1);
        Serial.print(": trabajo=");
        Serial.print(ciclos[i].trabajoSeg);
        Serial.print("s, pausa=");
        Serial.print(ciclos[i].pausaSeg);
        Serial.println("s");
    }
    Serial.print("Frecuencia: ");
    Serial.print(frecuenciaHz);
    Serial.println(" Hz");
    Serial.print("Ancho pulso: ");
    Serial.print(anchoPulsoMs);
    Serial.println(" ms");
    
    // Responder con confirmación
    String resp = "{";
    resp += "\"ok\":true,";
    resp += "\"receta_id\":\"" + recetaId + "\",";
    resp += "\"num_ciclos\":" + String(numCiclos) + ",";
    resp += "\"frecuencia_hz\":" + String(frecuenciaHz) + ",";
    resp += "\"ancho_pulso_ms\":" + String(anchoPulsoMs, 2);
    resp += "}";
    server.send(200, "application/json", resp);
  });
  
  // Endpoint para iniciar terapia (usa la receta cargada)
  server.on("/iniciar", []() {
    if (server.hasArg("int")) {
      intensidad = server.arg("int").toInt();
      if (intensidad < 0) intensidad = 0;
      if (intensidad > 255) intensidad = 255;
    }
    if (numCiclos == 0) {
      server.send(400, "text/plain", "No hay receta cargada");
      return;
    }
    iniciarReceta();
    server.send(200, "text/plain", "OK");
  });
  
  // Endpoint para detener terapia
  server.on("/detener", []() {
    detenerTerapiaManual();
    server.send(200, "text/plain", "OK");
  });
  
  // Endpoint para emergencia (detiene todo y pone intensidad 0)
  server.on("/emergencia", []() {
    emergenciaTotal();
    server.send(200, "text/plain", "OK");
  });
  
  // Endpoint para obtener estado completo
  server.on("/estado_completo", []() {
    // Calcular progreso
    float progreso = 0;
    if (numCiclos > 0 && (estadoActual == ESTADO_TRABAJANDO || estadoActual == ESTADO_PAUSA)) {
      // Tiempo total de la receta
      unsigned long totalSeg = 0;
      unsigned long transcurridoSeg = 0;
      for (int i = 0; i < numCiclos; i++) {
        totalSeg += ciclos[i].trabajoSeg + ciclos[i].pausaSeg;
      }
      // Tiempo transcurrido: ciclos completados + fase actual
      for (int i = 0; i < cicloActualIdx; i++) {
        transcurridoSeg += ciclos[i].trabajoSeg + ciclos[i].pausaSeg;
      }
      unsigned long enFaseSeg = (millis() - inicioFaseMs) / 1000;
      if (estadoActual == ESTADO_TRABAJANDO) {
        transcurridoSeg += enFaseSeg;
      } else if (estadoActual == ESTADO_PAUSA) {
        transcurridoSeg += ciclos[cicloActualIdx].trabajoSeg + enFaseSeg;
      }
      if (totalSeg > 0) progreso = (float)transcurridoSeg / totalSeg * 100.0;
      if (progreso > 100) progreso = 100;
    } else if (estadoActual == ESTADO_FINALIZADO) {
      progreso = 100;
    }
    
    // Nombre del estado
    String estadoStr = "idle";
    if (estadoActual == ESTADO_TRABAJANDO) estadoStr = "trabajando";
    else if (estadoActual == ESTADO_PAUSA) estadoStr = "pausa";
    else if (estadoActual == ESTADO_FINALIZADO) estadoStr = "finalizado";
    
    String json = "{";
    json += "\"activa\":" + String(terapiaActiva ? "true" : "false") + ",";
    json += "\"estado\":\"" + estadoStr + "\",";
    json += "\"intensidad\":" + String(intensidad) + ",";
    json += "\"frecuencia_hz\":" + String(frecuenciaHz) + ",";
    json += "\"ancho_pulso_ms\":" + String(anchoPulsoMs, 2) + ",";
    json += "\"receta_id\":\"" + recetaId + "\",";
    json += "\"ciclo_actual\":" + String(cicloActualIdx + 1) + ",";
    json += "\"num_ciclos\":" + String(numCiclos) + ",";
    json += "\"progreso\":" + String(progreso, 1);
    json += "}";
    server.send(200, "application/json", json);
  });
  
  server.begin();
  Serial.println("✅ Servidor web iniciado");
}

void descargarRecetaServidor() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    // URL de tu servidor (ejemplo: un .json en GitHub o tu propia API)
    String url = "https://tesis.testbackup.online/receta.json";
    
    http.begin(url);
    int httpCode = http.GET();
    
    if (httpCode > 0) {
      String payload = http.getString();
      Serial.println("Receta descargada:");
      Serial.println(payload);
      
      // Reutilizamos la lógica que ya tienes para procesar el JSON
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      
      if (!error) {
        // Extraer datos (Copia la lógica que tienes en server.on("/receta"))
        recetaId = doc["receta_id"] | "externa"; 
        numCiclos = doc["num_ciclos"] | 0; 
        frecuenciaHz = doc["parametros_pulso"]["frecuencia_hz"] | 50; 
        anchoPulsoMs = doc["parametros_pulso"]["ancho_pulso_ms"] | 0.5; 

        // Cargar ciclos al array
        JsonArray ciclosArr = doc["ciclos"].as<JsonArray>(); 
        int idx = 0;
        for (JsonObject c : ciclosArr) {
          if (idx >= MAX_CICLOS) break;
          ciclos[idx].trabajoSeg = c["trabajo_seg"] | 0; 
          ciclos[idx].pausaSeg = c["pausa_seg"] | 0;
          idx++;
        }
        numCiclos = idx;
        Serial.println("✅ Receta sincronizada con éxito.");
      }
    } else {
      Serial.printf("❌ Error en HTTP: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  }
}


// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=================================");
  Serial.println("⚡ ELECTROTERAPIA TENS - WiFiManager");
  Serial.println("=================================");
  
  // Configurar pines
  digitalWrite(PIN_TENS, FISICO_APAGADO);
  pinMode(PIN_TENS, OUTPUT);
  pinMode(PIN_BOOT, INPUT_PULLUP);  // Botón BOOT con pull-up interno

  
  // Configurar PWM en GPIO23
  ledcAttach(PIN_TENS, frecuenciaHz, 8);
  ledcWrite(PIN_TENS, 255);
  // DETECTA SI EL WIFI MANAGER ESTA ACTIVO
  bool forzarConfigWiFi = false;

  Serial.println("🔘 Mantén presionado BOOT (3 seg) para reconfigurar WiFi...");  

   // Parpadeo rápido del LED durante 3 segundos para indicar ventana de decisión
  unsigned long inicioVentana = millis();
  while (millis() - inicioVentana < 3000) {
    if (digitalRead(PIN_BOOT) == LOW) {  // Botón presionado = LOW
      forzarConfigWiFi = true;
      break;
    }
    // Parpadeo rápido para indicar que está esperando
    digitalWrite(PIN_LED_BUILTIN, (millis() / 150) % 2);
    delay(50);
  }
  digitalWrite(PIN_LED_BUILTIN, LOW);

  // ========== WIFIMANAGER - Configuración WiFi sin hardcodear ==========
  WiFiManager wifiManager;
  
  
  // Configurar timeout (si no se configura en 3 minutos, el ESP32 se reinicia)
  wifiManager.setConfigPortalTimeout(180);

  if (forzarConfigWiFi) {
    // ---- MODO RECONFIGURACIÓN: Borrar red guardada y abrir portal ----
    Serial.println("🔄 RECONFIGURACIÓN WiFi solicitada");
    Serial.println("   Borrando red guardada...");
    wifiManager.resetSettings();  // Borra SSID/password almacenados
    
    // LED encendido fijo = modo configuración
    digitalWrite(PIN_LED_BUILTIN, HIGH);
    
    Serial.println("📡 Portal de configuración abierto:");
    Serial.println("   1. Conecta tu celular a la red 'TENS_Config_WiFi'");
    Serial.println("   2. Abre 192.168.4.1 en el navegador");
    Serial.println("   3. Selecciona tu red WiFi e ingresa la contraseña");
    
    if (!wifiManager.startConfigPortal("TENS_Config_WiFi")) {
      Serial.println("❌ Tiempo agotado. Reiniciando...");
      delay(1000);
      ESP.restart();
    }
  } else {
    // ---- MODO NORMAL: Intentar conectar a la red guardada ----
    Serial.println("📶 Conectando a red WiFi guardada...");
    
    if (!wifiManager.autoConnect("TENS_Config_WiFi")) {
      Serial.println("❌ Error de conexión. Reiniciando...");
      delay(1000);
      ESP.restart();
    }
  }

  // Si llegamos aquí, estamos conectados
  Serial.println("✅ Conectado a WiFi!");
  Serial.print("📡 IP del ESP32: ");
  Serial.println(WiFi.localIP());
  Serial.print("📶 Red: ");
  Serial.println(WiFi.SSID());

  descargarRecetaServidor(); 
  
  // Parpadeo para indicar que está listo
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(PIN_LED_BUILTIN, LOW);
    delay(200);
  }
  
  // Iniciar servidor web
  setupServer();
  
  Serial.println("=================================");
  Serial.print("🌐 Abre en tu navegador: http://");
  Serial.println(WiFi.localIP());
  Serial.println("💡 Para reconfigurar WiFi: reinicia con BOOT presionado");
  Serial.println("=================================");
}

// ================== LOOP ==================
void loop() {
  server.handleClient();
  
  // Gestionar máquina de estados de la receta (trabajo/pausa/fin)
  gestionarReceta();
  
  // Generar onda solo si estamos en fase de trabajo
  generarOnda();

  // Heartbeat solo si hay terapia activa Y conexión estable
  static unsigned long lastHeartbeat = 0;
  if (terapiaActiva && WiFi.status() == WL_CONNECTED && 
      millis() - lastHeartbeat > 3000) { // 3s en lugar de 2s
    lastHeartbeat = millis();
    checkHeartbeat(); 
  }
  
  // LED indicador
  static unsigned long lastBlink = 0;
  if (estadoActual == ESTADO_TRABAJANDO && intensidad > 0) {
    // Parpadeo rápido: terapia activa
    if (millis() - lastBlink > 300) {
      lastBlink = millis();
      digitalWrite(PIN_LED_BUILTIN, !digitalRead(PIN_LED_BUILTIN));
    }
  } else if (estadoActual == ESTADO_PAUSA) {
    // Parpadeo lento: en pausa
    if (millis() - lastBlink > 1000) {
      lastBlink = millis();
      digitalWrite(PIN_LED_BUILTIN, !digitalRead(PIN_LED_BUILTIN));
    }
  } else {
    digitalWrite(PIN_LED_BUILTIN, LOW);
  }
  
  delay(5);
}

void checkHeartbeat() {
  HTTPClient http;
  http.setTimeout(2000); // Timeout corto para no bloquear
  http.begin("https://tesis.testbackup.online/api/heartbeat");
  
  int httpCode = http.GET();
  if (httpCode == 200) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getString());
    
    if (!error) {
      // Solo actualizar si los valores son válidos y diferentes
      float newPulseWidth = doc["pulse_width_ms"] | -1.0f;
      int newIntensity = doc["intensity"] | -1;
      
      if (newPulseWidth > 0 && newPulseWidth != anchoPulsoMs) {
        anchoPulsoMs = newPulseWidth;
        Serial.println("📡 Ancho de pulso actualizado: " + String(anchoPulsoMs) + "ms");
      }
      
      if (newIntensity >= 0 && newIntensity <= 255 && newIntensity != intensidad) {
        intensidad = newIntensity;
        Serial.println("📡 Intensidad actualizada: " + String(intensidad));
      }
      
      String cmd = doc["command"] | "NONE";
      if (cmd == "EMERGENCY_STOP") {
        Serial.println("🛑 PARADA DE EMERGENCIA REMOTA");
        emergenciaTotal();
      }
    }
  } else if (httpCode > 0) {
    Serial.println("⚠️ Heartbeat error: " + String(httpCode));
  }
  // Si falla silenciosamente, continúa con parámetros locales
  
  http.end();
}
