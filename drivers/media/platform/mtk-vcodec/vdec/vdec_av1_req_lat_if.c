// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 * Author: George Sun <george.sun@mediatek.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <media/videobuf2-dma-contig.h>

#include "../mtk_vcodec_util.h"
#include "../mtk_vcodec_dec.h"
#include "../mtk_vcodec_intr.h"
#include "../vdec_drv_base.h"
#include "../vdec_drv_if.h"
#include "../vdec_vpu_if.h"


#define AV1_REFS_PER_FRAME	  7
#define AV1_TOTAL_REFS_PER_FRAME  8
#define AV1_MAX_FRAME_BUF_COUNT	  (AV1_TOTAL_REFS_PER_FRAME + 1)
#define AV1_TILE_BUF_SIZE   64
#define AV1_MAX_TILE_COUNT  4096
#define AV1_SCALE_SUBPEL_BITS  10
#define AV1_REF_SCALE_SHIFT  14
#define AV1_REF_NO_SCALE  (1 << AV1_REF_SCALE_SHIFT)
#define AV1_REF_INVALID_SCALE  -1

#define AV1_INVALID_IDX  -1
#define AV1_MAX_TILE_ROWS  64
#define AV1_MAX_TILE_COLS  64

#define ALIGN_POWER_OF_TWO(value, n) \
  (((value) + ((1 << (n)) - 1)) & ~((1 << (n)) - 1))

#define ROUND_POWER_OF_TWO(value, n) (((value) + (((1 << (n)) >> 1))) >> (n))

#define ROUND_POWER_OF_TWO_SIGNED(value, n)           \
	(((value) < 0) ? -ROUND_POWER_OF_TWO(-(value), (n)) \
		: ROUND_POWER_OF_TWO((value), (n)))

#define ROUND_POWER_OF_TWO_64(value, n) \
	(((value) + (((1LL << (n)) >> 1))) >> (n))

#define ROUND_POWER_OF_TWO_SIGNED_64(value, n)           \
	(((value) < 0) ? -ROUND_POWER_OF_TWO_64(-(value), (n)) \
		: ROUND_POWER_OF_TWO_64((value), (n)))

#define BIT_FLAG(x,bit) (!!((x)->flags & bit))

#ifndef INT16_MIN
#define INT16_MIN      (-32767-1)
#endif

#ifndef INT16_MAX
#define INT16_MAX      (32767)
#endif

#define MINQ 0
#define MAXQ 255

#define DIV_LUT_PREC_BITS 14
#define DIV_LUT_BITS 8
#define DIV_LUT_NUM (1 << DIV_LUT_BITS)
#define WARP_PARAM_REDUCE_BITS 6
#define WARPEDMODEL_PREC_BITS 16

static const unsigned short div_lut[DIV_LUT_NUM + 1] = {
	16384, 16320, 16257, 16194, 16132, 16070, 16009, 15948, 15888, 15828, 15768,
	15709, 15650, 15592, 15534, 15477, 15420, 15364, 15308, 15252, 15197, 15142,
	15087, 15033, 14980, 14926, 14873, 14821, 14769, 14717, 14665, 14614, 14564,
	14513, 14463, 14413, 14364, 14315, 14266, 14218, 14170, 14122, 14075, 14028,
	13981, 13935, 13888, 13843, 13797, 13752, 13707, 13662, 13618, 13574, 13530,
	13487, 13443, 13400, 13358, 13315, 13273, 13231, 13190, 13148, 13107, 13066,
	13026, 12985, 12945, 12906, 12866, 12827, 12788, 12749, 12710, 12672, 12633,
	12596, 12558, 12520, 12483, 12446, 12409, 12373, 12336, 12300, 12264, 12228,
	12193, 12157, 12122, 12087, 12053, 12018, 11984, 11950, 11916, 11882, 11848,
	11815, 11782, 11749, 11716, 11683, 11651, 11619, 11586, 11555, 11523, 11491,
	11460, 11429, 11398, 11367, 11336, 11305, 11275, 11245, 11215, 11185, 11155,
	11125, 11096, 11067, 11038, 11009, 10980, 10951, 10923, 10894, 10866, 10838,
	10810, 10782, 10755, 10727, 10700, 10673, 10645, 10618, 10592, 10565, 10538,
	10512, 10486, 10460, 10434, 10408, 10382, 10356, 10331, 10305, 10280, 10255,
	10230, 10205, 10180, 10156, 10131, 10107, 10082, 10058, 10034, 10010, 9986,
	9963,  9939,  9916,  9892,  9869,  9846,  9823,  9800,  9777,  9754,  9732,
	9709,  9687,  9664,  9642,  9620,  9598,  9576,  9554,  9533,  9511,  9489,
	9468,  9447,  9425,  9404,  9383,  9362,  9341,  9321,  9300,  9279,  9259,
	9239,  9218,  9198,  9178,  9158,  9138,  9118,  9098,  9079,  9059,  9039,
	9020,  9001,  8981,  8962,  8943,  8924,  8905,  8886,  8867,  8849,  8830,
	8812,  8793,  8775,  8756,  8738,  8720,  8702,  8684,  8666,  8648,  8630,
	8613,  8595,  8577,  8560,  8542,  8525,  8508,  8490,  8473,  8456,  8439,
	8422,  8405,  8389,  8372,  8355,  8339,  8322,  8306,  8289,  8273,  8257,
	8240,  8224,  8208,  8192,
};

/**
 * struct vdec_av1_slice_init_vsi - VSI used to initialize instance
 * @architecture          : architecture type
 * @reserved              : reserved
 * @core_vsi              : for core vsi
 * @cdf_table_addr        : cdf table addr
 * @cdf_table_size        : cdf table size
 * @iq_table_addr         : iq table addr
 * @iq_table_size         : iq table size
 * @vsi_size              : share vsi structure size 
 */
struct vdec_av1_slice_init_vsi {
	unsigned int architecture;
	unsigned int reserved;
	uint64_t core_vsi;
	uint64_t cdf_table_addr;
	unsigned int cdf_table_size;
	uint64_t iq_table_addr;
	unsigned int iq_table_size;
	unsigned int vsi_size;
};

/**
 * struct vdec_av1_slice_mem - memory address and size
 * @buf                   : dma_addr padding
 * @dma_addr              : buffer address
 * @size                  : buffer size
 * @dma_addr_end          : buffer end address
 * @padding               : for padding
 */
struct vdec_av1_slice_mem {
	union {
		uint64_t buf;
		dma_addr_t dma_addr;
	};
	union {
		size_t size;
		dma_addr_t dma_addr_end;
		uint64_t padding;
	};
};


/**
 * struct vdec_av1_slice_state - decoding state
 * @err                   : err type for decode
 * @full                  : transcoded buffer is full or not
 * @timeout               : decode timeout or not
 * @perf                  : performance enable
 * @crc                   : hw checksum
 * @out_size              : hw output size
 */
struct vdec_av1_slice_state {
	int err;
	unsigned int full;
	unsigned int timeout;
	unsigned int perf;
	unsigned int crc[16];
	unsigned int out_size;
};

/*
 * enum vdec_av1_slice_resolution_level - resolution level
 */
enum vdec_av1_slice_resolution_level {
	AV1_RES_NONE,
	AV1_RES_FHD,
	AV1_RES_4K,
	AV1_RES_8K,
};

/*
 * enum vdec_av1_slice_frame_type - av1 frame type
 */
enum vdec_av1_slice_frame_type {
	AV1_KEY_FRAME = 0,
	AV1_INTER_FRAME = 1,
	AV1_INTRA_ONLY_FRAME = 2,
	AV1_SWITCH_FRAME = 3,
	AV1_FRAME_TYPES,
};

/*
 * enum vdec_av1_slice_reference_mode - reference mode type
 */
enum vdec_av1_slice_reference_mode {
	AV1_SINGLE_REFERENCE = 0,
	AV1_COMPOUND_REFERENCE = 1,
	AV1_REFERENCE_MODE_SELECT = 2,
	AV1_REFERENCE_MODES = 3,
};

/**
 * struct vdec_av1_slice_tile_group - info for each tile
 * @num_tiles               : tile number
 * @last_tile_in_tile_group : last tile in group or not
 * @tile_size               : input size for each tile
 * @tile_start_offset       : tile offset to input buffer
 */
struct vdec_av1_slice_tile_group {
	int num_tiles;
	unsigned char last_tile_in_tile_group[AV1_MAX_TILE_COUNT];
	unsigned int tile_size[AV1_MAX_TILE_COUNT];
	unsigned int tile_start_offset[AV1_MAX_TILE_COUNT];
};

/**
 * struct vdec_av1_slice_scale_factors - scale info for each ref frame
 * @is_scaled               : frame is scaled or not
 * @x_scale                 : frame width scale coefficient
 * @y_scale                 : frame height scale coefficient
 * @x_step                  : width step for x_scale
 * @y_step                  : height step for y_scale
 */
struct vdec_av1_slice_scale_factors {
	unsigned char is_scaled;
	int x_scale;
	int y_scale;
	int x_step;
	int y_step;
};

/**
 * struct vdec_av1_slice_frame_refs - ref frame info
 * @ref_fb_idx               : ref slot index
 * @ref_map_idx              : ref frame index
 * @scale_factors            : scale factors for each ref frame
 */
struct vdec_av1_slice_frame_refs {
	int ref_fb_idx;
	int ref_map_idx;
	struct vdec_av1_slice_scale_factors scale_factors;
};

/**
 * struct vdec_av1_slice_gm - AV1 Global Motion parameters
 * @wmtype                  : The type of global motion transform used
 * @wmmat                   : gm_params
 * @alpha                   : alpha info
 * @beta                    : beta info
 * @gamma                   : gamma info
 * @delta                   : delta info
 * @invalid                 : is invalid
 */
struct vdec_av1_slice_gm {
	int wmtype;
	int wmmat[8];
	short alpha;
	short beta;
	short gamma;
	short delta;
	char invalid;
};

/**
 * struct vdec_av1_slice_sm - AV1 Skip Mode parameters
 * @skip_mode_allowed    : Skip Mode is allowed or not
 * @skip_mode_present    : specified that the skip_mode will be present or not
 * @skip_mode_frame      : specifies the frames to use for compound prediction
 */
struct vdec_av1_slice_sm {
	unsigned char skip_mode_allowed;
	unsigned char skip_mode_present;
	int skip_mode_frame[2];
};

/**
 * struct vdec_av1_slice_seg - AV1 Segmentation params
 * @segmentation_enabled    : this frame makes use of the segmentation tool or not
 * @segmentation_update_map : segmentation map are updated during the decoding frame
 * @segmentation_temporal_update : updated to segmentation map are coded relative to the existing segmentaion map.
 * @segmentation_update_data    :new parameters are about to be specified for each segment
 * @feature_data    : specifies the feature data for a segment feature
 * @feature_enabled_mask    : the corresponding feature value is coded or not.
 * @segid_preskip    : segment id will be read before the skip syntax element.
 * @last_active_segid  :indicated the highest numbered segment id that has some enabled feature
 */
struct vdec_av1_slice_seg {
	unsigned char segmentation_enabled;
	unsigned char segmentation_update_map;
	unsigned char segmentation_temporal_update;
	unsigned char segmentation_update_data;
	int feature_data[V4L2_AV1_MAX_SEGMENTS][V4L2_AV1_SEG_LVL_MAX];
	unsigned short feature_enabled_mask[V4L2_AV1_MAX_SEGMENTS];
	int segid_preskip;
	int last_active_segid;
};

/**
 * struct vdec_av1_slicee_delta_q_lf - AV1 Loop Filter delta parameters
 * @delta_q_present  : specified whether quantizer index delta values are present
 * @delta_q_res  :  specifies the left shift which should be applied to decoded quantizer index
 * @delta_lf_present  : specifies whether loop filter delta values are present
 * @delta_lf_res  : specifies the left shift which should be applied to decoded 
 *                  loop filter delta values
 * @delta_lf_multi : specifies that separate loop filter deltas are sent for horizontal
 *                   luma edges,vertical luma edges,the u edges, and the v edges.
 */
struct vdec_av1_slicee_delta_q_lf {
	unsigned char delta_q_present;
	unsigned char delta_q_res;
	unsigned char delta_lf_present;
	unsigned char delta_lf_res;
	unsigned char delta_lf_multi;
};

