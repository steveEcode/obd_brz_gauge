#include "../ui.h"

LV_IMG_DECLARE(imgBoot86DialRing);
LV_IMG_DECLARE(imgBoot86Logo86);
LV_IMG_DECLARE(imgBoot86Logo86MotionBlur);
LV_IMG_DECLARE(imgBoot86GRLogo);
LV_IMG_DECLARE(imgBoot86GRLogoMotionBlur);
LV_IMG_DECLARE(imgBoot86GRBlack);
LV_IMG_DECLARE(imgBoot86GRRed);
LV_IMG_DECLARE(imgBoot86GRText);

/*
 * Boot86 时间轴：
 *
 *    0 -  800 ms：黑色块与红色块从左右两侧以 1.25 倍大小
 *                  同时淡入、向中间移动并缩放到 1.0 倍；
 *                  运动过程显示方向性拖影。
 *  700 - 1000 ms：GR 文字在中间淡入。
 *  800 - 1200 ms：黑色块与红色块定格；
 *  1000 - 1200 ms：完整拆分画面定格。
 *  1200 - 3100 ms：切换成完整 GR Logo，沿用原三次曲线加速旋转。
 * 3100 - 4600 ms：86 从 6000°/s 减速并定格，共 1500ms。
 * 4600 - 5200 ms：Dial 淡入并顺时针旋转 3/4 圈。
 * 5200 - 5800 ms：86 与 Dial 完全显示并定格。
 * 5800 - 6400 ms：86 淡出；Dial 淡出并逆时针旋转 3/4 圈。
 */
#define BOOT86_GR_FADE_END_MS                      1200U
#define BOOT86_GR_ACCEL_END_MS                     3100U
#define BOOT86_86_STOP_END_MS                      4600U
#define BOOT86_DIAL_IN_END_MS                      5200U
#define BOOT86_HOLD_END_MS                         5800U
#define BOOT86_EXIT_END_MS                         6400U
#define BOOT86_TOTAL_MS                            6400U

#define BOOT86_GR_SPLIT_MOVE_END_MS                800U
#define BOOT86_GR_TEXT_FADE_START_MS               700U
#define BOOT86_GR_TEXT_FADE_END_MS                 1000U

#define BOOT86_GR_SPLIT_START_ZOOM            320U
#define BOOT86_GR_SPLIT_END_ZOOM              256U

#define BOOT86_GR_BLACK_START_X              (-220)
#define BOOT86_GR_RED_START_X                  220
#define BOOT86_GR_TRAIL_MAX_OFFSET              48


/*
 * 33ms 对应约 30.3 FPS。
 */
#define BOOT86_FRAME_MS                       33U

/*
 * GR：
 *
 * 使用 angle ∝ t³。
 * 1900ms 内达到 9000°/s。
 *
 * 总角度：
 * 9000 × 1.9 / 3 = 5700°
 */
#define BOOT86_GR_END_ANGLE_10             57000
#define BOOT86_GR_BLUR_SPEED_10_PER_S      30000LL

/*
 * 86：
 *
 * 从 5700° 开始，以 6000°/s 起步。
 * 1500ms 内减速到 0。
 *
 * 最终角度为 10080°，正好是 28 圈，
 * 因此最后精确停在正方向。
 */
#define BOOT86_86_START_ANGLE_10           57000
#define BOOT86_86_END_ANGLE_10            100800
#define BOOT86_86_START_SPEED_10_PER_S     60000LL
#define BOOT86_86_BLUR_SPEED_10_PER_S      22000LL

/*
 * Dial：
 *
 * 90° → 360°：
 * 顺时针旋转 270°，即 3/4 圈。
 *
 * 退出时完全反向播放：
 * 360° → 90°，逆时针旋转 270°。
 */
#define BOOT86_DIAL_START_ANGLE_10           900
#define BOOT86_DIAL_END_ANGLE_10            3600

lv_obj_t *ui_ScreenPageBoot86 = NULL;

static lv_obj_t *s_boot86_ring = NULL;

static lv_obj_t *s_boot86_logo_sharp = NULL;
static lv_obj_t *s_boot86_logo_blur = NULL;

