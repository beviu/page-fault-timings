#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>

#define MAX_FAST_TRACEPOINTS 32
#define PF_TRACING_MAGIC 0xb141a52a

static bool no_msg_received = false;
static int fast_tracepoints_dir_fd = -1;
static int userfaultfd_fd;
static ssize_t page_size;
static char *fast_tracepoints[MAX_FAST_TRACEPOINTS + 1];
static struct uffdio_range userfaultfd_range;

static bool parse_uint64_t(const char **buf, const char *end, uint64_t *out) {
  if (*buf == end || **buf < '0' || **buf > '9')
    return false;

  *out = 0;

  while (*buf != end && **buf >= '0' && **buf <= '9') {
    *out *= 10;
    *out += **buf - '0';
    ++*buf;
  }

  return true;
}

static bool read_fast_tracepoint(const char *name, uint64_t *out) {
  int timestamp_fd;
  char buf[21];
  ssize_t n;
  const char *cursor;
  bool ok = false;

  timestamp_fd = openat(fast_tracepoints_dir_fd, name, O_RDONLY);
  if (timestamp_fd == -1) {
    fprintf(stderr, "open(/proc/sys/debug/fast-tracepoints/%s): %s\n", name,
            strerror(errno));
    goto fail;
  }

  n = read(timestamp_fd, buf, sizeof(buf));
  if (n == -1) {
    fprintf(stderr, "read(/proc/sys/debug/fast-tracepoints/%s): %s\n", name,
            strerror(errno));
    goto fail;
  }

  cursor = buf;
  if (!parse_uint64_t(&cursor, buf + n, out)) {
    fprintf(stderr, "Failed to parse timestamp: %.*s.\n", (int)n, buf);
    goto fail;
  }

  ok = true;

fail:
  close(timestamp_fd);

  return ok;
}

static int userfaultfd(int flags) { return syscall(SYS_userfaultfd, flags); }

static inline uint64_t rdtsc_serialize() {
  uint32_t counter_low, counter_high;

  asm("mfence; rdtscp; lfence"
      : "=a"(counter_low), "=d"(counter_high)
      :
      : "ecx", "memory");

  return counter_low | ((uint64_t)counter_high << 32);
}

static _Atomic uint64_t timestamp_msg_received;

static void *userfaultfd_thread_routine(void *data) {
  size_t n;
  struct uffd_msg msg;
  unsigned int aux;
  struct uffdio_zeropage zeropage_args = {0};

  (void)data;

  for (;;) {
    n = read(userfaultfd_fd, &msg, sizeof(msg));
    if (n == -1) {
      perror("read");
      break;
    } else if (n != sizeof(msg)) {
      fprintf(stderr, "Could only read %zd bytes from userfaultfd.\n", n);
      break;
    }

    if (msg.event != UFFD_EVENT_PAGEFAULT)
      continue;

    if (!no_msg_received)
      atomic_store_explicit(&timestamp_msg_received, rdtsc_serialize(),
                            memory_order_relaxed);

    zeropage_args.range.start = msg.arg.pagefault.address & ~(page_size - 1);
    zeropage_args.range.len = page_size;
    zeropage_args.mode = 0;
    if (ioctl(userfaultfd_fd, UFFDIO_ZEROPAGE, &zeropage_args) == -1) {
      perror("ioctl(UFFDIO_ZEROPAGE)");
      break;
    }
  }

  return NULL;
}

static void print_usage(const char *arg0) {
  printf(
      "Usage: %s [OPTION]...\n"
      "Collect page fault timings.\n"
      "\n"
      "Mandatory arguments to long options are mandatory for short options "
      "too.\n"
      "  -f, --file=FILE              access a memory mapping of FILE instead "
      "of an anonymous mapping\n"
      "  -s, --start=OFFSET           start reading from OFFSET in the memory "
      "mapping\n"
      "  -l, --length=LENGTH          read pages spanning LENGTH bytes\n"
      "  -i, --iterations=COUNT       do COUNT iterations\n"
      "  -t, --type=TYPE              set the type of fault, can be major or "
      "minor\n"
      "  -a, --access=ACCESS          set the type of access, can be read, "
      "write, "
      "or rw (do a read before collecting timings for a write)\n"
      "  -u, --userfaultfd            use userfaultfd\n"
      "  -c, --userfaultfd-same-cpus  pin the faulting thread and userfaultfd "
      "thread to the same CPUs\n"
      "  -n, --no-msg-received        disable the msg_received timing\n"
      "  -h, --help                   display this help and exit\n",
      arg0);
}

static bool list_fast_tracepoints() {
  DIR *dir;
  struct dirent *entry;
  int i = 0;

  dir = opendir("/proc/sys/debug/fast-tracepoints");
  if (!dir) {
    perror("opendir(/proc/sys/debug/fast-tracepoints)");
    return false;
  }

  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_type & DT_REG) {
      if (i == MAX_FAST_TRACEPOINTS) {
        fputs("Too many fast tracepoints! Increase MAX_FAST_TRACEPOINTS.\n",
              stderr);
        while (--i > 0)
          free(fast_tracepoints[i]);
        closedir(dir);
        return false;
      }
      fast_tracepoints[i++] = strdup(entry->d_name);
    }
  }

  closedir(dir);

  return true;
}

