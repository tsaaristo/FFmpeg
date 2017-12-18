/*
 * GDI video grab interface
 *
 * This file is part of FFmpeg.
 *
 * Copyright (C) 2013 Calvin Walton <calvin.walton@kepstin.ca>
 * Copyright (C) 2007-2010 Christophe Gisquet <word1.word2@gmail.com>
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * GDI frame device demuxer
 * @author Calvin Walton <calvin.walton@kepstin.ca>
 * @author Christophe Gisquet <word1.word2@gmail.com>
 */

#include "config.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/mem.h"
#include "avdevice.h"

#include <windows.h>

#if !defined(_WIN32_WINNT_WIN8) || _WIN32_WINNT_WIN8 < 0x0602
#undef _WIN32_WINNT_WIN8
#define _WIN32_WINNT_WIN8 0x0602
#endif

#define COBJMACROS

#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

/**
 * GDI Device Demuxer context
 */
struct mondup {
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

    IDXGIFactory1 *dxgi_factory;
    IDXGIOutput *dxgi_output_temp;

    IDXGIAdapter1 *dxgi_adapter;
    IDXGIOutput1 *dxgi_output;

    ID3D11Device *d3d11_device;
    ID3D11DeviceContext *d3d11_context;
    IDXGIOutputDuplication *output_duplication;
    ID3D11Texture2D *duplicator_texture;

    unsigned int output_width;
    unsigned int output_height;
    unsigned int frame_size; // w * h * 4;

    int64_t start_time;
    int64_t next_frame;
    int frame_acquired;
    int frame_mapped;

    D3D11_MAPPED_SUBRESOURCE mapped_texture;

    // HWND       region_hwnd; /**< Handle of the region border window */

    // int cursor_error_printed;
};

#define WIN32_API_ERROR(str)                                            \
    av_log(s1, AV_LOG_ERROR, str " (error %li)\n", GetLastError())

#define REGION_WND_BORDER 3


/**
 * Initializes the gdi grab device demuxer (public device demuxer API).
 *
 * @param s1 Context from avformat core
 * @return AVERROR_IO error, 0 success
 */