/**
 * struct vdec_av1_slice_quantization - AV1 Quantization params
 * @base_q_idx  : indicates the base frame qindex. This is used for Y AC
 *               coefficients and as the base value for the other quantizers.
 * @qindex      : qindex 
 * @delta_qydc  : indicates the Y DC quantizer relative to base_q_idx
 * @delta_qudc  : indicates the U DC quantizer relative to base_q_idx.
 * @delta_quac  : indicates the U AC quantizer relative to base_q_idx
 * @delta_qvdc  : indicates the V DC quantizer relative to base_q_idx
 * @delta_qvac  : indicates the V AC quantizer relative to base_q_idx
 * @using_qmatrix : specifies that the quantizer matrix will be used to
 *                  compute quantizers
 * @qm_y        : specifies the level in the quantizer matrix that should
 *                be used for luma plane decoding
 * @qm_u        : specifies the level in the quantizer matrix that should
 *                be used for chroma U plane decoding.
 * @qm_v        : specifies the level in the quantizer matrix that should be
 *                used for chroma V plane decoding   
 */
struct vdec_av1_slice_quantization {
	int base_q_idx;
	int qindex[V4L2_AV1_MAX_SEGMENTS];
	int delta_qydc;
	int delta_qudc;
	int delta_quac;
	int delta_qvdc;
	int delta_qvac;
	unsigned char using_qmatrix;
	unsigned char qm_y;
	unsigned char qm_u;
	unsigned char qm_v;
};

/**
 * struct vdec_av1_slice_lr - AV1 Loop Restauration parameters
 * @use_lr        : whether to use loop restoration
 * @use_chroma_lr : whether to use chroma loop restoration
 * @frame_restoration_type : specifies the type of restoration used for each plane
 * @loop_restoration_size : pecifies the size of loop restoration units in units
 *                          of samples in the current plane
 */
struct vdec_av1_slice_lr {
	unsigned char use_lr;
	unsigned char use_chroma_lr;
	unsigned char frame_restoration_type[V4L2_AV1_NUM_PLANES_MAX];
	unsigned int loop_restoration_size[V4L2_AV1_NUM_PLANES_MAX];
};

/**
 * struct vdec_av1_slice_loop_filter - AV1 Loop filter parameters
 * @loop_filter_level : an array containing loop filter strength values.
 * @loop_filter_ref_deltas : contains the adjustment needed for the filter
 *                           level based on the chosen reference frame
 * @loop_filter_mode_deltas : contains the adjustment needed for the filter
 *                            level based on the chosen mode
 * @loop_filter_sharpness : indicates the sharpness level. The loop_filter_level
 *                          and loop_filter_sharpness together determine when
 *                          a block edge is filtered, and by how much the
 *                          filtering can change the sample values
 * @loop_filter_delta_enabled : filetr level depends on the mode and reference
 *                              frame used to predict a block
 */
struct vdec_av1_slice_loop_filter {
	unsigned char loop_filter_level[4];
	int loop_filter_ref_deltas[AV1_TOTAL_REFS_PER_FRAME];
	int loop_filter_mode_deltas[4];
	unsigned char loop_filter_sharpness;
	unsigned char loop_filter_delta_enabled;
};

/**
 * struct vdec_av1_slice_cdef - AV1 CDEF parameters
 * @cdef_damping : controls the amount of damping in the deringing filter
 * @cdef_y_strength  :specifies the strength of the primary filter and secondary filter 
 * @cdef_uv_strength : specifies the strength of the primary filter and secondary filter
 * @cdef_bits :specifies the number of bits needed to specify which
 *             CDEF filter to apply
 */
struct vdec_av1_slice_cdef {
	unsigned char cdef_damping;
	unsigned char cdef_y_strength[8];
	unsigned char cdef_uv_strength[8];
	unsigned char cdef_bits;
};

/**
 * struct vdec_av1_slice_mfmv - AV1 mfmv parameters
 * @mfmv_valid_ref : mfmv_valid_ref
 * @mfmv_dir : mfmv_dir
 * @mfmv_ref_to_cur : mfmv_ref_to_cur
 * @mfmv_ref_frame_idx : mfmv_ref_frame_idx
 * @mfmv_count : mfmv_count
 */
struct vdec_av1_slice_mfmv {
	unsigned int mfmv_valid_ref[3];
	unsigned int mfmv_dir[3];
	int mfmv_ref_to_cur[3];
	int mfmv_ref_frame_idx[3];
	int mfmv_count;
};

/**
 * struct vdec_av1_slice_tile - AV1 Tile info
 * @tile_cols : specifies the number of tiles across the frame
 * @tile_rows : pecifies the number of tiles down the frame
 * @mi_col_starts : an array specifying the start column
 * @mi_row_starts : an array specifying the start row
 * @context_update_tile_id : specifies which tile to use for the CDF update
 * @uniform_tile_spacing_flag : tiles are uniformly spaced across the frame
 *                              or the tile sizes are coded
 */
struct vdec_av1_slice_tile {
	int tile_cols;
	int tile_rows;
	int mi_col_starts[AV1_MAX_TILE_COLS + 1];
	int mi_row_starts[AV1_MAX_TILE_ROWS + 1];
	int context_update_tile_id;
	unsigned char uniform_tile_spacing_flag;
};

/**
 * struct vdec_av1_slice_uncompressed_header - Represents an AV1 Frame Header OBU
 * @use_ref_frame_mvs : use_ref_frame_mvs flag
 * @order_hint : specifies OrderHintBits least significant bits of the expected
 * @gm : global motion param
 * @upscaled_width : the upscaled width
 * @frame_width : frame's width
 * @frame_height : frame's height
 * @reduced_tx_set : frame is restricted to a reduced subset of the full
 *                   set of transform types
 * @tx_mode : specifies how the transform size is determined
 * @uniform_tile_spacing_flag : tiles are uniformly spaced across the frame
 *                              or the tile sizes are coded
 * @interpolation_filter : specifies the filter selection used for performing
 *                         inter prediction
 * @allow_warped_motion : motion_mode may be present or not
 * @is_motion_mode_switchable : euqlt to 0 specifies that only the
 * SIMPLE motion mode will be used
 * @reference_mode :
 * @allow_high_precision_mv : specifies that motion vectors are specified to
 * quarter pel precision or to eighth pel precision
 * @allow_intra_bc : ubducates that intra block copy may be used in this frame
 * @force_integer_mv : specifies motion vectors will always be integers or
 * can contain fractional bits
 * @allow_screen_content_tools : intra blocks may use palette encoding
 * @error_resilient_mode : error resislent mode is enable/disable
 * @frame_type : specifies the AV1 frame type
 * @primary_ref_frame : specifies which reference frame contains the CDF values
 * and other state taht should be loaded at the start of the frame
 * @refresh_frame_flags :contains a bitmask that specifies which reference frame
 * slots will be updated with the current frame after it is decoded
 * @disable_frame_end_update_cdf :
 * @disable_cdf_update : specified whether the CDF update in the symbol
 * decoding process should be disables
 * @skip_mode : av1 skip mode parameters
 * @seg : av1 segmentaon parameters
 * @delta_q_lf : av1 delta loop fileter 
 * @quant : av1 Quantization params
 * @lr : av1 Loop Restauration parameters
 * @superres_denom : the denominator for the upscaling ratio
 * @loop_filter : av1 Loop filter parameters
 * @cdef : av1 CDEF parameters
 * @mfmv : av1 mfmv parameters
 * @tile : av1 Tile info
 * @frame_refs : reference frame info
 * @frame_is_intra : intra frame
 * @loss_less_array : loss less array
 * @coded_loss_less : coded lsss less
 * @mi_rows : size of mi unit in rows
 * @mi_cols : size of mi unit in cols
 */
struct vdec_av1_slice_uncompressed_header {
	unsigned char use_ref_frame_mvs;
	int order_hint;
	struct vdec_av1_slice_gm gm[V4L2_AV1_TOTAL_REFS_PER_FRAME];
	unsigned int upscaled_width;
	unsigned int frame_width;
	unsigned int frame_height;
	unsigned char reduced_tx_set;
	unsigned char tx_mode;
	unsigned char uniform_tile_spacing_flag;
	unsigned char interpolation_filter;
	unsigned char allow_warped_motion;
	unsigned char is_motion_mode_switchable;
	unsigned char reference_mode;
	unsigned char allow_high_precision_mv;
	unsigned char allow_intra_bc;
	unsigned char force_integer_mv;
	unsigned char allow_screen_content_tools;
	unsigned char error_resilient_mode;
	unsigned char frame_type;
	unsigned char primary_ref_frame;
	unsigned int refresh_frame_flags;
	unsigned char disable_frame_end_update_cdf;
	unsigned int disable_cdf_update;
	struct vdec_av1_slice_sm skip_mode;
	struct vdec_av1_slice_seg seg;
	struct vdec_av1_slicee_delta_q_lf delta_q_lf;
	struct vdec_av1_slice_quantization quant;
	struct vdec_av1_slice_lr lr;
	unsigned int superres_denom;
	struct vdec_av1_slice_loop_filter loop_filter;
	struct vdec_av1_slice_cdef cdef;
	struct vdec_av1_slice_mfmv mfmv;
	struct vdec_av1_slice_tile tile;
	struct vdec_av1_slice_frame_refs frame_refs[AV1_REFS_PER_FRAME];
	unsigned char frame_is_intra;
	unsigned char loss_less_array[V4L2_AV1_MAX_SEGMENTS];
	unsigned char coded_loss_less;
	unsigned int mi_rows;
	unsigned int mi_cols;
};

/**
 * struct vdec_av1_slice_seq_header - Represents an AV1 Sequence OBU
 * @bitdepth : the bitdepth to use for the sequence
 * @enable_superres : specifies whether the use_superres syntax element
 * may be present
 * @enable_filter_intra : specifies the use_filter_intra syntax element
 * may be present 
 * @enable_intra_edge_filter : specifies whether the intra edge filtering
 * process should be enabled
 * @enable_interintra_compound : specifies the mode info fo rinter blocks may
 * contain the syntax element interintra 
 * @enable_masked_compound : specifies the mode info fo rinter blocks may
 * contain the syntax element compound_type
 * @enable_dual_filter : indicates the inter prediction filter type may
 * be specified independently in the horizontal and vertical directions
 * @enable_jnt_comp : indicates the distance weights process may
 * be used for inter prediction  
 * @mono_chrome : indicates the video does not contain U and V color planes
 * @enable_order_hint : indicates tools based on the values of order hints may
 * be used.
 * @order_hint_bits : specifies the number of bits used for the order_hint field
 * at each frame 
 * @use_128x128_superblock : indicates superblocks contain 128*128 luma samples
 * @subsampling_x : specifies the chroma subsamling format
 * @subsampling_y : specifies the chroma subsamling format
 * @max_frame_width : specifies the maximum frame width for the
 * frames represented by this sequence header
 * @max_frame_height : specifies the maximum frame height for the
 * frames represented by this sequence header
 */
struct vdec_av1_slice_seq_header {
	unsigned char bitdepth;
	unsigned char enable_superres;
	unsigned char enable_filter_intra;
	unsigned char enable_intra_edge_filter;
	unsigned char enable_interintra_compound;
	unsigned char enable_masked_compound;
	unsigned char enable_dual_filter;
	unsigned char enable_jnt_comp;
	unsigned char mono_chrome;
	unsigned char enable_order_hint;
	unsigned char order_hint_bits;
	unsigned char use_128x128_superblock;
	unsigned char subsampling_x;
	unsigned char subsampling_y;
	unsigned int max_frame_width;
	unsigned int max_frame_height;
};

/**
 * struct vdec_av1_slice_frame - Represents current Frame info
 * @uh : uncompressed header info
 * @seq : sequence header info
 * @large_scale_tile : is large scale mode
 * @cur_ts : current frame timestamp
 * @prev_fb_idx : prev slot id
 * @ref_frame_sign_bias : arrays for ref_frame sign bias
 * @order_hints : arrays for ref_frame order hint 
 * @ref_frame_valid : arrays for valid ref_frame
 * @frame_refs : ref_frame info
 */
struct vdec_av1_slice_frame {
	struct vdec_av1_slice_uncompressed_header uh;
	struct vdec_av1_slice_seq_header seq;
	unsigned char large_scale_tile;
	uint64_t cur_ts;
	int prev_fb_idx;
	unsigned char ref_frame_sign_bias[AV1_TOTAL_REFS_PER_FRAME];
	unsigned int order_hints[AV1_REFS_PER_FRAME];
	unsigned int ref_frame_valid[AV1_REFS_PER_FRAME];
	struct vdec_av1_slice_frame_refs frame_refs[AV1_REFS_PER_FRAME];
};

