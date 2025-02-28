/**
 * \file    dump1090.c
 * \ingroup Main
 * \brief   Dump1090, a Mode-S messages decoder for RTLSDR devices.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <malloc.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>
#include <assert.h>
#include <sys/stat.h>
#include <io.h>
#include <process.h>

#include "misc.h"
#include "trace.h"
#include "sdrplay.h"
#include "location.h"
#include "aircraft.h"
#include "airports.h"
#include "interactive.h"

global_data Modes;

/**
 * \addtogroup Main      Main decoder
 * \addtogroup Misc      Support functions
 * \addtogroup Mongoose  Web server
 *
 * \mainpage Dump1090
 *
 * # Introduction
 *
 * A simple ADS-B (**Automatic Dependent Surveillance - Broadcast**) receiver, decoder and web-server. <br>
 * It requires a *RTLSDR* USB-stick and a USB-driver installed using the *Automatic Driver Installer*
 * [**Zadig**](https://zadig.akeo.ie/).
 *
 * The code for Osmocom's [**librtlsdr**](https://osmocom.org/projects/rtl-sdr/wiki) is built into this program.
 * Hence no dependency on *RTLSDR.DLL*.
 *
 * This *Mode S* decoder is based on the Dump1090 by *Salvatore Sanfilippo*.
 *
 * ### Basic block-diagram:
 * \image html dump1090-blocks.png
 *
 * ### Example Web-client page:
 * \image html dump1090-web.png
 *
 * ### More here later ...
 *
 * Copyright (C) 2012 by Salvatore Sanfilippo <antirez@gmail.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * ```
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ```
 */
static void        modeS_send_raw_output (const modeS_message *mm);
static void        modeS_send_SBS_output (const modeS_message *mm, const aircraft *a);
static void        modeS_user_message (const modeS_message *mm);

static int         fix_single_bit_errors (uint8_t *msg, int bits);
static int         fix_two_bits_errors (uint8_t *msg, int bits);
static uint32_t    detect_modeS (uint16_t *m, uint32_t mlen);
static bool        decode_hex_message (mg_iobuf *msg, int loop_cnt);
static bool        decode_SBS_message (mg_iobuf *msg, int loop_cnt);
static int         modeS_message_len_by_type (int type);
static uint16_t   *compute_magnitude_vector (const uint8_t *data);
static void        background_tasks (void);
static void        modeS_exit (void);
static void        signal_handler (int sig);

static u_short        handler_port (intptr_t service);
static const char    *handler_descr (intptr_t service);
static mg_connection *handler_conn (intptr_t service);
static void           connection_read (connection *conn, msg_handler handler, bool is_server);
static void           connection_send (intptr_t service, const void *msg, size_t len);
static void           connection_free (connection *this_conn, intptr_t service);
static unsigned       connection_free_all (void);

#if defined(_DEBUG)
static _CrtMemState start_state;

static void crtdbug_exit (void)
{
  _CrtMemState end_state, diff_state;

  _CrtMemCheckpoint (&end_state);
  if (!_CrtMemDifference(&diff_state, &start_state, &end_state))
     LOG_STDERR ("No mem-leaks detected.\n");
  else
  {
    _CrtCheckMemory();
    _CrtSetDbgFlag (0);
    _CrtDumpMemoryLeaks();
  }
}

static void crtdbug_init (void)
{
  _HFILE file  = _CRTDBG_FILE_STDERR;
  int    mode  = _CRTDBG_MODE_FILE;
  int    flags = _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_DELAY_FREE_MEM_DF;

  _CrtSetReportFile (_CRT_ASSERT, file);
  _CrtSetReportMode (_CRT_ASSERT, mode);
  _CrtSetReportFile (_CRT_ERROR, file);
  _CrtSetReportMode (_CRT_ERROR, mode);
  _CrtSetReportFile (_CRT_WARN, file);
  _CrtSetReportMode (_CRT_WARN, mode);
  _CrtSetDbgFlag (flags | _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG));
  _CrtMemCheckpoint (&start_state);
}
#endif  /* _DEBUG */

/**
 * Return a string describing an error-code from RTLSDR
 *
 * This can be from `librtlsdr` itself or from `WinUsb`.
 */
static const char *get_rtlsdr_error (void)
{
  uint32_t err = rtlsdr_last_error();

  if (err == 0)
     return ("No error");
  return trace_strerror (err);
}

/**
 * Set the RTLSDR gain verbosively.
 */
static void verbose_gain_set (rtlsdr_dev_t *dev, int gain)
{
  int r = rtlsdr_set_tuner_gain_mode (dev, 1);

  if (r < 0)
  {
    LOG_STDERR ("WARNING: Failed to enable manual gain.\n");
    return;
  }
  r = rtlsdr_set_tuner_gain (dev, gain);
  if (r)
       LOG_STDERR ("WARNING: Failed to set tuner gain.\n");
  else LOG_STDOUT ("Tuner gain set to %.0f dB.\n", gain/10.0);
}

/**
 * Set the RTLSDR gain verbosively to AUTO.
 */
static void verbose_gain_auto (rtlsdr_dev_t *dev)
{
  int r = rtlsdr_set_tuner_gain_mode (dev, 0);

  if (r)
       LOG_STDERR ("WARNING: Failed to enable automatic gain.\n");
  else LOG_STDOUT ("Tuner gain set to automatic.\n");
}

/**
 * Set the RTLSDR gain verbosively to the nearest available
 * gain value given in `*target_gain`.
 */
static void nearest_gain (rtlsdr_dev_t *dev, uint16_t *target_gain)
{
  int    gain_in;
  int    i, err1, err2, nearest;
  int    r = rtlsdr_set_tuner_gain_mode (dev, 1);
  char   gbuf [200], *p = gbuf;
  size_t left = sizeof(gbuf);

  if (r)
  {
    LOG_STDERR ("WARNING: Failed to enable manual gain.\n");
    return;
  }

  Modes.rtlsdr.gain_count = rtlsdr_get_tuner_gains (dev, NULL);
  if (Modes.rtlsdr.gain_count <= 0)
     return;

  Modes.rtlsdr.gains = malloc (sizeof(int) * Modes.rtlsdr.gain_count);
  Modes.rtlsdr.gain_count = rtlsdr_get_tuner_gains (dev, Modes.rtlsdr.gains);
  nearest = Modes.rtlsdr.gains[0];
  if (!target_gain)
     return;

  gain_in = *target_gain;

  for (i = 0; i < Modes.rtlsdr.gain_count; i++)
  {
    err1 = abs (gain_in - nearest);
    err2 = abs (gain_in - Modes.rtlsdr.gains[i]);

    p += snprintf (p, left, "%.1f, ", Modes.rtlsdr.gains[i] / 10.0);
    left = sizeof(gbuf) - (p - gbuf) - 1;
    if (err2 < err1)
       nearest = Modes.rtlsdr.gains[i];
  }
  p [-2] = '\0';
  LOG_STDOUT ("Supported gains: %s.\n", gbuf);
  *target_gain = (uint16_t) nearest;
}

/**
 * Enable RTLSDR direct sampling mode (not used yet).
 */
static void verbose_direct_sampling (rtlsdr_dev_t *dev, int on)
{
  int r = rtlsdr_set_direct_sampling (dev, on);

  if (r)
  {
    LOG_STDERR ("WARNING: Failed to set direct sampling mode.\n");
    return;
  }
  if (on == 0)
     LOG_STDOUT ("Direct sampling mode disabled.\n");
  else if (on == 1)
     LOG_STDOUT ("Enabled direct sampling mode, input 1/I.\n");
  else if (on == 2)
     LOG_STDOUT ("Enabled direct sampling mode, input 2/Q.\n");
}

/**
 * Set RTLSDR PPM error-correction.
 */
static void verbose_ppm_set (rtlsdr_dev_t *dev, int ppm_error)
{
  double tuner_freq = 0.0;
  int    r;

  r = rtlsdr_set_freq_correction (dev, ppm_error);
  if (r < 0)
     LOG_STDERR ("WARNING: Failed to set PPM correction.\n");
  else
  {
    rtlsdr_get_xtal_freq (dev, NULL, &tuner_freq);
    LOG_STDOUT ("Tuner correction set to %d PPM; %.3lf MHz.\n", ppm_error, tuner_freq / 1E6);
  }
}

/**
 * Set RTLSDR automatic gain control.
 */
static void verbose_agc_set (rtlsdr_dev_t *dev, int agc)
{
  int r = rtlsdr_set_agc_mode (dev, agc);

  if (r < 0)
       LOG_STDERR ("WARNING: Failed to set AGC.\n");
  else LOG_STDOUT ("AGC %s okay.\n", agc ? "enabled" : "disabled");
}

/**
 * Set RTLSDR Bias-T
 */
static void verbose_bias_tee (rtlsdr_dev_t *dev, int bias_t)
{
  int r = rtlsdr_set_bias_tee (dev, bias_t);

  if (bias_t && r)
     LOG_STDERR ("Failed to activate Bias-T.\n");
}

/**
 * Populate a I/Q -> Magnitude lookup table. It is used because
 * hypot() or round() may be expensive and may vary a lot depending on
 * the CRT used.
 *
 * We scale to 0-255 range multiplying by 1.4 in order to ensure that
 * every different I/Q pair will result in a different magnitude value,
 * not losing any resolution.
 */
static uint16_t *c_gen_magnitude_lut (void)
{
  int       I, Q;
  uint16_t *lut = malloc (sizeof(*lut) * 129 * 129);

  if (!lut)
  {
    LOG_STDERR ("Out of memory in 'c_gen_magnitude_lut()'.\n");
    modeS_exit();
  }
  for (I = 0; I < 129; I++)
  {
    for (Q = 0; Q < 129; Q++)
       lut [I*129 + Q] = (uint16_t) round (360 * hypot(I, Q));
  }
  return (lut);
}

#ifdef USE_GEN_LUT
#include "py_gen_magnitude_lut.h"

static bool check_py_gen_magnitude_lut (void)
{
  uint16_t *lut = c_gen_magnitude_lut();
  int       I, Q, equals;

  assert (lut);

  for (I = equals = 0; I < 129; I++)
  {
    for (Q = 0; Q < 129; Q++)
    {
      int idx = I*129+Q;

      if (lut[idx] == py_gen_magnitude_lut[idx])
           equals++;
      else printf ("%8u != %-8u.\n", py_gen_magnitude_lut[idx], lut[idx]);
    }
  }
  free (lut);
  if (equals != DIM(py_gen_magnitude_lut))
  {
    printf ("There were %zu errors in 'py_gen_magnitude_lut[]'.\n", DIM(py_gen_magnitude_lut) - equals);
    return (false);
  }
  puts ("'py_gen_magnitude_lut[]' values all OK.");
  return (true);
}

#else
static bool check_py_gen_magnitude_lut (void)
{
  puts ("No 'py_gen_magnitude_lut[]'. Hence nothing to check.");
  return (true);
}
#endif

/**
 * Step 1: Initialize the program with default values.
 */
static void modeS_init_config (void)
{
  memset (&Modes, '\0', sizeof(Modes));
  GetCurrentDirectoryA (sizeof(Modes.where_am_I), Modes.where_am_I);
  GetModuleFileNameA (NULL, Modes.who_am_I, sizeof(Modes.who_am_I));

  strcpy (Modes.web_page, basename(INDEX_HTML));
  snprintf (Modes.web_root, sizeof(Modes.web_root), "%s\\web_root", dirname(Modes.who_am_I));
  slashify (Modes.web_root);
  snprintf (Modes.aircraft_db, sizeof(Modes.aircraft_db), "%s\\%s", dirname(Modes.who_am_I), AIRCRAFT_DATABASE_CSV);
  slashify (Modes.aircraft_db);
  snprintf (Modes.airport_db, sizeof(Modes.airport_db), "%s\\%s", dirname(Modes.who_am_I), AIRPORT_DATABASE_CSV);
  slashify (Modes.airport_db);

  Modes.gain_auto       = true;
  Modes.sample_rate     = MODES_DEFAULT_RATE;
  Modes.freq            = MODES_DEFAULT_FREQ;
  Modes.interactive_ttl = MODES_INTERACTIVE_TTL;
  Modes.json_interval   = 1000;
  Modes.keep_alive      = 1;
  Modes.tui_interface   = TUI_WINCON;
  Modes.airport_show    = true;

  InitializeCriticalSection (&Modes.data_mutex);
  InitializeCriticalSection (&Modes.print_mutex);
}

/**
 * Step 2:
 *  \li Open and append to the `--logfile` if specified.
 *  \li In `--net` mode (but not `--net-active` mode), check the precence of the Web-page.
 *  \li Set our home position from the env-var `%DUMP1090_HOMEPOS%`.
 *  \li Initialize the `Modes.data_mutex`.
 *  \li Setup a SIGINT/SIGBREAK handler for a clean exit.
 *  \li Allocate and initialize the needed buffers.
 *  \li Open and parse the `Modes.aircraft_db` file (unless `NUL`).
 */
static bool modeS_init (void)
{
  pos_t       pos;
  const char *env;

  if (Modes.logfile)
  {
    char   args [1000] = "";
    char   buf [sizeof(args) + sizeof(mg_file_path) + 10];
    char  *p = args;
    size_t n, left = sizeof(args);
    int    i;

    Modes.log = fopen (Modes.logfile, "a");
    if (!Modes.log)
    {
      LOG_STDERR ("Failed to create/append to \"%s\".\n", Modes.logfile);
      return (false);
    }
    for (i = 1; i < __argc && left > 2; i++)
    {
      n = snprintf (p, left, " %s", __argv[i]);
      p    += n;
      left -= n;
    }
    fputc ('\n', Modes.log);
    snprintf (buf, sizeof(buf), "------- Starting '%s%s' -----------\n", Modes.who_am_I, args);
    modeS_log (buf);
  }

  modeS_set_log();

  if (strcmp(Modes.aircraft_db, "NUL"))
  {
    if (Modes.use_sql_db)
    {
      snprintf (Modes.aircraft_sql, sizeof(Modes.aircraft_sql), "%s.sqlite", Modes.aircraft_db);
      Modes.have_sql_file = (access(Modes.aircraft_sql, 0) == 0);
    }
    if (Modes.aircraft_db_update)
    {
      aircraft_CSV_update (Modes.aircraft_db, Modes.aircraft_db_update);
      aircraft_CSV_load();
      return (false);
    }
  }

  /**
   * \todo
   * Regenerate using `py -3 tools/gen_airport_codes_csv.py > airport-codes.csv`
   */
#if 0
  if (Modes.airport_db_update && strcmp(Modes.airport_db, "NUL"))
  {
    airports_update_CSV (Modes.airport_db);
    airports_init_CSV();
    return (false);
  }
#endif

  env = getenv ("DUMP1090_HOMEPOS");
  if (env)
  {
    if (sscanf(env, "%lf,%lf", &pos.lat, &pos.lon) != 2 || !VALID_POS(pos))
    {
      LOG_STDERR ("Invalid home-pos %s\n", env);
      return (false);
    }
    Modes.home_pos    = pos;
    Modes.home_pos_ok = true;
    spherical_to_cartesian (&Modes.home_pos, &Modes.home_pos_cart);
  }

  /* Use `Windows Location API` to set `Modes.home_pos`.
   * If an error happened, the error was already reported.
   * Otherwise poll the result in 'location_poll()'
   */
  if (Modes.win_location && !location_get_async())
     return (false);

  signal (SIGINT, signal_handler);
  signal (SIGBREAK, signal_handler);
  signal (SIGABRT, signal_handler);

  /* We add a full message minus a final bit to the length, so that we
   * can carry the remaining part of the buffer that we can't process
   * in the message detection loop, back at the start of the next data
   * to process. This way we are able to also detect messages crossing
   * two reads.
   */
  Modes.data_len = MODES_DATA_LEN + 4*(MODES_FULL_LEN-1);
  Modes.data_ready = false;

  /**
   * Allocate the ICAO address cache. We use two uint32_t for every
   * entry because it's a addr / timestamp pair for every entry.
   */
  Modes.ICAO_cache    = calloc (2 * sizeof(uint32_t) * MODES_ICAO_CACHE_LEN, 1);
  Modes.data          = malloc (Modes.data_len);
  Modes.magnitude     = malloc (2 * Modes.data_len);

  if (!Modes.ICAO_cache || !Modes.data || !Modes.magnitude)
  {
    LOG_STDERR ("Out of memory allocating data buffer.\n");
    return (false);
  }

  memset (Modes.data, 127, Modes.data_len);

#if defined(USE_GEN_LUT)
  Modes.magnitude_lut = py_gen_magnitude_lut;
#else
  Modes.magnitude_lut = c_gen_magnitude_lut();
#endif

  if (Modes.tests)
  {
    if (!airports_init())
       return (false);

    if (!check_py_gen_magnitude_lut())
       return (false);
    test_assert();
  }

  if (!aircraft_CSV_load())
     return (false);

  if (Modes.interactive)
     return interactive_init();
  return (true);
}

/**
 * Step 3: Initialize the RTLSDR device.
 *
 * If `Modes.rtlsdr.name` is specified, select the device that matches `product`.
 * Otherwise select on `Modes.rtlsdr.index` where 0 is the first device found.
 *
 * If you have > 1 RTLSDR device with the same product name and serial-number,
 * then the command `rtl_eeprom -d 1 -p RTL2838-Silver` is handy to set them apart.
 * Like:
 *  ```
 *   product: RTL2838-Silver, serial: 00000001
 *   product: RTL2838-Blue,   serial: 00000001
 *  ```
 */
