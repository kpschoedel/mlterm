/*
 *	$Id$
 */

#include  "mkf_eucjp_conv.h"

#include  <kiklib/kik_mem.h>
#include  <kiklib/kik_debug.h>

#include  "mkf_iso2022_intern.h"
#include  "mkf_ja_jp_map.h"


/* --- static functions --- */

static void
remap_unsupported_charset(
	mkf_char_t *  ch ,
	mkf_charset_t  g1 ,
	mkf_charset_t  g3
	)
{
	mkf_char_t  c ;

	if( ch->cs == ISO10646_UCS4_1)
	{
		if( ! mkf_map_ucs4_to_ja_jp( &c , ch))
		{
			return ;
		}
		
		*ch = c ;
	}
	
	/*
	 * various private chars -> jis
	 */
	if( ch->cs == JISC6226_1978_NEC_EXT)
	{
		if( mkf_map_nec_ext_to_jisx0208_1983( &c , ch))
		{
			*ch = c ;
		}
		else if( mkf_map_nec_ext_to_jisx0212_1990( &c , ch))
		{
			*ch = c ;
		}
	}
	else if( ch->cs == JISC6226_1978_NECIBM_EXT)
	{
		if( mkf_map_necibm_ext_to_jisx0208_1983( &c , ch))
		{
			*ch = c ;
		}
		else if( mkf_map_necibm_ext_to_jisx0212_1990( &c , ch))
		{
			*ch = c ;
		}
	}
	else if( ch->cs == SJIS_IBM_EXT)
	{
		if( mkf_map_ibm_ext_to_jisx0208_1983( &c , ch))
		{
			*ch = c ;
		}
		else if( mkf_map_ibm_ext_to_jisx0212_1990( &c , ch))
		{
			*ch = c ;
		}
	}
	else if( ch->cs == JISX0208_1983_MAC_EXT)
	{
		if( mkf_map_mac_ext_to_jisx0208_1983( &c , ch))
		{
			*ch = c ;
		}
		else if( mkf_map_mac_ext_to_jisx0212_1990( &c , ch))
		{
			*ch = c ;
		}
	}
	/*
	 * conversion between JIS charsets.
	 */
	else if( ch->cs == JISC6226_1978)
	{
		/*
		 * we mkf_eucjp_parser don't support JISC6226_1978.
		 * If you want to use JISC6226_1978 , use iso2022(jp).
		 *
		 * XXX
		 * 22 characters are swapped between 1978 and 1983.
		 * so , we should reswap these here , but for the time being ,
		 * we do nothing.
		 */

		ch->cs = JISX0208_1983 ;
	}
	else if( g1 == JISX0208_1983 && ch->cs == JISX0213_2000_1)
	{
		if( mkf_map_jisx0213_2000_1_to_jisx0208_1983( &c , ch))
		{
			*ch = c ;
		}
	}
	else if( g1 == JISX0213_2000_1 && ch->cs == JISX0208_1983)
	{
		if( mkf_map_jisx0208_1983_to_jisx0213_2000_1( &c , ch))
		{
			*ch = c ;
		}
	}
	else if( g3 == JISX0212_1990 && ch->cs == JISX0213_2000_2)
	{
		if( mkf_map_jisx0213_2000_2_to_jisx0212_1990( &c , ch))
		{
			*ch = c ;
		}
	}
	else if( g3 == JISX0213_2000_2 && ch->cs == JISX0212_1990)
	{
		if( mkf_map_jisx0212_1990_to_jisx0213_2000_2( &c , ch))
		{
			*ch = c ;
		}
	}
}

