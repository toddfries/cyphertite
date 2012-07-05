/*
 * Copyright (c) 2011, 2012 Conformal Systems LLC <info@conformal.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef NEED_LIBCLENS
#include <clens.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/tree.h>
#include <sys/queue.h>

#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <fts.h>
#include <pwd.h>
#include <limits.h>

#include <clog.h>
#include <exude.h>

#include <ct_ctfile.h>
#include <ct_match.h>
#include <cyphertite.h>
#include <ct_internal.h>

/*
 * flist is a structure that keeps track of the files that still need to be
 * accessed. Turned into a fnode by populate_fnode_from_flist() when it is
 * time to process the file fully.
 */
struct flist {
	TAILQ_ENTRY(flist)	fl_list;
	RB_ENTRY(flist)		fl_inode_entry;
	struct dnode		*fl_parent_dir;
	struct flist		*fl_hlnode;
	char			*fl_fname;
	struct fnode		*fl_node;
	dev_t			fl_dev;
	ino_t			fl_ino;
#define C_FF_FORCEDIR	0x1
#define C_FF_CLOSEDIR	0x2
#define C_FF_WASDIR	0x4
	int			fl_flags;
};
RB_HEAD(fl_tree, flist);
TAILQ_HEAD(flist_head, flist);

/* tree for hardlink calculations */
int		 	 fl_inode_sort(struct flist *, struct flist *);
RB_PROTOTYPE(fl_tree, flist, fl_inode_entry, fl_inode_sort);
RB_GENERATE(fl_tree, flist, fl_inode_entry, fl_inode_sort);

/* Directory traversal and transformation of generated data */
static void		 ct_traverse(struct ct_archive_state *, char **,
			     struct flist_head *, int, int, int, int,
			     struct ct_statistics *);
static void		 ct_sched_backup_file(struct ct_archive_state *,
			     struct stat *, char *, int, int, int,
			     struct flist_head *, struct fl_tree *,
			     struct ct_statistics *);
static struct fnode	*ct_populate_fnode_from_flist(struct ct_archive_state *,
			     struct flist *, int, int);
static char		*ct_name_to_safename(char *, int);

/* Helper functions for the above */
static char		*eat_double_dots(char *, char *);
static int		 backup_prefix(struct ct_archive_state *, char *,
			     struct flist_head *, struct fl_tree *, int,
			     struct ct_statistics *);
static char		*gen_fname(struct flist *);
static int		 s_to_e_type(int);


int                      ct_dname_cmp(struct dnode *, struct dnode *);

int			 ct_open(struct ct_archive_state *, struct fnode *,
			     int , int);
int			 ct_readlink(struct ct_archive_state *, struct fnode *,
			     char *, size_t);
int			 ct_stat(struct fnode *, struct stat *, int,
			     struct ct_extract_state *,
			     struct ct_archive_state *);
int			 ct_mkdir(struct ct_extract_state *, struct fnode *,
			     mode_t);
int			 ct_mknod(struct ct_extract_state *, struct fnode *);
int			 ct_link(struct ct_extract_state *, struct fnode *,
			     char *);
int			 ct_symlink(struct ct_extract_state *, struct fnode *);
int			 ct_rename(struct ct_extract_state *, struct fnode *);
int			 ct_chown(struct ct_extract_state *, struct fnode *,
			     int);
int			 ct_chmod(struct ct_extract_state *, struct fnode *,
			     mode_t);
int			 ct_utimes(struct ct_extract_state *, struct fnode *);
int			 ct_unlink(struct ct_extract_state *, struct fnode *);

RB_HEAD(d_name_tree, dnode);
RB_PROTOTYPE(d_name_tree, dnode, ds_rb, ct_dname_cmp);
RB_GENERATE(d_name_tree, dnode, d_rb_name, ct_dname_cmp);

int
ct_dname_cmp(struct dnode *d1, struct dnode *d2)
{
	return strcmp(d2->d_name, d1->d_name);
}

static void
ct_flnode_cleanup(struct flist_head *head)
{
	struct flist *flnode;

	while (!TAILQ_EMPTY(head)) {
		flnode = TAILQ_FIRST(head);
		TAILQ_REMOVE(head, flnode, fl_list);
		if (flnode->fl_fname)
			e_free(&flnode->fl_fname);
		e_free(&flnode);
	}
}

void
ct_free_fnode(struct fnode *fnode)
{
	if (fnode->fl_hlname != NULL)
		e_free(&fnode->fl_hlname);
	if (fnode->fl_sname != NULL)
		e_free(&fnode->fl_sname);
	if (fnode->fl_fname)
		e_free(&fnode->fl_fname);
	if (fnode->fl_name)
		e_free(&fnode->fl_name);
	e_free(&fnode);

}

int
fl_inode_sort(struct flist *f1, struct flist *f2)
{
	int rv;

	if ((rv = f2->fl_dev - f1->fl_dev) != 0)
		return (rv);
	if ((rv = f2->fl_ino - f1->fl_ino) != 0)
		return (rv);

	return (0);
}

static char *
ct_name_to_safename(char *filename, int strip_slash)
{
	char		*safe;

	/* compute 'safe' name */
	safe = filename;
	if (strip_slash && safe[0] == CT_PATHSEP) {
		safe++;
		if (safe[0] == '\0') {
			return NULL;
		}
	}
	while (!(strncmp(safe, ".." CT_PATHSEP_STR, 3)))
		safe += 3;
	if (!strcmp(safe, ".."))
		return NULL;
	/* skip '.' */
	if (!strcmp(filename, ".")) {
		return NULL;
	}
	return safe;
}

static char *
gen_fname(struct flist *flnode)
{
	char *name;

	if (flnode->fl_parent_dir && flnode->fl_parent_dir->d_num != -3) {
		e_asprintf(&name, "%s%c%s", flnode->fl_parent_dir->d_name,
		    CT_PATHSEP, flnode->fl_fname);
	} else {
		name = e_strdup(flnode->fl_fname);
	}

	return name;
}

char *
gen_sname(struct flist *flnode, int strip_slash)
{
	char		*name, *sname;

	name = gen_fname(flnode);
	sname = e_strdup(ct_name_to_safename(name, strip_slash));
	e_free(&name);

	return sname;
}

struct fnode *
ct_get_next_fnode(struct ct_archive_state *cas, struct flist_head *head,
    struct flist **flist, struct ct_match *include, struct ct_match *exclude,
    int strip_slash, int follow_symlinks)
{
	struct fnode	*fnode;
again:
	if (*flist == NULL)
		*flist = TAILQ_FIRST(head);
	else
		*flist = TAILQ_NEXT(*flist, fl_list);
	if (*flist == NULL)
		return (NULL);
	/*
	 * Deleted files will return NULL here, so keep looking until
	 * we find a valid file or we run out of options.
	 */
	while ((fnode = ct_populate_fnode_from_flist(cas, *flist,
	    strip_slash, follow_symlinks)) == NULL &&
	    (*flist = TAILQ_NEXT(*flist, fl_list)) != NULL)
		;
	if (fnode == NULL)
		return (NULL);

	if (include && ct_match(include, fnode->fl_sname)) {
		CNDBG(CT_LOG_FILE, "%s not in include list, skipping",
		    fnode->fl_sname);
		ct_free_fnode(fnode);
		goto again;
	}
	if (exclude && !ct_match(exclude, fnode->fl_sname)) {
		CNDBG(CT_LOG_FILE, "%s in exclude list, skipping",
		    fnode->fl_sname);
		ct_free_fnode(fnode);
		goto again;
	}

	return (fnode);
}

static struct fnode *
ct_populate_fnode_from_flist(struct ct_archive_state *cas,
    struct flist *flnode, int strip_slash, int follow_symlinks)
{
	struct fnode		*fnode;
	struct stat		*sb, sbstore;
	struct dnode		*dfound;
	char			*fname, *dname;
#ifndef CT_NO_OPENAT
	int			dopenflags;
#endif
	int			ret;

	if (flnode->fl_flags & C_FF_CLOSEDIR) {
		dname = gen_fname(flnode);
		if ((dfound = ct_archive_lookup_dir(cas, dname)) == NULL)
			CABORTX("close entry for non existant directory %s",
			    dname);
		e_free(&dname);
#ifndef CT_NO_OPENAT
		if (dfound->d_fd != -1)
			close(dfound->d_fd);
#endif
		dfound->d_fd = -1;
		return (NULL);
	}

	sb = &sbstore;
#ifdef CT_NO_OPENAT
	char	path[PATH_MAX];

	fname = gen_fname(flnode);

	if (ct_absolute_path(fname)) {
		strlcpy(path, fname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s",
		    ct_archive_get_rootdir(cas)->d_name, fname);
	}

	if (follow_symlinks)
		ret = stat(path, sb);
	else
		ret = lstat(path, sb);
#else
	fname = e_strdup(flnode->fl_fname);
	ret = fstatat(flnode->fl_parent_dir->d_fd, flnode->fl_fname,
	    sb, follow_symlinks ? 0 : AT_SYMLINK_NOFOLLOW);
#endif
	if (ret != 0) {
		e_free(&fname);
		/* file no longer available return failure */
		return NULL;
	}

	fnode = e_calloc(1, sizeof(*fnode));

	/*
	 * ct_name_to_safename has run before and not returned failure
	 * so safe to not check for failure of gen_sname() here.
	 */
	fnode->fl_fname = fname;
	fnode->fl_sname = gen_sname(flnode, strip_slash);
	fnode->fl_dev = sb->st_dev;
	fnode->fl_rdev = sb->st_rdev;
	fnode->fl_ino = sb->st_ino;
	fnode->fl_uid = sb->st_uid;
	fnode->fl_gid = sb->st_gid;
	fnode->fl_mode = sb->st_mode;
	fnode->fl_atime = sb->st_atime;
	fnode->fl_mtime = sb->st_mtime;
	fnode->fl_type = s_to_e_type(sb->st_mode);
	fnode->fl_size = sb->st_size;
	fnode->fl_offset = 0;