static bool modeS_init_RTLSDR (void)
{
  int    i, rc, device_count;
  double gain;

  device_count = rtlsdr_get_device_count();
  if (device_count <= 0)
  {
    LOG_STDERR ("No supported RTLSDR devices found. Error: %s\n", get_rtlsdr_error());
    return (false);
  }

  LOG_STDOUT ("Found %d device(s):\n", device_count);
  for (i = 0; i < device_count; i++)
  {
    char manufact [256] = "??";
    char product  [256] = "??";
    char serial   [256] = "??";
    bool selected = false;
    int  r = rtlsdr_get_device_usb_strings (i, manufact, product, serial);

    if (r == 0)
    {
      if (Modes.rtlsdr.name && product[0] && !stricmp(Modes.rtlsdr.name, product))
      {
        selected = true;
        Modes.rtlsdr.index = i;
      }
      else
        selected = (i == Modes.rtlsdr.index);

      if (selected)
         Modes.selected_dev = mg_mprintf ("%s (%s)", product, manufact);
    }
    LOG_STDOUT ("%d: %-10s %-20s SN: %s%s\n", i, manufact, product, serial,
                selected ? " (currently selected)" : "");
  }

  if (Modes.rtlsdr.calibrate)
     rtlsdr_cal_imr (1);

  rc = rtlsdr_open (&Modes.rtlsdr.device, Modes.rtlsdr.index);
  if (rc)
  {
    const char *err = get_rtlsdr_error();

    if (Modes.rtlsdr.name)
         LOG_STDERR ("Error opening the RTLSDR device %s: %s.\n", Modes.rtlsdr.name, err);
    else LOG_STDERR ("Error opening the RTLSDR device %d: %s.\n", Modes.rtlsdr.index, err);
    return (false);
  }

  /* Set gain, frequency, sample rate, and reset the device.
   */
  if (Modes.gain_auto)
  {
    nearest_gain (Modes.rtlsdr.device, NULL);
    verbose_gain_auto (Modes.rtlsdr.device);
  }
  else
  {
    nearest_gain (Modes.rtlsdr.device, &Modes.gain);
    verbose_gain_set (Modes.rtlsdr.device, Modes.gain);
  }

  if (Modes.dig_agc)
     verbose_agc_set (Modes.rtlsdr.device, 1);

  if (Modes.rtlsdr.ppm_error)
     verbose_ppm_set (Modes.rtlsdr.device, Modes.rtlsdr.ppm_error);

  if (Modes.bias_tee)
     verbose_bias_tee (Modes.rtlsdr.device, Modes.bias_tee);

  rc = rtlsdr_set_center_freq (Modes.rtlsdr.device, Modes.freq);
  if (rc)
  {
    LOG_STDERR ("Error setting frequency: %d.\n", rc);
    return (false);
  }

  rc = rtlsdr_set_sample_rate (Modes.rtlsdr.device, Modes.sample_rate);
  if (rc)
  {
    LOG_STDERR ("Error setting sample-rate: %d.\n", rc);
    return (false);
  }

  if (Modes.band_width > 0)
  {
    uint32_t applied_bw = 0;

    rc = rtlsdr_set_and_get_tuner_bandwidth (Modes.rtlsdr.device, 0, &applied_bw, 0);
    if (rc == 0)
         LOG_STDOUT ("Bandwidth reported by device: %.3f MHz.\n", applied_bw/1E6);
    else LOG_STDOUT ("Bandwidth reported by device: <unknown>.\n");

    LOG_STDOUT ("Setting Bandwidth to: %.3f MHz.\n", Modes.band_width/1E6);
    rc = rtlsdr_set_tuner_bandwidth(Modes.rtlsdr.device, Modes.band_width);
    if (rc != 0)
    {
      LOG_STDERR ("Error setting bandwidth: %d.\n", rc);
      return (false);
    }
  }

  LOG_STDOUT ("Tuned to %.03f MHz.\n", Modes.freq / 1E6);

  gain = rtlsdr_get_tuner_gain (Modes.rtlsdr.device);
  if ((unsigned int)gain == 0)
       LOG_STDOUT ("Gain reported by device: AUTO.\n");
  else LOG_STDOUT ("Gain reported by device: %.2f dB.\n", gain/10.0);

  rtlsdr_reset_buffer (Modes.rtlsdr.device);

  return (true);
}

/**
 * This reading callback gets data from the RTLSDR or
 * SDRplay API asynchronously. We then populate the data buffer.
 *
 * A Mutex is used to avoid race-condition with the decoding thread.
 */
static void rx_callback (uint8_t *buf, uint32_t len, void *ctx)
{
  volatile bool exit = *(volatile bool*) ctx;

  if (exit)
     return;

  EnterCriticalSection (&Modes.data_mutex);
  if (len > MODES_DATA_LEN)
     len = MODES_DATA_LEN;

  /* Move the last part of the previous buffer, that was not processed,
   * to the start of the new buffer.
   */
  memcpy (Modes.data, Modes.data + MODES_DATA_LEN, 4*(MODES_FULL_LEN-1));

  /* Read the new data.
   */
  memcpy (Modes.data + 4*(MODES_FULL_LEN-1), buf, len);
  Modes.data_ready = true;
  LeaveCriticalSection (&Modes.data_mutex);
}

/**
 * This is used when `--infile` is specified in order to read data from file
 * instead of using a RTLSDR / SDRplay device.
 */
static int read_from_data_file (void)
{
  uint32_t rc = 0;

  if (Modes.loops > 0 && Modes.fd == STDIN_FILENO)
  {
    LOG_STDERR ("Option `--loop <N>' not supported for `stdin'.\n");
    Modes.loops = 0;
  }

  do
  {
     int      nread, toread;
     uint8_t *data;

     if (Modes.interactive)
     {
       /* When --infile and --interactive are used together, slow down
        * mimicking the real RTLSDR / SDRplay rate.
        */
       Sleep (1000);
     }

     /* Move the last part of the previous buffer, that was not processed,
      * on the start of the new buffer.
      */
     memcpy (Modes.data, Modes.data + MODES_DATA_LEN, 4*(MODES_FULL_LEN-1));
     toread = MODES_DATA_LEN;
     data   = Modes.data + 4*(MODES_FULL_LEN-1);

     while (toread)
     {
       nread = _read (Modes.fd, data, toread);
       if (nread <= 0)
          break;
       data   += nread;
       toread -= nread;
     }

     if (toread)
     {
       /* Not enough data on file to fill the buffer? Pad with
        * no signal.
        */
       memset (data, 127, toread);
     }

     compute_magnitude_vector (Modes.data);
     rc += detect_modeS (Modes.magnitude, Modes.data_len/2);
     background_tasks();

     if (Modes.exit || Modes.fd == STDIN_FILENO)
        break;

     /* seek the file again from the start
      * and re-play it if --loop was given.
      */
     if (Modes.loops > 0)
        Modes.loops--;
     if (Modes.loops == 0 || _lseek(Modes.fd, 0, SEEK_SET) == -1)
        break;
  }
  while (1);
  return (rc);
}

/**
 * We read RTLSDR or SDRplay data using a separate thread, so the main thread
 * only handles decoding without caring about data acquisition.
 * Ref. `main_data_loop()` below.
 */
static unsigned int __stdcall data_thread_fn (void *arg)
{
  int rc;

  if (Modes.sdrplay.device)
  {
    rc = sdrplay_read_async (Modes.sdrplay.device, rx_callback, (void*)&Modes.exit,
                             MODES_ASYNC_BUF_NUMBER, MODES_DATA_LEN);

    DEBUG (DEBUG_GENERAL, "sdrplay_read_async(): rc: %d / %s.\n",
           rc, sdrplay_strerror(rc));

    signal_handler (0);   /* break out of main_data_loop() */
  }
  else if (Modes.rtlsdr.device)
  {
    rc = rtlsdr_read_async (Modes.rtlsdr.device, rx_callback, (void*)&Modes.exit,
                            MODES_ASYNC_BUF_NUMBER, MODES_DATA_LEN);

    DEBUG (DEBUG_GENERAL, "rtlsdr_read_async(): rc: %d/%s.\n",
           rc, get_rtlsdr_error());

    signal_handler (0);    /* break out of main_data_loop() */
  }
  MODES_NOTUSED (arg);
  return (0);
}

/**
 * Main data processing loop.
 *
 * This runs in the main thread of the program.
 */
static void main_data_loop (void)
{
  while (!Modes.exit)
  {
    background_tasks();

    if (!Modes.data_ready)
       continue;

    compute_magnitude_vector (Modes.data);

    /* Signal to the other thread that we processed the available data
     * and we want more.
     */
    Modes.data_ready = false;

    /* Process data after releasing the lock, so that the capturing
     * thread can read data while we perform computationally expensive
     * stuff at the same time. (This should only be useful with very
     * slow processors).
     */
    EnterCriticalSection (&Modes.data_mutex);

#if 0     /**\todo */
    if (Modes.sdrplay_device && Modes.sdrplay.over_sample)
    {
      struct mag_buf *buf = &Modes.mag_buffers [Modes.first_filled_buffer];

      demodulate_8000 (buf);
    }
    else
#endif
      detect_modeS (Modes.magnitude, Modes.data_len/2);

    LeaveCriticalSection (&Modes.data_mutex);

    if (/* rc > 0 && */ Modes.max_messages > 0)
    {
      if (--Modes.max_messages == 0)
      {
        LOG_STDOUT ("'Modes.max_messages' reached 0.\n");
        Modes.exit = true;
       }
    }
  }
}

/**
 * Helper function for `dump_magnitude_vector()`.
 * It prints a single bar used to display raw signals.
 *
 * Since every magnitude sample is between 0 - 255, the function uses
 * up to 63 characters for every bar. Every character represents
 * a length of 4, 3, 2, 1, specifically:
 *
 * \li "O" is 4
 * \li "o" is 3
 * \li "-" is 2
 * \li "." is 1
 */
static void dump_magnitude_bar (uint16_t magnitude, int index)
{
  const char *set = " .-o";
  char        buf [256];
  uint16_t    div = (magnitude / 256) / 4;
  uint16_t    rem = (magnitude / 256) % 4;
  int         markchar = ']';

  memset (buf, 'O', div);
  buf [div] = set[rem];
  buf [div+1] = '\0';

  if (index >= 0)
  {
    /* preamble peaks are marked with ">"
     */
    if (index == 0 || index == 2 || index == 7 || index == 9)
       markchar = '>';

    /* Data peaks are marked to distinguish pairs of bits.
     */
    if (index >= 16)
       markchar = ((index - 16)/2 & 1) ? '|' : ')';
    printf ("[%3d%c |%-66s %u\n", index, markchar, buf, magnitude);
  }
  else
    printf ("[%3d] |%-66s %u\n", index, buf, magnitude);
}

/**
 * Display an *ASCII-art* alike graphical representation of the undecoded
 * message as a magnitude signal.
 *
 * The message starts at the specified offset in the `m` buffer.
 * The function will display enough data to cover a short 56 bit
 * (`MODES_SHORT_MSG_BITS`) message.
 *
 * If possible a few samples before the start of the messsage are included
 * for context.
 */
static void dump_magnitude_vector (const uint16_t *m, uint32_t offset)
{
  uint32_t padding = 5;  /* Show a few samples before the actual start. */
  uint32_t start = (offset < padding) ? 0 : offset - padding;
  uint32_t end = offset + (2*MODES_PREAMBLE_US) + (2*MODES_SHORT_MSG_BITS) - 1;
  uint32_t i;

  for (i = start; i <= end; i++)
      dump_magnitude_bar (m[i], i - offset);
}

/**
 * Produce a raw representation of the message as a Javascript file
 * loadable by `debug.html`.
 */
static void dump_raw_message_JS (const char *descr, uint8_t *msg, const uint16_t *m, uint32_t offset, int fixable)
{
  int   padding = 5;     /* Show a few samples before the actual start. */
  int   start = offset - padding;
  int   end = offset + (MODES_PREAMBLE_US*2)+(MODES_LONG_MSG_BITS*2) - 1;
  int   j, fix1 = -1, fix2 = -1;
  FILE *fp;

  if (fixable != -1)
  {
    fix1 = fixable & 0xFF;
    if (fixable > 255)
       fix2 = fixable >> 8;
  }
  fp = fopen ("frames.js", "a");
  if (!fp)
  {
    LOG_STDERR ("Error opening frames.js: %s\n", strerror(errno));
    Modes.exit = 1;
    return;
  }

  fprintf (fp, "frames.push({\"descr\": \"%s\", \"mag\": [", descr);
  for (j = start; j <= end; j++)
  {
    fprintf (fp, "%d", j < 0 ? 0 : m[j]);
    if (j != end)
       fprintf (fp, ",");
  }
  fprintf (fp, "], \"fix1\": %d, \"fix2\": %d, \"bits\": %d, \"hex\": \"",
           fix1, fix2, modeS_message_len_by_type(msg[0] >> 3));

  for (j = 0; j < MODES_LONG_MSG_BYTES; j++)
      fprintf (fp, "\\x%02x", msg[j]);
  fprintf (fp, "\"});\n");
  fclose (fp);
}

/**
 * This is a wrapper for `dump_magnitude_vector()` that also show the message
 * in hex format with an additional description.
 *
 * \param in  descr  the additional message to show to describe the dump.
 * \param out msg    the decoded message
 * \param in  m      the original magnitude vector
 * \param in  offset the offset where the message starts
 *
 * The function also produces the Javascript file used by `debug.html` to
 * display packets in a graphical format if the Javascript output was
 * enabled.
 */
static void dump_raw_message (const char *descr, uint8_t *msg, const uint16_t *m, uint32_t offset)
{
  int j;
  int msg_type = msg[0] >> 3;
  int fixable = -1;

  if (msg_type == 11 || msg_type == 17)
  {
    int msg_bits = (msg_type == 11) ? MODES_SHORT_MSG_BITS :
                                      MODES_LONG_MSG_BITS;
    fixable = fix_single_bit_errors (msg, msg_bits);
    if (fixable == -1)
       fixable = fix_two_bits_errors (msg, msg_bits);
  }

  if (Modes.debug & DEBUG_JS)
  {
    dump_raw_message_JS (descr, msg, m, offset, fixable);
    return;
  }

  EnterCriticalSection (&Modes.print_mutex);

  printf ("\n--- %s:\n    ", descr);
  for (j = 0; j < MODES_LONG_MSG_BYTES; j++)
  {
    printf ("%02X", msg[j]);
    if (j == MODES_SHORT_MSG_BYTES-1)
       printf (" ... ");
  }
  printf (" (DF %d, Fixable: %d)\n", msg_type, fixable);
  dump_magnitude_vector (m, offset);
  puts ("---\n");

  LeaveCriticalSection (&Modes.print_mutex);
}

/**
 * Parity table for MODE S Messages.
 *
 * The table contains 112 (`MODES_LONG_MSG_BITS`) elements, every element
 * corresponds to a bit set in the message, starting from the first bit of
 * actual data after the preamble.
 *
 * For messages of 112 bit, the whole table is used.
 * For messages of 56 bits only the last 56 elements are used.
 *
 * The algorithm is as simple as XOR-ing all the elements in this table
 * for which the corresponding bit on the message is set to 1.
 *
 * The last 24 elements in this table are set to 0 as the checksum at the
 * end of the message should not affect the computation.
 *
 * \note
 * This function can be used with DF11 and DF17. Other modes have
 * the CRC *XOR-ed* with the sender address as they are replies to interrogations,
 * but a casual listener can't split the address from the checksum.
 */
static const uint32_t modeS_checksum_table [MODES_LONG_MSG_BITS] = {
             0x3935EA, 0x1C9AF5, 0xF1B77E, 0x78DBBF, 0xC397DB, 0x9E31E9, 0xB0E2F0, 0x587178,
             0x2C38BC, 0x161C5E, 0x0B0E2F, 0xFA7D13, 0x82C48D, 0xBE9842, 0x5F4C21, 0xD05C14,
             0x682E0A, 0x341705, 0xE5F186, 0x72F8C3, 0xC68665, 0x9CB936, 0x4E5C9B, 0xD8D449,
             0x939020, 0x49C810, 0x24E408, 0x127204, 0x093902, 0x049C81, 0xFDB444, 0x7EDA22,
             0x3F6D11, 0xE04C8C, 0x702646, 0x381323, 0xE3F395, 0x8E03CE, 0x4701E7, 0xDC7AF7,
             0x91C77F, 0xB719BB, 0xA476D9, 0xADC168, 0x56E0B4, 0x2B705A, 0x15B82D, 0xF52612,
             0x7A9309, 0xC2B380, 0x6159C0, 0x30ACE0, 0x185670, 0x0C2B38, 0x06159C, 0x030ACE,
             0x018567, 0xFF38B7, 0x80665F, 0xBFC92B, 0xA01E91, 0xAFF54C, 0x57FAA6, 0x2BFD53,
             0xEA04AD, 0x8AF852, 0x457C29, 0xDD4410, 0x6EA208, 0x375104, 0x1BA882, 0x0DD441,
             0xF91024, 0x7C8812, 0x3E4409, 0xE0D800, 0x706C00, 0x383600, 0x1C1B00, 0x0E0D80,
             0x0706C0, 0x038360, 0x01C1B0, 0x00E0D8, 0x00706C, 0x003836, 0x001C1B, 0xFFF409,
             0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
             0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
             0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000
           };

static uint32_t modeS_checksum (const uint8_t *msg, int bits)
{
  uint32_t crc = 0;
  int      offset = 0;
  int      j;

  if (bits != MODES_LONG_MSG_BITS)
     offset = MODES_LONG_MSG_BITS - MODES_SHORT_MSG_BITS;

  for (j = 0; j < bits; j++)
  {
    int byte = j / 8;
    int bit  = j % 8;
    int bitmask = 1 << (7 - bit);

    /* If bit is set, XOR with corresponding table entry.
     */
    if (msg[byte] & bitmask)
       crc ^= modeS_checksum_table [j + offset];
  }
  return (crc); /* 24 bit checksum. */
}

/**
 * Given the Downlink Format (DF) of the message, return the message length
 * in bits.
 */
static int modeS_message_len_by_type (int type)
{
  if (type == 16 || type == 17 || type == 19 || type == 20 || type == 21)
     return (MODES_LONG_MSG_BITS);
  return (MODES_SHORT_MSG_BITS);
}

/**
 * Try to fix single bit errors using the checksum. On success modifies
 * the original buffer with the fixed version, and returns the position
 * of the error bit. Otherwise if fixing failed, -1 is returned.
 */
static int fix_single_bit_errors (uint8_t *msg, int bits)
{
  int     i;
  uint8_t aux [MODES_LONG_MSG_BITS/8];

  for (i = 0; i < bits; i++)
  {
    int      byte = i / 8;
    int      bitmask = 1 << (7-(i % 8));
    uint32_t crc1, crc2;

    memcpy (aux, msg, bits/8);
    aux[byte] ^= bitmask;   /* Flip j-th bit. */

    crc1 = ((uint32_t)aux[(bits/8)-3] << 16) |
           ((uint32_t)aux[(bits/8)-2] << 8) |
            (uint32_t)aux[(bits/8)-1];
    crc2 = modeS_checksum (aux, bits);

    if (crc1 == crc2)
    {
      /* The error is fixed. Overwrite the original buffer with
       * the corrected sequence, and returns the error bit
       * position.
       */
      memcpy (msg, aux, bits/8);
      return (i);
    }
  }
  return (-1);
}

/**
 * Similar to `fix_single_bit_errors()` but try every possible two bit combination.
 *
 * This is very slow and should be tried only against DF17 messages that
 * don't pass the checksum, and only in Aggressive Mode.
 */
static int fix_two_bits_errors (uint8_t *msg, int bits)
{
  int     j, i;
  uint8_t aux [MODES_LONG_MSG_BITS/8];

  for (j = 0; j < bits; j++)
  {
    int byte1 = j / 8;
    int bitmask1 = 1 << (7-(j % 8));

    /* Don't check the same pairs multiple times, so i starts from j+1 */
    for (i = j+1; i < bits; i++)
    {
      int      byte2 = i / 8;
      int      bitmask2 = 1 << (7-(i % 8));
      uint32_t crc1, crc2;

      memcpy (aux, msg, bits/8);

      aux[byte1] ^= bitmask1; /* Flip j-th bit. */
      aux[byte2] ^= bitmask2; /* Flip i-th bit. */

      crc1 = ((uint32_t) aux [(bits/8)-3] << 16) |
             ((uint32_t) aux [(bits/8)-2] << 8) |
              (uint32_t) aux [(bits/8)-1];
      crc2 = modeS_checksum (aux, bits);

      if (crc1 == crc2)
      {
        /* The error is fixed. Overwrite the original buffer with
         * the corrected sequence, and returns the error bit
         * position.
         */
        memcpy (msg, aux, bits/8);

        /* We return the two bits as a 16 bit integer by shifting
         * 'i' on the left. This is possible since 'i' will always
         * be non-zero because i starts from j+1.
         */
        return (j | (i << 8));
      }
    }
  }
  return (-1);
}

/**
 * Hash the ICAO address to index our cache of MODES_ICAO_CACHE_LEN
 * elements, that is assumed to be a power of two.
 */
