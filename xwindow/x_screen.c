/*
 *	$Id$
 */

#include  "x_screen.h"

#include  <signal.h>
#include  <stdio.h>		/* sprintf */
#include  <unistd.h>            /* getcwd */
#include  <kiklib/kik_mem.h>	/* alloca */
#include  <kiklib/kik_debug.h>
#include  <kiklib/kik_str.h>	/* strdup, kik_snprintf */
#include  <kiklib/kik_util.h>	/* K_MIN */
#include  <kiklib/kik_locale.h>	/* kik_get_locale */
#include  <kiklib/kik_conf_io.h>	/* kik_get_user_rc_path */
#include  <kiklib/kik_def.h>	/* PATH_MAX , HAVE_WINDOWS_H */
#include  <kiklib/kik_args.h>	/* kik_arg_str_to_array */
#include  <mkf/mkf_xct_parser.h>
#include  <mkf/mkf_xct_conv.h>
#ifdef  USE_WIN32GUI
#include  <mkf/mkf_utf16_conv.h>
#include  <mkf/mkf_utf16_parser.h>
#endif
#include  <ml_str_parser.h>

#include  "x_xic.h"
#include  "x_draw_str.h"


#define  HAS_SYSTEM_LISTENER(screen,function) \
	((screen)->system_listener && (screen)->system_listener->function)

#define  HAS_SCROLL_LISTENER(screen,function) \
	((screen)->screen_scroll_listener && (screen)->screen_scroll_listener->function)

#define  MOUSE_POS_LIMIT  (0xff - 0x20)
#define  EXT_MOUSE_POS_LIMIT  (0x7ff - 0x20)

#if  1
#define  NL_TO_CR_IN_PAST_TEXT
#endif

#if  0
#define  __DEBUG
#endif

#if  0
#define  SCROLL_BY_SIXEL
#endif


/*
 * For x_window_update()
 *
 * XXX
 * Note that vte.c calls x_window_update( ... , 1), so if you change following enums,
 * vte.c must be changed at the same time.
 */
enum
{
	UPDATE_SCREEN = 0x1 ,
	UPDATE_CURSOR = 0x2 ,
} ;


/* --- static variables --- */

/*
 * 0 = traditional (not open)
 * 1 = conf_menu_path_1
 * 2 = conf_menu_path_2
 * 3 = conf_menu_path_3
 * 4 = mlclient -e w3m
 * 5 = do nothing
 */
static int  button3_open ;
static char *  button3_command ;
static int  exit_backscroll_by_pty ;
#ifdef  USE_IM_CURSOR_COLOR
static char *  im_cursor_color = NULL ;
#endif


/* --- static functions --- */

static int
true_or_false(
	char *  str
	)
{
	if( strcmp( str , "true") == 0)
	{
		return  1 ;
	}
	else if( strcmp( str , "false") == 0)
	{
		return  0 ;
	}
	else
	{
		return  -1 ;
	}
}

static int
convert_row_to_y(
	x_screen_t *  screen ,
	int  row		/* Should be 0 >= and <= ml_term_get_rows() */
	)
{
	/*
	 * !! Notice !!
	 * assumption: line hight is always the same!
	 */

	return  x_line_height( screen) * row ;
}

/*
 * If y < 0 , return 0 with *y_rest = 0.
 * If y > screen->window.height , return screen->window.height / line_height with *y_rest =
 * y - screen->window.height.
 */
static int
convert_y_to_row(
	x_screen_t *  screen ,
	u_int *  y_rest ,
	int  y
	)
{
	int  row ;

	if( y < 0)
	{
		y = 0 ;
	}
	
	/*
	 * !! Notice !!
	 * assumption: line hight is always the same!
	 */

	if( y >= screen->window.height)
	{
		row = (screen->window.height - 1) / x_line_height( screen) ;
	}
	else
	{
		row = y / x_line_height( screen) ;
	}

	if( y_rest)
	{
		*y_rest = y - row * x_line_height( screen) ;
	}

	return  row ;
}

static int
convert_char_index_to_x(
	x_screen_t *  screen ,
	ml_line_t *  line ,
	int  char_index		/* Should be 0 >= and <= ml_line_end_char_index() */
	)
{
	int  count ;
	int  x ;

	if( ml_line_is_rtl( line))
	{
		x = screen->window.width ;

		for( count = ml_line_end_char_index(line) ; count >= char_index ; count --)
		{
			ml_char_t *  ch ;

			ch = ml_char_at( line , count) ;

			if( ml_char_cols( ch) > 0)
			{
				x -= x_calculate_char_width(
					x_get_font( screen->font_man , ml_char_font( ch)) ,
					ml_char_bytes( ch) , ml_char_size( ch) ,
					ml_char_cs( ch) , NULL) ;
			}
		}
	}
	else
	{
		/*
		 * excluding the last char width.
		 */
		x = 0 ;
		for( count = 0 ; count < char_index ; count ++)
		{
			ml_char_t *  ch ;

			ch = ml_char_at( line , count) ;

			if( ml_char_cols( ch) > 0)
			{
				x += x_calculate_char_width(
					x_get_font( screen->font_man , ml_char_font( ch)) ,
					ml_char_bytes( ch) , ml_char_size( ch) ,
					ml_char_cs( ch) , NULL) ;
			}
		}
	}

	return  x ;
}

static int
convert_char_index_to_x_with_shape(
	x_screen_t *  screen ,
	ml_line_t *  line ,
	int  char_index
	)
{
	ml_line_t *  orig ;
	int  x ;

	if( screen->term->shape)
	{
		if( ( orig = ml_line_shape( line , screen->term->shape)) == NULL)
		{
		#ifdef  DEBUG
			kik_warn_printf( KIK_DEBUG_TAG " ml_line_shape() failed.\n") ;
		#endif
		}
	}
	else
	{
		orig = NULL ;
	}

	x = convert_char_index_to_x( screen , line , char_index) ;

	if( orig)
	{
		ml_line_unshape( line , orig) ;
	}

	return  x ;
}

/*
 * If x < 0 , return 0 with *x_rest = 0.
 * If x > screen->window.width , return screen->window.width / char_width with *x_rest =
 * x - screen->window.width.
 */
static int
convert_x_to_char_index(
	x_screen_t *  screen ,
	ml_line_t *  line ,
	u_int *  x_rest ,
	int  x
	)
{
	int  count ;
	u_int  width ;
	int  end_char_index ;

	end_char_index = ml_line_end_char_index(line) ;

	if( ml_line_is_rtl( line))
	{
		if( x > screen->window.width)
		{
			x = 0 ;
		}
		else
		{
			x = screen->window.width - x ;
		}

		for( count = end_char_index ; count > 0 ; count --)
		{
			ml_char_t *  ch ;

			ch = ml_char_at( line , count) ;

			if( ml_char_cols( ch) == 0)
			{
				continue ;
			}

			width = x_calculate_char_width(
				x_get_font( screen->font_man , ml_char_font( ch)) ,
				ml_char_bytes( ch) , ml_char_size( ch) , ml_char_cs( ch) , NULL) ;

			if( x <= width)
			{
				break ;
			}

			x -= width ;
		}
	}
	else
	{
		if( x < 0)
		{
			x = 0 ;
		}

		for( count = 0 ; count < end_char_index ; count ++)
		{
			ml_char_t *  ch ;

			ch = ml_char_at( line , count) ;

			if( ml_char_cols( ch) == 0)
			{
				continue ;
			}

			width = x_calculate_char_width(
				x_get_font( screen->font_man , ml_char_font( ch)) ,
				ml_char_bytes( ch) , ml_char_size( ch) , ml_char_cs( ch) , NULL) ;

			if( x < width)
			{
				break ;
			}

			x -= width ;
		}
	}

	if( x_rest != NULL)
	{
		*x_rest = x ;
	}

	return  count ;
}

static int
convert_x_to_char_index_with_shape(
	x_screen_t *  screen ,
	ml_line_t *  line ,
	u_int *  x_rest ,
	int  x
	)
{
	ml_line_t *  orig ;
	int  char_index ;

	if( screen->term->shape)
	{
		if( ( orig = ml_line_shape( line , screen->term->shape)) == NULL)
		{
		#ifdef  DEBUG
			kik_warn_printf( KIK_DEBUG_TAG " ml_line_shape() failed.\n") ;
		#endif
		}
	}
	else
	{
		orig = NULL ;
	}

	char_index = convert_x_to_char_index( screen , line , x_rest , x) ;

	if( orig)
	{
		ml_line_unshape( line , orig) ;
	}

	return  char_index ;
}

static u_int
screen_width(
	x_screen_t *  screen
	)
{
	u_int  width ;

	/*
	 * logical cols/rows => visual width/height.
	 */

	if( ml_term_get_vertical_mode( screen->term))
	{
		width = ml_term_get_logical_rows( screen->term) * x_col_width( screen) ;
	}
	else
	{
		width = ml_term_get_logical_cols( screen->term) * x_col_width( screen) ;
	}

	return  (width * screen->screen_width_ratio) / 100 ;
}

static u_int
screen_height(
	x_screen_t *  screen
	)
{
	u_int  height ;

	/*
	 * logical cols/rows => visual width/height.
	 */

	if( ml_term_get_vertical_mode( screen->term))
	{
		height = ml_term_get_logical_cols( screen->term) * x_line_height( screen) ;
	}
	else
	{
		height = ml_term_get_logical_rows( screen->term) * x_line_height( screen) ;
	}

	return  (height * screen->screen_height_ratio) / 100 ;
}

static int
activate_xic(
	x_screen_t *  screen
	)
{
	/*
	 * FIXME: This function is a dirty wrapper on x_xic_activate().
	 */

	char *  saved_ptr;
	char *  xim_name ;
	char *  xim_locale ;

	xim_name = xim_locale = NULL ;

	saved_ptr = kik_str_sep( &screen->input_method , ":") ;
	xim_name = kik_str_sep( &screen->input_method , ":") ;
	xim_locale = kik_str_sep( &screen->input_method , ":") ;

	x_xic_activate( &screen->window ,
			xim_name ? xim_name : "" ,
			xim_locale ? xim_locale : "") ;

	if( xim_name)
	{
		*(xim_name-1) = ':' ;
	}

	if( xim_locale)
	{
		*(xim_locale-1) = ':' ;
	}

	screen->input_method = saved_ptr ;

	return  1 ;
}

/*
 * drawing screen functions.
 */

static int
draw_line(
	x_screen_t *  screen ,
	ml_line_t *  line ,
	int  y
	)
{
	int  beg_x ;
	int  ret ;

	ret = 0 ;
	
	if( ml_line_is_empty( line))
	{
		x_window_clear( &screen->window , (beg_x = 0) , y ,
			screen->window.width , x_line_height(screen)) ;
		ret = 1 ;
	}
	else
	{
		int  beg_char_index ;
		u_int  num_of_redrawn ;
		int  is_cleared_to_end ;
		ml_line_t *  orig ;
		x_font_present_t  present ;

		if( screen->term->shape)
		{
			if( ( orig = ml_line_shape( line , screen->term->shape)) == NULL)
			{
				return  0 ;
			}
		}
		else
		{
			orig = NULL ;
		}

		present = x_get_font_present( screen->font_man) ;

		if( ml_line_is_cleared_to_end( line) || ( present & FONT_VAR_WIDTH))
		{
			is_cleared_to_end = 1 ;
		}
		else
		{
			is_cleared_to_end = 0 ;
		}

		beg_char_index = ml_line_get_beg_of_modified( line) ;
		num_of_redrawn = ml_line_get_num_of_redrawn_chars( line , is_cleared_to_end) ;

		if( ( present & FONT_VAR_WIDTH) && ml_line_is_rtl( line))
		{
			num_of_redrawn += beg_char_index ;
			beg_char_index = 0 ;
		}

		/* don't use _with_shape function since line is already shaped */
		beg_x = convert_char_index_to_x( screen , line , beg_char_index) ;

		if( is_cleared_to_end)
		{
			if( ml_line_is_rtl( line))
			{
				x_window_clear( &screen->window , 0 , y ,
					beg_x , x_line_height( screen)) ;

				if( ! x_draw_str( &screen->window ,
					screen->font_man , screen->color_man ,
					ml_char_at( line , beg_char_index) ,
					num_of_redrawn , beg_x , y ,
					x_line_height( screen) ,
					x_line_ascent( screen) ,
					x_line_top_margin( screen) ,
					x_line_bottom_margin( screen) ,
					screen->hide_underline))
				{
					goto  end ;
				}
			}
			else
			{
				if( ! x_draw_str_to_eol( &screen->window ,
					screen->font_man , screen->color_man ,
					ml_char_at( line , beg_char_index) ,
					num_of_redrawn , beg_x , y ,
					x_line_height( screen) ,
					x_line_ascent( screen) ,
					x_line_top_margin( screen) ,
					x_line_bottom_margin( screen) ,
					screen->hide_underline))
				{
					goto  end ;
				}
			}
		}
		else
		{
			if( ! x_draw_str( &screen->window ,
				screen->font_man , screen->color_man ,
				ml_char_at( line , beg_char_index) ,
				num_of_redrawn , beg_x , y ,
				x_line_height( screen) ,
				x_line_ascent( screen) ,
				x_line_top_margin( screen) ,
				x_line_bottom_margin( screen) ,
				screen->hide_underline))
			{
				goto  end ;
			}
		}

		ret = 1 ;

	end:
		if( orig)
		{
			ml_line_unshape( line , orig) ;
		}
	}

	return  ret ;
}

static int xterm_im_is_active( void *  p) ;

/*
 * Don't call this function directly.
 * Call this function via highlight_cursor.
 */
static int
draw_cursor(
	x_screen_t *  screen
	)
{
	int  row ;
	int  x ;
	int  y ;
	ml_line_t *  line ;
	ml_line_t *  orig ;
	ml_char_t  ch ;
#ifdef  USE_IM_CURSOR_COLOR
	char *  orig_cursor_bg ;
	int  cursor_bg_is_replaced = 0 ;
#endif

	if( screen->is_preediting)
	{
		return  1 ;
	}

	if( ! ml_term_is_cursor_visible( screen->term))
	{
		return  1 ;
	}

	if( ( row = ml_term_cursor_row_in_screen( screen->term)) == -1)
	{
		return  0 ;
	}

	y = convert_row_to_y( screen , row) ;

	if( ( line = ml_term_get_cursor_line( screen->term)) == NULL || ml_line_is_empty( line))
	{
	#ifdef  DEBUG
		kik_warn_printf( KIK_DEBUG_TAG " cursor line doesn't exist.\n") ;
	#endif

		return  0 ;
	}

	if( screen->term->shape)
	{
		if( ( orig = ml_line_shape( line , screen->term->shape)) == NULL)
		{
			return  0 ;
		}
	}
	else
	{
		orig = NULL ;
	}

	/* don't use _with_shape function since line is already shaped */
	x = convert_char_index_to_x( screen , line , ml_term_cursor_char_index( screen->term)) ;

	ml_char_init( &ch) ;
	ml_char_copy( &ch , ml_char_at( line , ml_term_cursor_char_index( screen->term))) ;
	
	if( screen->window.is_focused)
	{
	#ifdef  USE_IM_CURSOR_COLOR
		if( im_cursor_color && xterm_im_is_active( screen))
		{
			if( ( orig_cursor_bg = x_color_manager_get_cursor_bg_color(
							screen->color_man)))
			{
				orig_cursor_bg = strdup( orig_cursor_bg) ;
			}

			x_color_manager_set_cursor_bg_color( screen->color_man , im_cursor_color) ;
			cursor_bg_is_replaced = 1 ;
		}
	#endif

		/* if fg/bg color should be overriden, reset ch's color to default */
		if( x_color_manager_adjust_cursor_fg_color( screen->color_man))
		{
			/* for curosr's bg */
			ml_char_set_bg_color( &ch, ML_BG_COLOR) ;
		}

		if( x_color_manager_adjust_cursor_bg_color( screen->color_man))
		{
			/* for cursor's fg */
			ml_char_set_fg_color( &ch, ML_FG_COLOR);
		}

		ml_char_reverse_color( &ch) ;
	}

	x_draw_str( &screen->window , screen->font_man ,
		screen->color_man , &ch , 1 , x , y ,
		x_line_height( screen) ,
		x_line_ascent( screen) ,
		x_line_top_margin( screen) ,
		x_line_bottom_margin( screen) ,
		screen->hide_underline) ;

	if( screen->window.is_focused)
	{
		x_color_manager_adjust_cursor_fg_color( screen->color_man) ;
		x_color_manager_adjust_cursor_bg_color( screen->color_man) ;

	#ifdef  USE_IM_CURSOR_COLOR
		if( cursor_bg_is_replaced)
		{
			x_color_manager_set_cursor_bg_color( screen->color_man , orig_cursor_bg);
			free( orig_cursor_bg) ;
		}
	#endif
	}
	else
	{
		x_font_t *  xfont ;

		xfont = x_get_font( screen->font_man , ml_char_font( &ch)) ;

		x_window_set_fg_color( &screen->window ,
			x_get_xcolor( screen->color_man , ml_char_fg_color(&ch))) ;

		x_window_draw_rect_frame( &screen->window ,
			x , y + x_line_top_margin( screen) ,
			x + x_calculate_char_width( xfont , ml_char_bytes(&ch) ,
				ml_char_size(&ch) , ml_char_cs(&ch) , NULL) - 1 ,
			y + x_line_top_margin( screen) + xfont->height - 1) ;
	}

	ml_char_final( &ch) ;

	if( orig)
	{
		ml_line_unshape( line , orig) ;
	}

	return  1 ;
}

static int
flush_scroll_cache(
	x_screen_t *  screen ,
	int  scroll_actual_screen
	)
{
	int  scroll_cache_rows ;
	int  scroll_region_rows ;

	if( ! screen->scroll_cache_rows)
	{
		return  0 ;
	}

	/*
	 * x_window_scroll_*() can invoke window_exposed event internally,
	 * and flush_scroll_cache() is called twice.
	 * To avoid this, screen->scroll_cache_row is set 0 here before calling
	 * x_window_scroll_*().
	 *
	 * 1) Stop processing VT100 sequence.
	 * 2) flush_scroll_cache() (x_screen.c)
	 * 3) scroll_region() (x_window.c)
	 *   - XCopyArea
	 * 4) Start processing VT100 sequence.
	 * 5) Stop processing VT100 sequence.
	 * 6) x_window_update() to redraw data modified by VT100 sequence.
	 * 7) flush_scroll_cache()
	 * 8) scroll_region()
	 *   - XCopyArea
	 *   - Wait and process GraphicsExpose caused by 3).
	 * 9)flush_scroll_cache()
	 * 10)scroll_region() <- avoid this by screen->scroll_cache_rows = 0.
	 *   - XCopyArea
	 */
	scroll_cache_rows = screen->scroll_cache_rows ;
	screen->scroll_cache_rows = 0 ;
	
	if( scroll_cache_rows >=
	    ( scroll_region_rows = screen->scroll_cache_boundary_end -
	                         screen->scroll_cache_boundary_start + 1))
	{
		return  1 ;
	}

	

	if( scroll_actual_screen && x_window_is_scrollable( &screen->window))
	{
		if( ! ml_term_get_vertical_mode( screen->term))
		{
			int  beg_y ;
			int  end_y ;
			u_int  scroll_height ;

			scroll_height = x_line_height( screen) * abs( scroll_cache_rows) ;

			if( scroll_height < screen->window.height)
			{
				beg_y = convert_row_to_y( screen , screen->scroll_cache_boundary_start) ;
				end_y = beg_y + x_line_height( screen) * scroll_region_rows ;

				if( scroll_cache_rows > 0)
				{
					x_window_scroll_upward_region( &screen->window ,
						beg_y , end_y , scroll_height) ;
				}
				else
				{
					x_window_scroll_downward_region( &screen->window ,
						beg_y , end_y , scroll_height) ;
				}
			}
		#if  0
			else
			{
				x_window_clear_all( &screen->window) ;
			}
		#endif
		}
		else
		{
			int  beg_x ;
			int  end_x ;
			u_int  scroll_width ;

			scroll_width = x_col_width( screen) * abs( scroll_cache_rows) ;

			if( scroll_width < screen->window.width)
			{
				beg_x = x_col_width( screen) * screen->scroll_cache_boundary_start ;
				end_x = beg_x + x_col_width( screen) * scroll_region_rows ;

				if( ml_term_get_vertical_mode( screen->term) & VERT_RTL)
				{
					end_x = screen->window.width - beg_x ;
					beg_x = screen->window.width - end_x ;
					scroll_cache_rows = -(scroll_cache_rows) ;
				}

				if( scroll_cache_rows > 0)
				{
					x_window_scroll_leftward_region( &screen->window ,
						beg_x , end_x , scroll_width) ;
				}
				else
				{
					x_window_scroll_rightward_region( &screen->window ,
						beg_x , end_x , scroll_width) ;
				}
			}
		#if  0
			else
			{
				x_window_clear_all( &screen->window) ;
			}
		#endif
		}
	}
	else
	{
		/*
		 * setting modified mark to the lines within scroll region.
		 *
		 * XXX
		 * Not regarding vertical mode.
		 */

	#if  0
		if( ! ml_term_get_vertical_mode( screen->term))
		{
		}
		else
	#endif
		{
			if( scroll_cache_rows > 0)
			{
				/*
				 * scrolling upward.
				 */
				ml_term_set_modified_lines_in_screen( screen->term ,
					screen->scroll_cache_boundary_start ,
					screen->scroll_cache_boundary_end -
						scroll_cache_rows) ;
			}
			else
			{
				/*
				 * scrolling downward.
				 */
				ml_term_set_modified_lines_in_screen( screen->term ,
					screen->scroll_cache_boundary_start -
						scroll_cache_rows ,
					screen->scroll_cache_boundary_end) ;
			}
		}
	}

	return  1 ;
}

static int
set_scroll_boundary(
	x_screen_t *  screen ,
	int  boundary_start ,
	int  boundary_end
	)
{
	if( screen->scroll_cache_rows)
	{
		if( screen->scroll_cache_boundary_end - screen->scroll_cache_boundary_start
		    > boundary_end - boundary_start)
		{
			/*
			 * Don't call flush_scroll_cache() if new boundary is smaller
			 * in order to avoid convergence of flush_scroll_cache().
			 */
			return  0 ;
		}

		if( screen->scroll_cache_boundary_start != boundary_start ||
		    screen->scroll_cache_boundary_end != boundary_end)
		{
			flush_scroll_cache( screen , 0) ;
		}
	}

	screen->scroll_cache_boundary_start = boundary_start ;
	screen->scroll_cache_boundary_end = boundary_end ;

	return  1 ;
}

/*
 * Don't call this function except from window_exposed or update_window.
 * Call this function via x_window_update.
 */
static int
redraw_screen(
	x_screen_t *  screen
	)
{
	int  count ;
	ml_line_t *  line ;
	int  y ;
	int  line_height ;

	flush_scroll_cache( screen , 1) ;

	count = 0 ;
	while(1)
	{
		if( ( line = ml_term_get_line_in_screen( screen->term , count)) == NULL)
		{
		#ifdef  __DEBUG
			kik_debug_printf( KIK_DEBUG_TAG " nothing is redrawn.\n") ;
		#endif

			return  1 ;
		}

		if( ml_line_is_modified( line))
		{
			break ;
		}

		count ++ ;
	}

#ifdef  __DEBUG
	kik_debug_printf( KIK_DEBUG_TAG " redrawing -> line %d\n" , count) ;
#endif

	y = convert_row_to_y( screen , count) ;

	draw_line( screen , line , y) ;

	count ++ ;
	y += (line_height = x_line_height(screen)) ;

	while( ( line = ml_term_get_line_in_screen( screen->term , count)) != NULL)
	{
		if( ml_line_is_modified( line))
		{
		#ifdef  __DEBUG
			kik_debug_printf( KIK_DEBUG_TAG " redrawing -> line %d\n" , count) ;
		#endif

			draw_line( screen , line , y) ;
		}
	#ifdef  __DEBUG
		else
		{
			kik_debug_printf( KIK_DEBUG_TAG " not redrawing -> line %d\n" , count) ;
		}
	#endif
		
		y += line_height ;
		count ++ ;
	}

	ml_term_updated_all( screen->term) ;

	if( screen->im)
	{
		x_im_redraw_preedit( screen->im , screen->window.is_focused) ;
	}

	return  1 ;
}

/*
 * Don't call this function except from window_exposed or update_window.
 * Call this function via x_window_update.
 */
static int
highlight_cursor(
	x_screen_t *  screen
	)
{
	flush_scroll_cache( screen , 1) ;

	draw_cursor( screen) ;

	x_xic_set_spot( &screen->window) ;

	return  1 ;
}

static int
unhighlight_cursor(
	x_screen_t *  screen ,
	int  revert_visual
	)
{
	return  ml_term_unhighlight_cursor( screen->term , revert_visual) ;
}


/*
 * {enter|exit}_backscroll_mode() and bs_XXX() functions provides backscroll operations.
 *
 * Similar processing to bs_XXX() is done in x_screen_scroll_{upward|downward|to}().
 */

static void
enter_backscroll_mode(
	x_screen_t *  screen
	)
{
	if( ml_term_is_backscrolling( screen->term))
	{
		return ;
	}

	ml_term_enter_backscroll_mode( screen->term) ;

	if( HAS_SCROLL_LISTENER(screen,bs_mode_entered))
	{
		(*screen->screen_scroll_listener->bs_mode_entered)(
			screen->screen_scroll_listener->self) ;
	}
}

static void
exit_backscroll_mode(
	x_screen_t *  screen
	)
{
	if( ! ml_term_is_backscrolling( screen->term))
	{
		return ;
	}

	ml_term_exit_backscroll_mode( screen->term) ;
	x_window_update( &screen->window , UPDATE_SCREEN|UPDATE_CURSOR) ;

	if( HAS_SCROLL_LISTENER(screen,bs_mode_exited))
	{
		(*screen->screen_scroll_listener->bs_mode_exited)(
			screen->screen_scroll_listener->self) ;
	}
}

static void
bs_scroll_upward(
	x_screen_t *  screen
	)
{
	if( ml_term_backscroll_upward( screen->term , 1))
	{
		x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;

		if( HAS_SCROLL_LISTENER(screen,scrolled_upward))
		{
			(*screen->screen_scroll_listener->scrolled_upward)(
				screen->screen_scroll_listener->self , 1) ;
		}
	}
}

static void
bs_scroll_downward(
	x_screen_t *  screen
	)
{
	if( ml_term_backscroll_downward( screen->term , 1))
	{
		x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;

		if( HAS_SCROLL_LISTENER(screen,scrolled_downward))
		{
			(*screen->screen_scroll_listener->scrolled_downward)(
				screen->screen_scroll_listener->self , 1) ;
		}
	}
}