	if (flnode->fl_flags & C_FF_FORCEDIR)
		fnode->fl_type = C_TY_DIR;
	/*
	 * If we someone tries to perform a symlink race and it happens before
	 * we stat the directory this second time then we may hit the case
	 * where we think a file is a directory, but it is a symlink,
	 * allowing evil path manipulation games. Therefore, if we think it is
	 * a directory then check that it is, in case we have children.
	 */
	if (flnode->fl_flags & C_FF_WASDIR && fnode->fl_type != C_TY_DIR) {
		CWARNX("%s is no longer a directory", fnode->fl_sname);
		ct_free_fnode(fnode);
		return (NULL);
	}

	/* either the parent is NULL (which is fine) or is our parent */
	fnode->fl_parent_dir = flnode->fl_parent_dir;

	fnode->fl_state = CT_FILE_START;
	ct_sha1_setup(&fnode->fl_shactx);

	if (C_ISDIR(fnode->fl_type)) {
		dname = gen_fname(flnode);
		if ((dfound = ct_archive_lookup_dir(cas, dname)) == NULL)
			CABORTX("directory not found in d_name_tree %s",
			    dname);
		e_free(&dname);
		fnode->fl_curdir_dir = dfound;

#ifndef CT_NO_OPENAT
		/* XXX O_SEARCH */
		dopenflags = O_DIRECTORY | O_RDONLY | O_NOFOLLOW;
		if ((flnode->fl_flags & C_FF_FORCEDIR) || follow_symlinks)
			dopenflags &= ~O_NOFOLLOW;

		if ((dfound->d_fd = openat(fnode->fl_parent_dir->d_fd,
		    fnode->fl_fname, dopenflags)) == -1) {
			CWARN("can't open directory %s", fnode->fl_sname);
		}
#endif
	}

	if (flnode->fl_hlnode != NULL) {
		fnode->fl_hardlink = 1;
		fnode->fl_type = C_TY_LINK;
		fnode->fl_hlname = gen_sname(flnode->fl_hlnode, strip_slash);
	} else if (C_ISLINK(fnode->fl_type) && fnode->fl_hardlink == 0) {
		char			 mylink[PATH_MAX];

		ret = ct_readlink(cas, fnode, mylink, sizeof(mylink));
		if (ret == -1 || ret == sizeof(mylink)) {
			CWARN("can't read link for %s", fnode->fl_sname);
			ct_free_fnode(fnode);
			return (NULL);
		}
		/*
		 * readlink(2) does not append a NUL.
		 */
		mylink[ret] = '\0';
		fnode->fl_hlname = e_strdup(mylink);
	}

	return fnode;
}

struct ct_archive_state {
	struct dnode		 cas_rootdir;
	struct d_name_tree	 cas_dname_head;
};

int
ct_archive_init(struct ct_archive_state **casp, const char *tdir)
{
	struct ct_archive_state	*cas;
	cas = e_calloc(1, sizeof(*cas));
	RB_INIT(&cas->cas_dname_head);

	cas->cas_rootdir.d_num = -3;
	cas->cas_rootdir.d_parent = NULL;
	if (tdir != NULL)
		cas->cas_rootdir.d_name = e_strdup(tdir);
	else
		cas->cas_rootdir.d_name = e_strdup(".");

#ifndef CT_NO_OPENAT
	if ((cas->cas_rootdir.d_fd = open(tdir ? tdir : ".",
	    O_RDONLY | O_DIRECTORY)) == -1) {
		int  s_errno = errno;;

		e_free(&cas->cas_rootdir.d_name);
		e_free(&cas);
		errno = s_errno;

		return (CTE_ERRNO);
	}
#endif
	*casp = cas;
	return (0);
}

struct dnode *
ct_archive_get_rootdir(struct ct_archive_state *cas)
{
	return (&cas->cas_rootdir);
}
struct dnode *
ct_archive_lookup_dir(struct ct_archive_state *cas, const char *name)
{
	struct dnode	dsearch;

	dsearch.d_name = (char *)name;
	return (RB_FIND(d_name_tree, &cas->cas_dname_head, &dsearch));
}

struct dnode *
ct_archive_insert_dir(struct ct_archive_state *cas, struct dnode *dir)
{
	return (RB_INSERT(d_name_tree, &cas->cas_dname_head, dir));
}


void
ct_archive_cleanup(struct ct_archive_state *cas)
{
	if (cas->cas_rootdir.d_name != NULL)
		e_free(&cas->cas_rootdir.d_name);
#ifndef CT_NO_OPENAT
	struct dnode	*dnode;
	/*
	 * ct -cf foo.md foo/bar/baz will have foo and foo/bar open at this
	 * point (no fts postorder visiting), close them since we have just
	 * finished with the filesystem.
	 */
	while ((dnode = RB_ROOT(&cas->cas_dname_head)) != NULL) {
		if (dnode->d_fd != -1) {
			CNDBG(CT_LOG_FILE, "%s wasn't closed", dnode->d_name);
			close(dnode->d_fd);
			dnode->d_fd = -1;
		}
		RB_REMOVE(d_name_tree, &cas->cas_dname_head, dnode);
		e_free(&dnode->d_name);
		e_free(&dnode);
	}
	close(cas->cas_rootdir.d_fd);
#endif
	e_free(&cas);
}

static void
ct_sched_backup_file(struct ct_archive_state *cas, struct stat *sb,
    char *filename, int forcedir, int closedir, int strip_slash,
    struct flist_head *flist, struct fl_tree *ino_tree,
    struct ct_statistics *ct_stats)
{
	struct flist		*flnode;
	const char		*safe;
	struct flist		*flnode_exists;
	struct dnode		*dfound, *dnode = NULL, *e_dnode;
	char			*dir_name;

	/* compute 'safe' name */
	safe = ct_name_to_safename(filename, strip_slash);
	if (safe == NULL)
		return;

	if (closedir) {
		if ((dnode = ct_archive_lookup_dir(cas, filename)) == NULL)
			CABORTX("close directory for nonexistant dir %s",
			    filename);
	} else if (forcedir || S_ISDIR(sb->st_mode)) {
		dnode = e_calloc(1, sizeof(*dnode));
		dnode->d_name = e_strdup(filename);
		dnode->d_num = -1; /* numbers are allocated on xdr write */
		if ((e_dnode = ct_archive_insert_dir(cas, dnode)) != NULL) {
			/* this directory already exists, do not record twice */
			e_free(&dnode->d_name);
			e_free(&dnode);
			return;
		} else
			CNDBG(CT_LOG_CTFILE, "inserted %s", filename);
		/* The rest of the intialisation happens below */
	}

	//ct_numalloc++;
	flnode = e_calloc(1, sizeof (*flnode));

	flnode->fl_dev = sb->st_dev;
	flnode->fl_ino = sb->st_ino;
	flnode->fl_parent_dir = NULL;

	dir_name = ct_dirname(filename);
	if ((dfound = ct_archive_lookup_dir(cas, dir_name)) != NULL) {
		flnode->fl_parent_dir = dfound;
		CNDBG(CT_LOG_CTFILE, "parent of %s is %s", filename,
		    dfound->d_name);
		flnode->fl_fname = ct_basename(filename);
		CNDBG(CT_LOG_CTFILE, "setting name of %s as %s", filename,
		    flnode->fl_fname);
	} else {
		flnode->fl_fname = e_strdup(filename);
		flnode->fl_parent_dir = ct_archive_get_rootdir(cas);
		CNDBG(CT_LOG_CTFILE, "parent of %s is not found",
		    flnode->fl_fname);
	}
	e_free(&dir_name);

	if (closedir) {
		flnode->fl_flags |= C_FF_CLOSEDIR;
		goto insert;
	}

	/* fill in directory data now that we have the parent information */
	if (dnode != NULL) {
		dnode->d_parent = flnode->fl_parent_dir;
		dnode->d_sname = e_strdup(flnode->fl_fname);
		dnode->d_fd = -1;
		dnode->d_mode = sb->st_mode;
		dnode->d_atime = sb->st_atime;
		dnode->d_mtime = sb->st_mtime;
		dnode->d_uid = sb->st_uid;
		dnode->d_gid = sb->st_gid;
		 /* we may have children, enforce this being a directory. */
		flnode->fl_flags |= C_FF_WASDIR;
	}

	if (forcedir)
		flnode->fl_flags |= C_FF_FORCEDIR;

	flnode->fl_hlnode = NULL;
	/* deal with hardlink */
	flnode_exists = RB_INSERT(fl_tree, ino_tree, flnode);
	if (flnode_exists != NULL) {
		flnode->fl_hlnode = flnode_exists;
		CNDBG(CT_LOG_CTFILE, "found %s as hardlink of %s", safe,
		    ct_name_to_safename(flnode->fl_hlnode->fl_fname,
		    strip_slash));
	} else {
		if (S_ISREG(sb->st_mode))
			ct_stats->st_bytes_tot += sb->st_size;
	}
	ct_stats->st_files_scanned++;

insert:
	TAILQ_INSERT_TAIL(flist, flnode, fl_list);

	return;
}

struct ct_archive_priv {
	struct flist_head		 cap_flist;
	struct ctfile_write_state	*cap_cws;
	struct ct_match			*cap_include;
	struct ct_match			*cap_exclude;
	struct fnode			*cap_curnode;
	struct flist			*cap_curlist;
	time_t				 cap_prev_backup_time;
	int				 cap_fd;
};

int
ct_archive_complete_special(struct ct_global_state *state,
    struct ct_trans *trans)
{
	state->ct_print_file_start(state->ct_print_state, trans->tr_fl_node);
	state->ct_print_file_end(state->ct_print_state, trans->tr_fl_node,
	    state->ct_max_block_size);
	if (ctfile_write_special(trans->tr_ctfile, trans->tr_fl_node))
		CWARNX("failed to write special entry for %s",
		    trans->tr_fl_node->fl_sname);
	ct_free_fnode(trans->tr_fl_node);
	trans->tr_fl_node = NULL;