/**
 * struct vdec_av1_slice_work_buffer - work buffer for lat
 * @mv_addr :
 * @cdf_addr :
 * @segid_addr :
 */
struct vdec_av1_slice_work_buffer {
	struct vdec_av1_slice_mem mv_addr;
	struct vdec_av1_slice_mem cdf_addr;
	struct vdec_av1_slice_mem segid_addr;
};

/**
 * struct vdec_av1_slice_frame_info - frame info for each slot
 * @frame_type : frame type
 * @frame_is_intra : is intra frame
 * @order_hint : order hint
 * @upscaled_width : upscale width
 * @pic_pitch : buffer pitch
 * @frame_width : frane width
 * @frame_height : frame height
 * @mi_rows : rows in mode info
 * @mi_cols : cols in mode info
 * @ref_count : mark to reference frame counts
 */
struct vdec_av1_slice_frame_info {
	unsigned char frame_type;
	unsigned char frame_is_intra;
	int order_hint;
	unsigned int upscaled_width;
	unsigned int pic_pitch;
	unsigned int frame_width;
	unsigned int frame_height;
	unsigned int mi_rows;
	unsigned int mi_cols;
	int ref_count;
};

/**
 * struct vdec_av1_slice_slot - slot info need save in golbal instance
 * @frame_info : frame infos
 * @ref_frame_map : point to frame_info
 * @timestamp : time stamp
 */
struct vdec_av1_slice_slot {
       struct vdec_av1_slice_frame_info frame_info[AV1_MAX_FRAME_BUF_COUNT];
	   int ref_frame_map[AV1_TOTAL_REFS_PER_FRAME];
       uint64_t timestamp[AV1_MAX_FRAME_BUF_COUNT];
};

/**
 * struct vdec_av1_slice_fb - frame buffer for decoding
 * @y : current y buffer adress info
 * @c : current c buffer adress info
 */
struct vdec_av1_slice_fb {
	struct vdec_av1_slice_mem y;
	struct vdec_av1_slice_mem c;
};

/**
 * struct vdec_av1_slice_frame - exchange frame information
 * 								 between Main CPU and MicroP
 * @bs: 
 * @work_buffer : 
 * @cdf_table : 
 * @cdf_tmp: 
 * @rd_mv : 
 * @ube: 
 * @trans: 
 * @err_map: 
 * @row_info : 
 * @fb : 
 * @ref : 
 * @iq_table : 
 * @tile : 
 * @slots : 
 * @slot_id : 
 * @frame : 
 * @state : 
 * @cur_lst_tile_id : 
 */
struct vdec_av1_slice_vsi {
	/* lat */
	struct vdec_av1_slice_mem bs;
	struct vdec_av1_slice_work_buffer work_buffer[AV1_MAX_FRAME_BUF_COUNT];
	struct vdec_av1_slice_mem cdf_table;
	struct vdec_av1_slice_mem cdf_tmp;
	/* LAT stage's output, Core stage's input */
	struct vdec_av1_slice_mem rd_mv;
	struct vdec_av1_slice_mem ube;
	struct vdec_av1_slice_mem trans;
	struct vdec_av1_slice_mem err_map;
	struct vdec_av1_slice_mem row_info;
	/* core */
	struct vdec_av1_slice_fb fb;
	struct vdec_av1_slice_fb ref[AV1_REFS_PER_FRAME];
	struct vdec_av1_slice_mem iq_table;
	/* lat and core share*/
	struct vdec_av1_slice_mem tile;
	struct vdec_av1_slice_slot slots;
	unsigned char slot_id;
	struct vdec_av1_slice_frame frame;
	struct vdec_av1_slice_state state;
	unsigned int cur_lst_tile_id;
};

/*
 * struct vdec_av1_slice_pfc - per-frame context that contains a local vsi.
 *                             pass it from lat to core
 * @vsi         : local vsi. copy to/from remote vsi before/after decoding
 * @ref_idx     : reference buffer timestamp
 * @seq         : picture sequence
 */
struct vdec_av1_slice_pfc {
	struct vdec_av1_slice_vsi vsi;
	u64 ref_idx[AV1_REFS_PER_FRAME];
	int seq;
};


/*
 * struct vdec_av1_slice_instance - represent one av1 instance
 * @ctx         : pointer to codec's context
 * @vpu         : VPU instance
 * @iq_table    : iq table buffer
 * @cdf_table   : cdf table buffer
 * @mv          : mv working buffer
 * @cdf         : cdf working buffer
 * @seg         : segmentation working buffer
 * @cdf_temp    : cdf temp buffer
 * @tile		: tile buffer
 * @slots       : slots info
 * @tile_group  : tile_group entry
 * @level		: level of current resolution
 * @width       : width of last picture
 * @height      : height of last picture
 * @frame_type  : frame_type of last picture
 * @irq         : irq to Main CPU or MicroP
 * @show_frame  : show_frame of last picture
 * @init_vsi    : vsi used for initialized AV1 instance
 * @vsi         : vsi used for decoding/flush ...
 * @core_vsi    : vsi used for Core stage
 * @seq         : global picture sequence
 */
struct vdec_av1_slice_instance {
	struct mtk_vcodec_ctx *ctx;
	struct vdec_vpu_inst vpu;

	struct mtk_vcodec_mem iq_table;
	struct mtk_vcodec_mem cdf_table;

	struct mtk_vcodec_mem mv[AV1_MAX_FRAME_BUF_COUNT];
	struct mtk_vcodec_mem cdf[AV1_MAX_FRAME_BUF_COUNT];
	struct mtk_vcodec_mem seg[AV1_MAX_FRAME_BUF_COUNT];
	struct mtk_vcodec_mem cdf_temp;
	struct mtk_vcodec_mem tile;
	struct vdec_av1_slice_slot slots;
	struct vdec_av1_slice_tile_group tile_group;
	
	/* for resolution change and get_pic_info */
	enum vdec_av1_slice_resolution_level level;
	unsigned int width;
	unsigned int height;

	unsigned int frame_type;
	unsigned int irq;
	unsigned int inneracing_mode;

	/* MicroP vsi */
	union {
		struct vdec_av1_slice_init_vsi *init_vsi;
		struct vdec_av1_slice_vsi *vsi;
	};
	struct vdec_av1_slice_vsi *core_vsi;
	int seq;
};

static int vdec_av1_slice_core_decode(
	struct vdec_lat_buf *lat_buf);

static inline int clip3(int value, int low, int high)
{
  return value < low ? low : (value > high ? high : value);
}

static inline int get_msb(unsigned int n)
{
	if (n == 0)
		return 0;
	return 31 ^ __builtin_clz(n);
}

static inline int valid_ref_frame_size(
	unsigned int ref_width,
	unsigned int ref_height,
	unsigned int this_width,
	unsigned int this_height)
{
	return ((this_width << 1) >= ref_width) &&
		((this_height << 1) >= ref_height) &&
		(this_width <= (ref_width << 4)) &&
		(this_height <= (ref_height << 4));
}

static void *vdec_av1_get_ctrl_ptr(
	struct mtk_vcodec_ctx *ctx,
	int id)
{
	struct v4l2_ctrl *ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, id);

	return ctrl->p_cur.p;
}

static int vdec_av1_slice_init_cdf_table(
	struct vdec_av1_slice_instance *instance)
{
	unsigned char *remote_cdf_table;
	struct mtk_vcodec_ctx *ctx;
	struct vdec_av1_slice_init_vsi *vsi;
	int ret = 0;

	ctx = instance->ctx;
	vsi = instance->vpu.vsi;
	if (!ctx || !vsi) {
		mtk_vcodec_err(instance, "invalid ctx or vsi 0x%px 0x%px\n",
			ctx, vsi);
		return -EINVAL;
	}

	remote_cdf_table = mtk_vcodec_fw_map_dm_addr(ctx->dev->fw_handler,
		(u32)vsi->cdf_table_addr);
	if (!remote_cdf_table) {
		mtk_vcodec_err(instance, "failed to map cdf table\n");
		goto err;
	}

	mtk_vcodec_debug(instance, "map cdf table to 0x%px\n",
		remote_cdf_table);

	if (instance->cdf_table.va)
		mtk_vcodec_mem_free(ctx, &instance->cdf_table);
	instance->cdf_table.size = vsi->cdf_table_size;
	if (mtk_vcodec_mem_alloc(ctx, &instance->cdf_table))
		goto err;
	memcpy_fromio(instance->cdf_table.va, remote_cdf_table, vsi->cdf_table_size);

	return 0;
err:
	ret = -ENOMEM;
	return ret;
}

static int vdec_av1_slice_init_iq_table(
	struct vdec_av1_slice_instance *instance)
{
	unsigned char *remote_iq_table;
	struct mtk_vcodec_ctx *ctx;
	struct vdec_av1_slice_init_vsi *vsi;
	int ret = 0;

	ctx = instance->ctx;
	vsi = instance->vpu.vsi;
	if (!ctx || !vsi) {
		mtk_vcodec_err(instance, "invalid ctx or vsi 0x%px 0x%px\n",
			ctx, vsi);
		return -EINVAL;
	}

	remote_iq_table = mtk_vcodec_fw_map_dm_addr(ctx->dev->fw_handler,
		(u32)vsi->iq_table_addr);
	if (!remote_iq_table) {
		mtk_vcodec_err(instance, "failed to map iq table\n");
		goto err;
	}

	mtk_vcodec_debug(instance, "map iq table to 0x%px\n",
		remote_iq_table);

	if (instance->iq_table.va)
		mtk_vcodec_mem_free(ctx, &instance->iq_table);
	instance->iq_table.size = vsi->iq_table_size;
	if (mtk_vcodec_mem_alloc(ctx, &instance->iq_table))
		goto err;
	memcpy_fromio(instance->iq_table.va, remote_iq_table, vsi->iq_table_size);

	return 0;
err:
	ret = -ENOMEM;
	return ret;
}

static int vdec_av1_slice_get_new_slot(
	struct vdec_av1_slice_vsi *vsi)
{
	struct vdec_av1_slice_slot *slots = &vsi->slots;

	int new_slot_idx = AV1_INVALID_IDX;
	int i = 0;

	for (i = 0; i < AV1_MAX_FRAME_BUF_COUNT; i ++) {
		if (slots->frame_info[i].ref_count == 0) {
			new_slot_idx = i;
			break;
		}
	}
	if (new_slot_idx != AV1_INVALID_IDX) {
		slots->frame_info[new_slot_idx].ref_count ++;
		slots->timestamp[new_slot_idx] = vsi->frame.cur_ts;
	}

	return new_slot_idx;
}

static void vdec_av1_slice_clear_fb(
	struct vdec_av1_slice_frame_info *frame_info)
{
	memset_io((void *)frame_info, 0, sizeof(struct vdec_av1_slice_frame_info));
}

static int vdec_av1_slice_decrease_ref_count(
	struct vdec_av1_slice_slot *slots,
	int fb_idx)
{
	struct vdec_av1_slice_frame_info *frame_info = slots->frame_info;

	if (fb_idx >= 0 && fb_idx < AV1_MAX_FRAME_BUF_COUNT) {
		frame_info[fb_idx].ref_count--;
		if (frame_info[fb_idx].ref_count < 0) {
			frame_info[fb_idx].ref_count = 0;
			mtk_v4l2_err(
				"av1_error: %s() fb_idx %d decrease ref_count error\n",
				__func__, fb_idx);
			return -EINVAL;
		}
		if (frame_info[fb_idx].ref_count == 0) {
			vdec_av1_slice_clear_fb(&frame_info[fb_idx]);
		}
		return 0;
	} else
		mtk_v4l2_err(
			"av1_error: %s() invalid fb_idx %d\n", __func__, fb_idx);
	return -EINVAL;
}

static int vdec_av1_slice_increase_ref_count(
	struct vdec_av1_slice_slot *slots,
	int fb_idx)
{
	if (fb_idx >= 0 && fb_idx < AV1_MAX_FRAME_BUF_COUNT) {
		slots->frame_info[fb_idx].ref_count++;
		return 0;
	} else
		mtk_v4l2_err(
			"av1_error: %s() invalid fb_idx %d\n", __func__, fb_idx);
	return -EINVAL;
}