static lv_obj_t *s_boot86_gr_sharp = NULL;
static lv_obj_t *s_boot86_gr_blur = NULL;

static lv_obj_t *s_boot86_gr_black = NULL;
static lv_obj_t *s_boot86_gr_black_trail = NULL;
static lv_obj_t *s_boot86_gr_red = NULL;
static lv_obj_t *s_boot86_gr_red_trail = NULL;
static lv_obj_t *s_boot86_gr_text = NULL;

static lv_timer_t *s_boot86_timer = NULL;
static uint32_t s_boot86_started_tick = 0;
static bool s_boot86_finished = false;

static void boot86_timer_cb(lv_timer_t *timer);
static void boot86_screen_delete_cb(lv_event_t *event);

static int16_t boot86_normalize_angle(int32_t angle_10)
{
    angle_10 %= 3600;

    if (angle_10 < 0) {
        angle_10 += 3600;
    }

    return (int16_t)angle_10;
}

static uint32_t boot86_progress_1024(
    uint32_t elapsed_ms,
    uint32_t start_ms,
    uint32_t end_ms
)
{
    if (elapsed_ms <= start_ms) {
        return 0;
    }

    if (elapsed_ms >= end_ms || end_ms <= start_ms) {
        return 1024;
    }

    return (
        (elapsed_ms - start_ms) *
        1024U
    ) / (
        end_ms - start_ms
    );
}

static uint32_t boot86_cubic_ease_out_1024(
    uint32_t progress
)
{
    if (progress >= 1024U) {
        return 1024U;
    }

    uint32_t remaining =
        1024U - progress;

    return 1024U - (uint32_t)(
        (
            (uint64_t)remaining *
            remaining *
            remaining
        ) /
        (
            1024ULL *
            1024ULL
        )
    );
}

static uint32_t boot86_cubic_ease_in_1024(
    uint32_t progress
)
{
    if (progress >= 1024U) {
        return 1024U;
    }

    return (uint32_t)(
        (
            (uint64_t)progress *
            progress *
            progress
        ) /
        (
            1024ULL *
            1024ULL
        )
    );
}

static uint8_t boot86_opacity_from_progress(
    uint32_t progress
)
{
    if (progress >= 1024U) {
        return 255;
    }

    return (uint8_t)(
        (255U * progress) /
        1024U
    );
}

