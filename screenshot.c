#include "rl.h"
#include <WinUser.h>

void free_screenshot_rasters(raster_t **r, int *r_count)
{
	int i;

	// Free each captured screenshot before freeing the array
	if (r && *r && r_count)
		for (i=0; i < *r_count; i++)
			free_raster(&(*r)[i]);

	// Clear the screenshot array and count
	if (r)
		free_null(r);
	if (r_count)
		*r_count = 0;
}

void take_and_process_screenshot(raster_t **r, int *r_count, mipmap_t *mm)
{
	// Release the previous capture before taking a new one
	free_screenshot_rasters(r, r_count);
	free_mipmap(mm);

	// Store the newly captured desktop rasters and their count
	*r = take_desktop_screenshot(r_count);
}

xy_t sel_coord_to_pix_coord(xy_t sel_coord, rect_t im_rect, xyi_t dim)
{
	xy_t p, im_dim = xyi_to_xy(dim);

	im_rect = sort_rect(im_rect);
	p = sub_xy(sel_coord, rect_p01(im_rect));
	p = div_xy(p, neg_y(get_rect_dim(im_rect)));
	p = mul_xy(p, sub_xy(xyi_to_xy(dim), set_xy(1.)));

	return p;
}

xy_t set_pix_coord_to_coord(xy_t pix_coord, rect_t im_rect, xyi_t dim)
{
	xy_t p, im_dim = xyi_to_xy(dim);

	// Convert a pixel coordinate back to the selection coordinate space
	im_rect = sort_rect(im_rect);
	p = div_xy(pix_coord, sub_xy(im_dim, set_xy(1.)));
	p = mul_xy(p, neg_y(get_rect_dim(im_rect)));
	p = add_xy(p, rect_p01(im_rect));

	return p;
}

void set_crop_knob_limits(gui_layout_t *layout, xyi_t dim)
{
	// Match crop knobs to the edited image dimensions
	get_knob_data_fromlayout(layout, 70)->max = dim.y-1;
	get_knob_data_fromlayout(layout, 73)->max = dim.y-1;
	get_knob_data_fromlayout(layout, 71)->max = dim.x-1;
	get_knob_data_fromlayout(layout, 72)->max = dim.x-1;
	get_knob_data_fromlayout(layout, 76)->default_value = dim.x;
	get_knob_data_fromlayout(layout, 76)->max = dim.x;
	get_knob_data_fromlayout(layout, 77)->default_value = dim.y;
	get_knob_data_fromlayout(layout, 77)->max = dim.y;
}

typedef struct
{
	int hide_flag, exit_flag, shot_flag, raise_flag, hotkey_diag_on;
	raster_t *r, rc;
	mipmap_t mm;
	int knob_ret, crop_recalc, preview, r_count, image_recalc, image_id_moving, image_reset_crop, filename_recalc;
	double image_id, crop_width, crop_height;
	double save_fail_time;
	char datestamp[32];
	ctrl_resize_rect_t resize_state;
	rect_t resize_box, crop_rect, im_rect, prev_rect;
	recti_t crop_recti;
	uint32_t hotkey_mod, hotkey_vk;
	int apply_status;
	double apply_time;
} edit_data_t;

recti_t get_limited_crop_recti(edit_data_t *d, xyi_t dim)
{
	// Round and clamp the crop rectangle to the edited image bounds
	return recti_boolean_intersection(rect_to_recti_round(d->crop_rect), recti(XYI0, sub_xyi(dim, set_xyi(1))));
}

void set_resize_box_from_crop_rect(edit_data_t *d, raster_t *r)
{
	xy_t p0, p1;

	// Convert the crop rectangle corners back to selection coordinates
	p0 = set_pix_coord_to_coord(d->crop_rect.p0, d->im_rect, r->dim);
	p1 = set_pix_coord_to_coord(d->crop_rect.p1, d->im_rect, r->dim);
	d->resize_box.p0.x = p0.x;
	d->resize_box.p1.y = p0.y;
	d->resize_box.p1.x = p1.x;
	d->resize_box.p0.y = p1.y;
}