static void
bs_half_page_upward(
	x_screen_t *  screen
	)
{
	if( ml_term_backscroll_upward( screen->term , ml_term_get_rows( screen->term) / 2))
	{
		x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;

		if( HAS_SCROLL_LISTENER(screen,scrolled_upward))
		{
			/* XXX Not necessarily ml_term_get_rows( screen->term) / 2. */
			(*screen->screen_scroll_listener->scrolled_upward)(
				screen->screen_scroll_listener->self ,
				ml_term_get_rows( screen->term) / 2) ;
		}
	}
}

static void
bs_half_page_downward(
	x_screen_t *  screen
	)
{
	if( ml_term_backscroll_downward( screen->term , ml_term_get_rows( screen->term) / 2))
	{
		x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;

		if( HAS_SCROLL_LISTENER(screen,scrolled_downward))
		{
			/* XXX Not necessarily ml_term_get_rows( screen->term) / 2. */
			(*screen->screen_scroll_listener->scrolled_downward)(
				screen->screen_scroll_listener->self ,
				ml_term_get_rows( screen->term) / 2) ;
		}
	}
}

static void
bs_page_upward(
	x_screen_t *  screen
	)
{
	if( ml_term_backscroll_upward( screen->term , ml_term_get_rows( screen->term)))
	{
		x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;

		if( HAS_SCROLL_LISTENER(screen,scrolled_upward))
		{
			/* XXX Not necessarily ml_term_get_rows( screen->term). */
			(*screen->screen_scroll_listener->scrolled_upward)(
				screen->screen_scroll_listener->self ,
				ml_term_get_rows( screen->term)) ;
		}
	}
}

static void
bs_page_downward(
	x_screen_t *  screen
	)
{
	if( ml_term_backscroll_downward( screen->term , ml_term_get_rows( screen->term)))
	{
		x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;

		if( HAS_SCROLL_LISTENER(screen,scrolled_downward))
		{
			/* XXX Not necessarily ml_term_get_rows( screen->term). */
			(*screen->screen_scroll_listener->scrolled_downward)(
				screen->screen_scroll_listener->self ,
				ml_term_get_rows( screen->term)) ;
		}
	}
}


/*
 * Utility function to execute both x_restore_selected_region_color() and x_window_update().
 */
static void
restore_selected_region_color_instantly(
	x_screen_t *  screen
	)
{
	if( x_restore_selected_region_color( &screen->sel))
	{
		x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;
	}
}


static void
write_to_pty(
	x_screen_t *  screen ,
	u_char *  str ,			/* str may be NULL */
	size_t  len ,
	mkf_parser_t *  parser		/* parser may be NULL */
	)
{
	if( parser && str)
	{
		(*parser->init)( parser) ;
		(*parser->set_str)( parser , str , len) ;
	}

	ml_term_init_encoding_conv( screen->term) ;

	if( parser)
	{
		u_char  conv_buf[512] ;
		u_char *  p ;
		size_t  filled_len ;

		p = conv_buf ;

	#ifdef  __DEBUG
		{
			size_t  i ;

			kik_debug_printf( KIK_DEBUG_TAG " written str:\n") ;
			for( i = 0 ; i < len ; i ++)
			{
				kik_msg_printf( "[%.2x]" , str[i]) ;
			}
			kik_msg_printf( "=>\n") ;
		}
	#endif

		while( ! parser->is_eos)
		{
			if( ( filled_len = ml_term_convert_to( screen->term ,
						conv_buf , sizeof( conv_buf) , parser)) == 0)
			{
				break ;
			}

		#ifdef  __DEBUG
			{
				size_t  i ;

				for( i = 0 ; i < filled_len ; i ++)
				{
					kik_msg_printf( "[%.2x]" , conv_buf[i]) ;
				}
			}
		#endif

			ml_term_write( screen->term , conv_buf , filled_len , 0) ;
		}
	}
	else if( str)
	{
	#ifdef  __DEBUG
		{
			size_t  i ;

			kik_debug_printf( KIK_DEBUG_TAG " written str: ") ;
			for( i = 0 ; i < len ; i ++)
			{
				kik_msg_printf( "%.2x" , str[i]) ;
			}
			kik_msg_printf( "\n") ;
		}
	#endif

		ml_term_write( screen->term , str , len , 0) ;
	}
	else
	{
		return ;
	}
}


static int
set_wall_picture(
	x_screen_t *  screen
	)
{
	x_picture_t *  pic ;

	if( ! screen->pic_file_path)
	{
		return  0 ;
	}

	if( ! ( pic = x_acquire_bg_picture( &screen->window ,
			x_screen_get_picture_modifier( screen) , screen->pic_file_path)))
	{
		kik_msg_printf( "Wall picture file %s is not found.\n" ,
			screen->pic_file_path) ;

		free( screen->pic_file_path) ;
		screen->pic_file_path = NULL ;

		x_window_unset_wall_picture( &screen->window) ;

		return  0 ;
	}

	if( ! x_window_set_wall_picture( &screen->window , pic->pixmap))
	{
		x_release_picture( pic) ;
		
		/* Because picture is loaded successfully, screen->pic_file_path retains. */

		return  0 ;
	}

	if( screen->bg_pic)
	{
		x_release_picture( screen->bg_pic) ;
	}

	screen->bg_pic = pic ;
	
	return  1 ;
}

static int
set_icon(
	x_screen_t *  screen
	)
{
	x_icon_picture_t *  icon ;
	char *  path ;

	if( ( path = ml_term_icon_path( screen->term)))
	{
		if( screen->icon && strcmp( path , screen->icon->file_path) == 0)
		{
			/* Not changed. */
			return  0 ;
		}

		if( ( icon = x_acquire_icon_picture( screen->window.disp , path)))
		{
			x_window_set_icon( &screen->window , icon) ;
		}
		else
		{
			x_window_remove_icon( &screen->window) ;
		}
	}
	else
	{
		if( screen->icon == NULL)
		{
			/* Not changed. */
			return  0 ;
		}

		icon = NULL ;
		x_window_remove_icon( &screen->window) ;
	}

	if( screen->icon)
	{
		x_release_icon_picture( screen->icon) ;
	}
	
	screen->icon = icon ;

	return  1 ;
}


/* referred in update_special_visual. */
static void  change_font_present( x_screen_t *  screen , x_type_engine_t  type_engine ,
					x_font_present_t  font_present) ;

static int
update_special_visual(
	x_screen_t *  screen
	)
{
	x_font_present_t  font_present ;

	if( ! ml_term_update_special_visual( screen->term))
	{
		/* If special visual is not changed, following processing is not necessary. */
		return  0 ;
	}

	font_present = x_get_font_present( screen->font_man) ;

	/* Similar if-else conditions exist in ml_term_update_special_visual. */
	if( ml_term_get_vertical_mode( screen->term))
	{
		font_present |= FONT_VERTICAL ;
	}
	else
	{
		font_present &= ~FONT_VERTICAL ;
	}

	change_font_present( screen , x_get_type_engine( screen->font_man) , font_present) ;

	return  1 ;
}


/*
 * callbacks of x_window events
 */

static void  xterm_set_window_name( void *  p , u_char *  name) ;

static void
window_realized(
	x_window_t *  win
	)
{
	x_screen_t *  screen ;
	char *  name ;

	screen = (x_screen_t*) win ;

	x_window_set_type_engine( win , x_get_type_engine( screen->font_man)) ;

	screen->mod_meta_mask = x_window_get_mod_meta_mask( win , screen->mod_meta_key) ;
	screen->mod_ignore_mask = x_window_get_mod_ignore_mask( win , NULL) ;

#if  defined(USE_WIN32GUI) && ! defined(DISABLE_XDND)
	DragAcceptFiles( win->my_window , TRUE) ;
#endif

	if( screen->input_method)
	{
		/* XIM or other input methods? */
		if( strncmp( screen->input_method , "xim" , 3) == 0)
		{
			activate_xic( screen) ;
		}
		else
		{
			x_xic_activate( &screen->window , "none" , "") ;

			if( ! ( screen->im = x_im_new(
					ml_term_get_encoding( screen->term) ,
					&screen->im_listener ,
					screen->input_method ,
					screen->mod_ignore_mask)))
			{
				free( screen->input_method) ;
				screen->input_method = NULL ;
			}
		}
	}

	x_window_set_fg_color( win , x_get_xcolor( screen->color_man , ML_FG_COLOR)) ;
	x_window_set_bg_color( win , x_get_xcolor( screen->color_man , ML_BG_COLOR)) ;

	x_get_xcolor_rgba( &screen->pic_mod.blend_red , &screen->pic_mod.blend_green ,
			&screen->pic_mod.blend_blue , NULL ,
			x_get_xcolor( screen->color_man , ML_BG_COLOR)) ;
	
	if( ( name = ml_term_window_name( screen->term)))
	{
		xterm_set_window_name( screen , name) ;
	}

	if( ( name = ml_term_icon_name( screen->term)))
	{
		x_set_icon_name( &screen->window , name) ;
	}

	set_icon( screen) ;
	
	if( screen->borderless)
	{
		x_window_set_borderless_flag( &screen->window , 1) ;
	}

	set_wall_picture( screen) ;
}

static void
window_exposed(
	x_window_t *  win ,
	int  x ,
	int  y ,
	u_int  width ,
	u_int  height
	)
{
	int  beg_row ;
	int  end_row ;
	x_screen_t *  screen ;

	screen = (x_screen_t *) win ;

	if( ml_term_get_vertical_mode( screen->term))
	{
		u_int  ncols ;

		ncols = ml_term_get_cols( screen->term) ;

		if( ( beg_row = x / x_col_width( screen)) >= ncols)
		{
			beg_row = ncols - 1 ;
		}

		if( ( end_row = (x + width) / x_col_width( screen) + 1) >= ncols)
		{
			end_row = ncols - 1 ;
		}

		if( ml_term_get_vertical_mode( screen->term) & VERT_RTL)
		{
			u_int  swp ;

			swp = ncols - beg_row - 1 ;
			beg_row = ncols - end_row - 1 ;
			end_row = swp ;
		}

	#ifdef  __DEBUG
		kik_debug_printf( KIK_DEBUG_TAG
			" exposed [row] from %d to %d [x] from %d to %d\n" ,
			beg_row , end_row , x , x + width) ;
	#endif
	}
	else
	{
		beg_row = convert_y_to_row( screen , NULL , y) ;
		end_row = convert_y_to_row( screen , NULL , y + height) ;

	#ifdef  __DEBUG
		kik_debug_printf( KIK_DEBUG_TAG
			" exposed [row] from %d to %d [y] from %d to %d\n" ,
			beg_row , end_row , y , y + height) ;
	#endif
	}

	/*
	 * XXX
	 * ml_term_set_modified_region_in_screen() is not used here to
	 * simplify logic.
	 */
	ml_term_set_modified_lines_in_screen( screen->term , beg_row , end_row) ;
	
	redraw_screen( screen) ;

	if( beg_row <= ml_term_cursor_row_in_screen( screen->term) &&
	    ml_term_cursor_row_in_screen( screen->term) <= end_row)
	{
		highlight_cursor( screen) ;
	}
}

static void
update_window(
	x_window_t *  win ,
	int  flag
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t*)win ;

	if( flag & UPDATE_SCREEN)
	{
		redraw_screen( screen) ;
	}

	if( flag & UPDATE_CURSOR)
	{
		highlight_cursor( screen) ;
	}
}

static void
window_resized(
	x_window_t *  win
	)
{
	x_screen_t *  screen ;
	u_int  rows ;
	u_int  cols ;
	u_int  width ;
	u_int  height ;

#ifdef  __DEBUG
	kik_debug_printf( KIK_DEBUG_TAG " term screen resized => width %d height %d.\n" ,
		win->width , win->height) ;
#endif

	screen = (x_screen_t *) win ;

	/* This is necessary since ml_term_t size is changed. */
	x_stop_selecting( &screen->sel) ;
	x_restore_selected_region_color( &screen->sel) ;
	exit_backscroll_mode( screen) ;

	unhighlight_cursor( screen , 1) ;

	/*
	 * visual width/height => logical cols/rows
	 */

	width = (screen->window.width * 100) / screen->screen_width_ratio ;
	height = (screen->window.height * 100) / screen->screen_height_ratio ;

	if( ml_term_get_vertical_mode( screen->term))
	{
		rows = width / x_col_width( screen) ;
		cols = height / x_line_height( screen) ;
	}
	else
	{
		cols = width / x_col_width( screen) ;
		rows = height / x_line_height( screen) ;
	}

	ml_term_resize( screen->term , cols , rows) ;

	set_wall_picture( screen) ;

	x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;

	x_xic_resized( &screen->window) ;
}

static void
window_focused(
	x_window_t *  win
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t *) win ;

	if( screen->fade_ratio != 100)
	{
		if( x_color_manager_unfade( screen->color_man))
		{
			x_window_set_fg_color( &screen->window ,
				x_get_xcolor( screen->color_man , ML_FG_COLOR)) ;
			x_window_set_bg_color( &screen->window ,
				x_get_xcolor( screen->color_man , ML_BG_COLOR)) ;

			ml_term_set_modified_all_lines_in_screen( screen->term) ;

			x_window_update( &screen->window, UPDATE_SCREEN) ;
		}
	}

	x_window_update( &screen->window, UPDATE_CURSOR) ;

	if( screen->im)
	{
		(*screen->im->focused)( screen->im) ;
	}

	if( ml_term_want_focus_event( screen->term))
	{
		write_to_pty( screen , "\x1b[I" , 3 , NULL) ;
	}
}

static void
window_unfocused(
	x_window_t *  win
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t *) win ;

	if( screen->fade_ratio != 100)
	{
		if( x_color_manager_fade( screen->color_man , screen->fade_ratio))
		{
			x_window_set_fg_color( &screen->window ,
				x_get_xcolor( screen->color_man , ML_FG_COLOR)) ;
			x_window_set_bg_color( &screen->window ,
				x_get_xcolor( screen->color_man , ML_BG_COLOR)) ;

			ml_term_set_modified_all_lines_in_screen( screen->term) ;

			x_window_update( &screen->window, UPDATE_SCREEN) ;
		}
	}

	x_window_update( &screen->window, UPDATE_CURSOR) ;

	if( screen->im)
	{
		(*screen->im->unfocused)( screen->im) ;
	}

	if( ml_term_want_focus_event( screen->term))
	{
		write_to_pty( screen , "\x1b[O" , 3 , NULL) ;
	}
}

/*
 * the finalizer of x_screen_t.
 *
 * x_display_close or x_display_remove_root -> x_window_final -> window_finalized
 */
static void
window_finalized(
	x_window_t *  win
	)
{
	x_screen_delete( (x_screen_t*)win) ;
}

static void
window_deleted(
	x_window_t *  win
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t*) win ;

	if( HAS_SYSTEM_LISTENER(screen,close_screen))
	{
		(*screen->system_listener->close_screen)(
			screen->system_listener->self , screen) ;
	}
}

static void
mapping_notify(
	x_window_t *  win
	)
{
	x_screen_t *  screen ;
	screen = (x_screen_t*) win ;

	screen->mod_meta_mask = x_window_get_mod_meta_mask( win, screen->mod_meta_key) ;
	screen->mod_ignore_mask = x_window_get_mod_ignore_mask( win, NULL) ;
}

static void
config_menu(
	x_screen_t *  screen ,
	int  x ,
	int  y ,
	char *  conf_menu_path
	)
{
	int  global_x ;
	int  global_y ;
	Window  child ;

	x_window_translate_coordinates( &screen->window, x, y, &global_x, &global_y, &child) ;

	/* XXX I don't know why but XGrabPointer() in child processes fails without this. */
	x_window_ungrab_pointer( &screen->window) ;

	ml_term_start_config_menu( screen->term , conf_menu_path , global_x , global_y ,
		DisplayString( screen->window.disp->display)) ;
}

static void
open_button3_command(
	x_screen_t *  screen
	)
{
	size_t  cmd_len ;
	char *  key ;
	size_t  key_len ;

	if( screen->sel.sel_str == NULL || screen->sel.sel_len == 0)
	{
		return ;
	}

	cmd_len = strlen( button3_command) + 1 ;
	
	key_len = cmd_len + screen->sel.sel_len * MLCHAR_UTF_MAX_SIZE + 1 ;
	key = alloca( key_len) ;

	strcpy( key , button3_command) ;
	key[cmd_len - 1] = ' ' ;

	(*screen->ml_str_parser->init)( screen->ml_str_parser) ;
	ml_str_parser_set_str( screen->ml_str_parser , screen->sel.sel_str , screen->sel.sel_len) ;

	ml_term_init_encoding_conv( screen->term) ;
	key_len = ml_term_convert_to( screen->term , key + cmd_len , key_len - cmd_len ,
				screen->ml_str_parser) + cmd_len ;
	key[key_len] = '\0' ;

	if( strncmp( key , "mlclient" , 8) == 0)
	{
		x_screen_exec_cmd( screen , key) ;
	}
#ifndef  USE_WIN32API
	else
	{
		char **  argv ;
		int  argc ;

		argv = kik_arg_str_to_array( &argc , key) ;

		if( fork() == 0)
		{
			/* child process */
			execvp( argv[0] , argv) ;
			exit( 1) ;
		}
	}
#endif
}

static int
use_utf_selection(
	x_screen_t *  screen
	)
{
	ml_char_encoding_t  encoding ;

	encoding = ml_term_get_encoding( screen->term) ;

	if( encoding == UTF8)
	{
		return  1 ;
	}
	else if( IS_UCS_SUBSET_ENCODING(encoding) && screen->receive_string_via_ucs)
	{
		return  1 ;
	}
	else
	{
		return  0 ;
	}
}

#ifdef  NL_TO_CR_IN_PAST_TEXT
static void
convert_nl_to_cr1(
	u_char *  str ,
	size_t  len
	)
{
	size_t  count ;

	for( count = 0 ; count < len ; count ++)
	{
		if( str[count] == '\n')
		{
			str[count] = '\r' ;
		}
	}
}

static void
convert_nl_to_cr2(
	ml_char_t *  str ,
	u_int  len
	)
{
	u_int  count ;

	for( count = 0 ; count < len ; count ++)
	{
		if( ml_char_bytes_is( &str[count] , "\n" , 1 , US_ASCII))
		{
			ml_char_set_bytes( &str[count] , "\r") ;
		}
	}
}
#endif

static int
yank_event_received(
	x_screen_t *  screen ,
	Time  time
	)
{
	if( screen->window.is_sel_owner)
	{
		if( screen->sel.sel_str == NULL || screen->sel.sel_len == 0)
		{
			return  0 ;
		}

	#ifdef  NL_TO_CR_IN_PAST_TEXT
		/*
		 * Convert normal newline chars to carriage return chars which are
		 * common return key sequences.
		 */
		convert_nl_to_cr2( screen->sel.sel_str , screen->sel.sel_len) ;
	#endif

		(*screen->ml_str_parser->init)( screen->ml_str_parser) ;
		ml_str_parser_set_str( screen->ml_str_parser ,
			screen->sel.sel_str , screen->sel.sel_len) ;

		if( ml_term_is_bracketed_paste_mode( screen->term))
		{
			write_to_pty( screen , "\x1b[200~" , 6 , NULL) ;
		}

		write_to_pty( screen , NULL , 0 , screen->ml_str_parser) ;

		if( ml_term_is_bracketed_paste_mode( screen->term))
		{
			write_to_pty( screen , "\x1b[201~" , 6 , NULL) ;
		}

		return  1 ;
	}
	else
	{
		if( use_utf_selection(screen))
		{
			return  x_window_utf_selection_request( &screen->window , time) ;
		}
		else
		{
			return  x_window_xct_selection_request( &screen->window , time) ;
		}
	}
}

static int
receive_string_via_ucs(
	x_screen_t *  screen
	)
{
	ml_char_encoding_t  encoding ;

	encoding = ml_term_get_encoding( screen->term) ;

	if( IS_UCS_SUBSET_ENCODING(encoding) && screen->receive_string_via_ucs)
	{
		return  1 ;
	}
	else
	{
		return  0 ;
	}
}

/* referred in key_pressed. */
static void change_im( x_screen_t * , char *) ;
static int compare_key_state_with_modmap( void *  p , u_int  state ,
	int *  is_shift , int *  is_lock , int *  is_ctl , int *  is_alt ,
	int *  is_meta , int *  is_numlock , int *  is_super , int *  is_hyper) ;
static void xterm_set_selection( void *  p , ml_char_t *  str ,	u_int  len) ;

typedef struct ksym_conv
{
	KeySym  before ;
	KeySym  after ;

} ksym_conv_t ;

static KeySym
convert_ksym(
	KeySym  ksym ,
	ksym_conv_t *  table ,
	u_int  table_size
	)
{
	u_int  count ;

	for( count = 0 ; count < table_size ; count++)
	{
		if( table[count].before == ksym)
		{
			return  table[count].after ;
		}
	}

	/* Not converted. */
	return  ksym ;
}

