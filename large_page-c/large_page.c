// Copyright (C) 2018 Intel Corporation
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom
// the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
// OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.
//
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE
#include "large_page.h"
#include <link.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>
#include <linux/limits.h>
#include <regex.h>

typedef struct {
  void*     from;
  void*     to;
} mem_range;

typedef struct {
  uintptr_t start;
  uintptr_t end;
  regex_t regex;
  bool have_regex;
} FindParams;

#define HPS (2L * 1024 * 1024)
#define SAFE_DEL(p, func) {if (p) { func(p); p = NULL; }}

static inline uintptr_t largepage_align_down(uintptr_t addr) {
  return (addr & ~(HPS - 1));
}

static inline uintptr_t largepage_align_up(uintptr_t addr) {
  return largepage_align_down(addr + HPS - 1);
}

static int FindMapping(struct dl_phdr_info* hdr, size_t size, void* data) {
  int idx, fd, old_errno;
  const char* filename;
  struct stat st;
  char* exe;
  const char* strtab;
  ElfW(Ehdr)* ehdr;
  ElfW(Shdr)* shdr;
  ElfW(Shdr)* shdr_text;
  FindParams* find_params = (FindParams*)data;

  // We are only interested in the information matching the regex or, if no
  // regex was given, the mapping matching the main executable. This latter
  // mapping has the empty string for a name.
  if ((find_params->have_regex &&
        regexec(&find_params->regex, hdr->dlpi_name, 0, NULL, 0) == 0) ||
      hdr->dlpi_name[0] == 0) {

    filename = ((hdr->dlpi_name[0] == 0)
        ? "/proc/self/exe"
        : hdr->dlpi_name);

    if (stat(filename, &st) != 0) {
      return -map_failed_to_stat_exe;
    }

    fd = open(filename, O_RDONLY);
    if (fd == -1) {
      return -map_failed_to_open_exe;
    }

    // Knowing the size of the file from stat(2) and with a file descriptor from
    // open(2) we can now mmap(2) the executable.
    exe = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (exe == MAP_FAILED) {
      old_errno = errno;
      close(fd);
      errno = old_errno;
      return -map_failed_to_map_exe_see_errno;
    }

    // We get the offsets to the string table storing the names of the sections.
    ehdr = (ElfW(Ehdr)*)exe;
    shdr = (ElfW(Shdr)*)(exe + ehdr->e_shoff);
    shdr_text = NULL;
    strtab = exe + shdr[ehdr->e_shstrndx].sh_offset;

    // We look for the .text section headers.
    for (idx = 0; idx < ehdr->e_shnum; idx++) {
      if (!strncmp(strtab + shdr[idx].sh_name, ".text", 5)) {
        shdr_text = &shdr[idx];
      }
    }

    if (shdr_text == NULL) {
      munmap(exe, st.st_size);
      close(fd);
      return -map_failed_to_find_text_section;
    }

    // If we found the .text section header we use the address specified therein
    // along with the base address given in the program header info to calculate
    // the current base address of the .text section, as well as the address of
    // its end.
    find_params->start = shdr_text->sh_addr + hdr->dlpi_addr;
    find_params->end = find_params->start + shdr_text->sh_size;

    // As part of the cleanup we unmap the executable.
    if (munmap(exe, st.st_size) != 0) {
      old_errno = errno;
      close(fd);
      errno = old_errno;
      return map_failed_to_unmap_exe_see_errno;
    }

    // We close the executable file.
    if (close(fd) == -1) {
      return -map_failed_to_close_exe_see_errno;
    }

    return 1;
  }

  return -map_region_not_found;
}

// Identify and return the text region in the currently mapped memory regions.
static map_status FindTextRegion(const char* lib_regex, mem_range* region) {
  int status;
  FindParams find_params = { 0, 0, { 0 }, false };

  if (lib_regex != NULL) {
    if (regcomp(&find_params.regex, lib_regex, 0) != 0) {
      return map_invalid_regex;
    }
    find_params.have_regex = true;
  }

  // We iterate over all the mappings created for the main executable and any of
  // its linked-in dependencies. The return value of `FindMapping` will become
  // the return value of `dl_iterate_phdr`.
  status = dl_iterate_phdr(FindMapping, &find_params);
  if ( status < 0) {
    regfree(&find_params.regex);
    return -status;
  }

  region->from = (void*)find_params.start;
  region->to = (void*)find_params.end;

  regfree(&find_params.regex);
  return map_ok;
}

