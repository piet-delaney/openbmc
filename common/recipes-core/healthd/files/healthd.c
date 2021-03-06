/*
 * healthd
 *
 * Copyright 2015-present Facebook. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <pthread.h>
#include <jansson.h>
#include <stdbool.h>
#include <openbmc/pal.h>
#include <sys/sysinfo.h>
#include <sys/reboot.h>
#include "watchdog.h"
#include <openbmc/pal.h>
#include <openbmc/kv.h>
#include <openbmc/obmc-i2c.h>

#define I2C_BUS_NUM            14
#define AST_I2C_BASE           0x1E78A000  /* I2C */
#define I2C_CMD_REG            0x14
#define AST_I2CD_SCL_LINE_STS  (0x1 << 18)
#define AST_I2CD_SDA_LINE_STS  (0x1 << 17)
#define AST_I2CD_BUS_BUSY_STS  (0x1 << 16)
#define PAGE_SIZE              0x1000
#define MIN_THRESHOLD          60.0
#define MAX_THRESHOLD          95.0

struct i2c_bus_s {
  uint32_t offset;
  char     *name;
  bool     enabled;
};
struct i2c_bus_s ast_i2c_dev_offset[I2C_BUS_NUM] = {
  {0x040,  "I2C DEV1 OFFSET", false},
  {0x080,  "I2C DEV2 OFFSET", false},
  {0x0C0,  "I2C DEV3 OFFSET", false},
  {0x100,  "I2C DEV4 OFFSET", false},
  {0x140,  "I2C DEV5 OFFSET", false},
  {0x180,  "I2C DEV6 OFFSET", false},
  {0x1C0,  "I2C DEV7 OFFSET", false},
  {0x300,  "I2C DEV8 OFFSET", false},
  {0x340,  "I2C DEV9 OFFSET", false},
  {0x380,  "I2C DEV10 OFFSET", false},
  {0x3C0,  "I2C DEV11 OFFSET", false},
  {0x400,  "I2C DEV12 OFFSET", false},
  {0x440,  "I2C DEV13 OFFSET", false},
  {0x480,  "I2C DEV14 OFFSET", false},
};
enum {
  BUS_LOCK_RECOVER_ERROR = 0,
  BUS_LOCK_RECOVER_TIMEOUT,
  BUS_LOCK_RECOVER_SUCCESS,
  BUS_LOCK_PRESERVE,
  SLAVE_DEAD_RECOVER_ERROR,
  SLAVE_DEAD_RECOVER_TIMEOUT,
  SLAVE_DEAD_RECOVER_SUCCESS,
  SLAVE_DEAD_PRESERVE,
  UNDEFINED_CASE,
};

#define CPU_INFO_PATH "/proc/stat"
#define CPU_NAME_LENGTH 10
#define DEFAULT_WINDOW_SIZE 120
#define DEFAULT_MONITOR_INTERVAL 1
#define HEALTHD_MAX_RETRY 10
#define CONFIG_PATH "/etc/healthd-config.json"

struct threshold_s {
  float value;
  float hysteresis;
  bool asserted;
  bool log;
  int log_level;
  bool reboot;
  bool bmc_error_trigger;
};

#define AST_MCR_BASE 0x1e6e0000 // Base Address of SDRAM Memory Controller
#define INTR_CTRL_STS_OFFSET 0x50 // Interrupt Control/Status Register
#define ADDR_FIRST_UNRECOVER_ECC_OFFSET 0x58 // Address of First Un-Recoverable ECC Error Addr
#define ADDR_LAST_RECOVER_ECC_OFFSET 0x5c // Address of Last Recoverable ECC Error Addr
#define MAX_ECC_RECOVERABLE_ERROR_COUNTER 255
#define MAX_ECC_UNRECOVERABLE_ERROR_COUNTER 15

#define VERIFIED_BOOT_STRUCT_BASE     0x1E720000
#define VERIFIED_BOOT_RECOVERY_FLAG(base)  *((uint8_t *)(base + 0x217))
#define VERIFIED_BOOT_ERROR_TYPE(base)  *((uint8_t *)(base + 0x219))
#define VERIFIED_BOOT_ERROR_CODE(base)  *((uint8_t *)(base + 0x21A))
#define VERIFIED_BOOT_ERROR_TPM(base)  *((uint8_t *)(base + 0x21B))

#define BMC_HEALTH_FILE "bmc_health"
#define HEALTH "1"
#define NOT_HEALTH "0"

#define VM_PANIC_ON_OOM_FILE "/proc/sys/vm/panic_on_oom"

enum ASSERT_BIT {
  BIT_CPU_OVER_THRESHOLD = 0,
  BIT_MEM_OVER_THRESHOLD = 1,
  BIT_RECOVERABLE_ECC    = 2,
  BIT_UNRECOVERABLE_ECC  = 3,
};

/* Heartbeat configuration */
static unsigned int hb_interval = 500;

/* CPU configuration */
static char *cpu_monitor_name = "BMC CPU utilization";
static bool cpu_monitor_enabled = false;
static unsigned int cpu_window_size = DEFAULT_WINDOW_SIZE;
static unsigned int cpu_monitor_interval = DEFAULT_MONITOR_INTERVAL;
static struct threshold_s *cpu_threshold;
static size_t cpu_threshold_num = 0;

/* Memory monitor enabled */
static char *mem_monitor_name = "BMC Memory utilization";
static bool mem_monitor_enabled = false;
static bool mem_enable_panic = false;
static unsigned int mem_window_size = DEFAULT_WINDOW_SIZE;
static unsigned int mem_monitor_interval = DEFAULT_MONITOR_INTERVAL;
static struct threshold_s *mem_threshold;
static size_t mem_threshold_num = 0;

static pthread_mutex_t global_error_mutex = PTHREAD_MUTEX_INITIALIZER;
static int bmc_health = 0; // CPU/MEM/ECC error flag

/* I2C Monitor enabled */
static bool i2c_monitor_enabled = false;