static void boot86_set_image(
    lv_obj_t *object,
    bool visible,
    uint8_t opacity,
    int32_t angle_10
)
{
    if (!object) {
        return;
    }

    if (!visible) {
        lv_obj_add_flag(
            object,
            LV_OBJ_FLAG_HIDDEN
        );
        return;
    }

    lv_obj_clear_flag(
        object,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_set_style_img_opa(
        object,
        opacity,
        LV_PART_MAIN
    );

    lv_img_set_angle(
        object,
        boot86_normalize_angle(angle_10)
    );
}


static void boot86_set_split_image(
    lv_obj_t *object,
    bool visible,
    uint8_t opacity,
    int32_t x_offset,
    uint16_t zoom
)
{
    if (!object) {
        return;
    }

    if (!visible) {
        lv_obj_add_flag(
            object,
            LV_OBJ_FLAG_HIDDEN
        );

        return;
    }

    lv_obj_clear_flag(
        object,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_align(
        object,
        LV_ALIGN_CENTER,
        (lv_coord_t)x_offset,
        0
    );

    lv_img_set_zoom(
        object,
        zoom
    );

    lv_img_set_angle(
        object,
        0
    );

    lv_obj_set_style_img_opa(
        object,
        opacity,
        LV_PART_MAIN
    );
}

static void boot86_set_logo_pair(
    lv_obj_t *sharp,
    lv_obj_t *blur,
    bool visible,
    bool use_blur,
    uint8_t opacity,
    int32_t angle_10
)
{
    if (!visible) {
        boot86_set_image(
            sharp,
            false,
            0,
            0
        );

        boot86_set_image(
            blur,
            false,
            0,
            0
        );

        return;
    }

    boot86_set_image(
        sharp,
        !use_blur,
        opacity,
        angle_10
    );

    boot86_set_image(
        blur,
        use_blur,
        opacity,
        angle_10
    );
}

static lv_obj_t *boot86_create_image(
    lv_obj_t *parent,
    const lv_img_dsc_t *source
)
{
    lv_obj_t *image =
        lv_img_create(parent);

    lv_img_set_src(
        image,
        source
    );

    lv_obj_set_size(
        image,
        360,
        360
    );

    lv_obj_center(image);

    lv_img_set_pivot(
        image,
        180,
        180
    );

    lv_img_set_zoom(
        image,
        256
    );

    lv_obj_clear_flag(
        image,
        LV_OBJ_FLAG_CLICKABLE |
        LV_OBJ_FLAG_SCROLLABLE
    );

    return image;
}

void ui_ScreenPageBoot86_screen_init(void)
{
    if (ui_ScreenPageBoot86) {
        return;
    }

    ui_ScreenPageBoot86 =
        lv_obj_create(NULL);

    lv_obj_add_event_cb(
        ui_ScreenPageBoot86,
        boot86_screen_delete_cb,
        LV_EVENT_DELETE,
        NULL
    );

    lv_obj_clear_flag(
        ui_ScreenPageBoot86,
        LV_OBJ_FLAG_SCROLLABLE
    );

    lv_obj_set_style_bg_color(
        ui_ScreenPageBoot86,
        lv_color_hex(0x000000),
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_opa(
        ui_ScreenPageBoot86,
        LV_OPA_COVER,
        LV_PART_MAIN
    );

    lv_obj_set_style_border_width(
        ui_ScreenPageBoot86,
        0,
        LV_PART_MAIN
    );

    lv_obj_set_style_pad_all(
        ui_ScreenPageBoot86,
        0,
        LV_PART_MAIN
    );

    /*
     * 图层顺序：
     * Dial 在最底层。
     * 每组 Logo 的模糊版在锐利版下方。
     *
     * 任意时刻每组只显示其中一个版本，
     * 因此不会额外同时绘制多个残影图层。
     */
    s_boot86_ring = boot86_create_image(
        ui_ScreenPageBoot86,
        &imgBoot86DialRing
    );

    s_boot86_logo_blur = boot86_create_image(
        ui_ScreenPageBoot86,
        &imgBoot86Logo86MotionBlur
    );

    s_boot86_logo_sharp = boot86_create_image(
        ui_ScreenPageBoot86,
        &imgBoot86Logo86
    );

    /*
     * GR 拆分入场图层：
     * 拖影位于主体下方，GR 文字位于主体上方。
     */
    s_boot86_gr_black_trail = boot86_create_image(
        ui_ScreenPageBoot86,
        &imgBoot86GRBlack
    );

    s_boot86_gr_red_trail = boot86_create_image(
        ui_ScreenPageBoot86,
        &imgBoot86GRRed
    );

    s_boot86_gr_black = boot86_create_image(
        ui_ScreenPageBoot86,
        &imgBoot86GRBlack
    );

    s_boot86_gr_red = boot86_create_image(
        ui_ScreenPageBoot86,
        &imgBoot86GRRed
    );

    s_boot86_gr_text = boot86_create_image(
        ui_ScreenPageBoot86,
        &imgBoot86GRText
    );

    s_boot86_gr_blur = boot86_create_image(
        ui_ScreenPageBoot86,
        &imgBoot86GRLogoMotionBlur
    );

    s_boot86_gr_sharp = boot86_create_image(
        ui_ScreenPageBoot86,
        &imgBoot86GRLogo
    );

    ui_boot86_reset();
}

void ui_boot86_reset(void)
{
    if (s_boot86_timer) {
        lv_timer_del(s_boot86_timer);
        s_boot86_timer = NULL;
    }

    s_boot86_finished = false;

    boot86_set_image(
        s_boot86_ring,
        false,
        0,
        0
    );

    boot86_set_logo_pair(
        s_boot86_logo_sharp,
        s_boot86_logo_blur,
        false,
        false,
        0,
        0
    );

    boot86_set_split_image(
        s_boot86_gr_black,
        false,
        0,
        0,
        BOOT86_GR_SPLIT_END_ZOOM
    );

    boot86_set_split_image(
        s_boot86_gr_black_trail,
        false,
        0,
        0,
        BOOT86_GR_SPLIT_END_ZOOM
    );

    boot86_set_split_image(
        s_boot86_gr_red,
        false,
        0,
        0,
        BOOT86_GR_SPLIT_END_ZOOM
    );

    boot86_set_split_image(
        s_boot86_gr_red_trail,
        false,
        0,
        0,
        BOOT86_GR_SPLIT_END_ZOOM
    );

    boot86_set_split_image(
        s_boot86_gr_text,
        false,
        0,
        0,
        BOOT86_GR_SPLIT_END_ZOOM
    );

    boot86_set_logo_pair(
        s_boot86_gr_sharp,
        s_boot86_gr_blur,
        false,
        false,
        0,
        0
    );
}

void ui_boot86_show_waiting(void)
{
    /*
     * 左右从表保持纯黑。
     */
    ui_boot86_reset();
}

bool ui_boot86_render(uint32_t elapsed_ms)
{
    if (!ui_ScreenPageBoot86) {
        ui_ScreenPageBoot86_screen_init();
    }

    /*
     * 第一阶段：GR 拆分入场。
     *
     * 0~800ms：
     * - 黑色块从左侧、红色块从右侧同时进入；
     * - 1.25 倍缩小到 1.0 倍；
     * - 同时淡入；
     * - 拖影位于运动方向后方，并随速度降低而消失。
     *
     * 700~1000ms：GR 文字在中间淡入。
     * 800~1200ms：黑色块与红色块定格。
     * 1000~1200ms：完整拆分画面定格。
     */
    if (elapsed_ms < BOOT86_GR_FADE_END_MS) {
        uint32_t move_progress =
            boot86_progress_1024(
                elapsed_ms,
                0,
                BOOT86_GR_SPLIT_MOVE_END_MS
            );

        /*
         * 800ms 入场使用线性时间进度。
         *
         * 原来的 cubic ease-out 会在动画前半段快速接近中心，
         * 模拟器只有少数几帧能看到移动，看起来近似普通淡入。
         * 线性进度保证左右部件在完整 800ms 内持续移动和缩放。
         */
        uint32_t linear_move =
            move_progress;

        uint32_t remaining =
            1024U - linear_move;

        int32_t black_x =
            (
                BOOT86_GR_BLACK_START_X *
                (int32_t)remaining
            ) /
            1024;

        int32_t red_x =
            (
                BOOT86_GR_RED_START_X *
                (int32_t)remaining
            ) /
            1024;

        uint16_t zoom =
            BOOT86_GR_SPLIT_END_ZOOM +
            (uint16_t)(
                (
                    (
                        BOOT86_GR_SPLIT_START_ZOOM -
                        BOOT86_GR_SPLIT_END_ZOOM
                    ) *
                    remaining
                ) /
                1024U
            );

        uint8_t main_opacity =
            boot86_opacity_from_progress(
                linear_move
            );

        int32_t trail_offset =
            (
                BOOT86_GR_TRAIL_MAX_OFFSET *
                (int32_t)remaining
            ) /
            1024;

        uint8_t trail_opacity =
            (uint8_t)(
                (
                    (uint64_t)main_opacity *
                    remaining *
                    140U
                ) /
                (
                    1024ULL *
                    255ULL
                )
            );

        uint16_t trail_zoom =
            zoom +
            (uint16_t)(
                (10U * remaining) /
                1024U
            );

        uint32_t text_progress =
            boot86_progress_1024(
                elapsed_ms,
                BOOT86_GR_TEXT_FADE_START_MS,
                BOOT86_GR_TEXT_FADE_END_MS
            );

        uint8_t text_opacity =
            boot86_opacity_from_progress(
                text_progress
            );

        boot86_set_split_image(
            s_boot86_gr_black_trail,
            true,
            trail_opacity,
            black_x - trail_offset,
            trail_zoom
        );

        boot86_set_split_image(
            s_boot86_gr_red_trail,
            true,
            trail_opacity,
            red_x + trail_offset,
            trail_zoom
        );

        boot86_set_split_image(
            s_boot86_gr_black,
            true,
            main_opacity,
            black_x,
            zoom
        );

        boot86_set_split_image(
            s_boot86_gr_red,
            true,
            main_opacity,
            red_x,
            zoom
        );

        boot86_set_split_image(
            s_boot86_gr_text,
            true,
            text_opacity,
            0,
            BOOT86_GR_SPLIT_END_ZOOM
        );

        boot86_set_logo_pair(
            s_boot86_gr_sharp,
            s_boot86_gr_blur,
            false,
            false,
            0,
            0
        );

        boot86_set_logo_pair(
            s_boot86_logo_sharp,
            s_boot86_logo_blur,
            false,
            false,
            0,
            0
        );

        boot86_set_image(
            s_boot86_ring,
            false,
            0,
            0
        );

        return false;
    }

    /*
     * 1200ms 时切换为完整 GR Logo。
     * 后续旋转、切换 86、Dial 入场和退出逻辑保持原样。
     */
    boot86_set_split_image(
        s_boot86_gr_black,
        false,
        0,
        0,
        BOOT86_GR_SPLIT_END_ZOOM
    );

    boot86_set_split_image(
        s_boot86_gr_black_trail,
        false,
        0,
        0,
        BOOT86_GR_SPLIT_END_ZOOM
    );

    boot86_set_split_image(
        s_boot86_gr_red,
        false,
        0,
        0,
        BOOT86_GR_SPLIT_END_ZOOM
    );

    boot86_set_split_image(
        s_boot86_gr_red_trail,
        false,
        0,
        0,
        BOOT86_GR_SPLIT_END_ZOOM
    );

    boot86_set_split_image(
        s_boot86_gr_text,
        false,
        0,
        0,
        BOOT86_GR_SPLIT_END_ZOOM
    );

    /*
     * 第二阶段：GR 三次加速。
     *
     * angle ∝ t³
     * velocity ∝ t²
     * acceleration ∝ t
     *
     * 所以加速度会持续增大。
     */
    if (elapsed_ms < BOOT86_GR_ACCEL_END_MS) {
        uint32_t time_ms =
            elapsed_ms -
            BOOT86_GR_FADE_END_MS;

        uint32_t duration_ms =
            BOOT86_GR_ACCEL_END_MS -
            BOOT86_GR_FADE_END_MS;

        int64_t duration_cubed =
            (int64_t)duration_ms *
            duration_ms *
            duration_ms;

        int32_t angle_10 = (int32_t)(
            (
                (int64_t)BOOT86_GR_END_ANGLE_10 *
                time_ms *
                time_ms *
                time_ms
            ) /
            duration_cubed
        );

        int64_t speed_10_per_s =
            (
                3LL *
                BOOT86_GR_END_ANGLE_10 *
                time_ms *
                time_ms *
                1000LL
            ) /
            duration_cubed;

        bool use_blur =
            speed_10_per_s >=
            BOOT86_GR_BLUR_SPEED_10_PER_S;

        boot86_set_logo_pair(
            s_boot86_gr_sharp,
            s_boot86_gr_blur,
            true,
            use_blur,
            255,
            angle_10
        );

        boot86_set_logo_pair(
            s_boot86_logo_sharp,
            s_boot86_logo_blur,
            false,
            false,
            0,
            0
        );

        boot86_set_image(
            s_boot86_ring,
            false,
            0,
            0
        );

        return false;
    }

    /*
     * 第三阶段：86 从出现开始立即减速。
     *
     * 整个出现到定格过程为 1500ms。
     * 初始速度为 6000°/s，结束速度为 0。
     */
    if (elapsed_ms < BOOT86_86_STOP_END_MS) {
        uint32_t time_ms =
            elapsed_ms -
            BOOT86_GR_ACCEL_END_MS;

        uint32_t duration_ms =
            BOOT86_86_STOP_END_MS -
            BOOT86_GR_ACCEL_END_MS;

        int64_t displacement =
            BOOT86_86_END_ANGLE_10 -
            BOOT86_86_START_ANGLE_10;

        int64_t initial_velocity_span =
            (
                BOOT86_86_START_SPEED_10_PER_S *
                duration_ms
            ) /
            1000LL;

        int64_t coefficient_a =
            initial_velocity_span -
            2LL * displacement;

        int64_t coefficient_b =
            3LL * displacement -
            2LL * initial_velocity_span;

        int64_t coefficient_c =
            initial_velocity_span;

        int64_t time_squared =
            (int64_t)time_ms *
            time_ms;

        int64_t time_cubed =
            time_squared *
            time_ms;

        int64_t duration_squared =
            (int64_t)duration_ms *
            duration_ms;

        int64_t duration_cubed =
            duration_squared *
            duration_ms;

        int64_t angle_delta =
            (
                coefficient_a *
                time_cubed
            ) /
            duration_cubed +
            (
                coefficient_b *
                time_squared
            ) /
            duration_squared +
            (
                coefficient_c *
                time_ms
            ) /
            duration_ms;

        int32_t angle_10 =
            BOOT86_86_START_ANGLE_10 +
            (int32_t)angle_delta;

        int64_t derivative_numerator =
            3LL *
            coefficient_a *
            time_squared +
            2LL *
            coefficient_b *
            time_ms *
            duration_ms +
            coefficient_c *
            duration_squared;

        int64_t speed_10_per_s =
            (
                derivative_numerator *
                1000LL
            ) /
            duration_cubed;

        if (speed_10_per_s < 0) {
            speed_10_per_s = 0;
        }

        bool use_blur =
            speed_10_per_s >=
            BOOT86_86_BLUR_SPEED_10_PER_S;

        boot86_set_logo_pair(
            s_boot86_gr_sharp,
            s_boot86_gr_blur,
            false,
            false,
            0,
            0
        );

        boot86_set_logo_pair(
            s_boot86_logo_sharp,
            s_boot86_logo_blur,
            true,
            use_blur,
            255,
            angle_10
        );

        boot86_set_image(
            s_boot86_ring,
            false,
            0,
            0
        );

        return false;
    }

    /*
     * 第四阶段：
     *
     * 86 已经停在正方向。
     * Dial 淡入并顺时针旋转 3/4 圈。
     *
     * 使用 ease-out：
     * 开始较快，靠近终点逐渐减速。
     */
    if (elapsed_ms < BOOT86_DIAL_IN_END_MS) {
        uint32_t progress =
            boot86_progress_1024(
                elapsed_ms,
                BOOT86_86_STOP_END_MS,
                BOOT86_DIAL_IN_END_MS
            );

        uint32_t eased =
            boot86_cubic_ease_out_1024(
                progress
            );

        int32_t dial_angle_10 =
            BOOT86_DIAL_START_ANGLE_10 +
            (int32_t)(
                (
                    (int64_t)(
                        BOOT86_DIAL_END_ANGLE_10 -
                        BOOT86_DIAL_START_ANGLE_10
                    ) *
                    eased
                ) /
                1024LL
            );

        boot86_set_logo_pair(
            s_boot86_logo_sharp,
            s_boot86_logo_blur,
            true,
            false,
            255,
            0
        );

        boot86_set_logo_pair(
            s_boot86_gr_sharp,
            s_boot86_gr_blur,
            false,
            false,
            0,
            0
        );

        boot86_set_image(
            s_boot86_ring,
            true,
            boot86_opacity_from_progress(
                eased
            ),
            dial_angle_10
        );

        return false;
    }

    /*
     * 第五阶段：最终画面定格 600ms。
     *
     * 86 保持清晰、完全显示并停在正方向；
     * Dial 保持完全显示并停在正方向。
     */
    if (elapsed_ms < BOOT86_HOLD_END_MS) {
        boot86_set_logo_pair(
            s_boot86_logo_sharp,
            s_boot86_logo_blur,
            true,
            false,
            255,
            0
        );

        boot86_set_logo_pair(
            s_boot86_gr_sharp,
            s_boot86_gr_blur,
            false,
            false,
            0,
            0
        );

        boot86_set_image(
            s_boot86_ring,
            true,
            255,
            0
        );

        return false;
    }

    /*
     * 第六阶段：退出动画，共 600ms。
     *
     * 86：
     * - 线性淡出；
     *
     * Dial：
     * - 完全反向播放出现动画；
     * - 从正方向逆时针旋转 3/4 圈；
     * - 同时淡出；
     * - 使用 ease-in，因此开始缓慢、随后加速离场。
     */
    if (elapsed_ms < BOOT86_EXIT_END_MS) {
        uint32_t progress =
            boot86_progress_1024(
                elapsed_ms,
                BOOT86_HOLD_END_MS,
                BOOT86_EXIT_END_MS
            );

        uint32_t reverse_eased =
            boot86_cubic_ease_in_1024(
                progress
            );

        uint8_t logo_opacity =
            (uint8_t)(
                (
                    255U *
                    (1024U - progress)
                ) /
                1024U
            );

        uint8_t dial_opacity =
            (uint8_t)(
                (
                    255U *
                    (1024U - reverse_eased)
                ) /
                1024U
            );

        int32_t dial_angle_10 =
            BOOT86_DIAL_END_ANGLE_10 -
            (int32_t)(
                (
                    (int64_t)(
                        BOOT86_DIAL_END_ANGLE_10 -
                        BOOT86_DIAL_START_ANGLE_10
                    ) *
                    reverse_eased
                ) /
                1024LL
            );

        boot86_set_logo_pair(
            s_boot86_logo_sharp,
            s_boot86_logo_blur,
            true,
            false,
            logo_opacity,
            0
        );

        boot86_set_logo_pair(
            s_boot86_gr_sharp,
            s_boot86_gr_blur,
            false,
            false,
            0,
            0
        );

        boot86_set_image(
            s_boot86_ring,
            true,
            dial_opacity,
            dial_angle_10
        );

        return false;
    }

    /*
     * 退出完成，回到纯黑。
     */
    boot86_set_logo_pair(
        s_boot86_logo_sharp,
        s_boot86_logo_blur,
        false,
        false,
        0,
        0
    );

    boot86_set_logo_pair(
        s_boot86_gr_sharp,
        s_boot86_gr_blur,
        false,
        false,
        0,
        0
    );

    boot86_set_image(
        s_boot86_ring,
        false,
        0,
        0
    );

    return elapsed_ms >= BOOT86_TOTAL_MS;
}

void ui_boot86_start(void)
{
    if (!ui_ScreenPageBoot86) {
        ui_ScreenPageBoot86_screen_init();
    }

    ui_boot86_reset();

    s_boot86_started_tick =
        lv_tick_get();

    s_boot86_finished = false;

    ui_boot86_render(0);

    s_boot86_timer =
        lv_timer_create(
            boot86_timer_cb,
            BOOT86_FRAME_MS,
            NULL
        );
}

bool ui_boot86_is_finished(void)
{
    return s_boot86_finished;
}

static void boot86_timer_cb(lv_timer_t *timer)
{
    uint32_t elapsed_ms =
        lv_tick_get() -
        s_boot86_started_tick;

    if (ui_boot86_render(elapsed_ms)) {
        s_boot86_finished = true;
        s_boot86_timer = NULL;
        lv_timer_del(timer);
    }
}

static void boot86_screen_delete_cb(lv_event_t *event)
{
    (void)event;

    if (s_boot86_timer) {
        lv_timer_del(s_boot86_timer);
        s_boot86_timer = NULL;
    }

    s_boot86_ring = NULL;

    s_boot86_logo_sharp = NULL;
    s_boot86_logo_blur = NULL;

    s_boot86_gr_sharp = NULL;
    s_boot86_gr_blur = NULL;

    s_boot86_gr_black = NULL;
    s_boot86_gr_black_trail = NULL;
    s_boot86_gr_red = NULL;
    s_boot86_gr_red_trail = NULL;
    s_boot86_gr_text = NULL;

    ui_ScreenPageBoot86 = NULL;
}
