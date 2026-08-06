// Test del nucleo offline: Document (edicion/undo/formatos), resampler HQ y
// exportacion WAV con chunk smpl + recarga.
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

#include "../src/document.hpp"
#include "../src/engine.hpp"
#include "../src/fft.hpp"
#include "../src/foreign_metadata.hpp"
#include "../src/loopmods/auto_loop_finder.hpp"
#include "../src/loopmods/crossfade_inside_core.hpp"
#include "../src/loopmods/freeze_loop_dsp.hpp"
#include "../src/loopmods/freeze_stretch_dsp.hpp"
#include "../src/loopmods/granular_loop_dsp.hpp"
#include "../src/loopmods/harmonic_loop_dsp.hpp"
#include "../src/loopmods/loopauditioneer_finder.hpp"
#include "../src/loopmods/pingpong_harmonic_dsp.hpp"
#include "../src/loopmods/rotation_loop_dsp.hpp"
#include "../src/procmods/amplitude_dsp.hpp"
#include "../src/procmods/filter_dsp.hpp"
#include "../src/procmods/loris_dsp.hpp"
#include "../src/procmods/pitch_dsp.hpp"
#include "../src/procmods/pitch_signalsmith_dsp.hpp"
#include "../src/procmods/rubberband_stretch_dsp.hpp"
#include "../src/procmods/signalsmith_stretch_dsp.hpp"
#include "../src/procmods/stretch_dsp.hpp"
#include "../src/resampler.hpp"
#include "../src/sinc_interp.hpp"
#include "../src/wav_io.hpp"

static int fails = 0;
#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            printf("FALLO: %s\n", msg);           \
            ++fails;                              \
        } else {                                  \
            printf("ok: %s\n", msg);              \
        }                                         \
    } while (0)

