/* Mini host CLAP: carga el .clap, instancia el plugin, procesa audio y
 * comprueba passthrough + estado. Uso: mini_host <ruta al .clap> [gui] */
#include <clap/clap.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define CLAP_TEST_WINDOW_API CLAP_WINDOW_API_WIN32
#else
#include <dlfcn.h>
#include <unistd.h>
#define CLAP_TEST_WINDOW_API CLAP_WINDOW_API_X11
#endif

static const void* host_get_extension(const struct clap_host* h, const char* id) {
    (void)h; (void)id;
    return NULL;
}
static void host_request_restart(const struct clap_host* h) { (void)h; }
static void host_request_process(const struct clap_host* h) { (void)h; }
static void host_request_callback(const struct clap_host* h) { (void)h; }

static uint32_t ev_size(const struct clap_input_events* l) {
    (void)l;
    return 0;
}
static const clap_event_header_t* ev_get(const struct clap_input_events* l,
                                         uint32_t i) {
    (void)l; (void)i;
    return NULL;
}
static bool ev_push(const struct clap_output_events* l,
                    const clap_event_header_t* e) {
    (void)l; (void)e;
    return true;
}

/* stream de estado en memoria */
typedef struct {
    uint8_t buf[1 << 20];
    uint64_t len, pos;
} MemStream;

static int64_t mem_write(const struct clap_ostream* s, const void* d,
                         uint64_t n) {
    MemStream* m = (MemStream*)s->ctx;
    if (m->len + n > sizeof(m->buf)) n = sizeof(m->buf) - m->len;
    memcpy(m->buf + m->len, d, n);
    m->len += n;
    return (int64_t)n;
}
static int64_t mem_read(const struct clap_istream* s, void* d, uint64_t n) {
    MemStream* m = (MemStream*)s->ctx;
    uint64_t left = m->len - m->pos;
    if (n > left) n = left;
    memcpy(d, m->buf + m->pos, n);
    m->pos += n;
    return (int64_t)n;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "uso: %s <plugin.clap> [gui]\n", argv[0]);
        return 2;
    }
#ifdef _WIN32
    HMODULE dl = LoadLibraryA(argv[1]);
    if (!dl) {
        fprintf(stderr, "LoadLibrary fallo (error %lu)\n", GetLastError());
        return 1;
    }
    const clap_plugin_entry_t* entry =
        (const clap_plugin_entry_t*)GetProcAddress(dl, "clap_entry");
#else
    void* dl = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    const clap_plugin_entry_t* entry =
        (const clap_plugin_entry_t*)dlsym(dl, "clap_entry");