static uint32_t ICAO_cache_hash_address (uint32_t a)
{
  /* The following three rounds will make sure that every bit affects
   * every output bit with ~ 50% of probability.
   */
  a = ((a >> 16) ^ a) * 0x45D9F3B;
  a = ((a >> 16) ^ a) * 0x45D9F3B;
  a = ((a >> 16) ^ a);
  return (a & (MODES_ICAO_CACHE_LEN-1));
}

/**
 * Add the specified entry to the cache of recently seen ICAO addresses.
 *
 * Note that we also add a timestamp so that we can make sure that the
 * entry is only valid for `MODES_ICAO_CACHE_TTL` seconds.
 */
static void ICAO_cache_add_address (uint32_t addr)
{
  uint32_t h = ICAO_cache_hash_address (addr);

  Modes.ICAO_cache [h*2] = addr;
  Modes.ICAO_cache [h*2+1] = (uint32_t) time (NULL);
}

/**
 * Returns 1 if the specified ICAO address was seen in a DF format with
 * proper checksum (not XORed with address) no more than
 * `MODES_ICAO_CACHE_TTL` seconds ago. Otherwise returns 0.
 */
static int ICAO_address_recently_seen (uint32_t addr)
{
  uint32_t h_idx = ICAO_cache_hash_address (addr);
  uint32_t _addr = Modes.ICAO_cache [2*h_idx];
  uint32_t seen  = Modes.ICAO_cache [2*h_idx + 1];

  return (_addr && _addr == addr && (time(NULL) - seen) <= MODES_ICAO_CACHE_TTL);
}

/**
 * If the message type has the checksum XORed with the ICAO address, try to
 * brute force it using a list of recently seen ICAO addresses.
 *
 * Do this in a brute-force fashion by XORing the predicted CRC with
 * the address XOR checksum field in the message. This will recover the
 * address: if we found it in our cache, we can assume the message is okay.
 *
 * This function expects `mm->msg_type` and `mm->msg_bits` to be correctly
 * populated by the caller.
 *
 * On success the correct ICAO address is stored in the `modeS_message`
 * structure in the `AA [0..2]` fields.
 *
 * \retval 1 successfully recovered a message with a correct checksum.
 * \retval 0 failed to recover a message with a correct checksum.
 */
static int brute_force_AP (uint8_t *msg, modeS_message *mm)
{
  uint8_t aux [MODES_LONG_MSG_BYTES];
  int     msg_type = mm->msg_type;
  int     msg_bits = mm->msg_bits;

  if (msg_type == 0 ||         /* Short air surveillance */
      msg_type == 4 ||         /* Surveillance, altitude reply */
      msg_type == 5 ||         /* Surveillance, identity reply */
      msg_type == 16 ||        /* Long Air-Air Surveillance */
      msg_type == 20 ||        /* Comm-A, altitude request */
      msg_type == 21 ||        /* Comm-A, identity request */
      msg_type == 24)          /* Comm-C ELM */
  {
    uint32_t addr;
    uint32_t crc;
    int      last_byte = (msg_bits / 8) - 1;

    /* Work on a copy. */
    memcpy (aux, msg, msg_bits/8);

    /* Compute the CRC of the message and XOR it with the AP field
     * so that we recover the address, because:
     *
     * (ADDR xor CRC) xor CRC = ADDR.
     */
    crc = modeS_checksum (aux, msg_bits);
    aux [last_byte]   ^= crc & 0xFF;
    aux [last_byte-1] ^= (crc >> 8) & 0xFF;
    aux [last_byte-2] ^= (crc >> 16) & 0xFF;

    /* If the obtained address exists in our cache we consider
     * the message valid.
     */
    addr = aircraft_get_addr (aux[last_byte-2], aux[last_byte-1], aux[last_byte]);
    if (ICAO_address_recently_seen(addr))
    {
      mm->AA [0] = aux [last_byte-2];
      mm->AA [1] = aux [last_byte-1];
      mm->AA [2] = aux [last_byte];
      return (1);
    }
  }
  return (0);
}

/**
 * Decode the 13 bit AC altitude field (in DF 20 and others).
 *
 * \param in  msg   the raw message to work with.
 * \param out unit  set to either `MODES_UNIT_METERS` or `MODES_UNIT_FEETS`.
 * \retval the altitude.
 */
static int decode_AC13_field (const uint8_t *msg, metric_unit_t *unit)
{
  int m_bit = msg[3] & (1 << 6);
  int q_bit = msg[3] & (1 << 4);
  int ret;

  if (!m_bit)
  {
    *unit = MODES_UNIT_FEET;
    if (q_bit)
    {
      /* N is the 11 bit integer resulting from the removal of bit Q and M
       */
      int n = ((msg[2] & 31) << 6)   |
              ((msg[3] & 0x80) >> 2) |
              ((msg[3] & 0x20) >> 1) |
               (msg[3] & 15);

      /**
       * The final altitude is due to the resulting number multiplied by 25, minus 1000.
       */
      ret = 25 * n - 1000;
      if (ret < 0)
         ret = 0;
      return (ret);
    }
    else
    {
      /** \todo Implement altitude where Q=0 and M=0 */
    }
  }
  else
  {
    *unit = MODES_UNIT_METERS;

    /** \todo Implement altitude when meter unit is selected.
     */
  }
  return (0);
}

/**
 * Decode the 12 bit AC altitude field (in DF 17 and others).
 * Returns the altitude or 0 if it can't be decoded.
 */
static int decode_AC12_field (uint8_t *msg, metric_unit_t *unit)
{
  int ret, n, q_bit = msg[5] & 1;

  if (q_bit)
  {
    /* N is the 11 bit integer resulting from the removal of bit Q
     */
    *unit = MODES_UNIT_FEET;
    n = ((msg[5] >> 1) << 4) | ((msg[6] & 0xF0) >> 4);

    /* The final altitude is due to the resulting number multiplied
     * by 25, minus 1000.
     */
    ret = 25 * n - 1000;
    if (ret < 0)
       ret = 0;
    return (ret);
  }
  return (0);
}

/**
 * Capability table.
 */
static const char *capability_str[8] = {
    /* 0 */ "Level 1 (Surveillance Only)",
    /* 1 */ "Level 2 (DF0,4,5,11)",
    /* 2 */ "Level 3 (DF0,4,5,11,20,21)",
    /* 3 */ "Level 4 (DF0,4,5,11,20,21,24)",
    /* 4 */ "Level 2+3+4 (DF0,4,5,11,20,21,24,code7 - is on ground)",
    /* 5 */ "Level 2+3+4 (DF0,4,5,11,20,21,24,code7 - is airborne)",
    /* 6 */ "Level 2+3+4 (DF0,4,5,11,20,21,24,code7)",
    /* 7 */ "Level 7 ???"
};

/**
 * Flight status table.
 */
static const char *flight_status_str[8] = {
    /* 0 */ "Normal, Airborne",
    /* 1 */ "Normal, On the ground",
    /* 2 */ "ALERT,  Airborne",
    /* 3 */ "ALERT,  On the ground",
    /* 4 */ "ALERT & Special Position Identification. Airborne or Ground",
    /* 5 */ "Special Position Identification. Airborne or Ground",
    /* 6 */ "Value 6 is not assigned",
    /* 7 */ "Value 7 is not assigned"
};

/**
 * Emergency state table from: <br>
 *   https://www.ll.mit.edu/mission/aviation/publications/publication-files/atc-reports/Grappel_2007_ATC-334_WW-15318.pdf
 *
 * and 1090-DO-260B_FRAC
 */
static const char *emerg_state_str[8] = {
    /* 0 */ "No emergency",
    /* 1 */ "General emergency (Squawk 7700)",
    /* 2 */ "Lifeguard/Medical",
    /* 3 */ "Minimum fuel",
    /* 4 */ "No communications (Squawk 7600)",
    /* 5 */ "Unlawful interference (Squawk 7500)",
    /* 6 */ "Reserved",
    /* 7 */ "Reserved"
};

static const char *get_ME_description (const modeS_message *mm)
{
  static char buf [100];

  if (mm->ME_type >= 1 && mm->ME_type <= 4)
     return ("Aircraft Identification and Category");

  if (mm->ME_type >= 5 && mm->ME_type <= 8)
     return ("Surface Position");

  if (mm->ME_type >= 9 && mm->ME_type <= 18)
     return ("Airborne Position (Baro Altitude)");

  if (mm->ME_type == 19 && mm->ME_subtype >=1 && mm->ME_subtype <= 4)
     return ("Airborne Velocity");

  if (mm->ME_type >= 20 && mm->ME_type <= 22)
     return ("Airborne Position (GNSS Height)");

  if (mm->ME_type == 23 && mm->ME_subtype == 0)
     return ("Test Message");

   if (mm->ME_type == 23 && mm->ME_subtype == 7)
     return ("Test Message -- Squawk");

  if (mm->ME_type == 24 && mm->ME_subtype == 1)
     return ("Surface System Status");

  if (mm->ME_type == 28 && mm->ME_subtype == 1)
     return ("Extended Squitter Aircraft Status (Emergency)");

  if (mm->ME_type == 28 && mm->ME_subtype == 2)
     return ("Extended Squitter Aircraft Status (1090ES TCAS RA)");

  if (mm->ME_type == 29 && (mm->ME_subtype == 0 || mm->ME_subtype == 1))
     return ("Target State and Status Message");

  if (mm->ME_type == 31 && (mm->ME_subtype == 0 || mm->ME_subtype == 1))
     return ("Aircraft Operational Status Message");

  snprintf (buf, sizeof(buf), "Unknown: %d/%d", mm->ME_type, mm->ME_subtype);
  return (buf);
}

/**
 * Decode a raw Mode S message demodulated as a stream of bytes by `detect_modeS()`.
 *
 * And split it into fields populating a `modeS_message` structure.
 */
static int decode_modeS_message (modeS_message *mm, const uint8_t *_msg)
{
  uint32_t    crc2;   /* Computed CRC, used to verify the message CRC. */
  const char *AIS_charset = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";
  uint8_t    *msg;

  memset (mm, '\0', sizeof(*mm));

  /* Work on our local copy
   */
  memcpy (mm->msg, _msg, sizeof(mm->msg));
  msg = mm->msg;

  /* Get the message type ASAP as other operations depend on this
   */
  mm->msg_type = msg[0] >> 3;    /* Downlink Format */
  mm->msg_bits = modeS_message_len_by_type (mm->msg_type);

  /* CRC is always the last three bytes.
   */
  mm->CRC = ((uint32_t)msg[(mm->msg_bits/8)-3] << 16) |
            ((uint32_t)msg[(mm->msg_bits/8)-2] << 8) |
             (uint32_t)msg[(mm->msg_bits/8)-1];
  crc2 = modeS_checksum (msg, mm->msg_bits);

  /* Check CRC and fix single bit errors using the CRC when
   * possible (DF 11 and 17).
   */
  mm->error_bit = -1;    /* No error */
  mm->CRC_ok = (mm->CRC == crc2);

  if (!mm->CRC_ok && (mm->msg_type == 11 || mm->msg_type == 17))
  {
    mm->error_bit = fix_single_bit_errors (msg, mm->msg_bits);
    if (mm->error_bit != -1)
    {
      mm->CRC = modeS_checksum (msg, mm->msg_bits);
      mm->CRC_ok = true;
    }
    else if (Modes.aggressive && mm->msg_type == 17 && (mm->error_bit = fix_two_bits_errors(msg, mm->msg_bits)) != -1)
    {
      mm->CRC = modeS_checksum (msg, mm->msg_bits);
      mm->CRC_ok = true;
    }
  }

  /* Note: most of the other computation happens **after** we fix the single bit errors.
   * Otherwise we would need to recompute the fields again.
   */
  mm->ca = msg[0] & 7;        /* Responder capabilities. */

  /* ICAO address
   */
  mm->AA [0] = msg [1];
  mm->AA [1] = msg [2];
  mm->AA [2] = msg [3];

  /* DF17 type (assuming this is a DF17, otherwise not used)
   */
  mm->ME_type = msg[4] >> 3;         /* Extended squitter message type. */
  mm->ME_subtype = msg[4] & 7;       /* Extended squitter message subtype. */

  /* Fields for DF4,5,20,21
   */
  mm->flight_status = msg[0] & 7;         /* Flight status for DF4,5,20,21 */
  mm->DR_status = msg[1] >> 3 & 31;       /* Request extraction of downlink request. */
  mm->UM_status = ((msg[1] & 7) << 3) |   /* Request extraction of downlink request. */
                  (msg[2] >> 5);

  /*
   * In the squawk (identity) field bits are interleaved like this:
   * (message bit 20 to bit 32):
   *
   * C1-A1-C2-A2-C4-A4-ZERO-B1-D1-B2-D2-B4-D4
   *
   * So every group of three bits A, B, C, D represent an integer
   * from 0 to 7.
   *
   * The actual meaning is just 4 octal numbers, but we convert it
   * into a base ten number that happens to represent the four octal numbers.
   *
   * For more info: http://en.wikipedia.org/wiki/Gillham_code
   */
  {
    int a, b, c, d;

    a = ((msg[3] & 0x80) >> 5) |
        ((msg[2] & 0x02) >> 0) |
        ((msg[2] & 0x08) >> 3);
    b = ((msg[3] & 0x02) << 1) |
        ((msg[3] & 0x08) >> 2) |
        ((msg[3] & 0x20) >> 5);
    c = ((msg[2] & 0x01) << 2) |
        ((msg[2] & 0x04) >> 1) |
        ((msg[2] & 0x10) >> 4);
    d = ((msg[3] & 0x01) << 2) |
        ((msg[3] & 0x04) >> 1) |
        ((msg[3] & 0x10) >> 4);
    mm->identity = a*1000 + b*100 + c*10 + d;
  }

  /* DF 11 & 17: try to populate our ICAO addresses whitelist.
   * DFs with an AP field (XORed addr and CRC), try to decode it.
   */
  if (mm->msg_type != 11 && mm->msg_type != 17)
  {
    /* Check if we can check the checksum for the Downlink Formats where
     * the checksum is XORed with the aircraft ICAO address. We try to
     * brute force it using a list of recently seen aircraft addresses.
     */
    if (brute_force_AP(msg, mm))
    {
      /* We recovered the message, mark the checksum as valid.
       */
      mm->CRC_ok = true;
    }
    else
      mm->CRC_ok = false;
  }
  else
  {
    /* If this is DF 11 or DF 17 and the checksum was ok, we can add this address to the list
     * of recently seen addresses.
     */
    if (mm->CRC_ok && mm->error_bit == -1)
       ICAO_cache_add_address (aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]));
  }

  /* Decode 13 bit altitude for DF0, DF4, DF16, DF20
   */
  if (mm->msg_type == 0 || mm->msg_type == 4 || mm->msg_type == 16 || mm->msg_type == 20)
     mm->altitude = decode_AC13_field (msg, &mm->unit);

  /** Decode extended squitter specific stuff.
   */
  if (mm->msg_type == 17)
  {
    /* Decode the extended squitter message.
     */
    if (mm->ME_type >= 1 && mm->ME_type <= 4)
    {
      /* Aircraft Identification and Category
       */
      mm->aircraft_type = mm->ME_type - 1;
      mm->flight[0] = AIS_charset [msg[5] >> 2];
      mm->flight[1] = AIS_charset [((msg[5] & 3) << 4) | (msg[6] >> 4)];
      mm->flight[2] = AIS_charset [((msg[6] & 15) <<2 ) | (msg[7] >> 6)];
      mm->flight[3] = AIS_charset [msg[7] & 63];
      mm->flight[4] = AIS_charset [msg[8] >> 2];
      mm->flight[5] = AIS_charset [((msg[8] & 3) << 4) | (msg[9] >> 4)];
      mm->flight[6] = AIS_charset [((msg[9] & 15) << 2) | (msg[10] >> 6)];
      mm->flight[7] = AIS_charset [msg[10] & 63];
      mm->flight[8] = '\0';
    }
    else if (mm->ME_type >= 9 && mm->ME_type <= 18)
    {
      /* Airborne position Message
       */
      mm->odd_flag = msg[6] & (1 << 2);
      mm->UTC_flag = msg[6] & (1 << 3);
      mm->altitude = decode_AC12_field (msg, &mm->unit);
      mm->raw_latitude  = ((msg[6] & 3) << 15) | (msg[7] << 7) | (msg[8] >> 1); /* Bits 23 - 39 */
      mm->raw_longitude = ((msg[8] & 1) << 16) | (msg[9] << 8) | msg[10];       /* Bits 40 - 56 */
    }
    else if (mm->ME_type == 19 && mm->ME_subtype >= 1 && mm->ME_subtype <= 4)
    {
      /* Airborne Velocity Message
       */
      if (mm->ME_subtype == 1 || mm->ME_subtype == 2)
      {
        mm->EW_dir           = (msg[5] & 4) >> 2;
        mm->EW_velocity      = ((msg[5] & 3) << 8) | msg[6];
        mm->NS_dir           = (msg[7] & 0x80) >> 7;
        mm->NS_velocity      = ((msg[7] & 0x7F) << 3) | ((msg[8] & 0xE0) >> 5);
        mm->vert_rate_source = (msg[8] & 0x10) >> 4;
        mm->vert_rate_sign   = (msg[8] & 0x08) >> 3;
        mm->vert_rate        = ((msg[8] & 7) << 6) | ((msg[9] & 0xFC) >> 2);

        /* Compute velocity and angle from the two speed components.
         */
        mm->velocity = (int) hypot (mm->NS_velocity, mm->EW_velocity);   /* hypot(x,y) == sqrt(x*x+y*y) */

        if (mm->velocity)
        {
          int    ewV = mm->EW_velocity;
          int    nsV = mm->NS_velocity;
          double heading;

          if (mm->EW_dir)
             ewV *= -1;
          if (mm->NS_dir)
             nsV *= -1;
          heading = atan2 (ewV, nsV);

          /* Convert to degrees.
           */
          mm->heading = (int) (heading * 360 / TWO_PI);
          mm->heading_is_valid = true;

          /* We don't want negative values but a [0 .. 360> scale.
           */
          if (mm->heading < 0)
             mm->heading += 360;
        }
        else
          mm->heading = 0;
      }
      else if (mm->ME_subtype == 3 || mm->ME_subtype == 4)
      {
        mm->heading_is_valid = msg[5] & (1 << 2);
        mm->heading = (int) (360.0/128) * (((msg[5] & 3) << 5) | (msg[6] >> 3));
      }
    }
  }
  mm->phase_corrected = false;  /* Set to 'true' by the caller if needed. */
  return (mm->CRC_ok);
}

/**
 * Accumulate statistics of unrecognized ME types and sub-types.
 */
static void add_unrecognized_ME (int type, int subtype)
{
  if (type >= 0 && type < MAX_ME_TYPE && subtype >= 0 && subtype < MAX_ME_SUBTYPE)
  {
    unrecognized_ME *me = &Modes.stat.unrecognized_ME [type];

    me->sub_type [subtype]++;
  }
}

/**
 * Sum the number of unrecognized ME sub-types for a type.
 */
static uint64_t sum_unrecognized_ME (int type)
{
  unrecognized_ME *me = &Modes.stat.unrecognized_ME [type];
  uint64_t         sum = 0;
  int              i;

  for (i = 0; i < MAX_ME_SUBTYPE; i++)
      sum += me->sub_type [i];
  return (sum);
}

/**
 * Print statistics of unrecognized ME types and sub-types.
 */
