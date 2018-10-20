#include "config.h"

#include <windows.h>

#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavutil/opt.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"
#include "libavutil/mem.h"
#include "avdevice.h"

#define COBJMACROS

#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#define MAX_OUTPUTS 16
#define MAX_OUTPUTS 16

/** TODO
rewrite mondup to use multiple regions (for multiple monitors)
handle dpi? may not be needed (AFAIK)
handle rotation! Will be a pain, need D3D rendering
draw mouse etc
*/
struct capture_region {
    IDXGIOutput1 *dxgi_output;
    DXGI_OUTPUT_DESC description;

    IDXGIOutputDuplication *output_duplication;
    IDXGIResource *duplicated_resource;
    DXGI_OUTDUPL_FRAME_INFO frame_info;

    int metadata_size;
    void *metadata;

    // Region dimensions
    int width;
    int height;

    // Offset from source monitor
    int src_x;
    int src_y;

    // Offset to output image
    int dst_x;
    int dst_y;
};

/**
 * GDI Device Demuxer context
 */
struct mondup_ctx {
    const AVClass *class;   /**< Class for private options */

    int        draw_mouse;  /**< Draw mouse cursor (private option) */
    // int        show_region; /**< Draw border (private option) */
    AVRational framerate;   /**< Capture framerate (private option) */
    AVRational time_base;   /**< Capture framerate (private option) */

    int        width;       /**< Widh of the grab frame (private option) */
    int        height;      /**< Height of the grab frame (private option) */
    int        offset_x;    /**< Capture x offset (private option) */
    int        offset_y;    /**< Capture y offset (private option) */

    ID3D11Device *d3d11_device;
    ID3D11DeviceContext *d3d11_context;
    ID3D11Texture2D *duplicator_texture;
    D3D11_MAPPED_SUBRESOURCE mapped_texture;

    int capture_region_count;
    struct capture_region *capture_regions;

    int out_width;
    int out_height;
    int out_size;

    // Mouse stuff
    LARGE_INTEGER last_mouse_update;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO mouse_shape;
    int mouse_x;
    int mouse_y;
    int mouse_visible;
    int mouse_shape_size;
    void *mouse_shape_buffer;

    // av_gettime_relative() timestamp
    int64_t start_time;
    // relative from start_time
    int64_t next_frame;
    int64_t previous_frame;

    int want_frame;

    int output_frame_index;
    void *frame_data[2];
    // void *frame_data;

    pthread_mutex_t frame_lock;
    pthread_t thread;
    int thread_alive;

    int64_t last_end;

    int64_t grab_delay;
    int64_t copy_delay;
    int64_t frame_delay;
    int do_log;

};

struct output_info {
    int adapter_index;
    int output_index;
    DXGI_OUTPUT_DESC description;
    MONITORINFO info;
};

// struct Vertex {
//     XMFLOAT3 pos;
//     XMFLOAT2 uv;
// };

// static const char[] PIXEL_SHADER = \



#define ERR_LOG_GOTO(ERROR_VAL, ...) do {         \
    av_log(s1, AV_LOG_ERROR, __VA_ARGS__);        \
    ret = ERROR_VAL;                              \
    goto error;                                   \
} while(0)
#define IF_ERR_LOG_GOTO(CHECK, ERROR_VAL, ...) if (CHECK) ERR_LOG_GOTO(ERROR_VAL, __VA_ARGS__);

#define RECTS_OVERLAP(A, B)  ( ((A.left >= B.left && A.left <= B.right) || (A.right >= B.left && A.right <= B.right)) && ((A.top >= B.top && A.top <= B.bottom) || (A.bottom >= B.top && A.bottom <= B.bottom)) )
#define RECT_CONTAINED(A, B) ( ((A.left >= B.left && A.left <= B.right) && (A.right >= B.left && A.right <= B.right)) && ((A.top >= B.top && A.top <= B.bottom) && (A.bottom >= B.top && A.bottom <= B.bottom)) )

static int mondup_handle_mouse(AVFormatContext *s1, struct capture_region *cap_reg) {
    struct mondup_ctx *mondup_state = s1->priv_data;

    HRESULT hr;

    if (cap_reg->frame_info.LastMouseUpdateTime.QuadPart <= mondup_state->last_mouse_update.QuadPart) {
    // if (cap_reg->frame_info.LastMouseUpdateTime.QuadPart == 0) {
        // No mouse update or already updated
        return 0;
    }

    // cap_reg->frame_info.PointerPosition.Position.x
    // cap_reg->frame_info.PointerPosition.Position.y

    // mondup_state->mouse_x = cap_reg->src_x

    mondup_state->last_mouse_update = cap_reg->frame_info.LastMouseUpdateTime;
    mondup_state->mouse_x = cap_reg->frame_info.PointerPosition.Position.x - cap_reg->src_x + cap_reg->dst_x;
    mondup_state->mouse_y = cap_reg->frame_info.PointerPosition.Position.y - cap_reg->src_y + cap_reg->dst_y;
    mondup_state->mouse_visible = cap_reg->frame_info.PointerPosition.Visible ? 1 : 0;

    if (cap_reg->frame_info.PointerShapeBufferSize > 0) {
        if (cap_reg->frame_info.PointerShapeBufferSize > mondup_state->mouse_shape_size) {
            if (mondup_state->mouse_shape_buffer) {
                av_freep(&mondup_state->mouse_shape_buffer);
            }
            mondup_state->mouse_shape_size = cap_reg->frame_info.PointerShapeBufferSize;
            mondup_state->mouse_shape_buffer = av_malloc(mondup_state->mouse_shape_size);
        }
        int mouse_shape_size;
        hr = IDXGIOutputDuplication_GetFramePointerShape(cap_reg->output_duplication,
            mondup_state->mouse_shape_size,
            mondup_state->mouse_shape_buffer,
            &mouse_shape_size,
            &mondup_state->mouse_shape
        );
        if (hr) {
            av_log(s1, AV_LOG_ERROR, "Failed to get mouse pointer shape (%lx).\n", hr);
            return hr;
        }
    }

    return 0;
}

