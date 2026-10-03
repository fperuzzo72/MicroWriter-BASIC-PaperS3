#include "ota_apps.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>
#include <spi_flash_mmap.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kNamespace = "ota_names";

// "ota_0".."ota_15"
void slotKey(int slot, char* out, size_t outSize) { snprintf(out, outSize, "ota_%d", slot); }

int slotOf(const esp_partition_t* p) { return p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0; }

bool isOtaAppSlot(const esp_partition_t* p) {
  return p && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 && p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15;
}

// True when `p` is the reader's slot (ota_0, CrossPoint's) and holds a
// readable app image. A slot with no valid descriptor (never flashed, or
// flashed with garbage) fails the read and is correctly rejected.
//
// This used to be "a different project_name than ours", which also hid
// RetroComputer in app2: the owner wants this menu to offer the reader only,
// and RetroComputer is reached through the reader's Home. But every app on
// this unit now builds as "arduino-lib-builder" (CrossPoint too, since 1.6.5),
// so the name test hid the reader as well and READER found nothing. No field
// of the descriptor tells the apps apart; the slot does.
bool holdsReader(const esp_partition_t* p) {
  if (!p || p->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0) return false;
  esp_app_desc_t theirs = {};
  return esp_ota_get_partition_description(p, &theirs) == ESP_OK;
}

// --- otadata, written by hand ------------------------------------------------
// Layout reference: esp_flash_partitions.h. The CRC covers ota_seq only.

struct __attribute__((packed)) SelectEntry {
  uint32_t ota_seq;
  uint8_t seq_label[20];
  uint32_t ota_state;
  uint32_t crc;
};
static_assert(sizeof(SelectEntry) == 32, "SelectEntry must be 32 bytes");

constexpr uint32_t kOtaImgNew = 0;      // ESP_OTA_IMG_NEW
constexpr uint32_t kOtaImgValid = 2;    // ESP_OTA_IMG_VALID
constexpr uint32_t kOtaImgInvalid = 3;  // ESP_OTA_IMG_INVALID
constexpr uint32_t kOtaImgAborted = 4;  // ESP_OTA_IMG_ABORTED

uint32_t computeSeqCrc(uint32_t seq) {
  return esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&seq), sizeof(seq));
}

const esp_partition_t* findOtadata() {
  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata || otadata->size < 2 * SPI_FLASH_SEC_SIZE) return nullptr;
  return otadata;
}