static void
key_pressed(
	x_window_t *  win ,
	XKeyEvent *  event
	)
{
	x_screen_t *  screen ;
	size_t  size ;
	u_char  ch[UTF_MAX_SIZE] ;
	u_char *  kstr ;
	KeySym  ksym ;
	mkf_parser_t *  parser ;
	u_int  masked_state ;

	screen = (x_screen_t *) win ;

	masked_state = event->state & screen->mod_ignore_mask ;

	if( ( size = x_window_get_str( win , ch , sizeof(ch) , &parser , &ksym , event))
	    > sizeof(ch))
	{
		if( ! ( kstr = alloca( size)))
		{
			return ;
		}

		size = x_window_get_str( win , kstr , size , &parser , &ksym , event) ;
	}
	else
	{
		kstr = ch ;
	}

#if  0
	kik_debug_printf( "state %x %x ksym %x str ", event->state , masked_state , ksym) ;
	{
		size_t  i ;
		for( i = 0 ; i < size ; i++)
		{
			kik_msg_printf( "%c", kstr[i]) ;
		}
		kik_msg_printf( " hex ") ;
		for( i = 0 ; i < size ; i++)
		{
			kik_msg_printf( "%x", kstr[i]) ;
		}
		kik_msg_printf( "\n") ;
	}
#endif

	if( screen->im)
	{
		u_char  kchar = 0 ;

		if( x_shortcut_match( screen->shortcut , IM_HOTKEY , ksym , masked_state) ||
		    /* for backward compatibility */
		    x_shortcut_match( screen->shortcut , EXT_KBD , ksym , masked_state))
		{
			if( (*screen->im->switch_mode)( screen->im))
			{
				return ;
			}
		}

		if( size == 1)
		{
			kchar = kstr[0] ;
		}
	#if  defined(USE_WIN32GUI) && defined(UTF16_IME_CHAR)
		else if( size == 2 && kstr[0] == 0)
		{
			/* UTF16BE */
			kchar = kstr[1] ;
		}
	#endif

		if( ! (*screen->im->key_event)( screen->im , kchar , ksym , event))
		{
			if( ml_term_is_backscrolling( screen->term))
			{
				exit_backscroll_mode( screen) ;
				x_window_update( &screen->window, UPDATE_SCREEN) ;
			}

			return ;
		}
	}

#ifdef  __DEBUG
	{
		int  i ;

		kik_debug_printf( KIK_DEBUG_TAG " received sequence =>") ;
		for( i = 0 ; i < size ; i ++)
		{
			kik_msg_printf( "%.2x" , kstr[i]) ;
		}
		kik_msg_printf( "\n") ;
	}
#endif

	if( x_shortcut_match( screen->shortcut , OPEN_SCREEN , ksym , masked_state))
	{
		if( HAS_SYSTEM_LISTENER(screen,open_screen))
		{
			(*screen->system_listener->open_screen)(
				screen->system_listener->self , screen) ;
		}

		return ;
	}
	else if( x_shortcut_match( screen->shortcut , OPEN_PTY , ksym , masked_state))
	{
		if( HAS_SYSTEM_LISTENER(screen,open_pty))
		{
			(*screen->system_listener->open_pty)(
				screen->system_listener->self , screen , NULL) ;
		}

		return ;
	}
	else if( x_shortcut_match( screen->shortcut , NEXT_PTY , ksym , masked_state))
	{
		if( HAS_SYSTEM_LISTENER(screen,next_pty))
		{
			(*screen->system_listener->next_pty)( screen->system_listener->self , screen) ;
		}

		return ;
	}
	else if( x_shortcut_match( screen->shortcut , PREV_PTY , ksym , masked_state))
	{
		if( HAS_SYSTEM_LISTENER(screen,prev_pty))
		{
			(*screen->system_listener->prev_pty)( screen->system_listener->self , screen) ;
		}

		return ;
	}
	/* for backward compatibility */
	else if( x_shortcut_match( screen->shortcut , EXT_KBD , ksym , masked_state))
	{
		change_im( screen , "kbd") ;

		return ;
	}
	else if( x_shortcut_match( screen->shortcut , SWITCH_OSC52 , ksym , masked_state))
	{
		if( screen->xterm_listener.set_selection)
		{
			screen->xterm_listener.set_selection = NULL ;
		}
		else
		{
			screen->xterm_listener.set_selection = xterm_set_selection ;
		}

		return ;
	}
#ifdef  DEBUG
	else if( x_shortcut_match( screen->shortcut , EXIT_PROGRAM , ksym , masked_state))
	{
		if( HAS_SYSTEM_LISTENER(screen,exit))
		{
			(*screen->system_listener->exit)( screen->system_listener->self , 1) ;
		}

		return ;
	}
#endif

	if( ml_term_is_backscrolling( screen->term))
	{
		if( screen->use_extended_scroll_shortcut)
		{
			if( x_shortcut_match( screen->shortcut , SCROLL_UP , ksym , masked_state))
			{
				bs_scroll_downward( screen) ;

				return ;
			}
			else if( x_shortcut_match( screen->shortcut , SCROLL_DOWN , ksym , masked_state))
			{
				bs_scroll_upward( screen) ;

				return ;
			}
		#if  1
			else if( ksym == 'u' || ksym == XK_Prior || ksym == XK_KP_Prior)
			{
				bs_half_page_downward( screen) ;

				return ;
			}
			else if( ksym == 'd' || ksym == XK_Next || ksym == XK_KP_Next)
			{
				bs_half_page_upward( screen) ;

				return ;
			}
			else if( ksym == 'k' || ksym == XK_Up || ksym == XK_KP_Up)
			{
				bs_scroll_downward( screen) ;

				return ;
			}
			else if( ksym == 'j' || ksym == XK_Down || ksym == XK_KP_Down)
			{
				bs_scroll_upward( screen) ;

				return ;
			}
		#endif
		}

		if( x_shortcut_match( screen->shortcut , PAGE_UP , ksym , masked_state))
		{
			bs_half_page_downward( screen) ;

			return ;
		}
		else if( x_shortcut_match( screen->shortcut , PAGE_DOWN , ksym , masked_state))
		{
			bs_half_page_upward( screen) ;

			return ;
		}
		else if( ksym == XK_Shift_L || ksym == XK_Shift_R || ksym == XK_Control_L ||
			ksym == XK_Control_R || ksym == XK_Caps_Lock || ksym == XK_Shift_Lock ||
			ksym == XK_Meta_L || ksym == XK_Meta_R || ksym == XK_Alt_L ||
			ksym == XK_Alt_R || ksym == XK_Super_L || ksym == XK_Super_R ||
			ksym == XK_Hyper_L || ksym == XK_Hyper_R || ksym == XK_Escape)
		{
			/* any modifier keys(X11/keysymdefs.h) */

			return ;
		}
		else
		{
			exit_backscroll_mode( screen) ;
		}
	}

	if( screen->use_extended_scroll_shortcut &&
		x_shortcut_match( screen->shortcut , SCROLL_UP , ksym , masked_state))
	{
		enter_backscroll_mode( screen) ;
		bs_scroll_downward( screen) ;
	}
	else if( x_shortcut_match( screen->shortcut , PAGE_UP , ksym , masked_state))
	{
		enter_backscroll_mode( screen) ;
		bs_half_page_downward( screen) ;
	}
	else if( x_shortcut_match( screen->shortcut , PAGE_DOWN , ksym , masked_state))
	{
		/* do nothing */
	}
	else if( x_shortcut_match( screen->shortcut , INSERT_SELECTION , ksym , masked_state))
	{
		yank_event_received( screen , CurrentTime) ;
	}
	else
	{
		int  is_app_keypad ;
		int  is_app_cursor_keys ;
		char *  buf ;
		/*
		 *    intermediate_ch
		 *      /
		 * ESC [ Ps ; Ps ~
		 *        \    \  \
		 *       param  \  final_ch
		 *             modcode
		 */
		char  buf_escseq[15] ;
		char  intermediate_ch ;
		char  final_ch ;
		int  param ;
		int  modcode ;
		#define KEY_ESCSEQ( i , p , f) do {	\
			intermediate_ch = (i) ;		\
			param = ( p) ;			\
			final_ch = ( f) ;		\
		} while ( 0)

		intermediate_ch = 0 ;
		modcode = 0 ;

		is_app_cursor_keys = ml_term_is_app_cursor_keys( screen->term) ;
		is_app_keypad = ml_term_is_app_keypad( screen->term) ;

		if( event->state)	/* Check unmasked (raw) state of event. */
		{
			int  is_shift ;
			int  is_meta ;
			int  is_alt ;
			int  is_ctl ;
			int  is_numlock ;

			if( compare_key_state_with_modmap( screen , event->state ,
						       &is_shift , NULL ,
						       &is_ctl , &is_alt ,
						       &is_meta , &is_numlock , NULL ,
						       NULL) &&
			    /* compatible with xterm (Modifier codes in input.c) */
			    ( modcode = (is_shift ? 1 : 0) + (is_alt   ? 2 : 0) +
						(is_ctl   ? 4 : 0) + (is_meta  ? 8 : 0)))
			{
				modcode++ ;

				if( ml_term_modify_other_keys(screen->term) == 2)
				{
					if( size == 1
				#if  defined(USE_WIN32GUI) && defined(UTF16_IME_CHAR)
					    || ( size == 2 && kstr[0] == 0)
				#endif
					    )
					{
						u_char  kchar ;

						if( ( kchar = kstr[size - 1]) < 0x20)
						{
							kchar += (is_shift ? 0 : 0x20) +
							         (is_ctl ? 0x40 : 0) ;
						}

						KEY_ESCSEQ( '[' , kchar , 'u') ;

						goto  write_buf ;
					}
				}
			}
			
			if( is_numlock)
			{
				is_app_keypad = 0 ;
			}
		}
		
		if( screen->use_vertical_cursor)
		{
			if( ml_term_get_vertical_mode( screen->term) & VERT_RTL)
			{
				ksym_conv_t  table[] =
				{
					{ XK_Up	,	XK_Left , } ,
					{ XK_KP_Up ,	XK_KP_Left , } ,
					{ XK_Down ,	XK_Right , } ,
					{ XK_KP_Down ,	XK_KP_Right , } ,
					{ XK_Left ,	XK_Down , } ,
					{ XK_KP_Left ,	XK_KP_Down , } ,
					{ XK_Right ,	XK_Up , } ,
					{ XK_KP_Right ,	XK_KP_Up , } ,
				} ;

				ksym = convert_ksym( ksym , table ,
						sizeof(table) / sizeof(table[0])) ;
			}
			else if( ml_term_get_vertical_mode( screen->term) & VERT_LTR)
			{
				ksym_conv_t  table[] =
				{
					{ XK_Up	,	XK_Left , } ,
					{ XK_KP_Up ,	XK_KP_Left , } ,
					{ XK_Down ,	XK_Right , } ,
					{ XK_KP_Down ,	XK_KP_Right , } ,
					{ XK_Left ,	XK_Up , } ,
					{ XK_KP_Left ,	XK_KP_Up , } ,
					{ XK_Right ,	XK_Down , } ,
					{ XK_KP_Right ,	XK_KP_Down , } ,
				} ;

				ksym = convert_ksym( ksym , table ,
						sizeof(table) / sizeof(table[0])) ;
			}
		}

		if( IsKeypadKey( ksym))
		{
			if( is_app_keypad)
			{
				if( ksym == XK_KP_Multiply)
				{
					KEY_ESCSEQ( 'O' , 0 , 'j') ;
				}
				else if( ksym == XK_KP_Add)
				{
					KEY_ESCSEQ( 'O' , 0 , 'k') ;
				}
				else if( ksym == XK_KP_Separator)
				{
					KEY_ESCSEQ( 'O' , 0 , 'l') ;
				}
				else if( ksym == XK_KP_Subtract)
				{
					KEY_ESCSEQ( 'O' , 0 , 'm') ;
				}
				else if( (ksym == XK_KP_Decimal || ksym == XK_KP_Delete))
				{
					KEY_ESCSEQ( 'O' , 0 , 'n') ;
				}
				else if( ksym == XK_KP_Divide)
				{
					KEY_ESCSEQ( 'O' , 0 , 'o') ;
				}
				else if( ksym == XK_KP_F1)
				{
					KEY_ESCSEQ( 'O' , 0 , 'P') ;
				}
				else if( ksym == XK_KP_F2)
				{
					KEY_ESCSEQ( 'O' , 0 , 'Q') ;
				}
				else if( ksym == XK_KP_F3)
				{
					KEY_ESCSEQ( 'O' , 0 , 'R') ;
				}
				else if( ksym == XK_KP_F4)
				{
					KEY_ESCSEQ( 'O' , 0 , 'S') ;
				}
				else if( ksym == XK_KP_Insert)
				{
					KEY_ESCSEQ( 'O' , 0 , 'p') ;
				}
				else if( ksym == XK_KP_End)
				{
					KEY_ESCSEQ( 'O' , 0 , 'q') ;
				}
				else if( ksym == XK_KP_Down)
				{
					KEY_ESCSEQ( 'O' , 0 , 'r') ;
				}
				else if( ksym == XK_KP_Next)
				{
					KEY_ESCSEQ( 'O' , 0 , 's') ;
				}
				else if( ksym == XK_KP_Left)
				{
					KEY_ESCSEQ( 'O' , 0 , 't') ;
				}
				else if( ksym == XK_KP_Begin)
				{
					KEY_ESCSEQ( 'O' , 0 , 'u') ;
				}
				else if( ksym == XK_KP_Right)
				{
					KEY_ESCSEQ( 'O' , 0 , 'v') ;
				}
				else if( ksym == XK_KP_Home)
				{
					KEY_ESCSEQ( 'O' , 0 , 'w') ;
				}
				else if( ksym == XK_KP_Up)
				{
					KEY_ESCSEQ( 'O' , 0 , 'x') ;
				}
				else if( ksym == XK_KP_Prior)
				{
					KEY_ESCSEQ( 'O' , 0 , 'y') ;
				}
				else
				{
					goto  no_keypad ;
				}
				
				goto  write_buf ;
			}
			else
			{
				ksym_conv_t  table[] =
				{
					{ XK_KP_F1 ,	XK_F1 , } ,
					{ XK_KP_F2 ,	XK_F2 , } ,
					{ XK_KP_F3 ,	XK_F3 , } ,
					{ XK_KP_F4 ,	XK_F4 , } ,
					{ XK_KP_Insert , XK_Insert , } ,
					{ XK_KP_End ,	XK_End , } ,
					{ XK_KP_Down ,	XK_Down , } ,
					{ XK_KP_Next ,	XK_Next , } ,
					{ XK_KP_Left ,	XK_Left , } ,
					{ XK_KP_Begin ,	XK_Begin , } ,
					{ XK_KP_Right ,	XK_Right , } ,
					{ XK_KP_Home ,	XK_Home , } ,
					{ XK_KP_Up ,	XK_Up , } ,
					{ XK_KP_Prior ,	XK_Prior , } ,
				} ;

				ksym = convert_ksym( ksym , table ,
						sizeof(table) / sizeof(table[0])) ;
			}
		}

no_keypad:
		if( ( buf = x_shortcut_str( screen->shortcut , ksym , masked_state)))
		{
			/*
			 * Set 0 to ignore (screen->mod_meta_mask & event->state) check
			 * at write_buf.
			 */
			event->state = 0 ;

			if( strncmp( buf , "proto:" , 6) == 0)
			{
				size = 7 + strlen( buf + 6) + 2 ;
				if( ( kstr = alloca( size)))
				{
					sprintf( kstr , "\x1b]5379;%s\x07" , buf + 6) ;
					/* processing_vtseq == -1 means loopback processing. */
					screen->processing_vtseq = -1 ;
					ml_term_write_loopback( screen->term , kstr , size - 1) ;
					x_window_update( &screen->window ,
						UPDATE_SCREEN|UPDATE_CURSOR) ;
				}

				return  ;
			}
		}
		else if( ( ksym == XK_Delete
		#if ! defined(USE_WIN32GUI) && ! defined(USE_FRAMEBUFFER)
			&& size == 1
		#endif
			) || ksym == XK_KP_Delete)
		{
			if( modcode ||
			    ! (buf = x_termcap_get_str_field( screen->termcap , ML_DELETE)))
			{
				KEY_ESCSEQ( '[' , 3 , '~') ;
			}
		}
		/*
		 * XXX
		 * In some environment, if backspace(1) -> 0-9 or space(2) pressed continuously,
		 * ksym in (2) as well as (1) is XK_BackSpace.
		 */
		else if( ksym == XK_BackSpace && size == 1 && kstr[0] == 0x8)
		{
			if( ! (buf = x_termcap_get_str_field( screen->termcap , ML_BACKSPACE)))
			{
				buf = "\x08" ;
			}
		}
		else if( ksym == XK_Escape && ml_term_is_app_escape( screen->term))
		{
			buf = "\x1bO[" ;
		}
		else if( size > 0)
		{
			buf = NULL ;
		}
		/*
		 * following ksym is processed only if no key string is received
		 * (size == 0)
		 */
		else if( ksym == XK_Up)
		{
			KEY_ESCSEQ( is_app_cursor_keys ? 'O' : '[' ,
				    modcode ? 1 : 0 , 'A') ;
		}
		else if( ksym == XK_Down)
		{
			KEY_ESCSEQ( is_app_cursor_keys ? 'O' : '[' ,
				    modcode ? 1 : 0 , 'B') ;
		}
		else if( ksym == XK_Right)
		{
			KEY_ESCSEQ( is_app_cursor_keys ? 'O' : '[' ,
				    modcode ? 1 : 0 , 'C') ;
		}
		else if( ksym == XK_Left)
		{
			KEY_ESCSEQ( is_app_cursor_keys ? 'O' : '[' ,
				    modcode ? 1 : 0 , 'D') ;
		}
		else if( ksym == XK_Begin)
		{
			KEY_ESCSEQ( '[' , modcode ? 1 : 0 , 'E') ;
		}
		else if( ksym == XK_End)
		{
			if( modcode ||
			    ! is_app_cursor_keys ||
			    ! (buf = x_termcap_get_str_field( screen->termcap , ML_END)) )
			{
				KEY_ESCSEQ( (is_app_cursor_keys && ! modcode) ? 'O' : '[' ,
				            modcode ? 1 : 0 , 'F') ;
			}
		}
		else if( ksym == XK_Home)
		{
			if( modcode ||
			    ! is_app_cursor_keys ||
			    ! (buf = x_termcap_get_str_field( screen->termcap , ML_HOME)) )
			{
				KEY_ESCSEQ( (is_app_cursor_keys && ! modcode) ? 'O' : '[' ,
				            modcode ? 1 : 0 , 'H') ;
			}
		}
		else if( ksym == XK_Prior)
		{
			KEY_ESCSEQ( '[' , 5 , '~') ;
		}
		else if( ksym == XK_Next)
		{
			KEY_ESCSEQ( '[' , 6 , '~') ;
		}
		else if( ksym == XK_Insert)
		{
			KEY_ESCSEQ( '[' , 2 , '~') ;
		}
		else if( ksym == XK_Find)
		{
			KEY_ESCSEQ( '[' , 1 , '~') ;
		}
		else if( ksym == XK_Execute)
		{
			KEY_ESCSEQ( '[' , 3 , '~') ;
		}
		else if( ksym == XK_Select)
		{
			KEY_ESCSEQ( '[' , 4 , '~') ;
		}
	#ifdef  XK_ISO_Left_Tab
		else if( ksym == XK_ISO_Left_Tab)
		{
			KEY_ESCSEQ( '[' , 0 , 'Z') ;
			modcode = 0 ;
		}
	#endif
		else if( ksym == XK_F15 || ksym == XK_Help)
		{
			KEY_ESCSEQ( '[' , 28 , '~') ;
		}
		else if( ksym == XK_F16 || ksym == XK_Menu)
		{
			KEY_ESCSEQ( '[' , 29 , '~') ;
		}
		else if( XK_F1 <= ksym && ksym <= XK_FMAX)
		{
			if( ksym <= XK_F5)
			{
				if( modcode ||
				    ! (buf = x_termcap_get_str_field( screen->termcap ,
							ML_F1 + ksym - XK_F1)))
				{
					if( ksym == XK_F5)
					{
						/* 15 */
						KEY_ESCSEQ( '[' , 15 , '~') ;
					}
					else
					{
						/* PQRS */
						KEY_ESCSEQ( 'O' , modcode , (ksym - XK_F1) + 'P') ;

						/*
						 * Shift+F1 is not ^[O1;2P but ^[O2P.
						 * So 'modcode' is copied to 'param' varaiable
						 * above and then cleared to 0 here.
						 */
						modcode = 0 ;
					}
				}
			}
			else if( ksym <= XK_F10)
			{
				/* 17 - 21 */
				KEY_ESCSEQ( '[' , (ksym - XK_F6) + 17 , '~') ;
			}
			else if( ksym <= XK_F14)
			{
				/* 23 - 26 */
				KEY_ESCSEQ( '[' , (ksym - XK_F11) + 23 , '~') ;
			}
			else
		#if  XK_FMAX > XK_F20
			if( ksym <= XK_F20)
		#endif
			{
				/* 31 - 34 */
				KEY_ESCSEQ( '[' , (ksym - XK_F17) + 31 , '~') ;
			}
		#if  XK_FMAX > XK_F20
			else /* if( ksym <= XK_FMAX) */
			{
				/*
				 * X11: 42 - 56(F35)
				 * W32: 42 - 45(F24)
				 */
				KEY_ESCSEQ( '[' , (ksym - XK_F21) + 42 , '~') ;
			}
		#endif
		}
	#ifdef SunXK_F36
		else if( ksym == SunXK_F36)
		{
			KEY_ESCSEQ( '[' , 57 , '~') ;
		}
		else if( ksym == SunXK_F37)
		{
			KEY_ESCSEQ( '[' , 58 , '~') ;
		}
	#endif
		else
		{
			return ;
		}

write_buf:
		if( intermediate_ch)
		{
			if( modcode) /* ESC <intermediate> Ps ; Ps <final> */
			{
				kik_snprintf( buf_escseq , sizeof(buf_escseq) ,
					      "\x1b%c%d;%d%c" ,
					      intermediate_ch , param ,
					      modcode , final_ch) ;
			}
			else if( param) /* ESC <intermediate> Ps <final> */
			{
				kik_snprintf( buf_escseq , sizeof(buf_escseq) ,
					      "\x1b%c%d%c" ,
					      intermediate_ch , param ,
					      final_ch) ;
			}
			else /* ESC <intermediate> <final> */
			{
				kik_snprintf( buf_escseq , sizeof(buf_escseq) ,
					      "\x1b%c%c" ,
					      intermediate_ch , final_ch) ;
			}

			buf = buf_escseq ;
		}
		/* Check unmasked (raw) state of event. */
		else if( screen->mod_meta_mask & event->state)
		{
			if( screen->mod_meta_mode == MOD_META_OUTPUT_ESC)
			{
				write_to_pty( screen , "\x1b" , 1 , NULL) ;
			}
			else if( screen->mod_meta_mode == MOD_META_SET_MSB)
			{
				int  count ;

				for( count = 0 ; count < size ; count ++)
				{
					if( 0x20 <= kstr[count] && kstr[count] <= 0x7e)
					{
						kstr[count] |= 0x80 ;
					}
				}
				/* shouldn't try to parse the modified sequence */
				parser = NULL ;
			}
		}

		if( buf)
		{
			write_to_pty( screen , buf , strlen(buf) , NULL) ;
		}
		else
		{
			if( parser && receive_string_via_ucs(screen))
			{
				/* XIM Text -> UCS -> PTY ENCODING */

				u_char  conv_buf[512] ;
				size_t  filled_len ;

				(*parser->init)( parser) ;
				(*parser->set_str)( parser , kstr , size) ;

				(*screen->utf_conv->init)( screen->utf_conv) ;

				while( ! parser->is_eos)
				{
					if( ( filled_len = (*screen->utf_conv->convert)(
						screen->utf_conv , conv_buf , sizeof( conv_buf) ,
						parser)) == 0)
					{
						break ;
					}

					write_to_pty( screen , conv_buf , filled_len ,
						screen->utf_parser) ;
				}
			}
			else
			{
				write_to_pty( screen , kstr , size , parser) ;
			}
		}
	}
}

static void
selection_cleared(
	x_window_t *  win
	)
{
	if( x_sel_clear( &((x_screen_t*)win)->sel))
	{
		x_window_update( win , UPDATE_SCREEN|UPDATE_CURSOR) ;
	}
}

static size_t
convert_selection_to_xct(
	x_screen_t *  screen ,
	u_char *  str ,
	size_t  len
	)
{
	size_t  filled_len ;

#ifdef  __DEBUG
	{
		int  i ;

		kik_debug_printf( KIK_DEBUG_TAG " sending internal str: ") ;
		for( i = 0 ; i < screen->sel.sel_len ; i ++)
		{
			ml_char_dump( &screen->sel.sel_str[i]) ;
		}
		kik_msg_printf( "\n -> converting to ->\n") ;
	}
#endif

	(*screen->ml_str_parser->init)( screen->ml_str_parser) ;
	ml_str_parser_set_str( screen->ml_str_parser , screen->sel.sel_str , screen->sel.sel_len) ;

	(*screen->xct_conv->init)( screen->xct_conv) ;
	filled_len = (*screen->xct_conv->convert)( screen->xct_conv ,
		str , len , screen->ml_str_parser) ;

#ifdef  __DEBUG
	{
		int  i ;

		kik_debug_printf( KIK_DEBUG_TAG " sending xct str: ") ;
		for( i = 0 ; i < filled_len ; i ++)
		{
			kik_msg_printf( "%.2x" , str[i]) ;
		}
		kik_msg_printf( "\n") ;
	}
#endif

	return  filled_len ;
}

static size_t
convert_selection_to_utf(
	x_screen_t *  screen ,
	u_char *  str ,
	size_t  len
	)
{
	size_t  filled_len ;

#ifdef  __DEBUG
	{
		int  i ;

		kik_debug_printf( KIK_DEBUG_TAG " sending internal str: ") ;
		for( i = 0 ; i < screen->sel.sel_len ; i ++)
		{
			ml_char_dump( &screen->sel.sel_str[i]) ;
		}
		kik_msg_printf( "\n -> converting to ->\n") ;
	}
#endif

	(*screen->ml_str_parser->init)( screen->ml_str_parser) ;
	ml_str_parser_set_str( screen->ml_str_parser , screen->sel.sel_str , screen->sel.sel_len) ;

	(*screen->utf_conv->init)( screen->utf_conv) ;
	filled_len = (*screen->utf_conv->convert)( screen->utf_conv ,
		str , len , screen->ml_str_parser) ;

#ifdef  __DEBUG
	{
		int  i ;

		kik_debug_printf( KIK_DEBUG_TAG " sending utf str: ") ;
		for( i = 0 ; i < filled_len ; i ++)
		{
			kik_msg_printf( "%.2x" , str[i]) ;
		}
		kik_msg_printf( "\n") ;
	}
#endif

	return  filled_len ;
}

static void
xct_selection_requested(
	x_window_t * win ,
	XSelectionRequestEvent *  event ,
	Atom  type
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t*) win ;

	if( screen->sel.sel_str == NULL || screen->sel.sel_len == 0)
	{
		x_window_send_selection( win , event , NULL , 0 , 0 , 0) ;
	}
	else
	{
		u_char *  xct_str ;
		size_t  xct_len ;
		size_t  filled_len ;

		xct_len = screen->sel.sel_len * MLCHAR_XCT_MAX_SIZE ;

		if( ( xct_str = alloca( xct_len)) == NULL)
		{
			return ;
		}

		filled_len = convert_selection_to_xct( screen , xct_str , xct_len) ;

		x_window_send_selection( win , event , xct_str , filled_len , type , 8) ;
	}
}

static void
utf_selection_requested(
	x_window_t * win ,
	XSelectionRequestEvent *  event ,
	Atom  type
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t*) win ;

	if( screen->sel.sel_str == NULL || screen->sel.sel_len == 0)
	{
		x_window_send_selection( win , event , NULL , 0 , 0 , 0) ;
	}
	else
	{
		u_char *  utf_str ;
		size_t  utf_len ;
		size_t  filled_len ;

		utf_len = screen->sel.sel_len * MLCHAR_UTF_MAX_SIZE ;

		if( ( utf_str = alloca( utf_len)) == NULL)
		{
			return ;
		}

		filled_len = convert_selection_to_utf( screen , utf_str , utf_len) ;

		x_window_send_selection( win , event , utf_str , filled_len , type , 8) ;
	}
}

static void
xct_selection_notified(
	x_window_t *  win ,
	u_char *  str ,
	size_t  len
	)
{
	x_screen_t *  screen ;

#ifdef  NL_TO_CR_IN_PAST_TEXT
	/*
	 * Convert normal newline chars to carriage return chars which are
	 * common return key sequences.
	 */
	convert_nl_to_cr1( str , len) ;
#endif

	screen = (x_screen_t*) win ;

	if( ml_term_is_bracketed_paste_mode( screen->term))
	{
		write_to_pty( screen , "\x1b[200~" , 6 , NULL) ;
	}

	/* utf_parser is utf16le in win32. */
#ifndef  USE_WIN32GUI
	/*
	 * XXX
	 * parsing UTF-8 sequence designated by ESC % G.
	 */
	if( len > 3 && strncmp( str , "\x1b%G" , 3) == 0)
	{
	#if  0
		int  i;
		for( i = 0 ; i < len ; i ++)
		{
			kik_msg_printf( "%.2x " , str[i]) ;
		}
	#endif

		write_to_pty( screen , str + 3 , len - 3 , screen->utf_parser) ;
	}
	else
#endif
	if( receive_string_via_ucs(screen))
	{
		/* XCOMPOUND TEXT -> UCS -> PTY ENCODING */

		u_char  conv_buf[512] ;
		size_t  filled_len ;

		(*screen->xct_parser->init)( screen->xct_parser) ;
		(*screen->xct_parser->set_str)( screen->xct_parser , str , len) ;

		(*screen->utf_conv->init)( screen->utf_conv) ;

		while( ! screen->xct_parser->is_eos)
		{
			if( ( filled_len = (*screen->utf_conv->convert)(
				screen->utf_conv , conv_buf , sizeof( conv_buf) ,
				screen->xct_parser)) == 0)
			{
				break ;
			}

			write_to_pty( screen , conv_buf , filled_len , screen->utf_parser) ;
		}
	}
	else
	{
		/* XCOMPOUND TEXT -> PTY ENCODING */

		write_to_pty( screen , str , len , screen->xct_parser) ;
	}

	if( ml_term_is_bracketed_paste_mode( screen->term))
	{
		write_to_pty( screen , "\x1b[201~" , 6 , NULL) ;
	}
}

static void
utf_selection_notified(
	x_window_t *  win ,
	u_char *  str ,
	size_t  len
	)
{
	x_screen_t *  screen ;

#ifdef  NL_TO_CR_IN_PAST_TEXT
	/*
	 * Convert normal newline chars to carriage return chars which are
	 * common return key sequences.
	 */
	convert_nl_to_cr1( str , len) ;
#endif

	screen = (x_screen_t*) win ;

	if( ml_term_is_bracketed_paste_mode( screen->term))
	{
		write_to_pty( screen , "\x1b[200~" , 6 , NULL) ;
	}

	write_to_pty( screen , str , len , screen->utf_parser) ;

	if( ml_term_is_bracketed_paste_mode( screen->term))
	{
		write_to_pty( screen , "\x1b[201~" , 6 , NULL) ;
	}
}

#ifndef  DISABLE_XDND
static void
set_xdnd_config(
	x_window_t *  win ,
	char *  dev ,
	char *  key ,
	char *  value
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t*)win ;

	x_screen_set_config( screen , dev , key , value) ;

	x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;
}
#endif

