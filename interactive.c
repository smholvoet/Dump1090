/**
 * \file    interactive.c
 * \ingroup Misc
 *
 * Function for interactive mode.
 * Using either Windows-Console or Curses functions.
 */
#include <io.h>
#include <conio.h>

#include "interactive.h"
#include "aircraft.h"
#include "airports.h"
#include "sdrplay.h"
#include "misc.h"

#if defined(USE_CURSES)
  #undef MOUSE_MOVED
  #include <curses.h>
#else
  #define chtype WORD
#endif

typedef enum colours {
        COLOUR_DEFAULT = 0,
        COLOUR_WHITE,
        COLOUR_GREEN,
        COLOUR_RED,
        COLOUR_YELLOW,
        COLOUR_MAX,
      } colours;

typedef struct colour_mapping {
        int    pair;    /* Not used in 'wincon_*()' functions */
        chtype attrib;
      } colour_mapping;

typedef struct API_funcs {
        int  (*init) (void);
        void (*exit) (void);
        void (*set_colour) (enum colours colour);
        int  (*clr_scr) (void);
        int  (*clr_eol) (void);
        int  (*gotoxy) (int y, int x);
        int  (*refresh) (int y, int x);
        int  (*print_line) (int y, int x, const char *str);
        void (*print_header) (void);
      } API_funcs;

static int  wincon_init (void);
static void wincon_exit (void);
static void wincon_set_colour (enum colours colour);
static int  wincon_gotoxy (int y, int x);
static int  wincon_clreol (void);
static int  wincon_clrscr (void);
static int  wincon_refresh (int y, int x);
static int  wincon_print_line (int y, int x, const char *str);
static void wincon_print_header (void);

static CONSOLE_SCREEN_BUFFER_INFO  console_info;
static HANDLE                      console_hnd = INVALID_HANDLE_VALUE;
static DWORD                       console_mode = 0;
static colour_mapping              colour_map [COLOUR_MAX];

#if defined(USE_CURSES)
  static int  curses_init (void);
  static void curses_exit (void);
  static void curses_set_colour (enum colours colour);
  static void curses_print_header (void);
  static int  curses_refresh (int y, int x);

  static WINDOW *stats_win  = NULL;
  static WINDOW *flight_win = NULL;

  static API_funcs curses_api = {
         .init         = curses_init,
         .exit         = curses_exit,
         .set_colour   = curses_set_colour,
         .clr_scr      = clear,
         .clr_eol      = clrtoeol,
         .gotoxy       = move,
         .refresh      = curses_refresh,
         .print_line   = mvaddstr,
         .print_header = curses_print_header
        };
#endif

static API_funcs wincon_api = {
       .init         = wincon_init,
       .exit         = wincon_exit,
       .set_colour   = wincon_set_colour,
       .clr_scr      = wincon_clrscr,
       .clr_eol      = wincon_clreol,
       .gotoxy       = wincon_gotoxy,
       .refresh      = wincon_refresh,
       .print_line   = wincon_print_line,
       .print_header = wincon_print_header,
     };

/*
 * List of API function for the TUI (text user interface).
 */
static API_funcs *api = NULL;

/**
 * Do some `assert()` checks first and if Curses interface was
 * selected, set `api = &curses_api`.
 * Otherwise set `api = &wincon_api`.
 */
bool interactive_init (void)
{
  aircraft a;

  assert ((DIM(a.sig_levels) & -(int)DIM(a.sig_levels)) == DIM(a.sig_levels));

  assert (api == NULL);

#ifdef USE_CURSES
  if (Modes.tui_interface == TUI_CURSES)
     api = &curses_api;
  else
#endif
     api = &wincon_api;

  airports_init();

  return ((*api->init)() == 0);
}

void interactive_exit (void)
{
  assert (api != NULL);

  (*api->exit)();
  api = NULL;
}

void interactive_clreol (void)
{
  if (api)
    (*api->clr_eol)();
}

