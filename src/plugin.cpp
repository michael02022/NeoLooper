// CLAP glue: entry point, factory, plugin, audio ports, state and GUI.
#include <clap/clap.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#include "document.hpp"
#include "engine.hpp"
#include "gui/editor_ui.hpp"
#include "gui/gui_window.hpp"

namespace {

constexpr uint32_t kDefaultW = 1150, kDefaultH = 720;

struct Plugin {
    clap_plugin_t plug{};
    const clap_host_t* host = nullptr;

    Engine engine;
    // Serializes Document between the GUI thread (draw) and the main one (state).
    std::recursive_mutex docMutex;
    Document doc;
    std::unique_ptr<EditorUI> ui;
    std::unique_ptr<GuiWindow> window;

    explicit Plugin(const clap_host_t* h)
        : host(h),
          doc(engine.params,
              [this](AudioDataPtr d) { engine.publishDocument(std::move(d)); }) {
        ui = std::make_unique<EditorUI>(doc, engine);
    }
};

Plugin* self(const clap_plugin_t* p) {
    return static_cast<Plugin*>(p->plugin_data);
}

const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                           CLAP_PLUGIN_FEATURE_UTILITY, nullptr};

const clap_plugin_descriptor_t kDesc = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.michael02022.neolooper",
    .name = "NeoLooper",
    .vendor = "michael02022",
    .url = "https://github.com/michael02022/NeoLooper",
    .manual_url = "",
    .support_url = "",
    .version = "0.1.0",
    .description = "Audio editor for looping samples, Edison-inspired. "
                   "Record, edit, mark loops with crossfades, and export WAV+smpl.",
    .features = kFeatures,
};

// ------------------------------------------------------------- audio ports

uint32_t portsCount(const clap_plugin_t*, bool) { return 1; }

bool portsGet(const clap_plugin_t*, uint32_t index, bool is_input,
              clap_audio_port_info_t* info) {
    if (index != 0) return false;
    info->id = is_input ? 0 : 1;
    snprintf(info->name, sizeof(info->name), "%s",
             is_input ? "Input" : "Output");
    info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = is_input ? 1 : 0;
    return true;
}

const clap_plugin_audio_ports_t kExtAudioPorts = {
    .count = portsCount,
    .get = portsGet,
};

// ------------------------------------------------------------------ state

bool writeAll(const clap_ostream_t* s, const void* buf, uint64_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n > 0) {
        int64_t w = s->write(s, p, n);
        if (w <= 0) return false;
        p += w;
        n -= (uint64_t)w;
    }
    return true;
}

bool readAll(const clap_istream_t* s, void* buf, uint64_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n > 0) {
        int64_t r = s->read(s, p, n);
        if (r <= 0) return false;
        p += r;
        n -= (uint64_t)r;
    }
    return true;
}

constexpr uint32_t kStateMagic = 0x53434C50; // "SCLP"
constexpr uint32_t kStateVersion = 2; // v2: adds the sample name

bool stateSave(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    Plugin* p = self(plugin);
    std::lock_guard<std::recursive_mutex> l(p->docMutex);
    AudioDataPtr d = p->doc.data();

    uint32_t magic = kStateMagic, ver = kStateVersion;
    double sr = d ? d->sampleRate : 44100.0;
    uint32_t fmt = d && d->format == SampleFormat::Int16 ? 16 : 32;
    uint32_t nch = d ? (uint32_t)d->channels() : 0;
    uint64_t frames = d ? (uint64_t)d->frames() : 0;
    int64_t ls = p->doc.loopStart(), le = p->doc.loopEnd();
    int32_t root = p->doc.rootNote();
    float cents = p->doc.tuneCents();
    uint8_t loopOn = p->engine.params.loopEnabled.load() ? 1 : 0;
    const std::string& name = p->doc.sampleName();
    uint32_t nameLen = (uint32_t)name.size();

    if (!writeAll(stream, &magic, 4) || !writeAll(stream, &ver, 4) ||
        !writeAll(stream, &sr, 8) || !writeAll(stream, &fmt, 4) ||
        !writeAll(stream, &nch, 4) || !writeAll(stream, &frames, 8) ||
        !writeAll(stream, &ls, 8) || !writeAll(stream, &le, 8) ||
        !writeAll(stream, &root, 4) || !writeAll(stream, &cents, 4) ||
        !writeAll(stream, &loopOn, 1) || !writeAll(stream, &nameLen, 4) ||
        (nameLen && !writeAll(stream, name.data(), nameLen)))
        return false;
    if (d)
        for (int c = 0; c < d->channels(); ++c)
            if (!writeAll(stream, d->ch[c].data(), frames * 4)) return false;
    return true;
}

