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

typedef struct ILocationEvents2 {
        struct ILocationEvents2Vtbl *lpVtbl;
        ULONG                        ref_count;
      } ILocationEvents2;

typedef struct ILocationEvents2Vtbl {
        /*
         * The order of these func-pointers matters a great deal.
         */
        HRESULT (__stdcall *QueryInterface)    (ILocationEvents2 *self, const IID *iid, void **obj);
        ULONG   (__stdcall *AddRef)            (ILocationEvents2 *self);
        ULONG   (__stdcall *Release)           (ILocationEvents2 *self);
        HRESULT (__stdcall *OnLocationChanged) (ILocationEvents2 *self, const IID *report_type, struct ILocationReport *location_report);
        HRESULT (__stdcall *OnStatusChanged)   (ILocationEvents2 *self, const IID *report_type, LOCATION_REPORT_STATUS new_status);
      } ILocationEvents2Vtbl;

/*
 * Avoid linking with 'locationapi.lib' and define these here.
 */
#undef  DEFINE_GUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
        const GUID DECLSPEC_SELECTANY name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }

DEFINE_GUID (IID_ILatLongReport,  0x7fed806d, 0x0ef8, 0x4f07, 0x80, 0xac, 0x36, 0xa0, 0xbe, 0xae, 0x31, 0x34);
DEFINE_GUID (IID_ILocation,       0xab2ece69, 0x56d9, 0x4f28, 0xb5, 0x25, 0xde, 0x1b, 0x0e, 0xe4, 0x42, 0x37);
DEFINE_GUID (IID_ILocationEvents, 0xcae02bbf, 0x798b, 0x4508, 0xa2, 0x07, 0x35, 0xa7, 0x90, 0x6d, 0xc7, 0x3d);
DEFINE_GUID (CLSID_Location,      0xe5b8e079, 0xee6d, 0x4e33, 0xa4, 0x38, 0xc8, 0x7f, 0x2e, 0x95, 0x92, 0x54);

static pos_t                   g_pos;
static bool                    g_CoInitializeEx_done;
static struct ILocation       *g_location;
static struct ILocationEvents *g_location_events;

static ULONG __stdcall AddRef (ILocationEvents2 *self)
{
  InterlockedIncrement ((volatile long*)&self->ref_count);
  return (self->ref_count);
}

static ULONG __stdcall Release (ILocationEvents2 *self)
{
  InterlockedDecrement ((volatile long*)&self->ref_count);
  if (self->ref_count == 0)
  {
    memset (self, '\0', sizeof(*self)); /* Force a crash if used incorrectly */
    free (self);
    g_location_events = NULL;           /* Since 'g_location_events == self' */
    return (0);
  }
  return (self->ref_count);
}

static HRESULT __stdcall QueryInterface (ILocationEvents2 *self, const IID *iid, void **obj)
{
  if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ILocationEvents))
  {
    *obj = self;
    (*self->lpVtbl->AddRef) (self);
    return (S_OK);
  }
  *obj = NULL;
  return (E_NOINTERFACE);
}

/**
 * This is called when there is a new location report.
 */
static HRESULT __stdcall OnLocationChanged (ILocationEvents2 *self, const IID *report_type, struct ILocationReport *location_report)
{
  ILatLongReport *lat_long_report = NULL;
  HRESULT         hr;

  if (!IsEqualCLSID(report_type, &IID_ILatLongReport))
     return (S_OK);

  hr = (*location_report->lpVtbl->QueryInterface) (location_report, &IID_ILatLongReport, (void**)&lat_long_report);
  TRACE ("LocationEvents::QueryInterface(): hr: %lu\n", hr);

  if (!SUCCEEDED(hr) || !lat_long_report || !lat_long_report->lpVtbl)
     return (S_OK);     /* or signal 'location_poll()' somehow? */

  hr = (*lat_long_report->lpVtbl->GetLatitude) (lat_long_report, &g_pos.lat);
  if (SUCCEEDED(hr))
       TRACE ("Latitude:  %12.8f\n", g_pos.lat);
  else TRACE ("Latitude:  Not available: %s\n", win_strerror(hr));

  hr = (*lat_long_report->lpVtbl->GetLongitude) (lat_long_report, &g_pos.lon);
  if (SUCCEEDED(hr))
       TRACE ("Longitude: %12.8f\n", g_pos.lon);
  else TRACE ("Longitude: Not available: %s\n", win_strerror(hr));

  (void) self;
  return (S_OK);
}

/**
 * This is called when the status of a report type changes.
 */
