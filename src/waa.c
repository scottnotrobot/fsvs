/************************************************************************
 * Copyright (C) 2005-2007 Philipp Marek.
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 ************************************************************************/

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <apr_pools.h>
#include <apr_md5.h>
#include <subversion-1/svn_md5.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>


#include "waa.h"
#include "interface.h"
#include "direnum.h"
#include "options.h"
#include "add_unvers.h"
#include "checksum.h"
#include "helper.h"
#include "global.h"
#include "est_ops.h"
#include "ignore.h"
#include "actions.h"


/** \file
 * Handling of multiple <tt>struct estat</tt>s, WAA (working copy 
 * administrative area) function.
 *
 * In other words, handling single directories or complete trees of entries
 * (whereas est_ops.c is concerned with operations on single entries).
 *
 * \note \e WAA is short for <b>W</b>orking copy <b>A</b>dministrative
 * <b>A</b>rea, ie. the directory hierarchy where local data concerning
 * the remote state and some caches are stored.
 *
 * This is not needed for all operations; eg. an \a export works without it.
 * */


/** The extension temporary files in the WAA get. */
static const char ext_tmp[]=".tmp";

/** The base path of the WAA. */
static char const *waa_path;
/** The length of \ref waa_path. */
static int waa_len;
/** The base path of the configuration area. */
static char const *conf_path;
/** The length of \ref conf_path. */
static int conf_len;

/** -.
 * They are long enough to hold \ref waa_path plus the 3-level deep
 * subdirectory structure for cache and data files.
 * The \ref conf_path plus additional data gets it own buffers.
 * @{ */
char *waa_tmp_path, *waa_tmp_fn,
						*conf_tmp_path, *conf_tmp_fn;
/** @} */
/** The meta-data for the WAA base directory.
 * The WAA itself doesn't get committed; checked via this inode. */
static struct sstat_t waa_stat;

/** The maximum path length encountered so far. Stored in the \a dir-file,
 * to enable construction of paths without reallocating. */
static unsigned max_path_len;

/** -.
 * This gets sorted by priority and URL on reading in \a url__load_list() . */
struct url_t **urllist=NULL;
/** -. */
int urllist_count=0;
/** How many entries we have; this is used to show the user
 * some kind of progress report, in percent. */
unsigned approx_entry_count;


struct waa___temp_names_t {
  char *temp_name;
	char *dest_name;
};
/** This array stores the target names.
 * Writes to the waa-area use temporary files, 
 * which get renamed on waa__close(). */
static struct waa___temp_names_t *target_name_array=NULL;
/** How many entries have been in use in the \ref target_name_array. */
static int target_name_array_len=0;

/** Length of paths of temporary files. */
static int waa_tmp_path_len;

/** -. */
struct waa__entry_blocks_t waa__entry_block;


/** -.
 * Valid after a successfull call to \ref waa__find_common_base(). */
char *wc_path;
/** -. */
int wc_path_len;


const char
/** The header line of the dir-files.
 *
 * Consists of
 * - header version (for verification), 
 * - header length (for verification), 
 * - number of entries (for space allocation), 
 * - subdirectory count (currently only informational), 
 * - needed string space (in bytes), 
 * - length of longest path in bytes.
 * */
waa__header_line[]="%u %lu %u %u %u %u";


/** Convenience function for creating two paths. */
inline void waa___init_path(char *dest, const char *const src, 
		int *len, char **eos)
{
	int l;


	l=0;
	if (strncmp(opt__get_string(OPT__SOFTROOT), 
				src, opt__get_int(OPT__SOFTROOT)) != 0 )
	{
		strcpy(dest, opt__get_string(OPT__SOFTROOT));
		l=opt__get_int(OPT__SOFTROOT);
		/* OPT__SOFTROOT is defined to have *no* PATH_SEPARATOR at the end.  
		 * */
	  dest[l++]=PATH_SEPARATOR;
	}

	l+= strlen( strcpy(dest+l, src) );

	/* ensure a delimiter */
	if (dest[l-1] != PATH_SEPARATOR)
	{
		dest[l++]=PATH_SEPARATOR;
		dest[l]='\0';
	}

	*eos=dest + l;
	*len=l;
}

/** -.
 * If not a WAA-less operation, find the WAA and define an ignore
 * pattern. */
int waa__init(void)
{
	int status;


	status=0;
	/* If we're doing an import/export operation, we must not use the waa
	 * area. We may be running off a KNOPPIX CD, or whatever.
	 *
	 * What we *need* is the conf directory ... it might have options for us.
	 *
	 * So waa_path is NULL, and serves as a validation point - every access 
	 * tried will get a SEGV and can be debugged. */
	conf_path=getenv(CONF__PATH_ENV);
	if (!conf_path ) conf_path="/etc/fsvs";

	/* at least /w or some such */
	conf_len=strlen(conf_path);
	STOPIF_CODE_ERR( conf_len<3, EINVAL, 
			"environment variable " CONF__PATH_ENV " should be set to a directory");


	if (!action->is_import_export)
	{
		waa_path=getenv(WAA__PATH_ENV);
		if (!waa_path ) waa_path="/var/spool/fsvs";

		waa_len=strlen(waa_path);
		STOPIF_CODE_ERR( waa_len<3, EINVAL, 
				"environment variable " WAA__PATH_ENV " should be set to a directory");

		/* validate existence and save dev/inode for later checking */
		STOPIF_CODE_ERR( hlp__lstat(waa_path, &waa_stat) == -1, errno,
				"stat() of waa-path '%s' failed. "
				"Does your local storage area exist? ", waa_path);
		DEBUGP("got the WAA as inode %llu", (t_ull)waa_stat.ino);
	}
	else
		waa_len=0;


	/* This memory has lifetime of the process.
	 *   /path/to/waa / 01/02/03..0F/ extension .tmp
	 * The memory allocated is enough for the longest possible path. */
	waa_tmp_path_len=
		opt__get_int(OPT__SOFTROOT) + 1 +
		(waa_len > conf_len ? waa_len : conf_len) + 1 + 
		APR_MD5_DIGESTSIZE*2 + 3 + 
		WAA__MAX_EXT_LENGTH + strlen(ext_tmp) + 1 +4;
	DEBUGP("using %d bytes for temporary WAA+conf paths", waa_tmp_path_len);

	conf_tmp_path=malloc(waa_tmp_path_len);
	STOPIF_ENOMEM(!conf_tmp_path);
	waa___init_path(conf_tmp_path, conf_path, &conf_len, &conf_tmp_fn);

	if (!action->is_import_export)
	{
		waa_tmp_path=malloc(waa_tmp_path_len);
		STOPIF_ENOMEM(!waa_tmp_path);

		waa___init_path(waa_tmp_path, waa_path, &waa_len, &waa_tmp_fn);
	}

ex:
	return status;
}


/** -.
 * This is more or less a portable reimplementation of GNU \c 
 * getcwd(NULL,0) ... self-allocating the needed buffer.
 *
 * \a where gets the cwd; the optional \a len and \a buffer_size can be set 
 * to the actual length of the cwd, and the number of bytes allocated (if 
 * something should be appended).
 * 
 * If the cwd has been removed, we get \c ENOENT. But returning that would 
 * not necessarily signal a fatal error to all callers, so we return \c 
 * ENOTDIR in that case. */ 
int waa__save_cwd(char **where, int *ret_len, int *buffer_size)
{
	int status;
	/* We remember how many bytes we used last time, hoping that we need no 
	 * realloc() call in later invocations. */
	static int len=256;
	char *path;


	path=NULL;
	status=0;
	while (1)
	{
		path=realloc(path, len);
		STOPIF_ENOMEM(!path);
		if (getcwd(path, len-1)) break;

		STOPIF_CODE_ERR(errno != ERANGE, errno == ENOENT ? ENOTDIR : errno,
				"Cannot get the current directory.");

		len += 512;
		STOPIF_CODE_ERR(len > 1<<13, ERANGE,
				"You have mighty long paths. Too long. More than %d bytes? Sorry.",
				len);
	}

	*where=path;
	if (ret_len) *ret_len=strlen(path);
	if (buffer_size) *buffer_size=len;

ex:
	return status;
}


/** Creates a directory \a dir.
 * If it already exists, no error is returned.
 *
 * If needed, the structure is generated recursively.
 *
 * \note The mask used is \c 0777 - so mind your umask! */
int waa__mkdir(char *dir)
{
	int status;
	char *last_ps;


	/* just trying is faster than checking ? 
	 * EEXIST does not differentiate type of existing node -
	 * so it may be a file. */
	if (mkdir(dir, 0777) == -1)
	{
		status=errno;
		switch(status)
		{
			case EEXIST:
				/* Is ok ... */
				status=0;
				break;
			case ENOENT:
				/* Some intermediate levels are still missing; try again 
				 * recursively. */
				last_ps=strrchr(dir, PATH_SEPARATOR);
				BUG_ON(!last_ps);

				/* Strip last directory, and *always* undo the change. */
				*last_ps=0;
				status=waa__mkdir(dir);
				*last_ps=PATH_SEPARATOR;
				STOPIF( status, NULL);

				/* Now the parent was done ... so we should not get ENOENT again. */
				STOPIF( waa__mkdir(dir), NULL);
				break;
			default:
				STOPIF(status, "cannot mkdir(%s)", dir);
		}
	}
	status=0;

ex:
	return status;
}