static void print_unrecognized_ME (void)
{
  int      t;
  uint64_t totals = 0;
  int      num_totals = 0;

  for (t = 0; t < MAX_ME_TYPE; t++)
      totals += sum_unrecognized_ME (t);

  if (totals == 0ULL)
  {
    LOG_STDOUT (" %8llu unrecognized ME types.\n", 0ULL);
    return;
  }

  LOG_STDOUT (" %8llu unrecognized ME types:", totals);

  for (t = 0; t < MAX_ME_TYPE; t++)
  {
    const unrecognized_ME *me = &Modes.stat.unrecognized_ME [t];
    char   sub_types [200];
    char  *p = sub_types;
    size_t j, left = sizeof(sub_types);
    int    n;

    totals = sum_unrecognized_ME (t);
    if (totals == 0ULL)
       continue;

    *p = '\0';
    for (j = 0; j < MAX_ME_SUBTYPE; j++)
        if (me->sub_type[j] > 0ULL)
        {
          n = snprintf (p, left, "%zd,", j);
          left -= n;
          p    += n;
        }

    if (p > sub_types) /* remove the comma */
         p[-1] = '\0';
    else *p = '\0';

    /* indent next line to print like:
     *   45 unrecognized ME types: 29: 20 (2)
     *                             31: 25 (3)
     */
    if (++num_totals > 1)
       LOG_STDOUT ("! \n                                ");

    if (sub_types[0])
         LOG_STDOUT ("! %3llu: %2d (%s)", totals, t, sub_types);
    else LOG_STDOUT ("! %3llu: %2d", totals, t);
  }
  LOG_STDOUT ("! \n");
}

/**
 * This function gets a decoded Mode S Message and prints it on the screen
 * in a human readable format.
 */
static void display_modeS_message (const modeS_message *mm)
{
  char   buf [200];
  char  *p = buf;
  size_t left = sizeof(buf);
  int    i;

  /* Handle only addresses mode first.
   */
  if (Modes.only_addr)
  {
    puts (aircraft_get_details(&mm->AA[0]));
    return;
  }

  /* Show the raw message.
   */
  *p++ = '*';
  left--;
  for (i = 0; i < mm->msg_bits/8 && left > 5; i++)
  {
    snprintf (p, left, "%02x", mm->msg[i]);
    p    += 2;
    left -= 2;
  }
  *p++ = ';';
  *p++ = '\n';
  *p = '\0';
  LOG_STDOUT ("%s", buf);

  if (Modes.raw)
     return;         /* Enough for --raw mode */

  LOG_STDOUT ("CRC: %06X (%s)\n", (int)mm->CRC, mm->CRC_ok ? "ok" : "wrong");
  if (mm->error_bit != -1)
     LOG_STDOUT ("Single bit error fixed, bit %d\n", mm->error_bit);

  if (mm->sig_level > 0)
     LOG_STDOUT ("RSSI: %.1lf dBFS\n", 10 * log10(mm->sig_level));

  if (mm->msg_type == 0)
  {
    /* DF 0 */
    LOG_STDOUT ("DF 0: Short Air-Air Surveillance.\n");
    LOG_STDOUT ("  Altitude       : %d %s\n", mm->altitude, UNIT_NAME(mm->unit));
    LOG_STDOUT ("  ICAO Address   : %s\n", aircraft_get_details(&mm->AA[0]));
  }
  else if (mm->msg_type == 4 || mm->msg_type == 20)
  {
    LOG_STDOUT ("DF %d: %s, Altitude Reply.\n", mm->msg_type, mm->msg_type == 4 ? "Surveillance" : "Comm-B");
    LOG_STDOUT ("  Flight Status  : %s\n", flight_status_str [mm->flight_status]);
    LOG_STDOUT ("  DR             : %d\n", mm->DR_status);
    LOG_STDOUT ("  UM             : %d\n", mm->UM_status);
    LOG_STDOUT ("  Altitude       : %d %s\n", mm->altitude, UNIT_NAME(mm->unit));
    LOG_STDOUT ("  ICAO Address   : %s\n", aircraft_get_details(&mm->AA[0]));

    if (mm->msg_type == 20)
    {
      /** \todo 56 bits DF20 MB additional field. */
    }
  }
  else if (mm->msg_type == 5 || mm->msg_type == 21)
  {
    LOG_STDOUT ("DF %d: %s, Identity Reply.\n", mm->msg_type, mm->msg_type == 5 ? "Surveillance" : "Comm-B");
    LOG_STDOUT ("  Flight Status  : %s\n", flight_status_str [mm->flight_status]);
    LOG_STDOUT ("  DR             : %d\n", mm->DR_status);
    LOG_STDOUT ("  UM             : %d\n", mm->UM_status);
    LOG_STDOUT ("  Squawk         : %d\n", mm->identity);
    LOG_STDOUT ("  ICAO Address   : %s\n", aircraft_get_details(&mm->AA[0]));

    if (mm->msg_type == 21)
    {
      /** \todo 56 bits DF21 MB additional field. */
    }
  }
  else if (mm->msg_type == 11)
  {
    /* DF 11 */
    LOG_STDOUT ("DF 11: All Call Reply.\n");
    LOG_STDOUT ("  Capability  : %s\n", capability_str[mm->ca]);
    LOG_STDOUT ("  ICAO Address: %s\n", aircraft_get_details(&mm->AA[0]));
  }
  else if (mm->msg_type == 17)
  {
    /* DF 17 */
    LOG_STDOUT ("DF 17: ADS-B message.\n");
    LOG_STDOUT ("  Capability     : %d (%s)\n", mm->ca, capability_str[mm->ca]);
    LOG_STDOUT ("  ICAO Address   : %s\n", aircraft_get_details(&mm->AA[0]));
    LOG_STDOUT ("  Extended Squitter Type: %d\n", mm->ME_type);
    LOG_STDOUT ("  Extended Squitter Sub : %d\n", mm->ME_subtype);
    LOG_STDOUT ("  Extended Squitter Name: %s\n", get_ME_description(mm));

    /* Decode the extended squitter message. */
    if (mm->ME_type >= 1 && mm->ME_type <= 4)
    {
      /* Aircraft identification. */
      const char *ac_type_str[4] = {
                 "Aircraft Type D",
                 "Aircraft Type C",
                 "Aircraft Type B",
                 "Aircraft Type A"
             };
      LOG_STDOUT ("    Aircraft Type  : %s\n", ac_type_str[mm->aircraft_type]);
      LOG_STDOUT ("    Identification : %s\n", mm->flight);
    }
    else if (mm->ME_type >= 9 && mm->ME_type <= 18)
    {
      LOG_STDOUT ("    F flag   : %s\n", mm->odd_flag ? "odd" : "even");
      LOG_STDOUT ("    T flag   : %s\n", mm->UTC_flag ? "UTC" : "non-UTC");
      LOG_STDOUT ("    Altitude : %d feet\n", mm->altitude);
      LOG_STDOUT ("    Latitude : %d (not decoded)\n", mm->raw_latitude);
      LOG_STDOUT ("    Longitude: %d (not decoded)\n", mm->raw_longitude);
    }
    else if (mm->ME_type == 19 && mm->ME_subtype >= 1 && mm->ME_subtype <= 4)
    {
      if (mm->ME_subtype == 1 || mm->ME_subtype == 2)
      {
        /* Velocity */
        LOG_STDOUT ("    EW direction      : %d\n", mm->EW_dir);
        LOG_STDOUT ("    EW velocity       : %d\n", mm->EW_velocity);
        LOG_STDOUT ("    NS direction      : %d\n", mm->NS_dir);
        LOG_STDOUT ("    NS velocity       : %d\n", mm->NS_velocity);
        LOG_STDOUT ("    Vertical rate src : %d\n", mm->vert_rate_source);
        LOG_STDOUT ("    Vertical rate sign: %d\n", mm->vert_rate_sign);
        LOG_STDOUT ("    Vertical rate     : %d\n", mm->vert_rate);
      }
      else if (mm->ME_subtype == 3 || mm->ME_subtype == 4)
      {
        LOG_STDOUT ("    Heading status: %d\n", mm->heading_is_valid);
        LOG_STDOUT ("    Heading: %d\n", mm->heading);
      }
    }
    else if (mm->ME_type == 23)  /* Test Message */
    {
      if (mm->ME_subtype == 7)
           LOG_STDOUT ("    Squawk: %04x\n", mm->identity);
      else LOG_STDOUT ("    Unrecognized ME subtype: %d\n", mm->ME_subtype);
    }
    else if (mm->ME_type == 28)  /* Extended Squitter Aircraft Status */
    {
      if (mm->ME_subtype == 1)
      {
        LOG_STDOUT ("    Emergency State: %s\n", emerg_state_str[(mm->msg[5] & 0xE0) >> 5]);
        LOG_STDOUT ("    Squawk: %04x\n", mm->identity);
      }
      else
        LOG_STDOUT ("    Unrecognized ME subtype: %d\n", mm->ME_subtype);
    }
#if 1
    else if (mm->ME_type == 29)
    {
      /**\todo
       * Target State + Status Message
       */
      add_unrecognized_ME (29, mm->ME_subtype);
    }
    else if (mm->ME_type == 31)  /* Aircraft operation status */
    {
      /**\todo Ref: chapter 8 in `The-1090MHz-riddle.pdf`
       */
      add_unrecognized_ME (31, mm->ME_subtype);
    }
#endif
    else
    {
      LOG_STDOUT ("    Unrecognized ME type: %d, subtype: %d\n", mm->ME_type, mm->ME_subtype);
      add_unrecognized_ME (mm->ME_type, mm->ME_subtype);
    }
  }
  else
  {
    LOG_STDOUT ("DF %d with good CRC received (decoding still not implemented).\n", mm->msg_type);
  }
}

/**
 * Turn I/Q samples pointed by `Modes.data` into the magnitude vector
 * pointed by `Modes.magnitude`.
 */
static uint16_t *compute_magnitude_vector (const uint8_t *data)
{
  uint16_t *m = Modes.magnitude;
  uint32_t  i;

  /* Compute the magnitude vector. It's just `sqrt(I^2 + Q^2)`, but
   * we rescale to the 0-255 range to exploit the full resolution.
   */
  for (i = 0; i < Modes.data_len; i += 2)
  {
    int I = data [i] - 127;
    int Q = data [i+1] - 127;

    if (I < 0)
        I = -I;
    if (Q < 0)
        Q = -Q;
    m [i / 2] = Modes.magnitude_lut [129*I + Q];
  }
  return (m);
}

/**
 * Return -1 if the message is out of phase left-side
 * Return  1 if the message is out of phase right-size
 * Return  0 if the message is not particularly out of phase.
 *
 * Note: this function will access m[-1], so the caller should make sure to
 * call it only if we are not at the start of the current buffer.
 */
static int detect_out_of_phase (const uint16_t *m)
{
  if (m[3] > m[2]/3)
     return (1);
  if (m[10] > m[9]/3)
     return (1);
  if (m[6] > m[7]/3)
     return (-1);
  if (m[-1] > m[1]/3)
     return (-1);
  return (0);
}

/**
 * This function does not really correct the phase of the message, it just
 * applies a transformation to the first sample representing a given bit:
 *
 * If the previous bit was one, we amplify it a bit.
 * If the previous bit was zero, we decrease it a bit.
 *
 * This simple transformation makes the message a bit more likely to be
 * correctly decoded for out of phase messages:
 *
 * When messages are out of phase there is more uncertainty in
 * sequences of the same bit multiple times, since `11111` will be
 * transmitted as continuously altering magnitude (high, low, high, low...)
 *
 * However because the message is out of phase some part of the high
 * is mixed in the low part, so that it is hard to distinguish if it is
 * a zero or a one.
 *
 * However when the message is out of phase passing from `0` to `1` or from
 * `1` to `0` happens in a very recognizable way, for instance in the `0 -> 1`
 * transition, magnitude goes low, high, high, low, and one of of the
 * two middle samples the high will be *very* high as part of the previous
 * or next high signal will be mixed there.
 *
 * Applying our simple transformation we make more likely if the current
 * bit is a zero, to detect another zero. Symmetrically if it is a one
 * it will be more likely to detect a one because of the transformation.
 * In this way similar levels will be interpreted more likely in the
 * correct way.
 */
static void apply_phase_correction (uint16_t *m)
{
  int j;

  m += 16; /* Skip preamble. */
  for (j = 0; j < 2*(MODES_LONG_MSG_BITS-1); j += 2)
  {
    if (m[j] > m[j+1])
    {
      /* One */
      m[j+2] = (m[j+2] * 5) / 4;
    }
    else
    {
      /* Zero */
      m[j+2] = (m[j+2] * 4) / 5;
    }
  }
}

#if defined(USE_READSB_DEMOD)
/**
 * Use a rewrite of the 'demodulate2400()' function from
 * https://github.com/wiedehopf/readsb.git
 */
static void detect_modeS (uint16_t *m, uint32_t mlen)
{
  struct mag_buf mag;

  memset (&mag, '\0', sizeof(mag));
  mag.data   = m;
  mag.length = mlen;
  mag.sysTimestamp = MSEC_TIME();
  demodulate2400 (&mag);
}

#else
/**
 * Detect a Mode S messages inside the magnitude buffer pointed by `m`
 * and of size `mlen` bytes. Every detected Mode S message is converted
 * into a stream of bits and passed to the function to display it.
 */
