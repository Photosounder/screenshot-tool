#include "rl.h"
#include <WinUser.h>

void take_and_process_screenshot(raster_t *r, mipmap_t *mm)
{
	free_raster(r);
	free_mipmap(mm);

	*r = take_desktop_screenshot();
	convert_image_srgb8_fullarg(r, (uint8_t *) r->srgb, IMAGE_USE_SQRGB, 0);
	*mm = raster_to_tiled_mipmaps_fast_defaults(*r, IMAGE_USE_SQRGB);
	free_null(&r->sq);
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

typedef struct
{
	int hide_flag, exit_flag, shot_flag, raise_flag, hotkey_diag_on;
	raster_t r, rc;
	mipmap_t mm;
	int knob_ret, crop_recalc, preview;
	double save_fail_time;
	char datestamp[32];
	ctrl_resize_rect_t resize_state;
	rect_t resize_box, crop_rect, im_rect, prev_rect;
	recti_t crop_recti;
	uint32_t hotkey_mod, hotkey_vk;
	int apply_status;
	double apply_time;
} edit_data_t;

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
		"elem 0", "type none", "label Options", "pos	0	0", "dim	3	8;5", "off	0	1", "",
		"elem 10", "type button", "label Reset sel", "pos	1;5	-2;7;6", "dim	1	0;4;6", "off	1", "",
		"elem 11", "type button", "label Reuse prev sel", "link_pos_id 73.cb", "pos	-0;6;6	-0;8", "dim	1;1	0;3", "off	0;6	1", "",
		"elem 20", "type button", "label Save screenshot", "link_pos_id 60", "pos	0	-0;8", "dim	2	0;7", "off	0;6	1", "",
		"elem 30", "type button", "label Hide window", "link_pos_id 20", "pos	0	-0;10", "dim	2	0;7", "off	0;6	1", "",
		"elem 40", "type button", "label Exit", "link_pos_id 30", "pos	0	-0;10", "dim	2	0;7", "off	0;6	1", "",
		"elem 50", "type textedit", "link_pos_id 11.rb", "pos	0	-0;6", "dim	2	0;5", "off	0;6	1", "",
		"elem 51", "type label", "label Folder", "link_pos_id 50", "pos	-0;11;6	0;0;6", "dim	1	0;3;6", "off	0", "",
		"elem 52", "type button", "label Open \360\237\223\201", "link_pos_id 50", "pos	1	-0;6", "dim	0;9	0;3;6", "off	1", "",
		"elem 60", "type textedit", "link_pos_id 50", "pos	0	-1;1", "dim	2	0;5", "off	0;6	1", "",
		"elem 61", "type label", "label Filename", "link_pos_id 60", "pos	-0;11;6	0;0;6", "dim	1;5;6	0;3;6", "off	0", "",
		"elem 70", "type knob", "label Top", "knob 0 0 1079 linear %.0f", "pos	1;6	-1", "dim	0;8", "off	0;6	1", "",
		"elem 71", "type knob", "label Left", "knob 0 0 1919 linear %.0f", "pos	0;8	-1;5", "dim	0;8", "off	0;6	1", "",
		"elem 72", "type knob", "label Right", "knob 0 0 1919 linear %.0f", "pos	2;4	-1;5", "dim	0;8", "off	0;6	1", "",
		"elem 73", "type knob", "label Bottom", "knob 0 0 1079 linear %.0f", "pos	1;6	-1;10", "dim	0;8", "off	0;6	1", "",
		"elem 75", "type label", "label Selection", "link_pos_id 70", "pos	0	0;3;6", "dim	2	0;2;6", "off	0;6	1", "",
		"elem 80", "type checkbox", "label Preview", "link_pos_id 10", "pos	0;8	-0;0;6", "dim	1	0;3;6", "off	0;6	1", "",
		"elem 90", "type checkbox", "label Set hotkey", "link_pos_id 11.rc", "pos	0;2	0", "dim	1	0;3", "off	0	0;6", "",
	};

	gui_layout_init_pos_scale(&layout, xy(zc.limit_u.x-1.5, 7.5), 1.4, xy(-3., 0.), 0);
	make_gui_layout(&layout, layout_src, sizeof(layout_src)/sizeof(char *), "Screenshot editor");

	if (d->shot_flag)
	{
		time_t now = time(NULL);
		take_and_process_screenshot(&d->r, &d->mm);
		d->shot_flag = 0;

		// Make d->datestamp
		strftime(d->datestamp, sizeof(d->datestamp), "%Y-%m-%d %H.%M.%S", localtime(&now));
		print_to_layout_textedit(&layout, 60, 0, "%s.png", d->datestamp);			// generate filename

		// Set knob limits
		get_knob_data_fromlayout(&layout, 70)->max = d->r.dim.y-1;
		get_knob_data_fromlayout(&layout, 73)->max = d->r.dim.y-1;
		get_knob_data_fromlayout(&layout, 71)->max = d->r.dim.x-1;
		get_knob_data_fromlayout(&layout, 72)->max = d->r.dim.x-1;

		d->resize_box = d->im_rect;
		d->crop_rect = rect(XY0, xyi_to_xy(sub_xyi(d->r.dim, set_xyi(1))));
		d->crop_recalc = 1;

		// Make copy
		free_raster(&d->rc);
		d->rc = copy_raster(d->r);
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

	// GUI controls
	if (ctrl_button_fromlayout(&layout, 10))		// Reset selection
	{
		d->resize_box = d->im_rect;
		d->crop_recalc = 1;
	}

	if (ctrl_button_fromlayout(&layout, 11))		// Reuse previous selection
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

	// Selection knobs
	draw_label_fromlayout(&layout, 75, ALIG_LEFT | MONODIGITS);
	d->knob_ret |= ctrl_knob_fromlayout(&d->crop_rect.p0.y, &layout, 70);
	d->knob_ret |= ctrl_knob_fromlayout(&d->crop_rect.p0.x, &layout, 71);
	d->knob_ret |= ctrl_knob_fromlayout(&d->crop_rect.p1.x, &layout, 72);
	d->knob_ret |= ctrl_knob_fromlayout(&d->crop_rect.p1.y, &layout, 73);

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
		// Copy cropped image
		raster_t rs={0};
		rs = make_raster(NULL, sub_xyi(add_xyi(XYI1, d->crop_recti.p1), d->crop_recti.p0), XYI0, IMAGE_USE_SRGB);
		for (i=d->crop_recti.p0.y; i <= d->crop_recti.p1.y; i++)
			memcpy(&rs.srgb[(i-d->crop_recti.p0.y)*rs.dim.x], &d->r.srgb[i*d->r.dim.x + d->crop_recti.p0.x], rs.dim.x * sizeof(srgb_t));

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

	// Apply the cropping visually
	if (d->crop_recalc || d->knob_ret)
	{
		if (d->crop_recalc)
		{
			// Crop recalculation
			d->crop_rect.p0 = sel_coord_to_pix_coord(d->resize_box.p0, d->im_rect, d->r.dim);
			d->crop_rect.p1 = sel_coord_to_pix_coord(d->resize_box.p1, d->im_rect, d->r.dim);
			d->crop_rect = sort_rect(d->crop_rect);
		}

		d->crop_recti = rect_to_recti_round(d->crop_rect);
		d->crop_recti = recti_boolean_intersection(d->crop_recti, recti(XYI0, sub_xyi(d->r.dim, set_xyi(1))));

		memset(d->rc.srgb, 0, mul_x_by_y_xyi(d->r.dim) * sizeof(srgb_t));
		for (i=d->crop_recti.p0.y; i <= d->crop_recti.p1.y; i++)
			memcpy(&d->rc.srgb[i*d->rc.dim.x + d->crop_recti.p0.x], &d->r.srgb[i*d->r.dim.x + d->crop_recti.p0.x], (1+d->crop_recti.p1.x-d->crop_recti.p0.x) * sizeof(srgb_t));

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
	//rect_t d->im_rect = blit_in_rect(&d->r, sc_rect(make_rect_off(XY0, mul_xy(zc.limit_u, set_xy(2.)), xy(0.5, 0.5))), 1, AA_NEAREST_INTERP);	// the mipmap image is fitted inside a rectangle that represents the default view
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
