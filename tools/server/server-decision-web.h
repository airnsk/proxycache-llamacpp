// Decision demo web UI for the /v1/decision endpoint (Jev / parallel-decision).
// Self-contained single page: no JS toolchain, no external assets. Served at GET /decision/.
// Toggle with --no-decision-webui (default: on when --decision-seqs is set).

static const char * SRV_DECISION_WEB_HTML = R"DECISIONWEB(<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<title>Decision demo — /v1/decision</title>
<style>
:root { color-scheme: dark light; }
* { box-sizing: border-box; }
body { margin: 0; font: 14px/1.45 -apple-system, Segoe UI, Roboto, sans-serif; background: #14161a; color: #e6e6e6; }
header { padding: 10px 16px; background: #1c1f26; border-bottom: 1px solid #2c313c; display: flex; gap: 16px; align-items: baseline; }
header h1 { font-size: 15px; margin: 0; font-weight: 600; }
header .sub { color: #98a2b3; font-size: 12px; }
main { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; padding: 14px; align-items: start; }
.panel { background: #1c1f26; border: 1px solid #2c313c; border-radius: 8px; padding: 10px 12px; }
.panel h2 { font-size: 13px; margin: 0 0 8px; color: #cfd6e4; font-weight: 600; }
.panel .hint { color: #8b95a7; font-size: 12px; margin: 6px 0 0; }
textarea, input, select { width: 100%; background: #0f1115; color: #e6e6e6; border: 1px solid #313846; border-radius: 6px; padding: 8px; font: 13px/1.4 ui-monospace, SFMono-Regular, Menlo, monospace; }
textarea { resize: vertical; }
#prefix { height: 150px; }
#context { height: 110px; }
#schema { height: 150px; }
#answer { height: 190px; }
.row { display: flex; gap: 10px; align-items: center; margin-top: 8px; flex-wrap: wrap; }
.row > * { flex: 0 0 auto; }
.row label { color: #98a2b3; font-size: 12px; }
button { background: #2b6cb0; color: #fff; border: 0; border-radius: 6px; padding: 8px 12px; font-size: 13px; cursor: pointer; }
button.sec { background: #2c313c; }
button:disabled { opacity: .5; cursor: default; }
table { width: 100%; border-collapse: collapse; font-size: 12.5px; }
th, td { text-align: left; padding: 4px 6px; border-bottom: 1px solid #262c37; }
th { color: #98a2b3; font-weight: 500; }
.badge { display: inline-block; padding: 1px 6px; border-radius: 999px; background: #263041; color: #9ec5ff; font-size: 11px; }
.timings { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px; margin-top: 8px; font-size: 12px; }
.timings div { background: #0f1115; border: 1px solid #262c37; border-radius: 6px; padding: 6px 8px; }
.timings b { display: block; color: #9ec5ff; font-weight: 600; }
.err { color: #ff9c9c; }
details { margin-top: 8px; }
summary { cursor: pointer; color: #98a2b3; font-size: 12px; }
.wide { grid-column: 1 / -1; }
</style>
</head>
<body>
<header>
  <h1>Decision demo</h1>
  <span class="sub">POST /v1/decision — параллельные ограниченные решения (Jev). Ответ собирается кодом, поэтому всегда валиден по схеме.</span>
</header>

<main>
  <section class="panel">
    <h2>1. Префикс (кэшируется)</h2>
    <textarea id="prefix" spellcheck="false" placeholder="Инструкции/справочник — эта часть уходит в кэш общего префикса"></textarea>
    <div class="row">
      <button class="sec" id="fillPrefix">Заполнить примером</button>
      <label><input type="checkbox" id="cachePrompt" checked style="width:auto"> cache_prompt</label>
    </div>
    <p class="hint">Запусти один и тот же запрос дважды: второй раз <b>cached_tokens</b> будет почти равен длине префикса, а <b>prefill_ms</b> обвалится — это и есть демонстрация кэша.</p>
  </section>

  <section class="panel">
    <h2>2. Задача (контекст)</h2>
    <textarea id="context" spellcheck="false" placeholder="Контекст решения. Несколько контекстов — разделяй строкой ---"></textarea>
    <div class="row">
      <span class="hint" id="ctxCount">контекстов: 1</span>
      <button class="sec" id="fillCtx">Заполнить примером</button>
    </div>
  </section>

  <section class="panel">
    <h2>3. Схема полей</h2>
    <div class="row">
      <label>пресет</label>
      <select id="preset" style="width:auto"></select>
      <button class="sec" id="applyPreset">Применить</button>
    </div>
    <textarea id="schema" spellcheck="false"></textarea>
    <p class="hint">Типы: <span class="badge">enum</span> <span class="badge">boolean</span> <span class="badge">integer</span> (min/max) <span class="badge">number</span> (min/max/step, aggregate: mode|median|mean)</p>
  </section>

  <section class="panel">
    <h2>4. Режим</h2>
    <div class="row">
      <label>mode</label>
      <select id="mode" style="width:auto">
        <option value="auto">auto</option>
        <option value="tree">tree (точные вероятности)</option>
        <option value="greedy">greedy</option>
      </select>
      <label>tree_max</label>
      <input id="treeMax" type="number" value="128" min="1" max="255" style="width:80px">
    </div>
    <div class="row">
      <button id="run">Решить</button>
      <button class="sec" id="runTwice">Решить дважды (показать кэш)</button>
    </div>
    <p class="hint" id="status"></p>
  </section>

  <section class="panel wide">
    <h2>5. Ответ</h2>
    <table id="ansTable"><thead><tr><th>поле</th><th>значение</th><th>вероятность</th><th>scored_nodes</th><th>tree</th></tr></thead><tbody></tbody></table>
    <div class="timings" id="timings"></div>
    <details><summary>сырой JSON ответа</summary><textarea id="answer" spellcheck="false"></textarea></details>
    <details><summary>чем это демонстрируется (все типы задач)</summary>
      <p class="hint">Пресеты: классификация (enum+boolean+enum) · числовая шкала integer 0..10 с агрегатами · дробное число с шагом · бинарный гейт · большое enum (20 меток) · батч контекстов (несколько решений в одном запросе) · режимы auto/tree/greedy на одном вводе · кэш префикса (тот же запрос дважды) · краевые случаи (поле с одним значением, низкая уверенность).</p>
    </details>
  </section>
</main>

<script>
const $ = (id) => document.getElementById(id);

const PREFIX_EXAMPLE = "Справочник для классификации обращений.\n" +
  "Категория определяется по предмету претензии: billing — списания и счета, technical — работа сервиса и доступ, " +
  "cancellation — отказ от подписки, other — прочее.\n" +
  "Приоритет оценивается по срокам ответа: low — в течение недели, medium — в течение двух дней, high — в течение дня, " +
  "critical — в течение часа. Признак urgent выставляется, если ответ нужен сегодня.";

const ctxExample = "С меня списали дважды, нужно исправить сегодня.";

const PRESETS = {
  "классификация: enum + boolean + enum": {
    prefix: PREFIX_EXAMPLE,
    context: ctxExample,
    schema: {
      category: { type: "enum", choices: ["billing", "technical", "cancellation", "other"], description: "Что это за обращение?" },
      urgent:   { type: "boolean", description: "Нужен ответ сегодня?" },
      priority: { type: "enum", choices: ["low", "medium", "high", "critical"], description: "Какой приоритет?" }
    }
  },
  "числовая шкала: integer 0..10 (mode)": {
    prefix: PREFIX_EXAMPLE,
    context: ctxExample,
    schema: { urgency_score: { type: "integer", minimum: 0, maximum: 10, description: "Насколько срочно, от 0 до 10?" } }
  },
  "числовая шкала: integer (median)": {
    prefix: PREFIX_EXAMPLE,
    context: ctxExample,
    schema: { urgency_score: { type: "integer", minimum: 0, maximum: 10, aggregate: "median", description: "Насколько срочно, от 0 до 10 (медиана)?" } }
  },
  "дробное число: number 35..42 шаг 0.1": {
    prefix: "Извлеки числовое значение из текста.",
    context: "Температура пациента по показаниям: 36,6 °C, жалоб нет.",
    schema: { temperature: { type: "number", minimum: 35, maximum: 42, step: 0.1, description: "Какая температура?" } }
  },
  "бинарный гейт: boolean": {
    prefix: "Реши, нужен ли человек.",
    context: "Клиент просит вернуть деньги за двойное списание.",
    schema: { needs_human: { type: "boolean", description: "Нужен человек?" } }
  },
  "большое enum (20 меток)": {
    prefix: PREFIX_EXAMPLE,
    context: "Не приходит письмо для сброса пароля.",
    schema: { topic: { type: "enum", description: "Тема обращения?",
      choices: ["accounts","passwords","billing","refunds","invoices","technical","api","integrations","mobile-app","web-app",
                "performance","outages","security","privacy","data-export","onboarding","cancellation","plans","partners","other"] } }
  },
  "батч контекстов (4 в одном запросе)": {
    prefix: PREFIX_EXAMPLE,
    context: ["С меня списали дважды, нужно исправить сегодня.",
              "Не работает вход в личный кабинет третий день.",
              "Хочу отменить подписку и вернуть деньги за год.",
              "Как настроить интеграцию с нашей CRM?"].join("\n---\n"),
    schema: {
      category: { type: "enum", choices: ["billing", "technical", "cancellation", "other"], description: "Тип обращения?" },
      urgent:   { type: "boolean", description: "Нужен ответ сегодня?" }
    }
  },
  "кэш: тот же запрос дважды (кнопка «Решить дважды»)": {
    prefix: PREFIX_EXAMPLE,
    context: ctxExample,
    schema: { category: { type: "enum", choices: ["billing", "technical", "cancellation", "other"], description: "Тип обращения?" } }
  },
  "краевой случай: одно значение + неуверенность": {
    prefix: "Выбери ровно один вариант; если неясно — всё равно выбери самый вероятный.",
    context: "Текст без явных признаков: «здравствуйте».",
    schema: {
      only_one: { type: "enum", choices: ["other"], description: "Поле с единственным допустимым значением" },
      guess:    { type: "enum", choices: ["billing", "technical", "cancellation", "other"], description: "Догадка (смотри вероятность)" }
    }
  }
};

function setStatus(t, isErr) { const s = $("status"); s.textContent = t || ""; s.className = isErr ? "err" : ""; }

function currentRequest() {
  const schema = JSON.parse($("schema").value);
  const contexts = $("context").value.split(/\n\s*---\s*\n/).map(s => s.trim()).filter(Boolean);
  const req = { instructions: $("prefix").value, schema: schema, contexts: contexts };
  const mode = $("mode").value;
  if (mode !== "auto") req.mode = mode;
  const tm = parseInt($("treeMax").value, 10);
  if (!isNaN(tm)) req.tree_max = tm;
  req.cache_prompt = $("cachePrompt").checked;
  return req;
}

async function decide(req) {
  const t0 = performance.now();
  const r = await fetch("/v1/decision", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(req)
  });
  const txt = await r.text();
  const wall = performance.now() - t0;
  let json = null;
  try { json = JSON.parse(txt); } catch (e) {}
  if (!r.ok) throw new Error("HTTP " + r.status + ": " + txt.slice(0, 300));
  return { json: json, text: txt, wall: wall };
}

function render(json, wall) {
  $("answer").value = JSON.stringify(json, null, 2);
  const tb = $("ansTable").querySelector("tbody");
  tb.innerHTML = "";
  const names = Object.keys(json.results[0].fields);
  json.results.forEach((res, i) => {
    names.forEach((n) => {
      const f = res.fields[n];
      const tr = document.createElement("tr");
      tr.innerHTML = "<td>" + (json.results.length > 1 ? "[" + i + "] " : "") + n + "</td>" +
        "<td><b>" + JSON.stringify(f.value) + "</b></td>" +
        "<td>" + (f.probability !== undefined ? f.probability.toFixed(4) : "") + "</td>" +
        "<td>" + f.scored_nodes + "</td><td>" + (f.tree ? "да" : "нет") + "</td>";
      tb.appendChild(tr);
    });
  });
  const t = json.timings || {};
  const u = json.usage || {};
  $("timings").innerHTML =
    "<div><b>" + (t.prefill_ms !== undefined ? t.prefill_ms.toFixed(1) : "—") + " мс</b>prefill</div>" +
    "<div><b>" + (t.scoring_ms !== undefined ? t.scoring_ms.toFixed(1) : "—") + " мс</b>scoring</div>" +
    "<div><b>" + (t.total_ms !== undefined ? t.total_ms.toFixed(1) : "—") + " мс</b>total" +
      (wall ? " (+" + Math.round(wall - (t.total_ms || 0)) + " мс сеть)" : "") + "</div>" +
    "<div><b>" + (u.cached_tokens || 0) + "</b>cached_tokens</div>" +
    "<div><b>" + (u.prompt_tokens || 0) + "</b>prompt_tokens</div>" +
    "<div><b>" + (t.rounds !== undefined ? t.rounds : "—") + "</b>rounds" +
      (t.per_decision_ms !== undefined ? " · " + t.per_decision_ms.toFixed(1) + " мс/решение" : "") + "</div>";
}

async function run(once) {
  const btn = $("run"), btn2 = $("runTwice");
  btn.disabled = btn2.disabled = true;
  try {
    const req = currentRequest();
    setStatus("запрос…");
    const a = await decide(req);
    render(a.json, a.wall);
    if (once) { setStatus("готово"); return; }
    setStatus("первый ответ получен, повторяю тот же запрос для демонстрации кэша…");
    const b = await decide(req);
    render(b.json, b.wall);
    const ua = a.json.usage || {}, ub = b.json.usage || {};
    const ta = a.json.timings || {}, tb2 = b.json.timings || {};
    setStatus("кэш: cached_tokens " + (ua.cached_tokens || 0) + " → " + (ub.cached_tokens || 0) +
      ", prefill " + (ta.prefill_ms || 0).toFixed(0) + " → " + (tb2.prefill_ms || 0).toFixed(0) + " мс");
  } catch (e) {
    setStatus("ошибка: " + e.message, true);
  } finally {
    btn.disabled = btn2.disabled = false;
  }
}

function applyPreset() {
  const p = PRESETS[$("preset").value];
  if (!p) return;
  $("prefix").value = p.prefix;
  $("context").value = Array.isArray(p.context) ? p.context.join("\n---\n") : p.context;
  $("schema").value = JSON.stringify(p.schema, null, 2);
  updateCtxCount();
}

function updateCtxCount() {
  const n = $("context").value.split(/\n\s*---\s*\n/).map(s => s.trim()).filter(Boolean).length;
  $("ctxCount").textContent = "контекстов: " + Math.max(1, n);
}

const sel = $("preset");
Object.keys(PRESETS).forEach(k => { const o = document.createElement("option"); o.value = k; o.textContent = k; sel.appendChild(o); });
$("applyPreset").onclick = applyPreset;
$("run").onclick = () => run(true);
$("runTwice").onclick = () => run(false);
$("fillPrefix").onclick = () => { $("prefix").value = PREFIX_EXAMPLE; };
$("fillCtx").onclick = () => { $("context").value = ctxExample; updateCtxCount(); };
$("context").oninput = updateCtxCount;
sel.onchange = applyPreset;
applyPreset();
</script>
</body>
</html>
)DECISIONWEB";
