//#define DEBUG_SOCKET
#define DEBUG_IP "192.168.2.2"
#define DEBUG_PORT 9023

#include "ps4.h"
#include "main.h"

int nthread_run = 1;
char notify_buf[512] = {0};
configuration config;

void *nthread_func(void *arg) {
  UNUSED(arg);
  time_t t1 = 0;

  while (nthread_run) {
    if (notify_buf[0]) {
      time_t t2 = time(NULL);
      if ((t2 - t1) >= config.notify) {
        t1 = t2;
        printf_notification("%s", notify_buf);
      }
    } else {
      t1 = 0;
    }
    sceKernelSleep(1);
  }

  return NULL;
}

static int config_handler(void *user, const char *name, const char *value) {
  configuration *pconfig = (configuration *)user;

#define MATCH(n) strcmp(name, n) == 0

  if (MATCH("split")) {
    pconfig->split = atoi(value);
  } else if (MATCH("notify")) {
    pconfig->notify = atoi(value);
  } else if (MATCH("shutdown")) {
    pconfig->shutdown = atoi(value);
  }

  return 1;
}

int npbind_parse(const char *filename) {
  npbind_header header;
  memset(&header, 0, sizeof(npbind_header));

  int fd = open(filename, O_RDONLY, 0);
  if (fd < 0) {
    return 1;
  }

  lseek(fd, 0, SEEK_SET);
  if (sizeof(npbind_header) != read(fd, &header, sizeof(npbind_header))) {
    close(fd);
    return 2;
  }

  if (bswap32(header.magic) != NPBIND_MAGIC) {
    close(fd);
    return 3;
  }

  uint64_t entry_size = bswap64(header.entry_size);
  uint64_t file_size = bswap64(header.file_size);
  uint64_t num_entries = bswap64(header.num_entries);

  npbind_content content;
  memset(&content, 0, sizeof(npbind_content));
  content.header = header;

  lseek(fd, sizeof(npbind_header), SEEK_SET);

  for (uint64_t i = 0; i < num_entries; i++) {
    npbind_body body;
    uint64_t total = 0;

    while (total < entry_size) {
      uint16_t type;
      uint16_t size;

      if (sizeof(uint16_t) != read(fd, &type, sizeof(uint16_t)) ||
          sizeof(uint16_t) != read(fd, &size, sizeof(uint16_t))) {
        close(fd);
        return 4;
      }

      char data[bswap16(size)];

      if (bswap16(size) != read(fd, &data, bswap16(size))) {
        close(fd);
        return 5;
      }

      if (bswap16(type) == 0x0010) {
        npbind_npcommid_entry entry;
        entry.type = type;
        entry.size = size;
        memcpy(entry.data, data, sizeof(entry.data));
        body.npcommid = entry;
      } else if (bswap16(type) == 0x0011) {
        npbind_trophy_number_entry entry;
        entry.type = type;
        entry.size = size;
        memcpy(entry.data, data, sizeof(entry.data));
        body.trophy_number = entry;
      } else if (bswap16(type) == 0x0012) {
        npbind_unk1_entry entry;
        entry.type = type;
        entry.size = size;
        memcpy(entry.data, data, sizeof(entry.data));
        body.unk1 = entry;
      } else if (bswap16(type) == 0x0013) {
        npbind_unk2_entry entry;
        entry.type = type;
        entry.size = size;
        memcpy(entry.data, data, sizeof(entry.data));
        body.unk2 = entry;
      }

      total += (sizeof(uint16_t) * 2) + bswap16(size);
    }

    memset(&body.padding, 0, sizeof(body.padding));
  }

  char digest[0x14] = {0};

  lseek(fd, -(sizeof(digest)), SEEK_END);

  if (sizeof(digest) != read(fd, &digest, sizeof(digest))) {
    close(fd);
    return 6;
  }

  close(fd);

  content.header.magic = bswap32(content.header.magic);
  content.header.version = bswap32(content.header.version);
  content.header.file_size = bswap64(content.header.file_size);
  content.header.entry_size = bswap64(content.header.entry_size);
  content.header.num_entries = bswap64(content.header.num_entries);

  npbind_file file;
  file.content = content;
  memmove(file.digest, digest, sizeof(digest));

  return 0;
}

/*
 * Copy useful package metadata that may not be present in the mounted
 * application filesystem.
 *
 * Different firmware/install layouts can expose appmeta in different places,
 * so try both known locations.
 */
