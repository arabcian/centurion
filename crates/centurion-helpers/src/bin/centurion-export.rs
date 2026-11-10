//! centurion-export — the running GUI's SensorValues (D-Bus, see dbusapi.h) for Prometheus or MQTT.
//! Runs as the user (never through pkexec); reads the session bus with dbus-send, so nothing new is
//! linked and no port is opened.
//!
//!   centurion-export prometheus [FILE] [--interval S]
//!       Prometheus text format, one gauge per value (centurion_cpu_temp_c …). FILE is replaced
//!       atomically (node_exporter's textfile collector: FILE = <dir>/centurion.prom), else stdout.
//!   centurion-export mqtt HOST [--topic PREFIX] [--interval S] [-- MOSQUITTO_PUB_ARGS…]
//!       One retained message per value on PREFIX/<key> (default centurion) via mosquitto_pub;
//!       arguments after `--` go to mosquitto_pub as they are (-p PORT, -u USER, -P PASS, …).
//!
//! Once by default; with --interval S it repeats every S seconds. Exit 1 when Centurion is not running.

use std::process::Command;

fn sensor_values() -> Result<Vec<(String, f64)>, String> {
    let out = Command::new("dbus-send")
        .args(["--session", "--print-reply", "--dest=com.centurion.Centurion", "/com/centurion/Centurion",
               "com.centurion.Centurion.SensorValues"])
        .output().map_err(|e| format!("dbus-send: {e}"))?;
    if !out.status.success() {
        return Err(format!("Centurion is not answering on the session bus: {}", String::from_utf8_lossy(&out.stderr).trim()));
    }
    Ok(parse_reply(&String::from_utf8_lossy(&out.stdout)))
}

/// dbus-send's a{sd}: `string "key"` then `double value` inside each dict entry.
fn parse_reply(text: &str) -> Vec<(String, f64)> {
    let mut key = None;
    let mut out = Vec::new();
    for line in text.lines().map(str::trim) {
        if let Some(k) = line.strip_prefix("string \"").and_then(|s| s.strip_suffix('"')) {
            key = Some(k.to_owned());
        } else if let Some(v) = line.strip_prefix("double ").and_then(|s| s.parse::<f64>().ok()) {
            if let Some(k) = key.take() { if v.is_finite() { out.push((k, v)); } }
        }
    }
    out
}

fn metric_name(key: &str) -> String {
    let k: String = key.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect();
    format!("centurion_{k}")
}

fn prometheus_text(values: &[(String, f64)]) -> String {
    values.iter().map(|(k, v)| { let m = metric_name(k); format!("# TYPE {m} gauge\n{m} {v}\n") }).collect()
}

fn prometheus(file: Option<&str>) -> Result<(), String> {
    let text = prometheus_text(&sensor_values()?);
    let Some(file) = file else { print!("{text}"); return Ok(()) };
    let tmp = format!("{file}.tmp");
    std::fs::write(&tmp, text).and_then(|_| std::fs::rename(&tmp, file)).map_err(|e| format!("{file}: {e}"))
}

fn mqtt(host: &str, topic: &str, extra: &[String]) -> Result<(), String> {
    for (k, v) in sensor_values()? {
        let ok = Command::new("mosquitto_pub")
            .args(["-h", host, "-r", "-t", &format!("{topic}/{k}"), "-m", &v.to_string()]).args(extra)
            .status().map_err(|e| format!("mosquitto_pub: {e}"))?.success();
        if !ok { return Err(format!("mosquitto_pub failed for {topic}/{k}")); }
    }
    Ok(())
}

fn usage() -> ! {
    eprintln!("usage: centurion-export prometheus [FILE] [--interval S]\n       centurion-export mqtt HOST [--topic PREFIX] [--interval S] [-- MOSQUITTO_PUB_ARGS...]");
    std::process::exit(2);
}

fn main() {
    let all: Vec<String> = std::env::args().skip(1).collect();
    let (args, extra) = match all.iter().position(|a| a == "--") {
        Some(i) => (&all[..i], &all[i + 1..]),
        None => (&all[..], &[][..]),
    };
    let mut pos = Vec::new();
    let (mut interval, mut topic) = (0u64, "centurion".to_owned());
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--interval" => interval = it.next().and_then(|s| s.parse().ok()).filter(|&s| s > 0).unwrap_or_else(|| usage()),
            "--topic" => topic = it.next().cloned().unwrap_or_else(|| usage()),
            _ => pos.push(a.as_str()),
        }
    }
    let run = || match pos.as_slice() {
        ["prometheus"] => prometheus(None),
        ["prometheus", file] => prometheus(Some(file)),
        ["mqtt", host] => mqtt(host, &topic, extra),
        _ => usage(),
    };
    loop {
        let r = run();
        if let Err(e) = &r { eprintln!("centurion-export: {e}"); }
        if interval == 0 { std::process::exit(i32::from(r.is_err())); }
        std::thread::sleep(std::time::Duration::from_secs(interval));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_dbus_send_dict_and_formats_gauges() {
        let reply = "method return time=1 sender=:1.5 -> destination=:1.9 serial=7 reply_serial=2\n   array [\n      dict entry(\n         string \"cpu_temp_c\"\n         double 45.5\n      )\n      dict entry(\n         string \"power_BAT-0_w\"\n         double 12\n      )\n   ]\n";
        let v = parse_reply(reply);
        assert_eq!(v, vec![("cpu_temp_c".into(), 45.5), ("power_BAT-0_w".into(), 12.0)]);
        assert_eq!(prometheus_text(&v), "# TYPE centurion_cpu_temp_c gauge\ncenturion_cpu_temp_c 45.5\n# TYPE centurion_power_BAT_0_w gauge\ncenturion_power_BAT_0_w 12\n");
    }
}