static int vdec_av1_slice_update_ref_slot(
	struct vdec_av1_slice_vsi *vsi,
	struct vdec_av1_slice_slot *slots)
{
	struct vdec_av1_slice_uncompressed_header *uh = &vsi->frame.uh;
	int *ref_frame_map = slots->ref_frame_map;
	unsigned int mask = 0;
	int i = 0;

	for (mask = uh->refresh_frame_flags;
		(mask && i < AV1_TOTAL_REFS_PER_FRAME); mask >>= 1) {
		if (mask & 1) {
			if (ref_frame_map[i] != AV1_INVALID_IDX)
				vdec_av1_slice_decrease_ref_count(slots, ref_frame_map[i]);
			ref_frame_map[i] = vsi->slot_id;
			vdec_av1_slice_increase_ref_count(slots, ref_frame_map[i]);
		}
		i++;
	}

	vdec_av1_slice_decrease_ref_count(slots, vsi->slot_id);

	return 0;
}

static int vdec_av1_slice_setup_slot(
	struct vdec_av1_slice_instance *instance,
	struct vdec_av1_slice_vsi *vsi)
{
 	struct vdec_av1_slice_frame_info *cur_frame_info;
	struct vdec_av1_slice_uncompressed_header *uh = &vsi->frame.uh;

	memcpy_toio(&vsi->slots, &instance->slots, sizeof(instance->slots));
	vsi->slot_id = vdec_av1_slice_get_new_slot(vsi);

	if (vsi->slot_id == AV1_INVALID_IDX) {
		mtk_v4l2_err("warning:av1 get invalid index slot\n");
		vsi->slot_id = 0;
	}
	cur_frame_info =
			&vsi->slots.frame_info[vsi->slot_id];

	cur_frame_info->frame_type = uh->frame_type;
	cur_frame_info->frame_is_intra =
		((uh->frame_type == AV1_INTRA_ONLY_FRAME) ||
		(uh->frame_type == AV1_KEY_FRAME));
	cur_frame_info->order_hint = uh->order_hint;

	cur_frame_info->upscaled_width = uh->upscaled_width;
	cur_frame_info->pic_pitch = 0;
	cur_frame_info->frame_width = uh->frame_width;
	cur_frame_info->frame_height = uh->frame_height;
	cur_frame_info->mi_cols = ((uh->frame_width + 7) >> 3) << 1;
	cur_frame_info->mi_rows = ((uh->frame_height + 7) >> 3) << 1;

	return 0;
}
static int vdec_av1_slice_alloc_working_buffer(
	struct vdec_av1_slice_instance *instance,
	struct vdec_av1_slice_vsi *vsi)
{
	struct mtk_vcodec_ctx *ctx = instance->ctx;
	struct vdec_av1_slice_work_buffer *work_buffer = vsi->work_buffer;
	enum vdec_av1_slice_resolution_level level;

	unsigned int max_sb_w;
	unsigned int max_sb_h;
	unsigned int max_w;
	unsigned int max_h;
	unsigned int w;
	unsigned int h;
	size_t size;
	int ret;
	int i;

	w = vsi->frame.uh.frame_width;
	h = vsi->frame.uh.frame_height;

	if (w > VCODEC_DEC_4K_CODED_WIDTH ||
		h > VCODEC_DEC_4K_CODED_HEIGHT) {
		/* 8K? */
		return -EINVAL;
	} else if (w > MTK_VDEC_MAX_W || h > MTK_VDEC_MAX_H) {
		/* 4K */
		level = AV1_RES_4K;
		max_w = VCODEC_DEC_4K_CODED_WIDTH;
		max_h = VCODEC_DEC_4K_CODED_HEIGHT;
	} else {
		/* FHD */
		level = AV1_RES_FHD;
		max_w = MTK_VDEC_MAX_W;
		max_h = MTK_VDEC_MAX_H;
	}

	if (level == instance->level)
		return 0;

	mtk_vcodec_debug(instance,
		"resolution level changed, from %u to %u, %ux%u",
		instance->level, level, w, h);

	max_sb_w = DIV_ROUND_UP(max_w, 128);
	max_sb_h = DIV_ROUND_UP(max_h, 128);
	ret = -ENOMEM;

	/*
	 * Lat-flush must wait core idle, otherwise core will
	 * use released buffers
	 */

	size = max_sb_w * max_sb_h * 1024;
	for (i = 0; i < AV1_MAX_FRAME_BUF_COUNT; i++) {
		if (instance->mv[i].va)
			mtk_vcodec_mem_free(ctx, &instance->mv[i]);
		instance->mv[i].size = size;
		if (mtk_vcodec_mem_alloc(ctx, &instance->mv[i]))
			goto err;
		work_buffer[i].mv_addr.buf = instance->mv[i].dma_addr;
		work_buffer[i].mv_addr.size = size;
		
	}

	size = max_sb_w * max_sb_h * 512;
	for (i = 0; i < AV1_MAX_FRAME_BUF_COUNT; i++) {
		if (instance->seg[i].va)
			mtk_vcodec_mem_free(ctx, &instance->seg[i]);
		instance->seg[i].size = size;
		if (mtk_vcodec_mem_alloc(ctx, &instance->seg[i]))
			goto err;
		work_buffer[i].segid_addr.buf = instance->seg[i].dma_addr;
		work_buffer[i].segid_addr.size = size;
	}
	size = 16384;
	for (i = 0; i < AV1_MAX_FRAME_BUF_COUNT; i++) {
		if (instance->cdf[i].va)
			mtk_vcodec_mem_free(ctx, &instance->cdf[i]);
		instance->cdf[i].size = size;
		if (mtk_vcodec_mem_alloc(ctx, &instance->cdf[i]))
			goto err;
		work_buffer[i].cdf_addr.buf = instance->cdf[i].dma_addr;
		work_buffer[i].cdf_addr.size = size;
	}
	if (!instance->cdf_temp.va) {
		instance->cdf_temp.size = (1024*16*100);
		if (mtk_vcodec_mem_alloc(ctx, &instance->cdf_temp))
			goto err;
		vsi->cdf_tmp.buf = instance->cdf_temp.dma_addr;
		vsi->cdf_tmp.size = instance->cdf_temp.size;
	}
	size = AV1_TILE_BUF_SIZE * AV1_MAX_TILE_COUNT;

	if (instance->tile.va)
		mtk_vcodec_mem_free(ctx, &instance->tile);
	instance->tile.size = size;
	if (mtk_vcodec_mem_alloc(ctx, &instance->tile))
		goto err;
	vsi->tile.buf = instance->tile.dma_addr;
	vsi->tile.size= size;

	instance->level = level;
	return 0;

err:
	instance->level = AV1_RES_NONE;
	return ret;
}

static void vdec_av1_slice_free_working_buffer(
	struct vdec_av1_slice_instance *instance)
{
	struct mtk_vcodec_ctx *ctx = instance->ctx;
	int i;

	for (i = 0; i < ARRAY_SIZE(instance->mv); i++) {
		if (instance->mv[i].va)
			mtk_vcodec_mem_free(ctx, &instance->mv[i]);
	}
	for (i = 0; i < ARRAY_SIZE(instance->seg); i++) {
		if (instance->seg[i].va)
			mtk_vcodec_mem_free(ctx, &instance->seg[i]);
	}
	for (i = 0; i < ARRAY_SIZE(instance->cdf); i++) {
		if (instance->cdf[i].va)
			mtk_vcodec_mem_free(ctx, &instance->cdf[i]);
	}

	if (instance->tile.va)
		mtk_vcodec_mem_free(ctx, &instance->tile);
	if (instance->cdf_temp.va)
		mtk_vcodec_mem_free(ctx, &instance->cdf_temp);
	if (instance->cdf_table.va)
		mtk_vcodec_mem_free(ctx, &instance->cdf_table);
	if (instance->iq_table.va)
		mtk_vcodec_mem_free(ctx, &instance->iq_table);

	instance->level = AV1_RES_NONE;
}

static void vdec_av1_slice_vsi_from_remote(
	struct vdec_av1_slice_vsi *vsi,
	struct vdec_av1_slice_vsi *remote_vsi,
	int skip)
{
	/*
	 * buffer position
	 * decode state
	 */
	if (!skip) {
		//modify in scp, and core thread will use it.
		//memcpy_fromio(&vsi->frame.uh.mfmv, &remote_vsi->frame.uh.mfmv,
		//	sizeof(vsi->frame.uh.mfmv)); //add to modify 
		memcpy_fromio(&vsi->trans, &remote_vsi->trans,
			sizeof(vsi->trans));
	}
	memcpy_fromio(&vsi->state, &remote_vsi->state, sizeof(vsi->state));
}

static void vdec_av1_slice_vsi_to_remote(
	struct vdec_av1_slice_vsi *vsi,
	struct vdec_av1_slice_vsi *remote_vsi)
{
	memcpy_toio(remote_vsi, vsi, sizeof(*vsi));
}


static int vdec_av1_slice_setup_lat_from_src_buf(
	struct vdec_av1_slice_instance *instance,
	struct vdec_av1_slice_vsi *vsi,
	struct vdec_lat_buf *lat_buf)
{
	struct vb2_v4l2_buffer *src;
	struct vb2_v4l2_buffer *dst;

	src = v4l2_m2m_next_src_buf(instance->ctx->m2m_ctx);
	if (!src)
		return -EINVAL;

	dst = &lat_buf->ts_info;
	v4l2_m2m_buf_copy_metadata(src, dst, true);
	vsi->frame.cur_ts = dst->vb2_buf.timestamp;
	return 0;
}

static short vdec_av1_slice_resolve_divisor_32(
	unsigned int D, short *shift)
{
	int f;
	int e;

	*shift = get_msb(D);
	// e is obtained from D after resetting the most significant 1 bit.
	e = D - ((unsigned int)1 << *shift);
	// Get the most significant DIV_LUT_BITS (8) bits of e into f
	if (*shift > DIV_LUT_BITS)
		f = ROUND_POWER_OF_TWO(e, *shift - DIV_LUT_BITS);
	else
		f = e << (DIV_LUT_BITS - *shift);
	if (f > DIV_LUT_NUM)
		return -1;
	*shift += DIV_LUT_PREC_BITS;
	// Use f as lookup into the precomputed table of multipliers
	return div_lut[f];
}


static int vdec_av1_slice_get_shear_params(
	struct vdec_av1_slice_gm *gm_params)
{
	const int *mat = gm_params->wmmat;
	short shift;
	short y;
	long long v;

	if (gm_params->wmmat[2] <= 0)
		return 0;

	gm_params->alpha =
		clip3(mat[2] - (1 << WARPEDMODEL_PREC_BITS), INT16_MIN, INT16_MAX);
	gm_params->beta = clip3(mat[3], INT16_MIN, INT16_MAX);

	y = vdec_av1_slice_resolve_divisor_32(abs(mat[2]), &shift) * (mat[2] < 0 ? -1 : 1);
	v = ((long long)mat[4] * (1 << WARPEDMODEL_PREC_BITS)) * y;
	gm_params->gamma = clip3((int)ROUND_POWER_OF_TWO_SIGNED_64(v, shift), INT16_MIN, INT16_MAX);
	v = ((long long)mat[3] * mat[4]) * y;
	gm_params->delta = clip3(mat[5] - (int)ROUND_POWER_OF_TWO_SIGNED_64(v, shift) -
        (1 << WARPEDMODEL_PREC_BITS), INT16_MIN, INT16_MAX);
	gm_params->alpha = ROUND_POWER_OF_TWO_SIGNED(gm_params->alpha, WARP_PARAM_REDUCE_BITS) *
		(1 << WARP_PARAM_REDUCE_BITS);
	gm_params->beta = ROUND_POWER_OF_TWO_SIGNED(gm_params->beta, WARP_PARAM_REDUCE_BITS) *
		(1 << WARP_PARAM_REDUCE_BITS);
	gm_params->gamma = ROUND_POWER_OF_TWO_SIGNED(gm_params->gamma, WARP_PARAM_REDUCE_BITS) *
		(1 << WARP_PARAM_REDUCE_BITS);
	gm_params->delta = ROUND_POWER_OF_TWO_SIGNED(gm_params->delta, WARP_PARAM_REDUCE_BITS) *
		(1 << WARP_PARAM_REDUCE_BITS);
	return 0;
}