static uint32_t detect_modeS (uint16_t *m, uint32_t mlen)
{
  uint8_t  bits [MODES_LONG_MSG_BITS];
  uint8_t  msg [MODES_LONG_MSG_BITS/2];
  uint16_t aux [MODES_LONG_MSG_BITS*2];
  uint32_t j;
  bool     use_correction = false;
  uint32_t rc = 0;  /**\todo fix this */

  /* The Mode S preamble is made of impulses of 0.5 microseconds at
   * the following time offsets:
   *
   * 0   - 0.5 usec: first impulse.
   * 1.0 - 1.5 usec: second impulse.
   * 3.5 - 4   usec: third impulse.
   * 4.5 - 5   usec: last impulse.
   *
   * If we are sampling at 2 MHz, every sample in our magnitude vector
   * is 0.5 usec. So the preamble will look like this, assuming there is
   * an impulse at offset 0 in the array:
   *
   * 0   -----------------
   * 1   -
   * 2   ------------------
   * 3   --
   * 4   -
   * 5   --
   * 6   -
   * 7   ------------------
   * 8   --
   * 9   -------------------
   */
  for (j = 0; j < mlen - 2*MODES_FULL_LEN; j++)
  {
    int  low, high, delta, i, errors;
    bool good_message = false;

    if (Modes.exit)
       break;

    if (use_correction)
       goto good_preamble;    /* We already checked it. */

    /* First check of relations between the first 10 samples
     * representing a valid preamble. We don't even investigate further
     * if this simple test is not passed.
     */
    if (!(m[j]   > m[j+1] &&
          m[j+1] < m[j+2] &&
          m[j+2] > m[j+3] &&
          m[j+3] < m[j]   &&
          m[j+4] < m[j]   &&
          m[j+5] < m[j]   &&
          m[j+6] < m[j]   &&
          m[j+7] > m[j+8] &&
          m[j+8] < m[j+9] &&
          m[j+9] > m[j+6]))
    {
      if ((Modes.debug & DEBUG_NOPREAMBLE) && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Unexpected ratio among first 10 samples", msg, m, j);
      continue;
    }

    /* The samples between the two spikes must be < than the average
     * of the high spikes level. We don't test bits too near to
     * the high levels as signals can be out of phase so part of the
     * energy can be in the near samples.
     */
    high = (m[j] + m[j+2] + m[j+7] + m[j+9]) / 6;
    if (m[j+4] >= high || m[j+5] >= high)
    {
      if ((Modes.debug & DEBUG_NOPREAMBLE) && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Too high level in samples between 3 and 6", msg, m, j);
      continue;
    }

    /* Similarly samples in the range 11-14 must be low, as it is the
     * space between the preamble and real data. Again we don't test
     * bits too near to high levels, see above.
     */
    if (m[j+11] >= high || m[j+12] >= high || m[j+13] >= high || m[j+14] >= high)
    {
      if ((Modes.debug & DEBUG_NOPREAMBLE) && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Too high level in samples between 10 and 15", msg, m, j);
      continue;
    }

    Modes.stat.valid_preamble++;

good_preamble:

    /* If the previous attempt with this message failed, retry using
     * magnitude correction.
      */
    if (use_correction)
    {
      memcpy (aux, m + j + MODES_PREAMBLE_US*2, sizeof(aux));
      if (j && detect_out_of_phase(m + j))
      {
        apply_phase_correction (m + j);
        Modes.stat.out_of_phase++;
      }
      /** \todo Apply other kind of corrections. */
    }

    /* Decode all the next 112 bits, regardless of the actual message
     * size. We'll check the actual message type later.
     */
    errors = 0;
    for (i = 0; i < 2*MODES_LONG_MSG_BITS; i += 2)
    {
      low   = m [j + i + 2*MODES_PREAMBLE_US];
      high  = m [j + i + 2*MODES_PREAMBLE_US + 1];
      delta = low - high;
      if (delta < 0)
         delta = -delta;

      if (i > 0 && delta < 256)
         bits[i/2] = bits[i/2-1];

      else if (low == high)
      {
        /* Checking if two adjacent samples have the same magnitude
         * is an effective way to detect if it's just random noise
         * that was detected as a valid preamble.
         */
        bits[i/2] = 2;    /* error */
        if (i < 2*MODES_SHORT_MSG_BITS)
           errors++;
      }
      else if (low > high)
      {
        bits[i/2] = 1;
      }
      else
      {
        /* (low < high) for exclusion
         */
        bits[i/2] = 0;
      }
    }

    /* Restore the original message if we used magnitude correction.
     */
    if (use_correction)
       memcpy (m + j + 2*MODES_PREAMBLE_US, aux, sizeof(aux));

    /* Pack bits into bytes
     */
    for (i = 0; i < MODES_LONG_MSG_BITS; i += 8)
    {
      msg [i/8] = bits[i]   << 7 |
                  bits[i+1] << 6 |
                  bits[i+2] << 5 |
                  bits[i+3] << 4 |
                  bits[i+4] << 3 |
                  bits[i+5] << 2 |
                  bits[i+6] << 1 |
                  bits[i+7];
    }

    int msg_type = msg[0] >> 3;
    int msg_len = modeS_message_len_by_type (msg_type) / 8;

    /* Last check, high and low bits are different enough in magnitude
     * to mark this as real message and not just noise?
     */
    delta = 0;
    for (i = 0; i < 8 * 2 * msg_len; i += 2)
    {
      delta += abs (m[j + i + 2*MODES_PREAMBLE_US] -
                    m[j + i + 2*MODES_PREAMBLE_US + 1]);
    }
    delta /= 4 * msg_len;

    /* Filter for an average delta of three is small enough to let almost
     * every kind of message to pass, but high enough to filter some
     * random noise.
     */
    if (delta < 10*255)
    {
      use_correction = false;
      continue;
    }

    /* If we reached this point, and error is zero, we are very likely
     * with a Mode S message in our hands, but it may still be broken
     * and CRC may not be correct. This is handled by the next layer.
     */
    if (errors == 0 || (Modes.aggressive && errors <= 2))
    {
      modeS_message mm;
      double        signal_power = 0ULL;
      int           signal_len = mlen;
      uint32_t      k, mag;

      /* Decode the received message and update statistics
       */
      rc += decode_modeS_message (&mm, msg);

      /* measure signal power
       */
      for (k = j; k < j + MODES_FULL_LEN; k++)
      {
        mag = m [k];
        signal_power += mag * mag;
      }
      mm.sig_level = signal_power / (65536.0 * signal_len);

      /* Update statistics.
       */
      if (mm.CRC_ok || use_correction)
      {
        if (errors == 0)
           Modes.stat.demodulated++;
        if (mm.error_bit == -1)
        {
          if (mm.CRC_ok)
               Modes.stat.good_CRC++;
          else Modes.stat.bad_CRC++;
        }
        else
        {
          Modes.stat.bad_CRC++;
          Modes.stat.fixed++;
          if (mm.error_bit < MODES_LONG_MSG_BITS)
               Modes.stat.single_bit_fix++;
          else Modes.stat.two_bits_fix++;
        }
      }

      /* Output debug mode info if needed.
       */
      if (!use_correction)
      {
        if (Modes.debug & DEBUG_DEMOD)
           dump_raw_message ("Demodulated with 0 errors", msg, m, j);

        else if ((Modes.debug & DEBUG_BADCRC) && mm.msg_type == 17 && (!mm.CRC_ok || mm.error_bit != -1))
           dump_raw_message ("Decoded with bad CRC", msg, m, j);

        else if ((Modes.debug & DEBUG_GOODCRC) && mm.CRC_ok && mm.error_bit == -1)
           dump_raw_message ("Decoded with good CRC", msg, m, j);
      }

      /* Skip this message if we are sure it's fine.
       */
      if (mm.CRC_ok)
      {
        j += 2 * (MODES_PREAMBLE_US + (8 * msg_len));
        good_message = true;
        if (use_correction)
           mm.phase_corrected = true;
      }

      /* Pass data to the next layer
       */
      if (mm.CRC_ok)
         modeS_user_message (&mm);
    }
    else
    {
      if ((Modes.debug & DEBUG_DEMODERR) && use_correction)
      {
        LOG_STDERR ("The following message has %d demod errors", errors);
        dump_raw_message ("Demodulated with errors", msg, m, j);
      }
    }

    /* Retry with phase correction if possible.
     */
    if (!good_message && !use_correction)
    {
      j--;
      use_correction = true;
    }
    else
    {
      use_correction = false;
    }
  }
  return (rc);
}
#endif  /* USE_READSB_DEMOD */

/**
 * When a new message is available, because it was decoded from the
 * RTL/SDRplay device, file, or received in a TCP input port, or any other
 * way we can receive a decoded message, we call this function in order
 * to use the message.
 *
 * Basically this function passes a raw message to the upper layers for
 * further processing and visualization.
 */
static void modeS_user_message (const modeS_message *mm)
{
  uint64_t num_clients;

  Modes.stat.messages_total++;

  /* Track aircrafts in interactive mode or if we have some HTTP / SBS clients.
   */
  num_clients = Modes.stat.cli_accepted [MODES_NET_SERVICE_HTTP] +
                Modes.stat.cli_accepted [MODES_NET_SERVICE_SBS_OUT];

  if (Modes.interactive || num_clients > 0)
  {
    uint64_t  now = MSEC_TIME();
    aircraft *a = interactive_receive_data (mm, now);

    if (a && Modes.stat.cli_accepted[MODES_NET_SERVICE_SBS_OUT] > 0)
       modeS_send_SBS_output (mm, a);     /* Feed SBS output clients. */
  }

  /* In non-interactive mode, display messages on standard output.
   * In silent-mode, do nothing just to consentrate on network traces.
   */
  if (!Modes.interactive && !Modes.silent)
  {
    display_modeS_message (mm);
    if (!Modes.raw && !Modes.only_addr)
    {
      puts ("");
      modeS_log ("\n\n");
    }
  }

  /* Send data to connected clients.
   * In `--net-active` mode we have no clients.
   */
  if (Modes.net)
     modeS_send_raw_output (mm);
}

/**
 * Read raw IQ samples from `stdin` and filter everything that is lower than the
 * specified level for more than 256 samples in order to reduce
 * example file size.
 *
 * Will print to `stdout` in BINARY-mode.
 */
static bool strip_mode (int level)
{
  int I, Q;
  uint64_t c = 0;

  _setmode (_fileno(stdin), O_BINARY);
  _setmode (_fileno(stdout), O_BINARY);

  while ((I = getchar()) != EOF && (Q = getchar()) != EOF)
  {
    if (abs(I-127) < level && abs(Q-127) < level)
    {
      c++;
      if (c > 4*MODES_PREAMBLE_US)
         continue;
    }
    else
      c = 0;

    putchar (I);
    putchar (Q);
  }
  return (true);
}

/**
 * Return a description of the receiver in JSON.
 *  { "version" : "0.3", "refresh" : 1000, "history" : 3 }
 */
static char *receiver_to_json (void)
{
  int history_size = DIM(Modes.json_aircraft_history)-1;

  /* work out number of valid history entries
   */
  if (!Modes.json_aircraft_history [history_size].ptr)
     history_size = Modes.json_aircraft_history_next;

  return mg_mprintf ("{\"version\": \"%s\", "
                      "\"refresh\": %llu, "
                      "\"history\": %d, "
                      "\"lat\": %.6g, "          /* if 'Modes.home_pos_ok == false', this is 0. */
                      "\"lon\": %.6g}",          /* ditto */
                      PROG_VERSION,
                      Modes.json_interval,
                      history_size,
                      Modes.home_pos.lat,
                      Modes.home_pos.lon);
}

/**
 * Returns a `connection *` based on the remote `addr` and `service`.
 * This can be either client or server.
 */
static connection *connection_get_addr (const mg_addr *addr, intptr_t service, bool is_server)
{
  connection *srv;

  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);

  for (srv = Modes.connections[service]; srv; srv = srv->next)
  {
    if (srv->service == service && !memcmp(&srv->addr, addr, sizeof(srv->addr)))
       return (srv);
  }
  is_server ? Modes.stat.srv_unknown [service]++ :   /* Should never happen */
              Modes.stat.cli_unknown [service]++;
  return (NULL);
}

/**
 * Free a specific connection, client or server.
 */
static void connection_free (connection *this_conn, intptr_t service)
{
  connection *conn;
  uint32_t    conn_id = (uint32_t)-1;
  int         is_server = -1;

  if (!this_conn)
     return;

  for (conn = Modes.connections[service]; conn; conn = conn->next)
  {
    if (conn != this_conn)
       continue;

    LIST_DELETE (connection, &Modes.connections[service], conn);
    if (conn->conn->is_accepted)
    {
      Modes.stat.cli_removed [service]++;
      is_server = 0;
    }
    else
    {
      Modes.stat.srv_removed [service]++;
      is_server = 1;
    }
    conn_id = conn->id;
    free (conn);
    break;
  }

  DEBUG (DEBUG_NET2, "Freeing %s %u for service \"%s\".\n",
         is_server == 1 ? "server" :
         is_server == 0 ? "client" : "?",
         conn_id, handler_descr(service));
}

/*
 * Free all connections in all services.
 */
static unsigned connection_free_all (void)
{
  intptr_t service;
  unsigned num = 0;

  for (service = MODES_NET_SERVICE_RAW_OUT; service < MODES_NET_SERVICES_NUM; service++)
  {
    connection *conn, *conn_next;

    for (conn = Modes.connections[service]; conn; conn = conn_next)
    {
      conn_next = conn->next;
      connection_free (conn, service);
      num++;
    }
  }
  return (num);
}

/**
 * Iterate over all the listening connections and send a `msg` to
 * all clients in the specified `service`.
 *
 * There can only be 1 service that matches this. But this
 * service can have many clients.
 *
 * \note
 *  \li This function is not used for sending HTTP data.
 *  \li This function is not called when `--net-active` is used.
 */
static void connection_send (intptr_t service, const void *msg, size_t len)
{
  connection *c;
  int         found = 0;

  for (c = Modes.connections[service]; c; c = c->next)
  {
    if (c->service != service)
       continue;

    mg_send (c->conn, msg, len);   /* if write fails, the client gets freed in connection_handler() */
    found++;
  }
  if (found > 0)
     DEBUG (DEBUG_NET, "Sent %zd bytes to %d clients in service \"%s\".\n",
            len, found, handler_descr(service));
}

/**
 * Handlers for the network services.
 *
 * We use Mongoose for handling all the server and low-level network I/O. <br>
 * We register event-handlers that gets called on important network events.
 *
 * Keep the data for our 4 network services in this structure.
 */
static net_service modeS_net_services [MODES_NET_SERVICES_NUM] = {
                 { &Modes.raw_out,  NULL, "Raw TCP output", MODES_NET_PORT_RAW_OUT },
                 { &Modes.raw_in,   NULL, "Raw TCP input",  MODES_NET_PORT_RAW_IN },
                 { &Modes.sbs_out,  NULL, "SBS TCP output", MODES_NET_PORT_SBS },
                 { &Modes.sbs_in,   NULL, "SBS TCP input",  MODES_NET_PORT_SBS },
                 { &Modes.http_out, NULL, "HTTP server",    MODES_NET_PORT_HTTP }
               };

/**
 * Mongoose event names.
 */
static const char *event_name (int ev)
{
  static char buf [20];

  if (ev >= MG_EV_USER)
  {
    snprintf (buf, sizeof(buf), "MG_EV_USER%d", ev - MG_EV_USER);
    return (buf);
  }

  return (ev == MG_EV_OPEN       ? "MG_EV_OPEN" :     /* Event on 'connect()', 'listen()' and 'accept()'. Ignored */
          ev == MG_EV_POLL       ? "MG_EV_POLL" :
          ev == MG_EV_RESOLVE    ? "MG_EV_RESOLVE" :
          ev == MG_EV_CONNECT    ? "MG_EV_CONNECT" :
          ev == MG_EV_ACCEPT     ? "MG_EV_ACCEPT" :
          ev == MG_EV_READ       ? "MG_EV_READ" :
          ev == MG_EV_WRITE      ? "MG_EV_WRITE" :
          ev == MG_EV_CLOSE      ? "MG_EV_CLOSE" :
          ev == MG_EV_ERROR      ? "MG_EV_ERROR" :
          ev == MG_EV_HTTP_MSG   ? "MG_EV_HTTP_MSG" :
          ev == MG_EV_HTTP_CHUNK ? "MG_EV_HTTP_CHUNK" :
          ev == MG_EV_WS_OPEN    ? "MG_EV_WS_OPEN" :
          ev == MG_EV_WS_MSG     ? "MG_EV_WS_MSG" :
          ev == MG_EV_WS_CTL     ? "MG_EV_WS_CTL" :
          ev == MG_EV_MQTT_CMD   ? "MG_EV_MQTT_CMD" :   /* Can never occur here */
          ev == MG_EV_MQTT_MSG   ? "MG_EV_MQTT_MSG" :   /* Can never occur here */
          ev == MG_EV_MQTT_OPEN  ? "MG_EV_MQTT_OPEN" :  /* Can never occur here */
          ev == MG_EV_SNTP_TIME  ? "MG_EV_SNTP_TIME"    /* Can never occur here */
                                 : "?");
}

static mg_connection *handler_conn (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (*modeS_net_services [service].conn);
}

static uint16_t *handler_num_connections (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (&modeS_net_services [service].num_connections);
}

static const char *handler_descr (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (modeS_net_services [service].descr);
}

static u_short handler_port (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (modeS_net_services [service].port);
}

static char *handler_error (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (modeS_net_services [service].last_err);
}

static char *handler_store_error (intptr_t service, const char *err)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  free (modeS_net_services [service].last_err);
  modeS_net_services [service].last_err = NULL;
  if (err)
  {
    modeS_net_services [service].last_err = strdup (err);
    DEBUG (DEBUG_NET, "%s\n", err);
  }
  return (modeS_net_services [service].last_err);
}

static bool handler_sending (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (modeS_net_services [service].active_send);
}

static void net_flushall (void)
{
  mg_connection *conn;
  unsigned       num_active  = 0;
  unsigned       num_passive = 0;
  unsigned       num_unknown = 0;
  unsigned       total_rx = 0;
  unsigned       total_tx = 0;

  for (conn = Modes.mgr.conns; conn; conn = conn->next)
  {
    total_rx += conn->recv.len;
    total_tx += conn->send.len;

    mg_iobuf_free (&conn->recv);
    mg_iobuf_free (&conn->send);

    if (conn->is_accepted || conn->is_listening)
         num_passive++;
    else if (conn->is_client)
         num_active++;
    else num_unknown++;
  }
  DEBUG (DEBUG_NET,
         "Flushed %u active connections, %u passive, %u unknown. Remaining bytes: %u Rx, %u Tx.\n",
         num_active, num_passive, num_unknown, total_rx, total_tx);
}

static int print_server_errors (void)
{
  int   service, num = 0;
  char *err;

  for (service = MODES_NET_SERVICE_RAW_OUT; service < MODES_NET_SERVICES_NUM; service++)
  {
    err = handler_error (service);
    if (!err)
       continue;

    LOG_STDERR ("%s\n", err);
    handler_store_error (service, NULL);
    num++;
  }
  return (num);
}

/**
 * \todo
 * The event handler for WebSocket control messages.
 */
static void connection_handler_websocket (mg_connection *conn, const char *remote, int ev, void *ev_data)
{
  mg_ws_message *ws = ev_data;

  DEBUG (DEBUG_NET, "WebSocket event %s from client at %s has %zd bytes for us.\n",
         event_name(ev), remote, conn->recv.len);

  if (ev == MG_EV_WS_OPEN)
  {
    DEBUG (DEBUG_MONGOOSE2, "HTTP WebSock open from client %lu:\n", conn->id);
    HEX_DUMP (ws->data.ptr, ws->data.len);
  }
  else if (ev == MG_EV_WS_MSG)
  {
    DEBUG (DEBUG_MONGOOSE2, "HTTP WebSock message from client %lu:\n", conn->id);
    HEX_DUMP (ws->data.ptr, ws->data.len);
  }
  else if (ev == MG_EV_WS_CTL)
  {
    DEBUG (DEBUG_MONGOOSE2, "HTTP WebSock control from client %lu:\n", conn->id);
    HEX_DUMP (ws->data.ptr, ws->data.len);
    Modes.stat.HTTP_websockets++;
  }
}

static const char *set_headers (const connection *cli,
                                const char       *content_type)
{
  static char headers [200];
  char       *p = headers;

  *p = '\0';
  if (content_type)
  {
    strcpy (headers, "Content-Type: ");
    p += strlen ("Content-Type: ");
    strcpy (headers, content_type);
    p += strlen (content_type);
    strcpy (p, "\r\n");
    p += 2;
  }

  if (Modes.keep_alive && cli->keep_alive)
  {
    strcpy (p, "Connection: keep-alive\r\n");
    Modes.stat.HTTP_keep_alive_sent++;
  }
  return (headers);
}

/*
 * Generated arrays from
 *   xxd -i favicon.png
 *   xxd -i favicon.ico
 */
#include "favicon.c"

static void send_favicon (mg_connection *conn,
                          connection    *cli,
                          const uint8_t *data,
                          size_t         data_len,
                          const char    *content_type)
{
  DEBUG (DEBUG_NET, "Sending favicon (%s, %zu bytes) to client %lu.\n", content_type, data_len, conn->id);

  mg_printf (conn,
             "HTTP/1.1 200 OK\r\n"
             "Content-Length: %lu\r\n"
             "%s\r\n", data_len, set_headers(cli, content_type));
  mg_send (conn, data, data_len);
  conn->is_resp = 0;
}

/**
 * The event handler for all HTTP traffic.
 */
static int connection_handler_http (mg_connection *conn,
                                    int            ev,
                                    void          *ev_data,
                                    char          *request_uri,
                                    size_t         request_uri_size)
{
  mg_http_message *hm = ev_data;
  mg_str          *head;
  connection      *cli;
  bool             is_dump1090, is_extended;
  const char      *content_type = NULL;
  const char      *uri, *ext;

  request_uri[0] = '\0';

  if (strncmp(hm->head.ptr, "GET /", 5))
  {
    DEBUG (DEBUG_NET, "Bad Request from client %lu: '%.*s'\n",
           conn->id, (int)hm->head.len, hm->head.ptr);
    Modes.stat.HTTP_400_responses++;
    return (400);
  }

  cli = connection_get_addr (&conn->rem, MODES_NET_SERVICE_HTTP, false);

  /* Make a copy of the request for the caller
   */
  uri = strncpy (request_uri, hm->uri.ptr, request_uri_size-1);
  request_uri [hm->uri.len] = '\0';
  DEBUG (DEBUG_NET, "ev: %s, uri: '%s'\n", event_name(ev), uri);

  Modes.stat.HTTP_get_requests++;

  head = mg_http_get_header (hm, "Connection");
  if (head && !mg_vcasecmp(head, "keep-alive"))
  {
    DEBUG (DEBUG_NET2, "Connection: '%.*s'\n", (int)head->len, head->ptr);
    Modes.stat.HTTP_keep_alive_recv++;
    cli->keep_alive = true;
  }

  head = mg_http_get_header (hm, "Accept-Encoding");
  if (head && !mg_vcasecmp(head, "gzip"))
  {
    DEBUG (DEBUG_NET, "Accept-Encoding: '%.*s'\n", (int)head->len, head->ptr);
    cli->encoding_gzip = true;  /**\todo Add gzip compression */
  }

  /* Redirect a 'GET /' to a 'GET /' + 'web_page'
   */
  if (!strcmp(uri, "/"))
  {
    const char *base_name;

    if (cli->redirect_sent)
       return (0);

    cli->redirect_sent = true;
    base_name = Modes.web_page;
    mg_printf (conn,
               "HTTP/1.1 301 Moved\r\n"
               "Location: %s\r\n"
               "Content-Length: 0\r\n\r\n", base_name);

    DEBUG (DEBUG_NET, "301 redirect to: '%s/%s'\n", Modes.web_root, base_name);
    return (301);
  }

  /**
   * \todo Check header for a "Upgrade: websocket" and call mg_ws_upgrade()?
   */
  if (!stricmp(uri, "/echo"))
  {
    DEBUG (DEBUG_NET, "Got WebSocket echo:\n'%.*s'.\n", (int)hm->head.len, hm->head.ptr);
    mg_ws_upgrade (conn, hm, "WS test");
    return (200);
  }

  if (!stricmp(uri, "/data/receiver.json"))
  {
    char *data = receiver_to_json();

    DEBUG (DEBUG_NET, "Feeding client %lu with receiver-data:\n%.100s\n", conn->id, data);

    mg_http_reply (conn, 200, MODES_CONTENT_TYPE_JSON "\r\n", data);
    free (data);
    return (200);
  }

  /* What we normally expect with the default 'web_root/index.html'
   */
  is_dump1090 = stricmp (uri, "/data.json") == 0;

  /* Or From an OpenLayers3/Tar1090/FlightAware web-client
   */
  is_extended = (stricmp (uri, "/data/aircraft.json") == 0) ||
                (stricmp (uri, "/chunks/chunks.json") == 0);

  if (is_dump1090 || is_extended)
  {
    char *data = aircraft_make_json (is_extended);

    /* "Cross Origin Resource Sharing":
     * https://www.freecodecamp.org/news/access-control-allow-origin-header-explained/
     */
    #define CORS_HEADER "Access-Control-Allow-Origin: *\r\n"

    if (!data)
    {
      conn->is_closing = 1;
      Modes.stat.HTTP_500_responses++;   /* malloc() failed -> "Internal Server Error" */
      return (500);
    }

    /* This is rather inefficient way to pump data over to the client.
     * Better use a WebSocket instead.
     */
    if (is_extended)
         mg_http_reply (conn, 200, CORS_HEADER, data);
    else mg_http_reply (conn, 200, CORS_HEADER MODES_CONTENT_TYPE_JSON "\r\n", data);
    free (data);
    return (200);
  }

  ext = strrchr (uri, '.');
  if (ext)
  {
    int rc = 200;        /* Assume status 200 OK */

    if (!stricmp(uri, "/favicon.png"))
       send_favicon (conn, cli, favicon_png, favicon_png_len, MODES_CONTENT_TYPE_PNG);

    else if (!stricmp(uri, "/favicon.ico"))   /* Some browsers may want a 'favicon.ico' file */
       send_favicon (conn, cli, favicon_ico, favicon_ico_len, MODES_CONTENT_TYPE_ICON);

    else
    {
      mg_http_serve_opts  opts;
      mg_file_path        file;

      memset (&opts, '\0', sizeof(opts));
      opts.page404       = NULL;
      opts.extra_headers = set_headers (cli, content_type);

#if defined(PACKED_WEB_ROOT)
      opts.fs = &mg_fs_packed;
      snprintf (file, sizeof(file), "%s/%s", Modes.web_root, uri+1);
      DEBUG (DEBUG_NET, "Serving packed file: '%s'.\n", file);
#else
      snprintf (file, sizeof(file), "%s/%s", Modes.web_root, uri+1);
      DEBUG (DEBUG_NET, "Serving file: '%s'.\n", file);
#endif

      DEBUG (DEBUG_NET, "extra-headers: '%s'.\n", opts.extra_headers);

      mg_http_serve_file (conn, hm, file, &opts);
      if (access(file, 0) != 0)
      {
        Modes.stat.HTTP_404_responses++;
        rc = 404;
      }
    }
    return (rc);
  }

  mg_http_reply (conn, 404, set_headers(cli, NULL), "Not found\n");
  DEBUG (DEBUG_NET, "Unhandled URI '%.20s' from client %lu.\n", uri, conn->id);
  return (404);
}

/**
 * The timer callback for an active `connect()`.
 */
static void connection_timeout (void *fn_data)
{
  INT_PTR service = (int)(INT_PTR) fn_data;
  char    err [200];
  char    host_port [100];

  snprintf (host_port, sizeof(host_port), modeS_net_services[service].is_ip6 ? "[%s]:%u" : "%s:%u",
            modeS_net_services[service].host, modeS_net_services[service].port);

  snprintf (err, sizeof(err), "Timeout in connection to service \"%s\" on host %s",
            handler_descr(service), host_port);
  handler_store_error (service, err);

  signal_handler (0);  /* break out of main_data_loop()  */
}

/**
 * The event handler for ALL network I/O.
 */
static void connection_handler (mg_connection *this_conn, int ev, void *ev_data, void *fn_data)
{
  connection *conn;
  char       *remote, remote_buf [100];
  uint16_t    port;
  INT_PTR     service = (int)(INT_PTR) fn_data;   /* 'fn_data' is arbitrary user data */

  if (Modes.exit)
     return;

  if (ev == MG_EV_POLL)    /* Ignore this events */
     return;

  if (ev == MG_EV_ERROR)
  {
    char err [200];

    remote = modeS_net_services [service].host;
    port   = modeS_net_services [service].port;

    if (remote && service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM)
    {
      snprintf (err, sizeof(err), "Connection to %s:%u failed: %s", remote, port, (const char*)ev_data);
      handler_store_error (service, err);
      signal_handler (0);   /* break out of main_data_loop()  */
    }
    return;
  }


  if (ev == MG_EV_OPEN)
  {
    remote = modeS_net_services [service].host;
    port   = modeS_net_services [service].port;

    DEBUG (DEBUG_NET, "MG_EV_OPEN for host %s, port %u%s\n",
           remote ? remote : "*", port, this_conn->is_listening ? " (listen socket)" : "");
    return;
  }

  remote = _mg_straddr (&this_conn->rem, remote_buf, sizeof(remote_buf));

  if (ev == MG_EV_RESOLVE)
  {
    DEBUG (DEBUG_NET, "Resolved to host %s\n", remote);
    return;
  }

  if (ev == MG_EV_CONNECT)
  {
    mg_timer_free (&Modes.mgr.timers, &modeS_net_services[service].timer);
    conn = calloc (sizeof(*conn), 1);
    if (!conn)
    {
      this_conn->is_closing = 1;
      return;
    }

    conn->conn    = this_conn;      /* Keep a copy of the active connection */
    conn->service = service;
    conn->id      = this_conn->id;
    conn->addr    = this_conn->rem;

    LIST_ADD_TAIL (connection, &Modes.connections[service], conn);
    ++ (*handler_num_connections (service));  /* should never go above 1 */
    Modes.stat.srv_connected [service]++;

    DEBUG (DEBUG_NET, "Connected to host %s (service \"%s\")\n", remote, handler_descr(service));
    return;
  }

  if (ev == MG_EV_ACCEPT)
  {
    conn = calloc (sizeof(*conn), 1);
    if (!conn)
    {
      this_conn->is_closing = 1;
      return;
    }

    conn->conn    = this_conn;      /* Keep a copy of the passive (listen) connection */
    conn->service = service;
    conn->id      = this_conn->id;
    conn->addr    = this_conn->rem;

    LIST_ADD_TAIL (connection, &Modes.connections[service], conn);
    ++ (*handler_num_connections (service));
    Modes.stat.cli_accepted [service]++;

    DEBUG (DEBUG_NET, "New client %u (service \"%s\") from %s.\n",
           conn->id, handler_descr(service), remote);
    return;
  }

  if (ev == MG_EV_READ)
  {
    Modes.stat.bytes_recv [service] += *(const long*) ev_data;

    DEBUG (DEBUG_NET2, "MG_EV_READ from %s (service \"%s\")\n", remote, handler_descr(service));

    if (service == MODES_NET_SERVICE_RAW_IN)
    {
      conn = connection_get_addr (&this_conn->rem, service, false);
      connection_read (conn, decode_hex_message, false);

      conn = connection_get_addr (&this_conn->rem, service, true);
      connection_read (conn, decode_hex_message, true);
    }
    else if (service == MODES_NET_SERVICE_SBS_IN)
    {
      conn = connection_get_addr (&this_conn->rem, service, true);
      connection_read (conn, decode_SBS_message, true);
    }
    return;
  }

  if (ev == MG_EV_WRITE)         /* Increment our own send() bytes */
  {
    Modes.stat.bytes_sent[service] += *(const long*) ev_data;
    DEBUG (DEBUG_NET2, "writing %d bytes to client %lu (%s)\n", *(const int*)ev_data, this_conn->id, remote);
    return;
  }

  if (ev == MG_EV_CLOSE)
  {
    conn = connection_get_addr (&this_conn->rem, service, false);
    connection_free (conn, service);

    conn = connection_get_addr (&this_conn->rem, service, true);
    connection_free (conn, service);
    -- (*handler_num_connections (service));
    return;
  }

  if (service == MODES_NET_SERVICE_HTTP)
  {
    char request_uri [200];
    int  status;

    if (this_conn->is_websocket && (ev == MG_EV_WS_OPEN || ev == MG_EV_WS_MSG || ev == MG_EV_WS_CTL))
       connection_handler_websocket (this_conn, remote, ev, ev_data);

    else if (ev == MG_EV_HTTP_MSG)
    {
      status = connection_handler_http (this_conn, ev, ev_data,
                                        request_uri, sizeof(request_uri));
      DEBUG (DEBUG_NET, "HTTP %d for '%.30s' (client %lu)\n",
             status, request_uri, this_conn->id);
    }
    else if (ev == MG_EV_HTTP_CHUNK)
    {
      mg_http_message *hm = ev_data;

      DEBUG (DEBUG_MONGOOSE2, "HTTP chunk from client %lu:\n", this_conn->id);
      HEX_DUMP (hm->message.ptr, hm->message.len);
    }
    else
      DEBUG (DEBUG_NET2, "Ignoring HTTP event '%s' (client %lu)\n",
             event_name(ev), this_conn->id);
  }
}

/**
 * Setup a connection for a service.
 * Active or passive (`listen == true`).
 */
static mg_connection *connection_setup (intptr_t service, bool listen, bool sending)
{
  mg_connection *conn = NULL;
  char           url [50];

  /* Temporary enable important errors to go to `stderr` only.
   * For both an active and listen (passive) coonections we handle
   * "early" errors (like out of memory) by returning NULL. A failed
   * active connection will fail later. See comment below.
   */
  mg_log_set_fn (modeS_logc, stderr);
  mg_log_set (MG_LL_ERROR);

  if (listen)
  {
    mg_listen_func passive = (service == MODES_NET_SERVICE_HTTP) ? mg_http_listen : mg_listen;

    snprintf (url, sizeof(url), "tcp://0.0.0.0:%u", modeS_net_services[service].port);
    conn = (*passive) (&Modes.mgr, url, connection_handler, (void*)service);
    modeS_net_services [service].active_send = sending;
  }
  else
  {
    /* For an active connect(), we'll get one of these event in connection_handler():
     *  - MG_EV_ERROR    -- the `--host-xx` argument was not resolved or the connection failed or timed out.
     *  - MG_EV_RESOLVE  -- the `--host-xx` argument was successfully resolved to an IP-address.
     *  - MG_EV_CONNECT  -- successfully connected.
     */
    const char *fmt = (modeS_net_services[service].is_ip6 ? "tcp://[%s]:%u" : "tcp://%s:%u");

    snprintf (url, sizeof(url), fmt, modeS_net_services[service].host, modeS_net_services[service].port);
    mg_timer_add (&Modes.mgr, MODES_CONNECT_TIMEOUT, 0, connection_timeout, (void*)service);
    modeS_net_services [service].active_send = sending;

    DEBUG (DEBUG_NET, "Connecting to %s for service \"%s\".\n", url, handler_descr(service));
    conn = mg_connect (&Modes.mgr, url, connection_handler, (void*) service);
  }

  modeS_set_log();

  if (conn && (Modes.debug & DEBUG_MONGOOSE2))
     conn->is_hexdumping = 1;
  return (conn);
}

/**
 * Setup an active connection for a service.
 */
static bool connection_setup_active (intptr_t service, mg_connection **conn)
{
  *conn = connection_setup (service, false, false);
  if (*conn == NULL)
  {
    LOG_STDERR ("Fail to set-up active socket for %s.\n", handler_descr(service));
    return (false);
  }
  return (true);
}

/**
 * Setup a listen connection for a service.
 */
static bool connection_setup_listen (intptr_t service, mg_connection **conn, bool sending)
{
  *conn = connection_setup (service, true, sending);
  if (*conn == NULL)
  {
    LOG_STDERR ("Fail to set-up listen socket for %s.\n", handler_descr(service));
    return (false);
  }
  return (true);
}

#if defined(PACKED_WEB_ROOT)
/**
 * Functions for a "Packed Web Filesystem"
 *
 * Functions in the generated '$(OBJ_DIR)/packed_webfs.c' file.
 */
extern const char *mg_unlist (size_t i);
extern unsigned    mg_usage_count (size_t i);

static size_t num_packed = 0;
static bool   has_index_html = false;

static void count_packed_fs (void)
{
  const char *fname;
  size_t      i;

  for (i = 0; (fname = mg_unlist(i)) != NULL; i++)
  {
    if (!strcmp(basename(fname), "index.html"))
       has_index_html = true;
  }
  num_packed = i;
}

static void show_packed_usage (void)
{
  unsigned i, count;
  const char *fname;

  for (i = 0; (fname = mg_unlist(i)) != NULL; i++)
  {
    count = mg_usage_count (i);
    if (count > 0)
       LOG_FILEONLY ("  %3u: %s\n", count, fname);
  }
}

static bool check_web_page (void)
{
  if (num_packed == 0)
  {
    LOG_STDERR ("The Packed Filesystem has no files!\n");
    return (false);
  }
  if (!has_index_html)
  {
    LOG_STDERR ("The Packed Filesystem has no 'index.html' file!\n");
    return (false);
  }
  return (true);
}

#else
static bool check_web_page (void)
{
  mg_file_path full_name;
  struct stat  st;

  snprintf (full_name, sizeof(full_name), "%s/%s", Modes.web_root, Modes.web_page);
  DEBUG (DEBUG_NET, "Web-page: \"%s\"\n", full_name);

  if (stat(full_name, &st) != 0)
  {
    LOG_STDERR ("Web-page \"%s\" does not exist.\n", full_name);
    return (false);
  }
  if (((st.st_mode) & _S_IFMT) != _S_IFREG)
  {
    LOG_STDERR ("Web-page \"%s\" is not a regular file.\n", full_name);
    return (false);
  }
  return (true);
}

static void show_packed_usage (void)
{
  LOG_FILEONLY ("  <None>\n");
}
#endif  /* PACKED_WEB_ROOT */

/**
 * Initialize the Mongoose network manager and:
 *  \li start the 2 active network services.
 *  \li or start the 4 listening (passive) network services.
 */
static bool modeS_init_net (void)
{
#if defined(PACKED_WEB_ROOT)
  Modes.touch_web_root = false;
  LOG_STDOUT ("Ignoring the '--web-page %s/%s' option since we use a built-in 'Packed Filesystem'.\n",
              Modes.web_root, Modes.web_page);

  strncpy (Modes.web_root, PACKED_WEB_ROOT, sizeof(Modes.web_root));
  strcpy (Modes.web_page, "index.html");
  count_packed_fs();
#endif

#if MG_ENABLE_FILE
  if (Modes.touch_web_root)
     touch_dir (Modes.web_root, true);
#endif

  mg_mgr_init (&Modes.mgr);

  if (Modes.net_active)
  {
    if (modeS_net_services[MODES_NET_SERVICE_RAW_IN].host &&
        !connection_setup_active (MODES_NET_SERVICE_RAW_IN, &Modes.raw_in))
       return (false);

    if (modeS_net_services[MODES_NET_SERVICE_SBS_IN].host &&
        !connection_setup_active (MODES_NET_SERVICE_SBS_IN, &Modes.sbs_in))
       return (false);

    if (!Modes.raw_in && !Modes.sbs_in)
    {
      LOG_STDERR ("No hosts for any `--net-active' services specified.\n");
      return (false);
    }
  }
  else
  {
    if (!connection_setup_listen(MODES_NET_SERVICE_RAW_IN, &Modes.raw_in, false))
       return (false);

    if (!connection_setup_listen(MODES_NET_SERVICE_RAW_OUT, &Modes.raw_out, true))
       return (false);

    if (!connection_setup_listen(MODES_NET_SERVICE_SBS_OUT, &Modes.sbs_out, true))
       return (false);

    if (!connection_setup_listen(MODES_NET_SERVICE_HTTP, &Modes.http_out, true))
       return (false);
  }
  if (Modes.http_out && !check_web_page())
     return (false);
  return (true);
}

/**
 * Write raw output to TCP clients.
 */
static void modeS_send_raw_output (const modeS_message *mm)
{
  char  msg [10 + 2*MODES_LONG_MSG_BYTES];
  char *p = msg;

  if (!handler_sending(MODES_NET_SERVICE_RAW_OUT))
     return;

  *p++ = '*';
  mg_hex (&mm->msg, mm->msg_bits/8, p);
  p = strchr (p, '\0');
  *p++ = ';';
  *p++ = '\n';
  connection_send (MODES_NET_SERVICE_RAW_OUT, msg, p - msg);
}

/**
 * Write SBS output to TCP clients (Base Station format).
 */
static void modeS_send_SBS_output (const modeS_message *mm, const aircraft *a)
{
  char msg [MODES_MAX_SBS_SIZE], *p = msg;
  int  emergency = 0, ground = 0, alert = 0, spi = 0;

  if (mm->msg_type == 4 || mm->msg_type == 5 || mm->msg_type == 21)
  {
    /**\note
     * identity is calculated/kept in base10 but is actually
     * octal (07500 is represented as 7500)
     */
    if (mm->identity == 7500 || mm->identity == 7600 || mm->identity == 7700)
       emergency = -1;
    if (mm->flight_status == 1 || mm->flight_status == 3)
       ground = -1;
    if (mm->flight_status == 2 || mm->flight_status == 3 || mm->flight_status == 4)
       alert = -1;
    if (mm->flight_status == 4 || mm->flight_status == 5)
       spi = -1;
  }

  /* Field 11 could contain the call-sign we can get from `aircraft_find_or_create()::reg_num`.
   */
  if (mm->msg_type == 0)
  {
    p += sprintf (p, "MSG,5,,,%06X,,,,,,,%d,,,,,,,,,,",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  mm->altitude);
  }
  else if (mm->msg_type == 4)
  {
    p += sprintf (p, "MSG,5,,,%06X,,,,,,,%d,,,,,,,%d,%d,%d,%d",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  mm->altitude, alert, emergency, spi, ground);
  }
  else if (mm->msg_type == 5)
  {
    p += sprintf (p, "MSG,6,,,%06X,,,,,,,,,,,,,%d,%d,%d,%d,%d",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  mm->identity, alert, emergency, spi, ground);
  }
  else if (mm->msg_type == 11)
  {
    p += sprintf (p, "MSG,8,,,%06X,,,,,,,,,,,,,,,,,",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]));
  }
  else if (mm->msg_type == 17 && mm->ME_type == 4)
  {
    p += sprintf (p, "MSG,1,,,%06X,,,,,,%s,,,,,,,,0,0,0,0",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  mm->flight);
  }
  else if (mm->msg_type == 17 && mm->ME_type >= 9 && mm->ME_type <= 18)
  {
    if (!VALID_POS(a->position))
         p += sprintf (p, "MSG,3,,,%06X,,,,,,,%d,,,,,,,0,0,0,0",
                       aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                       mm->altitude);
    else p += sprintf (p, "MSG,3,,,%06X,,,,,,,%d,,,%1.5f,%1.5f,,,0,0,0,0",
                       aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                       mm->altitude, a->position.lat, a->position.lon);
  }
  else if (mm->msg_type == 17 && mm->ME_type == 19 && mm->ME_subtype == 1)
  {
    int vr = (mm->vert_rate_sign == 0 ? 1 : -1) * 64 * (mm->vert_rate - 1);

    p += sprintf (p, "MSG,4,,,%06X,,,,,,,,%d,%d,,,%i,,0,0,0,0",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  a->speed, a->heading, vr);
  }
  else if (mm->msg_type == 21)
  {
    p += sprintf (p, "MSG,6,,,%06X,,,,,,,,,,,,,%d,%d,%d,%d,%d",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  mm->identity, alert, emergency, spi, ground);
  }
  else
    return;

  *p++ = '\n';
  connection_send (MODES_NET_SERVICE_SBS_OUT, msg, p - msg);
}

