/**\file    sntp.c
 * \ingroup Misc
 *
 * Simple Network Time Protocol functions for Dump1090.
 */
#include "net_io.h"
#include "sntp.h"

#if defined(SNTP_TEST_IN_MAINLOOP)
  #define SNTP_TRACE(fmt, ...)  do {                                 \
	                          if (test_mode)                     \
	                             printf ("  " fmt, __VA_ARGS__); \
                                } while (0)
#else
  #define SNTP_TRACE(fmt, ...)  ((void)0)
#endif

static const char *test_servers[] = {
                  "time.windows.com",
                  "time.apple.com",
                  "ntp.cesnet.cz",
                  "ntps1-2.uni-erlangen.de",
                  "time.service.uit.no",
                  "ntp1.uio.no",
                  "ntp2.uio.no",
                  NULL,                       /* tests the `SNTP_DEFAULT_SERVER` */
                  "ntp.ise.canberra.edu.au",  /* Fails on DNS lookup */
                  "fartein.ifi.uio.no"        /* Not publicly accessible */
                };

static int     sntp_idx = -1;
static int64_t sntp_msec     [DIM(test_servers)];
static int64_t sntp_diff     [DIM(test_servers)];
static bool    sntp_error    [DIM(test_servers)];
static bool    sntp_resolved [DIM(test_servers)];
static bool    test_mode = false;

void sntp_init (bool testing)
{
  test_mode = testing;
  Modes.sntp_enable   = true;
  Modes.sntp_log_file = true;
}

void sntp_exit (void)
{
  Modes.sntp_enable   = false;
  Modes.sntp_log_file = false;
}

uint64_t sntp_time (int idx)
{
  assert (idx >= 0);
  assert (idx < DIM(sntp_msec));

  if (sntp_msec[idx] > 0)
     return (sntp_msec[idx]);     /* we got a result */
  return (0ULL);
}

void sntp_send (int idx, mg_connection *c)
{
  const connection *conn = Modes.connections [MODES_NET_SERVICE_SNTP];

  assert (idx >= 0);
  assert (idx < DIM(sntp_resolved));
  assert (sntp_resolved[idx]);        /* cannot send before it was resolved */

  printf ("  Sending SNTP-%d request: %s\n", idx, conn ? conn->rem_buf : "?");


  mg_sntp_request (c);
}

bool sntp_handler (int idx, mg_connection *c)
{
  bool    rc = false;
  int64_t now, msec = mg_sntp_parse (c->recv.buf, c->recv.len);

  if (msec > 0)
  {
    /* 's_boot_timestamp' is now accessible via 'mg_now()'
     */
    now = MSEC_TIME();
    sntp_diff [idx] = now > msec ? now - msec : msec - now;
    sntp_msec [idx] = msec;
    printf ("  diff_msec[%d]: %lld msec\n", idx, sntp_diff[idx]);
    c->is_closing = 1;
    net_timer_del (MODES_NET_SERVICE_SNTP);
    rc = true;
  }
  SNTP_TRACE ("%s() for SNTP server idx: %d -> rc: %d\n", __FUNCTION__, idx, rc);
  return (rc);
}

int sntp_close (int idx, mg_connection *c, int event)
{
  intptr_t          service = MODES_NET_SERVICE_SNTP;
  const connection *conn = Modes.connections [service];
  net_service      *ns   = &modeS_net_services [service];

  (void) c;

  SNTP_TRACE ("%s for SNTP server idx: %d\n", net_ev_name(event), idx);

  if (conn)
     net_timer_del (service);

  free (ns->url);
  ns->url = NULL;

  if (event == MG_EV_ERROR)
  {
    sntp_error [idx] = true;
  }
  else if (event == MG_EV_SNTP_TIMEOUT)
  {
    mg_call (Modes.sntp_in, MG_EV_SNTP_TIMEOUT, (void*)service);
  }

  Modes.sntp_in = NULL;
  idx++;
  if (idx >= DIM(test_servers))
     idx = 0;
  return (idx);
}

