'use strict';

const $ = id => document.getElementById(id);

let receipts = [];
let selected = null;
let busy = false;

// =============================================
// HELPER FUNCTIONS
// =============================================

const number = v =>
    v === null ||
    v === undefined ||
    v === '' ||
    !Number.isFinite(Number(v))
        ? null
        : Number(v);

const money = (v, c = 'BGN') =>
    v === null ? '—' : `${v.toFixed(2)} ${c}`;

const text = (id, v) => $(id).textContent = v;

const node = (tag, value) => {
    const e = document.createElement(tag);

    if (value !== undefined) {
        e.textContent = value;
    }

    return e;
};

const currentMonth = () => {
    const d = new Date();

    return `${d.getFullYear()}-${String(
        d.getMonth() + 1
    ).padStart(2, '0')}`;
};

// =============================================
// RECEIPT NORMALIZATION
// =============================================

function normalize(entry) {
    const r = entry.receipt;

    return {
        id: entry.path,
        raw: r,
        store: r.store,
        date: r.date,
        currency: 'BGN',
        total: number(r.total),

        items: r.items.map(i => {
            const q = number(i.quantity);
            const t = number(i.price);

            return {
                name: i.name,
                q,
                t,
                u: q > 0 && t !== null ? t / q : null
            };
        })
    };
}

// =============================================
// FINANCIAL CALCULATIONS
// =============================================

function totals(list) {
    const m = new Map();

    for (const r of list) {
        if (r.total !== null) {
            m.set(
                r.currency,
                (m.get(r.currency) || 0) + r.total
            );
        }
    }

    return m;
}

function summary(list, avg = false) {
    const m = totals(list);

    return [...m].map(([c, v]) =>
        money(
            avg
                ? v / list.filter(
                    r => r.currency === c && r.total !== null
                ).length
                : v,
            c
        )
    ).join(' / ') || '—';
}

// =============================================
// MONTH FILTERS
// =============================================

function months(id, all) {
    const e = $(id);
    const old = e.value;

    const values = [...new Set([
        currentMonth(),

        ...receipts
            .map(r => r.date.slice(0, 7))
            .filter(s => /^\d{4}-\d{2}$/.test(s))
    ])].sort().reverse();

    e.replaceChildren(
        new Option('Всички месеци', 'all'),
        ...values.map(v => new Option(v, v))
    );

    e.value = [...e.options].some(o => o.value === old)
        ? old
        : all
            ? 'all'
            : currentMonth();
}

// =============================================
// CHART GENERATION
// =============================================

function bars(id, list, key) {
    const e = $(id);

    e.replaceChildren();

    const groups = new Map();

    for (const r of list) {
        if (r.total === null) {
            continue;
        }

        const k = key(r) + ' · ' + r.currency;

        groups.set(k, {
            v: (groups.get(k)?.v || 0) + r.total,
            c: r.currency
        });
    }

    const entries = [...groups];

    if (id === 'stores') {
        entries.sort((a, b) => b[1].v - a[1].v);
    } else {
        entries.sort((a, b) => a[0].localeCompare(b[0]));
    }

    if (!entries.length) {
        e.append(node('p', 'Няма данни.'));
    }

    for (const [k, {v, c}] of entries) {
        const max = Math.max(
            1,
            ...entries
                .filter(x => x[1].c === c)
                .map(x => Math.abs(x[1].v))
        );

        const row = node('div');
        const label = node('div');
        const track = node('div');
        const fill = node('div');

        label.className = 'bar-label';

        label.append(
            node('span', k),
            node('strong', money(v, c))
        );

        track.className = 'track';
        fill.className = 'fill';

        fill.style.width = `${Math.abs(v) / max * 100}%`;

        track.append(fill);
        row.append(label, track);

        e.append(row);
    }
}

// =============================================
// TABLE GENERATION
// =============================================

function row(body, values) {
    const tr = node('tr');

    for (const v of values) {
        const td = node('td');

        td.append(
            v instanceof Node
                ? v
                : document.createTextNode(String(v))
        );

        tr.append(td);
    }

    body.append(tr);
}

// =============================================
// DASHBOARD
// =============================================

function dashboard() {
    const p = $('period').value;

    const list = receipts.filter(
        r => p === 'all' || r.date.startsWith(p)
    );

    text('total', summary(list));
    text('count', list.length);
    text('average', summary(list, true));

    // Monthly budget

    const b = number($('budget').value);

    const monthly = totals(
        receipts.filter(
            r => r.date.startsWith(currentMonth())
        )
    );

    const spent = monthly.get('BGN') || 0;

    text(
        'budget-text',
        b > 0
            ? `${money(spent)} / ${money(b)}`
            : 'Без лимит (BGN)'
    );

    $('budget-bar').value = b > 0
        ? Math.max(0, Math.min(100, spent / b * 100))
        : 0;

    // Spending charts

    bars('timeline', list, r => r.date || 'Без дата');
    bars('stores', list, r => r.store);

    // Recent receipts

    $('recent').replaceChildren();

    for (const r of list.slice(0, 8)) {
        const btn = node('button', r.store);

        btn.onclick = () => {
            selected = r.id;
            location.hash = 'receipts';
            renderReceipts();
        };

        row($('recent'), [
            btn,
            r.date || '—',
            r.items.length,
            money(r.total, r.currency)
        ]);
    }
}

// =============================================
// RECEIPT LIST AND DETAILS
// =============================================

