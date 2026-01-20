/**
 * whisper_subs.cpp
 * VLC Audio Filter Module using OpenAI Whisper for real-time subtitling
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef _WIN32
# include <basetsd.h>
typedef SSIZE_T ssize_t;
# include <winsock2.h>
# define poll WSAPoll
#endif

#ifndef __PLUGIN__
# define __PLUGIN__
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_filter.h>
#include <vlc_vout.h>
#include <vlc_vout_osd.h>

// Fallback for VLC 3.0 macros if hidden or missing in MSVC
#ifndef VLC_OBJECT_INPUT
# define VLC_OBJECT_INPUT 6
#endif
#ifndef FIND_ANYWHERE
# define FIND_ANYWHERE 0x0001
#endif
#ifndef N_
# define N_(str) (str)
#endif
#ifndef MODULE_STRING
# define MODULE_STRING "whisper_subs"
#endif

// Manual declarations to avoid circular header issues in MSVC
typedef struct input_thread_t input_thread_t;
VLC_API vout_thread_t *input_GetVout(input_thread_t *);
VLC_API void *vlc_object_find(vlc_object_t *, int, int);
VLC_API void vlc_object_release(void *);

#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include "whisper.h"

// -----------------------------------------------------------------------------
// Module Description
// -----------------------------------------------------------------------------
static int  Open (vlc_object_t *);
static void Close(vlc_object_t *);

#define MODEL_PATH_TEXT N_("Path to Whisper Model")
#define MODEL_PATH_LONGTEXT N_("Full path to the ggml whisper model binary (e.g. ggml-base.bin).")

vlc_module_begin ()
    set_description(N_("Whisper Subtitles Audio Filter"))
    set_shortname(N_("Whisper Subs"))
    set_capability("audio filter", 0)
    set_category(CAT_AUDIO)
    set_subcategory(SUBCAT_AUDIO_AFILTER)
    
    add_string("whisper-model-path", "ggml-base.bin", MODEL_PATH_TEXT, MODEL_PATH_LONGTEXT, false)

    set_callbacks(Open, Close)
vlc_module_end ()

// -----------------------------------------------------------------------------
// Internal Data Structures
// -----------------------------------------------------------------------------

struct filter_sys_t
{
    // Whisper State
    struct whisper_context *ctx;
    struct whisper_full_params wparams;
    
    // Audio Buffering
    std::vector<float> pcm_buffer;
    std::mutex buffer_mutex;
    
    // Threading
    std::thread worker_thread;
    bool running;
    
    // VLC references
    vlc_object_t *obj;
    std::string model_path;
};

// -----------------------------------------------------------------------------
// Forward Declarations
// -----------------------------------------------------------------------------
static block_t *Process(filter_t *, block_t *);
static void WorkerLoop(filter_t *p_filter);

// -----------------------------------------------------------------------------
// Open: Initialize Module
// -----------------------------------------------------------------------------
static int Open(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    
    // Allocate system structure
    filter_sys_t *sys = new(std::nothrow) filter_sys_t();
    if (!sys)
        return VLC_ENOMEM;
    
    p_filter->p_sys = sys;
    sys->obj = obj;
    p_filter->fmt_in.audio.i_format = VLC_CODEC_FL32;
    p_filter->fmt_out.audio = p_filter->fmt_in.audio;
    p_filter->pf_audio_filter = Process;

    // Load Model Path configuration
    char *psz_model = var_InheritString(p_filter, "whisper-model-path");
    if (psz_model) {
        sys->model_path = psz_model;
        free(psz_model);
    } else {
        sys->model_path = "ggml-base.bin"; // Default fallback
    }

    // Initialize Whisper Context
    struct whisper_context_params cparams = whisper_context_default_params();
    sys->ctx = whisper_init_from_file_with_params(sys->model_path.c_str(), cparams);
    if (!sys->ctx) {
        msg_Err(p_filter, "Failed to initialize Whisper context from '%s'", sys->model_path.c_str());
        delete sys;
        return VLC_EGENERIC;
    }

    // Start Worker Thread
    sys->running = true;
    sys->worker_thread = std::thread(WorkerLoop, p_filter);

    msg_Info(p_filter, "Whisper Subs module initialized successfully.");
    return VLC_SUCCESS;
}

// -----------------------------------------------------------------------------
// Close: Cleanup
// -----------------------------------------------------------------------------
static void Close(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    filter_sys_t *sys = p_filter->p_sys;

    // Stop worker thread
    sys->running = false;
    if (sys->worker_thread.joinable()) {
        sys->worker_thread.join();
    }

    if (sys->ctx) {
        whisper_free(sys->ctx);
    }

    delete sys;
}

// -----------------------------------------------------------------------------
// Process: Audio Callback
// -----------------------------------------------------------------------------
static block_t *Process(filter_t *p_filter, block_t *p_block)
{
    if (!p_block) return NULL;

    filter_sys_t *sys = p_filter->p_sys;

    float *p_samples = (float *)p_block->p_buffer;

    {
        std::lock_guard<std::mutex> lock(sys->buffer_mutex);
        for (size_t i = 0; i < p_block->i_nb_samples; ++i) {
            // Grab the first channel of the frame (usually Left)
            float sample = p_samples[i * p_filter->fmt_in.audio.i_channels]; 
            sys->pcm_buffer.push_back(sample);
        }
    }

    return p_block; // Pass block through untouched
}

// -----------------------------------------------------------------------------
// WorkerLoop: Whisper Inference
// -----------------------------------------------------------------------------
static void WorkerLoop(filter_t *p_filter)
{
    filter_sys_t *sys = p_filter->p_sys;
    const int SAMPLE_RATE = 16000;
    const size_t MIN_DURATION_SEC = 3; 
    const size_t MIN_SAMPLES = SAMPLE_RATE * MIN_DURATION_SEC;

    while (sys->running) {
        std::vector<float> process_buffer;

        // check buffer size
        {
            std::lock_guard<std::mutex> lock(sys->buffer_mutex);
            if (sys->pcm_buffer.size() >= MIN_SAMPLES) {
                // Take content to process
                process_buffer = sys->pcm_buffer;
                sys->pcm_buffer.clear(); 
            }
        }

        if (process_buffer.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Run Inference
        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_progress = false;
        wparams.print_special = false;
        wparams.print_realtime = false;
        wparams.print_timestamps = false;
        wparams.translate = false;
        wparams.language = "es"; 

        if (whisper_full(sys->ctx, wparams, process_buffer.data(), (int)process_buffer.size()) != 0) {
            msg_Err(p_filter, "Failed to process audio with Whisper");
            continue;
        }

        // Get Text
        int n_segments = whisper_full_n_segments(sys->ctx);
        for (int i = 0; i < n_segments; ++i) {
            const char *text = whisper_full_get_segment_text(sys->ctx, i);
            if (text && text[0] != '\0') {
                msg_Dbg(p_filter, "Whisper: %s", text);
                
                // Show on OSD
                // Finding the input object to get the VOUT
                vlc_object_t *p_input = (vlc_object_t *)vlc_object_find(p_filter, VLC_OBJECT_INPUT, FIND_ANYWHERE);
                if (p_input) {
                    vout_thread_t *p_vout = input_GetVout((input_thread_t *)p_input);
                    if (p_vout) {
                        // Channel 1 is usually the default OSD channel
                        vout_OSDMessage(p_vout, 1, "%s", text);
                        vlc_object_release(p_vout);
                    }
                    vlc_object_release(p_input);
                }
            }
        }
    }
}