static int mondup_get_frame(AVFormatContext *s1, struct capture_region *cap_reg, int timeout) {
    struct mondup_ctx *mondup_state = s1->priv_data;

    HRESULT hr;
    int ret;
    int move_rect_count;
    int dirty_rect_count;
    int move_rects_size;
    int dirty_rects_size;

    int cap_is_dirty = 0;


    D3D11_BOX src_box;
    ID3D11Texture2D *frame_texture = NULL;

    // if (mondup_state->duplicated_resource) {
    //     // Release held frame
    //     IDXGIResource_Release(mondup_state->duplicated_resource);
    //     mondup_state->duplicated_resource = NULL;
    //     IDXGIOutputDuplication_ReleaseFrame(mondup_state->output_duplication);
    // }

    // Ask for a new frame
    hr = IDXGIOutputDuplication_AcquireNextFrame(cap_reg->output_duplication, timeout,
                                                 &cap_reg->frame_info, &cap_reg->duplicated_resource);
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        av_log(s1, AV_LOG_ERROR, "Lost access to display.\n");
        return AVERROR(EIO);
    } else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame
        return 0;
    } else if (hr) {
        av_log(s1, AV_LOG_ERROR, "duplicator failed (%lx).\n", hr);
        return AVERROR(EIO);
    }

    int rasdas = mondup_handle_mouse(s1, cap_reg);
    if (rasdas) {
        return rasdas;
    }

    // av_log(s1, AV_LOG_ERROR, "frame_info %p (%ld):\n"
    //                          " LastPresentTime: %lld LastMouseUpdateTime: %lld\n"
    //                          " Frames: %u\n"
    //                          " Rects: %d Masked: %d\n"
    //                          " Pointer: %ld,%ld (vis:%d)\n"
    //                          " Metadata size: %u Pointer size: %u\n",
    //     (void *) &cap_reg->frame_info, hr,
    //     cap_reg->frame_info.LastPresentTime.QuadPart, cap_reg->frame_info.LastMouseUpdateTime.QuadPart,
    //     cap_reg->frame_info.AccumulatedFrames,
    //     cap_reg->frame_info.RectsCoalesced, cap_reg->frame_info.ProtectedContentMaskedOut,
    //     cap_reg->frame_info.PointerPosition.Position.x, cap_reg->frame_info.PointerPosition.Position.y, cap_reg->frame_info.PointerPosition.Visible,
    //     cap_reg->frame_info.TotalMetadataBufferSize, cap_reg->frame_info.PointerShapeBufferSize);

    // Get the texture from the duplicated resource
    hr = IDXGIResource_QueryInterface(cap_reg->duplicated_resource, &IID_ID3D11Texture2D,
                                      (void **)&frame_texture);
    if (hr) {
        av_log(s1, AV_LOG_ERROR, "Unable to attain texture (%lx).\n", hr);
        IDXGIOutputDuplication_ReleaseFrame(cap_reg->output_duplication);
        cap_reg->duplicated_resource = NULL;
        return AVERROR(EIO);
    }

    if (cap_reg->frame_info.TotalMetadataBufferSize) {
        if (cap_reg->frame_info.TotalMetadataBufferSize > cap_reg->metadata_size) {
            if (cap_reg->metadata) {
                av_freep(&cap_reg->metadata);
            }
            cap_reg->metadata_size = cap_reg->frame_info.TotalMetadataBufferSize;
            cap_reg->metadata = av_malloc(cap_reg->metadata_size);
        }
        // RECT src_rect = {
        //     .left = cap_reg->src_x,
        //     .top = cap_reg->src_y,
        //     .right = cap_reg->src_x + cap_reg->width,
        //     .bottom = cap_reg->src_y + cap_reg->height
        // };
        // av_log(s1, AV_LOG_ERROR, "src: %d,%d %d,%d \n",
            // src_rect.left, src_rect.top, src_rect.right, src_rect.bottom);

        hr = IDXGIOutputDuplication_GetFrameMoveRects(cap_reg->output_duplication,
            cap_reg->metadata_size, cap_reg->metadata, &move_rects_size);
        if (hr) {
            av_log(s1, AV_LOG_ERROR, "IDXGIOutputDuplication_GetFrameMoveRects (%lx)\n", hr);
            return AVERROR(EIO);
        }
        DXGI_OUTDUPL_MOVE_RECT *move_rects = (DXGI_OUTDUPL_MOVE_RECT *) cap_reg->metadata;
        move_rect_count = move_rects_size / sizeof(DXGI_OUTDUPL_MOVE_RECT);

        // for (int i = 0; i < move_rect_count; ++i) {
        //     DXGI_OUTDUPL_MOVE_RECT move_rect = move_rects[i];

        //     // av_log(s1, AV_LOG_ERROR, "move: %d,%d %d,%d \n",
        //     // move_rect.DestinationRect.left, move_rect.DestinationRect.top, move_rect.DestinationRect.right, move_rect.DestinationRect.bottom);

        //     // if ( RECTS_OVERLAP(src_rect, move_rect.DestinationRect) ) {
        //     //     cap_is_dirty = 1;
        //     // }

        //     // src_box = (D3D11_BOX) {
        //     //     .left   = move_rect.SourcePoint.x,
        //     //     .top    = move_rect.SourcePoint.y,
        //     //     .front  = 0,
        //     //     .right  = move_rect.SourcePoint.x + (move_rect.DestinationRect.right - move_rect.DestinationRect.left),
        //     //     .bottom  = move_rect.SourcePoint.y + (move_rect.DestinationRect.bottom - move_rect.DestinationRect.top),
        //     //     .back = 1,
        //     // };
        //     // ID3D11DeviceContext_CopySubresourceRegion(
        //     //     mondup_state->d3d11_context,
        //     //     (ID3D11Resource *) mondup_state->duplicator_texture, 0,
        //     //     move_rect.DestinationRect.left, move_rect.DestinationRect.top, 0,
        //     //     (ID3D11Resource *) mondup_state->duplicator_texture, 0, &src_box);
        // }

        int dirty_rects_size;
        hr = IDXGIOutputDuplication_GetFrameDirtyRects (cap_reg->output_duplication,
            cap_reg->metadata_size, cap_reg->metadata, &dirty_rects_size);
        if (hr) {
            av_log(s1, AV_LOG_ERROR, "IDXGIOutputDuplication_GetFrameDirtyRects  (%lx)\n", hr);
            return AVERROR(EIO);
        }
        RECT *dirty_rects = (RECT *) cap_reg->metadata;
        dirty_rect_count = dirty_rects_size / sizeof(RECT);
        // for (int i = 0; i < dirty_rect_count; ++i) {
        //     RECT dirty_rect = dirty_rects[i];

        //     // av_log(s1, AV_LOG_ERROR, "move: %d,%d %d,%d \n",
        //     // dirty_rect.left, dirty_rect.top, dirty_rect.right, dirty_rect.bottom);

        //     // if ( RECTS_OVERLAP(src_rect, dirty_rect) ) {
        //     //     cap_is_dirty = 1;
        //     // }

        //     // src_box = (D3D11_BOX) {
        //     //     .left   = dirty_rect.left,
        //     //     .top    = dirty_rect.top,
        //     //     .front  = 0,
        //     //     .right  = dirty_rect.right,
        //     //     .bottom  = dirty_rect.bottom,
        //     //     .back = 1,
        //     // };

        //     // ID3D11DeviceContext_CopySubresourceRegion(
        //     //     mondup_state->d3d11_context,
        //     //     (ID3D11Resource *) mondup_state->duplicator_texture, 0,
        //     //     dirty_rect.left, dirty_rect.top, 0,
        //     //     (ID3D11Resource *) frame_texture, 0, &src_box);
        // }

    }

    // av_log(s1, AV_LOG_ERROR, "cap_is_dirty: %d\n", cap_is_dirty);
    if ( move_rect_count || dirty_rect_count ) {
        // Copy relevant part to our staging texture
        src_box = (D3D11_BOX) {
            .left   = cap_reg->src_x,
            .top    = cap_reg->src_y,
            .front  = 0,
            .right  = cap_reg->src_x + cap_reg->width,
            .bottom = cap_reg->src_y + cap_reg->height,
            .back = 1,
        };
        // av_log(s1, AV_LOG_ERROR, "%d,%d:%d,%d to %d,%d:%d,%d\n",
        //     src_box.left, src_box.top, src_box.right, src_box.bottom,
        //     cap_reg->dst_x, cap_reg->dst_y, cap_reg->dst_x + cap_reg->width, cap_reg->dst_y + cap_reg->height);

        ID3D11DeviceContext_CopySubresourceRegion(
            mondup_state->d3d11_context,
            (ID3D11Resource *) mondup_state->duplicator_texture,
            0, cap_reg->dst_x, cap_reg->dst_y, 0,
            (ID3D11Resource *) frame_texture, 0, &src_box);
        // ID3D11DeviceContext_Flush(mondup_state->d3d11_context);
    }

    // Release texture
    ID3D11Texture2D_Release(frame_texture);
    frame_texture = NULL;
    IDXGIResource_Release(cap_reg->duplicated_resource);
    cap_reg->duplicated_resource = NULL;



    IDXGIOutputDuplication_ReleaseFrame(cap_reg->output_duplication);
    return 1;
}

static void draw_mouse(AVFormatContext *s1, uint32_t *target_pixels);

static int mondup_render_frame(AVFormatContext *s1) {
    struct mondup_ctx *mondup_state = s1->priv_data;

    int get_frame_res;
    HRESULT hr;

    int data_changed = 0;
    LARGE_INTEGER current_mouse_update = mondup_state->last_mouse_update;
    // int get_frame_res = 0;
    // HRESULT hr;

    int64_t grab_start = av_gettime_relative();
    for (int i = 0; i < mondup_state->capture_region_count; ++i) {
        struct capture_region *cap_reg = (mondup_state->capture_regions + i);

        get_frame_res = mondup_get_frame(s1, cap_reg, 0);
        if (get_frame_res < 0) {
            return get_frame_res;
        } else if (get_frame_res > 0) {
            data_changed = 1;
        }

        // get_frame_res = mondup_handle_mouse(s1, cap_reg);
        // if (get_frame_res < 0) {
        //     return get_frame_res;
        // }
    }

    if (data_changed) {
        hr = ID3D11DeviceContext_Map(mondup_state->d3d11_context,
            (ID3D11Resource *)mondup_state->duplicator_texture,
            0, D3D11_MAP_READ, 0, &mondup_state->mapped_texture );
        if (hr) {
            av_log(s1, AV_LOG_ERROR, "Unable to map texture (%lx).\n", hr);
            return AVERROR_EXIT;
        }

        if (mondup_state->mapped_texture.RowPitch == mondup_state->out_width*4) {
            memcpy(mondup_state->frame_data[mondup_state->output_frame_index], mondup_state->mapped_texture.pData, mondup_state->out_size);
            // memcpy(mondup_state->frame_data, mondup_state->mapped_texture.pData, mondup_state->out_size);
        } else {
            unsigned int row_bytes = mondup_state->out_width * 4;
            uint8_t *src_ptr = (uint8_t *)mondup_state->mapped_texture.pData;
            uint8_t *dst_ptr = mondup_state->frame_data[mondup_state->output_frame_index];
            // uint8_t *dst_ptr = mondup_state->frame_data;

            for (unsigned int y = 0; y < mondup_state->out_height; y++) {
                memcpy(dst_ptr, src_ptr, row_bytes);
                src_ptr += mondup_state->mapped_texture.RowPitch;
                dst_ptr += row_bytes;
            }
        }
        ID3D11DeviceContext_Unmap(mondup_state->d3d11_context, (ID3D11Resource *)mondup_state->duplicator_texture, 0);

        // data_changed = 0;
    }

    // if (mondup_state->mouse_visible) {
    //     av_log(s1, AV_LOG_ERROR, "Mouse: Visible:%d X:%d Y:%d Type: %d %dx%d DataSize: %d\n",
    //         mondup_state->mouse_visible,
    //         mondup_state->mouse_x, mondup_state->mouse_y,
    //         mondup_state->mouse_shape.Type,
    //         mondup_state->mouse_shape.Width,
    //         mondup_state->mouse_shape.Height,
    //         mondup_state->mouse_shape_size
    //     );
    // }
    int mouse_changed = mondup_state->last_mouse_update.QuadPart != current_mouse_update.QuadPart;
    if (mondup_state->draw_mouse && (data_changed || mouse_changed)) {
        // Draw the mouse
        draw_mouse(s1, mondup_state->frame_data[0]);
    }

    int64_t grab_end = av_gettime_relative();
    mondup_state->grab_delay = grab_end - grab_start;

    return data_changed;
}



