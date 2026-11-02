//! GlowDen plugin running Blue VO!CE (DTS ClearVoice 2) and G HUB's voice effects on Linux.
//!
//! The engine is the user's own copy of the DLL, loaded by a small PE loader (loader/) and
//! hosted on PipeWire by host.c as a virtual microphone. GlowDen talks to this program
//! with JSON lines on stdin/stdout:
//!
//!   {"id":1,"method":"describe"}                 → controls, switches, effect modes
//!   {"id":2,"method":"summarize","params":{"presets":[{"name","settings"}]}}
//!   {"id":3,"method":"start","params":{"engine","source","name","description","voice","effects"}}
//!   {"id":4,"method":"apply","params":{"voice","effects"}}
//!   {"id":5,"method":"process","params":{"engine","in","out","voice","effects"}}
//!
//! Replies are {"id","result"} or {"id","error"}; while running it also sends
//! {"event":"levels","input":dB,"output":dB} about ten times a second. `voice` is a G HUB
//! VO!CE preset's settings, `effects` a G HUB voice effect preset's (either may be null).
//! The host stops when stdin closes.

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
            std::process::exit(unsafe { blue_voice_host_main(c.len() as c_int, argv.as_mut_ptr()) });
        }
        Some("serve") => serve(),
        _ => {
            eprintln!("usage: {} serve   (GlowDen plugin protocol on stdin/stdout)", args[0]);
            std::process::exit(2);
        }
    }
}

fn send(v: &Value) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{v}");
    let _ = out.flush();
}

fn serve() {
    let mut host: Option<Child> = None;
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
                    .filter_map(|x| Some((x["name"].as_str()?.to_string(), summary(&x["settings"]))))
                    .collect(),
            )),
            "start" => start(p).map(|child| {
                if let Some(mut old) = host.replace(child) {
                    let _ = old.kill();
                    let _ = old.wait();
                }
                Value::Null
            }),
            "apply" => apply(host.as_mut(), p),
            "process" => process(p),
            m => Err(format!("unknown method {m}")),
        };
        send(&match result {
            Ok(r) => json!({ "id": id, "result": r }),
            Err(e) => json!({ "id": id, "error": e }),
        });
    }
    if let Some(mut h) = host {
        let _ = h.kill();
        let _ = h.wait();
    }
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
        .args(["--target", s("source")?, "--name", s("name")?, "--description", s("description")?])
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

fn apply(host: Option<&mut Child>, p: &Value) -> Result<Value, String> {
    let host = host.ok_or("not started")?;
    let stdin = host.stdin.as_mut().ok_or("host has no input")?;
    writeln!(stdin, "{}", settings_args(p).join(" "))
        .and_then(|_| stdin.flush())
        .map_err(|e| format!("host stopped: {e}"))?;
    Ok(Value::Null)
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
    status.success().then_some(Value::Null).ok_or_else(|| "processing failed".into())
}

fn describe() -> Value {
    json!({
        "controls": CONTROLS.iter().map(|(path, group, label, min, max, step, unit)| {
            json!({ "path": path, "group": group, "label": label, "min": min, "max": max, "step": step, "unit": unit })
        }).collect::<Vec<_>>(),
        "switches": SWITCHES.iter().map(|(group, path)| json!({ "group": group, "path": path })).collect::<Vec<_>>(),
        "effect_modes": {
            "vocalMode": VOCAL_MODES, "phaserMode": PHASER_MODES, "shapeMode": SHAPE_MODES,
            "reflectionMode": REFLECTION_MODES, "inputMode": INPUT_MODES,
            "feedbackMode": FEEDBACK_MODES, "feedbackTimbre": FEEDBACK_TIMBRES,
        },
    })
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
const EFFECTS: &[(&str, &str, Option<&str>, u16, &[EffectField])] = &[
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