/* ECC configuration */
static char *recoverable_ecc_name = "ECC Recoverable Error";
static char *unrecoverable_ecc_name = "ECC Unrecoverable Error";
static bool ecc_monitor_enabled = false;
static unsigned int ecc_monitor_interval = DEFAULT_MONITOR_INTERVAL;
// to show the address of ecc error or not. supported chip: AST2500 serials
static bool ecc_addr_log = false;
static struct threshold_s *recov_ecc_threshold;
static size_t recov_ecc_threshold_num = 0;
static struct threshold_s *unrec_ecc_threshold;
static size_t unrec_ecc_threshold_num = 0;
static unsigned int ecc_recov_max_counter = MAX_ECC_RECOVERABLE_ERROR_COUNTER;
static unsigned int ecc_unrec_max_counter = MAX_ECC_UNRECOVERABLE_ERROR_COUNTER;

/* BMC Health Monitor */
static bool regen_log_enabled = false;
static unsigned int bmc_health_monitor_interval = DEFAULT_MONITOR_INTERVAL;
static int regen_interval = 1200;

/* Node Manager Monitor enabled */
static bool nm_monitor_enabled = false;
static int nm_monitor_interval = DEFAULT_MONITOR_INTERVAL;
static unsigned char nm_retry_threshold = 0;

static bool vboot_state_check = false;

static void
initialize_threshold(const char *target, json_t *thres, struct threshold_s *t) {
  json_t *tmp;
  size_t i;
  size_t act_size;

  tmp = json_object_get(thres, "value");
  if (!tmp || !json_is_real(tmp)) {
    return;
  }
  t->value = json_real_value(tmp);

  /* Do not let the value (CPU/MEM thresholds) exceed these safe ranges */
  if ((strcasestr(target, "CPU") != 0ULL) ||
      (strcasestr(target, "MEM") != 0ULL)) {
    if (t->value > MAX_THRESHOLD) {
      syslog(LOG_WARNING,
             "%s: user setting %s threshold %f is too high and set threshold as %f",
             __func__, target, t->value, MAX_THRESHOLD);
      t->value = MAX_THRESHOLD;
    }
    if (t->value < MIN_THRESHOLD) {
      syslog(LOG_WARNING,
             "%s: user setting %s threshold %f is too low and set threshold as %f",
             __func__, target, t->value, MIN_THRESHOLD);
      t->value = MIN_THRESHOLD;
    }
  }

  tmp = json_object_get(thres, "hysteresis");
  if (tmp && json_is_real(tmp)) {
    t->hysteresis = json_real_value(tmp);
  }
  tmp = json_object_get(thres, "action");
  if (!tmp || !json_is_array(tmp)) {
    return;
  }
  act_size = json_array_size(tmp);
  for(i = 0; i < act_size; i++) {
    const char *act;
    json_t *act_o = json_array_get(tmp, i);
    if (!act_o || !json_is_string(act_o)) {
      continue;
    }
    act = json_string_value(act_o);
    if (!strcmp(act, "log-warning")) {
      t->log_level = LOG_WARNING;
      t->log = true;
    } else if(!strcmp(act, "log-critical")) {
      t->log_level = LOG_CRIT;
      t->log = true;
    } else if (!strcmp(act, "reboot")) {
      t->reboot = true;
    } else if(!strcmp(act, "bmc-error-trigger")) {
      t->bmc_error_trigger = true;
    }
  }

  if (tmp && json_is_boolean(tmp)) {
    t->reboot = json_is_true(tmp);
  }
}

static void initialize_thresholds(const char *target, json_t *array, struct threshold_s **out_arr, size_t *out_len) {
  size_t size = json_array_size(array);
  size_t i;
  struct threshold_s *thres;

  if (size == 0) {
    return;
  }
  thres = *out_arr = calloc(size, sizeof(struct threshold_s));
  if (!thres) {
    return;
  }
  *out_len = size;

  for(i = 0; i < size; i++) {
    json_t *e = json_array_get(array, i);
    if (!e) {
      continue;
    }
    initialize_threshold(target, e, &thres[i]);
  }
}

static void
initialize_hb_config(json_t *conf) {
  json_t *tmp;

  tmp = json_object_get(conf, "interval");
  if (!tmp || !json_is_number(tmp)) {
    return;
  }
  hb_interval = json_integer_value(tmp);
}

static void
initialize_cpu_config(json_t *conf) {
  json_t *tmp;

  tmp = json_object_get(conf, "enabled");
  if (!tmp || !json_is_boolean(tmp)) {
    return;
  }
  cpu_monitor_enabled = json_is_true(tmp);
  if (!cpu_monitor_enabled) {
    return;
  }
  tmp = json_object_get(conf, "window_size");
  if (tmp && json_is_number(tmp)) {
    cpu_window_size = json_integer_value(tmp);
  }
  tmp = json_object_get(conf, "monitor_interval");
  if (tmp && json_is_number(tmp)) {
    cpu_monitor_interval = json_integer_value(tmp);
    if (cpu_monitor_interval <= 0)
      cpu_monitor_interval = DEFAULT_MONITOR_INTERVAL;
  }
  tmp = json_object_get(conf, "threshold");
  if (!tmp || !json_is_array(tmp)) {
    cpu_monitor_enabled = false;
    return;
  }
  initialize_thresholds(cpu_monitor_name, tmp, &cpu_threshold, &cpu_threshold_num);
}

static void
initialize_mem_config(json_t *conf) {
  json_t *tmp;

  tmp = json_object_get(conf, "enabled");
  if (!tmp || !json_is_boolean(tmp)) {
    return;
  }
  mem_monitor_enabled = json_is_true(tmp);
  if (!mem_monitor_enabled) {
    return;
  }
  tmp = json_object_get(conf, "enable_panic_on_oom");
  if (tmp && json_is_true(tmp)) {
    mem_enable_panic = true;
  }
  tmp = json_object_get(conf, "window_size");
  if (tmp && json_is_number(tmp)) {
    mem_window_size = json_integer_value(tmp);
  }
  tmp = json_object_get(conf, "monitor_interval");
  if (tmp && json_is_number(tmp)) {
    mem_monitor_interval = json_integer_value(tmp);
    if (mem_monitor_interval <= 0)
      mem_monitor_interval = DEFAULT_MONITOR_INTERVAL;
  }
  tmp = json_object_get(conf, "threshold");
  if (!tmp || !json_is_array(tmp)) {
    mem_monitor_enabled = false;
    return;
  }
  initialize_thresholds(mem_monitor_name, tmp, &mem_threshold, &mem_threshold_num);
}