static int
mondup_read_header(AVFormatContext *s1)
{
    struct mondup *mondup_state = s1->priv_data;

    int ret;
    HRESULT hr;


    AVStream *st = NULL;

    IDXGIFactory1 *dxgi_factory = NULL;
    IDXGIAdapter1 *dxgi_adapter = NULL;
    IDXGIOutput *dxgi_output_temp = NULL;
    IDXGIOutput1 *dxgi_output = NULL;

    ID3D11Device *d3d11_device = NULL;
    ID3D11DeviceContext *d3d11_context = NULL;
    IDXGIOutputDuplication *output_duplication = NULL;
    ID3D11Texture2D *duplicator_texture = NULL;

    const char *filename = s1->filename;
    unsigned int adapter_index;
    unsigned int output_index;
    int use_primary_display = 0;

    char *filename_colon = strchr(filename, ':');
    av_log(s1, AV_LOG_ERROR, "Filename '%s'.\n", filename);

    if (strcmp(filename, "primary") == 0) {
        use_primary_display = 1;
    } else if (filename_colon)  {
        *filename_colon = 0;

        adapter_index = strtol(filename, NULL, 10);
        output_index = strtol(filename_colon + 1, NULL, 10);

    } else {
        av_log(s1, AV_LOG_ERROR,
            "Specify target display using 'primary' or <adapter>:<display>"
            " (eg. 0:0).\nList displays using -sources mondup\n");
        ret = AVERROR_EXIT;
        goto error;
    }

    // if (!use_primary_display && (adapter_index < 0 || output_index < 0)) {
    //     av_log(s1, AV_LOG_ERROR, "Adapter and display IDs must not be negative.\n");
    //     ret = AVERROR_EXIT;
    //     goto error;
    // }

    if (CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&dxgi_factory)) {
        av_log(s1, AV_LOG_ERROR, "Unable to acquire a DXGIFactory.\n");
        ret = AVERROR_EXIT;
        goto error;
    }

    if (use_primary_display) {
        int display_found = 0;

        // Enumerate until we find out the primary display
        for (adapter_index = 0;
            !display_found && IDXGIFactory1_EnumAdapters1(dxgi_factory, adapter_index, &dxgi_adapter) == 0;
            adapter_index++) {

            for (output_index = 0;
                !display_found && IDXGIAdapter1_EnumOutputs(dxgi_adapter, output_index, &dxgi_output_temp) == 0;
                output_index++) {

                DXGI_OUTPUT_DESC output_desc;
                IDXGIOutput1_GetDesc(dxgi_output_temp, &output_desc);
                MONITORINFO output_monitor = { .cbSize = sizeof(MONITORINFO) };
                GetMonitorInfo(output_desc.Monitor, &output_monitor);

                if (output_monitor.dwFlags & 1) {
                    display_found = 1;
                }

                if (!display_found) {
                    IDXGIOutput1_Release(dxgi_output_temp);
                }
            }

            if (!display_found) {
                IDXGIAdapter1_Release(dxgi_adapter);
            }
        }

        if (!display_found) {
            av_log(s1, AV_LOG_ERROR, "Unable to find primary display.\n");
            ret = AVERROR_EXIT;
            goto error;
        }

    } else {
        // Try to grab a single display
        if (IDXGIFactory1_EnumAdapters1(dxgi_factory, adapter_index, &dxgi_adapter)) {
            av_log(s1, AV_LOG_ERROR, "Unable to acquire adapter %d.\n", adapter_index);
            ret = AVERROR_EXIT;
            goto error;
        }

        if (IDXGIAdapter1_EnumOutputs(dxgi_adapter, output_index, &dxgi_output_temp)) {
            av_log(s1, AV_LOG_ERROR, "Unable to acquire display %d.\n", output_index);
            ret = AVERROR_EXIT;
            goto error;
        }

    }

    if (IDXGIOutput_QueryInterface(dxgi_output_temp, &IID_IDXGIOutput1, (void**) &dxgi_output)) {
        av_log(s1, AV_LOG_ERROR, "Unable to upgrade display.\n");
        ret = AVERROR_EXIT;
        goto error;
    }

    DXGI_OUTPUT_DESC output_desc;
    IDXGIOutput1_GetDesc(dxgi_output, &output_desc);
    int output_width = output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left;
    int output_height = output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top;
    if (!mondup_state->width || !mondup_state->height) {
        mondup_state->width = output_width;
        mondup_state->height = output_height;
    }

    if (mondup_state->offset_x < 0 || mondup_state->offset_y < 0 ) {
        av_log(s1, AV_LOG_ERROR, "offset_x/offset_y must not be negative\n");
        ret = AVERROR_EXIT;
        goto error;
    } else if (mondup_state->width + mondup_state->offset_x > output_width || mondup_state->height + mondup_state->offset_y > output_height) {
        av_log(s1, AV_LOG_ERROR, "Target rectangle must not go over screen bounds\n");
        ret = AVERROR_EXIT;
        goto error;
    }

    // Init the device
    D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_1
    };
    D3D_FEATURE_LEVEL feature_level;
    hr = D3D11CreateDevice((IDXGIAdapter1 *) dxgi_adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                           D3D11_CREATE_DEVICE_SINGLETHREADED, feature_levels, 5,
                           // 0, feature_levels, 5,
                           D3D11_SDK_VERSION, &d3d11_device, &feature_level, &d3d11_context);
    if (hr) {
        av_log(s1, AV_LOG_ERROR, "Failed to create Direct3D device for %d:%d (%lx).\n",
               adapter_index, output_index, hr);
        ret = AVERROR_EXIT;
        goto error;
    }

    D3D11_TEXTURE2D_DESC texture_desc = {
        .Width = mondup_state->width,
        .Height = mondup_state->height,
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
        // .CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE,
        .CPUAccessFlags = D3D11_CPU_ACCESS_READ,
        .MiscFlags  = 0,
    };
    hr = ID3D11Device_CreateTexture2D(d3d11_device, &texture_desc, NULL, &duplicator_texture);
    if (hr) {
        av_log(s1, AV_LOG_ERROR, "Failed to create Texture2D (%lx).\n", hr);
        ret = AVERROR_EXIT;
        goto error;
    }

    hr = IDXGIOutput1_DuplicateOutput(dxgi_output, (IUnknown *) d3d11_device, &output_duplication);
    if (hr) {
        av_log(s1, AV_LOG_ERROR, "Unable to create output duplicator (%lx).\n", hr);
        ret = AVERROR_EXIT;
        goto error;
    }

    DXGI_OUTDUPL_DESC out_desc;
    IDXGIOutputDuplication_GetDesc(dxgi_output, &out_desc);
    av_log(s1, AV_LOG_ERROR, "ModeDesc: %ux%u@%u/%u fmt:%x scn:%d scl:%d\nRot: %d Inmem: %d\n",
        out_desc.ModeDesc.Width, out_desc.ModeDesc.Height, out_desc.ModeDesc.RefreshRate.Numerator, out_desc.ModeDesc.RefreshRate.Denominator, out_desc.ModeDesc.Format, out_desc.ModeDesc.ScanlineOrdering, out_desc.ModeDesc.Scaling,
        out_desc.Rotation,
        out_desc.DesktopImageInSystemMemory
        );


    // Grab first frame
    IDXGIResource *duplicated_resource;
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    hr = IDXGIOutputDuplication_AcquireNextFrame(output_duplication, 500, &frame_info, &duplicated_resource);
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        av_log(s1, AV_LOG_ERROR, "initial Lost access to display.\n");
        return AVERROR(EIO);
    } else if (hr && hr != DXGI_ERROR_WAIT_TIMEOUT) {
        av_log(s1, AV_LOG_ERROR, "duplicator failed initial (%lx).\n", hr);
        return AVERROR(EIO);
    }
    IDXGIOutputDuplication_ReleaseFrame(mondup_state->output_duplication);
    mondup_state->frame_acquired = 0;

    mondup_state->dxgi_factory = dxgi_factory;
    mondup_state->dxgi_adapter = dxgi_adapter;
    mondup_state->dxgi_output_temp = dxgi_output_temp;
    mondup_state->dxgi_output = dxgi_output;

    mondup_state->d3d11_device = d3d11_device;
    mondup_state->d3d11_context = d3d11_context;
    mondup_state->output_duplication = output_duplication;
    mondup_state->duplicator_texture = duplicator_texture;

    mondup_state->frame_size = mondup_state->width * mondup_state->height * 4;
    mondup_state->output_width = output_width;
    mondup_state->output_height = output_height;

    mondup_state->time_base = av_inv_q(mondup_state->framerate);
    mondup_state->start_time = av_gettime_relative();
    mondup_state->next_frame = 0;

    st = avformat_new_stream(s1, NULL);
    if (!st) {
        av_log(s1, AV_LOG_ERROR, "Cannot add stream\n");
        ret = AVERROR(ENOMEM);
        goto error;
    }
    avpriv_set_pts_info(st, 64, 1, 1000000); /* 64 bits pts in us */

    // Setup video stream
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->format = AV_PIX_FMT_BGRA;
    st->codecpar->codec_tag = avcodec_pix_fmt_to_codec_tag(st->codecpar->format);

    st->codecpar->width = mondup_state->width;
    st->codecpar->height = mondup_state->height;
    st->codecpar->bit_rate = av_rescale(mondup_state->frame_size, mondup_state->time_base.den, mondup_state->time_base.num);

    st->r_frame_rate = av_inv_q(mondup_state->time_base);
    st->avg_frame_rate = av_inv_q(mondup_state->time_base);

    // av_log(s1, AV_LOG_ERROR, "Created state successfully: %dx%d.\n", mondup_state->width, mondup_state->height);

    return 0;

