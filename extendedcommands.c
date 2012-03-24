#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "../../external/yaffs2/yaffs2/utils/mkyaffs2image.h"
#include "../../external/yaffs2/yaffs2/utils/unyaffs.h"

#include "extendedcommands.h"
#include "nandroid.h"
#include "mounts.h"
#include "flashutils/flashutils.h"
#include "edify/expr.h"
#include <libgen.h>
#include "mtdutils/mtdutils.h"
#include "bmlutils/bmlutils.h"
#include "cutils/android_reboot.h"


int signature_check_enabled = 1;
int script_assert_enabled = 1;
static const char *SDCARD_UPDATE_FILE = "/sdcard/update.zip";

void
toggle_signature_check()
{
    signature_check_enabled = !signature_check_enabled;
    ui_print("signature check: %s\n", signature_check_enabled ? "enabled" : "disabled");
}

void toggle_script_asserts()
{
    script_assert_enabled = !script_assert_enabled;
    ui_print("script asserts: %s\n", script_assert_enabled ? "enabled" : "disabled");
}

int install_zip(const char* packagefilepath)
{
    ui_print("\n-- installing: %s\n", packagefilepath);
    if (device_flash_type() == MTD) {
        set_sdcard_update_bootloader_message();
    }
    int status = install_package(packagefilepath);
    ui_reset_progress();
    if (status != INSTALL_SUCCESS) {
        ui_set_background(BACKGROUND_ICON_ERROR);
        ui_print("installation aborted.\n");
        return 1;
    }
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_print("\ninstall from sdcard complete.\n");
    return 0;
}

char* INSTALL_MENU_ITEMS[] = {  "choose zip from sdcard",
                                "apply /sdcard/update.zip",
                                "toggle signature verification",
                                "toggle script asserts",
                                "choose zip from internal sdcard",
                                NULL };
#define ITEM_CHOOSE_ZIP       0
#define ITEM_APPLY_SDCARD     1
#define ITEM_SIG_CHECK        2
#define ITEM_ASSERTS          3
#define ITEM_CHOOSE_ZIP_INT   4

void show_install_update_menu()
{
    static char* headers[] = {  "apply update from .zip file on SD card",
                                "",
                                NULL
    };
    
    if (volume_for_path("/emmc") == NULL)
        INSTALL_MENU_ITEMS[ITEM_CHOOSE_ZIP_INT] = NULL;
    
    for (;;)
    {
        int chosen_item = get_menu_selection(headers, INSTALL_MENU_ITEMS, 0, 0);
        switch (chosen_item)
        {
            case ITEM_ASSERTS:
                toggle_script_asserts();
                break;
            case ITEM_SIG_CHECK:
                toggle_signature_check();
                break;
            case ITEM_APPLY_SDCARD:
            {
                if (confirm_selection("confirm install?", "yes - install /sdcard/update.zip"))
                    install_zip(SDCARD_UPDATE_FILE);
                break;
            }
            case ITEM_CHOOSE_ZIP:
                show_choose_zip_menu("/sdcard/");
                break;
            case ITEM_CHOOSE_ZIP_INT:
                show_choose_zip_menu("/emmc/");
                break;
            default:
                return;
        }

    }
}

void free_string_array(char** array)
{
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL)
    {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles)
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(directory);

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("Couldn't open directory.\n");
        return NULL;
    }

    int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);

    int isCounting = 1;
    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de=readdir(dir)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;

            // NULL means that we are gathering directories, so skip this
            if (fileExtensionOrDirectory != NULL)
            {
                // make sure that we can have the desired extension (prevent seg fault)
                if (strlen(de->d_name) < extension_length)
                    continue;
                // compare the extension
                if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                    continue;
            }
            else
            {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                stat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }

            if (pass == 0)
            {
                total++;
                continue;
            }

            files[i] = (char*) malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        rewinddir(dir);
        *numFiles = total;
        files = (char**) malloc((total+1)*sizeof(char*));
        files[total]=NULL;
    }

    if(closedir(dir) < 0) {
        LOGE("Failed to close directory.");
    }

    if (total==0) {
        return NULL;
    }

    // sort the result
    if (files != NULL) {
        for (i = 0; i < total; i++) {
            int curMax = -1;
            int j;
            for (j = 0; j < total - i; j++) {
                if (curMax == -1 || strcmp(files[curMax], files[j]) < 0)
                    curMax = j;
            }
            char* temp = files[curMax];
            files[curMax] = files[total - i - 1];
            files[total - i - 1] = temp;
        }
    }

    return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
