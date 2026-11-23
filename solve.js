// Fast, dependency-free, byte-exact implementation of makefun-site 1.0.0.

const REPORT_FILE = 'build-report.txt';
const MONTHS = [
  'January', 'February', 'March', 'April', 'May', 'June',
  'July', 'August', 'September', 'October', 'November', 'December',
];

const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

// Fast HTML escape using chunked scan (avoids per-char string concatenation).
function escapeHtml(s) {
  const len = s.length;
  let out = '';
  let last = 0;
  for (let i = 0; i < len; i++) {
    const c = s.charCodeAt(i);
    let rep;
    if (c === 38) rep = '&amp;';
    else if (c === 60) rep = '&lt;';
    else if (c === 62) rep = '&gt;';
    else if (c === 34) rep = '&quot;';
    else if (c === 39) rep = '&#39;';
    else continue;
    if (last !== i) out += s.slice(last, i);
    out += rep;
    last = i + 1;
  }
  return last === 0 ? s : last === len ? out : out + s.slice(last);
}

// Fast ASCII-first slugify matching NFKD + \p{M} strip + [a-z0-9] + '-' collapse + 64 cap.
const SLUG_MAX = 64;
const COMBINING_RE = /\p{M}/gu;

function slugify(text) {
  let isAscii = true;
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) > 127) {
      isAscii = false;
      break;
    }
  }
  const plain = isAscii ? text : text.normalize('NFKD').replace(COMBINING_RE, '');
  let out = '';
  let gap = false;
  for (let i = 0; i < plain.length; i++) {
    let code = plain.charCodeAt(i);
    if (code >= 0xd800 && code <= 0xdbff && i + 1 < plain.length) {
      // Surrogate pair: outside a-z0-9, counts as non-keep gap.
      i++;
      if (out.length > 0) gap = true;
      continue;
    }
    if (code >= 65 && code <= 90) code += 32;
    if ((code >= 97 && code <= 122) || (code >= 48 && code <= 57)) {
      if (gap) {
        out += '-';
        gap = false;
      }
      out += String.fromCharCode(code);
      if (out.length >= SLUG_MAX) break;
    } else if (out.length > 0) {
      gap = true;
    }
  }
  if (out.length > SLUG_MAX) out = out.slice(0, SLUG_MAX);
  let end = out.length;
  while (end > 0 && out.charCodeAt(end - 1) === 45) end--;
  if (end === 0) return 'untitled';
  return end === out.length ? out : out.slice(0, end);
}

function uniqueSlug(slug, taken) {
  if (!taken.has(slug)) {
    taken.add(slug);
    return slug;
  }
  let n = 2;
  let out = slug + '-' + n;
  while (taken.has(out)) {
    n++;
    out = slug + '-' + n;
  }
  taken.add(out);
  return out;
}

function longDate(date) {
  const y = date.slice(0, 4);
  const m = (date.charCodeAt(5) - 48) * 10 + (date.charCodeAt(6) - 48);
  const d = (date.charCodeAt(8) - 48) * 10 + (date.charCodeAt(9) - 48);
  return `${d} ${MONTHS[m - 1]} ${y}`;
}

function wordsOf(text) {
  let count = 0;
  let inWord = false;
  const len = text.length;
  for (let i = 0; i < len; i++) {
    const c = text.charCodeAt(i);
    // Match JS \S (whitespace: 0x09-0x0d, 0x20, 0xa0, 0x1680, 0x2000-0x200a, 0x2028-0x2029, 0x202f, 0x205f, 0x3000, 0xfeff)
    const ws =
      c === 32 ||
      (c >= 9 && c <= 13) ||
      (c >= 160 &&
        (c === 0xa0 ||
          c === 0x1680 ||
          (c >= 0x2000 && c <= 0x200a) ||
          c === 0x2028 ||
          c === 0x2029 ||
          c === 0x202f ||
          c === 0x205f ||
          c === 0x3000 ||
          c === 0xfeff));
    if (ws) {
      inWord = false;
    } else if (!inWord) {
      inWord = true;
      count++;
    }
  }
  return count;
}

function dirOf(path) {
  const idx = path.lastIndexOf('/');
  return idx === -1 ? '' : path.slice(0, idx);
}

function resolvePath(dir, rel) {
  const parts = dir === '' ? [] : dir.split('/');
  let start = 0;
  const len = rel.length;
  while (start <= len) {
    let slash = rel.indexOf('/', start);
    if (slash === -1) slash = len;
    const seg = rel.slice(start, slash);
    start = slash + 1;
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else {
      parts.push(seg);
    }
  }
  return parts.join('/');
}