raster_t *get_selected_screenshot(edit_data_t *d)
{
	int id;

	// Return no raster when no screenshot capture is loaded
	if (d->r == NULL || d->r_count <= 0)
		return NULL;

	// Convert the one-based image ID knob to a zero-based raster index
	id = rangelimit_i32((int) nearbyint(d->image_id) - 1, 0, d->r_count - 1);

	return &d->r[id];
}

void prepare_selected_screenshot(edit_data_t *d, gui_layout_t *layout, int reset_crop)
{
	raster_t *r;

	// Resolve the active screenshot raster from the selected image ID
	r = get_selected_screenshot(d);
	if (r == NULL)
		return;

	// Rebuild the display mipmap for the selected screenshot
	free_mipmap(&d->mm);
	convert_image_srgb8_fullarg(r, (uint8_t *) r->srgb, IMAGE_USE_SQRGB, 0);
	d->mm = raster_to_tiled_mipmaps_fast_defaults(*r, IMAGE_USE_SQRGB);
	free_null(&r->sq);

	// Update crop knob limits to the selected screenshot dimensions
	if (layout)
		set_crop_knob_limits(layout, r->dim);

	if (reset_crop)
	{
		// Reset the crop state to the full selected screenshot
		d->resize_box = d->im_rect;
		d->crop_rect = rect(XY0, xyi_to_xy(sub_xyi(r->dim, set_xyi(1))));
		d->crop_recti = recti(XYI0, sub_xyi(r->dim, set_xyi(1)));
	}
	d->crop_recalc = 1;

	// Keep a cropped-preview copy of the selected screenshot
	free_raster(&d->rc);
	d->rc = copy_raster(*r);
}

typedef struct
{
	uint32_t id;
	char name[13];
} vk_name_t;

vk_name_t vk_name[] = {
	{0x2C, "Print Screen"},		// IDs from https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
	{0x70, "F1"},
	{0x71, "F2"},
	{0x72, "F3"},
	{0x73, "F4"},
	{0x74, "F5"},
	{0x75, "F6"},
	{0x76, "F7"},
	{0x77, "F8"},
	{0x78, "F9"},
	{0x79, "F10"},
	{0x7A, "F11"},
	{0x7B, "F12"},
};

#define VK_COUNT (sizeof(vk_name)/sizeof(*vk_name))

void knob_print_vk(char *str, knob_t *knob_data, double value)
{
	int id = nearbyint(value);

	if (isnan(value) || id < 0 || id >= VK_COUNT)
		sprintf(str, "\357\277\275");
	else
		sprintf(str, "%s", vk_name[id].name);
}

double knob_parse_vk(const char *str, knob_t *knob_data)
{
	for (int i=0; i < VK_COUNT; i++)
		if (strcmp(str, vk_name[i].name) == 0)
			return i;

	return -1;
}