static HRESULT __stdcall OnStatusChanged (ILocationEvents2 *self, const IID *report_type, LOCATION_REPORT_STATUS new_status)
{
  if (IsEqualCLSID(report_type, &IID_ILatLongReport))
  {
    switch (new_status)
    {
      case REPORT_NOT_SUPPORTED:
           TRACE ("No devices detected\n");
           break;
      case REPORT_ERROR:
           TRACE ("Report error\n");
           break;
      case REPORT_ACCESS_DENIED:
           TRACE ("Access denied\n");
           break;
      case REPORT_INITIALIZING:
           TRACE ("Report is initializing\n");
           break;
      case REPORT_RUNNING:
           TRACE ("Running\n");
           break;
    }
  }
  (void) self;
  return (S_OK);
}

/**
 * Setup everything for an asynchronous lookup.
 */
static bool location_init (void)
{
  HRESULT hr = CoInitializeEx (NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);

  if (!SUCCEEDED(hr))
  {
    TRACE ("CoInitializeEx() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  g_CoInitializeEx_done = true;
  g_location = NULL;

  hr = CoCreateInstance (&CLSID_Location, NULL, CLSCTX_INPROC_SERVER, &IID_ILocation, (void**)&g_location);
  if (!SUCCEEDED(hr) || !g_location)
  {
    TRACE ("CoCreateInstance() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }
  TRACE ("g_location: 0x%p\n", g_location);

  /* Request permissions for this user account to receive location data
   * for the '&IID_ILatLongReport'. Could return an 'Access Denied' or
   * 'Class not registered' etc.
   */
  hr = (*g_location->lpVtbl->RequestPermissions) (g_location,
                                                  NULL,
                                                  (IID*)&IID_ILatLongReport, 1,
                                                  FALSE);

  TRACE ("Location::RequestPermissions() -> hr: %lu\n", hr);
  if (!SUCCEEDED(hr))
  {
    LOG_STDOUT ("RequestPermissions() failed: %s.\n", win_strerror(hr));
    if (hr == REGDB_E_CLASSNOTREG)
       return (false);
    goto no_access;
  }

  /* Tell the Location API that we want to register for report.
   * Can also return an 'Access Denied'.
   */
  hr = (*g_location->lpVtbl->RegisterForReport) (g_location, g_location_events, &IID_ILatLongReport, 0);
  if (!SUCCEEDED(hr))
  {
    LOG_STDOUT ("Location::RegisterForReport() failed; %s.\n", win_strerror(hr));
    goto no_access;
  }

  return (true);

no_access:
  LOG_STDOUT ("Fix your Windows settings to allow applications to access you location.\n"
              "Ref: 'Start | Settings | Privacy | Location' "
              "Or do not use the 'location = yes' config-setting.\n");
  return (false);
}

/**
 * Unregister reports from the Location API
 * and call 'CoUninitialize()'.
 */
void location_exit (void)
{
  if (g_location && g_location->lpVtbl)
  {
    HRESULT hr = (*g_location->lpVtbl->UnregisterForReport) (g_location, &IID_ILatLongReport);

    TRACE ("Location::UnregisterForReport(); hr: %lu\n", hr);
  }

  if (g_CoInitializeEx_done)
     CoUninitialize();
  g_CoInitializeEx_done = false;

  if (g_location_events)
  {
    free (g_location_events);
    g_location_events = NULL;
  }
  g_location = NULL;
}

/**
 * Poll the asynchronous lookup for a good position.
 */
bool location_poll (pos_t *pos)
{
  if (!g_location)
     return (false);  /* polled before or after 'location_exit()'! */

  if (!VALID_POS(g_pos))
  {
    TRACE ("VALID_POS()=0\n");
    return (false);
  }
  pos->lat = g_pos.lat;
  pos->lon = g_pos.lon;
  TRACE ("VALID_POS()=1: Latitude: %.8f, Longitude: %.8f\n", pos->lat, pos->lon);
  return (true);
}

/**
 * Start an asynchronous lookup of the latitude/longitude from 'ILocationReport'.
 */
bool location_get_async (void)
{
  struct ILocationEvents2Vtbl *ev2vtbl;
  struct ILocationEvents2     *ev2 = calloc (sizeof(*ev2) + sizeof(*ev2vtbl), 1);
  bool   rc;

  if (!ev2)
     return (false);

  ev2vtbl = (struct ILocationEvents2Vtbl*) (ev2 + 1);
  ev2vtbl->AddRef            = AddRef;
  ev2vtbl->Release           = Release;
  ev2vtbl->QueryInterface    = QueryInterface;
  ev2vtbl->OnLocationChanged = OnLocationChanged;
  ev2vtbl->OnStatusChanged   = OnStatusChanged;

  g_location_events         = (struct ILocationEvents*) ev2;
  g_location_events->lpVtbl = (struct ILocationEventsVtbl*) ev2vtbl;

  rc = location_init();
  if (!rc)
     location_exit();
  return (rc);
}