static void
initialize_i2c_config(json_t *conf) {
  json_t *tmp;
  size_t i;
  size_t i2c_num_busses;

  tmp = json_object_get(conf, "enabled");
  if (!tmp || !json_is_boolean(tmp)) {
    return;
  }
  i2c_monitor_enabled = json_is_true(tmp);
  if (!i2c_monitor_enabled) {
    return;
  }
  tmp = json_object_get(conf, "busses");
  if (!tmp || !json_is_array(tmp)) {
    goto error_bail;
  }
  i2c_num_busses = json_array_size(tmp);
  if (!i2c_num_busses) {
    /* Nothing to monitor */
    goto error_bail;
  }
  for(i = 0; i < i2c_num_busses; i++) {
    size_t bus;
    json_t *ind = json_array_get(tmp, i);
    if (!ind || !json_is_number(ind)) {
      goto error_bail;
    }
    bus = json_integer_value(ind);
    if (bus >= I2C_BUS_NUM) {
      syslog(LOG_CRIT, "HEALTHD: Warning: Ignoring unsupported I2C Bus:%u\n", (unsigned int)bus);
      continue;
    }
    ast_i2c_dev_offset[bus].enabled = true;
  }
  return;
error_bail:
  i2c_monitor_enabled = false;
}

static void
initialize_ecc_config(json_t *conf) {
  json_t *tmp;

  tmp = json_object_get(conf, "enabled");
  if (!tmp || !json_is_boolean(tmp)) {
    return;
  }
  ecc_monitor_enabled = json_is_true(tmp);
  if (!ecc_monitor_enabled) {
    return;
  }
  tmp = json_object_get(conf, "ecc_address_log");
  if (tmp || json_is_boolean(tmp)) {
    ecc_addr_log = json_is_true(tmp);
  }
  tmp = json_object_get(conf, "monitor_interval");
  if (tmp && json_is_number(tmp)) {
    ecc_monitor_interval = json_integer_value(tmp);
  }
  tmp = json_object_get(conf, "recov_max_counter");
  if (tmp && json_is_number(tmp)) {
    ecc_recov_max_counter = json_integer_value(tmp);
  }
  tmp = json_object_get(conf, "unrec_max_counter");
  if (tmp && json_is_number(tmp)) {
    ecc_unrec_max_counter = json_integer_value(tmp);
  }
  tmp = json_object_get(conf, "recov_threshold");
  if (tmp || json_is_array(tmp)) {
    initialize_thresholds(recoverable_ecc_name, tmp, &recov_ecc_threshold, &recov_ecc_threshold_num);
  }
  tmp = json_object_get(conf, "unrec_threshold");
  if (tmp || json_is_array(tmp)) {
    initialize_thresholds(unrecoverable_ecc_name, tmp, &unrec_ecc_threshold, &unrec_ecc_threshold_num);
  }
}

static void
initialize_bmc_health_config(json_t *conf) {
  json_t *tmp;

  tmp = json_object_get(conf, "enabled");
  if (tmp || json_is_boolean(tmp)) {
    regen_log_enabled = json_is_true(tmp);
  }
  tmp = json_object_get(conf, "monitor_interval");
  if (tmp && json_is_number(tmp)) {
    bmc_health_monitor_interval = json_integer_value(tmp);
    if (bmc_health_monitor_interval <= 0)
      bmc_health_monitor_interval = DEFAULT_MONITOR_INTERVAL;
  }
  tmp = json_object_get(conf, "regenerating_interval");
  if (tmp && json_is_number(tmp)) {
    regen_interval = json_integer_value(tmp);
  }
}

static void
initialize_nm_monitor_config(json_t *conf)
{
  json_t *tmp;

  tmp = json_object_get(conf, "enabled");
  if (!tmp || !json_is_boolean(tmp))
  {
    goto error_bail;
  }

  nm_monitor_enabled = json_is_true(tmp);
  if ( !nm_monitor_enabled )
  {
    goto error_bail;
  }

  tmp = json_object_get(conf, "monitor_interval");
  if (tmp && json_is_number(tmp))
  {
    nm_monitor_interval = json_integer_value(tmp);
    if (nm_monitor_interval <= 0)
    {
      nm_monitor_interval = DEFAULT_MONITOR_INTERVAL;
    }
  }

  tmp = json_object_get(conf, "retry_threshold");
  if (tmp && json_is_number(tmp))
  {
    nm_retry_threshold = json_integer_value(tmp);
  }
#ifdef DEBUG
  syslog(LOG_WARNING, "enabled:%d, monitor_interval:%d, retry_threshold:%d", nm_monitor_enabled, nm_monitor_interval, nm_retry_threshold);
#endif
  return;

error_bail:
  nm_monitor_enabled = false;
}

static void initialize_vboot_config(json_t *obj)
{
  json_t *tmp;

  if (!obj) {
    return;
  }
  tmp = json_object_get(obj, "enabled");
  if (!tmp || !json_is_boolean(tmp)) {
    return;
  }
  vboot_state_check = json_is_true(tmp);
}

static int
initialize_configuration(void) {
  json_error_t error;
  json_t *conf;
  json_t *v;

  conf = json_load_file(CONFIG_PATH, 0, &error);
  if (!conf) {
    syslog(LOG_CRIT, "HEALTHD configuration load failed");
    return -1;
  }
  v = json_object_get(conf, "version");
  if (v && json_is_string(v)) {
    syslog(LOG_INFO, "Loaded configuration version: %s\n", json_string_value(v));
  }
  initialize_hb_config(json_object_get(conf, "heartbeat"));
  initialize_cpu_config(json_object_get(conf, "bmc_cpu_utilization"));
  initialize_mem_config(json_object_get(conf, "bmc_mem_utilization"));
  initialize_i2c_config(json_object_get(conf, "i2c"));
  initialize_ecc_config(json_object_get(conf, "ecc_monitoring"));
  initialize_bmc_health_config(json_object_get(conf, "bmc_health"));
  initialize_nm_monitor_config(json_object_get(conf, "nm_monitor"));
  initialize_vboot_config(json_object_get(conf, "verified_boot"));

  json_decref(conf);

  return 0;
}