static size_t
convert_to_eucjp_intern(
	mkf_conv_t *  conv ,
	u_char *  dst ,
	size_t  dst_size ,
	mkf_parser_t *  parser ,
	mkf_charset_t  g1 ,
	mkf_charset_t  g3
	)
{
	size_t  filled_size ;
	mkf_char_t  ch ;

	filled_size = 0 ;
	while( 1)
	{
		mkf_parser_mark( parser) ;

		if( ! (*parser->next_char)( parser , &ch))
		{
			if( parser->is_eos)
			{
			#ifdef  __DEBUG
				kik_debug_printf( KIK_DEBUG_TAG
					" parser reached the end of string.\n") ;
			#endif
			
				return  filled_size ;
			}
			else
			{
			#ifdef  DEBUG
				kik_warn_printf( KIK_DEBUG_TAG
					" parser->next_char() returns error , but the process is continuing...\n") ;
			#endif

				/*
				 * passing unrecognized byte...
				 */
				if( mkf_parser_increment( parser) == 0)
				{
					return  filled_size ;
				}

				continue ;
			}
		}

		remap_unsupported_charset( &ch , g1 , g3) ;
		
		
		if( ch.cs == US_ASCII || ch.cs == JISX0201_ROMAN)
		{
			if( filled_size >= dst_size)
			{
				mkf_parser_reset( parser) ;
			
				return  filled_size ;
			}

			*(dst ++) = *ch.ch ;

			filled_size ++ ;
		}
		else if( ch.cs == g1)
		{
			if( filled_size + 1 >= dst_size)
			{
				mkf_parser_reset( parser) ;
				
				return  filled_size ;
			}
			
			*(dst ++) = MAP_TO_GR( ch.ch[0]) ;
			*(dst ++) = MAP_TO_GR( ch.ch[1]) ;

			filled_size += 2 ;
		}
		else if( ch.cs == JISX0201_KATA)
		{
			if( filled_size + 1 >= dst_size)
			{
				mkf_parser_reset( parser) ;
			
				return  filled_size ;
			}

			*(dst ++) = SS2 ;
			*(dst ++) = SET_MSB(*ch.ch) ;

			filled_size += 2 ;
		}
		else if( ch.cs == g3)
		{
			if( filled_size + 2 >= dst_size)
			{
				mkf_parser_reset( parser) ;
			
				return  filled_size ;
			}
			
			*(dst ++) = SS3 ;
			*(dst ++) = ch.ch[0] ;
			*(dst ++) = ch.ch[1] ;

			filled_size += 3 ;
		}
		else
		{
		#ifdef  DEBUG
			kik_warn_printf( KIK_DEBUG_TAG
				" cs(%x) is not supported by eucjp. char(%x) is discarded.\n" ,
				ch.cs , mkf_char_to_int( &ch)) ;
		#endif

			if( filled_size >= dst_size)
			{
				mkf_parser_reset( parser) ;
			
				return  filled_size ;
			}
			
			*(dst ++) = ' ' ;
			filled_size ++ ;
		}
	}
}

static size_t
convert_to_eucjisx0213(
	mkf_conv_t *  conv ,
	u_char *  dst ,
	size_t  dst_size ,
	mkf_parser_t *  parser
	)
{
	return  convert_to_eucjp_intern( conv , dst , dst_size , parser ,
		JISX0213_2000_1 , JISX0213_2000_2) ;
}

static size_t
convert_to_eucjp(
	mkf_conv_t *  conv ,
	u_char *  dst ,
	size_t  dst_size ,
	mkf_parser_t *  parser
	)
{
	return  convert_to_eucjp_intern( conv , dst , dst_size , parser ,
		JISX0208_1983 , JISX0212_1990) ;
}

static void
conv_init(
	mkf_conv_t *  conv
	)
{
}

static void
conv_delete(
	mkf_conv_t *  conv
	)
{
	free( conv) ;
}


/* --- global functions --- */

mkf_conv_t *
mkf_eucjp_conv_new(void)
{
	mkf_conv_t *  conv ;

	if( ( conv = malloc( sizeof( mkf_conv_t))) == NULL)
	{
		return  NULL ;
	}

	conv->convert = convert_to_eucjp ;
	conv->init = conv_init ;
	conv->delete = conv_delete ;

	return  conv ;
}

mkf_conv_t *
mkf_eucjisx0213_conv_new(void)
{
	mkf_conv_t *  conv ;

	if( ( conv = malloc( sizeof( mkf_conv_t))) == NULL)
	{
		return  NULL ;
	}

	conv->convert = convert_to_eucjisx0213 ;
	conv->init = conv_init ;
	conv->delete = conv_delete ;

	return  conv ;
}
