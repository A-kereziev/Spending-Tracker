#include "ESP32S3_Touchscreen_UI.h"

/* STATIC VARIABLES */
static lv_obj_t *cam_img;
static lv_image_dsc_t cam_img_dsc;
static uint8_t camera_buffer[320 * 240 * 2]; /* RGB565 */

void ESP32S3_Touchscreen_UI_init(const char * asset_path)
{
    LV_LOG("Initializing custom C code using LVGL %d.%d.%d",
           LVGL_VERSION_MAJOR,
           LVGL_VERSION_MINOR,
           LVGL_VERSION_PATCH);

    ESP32S3_Touchscreen_UI_init_gen(asset_path);

    /* Find the image widget by name (searches from the active screen) */
    cam_img = lv_obj_find_by_name(lv_screen_active(),
                                "cam_feed");

    cam_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    cam_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    cam_img_dsc.header.w = 320;
    cam_img_dsc.header.h = 240;
    cam_img_dsc.header.stride = 640;
    cam_img_dsc.data_size = 320 * 240 * 2;
    cam_img_dsc.data = camera_buffer;

    lv_image_set_src(cam_img, &cam_img_dsc);
}