static void vdec_av1_slice_setup_gm(
	struct vdec_av1_slice_gm *gm,
	struct v4l2_av1_global_motion *ctrl_gm)
{
	unsigned int i, j;
	for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++) {
		gm[i].wmtype = ctrl_gm->type[i];
		for (j = 0; j < 6; j++)
			gm[i].wmmat[j] = ctrl_gm->params[i][j];
		gm[i].invalid = !!(ctrl_gm->invalid & (1 << i));
		gm[i].alpha = 0;
		gm[i].beta = 0;
		gm[i].gamma = 0;
		gm[i].delta = 0;
		if (gm[i].wmtype <= 3)
			vdec_av1_slice_get_shear_params(&gm[i]);
		
	}
}

static void vdec_av1_slice_setup_seg(
	struct vdec_av1_slice_seg *seg,
	struct v4l2_av1_segmentation *ctrl_seg)
{
	unsigned int i, j;
	seg->segmentation_enabled =
		BIT_FLAG(ctrl_seg, V4L2_AV1_SEGMENTATION_FLAG_ENABLED);
	seg->segmentation_update_map =
		BIT_FLAG(ctrl_seg, V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP);
	seg->segmentation_temporal_update =
		BIT_FLAG(ctrl_seg, V4L2_AV1_SEGMENTATION_FLAG_TEMPORAL_UPDATE);
	seg->segmentation_update_data =
		BIT_FLAG(ctrl_seg, V4L2_AV1_SEGMENTATION_FLAG_UPDATE_DATA);
	seg->segid_preskip =
		BIT_FLAG(ctrl_seg, V4L2_AV1_SEGMENTATION_FLAG_SEG_ID_PRE_SKIP);
	seg->last_active_segid = ctrl_seg->last_active_seg_id;

	for (i = 0; i < V4L2_AV1_MAX_SEGMENTS; i++) {
		seg->feature_enabled_mask[i] = ctrl_seg->feature_enabled[i];
		for (j = 0; j < V4L2_AV1_SEG_LVL_MAX; j++)
			seg->feature_data[i][j] = ctrl_seg->feature_data[i][j];
	}
}

static void vdec_av1_slice_setup_quant(
	struct vdec_av1_slice_quantization *quant,
	struct v4l2_av1_quantization *ctrl_quant)
{
	quant->base_q_idx = ctrl_quant->base_q_idx;
	quant->delta_qydc = ctrl_quant->delta_q_y_dc;
	quant->delta_qudc = ctrl_quant->delta_q_u_dc;
	quant->delta_quac = ctrl_quant->delta_q_u_ac;
	quant->delta_qvdc = ctrl_quant->delta_q_v_dc;
	quant->delta_qvac = ctrl_quant->delta_q_v_ac;
	quant->qm_y = ctrl_quant->qm_y;
	quant->qm_u = ctrl_quant->qm_u;
	quant->qm_v = ctrl_quant->qm_v;
	quant->using_qmatrix =
		BIT_FLAG(ctrl_quant, V4L2_AV1_QUANTIZATION_FLAG_USING_QMATRIX);
}

static int vdec_av1_slice_get_qindex(
	struct vdec_av1_slice_uncompressed_header *uh,
	int segmentation_id)
{
	struct vdec_av1_slice_seg *seg = &uh->seg;
	struct vdec_av1_slice_quantization *quant = &uh->quant;
	int data = 0, qindex = 0;

	if (seg->segmentation_enabled &&
		(seg->feature_enabled_mask[segmentation_id] & (1 << 0))) {
		data = seg->feature_data[segmentation_id][0];
		qindex = quant->base_q_idx + data;
		return clip3(qindex, 0, MAXQ);
	}

	return quant->base_q_idx;
}

static void vdec_av1_slice_setup_lr(
	struct vdec_av1_slice_lr *lr,
	struct v4l2_av1_loop_restoration  *ctrl_lr)
{
	int i;

	lr->use_lr =
		BIT_FLAG(ctrl_lr, V4L2_AV1_LOOP_RESTORATION_FLAG_USES_LR);
	lr->use_chroma_lr =
		BIT_FLAG(ctrl_lr, V4L2_AV1_LOOP_RESTORATION_FLAG_USES_CHROMA_LR);

	for (i = 0; i < V4L2_AV1_NUM_PLANES_MAX; i++) {
		lr->frame_restoration_type[i] = ctrl_lr->frame_restoration_type[i];
		lr->loop_restoration_size[i] = ctrl_lr->loop_restoration_size[i];
	}
}

static void vdec_av1_slice_setup_lf (
	struct vdec_av1_slice_loop_filter *lf,
	struct v4l2_av1_loop_filter *ctrl_lf)
{
	int i;

	for (i = 0; i < 4; i++)
		lf->loop_filter_level[i] = ctrl_lf->level[i];

	for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++)
			lf->loop_filter_ref_deltas[i] = ctrl_lf->ref_deltas[i];

	for (i = 0; i < 2; i++)
		lf->loop_filter_mode_deltas[i] = ctrl_lf->mode_deltas[i];

	lf->loop_filter_sharpness = ctrl_lf->sharpness;
	lf->loop_filter_delta_enabled =
		   BIT_FLAG(ctrl_lf, V4L2_AV1_LOOP_FILTER_FLAG_DELTA_ENABLED);
}

static void vdec_av1_slice_setup_cdef (
	struct vdec_av1_slice_cdef *cdef,
	struct v4l2_av1_cdef *ctrl_cdef)
{
	int i;

	cdef->cdef_damping = ctrl_cdef->damping_minus_3 + 3;
	cdef->cdef_bits = ctrl_cdef->bits;

	for (i = 0; i < V4L2_AV1_CDEF_MAX; i++) {
		if (ctrl_cdef->y_sec_strength[i] == 4)
			ctrl_cdef->y_sec_strength[i] -= 1;

		if (ctrl_cdef->uv_sec_strength[i] == 4)
			ctrl_cdef->uv_sec_strength[i] -= 1;

		cdef->cdef_y_strength[i] = ctrl_cdef->y_pri_strength[i] << 2 |
			ctrl_cdef->y_sec_strength[i];
		cdef->cdef_uv_strength[i] = ctrl_cdef->uv_pri_strength[i] << 2 |
				ctrl_cdef->uv_sec_strength[i];
	}
}

static void vdec_av1_slice_setup_seq(
	struct vdec_av1_slice_seq_header *seq,
	struct v4l2_ctrl_av1_sequence *ctrl_seq)
{
	seq->bitdepth = ctrl_seq->bit_depth;
	seq->max_frame_width = ctrl_seq->max_frame_width_minus_1 + 1;
	seq->max_frame_height = ctrl_seq->max_frame_height_minus_1 + 1;
	seq->enable_superres =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_ENABLE_SUPERRES);
	seq->enable_filter_intra =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_ENABLE_FILTER_INTRA);
	seq->enable_intra_edge_filter =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTRA_EDGE_FILTER);
	seq->enable_interintra_compound =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTERINTRA_COMPOUND);
	seq->enable_masked_compound =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_ENABLE_MASKED_COMPOUND);
	seq->enable_dual_filter =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_ENABLE_DUAL_FILTER);
	seq->enable_jnt_comp =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_ENABLE_JNT_COMP);
	seq->mono_chrome =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_MONO_CHROME);
	seq->enable_order_hint =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_ENABLE_ORDER_HINT);
	seq->order_hint_bits = ctrl_seq->order_hint_bits;
	seq->use_128x128_superblock =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_USE_128X128_SUPERBLOCK);
	seq->subsampling_x =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X);
	seq->subsampling_y =
		BIT_FLAG(ctrl_seq, V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y);
}

static void vdec_av1_slice_setup_tile(
	struct vdec_av1_slice_frame *frame,
	struct v4l2_av1_tile_info *ctrl_tile)
{
	struct vdec_av1_slice_seq_header *seq = &frame->seq;
	struct vdec_av1_slice_tile *tile = &frame->uh.tile;
	int i;
	unsigned int mib_size_log2 = seq->use_128x128_superblock ? 5 : 4;

	tile->tile_cols = ctrl_tile->tile_cols;
	tile->tile_rows = ctrl_tile->tile_rows;
	tile->context_update_tile_id = ctrl_tile->context_update_tile_id;
	tile->uniform_tile_spacing_flag =
		BIT_FLAG(ctrl_tile, V4L2_AV1_TILE_INFO_FLAG_UNIFORM_TILE_SPACING); 

	for (i = 0; i < tile->tile_cols + 1; i++) {
		tile->mi_col_starts[i] =
			ALIGN_POWER_OF_TWO(ctrl_tile->mi_col_starts[i],mib_size_log2)
				>> mib_size_log2;
	}
	for (i = 0; i < tile->tile_rows + 1; i++) {
		tile->mi_row_starts[i] =
			ALIGN_POWER_OF_TWO(ctrl_tile->mi_row_starts[i], mib_size_log2)
				>> mib_size_log2;
	}
}

static void vdec_av1_slice_setup_uh(
	struct vdec_av1_slice_instance *instance,
	struct vdec_av1_slice_frame *frame,
	struct v4l2_ctrl_av1_frame_header *ctrl_fh)
{
	int i;
	struct vdec_av1_slice_uncompressed_header *uh = &frame->uh;

	uh->use_ref_frame_mvs =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_USE_REF_FRAME_MVS);
 	uh->order_hint = ctrl_fh->order_hint;
	vdec_av1_slice_setup_gm(uh->gm, &ctrl_fh->global_motion);
	uh->upscaled_width = ctrl_fh->upscaled_width;
	uh->frame_width = ctrl_fh->frame_width_minus_1 + 1;
	uh->frame_height = ctrl_fh->frame_height_minus_1 + 1;
	uh->mi_cols = ((uh->frame_width + 7) >> 3) << 1;
	uh->mi_rows = ((uh->frame_height + 7) >> 3) << 1;
	uh->reduced_tx_set =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_REDUCED_TX_SET);
	uh->tx_mode = ctrl_fh->tx_mode;
	uh->uniform_tile_spacing_flag =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_UNIFORM_TILE_SPACING);
	uh->interpolation_filter = ctrl_fh->interpolation_filter;
	uh->allow_warped_motion =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_ALLOW_WARPED_MOTION);
	uh->is_motion_mode_switchable =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_IS_MOTION_MODE_SWITCHABLE);
	uh->frame_type = ctrl_fh->frame_type;
	uh->frame_is_intra = (uh->frame_type == V4L2_AV1_INTRA_ONLY_FRAME ||
				uh->frame_type == V4L2_AV1_KEY_FRAME);
	if (uh->frame_is_intra)
		uh->reference_mode = AV1_SINGLE_REFERENCE;
	else
		uh->reference_mode =
			BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_REFERENCE_SELECT) ?
			AV1_REFERENCE_MODE_SELECT : AV1_SINGLE_REFERENCE;

	uh->allow_high_precision_mv =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_ALLOW_HIGH_PRECISION_MV);
	uh->allow_intra_bc =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_ALLOW_INTRABC);
	uh->force_integer_mv =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_FORCE_INTEGER_MV);
	uh->allow_screen_content_tools =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_ALLOW_SCREEN_CONTENT_TOOLS);
	uh->error_resilient_mode =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_ERROR_RESILIENT_MODE);
	
	uh->primary_ref_frame = ctrl_fh->primary_ref_frame;
	uh->refresh_frame_flags = ctrl_fh->refresh_frame_flags;
	uh->disable_frame_end_update_cdf =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_DISABLE_FRAME_END_UPDATE_CDF);
	uh->disable_cdf_update =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_DISABLE_CDF_UPDATE);
	uh->skip_mode.skip_mode_allowed =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_SKIP_MODE_ALLOWED);
	uh->skip_mode.skip_mode_present =
		BIT_FLAG(ctrl_fh, V4L2_AV1_FRAME_HEADER_FLAG_SKIP_MODE_PRESENT);
	uh->skip_mode.skip_mode_frame[0] =
		ctrl_fh->skip_mode_frame[0] - V4L2_AV1_REF_LAST_FRAME;
	uh->skip_mode.skip_mode_frame[1] =
		ctrl_fh->skip_mode_frame[1] - V4L2_AV1_REF_LAST_FRAME;

	vdec_av1_slice_setup_seg(&uh->seg, &ctrl_fh->segmentation);
	uh->delta_q_lf.delta_q_present =
		BIT_FLAG(&ctrl_fh->quantization, V4L2_AV1_QUANTIZATION_FLAG_DELTA_Q_PRESENT);
	uh->delta_q_lf.delta_q_res = 1 << ctrl_fh->quantization.delta_q_res;
	uh->delta_q_lf.delta_lf_present =
		BIT_FLAG(&ctrl_fh->loop_filter, V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_PRESENT);
	uh->delta_q_lf.delta_lf_res = ctrl_fh->loop_filter.delta_lf_res;
	uh->delta_q_lf.delta_lf_multi = ctrl_fh->loop_filter.delta_lf_multi;
	vdec_av1_slice_setup_quant(&uh->quant, &ctrl_fh->quantization);

	uh->coded_loss_less = 1;
	for (i = 0; i < V4L2_AV1_MAX_SEGMENTS; i++) {
		uh->quant.qindex[i] = vdec_av1_slice_get_qindex(uh, i);
		uh->loss_less_array[i] =
			(uh->quant.qindex[i] == 0 && uh->quant.delta_qydc == 0
			&& uh->quant.delta_quac == 0 && uh->quant.delta_qudc == 0 &&
			uh->quant.delta_qvac == 0 && uh->quant.delta_qvdc == 0);

		if (!uh->loss_less_array[i])
			uh->coded_loss_less = 0;
	}

	vdec_av1_slice_setup_lr(&uh->lr, &ctrl_fh->loop_restoration);
	uh->superres_denom = ctrl_fh->superres_denom;
	vdec_av1_slice_setup_lf(&uh->loop_filter, &ctrl_fh->loop_filter);
	vdec_av1_slice_setup_cdef(&uh->cdef, &ctrl_fh->cdef);
	vdec_av1_slice_setup_tile(frame, &ctrl_fh->tile_info);
}

