/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba-util/gui/file-select.h>

#include <mgba-util/gui/font.h>
#include <mgba-util/gui/menu.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#include <stdlib.h>

/* A trailing ':' is a complete separator on its own only for an
 * AmigaDOS volume root (e.g. "SYS:", "MGBA:" -- see vfs-dirent.c's
 * __amigaos__ volume-list branch); a bare Windows drive-letter root
 * ("C:") means something different from "C:/" (drive-relative vs.
 * absolute), so this must not also fire there. Defined to a character
 * no real path ever ends in on any other platform, so the two `== `
 * checks below stay a single unconditional expression rather than a
 * platform #ifdef in the middle of each one. */
#ifdef __amigaos__
#define VOLUME_ROOT_SEP ':'
#else
#define VOLUME_ROOT_SEP '\0'
#endif

#define ITERATION_SIZE 5
#define SCANNING_THRESHOLD_1 50
#ifdef __3DS__
// 3DS is slooooow at opening files
#define SCANNING_THRESHOLD_2 10
#else
#define SCANNING_THRESHOLD_2 50
#endif

static void _cleanFiles(struct GUIMenuItemList* currentFiles) {
	size_t size = GUIMenuItemListSize(currentFiles);
	size_t i;
	for (i = 1; i < size; ++i) {
		free((char*) GUIMenuItemListGetPointer(currentFiles, i)->title);
	}
	GUIMenuItemListClear(currentFiles);
}

static void _upDirectory(char* currentPath) {
	char* end = strrchr(currentPath, '/');
	if (!end) {
		currentPath[0] = '\0';
		return;
	}
	if (!end[1]) {
		// Trailing slash
		end[0] = '\0';
		return _upDirectory(currentPath);
	}
	end[1] = '\0';
}

static int _strpcmp(const void* a, const void* b) {
	return strcasecmp(((const struct GUIMenuItem*) a)->title, ((const struct GUIMenuItem*) b)->title);
}

static bool _refreshDirectory(struct GUIParams* params, const char* currentPath, struct GUIMenuItemList* currentFiles, bool (*filterName)(const char* name), bool (*filterContents)(struct VFile*), const char* preselect) {
	_cleanFiles(currentFiles);

	struct VDir* dir = VDirOpen(currentPath);
	if (!dir) {
		return false;
	}
	*GUIMenuItemListAppend(currentFiles) = (struct GUIMenuItem) { .title = "(Up)" };
	size_t i = 0;
	size_t items = 0;
	struct VDirEntry* de;
	while ((de = dir->listNext(dir))) {
		++i;
		if (!(i % SCANNING_THRESHOLD_1)) {
			uint32_t input = 0;
			GUIPollInput(params, &input, 0);
			if (input & (1 << GUI_INPUT_CANCEL)) {
				dir->close(dir);
				return false;
			}

			params->drawStart();
			if (params->guiPrepare) {
				params->guiPrepare();
			}
			GUIFontPrintf(params->font, 0, GUIFontHeight(params->font), GUI_ALIGN_LEFT, 0xFFFFFFFF, "(scanning for items: %"PRIz"u)", i);
			GUIFontPrintf(params->font, 0, GUIFontHeight(params->font) * 2, GUI_ALIGN_LEFT, 0xFFFFFFFF, "%s", currentPath);
			if (params->guiFinish) {
				params->guiFinish();
			}
			params->drawEnd();
		}
		const char* name = de->name(de);
		if (name[0] == '.') {
			continue;
		}
		if (de->type(de) == VFS_DIRECTORY) {
			size_t nameLen = strlen(name);
			/* A name already ending in its own separator (an AmigaDOS
			 * volume entry's own trailing ':', e.g. "MGBA:" -- see
			 * vfs-dirent.c's __amigaos__ volume-list branch) is already
			 * a complete, directly-openable path on its own; appending
			 * PATH_SEP too would produce "MGBA:/", which, unlike a
			 * plain directory name's cosmetic "Name/", AmigaDOS can't
			 * actually open (there's no name between the ':' and the
			 * '/' for it to resolve). */
			bool alreadyTerminated = nameLen && (name[nameLen - 1] == *PATH_SEP || name[nameLen - 1] == VOLUME_ROOT_SEP);
			size_t len = nameLen + (alreadyTerminated ? 1 : 2);
			char* n2 = malloc(len);
			snprintf(n2, len, alreadyTerminated ? "%s" : "%s/", name);
			name = n2;
		} else if (filterName && !filterName(name)) {
			continue;
		} else {
			name = strdup(name);
		}
		*GUIMenuItemListAppend(currentFiles) = (struct GUIMenuItem) { .title = name, .data = GUI_V_U(de->type(de)) };
		++items;
	}
	qsort(GUIMenuItemListGetPointer(currentFiles, 1), GUIMenuItemListSize(currentFiles) - 1, sizeof(struct GUIMenuItem), _strpcmp);
	if (preselect || filterContents) {
		i = 0;
		size_t item = 0;
		while (item < GUIMenuItemListSize(currentFiles)) {
			++i;
			// If we're not filtering the contents, this loop is fast, so there's no need to show updates
			if (filterContents && !(i % SCANNING_THRESHOLD_2)) {
				uint32_t input = 0;
				GUIPollInput(params, &input, 0);
				if (input & (1 << GUI_INPUT_CANCEL)) {
					dir->close(dir);
					return false;
				}

				params->drawStart();
				if (params->guiPrepare) {
					params->guiPrepare();
				}
				GUIFontPrintf(params->font, 0, GUIFontHeight(params->font), GUI_ALIGN_LEFT, 0xFFFFFFFF, "(scanning item %"PRIz"u of %"PRIz"u)", i, items);
				GUIFontPrintf(params->font, 0, GUIFontHeight(params->font) * 2, GUI_ALIGN_LEFT, 0xFFFFFFFF, "%s", currentPath);
				if (params->guiFinish) {
					params->guiFinish();
				}
				params->drawEnd();
			}
			struct GUIMenuItem* testItem = GUIMenuItemListGetPointer(currentFiles, item);
			if (!GUIVariantCompareUInt(testItem->data, VFS_FILE)) {
				++item;
				continue;
			}
			bool failed = false;
			if (filterContents) {
				struct VFile* vf = dir->openFile(dir, testItem->title, O_RDONLY);
				if (!vf) {
					failed = true;
				} else {
					if (!filterContents(vf)) {
						failed = true;
					}
					vf->close(vf);
				}
			}

			if (failed) {
				free((char*) testItem->title);
				GUIMenuItemListShift(currentFiles, item, 1);
			} else {
				if (preselect && strncmp(testItem->title, preselect, PATH_MAX) == 0) {
					params->fileIndex = item;
				}
				++item;
			}
		}
	}
	dir->close(dir);

	return true;
}