// static void *test_thread(void *priv_data_ptr) {
static void *test_thread(void *s1_ptr) {
    AVFormatContext *s1 = s1_ptr;
    struct mondup_ctx *mondup_state = s1->priv_data;

    // int i = 0;
    int get_frame_res = 0;
    int data_changed = 1;

    int64_t start_time, end_time;
    HRESULT hr;

    int64_t delay, fkajsdalkjsdlka;
    int64_t frame_time = av_rescale_q(1, mondup_state->time_base, AV_TIME_BASE_Q);

    while(mondup_state->thread_alive) {
        // av_log(s1, AV_LOG_ERROR, "Thread says hi!\n");
        // printf("Thread says hi! Loop: %d %d\n", i++, (av_gettime_relative() - mondup_state->start_time)/1000000);
        // Sleep for a second
        // av_usleep(1000000);
        // av_usleep(10 * 1000);
        // av_usleep(8 * 1000);

        // if (mondup_state->want_frame) {
        //     av_usleep(1000);
        // }

        pthread_mutex_lock(&mondup_state->frame_lock);
        start_time = av_gettime_relative();

        for (int i = 0; i < mondup_state->capture_region_count; ++i) {
            struct capture_region *cap_reg = (mondup_state->capture_regions + i);

            get_frame_res = mondup_get_frame(s1, cap_reg, 0);
            if (get_frame_res < 0) {
                // return get_frame_res;
                break;
            } else if (get_frame_res > 0) {
                data_changed = 1;
            }
        }

        if (data_changed) {
            hr = ID3D11DeviceContext_Map(mondup_state->d3d11_context,
                (ID3D11Resource *)mondup_state->duplicator_texture,
                0, D3D11_MAP_READ, 0, &mondup_state->mapped_texture );
            if (hr) {
                av_log(s1, AV_LOG_ERROR, "Unable to map texture (%lx).\n", hr);
                return AVERROR_EXIT;
            }

            if (mondup_state->mapped_texture.RowPitch == mondup_state->out_width*4) {
                memcpy(mondup_state->frame_data[mondup_state->output_frame_index], mondup_state->mapped_texture.pData, mondup_state->out_size);
                // memcpy(mondup_state->frame_data, mondup_state->mapped_texture.pData, mondup_state->out_size);

            } else {
                unsigned int row_bytes = mondup_state->out_width * 4;
                uint8_t *src_ptr = (uint8_t *)mondup_state->mapped_texture.pData;
                uint8_t *dst_ptr = mondup_state->frame_data[mondup_state->output_frame_index];
                // uint8_t *dst_ptr = mondup_state->frame_data;


                for (unsigned int y = 0; y < mondup_state->out_height; y++) {
                    memcpy(dst_ptr, src_ptr, row_bytes);
                    src_ptr += mondup_state->mapped_texture.RowPitch;
                    dst_ptr += row_bytes;
                }
            }
            ID3D11DeviceContext_Unmap(mondup_state->d3d11_context, (ID3D11Resource *)mondup_state->duplicator_texture, 0);

            data_changed = 0;
        }


        end_time = av_gettime_relative();
        mondup_state->grab_delay = end_time - start_time;

        pthread_mutex_unlock(&mondup_state->frame_lock);

        // delay = frame_time - (end_time - start_time);
        // if (delay > 0) {
        //     av_usleep(delay);
        // }
        fkajsdalkjsdlka = (av_gettime_relative() - mondup_state->start_time);
        // Align time to frame-time
        mondup_state->next_frame = (fkajsdalkjsdlka / frame_time) * frame_time + frame_time;


        end_time = av_gettime_relative();
        // delay = frame_time - (end_time - start_time);
        delay = mondup_state->next_frame - (end_time - mondup_state->start_time);

        // mondup_state->copy_delay = delay;
        if (delay > 0) {
            av_usleep(delay);
        }


        if (get_frame_res < 0) {
            break;
        }
    }
    mondup_state->thread_alive = 0;

    return NULL;
}


static int mondup_read_header(AVFormatContext *s1) {
    struct mondup_ctx *mondup_state = s1->priv_data;

    mondup_state->time_base = av_inv_q(mondup_state->framerate);

    // av_log(s1, AV_LOG_ERROR, "Input: w:%d h:%d.\n",
    //         mondup_state->width, mondup_state->height);

    int ret;

    AVStream *st = NULL;

    IDXGIFactory1 *dxgi_factory = NULL;
    IDXGIAdapter1 *dxgi_adapter = NULL;
    IDXGIOutput *tmp_dxgi_output = NULL;

    ID3D11Device *d3d11_device = NULL;
    ID3D11DeviceContext *d3d11_context = NULL;

    RECT target_rect = {0,0,0,0};
    struct output_info *source_outputs = NULL;
    int source_outputs_size = 0;

    HRESULT hr;

    const char *filename = s1->filename;

    int use_whole_desktop = (strcmp(filename, "desktop") == 0);

    if (CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&dxgi_factory)) {
        ERR_LOG_GOTO(AVERROR(EIO), "Unable to acquire a DXGIFactory.\n");
        // av_log(s1, AV_LOG_ERROR, "Unable to acquire a DXGIFactory.\n");
        // ret = AVERROR(EIO);
        // goto error;
    }

    if (IDXGIFactory1_EnumAdapters(dxgi_factory, 0, &dxgi_adapter)) {
        ERR_LOG_GOTO(AVERROR(EIO), "Unable to get adapter.\n");
        // av_log(s1, AV_LOG_ERROR, "Unable to get adapter.\n");
        // ret = AVERROR(EIO);
        // goto error;
    }

    // struct output_info source_outputs[MAX_OUTPUTS];
    source_outputs = av_mallocz(sizeof(struct output_info) * MAX_OUTPUTS);

    target_rect.left = mondup_state->offset_x;
    target_rect.top = mondup_state->offset_y;