/**
 * Turn an hex digit into its 4 bit decimal value.
 * Returns -1 if the digit is not in the 0-F range.
 */
static int hex_digit_val (int c)
{
  c = tolower (c);
  if (c >= '0' && c <= '9')
     return (c - '0');
  if (c >= 'a' && c <= 'f')
     return (c - 'a' + 10);
  return (-1);
}

/**
 * This function decodes a string representing a Mode S message in
 * raw hex format like: `*8D4B969699155600E87406F5B69F;<eol>`
 *
 * The string is supposed to be at the start of the client buffer
 * and NUL-terminated. It accepts both `\n` and `\r\n` terminated records.
 *
 * The message is passed to the higher level layers, so it feeds
 * the selected screen output, the network output and so forth.
 *
 * If the message looks invalid, it is silently discarded.
 *
 * The `readsb` program will send 5 heart-beats like this:
 *  `*0000;\n*0000;\n*0000;\n*0000;\n*0000;\n` (35 bytes)
 *
 * on accepting a new client. Hence check for that too.
 */
static bool decode_hex_message (mg_iobuf *msg, int loop_cnt)
{
  modeS_message mm;
  uint8_t       bin_msg [MODES_LONG_MSG_BYTES];
  int           len, j;
  uint8_t      *hex;
  uint8_t      *end = memchr (msg->buf, '\n', msg->len);

  if (!end)
  {
    if (!Modes.interactive)
       LOG_STDOUT ("RAW(%d): Bogus msg: '%.*s'...\n", loop_cnt, (int)msg->len, msg->buf);
    Modes.stat.unrecognized_raw++;
    mg_iobuf_del (msg, 0, msg->len);
    return (false);
  }

  *end++ = '\0';
  if (end[-2] == '\r')
     end[-2] = '\0';

  /* Remove spaces on the left and on the right.
   */
  hex = msg->buf;
  len = end - msg->buf - 1;

  if (!strcmp((const char*)hex, MODES_RAW_HEART_BEAT))
  {
    DEBUG (DEBUG_NET, "Got heart-beat signal.\n");
    mg_iobuf_del (msg, 0, msg->len);
    return (true);
  }

  while (len && isspace(hex[len-1]))
  {
    hex[len-1] = '\0';
    len--;
  }
  while (isspace(*hex))
  {
    hex++;
    len--;
  }

  /* Check it's format.
   */
  if (len < 2)
  {
    Modes.stat.empty_raw++;
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }
  if (hex[0] != '*' || !memchr(msg->buf, ';', len))
  {
    Modes.stat.unrecognized_raw++;
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }

  /* Turn the message into binary.
   */
  hex++;   /* Skip `*` and `;` */
  len -= 2;
  if (len > 2*MODES_LONG_MSG_BYTES)   /* Too long message... broken. */
  {
    Modes.stat.unrecognized_raw++;
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }

  for (j = 0; j < len; j += 2)
  {
    int high = hex_digit_val (hex[j]);
    int low  = hex_digit_val (hex[j+1]);

    if (high == -1 || low == -1)
    {
      Modes.stat.unrecognized_raw++;
      mg_iobuf_del (msg, 0, end - msg->buf);
      return (false);
    }
    bin_msg[j/2] = (high << 4) | low;
  }
  mg_iobuf_del (msg, 0, end - msg->buf);
  Modes.stat.good_raw++;
  decode_modeS_message (&mm, bin_msg);
  if (mm.CRC_ok)
     modeS_user_message (&mm);
  return (true);
}

