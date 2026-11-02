/* Blue VO!CE as a PipeWire virtual microphone.
 *
 * Captures the real microphone, runs it through the user's own copy of the DTS ClearVoice 2
 * DLL (loaded by our PE loader) and plays the result into an Audio/Source node that apps can
 * pick as a microphone. Both streams run on the PipeWire main loop thread, the only thread
 * that calls into the DLL.
 *
 *   glowden-voice <dll> [--target NODE] [--name NAME] [--description TEXT]
 *              [f OFF VALUE]... [b OFF VALUE]... [u OFF VALUE]...
 *
 * f/b/u set a float, byte or u32 in the parameter block (offsets in hex; the field map is in main.rs).
 * Settings may be split into sections: "@cv2" (VO!CE) and the voice effect stages "@vocal",
 * "@flanger", "@ring", "@reverb", "@delay", each followed by its own f/b/u triples. Settings
 * before any section are VO!CE's; once sections are used, a stage left out is bypassed.
 * The output is stereo (the reverb and delay stages are).
 * With --process IN.wav OUT.wav it processes a recording instead (48 kHz mono float WAV, as
 * pw-record writes it with --format f32; the result is stereo), for comparing presets on the
 * same take.
 */
#include "pe.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#define RATE 48000
#define BLOCK 256
#define FIFO (RATE / 2)

typedef struct {
    uint32_t sample_rate, block_frames, channels, sample_rate2, block_frames2, two;
    uint8_t params[1024];
} cv2_config;

typedef struct {
    float buf[FIFO];
    size_t head, len;
} fifo;

static void fifo_push(fifo *f, const float *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (f->len == FIFO) { /* overrun: drop the oldest sample */
            f->head = (f->head + 1) % FIFO;
            f->len--;
        }
        f->buf[(f->head + f->len++) % FIFO] = s[i];
    }
}

static size_t fifo_pop(fifo *f, float *s, size_t n)
{
    size_t got = n < f->len ? n : f->len;
    for (size_t i = 0; i < got; i++) s[i] = f->buf[(f->head + i) % FIFO];
    f->head = (f->head + got) % FIFO;
    f->len -= got;
    return got;
}

/* The voice effect stages exported by the same DLL, in processing order. Each takes the
 * same 6×u32 config as ClearVoice 2 followed by its parameters, and processes its channel
 * buffers in place. */
typedef struct {
    uint32_t c[6];
    uint8_t params[256];
} __attribute__((aligned(32))) stage_config;

typedef struct {
    const char *section, *api;
    uint32_t channels;
    void *inst;
    int (WINAPI *get_size)(const void *, size_t *);
    int (WINAPI *init)(void *, const void *);
    int (WINAPI *process)(void *, float **);
    int (WINAPI *update)(void *, void *);
    int (WINAPI *flush)(void *);
    uint8_t defaults[256];
    int active;
} stage;

static stage stages[] = {
    {.section = "@vocal", .api = "VocalChanger", .channels = 1},
    {.section = "@flanger", .api = "FlangerPhaser", .channels = 1},
    {.section = "@ring", .api = "RingModulator", .channels = 1},
    {.section = "@reverb", .api = "Reverb", .channels = 2},
    {.section = "@delay", .api = "Delay", .channels = 2},
};
#define STAGES (sizeof stages / sizeof *stages)

static struct {
    struct pw_main_loop *loop;
    struct pw_stream *capture, *source;
    int (WINAPI *process)(void *, float **);
    void *inst;
    float *in, *out, *left, *right;
    int cv2_active;
    fifo raw, done;
    int primed;
    unsigned long captured, processed, failed, served;
    /* Level meter: peaks over the last ~100 ms, written to stdout when it's a pipe. */
    int meter;
    float peak_in, peak_out;
    unsigned meter_blocks;
} st;

