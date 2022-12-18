/**
 * \file    location.c
 * \ingroup Misc
 * \brief   Simple asynchronous interface for the Windows Location API.
 *          It's sole purpose is to get a latitude and longitude.
 *
 * Rewritten from 2 Windows Classic Samples:
 *   https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/LocationAwarenessEvents/cpp/
 *   https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/LocationSynchronousAccess/cpp/
 *
 * And an article on "COM Programming in Plain C":
 *   https://www.codeproject.com/Articles/13601/COM-in-plain-C
 *
 * The OnLocationChanged() callback gets triggered once a minute.
 * The GPSDirect driver in Simulation mode can be used for more rapid changes.
 */
#define CINTERFACE
#define COBJMACROS

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <locationapi.h>

#include "misc.h"
#include "location.h"

#define MODES_LOCATION_TIMEOUT  2000   /* msec timeout for an active connect */

#define TRACE(fmt, ...)  do {                            \
                           printf ("%s(%u): " fmt ".\n", \
                                   __FILE__, __LINE__,   \
                                   __VA_ARGS__);         \
                         } while (0)

#define LOC_API static HRESULT __stdcall

typedef struct ILocationEvents2 {
        struct ILocationEvents2Vtbl *lpVtbl;
        ULONG                        ref_count;
      } ILocationEvents2;

typedef struct ILocationEvents2Vtbl {
        HRESULT (__stdcall *QueryInterface) (ILocationEvents2 *self, const IID *iid, void **obj);
        ULONG   (__stdcall *AddRef)  (ILocationEvents2 *self);
        ULONG   (__stdcall *Release) (ILocationEvents2 *self);
        HRESULT (__stdcall *OnLocationChanged) (ILocationEvents2 *self, const IID *report_type, struct ILocationReport *location_report);
        HRESULT (__stdcall *OnStatusChanged)   (ILocationEvents2 *self, const IID *report_type, LOCATION_REPORT_STATUS new_status);
      } ILocationEvents2Vtbl;


static struct pos_t g_pos;
static int          g_got_pos;
static mg_timer    *g_timer;

static struct ILocation       *g_location;
static struct ILocationEvents *g_location_events;

LOC_API OnLocationChanged (ILocationEvents2  *self, const IID *report_type, struct ILocationReport *location_report);
LOC_API OnStatusChanged   (ILocationEvents2  *self, const IID *report_type, LOCATION_REPORT_STATUS new_status);

static ULONG __stdcall AddRef (ILocationEvents2 *self)
{
  InterlockedIncrement ((volatile long*)&self->ref_count);
  TRACE ("%s() called, ref_count: %lu", __FUNCTION__, self->ref_count);
  return (self->ref_count);
}

static ULONG __stdcall Release (ILocationEvents2 *self)
{
  InterlockedDecrement ((volatile long*)&self->ref_count);
  TRACE ("%s() called, ref_count: %lu", __FUNCTION__, self->ref_count);
  if (self->ref_count == 0)
  {
    memset (self, '\0', sizeof(*self));
    free (self);
    return (0);
  }
  return (self->ref_count);
}

LOC_API QueryInterface (ILocationEvents2 *self, const IID *iid, void **obj)
{
  if (IsEqualIID(iid, &IID_IUnknown))
  {
    *obj = self;
    TRACE ("%s() called, iid 'IID_IUnknown'", __FUNCTION__);
  }
  else if (IsEqualIID(iid, &IID_ILocationEvents))
  {
    *obj = self;
    TRACE ("%s() called, iid 'IID_ILocationEvents'", __FUNCTION__);
  }
  else
  {
    TRACE ("%s() called -> E_NOINTERFACE", __FUNCTION__);
    *obj = NULL;
    return (E_NOINTERFACE);
  }
  self->lpVtbl->AddRef (self);
  return (S_OK);
}

bool location_init (void)
{
  HRESULT hr = CoInitializeEx (NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);

  if (!SUCCEEDED(hr))
  {
    TRACE ("CoInitializeEx() failed: %s", win_strerror(GetLastError()));
    CoUninitialize();
    return (false);
  }

  g_location = NULL;
  CoCreateInstance (&CLSID_Location, NULL, CLSCTX_INPROC_SERVER, &IID_ILocation, (void**)&g_location);
  TRACE ("g_location: 0x%p", g_location);
  if (!g_location)
  {
    TRACE ("CoCreateInstance() failed: %s", win_strerror(GetLastError()));
    CoUninitialize();
    return (false);
  }

  /* Request permissions for this user account to receive location data
   * for the '&IID_ILatLongReport'
   */
  hr = (*g_location->lpVtbl->RequestPermissions) (g_location,
                                                  NULL,
                                                  (IID*)&IID_ILatLongReport, 1,
                                                  FALSE);

  TRACE ("Location::RequestPermissions() -> hr: %lu", hr);
  if (!SUCCEEDED(hr))
  {
    TRACE ("RequestPermissions() failed: %s.\n"
           "Fix your Windows settings to allow applications to access you location.\n"
           "Ref: 'Start | Settings | Privacy | Location'", win_strerror(hr));
    CoUninitialize();
    return (false);
  }

  /* Tell the Location API that we want to register for report
   */
  hr = (*g_location->lpVtbl->RegisterForReport) (g_location, g_location_events, &IID_ILatLongReport, 0);
  if (!SUCCEEDED(hr))
  {
    TRACE ("Location::RegisterForReport() failed; %s", win_strerror(hr));
    CoUninitialize();
    return (false);
  }
  return (true);
}

