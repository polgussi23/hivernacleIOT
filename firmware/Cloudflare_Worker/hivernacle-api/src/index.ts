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

        // Registre d'un nou usuari
        if (url.pathname === '/api/register' && request.method === 'POST') {
            try {
                const { user_name, password } = await request.json();

                const existeix = await env.DB.prepare(
                    "SELECT 1 FROM usuaris WHERE user_name = ?"
                ).bind(user_name).first();

                if (existeix) {
                    return new Response(
                        JSON.stringify({ error: "L'usuari ja existeix" }),
                        { status: 409, headers: corsHeaders }
                    );
                }

                const hashed_pass = await hashPassword(password);
                await env.DB.prepare(
                    "INSERT INTO usuaris (user_name, password) VALUES (?, ?)"
                ).bind(user_name, hashed_pass).run();

                return new Response(
                    JSON.stringify({ ok: true, msg: "Usuari creat correctament" }),
                    { headers: { "Content-Type": "application/json", ...corsHeaders } }
                );
            } catch (e) {
                return new Response(
                    JSON.stringify({ error: "Error intern al registrar" }),
                    { status: 500, headers: { "Content-Type": "application/json", ...corsHeaders } }
                );
            }
        }

        // Login: retorna l'usuari i els seus hivernacles associats
        if (url.pathname === '/api/login' && request.method === 'POST') {
            try {
                const { user_name, password } = await request.json();

                const user: any = await env.DB.prepare(
                    "SELECT user_id, user_name, password FROM usuaris WHERE user_name = ?"
                ).bind(user_name).first();

                if (!user || !await verifyPassword(user.password, password)) {
                    return new Response(
                        JSON.stringify({ error: "Credencials incorrectes" }),
                        { status: 401, headers: corsHeaders }
                    );
                }

                const hivernacles = await env.DB.prepare(
                    "SELECT device_id FROM hivernacles WHERE propietari = ?"
                ).bind(user.user_id).all();

                return new Response(JSON.stringify({
                    user_id: user.user_id,
                    user_name: user.user_name,
                    hivernacles: hivernacles.results.map((h: any) => h.device_id)
                }), { headers: { "Content-Type": "application/json", ...corsHeaders } });

            } catch (e) {
                return new Response(
                    JSON.stringify({ error: "Error intern al login" }),
                    { status: 500, headers: { "Content-Type": "application/json", ...corsHeaders } }
                );
            }
        }

        // Vincula un hivernacle a un usuari (només si no en té cap de propietari ja)
        if (url.pathname === '/api/claim-hivernacle' && request.method === 'POST') {
            try {
                const { user_id, device_id } = await request.json();

                const hivernacle = await env.DB.prepare(
                    "SELECT propietari FROM hivernacles WHERE device_id = ?"
                ).bind(device_id).first();

                if (!hivernacle) {
                    return new Response(
                        JSON.stringify({ error: "L'hivernacle no existeix" }),
                        { status: 404, headers: corsHeaders }
                    );
                }

                if (hivernacle.propietari !== null && hivernacle.propietari !== "") {
                    return new Response(
                        JSON.stringify({ error: "Aquest hivernacle ja està vinculat a un altre usuari" }),
                        { status: 403, headers: corsHeaders }
                    );
                }

                await env.DB.prepare(
                    "UPDATE hivernacles SET propietari = ? WHERE device_id = ?"
                ).bind(user_id, device_id).run();

                return new Response(
                    JSON.stringify({ ok: true, msg: "Hivernacle assignat correctament" }),
                    { headers: { "Content-Type": "application/json", ...corsHeaders } }
                );
            } catch (e) {
                return new Response(
                    JSON.stringify({ error: "Error intern al vincular hivernacle" }),
                    { status: 500, headers: { "Content-Type": "application/json", ...corsHeaders } }
                );
            }
        }

        // L'ESP32 puja les seves lectures periòdiques aquí
        if (url.pathname === '/api/upload' && request.method === 'POST') {
            try {
                const token = request.headers.get("X-Auth-Token");
                const data: any = await request.json();

                // Validem el token i de passada obtenim la config i el nom del propietari
                const config: any = await env.DB.prepare(`
                    SELECT h.*, u.user_name 
                    FROM hivernacles h 
                    LEFT JOIN usuaris u ON h.propietari = u.user_id 
                    WHERE h.device_id = ? AND h.api_token = ?
                `).bind(data.device_id, token).first();

                if (!config) return new Response("Error: Token ESP32 Invàlid", { status: 403 });

                await env.DB.prepare(
                    "INSERT INTO lectures (device_id, temp, hum, hum_sol, llum, pid_fan, pid_heater) VALUES (?, ?, ?, ?, ?, ?, ?)"
                ).bind(
                    data.device_id,
                    data.temp,
                    data.hum,
                    data.soil || 0,
                    data.light || 0,
                    data.pid_fan || 0,
                    data.pid_heater || 0
                ).run();

                await env.DB.prepare(
                    "UPDATE hivernacles SET ultima_connexio = CURRENT_TIMESTAMP WHERE device_id = ?"
                ).bind(data.device_id).run();

                // En mode AUTO enviem alertes si els valors crítiques se'n van de mare
                if (config.mode_operacio === "AUTO") {
                    const nomPropietari = config.user_name || "Usuari Desconegut";

                    if (data.temp > (config.target_temp_max + 2)) {
                        ctx.waitUntil(enviarAlerta(`🔥 ALERTA: ${nomPropietari}, temperatura crítica (${data.temp}ºC) al dispositiu ${data.device_id}!`, "fire"));
                        ctx.waitUntil(insertLog(env, data.device_id, `⚠️ Temperatura crítica detectada: ${data.temp}ºC`, 'hivernacle'));
                    }
                    if (data.soil < (config.target_hum_sol_min - 5)) {
                        ctx.waitUntil(enviarAlerta(`💧 ALERTA: ${nomPropietari}, cal regar (${data.soil}%) al dispositiu ${data.device_id}!`, "droplet"));
                        ctx.waitUntil(insertLog(env, data.device_id, `⚠️ Humitat del sòl crítica: ${data.soil}%`, 'hivernacle'));
                    }
                }

                return new Response(JSON.stringify({ status: "ok" }), {
                    status: 200, headers: { "Content-Type": "application/json" }
                });
            } catch (e) {
                return new Response(
                    JSON.stringify({ error: "Error intern al pujar dades" }),
                    { status: 500, headers: { "Content-Type": "application/json", ...corsHeaders } }
                );
            }
        }

        // L'ESP32 demana la seva configuració en cada cicle
        if (url.pathname === '/api/config' && request.method === 'GET') {
            try {
                const device_id = url.searchParams.get("device_id");
                const token = request.headers.get("X-Auth-Token");
                if (!device_id) return new Response("Falta device_id", { status: 400 });

                const config = await env.DB.prepare(
                    "SELECT * FROM hivernacles WHERE device_id = ? AND api_token = ?"
                ).bind(device_id, token).first();

                if (!config) return new Response("Error: Token ESP32 Invàlid", { status: 403 });

                // Formatem la resposta amb els camps que espera el firmware
                const respostaESP = {
                    mode: config.mode_operacio,
                    manual: {
                        fan:    config.manual_fan,
                        pump:   config.manual_pump,
                        light:  config.manual_light,
                        heater: config.manual_heater
                    },
                    auto: {
                        t_min:     config.target_temp_min,
                        t_max:     config.target_temp_max,
                        soil_min:  config.target_hum_sol_min,
                        light_on:  config.target_llum_on,
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

        // La web demana l'estat actual: última lectura + configuració + historial
        if (url.pathname === '/api/status' && request.method === 'POST') {
            const { device_id, user_id } = await request.json();

            const valid = await env.DB.prepare(
                "SELECT 1 FROM hivernacles WHERE device_id = ? AND propietari = ?"
            ).bind(device_id, user_id).first();

            if (!valid) return new Response("No autoritzat: L'hivernacle no et pertany", { status: 401, headers: corsHeaders });

            const lectura  = await env.DB.prepare("SELECT * FROM lectures WHERE device_id = ? ORDER BY id DESC LIMIT 1").bind(device_id).first();
            const estat    = await env.DB.prepare("SELECT * FROM hivernacles WHERE device_id = ?").bind(device_id).first();
            const historic = await env.DB.prepare("SELECT temp, hum, hum_sol, llum, data_hora FROM lectures WHERE device_id = ? ORDER BY id DESC LIMIT 50").bind(device_id).all();

            return new Response(
                JSON.stringify({ lectura, estat, historic: historic.results }),
                { headers: { "Content-Type": "application/json", ...corsHeaders } }
            );
        }

        // La web guarda la configuració (mode, actuadors manuals, targets auto...)
        if (url.pathname === '/api/settings' && request.method === 'POST') {
            const { device_id, user_id, settings } = await request.json();

            const valid = await env.DB.prepare(
                "SELECT 1 FROM hivernacles WHERE device_id = ? AND propietari = ?"
            ).bind(device_id, user_id).first();
            if (!valid) return new Response("No autoritzat", { status: 401, headers: corsHeaders });

            // Guardem l'estat anterior per saber si cal logar algun canvi
            const prev: any = await env.DB.prepare(
                "SELECT mode_operacio, manual_fan, manual_pump, manual_light, manual_heater FROM hivernacles WHERE device_id = ?"
            ).bind(device_id).first();

            await env.DB.prepare(`
                UPDATE hivernacles SET 
                    mode_operacio     = ?, 
                    manual_fan        = ?, manual_pump  = ?, manual_light  = ?, manual_heater = ?,
                    planta_activa     = ?,
                    target_temp_max   = ?, target_temp_min   = ?,
                    target_hum_sol_min = ?,
                    target_llum_on    = ?, target_llum_off   = ?
                WHERE device_id = ?
            `).bind(
                settings.mode,
                settings.manual_fan    ? 1 : 0,
                settings.manual_pump   ? 1 : 0,
                settings.manual_light  ? 1 : 0,
                settings.manual_heater ? 1 : 0,
                settings.planta_activa,
                settings.t_max, settings.t_min,
                settings.soil_min,
                settings.l_on, settings.l_off,
                device_id
            ).run();

            const logOps: Promise<any>[] = [];

            if (prev && prev.mode_operacio !== settings.mode) {
                const msg = settings.mode === "MANUAL"
                    ? "🎛️ Mode canviat a MANUAL des de la web"
                    : "⚡ Mode canviat a AUTOMÀTIC des de la web";
                logOps.push(insertLog(env, device_id, msg, 'web'));
            }

            // En mode manual, registrem canvis individuals d'actuadors
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

            return new Response(
                JSON.stringify({ msg: "Configuració guardada" }),
                { headers: { "Content-Type": "application/json", ...corsHeaders } }
            );
        }

        // Biblioteca de plantes disponibles
        if (url.pathname === '/api/plants') {
            const plantes = await env.DB.prepare("SELECT * FROM biblioteca_plantes").all();
            return new Response(
                JSON.stringify(plantes.results),
                { headers: { "Content-Type": "application/json", ...corsHeaders } }
            );
        }

        // L'ESP32 comprova si hi ha firmware nou disponible
        if (url.pathname === '/api/check-update') {
            const reqData = await request.json();
            const clientVersion = reqData.current_version;
            const LATEST_VERSION = "0.2.2";
            const BIN_URL = "https://github.com/polgussi23/hivernacleIOT/releases/download/v0.2.2/firmware.bin";

            if (clientVersion !== LATEST_VERSION) {
                return new Response(
                    JSON.stringify({ update_available: true, new_version: LATEST_VERSION, bin_url: BIN_URL }),
                    { headers: { 'Content-Type': 'application/json' } }
                );
            }
            return new Response(
                JSON.stringify({ update_available: false }),
                { headers: { 'Content-Type': 'application/json' } }
            );
        }

        // L'ESP32 envia un missatge de log (errors, arrencades, etc.)
        if (url.pathname === '/api/log' && request.method === 'POST') {
            try {
                const token = request.headers.get("X-Auth-Token");
                const data: any = await request.json();

                const valid = await env.DB.prepare(
                    "SELECT 1 FROM hivernacles WHERE device_id = ? AND api_token = ?"
                ).bind(data.device_id, token).first();
                if (!valid) return new Response("Token invàlid", { status: 403 });

                await insertLog(env, data.device_id, data.missatge);
                return new Response(JSON.stringify({ ok: true }), {
                    status: 200, headers: { "Content-Type": "application/json" }
                });
            } catch (e) {
                return new Response(
                    JSON.stringify({ error: "Error intern al guardar log" }),
                    { status: 500, headers: { "Content-Type": "application/json" } }
                );
            }
        }

        // La web llegeix els últims 100 logs d'un dispositiu
        if (url.pathname === '/api/logs' && request.method === 'POST') {
            try {
                const { device_id, user_id } = await request.json();

                const valid = await env.DB.prepare(
                    "SELECT 1 FROM hivernacles WHERE device_id = ? AND propietari = ?"
                ).bind(device_id, user_id).first();
                if (!valid) return new Response("No autoritzat", { status: 401, headers: corsHeaders });

                const logs = await env.DB.prepare(
                    "SELECT id, data_hora, missatge, origen FROM logs WHERE device_id = ? ORDER BY id DESC LIMIT 100"
                ).bind(device_id).all();

                return new Response(
                    JSON.stringify(logs.results),
                    { headers: { "Content-Type": "application/json", ...corsHeaders } }
                );
            } catch (e) {
                return new Response(
                    JSON.stringify({ error: "Error intern al llegir logs" }),
                    { status: 500, headers: { "Content-Type": "application/json", ...corsHeaders } }
                );
            }
        }

        // L'ESP32 avisa que ha acabat el pols de la bomba (5 s) i la desactiva
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

                return new Response(JSON.stringify({ ok: true }), {
                    status: 200, headers: { "Content-Type": "application/json" }
                });
            } catch (e) {
                return new Response(
                    JSON.stringify({ error: "Error intern al reset d'actuador" }),
                    { status: 500, headers: { "Content-Type": "application/json" } }
                );
            }
        }

        return env.ASSETS.fetch(request);
    },

    // Tasca programada diàriament per netejar registres vells
    async scheduled(event: ScheduledEvent, env: Env, ctx: ExecutionContext) {
        ctx.waitUntil(netejarRegistresAntics(env));
    }
};

