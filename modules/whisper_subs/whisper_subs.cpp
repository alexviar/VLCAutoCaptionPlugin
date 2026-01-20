/**
 * whisper_subs.cpp - PASO 2: Memoria Dinámica y Logs
 * 
 * Verificamos que podemos asignar p_sys y emitir logs sin inestabilidad.
 */

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

#ifndef MODULE_STRING
# define MODULE_STRING "whisper_subs"
#endif

// Estructura de sistema simple
struct filter_sys_t {
    int block_count;
};

extern "C" {
    static int  OpenAudio (vlc_object_t *);
    static void CloseAudio(vlc_object_t *);
}

vlc_module_begin ()
    set_description("Whisper Step 2: Logs & System")
    set_capability("audio filter", 0)
    set_callbacks(OpenAudio, CloseAudio)
vlc_module_end ()

static block_t *ProcessAudio(filter_t *p_filter, block_t *p_block)
{
    filter_sys_t *p_sys = (filter_sys_t *)p_filter->p_sys;
    
    if (p_sys) {
        p_sys->block_count++;
        // Logueamos solo una vez cada 1000 bloques para confirmar actividad
        if (p_sys->block_count % 1000 == 0) {
            msg_Info(p_filter, "Actividad de filtro Whisper: %d bloques recibidos", p_sys->block_count);
        }
    }

    return p_block; // Passthrough
}

static int OpenAudio(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    
    msg_Info(p_filter, "Inicializando p_sys de prueba...");

    filter_sys_t *p_sys = (filter_sys_t *)malloc(sizeof(filter_sys_t));
    if (!p_sys) return VLC_ENOMEM;

    p_sys->block_count = 0;
    p_filter->p_sys = p_sys;
    p_filter->pf_audio_filter = ProcessAudio;
    
    return VLC_SUCCESS;
}

static void CloseAudio(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    if (p_filter->p_sys) {
        msg_Info(p_filter, "Liberando p_sys de prueba.");
        free(p_filter->p_sys);
        p_filter->p_sys = NULL;
    }
}