// #define ADD_OUTPUT(OUTPUT, OUTPUT_INDEX, SOURCE_OUTPUTS, SOURCE_OUTPUTS_SIZE ) \
// do { \
//     struct output_info mon_info = SOURCE_OUTPUTS[SOURCE_OUTPUTS_SIZE]; \
//     mon_info.info.cbSize = sizeof(MONITORINFO); \
//     mon_info.adapter_index = 0; \
//     mon_info.output_index = OUTPUT_INDEX; \
//     SOURCE_OUTPUTS_SIZE++; \
//  \
//     HRESULT hr = IDXGIOutput_GetDesc(OUTPUT, &mon_info.description); \
//     av_log(s1, AV_LOG_ERROR, "IDXGIOutput_GetDesc %ld\n", hr); \
//     GetMonitorInfo(mon_info.description.Monitor, &mon_info.info); \
//     IDXGIOutput_Release(OUTPUT); \
//     OUTPUT = NULL;\
// } while(0)

    if (use_whole_desktop) {
        // if (IDXGIFactory1_EnumAdapters(dxgi_factory, 0, &dxgi_adapter) == 0) {
            for (int output_index = 0;
                 IDXGIAdapter1_EnumOutputs(dxgi_adapter, output_index, &tmp_dxgi_output) == 0 && source_outputs_size < MAX_OUTPUTS;
                 output_index++)
            {
                // struct output_info mon_info = source_outputs[source_outputs_size];
                // mon_info.info.cbSize = sizeof(MONITORINFO);
                // mon_info.adapter_index = 0;
                // mon_info.output_index = output_index;
                // source_outputs_size++;

                // IDXGIOutput1_GetDesc(tmp_dxgi_output, &mon_info.description);
                // GetMonitorInfo(mon_info.description.Monitor, &mon_info.info);
                // IDXGIOutput1_Release(tmp_dxgi_output);

                // ADD_OUTPUT(tmp_dxgi_output, output_index, source_outputs, source_outputs_size);
                struct output_info *mon_info = (source_outputs + source_outputs_size);
                mon_info->info.cbSize = sizeof(MONITORINFO);
                mon_info->adapter_index = 0;
                mon_info->output_index = output_index;
                source_outputs_size++;

                IDXGIOutput_GetDesc(tmp_dxgi_output, &mon_info->description);
                GetMonitorInfo(mon_info->description.Monitor, &mon_info->info);
                IDXGIOutput_Release(tmp_dxgi_output);
                tmp_dxgi_output = NULL;


                // if (output_monitor.dwFlags & 1) {
                //     display_found = 1;
                // }

                // if (!display_found) {
                //     IDXGIOutput1_Release(dxgi_output_temp);
                // }
            }
        // }
    } else {
        // Assume filename is an output index
        int target_output_index = strtol(filename, NULL, 10);

        // if (IDXGIFactory1_EnumAdapters(dxgi_factory, 0, &dxgi_adapter) == 0) {
        // Query for the given output
        if (IDXGIAdapter1_EnumOutputs(dxgi_adapter, target_output_index, &tmp_dxgi_output) == 0) {
            // ADD_OUTPUT(tmp_dxgi_output, target_output_index, source_outputs, source_outputs_size);
            struct output_info *mon_info = (source_outputs + source_outputs_size);
            mon_info->info.cbSize = sizeof(MONITORINFO);
            mon_info->adapter_index = 0;
            mon_info->output_index = target_output_index;
            source_outputs_size++;

            IDXGIOutput_GetDesc(tmp_dxgi_output, &mon_info->description);
            GetMonitorInfo(mon_info->description.Monitor, &mon_info->info);
            IDXGIOutput_Release(tmp_dxgi_output);
            tmp_dxgi_output = NULL;

            // av_log(s1, AV_LOG_ERROR, "Mon %d: (%ld,%ld,%ld,%ld).\n",
            //     target_output_index,
            //     mon_info->description.DesktopCoordinates.left, mon_info->description.DesktopCoordinates.top, mon_info->description.DesktopCoordinates.right, mon_info->description.DesktopCoordinates.bottom);

            // Set rect to start from display
            target_rect.left += mon_info->description.DesktopCoordinates.left;
            target_rect.top  += mon_info->description.DesktopCoordinates.top;
            // if (mondup_state->width && mondup_state->height) {
            //     target_rect.right  = target_rect.left + mondup_state->width;
            //     target_rect.bottom = target_rect.top  + mondup_state->height;
            // } else {
            // if (mondup_state->width && mondup_state->height) {
            //     target_rect.right  = target_rect.left + (mon_info->description.DesktopCoordinates.right  - mon_info->description.DesktopCoordinates.left);
            //     target_rect.bottom = target_rect.top  + (mon_info->description.DesktopCoordinates.bottom - mon_info->description.DesktopCoordinates.top);
            // }
        }
        // }
    }
    // IDXGIFactory1_Release(dxgi_factory);

    // av_log(s1, AV_LOG_ERROR, "Target rect (%ld,%ld,%ld,%ld).\n",
    //         target_rect.left, target_rect.top, target_rect.right, target_rect.bottom);

    IF_ERR_LOG_GOTO(source_outputs_size == 0, AVERROR(EIO), "No output sources available.\n");
    // if (source_outputs_size == 0) {
    //     av_log(s1, AV_LOG_ERROR, "No output sources available.\n");
    //     ret = AVERROR(EIO);
    //     goto error;
    // }

    int initial_monitor = -1;

    RECT full_extents = {
        .left = INT_MAX,
        .right = INT_MIN,
        .top = INT_MAX,
        .bottom = INT_MIN
    };
    for (int i = 0; i < source_outputs_size; ++i) {
        struct output_info mon_info = source_outputs[i];
        RECT mon_rect = mon_info.description.DesktopCoordinates;

        // av_log(s1, AV_LOG_ERROR, "%d (%ld,%ld %ld,%ld) %d\n",
        //     i,
        //     mon_rect.left, mon_rect.top, mon_rect.right, mon_rect.bottom,
        //     mon_info.description.Rotation);

        // Collect full extents
        if (mon_rect.left < full_extents.left)
            full_extents.left = mon_rect.left;
        if (mon_rect.top < full_extents.top)
            full_extents.top = mon_rect.top;
        if (mon_rect.right > full_extents.right)
            full_extents.right = mon_rect.right;
        if (mon_rect.bottom > full_extents.bottom)
            full_extents.bottom = mon_rect.bottom;

        // If target rect top-left corner lies inside this monitor, it's the initial one
        if ((target_rect.left >= mon_rect.left && target_rect.left <= mon_rect.right)
            && (target_rect.top >= mon_rect.top && target_rect.top <= mon_rect.bottom)) {
            initial_monitor = i;
        }

        // av_log(s1, AV_LOG_ERROR, "%d (%ld,%ld %ld,%ld)\n",
        //     i,
        //     full_extents.left, full_extents.top, full_extents.right, full_extents.bottom);
    }

    // If using capture area, set it
    if (mondup_state->width && mondup_state->height) {
        target_rect.right = target_rect.left + mondup_state->width;
        target_rect.bottom = target_rect.top + mondup_state->height;
    } else if (initial_monitor >= 0) {
        // Otherwise use the full monitor
        RECT mon_rect = (source_outputs + initial_monitor)->description.DesktopCoordinates;
        target_rect.right = target_rect.left + (mon_rect.right - mon_rect.left);
        target_rect.bottom = target_rect.top + (mon_rect.bottom - mon_rect.top);
    } else {
        ERR_LOG_GOTO(AVERROR(EIO), "Initial monitor not found! Target rect (%ld,%ld,%ld,%ld) is outside available space (%ld,%ld,%ld,%ld).\n",
            target_rect.left, target_rect.top, target_rect.right, target_rect.bottom,
            full_extents.left, full_extents.top, full_extents.right, full_extents.bottom);
        // av_log(s1, AV_LOG_ERROR, "Initial monitor not found! Target rect (%ld,%ld,%ld,%ld) is outside available space (%ld,%ld,%ld,%ld).\n",
        //     target_rect.left, target_rect.top, target_rect.right, target_rect.bottom,
        //     full_extents.left, full_extents.top, full_extents.right, full_extents.bottom);
        // ret = AVERROR(EIO);
        // goto error;
    }

    // av_log(s1, AV_LOG_ERROR, "Target rect (%ld,%ld,%ld,%ld), all space (%ld,%ld,%ld,%ld).\n",
    //         target_rect.left, target_rect.top, target_rect.right, target_rect.bottom,
    //         full_extents.left, full_extents.top, full_extents.right, full_extents.bottom);

    if (!RECT_CONTAINED(target_rect, full_extents)) {
        ERR_LOG_GOTO(AVERROR(EIO), "Target rect (%ld,%ld,%ld,%ld) is outside available space (%ld,%ld,%ld,%ld).\n",
            target_rect.left, target_rect.top, target_rect.right, target_rect.bottom,
            full_extents.left, full_extents.top, full_extents.right, full_extents.bottom);
        // av_log(s1, AV_LOG_ERROR, "Target rect (%ld,%ld,%ld,%ld) is outside available space (%ld,%ld,%ld,%ld).\n",
        //     target_rect.left, target_rect.top, target_rect.right, target_rect.bottom,
        //     full_extents.left, full_extents.top, full_extents.right, full_extents.bottom);
        // ret = AVERROR(EIO);
        // goto error;
    }

    mondup_state->out_width = (target_rect.right - target_rect.left);
    mondup_state->out_height = (target_rect.bottom - target_rect.top);
    mondup_state->out_size = mondup_state->out_width * mondup_state->out_height * 4;

    //////////////////
    // Set up D3D11 //
    //////////////////
    D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_1
    };
    D3D_FEATURE_LEVEL used_feature_level;
    hr = D3D11CreateDevice((IDXGIAdapter *) dxgi_adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                           // D3D11_CREATE_DEVICE_SINGLETHREADED, feature_levels, 5,
                           0, feature_levels, 5,
                           // 0, feature_levels, 5,
                           D3D11_SDK_VERSION, &d3d11_device, &used_feature_level, &d3d11_context);
    IF_ERR_LOG_GOTO(hr, AVERROR(EIO), "Failed to create Direct3D device (%lx).\n", hr);
    // if (hr) {
    //     av_log(s1, AV_LOG_ERROR, "Failed to create Direct3D device (%lx).\n", hr);
    //     ret = AVERROR_EXIT;
    //     goto error;
    // }

    D3D11_TEXTURE2D_DESC texture_desc = {
        .Width = mondup_state->out_width,
        .Height = mondup_state->out_height,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .ArraySize  = 1,
        .Usage      = D3D11_USAGE_STAGING,
        // .Usage      = D3D11_USAGE_STAGING,
        // .BindFlags  = D3D11_BIND_RENDER_TARGET,
        // .BindFlags  = D3D11_BIND_RENDER_TARGET,
        .BindFlags  = 0,
        .CPUAccessFlags = D3D11_CPU_ACCESS_READ,
        .MiscFlags  = 0,
    };
    hr = ID3D11Device_CreateTexture2D(d3d11_device, &texture_desc, NULL, &mondup_state->duplicator_texture);
    IF_ERR_LOG_GOTO(hr, AVERROR(EIO), "Failed to create Texture2D (%lx).\n", hr);
    // if (hr) {
    //     av_log(s1, AV_LOG_ERROR, "Failed to create Texture2D (%lx).\n", hr);
    //     ret = AVERROR_EXIT;
    //     goto error;
    // }

    // Create captures

    mondup_state->capture_regions = av_mallocz(source_outputs_size * sizeof(struct capture_region));
    mondup_state->capture_region_count = 0;

    for (int i = 0; i < source_outputs_size; ++i) {
        struct output_info *mon_info = (source_outputs + i);
        if (!RECTS_OVERLAP(target_rect, mon_info->description.DesktopCoordinates))
            continue;

        RECT overlap = {
            .left   = FFMAX(target_rect.left, mon_info->description.DesktopCoordinates.left),
            .right  = FFMIN(target_rect.right, mon_info->description.DesktopCoordinates.right),
            .top    = FFMAX(target_rect.top, mon_info->description.DesktopCoordinates.top),
            .bottom = FFMIN(target_rect.bottom, mon_info->description.DesktopCoordinates.bottom)
        };
        if (overlap.left >= overlap.right || overlap.top >= overlap.bottom)
            continue;

        struct capture_region *cap_reg = (mondup_state->capture_regions + mondup_state->capture_region_count);
        mondup_state->capture_region_count++;


        cap_reg->src_x = overlap.left - mon_info->description.DesktopCoordinates.left;
        cap_reg->src_y = overlap.top - mon_info->description.DesktopCoordinates.top;

        cap_reg->dst_x = overlap.left - target_rect.left;
        cap_reg->dst_y = overlap.top - target_rect.top;

        cap_reg->width = (overlap.right - overlap.left);
        cap_reg->height = (overlap.bottom - overlap.top);

        memcpy(&cap_reg->description, &mon_info->description, sizeof(DXGI_OUTPUT_DESC));

        // av_log(s1, AV_LOG_ERROR, "%dx%d from %d,%d to %d,%d\n",
        //     cap_reg->width, cap_reg->height,
        //     cap_reg->src_x, cap_reg->src_y,
        //     cap_reg->dst_x, cap_reg->dst_y
        // );

        // av_log(s1, AV_LOG_ERROR, "Overlap on %d (%ld,%ld,%ld,%ld) -> (%ld,%ld,%ld,%ld).\n",
        //     i,
        //     mon_info->description.DesktopCoordinates.left, mon_info->description.DesktopCoordinates.top, mon_info->description.DesktopCoordinates.right, mon_info->description.DesktopCoordinates.bottom,
        //     overlap.left, overlap.top, overlap.right, overlap.bottom
        // );

        if (IDXGIFactory1_EnumAdapters(dxgi_factory, 0, &dxgi_adapter)) {
            ERR_LOG_GOTO(AVERROR(EIO), "Unable to enumerate adapter.\n");
            // av_log(s1, AV_LOG_ERROR, "Unable to enumerate adapter.\n");
            // ret = AVERROR_EXIT;
            // goto error;
        }
        if (IDXGIAdapter1_EnumOutputs(dxgi_adapter, mon_info->output_index, &tmp_dxgi_output)) {
            ERR_LOG_GOTO(AVERROR(EIO), "Unable to enumerate output.\n");
            // av_log(s1, AV_LOG_ERROR, "Unable to enumerate output.\n");
            // ret = AVERROR_EXIT;
            // goto error;
        }
        IDXGIAdapter1_Release(dxgi_adapter);
        if (IDXGIOutput_QueryInterface(tmp_dxgi_output, &IID_IDXGIOutput1, (void**) &cap_reg->dxgi_output)) {
            ERR_LOG_GOTO(AVERROR(EIO), "Unable to upgrade output.\n");
            // av_log(s1, AV_LOG_ERROR, "Unable to upgrade output.\n");
            // ret = AVERROR_EXIT;
            // goto error;
        }
        IDXGIOutput_Release(tmp_dxgi_output);

        hr = IDXGIOutput1_DuplicateOutput(cap_reg->dxgi_output, (IUnknown *) d3d11_device, &cap_reg->output_duplication);
        IF_ERR_LOG_GOTO(hr, AVERROR(EIO), "Unable to create output duplicator (%lx).\n", hr);
        // if (hr) {
        //     av_log(s1, AV_LOG_ERROR, "Unable to create output duplicator (%lx).\n", hr);
        //     ret = AVERROR_EXIT;
        //     goto error;
        // }
    }
    IDXGIFactory1_Release(dxgi_factory);
    av_freep(&source_outputs);

    mondup_state->d3d11_device = d3d11_device;
    mondup_state->d3d11_context = d3d11_context;

    mondup_state->grab_delay = 0;
    mondup_state->copy_delay = 0;
    mondup_state->frame_delay = 0;


    // // Create mutex for output access
    // if (pthread_mutex_init(&mondup_state->frame_lock, NULL)) {
    //     ERR_LOG_GOTO(AVERROR(EIO), "Creating mutex failed\n");
    // }



    // av_log(s1, AV_LOG_ERROR, "target_rect (%ld,%ld,%ld,%ld) full_extents (%ld,%ld,%ld,%ld).\n",
    //         target_rect.left, target_rect.top, target_rect.right, target_rect.bottom,
    //         full_extents.left, full_extents.top, full_extents.right, full_extents.bottom);

    // Create stream
    st = avformat_new_stream(s1, NULL);
    IF_ERR_LOG_GOTO(!st, AVERROR(ENOMEM), "Unable to create stream\n");
    // if (!st) {
    //     av_log(s1, AV_LOG_ERROR, "Cannot add stream\n");
    //     ret = AVERROR(ENOMEM);
    //     goto error;
    // }
    avpriv_set_pts_info(st, 64, 1, 1000000); /* 64 bits pts in us */

    // Setup video stream
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->format     = AV_PIX_FMT_BGRA;
    st->codecpar->codec_tag  = avcodec_pix_fmt_to_codec_tag(st->codecpar->format);

    st->codecpar->width      = mondup_state->out_width;
    st->codecpar->height     = mondup_state->out_height;
    st->codecpar->bit_rate   = av_rescale(mondup_state->out_size, mondup_state->time_base.den, mondup_state->time_base.num);

    // st->r_frame_rate = av_inv_q(mondup_state->time_base);
    st->avg_frame_rate       = av_inv_q(mondup_state->time_base);

    // mondup_state->next_frame = av_gettime() / av_q2d(mondup_state->time_base);

    // mondup_state->want_frame = 0;
    mondup_state->output_frame_index = 0;
    // mondup_state->frame_data = av_malloc(mondup_state->out_size);
    mondup_state->frame_data[0] = av_malloc(mondup_state->out_size);
    mondup_state->frame_data[1] = av_malloc(mondup_state->out_size);

    // LARGE_INTEGER last_mouse_update;
    // DXGI_OUTDUPL_POINTER_SHAPE_INFO mouse_shape;
    mondup_state->mouse_x = 0;
    mondup_state->mouse_y = 0;
    mondup_state->mouse_visible = 0;
    mondup_state->mouse_shape_size = 0;
    // void *mouse_shape_buffer;

    // Warm up duplicator(s)
    int get_frame_res;
    // while (1) {
    //     get_frame_res = mondup_render_frame(s1);
    //     IF_ERR_LOG_GOTO(get_frame_res < 0, get_frame_res, "Duplicator warmup failed\n");
    //     if (get_frame_res) {
    //         break;
    //     }
    // }
    for (int i = 0; i < mondup_state->capture_region_count; ++i) {
        struct capture_region *cap_reg = (mondup_state->capture_regions + i);
        while(cap_reg->frame_info.AccumulatedFrames == 0) {
            get_frame_res = mondup_get_frame(s1, cap_reg, 0);
            // av_log(s1, AV_LOG_ERROR, "got %d.\n", get_frame_res);
            IF_ERR_LOG_GOTO(get_frame_res < 0, get_frame_res, "Duplicator warmup failed\n");
        }
        // mondup_handle_mouse(s1, cap_reg);
    }

    // Finalize state
    mondup_state->start_time = av_gettime_relative();
    mondup_state->next_frame = 0;
    mondup_state->previous_frame = 0;


    // Start thread
    // av_log(s1, AV_LOG_ERROR, "Starting thread (%p)\n", &mondup_state->thread);
    // mondup_state->thread_alive = 1;
    // // pthread_create(&mondup_state->thread, NULL, test_thread, mondup_state);
    // pthread_create(&mondup_state->thread, NULL, test_thread, s1);
    // av_log(s1, AV_LOG_ERROR, "Started thread (%p)\n", &mondup_state->thread);

    return 0;

