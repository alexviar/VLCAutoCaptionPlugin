/**
 * whisper_subs.cpp - PRUEBA DE ESTABILIDAD MÍNIMA
 * 
 * Este archivo ha sido reducido al mínimo absoluto para diagnosticar crashes en el playback.
 * - Sin objetos globales.
 * - Sin hilos.
 * - Sin C++ Standard Library (vector, string, etc).
 * - Sin dependencias externas (whisper).
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

extern "C" {
    static int  OpenAudio (vlc_object_t *);
    static void CloseAudio(vlc_object_t *);
}

vlc_module_begin ()
    set_description("Whisper Stability Test")
    set_capability("audio filter", 0)
    set_callbacks(OpenAudio, CloseAudio)
vlc_module_end ()

// Procesado passthrough puro (no debería crashear bajo ninguna circunstancia)
static block_t *ProcessAudio(filter_t *p_filter, block_t *p_block)
{
    (void)p_filter;
    return p_block;
}

static int OpenAudio(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    
    // No reservamos memoria, solo asignamos el callback
    p_filter->p_sys = NULL; 
    p_filter->pf_audio_filter = ProcessAudio;
    
    return VLC_SUCCESS;
}

static void CloseAudio(vlc_object_t *obj)
{
    (void)obj;
}