static void threshold_assert_check(const char *target, float value, struct threshold_s *thres) {

  struct sysinfo info;

  if (!thres->asserted && value >= thres->value) {
    thres->asserted = true;
    if (thres->log) {
      syslog(thres->log_level, "ASSERT: %s (%.2f%%) exceeds the threshold (%.2f%%).\n", target, value, thres->value);
    }
    if (thres->reboot) {
      sysinfo(&info);
      syslog(thres->log_level, "Rebooting BMC; latest uptime: %ld sec", info.uptime);

      sleep(1);
      reboot(RB_AUTOBOOT);
    }
    if (thres->bmc_error_trigger) {
      pthread_mutex_lock(&global_error_mutex);
      if (!bmc_health) { // assert bmc_health key only when not yet set
        pal_set_key_value(BMC_HEALTH_FILE, NOT_HEALTH);
      }
      if (strcasestr(target, "CPU") != 0ULL) {
        bmc_health = SETBIT(bmc_health, BIT_CPU_OVER_THRESHOLD);
      } else if (strcasestr(target, "Mem") != 0ULL) {
        bmc_health = SETBIT(bmc_health, BIT_MEM_OVER_THRESHOLD);
      } else {
        pthread_mutex_unlock(&global_error_mutex);
        return;
      }
      pthread_mutex_unlock(&global_error_mutex);
      pal_bmc_err_enable(target);
    }
  }
}

static void threshold_deassert_check(const char *target, float value, struct threshold_s *thres) {
  if (thres->asserted && value < (thres->value - thres->hysteresis)) {
    thres->asserted = false;
    if (thres->log) {
      syslog(thres->log_level, "DEASSERT: %s (%.2f%%) is under the threshold (%.2f%%).\n", target, value, thres->value);
    }
    if (thres->bmc_error_trigger) {
      pthread_mutex_lock(&global_error_mutex);
      if (strcasestr(target, "CPU") != 0ULL) {
        bmc_health = CLEARBIT(bmc_health, BIT_CPU_OVER_THRESHOLD);
      } else if (strcasestr(target, "Mem") != 0ULL) {
        bmc_health = CLEARBIT(bmc_health, BIT_MEM_OVER_THRESHOLD);
      } else {
        pthread_mutex_unlock(&global_error_mutex);
        return;
      }
      if (!bmc_health) { // deassert bmc_health key if no any error bit assertion
        pal_set_key_value(BMC_HEALTH_FILE, HEALTH);
      }
      pthread_mutex_unlock(&global_error_mutex);
      pal_bmc_err_disable(target);
    }
  }
}

static void
threshold_check(const char *target, float value, struct threshold_s *thresholds, size_t num) {
  size_t i;

  for(i = 0; i < num; i++) {
    threshold_assert_check(target, value, &thresholds[i]);
    threshold_deassert_check(target, value, &thresholds[i]);
  }
}

static void ecc_threshold_assert_check(const char *target, int value,
                                       struct threshold_s *thres, uint32_t ecc_err_addr) {
  int thres_counter = 0;

  if (strcasestr(target, "Unrecover") != 0ULL) {
    thres_counter = (ecc_unrec_max_counter * thres->value / 100);
  } else if (strcasestr(target, "Recover") != 0ULL) {
    thres_counter = (ecc_recov_max_counter * thres->value / 100);
  } else {
    return;
  }
  if (!thres->asserted && value > thres_counter) {
    thres->asserted = true;
    if (thres->log) {
      if (ecc_addr_log) {
        syslog(LOG_CRIT, "%s occurred (over %d%%) "
            "Counter = %d Address of last recoverable ECC error = 0x%x",
            target, (int)thres->value, value, (ecc_err_addr >> 4) & 0xFFFFFFFF);
      } else {
        syslog(LOG_CRIT, "ECC occurred (over %d%%): %s Counter = %d",
            (int)thres->value, target, value);
      }
    }
    if (thres->reboot) {
      reboot(RB_AUTOBOOT);
    }
    if (thres->bmc_error_trigger) {
      pthread_mutex_lock(&global_error_mutex);
      if (!bmc_health) { // assert in bmc_health key only when not yet set
        pal_set_key_value(BMC_HEALTH_FILE, NOT_HEALTH);
      }
      if (strcasestr(target, "Unrecover") != 0ULL) {
        bmc_health = SETBIT(bmc_health, BIT_UNRECOVERABLE_ECC);
      } else if (strcasestr(target, "Recover") != 0ULL) {
        bmc_health = SETBIT(bmc_health, BIT_RECOVERABLE_ECC);
      } else {
        pthread_mutex_unlock(&global_error_mutex);
        return;
      }
      pthread_mutex_unlock(&global_error_mutex);
      pal_bmc_err_enable(target);
    }
  }
}

static void
ecc_threshold_check(const char *target, int value, struct threshold_s *thresholds,
                    size_t num, uint32_t ecc_err_addr) {
  size_t i;

  for(i = 0; i < num; i++) {
    ecc_threshold_assert_check(target, value, &thresholds[i], ecc_err_addr);
  }
}

static void
initilize_all_kv() {
  pal_set_def_key_value();
}

static void *
hb_handler() {
  while(1) {
    /* Turn ON the HB Led*/
    pal_set_hb_led(1);
    msleep(hb_interval);

    /* Turn OFF the HB led */
    pal_set_hb_led(0);
    msleep(hb_interval);
  }
  return NULL;
}

static void *
watchdog_handler() {

  /* Start watchdog in manual mode */
  start_watchdog(0);

  /* Set watchdog to persistent mode so timer expiry will happen independent
   * of this process's liveliness.
   */
  set_persistent_watchdog(WATCHDOG_SET_PERSISTENT);

  while(1) {

    sleep(5);

    /*
     * Restart the watchdog countdown. If this process is terminated,
     * the persistent watchdog setting will cause the system to reboot after
     * the watchdog timeout.
     */
    kick_watchdog();
  }
  return NULL;
}

