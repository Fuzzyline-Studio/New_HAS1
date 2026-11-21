#include "HAS1_altar.h"
#include <esp_log.h>
#include <lwip/sockets.h>

TelnetSerial SerialMirror;

static int telnetVprintf(const char *fmt, va_list args) {
    char buf[512];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        size_t n = (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1;
        SerialMirror.write((const uint8_t *)buf, n);
    }
    return len;
}

// 바이트 단위 링 버퍼 적재 — '\n'마다 한 줄로 확정
void TelnetSerial::_logByte(uint8_t c) {
    if (c == '\r') return;
    if (c == '\n') {
        _log_cur[_log_cur_pos] = '\0';
        int slot = (_log_head + _log_count) % LOG_LINES;
        strncpy(_log_lines[slot], _log_cur, LOG_LINE_LEN - 1);
        _log_lines[slot][LOG_LINE_LEN - 1] = '\0';
        if (_log_count < LOG_LINES) {
            _log_count++;
        } else {
            _log_head = (_log_head + 1) % LOG_LINES;  // 가장 오래된 줄 덮어쓰기
        }
        _log_cur_pos = 0;
        _log_cur[0]  = '\0';
    } else {
        if (_log_cur_pos < LOG_LINE_LEN - 1) {
            _log_cur[_log_cur_pos++] = (char)c;
        }
    }
}

size_t TelnetSerial::write(uint8_t c) {
    _uart.write(c);
    _logByte(c);
    if (_client && _client.connected()) {
        _client.write(c);
    }
    return 1;
}

size_t TelnetSerial::write(const uint8_t *buf, size_t size) {
    _uart.write(buf, size);
    for (size_t i = 0; i < size; i++) _logByte(buf[i]);
    if (_client && _client.connected()) {
        _client.write(buf, size);
    }
    return size;
}

void TelnetSerial::telnetBegin() {
    _server.begin();
    _server.setNoDelay(true);
    esp_log_set_vprintf(telnetVprintf);
    Serial.println("[Telnet] Server ready on port 23");
}

void TelnetSerial::telnetLoop() {
    if (_server.hasClient()) {
        if (_client && _client.connected()) {
            _client.stop();
        }
        _client = _server.available();
        _client.setNoDelay(true);

        // 재접속 때마다 현재 정보를 출력하여 부팅 로그가 밀려나도 버전을 확인한다.
        // generator/duct와 동일하게 느린 진단 클라이언트가 타이머를 지연시키지 않도록
        // 한 번만 논블로킹 전송을 시도한다. 전송 실패 시 다시 접속해 확인할 수 있다.
        const int socketFd = _client.fd();
        if (socketFd >= 0) {
            const char *deviceName = my["device_name"].as<const char *>();
            char buffer[448];
            int length = snprintf(
                buffer, sizeof(buffer),
                "[diagnostics] firmware=%u device=%.18s uptime_ms=%lu",
                static_cast<unsigned>(FIRMWARE_VER), deviceName ? deviceName : "",
                static_cast<unsigned long>(millis()));
            // 블랙박스 상태(BLACKBOX.md): USB 없이도 기록/업로드가 되는지 확인한다.
            if (length > 0 && static_cast<size_t>(length) < sizeof(buffer)) {
                const int extra = BlackBoxDiagnosticsText(buffer + length, sizeof(buffer) - length);
                length = (extra > 0 && static_cast<size_t>(extra) < sizeof(buffer) - length)
                             ? length + extra : -1;
            }
            if (length > 0 && static_cast<size_t>(length) + 2 < sizeof(buffer)) {
                buffer[length++] = '\r';
                buffer[length++] = '\n';
                (void)send(socketFd, buffer, static_cast<size_t>(length), MSG_DONTWAIT);
            }
        }

        // 현재 진단 정보 다음에 접속 전 출력된 로그 재전송 (flush 포함)
        if (_log_count > 0) {
            _client.println("=== log replay (" + String(_log_count) + " lines) ===");
            for (int i = 0; i < _log_count; i++) {
                int idx = (_log_head + i) % LOG_LINES;
                _client.println(_log_lines[idx]);
                _client.flush();
            }
            _client.println("==================");
            _client.flush();
        }

        // replay 이후에 찍어야 링 버퍼에 중복 안 들어감
        Serial.println("[Telnet] Client connected: " + _client.remoteIP().toString());
    }
    if (_client && !_client.connected()) {
        _client.stop();
    }
}

void TelnetInit() {
    SerialMirror.telnetBegin();
}

void TelnetLoop() {
    SerialMirror.telnetLoop();
}
