/**
 * @file screen_components_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "screen_components_gen.h"
#include "../ESP32S3_Touchscreen_UI.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/***********************
 *  STATIC VARIABLES
 **********************/

/***********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * screen_components_create(void)
{
    LV_TRACE_OBJ_CREATE("begin");

    static bool screen_created = false;
    if (screen_created) {
        LV_LOG_WARN("`screen_components` is already initialized as a permanent screen. Returning with no change.");
        return screen_components;
    }


    lv_obj_t * the_root = NULL;

    #if ESP32S3_TOUCHSCREEN_UI_CHECK_COMPILE_TARGET(ESP32S3_TOUCHSCREEN_UI_TARGET_ALL)
    if (ESP32S3_Touchscreen_UI_check_target(ESP32S3_TOUCHSCREEN_UI_TARGET_ALL)) {
        screen_components = lv_obj_create(NULL);
        lv_obj_t * lv_obj_0 = screen_components;
        lv_obj_set_name_static(lv_obj_0, "screen_components_#");

        lv_obj_add_style(lv_obj_0, &style_screen_light, 0);
        lv_obj_bind_style(lv_obj_0, &style_screen_dark, 0, &subject_theme_dark, 1);
        lv_obj_add_style(lv_obj_0, &style_scrollbar, LV_PART_SCROLLBAR);
        lv_obj_t * content = column_create(lv_obj_0, SPACE_LG, SPACE_MD, 0, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_name(content, "content");
        lv_obj_set_width(content, 236);
        lv_obj_set_height(content, 316);
        lv_obj_set_x(content, 2);
        lv_obj_set_y(content, 2);

        lv_obj_t * base_box_1 = base_box_create(lv_obj_0);
        lv_obj_set_name(base_box_1, "base_box_1");
        lv_obj_set_width(base_box_1, 239);
        lv_obj_set_height(base_box_1, 319);
        lv_obj_t * cam_feed = image_create(base_box_1, NULL);
        lv_obj_set_name(cam_feed, "cam_feed");
        lv_obj_set_flag(cam_feed, LV_OBJ_FLAG_IGNORE_LAYOUT, true);
        lv_obj_set_x(cam_feed, 0);
        lv_obj_set_y(cam_feed, 24);
        lv_obj_set_width(cam_feed, 239);
        lv_obj_set_height(cam_feed, 240);

        lv_obj_t * capture_img = button_create(base_box_1, "Capture", icon_camera, COLOR_ACCENT, COLOR_ACCENT_TEXT, RADIUS_DEFAULT);
        lv_obj_set_name(capture_img, "capture_img");
        lv_obj_set_flag(capture_img, LV_OBJ_FLAG_IGNORE_LAYOUT, true);
        lv_obj_set_x(capture_img, 17);
        lv_obj_set_y(capture_img, 270);
        lv_obj_add_event_cb(capture_img, capture_img_callback, LV_EVENT_CLICKED, NULL);

        lv_obj_t * wifi_reroute = button_create(base_box_1, "WiFi", icon_wifi, COLOR_ACCENT, COLOR_ACCENT_TEXT, RADIUS_DEFAULT);
        lv_obj_set_name(wifi_reroute, "wifi_reroute");
        lv_obj_set_flag(wifi_reroute, LV_OBJ_FLAG_IGNORE_LAYOUT, true);
        lv_obj_set_x(wifi_reroute, 137);
        lv_obj_set_y(wifi_reroute, 270);
        lv_obj_add_event_cb(wifi_reroute, wifi_reroute_callback, LV_EVENT_CLICKED, NULL);

        the_root = lv_obj_0;
    }
    #endif

    if (the_root) screen_created = true;

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

