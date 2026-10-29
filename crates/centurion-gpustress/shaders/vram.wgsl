// VRAM test patterns (docs/GPUSTRESS.md §4). The expected value of a word depends only on
// (pattern, global word index, seed, phase), so `verify` needs no copy of the data.
// vram.rs holds the same functions in Rust; its unit test compares the two by running this shader.

struct Params {
    pattern: u32,  // 0 addr, 1 walk1, 2 walk0, 3 solid, 4 random
    seed: u32,     // the pass number
    base: u32,     // global index of this chunk's first word (chunk index x words per chunk), low 32 bits
    words: u32,    // words in this chunk
    phase: u32,    // solid only: 0 = the pass's value, 1 = its inverse
    base_hi: u32,  // the same index, high 32 bits
    pad1: u32,
    pad2: u32,
}
// count of all mismatches, then the first 16 as (global word index, expected, actual)
struct Found { count: atomic<u32>, first: array<u32, 48> }

@group(0) @binding(0) var<storage, read_write> data: array<u32>;
@group(0) @binding(1) var<uniform> p: Params;
@group(0) @binding(2) var<storage, read_write> found: Found;

// murmur3-style 32-bit finaliser
fn hash(x: u32) -> u32 {
    var h = x;
    h ^= h >> 16u;
    h *= 0x85ebca6bu;
    h ^= h >> 13u;
    h *= 0xc2b2ae35u;
    h ^= h >> 16u;
    return h;
}

// `index` is the low 32 bits of the global word index, `hi` the high 32 bits. addr and random mix
// hash(hi) in (hash(0) = 0, so below 2^32 words they are unchanged) so words 2^32 apart differ.
fn value(index: u32, hi: u32) -> u32 {
    switch p.pattern {
        case 0u: { return index ^ hash(p.seed) ^ hash(hi); }
        case 1u: { return 1u << ((index + p.seed) % 32u); }
        case 2u: { return ~(1u << ((index + p.seed) % 32u)); }
        case 3u: { return select(0u, 0xffffffffu, ((p.seed + p.phase) & 1u) == 1u); }
        default: { return hash(index ^ hash(p.seed + 0x9e3779b9u) ^ hash(hi)); }
    }
}

// High half of the global index of the word `lo` (= base + offset, wrapped): a carry when it wrapped.
fn high(lo: u32) -> u32 {
    return p.base_hi + select(0u, 1u, lo < p.base);
}

// 256 invocations x 4 consecutive words; the dispatch is 2D because a 256 MiB chunk needs 65536
// workgroups, more than the guaranteed maxComputeWorkGroupCount of 65535.
fn first_word(wg: vec3<u32>, nwg: vec3<u32>, li: u32) -> u32 {
    return ((wg.y * nwg.x + wg.x) * 256u + li) * 4u;
}

@compute @workgroup_size(256)
fn fill(@builtin(workgroup_id) wg: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>, @builtin(local_invocation_index) li: u32) {
    let first = first_word(wg, nwg, li);
    if first >= p.words { return; }  // words is a multiple of 4
    for (var j = 0u; j < 4u; j++) {
        let lo = p.base + first + j;
        data[first + j] = value(lo, high(lo));
    }
}

@compute @workgroup_size(256)
fn verify(@builtin(workgroup_id) wg: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>, @builtin(local_invocation_index) li: u32) {
    let first = first_word(wg, nwg, li);
    if first >= p.words { return; }
    for (var j = 0u; j < 4u; j++) {
        let lo = p.base + first + j;
        let want = value(lo, high(lo));
        let got = data[first + j];
        if got != want {
            let slot = atomicAdd(&found.count, 1u);
            if slot < 16u {
                found.first[slot * 3u] = lo;  // the host adds the high half
                found.first[slot * 3u + 1u] = want;
                found.first[slot * 3u + 2u] = got;
            }
        }
    }
}