// Point the bootloader at `dest` by writing a fresh otadata entry into the
// inactive otadata sector. Bypasses esp_ota_set_boot_partition()'s
// esp_image_verify(), which fails on this silicon for reasons that have
// nothing to do with the image being valid.
bool switchTo(const esp_partition_t* dest) {
  if (!dest) return false;
  const esp_partition_t* otadata = findOtadata();
  if (!otadata) {
    Serial.println("[ota] otadata partition missing or too small");
    return false;
  }

  SelectEntry slots[2] = {};
  if (esp_partition_read(otadata, 0, &slots[0], sizeof(SelectEntry)) != ESP_OK ||
      esp_partition_read(otadata, SPI_FLASH_SEC_SIZE, &slots[1], sizeof(SelectEntry)) != ESP_OK) {
    Serial.println("[ota] otadata read failed");
    return false;
  }

  // The live entry is the one with a valid CRC and the highest sequence,
  // ignoring any the bootloader has already written off.
  int activeIdx = -1;
  uint32_t activeSeq = 0;
  for (int i = 0; i < 2; ++i) {
    if (slots[i].ota_seq == 0xFFFFFFFFu) continue;
    if (slots[i].crc != computeSeqCrc(slots[i].ota_seq)) continue;
    if (slots[i].ota_state == kOtaImgInvalid || slots[i].ota_state == kOtaImgAborted) continue;
    if (activeIdx < 0 || slots[i].ota_seq > activeSeq) {
      activeIdx = i;
      activeSeq = slots[i].ota_seq;
    }
  }

  const uint32_t destOtaIdx =
      static_cast<uint32_t>(dest->subtype) - static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (destOtaIdx > 15) {
    Serial.printf("[ota] destination is not an OTA app partition (subtype=0x%02X)\n", dest->subtype);
    return false;
  }

  // ota_seq encoding: (seq - 1) % <number of OTA partitions> selects the slot.
  // Counted, not assumed: the table has three since 2026-09-30 (CrossPoint,
  // this, RetroComputer; see docs/DUAL_BOOT.md), and a hard-coded 2 picked
  // the wrong app.
  uint32_t otaCount = 0;
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  for (; it; it = esp_partition_next(it)) {
    if (isOtaAppSlot(esp_partition_get(it))) ++otaCount;
  }
  esp_partition_iterator_release(it);
  if (destOtaIdx >= otaCount) {
    Serial.printf("[ota] ota_%u is past the %u OTA slots\n", static_cast<unsigned>(destOtaIdx),
                  static_cast<unsigned>(otaCount));
    return false;
  }
  uint32_t newSeq = activeSeq + 1;
  while (((newSeq - 1u) % otaCount) != destOtaIdx) ++newSeq;

  SelectEntry next = {};
  next.ota_seq = newSeq;
  memset(next.seq_label, 0xFF, sizeof(next.seq_label));
  // Written straight to VALID rather than NEW. NEW is right for a real
  // self-update, where the bootloader's rollback net should cover code that
  // has never run -- but this only ever points at an already-flashed sibling
  // that was working the last time it booted. Left as NEW, the first reset
  // before the app marks itself valid (and neither app calls
  // esp_ota_mark_app_valid_cancel_rollback()) silently rolls back to the other
  // slot, which shows up as waking from sleep in the app you just left.
  // CrossPoint reaches the same end state via confirmLastOtaSwitch(), which
  // rewrites the entry afterwards; there is nothing here to preserve rollback
  // for, so it is written correctly the first time.
  next.ota_state = kOtaImgValid;
  next.crc = computeSeqCrc(next.ota_seq);

  // Always into the *other* sector, so the bootloader sees the higher sequence.
  const int targetSlot = (activeIdx == 0) ? 1 : 0;
  const size_t targetOff = static_cast<size_t>(targetSlot) * SPI_FLASH_SEC_SIZE;

  if (esp_partition_erase_range(otadata, targetOff, SPI_FLASH_SEC_SIZE) != ESP_OK) {
    Serial.printf("[ota] otadata erase failed (sector=%d)\n", targetSlot);
    return false;
  }
  if (esp_partition_write(otadata, targetOff, &next, sizeof(next)) != ESP_OK) {
    Serial.printf("[ota] otadata write failed (sector=%d)\n", targetSlot);
    return false;
  }

  Serial.printf("[ota] otadata sector=%d seq=%u -> %s\n", targetSlot, static_cast<unsigned>(newSeq), dest->label);
  return true;
}

}  // namespace

void registerOtaAppName(const char* name) {
  const esp_partition_t* self = esp_ota_get_running_partition();
  if (!isOtaAppSlot(self)) return;

  char key[8];
  slotKey(slotOf(self), key, sizeof(key));

  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putString(key, name);
  prefs.end();
}

int detectOtaApps(OtaAppEntry* apps, int maxApps) {
  int count = 0;
  const esp_partition_t* running = esp_ota_get_running_partition();

  Preferences prefs;
  const bool prefsOpen = prefs.begin(kNamespace, true);

  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it && count < maxApps) {
    const esp_partition_t* p = esp_partition_get(it);
    if (p && p != running && isOtaAppSlot(p) && holdsReader(p)) {
      const int slot = slotOf(p);
      char key[8];
      slotKey(slot, key, sizeof(key));

      OtaAppEntry& entry = apps[count];
      const String nvsName = prefsOpen ? prefs.getString(key, "") : String();
      if (nvsName.length() > 0) {
        strncpy(entry.name, nvsName.c_str(), sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';
      } else {
        // Never booted with a version that registers a name.
        snprintf(entry.name, sizeof(entry.name), "OTA Slot %d", slot);
      }
      entry.partitionSubtype = p->subtype;
      count++;
    }
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  if (prefsOpen) prefs.end();
  return count;
}

void switchToOtaApp(int partitionSubtype) {
  const esp_partition_t* target =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, static_cast<esp_partition_subtype_t>(partitionSubtype), nullptr);
  if (!target) {
    Serial.printf("[ota] target subtype 0x%02X not found\n", partitionSubtype);
    return;
  }
  if (!switchTo(target)) return;
  Serial.printf("[ota] switching to %s\n", target->label);
  Serial.flush();
  esp_restart();
}
