// Core test (docs/GPUSTRESS.md §5): long dependent f32 + integer arithmetic that keeps the shader cores
// at full clock. The result is not known in advance: a correct GPU gives the same bits every time for the
// same pipeline and inputs, so cores.rs compares each run with a reference run of the same work.

struct Params {
    n: u32,        // loop iterations (calibrated to ~100 ms a dispatch)
    seed: u32,     // changes every 60 s: a different data set, a new reference
    perturb: u32,  // invocation whose input is disturbed; 0xffffffff = none (only the fault-inject build sets it)
    pad: u32,
}
// count of all mismatches, then the first 16 as (word index, expected, actual): same layout as vram.wgsl
struct Found { count: atomic<u32>, first: array<u32, 48> }

@group(0) @binding(0) var<storage, read_write> out: array<u32>;
@group(0) @binding(1) var<uniform> p: Params;
@group(0) @binding(2) var<storage, read_write> reference: array<u32>;
@group(0) @binding(3) var<storage, read_write> found: Found;

const S8 = vec4<u32>(8u);
const S15 = vec4<u32>(15u);
const S24 = vec4<u32>(24u);

// murmur3-style 32-bit finaliser, on four lanes
fn hash4(x: vec4<u32>) -> vec4<u32> {
    var h = x;
    h ^= h >> vec4<u32>(16u);
    h *= vec4<u32>(0x85ebca6bu);
    h ^= h >> vec4<u32>(13u);
    h *= vec4<u32>(0xc2b2ae35u);
    h ^= h >> vec4<u32>(16u);
    return h;
}

// Four independent accumulators per invocation (instruction-level parallelism). Per iteration and lane:
//   f = fma(f, a, b + small term from u)   a in [0.90, 0.999), b in [0.001, 0.002): f stays in [0.001, ~2.3],
//                                          so never inf / NaN / denormal
//   u = (u * 0x9E3779B1) ^ (u >> 15) ^ bits(f)
// Every step depends on the previous one and both the f32 and the integer units are busy. Feeding u back
// into the addend keeps f's operands changing: a settled f would stop toggling the multiplier.
@compute @workgroup_size(256)
fn burn(@builtin(global_invocation_id) gid: vec3<u32>) {
    let id = gid.x;
    let lane = vec4<u32>(0u, 1u, 2u, 3u) + vec4<u32>(id * 4u);
    let s = vec4<u32>(hash4(vec4<u32>(p.seed, 0u, 0u, 0u)).x);
    var u = hash4(lane ^ s);
    let a = vec4<f32>(0.9) + vec4<f32>(hash4(u ^ vec4<u32>(0x68e31da4u)) >> S8) * (0.099 / 16777216.0);
    let b = vec4<f32>(0.001) + vec4<f32>(hash4(u ^ vec4<u32>(0xb5297a4du)) >> S8) * (0.001 / 16777216.0);
    var f = vec4<f32>(1.0) + vec4<f32>(hash4(u ^ vec4<u32>(0x1b873593u)) >> S8) * (1.0 / 16777216.0);
    if id == p.perturb { u.x ^= 1u; }
    for (var i = 0u; i < p.n; i++) {
        f = fma(f, a, b + vec4<f32>(u >> S24) * 1.0e-6);
        u = (u * vec4<u32>(0x9E3779B1u)) ^ (u >> S15) ^ bitcast<vec4<u32>>(f);
    }
    out[id * 4u] = u.x;
    out[id * 4u + 1u] = u.y;
    out[id * 4u + 2u] = u.z;
    out[id * 4u + 3u] = u.w;
}

@compute @workgroup_size(256)
fn compare(@builtin(global_invocation_id) gid: vec3<u32>) {
    for (var j = 0u; j < 4u; j++) {
        let i = gid.x * 4u + j;
        let want = reference[i];
        let got = out[i];
        if got != want {
            let slot = atomicAdd(&found.count, 1u);
            if slot < 16u {
                found.first[slot * 3u] = i;
                found.first[slot * 3u + 1u] = want;
                found.first[slot * 3u + 2u] = got;
            }
        }
    }
}