static void copy_appmeta_file(
    const char *title_id,
    const char *name,
    const char *dst_app) {

  char src[PATH_MAX] = {0};
  char dst[PATH_MAX] = {0};

  snprintf_s(dst, sizeof(dst), "%s/sce_sys/%s", dst_app, name);

  snprintf_s(
      src,
      sizeof(src),
      "/system_data/priv/appmeta/%s/%s",
      title_id,
      name);

  if (file_exists(src)) {
    copy_file(src, dst);
    return;
  }

  snprintf_s(
      src,
      sizeof(src),
      "/user/appmeta/%s/%s",
      title_id,
      name);

  if (file_exists(src)) {
    copy_file(src, dst);
  }
}

static void copy_appmeta(const char *title_id, const char *dst_app) {

  static const char *files[] = {
      "param.sfo",
      "icon0.png",
      "pic0.png",
      "pic1.png",
      "pronunciation.xml",
      "nptitle.dat",
      "npbind.dat"
  };

  unsigned int count = sizeof(files) / sizeof(files[0]);

  for (unsigned int i = 0; i < count; i++) {
    copy_appmeta_file(title_id, files[i], dst_app);
  }
}

void dump_app(char *title_id, char *usb_path) {

  char base_path[PATH_MAX] = {0};
  char src_path[PATH_MAX] = {0};
  char dst_file[PATH_MAX] = {0};
  char dst_app[PATH_MAX] = {0};
  char dst_pat[PATH_MAX] = {0};
  char dump_sem[PATH_MAX] = {0};
  char comp_sem[PATH_MAX] = {0};

  snprintf_s(base_path, sizeof(base_path), "%s/%s", usb_path, title_id);

  snprintf_s(dump_sem, sizeof(dump_sem), "%s.dumping", base_path);
  snprintf_s(comp_sem, sizeof(comp_sem), "%s.complete", base_path);

  unlink(comp_sem);
  touch_file(dump_sem);

  if (config.split) {

    snprintf_s(dst_app, sizeof(dst_app), "%s-app", base_path);
    snprintf_s(dst_pat, sizeof(dst_pat), "%s-patch", base_path);

    if (config.split & SPLIT_APP) {
      mkdir(dst_app, 0777);
    }

    if (config.split & SPLIT_PATCH) {
      mkdir(dst_pat, 0777);
    }

  } else {

    snprintf_s(dst_app, sizeof(dst_app), "%s", base_path);
    snprintf_s(dst_pat, sizeof(dst_pat), "%s", base_path);

    mkdir(base_path, 0777);
  }

  /*
   * BASE APPLICATION
   */
  if ((!config.split) || (config.split & SPLIT_APP)) {

    snprintf_s(
        src_path,
        sizeof(src_path),
        "/user/app/%s/app.pkg",
        title_id);

    printf_notification("Extracting app package...");
    unpkg(src_path, dst_app);

    /*
     * Keep the original appmeta files used by the upstream dumper.
     */
    snprintf_s(
        src_path,
        sizeof(src_path),
        "/system_data/priv/appmeta/%s/nptitle.dat",
        title_id);

    snprintf_s(
        dst_file,
        sizeof(dst_file),
        "%s/sce_sys/nptitle.dat",
        dst_app);

    if (file_exists(src_path)) {
      copy_file(src_path, dst_file);
    }

    snprintf_s(
        src_path,
        sizeof(src_path),
        "/system_data/priv/appmeta/%s/npbind.dat",
        title_id);

    snprintf_s(
        dst_file,
        sizeof(dst_file),
        "%s/sce_sys/npbind.dat",
        dst_app);

    if (file_exists(src_path)) {
      copy_file(src_path, dst_file);
    }

    /*
     * Extra metadata for easier GP4 / Fake-PKG rebuilding.
     */
    copy_appmeta(title_id, dst_app);
  }

  /*
   * PATCH
   */
  if ((!config.split) || (config.split & SPLIT_PATCH)) {

    snprintf_s(
        src_path,
        sizeof(src_path),
        "/user/patch/%s/patch.pkg",
        title_id);

    if (file_exists(src_path)) {

      if (config.split) {
        printf_notification("Extracting patch package...");
      } else {
        printf_notification("Merging patch package...");
      }

      unpkg(src_path, dst_pat);

      snprintf_s(
          src_path,
          sizeof(src_path),
          "/system_data/priv/appmeta/%s/nptitle.dat",
          title_id);

      snprintf_s(
          dst_file,
          sizeof(dst_file),
          "%s/sce_sys/nptitle.dat",
          dst_pat);

      if (file_exists(src_path)) {
        copy_file(src_path, dst_file);
      }

      snprintf_s(
          src_path,
          sizeof(src_path),
          "/system_data/priv/appmeta/%s/npbind.dat",
          title_id);

      snprintf_s(
          dst_file,
          sizeof(dst_file),
          "%s/sce_sys/npbind.dat",
          dst_pat);

      if (file_exists(src_path)) {
        copy_file(src_path, dst_file);
      }

      copy_appmeta(title_id, dst_pat);
    }
  }

  /*
   * Extract base PFS.
   */
  if ((!config.split) || (config.split & SPLIT_APP)) {

    snprintf_s(
        src_path,
        sizeof(src_path),
        "/mnt/sandbox/pfsmnt/%s-app0-nest/pfs_image.dat",
        title_id);

    printf_notification("Extracting app image...");
    unpfs(src_path, dst_app);
  }

  /*
   * Extract patch PFS when present.
   */
  if ((!config.split) || (config.split & SPLIT_PATCH)) {

    snprintf_s(
        src_path,
        sizeof(src_path),
        "/mnt/sandbox/pfsmnt/%s-patch0-nest/pfs_image.dat",
        title_id);

    if (file_exists(src_path)) {

      if (config.split) {
        printf_notification("Extracting patch image...");
      } else {
        printf_notification("Applying patch...");
      }

      unpfs(src_path, dst_pat);
    }
  }

  /*
   * Decrypt executable SELF/PRX files from the live base mount.
   */
  if ((!config.split) || (config.split & SPLIT_APP)) {

    snprintf_s(
        src_path,
        sizeof(src_path),
        "/mnt/sandbox/pfsmnt/%s-app0",
        title_id);

    printf_notification("Decrypting selfs...");
    decrypt_dir(src_path, dst_app);
  }

  /*
   * Decrypt patch SELF/PRX files when a patch mount exists.
   */
  if ((!config.split) || (config.split & SPLIT_PATCH)) {

    snprintf_s(
        src_path,
        sizeof(src_path),
        "/mnt/sandbox/pfsmnt/%s-patch0",
        title_id);

    if (file_exists(src_path)) {
      printf_notification("Decrypting patch...");
      decrypt_dir(src_path, dst_pat);
    }
  }

  unlink(dump_sem);
  touch_file(comp_sem);
}

