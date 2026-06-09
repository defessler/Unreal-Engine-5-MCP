// Semver-aware version compare (TBX-F8): >0 if a newer than b, <0 older, 0 equal.
// Compares the numeric core, THEN pre-release precedence (a version WITH a
// pre-release ranks BELOW the same core without one — v0.6.0 > v0.6.0-rc2);
// build metadata (+…) is ignored. The old renderer copy stripped the whole
// `-suffix`, so v0.6.0-rc2 compared EQUAL to v0.6.0 and a real update was hidden.
export function cmpVersion(a: string, b: string): number {
  const split = (s: string) => {
    const clean = (s ?? '').trim().replace(/^v/i, '').split('+')[0];
    const [core, pre = ''] = clean.split('-');
    return { core: core.split('.').map((n) => parseInt(n, 10) || 0), pre };
  };
  const A = split(a), B = split(b);
  for (let i = 0; i < Math.max(A.core.length, B.core.length); i++) {
    const d = (A.core[i] ?? 0) - (B.core[i] ?? 0);
    if (d !== 0) return d > 0 ? 1 : -1;
  }
  if (!A.pre && !B.pre) return 0;
  if (!A.pre) return 1;
  if (!B.pre) return -1;
  const pa = A.pre.split('.'), pb = B.pre.split('.');
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const x = pa[i], y = pb[i];
    if (x === undefined) return -1;
    if (y === undefined) return 1;
    const nx = /^\d+$/.test(x), ny = /^\d+$/.test(y);
    if (nx && ny) { const d = parseInt(x, 10) - parseInt(y, 10); if (d) return d > 0 ? 1 : -1; }
    else if (nx) return -1;
    else if (ny) return 1;
    else if (x !== y) return x < y ? -1 : 1;
  }
  return 0;
}