/**
 * Unregister reports from the Location API
 */
void location_exit (void)
{
  if (g_location && g_location->lpVtbl)
  {
    HRESULT hr = (*g_location->lpVtbl->UnregisterForReport) (g_location, &IID_ILatLongReport);

    TRACE ("Location::UnregisterForReport(); hr: %lu", hr);
  }
  if (g_timer)
  {
    mg_timer_free (&Modes.mgr.timers, g_timer);
    g_timer = NULL;

    free (g_location_events);
    g_location_events = NULL;
    CoUninitialize();
  }
}

/**
 * The timer callback for the Location API.
 *
 * Wait until timeout occured. During this time the Location API
 * will send reports to our callback interface on another thread.
 */
static void location_timer (void *fn_data)
{
  pos_t pos = *(pos_t*) fn_data;

  TRACE ("%s() called", __FUNCTION__);

  if (VALID_POS(pos))
  {
    DEBUG (DEBUG_GENERAL, "Got position from Location API: %.3f,%3f.\n", pos.lat, pos.lon);
    g_got_pos = 1;
  }
  else
  {
    DEBUG (DEBUG_GENERAL, "Timeout in Location API\n");
    g_got_pos = 0;
  }
}

bool location_poll (void)
{
  TRACE ("g_got_pos: %d", g_got_pos);
  return (g_got_pos == 1);
}

bool location_get_async (void)
{
  struct ILocationEvents2Vtbl *ev2vtbl;
  struct ILocationEvents2     *ev2 = calloc (sizeof(*ev2) + sizeof(*ev2vtbl), 1);

  if (!ev2)
     return (false);

  ev2vtbl = (struct ILocationEvents2Vtbl*) (ev2 + 1);
  ev2vtbl->AddRef            = AddRef;
  ev2vtbl->Release           = Release;
  ev2vtbl->QueryInterface    = QueryInterface;
  ev2vtbl->OnLocationChanged = OnLocationChanged;
  ev2vtbl->OnStatusChanged   = OnStatusChanged;
  g_location_events          = (struct ILocationEvents*) ev2;
  g_location_events->lpVtbl  = (struct ILocationEventsVtbl*) ev2vtbl;
  g_timer = mg_timer_add (&Modes.mgr, MODES_LOCATION_TIMEOUT, MG_TIMER_REPEAT, location_timer, &g_pos);
  g_got_pos = -1;

  return location_init();
}

/**
 * This is called when there is a new location report
 * Get the ILatLongReport interface from ILocationReport
 */
LOC_API OnLocationChanged (ILocationEvents2 *self, const IID *report_type, struct ILocationReport *location_report)
{
  HRESULT hr;

  if (!IsEqualCLSID(report_type, &IID_ILatLongReport))
     return (S_OK);

  ILatLongReport *lat_long_report = NULL;
  hr = (*location_report->lpVtbl->QueryInterface) (location_report, &IID_ILatLongReport, (void**)&lat_long_report);

  TRACE ("LocationEvents::QueryInterface(): hr: %lu", hr);

  if (!SUCCEEDED(hr) || !lat_long_report || !lat_long_report->lpVtbl)
  {
    TRACE ("LocationEvents::QueryInterface() failed: %s", win_strerror(GetLastError()));
    return (S_OK);   /* or quit the async loop by stopping 'g_timer'? */
  }

  hr = (*lat_long_report->lpVtbl->GetLatitude) (lat_long_report, &g_pos.lat);
  if (SUCCEEDED(hr))
       TRACE ("Latitude: %12.6f\n", g_pos.lat);
  else TRACE ("Latitude: Not available: %s.\n", win_strerror(hr));

  hr = (*lat_long_report->lpVtbl->GetLongitude) (lat_long_report, &g_pos.lon);
  if (SUCCEEDED(hr))
       TRACE ("Longitude: %12.6f\n", g_pos.lon);
  else TRACE ("Longitude: Not available: %s.\n", win_strerror(hr));

  (void) self;
  return (S_OK);
}

/**
 * This is called when the status of a report type changes.
 */
LOC_API OnStatusChanged (ILocationEvents2 *self, const IID *report_type, LOCATION_REPORT_STATUS new_status)
{
  if (IsEqualCLSID(report_type, &IID_ILatLongReport))
  {
    switch (new_status)
    {
      case REPORT_NOT_SUPPORTED:
           TRACE ("No devices detected");
           break;
      case REPORT_ERROR:
           TRACE ("Report error");
           break;
      case REPORT_ACCESS_DENIED:
           TRACE ("Access denied: %s", win_strerror(GetLastError()));
           break;
      case REPORT_INITIALIZING:
           TRACE ("Report is initializing");
           break;
      case REPORT_RUNNING:
           TRACE ("Running");
           break;
    }
  }
  (void) self;
  return (S_OK);
}