/* st.in (one mono block) → interleaved stereo. */
static int process_block(float *frame)
{
    if (st.cv2_active) {
        if (st.process(st.inst, (float *[]){st.in, st.out})) return -1;
        memcpy(st.left, st.out, BLOCK * sizeof(float));
    } else {
        memcpy(st.left, st.in, BLOCK * sizeof(float));
    }
    int split = 0;
    for (size_t i = 0; i < STAGES; i++) {
        stage *s = &stages[i];
        if (!s->active) continue;
        if (s->channels == 2 && !split) {
            memcpy(st.right, st.left, BLOCK * sizeof(float));
            split = 1;
        }
        if (s->process(s->inst, (float *[]){st.left, st.right})) return -1;
    }
    const float *r = split ? st.right : st.left;
    for (int n = 0; n < BLOCK; n++) {
        frame[2 * n] = st.left[n];
        frame[2 * n + 1] = r[n];
    }
    return 0;
}

static void on_capture(void *data)
{
    struct pw_buffer *b = pw_stream_dequeue_buffer(st.capture);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    if (d->data) {
        uint32_t n = d->chunk->size / sizeof(float);
        fifo_push(&st.raw, (float *)SPA_PTROFF(d->data, d->chunk->offset, void), n);
        st.captured += n;
    }
    pw_stream_queue_buffer(st.capture, b);
    while (st.raw.len >= BLOCK) {
        fifo_pop(&st.raw, st.in, BLOCK);
        float frame[2 * BLOCK];
        float in_peak = 0;
        for (int n = 0; n < BLOCK; n++) in_peak = fmaxf(in_peak, fabsf(st.in[n]));
        if (process_block(frame) == 0) {
            fifo_push(&st.done, frame, 2 * BLOCK);
            st.processed++;
            if (st.meter) {
                st.peak_in = fmaxf(st.peak_in, in_peak);
                for (int n = 0; n < 2 * BLOCK; n++) st.peak_out = fmaxf(st.peak_out, fabsf(frame[n]));
                if (++st.meter_blocks >= RATE / 10 / BLOCK) {
                    printf("%.1f %.1f\n", 20 * log10f(fmaxf(st.peak_in, 1e-6f)), 20 * log10f(fmaxf(st.peak_out, 1e-6f)));
                    fflush(stdout);
                    st.peak_in = st.peak_out = 0;
                    st.meter_blocks = 0;
                }
            }
        } else {
            st.failed++;
        }
    }
}

static void on_source(void *data)
{
    struct pw_buffer *b = pw_stream_dequeue_buffer(st.source);
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    if (!d->data) {
        pw_stream_queue_buffer(st.source, b);
        return;
    }
    uint32_t frames = d->maxsize / (2 * sizeof(float));
#if PW_CHECK_VERSION(0, 3, 49)
    if (b->requested && b->requested < frames) frames = b->requested;
#endif
    uint32_t n = 2 * frames;
    float *dst = d->data;
    /* Start once a little audio is buffered, so the first reads don't underrun. While no app
     * reads the virtual mic, processed audio piles up; keep only the newest so a recording
     * that starts later isn't delayed by it. */
    if (!st.primed && st.done.len >= 4 * BLOCK) st.primed = 1;
    size_t keep = n + 4 * BLOCK;
    if (st.done.len > keep) {
        size_t drop = st.done.len - keep;
        st.done.head = (st.done.head + drop) % FIFO;
        st.done.len -= drop;
    }
    size_t got = st.primed ? fifo_pop(&st.done, dst, n) : 0;
    if (got < n) memset(dst + got, 0, (n - got) * sizeof(float));
    st.served += got / 2;
    d->chunk->offset = 0;
    d->chunk->stride = 2 * sizeof(float);
    d->chunk->size = n * sizeof(float);
    pw_stream_queue_buffer(st.source, b);
}

static const struct pw_stream_events capture_events = {PW_VERSION_STREAM_EVENTS, .process = on_capture};
static const struct pw_stream_events source_events = {PW_VERSION_STREAM_EVENTS, .process = on_source};