static void *
i2c_mon_handler() {
  char i2c_bus_device[16];
  int dev;
  int bus_status = 0;
  int asserted_flag[I2C_BUS_NUM] = {};
  bool assert_handle = 0;
  int i;

  while (1) {
    for (i = 0; i < I2C_BUS_NUM; i++) {
      if (!ast_i2c_dev_offset[i].enabled) {
        continue;
      }
      sprintf(i2c_bus_device, "/dev/i2c-%d", i);
      dev = open(i2c_bus_device, O_RDWR);
      if (dev < 0) {
        syslog(LOG_DEBUG, "%s(): open() failed", __func__);
        continue;
      }
      bus_status = i2c_smbus_status(dev);
      close(dev);

      assert_handle = 0;
      if (bus_status == 0) {
        /* Bus status is normal */
        if (asserted_flag[i] != 0) {
          asserted_flag[i] = 0;
          syslog(LOG_CRIT, "DEASSERT: I2C(%d) Bus recoveried. (I2C bus index base 0)", i);
          pal_i2c_crash_deassert_handle(i);
        }
      } else {
        /* Check each case */
        if (GETBIT(bus_status, BUS_LOCK_RECOVER_ERROR)
            && !GETBIT(asserted_flag[i], BUS_LOCK_RECOVER_ERROR)) {
          asserted_flag[i] = SETBIT(asserted_flag[i], BUS_LOCK_RECOVER_ERROR);
          syslog(LOG_CRIT, "ASSERT: I2C(%d) bus is locked (Master Lock or Slave Clock Stretch). "
                           "Recovery error. (I2C bus index base 0)", i);
          assert_handle = 1;
        }
        bus_status = CLEARBIT(bus_status, BUS_LOCK_RECOVER_ERROR);
        if (GETBIT(bus_status, BUS_LOCK_RECOVER_TIMEOUT)
            && !GETBIT(asserted_flag[i], BUS_LOCK_RECOVER_TIMEOUT)) {
          asserted_flag[i] = SETBIT(asserted_flag[i], BUS_LOCK_RECOVER_TIMEOUT);
          syslog(LOG_CRIT, "ASSERT: I2C(%d) bus is locked (Master Lock or Slave Clock Stretch). "
                           "Recovery timed out. (I2C bus index base 0)", i);
          assert_handle = 1;
        }
        bus_status = CLEARBIT(bus_status, BUS_LOCK_RECOVER_TIMEOUT);
        if (GETBIT(bus_status, BUS_LOCK_RECOVER_SUCCESS)) {
          syslog(LOG_CRIT, "I2C(%d) bus had been locked (Master Lock or Slave Clock Stretch) "
                           "and has been recoveried successfully. (I2C bus index base 0)", i);
        }
        bus_status = CLEARBIT(bus_status, BUS_LOCK_RECOVER_SUCCESS);
        if (GETBIT(bus_status, SLAVE_DEAD_RECOVER_ERROR)
            && !GETBIT(asserted_flag[i], SLAVE_DEAD_RECOVER_ERROR)) {
          asserted_flag[i] = SETBIT(asserted_flag[i], SLAVE_DEAD_RECOVER_ERROR);
          syslog(LOG_CRIT, "ASSERT: I2C(%d) Slave is dead (SDA keeps low). "
                           "Bus recovery error. (I2C bus index base 0)", i);
          assert_handle = 1;
        }
        bus_status = CLEARBIT(bus_status, SLAVE_DEAD_RECOVER_ERROR);
        if (GETBIT(bus_status, SLAVE_DEAD_RECOVER_TIMEOUT)
            && !GETBIT(asserted_flag[i], SLAVE_DEAD_RECOVER_TIMEOUT)) {
          asserted_flag[i] = SETBIT(asserted_flag[i], SLAVE_DEAD_RECOVER_TIMEOUT);
          syslog(LOG_CRIT, "ASSERT: I2C(%d) Slave is dead (SDAs keep low). "
                           "Bus recovery timed out. (I2C bus index base 0)", i);
          assert_handle = 1;
        }
        bus_status = CLEARBIT(bus_status, SLAVE_DEAD_RECOVER_TIMEOUT);
        if (GETBIT(bus_status, SLAVE_DEAD_RECOVER_SUCCESS)) {
          syslog(LOG_CRIT, "I2C(%d) Slave was dead. and bus has been recoveried successfully. "
                           "(I2C bus index base 0)", i);
        }
        bus_status = CLEARBIT(bus_status, SLAVE_DEAD_RECOVER_SUCCESS);
        /* Check if any undefined bit remain in bus_status */
        if ((bus_status != 0) && !GETBIT(asserted_flag[i], UNDEFINED_CASE)) {
          asserted_flag[i] = SETBIT(asserted_flag[i], 8);
          syslog(LOG_CRIT, "ASSERT: I2C(%d) Undefined case. (I2C bus index base 0)", i);
          assert_handle = 1;
        }

        if (assert_handle) {
          pal_i2c_crash_assert_handle(i);
        }
      }
    }
    sleep(30);
  }
  return NULL;
}