static int vdec_av1_slice_setup_tile_group(
	struct vdec_av1_slice_instance *instance,
	struct vdec_av1_slice_vsi *vsi)
{
	struct v4l2_ctrl_av1_tile_group *ctrl_tg;
	struct v4l2_ctrl_av1_tile_group_entry *ctrl_tge;
	struct vdec_av1_slice_tile_group *tile_group = &instance->tile_group;
	struct vdec_av1_slice_uncompressed_header *uh= &vsi->frame.uh;
	struct vdec_av1_slice_tile *tile = &uh->tile;
	unsigned int tg_size, tge_size;
	int i;
	struct v4l2_ctrl *ctrl;

	ctrl = v4l2_ctrl_find(&instance->ctx->ctrl_hdl,
		V4L2_CID_STATELESS_AV1_TILE_GROUP);
	if (ctrl == NULL)
		return -EINVAL;

	tg_size = ctrl->elems;
	ctrl_tg = (struct v4l2_ctrl_av1_tile_group*)ctrl->p_cur.p;
			
	ctrl = v4l2_ctrl_find(&instance->ctx->ctrl_hdl,
		V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY);
	if (ctrl == NULL)
		return -EINVAL;

	tge_size = ctrl->elems;
	ctrl_tge = (struct v4l2_ctrl_av1_tile_group_entry*)ctrl->p_cur.p;

	tile_group->num_tiles = tile->tile_cols * tile->tile_rows;

	if ((tile_group->num_tiles != tge_size) ||
		(tile_group->num_tiles > AV1_MAX_TILE_COUNT)) {
		mtk_vcodec_err(instance, 
			"Invalid tge_size %d, tile_num:%d\n",
			tge_size, tile_group->num_tiles);
		return -EINVAL;
	}	

	for (i = 0; i < tile_group->num_tiles ; i++)
		tile_group->last_tile_in_tile_group[i] = 0;

	for (i = 0; i < tg_size; i++) {
		if (ctrl_tg[i].tg_end >= tile_group->num_tiles) {
			mtk_vcodec_err(instance, 
				"Invalid tg_end  %d, larger than tile_num %d\n",
				ctrl_tg[i].tg_end, tile_group->num_tiles);
			return -EINVAL;
		}
		tile_group->last_tile_in_tile_group[ctrl_tg[i].tg_end] = 1;
	}

	for (i = 0; i < tge_size; i++) {
		if (i != ctrl_tge[i].tile_row * vsi->frame.uh.tile.tile_rows +
			ctrl_tge[i].tile_col) {
			mtk_vcodec_err(instance, 
				"Invalid tge info %d, %d %d %d\n",
				i, ctrl_tge[i].tile_row, ctrl_tge[i].tile_col,
				vsi->frame.uh.tile.tile_rows);
			return -EINVAL;

		}		
		tile_group->tile_size[i] = ctrl_tge[i].tile_size;
		tile_group->tile_start_offset[i] = ctrl_tge[i].tile_offset;
	}

	return 0;
}

static void vdec_av1_slice_setup_state(struct vdec_av1_slice_vsi *vsi)
{
	memset(&vsi->state, 0, sizeof(vsi->state));
}


static void vdec_av1_slice_setup_scale_factors(
	struct vdec_av1_slice_frame_refs *frame_ref,
	struct vdec_av1_slice_frame_info *ref_frame_info,
	struct vdec_av1_slice_uncompressed_header *uh)
{
	unsigned int ref_upscaled_width = ref_frame_info->upscaled_width;
	unsigned int ref_frame_height = ref_frame_info->frame_height;
	unsigned int frame_width = uh->frame_width;
	unsigned int frame_height = uh->frame_height;
	struct vdec_av1_slice_scale_factors *scale_factors = &frame_ref->scale_factors;

	if (!valid_ref_frame_size(
		ref_upscaled_width, ref_frame_height, frame_width, frame_height)) {
		scale_factors->x_scale = -1;
		scale_factors->y_scale = -1;
		scale_factors->is_scaled = 0;
		return;
	}

	scale_factors->x_scale =
		((ref_upscaled_width << AV1_REF_SCALE_SHIFT) + (frame_width >> 1)) /
		frame_width;
	scale_factors->y_scale =
		((ref_frame_height << AV1_REF_SCALE_SHIFT) + (frame_height >> 1)) /
		frame_height;
	scale_factors->is_scaled =
		(scale_factors->x_scale != AV1_REF_INVALID_SCALE) &&
		(scale_factors->y_scale != AV1_REF_INVALID_SCALE) &&
		(scale_factors->x_scale != AV1_REF_NO_SCALE ||
		scale_factors->y_scale != AV1_REF_NO_SCALE);
	scale_factors->x_step =
		ROUND_POWER_OF_TWO(scale_factors->x_scale,
		AV1_REF_SCALE_SHIFT - AV1_SCALE_SUBPEL_BITS);
	scale_factors->y_step =
		ROUND_POWER_OF_TWO(scale_factors->y_scale,
		AV1_REF_SCALE_SHIFT - AV1_SCALE_SUBPEL_BITS);
}

static int vdec_av1_slice_get_relative_dist(
	int a,
	int b,
	unsigned char enable_order_hint,
	unsigned char order_hint_bits)
{
	int diff = 0;
	int m = 0;

	if (!enable_order_hint)
		return 0;
	diff = a -b;
	m = 1 << (order_hint_bits - 1);
	diff = (diff & (m - 1)) - (diff & m);
	return diff;
}


static void vdec_av1_slice_setup_ref(
	struct vdec_av1_slice_pfc *pfc,
	struct v4l2_ctrl_av1_frame_header *ctrl_fh)
{
	struct vdec_av1_slice_vsi *vsi = &pfc->vsi;
	struct vdec_av1_slice_frame *frame = &vsi->frame;
	struct vdec_av1_slice_slot *slots = &vsi->slots;
	struct vdec_av1_slice_uncompressed_header *uh = &frame->uh;
	struct vdec_av1_slice_seq_header *seq = &frame->seq;
	int i, j;

	if (uh->frame_is_intra)
		return;

	for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
		frame->order_hints[i] = 0;
		frame->ref_frame_valid[i] = 0;
		pfc->ref_idx[i] = ctrl_fh->reference_frame_ts[i];

		for (j = 0; j < AV1_MAX_FRAME_BUF_COUNT; j++) {
			if (slots->timestamp[j] == ctrl_fh->reference_frame_ts[i]) {
				frame->frame_refs[i].ref_fb_idx = j;
				vdec_av1_slice_setup_scale_factors(&frame->frame_refs[i],
					&slots->frame_info[j], uh);
				if (!seq->enable_order_hint)
					frame->ref_frame_sign_bias[i + 1] = 0;
				else
					frame->ref_frame_sign_bias[i + 1] =
						vdec_av1_slice_get_relative_dist(
						slots->frame_info[j].order_hint, uh->order_hint,
						seq->enable_order_hint, seq->order_hint_bits) <= 0 ?
						0 : 1;

				frame->order_hints[i] = ctrl_fh->order_hints[i + 1];
				frame->ref_frame_valid[i] = 1;
				break;
			}
		}
		if (j == AV1_MAX_FRAME_BUF_COUNT)
			mtk_v4l2_err("cannot match reference[%d] 0x%lx\n",
				i, ctrl_fh->reference_frame_ts[i]);
	}
}

static void vdec_av1_slice_get_previous(struct vdec_av1_slice_vsi *vsi)
{
	struct vdec_av1_slice_frame *frame = &vsi->frame;
	if (frame->uh.primary_ref_frame == 7)
		frame->prev_fb_idx = AV1_INVALID_IDX;
	else
		frame->prev_fb_idx = frame->frame_refs[frame->uh.primary_ref_frame].ref_fb_idx;
}

static void vdec_av1_slice_setup_operating_mode(
	struct vdec_av1_slice_instance *instance,
	struct vdec_av1_slice_frame *frame)
{
	enum v4l2_stateless_av1_operating_mode *operating_mode;
	operating_mode = (enum v4l2_stateless_av1_operating_mode *)
			vdec_av1_get_ctrl_ptr(instance->ctx,
			V4L2_CID_STATELESS_AV1_OPERATING_MODE);
	frame->large_scale_tile =
		(*operating_mode == V4L2_STATELESS_AV1_OPERATING_MODE_LARGE_SCALE_TILE_DECODING);
}
static int vdec_av1_slice_setup_pfc(
	struct vdec_av1_slice_instance *instance,
	struct vdec_av1_slice_pfc *pfc)
{
	struct v4l2_ctrl_av1_frame_header *ctrl_fh;
	struct v4l2_ctrl_av1_sequence *ctrl_seq;
	struct vdec_av1_slice_vsi *vsi;

	/* frame header */
	ctrl_fh = (struct v4l2_ctrl_av1_frame_header *)
		vdec_av1_get_ctrl_ptr(instance->ctx,
		V4L2_CID_STATELESS_AV1_FRAME_HEADER);
	ctrl_seq = (struct v4l2_ctrl_av1_sequence *)
		vdec_av1_get_ctrl_ptr(instance->ctx,
		V4L2_CID_STATELESS_AV1_SEQUENCE);

	vsi = &pfc->vsi;

	/* setup vsi information */
	vdec_av1_slice_setup_seq(&vsi->frame.seq, ctrl_seq);
	vdec_av1_slice_setup_uh(instance, &vsi->frame, ctrl_fh);
	vdec_av1_slice_setup_operating_mode(instance, &vsi->frame);
	vdec_av1_slice_setup_state(vsi);
	vdec_av1_slice_setup_slot(instance, vsi);
	vdec_av1_slice_setup_ref(pfc, ctrl_fh);
	vdec_av1_slice_get_previous(vsi);

	pfc->seq = instance->seq;
	instance->seq++;

	return 0;
}

static int vdec_av1_slice_setup_lat_buffer(
	struct vdec_av1_slice_instance *instance,
	struct vdec_av1_slice_vsi *vsi,
	struct mtk_vcodec_mem *bs,
	struct vdec_lat_buf *lat_buf)
{
	struct vdec_av1_slice_work_buffer *work_buffer;
	int i;

	vsi->bs.dma_addr = bs->dma_addr;
	vsi->bs.size = bs->size;

	vsi->ube.dma_addr = lat_buf->ctx->msg_queue.wdma_addr.dma_addr;
	vsi->ube.size = lat_buf->ctx->msg_queue.wdma_addr.size;
	vsi->trans.dma_addr = lat_buf->ctx->msg_queue.wdma_wptr_addr;
	/* used to store trans end */
	vsi->trans.dma_addr_end = lat_buf->ctx->msg_queue.wdma_rptr_addr;
	vsi->err_map.dma_addr = lat_buf->wdma_err_addr.dma_addr;
	vsi->err_map.size = lat_buf->wdma_err_addr.size;
	vsi->rd_mv.dma_addr = lat_buf->rd_mv_addr.dma_addr;
	vsi->rd_mv.size = lat_buf->rd_mv_addr.size;

