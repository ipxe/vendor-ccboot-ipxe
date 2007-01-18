/*
 * Copyright (C) 2007 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/**
 * @file
 *
 * Linux bzImage image format
 *
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <realmode.h>
#include <bzimage.h>
#include <gpxe/uaccess.h>
#include <gpxe/image.h>
#include <gpxe/segment.h>
#include <gpxe/memmap.h>
#include <gpxe/shutdown.h>
#include <gpxe/initrd.h>

struct image_type bzimage_image_type __image_type ( PROBE_NORMAL );

/**
 * bzImage load context
 */
struct bzimage_load_context {
	/** Real-mode kernel portion load segment address */
	unsigned int rm_kernel_seg;
	/** Real-mode kernel portion load address */
	userptr_t rm_kernel;
	/** Real-mode kernel portion file size */
	size_t rm_filesz;
	/** Real-mode heap top (offset from rm_kernel) */
	size_t rm_heap;
	/** Command line (offset from rm_kernel) */
	size_t rm_cmdline;
	/** Real-mode kernel portion total memory size */
	size_t rm_memsz;
	/** Non-real-mode kernel portion load address */
	userptr_t pm_kernel;
	/** Non-real-mode kernel portion file and memory size */
	size_t pm_sz;
};

/**
 * bzImage execution context
 */
struct bzimage_exec_context {
	/** Real-mode kernel portion load segment address */
	unsigned int rm_kernel_seg;
	/** Real-mode kernel portion load address */
	userptr_t rm_kernel;
	/** Real-mode heap top (offset from rm_kernel) */
	size_t rm_heap;
	/** Command line (offset from rm_kernel) */
	size_t rm_cmdline;
	/** Video mode */
	unsigned int vid_mode;
	/** Memory limit */
	uint64_t mem_limit;
	/** Initrd address */
	physaddr_t ramdisk_image;
	/** Initrd size */
	physaddr_t ramdisk_size;
};

/**
 * Parse kernel command line for bootloader parameters
 *
 * @v image		bzImage file
 * @v exec_ctx		Execution context
 * @v cmdline		Kernel command line
 * @ret rc		Return status code
 */
static int bzimage_parse_cmdline ( struct image *image,
				   struct bzimage_exec_context *exec_ctx,
				   const char *cmdline ) {
	char *vga;
	char *mem;

	/* Look for "vga=" */
	if ( ( vga = strstr ( cmdline, "vga=" ) ) ) {
		vga += 4;
		if ( strcmp ( vga, "normal" ) == 0 ) {
			exec_ctx->vid_mode = BZI_VID_MODE_NORMAL;
		} else if ( strcmp ( vga, "ext" ) == 0 ) {
			exec_ctx->vid_mode = BZI_VID_MODE_EXT;
		} else if ( strcmp ( vga, "ask" ) == 0 ) {
			exec_ctx->vid_mode = BZI_VID_MODE_ASK;
		} else {
			exec_ctx->vid_mode = strtoul ( vga, &vga, 16 );
			if ( *vga && ( *vga != ' ' ) ) {
				DBGC ( image, "bzImage %p strange \"vga=\""
				       "terminator '%c'\n", image, *vga );
			}
		}
	}

	/* Look for "mem=" */
	if ( ( mem = strstr ( cmdline, "mem=" ) ) ) {
		mem += 4;
		exec_ctx->mem_limit = strtoul ( mem, &mem, 0 );
		switch ( *mem ) {
		case 'G':
		case 'g':
			exec_ctx->mem_limit <<= 10;
		case 'M':
		case 'm':
			exec_ctx->mem_limit <<= 10;
		case 'K':
		case 'k':
			exec_ctx->mem_limit <<= 10;
			break;
		case '\0':
		case ' ':
			break;
		default:
			DBGC ( image, "bzImage %p strange \"mem=\" "
			       "terminator '%c'\n", image, *mem );
			break;
		}
	}

	return 0;
}

/**
 * Set command line
 *
 * @v image		bzImage image
 * @v exec_ctx		Execution context
 * @v cmdline		Kernel command line
 * @ret rc		Return status code
 */