bool GUISelectFile(struct GUIParams* params, char* outPath, size_t outLen, bool (*filterName)(const char* name), bool (*filterContents)(struct VFile*), const char* preselect) {
	struct GUIMenu menu = {
		.title = "Select file",
		.subtitle = params->currentPath,
	};
	GUIMenuItemListInit(&menu.items, 0);
	while (true) {
		if (_refreshDirectory(params, params->currentPath, &menu.items, filterName, filterContents, preselect)) {
			break;
		}
		if (strncmp(params->currentPath, params->basePath, PATH_MAX) == 0 || !params->currentPath[0]) {
			mLOG(GUI_MENU, ERROR, "Failed to load base directory");
			return false;
		}
		_upDirectory(params->currentPath);
	}
	menu.index = params->fileIndex;

	while (true) {
		struct GUIMenuItem* item;
		enum GUIMenuExitReason reason = GUIShowMenu(params, &menu, &item);
		params->fileIndex = menu.index;
		if (reason == GUI_MENU_EXIT_CANCEL) {
			break;
		}
		if (reason == GUI_MENU_EXIT_ACCEPT) {
			if (params->fileIndex == 0) {
				if (strncmp(params->currentPath, params->basePath, PATH_MAX) == 0) {
					continue;
				}
				_upDirectory(params->currentPath);
				if (!_refreshDirectory(params, params->currentPath, &menu.items, filterName, filterContents, NULL)) {
					break;
				}
			} else {
				size_t len = strlen(params->currentPath);
				const char* sep = PATH_SEP;
				/* A trailing ':' is already a complete separator on its
				 * own for an AmigaDOS volume root (e.g. "SYS:") --
				 * joining with PATH_SEP too would insert a bogus extra
				 * slash right after it ("SYS:/Name"), which AmigaDOS's
				 * own path parser doesn't accept the way POSIX tolerates
				 * a doubled "/". VOLUME_ROOT_SEP (this file's own,
				 * Amiga-only) is deliberately NOT ':' on other platforms
				 * -- a bare Windows drive-letter root ("C:") means
				 * something different from "C:/" (drive-relative vs.
				 * absolute), so this must not also fire there. Every
				 * directory entry's own title already carries ITS OWN
				 * trailing PATH_SEP too (see the VFS_DIRECTORY branch
				 * above), so the join only ever needs at most one
				 * separator in between, never both. */
				if (!len || params->currentPath[len - 1] == *sep || params->currentPath[len - 1] == VOLUME_ROOT_SEP) {
					sep = "";
				}
				snprintf(outPath, outLen, "%s%s%s", params->currentPath, sep, item->title);

				struct GUIMenuItemList newFiles;
				GUIMenuItemListInit(&newFiles, 0);
				if (!_refreshDirectory(params, outPath, &newFiles, filterName, filterContents, NULL)) {
					_cleanFiles(&newFiles);
					GUIMenuItemListDeinit(&newFiles);
					_cleanFiles(&menu.items);
					GUIMenuItemListDeinit(&menu.items);
					return true;
				} else {
					_cleanFiles(&menu.items);
					GUIMenuItemListDeinit(&menu.items);
					menu.items = newFiles;
					strlcpy(params->currentPath, outPath, PATH_MAX);
				}
			}
			params->fileIndex = 0;
			menu.index = 0;
		}
		if (reason == GUI_MENU_EXIT_BACK) {
			if (strncmp(params->currentPath, params->basePath, PATH_MAX) == 0) {
				break;
			}
			_upDirectory(params->currentPath);
			if (!_refreshDirectory(params, params->currentPath, &menu.items, filterName, filterContents, NULL)) {
				break;
			}
			params->fileIndex = 0;
			menu.index = 0;
		}
	}

	_cleanFiles(&menu.items);
	GUIMenuItemListDeinit(&menu.items);
	return false;
}
