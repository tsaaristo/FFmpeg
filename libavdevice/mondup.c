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

/** TODO
rewrite mondup to use multiple regions (for multiple monitors)
handle dpi, handle rotation, draw mouse etc
*/
struct capture_region {
    IDXGIOutput1 *dxgi_output;
    DXGI_OUTPUT_DESC description;

    IDXGIOutputDuplication *output_duplication;
    IDXGIResource *duplicated_resource;
    DXGI_OUTDUPL_FRAME_INFO frame_info;

    int metadata_size;
    void *metadata;

    int width;
    int height;

    int src_x;
    int src_y;

    int dst_x;
    int dst_y;
};

/**
 * GDI Device Demuxer context
 */
struct mondup_ctx {
    const AVClass *class;   /**< Class for private options */

    // int        frame_size;  /**< Size in bytes of the frame pixel data */
    // int        header_size; /**< Size in bytes of the DIB header */
    // AVRational time_base;   /**< Time base */
    // int64_t    time_frame;  /**< Current time */

    // int        draw_mouse;  /**< Draw mouse cursor (private option) */
    // int        show_region; /**< Draw border (private option) */
    AVRational framerate;   /**< Capture framerate (private option) */
    AVRational time_base;   /**< Capture framerate (private option) */

    int        width;       /**< Widh of the grab frame (private option) */
    int        height;      /**< Height of the grab frame (private option) */
    int        offset_x;    /**< Capture x offset (private option) */
    int        offset_y;    /**< Capture y offset (private option) */

    // HWND       hwnd;        /**< Handle of the window for the grab */
    // HDC        source_hdc;  /**< Source device context */
    // HDC        dest_hdc;    /**< Destination, source-compatible DC */
    // BITMAPINFO bmi;         /**< Information describing DIB format */
    // HBITMAP    hbmp;        /**< Information on the bitmap captured */
    // void      *buffer;      /**< The buffer containing the bitmap image data */
    // RECT       clip_rect;   /**< The subarea of the screen or window to clip */

    // IDXGIFactory1 *dxgi_factory;
    // IDXGIOutput *dxgi_output_temp;

    // IDXGIAdapter1 *dxgi_adapter;
    // IDXGIOutput1 *dxgi_output;

    ID3D11Device *d3d11_device;
    ID3D11DeviceContext *d3d11_context;
    // IDXGIOutputDuplication *output_duplication;
    ID3D11Texture2D *duplicator_texture;

    int capture_region_count;
    struct capture_region *capture_regions;

    // IDXGIResource *duplicated_resource;
    // DXGI_OUTDUPL_FRAME_INFO frame_info;
    // int frame_metadata_size;
    // void *frame_metadata;

    // unsigned int output_width;
    // unsigned int output_height;
    // unsigned int frame_size; // w * h * 4;

    int out_width;
    int out_height;
    int out_size;

    // av_gettime_relative() timestamp
    int64_t start_time;
    // relative from start_time
    int64_t next_frame;

    // int frame_acquired;
    // int frame_mapped;

    D3D11_MAPPED_SUBRESOURCE mapped_texture;

    // HWND       region_hwnd; /**< Handle of the region border window */

    // int cursor_error_printed;
};

struct output_info {
    int adapter_index;
    int output_index;
    DXGI_OUTPUT_DESC description;
    MONITORINFO info;
};

#define ERR_LOG_GOTO(ERROR_VAL, ...) do {         \
    av_log(s1, AV_LOG_ERROR, __VA_ARGS__);        \
    ret = ERROR_VAL;                              \
    goto error;                                   \
} while(0)
#define IF_ERR_LOG_GOTO(CHECK, ERROR_VAL, ...) if (CHECK) ERR_LOG_GOTO(ERROR_VAL, __VA_ARGS__);

#define RECTS_OVERLAP(A, B)  ( ((A.left >= B.left && A.left <= B.right) || (A.right >= B.left && A.right <= B.right)) && ((A.top >= B.top && A.top <= B.bottom) || (A.bottom >= B.top && A.bottom <= B.bottom)) )
#define RECT_CONTAINED(A, B) ( ((A.left >= B.left && A.left <= B.right) && (A.right >= B.left && A.right <= B.right)) && ((A.top >= B.top && A.top <= B.bottom) && (A.bottom >= B.top && A.bottom <= B.bottom)) )

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