error:
    // Free everything
    av_freep(&source_outputs);

    if (mondup_state->capture_regions) {
        for (int i = 0; i < mondup_state->capture_region_count; ++i) {
            struct capture_region *cap_reg = (mondup_state->capture_regions + i);
            IDXGIOutput1_Release(cap_reg->dxgi_output);
            IDXGIOutputDuplication_Release(cap_reg->output_duplication);
            // IDXGIResource_Release(cap_reg->duplicated_resource);
            av_freep(&cap_reg->metadata);
        }
    }
    av_freep(&mondup_state->capture_regions);

    if (dxgi_factory)
        IDXGIFactory1_Release(dxgi_factory);
    if (dxgi_adapter)
        IDXGIAdapter1_Release(dxgi_adapter);
    if (tmp_dxgi_output)
        IDXGIOutput_Release(tmp_dxgi_output);
    if (d3d11_device)
        ID3D11Device_Release(d3d11_device);
    if (d3d11_context)
        ID3D11DeviceContext_Release(d3d11_context);
    if (mondup_state->duplicator_texture)
        ID3D11Texture2D_Release(mondup_state->duplicator_texture);

    return ret;
}

#define RGB2V(pixel, n) ( (pixel >> n*8) & 0xFF )

static uint32_t pixel_blend(uint32_t fg, uint32_t bg) {
    // unsigned int alpha = RGB2V(fg, 3) + 1;
    // unsigned int inv_alpha = 256 - RGB2V(fg, 3);
    float alpha = (float) (RGB2V(fg, 3) / 255.0f);
    float inv_alpha = 1.0f - alpha;

    // return 0xff << 24 +
    //        ((unsigned char)((alpha * RGB2V(fg, 2) + inv_alpha * RGB2V(bg, 2)) >> 8)) << 16 +
    //        ((unsigned char)((alpha * RGB2V(fg, 1) + inv_alpha * RGB2V(bg, 1)) >> 8)) <<  8 +
    //        ((unsigned char)((alpha * RGB2V(fg, 0) + inv_alpha * RGB2V(bg, 0)) >> 8)) <<  0;
    return (0xFF << 24) |
           ( (uint8_t)(alpha * RGB2V(fg, 2) + inv_alpha * RGB2V(bg, 2)) << 16) |
           ( (uint8_t)(alpha * RGB2V(fg, 1) + inv_alpha * RGB2V(bg, 1)) <<  8) |
           ( (uint8_t)(alpha * RGB2V(fg, 0) + inv_alpha * RGB2V(bg, 0)) <<  0);
}