char* choose_file_menu(const char* directory, const char* fileExtensionOrDirectory, const char* headers[])
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    int dir_len = strlen(directory);

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0)
    {
        ui_print("no files found.\n");
    }
    else
    {
        char** list = (char**) malloc((total + 1) * sizeof(char*));
        list[total] = NULL;


        for (i = 0 ; i < numDirs; i++)
        {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0 ; i < numFiles; i++)
        {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;)
        {
            int chosen_item = get_menu_selection(headers, list, 0, 0);
            if (chosen_item == GO_BACK)
                break;
            static char ret[PATH_MAX];
            if (chosen_item < numDirs)
            {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL)
                {
                    strcpy(ret, subret);
                    return_value = ret;
                    break;
                }
                continue;
            }
            strcpy(ret, files[chosen_item - numDirs]);
            return_value = ret;
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

void show_choose_zip_menu(const char *mount_point)
{
    if (ensure_path_mounted(mount_point) != 0) {
        LOGE ("Can't mount %s\n", mount_point);
        return;
    }

    static char* headers[] = {  "choose a zip to apply",
                                "",
                                NULL
    };

    char* file = choose_file_menu(mount_point, ".zip", headers);
    if (file == NULL)
        return;
    static char* confirm_install  = "confirm install?";
    static char confirm[PATH_MAX];
    sprintf(confirm, "yes - install %s", basename(file));
    if (confirm_selection(confirm_install, confirm))
        install_zip(file);
}

void show_nandroid_restore_menu(const char* path)
{
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return;
    }

    static char* headers[] = {  "choose an image to restore",
                                "",
                                NULL
    };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, headers);
    if (file == NULL)
        return;

    if (confirm_selection("confirm restore?", "yes - restore"))
        nandroid_restore(file, 1, 1, 1, 1, 1, 0);
}

int confirm_selection(const char* title, const char* confirm)
{
    struct stat info;
    if (0 == stat("/sdcard/clockworkmod/.no_confirm", &info))
        return 1;

    char* confirm_headers[]  = {  title, "  THIS CAN NOT BE UNDONE.", "", NULL };
	if (0 == stat("/sdcard/clockworkmod/.one_confirm", &info)) {
		char* items[] = { "no",
						confirm, //" Yes -- wipe partition",   // [1]
						NULL };
		int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
		return chosen_item == 1;
	}
	else {
		char* items[] = { "no",
						"no",
						"no",
						"no",
						"no",
						"no",
						"no",
						confirm, //" Yes -- wipe partition",   // [7]
						"no",
						"no",
						"no",
						NULL };
                /*char* items[] = { "No",
                                  "Yes",
                                  NULL };*/
		int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
		return chosen_item == 7;
	}
	}

#define MKE2FS_BIN      "/sbin/mke2fs"
#define TUNE2FS_BIN     "/sbin/tune2fs"
#define E2FSCK_BIN      "/sbin/e2fsck"

