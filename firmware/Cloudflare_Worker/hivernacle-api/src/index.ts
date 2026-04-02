export interface Env {
	DB: D1Database;
	ASSETS: Fetcher;
}

const corsHeaders = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET,HEAD,POST,OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, X-Auth-Token",
};

const NTFY_TOPIC = "hivernacle-iot-gussi";

async function enviarAlerta(missatge: string, tag: string = "warning") {
	await fetch(`https://ntfy.sh/${NTFY_TOPIC}`, {
		method: "POST",
		body: missatge,
		headers: { "Tags": tag, "Title": "Alerta Hivernacle" }
	});
}

// ── HELPER: Inserir un log a la BBDD ─────────────────────────────────────────
async function insertLog(env: Env, device_id: string, missatge: string, origen: string = 'hivernacle') {
	await env.DB.prepare(
		"INSERT INTO logs (device_id, missatge, origen) VALUES (?, ?, ?)"
	).bind(device_id, missatge, origen).run();
}

export default {
	async fetch(request: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
		const url = new URL(request.url);

        if (request.method === "OPTIONS") {
            return new Response(null, { headers: corsHeaders });
        }

		// --- LOGIN WEB ---
		if (url.pathname === '/api/login' && request.method === 'POST') {
            try {
                const { device_id, password } = await request.json();
                const user = await env.DB.prepare(
                    "SELECT device_id, propietari FROM hivernacles WHERE device_id = ? AND password_web = ?"
                ).bind(device_id, password).first();
                if (!user) {
                    return new Response(JSON.stringify({ error: "Credencials incorrectes" }), { status: 401, headers: corsHeaders });
                }
                return new Response(JSON.stringify(user), { headers: { "Content-Type": "application/json", ...corsHeaders } });
            } catch (e) { return new Response("Error Login", { status: 500, headers: corsHeaders }); }
		}

        // --- PUJAR DADES (ESP32 -> NÚVOL) ---
        if (url.pathname === '/api/upload' && request.method === 'POST') {
            try {
                const token = request.headers.get("X-Auth-Token");
                const data: any = await request.json();
                
                const config = await env.DB.prepare(
                    "SELECT * FROM hivernacles WHERE device_id = ? AND api_token = ?"
                ).bind(data.device_id, token).first();

                if (!config) return new Response("Error: Token ESP32 Invàlid", { status: 403 });

                await env.DB.prepare(
                    "INSERT INTO lectures (device_id, temp, hum, hum_sol, llum) VALUES (?, ?, ?, ?, ?)"
                ).bind(data.device_id, data.temp, data.hum, data.soil || 0, data.light || 0).run();

                await env.DB.prepare(
                    "UPDATE hivernacles SET ultima_connexio = CURRENT_TIMESTAMP WHERE device_id = ?"
                ).bind(data.device_id).run();

                // Alertes + log automàtic — només en mode AUTO
                // En mode MANUAL l'usuari ja sap el que fa, no cal alertar
                if (config.mode_operacio === "AUTO") {
                    if (data.temp > (config.target_temp_max + 2)) {
                        ctx.waitUntil(enviarAlerta(`🔥 ALERTA: ${config.propietari}, temperatura crítica (${data.temp}ºC)!`, "fire"));
                        ctx.waitUntil(insertLog(env, data.device_id, `⚠️ Temperatura crítica detectada: ${data.temp}ºC`, 'hivernacle'));
                    }
                    if (data.soil < (config.target_hum_sol_min - 5)) {
                        ctx.waitUntil(enviarAlerta(`💧 ALERTA: ${config.propietari}, cal regar (${data.soil}%)!`, "droplet"));
                        ctx.waitUntil(insertLog(env, data.device_id, `⚠️ Humitat del sòl crítica: ${data.soil}%`, 'hivernacle'));
                    }
                }

                return new Response(JSON.stringify({ status: "ok" }), { 
                    status: 200, headers: { "Content-Type": "application/json" } 
                });
            } catch (e) {
                return new Response("Error Upload: " + e, { status: 500 });
            }
        }

        // --- LLEGIR CONFIG (ESP32 -> NÚVOL) ---
        if (url.pathname === '/api/config' && request.method === 'GET') {
            try {
                const device_id = url.searchParams.get("device_id");
                const token = request.headers.get("X-Auth-Token");
                if (!device_id) return new Response("Falta device_id", { status: 400 });

                const config = await env.DB.prepare(
                    "SELECT * FROM hivernacles WHERE device_id = ? AND api_token = ?"
                ).bind(device_id, token).first();
                if (!config) return new Response("Error: Token ESP32 Invàlid", { status: 403 });

                const respostaESP = {
                    mode: config.mode_operacio,
                    manual: {
                        fan: config.manual_fan,
                        pump: config.manual_pump,
                        light: config.manual_light,
                        heater: config.manual_heater
                    },
                    auto: {
                        t_min: config.target_temp_min,
                        t_max: config.target_temp_max,
                        soil_min: config.target_hum_sol_min,
                        light_on: config.target_llum_on,
                        light_off: config.target_llum_off
                    }
                };
                return new Response(JSON.stringify(respostaESP), { 
                    status: 200, headers: { "Content-Type": "application/json" } 
                });
            } catch (e) {
                return new Response("Error Config: " + e, { status: 500 });
            }
        }

        // --- ESTAT ACTUAL (WEB -> NÚVOL) ---
        if (url.pathname === '/api/status' && request.method === 'POST') {
            const { device_id, password } = await request.json();
            const valid = await env.DB.prepare("SELECT 1 FROM hivernacles WHERE device_id = ? AND password_web = ?").bind(device_id, password).first();
            if (!valid) return new Response("No autoritzat", { status: 401, headers: corsHeaders });

            const lectura = await env.DB.prepare("SELECT * FROM lectures WHERE device_id = ? ORDER BY id DESC LIMIT 1").bind(device_id).first();
            const estat = await env.DB.prepare("SELECT * FROM hivernacles WHERE device_id = ?").bind(device_id).first();
            const historic = await env.DB.prepare("SELECT temp, hum, hum_sol, llum, data_hora FROM lectures WHERE device_id = ? ORDER BY id DESC LIMIT 50").bind(device_id).all();

            return new Response(JSON.stringify({ lectura, estat, historic: historic.results }), { headers: { "Content-Type": "application/json", ...corsHeaders } });
        }
        
        // --- GUARDAR CONFIGURACIÓ (WEB -> NÚVOL) ---
        if (url.pathname === '/api/settings' && request.method === 'POST') {
             const { device_id, password, settings } = await request.json();
             const valid = await env.DB.prepare("SELECT 1 FROM hivernacles WHERE device_id = ? AND password_web = ?").bind(device_id, password).first();
             if (!valid) return new Response("No autoritzat", { status: 401, headers: corsHeaders });

             // Llegim l'estat anterior per saber QUÈ ha canviat
             const prev: any = await env.DB.prepare("SELECT mode_operacio, manual_fan, manual_pump, manual_light, manual_heater FROM hivernacles WHERE device_id = ?").bind(device_id).first();

             await env.DB.prepare(`
                UPDATE hivernacles SET 
                mode_operacio = ?, 
                manual_fan = ?, manual_pump = ?, manual_light = ?, manual_heater = ?,
                planta_activa = ?,
                target_temp_max = ?, target_temp_min = ?,
                target_hum_sol_min = ?,
                target_llum_on = ?, target_llum_off = ?
                WHERE device_id = ?
             `).bind(
                 settings.mode, 
                 settings.manual_fan ? 1 : 0, settings.manual_pump ? 1 : 0, settings.manual_light ? 1 : 0, settings.manual_heater ? 1 : 0,
                 settings.planta_activa,
                 settings.t_max, settings.t_min, 
                 settings.soil_min,
                 settings.l_on, settings.l_off,
                 device_id
             ).run();

             // ── LOGS AUTOMÀTICS DES DE LA WEB ────────────────────────────
             const logOps: Promise<any>[] = [];

             // Canvi de mode
             if (prev && prev.mode_operacio !== settings.mode) {
                 const msg = settings.mode === "MANUAL"
                     ? "🎛️ Mode canviat a MANUAL des de la web"
                     : "⚡ Mode canviat a AUTOMÀTIC des de la web";
                 logOps.push(insertLog(env, device_id, msg, 'web'));
             }

             // Canvis d'actuadors manuals (només si s'entra a mode MANUAL)
             if (settings.mode === "MANUAL" && prev) {
                 const actuadors: [string, string, number][] = [
                     ["💨 Ventilador", "fan",    settings.manual_fan    ? 1 : 0],
                     ["💧 Bomba",      "pump",   settings.manual_pump   ? 1 : 0],
                     ["💡 Llums",      "light",  settings.manual_light  ? 1 : 0],
                     ["🔥 Calefacció", "heater", settings.manual_heater ? 1 : 0],
                 ];
                 for (const [nom, key, nouVal] of actuadors) {
                     const prevVal = prev[`manual_${key}`] ?? 0;
                     if (prevVal !== nouVal) {
                         const accio = nouVal ? "activat" : "desactivat";
                         logOps.push(insertLog(env, device_id, `${nom} ${accio} des de la web (mode manual)`, 'web'));
                     }
                 }
             }

             if (logOps.length > 0) ctx.waitUntil(Promise.all(logOps));
             // ─────────────────────────────────────────────────────────────

             return new Response(JSON.stringify({ msg: "Configuració guardada" }), { headers: { "Content-Type": "application/json", ...corsHeaders } });
        }

        // --- LLISTA DE PLANTES ---
        if (url.pathname === '/api/plants') {
            const plantes = await env.DB.prepare("SELECT * FROM biblioteca_plantes").all();
            return new Response(JSON.stringify(plantes.results), { headers: { "Content-Type": "application/json", ...corsHeaders } });
        }

        // --- CHECK FIRMWARE UPDATE ---
        if (url.pathname === '/api/check-update') {
            const reqData = await request.json();
            const clientVersion = reqData.current_version;
            const LATEST_VERSION = "0.1.9";
            const BIN_URL = "https://github.com/polgussi23/hivernacleIOT/releases/download/v0.1.9/firmware.bin"; 

            if (clientVersion !== LATEST_VERSION) {
                return new Response(JSON.stringify({ update_available: true, new_version: LATEST_VERSION, bin_url: BIN_URL }), { headers: { 'Content-Type': 'application/json' } });
            } else {
                return new Response(JSON.stringify({ update_available: false }), { headers: { 'Content-Type': 'application/json' } });
            }
        }

        // ── NOU: LOG DES DE L'ESP32 (POST /api/log) ──────────────────────────
        // L'ESP32 envia: { device_id, missatge } + header X-Auth-Token
        if (url.pathname === '/api/log' && request.method === 'POST') {
            try {
                const token = request.headers.get("X-Auth-Token");
                const data: any = await request.json();

                // Verificació d'identitat (mateixa que /api/upload)
                const valid = await env.DB.prepare(
                    "SELECT 1 FROM hivernacles WHERE device_id = ? AND api_token = ?"
                ).bind(data.device_id, token).first();
                if (!valid) return new Response("Token invàlid", { status: 403 });

                await insertLog(env, data.device_id, data.missatge);
                return new Response(JSON.stringify({ ok: true }), { status: 200, headers: { "Content-Type": "application/json" } });
            } catch (e) {
                return new Response("Error Log: " + e, { status: 500 });
            }
        }

        // ── LLEGIR LOGS (GET /api/logs) ─────────────────────────────────
        // La web envia: POST { device_id, password }
        if (url.pathname === '/api/logs' && request.method === 'POST') {
            try {
                const { device_id, password } = await request.json();
                const valid = await env.DB.prepare("SELECT 1 FROM hivernacles WHERE device_id = ? AND password_web = ?").bind(device_id, password).first();
                if (!valid) return new Response("No autoritzat", { status: 401, headers: corsHeaders });

                const logs = await env.DB.prepare(
                    "SELECT id, data_hora, missatge, origen FROM logs WHERE device_id = ? ORDER BY id DESC LIMIT 100"
                ).bind(device_id).all();

                return new Response(JSON.stringify(logs.results), { headers: { "Content-Type": "application/json", ...corsHeaders } });
            } catch (e) {
                return new Response("Error Logs: " + e, { status: 500 });
            }
        }

        if (url.pathname === '/api/actuator-reset' && request.method === 'POST') {
            try {
                const token = request.headers.get("X-Auth-Token");
                const data: any = await request.json();
                const valid = await env.DB.prepare(
                    "SELECT 1 FROM hivernacles WHERE device_id = ? AND api_token = ?"
                ).bind(data.device_id, token).first();
                if (!valid) return new Response("Token invàlid", { status: 403 });
 
                if (data.actuator === "pump") {
                    await env.DB.prepare(
                        "UPDATE hivernacles SET manual_pump = 0 WHERE device_id = ?"
                    ).bind(data.device_id).run();
                    ctx.waitUntil(insertLog(env, data.device_id, "💧 Bomba desactivada automàticament (fi de pols de 5 s)", "hivernacle"));
                }
 
                return new Response(JSON.stringify({ ok: true }), { status: 200, headers: { "Content-Type": "application/json" } });
            } catch (e) {
                return new Response("Error actuator-reset: " + e, { status: 500 });
            }
        }

		return env.ASSETS.fetch(request);
	},

    async scheduled(event: ScheduledEvent, env: Env, ctx: ExecutionContext) {
        ctx.waitUntil(netejarRegistresAntics(env));
    }
};

async function netejarRegistresAntics(env: Env) {
  // Esborra lectures amb més de 5 dies
  const lecturesResult = await env.DB.prepare(`
    DELETE FROM lectures WHERE data_hora < datetime('now', '-5 days')
  `).run();

  // Esborra logs amb més de 5 dies
  const logsResult = await env.DB.prepare(`
    DELETE FROM logs WHERE data_hora < datetime('now', '-5 days')
  `).run();

  console.log(`Neteja diària: ${lecturesResult.meta.changes} lectures i ${logsResult.meta.changes} logs eliminats`);
}