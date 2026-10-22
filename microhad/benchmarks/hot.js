function leaf() { return benchmark.tick(); }
function four() { return leaf(); }
function three() { return four(); }
function two() { return three(); }
function one() { return two(); }

function run() {
  let total = 0;
  for (let i = 0; i < 10000; ++i) total += one();
  return total;
}
