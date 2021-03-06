/*
 * cl_image_360_stitch.cpp - CL Image 360 stitch
 *
 *  Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Wind Yuan <feng.yuan@intel.com>
 */

#include "cl_image_360_stitch.h"

#define XCAM_BLENDER_GLOBAL_SCALE_EXT_WIDTH 64

namespace XCam {

CLBlenderGlobalScaleKernel::CLBlenderGlobalScaleKernel (SmartPtr<CLContext> &context, bool is_uv)
    : CLBlenderScaleKernel (context, is_uv)
{
}

SmartPtr<CLImage>
CLBlenderGlobalScaleKernel::get_input_image (SmartPtr<DrmBoBuffer> &input) {
    SmartPtr<CLContext> context = get_context ();

    CLImageDesc cl_desc;
    SmartPtr<CLImage> cl_image;
    const VideoBufferInfo &buf_info = input->get_video_info ();

    cl_desc.format.image_channel_data_type = CL_UNORM_INT8;
    if (_is_uv) {
        cl_desc.format.image_channel_order = CL_RG;
        cl_desc.width = buf_info.width / 2;
        cl_desc.height = buf_info.height / 2;
        cl_desc.row_pitch = buf_info.strides[1];
        cl_image = new CLVaImage (context, input, cl_desc, buf_info.offsets[1]);
    } else {
        cl_desc.format.image_channel_order = CL_R;
        cl_desc.width = buf_info.width;
        cl_desc.height = buf_info.height;
        cl_desc.row_pitch = buf_info.strides[0];
        cl_image = new CLVaImage (context, input, cl_desc, buf_info.offsets[0]);
    }

    return cl_image;
}

SmartPtr<CLImage>
CLBlenderGlobalScaleKernel::get_output_image (SmartPtr<DrmBoBuffer> &output) {
    SmartPtr<CLContext> context = get_context ();

    CLImageDesc cl_desc;
    SmartPtr<CLImage> cl_image;
    const VideoBufferInfo &buf_info = output->get_video_info ();

    cl_desc.format.image_channel_data_type = CL_UNSIGNED_INT16;
    cl_desc.format.image_channel_order = CL_RGBA;
    if (_is_uv) {
        cl_desc.width = buf_info.width / 8;
        cl_desc.height = buf_info.height / 2;
        cl_desc.row_pitch = buf_info.strides[1];
        cl_image = new CLVaImage (context, output, cl_desc, buf_info.offsets[1]);
    } else {
        cl_desc.width = buf_info.width / 8;
        cl_desc.height = buf_info.height;
        cl_desc.row_pitch = buf_info.strides[0];
        cl_image = new CLVaImage (context, output, cl_desc, buf_info.offsets[0]);
    }

    return cl_image;
}

bool
CLBlenderGlobalScaleKernel::get_output_info (
    SmartPtr<DrmBoBuffer> &output,
    uint32_t &out_width, uint32_t &out_height, int &out_offset_x)
{
    const VideoBufferInfo &output_info = output->get_video_info ();

    out_width = output_info.width / 8;
    out_height = _is_uv ? output_info.height / 2 : output_info.height;
    out_offset_x = 0;

    return true;
}

CLImage360Stitch::CLImage360Stitch (CLBlenderScaleMode scale_mode)
    : CLMultiImageHandler ("CLImage360Stitch")
    , _output_width (0)
    , _output_height (0)
    , _scale_mode (scale_mode)
{
}

bool
CLImage360Stitch::set_left_blender (SmartPtr<CLBlender> blender)
{
    _left_blender = blender;

    SmartPtr<CLImageHandler> handler = blender;
    return add_image_handler (handler);
}

bool
CLImage360Stitch::set_right_blender (SmartPtr<CLBlender> blender)
{
    _right_blender = blender;

    SmartPtr<CLImageHandler> handler = blender;
    return add_image_handler (handler);
}

bool
CLImage360Stitch::set_image_overlap (const int idx, const Rect &overlap0, const Rect &overlap1)
{
    XCAM_ASSERT (idx < ImageIdxCount);
    _overlaps[idx][0] = overlap0;
    _overlaps[idx][1] = overlap1;
    return true;
}

XCamReturn
CLImage360Stitch::prepare_buffer_pool_video_info (
    const VideoBufferInfo &input,
    VideoBufferInfo &output)
{
    uint32_t output_width = _output_width;
    uint32_t output_height = _output_height;

    XCAM_FAIL_RETURN(
        WARNING,
        output_width && output_height,
        XCAM_RETURN_ERROR_PARAM,
        "CLImage360Stitch(%s) prepare buffer pool info failed since width:%d height:%d was not set correctly",
        XCAM_STR(get_name()), output_width, output_height);

    // aligned at least XCAM_BLENDER_ALIGNED_WIDTH
    uint32_t aligned_width = XCAM_MAX (16, XCAM_BLENDER_ALIGNED_WIDTH);
    output.init (
        input.format, output_width, output_height,
        XCAM_ALIGN_UP(output_width, aligned_width), XCAM_ALIGN_UP(output_height, 16));
    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
CLImage360Stitch::prepare_global_scale_blender_parameters (
    SmartPtr<DrmBoBuffer> &input0, SmartPtr<DrmBoBuffer> &input1, SmartPtr<DrmBoBuffer> &output)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    const VideoBufferInfo &in0_info = input0->get_video_info ();
    const VideoBufferInfo &in1_info = input1->get_video_info ();
    const VideoBufferInfo &out_info = output->get_video_info ();

    XCAM_ASSERT (in0_info.height == in1_info.height);
    XCAM_ASSERT (in0_info.width <= out_info.width && in1_info.width <= out_info.width);

    Rect main_left = get_image_overlap (ImageIdxMain, 0);
    Rect main_right = get_image_overlap (ImageIdxMain, 1);
    Rect scnd_left = get_image_overlap (ImageIdxSecondary, 1);
    Rect scnd_right = get_image_overlap (ImageIdxSecondary, 0);
    int main_mid = XCAM_ALIGN_DOWN (in0_info.width / 2, XCAM_BLENDER_ALIGNED_WIDTH);
    int scnd_mid = 0;
    int out_mid = XCAM_ALIGN_DOWN (out_info.width / 2, XCAM_BLENDER_ALIGNED_WIDTH);
    Rect area, out_merge_window;
    area.pos_y = out_merge_window.pos_y = 0;
    area.height = out_merge_window.pos_y = out_info.height;

    //calculate left stitching area(input)
    int32_t prev_pos = main_left.pos_x;
    main_left.pos_x = XCAM_ALIGN_AROUND (main_left.pos_x, XCAM_BLENDER_ALIGNED_WIDTH);
    main_left.width = XCAM_ALIGN_UP (main_left.width, XCAM_BLENDER_ALIGNED_WIDTH);
    scnd_left.pos_x += main_left.pos_x - prev_pos;
    scnd_left.pos_x = XCAM_ALIGN_AROUND (scnd_left.pos_x, XCAM_BLENDER_ALIGNED_WIDTH);
    scnd_left.width = main_left.width;

    //calculate right stitching area(input)
    prev_pos = main_right.pos_x;
    main_right.pos_x = XCAM_ALIGN_AROUND (main_right.pos_x, XCAM_BLENDER_ALIGNED_WIDTH);
    main_right.width = XCAM_ALIGN_UP (main_right.width, XCAM_BLENDER_ALIGNED_WIDTH);
    scnd_right.pos_x += main_right.pos_x - prev_pos;
    scnd_right.pos_x = XCAM_ALIGN_AROUND (scnd_right.pos_x, XCAM_BLENDER_ALIGNED_WIDTH);
    scnd_right.width = main_right.width;

    //find scnd_mid
    scnd_mid = scnd_left.pos_x + (main_mid - main_left.pos_x) - out_mid;
    if (scnd_mid < scnd_right.pos_x + scnd_right.width)
        scnd_mid = scnd_right.pos_x + scnd_right.width;

    // set left blender
    area.pos_x = scnd_mid;
    area.width = scnd_left.pos_x + scnd_left.width - scnd_mid;
    _left_blender->set_input_valid_area (area, 0);

    area.pos_x = main_left.pos_x;
    area.width = main_mid - main_left.pos_x;
    _left_blender->set_input_valid_area (area, 1);

    out_merge_window.width = main_left.width;
    out_merge_window.pos_x = out_mid - (main_mid - main_left.pos_x);
    _left_blender->set_merge_window (out_merge_window);
    _left_blender->set_input_merge_area (scnd_left, 0);
    _left_blender->set_input_merge_area (main_left, 1);

    // set right blender
    area.pos_x = main_mid;
    area.width = main_right.pos_x + main_right.width - main_mid;
    _right_blender->set_input_valid_area (area, 0);

    area.pos_x = scnd_right.pos_x;
    area.width = scnd_mid - scnd_right.pos_x;
    _right_blender->set_input_valid_area (area, 1);

    out_merge_window.pos_x = out_mid + (main_right.pos_x - main_mid);
    out_merge_window.width = main_right.width;
    _right_blender->set_merge_window (out_merge_window);
    _right_blender->set_input_merge_area (main_right, 0);
    _right_blender->set_input_merge_area (scnd_right, 1);

    return ret;
}

XCamReturn
CLImage360Stitch::prepare_local_scale_blender_parameters (
    SmartPtr<DrmBoBuffer> &input0, SmartPtr<DrmBoBuffer> &input1, SmartPtr<DrmBoBuffer> &output)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    const VideoBufferInfo &in0_info = input0->get_video_info ();
    const VideoBufferInfo &in1_info = input1->get_video_info ();
    const VideoBufferInfo &out_info = output->get_video_info ();