/* "f|b|u OFF VALUE" → parameter block. Returns 0, or -1 for an offset out of range. */
static int set_field(uint8_t *params, size_t cap, char kind, const char *off_hex, const char *value)
{
    long off = strtol(off_hex, NULL, 16);
    if (off < 0 || off + 4 > (long)cap) return -1;
    if (kind == 'f') {
        float v = atof(value);
        memcpy(params + off, &v, 4);
    } else if (kind == 'u') {
        uint32_t v = strtoul(value, NULL, 10);
        memcpy(params + off, &v, 4);
    } else {
        params[off] = atoi(value);
    }
    return 0;
}

/* Settings: "f|b|u OFF VALUE" triples, optionally in sections (see the top of this file),
 * each applied on top of that block's defaults. */
static uint8_t default_params[1024];
static int (WINAPI *update_params)(void *, void *);
static char line[16384];
static size_t line_len;
static struct spa_source *input_source;

typedef struct {
    uint8_t cv2[1024];
    uint8_t fx[STAGES][256];
    int cv2_on, fx_on[STAGES];
} settings;

static int parse_settings(char **tok, int n, settings *out)
{
    memcpy(out->cv2, default_params, sizeof out->cv2);
    for (size_t i = 0; i < STAGES; i++) {
        memcpy(out->fx[i], stages[i].defaults, sizeof out->fx[i]);
        out->fx_on[i] = 0;
    }
    int sections = 0, loose = 0;
    out->cv2_on = 0;
    uint8_t *target = out->cv2;
    size_t cap = sizeof out->cv2;
    for (int i = 0; i < n; i++) {
        if (tok[i][0] == '@') {
            sections = 1;
            if (!strcmp(tok[i], "@cv2")) {
                out->cv2_on = 1;
                target = out->cv2;
                cap = sizeof out->cv2;
                continue;
            }
            size_t k = 0;
            while (k < STAGES && strcmp(tok[i], stages[k].section)) k++;
            if (k == STAGES) {
                fprintf(stderr, "blue-voice: unknown section %s\n", tok[i]);
                return -1;
            }
            out->fx_on[k] = 1;
            target = out->fx[k];
            cap = sizeof out->fx[k];
            continue;
        }
        if (i + 2 >= n || !strchr("fbu", tok[i][0]) || tok[i][1] || set_field(target, cap, tok[i][0], tok[i + 1], tok[i + 2])) {
            fprintf(stderr, "blue-voice: bad setting %s\n", tok[i]);
            return -1;
        }
        if (!sections) loose = 1;
        i += 2;
    }
    if (!sections || loose) out->cv2_on = 1;
    return 0;
}

static int stage_start(stage *s, const uint8_t *params)
{
    if (!s->inst) {
        static stage_config cfg;
        uint32_t c[6] = {RATE, BLOCK, s->channels, RATE, BLOCK, s->channels};
        memcpy(cfg.c, c, sizeof c);
        memcpy(cfg.params, params, sizeof cfg.params);
        size_t size = 0;
        int r = s->get_size(&cfg, &size);
        if (r) return r;
        s->inst = aligned_alloc(64, (size + 63) & ~(size_t)63);
        memset(s->inst, 0, size);
        r = s->init(s->inst, &cfg);
        if (r) {
            free(s->inst);
            s->inst = NULL;
        }
        return r;
    }
    if (!s->active) s->flush(s->inst);
    static uint8_t p[256] __attribute__((aligned(32)));
    memcpy(p, params, sizeof p);
    return s->update(s->inst, p);
}

/* Returns 0, or the first engine error. A stage that rejects its settings stays as it was. */
static int apply_settings(settings *set, int initial)
{
    int err = 0;
    if (set->cv2_on && !initial) {
        int r = update_params(st.inst, set->cv2);
        if (r) {
            fprintf(stderr, "blue-voice: engine rejected the new settings (%d)\n", r);
            err = r;
        }
    }
    if (!err) st.cv2_active = set->cv2_on;
    for (size_t i = 0; i < STAGES; i++) {
        stage *s = &stages[i];
        if (!set->fx_on[i]) {
            s->active = 0;
            continue;
        }
        int r = stage_start(s, set->fx[i]);
        if (r) {
            fprintf(stderr, "blue-voice: %s rejected its settings (%d)\n", s->api, r);
            err = err ? err : r;
            continue;
        }
        s->active = 1;
    }
    return err;
}