// static void draw_mouse(struct mondup_ctx *mondup_state, uint32_t *target_pixels) {
static void draw_mouse(AVFormatContext *s1, uint32_t *target_pixels) {
    struct mondup_ctx *mondup_state = s1->priv_data;
    if (!mondup_state->mouse_visible) {
        return;
    }
    int pointer_w = mondup_state->mouse_shape.Width;
    int pointer_h = mondup_state->mouse_shape.Height;

    int i_y, i_x;
    int i_w = mondup_state->out_width;
    int i_h = mondup_state->out_height;


    int pitch = mondup_state->mouse_shape.Pitch;
    if (mondup_state->mouse_shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // 1 bpp AND and XOR mask for pointer
        uint8_t *and_xor_mask = (uint8_t *) mondup_state->mouse_shape_buffer;

        for (int p_y = 0; p_y < pointer_h/2; ++p_y) {
            uint8_t bit_mask = 0x80; // 0b10000000
            i_y = mondup_state->mouse_y + p_y;
            if (i_y < 0 || i_y >= i_h) {
                continue;
            }

            for (int p_x = 0; p_x < pointer_w; ++p_x) {
                i_x = mondup_state->mouse_x + p_x;

                if (i_x >= 0 && i_x < i_w) {
                    int bit_index = p_y * pitch + (p_x/8);
                    int target_index = i_y * i_w + i_x;

                    uint8_t and_mask = and_xor_mask[bit_index] & bit_mask;
                    uint8_t xor_mask = and_xor_mask[bit_index + (pointer_h/2) * pitch] & bit_mask;

                    uint32_t and_mask_32 = and_mask ? 0xFFFFFFFF : 0xFF000000;
                    uint32_t xor_mask_32 = xor_mask ? 0x00FFFFFF : 0x00000000;

                    target_pixels[target_index] = (target_pixels[target_index] & and_mask_32) ^ xor_mask_32;

                    // Shift the mask bit (pixel)
                    bit_mask = (bit_mask == 0x01) ? 0x80 : bit_mask >> 1;
                }
            }
        }

    } else if (mondup_state->mouse_shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        // Color cursor ARGB (0xAARRGGBB)
        uint32_t *argb_cursor = (uint32_t *) mondup_state->mouse_shape_buffer;

        for (int p_y = 0; p_y < pointer_h; ++p_y) {
            i_y = mondup_state->mouse_y + p_y;
            if (i_y < 0 || i_y >= i_h) {
                continue;
            }

            for (int p_x = 0; p_x < pointer_w; ++p_x) {
                i_x = mondup_state->mouse_x + p_x;

                if (i_x >= 0 && i_x < i_w) {
                    int target_index = i_y * i_w + i_x;
                    target_pixels[target_index] = pixel_blend(argb_cursor[p_y * (pitch / 4) + p_x], target_pixels[target_index]);
                }

            }
        }
    } else if (mondup_state->mouse_shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        // Color-masked cursor
        uint32_t *masked_cursor = (uint32_t *) mondup_state->mouse_shape_buffer;

        for (int p_y = 0; p_y < pointer_h; ++p_y) {
            i_y = mondup_state->mouse_y + p_y;
            if (i_y < 0 || i_y >= i_h) {
                continue;
            }

            for (int p_x = 0; p_x < pointer_w; ++p_x) {
                i_x = mondup_state->mouse_x + p_x;

                if (i_x >= 0 && i_x < i_w) {
                    int target_index = i_y * i_w + i_x;

                    if (target_pixels[target_index] & 0xFF000000) {
                        // XOR the source and cursor
                        target_pixels[target_index] = 0xFF000000 | (target_pixels[target_index] ^ masked_cursor[p_y * (pitch / 4) + p_x]);
                    } else {
                        // Replace with cursor RGB
                        target_pixels[target_index] = 0xFF000000 | masked_cursor[p_y * (pitch / 4) + p_x];
                    }

                }

            }
        }
    }

}


