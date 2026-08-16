#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "device_sleep.h"
#include "device_state.h"
#include "device_axp2101.h"
#include "device_flush_gate.h"
#include "device_step_source.h"
#include "device_familiar.h"
#include "display_sleep.h"
#include "power_button.h"
#include "battery_source.h"
#include "nvs.h"
#include <time.h>

/*
 * Manual light sleep for the dark hours — the full-day-battery lever.
 *
 * Deliberately NOT esp_pm automatic light sleep (tickless idle): tickless
 * measurably drops panel SPI flushes on this board even while fully awake
 * with every pm lock held (~3-5 failed flushes/second during rendering,
 * A/B-verified against the DFS-only build), and automatic sleep can engage
 * anywhere the locks allow, which twice ended in display corruption. Here
 * sleep is an explicit, owned loop that runs only when the state machine
 * says DOZING on battery, with rendering sealed first — so a light sleep
 * can never cut an in-flight SPI flush or I2C transaction, and the awake
 * system stays bit-identical to the verified DFS-only firmware.
 *
 * Loop shape: sleep in ~1s quanta — the accelerometer keeps sampling into
 * its on-die FIFO (~4s deep), and each wake drains it in one burst so steps
 * keep counting with nothing lost (counts land <=1s late, by design). The
 * AXP2101 power key is polled every wake (~1s wake latency); BOOT wakes
 * instantly via GPIO low-level wakeup. Slept time is credited back to the
 * FreeRTOS tick with xTaskCatchUpTicks, then a short awake window lets due
 * tasks (housekeeping, NVS persist, idle/TWDT) run.
 *
 * Requires CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=n: with it on, a startup
 * hook arms hardware sleep-sel isolation on every pad and the first sleep
 * entry floats the QSPI panel bus + GPIO13, latching the panel (the frozen
 * corruption of the earlier attempts). With it off, pads hold state.
 */

#define BOOT_BUTTON GPIO_NUM_0
#define QUANTUM_MS 1000
#define PWR_POLL_QUANTA 1
#define CATCH_UP_MS 1000
#define AWAKE_WINDOW_MS 10
#define ENTRY_POLL_MS 500

/*
 * Sleep telemetry: the overnight drain test failed (~8-12%/h) and serial
 * is dead unplugged, so the loop records what actually happened for later
 * reading — dark-loop entries, slept time, wake causes, WHY eligibility
 * was blocked while dozing, and the battery %% consumed per dark session.
 * Persisted to NVS on every loop exit and every ~5min while dark; printed
 * at boot and via the `sleepstats` console command.
 */
typedef struct {
    uint32_t dark_entries;
    uint32_t slept_ms;
    uint32_t quanta;
    uint32_t button_wakes;  /* BOOT pin (instant GPIO wake) */
    uint32_t pwr_wakes;     /* AXP2101 latched short-press flag */
    uint32_t state_exits;
    uint32_t blocked_vbus;  /* DOZING polls refused: vbus said plugged */
    uint32_t blocked_radio; /* DOZING polls refused: BLE session live */
    uint32_t dark_battery_pct; /* cumulative %% consumed inside dark loops */
    uint32_t dark_wall_ms;     /* cumulative wall time inside dark loops */
    /* Boot-reason breadcrumbs (recorded every boot): a nighttime screen
       light-up with no button press is a reboot — these say which kind. */
    uint32_t boots_panic;
    uint32_t boots_wdt;      /* interrupt/task watchdog */
    uint32_t boots_brownout;
    uint32_t boots_sw;       /* esp_restart (self-heal) */
    uint32_t boots_poweron;
    uint32_t boots_other;
} sleep_stats_t;

static sleep_stats_t stats;

/*
 * Event journal: a timestamped ring of the moments that matter for field
 * mysteries (boots with reason, wakes with cause, dark entries), persisted
 * beside the stats and printed by `sleepstats`. Answers "WHEN did the
 * screen light up and why" after a night nobody watched.
 */
