/*
 * Copyright 2012-2013 BrewPi/Elco Jacobs.
 * Copyright 2013 Matthew McGowan.
 * Copyright 2020 Scott Peshak.
 *
 * This file is part of BrewPi.
 *
 * BrewPi is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BrewPi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BrewPi.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Brewpi.h"
#include <ArduinoJson.h>

#include <driver/uart.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <errno.h>
#include <unistd.h>

#include <cstdarg>
#include <cstring>
#include <string>

// -----------------------------------------------------------------------
// Backend interface
// -----------------------------------------------------------------------

/**
 * \brief Abstract I/O backend for PiStream
 *
 * Provides the low-level byte transport that PiStream delegates to.
 * Two concrete implementations exist: UartBackend (serial) and
 * TcpBackend (WiFi / telnet socket).
 */
class PiStreamBackend {
public:
  virtual ~PiStreamBackend() = default;

  /** \brief Read a single byte (non-blocking). Returns -1 if nothing available. */
  virtual int read() = 0;

  /** \brief Return the number of bytes available to read without blocking. */
  virtual int available() = 0;

  /** \brief Write a buffer of bytes. Returns number of bytes written. */
  virtual size_t write(const uint8_t *buf, size_t len) = 0;

  /** \brief Return true if the transport is connected / usable. */
  virtual bool connected() = 0;

  /** \brief One-time initialisation of the transport. */
  virtual void init() = 0;

  /** \brief Return true if the transport is in a valid state. */
  virtual operator bool() = 0;
};

// -----------------------------------------------------------------------
// UART backend  (replaces HardwareSerial / USBCDC)
// -----------------------------------------------------------------------

/**
 * \brief ESP-IDF UART driver backend
 *
 * Wraps the ESP-IDF UART driver so that PiStream can read/write a
 * hardware serial port without any Arduino dependency.
 */
class UartBackend : public PiStreamBackend {
  uart_port_t _port;

public:
  explicit UartBackend(uart_port_t port = UART_NUM_0) : _port(port) {}

  void init() override {
    uart_config_t uart_config = {
        .baud_rate = Config::PiLink::serialSpeed,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };
    // Only configure if not already installed (UART0 is often pre-configured)
    uart_driver_install(_port, 1024, 0, 0, nullptr, 0);
    uart_param_config(_port, &uart_config);
  }

  int read() override {
    uint8_t byte;
    int len = uart_read_bytes(_port, &byte, 1, 0); // non-blocking
    return len > 0 ? byte : -1;
  }

  int available() override {
    size_t avail = 0;
    uart_get_buffered_data_len(_port, &avail);
    return static_cast<int>(avail);
  }

  size_t write(const uint8_t *buf, size_t len) override {
    return uart_write_bytes(_port, buf, len);
  }

  bool connected() override { return true; } // UART is always "connected"
  operator bool() override { return true; }
};

// -----------------------------------------------------------------------
// TCP backend  (replaces WiFiClient)
// -----------------------------------------------------------------------

/**
 * \brief BSD-socket TCP backend
 *
 * Wraps a connected client socket file descriptor so that PiStream can
 * read/write over a TCP (telnet) connection without WiFiClient.
 */
class TcpBackend : public PiStreamBackend {
  int &_fd; // Reference to the external client fd (e.g. telnet_client_fd)

  static bool transientError(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
  }

  void closeIfCurrent(int fd) {
    // The accept loop owns replacements. An error from an earlier operation
    // must not close or clear a client that has since replaced that descriptor.
    if (_fd != fd) return;
    _fd = -1;
    close(fd);
  }

  int receive(int fd, uint8_t &byte, int flags) {
    const int received = recv(fd, &byte, 1, flags);
    if (received == 0 || (received < 0 && !transientError(errno)))
      closeIfCurrent(fd);
    return received;
  }

public:
  /**
   * \brief Construct with a reference to an externally managed socket fd.
   *
   * The fd variable (e.g. telnet_client_fd in ESP_BP_WiFi) is updated by
   * wifi_connect_clients() as clients connect/disconnect.  TcpBackend
   * always reads through the reference so it sees the latest value.
   */
  explicit TcpBackend(int &fd_ref) : _fd(fd_ref) {}

  /** \brief Return the current socket fd (-1 if none). */
  int getFd() const { return _fd; }