static map_status IsTransparentHugePagesEnabled(bool* result) {
#if defined(ENABLE_LARGE_CODE_PAGES) && ENABLE_LARGE_CODE_PAGES
  FILE* ifs;
  char always[16] = {0};
  char madvise[16] = {0};
  char never[16] = {0};
  int matched;

  *result = false;
  ifs = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "rt");
  if (!ifs) {
    return map_failed_to_open_thp_file;
  }

  matched = fscanf(ifs, "%s %s %s", always, madvise, never);
  fclose(ifs);

  if (matched != 3) {
    return map_malformed_thp_file;
  }

  if (strcmp(always, "[always]") == 0) {
    *result = true;
  } else if (strcmp(madvise, "[madvise]") == 0) {
    *result = true;
  } else if (strcmp(never, "[never]") == 0) {
    *result = false;
  }

  return map_ok;
#else
  return map_unsupported_platform;
#endif  // ENABLE_LARGE_CODE_PAGES
}

// Move specified region to large pages. We need to be very careful.
// 1: This function itself should not be moved.
// We use a gcc attributes
// (__section__) to put it outside the ".text" section
// (__aligned__) to align it at 2M boundary
// (__noline__) to not inline this function
// 2: This function should not call any function(s) that might be moved.
// a. map a new area and copy the original code there
// b. mmap using the start address with MAP_FIXED so we get exactly
//    the same virtual address
// c. madvise with MADV_HUGE_PAGE
// d. If successful copy the code there and unmap the original region
static map_status
__attribute__((__section__(".lpstub")))
__attribute__((__aligned__(HPS)))
__attribute__((__noinline__))
MoveRegionToLargePages(const mem_range* r) {
  void* nmem = NULL;
  void* tmem = NULL;
  int ret = 0;
  map_status status = map_ok;
  void* start = r->from;
  size_t size = r->to - r->from;

  // Allocate temporary region preparing for copy
  nmem = mmap(NULL, size,
              PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (nmem == MAP_FAILED) {
    return map_see_errno;
  }

  memcpy(nmem, r->from, size);

  // We already know the original page is r-xp
  // (PROT_READ, PROT_EXEC, MAP_PRIVATE)
  // We want PROT_WRITE because we are writing into it.
  // We want it at the fixed address and we use MAP_FIXED.
#define CLEAN_EXIT_CHECK(oper)                          \
  if (tmem == MAP_FAILED) {                             \
    status = oper##_failed;                             \
    ret = munmap(nmem, size);                           \
    if (ret < 0) {                                      \
      status = oper##_munmap_nmem_failed;               \
    }                                                   \
    return status;                                      \
  }

  tmem = mmap(start, size,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1 , 0);
  CLEAN_EXIT_CHECK(map_see_errno_mmap_tmem);

#undef CLEAN_EXIT_CHECK

#define CLEAN_EXIT_CHECK(oper)                          \
  if (ret < 0) {                                        \
    status = oper##_failed;                             \
    ret = munmap(tmem, size);                           \
    if (ret < 0) {                                      \
      status = oper##_munmap_tmem_failed;               \
    }                                                   \
    ret = munmap(nmem, size);                           \
    if (ret < 0) {                                      \
      status = (status == oper##_munmap_tmem_failed)    \
        ? oper##_munmaps_failed                         \
        : oper##_munmap_nmem_failed;                    \
    }                                                   \
    return status;                                      \
  }

  ret = madvise(tmem, size, MADV_HUGEPAGE);
  CLEAN_EXIT_CHECK(map_see_errno_madvise_tmem);

  memcpy(start, nmem, size);
  ret = mprotect(start, size, PROT_READ | PROT_EXEC);
  CLEAN_EXIT_CHECK(map_see_errno_mprotect);

#undef CLEAN_EXIT_CHECK

  // Release the old/temporary mapped region
  ret = munmap(nmem, size);
  if (ret < 0) {
    status = map_see_errno_munmap_nmem_failed;
  }

  return status;
}

// Align the region to to be mapped to 2MB page boundaries.
static void AlignRegionToPageBoundary(mem_range* r) {
  r->from = (void*)(largepage_align_up((uintptr_t)r->from));
  r->to = (void*)(largepage_align_down((uintptr_t)r->to));
}

static map_status CheckMemRange(mem_range* r) {
  if (r->from == NULL || r->to == NULL || r->from > r->to) {
    return map_invalid_region_address;
  }

  if (r->to - r->from < HPS) {
    return map_region_too_small;
  }

  return map_ok;
}

// Align the region to to be mapped to 2MB page boundaries and then move the
// region to large pages.
static map_status AlignMoveRegionToLargePages(mem_range* r) {
  map_status status;

  fprintf(stderr,
          "Found memory region %016lx - %016lx\n",
          ((uintptr_t)r->from),
          ((uintptr_t)r->to));

  AlignRegionToPageBoundary(r);

  fprintf(stderr,
          "Aligned to %016lx - %016lx (%d pages)\n",
          ((uintptr_t)r->from),
          ((uintptr_t)r->to),
          (((uintptr_t)r->to) - ((uintptr_t)r->from)) / HPS);

  status = CheckMemRange(r);
  if (status != map_ok) {
    return status;
  }

  return MoveRegionToLargePages(r);
}

// Map the .text segment of the linked application into 2MB pages.
// The algorithm is simple:
// 1. Find the text region of the executing binary in memory
//    * Examine the /proc/self/maps to determine the currently mapped text
//      region and obtain the start and end addresses.
//    * Modify the start address to point to the very beginning of .text segment
//      (from variable textsegment setup in ld.script).
//    * Align the address of start and end addresses to large page boundaries.
//
// 2: Move the text region to large pages
//    * Map a new area and copy the original code there.
//    * Use mmap using the start address with MAP_FIXED so we get exactly the
//      same virtual address.
//    * Use madvise with MADV_HUGE_PAGE to use anonymous 2M pages.
//    * If successful, copy the code to the newly mapped area and unmap the
//      original region.
map_status MapStaticCodeToLargePages() {
  mem_range r = {0};
  map_status status = FindTextRegion(NULL, &r);
  if (status != map_ok) {
    return status;
  }
  return AlignMoveRegionToLargePages(&r);
}

map_status MapDSOToLargePages(const char* lib_regex) {
  mem_range r = {0};
  map_status status;

  if (lib_regex == NULL) {
    return map_null_regex;
  }

  status = FindTextRegion(lib_regex, &r);
  if (status != map_ok) {
    return status;
  }
  return AlignMoveRegionToLargePages(&r);
}

// This function is similar to the function above. However, the region to be
// mapped to 2MB pages is specified for this version as hotStart and hotEnd.
map_status MapStaticCodeRangeToLargePages(void* from, void* to) {
  mem_range r = {from, to};
  return AlignMoveRegionToLargePages(&r);
}

// Return true if transparent huge pages is enabled on the system. Otherwise,
// return false.
map_status IsLargePagesEnabled(bool* result) {
  return IsTransparentHugePagesEnabled(result);
}

const char* MapStatusStr(map_status status, bool fulltext) {
  static const char* map_status_text[] = {
    "map_ok",
      "ok",
    "map_failed_to_open_thp_file",
      "failed to open thp enablement status file",
    "map_failed_to_stat_exe",
      "failed to stat executable",
    "map_failed_to_open_exe",
      "failed to open executable",
    "map_failed_to_map_exe_see_errno",
      "failed to map executable",
    "map_failed_to_find_text_section",
      "failed to find text section",
    "map_failed_to_unmap_exe_see_errno",
      "failed to unmap executable",
    "map_failed_to_close_exe_see_errno",
      "failed to close executable",
    "map_invalid_regex",
      "invalid regex",
    "map_invalid_region_address",
      "invalid region boundaries",
    "map_malformed_thp_file",
      "malformed thp enablement status file",
    "map_null_regex",
      "regex was NULL",
    "map_region_not_found",
      "map region not found",
    "map_region_too_small",
      "map region too small",
    "map_see_errno",
      "see errno",
    "map_see_errno_madvise_tmem_failed",
      "madvise for destination failed",
    "map_see_errno_madvise_tmem_munmap_nmem_failed",
      "madvise for destination and unmapping of temporary failed",
    "map_see_errno_madvise_tmem_munmaps_failed",
      "madvise for destination and unmappings failed",
    "map_see_errno_madvise_tmem_munmap_tmem_failed",
      "madvise for destination and unmapping of destination failed",
    "map_see_errno_mmap_tmem_failed",
      "mapping of destination failed",
    "map_see_errno_mmap_tmem_munmap_nmem_failed",
      "mapping of destination and unmapping of temporary failed",
    "map_see_errno_mprotect_failed",
      "mprotect failed",
    "map_see_errno_mprotect_munmap_nmem_failed",
      "mprotect and unmapping of temporary failed",
    "map_see_errno_mprotect_munmaps_failed",
      "mprotect and unmappings failed",
    "map_see_errno_mprotect_munmap_tmem_failed",
      "mprotect and unmapping of destination failed",
    "map_see_errno_munmap_nmem_failed",
      "unmapping of temporary failed",
    "map_unsupported_platform",
      "mapping to large pages is not supported on this platform",
  };
  return map_status_text[((int)status << 1) + (fulltext & 1)];
}
