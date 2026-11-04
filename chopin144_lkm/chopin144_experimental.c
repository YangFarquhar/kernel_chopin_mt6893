// SPDX-License-Identifier: GPL-2.0
/*
 * Chopin135 Experimental LKM V0.3
 * Target: MT6893 / K10A 42-02-0B DSC VDO
 *
 * Adds a seventh DRM mode and wraps the K10A MTK panel extension callbacks.
 * 135Hz profile (HFP 72, VFP 192, PLL 570, data_rate 1140, DCS selector 0x1F)
 * is experimental and carried forward from the project's V0.2 candidate.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/of.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <drm/drmP.h>
#include <drm/drm_mipi_dsi.h>

#include "../drivers/gpu/drm/mediatek/mi_disp/mi_dsi_panel.h"

#define MOD_NAME             "chopin135_experimental"
#define TARGET_COMPAT        "k10a_42_02_0b_dsc_vdo,lcm"
#define TARGET_MODE_INDEX    6
#define MODE_135_FPS         135
#define MODE_135_HFP         72
#define MODE_135_VFP         192
#define MODE_135_PLL         570
#define MODE_135_DATA_RATE   1140
#define MODE_135_CLOCK       412565
#define MODE_135_SELECTOR    0x1F

/*
 * Keep a non-empty __versions section for the 4.14 loader's MODVERSIONS
 * path. The build workflow replaces this section with this one blank entry.
 * On this kernel branch, a missing name in an existing __versions section
 * warns and continues; an absent section is rejected when force-load is off.
 */
#ifdef CONFIG_MODVERSIONS
static const struct modversion_info chopin135_empty_versions[]
	__used __section(__versions) = {
	{ .crc = 0, .name = "" },
};
#endif

typedef int (*ddp_io_cmd_fn)(struct mtk_ddp_comp *comp, struct cmdq_pkt *handle,
			     enum mtk_ddp_io_cmd cmd, void *params);
typedef int (*of_compatible_fn)(const struct device_node *node,
				const char *compatible);
typedef struct drm_display_mode *(*mode_duplicate_fn)(
	struct drm_device *dev, const struct drm_display_mode *mode);
typedef void (*mode_set_name_fn)(struct drm_display_mode *mode);
typedef void (*mode_probed_add_fn)(struct drm_connector *connector,
				   struct drm_display_mode *mode);
typedef void (*mode_destroy_fn)(struct drm_device *dev,
				struct drm_display_mode *mode);
typedef void (*hotplug_event_fn)(struct drm_device *dev);
typedef ssize_t (*dcs_write_buffer_fn)(struct mipi_dsi_device *dsi,
				       const void *data, size_t len);
typedef struct mtk_panel_ext *(*find_panel_ext_fn)(struct drm_panel *panel);

static DEFINE_MUTEX(chopin135_lock);
static struct work_struct probe_work;
static struct timer_list probe_timer;
static struct mtk_dsi *target_dsi;
static struct mtk_panel_ext *target_ext;
static struct mtk_panel_funcs *original_funcs;
static struct mtk_panel_funcs hooked_funcs;
static struct mtk_ddp_comp_funcs hooked_comp_funcs;
static const struct mtk_ddp_comp_funcs *original_comp_funcs;
static ddp_io_cmd_fn original_io_cmd;
static struct mtk_panel_params params_135hz;
static struct drm_display_mode *mode_135hz;
static bool installed;
static unsigned int probe_attempts;

static of_compatible_fn of_compatible;
static mode_duplicate_fn mode_duplicate;
static mode_set_name_fn mode_set_name;
static mode_probed_add_fn mode_probed_add;
static mode_destroy_fn mode_destroy;
static hotplug_event_fn hotplug_event;
static dcs_write_buffer_fn dcs_write_buffer;