#endif
    if (!entry) {
        fprintf(stderr, "sin clap_entry\n");
        return 1;
    }
    if (!entry->init(argv[1])) {
        fprintf(stderr, "entry->init fallo\n");
        return 1;
    }
    const clap_plugin_factory_t* factory =
        (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory || factory->get_plugin_count(factory) < 1) {
        fprintf(stderr, "factory vacia\n");
        return 1;
    }
    const clap_plugin_descriptor_t* desc =
        factory->get_plugin_descriptor(factory, 0);
    printf("plugin: %s (%s) v%s\n", desc->name, desc->id, desc->version);

    clap_host_t host = {
        .clap_version = CLAP_VERSION_INIT,
        .host_data = NULL,
        .name = "mini_host",
        .vendor = "test",
        .url = "",
        .version = "1.0",
        .get_extension = host_get_extension,
        .request_restart = host_request_restart,
        .request_process = host_request_process,
        .request_callback = host_request_callback,
    };

    const clap_plugin_t* plug = factory->create_plugin(factory, &host, desc->id);
    if (!plug || !plug->init(plug)) {
        fprintf(stderr, "no se pudo crear/init\n");
        return 1;
    }
    if (!plug->activate(plug, 48000.0, 32, 512)) {
        fprintf(stderr, "activate fallo\n");
        return 1;
    }
    if (!plug->start_processing(plug)) {
        fprintf(stderr, "start_processing fallo\n");
        return 1;
    }

    /* procesa 8 bloques de una senal de prueba y verifica el passthrough */
    enum { N = 256 };
    float inL[N], inR[N], outL[N], outR[N];
    float* inChans[2] = {inL, inR};
    float* outChans[2] = {outL, outR};
    clap_audio_buffer_t inBuf = {.data32 = inChans, .channel_count = 2};
    clap_audio_buffer_t outBuf = {.data32 = outChans, .channel_count = 2};
    clap_input_events_t inEv = {.ctx = NULL, .size = ev_size, .get = ev_get};
    clap_output_events_t outEv = {.ctx = NULL, .try_push = ev_push};
    clap_process_t proc = {
        .steady_time = 0,
        .frames_count = N,
        .transport = NULL,
        .audio_inputs = &inBuf,
        .audio_outputs = &outBuf,
        .audio_inputs_count = 1,
        .audio_outputs_count = 1,
        .in_events = &inEv,
        .out_events = &outEv,
    };

    double maxErr = 0;
    for (int b = 0; b < 8; ++b) {
        for (int i = 0; i < N; ++i) {
            inL[i] = (float)sin(2 * M_PI * 440.0 * (b * N + i) / 48000.0);
            inR[i] = inL[i] * 0.5f;
            outL[i] = outR[i] = 0.f;
        }
        clap_process_status st = plug->process(plug, &proc);
        if (st == CLAP_PROCESS_ERROR) {
            fprintf(stderr, "process devolvio error\n");
            return 1;
        }
        for (int i = 0; i < N; ++i) {
            double e1 = fabs(outL[i] - inL[i]);
            double e2 = fabs(outR[i] - inR[i]);
            if (e1 > maxErr) maxErr = e1;
            if (e2 > maxErr) maxErr = e2;
        }
    }
    printf("passthrough: error max %.9f %s\n", maxErr,
           maxErr < 1e-6 ? "(OK)" : "(FALLO)");
    if (maxErr >= 1e-6) return 1;

    plug->stop_processing(plug);

    /* estado: guardar y recargar */
    const clap_plugin_state_t* state =
        (const clap_plugin_state_t*)plug->get_extension(plug, CLAP_EXT_STATE);
    if (state) {
        // static: 1<<20 bytes is fine on an 8 MB Linux thread stack but
        // overflows Windows' 1 MB default stack as a plain local.
        static MemStream m;
        m.len = m.pos = 0;
        clap_ostream_t os = {.ctx = &m, .write = mem_write};
        if (!state->save(plug, &os)) {
            fprintf(stderr, "state save fallo\n");
            return 1;
        }
        printf("estado guardado: %llu bytes\n", (unsigned long long)m.len);
        clap_istream_t is = {.ctx = &m, .read = mem_read};
        if (!state->load(plug, &is)) {
            fprintf(stderr, "state load fallo\n");
            return 1;
        }
        printf("estado recargado (OK)\n");
    }

    /* gui opcional (crea y destruye la ventana embebible, sin mostrarla) */
    if (argc > 2 && !strcmp(argv[2], "gui")) {
        /* inyecta un documento via state: seno de 2 s con loop y root note,
         * para ver la onda, los marcadores y el osciloscopio en accion */
        if (state) {
            static MemStream m2;
            m2.len = m2.pos = 0;
            uint32_t magic = 0x53434C50, ver = 2, fmt = 32, nch = 2;
            double sr = 44100.0;
            uint64_t frames = 88200;
            int64_t ls = 10000, le = 30000;
            int32_t root = 69;
            float cents = -25.f;
            uint8_t loopOn = 1;
            const char* name = "kick_test";
            uint32_t nameLen = (uint32_t)strlen(name);
            clap_ostream_t os2 = {.ctx = &m2, .write = mem_write};
            mem_write(&os2, &magic, 4); mem_write(&os2, &ver, 4);
            mem_write(&os2, &sr, 8); mem_write(&os2, &fmt, 4);
            mem_write(&os2, &nch, 4); mem_write(&os2, &frames, 8);
            mem_write(&os2, &ls, 8); mem_write(&os2, &le, 8);
            mem_write(&os2, &root, 4); mem_write(&os2, &cents, 4);
            mem_write(&os2, &loopOn, 1);
            mem_write(&os2, &nameLen, 4); mem_write(&os2, name, nameLen);
            for (uint32_t c = 0; c < nch; ++c)
                for (uint64_t i = 0; i < frames; ++i) {
                    double env = exp(-(double)i / 30000.0);
                    float v = (float)(0.8 * env *
                                      sin(2 * M_PI * 440.0 * i / 44100.0));
                    mem_write(&os2, &v, 4);
                }
            clap_istream_t is2 = {.ctx = &m2, .read = mem_read};
            if (state->load(plug, &is2))
                printf("documento de prueba inyectado via state\n");
        }
        const clap_plugin_gui_t* gui =
            (const clap_plugin_gui_t*)plug->get_extension(plug, CLAP_EXT_GUI);
        if (gui && gui->is_api_supported(plug, CLAP_TEST_WINDOW_API, false)) {
            if (!gui->create(plug, CLAP_TEST_WINDOW_API, false)) {
                fprintf(stderr, "gui create fallo\n");
                return 1;
            }
            uint32_t w = 0, h = 0;
            gui->get_size(plug, &w, &h);
            gui->show(plug);
            printf("gui creada y visible (%ux%u), renderizando 4 s...\n", w, h);
            fflush(stdout);
#ifdef _WIN32
            Sleep(4000);
#else
            sleep(4);
#endif
            gui->destroy(plug);
            printf("gui destruida (OK)\n");
        }
    }

    plug->deactivate(plug);
    plug->destroy(plug);
    entry->deinit();
    printf("todo OK\n");
    return 0;
}
