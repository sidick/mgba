/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba-util/vfs.h>

#include <mgba-util/string.h>

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef __amigaos__
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

/* AmigaDOS has no single filesystem root the way POSIX has "/" -- SYS:,
 * RAM:, and any other mounted volume all sit side by side with nothing
 * above them, the same way Wii's VDeviceList() (vfs-devlist.c) stands in
 * for a root on a system with multiple memory-card/USB mounts and no
 * unifying tree either. VDirOpen(""), below, returns this in place of
 * opendir() for exactly that case -- letting a caller (gui-platform.c's
 * own basePath, in particular) start the ROM browser at a real "every
 * mounted volume" listing instead of hardcoding one specific volume
 * (e.g. "SYS:") as if it were the top of everything. */
struct VDirEntryAmigaVol {
	struct VDirEntry d;
	char name[32 + 2]; /* AmigaDOS volume/device names: max 30 chars (BSTR length byte is a UBYTE, but real filesystems cap well under that) plus ':' and NUL. */
};

struct VDirAmigaVolList {
	struct VDir d;
	struct VDirEntryAmigaVol vde;
	char** names;
	size_t count;
	size_t index;
};

static bool _vdavClose(struct VDir* vd) {
	struct VDirAmigaVolList* vdav = (struct VDirAmigaVolList*) vd;
	size_t i;
	for (i = 0; i < vdav->count; ++i) {
		free(vdav->names[i]);
	}
	free(vdav->names);
	free(vdav);
	return true;
}

static void _vdavRewind(struct VDir* vd) {
	struct VDirAmigaVolList* vdav = (struct VDirAmigaVolList*) vd;
	vdav->index = 0;
}

static struct VDirEntry* _vdavListNext(struct VDir* vd) {
	struct VDirAmigaVolList* vdav = (struct VDirAmigaVolList*) vd;
	if (vdav->index >= vdav->count) {
		return 0;
	}
	strncpy(vdav->vde.name, vdav->names[vdav->index], sizeof(vdav->vde.name) - 1);
	vdav->vde.name[sizeof(vdav->vde.name) - 1] = '\0';
	++vdav->index;
	return &vdav->vde.d;
}

static struct VFile* _vdavOpenFile(struct VDir* vd, const char* path, int mode) {
	UNUSED(vd);
	return VFileOpen(path, mode);
}

static struct VDir* _vdavOpenDir(struct VDir* vd, const char* path) {
	UNUSED(vd);
	return VDirOpen(path);
}

static bool _vdavDeleteFile(struct VDir* vd, const char* path) {
	UNUSED(vd);
	UNUSED(path);
	return false;
}

static const char* _vdaveName(struct VDirEntry* vde) {
	return ((struct VDirEntryAmigaVol*) vde)->name;
}

static enum VFSType _vdaveType(struct VDirEntry* vde) {
	UNUSED(vde);
	return VFS_DIRECTORY;
}

/* Snapshots the volume list once at open time (copying each name out of
 * the BCPL BSTR the dos.library list itself owns) rather than holding
 * LockDosList() across the VDir's whole lifetime -- listNext() just
 * walks our own copy, so a volume mounted/removed mid-browse can't
 * invalidate a pointer we're still iterating. */
static struct VDir* _amigaVolumeList(void) {
	struct VDirAmigaVolList* vd = malloc(sizeof(struct VDirAmigaVolList));
	if (!vd) {
		return 0;
	}
	vd->names = 0;
	vd->count = 0;
	vd->index = 0;

	size_t capacity = 0;
	struct DosList* dl = LockDosList(LDF_VOLUMES | LDF_READ);
	while ((dl = NextDosEntry(dl, LDF_VOLUMES))) {
		if (vd->count == capacity) {
			capacity = capacity ? capacity * 2 : 8;
			char** grown = realloc(vd->names, capacity * sizeof(char*));
			if (!grown) {
				break;
			}
			vd->names = grown;
		}
		const UBYTE* bstr = (const UBYTE*) BADDR(dl->dol_Name);
		UBYTE nameLen = bstr[0];
		char* name = malloc((size_t) nameLen + 2);
		if (!name) {
			continue;
		}
		memcpy(name, bstr + 1, nameLen);
		name[nameLen] = ':';
		name[nameLen + 1] = '\0';
		vd->names[vd->count++] = name;
	}
	UnLockDosList(LDF_VOLUMES | LDF_READ);

	vd->d.close = _vdavClose;
	vd->d.rewind = _vdavRewind;
	vd->d.listNext = _vdavListNext;
	vd->d.openFile = _vdavOpenFile;
	vd->d.openDir = _vdavOpenDir;
	vd->d.deleteFile = _vdavDeleteFile;
	vd->vde.d.name = _vdaveName;
	vd->vde.d.type = _vdaveType;
	vd->vde.name[0] = '\0';
	return &vd->d;
}
#endif