/**
 * Set this aircraft's estimated distance to our home position.
 *
 * Assuming a constant good last heading and speed, calculate the
 * new position from that using the elapsed time.
 */
static void set_est_home_distance (aircraft *a, uint64_t now)
{
  double      heading, distance, gc_distance, cart_distance;
  double      delta_X, delta_Y;
  cartesian_t cpos = { 0.0, 0.0, 0.0 };

  if (!Modes.home_pos_ok || a->speed == 0 || !a->heading_is_valid)
     return;

  if (!VALID_POS(a->EST_position) || a->EST_seen_last < a->seen_last)
     return;

  ASSERT_POS (a->EST_position);

  spherical_to_cartesian (&a->EST_position, &cpos);

  /* Ensure heading is in range '[-Phi .. +Phi]'
   */
  if (a->heading >= 180)
       heading = a->heading - 360;
  else heading = a->heading;

  heading = (TWO_PI * heading) / 360;  /* In radians */

  /* knots (1852 m/s) to distance (in meters) traveled in dT msec:
   */
  distance = 0.001852 * (double)a->speed * (now - a->EST_seen_last);
  a->EST_seen_last = now;

  delta_X = distance * sin (heading);
  delta_Y = distance * cos (heading);
  cpos.c_x += delta_X;
  cpos.c_y += delta_Y;

  cartesian_to_spherical (&cpos, &a->EST_position, heading);
  ASSERT_POS (a->EST_position);

  gc_distance     = great_circle_dist (a->EST_position, Modes.home_pos);
  cart_distance   = cartesian_distance (&cpos, &Modes.home_pos_cart);
  a->EST_distance = closest_to (a->EST_distance, gc_distance, cart_distance);

#if 0
  LOG_FILEONLY ("addr %04X: heading: %+7.1lf, delta_X: %+8.3lf, delta_Y: %+8.3lf, gc_distance: %6.1lf, cart_distance: %6.1lf\n",
                a->addr, 360.0*heading/TWO_PI, delta_X, delta_Y, gc_distance/1000, cart_distance/1000);
#endif
}

/**
 * Return a string showing this aircraft's distance to our home position.
 *
 * If `Modes.metric == true`, return it in kilo-meters. <br>
 * Otherwise Nautical Miles.
 */
static void get_home_distance (aircraft *a, const char **km_nmiles)
{
  double divisor = Modes.metric ? 1000.0 : 1852.0;

  if (km_nmiles)
     *km_nmiles = Modes.metric ? "km" : "Nm";

  if (a->distance > SMALL_VAL)
       snprintf (a->distance_buf, sizeof(a->distance_buf), "%.1lf", a->distance / divisor);
  else a->distance_buf[0] = '\0';
}

/**
 * As for `get_home_distance()`, but return the estimated distance.
 */
static void get_est_home_distance (aircraft *a, const char **km_nmiles)
{
  double divisor = Modes.metric ? 1000.0 : 1852.0;

  if (km_nmiles)
     *km_nmiles = Modes.metric ? "km" : "Nm";

  if (a->EST_distance > SMALL_VAL)
       snprintf (a->EST_distance_buf, sizeof(a->EST_distance_buf), "%.1lf", a->EST_distance / divisor);
  else a->EST_distance_buf[0] = '\0';
}

/*
 * Called every 250 msec (`MODES_INTERACTIVE_REFRESH_TIME`) while
 * in interactive mode to update the Console Windows Title.
 *
 * Called from `background_tasks()` in the main thread.
 */
