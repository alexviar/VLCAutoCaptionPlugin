
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

#ifndef MODULE_STRING
# define MODULE_STRING "whisper_subs"
#endif

#ifndef N_
# define N_(str) (str)
#endif

struct filter_sys_t {
    std::vector<float> pcm_buffer; 
};

extern "C" {
    static int  OpenAudio (vlc_object_t *);
    static void CloseAudio(vlc_object_t *);
}

vlc_module_begin ()
    set_description(N_("Whisper Audio-to-Text (Audio Filter)"))
    set_shortname(N_("Whisper ASR"))
    set_capability("audio filter", 0)
    set_callbacks(OpenAudio, CloseAudio)
vlc_module_end ()

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

    // Límite de seguridad: 10 segundos (160k samples a 16kHz) para evitar OOM
    if (p_sys->pcm_buffer.size() > 160000) {
        p_sys->pcm_buffer.erase(p_sys->pcm_buffer.begin(), 
                                p_sys->pcm_buffer.begin() + p_block->i_nb_samples);
    }

    p_sys->pcm_buffer.reserve(p_sys->pcm_buffer.size() + p_block->i_nb_samples);

    for (size_t i = 0; i < p_block->i_nb_samples; ++i) {
        p_sys->pcm_buffer.push_back(p_samples[i * ch]); // Canal 0
    }

    // DEBUG: Should remove it
    static int log_counter = 0;
    if (++log_counter % 500 == 0) {
        msg_Info(p_filter, "Buffer Whisper: %zu muestras acumuladas", p_sys->pcm_buffer.size());
    }

    return p_block;
}

static int OpenAudio(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    
    msg_Info(p_filter, "Inicializando p_sys de prueba...");

    filter_sys_t *p_sys = new(std::nothrow) filter_sys_t();
    if (!p_sys) return VLC_ENOMEM;

    p_filter->p_sys = p_sys;
    p_filter->pf_audio_filter = ProcessAudio;
    
    return VLC_SUCCESS;
}

static void CloseAudio(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    filter_sys_t *p_sys = (filter_sys_t *)p_filter->p_sys;
    
    if (p_sys) {
        msg_Info(p_filter, "Liberando p_sys de prueba.");
        delete p_sys; 
        p_filter->p_sys = NULL;
    }
}
