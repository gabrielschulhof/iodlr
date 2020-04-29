#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "large_page.h"

static FILE* open_log() {
  char pattern[256] = "";
  char* tmpdir = getenv("TMPDIR");

  if (tmpdir == NULL)
    tmpdir = "/tmp";

  snprintf(pattern,
           255,
           "%s/lp_preload.%d.XXXXXX",
           tmpdir,
           getpid());
  pattern[255] = 0;
  int fd = mkstemp(pattern);
  if (fd == -1)
    return NULL;
  return fdopen(fd, "w");
}

void __attribute__((constructor)) map_to_large_pages() {
  bool is_stderr = false;
  FILE* log_file = open_log();
  bool is_enabled = true;

  if (log_file == NULL) {
    is_stderr = true;
    log_file = stderr;
  }

  map_status status = IsLargePagesEnabled(&is_enabled);
  if (status != map_ok) goto fail;

  if (!is_enabled) goto fail;

  status = MapStaticCodeToLargePages();
  if (status != map_ok) goto fail;
  goto done;
fail:
  if (status == map_ok) {
    if (!is_enabled)
      fprintf(log_file,
              "Mapping to large pages in not enabled on your system. "
              "Make sure /sys/kernel/mm/transparent_hugepage/enabled is set to "
              "'madvise' or 'enabled'\n");
  } else {
    fprintf(log_file,
            "Mapping to large pages failed: %s\n",
            MapStatusStr(status, true));
  }
done:
  if (!is_stderr) fclose(log_file);
}