error:
    if (dxgi_factory)
        IDXGIFactory1_Release(dxgi_factory);
    if (dxgi_adapter)
        IDXGIAdapter1_Release(dxgi_adapter);
    if (dxgi_output_temp)
        IDXGIAdapter1_Release(dxgi_output_temp);
    if (dxgi_output)
        IDXGIAdapter1_Release(dxgi_output);
    if (d3d11_device)
        ID3D11Device_Release(d3d11_device);
    if (output_duplication)
        IDXGIOutputDuplication_Release(output_duplication);
    return ret;
}

/**
 * Paints a mouse pointer in a Win32 image.
 *
 * @param s1 Context of the log information
 * @param s  Current grad structure
 */
// static void paint_mouse_pointer(AVFormatContext *s1, struct mondup *mondup)
// {
//     CURSORINFO ci = {0};

// #define CURSOR_ERROR(str)                 \
//     if (!mondup->cursor_error_printed) {       \
//         WIN32_API_ERROR(str);             \
//         mondup->cursor_error_printed = 1;      \
//     }

//     ci.cbSize = sizeof(ci);

//     if (GetCursorInfo(&ci)) {
//         HCURSOR icon = CopyCursor(ci.hCursor);
//         ICONINFO info;
//         POINT pos;
//         RECT clip_rect = mondup->clip_rect;
//         HWND hwnd = mondup->hwnd;
//         int vertres = GetDeviceCaps(mondup->source_hdc, VERTRES);
//         int desktopvertres = GetDeviceCaps(mondup->source_hdc, DESKTOPVERTRES);
//         info.hbmMask = NULL;
//         info.hbmColor = NULL;