static void print_columns() {
  int i = 0;

  fputs("end", stdout);

  if (!no_msg_received)
    fputs(",msg_received", stdout);

  if (fast_tracepoints_dir_fd != -1) {
    fputs(",isr_entry,iret", stdout);

    while (fast_tracepoints[i] != NULL)
      fprintf(stdout, ",%s", fast_tracepoints[i++]);
  }

  fputs("\n", stdout);
}

static const struct option long_options[] = {
    {"length", required_argument, 0, 'l'},
    {"iterations", required_argument, 0, 'i'},
    {"type", required_argument, 0, 't'},
    {"access", required_argument, 0, 'a'},
    {"userfaultfd", no_argument, 0, 'u'},
    {"userfaultfd-same-cpus", no_argument, 0, 'c'},
    {"no-msg_received", no_argument, 0, 'n'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}};

static bool do_iteration(uint64_t length, int access, bool with_userfaultfd,
                         size_t page_size) {
  void *memory;
  struct uffdio_register register_args = {0};
  volatile uint8_t *byte;
  uint64_t timestamp_page_fault;
  uint64_t timestamp_isr_entry;
  uint64_t timestamp_iret;
  uint8_t byte_copy;
  uint64_t timestamp_end;
  uint64_t timestamp_msg_received_copy;
  int i;
  uint64_t timestamp;

  print_columns();

  memory = mmap(NULL, length, access, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (memory == MAP_FAILED) {
    perror("mmap");
    return false;
  }

  if (madvise(memory, length, MADV_NOHUGEPAGE) == -1) {
    perror("madvise(MADV_NOHUGEPAGE)");
    munmap(memory, length);
    return false;
  }

  if (with_userfaultfd) {
    register_args.range.start = (uint64_t)memory;
    register_args.range.len = length;
    register_args.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(userfaultfd_fd, UFFDIO_REGISTER, &register_args) == -1) {
      perror("ioctl(UFFDIO_REGISTER)");
      munmap(memory, length);
      return false;
    }
  }

  for (byte = memory; (uintptr_t)byte - (uintptr_t)memory < length;
       byte += page_size) {
    if (access == (PROT_READ | PROT_WRITE))
      *byte;

    timestamp_page_fault = rdtsc_serialize();

    if (fast_tracepoints_dir_fd == -1) {
      if (access == PROT_READ) {
        *byte;
      } else {
        *byte = 0;
      }
    } else {
      /* Trigger a page fault with the magic value in ebx. */
      if (access == PROT_READ) {
        asm volatile("movb (%[byte]), %[byte_copy]"
                     : "=&a"(timestamp_isr_entry),
                       "=&c"(timestamp_iret), [byte_copy] "=r"(byte_copy)
                     : [byte] "r"(byte), "b"(PF_TRACING_MAGIC)
                     : "edx");
      } else {
        asm volatile("movb $0, (%[byte])"
                     : "=&a"(timestamp_isr_entry), "=&c"(timestamp_iret)
                     : [byte] "r"(byte), "b"(PF_TRACING_MAGIC)
                     : "edx");
      }
    }

    timestamp_end = rdtsc_serialize();

    printf("%" PRIu64, timestamp_end - timestamp_page_fault);

    if (!no_msg_received) {
      timestamp_msg_received_copy =
          atomic_load_explicit(&timestamp_msg_received, memory_order_relaxed);

      if (timestamp_msg_received_copy > timestamp_page_fault)
        printf(",%" PRIu64, timestamp_msg_received_copy - timestamp_page_fault);
      else
        fputc(',', stdout);
    }

    if (fast_tracepoints_dir_fd != -1) {
      printf(",%" PRIu64 ",%" PRIu64,
             timestamp_isr_entry - timestamp_page_fault,
             timestamp_iret - timestamp_page_fault);

      for (i = 0; fast_tracepoints[i] != NULL; ++i) {
        if (!read_fast_tracepoint(fast_tracepoints[i], &timestamp)) {
          munmap(memory, length);
          return false;
        }
        /* Check if the tracepoint was hit. */
        if (timestamp > timestamp_page_fault)
          printf(",%" PRIu64, timestamp - timestamp_page_fault);
        else
          fputc(',', stdout);
      }
    }

    fputc('\n', stdout);

    if (!no_msg_received)
      atomic_store_explicit(&timestamp_msg_received, 0, memory_order_relaxed);
  }

  munmap(memory, length);

  return true;
}

