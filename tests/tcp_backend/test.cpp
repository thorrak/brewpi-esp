#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cassert>
#include <algorithm>
#include <functional>
#include <iostream>
#include <iterator>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace Native {
std::function<ssize_t(int, void *, size_t, int)> recvHook;
std::function<ssize_t(int, const void *, size_t, int)> sendHook;
std::function<int(int, unsigned long, int *)> ioctlHook;
std::vector<int> closed;

ssize_t receive(int fd, void *buffer, size_t length, int flags) {
  return recvHook ? recvHook(fd, buffer, length, flags) : ::recv(fd, buffer, length, flags);
}
ssize_t send(int fd, const void *buffer, size_t length, int flags) {
  return sendHook ? sendHook(fd, buffer, length, flags) : ::send(fd, buffer, length, flags);
}
int ioctl(int fd, unsigned long request, int *value) {
  return ioctlHook ? ioctlHook(fd, request, value) : ::ioctl(fd, request, value);
}
int close(int fd) {
  closed.push_back(fd);
  return ::close(fd);
}
void reset() {
  recvHook = {};
  sendHook = {};
  ioctlHook = {};
  closed.clear();
}
} // namespace Native

#define recv Native::receive
#define send Native::send
#define ioctl Native::ioctl
#define close Native::close
#include "production_tcp_backend.h"
#undef recv
#undef send
#undef ioctl
#undef close

