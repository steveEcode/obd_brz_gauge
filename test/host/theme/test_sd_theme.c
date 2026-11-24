/* Real loader and library; simulated card/NVS failures, real temporary file.
 * No Flash API is provided: any runtime dependency on one fails to link. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE 1
#include "th_util.h"
#include <string.h>
static size_t test_strlcpy(char *dst,const char *src,size_t n)
{ size_t len=strlen(src); if(n) { size_t m=len<n-1 ? len : n-1; memcpy(dst,src,m); dst[m]=0; } return len; }
#define strlcpy test_strlcpy
#include "../../../main/theme_engine/theme_loader.c"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
static unsigned open_dirs, peak_dirs;
static bool fail_directory_read;
static DIR *tracked_opendir(const char *path)
{
    DIR *dir = opendir(path);
    if (dir) { ++open_dirs; if (open_dirs > peak_dirs) peak_dirs = open_dirs; }
    return dir;
}
static int tracked_closedir(DIR *dir)
{
    TH_CHECK(open_dirs > 0); --open_dirs;
    return closedir(dir);
}
static struct dirent *tracked_readdir(DIR *dir)
{
    if (fail_directory_read) { errno = EIO; return NULL; }
    return readdir(dir);
}
#define opendir tracked_opendir
#define closedir tracked_closedir
#define readdir tracked_readdir
#include "../../../main/app_obd_dsp/theme_library.c"
#undef opendir
#undef closedir
#undef readdir

static FILE *package;
static char selection[65], pending[65];
static bool mounted=true, locked=true, ota, nvs_fail, missing, fail_io;
static unsigned depth, reads, renames;
static esp_err_t metadata_error;
static const ui_theme_t fallback={.id="default",.name="DEFAULT"};
const ui_theme_t *ui_theme_get(uint8_t i) { (void)i; return &fallback; }
uint32_t ui_theme_color(ui_color_role_t role) { (void)role; return 0; }
void te_sidecar_reload(void) {}
void te_sidecar_drop(void) {}
bool sd_card_is_mounted(void) { return mounted; }
bool ota_wifi_server_is_active(void) { return ota; }
bool storage_job_lock(TickType_t ticks) { (void)ticks; if (!locked) return false; depth++; return true; }
void storage_job_unlock(void) { TH_CHECK(depth>0); depth--; }
bool storage_job_busy(void) { return depth!=0; }
void storage_sd_mkdir_parents(const char *path) { (void)path; }
esp_err_t sd_card_rename(const char *from, const char *to) { (void)from; (void)to; renames++; return ESP_OK; }
void storage_theme_key(const char *id, char *out, size_t n) { snprintf(out,n,"%s",id); }
int64_t storage_sd_file_size(const char *path)
{
    (void)path;
    if (missing || !package) return -1;
    fseek(package,0,SEEK_END); return ftell(package);
}
esp_err_t storage_theme_meta_from_file(const char *path, storage_theme_meta_t *meta)
{
    if (missing || !package) return ESP_ERR_NOT_FOUND;
    if (metadata_error) return metadata_error;
    memset(meta,0,sizeof(*meta)); strcpy(meta->id,"demo");
    meta->used_len=(uint32_t)storage_sd_file_size(path);
    return ESP_OK;
}
esp_err_t storage_sd_file_foreach(const char *path,uint32_t off,uint32_t len,storage_chunk_cb_t cb,void *ctx)
{
    TH_CHECK(depth>0);
    TH_CHECK(strstr(path,"/OBD/THEMES/")!=NULL);
    reads++;
    if (fail_io || missing) return ESP_FAIL;
    if (fseek(package,(long)off,SEEK_SET)) return ESP_FAIL;
    uint8_t bytes[4096];
    while (len) {
        size_t n=len<sizeof(bytes) ? len : sizeof(bytes);
        if (fread(bytes,1,n,package)!=n) return ESP_ERR_INVALID_SIZE;
        if (!cb(bytes,n,ctx)) break;
        len-=(uint32_t)n;
    }
    return ESP_OK;
}
esp_err_t nvs_app_get_str(const char *ns,const char *key,char *out,size_t len)
{
    (void)ns;
    const char *value=!strcmp(key,"th_cur") ? selection : pending;
    if (!value[0]) return ESP_ERR_NVS_NOT_FOUND;
    snprintf(out,len,"%s",value); return ESP_OK;
}
esp_err_t nvs_app_set_str(const char *ns,const char *key,const char *value)
{
    (void)ns;
    if (nvs_fail) return ESP_FAIL;
    snprintf(!strcmp(key,"th_cur") ? selection : pending,65,"%s",value); return ESP_OK;
}
esp_err_t nvs_app_erase_key(const char *ns,const char *key)
{
    (void)ns;
    if (nvs_fail) return ESP_FAIL;
    if (!strcmp(key,"th_wr")) pending[0]=0;
    if (!strcmp(key,"th_cur")) selection[0]=0;
    return ESP_OK;
}
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned prio,void *handle)
{ (void)name; (void)stack; (void)prio; (void)handle; fn(arg); return pdPASS; }
void vTaskDelete(void *task) { (void)task; }

static void make_package(void)
{
    package=tmpfile(); TH_CHECK(package!=NULL);
    uint8_t manifest[TE_MANIFEST_SIZE]; memset(manifest,0xff,sizeof(manifest));
    const char *json="{\"schema_version\":\"1.0\",\"theme\":{\"id\":\"demo\"},"
      "\"assets\":{\"dial\":{\"format\":\"rgb565\",\"width\":1,\"height\":1,\"offset\":4194304,\"size\":2},"
      "\"font_demo\":{\"format\":\"lv_font_bin\",\"offset\":4194306,\"size\":4}},"
      "\"pages\":{\"theme_pages\":[{\"id\":\"main_gauge\",\"layout_data_offset\":4194310,\"layout_data_size\":2}]}}";
    memcpy(manifest,json,strlen(json));
    TH_CHECK(fwrite(manifest,1,sizeof(manifest),package)==sizeof(manifest));
    TH_CHECK(fseek(package,4194304,SEEK_SET)==0);
    const uint8_t assets[]={0xf8,0,1,2,3,4,'{','}'};
    TH_CHECK(fwrite(assets,1,sizeof(assets),package)==sizeof(assets)); fflush(package);
}
static void test_deep_theme_delete(void)
{
    char root[] = "/tmp/obd-theme-delete-XXXXXX";
    TH_CHECK(mkdtemp(root) != NULL);
    char path[384]; snprintf(path, sizeof(path), "%s", root);
    /* Real FAT-compatible short names, with a depth that cannot fit in an
     * embedded task stack if one 384-byte path and DIR stay live per level. */
    for (unsigned i = 0; i < 120; ++i) {
        strcat(path, "/d"); TH_CHECK(mkdir(path, 0700) == 0);
    }
    strcat(path, "/asset.bin");
    FILE *asset = fopen(path, "wb"); TH_CHECK(asset != NULL); TH_CHECK(fclose(asset) == 0);
    peak_dirs = 0;
    TH_CHECK(remove_tree(root));
    TH_CHECK(peak_dirs == 1 && open_dirs == 0);
    TH_CHECK(access(root, F_OK) != 0 && errno == ENOENT);
    TH_CHECK(remove_tree(root)); /* already absent */

    char broken[] = "/tmp/obd-theme-read-error-XXXXXX";
    TH_CHECK(mkdtemp(broken) != NULL);
    fail_directory_read = true;
    TH_CHECK(!remove_tree(broken) && open_dirs == 0);
    fail_directory_read = false;
    TH_CHECK(access(broken, F_OK) == 0 && remove_tree(broken));
}
int main(void)
{
    test_deep_theme_delete();
    lv_init(); make_package();
    TH_CHECK(theme_engine_init()==ESP_ERR_NOT_FOUND && !g_te->custom);
    TH_CHECK(theme_library_select("demo")==ESP_OK);
    TH_CHECK(theme_load(0)==ESP_OK && g_te->custom && g_te->file_size>0x400000);
    TH_CHECK(!g_te->map && !g_te->part && g_te->n_images==1 && g_te->n_fonts==1);
    TH_CHECK(theme_page_list_count()==1);
    uint8_t pixel[2]; TH_CHECK(te_read(4194304,pixel,2)==ESP_OK && pixel[0]==0xf8);
    unsigned before=reads;
    TH_CHECK(te_read(UINT32_MAX,pixel,2)==ESP_ERR_INVALID_SIZE && reads==before);
    TH_CHECK(te_read(0,pixel,0)==ESP_OK && reads==before);
    te_font_file_t *font=te_fs_open(&g_te->fs_drv,"font_demo",LV_FS_MODE_RD);
    TH_CHECK(font!=NULL); uint8_t bytes[4]; uint32_t got=0;
    TH_CHECK(te_fs_read(&g_te->fs_drv,font,bytes,4,&got)==LV_FS_RES_OK && got==4 && bytes[3]==4);
    te_fs_close(&g_te->fs_drv,font);
    g_te->n_live=1; TH_CHECK(theme_load(0)==ESP_ERR_INVALID_STATE && g_te->custom); g_te->n_live=0;
    mounted=false; TH_CHECK(theme_load(0)==ESP_ERR_NOT_FOUND && !g_te->custom && !strcmp(selection,"demo"));
    TH_CHECK(theme_library_select("other")==ESP_ERR_INVALID_STATE);
    mounted=true; missing=true;
    TH_CHECK(theme_load(0)==ESP_ERR_NOT_FOUND && !g_te->custom);
    TH_CHECK(theme_library_select("other")==ESP_ERR_NOT_FOUND && !strcmp(selection,"demo")); missing=false;
    metadata_error=ESP_ERR_INVALID_SIZE;
    TH_CHECK(theme_load(0)==ESP_ERR_INVALID_SIZE && !g_te->custom); metadata_error=0;
    nvs_fail=true; TH_CHECK(theme_library_select("other")==ESP_FAIL && !strcmp(selection,"demo")); nvs_fail=false;
    strcpy(selection,"../escape"); TH_CHECK(theme_load(0)==ESP_ERR_INVALID_ARG && !g_te->custom);
    strcpy(selection,"demo"); locked=false; TH_CHECK(theme_load(0)==ESP_ERR_TIMEOUT && !g_te->custom); locked=true;
    fail_io=true; TH_CHECK(theme_load(0)==ESP_FAIL && !g_te->custom); fail_io=false;
    TH_CHECK(theme_library_activate("demo")==ESP_OK && theme_library_get_state(NULL)==THEME_LIB_DONE);
    theme_library_ack(); TH_CHECK(theme_load(0)==ESP_OK);
    ota=true; TH_CHECK(theme_library_activate("demo")==ESP_ERR_INVALID_STATE); ota=false;
    TH_CHECK(theme_library_import_file("/drop/theme.bin",NULL,0)==ESP_ERR_INVALID_STATE && renames==0);
    ota=true; TH_CHECK(theme_library_import_file("/drop/theme.bin",NULL,0)==ESP_OK && renames==1); ota=false;
    strcpy(pending,"demo"); mounted=false;
    TH_CHECK(theme_library_resume()==ESP_ERR_INVALID_STATE && pending[0]); mounted=true;
    TH_CHECK(theme_library_resume()==ESP_OK && !pending[0]); theme_library_ack();
    TH_CHECK(theme_library_select("../bad")==ESP_ERR_INVALID_ARG);
    TH_CHECK(depth==0);
    theme_unload(); fclose(package);
    vSemaphoreDelete(g_te->lock); te_free(g_te); g_te=NULL;
    return th_report("test_sd_theme");
}