bool stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    Plugin* p = self(plugin);
    uint32_t magic = 0, ver = 0, fmt = 0, nch = 0;
    double sr = 0;
    uint64_t frames = 0;
    int64_t ls = -1, le = -1;
    int32_t root = 60;
    float cents = 0;
    uint8_t loopOn = 0;
    uint32_t nameLen = 0;
    std::string name;
    if (!readAll(stream, &magic, 4) || magic != kStateMagic) return false;
    if (!readAll(stream, &ver, 4) || ver != kStateVersion) return false;
    if (!readAll(stream, &sr, 8) || !readAll(stream, &fmt, 4) ||
        !readAll(stream, &nch, 4) || !readAll(stream, &frames, 8) ||
        !readAll(stream, &ls, 8) || !readAll(stream, &le, 8) ||
        !readAll(stream, &root, 4) || !readAll(stream, &cents, 4) ||
        !readAll(stream, &loopOn, 1) || !readAll(stream, &nameLen, 4))
        return false;
    if (nch > 2 || frames > (1ull << 33) || nameLen > 4096) return false;
    if (nameLen) {
        name.resize(nameLen);
        if (!readAll(stream, name.data(), nameLen)) return false;
    }

    std::lock_guard<std::recursive_mutex> l(p->docMutex);
    AudioDataPtr data;
    if (nch > 0 && frames > 0) {
        auto d = std::make_shared<AudioData>();
        d->sampleRate = sr;
        d->format = fmt == 16 ? SampleFormat::Int16 : SampleFormat::Float32;
        d->ch.resize(nch);
        for (uint32_t c = 0; c < nch; ++c) {
            d->ch[c].resize((size_t)frames);
            if (!readAll(stream, d->ch[c].data(), frames * 4)) return false;
        }
        d->buildPeaks();
        data = std::move(d);
    }
    p->doc.loadFromState(data, ls, le, root, cents, name);
    p->engine.params.loopEnabled.store(loopOn != 0);
    return true;
}

const clap_plugin_state_t kExtState = {
    .save = stateSave,
    .load = stateLoad,
};

// -------------------------------------------------------------------- gui

bool guiIsApiSupported(const clap_plugin_t*, const char* api, bool is_floating) {
    return !is_floating && strcmp(api, CLAP_WINDOW_API_X11) == 0;
}

bool guiGetPreferredApi(const clap_plugin_t*, const char** api,
                        bool* is_floating) {
    *api = CLAP_WINDOW_API_X11;
    *is_floating = false;
    return true;
}

bool guiCreate(const clap_plugin_t* plugin, const char* api, bool is_floating) {
    if (!guiIsApiSupported(plugin, api, is_floating)) return false;
    Plugin* p = self(plugin);
    if (p->window) return true;
    p->window = std::make_unique<GuiWindow>(
        [p] {
            std::lock_guard<std::recursive_mutex> l(p->docMutex);
            p->ui->draw();
        },
        [p](const std::string& path) {
            std::lock_guard<std::recursive_mutex> l(p->docMutex);
            p->ui->openPath(path);
        });
    return p->window->create(kDefaultW, kDefaultH);
}

void guiDestroy(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (p->window) {
        p->window->destroy();
        p->window.reset();
    }
}

bool guiSetScale(const clap_plugin_t*, double) { return true; }

bool guiGetSize(const clap_plugin_t* plugin, uint32_t* w, uint32_t* h) {
    Plugin* p = self(plugin);
    if (p->window) p->window->getSize(*w, *h);
    else { *w = kDefaultW; *h = kDefaultH; }
    return true;
}

bool guiCanResize(const clap_plugin_t*) { return true; }

bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = false;
    return true;
}

bool guiAdjustSize(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
    if (*w < 980) *w = 980;
    if (*h < 600) *h = 600;
    return true;
}

bool guiSetSize(const clap_plugin_t* plugin, uint32_t w, uint32_t h) {
    Plugin* p = self(plugin);
    if (p->window) p->window->setSize(w, h);
    return true;
}

bool guiSetParent(const clap_plugin_t* plugin, const clap_window_t* win) {
    Plugin* p = self(plugin);
    if (!p->window || !win) return false;
    p->window->setParent((unsigned long)win->x11);
    return true;
}

bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return true; }

void guiSuggestTitle(const clap_plugin_t*, const char*) {}

bool guiShow(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (!p->window) return false;
    p->window->setVisible(true);
    return true;
}

bool guiHide(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (!p->window) return false;
    p->window->setVisible(false);
    return true;
}

const clap_plugin_gui_t kExtGui = {
    .is_api_supported = guiIsApiSupported,
    .get_preferred_api = guiGetPreferredApi,
    .create = guiCreate,
    .destroy = guiDestroy,
    .set_scale = guiSetScale,
    .get_size = guiGetSize,
    .can_resize = guiCanResize,
    .get_resize_hints = guiGetResizeHints,
    .adjust_size = guiAdjustSize,
    .set_size = guiSetSize,
    .set_parent = guiSetParent,
    .set_transient = guiSetTransient,
    .suggest_title = guiSuggestTitle,
    .show = guiShow,
    .hide = guiHide,
};

// ------------------------------------------------------------------ plugin

bool plugInit(const clap_plugin_t*) { return true; }

void plugDestroy(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    guiDestroy(plugin);
    delete p;
}

bool plugActivate(const clap_plugin_t* plugin, double sr, uint32_t,
                  uint32_t) {
    self(plugin)->engine.activate(sr);
    return true;
}

void plugDeactivate(const clap_plugin_t*) {}

bool plugStartProcessing(const clap_plugin_t*) { return true; }

void plugStopProcessing(const clap_plugin_t*) {}

void plugReset(const clap_plugin_t*) {}

clap_process_status plugProcess(const clap_plugin_t* plugin,
                                const clap_process_t* process) {
    Plugin* p = self(plugin);
    const uint32_t n = process->frames_count;
    if (process->audio_outputs_count < 1 ||
        process->audio_outputs[0].channel_count < 1)
        return CLAP_PROCESS_CONTINUE;

    float** out = process->audio_outputs[0].data32;
    if (!out || !out[0]) return CLAP_PROCESS_CONTINUE;
    float* outL = out[0];
    float* outR = process->audio_outputs[0].channel_count > 1 ? out[1] : out[0];

    const float* inL = nullptr;
    const float* inR = nullptr;
    if (process->audio_inputs_count > 0 && process->audio_inputs[0].data32) {
        const clap_audio_buffer_t& ib = process->audio_inputs[0];
        if (ib.channel_count > 0) inL = ib.data32[0];
        if (ib.channel_count > 1) inR = ib.data32[1];
    }

    p->engine.process(inL, inR, outL, outR, n);
    return CLAP_PROCESS_CONTINUE;
}

const void* plugGetExtension(const clap_plugin_t*, const char* id) {
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kExtAudioPorts;
    if (!strcmp(id, CLAP_EXT_STATE)) return &kExtState;
    if (!strcmp(id, CLAP_EXT_GUI)) return &kExtGui;
    return nullptr;
}

void plugOnMainThread(const clap_plugin_t*) {}

// ----------------------------------------------------------------- factory

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*,
                                   const clap_host_t* host,
                                   const char* plugin_id) {
    if (strcmp(plugin_id, kDesc.id) != 0) return nullptr;
    Plugin* p = new (std::nothrow) Plugin(host);
    if (!p) return nullptr;
    p->plug.desc = &kDesc;
    p->plug.plugin_data = p;
    p->plug.init = plugInit;
    p->plug.destroy = plugDestroy;
    p->plug.activate = plugActivate;
    p->plug.deactivate = plugDeactivate;
    p->plug.start_processing = plugStartProcessing;
    p->plug.stop_processing = plugStopProcessing;
    p->plug.reset = plugReset;
    p->plug.process = plugProcess;
    p->plug.get_extension = plugGetExtension;
    p->plug.on_main_thread = plugOnMainThread;
    return &p->plug;
}

uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factoryDesc(const clap_plugin_factory_t*,
                                            uint32_t index) {
    return index == 0 ? &kDesc : nullptr;
}

const clap_plugin_factory_t kFactory = {
    .get_plugin_count = factoryCount,
    .get_plugin_descriptor = factoryDesc,
    .create_plugin = factoryCreate,
};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryGetFactory(const char* id) {
    if (!strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &kFactory;
    return nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entryInit,
    .deinit = entryDeinit,
    .get_factory = entryGetFactory,
};
