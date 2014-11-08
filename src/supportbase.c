#include "include/opl.h"
#include "include/lang.h"
#include "include/util.h"
#include "include/iosupport.h"
#include "include/system.h"
#include "include/supportbase.h"
#include "include/ioman.h"
#include "modules/iopcore/common/cdvd_config.h"

/// internal linked list used to populate the list from directory listing
struct game_list_t {
	base_game_info_t gameinfo;
	struct game_list_t *next;
};

int sbIsSameSize(const char* prefix, int prevSize) {
	int size = -1;
	char path[256];
	snprintf(path, sizeof(path), "%sul.cfg", prefix);

	int fd = openFile(path, O_RDONLY);
	if (fd >= 0) {
		size = getFileSize(fd);
		fioClose(fd);
	}

	return size == prevSize;
}

static int isValidIsoName(char *name)
{
	// SCUS_XXX.XX.ABCDEFGHIJKLMNOP.iso

	// Minimum is 17 char, GameID (11) + "." (1) + filename (1 min.) + ".iso" (4)
	int size = strlen(name);
	if ((size >= 17) && (name[4] == '_') && (name[8] == '.') && (name[11] == '.') && (stricmp(&name[size - 4], ".iso") == 0)) {
		size -= 16;
		if (size <= ISO_GAME_NAME_MAX)
			return size;
	}

	return 0;
}

static int scanForISO(char* path, char type, struct game_list_t** glist) {
	int fd, size, count = 0;
	fio_dirent_t record;

	if ((fd = fioDopen(path)) > 0) {
		while (fioDread(fd, &record) > 0) {
			if ((size = isValidIsoName(record.name)) > 0) {
				struct game_list_t *next = (struct game_list_t*)malloc(sizeof(struct game_list_t));

				next->next = *glist;
				*glist = next;

				base_game_info_t *game = &(*glist)->gameinfo;

				strncpy(game->name, &record.name[GAME_STARTUP_MAX], size);
				game->name[size] = '\0';
				strncpy(game->startup, record.name, GAME_STARTUP_MAX - 1);
				game->startup[GAME_STARTUP_MAX - 1] = '\0';
				strncpy(game->extension, &record.name[GAME_STARTUP_MAX + size], sizeof(game->extension));
				game->extension[sizeof(game->extension)-1] = '\0';
				game->parts = 0x01;
				game->media = type;
				game->isISO = 1;
				game->sizeMB = (record.stat.size >> 20) | (record.stat.hisize << 12);

				count++;
			}
		}
		fioDclose(fd);
	}else{
		count = fd;
	}

	return count;
}

int sbReadList(base_game_info_t **list, const char* prefix, int *fsize, int* gamecount) {
	int fd, size, id = 0, result;
	int count;
	char path[256];

	free(*list);
	*list = NULL;
	*fsize = -1;
	*gamecount = 0;

	// temporary storage for the game names
	struct game_list_t *dlist_head = NULL;

	// count iso games in "cd" directory
	snprintf(path, sizeof(path), "%sCD", prefix);
	count = scanForISO(path, 0x12, &dlist_head);

	// count iso games in "dvd" directory
	snprintf(path, sizeof(path), "%sDVD", prefix);
	if((result = scanForISO(path, 0x14, &dlist_head)) >= 0){
		count = count<0?result:count+result;
	}

	// count and process games in ul.cfg
	snprintf(path, sizeof(path), "%sul.cfg", prefix);
	fd = openFile(path, O_RDONLY);
	if(fd >= 0) {
		char buffer[0x040];

		if(count < 0) count = 0;
		size = getFileSize(fd);
		*fsize = size;
		count += size / 0x040;

		if (count > 0) {
			*list = (base_game_info_t*)malloc(sizeof(base_game_info_t) * count);

			while (size > 0) {
				fioRead(fd, buffer, 0x40);

				base_game_info_t *g = &(*list)[id++];

				// to ensure no leaks happen, we copy manually and pad the strings
				memcpy(g->name, buffer, UL_GAME_NAME_MAX);
				g->name[UL_GAME_NAME_MAX] = '\0';
				memcpy(g->startup, &buffer[UL_GAME_NAME_MAX + 3], GAME_STARTUP_MAX);
				g->startup[GAME_STARTUP_MAX] = '\0';
				g->extension[0] = '\0';
				memcpy(&g->parts, &buffer[47], 1);
				memcpy(&g->media, &buffer[48], 1);
				g->isISO = 0;
				g->sizeMB = -1;
				size -= 0x40;
			}
		}
		fioClose(fd);
	}
	else if (count > 0){
		*list = (base_game_info_t*)malloc(sizeof(base_game_info_t) * count);
	}

	// copy the dlist into the list
	while ((id < count) && dlist_head) {
		// copy one game, advance
		struct game_list_t *cur = dlist_head;
		dlist_head = dlist_head->next;

		memcpy(&(*list)[id++], &cur->gameinfo, sizeof(base_game_info_t));
		free(cur);
	}

	if(count > 0) *gamecount = count;

	return count;
}

