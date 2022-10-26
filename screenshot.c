#include "rl.h"

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

void screenshot_editor(int *shot_flag, int *hide_flag, int *exit_flag)
{
	static raster_t r={0}, rc={0};
	static mipmap_t mm={0};
	int i, ret;
	static int init=1, knob_ret=0, crop_recalc=0, preview=0;
	static double save_fail_time=0.;
	static char datestamp[32];
	static ctrl_resize_rect_t resize_state={0};
	static rect_t resize_box={0}, crop_rect={0}, im_rect={0}, prev_rect={0};
	static recti_t crop_recti={0};

	// GUI layout
	static gui_layout_t layout={0};
	const char *layout_src[] = {
		"elem 0", "type none", "label Options", "pos	0	0", "dim	3	8;5", "off	0	1", "",
		"elem 10", "type button", "label Reset sel", "pos	1;5	-2;7;6", "dim	1	0;4;6", "off	1", "",
		"elem 11", "type button", "label Reuse prev sel", "link_pos_id 73.cb", "pos	0	-0;8", "dim	1;5;6	0;3", "off	0;6	1", "",
		"elem 20", "type button", "label Save screenshot", "link_pos_id 60", "pos	0	-0;8", "dim	2	0;7", "off	0;6	1", "",
		"elem 30", "type button", "label Hide window", "link_pos_id 20", "pos	0	-0;10", "dim	2	0;7", "off	0;6	1", "",
		"elem 40", "type button", "label Exit", "link_pos_id 30", "pos	0	-0;10", "dim	2	0;7", "off	0;6	1", "",
		"elem 50", "type textedit", "link_pos_id 11.cb", "pos	0	-0;6", "dim	2	0;5", "off	0;6	1", "",
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
	};

	gui_layout_init_pos_scale(&layout, xy(zc.limit_u.x-1.5, 7.5), 1.4, xy(-3., 0.), 0);
	make_gui_layout(&layout, layout_src, sizeof(layout_src)/sizeof(char *), "Screenshot editor");

	if (*shot_flag)
	{
		time_t now = time(NULL);
		take_and_process_screenshot(&r, &mm);
		*shot_flag = 0;

		// Make datestamp
		strftime(datestamp, sizeof(datestamp), "%Y-%m-%d %H.%M.%S", localtime(&now));
		print_to_layout_textedit(&layout, 60, 0, "%s.png", datestamp);			// generate filename

		// Set knob limits
		get_knob_data_fromlayout(&layout, 70)->max = r.dim.y-1;
		get_knob_data_fromlayout(&layout, 73)->max = r.dim.y-1;
		get_knob_data_fromlayout(&layout, 71)->max = r.dim.x-1;
		get_knob_data_fromlayout(&layout, 72)->max = r.dim.x-1;

		resize_box = im_rect;
		crop_rect = rect(XY0, xyi_to_xy(sub_xyi(r.dim, set_xyi(1))));
		crop_recalc = 1;

		// Make copy
		free_raster(&rc);
		rc = copy_raster(r);
	}

	if (init)
	{
		init = 0;

		// Set saved folder path
		char *default_folder = win_get_system_folder_path(0x0027 /*CSIDL_MYPICTURES*/);
		print_to_layout_textedit(&layout, 50, 1, "%s", pref_get_string(&pref_def, "Folder location", default_folder));
		free(default_folder);
	}

	// Apply the cropping visually
	if (crop_recalc || knob_ret)
	{
		if (crop_recalc)
		{
			// Crop recalculation
			crop_rect.p0 = sel_coord_to_pix_coord(resize_box.p0, im_rect, r.dim);
			crop_rect.p1 = sel_coord_to_pix_coord(resize_box.p1, im_rect, r.dim);
			crop_rect = sort_rect(crop_rect);
		}

		crop_recti = rect_to_recti_round(crop_rect);
		crop_recti = recti_boolean_intersection(crop_recti, recti(XYI0, sub_xyi(r.dim, set_xyi(1))));

		memset(rc.srgb, 0, mul_x_by_y_xyi(r.dim) * sizeof(srgb_t));
		for (i=crop_recti.p0.y; i <= crop_recti.p1.y; i++)
			memcpy(&rc.srgb[i*rc.dim.x + crop_recti.p0.x], &r.srgb[i*r.dim.x + crop_recti.p0.x], (1+crop_recti.p1.x-crop_recti.p0.x) * sizeof(srgb_t));

		free_mipmap(&mm);
		convert_image_srgb8_fullarg(&rc, (uint8_t *) rc.srgb, IMAGE_USE_SQRGB, 0);
		mm = raster_to_tiled_mipmaps_fast_defaults(rc, IMAGE_USE_SQRGB);
		free_null(&rc.sq);
		crop_recalc = 0;
		knob_ret = 0;
	}

	// Draw image
	drawq_bracket_open();
	im_rect = blit_mipmap_in_rect(mm, sc_rect(make_rect_off(XY0, mul_xy(zc.limit_u, set_xy(2.)), xy(0.5, 0.5))), 1, AA_NEAREST_INTERP);	// the mipmap image is fitted inside a rectangle that represents the default view
	//rect_t im_rect = blit_in_rect(&r, sc_rect(make_rect_off(XY0, mul_xy(zc.limit_u, set_xy(2.)), xy(0.5, 0.5))), 1, AA_NEAREST_INTERP);	// the mipmap image is fitted inside a rectangle that represents the default view
	if (preview==0)
		draw_gain(0.6);
	drawq_bracket_close(DQB_ADD);

	// Selection control
	gui_col_def = make_grey(0.4);
	if (preview==0)
		if (ctrl_resizing_rect(&resize_state, &resize_box))
			crop_recalc = 1;
	gui_col_def = make_grey(0.25);

	// GUI window
	static flwindow_t window={0};
	flwindow_init_defaults(&window);
	flwindow_init_pinned(&window);
	window.bg_opacity = 0.94;
	window.shadow_strength = 0.5*window.bg_opacity;
	draw_dialog_window_fromlayout(&window, NULL, NULL, &layout, 0);	// this handles and displays the window that contains the control

	// Save failure colour
	col_t save_fail_col = mix_colours(make_colour(1., 0., 0., 0.), gui_col_def, 1.-gaussian(get_time_hr()-save_fail_time));
	gui_set_control_colour(save_fail_col, &layout, 60);	// path textedit
	gui_set_control_colour(save_fail_col, &layout, 20);	// Save button

	// GUI controls
	if (ctrl_button_fromlayout(&layout, 10))		// Reset selection
	{
		resize_box = im_rect;
		crop_recalc = 1;
	}

	if (ctrl_button_fromlayout(&layout, 11))		// Reuse previous selection
	{
		resize_box = prev_rect;
		crop_recalc = 1;
	}

	ctrl_checkbox_fromlayout(&preview, &layout, 80);	// Preview

	if (ctrl_button_fromlayout(&layout, 30))		// Hide window
		*hide_flag = 1;

	if (ctrl_button_fromlayout(&layout, 40))		// Exit
		*exit_flag = 1;

	if (ctrl_button_fromlayout(&layout, 52))		// Open folder
		system_open(get_textedit_string_fromlayout(&layout, 50));

	// Selection knobs
	draw_label_fromlayout(&layout, 75, ALIG_LEFT | MONODIGITS);
	knob_ret |= ctrl_knob_fromlayout(&crop_rect.p0.y, &layout, 70);
	knob_ret |= ctrl_knob_fromlayout(&crop_rect.p0.x, &layout, 71);
	knob_ret |= ctrl_knob_fromlayout(&crop_rect.p1.x, &layout, 72);
	knob_ret |= ctrl_knob_fromlayout(&crop_rect.p1.y, &layout, 73);

	// Path setting
	draw_label_fromlayout(&layout, 51, ALIG_LEFT);
	ret = ctrl_textedit_fromlayout(&layout, 50);
	if (ret > 0 && ret < 4)				// Save the path to prefs if changed
		pref_set_string(&pref_def, "Folder location", get_textedit_string_fromlayout(&layout, 50));

	// File name
	ret = ctrl_textedit_fromlayout(&layout, 60);
	draw_label_fromlayout(&layout, 61, ALIG_LEFT);

	// Save screenshot
	if (ctrl_button_fromlayout(&layout, 20) || ret==1)
	{
		// Copy cropped image
		raster_t rs={0};
		rs = make_raster(NULL, sub_xyi(add_xyi(XYI1, crop_recti.p1), crop_recti.p0), XYI0, IMAGE_USE_SRGB);
		for (i=crop_recti.p0.y; i <= crop_recti.p1.y; i++)
			memcpy(&rs.srgb[(i-crop_recti.p0.y)*rs.dim.x], &r.srgb[i*r.dim.x + crop_recti.p0.x], rs.dim.x * sizeof(srgb_t));

		// Save it to file
		char *save_path = append_name_to_path(NULL, get_textedit_string_fromlayout(&layout, 50), get_textedit_string_fromlayout(&layout, 60));
		int save_ret = save_image(save_path, rs, 92);
		free(save_path);
		free_raster(&rs);

		if (save_ret == 0)
			save_fail_time = get_time_hr();
		else
			*hide_flag = 1;
	}
	
	if (*hide_flag)
	{
		// Reset view
		zoom_reset(&zc, &mouse.zoom_flag);
		gui_layout_edit_toolbar(mouse.key_state[RL_SCANCODE_F6]==2);

		// Save rect for potential later reuse
		prev_rect = resize_box;

		// Reset the preview checkbox to off
		preview = 0;
	}
}

