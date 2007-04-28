/*
 *	$Id$
 */

#include  "ml_config_menu.h"

#include  <stdio.h>		/* sprintf */
#include  <string.h>		/* strchr */
#include  <unistd.h>		/* fork */
#include  <kiklib/kik_sig_child.h>
#include  <kiklib/kik_debug.h>
#include  <kiklib/kik_util.h>	/* DIGIT_STR_LEN */
#include  <kiklib/kik_mem.h>	/* malloc */
#include  <kiklib/kik_file.h>   /* kik_file_set_cloexec, kik_file_unset_cloexec */

#ifndef  LIBEXECDIR
#define  LIBEXECDIR  /usr/local/libexec
#endif


/* --- static functions --- */

static void
sig_child(
	void *  self ,
	pid_t  pid
	)
{
	ml_config_menu_t *  config_menu ;
	
	config_menu = self ;

	if( config_menu->pid == pid)
	{
		config_menu->pid = 0 ;
		close( config_menu->fd) ;
		config_menu->fd = -1 ;
	}
}


/* --- global functions --- */

int
ml_config_menu_init(
	ml_config_menu_t *  config_menu
	)
{
	config_menu->pid = 0 ;
	config_menu->fd = -1 ;

	kik_add_sig_child_listener( config_menu , sig_child) ;

	return  1 ;
}

int
ml_config_menu_final(
	ml_config_menu_t *  config_menu
	)
{
	kik_remove_sig_child_listener( config_menu , sig_child) ;

	return  1 ;
}

int
ml_config_menu_start(
	ml_config_menu_t *  config_menu ,
	char *  cmd_path ,
	int  x ,
	int  y ,
	char *  display ,
	int  pty_fd
	)
{
	pid_t  pid ;
	int  fds[2] ;

	if( config_menu->pid > 0)
	{
		/* configuration menu is active now */
		
		return  0 ;
	}

	if( !kik_file_unset_cloexec( pty_fd))
	{
		/* configulators require an inherited pty. */
		return  0 ;
	}

	if( pipe( fds) == -1)
	{
		return  0 ;
	}

	pid = fork() ;
	if( pid == -1)
	{
		return  0 ;
	}

	if( pid == 0)
	{
		/* child process */

		char *  args[6] ;
		char  geom[2 + DIGIT_STR_LEN(int)*2 + 1] ;

		/* specified program name without path. */
		if( strchr( cmd_path , '/') == NULL)
		{
			char *  p ;
			char  dir[] = LIBEXECDIR ;

			/* not freed, since this process soon execv() */
			if( ( p = malloc( strlen( cmd_path) + sizeof(dir) + 2)) == NULL)
			{
				exit(1) ;
			}
			
			sprintf( p , "%s/%s" , dir , cmd_path) ;
			cmd_path = p ;
		}

		args[0] = cmd_path ;
		
		args[1] = "--display" ;
		args[2] = display ;

		sprintf( geom , "+%d+%d" , x , y) ;
		
		args[3] = "--geometry" ;
		args[4] = geom ;
		
		args[5] = NULL ;

		close( fds[1]) ;

		/* for configulators,
		 * STDIN => to read replys from mlterm
		 * STDOUT => to write the "master" side of pty
		 * STDERR => is retained to be the mlterm's STDERR
		 */
		if( dup2( fds[0] , STDIN_FILENO) == -1
		 || dup2( pty_fd , STDOUT_FILENO) == -1
	       	 || execv( cmd_path , args) == -1)
		{
			/* failed */

			kik_msg_printf( "%s is not found.\n" , cmd_path) ;
			
			exit(1) ;
		}
	}

	/* parent process */

	close( fds[0]) ;

	config_menu->fd = fds[1] ;
	config_menu->pid = pid ;
	
	kik_file_set_cloexec( pty_fd) ;
	kik_file_set_cloexec( config_menu->fd) ;

	return  1 ;
}

int
ml_config_menu_write(
	ml_config_menu_t *  config_menu ,
	u_char *  buf ,
	size_t  len
	)
{
	ssize_t  write_len ;
	
	if( config_menu->fd == -1)
	{
		return  0 ;
	}

	if( ( write_len = write( config_menu->fd , buf , len)) == -1)
	{
		return  0 ;
	}
	else
	{
		return  write_len ;
	}
}