static int
report_mouse_tracking(
	x_screen_t *  screen ,
	int  x ,
	int  y ,
	int  button ,
	int  state ,
	int  is_motion ,
	int  is_released
	)
{
	int  key_state ;
	ml_line_t *  line ;
	int  col ;
	int  row ;
	u_int  x_rest ;
	/*
	 * Max length is SGR style => ESC [ < %d ; %d(col) ; %d(row) ; %c('M' or 'm') NULL
	 *                            1   1 1 3  1 3       1  3      1 1              1
	 */
	u_char  seq[17] ;
	size_t  seq_len ;

	if( ( is_released && ml_term_get_extended_mouse_report_mode( screen->term) !=
				EXTENDED_MOUSE_REPORT_SGR) ||
	    ( is_motion && button == 0) )
	{
		/* ButtonRelease or PointerMotion */
		key_state = 0 ;
		button = 3 ;
	}
	else
	{
		/*
		 * Shift = 4
		 * Meta = 8
		 * Control = 16
		 * Button Motion = 32
		 *
		 * NOTE: with Ctrl/Shift, the click is interpreted as region selection at present.
		 * So Ctrl/Shift will never be catched here.
		 */
		key_state = ((state & ShiftMask) ? 4 : 0) +
				((state & screen->mod_meta_mask) ? 8 : 0) +
				((state & ControlMask) ? 16 : 0) +
				(is_motion /* && (state & (Button1Mask|Button2Mask|Button3Mask)) */
					? 32 : 0) ;

		if( is_released)
		{
			/* is EXTENDED_MOUSE_REPORT_SGR */
			key_state += 0x80 ;
		}

		/* if( button > 0) */
		{
			/* ButtonPress */
			button -= Button1 ;

			while( button >= 3)
			{
				/* Wheel mouse */
				key_state += 64 ;
				button -= 3 ;
			}
		}
	}

	if( ml_term_get_vertical_mode( screen->term))
	{
		col = convert_y_to_row( screen , NULL , y) ;

	#if  0
		if( x_is_using_multi_col_char( screen->font_man))
		{
			/*
			 * XXX
			 * col can be inaccurate if full width characters are used.
			 */
		}
	#endif

		if( ( line = ml_term_get_line_in_screen( screen->term , col)) == NULL)
		{
			return  0 ;
		}

		row = ml_convert_char_index_to_col( line ,
			convert_x_to_char_index_with_shape( screen , line , &x_rest , x) ,
			0) ;

		if( ml_term_get_vertical_mode( screen->term) & VERT_RTL)
		{
			row = ml_term_get_cols( screen->term) - row - 1 ;
		}

	#if  0
		if( x_is_using_multi_col_char( screen->font_man))
		{
			/*
			 * XXX
			 * row can be inaccurate if full width characters are used.
			 */
		}
	#endif
	}
	else
	{
		u_int  width ;
		int  char_index ;

		row = convert_y_to_row( screen , NULL , y) ;

		if( ( line = ml_term_get_line_in_screen( screen->term , row)) == NULL)
		{
			return  0 ;
		}

		char_index = convert_x_to_char_index_with_shape( screen , line , &x_rest , x) ;
		if( ml_line_is_rtl( line))
		{
			/* XXX */
			char_index = ml_line_convert_visual_char_index_to_logical( line ,
						char_index) ;
		}

		col = ml_convert_char_index_to_col( line , char_index , 0) ;

		width = x_calculate_char_width(
				x_get_font( screen->font_man , ml_char_font( ml_sp_ch())) ,
				ml_char_bytes( ml_sp_ch()) , 1 , US_ASCII , NULL) ;
		if( x_rest > width)
		{
			if( ( col += x_rest / width) >= ml_term_get_cols( screen->term))
			{
				col = ml_term_get_cols( screen->term) - 1 ;
			}
		}
	}

	/* count starts from 1, not 0 */
	col ++ ;
	row ++ ;

	/* clear all bytes of seq to compare with prev_mouse_report_seq. */
	memcpy( seq , "\x1b[M\0\0\0\0\0" , 8) ;

	seq[3] = 0x20 + button + key_state ;

	if( ml_term_get_extended_mouse_report_mode( screen->term))
	{
		int  ch ;
		u_char *  p ;

		p = seq + 4 ;

		if( col > EXT_MOUSE_POS_LIMIT)
		{
			col = EXT_MOUSE_POS_LIMIT ;
		}

		if( (ch = 0x20 + col) >= 0x80)
		{
			*(p ++) = ((ch >> 6) & 0x1f) | 0xc0 ;
			*(p ++) = (ch & 0x3f) | 0x80 ;
		}
		else
		{
			*(p ++) = ch ;
		}

		if( row > EXT_MOUSE_POS_LIMIT)
		{
			row = EXT_MOUSE_POS_LIMIT ;
		}

		if( (ch = 0x20 + row) >= 0x80)
		{
			*(p ++) = ((ch >> 6) & 0x1f) | 0xc0 ;
			*p = (ch & 0x3f) | 0x80 ;
		}
		else
		{
			*p = ch ;
		}

		seq_len = p - seq + 1 ;
	}
	else
	{
		seq[4] = 0x20 + (col < MOUSE_POS_LIMIT ? col : MOUSE_POS_LIMIT) ;
		seq[5] = 0x20 + (row < MOUSE_POS_LIMIT ? row : MOUSE_POS_LIMIT) ;
		seq_len = 6 ;
	}

	if( key_state >= 64 ||						/* Wheeling mouse */
	    memcmp( screen->prev_mouse_report_seq , seq + 3 , 5) != 0)	/* Position is changed */
	{
		memcpy( screen->prev_mouse_report_seq , seq + 3 , 5) ;

		if( ml_term_get_extended_mouse_report_mode( screen->term) >
			EXTENDED_MOUSE_REPORT_UTF8)
		{
			if( ml_term_get_extended_mouse_report_mode( screen->term) ==
				EXTENDED_MOUSE_REPORT_SGR)
			{
				sprintf( seq , "\x1b[<%d;%d;%d%c" ,
					(button + key_state) & 0x7f , col , row ,
					((button + key_state) & 0x80) ? 'm' : 'M') ;
			}
			else /* if( ml_term_get_extended_mouse_report_mode( screen->term) ==
					EXTENDED_MOUSE_REPORT_URXVT) */
			{
				sprintf( seq , "\x1b[%d;%d;%dM" ,
					0x20 + button + key_state , col , row) ;
			}

			seq_len = strlen( seq) ;
		}

		write_to_pty( screen , seq , seq_len , NULL) ;

	#ifdef  __DEBUG
		kik_debug_printf( KIK_DEBUG_TAG " [reported cursor pos] %d %d\n" , col , row) ;
	#endif
	}
#ifdef  __DEBUG
	else
	{
		kik_debug_printf( KIK_DEBUG_TAG
			" cursor pos %d %d is not changed and not reported.\n") ;
	}
#endif

	return  1 ;
}

/*
 * Functions related to selection.
 */

static void
start_selection(
	x_screen_t *  screen ,
	int  col_r ,
	int  row_r ,
	x_sel_type_t  type
	)
{
	int  col_l ;
	int  row_l ;
	ml_line_t *  line ;

	/* XXX */
	if( ml_term_get_vertical_mode( screen->term))
	{
		kik_msg_printf( "Not supported selection in vertical mode.\n") ;

		return ;
	}

	if( ( line = ml_term_get_line( screen->term , row_r)) == NULL || ml_line_is_empty( line))
	{
		return ;
	}

	if( ( ! ml_line_is_rtl( line) && col_r == 0) ||
		( ml_line_is_rtl( line) && abs( col_r) == ml_line_end_char_index( line)))
	{
		if( ( line = ml_term_get_line( screen->term , row_r - 1)) == NULL ||
			ml_line_is_empty( line))
		{
			/* XXX col_l can be underflowed, but anyway it works. */
			col_l = col_r - 1 ;
			row_l = row_r ;
		}
		else
		{
			if( ml_line_is_rtl( line))
			{
				col_l = 0 ;
			}
			else
			{
				col_l = ml_line_end_char_index( line) ;
			}
			row_l = row_r - 1 ;
		}
	}
	else
	{
		col_l = col_r - 1 ;
		row_l = row_r ;
	}

	if( x_start_selection( &screen->sel , col_l , row_l , col_r , row_r , type))
	{
		x_window_update( &screen->window, UPDATE_SCREEN) ;
	}
}

static void
selecting(
	x_screen_t *  screen ,
	int  char_index ,
	int  row
	)
{
	/* XXX */
	if( ml_term_get_vertical_mode( screen->term))
	{
		kik_msg_printf( "Not supported selection in vertical mode.\n") ;

		return ;
	}
	
	if( x_selected_region_is_changed( &screen->sel , char_index , row , 1) &&
	    x_selecting( &screen->sel , char_index , row))
	{
		x_window_update( &screen->window, UPDATE_SCREEN) ;
	}
}

static void
selecting_with_motion(
	x_screen_t *  screen ,
	int  x ,
	int  y ,
	Time  time
	)
{
	int  char_index ;
	int  row ;
	int  x_is_outside ;
	u_int  x_rest ;
	ml_line_t *  line ;

	if( x < 0)
	{
		x = 0 ;
		x_is_outside = 1 ;
	}
	else if( x > screen->window.width)
	{
		x = screen->window.width ;
		x_is_outside = 1 ;
	}
	else
	{
		x_is_outside = 0 ;
	}

	if( y < 0)
	{
		if( ml_term_get_num_of_logged_lines( screen->term) > 0)
		{
			if( ! ml_term_is_backscrolling( screen->term))
			{
				enter_backscroll_mode( screen) ;
			}

			bs_scroll_downward( screen) ;
		}

		y = 0 ;
	}
	else if( y > screen->window.height)
	{
		if( ml_term_is_backscrolling( screen->term))
		{
			bs_scroll_upward( screen) ;
		}

		y = screen->window.height - x_line_height( screen) ;
	}

	row = ml_term_convert_scr_row_to_abs( screen->term , convert_y_to_row( screen , NULL , y)) ;

	if( ( line = ml_term_get_line( screen->term , row)) == NULL)
	{
	#ifdef  DEBUG
		kik_warn_printf( KIK_DEBUG_TAG " line(%d) not found.\n" , row) ;
	#endif

		return ;
	}

	if( ml_line_is_empty( line))
	{
		selecting( screen , 0 , row) ;

		return ;
	}

	char_index = convert_x_to_char_index_with_shape( screen , line , &x_rest , x) ;

	if( char_index == ml_line_end_char_index( line) && x_rest > 0)
	{
		x_is_outside = 1 ;

		/* Inform ml_screen that the mouse position is outside of the line. */
		char_index ++ ;
	}

	if( ml_line_is_rtl( line))
	{
		char_index = -char_index ;
	}

	if( ! x_is_selecting( &screen->sel))
	{
		restore_selected_region_color_instantly( screen) ;

		if( ! x_window_set_selection_owner( &screen->window , time))
		{
			return ;
		}

		start_selection( screen , char_index , row , SEL_CHAR) ;
	}
	else
	{
		if( ! x_is_outside)
		{
			if( x_is_after_sel_right_base_pos( &screen->sel , char_index , row))
			{
				if( abs( char_index) > 0)
				{
					char_index -- ;
				}
			}
			else if( x_is_before_sel_left_base_pos( &screen->sel , char_index , row))
			{
				if( abs( char_index) < ml_line_end_char_index( line))
				{
					char_index ++ ;
				}
			}
		}

		selecting( screen , char_index , row) ;
	}
}

static void
selecting_word(
	x_screen_t *  screen ,
	int  x ,
	int  y ,
	Time  time
	)
{
	int  char_index ;
	int  row ;
	u_int  x_rest ;
	int  beg_row ;
	int  beg_char_index ;
	int  end_row ;
	int  end_char_index ;
	ml_line_t *  line ;

	row = ml_term_convert_scr_row_to_abs( screen->term , convert_y_to_row( screen , NULL , y)) ;

	if( ( line = ml_term_get_line( screen->term , row)) == NULL || ml_line_is_empty( line))
	{
		return ;
	}

	char_index = convert_x_to_char_index_with_shape( screen , line , &x_rest , x) ;

	if( ml_line_end_char_index( line) == char_index && x_rest > 0)
	{
		/* over end of line */

		return ;
	}

	if( ml_term_get_word_region( screen->term , &beg_char_index , &beg_row , &end_char_index ,
		&end_row , char_index , row) == 0)
	{
		return ;
	}

	if( ml_line_is_rtl( ml_term_get_line( screen->term , beg_row)))
	{
		if( x_is_selecting( &screen->sel))
		{
			beg_char_index = -beg_char_index ;
		}
		else
		{
			beg_char_index = -beg_char_index + 1 ;
		}
	}

	if( ml_line_is_rtl( ml_term_get_line( screen->term , end_row)))
	{
		end_char_index = -end_char_index ;
	}

	if( ! x_is_selecting( &screen->sel))
	{
		restore_selected_region_color_instantly( screen) ;

		if( ! x_window_set_selection_owner( &screen->window , time))
		{
			return ;
		}

		start_selection( screen , beg_char_index , beg_row , SEL_WORD) ;
		selecting( screen , end_char_index , end_row) ;
		x_sel_lock( &screen->sel) ;
	}
	else
	{
		if( beg_row == end_row &&
		    ml_line_is_rtl( ml_term_get_line( screen->term , beg_row)))
		{
			int  tmp ;

			tmp = end_char_index ;
			end_char_index = beg_char_index ;
			beg_char_index = tmp ;
		}

		if( x_is_before_sel_left_base_pos( &screen->sel , beg_char_index , beg_row))
		{
			selecting( screen , beg_char_index , beg_row) ;
		}
		else
		{
			selecting( screen , end_char_index , end_row) ;
		}
	}
}

static void
selecting_line(
	x_screen_t *  screen ,
	int  y ,
	Time  time
	)
{
	int  row ;
	int  beg_char_index ;
	int  beg_row ;
	int  end_char_index ;
	int  end_row ;

	row = ml_term_convert_scr_row_to_abs( screen->term , convert_y_to_row( screen , NULL , y)) ;

	if( ml_term_get_line_region( screen->term , &beg_row , &end_char_index , &end_row , row) == 0)
	{
		return ;
	}

	if( ml_line_is_rtl( ml_term_get_line( screen->term , beg_row)))
	{
		beg_char_index = -ml_line_end_char_index( ml_term_get_line( screen->term , beg_row)) ;
	}
	else
	{
		beg_char_index = 0 ;
	}

	if( ml_line_is_rtl( ml_term_get_line( screen->term , end_row)))
	{
		end_char_index -=
			ml_line_end_char_index( ml_term_get_line( screen->term , end_row)) ;
	}

	if( ! x_is_selecting( &screen->sel))
	{
		restore_selected_region_color_instantly( screen) ;

		if( ! x_window_set_selection_owner( &screen->window , time))
		{
			return ;
		}

		start_selection( screen , beg_char_index , beg_row , SEL_LINE) ;
		selecting( screen , end_char_index , end_row) ;
		x_sel_lock( &screen->sel) ;
	}
	else if( x_is_before_sel_left_base_pos( &screen->sel , beg_char_index , beg_row))
	{
		selecting( screen , beg_char_index , beg_row) ;
	}
	else
	{
		selecting( screen , end_char_index , end_row) ;
	}
}

static void
pointer_motion(
	x_window_t *  win ,
	XMotionEvent *  event
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t*) win ;

	if( ! (event->state & (ShiftMask|ControlMask)) &&
		ml_term_get_mouse_report_mode( screen->term) == ANY_EVENT_MOUSE_REPORT)
	{
		restore_selected_region_color_instantly( screen) ;
		report_mouse_tracking( screen , event->x , event->y , 0 , event->state , 1 , 0) ;
	}
}

static void
button_motion(
	x_window_t *  win ,
	XMotionEvent *  event
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t*) win ;

	/*
	 * event->state is never 0 because this function is 'button'_motion,
	 * not 'pointer'_motion.
	 */

	if( ! (event->state & (ShiftMask|ControlMask)) &&
		ml_term_get_mouse_report_mode( screen->term))
	{
		if( ml_term_get_mouse_report_mode( screen->term) >= BUTTON_EVENT_MOUSE_REPORT)
		{
			int  button ;

			if( event->state & Button1Mask)
			{
				button = Button1 ;
			}
			else if( event->state & Button2Mask)
			{
				button = Button2 ;
			}
			else if( event->state & Button3Mask)
			{
				button = Button3 ;
			}
			else
			{
				return ;
			}

			restore_selected_region_color_instantly( screen) ;
			report_mouse_tracking( screen , event->x , event->y ,
				button , event->state , 1 , 0) ;
		}
	}
	else if( ! ( event->state & Button2Mask))
	{
		switch( x_is_selecting( &screen->sel))
		{
		case  SEL_WORD:
			selecting_word( screen , event->x , event->y , event->time) ;
			break ;

		case  SEL_LINE:
			selecting_line( screen , event->y , event->time) ;
			break ;

		default:
			selecting_with_motion( screen , event->x , event->y , event->time) ;
			break ;
		}
	}
}

static void
button_press_continued(
	x_window_t *  win ,
	XButtonEvent *  event
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t*) win ;

	if( x_is_selecting( &screen->sel) && (event->y < 0 || win->height < event->y))
	{
		selecting_with_motion( screen , event->x , event->y , event->time) ;
	}
}

static void
button_pressed(
	x_window_t *  win ,
	XButtonEvent *  event ,
	int  click_num
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t*)win ;

	if( ml_term_get_mouse_report_mode( screen->term) &&
		! (event->state & (ShiftMask|ControlMask)))
	{
		restore_selected_region_color_instantly( screen) ;
		report_mouse_tracking( screen , event->x , event->y ,
			event->button , event->state , 0 , 0) ;

		return ;
	}

	if( event->button == Button2)
	{
		if( (event->state & ControlMask) && screen->conf_menu_path_2)
		{
			config_menu( screen , event->x , event->y , screen->conf_menu_path_2) ;
		}

		return ;
	}
	else if( event->button == Button1)
	{
		if( click_num == 2)
		{
			/* double clicked */
			selecting_word( screen , event->x , event->y , event->time) ;

			return ;
		}
		else if( click_num == 3)
		{
			/* triple click */
			selecting_line( screen , event->y , event->time) ;

			return ;
		}
		else if( event->state & ControlMask)
		{
			config_menu( screen , event->x , event->y ,
				screen->conf_menu_path_1 ?
					screen->conf_menu_path_1 : "mlterm-menu"
				#ifdef  HAVE_WINDOWS_H
					".exe"
				#endif
				) ;

			return ;
		}
	}
	else if( event->button == Button3)
	{
		if( event->state & ControlMask)
		{
			config_menu( screen , event->x , event->y ,
				screen->conf_menu_path_3 ?
					screen->conf_menu_path_3 : "mlconfig"
				#ifdef  HAVE_WINDOWS_H
					".exe"
				#endif
				) ;

			return ;
		}
		else if( button3_open)
		{
			if( button3_open == 1)
			{
				config_menu( screen , event->x , event->y ,
					screen->conf_menu_path_1 ?
						screen->conf_menu_path_1 : "mlterm-menu"
					#ifdef  HAVE_WINDOWS_H
						".exe"
					#endif
					) ;
			}
			else if( button3_open == 2)
			{
				if( screen->conf_menu_path_2)
				{
					config_menu( screen , event->x , event->y ,
						screen->conf_menu_path_2) ;
				}
			}
			else if( button3_open == 3)
			{
				config_menu( screen , event->x , event->y ,
					screen->conf_menu_path_3 ?
						screen->conf_menu_path_3 : "mlconfig"
					#ifdef  HAVE_WINDOWS_H
						".exe"
					#endif
					) ;
			}
			else if( button3_open == 4)
			{
				open_button3_command( screen) ;
			}

			return ;
		}
		else if( x_sel_is_reversed( &screen->sel))
		{
			/* expand if current selection exists. */
			/* FIXME: move sel.* stuff should be in x_selection.c */
			screen->sel.is_selecting = SEL_CHAR ;
			selecting_with_motion( screen, event->x, event->y, event->time);
			/* keep sel as selected to handle succeeding MotionNotify */

			return ;
		}
	}
	else if ( event->button == Button4)
	{
		/* wheel mouse */

		enter_backscroll_mode(screen) ;
		if( event->state & ShiftMask)
		{
			bs_scroll_downward(screen) ;
		}
		else if( event->state & ControlMask)
		{
			bs_page_downward(screen) ;
		}
		else
		{
			bs_half_page_downward(screen) ;
		}

		return ;
	}
	else if ( event->button == Button5)
	{
		/* wheel mouse */

		enter_backscroll_mode(screen) ;
		if( event->state & ShiftMask)
		{
			bs_scroll_upward(screen) ;
		}
		else if( event->state & ControlMask)
		{
			bs_page_upward(screen) ;
		}
		else
		{
			bs_half_page_upward(screen) ;
		}

		return ;
	}

	restore_selected_region_color_instantly( screen) ;
}

static void
button_released(
	x_window_t *  win ,
	XButtonEvent *  event
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t*) win ;

	if( ml_term_get_mouse_report_mode( screen->term) &&
		! (event->state & (ShiftMask|ControlMask)))
	{
		if( event->button >= Button4)
		{
			/* Release events for the wheel buttons are not reported. */
		}
		else
		{
			report_mouse_tracking( screen , event->x , event->y ,
				event->button , event->state , 0 , 1) ;
		}
		
		return ;
	}

	if( event->button == Button2)
	{
		if( (event->state & ControlMask) && screen->conf_menu_path_2)
		{
			/* FIXME: should check whether a menu is really active? */
			return ;
		}
		else
		{
			yank_event_received( screen , event->time) ;
		}
	}

	x_stop_selecting( &screen->sel) ;
	highlight_cursor( screen) ;
}

static void
idling(
	x_window_t *  win
	)
{
	x_screen_t *  screen ;

	screen = (x_screen_t*) win ;

	if( screen->cursor_blink_wait >= 0)
	{
		if( screen->cursor_blink_wait == 5)
		{
			if( screen->window.is_focused)
			{
				unhighlight_cursor( screen , 1) ;
				x_window_update( &screen->window , UPDATE_SCREEN) ;
			}

			screen->cursor_blink_wait = -1 ;
		}
		else
		{
			screen->cursor_blink_wait ++ ;
		}
	}
	else
	{
		if( screen->cursor_blink_wait == -6)
		{
			x_window_update( &screen->window , UPDATE_SCREEN|UPDATE_CURSOR) ;
			screen->cursor_blink_wait = 0 ;
		}
		else
		{
			screen->cursor_blink_wait -- ;
		}
	}
}


#ifdef  HAVE_REGEX

#include  <regex.h>

static int
match(
	size_t *  beg ,
	size_t *  len ,
	void *  regex ,
	u_char *  str ,
	int  backward
	)
{
	regmatch_t  pmatch[1] ;

	if( regexec( regex , str , 1 , pmatch , 0) != 0)
	{
		return  0 ;
	}

	*beg = pmatch[0].rm_so ;
	*len = pmatch[0].rm_eo - pmatch[0].rm_so ;

	if( backward)
	{
		while( 1)
		{
			str += pmatch[0].rm_eo ;

			if( regexec( regex , str , 1 , pmatch , 0) != 0)
			{
				break ;
			}

			(*beg) += ((*len) + pmatch[0].rm_so) ;
			*len = pmatch[0].rm_eo - pmatch[0].rm_so ;
		}
	}

	return  1 ;
}

#else	/* HAVE_REGEX */

static int
match(
	size_t *  beg ,
	size_t *  len ,
	void *  regex ,
	u_char *  str ,
	int  backward
	)
{
	size_t  regex_len ;
	size_t  str_len ;
	u_char *  p ;

	if( ( regex_len = strlen( regex)) > (str_len = strlen( str)))
	{
		return  0 ;
	}

#if  0
	{
		kik_msg_printf( "S T R => ") ;
		p = str ;
		while( *p)
		{
			kik_msg_printf( "%.2x" , *p) ;
			p ++ ;
		}
		kik_msg_printf( "\nREGEX => ") ;

		p = regex ;
		while( *p)
		{
			kik_msg_printf( "%.2x" , *p) ;
			p ++ ;
		}
		kik_msg_printf( "\n") ;
	}
#endif

	if( backward)
	{
		p = str + str_len - regex_len ;

		do
		{
			if( strncasecmp( p , regex , regex_len) == 0)
			{
				goto  found ;
			}
		}
		while( p -- != str) ;

		return  0 ;
	}
	else
	{
		p = str ;

		do
		{
			if( strncasecmp( p , regex , regex_len) == 0)
			{
				goto  found ;
			}
		}
		while( *(++p)) ;

		return  0 ;
	}

found:
	*beg = p - str ;
	*len = regex_len ;

	return  1 ;
}

#endif	/* HAVE_REGEX */

static int
search_find(
	x_screen_t *  screen ,
	u_char *  pattern ,
	int  backward
	)
{
	int  beg_char_index ;
	int  beg_row ;
	int  end_char_index ;
	int  end_row ;
#ifdef  HAVE_REGEX
	regex_t  regex ;
#endif

	if( pattern && *pattern
	#ifdef  HAVE_REGEX
		&& regcomp( &regex , pattern , REG_EXTENDED|REG_ICASE) == 0
	#endif
		)
	{
		ml_term_search_init( screen->term , match) ;
	#ifdef  HAVE_REGEX
		if( ml_term_search_find( screen->term , &beg_char_index , &beg_row ,
				&end_char_index , &end_row , &regex , backward))
	#else
		if( ml_term_search_find( screen->term , &beg_char_index , &beg_row ,
				&end_char_index , &end_row , pattern , backward))
	#endif
		{
		#ifdef  DEBUG
			kik_debug_printf( KIK_DEBUG_TAG " Search find %d %d - %d %d\n" ,
					beg_char_index , beg_row , end_char_index , end_row) ;
		#endif

			x_sel_clear( &screen->sel) ;
			start_selection( screen , beg_char_index , beg_row , SEL_CHAR) ;
			selecting( screen , end_char_index , end_row) ;
			x_stop_selecting( &screen->sel) ;

			x_screen_scroll_to( screen , beg_row) ;
			if( HAS_SCROLL_LISTENER(screen,scrolled_to))
			{
				(*screen->screen_scroll_listener->scrolled_to)(
					screen->screen_scroll_listener->self , beg_row) ;
			}
		}

	#ifdef  HAVE_REGEX
		regfree( &regex) ;
	#endif
	}
	else
	{
		ml_term_search_final( screen->term) ;
	}

	return  1 ;
}


static void
resize_window(
	x_screen_t *  screen
	)
{
	/* screen will redrawn in window_resized() */
	if( x_window_resize( &screen->window , screen_width( screen) , screen_height( screen) ,
		NOTIFY_TO_PARENT))
	{
		/*
		 * !! Notice !!
		 * x_window_resize() will invoke ConfigureNotify event but window_resized()
		 * won't be called , since xconfigure.width , xconfigure.height are the same
		 * as the already resized window.
		 */
		if( screen->window.window_resized)
		{
			(*screen->window.window_resized)( &screen->window) ;
		}
	}
}