int format_device(const char *device, const char *path, const char *fs_type) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        // no /sdcard? let's assume /data/media
        if (strstr(path, "/sdcard") == path && is_data_media()) {
            return format_unknown_device(NULL, path, NULL);
        }
        // silent failure for sd-ext
        if (strcmp(path, "/sd-ext") == 0)
            return -1;
        LOGE("unknown volume \"%s\"\n", path);
        return -1;
    }
    if (strstr(path, "/data") == path && volume_for_path("/sdcard") == NULL && is_data_media()) {
        return format_unknown_device(NULL, path, NULL);
    }
    if (strcmp(fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOGE("can't format_volume \"%s\"", path);
        return -1;
    }

    if (strcmp(fs_type, "rfs") == 0) {
        if (ensure_path_unmounted(path) != 0) {
            LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
            return -1;
        }
        if (0 != format_rfs_device(device, path)) {
            LOGE("format_volume: format_rfs_device failed on %s\n", device);
            return -1;
        }
        return 0;
    }
 
    if (strcmp(v->mount_point, path) != 0) {
        return format_unknown_device(v->device, path, NULL);
    }

    if (ensure_path_unmounted(path) != 0) {
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    if (strcmp(fs_type, "yaffs2") == 0 || strcmp(fs_type, "mtd") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(device);
        if (partition == NULL) {
            LOGE("format_volume: no MTD partition \"%s\"\n", device);
            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            LOGW("format_volume: can't open MTD \"%s\"\n", device);
            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
            LOGW("format_volume: can't erase MTD \"%s\"\n", device);
            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
            LOGW("format_volume: can't close MTD \"%s\"\n",device);
            return -1;
        }
        return 0;
    }

    if (strcmp(fs_type, "ext4") == 0) {
        reset_ext4fs_info();
        int result = make_ext4fs(device, NULL, NULL, 0, 0, 0);
        if (result != 0) {
            LOGE("format_volume: make_extf4fs failed on %s\n", device);
            return -1;
        }
        return 0;
    }

    return format_unknown_device(device, path, fs_type);
}

int format_unknown_device(const char *device, const char* path, const char *fs_type)
{
    LOGI("formatting unknown device.\n");

    if (fs_type != NULL && get_flash_type(fs_type) != UNSUPPORTED)
        return erase_raw_partition(fs_type, device);

    // if this is SDEXT:, don't worry about it if it does not exist.
    if (0 == strcmp(path, "/sd-ext"))
    {
        struct stat st;
        Volume *vol = volume_for_path("/sd-ext");
        if (vol == NULL || 0 != stat(vol->device, &st))
        {
            ui_print("no app2sd partition found. skipping format of /sd-ext.\n");
            return 0;
        }
    }

    if (NULL != fs_type) {
        if (strcmp("ext3", fs_type) == 0) {
            LOGI("formatting ext3 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("frror while unmounting %s.\n", path);
                return -12;
            }
            return format_ext3_device(device);
        }

        if (strcmp("ext2", fs_type) == 0) {
            LOGI("formatting ext2 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("error while unmounting %s.\n", path);
                return -12;
            }
            return format_ext2_device(device);
        }
    }

    if (0 != ensure_path_mounted(path))
    {
        ui_print("error mounting %s!\n", path);
        ui_print("skipping format...\n");
        return 0;
    }

    static char tmp[PATH_MAX];
    if (strcmp(path, "/data") == 0) {
        sprintf(tmp, "cd /data ; for f in $(ls -a | grep -v ^media$); do rm -rf $f; done");
        __system(tmp);
    }
    else {
        sprintf(tmp, "rm -rf %s/*", path);
        __system(tmp);
        sprintf(tmp, "rm -rf %s/.*", path);
        __system(tmp);
    }

    ensure_path_unmounted(path);
    return 0;
}

//#define MOUNTABLE_COUNT 5
//#define DEVICE_COUNT 4
//#define MMC_COUNT 2

typedef struct {
    char mount[255];
    char unmount[255];
    Volume* v;
} MountMenuEntry;

typedef struct {
    char txt[255];
    Volume* v;
} FormatMenuEntry;

int is_safe_to_format(char* name)
{
    char str[255];
    char* partition;
    property_get("ro.cwm.forbid_format", str, "/misc,/radio,/bootloader,/recovery,/efs");

    partition = strtok(str, ", ");
    while (partition != NULL) {
        if (strcmp(name, partition) == 0) {
            return 0;
        }
        partition = strtok(NULL, ", ");
    }

    return 1;
}

void show_power_menu()
{
    static char* headers[] = {  "power Menu",
                                "",
                                NULL
    };

    static char* list[] = { "normal reboot",
			    "reboot recovery",
			    "reboot bootloader",
                            "power off",
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
            case 0:
            {
	        ui_print("rebooting...\n");
                android_reboot(ANDROID_RB_RESTART, 0, 0);
                break;
            }
            case 1:
            {
                android_reboot(ANDROID_RB_RESTART2, 0, "recovery");
                break;
            }
            case 2:
            {
                android_reboot(ANDROID_RB_RESTART2, 0, "bootloader");
                break;
            }
            case 3:
            {
	        ui_print("shutting down...\n");
                android_reboot(ANDROID_RB_POWEROFF, 0, 0);
                break;
            }
        }
    }
}