static void apply_line(char *text)
{
    static char *tok[2048];
    static settings set;
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(text, " \t", &save); t && n < 2048; t = strtok_r(NULL, " \t", &save)) tok[n++] = t;
    if (parse_settings(tok, n, &set)) return;
    if (apply_settings(&set, 0) == 0) fprintf(stderr, "blue-voice: settings updated\n");
}

static void on_stdin(void *data, int fd, uint32_t mask)
{
    ssize_t got = read(fd, line + line_len, sizeof line - 1 - line_len);
    if (got <= 0) {
        /* No (more) settings coming: stop watching; the parent check handles shutdown. */
        if (got == 0) pw_loop_destroy_source(pw_main_loop_get_loop(st.loop), input_source);
        return;
    }
    line_len += got;
    char *nl;
    while ((nl = memchr(line, '\n', line_len))) {
        *nl = 0;
        apply_line(line);
        line_len -= nl + 1 - line;
        memmove(line, nl + 1, line_len);
    }
    if (line_len == sizeof line - 1) line_len = 0;
}

static void stop(int sig)
{
    pw_main_loop_quit(st.loop);
}

/* Quit when the service that started us is gone, instead of lingering as an orphan. */
static pid_t parent;
static void check_parent(void *data, uint64_t expirations)
{
    if (getppid() != parent) pw_main_loop_quit(st.loop);
}

static const struct spa_pod *mono_format(struct spa_pod_builder *b)
{
    return spa_format_audio_raw_build(b, SPA_PARAM_EnumFormat,
                                      &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = RATE,
                                                               .channels = 1, .position = {SPA_AUDIO_CHANNEL_MONO}));
}

static const struct spa_pod *stereo_format(struct spa_pod_builder *b)
{
    return spa_format_audio_raw_build(
        b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = RATE, .channels = 2,
                                 .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}));
}

/* Offline: run a 48 kHz mono float WAV through the engine into a stereo WAV. The output is
 * shifted by VO!CE's latency so it lines up with the input. */
static int process_file(const char *in_path, const char *out_path)
{
    FILE *in = fopen(in_path, "rb");
    if (!in) {
        fprintf(stderr, "blue-voice: cannot open %s\n", in_path);
        return 1;
    }
    uint8_t head[12];
    if (fread(head, 1, 12, in) != 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) {
        fprintf(stderr, "blue-voice: %s is not a WAV file\n", in_path);
        return 1;
    }
    uint32_t len = 0, rate = 0;
    uint16_t format = 0, channels = 0;
    char id[4];
    while (fread(id, 1, 4, in) == 4 && fread(&len, 4, 1, in) == 1) {
        if (!memcmp(id, "fmt ", 4)) {
            uint8_t fmt[64] = {0};
            if (fread(fmt, 1, len < sizeof fmt ? len : sizeof fmt, in) == 0) break;
            if (len > sizeof fmt) fseek(in, len - sizeof fmt, SEEK_CUR);
            memcpy(&format, fmt, 2);
            memcpy(&channels, fmt + 2, 2);
            memcpy(&rate, fmt + 4, 4);
        } else if (!memcmp(id, "data", 4)) {
            break;
        } else {
            fseek(in, len + (len & 1), SEEK_CUR);
        }
    }
    if (memcmp(id, "data", 4) || format != 3 || channels != 1 || rate != RATE) {
        fprintf(stderr, "blue-voice: %s must be 48 kHz mono float\n", in_path);
        return 1;
    }
    size_t n = len / sizeof(float), latency = st.cv2_active ? 1280 : 0;
    float *samples = calloc(n + latency + BLOCK, sizeof(float));
    n = fread(samples, sizeof(float), n, in);
    fclose(in);
    float *result = calloc(2 * (n + latency + BLOCK), sizeof(float));
    for (size_t at = 0; at < n + latency; at += BLOCK) {
        memcpy(st.in, samples + at, BLOCK * sizeof(float));
        if (process_block(result + 2 * at)) return 1;
    }
    FILE *out = fopen(out_path, "wb");
    if (!out) return 1;
    uint32_t bytes = 2 * n * sizeof(float), riff = 36 + bytes, fmt_len = 16, byte_rate = RATE * 8;
    uint16_t fmt_tag = 3, ch = 2, align = 8, bits = 32;
    uint32_t r = RATE;
    fwrite("RIFF", 1, 4, out);
    fwrite(&riff, 4, 1, out);
    fwrite("WAVEfmt ", 1, 8, out);
    fwrite(&fmt_len, 4, 1, out);
    fwrite(&fmt_tag, 2, 1, out);
    fwrite(&ch, 2, 1, out);
    fwrite(&r, 4, 1, out);
    fwrite(&byte_rate, 4, 1, out);
    fwrite(&align, 2, 1, out);
    fwrite(&bits, 2, 1, out);
    fwrite("data", 1, 4, out);
    fwrite(&bytes, 4, 1, out);
    fwrite(result + 2 * latency, sizeof(float), 2 * n, out);
    fclose(out);
    return 0;
}

