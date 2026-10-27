/**
 * @file src/platform/macos/virtual_display.m
 * @brief CGVirtualDisplay-based virtual display management for macOS 14+.
 *
 * Spawns a helper subprocess (vd_helper) per display to create and hold it.
 * This avoids process-level state in Sunshine (TCC, frameworks, etc.) that prevents
 * CGVirtualDisplay from registering with WindowServer when created in-process.
 *
 * This file is compiled with ARC (-fobjc-arc). See macos.cmake.
 */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "virtual_display.h"

extern char **environ;

#define MAX_VIRTUAL_DISPLAYS 16

typedef struct {
  pid_t helper_pid;
  uint32_t display_id;
} virtual_display_entry_t;

// State protected by mutex
static pthread_mutex_t vd_mutex = PTHREAD_MUTEX_INITIALIZER;
static virtual_display_entry_t vd_entries[MAX_VIRTUAL_DISPLAYS];

// Get the path to the vd_helper binary (same directory as sunshine)
static NSString *helperPath(void) {
  NSString *mainExe = [[NSBundle mainBundle] executablePath];
  if (!mainExe) {
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
      mainExe = [[NSString stringWithUTF8String:buf] stringByResolvingSymlinksInPath];
    }
  }
  if (!mainExe) return nil;
  NSString *dir = [mainExe stringByDeletingLastPathComponent];
  return [dir stringByAppendingPathComponent:@"vd_helper"];
}

static void stop_helper(pid_t pid) {
  kill(pid, SIGTERM);
  // Non-blocking wait, the helper may take a moment to release the display
  for (int i = 0; i < 10; i++) {
    if (waitpid(pid, NULL, WNOHANG) != 0) break;
    usleep(100000);
  }
}

uint32_t virtual_display_create(int width, int height, int fps) {
  pthread_mutex_lock(&vd_mutex);

  uint32_t displayID = 0;
  virtual_display_entry_t *slot = NULL;
  for (int i = 0; i < MAX_VIRTUAL_DISPLAYS && slot == NULL; i++) {
    if (vd_entries[i].helper_pid == 0) slot = &vd_entries[i];
  }

  NSString *helper = helperPath();
  if (slot == NULL) {
    NSLog(@"[Sunshine] All %d virtual display slots are in use", MAX_VIRTUAL_DISPLAYS);
  } else if (!helper || ![[NSFileManager defaultManager] isExecutableFileAtPath:helper]) {
    NSLog(@"[Sunshine] vd_helper not found next to the sunshine binary: %@", helper);
  } else {
    NSLog(@"[Sunshine] Spawning vd_helper: %@ %d %d %d", helper, width, height, fps);

    // Set up pipe for reading displayID from child's stdout
    int pipefd[2];
    if (pipe(pipefd) != 0) {
      NSLog(@"[Sunshine] pipe() failed: %s", strerror(errno));
    } else {
      char widthStr[16], heightStr[16], fpsStr[16];
      snprintf(widthStr, sizeof(widthStr), "%d", width);
      snprintf(heightStr, sizeof(heightStr), "%d", height);
      snprintf(fpsStr, sizeof(fpsStr), "%d", fps);

      const char *argv[] = {[helper fileSystemRepresentation], widthStr, heightStr, fpsStr, NULL};

      // Child's stdout goes to the pipe's write end
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
      posix_spawn_file_actions_addclose(&actions, pipefd[0]);
      posix_spawn_file_actions_addclose(&actions, pipefd[1]);

      pid_t pid;
      int err = posix_spawn(&pid, argv[0], &actions, NULL, (char *const *) argv, environ);
      posix_spawn_file_actions_destroy(&actions);
      close(pipefd[1]);

      if (err != 0) {
        NSLog(@"[Sunshine] posix_spawn failed: %s", strerror(err));
      } else {
        // Read displayID from child's stdout, with a timeout
        char buf[64] = {0};
        ssize_t n = 0;
        fd_set readfds;
        struct timeval tv = {10, 0};
        FD_ZERO(&readfds);
        FD_SET(pipefd[0], &readfds);
        if (select(pipefd[0] + 1, &readfds, NULL, NULL, &tv) > 0) {
          n = read(pipefd[0], buf, sizeof(buf) - 1);
        }

        displayID = n > 0 ? (uint32_t) strtoul(buf, NULL, 10) : 0;
        if (displayID == 0) {
          NSLog(@"[Sunshine] vd_helper (pid=%d) reported no display, stopping it", pid);
          stop_helper(pid);
        } else {
          slot->helper_pid = pid;
          slot->display_id = displayID;
          NSLog(@"[Sunshine] Virtual display %u created via vd_helper (pid=%d)", displayID, pid);
        }
      }
      close(pipefd[0]);
    }
  }

  pthread_mutex_unlock(&vd_mutex);
  return displayID;
}

void virtual_display_destroy(uint32_t display_id) {
  pthread_mutex_lock(&vd_mutex);

  for (int i = 0; i < MAX_VIRTUAL_DISPLAYS; i++) {
    if (vd_entries[i].helper_pid != 0 && vd_entries[i].display_id == display_id) {
      NSLog(@"[Sunshine] Destroying virtual display %u (vd_helper pid=%d)", display_id, vd_entries[i].helper_pid);
      stop_helper(vd_entries[i].helper_pid);
      vd_entries[i].helper_pid = 0;
      vd_entries[i].display_id = 0;
    }
  }

  pthread_mutex_unlock(&vd_mutex);
}