void show_partition_menu()
{
    static char* headers[] = {  "mounts and storage menu",
                                "",
                                NULL
    };

    static MountMenuEntry* mount_menue = NULL;
    static FormatMenuEntry* format_menue = NULL;

    typedef char* string;

    int i, mountable_volumes, formatable_volumes;
    int num_volumes;
    Volume* device_volumes;

    num_volumes = get_num_volumes();
    device_volumes = get_device_volumes();

    string options[255];

    if(!device_volumes)
		return;

		mountable_volumes = 0;
		formatable_volumes = 0;

		mount_menue = malloc(num_volumes * sizeof(MountMenuEntry));
		format_menue = malloc(num_volumes * sizeof(FormatMenuEntry));

		for (i = 0; i < num_volumes; ++i) {
			Volume* v = &device_volumes[i];
			if(strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) != 0 && strcmp("emmc", v->fs_type) != 0 && strcmp("bml", v->fs_type) != 0)
			{
				sprintf(&mount_menue[mountable_volumes].mount, "mount %s", v->mount_point);
				sprintf(&mount_menue[mountable_volumes].unmount, "unmount %s", v->mount_point);
				mount_menue[mountable_volumes].v = &device_volumes[i];
				++mountable_volumes;
				if (is_safe_to_format(v->mount_point)) {
					sprintf(&format_menue[formatable_volumes].txt, "format %s", v->mount_point);
					format_menue[formatable_volumes].v = &device_volumes[i];
					++formatable_volumes;
				}
		    }
		    else if (strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) == 0 && is_safe_to_format(v->mount_point))
		    {
				sprintf(&format_menue[formatable_volumes].txt, "format %s", v->mount_point);
				format_menue[formatable_volumes].v = &device_volumes[i];
				++formatable_volumes;
			}
		}


    static char* confirm_format  = "confirm format?";
    static char* confirm = "yes - format";
    char confirm_string[255];

    for (;;)
    {

		for (i = 0; i < mountable_volumes; i++)
		{
			MountMenuEntry* e = &mount_menue[i];
			Volume* v = e->v;
			if(is_path_mounted(v->mount_point))
				options[i] = e->unmount;
			else
				options[i] = e->mount;
		}

		for (i = 0; i < formatable_volumes; i++)
		{
			FormatMenuEntry* e = &format_menue[i];

			options[mountable_volumes+i] = e->txt;
		}

        options[mountable_volumes+formatable_volumes + 1] = NULL;

        int chosen_item = get_menu_selection(headers, &options, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        if (chosen_item < mountable_volumes)
        {
			MountMenuEntry* e = &mount_menue[chosen_item];
            Volume* v = e->v;

            if (is_path_mounted(v->mount_point))
            {
                if (0 != ensure_path_unmounted(v->mount_point))
                    ui_print("Error unmounting %s!\n", v->mount_point);
            }
            else
            {
                if (0 != ensure_path_mounted(v->mount_point))
                    ui_print("Error mounting %s!\n",  v->mount_point);
            }
        }
        else if (chosen_item < (mountable_volumes + formatable_volumes))
        {
            chosen_item = chosen_item - mountable_volumes;
            FormatMenuEntry* e = &format_menue[chosen_item];
            Volume* v = e->v;

            sprintf(confirm_string, "%s - %s", v->mount_point, confirm_format);

            if (!confirm_selection(confirm_string, confirm))
                continue;
            ui_print("Formatting %s...\n", v->mount_point);
            if (0 != format_volume(v->mount_point))
                ui_print("Error formatting %s!\n", v->mount_point);
            else
                ui_print("Done.\n");
        }
    }

    free(mount_menue);
    free(format_menue);

}