    XCAM_ASSERT (in0_info.height == in1_info.height);
    XCAM_ASSERT (in0_info.width <= out_info.width && in1_info.width <= out_info.width);

    Rect main_left = get_image_overlap (ImageIdxMain, 0);
    Rect main_right = get_image_overlap (ImageIdxMain, 1);
    Rect scnd_left = get_image_overlap (ImageIdxSecondary, 1);
    Rect scnd_right = get_image_overlap (ImageIdxSecondary, 0);

    int main_mid = XCAM_ALIGN_DOWN (in0_info.width / 2, XCAM_BLENDER_ALIGNED_WIDTH);
    int scnd_mid = XCAM_ALIGN_DOWN (in1_info.width / 2, XCAM_BLENDER_ALIGNED_WIDTH);
    int out_mid = XCAM_ALIGN_DOWN (out_info.width / 2, XCAM_BLENDER_ALIGNED_WIDTH);
    Rect area, out_merge_window;
    area.pos_y = out_merge_window.pos_y = 0;
    area.height = out_merge_window.pos_y = out_info.height;

    //calculate left stitching area(input)
    int32_t prev_pos = main_left.pos_x;
    main_left.pos_x = XCAM_ALIGN_AROUND (main_left.pos_x, XCAM_BLENDER_ALIGNED_WIDTH);
    main_left.width = XCAM_ALIGN_UP (main_left.width, XCAM_BLENDER_ALIGNED_WIDTH);
    scnd_left.pos_x += main_left.pos_x - prev_pos;
    scnd_left.pos_x = XCAM_ALIGN_AROUND (scnd_left.pos_x, XCAM_BLENDER_ALIGNED_WIDTH);
    scnd_left.width = main_left.width;