  void init() override {} // Server setup is handled externally

  int read() override {
    const int fd = _fd;
    if (fd < 0) return -1;
    uint8_t byte;
    const int n = receive(fd, byte, MSG_DONTWAIT);
    return n > 0 ? byte : -1;
  }

  int available() override {
    const int fd = _fd;
    if (fd < 0) return 0;
    int count = 0;
    if (ioctl(fd, FIONREAD, &count) < 0) {
      if (!transientError(errno)) closeIfCurrent(fd);
      return 0;
    }
    // FIONREAD also returns zero after peer EOF. Probe without consuming data
    // so the regular command-loop poll releases closed clients even when no
    // command follows. Drain queued bytes before treating peer EOF as closed.
    if (count > 0) return count;
    uint8_t byte;
    const int n = receive(fd, byte, MSG_PEEK | MSG_DONTWAIT);
    return n > 0 ? n : 0;
  }

  size_t write(const uint8_t *buf, size_t len) override {
    const int fd = _fd;
    if (fd < 0 || len == 0) return 0;
    const int sent = send(fd, buf, len, MSG_NOSIGNAL);
    if (sent < 0 && !transientError(errno)) closeIfCurrent(fd);
    return sent > 0 ? static_cast<size_t>(sent) : 0;
  }

  bool connected() override {
    const int fd = _fd;
    if (fd < 0) return false;
    // Peek to check whether the peer has closed the connection
    uint8_t byte;
    const int n = receive(fd, byte, MSG_PEEK | MSG_DONTWAIT);
    return n > 0 || (n < 0 && _fd == fd);
  }

  operator bool() override { return _fd >= 0; }

  // New/replacement clients are managed by wifi_connect_clients; terminal I/O
  // failures release the current client here, including the shared descriptor.
};

// -----------------------------------------------------------------------
// PiStream  (non-templated)
// -----------------------------------------------------------------------

/**
 * \brief Abstraction for PiLink I/O
 *
 * Provides buffered printing, printf-style formatting, and JSON
 * serialisation/deserialisation over an arbitrary PiStreamBackend.
 * Replaces the former template class that was parameterised on an
 * Arduino Stream type.
 */
class PiStream {
public:
  /**
   * \brief Constructor
   *
   * \param backend - Reference to the PiStreamBackend used for I/O
   */
  PiStream(PiStreamBackend &backend) : _backend(backend), intBuffOn(0) {
    intBuff[0] = '\0';
  }

  /**
   * \brief One-time initialisation of the underlying transport
   */
  void init() { _backend.init(); }

  // -- reading ----------------------------------------------------------

  /**
   * \brief Read a single byte from the backend (non-blocking)
   */
  int read() { return _backend.read(); }

  /**
   * \brief Read from the backend, waiting if nothing is yet available
   *
   * Polls once per millisecond until data arrives or the timeout expires.
   *
   * \return Data byte, or -1 on timeout
   * \param timeout - How long to wait for data, in milliseconds
   */
  int readPersistent(const int timeout = 10) {
    uint8_t retries = 0;
    while (!available()) {
      vTaskDelay(pdMS_TO_TICKS(1));
      retries++;
      if (retries >= timeout) {
        return -1;
      }
    }
    return _backend.read();
  }

  // -- printing ---------------------------------------------------------

  /**
   * \brief Print a single char into the internal buffer
   */
  void print(const char out) {
    if (static_cast<unsigned>(intBuffOn + 1) < Config::PiLink::intBufferSize()) {
      intBuff[intBuffOn] = out;
      intBuff[intBuffOn + 1] = '\0';
      intBuffOn++;
    }
  }

  /**
   * \brief Print a C-string into the internal buffer
   */
  void print(const char *out) {
    for (uint16_t x = 0; x < strlen(out); x++) {
      if (static_cast<unsigned>(intBuffOn + 1) < Config::PiLink::intBufferSize()) {
        intBuff[intBuffOn] = out[x];
        intBuff[intBuffOn + 1] = '\0';
        intBuffOn++;
      }
    }
  }

  /**
   * \brief Print a C++ string
   */
  void print(const std::string &out) { print(out.c_str()); }