static int bzimage_set_cmdline ( struct image *image,
				 struct bzimage_exec_context *exec_ctx,
				 const char *cmdline ) {
	size_t cmdline_len;

	/* Copy command line down to real-mode portion */
	cmdline_len = ( strlen ( cmdline ) + 1 );
	if ( cmdline_len > BZI_CMDLINE_SIZE )
		cmdline_len = BZI_CMDLINE_SIZE;
	copy_to_user ( exec_ctx->rm_kernel, exec_ctx->rm_cmdline,
		       cmdline, cmdline_len );
	DBGC ( image, "bzImage %p command line \"%s\"\n", image, cmdline );

	return 0;
}

/**
 * Load initrd, if any
 *
 * @v image		bzImage image
 * @v exec_ctx		Execution context
 * @ret rc		Return status code
 */
static int bzimage_load_initrd ( struct image *image,
				 struct bzimage_exec_context *exec_ctx,
				 struct image *initrd ) {
	physaddr_t start = user_to_phys ( initrd->data, 0 );
	int rc;

	DBGC ( image, "bzImage %p loading initrd %p (%s)\n",
	       image, initrd, initrd->name );
	
	/* Find a suitable start address */
	if ( ( start + initrd->len ) <= exec_ctx->mem_limit ) {
		/* Just use initrd in situ */
		DBGC ( image, "bzImage %p using initrd as [%lx,%lx)\n",
		       image, start, ( start + initrd->len ) );
	} else {
		for ( ; ; start -= 0x100000 ) {
			/* Check that we're not going to overwrite the
			 * kernel itself.  This check isn't totally
			 * accurate, but errs on the side of caution.
			 */
			if ( start <= ( BZI_LOAD_HIGH_ADDR + image->len ) ) {
				DBGC ( image, "bzImage %p could not find a "
				       "location for initrd\n", image );
				return -ENOBUFS;
			}
			/* Check that we are within the kernel's range */
			if ( ( start + initrd->len ) > exec_ctx->mem_limit )
				continue;
			/* Prepare and verify segment */
			if ( ( rc = prep_segment ( phys_to_user ( start ),
						   initrd->len,
						   initrd->len ) ) != 0 )
				continue;
			/* Copy to segment */
			DBGC ( image, "bzImage %p relocating initrd to "
			       "[%lx,%lx)\n", image, start,
			       ( start + initrd->len ) );
			memcpy_user ( phys_to_user ( start ), 0,
				      initrd->data, 0, initrd->len );
			break;
		}
	}

	/* Record initrd location */
	exec_ctx->ramdisk_image = start;
	exec_ctx->ramdisk_size = initrd->len;

	return 0;
}

/**
 * Execute bzImage image
 *
 * @v image		bzImage image
 * @ret rc		Return status code
 */
static int bzimage_exec ( struct image *image ) {
	struct bzimage_exec_context exec_ctx;
	struct bzimage_header bzhdr;
	const char *cmdline = ( image->cmdline ? image->cmdline : "" );
	struct image *initrd;
	int rc;

	/* Initialise context */
	memset ( &exec_ctx, 0, sizeof ( exec_ctx ) );

	/* Retrieve kernel header */
	exec_ctx.rm_kernel_seg = image->priv.ul;
	exec_ctx.rm_kernel = real_to_user ( exec_ctx.rm_kernel_seg, 0 );
	copy_from_user ( &bzhdr, exec_ctx.rm_kernel, BZI_HDR_OFFSET,
			 sizeof ( bzhdr ) );
	exec_ctx.rm_cmdline = exec_ctx.rm_heap = 
		( bzhdr.heap_end_ptr + 0x200 );
	exec_ctx.vid_mode = bzhdr.vid_mode;
	if ( bzhdr.version >= 0x0203 ) {
		exec_ctx.mem_limit = ( bzhdr.initrd_addr_max + 1 );
	} else {
		exec_ctx.mem_limit = ( BZI_INITRD_MAX + 1 );
	}

	/* Parse command line for bootloader parameters */
	if ( ( rc = bzimage_parse_cmdline ( image, &exec_ctx, cmdline ) ) != 0)
		return rc;

	/* Store command line */
	if ( ( rc = bzimage_set_cmdline ( image, &exec_ctx, cmdline ) ) != 0 )
		return rc;

	/* Load an initrd, if one exists */
	for_each_image ( initrd ) {
		if ( initrd->type == &initrd_image_type ) {
			if ( ( rc = bzimage_load_initrd ( image, &exec_ctx,
							  initrd ) ) != 0 )
				return rc;
			break;
		}
	}

	/* Update and store kernel header */
	bzhdr.vid_mode = exec_ctx.vid_mode;
	bzhdr.ramdisk_image = exec_ctx.ramdisk_image;
	bzhdr.ramdisk_size = exec_ctx.ramdisk_size;
	copy_to_user ( exec_ctx.rm_kernel, BZI_HDR_OFFSET, &bzhdr,
		       sizeof ( bzhdr ) );

	/* Prepare for exiting */
	shutdown();

	/* Jump to the kernel */
	__asm__ __volatile__ ( REAL_CODE ( "movw %w0, %%ds\n\t"
					   "movw %w0, %%es\n\t"
					   "movw %w0, %%fs\n\t"
					   "movw %w0, %%gs\n\t"
					   "movw %w0, %%ss\n\t"
					   "movw %w1, %%sp\n\t"
					   "pushw %w2\n\t"
					   "pushw $0\n\t"
					   "lret\n\t" )
			       : : "r" ( exec_ctx.rm_kernel_seg ),
			           "r" ( exec_ctx.rm_heap ),
			           "r" ( exec_ctx.rm_kernel_seg + 0x20 ) );

	/* There is no way for the image to return, since we provide
	 * no return address.
	 */
	assert ( 0 );

	return -ECANCELED; /* -EIMPOSSIBLE */
}

