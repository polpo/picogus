#include <string.h>

#include "flash_settings.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

static const Settings defaultSettings = {
    .magic = SETTINGS_MAGIC,
    .version = SETTINGS_VERSION,
    .startupMode = 1,
    .Global = {
        .waveTableVolume = 100
    },
    .Joy = {
        .basePort = 0xffff
    },
    .GUS = {
        .basePort = 0x240,
        .audioBuffer = 4,
        .dmaInterval = 0,
        .force44k = false
    },
    .SB = {
        .basePort = 0x220,
        .oplBasePort = 0x388,
        .oplSpeedSensitive = false
    },
    .MPU = {
        .basePort = 0x330,
        .delaySysex = false,
        .fakeAllNotesOff = false
    },
    .CMS = {
        .basePort = 0x220
    },
    .Tandy = {
        .basePort = 0x2c0
    }
};


#define SETTINGS_SECTOR ((2048 * 1024) - FLASH_SECTOR_SIZE)   //  last sector of a 2M flash (adjust for other flash sizes if needed)
Settings loadSettings(void)
{
    putchar('l');
    Settings settings;
    memcpy(&settings, (uint8_t*)(XIP_BASE + SETTINGS_SECTOR), sizeof(Settings));
    putchar('o');

    if (settings.magic != SETTINGS_MAGIC || settings.version != SETTINGS_VERSION) {
        // Initialize settings to default
        putchar('!');
        return defaultSettings;
    }
    putchar('.');
    return settings;
}

void saveSettings(const Settings* settings)
{
    uint8_t data[FLASH_PAGE_SIZE] = {0};
    static_assert(sizeof(Settings) < FLASH_PAGE_SIZE, "Settings struct doesn't fit inside one flash page");
    memcpy(data, settings, sizeof(Settings));
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_SECTOR, FLASH_SECTOR_SIZE);    // last sector
    flash_range_program(SETTINGS_SECTOR, data, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

void resetSettings(void)
{
    saveSettings(&defaultSettings);
}
