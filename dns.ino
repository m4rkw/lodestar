// DNS URC parser and cache for BG96 AT+QIDNSGIP.
//
// Resolving once per session and caching the IP avoids a race where QIOPEN
// with a hostname returns "system busy" (error 568) if the modem's internal
// resolver is still in flight when a socket teardown is pending.
//
// AT+QIDNSGIP=1,"<host>" emits two +QIURC shapes:
//   +QIURC: "dnsgip",<err>,<ip_count>,<ttl>      — status URC (skip)
//   +QIURC: "dnsgip","<ipv4>"                    — IP URC (extract)
// A reply may contain both; we pick the first IP URC.

int gsm_parse_dnsgip(const char *reply, char *ip_out, int ip_out_len) {
  const char *p = reply;
  while ((p = strstr(p, "\"dnsgip\",")) != NULL) {
    p += strlen("\"dnsgip\",");
    if (*p != '"') {            // status URC (bare number) — keep looking
      continue;
    }
    p++;                         // past opening quote
    int i = 0;
    while (*p && *p != '"') {
      char c = *p++;
      if (!((c >= '0' && c <= '9') || c == '.')) return 0;
      if (i >= ip_out_len - 1) return 0;
      ip_out[i++] = c;
    }
    if (*p != '"') return 0;    // unterminated
    ip_out[i] = '\0';
    return i > 0;
  }
  return 0;
}