/** -.
 *
 * In \a erg a pointer to an static buffer (at least as far as the caller
 * should mind!) is returned; \a eos, if not \c NULL, is set to the end of 
 * the string. \a start_of_spec points at the first character specific to 
 * this file, ie. after the constant part of \c $FSVS_WAA or \c $FSVS_CONF 
 * and the \c PATH_SEPARATOR.
 *
 * \a flags tell whether the path is in the WAA (\ref GWD_WAA) or in the 
 * configuration area (\ref GWD_CONF); furthermore you can specify that 
 * directories should be created as needed with \ref GWD_MKDIR.
 *
 * The intermediate directories are created, so files can be created
 * or read directly after calling this function. */
int waa__get_waa_directory(char *path,
		char **erg, char **eos, char **start_of_spec,
		int flags)
{
	int status, len, plen, wdlen;
	char *cp;
	unsigned char digest[APR_MD5_DIGESTSIZE], *p2dig;

	status=0;
	cp=NULL;
	plen=strlen(path);
	DEBUGP("path is %s", path);

	/* If we have a relative path, ie. one without / as first character,
	 * we have to take the current directory first. */
	if (path[0] != PATH_SEPARATOR)
	{
		/* This may be suboptimal for performance, but the only usage
		 * currently is for MD5 of large files - and there it doesn't
		 * matter, because shortly afterwards we'll be reading many KB. */
		STOPIF( waa__save_cwd(&cp, &wdlen, &len), NULL);

		/* Check whether we have enough space. */
		plen=wdlen + 1 + plen + 1 + 3;
		if (len < plen)
		{
			cp=realloc(cp, plen);
			STOPIF_ENOMEM(!cp);
		}

		path= hlp__pathcopy(cp, NULL, cp, "/", path, NULL);
		/* hlp__pathcopy() can return shorter strings, eg. by removing ./././// 
		 * etc. So we have to count again. */
		plen=strlen(path);
	}

	while (plen>1 && path[plen-1] == PATH_SEPARATOR)
		plen--;

	if (opt__get_string(OPT__SOFTROOT))
	{
		DEBUGP("have softroot %s for %s, compare %d bytes", 
				opt__get_string(OPT__SOFTROOT), path, opt__get_int(OPT__SOFTROOT));
		if (strncmp(opt__get_string(OPT__SOFTROOT), 
					path, opt__get_int(OPT__SOFTROOT)) == 0 )
			path+=opt__get_int(OPT__SOFTROOT);

		/* We need to be sure that the path starts with a PATH_SEPARATOR.
		 * That is achieved in waa__init(); the softroot path gets normalized 
		 * there. */

		plen=strlen(path);
	}

	DEBUGP("md5 of %s", path);
	apr_md5(digest, path, plen);
	IF_FREE(cp);


	p2dig=digest;
	len=APR_MD5_DIGESTSIZE;

	if (flags & GWD_WAA)
	{
		*erg = waa_tmp_path;
		cp = waa_tmp_fn;
		if (start_of_spec) *start_of_spec=cp;


		Mbin2hex(p2dig, cp, 1);
		len--;

		*(cp++) = PATH_SEPARATOR;

		Mbin2hex(p2dig, cp, 1);
		len--;

		*(cp++) = PATH_SEPARATOR;
	}
	else if (flags & GWD_CONF)
	{
		*erg = conf_tmp_path;
		cp = conf_tmp_fn;
		if (start_of_spec) *start_of_spec=cp;
	}
	else
	{
		BUG(".:8:.");
	}

	Mbin2hex(p2dig, cp, len);
	if (flags & GWD_MKDIR)
		STOPIF( waa__mkdir(*erg), NULL);

	*(cp++) = PATH_SEPARATOR;
	*cp = '\0';

	if (eos) *eos=cp;

	DEBUGP("returning %s", *erg);

ex:
	return status;
}


/** Base function to open files in the WAA.
 *
 * For the \a flags the values of \c creat or \c open are used;
 * the mode is \c 0777, so take care of your umask. 
 *
 * If the flags include one or more of \c O_WRONLY, \c O_TRUNC or \c O_RDWR
 * the file is opened as a temporary file and \b must be closed with 
 * waa__close(); depending on the success value given there it is renamed
 * to the destination name or deleted. 
 * This temporary path is stored in a per-filehandle array, so there's no
 * limit here on the number of written-to files. 
 *
 * For read-only files simply do a \c close() on their filehandles.
 *
 * Does return \c ENOENT without telling the user.
 *
 * \note If \a extension is given as \c NULL, only the existence of the 
 * given WAA directory is checked. So the caller gets a \c 0 or an error 
 * code (like \c ENOENT); \a flags and \a filehandle are ignored.
 * */
int waa__open(char *path,
		const char *extension,
		int flags,
		int *filehandle)
{
	char *cp, *orig, *eos, *dest, *start_spec;
	int fh, status;
	int to_be_written_to;


	fh=-1;
	orig=NULL;

	to_be_written_to=flags & (O_WRONLY | O_RDWR | O_CREAT);
	STOPIF( waa__get_waa_directory(path, &dest, &eos, &start_spec,
				waa__get_gwd_flag(extension) ), NULL);

	if (!extension)
	{
		/* Remove the last PATH_SEPARATOR. */
		BUG_ON(eos == dest);
		eos[-1]=0;
		return hlp__lstat(dest, NULL);
	}

	strcpy(eos, extension);
	if (to_be_written_to)
	{
		orig=strdup(dest);
		STOPIF_ENOMEM(!orig);


		strcat(eos, ext_tmp);

		/* In order to avoid generating directories (eg. for md5s-files) that 
		 * aren't really used (because the data files are < 128k, and so the md5s 
		 * files get deleted again), we change the PATH_SEPARATOR in the 
		 * destination filename to '_' - this way we get different filenames 
		 * and avoid collisions with more than a single temporary file (as 
		 * would happen with just $FSVS_WAA/tmp).
		 *
		 * Can that filename get longer than allowed? POSIX has 255 characters, 
		 * IIRC - that should be sufficient. */
		cp=strchr(start_spec, PATH_SEPARATOR);
		while (cp)
		{
			*cp='_';
			cp=strchr(cp+1, PATH_SEPARATOR);
		}

		/* We want to know the name later, so keep a copy. */
		dest=strdup(dest);
		STOPIF_ENOMEM( !dest );
		DEBUGP("tmp for target %s is %s", orig, dest);
	}
	else
		DEBUGP("reading target %s", dest);


	/* in case there's a O_CREAT */
	fh=open(dest, flags, 0777);
	if (fh<0) 
	{
		status=errno;
		if (status == ENOENT) goto ex;
		STOPIF(status, "open %s with flags 0x%X",
				dest, flags);
	}

	DEBUGP("got fh %d", fh);

	/* For files written to remember the original filename, indexed by the 
	 * filehandle. That must be done *after* the open - we don't know the 
	 * filehandle in advance! */
	if (to_be_written_to)
	{
		if (fh >= target_name_array_len) 
		{
			/* store old length */
			status=target_name_array_len;

			/* Assume some more filehandles will be opened */
			target_name_array_len=fh+8;
			DEBUGP("reallocate target name array to %d", target_name_array_len);
			target_name_array=realloc(target_name_array, 
					sizeof(*target_name_array) * target_name_array_len);
			STOPIF_ENOMEM(!target_name_array);

			/* zero out */
			memset(target_name_array + status, 0, 
					sizeof(*target_name_array) * (target_name_array_len-status));
		}

		/* These are already copies. */
		target_name_array[fh].dest_name=orig;
		target_name_array[fh].temp_name=dest;
	}

	*filehandle=fh;
	status=0;

ex:
	if (status && fh>-1) close(fh);
	return status;
}


/** -.
 *
 * If \a has_failed is !=0, the writing to the file has
 * failed somewhere; so the temporary file is not renamed to the 
 * destination name, just removed.
 *
 * This may be called only for *writeable* files of waa__open and similar;
 * readonly files should just be close()d. */
int waa__close(int filehandle, int has_failed)
{
	int status, do_unlink;
	struct waa___temp_names_t *target;
	char *cp;


	/* Assume we have to remove the file; only if the rename
	 * is successful, this ain't true. */
	do_unlink=1;
	status=0;
	BUG_ON(!target_name_array);
	target=target_name_array+filehandle;
	BUG_ON(!target);

	DEBUGP("filehandle %d should be %s", filehandle, target->dest_name);

	status=close(filehandle);
	if (!has_failed) 
	{
		STOPIF_CODE_ERR(status == -1, errno, "closing tmp file");

		/* Now that we know we'd like to keep that file, make the directories 
		 * as needed.
		 * Sadly we have to duplicate some logic from waa__mkdir().
		 * Maybe that could be refactored a bit - with a function like 
		 * waa__make_parent_dirs()? */
		cp=strrchr(target->dest_name, PATH_SEPARATOR);
		BUG_ON(!cp);

		*cp=0;
		STOPIF( waa__mkdir(target->dest_name), NULL);
		*cp=PATH_SEPARATOR;

		/* And give it the correct name. */
		STOPIF_CODE_ERR( 
				rename(target->temp_name, target->dest_name) == -1, 
				errno, "renaming tmp file from %s to %s",
				target->temp_name, target->dest_name);
		do_unlink=0;
	}

	status=0;

ex:
	/* If there's an error while closing the file (or already given 
	 * due to has_failed), unlink the file. */
	if (do_unlink)
	{
		do_unlink=0;
		STOPIF_CODE_ERR( unlink(target->temp_name) == -1, errno,
				"Cannot remove temporary file %s", target->temp_name);
	}

	IF_FREE(target->temp_name);
	IF_FREE(target->dest_name);

	return status;
}


