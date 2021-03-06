/*
 * Copyright (c) 2013 Jens Kuske <jenskuske@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vdpau_private.h"
#include "ve.h"
#include <time.h>

extern uint64_t get_time(void);

static int find_startcode(CEDARV_MEMORY mem, int len, int start)
{
	int pos, zeros = 0;
	uint8_t* data = (uint8_t*)cedarv_getPointer(mem);
	
	for (pos = start; pos < len; pos++)
	{
		if (data[pos] == 0x00)
			zeros++;
		else if (data[pos] == 0x01 && zeros >= 2)
			return pos - 2;
		else
			zeros = 0;
	}

	return -1;
}

static uint32_t get_u(void *regs, int num)
{
	writel(0x00000002 | (num << 8), regs + CEDARV_H264_TRIGGER);

	while (readl(regs + CEDARV_H264_STATUS) & (1 << 8));

	return readl(regs + CEDARV_H264_BASIC_BITS);
}

static uint32_t get_ue(void *regs)
{
	writel(0x00000005, regs + CEDARV_H264_TRIGGER);

	while (readl(regs + CEDARV_H264_STATUS) & (1 << 8));

	return readl(regs + CEDARV_H264_BASIC_BITS);
}

static int32_t get_se(void *regs)
{
	writel(0x00000004, regs + CEDARV_H264_TRIGGER);

	while (readl(regs + CEDARV_H264_STATUS) & (1 << 8));

	return readl(regs + CEDARV_H264_BASIC_BITS);
}

#define REF_FLAG_LONGTERM	0x1
#define REF_FLAG_TOP_REF	0x2
#define REF_FLAG_BOTTOM_REF	0x3

typedef struct
{
	video_surface_ctx_t *surface;
	uint8_t flags;
	uint16_t top_pic_order_cnt;
	uint16_t bottom_pic_order_cnt;
	uint16_t frame_idx;
} h264_reference_frame_t;


#define SLICE_TYPE_P	0
#define SLICE_TYPE_B	1
#define SLICE_TYPE_I	2
#define SLICE_TYPE_SP	3
#define SLICE_TYPE_SI	4

typedef struct
{
	uint8_t nal_unit_type;
	uint16_t first_mb_in_slice;
	uint8_t slice_type;
	uint8_t pic_parameter_set_id;
	uint16_t frame_num;
	uint8_t field_pic_flag;
	uint8_t bottom_field_flag;
	uint16_t idr_pic_id;
	uint32_t pic_order_cnt_lsb;
	int32_t delta_pic_order_cnt_bottom;
	int32_t delta_pic_order_cnt[2];
	uint8_t redundant_pic_cnt;
	uint8_t direct_spatial_mv_pred_flag;
	uint8_t num_ref_idx_active_override_flag;
	uint8_t num_ref_idx_l0_active_minus1;
	uint8_t num_ref_idx_l1_active_minus1;
	uint8_t cabac_init_idc;
	int8_t slice_qp_delta;
	uint8_t sp_for_switch_flag;
	int8_t slice_qs_delta;
	uint8_t disable_deblocking_filter_idc;
	int8_t slice_alpha_c0_offset_div2;
	int8_t slice_beta_offset_div2;

	uint8_t luma_log2_weight_denom;
	uint8_t chroma_log2_weight_denom;
	int8_t luma_weight_l0[32];
	int8_t luma_offset_l0[32];
	int8_t chroma_weight_l0[32][2];
	int8_t chroma_offset_l0[32][2];
	int8_t luma_weight_l1[32];
	int8_t luma_offset_l1[32];
	int8_t chroma_weight_l1[32][2];
	int8_t chroma_offset_l1[32][2];

	h264_reference_frame_t *RefPicList0[32];
	h264_reference_frame_t *RefPicList1[32];
} h264_header_t;

typedef struct
{
	void *regs;
	h264_header_t header;
	VdpPictureInfoH264 const *info;
	video_surface_ctx_t *output;
	uint8_t picture_width_in_mbs_minus1;
	uint8_t picture_height_in_mbs_minus1;
	uint8_t default_scaling_lists;

	int ref_count;
	h264_reference_frame_t ref_frames[16];
} h264_context_t;

typedef struct
{
	CEDARV_MEMORY extra_data;
} h264_private_t;

static void h264_private_free(decoder_ctx_t *decoder)
{
	h264_private_t *decoder_p = (h264_private_t *)decoder->private;
	cedarv_free(decoder_p->extra_data);
	free(decoder_p);
}

typedef struct
{
	CEDARV_MEMORY extra_data;
	int extra_data_len;
	int pos;
} h264_video_private_t;

static void h264_video_private_free(video_surface_ctx_t *surface)
{
	h264_video_private_t *surface_p = (h264_video_private_t *)surface->decoder_private;
	cedarv_free(surface_p->extra_data);
	free(surface_p);
}

static void ref_pic_list_modification(h264_context_t *c)
{
	h264_header_t *h = &c->header;
	VdpPictureInfoH264 const *info = c->info;
	const int MaxFrameNum = 1 << (info->log2_max_frame_num_minus4 + 4);
	const int MaxPicNum = (info->field_pic_flag) ? 2 * MaxFrameNum : MaxFrameNum;

	void *cedarv_regs = cedarv_get_regs();

	if (h->slice_type != SLICE_TYPE_I && h->slice_type != SLICE_TYPE_SI)
	{
		int ref_pic_list_modification_flag_l0 = get_u(cedarv_regs, 1);
		if (ref_pic_list_modification_flag_l0)
		{
			unsigned int modification_of_pic_nums_idc;
			int refIdxL0 = 0;
			unsigned int picNumL0 = info->frame_num;
			do
			{
				modification_of_pic_nums_idc = get_ue(cedarv_regs);
				if (modification_of_pic_nums_idc == 0 || modification_of_pic_nums_idc == 1)
				{
					unsigned int abs_diff_pic_num_minus1 = get_ue(cedarv_regs);

					if (modification_of_pic_nums_idc == 0)
						picNumL0 -= (abs_diff_pic_num_minus1 + 1);
					else
						picNumL0 += (abs_diff_pic_num_minus1 + 1);

					picNumL0 &= (MaxPicNum - 1);

					int i, j;
					for (i = 0; i < c->ref_count; i++)
					{
						if (c->ref_frames[i].frame_idx == picNumL0)
							break;
					}

					for (j = h->num_ref_idx_l0_active_minus1 + 1; j > refIdxL0; j--)
						h->RefPicList0[j] = h->RefPicList0[j - 1];
                                        //prevent overrun of array
					h->RefPicList0[(refIdxL0++ & 0x1F)] = &c->ref_frames[i];
					i = refIdxL0;
					for (j = refIdxL0; j <= h->num_ref_idx_l0_active_minus1 + 1; j++)
						if (h->RefPicList0[j] && h->RefPicList0[j]->frame_idx != picNumL0)
                                                {
							h->RefPicList0[(i++ & 0x1F)] = h->RefPicList0[j];
                                                }
				}
				else if (modification_of_pic_nums_idc == 2)
				{
					VDPAU_DBG("NOT IMPLEMENTED: modification_of_pic_nums_idc == 2");
					unsigned int long_term_pic_num = get_ue(cedarv_regs);
				}
			} while (modification_of_pic_nums_idc != 3);
		}
	}

	if (h->slice_type == SLICE_TYPE_B)
	{
		int ref_pic_list_modification_flag_l1 = get_u(cedarv_regs, 1);
		if (ref_pic_list_modification_flag_l1)
		{
			VDPAU_DBG("NOT IMPLEMENTED: ref_pic_list_modification_flag_l1 == 1");
			unsigned int modification_of_pic_nums_idc;
			do
			{
				modification_of_pic_nums_idc = get_ue(cedarv_regs);
				if (modification_of_pic_nums_idc == 0 || modification_of_pic_nums_idc == 1)
				{
					unsigned int abs_diff_pic_num_minus1 = get_ue(cedarv_regs);
				}
				else if (modification_of_pic_nums_idc == 2)
				{
					unsigned int long_term_pic_num = get_ue(cedarv_regs);
				}
			} while (modification_of_pic_nums_idc != 3);
		}
	}
}

static void pred_weight_table(h264_context_t *c)
{
	h264_header_t *h = &c->header;
	int i, j, ChromaArrayType = 1;
	void* cedarv_regs = cedarv_get_regs();

	h->luma_log2_weight_denom = get_ue(cedarv_regs);
	if (ChromaArrayType != 0)
		h->chroma_log2_weight_denom = get_ue(cedarv_regs);

	for (i = 0; i < 32; i++)
	{
		h->luma_weight_l0[i] = (1 << h->luma_log2_weight_denom);
		h->luma_weight_l1[i] = (1 << h->luma_log2_weight_denom);
		h->chroma_weight_l0[i][0] = (1 << h->chroma_log2_weight_denom);
		h->chroma_weight_l1[i][0] = (1 << h->chroma_log2_weight_denom);
		h->chroma_weight_l0[i][1] = (1 << h->chroma_log2_weight_denom);
		h->chroma_weight_l1[i][1] = (1 << h->chroma_log2_weight_denom);
	}

	for (i = 0; i <= h->num_ref_idx_l0_active_minus1; i++)
	{
		int luma_weight_l0_flag = get_u(cedarv_regs, 1);
		if (luma_weight_l0_flag)
		{
			h->luma_weight_l0[i] = get_se(cedarv_regs);
			h->luma_offset_l0[i] = get_se(cedarv_regs);
		}
		if (ChromaArrayType != 0)
		{
			int chroma_weight_l0_flag = get_u(cedarv_regs, 1);
			if (chroma_weight_l0_flag)
				for (j = 0; j < 2; j++)
				{
					h->chroma_weight_l0[i][j] = get_se(cedarv_regs);
					h->chroma_offset_l0[i][j] = get_se(cedarv_regs);
				}
		}
	}

	if (h->slice_type == SLICE_TYPE_B)
		for (i = 0; i <= h->num_ref_idx_l1_active_minus1; i++)
		{
			int luma_weight_l1_flag = get_u(cedarv_regs, 1);
			if (luma_weight_l1_flag)
			{
				h->luma_weight_l1[i] = get_se(cedarv_regs);
				h->luma_offset_l1[i] = get_se(cedarv_regs);
			}
			if (ChromaArrayType != 0)
			{
				int chroma_weight_l1_flag = get_u(cedarv_regs, 1);
				if (chroma_weight_l1_flag)
					for (j = 0; j < 2; j++)
					{
						h->chroma_weight_l1[i][j] = get_se(cedarv_regs);
						h->chroma_offset_l1[i][j] = get_se(cedarv_regs);
					}
			}
		}

	writel(((h->chroma_log2_weight_denom & 0xf) << 4)
		| ((h->luma_log2_weight_denom & 0xf) << 0)
		, cedarv_regs + CEDARV_H264_PRED_WEIGHT);

	writel(CEDARV_SRAM_H264_PRED_WEIGHT_TABLE, cedarv_regs + CEDARV_H264_RAM_WRITE_PTR);
	for (i = 0; i < 32; i++)
		writel(((h->luma_offset_l0[i] & 0x1ff) << 16)
			| (h->luma_weight_l0[i] & 0xff), cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
	for (i = 0; i < 32; i++)
		for (j = 0; j < 2; j++)
			writel(((h->chroma_offset_l0[i][j] & 0x1ff) << 16)
				| (h->chroma_weight_l0[i][j] & 0xff), cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
	for (i = 0; i < 32; i++)
		writel(((h->luma_offset_l1[i] & 0x1ff) << 16)
			| (h->luma_weight_l1[i] & 0xff), cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
	for (i = 0; i < 32; i++)
		for (j = 0; j < 2; j++)
			writel(((h->chroma_offset_l1[i][j] & 0x1ff) << 16)
				| (h->chroma_weight_l1[i][j] & 0xff), cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
}

static void dec_ref_pic_marking(h264_context_t *c)
{
	void* cedarv_regs = cedarv_get_regs();

	h264_header_t *h = &c->header;
	// only reads bits to allow decoding, doesn't mark anything
	if (h->nal_unit_type == 5)
	{
		get_u(cedarv_regs, 1);
		get_u(cedarv_regs, 1);
	}
	else
	{
		int adaptive_ref_pic_marking_mode_flag = get_u(cedarv_regs, 1);
		if (adaptive_ref_pic_marking_mode_flag)
		{
			unsigned int memory_management_control_operation;
			do
			{
				memory_management_control_operation = get_ue(cedarv_regs);
				if (memory_management_control_operation == 1 || memory_management_control_operation == 3)
				{
					get_ue(cedarv_regs);
				}
				if (memory_management_control_operation == 2)
				{
					get_ue(cedarv_regs);
				}
				if (memory_management_control_operation == 3 || memory_management_control_operation == 6)
				{
					get_ue(cedarv_regs);
				}
				if (memory_management_control_operation == 4)
				{
					get_ue(cedarv_regs);
				}
			} while (memory_management_control_operation != 0);
		}
	}
}

static void decode_slice_header(h264_context_t *c)
{
	void* cedarv_regs = cedarv_get_regs();
	h264_header_t *h = &c->header;
	VdpPictureInfoH264 const *info = c->info;
	h->num_ref_idx_l0_active_minus1 = info->num_ref_idx_l0_active_minus1;
	h->num_ref_idx_l1_active_minus1 = info->num_ref_idx_l1_active_minus1;

	h->first_mb_in_slice = get_ue(cedarv_regs);
	h->slice_type = get_ue(cedarv_regs);
	if (h->slice_type >= 5)
		h->slice_type -= 5;
	h->pic_parameter_set_id = get_ue(cedarv_regs);

	// separate_colour_plane_flag isn't available in VDPAU
	/*if (separate_colour_plane_flag == 1)
		colour_plane_id u(2)*/

	h->frame_num = get_u(cedarv_regs, info->log2_max_frame_num_minus4 + 4);

	if (!info->frame_mbs_only_flag)
	{
		h->field_pic_flag = get_u(cedarv_regs, 1);
		if (h->field_pic_flag)
			h->bottom_field_flag = get_u(cedarv_regs, 1);
	}

	if (h->nal_unit_type == 5)
		h->idr_pic_id = get_ue(cedarv_regs);

	if (info->pic_order_cnt_type == 0)
	{
		h->pic_order_cnt_lsb = get_u(cedarv_regs, info->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (info->pic_order_present_flag && !info->field_pic_flag)
			h->delta_pic_order_cnt_bottom = get_se(cedarv_regs);
	}

	if (info->pic_order_cnt_type == 1 && !info->delta_pic_order_always_zero_flag)
	{
		h->delta_pic_order_cnt[0] = get_se(cedarv_regs);
		if (info->pic_order_present_flag && !info->field_pic_flag)
			h->delta_pic_order_cnt[1] = get_se(cedarv_regs);
	}

	if (info->redundant_pic_cnt_present_flag)
		h->redundant_pic_cnt = get_ue(cedarv_regs);

	if (h->slice_type == SLICE_TYPE_B)
		h->direct_spatial_mv_pred_flag = get_u(cedarv_regs, 1);

	if (h->slice_type == SLICE_TYPE_P || h->slice_type == SLICE_TYPE_SP || h->slice_type == SLICE_TYPE_B)
	{
		h->num_ref_idx_active_override_flag = get_u(cedarv_regs, 1);
		if (h->num_ref_idx_active_override_flag)
		{
			h->num_ref_idx_l0_active_minus1 = get_ue(cedarv_regs);
			if (h->slice_type == SLICE_TYPE_B)
				h->num_ref_idx_l1_active_minus1 = get_ue(cedarv_regs);
		}
	}

	if (h->nal_unit_type == 20)
		{}//ref_pic_list_mvc_modification(); // specified in Annex H
	else
		ref_pic_list_modification(c);

	if ((info->weighted_pred_flag && (h->slice_type == SLICE_TYPE_P || h->slice_type == SLICE_TYPE_SP)) || (info->weighted_bipred_idc == 1 && h->slice_type == SLICE_TYPE_B))
		pred_weight_table(c);

	if (info->is_reference)
		dec_ref_pic_marking(c);

	if (info->entropy_coding_mode_flag && h->slice_type != SLICE_TYPE_I && h->slice_type != SLICE_TYPE_SI)
		h->cabac_init_idc = get_ue(cedarv_regs);

	h->slice_qp_delta = get_se(cedarv_regs);

	if (h->slice_type == SLICE_TYPE_SP || h->slice_type == SLICE_TYPE_SI)
	{
		if (h->slice_type == SLICE_TYPE_SP)
			h->sp_for_switch_flag = get_u(cedarv_regs, 1);
		h->slice_qs_delta = get_se(cedarv_regs);
	}

	if (info->deblocking_filter_control_present_flag)
	{
		h->disable_deblocking_filter_idc = get_ue(cedarv_regs);
		if (h->disable_deblocking_filter_idc != 1)
		{
			h->slice_alpha_c0_offset_div2 = get_se(cedarv_regs);
			h->slice_beta_offset_div2 = get_se(cedarv_regs);
		}
	}

	// num_slice_groups_minus1, slice_group_map_type, slice_group_map_type aren't available in VDPAU
	/*if (num_slice_groups_minus1 > 0 && slice_group_map_type >= 3 && slice_group_map_type <= 5)
		slice_group_change_cycle u(v)*/
}