void hotkey_dialog(edit_data_t *d)
{
	static int init=1;
	int i, ret;
	static double vk_v=NAN;

	// GUI layout
	static gui_layout_t layout={0};
	static const char *layout_src[] = {
		"elem 0", "type none", "label Set hotkey", "pos	0	0", "dim	3;8	4;5", "off	0	1", "",
		"elem 10", "type checkbox", "label Ctrl", "pos	0;6	-1;2", "dim	1;4	0;5", "off	0	1", "",
		"elem 11", "type checkbox", "label Win", "link_pos_id 10.lb", "pos	0	-0;1", "dim	1;4	0;5", "off	0	1", "",
		"elem 12", "type checkbox", "label Alt", "link_pos_id 11.lb", "pos	0	-0;1", "dim	1;4	0;5", "off	0	1", "",
		"elem 13", "type checkbox", "label Shift", "link_pos_id 12.lb", "pos	0	-0;1", "dim	1;4	0;5", "off	0	1", "",
		"elem 20", "type label", "label Modifier keys", "link_pos_id 10.lt", "pos	0;1	0;1", "dim	1;2	0;3", "off	0", "",
		"elem 21", "type label", "label Key", "link_pos_id 20.r_", "pos	0;6	0", "dim	1;2	0;3", "off	0", "",
		"elem 30", "type knob", "label Key name", "knob -1 0 1 linear", "link_pos_id 21.lb", "pos	-0;1	-0;1", "dim	1", "off	0	1", "",
		"elem 40", "type button", "label Apply & save", "link_pos_id 13.lb", "pos	0	-0;4", "dim	2;8	0;6", "off	0	1", "",
	};

	make_gui_layout(&layout, layout_src, sizeof(layout_src)/sizeof(char *), "Set hotkey");

	// GUI window
	static flwindow_t window={0};
	flwindow_init_defaults(&window);
	window.bg_opacity = 0.94;
	window.shadow_strength = 0.5*window.bg_opacity;
	window.parent_fit_offset = xy(0.5, 0.5);
	draw_dialog_window_fromlayout(&window, cur_wind_on, &cur_parent_area, &layout, 0);

	if (init)
	{
		init = 0;

		// Set the VK knob
		knob_t *knob_data = get_knob_data_fromlayout(&layout, 30);
		knob_data->max = VK_COUNT - 1;
		knob_data->display_print_func = knob_print_vk;
		knob_data->editor_print_func = knob_print_vk;
		knob_data->parse_func = knob_parse_vk;

		vk_v = -1.;
		for (i=0; i < VK_COUNT; i++)
			if (vk_name[i].id == d->hotkey_vk)
				vk_v = i;
	}

	// Checkbox prep
	int state_ctrl  = (d->hotkey_mod & 0x0002) != 0;
	int state_win   = (d->hotkey_mod & 0x0008) != 0;
	int state_alt   = (d->hotkey_mod & 0x0001) != 0;
	int state_shift = (d->hotkey_mod & 0x0004) != 0;

	// Checkboxes
	draw_label_fromlayout(&layout, 20, ALIG_LEFT | MONODIGITS);
	ctrl_checkbox_fromlayout(&state_ctrl,  &layout, 10);
	ctrl_checkbox_fromlayout(&state_win,   &layout, 11);
	ctrl_checkbox_fromlayout(&state_alt,   &layout, 12);
	ctrl_checkbox_fromlayout(&state_shift, &layout, 13);

	// Checkbox statuses back to packed flags
	d->hotkey_mod = 0x4000;	// MOD_NOREPEAT
	if (state_ctrl)  d->hotkey_mod |= 0x0002;
	if (state_win)   d->hotkey_mod |= 0x0008;
	if (state_alt)   d->hotkey_mod |= 0x0001;
	if (state_shift) d->hotkey_mod |= 0x0004;

	// Key
	draw_label_fromlayout(&layout, 21, ALIG_LEFT | MONODIGITS);
	if (ctrl_knob_fromlayout(&vk_v, &layout, 30))
	{
		int id = nearbyint(vk_v);
		if (id >= 0)
			d->hotkey_vk = vk_name[id].id;
	}

	// Apply-save button
	col_t col = gui_col_def;
	if (d->apply_status == 1)
		col = make_colour(0., 0.4, 0., 0.);
	if (d->apply_status == -1)
		col = make_colour(1., 0., 0., 0.);
	col = mix_colours(col, gui_col_def, 1.-gaussian(get_time_hr()-d->apply_time));
	gui_set_control_colour(col, &layout, 40);

	if (ctrl_button_fromlayout(&layout, 40))
	{
		d->apply_time = get_time_hr();

		UnregisterHotKey(NULL, 1);
		if (RegisterHotKey(NULL, 1, d->hotkey_mod, d->hotkey_vk) == 0)
		{
			d->apply_status = -1;
		}
		else
		{
			d->apply_status = 1;
		}

		pref_set_double(&pref_def, "Hotkey:Modifiers", d->hotkey_mod, NULL);
		pref_set_double(&pref_def, "Hotkey:Key", d->hotkey_vk, NULL);
	}
}