static bool _vdClose(struct VDir* vd);
static void _vdRewind(struct VDir* vd);
static struct VDirEntry* _vdListNext(struct VDir* vd);
static struct VFile* _vdOpenFile(struct VDir* vd, const char* path, int mode);
static struct VDir* _vdOpenDir(struct VDir* vd, const char* path);
static bool _vdDeleteFile(struct VDir* vd, const char* path);

static const char* _vdeName(struct VDirEntry* vde);
static enum VFSType _vdeType(struct VDirEntry* vde);

struct VDirDE;
struct VDirEntryDE {
	struct VDirEntry d;
	struct VDirDE* p;
	struct dirent* ent;
};

struct VDirDE {
	struct VDir d;
	DIR* de;
	struct VDirEntryDE vde;
	char* path;
};

struct VDir* VDirOpen(const char* path) {
#ifdef __wii__
	if (!path || !path[0]) {
		return VDeviceList();
	}
#endif
#ifdef __amigaos__
	if (!path || !path[0]) {
		return _amigaVolumeList();
	}
#endif
	DIR* de = opendir(path);
	if (!de) {
		return 0;
	}

	struct VDirDE* vd = malloc(sizeof(struct VDirDE));
	if (!vd) {
		closedir(de);
		return 0;
	}

	vd->d.close = _vdClose;
	vd->d.rewind = _vdRewind;
	vd->d.listNext = _vdListNext;
	vd->d.openFile = _vdOpenFile;
	vd->d.openDir = _vdOpenDir;
	vd->d.deleteFile = _vdDeleteFile;
	vd->path = strdup(path);
	vd->de = de;

	vd->vde.d.name = _vdeName;
	vd->vde.d.type = _vdeType;
	vd->vde.p = vd;

	return &vd->d;
}

bool _vdClose(struct VDir* vd) {
	struct VDirDE* vdde = (struct VDirDE*) vd;
	if (closedir(vdde->de) < 0) {
		return false;
	}
	free(vdde->path);
	free(vdde);
	return true;
}

void _vdRewind(struct VDir* vd) {
	struct VDirDE* vdde = (struct VDirDE*) vd;
	rewinddir(vdde->de);
}

struct VDirEntry* _vdListNext(struct VDir* vd) {
	struct VDirDE* vdde = (struct VDirDE*) vd;
	vdde->vde.ent = readdir(vdde->de);
	if (vdde->vde.ent) {
		return &vdde->vde.d;
	}

	return 0;
}

struct VFile* _vdOpenFile(struct VDir* vd, const char* path, int mode) {
	struct VDirDE* vdde = (struct VDirDE*) vd;
	if (!path) {
		return 0;
	}
	const char* dir = vdde->path;
	char* combined = malloc(sizeof(char) * (strlen(path) + strlen(dir) + 2));
	sprintf(combined, "%s%s%s", dir, PATH_SEP, path);

	struct VFile* file = VFileOpen(combined, mode);
	free(combined);
	return file;
}

struct VDir* _vdOpenDir(struct VDir* vd, const char* path) {
	struct VDirDE* vdde = (struct VDirDE*) vd;
	if (!path) {
		return 0;
	}
	const char* dir = vdde->path;
	char* combined = malloc(sizeof(char) * (strlen(path) + strlen(dir) + 2));
	sprintf(combined, "%s%s%s", dir, PATH_SEP, path);

	struct VDir* vd2 = VDirOpen(combined);
	if (!vd2) {
		vd2 = VDirOpenArchive(combined);
	}
	free(combined);
	return vd2;
}

bool _vdDeleteFile(struct VDir* vd, const char* path) {
	struct VDirDE* vdde = (struct VDirDE*) vd;
	if (!path) {
		return false;
	}
	const char* dir = vdde->path;
	char* combined = malloc(sizeof(char) * (strlen(path) + strlen(dir) + 2));
	sprintf(combined, "%s%s%s", dir, PATH_SEP, path);

	bool ret = !remove(combined);
	free(combined);
	return ret;
}

const char* _vdeName(struct VDirEntry* vde) {
	struct VDirEntryDE* vdede = (struct VDirEntryDE*) vde;
	if (vdede->ent) {
		return vdede->ent->d_name;
	}
	return 0;
}

static enum VFSType _vdeType(struct VDirEntry* vde) {
	struct VDirEntryDE* vdede = (struct VDirEntryDE*) vde;
#if !defined(WIN32) && !defined(__HAIKU__)
	if (vdede->ent->d_type == DT_DIR) {
		return VFS_DIRECTORY;
	} else if (vdede->ent->d_type == DT_REG) {
		return VFS_FILE;
	}
#endif
	const char* dir = vdede->p->path;
	char* combined = malloc(sizeof(char) * (strlen(vdede->ent->d_name) + strlen(dir) + 2));
	sprintf(combined, "%s%s%s", dir, PATH_SEP, vdede->ent->d_name);
	struct stat sb;
	stat(combined, &sb);
	free(combined);

	if (S_ISDIR(sb.st_mode)) {
		return VFS_DIRECTORY;
	}
	return VFS_FILE;
}

bool VDirCreate(const char* path) {
	return mkdir(path, 0777) == 0 || errno == EEXIST;
}
