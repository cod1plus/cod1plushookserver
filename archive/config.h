/*
 * config.h - cod1plus configuration
 */

#ifndef CONFIG_H
#define CONFIG_H

#define COD1PLUS_VERSION    "1.0.0"
#define COD1PLUS_TAG        "[cod1plus]"

/* Backend HTTP configuration */
#define BACKEND_URL         "http://localhost:3005"
#define STATS_ENDPOINT      "/api/stats"
#define HTTP_TIMEOUT_SECS   10L

/* Full URL for stats POST */
#define STATS_FULL_URL      BACKEND_URL STATS_ENDPOINT

/* Game module name (Linux) */
#define GAME_MODULE_NAME    "game.mp.i386.so"

#endif /* CONFIG_H */