	return (0);
}

int
ct_archive_complete_file_start(struct ct_global_state *state,
    struct ct_trans *trans)
{
	if (ctfile_write_file_start(trans->tr_ctfile, trans->tr_fl_node))
		CWARNX("header write failed");

	state->ct_print_file_start(state->ct_print_state, trans->tr_fl_node);
	/* Empty file, or allfiles backup */
	if (trans->tr_eof == 1 || trans->tr_fl_node->fl_skip_file) {
		if (ctfile_write_file_end(trans->tr_ctfile,
		    trans->tr_fl_node))
			CWARNX("failed to write trailer sha");
		state->ct_print_file_end(state->ct_print_state,
		    trans->tr_fl_node, state->ct_max_block_size);
		state->ct_stats->st_files_completed++;
		ct_free_fnode(trans->tr_fl_node);
		trans->tr_fl_node = NULL;
	}
	return (0);
}

int
ct_archive_complete_write_chunk(struct ct_global_state *state,
    struct ct_trans *trans)
{
	state->ct_stats->st_chunks_completed++;
	if (trans->tr_eof < 2) {
		CNDBG(CT_LOG_CTFILE, "XoX sha sz %d eof %d",
		    trans->tr_size[(int)trans->tr_dataslot],
		    trans->tr_eof);

		if (ctfile_write_file_sha(trans->tr_ctfile,
		    trans->tr_sha, trans->tr_csha, trans->tr_iv) != 0)
			CWARNX("failed to write sha for %s",
			    trans->tr_fl_node->fl_sname);
	}

	if (trans->tr_eof) {
		if (trans->tr_eof == 2 && ctfile_write_file_pad(
		    trans->tr_ctfile, trans->tr_fl_node) != 0)
			CWARNX("failed to pad file for %s",
			    trans->tr_fl_node->fl_sname);
		if (ctfile_write_file_end(trans->tr_ctfile,
		    trans->tr_fl_node) != 0)
			CWARNX("failed to write trailer for %s",
			    trans->tr_fl_node->fl_sname);
		state->ct_print_file_end(state->ct_print_state,
		    trans->tr_fl_node, state->ct_max_block_size);
		ct_free_fnode(trans->tr_fl_node);
		trans->tr_fl_node = NULL;
	}

	return (0);
}

int
ct_archive_complete_done(struct ct_global_state *state,
    struct ct_trans *trans)
{
	if (trans->tr_ctfile) {
		ctfile_write_close(trans->tr_ctfile);
	}
	if (state->archive_state) {
		ct_archive_cleanup(state->archive_state);
		state->archive_state = NULL;
	}
	return (1); /* operation is complete */
}

void
ct_archive(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_archive_args	*caa = op->op_args;
	const char		*ctfile = caa->caa_local_ctfile;
	char			**filelist = caa->caa_filelist;
	const char		*basisbackup = caa->caa_basis;
	ssize_t			rlen;
	off_t			rsz;
	struct stat		sb;
	struct ct_trans		*ct_trans;
	struct ct_archive_priv	*cap = op->op_priv;
	char			cwd[PATH_MAX];
	int			new_file = 0;
	int			error;
	int			nextlvl = 0;

	CNDBG(CT_LOG_TRANS, "processing");
	switch (ct_get_file_state(state)) {
	case CT_S_STARTING:
		if (*filelist == NULL) {
			CFATALX("%s", ct_strerror(CTE_NO_FILES_SPECIFIED));
		}

		cap = e_calloc(1, sizeof(*cap));
		cap->cap_fd = -1;
		TAILQ_INIT(&cap->cap_flist);
		op->op_priv = cap;
		if (caa->caa_includelist) {
			if ((error = ct_match_compile(&cap->cap_include,
			    caa->caa_matchmode, caa->caa_includelist)) != 0)
				CFATALX("can't compile include list: %s",
				    ct_strerror(error));
		}
		if (caa->caa_excllist) {
			if ((error = ct_match_compile(&cap->cap_exclude,
			    caa->caa_matchmode, caa->caa_excllist)) != 0)
				CFATALX("can't compile exclude list: %s",
				    ct_strerror(error));
		}

		if (basisbackup != NULL &&
		    (nextlvl = ct_basis_setup(basisbackup, filelist,
		        caa->caa_max_incrementals,
		        &cap->cap_prev_backup_time)) == 0)
			e_free(&basisbackup);

		if (getcwd(cwd, PATH_MAX) == NULL)
			CFATAL("getcwd: %s", ct_strerror(CTE_ERRNO));

		if ((error = ct_archive_init(&state->archive_state,
		    caa->caa_tdir)) != 0)
			CFATALX("can't initialize archive mode: %s",
			    ct_strerror(error));
		if (caa->caa_tdir && chdir(caa->caa_tdir) != 0)
			CFATALX("can't chdir to %s: %s", caa->caa_tdir,
			    ct_strerror(CTE_ERRNO));
		state->ct_print_traverse_start(state->ct_print_state, filelist);
		ct_traverse(state->archive_state, filelist, &cap->cap_flist,
		    caa->caa_no_cross_mounts, caa->caa_strip_slash,
		    caa->caa_follow_root_symlink, caa->caa_follow_symlinks,
		    state->ct_stats);
		state->ct_print_traverse_end(state->ct_print_state, filelist);
		if (caa->caa_tdir && chdir(cwd) != 0)
			CFATALX("can't chdir back to %s: %s", cwd,
			    ct_strerror(CTE_ERRNO));
		/*
		 * Get the first file we must operate on.
		 * Do this before we open the ctfile for writing so
		 * if all are excluded we don't then have to unlink it.
		 */
		cap->cap_curlist = NULL;
		if ((cap->cap_curnode = ct_get_next_fnode(state->archive_state,
		    &cap->cap_flist, &cap->cap_curlist, cap->cap_include,
		    cap->cap_exclude, caa->caa_strip_slash,
		    caa->caa_follow_symlinks)) == NULL)
			CFATALX("%s", ct_strerror(CTE_ALL_FILES_EXCLUDED));


		/* XXX - deal with stdin */
		/* XXX - if basisbackup should the type change ? */
		if ((error = ctfile_write_init(&cap->cap_cws, ctfile,
		    caa->caa_ctfile_basedir, CT_MD_REGULAR, basisbackup,
		    nextlvl, cwd, filelist, caa->caa_encrypted,
		    caa->caa_allfiles, state->ct_max_block_size)) != 0)
			CFATAL("can't create %s: %s", ctfile,
			    ct_strerror(error));

		if (basisbackup != NULL)
			e_free(&basisbackup);
		break;
	case CT_S_FINISHED:
		return;
	default:
		break;
	}
	ct_set_file_state(state, CT_S_RUNNING);

	if (cap->cap_curnode == NULL)
		goto done;
loop:
	CNDBG(CT_LOG_CTFILE, "file %s state %d", cap->cap_curnode->fl_sname,
	    cap->cap_curnode->fl_state);
	new_file = (cap->cap_curnode->fl_state == CT_FILE_START);

	/* allocate transaction */
	ct_trans = ct_trans_alloc(state);
	if (ct_trans == NULL) {
		/* system busy, return */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}

	/*
	 * Only regular files that haven't just been opened need to talk
	 * to the server. don't waste slots.
	 */
	if (!C_ISREG(cap->cap_curnode->fl_type) || new_file)
		ct_trans = ct_trans_realloc_local(state, ct_trans);

	/* handle special files */
	if (!C_ISREG(cap->cap_curnode->fl_type)) {
		if (C_ISDIR(cap->cap_curnode->fl_type)) {
			/*
			 * we do want to skip old directories with
			 * no (new) files in them
			 */
			if (ct_stat(cap->cap_curnode, &sb,
			    caa->caa_follow_symlinks, NULL,
			    state->archive_state) != 0) {
				CWARN("archive: dir %s stat error",
				    cap->cap_curnode->fl_sname);
			} else {
				if (sb.st_mtime < cap->cap_prev_backup_time) {
					CNDBG(CT_LOG_FILE, "skipping dir"
					    " based on mtime %s",
					    cap->cap_curnode->fl_sname);
					ct_free_fnode(cap->cap_curnode);
					ct_trans_free(state, ct_trans);
					goto skip;
				}
			}
		}
		ct_trans->tr_ctfile = cap->cap_cws;
		ct_trans->tr_fl_node = cap->cap_curnode;
		cap->cap_curnode->fl_state = CT_FILE_FINISHED;
		cap->cap_curnode->fl_size = 0;
		ct_trans->tr_state = TR_S_SPECIAL;
		ct_trans->tr_type = TR_T_SPECIAL;
		ct_trans->tr_complete = ct_archive_complete_special;
		ct_trans->tr_eof = 0;
		ct_queue_first(state, ct_trans);
		goto next_file;
	}

	/* do not open zero length files */
	if (new_file) {
		cap->cap_curnode->fl_state = CT_FILE_PROCESSING;
		if (cap->cap_fd != -1) {
			CABORTX("state error, new file open,"
			    " sz %" PRId64 " offset %" PRId64,
			    (int64_t) cap->cap_curnode->fl_size,
			    (int64_t) cap->cap_curnode->fl_offset);
		}

		if ((cap->cap_fd = ct_open(state->archive_state,
		    cap->cap_curnode, O_RDONLY,
		    caa->caa_follow_symlinks)) == -1) {
			CWARN("archive: unable to open file '%s'",
			    cap->cap_curnode->fl_sname);
			ct_trans_free(state, ct_trans);
			cap->cap_curnode->fl_state = CT_FILE_FINISHED;
			goto next_file;
		}

		error = fstat(cap->cap_fd, &sb);
		if (error) {
			CWARN("archive: file %s stat error",
			    cap->cap_curnode->fl_sname);
		} else {
			if (sb.st_mtime < cap->cap_prev_backup_time) {
				state->ct_print_file_skip(state->ct_print_state,
				    cap->cap_curnode);
				cap->cap_curnode->fl_skip_file = 1;
			}
		}
		/*
		 * Now we have actually statted the file atomically
		 * confirm the permissions bits that we got with the last
		 * stat.
		 */
		if (!S_ISREG(sb.st_mode)) {
			CWARNX("%s is no longer a regular file, skipping",
			    cap->cap_curnode->fl_sname);
			cap->cap_curnode->fl_skip_file = 1;
		}
		cap->cap_curnode->fl_dev = sb.st_dev;
		cap->cap_curnode->fl_rdev = sb.st_rdev;
		cap->cap_curnode->fl_ino = sb.st_ino;
		cap->cap_curnode->fl_uid = sb.st_uid;
		cap->cap_curnode->fl_gid = sb.st_gid;
		cap->cap_curnode->fl_mode = sb.st_mode;
		cap->cap_curnode->fl_atime = sb.st_atime;
		cap->cap_curnode->fl_mtime = sb.st_mtime;
		cap->cap_curnode->fl_size = sb.st_size;

		ct_trans->tr_ctfile = cap->cap_cws;
		ct_trans->tr_fl_node = cap->cap_curnode;
		ct_trans->tr_state = TR_S_FILE_START;
		ct_trans->tr_type = TR_T_WRITE_HEADER;
		ct_trans->tr_complete = ct_archive_complete_file_start;
		if (cap->cap_curnode->fl_size == 0 ||
		    cap->cap_curnode->fl_skip_file) {
			close(cap->cap_fd);
			cap->cap_fd = -1;
			ct_trans->tr_eof = 1;
			cap->cap_curnode->fl_state = CT_FILE_FINISHED;
		} else {
			ct_trans->tr_eof = 0;
		}

		/*
		 * Allfiles backups needs to still record skipped files.
		 * Non allfiles backups don't need to do anything with them
		 * so we can dump them here.
		 */
		if (cap->cap_curnode->fl_skip_file && caa->caa_allfiles == 0) {
			ct_free_fnode(cap->cap_curnode);
			ct_trans_free(state, ct_trans);
			goto skip;
		}

		ct_queue_first(state, ct_trans);

		if (cap->cap_curnode->fl_size == 0 ||
		    cap->cap_curnode->fl_skip_file) {
			goto next_file;
		}
		goto loop;
	} else {
		if (cap->cap_fd == -1) {
			CABORTX("state error, old file not open,"
			    " sz %" PRId64 " offset %" PRId64,
			    (int64_t) cap->cap_curnode->fl_size,
			    (int64_t) cap->cap_curnode->fl_offset);
		}
	}

	/* perform read */
	rsz = cap->cap_curnode->fl_size - cap->cap_curnode->fl_offset;
	CNDBG(CT_LOG_FILE, "rsz %lu max %d", (unsigned long) rsz,
	    state->ct_max_block_size);
	if (rsz > state->ct_max_block_size) {
		rsz = state->ct_max_block_size;
	}
	ct_trans->tr_dataslot = 0;
	rlen = 0;
	if (rsz > 0)
		rlen = read(cap->cap_fd, ct_trans->tr_data[0], rsz);

	if (rlen > 0)
		state->ct_stats->st_bytes_read += rlen;

	ct_trans->tr_ctfile = cap->cap_cws;
	ct_trans->tr_fl_node = cap->cap_curnode;
	ct_trans->tr_size[0] = rlen;
	ct_trans->tr_chsize = rlen;
	ct_trans->tr_state = TR_S_READ;
	ct_trans->tr_type = TR_T_WRITE_CHUNK;
	ct_trans->tr_complete = ct_archive_complete_write_chunk;
	ct_trans->tr_eof = 0;
	ct_trans->hdr.c_flags = caa->caa_encrypted ? C_HDR_F_ENCRYPTED : 0;
	/* update offset */
	if (rsz != rlen || rlen == 0 || ((cap->cap_curnode->fl_offset + rlen) ==
	        cap->cap_curnode->fl_size)) {
		/* short read, file truncated, or end of file */
		/* restat file for modifications */
		error = fstat(cap->cap_fd, &sb);

		close(cap->cap_fd);
		cap->cap_fd = -1;
		ct_trans->tr_eof = 1;
		cap->cap_curnode->fl_state = CT_FILE_FINISHED;

		if (error) {
			CWARN("archive: file %s stat error",
			    cap->cap_curnode->fl_sname);
		} else if (sb.st_size != cap->cap_curnode->fl_size) {
			CWARNX("\"%s\" %s during backup",
			    cap->cap_curnode->fl_sname,
			    (sb.st_size > cap->cap_curnode->fl_size) ? "grew" :
				"truncated");
			ct_trans->tr_state = TR_S_WMD_READY;
			ct_trans->tr_eof = 2;
		}
	} else {
		cap->cap_curnode->fl_offset += rlen;
	}
	ct_queue_first(state, ct_trans);
	CNDBG(CT_LOG_FILE, "read %ld for block %" PRIu64 " eof %d",
	    (long)rlen, ct_trans->tr_trans_id, ct_trans->tr_eof);


	/* if file finished, update curnode to next node in list */
	/* XXX is there other file metadata that needs to be saved?  */
next_file:
	/* XXX should node be removed from list at this time? */
	if (cap->cap_curnode->fl_state == CT_FILE_FINISHED) {
		CNDBG(CT_LOG_FILE, "going to next file %s",
		    cap->cap_curnode->fl_sname);
skip:
		if ((cap->cap_curnode = ct_get_next_fnode(state->archive_state,
		    &cap->cap_flist, &cap->cap_curlist, cap->cap_include,
		    cap->cap_exclude, caa->caa_strip_slash,
		    caa->caa_follow_symlinks)) == NULL) {
			CNDBG(CT_LOG_FILE, "no more files");
		} else {
			CNDBG(CT_LOG_FILE, "going to next file %s",
			    cap->cap_curnode->fl_sname);
		}
	}

	if (cap->cap_curnode != NULL)
		goto loop;

done:
	CNDBG(CT_LOG_FILE, "last file read");
	/* done with backup */
	ct_set_file_state(state, CT_S_FINISHED);

	ct_trans = ct_trans_alloc(state);
	if (ct_trans == NULL) {
		/* system busy, return */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}
	ct_trans->tr_ctfile = cap->cap_cws;
	ct_trans->tr_fl_node = NULL;
	ct_trans->tr_state = TR_S_DONE;
	ct_trans->tr_complete = ct_archive_complete_done;
	ct_trans->tr_eof = 0;

	/* We're done, cleanup local state. */
	if (cap->cap_include)
		ct_match_unwind(cap->cap_include);
	if (cap->cap_exclude)
		ct_match_unwind(cap->cap_exclude);
	ct_flnode_cleanup(&cap->cap_flist);
	/* cws is cleaned up by the completion handler */
	e_free(&cap);

	ct_queue_first(state, ct_trans);
}