static void *
CPU_usage_monitor() {
  unsigned long long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
  unsigned long long total_diff, idle_diff, non_idle, idle_time = 0, total = 0, pre_total = 0, pre_idle = 0;
  char cpu[CPU_NAME_LENGTH] = {0};
  int i, ready_flag = 0, timer = 0, retry = 0;
  float cpu_util_avg, cpu_util_total;
  float cpu_utilization[cpu_window_size];
  FILE *fp;

  memset(cpu_utilization, 0, sizeof(float) * cpu_window_size);

  while (1) {

    if (retry > HEALTHD_MAX_RETRY) {
      syslog(LOG_CRIT, "Cannot get CPU statistics. Stop %s\n", __func__);
      return NULL;
    }

    // Get CPU statistics. Time unit: jiffies
    fp = fopen(CPU_INFO_PATH, "r");
    if(!fp) {
      syslog(LOG_WARNING, "Failed to get CPU statistics.\n");
      retry++;
      continue;
    }
    retry = 0;

    fscanf(fp, "%s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                cpu, &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);

    fclose(fp);

    timer %= cpu_window_size;

    // Need more data to cacluate the avg. utilization. We average 60 records here.
    if (timer == (cpu_window_size-1) && !ready_flag)
      ready_flag = 1;


    // guset and guest_nice are already accounted in user and nice so they are not included in total caculation
    idle_time = idle + iowait;
    non_idle = user + nice + system + irq + softirq + steal;
    total = idle_time + non_idle;

    // For runtime caculation, we need to take into account previous value.
    total_diff = total - pre_total;
    idle_diff = idle_time - pre_idle;

    // These records are used to caculate the avg. utilization.
    cpu_utilization[timer] = (float) (total_diff - idle_diff)/total_diff;

    // Start to average the cpu utilization
    if (ready_flag) {
      cpu_util_total = 0;
      for (i=0; i<cpu_window_size; i++) {
        cpu_util_total += cpu_utilization[i];
      }
      cpu_util_avg = (cpu_util_total/cpu_window_size) * 100.0;
      threshold_check(cpu_monitor_name, cpu_util_avg, cpu_threshold, cpu_threshold_num);
    }

    // Record current value for next caculation
    pre_total = total;
    pre_idle  = idle_time;

    timer++;
    sleep(cpu_monitor_interval);
  }
  return NULL;
}

static int set_panic_on_oom(void) {

  FILE *fp;
  int ret;
  int tmp_value;

  fp = fopen(VM_PANIC_ON_OOM_FILE, "r+");
  if (fp == NULL) {
    syslog(LOG_CRIT, "%s: failed to open file: %s", __func__, VM_PANIC_ON_OOM_FILE);
    return -1;
  }

  ret = fscanf(fp, "%d", &tmp_value);
  if (ret != 1) {
    syslog(LOG_CRIT, "%s: failed to read file: %s", __func__, VM_PANIC_ON_OOM_FILE);
    fclose(fp);
    return -1;
  }

  // if /proc/sys/vm/panic_on_oom is 0; set it to 1
  if (tmp_value == 0) {
    fseek(fp, 0, SEEK_SET);
    ret = fputs("1", fp);
    if (ret < 0) {
      syslog(LOG_CRIT, "%s: failed to write to file: %s", __func__, VM_PANIC_ON_OOM_FILE);
      fclose(fp);
      return -1;
    }
  }

  fclose(fp);
  return 0;
}

static void *
memory_usage_monitor() {
  struct sysinfo s_info;
  int i, error, timer = 0, ready_flag = 0, retry = 0;
  float mem_util_avg, mem_util_total;
  float mem_utilization[mem_window_size];

  memset(mem_utilization, 0, sizeof(float) * mem_window_size);

  if (mem_enable_panic) {
    set_panic_on_oom();
  }

  while (1) {

    if (retry > HEALTHD_MAX_RETRY) {
      syslog(LOG_CRIT, "Cannot get sysinfo. Stop the %s\n", __func__);
      return NULL;
    }

    timer %= mem_window_size;

    // Need more data to cacluate the avg. utilization. We average 60 records here.
    if (timer == (mem_window_size-1) && !ready_flag)
      ready_flag = 1;

    // Get sys info
    error = sysinfo(&s_info);
    if (error) {
      syslog(LOG_WARNING, "%s Failed to get sys info. Error: %d\n", __func__, error);
      retry++;
      continue;
    }
    retry = 0;

    // These records are used to caculate the avg. utilization.
    mem_utilization[timer] = (float) (s_info.totalram - s_info.freeram)/s_info.totalram;

    // Start to average the memory utilization
    if (ready_flag) {
      mem_util_total = 0;
      for (i=0; i<mem_window_size; i++)
        mem_util_total += mem_utilization[i];

      mem_util_avg = (mem_util_total/mem_window_size) * 100.0;

      threshold_check(mem_monitor_name, mem_util_avg, mem_threshold, mem_threshold_num);
    }

    timer++;
    sleep(mem_monitor_interval);
  }
  return NULL;
}

// Thread to monitor the ECC counter
static void *
ecc_mon_handler() {
  uint32_t mcr_fd = 0;
  uint32_t ecc_status = 0;
  uint32_t unrecover_ecc_err_addr = 0;
  uint32_t recover_ecc_err_addr = 0;
  uint16_t ecc_recoverable_error_counter = 0;
  uint8_t ecc_unrecoverable_error_counter = 0;
  void *mcr_base_addr;
  void *mcr50_addr;
  void *mcr58_addr;
  void *mcr5c_addr;
  int retry_err = 0;

  while (1) {
    mcr_fd = open("/dev/mem", O_RDWR | O_SYNC );
    if (mcr_fd < 0) {
      // In case of error opening the file, sleep for 2 sec and retry.
      // During continuous failures, log the error every 20 minutes.
      sleep(2);
      if (++retry_err >= 600) {
        syslog(LOG_ERR, "%s - cannot open /dev/mem", __func__);
        retry_err = 0;
      }
      continue;
    }

    retry_err = 0;

    mcr_base_addr = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, mcr_fd,
        AST_MCR_BASE);
    mcr50_addr = (char*)mcr_base_addr + INTR_CTRL_STS_OFFSET;
    ecc_status = *(volatile uint32_t*) mcr50_addr;
    if (ecc_addr_log) {
      mcr58_addr = (char*)mcr_base_addr + ADDR_FIRST_UNRECOVER_ECC_OFFSET;
      unrecover_ecc_err_addr = *(volatile uint32_t*) mcr58_addr;
      mcr5c_addr = (char*)mcr_base_addr + ADDR_LAST_RECOVER_ECC_OFFSET;
      recover_ecc_err_addr = *(volatile uint32_t*) mcr5c_addr;
    }
    munmap(mcr_base_addr, PAGE_SIZE);
    close(mcr_fd);

    ecc_recoverable_error_counter = (ecc_status >> 16) & 0xFF;
    ecc_unrecoverable_error_counter = (ecc_status >> 12) & 0xF;

    // Check ECC recoverable error counter
    ecc_threshold_check(recoverable_ecc_name, ecc_recoverable_error_counter,
                        recov_ecc_threshold, recov_ecc_threshold_num, recover_ecc_err_addr);

    // Check ECC un-recoverable error counter
    ecc_threshold_check(unrecoverable_ecc_name, ecc_unrecoverable_error_counter,
                        unrec_ecc_threshold, unrec_ecc_threshold_num, unrecover_ecc_err_addr);

    sleep(ecc_monitor_interval);
  }
  return NULL;
}

static void *
bmc_health_monitor()
{
  int bmc_health_last_state = 1;
  int bmc_health_kv_state = 1;
  char tmp_health[MAX_VALUE_LEN];
  int relog_counter = 0;
  int relog_counter_criteria = regen_interval / bmc_health_monitor_interval;
  size_t i;
  int ret = 0;

  while(1) {
    // get current health status from kv_store
    memset(tmp_health, 0, MAX_VALUE_LEN);
    ret = pal_get_key_value(BMC_HEALTH_FILE, tmp_health);
    if (ret){
      syslog(LOG_ERR, " %s - kv get bmc_health status failed", __func__);
    }
    bmc_health_kv_state = atoi(tmp_health);

    // If log-util clear all fru, cleaning CPU/MEM/ECC error status
    // After doing it, daemon will regenerate asserted log
    // Generage a syslog every regen_interval loop counter
    if ((relog_counter >= relog_counter_criteria) ||
        ((bmc_health_last_state == 0) && (bmc_health_kv_state == 1))) {

      for(i = 0; i < cpu_threshold_num; i++)
        cpu_threshold[i].asserted = false;
      for(i = 0; i < mem_threshold_num; i++)
        mem_threshold[i].asserted = false;
      for(i = 0; i < recov_ecc_threshold_num; i++)
        recov_ecc_threshold[i].asserted = false;
      for(i = 0; i < unrec_ecc_threshold_num; i++)
        unrec_ecc_threshold[i].asserted = false;

      pthread_mutex_lock(&global_error_mutex);
      bmc_health = 0;
      pthread_mutex_unlock(&global_error_mutex);
      relog_counter = 0;
    }
    bmc_health_last_state = bmc_health_kv_state;
    relog_counter++;
    sleep(bmc_health_monitor_interval);
  }
  return NULL;
}