int sntp_timeout (int idx)
{
  intptr_t service = MODES_NET_SERVICE_SNTP;

  SNTP_TRACE ("MG_EV_SNTP_TIMEOUT for SNTP server idx: %d\n", idx);
  mg_call (Modes.sntp_in, MG_EV_SNTP_TIMEOUT, (void*)service);
  idx++;
  if (idx >= DIM(test_servers))
     idx = 0;
  return (idx);
}

static void _sntp_timeout (void *arg)
{
  intptr_t service = (intptr_t) arg;

  assert (service == MODES_NET_SERVICE_SNTP);
  printf ("  SNTP timeout for '%s'\n", modeS_net_services[service].url);
  sntp_close (sntp_idx, Modes.sntp_in, MG_EV_SNTP_TIMEOUT);
}

void sntp_resolve (int idx, bool resolved)
{
  SNTP_TRACE ("MG_EV_RESOLVE for SNTP server idx: %d\n", idx);
  sntp_resolved [idx] = resolved;
}

bool sntp_poll (int idx)
{
  SNTP_TRACE ("MG_EV_POLL for SNTP server idx: %d\n", idx);

  return (sntp_msec[idx] != 0 ||     /* we got a positive result for current server index */
          Modes.sntp_in == NULL);    /* due to `MG_EV_ERROR` or `MG_EV_CLOSE` */
}

static bool sntp_test_server (int idx, const char *host)
{
  intptr_t     service = MODES_NET_SERVICE_SNTP;
  const char  *def_host = "";
  char        *url;
  net_service *ns;

  sntp_resolved [idx] = false;

  if (!host)
  {
    host = SNTP_DEFAULT_SERVER;
    def_host = " (default)";
  }

  ns = &modeS_net_services [service];
  strncpy (ns->host, host, sizeof(ns->host));

  url = mg_mprintf ("udp://%s:%u", ns->host, ns->port);
  ns->url         = url;
  ns->active_send = false;
  ns->is_udp      = true;
  ns->is_ip6      = false;

  printf ("\n  Connecting to SNTP-%d: %s %s\n", idx, url, def_host);

  net_timer_add (service, SNTP_TIMEOUT, MG_TIMER_ONCE, _sntp_timeout);
  Modes.sntp_in = mg_connect (&Modes.mgr, url, net_ev_handler, (void*)service);

#if !defined(SNTP_TEST_IN_MAINLOOP)
  while (!Modes.exit)
  {
    if (sntp_poll(idx))
       break;
    net_poll();
  }
#endif

  return (sntp_msec[idx] > 0);
}

/*
 * Test all SNTP servers in blocking mode.
 */
static int sntp_test_blocking (void)
{
  int i, rc, save = Modes.silent;

  Modes.silent   = 1;
  Modes.no_stats = 1;

  for (i = rc = 0; i < DIM(test_servers); i++)
  {
    sntp_msec [i] = 0ULL;
    sntp_idx = i;

    if (sntp_test_server(i, test_servers[i]))
    {
      rc++;
      puts ("  Got response");
    }
    else if (!sntp_resolved[i])
    {
      puts ("  No DNS result");
//    free ();
    }
    else
    {
      puts ("  No response");
    }
  }
  printf ("\n  Got %d/%d good replies.\n", rc, (int)DIM(test_servers));
  Modes.silent = save;
  return (i);
}

int sntp_test (int idx)
{
  printf ("\n%s():\n", __FUNCTION__);

  if (!Modes.sntp_enable)
  {
    printf ("  Modes.sntp_enable == false!\n");
    return (-1);
  }

  if (idx == -1)
     return sntp_test_blocking();

  /* test only 1 SNTP server in non-blocking mode
   */
  if (idx >= DIM(test_servers))
     idx = 0;
  sntp_test_server (idx, test_servers[idx]);
  return (idx);
}

