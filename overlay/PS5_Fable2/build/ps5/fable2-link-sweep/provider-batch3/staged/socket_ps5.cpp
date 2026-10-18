#include <rex/net/socket.h>
#include <rex/platform.h>

// PS5 POSIX compatibility candidate

#include <sys/ioctl.h>
#include <unistd.h>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  return ioctl(static_cast<int>(handle), cmd, arg);
}

}  // namespace rex::net