int main(int argc, char** argv) {
    std::string tmpdir = argc > 1 ? argv[1] : "/tmp";

    SharedParams params;
    AudioDataPtr published;
    Document doc(params, [&](AudioDataPtr d) { published = std::move(d); });

    // seno estereo de 1 s a 44100, pico 0.5
    auto d = std::make_shared<AudioData>();
    d->sampleRate = 44100;
    d->format = SampleFormat::Float32;
    d->ch.resize(2);
    const int n = 44100;
    for (int c = 0; c < 2; ++c) d->ch[c].resize(n);
    for (int i = 0; i < n; ++i) {
        float v = 0.5f * sinf(2.f * (float)M_PI * 440.f * i / 44100.f);
        d->ch[0][i] = v;
        d->ch[1][i] = v * 0.25f;
    }
    d->buildPeaks();
    doc.setAudio(d, "test");
    CHECK(doc.frames() == 44100, "carga: 44100 muestras");
    CHECK(published && published->frames() == 44100, "snapshot publicado");

    doc.normalize();
    CHECK(std::fabs(doc.data()->peakAbs() - 1.0f) < 1e-4f, "normalizar a 1.0");
    doc.undo();
    CHECK(std::fabs(doc.data()->peakAbs() - 0.5f) < 1e-4f, "deshacer normalizado");
    doc.redo();
    CHECK(std::fabs(doc.data()->peakAbs() - 1.0f) < 1e-4f, "rehacer");

    doc.setSelection(0, 1000);
    doc.cutSelection();
    CHECK(doc.frames() == 43100, "cortar 1000 muestras");
    doc.clearSelection();
    doc.paste();
    CHECK(doc.frames() == 44100, "pegar al final (sin seleccion)");
    doc.setSelection(500, 600);
    doc.paste();
    CHECK(doc.frames() == 45100, "pegar en el inicio de la seleccion");
    doc.undo();
    doc.undo();
    doc.undo();
    CHECK(doc.frames() == 44100, "deshacer x3 restaura la longitud");

    // 16 bits y auto-promocion a 32 al pasarse de amplitud
    doc.toggleBits();
    CHECK(doc.data()->format == SampleFormat::Int16, "toggle a 16-bit");
    doc.pushEnvUndo("Punto de envelope");
    doc.env.amp.addPoint(0, 1.8f);
    doc.env.amp.addPoint(44099, 1.8f);
    doc.applyEnvelopes();
    CHECK(doc.data()->format == SampleFormat::Float32,
          "auto-promocion 16 -> 32 bits al superar la amplitud");
    CHECK(!doc.env.anyActive(), "los envelopes se reinician tras aplicar");
    doc.undo();
    CHECK(doc.env.anyActive() && doc.data()->format == SampleFormat::Int16,
          "deshacer apply restaura los envelopes en edicion");
    doc.redo();
    CHECK(!doc.env.anyActive() && doc.data()->format == SampleFormat::Float32,
          "rehacer vuelve a aplicar los envelopes");

    // Envelope de Pitch (varispeed): a diferencia de amp/pan/sep cambia la
    // duracion, asi que Document::applyEnvelopes() tiene que resamplear y
    // reubicar loop/seleccion -- y el deshacer tiene que restaurar los
    // marcadores VIEJOS (en la escala de tiempo original), no los ya
    // remapeados a la nueva duracion (bug facil: si se escriben los nuevos
    // marcadores ANTES de commit(), el snapshot de deshacer los captura mal).
    // Usa su propio Document/SharedParams, aislado del 'doc' compartido por
    // el resto de este archivo, para no dejarlo con audio/historial
    // inesperado de cara a los tests que le siguen.
    {
        SharedParams pparams;
        AudioDataPtr ppublished;
        Document pdoc(pparams, [&](AudioDataPtr d) { ppublished = std::move(d); });

        auto ps = std::make_shared<AudioData>();
        ps->sampleRate = 44100.0;
        ps->format = SampleFormat::Float32;
        ps->ch.resize(1);
        ps->ch[0].resize(44100);
        for (int i = 0; i < 44100; ++i)
            ps->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * 220.f * i / 44100.f);
        ps->buildPeaks();
        pdoc.setAudio(ps, "Pitch env: setup");
        pdoc.setLoopPoints(10000, 20000, false);

        // Un solo punto de envelope = valor constante en todo el sample (ver
        // Envelope::valueAt), asi que el ratio de varispeed es constante y
        // exacto: 2^(1200/1200) = 2.0 -> la salida es la mitad de larga.
        pdoc.pushEnvUndo("Punto de pitch");
        pdoc.env.pitch.addPoint(0.0, 1200.f);
        pdoc.applyEnvelopes();

        CHECK(pdoc.frames() == 22050, "Pitch env: +1 octava -> mitad de duracion (varispeed x2)");
        CHECK(pdoc.loopStart() == 5000 && pdoc.loopEnd() == 10000,
              "Pitch env: loop start/end reubicados a la nueva escala de tiempo");
        CHECK(!pdoc.env.anyActive(), "Pitch env: se reinicia tras aplicar");
        float peak = pdoc.data()->peakAbs();
        CHECK(peak > 0.01f && peak < 1.0f, "Pitch env: amplitud de salida razonable (sin NaN/overflow)");

        pdoc.undo();
        CHECK(pdoc.frames() == 44100 && pdoc.loopStart() == 10000 && pdoc.loopEnd() == 20000,
              "Pitch env: deshacer restaura duracion y marcadores ORIGINALES (no los remapeados)");
        CHECK(pdoc.env.anyActive(), "Pitch env: deshacer restaura el punto de envelope en edicion");
        pdoc.redo();
        CHECK(pdoc.frames() == 22050 && pdoc.loopStart() == 5000 && pdoc.loopEnd() == 10000,
              "Pitch env: rehacer vuelve a aplicar con los marcadores remapeados");

        // Pitch negativo (mas grave) -> la salida se alarga. Combinado con
        // amp para verificar que ambos caminos conviven sin romperse.
        pdoc.undo(); // vuelve a los 44100 originales, con loop [10000,20000)
        pdoc.env.pitch.pts.clear();
        pdoc.env.pitch.addPoint(0.0, -1200.f);
        pdoc.env.amp.addPoint(0.0, 1.5f);
        pdoc.applyEnvelopes();
        CHECK(pdoc.frames() == 88200, "Pitch env: -1 octava -> el doble de duracion (varispeed x0.5)");
        CHECK(!pdoc.env.anyActive(), "Pitch env: amp+pitch combinados tambien se reinician tras aplicar");
    }

    // Document::reverse(): invierte seleccion (o todo, sin seleccion), sin
    // mover loop start/end (la duracion no cambia). Aislado en su propio
    // Document, mismo motivo que el bloque de Pitch env de arriba.
    {
        SharedParams rparams;
        AudioDataPtr rpublished;
        Document rdoc(rparams, [&](AudioDataPtr d) { rpublished = std::move(d); });

        auto rs = std::make_shared<AudioData>();
        rs->sampleRate = 44100.0;
        rs->format = SampleFormat::Float32;
        rs->ch.resize(1);
        rs->ch[0] = {0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f};
        rs->buildPeaks();
        rdoc.setAudio(rs, "Reverse: setup");
        rdoc.setLoopPoints(2, 6, false);

        // Sin seleccion: invierte todo el sample.
        rdoc.reverse();
        std::vector<float> want1 = {9.f, 8.f, 7.f, 6.f, 5.f, 4.f, 3.f, 2.f, 1.f, 0.f};
        CHECK(rdoc.data()->ch[0] == want1, "Reverse: sin seleccion invierte todo el sample");
        CHECK(rdoc.loopStart() == 2 && rdoc.loopEnd() == 6,
              "Reverse: loop start/end quedan intactos (sin seleccion)");

        rdoc.undo();
        CHECK(rdoc.data()->ch[0] == std::vector<float>({0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f}),
              "Reverse: deshacer restaura el orden original");
        CHECK(rdoc.loopStart() == 2 && rdoc.loopEnd() == 6, "Reverse: deshacer no altera el loop");

        // Con seleccion [2,6): invierte SOLO ese tramo, el resto queda igual.
        rdoc.setSelection(2, 6);
        rdoc.reverse();
        std::vector<float> want2 = {0.f, 1.f, 5.f, 4.f, 3.f, 2.f, 6.f, 7.f, 8.f, 9.f};
        CHECK(rdoc.data()->ch[0] == want2, "Reverse: con seleccion invierte SOLO ese tramo");
        CHECK(rdoc.loopStart() == 2 && rdoc.loopEnd() == 6,
              "Reverse: loop start/end quedan intactos (con seleccion)");
        CHECK(rdoc.frames() == 10, "Reverse: no cambia la duracion del sample");
    }

    // Filtro (filterdsp): high-pass resonante en cascada + banda bell
    // (peaking EQ con ganancia +/-) en el DOMINIO DEL TIEMPO (biquads),
    // reemplaza al Ecualizador anterior por STFT -- mucho mas simple de
    // verificar: sin bandas activas es identidad; un high-pass atenua un
    // tono grave pero deja pasar uno agudo; la banda realza o corta solo
    // alrededor de su frecuencia.
    {
        const double sr = 44100.0;
        const int n = (int)sr; // 1s

        auto makeTone = [&](double freq) {
            auto d = std::make_shared<AudioData>();
            d->sampleRate = sr;
            d->format = SampleFormat::Float32;
            d->ch.resize(1);
            d->ch[0].resize((size_t)n);
            for (int i = 0; i < n; ++i)
                d->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * (float)freq * i / (float)sr);
            return d;
        };
        auto rmsOf = [&](const std::vector<float>& v, size_t skip) {
            double s = 0.0;
            for (size_t i = skip; i < v.size(); ++i) s += (double)v[i] * v[i];
            return std::sqrt(s / (double)(v.size() - skip));
        };
        const size_t skip = (size_t)n / 10; // se salta el asentamiento inicial del filtro

        // sin bandas activas: identidad
        {
            auto tone = makeTone(440.0);
            filterdsp::FilterSpec flat; // hp y band apagados por defecto
            auto out = filterdsp::apply(*tone, 0, n, flat);
            CHECK(!out.empty(), "Filtro: apply sin bandas activas no falla");
            if (!out.empty()) {
                double maxDiff = 0.0;
                for (int i = 0; i < n; ++i)
                    maxDiff = std::max(maxDiff, (double)std::fabs(out[0][(size_t)i] - tone->ch[0][(size_t)i]));
                CHECK(maxDiff < 1e-6, "Filtro: sin bandas activas reconstruye identico a la entrada");
            }
        }

        // high-pass 48 dB/oct @1kHz: atenua fuerte un tono de 100Hz, deja pasar uno de 5kHz
        {
            filterdsp::FilterSpec spec;
            spec.hpOn = true;
            spec.hpStages = 4;
            spec.hpFreq = 1000.f;

            auto toneLow = makeTone(100.0);
            auto outLow = filterdsp::apply(*toneLow, 0, n, spec);
            CHECK(!outLow.empty(), "Filtro: high-pass apply no falla (tono grave)");
            if (!outLow.empty())
                CHECK(rmsOf(outLow[0], skip) < rmsOf(toneLow->ch[0], skip) * 0.1,
                      "Filtro: high-pass de 48 dB/oct @1kHz atenua fuertemente un tono de 100 Hz");

            auto toneHigh = makeTone(5000.0);
            auto outHigh = filterdsp::apply(*toneHigh, 0, n, spec);
            CHECK(!outHigh.empty(), "Filtro: high-pass apply no falla (tono agudo)");
            if (!outHigh.empty())
                CHECK(rmsOf(outHigh[0], skip) > rmsOf(toneHigh->ch[0], skip) * 0.9,
                      "Filtro: high-pass @1kHz deja pasar practicamente intacto un tono de 5 kHz");
        }

        // Q/resonancia: en el corte, un Q alto produce un pico de ganancia
        // (~Q en lineal para una etapa); con el Q por defecto (0.707) la
        // respuesta en el corte es ~-3dB. El Q va solo en la primera etapa,
        // asi que el pico no se multiplica al subir la pendiente.
        {
            filterdsp::FilterSpec spec;
            spec.hpOn = true;
            spec.hpStages = 1;
            spec.hpFreq = 1000.f;
            spec.hpQ = 8.f;
            CHECK(filterdsp::responseAt(spec, 1000.0, sr) > 4.0,
                  "Filtro: Q alto en el high-pass produce un pico resonante en el corte");
            spec.hpQ = 0.707f;
            const double flat = filterdsp::responseAt(spec, 1000.0, sr);
            CHECK(flat > 0.6 && flat < 0.8,
                  "Filtro: Q por defecto (Butterworth) da ~-3dB en el corte, sin resonancia");

            spec.hpQ = 8.f;
            spec.hpStages = 2;
            const double peak = filterdsp::responseAt(spec, 1000.0, sr);
            CHECK(peak > 4.0 && peak < 8.0 * 0.708,
                  "Filtro: el Q resuena solo en la primera etapa (la segunda, Butterworth, "
                  "no multiplica el pico)");
        }

        // banda bell +12dB @5kHz: realza un tono de esa frecuencia (~x4 en
        // lineal) y deja practicamente intacto uno grave -- el caso de uso
        // real: dar brillo a los agudos sin tocar el resto.
        {
            filterdsp::FilterSpec spec;
            spec.bandOn = true;
            spec.bandFreq = 5000.f;
            spec.bandGainDb = 12.f;
            spec.bandQ = 1.f;

            auto tone = makeTone(5000.0);
            auto out = filterdsp::apply(*tone, 0, n, spec);
            CHECK(!out.empty(), "Filtro: band apply no falla (realce)");
            if (!out.empty()) {
                const double ratio = rmsOf(out[0], skip) / rmsOf(tone->ch[0], skip);
                CHECK(ratio > 3.5 && ratio < 4.5,
                      "Filtro: band de +12 dB @5kHz realza ~x4 un tono de 5 kHz");
            }

            auto toneLow = makeTone(100.0);
            auto outLow = filterdsp::apply(*toneLow, 0, n, spec);
            CHECK(!outLow.empty(), "Filtro: band apply no falla (tono lejano)");
            if (!outLow.empty())
                CHECK(rmsOf(outLow[0], skip) < rmsOf(toneLow->ch[0], skip) * 1.1,
                      "Filtro: band @5kHz no realza un tono de 100 Hz");
        }

        // banda bell -40dB @1kHz: cortando tambien funciona (mismo nucleo
        // peaking que los notches que reemplaza)
        {
            filterdsp::FilterSpec spec;
            spec.bandOn = true;
            spec.bandFreq = 1000.f;
            spec.bandGainDb = -40.f;
            spec.bandQ = 4.f;

            auto tone = makeTone(1000.0);
            auto out = filterdsp::apply(*tone, 0, n, spec);
            CHECK(!out.empty(), "Filtro: band apply no falla (corte)");
            if (!out.empty())
                CHECK(rmsOf(out[0], skip) < rmsOf(tone->ch[0], skip) * 0.1,
                      "Filtro: band de -40 dB centrada en 1 kHz atenua fuertemente un tono de 1 kHz");
        }

        // rango invalido -> vacio con gracia (un biquad no exige un largo
        // minimo como el STFT anterior, asi que el unico caso de fallo real
        // es s1<=s0)
        {
            auto tone = makeTone(440.0);
            filterdsp::FilterSpec spec;
            CHECK(filterdsp::apply(*tone, 100, 100, spec).empty(),
                  "Filtro: apply devuelve vacio con gracia si s1<=s0");
        }

        // responseAt: el high-pass esta muy atenuado 2 octavas por debajo
        // del corte y practicamente en 0dB una decada por encima; la banda
        // tiene su pico justo en su frecuencia central y no afecta
        // frecuencias lejanas.
        {
            filterdsp::FilterSpec spec;
            spec.hpOn = true;
            spec.hpStages = 2; // 24 dB/oct
            spec.hpFreq = 200.f;
            CHECK(filterdsp::responseAt(spec, 50.0, sr) < 0.1,
                  "Filtro: responseAt del high-pass esta muy atenuado 2 octavas por debajo del corte");
            CHECK(filterdsp::responseAt(spec, 2000.0, sr) > 0.9,
                  "Filtro: responseAt del high-pass esta practicamente en 0dB una decada por encima del corte");
        }
        // Pendiente "pared" (kMaxStages, 96 dB/oct): a UNA octava por
        // debajo del corte ya tiene que estar muchisimo mas atenuada que la
        // pendiente suave de 12 dB/oct a esa misma distancia relativa.
        {
            filterdsp::FilterSpec gentle;
            gentle.hpOn = true;
            gentle.hpStages = 1; // 12 dB/oct
            gentle.hpFreq = 200.f;
            filterdsp::FilterSpec steep;
            steep.hpOn = true;
            steep.hpStages = filterdsp::kMaxStages; // 96 dB/oct
            steep.hpFreq = 200.f;
            const double gGentle = filterdsp::responseAt(gentle, 100.0, sr); // 1 octava por debajo
            const double gSteep = filterdsp::responseAt(steep, 100.0, sr);
            CHECK(gSteep < gGentle * 0.01,
                  "Filtro: la pendiente mas agresiva (96 dB/oct) atenua muchisimo mas que 12 dB/oct a "
                  "la misma distancia del corte");
        }
        {
            filterdsp::FilterSpec spec;
            spec.bandOn = true;
            spec.bandFreq = 1000.f;
            spec.bandGainDb = 12.f;
            spec.bandQ = 8.f;
            const double center = filterdsp::responseAt(spec, 1000.0, sr);
            CHECK(center > 3.5 && center < 4.5,
                  "Filtro: responseAt de la band tiene su pico (+12 dB ~ x4) en la frecuencia central");
            CHECK(filterdsp::responseAt(spec, 100.0, sr) < 1.1,
                  "Filtro: responseAt de la band no afecta frecuencias lejanas");
        }

        // analyzeStaticSpectrum: el pico de un tono de 1kHz aparece en el
        // bin correcto (una sola curva promediada, no una nube de puntos
        // por frame -- por diseno no puede tener el bug de conexiones
        // espurias entre frames del intento anterior).
        {
            auto tone = makeTone(1000.0);
            filterdsp::SpectrumCurve curve;
            filterdsp::analyzeStaticSpectrum(*tone, 0, n, 2048, 100, curve);
            CHECK(!curve.db.empty(), "Filtro: analyzeStaticSpectrum devuelve una curva para un tono audible");
            if (!curve.db.empty()) {
                int peakBin = 0;
                float peakDb = -1000.f;
                for (int k = 0; k < (int)curve.db.size(); ++k)
                    if (curve.db[(size_t)k] > peakDb) { peakDb = curve.db[(size_t)k]; peakBin = k; }
                double peakFreq = peakBin * curve.sr / curve.nFft;
                CHECK(peakFreq > 950.0 && peakFreq < 1050.0,
                      "Filtro: analyzeStaticSpectrum ubica el pico de un tono de 1 kHz en el bin correcto");
            }
        }
    }

    // Document::applyFilter: seleccion vs todo el sample, loop start/end
    // intactos (misma duracion), deshacer restaura el original. Aislado en
    // su propio Document, mismo motivo que los bloques de Pitch/Reverse.
    {
        SharedParams fparams;
        AudioDataPtr fpublished;
        Document fdoc(fparams, [&](AudioDataPtr d) { fpublished = std::move(d); });

        auto fs = std::make_shared<AudioData>();
        fs->sampleRate = 44100.0;
        fs->format = SampleFormat::Float32;
        fs->ch.resize(1);
        fs->ch[0].resize(44100);
        for (int i = 0; i < 44100; ++i)
            // tono grave (100Hz): un high-pass @1kHz lo corta fuerte
            fs->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * 100.f * i / 44100.f);
        fs->buildPeaks();
        fdoc.setAudio(fs, "Filtro doc: setup");
        fdoc.setLoopPoints(1000, 2000, false);

        filterdsp::FilterSpec spec;
        spec.hpOn = true;
        spec.hpStages = 4;
        spec.hpFreq = 1000.f;

        fdoc.applyFilter(spec);
        CHECK(fdoc.frames() == 44100, "Filtro doc: no cambia la duracion (todo el sample)");
        CHECK(fdoc.loopStart() == 1000 && fdoc.loopEnd() == 2000,
              "Filtro doc: loop start/end quedan intactos");
        {
            const auto& v = fdoc.data()->ch[0];
            double s = 0.0;
            for (size_t i = v.size() / 10; i < v.size(); ++i) s += (double)v[i] * v[i];
            double rms = std::sqrt(s / (double)(v.size() - v.size() / 10));
            CHECK(rms < 0.05, "Filtro doc: high-pass fuerte atenua un tono grave (RMS de la cola)");
        }

        fdoc.undo();
        CHECK(std::fabs(fdoc.data()->peakAbs() - 0.5f) < 1e-3f,
              "Filtro doc: deshacer restaura la amplitud original");
        CHECK(fdoc.loopStart() == 1000 && fdoc.loopEnd() == 2000, "Filtro doc: deshacer no altera el loop");

        // con seleccion: solo esa zona cambia
        fdoc.setSelection(10000, 20000);
        fdoc.applyFilter(spec);
        CHECK(fdoc.frames() == 44100, "Filtro doc (seleccion): no cambia la duracion");
        float outsideSel = fdoc.data()->ch[0][5000]; // fuera de [10000,20000): copia sin tocar de cur_
        float expected = 0.5f * sinf(2.f * (float)M_PI * 100.f * 5000.f / 44100.f);
        CHECK(std::fabs(outsideSel - expected) < 1e-5f,
              "Filtro doc (seleccion): fuera de la seleccion el audio no cambia");
    }

    // amplitudedsp (procmods/amplitude_dsp.hpp): ganancia simple en dB +
    // clip opcional (ON por defecto).
    {
        CHECK(std::fabs(amplitudedsp::gainLinear(amplitudedsp::AmplitudeSpec{}) - 1.0) < 1e-9,
              "Amplitude: gainLinear() en 0 dB es 1.0 (passthrough)");
        {
            amplitudedsp::AmplitudeSpec up20;
            up20.gainDb = 20.f;
            CHECK(std::fabs(amplitudedsp::gainLinear(up20) - 10.0) < 1e-6,
                  "Amplitude: gainLinear() de +20 dB es x10");
        }
        {
            amplitudedsp::AmplitudeSpec down20;
            down20.gainDb = -20.f;
            CHECK(std::fabs(amplitudedsp::gainLinear(down20) - 0.1) < 1e-6,
                  "Amplitude: gainLinear() de -20 dB es x0.1");
        }

        auto sig = std::make_shared<AudioData>();
        sig->sampleRate = 44100.0;
        sig->format = SampleFormat::Float32;
        sig->ch.resize(1);
        sig->ch[0].resize(1000);
        for (int i = 0; i < 1000; ++i)
            sig->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * 5.f * i / 1000.f);
        // Envuelve un vector de salida crudo en un AudioData para poder
        // reusar amplitudedsp::peakAbs sobre el, mismo patron 'wrap' que
        // los tests de pitchdsp de mas abajo.
        auto wrapOut = [&](const std::vector<float>& v) {
            AudioData d;
            d.sampleRate = 44100.0;
            d.format = SampleFormat::Float32;
            d.ch.resize(1);
            d.ch[0] = v;
            return d;
        };

        // peakAbs(): SOLO mira la region pedida, no el buffer entero -- el
        // pico real (0.5) esta cerca del centro; una region que lo excluye
        // debe reportar un pico menor.
        {
            float peakWhole = amplitudedsp::peakAbs(*sig, 0, 1000);
            CHECK(std::fabs(peakWhole - 0.5f) < 1e-3f,
                  "Amplitude: peakAbs() sobre todo el buffer encuentra el pico real (~0.5)");
            float peakNarrow = amplitudedsp::peakAbs(*sig, 0, 10);
            CHECK(peakNarrow < peakWhole,
                  "Amplitude: peakAbs() esta acotado a la region pedida, no al buffer entero");
        }

        // 0 dB, clip=true: passthrough exacto (los valores ya caen dentro
        // de [-1,1], el clamp no hace nada).
        {
            amplitudedsp::AmplitudeSpec z;
            auto out = amplitudedsp::apply(*sig, 0, 1000, z);
            CHECK(out.size() == 1 && out[0].size() == 1000,
                  "Amplitude: apply a 0 dB devuelve la longitud pedida");
            double maxDiff = 0.0;
            for (int i = 0; i < 1000; ++i)
                maxDiff = std::max(maxDiff, (double)std::fabs(out[0][(size_t)i] - sig->ch[0][(size_t)i]));
            CHECK(maxDiff < 1e-9, "Amplitude: 0 dB con clip=true es passthrough exacto");
        }

        // +20 dB (x10) con clip=false: el pico (0.5) se va a 5.0, SIN
        // recortar -- valor exacto, no solo "mayor que 1".
        {
            amplitudedsp::AmplitudeSpec up20;
            up20.gainDb = 20.f;
            up20.clip = false;
            auto out = amplitudedsp::apply(*sig, 0, 1000, up20);
            float peakOut = amplitudedsp::peakAbs(wrapOut(out[0]), 0, 1000);
            CHECK(std::fabs(peakOut - 5.0f) < 1e-3f,
                  "Amplitude: +20 dB con clip=false deja el pico exactamente en x10, sin recortar");
        }

        // Mismo +20 dB con clip=true: el pico queda pegado al techo de
        // [-1,1], nunca por encima.
        {
            amplitudedsp::AmplitudeSpec up20c;
            up20c.gainDb = 20.f;
            up20c.clip = true;
            auto out = amplitudedsp::apply(*sig, 0, 1000, up20c);
            float peakOut = amplitudedsp::peakAbs(wrapOut(out[0]), 0, 1000);
            CHECK(std::fabs(peakOut - 1.0f) < 1e-6f,
                  "Amplitude: +20 dB con clip=true recorta el pico exactamente a 1.0");
        }

        // -20 dB (x0.1): el pico baja a 0.05, clip no interviene (nunca se
        // acerca al techo).
        {
            amplitudedsp::AmplitudeSpec down20;
            down20.gainDb = -20.f;
            auto out = amplitudedsp::apply(*sig, 0, 1000, down20);
            float peakOut = amplitudedsp::peakAbs(wrapOut(out[0]), 0, 1000);
            CHECK(std::fabs(peakOut - 0.05f) < 1e-4f,
                  "Amplitude: -20 dB deja el pico exactamente en x0.1");
        }

        // rango invalido -> vacio con gracia
        CHECK(amplitudedsp::apply(*sig, 100, 100, amplitudedsp::AmplitudeSpec{}).empty(),
              "Amplitude: apply devuelve vacio con gracia si s1<=s0");
    }

    // Document::applyAmplitude: seleccion vs todo el sample, loop start/end
    // intactos (misma duracion), deshacer restaura el original -- mismo
    // criterio que el bloque de Filtro de arriba.
    {
        SharedParams aparams;
        AudioDataPtr apublished;
        Document adoc(aparams, [&](AudioDataPtr d) { apublished = std::move(d); });

        auto as2 = std::make_shared<AudioData>();
        as2->sampleRate = 44100.0;
        as2->format = SampleFormat::Float32;
        as2->ch.resize(1);
        as2->ch[0].resize(44100);
        for (int i = 0; i < 44100; ++i)
            as2->ch[0][(size_t)i] = 0.25f * sinf(2.f * (float)M_PI * 220.f * i / 44100.f);
        as2->buildPeaks();
        adoc.setAudio(as2, "Amplitude doc: setup");
        adoc.setLoopPoints(1000, 2000, false);

        amplitudedsp::AmplitudeSpec up12; // x4 (~+12dB): 0.25 pico -> 1.0 pico
        up12.gainDb = 20.f * std::log10(4.0);
        adoc.applyAmplitude(up12);
        CHECK(adoc.frames() == 44100, "Amplitude doc: no cambia la duracion (todo el sample)");
        CHECK(adoc.loopStart() == 1000 && adoc.loopEnd() == 2000,
              "Amplitude doc: loop start/end quedan intactos");
        CHECK(std::fabs(adoc.data()->peakAbs() - 1.0f) < 1e-3f,
              "Amplitude doc: la ganancia se aplica a todo el sample (pico x4)");

        adoc.undo();
        CHECK(std::fabs(adoc.data()->peakAbs() - 0.25f) < 1e-3f,
              "Amplitude doc: deshacer restaura la amplitud original");
        CHECK(adoc.loopStart() == 1000 && adoc.loopEnd() == 2000, "Amplitude doc: deshacer no altera el loop");

        // con seleccion: solo esa zona cambia
        adoc.setSelection(10000, 20000);
        adoc.applyAmplitude(up12);
        CHECK(adoc.frames() == 44100, "Amplitude doc (seleccion): no cambia la duracion");
        float outsideSel = adoc.data()->ch[0][5000]; // fuera de [10000,20000): copia sin tocar de cur_
        float expected = 0.25f * sinf(2.f * (float)M_PI * 220.f * 5000.f / 44100.f);
        CHECK(std::fabs(outsideSel - expected) < 1e-5f,
              "Amplitude doc (seleccion): fuera de la seleccion el audio no cambia");
    }

    // pitchdsp (procmods/pitch_dsp.hpp): transpose real, dos modos --
    // tradicional (resample simple, la duracion cambia con el tono, modo
    // por defecto) y stretch (vocoder de fase + resample, duracion fija,
    // opt-in). No confundir con el envelope de Pitch (varispeed) testeado
    // arriba, que tiene su propio mecanismo (Document::applyEnvelopes).
    {
        // ratio(): formula estandar, semitonos y cents son intercambiables
        // (100 cents == 1 semitono).
        {
            pitchdsp::PitchSpec z;
            CHECK(std::fabs(pitchdsp::ratio(z) - 1.0) < 1e-12,
                  "Pitch shift: ratio() sin desplazamiento es 1.0");
            pitchdsp::PitchSpec up12; up12.semitones = 12.f;
            CHECK(std::fabs(pitchdsp::ratio(up12) - 2.0) < 1e-9,
                  "Pitch shift: ratio() de +12 semitonos es 2.0 (una octava arriba)");
            pitchdsp::PitchSpec down12; down12.semitones = -12.f;
            CHECK(std::fabs(pitchdsp::ratio(down12) - 0.5) < 1e-9,
                  "Pitch shift: ratio() de -12 semitonos es 0.5 (una octava abajo)");
            pitchdsp::PitchSpec cents100; cents100.cents = 100.f;
            pitchdsp::PitchSpec semi1; semi1.semitones = 1.f;
            CHECK(std::fabs(pitchdsp::ratio(cents100) - pitchdsp::ratio(semi1)) < 1e-9,
                  "Pitch shift: 100 cents == 1 semitono");
        }

        const double sr = 44100.0;
        const int n = (int)sr; // 1s
        auto makeTone = [&](double freq) {
            auto d = std::make_shared<AudioData>();
            d->sampleRate = sr;
            d->format = SampleFormat::Float32;
            d->ch.resize(1);
            d->ch[0].resize((size_t)n);
            for (int i = 0; i < n; ++i)
                d->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * (float)freq * i / (float)sr);
            return d;
        };
        // Envuelve un vector crudo en un AudioData para reusar
        // granulardsp::estimatePeriod (autocorrelacion ya testeada) como
        // verificacion real del pitch resultante, no solo de la forma/
        // longitud del buffer.
        auto wrap = [&](const std::vector<float>& v) {
            AudioData d;
            d.sampleRate = sr;
            d.format = SampleFormat::Float32;
            d.ch.resize(1);
            d.ch[0] = v;
            return d;
        };

        // sin desplazamiento: passthrough exacto (rama sin FFT/resample),
        // bit a bit, en ambos modos.
        for (bool stretch : {false, true}) {
            auto tone = makeTone(440.0);
            pitchdsp::PitchSpec z;
            auto out = pitchdsp::apply(*tone, 0, n, z, 2048, stretch);
            CHECK(out.size() == 1 && out[0].size() == (size_t)n,
                  "Pitch shift: apply sin desplazamiento devuelve la longitud pedida");
            if (out.size() == 1 && out[0].size() == (size_t)n) {
                double maxDiff = 0.0;
                for (int i = 0; i < n; ++i)
                    maxDiff = std::max(maxDiff, (double)std::fabs(out[0][(size_t)i] - tone->ch[0][(size_t)i]));
                CHECK(maxDiff < 1e-9,
                      "Pitch shift: sin desplazamiento reconstruye identico a la entrada (sin gastar FFT/resample)");
            }
        }

        // Modo stretch (preserveDuration=true): +1 octava lleva 220Hz a
        // ~440Hz SIN cambiar la longitud del buffer.
        {
            auto tone = makeTone(220.0);
            pitchdsp::PitchSpec up;
            up.semitones = 12.f;
            auto out = pitchdsp::apply(*tone, 0, n, up, 2048, /*preserveDuration=*/true);
            CHECK(out.size() == 1 && out[0].size() == (size_t)n,
                  "Pitch shift (stretch): +1 octava preserva la longitud exacta del buffer");
            if (out.size() == 1 && out[0].size() == (size_t)n) {
                // se salta el primer cuarto (asentamiento del vocoder de
                // fase) y mide sobre una ventana central, igual criterio que
                // el skip de asentamiento del test de Filtro.
                AudioData shifted = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(shifted, n / 4, n / 2);
                CHECK(period > 0, "Pitch shift (stretch): +1 octava produce una senal con periodo detectable");
                if (period > 0) {
                    double freq = sr / (double)period;
                    printf("   Pitch shift (stretch) +12st: 220Hz -> %.1fHz detectado (periodo %lld)\n",
                          freq, (long long)period);
                    CHECK(freq > 420.0 && freq < 460.0,
                          "Pitch shift (stretch): +1 octava lleva un tono de 220Hz a ~440Hz");
                }
            }
        }

        // Modo stretch: -1 octava, 440Hz -> ~220Hz.
        {
            auto tone = makeTone(440.0);
            pitchdsp::PitchSpec down;
            down.semitones = -12.f;
            auto out = pitchdsp::apply(*tone, 0, n, down, 2048, /*preserveDuration=*/true);
            CHECK(out.size() == 1 && out[0].size() == (size_t)n,
                  "Pitch shift (stretch): -1 octava preserva la longitud exacta del buffer");
            if (out.size() == 1 && out[0].size() == (size_t)n) {
                AudioData shifted = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(shifted, n / 4, n / 2);
                CHECK(period > 0, "Pitch shift (stretch): -1 octava produce una senal con periodo detectable");
                if (period > 0) {
                    double freq = sr / (double)period;
                    printf("   Pitch shift (stretch) -12st: 440Hz -> %.1fHz detectado (periodo %lld)\n",
                          freq, (long long)period);
                    CHECK(freq > 205.0 && freq < 235.0,
                          "Pitch shift (stretch): -1 octava lleva un tono de 440Hz a ~220Hz");
                }
            }
        }

        // Modo tradicional (preserveDuration=false, default): +1 octava
        // ACORTA el buffer a la mitad (resample simple) y tambien lleva
        // 220Hz a ~440Hz.
        {
            auto tone = makeTone(220.0);
            pitchdsp::PitchSpec up;
            up.semitones = 12.f;
            auto out = pitchdsp::apply(*tone, 0, n, up, 2048, /*preserveDuration=*/false);
            CHECK(out.size() == 1, "Pitch shift (tradicional): apply no falla");
            if (out.size() == 1) {
                CHECK(std::llabs((long long)out[0].size() - n / 2) <= 2,
                      "Pitch shift (tradicional): +1 octava acorta el buffer a ~la mitad");
                AudioData shifted = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(shifted, (int64_t)out[0].size() / 4,
                                                              (int64_t)out[0].size() / 2);
                CHECK(period > 0, "Pitch shift (tradicional): +1 octava produce una senal con periodo detectable");
                if (period > 0) {
                    double freq = sr / (double)period;
                    printf("   Pitch shift (tradicional) +12st: 220Hz -> %.1fHz detectado (periodo %lld)\n",
                          freq, (long long)period);
                    CHECK(freq > 420.0 && freq < 460.0,
                          "Pitch shift (tradicional): +1 octava lleva un tono de 220Hz a ~440Hz");
                }
            }
        }

        // Modo tradicional: -1 octava DUPLICA el buffer.
        {
            auto tone = makeTone(440.0);
            pitchdsp::PitchSpec down;
            down.semitones = -12.f;
            auto out = pitchdsp::apply(*tone, 0, n, down, 2048, /*preserveDuration=*/false);
            CHECK(out.size() == 1, "Pitch shift (tradicional, grave): apply no falla");
            if (out.size() == 1)
                CHECK(std::llabs((long long)out[0].size() - n * 2) <= 4,
                      "Pitch shift (tradicional): -1 octava duplica el largo del buffer");
        }

        // rango invalido -> vacio con gracia, en ambos modos
        for (bool stretch : {false, true}) {
            auto tone = makeTone(440.0);
            pitchdsp::PitchSpec up;
            up.semitones = 5.f;
            CHECK(pitchdsp::apply(*tone, 100, 100, up, 2048, stretch).empty(),
                  "Pitch shift: apply devuelve vacio con gracia si s1<=s0");
        }
    }

    // pitchsignalsmithdsp (procmods/pitch_signalsmith_dsp.hpp): pitch shift
    // via Signalsmith Stretch, SIEMPRE preserva la duracion (a diferencia
    // de pitchdsp en modo tradicional) -- verificado con
    // granulardsp::estimatePeriod, mismo criterio que los tests de
    // pitchdsp de arriba.
    {
        const double sr = 44100.0;
        const int n = (int)sr; // 1s
        auto makeTone = [&](double freq) {
            auto d = std::make_shared<AudioData>();
            d->sampleRate = sr;
            d->format = SampleFormat::Float32;
            d->ch.resize(1);
            d->ch[0].resize((size_t)n);
            for (int i = 0; i < n; ++i)
                d->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * (float)freq * i / (float)sr);
            return d;
        };
        auto wrap = [&](const std::vector<float>& v) {
            AudioData d;
            d.sampleRate = sr;
            d.format = SampleFormat::Float32;
            d.ch.resize(1);
            d.ch[0] = v;
            return d;
        };

        // +1 octava: 220Hz -> ~440Hz, longitud EXACTA (a diferencia de
        // pitchdsp's modo tradicional, este metodo no tiene ese modo).
        {
            auto tone = makeTone(220.0);
            pitchdsp::PitchSpec up;
            up.semitones = 12.f;
            auto out = pitchsignalsmithdsp::apply(*tone, 0, n, up, 0.f, false);
            CHECK(out.size() == 1 && out[0].size() == (size_t)n,
                  "Pitch (Signalsmith): +1 octava preserva la longitud exacta del buffer");
            if (out.size() == 1 && out[0].size() == (size_t)n) {
                AudioData shifted = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(shifted, n / 4, n / 2);
                CHECK(period > 0, "Pitch (Signalsmith): +1 octava produce una senal con periodo detectable");
                if (period > 0) {
                    double freq = sr / (double)period;
                    printf("   Pitch (Signalsmith) +12st: 220Hz -> %.1fHz detectado (periodo %lld)\n",
                          freq, (long long)period);
                    CHECK(freq > 420.0 && freq < 460.0,
                          "Pitch (Signalsmith): +1 octava lleva un tono de 220Hz a ~440Hz");
                }
            }
        }

        // -1 octava: 440Hz -> ~220Hz.
        {
            auto tone = makeTone(440.0);
            pitchdsp::PitchSpec down;
            down.semitones = -12.f;
            auto out = pitchsignalsmithdsp::apply(*tone, 0, n, down, 0.f, false);
            CHECK(out.size() == 1 && out[0].size() == (size_t)n,
                  "Pitch (Signalsmith): -1 octava preserva la longitud exacta del buffer");
            if (out.size() == 1 && out[0].size() == (size_t)n) {
                AudioData shifted = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(shifted, n / 4, n / 2);
                CHECK(period > 0, "Pitch (Signalsmith): -1 octava produce una senal con periodo detectable");
                if (period > 0) {
                    double freq = sr / (double)period;
                    printf("   Pitch (Signalsmith) -12st: 440Hz -> %.1fHz detectado (periodo %lld)\n",
                          freq, (long long)period);
                    CHECK(freq > 205.0 && freq < 235.0,
                          "Pitch (Signalsmith): -1 octava lleva un tono de 440Hz a ~220Hz");
                }
            }
        }

        // tonalityLimitHz y preserveFormants activos a la vez: el tono de
        // prueba (220Hz) cae muy por debajo de cualquier limite razonable,
        // asi que el pitch detectado no deberia cambiar -- solo verifica
        // que ninguna de las dos opciones rompe el resultado.
        {
            auto tone = makeTone(220.0);
            pitchdsp::PitchSpec up;
            up.semitones = 12.f;
            auto out = pitchsignalsmithdsp::apply(*tone, 0, n, up, 5000.f, true);
            CHECK(out.size() == 1 && out[0].size() == (size_t)n,
                  "Pitch (Signalsmith): con tonality limit + preserve formants, la longitud sigue exacta");
            if (out.size() == 1 && out[0].size() == (size_t)n) {
                AudioData shifted = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(shifted, n / 4, n / 2);
                CHECK(period > 0,
                      "Pitch (Signalsmith): con tonality limit + preserve formants, el periodo sigue siendo detectable");
                if (period > 0) {
                    double freq = sr / (double)period;
                    CHECK(freq > 420.0 && freq < 460.0,
                          "Pitch (Signalsmith): tonality limit + preserve formants no descarrilan el shift de +1 octava");
                }
            }
        }

        // rango invalido -> vacio con gracia
        {
            auto tone = makeTone(440.0);
            pitchdsp::PitchSpec up;
            up.semitones = 5.f;
            CHECK(pitchsignalsmithdsp::apply(*tone, 100, 100, up, 0.f, false).empty(),
                  "Pitch (Signalsmith): apply devuelve vacio con gracia si s1<=s0");
        }
    }

    // Document::applyPitch, modo stretch (preserveDuration=true): duracion
    // fija, loop start/end intactos, deshacer restaura el tuneCents_
    // previo -- aislado en su propio Document, mismo motivo que el bloque
    // de Filtro doc de arriba.
    {
        SharedParams pparams;
        AudioDataPtr ppublished;
        Document pdoc2(pparams, [&](AudioDataPtr d) { ppublished = std::move(d); });

        auto ps2 = std::make_shared<AudioData>();
        ps2->sampleRate = 44100.0;
        ps2->format = SampleFormat::Float32;
        ps2->ch.resize(1);
        ps2->ch[0].resize(44100);
        for (int i = 0; i < 44100; ++i)
            ps2->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * 220.f * i / 44100.f);
        ps2->buildPeaks();
        pdoc2.setAudio(ps2, "Pitch doc: setup");
        pdoc2.setLoopPoints(1000, 2000, false);
        pdoc2.setTuneCents(37.f);

        pitchdsp::PitchSpec spec;
        spec.semitones = 12.f;
        auto renderStretch = [&](const AudioData& s, int64_t a, int64_t b) {
            return pitchdsp::apply(s, a, b, spec, 2048, /*preserveDuration=*/true);
        };
        pdoc2.applyPitch(renderStretch, "Pitch", /*resetTune=*/false);
        CHECK(pdoc2.frames() == 44100, "Pitch doc (stretch): no cambia la duracion (todo el sample)");
        CHECK(pdoc2.loopStart() == 1000 && pdoc2.loopEnd() == 2000,
              "Pitch doc (stretch): loop start/end quedan intactos");
        CHECK(std::fabs(pdoc2.tuneCents() - 37.f) < 1e-6f,
              "Pitch doc (stretch): sin resetTune, Tune no se toca");

        pdoc2.undo();
        CHECK(std::fabs(pdoc2.tuneCents() - 37.f) < 1e-6f,
              "Pitch doc (stretch): deshacer restaura Tune (aunque no lo hubiera tocado Apply)");
        CHECK(pdoc2.loopStart() == 1000 && pdoc2.loopEnd() == 2000,
              "Pitch doc (stretch): deshacer no altera el loop");

        // resetTune=true: Tune vuelve a 0 tras aplicar (deshacer lo restaura)
        pdoc2.applyPitch(renderStretch, "Pitch", /*resetTune=*/true);
        CHECK(std::fabs(pdoc2.tuneCents()) < 1e-6f, "Pitch doc (stretch): resetTune pone Tune a 0 tras Aplicar");
        pdoc2.undo();
        CHECK(std::fabs(pdoc2.tuneCents() - 37.f) < 1e-6f,
              "Pitch doc (stretch): deshacer tras resetTune restaura el Tune PREVIO (37 cents)");
    }

    // Document::applyPitch, modo tradicional (preserveDuration=false): la
    // duracion CAMBIA -- verifica reubicacion de marcadores (proporcional
    // dentro de la region editada, desplazada mas alla de ella) y que el
    // audio fuera de la seleccion no se toca.
    {
        SharedParams tparams;
        AudioDataPtr tpublished;
        Document tdoc(tparams, [&](AudioDataPtr d) { tpublished = std::move(d); });

        const double sr = 44100.0;
        const int64_t total = (int64_t)(sr * 3.0); // 3s
        auto ts = std::make_shared<AudioData>();
        ts->sampleRate = sr;
        ts->format = SampleFormat::Float32;
        ts->ch.resize(1);
        ts->ch[0].resize((size_t)total);
        for (int64_t i = 0; i < total; ++i)
            ts->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * 220.f * i / (float)sr);
        ts->buildPeaks();
        tdoc.setAudio(ts, "Pitch doc trad: setup");

        // seleccion = el segundo de en medio [44100, 88200); marcadores
        // antes/dentro/despues de la seleccion para verificar cada rama del
        // remapeo.
        tdoc.setSelection(44100, 88200);
        tdoc.setLoopPoints(10000, 100000, false); // start ANTES de s0, end DESPUES de s1

        pitchdsp::PitchSpec up;
        up.semitones = 12.f; // ratio=2.0 -> la seleccion (44100 muestras) se acorta a ~22050
        tdoc.applyPitch(
            [&](const AudioData& s, int64_t a, int64_t b) {
                return pitchdsp::apply(s, a, b, up, 2048, /*preserveDuration=*/false);
            },
            "Pitch", /*resetTune=*/false);

        const int64_t newTotal = tdoc.frames();
        CHECK(std::llabs((long long)newTotal - (long long)(total - 22050)) <= 2,
              "Pitch doc (tradicional): el documento se acorta exactamente lo que se acorto la seleccion");

        CHECK(tdoc.loopStart() == 10000,
              "Pitch doc (tradicional): marcador ANTES de la seleccion no se mueve");
        // loopEnd (100000, tras s1=88200) se desplaza por el mismo delta
        // que el resto del audio posterior a la seleccion.
        const int64_t delta = newTotal - total;
        CHECK(std::llabs((long long)tdoc.loopEnd() - (long long)(100000 + delta)) <= 2,
              "Pitch doc (tradicional): marcador DESPUES de la seleccion se desplaza por delta");

        // audio antes de s0: bit-identico al original (no formaba parte de
        // la seleccion editada)
        float before = tdoc.data()->ch[0][5000];
        float expectedBefore = 0.5f * sinf(2.f * (float)M_PI * 220.f * 5000.f / (float)sr);
        CHECK(std::fabs(before - expectedBefore) < 1e-5f,
              "Pitch doc (tradicional): el audio antes de la seleccion no cambia");

        // audio bien despues del nuevo limite de la seleccion editada:
        // corresponde al MISMO contenido original, solo desplazado por delta.
        const int64_t probeNew = newTotal - 1000; // cerca del final
        const int64_t probeOld = probeNew - delta;
        if (probeOld >= 0 && probeOld < total) {
            float after = tdoc.data()->ch[0][(size_t)probeNew];
            float expectedAfter = 0.5f * sinf(2.f * (float)M_PI * 220.f * (float)probeOld / (float)sr);
            CHECK(std::fabs(after - expectedAfter) < 1e-4f,
                  "Pitch doc (tradicional): el audio tras la seleccion conserva su contenido, solo desplazado");
        }

        tdoc.undo();
        CHECK(tdoc.frames() == total, "Pitch doc (tradicional): deshacer restaura la duracion original");
        CHECK(tdoc.loopStart() == 10000 && tdoc.loopEnd() == 100000,
              "Pitch doc (tradicional): deshacer restaura los marcadores originales");
    }

    // stretchdsp (procmods/stretch_dsp.hpp): cambia la duracion SIN tocar
    // el tono -- la imagen espejo de Pitch (modo tradicional). Reusa
    // stretchcore::timeStretch (mismo nucleo que el modo "preserve
    // duration" de Pitch), sin el remuestreo final que ese modo usa para
    // volver a la duracion original.
    {
        {
            stretchdsp::StretchSpec z;
            CHECK(std::fabs(stretchdsp::factor(z) - 1.0) < 1e-12,
                  "Stretch: factor() a 100% es 1.0");
            stretchdsp::StretchSpec two; two.percent = 200.f;
            CHECK(std::fabs(stretchdsp::factor(two) - 2.0) < 1e-9,
                  "Stretch: factor() a 200% es 2.0");
        }

        const double sr = 44100.0;
        const int n = (int)sr; // 1s
        auto makeTone = [&](double freq) {
            auto d = std::make_shared<AudioData>();
            d->sampleRate = sr;
            d->format = SampleFormat::Float32;
            d->ch.resize(1);
            d->ch[0].resize((size_t)n);
            for (int i = 0; i < n; ++i)
                d->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * (float)freq * i / (float)sr);
            return d;
        };
        auto wrap = [&](const std::vector<float>& v) {
            AudioData d;
            d.sampleRate = sr;
            d.format = SampleFormat::Float32;
            d.ch.resize(1);
            d.ch[0] = v;
            return d;
        };

        // 100%: passthrough exacto, sin gastar FFT
        {
            auto tone = makeTone(440.0);
            stretchdsp::StretchSpec z;
            auto out = stretchdsp::apply(*tone, 0, n, z, 2048);
            CHECK(out.size() == 1 && out[0].size() == (size_t)n,
                  "Stretch: apply a 100% devuelve la longitud pedida");
            if (out.size() == 1 && out[0].size() == (size_t)n) {
                double maxDiff = 0.0;
                for (int i = 0; i < n; ++i)
                    maxDiff = std::max(maxDiff, (double)std::fabs(out[0][(size_t)i] - tone->ch[0][(size_t)i]));
                CHECK(maxDiff < 1e-9, "Stretch: a 100% reconstruye identico a la entrada (sin gastar FFT)");
            }
        }

        // 200%: el buffer se duplica y el tono SIGUE sonando a 220Hz (a
        // diferencia de Pitch, donde cambiar la duracion cambia el tono).
        {
            auto tone = makeTone(220.0);
            stretchdsp::StretchSpec s;
            s.percent = 200.f;
            auto out = stretchdsp::apply(*tone, 0, n, s, 2048);
            CHECK(out.size() == 1, "Stretch (200%): apply no falla");
            if (out.size() == 1) {
                CHECK(std::llabs((long long)out[0].size() - 2 * n) <= 2,
                      "Stretch (200%): el buffer se duplica");
                AudioData stretched = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(stretched, (int64_t)out[0].size() / 4,
                                                              (int64_t)out[0].size() / 2);
                CHECK(period > 0, "Stretch (200%): produce una senal con periodo detectable");
                if (period > 0) {
                    double freq = sr / (double)period;
                    printf("   Stretch 200%%: 220Hz -> %.1fHz detectado (periodo %lld)\n",
                          freq, (long long)period);
                    CHECK(freq > 210.0 && freq < 230.0,
                          "Stretch (200%): el tono NO cambia (sigue ~220Hz) aunque la duracion se duplique");
                }
            }
        }

        // 50%: el buffer se reduce a la mitad, tono intacto.
        {
            auto tone = makeTone(440.0);
            stretchdsp::StretchSpec s;
            s.percent = 50.f;
            auto out = stretchdsp::apply(*tone, 0, n, s, 2048);
            CHECK(out.size() == 1, "Stretch (50%): apply no falla");
            if (out.size() == 1)
                CHECK(std::llabs((long long)out[0].size() - n / 2) <= 2,
                      "Stretch (50%): el buffer se reduce a la mitad");
        }

        // rango invalido -> vacio con gracia
        {
            auto tone = makeTone(440.0);
            stretchdsp::StretchSpec s;
            s.percent = 150.f;
            CHECK(stretchdsp::apply(*tone, 100, 100, s, 2048).empty(),
                  "Stretch: apply devuelve vacio con gracia si s1<=s0");
        }
    }

    // Document::applyStretch: reubicacion de marcadores igual que Pitch
    // (tradicional) -- reusa Document::spliceAndRemap, mismo criterio de
    // seleccion vs todo el sample. Aislado en su propio Document.
    {
        SharedParams sparams;
        AudioDataPtr spublished;
        Document sdoc(sparams, [&](AudioDataPtr d) { spublished = std::move(d); });

        const double sr = 44100.0;
        const int64_t total = (int64_t)(sr * 3.0); // 3s
        auto ss = std::make_shared<AudioData>();
        ss->sampleRate = sr;
        ss->format = SampleFormat::Float32;
        ss->ch.resize(1);
        ss->ch[0].resize((size_t)total);
        for (int64_t i = 0; i < total; ++i)
            ss->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * 220.f * i / (float)sr);
        ss->buildPeaks();
        sdoc.setAudio(ss, "Stretch doc: setup");

        sdoc.setSelection(44100, 88200); // segundo del medio
        sdoc.setLoopPoints(10000, 100000, false); // start ANTES, end DESPUES de la seleccion

        stretchdsp::StretchSpec s;
        s.percent = 200.f; // la seleccion (44100 muestras) pasa a ~88200
        sdoc.applyStretch(
            [&](const AudioData& src, int64_t a, int64_t b) {
                return stretchdsp::apply(src, a, b, s, 2048);
            },
            "Stretch");

        const int64_t newTotal = sdoc.frames();
        CHECK(std::llabs((long long)newTotal - (long long)(total + 44100)) <= 2,
              "Stretch doc: el documento crece exactamente lo que crecio la seleccion");
        CHECK(sdoc.loopStart() == 10000, "Stretch doc: marcador ANTES de la seleccion no se mueve");
        const int64_t delta = newTotal - total;
        CHECK(std::llabs((long long)sdoc.loopEnd() - (long long)(100000 + delta)) <= 2,
              "Stretch doc: marcador DESPUES de la seleccion se desplaza por delta");

        float before = sdoc.data()->ch[0][5000];
        float expectedBefore = 0.5f * sinf(2.f * (float)M_PI * 220.f * 5000.f / (float)sr);
        CHECK(std::fabs(before - expectedBefore) < 1e-5f,
              "Stretch doc: el audio antes de la seleccion no cambia");

        sdoc.undo();
        CHECK(sdoc.frames() == total, "Stretch doc: deshacer restaura la duracion original");
        CHECK(sdoc.loopStart() == 10000 && sdoc.loopEnd() == 100000,
              "Stretch doc: deshacer restaura los marcadores originales");
    }

    // rubberbandstretchdsp (procmods/rubberband_stretch_dsp.hpp): port of
    // Rubber Band Library (breakfastquay.com/rubberband) time-stretch.
    // Same shape of checks as stretchdsp above: length matches the
    // requested ratio and the tone's FREQUENCY is preserved (unlike
    // Pitch, changing duration here doesn't change pitch).
    {
        const double sr = 44100.0;
        const int n = (int)sr; // 1s
        auto makeTone = [&](double freq) {
            auto d = std::make_shared<AudioData>();
            d->sampleRate = sr;
            d->format = SampleFormat::Float32;
            d->ch.resize(1);
            d->ch[0].resize((size_t)n);
            for (int i = 0; i < n; ++i)
                d->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * (float)freq * i / (float)sr);
            return d;
        };
        auto wrap = [&](const std::vector<float>& v) {
            AudioData d;
            d.sampleRate = sr;
            d.format = SampleFormat::Float32;
            d.ch.resize(1);
            d.ch[0] = v;
            return d;
        };

        for (auto engine : {rubberbandstretchdsp::Engine::Finer, rubberbandstretchdsp::Engine::Faster}) {
            const char* engName = engine == rubberbandstretchdsp::Engine::Finer ? "Finer" : "Faster";
            auto msg = [&](const char* s) { return std::string("Rubber Band (") + engName + "): " + s; };

            auto tone = makeTone(220.0);
            auto out = rubberbandstretchdsp::apply(*tone, 0, n, 200.f, engine);
            CHECK(out.size() == 1, msg("apply (200%) no falla").c_str());
            if (out.size() == 1) {
                CHECK(std::llabs((long long)out[0].size() - 2 * n) <= (long long)(0.05 * n),
                      msg("200% aproximadamente duplica la duracion").c_str());
                AudioData stretched = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(stretched, (int64_t)out[0].size() / 4,
                                                              (int64_t)out[0].size() / 2);
                if (period > 0) {
                    double freq = sr / (double)period;
                    CHECK(freq > 200.0 && freq < 240.0,
                          msg("el tono NO cambia (sigue ~220Hz) aunque la duracion cambie").c_str());
                }
            }

            auto tone50 = makeTone(440.0);
            auto out50 = rubberbandstretchdsp::apply(*tone50, 0, n, 50.f, engine);
            CHECK(out50.size() == 1 &&
                      std::llabs((long long)out50[0].size() - n / 2) <= (long long)(0.05 * n),
                  msg("50% aproximadamente reduce a la mitad").c_str());
        }

        // rango invalido -> vacio con gracia
        auto tone = makeTone(440.0);
        CHECK(rubberbandstretchdsp::apply(*tone, 100, 100, 150.f, rubberbandstretchdsp::Engine::Finer)
                  .empty(),
              "Rubber Band: apply devuelve vacio con gracia si s1<=s0");
    }

    // signalsmithstretchdsp (procmods/signalsmith_stretch_dsp.hpp): port of
    // Signalsmith Stretch (signalsmith-audio.co.uk/code/stretch).
    {
        const double sr = 44100.0;
        const int n = (int)sr; // 1s -- comfortably longer than the
                                // library's default ~120ms analysis block
        auto makeTone = [&](double freq) {
            auto d = std::make_shared<AudioData>();
            d->sampleRate = sr;
            d->format = SampleFormat::Float32;
            d->ch.resize(1);
            d->ch[0].resize((size_t)n);
            for (int i = 0; i < n; ++i)
                d->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * (float)freq * i / (float)sr);
            return d;
        };
        auto wrap = [&](const std::vector<float>& v) {
            AudioData d;
            d.sampleRate = sr;
            d.format = SampleFormat::Float32;
            d.ch.resize(1);
            d.ch[0] = v;
            return d;
        };

        for (bool cheaper : {false, true}) {
            const char* presetName = cheaper ? "cheaper" : "default";
            auto msg = [&](const char* s) { return std::string("Signalsmith (") + presetName + "): " + s; };

            auto tone = makeTone(220.0);
            auto out = signalsmithstretchdsp::apply(*tone, 0, n, 200.f, cheaper);
            CHECK(out.size() == 1 && out[0].size() == (size_t)(2 * n),
                  msg("apply (200%) da exactamente 2x muestras").c_str());
            if (out.size() == 1) {
                AudioData stretched = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(stretched, (int64_t)out[0].size() / 4,
                                                              (int64_t)out[0].size() / 2);
                if (period > 0) {
                    double freq = sr / (double)period;
                    CHECK(freq > 200.0 && freq < 240.0,
                          msg("el tono NO cambia (sigue ~220Hz) aunque la duracion cambie").c_str());
                }
            }

            auto tone50 = makeTone(440.0);
            auto out50 = signalsmithstretchdsp::apply(*tone50, 0, n, 50.f, cheaper);
            CHECK(out50.size() == 1 && out50[0].size() == (size_t)(n / 2),
                  msg("apply (50%) da exactamente la mitad").c_str());
        }

        // rango invalido -> vacio con gracia
        {
            auto tone = makeTone(440.0);
            CHECK(signalsmithstretchdsp::apply(*tone, 100, 100, 150.f, false).empty(),
                  "Signalsmith: apply devuelve vacio con gracia si s1<=s0");
        }

        // region demasiado corta para el analisis -> vacio con gracia, sin crashear
        {
            auto tiny = makeTone(440.0);
            auto outTiny = signalsmithstretchdsp::apply(*tiny, 0, 50, 150.f, false);
            CHECK(outTiny.empty(),
                  "Signalsmith: region demasiado corta para un bloque de analisis falla con gracia");
        }
    }

    // lorisdsp (procmods/loris_dsp.hpp): transpone via resintesis aditiva
    // (third_party/loris-2.0: Analyzer -> Channelizer -> Distiller ->
    // PartialUtils::shiftPitch -> Synthesizer) en vez de vocoder de fase.
    // SIEMPRE fuerza la salida a exactamente (s1-s0) muestras por canal
    // (zero-pad/truncate) -- a diferencia de Pitch/Stretch, no hay
    // passthrough bit-exacto ni siquiera a 0 semitonos/0 cents, porque
    // reconstruye desde los partials trackeados.
    {
        {
            lorisdsp::LorisSpec z;
            CHECK(std::fabs(lorisdsp::totalCents(z)) < 1e-9,
                  "Loris: totalCents() en 0 semitonos/0 cents es 0");
            lorisdsp::LorisSpec up;
            up.semitones = 12.f;
            up.cents = 50.f;
            CHECK(std::fabs(lorisdsp::totalCents(up) - 1250.0) < 1e-9,
                  "Loris: totalCents() = semitones*100 + cents");
        }

        const double sr = 44100.0;
        const int n = (int)sr; // 1s
        auto makeTone = [&](double freq) {
            auto d = std::make_shared<AudioData>();
            d->sampleRate = sr;
            d->format = SampleFormat::Float32;
            d->ch.resize(1);
            d->ch[0].resize((size_t)n);
            for (int i = 0; i < n; ++i)
                d->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * (float)freq * i / (float)sr);
            return d;
        };
        auto wrap = [&](const std::vector<float>& v) {
            AudioData d;
            d.sampleRate = sr;
            d.format = SampleFormat::Float32;
            d.ch.resize(1);
            d.ch[0] = v;
            return d;
        };

        // Sin transponer: el tono resintetizado sigue sonando a ~220Hz.
        {
            auto tone = makeTone(220.0);
            lorisdsp::LorisSpec z;
            z.refFreqHz = 220.0;
            auto out = lorisdsp::apply(*tone, 0, n, z);
            CHECK(out.size() == 1 && out[0].size() == (size_t)n,
                  "Loris: apply devuelve exactamente la longitud pedida");
            if (out.size() == 1 && out[0].size() == (size_t)n) {
                AudioData resynth = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(resynth, n / 4, n / 2);
                CHECK(period > 0, "Loris (sin transponer): produce una senal con periodo detectable");
                if (period > 0) {
                    double freq = sr / (double)period;
                    printf("   Loris sin transponer: 220Hz -> %.1fHz detectado (periodo %lld)\n",
                          freq, (long long)period);
                    CHECK(freq > 210.0 && freq < 230.0,
                          "Loris (sin transponer): el tono resintetizado sigue ~220Hz");
                }
            }
        }

        // +12 semitonos: sube una octava (~440Hz), sin cambiar la duracion.
        {
            auto tone = makeTone(220.0);
            lorisdsp::LorisSpec up;
            up.refFreqHz = 220.0;
            up.semitones = 12.f;
            auto out = lorisdsp::apply(*tone, 0, n, up);
            CHECK(out.size() == 1 && out[0].size() == (size_t)n,
                  "Loris (+12 semitonos): la duracion del buffer no cambia");
            if (out.size() == 1 && out[0].size() == (size_t)n) {
                AudioData resynth = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(resynth, n / 4, n / 2);
                CHECK(period > 0, "Loris (+12 semitonos): produce una senal con periodo detectable");
                if (period > 0) {
                    double freq = sr / (double)period;
                    printf("   Loris +12 semitonos: 220Hz -> %.1fHz detectado (periodo %lld)\n",
                          freq, (long long)period);
                    CHECK(freq > 420.0 && freq < 460.0,
                          "Loris (+12 semitonos): el tono sube una octava (~440Hz)");
                }
            }
        }

        // -12 semitonos: baja una octava (~110Hz).
        {
            auto tone = makeTone(220.0);
            lorisdsp::LorisSpec down;
            down.refFreqHz = 220.0;
            down.semitones = -12.f;
            auto out = lorisdsp::apply(*tone, 0, n, down);
            CHECK(out.size() == 1 && out[0].size() == (size_t)n,
                  "Loris (-12 semitonos): la duracion del buffer no cambia");
            if (out.size() == 1 && out[0].size() == (size_t)n) {
                AudioData resynth = wrap(out[0]);
                int64_t period = granulardsp::estimatePeriod(resynth, n / 4, n / 2);
                CHECK(period > 0, "Loris (-12 semitonos): produce una senal con periodo detectable");
                if (period > 0) {
                    double freq = sr / (double)period;
                    printf("   Loris -12 semitonos: 220Hz -> %.1fHz detectado (periodo %lld)\n",
                          freq, (long long)period);
                    CHECK(freq > 100.0 && freq < 120.0,
                          "Loris (-12 semitonos): el tono baja una octava (~110Hz)");
                }
            }
        }

        // rango invalido o refFreqHz<=0 -> vacio con gracia
        {
            auto tone = makeTone(220.0);
            lorisdsp::LorisSpec s;
            s.refFreqHz = 220.0;
            CHECK(lorisdsp::apply(*tone, 100, 100, s).empty(),
                  "Loris: apply devuelve vacio con gracia si s1<=s0");
            lorisdsp::LorisSpec badRef;
            badRef.refFreqHz = 0.0;
            CHECK(lorisdsp::apply(*tone, 0, n, badRef).empty(),
                  "Loris: apply devuelve vacio con gracia si refFreqHz<=0");
        }
    }

    // Document::applyPitch con Loris (procmods/pitch_loris.hpp, PitchLoris::
    // render): duracion FIJA (misma longitud que la region editada, a
    // diferencia de applyPitch en modo tradicional/applyStretch) -- no hace
    // falta remapeo de marcadores, mismo criterio que applyFilter/applyPitch
    // en modo preserveDuration. resetTune se comporta igual que con
    // cualquier otro metodo de Pitch (Loris ya no tiene su propio
    // Document::applyLoris -- ver PitchMethod::draw, comparte el mismo
    // Apply/Apply-and-reset-tune que PitchBuiltin/PitchSignalsmith).
    {
        SharedParams lparams;
        AudioDataPtr lpublished;
        Document ldoc(lparams, [&](AudioDataPtr d) { lpublished = std::move(d); });

        auto ls2 = std::make_shared<AudioData>();
        ls2->sampleRate = 44100.0;
        ls2->format = SampleFormat::Float32;
        ls2->ch.resize(1);
        ls2->ch[0].resize(44100);
        for (int i = 0; i < 44100; ++i)
            ls2->ch[0][(size_t)i] = 0.5f * sinf(2.f * (float)M_PI * 220.f * i / 44100.f);
        ls2->buildPeaks();
        ldoc.setAudio(ls2, "Loris doc: setup");
        ldoc.setLoopPoints(1000, 2000, false);
        ldoc.setTuneCents(37.f);

        lorisdsp::LorisSpec spec;
        spec.refFreqHz = 220.0;
        spec.semitones = 12.f;
        auto lorisRender = [spec](const AudioData& s, int64_t a, int64_t b) {
            return lorisdsp::apply(s, a, b, spec);
        };
        ldoc.applyPitch(lorisRender, "Loris", /*resetTune=*/false);
        CHECK(ldoc.frames() == 44100, "Loris doc: no cambia la duracion (todo el sample)");
        CHECK(ldoc.loopStart() == 1000 && ldoc.loopEnd() == 2000,
              "Loris doc: loop start/end quedan intactos");
        CHECK(std::fabs(ldoc.tuneCents() - 37.f) < 1e-6f,
              "Loris doc: sin resetTune, Tune no se toca");

        ldoc.undo();
        CHECK(std::fabs(ldoc.tuneCents() - 37.f) < 1e-6f,
              "Loris doc: deshacer restaura Tune (aunque no lo hubiera tocado Apply)");
        CHECK(ldoc.loopStart() == 1000 && ldoc.loopEnd() == 2000,
              "Loris doc: deshacer no altera el loop");

        // resetTune=true: Tune vuelve a 0 tras aplicar (deshacer lo restaura)
        ldoc.applyPitch(lorisRender, "Loris", /*resetTune=*/true);
        CHECK(std::fabs(ldoc.tuneCents()) < 1e-6f, "Loris doc: resetTune pone Tune a 0 tras Aplicar");
        ldoc.undo();
        CHECK(std::fabs(ldoc.tuneCents() - 37.f) < 1e-6f,
              "Loris doc: deshacer tras resetTune restaura el Tune PREVIO (37 cents)");

        // con seleccion: solo esa parte se resintetiza, el resto queda intacto
        ldoc.setSelection(10000, 30000);
        float beforeSel = ldoc.data()->ch[0][5000];
        ldoc.applyPitch(lorisRender, "Loris", false);
        CHECK(ldoc.frames() == 44100, "Loris doc (seleccion): no cambia la duracion");
        float afterSel = ldoc.data()->ch[0][5000];
        CHECK(std::fabs(afterSel - beforeSel) < 1e-9f,
              "Loris doc (seleccion): el audio FUERA de la seleccion no se toca");
    }

    // Document::extendLoop: duplica el loop marcado y lo inserta justo
    // despues, extendiendo loopEnd -- un loop de 1 ciclo pasa a ser de 2.
    {
        SharedParams eparams;
        AudioDataPtr epublished;
        Document edoc(eparams, [&](AudioDataPtr d) { epublished = std::move(d); });

        const double sr = 44100.0;
        const int64_t total = 10000;
        auto es = std::make_shared<AudioData>();
        es->sampleRate = sr;
        es->format = SampleFormat::Float32;
        es->ch.resize(1);
        es->ch[0].resize((size_t)total);
        // Rampa: cada muestra tiene un valor unico -> facil verificar que
        // el contenido copiado/desplazado es exactamente el esperado.
        for (int64_t i = 0; i < total; ++i) es->ch[0][(size_t)i] = (float)i;
        es->buildPeaks();
        edoc.setAudio(es, "Extend loop doc: setup");

        edoc.extendLoop();
        CHECK(edoc.frames() == total, "Extend loop: sin loop marcado, no hace nada");

        edoc.setLoopPoints(2000, 3000, false); // loop de 1000 muestras
        edoc.setSelection(3500, 4000);         // despues de loopEnd: debe desplazarse tambien

        edoc.extendLoop();
        CHECK(edoc.frames() == total + 1000,
              "Extend loop: el documento crece exactamente el largo del loop");
        CHECK(edoc.loopStart() == 2000, "Extend loop: loopStart no se mueve");
        CHECK(edoc.loopEnd() == 4000,
              "Extend loop: loopEnd se extiende al final de la copia (3000+1000)");
        CHECK(edoc.selStart() == 4500 && edoc.selEnd() == 5000,
              "Extend loop: la seleccion (estaba despues del loop) se desplaza por el largo insertado");

        bool copyOk = true;
        for (int64_t i = 0; i < 1000; ++i)
            if (edoc.data()->ch[0][(size_t)(3000 + i)] != (float)(2000 + i)) { copyOk = false; break; }
        CHECK(copyOk, "Extend loop: el contenido duplicado es identico al de la zona del loop original");

        bool tailOk = true;
        for (int64_t i = 4000; i < edoc.frames(); ++i)
            if (edoc.data()->ch[0][(size_t)i] != (float)(i - 1000)) { tailOk = false; break; }
        CHECK(tailOk, "Extend loop: el audio tras la copia conserva el contenido original, solo desplazado");

        edoc.undo();
        CHECK(edoc.frames() == total, "Extend loop: deshacer restaura la duracion original");
        CHECK(edoc.loopStart() == 2000 && edoc.loopEnd() == 3000,
              "Extend loop: deshacer restaura el loop original");
        // La seleccion NO es parte del snapshot de undo (ver Document::
        // Snapshot): es estado efimero de UI, igual que en el resto de los
        // tests de este archivo, por eso no se verifica que "vuelva".
    }

    // Document::trimSilence: elimina silencio (o casi-silencio, segun
    // thresholdDb) del inicio y del final, dejando solo el contenido.
    {
        SharedParams tsparams;
        AudioDataPtr tspublished;
        Document tsdoc(tsparams, [&](AudioDataPtr d) { tspublished = std::move(d); });

        const double sr = 44100.0;
        const int64_t headSilence = 500;
        const int64_t bodyLen = 2000;
        const int64_t tailSilence = 300;
        const int64_t total = headSilence + bodyLen + tailSilence;
        // Mesetas planas (en vez de un tono) para que el limite silencio/
        // contenido sea exacto y sin ambiguedad por cruces por cero.
        auto build = [&]() {
            auto d = std::make_shared<AudioData>();
            d->sampleRate = sr;
            d->format = SampleFormat::Float32;
            d->ch.resize(1);
            d->ch[0].resize((size_t)total, 0.f);
            for (int64_t i = 0; i < bodyLen; ++i) d->ch[0][(size_t)(headSilence + i)] = 0.5f;
            d->buildPeaks();
            return d;
        };

        tsdoc.setAudio(build(), "Trim silence doc: setup");
        tsdoc.setLoopPoints(headSilence + 100, headSilence + 200, false); // dentro del cuerpo

        tsdoc.trimSilence(-100.f); // umbral estricto: solo silencio EXACTO
        CHECK(tsdoc.frames() == bodyLen,
              "Trim silence (-100dB): recorta exactamente el silencio exacto de cabeza y cola");
        CHECK(tsdoc.loopStart() == 100 && tsdoc.loopEnd() == 200,
              "Trim silence: el loop (dentro del cuerpo) se desplaza hacia atras por 'head'");
        CHECK(tsdoc.data()->ch[0][0] == 0.5f && tsdoc.data()->ch[0][(size_t)(bodyLen - 1)] == 0.5f,
              "Trim silence: el contenido conservado es exactamente el cuerpo (sin recortar de mas)");

        tsdoc.undo();
        CHECK(tsdoc.frames() == total, "Trim silence: deshacer restaura la duracion original");
        CHECK(tsdoc.loopStart() == headSilence + 100 && tsdoc.loopEnd() == headSilence + 200,
              "Trim silence: deshacer restaura el loop original");

        // sample totalmente silencioso -> no-op
        {
            SharedParams zparams;
            AudioDataPtr zpublished;
            Document zdoc(zparams, [&](AudioDataPtr d) { zpublished = std::move(d); });
            auto zs = std::make_shared<AudioData>();
            zs->sampleRate = sr;
            zs->format = SampleFormat::Float32;
            zs->ch.resize(1);
            zs->ch[0].resize(1000, 0.f);
            zs->buildPeaks();
            zdoc.setAudio(zs, "Trim silence (todo silencio): setup");
            zdoc.trimSilence(-90.f);
            CHECK(zdoc.frames() == 1000, "Trim silence: sample totalmente silencioso no cambia (no-op)");
        }

        // sin silencio en los bordes -> no-op
        {
            SharedParams lparams2;
            AudioDataPtr lpublished2;
            Document ldoc2(lparams2, [&](AudioDataPtr d) { lpublished2 = std::move(d); });
            auto lsAll = std::make_shared<AudioData>();
            lsAll->sampleRate = sr;
            lsAll->format = SampleFormat::Float32;
            lsAll->ch.resize(1);
            lsAll->ch[0].assign(1000, 0.5f);
            lsAll->buildPeaks();
            ldoc2.setAudio(lsAll, "Trim silence (sin bordes): setup");
            ldoc2.trimSilence(-90.f);
            CHECK(ldoc2.frames() == 1000, "Trim silence: sin silencio en los bordes, no cambia (no-op)");
        }

        // marcador ENTERO dentro de la region de silencio removida -> se
        // invalida (mismo criterio de clampMarkersToLength que cualquier
        // otra edicion: un marcador que queda con start<0 se descarta).
        {
            SharedParams mparams;
            AudioDataPtr mpublished;
            Document mdoc(mparams, [&](AudioDataPtr d) { mpublished = std::move(d); });
            mdoc.setAudio(build(), "Trim silence (marcador en silencio): setup");
            mdoc.setLoopPoints(100, 300, false); // dentro de la cabeza silenciosa (headSilence=500)
            mdoc.trimSilence(-100.f);
            CHECK(!mdoc.hasLoop(),
                  "Trim silence: un loop que caia enteramente en el silencio removido se invalida");
        }
    }

    // umbral mas laxo (-20dB): tambien recorta una zona de amplitud baja
    // (no exactamente 0) que -100dB (solo silencio exacto) deja intacta.
    {
        const double sr = 44100.0;
        const int64_t quietLen = 200;
        const int64_t loudLen = 1000;
        auto build = [&]() {
            auto d = std::make_shared<AudioData>();
            d->sampleRate = sr;
            d->format = SampleFormat::Float32;
            d->ch.resize(1);
            d->ch[0].assign((size_t)quietLen, 0.05f); // ~-26dB: por debajo de -20dB, por encima de -100dB
            d->ch[0].resize((size_t)(quietLen + loudLen), 0.5f);
            d->buildPeaks();
            return d;
        };

        // Dos documentos aislados (en vez de deshacer entre pasos): el
        // primer trimSilence es un no-op y no empuja nada al stack de
        // undo, asi que un undo() ahi deshacia el setAudio() anterior, no
        // el trimSilence -- mismo motivo por el que el resto de los tests
        // de este archivo aislan cada escenario en su propio Document.
        {
            SharedParams qparams;
            AudioDataPtr qpublished;
            Document qdoc(qparams, [&](AudioDataPtr d) { qpublished = std::move(d); });
            qdoc.setAudio(build(), "Trim silence quiet doc (-100dB): setup");
            qdoc.trimSilence(-100.f);
            CHECK(qdoc.frames() == quietLen + loudLen,
                  "Trim silence (-100dB): una zona de amplitud baja (no exactamente 0) NO se recorta");
        }
        {
            SharedParams qparams2;
            AudioDataPtr qpublished2;
            Document qdoc2(qparams2, [&](AudioDataPtr d) { qpublished2 = std::move(d); });
            qdoc2.setAudio(build(), "Trim silence quiet doc (-20dB): setup");
            qdoc2.trimSilence(-20.f);
            CHECK(qdoc2.frames() == loudLen,
                  "Trim silence (-20dB): la misma zona de amplitud baja SI se recorta con un umbral mas laxo");
        }
    }

    // a mono (mezcla)
    doc.toMono(2);
    CHECK(doc.data()->channels() == 1, "conversion a mono");
    doc.undo();
    CHECK(doc.data()->channels() == 2, "deshacer vuelve a estereo");

    // resampler HQ: seno 1 kHz 44100 -> 48000, error frente al analitico
    {
        std::vector<std::vector<float>> in(1);
        in[0].resize(44100);
        for (int i = 0; i < 44100; ++i)
            in[0][i] = 0.4f * sinf(2.f * (float)M_PI * 1000.f * i / 44100.f);
        auto out = resampleHQ(in, 44100, 48000);
        CHECK(std::llabs((long long)out[0].size() - 48000) <= 1,
              "resample: longitud 48000 +-1");
        double maxErr = 0;
        for (size_t k = 400; k + 400 < out[0].size(); ++k) {
            double ref = 0.4 * sin(2.0 * M_PI * 1000.0 * (double)k / 48000.0);
            maxErr = std::max(maxErr, std::fabs(ref - (double)out[0][k]));
        }
        printf("   error max resample: %.2e\n", maxErr);
        CHECK(maxErr < 5e-4, "resample HQ: error < 5e-4 (>66 dB)");
    }

    // interpolador sinc16 (motor RT): particion de la unidad + reconstruccion
    {
        SincInterp16 sinc;
        float maxUnitySum = 0.f;
        for (int s = 0; s <= 20; ++s) {
            float t = s / 20.f;
            float w[SincInterp16::kTaps];
            sinc.weights(t, w);
            float sum = 0.f;
            for (float v : w) sum += v;
            maxUnitySum = std::max(maxUnitySum, std::fabs(sum - 1.f));
        }
        printf("   max desviacion particion-unidad: %.2e\n", maxUnitySum);
        CHECK(maxUnitySum < 0.02f,
              "sinc16: particion de la unidad (ganancia unitaria)");

        // reconstruye un tono de 5 kHz a 44100 Hz en posiciones fraccionarias
        // arbitrarias (simula reproduccion con pitch) y compara con el valor
        // analitico exacto.
        const int n = 4410;
        std::vector<float> buf(n);
        const double freq = 5000.0, sr = 44100.0;
        for (int i = 0; i < n; ++i)
            buf[i] = (float)(0.7 * sin(2.0 * M_PI * freq * i / sr));
        double maxErr = 0;
        for (int i = 20; i < n - 20; ++i) {
            double pos = i + 0.37; // desplazamiento fraccionario fijo
            int64_t i0 = (int64_t)pos;
            float t = (float)(pos - (double)i0);
            float w[SincInterp16::kTaps];
            sinc.weights(t, w);
            float acc = 0.f;
            for (int k = 0; k < SincInterp16::kTaps; ++k) {
                int64_t idx = i0 + (k - (SincInterp16::kHalf - 1));
                acc += w[k] * buf[(size_t)idx];
            }
            double ref = 0.7 * sin(2.0 * M_PI * freq * pos / sr);
            maxErr = std::max(maxErr, std::fabs(ref - (double)acc));
        }
        printf("   error max sinc16 @5kHz/44100: %.2e\n", maxErr);
        CHECK(maxErr < 5e-3, "sinc16: reconstruccion fiel de un tono agudo");
    }

    // motor RT: el kernel sinc16 no debe morder audio de fuera del loop al
    // envolver, ni siquiera cuando el sample esta "cortado justo en el end
    // loop" (loopEnd == frames(), el peor caso: no hay cola real tras el
    // loop de la que "sangrar" contenido). Reproduce con un ratio
    // fraccionario real (host a 48 kHz, documento a 44.1 kHz) para ejercitar
    // de verdad los 16 taps del interpolador, no solo posiciones enteras.
    {
        Engine engine;
        engine.activate(48000.0);
        auto ad = std::make_shared<AudioData>();
        ad->sampleRate = 44100.0;
        ad->format = SampleFormat::Float32;
        ad->ch.resize(2);
        const int64_t loopFrames = 2000;
        ad->ch[0].assign((size_t)loopFrames, 0.3f);
        ad->ch[1].assign((size_t)loopFrames, 0.9f); // R con mas amplitud: revela mejor el sangrado
        ad->buildPeaks();
        engine.publishDocument(ad);
        engine.params.loopStart.store(0);
        engine.params.loopEnd.store(loopFrames); // cortado justo en el end loop
        engine.params.loopEnabled.store(true);
        engine.seek(0.0);
        engine.play();

        const uint32_t block = 64;
        float outL[block], outR[block];
        float maxErrL = 0.f, maxErrR = 0.f;
        const int totalBlocks = 140; // ~9000 muestras de host, varias vueltas del loop
        for (int b = 0; b < totalBlocks; ++b) {
            for (uint32_t i = 0; i < block; ++i) { outL[i] = 0.f; outR[i] = 0.f; }
            engine.process(nullptr, nullptr, outL, outR, block);
            if (b * (int)block > 1000) { // salta la rampa anti-click inicial (~3 ms)
                for (uint32_t i = 0; i < block; ++i) {
                    maxErrL = std::max(maxErrL, std::fabs(outL[i] - 0.3f));
                    maxErrR = std::max(maxErrR, std::fabs(outR[i] - 0.9f));
                }
            }
        }
        printf("   error max motor RT en el wrap del loop: L=%.4f R=%.4f\n",
               maxErrL, maxErrR);
        CHECK(maxErrL < 0.01f && maxErrR < 0.01f,
              "motor RT: sin click al envolver un loop cortado justo en el end loop");
    }

    // motor RT: el envolvimiento de taps NO debe contaminar el ataque previo
    // al loop start (la parte que se toca una sola vez antes de entrar al
    // bucle). Es el riesgo de regresion directo del fix anterior: si se
    // envolviera siempre que un tap cae fuera de [ls,le), el ataque sonaria
    // a contenido del loop desde el primer instante.
    {
        Engine engine;
        engine.activate(48000.0);
        auto ad = std::make_shared<AudioData>();
        ad->sampleRate = 44100.0;
        ad->format = SampleFormat::Float32;
        ad->ch.resize(2);
        // Ataque largo a proposito: la rampa anti-click (~3 ms, ~700-1000
        // muestras de host para asentarse del todo) necesita margen de sobra
        // antes Y despues sin acercarse al loop start ni al wrap.
        const int64_t attackFrames = 5000, loopStart = 5000, totalFrames = 8000;
        ad->ch[0].assign((size_t)totalFrames, 0.f);
        ad->ch[1].assign((size_t)totalFrames, 0.f);
        for (int64_t i = 0; i < attackFrames; ++i) {
            ad->ch[0][(size_t)i] = 0.1f;
            ad->ch[1][(size_t)i] = 0.15f;
        }
        for (int64_t i = loopStart; i < totalFrames; ++i) {
            ad->ch[0][(size_t)i] = 0.3f;
            ad->ch[1][(size_t)i] = 0.9f;
        }
        ad->buildPeaks();
        engine.publishDocument(ad);
        engine.params.loopStart.store(loopStart);
        engine.params.loopEnd.store(totalFrames);
        engine.params.loopEnabled.store(true);
        engine.seek(0.0); // arranca en el ataque, ANTES del loop start
        engine.play();

        const uint32_t block = 64;
        float outL[block], outR[block];
        float maxErrAttack = 0.f, maxErrLoop = 0.f;
        // ratio ~0.91875: el ataque (5000 muestras) dura ~5443 muestras de
        // host y el loop entero (le=8000) envuelve recien en ~8710.
        for (int b = 0; b < 130; ++b) {
            for (uint32_t i = 0; i < block; ++i) { outL[i] = 0.f; outR[i] = 0.f; }
            engine.process(nullptr, nullptr, outL, outR, block);
            int64_t sampleIdx = (int64_t)b * block;
            if (sampleIdx > 1500 && sampleIdx + (int64_t)block < 5000) {
                // bien dentro del ataque: rampa asentada, lejos del loop start
                for (uint32_t i = 0; i < block; ++i)
                    maxErrAttack = std::max({maxErrAttack, std::fabs(outL[i] - 0.1f),
                                             std::fabs(outR[i] - 0.15f)});
            }
            if (sampleIdx > 6000 && sampleIdx + (int64_t)block < 8300) {
                // ya bien adentro del loop, sin llegar al wrap (~8710)
                for (uint32_t i = 0; i < block; ++i)
                    maxErrLoop = std::max({maxErrLoop, std::fabs(outL[i] - 0.3f),
                                           std::fabs(outR[i] - 0.9f)});
            }
        }
        printf("   error max ataque: %.4f | error max loop: %.4f\n", maxErrAttack,
               maxErrLoop);
        CHECK(maxErrAttack < 0.01f,
              "motor RT: el ataque previo al loop start no se contamina con el loop");
        CHECK(maxErrLoop < 0.01f,
              "motor RT: el contenido del loop se reproduce correctamente tras el ataque");
    }

    // motor RT: Engine::publishPreview con loopOffset > 0 (usado por
    // Harmonic para previsualizar ataque fundido + loop puro) tiene que
    // reproducir todo lo anterior al offset UNA SOLA VEZ y despues repetir
    // [offset, frames()) para siempre -- mismo mecanismo que un loop
    // marcado en reproduccion normal, reutilizado en modo preview. Esto es
    // justo lo que evita el "fade out" que aparecia al suavizar a mano el
    // wrap del preview (ver ronda anterior en memoria del proyecto).
    {
        Engine engine;
        engine.activate(44100.0); // = sampleRate del buffer -> ratio 1:1, sin pitch
        auto prev = std::make_shared<AudioData>();
        prev->sampleRate = 44100.0;
        prev->format = SampleFormat::Float32;
        prev->ch.resize(2);
        const int64_t offset = 2000, total = 5000;
        prev->ch[0].assign((size_t)total, 0.f);
        prev->ch[1].assign((size_t)total, 0.f);
        for (int64_t i = 0; i < offset; ++i) {
            prev->ch[0][(size_t)i] = 0.2f; // "ataque": suena una sola vez
            prev->ch[1][(size_t)i] = 0.25f;
        }
        for (int64_t i = offset; i < total; ++i) {
            prev->ch[0][(size_t)i] = 0.6f; // "loop puro": se repite para siempre
            prev->ch[1][(size_t)i] = 0.8f;
        }
        prev->buildPeaks();
        engine.publishPreview(prev, offset);
        engine.play();

        const uint32_t block = 64;
        float outL[block], outR[block];
        float maxErrAttack = 0.f, maxErrLoopFirstPass = 0.f, maxErrLoopAfterWrap = 0.f;
        for (int b = 0; b < 250; ++b) { // ~16000 muestras: varias vueltas del loop (3000 c/u)
            for (uint32_t i = 0; i < block; ++i) { outL[i] = 0.f; outR[i] = 0.f; }
            engine.process(nullptr, nullptr, outL, outR, block);
            int64_t sampleIdx = (int64_t)b * block;
            if (sampleIdx > 500 && sampleIdx + (int64_t)block < offset)
                for (uint32_t i = 0; i < block; ++i)
                    maxErrAttack = std::max({maxErrAttack, std::fabs(outL[i] - 0.2f),
                                             std::fabs(outR[i] - 0.25f)});
            if (sampleIdx > offset && sampleIdx + (int64_t)block < total)
                for (uint32_t i = 0; i < block; ++i)
                    maxErrLoopFirstPass = std::max({maxErrLoopFirstPass, std::fabs(outL[i] - 0.6f),
                                                    std::fabs(outR[i] - 0.8f)});
            if (sampleIdx > total) // ya envolvio al menos una vez dentro de [offset,total)
                for (uint32_t i = 0; i < block; ++i)
                    maxErrLoopAfterWrap = std::max({maxErrLoopAfterWrap, std::fabs(outL[i] - 0.6f),
                                                    std::fabs(outR[i] - 0.8f)});
        }
        CHECK(maxErrAttack < 0.01f,
              "preview con offset: el ataque (antes de offset) suena tal cual una vez");
        CHECK(maxErrLoopFirstPass < 0.01f,
              "preview con offset: el tramo [offset,frames()) ya suena a loop en la primera pasada");
        CHECK(maxErrLoopAfterWrap < 0.01f,
              "preview con offset: tras envolver, se repite [offset,frames()) para siempre "
              "(nunca vuelve a sonar el ataque)");
    }

    // export WAV + smpl y recarga
    {
        doc.setLoopPoints(100, 40000, false);
        doc.setRootNote(69);
        doc.setTuneCents(-25.f);
        std::string path = tmpdir + "/neolooper_test.wav";
        std::string err;
        bool ok = exportWavSmpl(path, *doc.data(), 32, doc.loopStart(),
                                doc.loopEnd(), doc.rootNote(),
                                doc.tuneCents(), err);
        CHECK(ok, "exportar WAV");
        LoadResult r = loadAudioFile(path);
        CHECK(r.data != nullptr, "recargar WAV exportado");
        if (r.data) {
            CHECK(r.data->frames() == doc.frames(), "roundtrip: longitud");
            CHECK(r.data->channels() == 2, "roundtrip: canales");
            CHECK(r.hasInstrument, "roundtrip: chunk smpl presente");
            CHECK(r.loopStart == 100 && r.loopEnd == 40000,
                  "roundtrip: puntos de loop");
            // -25 cents viaja como unity 68 + 75 cents y se reconstruye
            CHECK(r.rootNote == 69 && std::fabs(r.tuneCents + 25.f) < 0.5f,
                  "roundtrip: root note y finetune reconstruidos");
            double maxErr = 0;
            for (int i = 0; i < 2000; ++i)
                maxErr = std::max(maxErr,
                                  (double)std::fabs(r.data->ch[0][i] -
                                                    doc.data()->ch[0][i]));
            CHECK(maxErr < 1e-6, "roundtrip: datos identicos (float32)");
        }

        // La profundidad la elige el usuario al exportar (no el formato del
        // documento): 16 y 24 bits salen como PCM entero -- lo que Polyphone
        // y compania SI leen, a diferencia del WAV de 32-bit float -- con el
        // error de cuantizacion correspondiente y el mismo chunk smpl.
        for (int bits : {16, 24}) {
            std::string bpath = tmpdir + "/neolooper_test" + std::to_string(bits) + ".wav";
            CHECK(exportWavSmpl(bpath, *doc.data(), bits, doc.loopStart(),
                                doc.loopEnd(), doc.rootNote(), doc.tuneCents(), err),
                  bits == 16 ? "exportar WAV 16-bit PCM" : "exportar WAV 24-bit PCM");
            LoadResult rb = loadAudioFile(bpath);
            CHECK(rb.data != nullptr && rb.data->frames() == doc.frames(),
                  bits == 16 ? "16-bit: recarga con la longitud correcta"
                             : "24-bit: recarga con la longitud correcta");
            if (rb.data) {
                CHECK(rb.hasInstrument && rb.loopStart == 100 && rb.loopEnd == 40000,
                      bits == 16 ? "16-bit: chunk smpl intacto" : "24-bit: chunk smpl intacto");
                // Referencia: exactamente lo que debe producir el exportador
                // (si el pico del doc supera el techo del formato -- puede
                // traer muestras fuera de [-1,1), promovidas a 32 bits en
                // tests anteriores -- TODO el archivo se escala para que el
                // pico caiga justo en el techo, no se recorta muestra a
                // muestra; ver el test dedicado mas abajo que lo distingue
                // claramente de un recorte).
                const double intScale = bits == 16 ? 32768.0 : 8388608.0;
                const float ceiling = bits == 16 ? kInt16Max : kInt24Max;
                const float peak = doc.data()->peakAbs();
                // float precision throughout, matching wav_io.cpp's
                // `s * scale` step exactly (only the final lround widens
                // to double), so this reference isn't off by float/double
                // rounding at the 24-bit quantization step (~1.2e-7).
                const float preScale = peak > ceiling ? ceiling / peak : 1.0f;
                double maxErr = 0;
                for (int i = 0; i < 2000; ++i) {
                    float sScaled = doc.data()->ch[0][i] * preScale;
                    double v = std::llround((double)sScaled * intScale);
                    v = std::min(intScale - 1.0, std::max(-intScale, v));
                    maxErr = std::max(maxErr, std::fabs(rb.data->ch[0][i] - v / intScale));
                }
                CHECK(maxErr < 1e-6,
                      bits == 16 ? "16-bit: datos exactos (escalados si hace falta, cuantizados)"
                                 : "24-bit: datos exactos (escalados si hace falta, cuantizados)");
            }
        }
    }

    // foreignmeta::scan (src/foreign_metadata.hpp): parser de respaldo,
    // independiente de libsndfile, para WAV 'smpl', AIFF 'MARK'+'INST'
    // (por NOMBRE de marcador, no por el enlace de ID de INST que
    // libsndfile exige) y FLAC con `flac --keep-foreign-metadata` (bloques
    // APPLICATION "riff"/"aiff" con copias byte-exactas de esos mismos
    // chunks). Byte helpers locales, sin depender de wav_io.cpp (sus
    // helpers de armado de chunks son de namespace anonimo, no exportados).
    {
        auto put16le = [](std::vector<uint8_t>& v, uint16_t x) {
            v.push_back((uint8_t)(x & 0xff));
            v.push_back((uint8_t)((x >> 8) & 0xff));
        };
        auto put32le = [](std::vector<uint8_t>& v, uint32_t x) {
            v.push_back((uint8_t)(x & 0xff));
            v.push_back((uint8_t)((x >> 8) & 0xff));
            v.push_back((uint8_t)((x >> 16) & 0xff));
            v.push_back((uint8_t)((x >> 24) & 0xff));
        };
        auto put16be = [](std::vector<uint8_t>& v, uint16_t x) {
            v.push_back((uint8_t)((x >> 8) & 0xff));
            v.push_back((uint8_t)(x & 0xff));
        };
        auto put32be = [](std::vector<uint8_t>& v, uint32_t x) {
            v.push_back((uint8_t)((x >> 24) & 0xff));
            v.push_back((uint8_t)((x >> 16) & 0xff));
            v.push_back((uint8_t)((x >> 8) & 0xff));
            v.push_back((uint8_t)(x & 0xff));
        };
        auto wrapLE = [](std::vector<uint8_t>& out, const char* tag,
                         const std::vector<uint8_t>& payload) {
            out.insert(out.end(), tag, tag + 4);
            uint32_t sz = (uint32_t)payload.size();
            out.push_back((uint8_t)(sz & 0xff));
            out.push_back((uint8_t)((sz >> 8) & 0xff));
            out.push_back((uint8_t)((sz >> 16) & 0xff));
            out.push_back((uint8_t)((sz >> 24) & 0xff));
            out.insert(out.end(), payload.begin(), payload.end());
            if (payload.size() & 1) out.push_back(0);
        };
        auto wrapBE = [](std::vector<uint8_t>& out, const char* tag,
                         const std::vector<uint8_t>& payload) {
            out.insert(out.end(), tag, tag + 4);
            uint32_t sz = (uint32_t)payload.size();
            out.push_back((uint8_t)((sz >> 24) & 0xff));
            out.push_back((uint8_t)((sz >> 16) & 0xff));
            out.push_back((uint8_t)((sz >> 8) & 0xff));
            out.push_back((uint8_t)(sz & 0xff));
            out.insert(out.end(), payload.begin(), payload.end());
            if (payload.size() & 1) out.push_back(0);
        };
        auto pstring = [](const std::string& s) {
            std::vector<uint8_t> b;
            b.push_back((uint8_t)s.size());
            b.insert(b.end(), s.begin(), s.end());
            if ((1 + s.size()) % 2 != 0) b.push_back(0);
            return b;
        };
        auto writeFile = [](const std::string& path, const std::vector<uint8_t>& bytes) {
            FILE* fp = fopen(path.c_str(), "wb");
            if (!fp) return false;
            size_t w = fwrite(bytes.data(), 1, bytes.size(), fp);
            fclose(fp);
            return w == bytes.size();
        };

        // smpl payload: manufacturer,product,period,unityNote,pitchFraction,
        // smpteFormat,smpteOffset,numLoops,samplerData, loop{id,type,start,
        // end(inclusive),fraction,playCount}.
        auto buildSmplPayload = [&](int unityNote, uint32_t pitchFrac, uint32_t start,
                                    uint32_t endIncl) {
            std::vector<uint8_t> p;
            put32le(p, 0);
            put32le(p, 0);
            put32le(p, (uint32_t)(1000000000ull / 44100));
            put32le(p, (uint32_t)unityNote);
            put32le(p, pitchFrac);
            put32le(p, 0);
            put32le(p, 0);
            put32le(p, 1); // numLoops
            put32le(p, 0);
            put32le(p, 0); // cueId
            put32le(p, 0); // type
            put32le(p, start);
            put32le(p, endIncl);
            put32le(p, 0);
            put32le(p, 0);
            return p;
        };
        // MARK payload with two named markers (the loop convention this
        // parser matches, see foreign_metadata.cpp): "beg loop"/"end loop".
        auto buildMarkPayload = [&](uint32_t startPos, uint32_t endPos) {
            std::vector<uint8_t> p;
            put16be(p, 2); // numMarkers
            put16be(p, 1);
            put32be(p, startPos);
            auto s1 = pstring("beg loop");
            p.insert(p.end(), s1.begin(), s1.end());
            put16be(p, 2);
            put32be(p, endPos);
            auto s2 = pstring("end loop");
            p.insert(p.end(), s2.begin(), s2.end());
            return p;
        };
        // INST payload: baseNote,detune,lowNote,highNote,lowVel,highVel,gain,
        // sustainLoop{mode,beginID,endID}, releaseLoop{mode,beginID,endID}.
        // sustainLoop.mode=0 (no loop): deliberately NOT linked to the MARK
        // markers by ID -- libsndfile's own SFC_GET_INSTRUMENT must report
        // loop_count==0 for this file even though the named markers make
        // the loop obvious, exactly the real-world gap this parser covers.
        auto buildInstPayload = [&](int baseNote, int8_t detune) {
            std::vector<uint8_t> p;
            p.push_back((uint8_t)baseNote);
            p.push_back((uint8_t)detune);
            p.push_back(0);
            p.push_back(127);
            p.push_back(1);
            p.push_back(127);
            put16be(p, 0); // gain
            put16be(p, 0); put16be(p, 0); put16be(p, 0); // sustainLoop: no loop
            put16be(p, 0); put16be(p, 0); put16be(p, 0); // releaseLoop
            return p;
        };

        // --- WAV, smpl placed AFTER data (trickier order) ---
        {
            std::vector<uint8_t> fmtP;
            put16le(fmtP, 1);
            put16le(fmtP, 1);
            put32le(fmtP, 44100);
            put32le(fmtP, 44100 * 2);
            put16le(fmtP, 2);
            put16le(fmtP, 16);
            std::vector<uint8_t> dataP(400, 0);
            // +25 cents ~ round(0.25 * 2^32)
            auto smplP = buildSmplPayload(69, 1073741824u, 50, 149);

            std::vector<uint8_t> body;
            body.insert(body.end(), {'W', 'A', 'V', 'E'});
            wrapLE(body, "fmt ", fmtP);
            wrapLE(body, "data", dataP);
            wrapLE(body, "smpl", smplP);
            std::vector<uint8_t> file;
            wrapLE(file, "RIFF", body);

            std::string path = tmpdir + "/fm_test.wav";
            CHECK(writeFile(path, file), "foreignmeta WAV: archivo de prueba escrito");

            foreignmeta::InstrumentInfo fi;
            bool found = foreignmeta::scan(path, fi);
            CHECK(found, "foreignmeta WAV: scan encuentra el chunk smpl (tras 'data')");
            CHECK(fi.hasNote && fi.rootNote == 69, "foreignmeta WAV: root note");
            CHECK(std::fabs(fi.tuneCents - 25.f) < 0.01f, "foreignmeta WAV: tune cents");
            CHECK(fi.hasLoop && fi.loopStart == 50 && fi.loopEnd == 150,
                  "foreignmeta WAV: loop (end inclusive+1 = 150)");
        }

        // --- AIFF: MARK con nombres convencionales + INST sin loop
        // enlazado (el caso real que motiva este parser) ---
        std::vector<uint8_t> aiffFile;
        {
            std::vector<uint8_t> commP;
            put16be(commP, 1); // channels
            put32be(commP, 200); // numSampleFrames
            put16be(commP, 16); // sampleSize
            const uint8_t sr44100[10] = {0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0};
            commP.insert(commP.end(), sr44100, sr44100 + 10);

            std::vector<uint8_t> ssndP;
            put32be(ssndP, 0);
            put32be(ssndP, 0);
            for (int i = 0; i < 200; ++i) put16be(ssndP, (uint16_t)(i * 10));

            auto markP = buildMarkPayload(50, 149);
            auto instP = buildInstPayload(69, -25);

            std::vector<uint8_t> body;
            body.insert(body.end(), {'A', 'I', 'F', 'F'});
            wrapBE(body, "COMM", commP);
            wrapBE(body, "SSND", ssndP);
            wrapBE(body, "MARK", markP);
            wrapBE(body, "INST", instP);
            wrapBE(aiffFile, "FORM", body);

            std::string path = tmpdir + "/fm_test.aiff";
            CHECK(writeFile(path, aiffFile), "foreignmeta AIFF: archivo de prueba escrito");

            foreignmeta::InstrumentInfo fi;
            bool found = foreignmeta::scan(path, fi);
            CHECK(found, "foreignmeta AIFF: scan encuentra MARK+INST");
            CHECK(fi.hasNote && fi.rootNote == 69, "foreignmeta AIFF: root note (INST)");
            CHECK(std::fabs(fi.tuneCents - (-25.f)) < 0.01f, "foreignmeta AIFF: tune cents (INST)");
            CHECK(fi.hasLoop && fi.loopStart == 50 && fi.loopEnd == 150,
                  "foreignmeta AIFF: loop por NOMBRE de marcador (\"beg loop\"/\"end loop\")");

            // extremo a extremo: loadAudioFile() debe encontrar el loop via
            // el fallback aunque libsndfile (INST sin enlazar) no lo vea.
            LoadResult r = loadAudioFile(path);
            CHECK(r.data != nullptr, "foreignmeta AIFF: loadAudioFile abre el audio (via libsndfile)");
            if (r.data) {
                CHECK(r.hasInstrument, "foreignmeta AIFF (e2e): hasInstrument (root note via libsndfile)");
                CHECK(r.rootNote == 69, "foreignmeta AIFF (e2e): root note (via libsndfile, no tocado)");
                CHECK(r.loopStart == 50 && r.loopEnd == 150,
                      "foreignmeta AIFF (e2e): el loop llega via el fallback de foreignmeta");
            }
        }

        // --- FLAC (`flac --keep-foreign-metadata`): bloques APPLICATION
        // con copias byte-exactas de los chunks originales. Solo se
        // construyen los bloques de METADATA (todo lo que este parser
        // lee); no hace falta un stream de audio FLAC valido de verdad,
        // ya que scan() nunca llega a leer los frames de audio.
        {
            std::vector<uint8_t> file = {'f', 'L', 'a', 'C'};
            auto appBlock = [&](const char* appid, const std::vector<uint8_t>& rawChunk,
                                bool last) {
                std::vector<uint8_t> payload(appid, appid + 4);
                payload.insert(payload.end(), rawChunk.begin(), rawChunk.end());
                uint32_t len = (uint32_t)payload.size();
                uint8_t hdr0 = (uint8_t)(2 | (last ? 0x80 : 0)); // type=2 APPLICATION
                file.push_back(hdr0);
                file.push_back((uint8_t)((len >> 16) & 0xff));
                file.push_back((uint8_t)((len >> 8) & 0xff));
                file.push_back((uint8_t)(len & 0xff));
                file.insert(file.end(), payload.begin(), payload.end());
            };

            // 'smpl' wrapped as a WAV chunk (tag+LE size+payload), embedded
            // in a "riff"-tagged APPLICATION block.
            std::vector<uint8_t> smplChunk;
            wrapLE(smplChunk, "smpl", buildSmplPayload(60, 0, 1000, 1999));
            appBlock("riff", smplChunk, /*last=*/true);

            std::string path = tmpdir + "/fm_test_wav.flac";
            CHECK(writeFile(path, file), "foreignmeta FLAC (origen WAV): archivo de prueba escrito");
            foreignmeta::InstrumentInfo fi;
            CHECK(foreignmeta::scan(path, fi),
                  "foreignmeta FLAC (origen WAV): scan encuentra el 'smpl' embebido");
            CHECK(fi.hasNote && fi.rootNote == 60, "foreignmeta FLAC (origen WAV): root note");
            CHECK(fi.hasLoop && fi.loopStart == 1000 && fi.loopEnd == 2000,
                  "foreignmeta FLAC (origen WAV): loop");
        }
        {
            std::vector<uint8_t> file = {'f', 'L', 'a', 'C'};
            auto appBlock = [&](const char* appid, const std::vector<uint8_t>& rawChunk,
                                bool last) {
                std::vector<uint8_t> payload(appid, appid + 4);
                payload.insert(payload.end(), rawChunk.begin(), rawChunk.end());
                uint32_t len = (uint32_t)payload.size();
                uint8_t hdr0 = (uint8_t)(2 | (last ? 0x80 : 0));
                file.push_back(hdr0);
                file.push_back((uint8_t)((len >> 16) & 0xff));
                file.push_back((uint8_t)((len >> 8) & 0xff));
                file.push_back((uint8_t)(len & 0xff));
                file.insert(file.end(), payload.begin(), payload.end());
            };

            std::vector<uint8_t> markChunk, instChunk;
            wrapBE(markChunk, "MARK", buildMarkPayload(2000, 3999));
            wrapBE(instChunk, "INST", buildInstPayload(72, 10));
            appBlock("aiff", markChunk, /*last=*/false);
            appBlock("aiff", instChunk, /*last=*/true);

            std::string path = tmpdir + "/fm_test_aiff.flac";
            CHECK(writeFile(path, file), "foreignmeta FLAC (origen AIFF): archivo de prueba escrito");
            foreignmeta::InstrumentInfo fi;
            CHECK(foreignmeta::scan(path, fi),
                  "foreignmeta FLAC (origen AIFF): scan encuentra MARK+INST embebidos");
            CHECK(fi.hasNote && fi.rootNote == 72, "foreignmeta FLAC (origen AIFF): root note");
            CHECK(std::fabs(fi.tuneCents - 10.f) < 0.01f, "foreignmeta FLAC (origen AIFF): tune cents");
            CHECK(fi.hasLoop && fi.loopStart == 2000 && fi.loopEnd == 4000,
                  "foreignmeta FLAC (origen AIFF): loop por nombre de marcador");
        }

        // archivo irrelevante -> false, sin crash
        {
            std::string path = tmpdir + "/fm_test_garbage.bin";
            CHECK(writeFile(path, {1, 2, 3, 4, 5, 6, 7, 8}), "foreignmeta: archivo basura escrito");
            foreignmeta::InstrumentInfo fi;
            CHECK(!foreignmeta::scan(path, fi), "foreignmeta: archivo irreconocible -> false, sin crash");
        }
    }

    // grabacion: se concatena al final adaptando el sample rate
    {
        int64_t before = doc.frames();
        int64_t ls = doc.loopStart(), le = doc.loopEnd();
        auto rec = std::make_shared<AudioData>();
        rec->sampleRate = 88200; // "host" al doble del documento
        rec->format = SampleFormat::Float32;
        rec->ch.resize(2);
        for (int c = 0; c < 2; ++c) rec->ch[c].assign(8820, 0.25f);
        rec->buildPeaks();
        doc.appendAudio(rec, "Grabar (concatenar)");
        CHECK(std::llabs((long long)(doc.frames() - before) - 4410) <= 1,
              "grabacion concatenada (resample 2:1 al SR del doc)");
        CHECK(doc.loopStart() == ls && doc.loopEnd() == le,
              "concatenar no toca los marcadores de loop");
        CHECK(doc.selStart() == before,
              "la seleccion marca la zona recien grabada");
        doc.undo();
        CHECK(doc.frames() == before, "deshacer la grabacion concatenada");
    }

    // normalizar solo la seleccion
    {
        float outsideBefore = doc.data()->ch[0][40500];
        doc.setSelection(0, 5000);
        doc.normalize();
        float inPeak = 0.f;
        for (int i = 0; i < 5000; ++i)
            inPeak = std::max(inPeak, std::fabs(doc.data()->ch[0][i]));
        CHECK(std::fabs(inPeak - 1.0f) < 1e-3f,
              "normalizar seleccion: pico interno a 1.0");
        CHECK(doc.data()->ch[0][40500] == outsideBefore,
              "normalizar seleccion: fuera de la seleccion intacto");
        doc.undo();
        doc.clearSelection();
    }

    // nombre del sample: sigue al documento a traves de undo/redo
    {
        doc.setSampleName("kick_808");
        CHECK(doc.sampleName() == "kick_808", "asignar nombre del sample");
        doc.normalize(); // cualquier edicion normal preserva el nombre
        CHECK(doc.sampleName() == "kick_808",
              "el nombre sobrevive a una edicion normal");
        doc.undo();
        CHECK(doc.sampleName() == "kick_808",
              "deshacer una edicion no toca el nombre (ya estaba puesto)");
    }

    // Supr sin seleccion: vaciar el editor (undoable) - tambien resetea tune
    // a 0 cents y root note a 60/C4 (valores de fabrica) y el nombre del sample.
    {
        int64_t lenBefore = doc.frames();
        doc.setTuneCents(-25.f);
        CHECK(doc.rootNote() == 69, "root note previa al vaciado (de prueba anterior)");
        doc.clearAll();
        CHECK(!doc.hasAudio(), "vaciar el editor");
        CHECK(doc.tuneCents() == 0.f, "vaciar resetea el tune a 0 cents");
        CHECK(doc.rootNote() == 60, "vaciar resetea la root note a 60 (C4)");
        CHECK(doc.sampleName().empty(), "vaciar resetea el nombre del sample");
        doc.undo();
        CHECK(doc.frames() == lenBefore && doc.loopStart() == 100,
              "deshacer el vaciado recupera audio y loop");
        CHECK(doc.tuneCents() == -25.f,
              "deshacer el vaciado recupera el tune anterior");
        CHECK(doc.rootNote() == 69,
              "deshacer el vaciado recupera la root note anterior");
        CHECK(doc.sampleName() == "kick_808",
              "deshacer el vaciado recupera el nombre del sample");
        doc.setTuneCents(0.f);
    }

    // recortar todo lo posterior al loop end
    {
        doc.trimAfterLoopEnd();
        CHECK(doc.frames() == 40000, "recortar tras loop end");
        CHECK(doc.loopStart() == 100 && doc.loopEnd() == 40000,
              "loop intacto tras el recorte");
        doc.undo();
        CHECK(doc.frames() == 44100, "deshacer el recorte");
    }

    // quitar ataque: recortar todo lo anterior al loop start
    {
        float atLoopStart = doc.data()->ch[0][100];
        doc.trimBeforeLoopStart();
        CHECK(doc.frames() == 44000, "quitar ataque: 44100 - 100 muestras");
        CHECK(doc.loopStart() == 0 && doc.loopEnd() == 39900,
              "quitar ataque: loop pasa a empezar en 0");
        CHECK(doc.data()->ch[0][0] == atLoopStart,
              "quitar ataque: la muestra 0 es la vieja muestra del loop start");
        doc.undo();
        CHECK(doc.frames() == 44100 && doc.loopStart() == 100 &&
                  doc.loopEnd() == 40000,
              "deshacer quitar ataque restaura audio y loop");
    }

    // loadFromState (recarga de proyecto del host) trae tambien el nombre
    {
        AudioDataPtr saved = doc.data();
        doc.loadFromState(saved, doc.loopStart(), doc.loopEnd(),
                          doc.rootNote(), doc.tuneCents(), "snare_2");
        CHECK(doc.sampleName() == "snare_2",
              "loadFromState restaura el nombre del sample");
    }

    // dsp::dft: radix-2 (potencia de dos) y Bluestein (longitud arbitraria)
    // contra una DFT directa O(n^2), y round-trip de rfft/irfft/ifft.
    {
        auto bruteDft = [](const std::vector<dsp::Complex>& x) {
            const size_t n = x.size();
            std::vector<dsp::Complex> X(n);
            for (size_t k = 0; k < n; ++k) {
                dsp::Complex sum(0.0, 0.0);
                for (size_t j = 0; j < n; ++j) {
                    double ang = -2.0 * M_PI * (double)((j * k) % n) / (double)n;
                    sum += x[j] * dsp::Complex(std::cos(ang), std::sin(ang));
                }
                X[k] = sum;
            }
            return X;
        };
        auto maxDiff = [](const std::vector<dsp::Complex>& a, const std::vector<dsp::Complex>& b) {
            double m = 0.0;
            for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i]));
            return m;
        };

        std::vector<dsp::Complex> x16(16);
        for (int i = 0; i < 16; ++i) x16[i] = dsp::Complex(std::sin(0.3 * i) + 0.5 * i, std::cos(0.7 * i));
        auto got16 = x16;
        dsp::dft(got16, false);
        CHECK(maxDiff(got16, bruteDft(x16)) < 1e-9,
              "dft radix-2 coincide con DFT directa (n=16)");

        std::vector<dsp::Complex> x17(17);
        for (int i = 0; i < 17; ++i) x17[i] = dsp::Complex(std::sin(0.3 * i) + 0.5 * i, std::cos(0.7 * i));
        auto got17 = x17;
        dsp::dft(got17, false);
        CHECK(maxDiff(got17, bruteDft(x17)) < 1e-6,
              "dft Bluestein coincide con DFT directa (n=17, no potencia de dos)");
        auto back17 = got17;
        dsp::dft(back17, true);
        CHECK(maxDiff(back17, x17) < 1e-9,
              "dft Bluestein: ida y vuelta recupera la senal (n=17)");

        std::vector<double> xr(64);
        for (int i = 0; i < 64; ++i) xr[i] = std::sin(0.2 * i) * std::exp(-0.01 * i);
        auto half = dsp::rfft(xr);
        auto backr = dsp::irfft(half, 64);
        double m = 0.0;
        for (int i = 0; i < 64; ++i) m = std::max(m, std::fabs(backr[i] - xr[i]));
        CHECK(m < 1e-9, "rfft/irfft: ida y vuelta recupera la senal real");
    }

    // Harmonic (harmonicdsp::buildLoop): puerto de proto/freeze_loop.py
    // "loop" (build_seamless_loop) -- vocoder de fase + rotacion alineada.
    {
        const double sr = 44100.0;
        const int n = (int)(sr * 3.0); // 3 s, tono armonico con decaimiento
        auto sig = std::make_shared<AudioData>();
        sig->sampleRate = sr;
        sig->format = SampleFormat::Float32;
        sig->ch.resize(2);
        for (auto& c : sig->ch) c.resize(n);
        for (int i = 0; i < n; ++i) {
            double t = i / sr;
            double v = 0.0;
            for (int h = 1; h <= 6; ++h)
                v += (1.0 / h) * std::sin(2.0 * M_PI * 220.0 * h * t) * std::exp(-t * 0.12 * h);
            sig->ch[0][i] = (float)(v * 0.4);
            sig->ch[1][i] = (float)(v * 0.4);
        }

        const int64_t ls = (int64_t)(sr * 1.0);      // freeze-time = 1 s
        const int64_t le = ls + (int64_t)(sr * 0.5); // loop-duration = 0.5 s

        harmonicdsp::Result res;
        bool ok = harmonicdsp::buildLoop(*sig, ls, le, 2048, 30.f, 0.5f, false, 60, 0.f, res);
        CHECK(ok, "Harmonic: buildLoop tiene suficiente audio y no falla");
        if (ok) {
            CHECK(res.nLoopSamples > 0 && res.nLoopSamples % (2048 / 4) == 0,
                  "Harmonic: nLoopSamples es multiplo del hop");
            CHECK((int64_t)res.ch[0].loopRegion.size() == res.nLoopSamples,
                  "Harmonic: loopRegion tiene la longitud esperada");
            CHECK((int64_t)res.ch[0].attackTail.size() == res.xfadeN,
                  "Harmonic: attackTail tiene la longitud del crossfade resuelto");
            CHECK(res.seamError < 1e-3,
                  "Harmonic: el loop sintetizado es periodico (wrap end->start sin discontinuidad)");
            float peak = 0.f;
            for (float v : res.ch[0].loopRegion) peak = std::max(peak, std::fabs(v));
            CHECK(peak > 0.01f && peak < 2.0f,
                  "Harmonic: el loop sintetizado tiene una amplitud razonable");
        }

        // Modo nota MIDI: nLoopSamples debe ser multiplo EXACTO del periodo
        // de la nota (sr/f0 redondeado), no de la rejilla del hop.
        {
            harmonicdsp::Result resNote;
            const int rootNote = 57; // A3, 220 Hz -- coincide con la fundamental de sig
            bool okNote = harmonicdsp::buildLoop(*sig, ls, le, 2048, 30.f, 0.5f, true, rootNote,
                                                 0.f, resNote);
            CHECK(okNote, "Harmonic (nota MIDI): buildLoop no falla");
            if (okNote) {
                const int64_t period =
                    (int64_t)std::llround(sr / harmonicdsp::noteFreq(rootNote, 0.f));
                CHECK(resNote.notePeriod == period,
                      "Harmonic (nota MIDI): notePeriod coincide con sr/f0(rootNote)");
                CHECK(resNote.nLoopSamples > 0 && resNote.nLoopSamples % resNote.notePeriod == 0,
                      "Harmonic (nota MIDI): nLoopSamples es multiplo exacto del periodo de la nota");
                CHECK(resNote.nLoopSamples / resNote.notePeriod > 1,
                      "Harmonic (nota MIDI): conserva multiples ciclos (no pliega a 1 como Freeze)");
            }
        }

        // audio mas corto que el fft-size elegido -> falla con gracia
        auto shortSig = std::make_shared<AudioData>();
        shortSig->sampleRate = 44100.0;
        shortSig->format = SampleFormat::Float32;
        shortSig->ch.resize(1);
        shortSig->ch[0].assign(100, 0.1f);
        harmonicdsp::Result res2;
        bool ok2 = harmonicdsp::buildLoop(*shortSig, 10, 90, 2048, 30.f, 0.5f, false, 60, 0.f, res2);
        CHECK(!ok2, "Harmonic: buildLoop reporta fallo con gracia si el audio es mas corto que el fft-size");

        // Continuidad en ls (regresion del bug de rotacion: el loop marcado
        // tiene que arrancar en lb[xfadeN], no en lb[0], para empalmar sin
        // salto con el final de attackTail). Se usa un tono puro de baja
        // frecuencia (no el tono armonico de arriba, que tiene contenido de
        // alta frecuencia y por lo tanto pasos de una muestra a la
        // siguiente ya de por si grandes): a 110 Hz / 44100 Hz el paso
        // maximo entre DOS MUESTRAS ADYACENTES esta acotado por
        // amplitud*2*pi*f/sr =~ 0.008, así que cualquier salto mayor solo
        // puede venir de comparar muestras que en realidad estan separadas
        // por xfadeN muestras (~3 periodos a 110 Hz) en vez de 1 -- el bug
        // original.
        {
            const int nLo = (int)(sr * 3.0);
            auto lo = std::make_shared<AudioData>();
            lo->sampleRate = sr;
            lo->format = SampleFormat::Float32;
            lo->ch.resize(1);
            lo->ch[0].resize(nLo);
            for (int i = 0; i < nLo; ++i)
                lo->ch[0][i] = (float)(0.5 * std::sin(2.0 * M_PI * 110.0 * i / sr));
            const int64_t lsLo = (int64_t)(sr * 1.0);
            const int64_t leLo = lsLo + (int64_t)(sr * 0.5);
            harmonicdsp::Result resLo;
            bool okLo = harmonicdsp::buildLoop(*lo, lsLo, leLo, 2048, 30.f, 0.5f, false, 60, 0.f, resLo);
            CHECK(okLo, "Harmonic (tono puro): buildLoop no falla");
            if (okLo && resLo.xfadeN > 0) {
                const float gap = std::fabs(resLo.ch[0].attackTail.back() - resLo.ch[0].loopRegion.front());
                CHECK(gap < 0.02f,
                      "Harmonic: attackTail empalma sin salto con loopRegion en ls (sin click de union)");
            }
        }

        // sizes(): el crossfade pedido se recorta al audio disponible antes de ls
        int64_t req, xf, nls, notePer;
        harmonicdsp::sizes(50, 50 + 4410, sr, 2048, 1000.f, false, 60, 0.f, req, xf, nls, notePer);
        CHECK(xf == 50, "Harmonic: sizes() recorta el crossfade al audio disponible antes de ls");
        CHECK(notePer == 0, "Harmonic: sizes() deja notePeriod en 0 cuando noteMode==false");

        // PingPong Harmonic (pingpongdsp::buildLoop): puerto de
        // proto/mirror_loop.py -- recorre los frames STFT de la region
        // adelante y atras (bloqueo de fase por identidad sobre los picos
        // espectrales), reusa el mismo tono armonico con decaimiento de
        // arriba (tiene movimiento espectral real, no un tono estacionario,
        // que es justo lo que hace audible la inversion de orden de frames).
        {
            const int laps = 2;
            pingpongdsp::Result res;
            bool okpp = pingpongdsp::buildLoop(*sig, ls, le, 2048, laps, 30.f, 0.5f, res);
            CHECK(okpp, "PingPong Harmonic: buildLoop tiene suficiente audio y no falla");
            if (okpp) {
                CHECK(res.oneLapSamples > 0 && res.oneLapSamples % (2048 / 4) == 0,
                      "PingPong Harmonic: oneLapSamples es multiplo del hop");
                CHECK((int64_t)res.ch[0].body.size() == laps * res.oneLapSamples,
                      "PingPong Harmonic: body tiene 'laps' vueltas concatenadas");
                CHECK((int64_t)res.ch[0].attackTail.size() == res.xfadeN,
                      "PingPong Harmonic: attackTail tiene la longitud del crossfade resuelto");
                CHECK(res.seamError < 1e-3,
                      "PingPong Harmonic: la vuelta sintetizada es periodica (wrap vuelta->vuelta sin discontinuidad)");
                float peak = 0.f;
                for (float v : res.ch[0].body) peak = std::max(peak, std::fabs(v));
                CHECK(peak > 0.01f && peak < 2.0f,
                      "PingPong Harmonic: la vuelta sintetizada tiene una amplitud razonable");
                if (res.xfadeN > 0) {
                    const float gap = std::fabs(res.ch[0].attackTail.back() - res.ch[0].body.front());
                    CHECK(gap < 0.02f,
                          "PingPong Harmonic: attackTail empalma sin salto con el inicio de la primera vuelta");
                }
            }

            // ls < hop (aqui hop=512): no hay contexto de fase antes de ls -> falla con gracia
            pingpongdsp::Result resNoCtx;
            bool okNoCtx = pingpongdsp::buildLoop(*sig, 10, 1000, 2048, laps, 30.f, 0.5f, resNoCtx);
            CHECK(!okNoCtx, "PingPong Harmonic: buildLoop falla con gracia si ls < hop (sin contexto de fase)");

            // region demasiado corta para el fft-size elegido (menos de 3 frames STFT)
            auto shortSig = std::make_shared<AudioData>();
            shortSig->sampleRate = sr;
            shortSig->format = SampleFormat::Float32;
            shortSig->ch.resize(1);
            shortSig->ch[0].assign(700, 0.1f);
            pingpongdsp::Result resShort;
            bool okShort = pingpongdsp::buildLoop(*shortSig, 600, 650, 2048, laps, 30.f, 0.5f, resShort);
            CHECK(!okShort, "PingPong Harmonic: buildLoop reporta fallo con gracia si la region es mas corta que el fft-size");

            int64_t reqPP, xfPP, oneLapPP;
            pingpongdsp::sizes(50, 50 + 4410, sr, 2048, 1000.f, reqPP, xfPP, oneLapPP);
            CHECK(xfPP == 50, "PingPong Harmonic: sizes() recorta el crossfade al audio disponible antes de ls");
        }
    }

    // Document::insertPiece: igual que replaceRange pero SIN tocar los
    // marcadores de loop (usado por Freeze, que no requiere loop marcado).
    {
        auto d = std::make_shared<AudioData>();
        d->sampleRate = 44100.0;
        d->format = SampleFormat::Float32;
        d->ch.resize(1);
        d->ch[0].assign(1000, 0.1f);
        d->buildPeaks();
        doc.setAudio(d, "insertPiece: setup");
        doc.setLoopPoints(500, 800, false);

        std::vector<std::vector<float>> piece(1, std::vector<float>(50, 0.9f));
        doc.insertPiece(100, 120, std::move(piece), "insertPiece: test");
        CHECK(doc.frames() == 1030,
              "insertPiece: la longitud crece segun lo insertado menos lo borrado");
        CHECK(doc.loopStart() == 500 && doc.loopEnd() == 800,
              "insertPiece: no reescribe marcadores de loop existentes (solo se clampan)");
        doc.undo();
        CHECK(doc.frames() == 1000, "insertPiece: deshacer restaura la longitud");
    }

    // Freeze (freezedsp::buildFreeze): puerto de proto/freeze_loop.py
    // "freeze" (spectral_freeze), simplificado a pedido del usuario (sin
    // busqueda automatica de rotacion, sin crossfade de salida). Comparte
    // el vocoder de fase y el fundido de entrada con Harmonic
    // (specloop::rotateAndBlend), asi que la continuidad de esa union ya
    // esta cubierta por los tests de Harmonic; aqui se verifica lo propio
    // de Freeze: freezeStart derivado de freezePos, la rotacion manual (sin
    // busqueda) y que funciona sin loop/seleccion.
    {
        const double sr = 44100.0;
        const int n = (int)(sr * 3.0);
        auto sig = std::make_shared<AudioData>();
        sig->sampleRate = sr;
        sig->format = SampleFormat::Float32;
        sig->ch.resize(2);
        for (auto& c : sig->ch) c.resize(n);
        for (int i = 0; i < n; ++i) {
            double t = i / sr;
            double v = 0.0;
            for (int h = 1; h <= 6; ++h)
                v += (1.0 / h) * std::sin(2.0 * M_PI * 220.0 * h * t) * std::exp(-t * 0.12 * h);
            sig->ch[0][i] = (float)(v * 0.4);
            sig->ch[1][i] = (float)(v * 0.4);
        }

        const double freezePos = 1.0 / 3.0;
        freezedsp::Result res;
        bool ok = freezedsp::buildFreeze(*sig, freezePos, 500.f, 0.3f, 30.f, 0.5f, 0.f, 2048, false, 60, res);
        CHECK(ok, "Freeze: buildFreeze tiene suficiente audio y no falla");
        if (ok) {
            const int64_t expectedStart = (int64_t)std::llround(freezePos * (double)(n - 1));
            CHECK(res.freezeStart == expectedStart,
                  "Freeze: freezeStart se deriva correctamente de freezePos");
            CHECK(res.nLoopSamples > 0 && res.nLoopSamples % (2048 / 4) == 0,
                  "Freeze: nLoopSamples es multiplo del hop");
            CHECK((int64_t)res.ch[0].loopRegion.size() == res.nLoopSamples,
                  "Freeze: loopRegion tiene la longitud esperada");
            CHECK((int64_t)res.ch[0].attackTail.size() == res.xfadeN,
                  "Freeze: attackTail tiene la longitud del crossfade resuelto");
            CHECK(res.seamError < 1e-3,
                  "Freeze: el tramo congelado es periodico (para un eventual loop manual)");
            const int64_t expectedShift =
                (int64_t)std::llround(0.3 * (double)res.nLoopSamples) % res.nLoopSamples;
            CHECK(res.shift == expectedShift,
                  "Freeze: la rotacion es exactamente offsetFrac, sin busqueda automatica");
        }

        // audio mas corto que el fft-size elegido -> falla con gracia
        auto shortSig = std::make_shared<AudioData>();
        shortSig->sampleRate = 44100.0;
        shortSig->format = SampleFormat::Float32;
        shortSig->ch.resize(1);
        shortSig->ch[0].assign(100, 0.1f);
        freezedsp::Result res2;
        bool ok2 = freezedsp::buildFreeze(*shortSig, 0.5, 500.f, 0.f, 30.f, 0.5f, 0.f, 2048, false, 60, res2);
        CHECK(!ok2, "Freeze: buildFreeze reporta fallo con gracia si el audio es mas corto que el fft-size");

        // freezePos=0 (inicio del sample): sin material previo -> xfadeN=0,
        // no requiere selection/loop y no crashea
        freezedsp::Result res3;
        bool ok3 = freezedsp::buildFreeze(*sig, 0.0, 500.f, 0.f, 30.f, 0.5f, 0.f, 2048, false, 60, res3);
        CHECK(ok3 && res3.freezeStart == 0 && res3.xfadeN == 0,
              "Freeze: freezePos=0.0 (inicio del sample) da xfadeN=0 sin fallar");

        // Modo "ciclo exacto" (Infinity): vocoder anclado a la serie
        // armonica de la root note, periodo redondeado a muestras enteras,
        // cada ciclo BIT-identico. El mismo tono estereo (fundamental
        // 220 Hz, canales L/R identicos) usado arriba: MIDI 57 -> periodo
        // objetivo 44100/220 = 200.45 -> 200 muestras exactas (fundamental
        // real 220.5 Hz).
        {
            freezedsp::Result rc;
            bool okC = freezedsp::buildFreeze(*sig, 1.0 / 3.0, 500.f, 0.f, 30.f, 0.5f, 0.f,
                                              2048, true, 57, rc);
            CHECK(okC, "Freeze (ciclo): buildFreeze no falla en modo ciclo exacto");
            if (okC) {
                CHECK(rc.cyclePeriod == 200,
                      "Freeze (ciclo): periodo = round(sr/f0) = 200 muestras para MIDI 57");
                CHECK(rc.nLoopSamples % rc.cyclePeriod == 0,
                      "Freeze (ciclo): la duracion es un numero entero de ciclos");
                bool identical = true;
                for (int c = 0; c < sig->channels() && identical; ++c) {
                    const auto& lr = rc.ch[c].loopRegion;
                    for (size_t i = 0; i + 200 < lr.size(); ++i)
                        if (lr[i] != lr[i + 200]) { identical = false; break; }
                }
                CHECK(identical,
                      "Freeze (ciclo): cada ciclo del cuerpo es BIT-identico al siguiente");
                // Toda la energia en armonicos de sr/periodo: el pico
                // espectral del cuerpo cae en la fundamental real (220.5 Hz).
                std::vector<double> buf(rc.ch[0].loopRegion.begin(), rc.ch[0].loopRegion.end());
                const auto X = dsp::rfft(buf);
                size_t pk = 1;
                for (size_t k = 1; k < X.size(); ++k)
                    if (std::abs(X[k]) > std::abs(X[pk])) pk = k;
                const double peakHz = (double)pk * sr / (double)buf.size();
                CHECK(std::fabs(peakHz - 44100.0 / 200.0) < 1.0,
                      "Freeze (ciclo): el pico espectral cae en la fundamental real sr/periodo");
                CHECK(rc.seamError < 0.05,
                      "Freeze (ciclo): el residuo del plegado es pequeno (senal ~armonica)");
            }

            // Duracion ~0 -> exactamente UN ciclo (el minimo de memoria).
            freezedsp::Result r1;
            bool ok1 = freezedsp::buildFreeze(*sig, 1.0 / 3.0, 0.f, 0.f, 0.f, 0.5f, 0.f,
                                              2048, true, 57, r1);
            CHECK(ok1 && r1.nLoopSamples == r1.cyclePeriod,
                  "Freeze (ciclo): duracion 0 produce exactamente un ciclo");

            // La afinacion ajusta el tono OBJETIVO (periodo distinto), sin
            // remuestrear: +99 cents sobre 220 Hz -> 232.9 Hz -> periodo 189.
            freezedsp::Result r2;
            bool ok2c = freezedsp::buildFreeze(*sig, 1.0 / 3.0, 500.f, 0.f, 30.f, 0.5f, 99.f,
                                               2048, true, 57, r2);
            CHECK(ok2c && r2.cyclePeriod == 189 && r2.nLoopSamples % 189 == 0,
                  "Freeze (ciclo): tuneCents mueve el periodo objetivo (189 muestras a +99 cents)");
        }
    }

    // Freeze usa method=Naive (sin vocoder de fase, ver spectral_loop_core.hpp),
    // asi que la afinacion manual (tuneCents) no puede desplazar una
    // frecuencia por bin (todas quedan en 0 por diseno) -- se remuestrea el
    // periodo ya sintetizado en su lugar (ver freeze_loop_dsp.cpp), lo que
    // cambia su longitud real. Se verifica esa relacion de longitudes
    // directamente (mas robusto que detectar picos de frecuencia sobre una
    // señal deliberadamente no tonal/con "buzz" por construccion).
    {
        const double sr = 44100.0;
        const int n = (int)(sr * 3.0);
        auto sig = std::make_shared<AudioData>();
        sig->sampleRate = sr;
        sig->format = SampleFormat::Float32;
        sig->ch.resize(1);
        sig->ch[0].resize(n);
        for (int i = 0; i < n; ++i)
            sig->ch[0][i] = (float)(0.5 * std::sin(2.0 * M_PI * 220.0 * i / sr));

        freezedsp::Result resFlat;
        bool okFlat = freezedsp::buildFreeze(*sig, 1.0 / 3.0, 500.f, 0.f, 30.f, 0.5f, 0.f, 2048, false, 60, resFlat);
        CHECK(okFlat, "Freeze (afinacion): buildFreeze con tuneCents=0 no falla");

        const float tuneCents = 50.f; // medio semitono, sube de tono -> periodo mas corto
        freezedsp::Result resShift;
        bool okShift = freezedsp::buildFreeze(*sig, 1.0 / 3.0, 500.f, 0.f, 30.f, 0.5f, tuneCents,
                                              2048, false, 60, resShift);
        CHECK(okShift, "Freeze (afinacion): buildFreeze con tuneCents!=0 no falla");

        if (okFlat && okShift) {
            int64_t reqFreezeStart, reqXfadeNReq, reqXfadeN, reqNLoopSamples;
            freezedsp::sizes(sig->frames(), sr, 1.0 / 3.0, 500.f, 30.f, 2048, reqFreezeStart,
                             reqXfadeNReq, reqXfadeN, reqNLoopSamples);
            CHECK(resFlat.nLoopSamples == reqNLoopSamples,
                  "Freeze (afinacion): tuneCents=0 no altera la duracion solicitada");
            const double pitchRatio = std::pow(2.0, (double)tuneCents / 1200.0);
            const int64_t expectedShifted =
                (int64_t)std::llround((double)resFlat.nLoopSamples / pitchRatio);
            printf("   Freeze afinacion: nLoopSamples sin afinar=%lld, afinado=%lld (esperado ~%lld)\n",
                   (long long)resFlat.nLoopSamples, (long long)resShift.nLoopSamples,
                   (long long)expectedShifted);
            CHECK(std::llabs(resShift.nLoopSamples - expectedShifted) <= 2,
                  "Freeze (afinacion): la afinacion remuestrea el periodo al ratio de pitch esperado");
            CHECK(resShift.nLoopSamples < resFlat.nLoopSamples,
                  "Freeze (afinacion): subir de tono acorta el periodo (remuestreo, no vocoder)");
        }

        // Si el remuestreo de afinacion ACORTA el periodo por debajo del
        // crossfade solicitado, xfadeN debe recortarse a la nueva duracion
        // -- sin ese recorte, rotateAndBlend leeria el periodo fuera de
        // rango (buffer mas corto que xfadeN).
        freezedsp::Result resTight;
        bool okTight = freezedsp::buildFreeze(*sig, 1.0 / 3.0, 104.f, 0.f, 104.f, 0.5f, 99.f, 2048, false, 60, resTight);
        CHECK(okTight,
              "Freeze (afinacion): no falla cuando el remuestreo acorta el periodo por debajo "
              "del crossfade pedido");
        if (okTight) {
            CHECK(resTight.xfadeN <= resTight.nLoopSamples,
                  "Freeze (afinacion): xfadeN se recorta a la duracion ya afinada");
            CHECK((int64_t)resTight.ch[0].attackTail.size() == resTight.xfadeN &&
                      (int64_t)resTight.ch[0].loopRegion.size() == resTight.nLoopSamples,
                  "Freeze (afinacion): las longitudes resultantes son consistentes tras el recorte");
        }
    }

    // Rotation (rotationdsp::buildRotationLoop): puerto de proto/rotation_loop.py
    // -- reordena en capas rotadas un loop YA MARCADO (se asume seamless, no
    // sintetiza nada propio), luego reutiliza specloop (empalme con el
    // ataque real, igual que Harmonic) y ccinside (fundido final, Classic
    // Crossfade Inside) tal cual las usan Harmonic y ClassicCrossfadeInside
    // respectivamente.
    {
        const double sr = 44100.0;
        const int n = (int)(sr * 3.0);
        const double f = 110.0; // tono puro y bajo: paso maximo entre muestras acotado
        const int64_t ls = (int64_t)(sr * 1.0);
        const int64_t L = 4410; // 0.1 s -- cabe un numero entero de ciclos a 110 Hz (periodico exacto)
        const int64_t le = ls + L;

        auto sig = std::make_shared<AudioData>();
        sig->sampleRate = sr;
        sig->format = SampleFormat::Float32;
        sig->ch.resize(1);
        sig->ch[0].resize(n);
        for (int i = 0; i < n; ++i)
            sig->ch[0][i] = (float)(0.5 * std::sin(2.0 * M_PI * f * i / sr));

        rotationdsp::Result res;
        bool ok = rotationdsp::buildRotationLoop(*sig, ls, le, 8, 42, 30.f, 0.5f, 25.f, 1.f, res);
        CHECK(ok, "Rotation: buildRotationLoop no falla con un loop ya periodico");
        if (ok) {
            CHECK(res.nLoopSamples == L,
                  "Rotation: nLoopSamples == le-ls (no cambia la duracion del loop)");
            CHECK((int64_t)res.offsets.size() == 8 && res.offsets[0] == 0,
                  "Rotation: 8 offsets, el primero sin rotar (capa de referencia)");
            CHECK((int64_t)res.ch[0].attackTail.size() == res.xfadeN,
                  "Rotation: attackTail tiene la longitud del crossfade resuelto");
            CHECK((int64_t)res.ch[0].ccHead.size() == res.ccAvail,
                  "Rotation: ccHead tiene la longitud del fundido interno resuelto");
            CHECK((int64_t)res.ch[0].loopBlended.size() == res.nLoopSamples - res.ccAvail,
                  "Rotation: loopBlended == nLoopSamples - ccAvail");

            float peak = 0.f;
            for (float v : res.ch[0].loopBlended) peak = std::max(peak, std::fabs(v));
            CHECK(peak > 0.01f && peak < 2.0f, "Rotation: el loop final tiene una amplitud razonable");

            // Continuidad en el wrap del loop final (end->start): mismo
            // argumento que el regression test de Harmonic a 110 Hz -- un
            // salto mayor al paso normal entre dos muestras adyacentes solo
            // puede venir de un error de indices en el fundido final.
            if (res.ccAvail > 0 && !res.ch[0].loopBlended.empty()) {
                const float gap =
                    std::fabs(res.ch[0].loopBlended.back() - res.ch[0].loopBlended.front());
                CHECK(gap < 0.02f,
                      "Rotation: el loop final (tras Classic Crossfade Inside) empalma sin "
                      "salto en su propio wrap");
            }
        }

        // Reproducibilidad: misma seed -> mismos offsets y mismo audio.
        rotationdsp::Result resA, resB;
        bool okA = rotationdsp::buildRotationLoop(*sig, ls, le, 8, 7, 30.f, 0.5f, 25.f, 1.f, resA);
        bool okB = rotationdsp::buildRotationLoop(*sig, ls, le, 8, 7, 30.f, 0.5f, 25.f, 1.f, resB);
        CHECK(okA && okB && resA.offsets == resB.offsets,
              "Rotation: la misma seed produce los mismos offsets de rotacion");
        if (okA && okB)
            CHECK(resA.ch[0].loopBlended == resB.ch[0].loopBlended,
                  "Rotation: la misma seed produce el mismo audio (reproducible)");

        rotationdsp::Result resC;
        bool okC = rotationdsp::buildRotationLoop(*sig, ls, le, 8, 99, 30.f, 0.5f, 25.f, 1.f, resC);
        CHECK(okC && resA.offsets != resC.offsets, "Rotation: seeds distintas producen offsets distintos");

        // Validacion de rango: fuera de [2,100] o le<=ls falla con gracia.
        rotationdsp::Result resBad;
        CHECK(!rotationdsp::buildRotationLoop(*sig, ls, le, 1, 0, 30.f, 0.5f, 25.f, 1.f, resBad),
              "Rotation: nLayers=1 falla con gracia (fuera de [2,100])");
        CHECK(!rotationdsp::buildRotationLoop(*sig, ls, le, 101, 0, 30.f, 0.5f, 25.f, 1.f, resBad),
              "Rotation: nLayers=101 falla con gracia (fuera de [2,100])");
        CHECK(!rotationdsp::buildRotationLoop(*sig, le, ls, 8, 0, 30.f, 0.5f, 25.f, 1.f, resBad),
              "Rotation: le<=ls falla con gracia");

        // sizes(): coherente con buildRotationLoop; a ccPct=100 el fundido
        // interno es el maximo posible (mitad del loop).
        int64_t req, xf, nls, cc;
        rotationdsp::sizes(ls, le, sr, 1000.f, 100.f, req, xf, nls, cc);
        CHECK(nls == L, "Rotation: sizes() reporta nLoopSamples == le-ls");
        CHECK(cc == L / 2, "Rotation: sizes() con ccPct=100 da el fundido interno maximo (mitad del loop)");
        CHECK(xf == nls, "Rotation: sizes() recorta el crossfade de entrada a como mucho la duracion del loop");
    }

    // ccinside (crossfade_inside_core): nucleo compartido por Classic
    // Crossfade (Inside) y por el ultimo paso de Rotation.
    {
        CHECK(ccinside::fadeLen(1000, 0.f) == 0, "ccinside::fadeLen: 0% -> sin fundido");
        CHECK(ccinside::fadeLen(1000, 100.f) == 500,
              "ccinside::fadeLen: 100% -> mitad del loop (maximo posible)");
        CHECK(ccinside::fadeLen(1000, 50.f) == 250, "ccinside::fadeLen: 50% -> un cuarto del loop");

        std::vector<float> head = {0.f, 0.2f, 0.4f, 0.6f, 0.8f};
        std::vector<float> tail = {1.f, 1.f, 1.f, 1.f, 1.f};
        auto blended = ccinside::blendTail(head, tail, 0.f); // equal gain
        CHECK(blended.size() == head.size(), "ccinside::blendTail: longitud igual a head/tail");
        CHECK(std::fabs(blended.back() - head.back()) < 1e-5f,
              "ccinside::blendTail: la ultima muestra del fundido coincide con la ultima de "
              "head (empalme sin salto con lo que sigue)");
    }

    // Granular (granulardsp::buildGranularLoop / estimatePeriod): puerto de
    // proto/granaular sample looping.py (metodo de Peter Eastman) -- a
    // diferencia de Harmonic/Freeze no exige sintesis espectral ni un unico
    // periodo puro, y a diferencia de Rotation no reordena un loop ya
    // seamless: encadena varios grains de longitud aleatoria DENTRO de
    // [ls,le) via crossfades alineados en fase, construyendo un cuerpo de
    // loop mas largo que la region marcada.
    {
        const double sr = 44100.0;
        const int n = (int)(sr * 2.0);
        const double f = 220.0; // periodo ~200 muestras: acota el costo de la busqueda de fase
        const int64_t ls = (int64_t)(sr * 0.5);
        const int64_t le = ls + 22050; // 0.5 s de region marcada

        auto sig = std::make_shared<AudioData>();
        sig->sampleRate = sr;
        sig->format = SampleFormat::Float32;
        sig->ch.resize(1);
        sig->ch[0].resize(n);
        for (int i = 0; i < n; ++i)
            sig->ch[0][i] = (float)(0.5 * std::sin(2.0 * M_PI * f * i / sr));

        int64_t period = granulardsp::estimatePeriod(*sig, ls, le - ls);
        CHECK(period > 0, "Granular: estimatePeriod detecta periodicidad en un tono puro");
        if (period > 0) {
            const double expected = sr / f;
            printf("   Granular periodo: detectado=%lld, esperado~%.2f\n", (long long)period,
                   expected);
            CHECK(std::fabs((double)period - expected) < 2.0,
                  "Granular: estimatePeriod acierta el periodo dentro de 2 muestras");
        }

        granulardsp::Result res;
        bool ok = granulardsp::buildGranularLoop(*sig, ls, le, 10, 20.f, 33.3f, 50.f, 42, res);
        CHECK(ok, "Granular: buildGranularLoop no falla con parametros razonables");
        int64_t splitPoint = 0;
        if (ok) {
            int64_t headLen = 0, estMin = 0, estMax = 0;
            granulardsp::sizes(ls, le, 50.f, 20.f, 33.f, 10, headLen, splitPoint, estMin, estMax);
            CHECK(res.headLen == headLen, "Granular: headLen coincide con sizes()");
            CHECK(splitPoint == ls + headLen, "Granular: splitPoint == ls + headLen");
            CHECK((int64_t)res.head[0].size() == headLen, "Granular: head tiene headLen muestras");
            CHECK((int64_t)res.body[0].size() == res.nLoopSamples,
                  "Granular: body tiene nLoopSamples muestras");
            CHECK(res.nLoopSamples > 0, "Granular: el cuerpo del loop no esta vacio");

            // Cierre exacto del wrap (body.back() -> body.front()): por
            // construccion, ambas muestras son ADYACENTES en el audio
            // original (splitPoint-1 y splitPoint) -- no una costura
            // sintetica, ver el comentario de granular_loop_dsp.hpp.
            CHECK(std::fabs(res.body[0].front() - sig->ch[0][(size_t)splitPoint]) < 1e-6f,
                  "Granular: body.front() == audio original en splitPoint (exacto)");
            CHECK(std::fabs(res.body[0].back() - sig->ch[0][(size_t)(splitPoint - 1)]) < 1e-6f,
                  "Granular: body.back() == audio original en splitPoint-1 (exacto)");

            // Y por lo tanto tambien el wrap en si (mismo argumento que el
            // test de continuidad de Rotation/Harmonic): un salto mayor al
            // paso normal entre dos muestras adyacentes solo puede venir de
            // un error de indices.
            const float gap = std::fabs(res.body[0].back() - res.body[0].front());
            CHECK(gap < 0.05f, "Granular: el cuerpo del loop empalma sin salto en su propio wrap");
        }

        // Reproducibilidad: misma seed -> mismo audio; distinta seed -> distinto.
        granulardsp::Result resA, resB, resC;
        bool okA =
            granulardsp::buildGranularLoop(*sig, ls, le, 10, 20.f, 33.3f, 50.f, 7, resA);
        bool okB =
            granulardsp::buildGranularLoop(*sig, ls, le, 10, 20.f, 33.3f, 50.f, 7, resB);
        CHECK(okA && okB && resA.body[0] == resB.body[0],
              "Granular: la misma seed produce el mismo audio (reproducible)");
        bool okC =
            granulardsp::buildGranularLoop(*sig, ls, le, 10, 20.f, 33.3f, 50.f, 99, resC);
        CHECK(okC && resA.body[0] != resC.body[0],
              "Granular: seeds distintas producen audio distinto");

        // Validacion de rango: falla con gracia, no crashea.
        granulardsp::Result resBad;
        CHECK(!granulardsp::buildGranularLoop(*sig, le, ls, 10, 20.f, 33.3f, 50.f, 0, resBad),
              "Granular: le<=ls falla con gracia");
        CHECK(!granulardsp::buildGranularLoop(*sig, ls, le, 0, 20.f, 33.3f, 50.f, 0, resBad),
              "Granular: nGrains=0 falla con gracia");
        CHECK(!granulardsp::buildGranularLoop(*sig, ls, ls + 1, 10, 20.f, 33.3f, 50.f, 0,
                                              resBad),
              "Granular: region de 1 muestra falla con gracia (sin lugar para head+cuerpo)");
    }

    // --- Auto Loop (autoloopdsp, port of milselarch/autolooper) ---
    {
        const double sr = 44100.0;
        const int64_t attackLen = 1000, tailLen = 1000;
        const int64_t period = 100;        // 441 Hz @ 44100 -> exact integer period
        const int64_t nPeriods = 20;
        const int64_t loopLen = period * nPeriods; // 2000
        const int64_t loopStart = attackLen;
        const int64_t loopEnd = attackLen + loopLen;
        const int64_t total = attackLen + loopLen + tailLen;

        auto sig = std::make_shared<AudioData>();
        sig->sampleRate = sr;
        sig->format = SampleFormat::Float32;
        sig->ch.resize(1);
        auto& v = sig->ch[0];
        v.resize((size_t)total);

        // Attack/tail: deterministic noise, no periodic structure at all --
        // any genuine match found by the search must come from the loop
        // region, not an accidental coincidence in the noise.
        std::minstd_rand rng(12345);
        std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
        for (int64_t i = 0; i < attackLen; ++i) v[(size_t)i] = noise(rng);
        for (int64_t i = 0; i < loopLen; ++i)
            v[(size_t)(loopStart + i)] =
                0.5f * sinf(2.f * (float)M_PI * (float)i / (float)period);
        for (int64_t i = 0; i < tailLen; ++i) v[(size_t)(loopEnd + i)] = noise(rng);
        sig->buildPeaks();

        autoloopdsp::Params p;
        p.minLoopSec = 90.0 / sr;
        p.maxLoopSec = 220.0 / sr;
        autoloopdsp::Result r = autoloopdsp::findLoopPoints(*sig, 0, total, p);
        CHECK(r.ok, "Auto Loop: encuentra un loop en una region periodica dentro de ruido");
        if (r.ok) {
            CHECK(r.start >= 0 && r.end <= total && r.end > r.start,
                  "Auto Loop: start/end dentro de rango y en orden");
            int64_t len = r.end - r.start;
            int64_t nearestMultiple = ((len + period / 2) / period) * period;
            CHECK(std::llabs(len - nearestMultiple) <= 2,
                  "Auto Loop: la longitud encontrada es (casi) un multiplo exacto del periodo");
            CHECK(r.start >= attackLen - 10 && r.end <= loopEnd + 10,
                  "Auto Loop: el par encontrado cae dentro de la region periodica (no en el ruido)");
            CHECK(r.score < 0.02, "Auto Loop: score bajo (la seno calza consigo misma casi exacto)");
        }

        // Restricting the search to a range with no room for even the
        // shortest requested window fails gracefully instead of crashing.
        autoloopdsp::Result rBad = autoloopdsp::findLoopPoints(*sig, 0, 50, p);
        CHECK(!rBad.ok, "Auto Loop: rango demasiado corto falla con gracia (ok=false)");

        // Restricting the search to a sub-range confines the result to it.
        autoloopdsp::Result rSel =
            autoloopdsp::findLoopPoints(*sig, loopStart, loopEnd, p);
        CHECK(rSel.ok && rSel.start >= loopStart && rSel.end <= loopEnd,
              "Auto Loop: restringido a una seleccion, el resultado no sale de ese rango");
    }

    // --- LoopAuditioneer (loopauditioneerdsp, port of GrandOrgue/LoopAuditioneer) ---
    {
        const double sr = 44100.0;
        const int64_t attackLen = 1000, tailLen = 1000;
        const int64_t period = 100; // 441 Hz @ 44100 -> exact integer period
        const int64_t nPeriods = 40;
        const int64_t loopLen = period * nPeriods; // 4000
        const int64_t loopStart = attackLen;
        const int64_t loopEnd = attackLen + loopLen;
        const int64_t total = attackLen + loopLen + tailLen;

        auto sig = std::make_shared<AudioData>();
        sig->sampleRate = sr;
        sig->format = SampleFormat::Float32;
        sig->ch.resize(1);
        auto& v = sig->ch[0];
        v.resize((size_t)total);

        std::minstd_rand rng(777);
        std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
        for (int64_t i = 0; i < attackLen; ++i) v[(size_t)i] = noise(rng);
        for (int64_t i = 0; i < loopLen; ++i)
            v[(size_t)(loopStart + i)] = 0.5f * sinf(2.f * (float)M_PI * (float)i / (float)period);
        for (int64_t i = 0; i < tailLen; ++i) v[(size_t)(loopEnd + i)] = noise(rng);
        sig->buildPeaks();

        // Matching engine test: autoSustain off (the whole buffer is the
        // "sustain section" as given), min loop length/distance small
        // enough to allow multi-period loops within this short buffer --
        // the strict quality threshold (default 0.01) should still reject
        // every noise-region pair (uncorrelated amplitudes) and only
        // accept genuine periodic matches inside the sine region.
        loopauditioneerdsp::Params p;
        p.autoSustain = false;
        p.minLoopDurationSec = 88.0 / sr;
        p.minDistanceBetweenLoopsSec = 44.0 / sr;
        p.loopsToReturn = 5;
        loopauditioneerdsp::Result r = loopauditioneerdsp::findLoops(*sig, 0, total, p);
        CHECK(r.ok, "LoopAuditioneer: encuentra loops en una region periodica dentro de ruido");
        if (r.ok) {
            CHECK(!r.loops.empty() && (int)r.loops.size() <= p.loopsToReturn,
                  "LoopAuditioneer: devuelve entre 1 y loopsToReturn candidatos");
            // Only loops[0] is guaranteed to be the global best; the rest
            // are chosen by "overlaps the selection so far" (see the final
            // selection loop in loopauditioneer_finder.cpp), which can
            // pick a worse-quality candidate before a better one that
            // doesn't yet overlap anything -- same as the original. So we
            // only check that nothing beats loops[0], not full ordering.
            bool firstIsBest = true;
            for (size_t i = 1; i < r.loops.size(); ++i)
                if (r.loops[i].quality < r.loops[0].quality) firstIsBest = false;
            CHECK(firstIsBest, "LoopAuditioneer: el primer candidato es el de mejor calidad");

            const auto& best = r.loops[0];
            CHECK(best.quality < 0.005, "LoopAuditioneer: el mejor candidato tiene calidad muy buena (seno exacto)");
            CHECK(best.start >= attackLen - 10 && best.end <= loopEnd + 10,
                  "LoopAuditioneer: el mejor candidato cae dentro de la region periodica (no en el ruido)");
            int64_t len = best.end - best.start;
            int64_t nearestMultiple = ((len + period / 2) / period) * period;
            CHECK(std::llabs(len - nearestMultiple) <= 2,
                  "LoopAuditioneer: la longitud del mejor candidato es (casi) un multiplo del periodo");

            // Structural invariant from the original: every returned loop
            // must overlap at least one other returned loop.
            bool allOverlap = true;
            for (size_t i = 0; i < r.loops.size() && r.loops.size() > 1; ++i) {
                bool overlapsAny = false;
                for (size_t j = 0; j < r.loops.size(); ++j) {
                    if (i == j) continue;
                    if ((r.loops[i].start > r.loops[j].start && r.loops[i].start < r.loops[j].end) ||
                        (r.loops[i].end < r.loops[j].end && r.loops[i].end > r.loops[j].start)) {
                        overlapsAny = true;
                        break;
                    }
                }
                if (!overlapsAny) allOverlap = false;
            }
            CHECK(allOverlap, "LoopAuditioneer: todo candidato devuelto solapa a otro (invariante del original)");
        }

        // Graceful failure: min loop duration longer than the whole buffer
        // means no candidate pair can ever qualify.
        loopauditioneerdsp::Params pBad = p;
        pBad.minLoopDurationSec = 10.0; // way longer than 'total' samples at 44100 Hz
        loopauditioneerdsp::Result rBad = loopauditioneerdsp::findLoops(*sig, 0, total, pBad);
        CHECK(!rBad.ok, "LoopAuditioneer: duracion minima imposible falla con gracia (ok=false)");

        // Too-short range fails gracefully instead of crashing.
        loopauditioneerdsp::Result rTiny = loopauditioneerdsp::findLoops(*sig, 0, 10, p);
        CHECK(!rTiny.ok, "LoopAuditioneer: rango demasiado corto falla con gracia (ok=false)");

        // Auto-sustain path: just exercise it on a clear attack+sustain
        // shape (loud decaying noise burst, then a steady tone, no
        // release) and check it doesn't crash and reports a sane range --
        // the RMS heuristic itself is approximate by design (ported as-is
        // from the original), so this doesn't pin exact sample values.
        const int64_t burstLen = (int64_t)(0.05 * sr);   // ~50ms decaying burst
        const int64_t steadyLen = (int64_t)(0.3 * sr);   // ~300ms steady tone
        auto sig2 = std::make_shared<AudioData>();
        sig2->sampleRate = sr;
        sig2->format = SampleFormat::Float32;
        sig2->ch.resize(1);
        auto& v2 = sig2->ch[0];
        v2.resize((size_t)(burstLen + steadyLen));
        std::minstd_rand rng2(555);
        for (int64_t i = 0; i < burstLen; ++i) {
            float envelope = 1.f - (float)i / (float)burstLen;
            v2[(size_t)i] = envelope * noise(rng2) * 3.f; // louder than the steady part
        }
        for (int64_t i = 0; i < steadyLen; ++i)
            v2[(size_t)(burstLen + i)] = 0.3f * sinf(2.f * (float)M_PI * (float)i / (float)period);
        sig2->buildPeaks();
        loopauditioneerdsp::Params p2;
        p2.minLoopDurationSec = 88.0 / sr;
        p2.minDistanceBetweenLoopsSec = 44.0 / sr;
        loopauditioneerdsp::Result r2 = loopauditioneerdsp::findLoops(*sig2, 0, (int64_t)v2.size(), p2);
        CHECK(r2.sustainStart >= 0 && r2.sustainEnd <= (int64_t)v2.size() && r2.sustainStart < r2.sustainEnd,
              "LoopAuditioneer: auto-sustain no crashea y reporta un rango valido");
    }

    // --- Reduccion de bit-depth: escalar, no recortar (toggleBits) ---
    // Bug reportado: forzar 32->16 bit en una muestra con picos por encima
    // del techo de 16 bits recortaba esos picos (clipping) en vez de bajar
    // el volumen de TODA la muestra para que el pico caiga justo en el
    // techo. Un seno de amplitud 1.5 pasa de 1.0 en una fraccion grande de
    // cada semiciclo -- con el bug viejo, esa fraccion entera quedaria
    // aplanada exactamente en el techo; con el escalado, solo las muestras
    // MUY cerca del pico real lo tocan.
    {
        SharedParams paramsBits;
        AudioDataPtr publishedBits;
        Document bitsDoc(paramsBits, [&](AudioDataPtr d) { publishedBits = std::move(d); });

        auto d = std::make_shared<AudioData>();
        d->sampleRate = 44100;
        d->format = SampleFormat::Float32;
        d->ch.resize(1);
        const int n = 1000;
        d->ch[0].resize(n);
        const float amp = 1.5f; // a proposito, fuera del rango de 16-bit
        for (int i = 0; i < n; ++i)
            d->ch[0][i] = amp * sinf(2.f * (float)M_PI * (float)i / 200.f);
        d->buildPeaks();
        bitsDoc.setAudio(d, "sobre-amplificado");
        CHECK(std::fabs(bitsDoc.data()->peakAbs() - amp) < 1e-4f,
              "toggleBits: carga con pico > 1.0 (32-bit)");

        bitsDoc.toggleBits(); // -> 16-bit
        CHECK(bitsDoc.data()->format == SampleFormat::Int16, "toggleBits: convierte a 16-bit");
        float peakAfter = bitsDoc.data()->peakAbs();
        CHECK(std::fabs(peakAfter - kInt16Max) < 0.001f,
              "toggleBits: el pico queda escalado justo al techo de 16 bits, no recortado a 1.0");

        int atCeiling = 0;
        for (float s : bitsDoc.data()->ch[0])
            if (s >= kInt16Max - 1e-4f) atCeiling++;
        CHECK(atCeiling < n / 10,
              "toggleBits: pocas muestras tocan el techo (un recorte dejaria muchas aplanadas ahi)");

        // Dentro de rango: no debe tocar nada.
        bitsDoc.undo(); // vuelve a 32-bit, pico 1.5
        bitsDoc.normalize();
        CHECK(std::fabs(bitsDoc.data()->peakAbs() - 1.0f) < 1e-4f, "toggleBits: preparacion, normalizado a 1.0");
        bitsDoc.toggleBits(); // 32 -> 16, pico 1.0 (apenas sobre el techo)
        CHECK(bitsDoc.data()->format == SampleFormat::Int16 &&
                  std::fabs(bitsDoc.data()->peakAbs() - kInt16Max) < 0.001f,
              "toggleBits: pico justo en 1.0 tambien se ajusta al techo exacto");
    }

    // --- Reduccion de bit-depth: escalar, no recortar (export WAV) ---
    {
        auto d = std::make_shared<AudioData>();
        d->sampleRate = 44100;
        d->format = SampleFormat::Float32;
        d->ch.resize(1);
        const int n = 1000;
        d->ch[0].resize(n);
        const float amp = 2.0f; // bien fuera de rango
        for (int i = 0; i < n; ++i)
            d->ch[0][i] = amp * sinf(2.f * (float)M_PI * (float)i / 200.f);
        d->buildPeaks();

        std::string path16 = tmpdir + "/neolooper_test_scaledown16.wav";
        std::string err;
        bool scaledDown = false;
        bool ok = exportWavSmpl(path16, *d, 16, -1, -1, 60, 0.f, err, &scaledDown);
        CHECK(ok, "export escalado: exportar WAV 16-bit con pico fuera de rango");
        CHECK(scaledDown, "export escalado: scaledDown reporta que se escalo");

        LoadResult rb = loadAudioFile(path16);
        CHECK(rb.data != nullptr, "export escalado: recarga el WAV 16-bit exportado");
        if (rb.data) {
            float peakBack = rb.data->peakAbs();
            CHECK(std::fabs(peakBack - kInt16Max) < 0.001f,
                  "export escalado: el pico recargado queda justo en el techo de 16 bits, no en 1.0");
            int atCeiling = 0;
            for (float s : rb.data->ch[0])
                if (s >= kInt16Max - 1e-4f) atCeiling++;
            CHECK(atCeiling < n / 10,
                  "export escalado: pocas muestras tocan el techo (un recorte dejaria muchas aplanadas ahi)");
        }

        // Dentro de rango: no debe escalar nada.
        auto d2 = std::make_shared<AudioData>();
        d2->sampleRate = 44100;
        d2->format = SampleFormat::Float32;
        d2->ch.resize(1);
        d2->ch[0].resize(n);
        for (int i = 0; i < n; ++i) d2->ch[0][i] = 0.4f * sinf(2.f * (float)M_PI * (float)i / 200.f);
        std::string path16b = tmpdir + "/neolooper_test_noscale16.wav";
        bool scaledDown2 = true; // para verificar que la funcion SI lo pone en false
        CHECK(exportWavSmpl(path16b, *d2, 16, -1, -1, 60, 0.f, err, &scaledDown2),
              "export sin escalar: exportar WAV 16-bit con pico dentro de rango");
        CHECK(!scaledDown2, "export sin escalar: scaledDown queda en false cuando no hace falta escalar");
    }

    // --- freezestretchdsp (loopmods/freeze_stretch_dsp.hpp): ambiance
    // effect built on Signalsmith Stretch pushed past its own "clean"
    // ratio. Output length is exact by construction (signalsmithstretchdsp
    // pre-sizes the buffer), but the CONTENT involves the library's own
    // RNG for extreme ratios, so these checks are structural (length,
    // anchor, non-silence, graceful failure), not sample-exact.
    {
        const double sr = 44100.0;
        const int64_t total = (int64_t)(sr * 3.0); // 3s
        auto sig = std::make_shared<AudioData>();
        sig->sampleRate = sr;
        sig->format = SampleFormat::Float32;
        sig->ch.resize(2);
        for (auto& ch : sig->ch) ch.resize((size_t)total);
        std::minstd_rand rng(2024);
        std::uniform_real_distribution<float> noise(-0.4f, 0.4f);
        for (int64_t i = 0; i < total; ++i) {
            sig->ch[0][(size_t)i] = noise(rng);
            sig->ch[1][(size_t)i] = noise(rng);
        }

        freezestretchdsp::Params p;
        p.positionFrac = 0.5;
        p.durationSec = 1.0;
        freezestretchdsp::Result res = freezestretchdsp::generate(*sig, p);
        CHECK(res.ok, "FreezeStretch: genera ambiance en una posicion valida");
        if (res.ok) {
            CHECK(res.ambiance.size() == 2 &&
                      res.ambiance[0].size() == (size_t)std::llround(p.durationSec * sr) &&
                      res.ambiance[1].size() == res.ambiance[0].size(),
                  "FreezeStretch: la duracion generada es exacta (samples = durationSec*sr), "
                  "el ataque original NO se antepone");
            CHECK(res.loopEnd == (int64_t)res.ambiance[0].size(),
                  "FreezeStretch: loopEnd coincide con el final del chunk generado");
            CHECK(res.loopStart >= 0 && res.loopStart <= res.loopEnd / 2,
                  "FreezeStretch: loopStart respeta el tope de Classic Crossfade (Inside) "
                  "(maximo la mitad)");
            double rms = 0.0;
            for (float s : res.ambiance[0]) rms += (double)s * (double)s;
            rms = std::sqrt(rms / (double)res.ambiance[0].size());
            CHECK(rms > 0.01, "FreezeStretch: el resultado no esta en silencio");
        }

        // Crossfade en 0% -> sin blend, loopStart en 0 (mismo criterio que
        // ccinside::fadeLen(_, 0)==0, ver su propio test mas abajo).
        freezestretchdsp::Params pNoCc = p;
        pNoCc.ccPct = 0.f;
        freezestretchdsp::Result resNoCc = freezestretchdsp::generate(*sig, pNoCc);
        CHECK(resNoCc.ok && resNoCc.loopStart == 0,
              "FreezeStretch: Crossfade en 0%% deja loopStart en 0 (todo el chunk es el loop)");

        // posicion sin lugar para ni un grano -> falla con gracia
        freezestretchdsp::Params pEnd;
        pEnd.positionFrac = 1.0;
        pEnd.durationSec = 1.0;
        CHECK(!freezestretchdsp::generate(*sig, pEnd).ok,
              "FreezeStretch: sin audio suficiente tras la posicion falla con gracia");

        // duracion invalida -> falla con gracia
        freezestretchdsp::Params pZero;
        pZero.positionFrac = 0.5;
        pZero.durationSec = 0.0;
        CHECK(!freezestretchdsp::generate(*sig, pZero).ok,
              "FreezeStretch: duracion 0 falla con gracia");

        // La duracion mas corta permitida por el slider de la UI (0.5s)
        // tambien debe funcionar y seguir sonando "congelado": el grano
        // (kGrainSec) es fijo, no se deriva de la duracion, precisamente
        // para que una duracion corta no reduzca el ratio de stretch por
        // debajo del umbral de "clean stretch" de Signalsmith.
        freezestretchdsp::Params pShort;
        pShort.positionFrac = 0.5;
        pShort.durationSec = 0.5;
        freezestretchdsp::Result resShort = freezestretchdsp::generate(*sig, pShort);
        CHECK(resShort.ok && resShort.ambiance[0].size() == (size_t)std::llround(0.5 * sr),
              "FreezeStretch: la duracion minima del slider (0.5s) tambien funciona");
    }

    printf(fails ? "\n%d TESTS FALLARON\n" : "\nTODOS LOS TESTS OK\n", fails);
    return fails ? 1 : 0;
}