/** -.
 *
 * Normally this is used to mark the base directory used in some WAA path,
 * ie. if you are versioning \c /etc, you'll get a symlink
 * \c $WAA/18/2f/153bd94803955c2043e6f2581d5d/_base
 * pointing to \c /etc . */
int waa__make_info_link(char *directory, char *name, char *dest)
{
	int status;
	char *path, *eos;


	STOPIF( waa__get_waa_directory(directory, &path, &eos, NULL,
				GWD_CONF | GWD_MKDIR), NULL);

	strcpy(eos, name);
	/* If the link does not exist, try to make it. */
	if (access(path, F_OK) != 0)
		STOPIF_CODE_ERR( symlink(dest, path) == -1,
				errno, "cannot create informational symlink '%s' -> '%s'",
				path, dest);

ex:
	return status;
}


/** -.
 *
 * This function takes the parameter \a directory, and returns a 
 * freshly allocated bit of memory with the given value or - if \c NULL -
 * the current working directory.
 *
 * That the string is always freshly allocated on the heap makes
 * sense in that callers can \b always just free it. */
int waa__given_or_current_wd(char *directory, char **erg)
{
	int status;


	status=0;
	if (directory)
	{
		*erg=strdup(directory);
		STOPIF_ENOMEM(!*erg);
	}
	else
	{
		STOPIF( waa__save_cwd( erg, NULL, NULL), NULL);

	}

ex:
	return status;
}


/** -.
 *
 * If the \c unlink()-call succeeds, the directory levels above are removed,
 * if possible.
 *
 * Via the parameter \a ignore_not_exist the caller can say whether a
 * \c ENOENT should be returned silently.
 * 
 * \see waa_files. */
int waa__delete_byext(char *path, 
		char *extension,
		int ignore_not_exist)
{
	int status;
	char *cp, *eos;

	status=0;
	STOPIF( waa__get_waa_directory(path, &cp, &eos, NULL,
				waa__get_gwd_flag(extension)), NULL);
	strcpy(eos, extension);

	if (unlink(cp) == -1)
	{
		status=errno;
		if (status == ENOENT && ignore_not_exist) status=0;

		STOPIF(status, "Cannot remove spool entry %s", cp);
	}

	/* Try to unlink the (possibly) empty directory. 
	 * If we get an error don't try further, but don't give it to 
	 * the caller, either.
	 * After all, it's just a clean-up. */
	*eos=0;
	if (rmdir(cp) == 0)
	{
		eos=strrchr(cp, PATH_SEPARATOR);
		if (eos)
		{
			*eos=0;
			rmdir(cp);
			/* There is one level left; but the chance that it's unused is
			 * very low.
			 * On my machine I have ~2000 files >= 512kB; there's no first level
			 * directory with only one child. */
		}
	}

ex:
	return status;
}


/** -.
 *
 * The \a directory may be \c NULL; then the current working directory
 * is taken.
 * \a write is just a flag; if set, <tt>O_CREAT | O_WRONLY | O_TRUNC</tt>
 * is given to \c waa__open().
 * 
 * \c ENOENT is returned without giving an error message. */
int waa__open_byext(char *directory,
		char *extension,
		int write,
		int *fh)
{
	int status;
	char *dir;


	status=0;
	STOPIF( waa__given_or_current_wd(directory, &dir), NULL );

	status=waa__open(dir, extension, 
			write ? (O_CREAT | O_WRONLY | O_TRUNC) : O_RDONLY,
			fh);
	if (status == ENOENT) goto ex;
	STOPIF(status, NULL);

ex:
	if (dir) IF_FREE(dir);
	return status;
}


/** -.
 * */
int waa__open_dir(char *directory,
		int write,
		int *fh)
{
	return waa__open_byext(directory, WAA__DIR_EXT, write, fh);
}


/** -.
 *
 * All entries are defined as new. */
int waa__build_tree(struct estat *root)
{
	int status;
	struct estat *sts;
	int i, ignore, have_ignored, have_found;

	status=0;
	/* no stat info on first iteration */
	STOPIF( waa__dir_enum( root, 0, 0), NULL);


	DEBUGP("found %d entries ...", root->entry_count);
	have_ignored=0;
	have_found=0;
	for(i=0; i<root->entry_count; i++)
	{
		sts=root->by_inode[i];

		STOPIF( ign__is_ignore(sts, &ignore), NULL);
		if (ignore>0)
		{
			DEBUGP("ignoring entry %s", sts->name);

			sts->entry_type=FT_IGNORE;
			have_ignored=1;
			continue;
		}

		sts->path_level = sts->parent->path_level+1;
		/* in build_tree, it must be a new entry. */
		sts->entry_status=FS_NEW;
		approx_entry_count++;
		have_found++;

		if (S_ISDIR(sts->st.mode))
		{
			sts->entry_type=FT_DIR;

			if (opt_recursive>0)
			{
				STOPIF_CODE_ERR( chdir(sts->name) == -1, errno,
						"chdir(%s)", sts->name);

				STOPIF( waa__build_tree(sts), NULL );

				/* this can fail if the parent directories have been removed. */
				STOPIF_CODE_ERR( chdir("..") == -1, errno,
						"parent has gone");
			}
		}
		else
		{
			sts->entry_type=ops___filetype(&(sts->st));
		}

		STOPIF( ac__dispatch(sts, NULL), NULL);
	}

	if (have_ignored)
		/* Delete per index faster */
		STOPIF( ops__free_marked(root), NULL);

	if (have_found)
		root->entry_status |= FS_CHANGED | FS_CHILD_CHANGED;

ex:
	return status;
}


/** Returns the index at which the element should be
 * (the index at which an equal or first bigger inode is). */
int waa___find_position(struct estat **new, 
		struct estat ***array, int count)
{
	int smaller, middle, bigger_eq;
	int status;


	/* That's easy. */
	if (count == 0) 
		return 0;

	/* A special case. As the directories are normally laid out
	 * sequentially on a hard disk, the inode number are often
	 * grouped in their directories.
	 * In a test case (my /etc) this shortcut was taken 1294 times, and
	 * didn't catch 1257 times (with up to 80 entries in the array). */
	if (dir___f_sort_by_inode(new, array[0]) < 0)
	{
		DEBUGP("short path taken for 0<1");
		return 0;
	}
	/* if only one element, and not on first position ... */
	if (count == 1) return 1;

	/* some more cheating :-) */
	if (dir___f_sort_by_inode(new, array[count-1]) >= 0)
	{
		DEBUGP("short path taken for >count");
		return count;
	}
	smaller=1; 

	/* bsearch can only find the _equal_ element - we need 
	 * the first one higher. */
	/* order is wrong - find new place for this element. */
	bigger_eq=count-1;
	/* i is a smaller element, k a possibly higher */
#ifdef DEBUG
	if (1)
	{
		char tmp[count*(18+1)+10];
		int i, n;
		for (i=n=0; i<count; i++)
		{
			n += sprintf(tmp+n,"%llu ", (t_ull)(*array[i])->st.ino);
		}
		DEBUGP("having %d [ %s]", count, tmp);
		DEBUGP("looking for %llu", (t_ull)(*new)->st.ino);
	}
#endif

	while (1) //bigger_eq>smaller+1)
	{
		middle=(bigger_eq+smaller)/2;
		DEBUGP("at %d=%llu - %d=%llu - %d=%llu", 
				smaller, (t_ull)(*array[smaller])->st.ino,
				middle, (t_ull)(*array[middle])->st.ino,
				bigger_eq, (t_ull)(*array[bigger_eq])->st.ino);

		status=dir___f_sort_by_inode(new, array[middle]);
		if (status > 0)
			smaller=middle+1;
		else if (status	< 0)
			bigger_eq=middle;
		else 
			/* status == 0 means identical inodes => hardlinks. */
		{
			bigger_eq=middle;
			break;
		}
		if (bigger_eq<=smaller) break;
	}

	DEBUGP("believing in %d %llu",
			bigger_eq, (t_ull)(*array[bigger_eq])->st.ino);
	/* now we have an index bigger_eq, whose element is bigger or equal 
	 * than the new, and its left is smaller or equal: */
#if DEBUG
	BUG_ON((bigger_eq < count-1 && dir___f_sort_by_inode(new, array[bigger_eq])>0) ||
			(bigger_eq >0 && dir___f_sort_by_inode(new, array[bigger_eq-1])<0));
#endif

	return bigger_eq;
}


