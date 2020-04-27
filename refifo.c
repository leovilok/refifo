#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define NAME "refifo"

struct
{
  char *data;
  size_t size;
  size_t read;
  size_t end;
  bool closed;
} buffer;

char *fifo_path;

static int
errexit(const char *msg)
{
  perror(msg);
  exit(1);
}

static void
config_output()
{
  int ret = mkfifo(fifo_path, 0666);

  if (ret == -1) {
    if (errno != EEXIST) {
      errexit("mkfifo");
    } else {
      errno = 0;
    }
  }

  void (*sigret)(int) = signal(SIGPIPE, SIG_IGN);
  if (sigret == SIG_ERR)
    errexit("signal");

  int fd = open(fifo_path, O_WRONLY);
  if (fd == -1)
    errexit("open");

  ret = dup2(fd, 1);
  if (ret == -1)
    errexit("dup2");

  ret = close(fd);
  if (ret == -1)
    errexit("close");
}

static void
config_input(void)
{
  int flags = fcntl(0, F_GETFD);
  if (flags < 0)
    errexit("fcntl(0, F_GETFD)");

  int ret = fcntl(0, F_SETFD, flags | O_NONBLOCK);
  if (ret == -1)
    errexit("fcntl(0, F_SETFD, ...)");
}

static void
grow_buffer(size_t size)
{
  if (!size)
    size = PIPE_BUF;

  buffer.size += size;
  buffer.data = realloc(buffer.data, buffer.size);

  if (!buffer.data)
    errexit("realloc");
}

static void
write_file(void)
{
  char path[] = NAME ".tmp.XXXXXX";
  int fd = mkstemp(path);
  if (fd == -1)
    errexit("mkstemp");

  ssize_t ret = 0;
  size_t wrote = 0;
  while (wrote != buffer.end) {
    ret = write(fd, buffer.data + wrote, buffer.end - wrote);
    if (ret == -1) {
      perror("write(TMPFILE)");
      goto closeexit;
    } else {
      wrote += ret;
    }
  }

  ret = close(fd);
  if (ret == -1) {
    perror("close(TMPFILE)");
    goto unlinkexit;
  }

  ret = rename(path, fifo_path);
  if (ret == -1) {
    perror("rename");
    goto unlinkexit;
  }

  return;

closeexit:
  ret = close(fd);
  if (ret == -1)
    perror("close(TMPFILE)");

unlinkexit:
  ret = unlink(path);
  if (ret == -1)
    perror("unlink(TMPFILE)");

  exit(1);
}

static int
read_data(void)
{
  struct pollfd fds[] = {
    { .fd = 0, .events = POLLIN },
    { .fd = 1, .events = 0 },
  };

  int ret = poll(fds, 2, -1);

  if (ret == -1)
    errexit("poll(IN)");

  if (fds[1].revents)
    return -1;

  if (fds[0].revents & POLLIN) {

    if (buffer.end == buffer.size)
      grow_buffer(0);

    ret = read(0, buffer.data + buffer.end, buffer.size - buffer.end);

    if (ret == -1) {
      errexit("read");
    }

    if (ret == 0) {
      write_file();
      buffer.closed = true;
      return 0;
    }
    buffer.end += ret;
    return ret;
  }

  assert(fds[0].revents & (POLLHUP | POLLERR));

  write_file();
  buffer.closed = true;
  return 0;
}

static void
io_loop(void)
{
  while (true) {
    struct pollfd writefd = {
      .fd = 1,
      .events = POLLOUT,
    };

    int ret = poll(&writefd, 1, -1);
    if (ret == -1)
      errexit("poll(OUT)");

    if (writefd.revents & (POLLHUP | POLLERR)) {
      if (buffer.closed)
        return;
      buffer.read = 0;
    }
    if (writefd.revents & POLLOUT) {
      if (buffer.read == buffer.end) {
        if (buffer.closed) {
          return;
        } else {
          ret = read_data();
          if (ret == -1) {
            buffer.read = 0;
            continue;
          }
          if (ret == 0)
            return;
        }
      }
      ssize_t wrote =
        write(1, buffer.data + buffer.read, buffer.end - buffer.read);
      if (wrote == -1) {
        if (errno == EPIPE) {
          errno = 0;
          if (buffer.closed)
            return;
          buffer.read = 0;
        } else {
          errexit("write(FIFO)");
        }
      } else {
        buffer.read += wrote;
      }
    }
  }
}

int
main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(stderr, "Usage: %s FIFO\n", argv[0]);
    return 1;
  }

  fifo_path = argv[1];

  config_output();
  config_input();
  io_loop();

  return 0;
}