void screenshot_editor_dialog(edit_data_t *d)
{
	static int init=1;
	int i, ret;

	// GUI layout
	static gui_layout_t layout={0};
	static const char *layout_src[] = {
		"elem 0", "type none", "label Options", "pos	0	0;3", "dim	3	9;2", "off	0	1", "",
		"elem 10", "type button", "label Reset sel", "pos	1;6	-3;2", "dim	1;1	0;4", "off	1", "",
		"elem 11", "type button", "label Reuse prev sel", "link_pos_id 10.lb", "pos	0	-0;2", "dim	1;1	0;3", "off	0	1", "",
		"elem 20", "type button", "label Save screenshot", "link_pos_id 60", "pos	0	-0;8", "dim	2	0;7", "off	0;6	1", "",
		"elem 30", "type button", "label Hide window", "link_pos_id 20", "pos	0	-0;10", "dim	2	0;7", "off	0;6	1", "",
		"elem 40", "type button", "label Exit", "link_pos_id 30", "pos	0	-0;10", "dim	2	0;7", "off	0;6	1", "",
		"elem 50", "type textedit", "link_pos_id 11.rb", "pos	0	-0;6", "dim	2	0;5", "off	0;6	1", "",
		"elem 51", "type label", "label Folder", "link_pos_id 50", "pos	-0;11;6	0;0;6", "dim	1	0;3;6", "off	0", "",
		"elem 52", "type button", "label Open \360\237\223\201", "link_pos_id 50", "pos	1	-0;6", "dim	0;9	0;3;6", "off	1", "",
		"elem 60", "type textedit", "link_pos_id 50", "pos	0	-1;1", "dim	2	0;5", "off	0;6	1", "",
		"elem 61", "type label", "label Filename", "link_pos_id 60", "pos	-0;11;6	0;0;6", "dim	1;5;6	0;3;6", "off	0", "",
		"elem 65", "type knob", "label Image ID", "knob 1 1 2 linear %.0f", "pos	2;4	-0;6", "dim	0;8", "off	0;6	1", "",
		"elem 70", "type knob", "label Top", "knob 0 0 1079 linear %.0f", "pos	1;6	-1", "dim	0;8", "off	0;6	1", "",
		"elem 71", "type knob", "label Left", "knob 0 0 1919 linear %.0f", "link_pos_id 70.lc", "pos	-0;2	-0;5", "dim	0;8", "off	1	0;6", "",
		"elem 72", "type knob", "label Right", "knob 0 0 1919 linear %.0f", "link_pos_id 70.rc", "pos	0;2	-0;5", "dim	0;8", "off	0	0;6", "",
		"elem 73", "type knob", "label Bottom", "knob 0 0 1079 linear %.0f", "link_pos_id 70.cb", "pos	0	-0;2", "dim	0;8", "off	0;6	1", "",
		"elem 75", "type label", "label Selection", "link_pos_id 70", "pos	0	0;3;6", "dim	2	0;2;6", "off	0;6	1", "",
		"elem 76", "type knob", "label Width", "knob 1 1920 1920 linear %.0f", "link_pos_id 71.cb", "pos	0	-0;2", "dim	0;8", "off	0;6	1", "",
		"elem 77", "type knob", "label Height", "knob 1 1080 1080 linear %.0f", "link_pos_id 72.cb", "pos	0	-0;2", "dim	0;8", "off	0;6	1", "",
		"elem 80", "type checkbox", "label Preview", "link_pos_id 10.rc", "pos	0;2	0", "dim	1	0;3;6", "off	0	0;6", "",
		"elem 90", "type checkbox", "label Set hotkey", "link_pos_id 11.rc", "pos	0;2	0", "dim	1	0;3", "off	0	0;6", "",
	};

	gui_layout_init_pos_scale(&layout, xy(zc.limit_u.x-1.5, 7.5), 1.4, xy(-3., 0.), 0);
	make_gui_layout(&layout, layout_src, sizeof(layout_src)/sizeof(char *), "Screenshot editor");

	if (d->filename_recalc)
	{
		d->filename_recalc = 0;

		// Update the generated filename
		print_to_layout_textedit(&layout, 60, 0, "%s.png", d->datestamp);			// generate filename
	}

	if (init)
	{
		init = 0;

		// Set saved folder path
		char *default_folder = win_get_system_folder_path(0x0027 /*CSIDL_MYPICTURES*/);
		print_to_layout_textedit(&layout, 50, 1, "%s", pref_get_string(&pref_def, "Folder location", default_folder));
		free(default_folder);
	}

	// GUI window
	static flwindow_t window={0};
	flwindow_init_defaults(&window);
	flwindow_init_pinned(&window);
	window.bg_opacity = 0.94;
	window.shadow_strength = 0.5*window.bg_opacity;
	draw_dialog_window_fromlayout(&window, NULL, NULL, &layout, 0);	// this handles and displays the window that contains the control

	// Save failure colour
	col_t save_fail_col = mix_colours(make_colour(1., 0., 0., 0.), gui_col_def, 1.-gaussian(get_time_hr()-d->save_fail_time));
	gui_set_control_colour(save_fail_col, &layout, 60);	// path textedit
	gui_set_control_colour(save_fail_col, &layout, 20);	// Save button

	// Match the image selector range to the current capture count
	get_knob_data_fromlayout(&layout, 65)->max = d->r_count > 0 ? d->r_count : 1;

	if (d->rc.srgb)
		set_crop_knob_limits(&layout, d->rc.dim);

	// GUI controls
	if (ctrl_button_fromlayout(&layout, 10))		// Reset selection
	{
		d->resize_box = d->im_rect;
		d->crop_recalc = 1;
	}

	if (ctrl_button_fromlayout(&layout, 11) && is0_rect(d->prev_rect) == 0)		// Reuse previous selection
	{
		d->resize_box = d->prev_rect;
		d->crop_recalc = 1;
	}

	ctrl_checkbox_fromlayout(&d->preview, &layout, 80);	// Preview

	if (ctrl_button_fromlayout(&layout, 30))		// Hide window
		d->hide_flag = 1;

	if (ctrl_button_fromlayout(&layout, 40))		// Exit
		d->exit_flag = 1;

	if (ctrl_button_fromlayout(&layout, 52))		// Open folder
		system_open(get_textedit_string_fromlayout(&layout, 50));

	ctrl_checkbox_fromlayout(&d->hotkey_diag_on, &layout, 90);		// Hotkey dialog toggle	

	// Image ID knob
	int image_id_ret = ctrl_knob_fromlayout(&d->image_id, &layout, 65);
	if (image_id_ret == 1)
	{
		d->image_recalc = 1;
		d->image_id_moving = 0;
	}
	else
		d->image_id_moving = image_id_ret == 2;

	// Selection knobs
	int top_ret, left_ret, right_ret, bottom_ret, width_ret, height_ret, crop_knob_ret, size_knob_ret;
	raster_t *r;
	recti_t crop_recti;
	draw_label_fromlayout(&layout, 75, ALIG_LEFT | MONODIGITS);
	top_ret = ctrl_knob_fromlayout(&d->crop_rect.p0.y, &layout, 70);
	left_ret = ctrl_knob_fromlayout(&d->crop_rect.p0.x, &layout, 71);
	right_ret = ctrl_knob_fromlayout(&d->crop_rect.p1.x, &layout, 72);
	bottom_ret = ctrl_knob_fromlayout(&d->crop_rect.p1.y, &layout, 73);
	crop_knob_ret = top_ret | left_ret | right_ret | bottom_ret;
	d->knob_ret |= crop_knob_ret;

	r = get_selected_screenshot(d);
	if (r)
	{
		if (crop_knob_ret)
			set_resize_box_from_crop_rect(d, r);

		// Update the size knobs from the rounded crop edges
		crop_recti = get_limited_crop_recti(d, r->dim);
		d->crop_width = 1 + crop_recti.p1.x - crop_recti.p0.x;
		d->crop_height = 1 + crop_recti.p1.y - crop_recti.p0.y;
	}

	width_ret = ctrl_knob_fromlayout(&d->crop_width, &layout, 76);
	height_ret = ctrl_knob_fromlayout(&d->crop_height, &layout, 77);
	size_knob_ret = width_ret | height_ret;
	d->knob_ret |= size_knob_ret;

	if (r && size_knob_ret)
	{
		crop_recti = get_limited_crop_recti(d, r->dim);

		// Extend the right edge or shift the left edge when the image edge is reached
		if (width_ret)
		{
			int width = rangelimit_i32((int) nearbyint(d->crop_width), 1, r->dim.x);
			crop_recti.p1.x = crop_recti.p0.x + width - 1;
			if (crop_recti.p1.x >= r->dim.x)
			{
				crop_recti.p1.x = r->dim.x - 1;
				crop_recti.p0.x = rangelimit_i32(crop_recti.p1.x - width + 1, 0, crop_recti.p1.x);
			}
			d->crop_width = width;
		}

		// Extend the bottom edge or shift the top edge when the image edge is reached
		if (height_ret)
		{
			int height = rangelimit_i32((int) nearbyint(d->crop_height), 1, r->dim.y);
			crop_recti.p1.y = crop_recti.p0.y + height - 1;
			if (crop_recti.p1.y >= r->dim.y)
			{
				crop_recti.p1.y = r->dim.y - 1;
				crop_recti.p0.y = rangelimit_i32(crop_recti.p1.y - height + 1, 0, crop_recti.p1.y);
			}
			d->crop_height = height;
		}

		// Store the adjusted crop rectangle and matching resize selection
		d->crop_rect = rect(xyi_to_xy(crop_recti.p0), xyi_to_xy(crop_recti.p1));
		set_resize_box_from_crop_rect(d, r);
	}

	// Path setting
	draw_label_fromlayout(&layout, 51, ALIG_LEFT);
	ret = ctrl_textedit_fromlayout(&layout, 50);
	if (ret > 0 && ret < 4)				// Save the path to prefs if changed
		pref_set_string(&pref_def, "Folder location", get_textedit_string_fromlayout(&layout, 50));

	// File name
	ret = ctrl_textedit_fromlayout(&layout, 60);
	draw_label_fromlayout(&layout, 61, ALIG_LEFT);

	// Save screenshot
	if (ctrl_button_fromlayout(&layout, 20) || ret == 1 || (mouse.key_state[RL_SCANCODE_RETURN] == 2 && get_kb_alt() < 0 && cur_textedit == NULL))
	{
		raster_t *r;

		// Resolve the selected image to save from
		r = get_selected_screenshot(d);
		if (r == NULL)
			return;

		// Copy cropped image
		raster_t rs={0};
		rs = make_raster(NULL, sub_xyi(add_xyi(XYI1, d->crop_recti.p1), d->crop_recti.p0), XYI0, IMAGE_USE_SRGB);
		for (i=d->crop_recti.p0.y; i <= d->crop_recti.p1.y; i++)
			memcpy(&rs.srgb[(i-d->crop_recti.p0.y)*rs.dim.x], &r->srgb[i*r->dim.x + d->crop_recti.p0.x], rs.dim.x * sizeof(srgb_t));

		// Save it to file
		char *save_path = append_name_to_path(NULL, get_textedit_string_fromlayout(&layout, 50), get_textedit_string_fromlayout(&layout, 60));
		int save_ret = save_image(save_path, rs, 92);
		free(save_path);
		free_raster(&rs);

		if (save_ret == 0)
			d->save_fail_time = get_time_hr();
		else
			d->hide_flag = 1;
	}
	
	if (d->hide_flag)
	{
		// Reset view
		zoom_reset(&zc, &mouse.zoom_flag);
		gui_layout_edit_toolbar(mouse.key_state[RL_SCANCODE_F6]==2);

		// Save rect for potential later reuse
		if (!equal_rect(d->resize_box, d->im_rect))
			d->prev_rect = d->resize_box;

		// Reset the d->preview checkbox to off
		d->preview = 0;

		// Close hotkey window
		d->hotkey_diag_on = 0;
	}
}