//         if (ci.flags != CURSOR_SHOWING)
//             return;

//         if (!icon) {
//             /* Use the standard arrow cursor as a fallback.
//              * You'll probably only hit this in Wine, which can't fetch
//              * the current system cursor. */
//             icon = CopyCursor(LoadCursor(NULL, IDC_ARROW));
//         }

//         if (!GetIconInfo(icon, &info)) {
//             CURSOR_ERROR("Could not get icon info");
//             goto icon_error;
//         }

//         pos.x = ci.ptScreenPos.x - clip_rect.left - info.xHotspot;
//         pos.y = ci.ptScreenPos.y - clip_rect.top - info.yHotspot;

//         if (hwnd) {
//             RECT rect;

//             if (GetWindowRect(hwnd, &rect)) {
//                 pos.x -= rect.left;
//                 pos.y -= rect.top;
//             } else {
//                 CURSOR_ERROR("Couldn't get window rectangle");
//                 goto icon_error;
//             }
//         }

//         //that would keep the correct location of mouse with hidpi screens
//         pos.x = pos.x * desktopvertres / vertres;
//         pos.y = pos.y * desktopvertres / vertres;

//         av_log(s1, AV_LOG_DEBUG, "Cursor pos (%li,%li) -> (%li,%li)\n",
//                 ci.ptScreenPos.x, ci.ptScreenPos.y, pos.x, pos.y);

//         if (pos.x >= 0 && pos.x <= clip_rect.right - clip_rect.left &&
//                 pos.y >= 0 && pos.y <= clip_rect.bottom - clip_rect.top) {
//             if (!DrawIcon(mondup->dest_hdc, pos.x, pos.y, icon))
//                 CURSOR_ERROR("Couldn't draw icon");
//         }

// icon_error:
//         if (info.hbmMask)
//             DeleteObject(info.hbmMask);
//         if (info.hbmColor)
//             DeleteObject(info.hbmColor);
//         if (icon)
//             DestroyCursor(icon);
//     } else {
//         CURSOR_ERROR("Couldn't get cursor info");
//     }
// }

static void log_texture(AVFormatContext *s1, ID3D11Texture2D *tex_addr) {
    D3D11_TEXTURE2D_DESC texture_desc;
    ID3D11Texture2D_GetDesc(tex_addr, &texture_desc);
    // av_log(s1, AV_LOG_ERROR, "Got texture: %dx%d usage:%x bind:%x fmt:%x cpu:%x\n",
        // texture_desc.Width, texture_desc.Height,
        // texture_desc.Usage, texture_desc.BindFlags, texture_desc.Format, texture_desc.CPUAccessFlags);
    av_log(s1, AV_LOG_ERROR, "Texture2D %p:\n"
                             " Size: %dx%d Format: 0x%x\n"
                             " Mip: %d ArraySize: 0x%x\n"
                             " SampleCount: %u SampleQuality: %u\n"
                             " Usage: 0x%x Bind: 0x%x Access: 0x%x\n"
                             " Misc: 0x%x\n",
        (void *) tex_addr,
        texture_desc.Width, texture_desc.Height, texture_desc.Format,
        texture_desc.MipLevels, texture_desc.ArraySize,
        texture_desc.SampleDesc.Count, texture_desc.SampleDesc.Quality,
        texture_desc.Usage, texture_desc.BindFlags, texture_desc.CPUAccessFlags,
        texture_desc.MiscFlags);

}

