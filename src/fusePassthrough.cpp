/*
 *    Copyright (C) 2019-2021 Joshua Boudreau <jboudreau@45drives.com>
 *    
 *    This file is part of autotier.
 * 
 *    autotier is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 * 
 *    autotier is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 * 
 *    You should have received a copy of the GNU General Public License
 *    along with autotier.  If not, see <https://www.gnu.org/licenses/>.
 */

#define HAVE_UTIMENSAT

#include "fusePassthrough.hpp"
#include "tierEngine.hpp"
#include "file.hpp"
#include "alert.hpp"


#define FUSE_USE_VERSION 30
extern "C"{
	#include <string.h>
	#include <fuse.h>
	#include <stdlib.h>
	#include <unistd.h>
	#include <fcntl.h>
	#include <sys/stat.h>
	#include <dirent.h>
	#include <errno.h>
	#ifdef __FreeBSD__
	#include <sys/socket.h>
	#include <sys/un.h>
	#endif
	#include <sys/time.h>
	#ifdef HAVE_SETXATTR
	#include <sys/xattr.h>
	#endif
}

namespace FuseGlobal{
	static rocksdb::DB *db_;
	static std::vector<Tier *> tiers_;
}

fs::path _mountpoint_;

FusePassthrough::FusePassthrough(std::list<Tier> &tiers, rocksdb::DB *db){
	for(std::list<Tier>::iterator tptr = tiers.begin(); tptr != tiers.end(); ++tptr){
		FuseGlobal::tiers_.push_back(&(*tptr));
	}
	FuseGlobal::db_ = db;
}

// helpers

static int is_directory(const char *relative_path){
	return fs::is_directory(FuseGlobal::tiers_.front()->path() / relative_path);
}

static int mknod_wrapper(int dirfd, const char *path, const char *link,
						 int mode, dev_t rdev)
{
	int res;
	
	if (S_ISREG(mode)) {
		res = openat(dirfd, path, O_CREAT | O_EXCL | O_WRONLY, mode);
		if (res >= 0)
			res = close(res);
	} else if (S_ISDIR(mode)) {
		res = mkdirat(dirfd, path, mode);
	} else if (S_ISLNK(mode) && link != NULL) {
		res = symlinkat(link, dirfd, path);
	} else if (S_ISFIFO(mode)) {
		res = mkfifoat(dirfd, path, mode);
		#ifdef __FreeBSD__
	} else if (S_ISSOCK(mode)) {
		struct sockaddr_un su;
		int fd;
		
		if (strlen(path) >= sizeof(su.sun_path)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd >= 0) {
			/*
			 * We must bind the socket to the underlying file
			 * system to create the socket file, even though
			 * we'll never listen on this socket.
			 */
			su.sun_family = AF_UNIX;
			strncpy(su.sun_path, path, sizeof(su.sun_path));
			res = bindat(dirfd, fd, (struct sockaddr*)&su,
						 sizeof(su));
			if (res == 0)
				close(fd);
		} else {
			res = -1;
		}
		#endif
	} else {
		res = mknodat(dirfd, path, mode, rdev);
	}
	
	return res;
}

// methods

static int at_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi){
	int res;
	(void) path;
	
	if(fi == NULL){
		if(is_directory(path)){
			res = lstat((FuseGlobal::tiers_.front()->path() / path).c_str(), stbuf);
		}else{
			std::string real_path = Metadata(path, FuseGlobal::db_).tier_path() + "/" + path;
			res = lstat(real_path.c_str(), stbuf);
		}
	}else{
		res = fstat(fi->fh, stbuf);
	}
	
	if (res == -1)
		return -errno;
	return 0;
}

static int at_readlink(const char *path, char *buf, size_t size){
	int res;
	
	std::string real_path = Metadata(path, FuseGlobal::db_).tier_path() + "/" + path;
	res = readlink(real_path.c_str(), buf, size - 1);
	
	if (res == -1)
		return -errno;
	buf[res] = '\0';
	return 0;
}

static int at_mknod(const char *path, mode_t mode, dev_t rdev){
	int res;
	fs::path fullpath(FuseGlobal::tiers_.front()->path() / path);
	
	res = mknod_wrapper(AT_FDCWD, fullpath.c_str(), NULL, mode, rdev);
	
	if (res == -1)
		return -errno;
	File(path, FuseGlobal::db_, FuseGlobal::tiers_.front());
	return 0;
}

static int at_mkdir(const char *path, mode_t mode){
	int res;
	
	for(Tier *tptr : FuseGlobal::tiers_){
		res = mkdir((tptr->path() / fs::path(path)).c_str(), mode);
		if (res == -1)
			return -errno;
	}
	
	return 0;
}