static const struct drm_display_mode mode_135_template = {
	.clock = MODE_135_CLOCK,
	.hdisplay = 1080,
	.hsync_start = 1080 + MODE_135_HFP,
	.hsync_end = 1080 + MODE_135_HFP + 10,
	.htotal = 1080 + MODE_135_HFP + 10 + 8,
	.vdisplay = 2400,
	.vsync_start = 2400 + MODE_135_VFP,
	.vsync_end = 2400 + MODE_135_VFP + 2,
	.vtotal = 2400 + MODE_135_VFP + 2 + 18,
	.vrefresh = MODE_135_FPS,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static unsigned long resolve_symbol(const char *name)
{
	return kallsyms_lookup_name(name);
}

static struct mtk_dsi *get_target_dsi(void)
{
	struct mtk_ddp_comp **output_slot;
	struct mtk_ddp_comp *output;

	output_slot = (struct mtk_ddp_comp **)
		resolve_symbol("g_output_comp");
	if (!output_slot)
		return NULL;

	output = READ_ONCE(*output_slot);
	if (!output)
		return NULL;

	return container_of(output, struct mtk_dsi, ddp_comp);
}

static bool is_target_dsi(struct mtk_dsi *dsi)
{
	if (!dsi || !dsi->panel || !dsi->panel->dev ||
	    !dsi->panel->dev->of_node || !of_compatible)
		return false;

	return of_compatible(dsi->panel->dev->of_node, TARGET_COMPAT) != 0;
}

static struct drm_display_mode *mode_at_index(struct mtk_dsi *dsi,
					       unsigned int index)
{
	struct drm_display_mode *mode;
	unsigned int i = 0;

	if (!dsi || !dsi->conn.dev)
		return NULL;

	list_for_each_entry(mode, &dsi->conn.modes, head) {
		if (i++ == index)
			return mode;
	}
	return NULL;
}

static unsigned int current_refresh(struct mtk_dsi *dsi)
{
	if (dsi && dsi->encoder.crtc && dsi->encoder.crtc->state)
		return dsi->encoder.crtc->state->adjusted_mode.vrefresh;
	return 120;
}

static unsigned int refresh_to_index(unsigned int fps)
{
	switch (fps) {
	case 60: return 0;
	case 90: return 1;
	case 120: return 2;
	case 50: return 3;
	case 48: return 4;
	case 30: return 5;
	default: return 2;
	}
}


static void refresh_crtc_mode_cache(struct mtk_dsi *dsi)
{
	struct mtk_ddp_comp *comp;

	if (!dsi)
		return;

	comp = &dsi->ddp_comp;
	if (comp->mtk_crtc && comp->funcs && comp->funcs->io_cmd) {
		/*
		 * The stock DSI ioctl copies connector->modes into
		 * mtk_crtc->avail_modes, which is what DISP_MODE_IDX uses.
		 */
		if (comp->mtk_crtc->avail_modes)
			vfree(comp->mtk_crtc->avail_modes);
		comp->mtk_crtc->avail_modes = NULL;
		comp->mtk_crtc->avail_modes_num = 0;
		comp->funcs->io_cmd(comp, NULL, DSI_SET_CRTC_AVAIL_MODES,
				    comp->mtk_crtc);
	}
}

static int add_135hz_mode(struct mtk_dsi *dsi)
{
	struct drm_display_mode *mode;
	struct drm_display_mode *iter;

	if (!dsi || !dsi->panel || !dsi->panel->connector ||
	    !dsi->panel->drm || !mode_duplicate || !mode_probed_add ||
	    !mode_set_name)
		return -EINVAL;

	mutex_lock(&dsi->conn.dev->mode_config.mutex);
	list_for_each_entry(iter, &dsi->conn.modes, head) {
		if (iter->vrefresh == MODE_135_FPS) {
			mode_135hz = iter;
			mutex_unlock(&dsi->conn.dev->mode_config.mutex);
			return 0;
		}
	}

	mode = mode_duplicate(dsi->panel->drm, &mode_135_template);
	if (!mode) {
		mutex_unlock(&dsi->conn.dev->mode_config.mutex);
		return -ENOMEM;
	}
	mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER;
	mode_probed_add(dsi->panel->connector, mode);
	mode_135hz = mode;
	mutex_unlock(&dsi->conn.dev->mode_config.mutex);

	refresh_crtc_mode_cache(dsi);
	if (hotplug_event)
		hotplug_event(dsi->conn.dev);
	pr_info(MOD_NAME ": appended DRM mode %s @%u, clock=%u kHz, totals=%ux%u\n",
		mode->name, mode->vrefresh, mode->clock,
		mode->htotal, mode->vtotal);
	return 0;
}

static int switch_panel_dcs_to_135(struct drm_panel *panel)
{
	struct mipi_dsi_device *dsi;
	static const u8 page[] = { 0xFF, 0x25 };
	static const u8 unlock[] = { 0xFB, 0x01 };
	static const u8 selector[] = { 0x18, MODE_135_SELECTOR };
	ssize_t ret;

	if (!panel || !panel->dev || !dcs_write_buffer)
		return -ENODEV;

	dsi = to_mipi_dsi_device(panel->dev);
	ret = dcs_write_buffer(dsi, page, sizeof(page));
	if (ret < 0)
		return (int)ret;
	ret = dcs_write_buffer(dsi, unlock, sizeof(unlock));
	if (ret < 0)
		return (int)ret;
	ret = dcs_write_buffer(dsi, selector, sizeof(selector));
	return ret < 0 ? (int)ret : 0;
}

static int chopin135_ext_param_set(struct drm_panel *panel,
				   unsigned int mode)
{
	struct mtk_dsi *dsi = target_dsi;
	struct drm_display_mode *target_mode;
	int ret;

	if (!dsi || !target_ext || !original_funcs ||
	    !original_funcs->ext_param_set)
		return -ENODEV;

	if (mode == TARGET_MODE_INDEX) {
		target_ext->params = &params_135hz;
		dsi->vm.hfront_porch = MODE_135_HFP;
		dsi->vm.vfront_porch = MODE_135_VFP;

		pr_info(MOD_NAME ": selected 135Hz profile, pll=%u data_rate=%u HFP=%u VFP=%u selector=0x%02X\n",
			params_135hz.pll_clk, params_135hz.data_rate,
			MODE_135_HFP, MODE_135_VFP, MODE_135_SELECTOR);
		return 0;
	}

	/*
	 * The target profile is changed here. The DSI_TIMING_CHANGE wrapper
	 * performs the matching PLL/porch transition after DYN_FPS_INDEX has
	 * calculated the target mode.
	 */
	if (target_ext->params == &params_135hz && dsi->mipi_hopping_sta) {
		target_mode = mode_at_index(dsi, mode);
		if (target_mode) {
			dsi->vm.hfront_porch =
				target_mode->hsync_start - target_mode->hdisplay;
			dsi->vm.vfront_porch =
				target_mode->vsync_start - target_mode->vdisplay;
		}
	}

	ret = original_funcs->ext_param_set(panel, mode);
	return ret;
}

static int chopin135_mode_switch(struct drm_panel *panel,
				 unsigned int cur_mode,
				 unsigned int dst_mode,
				 enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	if (dst_mode == TARGET_MODE_INDEX) {
		/* Match the panel driver's AFTER_DSI_POWERON recovery stage. */
		if (stage == AFTER_DSI_POWERON)
			return switch_panel_dcs_to_135(panel);
		return 0;
	}

	if (original_funcs && original_funcs->mode_switch)
		return original_funcs->mode_switch(panel, cur_mode,
						   dst_mode, stage);
	return 1;
}


/*
 * The panel callback is invoked once during DYN_FPS_INDEX calculation and
 * again from mtk_dsi_timing_change(). Do not change the PLL in ext_param_set:
 * that is too early in the atomic commit. Intercept the DSI_TIMING_CHANGE
 * command instead, after the CRTC has selected the target mode and profile.
 */
static int chopin135_io_cmd(struct mtk_ddp_comp *comp,
			    struct cmdq_pkt *handle,
			    enum mtk_ddp_io_cmd cmd, void *params)
{
	struct mtk_dsi *dsi;
	struct mtk_panel_ext *ext;
	struct mtk_panel_params restore_params;
	struct mtk_panel_params *saved_params;
	struct drm_crtc_state *current_state;
	int hopping;
	bool use_dynamic;

	if (cmd == DSI_TIMING_CHANGE && comp) {
		dsi = container_of(comp, struct mtk_dsi, ddp_comp);
		ext = dsi->ext;
		use_dynamic = ext && ext->params &&
			ext->params->dyn.switch_en == 1;

		if (ext && ext->params &&
		    dsi->mipi_hopping_sta != use_dynamic) {
			if (!use_dynamic) {
				current_state = comp->mtk_crtc ?
					comp->mtk_crtc->base.state : NULL;
				if (current_state) {
					dsi->vm.hfront_porch =
						current_state->adjusted_mode.hsync_start -
						current_state->adjusted_mode.hdisplay;
					dsi->vm.vfront_porch =
						current_state->adjusted_mode.vsync_start -
						current_state->adjusted_mode.vdisplay;
				}

				/*
				 * The stock 4.14 mtk_dsi_clk_change() early-returns
				 * when dyn.switch_en is zero. Present a temporary copy
				 * with the same stock timing values but switch_en=1 so
				 * the function executes its normal-rate restore path.
				 */
				saved_params = ext->params;
				memcpy(&restore_params, saved_params,
				       sizeof(restore_params));
				restore_params.dyn.switch_en = 1;
				ext->params = &restore_params;
				hopping = 0;
				if (comp->mtk_crtc && original_io_cmd) {
					comp->mtk_crtc->mipi_hopping_sta = 0;
					original_io_cmd(comp, NULL, MIPI_HOPPING, &hopping);
				}
				ext->params = saved_params;
			} else if (comp->mtk_crtc && original_io_cmd) {
				hopping = 1;
				comp->mtk_crtc->mipi_hopping_sta = 1;
				original_io_cmd(comp, NULL, MIPI_HOPPING, &hopping);
			}
			pr_info(MOD_NAME ": DSI profile transition dynamic=%u rate=%u hopping=%u\n",
				use_dynamic,
				use_dynamic ? ext->params->dyn.data_rate :
				ext->params->data_rate,
				dsi->mipi_hopping_sta);
		}
	}

	if (!original_io_cmd)
		return -ENOSYS;
	return original_io_cmd(comp, handle, cmd, params);
}

static int install_hooks(struct mtk_dsi *dsi)
{
	struct drm_panel *panel;
	struct mtk_panel_params *saved_params;
	unsigned int restore_index;
	int ret;

	if (!is_target_dsi(dsi))
		return -ENODEV;
	if (!dsi->ddp_comp.funcs || !dsi->ddp_comp.funcs->io_cmd)
		return -ENODEV;

	panel = dsi->panel;
	target_ext = dsi->ext;
	if (!target_ext) {
		find_panel_ext_fn find_ext =
			(find_panel_ext_fn)resolve_symbol("find_panel_ext");
		if (find_ext)
			target_ext = find_ext(panel);
	}
	if (!target_ext || !target_ext->funcs ||
	    !target_ext->funcs->ext_param_set)
		return -ENODEV;

	if (installed)
		return 0;

	original_funcs = target_ext->funcs;
	saved_params = target_ext->params;
	restore_index = refresh_to_index(current_refresh(dsi));

	/* Select the genuine stock 120Hz block as the template. */
	ret = original_funcs->ext_param_set(panel, 2);
	if (ret || !target_ext->params) {
		target_ext->params = saved_params;
		return ret ? ret : -EINVAL;
	}

	memcpy(&params_135hz, target_ext->params, sizeof(params_135hz));
	params_135hz.pll_clk = MODE_135_PLL;
	params_135hz.data_rate = MODE_135_DATA_RATE;
	params_135hz.dyn.switch_en = 1;
	params_135hz.dyn.pll_clk = MODE_135_PLL;
	params_135hz.dyn.data_rate = MODE_135_DATA_RATE;
	params_135hz.dyn.hfp = MODE_135_HFP;
	params_135hz.dyn.vfp = MODE_135_VFP;
	params_135hz.dyn_fps.switch_en = 1;
	params_135hz.dyn_fps.vact_timing_fps = MODE_135_FPS;
	params_135hz.dyn_fps.data_rate = MODE_135_DATA_RATE;
	/* Stock table has FF 25 / FB 01 / 18 20 in entries 0..2. */
	params_135hz.dyn_fps.dfps_cmd_table[2].para_list[1] =
		MODE_135_SELECTOR;

	/* Restore the currently selected stock profile and current_fps. */
	original_funcs->ext_param_set(panel, restore_index);
	target_ext->params = saved_params;

	ret = add_135hz_mode(dsi);
	if (ret)
		return ret;

	memcpy(&hooked_funcs, original_funcs, sizeof(hooked_funcs));
	hooked_funcs.ext_param_set = chopin135_ext_param_set;
	hooked_funcs.mode_switch = chopin135_mode_switch;
	target_ext->funcs = &hooked_funcs;

	original_comp_funcs = dsi->ddp_comp.funcs;
	memcpy(&hooked_comp_funcs, original_comp_funcs,
	       sizeof(hooked_comp_funcs));
	original_io_cmd = original_comp_funcs->io_cmd;
	hooked_comp_funcs.io_cmd = chopin135_io_cmd;
	dsi->ddp_comp.funcs = &hooked_comp_funcs;

	target_dsi = dsi;
	dsi->ext = target_ext;
	installed = true;

	pr_info(MOD_NAME ": hooks installed on %s; 135Hz mode is available\n",
		TARGET_COMPAT);
	return 0;
}

static void probe_work_fn(struct work_struct *work)
{
	struct mtk_dsi *dsi;
	int ret;

	mutex_lock(&chopin135_lock);
	if (installed) {
		mutex_unlock(&chopin135_lock);
		return;
	}

	dsi = get_target_dsi();
	if (!dsi || !dsi->panel || !dsi->conn.dev) {
		probe_attempts++;
		mutex_unlock(&chopin135_lock);
		if (probe_attempts < 60)
			mod_timer(&probe_timer, jiffies + HZ);
		else
			pr_err(MOD_NAME ": target DSI/panel was not found after 60 attempts\n");
		return;
	}

	target_dsi = dsi;
	ret = install_hooks(dsi);
	if (ret) {
		probe_attempts++;
		mutex_unlock(&chopin135_lock);
		pr_err(MOD_NAME ": install attempt %u failed: %d\n",
			probe_attempts, ret);
		if (probe_attempts < 60)
			mod_timer(&probe_timer, jiffies + HZ);
		return;
	}

	mutex_unlock(&chopin135_lock);
}


static void chopin135_timer_fn(unsigned long data)
{
	schedule_work(&probe_work);
}

static int __init chopin135_init(void)
{
	of_compatible = (of_compatible_fn)
		resolve_symbol("of_device_is_compatible");
	mode_duplicate = (mode_duplicate_fn)
		resolve_symbol("drm_mode_duplicate");
	mode_set_name = (mode_set_name_fn)
		resolve_symbol("drm_mode_set_name");
	mode_probed_add = (mode_probed_add_fn)
		resolve_symbol("drm_mode_probed_add");
	mode_destroy = (mode_destroy_fn)
		resolve_symbol("drm_mode_destroy");
	hotplug_event = (hotplug_event_fn)
		resolve_symbol("drm_kms_helper_hotplug_event");
	dcs_write_buffer = (dcs_write_buffer_fn)
		resolve_symbol("mipi_dsi_dcs_write_buffer");

	if (!of_compatible || !mode_duplicate || !mode_set_name ||
	    !mode_probed_add || !dcs_write_buffer || !hotplug_event) {
		pr_err(MOD_NAME ": missing runtime symbols: of_compatible=%d duplicate=%d set_name=%d probed_add=%d dcs_write=%d hotplug=%d\\n",
		       !!of_compatible, !!mode_duplicate, !!mode_set_name,
		       !!mode_probed_add, !!dcs_write_buffer, !!hotplug_event);
		return -ENOSYS;
	}

	INIT_WORK(&probe_work, probe_work_fn);
	setup_timer(&probe_timer, chopin135_timer_fn, 0);
	probe_attempts = 0;
	mod_timer(&probe_timer, jiffies + 5 * HZ);
	pr_info(MOD_NAME ": loaded; waiting for K10A panel binding\n");
	return 0;
}

static void __exit chopin135_exit(void)
{
	struct drm_device *drm = NULL;

	del_timer_sync(&probe_timer);
	cancel_work_sync(&probe_work);
	mutex_lock(&chopin135_lock);

	if (installed && target_dsi && target_ext) {
		if (target_ext->params == &params_135hz &&
		    target_dsi->mipi_hopping_sta && original_io_cmd) {
			struct mtk_panel_params restore_params;
			struct mtk_panel_params *stock_params;
			struct drm_display_mode *mode120 =
				mode_at_index(target_dsi, 2);
			int hopping = 0;

			if (mode120) {
				target_dsi->vm.hfront_porch =
					mode120->hsync_start - mode120->hdisplay;
				target_dsi->vm.vfront_porch =
					mode120->vsync_start - mode120->vdisplay;
			}

			if (original_funcs && original_funcs->ext_param_set)
				original_funcs->ext_param_set(target_dsi->panel, 2);
			stock_params = target_ext->params;
			if (stock_params) {
				memcpy(&restore_params, stock_params,
				       sizeof(restore_params));
				restore_params.dyn.switch_en = 1;
				target_ext->params = &restore_params;
				if (target_dsi->ddp_comp.mtk_crtc) {
					target_dsi->ddp_comp.mtk_crtc->mipi_hopping_sta = 0;
					original_io_cmd(&target_dsi->ddp_comp, NULL,
							MIPI_HOPPING, &hopping);
				}
				target_ext->params = stock_params;
			}
		}

		target_ext->funcs = original_funcs;
		if (original_comp_funcs)
			target_dsi->ddp_comp.funcs = original_comp_funcs;
		drm = target_dsi->conn.dev;

		if (mode_135hz && target_dsi->panel &&
		    target_dsi->panel->connector && drm) {
			mutex_lock(&drm->mode_config.mutex);
			list_del_init(&mode_135hz->head);
			mutex_unlock(&drm->mode_config.mutex);
			if (mode_destroy)
				mode_destroy(drm, mode_135hz);
			mode_135hz = NULL;
			refresh_crtc_mode_cache(target_dsi);
			if (hotplug_event)
				hotplug_event(drm);
		}
	}
	installed = false;
	target_ext = NULL;
	target_dsi = NULL;
	original_funcs = NULL;
	original_comp_funcs = NULL;
	original_io_cmd = NULL;
	mutex_unlock(&chopin135_lock);
	pr_info(MOD_NAME ": unloaded\n");
}

module_init(chopin135_init);
module_exit(chopin135_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chopin135 experimental project");
MODULE_DESCRIPTION("Experimental 135Hz DRM/MTK DSI mode for POCO X3 GT K10A panel");
MODULE_VERSION("0.3");