/** -.
 *
 * Here the complete entry tree gets written to a file, which is used on the
 * next invocations to determine the entries' statii. It contains the names,
 * sizes, MD5s, devices, inode numbers, parent, mode and time informations,
 * and a reference to the parent to re-build the tree.
 *
 * \todo Currently hardlinks with duplicate inode-numbers are not well done
 * in fsvs.
 *
 *
 * <h3>Format</h3>
 * This file has a single header line with a defined length; it is padded
 * before the newline with spaces, and the last character before the newline
 * is a \c $ .
 * The other lines have space-delimited fields, and a \\0 delimited name 
 * at the end, followed by a newline.
 *
 * <h3>Order of entries in the file</h3>
 * We always write parents before childs, and (mostly) lower inode numbers 
 * before higher; mixing the subdirectories is allowed.
 * This allows us to rebuild the tree in one pass (because the parents are
 * already known), and gives us nearly linear reading on the storage media
 * (because the inodes are mostly in harddisk order, there's not much 
 * back-seeking necessary).
 *
 * As a consequence the root entry \c . is \b always the first one in
 * the written file. 
 *
 * \note 
 * If we were going \b strictly in inode-order, we would have to jump over 
 * some entries (if the parent directory has a higher inode
 * number than this entry and the [hard disk] head is already further down),
 * and then have a second run through ... (or possibly a third, and so on).
 * That's more complexity than wanted, and doesn't bring performance.
 * So currently only one run; hard disk must move back sometimes. 
 *
 *
 * <h3>\c directory Array</h3>
 * We use one array, named \c directory , to store pointers in the 
 * \a estat::by_inode arrays we're traversing (which are defined to 
 * be NULL-terminated).
 *
 * We just have to find the next inode in the active directories; they are
 * already sorted by inode, so that's very easy. 
 *
 * Here's a small drawing in ASCII, followed by a graphviz version.
 *
 * \verbatim
 *      (struct estat)
 *        ^ 
 *        |
 *    xxxxxxxxxxxxxxxxN    xxxxxxxxxxxxxxxN    xxN       xxxxN
 *          ^                          ^       ^           ^
 *   /->d >-/                          |       |           |
 *   |  d >----------------------------/       |           |
 *   |  d >------------------------------------/           |
 *   |  d >------------------------------------------------/
 *   |  
 *   directory
 * \endverbatim
 * \dot
 * digraph directory 
 * {
 *   node [shape=record, fontsize=9, height=0, width=0];
 *   rankdir=BT;
 *
 *   directory [label=" directory | { <1> 1 | <2> 2 | <3> 3 | <4> 4 }"];
 *   List1 [ label="<1>1|<2>2|<3>3|<4>4|NULL" ];
 *   List2 [ label="<1>1|<2>2|<3>3|<4>4|<5>5|NULL" ];
 *   List3 [ label="<1>1|<2>2|<3>3|<4>4|NULL" ];
 *   List4 [ label="<1>1|<2>2|<3>3|NULL" ];
 *   sts [label="(struct estat)"];
 *
 *
 *   directory:4:e -> List1:2:s;
 *   directory:3:e -> List2:3:s;
 *   directory:2:e -> List3:4:s;
 *   directory:1:e -> List4:1:s;
 *
 *   List1:1 -> sts;
 *
 *   node [style=invis];
 *   edge [style=invis];
 *
 *   directory:1:e -> Hidden1 -> List4:n;
 *   directory:2:e -> Hidden1 -> List3:n;
 * }
 * \enddot
 * The x's are the by_inode-arrays of pointers to struct, NULL-terminated.
 *      
 * The d's show the directories-array with 4 entries.
 * 
 * We don't really store the parent inode numbers in the file; that wouldn't
 * be enough, anyway - as soon as there are two or more filesystems, they
 * would collide.
 *
 * So instead of the inode number we store the number of the entry *in the
 * file*; so the root inode (which is always first) has parent_ino=0 (none),
 * its children get 1, and so on.
 * That means that as long as we allocate the memory block in a single
 * continuous block, we don't have to search any more; we can just reconstruct
 * the pointers to the parent. 
 * We keep the directory-array sorted; so we have to insert a new directory
 * at the correct position, but can otherwise output very fast. 
 * So for the array
 *   [10 20 30 40 50 60 70]
 * the element 10 is written; the next one, say 35, is inserted at the
 * correct position:
 *   [20 30 35 40 50 60 70]
 * Again the first (smallest) element is written, and so on.
 */ 
int waa__output_tree(struct estat *root)
{
	struct estat ***directory, *sts, **sts_pp;
	int max_dir, i, alloc_dir;
	unsigned this_len;
	int status, waa_info_hdl=-1;
	unsigned complete_count, string_space;
	char header[HEADER_LEN] = "UNFINISHED";


	directory=NULL;
	STOPIF( waa__open_dir(NULL, 1, &waa_info_hdl), NULL);

	/* allocate space for later use - entry count and similar. */
	status=strlen(header);
	memset(header + status, '\n', sizeof(header)-status);
	i=write(waa_info_hdl, header, sizeof(header));
	STOPIF_CODE_ERR( i != sizeof(header), errno,
			"header was not written");


	/* Take a page of pointers (on x86-32). Will be reallocated if
	 * necessary. */
	alloc_dir=1024;
	directory=calloc(alloc_dir+1, sizeof(*directory));
	STOPIF_ENOMEM(!directory);


	/* The root entry is visible above all URLs. */
	root->url=NULL;

	STOPIF( ops__save_1entry(root, 0, waa_info_hdl), NULL);
	root->file_index=complete_count=1;


	root->path_len=string_space=strlen(root->name);
	max_path_len=root->path_len;

	/* an if (root->entry_count) while (...) {...}
	 * would be possible, but then an indentation level would
	 * be wasted :-) ! */
	if (!root->entry_count) goto save_header;

	directory[0]=root->by_inode;
	max_dir=1;

	/* This check is duplicated in the loop.
	 * We could do that in ops__save_1entry(), but it doesn't belong here. */
	if (root->to_be_sorted)
	{
		DEBUGP("re-sorting root");
		STOPIF( dir__sortbyinode(root), NULL);
	}


	/* as long as there are directories to do... */
	while (max_dir)
	{
		// get current entry
		sts=( *directory[0] );


		/* find next element */
		directory[0]++;

		/* end of this directory ?*/
		if (*directory[0] == NULL)
		{
			/* remove this directory by shifting the list */
			max_dir--;
			DEBUGP("finished subdir");
			memmove(directory, directory+1, sizeof(*directory)*max_dir);
		}
		else if (max_dir>1)
		{
			/* check if it stays or gets moved.
			 * ignore element 0, as this is the new one. */
			i=waa___find_position(directory[0], directory+1, max_dir-1);
			if (i)
			{
				/* order is wrong - move elements.
				 * Mind that returned index is one element further in directory[]! 
				 * 
				 * [ 55 20 30 40 50 60 ] max_dir=6, i=4
				 * new^ ^0          ^4
				 *
				 * [ 20 30 40 50 55 60 ]
				 * */
				sts_pp=directory[0];
				memmove(directory, directory+1, sizeof(*directory)*i);
				directory[i]=sts_pp;
				DEBUGP("old current moves to #%u: %llu < %llu",
						i, 
						(t_ull)(*directory[i-1])->st.ino, 
						(t_ull)(*directory[i  ])->st.ino);
			}
		}


		if (sts->entry_type == FT_IGNORE) continue;

		// do current entry
		STOPIF( ops__save_1entry(sts, sts->parent->file_index, waa_info_hdl), 
				NULL);

		complete_count++;
		/* store position number for child -> parent relationship */
		sts->file_index=complete_count;

		this_len=strlen(sts->name)+1;
		string_space += this_len;

		if (!sts->path_len)
			ops__calc_path_len(sts);
		if (sts->path_len > max_path_len)
			max_path_len = sts->path_len;


		if (S_ISDIR(sts->st.mode) && sts->entry_count>0)
		{
			/* It's easy and possible to have always the correct number
			 * of subdirectories in root->subdir_count. We'd just have
			 * to walk up to the root in build_tree and add_directory
			 * and increment the number there.
			 *
			 * But 
			 * - we don't really know if this size is really required and
			 * - we'd like to decrease the size of the structure,
			 * so we don't use that really any more - we realloc the pointers
			 * if necessary. */
			if (max_dir >= alloc_dir)
			{
				alloc_dir *= 2;
				DEBUGP("reallocated directory pointers to %u entries", alloc_dir);
				directory=realloc(directory, (alloc_dir+1) * sizeof(*directory));
				STOPIF_ENOMEM(!directory);
			}

			/* Has this directory to be sorted, because it got new elements?
			 * Must be done *before* inserting into the array. */
			if (sts->to_be_sorted)
				STOPIF( dir__sortbyinode(sts), NULL);


			/* sort into array */
			i=waa___find_position(sts->by_inode, directory, max_dir);

			/* this time we have to shift all bigger elements one further:
			 *   new=45, max_dir=7,
			 *   [10 20 30 40 50 60 70]
			 *                i=4  
			 * results in
			 *   [10 20 30 40 45 50 60 70] */
			memmove(directory+i+1, directory+i, 
					sizeof(*directory)*(max_dir-i));

			directory[i]=sts->by_inode;
			DEBUGP("new subdir %llu #%u", (t_ull)(*directory[i])->st.ino, i);
			max_dir++;
		}

#ifdef DEBUG
		for(i=1; i<max_dir; i++)
			BUG_ON(
					dir___f_sort_by_inode( directory[i-1], directory[i] ) >0);
#endif
	}


save_header:
	/* save header information */
	/* path_len needs a terminating \0, so add a few bytes. */
	status=snprintf(header, sizeof(header), waa__header_line,
			WAA_VERSION, (t_ul)sizeof(header),
			complete_count, alloc_dir, string_space+4,
			max_path_len+4);
	BUG_ON(status >= sizeof(header)-1, "header space not large enough");

	/* keep \n at end */
	memset(header + status, ' ', sizeof(header)-1 -status);
	header[sizeof(header)-2]='$';
	STOPIF_CODE_ERR( lseek(waa_info_hdl, 0, SEEK_SET) == -1, errno,
			"seeking to start of file");
	status=write(waa_info_hdl, header, sizeof(header));
	STOPIF_CODE_ERR( status != sizeof(header), errno,
			"re-writing header failed");

	status=0;

ex:
	if (waa_info_hdl != -1)
	{
		i=waa__close(waa_info_hdl, status);
		waa_info_hdl=-1;
		STOPIF( i, "closing tree handle");
	}

	if (directory) IF_FREE(directory);

	return status;
}