int _main(struct thread *td) {

  UNUSED(td);

  /*
   * IMPORTANT:
   * Do not hard-code a CUSA here.
   * wait_for_app() fills this with the currently running application.
   */
  char title_id[10] = {0};

  char usb_name[7] = {0};
  char usb_path[13] = {0};
  char cfg_path[PATH_MAX] = {0};

  initKernel();
  initLibc();
  initPthread();

#ifdef DEBUG_SOCKET
  initNetwork();
  DEBUG_SOCK = SckConnect(DEBUG_IP, DEBUG_PORT);
#endif

  jailbreak();
  mmap_patch();

  initSysUtil();

  config.split = 3;
  config.notify = 60;
  config.shutdown = 0;

  ScePthread nthread;

  memset_s(
      &nthread,
      sizeof(ScePthread),
      0,
      sizeof(ScePthread));

  scePthreadCreate(
      &nthread,
      NULL,
      nthread_func,
      NULL,
      "nthread");

  printf_notification("Running App Dumper");

  snprintf_s(
      notify_buf,
      sizeof(notify_buf),
      "Waiting for USB device...");

  wait_for_usb(usb_name, usb_path);

  notify_buf[0] = '\0';

  snprintf_s(
      cfg_path,
      sizeof(cfg_path),
      "%s/dumper.cfg",
      usb_path);

  cfg_parse(
      cfg_path,
      config_handler,
      &config);

  /*
   * Automatically detect the currently running application.
   * If none is running yet, wait until one launches.
   */
  if (!wait_for_app(title_id)) {

    snprintf_s(
        notify_buf,
        sizeof(notify_buf),
        "Waiting for application to launch...");

    do {
      sceKernelSleep(1);
    } while (!wait_for_app(title_id));

    notify_buf[0] = '\0';
  }

  /*
   * Wait for disc/install copying if applicable.
   */
  if (wait_for_bdcopy(title_id) < 100) {

    int progress;

    do {

      sceKernelSleep(1);

      progress = wait_for_bdcopy(title_id);

      snprintf_s(
          notify_buf,
          sizeof(notify_buf),
          "Waiting for application to copy\n%i%% completed...",
          progress);

    } while (progress < 100);

    notify_buf[0] = '\0';
  }

  nthread_run = 0;

  printf_notification(
      "Start dumping\n%s to %s",
      title_id,
      usb_name);

  sceKernelSleep(5);

  dump_app(title_id, usb_path);

  if (config.shutdown) {

    printf_notification(
        "%s dumped.\nShutting down...",
        title_id);

  } else {

    printf_notification(
        "%s dumped.\nQuitting...",
        title_id);
  }

#ifdef DEBUG_SOCKET
  printf_debug("Closing socket...\n");
  SckClose(DEBUG_SOCK);
#endif

  if (config.shutdown) {
    sceKernelSleep(10);
    reboot();
  }

  return 0;
}