void show_nandroid_advanced_restore_menu(const char* path)
{
    if (ensure_path_mounted(path) != 0) {
        LOGE ("can't mount sdcard\n");
        return;
    }

    static char* advancedheaders[] = {  "choose an image to restore",
                                "",
                                "choose an image to restore",
                                "first. the next menu will",
                                "you more options.",
                                "",
                                NULL
    };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, advancedheaders);
    if (file == NULL)
        return;

    static char* headers[] = {  "nandroid advanced restore",
                                "",
                                NULL
    };

    static char* list[] = { "Restore boot",
                            "Restore system",
                            "Restore data",
                            "Restore cache",
                            "Restore wimax",
                            NULL
    };
    
    if (0 != get_partition_device("wimax", tmp)) {
        // disable wimax restore option
        list[4] = NULL;
    }

    static char* confirm_restore  = "confirm restore?";

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            if (confirm_selection(confirm_restore, "yes - restore boot"))
                nandroid_restore(file, 1, 0, 0, 0, 0, 0);
            break;
        case 1:
            if (confirm_selection(confirm_restore, "yes - restore system"))
                nandroid_restore(file, 0, 1, 0, 0, 0, 0);
            break;
        case 2:
            if (confirm_selection(confirm_restore, "yes - restore data"))
                nandroid_restore(file, 0, 0, 1, 0, 0, 0);
            break;
        case 3:
            if (confirm_selection(confirm_restore, "yes - restore cache"))
                nandroid_restore(file, 0, 0, 0, 1, 0, 0);
            break;
        case 4:
            if (confirm_selection(confirm_restore, "yes - restore wimax"))
                nandroid_restore(file, 0, 0, 0, 0, 0, 1);
            break;
    }
}

void show_nandroid_advanced_backup_menu()
{
    static char* advancedheaders[] = {  "choose a partition to backup",
                                "",
                                "advanced backup only backs up",
                                "one partition at a time.",
                                NULL
    };

    char tmp[PATH_MAX];
    //sprintf(tmp, "%s/clockworkmod/backup/", path);
    //char* file = choose_file_menu(tmp, NULL, advancedheaders);
    //if (file == NULL)
    //    return;

    static char* headers[] = {  "nandroid advanced backup",
                                "",
                                NULL
    };

    static char* list[] = { "backup boot",
                            "backup system",
                            "backup data",
                            "backup cache",
                            "backup wimax",
                            NULL
    };
    
    //if (0 != get_partition_device("wimax", tmp)) {
    //    // disable wimax restore option
    //    list[5] = NULL;
    //}

    static char* confirm_restore  = "confirm backup?";

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            if (confirm_selection(confirm_restore, "yes - backup boot"))
                //nandroid_restore(file, 1, 0, 0, 0, 0, 0);
            break;
        case 1:
            if (confirm_selection(confirm_restore, "yes - backup system"))
                //nandroid_restore(file, 0, 1, 0, 0, 0, 0);
            break;
        case 2:
            if (confirm_selection(confirm_restore, "yes - backup data"))
                //nandroid_restore(file, 0, 0, 1, 0, 0, 0);
            break;
        case 3:
            if (confirm_selection(confirm_restore, "yes - backup cache"))
                //nandroid_restore(file, 0, 0, 0, 1, 0, 0);
            break;
        case 4:
            if (confirm_selection(confirm_restore, "yes - backup wimax"))
                //nandroid_restore(file, 0, 0, 0, 0, 0, 1);
            break;
    }
}

void show_nandroid_menu()
{
    static char* headers[] = {  "Nandroid",
                                "",
                                NULL
    };

    static char* list[] = { "backup",
                            "restore",
                            "advanced restore",
                            "backup to internal sdcard",
                            "restore from internal sdcard",
                            "advanced restore from internal sdcard",
                            NULL
    };

    if (volume_for_path("/emmc") == NULL || volume_for_path("/sdcard") == NULL && is_data_media())
        list[4] = NULL;

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            {
                char backup_path[PATH_MAX];
                time_t t = time(NULL);
                struct tm *tmp = localtime(&t);
                if (tmp == NULL)
                {
                    struct timeval tp;
                    gettimeofday(&tp, NULL);
                    sprintf(backup_path, "/sdcard/clockworkmod/backup/%d", tp.tv_sec);
                }
                else
                {
                    strftime(backup_path, sizeof(backup_path), "/sdcard/clockworkmod/backup/%F.%H.%M.%S", tmp);
                }
                nandroid_backup(backup_path);
            }
            break;
        case 1:
            show_nandroid_restore_menu("/sdcard");
            break;
        case 2:
            show_nandroid_advanced_restore_menu("/sdcard");
            break;
        case 3:
            {
                char backup_path[PATH_MAX];
                time_t t = time(NULL);
                struct tm *tmp = localtime(&t);
                if (tmp == NULL)
                {
                    struct timeval tp;
                    gettimeofday(&tp, NULL);
                    sprintf(backup_path, "/emmc/clockworkmod/backup/%d", tp.tv_sec);
                }
                else
                {
                    strftime(backup_path, sizeof(backup_path), "/emmc/clockworkmod/backup/%F.%H.%M.%S", tmp);
                }
                nandroid_backup(backup_path);
            }
            break;
        case 4:
            show_nandroid_restore_menu("/emmc");
            break;
        case 5:
            show_nandroid_advanced_restore_menu("/emmc");
            break;
    }
}