    //calculate right stitching area(input)
    prev_pos = main_right.pos_x;
    main_right.pos_x = XCAM_ALIGN_AROUND (main_right.pos_x, XCAM_BLENDER_ALIGNED_WIDTH);
    main_right.width = XCAM_ALIGN_UP (main_right.width, XCAM_BLENDER_ALIGNED_WIDTH);
    scnd_right.pos_x += main_right.pos_x - prev_pos;
    scnd_right.pos_x = XCAM_ALIGN_AROUND (scnd_right.pos_x, XCAM_BLENDER_ALIGNED_WIDTH);
    scnd_right.width = main_right.width;

    // set left blender
    area.pos_x = scnd_mid;
    area.width = scnd_left.pos_x + scnd_left.width - scnd_mid;
    _left_blender->set_input_valid_area (area, 0);

    area.pos_x = main_left.pos_x;
    area.width = main_mid - main_left.pos_x;
    _left_blender->set_input_valid_area (area, 1);

    int delta_width = out_mid - (main_mid - main_left.pos_x) - (scnd_left.pos_x - scnd_mid);
    out_merge_window.width = main_left.width + delta_width;
    out_merge_window.pos_x = scnd_left.pos_x - scnd_mid;
    _left_blender->set_merge_window (out_merge_window);
    _left_blender->set_input_merge_area (scnd_left, 0);
    _left_blender->set_input_merge_area (main_left, 1);

