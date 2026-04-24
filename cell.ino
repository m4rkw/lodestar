// Cell tower info cache + URC parsers.
//
// The cache (cell_mcc, cell_mnc, cell_lac, cell_cid, cell_rat) is kept fresh
// by two mechanisms:
//   1. BG96 registration URCs (+CREG / +CEREG with <n>=2), parsed by
//      gsm_parse_reg_urc().  These fire on roaming, handover, or TAC/LAC
//      change — the trigger we need for a 1nce SIM that can flip operators.
//   2. One-shot AT+QENG="servingcell" query, parsed by
//      gsm_parse_qeng_servingcell().  URCs don't carry MCC/MNC so we use
//      QENG after wake and when a URC signals a change.

// Map a BG96 QENG RAT token to a short canonical code we emit to the server.
// Returns a pointer to a string literal (empty string for unknown tokens) so
// the server can distinguish "RAT not yet known" from "BG96 said GSM/CATM1/..."
static const char *canonical_rat(const char *s, size_t len) {
  if (len == 4 && strncmp(s, "eMTC",  4) == 0) return "CATM1";
  // Some BG96 firmware revisions emit "CAT-M" instead of the documented "eMTC".
  if (len == 5 && strncmp(s, "CAT-M", 5) == 0) return "CATM1";
  if (len == 5 && strncmp(s, "NBIoT", 5) == 0) return "NBIOT";
  if (len == 3 && strncmp(s, "GSM",   3) == 0) return "GSM";
  if (len == 5 && strncmp(s, "WCDMA", 5) == 0) return "WCDMA";
  if (len == 3 && strncmp(s, "LTE",   3) == 0) return "LTE";
  return "";
}