static void
font_size_changed(
	x_screen_t *  screen
	)
{
	if( HAS_SCROLL_LISTENER(screen,line_height_changed))
	{
		(*screen->screen_scroll_listener->line_height_changed)(
			screen->screen_scroll_listener->self , x_line_height( screen)) ;
	}

	x_window_set_normal_hints( &screen->window ,
		x_col_width( screen) , x_line_height( screen) , 0 , 0 ,
		x_col_width( screen) , x_line_height( screen)) ;

	resize_window( screen) ;
}

static void
change_font_size(
	x_screen_t *  screen ,
	u_int  font_size
	)
{
	if( font_size == x_get_font_size( screen->font_man))
	{
		/* not changed */

		return ;
	}

	if( ! x_change_font_size( screen->font_man , font_size))
	{
	#ifdef  DEBUG
		kik_warn_printf( KIK_DEBUG_TAG " x_change_font_size(%d) failed.\n" , font_size) ;
	#endif

		return ;
	}

	/* redrawing all lines with new fonts. */
	ml_term_set_modified_all_lines_in_screen( screen->term) ;

	font_size_changed( screen) ;

	/* this is because font_man->font_set may have changed in x_change_font_size() */
	x_xic_font_set_changed( &screen->window) ;
}

static void
change_line_space(
	x_screen_t *  screen ,
	u_int  line_space
	)
{
	if( screen->line_space == line_space)
	{
		/* not changed */

		return ;
	}

	screen->line_space = line_space ;

	font_size_changed( screen) ;
}

static void
change_letter_space(
	x_screen_t *  screen ,
	u_int  letter_space
	)
{
	if( ! x_set_letter_space( screen->font_man , letter_space))
	{
		return ;
	}

	font_size_changed( screen) ;
}

static void
change_screen_width_ratio(
	x_screen_t *  screen ,
	u_int  ratio
	)
{
	if( screen->screen_width_ratio == ratio)
	{
		return ;
	}

	screen->screen_width_ratio = ratio ;

	resize_window( screen) ;
}

static void
change_screen_height_ratio(
	x_screen_t *  screen ,
	u_int  ratio
	)
{
	if( screen->screen_height_ratio == ratio)
	{
		return ;
	}

	screen->screen_height_ratio = ratio ;

	resize_window( screen) ;
}

static void
change_font_present(
	x_screen_t *  screen ,
	x_type_engine_t  type_engine ,
	x_font_present_t  font_present
	)
{
	if( ml_term_get_vertical_mode( screen->term))
	{
		if( font_present & FONT_VAR_WIDTH)
		{
			kik_msg_printf( "Set use_variable_column_width=false forcibly.\n") ;
			font_present &= ~FONT_VAR_WIDTH ;
		}
	}

	if( ! x_change_font_present( screen->font_man , type_engine , font_present))
	{
		return ;
	}

	/* XXX This function is called from x_screen_new via update_special_visual. */
	if( ! screen->window.my_window)
	{
		return ;
	}
	
	x_window_set_type_engine( &screen->window , x_get_type_engine( screen->font_man)) ;

	/* redrawing all lines with new fonts. */
	ml_term_set_modified_all_lines_in_screen( screen->term) ;

	font_size_changed( screen) ;
}

static void
usascii_font_cs_changed(
	x_screen_t *  screen ,
	ml_char_encoding_t  encoding
	)
{
	x_font_manager_usascii_font_cs_changed( screen->font_man ,
				x_get_usascii_font_cs( encoding)) ;

	font_size_changed( screen) ;

	/*
	 * this is because font_man->font_set may have changed in
	 * x_font_manager_usascii_font_cs_changed()
	 */
	x_xic_font_set_changed( &screen->window) ;
}

static void
change_char_encoding(
	x_screen_t *  screen ,
	ml_char_encoding_t  encoding
	)
{
	if( ml_term_get_encoding( screen->term) == encoding)
	{
		/* not changed */

		return ;
	}

	usascii_font_cs_changed( screen , encoding) ;

	if( ! ml_term_change_encoding( screen->term , encoding))
	{
		kik_error_printf( "VT100 encoding and Terminal screen encoding are discrepant.\n") ;
	}

	if( update_special_visual( screen))
	{
		ml_term_set_modified_all_lines_in_screen( screen->term) ;
	}

	if( screen->im)
	{
		change_im( screen , kik_str_alloca_dup( screen->input_method)) ;
	}
}

static void
change_log_size(
	x_screen_t *  screen ,
	u_int  logsize
	)
{
	if( ml_term_get_log_size( screen->term) == logsize)
	{
		/* not changed */

		return ;
	}

	/*
	 * this is necessary since ml_logs_t size is changed.
	 */
	x_stop_selecting( &screen->sel) ;
	restore_selected_region_color_instantly( screen) ;
	exit_backscroll_mode( screen) ;

	ml_term_change_log_size( screen->term , logsize) ;

	if( HAS_SCROLL_LISTENER(screen,log_size_changed))
	{
		(*screen->screen_scroll_listener->log_size_changed)(
			screen->screen_scroll_listener->self , logsize) ;
	}
}

static void
change_sb_view(
	x_screen_t *  screen ,
	char *  name
	)
{
	if( HAS_SCROLL_LISTENER(screen,change_view))
	{
		(*screen->screen_scroll_listener->change_view)(
			screen->screen_scroll_listener->self , name) ;
	}
}

static void
change_mod_meta_key(
	x_screen_t *  screen ,
	char *  key
	)
{
	free( screen->mod_meta_key) ;

	if( strcmp( key , "none") == 0)
	{
		screen->mod_meta_key = NULL ;
	}
	else
	{
		screen->mod_meta_key = strdup( key) ;
	}

	screen->mod_meta_mask = x_window_get_mod_meta_mask( &(screen->window) ,
					screen->mod_meta_key) ;
}

static void
change_mod_meta_mode(
	x_screen_t *  screen ,
	x_mod_meta_mode_t  mod_meta_mode
	)
{
	screen->mod_meta_mode = mod_meta_mode ;
}

static void
change_bel_mode(
	x_screen_t *  screen ,
	x_bel_mode_t  bel_mode
	)
{
	screen->bel_mode = bel_mode ;
}

static void
change_vertical_mode(
	x_screen_t *  screen ,
	ml_vertical_mode_t  vertical_mode
	)
{
	if( ml_term_get_vertical_mode( screen->term) == vertical_mode)
	{
		/* not changed */

		return ;
	}

	ml_term_set_vertical_mode( screen->term , vertical_mode) ;

	if( update_special_visual( screen))
	{
		/* redrawing under new vertical mode. */
		ml_term_set_modified_all_lines_in_screen( screen->term) ;
	}

	resize_window( screen) ;
}

static void
change_sb_mode(
	x_screen_t *  screen ,
	x_sb_mode_t  sb_mode
	)
{
	if( HAS_SCROLL_LISTENER(screen,change_sb_mode))
	{
		(*screen->screen_scroll_listener->change_sb_mode)(
			screen->screen_scroll_listener->self , sb_mode) ;
	}
}

static void
change_dynamic_comb_flag(
	x_screen_t *  screen ,
	int  use_dynamic_comb
	)
{
	if( ml_term_is_using_dynamic_comb( screen->term) == use_dynamic_comb)
	{
		/* not changed */

		return ;
	}

	ml_term_set_use_dynamic_comb( screen->term , use_dynamic_comb) ;

	if( update_special_visual( screen))
	{
		ml_term_set_modified_all_lines_in_screen( screen->term) ;
	}
}

static void
change_receive_string_via_ucs_flag(
	x_screen_t *  screen ,
	int  flag
	)
{
	screen->receive_string_via_ucs = flag ;
}

static void
change_fg_color(
	x_screen_t *  screen ,
	char *  name
	)
{
	if( x_color_manager_set_fg_color( screen->color_man , name) &&
	    x_window_set_fg_color( &screen->window ,
		x_get_xcolor( screen->color_man , ML_FG_COLOR)))
	{
		x_xic_fg_color_changed( &screen->window) ;

		ml_term_set_modified_all_lines_in_screen( screen->term) ;
	}
}

static void  picture_modifier_changed( x_screen_t *  screen) ;

static void
change_bg_color(
	x_screen_t *  screen ,
	char *  name
	)
{
	if( x_color_manager_set_bg_color( screen->color_man , name) &&
	    x_window_set_bg_color( &screen->window ,
		x_get_xcolor( screen->color_man , ML_BG_COLOR)))
	{
		x_xic_bg_color_changed( &screen->window) ;

		x_get_xcolor_rgba( &screen->pic_mod.blend_red , &screen->pic_mod.blend_green ,
				&screen->pic_mod.blend_blue , NULL ,
				x_get_xcolor( screen->color_man , ML_BG_COLOR)) ;

		picture_modifier_changed( screen) ;

		ml_term_set_modified_all_lines_in_screen( screen->term) ;
	}
}

static void
change_bd_color(
	x_screen_t *  screen ,
	char *  name
	)
{
	if( x_color_manager_set_bd_color( screen->color_man , *name ? name : NULL))
	{
		ml_term_set_modified_all_lines_in_screen( screen->term) ;
	}
}

static void
change_ul_color(
	x_screen_t *  screen ,
	char *  name
	)
{
	if( x_color_manager_set_ul_color( screen->color_man , *name ? name : NULL))
	{
		ml_term_set_modified_all_lines_in_screen( screen->term) ;
	}
}

static void
change_sb_fg_color(
	x_screen_t *  screen ,
	char *  name
	)
{
	if( HAS_SCROLL_LISTENER(screen,change_fg_color))
	{
		(*screen->screen_scroll_listener->change_fg_color)(
			screen->screen_scroll_listener->self , name) ;
	}
}

static void
change_sb_bg_color(
	x_screen_t *  screen ,
	char *  name
	)
{
	if( HAS_SCROLL_LISTENER(screen,change_bg_color))
	{
		(*screen->screen_scroll_listener->change_bg_color)(
			screen->screen_scroll_listener->self , name) ;
	}
}

static void
change_use_bold_font_flag(
	x_screen_t *  screen ,
	int  flag
	)
{
	if( x_set_use_bold_font( screen->font_man , flag))
	{
		ml_term_set_modified_all_lines_in_screen( screen->term) ;
	}
}

static void
change_hide_underline_flag(
	x_screen_t *  screen ,
	int  flag
	)
{
	if( screen->hide_underline != flag)
	{
		screen->hide_underline = flag ;
		ml_term_set_modified_all_lines_in_screen( screen->term) ;
	}
}

static void
larger_font_size(
	x_screen_t *  screen
	)
{
	x_larger_font( screen->font_man) ;

	font_size_changed( screen) ;

	/* this is because font_man->font_set may have changed in x_larger_font() */
	x_xic_font_set_changed( &screen->window) ;

	/* redrawing all lines with new fonts. */
	ml_term_set_modified_all_lines_in_screen( screen->term) ;
}

static void
smaller_font_size(
	x_screen_t *  screen
	)
{
	x_smaller_font( screen->font_man) ;

	font_size_changed( screen) ;

	/* this is because font_man->font_set may have changed in x_smaller_font() */
	x_xic_font_set_changed( &screen->window) ;

	/* redrawing all lines with new fonts. */
	ml_term_set_modified_all_lines_in_screen( screen->term) ;
}

static void
change_transparent_flag(
	x_screen_t *  screen ,
	int  is_transparent
	)
{
	if( screen->window.is_transparent == is_transparent
	#if ! defined(USE_WIN32GUI) && ! defined(USE_FRAMEBUFFER)
	    /*
	     * If wall picture is not still set, do set it.
	     * This is necessary for gnome-terminal, because ConfigureNotify event never
	     * happens when it opens new tab.
	     */
	    && screen->window.wall_picture_is_set == is_transparent
	#endif
		)
	{
		/* not changed */

		return ;
	}

	if( is_transparent)
	{
		/*
		 * This change doesn't affect true transparency using
		 * x_color_manager_change_alpha() until change_alpha() is called.
		 */
		if( screen->pic_mod.alpha == 255)
		{
			screen->pic_mod.alpha = 0 ;
		}

		x_window_set_transparent( &screen->window ,
			x_screen_get_picture_modifier( screen)) ;
	}
	else
	{
		x_window_unset_transparent( &screen->window) ;
		set_wall_picture( screen) ;
	}

	if( HAS_SCROLL_LISTENER(screen,transparent_state_changed))
	{
		(*screen->screen_scroll_listener->transparent_state_changed)(
			screen->screen_scroll_listener->self , is_transparent ,
			x_screen_get_picture_modifier( screen)) ;
	}
}

static void
change_multi_col_char_flag(
	x_screen_t *  screen ,
	int  flag
	)
{
	x_set_use_multi_col_char( screen->font_man , flag) ;
	ml_term_set_use_multi_col_char( screen->term , flag) ;
}

static void
change_bidi_flag(
	x_screen_t *  screen ,
	int  use_bidi ,
	ml_bidi_mode_t  bidi_mode
	)
{
	int  do_update ;
	
	if( ml_term_is_using_bidi( screen->term) == use_bidi &&
	    ml_term_get_bidi_mode( screen->term) == bidi_mode)
	{
		/* not changed */

		return ;
	}

	/*
	 * If use_bidi flag is false and not changed, it is not necessary to update even if
	 * bidi_mode flag is changed.
	 */
	do_update = ( use_bidi != ml_term_is_using_bidi( screen->term)) ||
			ml_term_is_using_bidi( screen->term) ;

	ml_term_set_use_bidi( screen->term , use_bidi) ;
	ml_term_set_bidi_mode( screen->term , bidi_mode) ;
	
	if( do_update && update_special_visual( screen))
	{
		ml_term_set_modified_all_lines_in_screen( screen->term) ;
	}
}

static void
change_ind_flag(
	x_screen_t *  screen ,
	int  use_ind
	)
{
	if( ml_term_is_using_ind( screen->term) == use_ind)
	{
		/* not changed */

		return ;
	}

	ml_term_set_use_ind( screen->term , use_ind) ;

	if( update_special_visual( screen))
	{
		ml_term_set_modified_all_lines_in_screen( screen->term) ;
	}
}

static void
change_borderless_flag(
	x_screen_t *  screen ,
	int  flag
	)
{
	if( x_window_set_borderless_flag( &screen->window , flag))
	{
		screen->borderless = flag ;
	}
}

static void
change_wall_picture(
	x_screen_t *  screen ,
	char *  file_path
	)
{
	if( screen->pic_file_path)
	{
		if( strcmp( screen->pic_file_path , file_path) == 0)
		{
			/* not changed */

			return ;
		}

		free( screen->pic_file_path) ;
	}

	if( *file_path == '\0')
	{
		screen->pic_file_path = NULL ;
		x_window_unset_wall_picture( &screen->window) ;
	}
	else
	{
		/*
		 * This change doesn't affect true transparency using
		 * x_color_manager_change_alpha() until change_alpha() is called.
		 */
		if( screen->pic_mod.alpha == 255)
		{
			screen->pic_mod.alpha = 0 ;
		}

		screen->pic_file_path = strdup( file_path) ;
		set_wall_picture( screen) ;
	}
}

static void
picture_modifier_changed(
	x_screen_t *  screen
	)
{
	if( screen->window.is_transparent)
	{
		x_window_set_transparent( &screen->window ,
			x_screen_get_picture_modifier( screen)) ;

		if( HAS_SCROLL_LISTENER(screen,transparent_state_changed))
		{
			(*screen->screen_scroll_listener->transparent_state_changed)(
				screen->screen_scroll_listener->self , 1 ,
				x_screen_get_picture_modifier( screen)) ;
		}
	}
	else
	{
		set_wall_picture( screen) ;
	}
}

static void
change_brightness(
	x_screen_t *  screen ,
	u_int  brightness
	)
{
	if( screen->pic_mod.brightness == brightness)
	{
		/* not changed */

		return ;
	}

	screen->pic_mod.brightness = brightness ;

	picture_modifier_changed( screen) ;
}

static void
change_contrast(
	x_screen_t *  screen ,
	u_int  contrast
	)
{
	if( screen->pic_mod.contrast == contrast)
	{
		/* not changed */

		return ;
	}

	screen->pic_mod.contrast = contrast ;

	picture_modifier_changed( screen) ;
}

static void
change_gamma(
	x_screen_t *  screen ,
	u_int  gamma
	)
{
	if( screen->pic_mod.gamma == gamma)
	{
		/* not changed */

		return ;
	}

	screen->pic_mod.gamma = gamma ;

	picture_modifier_changed( screen) ;
}

static void
change_alpha(
	x_screen_t *  screen ,
	u_int  alpha
	)
{
	if( screen->pic_mod.alpha == alpha)
	{
		/* not changed */

		return ;
	}

	screen->pic_mod.alpha = alpha ;

	if( x_color_manager_change_alpha( screen->color_man , alpha))
	{
		/* Same processing as change_bg_color */
	
		if( x_window_set_bg_color( &screen->window ,
			x_get_xcolor( screen->color_man , ML_BG_COLOR)))
		{
			x_xic_bg_color_changed( &screen->window) ;

			ml_term_set_modified_all_lines_in_screen( screen->term) ;
		}
	}
	
	picture_modifier_changed( screen) ;
}

static void
change_fade_ratio(
	x_screen_t *  screen ,
	u_int  fade_ratio
	)
{
	if( screen->fade_ratio == fade_ratio)
	{
		/* not changed */

		return ;
	}

	screen->fade_ratio = fade_ratio ;

	x_color_manager_unfade( screen->color_man) ;

	if( ! screen->window.is_focused)
	{
		if( screen->fade_ratio < 100)
		{
			x_color_manager_fade( screen->color_man , screen->fade_ratio) ;
		}
	}

	x_window_set_fg_color( &screen->window ,
		x_get_xcolor( screen->color_man , ML_FG_COLOR)) ;
	x_window_set_bg_color( &screen->window ,
		x_get_xcolor( screen->color_man , ML_BG_COLOR)) ;

	ml_term_set_modified_all_lines_in_screen( screen->term) ;
}

static void
change_im(
	x_screen_t *  screen ,
	char *  input_method
	)
{
	x_im_t *  im ;

	x_xic_deactivate( &screen->window) ;

	/*
	 * Avoid to delete anything inside im-module by calling x_im_delete()
	 * after x_im_new().
	 */
	im = screen->im ;

	free( screen->input_method) ;
	screen->input_method = NULL ;

	if( ! input_method)
	{
		return ;
	}

	screen->input_method = strdup( input_method) ;

	if( strncmp( screen->input_method , "xim" , 3) == 0)
	{
		activate_xic( screen) ;
		screen->im = NULL ;
	}
	else
	{
		x_xic_activate( &screen->window , "none" , "");
	
		if( ( screen->im = x_im_new(
				ml_term_get_encoding( screen->term) ,
				&screen->im_listener ,
				screen->input_method ,
				screen->mod_ignore_mask)))
		{
			if(screen->window.is_focused)
			{
				screen->im->focused( screen->im) ;
			}
		}
		else
		{
			free( screen->input_method) ;
			screen->input_method = NULL ;
		}
	}

	if( im)
	{
		x_im_delete( im) ;
	}
}

static void
snapshot(
	x_screen_t *  screen ,
	ml_char_encoding_t  encoding ,
	char *  file_name
	)
{
	char *  path ;
	int  beg ;
	int  end ;
	ml_char_t *  buf ;
	u_int  num ;
	FILE *  file ;
	u_char  conv_buf[512] ;
	mkf_conv_t *  conv ;

	if( ( path = alloca( 7 + strlen( file_name) + 4 + 1)) == NULL)
	{
		return ;
	}
	sprintf( path , "mlterm/%s.snp" , file_name) ;

	if( ( path = kik_get_user_rc_path( path)) == NULL)
	{
		return ;
	}

	file = fopen( path , "w") ;
	free( path) ;
	if( ! file)
	{
	#ifdef  DEBUG
		kik_debug_printf( KIK_DEBUG_TAG " Failed to open %s\n" , file_name) ;
	#endif

		return ;
	}

	beg = - ml_term_get_num_of_logged_lines( screen->term) ;
	end = ml_term_get_rows( screen->term) ;

	num = ml_term_get_region_size( screen->term , 0 , beg , 0 , end) ;

	if( ( buf = ml_str_alloca( num)) == NULL)
	{
		fclose( file) ;

		return ;
	}

	ml_term_copy_region( screen->term , buf , num , 0 , beg , 0 , end) ;

	(*screen->ml_str_parser->init)( screen->ml_str_parser) ;
	ml_str_parser_set_str( screen->ml_str_parser , buf , num) ;

	if( encoding == ML_UNKNOWN_ENCODING || ( conv = ml_conv_new( encoding)) == NULL)
	{
		while( ! screen->ml_str_parser->is_eos)
		{
			if( ( num = ml_term_convert_to( screen->term , conv_buf ,
					sizeof( conv_buf) , screen->ml_str_parser)) == 0)
			{
				break ;
			}

			fwrite( conv_buf , num , 1 , file) ;
		}
	}
	else
	{
		while( ! screen->ml_str_parser->is_eos)
		{
			if( ( num = (*conv->convert)( conv , conv_buf , sizeof( conv_buf) ,
					screen->ml_str_parser)) == 0)
			{
				break ;
			}

			fwrite( conv_buf , num , 1 , file) ;
		}

		(*conv->delete)( conv) ;
	}

	fclose( file) ;
}


/*
 * Callbacks of x_config_event_listener_t events.
 */

