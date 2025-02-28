/**\file    aircraft.h
 * \ingroup Main
 */
#ifndef _AIRCRAFT_H
#define _AIRCRAFT_H

#include "misc.h"

/**
 * \def AIRCRAFT_DATABASE_CSV
 * Our default aircraft-database relative to `Modes.where_am_I`.
 */
#define AIRCRAFT_DATABASE_CSV   "aircraftDatabase.csv"

/**
 * \def AIRCRAFT_DATABASE_URL
 * The default URL for the `--aircrafts-update` option.
 */
#define AIRCRAFT_DATABASE_URL   "https://opensky-network.org/datasets/metadata/aircraftDatabase.zip"

/**
 * \def AIRCRAFT_DATABASE_TMP
 * The basename for downloading a new `aircraftDatabase.csv`.
 *
 * E.g. Use WinInet API to download:<br>
 *  `AIRCRAFT_DATABASE_URL` -> `%TEMP%\\aircraft-database-temp.zip`
 *
 * extract this using: <br>
 *  `zip_extract (\"%TEMP%\\aircraft-database-temp.zip\", \"%TEMP%\\aircraft-database-temp.csv\")`.
 *
 * and finally call: <br>
 *   `CopyFile ("%TEMP%\\aircraft-database-temp.csv", <final_destination>)`.
 */
#define AIRCRAFT_DATABASE_TMP  "aircraft-database-temp"

/**
 * \enum a_show_t
 * The "show-state" for an aircraft.
 */
typedef enum a_show_t {
        A_SHOW_FIRST_TIME = 1,
        A_SHOW_LAST_TIME,
        A_SHOW_NORMAL,
        A_SHOW_NONE,
      } a_show_t;

/**
 * \typedef aircraft_CSV
 * Describes an aircraft from a .CSV/.SQL-file.
 */
typedef struct aircraft_CSV {
        uint32_t addr;
        char     reg_num [10];
        char     manufact [30];
        char     call_sign [20];
      } aircraft_CSV;

/**
 * \typedef aircraft
 * Structure used to describe an aircraft in interactive mode.
 */
typedef struct aircraft {
        uint32_t  addr;                   /**< ICAO address */
        char      flight [9];             /**< Flight number */
        int       altitude;               /**< Altitude */
        uint32_t  speed;                  /**< Velocity computed from EW and NS components. In Knots */
        int       heading;                /**< Horizontal angle of flight */
        bool      heading_is_valid;       /**< It has a valid heading */
        uint64_t  seen_first;             /**< Tick-time (in milli-sec) at which the first packet was received */
        uint64_t  seen_last;              /**< Tick-time (in milli-sec) at which the last packet was received */
        uint64_t  EST_seen_last;          /**< Tick-time (in milli-sec) at which the last estimated position was done */
        uint32_t  messages;               /**< Number of Mode S messages received */
        int       identity;               /**< 13 bits identity (Squawk) */
        a_show_t  show;                   /**< The plane's show-state */
        double    distance;               /**< Distance (in meters) to home position */
        char      distance_buf [20];      /**< Buffer for `get_home_distance()` */
        double    EST_distance;           /**< Estimated `distance` based on last `speed` and `heading` */
        char      EST_distance_buf [20];  /**< Buffer for `get_est_home_distance()` */
        double    sig_levels [4];         /**< RSSI signal-levels from the last 4 messages */
        int       sig_idx;

        /* Encoded latitude and longitude as extracted by odd and even
         * CPR encoded messages.
         */
        int       odd_CPR_lat;            /**< Encoded odd CPR latitude */
        int       odd_CPR_lon;            /**< Encoded odd CPR longitude */
        int       even_CPR_lat;           /**< Encoded even CPR latitude */
        int       even_CPR_lon;           /**< Encoded even CPR longitude */
        uint64_t  odd_CPR_time;           /**< Tick-time for reception of an odd CPR message */
        uint64_t  even_CPR_time;          /**< Tick-time for reception of an even CPR message */
        pos_t     position;               /**< Coordinates obtained from decoded CPR data */
        pos_t     EST_position;           /**< Estimated position based on last `speed` and `heading` */

        aircraft_CSV       *SQL;          /**< A pointer to a SQL record (or NULL) */
        const aircraft_CSV *CSV;          /**< A pointer to a CSV record in `Modes.aircraft_list_CSV` (or NULL) */
        struct aircraft    *next;         /**< Next aircraft in our linked list */
      } aircraft;


bool        aircraft_CSV_load (void);
bool        aircraft_CSV_update (const char *db_file, const char *url);
aircraft   *aircraft_find_or_create (uint32_t addr, uint64_t now);
int         aircraft_numbers (void);
uint32_t    aircraft_get_addr (uint8_t a0, uint8_t a1, uint8_t a2);
const char *aircraft_get_details (const uint8_t *_a);
const char *aircraft_get_country (uint32_t addr, bool get_short);
bool        aircraft_is_military (uint32_t addr, const char **country);
char       *aircraft_make_json (bool extended_client);
void        aircraft_remove_stale (uint64_t now);
void        aircraft_tests (void);
void        aircraft_exit (bool free_aircrafts);

#endif /* _AIRCRAFT_H */