/** Checks for new entries in this directory, and updates the
 * directory information. */
int waa__update_dir(struct estat *old, char *path)
{
	int dir_hdl, status;
	struct estat current, *sts;
	int i_old, i_cur, nr_cur, ignore;
	int string_space;
	struct estat tmp;


	status=nr_cur=0;
	dir_hdl=-1;

	current=*old;
	current.by_inode=current.by_name=NULL;
	current.entry_count=0;

	if (!path)
		STOPIF( ops__build_path(&path, old), NULL);

	/* To avoid storing arbitrarily long pathnames, we just open this
	 * directory and do a fchdir() later. */
	dir_hdl=open(".", O_RDONLY | O_DIRECTORY);
	STOPIF_CODE_ERR( dir_hdl==-1, errno, "saving current directory with open(.)");

	DEBUGP("update_dir: chdir(%s)", path);
	STOPIF_CODE_ERR( chdir(path) == -1, errno, 
			"chdir(%s)", path);

	/* Here we need the entries sorted by name. */
	STOPIF( waa__dir_enum( &current, 0, 1), NULL);
	DEBUGP("update_dir: direnum found %d; old has %d (%d)", 
			current.entry_count, old->entry_count,
			status);
	/* No entries means no new entries; but not old entries deleted! */
	if (current.entry_count == 0) goto ex;

	/* Get a sorted list from the old entry, so we can compare */
	STOPIF( dir__sortbyname(old), NULL);


	/* Now go through the lists.
	 * Every element found in old will be dropped from current;
	 * only elements left behind in current are new. 
	 *
	 * But instead of moving memory around (the pointers), we put the 
	 * *unequal* elements in front of the list. 
	 *
	 * The elements we don't want (we already have them, or they are ignored) 
	 * are left at the end of the list - to be freed after the loop. 
	 *
	 * Before the loop:
	 *    A   b   c   D   e   F   g   h   NULL
	 * Now we need A, D, and F.
	 * 
	 * We see that we don't need b; so when the next element D is taken, it 
	 * is exchanged with the first to-be-discarded:
	 *    A   D   c   b   e   F   g   h   NULL
	 * c and F get swapped, too.
	 *
	 * So after the loop we have
	 *    A   D   F   b   e   c   g   h   NULL
	 *                ^
	 * with     nr_cur=3
	 * */
	i_cur=i_old=0;
	nr_cur=0;
	string_space=0;
	while (1)
	{
		status=(i_cur >= current.entry_count ? 2 : 0) |
			(i_old >= old->entry_count ? 1 : 0);
		DEBUGP("update_dir: loop %d %d = %d", i_cur, i_old, status);

		/* Current list (and maybe old, too) finished. 
		 * No further new entries. */
		if (status >= 2) break; 

		sts=current.by_name[i_cur];

		/* If both lists have elements left ... */
		if (!status)
		{
			status=dir___f_sort_by_name(
					old->by_name+i_old, &sts);
			DEBUGP("comparing %s, %s = %d",
					old->by_name[i_old]->name,
					sts->name,
					status);
		}


		if (status == 0)
		{
			/* Identical. Ignore. */
			i_cur++;
			i_old++;
		}
		else if (status > 0)
		{
			/* the "old" name is bigger, ie. this one does 
			 * not exist in old. => new entry. */
			STOPIF( ign__is_ignore(sts, &ignore), NULL);
			if (ignore>0)
				DEBUGP("ignoring entry %s", sts->name);
			else
			{
				sts->parent=old;

				/* Swap elements */
				if (nr_cur != i_cur)
				{
					current.by_name[i_cur]=current.by_name[nr_cur];
					current.by_name[nr_cur]=sts;
				}

				nr_cur++;
				DEBUGP("found a new one!");
				sts->entry_status=FS_NEW;
				STOPIF( ac__dispatch(sts, NULL), NULL);
				approx_entry_count++;

				STOPIF( ops__set_to_handle_bits(sts), NULL);

				/* if it's a directory, add all subentries, too. */
				/* Use the temporary variable to see whether child-entries are 
				 * interesting to us. */
				tmp.parent=sts;
				tmp.do_full_child=tmp.do_full=0;
				STOPIF( ops__set_to_handle_bits(&tmp), NULL);
				if (S_ISDIR(sts->st.mode) && tmp.do_full_child)
				{
					STOPIF_CODE_ERR( chdir(sts->name) == -1, errno,
							"chdir(%s)", sts->name);

					STOPIF( waa__build_tree(sts), NULL);

					STOPIF_CODE_ERR( chdir("..") == -1, errno,
							"parent went away");
				}

			}

			i_cur++;
		}
		else
		{
			/* Deleted entry. Simply ignore, will be found later or
			 * has already been found. */
			i_old++;
		}
	}

	DEBUGP("%d new entries", nr_cur);
	/* no new entries ?*/
	status=0;
	if (nr_cur)
	{
		STOPIF( ops__new_entries(old, nr_cur, current.by_name), 
				"adding %d new entries", nr_cur);
	}

	/* Free unused struct estats. */
	/* We use by_name - there the pointers are sorted by usage. */
	for(i_cur=nr_cur; i_cur < current.entry_count; i_cur++)
		STOPIF( ops__free_entry( current.by_name+i_cur ), NULL);

	/* Current is allocated on the stack, so we don't free it. */
	IF_FREE(current.by_inode);
	IF_FREE(current.by_name);
	/* The strings are still used. We would have to copy them to a new area, 
	 * like we're doing above in the by_name array. */
	//	IF_FREE(current.strings);


ex:
	/* There's no doubt now.
	 * The old entries have already been checked, and if there are new
	 * we're sure that this directory has changed. */
	old->entry_status &= ~FS_LIKELY;

	/* If we find a new entry, we know that this directory has changed. */
	if (nr_cur)
		old->entry_status |= FS_CHANGED | FS_CHILD_CHANGED;


	if (dir_hdl!=-1) 
	{
		i_cur=fchdir(dir_hdl);
		STOPIF_CODE_ERR(i_cur == -1 && !status, errno,
				"cannot fchdir() back");
		i_cur=close(dir_hdl);
		STOPIF_CODE_ERR(i_cur == -1 && !status, errno,
				"cannot close dirhandle");
	}
	DEBUGP("update_dir reports %d new found, status %d", nr_cur, status);
	return status;
}


/** Small helper macro for telling the user that the file is damaged. */
#define TREE_DAMAGED(condition, ...)                           \
	STOPIF_CODE_ERR( condition, EINVAL,                          \
			"!The entries file seems to be damaged -- \n"            \
			"  %s.\n"                                                \
			"\n"                                                     \
			"Please read the users@ mailing list.\n"                 \
			"  If you know what you're doing you could "             \
			"try using 'sync-repos'\n"                               \
			"  (but please _read_the_documentation_!)\n"             \
			"  'We apologize for the inconvenience.'",               \
			__VA_ARGS__);


/** -.
 * This may silently return -ENOENT, if the waa__open fails.
 *
 * The \a callback is called for \b every entry read; but for performance 
 * reasons the \c path parameter will be \c NULL.
 * */
