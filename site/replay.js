"use strict";
// Plays one recorded story from bench/record_demo. Every step is a pure function
// of the recording, so seeking anywhere gives the same state as playing to it,
// and each key keeps one element for the whole story.
(() => {
  const R = window.RECORDING;
  const $ = (id) => document.getElementById(id);
  const fmt = (n) => Number(n).toLocaleString("en-US");
  const clamp = (value, low, high) => Math.min(high, Math.max(low, value));
  const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");

  const CHAPTER = { PREDICT: 0, CORRECT: 1, FOUND: 2, INSERT: 3, REBUILD: 4, AGAIN: 5 };
  const GAP = 1.6; // Slots of air between the two leaves after the rebuild.
  const BUFFER_SLOTS = 38; // The buffer's peak, which also sets the first scale.

  /* The recording */

  const queryKey = BigInt(R.query_key);
  const baseKeys = R.leaf_before.points.map((point) => BigInt(point[1]));
  const leafAfter = R.leaf_after;
  const leafOneKeys = leafAfter[0].keys;
  const leafTwoKeys = leafAfter[1].points.map((point) => BigInt(point[1]));

  // The slope is fixed-point, 2^64 to one. This is the prediction before it is
  // floored, so the drawn line meets the query where the real model does.
  const modelPosition = (leaf, key) => {
    const distance = BigInt(key) - BigInt(leaf.first_key);
    return Number((distance * BigInt(leaf.slope)) >> 40n) / 2 ** 24;
  };

  // The same steps as branchless_lower_bound in include/ali/types.h.
  function searchSteps(keys, first, count, key) {
    const steps = [];
    let lo = first;
    let len = count;
    while (len > 1) {
      const half = Math.floor(len / 2);
      const probe = lo + half - 1;
      const less = keys[probe] < key;
      if (less) lo += half;
      len -= half;
      steps.push({ probe, less, lo, len });
    }
    if (len === 1) steps.push({ probe: lo, less: keys[lo] < key, lo, len, last: true });
    return steps;
  }

  const writesSeen = new Map();
  const keys = [];
  baseKeys.forEach((key, position) => keys.push({ key, position, stored: true }));
  R.inserts.forEach((insert, index) => {
    const key = BigInt(insert.key);
    if (!writesSeen.has(key)) {
      writesSeen.set(key, index);
      keys.push({ key, arrives: index, stored: false });
    }
  });
  keys.sort((a, b) => (a.key < b.key ? -1 : 1));
  keys.forEach((item, rank) => {
    item.merged = rank;
    item.y = Number(item.key - keys[0].key) / Number(keys[keys.length - 1].key - keys[0].key);
  });
  const queryItem = keys.find((item) => item.key === queryKey);

  // A frame is everything a step shows. Durations only pace the replay. For the
  // burst they follow the gaps between recorded operations, so its rhythm is real.
  function buildFrames() {
    const frames = [];
    const add = (fields) => frames.push({ chapter: -1, op: R.meta.query_op, dur: 2000, ...fields });

    add({
      dur: 1000,
      caption: "Each bar is a stored key, taller for a larger key. The job is to find the bar that reaches the query line.",
    });

    const predicted = R.query.predicted;
    const crossing = modelPosition(R.leaf_before, queryKey);
    const searched = R.query.window_hi - R.query.window_lo;
    const lookup = { model: true, ring: crossing, head: crossing, label: `Predicted ${predicted}`, probe: predicted };
    add({
      ...lookup, chapter: CHAPTER.PREDICT, dur: 1900,
      caption: `The leaf's model is one straight line. It meets the query at position ${predicted}, after one multiply.`,
    });
    add({
      ...lookup, chapter: CHAPTER.PREDICT, dur: 2100, window: [R.query.window_lo, searched], probe: null,
      caption: `This leaf's worst errors were ${R.leaf_before.over} and ${R.leaf_before.under} positions when it was built, so only ${searched} keys need checking.`,
    });
    const compare = (step, chapter, offset, merged) => {
      const shown = step.probe + offset;
      const told = step.last
        ? `With one key left, position ${step.probe} ${step.less ? "is below the line, so the answer is just past it." : "is not below the line, so it is the answer."}`
        : `Position ${step.probe} ${step.less ? "is below the line, so the answer is to the right." : "is not below the line, so the answer is here or to the left."}`;
      return { step, shown, told, chapter, merged };
    };
    searchSteps(baseKeys, R.query.window_lo, searched, queryKey).forEach((step) => {
      const c = compare(step, CHAPTER.CORRECT, 0, false);
      add({
        model: true, ring: crossing, head: c.shown + 0.5, label: `Position ${step.probe}`, probe: c.shown,
        window: [step.lo, step.len], error: true, chapter: c.chapter, dur: 1050, caption: c.told,
      });
    });
    add({
      model: true, ring: crossing, head: R.query.position + 0.5, label: `Found at ${R.query.position}`,
      window: [R.query.position, 1], settled: true, error: true, found: true, chapter: CHAPTER.FOUND, dur: 2300,
      caption: `The prediction was off by ${R.query.position - predicted}. A bad line can cost time, but the key itself is always checked.`,
    });

    let distinct = 0;
    R.inserts.forEach((insert, index) => {
      const repeat = insert.delta_keys === distinct;
      distinct = insert.delta_keys;
      const gap = index === 0 ? 0 : insert.op - R.inserts[index - 1].op;
      add({
        model: true, found: true, chapter: CHAPTER.INSERT, op: insert.op,
        written: index + 1, buffered: distinct, latest: BigInt(insert.key), repeat,
        dur: index === 0 ? 1500 : Math.round(clamp(gap * 2.2, 42, 400)),
        caption: "Inserts go to a small sorted buffer beside the leaf. The stored keys and their line stay as they are.",
      });
    });

    const inserted = R.inserts.length;
    const required = (1 + R.rebuild.margin) * R.rebuild.merge_cost_ns;
    add({
      model: true, found: true, chapter: CHAPTER.REBUILD, op: R.meta.rebuild_op,
      written: inserted, buffered: R.rebuild.delta_keys, meter: true, dur: 3100,
      caption: `A read at operation ${fmt(R.meta.rebuild_op)} tips the balance.`,
      spoken: `A read at operation ${fmt(R.meta.rebuild_op)} tips the balance. Expected saving ${fmt(Math.round(R.rebuild.expected_saving_ns))} nanoseconds beats the ${fmt(required)} it has to clear.`,
    });
    const [firstLeaf, secondLeaf] = leafAfter;
    add({
      model: true, found: true, merged: true, chapter: CHAPTER.REBUILD, op: R.meta.rebuild_op, dur: 2800,
      caption: `The buffer merges in and one line no longer fits, so the leaf splits into ${firstLeaf.keys} and ${secondLeaf.keys} keys.`,
    });

    // Positions in query_after are local to the second leaf.
    const again = R.query_after;
    const offset = leafOneKeys;
    const crossingAgain = modelPosition(secondLeaf, queryKey);
    const before = { model: true, found: true, merged: true, op: R.meta.rebuild_op, chapter: CHAPTER.AGAIN };
    const againSearched = again.window_hi - again.window_lo;
    add({
      ...before, ring: crossingAgain + offset, head: crossingAgain + offset, label: `Predicted ${again.predicted}`,
      dur: 1700, caption: `The same key now meets a new line, which predicts position ${again.predicted} in the second leaf.`,
    });
    add({
      ...before, ring: crossingAgain + offset, head: crossingAgain + offset, label: `Predicted ${again.predicted}`,
      window: [again.window_lo + offset, againSearched], dur: 1800,
      caption: `This line's worst errors are ${secondLeaf.over} and ${secondLeaf.under}, so the window is ${againSearched} keys instead of ${searched}.`,
    });
    searchSteps(leafTwoKeys, again.window_lo, againSearched, queryKey).forEach((step) => {
      const c = compare(step, CHAPTER.AGAIN, offset, true);
      add({
        ...before, ring: crossingAgain + offset, head: c.shown + 0.5, label: `Position ${step.probe}`, probe: c.shown,
        window: [step.lo + offset, step.len], error: true, dur: 620, caption: c.told,
      });
    });
    add({
      ...before, ring: crossingAgain + offset, head: again.position + offset + 0.5, label: `Found at ${again.position}`,
      window: [again.position + offset, 1], settled: true, error: true, dur: 1600,
      caption: `Found at position ${again.position}, from a window of ${againSearched} with an empty buffer. At full scale, rebuilds like this barely paid.`,
    });

    let at = 0;
    for (const frame of frames) {
      frame.at = at;
      at += frame.dur;
    }
    return { frames, total: at };
  }

  const { frames, total } = buildFrames();
  const chapterStarts = Object.values(CHAPTER).map((chapter) => frames.findIndex((frame) => frame.chapter === chapter));

  /* The scene's objects. Each is made once and only ever moved. */

  const scene = $("scene");
  const make = (className, text) => {
    const node = document.createElement("div");
    node.className = className;
    if (text) node.textContent = text;
    scene.appendChild(node);
    return node;
  };
  const band = make("band");
  for (const item of keys) {
    item.node = make("bar");
  }
  const ruleA = make("rule");
  const ruleB = make("rule");
  const ruleBuffer = make("rule");
  const rail = make("rail");
  const modelA = make("model");
  const modelB = make("model");
  const queryLine = make("query-line");
  const headLine = make("head-line");
  const error = make("error");
  const ring = make("ring");
  const queryTag = make("tag query", `Query ${fmt(R.query_key)}`);
  const headTag = make("tag active");
  const leafTagA = make("tag");
  const leafTagB = make("tag", `Leaf 2, ${leafAfter[1].keys} keys`);
  const countTag = make("tag count");
  const bufferTag = make("tag", "Write buffer");
  const bufferCount = make("tag right");

  const tracks = new Map();
  // Style writes are compared first, so a step touches only what actually moved.
  const put = (node, transform, opacity) => {
    let state = tracks.get(node);
    if (!state) tracks.set(node, (state = {}));
    if (state.transform !== transform) node.style.transform = state.transform = transform;
    if (state.opacity !== opacity) node.style.opacity = state.opacity = opacity;
  };
  const setText = (node, text) => {
    if (node.textContent !== text) node.textContent = text;
  };

  let box = null;
  function measure() {
    const width = scene.clientWidth;
    const height = scene.clientHeight;
    const pad = width < 600 ? 12 : 24;
    const inner = width - pad * 2;
    const bufferBars = clamp(Math.round(height * 0.17), 28, 72);
    const bufferBase = height - 10;
    const bufferTitle = bufferBase - bufferBars - 22;
    const base = bufferTitle - 30;
    const top = 26;
    box = {
      width, height, pad, inner, top, base, bufferBase, bufferBars, bufferTitle,
      slot: inner / BUFFER_SLOTS,
      mergedSlot: inner / (keys.length + GAP),
    };
  }

  const barTop = (item) => box.base - (0.06 + 0.94 * item.y) * (box.base - box.top);
  const queryY = () => Math.round(barTop(queryItem));

  const slotLeft = (slot, merged) => {
    const size = merged ? box.mergedSlot : box.slot;
    return box.pad + slot * size;
  };
  const mergedSlot = (position) => position + (position >= leafOneKeys ? GAP : 0);

  function barWidth(size) {
    return Math.max(size - (size >= 12 ? 2 : 1), 2);
  }

  function toneOf(item, frame) {
    if (frame.latest === item.key) return "latest";
    if (item === queryItem && frame.found) return "found";
    const position = frame.merged ? item.merged : item.stored ? item.position : null;
    if (position !== null && frame.probe === position) return "probe";
    if (frame.window && !frame.settled) {
      const [lo, len] = frame.window;
      if (position !== null && position >= lo && position < lo + len) return "window";
      if (position !== null) return "dim";
    }
    return item.stored ? "stored" : "buffered";
  }

  function placeLine(node, leaf, offset, frame, visible) {
    const size = frame.merged ? box.mergedSlot : box.slot;
    const start = { x: slotLeft(offset, frame.merged), y: barTop(keys.find((item) => item.key === BigInt(leaf.first_key))) };
    const lastKey = BigInt(leaf.last_key);
    const end = {
      x: start.x + modelPosition(leaf, lastKey) * size,
      y: barTop(keys.find((item) => item.key === lastKey)),
    };
    const length = Math.hypot(end.x - start.x, end.y - start.y);
    const angle = Math.atan2(end.y - start.y, end.x - start.x);
    const shown = visible ? length : 0;
    put(node, `translate(${start.x.toFixed(2)}px,${start.y.toFixed(2)}px) rotate(${angle.toFixed(5)}rad) scaleX(${shown.toFixed(2)})`, visible ? 0.72 : 0);
  }

  function renderScene(frame) {
    const { pad, inner, base, top, bufferBase, bufferBars, bufferTitle } = box;
    const merged = Boolean(frame.merged);
    const size = merged ? box.mergedSlot : box.slot;
    const left = (slot) => slotLeft(slot, merged);

    // The buffer is sorted, so a new key splices in and pushes its neighbours.
    const arrived = keys.filter((item) => !item.stored && item.arrives < (frame.written || 0));
    const rank = new Map(arrived.map((item, index) => [item, index]));
    const buffered = frame.buffered || 0;

    for (const item of keys) {
      let x, y, height, opacity = 1;
      if (merged) {
        x = left(mergedSlot(item.merged));
        y = barTop(item);
        height = base - y;
      } else if (item.stored) {
        x = left(item.position);
        y = barTop(item);
        height = base - y;
      } else {
        const here = rank.has(item);
        const slot = here ? rank.get(item) : arrived.filter((other) => other.key < item.key).length;
        height = here ? 4 + item.y * (bufferBars - 4) : 0;
        x = left(slot);
        y = bufferBase - height;
        opacity = here ? 1 : 0;
      }
      const width = barWidth(size);
      put(item.node, `translate(${x.toFixed(2)}px,${y.toFixed(2)}px) scale(${width.toFixed(2)},${height.toFixed(2)})`, opacity);
      const tone = toneOf(item, frame);
      if (item.node.dataset.tone !== tone) item.node.dataset.tone = tone;
    }

    const lineY = queryY();
    put(queryLine, `translate(${pad}px,${lineY}px) scaleX(${inner})`, 1);
    put(queryTag, `translate(${pad}px,${lineY - 21}px)`, 1);

    // Window: one band and one rail that narrow, and later jump to the new leaf.
    if (frame.window) {
      const [lo, len] = frame.window;
      const from = merged ? mergedSlot(lo) : lo;
      const x = left(from);
      const width = Math.max(len * size - 1, 2);
      put(band, `translate(${x.toFixed(2)}px,${top}px) scale(${width.toFixed(2)},${base - top})`, 1);
      put(rail, `translate(${x.toFixed(2)}px,${base + 3}px) scale(${width.toFixed(2)},2)`, 1);
      setText(countTag, `${len} ${len === 1 ? "key" : "keys"}`);
      const half = countTag.offsetWidth / 2;
      put(countTag, `translate(${clamp(x + width / 2 - half, pad, pad + inner - half * 2).toFixed(2)}px,${base + 8}px)`, 1);
    } else {
      put(band, tracks.get(band)?.transform || `translate(${pad}px,${top}px) scale(1,${base - top})`, 0);
      put(rail, tracks.get(rail)?.transform || `translate(${pad}px,${base + 3}px) scale(1,2)`, 0);
      put(countTag, tracks.get(countTag)?.transform || `translate(${pad}px,${base + 8}px)`, 0);
    }

    // The two model lines start as one. At the rebuild the second peels away.
    placeLine(modelA, merged ? leafAfter[0] : R.leaf_before, 0, frame, Boolean(frame.model));
    if (merged) placeLine(modelB, leafAfter[1], mergedSlot(leafOneKeys), frame, true);
    else placeLine(modelB, R.leaf_before, 0, frame, false);

    // Baselines split with the leaf.
    const endA = merged ? left(leafOneKeys) : left(R.leaf_before.keys);
    put(ruleA, `translate(${pad}px,${base}px) scale(${(endA - pad - 1).toFixed(2)},1)`, 1);
    const startB = merged ? left(mergedSlot(leafOneKeys)) : endA;
    const endB = merged ? left(mergedSlot(keys.length)) : endA;
    put(ruleB, `translate(${startB.toFixed(2)}px,${base}px) scale(${Math.max(endB - startB - 1, 1).toFixed(2)},1)`, merged ? 1 : 0);
    setText(leafTagA, merged ? `Leaf 1, ${leafAfter[0].keys} keys` : `One leaf, ${R.leaf_before.keys} stored keys`);
    put(leafTagA, `translate(${pad}px,${base + 8}px)`, 1);
    put(leafTagB, `translate(${startB.toFixed(2)}px,${base + 8}px)`, merged ? 1 : 0);

    // One line follows the active position, up from its bar to the query.
    if (frame.head !== undefined) {
      const x = merged && frame.head >= leafOneKeys ? left(frame.head + GAP) : left(frame.head);
      put(headLine, `translate(${x.toFixed(2)}px,${lineY}px) scaleY(${base - lineY})`, 1);
      setText(headTag, frame.label);
      const half = headTag.offsetWidth / 2;
      put(headTag, `translate(${clamp(x - half, pad, pad + inner - half * 2).toFixed(2)}px,${lineY - 21}px)`, 1);
      const ringX = merged ? left(frame.ring + GAP) : left(frame.ring);
      put(ring, `translate(${(ringX - 4.5).toFixed(2)}px,${(lineY - 4).toFixed(2)}px)`, 1);
      const from = Math.min(ringX, x);
      const span = frame.error ? Math.abs(x - ringX) : 0;
      put(error, `translate(${from.toFixed(2)}px,${lineY - 1}px) scale(${span.toFixed(2)},3)`, frame.error ? 1 : 0);
    } else {
      for (const node of [headLine, headTag, ring, error]) {
        const state = tracks.get(node);
        put(node, state?.transform || "translate(0px,0px)", 0);
      }
    }

    put(bufferTag, `translate(${pad}px,${bufferTitle}px)`, 1);
    put(ruleBuffer, `translate(${pad}px,${bufferBase}px) scale(${inner},1)`, 1);
    const written = frame.written || 0;
    let count = "Empty";
    if (merged) count = `Empty after merging ${R.rebuild.delta_keys} keys`;
    else if (written > 0) count = `${buffered} ${buffered === 1 ? "key" : "keys"}, insert ${written} of ${R.inserts.length}${frame.repeat && box.width >= 600 ? " rewrites a buffered key" : ""}`;
    setText(bufferCount, count);
    put(bufferCount, `translate(-${pad}px,${bufferTitle}px)`, 1);
  }

  /* The words and controls */

  const caption = $("caption");
  const status = $("status");
  const opNode = $("op");
  const meter = $("meter");
  const chapterButtons = [...$("chapters").children];
  const playButton = $("play");
  const backButton = $("back");
  const nextButton = $("next");
  const time = $("time");

  const required = (1 + R.rebuild.margin) * R.rebuild.merge_cost_ns;
  const widest = Math.max(R.rebuild.expected_saving_ns, required);
  $("saving-value").textContent = `${fmt(Math.round(R.rebuild.expected_saving_ns))} ns`;
  $("cost-value").textContent = `${fmt(required)} ns`;

  function renderMeter(frame) {
    meter.classList.toggle("on", Boolean(frame.meter));
    $("saving").style.transform = `scaleX(${frame.meter ? R.rebuild.expected_saving_ns / widest : 0})`;
    $("cost").style.transform = `scaleX(${frame.meter ? required / widest : 0})`;
  }

  let currentIndex = 0;
  let clock = 0;
  let playing = false;
  let lastNow = 0;
  let scrubbing = false;

  function frameAt(moment) {
    let low = 0;
    let high = frames.length - 1;
    while (low < high) {
      const middle = Math.ceil((low + high) / 2);
      if (frames[middle].at <= moment) low = middle;
      else high = middle - 1;
    }
    return low;
  }

  function paceFor(from, to) {
    if (reducedMotion.matches) return "none";
    if (scrubbing) return "scrub";
    if (Boolean(from.merged) !== Boolean(to.merged)) return "merge";
    if (from.chapter === CHAPTER.INSERT && to.chapter === CHAPTER.INSERT) return "quick";
    return "calm";
  }

  function announce(frame, previous) {
    // While playing, say each chapter once. A burst of 83 updates helps nobody.
    if (playing && frame.chapter === previous.chapter) return;
    status.textContent = frame.spoken || frame.caption;
  }

  function renderWords(frame) {
    setText(caption, frame.caption);
    setText(opNode, fmt(frame.op));
    renderMeter(frame);
    chapterButtons.forEach((button, index) => {
      const pressed = String(index === frame.chapter);
      if (button.getAttribute("aria-pressed") !== pressed) button.setAttribute("aria-pressed", pressed);
    });
    time.setAttribute("aria-valuetext", frame.caption);
  }

  function renderControls() {
    const finished = clock >= total;
    const label = playing ? "pause" : finished ? "replay" : "play";
    for (const span of playButton.children) span.classList.toggle("on", span.dataset.label === label);
    backButton.setAttribute("aria-disabled", String(clock === 0));
    nextButton.setAttribute("aria-disabled", String(finished));
  }

  function setClock(moment) {
    clock = clamp(moment, 0, total);
    const shown = reducedMotion.matches && playing ? frames[frameAt(clock)].at : clock;
    if (Number(time.value) !== Math.round(shown)) time.value = String(Math.round(shown));
    time.style.setProperty("--f", (shown / total).toFixed(5));
    const index = frameAt(clock);
    if (index === currentIndex) return;
    const from = frames[currentIndex];
    const to = frames[index];
    currentIndex = index;
    scene.dataset.pace = paceFor(from, to);
    renderScene(to);
    renderWords(to);
    announce(to, from);
  }

  function tick(now) {
    if (!playing) return;
    const moment = clock + (now - lastNow);
    lastNow = now;
    if (moment >= total) {
      playing = false;
      setClock(total);
      renderControls();
      return;
    }
    setClock(moment);
    requestAnimationFrame(tick);
  }

  function play() {
    if (playing) return;
    if (clock >= total) setClock(0);
    playing = true;
    lastNow = performance.now();
    renderControls();
    requestAnimationFrame(tick);
  }

  function pause() {
    if (!playing) return;
    playing = false;
    renderControls();
  }

  function seekFrame(index) {
    pause();
    if (index >= frames.length) setClock(total);
    else setClock(frames[clamp(index, 0, frames.length - 1)].at);
    renderControls();
  }

  function stepBack() {
    // A press part way through a step goes to that step's start, as a timeline does.
    const frame = frames[currentIndex];
    seekFrame(clock >= total || clock > frame.at + 240 ? currentIndex : currentIndex - 1);
  }

  const stepForward = () => seekFrame(currentIndex + 1);

  playButton.addEventListener("click", () => (playing ? pause() : play()));
  backButton.addEventListener("click", () => {
    if (clock > 0) stepBack();
  });
  nextButton.addEventListener("click", () => {
    if (clock < total) stepForward();
  });
  chapterButtons.forEach((button, chapter) => {
    // Jumping keeps playing if it was playing, so this can interrupt a run.
    button.addEventListener("click", () => {
      setClock(frames[chapterStarts[chapter]].at);
      renderControls();
    });
  });

  time.max = String(total);
  time.addEventListener("input", () => {
    pause();
    scrubbing = true;
    setClock(Number(time.value));
    renderControls();
  });
  time.addEventListener("change", () => {
    scrubbing = false;
  });
  time.addEventListener("keydown", (event) => {
    if (event.key === "ArrowRight" || event.key === "ArrowUp") {
      event.preventDefault();
      if (clock < total) stepForward();
    } else if (event.key === "ArrowLeft" || event.key === "ArrowDown") {
      event.preventDefault();
      if (clock > 0) stepBack();
    } else if (event.key === "PageUp" || event.key === "PageDown") {
      event.preventDefault();
      const forward = event.key === "PageUp";
      const next = forward
        ? chapterStarts.find((start) => start > currentIndex)
        : [...chapterStarts].reverse().find((start) => start < currentIndex);
      seekFrame(next === undefined ? (forward ? frames.length : 0) : next);
    }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") pause();
    if (event.target.matches("input, textarea, select")) return;
    if (event.altKey || event.ctrlKey || event.metaKey) return;
    if (event.key === "ArrowRight" && clock < total) stepForward();
    if (event.key === "ArrowLeft" && clock > 0) stepBack();
  });

  const track = $("track");
  for (const start of chapterStarts) {
    const mark = document.createElement("i");
    mark.style.left = `calc(var(--thumb) / 2 + (100% - var(--thumb)) * ${(frames[start].at / total).toFixed(5)})`;
    track.appendChild(mark);
  }

  function redraw() {
    // A resize repositions everything at once, so nothing trails the new size.
    scene.dataset.pace = "none";
    measure();
    renderScene(frames[currentIndex]);
  }

  redraw();
  renderWords(frames[0]);
  renderControls();
  new ResizeObserver(redraw).observe(scene);
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(redraw);
})();