void wipe_battery_stats()
{
    ensure_path_mounted("/data");
    remove("/data/system/batterystats.bin");
    ensure_path_unmounted("/data");
    ui_print("Battery Stats wiped.\n");
}

void show_advanced_menu()
{
    static char* headers[] = {  "advanced and debugging menu",
                                "",
                                NULL
    };

    static char* list[] = { "wipe dalvik cache",
                            "wipe battery stats",
			    "view battery stats",
                            "report error",
                            "key test",
                            "show log",
                            "fix permissions",
			    "copy recovery.log to /sdcard",
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
            case 0:
            {
                if (0 != ensure_path_mounted("/data"))
                    break;
                ensure_path_mounted("/sd-ext");
                ensure_path_mounted("/cache");
                if (confirm_selection( "Confirm wipe?", "Yes - Wipe Dalvik Cache")) {
                    __system("rm -r /data/dalvik-cache");
                    __system("rm -r /cache/dalvik-cache");
                    __system("rm -r /sd-ext/dalvik-cache");
                    ui_print("dalvik cache wiped.\n");
                }
                ensure_path_unmounted("/data");
                break;
            }
            case 1:
            {
                if (confirm_selection( "confirm wipe?", "yes - wipe battery stats"))
                    wipe_battery_stats();
                break;
            }
            case 2:
                show_battstat();
                break;
            case 3:
                handle_failure(1);
                break;
            case 4:
            {
                ui_print("outputting key codes.\n");
                ui_print("go back to end debugging.\n");
                int key;
                int action;
                do
                {
                    key = ui_wait_key();
                    action = device_handle_key(key, 1);
                    ui_print("key: %d\n", key);
                }
                while (action != GO_BACK);
                break;
            }
            case 5:
            {
                ui_printlogtail(12);
                break;
            }
            case 6:
            {
                ensure_path_mounted("/system");
                ensure_path_mounted("/data");
                ui_print("fixing permissions...\n");
                __system("fix_permissions");
                ui_print("done!\n");
                break;
            }
            case 7:
	    {
                ensure_path_mounted("/sdcard");
            	__system("cp /tmp/recovery.log /sdcard");
				sync();
                ui_print("copied recovery log to /sdcard.\n");
            	break;
	    }
        }
    }
}

void write_fstab_root(char *path, FILE *file)
{
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGW("unable to get recovery.fstab info for %s during fstab generation!\n", path);
        return;
    }

    char device[200];
    if (vol->device[0] != '/')
        get_partition_device(vol->device, device);
    else
        strcpy(device, vol->device);

    fprintf(file, "%s ", device);
    fprintf(file, "%s ", path);
    // special case rfs cause auto will mount it as vfat on samsung.
    fprintf(file, "%s rw\n", vol->fs_type2 != NULL && strcmp(vol->fs_type, "rfs") != 0 ? "auto" : vol->fs_type);
}