static void
ct_traverse(struct ct_archive_state *cas, char **paths,
    struct flist_head *files, int no_cross_mounts, int strip_slash,
    int follow_root_symlink, int follow_symlinks,
    struct ct_statistics *ct_stats)
{
	FTS			*ftsp;
	FTSENT			*fe;
	struct fl_tree		 ino_tree;
	char			 clean[PATH_MAX];
	int			 fts_options, cnt, forcedir, ret;

	RB_INIT(&ino_tree);
	fts_options = FTS_NOCHDIR;
	if (follow_symlinks)
		fts_options |= FTS_LOGICAL;
	else
		fts_options |= FTS_PHYSICAL;
	if (follow_root_symlink) {
		fts_options |= FTS_COMFOLLOW;
	}
	if (no_cross_mounts)
		fts_options |= FTS_XDEV;
	CDBG("options =  %d", fts_options);
	ftsp = fts_open(paths, fts_options, NULL);
	if (ftsp == NULL)
		CFATALX("fts_open: %s", ct_strerror(CTE_ERRNO));

	cnt = 0;
	while ((fe = fts_read(ftsp)) != NULL) {
		forcedir = 0;
		switch (fe->fts_info) {
		case FTS_D:
		case FTS_DEFAULT:
		case FTS_F:
		case FTS_SL:
		case FTS_SLNONE:
			cnt++;
			/* these are ok */
			/* FALLTHROUGH */
		case FTS_DP: /* Setup for close dir, no stats */
			/* sanitize path */
			if (eat_double_dots(fe->fts_path, clean) == NULL)
				CFATALX("%s: %s", fe->fts_path,
				    ct_strerror(CTE_CRAZY_PATH));
			if (fe->fts_info == FTS_DP)
				goto sched;
			break;
		case FTS_DC:
			CWARNX("file system cycle found");
			continue;
		case FTS_DNR:
		case FTS_NS:
			errno = fe->fts_errno;
			CWARN("unable to access %s", fe->fts_path);
			continue;
		default:
			CABORTX("bad fts_info (%d)", fe->fts_info);
		}

		/* backup dirs above fts starting point */
		if (fe->fts_level == 0) {
			/* XXX technically this should apply to files too */
			if (follow_root_symlink && fe->fts_info == FTS_D)
				forcedir = 1;
			if ((ret = backup_prefix(cas, clean, files, &ino_tree,
			    strip_slash, ct_stats)) != 0)
				CFATALX("backup_prefix: %s",
				    ct_strerror(ret));
		}

		CNDBG(CT_LOG_FILE, "scheduling backup of %s", clean);
		/* backup all other files */
sched:
		ct_sched_backup_file(cas, fe->fts_statp, clean, forcedir,
		    fe->fts_info == FTS_DP ? 1 : 0, strip_slash, files,
		    &ino_tree, ct_stats);

	}

	if (cnt == 0)
		CFATALX("%s", ct_strerror(CTE_NO_FILES_ACCESSIBLE));