    // set right blender
    area.pos_x = main_mid;
    area.width = main_right.pos_x + main_right.width - main_mid;
    _right_blender->set_input_valid_area (area, 0);

    area.pos_x = scnd_right.pos_x;
    area.width = scnd_mid - scnd_right.pos_x;
    _right_blender->set_input_valid_area (area, 1);

    delta_width = out_mid - (scnd_mid - scnd_right.pos_x) - (main_right.pos_x - main_mid);
    out_merge_window.width = main_right.width + delta_width;
    out_merge_window.pos_x = out_mid + (main_right.pos_x - main_mid);
    _right_blender->set_merge_window (out_merge_window);
    _right_blender->set_input_merge_area (main_right, 0);
    _right_blender->set_input_merge_area (scnd_right, 1);

    return ret;
}

SmartPtr<DrmBoBuffer>
CLImage360Stitch::create_scale_input_buffer (SmartPtr<DrmBoBuffer> &output)
{
    VideoBufferInfo buf_info;
    const VideoBufferInfo &output_info = output->get_video_info ();

    uint32_t preset_width = XCAM_ALIGN_UP (output_info.width + XCAM_BLENDER_GLOBAL_SCALE_EXT_WIDTH, 16);
    buf_info.init (
        output_info.format, preset_width, output_info.height,
        XCAM_ALIGN_UP (preset_width, 16),
        XCAM_ALIGN_UP (output_info.height, 16));

    SmartPtr<DrmDisplay> display = DrmDisplay::instance ();
    SmartPtr<BufferPool> buf_pool = new DrmBoBufferPool (display);
    XCAM_ASSERT (buf_pool.ptr ());
    buf_pool->set_video_info (buf_info);
    if (!buf_pool->reserve (1)) {
        XCAM_LOG_ERROR ("init buffer pool failed");
        return NULL;
    }

    return buf_pool->get_buffer (buf_pool).dynamic_cast_ptr<DrmBoBuffer> ();
}

XCamReturn
CLImage360Stitch::reset_buffer_info (SmartPtr<DrmBoBuffer> &input)
{
    VideoBufferInfo reset_info;
    const VideoBufferInfo &buf_info = input->get_video_info ();

    Rect img0_left = get_image_overlap (ImageIdxMain, 0);
    Rect img0_right = get_image_overlap (ImageIdxMain, 1);
    Rect img1_left = get_image_overlap (ImageIdxSecondary, 0);
    Rect img1_right = get_image_overlap (ImageIdxSecondary, 1);

    uint32_t reset_width = img0_right.pos_x - img0_left.pos_x + img1_right.pos_x - img1_left.pos_x;
    reset_width = XCAM_ALIGN_UP (reset_width, XCAM_BLENDER_ALIGNED_WIDTH);
    reset_info.init (buf_info.format, reset_width, buf_info.height,
                     buf_info.aligned_width, buf_info.aligned_height);

    input->set_video_info (reset_info);
    return XCAM_RETURN_NO_ERROR;
}


