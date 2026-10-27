#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <notification/notification_messages.h>
#include <stm32wbxx_ll_tim.h>

/* Пульт Beo4 для Bang & Olufsen (27.10.2026): Ouverture в главной комнате, BeoLab 2000 в комнатах-ссылках.
 *
 * У B&O несущая 455 кГц, а приложение «Инфракрасный порт» Флиппера пускает только 10–56 кГц. Поэтому
 * светодиодом управляем сами: ножка ИК (PB9) — это выход TIM1_CH3N, таймер крутит ШИМ 457 кГц
 * (64 МГц / 140), а вспышку открываем и закрываем сменой режима канала, как это делает сама прошивка.
 *
 * Кадр Beo4 (esp32_beo4, ESPHome beo4_protocol): вспышка 200 мкс, значение — время до следующей вспышки:
 * 3125 мкс «0», 6250 «как прошлый бит», 9375 «1», 12500 «стоп», 15625 «старт».
 * Порядок: 0, 0, старт, бит ссылки, 8 бит адреса, 8 бит команды (старшим вперёд), стоп, последняя вспышка.
 * Коды — Beomote (Commands.h): адрес «аудио» 0x01, A.AUX 0x83, RADIO 0x81, CD 0x92 и т. д. */

#define BEO_ARR     139    // 64 МГц / (139 + 1) = 457 кГц
#define BEO_MARK_US 200
#define BEO_T_US    3125
#define BEO_AUDIO   0x01

enum { SymZero = 1, SymSame = 2, SymOne = 3, SymStop = 4, SymStart = 5 };

typedef struct {
    const char* name;
    uint8_t addr, cmd;
    bool link;
} BeoKey;

static const BeoKey keys[] = {
    // 27.10 Иван проверил: вход AUX у Ouverture (туда идёт плата радио — пластинка и волна) — это PHONO 01/93.
    // A.AUX 01/83, V.AUX 01/82, A.AUX с адресом видео 00/83 и с битом ссылки — не отзываются
    {"Vinyl / wave (PHONO)", BEO_AUDIO, 0x93, false},
    {"RADIO", BEO_AUDIO, 0x81, false},
    {"CD", BEO_AUDIO, 0x92, false},
    {"A.TAPE", BEO_AUDIO, 0x91, false},
    {"Volume +", BEO_AUDIO, 0x60, false},
    {"Volume -", BEO_AUDIO, 0x64, false},
    {"GO / play", BEO_AUDIO, 0x35, false},
    {"STOP", BEO_AUDIO, 0x36, false},
    {"Standby (this room)", BEO_AUDIO, 0x0C, false},
    {"ALL off (0F/0C)", 0x0F, 0x0C, false},   // «всем»: как долгое • на Beo4 — гасит и комнаты-ссылки (проверить)
};

typedef struct {
    ViewDispatcher* vd;
    Submenu* menu;
    NotificationApp* notify;
} App;

static void beo_mark(void) {
    LL_TIM_OC_SetMode(TIM1, LL_TIM_CHANNEL_CH3, LL_TIM_OCMODE_PWM2);
    furi_delay_us(BEO_MARK_US);
    LL_TIM_OC_SetMode(TIM1, LL_TIM_CHANNEL_CH3, LL_TIM_OCMODE_FORCED_INACTIVE);
}

static void beo_sym(int sym) {
    beo_mark();
    furi_delay_us(sym * BEO_T_US - BEO_MARK_US);
}

static void beo_send(const BeoKey* k) {
    beo_sym(SymZero);
    beo_sym(SymZero);
    beo_sym(SymStart);
    int prev = k->link;
    beo_sym(k->link ? SymOne : SymZero);
    uint16_t code = ((uint16_t)k->addr << 8) | k->cmd;
    for(int i = 15; i >= 0; i--) {
        int b = (code >> i) & 1;
        beo_sym(b == prev ? SymSame : (b ? SymOne : SymZero));
        prev = b;
    }
    beo_sym(SymStop);
    beo_mark();
}

static bool beo_tim_on(void) {
    if(furi_hal_bus_is_enabled(FuriHalBusTIM1)) return false;   // таймер ИК занят кем-то ещё
    furi_hal_bus_enable(FuriHalBusTIM1);
    LL_TIM_SetPrescaler(TIM1, 0);
    LL_TIM_SetCounterMode(TIM1, LL_TIM_COUNTERMODE_UP);
    LL_TIM_SetAutoReload(TIM1, BEO_ARR);
    // PWM2: выход активен, пока счётчик выше сравнения — светим треть периода
    LL_TIM_OC_SetCompareCH3(TIM1, (BEO_ARR + 1) * 2 / 3);
    LL_TIM_OC_SetMode(TIM1, LL_TIM_CHANNEL_CH3, LL_TIM_OCMODE_FORCED_INACTIVE);
    LL_TIM_OC_SetPolarity(TIM1, LL_TIM_CHANNEL_CH3N, LL_TIM_OCPOLARITY_HIGH);
    LL_TIM_CC_EnableChannel(TIM1, LL_TIM_CHANNEL_CH3N);
    LL_TIM_EnableAllOutputs(TIM1);
    LL_TIM_EnableCounter(TIM1);
    furi_hal_gpio_init_ex(
        &gpio_infrared_tx, GpioModeAltFunctionPushPull, GpioPullNo, GpioSpeedVeryHigh, GpioAltFn1TIM1);
    return true;
}

static void beo_tim_off(void) {
    furi_hal_gpio_init(&gpio_infrared_tx, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    LL_TIM_DisableAllOutputs(TIM1);
    LL_TIM_DisableCounter(TIM1);
    furi_hal_bus_disable(FuriHalBusTIM1);
}

static void on_key(void* ctx, uint32_t index) {
    App* app = ctx;
    notification_message(app->notify, &sequence_blink_magenta_10);
    beo_send(&keys[index]);
}

static bool on_back(void* ctx) {
    App* app = ctx;
    view_dispatcher_stop(app->vd);
    return true;
}

int32_t beo4_remote_app(void* p) {
    UNUSED(p);
    App app = {0};
    app.notify = furi_record_open(RECORD_NOTIFICATION);
    if(!beo_tim_on()) {
        notification_message(app.notify, &sequence_blink_red_100);
        furi_record_close(RECORD_NOTIFICATION);
        return 1;
    }
    Gui* gui = furi_record_open(RECORD_GUI);
    app.vd = view_dispatcher_alloc();
    app.menu = submenu_alloc();
    submenu_set_header(app.menu, "Beo4 -> B&O, 455 kHz");
    for(size_t i = 0; i < COUNT_OF(keys); i++) submenu_add_item(app.menu, keys[i].name, i, on_key, &app);
    view_dispatcher_add_view(app.vd, 0, submenu_get_view(app.menu));
    view_dispatcher_set_event_callback_context(app.vd, &app);
    view_dispatcher_set_navigation_event_callback(app.vd, on_back);
    view_dispatcher_attach_to_gui(app.vd, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app.vd, 0);
    view_dispatcher_run(app.vd);

    view_dispatcher_remove_view(app.vd, 0);
    submenu_free(app.menu);
    view_dispatcher_free(app.vd);
    furi_record_close(RECORD_GUI);
    beo_tim_off();
    furi_record_close(RECORD_NOTIFICATION);
    return 0;
}
