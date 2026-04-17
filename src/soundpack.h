// soundpack.h — Runtime sound pack: memory-mapped from flash partition
// Allows swapping engine/hydraulic sounds without recompiling.
// Sound pack binary is written to the "sounds" partition via esptool.
#pragma once

#include <esp_partition.h>
#include <esp_spi_flash.h>
#include <string.h>

// ── Sound pack binary format ──────────────────────────────────
// Offset 0x00: "SNDP" magic (4 bytes)
// Offset 0x04: version (uint16_t) = 1
// Offset 0x06: numSlots (uint16_t)
// Offset 0x08: slot table (numSlots × 48 bytes each)
//   [32 bytes] name (null-terminated, e.g. "idle", "start", "rev")
//   [4 bytes]  dataOffset (from start of data section)
//   [4 bytes]  sampleCount
//   [4 bytes]  sampleRate
//   [4 bytes]  reserved = 0
// After slot table: raw int8_t PCM sample data (contiguous)

#define SNDPACK_MAGIC    0x504E4453   // "SNDP" little-endian
#define SNDPACK_VERSION  1
#define SNDPACK_PART_LABEL "sounds"
#define SNDPACK_PART_SUBTYPE ((esp_partition_subtype_t)0x82)

struct __attribute__((packed)) SndPackHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t numSlots;
};

struct __attribute__((packed)) SndPackSlot {
    char     name[32];
    uint32_t dataOffset;
    uint32_t sampleCount;
    uint32_t sampleRate;
    uint32_t reserved;
};

// ── Sound pack loader ─────────────────────────────────────────
// Memory-maps the sound partition and overrides the _rt_* pointer
// variables (declared in config.h) so the ISR reads from the pack
// instead of the compiled-in defaults.

static spi_flash_mmap_handle_t _sndpackHandle = 0;

bool loadSoundPack() {
    // Find the sound partition
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, SNDPACK_PART_SUBTYPE, SNDPACK_PART_LABEL);

    if (!part) {
        Serial.println("SoundPack: no partition found — using compiled defaults");
        return false;
    }

    // Memory-map the entire partition (read-only, zero RAM cost)
    const void* mapped = nullptr;
    esp_err_t err = esp_partition_mmap(
        part, 0, part->size, SPI_FLASH_MMAP_DATA, &mapped, &_sndpackHandle);
    if (err != ESP_OK) {
        Serial.printf("SoundPack: mmap failed (err %d) — using defaults\n", err);
        return false;
    }

    const uint8_t* base = (const uint8_t*)mapped;
    const SndPackHeader* hdr = (const SndPackHeader*)base;

    // Validate header
    if (hdr->magic != SNDPACK_MAGIC) {
        Serial.println("SoundPack: empty/invalid — using compiled defaults");
        return false;
    }
    if (hdr->version != SNDPACK_VERSION) {
        Serial.printf("SoundPack: unknown version %d — using defaults\n", hdr->version);
        return false;
    }
    if (hdr->numSlots == 0 || hdr->numSlots > 64) {
        Serial.println("SoundPack: bad slot count — using defaults");
        return false;
    }

    // Parse slot table
    const SndPackSlot* slotTable = (const SndPackSlot*)(base + sizeof(SndPackHeader));
    const int8_t* dataBase = (const int8_t*)(base + sizeof(SndPackHeader) +
                              hdr->numSlots * sizeof(SndPackSlot));

    Serial.printf("SoundPack: found %d slots\n", hdr->numSlots);

    for (uint16_t i = 0; i < hdr->numSlots; i++) {
        const SndPackSlot* s = &slotTable[i];
        const int8_t* sData = dataBase + s->dataOffset;
        uint32_t sCount = s->sampleCount;
        uint32_t sRate  = s->sampleRate;

        // Match by name and redirect the runtime pointers
        #define LOAD_SLOT(N, PTR, CNT, RATE) \
            if (strcmp(s->name, N) == 0) { PTR = sData; CNT = sCount; if(sRate) RATE = sRate; }
        #define LOAD_SLOT2(N, PTR, CNT) \
            if (strcmp(s->name, N) == 0) { PTR = sData; CNT = sCount; }

        LOAD_SLOT ("start",         _rt_startSamples,       _rt_startSampleCount,       _rt_startSampleRate)
        LOAD_SLOT ("idle",          _rt_idleSamples,        _rt_idleSampleCount,        _rt_idleSampleRate)
        LOAD_SLOT2("rev",           _rt_revSamples,         _rt_revSampleCount)
        LOAD_SLOT2("knock",         _rt_knockSamples,       _rt_knockSampleCount)
        LOAD_SLOT2("turbo",         _rt_turboSamples,       _rt_turboSampleCount)
        LOAD_SLOT2("fan",           _rt_fanSamples,         _rt_fanSampleCount)
        LOAD_SLOT2("charger",       _rt_chargerSamples,     _rt_chargerSampleCount)
        LOAD_SLOT2("wastegate",     _rt_wastegateSamples,   _rt_wastegateSampleCount)
        LOAD_SLOT2("horn",          _rt_hornSamples,        _rt_hornSampleCount)
        LOAD_SLOT2("siren",         _rt_sirenSamples,       _rt_sirenSampleCount)
        LOAD_SLOT2("brake",         _rt_brakeSamples,       _rt_brakeSampleCount)
        LOAD_SLOT2("shifting",      _rt_shiftingSamples,    _rt_shiftingSampleCount)
        LOAD_SLOT2("sound1",        _rt_sound1Samples,      _rt_sound1SampleCount)
        LOAD_SLOT2("reversing",     _rt_reversingSamples,   _rt_reversingSampleCount)
        LOAD_SLOT2("indicator",     _rt_indicatorSamples,   _rt_indicatorSampleCount)
        LOAD_SLOT2("coupling",      _rt_couplingSamples,    _rt_couplingSampleCount)
        LOAD_SLOT2("uncoupling",    _rt_uncouplingSamples,  _rt_uncouplingSampleCount)
        LOAD_SLOT2("hydraulicPump", _rt_hydraulicPumpSamples, _rt_hydraulicPumpSampleCount)
        LOAD_SLOT2("hydraulicFlow", _rt_hydraulicFlowSamples, _rt_hydraulicFlowSampleCount)
        LOAD_SLOT2("trackRattle",   _rt_trackRattleSamples, _rt_trackRattleSampleCount)
        LOAD_SLOT2("bucketRattle",  _rt_bucketRattleSamples, _rt_bucketRattleSampleCount)

        #undef LOAD_SLOT
        #undef LOAD_SLOT2

        Serial.printf("  [%d] %s: %u samples @ %uHz\n", i, s->name, sCount, sRate);
    }

    // Recalculate timer intervals with potentially new sample rates
    extern uint32_t maxSampleInterval;
    extern uint32_t minSampleInterval;
    extern uint32_t MAX_RPM_PERCENTAGE;
    maxSampleInterval = 4000000 / _rt_idleSampleRate;
    minSampleInterval = 4000000 / _rt_idleSampleRate * 100 / MAX_RPM_PERCENTAGE;

    Serial.println("SoundPack: loaded successfully!");
    return true;
}
