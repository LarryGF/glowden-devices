//! GlowDen plugin running Blue VO!CE (DTS ClearVoice 2) and G HUB's voice effects on Linux.
//!
//! The engine is the user's own copy of the DLL, loaded by a small PE loader (loader/) and
//! hosted on PipeWire by host.c as a virtual microphone. GlowDen talks to this program
//! with JSON lines on stdin/stdout:
//!
//!   {"id":1,"method":"describe"}         → fine-tuning controls and switches, effect stages
//!   {"id":2,"method":"summarize","params":{"presets":[{"name","settings"}]}}
//!                                         → per VO!CE preset: key numbers and a note
//!   {"id":3,"method":"summarize_effects","params":{"effects":[{"name","settings"}]}}
//!                                         → per effect preset: what it turns on
//!   {"id":4,"method":"start","params":{"engine","source","name","description","voice","effects"}}
//!   {"id":5,"method":"stop"}             → stops the virtual microphone
//!   {"id":6,"method":"apply","params":{"voice","effects"}}
//!   {"id":7,"method":"preview","params":{"engine","file"}}
//!                                         → loops a recording through the current settings
//!                                           to the speakers (the mic test); `apply` updates it
//!   {"id":8,"method":"preview_stop"}
//!   {"id":9,"method":"process","params":{"engine","in","out","voice","effects"}}
//!
//! Replies are {"id","result"} or {"id","error"}; while the microphone runs it also sends
//! {"event":"levels","input":dB,"output":dB} about ten times a second. `voice` is a G HUB
//! VO!CE preset's settings, `effects` a G HUB voice effect preset's (either may be null).
//! Everything stops when stdin closes.

use std::ffi::{CString, c_char, c_int};
use std::io::{BufRead, Write};
use std::process::{Child, Command, Stdio};

use serde_json::{Value, json};

unsafe extern "C" {
    fn blue_voice_host_main(argc: c_int, argv: *mut *mut c_char) -> c_int;
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        // The PipeWire host itself, started by `serve`.
        Some("host") => {
            let c: Vec<CString> = std::iter::once(args[0].clone())
                .chain(args[2..].iter().cloned())
                .map(|a| CString::new(a).expect("argument contains a NUL byte"))
                .collect();
            let mut argv: Vec<*mut c_char> = c.iter().map(|a| a.as_ptr() as *mut c_char).collect();
            argv.push(std::ptr::null_mut());
            std::process::exit(unsafe {
                blue_voice_host_main(c.len() as c_int, argv.as_mut_ptr())
            });
        }
        Some("serve") => serve(),
        _ => {
            eprintln!(
                "usage: {} serve   (GlowDen plugin protocol on stdin/stdout)",
                args[0]
            );
            std::process::exit(2);
        }
    }
}

fn send(v: &Value) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{v}");
    let _ = out.flush();
}

fn stop(child: &mut Option<Child>) {
    if let Some(mut c) = child.take() {
        let _ = c.kill();
        let _ = c.wait();
    }
}