void check_nm_selftest_result(uint8_t fru, int result)
{
  static uint8_t no_response_retry[MAX_NUM_FRUS] = {0};
  static uint8_t abnormal_status_retry[MAX_NUM_FRUS] = {0};
  static uint8_t is_duplicated_unaccess_event[MAX_NUM_FRUS] = {false};
  static uint8_t is_duplicated_abnormal_event[MAX_NUM_FRUS] = {false};
  char fru_name[10]={0};
  int fru_index = fru - 1;//fru id is start from 1.

  enum
  {
    NM_NORMAL_STATUS = 0,
  };

  //the fru data is validated, no need to check the data again.
  pal_get_fru_name(fru, fru_name);

#ifdef DEBUG
  syslog(LOG_WARNING, "fru_name:%s, fruid:%d, result:%d, nm_retry_threshold:%d", fru_name, fru, result, nm_retry_threshold);
#endif

  if ( PAL_ENOTSUP == result )
  {
    if ( no_response_retry[fru_index] >= nm_retry_threshold )
    {
      if ( !is_duplicated_unaccess_event[fru_index] )
      {
        is_duplicated_unaccess_event[fru_index] = true;
        syslog(LOG_CRIT, "ASSERT: ME Status - Controller Unavailable on the %s", fru_name);
      }
    }
    else
    {
      no_response_retry[fru_index]++;
    }
  }
  else
  {
    if ( NM_NORMAL_STATUS != result )
    {
      if ( abnormal_status_retry[fru_index] >=  nm_retry_threshold )
      {
        if ( !is_duplicated_abnormal_event[fru_index] )
        {
          is_duplicated_abnormal_event[fru_index] = true;
          syslog(LOG_CRIT, "ASSERT: ME Status - Controller Access Degraded or Unavailable on the %s", fru_name);
        }
      }
      else
      {
        abnormal_status_retry[fru_index]++;
      }
    }
    else
    {
      if ( is_duplicated_abnormal_event[fru_index] )
      {
        is_duplicated_abnormal_event[fru_index] = false;
        syslog(LOG_CRIT, "DEASSERT: ME Status - Controller Access Degraded or Unavailable on the %s", fru_name);
      }

      if ( is_duplicated_unaccess_event[fru_index] )
      {
        is_duplicated_unaccess_event[fru_index] = false;
        syslog(LOG_CRIT, "DEASSERT: ME Status - Controller Unavailable on the %s", fru_name);
      }

      no_response_retry[fru_index] = 0;
      abnormal_status_retry[fru_index] = 0;
    }
  }
}

static void *
nm_monitor()
{
  int fru;
  int ret;
  int result;
  const uint8_t normal_status[2] = {0x55, 0x00}; // If the selftest result is 55 00, the status of the controller is okay
  uint8_t data[2]={0x0};

  while (1)
  {
    for ( fru = 1; fru <= MAX_NUM_FRUS; fru++)
    {
      if ( pal_is_slot_server(fru) )
      {
        if ( pal_is_fw_update_ongoing(fru) )
        {
          continue;
        }

        ret = pal_get_nm_selftest_result(fru, data);
        if ( PAL_EOK == ret )
        {
          //if nm has the response, check the status
          result = memcmp(data, normal_status, sizeof(normal_status));
        }
        else
        {
          //if nm has no response, suppose it is in the not support state
          result = PAL_ENOTSUP;
        }
        check_nm_selftest_result(fru, result);
      }
    }

    sleep(nm_monitor_interval);
  }

  return NULL;
}

void
fwupdate_ongoing_handle(bool is_fw_updating)
{
  if (is_fw_updating) { // forbid the execution permission
    system("chmod 666 /sbin/shutdown.sysvinit");
    system("chmod 666 /sbin/halt.sysvinit");
  }
  else {
    system("chmod 4755 /sbin/shutdown.sysvinit");
    system("chmod 4755 /sbin/halt.sysvinit");
  }
}

//Block reboot and shutdown commands in BMC during any FW updating
static void *
fw_update_monitor() {

  bool is_fw_updating = false;
  bool prev_flag = false;

  while(1) {
    //is_fw_updating == true, means BMC is Updating a Device FW
    is_fw_updating = pal_is_fw_update_ongoing_system();

    if (is_fw_updating != prev_flag) {
      fwupdate_ongoing_handle(is_fw_updating);
    }
    prev_flag = is_fw_updating;
    sleep(1);
  }
  return NULL;
}

static int log_count(const char *str)
{
  char cmd[512];
  FILE *fp;
  int ret;
  snprintf(cmd, sizeof(cmd), "grep \"%s\" /mnt/data/logfile /mnt/data/logfile.0 2> /dev/null | wc -l", str);
  fp = popen(cmd, "r");
  if (!fp) {
    return 0;
  }
  if (fscanf(fp, "%d", &ret) != 1) {
    ret = 0;
  }
  pclose(fp);
  return ret;
}

/* Called when we have booted into the golden image
 * because of verified boot failure */