static unsigned long parse_hex_quoted(const char *s) {
  // Expect *s == '"'; read hex digits until the closing quote.
  if (*s != '"') return 0;
  s++;
  unsigned long v = 0;
  while (*s && *s != '"') {
    char c = *s++;
    int d;
    if      (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
    else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
    else return 0;
    v = (v << 4) | d;
  }
  return v;
}

// Parse +CREG or +CEREG URC.  Returns 1 if LAC/CI changed, 0 otherwise.
// Expected form (stat >= 1 with location):
//   +CREG:  <stat>,"<lac>","<ci>"[,<AcT>]
//   +CEREG: <stat>,"<tac>","<ci>"[,<AcT>]
// Stat=0 (not registered) has no location fields — returns 0.
int gsm_parse_reg_urc(const char *reply) {
  const char *p = strstr(reply, "+CEREG:");
  if (!p) p = strstr(reply, "+CREG:");
  if (!p) return 0;

  p = strchr(p, ':');
  if (!p) return 0;
  p++;
  while (*p == ' ') p++;

  // stat is a bare decimal
  int stat = atoi(p);
  if (stat != 1 && stat != 5) return 0;  // not registered (0/2/3/4) — keep cache

  // skip past stat to first comma
  p = strchr(p, ',');
  if (!p) return 0;
  p++;

  // LAC/TAC in quotes
  while (*p == ' ') p++;
  unsigned long new_lac = parse_hex_quoted(p);
  p = strchr(p + 1, '"');
  if (!p) return 0;
  p++;
  if (*p != ',') return 0;
  p++;

  // CI in quotes
  while (*p == ' ') p++;
  unsigned long new_cid = parse_hex_quoted(p);
  if (new_lac == 0 && new_cid == 0) return 0;

  if (new_lac == cell_lac && new_cid == cell_cid) return 0;
  cell_lac = new_lac;
  cell_cid = new_cid;
  return 1;
}

// Parse AT+QENG="servingcell" response.  BG96 emits two shapes depending on
// RAT; both are a comma-separated list after the "servingcell" prefix:
//   LTE/eMTC/NB-IoT: "servingcell",<state>,<rat>,<is_tdd>,<MCC>,<MNC>,<cellID>,
//                    <PCID>,<EARFCN>,<band>,<UL_BW>,<DL_BW>,<TAC>,<RSRP>,...
//   GSM:             "servingcell",<state>,"GSM",<MCC>,<MNC>,<LAC>,<cellID>,...
// Returns 1 on successful parse.  State of "SEARCH" / missing fields -> 0.
int gsm_parse_qeng_servingcell(const char *reply) {
  const char *p = strstr(reply, "+QENG:");
  if (!p) return 0;
  p = strstr(p, "\"servingcell\"");
  if (!p) return 0;
  p += strlen("\"servingcell\"");
  if (*p != ',') return 0;
  p++;

  // <state> — skip in quotes
  if (*p != '"') return 0;
  const char *state_start = p + 1;
  const char *state_end = strchr(state_start, '"');
  if (!state_end) return 0;
  // bail if not connected (SEARCH, LIMSRV, etc have no cell info)
  size_t state_len = state_end - state_start;
  if (state_len >= 6 && strncmp(state_start, "SEARCH", 6) == 0) return 0;
  p = state_end + 1;
  if (*p != ',') return 0;
  p++;

  // <rat> — quoted
  if (*p != '"') return 0;
  const char *rat_start = p + 1;
  const char *rat_end = strchr(rat_start, '"');
  if (!rat_end) return 0;
  size_t rat_len = rat_end - rat_start;
  int is_gsm = (rat_len == 3 && strncmp(rat_start, "GSM", 3) == 0);
  const char *new_rat = canonical_rat(rat_start, rat_len);
  p = rat_end + 1;
  if (*p != ',') return 0;
  p++;

  int new_mcc, new_mnc;
  unsigned long new_lac, new_cid;

  if (is_gsm) {
    // GSM: MCC,MNC,LAC(hex),cellID(hex)
    new_mcc = atoi(p);
    p = strchr(p, ','); if (!p) return 0; p++;
    new_mnc = atoi(p);
    p = strchr(p, ','); if (!p) return 0; p++;
    new_lac = strtoul(p, NULL, 16);
    p = strchr(p, ','); if (!p) return 0; p++;
    new_cid = strtoul(p, NULL, 16);
  } else {
    // LTE/eMTC/NB-IoT: skip <is_tdd>, then MCC,MNC,cellID(hex),PCID,EARFCN,
    // band,UL_BW,DL_BW,TAC(hex)
    p = strchr(p, ','); if (!p) return 0; p++;  // skip <is_tdd>
    new_mcc = atoi(p);
    p = strchr(p, ','); if (!p) return 0; p++;
    new_mnc = atoi(p);
    p = strchr(p, ','); if (!p) return 0; p++;
    new_cid = strtoul(p, NULL, 16);
    // from cellID, skip PCID,EARFCN,band,UL_BW,DL_BW to reach TAC
    for (int i = 0; i < 6; i++) {
      p = strchr(p, ','); if (!p) return 0; p++;
    }
    new_lac = strtoul(p, NULL, 16);
  }

  // Numeric sentinels: BG96 emits MCC/MNC=65535, cellID=0xFFFFFFFF, TAC/LAC
  // =0xFFFF when it has a serving radio but hasn't decoded SIB1 yet (seen in
  // CONNECT state on Cat-M1 during initial attach).  Treat as "cell unknown"
  // — we still want to propagate the RAT, just without numeric cell data.
  int numeric_unknown = (new_mcc == 0 || new_mcc > 999 || new_mnc > 999 ||
                         new_cid == 0xFFFFFFFFUL || new_lac == 0xFFFFUL ||
                         (new_lac == 0 && new_cid == 0));
  if (numeric_unknown) {
    new_mcc = 0;
    new_mnc = 0;
    new_lac = 0;
    new_cid = 0;
  }

  // Nothing worth caching if we have neither a known RAT nor numeric cell info.
  if (numeric_unknown && new_rat[0] == '\0') return 0;

  if (new_mcc == cell_mcc && new_mnc == cell_mnc &&
      new_lac == cell_lac && new_cid == cell_cid &&
      strcmp(new_rat, cell_rat) == 0) {
    return 0;  // no-op — caller can skip marking dirty
  }
  cell_mcc = new_mcc;
  cell_mnc = new_mnc;
  cell_lac = new_lac;
  cell_cid = new_cid;
  // cell_rat is a fixed-size buffer; canonical_rat returns literals <= 5 chars.
  strcpy(cell_rat, new_rat);
  return 1;
}

// Time-based refresh: BG96 CREG/CEREG URCs often only fire on TAC change or
// stat transition, not on cell (CID) handover within the same TAC.  On a long
// drive that stays in one TAC we'd otherwise never refresh, and the server
// sees the starting cell only.  Poll QENG every CELL_REFRESH_INTERVAL_MS while
// awake as a backstop.
#ifndef CELL_REFRESH_INTERVAL_MS
#define CELL_REFRESH_INTERVAL_MS 30000UL
#endif

// Returns 1 if gsm_refresh_cell_info() should be called now.
// now_ms/last_ms are uint32_t to match millis() width on the STM32 — using
// unsigned long would be 64-bit on the host test and hide wrap-around bugs.
int cell_refresh_due(uint32_t now_ms, uint32_t last_ms, unsigned char stale) {
  if (stale) return 1;
  return ((uint32_t)(now_ms - last_ms) >= CELL_REFRESH_INTERVAL_MS) ? 1 : 0;
}

#ifndef CELL_UNIT_TEST
// Runtime glue — not included in unit tests since it calls the modem.
// Guarded so the Arduino build still compiles this when CELL_UNIT_TEST
// is not defined, but test binaries (which define it) skip these.
byte     cell_info_stale = 1;        // force refresh on first use
uint32_t last_cell_refresh_ms = 0;   // set by gsm_refresh_cell_info()

void gsm_refresh_cell_info() {
  DEBUG_FUNCTION_CALL();
  gsm_port.print("AT+QENG=\"servingcell\"\r");
  gsm_wait_for_reply(1, 1);
  int ok = gsm_parse_qeng_servingcell(modem_reply);
  if (ok) {
    cell_fields_dirty = 1;  // values changed, re-send on next packet
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "QENG parse=%d mcc=%d mnc=%d lac=%lu cid=%lu rat=%s",
           ok, cell_mcc, cell_mnc, cell_lac, cell_cid, cell_rat);
  debug_print(buf);
  cell_info_stale = 0;                       // clear regardless — we queried
  last_cell_refresh_ms = (uint32_t)millis(); // restart the time-based trigger
}
#endif
