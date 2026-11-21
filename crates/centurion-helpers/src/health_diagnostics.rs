//! Read-only, on-demand Health Evidence Lab. No MSR/SMU/EC/PCI config writes,
//! no GPU wake-up queries, no background polling and no persistent root state.
//! Evidence is scoped to this boot and the bounded /dev/kmsg scan.
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

fn read(path: &Path) -> String {
    std::fs::read_to_string(path).unwrap_or_default().trim().to_owned()
}

fn cpu_topology(root: &Path) -> Vec<Value> {
    let base = root.join("sys/devices/system/cpu");
    let mut cpus = Vec::new();
    if let Ok(entries) = std::fs::read_dir(base) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            let Some(cpu) = name.strip_prefix("cpu").and_then(|n| n.parse::<u32>().ok()) else { continue };
            let dir = entry.path();
            let package = read(&dir.join("topology/physical_package_id"));
            let core = read(&dir.join("topology/core_id"));
            let die = read(&dir.join("topology/die_id"));
            let siblings = read(&dir.join("topology/thread_siblings_list"));
            let mut l3_group = String::new();
            for i in 0..16 {
                let c = dir.join(format!("cache/index{i}"));
                if !c.is_dir() { continue; }
                if read(&c.join("level")) == "3" {
                    l3_group = read(&c.join("shared_cpu_list"));
                    break;
                }
            }
            // An L3-sharing domain can correspond to a CCD on some AMD CPUs,
            // but it is NOT itself proof of a firmware CCD index.
            cpus.push(json!({"logical_cpu":cpu, "package":package, "core_id":core,
                "die_id":die, "threads":siblings, "l3_shared_cpus":l3_group}));
        }
    }
    cpus.sort_by_key(|v| v["logical_cpu"].as_u64().unwrap_or(u64::MAX));
    cpus
}

fn f2fs_mounts(root: &Path) -> Vec<Value> {
    let mut mounts = Vec::new();
    let content = read(&root.join("proc/mounts"));
    for line in content.lines() {
        let fields: Vec<_> = line.split_whitespace().collect();
        if fields.len() >= 4 && fields[2] == "f2fs" {
            mounts.push(json!({"source":fields[0],"mount":fields[1],"options":fields[3]}));
        }
        if mounts.len() >= 64 { break; }
    }
    mounts
}

/// Time window is 5 minutes, within the same boot. Cases are correlation only;
/// Xid 154 is a recovery directive and cannot identify the triggering failure.
fn xid_cases(events: &[Value]) -> Vec<Value> {
    let mut cases = Vec::new();
    for (i, event) in events.iter().enumerate() {
        if event["kind"] != "xid" || event["code"] != 154 { continue; }
        let ts = event["ts_us"].as_u64().unwrap_or(0);
        let pci = event["pci"].as_str().unwrap_or("");
        let mut evidence = Vec::new();
        for prev in events[..i].iter().rev() {
            let pts = prev["ts_us"].as_u64().unwrap_or(0);
            if pts > ts || ts - pts > 300_000_000 { continue; }
            let kind = prev["kind"].as_str().unwrap_or("");
            if !["xid", "gsp", "aer", "d3cold"].contains(&kind) ||
                (kind == "xid" && prev["code"] == 154) { continue; }
            let other_pci = prev["pci"].as_str().unwrap_or("");
            if kind == "xid" && !pci.is_empty() && !other_pci.is_empty() && pci != other_pci { continue; }
            evidence.push(json!({"ts_us":pts,"kind":kind,"code":prev["code"],
                "title":prev["title"],"pci":other_pci,
                "scope": if kind == "xid" && !pci.is_empty() && pci == other_pci {"same GPU"} else {"device unverified"}}));
            if evidence.len() == 16 { break; }
        }
        evidence.reverse();
        cases.push(json!({"ts_us":ts,"pci":pci,"xid":154,"preceding":evidence,
            "assessment": if evidence.is_empty() {"No earlier related event was captured; cause unknown"}
                          else {"Temporal correlation only; no root cause established"}}));
    }
    cases
}