static int at_unlink(const char *path){
	int res;
	std::string real_path = Metadata(path, FuseGlobal::db_).tier_path() + "/" + path;
	
	res = unlink(real_path.c_str());
	
	if (res == -1)
		return -errno;
	
	if(!FuseGlobal::db_->Delete(rocksdb::WriteOptions(), path).ok())
		return -1;
	return 0;
}

static int at_rmdir(const char *path){
	int res;
	
	for(Tier *tptr : FuseGlobal::tiers_){
		res = rmdir((tptr->path() / path).c_str());
		if (res == -1)
			return -errno;
	}
	
	return 0;
}

static int at_symlink(const char *from, const char *to){
	int res;
	
	res = symlink(from, (FuseGlobal::tiers_.front()->path() / to).c_str());
	
	if (res == -1)
		return -errno;
	return 0;
}

static int at_rename(const char *from, const char *to, unsigned int flags){
	int res;
	
	if (flags)
		return -EINVAL;
	
	Metadata f(from, FuseGlobal::db_);
	fs::path tier_path = f.tier_path();
	
	res = rename((tier_path / from).c_str(), (tier_path / to).c_str());
	if (res == -1)
		return -errno;
	
	f.update(to, FuseGlobal::db_);
	
	if(!FuseGlobal::db_->Delete(rocksdb::WriteOptions(), from).ok())
		return -1;
	return 0;
}

static int at_link(const char *from, const char *to){
	int res;
	
	Metadata f(from, FuseGlobal::db_);
	fs::path tier_path = f.tier_path();
	
	res = link((tier_path / from).c_str(), (tier_path / to).c_str());
	
	if(res == -1)
		return -errno;
	
	Metadata l(to, FuseGlobal::db_);
	
	return res;
}

static int at_chmod(const char *path, mode_t mode, struct fuse_file_info *fi){
	int res;
	(void) fi;
	
	if(is_directory(path)){
		for(Tier *tptr: FuseGlobal::tiers_){
			res = chmod((tptr->path() / path).c_str(), mode);
			if (res == -1)
				return -errno;
		}
	}else{
		Metadata f(path, FuseGlobal::db_);
		fs::path tier_path = f.tier_path();
		res = chmod((tier_path / path).c_str(), mode);
	}
	
	if (res == -1)
		return -errno;
	return res;
}

static int at_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi){
	int res;
	(void) fi;
	
	if(is_directory(path)){
		for(Tier *tptr: FuseGlobal::tiers_){
			res = lchown((tptr->path() / path).c_str(), uid, gid);
			if (res == -1)
				return -errno;
		}
	}else{
		Metadata f(path, FuseGlobal::db_);
		fs::path tier_path = f.tier_path();
		res = lchown((tier_path / path).c_str(), uid, gid);
	}
	
	if (res == -1)
		return -errno;
	return 0;
}

static int at_truncate(const char *path, off_t size, struct fuse_file_info *fi){
	int res;
	
	if (fi == NULL){
		Metadata f(path, FuseGlobal::db_);
		fs::path tier_path = f.tier_path();
		res = truncate((tier_path / path).c_str(), size);
	}else{
		res = ftruncate(fi->fh, size);
	}
	
	if (res == -1)
		return -errno;
	return 0;
}

static int at_open(const char *path, struct fuse_file_info *fi){
	int res;
	
	if(is_directory(path)){
		res = open((FuseGlobal::tiers_.front()->path() / path).c_str(), fi->flags);
		if (res == -1)
			return -errno;
	}else{
		Metadata f(path, FuseGlobal::db_);
		fs::path tier_path = f.tier_path();
		res = open((tier_path / path).c_str(), fi->flags);
	}
	
	if (res == -1)
		return -errno;
	fi->fh = res;
	return 0;
}

static int at_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
	int res;
	(void) path;
	
	res = pread(fi->fh, buf, size, offset);
	
	if(res == -1)
		return -errno;
	return res;
}

static int at_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
	int res;
	(void) path;
	
	res = pwrite(fi->fh, buf, size, offset);
	
	if (res == -1)
		return -errno;
	return res;
}