  /**
   * \brief A printf-like interface
   *
   * \param fmt - sprintf format string
   */
  void print_fmt(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(printfBuff, Config::PiLink::printfBufferSize, fmt, args);
    va_end(args);

    print(printfBuff);
  }

  /**
   * \brief Flush the internal buffer with a trailing CRLF
   *
   * Writes the accumulated contents of intBuff followed by "\r\n"
   * to the backend, then resets the buffer.
   */
  void printNewLine() {
    // Append \r\n inside the buffer if there is room, then write once
    size_t len = intBuffOn;
    if (len + 2 < Config::PiLink::intBufferSize()) {
      intBuff[len] = '\r';
      intBuff[len + 1] = '\n';
      intBuff[len + 2] = '\0';
      _backend.write(reinterpret_cast<const uint8_t *>(intBuff), len + 2);
    } else {
      // Buffer full -- write what we have, then the newline separately
      _backend.write(reinterpret_cast<const uint8_t *>(intBuff), len);
      const char crlf[] = "\r\n";
      _backend.write(reinterpret_cast<const uint8_t *>(crlf), 2);
    }
    intBuff[0] = '\0';
    intBuffOn = 0;
  }

  // -- status -----------------------------------------------------------

  /**
   * \brief Check if the transport is connected
   */
  bool connected() { return _backend.connected(); }

  /**
   * \brief Check if data is available for reading
   */
  bool available() { return _backend.available() > 0; }

  /**
   * \brief Bool operator -- true if the backend is in a valid state
   */
  operator bool() { return static_cast<bool>(_backend); }

  // -- JSON helpers -----------------------------------------------------

  /**
   * \brief Send a JSON document with an optional prefix character
   *
   * \param prefix - Character prefix (e.g. 'T' for temperatures).
   *                 If 0/null, no prefix is printed.
   * \param doc - Reference to JsonDocument to serialise
   */
  void sendJsonMessage(const char prefix, const JsonDocument &doc) {
    if (prefix) {
      print(prefix);
      print(':');
    }

    char buf[2048];
    serializeJson(doc, buf, sizeof(buf));
    print(buf);

    printNewLine();
  }

  /**
   * \brief Unpack a single-item array and send as a top-level object
   *
   * \param prefix - Character prefix for the message
   * \param doc - Reference to JsonDocument containing a single-element array
   */
  void sendSingleItemJsonMessage(const char prefix, JsonDocument &doc) {
    JsonDocument shallowDoc;

    JsonObject obj = doc.as<JsonArray>()[0].as<JsonObject>();
    for (auto kvp : obj) {
      shallowDoc[kvp.key()] = kvp.value();
    }

    sendJsonMessage(prefix, shallowDoc);
  }

  /**
   * \brief Parse JSON arriving from the backend
   *
   * Reads bytes into a local buffer (up to newline or timeout), then
   * deserialises from that buffer.
   *
   * \param doc - Reference to a JsonDocument to populate
   */
  void receiveJsonMessage(JsonDocument &doc) {
    // Read bytes until we hit a newline or fill the buffer
    char buf[2048];
    size_t pos = 0;
    const int timeout_ms = 500;
    int idle_ms = 0;

    while (pos < sizeof(buf) - 1) {
      int c = _backend.read();
      if (c < 0) {
        // No data right now -- wait a bit
        vTaskDelay(pdMS_TO_TICKS(1));
        idle_ms++;
        if (idle_ms >= timeout_ms) break;
        continue;
      }
      idle_ms = 0; // reset idle counter on successful read
      if (c == '\n' || c == '\r') break;
      buf[pos++] = static_cast<char>(c);
    }
    buf[pos] = '\0';

    const DeserializationError error = deserializeJson(doc, buf);

    if (error) {
      print("Error deserializing JSON data ");
      print(error.c_str());
      printNewLine();
    }
  }

private:
  /**
   * \brief Reference to the I/O backend
   */
  PiStreamBackend &_backend;

  /**
   * \brief Buffer used for printf operations
   * \see Config::PiLink::printfBufferSize
   */
  inline static char printfBuff[Config::PiLink::printfBufferSize];

  /**
   * \brief Internal line buffer used to accumulate output before flushing
   * \see Config::PiLink::intBufferSize
   */
  char intBuff[Config::PiLink::intBufferSize()];
  uint16_t intBuffOn;
};
