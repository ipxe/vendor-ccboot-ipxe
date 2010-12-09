/**************************************************************************
gPXE -  Network Bootstrap Program

Literature dealing with the network protocols:
	ARP - RFC826
	RARP - RFC903
	UDP - RFC768
	BOOTP - RFC951, RFC2132 (vendor extensions)
	DHCP - RFC2131, RFC2132 (options)
	TFTP - RFC1350, RFC2347 (options), RFC2348 (blocksize), RFC2349 (tsize)
	RPC - RFC1831, RFC1832 (XDR), RFC1833 (rpcbind/portmapper)

**************************************************************************/

FILE_LICENCE ( GPL2_OR_LATER );

#include <stdio.h>
#include <gpxe/init.h>
#include <gpxe/features.h>
#include <gpxe/shell.h>
#include <gpxe/shell_banner.h>
#include <gpxe/image.h>
#include <usr/autoboot.h>
#include <config/general.h>
#include <curses.h>
#include <console.h>
#include <unistd.h>

#define NORMAL	"\033[0m"
#define BOLD	"\033[1m"
#define CYAN	"\033[36m"


/**
 * Main entry point
 *
 * @ret rc		Return status code
 */
__asmcall int main ( void ) {
#if 0
	struct feature *feature;
	struct image *image;

	/* Some devices take an unreasonably long time to initialise */
	printf ( PRODUCT_SHORT_NAME " initialising devices...\n" );
#endif

	initialise();
	startup();

	printf( CYAN BOLD);
	printf( "CCBoot");
	printf( NORMAL);
	printf( " 2010/10/06 http://www.ccboot.com\n");

	autoboot();

#if 0

	/*
	 * Print welcome banner
	 *
	 *
	 * If you wish to brand this build of gPXE, please do so by
	 * defining the string PRODUCT_NAME in config/general.h.
	 *
	 * While nothing in the GPL prevents you from removing all
	 * references to gPXE or http://etherboot.org, we prefer you
	 * not to do so.
	 *
	 */
	printf ( NORMAL "\n\n" PRODUCT_NAME "\n" BOLD "gPXE " VERSION
		 NORMAL " -- Open Source Boot Firmware -- "
		 CYAN "http://etherboot.org" NORMAL "\n"
		 "Features:" );
	for_each_table_entry ( feature, FEATURES )
		printf ( " %s", feature->name );
	printf ( "\n" );

	/* Prompt for shell */
	if ( shell_banner() ) {
		/* User wants shell; just give them a shell */
		shell();
	} else {
		/* User doesn't want shell; load and execute the first
		 * image, or autoboot() if we have no images.  If
		 * booting fails for any reason, offer a second chance
		 * to enter the shell for diagnostics.
		 */
		if ( have_images() ) {
			for_each_image ( image ) {
				image_exec ( image );
				break;
			}
		} else {
			autoboot();
		}

		if ( shell_banner() )
			shell();
	}
#endif

	shutdown ( SHUTDOWN_EXIT | shutdown_exit_flags );
	return 0;
}
