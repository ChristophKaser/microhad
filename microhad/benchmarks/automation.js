const state = { occupied: false, count: 0, timers: [] };

function action(kind, value) {
  state.count += 1;
  state.timers[state.count % 32] = { kind, value, due: state.count + 60 };
  return benchmark.action(kind, value);
}

function run() {
  for (let i = 1; i <= 10000; ++i) {
    const event = i % 5 === 0 ? "occupied" : "temperature";
    if (event === "occupied") state.occupied = !state.occupied;
    if (state.occupied && i % 4 === 0) action("scene", i % 3);
    if (i % 40 === 0) action("notify", i);
    benchmark.tick();
  }
}