/**
 * \todo
 * Ref: http://woodair.net/sbs/article/barebones42_socket_data.htm
 */
static int modeS_recv_SBS_input (mg_iobuf *msg, modeS_message *mm)
{
  memset (mm, '\0', sizeof(*mm));

#if 0
  /* decode 'msg' and fill 'mm'
   */
  if (mm->CRC_ok)
     modeS_user_message (&mm);
#endif

  MODES_NOTUSED (msg);
  return (0);
}

/**
 * This function decodes a string representing a Mode S message in
 * SBS format (Base Station) like:
 * ```
 * MSG,5,1,1,4CC52B,1,2021/09/20,23:30:43.897,2021/09/20,23:30:43.901,,38000,,,,,,,0,,0,
 * ```
 *
 * It accepts both '\n' and '\r\n' terminated records.
 */
static bool decode_SBS_message (mg_iobuf *msg, int loop_cnt)
{
  modeS_message mm;
  uint8_t      *end = memchr (msg->buf, '\n', msg->len);

  if (!end)
  {
    if (!Modes.interactive)
       LOG_STDOUT ("SBS(%d): Bogus msg: '%.*s'...\n", loop_cnt, (int)msg->len, msg->buf);
    Modes.stat.unrecognized_SBS++;
    mg_iobuf_del (msg, 0, msg->len);
    return (false);
  }
  *end++ = '\0';
  if (end[-2] == '\r')
     end[-2] = '\0';

  if (!Modes.interactive)
     LOG_STDOUT ("SBS(%d): '%s'\n", loop_cnt, msg->buf);

  if (!strncmp((char*)msg->buf, "MSG,", 4))
  {
    modeS_recv_SBS_input (msg, &mm);
    Modes.stat.good_SBS++;
  }
  mg_iobuf_del (msg, 0, end - msg->buf);
  return (true);
}

/**
 * This function reads client/server data for services:
 *  \li `MODES_NET_SERVICE_RAW_IN` or
 *  \li `MODES_NET_SERVICE_SBS_IN`
 *
 * when the event `MG_EV_READ` is received in `connection_handler()`.
 *
 * The message is supposed to be separated by the next message by the
 * separator `sep`, that is a NUL-terminated C string.
 *
 * The `handler` function is responsible for freeing `msg` as it consumes each record
 * in the `msg`. This `msg` can consist of several records or incomplete records since
 * Mongoose uses non-blocking sockets.
 *
 * The `tools/SBS_client.py` script is sending this in "RAW-OUT" test-mode:
 * ```
 *  *8d4b969699155600e87406f5b69f;\n
 * ```
 *
 * This message shows up as ICAO "4B9696" and Reg-num "TC-ETV" in `--interactive` mode.
 */
static void connection_read (connection *conn, msg_handler handler, bool is_server)
{
  mg_iobuf *msg;
  int       loops;

  if (!conn)
     return;

  msg = &conn->conn->recv;
  if (msg->len == 0)
  {
    DEBUG (DEBUG_NET2, "No msg for %s.\n", is_server ? "server" : "client");
    return;
  }

  for (loops = 0; msg->len > 0; loops++)
  {
    DEBUG (DEBUG_NET2, "%s msg(%d): '%.*s'.\n", is_server ? "server" : "client", loops, (int)msg->len, msg->buf);
    (*handler) (msg, loops);
  }
}

#if defined(USE_CURSES)
  #define TUI_HELP  "wincon|curses      Select 'Windows-Console' or 'PCurses' interface at run-time.\n"
#else
  #define TUI_HELP  "wincon             'Windows-Console' is the default TUI.\n"
#endif

/**
 * Show the program usage
 */
static void show_help (const char *fmt, ...)
{
  if (fmt)
  {
    va_list args;

    va_start (args, fmt);
    vprintf (fmt, args);
    va_end (args);
  }
  else
  {
    printf ("A 1090 MHz receiver, decoder and web-server for ADS-B (Automatic Dependent Surveillance - Broadcast).\n"
            "Usage: %s [options]\n"
            "  General options:\n"
            "    --airports <file>        The CSV file for the airports database\n"
            "                             (default: `%s').\n"
            "    --aircrafts <file>       The CSV file for the aircrafts database\n"
            "                             (default: `%s').\n"
            "    --aircrafts-update<=url> Redownload the above .csv-file if older than 10 days,\n"
            "                             recreate the `<file>.sqlite' and exit the program.\n"
            "                             (default URL: `%s').\n"
            "    --aircrafts-sql          Create a `<file>.sqlite' from the above .CSV-file if it does not exist.\n"
            "                             Or use the `<file>.sqlite' if it exist.\n"
            "    --debug <flags>          Debug mode; see below for details.\n"
            "    --infile <filename>      Read data from file (use `-' for stdin).\n"
            "    --interactive            Interactive mode with a smimple TUI.\n"
            "    --interactive-ttl <sec>  Remove aircraft if not seen for <sec> (default: %u).\n"
            "    --location               Use `Windows Location API' to get the `DUMP1090_HOMEPOS'.\n"
            "    --logfile <file>         Enable logging to file (default: off)\n"
            "    --loop <N>               With `--infile', read the file in a loop <N> times (default: 2^63).\n"
            "    --metric                 Use metric units (meters, km/h, ...).\n"
            "    --silent                 Silent mode for testing network I/O (together with `--debug n').\n"
            "    --test                   Perform some test of internal functions.\n"
            "    --tui " TUI_HELP
            "     -V, -VV                 Show version info. `-VV' for details.\n"
            "     -h, --help              Show this help.\n\n",
            Modes.who_am_I, Modes.airport_db, Modes.aircraft_db, AIRCRAFT_DATABASE_URL, MODES_INTERACTIVE_TTL/1000);

    printf ("  Mode-S decoder options:\n"
            "    --aggressive             Use a more aggressive CRC check (two bits fixes, ...).\n"
            "    --max-messages <N>       Max number of messages to process (default: Infinite).\n"
            "    --no-fix                 Disable single-bits error correction using CRC.\n"
            "    --no-crc-check           Disable checking CRC of messages (discouraged).\n"
            "    --only-addr              Show only ICAO addresses (for testing).\n"
            "    --raw                    Show only the raw Mode-S hex message.\n"
            "    --strip <level>          Strip IQ file removing samples below `level'.\n\n");

    printf ("  Network options:\n"
            "    --net                    Enable network listening services.\n"
            "    --net-active             Enable network active services.\n"
            "    --net-only               Enable just networking, no physical device or file.\n"
            "    --net-http-port <port>   TCP listening port for HTTP server (default: %u).\n"
            "    --net-ri-port <port>     TCP listening port for raw input   (default: %u).\n"
            "    --net-ro-port <port>     TCP listening port for raw output  (default: %u).\n"
            "    --net-sbs-port <port>    TCP listening port for SBS output  (default: %u).\n"
            "    --no-keep-alive          Ignore `Connection: keep-alive' from HTTP clients.\n"
            "    --host-raw <addr:port>   Remote host/port for raw input with `--net-active'.\n"
            "    --host-sbs <addr:port>   Remote host/port for SBS input with `--net-active'.\n"
            "    --web-page <file>        The Web-page to serve for HTTP clients\n"
            "                             (default: `%s/%s').\n\n",
            MODES_NET_PORT_HTTP, MODES_NET_PORT_RAW_IN, MODES_NET_PORT_RAW_OUT,
            MODES_NET_PORT_SBS, Modes.web_root, Modes.web_page);

    printf ("  RTLSDR / SDRplay options:\n"
            "    --agc                    Enable Digital AGC              (default: off).\n"
            "    --bias                   Enable Bias-T output            (default: off).\n"
            "    --calibrate              Enable calibrating R820 devices (default: off).\n"
            "    --device <N / name>      Select device                   (default: 0; first found).\n"
            "                             e.g. `--device 0'              - select first RTLSDR device found.\n"
            "                                  `--device RTL2838-silver' - select on RTLSDR name.\n"
            "                                  `--device sdrplay'        - select first SDRPlay device found.\n"
            "                                  `--device sdrplay1'       - select on SDRPlay index.\n"
            "                                  `--device sdrplayRSP1A'   - select on SDRPlay name.\n"
            "    --freq <Hz>              Set frequency                   (default: %.0f MHz).\n"
            "    --gain <dB>              Set gain                        (default: AUTO).\n"
            "    --if-mode <ZIF | LIF>    Intermediate Frequency mode     (default: ZIF).\n"
            "    --ppm <correction>       Set frequency correction        (default: 0).\n"
            "    --samplerate <Hz>        Set sample-rate                 (default: %.0f MS/s).\n\n",
            MODES_DEFAULT_FREQ / 1E6, MODES_DEFAULT_RATE/1E6);

    printf ("  --debug <flags>: c = Log frames with bad CRC.\n"
            "                   C = Log frames with good CRC.\n"
            "                   D = Log frames decoded with 0 errors.\n"
            "                   E = Log frames decoded with errors.\n"
            "                   g = Log general debugging info.\n"
            "                   G = A bit more general debug info than flag `g'.\n"
            "                   j = Log frames to `frames.js', loadable by `debug.html'.\n"
            "                   m = Log activity in `externals/mongoose.c'.\n"
            "                   M = Log more activity in `externals/mongoose.c'.\n"
            "                   n = Log network debugging information.\n"
            "                   N = A bit more network information than flag `n'.\n"
            "                   p = Log frames with bad preamble.\n\n");

    printf ("  If the `--location' option is not used, your home-position for distance calculation can be set like:\n"
            "  `c:\\> set DUMP1090_HOMEPOS=51.5285578,-0.2420247' for London.\n");
  }

  modeS_exit();
  exit (0);
}

/**
 * This background function is called continously by `main_data_loop()`.
 * It performs:
 *  \li Removes inactive aircrafts from the list.
 *  \li Polls the network for events blocking less than 125 msec.
 *  \li Polls the `Windows Location API` for a location.
 *  \li Refreshes interactive data every 250 msec (`MODES_INTERACTIVE_REFRESH_TIME`).
 *  \li Refreshes the console-title with some statistics (also 4 times per second).
 */
static void background_tasks (void)
{
  bool     refresh;
  pos_t    pos;
  uint64_t now;

  if (Modes.net)
     mg_mgr_poll (&Modes.mgr, MG_NET_POLL_TIME);   /* Poll Mongoose for network events */

  if (Modes.exit)
     return;

  /* Check the asynchronous result from `Location API`.
   */
  if (Modes.win_location && location_poll(&pos))
  {
    /* Assume our location won't change while running this program.
     * Hence just stop the `Location API` event-handler.
     */
    location_exit();
    Modes.home_pos = pos;

    spherical_to_cartesian (&Modes.home_pos, &Modes.home_pos_cart);
    if (Modes.home_pos_ok)
       LOG_FILEONLY ("Ignoring the 'DUMP1090_HOMEPOS' env-var "
                     "since we use the 'Windows Location API': Latitude: %.6f, Longitude: %.6f.\n",
                     Modes.home_pos.lat, Modes.home_pos.lon);
    Modes.home_pos_ok = true;
  }

  now = MSEC_TIME();

  refresh = (now - Modes.last_update_ms) >= MODES_INTERACTIVE_REFRESH_TIME;
  if (!refresh)
     return;

  Modes.last_update_ms = now;

  if (Modes.log)
     fflush (Modes.log);

  aircraft_remove_stale (now);

  /* Refresh screen and console-title when in interactive mode
   */
  if (Modes.interactive)
     interactive_show_data (now);

  if (Modes.rtlsdr.device || Modes.sdrplay.device)
  {
    interactive_title_stats();
    interactive_update_gain();
    interactive_other_stats();  /* Only effective if '--tui curses' was used */
  }
#if 0
  else
    interactive_raw_SBS_stats();
#endif
}

/**
 * The handler called in for `SIGINT` or `SIGBREAK` . <br>
 * I.e. user presses `^C`.
 */
static void signal_handler (int sig)
{
  int rc;

  if (sig > 0)
     signal (sig, SIG_DFL);   /* reset signal handler - bit extra safety */

  Modes.exit = true;          /* Signal to threads that we are done */

  /* When PDCurses exists, it restores the startup console-screen.
   * Hence make it clear what is printed on exit by separating the
   * startup and shutdown messages with a dotted "----" bar.
   */
  if ((sig == SIGINT || sig == SIGBREAK || sig == SIGABRT) && Modes.tui_interface == TUI_CURSES)
     puts ("----------------------------------------------------------------------------------");

  if (sig == SIGINT)
     LOG_STDOUT ("Caught SIGINT, shutting down ...\n");
  else if (sig == SIGBREAK)
     LOG_STDOUT ("Caught SIGBREAK, shutting down ...\n");
  else if (sig == SIGABRT)
     LOG_STDOUT ("Caught SIGABRT, shutting down ...\n");
  else if (sig == 0)
     DEBUG (DEBUG_GENERAL, "Breaking 'main_data_loop()', shutting down ...\n");

  if (Modes.rtlsdr.device)
  {
    EnterCriticalSection (&Modes.data_mutex);
    rc = rtlsdr_cancel_async (Modes.rtlsdr.device);
    DEBUG (DEBUG_GENERAL, "rtlsdr_cancel_async(): rc: %d.\n", rc);

    if (rc == -2)  /* RTLSDR is not streaming data */
       Sleep (5);
    LeaveCriticalSection (&Modes.data_mutex);
  }
  else if (Modes.sdrplay.device)
  {
#if !defined(USE_RTLSDR_EMUL)
    rc = sdrplay_cancel_async (Modes.sdrplay.device);
    DEBUG (DEBUG_GENERAL, "sdrplay_cancel_async(): rc: %d / %s.\n", rc, sdrplay_strerror(rc));
#endif
  }
}

static void show_network_stats (void)
{
  const char *cli_srv = (Modes.net_active ? "server" : "client(s)");
  uint64_t    sum;
  int         s;

  LOG_STDOUT ("\nNetwork statistics:\n");

  for (s = MODES_NET_SERVICE_RAW_OUT; s < MODES_NET_SERVICES_NUM; s++)
  {
    LOG_STDOUT ("  %s (port %u):\n", handler_descr(s), handler_port(s));

    if (s == MODES_NET_SERVICE_HTTP)
    {
      if (Modes.net_active)
      {
        LOG_STDOUT ("    Not used.\n");
        continue;
      }
      LOG_STDOUT ("    %8llu HTTP GET requests received.\n", Modes.stat.HTTP_get_requests);
      LOG_STDOUT ("    %8llu HTTP 400 replies sent.\n", Modes.stat.HTTP_400_responses);
      LOG_STDOUT ("    %8llu HTTP 404 replies sent.\n", Modes.stat.HTTP_404_responses);
      LOG_STDOUT ("    %8llu HTTP/WebSocket upgrades.\n", Modes.stat.HTTP_websockets);
      LOG_STDOUT ("    %8llu server connection \"keep-alive\".\n", Modes.stat.HTTP_keep_alive_sent);
      LOG_STDOUT ("    %8llu client connection \"keep-alive\".\n", Modes.stat.HTTP_keep_alive_recv);
    }

    if (Modes.net_active)
         sum = Modes.stat.srv_connected[s] + Modes.stat.srv_removed[s] + Modes.stat.srv_unknown[s];
    else sum = Modes.stat.cli_accepted[s]  + Modes.stat.cli_removed[s] + Modes.stat.cli_unknown[s];

    sum += Modes.stat.bytes_sent[s] + Modes.stat.bytes_recv[s] + *handler_num_connections (s);

    if (sum == 0ULL)
    {
      LOG_STDOUT ("    Nothing.\n");
      continue;
    }

    if (Modes.net_active)
    {
      LOG_STDOUT ("    %8llu server connections done.\n", Modes.stat.srv_connected[s]);
      LOG_STDOUT ("    %8llu server connections removed.\n", Modes.stat.srv_removed[s]);
      LOG_STDOUT ("    %8llu server connections unknown.\n", Modes.stat.srv_unknown[s]);
    }
    else
    {
      LOG_STDOUT ("    %8llu client connections accepted.\n", Modes.stat.cli_accepted[s]);
      LOG_STDOUT ("    %8llu client connections removed.\n", Modes.stat.cli_removed[s]);
      LOG_STDOUT ("    %8llu client connections unknown.\n", Modes.stat.cli_unknown[s]);
    }

    LOG_STDOUT ("    %8llu bytes sent.\n", Modes.stat.bytes_sent[s]);
    LOG_STDOUT ("    %8llu bytes recv.\n", Modes.stat.bytes_recv[s]);
    LOG_STDOUT ("    %8u %s now.\n", *handler_num_connections(s), cli_srv);
  }

  LOG_FILEONLY ("\nPacked-Web statistics:\n");
  show_packed_usage();
}