// ---------------- Front Matter ----------------
const FM_KEYS = new Set(['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary']);
const FM_LINE = /^([a-z_]+):(?: (.*))?$/;
const FM_DATE = /^(\d{4})-(\d{2})-(\d{2})$/;
const FM_NAME = /^[a-z0-9-]+$/;
const FM_SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const FM_TAG = /^[^"[\]]+$/;

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return m === 4 || m === 6 || m === 9 || m === 11 ? 30 : 31;
}

function fmStringOf(raw) {
  if (raw.charCodeAt(0) === 34) {
    if (raw.length < 2 || raw.charCodeAt(raw.length - 1) !== 34) {
      return { error: 'a quoted string is not closed' };
    }
    let out = '';
    let last = 1;
    const end = raw.length - 1;
    for (let i = 1; i < end; i++) {
      const c = raw.charCodeAt(i);
      if (c === 92) {
        const n = raw.charCodeAt(i + 1);
        if ((n !== 34 && n !== 92) || i + 1 === end) {
          return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        }
        if (last !== i) out += raw.slice(last, i);
        out += n === 34 ? '"' : '\\';
        i++;
        last = i + 1;
      } else if (c === 34) {
        return { error: 'a quoted string holds a stray quote' };
      }
    }
    if (last === 1) return { value: raw.slice(1, end) };
    if (last < end) out += raw.slice(last, end);
    return { value: out };
  }
  if (raw.charCodeAt(0) === 91) return { error: 'a bare string opens on [' };
  return { value: raw };
}

function fmValueOf(key, raw) {
  switch (key) {
    case 'title':
    case 'summary': {
      const s = fmStringOf(raw);
      return 'error' in s ? { error: `key "${key}": ${s.error}` } : s;
    }
    case 'date': {
      const m = FM_DATE.exec(raw);
      if (!m) return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      const y = Number(m[1]);
      const mo = Number(m[2]);
      const d = Number(m[3]);
      if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) {
        return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      }
      return { value: raw };
    }
    case 'tags': {
      if (raw.charCodeAt(0) !== 91 || raw.charCodeAt(raw.length - 1) !== 93) {
        return { error: 'key "tags": not a list [a, b]' };
      }
      const inner = raw.slice(1, -1);
      if (inner.trim() === '') return { value: [] };
      const parts = inner.split(',');
      const tags = new Array(parts.length);
      for (let i = 0; i < parts.length; i++) {
        const t = parts[i].trim();
        if (t === '' || !FM_TAG.test(t)) {
          return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
        }
        tags[i] = t;
      }
      return { value: tags };
    }
    case 'draft':
      return raw === 'true' ? { value: true } : raw === 'false' ? { value: false } : { error: 'key "draft": true or false' };
    case 'layout':
      return FM_NAME.test(raw) ? { value: raw } : { error: 'key "layout": not a template name' };
    case 'slug':
      return raw.length <= 64 && FM_SLUG.test(raw) ? { value: raw } : { error: 'key "slug": not a slug' };
    default:
      return { error: `unknown key "${key}"` };
  }
}