static int mondup_read_packet(AVFormatContext *s1, AVPacket *pkt) {
    int64_t frame_start_time = av_gettime_relative();

    struct mondup_ctx *mondup_state = s1->priv_data;

    int64_t current_time;
    int64_t curtime;
    int64_t current_time_stream, delay;

    HRESULT hr;
    int get_frame_res;
    // DXGI_OUTDUPL_FRAME_INFO frame_info;
    // IDXGIResource *duplicated_resource;

    // if (!mondup_state->thread_alive) {
    //     // Thread quit on us
    //     return AVERROR(ENOMEM);
    // }

    // for (int i = 0; i < mondup_state->capture_region_count; ++i) {
    //     struct capture_region *cap_reg = (mondup_state->capture_regions + i);

    //     get_frame_res = mondup_get_frame(s1, cap_reg, 0);
    //     if (get_frame_res < 0) {
    //         return get_frame_res;
    //     }
    // }
    int64_t frame_time = av_rescale_q(1, mondup_state->time_base, AV_TIME_BASE_Q);
    // int64_t next_frame = mondup_state->next_frame;

    current_time = av_gettime_relative();
    if (mondup_state->next_frame == 0) {
        mondup_state->start_time = current_time;
    }
    current_time_stream = current_time - mondup_state->start_time;

    delay = mondup_state->next_frame - current_time_stream;
    // delay = mondup_state->next_frame - current_time_stream - mondup_state->copy_delay;

    if (mondup_state->do_log) {
        // av_log(s1, AV_LOG_ERROR, "Time: %lld Expected: %lld Thread sleep: %lld Grab delay: %lld Frame: %lld (base %lld) sleep: %lld\n",
        av_log(s1, AV_LOG_ERROR, "Time: %lld Expected: %lld Copy: %lld Grab delay: %lld Frame: %lld (target %d) sleep: %lld since-last: %lld\n",
            current_time_stream / 100,
            mondup_state->next_frame / 100,
            mondup_state->copy_delay / 100,
            mondup_state->grab_delay / 100,
            mondup_state->frame_delay / 100,
            // frame_time / 100,
            mondup_state->output_frame_index,
            delay / 100,
            (current_time - mondup_state->last_end) / 100
        );
    }
    if (delay > 0) {
        av_usleep(delay);
    }
    current_time_stream = av_gettime_relative() - mondup_state->start_time;

    // mondup_state->next_frame += frame_time;
    // for (;;) {
        // curtime = av_gettime();
        // delay   = mondup_state->next_frame - current_time_stream;
        // if (delay > 0) {
        //     av_usleep(delay);
        // }
    // }
    // pkt->pts = current_time_stream;
    // pkt->pts = mondup_state->next_frame;
    // pkt->dts = AV_NOPTS_VALUE;
    // mondup_state->next_frame = current_time_stream + frame_time;
    // mondup_state->next_frame += frame_time;

    // for (;;) {
    //     curtime = av_gettime();
    //     delay = next_frame * av_q2d(mondup_state->time_base) - curtime;
    //     if (delay <= 0) {
    //         if (delay < INT64_C(-1000000) * av_q2d(mondup_state->time_base)) {
    //             next_frame += INT64_C(1000000);
    //         }
    //         break;
    //     }
    //     if (s1->flags & AVFMT_FLAG_NONBLOCK) {
    //         return AVERROR(EAGAIN);
    //     } else {
    //         av_usleep(delay);
    //     }
    // }
    // mondup_state->next_frame = next_frame;


    /***********************/
    do {
    // Get something for the first frame by any means necessary!
    // if (mondup_state->next_frame == 0) {
    //     while(1) {
    //         get_frame_res = mondup_render_frame(s1);
    //         av_log(s1, AV_LOG_ERROR, "Call: %d at %lld.\n", get_frame_res, current_time_stream);
    //         if (get_frame_res < 0) {
    //             av_log(s1, AV_LOG_ERROR, "Rendering frame failed (%lx).\n", get_frame_res);
    //             return get_frame_res;
    //         } else if (get_frame_res) {
    //             break;
    //         }
    //     }

    // } else {
        get_frame_res = mondup_render_frame(s1);
        // av_log(s1, AV_LOG_ERROR, "Call: %d at %lld.\n", get_frame_res, current_time_stream);
        if (get_frame_res < 0) {
            av_log(s1, AV_LOG_ERROR, "Rendering frame failed (%lx).\n", get_frame_res);
            return get_frame_res;
        } else if (get_frame_res) {
                break;
            }
    // }
    } while (mondup_state->next_frame == 0);
    // if (mondup_state->next_frame == 0) {
    //     // First frame
    // } else {
    // }
    // int data_changed = 0;
    // LARGE_INTEGER current_mouse_update = mondup_state->last_mouse_update;
    // // int get_frame_res = 0;
    // // HRESULT hr;

    // int64_t grab_start = av_gettime_relative();
    // for (int i = 0; i < mondup_state->capture_region_count; ++i) {
    //     struct capture_region *cap_reg = (mondup_state->capture_regions + i);

    //     get_frame_res = mondup_get_frame(s1, cap_reg, 0);
    //     if (get_frame_res < 0) {
    //         return get_frame_res;
    //     } else if (get_frame_res > 0) {
    //         data_changed = 1;
    //     }

    //     // get_frame_res = mondup_handle_mouse(s1, cap_reg);
    //     // if (get_frame_res < 0) {
    //     //     return get_frame_res;
    //     // }
    // }

    // if (data_changed) {
    //     hr = ID3D11DeviceContext_Map(mondup_state->d3d11_context,
    //         (ID3D11Resource *)mondup_state->duplicator_texture,
    //         0, D3D11_MAP_READ, 0, &mondup_state->mapped_texture );
    //     if (hr) {
    //         av_log(s1, AV_LOG_ERROR, "Unable to map texture (%lx).\n", hr);
    //         return AVERROR_EXIT;
    //     }

    //     if (mondup_state->mapped_texture.RowPitch == mondup_state->out_width*4) {
    //         memcpy(mondup_state->frame_data[mondup_state->output_frame_index], mondup_state->mapped_texture.pData, mondup_state->out_size);
    //         // memcpy(mondup_state->frame_data, mondup_state->mapped_texture.pData, mondup_state->out_size);
    //     } else {
    //         unsigned int row_bytes = mondup_state->out_width * 4;
    //         uint8_t *src_ptr = (uint8_t *)mondup_state->mapped_texture.pData;
    //         uint8_t *dst_ptr = mondup_state->frame_data[mondup_state->output_frame_index];
    //         // uint8_t *dst_ptr = mondup_state->frame_data;

    //         for (unsigned int y = 0; y < mondup_state->out_height; y++) {
    //             memcpy(dst_ptr, src_ptr, row_bytes);
    //             src_ptr += mondup_state->mapped_texture.RowPitch;
    //             dst_ptr += row_bytes;
    //         }
    //     }
    //     ID3D11DeviceContext_Unmap(mondup_state->d3d11_context, (ID3D11Resource *)mondup_state->duplicator_texture, 0);

    //     // data_changed = 0;
    // }

    // // if (mondup_state->mouse_visible) {
    // //     av_log(s1, AV_LOG_ERROR, "Mouse: Visible:%d X:%d Y:%d Type: %d %dx%d DataSize: %d\n",
    // //         mondup_state->mouse_visible,
    // //         mondup_state->mouse_x, mondup_state->mouse_y,
    // //         mondup_state->mouse_shape.Type,
    // //         mondup_state->mouse_shape.Width,
    // //         mondup_state->mouse_shape.Height,
    // //         mondup_state->mouse_shape_size
    // //     );
    // // }

    // int64_t grab_end = av_gettime_relative();
    // mondup_state->grab_delay = grab_end - grab_start;

    /***********************/


    // int64_t current_frame_time = mondup_state->next_frame;

    // Lock
    // current_time = av_gettime_relative();

    // av_log(s1, AV_LOG_ERROR, "pkt: %lld fmr: %lld\n", pkt->pts, mondup_state->next_frame);

    // while (mondup_state->next_frame == pkt->pts) {
    // while (mondup_state->next_frame == current_frame_time) {
    // while (mondup_state->next_frame == mondup_state->previous_frame) {
    //     av_usleep(500);
    // }

    // int64_t lock_start = av_gettime_relative();
    // mondup_state->want_frame = 1;
        // pthread_mutex_lock(&mondup_state->frame_lock);
    // int64_t lock_time = av_gettime_relative();
    // mondup_state->want_frame = 0;

    // mondup_state->next_frame += INT64_C(1000000) * av_q2d(mondup_state->time_base);
    // mondup_state->next_frame += frame_time;

    // mondup_state->next_frame = current_time_stream + INT64_C(1000000) * av_q2d(mondup_state->time_base);

    // if (av_new_packet(pkt, mondup_state->out_size) < 0)
        // return AVERROR(ENOMEM);
    // pkt->pts = current_time_stream;
    // pkt->pts = mondup_state->next_frame;

    int64_t copy_start = av_gettime_relative();
    // if (av_new_packet(pkt, mondup_state->out_size) < 0)
        // return AVERROR(ENOMEM);

    // int64_t used_current_stream_time = (delay > 0) ? mondup_state->next_frame : current_time_stream;
    // int64_t used_current_stream_time = (delay > 0) ? mondup_state->next_frame : current_time_stream;
    pkt->pts = current_time_stream;
    // pkt->pts = mondup_state->next_frame;
    pkt->dts = AV_NOPTS_VALUE;

    // memcpy(pkt->data, mondup_state->frame_data[0], mondup_state->out_size);
    // draw_mouse(mondup_state, mondup_state->frame_data[1]);
        // memcpy(mondup_state->frame_data[1], mondup_state->frame_data[0], mondup_state->out_size);

    // LARGE_INTEGER current_mouse_update = mondup_state->last_mouse_update;
        // int mouse_changed = mondup_state->last_mouse_update.QuadPart != current_mouse_update.QuadPart;
        // if (mondup_state->draw_mouse && (data_changed || mouse_changed)) {
        //     // Draw the mouse
        //     draw_mouse(s1, mondup_state->frame_data[0]);
        // }

    pkt->size = mondup_state->out_size;
    pkt->data = mondup_state->frame_data[0];
    // pkt->data = mondup_state->frame_data[0];
    // pkt->data = mondup_state->frame_data[mondup_state->output_frame_index];
    // pkt->data = mondup_state->frame_data;
    int64_t copy_end = av_gettime_relative();

    // mondup_state->previous_frame = mondup_state->next_frame;
    // mondup_state->next_frame += frame_time;
    mondup_state->next_frame = (delay > 0) ? mondup_state->next_frame + frame_time : pkt->pts + frame_time;

    // mondup_state->output_frame_index = (mondup_state->output_frame_index + 1) % 2;


    // hr = ID3D11DeviceContext_Map(mondup_state->d3d11_context,
    //     (ID3D11Resource *)mondup_state->duplicator_texture,
    //     0, D3D11_MAP_READ, 0, &mondup_state->mapped_texture );
    // if (hr) {
    //     av_log(s1, AV_LOG_ERROR, "Unable to map texture (%lx).\n", hr);
    //     // IDXGIOutputDuplication_ReleaseFrame(mondup_state->output_duplication);
    //     return AVERROR_EXIT;
    // }

    // if (mondup_state->mapped_texture.RowPitch == mondup_state->out_width*4) {
    //     // pkt->data = mondup_state->mapped_texture.pData;
    //     memcpy(pkt->data, mondup_state->mapped_texture.pData, mondup_state->out_size);
    // } else {
    //     unsigned int row_bytes = mondup_state->out_width * 4;
    //     uint8_t *src_ptr = (uint8_t *)mondup_state->mapped_texture.pData;
    //     uint8_t *dst_ptr = pkt->data;

    //     for (unsigned int y = 0; y < mondup_state->out_height; y++) {
    //         memcpy(dst_ptr, src_ptr, row_bytes);
    //         src_ptr += mondup_state->mapped_texture.RowPitch;
    //         dst_ptr += row_bytes;
    //     }
    // }

    // ID3D11DeviceContext_Unmap(mondup_state->d3d11_context, (ID3D11Resource *)mondup_state->duplicator_texture, 0);

    // Unlock
        // pthread_mutex_unlock(&mondup_state->frame_lock);
    // mondup_state->copy_delay = av_gettime_relative() - lock_start;

    // av_log(s1, AV_LOG_ERROR, "lock took: %ldns\n", lock_time - current_time);
    // av_log(s1, AV_LOG_ERROR, "memcpy took: %ldns\n", copy_end - copy_start);
    // av_log(s1, AV_LOG_ERROR, "Time: %ld Expected: %ld Copy delay: %ld Grab delay: %ld\n",
    //     current_time_stream / 1000,
    //     mondup_state->next_frame / 1000,

    //     mondup_state->copy_delay / 1000,
    //     mondup_state->grab_delay / 1000,
    //     delay / 1000
    // );

    mondup_state->copy_delay = copy_end - copy_start;
    mondup_state->frame_delay = av_gettime_relative() - frame_start_time;
    mondup_state->last_end = av_gettime_relative();

    return mondup_state->out_size;
}

