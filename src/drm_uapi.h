/* Hand-declared subset of the Linux DRM UAPI (the cross container has no drm headers).
 * Layouts match <drm/drm.h> / <drm/drm_mode.h>; sizes are asserted below so a mismatch
 * fails the build instead of the ioctl. Ioctl numbers: _IO*('d', nr) with 'd' = 0x64.
 * Part of mpc-sample-tracker, MIT. */
#ifndef MPC_DRM_UAPI_H
#define MPC_DRM_UAPI_H
#include <stdint.h>
#include <assert.h>

struct mpc_drm_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};
struct mpc_drm_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct mpc_drm_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};
struct mpc_drm_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct mpc_drm_modeinfo mode;
};
struct mpc_drm_create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct mpc_drm_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct mpc_drm_destroy_dumb { uint32_t handle; };
struct mpc_drm_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};
struct mpc_drm_clip_rect { uint16_t x1, y1, x2, y2; };
struct mpc_drm_fb_dirty_cmd { uint32_t fb_id, flags, color, num_clips; uint64_t clips_ptr; };

static_assert(sizeof(struct mpc_drm_modeinfo) == 68, "modeinfo");
static_assert(sizeof(struct mpc_drm_card_res) == 64, "card_res");
static_assert(sizeof(struct mpc_drm_get_connector) == 80, "get_connector");
static_assert(sizeof(struct mpc_drm_crtc) == 104, "crtc");
static_assert(sizeof(struct mpc_drm_create_dumb) == 32, "create_dumb");
static_assert(sizeof(struct mpc_drm_map_dumb) == 16, "map_dumb");
static_assert(sizeof(struct mpc_drm_fb_cmd2) == 104, "fb_cmd2");
static_assert(sizeof(struct mpc_drm_fb_dirty_cmd) == 24, "fb_dirty_cmd");

#define MPC_DRM_IOCTL_SET_MASTER        0x641EUL
#define MPC_DRM_IOCTL_DROP_MASTER       0x641FUL
#define MPC_DRM_IOCTL_MODE_GETRESOURCES 0xC04064A0UL
#define MPC_DRM_IOCTL_MODE_SETCRTC      0xC06864A2UL
#define MPC_DRM_IOCTL_MODE_GETCONNECTOR 0xC05064A7UL
#define MPC_DRM_IOCTL_MODE_RMFB         0xC00464AFUL
#define MPC_DRM_IOCTL_MODE_DIRTYFB      0xC01864B1UL
#define MPC_DRM_IOCTL_MODE_CREATE_DUMB  0xC02064B2UL
#define MPC_DRM_IOCTL_MODE_MAP_DUMB     0xC01064B3UL
#define MPC_DRM_IOCTL_MODE_DESTROY_DUMB 0xC00464B4UL
#define MPC_DRM_IOCTL_MODE_ADDFB2       0xC06864B8UL
#define MPC_DRM_FORMAT_XRGB8888         0x34325258U   /* 'XR24' */

#endif