static int at_statfs(const char *path, struct statvfs *stbuf){
	int res;
	struct statvfs fs_stats_temp;
	memset(stbuf, 0, sizeof(struct statvfs));
	
	for(Tier *tptr : FuseGlobal::tiers_){
		res = statvfs((tptr->path() / path).c_str(), &fs_stats_temp);
		if (res == -1)
			return -errno;
		if(stbuf->f_bsize == 0) stbuf->f_bsize = fs_stats_temp.f_bsize;
		if(stbuf->f_frsize == 0) stbuf->f_frsize = fs_stats_temp.f_frsize;
		stbuf->f_blocks += fs_stats_temp.f_blocks;
		stbuf->f_bfree += fs_stats_temp.f_bfree;
		stbuf->f_bavail += fs_stats_temp.f_bavail;
		stbuf->f_files += fs_stats_temp.f_files;
		if(stbuf->f_ffree == 0) stbuf->f_ffree = fs_stats_temp.f_ffree;
		if(stbuf->f_favail == 0) stbuf->f_favail = fs_stats_temp.f_favail;
	}
	
	return 0;
}

static int at_flush(const char *path, struct fuse_file_info *fi)
{
	int res;
	(void) path;
	
	/* This is called from every close on an open file, so call the
	 *	   close on the underlying filesystem.	But since flush may be
	 *	   called multiple times for an open file, this must not really
	 *	   close the file.  This is important if used on a network
	 *	   filesystem like NFS which flush the data/metadata on close() */
	
	res = close(dup(fi->fh));
	
	if (res == -1)
		return -errno;
	return 0;
}


static int at_release(const char *path, struct fuse_file_info *fi){
	(void) path;
	close(fi->fh);
	return 0;
}

static int at_fsync(const char *path, int isdatasync, struct fuse_file_info *fi){
	int res;
	(void) path;
	
	#ifndef HAVE_FDATASYNC
	(void) isdatasync;
	#else
	if (isdatasync)
		res = fdatasync(fi->fh);
	else
		#endif
		res = fsync(fi->fh);
	
	if (res == -1)
		return -errno;
	return 0;
}

#ifdef HAVE_SETXATTR
static int at_setxattr(const char *path, const char *name, const char *value, size_t size, int flags){
	
}

static int at_getxattr(const char *path, const char *name, char *value, size_t size){
	
}

static int at_listxattr(const char *path, char *list, size_t size){
	
}

static int at_removexattr(const char *path, const char *name){
	
}

#endif
static int at_readdir(
	const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi,
	enum fuse_readdir_flags flags
 					){
	DIR *dp;
	struct dirent *de;
	
	(void) offset;
	(void) fi;
	
	for(Tier *tptr : FuseGlobal::tiers_){
		dp = opendir((tptr->path() / path).c_str());
		if (dp == NULL)
			return -errno;
		
		while ((de = readdir(dp)) != NULL) {
			if(de->d_type == DT_DIR && tptr != FuseGlobal::tiers_.front())
				continue;
			struct stat st;
			memset(&st, 0, sizeof(st));
			st.st_ino = de->d_ino;
			st.st_mode = de->d_type << 12;
			if (filler(buf, de->d_name, &st, 0, (fuse_fill_dir_flags)0))
				break;
		}
		
		closedir(dp);
	}
	return 0;
}

void *at_init(struct fuse_conn_info *conn, struct fuse_config *cfg){
	(void) conn;
	cfg->use_ino = 1;
	
	/* Pick up changes from lower filesystem right away. This is
	 *		 also necessary for better hardlink support. When the kernel
	 *		 calls the unlink() handler, it does not know the inode of
	 *		 the to-be-removed entry and can therefore not invalidate
	 *		 the cache of the associated inode - resulting in an
	 *		 incorrect st_nlink value being reported for any remaining
	 *		 hardlinks to this inode. */
	
	cfg->entry_timeout = 0;
	cfg->attr_timeout = 0;
	cfg->negative_timeout = 0;
	
	return NULL;
}

static int at_access(const char *path, int mask){
	int res;
	fs::path p(path);
	
	if(is_directory(path)){
		for(Tier *tptr: FuseGlobal::tiers_){
			res = access((tptr->path() / path).c_str(), mask);
			if (res == -1)
				return -errno;
		}
	}else{
		Metadata f(path, FuseGlobal::db_);
		fs::path tier_path = f.tier_path();
		res = access((tier_path / path).c_str(), mask);
		f.touch();
		f.update(path, FuseGlobal::db_);
	}
	
	if (res == -1)
		return -errno;
	return 0;
}

static int at_create(const char *path, mode_t mode, struct fuse_file_info *fi){
	int res;
	fs::path fullpath(FuseGlobal::tiers_.front()->path() / path);
	
	res = open(fullpath.c_str(), fi->flags, mode);
	
	if (res == -1)
		return -errno;
	
	Metadata(path, FuseGlobal::db_, FuseGlobal::tiers_.front());
	fi->fh = res;
	
	return 0;
}