	vsi->row_info.buf = 0;
	vsi->row_info.size = 0;

	work_buffer = vsi->work_buffer;

	for (i = 0; i < AV1_MAX_FRAME_BUF_COUNT; i++) {
		work_buffer[i].mv_addr.buf = instance->mv[i].dma_addr;
		work_buffer[i].mv_addr.size = instance->mv[i].size;
		work_buffer[i].segid_addr.buf = instance->seg[i].dma_addr;
		work_buffer[i].segid_addr.size = instance->seg[i].size;
		work_buffer[i].cdf_addr.buf = instance->cdf[i].dma_addr;
		work_buffer[i].cdf_addr.size = instance->cdf[i].size;
	}

	vsi->cdf_tmp.buf = instance->cdf_temp.dma_addr;
	vsi->cdf_tmp.size = instance->cdf_temp.size;

	vsi->tile.buf = instance->tile.dma_addr;
	vsi->tile.size= instance->tile.size;
	memcpy_fromio(lat_buf->tile_addr.va, instance->tile.va,
		64 * instance->tile_group.num_tiles);

	vsi->cdf_table.buf = instance->cdf_table.dma_addr;
	vsi->cdf_table.size = instance->cdf_table.size;
	vsi->iq_table.buf = instance->iq_table.dma_addr;
	vsi->iq_table.size = instance->iq_table.size;

	return 0;
}



static void vdec_av1_slice_setup_seg_buffer(
	struct vdec_av1_slice_instance *instance,
	struct vdec_av1_slice_vsi *vsi)
{
	struct vdec_av1_slice_uncompressed_header *uh;
	struct mtk_vcodec_mem *buf;

	/* reset segment buffer */
	uh = &vsi->frame.uh;
	if ((uh->primary_ref_frame == 7) ||
		!uh->seg.segmentation_enabled) {
		mtk_vcodec_debug(instance, "reset seg %d\n", vsi->slot_id);
		if (vsi->slot_id != AV1_INVALID_IDX) {
	  		buf = &instance->seg[vsi->slot_id];
	  		memset(buf->va, 0, buf->size);
		}
	}
}

static int vdec_av1_slice_setup_tile_buffer(
	struct vdec_av1_slice_instance *instance,
	struct vdec_av1_slice_vsi * vsi,
	struct mtk_vcodec_mem *bs)
{
	struct vdec_av1_slice_tile_group *tile_group = &instance->tile_group;
	struct vdec_av1_slice_uncompressed_header *uh= &vsi->frame.uh;
	struct vdec_av1_slice_tile *tile = &uh->tile;
	int tile_row, tile_col;
	int tile_num = 0;
	unsigned int allow_update_cdf = 0;
	unsigned int sb_boundary_x_m1 = 0, sb_boundary_y_m1 = 0;
	int tile_info_base = 0;
	unsigned int tile_buf_pa = 0;
	unsigned int *tile_info_buf = (unsigned int *)instance->tile.va;
	unsigned int pa = (unsigned int)bs->dma_addr;

	if (uh->disable_cdf_update == 0)
			allow_update_cdf = 1;	

	for (tile_num = 0; tile_num < tile_group->num_tiles; tile_num++) {
		// each uint32 takes place of 4 bytes
		tile_info_base = (AV1_TILE_BUF_SIZE * tile_num) >> 2;
		tile_row = tile_num / tile->tile_cols;
		tile_col = tile_num % tile->tile_cols;
		tile_info_buf[tile_info_base + 0] = (tile_group->tile_size[tile_num] << 3);
		tile_buf_pa = pa + tile_group->tile_start_offset[tile_num];

		tile_info_buf[tile_info_base + 1] = (tile_buf_pa >> 4) << 4;
		tile_info_buf[tile_info_base + 2] = (tile_buf_pa % 16) << 3;

		sb_boundary_x_m1 =
			(tile->mi_col_starts[tile_col + 1] - tile->mi_col_starts[tile_col] - 1) & 0x3F;
		sb_boundary_y_m1 =
			(tile->mi_row_starts[tile_row + 1] - tile->mi_row_starts[tile_row] - 1) & 0x1FF;

		tile_info_buf[tile_info_base + 3] = (sb_boundary_y_m1 << 7) | sb_boundary_x_m1;
		tile_info_buf[tile_info_base + 4] =
			((allow_update_cdf << 18) | (tile_group->last_tile_in_tile_group[tile_num] << 16));

		if ((tile_num == tile->context_update_tile_id) &&
			(uh->disable_frame_end_update_cdf == 0))
			tile_info_buf[tile_info_base + 4] |= (1 << 17);

		mtk_vcodec_debug(instance, "// tile buf %d pos(%dx%d) offset 0x%x\n",
			tile_num, tile_row, tile_col, tile_info_base);
		mtk_vcodec_debug(instance, "// %08x %08x %08x %08x\n",
			tile_info_buf[tile_info_base + 0],
			tile_info_buf[tile_info_base + 1],
			tile_info_buf[tile_info_base + 2],
			tile_info_buf[tile_info_base + 3]);
		mtk_vcodec_debug(instance, "// %08x %08x %08x %08x\n",
			tile_info_buf[tile_info_base + 4],
			tile_info_buf[tile_info_base + 5],
			tile_info_buf[tile_info_base + 6],
			tile_info_buf[tile_info_base + 7]);
	}

	return 0;
}

static int vdec_av1_slice_setup_lat(
	struct vdec_av1_slice_instance *instance,
	struct mtk_vcodec_mem *bs,
	struct vdec_lat_buf *lat_buf,
	struct vdec_av1_slice_pfc *pfc)
{
	struct vdec_av1_slice_vsi *vsi = &pfc->vsi;
	int ret;

	ret = vdec_av1_slice_setup_lat_from_src_buf(instance, vsi, lat_buf);
	if (ret)
		goto err;

	ret = vdec_av1_slice_setup_pfc(instance, pfc);
	if (ret)
		goto err;
	vdec_av1_slice_setup_tile_group(instance, vsi);

	ret = vdec_av1_slice_alloc_working_buffer(instance, vsi);
	if (ret)
		goto err;

	vdec_av1_slice_setup_seg_buffer(instance, vsi);

	ret = vdec_av1_slice_setup_tile_buffer(instance, vsi, bs);
	if (ret)
		goto err;
	
	ret = vdec_av1_slice_setup_lat_buffer(instance, vsi, bs, lat_buf);
		if (ret)
			goto err;

	return 0;

err:
	return ret;
}

static int vdec_av1_slice_update_lat(
	struct vdec_av1_slice_instance *instance,
	struct vdec_lat_buf *lat_buf,
	struct vdec_av1_slice_pfc *pfc)
{
	struct vdec_av1_slice_vsi *vsi;

	vsi = &pfc->vsi;
	mtk_vcodec_debug(instance,
		"Frame %u LAT CRC 0x%08x, output size is %d\n",
		pfc->seq, vsi->state.crc[0],vsi->state.out_size);

	/* buffer full, need to re-decode */
	if (vsi->state.full) {
		/* buffer not enough */
		if (vsi->trans.dma_addr_end - vsi->trans.dma_addr ==
			vsi->ube.size)
			return -ENOMEM;
		return -EAGAIN;
	}

	instance->width = vsi->frame.uh.upscaled_width;
	instance->height = vsi->frame.uh.frame_height;
	instance->frame_type = vsi->frame.uh.frame_type;

	return 0;
}

static int vdec_av1_slice_setup_core_to_dst_buf(
	struct vdec_av1_slice_instance *instance,
	struct vdec_lat_buf *lat_buf)
{
	struct vb2_v4l2_buffer *src;
	struct vb2_v4l2_buffer *dst;

	dst = v4l2_m2m_next_dst_buf(instance->ctx->m2m_ctx);
	if (!dst)
		return -EINVAL;

	src = &lat_buf->ts_info;
	dst->vb2_buf.timestamp = src->vb2_buf.timestamp;
	dst->timecode = src->timecode;
	dst->field = src->field;
	dst->flags = src->flags;
	dst->vb2_buf.copied_timestamp = src->vb2_buf.copied_timestamp;
	return 0;
}

static int vdec_av1_slice_setup_core_buffer(
	struct vdec_av1_slice_instance *instance,
	struct vdec_av1_slice_pfc *pfc,
	struct vdec_av1_slice_vsi *vsi,
	struct vdec_fb *fb,
	struct vdec_lat_buf *lat_buf)
{
	struct vb2_buffer *vb;
	struct vb2_queue *vq;
	int plane;
	int size;
	int idx;
	int w;
	int h;
	int i;

	plane = instance->ctx->q_data[MTK_Q_DATA_DST].fmt->num_planes;
	w = vsi->frame.uh.upscaled_width;
	h = vsi->frame.uh.frame_height;
	size = ALIGN(w, 64) * ALIGN(h, 64);

	/* frame buffer */
	vsi->fb.y.dma_addr = fb->base_y.dma_addr;
	if (plane == 1)
		vsi->fb.c.dma_addr = fb->base_y.dma_addr + size;
	else
		vsi->fb.c.dma_addr = fb->base_c.dma_addr;

	/* reference buffers */
	vq = v4l2_m2m_get_vq(instance->ctx->m2m_ctx,
		V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (!vq)
		return -EINVAL;

	/* get current output buffer */
	vb = &v4l2_m2m_next_dst_buf(instance->ctx->m2m_ctx)->vb2_buf;
	if (!vb)
		return -EINVAL;

	/*
	 * get buffer address from vb2buf
	 */
	for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
		idx = vb2_find_timestamp(vq, pfc->ref_idx[i], 0);
		if (idx < 0) {
			memset(&vsi->ref[i], 0, sizeof(vsi->ref[i]));
		} else {
			vb = vq->bufs[idx];
			vsi->ref[i].y.dma_addr =
				vb2_dma_contig_plane_dma_addr(vb, 0);
			if (plane == 1)
				vsi->ref[i].c.dma_addr =
					vsi->ref[i].y.dma_addr + size;
			else
				vsi->ref[i].c.dma_addr =
					vb2_dma_contig_plane_dma_addr(vb, 1);
		}
	}

	vsi->tile.dma_addr = lat_buf->tile_addr.dma_addr;
	vsi->tile.size = lat_buf->tile_addr.size;

	return 0;
}

static int vdec_av1_slice_setup_core(
	struct vdec_av1_slice_instance *instance,
	struct vdec_fb *fb,
	struct vdec_lat_buf *lat_buf,
	struct vdec_av1_slice_pfc *pfc)
{
	struct vdec_av1_slice_vsi *vsi = &pfc->vsi;
	int ret;

	ret = vdec_av1_slice_setup_core_to_dst_buf(instance, lat_buf);
	if (ret)
		goto err;

	ret = vdec_av1_slice_setup_core_buffer(instance, pfc, vsi,
		fb, lat_buf);
	if (ret)
		goto err;

	return 0;

err:
	return ret;
}

static int vdec_av1_slice_update_core(
	struct vdec_av1_slice_instance *instance,
	struct vdec_lat_buf *lat_buf,
	struct vdec_av1_slice_pfc *pfc)
{
	struct vdec_av1_slice_vsi *vsi;

	vsi = instance->core_vsi;
	mtk_vcodec_debug(instance, "Frame %u Y_CRC %08x %08x %08x %08x\n",
		pfc->seq,
		vsi->state.crc[0], vsi->state.crc[1],
		vsi->state.crc[2], vsi->state.crc[3]);
	mtk_vcodec_debug(instance, "Frame %u C_CRC %08x %08x %08x %08x\n",
		pfc->seq,
		vsi->state.crc[8], vsi->state.crc[9],
		vsi->state.crc[10], vsi->state.crc[11]);

	return 0;
}