XCamReturn
CLImage360Stitch::prepare_parameters (SmartPtr<DrmBoBuffer> &input, SmartPtr<DrmBoBuffer> &output)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;

    SmartPtr<DrmBoBuffer> input0 = input;
    SmartPtr<DrmBoBuffer> input1 = input0->find_typed_attach<DrmBoBuffer> ();
    XCAM_FAIL_RETURN(
        WARNING,
        input1.ptr (),
        XCAM_RETURN_ERROR_PARAM,
        "CLImage360Stitch(%s) does NOT find second buffer in attachment", get_name());

    SmartPtr<DrmBoBuffer> scale_input;
    if (_scale_mode == CLBlenderScaleLocal)
        ret = prepare_local_scale_blender_parameters (input0, input1, output);
    else {
        scale_input = create_scale_input_buffer (output);
        XCAM_ASSERT (scale_input.ptr ());
        ret = prepare_global_scale_blender_parameters (input0, input1, scale_input);
    }
    XCAM_FAIL_RETURN(
        WARNING,
        ret == XCAM_RETURN_NO_ERROR,
        XCAM_RETURN_ERROR_PARAM,
        "CLImage360Stitch(%s) failed to prepare blender parameters", get_name());

    if (_scale_mode == CLBlenderScaleLocal) {
        ret = CLMultiImageHandler::prepare_parameters (input0, output);
    } else {
        ret = CLMultiImageHandler::prepare_parameters (input0, scale_input);
        XCAM_FAIL_RETURN(
            WARNING,
            ret == XCAM_RETURN_NO_ERROR,
            XCAM_RETURN_ERROR_PARAM,
            "CLImage360Stitch(%s) failed to prepare parameters", get_name());

        input = scale_input;
        reset_buffer_info (input);
    }

    return ret;
}

static SmartPtr<CLImageKernel>
create_blender_global_scale_kernel (SmartPtr<CLContext> &context, bool is_uv)
{
    char transform_option[1024];
    snprintf (transform_option, sizeof(transform_option), "-DPYRAMID_UV=%d", is_uv ? 1 : 0);

    const XCamKernelInfo &kernel_info = {
        "kernel_pyramid_scale",
#include "kernel_gauss_lap_pyramid.clx"
        , 0
    };

    SmartPtr<CLImageKernel> kernel;
    kernel = new CLBlenderGlobalScaleKernel (context, is_uv);
    XCAM_ASSERT (kernel.ptr ());
    XCAM_FAIL_RETURN (
        ERROR,
        kernel->build_kernel (kernel_info, transform_option) == XCAM_RETURN_NO_ERROR,
        NULL,
        "load blender global scaling kernel(%s) failed", is_uv ? "UV" : "Y");

    return kernel;
}

SmartPtr<CLImageHandler>
create_image_360_stitch (SmartPtr<CLContext> &context, bool need_seam, CLBlenderScaleMode scale_mode)
{
    const int layer = 2;
    const bool need_uv = true;
    SmartPtr<CLBlender>  left_blender, right_blender;
    SmartPtr<CLImage360Stitch> stitch = new CLImage360Stitch (scale_mode);
    XCAM_ASSERT (stitch.ptr ());

    left_blender = create_pyramid_blender (context, layer, need_uv, need_seam, scale_mode).dynamic_cast_ptr<CLBlender> ();
    XCAM_FAIL_RETURN (ERROR, left_blender.ptr (), NULL, "image_360_stitch create left blender failed");
    left_blender->disable_buf_pool (true);
    left_blender->swap_input_idx (true);
    stitch->set_left_blender (left_blender);

    right_blender = create_pyramid_blender (context, layer, need_uv, need_seam, scale_mode).dynamic_cast_ptr<CLBlender> ();
    XCAM_FAIL_RETURN (ERROR, right_blender.ptr (), NULL, "image_360_stitch create right blender failed");
    right_blender->disable_buf_pool (true);
    stitch->set_right_blender (right_blender);

    if (scale_mode == CLBlenderScaleGlobal) {
        int max_plane = need_uv ? 2 : 1;
        bool uv_status[2] = {false, true};
        for (int plane = 0; plane < max_plane; ++plane) {
            SmartPtr<CLImageKernel> kernel = create_blender_global_scale_kernel (context, uv_status[plane]);
            XCAM_FAIL_RETURN (ERROR, kernel.ptr (), NULL, "create blender global scaling kernel failed");
            stitch->add_kernel (kernel);
        }
    }

    return stitch;
}

}