struct Connection {
  int fd, peer;
  TcpBackend backend;
  Connection() : fd(-1), peer(-1), backend(fd) {
    Native::reset();
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    fd = sockets[0];
    peer = sockets[1];
    assert(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
  }
  ~Connection() {
    if (fd >= 0) ::close(fd);
    if (peer >= 0) ::close(peer);
    Native::reset();
  }
  void disconnectPeer() {
    assert(::close(peer) == 0);
    peer = -1;
  }
  void expectReleased(int original) {
    assert(fd == -1 && backend.getFd() == -1 && !backend);
    assert(Native::closed == std::vector<int>{original});
    assert(fcntl(original, F_GETFD) == -1 && errno == EBADF);
    assert(backend.available() == 0 && backend.read() == -1 && !backend.connected());
    assert(Native::closed.size() == 1);
  }
};

void ordinaryTrafficAndEOF() {
  Connection client;
  const int fd = client.fd;
  assert(client.backend.available() == 0 && client.backend.connected());
  assert(client.backend.read() == -1 && client.fd == fd);
  assert(::send(client.peer, "abc", 3, 0) == 3);
  assert(client.backend.available() == 3);
  assert(client.backend.connected()); // Peek must not consume the first byte.
  client.disconnectPeer();
  assert(client.backend.available() == 3 && client.fd == fd);
  assert(client.backend.read() == 'a');
  assert(client.backend.available() == 2);
  assert(client.backend.read() == 'b');
  assert(client.backend.read() == 'c');
  // The command loop polls available(), not connected(), after the last byte.
  assert(client.backend.available() == 0);
  client.expectReleased(fd);
}

void EOFWithoutQueuedBytes() {
  for (int operation = 0; operation < 3; ++operation) {
    Connection client;
    const int fd = client.fd;
    client.disconnectPeer();
    if (operation == 0) assert(client.backend.available() == 0);
    if (operation == 1) assert(client.backend.read() == -1);
    if (operation == 2) assert(!client.backend.connected());
    client.expectReleased(fd);
  }
}

void transientReceiveAndIoctlErrors() {
  for (int error : {EAGAIN, EWOULDBLOCK, EINTR}) {
    Connection client;
    const int fd = client.fd;
    Native::recvHook = [error](int, void *, size_t, int) { errno = error; return -1; };
    assert(client.backend.read() == -1);
    assert(client.backend.available() == 0);
    assert(client.backend.connected());
    assert(client.fd == fd && Native::closed.empty());
    Native::recvHook = {};
    Native::ioctlHook = [error](int, unsigned long, int *) { errno = error; return -1; };
    assert(client.backend.available() == 0 && client.fd == fd);
    assert(Native::closed.empty());
    Native::ioctlHook = {};
    assert(::send(client.peer, "x", 1, 0) == 1);
    assert(client.backend.available() == 1 && client.backend.read() == 'x');
  }
}

void terminalReceiveAndIoctlErrors() {
  for (int operation = 0; operation < 4; ++operation) {
    Connection client;
    const int fd = client.fd;
    if (operation == 3) {
      Native::ioctlHook = [](int, unsigned long, int *) { errno = EBADF; return -1; };
    } else {
      Native::recvHook = [](int, void *, size_t, int) { errno = ECONNRESET; return -1; };
    }
    if (operation == 0) assert(client.backend.read() == -1);
    if (operation == 1 || operation == 3) assert(client.backend.available() == 0);
    if (operation == 2) assert(!client.backend.connected());
    client.expectReleased(fd);
  }
}

void nonblockingWrites() {
  const uint8_t text[] = {'h', 'e', 'l', 'l', 'o'};
  Connection client;
  const int fd = client.fd;
  assert(client.backend.write(text, sizeof(text)) == sizeof(text));
  uint8_t received[sizeof(text)];
  assert(::recv(client.peer, received, sizeof(received), 0) == sizeof(received));
  assert(std::equal(std::begin(text), std::end(text), std::begin(received)));
  for (int error : {EAGAIN, EWOULDBLOCK, EINTR}) {
    Native::sendHook = [error](int, const void *, size_t, int) { errno = error; return -1; };
    assert(client.backend.write(text, sizeof(text)) == 0 && client.fd == fd);
    assert(Native::closed.empty());
  }
  unsigned writes = 0;
  Native::sendHook = [&](int actual, const void *, size_t length, int) {
    assert(actual == fd && length == sizeof(text));
    ++writes;
    return 2;
  };
  assert(client.backend.write(text, sizeof(text)) == 2 && writes == 1);
  assert(client.backend.write(text, 0) == 0 && writes == 1);
  assert(client.fd == fd && Native::closed.empty());
  Native::sendHook = [](int, const void *, size_t, int) { errno = EPIPE; return -1; };
  assert(client.backend.write(text, sizeof(text)) == 0);
  client.expectReleased(fd);
}

void bytesArriveDuringAvailabilityCheck() {
  Connection client;
  Native::ioctlHook = [&](int, unsigned long, int *count) {
    *count = 0;
    assert(::send(client.peer, "q", 1, 0) == 1);
    return 0;
  };
  assert(client.backend.available() == 1);
  Native::ioctlHook = {};
  assert(client.backend.read() == 'q' && client.fd >= 0);
}

void replacementSurvivesOldFailure() {
  for (int operation = 0; operation < 5; ++operation) {
    Connection client, replacement;
    const int old = client.fd;
    const int current = replacement.fd;
    auto replace = [&]() {
      assert(::close(old) == 0); // The external owner releases its old client.
      client.fd = current;
      replacement.fd = -1; // Ownership moves to client's shared descriptor.
      errno = ECONNRESET;
      return -1;
    };
    Native::recvHook = [&](int, void *, size_t, int) { return replace(); };
    Native::sendHook = [&](int, const void *, size_t, int) { return replace(); };
    if (operation == 4)
      Native::ioctlHook = [&](int, unsigned long, int *) { return replace(); };
    if (operation == 0) assert(client.backend.read() == -1);
    if (operation == 1 || operation == 4) assert(client.backend.available() == 0);
    if (operation == 2) assert(!client.backend.connected());
    if (operation == 3) {
      const uint8_t byte = 'a';
      assert(client.backend.write(&byte, 1) == 0);
    }
    assert(client.fd == current && Native::closed.empty());
    Native::reset();
    assert(::send(replacement.peer, "n", 1, 0) == 1);
    assert(client.backend.available() == 1 && client.backend.read() == 'n');
  }
}

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  ordinaryTrafficAndEOF();
  EOFWithoutQueuedBytes();
  transientReceiveAndIoctlErrors();
  terminalReceiveAndIoctlErrors();
  nonblockingWrites();
  bytesArriveDuringAvailabilityCheck();
  replacementSurvivesOldFailure();
  std::cout << "TcpBackend socket lifecycle regressions passed.\n";
}