void create_fstab()
{
    struct stat info;
    __system("touch /etc/mtab");
    FILE *file = fopen("/etc/fstab", "w");
    if (file == NULL) {
        LOGW("Unable to create /etc/fstab!\n");
        return;
    }
    Volume *vol = volume_for_path("/boot");
    if (NULL != vol && strcmp(vol->fs_type, "mtd") != 0 && strcmp(vol->fs_type, "emmc") != 0 && strcmp(vol->fs_type, "bml") != 0)
         write_fstab_root("/boot", file);
    write_fstab_root("/cache", file);
    write_fstab_root("/data", file);
    write_fstab_root("/datadata", file);
    write_fstab_root("/emmc", file);
    write_fstab_root("/system", file);
    write_fstab_root("/sdcard", file);
    write_fstab_root("/sd-ext", file);
    fclose(file);
    LOGI("Completed outputting fstab.\n");
}

int bml_check_volume(const char *path) {
    ui_print("Checking %s...\n", path);
    ensure_path_unmounted(path);
    if (0 == ensure_path_mounted(path)) {
        ensure_path_unmounted(path);
        return 0;
    }
    
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGE("Unable process volume! Skipping...\n");
        return 0;
    }
    
    ui_print("%s may be rfs. Checking...\n", path);
    char tmp[PATH_MAX];
    sprintf(tmp, "mount -t rfs %s %s", vol->device, path);
    int ret = __system(tmp);
    printf("%d\n", ret);
    return ret == 0 ? 1 : 0;
}

void process_volumes() {
    create_fstab();

    if (is_data_media()) {
        setup_data_media();
    }

    return;

    // dead code.
    if (device_flash_type() != BML)
        return;

    ui_print("Checking for ext4 partitions...\n");
    int ret = 0;
    ret = bml_check_volume("/system");
    ret |= bml_check_volume("/data");
    if (has_datadata())
        ret |= bml_check_volume("/datadata");
    ret |= bml_check_volume("/cache");
    
    if (ret == 0) {
        ui_print("Done!\n");
        return;
    }
    
    char backup_path[PATH_MAX];
    time_t t = time(NULL);
    char backup_name[PATH_MAX];
    struct timeval tp;
    gettimeofday(&tp, NULL);
    sprintf(backup_name, "before-ext4-convert-%d", tp.tv_sec);
    sprintf(backup_path, "/sdcard/clockworkmod/backup/%s", backup_name);

    ui_set_show_text(1);
    ui_print("Filesystems need to be converted to ext4.\n");
    ui_print("A backup and restore will now take place.\n");
    ui_print("If anything goes wrong, your backup will be\n");
    ui_print("named %s. Try restoring it\n", backup_name);
    ui_print("in case of error.\n");

    nandroid_backup(backup_path);
    nandroid_restore(backup_path, 1, 1, 1, 1, 1, 0);
    ui_set_show_text(0);
}

void handle_failure(int ret)
{
    if (ret == 0)
        return;
    if (0 != ensure_path_mounted("/sdcard"))
        return;
    mkdir("/sdcard/clockworkmod", S_IRWXU);
    __system("cp /tmp/recovery.log /sdcard/clockworkmod/recovery.log");
    ui_print("/tmp/recovery.log was copied to /sdcard/clockworkmod/recovery.log. Please open ROM Manager to report the issue.\n");
}

int is_path_mounted(const char* path) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        return 0;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return 0;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv) {
        // volume is already mounted
        return 1;
    }
    return 0;
}

int has_datadata() {
    Volume *vol = volume_for_path("/datadata");
    return vol != NULL;
}

int volume_main(int argc, char **argv) {
    load_volume_table();
    return 0;
}

char* get_modversion()
{
  char* result;
  try_mount("/dev/block/platform/omap/omap_hsmmc.0/by-name/system", "/system", "ext4", NULL);
  FILE * vers = fopen("/system/build.prop", "r");
  if (vers == NULL) 
  {
	return NULL;
  }

  char line[512];
  while(fgets(line, sizeof(line), vers) != NULL && fgets(line, sizeof(line), vers) != EOF) //read a line
  {
	if (strstr(line, "ro.modversion") != NULL)
	{
	  char* strptr = strstr(line, "=") + 1; 
	  result = calloc(strlen(strptr) + 1, sizeof(char));
	  strcpy(result, strptr);  
	  break; //leave the loop, we found what we're after
	}
  }
  fclose(vers); 
  __system("umount /system");
  return result;
}