static int sort_ref_frames(const void *p1, const void *p2)
{
	const h264_reference_frame_t *r1 = p1;
	const h264_reference_frame_t *r2 = p2;

	return r1->top_pic_order_cnt - r2->top_pic_order_cnt;
}

static void fill_frame_lists(h264_context_t *c)
{
	int i;
	h264_video_private_t *output_p = (h264_video_private_t *)c->output->decoder_private;
	void* cedarv_regs = cedarv_get_regs();

	// collect reference frames
	h264_reference_frame_t *frame_list[18];
	memset(frame_list, 0, sizeof(frame_list));

	for (i = 0; i < 16; i++)
	{
		const VdpReferenceFrameH264 *rf = &(c->info->referenceFrames[i]);
		if (rf->surface != VDP_INVALID_HANDLE)
		{
			if (rf->is_long_term)
				VDPAU_DBG("NOT IMPLEMENTED: We got a longterm reference!");

			video_surface_ctx_t *surface = handle_get(rf->surface);
			if(surface && surface->frame_decoded)
			{
				h264_video_private_t *surface_p = (h264_video_private_t *)surface->decoder_private;
				c->ref_frames[c->ref_count].surface = surface;
				c->ref_frames[c->ref_count].top_pic_order_cnt = rf->field_order_cnt[0];
				c->ref_frames[c->ref_count].bottom_pic_order_cnt = rf->field_order_cnt[1];
				c->ref_frames[c->ref_count].frame_idx = rf->frame_idx;
				c->ref_frames[c->ref_count].flags = (rf->is_long_term ? REF_FLAG_LONGTERM : 0)
					| (rf->top_is_reference ? REF_FLAG_TOP_REF : 0)
					| (rf->bottom_is_reference ? REF_FLAG_BOTTOM_REF : 0);

				frame_list[surface_p->pos] = &c->ref_frames[c->ref_count];
				c->ref_count++;
                                
                                handle_release(rf->surface);
                        }
		}
	}

	// write picture buffer list
	writel(CEDARV_SRAM_H264_FRAMEBUFFER_LIST, cedarv_regs + CEDARV_H264_RAM_WRITE_PTR);

	int output_placed = 0;
	for (i = 0; i < 18; i++)
	{
		if (!output_placed && !frame_list[i])
		{
			writel((uint16_t)c->info->field_order_cnt[0], cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel((uint16_t)c->info->field_order_cnt[1], cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel((c->info->is_reference) ? 0x22 : 0x0, cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(cedarv_virt2phys(c->output->dataY), cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(cedarv_virt2phys(c->output->dataU)/* + c->output->plane_size*/, cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(cedarv_virt2phys(output_p->extra_data), cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(cedarv_virt2phys(output_p->extra_data) + (output_p->extra_data_len / 2), cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(0, cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);

			output_p->pos = i;
			output_placed = 1;
		}
		else if (!frame_list[i])
		{
			int j;
			for (j = 0; j < 8; j++)
				writel(0x0, cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
		}
		else
		{
			video_surface_ctx_t *surface = frame_list[i]->surface;
			h264_video_private_t *surface_p = (h264_video_private_t *)surface->decoder_private;

			writel(frame_list[i]->top_pic_order_cnt, cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(frame_list[i]->bottom_pic_order_cnt, cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(0, cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(cedarv_virt2phys(surface->dataY), cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(cedarv_virt2phys(surface->dataU)/* + surface->plane_size*/, cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(cedarv_virt2phys(surface_p->extra_data), cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(cedarv_virt2phys(surface_p->extra_data) + (surface_p->extra_data_len / 2), cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			writel(0, cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
		}
	}

	// output index
	writel(output_p->pos, cedarv_regs + CEDARV_H264_OUTPUT_FRAME_IDX);

	// sort reference frame list
	qsort(c->ref_frames, c->ref_count, sizeof(c->ref_frames[0]), &sort_ref_frames);
}
unsigned long num_pics=0;
unsigned long num_longs=0;

// VDPAU does not tell us if the scaling lists are default or custom
static int check_scaling_lists(h264_context_t *c)
{
	const uint32_t *sl4 = (uint32_t *)&c->info->scaling_lists_4x4[0][0];
	const uint32_t *sl8 = (uint32_t *)&c->info->scaling_lists_8x8[0][0];

	int i;
	for (i = 0; i < 6 * 16 / 4; i++)
		if (sl4[i] != 0x10101010)
			return 0;

	for (i = 0; i < 2 * 64 / 4; i++)
		if (sl8[i] != 0x10101010)
			return 0;

	return 1;
}

static VdpStatus h264_decode(decoder_ctx_t *decoder, VdpPictureInfo const *_info, const int len, video_surface_ctx_t *output)
{
	h264_private_t *decoder_p = (h264_private_t *)decoder->private;
	VdpPictureInfoH264 const *info = (VdpPictureInfoH264 const *)_info;

	h264_context_t *c = calloc(1, sizeof(h264_context_t));
	c->picture_width_in_mbs_minus1 = (decoder->width - 1) / 16;
	c->picture_height_in_mbs_minus1 = (decoder->height - 1) / 16;
	c->info = info;
	c->output = output;

#if 1 
	if (!c->info->frame_mbs_only_flag)
	{
		VDPAU_DBG("We can't decode interlaced Frames yet! Sorry");
		return VDP_STATUS_ERROR;
	}

	if (!c->output->decoder_private)
	{
		h264_video_private_t *output_p = calloc(1, sizeof(h264_video_private_t));
		if (!output_p)
			return VDP_STATUS_RESOURCES;

		// create extra buffer
		output_p->extra_data_len = (c->picture_width_in_mbs_minus1 + 1) * (c->picture_height_in_mbs_minus1 + 1) * 32;
		output_p->extra_data = cedarv_malloc(output_p->extra_data_len);

		c->output->decoder_private = output_p;
		c->output->decoder_private_free = h264_video_private_free;
	}

        void* cedarv_regs = cedarv_get(CEDARV_ENGINE_H264, 0x1 | (decoder->width >= 2048 ? (0x1 << 21) : 0x0));
	
        // activate H264 engine
	writel((readl(cedarv_regs + CEDARV_CTRL) & ~0xf) | 0x1
		| (decoder->width >= 2048 ? (0x1 << 21) : 0x0), cedarv_regs + CEDARV_CTRL);


#endif

	// some buffers
	uint32_t extra_buffers = cedarv_virt2phys(decoder_p->extra_data);
	writel(extra_buffers, cedarv_regs + CEDARV_H264_EXTRA_BUFFER1);
	writel(extra_buffers + 0x48000, cedarv_regs + CEDARV_H264_EXTRA_BUFFER2);
	if (cedarv_get_version() == 0x1625 || decoder->width >= 2048)
	{
		int size = (c->picture_width_in_mbs_minus1 + 32) * 192;
		size = (size + 4095) & ~4095;
		writel(decoder->width >= 2048 ? 0x5 : 0xa, cedarv_regs + 0x50);
		writel(extra_buffers + 0x50000, cedarv_regs + 0x54);
		writel(extra_buffers + 0x50000 + size, cedarv_regs + 0x58);
	}

	// write custom scaling lists
	if (!(c->default_scaling_lists = check_scaling_lists(c)))
	{
		const uint32_t *sl4 = (uint32_t *)&c->info->scaling_lists_4x4[0][0];
		const uint32_t *sl8 = (uint32_t *)&c->info->scaling_lists_8x8[0][0];

		writel(CEDARV_SRAM_H264_SCALING_LISTS, cedarv_regs + CEDARV_H264_RAM_WRITE_PTR);

		int i;
		for (i = 0; i < 2 * 64 / 4; i++)
			writel(sl8[i], cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);

		for (i = 0; i < 6 * 16 / 4; i++)
			writel(sl4[i], cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
	}

	// sdctrl
	writel(0x00000000, cedarv_regs + CEDARV_H264_SDROT_CTRL);

	fill_frame_lists(c);

	unsigned int slice, pos = 0;
	for (slice = 0; slice < info->slice_count; slice++)
	{
		h264_header_t *h = &c->header;
		memset(h, 0, sizeof(h264_header_t));

		pos = find_startcode(decoder->data, len, pos) + 3;

		h->nal_unit_type = cedarv_byteAccess(decoder->data, pos++) & 0x1f;

		if (h->nal_unit_type != 5 && h->nal_unit_type != 1)
		{
			free(c);
			cedarv_put();
			return VDP_STATUS_ERROR;
		}

		// Enable startcode detect and ??
		writel((0x1 << 25) | (0x1 << 10), cedarv_regs + CEDARV_H264_CTRL);

		// input buffer
		writel((len - pos) * 8, cedarv_regs + CEDARV_H264_VLD_LEN);
		writel(pos * 8, cedarv_regs + CEDARV_H264_VLD_OFFSET);
		uint32_t input_addr = cedarv_virt2phys(decoder->data);
		writel(input_addr + VBV_SIZE - 1, cedarv_regs + CEDARV_H264_VLD_END);
		writel((input_addr & 0x0ffffff0) | (input_addr >> 28) | (0x7 << 28), cedarv_regs + CEDARV_H264_VLD_ADDR);

#if 1 
		//if(num_pics == 0)
		// ?? some sort of reset maybe
		writel(0x7, cedarv_regs + CEDARV_H264_TRIGGER);
#endif

		// fill RefPicLists
		int i;
		int ptr0 = 0, ptr1 = 0;
		for (i = 0; i < c->ref_count; i++)
		{
			if (c->ref_frames[c->ref_count - 1 - i].top_pic_order_cnt < (uint16_t)info->field_order_cnt[0])
				h->RefPicList0[ptr0++] = &c->ref_frames[c->ref_count - 1  - i];

			if (c->ref_frames[i].top_pic_order_cnt >= (uint16_t)info->field_order_cnt[0])
				h->RefPicList1[ptr1++] = &c->ref_frames[i];
		}

		decode_slice_header(c);

#if 1 
		// write RefPicLists
		if (h->slice_type != SLICE_TYPE_I && h->slice_type != SLICE_TYPE_SI)
		{
			writel(CEDARV_SRAM_H264_REF_LIST0, cedarv_regs + CEDARV_H264_RAM_WRITE_PTR);
			for (i = 0; i < h->num_ref_idx_l0_active_minus1 + 1; i += 4)
			{
				int j;
				uint32_t list = 0;
				for (j = 0; j < 4; j++)
					if (h->RefPicList0[i + j])
					{
						h264_video_private_t *surface_p = (h264_video_private_t *)h->RefPicList0[i + j]->surface->decoder_private;
						list |= ((surface_p->pos * 2) << (j * 8));
					}
				writel(list, cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			}
		}
		if (h->slice_type == SLICE_TYPE_B)
		{
			writel(CEDARV_SRAM_H264_REF_LIST1, cedarv_regs + CEDARV_H264_RAM_WRITE_PTR);
			for (i = 0; i < h->num_ref_idx_l1_active_minus1 + 1; i += 4)
			{
				int j;
				uint32_t list = 0;
				for (j = 0; j < 4; j++)
					if (h->RefPicList1[i + j])
					{
						h264_video_private_t *surface_p = (h264_video_private_t *)h->RefPicList1[i + j]->surface->decoder_private;
						list |= ((surface_p->pos * 2) << (j * 8));
					}
				writel(list, cedarv_regs + CEDARV_H264_RAM_WRITE_DATA);
			}
		}
#endif 
		// picture parameters
		writel(((info->entropy_coding_mode_flag & 0x1) << 15)
			| ((info->num_ref_idx_l0_active_minus1 & 0x1f) << 10)
			| ((info->num_ref_idx_l1_active_minus1 & 0x1f) << 5)
			| ((info->weighted_pred_flag & 0x1) << 4)
			| ((info->weighted_bipred_idc & 0x3) << 2)
			| ((info->constrained_intra_pred_flag & 0x1) << 1)
			| ((info->transform_8x8_mode_flag & 0x1) << 0)
			, cedarv_regs + CEDARV_H264_PIC_HDR);

		// sequence parameters
		writel((0x1 << 19)
			| ((c->info->frame_mbs_only_flag & 0x1) << 18)
			| ((c->info->mb_adaptive_frame_field_flag & 0x1) << 17)
			| ((c->info->direct_8x8_inference_flag & 0x1) << 16)
			| ((c->picture_width_in_mbs_minus1 & 0xff) << 8)
			| ((c->picture_height_in_mbs_minus1 & 0xff) << 0)
			, cedarv_regs + CEDARV_H264_FRAME_SIZE);

		// slice parameters
		writel((((h->first_mb_in_slice % (c->picture_width_in_mbs_minus1 + 1)) & 0xff) << 24)
			| (((h->first_mb_in_slice / (c->picture_width_in_mbs_minus1 + 1)) & 0xff) << 16)
			| ((info->is_reference & 0x1) << 12)
			| ((h->slice_type & 0xf) << 8)
			| ((slice == 0 ? 0x1 : 0x0) << 5)
			| ((info->field_pic_flag & 0x1) << 4)
			| ((info->bottom_field_flag & 0x1) << 3)
			| ((h->direct_spatial_mv_pred_flag & 0x1) << 2)
			| ((h->cabac_init_idc & 0x3) << 0)
			, cedarv_regs + CEDARV_H264_SLICE_HDR);

		writel(((h->num_ref_idx_l0_active_minus1 & 0x1f) << 24)
			| ((h->num_ref_idx_l1_active_minus1 & 0x1f) << 16)
			| ((h->num_ref_idx_active_override_flag & 0x1) << 12)
			| ((h->disable_deblocking_filter_idc & 0x3) << 8)
			| ((h->slice_alpha_c0_offset_div2 & 0xf) << 4)
			| ((h->slice_beta_offset_div2 & 0xf) << 0)
			, cedarv_regs + CEDARV_H264_SLICE_HDR2);

		writel(((c->default_scaling_lists & 0x1) << 24)
			| ((info->second_chroma_qp_index_offset & 0x3f) << 16)
			| ((info->chroma_qp_index_offset & 0x3f) << 8)
			| (((info->pic_init_qp_minus26 + 26 + h->slice_qp_delta) & 0x3f) << 0)
			, cedarv_regs + CEDARV_H264_QP_PARAM);

		// clear status flags
		writel(readl(cedarv_regs + CEDARV_H264_STATUS), cedarv_regs + CEDARV_H264_STATUS);

		// enable int
		writel(readl(cedarv_regs + CEDARV_H264_CTRL) | 0x7, cedarv_regs + CEDARV_H264_CTRL);

		// SHOWTIME
		writel(0x8, cedarv_regs + CEDARV_H264_TRIGGER);

		++num_pics;
#if TIME_MEAS
uint64_t tv, tv2;
		tv = get_time();
#endif
		cedarv_wait(1);

#if TIME_MEAS
		tv2 = get_time();
		if (tv2-tv > 20000000) {
			printf("cedarv_wait, longer than 20ms:%lld, pics=%ld, longs=%ld\n", tv2-tv, num_pics, ++num_longs);
		}
#endif

		// clear status flags
		writel(readl(cedarv_regs + CEDARV_H264_STATUS), cedarv_regs + CEDARV_H264_STATUS);

		pos = (readl(cedarv_regs + CEDARV_H264_VLD_OFFSET) / 8) - 3;
	}

#if 1 
	// stop H264 engine
	//writel((readl(cedarv_regs + CEDARV_CTRL) & ~0xf) | 0x7, cedarv_regs + CEDARV_CTRL);
        cedarv_put();
#endif
        c->output->frame_decoded = 1;
	free(c);
	return VDP_STATUS_OK;
}

VdpStatus new_decoder_h264(decoder_ctx_t *decoder)
{
	h264_private_t *decoder_p = calloc(1, sizeof(h264_private_t));
	if (!decoder_p)
		return VDP_STATUS_RESOURCES;

	int extra_data_size = 320 * 1024;
	if (cedarv_get_version() == 0x1625 || decoder->width >= 2048)
	{
		// Engine version 0x1625 needs two extra buffers
		extra_data_size += ((decoder->width - 1) / 16 + 32) * 192;
		extra_data_size = (extra_data_size + 4095) & ~4095;
		extra_data_size += ((decoder->width - 1) / 16 + 64) * 80;
	}

	decoder_p->extra_data = cedarv_malloc(extra_data_size);
	if (! cedarv_isValid(decoder_p->extra_data))
	{
		free(decoder_p);
		return VDP_STATUS_RESOURCES;
	}

	decoder->decode = h264_decode;
	decoder->private = decoder_p;
	decoder->private_free = h264_private_free;
	return VDP_STATUS_OK;
}