	if (fe == NULL && errno)
		CFATALX("fts_read: %s", ct_strerror(CTE_ERRNO));
	if (fts_close(ftsp))
		CFATALX("fts_close: %s", ct_strerror(CTE_ERRNO));
	gettimeofday(&ct_stats->st_time_scan_end, NULL);
}

static char *
eat_double_dots(char *path, char *resolved)
{
	char	**tab = NULL, *buf = NULL, *rv = NULL, *cp, **ntab;
	int	sz = 0, bufsz, i;

	/* emulate realpath(3) for those cases */
	if (path == NULL || *path == '\0') {
		strlcpy(resolved, ".", PATH_MAX);
		return (resolved);
	}

	/*
	 * append dummy component that will be eventually ignored;
	 * greatly simplifies the splitting code below.
	 */
	bufsz = e_asprintf(&buf, "%s/dummy", path);

	/* split path into components */
	for (;;) {
		cp = dirname(buf);
		if (cp == NULL)
			goto done;
		else
			strlcpy(buf, cp, bufsz);

		cp = basename(buf);
		if (cp == NULL)
			goto done;

		ntab = e_realloc(tab, (sz + 1) * sizeof(char *));
		tab = ntab;
		tab[sz++] = e_strdup(cp);

		if (!strcmp(buf, ".") || !strcmp(buf, CT_PATHSEP_STR))
			break; /* reached the top */
	}

	/* walk path components top to bottom */
	for (i = sz - 1; i >= 0; i--) {
		cp = tab[i];

		/* topmost component is always either / or . */
		if (i == sz - 1) {
			strlcpy(resolved, cp, PATH_MAX);
			continue;
		}

		/* '.' component is redundant */
		if (!strcmp(cp, "."))
			continue;

		/* '..' component is special */
		if (!strcmp(cp, "..")) {
			if (!strcmp(resolved, CT_PATHSEP_STR))
				continue; /* cannot go beyond fs root */

			/* remove last component if other than '..' */
			if (strcmp(basename(resolved), ".") != 0 &&
			    strcmp(basename(resolved), "..") != 0)
				strlcpy(resolved, dirname(resolved), PATH_MAX);
			else
				strlcat(resolved, CT_PATHSEP_STR "..",
				    PATH_MAX);
			continue;
		}

		/* append regular component */
		if (strcmp(resolved, CT_PATHSEP_STR) != 0)
			strlcat(resolved, CT_PATHSEP_STR, PATH_MAX);
		strlcat(resolved, cp, PATH_MAX);
	}

	if (!strncmp(resolved, "." CT_PATHSEP_STR, 2))
		memmove(resolved, resolved + 2, PATH_MAX - 2);

	rv = resolved;
done:
	if (buf)
		e_free(&buf);
	for (i = 0; i < sz; i++)
		e_free(&tab[i]);
	e_free(&tab);
	return (rv);
}

static int
backup_prefix(struct ct_archive_state *cas, char *root,
    struct flist_head *flist, struct fl_tree *ino_tree, int strip_slash,
    struct ct_statistics *ct_stats)
{
	char			dir[PATH_MAX], rbuf[PATH_MAX], pfx[PATH_MAX];
	char			*cp, *p;
	struct stat		sb;

	/* it is just the prefix that needs to be parsed */
	strlcpy(rbuf, root, sizeof rbuf);
	strlcpy(pfx, dirname(rbuf), sizeof pfx);

	/* archive each leading dir */
	p = pfx;
	bzero(&dir, sizeof dir);
	for (;; strlcat(dir, CT_PATHSEP_STR, sizeof dir)) {
		cp = strsep(&p, CT_PATHSEP_STR);
		if (cp == NULL)
			break; /* parsed it all */
		if (*cp == '\0')
			continue; /* beginning of absolute path */

		/* extend prefix */
		strlcat(dir, cp, sizeof dir);

		/* XXX racy? */
		if (stat(dir, &sb))
			return (CTE_ERRNO);

		/* file type changed since fts_open */
		if (!S_ISDIR(sb.st_mode)) {
			errno = ENOTDIR;
			return (CTE_ERRNO);
		}

		ct_sched_backup_file(cas, &sb, dir, 1, 0, strip_slash,
		    flist, ino_tree, ct_stats);
	}

	return (0);
}

static int
s_to_e_type(int mode)
{
	int rv = C_TY_INVALID;

	if (S_ISREG(mode))
		rv = C_TY_REG;
	else if (S_ISDIR(mode))
		rv = C_TY_DIR;
	else if (S_ISCHR(mode))
		rv = C_TY_CHR;
	else if (S_ISBLK(mode))
		rv = C_TY_BLK;
	else if (S_ISFIFO(mode))
		rv = C_TY_FIFO;
	else if (S_ISLNK(mode))
		rv = C_TY_LINK;
	else if (S_ISSOCK(mode))
		rv = C_TY_SOCK;

	return (rv);
}

struct ct_extract_state {
	int			 ces_fd;
	int			 ces_attr;
	int			 ces_follow_symlinks;
	int			 ces_allfiles;
	struct dnode		*ces_rootdir;
	struct dnode		*ces_prevdir;
	struct dnode		**ces_prevdir_list;
	struct d_name_tree	 ces_dname_head;
	void			*ces_log_state;
	ct_log_chown_failed_fn	*ces_log_chown_failed;
};

void	ct_file_extract_nextdir(struct ct_extract_state *, struct fnode *);
void	ct_file_extract_closefrom(struct ct_extract_state *, struct dnode *,
	    struct dnode *);
void	ct_file_extract_opento(struct ct_extract_state *, struct dnode *,
	    struct dnode *);

void
ct_file_extract_log_default(void *state, struct fnode *fnode,
    struct dnode *dnode)
{
}

int
ct_file_extract_init(struct ct_extract_state **cesp, const char *tdir,
    int attr, int follow_symlinks, int allfiles, void *log_state,
    ct_log_chown_failed_fn *log_chown_failed)
{
	struct ct_extract_state	*ces;
	char			 tpath[PATH_MAX];
	int			 tries = 0, s_errno;

	ces = e_calloc(1, sizeof(*ces));
	ces->ces_fd = -1;
	ces->ces_rootdir = e_calloc(1, sizeof(*ces->ces_rootdir));
	ces->ces_attr = attr;
	ces->ces_follow_symlinks = follow_symlinks;
	ces->ces_allfiles = allfiles;
	if (log_chown_failed != NULL) {
		ces->ces_log_state = log_state;
		ces->ces_log_chown_failed = log_chown_failed;
	} else {
		ces->ces_log_chown_failed = ct_file_extract_log_default;
	}
	RB_INIT(&ces->ces_dname_head);

	ces->ces_rootdir->d_num = -3;
	ces->ces_rootdir->d_parent = NULL;
try_again:
	/* ct_make_full_path can mess with the string we are using */
	if (tdir != NULL) {
		strlcpy(tpath, tdir, sizeof(tpath));
	} else {
		strlcpy(tpath, ".", sizeof(tpath));
	}
	/* Open the root directory fd node */
#ifdef CT_NO_OPENAT
	struct stat sb;

	if (stat(tpath, &sb) == -1) {
#else
	if ((ces->ces_rootdir->d_fd = open(tpath,
	    O_RDONLY | O_DIRECTORY)) == -1) {
#endif
		/*
		 * We will only hit this case for tdir.
		 * XXX a more restrictive mask wanted?
		 */
		if (errno == ENOENT && tries++ == 0 &&
		    ct_make_full_path(tpath, 0777) == 0 &&
		    mkdir(tdir, 0777) == 0)
			goto try_again;
		s_errno = errno;
		
		e_free(&ces->ces_rootdir);
		e_free(&ces);
		errno = s_errno;

		return (CTE_ERRNO);
	}
	ces->ces_rootdir->d_name = e_strdup(tpath);

	*cesp = ces;
	return (0);
}

struct dnode *
ct_file_extract_get_rootdir(struct ct_extract_state *ces)
{
	return (ces->ces_rootdir);
}

struct dnode *
ct_file_extract_insert_dir(struct ct_extract_state *ces, struct dnode *dnode)
{
	return (RB_INSERT(d_name_tree, &ces->ces_dname_head, dnode));
}

struct dnode *
ct_file_extract_lookup_dir(struct ct_extract_state *ces, const char *path)
{
	struct dnode dsearch;

	dsearch.d_name = (char *)path;
	return (RB_FIND(d_name_tree, &ces->ces_dname_head, &dsearch));
}

void
ct_file_extract_cleanup(struct ct_extract_state *ces)
{
	struct dnode *dnode;
	/* Close all open directories, we are switching files */
	if (ces->ces_prevdir_list != NULL) {
		ct_file_extract_closefrom(ces, ces->ces_prevdir_list[0],
		    ces->ces_prevdir);
		e_free(&ces->ces_prevdir_list);
	}
	while ((dnode = RB_ROOT(&ces->ces_dname_head)) != NULL) {
		RB_REMOVE(d_name_tree, &ces->ces_dname_head, dnode);
		e_free(&dnode->d_name);
		e_free(&dnode);
	}
	if (ces->ces_rootdir->d_name)
		e_free(&ces->ces_rootdir->d_name);
#ifndef CT_NO_OPENAT
	close(ces->ces_rootdir->d_fd);
#endif
	e_free(&ces->ces_rootdir);
	e_free(&ces);
}


#ifndef CT_NO_OPENAT
#define TEMPCHARS	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
#define NUM_CHARS	(sizeof(TEMPCHARS) - 1)

/*
 * mkstemp() using openat in the specified directory fd. Semantics remain the
 * same.
 * Code borrowed from OpenBSD libc under the following license:
*
 * Copyright (c) 1996-1998, 2008 Theo de Raadt
 * Copyright (c) 1997, 2008-2009 Todd C. Miller
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
static int
mkstemp_at(int dir, char *path)
{
	char *start, *cp, *ep;
	const char *tempchars = TEMPCHARS;
	unsigned int r, tries;
	size_t len;
	int fd;

	len = strlen(path);
	if (len == 0) {
		errno = EINVAL;
		return(-1);
	}
	ep = path + len;

	tries = 1;
	for (start = ep; start > path && start[-1] == 'X'; start--) {
		if (tries < INT_MAX / NUM_CHARS)
			tries *= NUM_CHARS;
	}
	tries *= 2;

	do {
		for (cp = start; cp != ep; cp++) {
			r = arc4random_uniform(NUM_CHARS);
			*cp = tempchars[r];
		}

		fd = openat(dir, path, O_CREAT|O_EXCL|O_RDWR, S_IRUSR|S_IWUSR);
		if (fd != -1 || errno != EEXIST)
			return(fd);
	} while (--tries);

	errno = EEXIST;
	return(-1);
}
#endif


int
ct_file_extract_open(struct ct_extract_state *ces, struct fnode *fnode)
{
	if (ces->ces_fd != -1) {
		CABORTX("file open on extract_open");
	}

	ct_file_extract_nextdir(ces, fnode);

	CNDBG(CT_LOG_FILE, "opening %s for writing", fnode->fl_sname);

	if (fnode->fl_fname)
		e_free(&fnode->fl_fname);
	/*
	 * All previous directories should have been created when we changed
	 * directory above. If this is not the case then something changed
	 * after we made them. just warn and continue.
	 */
#ifdef CT_NO_OPENAT
	char tpath[PATH_MAX], dirpath[PATH_MAX], *dirp;

	if (ct_absolute_path(fnode->fl_sname)) {
		strlcpy(tpath, fnode->fl_sname, sizeof(tpath));
	} else {
		snprintf(tpath, sizeof(tpath), "%s%c%s",
		    ces->ces_rootdir->d_name, CT_PATHSEP, fnode->fl_sname);
	}
	strlcpy(dirpath, tpath, sizeof(dirpath));
	/*
	 * dirname() shouldn't fatal al-la posix, however implelemtations that
	 * use an internal buffer fail if the string is too long. Since our
	 * string is already capped at PATH_MAX this should not ever fail.
	 */
	if ((dirp = dirname(dirpath)) == NULL) /* XXX CTE_ERRNO */
		CABORTX("can't get dirname of %s", tpath);

	e_asprintf(&fnode->fl_fname, "%s%c%s", dirp, CT_PATHSEP,
	    "cyphertite.XXXXXXXXXX");
	ces->ces_fd = mkstemp(fnode->fl_fname);
#else
	fnode->fl_fname = e_strdup("cyphertite.XXXXXXXXXX");
	ces->ces_fd = mkstemp_at(fnode->fl_parent_dir->d_fd,
	    fnode->fl_fname);
#endif
	if (ces->ces_fd == -1)
		return (1);
	return (0);
}