static void check_vboot_recovery(uint8_t error_type, uint8_t error_code)
{
  char log[512];
  char curr_err[MAX_VALUE_LEN] = {0};
  int assert_count, deassert_count;

  /* Technically we can get this from the kv store vboot_error. But
   * we cannot trust it since it could be compromised. Hence try to
   * infer this by counting ASSERT and DEASSERT logs from the persistent
   * log file */
  snprintf(log, sizeof(log), " ASSERT: Verified boot failure (%d,%d)", error_type, error_code);
  assert_count = log_count(log);
  snprintf(log, sizeof(log), " DEASSERT: Verified boot failure (%d,%d)", error_type, error_code);
  deassert_count = log_count(log);
  if (assert_count <= deassert_count) {
    /* This is the first time we are seeing this error. Log it */
    syslog(LOG_CRIT, "ASSERT: Verified boot failure (%d,%d)", error_type, error_code);
  }
  /* Set the error so main can deassert with the correct error code */
  snprintf(curr_err, sizeof(curr_err), "(%d,%d)", error_type, error_code);
  kv_set("vboot_error", curr_err);
}

/* Called when we have booted into CS1. Note verified boot
 * could have still failed if we are not in SW enforcement */
static void check_vboot_main(uint8_t error_type, uint8_t error_code)
{
  char last_err[MAX_VALUE_LEN] = {0};

  if (error_type != 0 || error_code != 0) {
    /* The only way we will boot into BMC (CS1) while there
     * is an active vboot error is if we are not enforcing.
     * Act as if we are in recovery */
    check_vboot_recovery(error_type, error_code);
  } else {
    /* We have successfully booted into a verified BMC! */
    if (0 != kv_get("vboot_error", last_err)) {
      /* We do not have info of the previous error. Not much we can do
       * log an info message and carry on */
      syslog(LOG_INFO, "Verified boot successful!");
    } else if (strcmp(last_err, "(0,0)")) {
      /* We just recovered from a previous error! */
      syslog(LOG_CRIT, "DEASSERT: Verified boot failure %s", last_err);
      /* Do not deassert again on reboot */
      kv_set("vboot_error", "(0,0)");
    }
  }
}

static void check_vboot_state(void)
{
  int mem_fd;
  uint8_t *vboot_base;
  uint8_t error_type;
  uint8_t error_code;
  uint8_t recovery_flag;

  mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (mem_fd < 0) {
    syslog(LOG_CRIT, "%s: Error opening /dev/mem\n", __func__);
    return;
  }

  vboot_base = (uint8_t *)mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, VERIFIED_BOOT_STRUCT_BASE);
  if (!vboot_base) {
    syslog(LOG_CRIT, "%s: Error mapping VERIFIED_BOOT_STRUCT_BASE\n", __func__);
    close(mem_fd);
    return;
  }

  error_type = VERIFIED_BOOT_ERROR_TYPE(vboot_base);
  error_code = VERIFIED_BOOT_ERROR_CODE(vboot_base);
  recovery_flag = VERIFIED_BOOT_RECOVERY_FLAG(vboot_base);

  if (recovery_flag) {
    check_vboot_recovery(error_type, error_code);
  } else {
    check_vboot_main(error_type, error_code);
  }
  munmap(vboot_base, PAGE_SIZE);
  close(mem_fd);
}

int
main(int argc, char **argv) {
  pthread_t tid_watchdog;
  pthread_t tid_hb_led;
  pthread_t tid_i2c_mon;
  pthread_t tid_cpu_monitor;
  pthread_t tid_mem_monitor;
  pthread_t tid_fw_update_monitor;
  pthread_t tid_ecc_monitor;
  pthread_t tid_bmc_health_monitor;
  pthread_t tid_nm_monitor;

  if (argc > 1) {
    exit(1);
  }

  initilize_all_kv();

  initialize_configuration();

  if (vboot_state_check) {
    check_vboot_state();
  }

// For current platforms, we are using WDT from either fand or fscd
// TODO: keeping this code until we make healthd as central daemon that
//  monitors all the important daemons for the platforms.
  if (pthread_create(&tid_watchdog, NULL, watchdog_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for watchdog error\n");
    exit(1);
  }

  if (pthread_create(&tid_hb_led, NULL, hb_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for heartbeat error\n");
    exit(1);
  }

  if (cpu_monitor_enabled) {
    if (pthread_create(&tid_cpu_monitor, NULL, CPU_usage_monitor, NULL) < 0) {
      syslog(LOG_WARNING, "pthread_create for monitor CPU usage\n");
      exit(1);
    }
  }

  if (mem_monitor_enabled) {
    if (pthread_create(&tid_mem_monitor, NULL, memory_usage_monitor, NULL) < 0) {
      syslog(LOG_WARNING, "pthread_create for monitor memory usage\n");
      exit(1);
    }
  }

  if (i2c_monitor_enabled) {
    // Add a thread for monitoring all I2C buses crash or not
    if (pthread_create(&tid_i2c_mon, NULL, i2c_mon_handler, NULL) < 0) {
      syslog(LOG_WARNING, "pthread_create for I2C errorr\n");
      exit(1);
    }
  }

  if (ecc_monitor_enabled) {
    if (pthread_create(&tid_ecc_monitor, NULL, ecc_mon_handler, NULL) < 0) {
      syslog(LOG_WARNING, "pthread_create for ECC monitoring errorr\n");
      exit(1);
    }
  }

  if (regen_log_enabled) {
    if (pthread_create(&tid_bmc_health_monitor, NULL, bmc_health_monitor, NULL) < 0) {
      syslog(LOG_WARNING, "pthread_create for BMC Health monitoring errorr\n");
      exit(1);
    }
  }

  if ( nm_monitor_enabled )
  {
    if (pthread_create(&tid_nm_monitor, NULL, nm_monitor, NULL) < 0)
    {
      syslog(LOG_WARNING, "pthread_create for nm monitor error\n");
      exit(1);
    }
  }

  if (pthread_create(&tid_fw_update_monitor, NULL, fw_update_monitor, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for FW Update Monitor error\n");
    exit(1);
  }

  pthread_join(tid_watchdog, NULL);

  pthread_join(tid_hb_led, NULL);

  if (i2c_monitor_enabled) {
    pthread_join(tid_i2c_mon, NULL);
  }
  if (cpu_monitor_enabled) {
    pthread_join(tid_cpu_monitor, NULL);
  }

  if (mem_monitor_enabled) {
    pthread_join(tid_mem_monitor, NULL);
  }

  if (ecc_monitor_enabled) {
    pthread_join(tid_ecc_monitor, NULL);
  }

  if (regen_log_enabled) {
    pthread_join(tid_bmc_health_monitor, NULL);
  }

  if ( nm_monitor_enabled )
  {
    pthread_join(tid_nm_monitor, NULL);
  }

  pthread_join(tid_fw_update_monitor, NULL);

  return 0;
}