/**
 * Grabs a frame from gdi (public device demuxer API).
 *
 * @param s1 Context from avformat core
 * @param pkt Packet holding the grabbed frame
 * @return frame size in bytes
 */
static int mondup_read_packet(AVFormatContext *s1, AVPacket *pkt)
{
    struct mondup *mondup_state = s1->priv_data;

    // av_log(s1, AV_LOG_ERROR, "Read Packet %ld.\n", mondup_state->next_frame);
    // av_log(s1, AV_LOG_ERROR, "Timebase %d/%d.\n", mondup_state->time_base.den, mondup_state->time_base.num);

    int64_t current_time, delay;

    HRESULT hr;
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    IDXGIResource *duplicated_resource;

    for (;;) {
        current_time = av_gettime_relative() - mondup_state->start_time;

        delay = mondup_state->next_frame - current_time;
        // av_log(s1, AV_LOG_ERROR, "Now delay:%lld cur:%lld start:%lld frame:%lld.\n", delay, current_time, mondup_state->start_time, INT64_C(1000000) * av_q2d(mondup_state->time_base));
        if (delay <= 0) {
            mondup_state->next_frame = current_time + INT64_C(1000000) * av_q2d(mondup_state->time_base);
            break;
        }
        av_usleep(delay);
    }

    if (mondup_state->frame_acquired) {
        // av_log(s1, AV_LOG_ERROR, "Releasing frame\n");
        IDXGIOutputDuplication_ReleaseFrame(mondup_state->output_duplication);
        mondup_state->frame_acquired = 0;
    }

    int64_t start_time1 = av_gettime_relative();
    // av_log(s1, AV_LOG_ERROR, "Acquiring frame\n");
    hr = IDXGIOutputDuplication_AcquireNextFrame(mondup_state->output_duplication, 0, &frame_info, &duplicated_resource);
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        av_log(s1, AV_LOG_ERROR, "Lost access to display.\n");
        return AVERROR(EIO);
    } else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // av_log(s1, AV_LOG_ERROR, "duplicator timed out.\n");
        // return AVERROR(EIO);
        if (!mondup_state->frame_mapped) {
            ID3D11DeviceContext_Map(mondup_state->d3d11_context,
                                    (ID3D11Resource *)mondup_state->duplicator_texture,
                                    0, D3D11_MAP_READ, 0, &mondup_state->mapped_texture );
            mondup_state->frame_mapped = 1;
        }
        if (av_new_packet(pkt, mondup_state->frame_size) < 0)
            return AVERROR(ENOMEM);
        pkt->pts = current_time;
        if (mondup_state->mapped_texture.RowPitch == mondup_state->width*4) {
            pkt->data = mondup_state->mapped_texture.pData;
        } else {
            unsigned int row_bytes = mondup_state->width * 4;
            uint8_t *src_ptr = (uint8_t *)mondup_state->mapped_texture.pData;
            uint8_t *dst_ptr = pkt->data;
            for (unsigned int y = 0; y < mondup_state->height; y++) {
                memcpy(dst_ptr, src_ptr, row_bytes);
                src_ptr += mondup_state->mapped_texture.RowPitch;
                dst_ptr += row_bytes;
            }
        }
        return pkt->size;
    } else if (hr) {
        av_log(s1, AV_LOG_ERROR, "duplicator failed (%lx).\n", hr);
        return AVERROR(EIO);
    }
    int64_t start_time = av_gettime_relative();
    if (av_new_packet(pkt, mondup_state->frame_size) < 0)
        return AVERROR(ENOMEM);
    pkt->pts = current_time;

    mondup_state->frame_acquired = 1;
    // av_log(s1, AV_LOG_ERROR, "Got frame: %ld, %u %d,%d\n",
    //         frame_info.LastPresentTime, frame_info.AccumulatedFrames,
    //         frame_info.PointerPosition.Position.x,
    //         frame_info.PointerPosition.Position.y);

    ID3D11Texture2D *frame_texture;
    // av_log(s1, AV_LOG_ERROR, "Asking for texture\n");
    hr = IDXGIResource_QueryInterface(duplicated_resource, &IID_ID3D11Texture2D, (void **)&frame_texture);
    if (hr) {
        av_log(s1, AV_LOG_ERROR, "Unable to attain texture (%lx).\n", hr);
        IDXGIOutputDuplication_ReleaseFrame(mondup_state->output_duplication);
        return AVERROR(EIO);
    }

    // if (!mondup_state->duplicator_texture) {
    //     D3D11_TEXTURE2D_DESC duplicator_texture_desc;
    //     av_log(s1, AV_LOG_ERROR, "Creating Texture2D...\n");

    // }

    // D3D11_TEXTURE2D_DESC texture_desc;
    // ID3D11Texture2D_GetDesc(frame_texture, &texture_desc);
    // av_log(s1, AV_LOG_ERROR, "Got texture: %dx%d usage:%x bind:%x fmt:%x cpu:%x\n",
        // texture_desc.Width, texture_desc.Height,
        // texture_desc.Usage, texture_desc.BindFlags, texture_desc.Format, texture_desc.CPUAccessFlags);
    // log_texture(s1, frame_texture);
    // log_texture(s1, mondup_state->duplicator_texture);

    // D3D11_MAPPED_SUBRESOURCE mapped_texture;
    // av_log(s1, AV_LOG_ERROR, "Mapping texture1\n");
    // // hr = ID3D11DeviceContext_Map(mondup_state->d3d11_context, (ID3D11Resource *)frame_texture,
    // hr = ID3D11DeviceContext_Map(mondup_state->d3d11_context,
    //     (ID3D11Resource *)mondup_state->duplicator_texture,
    //     0, D3D11_MAP_READ, 0, &mapped_texture );
    // if (hr) {
    //     av_log(s1, AV_LOG_ERROR, "Unable to map texture (%lx).\n", hr);
    //     IDXGIOutputDuplication_ReleaseFrame(mondup_state->output_duplication);
    //     return AVERROR_EXIT;
    // }
    // uint32_t *t_Asdas = (uint32_t *)mapped_texture.pData;

    // av_log(s1, AV_LOG_ERROR, "First four source: %08X %08X %08X %08X\n", *t_Asdas, *(t_Asdas+1), *(t_Asdas+2), *(t_Asdas+3));
    // ID3D11DeviceContext_Unmap(mondup_state->d3d11_context, (ID3D11Resource *)mondup_state->duplicator_texture, 0);

    // av_log(s1, AV_LOG_ERROR, "Copying texture\n");
    // ID3D11DeviceContext_CopySubresourceRegion(mondup_state->d3d11_context,
    //                                           (ID3D11Resource *) mondup_state->duplicator_texture, 0, 0, 0, 0,
    //                                           (ID3D11Resource *) frame_texture, 0, NULL);
    if (mondup_state->frame_mapped) {
        ID3D11DeviceContext_Unmap(mondup_state->d3d11_context, (ID3D11Resource *)mondup_state->duplicator_texture, 0);
        mondup_state->frame_mapped = 0;
    }
    // ID3D11DeviceContext_CopyResource(mondup_state->d3d11_context,
    //                                  (ID3D11Resource *) mondup_state->duplicator_texture,
    //                                  (ID3D11Resource *) frame_texture);'
    D3D11_BOX src_box = {
        .left = mondup_state->offset_x,
        .top = mondup_state->offset_y,
        .front = 0,
        .right = mondup_state->offset_x + mondup_state->width,
        .bottom = mondup_state->offset_y + mondup_state->height,
        .back = 1,
    };
    ID3D11DeviceContext_CopySubresourceRegion(mondup_state->d3d11_context,
                                              (ID3D11Resource *) mondup_state->duplicator_texture, 0, 0, 0, 0,
                                              (ID3D11Resource *) frame_texture, 0, &src_box);
    IDXGIOutputDuplication_ReleaseFrame(mondup_state->output_duplication);
    mondup_state->frame_acquired = 0;
    // av_log(s1, AV_LOG_ERROR, "Flush...\n");
    // ID3D11DeviceContext_Flush(mondup_state->d3d11_context);
    // av_log(s1, AV_LOG_ERROR, "Ok, done, releasing frame tex.\n");
    // IDXGIResource_Release(duplicated_resource);
    // ID3D11Texture2D_Release(frame_texture);

    // D3D11_MAPPED_SUBRESOURCE mapped_texture;
    // av_log(s1, AV_LOG_ERROR, "Mapping texture2\n");
    // hr = ID3D11DeviceContext_Map(mondup_state->d3d11_context, (ID3D11Resource *)frame_texture,
    hr = ID3D11DeviceContext_Map(mondup_state->d3d11_context,
        (ID3D11Resource *)mondup_state->duplicator_texture,
        0, D3D11_MAP_READ, 0, &mondup_state->mapped_texture );
    if (hr) {
        av_log(s1, AV_LOG_ERROR, "Unable to map texture (%lx).\n", hr);
        IDXGIOutputDuplication_ReleaseFrame(mondup_state->output_duplication);
        return AVERROR_EXIT;
    }
    mondup_state->frame_mapped = 1;
    IDXGIResource_Release(duplicated_resource);
    ID3D11Texture2D_Release(frame_texture);

    // if (0) {
    //     unsigned int row_count = mondup_state->height;
        // unsigned int row_bytes = mondup_state->width * 4;

    //     unsigned int copied_bytes = 0;
    //     unsigned int total_bytes = mondup_state->frame_size;

    //     uint32_t *txt_src = (uint32_t *)mapped_texture.pData;
    //     uint32_t *txt_dst = (uint32_t *)pkt->data;

    //     // av_log(s1, AV_LOG_ERROR, "First four source: %08X %08X %08X %08X\n", *txt_src, *(txt_src+1), *(txt_src+2), *(txt_src+3));
    //     // av_log(s1, AV_LOG_ERROR, "First four dest:   %08X %08X %08X %08X\n", *txt_dst, *(txt_dst+1), *(txt_dst+2), *(txt_dst+3));
    //     // av_log(s1, AV_LOG_ERROR, "Copying texture\n");

    //     uint8_t *src_ptr = (uint8_t *)mapped_texture.pData;
    //     uint8_t *dst_ptr = pkt->data;
    //     for (unsigned int y = 0; y < row_count; y++) {
    //         uint8_t *old_src_ptr = src_ptr;
    //         uint8_t *old_dst_ptr = dst_ptr;

    //         // uint8_t *src_ptr = (uint8_t *)mapped_texture.pData + mapped_texture.RowPitch * y;
    //         // uint8_t *dst_ptr = pkt->data + copied_bytes; //mondup_state->height * y * 4;

    //         memcpy(dst_ptr, src_ptr, row_bytes);

    //         src_ptr += mapped_texture.RowPitch;
    //         dst_ptr += row_bytes;
    //         copied_bytes += row_bytes;

    //         // av_log(s1, AV_LOG_ERROR, "%p (%d) to %p (%d) %d %d/%d\n",
    //             // src_ptr, (src_ptr - old_src_ptr), dst_ptr, (dst_ptr - old_dst_ptr), row_bytes, copied_bytes, total_bytes);
    //     }
    //     // av_log(s1, AV_LOG_ERROR, "Unmapping texture\n");
    //     // ID3D11DeviceContext_Unmap(mondup_state->d3d11_context, (ID3D11Resource *)frame_texture, 0);
    //     ID3D11DeviceContext_Unmap(mondup_state->d3d11_context, (ID3D11Resource *)mondup_state->duplicator_texture, 0);
    // }
    if (mondup_state->mapped_texture.RowPitch == mondup_state->width*4) {
        pkt->data = mondup_state->mapped_texture.pData;
    } else {
        // av_log(s1, AV_LOG_ERROR, "Bad stripe-width %d %d.\n", mondup_state->mapped_texture.RowPitch, mondup_state->width);
        // return AVERROR_EXIT;
        unsigned int row_bytes = mondup_state->width * 4;
        uint8_t *src_ptr = (uint8_t *)mondup_state->mapped_texture.pData;
        uint8_t *dst_ptr = pkt->data;
        for (unsigned int y = 0; y < mondup_state->height; y++) {
            memcpy(dst_ptr, src_ptr, row_bytes);
            src_ptr += mondup_state->mapped_texture.RowPitch;
            dst_ptr += row_bytes;
        }
    }

    int64_t end_time = av_gettime_relative();
    // av_log(s1, AV_LOG_ERROR, "Took %ldms (%ld from acquire)\n", (end_time - start_time) / 1000, (end_time - start_time1) / 1000);

    // av_log(s1, AV_LOG_ERROR, "First four sourc2: %08X %08X %08X %08X\n", *txt_src, *(txt_src+1), *(txt_src+2), *(txt_src+3));
    // av_log(s1, AV_LOG_ERROR, "First four dest2:  %08X %08X %08X %08X\n", *txt_dst, *(txt_dst+1), *(txt_dst+2), *(txt_dst+3));


    // curtime = av_gettime();
    return mondup_state->frame_size;
}