int main(int argc, char **argv) {
  const char *arg0;
  int opt;
  const char *cursor;
  uint64_t length = 1;
  uint64_t iteration_count = 1;
  bool do_major_faults = false;
  int access = PROT_READ;
  bool with_userfaultfd = false;
  bool userfaultfd_same_cpus = true;
  int ret = EXIT_FAILURE;
  struct stat stat_buf;
  cpu_set_t cpu_set;
  int err;
  int initial_prot;
  void *memory = MAP_FAILED;
  uint8_t *byte;
  uint8_t byte_copy;
  struct uffdio_api api_args = {0};
  pthread_t userfaultfd_thread;
  unsigned int aux;
  int i;
  uint64_t timestamp;

  arg0 = argv[0] ? argv[0] : "collect-page-fault-timings";

  while ((opt = getopt_long(argc, argv, "s:l:i:t:a:uch", long_options, NULL)) !=
         -1) {
    switch (opt) {
    case 'l':
      cursor = optarg;
      if (!parse_uint64_t(&cursor, optarg + strlen(optarg), &length) ||
          *cursor) {
        fprintf(stderr, "%s: invalid length: ‘%s’\n", arg0, optarg);
        goto out;
      }
      break;
    case 'i':
      cursor = optarg;
      if (!parse_uint64_t(&cursor, optarg + strlen(optarg), &iteration_count) ||
          *cursor) {
        fprintf(stderr, "%s: invalid iteration count: ‘%s’\n", arg0, optarg);
        goto out;
      }
      break;
    case 't':
      if (!strcmp(optarg, "minor")) {
        do_major_faults = false;
      } else if (!strcmp(optarg, "major")) {
        do_major_faults = true;
      } else {
        fprintf(stderr, "%s: invalid fault type: ‘%s’\n", arg0, optarg);
        goto out;
      }
      break;
    case 'a':
      if (!strcmp(optarg, "read")) {
        access = PROT_READ;
      } else if (!strcmp(optarg, "write")) {
        access = PROT_WRITE;
      } else if (!strcmp(optarg, "rw")) {
        access = PROT_READ | PROT_WRITE;
      } else {
        fprintf(stderr, "%s: invalid access type: ‘%s’\n", arg0, optarg);
        goto out;
      }
      break;
    case 'u':
      with_userfaultfd = true;
      break;
    case 'c':
      userfaultfd_same_cpus = true;
      break;
    case 'n':
      no_msg_received = true;
      break;
    case 'h':
      print_usage(arg0);
      goto out;
    default:
      fprintf(stderr, "Try '%s --help' for more information.\n", arg0);
      goto out;
    }
  }

  if (with_userfaultfd && do_major_faults) {
    fprintf(stderr,
            "%s: major page faults are not supported when using userfaultfd\n",
            arg0);
    goto out;
  }

  if (!with_userfaultfd)
    no_msg_received = true;

  fast_tracepoints_dir_fd =
      open("/proc/sys/debug/fast-tracepoints", O_DIRECTORY | O_PATH);
  if (fast_tracepoints_dir_fd == -1) {
    if (errno != ENOENT) {
      perror("open(/proc/sys/debug/fast-tracepoints)");
      goto out;
    }
  }

  if (fast_tracepoints_dir_fd != -1 && !list_fast_tracepoints())
    goto cleanup_fast_tracepoints_dir_fd;

  page_size = sysconf(_SC_PAGESIZE);
  if (page_size == -1) {
    perror("sysconf(_SC_PAGESIZE)");
    goto cleanup_fast_tracepoints;
  }

  if (with_userfaultfd) {
    userfaultfd_fd = userfaultfd(O_CLOEXEC);
    if (userfaultfd_fd == -1) {
      perror("userfaultfd");
      goto cleanup_fast_tracepoints;
    }

    api_args.api = UFFD_API;
    if (ioctl(userfaultfd_fd, UFFDIO_API, &api_args) == -1) {
      perror("ioctl(UFFDIO_API)");
      goto cleanup_userfaultfd_fd;
    }

    err = pthread_create(&userfaultfd_thread, NULL, userfaultfd_thread_routine,
                         NULL);
    if (err) {
      fprintf(stderr, "pthread_create: %s\n", strerror(err));
      goto cleanup_userfaultfd_fd;
    }

    if (userfaultfd_same_cpus) {
      CPU_ZERO(&cpu_set);
      CPU_SET(0, &cpu_set);

      err = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
      if (err) {
        fprintf(stderr, "pthread_setaffinity_np: %s\n", strerror(err));
        goto cleanup;
      }

      err =
          pthread_setaffinity_np(userfaultfd_thread, sizeof(cpu_set), &cpu_set);
      if (err) {
        fprintf(stderr, "pthread_setaffinity_np: %s\n", strerror(err));
        goto cleanup;
      }
    }
  }

  length = (length + page_size - 1) & ~(page_size - 1);

  for (i = 0; i < iteration_count; ++i)
    do_iteration(length, access, with_userfaultfd, page_size);

  ret = EXIT_SUCCESS;

cleanup:
  if (with_userfaultfd) {
    pthread_cancel(userfaultfd_thread);
    pthread_join(userfaultfd_thread, NULL);
  }

cleanup_userfaultfd_fd:
  if (with_userfaultfd)
    close(userfaultfd_fd);

cleanup_fast_tracepoints:
  for (i = 0; fast_tracepoints[i] != NULL; ++i)
    free(fast_tracepoints[i]);

cleanup_fast_tracepoints_dir_fd:
  if (fast_tracepoints_dir_fd != -1)
    close(fast_tracepoints_dir_fd);

out:
  return ret;
}