async function netejarRegistresAntics(env: Env) {
    const lecturesResult = await env.DB.prepare(
        "DELETE FROM lectures WHERE data_hora < datetime('now', '-40 days')"
    ).run();

    const logsResult = await env.DB.prepare(
        "DELETE FROM logs WHERE data_hora < datetime('now', '-5 days')"
    ).run();

    console.log(`Neteja diària: ${lecturesResult.meta.changes} lectures i ${logsResult.meta.changes} logs eliminats`);
}

// PBKDF2 amb salt aleatori. Format guardat: "saltHex:hashHex"
async function hashPassword(password: string): Promise<string> {
    const encoder = new TextEncoder();
    const salt = crypto.getRandomValues(new Uint8Array(16));

    const keyMaterial = await crypto.subtle.importKey(
        "raw", encoder.encode(password), "PBKDF2", false, ["deriveBits"]
    );
    const derivedBits = await crypto.subtle.deriveBits(
        { name: "PBKDF2", salt, iterations: 100000, hash: "SHA-256" },
        keyMaterial, 256
    );

    const toHex = (buf: Uint8Array) => Array.from(buf).map(b => b.toString(16).padStart(2, '0')).join('');
    return `${toHex(salt)}:${toHex(new Uint8Array(derivedBits))}`;
}

async function verifyPassword(hashedPassword: string, password: string): Promise<boolean> {
    const [saltHex, storedHashHex] = hashedPassword.split(':');
    const encoder = new TextEncoder();

    const salt = new Uint8Array(saltHex.match(/.{2}/g)!.map(b => parseInt(b, 16)));

    const keyMaterial = await crypto.subtle.importKey(
        "raw", encoder.encode(password), "PBKDF2", false, ["deriveBits"]
    );
    const derivedBits = await crypto.subtle.deriveBits(
        { name: "PBKDF2", salt, iterations: 100000, hash: "SHA-256" },
        keyMaterial, 256
    );

    const computedHashHex = Array.from(new Uint8Array(derivedBits))
        .map(b => b.toString(16).padStart(2, '0')).join('');

    return computedHashHex === storedHashHex;
}