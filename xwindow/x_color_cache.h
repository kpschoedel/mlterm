/*
 *	$Id$
 */

#ifndef  __X_COLOR_CACHE_H__
#define  __X_COLOR_CACHE_H__


#include  <ml_color.h>
#include  "x_color.h"


typedef struct x_color_cache_256ext
{
	x_color_t  xcolors[MAX_256EXT_COLORS] ;
	u_int8_t  is_loaded[MAX_256EXT_COLORS] ;

	u_int  ref_count ;

} x_color_cache_256ext_t ;

typedef struct x_color_cache
{
	x_display_t *  disp ;

	x_color_t  xcolors[MAX_VTSYS_COLORS] ;
	u_int8_t  is_loaded[MAX_VTSYS_COLORS] ;

	x_color_cache_256ext_t *  cache_256ext ;

	x_color_t  black ;

	u_int8_t  fade_ratio ;

	u_int16_t  ref_count ;		/* 0 - 65535 */

} x_color_cache_t ;


x_color_cache_t *  x_acquire_color_cache( x_display_t *  disp , u_int8_t  fade_ratio) ;

int  x_release_color_cache( x_color_cache_t *  color_cache) ;

int  x_color_cache_unload( x_color_cache_t *  color_cache) ;

int  x_color_cache_unload_all(void) ;

int  x_load_xcolor( x_color_cache_t *  color_cache , x_color_t *  xcolor, char *  name) ;

x_color_t *  x_get_cached_xcolor( x_color_cache_t *  color_cache , ml_color_t  color) ;


#endif