static void
get_config(
	void *  p ,
	char *  dev ,	/* can be NULL */
	char *  key ,	/* can be "error" */
	int  to_menu
	)
{
	x_screen_t *  screen ;
	ml_term_t *  term ;
	char *  value ;
	char  digit[DIGIT_STR_LEN(u_int) + 1] ;
	char  cwd[PATH_MAX] ;

	screen = p ;
	
	if( dev && HAS_SYSTEM_LISTENER(screen,get_pty))
	{
		/* Don't response (ml_term_write) to this term whose config_menu->fd is -1. */
		if( ( term = (*screen->system_listener->get_pty)( screen->system_listener->self ,
				dev)) == NULL)
		{
			return ;
		}
	}
	else
	{
		term = screen->term ;
	}

	value = NULL ;

	if( strcmp( key , "encoding") == 0)
	{
		value = ml_get_char_encoding_name( ml_term_get_encoding( term)) ;
	}
	else if( strcmp( key , "is_auto_encoding") == 0)
	{
		if( ml_term_is_auto_encoding( term))
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "fg_color") == 0)
	{
		value = x_color_manager_get_fg_color( screen->color_man) ;
	}
	else if( strcmp( key , "bg_color") == 0)
	{
		value = x_color_manager_get_bg_color( screen->color_man) ;
	}
	else if( strcmp( key , "cursor_fg_color") == 0)
	{
		if( ( value = x_color_manager_get_cursor_fg_color( screen->color_man)) == NULL)
		{
			value = "" ;
		}
	}
	else if( strcmp( key , "cursor_bg_color") == 0)
	{
		if( ( value = x_color_manager_get_cursor_bg_color( screen->color_man)) == NULL)
		{
			value = "" ;
		}
	}
	else if( strcmp( key , "bd_color") == 0)
	{
		if( ( value = x_color_manager_get_bd_color( screen->color_man)) == NULL)
		{
			value = "" ;
		}
	}
	else if( strcmp( key , "ul_color") == 0)
	{
		if( ( value = x_color_manager_get_ul_color( screen->color_man)) == NULL)
		{
			value = "" ;
		}
	}
	else if( strcmp( key , "sb_fg_color") == 0)
	{
		if( screen->screen_scroll_listener && screen->screen_scroll_listener->fg_color)
		{
			value = (*screen->screen_scroll_listener->fg_color)(
					screen->screen_scroll_listener->self) ;
		}
		else
		{
			value = NULL ;
		}
	}
	else if( strcmp( key , "sb_bg_color") == 0)
	{
		if( screen->screen_scroll_listener && screen->screen_scroll_listener->bg_color)
		{
			value = (*screen->screen_scroll_listener->bg_color)(
					screen->screen_scroll_listener->self) ;
		}
		else
		{
			value = NULL ;
		}
	}
	else if( strcmp( key , "hide_underline") == 0)
	{
		if( screen->hide_underline)
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "tabsize") == 0)
	{
		sprintf( digit , "%d" , ml_term_get_tab_size( term)) ;
		value = digit ;
	}
	else if( strcmp( key , "logsize") == 0)
	{
		sprintf( digit , "%d" , ml_term_get_log_size( term)) ;
		value = digit ;
	}
	else if( strcmp( key , "fontsize") == 0)
	{
		sprintf( digit , "%d" , x_get_font_size( screen->font_man)) ;
		value = digit ;
	}
	else if( strcmp( key , "line_space") == 0)
	{
		sprintf( digit , "%d" , screen->line_space) ;
		value = digit ;
	}
	else if( strcmp( key , "letter_space") == 0)
	{
		sprintf( digit , "%d" , x_get_letter_space( screen->font_man)) ;
		value = digit ;
	}
	else if( strcmp( key , "screen_width_ratio") == 0)
	{
		sprintf( digit , "%d" , screen->screen_width_ratio) ;
		value = digit ;
	}
	else if( strcmp( key , "screen_height_ratio") == 0)
	{
		sprintf( digit , "%d" , screen->screen_height_ratio) ;
		value = digit ;
	}
	else if( strcmp( key , "scrollbar_view_name") == 0)
	{
		if( screen->screen_scroll_listener && screen->screen_scroll_listener->view_name)
		{
			value = (*screen->screen_scroll_listener->view_name)(
					screen->screen_scroll_listener->self) ;
		}
		else
		{
			value = NULL ;
		}
	}
	else if( strcmp( key , "mod_meta_key") == 0)
	{
		if( screen->mod_meta_key == NULL)
		{
			value = "none" ;
		}
		else
		{
			value = screen->mod_meta_key ;
		}
	}
	else if( strcmp( key , "mod_meta_mode") == 0)
	{
		value = x_get_mod_meta_mode_name( screen->mod_meta_mode) ;
	}
	else if( strcmp( key , "bel_mode") == 0)
	{
		value = x_get_bel_mode_name( screen->bel_mode) ;
	}
	else if( strcmp( key , "vertical_mode") == 0)
	{
		value = ml_get_vertical_mode_name( ml_term_get_vertical_mode( term)) ;
	}
	else if( strcmp( key , "scrollbar_mode") == 0)
	{
		if( screen->screen_scroll_listener &&
			screen->screen_scroll_listener->sb_mode)
		{
			value = x_get_sb_mode_name( (*screen->screen_scroll_listener->sb_mode)(
				screen->screen_scroll_listener->self)) ;
		}
		else
		{
			value = x_get_sb_mode_name( SBM_NONE) ;
		}
	}
	else if( strcmp( key , "use_combining") == 0)
	{
		if( ml_term_is_using_char_combining( term))
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "use_dynamic_comb") == 0)
	{
		if( ml_term_is_using_dynamic_comb( term))
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "receive_string_via_ucs") == 0 ||
		/* backward compatibility with 2.6.1 or before */
		strcmp( key , "copy_paste_via_ucs") == 0)
	{
		if( screen->receive_string_via_ucs)
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "use_transbg") == 0)
	{
		if( screen->window.is_transparent)
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "brightness") == 0)
	{
		sprintf( digit , "%d" , screen->pic_mod.brightness) ;
		value = digit ;
	}
	else if( strcmp( key , "contrast") == 0)
	{
		sprintf( digit , "%d" , screen->pic_mod.contrast) ;
		value = digit ;
	}
	else if( strcmp( key , "gamma") == 0)
	{
		sprintf( digit , "%d" , screen->pic_mod.gamma) ;
		value = digit ;
	}
	else if( strcmp( key , "alpha") == 0)
	{
		sprintf( digit , "%d" , screen->pic_mod.alpha) ;
		value = digit ;
	}
	else if( strcmp( key , "fade_ratio") == 0)
	{
		sprintf( digit , "%d" , screen->fade_ratio) ;
		value = digit ;
	}
	else if( strcmp( key , "type_engine") == 0)
	{
		value = x_get_type_engine_name( x_get_type_engine( screen->font_man)) ;
	}
	else if( strcmp( key , "use_anti_alias") == 0)
	{
		x_font_present_t  font_present ;

		font_present = x_get_font_present( screen->font_man) ;
		if( font_present & FONT_AA)
		{
			value = "true" ;
		}
		else if( font_present & FONT_NOAA)
		{
			value = "false" ;
		}
		else
		{
			value = "default" ;
		}
	}
	else if( strcmp( key , "use_variable_column_width") == 0)
	{
		if( x_get_font_present( screen->font_man) & FONT_VAR_WIDTH)
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "use_multi_column_char") == 0)
	{
		if( x_is_using_multi_col_char( screen->font_man))
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "use_bold_font") == 0)
	{
		if( x_is_using_bold_font( screen->font_man))
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "col_size_of_width_a") == 0)
	{
		if( ml_term_get_col_size_of_width_a( term) == 2)
		{
			value = "2" ;
		}
		else
		{
			value = "1" ;
		}
	}
	else if( strcmp( key , "use_bidi") == 0)
	{
		if( ml_term_is_using_bidi( term))
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "use_ind") == 0)
	{
		if( ml_term_is_using_ind( term))
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "bidi_mode") == 0)
	{
		value = ml_get_bidi_mode_name( ml_term_get_bidi_mode( term)) ;
	}
	else if( strcmp( key , "input_method") == 0)
	{
		if( screen->input_method)
		{
			value = screen->input_method ;
		}
		else
		{
			value = "none" ;
		}
	}
	else if( strcmp( key , "default_xim_name") == 0)
	{
		value = x_xic_get_default_xim_name() ;
	}
	else if( strcmp( key , "locale") == 0)
	{
		value = kik_get_locale() ;
	}
	else if( strcmp( key , "borderless") == 0)
	{
		if( screen->borderless)
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "wall_picture") == 0)
	{
		if( screen->pic_file_path)
		{
			value = screen->pic_file_path ;
		}
		else
		{
			value = "" ;
		}
	}
	else if( strcmp( key , "pwd") == 0)
	{
		value = getcwd( cwd , sizeof(cwd)) ;
	}
	else if( strcmp( key , "rows") == 0)
	{
		sprintf( digit , "%d" , ml_term_get_logical_rows( term)) ;
		value = digit ;
	}
	else if( strcmp( key , "cols") == 0)
	{
		sprintf( digit , "%d" , ml_term_get_logical_cols( term)) ;
		value = digit ;
	}
	else if( strcmp( key , "pty_list") == 0)
	{
		if( HAS_SYSTEM_LISTENER(screen,pty_list))
		{
			value = (*screen->system_listener->pty_list)( screen->system_listener->self) ;
		}
	}
	else if( strcmp( key , "pty_name") == 0)
	{
		if( dev)
		{
			if( ( value = ml_term_window_name( term)) == NULL)
			{
				value = dev ;
			}
		}
		else
		{
			value = ml_term_get_slave_name( term) ;
		}
	}
	else if( strcmp( key , "icon_path") == 0)
	{
		value = ml_term_icon_path( term) ;
	}
	else if( strcmp( key , "logging_vt_seq") == 0)
	{
		if( ml_term_is_logging_vt_seq( term))
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "gui") == 0)
	{
	#if  defined(USE_WIN32GUI)
		value = "win32" ;
	#elif  defined(USE_FRAMEBUFFER)
		value = "fb" ;
	#else
		value = "xlib" ;
	#endif
	}
	else if( strcmp( key , "use_clipboard") == 0)
	{
		if( x_is_using_clipboard_selection())
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "allow_osc52") == 0)
	{
		if( screen->xterm_listener.set_selection)
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "blink_cursor") == 0)
	{
		if( screen->window.idling)
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strcmp( key , "use_local_echo") == 0)
	{
		if( ml_term_is_using_local_echo( screen->term))
		{
			value = "true" ;
		}
		else
		{
			value = "false" ;
		}
	}
	else if( strncmp( key , "selected_text" , 13) == 0)
	{
		ml_term_write( screen->term , "#" , 1 , to_menu) ;
		ml_term_write( screen->term , key , strlen(key) , to_menu) ;
		ml_term_write( screen->term , "=" , 1 , to_menu) ;
		
		if( /* screen->window.is_sel_owner && */
			screen->sel.sel_str && screen->sel.sel_len > 0)
		{
		#ifndef  NL_TO_CR_IN_PAST_TEXT
			if( to_menu)
		#endif
			{
				/*
				 * Convert NL to CR because menu programs regard NL as terminator
				 * of returned value.
				 * Notice that menu programs must convert CR to NL in receiving
				 * selected text.
				 */
				convert_nl_to_cr2( screen->sel.sel_str , screen->sel.sel_len) ;
			}

			(*screen->ml_str_parser->init)( screen->ml_str_parser) ;
			ml_str_parser_set_str( screen->ml_str_parser ,
				screen->sel.sel_str , screen->sel.sel_len) ;

			if( to_menu)
			{
				mkf_conv_t *  conv ;
				
				if( ( conv = ml_conv_new( ml_get_char_encoding( key + 14))) ||
				    ( conv = ml_conv_new( ml_term_get_encoding( term))))
				{
					u_char  buf[512] ;
					size_t  len ;

					(*conv->init)( conv) ;

					while( ! screen->ml_str_parser->is_eos)
					{
						if( ( len = (*conv->convert)( conv ,
								buf , sizeof(buf) ,
								screen->ml_str_parser)) == 0)
						{
							break ;
						}

						ml_term_write( screen->term ,
							buf , len , to_menu) ;
					}

					(*conv->delete)( conv) ;
				}
			}
			else
			{
				write_to_pty( screen , NULL , 0 , screen->ml_str_parser) ;
			}
		}

		ml_term_write( screen->term , "\n" , 1 , to_menu) ;

		return ;
	}

	if( value == NULL)
	{
		ml_term_write( screen->term , "#error\n" , 7 , to_menu) ;

	#ifdef  __DEBUG
		kik_debug_printf( KIK_DEBUG_TAG " #error\n") ;
	#endif
	}
	else
	{
		ml_term_write( screen->term , "#" , 1 , to_menu) ;
		ml_term_write( screen->term , key , strlen( key) , to_menu) ;
		ml_term_write( screen->term , "=" , 1 , to_menu) ;
		ml_term_write( screen->term , value , strlen( value) , to_menu) ;
		ml_term_write( screen->term , "\n" , 1 , to_menu) ;

	#ifdef  __DEBUG
		kik_debug_printf( KIK_DEBUG_TAG " #%s=%s\n" , key , value) ;
	#endif
	}
}

static void
set_font_config(
	void *  p ,
	char *  file ,	/* can be NULL */
	char *  key ,
	char *  val ,
	int  save
	)
{
	x_screen_t *  screen ;

	screen = p ;

	if( x_customize_font_file( file , key , val , save))
	{
		screen->font_or_color_config_updated |= 0x1 ;
	}
}

static void
get_font_config(
	void *  p ,
	char *  file ,		/* can be NULL */
	char *  font_size_str ,	/* can be "error" */
	char *  cs ,		/* can be "error" */
	int  to_menu
	)
{
	x_screen_t *  screen ;
	char *  font_name ;
	u_int  font_size ;

	screen = p ;

	if( sscanf( font_size_str , "%u" , &font_size) != 1)
	{
		goto  error ;
	}

	font_name = x_get_config_font_name2( file , font_size , cs) ;

	ml_term_write( screen->term , "#" , 1 , to_menu) ;
	ml_term_write( screen->term , cs , strlen( cs) , to_menu) ;
	ml_term_write( screen->term , "," , 1 , to_menu) ;
	ml_term_write( screen->term , font_size_str , strlen( font_size_str) , to_menu) ;
	ml_term_write( screen->term , "=" , 1 , to_menu) ;
	if( font_name)
	{
		ml_term_write( screen->term , font_name , strlen( font_name) , to_menu) ;
	}
	ml_term_write( screen->term , "\n" , 1 , to_menu) ;

#ifdef  __DEBUG
	kik_debug_printf( KIK_DEBUG_TAG " #%s,%s=%s (%s)\n" , cs , font_size_str , font_name ,
		to_menu ? "to menu" : "to pty") ;
#endif

	free( font_name) ;

	return ;

error:
	ml_term_write( screen->term , "#error\n" , 7 , to_menu) ;
}

static void
set_color_config(
	void *  p ,
	char *  file ,	/* ignored */
	char *  key ,
	char *  val ,
	int  save
	)

{
	x_screen_t *  screen ;
	
	screen = p ;
	
	if( ml_customize_color_file( key , val , save))
	{
		screen->font_or_color_config_updated |= 0x2 ;
	}
}


/*
 * callbacks of x_sel_event_listener_t events.
 */

static void
reverse_color(
	void *  p ,
	int  beg_char_index ,
	int  beg_row ,
	int  end_char_index ,
	int  end_row
	)
{
	x_screen_t *  screen ;
	ml_line_t *  line ;

	screen = (x_screen_t*)p ;

	/*
	 * Char index -1 has special meaning in rtl lines, so don't use abs() here.
	 */

	if( ( line = ml_term_get_line( screen->term , beg_row)) && ml_line_is_rtl( line))
	{
		beg_char_index = -beg_char_index ;
	}

	if( ( line = ml_term_get_line( screen->term , end_row)) && ml_line_is_rtl( line))
	{
		end_char_index = -end_char_index ;
	}

#ifdef  __DEBUG
	kik_debug_printf( KIK_DEBUG_TAG " reversing region %d %d %d %d.\n" ,
		beg_char_index , beg_row , end_char_index , end_row) ;
#endif

	ml_term_reverse_color( screen->term , beg_char_index , beg_row ,
		end_char_index , end_row) ;
}

static void
restore_color(
	void *  p ,
	int  beg_char_index ,
	int  beg_row ,
	int  end_char_index ,
	int  end_row
	)
{
	x_screen_t *  screen ;
	ml_line_t *  line ;

	screen = (x_screen_t*)p ;

	/*
	 * Char index -1 has special meaning in rtl lines, so don't use abs() here.
	 */

	if( ( line = ml_term_get_line( screen->term , beg_row)) && ml_line_is_rtl( line))
	{
		beg_char_index = -beg_char_index ;
	}

	if( ( line = ml_term_get_line( screen->term , end_row)) && ml_line_is_rtl( line))
	{
		end_char_index = -end_char_index ;
	}

#ifdef  __DEBUG
	kik_debug_printf( KIK_DEBUG_TAG " restoring region %d %d %d %d.\n" ,
		beg_char_index , beg_row , end_char_index , end_row) ;
#endif

	ml_term_restore_color( screen->term , beg_char_index , beg_row ,
		end_char_index , end_row) ;
}

static int
select_in_window(
	void *  p ,
	ml_char_t **  chars ,
	u_int *  len ,
	int  beg_char_index ,
	int  beg_row ,
	int  end_char_index ,
	int  end_row
	)
{
	x_screen_t *  screen ;
	ml_line_t *  line ;
	u_int  size ;

	screen = p ;

	/*
	 * Char index -1 has special meaning in rtl lines, so don't use abs() here.
	 */

	if( ( line = ml_term_get_line( screen->term , beg_row)) && ml_line_is_rtl( line))
	{
		beg_char_index = -beg_char_index ;
	}

	if( ( line = ml_term_get_line( screen->term , end_row)) && ml_line_is_rtl( line))
	{
		end_char_index = -end_char_index ;
	}

	if( ( size = ml_term_get_region_size( screen->term , beg_char_index , beg_row ,
			end_char_index , end_row)) == 0)
	{
		return  0 ;
	}

	if( ( *chars = ml_str_new( size)) == NULL)
	{
		return  0 ;
	}

	*len = ml_term_copy_region( screen->term , *chars , size , beg_char_index ,
		beg_row , end_char_index , end_row) ;

#ifdef  DEBUG
	kik_debug_printf( "SELECTION: ") ;
	ml_str_dump( *chars , size) ;
#endif

#ifdef  DEBUG
	if( size != *len)
	{
		kik_warn_printf( KIK_DEBUG_TAG
			" ml_term_get_region_size() == %d and ml_term_copy_region() == %d"
			" are not the same size !\n" ,
			size , *len) ;
	}
#endif

	return  1 ;
}


/*
 * callbacks of ml_screen_event_listener_t events.
 */

static int
window_scroll_upward_region(
	void *  p ,
	int  beg_row ,
	int  end_row ,
	u_int  size
	)
{
	x_screen_t *  screen ;

	screen = p ;

	if( ! x_window_is_scrollable( &screen->window) ||
	    ! set_scroll_boundary( screen , beg_row , end_row))
	{
		return  0 ;
	}

	screen->scroll_cache_rows += size ;

	return  1 ;
}

static int
window_scroll_downward_region(
	void *  p ,
	int  beg_row ,
	int  end_row ,
	u_int  size
	)
{
	x_screen_t *  screen ;

	screen = p ;

	if( ! x_window_is_scrollable( &screen->window) ||
	    ! set_scroll_boundary( screen , beg_row , end_row))
	{
		return  0 ;
	}

	screen->scroll_cache_rows -= size ;

	return  1 ;
}

static void
line_scrolled_out(
	void *  p
	)
{
	x_screen_t *  screen ;

	screen = p ;

	x_sel_line_scrolled_out( &screen->sel , -((int)ml_term_get_log_size( screen->term))) ;
}

#ifdef  WINDOW_CLEAR
static void
window_clear(
	void *  p ,
	int  row ,
	u_int  num
	)
{
	x_screen_t *  screen ;
	int  y ;
	u_int  height ;

	screen = p ;

	y = row * x_line_height( screen) ;
	height = num * x_line_height( screen) ;

	x_window_clear( &screen->window , 0 , y , screen->window.width , height) ;
}
#endif


/*
 * callbacks of x_xim events.
 */

/*
 * this doesn't consider backscroll mode.
 */
static int
get_spot(
	void *  p ,
	int *  x ,
	int *  y
	)
{
	x_screen_t *  screen ;
	ml_line_t *  line ;

	screen = p ;

	if( ( line = ml_term_get_cursor_line( screen->term)) == NULL ||
		ml_line_is_empty( line))
	{
	#ifdef  DEBUG
		kik_warn_printf( KIK_DEBUG_TAG " cursor line doesn't exist ?.\n") ;
	#endif

		return  0 ;
	}

	*y = convert_row_to_y( screen , ml_term_cursor_row( screen->term))
	/* XXX */
	#ifndef  USE_WIN32GUI
		+ x_line_height( screen)
	#endif
		;

	*x = convert_char_index_to_x_with_shape( screen , line ,
		ml_term_cursor_char_index( screen->term)) ;

#ifdef  __DEBUG
	kik_debug_printf( KIK_DEBUG_TAG " xim spot => x %d y %d\n" , *x , *y) ;
#endif

	return  1 ;
}

static XFontSet
get_fontset(
	void *  p
	)
{
	x_screen_t *  screen ;

	screen = p ;

	return  x_get_fontset( screen->font_man) ;
}

static x_color_t *
get_fg_color(
	void *  p
	)
{
	x_screen_t *  screen ;

	screen = p ;

	return  x_get_xcolor( screen->color_man , ML_FG_COLOR) ;
}

static x_color_t *
get_bg_color(
	void *  p
	)
{
	x_screen_t *  screen ;

	screen = p ;

	return  x_get_xcolor( screen->color_man , ML_BG_COLOR) ;
}

/*
 * callbacks of x_im events.
 */

static int
get_im_spot(
	void *  p ,
	ml_char_t *  chars ,
	int  segment_offset ,
	int *  x ,
	int *  y
	)
{
	x_screen_t *  screen ;
	ml_line_t *  line ;
	ml_char_t *  comb_chars ;
	u_int  comb_size ;
	int  i ;
	int  win_x ;
	int  win_y ;
	Window  unused ;

	screen = p ;

	*x = *y = 0 ;

	if( ( line = ml_term_get_cursor_line( screen->term)) == NULL ||
		ml_line_is_empty( line))
	{
	#ifdef  DEBUG
		kik_warn_printf( KIK_DEBUG_TAG " cursor line doesn't exist ?.\n") ;
	#endif

		return  0 ;
	}

	if( ! ml_term_get_vertical_mode( screen->term))
	{
		int  row ;

		if( ( row = ml_term_cursor_row_in_screen( screen->term)) < 0)
		{
			return  0 ;
		}

		*x = convert_char_index_to_x_with_shape( screen , line ,
				ml_term_cursor_char_index( screen->term)) ;
		*y = convert_row_to_y( screen , row) ;
		*y += x_line_height( screen) ;
	}
	else
	{
		*x = convert_char_index_to_x_with_shape( screen , line ,
				ml_term_cursor_char_index( screen->term)) ;
		*y = convert_row_to_y( screen ,
				ml_term_cursor_row( screen->term)) ;
		*x += x_col_width( screen) ;
	}


	if( ! ml_term_get_vertical_mode( screen->term))
	{
		for( i = 0 ; i < segment_offset ; i++)
		{
			u_int  width ;

			width = x_calculate_char_width(
					x_get_font( screen->font_man , ml_char_font( &chars[i])) ,
					ml_char_bytes( &chars[i]) ,
					ml_char_size( &chars[i]) ,
					ml_char_cs( &chars[i]) , NULL) ;

			if( *x + width > screen->window.width)
			{
				*x = 0 ;
				*y += x_line_height( screen) ;
			}
			*x += width ;

			/* not count combining characters */
			comb_chars = ml_get_combining_chars( &chars[i] , &comb_size) ;
			if( comb_chars)
			{
				i += comb_size ;
			}
		}
	}
	else /* vertical_mode */
	{
		int  width ;
		u_int  height ;
		int  sign = 1 ;

		if( ml_term_get_vertical_mode( screen->term) == VERT_RTL)
		{
			sign = -1;
		}

		width = x_col_width( screen) ;
		height = x_line_height( screen) ;

		for( i = 0 ; i < segment_offset ; i++)
		{
			*y += height ;
			if( *y >= screen->window.height)
			{
				*x += width * sign;
				*y = 0 ;
			}

			/* not count combining characters */
			comb_chars = ml_get_combining_chars( &chars[i] , &comb_size) ;
			if( comb_chars)
			{
				i += comb_size ;
			}
		}
	}

	x_window_translate_coordinates( &screen->window, 0, 0, &win_x, &win_y, &unused) ;

	*x += win_x + screen->window.margin ;
	*y += win_y + screen->window.margin ;

#ifdef  __DEBUG
	kik_debug_printf( KIK_DEBUG_TAG " im spot => x %d y %d\n" , *x , *y) ;
#endif

	return  1 ;
}

static u_int
get_line_height(
	void *  p
	)
{
	return  x_line_height( (x_screen_t*) p) ;
}

static int
is_vertical(
	void *  p
	)
{
	if( ml_term_get_vertical_mode( ( (x_screen_t *) p)->term))
	{
		return  1 ;
	}
	else
	{
		return  0 ;
	}
}

static int
draw_preedit_str(
	void *  p ,
	ml_char_t *  chars ,
	u_int  num_of_chars ,
	int  cursor_offset
	)
{
	x_screen_t *  screen ;
	ml_line_t *  line ;
	x_font_t *  xfont ;
	int  x ;
	int  y ;
	u_int  total_width ;
	u_int  i ;
	u_int  start ;
	u_int  beg_row ;
	u_int  end_row ;
	u_int  row ;
	int  preedit_cursor_x ;
	int  preedit_cursor_y ;

	screen = p ;

	if( screen->is_preediting)
	{
		if( ! ml_term_get_vertical_mode( screen->term))
		{
			for( row = screen->im_preedit_beg_row ; row <= screen->im_preedit_end_row ; row++)
			{
				if( ( line = ml_term_get_line_in_screen( screen->term , row)))
				{
					y = convert_row_to_y( screen , row) ;
					draw_line( screen , line , y) ;
				}
			}
		}
		else
		{
			x_window_update( &screen->window, UPDATE_SCREEN) ;
		}
	}

	if( ! num_of_chars)
	{
		screen->is_preediting = 0 ;

		return  0 ;
	}

	screen->is_preediting = 1 ;

	if( ( line = ml_term_get_cursor_line( screen->term)) == NULL ||
		ml_line_is_empty( line))
	{
	#ifdef  DEBUG
		kik_warn_printf( KIK_DEBUG_TAG " cursor line doesn't exist ?.\n") ;
	#endif

		return  0 ;
	}

	if( ! ml_term_get_vertical_mode( screen->term))
	{
		int  row ;

		row = ml_term_cursor_row_in_screen( screen->term) ;

		if( row < 0)
		{
			return  0 ;
		}

		beg_row = row ;
	}
	else if( ml_term_get_vertical_mode( screen->term) == VERT_RTL)
	{
		u_int  ncols ;

		ncols = ml_term_get_cols( screen->term) ;
		beg_row = ml_term_cursor_col( screen->term) ;
		beg_row = ncols - beg_row - 1;
	}
	else /* VERT_LTR */
	{
		beg_row = ml_term_cursor_col( screen->term) ;
	}
	end_row = beg_row ;

	x = convert_char_index_to_x_with_shape( screen , line ,
				ml_term_cursor_char_index( screen->term)) ;
	y = convert_row_to_y( screen , ml_term_cursor_row_in_screen( screen->term)) ;

	preedit_cursor_x = x ;
	preedit_cursor_y = y ;

	total_width = 0 ;

	for( i = 0 , start = 0 ; i < num_of_chars ; i++)
	{
		u_int  width ;
		int  need_wraparound = 0 ;
		int  _x ;
		int  _y ;

		xfont = x_get_font( screen->font_man , ml_char_font( &chars[i])) ;
		width = x_calculate_char_width( xfont ,
						ml_char_bytes( &chars[i]) ,
						ml_char_size( &chars[i]) ,
						ml_char_cs( &chars[i]) , NULL) ;

		total_width += width ;

		if( ! ml_term_get_vertical_mode( screen->term))
		{
			if( x + total_width > screen->window.width)
			{
				need_wraparound = 1 ;
				_x = 0 ;
				_y = y + x_line_height( screen) ;
				end_row++ ;
			}
		}
		else if( ml_term_get_vertical_mode( screen->term) == VERT_RTL)
		{
			need_wraparound = 1 ;
			_x = x ;
			_y = y + x_line_height( screen) ;
			start = i ;

			if( _y > screen->window.height)
			{
				y = 0 ;
				_y = x_line_height( screen) ;
				_x = x = x - x_line_height( screen) ;
				end_row++ ;
			}
		}
		else /* VERT_LTR */
		{
			need_wraparound = 1 ;
			_x = x ;
			_y = y + x_line_height( screen) ;
			start = i ;
			
			if( _y > screen->window.height)
			{
				y = 0 ;
				_y = x_line_height( screen) ;
				_x = x = x + x_line_height( screen) ;
				end_row++ ;
			}
		}

		if( i == cursor_offset - 1)
		{
			if ( ! ml_term_get_vertical_mode( screen->term))
			{
				preedit_cursor_x = x + total_width ;
				preedit_cursor_y = y ;
			}
			else
			{
				preedit_cursor_x = x ;
				preedit_cursor_y = _y ;
			}
		}

		if( need_wraparound)
		{
			if( ! x_draw_str( &screen->window , screen->font_man ,
					screen->color_man , &chars[start] ,
					i - start + 1 , x , y ,
					x_line_height( screen) ,
					x_line_ascent( screen) ,
					x_line_top_margin( screen) ,
					x_line_bottom_margin( screen) ,
					screen->hide_underline))
			{
				return  0 ;
			}

			x = _x ;
			y = _y ;
			start = i;
			total_width = width ;
		}

		if( ml_term_get_vertical_mode( screen->term))
		{
			continue ;
		}

		if( i == num_of_chars - 1) /* last? */
		{
			if( ! x_draw_str( &screen->window , screen->font_man ,
					screen->color_man , &chars[start] ,
					i - start + 1 , x , y ,
					x_line_height( screen) ,
					x_line_ascent( screen) ,
					x_line_top_margin( screen) ,
					x_line_bottom_margin( screen) ,
					screen->hide_underline))
			{
				return  0 ;
			}
		}

	}

	if( cursor_offset == num_of_chars)
	{
		if ( ! ml_term_get_vertical_mode( screen->term))
		{
			preedit_cursor_x = x + total_width;
			preedit_cursor_y = y ;
		}
		else
		{
			preedit_cursor_x = x ;
			preedit_cursor_y = y ;
		}
	}

	if( cursor_offset >= 0)
	{
		if( ! ml_term_get_vertical_mode( screen->term))
		{
			x_window_draw_line( &screen->window,
				preedit_cursor_x + 1 ,
				preedit_cursor_y + x_line_top_margin( screen) + 2 ,
				preedit_cursor_x + 1,
				preedit_cursor_y + x_line_height( screen)) ;
		}
		else
		{
			x_window_draw_line( &screen->window,
				preedit_cursor_x + x_line_top_margin( screen) + 2 ,
				preedit_cursor_y + 2 ,
				preedit_cursor_x + x_line_top_margin( screen) + xfont->height ,
				preedit_cursor_y + 2 ) ;
		}
	}

	ml_term_set_modified_lines_in_screen( screen->term , beg_row , end_row) ;

	screen->im_preedit_beg_row = beg_row ;
	screen->im_preedit_end_row = end_row ;

	return  1 ;
}