typedef struct {
    uint32_t epoch;
    uint8_t code;
} journal_entry_t;

#define JOURNAL_CAPACITY 48
#define JOURNAL_BOOT_BASE 0x10 /* 0x10 + esp_reset_reason */
#define JOURNAL_DARK_ENTER 1
#define JOURNAL_WAKE_BOOT 2
#define JOURNAL_WAKE_PWR 3
#define JOURNAL_EXIT_STATE 4

static journal_entry_t journal[JOURNAL_CAPACITY];
static uint32_t journal_next;

static void journal_note(uint8_t code)
{
    journal[journal_next % JOURNAL_CAPACITY] =
        (journal_entry_t){.epoch = (uint32_t)time(NULL), .code = code};
    journal_next++;
}

static const char *journal_name(uint8_t code)
{
    switch (code) {
    case JOURNAL_DARK_ENTER: return "dark-enter";
    case JOURNAL_WAKE_BOOT: return "wake-BOOT";
    case JOURNAL_WAKE_PWR: return "wake-PWR";
    case JOURNAL_EXIT_STATE: return "exit-state/usb";
    }
    if (code >= JOURNAL_BOOT_BASE) {
        switch (code - JOURNAL_BOOT_BASE) {
        case 1: return "boot-poweron";
        case 3: return "boot-sw";
        case 4: return "boot-PANIC";
        case 5: return "boot-IWDT";
        case 6: return "boot-TWDT";
        case 7: return "boot-WDT";
        case 9: return "boot-brownout";
        default: return "boot-other";
        }
    }
    return "?";
}

