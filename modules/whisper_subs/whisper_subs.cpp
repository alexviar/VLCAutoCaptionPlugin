
#ifdef _WIN32
# include <basetsd.h>
typedef SSIZE_T ssize_t;
# include <winsock2.h>
# ifndef poll
#  define poll WSAPoll
# endif
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_block.h>

#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <string>
#include "whisper.h"

#ifndef MODULE_STRING
# define MODULE_STRING "whisper_subs"
#endif

#ifndef N_
# define N_(str) (str)
#endif

struct shared_state_t {
    std::mutex lock;
    std::string current_text;
    mtime_t last_update = 0;
};
static shared_state_t g_state;

struct filter_sys_t {
    whisper_context *ctx = nullptr;
    std::vector<float> pcm_buffer; 
    std::mutex buffer_mutex;
    std::thread worker_thread;
    bool running;
    std::string language;
    bool translate;
};

extern "C" {
    static int  OpenAudio (vlc_object_t *);
    static void CloseAudio(vlc_object_t *);
}

vlc_module_begin ()
    set_description(N_("Whisper Audio-to-Text (Audio Filter)"))
    set_shortname(N_("Whisper ASR"))
    set_capability("audio filter", 0)
    set_category(CAT_AUDIO)
    set_subcategory(SUBCAT_AUDIO_AFILTER)
    set_callbacks(OpenAudio, CloseAudio)
    add_string("whisper-model", "ggml-base.bin", N_("Model path"), NULL, false)
    add_string("whisper-language", "auto", N_("Inference language"), N_("ISO 639-1 language code (e.g. 'es', 'en', 'fr') or 'auto'"), false)
    add_bool("whisper-translate", false, N_("Translate to English"), N_("Translate the transcribed text to English"), false)
    add_bool("whisper-use-gpu", true, N_("Use GPU"), N_("Use GPU for inference if available"), false)
    add_bool("whisper-flash-attn", false, N_("Flash Attention"), N_("Use Flash Attention (speeds up inference, requires compatible GPU)"), false)
vlc_module_end ()

static void WhisperWorker(filter_t *);

static block_t *ProcessAudio(filter_t *p_filter, block_t *p_block)
{
    filter_sys_t *p_sys = (filter_sys_t *)p_filter->p_sys;
    if (!p_sys || !p_block) return p_block;

    if (p_filter->fmt_in.i_codec != VLC_CODEC_FL32)
        return p_block;

    const float *p_samples = (const float *)p_block->p_buffer;
    const unsigned ch = p_filter->fmt_in.audio.i_channels;

    if (ch == 0 || p_block->i_nb_samples == 0)
        return p_block;

    // Mutex para evitar crash al leer desde el hilo
    std::lock_guard<std::mutex> lock(p_sys->buffer_mutex);

    // Límite de seguridad: 10 segundos de audio (basado en el rate de entrada)
    // size_t max_samples = p_filter->fmt_in.audio.i_rate * 10;
    // if (p_sys->pcm_buffer.size() + p_block->i_nb_samples > max_samples) {
    //     size_t overflow = p_sys->pcm_buffer.size() + p_block->i_nb_samples - max_samples;
    //     p_sys->pcm_buffer.erase(p_sys->pcm_buffer.begin(), 
    //                             p_sys->pcm_buffer.begin() + overflow);
    // }

    // p_sys->pcm_buffer.reserve(max_samples);

    for (size_t i = 0; i < p_block->i_nb_samples; ++i) {
        p_sys->pcm_buffer.push_back(p_samples[i * ch]); // Canal 0
    }

    // DEBUG: Should remove it
    // static int log_counter = 0;
    // if (++log_counter % 500 == 0) {
    //     msg_Info(p_filter, "Buffer Whisper: %zu muestras acumuladas", p_sys->pcm_buffer.size());
    // }

    return p_block;
}