void screenshot_editor(edit_data_t *d)
{
	int i;

	if (d->shot_flag)
	{
		time_t now = time(NULL);

		// Capture the screenshots before queuing any draw work
		take_and_process_screenshot(&d->r, &d->r_count, &d->mm);

		// Queue the selected image refresh and filename update
		d->shot_flag = 0;
		d->image_recalc = 1;
		d->image_reset_crop = 1;
		d->filename_recalc = 1;
		strftime(d->datestamp, sizeof(d->datestamp), "%Y-%m-%d %H.%M.%S", localtime(&now));
	}

	if (d->image_recalc && d->image_id_moving==0)
	{
		// Prepare the selected image before queuing any draw work
		prepare_selected_screenshot(d, NULL, d->image_reset_crop);
		d->image_recalc = 0;
		d->image_reset_crop = 0;
	}

	// Apply the cropping visually
	if (d->image_id_moving==0 && (d->crop_recalc || d->knob_ret))
	{
		raster_t *r;

		// Resolve the selected screenshot for visual editing
		r = get_selected_screenshot(d);
		if (r == NULL)
			return;

		if (d->crop_recalc)
		{
			// Crop recalculation
			d->crop_rect.p0 = sel_coord_to_pix_coord(d->resize_box.p0, d->im_rect, r->dim);
			d->crop_rect.p1 = sel_coord_to_pix_coord(d->resize_box.p1, d->im_rect, r->dim);
			d->crop_rect = sort_rect(d->crop_rect);
		}

		d->crop_recti = rect_to_recti_round(d->crop_rect);
		d->crop_recti = recti_boolean_intersection(d->crop_recti, recti(XYI0, sub_xyi(r->dim, set_xyi(1))));

		memset(d->rc.srgb, 0, mul_x_by_y_xyi(r->dim) * sizeof(srgb_t));
		for (i=d->crop_recti.p0.y; i <= d->crop_recti.p1.y; i++)
			memcpy(&d->rc.srgb[i*d->rc.dim.x + d->crop_recti.p0.x], &r->srgb[i*r->dim.x + d->crop_recti.p0.x], (1+d->crop_recti.p1.x-d->crop_recti.p0.x) * sizeof(srgb_t));

		free_mipmap(&d->mm);
		convert_image_srgb8_fullarg(&d->rc, (uint8_t *) d->rc.srgb, IMAGE_USE_SQRGB, 0);
		d->mm = raster_to_tiled_mipmaps_fast_defaults(d->rc, IMAGE_USE_SQRGB);
		free_null(&d->rc.sq);
		d->crop_recalc = 0;
		d->knob_ret = 0;
	}

	// Draw image
	drawq_bracket_open();
	d->im_rect = blit_mipmap_in_rect(d->mm, sc_rect(make_rect_off(XY0, mul_xy(zc.limit_u, set_xy(2.)), xy(0.5, 0.5))), 1, AA_NEAREST_INTERP);	// the mipmap image is fitted inside a rectangle that represents the default view
	//rect_t d->im_rect = blit_in_rect(r, sc_rect(make_rect_off(XY0, mul_xy(zc.limit_u, set_xy(2.)), xy(0.5, 0.5))), 1, AA_NEAREST_INTERP);	// the mipmap image is fitted inside a rectangle that represents the default view
	if (d->preview==0)
		draw_gain(0.6);
	drawq_bracket_close(DQB_ADD);

	// Selection control
	gui_col_def = make_grey(0.4);
	if (d->preview==0)
		if (ctrl_resizing_rect(&d->resize_state, &d->resize_box))
			d->crop_recalc = 1;
	gui_col_def = make_grey(0.25);

	// Register windows
	window_register(1, screenshot_editor_dialog, NULL, RECTNAN, NULL, 1, d);
	if (d->hotkey_diag_on)
		window_register(1, hotkey_dialog, NULL, rect_size_mul(zc.corners, xy(0.25, 0.75)), &d->hotkey_diag_on, 1, d);
}