/**
 * Load and parse bzImage header
 *
 * @v image		bzImage file
 * @v load_ctx		Load context
 * @v bzhdr		Buffer for bzImage header
 * @ret rc		Return status code
 */
static int bzimage_load_header ( struct image *image,
				 struct bzimage_load_context *load_ctx,
				 struct bzimage_header *bzhdr ) {

	/* Sanity check */
	if ( image->len < ( BZI_HDR_OFFSET + sizeof ( *bzhdr ) ) ) {
		DBGC ( image, "bzImage %p too short for kernel header\n",
		       image );
		return -ENOEXEC;
	}

	/* Read and verify header */
	copy_from_user ( bzhdr, image->data, BZI_HDR_OFFSET,
			 sizeof ( *bzhdr ) );
	if ( bzhdr->header != BZI_SIGNATURE ) {
		DBGC ( image, "bzImage %p bad signature\n", image );
		return -ENOEXEC;
	}

	/* We don't support ancient kernels */
	if ( bzhdr->version < 0x0200 ) {
		DBGC ( image, "bzImage %p version %04x not supported\n",
		       image, bzhdr->version );
		return -ENOTSUP;
	}

	/* Calculate load address and size of real-mode portion */
	load_ctx->rm_kernel_seg = 0x1000; /* place RM kernel at 1000:0000 */
	load_ctx->rm_kernel = real_to_user ( load_ctx->rm_kernel_seg, 0 );
	load_ctx->rm_filesz = load_ctx->rm_memsz =
		( ( bzhdr->setup_sects ? bzhdr->setup_sects : 4 ) + 1 ) << 9;
	if ( load_ctx->rm_filesz > image->len ) {
		DBGC ( image, "bzImage %p too short for %zd byte of setup\n",
		       image, load_ctx->rm_filesz );
		return -ENOEXEC;
	}

	/* Calculate load address and size of non-real-mode portion */
	load_ctx->pm_kernel = ( ( bzhdr->loadflags & BZI_LOAD_HIGH ) ?
				phys_to_user ( BZI_LOAD_HIGH_ADDR ) :
				phys_to_user ( BZI_LOAD_LOW_ADDR ) );
	load_ctx->pm_sz = ( image->len - load_ctx->rm_filesz );

	DBGC ( image, "bzImage %p version %04x RM %#zx bytes PM %#zx bytes\n",
	       image, bzhdr->version, load_ctx->rm_filesz, load_ctx->pm_sz );
	return 0;
}

/**
 * Load real-mode portion of bzImage
 *
 * @v image		bzImage file
 * @v load_ctx		Load context
 * @ret rc		Return status code
 */