static void WhisperWorker(filter_t *p_filter)
{
    filter_sys_t *p_sys = (filter_sys_t *)p_filter->p_sys;
    const size_t CHUNK_SAMPLES = p_filter->fmt_in.audio.i_rate * 3;

    msg_Info(p_filter, "Hilo de Whisper iniciado.");

    while (p_sys->running) {
        std::vector<float> samples;

        {
            std::lock_guard<std::mutex> lock(p_sys->buffer_mutex);
            if (p_sys->pcm_buffer.size() >= CHUNK_SAMPLES) {
                samples.assign(p_sys->pcm_buffer.begin(), p_sys->pcm_buffer.end());
                p_sys->pcm_buffer.erase(p_sys->pcm_buffer.begin(), p_sys->pcm_buffer.end() - p_filter->fmt_in.audio.i_rate * 1);
            }
        }

        if (samples.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // msg_Info(p_filter, "Buffer OK (bloque de %zu), resampleando e iniciando inferencia...", samples.size());

        whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wp.language = p_sys->language.c_str();
        wp.translate = p_sys->translate;

        // Resampling a 16kHz (Requerido por Whisper)
        std::vector<float> samples16;
        const double scale = 16000.0 / p_filter->fmt_in.audio.i_rate;
        const size_t n_out = (size_t)(samples.size() * scale);
        samples16.reserve(n_out);

        for (size_t i = 0; i < n_out; i++) {
            size_t src_idx = (size_t)(i / scale);
            if (src_idx < samples.size()) {
                samples16.push_back(samples[src_idx]);
            }
        }
        
        if (whisper_full(p_sys->ctx, wp, samples16.data(), (int)samples16.size()) == 0) {
            const int n = whisper_full_n_segments(p_sys->ctx);
            std::string result;
            for (int i = 0; i < n; ++i) {
                const char* text = whisper_full_get_segment_text(p_sys->ctx, i);
                if (text) result += text;
            }

            if (!result.empty()) {
                msg_Info(p_filter, "Whisper: %s", result.c_str());
                // std::lock_guard<std::mutex> lock(g_state.lock);
                // g_state.current_text = result;
                // g_state.last_update = mdate();
            }
        }
    }

    msg_Info(p_filter, "Hilo de Whisper terminando.");
}

static int OpenAudio(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    
    msg_Info(p_filter, "Inicializando p_sys de prueba...");

    filter_sys_t *p_sys = new(std::nothrow) filter_sys_t();
    if (!p_sys) return VLC_ENOMEM;

    p_filter->p_sys = p_sys;
    p_filter->pf_audio_filter = ProcessAudio;
    msg_Info(p_filter, "Formato de entrada: %d Hz, %d canales", 
         p_filter->fmt_in.audio.i_rate, p_filter->fmt_in.audio.i_channels);

    // if (p_filter->fmt_in.audio.i_rate != 16000) {
    //     msg_Err(p_filter, "Whisper requiere 16000Hz, pero el stream es de %dHz", p_filter->fmt_in.audio.i_rate);
    //     delete p_sys;
    //     p_filter->p_sys = NULL;
    //     return VLC_EGENERIC;
    // }

    char *psz = var_InheritString(p_filter, "whisper-model");
    const char *model_path = psz ? psz : "ggml-base.bin";
    
    char *psz_lang = var_InheritString(p_filter, "whisper-language");
    p_sys->language = psz_lang ? psz_lang : "auto";
    // free(psz_lang); Cross-Heap Allocation Issue caused by mismatched compilers (msvc vs gcc)

    p_sys->translate = var_InheritBool(p_filter, "whisper-translate");
    bool use_gpu = var_InheritBool(p_filter, "whisper-use-gpu");
    bool flash_attn = var_InheritBool(p_filter, "whisper-flash-attn");

    msg_Info(p_filter, "Cargando modelo: %s (Idioma: %s, Traducción: %s, GPU: %s, FlashAttn: %s)", 
             model_path, p_sys->language.c_str(), p_sys->translate ? "SÍ" : "NO",
             use_gpu ? "SÍ" : "NO", flash_attn ? "SÍ" : "NO");

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = use_gpu;
    cparams.flash_attn = flash_attn;

    p_sys->ctx = whisper_init_from_file_with_params(model_path, cparams);
    // free(psz); Cross-Heap Allocation Issue caused by mismatched compilers (msvc vs gcc)

    if (!p_sys->ctx) {
        msg_Err(p_filter, "Error cargando Whisper");
        delete p_sys;
        return VLC_EGENERIC;
    }
    
    p_sys->running = true;
    p_sys->worker_thread = std::thread(WhisperWorker, p_filter);

    return VLC_SUCCESS;
}

static void CloseAudio(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    filter_sys_t *p_sys = (filter_sys_t *)p_filter->p_sys;
    
    if (p_sys) {
        msg_Info(p_filter, "Deteniendo hilo de Whisper...");
        p_sys->running = false;
        if (p_sys->worker_thread.joinable())
            p_sys->worker_thread.join();

        if (p_sys->ctx)
            whisper_free(p_sys->ctx);

        msg_Info(p_filter, "Liberando p_sys de prueba.");
        delete p_sys; 
        p_filter->p_sys = NULL;
    }
}