function renderReceipts() {
    const q = $('search').value.toLowerCase();
    const m = $('month').value;

    let list = receipts.filter(r =>
        (m === 'all' || r.date.startsWith(m)) &&
        (
            r.store.toLowerCase().includes(q) ||
            r.items.some(
                i => i.name.toLowerCase().includes(q)
            )
        )
    );

    // Sorting

    if ($('sort').value === 'old') {
        list.reverse();
    }

    if ($('sort').value === 'high') {
        list.sort((a, b) =>
            a.currency.localeCompare(b.currency) ||
            (b.total ?? -Infinity) - (a.total ?? -Infinity)
        );
    }

    // Select a receipt

    if (!list.some(r => r.id === selected)) {
        selected = list[0]?.id ?? null;
    }

    const el = $('receipt-list');

    el.replaceChildren();

    for (const r of list) {
        const b = node('button');

        b.append(
            node('span', `${r.store} · ${r.date || '—'}`),
            node('strong', money(r.total, r.currency))
        );

        b.classList.toggle('active', r.id === selected);

        b.onclick = () => {
            selected = r.id;
            renderReceipts();
        };

        el.append(b);
    }

    // Receipt details

    const r = list.find(r => r.id === selected);
    const d = $('detail');

    d.replaceChildren();

    $('raw-box').hidden = !r;

    if (!r) {
        d.textContent = 'Няма бележки.';
        return;
    }

    d.append(
        node('h3', r.store),
        node('p', r.date || 'Без дата'),
        node('p', r.id)
    );

    const t = node('table');
    const body = node('tbody');

    row(body, ['Артикул', 'К-во', 'Сума']);

    for (const i of r.items) {
        row(body, [
            i.name,
            i.q ?? '—',
            money(i.t, r.currency)
        ]);
    }

    t.append(body);

    d.append(
        t,
        node('h3', `Общо: ${money(r.total, r.currency)}`)
    );

    text('raw', JSON.stringify(r.raw, null, 2));
}

// =============================================
// PRODUCT PRICE ANALYSIS
// =============================================

function prices() {
    const q = $('product-search').value.toLowerCase();
    const body = $('products');

    body.replaceChildren();

    for (const r of receipts) {
        for (const i of r.items) {
            if (i.name.toLowerCase().includes(q)) {
                row(body, [
                    i.name,
                    r.store,
                    r.date || '—',
                    i.q ?? '—',
                    money(i.u, r.currency),
                    money(i.t, r.currency)
                ]);
            }
        }
    }
}

// =============================================
// RENDER ALL SECTIONS
// =============================================

function render() {
    dashboard();
    renderReceipts();
    prices();
}

// =============================================
// FETCH RECEIPTS FROM GO SERVER
// =============================================

async function load() {
    if (busy) {
        return;
    }

    busy = true;

    try {
        const res = await fetch('/api/receipts', {
            cache: 'no-store'
        });

        if (!res.ok) {
            throw Error(`HTTP ${res.status}`);
        }

        const data = await res.json();

        receipts = data.receipts
            .map(normalize)
            .sort((a, b) =>
                b.date.localeCompare(a.date) ||
                b.id.localeCompare(a.id)
            );

        months('period', true);
        months('month', true);

        render();

        text(
            'status',
            `${receipts.length} бележки от сървъра · ` +
            new Date().toLocaleTimeString('bg-BG') +
            (
                data.skipped
                    ? ' · Невалидни файлове: ' + data.skipped
                    : ''
            )
        );

    } catch (e) {
        text(
            'status',
            'Неуспешно обновяване: ' +
            e.message +
            ' (показаните данни може да са стари)'
        );

    } finally {
        busy = false;
    }
}

// =============================================
// NAVIGATION
// =============================================

const titles = {
    dashboard: 'Табло за разходи',
    receipts: 'Касови бележки',
    prices: 'Анализ на цени'
};

function navigate() {
    const v = location.hash.slice(1);

    const active = Object.hasOwn(titles, v)
        ? v
        : 'dashboard';

    for (const k of Object.keys(titles)) {
        $(k).hidden = k !== active;
    }

    document.querySelectorAll('[data-view]').forEach(b =>
        b.classList.toggle(
            'active',
            b.dataset.view === active
        )
    );

    text('title', titles[active]);
}

document.querySelectorAll('[data-view]').forEach(b => {
    b.onclick = () => location.hash = b.dataset.view;
});

window.addEventListener('hashchange', navigate);

// =============================================
// EVENT LISTENERS
// =============================================

$('refresh').onclick = load;

$('period').onchange = dashboard;

// Monthly budget persistence

$('budget').value =
    localStorage.getItem('billsnap-budget') || '';

$('budget').oninput = () => {
    localStorage.setItem(
        'billsnap-budget',
        $('budget').value
    );

    dashboard();
};

// Receipt filters

for (const id of ['search', 'month', 'sort']) {
    $(id).addEventListener(
        id === 'search' ? 'input' : 'change',
        renderReceipts
    );
}

// Product search

$('product-search').oninput = prices;

// =============================================
// JSON EXPORT
// =============================================

$('export').onclick = () => {
    const url = URL.createObjectURL(
        new Blob(
            [
                JSON.stringify(
                    receipts.map(r => r.raw),
                    null,
                    2
                )
            ],
            {
                type: 'application/json'
            }
        )
    );

    const a = node('a');

    a.href = url;
    a.download = 'receipts.json';

    a.click();

    setTimeout(
        () => URL.revokeObjectURL(url),
        1000
    );
};

// =============================================
// INITIALIZATION
// =============================================

navigate();
load();

// Refresh receipt data every 15 seconds.

setInterval(load, 15000);