int main(int argc, char *argv[])
{
	edit_data_t data={0}, *d = &data;
	int first_loop = 1;
	SDL_Event event;

	// Init program
	pref_def = pref_set_file_by_appdata_path("rouziclib screenshot", "config.txt");

	vector_font_load_from_header();

#ifdef RL_OPENCL
	fb->use_drawq = 1;
#else
	fb.use_drawq = 0;
#endif
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	fb->r.use_frgb = fb->use_drawq;
	sdl_graphics_init_autosize("rouziclib screenshot", SDL_WINDOW_RESIZABLE, 0);
	SDL_MaximizeWindow(fb->window);
	sdl_toggle_borderless_fullscreen();

	zc = init_zoom(&mouse, drawing_thickness);
	calc_screen_limits(&zc);
	init_mouse();
	SDL_HideWindow(fb->window);

	gui_col_def = make_grey(0.25);

	// Initialise the image selector to the first image
	d->image_id = 1.;

	// Load hotkey preference
	d->hotkey_mod = pref_get_double(&pref_def, "Hotkey:Modifiers", 2 /*MOD_CONTROL*/ | 0x4000 /*MOD_NOREPEAT*/, NULL);
	d->hotkey_vk =  pref_get_double(&pref_def, "Hotkey:Key", 0x70 /*VK_F1*/, NULL);

	// Hotkey registration
	if (RegisterHotKey(NULL, 1, d->hotkey_mod, d->hotkey_vk) == 0)
	{
		sdl_box_printf("rouziclib screenshot", "Hotkey already registered.");
		d->hotkey_diag_on = 1;
		d->apply_status = -1;
	}

hotkey_start:
	// Wait for hotkey
	if (first_loop == 0 && d->hotkey_diag_on == 0 && d->apply_status != -1)
	{
		MSG msg = {0};
		while (GetMessage(&msg, NULL, 0, 0))
		{
			if (msg.message == 0x0312 /*WM_HOTKEY*/)
			{
				d->shot_flag = 1;
				d->raise_flag = 1;
				break;
			}
		}
	}

	d->hide_flag = 0;

	while (d->hide_flag==0 && d->exit_flag==0)
	{
		//********Input handling********

		mouse_pre_event_proc(&mouse);
		keyboard_pre_event_proc(&mouse);
		sdl_handle_window_resize(&zc);

		while (SDL_PollEvent(&event))
		{
			dropfile_event_proc(event);
			sdl_mouse_event_proc(&mouse, event, &zc);
			sdl_keyboard_event_proc(&mouse, event);

			if (event.type==SDL_QUIT)
				d->hide_flag = 1;
		}

		if (mouse.key_state[RL_SCANCODE_RETURN] == 2 && get_kb_alt() > 0)
			sdl_toggle_borderless_fullscreen();

		textedit_add(cur_textedit, NULL);	// processes the new keystrokes in the current text editor

		mouse_post_event_proc(&mouse, &zc);

		//-------------input-----------------

		screenshot_editor(d);
		gui_layout_edit_toolbar(mouse.key_state[RL_SCANCODE_F6]==2);

		window_manager();

		mousecursor_logic_and_draw();

		sdl_flip_fb();

		if (first_loop==0)
		{
			SDL_ShowWindow(fb->window);
			if (d->raise_flag)
			{
				SDL_RaiseWindow(fb->window);
				d->raise_flag = 0;
			}
		}
		else
		{
			first_loop = 0;
			goto hotkey_start;
		}
	}

	SDL_HideWindow(fb->window);
	if (d->exit_flag==0)
		goto hotkey_start;

	sdl_quit_actions();

	return 0;
}