fn build(events: &[Value], topology: &[Value], aer: &[Value], mounts: &[Value]) -> Value {
    let by_cpu: BTreeMap<u64, &Value> = topology.iter().filter_map(|v| Some((v["logical_cpu"].as_u64()?, v))).collect();
    let mut mce = Vec::new();
    let mut f2fs = Vec::new();
    let mut radio = Vec::new();
    let mut radio_groups: BTreeMap<String, usize> = BTreeMap::new();
    let mut xid = Vec::new();   // every NVIDIA Xid of the scan window, for the per-code view (H04, H16)
    let mut seen = BTreeSet::new();
    for event in events {
        match event["kind"].as_str().unwrap_or("") {
            "mce" => {
                let cpu = event["cpu"].as_u64();
                let mapped = cpu.and_then(|id| by_cpu.get(&id));
                mce.push(json!({"ts_us":event["ts_us"],"text":event["text"],
                    "severity":event["level"],"cpu":event["cpu"],"bank":event["bank"],
                    "apic_id":event["apic_id"],"topology":mapped,
                    "confidence":if mapped.is_some() {"logical CPU mapped to current topology; CCD not verified"}
                        else {"Unknown (missing or unmapped CPU identity)"}}));
            }
            "f2fs" => f2fs.push(event.clone()),
            "xid" | "gsp" if xid.len() < 200 => xid.push(json!({"ts_us":event["ts_us"],"kind":event["kind"],"code":event["code"],
                "pci":event["pci"],"level":event["level"],"title":event["title"],"text":event["text"]})),
            "wifi" | "bluetooth" => {
                let kind = event["kind"].as_str().unwrap_or("unknown");
                let text = event["text"].as_str().unwrap_or("");
                let tokens: Vec<&str> = text.split_whitespace().collect();
                let device = if kind == "bluetooth" {
                    tokens.iter().find(|word| word.starts_with("hci") && word.ends_with(':'))
                        .map(|word| word.trim_end_matches(':').to_string())
                        .unwrap_or_else(|| "unknown controller".to_string())
                } else {
                    let driver = tokens.first().copied().unwrap_or("unknown driver");
                    let pci = tokens.iter().find(|word| word.starts_with("0000:") && word.contains('.'));
                    format!("{driver} {}", pci.map_or("unknown PCI device", |v| v.trim_end_matches(':')))
                };
                *radio_groups.entry(format!("{kind}: {device}")).or_default() += 1;
                radio.push(event.clone());
            },
            _ => {}
        }
    }
    for item in aer {
        if let Some(dev) = item["dev"].as_str() { seen.insert(dev.to_string()); }
    }
    let radio_summary: Vec<Value> = radio_groups.into_iter().map(|(device, errors)| json!({"device":device,"errors":errors})).collect();
    json!({"mce":mce,"topology":topology,"aer":aer,"xid_cases":xid_cases(events),"xid":xid,
        "f2fs_events":f2fs,"f2fs_mounts":mounts,"radio_events":radio,"radio_summary":radio_summary,
        "aer_devices":seen.len(), "event_count":events.len(),
        "limitations":"Latest 300 classified events from current boot; absence of recorded events does not prove health. No CCD index inferred from L3 topology."})
}

pub fn report() -> Value {
    let scan = crate::health::scan(0);
    if scan["ok"] != true { return scan; }
    let events = scan["events"].as_array().cloned().unwrap_or_default();
    let aer = scan["aer"].as_array().cloned().unwrap_or_default();
    let root = Path::new("/");
    let mut result = build(&events, &cpu_topology(root), &aer, &f2fs_mounts(root));
    result["ok"] = json!(true);
    result["boot_id"] = scan["boot_id"].clone();
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cpu_identifiers_do_not_infer_ccd() {
        let events = vec![json!({"kind":"mce","cpu":9,"bank":5,"ts_us":100,"level":"warn","text":"MCE"}),
                          json!({"kind":"mce","apic_id":5,"ts_us":101,"text":"MCE"})];
        let topo = vec![json!({"logical_cpu":9,"core_id":"1","l3_shared_cpus":"8-15"})];
        let r = build(&events, &topo, &[], &[]);
        assert_eq!(r["mce"][0]["topology"]["core_id"], "1");
        assert!(r["mce"][0]["confidence"].as_str().unwrap().contains("not verified"));
        assert!(r["mce"][1]["topology"].is_null());
    }
    #[test]
    fn xid_154_only_preceding_same_gpu() {
        let events = vec![json!({"kind":"xid","code":79,"pci":"0000:01:00","ts_us":2_000_000,"title":"Xid 79"}),
            json!({"kind":"xid","code":31,"pci":"0000:02:00","ts_us":3_000_000,"title":"Xid 31"}),
            json!({"kind":"xid","code":154,"pci":"0000:01:00","ts_us":4_000_000})];
        let c = xid_cases(&events);
        assert_eq!(c[0]["preceding"].as_array().unwrap().len(), 1);
        assert_eq!(c[0]["preceding"][0]["code"], 79);
        assert!(c[0]["assessment"].as_str().unwrap().contains("correlation"));
        let r = build(&events, &[], &[], &[]);
        assert_eq!(r["xid"].as_array().unwrap().len(), 3);
        assert_eq!(r["xid"][0]["code"], 79);
    }
    #[test]
    fn radio_ledger_groups_device_without_inventing_hardware_ids() {
        let events = vec![
            json!({"kind":"wifi","text":"mt7925e 0000:03:00.0: firmware timeout"}),
            json!({"kind":"wifi","text":"mt7925e 0000:03:00.0: firmware timeout"}),
            json!({"kind":"bluetooth","text":"Bluetooth: hci0: command tx timeout"})];
        let result = build(&events, &[], &[], &[]);
        assert_eq!(result["radio_summary"][0]["device"], "bluetooth: hci0");
        assert_eq!(result["radio_summary"][1]["errors"], 2);
    }
    #[test]
    fn silent_data_stays_unknown() {
        let r = build(&[], &[], &[], &[]);
        assert_eq!(r["mce"].as_array().unwrap().len(), 0);
        assert!(r["limitations"].as_str().unwrap().contains("does not prove"));
    }
}
