/*
 *	$Id$
 */

#include  "ml_drcs.h"

#include  <string.h>		/* memset */
#include  <kiklib/kik_mem.h>
#include  <kiklib/kik_str.h>	/* strdup */
#include  <kiklib/kik_debug.h>


/* --- static variables --- */

static ml_drcs_t *  drcs_fonts ;
static u_int  num_of_drcs_fonts ;
static mkf_charset_t  cached_font_cs = UNKNOWN_CS ;


/* --- static functions --- */

static int
drcs_final(
	ml_drcs_t *  font
	)
{
	int  idx ;

	for( idx = 0 ; idx < 0x5f ; idx++)
	{
		free( font->glyphs[idx]) ;
	}

	memset( font->glyphs , 0 , sizeof(font->glyphs)) ;

	if( cached_font_cs == font->cs)
	{
		/* Clear cache in ml_drcs_get(). */
		cached_font_cs = UNKNOWN_CS ;
	}

	return  1 ;
}


/* --- global functions --- */

ml_drcs_t *
ml_drcs_get_font(
	mkf_charset_t  cs ,
	int  create
	)
{
	static ml_drcs_t *  cached_font ;
	u_int  count ;
	void *  p ;

	if( cs == cached_font_cs)
	{
		if( cached_font || ! create)
		{
			return  cached_font ;
		}
	}
	else
	{
		for( count = 0 ; count < num_of_drcs_fonts ; count++)
		{
			if( drcs_fonts[count].cs == cs)
			{
				return  &drcs_fonts[count] ;
			}
		}
	}

	if( ! create ||
	    /* XXX leaks */
	    ! ( p = realloc( drcs_fonts , sizeof(ml_drcs_t) * (num_of_drcs_fonts + 1))))
	{
		return  NULL ;
	}

	drcs_fonts = p ;
	memset( drcs_fonts + num_of_drcs_fonts , 0 , sizeof(ml_drcs_t)) ;
	cached_font_cs = drcs_fonts[num_of_drcs_fonts].cs = cs ;

	return  (cached_font = &drcs_fonts[num_of_drcs_fonts ++]) ;
}

char *
ml_drcs_get_glyph(
	mkf_charset_t  cs ,
	u_char  idx
	)
{
	ml_drcs_t *  font ;

	/* msb can be set in ml_vt100_parser.c (e.g. ESC(I (JISX0201 kana)) */
	if( ( font = ml_drcs_get_font( cs , 0)) && 0x20 <= (idx & 0x7f))
	{
		return  font->glyphs[(idx & 0x7f) - 0x20] ;
	}
	else
	{
		return  NULL ;
	}
}

int
ml_drcs_final(
	mkf_charset_t  cs
	)
{
	u_int  count ;

	for( count = 0 ; count < num_of_drcs_fonts ; count++)
	{
		if( drcs_fonts[count].cs == cs)
		{
			drcs_final( drcs_fonts + count) ;
			drcs_fonts[count] = drcs_fonts[--num_of_drcs_fonts] ;

			return  1 ;
		}
	}

	return  1 ;
}

int
ml_drcs_final_full(void)
{
	u_int  count ;

	for( count = 0 ; count < num_of_drcs_fonts ; count++)
	{
		drcs_final( drcs_fonts + count) ;
	}

	free( drcs_fonts) ;
	drcs_fonts = NULL ;
	num_of_drcs_fonts = 0 ;

	return  1 ;
}

int
ml_drcs_add(
	ml_drcs_t *  font ,
	int  idx ,
	char *  seq ,
	u_int  width ,
	u_int  height
	)
{
	free( font->glyphs[idx]) ;

	if( ( font->glyphs[idx] = malloc( 2 + strlen(seq) + 1)))
	{
		font->glyphs[idx][0] = width ;
		font->glyphs[idx][1] = height ;
		strcpy( font->glyphs[idx] + 2 , seq) ;
	}

	return  1 ;
}