static int vdec_av1_slice_init(struct mtk_vcodec_ctx *ctx)
{
	struct vdec_av1_slice_instance *instance;
	struct vdec_av1_slice_init_vsi *vsi;
	int i, ret;

	instance = kzalloc(sizeof(*instance), GFP_KERNEL);
	if (!instance)
		return -ENOMEM;

	instance->ctx = ctx;
	instance->vpu.id = SCP_IPI_VDEC_LAT;
	instance->vpu.core_id = SCP_IPI_VDEC_CORE;
	instance->vpu.ctx = ctx;
	instance->vpu.codec_type = ctx->current_codec;

	ret = vpu_dec_init(&instance->vpu);
	if (ret) {
		mtk_vcodec_err(instance, "failed to init vpu dec, ret %d\n",
			ret);
		goto error_vpu_init;
	}

	/* init vsi and global flags */

	vsi = instance->vpu.vsi;
	if (!vsi) {
		mtk_vcodec_err(instance, "failed to get AV1 vsi\n");
		ret = -EINVAL;
		goto error_vsi;
	}
	instance->init_vsi = vsi;
	instance->core_vsi = mtk_vcodec_fw_map_dm_addr(ctx->dev->fw_handler,
		(u32)vsi->core_vsi);
	if (!instance->core_vsi) {
		mtk_vcodec_err(instance, "failed to get AV1 core vsi\n");
		ret = -EINVAL;
		goto error_vsi;
	}

	if (vsi->vsi_size != sizeof(struct vdec_av1_slice_vsi))
		mtk_vcodec_err(instance, "kernel vsi size 0x%x is not equal out 0x%x\n",
			vsi->vsi_size, sizeof(struct vdec_av1_slice_vsi));

	instance->irq = 1;
	instance->inneracing_mode = 
		IS_VDEC_INNER_RACING(instance->ctx->dev->dec_capability);

	mtk_vcodec_debug(instance,
		"vsi 0x%px core_vsi 0x%llx 0x%px, inneracing_mode %d\n",
		vsi, vsi->core_vsi, instance->core_vsi, instance->inneracing_mode);

	ret = vdec_av1_slice_init_cdf_table(instance);
	if (ret)
		goto error_vsi;

	ret = vdec_av1_slice_init_iq_table(instance);
	if (ret)
		goto error_vsi;

	ctx->drv_handle = instance;
	for (i = 0; i < AV1_TOTAL_REFS_PER_FRAME; i++)
		instance->slots.ref_frame_map[i] = AV1_INVALID_IDX;

	return 0;
error_vsi:
	vpu_dec_deinit(&instance->vpu);
error_vpu_init:
	kfree(instance);
	return ret;
}

static void vdec_av1_slice_deinit(void *h_vdec)
{
	struct vdec_av1_slice_instance *instance = h_vdec;

	if (!instance)
		return;
	mtk_vcodec_debug(instance, "h_vdec 0x%px\n", h_vdec);
	vpu_dec_deinit(&instance->vpu);
	vdec_av1_slice_free_working_buffer(instance);
	vdec_msg_queue_deinit(&instance->ctx->msg_queue, instance->ctx);
	kfree(instance);
}

static int vdec_av1_slice_flush(void *h_vdec, struct mtk_vcodec_mem *bs,
	struct vdec_fb *fb, bool *res_chg)
{
	struct vdec_av1_slice_instance *instance = h_vdec;
	int i;

	mtk_vcodec_debug(instance, "flush ...\n");
	for (i = 0; i < AV1_TOTAL_REFS_PER_FRAME; i++)
			instance->slots.ref_frame_map[i] = AV1_INVALID_IDX;

	vdec_msg_queue_wait_lat_buf_full(&instance->ctx->msg_queue);
	return vpu_dec_reset(&instance->vpu);
}

static void vdec_av1_slice_get_pic_info(
	struct vdec_av1_slice_instance *instance)
{
	struct mtk_vcodec_ctx *ctx = instance->ctx;
	unsigned int data[3];

	mtk_vcodec_debug(instance, "w %u h %u\n",
		ctx->picinfo.pic_w, ctx->picinfo.pic_h);

	data[0] = ctx->picinfo.pic_w;
	data[1] = ctx->picinfo.pic_h;
	data[2] = ctx->capture_fourcc;
	vpu_dec_get_param(&instance->vpu, data, 3, GET_PARAM_PIC_INFO);

	ctx->picinfo.buf_w = ALIGN(ctx->picinfo.pic_w, 64);
	ctx->picinfo.buf_h = ALIGN(ctx->picinfo.pic_h, 64);
	ctx->picinfo.fb_sz[0] = instance->vpu.fb_sz[0];
	ctx->picinfo.fb_sz[1] = instance->vpu.fb_sz[1];
}

static void vdec_av1_slice_get_dpb_size(
	struct vdec_av1_slice_instance *instance,
	unsigned int *dpb_sz)
{
	/* refer av1 specification */
	*dpb_sz = 9;
}

static void vdec_av1_slice_get_crop_info(
	struct vdec_av1_slice_instance *instance,
	struct v4l2_rect *cr)
{
	struct mtk_vcodec_ctx *ctx = instance->ctx;

	cr->left = 0;
	cr->top = 0;
	cr->width = ctx->picinfo.pic_w;
	cr->height = ctx->picinfo.pic_h;

	mtk_vcodec_debug(instance, "l=%d, t=%d, w=%d, h=%d\n",
		cr->left, cr->top, cr->width, cr->height);
}

static int vdec_av1_slice_get_param(void *h_vdec,
	enum vdec_get_param_type type, void *out)
{
	struct vdec_av1_slice_instance *instance = h_vdec;

	switch (type) {
	case GET_PARAM_PIC_INFO:
		vdec_av1_slice_get_pic_info(instance);
		break;
	case GET_PARAM_DPB_SIZE:
		vdec_av1_slice_get_dpb_size(instance, out);
		break;
	case GET_PARAM_CROP_INFO:
		vdec_av1_slice_get_crop_info(instance, out);
		break;
	default:
		mtk_vcodec_err(instance, "invalid get parameter type=%d\n",
			type);
		return -EINVAL;
	}

	return 0;
}

static int vdec_av1_slice_lat_decode(void *h_vdec,
	struct mtk_vcodec_mem *bs,
	struct vdec_fb *fb,
	bool *res_chg)
{
	struct vdec_av1_slice_instance *instance = h_vdec;
	struct vdec_lat_buf *lat_buf;
	struct vdec_av1_slice_pfc *pfc;
	struct vdec_av1_slice_vsi *vsi;
	struct mtk_vcodec_ctx *ctx;
	int ret;

	if (!instance || !instance->ctx)
		return -EINVAL;
	ctx = instance->ctx;

	/* init msgQ for the first time */
	if (vdec_msg_queue_init(&ctx->msg_queue, ctx,
		vdec_av1_slice_core_decode, sizeof(*pfc))) {
		mtk_vcodec_err(instance,
			"Failed to init AV1 msg queue\n");
		return -ENOMEM;
	}

	/* bs NULL means flush decoder */
	if (!bs)
		return vdec_av1_slice_flush(h_vdec, bs, fb, res_chg);

	lat_buf = vdec_msg_queue_dqbuf(&ctx->msg_queue.lat_ctx);
	if (!lat_buf) {
		mtk_vcodec_err(instance, "Failed to get AV1 lat buf\n");
		return -EBUSY;
	}
	pfc = (struct vdec_av1_slice_pfc *)lat_buf->private_data;
	if (!pfc)
		return -EINVAL;
	vsi = &pfc->vsi;

	ret = vdec_av1_slice_setup_lat(instance, bs, lat_buf, pfc);
	if (ret) {
		mtk_vcodec_err(instance,
			"Failed to setup AV1 lat ret %d\n", ret);
		return ret;
	}

	vdec_av1_slice_vsi_to_remote(vsi, instance->vsi);
	
	ret = vpu_dec_start(&instance->vpu, 0, 0);
	if (ret) {
		mtk_vcodec_err(instance,
			"Failed to dec AV1 ret %d\n", ret);
		return ret;
	}
	if (instance->inneracing_mode)
		vdec_msg_queue_qbuf(&ctx->dev->msg_queue_core_ctx, lat_buf);

	if (instance->irq) {
		ret = mtk_vcodec_wait_for_done_ctx(ctx, MTK_INST_IRQ_RECEIVED,
						   WAIT_INTR_TIMEOUT_MS,
						   MTK_VDEC_LAT0);
		/* update remote vsi if decode timeout */
		if (ret) {
			mtk_vcodec_err(instance,
				"AV1 decode timeout %d\n", ret);
			writel(1, &instance->vsi->state.timeout);
		}
		vpu_dec_end(&instance->vpu);
	}
	
	vdec_av1_slice_vsi_from_remote(vsi, instance->vsi, 0);
	ret = vdec_av1_slice_update_lat(instance, lat_buf, pfc);

	/* LAT trans full, re-decode */
	if (ret == -EAGAIN) {
		mtk_vcodec_err(instance, "AV1 trans full\n");
		vdec_msg_queue_qbuf(&ctx->dev->msg_queue_core_ctx, lat_buf);
		return 0;
	}

	/*
	 * LAT trans full, no more UBE
	 * decode timeout
	 */
	if (ret == -ENOMEM || vsi->state.timeout) {
		mtk_vcodec_err(instance,
			"AV1 insufficient buffer or timeout\n");
		vdec_msg_queue_qbuf(&ctx->dev->msg_queue_core_ctx, lat_buf);
		return -EBUSY;
	}
	vsi->trans.dma_addr_end += ctx->msg_queue.wdma_addr.dma_addr;
	mtk_vcodec_debug(instance, "lat dma 1 0x%lx 0x%lx\n",
		pfc->vsi.trans.dma_addr, pfc->vsi.trans.dma_addr_end);

	vdec_msg_queue_update_ube_wptr(&ctx->msg_queue,
		vsi->trans.dma_addr_end);
	if (!instance->inneracing_mode)
		vdec_msg_queue_qbuf(&ctx->dev->msg_queue_core_ctx, lat_buf);
	memcpy_fromio(&instance->slots, &vsi->slots, sizeof(instance->slots));
	vdec_av1_slice_update_ref_slot(vsi, &instance->slots);

	return 0;
}

static int vdec_av1_slice_core_decode(
	struct vdec_lat_buf *lat_buf)
{
	struct vdec_av1_slice_instance *instance;
	struct vdec_av1_slice_pfc *pfc;
	struct mtk_vcodec_ctx *ctx = NULL;
	struct vdec_fb *fb = NULL;
	int ret = -EINVAL;

	if (!lat_buf)
		return -EINVAL;

	pfc = lat_buf->private_data;
	ctx = lat_buf->ctx;
	if (!pfc || !ctx)
		return -EINVAL;

	instance = ctx->drv_handle;
	if (!instance)
		goto err;

	fb = ctx->dev->vdec_pdata->get_cap_buffer(ctx);
	if (!fb) {
		ret = -EBUSY;
		goto err;
	}

	ret = vdec_av1_slice_setup_core(instance, fb, lat_buf, pfc);
	if (ret) {
		mtk_vcodec_err(instance, "vdec_av1_slice_setup_core\n");
		goto err;
	}
	vdec_av1_slice_vsi_to_remote(&pfc->vsi, instance->core_vsi);
	ret = vpu_dec_core(&instance->vpu);
	if (ret) {
		mtk_vcodec_err(instance, "vpu_dec_core\n");
		goto err;
	}

	if (instance->irq) {
		ret = mtk_vcodec_wait_for_done_ctx(ctx, MTK_INST_IRQ_RECEIVED,
						   WAIT_INTR_TIMEOUT_MS,
						   MTK_VDEC_CORE);
		/* update remote vsi if decode timeout */
		if (ret) {
			mtk_vcodec_err(instance, "AV1 core timeout\n");
			writel(1, &instance->core_vsi->state.timeout);
		}
		vpu_dec_core_end(&instance->vpu);
	}

//	vdec_av1_slice_vsi_from_remote(&pfc->vsi, instance->core_vsi, 1);
	ret = vdec_av1_slice_update_core(instance, lat_buf, pfc);
	if (ret) {
		mtk_vcodec_err(instance, "vdec_av1_slice_update_core\n");
		goto err;
	}

	mtk_vcodec_debug(instance, "core dma_addr_end 0x%lx\n",
		instance->core_vsi->trans.dma_addr_end);
	vdec_msg_queue_update_ube_rptr(&ctx->msg_queue,
		instance->core_vsi->trans.dma_addr_end);
	ctx->dev->vdec_pdata->cap_to_disp(ctx, fb, 0);

	return 0;

err:
	/* always update read pointer */
	vdec_msg_queue_update_ube_rptr(&ctx->msg_queue,
		pfc->vsi.trans.dma_addr_end);

	if (fb)
		ctx->dev->vdec_pdata->cap_to_disp(ctx, fb, 1);

	return ret;
}

const struct vdec_common_if vdec_av1_slice_lat_if = {
	.init		= vdec_av1_slice_init,
	.decode		= vdec_av1_slice_lat_decode,
	.get_param	= vdec_av1_slice_get_param,
	.deinit		= vdec_av1_slice_deinit,
};