fn serve() {
    let mut live: Option<Child> = None;
    let mut preview: Option<Child> = None;
    // The settings last started or applied; a preview starts with them.
    let mut current = json!({ "voice": null, "effects": null });
    for line in std::io::stdin().lock().lines().map_while(Result::ok) {
        let Ok(msg) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        let id = msg["id"].clone();
        let p = &msg["params"];
        let result = match msg["method"].as_str().unwrap_or("") {
            "describe" => Ok(describe()),
            "summarize" => Ok(Value::Object(
                p["presets"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .filter_map(|x| {
                        let name = x["name"].as_str()?;
                        let mut sum = summary(&x["settings"]);
                        sum["note"] = json!(preset_note(name));
                        Some((name.to_string(), sum))
                    })
                    .collect(),
            )),
            "summarize_effects" => Ok(Value::Object(
                p["effects"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .filter_map(|x| {
                        Some((
                            x["name"].as_str()?.to_string(),
                            json!(effect_chips(&x["settings"])),
                        ))
                    })
                    .collect(),
            )),
            "start" => {
                current = json!({ "voice": p["voice"], "effects": p["effects"] });
                stop(&mut live);
                start(p).map(|child| {
                    live = Some(child);
                    Value::Null
                })
            }
            "stop" => {
                stop(&mut live);
                Ok(Value::Null)
            }
            "apply" => {
                current = json!({ "voice": p["voice"], "effects": p["effects"] });
                let mut result = Ok(Value::Null);
                for child in [live.as_mut(), preview.as_mut()].into_iter().flatten() {
                    if let Err(e) = apply(child, p) {
                        result = Err(e);
                    }
                }
                result
            }
            "preview" => {
                stop(&mut preview);
                let mut params = current.clone();
                params["engine"] = p["engine"].clone();
                params["file"] = p["file"].clone();
                start_preview(&params).map(|child| {
                    preview = Some(child);
                    Value::Null
                })
            }
            "preview_stop" => {
                stop(&mut preview);
                Ok(Value::Null)
            }
            "process" => process(p),
            m => Err(format!("unknown method {m}")),
        };
        send(&match result {
            Ok(r) => json!({ "id": id, "result": r }),
            Err(e) => json!({ "id": id, "error": e }),
        });
    }
    stop(&mut live);
    stop(&mut preview);
}

fn settings_args(p: &Value) -> Vec<String> {
    let voice = Some(&p["voice"]).filter(|v| !v.is_null());
    let effects = Some(&p["effects"]).filter(|v| !v.is_null());
    host_args(voice, effects)
}

fn start(p: &Value) -> Result<Child, String> {
    let s = |k: &str| p[k].as_str().ok_or_else(|| format!("start needs {k}"));
    let mut child = Command::new(std::env::current_exe().map_err(|e| e.to_string())?)
        .arg("host")
        .arg(s("engine")?)
        .args([
            "--target",
            s("source")?,
            "--name",
            s("name")?,
            "--description",
            s("description")?,
        ])
        .args(settings_args(p))
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .map_err(|e| format!("cannot start the host: {e}"))?;
    // The host prints "input output" peak levels; pass them on as events.
    if let Some(out) = child.stdout.take() {
        std::thread::spawn(move || {
            for line in std::io::BufReader::new(out).lines().map_while(Result::ok) {
                let mut v = line.split(' ').filter_map(|x| x.parse::<f32>().ok());
                if let (Some(i), Some(o)) = (v.next(), v.next()) {
                    send(&json!({ "event": "levels", "input": i, "output": o }));
                }
            }
        });
    }
    Ok(child)
}

fn apply(host: &mut Child, p: &Value) -> Result<Value, String> {
    let stdin = host.stdin.as_mut().ok_or("host has no input")?;
    writeln!(stdin, "{}", settings_args(p).join(" "))
        .and_then(|_| stdin.flush())
        .map_err(|e| format!("host stopped: {e}"))?;
    Ok(Value::Null)
}

fn start_preview(p: &Value) -> Result<Child, String> {
    let s = |k: &str| p[k].as_str().ok_or_else(|| format!("preview needs {k}"));
    Command::new(std::env::current_exe().map_err(|e| e.to_string())?)
        .arg("host")
        .arg(s("engine")?)
        .args([
            "--name",
            "glowden-voice-preview",
            "--description",
            "GlowDen mic test",
            "--play",
            s("file")?,
        ])
        .args(settings_args(p))
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .spawn()
        .map_err(|e| format!("cannot start the preview: {e}"))
}

fn process(p: &Value) -> Result<Value, String> {
    let s = |k: &str| p[k].as_str().ok_or_else(|| format!("process needs {k}"));
    let status = Command::new(std::env::current_exe().map_err(|e| e.to_string())?)
        .arg("host")
        .arg(s("engine")?)
        .args(settings_args(p))
        .args(["--process", s("in")?, s("out")?])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .status()
        .map_err(|e| e.to_string())?;
    status
        .success()
        .then_some(Value::Null)
        .ok_or_else(|| "processing failed".into())
}

fn describe() -> Value {
    let modes = |list: &[&str]| -> Value {
        json!(
            list.iter()
                .map(|m| json!({ "value": m, "label": mode_label(m) }))
                .collect::<Vec<_>>()
        )
    };
    let slider = |field: &str, label: &str, min: f64, max: f64, step: f64, unit: &str| json!({ "field": field, "label": label, "min": min, "max": max, "step": step, "unit": unit });
    let pick = |field: &str, label: &str, list: &[&str]| json!({ "field": field, "label": label, "modes": modes(list) });
    json!({
        "controls": CONTROLS.iter().map(|(path, group, label, min, max, step, unit)| {
            json!({ "path": path, "group": group, "label": label, "min": min, "max": max, "step": step, "unit": unit })
        }).collect::<Vec<_>>(),
        "switches": SWITCHES.iter().map(|(group, path)| json!({ "group": group, "path": path })).collect::<Vec<_>>(),
        // The voice effect controls, with G HUB's ranges; "enable" is a stage's on/off field.
        "effect_stages": [
            { "stage": "vocalChanger", "title": "Voice", "controls": [
                pick("vocalMode1", "Primary voice", VOCAL_MODES),
                slider("voiceModFactor1", "Primary amount", -2400.0, 2400.0, 50.0, "cents"),
                slider("voiceGain1", "Primary level", -60.0, 0.0, 1.0, "dB"),
                pick("vocalMode2", "Secondary voice", VOCAL_MODES),
                slider("voiceModFactor2", "Secondary amount", -2400.0, 2400.0, 50.0, "cents"),
                slider("voiceGain2", "Secondary level", -60.0, 0.0, 1.0, "dB"),
            ]},
            { "stage": "vocalChanger", "title": "Chorus", "enable": "chorusEnabled", "controls": [
                slider("chorusDelay", "Delay", 0.0, 100.0, 1.0, "ms"),
                slider("chorusDelayMod", "Delay swing", 0.0, 100.0, 1.0, "ms"),
                slider("chorusDelayModRate", "Delay swing rate", 0.0, 2.0, 0.05, "Hz"),
                slider("chorusPitchMod", "Pitch swing", -600.0, 600.0, 10.0, "cents"),
                slider("chorusPitchModRate", "Pitch swing rate", 0.0, 2.0, 0.05, "Hz"),
            ]},
            { "stage": "flangerPhaser", "title": "Flanger / phaser", "enable": "enabled", "controls": [
                pick("phaserMode", "Mode", PHASER_MODES),
                slider("depth", "Depth", 0.0, 1.0, 0.05, ""),
                slider("rate", "Rate", 0.0, 5.0, 0.1, "Hz"),
            ]},
            { "stage": "ringModulation", "title": "Ring modulator", "enable": "enabled", "controls": [
                pick("shapeMode", "Shape", SHAPE_MODES),
                slider("carrierFrequency", "Frequency", 1.0, 2000.0, 10.0, "Hz"),
                slider("mix", "Mix", 0.0, 1.0, 0.05, ""),
            ]},
            { "stage": "reverb", "title": "Reverb", "enable": "enabled", "controls": [
                pick("reflectionMode", "Reflections", REFLECTION_MODES),
                slider("mix", "Mix", 0.0, 1.0, 0.05, ""),
                slider("reflectionLowPassFrequency", "Reflection low-pass", 10.0, 10000.0, 20.0, "Hz"),
                slider("earlyLateMix", "Early / late", 0.0, 1.0, 0.05, ""),
                slider("roomSize", "Room size", 0.01, 1.0, 0.05, ""),
            ]},
            { "stage": "delay", "title": "Echo", "enable": "enabled", "controls": [
                pick("inputMode", "Input", INPUT_MODES),
                pick("feedbackMode", "Feedback routing", FEEDBACK_MODES),
                pick("feedbackTimbre", "Feedback tone", FEEDBACK_TIMBRES),
                slider("mix", "Mix", 0.0, 1.0, 0.05, ""),
                slider("feedback", "Feedback", 0.0, 0.6, 0.05, ""),
                slider("leftDelay", "Left delay", 10.0, 2000.0, 10.0, "ms"),
                slider("rightDelay", "Right delay", 10.0, 2000.0, 10.0, "ms"),
            ]},
        ],
    })
}

/// "VOCAL_MODE_FORMANT_WARP" → "Formant warp".
fn mode_label(mode: &str) -> String {
    let mut w = mode.to_string();
    for prefix in [
        "VOCAL_MODE_",
        "PHASER_MODE_",
        "REFLECTION_MODE_",
        "SHAPE_MODE_",
        "INPUT_MODE_",
        "FEEDBACK_MODE_",
        "FEEDBACK_TIMBRE_",
    ] {
        if let Some(rest) = w.strip_prefix(prefix) {
            w = rest.to_string();
            break;
        }
    }
    let w = w.replace('_', " ").to_lowercase();
    let w = match w.as_str() {
        "st flutter" => "stereo flutter".to_string(),
        "noice" => "noise".to_string(),
        "whispez" => "whisper".to_string(),
        "halfwaverectifier" => "half-wave".to_string(),
        "saw tooth" => "sawtooth".to_string(),
        "largediff" => "large diffuse".to_string(),
        "midmix" => "mid mix".to_string(),
        _ => w,
    };
    let mut c = w.chars();
    c.next()
        .map(|f| f.to_uppercase().collect::<String>() + c.as_str())
        .unwrap_or_default()
}

/// Our own wording for G HUB's VO!CE preset names, "<placement> - <character>".
fn preset_note(name: &str) -> String {
    let (place, character) = name.split_once(" - ").unwrap_or((name, ""));
    let how = match character {
        "Natural" => "Lightest processing, closest to the mic's own sound.",
        "Classic" => "All-round cleanup with a balanced tone.",
        "More Bass" | "Bass Boost" => "Warmer and fuller: bass up, muddy mids down.",
        "Broadcaster" => "Radio voice: strong compression for an even, up-front level.",
        "Increase Clarity" => "Brighter and crisper, for speech that cuts through.",
        _ => "",
    };
    let r#where = match place {
        "Desk" => "Mic on the desk, further from your mouth: more gain.",
        "Boom Arm" => "Mic on a boom arm, close to your mouth.",
        _ => "",
    };
    [how, r#where]
        .iter()
        .filter(|s| !s.is_empty())
        .cloned()
        .collect::<Vec<_>>()
        .join(" ")
}

/// What an effect preset turns on, as short labels.
fn effect_chips(s: &Value) -> Vec<String> {
    let on = |stage: &str, key: &str| s[stage][key] == true;
    let mut out = Vec::new();
    let v = &s["vocalChanger"];
    for (mode, factor) in [
        ("vocalMode1", "voiceModFactor1"),
        ("vocalMode2", "voiceModFactor2"),
    ] {
        let Some(m) = v[mode].as_str().filter(|m| *m != "VOCAL_MODE_OFF") else {
            continue;
        };
        let semis = (v[factor].as_f64().unwrap_or(0.0) / 10.0).round() / 10.0;
        out.push(if m.contains("PITCH") || m.contains("FORMANT") {
            format!(
                "{} {}{} st",
                mode_label(m),
                if semis > 0.0 { "+" } else { "" },
                semis
            )
        } else {
            mode_label(m)
        });
    }
    if on("vocalChanger", "chorusEnabled") {
        out.push("Chorus".into());
    }
    if on("flangerPhaser", "enabled") {
        out.push(mode_label(
            s["flangerPhaser"]["phaserMode"]
                .as_str()
                .unwrap_or("PHASER_MODE_FLANGER_UP"),
        ));
    }
    if on("ringModulation", "enabled") {
        out.push(format!(
            "Ring mod {} Hz",
            s["ringModulation"]["carrierFrequency"]
                .as_f64()
                .unwrap_or(0.0)
        ));
    }
    if on("reverb", "enabled") {
        out.push("Reverb".into());
    }
    if on("delay", "enabled") {
        out.push("Echo".into());
    }
    out
}

/// Preset field → (offset in the engine's parameter block, 'f' float or 'b' byte).
/// Measured one field at a time by signal probing.
const FIELDS: &[(&str, u16, char)] = &[
    ("highPassFilter.enabled", 0x014, 'b'),
    ("highPassFilter.frequency", 0x010, 'f'),
    ("noiseReduction.release", 0x01c, 'f'),
    ("noiseReduction.sensitivity", 0x020, 'f'),
    ("noiseReduction.bias", 0x024, 'f'),
    ("noiseReduction.maxAttenuation", 0x028, 'f'),
    ("noiseReduction.enabled", 0x02c, 'b'),
    ("gatingCompressor.noiseGateThreshold", 0x034, 'f'),
    ("gatingCompressor.noiseGateMaxAttenuation", 0x038, 'f'),
    ("gatingCompressor.noiseGateAttack", 0x03c, 'f'),
    ("gatingCompressor.noiseGateRelease", 0x040, 'f'),
    ("gatingCompressor.noiseGateHoldTime", 0x044, 'f'),
    ("gatingCompressor.compressorThreshold", 0x048, 'f'),
    ("gatingCompressor.compressorRatio", 0x04c, 'f'),
    ("gatingCompressor.compressorPreGain", 0x050, 'f'),
    ("gatingCompressor.compressorPostGain", 0x054, 'f'),
    ("gatingCompressor.compressorAttack", 0x058, 'f'),
    ("gatingCompressor.compressorRelease", 0x05c, 'f'),
    ("gatingCompressor.compressorEnabled", 0x061, 'b'),
    ("multiBandEq.lowBandFilter.frequency", 0x070, 'f'),
    ("multiBandEq.lowBandFilter.gain", 0x074, 'f'),
    ("multiBandEq.lowBandFilter.q", 0x078, 'f'),
    ("multiBandEq.midBandFilter.frequency", 0x07c, 'f'),
    ("multiBandEq.midBandFilter.gain", 0x080, 'f'),
    ("multiBandEq.midBandFilter.q", 0x084, 'f'),
    ("multiBandEq.highBandFilter.frequency", 0x088, 'f'),
    ("multiBandEq.highBandFilter.gain", 0x08c, 'f'),
    ("multiBandEq.highBandFilter.q", 0x090, 'f'),
    ("multiBandEq.enabled", 0x09c, 'b'),
    ("dePopper.threshold", 0x0a4, 'f'),
    ("deEsser.threshold", 0x0a8, 'f'),
    ("dePopper.frequency", 0x0ac, 'f'),
    ("dePopper.width", 0x0b0, 'f'),
    ("dePopper.maxAttenuation", 0x0b4, 'f'),
    ("deEsser.frequency", 0x0b8, 'f'),
    ("deEsser.width", 0x0bc, 'f'),
    ("deEsser.maxAttenuation", 0x0c0, 'f'),
    ("dePopper.attack", 0x0c4, 'f'),
    ("dePopper.release", 0x0cc, 'f'),
    ("deEsser.attack", 0x0d0, 'f'),
    ("deEsser.release", 0x0d4, 'f'),
    ("dePopper.enabled", 0x0dd, 'b'),
    ("deEsser.enabled", 0x0de, 'b'),
    ("hardLimiter.attack", 0x140, 'f'),
    ("hardLimiter.release", 0x144, 'f'),
    ("hardLimiter.boost", 0x148, 'f'),
    ("hardLimiter.enabled", 0x150, 'b'),
];

/// `glowden-voice` arguments setting each preset field present in `settings`.
pub fn engine_args(settings: &Value) -> Vec<String> {
    let mut args = Vec::new();
    for &(path, offset, kind) in FIELDS {
        let v = path.split('.').fold(settings, |v, k| &v[k]);
        let value = match (kind, v) {
            ('b', Value::Bool(b)) => (*b as u8).to_string(),
            ('f', Value::Number(n)) => n.to_string(),
            _ => continue,
        };
        args.extend([kind.to_string(), format!("{offset:03x}"), value]);
    }
    args
}

/// Voice effect stage → host section, the field enabling it, and its fields: (preset field,
/// offset, kind, enum names in value order). The layouts follow G HUB's field order, which
/// matches each stage's default parameters; offsets and the enable bytes were checked on a
/// test tone. Fields G HUB leaves out are 0/false/the first
/// enum value, as in its settings format.
type EffectField = (&'static str, u16, char, &'static [&'static str]);
type EffectStage = (
    &'static str,
    &'static str,
    Option<&'static str>,
    u16,
    &'static [EffectField],
);
const EFFECTS: &[EffectStage] = &[
    (
        "vocalChanger",
        "@vocal",
        None,
        0x30,
        &[
            ("vocalMode1", 0x00, 'u', VOCAL_MODES),
            ("voiceModFactor1", 0x04, 'f', &[]),
            ("vocalMode2", 0x08, 'u', VOCAL_MODES),
            ("voiceModFactor2", 0x0c, 'f', &[]),
            ("voiceGain1", 0x10, 'f', &[]),
            ("voiceGain2", 0x14, 'f', &[]),
            ("chorusEnabled", 0x18, 'b', &[]),
            ("chorusDelay", 0x1c, 'f', &[]),
            ("chorusDelayMod", 0x20, 'f', &[]),
            ("chorusDelayModRate", 0x24, 'f', &[]),
            ("chorusPitchMod", 0x28, 'f', &[]),
            ("chorusPitchModRate", 0x2c, 'f', &[]),
        ],
    ),
    (
        "flangerPhaser",
        "@flanger",
        Some("enabled"),
        0x10,
        &[
            ("phaserMode", 0x00, 'u', PHASER_MODES),
            ("depth", 0x04, 'f', &[]),
            ("rate", 0x08, 'f', &[]),
        ],
    ),
    (
        "ringModulation",
        "@ring",
        Some("enabled"),
        0x10,
        &[
            ("mix", 0x00, 'f', &[]),
            ("shapeMode", 0x04, 'u', SHAPE_MODES),
            ("carrierFrequency", 0x08, 'f', &[]),
        ],
    ),
    (
        "reverb",
        "@reverb",
        Some("enabled"),
        0x24,
        &[
            ("earlyLateMix", 0x00, 'f', &[]),
            ("reflectionMode", 0x04, 'u', REFLECTION_MODES),
            ("reflectionLowPassFrequency", 0x08, 'f', &[]),
            ("roomSize", 0x0c, 'f', &[]),
            ("mix", 0x1c, 'f', &[]),
        ],
    ),
    (
        "delay",
        "@delay",
        Some("enabled"),
        0x20,
        &[
            ("inputMode", 0x00, 'u', INPUT_MODES),
            ("feedbackMode", 0x04, 'u', FEEDBACK_MODES),
            ("feedbackTimbre", 0x08, 'u', FEEDBACK_TIMBRES),
            ("leftDelay", 0x0c, 'f', &[]),
            ("rightDelay", 0x10, 'f', &[]),
            ("feedback", 0x14, 'f', &[]),
            ("mix", 0x18, 'f', &[]),
        ],
    ),
];

pub const VOCAL_MODES: &[&str] = &[
    "VOCAL_MODE_OFF",
    "VOCAL_MODE_FORMANT_WARP",
    "VOCAL_MODE_PITCH_SHIFT",
    "VOCAL_MODE_FORMANT_SHIFT",
    "VOCAL_MODE_ROBOT",
    "VOCAL_MODE_WHISPEZ",
    "VOCAL_MODE_NOICE",
    "VOCAL_MODE_GROWL",
    "VOCAL_MODE_AIR",
    "VOCAL_MODE_ETHER",
    "VOCAL_MODE_HOUSEOTRON",
    "VOCAL_MODE_FUNKBOT",
    "VOCAL_MODE_TALKBOX",
    "VOCAL_MODE_SUBBY",
    "VOCAL_MODE_NO_TREBLE",
];
pub const PHASER_MODES: &[&str] = &[
    "PHASER_MODE_FLANGER_UP",
    "PHASER_MODE_FLANGER_DOWN",
    "PHASER_MODE_FLANGER_OSCILLATE",
    "PHASER_MODE_PHASER_STUN",
    "PHASER_MODE_PHASER_HEAVY",
    "PHASER_MODE_PHASER_KILL",
];
pub const SHAPE_MODES: &[&str] = &[
    "SHAPE_MODE_SINE",
    "SHAPE_MODE_TRIANGLE",
    "SHAPE_MODE_SAW_TOOTH",
    "SHAPE_MODE_SQUARE",
    "SHAPE_MODE_HALFWAVERECTIFIER",
];
pub const REFLECTION_MODES: &[&str] = &[
    "REFLECTION_MODE_NONE",
    "REFLECTION_MODE_TIGHT",
    "REFLECTION_MODE_MIDMIX",
    "REFLECTION_MODE_LARGEDIFF",
    "REFLECTION_MODE_ECHO",
    "REFLECTION_MODE_ECHO2",
    "REFLECTION_MODE_ECHO3",
    "REFLECTION_MODE_FLUTTER",
    "REFLECTION_MODE_ST_FLUTTER",
    "REFLECTION_MODE_ECHO_FLUTTER",
];
pub const INPUT_MODES: &[&str] = &[
    "INPUT_MODE_NORMAL",
    "INPUT_MODE_CROSSED",
    "INPUT_MODE_LEFT",
    "INPUT_MODE_RIGHT",
];
pub const FEEDBACK_MODES: &[&str] = &["FEEDBACK_MODE_NORMAL", "FEEDBACK_MODE_CROSSED"];
pub const FEEDBACK_TIMBRES: &[&str] = &[
    "FEEDBACK_TIMBRE_FULL",
    "FEEDBACK_TIMBRE_DARK",
    "FEEDBACK_TIMBRE_BRIGHT",
];

/// Host sections for the voice effects in `effects` (G HUB's effect settings format); a
/// stage that is off is left out, which bypasses it.
pub fn effect_args(effects: &Value) -> Vec<String> {
    let mut args = Vec::new();
    for &(stage, section, enabled, enable_at, fields) in EFFECTS {
        let o = &effects[stage];
        let on = match enabled {
            Some(key) => o[key].as_bool().unwrap_or(false),
            // The voice changer has no switch: it is on when a voice or the chorus is.
            None => {
                o["chorusEnabled"].as_bool().unwrap_or(false)
                    || ["vocalMode1", "vocalMode2"]
                        .iter()
                        .any(|k| o[k].as_str().is_some_and(|m| m != "VOCAL_MODE_OFF"))
            }
        };
        if !on {
            continue;
        }
        args.push(section.to_string());
        for &(name, offset, kind, names) in fields {
            let v = &o[name];
            let value = match kind {
                'u' => names
                    .iter()
                    .position(|n| v.as_str() == Some(n))
                    .unwrap_or(0)
                    .to_string(),
                'b' => (v.as_bool().unwrap_or(false) as u8).to_string(),
                _ => v.as_f64().unwrap_or(0.0).to_string(),
            };
            args.extend([kind.to_string(), format!("{offset:02x}"), value]);
        }
        args.extend(["b".into(), format!("{enable_at:02x}"), "1".into()]);
    }
    args
}

/// Every host argument for a device: VO!CE (when on) and the voice effects.
pub fn host_args(voice: Option<&Value>, effects: Option<&Value>) -> Vec<String> {
    let mut args = Vec::new();
    if let Some(v) = voice {
        args.push("@cv2".to_string());
        args.extend(engine_args(v));
    }
    if let Some(e) = effects {
        args.extend(effect_args(e));
    }
    args
}

/// Fine-tuning controls offered on top of a preset (G HUB's advanced VO!CE controls): preset
/// field, group, label, min, max, step, unit. Ranges stay inside what the engine was seen to
/// accept.
pub const CONTROLS: &[(&str, &str, &str, f64, f64, f64, &str)] = &[
    (
        "highPassFilter.frequency",
        "High-pass filter",
        "Cutoff",
        20.0,
        400.0,
        1.0,
        "Hz",
    ),
    (
        "noiseReduction.maxAttenuation",
        "Noise reduction",
        "Amount",
        -30.0,
        0.0,
        1.0,
        "dB",
    ),
    (
        "noiseReduction.sensitivity",
        "Noise reduction",
        "Sensitivity",
        0.0,
        40.0,
        1.0,
        "",
    ),
    (
        "noiseReduction.release",
        "Noise reduction",
        "Release",
        10.0,
        1000.0,
        5.0,
        "ms",
    ),
    (
        "gatingCompressor.noiseGateThreshold",
        "Noise gate",
        "Threshold",
        -80.0,
        -20.0,
        1.0,
        "dB",
    ),
    (
        "gatingCompressor.noiseGateMaxAttenuation",
        "Noise gate",
        "Depth",
        -60.0,
        0.0,
        1.0,
        "dB",
    ),
    (
        "gatingCompressor.compressorThreshold",
        "Compressor",
        "Threshold",
        -40.0,
        0.0,
        1.0,
        "dB",
    ),
    (
        "gatingCompressor.compressorRatio",
        "Compressor",
        "Ratio",
        2.0,
        10.0,
        0.5,
        ":1",
    ),
    (
        "gatingCompressor.compressorPostGain",
        "Compressor",
        "Make-up gain",
        0.0,
        10.0,
        0.5,
        "dB",
    ),
    (
        "multiBandEq.lowBandFilter.gain",
        "Voice EQ",
        "Bass",
        -12.0,
        12.0,
        0.5,
        "dB",
    ),
    (
        "multiBandEq.midBandFilter.gain",
        "Voice EQ",
        "Mids",
        -12.0,
        12.0,
        0.5,
        "dB",
    ),
    (
        "multiBandEq.highBandFilter.gain",
        "Voice EQ",
        "Treble",
        -12.0,
        12.0,
        0.5,
        "dB",
    ),
    (
        "deEsser.threshold",
        "De-esser",
        "Threshold",
        -60.0,
        0.0,
        1.0,
        "dB",
    ),
    (
        "deEsser.maxAttenuation",
        "De-esser",
        "Amount",
        -40.0,
        0.0,
        1.0,
        "dB",
    ),
    (
        "dePopper.threshold",
        "De-popper",
        "Threshold",
        -60.0,
        0.0,
        1.0,
        "dB",
    ),
    (
        "dePopper.maxAttenuation",
        "De-popper",
        "Amount",
        -40.0,
        0.0,
        1.0,
        "dB",
    ),
    (
        "hardLimiter.boost",
        "Limiter",
        "Boost",
        0.0,
        10.0,
        0.5,
        "dB",
    ),
];

/// Each stage's on/off switch, per group above.
pub const SWITCHES: &[(&str, &str)] = &[
    ("High-pass filter", "highPassFilter.enabled"),
    ("Noise reduction", "noiseReduction.enabled"),
    ("Compressor", "gatingCompressor.compressorEnabled"),
    ("Voice EQ", "multiBandEq.enabled"),
    ("De-esser", "deEsser.enabled"),
    ("De-popper", "dePopper.enabled"),
    ("Limiter", "hardLimiter.enabled"),
];

/// What a preset does, for the app: the numbers that differ between presets.
pub fn summary(settings: &Value) -> Value {
    let n = |path: &str| path.split('.').fold(settings, |v, k| &v[k]).as_f64();
    serde_json::json!({
        "noise_reduction": n("noiseReduction.maxAttenuation"),
        "bass": n("multiBandEq.lowBandFilter.gain").unwrap_or(0.0),
        "mids": n("multiBandEq.midBandFilter.gain").unwrap_or(0.0),
        "treble": n("multiBandEq.highBandFilter.gain").unwrap_or(0.0),
        "compression": n("gatingCompressor.compressorRatio"),
        "de_esser": n("deEsser.maxAttenuation"),
        "boost": n("hardLimiter.boost"),
        "input_gain": n("inputGain"),
    })
}