/* used for changing IM from plugin side */
static void
im_changed(
	void *  p ,
	char *  input_method
	)
{
	x_screen_t *  screen ;
	x_im_t *  new ;

	screen = p ;

	if( !( input_method = strdup( input_method)))
	{
		return;
	}

	if( !( new = x_im_new( ml_term_get_encoding( screen->term) ,
				&screen->im_listener ,
				input_method , screen->mod_ignore_mask)))
	{
		free( input_method);
		return ;
	}

	free( screen->input_method) ;
	screen->input_method = input_method ; /* strdup'ed one */

	x_im_delete( screen->im) ;
	screen->im = new ;
}

static int
compare_key_state_with_modmap(
	void *  p ,
	u_int  state ,
	int *  is_shift ,
	int *  is_lock ,
	int *  is_ctl ,
	int *  is_alt ,
	int *  is_meta ,
	int *  is_numlock ,
	int *  is_super ,
	int *  is_hyper
	)
{
	x_screen_t *  screen ;
	XModifierKeymap *  mod_map ;
	u_int  mod_mask[] = { Mod1Mask , Mod2Mask , Mod3Mask , Mod4Mask , Mod5Mask} ;

	screen = p ;

	if( is_shift)
	{
		*is_shift = 0 ;
	}
	if( is_lock)
	{
		*is_lock = 0 ;
	}
	if( is_ctl)
	{
		*is_ctl = 0 ;
	}
	if( is_alt)
	{
		*is_alt = 0 ;
	}
	if( is_meta)
	{
		*is_meta = 0 ;
	}
	if( is_numlock)
	{
		*is_numlock = 0 ;
	}
	if( is_super)
	{
		*is_super = 0 ;
	}
	if( is_hyper)
	{
		*is_hyper = 0 ;
	}

	if( is_shift && (state & ShiftMask))
	{
		*is_shift = 1 ;
	}

	if( is_lock && (state & LockMask))
	{
		*is_lock = 1 ;
	}

	if( is_ctl && (state & ControlMask))
	{
		*is_ctl = 1 ;
	}

	if( ! ( mod_map = x_window_get_modifier_mapping( &screen->window)))
	{
		/* Assume win32 */
		if( is_alt && (state & ModMask))
		{
			*is_alt = 1 ;
		}
	}
	else
	{
		int  i ;

		for( i = 0 ; i < 5 ; i++)
		{
			int  index ;
			int  mod1_index ;

			if( ! (state & mod_mask[i]))
			{
				continue ;
			}

			/* skip shift/lock/control */
			mod1_index = mod_map->max_keypermod * 3 ;

			for( index = mod1_index + (mod_map->max_keypermod * i) ;
			     index < mod1_index + (mod_map->max_keypermod * (i + 1)) ;
			     index ++)
			{
				KeySym  sym ;

				sym = XKeycodeToKeysym(  screen->window.disp->display ,
							mod_map->modifiermap[index] , 0) ;

				switch (sym)
				{
				case  XK_Meta_R:
				case  XK_Meta_L:
					if( is_meta)
					{
						*is_meta = 1 ;
					}
					break ;
				case  XK_Alt_R:
				case  XK_Alt_L:
					if( is_alt)
					{
						*is_alt = 1 ;
					}
					break ;
				case  XK_Super_R:
				case  XK_Super_L:
					if( is_super)
					{
						*is_super = 1 ;
					}
					break ;
				case  XK_Hyper_R:
				case  XK_Hyper_L:
					if( is_hyper)
					{
						*is_hyper = 1 ;
					}
					break ;
				case  XK_Num_Lock:
					if( is_numlock)
					{
						*is_numlock = 1 ;
					}
				default:
					break ;
				}
			}
		}
	}

	return  1 ;
}

static void
write_to_term(
	void *  p ,
	u_char *  str ,		/* must be same as term encoding */
	size_t  len
	)
{
	x_screen_t *  screen ;

	screen = p ;

#ifdef  __DEBUG
	{
		size_t  count ;

		kik_debug_printf( KIK_DEBUG_TAG " written str: ") ;

		for( count = 0 ; count < len ; count++)
		{
			kik_msg_printf( "%.2x ", str[count]) ;
		}
		kik_msg_printf( "\n") ;
	}
#endif

	ml_term_write( screen->term , str , len , 0) ;
}

static x_display_t *
get_display(
	void *  p
	)
{
	return  ((x_screen_t*)p)->window.disp ;
}

static x_font_manager_t *
get_font_man(
	void *  p
	)
{
	return  ((x_screen_t*)p)->font_man ;
}

static x_color_manager_t *
get_color_man(
	void *  p
	)
{
	return((x_screen_t *)p)->color_man ;
}


/*
 * callbacks of ml_xterm_event_listener_t
 */

static void
start_vt100_cmd(
	void *  p
	)
{
	x_screen_t *  screen ;

	screen = p ;

#if  0
	if( ! ml_term_is_backscrolling( screen->term) ||
		ml_term_is_backscrolling( screen->term) == BSM_DEFAULT)
	{
		x_stop_selecting( &screen->sel) ;
	}
#endif

	if( screen->sel.is_reversed)
	{
		if( x_is_selecting( &screen->sel))
		{
			x_restore_selected_region_color_except_logs( &screen->sel) ;
		}
		else
		{
			x_restore_selected_region_color( &screen->sel) ;
		}

		if( ! ml_term_logical_visual_is_reversible( screen->term))
		{
			/*
			 * If indic logical<=>visual conversion is enabled, x_window_update()
			 * in stop_vt100_cmd() can't reflect x_restore_selected_region_color*()
			 * functions above to screen.
			 */
			x_window_update( &screen->window , UPDATE_SCREEN) ;
		}
	}

	unhighlight_cursor( screen , 0) ;

	/*
	 * ml_screen_logical() is called in ml_term_unhighlight_cursor(), so
	 * not called directly from here.
	 */

	/* processing_vtseq == -1 means loopback processing. */
	if( screen->processing_vtseq != -1)
	{
		screen->processing_vtseq = 1 ;
	}
}

static void
stop_vt100_cmd(
	void *  p
	)
{
	x_screen_t *  screen ;

	screen = p ;

	screen->processing_vtseq = 0 ;

	if( x_is_selecting( &screen->sel))
	{
		/*
		 * XXX Fixme XXX
		 * If some lines are scrolled out after start_vt100_cmd(),
		 * color of them is not reversed.
		 */
		x_reverse_selected_region_color_except_logs( &screen->sel) ;
	}

	if( exit_backscroll_by_pty)
	{
		exit_backscroll_mode( screen) ;
	}

	if( ( screen->font_or_color_config_updated & 0x1) &&
		screen->system_listener->font_config_updated)
	{
		(*screen->system_listener->font_config_updated)() ;
	}

	if( ( screen->font_or_color_config_updated & 0x2) &&
		screen->system_listener->color_config_updated)
	{
		(*screen->system_listener->color_config_updated)() ;
	}

	screen->font_or_color_config_updated = 0 ;

	x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;
}

static void
xterm_resize(
	void *  p ,
	u_int  width ,
	u_int  height
	)
{
	x_screen_t *  screen ;

	screen = p ;

	if( width == 0 || height == 0)
	{
		/* ml_term_t is already resized. */
		resize_window( screen) ;
	}
	/* screen will redrawn in window_resized() */
	else if( x_window_resize( &screen->window , width , height ,
			NOTIFY_TO_PARENT|LIMIT_RESIZE))
	{
		/*
		 * !! Notice !!
		 * x_window_resize() will invoke ConfigureNotify event but window_resized()
		 * won't be called , since xconfigure.width , xconfigure.height are the same
		 * as the already resized window.
		 */
		if( screen->window.window_resized)
		{
			(*screen->window.window_resized)( &screen->window) ;
		}
	}
}

static void
xterm_reverse_video(
	void *  p ,
	int  do_reverse
	)
{
	x_screen_t *  screen ;

	screen = p ;

	if( do_reverse)
	{
		if( ! x_color_manager_reverse_video( screen->color_man))
		{
			return ;
		}
	}
	else
	{
		if( ! x_color_manager_restore_video( screen->color_man))
		{
			return ;
		}
	}

	x_window_set_fg_color( &screen->window ,
		x_get_xcolor( screen->color_man , ML_FG_COLOR)) ;
	x_window_set_bg_color( &screen->window ,
		x_get_xcolor( screen->color_man , ML_BG_COLOR)) ;

	ml_term_set_modified_all_lines_in_screen( screen->term) ;
	
	x_window_update( &screen->window, UPDATE_SCREEN) ;
}

static void
xterm_set_mouse_report(
	void *  p ,
	ml_mouse_report_mode_t  mode
	)
{
	x_screen_t *  screen ;

	screen = p ;

	if( mode)
	{
		x_stop_selecting( &screen->sel) ;
		restore_selected_region_color_instantly( screen) ;
		exit_backscroll_mode( screen) ;
	}
	else
	{
		memset( screen->prev_mouse_report_seq , 0 , 5) ;
	}

	if( mode == ANY_EVENT_MOUSE_REPORT)
	{
		screen->window.pointer_motion = pointer_motion ;
		x_window_add_event_mask( &screen->window , PointerMotionMask) ;
	}
	else
	{
		screen->window.pointer_motion = NULL ;
		x_window_remove_event_mask( &screen->window , PointerMotionMask) ;
	}
}

static void
xterm_set_window_name(
	void *  p ,
	u_char *  name
	)
{
	x_screen_t *  screen ;
#ifdef  USE_WIN32GUI
	u_char *  buf ;
	size_t  len ;
	mkf_parser_t *  parser ;
#endif

	screen = p ;

#ifdef  USE_WIN32GUI
	if( name && *name &&
	    ( buf = alloca( ( len = strlen(name) + 1) * 4)) && /* 4 == UTF16 surrogate pair. */
	    ( parser = ml_parser_new( ml_term_get_encoding( screen->term))))
	{
		(*parser->init)( parser) ;
		(*parser->set_str)( parser , name , len) ;

		(*screen->utf_conv->init)( screen->utf_conv) ;
		if( (*screen->utf_conv->convert)( screen->utf_conv ,
				buf , len * 4 , parser) > 0)
		{
			name = buf ;
		}

		(*parser->delete)( parser) ;
	}
#endif

	x_set_window_name( &screen->window , name) ;
}

static void
xterm_bel(
	void *  p
	)
{
	x_screen_t *  screen ;

	screen = p ;

	if( screen->bel_mode == BEL_SOUND)
	{
		x_window_bell( &screen->window , 0) ;
	}
	else if( screen->bel_mode == BEL_VISUAL)
	{
		x_window_bell( &screen->window , 1) ;
	}
}

static int
xterm_im_is_active(
	void *  p
	)
{
	x_screen_t *  screen ;

	screen = p ;

	if( screen->im)
	{
		return  (*screen->im->is_active)( screen->im) ;
	}

	return  x_xic_is_active( &screen->window) ;
}

static void
xterm_switch_im_mode(
	void *  p
	)
{
	x_screen_t *  screen ;

	screen = p ;

	if( screen->im)
	{
		(*screen->im->switch_mode)( screen->im) ;

		return ;
	}

	x_xic_switch_mode( &screen->window) ;
}

static void
xterm_set_selection(
	void *  p ,
	ml_char_t *  str ,	/* Should be free'ed by the event listener. */
	u_int  len
	)
{
	x_screen_t *  screen ;

	screen = p ;

	if( ! x_window_set_selection_owner( &screen->window , CurrentTime))
	{
		return ;
	}

	if( screen->sel.sel_str)
	{
		ml_str_delete( screen->sel.sel_str , screen->sel.sel_len) ;
	}

	screen->sel.sel_str = str ;
	screen->sel.sel_len = len ;
}

static int
xterm_get_rgb(
	void *  p ,
	u_int8_t *  red ,
	u_int8_t *  green ,
	u_int8_t *  blue ,
	ml_color_t  color
	)
{
	x_screen_t *  screen ;
	x_color_t *  xcolor ;

	screen = p ;

	if( ! ( xcolor = x_get_xcolor( screen->color_man , color)) ||
	    ! x_get_xcolor_rgba( red , green , blue , NULL , xcolor))
	{
		return  0 ;
	}

	return  1 ;
}

static int
xterm_get_window_size(
	void *  p ,
	u_int *  width ,
	u_int *  height
	)
{
	x_screen_t *  screen ;

	screen = p ;

	*width = screen->window.width ;
	*height = screen->window.height ;

	return  1 ;
}

#ifdef  ENABLE_SIXEL
static ml_char_t *
xterm_get_picture_data(
	void *  p ,
	char *  file_path ,
	int *  num_of_cols ,	/* can be 0 */
	int *  num_of_rows	/* can be 0 */
	)
{
	x_screen_t *  screen ;
	u_int  width ;
	u_int  height ;
	u_int  col_width ;
	u_int  line_height ;
	int  idx ;

	screen = p ;

	if( ml_term_get_vertical_mode( screen->term) ||
	    ( col_width = x_col_width(screen)) >= 32 ||
	    ( line_height = x_line_height(screen)) >= 32)
	{
		return  NULL ;
	}

	width = (*num_of_cols) * col_width ;
	height = (*num_of_rows) * line_height ;

	if( ( idx = x_load_inline_picture( screen->window.disp , file_path ,
			&width , &height , col_width , line_height , screen->term)) != -1)
	{
		u_int16_t  bytes[2] ;
		ml_char_t *  buf ;
		int  max_num_of_cols ;

		bytes[0] = idx ;

		max_num_of_cols = ml_term_get_cursor_line( screen->term)->num_of_chars -
				ml_term_cursor_col( screen->term) ;
		if( ( *num_of_cols = (width + col_width - 1) / col_width) > max_num_of_cols)
		{
			*num_of_cols = max_num_of_cols ;
		}

		*num_of_rows = (height + line_height - 1) / line_height ;

		if( ( buf = ml_str_new( (*num_of_cols) * (*num_of_rows))))
		{
			ml_char_t *  buf_p ;
			int  col ;
			int  row ;

			buf_p = buf ;
			for( row = 0 ; row < *num_of_rows ; row++)
			{
				for( col = 0 ; col < *num_of_cols ; col++)
				{
					ml_char_copy( buf_p , ml_sp_ch()) ;

					bytes[1] = col * (*num_of_rows) + row ;

					ml_char_combine( buf_p ++ ,
						(u_char*)bytes , 4 , PICTURE_CHARSET ,
						0 , 0 , 0 , 0 , 0 , 0) ;
				}
			}

			return  buf ;
		}
	}

	return  NULL ;
}
#else
#define  xterm_get_picture_data NULL
#endif	/* ENABLE_SIXEL */


/*
 * callbacks of ml_pty_event_listener_t
 */

static void
pty_closed(
	void *  p
	)
{
	x_screen_t *  screen ;

	screen = p ;

	/*
	 * Don't use x_screen_detach() here because screen->term is deleting just now.
	 */
	 
	/* This should be done before screen->term is NULL */
	x_sel_clear( &screen->sel) ;

	/*
	 * term is being deleted in this context.
	 * ml_close_dead_terms => ml_term_delete => ml_pty_delete => pty_closed.
	 */
	screen->term = NULL ;

	(*screen->system_listener->pty_closed)( screen->system_listener->self , screen) ;
}

#ifdef  USE_WIN32API
static void
pty_read_ready(
  	void *  p
  	)
{
	x_screen_t *  screen = p ;

	/* Occur dummy event(WM_USER_PAINT) to exit GetMessage() loop. */
	x_window_update( &screen->window, 0) ;
}
#endif


/* --- global functions --- */

void
x_set_button3_behavior(
	const char *  mode
	)
{
	if( strcmp( mode , "xterm") == 0)
	{
		button3_open = 0 ;
	}
	else if( strlen( mode) == 5 && '1' <= mode[4] && mode[4] <= '3')
	{
		/* menu1, menu2, menu3 */
		button3_open = mode[4] - '0' ;
	}
	else if( strcmp( mode , "none") == 0)
	{
		/* Hidden option for libvte */
		button3_open = 5 ;
	}
	else
	{
		char *  p ;

		if( ( p = strdup( mode)))	/* XXX Not free'ed. Leaked */
		{
			free( button3_command) ;
			button3_command = p ;
			/* Hidden option (e.g. w3m, lynx) */
			button3_open = 4 ;
		}
	}
}

void
x_exit_backscroll_by_pty(
	int  flag
	)
{
	exit_backscroll_by_pty = flag ;
}

#ifdef  USE_IM_CURSOR_COLOR
void
x_set_im_cursor_color(
	char *  color
	)
{
	im_cursor_color = strdup( color) ;
}
#endif


/*
 * If term is NULL, don't call other functions of x_screen until
 * x_screen_attach() is called. (x_screen_attach() must be called
 * before x_screen_t is realized.)
 */
x_screen_t *
x_screen_new(
	ml_term_t *  term ,		/* can be NULL */
	x_font_manager_t *  font_man ,
	x_color_manager_t *  color_man ,
	x_termcap_entry_t *  termcap ,
	u_int  brightness ,
	u_int  contrast ,
	u_int  gamma ,
	u_int  alpha ,
	u_int  fade_ratio ,
	x_shortcut_t *  shortcut ,
	u_int  screen_width_ratio ,
	u_int  screen_height_ratio ,
	char *  mod_meta_key ,
	x_mod_meta_mode_t  mod_meta_mode ,
	x_bel_mode_t  bel_mode ,
	int  receive_string_via_ucs ,
	char *  pic_file_path ,
	int  use_transbg ,
	int  use_vertical_cursor ,
	int  big5_buggy ,
	char *  conf_menu_path_1 ,
	char *  conf_menu_path_2 ,
	char *  conf_menu_path_3 ,
	int  use_extended_scroll_shortcut ,
	int  override_redirect ,
	u_int  line_space ,
	char *  input_method ,
	int  allow_osc52 ,
	int  blink_cursor ,
	int  margin ,
	int  hide_underline
	)
{
	x_screen_t *  screen ;

	if( ( screen = calloc( 1 , sizeof( x_screen_t))) == NULL)
	{
	#ifdef  DEBUG
		kik_warn_printf( KIK_DEBUG_TAG " malloc failed.\n") ;
	#endif

		return  NULL ;
	}

	screen->line_space = line_space ;
	screen->use_vertical_cursor = use_vertical_cursor ;
	screen->font_man = font_man ;
	screen->color_man = color_man ;

	screen->sel_listener.self = screen ;
	screen->sel_listener.select_in_window = select_in_window ;
	screen->sel_listener.reverse_color = reverse_color ;
	screen->sel_listener.restore_color = restore_color ;

	if( ! x_sel_init( &screen->sel , &screen->sel_listener))
	{
	#ifdef  DEBUG
		kik_warn_printf( KIK_DEBUG_TAG " x_sel_init failed.\n") ;
	#endif
		
		goto  error ;
	}
	
	if( pic_file_path)
	{
		screen->pic_file_path = strdup( pic_file_path) ;
	}

	screen->pic_mod.brightness = brightness ;
	screen->pic_mod.contrast = contrast ;
	screen->pic_mod.gamma = gamma ;
	if( alpha != 255 || ( ! pic_file_path && ! use_transbg))
	{
		screen->pic_mod.alpha = alpha ;
	}

	/*
	 * blend_xxx members will be set in window_realized().
	 */
#if  0
	x_get_xcolor_rgba( &screen->pic_mod.blend_red , &screen->pic_mod.blend_green ,
			&screen->pic_mod.blend_blue , NULL ,
			x_get_xcolor( screen->color_man , ML_BG_COLOR)) ;
#endif

	/* Be sure to use alpha here instead of screen->pic_mod.alpha which was modified above. */
	x_color_manager_change_alpha( color_man , alpha) ;

	screen->fade_ratio = fade_ratio ;

	screen->screen_width_ratio = screen_width_ratio ;
	screen->screen_height_ratio = screen_height_ratio ;

	/* screen->term must be set before screen_height() */
	screen->term = term ;

	if( x_window_init( &screen->window ,
		screen->term ? screen_width( screen) : x_col_width(screen) ,
		screen->term ? screen_height( screen) : x_line_height(screen) ,
		x_col_width( screen) , x_line_height( screen) , 0 , 0 ,
		x_col_width( screen) , x_line_height( screen) , margin , 0) == 0) /* min: 1x1 */
	{
	#ifdef  DEBUG
		kik_warn_printf( KIK_DEBUG_TAG " x_window_init failed.\n") ;
	#endif
		
		goto  error ;
	}

	screen->screen_listener.self = screen ;
	screen->screen_listener.window_scroll_upward_region = window_scroll_upward_region ;
	screen->screen_listener.window_scroll_downward_region = window_scroll_downward_region ;
	screen->screen_listener.line_scrolled_out = line_scrolled_out ;

	screen->xterm_listener.self = screen ;
	screen->xterm_listener.start = start_vt100_cmd ;
	screen->xterm_listener.stop = stop_vt100_cmd ;
	screen->xterm_listener.resize = xterm_resize ;
	screen->xterm_listener.reverse_video = xterm_reverse_video ;
	screen->xterm_listener.set_mouse_report = xterm_set_mouse_report ;
	screen->xterm_listener.set_window_name = xterm_set_window_name ;
	screen->xterm_listener.set_icon_name = x_set_icon_name ;
	screen->xterm_listener.bel = xterm_bel ;
	screen->xterm_listener.im_is_active = xterm_im_is_active ;
	screen->xterm_listener.switch_im_mode = xterm_switch_im_mode ;
	screen->xterm_listener.set_selection = (allow_osc52 ? xterm_set_selection : NULL) ;
	screen->xterm_listener.get_rgb = xterm_get_rgb ;
	screen->xterm_listener.get_window_size = xterm_get_window_size ;
	screen->xterm_listener.get_picture_data = xterm_get_picture_data ;

	screen->config_listener.self = screen ;
	screen->config_listener.exec = x_screen_exec_cmd ;
	screen->config_listener.set = x_screen_set_config ;
	screen->config_listener.get = get_config ;
	screen->config_listener.saved = NULL ;
	screen->config_listener.set_font = set_font_config ;
	screen->config_listener.get_font = get_font_config ;
	screen->config_listener.set_color = set_color_config ;

	screen->pty_listener.self = screen ;
	screen->pty_listener.closed = pty_closed ;
#ifdef  USE_WIN32API
	screen->pty_listener.read_ready = pty_read_ready ;
#endif

	if( screen->term)
	{
		ml_term_attach( term , &screen->xterm_listener , &screen->config_listener ,
			&screen->screen_listener , &screen->pty_listener) ;
		
		/*
		 * setup_encoding_aux() in update_special_visual() must be called
		 * after x_window_init()
		 */
		update_special_visual( screen) ;
	}

	screen->xim_listener.self = screen ;
	screen->xim_listener.get_spot = get_spot ;
	screen->xim_listener.get_fontset = get_fontset ;
	screen->xim_listener.get_fg_color = get_fg_color ;
	screen->xim_listener.get_bg_color = get_bg_color ;
	screen->window.xim_listener = &screen->xim_listener ;

	if( input_method)
	{
		screen->input_method = strdup( input_method) ;
	}

	screen->im_listener.self = screen ;
	screen->im_listener.get_spot = get_im_spot ;
	screen->im_listener.get_line_height = get_line_height ;
	screen->im_listener.is_vertical = is_vertical ;
	screen->im_listener.draw_preedit_str = draw_preedit_str ;
	screen->im_listener.im_changed = im_changed ;
	screen->im_listener.compare_key_state_with_modmap = compare_key_state_with_modmap ;
	screen->im_listener.write_to_term = write_to_term ;
	screen->im_listener.get_display = get_display ;
	screen->im_listener.get_font_man = get_font_man ;
	screen->im_listener.get_color_man = get_color_man ;

	x_window_set_cursor( &screen->window , XC_xterm) ;

	/*
	 * event call backs.
	 */

	x_window_init_event_mask( &screen->window ,
		ButtonPressMask | ButtonMotionMask | ButtonReleaseMask | KeyPressMask) ;

	screen->window.window_realized = window_realized ;
	screen->window.window_finalized = window_finalized ;
	screen->window.window_deleted = window_deleted ;
	screen->window.mapping_notify = mapping_notify ;
	screen->window.window_exposed = window_exposed ;
	screen->window.update_window = update_window ;
	screen->window.window_focused = window_focused ;
	screen->window.window_unfocused = window_unfocused ;
	screen->window.key_pressed = key_pressed ;
	screen->window.window_resized = window_resized ;
	screen->window.pointer_motion = NULL ;
	screen->window.button_motion = button_motion ;
	screen->window.button_released = button_released ;
	screen->window.button_pressed = button_pressed ;
	screen->window.button_press_continued = button_press_continued ;
	screen->window.selection_cleared = selection_cleared ;
	screen->window.xct_selection_requested = xct_selection_requested ;
	screen->window.utf_selection_requested = utf_selection_requested ;
	screen->window.xct_selection_notified = xct_selection_notified ;
	screen->window.utf_selection_notified = utf_selection_notified ;
#ifndef  DISABLE_XDND
	screen->window.set_xdnd_config = set_xdnd_config ;
#endif
	if( blink_cursor)
	{
		screen->window.idling = idling ;
	}

	if( use_transbg)
	{
		x_window_set_transparent( &screen->window ,
			x_screen_get_picture_modifier( screen)) ;
	}

	if( conf_menu_path_1)
	{
		screen->conf_menu_path_1 = strdup( conf_menu_path_1) ;
	}

	if( conf_menu_path_2)
	{
		screen->conf_menu_path_2 = strdup( conf_menu_path_2) ;
	}

	if( conf_menu_path_3)
	{
		screen->conf_menu_path_3 = strdup( conf_menu_path_3) ;
	}

	screen->shortcut = shortcut ;
	screen->termcap = termcap ;

	if( mod_meta_key && strcmp( mod_meta_key , "none") != 0)
	{
		screen->mod_meta_key = strdup( mod_meta_key) ;
	}

	screen->mod_meta_mode = mod_meta_mode ;
	screen->mod_meta_mask = 0 ;		/* set later in get_mod_meta_mask() */
	screen->mod_ignore_mask = ~0 ;		/* set later in get_mod_ignore_mask() */

	screen->bel_mode = bel_mode ;
	screen->use_extended_scroll_shortcut = use_extended_scroll_shortcut ;
	screen->borderless = override_redirect ;
	screen->font_or_color_config_updated = 0 ;

	screen->hide_underline = hide_underline ;

	/*
	 * for receiving selection.
	 */

#ifdef  USE_WIN32GUI
	if( ( screen->utf_parser = mkf_utf16le_parser_new()) == NULL)
	{
		goto  error ;
	}

	if( ( screen->xct_parser = ml_parser_new( ml_get_char_encoding( kik_get_codeset_win32())))
		== NULL)
	{
		goto  error ;
	}
#else
	if( ( screen->utf_parser = ml_parser_new( ML_UTF8)) == NULL)
	{
		goto  error ;
	}

	if( ( screen->xct_parser = mkf_xct_parser_new()) == NULL)
	{
		goto  error ;
	}
#endif

	/*
	 * for sending selection
	 */

	if( ( screen->ml_str_parser = ml_str_parser_new()) == NULL)
	{
		goto  error ;
	}

#ifdef  USE_WIN32GUI
	if( ( screen->utf_conv = mkf_utf16le_conv_new()) == NULL)
	{
		goto  error ;
	}

	if( ( screen->xct_conv = ml_conv_new( ml_get_char_encoding( kik_get_codeset_win32()))) == NULL)
	{
		goto  error ;
	}
#else
	if( ( screen->utf_conv = ml_conv_new( ML_UTF8)) == NULL)
	{
		goto  error ;
	}

	if( big5_buggy)
	{
		if( ( screen->xct_conv = mkf_xct_big5_buggy_conv_new()) == NULL)
		{
			goto  error ;
		}
	}
	else if( ( screen->xct_conv = mkf_xct_conv_new()) == NULL)
	{
		goto  error ;
	}
#endif

	screen->receive_string_via_ucs = receive_string_via_ucs ;

	return  screen ;

error:
	if( screen->utf_parser)
	{
		(*screen->utf_parser->delete)( screen->utf_parser) ;
	}

	if( screen->xct_parser)
	{
		(*screen->xct_parser->delete)( screen->xct_parser) ;
	}

	if( screen->ml_str_parser)
	{
		(*screen->ml_str_parser->delete)( screen->ml_str_parser) ;
	}

	if( screen->utf_conv)
	{
		(*screen->utf_conv->delete)( screen->utf_conv) ;
	}

	if( screen->xct_conv)
	{
		(*screen->xct_conv->delete)( screen->xct_conv) ;
	}

	free( screen->pic_file_path) ;
	free( screen->conf_menu_path_1) ;
	free( screen->conf_menu_path_2) ;
	free( screen->conf_menu_path_3) ;
	free( screen->mod_meta_key) ;
	free( screen->input_method) ;
	free( screen) ;

	return  NULL ;
}

