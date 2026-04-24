
// Alert delivery via UDP.
//
// Alert records use the format: A,priority,message
// Priority: 0=normal, 2=critical (maps to Pushover priority)
// Sent as a small encrypted datagram alongside telemetry.

#define ALERT_QUEUE_SIZE 5
#define ALERT_MSG_SIZE 120

char alert_queue[ALERT_QUEUE_SIZE][ALERT_MSG_SIZE];
int8_t alert_priority[ALERT_QUEUE_SIZE];
int alert_count = 0;

void alert_enqueue(const char *msg, int8_t priority) {
  if (alert_count >= ALERT_QUEUE_SIZE) {
    debug_print(F("alert queue full, dropping"));
    return;
  }
  strlcpy(alert_queue[alert_count], msg, ALERT_MSG_SIZE);
  alert_priority[alert_count] = priority;
  alert_count++;
  debug_print(F("alert queued:"));
  debug_print(msg);
}

// Send all queued alerts. Called after gsm_send_data() succeeds,
// or standalone during lightweight wake (movement alerts).
// Returns 1 if all sent (or none queued), 0 on failure.
int alert_send() {
  if (alert_count == 0) return 1;

  debug_print(F("alert_send(): sending queued alerts"));
  return alert_send_udp();
}

// Standalone alert send (modem must already be awake).
// Ensures connection is up, sends, doesn't manage modem power.
int alert_send_standalone() {
  if (alert_count == 0) return 1;

  gsm_send_at();

  int ipstat = gsm_get_connection_status();
  if (ipstat != 1) {
    if (ipstat != 0) {
      gsm_disconnect();
    }
    gsm_set_apn();
    if (!gsm_connect()) {
      debug_print(F("alert standalone: connect failed"));
      return 0;
    }
  }

  int ret = alert_send();

#if !GSM_STAY_ONLINE
  gsm_disconnect();
#endif

  return ret;
}
