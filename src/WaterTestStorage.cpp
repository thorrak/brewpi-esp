#include "WaterTestStorage.h"
#include "ESPEepromAccess.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unistd.h>
#include "WaterTestCore.h"

namespace WaterTestStorage {
namespace {
uint32_t crcByte(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (unsigned bit = 0; bit < 8; ++bit)
    crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  return crc;
}
bool whitespace(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// ArduinoJson writes the document once, directly into the escaped payload
// string. Neither the payload nor its enclosing JSON exists as a heap copy.
class EnvelopeWriter {
public:
  explicit EnvelopeWriter(FILE *file) : file_(file) {}
  bool raw(const char *data, size_t length) {
    if (!ok_ || length > maximumDocumentBytes - bytes_) {
      ok_ = false;
      return false;
    }
    bytes_ += length;
    for (size_t i = 0; i < length; ++i) {
      buffer_[used_++] = data[i];
      if (used_ == sizeof(buffer_) && !flush())
        return false;
    }
    return true;
  }
  size_t write(uint8_t value) {
    char escaped[6];
    size_t length = 1;
    escaped[0] = static_cast<char>(value);
    if (value == '"' || value == '\\') {
      escaped[0] = '\\';
      escaped[1] = static_cast<char>(value);
      length = 2;
    } else if (value < 0x20) {
      static const char hex[] = "0123456789abcdef";
      memcpy(escaped, "\\u00", 4);
      escaped[4] = hex[value >> 4];
      escaped[5] = hex[value & 15];
      length = 6;
    }
    if (!raw(escaped, length))
      return 0;
    crc_ = crcByte(crc_, value);
    return 1;
  }
  size_t write(const uint8_t *data, size_t length) {
    size_t written = 0;
    while (written < length && write(data[written]))
      ++written;
    return written;
  }
  bool flush() {
    if (!ok_)
      return false;
    if (used_ && fwrite(buffer_, 1, used_, file_) != used_) {
      ok_ = false;
      return false;
    }
    used_ = 0;
    return true;
  }
  uint32_t checksum() const { return ~crc_; }

private:
  FILE *file_;
  char buffer_[256];
  size_t used_ = 0, bytes_ = 0;
  uint32_t crc_ = 0xffffffffU;
  bool ok_ = true;
};

// The on-disk format has always been {"payload":"...","crc32":N}.
// Decode that JSON string incrementally, including legacy JSON escapes, so a
// document can be parsed directly or copied into one caller-owned upload buffer.
class PayloadReader {
public:
  explicit PayloadReader(FILE *file) : file_(file) {
    ok_ = expect('{') && key("payload") && expect(':') && expect('"');
  }
  int read() {
    if (!ok_ || ended_)
      return -1;
    if (pendingAt_ < pendingSize_)
      return emit(pending_[pendingAt_++]);
    int c = encoded();
    if (c == '"') {
      ended_ = true;
      return -1;
    }
    if (c < 0) {
      ok_ = false;
      return -1;
    }
    // ArduinoJson's previous writer leaves some uncommon control bytes raw.
    // Accept them here as its parser did; the CRC still covers exact bytes.
    if (c != '\\')
      return emit(c);
    c = encoded();
    switch (c) {
    case '"': case '\\': case '/': return emit(c);
    case 'b': return emit('\b');
    case 'f': return emit('\f');
    case 'n': return emit('\n');
    case 'r': return emit('\r');
    case 't': return emit('\t');
    case 'u': {
      int code = hex4();
      if (code >= 0xd800 && code <= 0xdbff) {
        if (encoded() != '\\' || encoded() != 'u')
          code = -1;
        else {
          int low = hex4();
          code = low >= 0xdc00 && low <= 0xdfff ? 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00 : -1;
        }
      } else if (code >= 0xdc00 && code <= 0xdfff)
        code = -1;
      if (code < 0) {
        ok_ = false;
        return -1;
      }
      pendingAt_ = 0;
      if (code < 0x80) {
        pending_[0] = code;
        pendingSize_ = 1;
      } else if (code < 0x800) {
        pending_[0] = 0xc0 | (code >> 6);
        pending_[1] = 0x80 | (code & 63);
        pendingSize_ = 2;
      } else if (code < 0x10000) {
        pending_[0] = 0xe0 | (code >> 12);
        pending_[1] = 0x80 | ((code >> 6) & 63);
        pending_[2] = 0x80 | (code & 63);
        pendingSize_ = 3;
      } else {
        pending_[0] = 0xf0 | (code >> 18);
        pending_[1] = 0x80 | ((code >> 12) & 63);
        pending_[2] = 0x80 | ((code >> 6) & 63);
        pending_[3] = 0x80 | (code & 63);
        pendingSize_ = 4;
      }
      return emit(pending_[pendingAt_++]);
    }
    default: ok_ = false; return -1;
    }
  }
  size_t readBytes(char *buffer, size_t length) {
    size_t readCount = 0;
    int c;
    while (readCount < length && (c = read()) >= 0)
      buffer[readCount++] = static_cast<char>(c);
    return readCount;
  }
  bool finish() {
    while (read() >= 0) {}
    if (!ok_ || !ended_ || !expect(',') || !key("crc32") || !expect(':'))
      return false;
    int c = nonSpace();
    if (c < '0' || c > '9')
      return false;
    uint32_t expected = 0;
    do {
      const unsigned digit = c - '0';
      if (expected > (std::numeric_limits<uint32_t>::max() - digit) / 10)
        return false;
      expected = expected * 10 + digit;
      c = encoded();
    } while (c >= '0' && c <= '9');
    while (whitespace(c))
      c = encoded();
    return c == '}' && nonSpace() == -1 && ok_ && !ferror(file_) && expected == ~crc_;
  }
  size_t length() const { return length_; }

private:
  int encoded() {
    int c = fgetc(file_);
    if (c != EOF && ++bytes_ > maximumDocumentBytes) {
      ok_ = false;
      return -1;
    }
    return c == EOF ? -1 : c;
  }
  int nonSpace() {
    int c;
    do { c = encoded(); } while (whitespace(c));
    return c;
  }
  bool expect(int expected) { return nonSpace() == expected; }
  bool key(const char *name) {
    if (!expect('"'))
      return false;
    for (; *name; ++name)
      if (encoded() != *name)
        return false;
    return encoded() == '"';
  }
  int hex4() {
    int value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      int c = encoded();
      const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                     : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (digit < 0)
        return -1;
      value = (value << 4) | digit;
    }
    return value;
  }
  int emit(int c) {
    ++length_;
    crc_ = crcByte(crc_, static_cast<uint8_t>(c));
    return c;
  }
  FILE *file_;
  uint8_t pending_[4]{};
  unsigned pendingAt_ = 0, pendingSize_ = 0;
  size_t bytes_ = 0, length_ = 0;
  uint32_t crc_ = 0xffffffffU;
  bool ok_ = true, ended_ = false;
};
} // namespace

size_t freeBytes() {
  size_t total = 0, used = 0;
  return esp_littlefs_info("spiffs", &total, &used) == ESP_OK && total > used ? total - used : 0;
}
// Persist the exact JSON bytes inside a checksummed envelope. Rename commits the
// complete replacement; a torn .tmp file never replaces the last good document.
bool saveDocument(const char *path, const JsonDocument &document) {
  if (document.overflowed())
    return false;
  char temporary[256], from[288], to[288];
  const int temporaryLength = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  const int fromLength = snprintf(from, sizeof(from), "%s%s.tmp", FS_PREFIX, path);
  const int toLength = snprintf(to, sizeof(to), "%s%s", FS_PREFIX, path);
  if (temporaryLength < 0 || size_t(temporaryLength) >= sizeof(temporary) || fromLength < 0 ||
      size_t(fromLength) >= sizeof(from) || toLength < 0 || size_t(toLength) >= sizeof(to))
    return false;
  FILE *file = fs_open(temporary, "wb");
  if (!file)
    return false;
  EnvelopeWriter writer(file);
  const char prefix[] = "{\"payload\":\"";
  bool ok = writer.raw(prefix, sizeof(prefix) - 1) && serializeJson(document, writer) == measureJson(document);
  char suffix[40];
  const int suffixLength = snprintf(suffix, sizeof(suffix), "\",\"crc32\":%lu}",
                                    static_cast<unsigned long>(writer.checksum()));
  ok = ok && suffixLength > 0 && size_t(suffixLength) < sizeof(suffix) &&
       writer.raw(suffix, size_t(suffixLength)) && writer.flush();
  if (ok)
    ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
  if (fclose(file) != 0)
    ok = false;
  if (ok)
    ok = ::rename(from, to) == 0;
  if (!ok)
    fs_remove(temporary);
  return ok;
}
bool readDocumentPayload(const char *path, char *destination, size_t capacity, size_t &length) {
  length = 0;
  FILE *file = fs_open(path, "rb");
  if (!file)
    return false;
  PayloadReader reader(file);
  bool fits = true;
  int c;
  while ((c = reader.read()) >= 0) {
    if (destination) {
      if (length < capacity)
        destination[length] = static_cast<char>(c);
      else
        fits = false;
    }
    ++length;
  }
  const bool ok = fits && reader.finish();
  fclose(file);
  return ok;
}
bool loadDocumentPayload(const char *path, std::string &payload) {
  size_t length = 0;
  if (!readDocumentPayload(path, nullptr, 0, length))
    return false;
  payload.resize(length);
  size_t actual = 0;
  return readDocumentPayload(path, payload.data(), payload.size(), actual) && actual == length;
}
bool loadDocument(const char *path, JsonDocument &document) {
  FILE *file = fs_open(path, "rb");
  if (!file)
    return false;
  document.clear();
  PayloadReader reader(file);
  const bool ok = deserializeJson(document, reader) == DeserializationError::Ok && reader.finish();
  fclose(file);
  if (!ok)
    document.clear();
  return ok;
}
bool reserveMetadata() {
  FILE *file = fs_open(reservePath, "wb");
  if (!file)
    return false;
  const char zeros[256] = {};
  bool ok = true;
  for (size_t written = 0; written < metadataReserveBytes && ok; written += sizeof(zeros))
    ok = fwrite(zeros, 1, sizeof(zeros), file) == sizeof(zeros);
  if (ok)
    ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
  if (fclose(file) != 0)
    ok = false;
  return ok;
}
void releaseMetadataReserve() { fs_remove(reservePath); }
bool removePreviousDataset() {
  bool removed = true;
  for (auto path : {journalPath, "/water-test-manifest.json", "/water-test-ack.json", "/water-test-resumed.json",
                    "/water-test-finish.json", "/water-test-boots.json", "/water-test-reserve.bin"}) {
    if (fs_exists(path) && !fs_remove(path))
      removed = false;
    const std::string temporary = std::string(path) + ".tmp";
    if (fs_exists(temporary.c_str()) && !fs_remove(temporary.c_str()))
      removed = false;
  }
  return removed;
}
} // namespace WaterTestStorage