int
ct_file_extract_write(struct ct_extract_state *ces, struct fnode *fnode,
    uint8_t *buf, size_t size)
{
	ssize_t	len;
	int	ret = 0;

	if (fnode == NULL)
		CABORTX("file write on non open file");

	len = write(ces->ces_fd, buf, size);
	if (len != size)
		ret = CTE_ERRNO;
	return (ret);
}

void
ct_file_extract_close(struct ct_extract_state *ces, struct fnode *fnode)
{
	struct timeval          tv[2];
	int                     safe_mode;

	safe_mode = S_IRWXU | S_IRWXG | S_IRWXO;
	if (ces->ces_attr) {
		if (fchown(ces->ces_fd, fnode->fl_uid, fnode->fl_gid) == -1) {
			ces->ces_log_chown_failed(ces->ces_log_state,
			    fnode, NULL);
		} else
			safe_mode = ~0;
	}

	if (fchmod(ces->ces_fd, fnode->fl_mode & safe_mode) == -1) {
		CWARN("chmod failed on %s", fnode->fl_sname);
	} else if (ces->ces_attr) {
		tv[0].tv_sec = fnode->fl_atime;
		tv[1].tv_sec = fnode->fl_mtime;
		tv[0].tv_usec = tv[1].tv_usec = 0;
		if (futimes(ces->ces_fd, tv) == -1)
			CWARN("utimes on %s failed", fnode->fl_sname);
	}
	if (ct_rename(ces, fnode) != 0) {
		CWARN("rename to %s failed", fnode->fl_sname);
		/* nuke temp file */
		(void)ct_unlink(ces, fnode);
	}

	close(ces->ces_fd);
	ces->ces_fd = -1;
}

void
ct_file_extract_special(struct ct_extract_state *ces, struct fnode *fnode)
{
	char			apath[PATH_MAX];
	char			*appath;
	int			ret = 0;
	int                     safe_mode;

	/*
	 * Create dependant directories and open/close any relvevant directory
	 * filedescriptors.
	 */
	ct_file_extract_nextdir(ces, fnode);

	CNDBG(CT_LOG_FILE, "special %s mode %d", fnode->fl_sname,
	    fnode->fl_mode);

	if (C_ISDIR(fnode->fl_type)) {
		if (ct_mkdir(ces, fnode, 0700)) {
			if (errno != EEXIST) /* XXX check it is a dir */
				CWARN("can't create directory %s",
				    fnode->fl_sname);
		}
	} else if (C_ISBLK(fnode->fl_type) || C_ISCHR(fnode->fl_type))  {
		if (ct_mknod(ces, fnode) != 0) {
			if (errno != EEXIST) /* XXX check it is a spec node */
				CWARN("can't create special file %s",
				    fnode->fl_sname);
		}
	} else if (C_ISLINK(fnode->fl_type)){
		if (fnode->fl_hardlink && fnode->fl_hlname[0] != '/') {
			snprintf(apath, sizeof(apath), "%s%c%s",
			    ces->ces_rootdir->d_name, CT_PATHSEP,
			    fnode->fl_hlname);
			appath = apath;
		} else {
			appath = fnode->fl_hlname;
		}

link_again:
		if (fnode->fl_hardlink) {
			/*
			 * XXX AT_FDCWD is dangerous here but we don't have
			 * sufficient information.
			 * We pass SYMLINK_FOLLOW to give the same semantics
			 * as link()
			 */
			ret = ct_link(ces, fnode, appath);
		} else {
			ret = ct_symlink(ces, fnode);
		}
		if (ret && errno == EEXIST) {
			if (fnode->fl_hardlink) {
				struct stat	tsb, lsb;

				/*
				 * XXX don't necessarily have the cwd of
				 * the hardlink open.
				 */
				if (lstat(appath, &tsb) != 0) {
					CWARN("can't stat %s", appath);
					goto link_out;
				}
				if (ct_stat(fnode, &lsb,
				    ces->ces_follow_symlinks, ces, NULL) != 0) {
					CWARN("can't stat %s", fnode->fl_sname);
					goto link_out;
				}
				if (tsb.st_dev != lsb.st_dev) {
					CWARNX("%s and %s no longer on same "
					    "device: can't link",
					    appath, fnode->fl_sname);
					goto link_out;
				}
				/*
				 * If inodes match, then carry on, we're
				 * already ok
				 */
				if (tsb.st_ino == lsb.st_ino) {
					ret = 0;
					goto link_out;
				}
			}

			if (ct_unlink(ces, fnode) == 0)
				goto link_again;
			CWARN("can't remove old link %s", fnode->fl_sname);
		}
link_out:
		if (ret) {
			CWARN("%s failed: %s to %s", fnode->fl_hardlink ?
			    "link" : "symlink", fnode->fl_sname, appath);
			return;
		}
	} else {
		CABORTX("illegal file %s of type %d", fnode->fl_sname,
		    fnode->fl_mode);
	}

	if (C_ISDIR(fnode->fl_type)) {
		/*
		 * Directory permissions are handled at directory close
		 * time when all dependancies are finished.
		 */
		;
	} else if (C_ISLINK(fnode->fl_type)){
		if (!fnode->fl_hardlink) {
			/* symlinks have no 'real' permissions */
			if (ces->ces_attr) {
				/* set the link's ownership */
				if (ct_chown(ces, fnode, 0) != 0) {
					ces->ces_log_chown_failed(
					    ces->ces_log_state, fnode, NULL);
				}
			}
		} else  {
			/* hardlinks have no mode/permissions */
			;
		}
	} else {
		safe_mode = S_IRWXU | S_IRWXG | S_IRWXO;
		if (ces->ces_attr) {
			/* XXX should this depend on follow_symlinks? */
			if (ct_chown(ces, fnode, 1) != 0) {
				ces->ces_log_chown_failed(ces->ces_log_state,
				    fnode, NULL);
			} else
				safe_mode = ~0;
		}

		if (ct_chown(ces, fnode, fnode->fl_mode & safe_mode) != 0) {
			CWARN("chmod failed on %s", fnode->fl_sname);
		} else if (ces->ces_attr) {
			if (ct_utimes(ces, fnode) != 0)
				CWARN("utimes failed");
		}
	}
}

void
ct_file_extract_nextdir(struct ct_extract_state *ces, struct fnode *fnode)
{
	struct dnode	*newdir = fnode->fl_parent_dir, *tdir;
	struct dnode	**newdirlist;
	int		 ndirs, i;

	/* If we're in the same directory, we're done */
	if (newdir == ces->ces_prevdir) {
		return;
	}

	/* count number of directories. */
	for (tdir = newdir, ndirs = 0; tdir != NULL;
	    (tdir = tdir->d_parent), ndirs++)
		;

	/* should never happen */
	if (ndirs == 0)
		CABORTX("no dirs");

	newdirlist = e_calloc(ndirs + 1, sizeof(*newdirlist));

	/* newdirlist is NULL terminated, build it backwards */
	for (tdir = newdir, i = ndirs - 1; tdir != NULL; tdir = tdir->d_parent)
		newdirlist[i--] = tdir;
	if (ces->ces_prevdir_list == NULL) {
		i = 0;
		goto open;
	}

	/*
	 * find the common parent
	 * we know the directories are not the same, so this should halt
	 */
	for (i = 0; ; i++) {
		if (newdirlist[i + 1] != ces->ces_prevdir_list[i + 1])
			break;
	}

	/* close all children from common parent up to old dir */
	ct_file_extract_closefrom(ces, ces->ces_prevdir_list[i],
	    ces->ces_prevdir);
open:
	/* open all children from common parent up to new dir */
	ct_file_extract_opento(ces, newdirlist[i], newdir);

	ces->ces_prevdir = newdir;
	if (ces->ces_prevdir_list != NULL)
		e_free(&ces->ces_prevdir_list);
	ces->ces_prevdir_list = newdirlist;
}

