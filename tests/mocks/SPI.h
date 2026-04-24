#ifndef MOCK_SPI_H
#define MOCK_SPI_H

#include <cstdint>
#include <cstring>

#define SPI_MODE1 1
#define SPI_MODE3 3
#define MSBFIRST  1

struct SPISettings {
    uint32_t clock;
    uint8_t  bitOrder;
    uint8_t  mode;
    SPISettings(uint32_t c = 0, uint8_t bo = MSBFIRST, uint8_t m = 0)
        : clock(c), bitOrder(bo), mode(m) {}
};

// Record of SPI transfers for test verification
struct SPITransfer {
    uint8_t tx;
    uint8_t rx;
};

#define SPI_LOG_MAX 64

class SPIClass {
public:
    SPITransfer log[SPI_LOG_MAX];
    int log_count;
    uint8_t rx_queue[SPI_LOG_MAX];  // pre-loaded responses
    int rx_queue_len;
    int rx_queue_pos;

    SPIClass() : log_count(0), rx_queue_len(0), rx_queue_pos(0) {}

    void begin() {}
    void end() {}
    void beginTransaction(SPISettings) {}
    void endTransaction() {}

    uint8_t transfer(uint8_t tx) {
        uint8_t rx = (rx_queue_pos < rx_queue_len) ? rx_queue[rx_queue_pos++] : 0;
        if (log_count < SPI_LOG_MAX) {
            log[log_count].tx = tx;
            log[log_count].rx = rx;
            log_count++;
        }
        return rx;
    }

    void mock_reset() {
        log_count = 0;
        rx_queue_len = 0;
        rx_queue_pos = 0;
    }

    void mock_queue_rx(const uint8_t *data, int len) {
        for (int i = 0; i < len && rx_queue_len < SPI_LOG_MAX; i++)
            rx_queue[rx_queue_len++] = data[i];
    }
};

extern SPIClass SPI;

#endif // MOCK_SPI_H