/**
 * Closes gdi frame grabber (public device demuxer API).
 *
 * @param s1 Context from avformat core
 * @return 0 success, !0 failure
 */
static int mondup_read_close(AVFormatContext *s1)
{
    struct mondup *mondup_state = s1->priv_data;

    if (mondup_state->dxgi_factory)
        IDXGIFactory1_Release(mondup_state->dxgi_factory);
    if (mondup_state->dxgi_adapter)
        IDXGIAdapter1_Release(mondup_state->dxgi_adapter);
    if (mondup_state->dxgi_output_temp)
        IDXGIAdapter1_Release(mondup_state->dxgi_output_temp);
    if (mondup_state->dxgi_output)
        IDXGIAdapter1_Release(mondup_state->dxgi_output);
    if (mondup_state->d3d11_device)
        ID3D11Device_Release(mondup_state->d3d11_device);
    if (mondup_state->output_duplication)
        IDXGIOutputDuplication_Release(mondup_state->output_duplication);
    // struct mondup *s = s1->priv_data;

    // // if (s->show_region)
    //     // mondup_region_wnd_destroy(s1, s);

    // if (s->source_hdc)
    //     ReleaseDC(s->hwnd, s->source_hdc);
    // if (s->dest_hdc)
    //     DeleteDC(s->dest_hdc);
    // if (s->hbmp)
    //     DeleteObject(s->hbmp);
    // if (s->source_hdc)
    //     DeleteDC(s->source_hdc);

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

    for (unsigned int adapterIndex = 0;
        IDXGIFactory1_EnumAdapters(pDXGIFactory, adapterIndex, (IDXGIAdapter **) &dxgi_adapter) == 0;
        adapterIndex++)
    {
        for (unsigned int output_index = 0;
            IDXGIAdapter1_EnumOutputs(dxgi_adapter, output_index, (IDXGIOutput**) &dxgi_output) == 0;
            output_index++)
        {
            DXGI_OUTPUT_DESC output_desc;
            IDXGIOutput1_GetDesc(dxgi_output, &output_desc);
            MONITORINFO output_monitor = { .cbSize = sizeof(MONITORINFO) };
            GetMonitorInfo(output_desc.Monitor, &output_monitor);

            output_width = output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left;
            output_height = output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top;

            new_device = av_malloc(sizeof(AVDeviceInfo));

            new_device->device_name = av_malloc(16);
            snprintf(new_device->device_name, 16, "%d:%d", adapterIndex, output_index);

            new_device->device_description = av_malloc(128);
            snprintf(new_device->device_description, 128,
                     "Adapter %d, display %d: %S (%dx%d @ %ld,%ld%s)",
                     adapterIndex+1, output_index+1, output_desc.DeviceName,
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

    // IDXGIFactory1_EnumAdapters(pDXGIFactory, 0, )


    // struct mondup *s = s1->priv_data;
    return device_list->nb_devices;
}

#define OFFSET(x) offsetof(struct mondup, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    // { "draw_mouse", "draw the mouse pointer", OFFSET(draw_mouse), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, DEC },
    // { "show_region", "draw border around capture area", OFFSET(show_region), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, DEC },
    { "framerate", "set video frame rate", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "30"}, 0, INT_MAX, DEC },
    { "video_size", "set video frame size", OFFSET(width), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL}, 0, 0, DEC },
    // { "offset_x", "capture area x offset", OFFSET(offset_x), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, INT_MAX, DEC },
    { "offset_x", "capture area x offset", OFFSET(offset_x), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, DEC },
    { "offset_y", "capture area y offset", OFFSET(offset_y), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, DEC },
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
    .priv_data_size = sizeof(struct mondup),
    .read_header    = mondup_read_header,
    .read_packet    = mondup_read_packet,
    .read_close     = mondup_read_close,
    .get_device_list     = mondup_get_device_list,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &mondup_class,
};