char* get_android_version()
{
  char* result;
  char* ANDROID_VERSION;
  try_mount("/dev/block/platform/omap/omap_hsmmc.0/by-name/system", "/system", "ext4", NULL);
  FILE * vers = fopen("/system/build.prop", "r");
  if (vers == NULL) 
  {
	return NULL;
  }

  char line[512];
  while(fgets(line, sizeof(line), vers) != NULL && fgets(line, sizeof(line), vers) != EOF) //read a line
  {
	if (strstr(line, "ro.build.display.id") != NULL)
	{
	  char* strptr = strstr(line, "=") + 1; 
	  result = calloc(strlen(strptr) + 1, sizeof(char));
	  strcpy(result, strptr);  
	  break; //leave the loop, we found what we're after
	}
  }
  fclose(vers); 
  __system("umount /system");
  
  //strip only the android version from it
  printf("RAW VERSION: %s\n", result);
  int LENGTH = strlen(result);
  printf("LENGTH: %d\n", LENGTH);  
  int i, k, found;
  for (i=0; i<LENGTH; i++)
  {
	 k = i;	
	 //Android versions follow this scheme: AAADD, A=A-Z, D=0-9, ICS has an extra digit at the end
	 if (isalpha(result[k]) && isalpha(result[k++]) && isalpha(result[k++]) && isdigit(result[k++]) && isdigit(result[k++]))
	 {
		printf("version number found at positions %d through %d:\n", i, k);
		found = 1;		
		break;
	 }
  }	
  if (found)
  {
    ANDROID_VERSION = calloc(strlen(result) + 1, sizeof(char)); //allocate the length of result plus 1 for null-terminator
	 int n = 0;	     
    for(i-=1; i<=k; i++)
    {		     
	   ANDROID_VERSION[n] = result[i];
		n++;  
    }	
    ANDROID_VERSION[n] = '\0'; //dont forget the almighty null-terminator!
  }
  else 
  {
    return NULL;
  }
  printf("Memory Address: %d\n", &ANDROID_VERSION);
  printf("ANDROID VERSION: %s\n", ANDROID_VERSION);
  return ANDROID_VERSION;
}

char* get_release()
{
  char* result;
  try_mount("/dev/block/platform/omap/omap_hsmmc.0/by-name/system", "/system", "ext4", NULL);
  FILE * vers = fopen("/system/build.prop", "r");
  if (vers == NULL) 
  {
	return NULL;
  }

  char line[512];
  while(fgets(line, sizeof(line), vers) != NULL && fgets(line, sizeof(line), vers) != EOF) //read a line
  {
	if (strstr(line, "ro.build.version.release") != NULL)
	{
	  char* strptr = strstr(line, "=") + 1; 
	  result = calloc(strlen(strptr) + 1, sizeof(char));
	  strcpy(result, strptr);  
	  break; //leave the loop, we found what we're after
	}
  }
  fclose(vers); 
  __system("umount /system");
  return result;
}

void show_battstat ()
{
  FILE *fs = fopen ("/sys/class/power_supply/battery/status", "r");
  char *bstat = calloc (14, sizeof (char));

  fgets (bstat, 14, fs);

  FILE *fc = fopen ("/sys/class/power_supply/battery/capacity", "r");
  char *bcap = calloc (14, sizeof (char));

  fgets (bcap, 4, fc);
  
  int noTemp;

  int caplen = strlen (bcap);

  if (bcap[caplen - 1] == '\n')
	  {
	    bcap[caplen - 1] = 0;
	  }

  bcap = strcat (bcap, "%");
  ui_print ("\nBattery Status: ");
  ui_print ("%s\n", bstat);
  char *cmp;

  if (!(cmp = strstr (bstat, "Unknown")))
	  {
	    ui_print ("Charge Level: ");
	    ui_print ("%s\n", bcap);
	    if (!access ("/sys/class/power_supply/battery/temp", F_OK)) {
		ui_print ("Temperature: ");
		FILE *ft = fopen ("/sys/class/power_supply/battery/temp", "r");
		char *btemp = calloc (13, sizeof (char));
		fgets (btemp, 3, ft);
		btemp = strcat (btemp, " C");
	    	ui_print ("%s\n", btemp);
		fclose(ft);
	    }
	   }
  fclose (fc);
  fclose (fs);
}