static int bzimage_load_real ( struct image *image,
			       struct bzimage_load_context *load_ctx ) {
	int rc;

	/* Allow space for the stack and heap */
	load_ctx->rm_memsz += BZI_STACK_SIZE;
	load_ctx->rm_heap = load_ctx->rm_memsz;

	/* Allow space for the command line */
	load_ctx->rm_cmdline = load_ctx->rm_memsz;
	load_ctx->rm_memsz += BZI_CMDLINE_SIZE;

	/* Prepare, verify, and load the real-mode segment */
	if ( ( rc = prep_segment ( load_ctx->rm_kernel, load_ctx->rm_filesz,
				   load_ctx->rm_memsz ) ) != 0 ) {
		DBGC ( image, "bzImage %p could not prepare RM segment: %s\n",
		       image, strerror ( rc ) );
		return rc;
	}
	memcpy_user ( load_ctx->rm_kernel, 0, image->data, 0,
		      load_ctx->rm_filesz );

	return 0;
}

/**
 * Load non-real-mode portion of bzImage
 *
 * @v image		bzImage file
 * @v load_ctx		Load context
 * @ret rc		Return status code
 */
static int bzimage_load_non_real ( struct image *image,
				   struct bzimage_load_context *load_ctx ) {
	int rc;

	/* Prepare, verify and load the non-real-mode segment */
	if ( ( rc = prep_segment ( load_ctx->pm_kernel, load_ctx->pm_sz,
				   load_ctx->pm_sz ) ) != 0 ) {
		DBGC ( image, "bzImage %p could not prepare PM segment: %s\n",
		       image, strerror ( rc ) );
		return rc;
	}
	memcpy_user ( load_ctx->pm_kernel, 0, image->data, load_ctx->rm_filesz,
		      load_ctx->pm_sz );

	return 0;
}

/**
 * Update and store bzImage header
 *
 * @v image		bzImage file
 * @v load_ctx		Load context
 * @v bzhdr		Original bzImage header
 * @ret rc		Return status code
 */
static int bzimage_write_header ( struct image *image __unused,
				  struct bzimage_load_context *load_ctx,
				  struct bzimage_header *bzhdr ) {
	struct bzimage_cmdline cmdline;

	/* Update the header and copy it into the loaded kernel */
	bzhdr->type_of_loader = BZI_LOADER_TYPE_ETHERBOOT;
	if ( bzhdr->version >= 0x0201 ) {
		bzhdr->heap_end_ptr = ( load_ctx->rm_heap - 0x200 );
		bzhdr->loadflags |= BZI_CAN_USE_HEAP;
	}
	if ( bzhdr->version >= 0x0202 ) {
		bzhdr->cmd_line_ptr = user_to_phys ( load_ctx->rm_kernel,
						     load_ctx->rm_cmdline );
	} else {
		cmdline.magic = BZI_CMDLINE_MAGIC;
		cmdline.offset = load_ctx->rm_cmdline;
		copy_to_user ( load_ctx->rm_kernel, BZI_CMDLINE_OFFSET,
			       &cmdline, sizeof ( cmdline ) );
		bzhdr->setup_move_size = load_ctx->rm_memsz;
	}
	copy_to_user ( load_ctx->rm_kernel, BZI_HDR_OFFSET,
		       bzhdr, sizeof ( *bzhdr ) );

	return 0;
}

/**
 * Load bzImage image into memory
 *
 * @v image		bzImage file
 * @ret rc		Return status code
 */
int bzimage_load ( struct image *image ) {
	struct bzimage_load_context load_ctx;
	struct bzimage_header bzhdr;
	int rc;

	/* Initialise context */
	memset ( &load_ctx, 0, sizeof ( load_ctx ) );

	/* Load and verify header */
	if ( ( rc = bzimage_load_header ( image, &load_ctx, &bzhdr ) ) != 0 )
		return rc;

	/* This is a bzImage image, valid or otherwise */
	if ( ! image->type )
		image->type = &bzimage_image_type;

	/* Load real-mode portion */
	if ( ( rc = bzimage_load_real ( image, &load_ctx ) ) != 0 )
		return rc;

	/* Load non-real-mode portion */
	if ( ( rc = bzimage_load_non_real ( image, &load_ctx ) ) != 0 )
		return rc;

	/* Update and write out header */
	if ( ( rc = bzimage_write_header ( image, &load_ctx, &bzhdr ) ) != 0 )
		return rc;

	/* Record real-mode segment in image private data field */
	image->priv.ul = load_ctx.rm_kernel_seg;

	return 0;
}

/** Linux bzImage image type */
struct image_type bzimage_image_type __image_type ( PROBE_NORMAL ) = {
	.name = "bzImage",
	.load = bzimage_load,
	.exec = bzimage_exec,
};