void
ct_file_extract_closefrom(struct ct_extract_state *ces, struct dnode *parent,
    struct dnode *child)
{
#ifdef CT_NO_OPENAT
	struct timeval		tv[2];
	char			path[PATH_MAX];

#else
	struct timespec		ts[2];
#endif
	int                     safe_mode;

	if (child == parent)
		return;

	/* Set directory permissions to what they should be, then close it. */
	safe_mode = S_IRWXU | S_IRWXG | S_IRWXO;

#ifdef CT_NO_OPENAT
	snprintf(path, sizeof(path), "%s/%s", ces->ces_rootdir->d_name,
	    child->d_name);
	if (ces->ces_attr) {
		if (chown(path, child->d_uid, child->d_gid) == -1) {
			ces->ces_log_chown_failed(ces->ces_log_state,
			    NULL, child);
		} else {
			safe_mode = ~0;
		}
	}
	if (chmod(path, child->d_mode & safe_mode) == -1) {
		CWARNX("can't chmod directory \"%s\"", child->d_name);
		/* no point trying to utimes, too */
		goto out;
	} else if (ces->ces_attr) {
		tv[0].tv_sec = child->d_atime;
		tv[1].tv_sec = child->d_mtime;
		tv[0].tv_usec = tv[1].tv_usec = 0;
		if (utimes(path, tv) == -1)
			CWARN("utimes on \"%s\" failed", child->d_name);
	}
#else
	if (ces->ces_attr) {
		if (fchown(child->d_fd, child->d_uid, child->d_gid) == -1) {
			ces->ces_log_chown_failed(ces->ces_log_state,
			    NULL, child);
		} else {
			safe_mode = ~0;
		}
	}
	if (fchmod(child->d_fd, child->d_mode & safe_mode) == -1) {
		CWARN("can't chmod directory \"%s\"", child->d_name);
		/* no point trying to utimes, too */
	} else if (ces->ces_attr) {
		ts[0].tv_sec = child->d_atime;
		ts[1].tv_sec = child->d_mtime;
		ts[0].tv_nsec = ts[1].tv_nsec = 0;
		if (futimens(child->d_fd, ts) == -1)
			CWARN("futimens on \"%s\" failed", child->d_name);
	}
	close(child->d_fd);
#endif
	child->d_fd = -2;

	ct_file_extract_closefrom(ces, parent, child->d_parent);
}

void
ct_file_extract_opento(struct ct_extract_state *ces, struct dnode *parent,
    struct dnode *child)
{
	int			createtries = 0, chmodtries = 0, savederrno;
#ifdef CT_NO_OPENAT
	struct stat		sb;
	char			path[PATH_MAX];
	int			ret;
#endif

	if (child == parent)
		return;

	ct_file_extract_opento(ces, parent, child->d_parent);
	/* check it exists, if it does not, create it */
try_again:
	/* XXX O_SEARCH would be applicable here but openbsd doesn't have it */
#ifdef CT_NO_OPENAT
	if (ct_absolute_path(child->d_name)) {
		strlcpy(path, child->d_name, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", ces->ces_rootdir->d_name,
		    child->d_name);
	}
	if (ces->ces_follow_symlinks) {
		ret = stat(path, &sb);
	} else {
		ret = lstat(path, &sb);
	}

	if (ret != 0) {
		savederrno = errno;
		/* if it doesn't exist, make the file with safe permissions */
		if (errno == ENOENT && createtries++ == 0 &&
		    mkdir(path, S_IRWXU) == 0)
			goto try_again;
		/* if it exists but we can't access it, try and chmod the dir */
		if (errno == EACCES && chmodtries++ == 0 &&
		    chmod(path, S_IRWXU) == 0)
			goto try_again;
		errno = savederrno;
		CWARN("can't open directory %s", child->d_name);
	}
#else
	if ((child->d_fd = openat(child->d_parent->d_fd, child->d_sname,
	    O_DIRECTORY | O_RDONLY |
	    ces->ces_follow_symlinks ? 0 : O_NOFOLLOW)) == -1) {
		savederrno = errno;
		/* if it doesn't exist, make the file with safe permissions */
		if (errno == ENOENT && createtries++ == 0 &&
		    mkdirat(child->d_parent->d_fd, child->d_sname,
		    S_IRWXU) == 0)
			goto try_again;
		/* if it exists but we can't access it, try and chmod the dir */
		if (errno == EACCES && chmodtries++ == 0 &&
		    fchmodat(child->d_parent->d_fd, child->d_sname,
			S_IRWXU, ces->ces_follow_symlinks ? 0 :
			AT_SYMLINK_NOFOLLOW) == 0)
			goto try_again;
		errno = savederrno;
		CWARN("can't open directory %s", child->d_name);
	}
#endif
}

/*
 * Portability functions to be used by archive-like operations.
 * These operate on fnode->fl_name as the correct ``real'' path.
 */
int
ct_open(struct ct_archive_state *cas , struct fnode *fnode, int flags,
    int follow_symlinks)
{
#ifdef CT_NO_OPENAT
	char	path[PATH_MAX];

	if (ct_absolute_path(fnode->fl_fname)) {
		strlcpy(path, fnode->fl_fname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", cas->cas_rootdir.d_name,
		    fnode->fl_fname);
	}

	return (open(path, flags | follow_symlinks ? 0 : O_NOFOLLOW));
#else
	return (openat(fnode->fl_parent_dir->d_fd, fnode->fl_fname,
	    flags | follow_symlinks ? 0 : O_NOFOLLOW));
#endif
}

int
ct_readlink(struct ct_archive_state *cas, struct fnode *fnode, char *mylink,
    size_t mylinksz)
{
#ifdef CT_NO_OPENAT
	char	path[PATH_MAX];

	if (ct_absolute_path(fnode->fl_fname)) {
		strlcpy(path, fnode->fl_fname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", cas->cas_rootdir.d_name,
		    fnode->fl_fname);
	}

	return (readlink(path, mylink, mylinksz));
#else
	return (readlinkat(fnode->fl_parent_dir->d_fd, fnode->fl_fname,
	    mylink, mylinksz));
#endif
}

/*
 * Portability function used by both extract and archive like operations.
 */
int
ct_stat(struct fnode *fnode, struct stat *sb, int follow_symlinks,
    struct ct_extract_state *ces, struct ct_archive_state *cas)
{
#ifdef CT_NO_OPENAT
	char	path[PATH_MAX];

	if (ct_absolute_path(ces ? fnode->fl_sname : fnode->fl_fname)) {
		strlcpy(path, ces ? fnode->fl_sname : fnode->fl_fname,
		    sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", ces ?
		    ces->ces_rootdir->d_name : cas->cas_rootdir.d_name,
		    ces ? fnode->fl_sname : fnode->fl_fname);
	}
	if (follow_symlinks)
		return (stat(path, sb));
	else
		return (lstat(path, sb));
#else
	return (fstatat(fnode->fl_parent_dir->d_fd,
	    ces ? fnode->fl_name : fnode->fl_fname, sb, follow_symlinks ? 0 :
	    AT_SYMLINK_NOFOLLOW));
#endif
}


/*
 * Portability functions to be used by extract operations.
 * These operated on fnode->fl_sname as the ``correct'' pathname.
 */
int
ct_mkdir(struct ct_extract_state *ces, struct fnode *fnode, mode_t mode)
{
#ifdef CT_NO_OPENAT
	char	path[PATH_MAX];

	if (ct_absolute_path(fnode->fl_sname)) {
		strlcpy(path, fnode->fl_sname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", ces->ces_rootdir->d_name,
		    fnode->fl_sname);
	}
	return (mkdir(path, mode));
#else
	return (mkdirat(fnode->fl_parent_dir->d_fd, fnode->fl_name, mode));
#endif
}

int
ct_mknod(struct ct_extract_state *ces, struct fnode *fnode)
{
#ifdef CT_NO_OPENAT
	char	path[PATH_MAX];

	if (ct_absolute_path(fnode->fl_sname)) {
		strlcpy(path, fnode->fl_sname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", ces->ces_rootdir->d_name,
		    fnode->fl_sname);
	}
	return (mknod(path, fnode->fl_mode, fnode->fl_dev));
#else
	return (mknodat(fnode->fl_parent_dir->d_fd, fnode->fl_name,
	    fnode->fl_mode, fnode->fl_dev));
#endif
}

int
ct_link(struct ct_extract_state *ces, struct fnode *fnode, char *destination)
{
#ifdef CT_NO_OPENAT
	char	path[PATH_MAX];

	if (ct_absolute_path(fnode->fl_sname)) {
		strlcpy(path, fnode->fl_sname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", ces->ces_rootdir->d_name,
		    fnode->fl_sname);
	}
	return (link(destination, path));
#else
	return (linkat(AT_FDCWD, destination, fnode->fl_parent_dir->d_fd,
	    fnode->fl_name, AT_SYMLINK_FOLLOW));
#endif
}

int
ct_symlink(struct ct_extract_state *ces, struct fnode *fnode)
{
#ifdef CT_NO_OPENAT
	char	path[PATH_MAX];

	if (ct_absolute_path(fnode->fl_sname)) {
		strlcpy(path, fnode->fl_sname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", ces->ces_rootdir->d_name,
		    fnode->fl_sname);
	}
	return (symlink(fnode->fl_hlname, path));
#else
	return (symlinkat(fnode->fl_hlname, fnode->fl_parent_dir->d_fd,
	    fnode->fl_name));
#endif
}