int waa__input_tree(struct estat *root,
		struct waa__entry_blocks_t **blocks,
		action_t *callback)
{
	int status, waa_info_hdl=-1;
	int i, cur, first;
	unsigned count, subdirs, string_space;
	/* use a cache for directories, so that the parent can be located quickly */
	/* substitute both array with one struct estat **cache, 
	 * which runs along ->by_inode until NULL */
	ino_t parent;
	char header[HEADER_LEN];
	char *filename;
	struct estat *sts, *stat_mem;
	char *strings;
	int sts_free;
	char *dir_mmap, *dir_end, *dir_curr;
	off_t length;
	t_ul header_len;
	struct waa__entry_blocks_t *cur_block;
	struct estat *sts_tmp;


	waa__entry_block.first=root;
	waa__entry_block.count=1;
	waa__entry_block.next=waa__entry_block.prev=NULL;
	cur_block=&waa__entry_block;

	length=0;
	dir_mmap=NULL;
	status=waa__open_dir(NULL, 0, &waa_info_hdl);
	if (status == ENOENT) 
	{
		status=-ENOENT;
		goto ex;
	}
	STOPIF(status, "cannot open .dir file");

	length=lseek(waa_info_hdl, 0, SEEK_END);
	STOPIF_CODE_ERR( length == (off_t)-1, errno, 
			"Cannot get length of .dir file");

	DEBUGP("mmap()ping %llu bytes", (t_ull)length);
	dir_mmap=mmap(NULL, length,
			PROT_READ, MAP_SHARED, 
			waa_info_hdl, 0);
	/* If there's an error, return it.
	 * Always close the file. Check close() return code afterwards. */
	status=errno;
	i=close(waa_info_hdl);
	STOPIF_CODE_ERR( !dir_mmap, status, "mmap failed");
	STOPIF_CODE_ERR( i, errno, "close() failed");

	dir_end=dir_mmap+length;

	TREE_DAMAGED( length < (HEADER_LEN+5) || 
			dir_mmap[HEADER_LEN-1] != '\n' || 
			dir_mmap[HEADER_LEN-2] != '$',
			"the header is not correctly terminated");

	/* Cut $ and beyond. Has to be in another buffer, as the file's
	 * mmap()ed read-only. */
	memcpy(header, dir_mmap, HEADER_LEN-2);
	header[HEADER_LEN-2]=0;
	status=sscanf(header, waa__header_line,
			&i, &header_len,
			&count, &subdirs, &string_space,
			&max_path_len);
	DEBUGP("got %d header fields", status);
	TREE_DAMAGED( status != 6,
			"not all needed header fields could be parsed");
	dir_curr=dir_mmap+HEADER_LEN;

	TREE_DAMAGED( i != WAA_VERSION || header_len != HEADER_LEN, 
			"the header has a wrong version");

	/* For progress display */
	approx_entry_count=count;

	/* for new subdirectories allow for some more space.
	 * Note that this is not clean - you may have to have more space
	 * than that for large structures!
	 */
	max_path_len+=1024;

	DEBUGP("reading %d subdirs, %d entries, %d bytes string-space",
			subdirs, count, string_space);


	/* Isn't there a snscanf() or something similar? I remember having seen
	 * such a beast. There's always the chance of a damaged file, so 
	 * I wouldn't depend on sscanf staying in its buffer.
	 *
	 * I now check for a \0\n at the end, so that I can be sure 
	 * there'll be an end to sscanf. */
	TREE_DAMAGED( dir_mmap[length-2] != '\0' || dir_mmap[length-1] != '\n',
			"the file is not correctly terminated");

	DEBUGP("ok, found \\0 or \\0\\n at end");

	strings=malloc(string_space);
	STOPIF_ENOMEM(!strings);
	root->strings=strings;

	/* read inodes */
	cur=0;
	sts_free=1;
	first=1;
	/* As long as there should be entries ... */
	while ( count > 0)
	{
		DEBUGP("curr=%p, end=%p, count=%d",
				dir_curr, dir_end, count);
		TREE_DAMAGED( dir_curr>=dir_end, 
				"An entry line has a wrong number of entries");

		if (sts_free == 0)
		{
			/* In all situations I can think about this will simply
			 * result in a big calloc, as at this time no block will
			 * have been freed, and the freelist will be empty. */
			STOPIF( ops__allocate(count, &stat_mem, &sts_free), NULL );
			/* This block has to be updated later. */
			STOPIF( waa__insert_entry_block(stat_mem, sts_free), NULL);

			cur_block=waa__entry_block.next;
		}

		sts_free--;
		count--;

		sts=first ? root : stat_mem+cur;

		DEBUGP("about to parse %p = '%-.40s...'", dir_curr, dir_curr);
		STOPIF( ops__load_1entry(&dir_curr, sts, &filename, &parent), NULL);

		/* Should this just be a BUG_ON? To not waste space in the release 
		 * binary just for people messing with their dir-file?  */
		TREE_DAMAGED( (parent && first) ||
				(!parent && !first) ||
				(parent && parent-1>cur), 
				"the parent pointers are invalid");

		if (first) first=0;
		else cur++;

		/* First - set all fields of this entry */
		strcpy(strings, filename);
		sts->name=strings;
		strings += strlen(filename)+1;
		BUG_ON(strings - root->strings > string_space);

		if (parent)
		{
			if (parent == 1) sts->parent=root;
			else
			{
				i=parent-2;
				BUG_ON(i >= cur);
				sts->parent=stat_mem+i;
			}

			sts->path_level = sts->parent->path_level+1;

			sts->parent->by_inode[ sts->parent->child_index++ ] = sts;
			BUG_ON(sts->parent->child_index > sts->parent->entry_count,
					"too many children for parent");

			/* Check the revision */
			if (sts->repos_rev != sts->parent->repos_rev)
			{
				sts_tmp=sts->parent;
				while (sts_tmp && !sts_tmp->other_revs)
				{
					sts_tmp->other_revs = 1;
					sts_tmp=sts_tmp->parent;
				}
			}
		} /* if parent */

		/* if it's a directory, we need the child-pointers. */
		if (S_ISDIR(sts->st.mode))
		{
			/* if it had children, we need to read them first - so make an array. */
			if (sts->entry_count)
			{
				sts->by_inode=malloc(sizeof(*sts->by_inode) * (sts->entry_count+1));
				sts->by_inode[sts->entry_count]=NULL;
				sts->child_index=0;
			}
		}

		if (callback)
			STOPIF( callback(sts, NULL), NULL);
	} /* while (count)  read entries */


ex:
	/* Return the first block even if we had eg. ENOENT */
	if (blocks)
		*blocks=&waa__entry_block;

	if (dir_mmap)
	{
		i=munmap(dir_mmap, length);
		if (!status)
			STOPIF_CODE_ERR(i, errno, "munmap() failed");
	}

	return status;
}


/** Check whether the conditions for update and/or printing the directory
 * are fulfilled.
 *
 * A directory has to be printed
 * - when it is to be fully processed (and not only walked through 
 *   because of some children),
 * - and either
 *   - it has changed (new or deleted entries), or
 *   - it was freshly added.
 *
 * */
inline int waa___check_dir_for_update(struct estat *sts, char *fullpath)
{
	int status;


	status=0;

	if (!sts->do_full_child) goto ex;

	/* If we have only do_a_child set, we don't update the directory - 
	 * so the changes will be found on the next commit. */
	/* If this directory has changed, check for new files. */
	/* If this entry was replaced, it must not have been a directory 
	 * before, so ->entry_count is defined as 0 (see ops__load_1entry()).
	 * For replaced entries which are _now_ directories we'll always
	 * get here, and waa__update_dir() will give us the children. */
	if (opt_recursive >=0 &&
			(sts->entry_status || 
			 opt_checksum || 
			 (sts->flags & RF_ADD) ||
			 (sts->flags & RF_CHECK) ) )
	{
		if (only_check_status)
			DEBUGP("Only check & set status - no update_dir");
		else
		{
			DEBUGP("dir_to_print | CHECK for %s", sts->name);
			STOPIF( waa__update_dir(sts, fullpath), NULL);
			/* After that update_dir fullpath may not be valid anymore! */
			fullpath=NULL;
		}
	}

	/* Whether to do something with this directory or not shall not be 
	 * decided here. Just pass it on. */
	STOPIF( ac__dispatch(sts, fullpath), NULL);

ex:
	return status;
}


/** -.
 *
 * It's not as trivial to scan the inodes in ascending order as it was when 
 * this part of code was included in 
 * \c waa__input_tree(); but we get a list of <tt>(location, number)</tt> 
 * blocks to run through, so it's the same performance-wise.
 *
 * This function \b consumes the list of entry blocks, ie. it destroys
 * their data - \a first gets incremented, \a count decremented.
 *
 * <h3>Threading</h3>
 * We could use several threads, to get more that one \c lstat() to run at
 * once. I have done this and a patch available, but testing on linux/x86 on 
 * ext3 seems to readahead the inodes, so the wall time got no shorter.
 *
 * If somebody wants to test with threads, I'll post the patch.
 *
 * For threading there has to be some synchronization - an entry can be done
 * only if its parent has been finished. That makes sense insofar, as when
 * some directory got deleted we don't need to \c lstat() the children - they
 * must be gone, too.
 * */
int waa__update_tree(struct estat *root,
		struct waa__entry_blocks_t *cur_block)
{
	int status;
	struct estat *sts;
	char *fullpath;


	if (! (root->do_full || root->do_a_child) )
	{
		/* If neither is set, waa__partial_update() wasn't called, so
		 * we start from the root. */
		root->do_full=root->do_full_child=1;
		DEBUGP("Full tree update");
	}


	status=0;
	while (cur_block)
	{
		/* For convenience */
		sts=cur_block->first;
		DEBUGP("doing update for %s ... %d left in %p",
				sts->name, cur_block->count, cur_block);

		/* For directories initialize the child counter. */
		if (S_ISDIR(sts->st.mode))
			sts->child_index=0;

		if (sts->parent)
		{
			STOPIF( ops__set_to_handle_bits(sts), NULL);

			/* If the parent's status is removed (or replaced), that tells us
			 *   - the parent was a directory
			 *   - the parent is no longer a directory
			 * So there can be no children now. */
			if (sts->parent->entry_status & FS_REMOVED)
			{
				sts->entry_status=FS_REMOVED;
				goto next_noparent;
			}
		}

		if (!(sts->do_full_child || sts->do_a_child))
			goto next;


		STOPIF( ops__build_path(&fullpath, sts), NULL);
		if (sts->do_full_child)
			STOPIF( ops__update_single_entry(sts, fullpath), NULL);

		/* If this entry is removed, the parent has changed. */
		if ( (sts->entry_status & FS_REMOVED) && sts->parent)
			sts->parent->entry_status = FS_CHANGED | 
				( sts->parent->entry_status & (~FS_LIKELY) );

		/* If a directory is removed, we don't allocate the by_inode
		 * and by_name arrays, and it is set to no child-entries. */
		if (S_ISDIR(sts->st.mode) && 
				(sts->entry_status & FS_REMOVED) && 
				!action->keep_children)
			sts->entry_count=0;

		/* If this entry was exactly removed (not replaced), 
		 * skip the next steps. 
		 * The sub-entries will be found missing because the parent is removed. */
		if ((sts->entry_status & FS_REPLACED) == FS_REMOVED)
			goto next;

		if (S_ISDIR(sts->st.mode) && (sts->entry_status & FS_REPLACED))
		{
			/* This entry was replaced, ie. was another type before.
			 * So the shared members have wrong data - 
			 * eg. entry_count, by_inode. We have to correct that here.
			 * That leads to an update_dir, which is exactly what we want. */
			sts->entry_count=0;
			sts->by_inode=sts->by_name=NULL;
			sts->strings=NULL;
			/* TODO: fill this members from the ignore list */
			// sts->active_ign=sts->subdir_ign=NULL;
		}


		/* This is more or less the same as below, only for this entry and 
		 * not its parent. */
		/* If this is a directory which had no children ... */
		if (S_ISDIR(sts->st.mode) && sts->entry_count==0)
		{
			DEBUGP("doing empty directory %s", sts->name);
			/* Check this entry for added entries. There cannot be deleted 
			 * entries, as this directory had no entries before. */
			STOPIF( waa___check_dir_for_update(sts, fullpath), NULL);
			/* Mind: \a fullpath may not be valid anymore. */
		}


next:
		if (sts->parent)
		{
			sts->parent->child_index++;

			/* If we did the last child of a directory ... */
			if (sts->parent->child_index >= sts->parent->entry_count 
					&& sts->parent->do_full_child)
			{
				DEBUGP("checking parent %s/%s", sts->parent->name, sts->name);
				/* Check the parent for added entries. 
				 * Deleted entries have already been found missing while 
				 * running through the list. */
				STOPIF( waa___check_dir_for_update(sts->parent, NULL), NULL);
			}
			else
				DEBUGP("deferring parent %s/%s", sts->parent->name, sts->name);
		}

next_noparent:
		/* If this is a normal entry, we print it now.
		 * Directories are shown after all child nodes have been checked. */
		if ((sts->entry_status & FS_REMOVED) || 
				(sts->do_full_child && !S_ISDIR(sts->st.mode)))
			STOPIF( ac__dispatch(sts, NULL), NULL);

		/* How is a "continue" block from perl named in C??  TODO */
		cur_block->first++;
		cur_block->count--;
		if (cur_block->count <= 0)
		{
			/* We should possibly free this memory, but as there's normally only 1
			 * struct allocated (the other declared static) we'd save about 16 bytes. */
			cur_block=cur_block->next;
		}
	}


ex:
	return status;
}