static int mondup_read_close(AVFormatContext *s1) {
    struct mondup_ctx *mondup_state = s1->priv_data;

    // mondup_state->thread_alive = 0;
    // if (&mondup_state->thread) {
    //     av_log(s1, AV_LOG_ERROR, "Joining thread.\n");
    //     pthread_join(mondup_state->thread, NULL);
    //     av_log(s1, AV_LOG_ERROR, "Thread joined.\n");
    // }

    if (mondup_state->capture_regions) {
        for (int i = 0; i < mondup_state->capture_region_count; ++i) {
            struct capture_region *cap_reg = (mondup_state->capture_regions + i);
            IDXGIOutputDuplication_Release(cap_reg->output_duplication);
            IDXGIOutput1_Release(cap_reg->dxgi_output);
            av_freep(&cap_reg->metadata);
        }
    }
    av_freep(&mondup_state->capture_regions);
    mondup_state->capture_region_count = 0;

    av_freep(&mondup_state->mouse_shape_buffer);

    av_freep(&mondup_state->frame_data[0]);
    av_freep(&mondup_state->frame_data[1]);

    ID3D11Texture2D_Release(mondup_state->duplicator_texture);
    ID3D11DeviceContext_Release(mondup_state->d3d11_context);
    ID3D11Device_Release(mondup_state->d3d11_device);

    return 0;
}

static int mondup_get_device_list(AVFormatContext *s1, AVDeviceInfoList *device_list)
{
    HRESULT hr;

    IDXGIFactory1 *pDXGIFactory;
    IDXGIAdapter1 *dxgi_adapter = NULL;
    IDXGIOutput1 *dxgi_output = NULL;
    AVDeviceInfo *new_device = NULL;
    int output_width, output_height;

    device_list->default_device = -1;


    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&pDXGIFactory);
    if (FAILED(hr)) {
        av_log(NULL, AV_LOG_ERROR, "Failed to get pDXGIFactory (%lx)\n", hr);
        return -1;
    }

    for (unsigned int adapter_index = 0;
        IDXGIFactory1_EnumAdapters(pDXGIFactory, adapter_index, (IDXGIAdapter **) &dxgi_adapter) == 0;
        adapter_index++) {
        for (unsigned int output_index = 0;
            IDXGIAdapter1_EnumOutputs(dxgi_adapter, output_index, (IDXGIOutput**) &dxgi_output) == 0;
            output_index++) {
            DXGI_OUTPUT_DESC output_desc;
            IDXGIOutput1_GetDesc(dxgi_output, &output_desc);
            MONITORINFO output_monitor = { .cbSize = sizeof(MONITORINFO) };
            GetMonitorInfo(output_desc.Monitor, &output_monitor);

            output_width = output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left;
            output_height = output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top;

            new_device = av_malloc(sizeof(AVDeviceInfo));

            new_device->device_name = av_malloc(4);
            // snprintf(new_device->device_name, 16, "%d:%d", adapter_index, output_index);
            snprintf(new_device->device_name, 4, "%d", output_index);

            new_device->device_description = av_malloc(128);
            snprintf(new_device->device_description, 128,
                     "Adapter %d, display %d: %S (%dx%d @ %ld,%ld%s)",
                     adapter_index+1, output_index+1, output_desc.DeviceName,
                     output_width, output_height,
                     output_desc.DesktopCoordinates.left, output_desc.DesktopCoordinates.top,
                     (output_monitor.dwFlags & 1) ? ", primary" : "");

            av_dynarray_add_nofree(&device_list->devices, &device_list->nb_devices, new_device);
            new_device = NULL;

            if (output_monitor.dwFlags & 1) {
                device_list->default_device = device_list->nb_devices - 1;
            }

            IDXGIOutput1_Release(dxgi_output);
        }
        IDXGIAdapter1_Release(dxgi_adapter);
    }
    IDXGIFactory1_Release(pDXGIFactory);

    return device_list->nb_devices;
}


#define OFFSET(x) offsetof(struct mondup_ctx, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "draw_mouse", "draw the mouse pointer", OFFSET(draw_mouse), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, DEC },
    // { "show_region", "draw border around capture area", OFFSET(show_region), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, DEC },
    { "framerate", "set video frame rate", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "30"}, 0, INT_MAX, DEC },
    { "video_size", "set video frame size", OFFSET(width), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0, 0, DEC },
    { "offset_x", "capture area x offset", OFFSET(offset_x), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, INT_MAX, DEC },
    { "offset_y", "capture area y offset", OFFSET(offset_y), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, INT_MAX, DEC },
    { "do-log", "enable debug logging", OFFSET(do_log), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, DEC },
    { NULL },
};

static const AVClass mondup_class = {
    .class_name = "mondup indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT,
};

/** gdi grabber device demuxer declaration */
AVInputFormat ff_mondup_demuxer = {
    .name           = "mondup",
    .long_name      = NULL_IF_CONFIG_SMALL("IDXGI API Windows desktop duplicator source"),
    .priv_data_size = sizeof(struct mondup_ctx),
    .read_header    = mondup_read_header,
    .read_packet    = mondup_read_packet,
    .read_close     = mondup_read_close,
    .get_device_list     = mondup_get_device_list,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &mondup_class,
};