int sbPrepare(base_game_info_t* game, config_set_t* configSet, int size_cdvdman, void** cdvdman_irx, int* patchindex) {
	int i;
	static const struct cdvdman_settings_common cdvdman_settings_common_sample={
		0x69, 0x69,
		0x1234,
		0x39393939,
		"B00BS"
	};
	struct cdvdman_settings_common *settings;

	unsigned int compatmask = 0;
	configGetInt(configSet, CONFIG_ITEM_COMPAT, &compatmask);

	char gameid[5];
	configGetDiscIDBinary(configSet, gameid);

	for (i = 0, settings = NULL; i < size_cdvdman; i+=4) {
		if (!memcmp((void*)((u8*)cdvdman_irx + i), &cdvdman_settings_common_sample, sizeof(cdvdman_settings_common_sample))) {
			settings=(struct cdvdman_settings_common*)((u8*)cdvdman_irx + i);
			break;
		}
	}
	if (settings == NULL) return -1;

	if(game != NULL){
		settings->NumParts = game->parts;
		settings->media = game->media;
	}
	settings->flags = 0;

	if (compatmask & COMPAT_MODE_2) {
		settings->flags |= IOPCORE_COMPAT_ALT_READ;
	}

	if (compatmask & COMPAT_MODE_4) {
		settings->flags |= IOPCORE_COMPAT_0_PSS;
	}

	if (compatmask & COMPAT_MODE_5) {
		settings->flags |= IOPCORE_COMPAT_DISABLE_DVDDL;
	}

	if (compatmask & COMPAT_MODE_6) {
		settings->flags |= IOPCORE_ENABLE_POFF;
	}
	
	gEnableGSM = 0;
	if (configGetInt(configSet, CONFIG_ITEM_ENABLEGSM, &gEnableGSM) && gEnableGSM) {
	//Load the rest of the per-game GSM configuration, only if GSM is enabled.
		configGetInt(configSet, CONFIG_ITEM_GSMVMODE, &gGSMVMode);
		configGetInt(configSet, CONFIG_ITEM_GSMXOFFSET, &gGSMXOffset);
		configGetInt(configSet, CONFIG_ITEM_GSMYOFFSET, &gGSMYOffset);
		configGetInt(configSet, CONFIG_ITEM_GSMSKIPVIDEOS, &gGSMSkipVideos);
	
	}

	// patch cdvdman timer
	int timer = 0;
	u32 cdvdmanTimer = 0;
	if (configGetInt(configSet, CONFIG_ITEM_CDVDMAN_TIMER, &timer)) {
		cdvdmanTimer = timer * 250;
	}
	settings->cb_timer = cdvdmanTimer;

	*patchindex = i;

	// game id
	memcpy(settings->DiscID, gameid, 5);

	return compatmask;
}