static void stats_persist(void)
{
    nvs_handle_t handle;
    if (nvs_open("slpstat", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_blob(handle, "v1", &stats, sizeof(stats));
    nvs_set_blob(handle, "j1", journal, sizeof(journal));
    nvs_set_u32(handle, "jn", journal_next);
    nvs_commit(handle);
    nvs_close(handle);
}

static void stats_restore(void)
{
    nvs_handle_t handle;
    if (nvs_open("slpstat", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = sizeof(stats);
    nvs_get_blob(handle, "v1", &stats, &size);
    size = sizeof(journal);
    nvs_get_blob(handle, "j1", journal, &size);
    nvs_get_u32(handle, "jn", &journal_next);
    nvs_close(handle);
}

void device_sleep_stats_print(void)
{
    uint32_t stored = journal_next < JOURNAL_CAPACITY ? journal_next : JOURNAL_CAPACITY;
    for (uint32_t i = journal_next - stored; i < journal_next; i++) {
        journal_entry_t *entry = &journal[i % JOURNAL_CAPACITY];
        struct tm stamp;
        time_t epoch = (time_t)entry->epoch;
        localtime_r(&epoch, &stamp);
        printf("sleepstats: @%02d:%02d:%02d %s\n", stamp.tm_hour,
               stamp.tm_min, stamp.tm_sec, journal_name(entry->code));
    }
    printf("sleepstats: boots panic=%lu wdt(iwdt*10000+twdt*100+wdt)=%lu brownout=%lu sw=%lu poweron=%lu other=%lu (this boot: %d)\n",
           (unsigned long)stats.boots_panic, (unsigned long)stats.boots_wdt,
           (unsigned long)stats.boots_brownout, (unsigned long)stats.boots_sw,
           (unsigned long)stats.boots_poweron, (unsigned long)stats.boots_other,
           (int)esp_reset_reason());
    uint32_t dark_min = stats.dark_wall_ms / 60000;
    uint32_t slept_min = stats.slept_ms / 60000;
    printf("sleepstats: entries=%lu slept=%lumin dark=%lumin quanta=%lu "
           "wakes(boot=%lu pwr=%lu state=%lu) blocked(vbus=%lu radio=%lu) "
           "dark_drain=%lu%%\n",
           (unsigned long)stats.dark_entries, (unsigned long)slept_min,
           (unsigned long)dark_min, (unsigned long)stats.quanta,
           (unsigned long)stats.button_wakes, (unsigned long)stats.pwr_wakes,
           (unsigned long)stats.state_exits,
           (unsigned long)stats.blocked_vbus, (unsigned long)stats.blocked_radio,
           (unsigned long)stats.dark_battery_pct);
    if (dark_min > 0) {
        printf("sleepstats: duty=%lu%% (slept/dark), drain=%lu%%/h while dark\n",
               (unsigned long)(stats.slept_ms / (stats.dark_wall_ms / 100)),
               (unsigned long)(stats.dark_battery_pct * 60 / (dark_min ? dark_min : 1)));
    }
}

void device_sleep_stats_reset(void)
{
    memset(&stats, 0, sizeof(stats));
    memset(journal, 0, sizeof(journal));
    journal_next = 0;
    stats_persist();
    printf("sleepstats: reset\n");
}

static void wake_display_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    display_sleep_wake();
}

/* The wake render must run on the LVGL task. */
static void schedule_display_wake(void)
{
    bsp_display_lock(0);
    lv_timer_t *wake_timer = lv_timer_create(wake_display_cb, 5, NULL);
    lv_timer_set_repeat_count(wake_timer, 1);
    bsp_display_unlock();
}

static bool sleep_eligible(void)
{
    /* BLE check: manual light sleep freezes the BLE controller, so a live
       session (central connected) blocks sleep; the BLE-follows-the-screen
       policy tears the session down shortly after DOZING lands. */
#ifdef FROLIC_SLEEP_ON_VBUS
    /* Bench: sleep even on USB power so the dark loop runs with the
       flasher attached (each WDT reboot re-enumerates USB for capture). */
    return device_state_get() == DEVICE_STATE_DOZING &&
           !device_familiar_central_connected();
#else
    return device_state_get() == DEVICE_STATE_DOZING &&
           !device_familiar_central_connected() &&
           !axp2101_vbus_present();
#endif
}

static void catch_up(int64_t *credit_us)
{
    if (*credit_us <= 0) return;
    xTaskCatchUpTicks(pdMS_TO_TICKS((uint32_t)(*credit_us / 1000)));
    *credit_us = 0;
}

static void sleep_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(ENTRY_POLL_MS));
        if (!sleep_eligible()) {
            /* While dozing, tally WHY sleep is refused — the counters
               distinguish "loop never engaged" from "engaged, still hot". */
            if (device_state_get() == DEVICE_STATE_DOZING) {
                if (device_familiar_central_connected()) stats.blocked_radio++;
                else if (axp2101_vbus_present()) stats.blocked_vbus++;
            }
            continue;
        }

        /* Seal rendering: no flush can start once the gate is closed, so a
           light sleep can never freeze the panel bus mid-transfer. The close
           handshake renders + drains via the LVGL task first. */
        device_flush_gate_close();
        if (!sleep_eligible()) {
            /* Lost the race (woke / BLE session / USB): hand the gate back. */
            device_flush_gate_open();
            continue;
        }

        /* BOOT wakes the chip out of light sleep instantly. Re-armed every
           entry: gpio_config() elsewhere resets the pin's trigger type. */
        gpio_wakeup_enable(BOOT_BUTTON, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();

        printf("device_sleep: dark loop begin\n");
        step_source_external_pacing(true);
        stats.dark_entries++;
        journal_note(JOURNAL_DARK_ENTER);
        int entry_battery = battery_source_percent();
        int64_t entry_wall_us = esp_timer_get_time();
        int64_t last_persist_us = entry_wall_us;
        int64_t credit_us = 0;
        uint32_t quantum = 0;
        bool wake_display = false;

        while (sleep_eligible()) {
            esp_sleep_enable_timer_wakeup(QUANTUM_MS * 1000);
            int64_t before = esp_timer_get_time();
            esp_light_sleep_start();
            int64_t slept_us = esp_timer_get_time() - before;
            credit_us += slept_us;
            stats.slept_ms += (uint32_t)(slept_us / 1000);
            stats.quanta++;

            /* Drain the accel FIFO every wake: steps keep counting through
               the night. Doubles as the I2C quiesce point — it serializes
               behind any transaction a briefly-scheduled task left in
               flight. */
            step_source_drain_now();

            if (gpio_get_level(BOOT_BUTTON) == 0) {
                stats.button_wakes++;
                journal_note(JOURNAL_WAKE_BOOT);
                wake_display = true;
                break;
            }
            if (++quantum % PWR_POLL_QUANTA == 0 && power_button_pressed()) {
                /* Counted separately from BOOT: an overnight pwr_wake with
                   nobody pressing anything is a phantom latched flag or a
                   glitched post-sleep I2C read. */
                stats.pwr_wakes++;
                journal_note(JOURNAL_WAKE_PWR);
                wake_display = true;
                break;
            }
            if (credit_us >= CATCH_UP_MS * 1000) {
                /* Credit the slept time to the RTOS tick so wall-clock task
                   scheduling (OTA windows, watchdogs, persist) stays honest,
                   then yield briefly so everything due — and the idle tasks,
                   which feed the task watchdog — gets to run. */
                catch_up(&credit_us);
                vTaskDelay(pdMS_TO_TICKS(AWAKE_WINDOW_MS));
                if (esp_timer_get_time() - last_persist_us > 5LL * 60 * 1000000) {
                    last_persist_us = esp_timer_get_time();
                    int battery_now = battery_source_percent();
                    if (entry_battery > 0 && battery_now > 0 &&
                        battery_now < entry_battery) {
                        stats.dark_battery_pct += entry_battery - battery_now;
                        entry_battery = battery_now;
                    }
                    stats.dark_wall_ms +=
                        (uint32_t)((esp_timer_get_time() - entry_wall_us) / 1000);
                    entry_wall_us = esp_timer_get_time();
                    stats_persist();
                }
            }
        }

        catch_up(&credit_us);
        step_source_external_pacing(false);

        if (!wake_display) {
            stats.state_exits++;
            journal_note(JOURNAL_EXIT_STATE);
        }
        int exit_battery = battery_source_percent();
        if (entry_battery > 0 && exit_battery > 0 && exit_battery < entry_battery) {
            stats.dark_battery_pct += entry_battery - exit_battery;
        }
        stats.dark_wall_ms += (uint32_t)((esp_timer_get_time() - entry_wall_us) / 1000);
        stats_persist();
        printf("device_sleep: dark loop end (%s)\n",
               wake_display ? "button wake" : "state/usb");

        /* Reopen the gate BEFORE the wake frame renders. */
        device_flush_gate_open();
        if (wake_display) schedule_display_wake();
    }
}

static void record_boot_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC: stats.boots_panic++; break;
    /* Subtype matters: INT_WDT implicates ISRs (the frozen BLE
       controller), TASK_WDT implicates the awake window starving idle. */
    case ESP_RST_INT_WDT: stats.boots_wdt += 10000; break;
    case ESP_RST_TASK_WDT: stats.boots_wdt += 100; break;
    case ESP_RST_WDT: stats.boots_wdt++; break;
    case ESP_RST_BROWNOUT: stats.boots_brownout++; break;
    case ESP_RST_SW: stats.boots_sw++; break;
    case ESP_RST_POWERON: stats.boots_poweron++; break;
    default: stats.boots_other++; break;
    }
    journal_note((uint8_t)(JOURNAL_BOOT_BASE + esp_reset_reason()));
    stats_persist();
}

void device_sleep_init(void)
{
    stats_restore();
    record_boot_reason();
    device_sleep_stats_print();
    /* Above the LVGL/step tasks: wake handling preempts routine polls, and
       the loop only yields CPU at its own explicit points. */
    xTaskCreate(sleep_task, "sleep", 3072, NULL, 5, NULL);
    printf("device_sleep: manual light sleep armed (DOZING on battery)\n");
}
