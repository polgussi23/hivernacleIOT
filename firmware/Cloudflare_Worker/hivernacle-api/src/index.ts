export interface Env {
	DB: D1Database;
	ASSETS: Fetcher;
}

// Capçaleres perquè la web pugui parlar amb el servidor sense errors (CORS)
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

export default {
	async fetch(request: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
		const url = new URL(request.url);

        // 1. GESTIÓ DE CORS (Important per la Web)
        if (request.method === "OPTIONS") {
            return new Response(null, { headers: corsHeaders });
        }

		// --- 2. API: LOGIN WEB (Per entrar al panell) ---
		if (url.pathname === '/api/login' && request.method === 'POST') {
            try {
                const { device_id, password } = await request.json();
                
                // Busquem l'usuari i comprovem la contrasenya
                const user = await env.DB.prepare(
                    "SELECT device_id, propietari FROM hivernacles WHERE device_id = ? AND password_web = ?"
                ).bind(device_id, password).first();
                
                if (!user) {
                    return new Response(JSON.stringify({ error: "Credencials incorrectes" }), { status: 401, headers: corsHeaders });
                }
                
                return new Response(JSON.stringify(user), { headers: { "Content-Type": "application/json", ...corsHeaders } });
            } catch (e) { return new Response("Error Login", { status: 500, headers: corsHeaders }); }
		}

        // --- 3. API: PUJAR DADES (ESP32 -> NÚVOL) ---
        // Aquesta és la part més important del sistema híbrid
        if (url.pathname === '/api/upload' && request.method === 'POST') {
            try {
                const token = request.headers.get("X-Auth-Token"); // L'ESP32 ha d'enviar això
                const data: any = await request.json();
                
                // VERIFIQUEM IDENTITAT DE L'ESP32
                const config = await env.DB.prepare(
                    "SELECT * FROM hivernacles WHERE device_id = ? AND api_token = ?"
                ).bind(data.device_id, token).first();

                if (!config) return new Response("Error: Token ESP32 Invàlid", { status: 403 });

                // A. GUARDEM DADES A L'HISTÒRIC
                // Nota: Assegura't que l'ESP32 envia 'soil' o canvia-ho aquí per 'sol' segons el teu JSON
                await env.DB.prepare(
                    "INSERT INTO lectures (device_id, temp, hum, hum_sol, llum) VALUES (?, ?, ?, ?, ?)"
                ).bind(data.device_id, data.temp, data.hum, data.soil || 0, data.light || 0).run();

                // B. ACTUALITZEM 'ULTIMA CONNEXIÓ'
                await env.DB.prepare(
                    "UPDATE hivernacles SET ultima_connexio = CURRENT_TIMESTAMP WHERE device_id = ?"
                ).bind(data.device_id).run();

                // C. GESTIÓ D'ALERTES (Dinàmiques segons la configuració!)
                // Si la temperatura supera el màxim configurat per l'usuari + 2 graus de marge
                if(data.temp > (config.target_temp_max + 2)){
                    ctx.waitUntil(enviarAlerta(`🔥 ALERTA: ${config.propietari}, temperatura crítica (${data.temp}ºC)!`, "fire"));
                }
                // Si el sòl està més sec del mínim configurat
                if(data.soil < (config.target_hum_sol_min - 5)) {
                    ctx.waitUntil(enviarAlerta(`💧 ALERTA: ${config.propietari}, cal regar (${data.soil}%)!`, "droplet"));
                }

                // D. JA NO RETORNEM LA CONFIG, NOMÉS UN 'OK' (Així és més ràpid i gasta menys dades)
                return new Response(JSON.stringify({ status: "ok" }), { 
                    status: 200, 
                    headers: { "Content-Type": "application/json" } 
                });

            } catch (e) {
                return new Response("Error Upload: " + e, { status: 500 });
            }
        }

        if (url.pathname === '/api/config' && request.method === 'GET') {
            try {
                // Agafem el device_id de la URL (ex: /api/config?device_id=hivernacle_1)
                const device_id = url.searchParams.get("device_id");
                const token = request.headers.get("X-Auth-Token");

                if (!device_id) return new Response("Falta device_id", { status: 400 });

                // VERIFIQUEM IDENTITAT
                const config = await env.DB.prepare(
                    "SELECT * FROM hivernacles WHERE device_id = ? AND api_token = ?"
                ).bind(device_id, token).first();

                if (!config) return new Response("Error: Token ESP32 Invàlid", { status: 403 });

                // RETORNEM NOMÉS LA CONFIGURACIÓ
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
                    status: 200, 
                    headers: { "Content-Type": "application/json" } 
                });

            } catch (e) {
                return new Response("Error Config: " + e, { status: 500 });
            }
        }

        // --- 4. API: LLEGIR ESTAT ACTUAL (WEB -> NÚVOL) ---
        if (url.pathname === '/api/status' && request.method === 'POST') {
            const { device_id, password } = await request.json();
            
            // Seguretat
            const valid = await env.DB.prepare("SELECT 1 FROM hivernacles WHERE device_id = ? AND password_web = ?").bind(device_id, password).first();
            if (!valid) return new Response("No autoritzat", { status: 401, headers: corsHeaders });

            // Obtenim última lectura i estat actual
            const lectura = await env.DB.prepare("SELECT * FROM lectures WHERE device_id = ? ORDER BY id DESC LIMIT 1").bind(device_id).first();
            const estat = await env.DB.prepare("SELECT * FROM hivernacles WHERE device_id = ?").bind(device_id).first();

            // Obtenim històric per la gràfica (ultimes 50)
            const historic = await env.DB.prepare("SELECT temp, hum, hum_sol, llum, data_hora FROM lectures WHERE device_id = ? ORDER BY id DESC LIMIT 50").bind(device_id).all();

            return new Response(JSON.stringify({ lectura, estat, historic: historic.results }), { headers: { "Content-Type": "application/json", ...corsHeaders } });
        }
        
        // --- 5. API: GUARDAR CANVIS CONFIGURACIÓ (WEB -> NÚVOL) ---
        if (url.pathname === '/api/settings' && request.method === 'POST') {
             const { device_id, password, settings } = await request.json();
             
             // Seguretat
             const valid = await env.DB.prepare("SELECT 1 FROM hivernacles WHERE device_id = ? AND password_web = ?").bind(device_id, password).first();
             if (!valid) return new Response("No autoritzat", { status: 401, headers: corsHeaders });

             // Actualitzem la BBDD
             // Això inclou tant canvis de mode (Manual/Auto) com canvis de planta (target_temp...)
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

             return new Response(JSON.stringify({ msg: "Configuració guardada" }), { headers: { "Content-Type": "application/json", ...corsHeaders } });
        }

        // --- 6. API: OBTENIR LLISTA DE PLANTES (WEB) ---
        if (url.pathname === '/api/plants') {
            const plantes = await env.DB.prepare("SELECT * FROM biblioteca_plantes").all();
            return new Response(JSON.stringify(plantes.results), { headers: { "Content-Type": "application/json", ...corsHeaders } });
        }

        if (url.pathname === '/api/check-update') {
            const reqData = await request.json();
            const clientVersion = reqData.current_version;
            
            // ------------------- VERSIÓ ACTUAL HIVERNACLE -------------------
            const LATEST_VERSION = "0.1.5";
            // ----------------------------------------------------------------
            const BIN_URL = "https://github.com/polgussi23/hivernacleIOT/releases/download/v0.1.5/firmware.bin"; 

            if (clientVersion !== LATEST_VERSION) {
                return new Response(JSON.stringify({
                update_available: true,
                new_version: LATEST_VERSION,
                bin_url: BIN_URL
                }), { headers: { 'Content-Type': 'application/json' } });
            } else {
                return new Response(JSON.stringify({
                update_available: false
                }), { headers: { 'Content-Type': 'application/json' } });
            }
        }

		// --- 7. SERVIR LA WEB (HTML) ---
        // Si no és cap ruta /api/..., serveix els fitxers de la carpeta public/
		return env.ASSETS.fetch(request);
	},
};