static int mondup_read_header(AVFormatContext *s1) {
    struct mondup_ctx *mondup_state = s1->priv_data;

    mondup_state->time_base = av_inv_q(mondup_state->framerate);

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

                // Set rect to start from display
                target_rect.left += mon_info->description.DesktopCoordinates.left;
                target_rect.top  += mon_info->description.DesktopCoordinates.top;
                if (mondup_state->width && mondup_state->height) {
                    target_rect.right += target_rect.left + mondup_state->width;
                    target_rect.bottom += target_rect.top + mondup_state->height;
                } else {
                    target_rect.right = target_rect.left + (mon_info->description.DesktopCoordinates.right - mon_info->description.DesktopCoordinates.left);
                    target_rect.bottom = target_rect.top + (mon_info->description.DesktopCoordinates.bottom - mon_info->description.DesktopCoordinates.top);
                }
            }
        // }
    }
    // IDXGIFactory1_Release(dxgi_factory);

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

        if (mon_rect.left < full_extents.left)
            full_extents.left = mon_rect.left;
        if (mon_rect.top < full_extents.top)
            full_extents.top = mon_rect.top;
        if (mon_rect.right > full_extents.right)
            full_extents.right = mon_rect.right;
        if (mon_rect.bottom > full_extents.bottom)
            full_extents.bottom = mon_rect.bottom;

        if ((target_rect.left >= mon_rect.left && target_rect.left <= mon_rect.right)
            && (target_rect.top >= mon_rect.top && target_rect.top <= mon_rect.bottom)) {
            initial_monitor = i;
        }

        // av_log(s1, AV_LOG_ERROR, "%d (%ld,%ld %ld,%ld)\n",
        //     i,
        //     full_extents.left, full_extents.top, full_extents.right, full_extents.bottom);
    }

    if (mondup_state->width && mondup_state->height) {
        target_rect.right += target_rect.left + mondup_state->width;
        target_rect.bottom += target_rect.top + mondup_state->height;
    } else if (initial_monitor >= 0) {
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

    // Warm up duplicator(s)
    mondup_state->d3d11_device = d3d11_device;
    mondup_state->d3d11_context = d3d11_context;

    for (int i = 0; i < mondup_state->capture_region_count; ++i) {
        int get_frame_res;
        struct capture_region *cap_reg = (mondup_state->capture_regions + i);
        while(cap_reg->frame_info.AccumulatedFrames == 0) {
            get_frame_res = mondup_get_frame(s1, cap_reg, 0);
            // av_log(s1, AV_LOG_ERROR, "got %d.\n", get_frame_res);
            IF_ERR_LOG_GOTO(get_frame_res < 0, get_frame_res, "Duplicator warmup failed\n");
        }
    }

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
    st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->format = AV_PIX_FMT_BGRA;
    st->codecpar->codec_tag = avcodec_pix_fmt_to_codec_tag(st->codecpar->format);

    st->codecpar->width = mondup_state->out_width;
    st->codecpar->height = mondup_state->out_height;
    st->codecpar->bit_rate = av_rescale(mondup_state->out_size, mondup_state->time_base.den, mondup_state->time_base.num);

    st->r_frame_rate = av_inv_q(mondup_state->time_base);
    st->avg_frame_rate = av_inv_q(mondup_state->time_base);

    // Finalize state
    mondup_state->start_time = av_gettime_relative();
    mondup_state->next_frame = 0;

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

static int mondup_read_packet(AVFormatContext *s1, AVPacket *pkt) {
    struct mondup_ctx *mondup_state = s1->priv_data;

    int64_t current_time, delay;

    HRESULT hr;
    int get_frame_res;
    // DXGI_OUTDUPL_FRAME_INFO frame_info;
    // IDXGIResource *duplicated_resource;

    for (int i = 0; i < mondup_state->capture_region_count; ++i) {
        struct capture_region *cap_reg = (mondup_state->capture_regions + i);

        get_frame_res = mondup_get_frame(s1, cap_reg, 0);
        if (get_frame_res < 0) {
            return get_frame_res;
        }
    }

    current_time = av_gettime_relative() - mondup_state->start_time;

    delay = mondup_state->next_frame - current_time;
    if (delay > 0) {
        av_usleep(delay);
    }
    mondup_state->next_frame += INT64_C(1000000) * av_q2d(mondup_state->time_base);

    if (av_new_packet(pkt, mondup_state->out_size) < 0)
        return AVERROR(ENOMEM);
    pkt->pts = current_time;

    hr = ID3D11DeviceContext_Map(mondup_state->d3d11_context,
        (ID3D11Resource *)mondup_state->duplicator_texture,
        0, D3D11_MAP_READ, 0, &mondup_state->mapped_texture );
    if (hr) {
        av_log(s1, AV_LOG_ERROR, "Unable to map texture (%lx).\n", hr);
        // IDXGIOutputDuplication_ReleaseFrame(mondup_state->output_duplication);
        return AVERROR_EXIT;
    }

    if (mondup_state->mapped_texture.RowPitch == mondup_state->out_width*4) {
        // pkt->data = mondup_state->mapped_texture.pData;
        memcpy(pkt->data, mondup_state->mapped_texture.pData, mondup_state->out_size);
    } else {
        unsigned int row_bytes = mondup_state->out_width * 4;
        uint8_t *src_ptr = (uint8_t *)mondup_state->mapped_texture.pData;
        uint8_t *dst_ptr = pkt->data;

        for (unsigned int y = 0; y < mondup_state->out_height; y++) {
            memcpy(dst_ptr, src_ptr, row_bytes);
            src_ptr += mondup_state->mapped_texture.RowPitch;
            dst_ptr += row_bytes;
        }
    }

    ID3D11DeviceContext_Unmap(mondup_state->d3d11_context, (ID3D11Resource *)mondup_state->duplicator_texture, 0);

    return mondup_state->out_size;
}

static int mondup_read_close(AVFormatContext *s1) {
    struct mondup_ctx *mondup_state = s1->priv_data;

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

    ID3D11Texture2D_Release(mondup_state->duplicator_texture);
    ID3D11DeviceContext_Release(mondup_state->d3d11_context);
    ID3D11Device_Release(mondup_state->d3d11_device);

    return 0;
}


#define OFFSET(x) offsetof(struct mondup_ctx, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    // { "draw_mouse", "draw the mouse pointer", OFFSET(draw_mouse), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, DEC },
    // { "show_region", "draw border around capture area", OFFSET(show_region), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, DEC },
    { "framerate", "set video frame rate", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "30"}, 0, INT_MAX, DEC },
    { "video_size", "set video frame size", OFFSET(width), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0, 0, DEC },
    { "offset_x", "capture area x offset", OFFSET(offset_x), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, INT_MAX, DEC },
    { "offset_y", "capture area y offset", OFFSET(offset_y), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, INT_MAX, DEC },
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
    // .get_device_list     = mondup_get_device_list,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &mondup_class,
};