int
x_screen_delete(
	x_screen_t *  screen
	)
{
	if( screen->term)
	{
		ml_term_detach( screen->term) ;
	}

	x_sel_final( &screen->sel) ;

	if( screen->bg_pic)
	{
		x_release_picture( screen->bg_pic) ;
	}
	free( screen->pic_file_path) ;
	
	if( screen->icon)
	{
		x_release_icon_picture( screen->icon) ;
	}

	free( screen->mod_meta_key) ;
	free( screen->conf_menu_path_1) ;
	free( screen->conf_menu_path_2) ;
	free( screen->conf_menu_path_3) ;

	if( screen->utf_parser)
	{
		(*screen->utf_parser->delete)( screen->utf_parser) ;
	}

	if( screen->xct_parser)
	{
		(*screen->xct_parser->delete)( screen->xct_parser) ;
	}

	if( screen->ml_str_parser)
	{
		(*screen->ml_str_parser->delete)( screen->ml_str_parser) ;
	}

	if( screen->utf_conv)
	{
		(*screen->utf_conv->delete)( screen->utf_conv) ;
	}

	if( screen->xct_conv)
	{
		(*screen->xct_conv->delete)( screen->xct_conv) ;
	}

	free( screen->input_method) ;

	if( screen->im)
	{
		x_im_delete( screen->im) ;
	}

	free( screen) ;

	return  1 ;
}

/*
 * Be careful that mlterm can die if x_screen_attach is called
 * before x_screen_t is realized, because callbacks of ml_term
 * may touch uninitialized object of x_screen_t.
 */
int
x_screen_attach(
	x_screen_t *  screen ,
	ml_term_t *  term
	)
{
	if( screen->term)
	{
		return  0 ;
	}

	screen->term = term ;

	ml_term_attach( term , &screen->xterm_listener , &screen->config_listener ,
		&screen->screen_listener , &screen->pty_listener) ;

	if( ! screen->window.my_window)
	{
		return  1 ;
	}

	usascii_font_cs_changed( screen , ml_term_get_encoding( screen->term)) ;

	update_special_visual( screen) ;
	/* Even if update_special_visual succeeded or not, all screen should be redrawn. */
	ml_term_set_modified_all_lines_in_screen( screen->term) ;

	if( HAS_SCROLL_LISTENER(screen,term_changed))
	{
		(*screen->screen_scroll_listener->term_changed)(
			screen->screen_scroll_listener->self ,
			ml_term_get_log_size( screen->term) ,
			ml_term_get_num_of_logged_lines( screen->term)) ;
	}

	/*
	 * if ml_term_(icon|window)_name() returns NULL, screen->window.app_name
	 * will be used in x_set_(icon|window)_name().
	 */
	xterm_set_window_name( &screen->window , ml_term_window_name( screen->term)) ;
	x_set_icon_name( &screen->window , ml_term_icon_name( screen->term)) ;

	/* reset icon to screen->term's one */
	set_icon( screen) ;

	if( screen->im)
	{
		x_im_t *  im ;

		im = screen->im ;
		screen->im = x_im_new( ml_term_get_encoding(term) ,
				&screen->im_listener , screen->input_method ,
				screen->mod_ignore_mask) ;
		/*
		 * Avoid to delete anything inside im-module by calling x_im_delete()
		 * after x_im_new().
		 */
		x_im_delete( im) ;
	}

	x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;

	/* XXX */
#ifdef  USE_FRAMEBUFFER
	x_window_resize_with_margin( &screen->window , screen->window.disp->width ,
		screen->window.disp->height , NOTIFY_TO_MYSELF) ;
#endif

	return  1 ;
}

ml_term_t *
x_screen_detach(
	x_screen_t *  screen
	)
{
	ml_term_t *  term ;

	if( screen->term == NULL)
	{
		return  NULL ;
	}

	/* This should be done before screen->term is NULL */
	x_sel_clear( &screen->sel) ;
	
#if  1
	exit_backscroll_mode( screen) ;
#endif

	ml_term_detach( screen->term) ;
	
	term = screen->term ;
	screen->term = NULL ;
	x_window_clear_all( &screen->window) ;

	return  term ;
}

int
x_screen_attached(
	x_screen_t *  screen
	)
{
	return  (screen->term != NULL) ;
}

int
x_set_system_listener(
	x_screen_t *  screen ,
	x_system_event_listener_t *  system_listener
	)
{
	if( screen->system_listener)
	{
	#ifdef  DEBUG
		kik_warn_printf( KIK_DEBUG_TAG " system listener is already set.\n") ;
	#endif

		return  0 ;
	}

	screen->system_listener = system_listener ;

	return  1 ;
}

int
x_set_screen_scroll_listener(
	x_screen_t *  screen ,
	x_screen_scroll_event_listener_t *  screen_scroll_listener
	)
{
	if( screen->screen_scroll_listener)
	{
	#ifdef  DEBUG
		kik_warn_printf( KIK_DEBUG_TAG " screen scroll listener is already set.\n") ;
	#endif

		return  0 ;
	}

	screen->screen_scroll_listener = screen_scroll_listener ;

	return  1 ;
}


/*
 * for scrollbar scroll.
 *
 * Similar processing is done in bs_xxx().
 */

int
x_screen_scroll_upward(
	x_screen_t *  screen ,
	u_int  size
	)
{
	if( ! ml_term_is_backscrolling( screen->term))
	{
		enter_backscroll_mode( screen) ;
	}

	ml_term_backscroll_upward( screen->term , size) ;

	x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;

	return  1 ;
}

int
x_screen_scroll_downward(
	x_screen_t *  screen ,
	u_int  size
	)
{
	if( ! ml_term_is_backscrolling( screen->term))
	{
		enter_backscroll_mode( screen) ;
	}

	ml_term_backscroll_downward( screen->term , size) ;

	x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;

	return  1 ;
}

int
x_screen_scroll_to(
	x_screen_t *  screen ,
	int  row
	)
{
	if( ! ml_term_is_backscrolling( screen->term))
	{
		enter_backscroll_mode( screen) ;
	}

	ml_term_backscroll_to( screen->term , row) ;

	x_window_update( &screen->window, UPDATE_SCREEN|UPDATE_CURSOR) ;

	return  1 ;
}


u_int
x_col_width(
	x_screen_t *  screen
	)
{
	return  x_get_usascii_font( screen->font_man)->width ;
}

u_int
x_line_height(
	x_screen_t *  screen
	)
{
	return  x_get_usascii_font( screen->font_man)->height + screen->line_space ;
}

u_int
x_line_ascent(
	x_screen_t *  screen
	)
{
	return  x_get_usascii_font( screen->font_man)->ascent +
			screen->line_space / 2 ;
}

u_int
x_line_top_margin(
	x_screen_t *  screen
	)
{
	return  screen->line_space / 2 ;
}

u_int
x_line_bottom_margin(
	x_screen_t *  screen
	)
{
	return  screen->line_space / 2 + screen->line_space % 2 ;
}


/*
 * Return value
 *  0 -> Not processed
 *  1 -> Processed (regardless of processing succeeded or not)
 */
int
x_screen_exec_cmd(
	x_screen_t *  screen ,
	char *  cmd
	)
{
	char *  arg ;

	if( strncmp( cmd , "mlclient" , 8) == 0)
	{
		if( HAS_SYSTEM_LISTENER(screen,mlclient))
		{
			/* processing_vtseq == -1 means loopback processing. */
			if( screen->processing_vtseq > 0)
			{
				char *  p ;

				/*
				 * Executing value of "-e" or "--initstr" option is dangerous
				 * in case 'cat dangerousfile'.
				 */
				if( ( ( p = strstr( cmd , "-e")) &&
				      p[2] < 'A' &&
				      /* XXX for mltracelog.sh */ strcmp( p , "-e cat") != 0) ||
				    ( p = strstr( cmd , "-initstr")) ||
				    ( p = strstr( cmd , "-#")))
				{
					kik_msg_printf( "Remove \"%s\" from mlclient args.\n" ,
						p) ;
					p[-1] = '\0' ;	/* Replace ' ', '\"' or '\''. */
				}
			}

			(*screen->system_listener->mlclient)(
				screen->system_listener->self ,
				cmd[8] == 'x' ? screen : NULL , cmd , stdout) ;
		}

		return  1 ;
	}

	/* Separate cmd to command string and argument string. */
	if( ( arg = strchr( cmd , ' ')))
	{
		/*
		 * If cmd is not matched below, *arg will be restored as ' '
		 * at the end of this function.
		 */
		*arg = '\0' ;

		while( *(++arg) == ' ') ;
		if( *arg == '\0')
		{
			arg = NULL ;
		}
	}

	/*
	 * Backward compatibility with mlterm 3.0.10 or before which accepts
	 * '=' like "paste=" is broken.
	 */

	if( strcmp( cmd , "paste") == 0)
	{
		yank_event_received( screen , 0) ;
	}
	else if( strcmp( cmd , "open_pty") == 0 ||
		strcmp( cmd , "select_pty") == 0)
	{
		if( HAS_SYSTEM_LISTENER(screen,open_pty))
		{
			/* arg is not NULL if cmd == "select_pty" */
			(*screen->system_listener->open_pty)(
				screen->system_listener->self , screen , arg) ;
		}
	}
	else if( strcmp( cmd , "close_pty") == 0)
	{
		/*
		 * close_pty is useful if pty doesn't react anymore and
		 * you want to kill it forcibly.
		 */

		if( HAS_SYSTEM_LISTENER(screen,close_pty))
		{
			/* If arg is NULL, screen->term will be closed. */
			(*screen->system_listener->close_pty)(
				screen->system_listener->self , screen , arg) ;
		}
	}
	else if( strcmp( cmd , "open_screen") == 0)
	{
		if( HAS_SYSTEM_LISTENER(screen,open_screen))
		{
			(*screen->system_listener->open_screen)(
				screen->system_listener->self , screen) ;
		}
	}
	else if( strcmp( cmd , "snapshot") == 0)
	{
		char **  argv ;
		int  argc ;
		ml_char_encoding_t  encoding ;
		char *  file ;

		if( arg)
		{
			argv = kik_arg_str_to_array( &argc , arg) ;
		}
		else
		{
			argc = 0 ;
			argv = NULL ;
		}

		if( argc >= 2)
		{
			encoding = ml_get_char_encoding( argv[1]) ;
		}
		else
		{
			encoding = ML_UNKNOWN_ENCODING ;
		}

		if( argc >= 1)
		{
			file = argv[0] ;
		}
		else
		{
			/* skip /dev/ */
			file = ml_term_get_slave_name( screen->term) + 5 ;
			if( strstr( file , ".."))
			{
				/* insecure file name */
				kik_msg_printf( "%s is insecure file name.\n" , file) ;

				return  1 ;
			}
		}

		snapshot( screen , encoding , file) ;
	}
	else if( strncmp( cmd , "search_" , 7) == 0)
	{
		ml_char_encoding_t  encoding ;

		if( arg && ( encoding = ml_term_get_encoding( screen->term)) != ML_UTF8)
		{
			char *  p ;
			size_t  len ;

			len = UTF_MAX_SIZE * strlen( arg) + 1 ;
			if( ( p = alloca( len)))
			{
				*(p + ml_char_encoding_convert( p , len - 1 , ML_UTF8 ,
					arg , strlen(arg) , encoding)) = '\0' ;

				arg = p ;
			}
		}

		if( strcmp( cmd + 7 , "prev") == 0)
		{
			search_find( screen , arg , 1) ;
		}
		else if( strcmp( cmd + 7 , "next") == 0)
		{
			search_find( screen , arg , 0) ;
		}
	}
	else if( strcmp( cmd , "update_all") == 0)
	{
		x_window_update_all( x_get_root_window( &screen->window)) ;
	}
	else
	{
		if( arg)
		{
			*(cmd + strlen(cmd)) = ' ' ;
		}

		return  0 ;
	}

	return  1 ;
}

/*
 * Return value
 *  0 -> Not processed
 *  1 -> Processed (regardless of processing succeeded or not)
 */
int
x_screen_set_config(
	x_screen_t *  screen,
	char *  dev ,		/* can be NULL */
	char *  key ,
	char *  value		/* can be NULL */
	)
{
	ml_term_t *  term ;

#ifdef  __DEBUG
	kik_debug_printf( KIK_DEBUG_TAG " %s=%s\n" , key , value) ;
#endif

	if( value == NULL)
	{
		value = "" ;
	}

	/*
	 * XXX
	 * 'dev' is not used for now, since many static functions used below use
	 * screen->term internally.
	 */
#if  0
	if( dev)
	{
		if( ( term = (*screen->system_listener->get_pty)( screen->system_listener->self ,
				dev)) == NULL)
		{
			return  0 ;
		}
	}
	else
#endif
	{
		term = screen->term ;
	}

	if( strcmp( key , "encoding") == 0)
	{
		ml_char_encoding_t  encoding ;

		if( ( encoding = ml_get_char_encoding( value)) != ML_UNKNOWN_ENCODING)
		{
			ml_term_set_auto_encoding( term ,
				strcasecmp( value , "auto") == 0 ? 1 : 0) ;

			change_char_encoding( screen , encoding) ;
		}
	}
	else if( strcmp( key , "fg_color") == 0)
	{
		change_fg_color( screen , value) ;
	}
	else if( strcmp( key , "bg_color") == 0)
	{
		change_bg_color( screen , value) ;
	}
	else if( strcmp( key , "cursor_fg_color") == 0)
	{
		x_color_manager_set_cursor_fg_color( screen->color_man ,
			*value == '\0' ? NULL : value) ;
	}
	else if( strcmp( key , "cursor_bg_color") == 0)
	{
		x_color_manager_set_cursor_bg_color( screen->color_man ,
			*value == '\0' ? NULL : value) ;
	}
	else if( strcmp( key , "bd_color") == 0)
	{
		change_bd_color( screen , value) ;
	}
	else if( strcmp( key , "ul_color") == 0)
	{
		change_ul_color( screen , value) ;
	}
	else if( strcmp( key , "sb_fg_color") == 0)
	{
		change_sb_fg_color( screen , value) ;
	}
	else if( strcmp( key , "sb_bg_color") == 0)
	{
		change_sb_bg_color( screen , value) ;
	}
	else if( strcmp( key , "hide_underline") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			change_hide_underline_flag( screen , flag) ;
		}
	}
	else if( strcmp( key , "tabsize") == 0)
	{
		u_int  tab_size ;

		if( kik_str_to_uint( &tab_size , value))
		{
			ml_term_set_tab_size( screen->term , tab_size) ;
		}
	}
	else if( strcmp( key , "logsize") == 0)
	{
		u_int  log_size ;

		if( kik_str_to_uint( &log_size , value))
		{
			change_log_size( screen , log_size) ;
		}
	}
	else if( strcmp( key , "fontsize") == 0)
	{
		u_int  font_size ;

		if( strcmp( value , "larger") == 0)
		{
			larger_font_size( screen) ;
		}
		else if( strcmp( value , "smaller") == 0)
		{
			smaller_font_size( screen) ;
		}
		else
		{
			if( kik_str_to_uint( &font_size , value))
			{
				change_font_size( screen , font_size) ;
			}
		}
	}
	else if( strcmp( key , "line_space") == 0)
	{
		u_int  line_space ;

		if( kik_str_to_uint( &line_space , value))
		{
			change_line_space( screen , line_space) ;
		}
	}
	else if( strcmp( key , "letter_space") == 0)
	{
		u_int  letter_space ;

		if( kik_str_to_uint( &letter_space , value))
		{
			change_letter_space( screen , letter_space) ;
		}
	}
	else if( strcmp( key , "screen_width_ratio") == 0)
	{
		u_int  ratio ;

		if( kik_str_to_uint( &ratio , value))
		{
			change_screen_width_ratio( screen , ratio) ;
		}
	}
	else if( strcmp( key , "screen_height_ratio") == 0)
	{
		u_int  ratio ;

		if( kik_str_to_uint( &ratio , value))
		{
			change_screen_height_ratio( screen , ratio) ;
		}
	}
	else if( strcmp( key , "scrollbar_view_name") == 0)
	{
		change_sb_view( screen , value) ;
	}
	else if( strcmp( key , "mod_meta_key") == 0)
	{
		change_mod_meta_key( screen , value) ;
	}
	else if( strcmp( key , "mod_meta_mode") == 0)
	{
		change_mod_meta_mode( screen , x_get_mod_meta_mode_by_name( value)) ;
	}
	else if( strcmp( key , "bel_mode") == 0)
	{
		change_bel_mode( screen , x_get_bel_mode_by_name( value)) ;
	}
	else if( strcmp( key , "vertical_mode") == 0)
	{
		change_vertical_mode( screen , ml_get_vertical_mode( value)) ;
	}
	else if( strcmp( key , "scrollbar_mode") == 0)
	{
		change_sb_mode( screen , x_get_sb_mode_by_name( value)) ;
	}
	else if( strcmp( key , "static_backscroll_mode") == 0)
	{
		ml_bs_mode_t  mode ;
		
		if( strcmp( value , "true") == 0)
		{
			mode = BSM_STATIC ;
		}
		else if( strcmp( value , "false") == 0)
		{
			mode = BSM_DEFAULT ;
		}
		else
		{
			return  1 ;
		}
		
		ml_term_set_backscroll_mode( term , mode) ;
	}
	else if( strcmp( key , "exit_backscroll_by_pty") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			x_exit_backscroll_by_pty( flag) ;
		}
	}
	else if( strcmp( key , "use_combining") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			ml_term_set_use_char_combining( screen->term , flag) ;
		}
	}
	else if( strcmp( key , "use_dynamic_comb") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			change_dynamic_comb_flag( screen , flag) ;
		}
	}
	else if( strcmp( key , "receive_string_via_ucs") == 0 ||
		/* backward compatibility with 2.6.1 or before */
		strcmp( key , "copy_paste_via_ucs") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			change_receive_string_via_ucs_flag( screen , flag) ;
		}
	}
	else if( strcmp( key , "use_transbg") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			change_transparent_flag( screen , flag) ;
		}
	}
	else if( strcmp( key , "brightness") == 0)
	{
		u_int  brightness ;

		if( kik_str_to_uint( &brightness , value))
		{
			change_brightness( screen , brightness) ;
		}
	}
	else if( strcmp( key , "contrast") == 0)
	{
		u_int  contrast ;

		if( kik_str_to_uint( &contrast , value))
		{
			change_contrast( screen , contrast) ;
		}
	}
	else if( strcmp( key , "gamma") == 0)
	{
		u_int  gamma ;

		if( kik_str_to_uint( &gamma , value))
		{
			change_gamma( screen , gamma) ;
		}
	}
	else if( strcmp( key , "alpha") == 0)
	{
		u_int  alpha ;

		if( kik_str_to_uint( &alpha , value))
		{
			change_alpha( screen , alpha) ;
		}
	}
	else if( strcmp( key , "fade_ratio") == 0)
	{
		u_int  fade_ratio ;

		if( kik_str_to_uint( &fade_ratio , value))
		{
			change_fade_ratio( screen , fade_ratio) ;
		}
	}
	else if( strcmp( key , "type_engine") == 0)
	{
		change_font_present( screen , x_get_type_engine_by_name( value) ,
			x_get_font_present(screen->font_man)) ;
	}
	else if( strcmp( key , "use_anti_alias") == 0)
	{
		x_font_present_t  font_present ;

		font_present = x_get_font_present( screen->font_man) ;

		if( strcmp( value , "true") == 0)
		{
			font_present &= ~FONT_NOAA ;
			font_present |= FONT_AA ;
		}
		else if( strcmp( value , "false") == 0)
		{
			font_present |= FONT_NOAA ;
			font_present &= ~FONT_AA ;
		}
		else /* if( strcmp( value , "default") == 0) */
		{
			font_present &= ~FONT_AA ;
			font_present &= ~FONT_NOAA ;
		}

		change_font_present( screen , x_get_type_engine( screen->font_man) ,
					font_present) ;
	}
	else if( strcmp( key , "use_variable_column_width") == 0)
	{
		x_font_present_t  font_present ;

		font_present = x_get_font_present( screen->font_man) ;

		if( strcmp( value , "true") == 0)
		{
			font_present |= FONT_VAR_WIDTH ;
		}
		else if( strcmp( value , "false") == 0)
		{
			font_present &= ~FONT_VAR_WIDTH ;
		}
		else
		{
			return  1 ;
		}

		change_font_present( screen , x_get_type_engine( screen->font_man) ,
					font_present) ;
	}
	else if( strcmp( key , "use_multi_column_char") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			change_multi_col_char_flag( screen , flag) ;
		}
	}
	else if( strcmp( key , "use_bold_font") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			change_use_bold_font_flag( screen , flag) ;
		}
	}
	else if( strcmp( key , "col_size_of_width_a") == 0)
	{
		u_int  size ;

		if( kik_str_to_uint( &size , value))
		{
			ml_term_set_col_size_of_width_a( term , size) ;
		}
	}
	else if( strcmp( key , "use_bidi") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			change_bidi_flag( screen , flag , ml_term_get_bidi_mode( term)) ;
		}
	}
	else if( strcmp( key , "bidi_mode") == 0)
	{
		change_bidi_flag( screen , ml_term_is_using_bidi( term) ,
			ml_get_bidi_mode( value)) ;
	}
	else if( strcmp( key , "use_ind") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			change_ind_flag( screen , flag) ;
		}
	}
	else if( strcmp( key , "input_method") == 0)
	{
		change_im( screen , value) ;
	}
	else if( strcmp( key , "borderless") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			change_borderless_flag( screen , flag) ;
		}
	}
	else if( strcmp( key , "wall_picture") == 0)
	{
		change_wall_picture( screen , value) ;
	}
	else if( strcmp( key , "icon_path") == 0)
	{
		ml_term_set_icon_path( term , value) ;
		set_icon( screen) ;
	}
	else if( strcmp( key , "logging_vt_seq") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			ml_term_set_logging_vt_seq( term , flag) ;
		}
	}
	else if( strcmp( key , "button3_behavior") == 0)
	{
		x_set_button3_behavior( value) ;
	}
	else if( strcmp( key , "word_separators") == 0)
	{
		ml_set_word_separators( value) ;
	}
	else if( strcmp( key , "use_clipboard") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			x_set_use_clipboard_selection( flag) ;
		}
	}
	else if( strcmp( key , "auto_restart") == 0)
	{
		ml_set_auto_restart_cmd( strcmp( value , "false") == 0 ? NULL : value) ;
	}
	else if( strcmp( key , "logging_msg") == 0)
	{
		if( true_or_false( value) > 0)
		{
			kik_set_msg_log_file_name( "mlterm/msg.log") ;
		}
		else
		{
			kik_set_msg_log_file_name( NULL) ;
		}
	}
	else if( strcmp( key , "blink_cursor") == 0)
	{
		if( true_or_false( value) > 0)
		{
			screen->window.idling = idling ;
		}
		else
		{
			screen->window.idling = NULL ;
		}
	}
	else if( strcmp( key , "use_local_echo") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			ml_term_set_use_local_echo( screen->term , flag) ;
		}
	}
	else if( strcmp( key , "use_alt_buffer") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			ml_set_use_alt_buffer( flag) ;
		}
	}
	else if( strcmp( key , "use_ansi_colors") == 0)
	{
		int  flag ;

		if( ( flag = true_or_false( value)) != -1)
		{
			ml_set_use_ansi_colors( flag) ;
		}
	}
	else
	{
		return  0 ;
	}

	return  1 ;
}


int
x_screen_reset_view(
	x_screen_t *  screen
	)
{
	x_color_manager_reload( screen->color_man) ;
	
	ml_term_set_modified_all_lines_in_screen( screen->term) ;
	font_size_changed( screen) ;
	x_window_update( &screen->window, UPDATE_SCREEN | UPDATE_CURSOR) ;

	return  1 ;
}


x_picture_modifier_t *
x_screen_get_picture_modifier(
	x_screen_t *  screen
	)
{
	if( x_picture_modifier_is_normal( &screen->pic_mod))
	{
		return  NULL ;
	}
	else
	{
		return  &screen->pic_mod ;
	}
}