/** -.
 *
 * \a argc and \a normalized tell which entries should be updated.
 *
 * We return the \c -ENOENT from waa__input_tree() if <b>no working 
 * copy</b> could be found. \c ENOENT is returned for a non-existing entry 
 * given on the command line.
 *
 * The \a callback is called for \b every entry read by waa__input_tree(), 
 * not filtered like the normal actions.
 */
int waa__read_or_build_tree(struct estat *root, 
		int argc, char *normalized[], char *orig[],
		action_t *callback,
		int return_ENOENT)
{
	int status;
	struct waa__entry_blocks_t *blocks;


	status=0;
	status=waa__input_tree(root, &blocks, callback);
	DEBUGP("read tree = %d", status);

	if (status == -ENOENT)
	{
		/* Some callers want to know whether we *really* know these entries. */
		if (return_ENOENT) 
			return ENOENT;
	}
	else 
		STOPIF( status, NULL);

	if (opt__get_int(OPT__PATH) == PATH_CACHEDENVIRON)
		STOPIF( hlp__match_path_envs(root), NULL);

	/* Do update. */
	STOPIF( waa__partial_update(root, argc, normalized, 
				orig, blocks), NULL);

	/* In case we're doing commit or something with progress report,
	 * uninit the progress. */
	if (action->local_uninit)
		STOPIF( action->local_uninit(), NULL);

ex:
	return status;
}


/** -.
 *
 * This function calculates the common root of the given paths, and tries
 * to find a working copy base there (or above).
 * It returns the paths of the parameters relative to the base found.
 *
 * Eg.: for \c /a/wc/sub/sub2 and \c /a/wc/file it returns
 * - \c base = \c /a/wc
 * - \c normalized[0] = \c sub/sub2
 * - \c normalized[1] = \c file
 *
 * We have to find a wc root before we can load the entries file; so we'd
 * have to process the given paths twice, possibly each time by prepending
 * the current working directory and so on; that's why this function returns
 * a block of relative path pointers. These have just to be walked up to the
 * root to process them (eg. mark for processing).
 *
 *
 * \c *normalized should be \c free()d after use; \c base is in the same
 * block, so needs no handling.
 *
 * The memory layout is .
 * \verbatim
 *   normalized[0] normalized[1] normalized...  path[0] path[1] path...  base  
 *       |              |                          ^       ^
 *       |              |                          |       |
 *       |              \--------------------------+-------/
 *       |                                         |       
 *       \-----------------------------------------/
 * \endverbatim
 * \dot
 * digraph {
 *   node [shape=record, fontsize=9, fontname="Courier New" ]
 *   x [label = " <n0> normalized[0] | <n1> normalized[1] | normalized ... | | | <p0> path[0] | <p1> path[1] | path ... | | | base "]
 *   x:n0:n -> x:p0:n;
 *   x:n1:s -> x:p1:s
 * }
 * \enddot
 *
 * \note In case \b no matching base is found, the common part of the paths 
 * is returned as base, and the paths are normalized relative to it. \c 
 * ENOENT is returned.
 * \todo Should that be changed to base="/"?
 * 
 *
 * If we get \b no parameters, we fake the current working directory as 
 * parameter and return it in \c normalized[0]. \c argc in the caller will 
 * still be \c 0!
 *
 * - If we have a dir-file, we look only from the current directory below - 
 *   so fake a parameter.
 * - If we have no dir-file:
 *   - If we find a base, we fake a parameter and show only below.
 *   - If we find no base, we believe that we're at the root of the wc.
 *
 * The parameter must not be shown as "added" ("n...") - because it isn't.
 * */