function isBlankAscii(s) {
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

function parseFrontMatter(text) {
  const nl0 = text.indexOf('\n');
  const firstLine = nl0 === -1 ? text : text.slice(0, nl0);
  if (firstLine !== '---') return { error: 'no front matter' };
  let pos = nl0 === -1 ? text.length : nl0 + 1;
  let closePos = -1;
  let bodyStart = text.length;
  const fmLines = [];
  while (pos <= text.length) {
    if (nl0 === -1) break;
    let nl = text.indexOf('\n', pos);
    const end = nl === -1 ? text.length : nl;
    const line = text.slice(pos, end);
    if (line === '---') {
      closePos = pos;
      bodyStart = nl === -1 ? text.length : nl + 1;
      break;
    }
    fmLines.push(line);
    if (nl === -1) break;
    pos = nl + 1;
  }
  if (closePos === -1) return { error: 'the front matter is not closed' };

  const data = Object.create(null);
  for (let i = 0; i < fmLines.length; i++) {
    const line = fmLines[i];
    if (isBlankAscii(line) || line.charCodeAt(0) === 35) continue;
    const m = FM_LINE.exec(line);
    if (!m) return { error: `line ${i + 2} is not "key: value"` };
    const key = m[1];
    let raw = m[2] ?? '';
    let rEnd = raw.length;
    while (rEnd > 0 && (raw.charCodeAt(rEnd - 1) === 32 || raw.charCodeAt(rEnd - 1) === 9)) rEnd--;
    if (rEnd !== raw.length) raw = raw.slice(0, rEnd);
    if (!FM_KEYS.has(key)) return { error: `unknown key "${key}"` };
    if (key in data) return { error: `key "${key}" is given twice` };
    if (raw === '') return { error: `key "${key}" has no value` };
    const v = fmValueOf(key, raw);
    if ('error' in v) return v;
    data[key] = v.value;
  }
  if (!('title' in data)) return { error: 'no title' };
  return { data, body: text.slice(bodyStart) };
}

// ---------------- Markdown ----------------
const ESCAPABLE = new Uint8Array(128);
for (const ch of '\\`*_[]()#+-.!>') ESCAPABLE[ch.charCodeAt(0)] = 1;

function trimSpacesTabs(s) {
  let a = 0;
  let b = s.length;
  while (a < b && (s.charCodeAt(a) === 32 || s.charCodeAt(a) === 9)) a++;
  while (b > a && (s.charCodeAt(b - 1) === 32 || s.charCodeAt(b - 1) === 9)) b--;
  return a === 0 && b === s.length ? s : s.slice(a, b);
}

function parseFenceOpen(line) {
  if (line.length < 3 || line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return null;
  let i = 3;
  const len = line.length;
  while (i < len) {
    const c = line.charCodeAt(i);
    if (
      (c >= 65 && c <= 90) ||
      (c >= 97 && c <= 122) ||
      (c >= 48 && c <= 57) ||
      c === 95 ||
      c === 43 ||
      c === 45
    ) {
      i++;
    } else break;
  }
  const langEnd = i;
  while (i < len) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return null;
    i++;
  }
  return line.slice(3, langEnd);
}

function isFenceClose(line) {
  if (line.length < 3 || line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return false;
  for (let i = 3; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

function parseHeading(line) {
  if (line.length < 3 || line.charCodeAt(0) !== 35) return null;
  let d = 1;
  while (d < 6 && d < line.length && line.charCodeAt(d) === 35) d++;
  if (d >= line.length) return null;
  let i = d;
  const c0 = line.charCodeAt(i);
  if (c0 !== 32 && c0 !== 9) return null;
  while (i < line.length && (line.charCodeAt(i) === 32 || line.charCodeAt(i) === 9)) i++;
  let end = line.length;
  while (end > i && (line.charCodeAt(end - 1) === 32 || line.charCodeAt(end - 1) === 9)) end--;
  if (end <= i) return null;
  return { depth: d, text: line.slice(i, end) };
}

function isHr(line) {
  if (line.length < 3) return false;
  const c0 = line.charCodeAt(0);
  if ((c0 !== 45 && c0 !== 42) || line.charCodeAt(1) !== c0 || line.charCodeAt(2) !== c0) return false;
  for (let i = 3; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

function endsParagraph(line) {
  return isBlankAscii(line) || parseFenceOpen(line) !== null || parseHeading(line) !== null || isHr(line);
}

function parseOrdered(line) {
  const len = line.length;
  if (len < 3) return null;
  let i = 0;
  while (i < len && i < 9) {
    const c = line.charCodeAt(i);
    if (c >= 48 && c <= 57) i++;
    else break;
  }
  if (i === 0 || i + 1 >= len || line.charCodeAt(i) !== 46 || line.charCodeAt(i + 1) !== 32) return null;
  return { start: Number(line.slice(0, i)), rest: line.slice(i + 2) };
}

const CONTINUED_RE = /^ {2,}\S/;
const WS_RE = /\s/;
const SCHEME_RE = /^[A-Za-z][A-Za-z0-9+.-]*:/;

function findUnescaped2(s, c1, c2, from) {
  const limit = s.length - 1;
  for (let j = from; j < limit; j++) {
    const c = s.charCodeAt(j);
    if (c === 92 && j + 1 < s.length) {
      const nc = s.charCodeAt(j + 1);
      if (nc < 128 && ESCAPABLE[nc]) {
        j++;
        continue;
      }
    }
    if (c === c1 && s.charCodeAt(j + 1) === c2) return j;
  }
  return -1;
}

function findUnescaped1(s, target, from) {
  const len = s.length;
  for (let j = from; j < len; j++) {
    const c = s.charCodeAt(j);
    if (c === 92 && j + 1 < len) {
      const nc = s.charCodeAt(j + 1);
      if (nc < 128 && ESCAPABLE[nc]) {
        j++;
        continue;
      }
    }
    if (c === target) return j;
  }
  return -1;
}

function findSingleStar(s, from) {
  const len = s.length;
  for (let j = from; j < len; j++) {
    const c = s.charCodeAt(j);
    if (c === 92 && j + 1 < len) {
      const nc = s.charCodeAt(j + 1);
      if (nc < 128 && ESCAPABLE[nc]) {
        j++;
        continue;
      }
    }
    if (c !== 42) continue;
    if (j + 1 < len && s.charCodeAt(j + 1) === 42) {
      j++;
      continue;
    }
    return j;
  }
  return -1;
}

function inline(s, resolveLink) {
  const len = s.length;
  let out = '';
  let last = 0;
  let i = 0;
  while (i < len) {
    const c = s.charCodeAt(i);
    if (c === 92) {
      if (i + 1 < len) {
        const nc = s.charCodeAt(i + 1);
        if (nc < 128 && ESCAPABLE[nc]) {
          if (last !== i) out += s.slice(last, i);
          out += nc === 62 ? '&gt;' : s[i + 1];
          i += 2;
          last = i;
          continue;
        }
      }
      i++;
      continue;
    }
    if (c === 96) {
      const j = s.indexOf('`', i + 1);
      if (j > i + 1) {
        if (last !== i) out += s.slice(last, i);
        out += '<code>' + escapeHtml(s.slice(i + 1, j)) + '</code>';
        i = j + 1;
        last = i;
        continue;
      }
      i++;
      continue;
    }
    if (c === 42) {
      if (i + 1 < len && s.charCodeAt(i + 1) === 42) {
        const j = findUnescaped2(s, 42, 42, i + 2);
        if (j > i + 2) {
          if (last !== i) out += s.slice(last, i);
          out += '<strong>' + inline(s.slice(i + 2, j), resolveLink) + '</strong>';
          i = j + 2;
          last = i;
        } else {
          i += 2;
        }
        continue;
      }
      const j = findSingleStar(s, i + 1);
      if (j > i + 1) {
        if (last !== i) out += s.slice(last, i);
        out += '<em>' + inline(s.slice(i + 1, j), resolveLink) + '</em>';
        i = j + 1;
        last = i;
      } else {
        i++;
      }
      continue;
    }
    if (c === 91) {
      const k = findUnescaped1(s, 93, i + 1);
      if (k !== -1 && k + 1 < len && s.charCodeAt(k + 1) === 40) {
        const m = s.indexOf(')', k + 2);
        if (m !== -1) {
          const target = s.slice(k + 2, m);
          if (target !== '' && !WS_RE.test(target)) {
            if (last !== i) out += s.slice(last, i);
            out += '<a href="' + escapeHtml(resolveLink(target)) + '">' + inline(s.slice(i + 1, k), resolveLink) + '</a>';
            i = m + 1;
            last = i;
            continue;
          }
        }
      }
      i++;
      continue;
    }
    if (c === 38 || c === 60 || c === 62 || c === 34 || c === 39) {
      if (last !== i) out += s.slice(last, i);
      out += c === 38 ? '&amp;' : c === 60 ? '&lt;' : c === 62 ? '&gt;' : c === 34 ? '&quot;' : '&#39;';
      i++;
      last = i;
      continue;
    }
    i++;
  }
  return last === 0 ? s : last === len ? out : out + s.slice(last);
}

function renderMarkdown(src, resolveLink) {
  const lines = src.split('\n');
  const ids = new Set();
  const toc = [];
  let html = '';
  let excerpt = null;
  let i = 0;
  const n = lines.length;
  while (i < n) {
    const line = lines[i];
    if (isBlankAscii(line)) {
      i++;
      continue;
    }
    const fenceLang = parseFenceOpen(line);
    if (fenceLang !== null) {
      i++;
      let codeOut = fenceLang === '' ? '<pre><code>' : `<pre><code class="language-${escapeHtml(fenceLang)}">`;
      while (i < n && !isFenceClose(lines[i])) {
        codeOut += escapeHtml(lines[i]) + '\n';
        i++;
      }
      i++;
      html += codeOut + '</code></pre>\n';
      continue;
    }
    const heading = parseHeading(line);
    if (heading !== null) {
      const id = uniqueSlug(slugify(heading.text), ids);
      const hHtml = inline(heading.text, resolveLink);
      html += `<h${heading.depth} id="${id}">${hHtml}</h${heading.depth}>\n`;
      if (heading.depth === 2 || heading.depth === 3) toc.push({ level: heading.depth, id, html: hHtml });
      i++;
      continue;
    }
    if (isHr(line)) {
      html += '<hr>\n';
      i++;
      continue;
    }
    if (line.charCodeAt(0) === 62) {
      const bLines = [];
      while (i < n && lines[i].charCodeAt(0) === 62) {
        const l = lines[i++];
        const stripped = l.length > 1 && l.charCodeAt(1) === 32 ? l.slice(2) : l.slice(1);
        const tr = trimSpacesTabs(stripped);
        if (tr !== '') bLines.push(tr);
      }
      if (bLines.length === 0) html += '<blockquote>\n</blockquote>\n';
      else html += `<blockquote>\n<p>${inline(bLines.join('\n'), resolveLink)}</p>\n</blockquote>\n`;
      continue;
    }
    const isBullet = line.length >= 2 && (line.charCodeAt(0) === 45 || line.charCodeAt(0) === 42) && line.charCodeAt(1) === 32;
    const ordered = isBullet ? null : parseOrdered(line);
    if (isBullet || ordered !== null) {
      const isOrd = ordered !== null;
      const startNum = isOrd ? ordered.start : 1;
      let listOut = isOrd ? (startNum !== 1 ? `<ol start="${startNum}">\n` : '<ol>\n') : '<ul>\n';
      let curItem = null;
      while (i < n) {
        const li = lines[i];
        if (!isOrd) {
          if (li.length >= 2 && (li.charCodeAt(0) === 45 || li.charCodeAt(0) === 42) && li.charCodeAt(1) === 32) {
            if (curItem !== null) listOut += `<li>${inline(curItem, resolveLink)}</li>\n`;
            curItem = trimSpacesTabs(li.slice(2));
            i++;
            continue;
          }
        } else {
          const om = parseOrdered(li);
          if (om !== null) {
            if (curItem !== null) listOut += `<li>${inline(curItem, resolveLink)}</li>\n`;
            curItem = trimSpacesTabs(om.rest);
            i++;
            continue;
          }
        }
        if (CONTINUED_RE.test(li)) {
          curItem += '\n' + trimSpacesTabs(li);
          i++;
        } else break;
      }
      if (curItem !== null) listOut += `<li>${inline(curItem, resolveLink)}</li>\n`;
      html += listOut + (isOrd ? '</ol>\n' : '</ul>\n');
      continue;
    }
    let para = trimSpacesTabs(line);
    i++;
    while (i < n && !endsParagraph(lines[i])) {
      para += '\n' + trimSpacesTabs(lines[i++]);
    }
    const pHtml = inline(para, resolveLink);
    if (excerpt === null) excerpt = pHtml;
    html += `<p>${pHtml}</p>\n`;
  }
  return { html, toc, excerpt: excerpt ?? '' };
}

// ---------------- Templates ----------------
const INCLUDE_DEPTH = 8;
const T_PATH = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const T_FOR = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const T_IF = /^if (not )?(\S+)$/;
const T_INCLUDE = /^include "([a-z0-9-]+)"$/;

class TemplateError extends Error {}

function pathOf(name, text) {
  if (!T_PATH.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  const len = src.length;
  while (i < len) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === len - 1) {
      text += src.slice(i);
      break;
    }
    const next = src.charCodeAt(open + 1);
    if (next !== 123 && next !== 37) {
      text += src.slice(i, open + 1);
      i = open + 1;
      continue;
    }
    text += src.slice(i, open);
    if (next === 123) {
      const raw = open + 2 < len && src.charCodeAt(open + 2) === 123;
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 'text', text });
      text = '';
      tokens.push({ t: raw ? 'raw' : 'var', path: pathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = open === 0 ? 0 : src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? len : nl;
    const standalone = isBlankAscii(src.slice(lineStart, open)) && isBlankAscii(src.slice(end, lineEnd));
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 'text', text });
    text = '';
    tokens.push({ t: 'tag', tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? len : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 'text', text });
  return tokens;
}

function parseTemplate(name, src) {
  const root = { kind: 'root', body: [] };
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 'text') body.push({ kind: 0, text: tok.text });
    else if (tok.t === 'var') body.push({ kind: 1, path: tok.path });
    else if (tok.t === 'raw') body.push({ kind: 2, path: tok.path });
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = T_FOR.exec(tag)) !== null) {
        const node = { kind: 3, name: m[1], path: pathOf(name, m[2]), body: [] };
        body.push(node);
        stack.push(node);
        body = node.body;
      } else if ((m = T_IF.exec(tag)) !== null) {
        const node = { kind: 4, not: m[1] !== undefined, path: pathOf(name, m[2]), then: [], else: null };
        body.push(node);
        stack.push(node);
        body = node.then;
      } else if (tag === 'else') {
        if (top.kind !== 4 || top.else !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.else = [];
        body = top.else;
      } else if (tag === 'endfor' || tag === 'endif') {
        const expectKind = tag === 'endfor' ? 3 : 4;
        if (top.kind !== expectKind) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.kind === 'root' ? parent.body : parent.kind === 3 ? parent.body : parent.else ?? parent.then;
      } else if ((m = T_INCLUDE.exec(tag)) !== null) {
        body.push({ kind: 5, name: m[1] });
      } else {
        throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
      }
    }
  }
  if (stack.length > 1) {
    const k = stack[stack.length - 1].kind === 3 ? 'for' : 'if';
    throw new TemplateError(`template "${name}": a ${k} is not closed`);
  }
  return root.body;
}

function lookup(scope, path) {
  let s = scope;
  const k0 = path[0];
  while (s !== null && !Object.hasOwn(s.vars, k0)) s = s.parent;
  if (s === null) return undefined;
  let v = s.vars[k0];
  for (let k = 1; k < path.length; k++) {
    if (v === null || typeof v !== 'object' || !Object.hasOwn(v, path[k])) return undefined;
    v = v[path[k]];
  }
  return v;
}

function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) {
    let out = '';
    for (let i = 0; i < v.length; i++) {
      if (i > 0) out += ', ';
      out += textOf(v[i]);
    }
    return out;
  }
  return '';
}