void interactive_title_stats (void)
{
  #define GAIN_TOO_LOW   " (too low?)"
  #define GAIN_TOO_HIGH  " (too high?)"
  #define GAIN_ERASE     "            "

  char            buf [100];
  char            gain [10];
  static uint64_t last_good_CRC, last_bad_CRC;
  static int      overload_count = 0;
  static char    *overload = GAIN_ERASE;
  uint64_t        good_CRC = Modes.stat.good_CRC + Modes.stat.fixed;
  uint64_t        bad_CRC  = Modes.stat.bad_CRC  - Modes.stat.fixed;

  if (Modes.gain_auto)
       strcpy (gain, "Auto");
  else snprintf (gain, sizeof(gain), "%.1f dB", (double)Modes.gain / 10.0);

  if (overload_count > 0)
  {
    if (--overload_count == 0)
       overload = GAIN_ERASE;
  }
  else if (bad_CRC - last_bad_CRC > 2*(good_CRC - last_good_CRC))
  {
    overload = GAIN_TOO_HIGH;
    overload_count = 4;     /* let it show for 4 period (1 sec) */
  }
#if 0
  else if (bad_CRC == last_bad_CRC && good_CRC == last_good_CRC)
  {
    overload = GAIN_TOO_LOW;
    overload_count = 4;     /* let it show for 4 period (1 sec) */
  }
#endif

  snprintf (buf, sizeof(buf), "Dev: %s. CRC: %llu / %llu / %llu. Gain: %s%s",
            Modes.selected_dev, good_CRC, Modes.stat.fixed, bad_CRC, gain, overload);

  last_good_CRC = good_CRC;
  last_bad_CRC  = bad_CRC;

  SetConsoleTitleA (buf);
}

/*
 * Also called from `background_tasks()` in the main thread.
 * But this function does nothing when `USE_CURSES` is not defined.
 */
void interactive_other_stats (void)
{
  if (Modes.tui_interface != TUI_CURSES)    /* this needs PDCurses; get out */
     return;

#if defined(USE_CURSES)
  if (stats_win)
  {
    /**\todo
     * Fill the `stats_win' with some handy accumulated statistics.
     * Like number of unique planes, CSV/SQL-lookups and cache hits,
     * Number of network clients, bytes etc.
     */
    mvwprintw (stats_win, 20, 0, "HTTP GET:   %llu", Modes.stat.HTTP_get_requests);
    mvwprintw (stats_win, 21, 0, "HTTP bytes: %llu/%llu",
                      Modes.stat.bytes_sent[MODES_NET_SERVICE_HTTP],
                      Modes.stat.bytes_recv[MODES_NET_SERVICE_HTTP]);
  }

  /* Refresh the sub-window for flight-information.
   */
  if (flight_win && Modes.airport_show)
  {
    // \todo
  }
#endif
}

