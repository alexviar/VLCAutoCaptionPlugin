/**
 * whisper_subs.cpp
 * VLC Hybrid Module: Audio Filter (Whisper) + Sub-Source (Renderer)
 *
 * Compatible con VLC SDK 3.0.x
 * Basado en:
 * - modules/audio_filter/gain.c (branch 3.0.x)
 * - modules/spu/marq.c (branch 3.0.x)
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef _WIN32
# include <basetsd.h>
typedef SSIZE_T ssize_t;
# include <winsock2.h>
# ifndef poll
#  define poll WSAPoll
# endif
#endif

// VLC headers
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_filter.h>
#include <vlc_subpicture.h>
#include <vlc_text_style.h>

#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>

#include "whisper.h"

#ifndef MODULE_STRING
# define MODULE_STRING "whisper_subs"
#endif

#ifndef N_
# define N_(str) (str)
#endif

// -----------------------------------------------------------------------------
// Global Shared State (Communication between Audio Filter and Sub-Source)
// -----------------------------------------------------------------------------
struct shared_state_t {
    std::mutex lock;
    std::string current_text;
    mtime_t last_update = 0;
};

static shared_state_t g_state;

// -----------------------------------------------------------------------------
// Prototypes (Linkage C para VLC)
// -----------------------------------------------------------------------------
extern "C" {
    static int  OpenAudio (vlc_object_t *);
    static void CloseAudio(vlc_object_t *);
    static int  OpenRender(vlc_object_t *);
    static void CloseRender(vlc_object_t *);
}

// -----------------------------------------------------------------------------
// VLC module definition
// -----------------------------------------------------------------------------
vlc_module_begin ()
    // --- Audio Filter Module ---
    set_description(N_("Whisper Audio-to-Text (Audio Filter)"))
    set_shortname(N_("Whisper ASR"))
    set_capability("audio filter", 0)
    set_category(CAT_AUDIO)
    set_subcategory(SUBCAT_AUDIO_AFILTER)
    set_callbacks(OpenAudio, CloseAudio) // Estilo 3.0.x
    add_string("whisper-model", "ggml-base.bin", N_("Model path"), NULL, false)

    // --- Sub-source Module ---
    add_submodule ()
        set_description(N_("Whisper Subtitle Renderer (Sub Source)"))
        set_shortname(N_("Whisper Subs"))
        set_capability("sub source", 10)
        set_category(CAT_VIDEO)
        set_subcategory(SUBCAT_VIDEO_SUBPIC)
        set_callbacks(OpenRender, CloseRender) // Estilo 3.0.x
vlc_module_end ()

// -----------------------------------------------------------------------------
// Audio Filter Implementation
// -----------------------------------------------------------------------------
struct filter_sys_t {
    whisper_context *ctx = nullptr;

    std::vector<float> pcm_buffer;
    std::mutex buffer_mutex;

    std::thread worker_thread;
    bool running = false;

    std::string model_path;
};

static block_t *ProcessAudio(filter_t *, block_t *);
static void WhisperWorker(filter_t *);

static int OpenAudio(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;

    auto *sys = new(std::nothrow) filter_sys_t();
    if (!sys) return VLC_ENOMEM;

    p_filter->p_sys = (filter_sys_t *)sys;
    p_filter->pf_audio_filter = ProcessAudio; // Estilo 3.0.x: asignación directa

    char *psz = var_InheritString(p_filter, "whisper-model");
    sys->model_path = psz ? psz : "ggml-base.bin";
    free(psz);

    msg_Info(p_filter, "Cargando modelo Whisper desde: %s", sys->model_path.c_str());
    
    whisper_context_params cparams = whisper_context_default_params();
    sys->ctx = whisper_init_from_file_with_params(sys->model_path.c_str(), cparams);
    if (!sys->ctx) {
        msg_Err(p_filter, "ERROR: No se pudo cargar el modelo en %s", sys->model_path.c_str());
        delete sys;
        p_filter->p_sys = nullptr;
        return VLC_EGENERIC;
    }
    
    msg_Info(p_filter, "Modelo Whisper cargado exitosamente.");

    {
        std::lock_guard<std::mutex> lock(g_state.lock);
        g_state.current_text.clear();
        g_state.last_update = 0;
    }

    sys->running = true;
    sys->worker_thread = std::thread(WhisperWorker, p_filter);

    return VLC_SUCCESS;
}

static void CloseAudio(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    auto *sys = (filter_sys_t *)p_filter->p_sys;
    if (!sys) return;

    sys->running = false;
    if (sys->worker_thread.joinable())
        sys->worker_thread.join();

    if (sys->ctx)
        whisper_free(sys->ctx);

    delete sys;
    p_filter->p_sys = nullptr;
}

static block_t *ProcessAudio(filter_t *p_filter, block_t *p_block)
{
    auto *sys = (filter_sys_t *)p_filter->p_sys;
    if (!sys || !p_block) return p_block;

    if (p_filter->fmt_in.i_codec != VLC_CODEC_FL32)
        return p_block;

    const unsigned ch = p_filter->fmt_in.audio.i_channels;
    if (ch == 0 || p_block->i_nb_samples == 0)
        return p_block;

    const float *p_samples = (const float *)p_block->p_buffer;

    std::lock_guard<std::mutex> lock(sys->buffer_mutex);
    
    // Evitar OOM: Mantener máx 10 segundos de audio (160k samples a 16kHz)
    if (sys->pcm_buffer.size() > 16000 * 10) {
        sys->pcm_buffer.erase(sys->pcm_buffer.begin(), sys->pcm_buffer.begin() + p_block->i_nb_samples);
    }

    sys->pcm_buffer.reserve(sys->pcm_buffer.size() + p_block->i_nb_samples);

    for (size_t i = 0; i < p_block->i_nb_samples; ++i) {
        sys->pcm_buffer.push_back(p_samples[i * ch]); // Mono (canal 0)
    }

    return p_block; // passthrough (no alteramos audio)
}

static void WhisperWorker(filter_t *p_filter)
{
    auto *sys = (filter_sys_t *)p_filter->p_sys;
    const size_t CHUNK_SAMPLES = 16000 * 3;

    while (sys->running) {
        std::vector<float> samples;

        {
            std::lock_guard<std::mutex> lock(sys->buffer_mutex);
            if (sys->pcm_buffer.size() >= CHUNK_SAMPLES) {
                samples.assign(sys->pcm_buffer.begin(), sys->pcm_buffer.begin() + CHUNK_SAMPLES);
                sys->pcm_buffer.erase(sys->pcm_buffer.begin(), sys->pcm_buffer.begin() + CHUNK_SAMPLES);
            }
        }

        if (samples.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        msg_Dbg(p_filter, "Iniciando inferencia Whisper (bloque de %d samples)", (int)samples.size());
        
        whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wp.language = "es";

        if (whisper_full(sys->ctx, wp, samples.data(), (int)samples.size()) == 0) {
            const int n = whisper_full_n_segments(sys->ctx);
            msg_Dbg(p_filter, "Inferencia completada: %d segmentos encontrados", n);
            
            std::string result;
            result.reserve(256);

            for (int i = 0; i < n; ++i) {
                const char* text = whisper_full_get_segment_text(sys->ctx, i);
                if (text) result += text;
            }

            // Publicamos lo último
            if (!result.empty()) {
                std::lock_guard<std::mutex> lock(g_state.lock);
                g_state.current_text = result;
                g_state.last_update = mdate();
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Sub-Source Renderer Implementation
// -----------------------------------------------------------------------------
static subpicture_t *FilterRender(filter_t *, mtime_t);

static int OpenRender(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    p_filter->pf_sub_source = FilterRender; // Estilo 3.0.x: asignación directa
    return VLC_SUCCESS;
}

static void CloseRender(vlc_object_t *obj)
{
    (void)obj;
}

static subpicture_t *FilterRender(filter_t *p_filter, mtime_t display_date)
{
    std::string text;
    mtime_t last;

    {
        std::lock_guard<std::mutex> lock(g_state.lock);
        text = g_state.current_text;
        last = g_state.last_update;
    }

    if (text.empty() || last == 0 || (mdate() - last > 3 * CLOCK_FREQ))
        return NULL;

    msg_Dbg(p_filter, "Renderer: Desplegando subtítulo: [%s]", text.c_str());

    subpicture_t *p_spu = filter_NewSubpicture(p_filter);
    if (!p_spu) return NULL;

    video_format_t fmt;
    video_format_Init(&fmt, VLC_CODEC_TEXT);
    subpicture_region_t *p_region = subpicture_region_New(&fmt);
    video_format_Clean(&fmt);

    if (!p_region) {
        subpicture_Delete(p_spu);
        return NULL;
    }

    p_region->p_text = text_segment_New(text.c_str());
    if (!p_region->p_text) {
        subpicture_region_Delete(p_region);
        subpicture_Delete(p_spu);
        return NULL;
    }

    p_spu->p_region = p_region;
    p_spu->i_start = display_date;
    p_spu->i_stop = display_date + 2 * CLOCK_FREQ;
    p_spu->b_ephemer = true;
    p_spu->b_absolute = false;

    return p_spu;
}