function truthy(v) {
  if (Array.isArray(v)) return v.length > 0;
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}

function renderNodes(nodes, scope, templates, depth) {
  let out = '';
  for (let i = 0; i < nodes.length; i++) {
    const n = nodes[i];
    switch (n.kind) {
      case 0:
        out += n.text;
        break;
      case 1:
        out += escapeHtml(textOf(lookup(scope, n.path)));
        break;
      case 2:
        out += textOf(lookup(scope, n.path));
        break;
      case 4: {
        const v = truthy(lookup(scope, n.path));
        const branch = v !== n.not ? n.then : n.else;
        if (branch !== null) out += renderNodes(branch, scope, templates, depth);
        break;
      }
      case 3: {
        const xs = lookup(scope, n.path);
        if (!Array.isArray(xs)) break;
        const len = xs.length;
        for (let k = 0; k < len; k++) {
          const vars = {
            [n.name]: xs[k],
            loop: { index: k + 1, first: k === 0, last: k === len - 1, length: len },
          };
          out += renderNodes(n.body, { vars, parent: scope }, templates, depth);
        }
        break;
      }
      case 5: {
        const t = templates.get(n.name);
        if (t !== undefined && depth < INCLUDE_DEPTH) {
          out += renderNodes(t, scope, templates, depth + 1);
        }
        break;
      }
    }
  }
  return out;
}