int
ct_rename(struct ct_extract_state *ces, struct fnode *fnode)
{
#ifdef CT_NO_OPENAT
	char	path[PATH_MAX];

	if (ct_absolute_path(fnode->fl_sname)) {
		strlcpy(path, fnode->fl_sname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", ces->ces_rootdir->d_name,
		    fnode->fl_sname);
	}
	return (rename(fnode->fl_fname, path));
#else
	return (renameat(fnode->fl_parent_dir->d_fd, fnode->fl_fname,
	    fnode->fl_parent_dir->d_fd, fnode->fl_name));
#endif
}

int
ct_chmod(struct ct_extract_state *ces, struct fnode *fnode, mode_t mode)
{
#ifdef CT_NO_OPENAT
	char	path[PATH_MAX];

	if (ct_absolute_path(fnode->fl_sname)) {
		strlcpy(path, fnode->fl_sname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", ces->ces_rootdir->d_name,
		    fnode->fl_sname);
	}
	return (chmod(path, mode));
#else
	return (fchmodat(fnode->fl_parent_dir->d_fd, fnode->fl_name, mode, 0));
#endif
}

int
ct_chown(struct ct_extract_state *ces, struct fnode *fnode, int follow_symlinks)
{
#ifdef CT_NO_OPENAT
	char	path[PATH_MAX];

	if (ct_absolute_path(fnode->fl_sname)) {
		strlcpy(path, fnode->fl_sname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", ces->ces_rootdir->d_name,
		    fnode->fl_sname);
	}
	if (follow_symlinks)
		return (chown(path, fnode->fl_uid,
		    fnode->fl_gid));
	else
		return (lchown(path, fnode->fl_uid,
		   fnode->fl_gid));
#else
	return (fchownat(fnode->fl_parent_dir->d_fd,
	    fnode->fl_name, fnode->fl_uid,
	    fnode->fl_gid, follow_symlinks ? 0 : AT_SYMLINK_NOFOLLOW));
#endif
}

int
ct_utimes(struct ct_extract_state *ces, struct fnode *fnode)
{
#ifdef CT_NO_OPENAT
	char		path[PATH_MAX];
	struct timeval	tv[2];

	tv[0].tv_sec = fnode->fl_atime;
	tv[1].tv_sec = fnode->fl_mtime;
	tv[0].tv_usec = tv[1].tv_usec = 0;

	if (ct_absolute_path(fnode->fl_sname)) {
		strlcpy(path, fnode->fl_sname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", ces->ces_rootdir->d_name,
		    fnode->fl_sname);
	}

	return (utimes(path, tv));
#else
	struct timespec	ts[2];

	ts[0].tv_sec = fnode->fl_atime;
	ts[1].tv_sec = fnode->fl_mtime;
	ts[0].tv_nsec = ts[1].tv_nsec = 0;

	return (utimensat(fnode->fl_parent_dir->d_fd,
	    fnode->fl_name, ts, 0));
#endif
}

int
ct_unlink(struct ct_extract_state *ces, struct fnode *fnode)
{
#ifdef CT_NO_OPENAT
	char		path[PATH_MAX];

	if (ct_absolute_path(fnode->fl_sname)) {
		strlcpy(path, fnode->fl_sname, sizeof(path));
	} else {
		snprintf(path, sizeof(path), "%s/%s", ces->ces_rootdir->d_name,
		    fnode->fl_sname);
	}

	return (unlink(path));
#else
	return (unlinkat(fnode->fl_parent_dir->d_fd, fnode->fl_name, 0));
#endif
}

int
ct_populate_fnode(struct ct_extract_state *ces, struct ctfile_parse_state *ctx,
    struct fnode *fnode, int *state, int allfiles, int strip_slash)
{
	struct dnode		*dnode, *tdnode;
	char			*name;

	if (C_ISLINK(ctx->xs_hdr.cmh_type)) {
		/* hardlink/symlink */
		fnode->fl_hlname = e_strdup(ctx->xs_lnkhdr.cmh_filename);
		fnode->fl_hardlink = !C_ISLINK(ctx->xs_lnkhdr.cmh_type);
		*state = TR_S_EX_SPECIAL;

	} else if (!C_ISREG(ctx->xs_hdr.cmh_type)) {
		/* special file/dir */
		*state = TR_S_EX_SPECIAL;
	} else {
		/* regular file */
		*state = TR_S_EX_FILE_START;
	}

	/* ino not preserved? */
	fnode->fl_rdev = ctx->xs_hdr.cmh_rdev;
	fnode->fl_uid = ctx->xs_hdr.cmh_uid;
	fnode->fl_gid = ctx->xs_hdr.cmh_gid;
	fnode->fl_mode = ctx->xs_hdr.cmh_mode;
	fnode->fl_mtime = ctx->xs_hdr.cmh_mtime;
	fnode->fl_atime = ctx->xs_hdr.cmh_atime;
	fnode->fl_type = ctx->xs_hdr.cmh_type;

	/* Default to parent being the ``root''. */
	fnode->fl_parent_dir = ct_file_extract_get_rootdir(ces);
	name = ctx->xs_hdr.cmh_filename;
	/* fnode->fl_parent_dir default to NULL */
	if (ctx->xs_hdr.cmh_parent_dir == -2) {
		/* rooted directory */
		e_asprintf(&fnode->fl_sname, "%s%s", strip_slash ? "" : "/",
		    ctx->xs_hdr.cmh_filename);
		name = fnode->fl_sname;
	} else if (ctx->xs_hdr.cmh_parent_dir != -1) {
		fnode->fl_parent_dir = ctfile_parse_finddir(ctx,
		    ctx->xs_hdr.cmh_parent_dir);
		if (fnode->fl_parent_dir == NULL)
			CABORTX("can't find parent dir %" PRId64 " for %s",
			    ctx->xs_hdr.cmh_parent_dir, ctx->xs_hdr.cmh_filename);
		e_asprintf(&fnode->fl_sname, "%s/%s",
		    fnode->fl_parent_dir->d_name, ctx->xs_hdr.cmh_filename);
		CNDBG(CT_LOG_CTFILE,
		    "parent_dir %p %" PRId64, fnode->fl_parent_dir,
		    ctx->xs_hdr.cmh_parent_dir);
	} else
		fnode->fl_sname = e_strdup(ctx->xs_hdr.cmh_filename);

	/* name needed for openat() */
	fnode->fl_name = e_strdup(name);

	CNDBG(CT_LOG_CTFILE,
	    "name %s from %s %" PRId64, fnode->fl_sname, ctx->xs_hdr.cmh_filename,
	    ctx->xs_hdr.cmh_parent_dir);

	if (C_ISDIR(ctx->xs_hdr.cmh_type)) {
		dnode = e_calloc(1,sizeof (*dnode));
		dnode->d_name = e_strdup(fnode->fl_sname);
		dnode->d_sname = e_strdup(name);
		dnode->d_fd = -1;
		dnode->d_parent = fnode->fl_parent_dir;
		dnode->d_mode = fnode->fl_mode;
		dnode->d_atime = fnode->fl_atime;
		dnode->d_mtime = fnode->fl_mtime;
		dnode->d_uid = fnode->fl_uid;
		dnode->d_gid = fnode->fl_gid;

		/* Insert into the name tree first to check for duplicates. */
		if ((tdnode = ct_file_extract_insert_dir(ces, dnode)) != NULL) {
			if (allfiles == 0) {
				/* update stat data */
				tdnode->d_mode = dnode->d_mode;
				tdnode->d_atime = dnode->d_atime;
				tdnode->d_mtime = dnode->d_mtime;
				tdnode->d_uid = dnode->d_uid;
				tdnode->d_gid = dnode->d_gid;
			}
			 e_free(&dnode);
			 dnode = tdnode;
		}
		/* XXX check duplicates? */
		ctfile_parse_insertdir(ctx, dnode);
		CNDBG(CT_LOG_CTFILE, "inserting %s as %" PRId64,
		    dnode->d_name, dnode->d_num );
	}


	return 0;
}

/* Trim trailing '/' */
char *
ct_normalize_path(char *path)
{
	char	*cp;

	cp = path + strlen(path) - 1;
	while (cp >= path && (*cp == '/')) {
		cp--;
	}

	if (cp >= path) {
		*(cp + 1) = '\0';
	}

	return path;
}

char *
ct_dirname(const char *orig_path)
{
	char		*path_buf = NULL;
	size_t		 end;

	if (orig_path == NULL || *orig_path == '\0') {
		return (e_strdup("."));
	}

	end = strlen(orig_path) - 1;
	while (end > 0 && orig_path[end] == '/')
		end--;

	while (end > 0 && orig_path[end] != '/')
		end--;

	if (end == 0) {
		if (orig_path[end] == '/')
			return (e_strdup("/"));
		else
			return (e_strdup("."));
	} else {
		while (end > 0 && orig_path[end] == '/') {
			end--;
		}
	}

	end++;
	path_buf = e_malloc(end + 1); /* will fatal if fails */

	memcpy(path_buf, orig_path, end);
	path_buf[end] = '\0';
	return (path_buf);
}

char *
ct_basename(const char *orig_path)
{
	char		*path_buf = NULL;
	size_t		 start, end, len;

	if (orig_path == NULL || *orig_path == '\0') {
		return (e_strdup("."));
	}

	end = strlen(orig_path) - 1;
	while (end > 0 && orig_path[end] == '/')
		end--;

	if (end == 0 && orig_path[end] == '/') {
		return (e_strdup("/"));
	}

	start = end;
	while (start > 0 && orig_path[start - 1] != '/')
		start--;

	len = end - start + 1;
	path_buf = e_malloc(len + 1); /* will fatal if fails */

	memcpy(path_buf, &orig_path[start], len);
	path_buf[len] = '\0';
	return (path_buf);
}

int
ct_absolute_path(const char *path)
{
	return (path[0] == '/');
}