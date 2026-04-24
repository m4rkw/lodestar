
// SD card storage
// Settings persistence and telemetry buffering (when SD_ENABLED)

#if SD_ENABLED

#define SD_CONFIG_FILE "config.bin"

byte sd_ready = 0;

void sd_init() {
  DEBUG_FUNCTION_CALL();
  if (SD.begin()) {
    delay(100);  // let SD card settle before file operations
    sd_ready = 1;
    debug_print(F("SD card init OK"));
  } else {
    debug_print(F("SD card init FAILED"));
  }
}

void settings_save_sd() {
  File f = SD.open(SD_CONFIG_FILE, FA_WRITE | FA_CREATE_ALWAYS);
  if (!f) {
    // SD may need re-init (e.g. after sleep power-down)
    debug_print(F("SD: save re-init"));
    BSP_SD_DeInit();
    delay(50);
    if (!SD.begin()) {
      debug_print(F("SD: re-init failed"));
      sd_ready = 0;
      return;
    }
    sd_ready = 1;
    f = SD.open(SD_CONFIG_FILE, FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) {
      debug_print(F("SD: open failed"));
      return;
    }
  }

  byte b[sizeof(settings)];
  memcpy(b, &config, sizeof(settings));
  f.write(b, sizeof(settings));
  f.close();
  debug_print(F("SD: config saved"));
}

int settings_load_sd() {
  if (!sd_ready) return 0;
  if (!SD.exists(SD_CONFIG_FILE)) return 0;
  File f = SD.open(SD_CONFIG_FILE);
  if (!f) return 0;
  if (f.size() != sizeof(settings)) {
    debug_print(F("SD: config size mismatch, ignoring"));
    f.close();
    return 0;
  }
  byte b[sizeof(settings)];
  int n = f.read(b, sizeof(settings));
  f.close();
  if (n != (int)sizeof(settings)) return 0;
  memcpy(&config, b, sizeof(settings));
  debug_print(F("SD: config loaded"));
  return 1;
}

#endif // SD_ENABLED

// --- Telemetry buffering (conditional) ---

#if SD_ENABLED && SD_BUFFERING

#define SD_PENDING_FILE "pending.txt"
#define SD_DRAIN_POS_FILE "drainpos.txt"
#define SD_MAX_PENDING_RECORDS 500

uint32_t sd_drain_pos = 0;
int sd_pending_count = 0;

void storage_save_drain_pos() {
  File f = SD.open(SD_DRAIN_POS_FILE, FA_WRITE | FA_CREATE_ALWAYS);
  if (!f) return;
  char buf[12];
  ltoa(sd_drain_pos, buf, 10);
  f.print(buf);
  f.close();
}

void storage_load_drain_pos() {
  if (!SD.exists(SD_DRAIN_POS_FILE)) return;
  File f = SD.open(SD_DRAIN_POS_FILE);
  if (!f) return;
  char buf[12];
  int i = 0;
  while (f.available() && i < 11) {
    buf[i++] = f.read();
  }
  buf[i] = '\0';
  f.close();
  sd_drain_pos = atol(buf);
}

void storage_init() {
  DEBUG_FUNCTION_CALL();

  if (!sd_ready) return;

  // restore drain position and count pending records from previous run
  if (SD.exists(SD_PENDING_FILE)) {
    storage_load_drain_pos();

    File f = SD.open(SD_PENDING_FILE);
    if (f) {
      // count only unsent records (from drain_pos to end)
      f.seek(sd_drain_pos);
      sd_pending_count = 0;
      while (f.available()) {
        if (f.read() == '\n') sd_pending_count++;
      }
      f.close();
      debug_print(F("SD: pending records to drain:"));
      debug_print(sd_pending_count);
    }
  }
}

void storage_save_current() {
  if (!sd_ready) return;

  // count records in buffer (newlines separate NDJSON records)
  int records = 1;
  for (int i = 0; data_current[i]; i++) {
    if (data_current[i] == '\n') records++;
  }

  if (sd_pending_count + records > SD_MAX_PENDING_RECORDS) {
    debug_print(F("SD: buffer full, dropping record"));
    return;
  }

  File f = SD.open(SD_PENDING_FILE, FA_WRITE | FA_OPEN_APPEND);
  if (!f) {
    debug_print(F("SD: failed to open file for write"));
    return;
  }

  f.println(data_current);
  f.close();
  sd_pending_count += records;

  debug_print(F("SD: saved pending records:"));
  debug_print(records);
}

int storage_has_pending() {
  if (!sd_ready) return 0;
  if (!SD.exists(SD_PENDING_FILE)) return 0;

  File f = SD.open(SD_PENDING_FILE);
  if (!f) return 0;

  uint32_t sz = f.size();
  f.close();

  return (sd_drain_pos < sz);
}

// Append pending SD records to data_current after the current telemetry record.
// Fills remaining space up to DATA_LIMIT, only complete records.
// Returns number of records appended.
int storage_append_pending() {
  if (!sd_ready || !SD.exists(SD_PENDING_FILE)) return 0;

  File f = SD.open(SD_PENDING_FILE);
  if (!f) return 0;

  uint32_t fsize = f.size();
  if (sd_drain_pos >= fsize) {
    f.close();
    return 0;
  }

  f.seek(sd_drain_pos);

  int idx = strlen(data_current);
  int lines = 0;
  int in_record = 0;
  int record_start_idx = idx;
  uint32_t record_start_pos = sd_drain_pos;

  while (f.available()) {
    int c = f.read();
    if (c < 0) break;

    if (c == '\n' || c == '\r') {
      if (in_record) {
        // record complete
        lines++;
        sd_pending_count--;
        sd_drain_pos = f.position();
        record_start_idx = idx;
        record_start_pos = sd_drain_pos;
        in_record = 0;
      }
      continue;
    }

    // starting a new record — add \n separator
    if (!in_record) {
      if (idx + 2 >= DATA_LIMIT) break;
      record_start_idx = idx;
      record_start_pos = f.position() - 1;
      data_current[idx++] = '\n';
      in_record = 1;
    }

    if (idx >= DATA_LIMIT - 1) {
      // overflow mid-record — revert entire record
      idx = record_start_idx;
      sd_drain_pos = record_start_pos;
      break;
    }

    data_current[idx++] = (char)c;
  }

  data_current[idx] = '\0';
  f.close();

  if (lines > 0) {
    storage_save_drain_pos();
  }

  return lines;
}

void storage_finish_drain() {
  SD.remove(SD_PENDING_FILE);
  SD.remove(SD_DRAIN_POS_FILE);
  sd_drain_pos = 0;
  sd_pending_count = 0;
  debug_print(F("SD: pending file cleared"));
}

#endif // SD_ENABLED && SD_BUFFERING