// ---------------- Pipeline ----------------
function linkOf(s) {
  if (s === null) return null;
  const d = s.data.date;
  return { title: s.data.title, url: s.url, date: d ?? '', date_long: d === undefined ? '' : longDate(d) };
}

function cardOf(s) {
  const d = s.data.date;
  const tRefs = s.tagRefs;
  const tags = new Array(tRefs.length);
  for (let i = 0; i < tRefs.length; i++) {
    const t = tRefs[i];
    tags[i] = { name: t.name, slug: t.slug, url: t.url };
  }
  return {
    title: s.data.title,
    url: s.url,
    date: d ?? '',
    date_long: d === undefined ? '' : longDate(d),
    summary: s.data.summary ?? '',
    excerpt: s.excerpt,
    reading_minutes: Math.max(1, Math.ceil(wordsOf(s.body) / 200)),
    tags,
  };
}

export function run(input) {
  const templates = new Map();
  const report = [];
  const tNames = Object.keys(input.templates).sort(byCodeUnits);
  for (let i = 0; i < tNames.length; i++) {
    const name = tNames[i];
    try {
      templates.set(name, parseTemplate(name, input.templates[name]));
    } catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      report.push(err.message);
    }
  }

  let base = input.site.base_url;
  let bEnd = base.length;
  while (bEnd > 0 && base.charCodeAt(bEnd - 1) === 47) bEnd--;
  if (bEnd !== base.length) base = base.slice(0, bEnd);

  const files = [...input.files].sort((x, y) => byCodeUnits(x.path, y.path));
  const sources = [];
  const statics = [];
  const staticPaths = new Set();

  for (let i = 0; i < files.length; i++) {
    const f = files[i];
    if (f.path.endsWith('.md')) {
      const raw = f.content;
      const text = raw.indexOf('\r') === -1 ? raw : raw.replace(/\r\n?/g, '\n');
      const fm = parseFrontMatter(text);
      if ('error' in fm) {
        report.push(`refused ${f.path}: ${fm.error}`);
        continue;
      }
      const data = fm.data;
      const isPost = f.path.startsWith('posts/');
      if (!isPost) {
        if (f.path.startsWith('tags/')) {
          report.push(`refused ${f.path}: tags/ is the generator's own`);
          continue;
        }
        if (f.path.startsWith('page/')) {
          report.push(`refused ${f.path}: page/ is the generator's own`);
          continue;
        }
      }
      if (isPost && !('date' in data)) {
        report.push(`refused ${f.path}: a post needs a date`);
        continue;
      }
      const layout = data.layout ?? (isPost ? 'post' : 'page');
      if (!templates.has(layout)) {
        report.push(`refused ${f.path}: layout "${layout}" is no template that parses`);
        continue;
      }
      if (data.draft === true) {
        report.push(`draft ${f.path}`);
        continue;
      }
      sources.push({
        path: f.path,
        data,
        body: fm.body,
        post: isPost,
        layout,
        slug: '',
        outDir: '',
        url: '',
        html: '',
        toc: null,
        excerpt: '',
        newer: null,
        older: null,
        tagRefs: null,
      });
    } else {
      statics.push(f);
      staticPaths.add(f.path);
    }
  }

  // Permalinks
  const taken = new Map();
  const bySource = new Map();
  for (let i = 0; i < sources.length; i++) {
    const s = sources[i];
    const dir = s.post ? 'posts' : dirOf(s.path);
    let set = taken.get(dir);
    if (set === undefined) {
      set = dir === '' ? new Set(['posts', 'tags', 'page']) : new Set();
      taken.set(dir, set);
    }
    const slug = uniqueSlug(s.data.slug ?? slugify(s.data.title), set);
    s.slug = slug;
    s.outDir = dir === '' ? slug : `${dir}/${slug}`;
    s.url = `${base}/${s.outDir}/`;
    bySource.set(s.path, s);
  }

  // Markdown
  const broken = new Set();
  for (let i = 0; i < sources.length; i++) {
    const s = sources[i];
    const dir = dirOf(s.path);
    const resolveLink = (target) => {
      const c0 = target.charCodeAt(0);
      if (c0 === 47 || c0 === 35 || SCHEME_RE.test(target)) return target;
      const hash = target.indexOf('#');
      const rel = hash === -1 ? target : target.slice(0, hash);
      const frag = hash === -1 ? '' : target.slice(hash);
      const path = rel === '' ? null : resolvePath(dir, rel);
      if (path !== null) {
        const dst = bySource.get(path);
        if (dst !== undefined) return dst.url + frag;
        if (staticPaths.has(path)) return `${base}/${path}${frag}`;
      }
      broken.add(`broken-link ${s.path} -> ${target}`);
      return target;
    };
    const md = renderMarkdown(s.body, resolveLink);
    s.html = md.html;
    s.toc = md.toc;
    s.excerpt = md.excerpt;
  }

  // Collections
  const posts = [];
  for (let i = 0; i < sources.length; i++) if (sources[i].post) posts.push(sources[i]);
  posts.sort((x, y) => byCodeUnits(y.data.date, x.data.date) || byCodeUnits(x.slug, y.slug));
  for (let k = 0; k < posts.length; k++) {
    posts[k].newer = k === 0 ? null : posts[k - 1];
    posts[k].older = k === posts.length - 1 ? null : posts[k + 1];
  }

  // Tags
  const bySlug = new Map();
  const slugCache = new Map();
  const cachedSlug = (name) => {
    let sl = slugCache.get(name);
    if (sl === undefined) {
      sl = slugify(name);
      slugCache.set(name, sl);
    }
    return sl;
  };
  for (let i = 0; i < posts.length; i++) {
    const p = posts[i];
    const rawTags = p.data.tags;
    const refs = [];
    if (rawTags && rawTags.length > 0) {
      const seen = new Set();
      for (let j = 0; j < rawTags.length; j++) {
        const name = rawTags[j];
        const slug = cachedSlug(name);
        if (seen.has(slug)) continue;
        seen.add(slug);
        let tag = bySlug.get(slug);
        if (tag === undefined) {
          tag = { name, slug, url: `${base}/tags/${slug}/`, posts: [] };
          bySlug.set(slug, tag);
        }
        tag.posts.push(p);
        refs.push(tag);
      }
    }
    p.tagRefs = refs;
  }
  for (let i = 0; i < sources.length; i++) {
    const s = sources[i];
    if (s.post) continue;
    const rawTags = s.data.tags;
    const refs = [];
    if (rawTags && rawTags.length > 0) {
      const seen = new Set();
      for (let j = 0; j < rawTags.length; j++) {
        const name = rawTags[j];
        const slug = cachedSlug(name);
        if (seen.has(slug)) continue;
        seen.add(slug);
        refs.push(bySlug.get(slug) ?? { name, slug, url: '', posts: [] });
      }
    }
    s.tagRefs = refs;
  }
  const tagsList = [...bySlug.values()].sort(
    (x, y) => y.posts.length - x.posts.length || byCodeUnits(x.slug, y.slug),
  );

  // Layouts
  const cards = new Map();
  for (let i = 0; i < posts.length; i++) cards.set(posts[i], cardOf(posts[i]));
  const site = { title: input.site.title, base_url: base };
  const allTags = tagsList.map((t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length }));
  const recent = posts.slice(0, input.site.recent).map((p) => cards.get(p));

  const render = (name, vars) => {
    const nodes = templates.get(name);
    if (nodes === undefined) return null;
    return renderNodes(nodes, { vars: { site, tags: allTags, recent, ...vars }, parent: null }, templates, 0);
  };

  const output = {};
  for (let i = 0; i < sources.length; i++) {
    const s = sources[i];
    const baseCard = s.post ? cards.get(s) : cardOf(s);
    const pageObj = {
      ...baseCard,
      slug: s.slug,
      content: s.html,
      toc: s.toc,
      newer: linkOf(s.newer),
      older: linkOf(s.older),
      is_post: s.post,
    };
    output[`${s.outDir}/index.html`] = render(s.layout, { page: pageObj });
  }

  const per = input.site.per_page;
  const pages = Math.max(1, Math.ceil(posts.length / per));
  const indexUrl = (k) => (k === 1 ? `${base}/` : `${base}/page/${k}/`);
  if (templates.has('index')) {
    for (let k = 1; k <= pages; k++) {
      const slicePosts = posts.slice((k - 1) * per, k * per).map((p) => cards.get(p));
      const html = render('index', {
        posts: slicePosts,
        pager: {
          number: k,
          total: pages,
          url: indexUrl(k),
          newer_url: k === 1 ? '' : indexUrl(k - 1),
          older_url: k === pages ? '' : indexUrl(k + 1),
        },
      });
      output[k === 1 ? 'index.html' : `page/${k}/index.html`] = html;
    }
  }
  if (templates.has('tag')) {
    for (let i = 0; i < tagsList.length; i++) {
      const t = tagsList[i];
      const html = render('tag', {
        tag: {
          name: t.name,
          slug: t.slug,
          url: t.url,
          count: t.posts.length,
          posts: t.posts.map((p) => cards.get(p)),
        },
      });
      output[`tags/${t.slug}/index.html`] = html;
    }
  }
  if (templates.has('tags')) {
    output['tags/index.html'] = render('tags', {});
  }
  for (let i = 0; i < statics.length; i++) {
    const f = statics[i];
    if (Object.hasOwn(output, f.path)) report.push(`shadowed ${f.path}: a built page has its path`);
    else output[f.path] = f.content;
  }

  const lines = [...report, ...broken].sort(byCodeUnits);
  const pageCount = sources.length - posts.length;
  lines.push(`built ${pageCount} pages, ${posts.length} posts, ${tagsList.length} tags and ${statics.length} static files`);
  output[REPORT_FILE] = lines.join('\n') + '\n';
  return output;
}