#include <WinUser.h>

int main(int argc, char *argv[])
{
	int hide_flag=0, exit_flag=0, init=1, shot_flag=0, raise_flag=0, first_loop=1;
	SDL_Event event;

	// Hotkey registration
	if (RegisterHotKey(NULL, 1, 2 /*MOD_CONTROL*/ | 0x4000 /*MOD_NOREPEAT*/, 0x70 /*VK_F1*/) == 0)
	{
		sdl_box_printf("rouziclib screenshot", "Hotkey already registered, error #%d\n", GetLastError());
		exit(0);
	}

	if (init)
	{
		init = 0;

		pref_def = pref_set_file_by_appdata_path("rouziclib screenshot", "config.txt");

		vector_font_load_from_header();

		#ifdef RL_OPENCL
		fb->use_drawq = 1;
		#else
		fb.use_drawq = 0;
		#endif
		fb->r.use_frgb = fb->use_drawq;
		sdl_graphics_init_autosize("rouziclib screenshot", SDL_WINDOW_RESIZABLE, 0);
		SDL_MaximizeWindow(fb->window);
		sdl_toggle_borderless_fullscreen();

		zc = init_zoom(&mouse, drawing_thickness);
		calc_screen_limits(&zc);
		mouse = init_mouse();
		SDL_HideWindow(fb->window);

		gui_col_def = make_grey(0.25);
	}

hotkey_start:
	if (first_loop==0)
	{
		// Wait for hotkey
		MSG msg = {0};
		while (GetMessage(&msg, NULL, 0, 0))
		{
			if (msg.message == 0x0312 /*WM_HOTKEY*/)
			{
				shot_flag = 1;
				raise_flag = 1;
				break;
			}
		}
	}

	hide_flag = 0;

	while (hide_flag==0 && exit_flag==0)
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
				hide_flag = 1;
		}

		if (mouse.key_state[RL_SCANCODE_RETURN] == 2 && get_kb_alt())
			sdl_toggle_borderless_fullscreen();

		textedit_add(cur_textedit, NULL);	// processes the new keystrokes in the current text editor

		mouse_post_event_proc(&mouse, &zc);

		//-------------input-----------------

		screenshot_editor(&shot_flag, &hide_flag, &exit_flag);
		gui_layout_edit_toolbar(mouse.key_state[RL_SCANCODE_F6]==2);

		window_manager();

		mousecursor_logic_and_draw();

		sdl_flip_fb();

		if (first_loop==0)
		{
			SDL_ShowWindow(fb->window);
			if (raise_flag)
			{
				SDL_RaiseWindow(fb->window);
				raise_flag = 0;
			}
		}
		else
		{
			first_loop = 0;
			goto hotkey_start;
		}
	}

	SDL_HideWindow(fb->window);
	if (exit_flag==0)
		goto hotkey_start;

	sdl_quit_actions();

	return 0;
}