int blue_voice_host_main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: glowden-voice <dll> [--target NODE] [--name NAME] [--description TEXT] [f|b|u OFF V]...\n");
        return 2;
    }
    const char *target = NULL, *name = "glowden-voice-mic", *description = "Blue VO!CE";
    const char *process_in = NULL, *process_out = NULL;
    static cv2_config cfg = {RATE, BLOCK, 1, RATE, BLOCK, 2, {0}};

    pe_image *img = pe_load(argv[1]);
    if (!img) {
        fprintf(stderr, "blue-voice: cannot load %s\n", argv[1]);
        return 1;
    }
    int (WINAPI *get_size)(const cv2_config *, size_t *) = pe_export(img, "?dtsClearVoice2GetInstanceSize@");
    int (WINAPI *init)(void *, const cv2_config *) = pe_export(img, "?dtsClearVoice2Init@");
    int (WINAPI *defaults)(void *) = pe_export(img, "?dtsClearVoice2GetDefaultParameters@");
    st.process = pe_export(img, "?dtsClearVoice2Process@");
    if (!get_size || !init || !defaults || !st.process) {
        fprintf(stderr, "blue-voice: %s is not the ClearVoice 2 engine\n", argv[1]);
        return 1;
    }
    defaults(cfg.params);
    memcpy(default_params, cfg.params, sizeof default_params);
    update_params = pe_export(img, "?dtsClearVoice2UpdateParameters@");
    for (size_t i = 0; i < STAGES; i++) {
        stage *s = &stages[i];
        char sym[96];
#define STAGE_FN(field, fn) \
        snprintf(sym, sizeof sym, "?dts%s" fn "@", s->api); \
        *(void **)&s->field = pe_export(img, sym);
        STAGE_FN(get_size, "GetInstanceSize")
        STAGE_FN(init, "Init")
        STAGE_FN(process, "Process")
        STAGE_FN(update, "UpdateParameters")
        STAGE_FN(flush, "Flush")
        int (WINAPI *stage_defaults)(void *);
        snprintf(sym, sizeof sym, "?dts%sGetDefaultParameters@", s->api);
        *(void **)&stage_defaults = pe_export(img, sym);
        if (!s->get_size || !s->init || !s->process || !s->update || !s->flush || !stage_defaults) {
            fprintf(stderr, "blue-voice: %s is missing the %s stage\n", argv[1], s->api);
            return 1;
        }
        stage_defaults(s->defaults);
    }

    static char *tok[2048];
    int ntok = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--target") && i + 1 < argc) target = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--description") && i + 1 < argc) description = argv[++i];
        else if (!strcmp(argv[i], "--process") && i + 2 < argc) {
            process_in = argv[++i];
            process_out = argv[++i];
        } else if (ntok < 2048) {
            tok[ntok++] = argv[i];
        }
    }
    static settings initial;
    if (parse_settings(tok, ntok, &initial)) return 2;
    memcpy(cfg.params, initial.cv2, sizeof cfg.params);

    size_t size = 0;
    if (get_size(&cfg, &size)) {
        fprintf(stderr, "blue-voice: engine rejected the configuration\n");
        return 1;
    }
    st.inst = aligned_alloc(64, (size + 63) & ~(size_t)63);
    memset(st.inst, 0, size);
    int r = init(st.inst, &cfg);
    if (r) {
        fprintf(stderr, "blue-voice: engine rejected the parameters (%d)\n", r);
        return 1;
    }
    st.in = aligned_alloc(32, BLOCK * sizeof(float));
    st.out = aligned_alloc(32, BLOCK * sizeof(float));
    st.left = aligned_alloc(32, BLOCK * sizeof(float));
    st.right = aligned_alloc(32, BLOCK * sizeof(float));
    apply_settings(&initial, 1);
    if (process_in) return process_file(process_in, process_out);

    struct stat out_stat;
    st.meter = fstat(STDOUT_FILENO, &out_stat) == 0 && S_ISFIFO(out_stat.st_mode);
    signal(SIGPIPE, SIG_IGN);
    pw_init(&argc, &argv);
    st.loop = pw_main_loop_new(NULL);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    parent = getppid();
    if (update_params && !isatty(STDIN_FILENO))
        input_source = pw_loop_add_io(pw_main_loop_get_loop(st.loop), STDIN_FILENO, SPA_IO_IN, false, on_stdin, NULL);
    struct spa_source *timer = pw_loop_add_timer(pw_main_loop_get_loop(st.loop), check_parent, NULL);
    struct timespec second = {1, 0};
    pw_loop_update_timer(pw_main_loop_get_loop(st.loop), timer, &second, &second, false);

    struct pw_properties *cp = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                 PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_NODE_NAME,
                                                 "glowden-voice-mic-input", PW_KEY_NODE_DESCRIPTION,
                                                 "Blue VO!CE input", PW_KEY_NODE_LATENCY, "256/48000", NULL);
    if (target) pw_properties_set(cp, PW_KEY_TARGET_OBJECT, target);
    /* Not offered to other apps as a stream to move: it belongs to the virtual mic. */
    pw_properties_set(cp, PW_KEY_NODE_DONT_RECONNECT, "true");
    st.capture = pw_stream_new_simple(pw_main_loop_get_loop(st.loop), "glowden-voice-mic-input", cp,
                                      &capture_events, NULL);

    struct pw_properties *sp = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CLASS, "Audio/Source",
                                                 PW_KEY_NODE_NAME, name, PW_KEY_NODE_DESCRIPTION, description,
                                                 PW_KEY_NODE_LATENCY, "256/48000", NULL);
    st.source = pw_stream_new_simple(pw_main_loop_get_loop(st.loop), name, sp, &source_events, NULL);

    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *params[1] = {mono_format(&b)};
    pw_stream_connect(st.capture, PW_DIRECTION_INPUT, PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    b = (struct spa_pod_builder)SPA_POD_BUILDER_INIT(buf, sizeof buf);
    params[0] = stereo_format(&b);
    pw_stream_connect(st.source, PW_DIRECTION_OUTPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, params, 1);

    fprintf(stderr, "blue-voice: running as \"%s\"%s%s\n", description, target ? " from " : "", target ? target : "");
    pw_main_loop_run(st.loop);
    fprintf(stderr, "blue-voice: captured %lu samples, processed %lu blocks (%lu failed), served %lu samples\n",
            st.captured, st.processed, st.failed, st.served);

    pw_stream_destroy(st.capture);
    pw_stream_destroy(st.source);
    pw_main_loop_destroy(st.loop);
    pw_deinit();
    return 0;
}