#ifdef HAVE_UTIMENSAT
static int at_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi){
	int res;
	(void) fi;
	
	Metadata f(path, FuseGlobal::db_);
	fs::path tier_path = f.tier_path();
	
	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, (tier_path / path).c_str(), ts, AT_SYMLINK_NOFOLLOW);
	
	if (res == -1)
		return -errno;
	return 0;
}

#endif

static int at_write_buf(const char *path, struct fuse_bufvec *buf, off_t offset, struct fuse_file_info *fi){
	struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));
	
	(void) path;
	
	dst.buf[0].flags = (fuse_buf_flags)(FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
	dst.buf[0].fd = fi->fh;
	dst.buf[0].pos = offset;
	
	return fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
}

static int at_read_buf(const char *path, struct fuse_bufvec **bufp, size_t size, off_t offset, struct fuse_file_info *fi){
	struct fuse_bufvec *src;
	
	(void) path;
	
	src = (struct fuse_bufvec *)malloc(sizeof(struct fuse_bufvec));
	if (src == NULL)
		return -ENOMEM;
	
	*src = FUSE_BUFVEC_INIT(size);
	
	src->buf[0].flags = (fuse_buf_flags)(FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
	src->buf[0].fd = fi->fh;
	src->buf[0].pos = offset;
	
	*bufp = src;
	
	return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int at_fallocate(const char *path, int mode, off_t offset, off_t length, struct fuse_file_info *fi){
	(void) path;
	
	if (mode)
		return -EOPNOTSUPP;
	
	return -posix_fallocate(fi->fh, offset, length);
}

#endif
#ifdef HAVE_COPY_FILE_RANGE
static ssize_t at_copy_file_range(
	const char *path_in, struct fuse_file_info *fi_in, off_t offset_in, const char *path_out,
	struct fuse_file_info *fi_out, off_t offset_out, size_t len, int flags
){
	
}

#endif
static off_t at_lseek(const char *path, off_t off, int whence, struct fuse_file_info *fi){
	int fd;
	off_t res;
	
	if (fi == NULL){
		Metadata f(path, FuseGlobal::db_);
		fs::path tier_path = f.tier_path();
		fd = open((tier_path / path).c_str(), O_RDONLY);
	}else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;
	
	res = lseek(fd, off, whence);
	if (res == -1)
		res = -errno;
	
	if (fi == NULL)
		close(fd);
	return res;
}


int FusePassthrough::mount_fs(fs::path mountpoint, char *fuse_opts){
	_mountpoint_ = mountpoint; // global
	Logging::log.message("Mounting filesystem", 2);
	static const struct fuse_operations at_oper = {
		.getattr					= at_getattr,
		.readlink					= at_readlink,
		.mknod						= at_mknod,
		.mkdir						= at_mkdir,
		.unlink						= at_unlink,
		.rmdir						= at_rmdir,
		.symlink					= at_symlink,
		.rename						= at_rename,
		.link							= at_link,
		.chmod						= at_chmod,
		.chown						= at_chown,
		.truncate					= at_truncate,
		.open							= at_open,
		.read							= at_read,
		.write						= at_write,
		.statfs						= at_statfs,
		.flush            = at_flush,
		.release					= at_release,
		.fsync						= at_fsync,
		#ifdef HAVE_SETXATTR
		.setxattr					= at_setxattr,
		.getxattr					= at_getxattr,
		.listxattr				= at_listxattr,
		.removexattr			= at_removexattr,
		#endif
		.readdir					= at_readdir,
		.init			 				= at_init,
		.access						= at_access,
		.create 					= at_create,
		#ifdef HAVE_UTIMENSAT
		.utimens					= at_utimens,
		#endif
		.write_buf        = at_write_buf,
		.read_buf         = at_read_buf,
		#ifdef HAVE_POSIX_FALLOCATE
		.fallocate				= at_fallocate,
		#endif
		#ifdef HAVE_COPY_FILE_RANGE
		.copy_file_range 	= at_copy_file_range,
		#endif
		.lseek						= at_lseek,
	};
	//std::cerr << std::string("\"") + std::string(mountpoint_) + std::string("\"") << std::endl;
	std::vector<char *>argv = {strdup("autotier"), strdup(mountpoint.c_str())};
	if(fuse_opts){
		argv.push_back(strdup("-o"));
		argv.push_back(fuse_opts);
	}
	return fuse_main(argv.size(), argv.data(), &at_oper, NULL);
}