static int gain_increase (int gain_idx)
{
  if (Modes.rtlsdr.device && gain_idx < Modes.rtlsdr.gain_count-1)
  {
    Modes.gain = Modes.rtlsdr.gains [++gain_idx];
    rtlsdr_set_tuner_gain (Modes.rtlsdr.device, Modes.gain);
    LOG_FILEONLY ("Increasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  else if (Modes.sdrplay.device && gain_idx < Modes.sdrplay.gain_count-1)
  {
    Modes.gain = Modes.sdrplay.gains [++gain_idx];
    sdrplay_set_gain (Modes.sdrplay.device, Modes.gain);
    LOG_FILEONLY ("Increasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  return (gain_idx);
}

static int gain_decrease (int gain_idx)
{
  if (Modes.rtlsdr.device && gain_idx > 0)
  {
    Modes.gain = Modes.rtlsdr.gains [--gain_idx];
    rtlsdr_set_tuner_gain (Modes.rtlsdr.device, Modes.gain);
    LOG_FILEONLY ("Decreasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  else if (Modes.sdrplay.device && gain_idx > 0)
  {
    Modes.gain = Modes.sdrplay.gains [--gain_idx];
    sdrplay_set_gain (Modes.sdrplay.device, Modes.gain);
    LOG_FILEONLY ("Decreasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  return (gain_idx);
}

/**
 * Poll for '+/-' keypresses and adjust the RTLSDR / SDRplay gain accordingly.
 * But within the min/max gain settings for the device.
 */
void interactive_update_gain (void)
{
  static int gain_idx = -1;
  int    i, ch;

  if (gain_idx == -1)
  {
    for (i = 0; i < Modes.rtlsdr.gain_count; i++)
        if (Modes.gain == Modes.rtlsdr.gains[i])
        {
          gain_idx = i;
          break;
        }
    if (Modes.sdrplay.device)
       gain_idx = Modes.sdrplay.gain_count / 2;
  }

  if (!_kbhit())
     return;

  ch = _getch();

  /* If we have auto-gain enabled, switch to manual gain
   * on a '-' or '+' keypress. Start with the middle gain-value.
   */
  if (Modes.gain_auto && (ch == '-' || ch == '+'))
  {
    LOG_FILEONLY ("Gain: AUTO -> manual.\n");
    Modes.gain_auto = false;
    if (Modes.rtlsdr.device)
    {
      rtlsdr_set_tuner_gain_mode (Modes.rtlsdr.device, 1);
      gain_idx = Modes.rtlsdr.gain_count / 2;
    }
    else if (Modes.sdrplay.device)
    {
      sdrplay_set_gain (Modes.sdrplay.device, 0);
      gain_idx = Modes.sdrplay.gain_count / 2;
    }
  }

  if (ch == '+')
     gain_idx = gain_increase (gain_idx);
  else if (ch == '-')
     gain_idx = gain_decrease (gain_idx);
#if 0
  else if (ch == 'g' || ch == 'G')
  {
    if (Modes.gain_auto)
    {
      gain_manual();
      LOG_FILEONLY ("Gain: AUTO -> manual.\n");
    }
    else
    {
      gain_auto();
      LOG_FILEONLY ("Gain: manual -> AUTO.\n");
    }
  }
#endif
}

/**
 * Show information for a single aircraft.
 *
 * If `a->show == A_SHOW_FIRST_TIME`, print in GREEN colour.
 * If `a->show == A_SHOW_LAST_TIME`, print in RED colour.
 *
 * \param in a    the aircraft to show.
 * \param in now  the currect tick-timer in milli-seconds.
 */
static void interactive_show_aircraft (aircraft *a, int row, uint64_t now)
{
  int   altitude          = a->altitude;
  int   speed             = a->speed;
  char  alt_buf [10]      = "  - ";
  char  lat_buf [10]      = "   - ";
  char  lon_buf [10]      = "    - ";
  char  speed_buf [8]     = " - ";
  char  heading_buf [8]   = " - ";
  char  distance_buf [10] = " - ";
  char  RSSI_buf [7]      = " - ";
  char  line_buf [120];
  bool  restore_colour    = false;
  const char *reg_num     = "";
  const char *call_sign   = "";
  const char *flight      = "";
  const char *km_nmiles   = NULL;
  const char *cc_short;
  double  sig_avg = 0;
  int64_t ms_diff;

  /* Convert units to metric if --metric was specified.
   */
  if (Modes.metric)
  {
    altitude = (int) round ((double)altitude / 3.2828);
    speed    = (int) round ((double)speed * 1.852);
  }

  /* Get the average RSSI from last 4 messages.
   */
  for (uint8_t i = 0; i < DIM(a->sig_levels); i++)
      sig_avg += a->sig_levels[i];
  sig_avg /= DIM(a->sig_levels);

  if (sig_avg > 1E-5)
     snprintf (RSSI_buf, sizeof(RSSI_buf), "% +4.1lf", 10 * log10(sig_avg));

  if (altitude)
     snprintf (alt_buf, sizeof(alt_buf), "%5d", altitude);

  if (a->position.lat != 0.0)
     snprintf (lat_buf, sizeof(lat_buf), "% +7.03f", a->position.lat);

  if (a->position.lon != 0.0)
     snprintf (lon_buf, sizeof(lon_buf), "% +8.03f", a->position.lon);

  if (speed)
     snprintf (speed_buf, sizeof(speed_buf), "%4d", speed);

  if (a->heading_is_valid)
     snprintf (heading_buf, sizeof(heading_buf), "%3d", a->heading);

  if (Modes.home_pos_ok)
  {
    get_home_distance (a, &km_nmiles);
    get_est_home_distance (a, &km_nmiles);
    if (a->EST_distance_buf[0])
       strncpy (distance_buf, a->EST_distance_buf, sizeof(distance_buf)-1);
  }

  if (a->SQL)
  {
    if (a->SQL->reg_num[0])
       reg_num = a->SQL->reg_num;
  }
  else if (a->CSV)
  {
    if (a->CSV->reg_num[0])
       reg_num = a->CSV->reg_num;
  }

  if (!a->flight[0] && call_sign[0])
       flight = call_sign;
  else flight = a->flight;

  if (a->show == A_SHOW_FIRST_TIME)
  {
    (*api->set_colour) (COLOUR_GREEN);
    restore_colour = true;
    LOG_FILEONLY ("plane '%06X' entering.\n", a->addr);
  }
  else if (a->show == A_SHOW_LAST_TIME)
  {
    char alt_buf2 [10] = "-";

    if (altitude >= 1)
       _itoa (altitude, alt_buf2, 10);

    (*api->set_colour) (COLOUR_RED);
    restore_colour = true;

    LOG_FILEONLY ("plane '%06X' leaving. Active for %.1lf sec. Altitude: %s m, Distance: %s/%s %s.\n",
                  a->addr, (double)(now - a->seen_first) / 1000.0,
                  alt_buf2,
                  a->distance_buf[0]     ? a->distance_buf     : "-",
                  a->EST_distance_buf[0] ? a->EST_distance_buf : "-", km_nmiles);
  }

  ms_diff = (now - a->seen_last);
  if (ms_diff < 0LL)  /* clock wrapped */
     ms_diff = 0L;

  cc_short = aircraft_get_country (a->addr, true);
  if (!cc_short)
     cc_short = "--";

  snprintf (line_buf, sizeof(line_buf),
            "%06X %-9.9s %-8s %-6s %-5s     %-5s %-7s %-8s %5s   %5.5s  %5s %5u  %2llu sec ",
            a->addr, flight, reg_num, cc_short, alt_buf, speed_buf, lat_buf, lon_buf, heading_buf,
            distance_buf, RSSI_buf, a->messages, ms_diff / 1000);

  (*api->print_line) (row, 0, line_buf);

  if (restore_colour)
     (*api->set_colour) (0);
}

/**
 * Show the currently captured aircraft information on screen.
 * \param in now  the currect tick-timer in mill-seconds
 */
void interactive_show_data (uint64_t now)
{
  static int old_count = -1;
  int        row = 2, count = 0;
  aircraft  *a = Modes.aircrafts;

  /*
   * Unless debug or raw-mode is active, clear the screen to remove old info.
   * But only if current number of aircrafts is less than last time. This is to
   * avoid an annoying blinking of the console.
   */
  if (Modes.debug == 0)
  {
    if (old_count == -1 || aircraft_numbers() < old_count)
      (*api->clr_scr)();
    (*api->gotoxy) (0, 0);
  }

  (*api->print_header)();

  while (a && count < Modes.interactive_rows && !Modes.exit)
  {
    if (a->show != A_SHOW_NONE)
    {
      set_est_home_distance (a, now);
      interactive_show_aircraft (a, row, now);
      row++;
    }

    /* Simple state-machine for the plane's show-state
     */
    if (a->show == A_SHOW_FIRST_TIME)
       a->show = A_SHOW_NORMAL;
    else if (a->show == A_SHOW_LAST_TIME)
       a->show = A_SHOW_NONE;      /* don't show again before deleting it */

    a = a->next;
    count++;
  }

  (*api->refresh) (row, 0);

  old_count = count;
}

/**
 * Receive new messages and populate the interactive mode with more info.
 */
aircraft *interactive_receive_data (const modeS_message *mm, uint64_t now)
{
  aircraft *a;
  char     *p;
  uint32_t  addr;

  if (!mm->CRC_ok)
     return (NULL);

  /* Loookup our aircraft or create a new one.
   */
  addr = aircraft_get_addr (mm->AA[0], mm->AA[1], mm->AA[2]);
  a = aircraft_find_or_create (addr, now);
  if (!a)
     return (NULL);

  a->seen_last = now;
  a->messages++;

  a->sig_levels [a->sig_idx++] = mm->sig_level;
  a->sig_idx &= DIM(a->sig_levels) - 1;

  if (mm->msg_type == 5 || mm->msg_type == 21)
  {
    if (mm->identity)
         a->identity = mm->identity;   /* Set thee Squawk code. */
    else a->identity = 0;
  }

  if (mm->msg_type == 0 || mm->msg_type == 4 || mm->msg_type == 20)
  {
    a->altitude = mm->altitude;
  }
  else if (mm->msg_type == 17)
  {
    if (mm->ME_type >= 1 && mm->ME_type <= 4)
    {
      memcpy (a->flight, mm->flight, sizeof(a->flight));
      p = a->flight + sizeof(a->flight)-1;
      while (*p == ' ')
        *p-- = '\0';  /* Remove trailing spaces */

    }
    else if ((mm->ME_type >= 9  && mm->ME_type <= 18) || /* Airborne Position (Baro Altitude) */
             (mm->ME_type >= 20 && mm->ME_type <= 22))   /* Airborne Position (GNSS Height) */
    {
      a->altitude = mm->altitude;
      if (mm->odd_flag)
      {
        a->odd_CPR_lat  = mm->raw_latitude;
        a->odd_CPR_lon  = mm->raw_longitude;
        a->odd_CPR_time = now;
      }
      else
      {
        a->even_CPR_lat  = mm->raw_latitude;
        a->even_CPR_lon  = mm->raw_longitude;
        a->even_CPR_time = now;
      }

      /* If the two reports are less than 10 minutes apart, compute the position.
       * This used to be '10 sec', but I used some code from:
       *   https://github.com/Mictronics/readsb/blob/master/track.c
       *
       * which says:
       *   A wrong relative position decode would require the aircraft to
       *   travel 360-100=260 NM in the 10 minutes of position validity.
       *   This is impossible for planes slower than 1560 knots (Mach 2.3) over the ground.
       */
      int64_t t_diff = (int64_t) (a->even_CPR_time - a->odd_CPR_time);

      if (llabs(t_diff) <= 60*10*1000)
           decode_CPR (a);
   /* else LOG_FILEONLY ("t_diff for '%04X' too large: %lld sec.\n", a->addr, t_diff/1000); */
    }
    else if (mm->ME_type == 19)
    {
      if (mm->ME_subtype == 1 || mm->ME_subtype == 2)
      {
        a->speed   = mm->velocity;
        a->heading = mm->heading;
        a->heading_is_valid = mm->heading_is_valid;
      }
    }
  }
  return (a);
}

/**
 * "Windows Console" API functions
 */
static int wincon_init (void)
{
  console_hnd = GetStdHandle (STD_OUTPUT_HANDLE);
  if (console_hnd == INVALID_HANDLE_VALUE)
     return (-1);

  if (_isatty(1) == 0)
  {
    LOG_STDERR ("Do not redirect 'stdout' in interactive mode.\n"
                "Do '%s [options] 2> file` instead.\n", Modes.who_am_I);
    return (-1);
  }

  GetConsoleScreenBufferInfo (console_hnd, &console_info);
  GetConsoleMode (console_hnd, &console_mode);
  if (console_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
     SetConsoleMode (console_hnd, console_mode | DISABLE_NEWLINE_AUTO_RETURN);

  DWORD new_mode = console_mode & ~(ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT);
  SetConsoleMode (console_hnd, new_mode);

  Modes.interactive_rows = console_info.srWindow.Bottom - console_info.srWindow.Top - 1;
  api = &wincon_api;

  colour_map [COLOUR_DEFAULT].attrib = console_info.wAttributes;              /* default colour */
  colour_map [COLOUR_WHITE  ].attrib = (console_info.wAttributes & ~7) | 15;  /* bright white */
  colour_map [COLOUR_GREEN  ].attrib = (console_info.wAttributes & ~7) | 10;  /* bright green */
  colour_map [COLOUR_RED    ].attrib = (console_info.wAttributes & ~7) | 12;  /* bright red */
  colour_map [COLOUR_YELLOW ].attrib = (console_info.wAttributes & ~7) | 14;  /* bright yellow */
  return (0);
}

static void wincon_exit (void)
{
  (*api->gotoxy) (Modes.interactive_rows-1, 0);
  (*api->set_colour) (0);
  if (console_hnd != INVALID_HANDLE_VALUE)
     SetConsoleMode (console_hnd, console_mode);
  console_hnd = INVALID_HANDLE_VALUE;
}

static int wincon_gotoxy (int y, int x)
{
  COORD coord;

  if (console_hnd == INVALID_HANDLE_VALUE)
     return (-1);

  coord.X = x + console_info.srWindow.Left;
  coord.Y = y + console_info.srWindow.Top;
  SetConsoleCursorPosition (console_hnd, coord);
  return (0);
}

static int wincon_clrscr (void)
{
  WORD width = console_info.srWindow.Right - console_info.srWindow.Left + 1;
  WORD y = console_info.srWindow.Top;

  while (y <= console_info.srWindow.Bottom)
  {
    DWORD written;
    COORD coord = { console_info.srWindow.Left, y++ };

    FillConsoleOutputCharacter (console_hnd, ' ', width, coord, &written);
    FillConsoleOutputAttribute (console_hnd, console_info.wAttributes, width, coord, &written);
  }
  return (0);
}

/*
 * Fill the current line with spaces and put the cursor back at position 0.
 */
static int wincon_clreol (void)
{
#if 0
  if (console_hnd != INVALID_HANDLE_VALUE)
  {
    WORD   width = console_info.srWindow.Right - console_info.srWindow.Left + 1;
    char *filler = alloca (width+1);

    memset (filler, ' ', width-3);
    filler [width-2] = '\r';
    filler [width-1] = '\0';
    fputs (filler, stdout);
  }
#endif
  return (0);
}

static void wincon_set_colour (enum colours colour)
{
  assert (colour >= 0);
  assert (colour < COLOUR_MAX);
  if (console_hnd != INVALID_HANDLE_VALUE)
     SetConsoleTextAttribute (console_hnd, (WORD)colour_map [colour].attrib);
}

static int wincon_refresh (int y, int x)
{
  /* Nothing to do here */
  (void) x;
  (void) y;
  return (0);
}

static int wincon_print_line (int y, int x, const char *str)
{
#if 1
  puts (str);
#else
  WriteConsoleA (console_hnd, str, strlen(str), NULL, NULL);
#endif

  (void) x; /* Cursor already set */
  (void) y;
  return (0);
}

static int  spin_idx = 0;
static char spinner[] = "|/-\\";

#define HEADER  "ICAO   Callsign  Reg-num  Cntry  Altitude  Speed   Lat      Long    Hdg    Dist   RSSI   Msg  Seen %c"

static void wincon_print_header (void)
{
  (*api->set_colour) (COLOUR_WHITE);
  printf (HEADER "\n", spinner[spin_idx & 3]);
  (*api->set_colour) (0);

#if 1
  puts ("-----------------------------------------------------------------------------------------------------");
#else
  WORD  width = strlen(HEADER) - 1;
  WORD  attr = console_info.wAttributes | COMMON_LVB_GRID_HORIZONTAL;
  DWORD written;
  COORD coord = { .X = console_info.srWindow.Left,
                  .Y = console_info.srWindow.Top + 2
                };

  attr |= 15;  /* bright white */
  FillConsoleOutputAttribute (console_hnd, attr, width, coord, &written);
#endif

  spin_idx++;
}

/**
 * PDCurses API functions
 */
#if defined(USE_CURSES)
static int curses_init (void)
{
  initscr();
  Modes.interactive_rows = getmaxy (stdscr);
  if (Modes.interactive_rows == 0)
     return (-1);

  if (has_colors())
     start_color();

  use_default_colors();
  if (!can_change_color())
     return (-1);

  init_pair (COLOUR_WHITE,  COLOR_WHITE, COLOR_BLUE);
  init_pair (COLOUR_GREEN,  COLOR_GREEN, COLOR_BLUE);
  init_pair (COLOUR_RED,    COLOR_RED, COLOR_BLUE);
  init_pair (COLOUR_YELLOW, COLOR_YELLOW, COLOR_GREEN);

  colour_map [COLOUR_DEFAULT].pair = 0;  colour_map [COLOUR_DEFAULT].attrib = A_NORMAL;
  colour_map [COLOUR_WHITE  ].pair = 1;  colour_map [COLOUR_WHITE  ].attrib = A_BOLD;
  colour_map [COLOUR_GREEN  ].pair = 2;  colour_map [COLOUR_GREEN  ].attrib = A_BOLD;
  colour_map [COLOUR_RED    ].pair = 3;  colour_map [COLOUR_RED    ].attrib = A_BOLD;
  colour_map [COLOUR_YELLOW ].pair = 4;  colour_map [COLOUR_YELLOW ].attrib = A_NORMAL;

  noecho();
  curs_set (0);
  mousemask (0, NULL);
  clear();
  refresh();

  stats_win = subwin (stdscr, 4, COLS, 0, 0);
  wattron (stats_win, A_REVERSE);
  LOG_FILEONLY ("stats_win: %p, SP->lines: %u, SP->cols: %u\n", stats_win, SP->lines, SP->cols);

#if 0 // \todo
  flight_win = subwin (stdscr, 4, COLS, 0, 0);
  wattron (flight_win, A_REVERSE);
  LOG_FILEONLY ("flight_win: %p, SP->lines: %u, SP->cols: %u\n", flight_win, SP->lines, SP->cols);
#endif

  slk_init (1);
  slk_set (1, "Help", 0);
  slk_set (2, "Quit", 0);
  slk_attron (A_REVERSE);

  return (0);
}

static void curses_exit (void)
{
  delwin (stats_win);
  endwin();
  delscreen (SP);  /* PDCurses does not free this memory */
}

static void curses_set_colour (enum colours colour)
{
  int pair, attrib;

  assert (colour >= 0);
  assert (colour < COLOUR_MAX);

  pair = colour_map [colour].pair;
  assert (pair < COLOR_PAIRS);

  attrib = colour_map [colour].attrib;
  assert (attrib == A_NORMAL || attrib == A_BOLD);
  attrset (COLOR_PAIR(pair) | attrib);
}

static int curses_refresh (int y, int x)
{
  wrefresh (stats_win);
  move (y, x);
//clrtobot();
  refresh();
  return (0);
}

static void curses_print_header (void)
{
  (*api->set_colour) (COLOUR_WHITE);
  mvprintw (0, 0, HEADER, spinner[spin_idx & 3]);
  spin_idx++;

  (*api->set_colour) (0);
  mvhline (1, 0, ACS_HLINE, strlen(HEADER)-1);
}
#endif  /* USE_CURSES */