static void show_raw_SBS_stats (void)
{
  LOG_STDOUT ("  SBS-in:  %8llu good messages.\n", Modes.stat.good_SBS);
  LOG_STDOUT ("           %8llu unrecognized messages.\n", Modes.stat.unrecognized_SBS);
  LOG_STDOUT ("           %8llu empty messages.\n", Modes.stat.empty_SBS);
  LOG_STDOUT ("  Raw-in:  %8llu good messages.\n", Modes.stat.good_raw);
  LOG_STDOUT ("           %8llu unrecognized messages.\n", Modes.stat.unrecognized_raw);
  LOG_STDOUT ("           %8llu empty messages.\n", Modes.stat.empty_raw);
  LOG_STDOUT ("  Unknown: %8llu empty messages.\n", Modes.stat.empty_unknown);
}

static void show_decoder_stats (void)
{
  LOG_STDOUT ("Decoder statistics:\n");
  interactive_clreol();  /* to clear the lines from startup messages */

  LOG_STDOUT (" %8llu valid preambles.\n", Modes.stat.valid_preamble);
  interactive_clreol();

  LOG_STDOUT (" %8llu demodulated after phase correction.\n", Modes.stat.out_of_phase);
  interactive_clreol();

  LOG_STDOUT (" %8llu demodulated with 0 errors.\n", Modes.stat.demodulated);
  interactive_clreol();

  LOG_STDOUT (" %8llu with CRC okay.\n", Modes.stat.good_CRC);
  interactive_clreol();

  LOG_STDOUT (" %8llu with CRC failure.\n", Modes.stat.bad_CRC);
  interactive_clreol();

  LOG_STDOUT (" %8llu errors corrected.\n", Modes.stat.fixed);
  interactive_clreol();

  LOG_STDOUT (" %8llu messages with 1 bit errors fixed.\n", Modes.stat.single_bit_fix);
  interactive_clreol();

  LOG_STDOUT (" %8llu messages with 2 bit errors fixed.\n", Modes.stat.two_bits_fix);
  interactive_clreol();

  LOG_STDOUT (" %8llu total usable messages (%llu + %llu).\n", Modes.stat.good_CRC + Modes.stat.fixed, Modes.stat.good_CRC, Modes.stat.fixed);
  interactive_clreol();

  LOG_STDOUT (" %8llu unique aircrafts of which %llu was in CSV-file and %llu in SQL-file.\n",
              Modes.stat.unique_aircrafts, Modes.stat.unique_aircrafts_CSV, Modes.stat.unique_aircrafts_SQL);

  print_unrecognized_ME();
}

static void show_statistics (void)
{
  if (!Modes.net_only)
     show_decoder_stats();
  if (Modes.net)
     show_network_stats();
  if (Modes.net_active)
     show_raw_SBS_stats();
}

/**
 * Our exit function. Free all resources here.
 */
static void modeS_exit (void)
{
  int rc;

  if (Modes.net)
  {
    unsigned num = connection_free_all();

    net_flushall();
    mg_mgr_free (&Modes.mgr);
    Modes.mgr.conns = NULL;
    if (num > 0)
       Sleep (100);
  }

  if (Modes.rtlsdr.device)
  {
    if (Modes.bias_tee)
       verbose_bias_tee (Modes.rtlsdr.device, 0);
    Modes.bias_tee = 0;
    rc = rtlsdr_close (Modes.rtlsdr.device);
    free (Modes.rtlsdr.gains);
    Modes.rtlsdr.device = NULL;
    DEBUG (DEBUG_GENERAL2, "rtlsdr_close(), rc: %d.\n", rc);
  }
  else if (Modes.sdrplay.device)
  {
    rc = sdrplay_exit (Modes.sdrplay.device);
    free (Modes.sdrplay.gains);
    Modes.sdrplay.device = NULL;
    DEBUG (DEBUG_GENERAL2, "sdrplay_exit(), rc: %d.\n", rc);
  }

  if (Modes.reader_thread)
     CloseHandle ((HANDLE)Modes.reader_thread);

  if (Modes.fd > STDIN_FILENO)
     _close (Modes.fd);

  aircraft_exit (true);
  airports_exit (true);

  if (Modes.interactive)
     interactive_exit();

#if !defined(USE_GEN_LUT)
  free (Modes.magnitude_lut);
#endif
  free (Modes.magnitude);
  free (Modes.data);
  free (Modes.ICAO_cache);
  free (Modes.selected_dev);
  free (Modes.rtlsdr.name);
  free (Modes.sdrplay.name);

  DeleteCriticalSection (&Modes.data_mutex);
  DeleteCriticalSection (&Modes.print_mutex);

  Modes.reader_thread = 0;
  Modes.data          = NULL;
  Modes.magnitude     = NULL;
  Modes.magnitude_lut = NULL;
  Modes.ICAO_cache    = NULL;
  Modes.selected_dev  = NULL;

  if (Modes.win_location)
     location_exit();

  if (Modes.log)
  {
    if (!Modes.home_pos_ok)
       LOG_FILEONLY ("A valid home-position was not used.\n");
    fclose (Modes.log);
  }

#if defined(USE_RTLSDR_EMUL)
  RTLSDR_emul_unload_DLL();
#endif

#if defined(_DEBUG)
  crtdbug_exit();
#endif
}

static void select_device (const char *arg)
{
  static bool dev_selection_done = false;

  if (dev_selection_done)
     show_help ("Option '--device' already done.\n\n");

  if (isdigit(arg[0]))
     Modes.rtlsdr.index = atoi (arg);
  else
  {
    Modes.rtlsdr.name  = strdup (arg);
    Modes.rtlsdr.index = -1;  /* select on name only */
  }

  if (!strnicmp(arg, "sdrplay", 7))
  {
    Modes.sdrplay.name = strdup (arg);
    if (isdigit(arg[7]))
    {
      Modes.sdrplay.index   = atoi (arg+7);
      Modes.sdrplay.name[7] = '\0';
    }
    else
      Modes.sdrplay.index = -1;
  }
  dev_selection_done = true;
}

static void select_tui (const char *arg)
{
  if (!stricmp(arg, "wincon"))
       Modes.tui_interface = TUI_WINCON;
  else if (!stricmp(arg, "curses"))
       Modes.tui_interface = TUI_CURSES;
  else show_help ("Unknown `--tui %s' mode.\n", arg);

#if !defined(USE_CURSES)
  if (Modes.tui_interface == TUI_CURSES)
     show_help ("I was not built with '-DUSE_CURSES'. Use `--tui wincon' or nothing.\n");
#endif
}

static void set_debug_bits (const char *flags)
{
  while (*flags)
  {
    switch (*flags)
    {
      case 'C':
           Modes.debug |= DEBUG_GOODCRC;
           break;
      case 'c':
           Modes.debug |= DEBUG_BADCRC;
           break;
      case 'D':
           Modes.debug |= DEBUG_DEMOD;
           break;
      case 'E':
           Modes.debug |= DEBUG_DEMODERR;
           break;
      case 'g':
           Modes.debug |= DEBUG_GENERAL;
           break;
      case 'G':
           Modes.debug |= (DEBUG_GENERAL2 | DEBUG_GENERAL);
           break;
      case 'j':
      case 'J':
           Modes.debug |= DEBUG_JS;
           break;
      case 'm':
           Modes.debug |= DEBUG_MONGOOSE;
           break;
      case 'M':
           Modes.debug |= DEBUG_MONGOOSE2;
           break;
      case 'n':
           Modes.debug |= DEBUG_NET;
           break;
      case 'N':
           Modes.debug |= (DEBUG_NET2 | DEBUG_NET);  /* A bit more network details */
           break;
      case 'p':
      case 'P':
           Modes.debug |= DEBUG_NOPREAMBLE;
           break;
      default:
           show_help ("Unknown debugging flag: %c\n", *flags);
           /* not reached */
           break;
    }
    flags++;
  }
}

static void select_if_mode (const char *arg)
{
  if (!stricmp(arg, "zif"))
       Modes.sdrplay.if_mode = false;
  else if (!stricmp(arg, "lif"))
       Modes.sdrplay.if_mode = true;
  else show_help ("Illegal '--if-mode': %s.\n", arg);
}

static struct option long_options[] = {
  { "agc",              no_argument,        &Modes.dig_agc,            1  },
  { "aggressive",       no_argument,        &Modes.aggressive,         1  },
  { "airports",         required_argument,  NULL,                     'a' },
  { "aircrafts",        required_argument,  NULL,                     'b' },
  { "aircrafts-update", optional_argument,  NULL,                     'u' },
  { "aircrafts-sql",    no_argument,        &Modes.use_sql_db,         1  },
  { "bandwidth",        required_argument,  NULL,                     'B' },
  { "bias",             no_argument,        &Modes.bias_tee,           1  },
  { "calibrate",        no_argument,        &Modes.rtlsdr.calibrate,   1  },
  { "debug",            required_argument,  NULL,                     'd' },
  { "device",           required_argument,  NULL,                     'D' },
  { "freq",             required_argument,  NULL,                     'f' },
  { "gain",             required_argument,  NULL,                     'g' },
  { "help",             no_argument,        NULL,                     'h' },
  { "if-mode",          required_argument,  NULL,                     'I' },
  { "infile",           required_argument,  NULL,                     'i' },
  { "interactive",      no_argument,        &Modes.interactive,        1  },
  { "interactive-ttl",  required_argument,  NULL,                     't' },
  { "location",         no_argument,        &Modes.win_location,       1  },
  { "logfile",          required_argument,  NULL,                     'L' },
  { "loop",             optional_argument,  NULL,                     'l' },
  { "max-messages",     required_argument,  NULL,                     'm' },
  { "metric",           no_argument,        &Modes.metric,             1  },
  { "net",              no_argument,        &Modes.net,                1  },
  { "net-active",       no_argument,        &Modes.net_active,         1  },
  { "net-only",         no_argument,        &Modes.net_only,           1  },
  { "net-http-port",    required_argument,  NULL,                     'y' + MODES_NET_SERVICE_HTTP },
  { "net-ri-port",      required_argument,  NULL,                     'y' + MODES_NET_SERVICE_RAW_IN },
  { "net-ro-port",      required_argument,  NULL,                     'y' + MODES_NET_SERVICE_RAW_OUT },
  { "net-sbs-port",     required_argument,  NULL,                     'y' + MODES_NET_SERVICE_SBS_OUT },
  { "host-raw",         required_argument,  NULL,                     'Z' + MODES_NET_SERVICE_RAW_IN },
  { "host-sbs",         required_argument,  NULL,                     'Z' + MODES_NET_SERVICE_SBS_IN },
  { "no-keep-alive",    no_argument,        &Modes.keep_alive,         0  },
  { "only-addr",        no_argument,        &Modes.only_addr,          1  },
  { "ppm",              required_argument,  NULL,                     'p' },
  { "raw",              no_argument,        &Modes.raw,                1  },
  { "samplerate",       required_argument,  NULL,                     's' },
  { "silent",           no_argument,        &Modes.silent,             1  },
  { "strip",            required_argument,  NULL,                     'S' },
  { "web-page",         required_argument,  NULL,                     'w' },
  { "test",             optional_argument,  NULL,                     'T' },
#if MG_ENABLE_FILE
  { "touch",            no_argument,        &Modes.touch_web_root,     1  },
#endif
  { "tui",              required_argument,  NULL,                     'A' },
  { NULL,               no_argument,        NULL,                      0  }
};

static bool parse_cmd_line (int argc, char **argv)
{
  char *end;
  int   c, show_ver = 0, idx = 0;
  bool  rc = true;

  while ((c = getopt_long (argc, argv, "+h?V", long_options, &idx)) != EOF)
  {
 /* printf ("c: '%c' / %d, long_options[%d]: '%s'\n", c, c, idx, long_options[idx].name); */

    switch (c)
    {
      case 'a':
           strncpy (Modes.airport_db, optarg, sizeof(Modes.airport_db)-1);
           break;

      case 'b':
           strncpy (Modes.aircraft_db, optarg, sizeof(Modes.aircraft_db)-1);
           break;

      case 'B':
           Modes.band_width = ato_hertz (optarg);
           if (Modes.band_width == 0)
              show_help ("Illegal band-width: %s\n", optarg);
           break;

      case 'D':
           select_device (optarg);
           break;

      case 'd':
           set_debug_bits (optarg);
           break;

      case 'f':
           Modes.freq = ato_hertz (optarg);
           if (Modes.freq == 0)
              show_help ("Illegal frequency: %s\n", optarg);
           break;

      case 'g':
           if (!stricmp(optarg, "auto"))
              Modes.gain_auto = true;
           else
           {
             Modes.gain = (uint16_t) (10.0 * strtof(optarg, &end));  /* Gain is in tens of dBs */
             if (end == optarg || *end != '\0')
                show_help ("Illegal gain: %s.\n", optarg);
             Modes.gain_auto = false;
           }
           break;

      case 'I':
           select_if_mode (optarg);
           break;

      case 'i':
           Modes.infile = optarg;
           break;

      case 'l':
           Modes.loops = optarg ? _atoi64 (optarg) : LLONG_MAX;
           break;

      case 'L':
           Modes.logfile = optarg;
           break;

      case 'm':
           Modes.max_messages = _atoi64 (optarg);
           break;

      case 'n':
           Modes.net_only = Modes.net = true;
           break;

      case 'N':
           Modes.net_active = Modes.net = true;
           break;

      case 'u':
           Modes.aircraft_db_update = optarg ? optarg : AIRCRAFT_DATABASE_URL;
           break;

      case 'y' + MODES_NET_SERVICE_RAW_OUT:
           modeS_net_services [MODES_NET_SERVICE_RAW_OUT].port = (uint16_t) atoi (optarg);
           break;

      case 'y' + MODES_NET_SERVICE_RAW_IN:
           modeS_net_services [MODES_NET_SERVICE_RAW_IN].port = (uint16_t) atoi (optarg);
           break;

      case 'y' + MODES_NET_SERVICE_HTTP:
           modeS_net_services [MODES_NET_SERVICE_HTTP].port = (uint16_t) atoi (optarg);
           break;

      case 'y' + MODES_NET_SERVICE_SBS_OUT:
           modeS_net_services [MODES_NET_SERVICE_SBS_OUT].port = (uint16_t) atoi (optarg);
           break;

      case 'Z' + MODES_NET_SERVICE_RAW_OUT:
           modeS_net_services [MODES_NET_SERVICE_RAW_OUT].host = optarg;
           break;

      case 'Z' + MODES_NET_SERVICE_RAW_IN:
           if (!set_host_port (optarg, &modeS_net_services [MODES_NET_SERVICE_RAW_IN], MODES_NET_PORT_RAW_IN))
              rc = false;
           break;

      case 'Z' + MODES_NET_SERVICE_SBS_IN:
           if (!set_host_port (optarg, &modeS_net_services [MODES_NET_SERVICE_SBS_IN], MODES_NET_PORT_SBS))
              rc = false;
           break;

      case 'p':
           Modes.rtlsdr.ppm_error = atoi (optarg);
           break;

      case 's':
           Modes.sample_rate = ato_hertz (optarg);
           if (Modes.sample_rate == 0)
              show_help ("Illegal sample_rate: %s\n", optarg);
           break;

      case 'S':
           Modes.strip_level = atoi (optarg);
           if (Modes.strip_level == 0)
              show_help ("Illegal --strip level %d.\n\n", Modes.strip_level);
           break;

      case 't':
           Modes.interactive_ttl = 1000 * atoi (optarg);
           break;

      case 'T':
           Modes.tests++;
           Modes.tests_arg = optarg ? atoi (optarg) : 0;
           break;

      case 'V':
           show_ver++;
           break;

      case 'w':
           strncpy (Modes.web_root, dirname(optarg), sizeof(Modes.web_root)-1);
           strncpy (Modes.web_page, basename(optarg), sizeof(Modes.web_page)-1);
           slashify (Modes.web_root);
           break;

      case 'A':        /* option `--tui wincon|curses' */
           select_tui (optarg);
           break;

      case 'h':
      case '?':
           show_help (NULL);
           break;
    }
  }

  if (show_ver > 0)
     show_version_info (show_ver >= 2);

  if (Modes.net_only || Modes.net_active)
     Modes.net = Modes.net_only = true;

  return (rc);
}

/**
 * Our main entry.
 */
int main (int argc, char **argv)
{
  bool dev_opened = false;
  bool net_opened = false;
  int  rc;

#if defined(_DEBUG)
  crtdbug_init();
#endif

  modeS_init_config();  /* Set sane defaults */

  if (!parse_cmd_line (argc, argv))
     goto quit;

  rc = modeS_init();    /* Initialization based on cmd-line options */
  if (!rc)
     goto quit;

  if (Modes.net_only)
  {
    LOG_STDERR ("Net-only mode, no physical device or file open.\n");
  }
  else if (Modes.strip_level)
  {
    rc = strip_mode (Modes.strip_level);
    _setmode (_fileno(stdin), O_TEXT);
    _setmode (_fileno(stdout), O_TEXT);
  }
  else if (Modes.infile)
  {
    rc = 1;
    if (Modes.infile[0] == '-' && Modes.infile[1] == '\0')
    {
      Modes.fd = STDIN_FILENO;
    }
    else if ((Modes.fd = _open(Modes.infile, O_RDONLY | O_BINARY)) == -1)
    {
      LOG_STDERR ("Error opening `%s`: %s\n", Modes.infile, strerror(errno));
      goto quit;
    }
  }
  else
  {
    if (Modes.sdrplay.name)
    {
#ifdef USE_RTLSDR_EMUL
      Modes.emul_loaded = RTLSDR_emul_load_DLL();
      if (!Modes.emul_loaded)
      {
        LOG_STDERR ("Cannot use device `%s` without `%s` loaded.\nError: %s\n",
                    Modes.sdrplay.name, emul.dll_name, trace_strerror(emul.last_rc));
        goto quit;
      }
#endif

      rc = sdrplay_init (Modes.sdrplay.name, Modes.sdrplay.index, &Modes.sdrplay.device);
      DEBUG (DEBUG_GENERAL, "sdrplay_init(): rc: %d / %s.\n", rc, sdrplay_strerror(rc));
      if (rc)
         goto quit;
    }
    else
    {
      rc = modeS_init_RTLSDR();
      DEBUG (DEBUG_GENERAL, "modeS_init_RTLSDR(): rc: %d.\n", rc);
      if (!rc)
         goto quit;
      dev_opened = true;
    }
  }

  if (Modes.net)
  {
    rc = modeS_init_net();
    DEBUG (DEBUG_GENERAL, "modeS_init_net(): rc: %d.\n", rc);
    if (!rc)
       goto quit;
    net_opened = true;
  }

  if (Modes.infile)
  {
    if (read_from_data_file() == 0)
       LOG_STDERR ("No good messages found in '%s'.\n", Modes.infile);
  }
  else if (Modes.strip_level == 0)
  {
    /* Create the thread that will read the data from the RTLSDR or SDRplay device.
     */
    Modes.reader_thread = _beginthreadex (NULL, 0, data_thread_fn, NULL, 0, NULL);
    if (!Modes.reader_thread)
    {
      LOG_STDERR ("_beginthreadex() failed: %s.\n", strerror(errno));
      goto quit;
    }
    main_data_loop();
  }

quit:
  if (print_server_errors() == 0 && (dev_opened || net_opened))
     show_statistics();
  modeS_exit();
  return (0);
}