static void sbRebuildULCfg(base_game_info_t **list, const char* prefix, int gamecount, int excludeID) {
	char path[256];
	snprintf(path, sizeof(path), "%sul.cfg", prefix);

	file_buffer_t* fileBuffer = openFileBuffer(path, O_WRONLY | O_CREAT | O_TRUNC, 0, 4096);
	if (fileBuffer) {
		int i;
		char buffer[0x40];
		base_game_info_t* game;

		memset(buffer, 0, 0x40);
		buffer[32] = 0x75; // u
		buffer[33] = 0x6C; // l
		buffer[34] = 0x2E; // .
		buffer[53] = 0x08; // just to be compatible with original ul.cfg

		for (i = 0; i < gamecount; i++) {
			game = &(*list)[i];

			if (!game->isISO  && (i != excludeID)) {
				memset(buffer, 0, UL_GAME_NAME_MAX);
				memset(&buffer[UL_GAME_NAME_MAX + 3], 0, GAME_STARTUP_MAX);

				memcpy(buffer, game->name, UL_GAME_NAME_MAX);
				memcpy(&buffer[UL_GAME_NAME_MAX + 3], game->startup, GAME_STARTUP_MAX);
				buffer[47] = game->parts;
				buffer[48] = game->media;

				writeFileBuffer(fileBuffer, buffer, 0x40);
			}
		}

		closeFileBuffer(fileBuffer);
	}
}

void sbDelete(base_game_info_t **list, const char* prefix, const char* sep, int gamecount, int id) {
	char path[256];
	base_game_info_t* game = &(*list)[id];

	if (game->isISO) {
		if (game->media == 0x12)
			snprintf(path, sizeof(path), "%sCD%s%s.%s%s", prefix, sep, game->startup, game->name, game->extension);
		else
			snprintf(path, sizeof(path), "%sDVD%s%s.%s%s", prefix, sep, game->startup, game->name, game->extension);
		fileXioRemove(path);
	} else {
		char *pathStr = "%sul.%08X.%s.%02x";
		unsigned int crc = USBA_crc32(game->name);
		int i = 0;
		do {
			snprintf(path, sizeof(path), pathStr, prefix, crc, game->startup, i++);
			fileXioRemove(path);
		} while(i < game->parts);

		sbRebuildULCfg(list, prefix, gamecount, id);
	}
}

void sbRename(base_game_info_t **list, const char* prefix, const char* sep, int gamecount, int id, char* newname) {
	char oldpath[256], newpath[256];
	base_game_info_t* game = &(*list)[id];

	if (game->isISO) {
		if (game->media == 0x12) {
			snprintf(oldpath, sizeof(oldpath), "%sCD%s%s.%s%s", prefix, sep, game->startup, game->name, game->extension);
			snprintf(newpath, sizeof(newpath), "%sCD%s%s.%s%s", prefix, sep, game->startup, newname, game->extension);
		} else {
			snprintf(oldpath, sizeof(oldpath), "%sDVD%s%s.%s%s", prefix, sep, game->startup, game->name, game->extension);
			snprintf(newpath, sizeof(newpath), "%sDVD%s%s.%s%s", prefix, sep, game->startup, newname, game->extension);
		}
		fileXioRename(oldpath, newpath);
	} else {
		memset(game->name, 0, UL_GAME_NAME_MAX);
		memcpy(game->name, newname, UL_GAME_NAME_MAX);

		char *pathStr = "%sul.%08X.%s.%02x";
		unsigned int oldcrc = USBA_crc32(game->name);
		unsigned int newcrc = USBA_crc32(newname);
		int i = 0;
		do {
			snprintf(oldpath, sizeof(oldpath), pathStr, prefix, oldcrc, game->startup, i);
			snprintf(newpath, sizeof(newpath), pathStr, prefix, newcrc, game->startup, i++);
			fileXioRename(oldpath, newpath);
		} while(i < game->parts);

		sbRebuildULCfg(list, prefix, gamecount, -1);
	}
}

config_set_t* sbPopulateConfig(base_game_info_t* game, const char* prefix, const char* sep) {
	char path[256];
	snprintf(path, sizeof(path), "%sCFG%s%s.cfg", prefix, sep, game->startup);
	config_set_t* config = configAlloc(0, NULL, path);
	configRead(config);

	configSetStr(config, CONFIG_ITEM_NAME, game->name);
	if (game->sizeMB != -1)
		configSetInt(config, CONFIG_ITEM_SIZE, game->sizeMB);

	configSetStr(config, CONFIG_ITEM_FORMAT, game->isISO ? "ISO" : "UL");
	configSetStr(config, CONFIG_ITEM_MEDIA, game->media == 0x12 ? "CD" : "DVD");

	configSetStr(config, CONFIG_ITEM_STARTUP, game->startup);

	return config;
}
