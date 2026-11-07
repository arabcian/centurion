//! Minimal data parallelism on `std::thread::scope` (no extra crates: the Gentoo build stays offline).
//!
//! * `threads()` - worker count: `CENTURION_THREADS` if set (1 = everything serial), else every
//!   online CPU.
//! * `map(n, f)` - f(0..n) on all workers, results in index order. Items are handed out one at a
//!   time from a shared counter, so uneven items (a long column next to a short one) balance
//!   themselves. Runs inline when there is one item or one worker, or when called from inside
//!   another `map` (no nested oversubscription: the outer level already holds every CPU).
//! * `rows(data, row_len, cost, f)` - f(first_row, rows) on disjoint mutable row blocks of a
//!   row-major matrix, block sizes balanced by `cost(row)`.

use std::cell::Cell;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::OnceLock;

thread_local! { static INSIDE: Cell<bool> = Cell::new(false); }

/// Worker threads available to one parallel section.
pub fn threads() -> usize {
    static N: OnceLock<usize> = OnceLock::new();
    *N.get_or_init(|| {
        std::env::var("CENTURION_THREADS").ok().and_then(|s| s.trim().parse::<usize>().ok()).filter(|n| *n >= 1)
            .unwrap_or_else(|| std::thread::available_parallelism().map_or(1, |n| n.get()))
            .min(256)
    })
}

/// Workers for this call: 1 inside a worker (the outer section already uses every CPU).
fn width(items: usize) -> usize { if INSIDE.with(|c| c.get()) { 1 } else { threads().min(items).max(1) } }

/// Workers for a section of `items` independent pieces (1 inside a worker).
pub fn threads_for(items: usize) -> usize { width(items) }

/// Runs `f` as a worker: parallel sections inside it run inline.
pub fn as_worker<R>(f: impl FnOnce() -> R) -> R { let _g = Inside::enter(); f() }

struct Inside(bool);
impl Inside { fn enter() -> Inside { Inside(INSIDE.with(|c| c.replace(true))) } }
impl Drop for Inside { fn drop(&mut self) { let was = self.0; INSIDE.with(|c| c.set(was)); } }

/// f(i) for i in 0..n on every worker; results in index order.
pub fn map<R: Send>(n: usize, f: impl Fn(usize) -> R + Sync) -> Vec<R> {
    let w = width(n);
    if w <= 1 { return (0..n).map(f).collect(); }
    let next = AtomicUsize::new(0);
    let mut parts: Vec<Vec<(usize, R)>> = std::thread::scope(|s| {
        let hs: Vec<_> = (0..w).map(|_| s.spawn(|| {
            let _g = Inside::enter();
            let mut out = Vec::new();
            loop {
                let i = next.fetch_add(1, Ordering::Relaxed);
                if i >= n { break; }
                out.push((i, f(i)));
            }
            out
        })).collect();
        hs.into_iter().map(|h| h.join().unwrap_or_else(|e| std::panic::resume_unwind(e))).collect()
    });
    let mut slots: Vec<Option<R>> = (0..n).map(|_| None).collect();
    for p in parts.iter_mut() { for (i, r) in p.drain(..) { slots[i] = Some(r); } }
    slots.into_iter().map(|r| r.expect("par::map: every index is computed once")).collect()
}

/// f(first_row, block) over disjoint row blocks of a row-major `data` (rows of `row_len`), with
/// about equal total `cost(row)` per block (a few blocks per worker for balance).
pub fn rows<T: Send>(data: &mut [T], row_len: usize, cost: impl Fn(usize) -> f64, f: impl Fn(usize, &mut [T]) + Sync) {
    if row_len == 0 || data.is_empty() { return; }
    let nrows = data.len() / row_len;
    let w = width(nrows);
    if w <= 1 { f(0, data); return; }
    let total: f64 = (0..nrows).map(&cost).sum::<f64>().max(1e-12);
    let pieces = (w * 4).min(nrows);
    let mut bounds = vec![0usize];
    let mut acc = 0.0;
    for r in 0..nrows {
        acc += cost(r);
        if acc >= total * bounds.len() as f64 / pieces as f64 && bounds.len() < pieces && r + 1 < nrows { bounds.push(r + 1); }
    }
    bounds.push(nrows);
    let mut blocks: Vec<(usize, &mut [T])> = Vec::with_capacity(bounds.len());
    let mut rest = data;
    for b in bounds.windows(2) {
        let (head, tail) = std::mem::take(&mut rest).split_at_mut((b[1] - b[0]) * row_len);
        blocks.push((b[0], head));
        rest = tail;
    }
    let next = AtomicUsize::new(0);
    let cells: Vec<std::sync::Mutex<Option<(usize, &mut [T])>>> = blocks.into_iter().map(|b| std::sync::Mutex::new(Some(b))).collect();
    std::thread::scope(|s| {
        for _ in 0..w {
            s.spawn(|| {
                let _g = Inside::enter();
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    let Some(c) = cells.get(i) else { break };
                    if let Some((r0, blk)) = c.lock().ok().and_then(|mut g| g.take()) { f(r0, blk); }
                }
            });
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn map_keeps_order_and_rows_cover_everything() {
        let v = map(1000, |i| i * 2);
        assert!(v.iter().enumerate().all(|(i, x)| *x == i * 2));
        let mut m = vec![0usize; 37 * 5];
        rows(&mut m, 5, |r| r as f64 + 1.0, |r0, blk| for (k, row) in blk.chunks_mut(5).enumerate() { for x in row { *x = r0 + k; } });
        assert!(m.chunks(5).enumerate().all(|(r, row)| row.iter().all(|x| *x == r)));
        // Nested: inner map runs inline, results unchanged.
        let nested = map(8, |i| map(4, |j| i * 10 + j).iter().sum::<usize>());
        assert_eq!(nested, (0..8).map(|i| (0..4).map(|j| i * 10 + j).sum::<usize>()).collect::<Vec<_>>());
    }
}