int waa__find_common_base(int argc, char *args[], char **normalized[])
{
	int status, i, j, longest_index;
	int len;
	char *cp, *confname;
	char *paths[argc], *space, *base_copy;
	char **norm;


	status=0;
	norm=NULL;
	/* Isn't used uninitialized, but to be sure and make gcc happy */
	space=NULL;


	/* Step 0: get cwd. Does someone have longer paths? */
	if (argc == 0)
	{
		argc=1;
		*args=start_path;
		DEBUGP("faked a single parameter to %s", *args);
	}


	/* Step 1: calculate needed space, and allocate it. 
	 * Delimiters are \0.
	 *
	 * Note: we need to copy only *relative* paths, absolute specifications
	 * can be directly referenced.
	 * Only the first path will be copied, to have a working area for 
	 * string manipulation. */

	/* We look for the longest path, and calculate the needed space 
	 * from it. That's a bit too much, but who cares :-)
	 * The number of parameters is typically small, and the buffer will
	 * be free()d before committing starts - then a whole lot more memory
	 * will be used.
	 *
	 * The correct way would be to know the base, and then look how 
	 * many bytes we'd actually need - but then there'd be more operations. */
	len=longest_index=0;
	for(i=0; i<argc; i++)
	{
		j=strlen(args[i]) + 1;
		if (args[i][0] != PATH_SEPARATOR )
			j+= start_path_len+1;
		if (j>len) len=j;
	}

	/* We need (argc || 1) pointers at the start, the base path, and (argc || 1)
	 * relative paths at the end. 
	 * We assume (yes, I know, "Silence of the lambs" :-) that all paths are of
	 * the full length.
	 *
	 * Actually we'll put a NULL pointer in, too. */
	len = argc * sizeof(char*) + sizeof(NULL) + len + len * argc;
	DEBUGP("need %d bytes for %d args", len, argc);
	norm=malloc(len);
	/* IF(!norm)STOPIF_ENOMEM would be visually more appealing :-² */
	STOPIF_ENOMEM(!norm);
	/* The filename space is after the pointers. */
	space=(char*)(norm + argc + 1);

	/* Step 2: convert all to full paths. 
	 * Need to check for some special cases while copying, to protect against
	 * cases like "/a/wc" compared against "/a//wc". */
	for(i=0; i<argc; i++)
	{
		paths[i]=space;
		hlp__pathcopy(space, NULL, args[i], NULL);

		/* Remove PATH_SEPARATOR at the end. */
		len=strlen(space);
		while (len>1 && space[len-1] == PATH_SEPARATOR) len--;
		space[len]=0;

		space+=strlen(space)+1;
		DEBUGP("path is %s", paths[i]);
	}
	/* Remember location *after* the last filename - there the base will
	 * be copied. */
	base_copy=space;


	/* Step 3: find the common base. */
	/* len always points to the *different* character (or to \0). */
	len=strlen(paths[0]);
	for(i=1; i<argc; i++)
	{
		DEBUGP("len before #%d is %d", i, len);
		for(j=0; j<len; j++)
			/* Shorten the common part ? */
			if (paths[i][j] != paths[0][j])
				len=j;
	}
	DEBUGP("len after is %d", len);

	/* Now [len] could be here:
	 *                   |             |
	 *    /fff/aaa/ggg/ggsfg          /a/b/c
	 *    /fff/aaa/ggg/ggtfg          /d/e/f
	 *
	 *                 |                     |
	 *    /fff/aaa/ggg/ggsfg          /a/b/c/d
	 *    /fff/aaa/ggg/agtfg
	 *    /fff/aaa/ggg/agt2g
	 *
	 *
	 * Or, in an invalid case:
	 *    |
	 *    hsh
	 *    zf
	 *
	 * We look for the first path separator before, and cut there.  */
	/* We must not always simply cut the last part off.
	 * 
	 * If the user gives *only* the WC as parameter (wc/), we'd not find the
	 * correct base!
	 * 
	 * But wc/1a, wc/1b, wc/1c must be cut to wc/.  So we look whether
	 * there's a PATH_SEPARATOR or a \0 after the current position.  */
	/* Note: Change for windows paths would be necessary, at least if the
	 * paths there are given as C:\X and D:\Y.
	 * 
	 * Using \\.\devices\Harddrive0\Partition1\... or similar we could avoid
	 * that.  */
	if (paths[0][len] == PATH_SEPARATOR ||
			paths[0][len] == 0)
	{
		DEBUGP("Is a directory, possible a wc root.");
	}
	else
	{
		DEBUGP("Reverting to next %c", PATH_SEPARATOR);
		/* Walk off the different character. */
		len--;
		/* And look for a PATH_SEPARATOR. */
		while (paths[0][len] != PATH_SEPARATOR && len >0)
			len--;
	}

	BUG_ON(len < 0, "Paths not even equal in separator - "
			"they have nothing in common!");

	/* paths[0][0] == PATH_SEPARATOR is satisfied by both branches above. */
	if (len == 0)
	{
		/* Special case - all paths are starting from the root. */
		len=1;
		DEBUGP("we're at root.");
	}

	strncpy(base_copy, paths[0], len);
	base_copy[len]=0;

	DEBUGP("starting search at %s", base_copy);


	/* Step 4: Look for a wc.
	 * The given value could possible describe a file (eg. if the only 
	 * argument is its path) - we have to find a directory. */
	while (1)
	{
		/* We cannot look for the entry file, because on the first commit it
		 * doesn't exist.
		 * A wc is defined by having an URL defined. */
		DEBUGP("looking for %s", base_copy);
		status=waa__open(base_copy, NULL, 0, 0);

		/* Is there a base? */
		if (!status) break;

		if (len <= 1) break;

		base_copy[len]=0;
		cp=rindex(base_copy, PATH_SEPARATOR);
		if (cp)
		{
			/* If we're at "/", don't delete the root - try with it, and stop. */
			if (cp == base_copy)
				cp[1]=0;
			else
				*cp=0;
		}
		len=cp - base_copy;
	}

	DEBUGP("after loop is len=%d, base=%s, and status=%d", 
			len, base_copy, status);

	/* Now status is either 0, or eg. ENOENT - just what we'd like to return.
	 * But do that silently. 
	 *
	 * Note: if there's *no* base found, we take the common path. */
	STOPIF( status, "!Couldn't find a working copy with matching base.");

	/* We hope (?) that the action won't overwrite these strings. */
	wc_path=base_copy;
	wc_path_len=len;

	DEBUGP("found working copy base at %s", wc_path);
	STOPIF_CODE_ERR( chdir(wc_path) == -1, errno, "chdir(%s)", wc_path);


	/* Step 5: Generate pointers to normalized paths.
	 * len is still valid, so we just have to use paths[i]+len. */
	for(i=0; i<argc; i++)
	{
		/* If the given parameter equals *exactly* the wc root, we'd jump off 
		 * with that +1. So return . for that case. */
		if (paths[i][len] == 0)
			norm[i] = ".";
		else
			if (len == 1)
				/* Special case for start_path=/. */
				norm[i]=paths[i]+1;
			else
				norm[i]=paths[i]+len+1;
		DEBUGP("we set norm[%d]=%s from %s", i,  norm[i], paths[i]);
	}
	norm[argc]=NULL;


	/* Step 6: Read wc-specific config file.
	 * It might be prettier to name the file "Conf" or such, so that we can 
	 * directly use the waa__open() function; but then it couldn't be copied 
	 * from other locations. (And the config read function would have to 
	 * accept a filehandle.) */
	STOPIF( waa__get_waa_directory( wc_path, &confname, &cp, NULL, GWD_CONF),
			NULL);
	STOPIF( opt__load_settings(confname, "config", PRIO_ETC_WC ), NULL);


ex:
	if (status && status!=ENOENT)
	{
		/* Free only if error encountered */
		IF_FREE(norm);
	}
	else 
	{
		/* No problems, return pointers. */
		*normalized=norm;
	}

	return status;
}


/** -.
 * */
int waa__partial_update(struct estat *root, 
		int argc, char *normalized[], char *orig[],
		struct waa__entry_blocks_t *blocks)
{
	int status;
	struct estat *sts;
	int i, flags;
	int faked_arg0;


	status=0;

	/* If the user gave no path argument to the action, the current directory 
	 * is faked into the first path, but without changing argc. (Some actions  
	 * want to know whether *any* path was given). */
	faked_arg0=(argc == 0 && *normalized);
	/* Not fully correct - we fake now, haven't faked ;-) */
	if (faked_arg0) argc=1;

	for(i=0; i<argc; i++)
	{
		DEBUGP("update %d=%s", i, normalized[i]);
		/* The given entry must either exist in the filesystem (then we'd 
		 * create it in the entry list, if necessary), or it must be already in 
		 * the list.
		 *
		 * So a removed entry must have been known, a new entry can be added.
		 * But a non-existing, unknown entry gives an error. */
		status=hlp__lstat(normalized[i], NULL); 

		if (status == ENOENT) 
			flags=OPS__ON_UPD_LIST | OPS__FAIL_NOT_LIST;
		else 
		{
			STOPIF( status, "Cannot query entry %s", normalized[i]);
			flags = OPS__ON_UPD_LIST | OPS__CREATE;
		}

		status=ops__traverse(root, normalized[i], flags, RF_ADD, &sts);
		if (status == ENOENT)
		{
			STOPIF_CODE_ERR( !(flags & OPS__CREATE), ENOENT,
					"!Entry '%s' is not known.", normalized[i]);
			BUG_ON(1);
		}
		else
			STOPIF(status, NULL);

		/* Remember which argument relates to this entry. */
		if (opt__get_int(OPT__PATH) == PATH_PARMRELATIVE && !sts->arg)
			sts->arg= faked_arg0 ? "" : orig[i];

		/* This new entry is surely updated.
		 * But what about its parents?
		 * They're not in the blocks list (that we get as parameter), so
		 * they'd get wrong information. */

		/* This is marked as full, parents as "look below". */
		sts->do_full=sts->do_full_child=1;
		while (sts)
		{
			sts->do_a_child = 1;
			sts->entry_status |= FS_CHILD_CHANGED;
			if (sts->flags & RF_ADD)
			{
				/* If this entry was created by the O_CREAT flag, get some data. */
				//				STOPIF( ops__update_single_entry(sts, NULL), NULL);
			}

			sts=sts->parent;
		}
	}

	STOPIF( waa__update_tree(root, blocks), NULL);

ex:
	return status;
}


/** -. */
int waa__new_entry_block(struct estat *entry, int count, 
		struct waa__entry_blocks_t *previous)
{
	int status;
	struct waa__entry_blocks_t *eblock;


	status=0;
	eblock=malloc(sizeof(*eblock));
	STOPIF_ENOMEM(!eblock);
	eblock->first=entry;
	eblock->count=count;

	/* The block is appended after the given block.
	 * - The root node is still the first entry.
	 * - We need not go to the end of the list, we have O(1). */
	eblock->next=previous->next;
	eblock->prev=previous;
	previous->next=eblock;
	if (eblock->next)
		eblock->next->prev=eblock;

ex:
	return status;
}


/** -.
 * */
int waa__find_base(struct estat *root, int *argc, char ***args)
{
	int status;
	char **normalized;

	status=0;
	/* Per default we use (shortened) per-wc paths, as there'll be no 
	 * arguments. */
	root->arg="";

	STOPIF( waa__find_common_base( *argc, *args, &normalized), NULL);
	if (*argc > 0 && strcmp(normalized[0], ".") == 0)
	{
		/* Use it for display, but otherwise ignore it. */
		root->arg = **args;

		(*args) ++;
		(*argc) --;
	}

	STOPIF_CODE_ERR( *argc, EINVAL,
			"!Only a working copy root is a valid path.");

	/* Return the normalized value */
	**args = normalized[0];

ex:
	return status;
}


/** -.
 * */
int waa__do_sorted_tree(struct estat *root, action_t handler)
{
	int status;
	struct estat **list, *sts;


	if ( !root->by_name)
		STOPIF( dir__sortbyname(root), NULL);

	/* Alternatively we could do a 
	 * for(i=0; i<root->entry_count; root++)
	 * */
	list=root->by_name;
	while ( (sts=*list) )
	{
		if (sts->do_full_child)
			STOPIF( handler(sts, NULL), NULL);

		if (sts->do_full && sts->entry_type==FT_DIR)
			STOPIF( waa__do_sorted_tree(sts, handler), NULL);
		list++;
	}

ex:
	IF_FREE(root->by_name);

	return status;
}


/** -.
 *
 * The cwd is the directory to be looked at.
 *
 * IIRC the inode numbers may change on NFS; but having the WAA on NFS 
 * isn't a good idea, anyway.
 * */
int waa__dir_enum(struct estat *this,
		int est_count,
		int by_name)
{
	int status;
	struct sstat_t cwd_stat;


	status=0;
	STOPIF( hlp__lstat(".", &cwd_stat), NULL);

	DEBUGP("checking: %llu to %llu",
			(t_ull)cwd_stat.ino,
			(t_ull)waa_stat.ino);
	/* Is the parent the WAA? */
	if (cwd_stat.dev == waa_stat.dev &&
			cwd_stat.ino == waa_stat.ino)
		goto ex;

	/* If not, get a list. */
	STOPIF( dir__enumerator(this, est_count, by_name), NULL);

ex:
	return status;
}
